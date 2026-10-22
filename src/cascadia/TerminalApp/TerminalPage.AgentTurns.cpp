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
//
// Measured on 2.1.291 against the e2e mock (tools/e2e-spike): a fork of a running session went busy ~5 s after launch and
// sent its first request ~0.4–1.1 s later; one Esc then left Claude Code's "Continue from where you left off." row
// followed by "[Request interrupted by user]" — the fork waits, and a later uninterrupt of the fork continues it. A fork
// taken while its source ran a TOOL re-runs that tool call (Claude Code's loader drops a call with no result and the turn
// goes on), which is why the watch presses within one 100 ms tick of the heartbeat.

#include "pch.h"
#include "TerminalPage.h"

#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentMaster/ApiErrorRewind.h" // PlanInterruptRewindForSession, RewindInterruptTailForSession, AssessForkContinuation, ProcessExitWait
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
            auto question = _UninterruptQuestionFor(sessionId);
            if (!question)
            {
                return false;
            }
            _AskUninterruptThenRestart(winrt::make_weak(paneContent), std::move(*question));
            return true;
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
            const auto outlook = ::Agentmaster::AssessForkContinuation(sourceId, sourceRunning, forkDir, ::Agentmaster::ResolveSessionEnv(_appSettings, forkDir));
            if (!outlook.ask)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-question] " + ::Agentmaster::ShortId(sourceId) + L" not asked: " + outlook.why + L"\n");
                return std::nullopt;
            }
            ForkTurnQuestion question;
            question.title = !title.empty() ? title : (src ? src->title : std::wstring{});
            question.resendsPrompt = outlook.tail.verdict == ::Agentmaster::ResumeContinuation::ResendsPrompt;
            question.rerunTool = outlook.tail.droppedTool;
            question.sourceRunning = sourceRunning;
            question.tailKind = outlook.tail.tailKind;
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
            dialog.Title(winrt::box_value(L"Fork a session in the middle of a turn?"));
            dialog.Content(winrt::box_value(winrt::hstring{ ::Agentmaster::ForkTurnQuestionBody(question.title, question.resendsPrompt, question.rerunTool, question.sourceRunning) }));
            dialog.PrimaryButtonText(L"Fork interrupted");
            dialog.SecondaryButtonText(L"Fork uninterrupted");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary); // two sessions running one turn side by side is what a stray Enter must not cause
            ::Agentmaster::LogNav(L"fork-question " + ::Agentmaster::ShortId(request.sourceId) + L" (" + request.origin + L"; " + (question.sourceRunning ? std::wstring{ L"running" } : question.tailKind) + (question.rerunTool.empty() ? std::wstring{} : (L"; re-runs " + question.rerunTool)) + L")");
            const auto weak = get_weak();
            const auto result = co_await presenter.ShowDialog(dialog);
            const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
            if (!strong)
            {
                co_return;
            }
            ::Agentmaster::ForkTurnChoice choice = ::Agentmaster::ForkTurnChoice::Interrupted;
            if (result == ContentDialogResult::Secondary)
            {
                choice = ::Agentmaster::ForkTurnChoice::Uninterrupted;
            }
            else if (result != ContentDialogResult::Primary)
            {
                ::Agentmaster::LogNav(L"fork-answer " + ::Agentmaster::ShortId(request.sourceId) + L" -> cancelled (no fork)");
                co_return;
            }
            ::Agentmaster::LogNav(L"fork-answer " + ::Agentmaster::ShortId(request.sourceId) + L" -> " + ::Agentmaster::ForkTurnChoiceName(choice));
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
        // The Sessions page's row menu opens its fork in the BACKGROUND (the list stays open for more): the flag is read by the
        // ONE launch below. Restored to what it was (an outer caller's value), not cleared.
        const bool previousBackground = _openClaudeTabInBackground;
        _openClaudeTabInBackground = request.background;
        auto restore = wil::scope_exit([this, previousBackground]() { _openClaudeTabInBackground = previousBackground; });
        if (request.managed)
        {
            std::wstring forkedId;
            _ForkManagedSessionById(request.sourceId, request.insertPosition, request.model, &forkedId, forkTurn);
            return forkedId;
        }
        return _ForkSessionFromDisk(request.sourceId, request.dir, request.title, request.model, request.insertPosition, forkTurn);
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
