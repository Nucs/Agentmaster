// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TerminalPage, the INTERRUPTED TURNS partial (HOOKS.md "Interrupted turns"; the user's request of
// 2026-10-22). Two questions and the machinery behind their answers:
//
//   * RESTART — "When restarting a session and user has interrupted the session, ask user if he/she would like to
//     uninterrupt (we edit the session transcripts on disk before restart)." A restart of a Claude session whose last
//     turn the user stopped with Esc asks "Continue the interrupted turn?". "Continue the turn" restarts with
//     RestartTurnChoice::Uninterrupt: _RestartManagedSession plans the interrupt rewind again, holds the new connection's
//     Start, and _StartAfterInterruptRewind (below) appends the one rewind line once the old claude has exited, then
//     starts the new one with CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 — so it continues the turn by itself. The question is
//     asked where the user clicked: the tab menu's restart in the hosting window (_AskUninterruptThenRestart), the
//     Manager's restart of a session another window hosts in the Manager's window (_AskUninterruptThenFanOut, which
//     hands the answer over), a Telegram restart in the topic (Messaging.Inbound.cpp).
//   * FORK — "A fork that is in the middle of a run should be prompted if to fork as interrupted or uninterrupted and
//     thus user chooses if agentmaster should lock the tab and press esc once the tab starts running as its supposed to
//     be stopped from the auto resume." Every interactive fork goes through _ForkAskingAboutRunningTurn: when a fork of
//     the source would continue its turn by itself (AssessForkContinuation), it asks "Fork interrupted / Fork
//     uninterrupted". "Fork interrupted" persists the intent on the fork (SessionInfo::stopForkedTurn) and arms the FORK
//     STOP WATCH (below): the fork's tab is locked read-only, and ONE Esc is written into its input the moment its own
//     claude's heartbeat reports the continued turn running; the tab is unlocked once the turn stopped (or nothing ran).
//   * RESTART OF A TURN THAT WOULD GO ON (the user's request of 2026-11-06: "When we fork or restart a session, we have (in
//     one of those) a dialog asking if we want interrupted or uninterrupted — make sure its for restart session asks too").
//     A restart resumes the conversation in a new claude, and Claude Code continues an unfinished turn by itself — a running
//     turn the restart's own shutdown cuts (interruptedByShutdown), a call in flight, a crash's cut. When the user
//     interrupted nothing (the first question's case comes first), the restart asks the fork's question for the restart
//     (_RestartTurnQuestionFor, AssessRestartContinuation): "Restart interrupted" restarts with
//     RestartTurnChoice::StopContinuedTurn — _RestartManagedSession records the stop intent on the session and arms the
//     SAME stop watch on its new connection — and "Restart uninterrupted" with LetTurnContinue (the turn carries on). Asked
//     where the user clicked, like the first question: the tab menu's restart in the hosting window
//     (_AskRestartTurnThenRestart), the Manager's restart of a session another window hosts (_AskRestartTurnThenFanOut), a
//     Telegram restart in the topic (Messaging.Inbound.cpp, _askRestartTurnBeforeRestart).
//
// Measured on 2.1.291 against the e2e mock (tools/e2e-spike): a fork of a running session went busy ~5 s after launch and
// sent its first request ~0.4–1.1 s later; one Esc then left Claude Code's "Continue from where you left off." row
// followed by "[Request interrupted by user]" — the fork waits, and a later uninterrupt of the fork continues it. A fork
// taken while its source ran a TOOL re-runs that tool call (Claude Code's loader drops a call with no result and the turn
// goes on), which is why the watch presses within one 100 ms tick of the heartbeat.

#include "pch.h"
#include "TerminalPage.h"

#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentMaster/ApiErrorRewind.h" // PlanInterruptRewindForSession, RewindInterruptTailForSession, AssessForkContinuation, AssessRestartContinuation, ProcessExitWait
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId, ResolveSessionEnv, ClaudeConversationExists
#include "AgentMaster/Engine.h" // RestartSessionInOtherWindows
#include "AgentMaster/PendingInput.h" // InputBoxState (the fork's input box on screen)
#include "AgentMaster/ProcessInspect.h" // SnapshotProcesses, FindDescendantByImage, ProcessStartUnixMs
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/TranscriptStore.h" // ReadSessionPresenceForPid, PresenceRowMatchesProcess

using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace
{
    // The tick clock the fork stop watch counts in (monotonic; every ForkTurnStopFacts time is one of these).
    int64_t TurnsTickMs() noexcept
    {
        return static_cast<int64_t>(::GetTickCount64());
    }

    // Unix ms (the registry's clock — the fork-carry pump's takeover baseline is one of these).
    int64_t TurnsUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Is a registry state a turn in flight (the fork question's "the source runs right now"; the stop watch's belt)?
    bool IsTurnRunningState(::Agentmaster::SessionState state) noexcept
    {
        return state == ::Agentmaster::SessionState::Running || state == ::Agentmaster::SessionState::NeedsApproval;
    }
}

namespace winrt::TerminalApp::implementation
{
    // ===== the RESTART question =================================================================================

    // See TerminalPage.h. Plans on the session's transcript tail (PlanInterruptRewindForSession: a byte scan for the interrupt
    // row first, so a healthy session costs one small read and no parse).
    std::optional<TerminalPage::UninterruptQuestion> TerminalPage::_UninterruptQuestionFor(const std::wstring& sessionId)
    {
        try
        {
            if (!_sessionRegistry || sessionId.empty())
            {
                return std::nullopt;
            }
            const auto info = _sessionRegistry->GetWithoutQueue(sessionId);
            if (!info || info->kind != ::Agentmaster::AgentKind::Claude)
            {
                return std::nullopt; // a Codex rollout has no interrupt rows to take back
            }
            const auto plan = ::Agentmaster::PlanInterruptRewindForSession(sessionId);
            if (plan.verdict != ::Agentmaster::InterruptRewindVerdict::Rewind)
            {
                // A user interrupt that cannot be taken back usefully (a meta / compact / local-command target, an unknown
                // shape, a torn line): the restart goes as before, and the log says why nothing was offered.
                if (plan.verdict != ::Agentmaster::InterruptRewindVerdict::NotApplicable)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[interrupt-rewind] " + ::Agentmaster::ShortId(sessionId) + L" restart: not offered \x2014 " + plan.reason + L"\n");
                }
                return std::nullopt;
            }
            // Where the restarted claude would read CLAUDE_CODE_RESUME_INTERRUPTED_TURN: its settings files and the environment
            // its launch adds (the cog's env for the dir) — the dialog says what happens when it is not simply on.
            const auto setting = ::Agentmaster::ResolveClaudeAutoContinue(info->workingDir, ::Agentmaster::ResolveSessionEnv(_appSettings, info->workingDir));
            UninterruptQuestion question;
            question.sessionId = sessionId;
            question.title = info->title;
            question.duringToolUse = plan.duringToolUse;
            question.resendsPrompt = plan.targetKind == L"prompt";
            question.promptText = plan.targetText;
            question.autoContinue = ::Agentmaster::ClassifyAutoContinueAtRestart(setting.on, setting.fromSettingsFile);
            question.autoContinueSource = setting.source;
            return question;
        }
        catch (...)
        {
            // Rule #18: the trace is kept; no question means the restart goes as it always did.
            ::Agentmaster::AgentLogCaughtException(L"_UninterruptQuestionFor");
            return std::nullopt;
        }
    }

    // See TerminalPage.h. Only a pane bound to a managed session (by connection identity) is asked about: a shell, an adopted
    // session's shell tab and a dead pane the liveness sweep unlinked restart exactly as before.
    bool TerminalPage::_AskBeforeRestartIfInterrupted(const TerminalApp::TerminalPaneContent& paneContent)
    {
        try
        {
            if (!paneContent)
            {
                return false;
            }
            const auto control = paneContent.GetTermControl();
            if (!control)
            {
                return false;
            }
            const std::wstring sessionId = _ManagedSessionForConnection(control.Connection());
            if (sessionId.empty())
            {
                return false;
            }
            // The first question wins: a tail that ends in the USER's interrupt is a finished turn to Claude Code's resume, so the
            // restarted claude would continue nothing and the third question has nothing to ask there. Only when the user
            // interrupted nothing does the restart ask whether the turn it would continue by itself may run.
            if (auto question = _UninterruptQuestionFor(sessionId))
            {
                _AskUninterruptThenRestart(winrt::make_weak(paneContent), std::move(*question));
                return true;
            }
            if (auto question = _RestartTurnQuestionFor(sessionId))
            {
                _AskRestartTurnThenRestart(winrt::make_weak(paneContent), std::move(*question));
                return true;
            }
            return false;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AskBeforeRestartIfInterrupted");
            return false; // the restart goes on without the question
        }
    }

    // See TerminalPage.h.
    void TerminalPage::_RestartPaneWithTurnChoice(const TerminalApp::TerminalPaneContent& paneContent, ::Agentmaster::RestartTurnChoice turnChoice)
    {
        try
        {
            if (turnChoice == ::Agentmaster::RestartTurnChoice::Ask)
            {
                _restartPaneConnection(paneContent, nullptr); // the tab menu's path, question included
                return;
            }
            // _restartPaneConnection's guard (TerminalPage.cpp): a restored tab never activated has a never-initialized control,
            // and restarting it AVs inside Microsoft.Terminal.Control.dll. Nothing runs there to restart: its connection starts
            // when the tab is first shown.
            if (paneContent)
            {
                if (const auto control{ paneContent.GetTermControl() };
                    control && control.ConnectionState() == ConnectionState::NotConnected)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart] skipped \x2014 connection not started (tab never activated)\n");
                    return;
                }
            }
            if (_RestartManagedSession(paneContent, turnChoice))
            {
                return;
            }
            _restartPaneConnection(paneContent, nullptr); // not a managed session (a shell): the upstream restart, nothing to ask
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_RestartPaneWithTurnChoice");
        }
    }

    // See TerminalPage.h. The default button is "Keep it interrupted": an Enter pressed by habit must never make an agent run
    // a turn the user stopped.
    ContentDialog TerminalPage::_BuildUninterruptDialog(const UninterruptQuestion& question)
    {
        ContentDialog dialog;
        dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
        dialog.Title(winrt::box_value(L"Continue the interrupted turn?"));
        dialog.Content(winrt::box_value(winrt::hstring{ ::Agentmaster::UninterruptQuestionBody(question.title, question.duringToolUse, question.resendsPrompt, question.promptText, question.autoContinue, question.autoContinueSource) }));
        dialog.PrimaryButtonText(L"Continue the turn");
        dialog.SecondaryButtonText(L"Keep it interrupted");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Secondary);
        return dialog;
    }

    // See TerminalPage.h. Terminate-net: a dialog lane — ShowDialog on a tearing-down presenter, the dialog build or the
    // restart throwing would std::terminate the app from a fire_and_forget; contained and logged (Rule #18).
    winrt::fire_and_forget TerminalPage::_AskUninterruptThenRestart(winrt::weak_ref<TerminalApp::TerminalPaneContent> weakContent, UninterruptQuestion question)
    {
        try
        {
            const auto presenter{ _dialogPresenter.get() };
            if (!presenter)
            {
                // No presenter to ask with: restart as this seam always did (the turn stays interrupted).
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[interrupt-rewind] " + ::Agentmaster::ShortId(question.sessionId) + L" restart: no dialog presenter \x2014 restarted with the turn kept interrupted\n");
                if (const auto content = weakContent.get())
                {
                    _RestartPaneWithTurnChoice(content, ::Agentmaster::RestartTurnChoice::KeepInterrupted);
                }
                co_return;
            }
            auto dialog = _BuildUninterruptDialog(question);
            ::Agentmaster::LogNav(L"restart-question " + ::Agentmaster::ShortId(question.sessionId) + L" (the turn the user interrupted" + (question.resendsPrompt ? L", resends the prompt)" : L", continues from a tool result)"));
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
            if (!strong)
            {
                co_return;
            }
            ::Agentmaster::RestartTurnChoice choice = ::Agentmaster::RestartTurnChoice::KeepInterrupted;
            if (result == ContentDialogResult::Primary)
            {
                choice = ::Agentmaster::RestartTurnChoice::Uninterrupt;
            }
            else if (result != ContentDialogResult::Secondary)
            {
                // Cancel / Esc — or another dialog was already up (ShowDialog answers None then): no restart at all.
                ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> cancelled (nothing restarted)");
                co_return;
            }
            const auto content = weakContent.get();
            if (!content)
            {
                ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> " + ::Agentmaster::RestartTurnChoiceName(choice) + L" (the tab closed while the question was up \x2014 nothing restarted)");
                co_return;
            }
            if (choice == ::Agentmaster::RestartTurnChoice::Uninterrupt)
            {
                // The answer was about ONE conversation: if the pane switched to another meanwhile (an in-session /resume),
                // that one was never asked about — restart it as before.
                const auto control = content.GetTermControl();
                const std::wstring now = control ? _ManagedSessionForConnection(control.Connection()) : std::wstring{};
                if (now != question.sessionId)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[interrupt-rewind] " + ::Agentmaster::ShortId(question.sessionId) + L" restart: the pane now hosts " + (now.empty() ? std::wstring{ L"no managed session" } : ::Agentmaster::ShortId(now)) + L" \x2014 restarted without taking anything back\n");
                    choice = ::Agentmaster::RestartTurnChoice::KeepInterrupted;
                }
            }
            ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> " + ::Agentmaster::RestartTurnChoiceName(choice));
            _RestartPaneWithTurnChoice(content, choice);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AskUninterruptThenRestart");
        }
    }

    // See TerminalPage.h. Terminate-net as above.
    winrt::fire_and_forget TerminalPage::_AskUninterruptThenFanOut(UninterruptQuestion question)
    {
        try
        {
            ::Agentmaster::RestartTurnChoice choice = ::Agentmaster::RestartTurnChoice::KeepInterrupted;
            if (const auto presenter{ _dialogPresenter.get() })
            {
                auto dialog = _BuildUninterruptDialog(question);
                ::Agentmaster::LogNav(L"restart-question " + ::Agentmaster::ShortId(question.sessionId) + L" (asked here; another window hosts the tab)");
                const auto weak = get_weak();
                const auto result = co_await presenter.ShowDialog(dialog);
                const auto strong = weak.get();
                if (!strong)
                {
                    co_return;
                }
                if (result == ContentDialogResult::Primary)
                {
                    choice = ::Agentmaster::RestartTurnChoice::Uninterrupt;
                }
                else if (result != ContentDialogResult::Secondary)
                {
                    ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> cancelled (nothing restarted)");
                    co_return;
                }
            }
            ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> " + ::Agentmaster::RestartTurnChoiceName(choice) + L" (handed to the hosting window)");
            // The tab may have moved into THIS window while the question was up: local first, like _RestartClaudeSession.
            if (!_RestartClaudeSessionLocal(question.sessionId, choice))
            {
                ::Agentmaster::RestartSessionInOtherWindows(question.sessionId, _windowId, choice);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AskUninterruptThenFanOut");
        }
    }

    // See TerminalPage.h. The _StartAfterApiErrorRewind recipe with the interrupt rewind: `this` is never touched after the first
    // suspension (the page can close while the old claude exits — the fire_and_forget teardown Gotcha), so everything below
    // works on the parameters, the engine and the dispatcher.
    winrt::fire_and_forget TerminalPage::_StartAfterInterruptRewind(std::wstring sessionId, ConptyConnection newConn, std::shared_ptr<::Agentmaster::ProcessExitWait> holders)
    {
        const auto dispatcher{ Dispatcher() };
        try
        {
            co_await winrt::resume_background();
            try
            {
                // Off the UI thread: an exit can take a few seconds (Windows ends a console client that ignores CTRL_CLOSE after
                // 5 s), and the UI thread must not stall for it.
                const uint64_t waitStarted = ::GetTickCount64();
                ::Agentmaster::InterruptRewindResult result;
                if (!holders || holders->WaitAll(::Agentmaster::kApiErrorRewindExitWaitMs))
                {
                    // Re-planned on the file as it is NOW (the old claude's exit re-stamp included — it names the interrupt row
                    // again, which the rewind then moves past) and re-checked for a live holder before the append.
                    result = ::Agentmaster::RewindInterruptTailForSession(sessionId);
                }
                else
                {
                    std::wstring pids;
                    for (const auto pid : holders->Pids())
                    {
                        pids += (pids.empty() ? L"" : L", ") + std::to_wstring(pid);
                    }
                    result.outcome = ::Agentmaster::ApiErrorRewindOutcome::Skipped;
                    result.detail = L"claude pid " + pids + L" did not exit within " + std::to_wstring(::Agentmaster::kApiErrorRewindExitWaitMs / 1000) + L" s (its exit would undo the line) \x2014 started without taking the interrupt back";
                }
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              ::Agentmaster::DescribeInterruptRewind(result, sessionId, L"restart") + L" (waited " +
                                                  std::to_wstring(::GetTickCount64() - waitStarted) + L" ms for " + std::to_wstring(holders ? holders->Count() : 0) + L" claude process(es))\n");
            }
            catch (...)
            {
                // Contained on the pool thread so the start below still runs: a failed rewind must never leave the restarted tab
                // without its claude.
                ::Agentmaster::AgentLogCaughtException(L"_StartAfterInterruptRewind: wait / append");
            }

            co_await wil::resume_foreground(dispatcher);
            // Start ONLY the connection nothing has touched since the swap (a closed tab, a closed window or a second restart has
            // CLOSED it; a start then would spawn a claude no pane shows).
            if (newConn.State() == ConnectionState::NotConnected)
            {
                newConn.Start();
            }
            else
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[interrupt-rewind] " + ::Agentmaster::ShortId(sessionId) + L" restart: the held connection was closed meanwhile (the tab closed or restarted again) \x2014 not started\n");
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_StartAfterInterruptRewind");
        }
    }

    // ===== the RESTART question for a turn that would go on (the third question) ================================

    // See TerminalPage.h. The lean registry read (no queue) + the rule the Telegram bot shares (AssessRestartContinuation),
    // which reads the resumed transcript's tail and the settings files that may set CLAUDE_CODE_RESUME_INTERRUPTED_TURN.
    std::optional<TerminalPage::RestartTurnQuestion> TerminalPage::_RestartTurnQuestionFor(const std::wstring& sessionId)
    {
        try
        {
            if (!_sessionRegistry || sessionId.empty())
            {
                return std::nullopt;
            }
            const auto info = _sessionRegistry->GetWithoutQueue(sessionId);
            if (!info || info->kind != ::Agentmaster::AgentKind::Claude)
            {
                return std::nullopt; // a Codex restart resumes a rollout: there is no Claude resume to stop
            }
            if (!info->started && !info->external)
            {
                // A parked tab (restored and never shown, or deactivated): the hosting window's restart skips a connection
                // that never started (_restartPaneConnection's guard), so an answer would restart nothing. Its claude starts
                // when the tab is entered — the restore wake's and Reactivate's business, not a restart's.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart-question] " + ::Agentmaster::ShortId(sessionId) + L" not asked: the tab's claude never started (a restart has nothing to restart)\n");
                return std::nullopt;
            }
            // "The turn runs right now": the session's own live state; for a never-messaged fork (no transcript of its own,
            // so the restart RE-FORKS it) also its SOURCE's — the re-fork continues the source's turn. The source is read only
            // in that case, the one in which AssessRestartContinuation reads the source's tail too.
            bool turnRunning = info->live && IsTurnRunningState(info->state);
            if (!turnRunning && !info->forkParentId.empty() && !::Agentmaster::ClaudeConversationExists(sessionId))
            {
                const auto source = _sessionRegistry->GetWithoutQueue(info->forkParentId);
                turnRunning = source && source->live && IsTurnRunningState(source->state);
            }
            // Situation B (a turn that died on an API error) is considered while Agentmaster may take API errors back at all
            // (AppSettings::resumeApiErrorTurns, default on): with it off, no restart takes one back, so there is nothing to offer.
            const auto outlook = ::Agentmaster::AssessRestartContinuation(sessionId, info->forkParentId, turnRunning, info->workingDir, ::Agentmaster::ResolveSessionEnv(_appSettings, info->workingDir), _appSettings.resumeApiErrorTurns);
            if (!outlook.ask)
            {
                // Logged on every restart that does not ask (an explicit, rare action): the trail says why a restart of a busy
                // session went without the question (the switch off, a finished turn, a fresh start).
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart-question] " + ::Agentmaster::ShortId(sessionId) + L" not asked: " + outlook.why + L"\n");
                return std::nullopt;
            }
            RestartTurnQuestion question;
            question.sessionId = sessionId;
            question.title = info->title;
            question.rerunTool = outlook.tail.droppedTool;
            question.turnRunning = outlook.turnRunning;
            question.reforks = outlook.reforks;
            question.tailKind = outlook.tail.tailKind;
            question.apiError = outlook.apiError;
            if (outlook.apiError)
            {
                // B: what the take-back lands on decides what the restarted claude does first (a prompt is sent again; a tool
                // result is carried on from), and the switch decides whether it does so by itself.
                question.resendsPrompt = outlook.apiErrorPlan.targetKind == L"prompt";
                question.errorText = outlook.apiErrorPlan.errorText;
                question.errorKind = outlook.apiErrorPlan.errorKind;
                question.autoContinue = ::Agentmaster::ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile);
                question.autoContinueSource = outlook.autoContinue.source;
            }
            else
            {
                question.resendsPrompt = outlook.tail.verdict == ::Agentmaster::ResumeContinuation::ResendsPrompt;
            }
            return question;
        }
        catch (...)
        {
            // Rule #18: the trace is kept; no question means the restart goes as it always did (the turn carries on).
            ::Agentmaster::AgentLogCaughtException(L"_RestartTurnQuestionFor");
            return std::nullopt;
        }
    }

    // See TerminalPage.h. The default button is "Restart interrupted", as in the fork question and as "Keep it interrupted" in
    // the first one: across all three questions an Enter pressed by habit leaves the agent waiting, never running a turn.
    ContentDialog TerminalPage::_BuildRestartTurnDialog(const RestartTurnQuestion& question)
    {
        ContentDialog dialog;
        dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
        // Situation B quotes the API error and says what the take-back does; the buttons and their meaning for the user stay
        // the same (interrupted = the restarted Claude waits; uninterrupted = it carries on with the turn).
        dialog.Title(winrt::box_value(question.apiError ? L"Restart a session whose turn stopped on an API error?" : L"Restart a session in the middle of a turn?"));
        dialog.Content(winrt::box_value(winrt::hstring{ question.apiError ? ::Agentmaster::ApiErrorTurnQuestionBody(false, question.title, question.errorText, question.resendsPrompt, question.autoContinue, question.autoContinueSource) :
                                                                            ::Agentmaster::RestartTurnQuestionBody(question.title, question.resendsPrompt, question.rerunTool, question.turnRunning) }));
        dialog.PrimaryButtonText(L"Restart interrupted");
        dialog.SecondaryButtonText(L"Restart uninterrupted");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        return dialog;
    }

    // See TerminalPage.h. ShowDialog answers None when another dialog was already up (the presenter shows one at a time), and
    // that must read as "no restart", like Cancel — never as an answer the user did not give.
    std::optional<::Agentmaster::RestartTurnChoice> TerminalPage::_RestartTurnChoiceFromDialog(const ContentDialogResult result) noexcept
    {
        switch (result)
        {
        case ContentDialogResult::Primary:
            return ::Agentmaster::RestartTurnChoice::StopContinuedTurn;
        case ContentDialogResult::Secondary:
            return ::Agentmaster::RestartTurnChoice::LetTurnContinue;
        default:
            return std::nullopt;
        }
    }

    // See TerminalPage.h. Terminate-net: a dialog lane — ShowDialog on a tearing-down presenter, the dialog build or the restart
    // throwing would std::terminate the app from a fire_and_forget; contained and logged (Rule #18).
    winrt::fire_and_forget TerminalPage::_AskRestartTurnThenRestart(winrt::weak_ref<TerminalApp::TerminalPaneContent> weakContent, RestartTurnQuestion question)
    {
        try
        {
            const std::wstring sid8 = ::Agentmaster::ShortId(question.sessionId);
            const auto presenter{ _dialogPresenter.get() };
            if (!presenter)
            {
                // No presenter to ask with: restart as this seam always did — the turn carries on by itself.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart-question] " + sid8 + L" no dialog presenter \x2014 restarted as before (the turn continues by itself)\n");
                if (const auto content = weakContent.get())
                {
                    _RestartPaneWithTurnChoice(content, ::Agentmaster::RestartTurnChoice::KeepInterrupted);
                }
                co_return;
            }
            auto dialog = _BuildRestartTurnDialog(question);
            ::Agentmaster::LogNav(L"restart-question " + sid8 + L" (" + (question.apiError ? (L"an API error: " + (question.errorKind.empty() ? std::wstring{ L"no kind" } : question.errorKind)) : (question.turnRunning ? std::wstring{ L"the turn runs" } : (L"stopped mid-turn: " + question.tailKind))) +
                                  (question.reforks ? std::wstring{ L"; re-forks its source" } : std::wstring{}) + (question.rerunTool.empty() ? std::wstring{} : (L"; re-runs " + question.rerunTool)) + L")");
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
            if (!strong)
            {
                co_return;
            }
            const auto answer = _RestartTurnChoiceFromDialog(result);
            if (!answer)
            {
                // Cancel / Esc — or another dialog was already up: no restart at all.
                ::Agentmaster::LogNav(L"restart-answer " + sid8 + L" -> cancelled (nothing restarted)");
                co_return;
            }
            ::Agentmaster::RestartTurnChoice choice = *answer;
            const auto content = weakContent.get();
            if (!content)
            {
                ::Agentmaster::LogNav(L"restart-answer " + sid8 + L" -> " + ::Agentmaster::RestartTurnChoiceName(choice) + L" (the tab closed while the question was up \x2014 nothing restarted)");
                co_return;
            }
            // The answer was about ONE conversation: a pane that hosts another one by now (an in-session /resume) was never asked
            // about — it restarts as before. Both answers are dropped: StopContinuedTurn would record and watch the WRONG
            // session, and LetTurnContinue would clear a stop intent nobody was asked about.
            const auto control = content.GetTermControl();
            const std::wstring now = control ? _ManagedSessionForConnection(control.Connection()) : std::wstring{};
            if (now != question.sessionId)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart-question] " + sid8 + L" the pane now hosts " + (now.empty() ? std::wstring{ L"no managed session" } : ::Agentmaster::ShortId(now)) + L" \x2014 restarted as before, without the answer\n");
                choice = ::Agentmaster::RestartTurnChoice::KeepInterrupted;
            }
            ::Agentmaster::LogNav(L"restart-answer " + sid8 + L" -> " + ::Agentmaster::RestartTurnChoiceName(choice));
            _RestartPaneWithTurnChoice(content, choice);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AskRestartTurnThenRestart");
        }
    }

    // See TerminalPage.h. Terminate-net as above.
    winrt::fire_and_forget TerminalPage::_AskRestartTurnThenFanOut(RestartTurnQuestion question)
    {
        try
        {
            const std::wstring sid8 = ::Agentmaster::ShortId(question.sessionId);
            ::Agentmaster::RestartTurnChoice choice = ::Agentmaster::RestartTurnChoice::KeepInterrupted;
            if (const auto presenter{ _dialogPresenter.get() })
            {
                auto dialog = _BuildRestartTurnDialog(question);
                ::Agentmaster::LogNav(L"restart-question " + sid8 + L" (asked here; another window hosts the tab; " +
                                      (question.apiError ? (L"an API error: " + (question.errorKind.empty() ? std::wstring{ L"no kind" } : question.errorKind)) : (question.turnRunning ? std::wstring{ L"the turn runs" } : (L"stopped mid-turn: " + question.tailKind))) + L")");
                const auto weak = get_weak();
                const auto result = co_await presenter.ShowDialog(dialog);
                const auto strong = weak.get();
                if (!strong)
                {
                    co_return;
                }
                const auto answer = _RestartTurnChoiceFromDialog(result);
                if (!answer)
                {
                    ::Agentmaster::LogNav(L"restart-answer " + sid8 + L" -> cancelled (nothing restarted)");
                    co_return;
                }
                choice = *answer;
            }
            else
            {
                // No presenter to ask with: the restart goes as it always did (the turn carries on by itself).
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[restart-question] " + sid8 + L" no dialog presenter \x2014 handed over as before (the turn continues by itself)\n");
            }
            ::Agentmaster::LogNav(L"restart-answer " + sid8 + L" -> " + ::Agentmaster::RestartTurnChoiceName(choice) + L" (handed to the hosting window)");
            // The tab may have moved into THIS window while the question was up: local first, like _RestartClaudeSession.
            if (!_RestartClaudeSessionLocal(question.sessionId, choice))
            {
                ::Agentmaster::RestartSessionInOtherWindows(question.sessionId, _windowId, choice);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AskRestartTurnThenFanOut");
        }
    }

    // ===== the FORK question =====================================================================================

    // See TerminalPage.h.
    std::optional<TerminalPage::ForkTurnQuestion> TerminalPage::_ForkTurnQuestionFor(const std::wstring& sourceId, const std::wstring& dir, const std::wstring& title)
    {
        try
        {
            if (sourceId.empty())
            {
                return std::nullopt;
            }
            const auto src = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(sourceId) : std::nullopt;
            if (src && src->kind == ::Agentmaster::AgentKind::Codex)
            {
                return std::nullopt; // `codex fork` has no Claude resume to stop
            }
            const bool sourceRunning = src && src->live && IsTurnRunningState(src->state);
            const std::wstring forkDir = !dir.empty() ? dir : (src ? src->workingDir : std::wstring{});
            // Situation B (the source's turn died on an API error) is considered while Agentmaster may take API errors back
            // (AppSettings::resumeApiErrorTurns, default on).
            const auto outlook = ::Agentmaster::AssessForkContinuation(sourceId, sourceRunning, forkDir, ::Agentmaster::ResolveSessionEnv(_appSettings, forkDir), _appSettings.resumeApiErrorTurns);
            if (!outlook.ask)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-question] " + ::Agentmaster::ShortId(sourceId) + L" not asked: " + outlook.why + L"\n");
                return std::nullopt;
            }
            ForkTurnQuestion question;
            question.title = !title.empty() ? title : (src ? src->title : std::wstring{});
            question.rerunTool = outlook.tail.droppedTool;
            question.sourceRunning = sourceRunning;
            question.tailKind = outlook.tail.tailKind;
            question.apiError = outlook.apiError;
            if (outlook.apiError)
            {
                question.resendsPrompt = outlook.apiErrorPlan.targetKind == L"prompt";
                question.errorText = outlook.apiErrorPlan.errorText;
                question.errorKind = outlook.apiErrorPlan.errorKind;
                question.autoContinue = ::Agentmaster::ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile);
                question.autoContinueSource = outlook.autoContinue.source;
            }
            else
            {
                question.resendsPrompt = outlook.tail.verdict == ::Agentmaster::ResumeContinuation::ResendsPrompt;
            }
            return question;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ForkTurnQuestionFor");
            return std::nullopt; // no question: the fork goes as it always did
        }
    }

    // See TerminalPage.h.
    void TerminalPage::_ForkAskingAboutRunningTurn(ForkLaunchRequest request)
    {
        try
        {
            auto question = _ForkTurnQuestionFor(request.sourceId, request.dir, request.title);
            if (!question)
            {
                _LaunchForkRequest(request, ::Agentmaster::ForkTurnChoice::Uninterrupted);
                return;
            }
            _AskForkTurnThenFork(std::move(request), std::move(*question));
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ForkAskingAboutRunningTurn");
        }
    }

    // See TerminalPage.h. Terminate-net: a dialog lane (Rule #18).
    winrt::fire_and_forget TerminalPage::_AskForkTurnThenFork(ForkLaunchRequest request, ForkTurnQuestion question)
    {
        try
        {
            const auto presenter{ _dialogPresenter.get() };
            if (!presenter)
            {
                // No presenter to ask with: fork as the seams always did.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-question] " + ::Agentmaster::ShortId(request.sourceId) + L" no dialog presenter \x2014 forked uninterrupted\n");
                _LaunchForkRequest(request, ::Agentmaster::ForkTurnChoice::Uninterrupted);
                co_return;
            }
            ContentDialog dialog;
            dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
            // Situation B quotes the source's API error and says what the take-back does; the buttons keep their places and their
            // meaning for the user (interrupted = the fork waits; uninterrupted = it carries on with the turn).
            dialog.Title(winrt::box_value(question.apiError ? L"Fork a session whose turn stopped on an API error?" : L"Fork a session in the middle of a turn?"));
            dialog.Content(winrt::box_value(winrt::hstring{ question.apiError ? ::Agentmaster::ApiErrorTurnQuestionBody(true, question.title, question.errorText, question.resendsPrompt, question.autoContinue, question.autoContinueSource) :
                                                                                ::Agentmaster::ForkTurnQuestionBody(question.title, question.resendsPrompt, question.rerunTool, question.sourceRunning) }));
            dialog.PrimaryButtonText(L"Fork interrupted");
            dialog.SecondaryButtonText(L"Fork uninterrupted");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary); // two sessions running one turn side by side is what a stray Enter must not cause
            ::Agentmaster::LogNav(L"fork-question " + ::Agentmaster::ShortId(request.sourceId) + L" (" + request.origin + L"; " +
                                  (question.apiError ? (L"an API error: " + (question.errorKind.empty() ? std::wstring{ L"no kind" } : question.errorKind)) : (question.sourceRunning ? std::wstring{ L"running" } : question.tailKind)) +
                                  (question.rerunTool.empty() ? std::wstring{} : (L"; re-runs " + question.rerunTool)) + L")");
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
            if (!strong)
            {
                co_return;
            }
            // The answers in situation A: interrupted = the stop watch (Interrupted), uninterrupted = nothing (Uninterrupted). In
            // situation B nothing would run by itself, so interrupted = nothing (Uninterrupted: the fork waits at the error) and
            // uninterrupted = the take-back (ContinueAfterApiError).
            ::Agentmaster::ForkTurnChoice choice = question.apiError ? ::Agentmaster::ForkTurnChoice::Uninterrupted : ::Agentmaster::ForkTurnChoice::Interrupted;
            if (result == ContentDialogResult::Secondary)
            {
                choice = question.apiError ? ::Agentmaster::ForkTurnChoice::ContinueAfterApiError : ::Agentmaster::ForkTurnChoice::Uninterrupted;
            }
            else if (result != ContentDialogResult::Primary)
            {
                ::Agentmaster::LogNav(L"fork-answer " + ::Agentmaster::ShortId(request.sourceId) + L" -> cancelled (no fork)");
                co_return;
            }
            ::Agentmaster::LogNav(L"fork-answer " + ::Agentmaster::ShortId(request.sourceId) + L" -> " +
                                  (question.apiError ? (result == ContentDialogResult::Secondary ? std::wstring{ L"fork uninterrupted (continue-after-api-error)" } : std::wstring{ L"fork interrupted (the fork waits at the API error)" }) :
                                                       std::wstring{ ::Agentmaster::ForkTurnChoiceName(choice) }));
            _LaunchForkRequest(request, choice);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AskForkTurnThenFork");
        }
    }

    // See TerminalPage.h.
    std::wstring TerminalPage::_LaunchForkRequest(const ForkLaunchRequest& request, ::Agentmaster::ForkTurnChoice forkTurn)
    {
        const auto run = [&]() -> std::wstring {
            // The Sessions page's row menu opens its fork in the BACKGROUND (the list stays open for more): the flag is read by
            // the ONE launch below. OR-ed with what it was, not overwritten — a page launch (below) set it already, and a
            // plain `request.background` (false) must not bring the page's new tab to the front. Restored to what it was (an
            // outer caller's value), not cleared.
            const bool previousBackground = _openClaudeTabInBackground;
            _openClaudeTabInBackground = previousBackground || request.background;
            auto restore = wil::scope_exit([this, previousBackground]() { _openClaudeTabInBackground = previousBackground; });
            if (request.managed)
            {
                std::wstring forkedId;
                _ForkManagedSessionById(request.sourceId, request.insertPosition, request.model, &forkedId, forkTurn);
                return forkedId;
            }
            return _ForkSessionFromDisk(request.sourceId, request.dir, request.title, request.model, request.insertPosition, forkTurn);
        };
        // MGR-K1 (2026-10-23): a fork started on the Agent Manager page is a page launch — decided NOW, when the fork runs
        // (after the fork question, if one was asked): with the page still on screen, the fork opens behind it, starts in
        // place and is selected there; with a tab on screen it opens in front, as before.
        if (request.fromManagerPage)
        {
            return _LaunchFromManagerPage(L"fork (" + request.origin + L")", run);
        }
        return run();
    }

    // See TerminalPage.h. Synchronous on purpose, like the restore's take-back (_RewindApiErrorBeforeRestore): the fork's connection
    // is built right after, and its claude reads the source's file when it starts — the line must be there first.
    std::vector<std::pair<std::wstring, std::wstring>> TerminalPage::_TakeBackApiErrorForFork(const std::wstring& sourceId)
    {
        if (sourceId.empty())
        {
            return {}; // the source has no transcript: the fork starts fresh, with no error to take back and no turn to continue
        }
        try
        {
            const auto taken = ::Agentmaster::RewindApiErrorTailForForkSource(sourceId);
            // Every outcome is logged: an Appended line names a live source claude it was written beside; NotNeeded means the
            // error was taken back already (an earlier fork, a restart) or the source went on; Skipped / Failed say why.
            ::Agentmaster::AppendStateLog(L"hooks.log", ::Agentmaster::DescribeApiErrorRewind(taken, sourceId, L"fork") + L"\n");
        }
        catch (...)
        {
            // Rule #18. Only the log line can throw (allocation); the fork goes on either way — without the line it waits at the
            // error, as a fork without the take-back does.
            ::Agentmaster::AgentLogCaughtException(L"_TakeBackApiErrorForFork");
        }
        // The switch rides the fork's launch whatever the outcome: with the line it makes the fork continue by itself; without
        // it Claude Code has nothing to continue, and the switch changes nothing (it acts only at the resume).
        return { { std::wstring{ ::Agentmaster::kClaudeResumeInterruptedTurnEnv }, L"1" } };
    }

    // See TerminalPage.h. The _FinishManagerPageLaunch recipe: LOW priority, after the launch's own queued work (the tab's settle, a
    // page's selection), so starting the claude does not interleave with it.
    void TerminalPage::_StartForkSoonAfterTakeBack(const std::wstring& forkedId)
    {
        if (forkedId.empty())
        {
            return; // no tab opened: nothing to start
        }
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis = get_weak(), forkedId]() {
                const auto self = weakThis.get();
                if (!self)
                {
                    return; // the window closed in between: the fork's tab went with it
                }
                try
                {
                    // False when the fork started meanwhile (a foreground fork's first layout started it) or its tab is gone.
                    const bool started = self->_ActivateDormantSession(forkedId);
                    ::Agentmaster::LogNav(L"fork-start " + ::Agentmaster::ShortId(forkedId) + (started ? std::wstring{ L" started in place (the source's API error was taken back: the turn goes on now)" } :
                                                                                                  std::wstring{ L" already starting (or its tab is gone)" }));
                }
                catch (...)
                {
                    // The tab exists either way and starts when the user enters it; only the early start was lost.
                    ::Agentmaster::AgentLogCaughtException(L"_StartForkSoonAfterTakeBack (tick)");
                }
            });
        }
        catch (...)
        {
            // Rule #18: the dispatcher refused the work item (a window tearing down) — the fork starts when its tab is entered.
            ::Agentmaster::AgentLogCaughtException(L"_StartForkSoonAfterTakeBack");
        }
    }

    // ===== the FORK STOP WATCH ===================================================================================

    // See TerminalPage.h.
    void TerminalPage::_ArmForkTurnStop(const std::wstring& forkId, const std::wstring& seam)
    {
        try
        {
            if (forkId.empty() || _claudeTabs.find(forkId) == _claudeTabs.end())
            {
                // Hosted elsewhere (or nowhere yet): the window that hosts the tab arms its own watch at its next start seam.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(forkId) + L" not armed (" + seam + L"): this window does not host the tab\n");
                return;
            }
            const bool existed = _forkTurnStopWatches.find(forkId) != _forkTurnStopWatches.end();
            auto& watch = _forkTurnStopWatches[forkId]; // an existing watch keeps its state (the tick resets it on a new connection)
            if (watch.armedTickMs == 0)
            {
                watch.armedTickMs = TurnsTickMs();
            }
            watch.seam = seam;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(forkId) + (existed ? L" re-armed (" : L" armed (") + seam + L"): the tab is locked, and ONE Esc goes the moment its claude runs the turn it continues\n");
            _UpdateForkTurnStopTimer();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ArmForkTurnStop");
        }
    }

    // See TerminalPage.h. UI thread.
    void TerminalPage::_UpdateForkTurnStopTimer()
    {
        if (_forkTurnStopWatches.empty())
        {
            if (_forkTurnStopTimer)
            {
                _forkTurnStopTimer.Stop();
            }
            _forkTurnStopTimerIntervalMs = 0;
            return;
        }
        bool anyStarted = false;
        for (const auto& [id, watch] : _forkTurnStopWatches)
        {
            anyStarted = anyStarted || watch.startedAtMs != 0;
        }
        const int64_t wanted = anyStarted ? ::Agentmaster::kForkTurnStopFastTickMs : ::Agentmaster::kForkTurnStopSlowTickMs;
        if (!_forkTurnStopTimer)
        {
            _forkTurnStopTimer = winrt::Windows::UI::Xaml::DispatcherTimer{};
            // Weak page + the sender (never a capture of the timer itself, which would never be freed — the self-capturing
            // handler Gotcha).
            _forkTurnStopTimer.Tick([weak = get_weak()](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable&) {
                if (auto self = weak.get())
                {
                    self->_ForkTurnStopTick();
                }
                else if (const auto timer = sender.try_as<winrt::Windows::UI::Xaml::DispatcherTimer>())
                {
                    timer.Stop(); // the page is gone: stop ticking (UI thread, safe)
                }
            });
        }
        if (wanted != _forkTurnStopTimerIntervalMs)
        {
            _forkTurnStopTimer.Interval(std::chrono::milliseconds{ wanted });
            _forkTurnStopTimerIntervalMs = wanted;
        }
        if (!_forkTurnStopTimer.IsEnabled())
        {
            _forkTurnStopTimer.Start();
        }
    }

    // See TerminalPage.h. Each watch: (1) the tab's control and connection (a new connection resets the per-connection state),
    // (2) the lock, taken as soon as the control exists, (3) once started, the fork's claude.exe under the connection's shell
    // and its own heartbeat, (4) the input box while the heartbeat says idle, (5) DecideForkTurnStop, and the action.
    void TerminalPage::_ForkTurnStopTick()
    {
        try
        {
            const int64_t now = TurnsTickMs();
            std::vector<std::pair<std::wstring, std::pair<::Agentmaster::ForkTurnStopAction, std::wstring>>> ended;
            for (auto& [id, watch] : _forkTurnStopWatches)
            {
                ::Agentmaster::ForkTurnStopFacts facts;
                const auto control = _ControlForSession(id);
                ITerminalConnection connection{ nullptr };
                if (control)
                {
                    connection = control.Connection();
                }
                if (!control || !connection)
                {
                    facts.connectionGone = true;
                }
                else
                {
                    if (watch.connection != connection)
                    {
                        // A new connection (the fork launch, a Restart's swap, a Deactivate's dormant donor): its claude has not
                        // run anything this watch saw. Everything per-connection starts over.
                        watch.connection = connection;
                        watch.startedAtMs = 0;
                        watch.claudePid = 0;
                        watch.claudeStartUnixMs = 0;
                        watch.nextPidLookupMs = 0;
                        watch.idleBoxSinceMs = 0;
                        watch.lastBoxProbeMs = 0;
                        watch.lastBoxUp = false;
                        watch.presses = 0;
                        watch.lastPressMs = 0;
                        watch.lastStatus.clear();
                    }
                    if (watch.lockedControl.get() != control)
                    {
                        // The lock is per CONTROL (a Deactivate replaces the control): take it on the one the tab shows now.
                        // ONLY a read-only we set is ever released, so a user's own read-only toggle survives the watch.
                        watch.lockTaken = false;
                        if (!control.ReadOnly())
                        {
                            control.SetReadOnly(true);
                            watch.lockTaken = true;
                        }
                        watch.lockedControl = winrt::make_weak(control);
                    }
                    const auto state = connection.State();
                    if (state == ConnectionState::Closing || state == ConnectionState::Closed || state == ConnectionState::Failed)
                    {
                        facts.connectionGone = true;
                    }
                    else if (state == ConnectionState::Connected)
                    {
                        facts.started = true;
                    }
                }
                if (facts.started)
                {
                    if (watch.startedAtMs == 0)
                    {
                        watch.startedAtMs = now;
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(id) + L" started (" + watch.seam + L", " + std::to_wstring(now - watch.armedTickMs) + L" ms after the arm) \x2014 watching its heartbeat every " + std::to_wstring(::Agentmaster::kForkTurnStopFastTickMs) + L" ms\n");
                    }
                    facts.startedAtMs = watch.startedAtMs;
                    // The fork's claude: the claude.exe under THIS connection's shell (pwsh hosts it). A Toolhelp snapshot costs
                    // ~10 ms, so it runs at most every kForkTurnStopPidLookupMs until the claude is found (normally once or
                    // twice: the shell starts claude within a second of the connection).
                    if (watch.claudePid == 0 && now >= watch.nextPidLookupMs)
                    {
                        watch.nextPidLookupMs = now + ::Agentmaster::kForkTurnStopPidLookupMs;
                        uint32_t shellPid = 0;
                        if (const auto conpty = connection.try_as<ConptyConnection>())
                        {
                            if (const auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(conpty.RootProcessHandle())))
                            {
                                shellPid = ::GetProcessId(h);
                            }
                        }
                        if (shellPid != 0)
                        {
                            const auto snapshot = ::Agentmaster::SnapshotProcesses();
                            const uint32_t claudePid = ::Agentmaster::FindDescendantByImage(snapshot, shellPid, L"claude.exe");
                            if (claudePid != 0)
                            {
                                watch.claudePid = claudePid;
                                watch.claudeStartUnixMs = ::Agentmaster::ProcessStartUnixMs(claudePid);
                                ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(id) + L" its claude is pid " + std::to_wstring(claudePid) + L"\n");
                            }
                        }
                    }
                    // Its own heartbeat (~/.claude/sessions/<pid>.json): Claude Code rewrites it at every status change, and
                    // "busy" was the first sign of the continued turn in every probe. The PID-reuse guard runs on the process
                    // the pid came from.
                    if (watch.claudePid != 0)
                    {
                        ::Agentmaster::SessionPresenceRow row;
                        if (::Agentmaster::ReadSessionPresenceForPid(watch.claudePid, row) && ::Agentmaster::PresenceRowMatchesProcess(row, watch.claudeStartUnixMs))
                        {
                            facts.heartbeatKnown = true;
                            facts.turnRunning = row.status == L"busy" || row.status == L"waiting";
                            if (row.status != watch.lastStatus)
                            {
                                ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(id) + L" heartbeat: " + (watch.lastStatus.empty() ? std::wstring{ L"(none)" } : watch.lastStatus) + L" -> " + row.status + L" (" + std::to_wstring(now - watch.startedAtMs) + L" ms after start)\n");
                                watch.lastStatus = row.status;
                            }
                        }
                    }
                    if (_sessionRegistry)
                    {
                        if (const auto s = _sessionRegistry->GetWithoutQueue(id))
                        {
                            facts.registryRunning = IsTurnRunningState(s->state);
                        }
                    }
                    // Idle with the input box on screen: the REPL is up and submitted nothing — the fork continues no turn.
                    if (facts.heartbeatKnown && !facts.turnRunning && watch.lastStatus == L"idle")
                    {
                        if (now - watch.lastBoxProbeMs >= ::Agentmaster::kForkTurnStopBoxProbeMs)
                        {
                            watch.lastBoxProbeMs = now;
                            const auto probe = _ReadInputBoxProbeForSession(id, 120);
                            watch.lastBoxUp = probe.first == static_cast<int32_t>(::Agentmaster::InputBoxState::Empty) ||
                                              probe.first == static_cast<int32_t>(::Agentmaster::InputBoxState::Draft);
                        }
                        if (watch.lastBoxUp)
                        {
                            if (watch.idleBoxSinceMs == 0)
                            {
                                watch.idleBoxSinceMs = now;
                            }
                        }
                        else
                        {
                            watch.idleBoxSinceMs = 0;
                        }
                    }
                    else
                    {
                        watch.idleBoxSinceMs = 0;
                    }
                    facts.idleBoxSinceMs = watch.idleBoxSinceMs;
                    facts.presses = watch.presses;
                    facts.lastPressMs = watch.lastPressMs;
                }

                const auto action = ::Agentmaster::DecideForkTurnStop(facts, now);
                switch (action)
                {
                case ::Agentmaster::ForkTurnStopAction::Wait:
                    break;
                case ::Agentmaster::ForkTurnStopAction::PressEsc:
                {
                    // ONE Esc, written into the fork's own connection — the read-only lock blocks the KEYBOARD, not our write.
                    // Claude Code reads a lone ESC as the Escape key (the probes' interrupt, Telegram's /interrupt).
                    static constexpr char16_t kEsc[] = { u'\x1b' };
                    connection.WriteInput(winrt::array_view<const char16_t>{ kEsc, kEsc + 1 });
                    ++watch.presses;
                    watch.lastPressMs = now;
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(id) + L" Esc pressed (" + std::to_wstring(watch.presses) + L"/" + std::to_wstring(::Agentmaster::kForkTurnStopMaxPresses) + L") \x2014 " + (facts.heartbeatKnown ? (L"heartbeat " + watch.lastStatus) : std::wstring{ L"the registry says the turn runs" }) + L", " + std::to_wstring(now - watch.startedAtMs) + L" ms after start\n");
                    break;
                }
                case ::Agentmaster::ForkTurnStopAction::Stopped:
                    ended.push_back({ id, { action, L"the continued turn stopped after " + std::to_wstring(watch.presses) + L" Esc press(es)" } });
                    break;
                case ::Agentmaster::ForkTurnStopAction::NothingToStop:
                    ended.push_back({ id, { action, watch.idleBoxSinceMs != 0 ? std::wstring{ L"the fork came up idle with its input box on screen \x2014 it continued no turn" } : std::wstring{ L"no turn ran within the watch's cap after the start" } } });
                    break;
                case ::Agentmaster::ForkTurnStopAction::GaveUp:
                    ended.push_back({ id, { action, L"the turn still runs after " + std::to_wstring(watch.presses) + L" Esc presses \x2014 stop it by hand (Esc in its tab)" } });
                    break;
                case ::Agentmaster::ForkTurnStopAction::Gone:
                    ended.push_back({ id, { action, L"the tab or its connection went away" } });
                    break;
                }
            }
            for (const auto& [id, outcome] : ended)
            {
                _EndForkTurnStop(id, outcome.first, outcome.second);
            }
            _UpdateForkTurnStopTimer();
        }
        catch (...)
        {
            // Rule #18. The next tick tries again; a watch that keeps throwing still ends at its cap.
            ::Agentmaster::AgentLogCaughtException(L"_ForkTurnStopTick");
        }
    }

    // See TerminalPage.h. UI thread.
    void TerminalPage::_EndForkTurnStop(const std::wstring& forkId, ::Agentmaster::ForkTurnStopAction outcome, const std::wstring& detail)
    {
        const auto it = _forkTurnStopWatches.find(forkId);
        if (it == _forkTurnStopWatches.end())
        {
            return;
        }
        auto watch = std::move(it->second);
        _forkTurnStopWatches.erase(it);
        // 1. The lock — ours only, and released on every outcome (a stuck read-only tab with no explanation is the failure that
        //    must never happen).
        if (watch.lockTaken)
        {
            try
            {
                if (const auto control = watch.lockedControl.get())
                {
                    control.SetReadOnly(false);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_EndForkTurnStop unlock");
            }
        }
        // 2. A fork-carry fill waiting for this box (the source's unsent prompt, PENDING_INPUT.md §11) was held while the watch
        //    owned it: re-anchor its takeover baseline and settle, so the continued turn the watch stopped is not read as
        //    "the user drove the fork" and the copy lands now.
        if (const auto pending = _pendingDraftRestores.find(forkId); pending != _pendingDraftRestores.end())
        {
            pending->second.armedUnixMs = TurnsUnixMs();
            pending->second.startedSeenMs = 0;
        }
        // 3. The persisted intent: kept while the fork's NEXT start would still continue a turn (its own transcript when it has
        //    one; a fork with none re-forks its source), so that start is watched too; cleared otherwise. A Gone watch keeps it
        //    untouched (the tab moved or closed — whoever opens the fork next arms again).
        std::wstring intent = L"kept";
        try
        {
            if (outcome != ::Agentmaster::ForkTurnStopAction::Gone && _sessionRegistry)
            {
                if (const auto s = _sessionRegistry->GetWithoutQueue(forkId); s && s->stopForkedTurn)
                {
                    const std::wstring next = ::Agentmaster::ClaudeConversationExists(forkId) ? forkId : s->forkParentId;
                    const auto continuation = next.empty() ? ::Agentmaster::ResumeContinuationPlan{} : ::Agentmaster::ClassifyResumeContinuationForSession(next);
                    const bool stillContinues = continuation.verdict == ::Agentmaster::ResumeContinuation::ContinuesTurn ||
                                                continuation.verdict == ::Agentmaster::ResumeContinuation::ResendsPrompt;
                    if (stillContinues)
                    {
                        intent = L"kept (its next start would continue a turn again: " + continuation.tailKind + L")";
                    }
                    else
                    {
                        _sessionRegistry->Update(forkId, [](::Agentmaster::SessionInfo& r) { r.stopForkedTurn = false; });
                        intent = L"cleared";
                    }
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_EndForkTurnStop intent");
        }
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-stop] " + ::Agentmaster::ShortId(forkId) + L" " + ::Agentmaster::ForkTurnStopActionName(outcome) + L": " + detail + L"; the tab is unlocked; the stop intent is " + intent + L"\n");
        if (outcome == ::Agentmaster::ForkTurnStopAction::GaveUp)
        {
            ::Agentmaster::LogNav(L"fork-stop " + ::Agentmaster::ShortId(forkId) + L" gave up: " + detail);
        }
    }
}
