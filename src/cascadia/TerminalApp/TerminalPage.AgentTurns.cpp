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
//     turn the user stopped with Esc asks "Restart a session you interrupted?" (2026-11-06: worded like the other questions,
//     "Restart interrupted" the default). "Restart uninterrupted" restarts with
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
//   * THE AUTOMATIC RESTART AFTER A LOST CONNECTION (the user's request of 2026-11-08: "Lets add self healing for error "●
//     API Error: Connection lost mid-response. The response above may be incomplete." by restarting session as uninterrupted
//     automatically. As soon as this error state is detected, we shall restart the tab automatically (without switching to it
//     and without opening a prompt asking for interrupted or uninterrupted restart)"). The window hosting the tab answers the
//     third question itself — "Restart uninterrupted", RestartTurnChoice::LetTurnContinue — the moment its session enters
//     Error on that error: a registry gate (no I/O, every registry change), a transcript gate (one tail read per Error
//     episode, off the UI thread), the restart of the session's OWN pane, then a watch for the turn to run again. Its pure
//     rules are AgentMaster/ConnectionLostRestart.h; HOOKS.md "Connection lost mid-response — the automatic restart". Since the
//     user's report of 2026-11-10 ("● API Error: The response stopped arriving. The response above may be incomplete." — "is
//     now stuck and did not self heal") it covers EVERY response Claude Code cut off (IsCutOffResponseError), not only the
//     lost connection it is named after; since the user's "Search claude code for other variants of errors we should recover
//     from, also consider accepting part of the message and not exact message" (the same day) also every TEMPORARY API
//     failure that outlived Claude Code's own retries (a 5xx, "Overloaded", "Connection error.", "Request timed out", …:
//     ClassifyAutoRestartError, matched by phrases), restarted after a back-off of 1, 5 and 15 minutes.
//
// Measured on 2.1.291 against the e2e mock (tools/e2e-spike): a fork of a running session went busy ~5 s after launch and
// sent its first request ~0.4–1.1 s later; one Esc then left Claude Code's "Continue from where you left off." row
// followed by "[Request interrupted by user]" — the fork waits, and a later uninterrupt of the fork continues it. A fork
// taken while its source ran a TOOL re-runs that tool call (Claude Code's loader drops a call with no result and the turn
// goes on), which is why the watch presses within one 100 ms tick of the heartbeat.

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // GuidToPlainString — the automatic restart finds the session's own pane by its WT_SESSION

#include "AgentAutomation.h" // AgentStateWord — the automatic restart's log words for a session state
#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentMaster/ApiErrorRewind.h" // PlanInterruptRewindForSession, RewindInterruptTailForSession, AssessForkContinuation, AssessRestartContinuation, ProcessExitWait, PlanApiErrorRewindForSession
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId, ResolveSessionEnv, ClaudeConversationExists, ClaudeProjectsDir
#include "AgentMaster/ConnectionLostRestart.h" // the automatic restart after a lost connection: its two gates, its outcome, the runner give-back, the ledger
#include "AgentMaster/SessionScanner.h" // PresenceIsWorking, kScanSubagentFreshMs — the automatic restart's background-work hold
#include "AgentMaster/Engine.h" // RestartSessionInOtherWindows
#include "AgentMaster/PendingInput.h" // InputBoxState (the fork's input box on screen)
#include "AgentMaster/ProcessInspect.h" // SnapshotProcesses, FindDescendantByImage, ProcessStartUnixMs, SubagentActivityUnixMs, EncodeCwdToProjectDir
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

    // HOOKS.md "Connection lost mid-response — the automatic restart": how long the transcript read of the automatic restart
    // may stay out before the liveness sweep settles its track as "not restarted" and shows the Error toast it held. The read
    // itself is one bounded tail read (normally the last 1 MiB; more only while the planner walks a long half-response); this
    // only catches a finish that never reached the UI thread (a dispatcher torn down, an exception logged in the coroutine), so
    // a track can never hold its toast — or block a later restart of the same episode — forever.
    constexpr int64_t kConnectionLostJudgeTimeoutMs = 120 * 1000;

    // A record's text as ONE hooks.log line: line breaks become spaces (a title can carry a lone CR, and hooks.log is one event
    // per line) and the text is cut at `maxChars` with an ellipsis.
    //   text     — the text (a title, an error message).
    //   maxChars — the most characters kept before the ellipsis.
    // Returns the line. Throws std::bad_alloc only (the callers' catch logs it, Rule #18).
    std::wstring OneLogLine(std::wstring_view text, size_t maxChars)
    {
        std::wstring out{ text.substr(0, maxChars) };
        for (auto& c : out)
        {
            if (c == L'\r' || c == L'\n')
            {
                c = L' ';
            }
        }
        if (text.size() > maxChars)
        {
            out += L"\x2026";
        }
        return out;
    }

    // A wait as hooks.log words: "45 s", "1 min 7 s", "15 min" (the automatic restart's back-off line).
    //   ms — the wait in milliseconds (negative reads as 0; rounded UP to a whole second, so a wait never reads shorter than
    //        it is).
    // Returns the words. Throws std::bad_alloc only (the caller's catch logs it, Rule #18).
    std::wstring WaitWords(int64_t ms)
    {
        const int64_t seconds = ms <= 0 ? 0 : (ms + 999) / 1000;
        if (seconds < 60)
        {
            return std::to_wstring(seconds) + L" s";
        }
        std::wstring out = std::to_wstring(seconds / 60) + L" min";
        if (seconds % 60 != 0)
        {
            out += L" " + std::to_wstring(seconds % 60) + L" s";
        }
        return out;
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

    // See TerminalPage.h. The default button is "Restart interrupted": an Enter pressed by habit must never make an agent run
    // a turn the user stopped. The words and the button places are the other two questions' (Primary = interrupted, the
    // default; Secondary = uninterrupted), so every restart and fork reads alike.
    ContentDialog TerminalPage::_BuildUninterruptDialog(const UninterruptQuestion& question)
    {
        ContentDialog dialog;
        dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
        dialog.Title(winrt::box_value(L"Restart a session you interrupted?"));
        dialog.Content(winrt::box_value(winrt::hstring{ ::Agentmaster::UninterruptQuestionBody(question.title, question.duringToolUse, question.resendsPrompt, question.promptText, question.autoContinue, question.autoContinueSource) }));
        dialog.PrimaryButtonText(L"Restart interrupted");
        dialog.SecondaryButtonText(L"Restart uninterrupted");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        return dialog;
    }

    // See TerminalPage.h. ShowDialog answers None when another dialog was already up (the presenter shows one at a time), and
    // that must read as "no restart", like Cancel — never as an answer the user did not give.
    std::optional<::Agentmaster::RestartTurnChoice> TerminalPage::_UninterruptChoiceFromDialog(const ContentDialogResult result) noexcept
    {
        switch (result)
        {
        case ContentDialogResult::Primary:
            return ::Agentmaster::RestartTurnChoice::KeepInterrupted;
        case ContentDialogResult::Secondary:
            return ::Agentmaster::RestartTurnChoice::Uninterrupt;
        default:
            return std::nullopt;
        }
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
            const auto answer = _UninterruptChoiceFromDialog(result);
            if (!answer)
            {
                // Cancel / Esc — or another dialog was already up (ShowDialog answers None then): no restart at all.
                ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> cancelled (nothing restarted)");
                co_return;
            }
            ::Agentmaster::RestartTurnChoice choice = *answer;
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
                const auto answer = _UninterruptChoiceFromDialog(result);
                if (!answer)
                {
                    ::Agentmaster::LogNav(L"restart-answer " + ::Agentmaster::ShortId(question.sessionId) + L" -> cancelled (nothing restarted)");
                    co_return;
                }
                choice = *answer;
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
            // (AppSettings::resumeApiErrorTurns, default on); situation C (the user's own interrupt) always — the restart of such
            // a turn asks the first question with no setting either.
            const auto outlook = ::Agentmaster::AssessForkContinuation(sourceId, sourceRunning, forkDir, ::Agentmaster::ResolveSessionEnv(_appSettings, forkDir), _appSettings.resumeApiErrorTurns, /*considerUserInterrupt*/ true);
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
            question.userInterrupt = outlook.userInterrupt;
            question.autoContinue = ::Agentmaster::ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile);
            question.autoContinueSource = outlook.autoContinue.source;
            if (outlook.apiError)
            {
                question.resendsPrompt = outlook.apiErrorPlan.targetKind == L"prompt";
                question.errorText = outlook.apiErrorPlan.errorText;
                question.errorKind = outlook.apiErrorPlan.errorKind;
            }
            else if (outlook.userInterrupt)
            {
                // C: what the interrupt take-back lands on (a prompt sent again, or a finished tool result carried on from).
                question.resendsPrompt = outlook.interruptPlan.targetKind == L"prompt";
                question.promptText = outlook.interruptPlan.targetText;
                question.duringToolUse = outlook.interruptPlan.duringToolUse;
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
            // Situation B quotes the source's API error, situation C names the user's interrupt; each says what the take-back does.
            // The buttons keep their places and their meaning for the user (interrupted = the fork waits; uninterrupted = it
            // carries on with the turn).
            if (question.apiError)
            {
                dialog.Title(winrt::box_value(L"Fork a session whose turn stopped on an API error?"));
                dialog.Content(winrt::box_value(winrt::hstring{ ::Agentmaster::ApiErrorTurnQuestionBody(true, question.title, question.errorText, question.resendsPrompt, question.autoContinue, question.autoContinueSource) }));
            }
            else if (question.userInterrupt)
            {
                dialog.Title(winrt::box_value(L"Fork a session you interrupted?"));
                dialog.Content(winrt::box_value(winrt::hstring{ ::Agentmaster::ForkInterruptQuestionBody(question.title, question.duringToolUse, question.resendsPrompt, question.promptText, question.autoContinue, question.autoContinueSource) }));
            }
            else
            {
                dialog.Title(winrt::box_value(L"Fork a session in the middle of a turn?"));
                dialog.Content(winrt::box_value(winrt::hstring{ ::Agentmaster::ForkTurnQuestionBody(question.title, question.resendsPrompt, question.rerunTool, question.sourceRunning) }));
            }
            dialog.PrimaryButtonText(L"Fork interrupted");
            dialog.SecondaryButtonText(L"Fork uninterrupted");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary); // two sessions running one turn side by side is what a stray Enter must not cause
            ::Agentmaster::LogNav(L"fork-question " + ::Agentmaster::ShortId(request.sourceId) + L" (" + request.origin + L"; " +
                                  (question.apiError ? (L"an API error: " + (question.errorKind.empty() ? std::wstring{ L"no kind" } : question.errorKind)) :
                                                       (question.userInterrupt ? std::wstring{ L"the user's interrupt" } : (question.sourceRunning ? std::wstring{ L"running" } : question.tailKind))) +
                                  (question.rerunTool.empty() ? std::wstring{} : (L"; re-runs " + question.rerunTool)) + L")");
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
            if (!strong)
            {
                co_return;
            }
            if (result != ContentDialogResult::Primary && result != ContentDialogResult::Secondary)
            {
                ::Agentmaster::LogNav(L"fork-answer " + ::Agentmaster::ShortId(request.sourceId) + L" -> cancelled (no fork)");
                co_return;
            }
            // The answers (Primary = "Fork interrupted", Secondary = "Fork uninterrupted"), per situation:
            //   A (the fork would continue by itself): interrupted = the stop watch (Interrupted); uninterrupted = nothing.
            //   B (an API error): both take the error back in the source's file; interrupted adds the stop watch, so the fork's
            //     branch ends in a regular interrupt instead of the error (TakeBackAndInterrupt); uninterrupted lets it run.
            //   C (the user's interrupt): interrupted = a plain fork (the interrupt is regular already); uninterrupted = the
            //     interrupt taken back (TakeBackAndContinue).
            const bool interrupted = result == ContentDialogResult::Primary;
            ::Agentmaster::ForkTurnChoice choice = ::Agentmaster::ForkTurnChoice::Uninterrupted;
            if (question.apiError)
            {
                choice = interrupted ? ::Agentmaster::ForkTurnChoice::TakeBackAndInterrupt : ::Agentmaster::ForkTurnChoice::TakeBackAndContinue;
            }
            else if (question.userInterrupt)
            {
                choice = interrupted ? ::Agentmaster::ForkTurnChoice::Uninterrupted : ::Agentmaster::ForkTurnChoice::TakeBackAndContinue;
            }
            else
            {
                choice = interrupted ? ::Agentmaster::ForkTurnChoice::Interrupted : ::Agentmaster::ForkTurnChoice::Uninterrupted;
            }
            ::Agentmaster::LogNav(L"fork-answer " + ::Agentmaster::ShortId(request.sourceId) + L" -> " + (interrupted ? L"fork interrupted (" : L"fork uninterrupted (") + ::Agentmaster::ForkTurnChoiceName(choice) + L")");
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
    std::vector<std::pair<std::wstring, std::wstring>> TerminalPage::_TakeBackStopForFork(const std::wstring& sourceId)
    {
        if (sourceId.empty())
        {
            return {}; // the source has no transcript: the fork starts fresh, with no stop to take back and no turn to continue
        }
        try
        {
            // The API error first: its line moves the leaf past the error AND the half-response. Only a source with no API error
            // to take back has its USER interrupt taken back (the two never stop one turn: after an Esc nothing errs).
            const auto taken = ::Agentmaster::RewindApiErrorTailForForkSource(sourceId);
            if (taken.outcome == ::Agentmaster::ApiErrorRewindOutcome::NotNeeded)
            {
                const auto untaken = ::Agentmaster::RewindInterruptTailForForkSource(sourceId);
                // Every outcome is logged except a plain "nothing to take back" for both (an API error taken back already shows in
                // its own line; a source that went on needs no word): Appended names a live source claude, Skipped / Failed why.
                ::Agentmaster::AppendStateLog(L"hooks.log", untaken.outcome == ::Agentmaster::ApiErrorRewindOutcome::NotNeeded ?
                                                                (L"[fork-take-back] " + ::Agentmaster::ShortId(sourceId) + L" fork: nothing to take back (no API error, no user interrupt at the source's end)\n") :
                                                                (::Agentmaster::DescribeInterruptRewind(untaken, sourceId, L"fork") + L"\n"));
            }
            else
            {
                // An Appended line names a live source claude it was written beside; Skipped / Failed say why.
                ::Agentmaster::AppendStateLog(L"hooks.log", ::Agentmaster::DescribeApiErrorRewind(taken, sourceId, L"fork") + L"\n");
            }
        }
        catch (...)
        {
            // Rule #18. Only the log lines can throw (allocation); the fork goes on either way — without the line it waits at the
            // stop, as a fork without the take-back does.
            ::Agentmaster::AgentLogCaughtException(L"_TakeBackStopForFork");
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
                    ::Agentmaster::LogNav(L"fork-start " + ::Agentmaster::ShortId(forkedId) + (started ? std::wstring{ L" started in place (the source's stop was taken back: the turn goes on now)" } :
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

    // ===== the AUTOMATIC RESTART after a lost connection ========================================================
    // HOOKS.md "Connection lost mid-response — the automatic restart"; the pure rules are AgentMaster/ConnectionLostRestart.h.
    // A session whose turn dies on a CUT-OFF RESPONSE — "API Error: Connection lost mid-response. The response above may be
    // incomplete.", "… The response stopped arriving. …", every other "… The response above may be incomplete." and the
    // "… before a response was produced. Try again." family (IsCutOffResponseError) — or on a TEMPORARY API FAILURE that outlived
    // Claude Code's own retries ("API Error: 500 Internal server error. This is a server-side issue, usually temporary …",
    // "Repeated 529 Overloaded errors", "Connection error.", "Request timed out", …: ClassifyAutoRestartError, by phrases and by
    // HTTP status) — is restarted by the window hosting its tab as "Restart uninterrupted" — no question, no tab switch — at once
    // for a cut-off response, after a back-off (1, 5, 15 minutes: TransientRestartBackoffMs) for a temporary failure, in four
    // steps, all on this window's UI thread except the transcript read:
    //   1. the REGISTRY gate on every change of the session (_EvaluateConnectionLostRestart, from the tab-dot drain and the
    //      liveness sweep): is it an error the restart heals, on a tab this window can restart safely, with nothing a restart
    //      would destroy — and has a temporary failure's back-off passed (Wait until it has)?
    //   2. the TRANSCRIPT gate (_JudgeConnectionLostRestart → _FinishConnectionLostJudge): one tail read per Error episode, off
    //      the UI thread — does the take-back continue the turn, is this error row new, is budget left?
    //   3. the RESTART of the session's own pane (_RestartAfterConnectionLost → _RestartManagedSession with LetTurnContinue);
    //   4. the WATCH (_CheckConnectionLostRestartOutcome): the turn runs again → healed (the held Error toast dropped, the
    //      Prompt Queue Runner given back the mode the stop-on-error pause took); 2 minutes without that → not healed (the held
    //      toast shown, the session left to the user).

    // See TerminalPage.h.
    ::Agentmaster::ConnectionLostRestartFacts TerminalPage::_ConnectionLostRestartFactsFor(const ::Agentmaster::SessionInfo& info, bool probeSideFiles)
    {
        ::Agentmaster::ConnectionLostRestartFacts f;
        f.settingOn = _appSettings.autoRestartOnConnectionLost;
        f.takeBackOn = _appSettings.resumeApiErrorTurns;
        f.isClaude = info.kind == ::Agentmaster::AgentKind::Claude;
        f.live = info.live;
        const auto tabIt = _claudeTabs.find(info.id);
        f.hostedHere = tabIt != _claudeTabs.end() && tabIt->second.get() != nullptr;
        f.started = info.started;
        f.external = info.external;
        f.stopIntent = info.stopForkedTurn;
        f.state = info.state;
        f.errorDismissed = info.errorDismissed;
        f.errorMessage = info.errorMessage;
        f.errorStatus = info.errorStatus;
        if (!f.hostedHere || f.state != ::Agentmaster::SessionState::Error || !f.started)
        {
            // The gate answers Ignore before it reads anything below, so the fleet's other sessions (every window's drain sees
            // every session's change) — and dormant ones — cost no classification, no registry lock and no map lookups.
            return f;
        }
        const auto kind = ::Agentmaster::ClassifyAutoRestartError(f.errorMessage, f.errorStatus);
        if (kind == ::Agentmaster::AutoRestartErrorKind::None)
        {
            return f; // an error the restart does not heal: Ignore, likewise before any lookup
        }
        if (kind == ::Agentmaster::AutoRestartErrorKind::Transient)
        {
            // The back-off before a temporary failure's restart: the ledger's count of this session's automatic restarts in the
            // last hour picks the step (1, 5, 15 minutes), the session's own share spreads a fleet hit by one outage. The age is
            // the wall clock since the registry stamped the Error entry (a negative age — a clock that went back — reads as 0,
            // so it only ever waits longer; an unstamped entry, which the registry never leaves, would read as long past).
            const auto look = ::Agentmaster::SharedConnectionLostRestartLedger().Look(info.id, TurnsTickMs());
            f.backoffMs = ::Agentmaster::TransientRestartBackoffMs(look.restartsInTransientWindow, info.id);
            f.errorAgeMs = (std::max)(int64_t{ 0 }, TurnsUnixMs() - info.stateChangedAtUnixMs);
        }
        f.draftInBox = !info.pendingInput.empty();
        // Every owner of the input box a restart would pull the box from under: a delivery or the mail button's clear (the
        // delivery gate), the draft swap, a remembered draft being typed back after a resume (its pump would type into the new
        // claude's box in the middle of the continuation), the fork stop watch (it presses Esc into this very tab).
        f.boxBusy = (_sessionRegistry && _sessionRegistry->DeliveryGateHeld(info.id)) || _draftSwapsInFlight.count(info.id) != 0 ||
                    _pendingDraftRestores.count(info.id) != 0 || _forkTurnStopWatches.count(info.id) != 0;
        // Claude's own heartbeat: "busy" / "shell" == a background shell or agent still works in that process, and a restart
        // ends the process with everything it runs.
        f.backgroundWork = ::Agentmaster::PresenceIsWorking(info.presenceStatus);
        if (probeSideFiles && !f.backgroundWork && !info.workingDir.empty())
        {
            // A background agent writes its own transcript beside the session's (subagents\, tool-results\) while the
            // heartbeat may already read idle — the scanner's recon-subagent and the completion toast's hold read the same
            // signal (AgentExternalWorkSignal, TerminalPage.AgentObserver.cpp). Two directory enumerations: only for a session
            // the gate would restart now.
            const std::wstring transcript = ::Agentmaster::ClaudeProjectsDir() + L"\\" + ::Agentmaster::EncodeCwdToProjectDir(info.workingDir) + L"\\" + info.id + L".jsonl";
            const int64_t sideMs = ::Agentmaster::SubagentActivityUnixMs(transcript);
            f.backgroundWork = sideMs > 0 && TurnsUnixMs() - sideMs <= ::Agentmaster::kScanSubagentFreshMs;
        }
        return f;
    }

    // See TerminalPage.h.
    void TerminalPage::_EvaluateConnectionLostRestart(const ::Agentmaster::SessionInfo& info)
    {
        try
        {
            using ::Agentmaster::ConnectionLostRestartGate;
            using ::Agentmaster::SessionState;
            const std::wstring& id = info.id;
            auto it = _connectionLostRestarts.find(id);
            if (it == _connectionLostRestarts.end() && info.state != SessionState::Error)
            {
                return; // nearly every call of every drain: no track, and the session is not in Error
            }
            if (it != _connectionLostRestarts.end())
            {
                if (it->second.phase == ConnectionLostRestartPhase::Restarted)
                {
                    // After its restart the session passes through Idle (the restart's normalization), maybe Error again (the
                    // error stays the active leaf until the take-back line is written, so the scanner asserts it anew), Waiting
                    // (the line moved the leaf) and Running (the continuation): only the outcome rule reads that, never the gate.
                    _CheckConnectionLostRestartOutcome(id, info);
                    return;
                }
                if (info.state != SessionState::Error || info.stateChangedAtUnixMs != it->second.errorEpisodeMs)
                {
                    // The Error episode this track was about ended: a retry, a rewind, the user's "Move to Idle/Done", a close, or
                    // a NEW Error episode. A read still out then finds no track and does nothing; a held toast is dropped, since
                    // the session moved on without the restart.
                    const auto& old = it->second;
                    if (old.phase == ConnectionLostRestartPhase::Held || old.phase == ConnectionLostRestartPhase::Waiting || old.phase == ConnectionLostRestartPhase::Judging || old.toastOwed)
                    {
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + ::Agentmaster::ShortId(id) + L" let go: the Error episode ended without the automatic restart (now " +
                                                                        std::wstring{ AgentStateWord(info.state) } + L")" + (old.toastOwed ? L"; its held Error toast is dropped" : L"") + L"\n");
                    }
                    _connectionLostRestarts.erase(it);
                    it = _connectionLostRestarts.end();
                    if (info.state != SessionState::Error)
                    {
                        return;
                    }
                }
                else if (it->second.phase == ConnectionLostRestartPhase::Judging || it->second.phase == ConnectionLostRestartPhase::Settled)
                {
                    return; // the read is out (its finish re-runs the gate on a fresh record), or this episode is settled
                }
            }

            // 1. THE REGISTRY GATE. The side-file probe (disk) only for a session the gate would restart now.
            auto facts = _ConnectionLostRestartFactsFor(info, false);
            auto ruling = ::Agentmaster::DecideConnectionLostRestart(facts);
            if (ruling.gate == ConnectionLostRestartGate::Go)
            {
                facts = _ConnectionLostRestartFactsFor(info, true);
                ruling = ::Agentmaster::DecideConnectionLostRestart(facts);
            }
            if (ruling.gate == ConnectionLostRestartGate::Ignore)
            {
                if (it != _connectionLostRestarts.end())
                {
                    _connectionLostRestarts.erase(it); // e.g. the tab left this window or was parked: no longer this window's to restart
                }
                return;
            }
            if (it == _connectionLostRestarts.end())
            {
                ConnectionLostRestartTrack fresh;
                fresh.errorEpisodeMs = info.stateChangedAtUnixMs;
                it = _connectionLostRestarts.emplace(id, std::move(fresh)).first;
            }
            auto& track = it->second;
            if (ruling.gate == ConnectionLostRestartGate::Decline || ruling.gate == ConnectionLostRestartGate::Hold || ruling.gate == ConnectionLostRestartGate::Wait)
            {
                const bool decline = ruling.gate == ConnectionLostRestartGate::Decline;
                const bool wait = ruling.gate == ConnectionLostRestartGate::Wait;
                track.phase = decline ? ConnectionLostRestartPhase::Declined : (wait ? ConnectionLostRestartPhase::Waiting : ConnectionLostRestartPhase::Held);
                if (track.loggedReason != ruling.reason)
                {
                    // Once per reason, not per notify: a held or waiting session is judged again on every registry change and
                    // every tick.
                    track.loggedReason.assign(ruling.reason);
                    std::wstring line = L"[auto-restart] " + ::Agentmaster::ShortId(id);
                    if (wait)
                    {
                        // The back-off's length and the restart it leads to, said once: the sweep re-judges the track every tick
                        // and the next line is "no longer waiting".
                        const auto look = ::Agentmaster::SharedConnectionLostRestartLedger().Look(id, TurnsTickMs());
                        line += L" waiting " + WaitWords(facts.backoffMs - facts.errorAgeMs) + L" before the restart: " + track.loggedReason + L" \x2014 automatic restart " +
                                std::to_wstring(look.restartsInTransientWindow + 1) + L" of at most " + std::to_wstring(::Agentmaster::kTransientRestartBudget) + L" in " +
                                std::to_wstring(::Agentmaster::kTransientRestartWindowMs / 60000) + L" min";
                    }
                    else
                    {
                        line += (decline ? L" not restarted: " : L" waiting before the restart: ") + track.loggedReason;
                    }
                    ::Agentmaster::AppendStateLog(L"hooks.log", line + L" (\"" + OneLogLine(info.title, 80) + L"\" stopped on \"" + OneLogLine(info.errorMessage, 120) + L"\")\n");
                }
                if (!wait && track.toastOwed)
                {
                    // A Waiting track held the Running -> Error toast for a restart that may still heal the session. A decline or
                    // a hold makes the error the user's again until the gate says otherwise: the held toast shows now (a restart
                    // made later, once a hold passes, heals without a second toast) — as the transcript gate's finish does.
                    _ReleaseConnectionLostRestartToast(id, track, info.state, ruling.reason);
                }
                return;
            }

            // 2. GO: the transcript gate decides, on one tail read off the UI thread. Until its finish the track is Judging — which
            // also makes this same pass's Running -> Error toast wait (_HoldToastForConnectionLostRestart: the drain calls this
            // function BEFORE the tab dot and its toast).
            if (!track.loggedReason.empty())
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + ::Agentmaster::ShortId(id) + L" no longer waiting (" + track.loggedReason + L")\n");
                track.loggedReason.clear();
            }
            track.phase = ConnectionLostRestartPhase::Judging;
            track.judgingSinceTickMs = TurnsTickMs();
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + ::Agentmaster::ShortId(id) + L" " +
                                                            ::Agentmaster::AutoRestartErrorKindWords(::Agentmaster::ClassifyAutoRestartError(info.errorMessage, info.errorStatus)) + L": \"" +
                                                            OneLogLine(info.title, 80) + L"\" stopped on \"" + OneLogLine(info.errorMessage, 120) +
                                                            L"\" \x2014 reading its transcript to restart it uninterrupted\n");
            _JudgeConnectionLostRestart(id, track.errorEpisodeMs);
        }
        catch (...)
        {
            // Rule #18. Recovery: the track stays as it was; the next registry change or liveness tick judges the session again.
            ::Agentmaster::AgentLogCaughtException(L"_EvaluateConnectionLostRestart");
        }
    }

    // See TerminalPage.h. Terminate-net: an exception escaping a fire_and_forget is std::terminate (Rule #18).
    winrt::fire_and_forget TerminalPage::_JudgeConnectionLostRestart(std::wstring sessionId, int64_t errorEpisodeMs)
    {
        // Both taken BEFORE the first suspension: after it `this` may be gone (the window closed while the file was read), so
        // the finish reaches the page only through the weak reference.
        const auto weak = get_weak();
        const auto dispatcher{ Dispatcher() };
        try
        {
            co_await winrt::resume_background();
            // A tail read (the last 1 MiB, more only while the planner walks a long half-response). PlanApiErrorRewindForSession
            // is noexcept: a failed read degrades to NotApplicable with its reason, which the finish reports.
            const ::Agentmaster::ApiErrorRewindPlan plan = ::Agentmaster::PlanApiErrorRewindForSession(sessionId);
            co_await wil::resume_foreground(dispatcher);
            if (const auto self = weak.get())
            {
                self->_FinishConnectionLostJudge(sessionId, errorEpisodeMs, plan);
            }
        }
        catch (...)
        {
            // Recovery: the track stays Judging; the liveness sweep settles it after kConnectionLostJudgeTimeoutMs and shows the
            // Error toast it held.
            ::Agentmaster::AgentLogCaughtException(L"_JudgeConnectionLostRestart");
        }
    }

    // See TerminalPage.h.
    void TerminalPage::_FinishConnectionLostJudge(const std::wstring& sessionId, int64_t errorEpisodeMs, const ::Agentmaster::ApiErrorRewindPlan& plan)
    {
        using ::Agentmaster::ConnectionLostRestartGate;
        using ::Agentmaster::SessionState;
        try
        {
            const auto sid8 = ::Agentmaster::ShortId(sessionId);
            const auto it = _connectionLostRestarts.find(sessionId);
            if (it == _connectionLostRestarts.end() || it->second.phase != ConnectionLostRestartPhase::Judging || it->second.errorEpisodeMs != errorEpisodeMs)
            {
                return; // the track moved on while the file was read (the episode ended, the window let it go): nothing to do
            }
            auto& track = it->second;
            const auto info = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(sessionId) : std::nullopt;
            if (!info || info->state != SessionState::Error || info->stateChangedAtUnixMs != errorEpisodeMs)
            {
                // The episode ended while the file was read (the user typed, rewound, closed the tab). The next registry change
                // opens a new track if a new Error comes. A held toast goes with it: the session moved on by itself.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" let go: the Error episode ended while its transcript was read" +
                                                                (track.toastOwed ? L"; its held Error toast is dropped" : L"") + L"\n");
                _connectionLostRestarts.erase(it);
                return;
            }
            // The registry gate AGAIN, on the fresh record: a draft typed, a delivery started, a setting turned off while the file
            // was read all count, and the side files are probed again.
            const auto facts = _ConnectionLostRestartFactsFor(*info, true);
            const auto ruling = ::Agentmaster::DecideConnectionLostRestart(facts);
            if (ruling.gate == ConnectionLostRestartGate::Wait)
            {
                // The back-off is not over after all (the read started once it was, so only a restart this process noted
                // meanwhile — another step of the ladder — can bring this): back to waiting, the held toast kept, the sweep
                // re-judges it every tick.
                track.phase = ConnectionLostRestartPhase::Waiting;
                track.loggedReason.assign(ruling.reason);
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" not restarted yet: " + track.loggedReason + L" (" +
                                                                WaitWords(facts.backoffMs - facts.errorAgeMs) + L" left)\n");
                return;
            }
            if (ruling.gate != ConnectionLostRestartGate::Go)
            {
                const std::wstring reason{ ruling.reason };
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" not restarted now: " + reason + L"\n");
                // The error is the user's again until the gate says otherwise: the held toast shows now (a restart made later, once
                // a hold passes, heals without a second toast).
                _ReleaseConnectionLostRestartToast(sessionId, track, info->state, reason);
                if (ruling.gate == ConnectionLostRestartGate::Ignore)
                {
                    _connectionLostRestarts.erase(it);
                }
                else
                {
                    track.phase = ruling.gate == ConnectionLostRestartGate::Decline ? ConnectionLostRestartPhase::Declined : ConnectionLostRestartPhase::Held;
                    track.loggedReason = reason;
                }
                return;
            }
            // 2. THE TRANSCRIPT GATE: the error must be one the restart heals, the take-back must continue the turn, the error row
            // must be new, its kind's budget must allow it. The registry's HTTP status counts only when the registry's text is the
            // plan's row (a newer error the scanner has not reported yet must not borrow an older row's status).
            const auto look = ::Agentmaster::SharedConnectionLostRestartLedger().Look(sessionId, TurnsTickMs());
            const int status = ::Agentmaster::SameApiErrorText(info->errorMessage, plan.errorText) ? info->errorStatus : 0;
            const auto verdict = ::Agentmaster::DecideConnectionLostRestartFromPlan(plan, status, look);
            if (verdict.verdict != ::Agentmaster::ConnectionLostPlanVerdict::Restart)
            {
                track.phase = ConnectionLostRestartPhase::Settled;
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" not restarted (" + ::Agentmaster::ConnectionLostPlanVerdictName(verdict.verdict) + L"): " + verdict.reason +
                                                                L" \x2014 the session waits at the error for you\n");
                _ReleaseConnectionLostRestartToast(sessionId, track, info->state, L"not restarted: " + verdict.reason);
                return;
            }
            // 3. THE RESTART. The track is marked Restarted FIRST, so a registry change the restart raises — which normalizes the
            // session out of Error — can only ever meet the watch, never the "episode ended" branch.
            track.phase = ConnectionLostRestartPhase::Restarted;
            track.restartedTickMs = TurnsTickMs();
            track.restartedUnixMs = TurnsUnixMs();
            track.errorUuid = plan.errorUuid;
            std::wstring why;
            if (!_RestartAfterConnectionLost(sessionId, *info, plan, why))
            {
                // Re-found: nothing in the refused restart touches the map, but it ran registry code, and a stale reference must
                // never be written through.
                if (const auto again = _connectionLostRestarts.find(sessionId); again != _connectionLostRestarts.end())
                {
                    again->second.phase = ConnectionLostRestartPhase::Settled;
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" not restarted: " + why + L" \x2014 the session waits at the error for you\n");
                    _ReleaseConnectionLostRestartToast(sessionId, again->second, info->state, L"not restarted: " + why);
                }
                return;
            }
            // 4. THE WATCH runs from the drain and the sweep (_CheckConnectionLostRestartOutcome).
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_FinishConnectionLostJudge");
            // Recovery: settle the episode (a failure must not run again on every notify) and give the user the toast it held.
            try
            {
                if (const auto it = _connectionLostRestarts.find(sessionId); it != _connectionLostRestarts.end() && it->second.phase == ConnectionLostRestartPhase::Judging)
                {
                    it->second.phase = ConnectionLostRestartPhase::Settled;
                    _ReleaseConnectionLostRestartToast(sessionId, it->second, SessionState::Error, L"the automatic restart failed (exception logged)");
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_FinishConnectionLostJudge settle");
            }
        }
    }

    // See TerminalPage.h.
    bool TerminalPage::_RestartAfterConnectionLost(const std::wstring& sessionId, const ::Agentmaster::SessionInfo& info, const ::Agentmaster::ApiErrorRewindPlan& plan, std::wstring& why)
    {
        why.clear();
        const auto tabIt = _claudeTabs.find(sessionId);
        const auto tab = tabIt != _claudeTabs.end() ? tabIt->second.get() : nullptr;
        const auto tabImpl = tab ? _GetTabImpl(tab) : nullptr;
        const auto rootPane = tabImpl ? tabImpl->GetRootPane() : nullptr;
        if (!rootPane)
        {
            why = L"its tab is no longer in this window";
            return false;
        }
        // The session's OWN pane: the one whose connection's WT_SESSION is the record's tabToken (the Deactivate and
        // /handover-here discipline). Without a match only a tab with ONE terminal pane is unambiguous — in a split the active
        // or the first pane can be the user's shell, and restarting it would end their shell to resume a conversation there.
        TerminalApp::TerminalPaneContent own{ nullptr };
        TerminalApp::TerminalPaneContent lastTerminal{ nullptr };
        size_t terminals = 0;
        rootPane->WalkTree([&](auto&& pane) {
            if (const auto content = pane->GetContent())
            {
                if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                {
                    ++terminals;
                    lastTerminal = term;
                    if (!info.tabToken.empty())
                    {
                        if (const auto ctrl = term.GetTermControl())
                        {
                            if (const auto conn = ctrl.Connection(); conn && ::Agentmaster::TabTokenEq(info.tabToken, ::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId())))
                            {
                                own = term;
                                return true; // the session's own pane — stop walking
                            }
                        }
                    }
                }
            }
            return false;
        });
        if (!own)
        {
            if (terminals != 1)
            {
                why = terminals == 0 ? std::wstring{ L"its tab holds no terminal pane" } :
                                       L"its tab is split and no pane carries the session's WT_SESSION (" + (info.tabToken.empty() ? std::wstring{ L"none recorded" } : info.tabToken) +
                                           L"), so the restart cannot tell which pane is Claude's";
                return false;
            }
            own = lastTerminal;
        }
        const auto control = own.GetTermControl();
        if (!control || control.ConnectionState() == ConnectionState::NotConnected)
        {
            // _restartPaneConnection's guard: restarting a never-initialized control AVs inside the control DLL.
            why = L"its pane's connection never started (a dormant tab takes the error back at its own start)";
            return false;
        }
        const auto oldConnection = control.Connection();
        // _RestartManagedSession picks the conversation it restarts from the connection: it must be this one.
        if (const auto owner = _ManagedSessionForConnection(oldConnection); owner != sessionId)
        {
            why = L"its pane resolves to " + (owner.empty() ? std::wstring{ L"no managed session" } : ::Agentmaster::ShortId(owner)) + L", not to this session";
            return false;
        }
        auto& ledger = ::Agentmaster::SharedConnectionLostRestartLedger();
        const int64_t now = TurnsTickMs();
        // The kind decides which budget the [nav] line counts against (the transcript gate already checked it): a cut-off
        // response's 3 in 30 minutes, a temporary failure's 3 in 60.
        const int status = ::Agentmaster::SameApiErrorText(info.errorMessage, plan.errorText) ? info.errorStatus : 0;
        const auto kind = ::Agentmaster::ClassifyAutoRestartError(plan.errorText, status);
        const bool transient = kind == ::Agentmaster::AutoRestartErrorKind::Transient;
        const auto look = ledger.Look(sessionId, now);
        const size_t before = transient ? look.restartsInTransientWindow : look.restartsInWindow;
        const size_t budget = transient ? ::Agentmaster::kTransientRestartBudget : ::Agentmaster::kConnectionLostRestartBudget;
        const int64_t windowMs = transient ? ::Agentmaster::kTransientRestartWindowMs : ::Agentmaster::kConnectionLostRestartWindowMs;
        // Noted BEFORE the restart: nothing that runs inside it can start a second one for this error row.
        ledger.Note(sessionId, plan.errorUuid, now);
        ::Agentmaster::LogNav(L"auto-restart-begin " + ::Agentmaster::ShortId(sessionId) + L" (" + ::Agentmaster::AutoRestartErrorKindWords(kind) + L": error row " +
                              ::Agentmaster::ShortId(plan.errorUuid) + L", taken back to the " + plan.targetKind + L" " + ::Agentmaster::ShortId(plan.targetUuid) +
                              L"; automatic restart " + std::to_wstring(before + 1) + L" of at most " + std::to_wstring(budget) + L" in " + std::to_wstring(windowMs / 60000) + L" min)");
        bool handled = false;
        {
            // No dialog on this path (the user's "without opening a prompt"): _PromptPresetLaunchError records a preset refusal
            // instead of showing it while the flag is up. Set around this ONE synchronous call, restored on every way out.
            const bool previousInFlight = _connectionLostRestartInFlight;
            _connectionLostRestartInFlight = true;
            _connectionLostRestartRefusal.clear();
            auto restore = wil::scope_exit([this, previousInFlight] {
                _connectionLostRestartInFlight = previousInFlight;
            });
            // "Restart uninterrupted": the take-back is planned again inside (the file may have moved), its line waits for the old
            // claude's exit (_StartAfterApiErrorRewind), the new claude gets CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1, and the
            // injector is re-pointed. Nothing on this path selects the tab or asks anything.
            handled = _RestartManagedSession(own, ::Agentmaster::RestartTurnChoice::LetTurnContinue);
        }
        // "Handled" also covers a restart the seam REFUSED (no claude.exe, a preset that no longer compiles): those return before
        // the swap, so the pane still holds the old connection. Only a new connection proves the restart ran.
        const bool swapped = handled && own.GetTermControl() && own.GetTermControl().Connection() != oldConnection;
        if (!swapped)
        {
            why = !_connectionLostRestartRefusal.empty() ? L"the restart was refused: " + _connectionLostRestartRefusal :
                                                           (handled ? std::wstring{ L"the restart seam refused it (see its [restart-blocked] line)" } :
                                                                      std::wstring{ L"the restart seam found no managed session on the pane" });
        }
        _connectionLostRestartRefusal.clear(); // a refusal belongs to this restart, never to the next one
        ::Agentmaster::LogNav(L"auto-restart-done " + ::Agentmaster::ShortId(sessionId) +
                              (swapped ? std::wstring{ L" (restarted in place, uninterrupted; the take-back is written once the old claude has exited)" } : L" (NOT restarted: " + why + L")"));
        return swapped;
    }

    // See TerminalPage.h.
    void TerminalPage::_CheckConnectionLostRestartOutcome(const std::wstring& sessionId, const ::Agentmaster::SessionInfo& info)
    {
        try
        {
            const auto it = _connectionLostRestarts.find(sessionId);
            if (it == _connectionLostRestarts.end() || it->second.phase != ConnectionLostRestartPhase::Restarted)
            {
                return;
            }
            auto& track = it->second;
            const auto sid8 = ::Agentmaster::ShortId(sessionId);
            const int64_t since = TurnsTickMs() - track.restartedTickMs;
            // A turn the NEW claude ended (a real Stop, or a terminal reply line in the transcript): the continuation ran,
            // even when no scan caught it running.
            const bool turnEnded = track.restartedUnixMs != 0 && info.turnEndUnixMs > track.restartedUnixMs;
            switch (::Agentmaster::DecideConnectionLostRestartOutcome(info.live, info.state, turnEnded, since))
            {
            case ::Agentmaster::ConnectionLostRestartOutcome::Waiting:
                return;
            case ::Agentmaster::ConnectionLostRestartOutcome::Gone:
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" closed " + std::to_wstring(since / 1000) + L" s after its automatic restart" +
                                                                (track.toastOwed ? L"; its held Error toast is dropped" : L"") + L"\n");
                _connectionLostRestarts.erase(it);
                return;
            case ::Agentmaster::ConnectionLostRestartOutcome::Healed:
            {
                const int64_t episode = track.errorEpisodeMs;
                const bool running = info.state == ::Agentmaster::SessionState::Running || info.state == ::Agentmaster::SessionState::NeedsApproval;
                std::wstring line = L"[auto-restart] " + sid8 + (running ? L" healed: the turn runs again (" : L" healed: the continued turn already ended (") + std::wstring{ AgentStateWord(info.state) } +
                                    L") " + std::to_wstring(since / 1000) + L" s after the automatic restart";
                if (track.toastOwed)
                {
                    line += L"; its held Error toast is dropped (held " + std::to_wstring((TurnsTickMs() - track.toastHeldTickMs) / 1000) + L" s)";
                }
                // Erased BEFORE the give-back: the runner's registry update notifies, and the drain it queues must find no track.
                _connectionLostRestarts.erase(it);
                // The stop-on-error backstop paused this session's runner when the API error (a cut-off response or a temporary
                // failure) put it in Error. The turn runs again, so the queue gets its mode back — decided again inside the
                // registry update, so a mode the user set meanwhile wins (ClearInterruptHold forgets the pause at every such
                // decision).
                ::Agentmaster::AutorunnerMode givenBack = ::Agentmaster::AutorunnerMode::Off;
                if (_sessionRegistry && ::Agentmaster::ShouldGiveRunnerBackAfterRestart(info.autorunner, episode))
                {
                    _sessionRegistry->Update(sessionId, [&givenBack, episode](::Agentmaster::SessionInfo& s) {
                        if (::Agentmaster::ShouldGiveRunnerBackAfterRestart(s.autorunner, episode))
                        {
                            givenBack = s.autorunner.stopOnErrorPausedMode;
                            s.autorunner.mode = givenBack;
                            s.autorunner.stopOnErrorPausedMode = ::Agentmaster::AutorunnerMode::Off;
                            s.autorunner.stopOnErrorPausedEpisodeMs = 0;
                        }
                    });
                }
                if (givenBack != ::Agentmaster::AutorunnerMode::Off)
                {
                    line += std::wstring{ L"; the Prompt Queue Runner is back to " } + ::Agentmaster::AutorunnerModeLabel(givenBack) + L" (stop-on-error had paused it for the API error)";
                    ::Agentmaster::AppendStateLog(L"autorunner.log", L"[auto-restart] " + sessionId + L" runner back to " + ::Agentmaster::AutorunnerModeLabel(givenBack) +
                                                                         L" (the API error's stop-on-error pause is given back: the turn runs again after the automatic restart)\n");
                }
                ::Agentmaster::AppendStateLog(L"hooks.log", line + L"\n");
                return;
            }
            case ::Agentmaster::ConnectionLostRestartOutcome::NotHealed:
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + sid8 + L" NOT healed: " +
                                                                (turnEnded ? L"the continued turn ended in Error again " + std::to_wstring(since / 1000) + L" s after the automatic restart" :
                                                                             L"still " + std::wstring{ AgentStateWord(info.state) } + L" " + std::to_wstring(since / 1000) + L" s after the automatic restart") +
                                                                L" \x2014 the session waits for you\n");
                _ReleaseConnectionLostRestartToast(sessionId, track, info.state, L"the automatic restart did not heal it");
                _connectionLostRestarts.erase(it);
                return;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the track stays; the next change or tick decides again (the confirm window still bounds it).
            ::Agentmaster::AgentLogCaughtException(L"_CheckConnectionLostRestartOutcome");
        }
    }

    // See TerminalPage.h.
    bool TerminalPage::_HoldToastForConnectionLostRestart(const std::wstring& sessionId, int64_t runningSinceMs)
    {
        try
        {
            const auto it = _connectionLostRestarts.find(sessionId);
            // Judging: the transcript read is out. Waiting: a temporary failure's back-off runs, and the restart after it may
            // still heal the session — the toast waits for that outcome too.
            if (it == _connectionLostRestarts.end() || (it->second.phase != ConnectionLostRestartPhase::Judging && it->second.phase != ConnectionLostRestartPhase::Waiting))
            {
                return false;
            }
            it->second.toastOwed = true;
            it->second.toastRunningSinceMs = runningSinceMs;
            it->second.toastHeldTickMs = TurnsTickMs();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_HoldToastForConnectionLostRestart");
            return false; // the toast shows, as it always did
        }
        // Logged once the hold is in place: a failed log line must not turn a held toast into an immediate second one.
        try
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[notify-hold] " + ::Agentmaster::ShortId(sessionId) +
                                                            L" running -> error (an API error that this window restarts by itself: the toast shows only if the restart does not heal it)\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_HoldToastForConnectionLostRestart log");
        }
        return true;
    }

    // See TerminalPage.h.
    void TerminalPage::_ReleaseConnectionLostRestartToast(const std::wstring& sessionId, ConnectionLostRestartTrack& track, ::Agentmaster::SessionState state, std::wstring_view why)
    {
        if (!track.toastOwed)
        {
            return;
        }
        track.toastOwed = false; // cleared FIRST: a throw below must never let a later step show it a second time
        try
        {
            const int64_t heldFor = TurnsTickMs() - track.toastHeldTickMs;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[notify-release] " + ::Agentmaster::ShortId(sessionId) + L" the held running -> error toast is shown as " + std::wstring{ AgentStateWord(state) } +
                                                            L" after " + std::to_wstring(heldFor / 1000) + L" s: " + std::wstring{ why } + L"\n");
            const auto tabIt = _claudeTabs.find(sessionId);
            if (const auto tab = tabIt != _claudeTabs.end() ? tabIt->second.get() : nullptr)
            {
                // The cog's per-state switches, the focused-tab skip and the duplicate guard all apply at this late fire too.
                _FireAgentCompletionToast(sessionId, tab, state, track.toastRunningSinceMs, heldFor);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ReleaseConnectionLostRestartToast");
        }
    }

    // See TerminalPage.h. Terminate-net + the self-marshal of every liveness-tick pass (_PumpDraftRestores, _SweepAgentPendingToasts).
    winrt::fire_and_forget TerminalPage::_SweepConnectionLostRestarts()
    {
        auto strongThis{ get_strong() };
        try
        {
            co_await wil::resume_foreground(Dispatcher());
            if (_connectionLostRestarts.empty() || !_sessionRegistry)
            {
                co_return; // the common case: nothing to watch
            }
            std::vector<std::wstring> ids;
            ids.reserve(_connectionLostRestarts.size());
            for (const auto& entry : _connectionLostRestarts)
            {
                ids.push_back(entry.first); // copied first: _EvaluateConnectionLostRestart below erases tracks
            }
            const int64_t now = TurnsTickMs();
            for (const auto& id : ids)
            {
                const auto it = _connectionLostRestarts.find(id);
                if (it == _connectionLostRestarts.end())
                {
                    continue; // an earlier step of this pass let it go
                }
                const auto info = _sessionRegistry->GetWithoutQueue(id);
                if (it->second.phase == ConnectionLostRestartPhase::Judging)
                {
                    if (now - it->second.judgingSinceTickMs >= kConnectionLostJudgeTimeoutMs)
                    {
                        it->second.phase = ConnectionLostRestartPhase::Settled;
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-restart] " + ::Agentmaster::ShortId(id) + L" not restarted: its transcript read did not come back within " +
                                                                        std::to_wstring(kConnectionLostJudgeTimeoutMs / 1000) + L" s \x2014 the session waits at the error for you\n");
                        _ReleaseConnectionLostRestartToast(id, it->second, info ? info->state : ::Agentmaster::SessionState::Error, L"its transcript read did not come back");
                    }
                    continue; // otherwise its finish judges on a fresh record
                }
                if (!info)
                {
                    // Gone from the registry (a resume-fresh drop, a delete): nothing left to restart or to toast about.
                    _connectionLostRestarts.erase(it);
                    continue;
                }
                // Every other phase is the registry change's own path on a fresh read: a hold re-judged (fresh side files age out
                // without a registry change), a temporary failure's back-off timed (Waiting: nothing changes in the registry while
                // it runs, so this tick is what restarts it), a restarted session's confirm window timed, an ended episode let go.
                _EvaluateConnectionLostRestart(*info);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SweepConnectionLostRestarts");
        }
    }
}
