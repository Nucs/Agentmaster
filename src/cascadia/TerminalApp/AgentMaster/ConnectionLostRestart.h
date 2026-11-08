// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the AUTOMATIC RESTART after a lost connection (HOOKS.md "Connection lost mid-response — the automatic
// restart"). The user's request of 2026-11-08: "Lets add self healing for error "● API Error: Connection lost
// mid-response. The response above may be incomplete." by restarting session as uninterrupted automatically. As soon as
// this error state is detected, we shall restart the tab automatically (without switching to it and without opening a
// prompt asking for interrupted or uninterrupted restart)".
//
// WHAT CLAUDE CODE WRITES (2.1.310, read from its bundle and from real transcripts). When the response stream dies after
// output began, Claude Code finalizes the half-streamed reply and appends the synthetic row
//     {"type":"assistant", …"content":[{"type":"text","text":"API Error: Connection lost mid-response. The response above may
//      be incomplete."}]…, "error":"server_error", "truncatedAfterOutput":true, "isApiErrorMessage":true,
//      "streamCutCause":"stale_connection"}
// (the same text for the cut causes "stale_connection", "network_down", "stream_truncated" and a "stream_suspended" that
// was not a sleep; 2.1.282 / 2.1.291 wrote it without "streamCutCause"). The scanner turns it into Error (recon-error),
// and the claude then waits at the error until someone types — a restart alone does not help either: the half-reply
// above the error makes Claude Code's resume call the turn complete. Only "Restart uninterrupted"
// (RestartTurnChoice::LetTurnContinue — the API-error take-back line plus CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1,
// ApiErrorRewind.h / InterruptedTurns.h) continues the turn.
//
// WHAT THIS ADDS. The window hosting such a session's tab does that restart BY ITSELF — no question, no tab switch
// (TerminalPage.AgentTurns.cpp, "the AUTOMATIC RESTART after a lost connection"). The rules are pure and live here so
// the m5 harness pins them (TestConnectionLostRestart, tests_rewind.cpp):
//   * DecideConnectionLostRestart — the REGISTRY gate, run on every change of a session the window hosts (no I/O): is it
//     this error, on a tab Agentmaster can safely restart, with nothing a restart would destroy (an unsent draft, a
//     delivery in flight, background work)?
//   * DecideConnectionLostRestartFromPlan — the TRANSCRIPT gate, one tail read per Error episode (off the UI thread):
//     restart only when the take-back really continues the turn, at most once per error row, and at most
//     kConnectionLostRestartBudget times per session within kConnectionLostRestartWindowMs (a network that keeps dropping
//     must not turn into a restart loop);
//   * DecideConnectionLostRestartOutcome — after the restart: healed (the turn runs again), not healed, or still waiting;
//   * ShouldGiveRunnerBackAfterRestart — the Prompt Queue Runner the stop-on-error backstop paused gets its mode back once
//     the turn runs again (a healed turn must not leave its queue paused);
//   * ConnectionLostRestartLedger — the process-wide memory of the restarts made (one engine per process, Rule #10).
//
// Header-only. Plain C++ (no Windows, no WinRT): the app and the m5 harness include it.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ApiErrorRewind.h" // ApiErrorRewindPlan, ApiErrorTakeBackContinues — what the transcript gate reads
#include "SessionModels.h" // SessionState, AutorunnerState — what the registry gate and the runner give-back read

namespace Agentmaster
{
    // The words that identify the error in its row's text ("API Error: Connection lost mid-response. The response above
    // may be incomplete."). Claude Code prints them for every mid-response cut of the CONNECTION; its other mid-response
    // cuts ("Server error mid-response", "The response stopped arriving", "Part of the response never arrived", "Your
    // computer went to sleep mid-response") and the before-any-output variant ("Connection lost before a response was
    // produced") are deliberately NOT matched: the user asked for this one, and the before-output variant already
    // continues by itself on any resume.
    inline constexpr std::wstring_view kConnectionLostMidResponseText = L"Connection lost mid-response";

    // At most this many automatic restarts of ONE session inside kConnectionLostRestartWindowMs. A connection that keeps
    // dropping mid-response would otherwise restart the session forever; past the budget the session stays in Error for
    // the user, exactly as without this feature. Three covers a flaky stretch without hiding a dead network for long.
    inline constexpr size_t kConnectionLostRestartBudget = 3;

    // The rolling window of kConnectionLostRestartBudget (30 minutes). Long enough that a burst of drops is capped,
    // short enough that a session hit again hours later heals again.
    inline constexpr int64_t kConnectionLostRestartWindowMs = 30LL * 60 * 1000;

    // How long after an automatic restart the window waits to see the turn run again before it calls the restart a
    // failure (and shows the Error toast it held). It covers the restart's held start (up to kApiErrorRewindExitWaitMs,
    // 15 s, for the old claude to exit before the take-back line is written), a slow claude.exe start on a loaded PC
    // (MCP servers included) and the first request of the continuation. Too short would toast for a restart that is
    // still healing; too long only delays the toast of a restart that failed.
    inline constexpr int64_t kConnectionLostRestartConfirmMs = 120LL * 1000;

    // Is this error text Claude Code's "Connection lost mid-response" (kConnectionLostMidResponseText)? An ordinal,
    // case-insensitive search for the ASCII words anywhere in the text, so a different prefix ("API Error: " today) or a
    // changed tail still matches. Pure; allocates nothing.
    //   errorText — the error row's text as the scanner captured it (SessionInfo::errorMessage) or as the take-back
    //               planner read it (ApiErrorRewindPlan::errorText). Empty never matches.
    // Returns true when the words are in the text. Never throws.
    inline bool IsConnectionLostMidResponseError(std::wstring_view errorText) noexcept
    {
        const std::wstring_view needle = kConnectionLostMidResponseText;
        if (errorText.size() < needle.size())
        {
            return false;
        }
        // ASCII-only case fold: the needle is ASCII, so a non-ASCII character of the text can never be part of a match.
        const auto fold = [](wchar_t c) noexcept { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; };
        for (size_t start = 0; start + needle.size() <= errorText.size(); ++start)
        {
            size_t i = 0;
            while (i < needle.size() && fold(errorText[start + i]) == fold(needle[i]))
            {
                ++i;
            }
            if (i == needle.size())
            {
                return true;
            }
        }
        return false;
    }

    // What the registry gate decided for one session at one moment. The order is the order of the checks: a session that
    // is not this feature's business is Ignore (nothing logged); a lost connection Agentmaster will not restart is Decline
    // (logged once per Error episode); one it will restart once something ends is Hold; Go starts the transcript gate.
    enum class ConnectionLostRestartGate
    {
        // Not this feature's business: not a live Claude session hosted in this window, not in an unacknowledged Error,
        // another error than the lost connection, or a dormant tab (its own start takes the error back — the restored
        // resume and Deactivate's wake do — and the shipped CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 continues it).
        Ignore,
        // A lost connection this window will not restart: the switch is off, an adopted tab, the take-back switch is off, a
        // stop intent the user recorded. The session stays in Error for the user, as without the feature.
        Decline,
        // A lost connection this window WILL restart once the reason passes (an unsent draft, a delivery or a draft fill
        // owning the input box, background work a restart would end). Re-evaluated on every registry change of the session
        // and on every liveness tick.
        Hold,
        // Restart it — after the transcript gate (DecideConnectionLostRestartFromPlan) agrees.
        Go,
    };

    // The registry facts the gate reads, gathered by the window from the session's record and its own state. Plain
    // values, so a test can state every case.
    struct ConnectionLostRestartFacts
    {
        bool settingOn{ false }; // AppSettings::autoRestartOnConnectionLost — the feature's switch
        bool takeBackOn{ false }; // AppSettings::resumeApiErrorTurns — without it a restart never takes the error back, so it would heal nothing
        bool isClaude{ false }; // SessionInfo::kind == AgentKind::Claude (a Codex rollout has no such rows)
        bool live{ false }; // SessionInfo::live (an archived record has no tab to restart)
        bool hostedHere{ false }; // this window's _claudeTabs holds the session's tab (only the hosting window may restart its ConPTY)
        bool started{ false }; // SessionInfo::started — its claude ran in this tab (false == a dormant tab)
        bool external{ false }; // SessionInfo::external — an ADOPTED session: its ConPTY is the user's own shell
        bool stopIntent{ false }; // SessionInfo::stopForkedTurn — the user chose that this session's continued turn be STOPPED ("Fork interrupted" / "Restart interrupted"); a LetTurnContinue restart would clear that choice
        SessionState state{ SessionState::Idle }; // SessionInfo::state
        bool errorDismissed{ false }; // SessionInfo::errorDismissed — the user acknowledged this error ("Move to Idle/Done")
        std::wstring_view errorMessage; // SessionInfo::errorMessage — the error row's text the scanner captured
        bool draftInBox{ false }; // SessionInfo::pendingInput is not empty — an unsent draft a restart would wipe
        bool boxBusy{ false }; // a delivery, a draft swap, a draft type-back or the fork stop watch owns the input box
        bool backgroundWork{ false }; // claude's heartbeat says busy / shell, or its side files are fresh (agents, shells)
    };

    // The registry gate's answer: the verdict and, for every verdict but Go, why — a static string for the log.
    struct ConnectionLostRestartRuling
    {
        ConnectionLostRestartGate gate{ ConnectionLostRestartGate::Ignore };
        std::wstring_view reason; // a string literal (static storage), never a view into the facts
    };

    // THE REGISTRY GATE: should the window restart this session now, later, or never for this Error episode? Pure and
    // cheap (no I/O), so the window runs it on every registry change of every session it hosts.
    //   f — the facts (ConnectionLostRestartFacts).
    // Returns the ruling; its reason is a string literal. Never throws.
    // ⚠ The checks are ordered: Ignore before Decline before Hold before Go. A Decline must stay a Decline whatever holds
    //   (a draft must not turn "the switch is off" into "waiting for the draft"), and a session that is not in this error
    //   must read Ignore whatever else is true, or the window would log a line for every session it hosts.
    inline ConnectionLostRestartRuling DecideConnectionLostRestart(const ConnectionLostRestartFacts& f) noexcept
    {
        if (!f.isClaude || !f.live || !f.hostedHere)
        {
            return { ConnectionLostRestartGate::Ignore, L"not a live Claude session hosted in this window" };
        }
        if (f.state != SessionState::Error || f.errorDismissed)
        {
            return { ConnectionLostRestartGate::Ignore, L"not in an unacknowledged Error" };
        }
        if (!IsConnectionLostMidResponseError(f.errorMessage))
        {
            return { ConnectionLostRestartGate::Ignore, L"another error than a lost connection" };
        }
        if (!f.started)
        {
            // A dormant tab heals at its own start: the restore and the Deactivate seams take the error back before the
            // resumed claude reads the file, and the shipped CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 continues it. Restarting
            // it would only start a claude nobody looks at (and _RestartPaneWithTurnChoice refuses a never-started control).
            return { ConnectionLostRestartGate::Ignore, L"a dormant tab (its own start takes the error back)" };
        }
        if (!f.settingOn)
        {
            return { ConnectionLostRestartGate::Decline, L"the automatic restart is off (Settings > Sessions)" };
        }
        if (f.external)
        {
            // An adopted session's ConPTY is the shell the user typed `claude` into: a restart replays that shell's
            // profile, it does not resume the conversation (the Deactivate seam refuses these tabs for the same reason).
            return { ConnectionLostRestartGate::Decline, L"an adopted session (its tab hosts the user's own shell, which a restart would replace)" };
        }
        if (!f.takeBackOn)
        {
            return { ConnectionLostRestartGate::Decline, L"\"Continue a turn that ended in an API error\" is off (a restart would wait at the error)" };
        }
        if (f.stopIntent)
        {
            // "Fork interrupted" / "Restart interrupted" recorded that the next continuation of this session must be stopped:
            // "Restart uninterrupted" is exactly the answer the user did not give (it clears that intent and lets the turn run).
            return { ConnectionLostRestartGate::Decline, L"the user chose to keep this session's turn stopped (\"Fork interrupted\" / \"Restart interrupted\")" };
        }
        if (f.draftInBox)
        {
            return { ConnectionLostRestartGate::Hold, L"an unsent draft is in the input box (a restart would wipe it)" };
        }
        if (f.boxBusy)
        {
            return { ConnectionLostRestartGate::Hold, L"a delivery, a draft fill or the fork stop watch owns the input box" };
        }
        if (f.backgroundWork)
        {
            return { ConnectionLostRestartGate::Hold, L"claude still runs background work (a shell or an agent) that a restart would end" };
        }
        return { ConnectionLostRestartGate::Go, L"" };
    }

    // What the transcript gate decided once the take-back planner read the session's tail.
    enum class ConnectionLostPlanVerdict
    {
        // Restart it (RestartTurnChoice::LetTurnContinue).
        Restart,
        // The active branch no longer ends in an API error (the planner's NotApplicable): the user rewound or retried
        // meanwhile, or the error was taken back already. Nothing to heal.
        NothingToTakeBack,
        // The planner left the error alone (a refusal, an unknown shape, a torn last line, a file larger than it reads):
        // a restart would take nothing back and the new claude would wait at the error.
        NotTakenBack,
        // The transcript's active error is another error than the lost connection (the registry's message was stale).
        NotThisError,
        // The take-back lands on a row Claude Code does not continue from by itself (a meta row, a compaction summary).
        WouldNotContinue,
        // This very error row was restarted for already: that restart did not take it back (another tab's claude runs the
        // conversation, the old claude outlived the 15 s wait), so a second restart would fail the same way.
        AlreadyRestarted,
        // kConnectionLostRestartBudget automatic restarts of this session within kConnectionLostRestartWindowMs.
        OverBudget,
    };

    // The log word of a transcript-gate verdict ("restart", "nothing-to-take-back", …). Pure; never throws.
    //   v — the verdict.
    // Returns a static string ("unknown" for a value outside the enum — a verdict cast from a stray integer).
    inline const wchar_t* ConnectionLostPlanVerdictName(ConnectionLostPlanVerdict v) noexcept
    {
        switch (v)
        {
        case ConnectionLostPlanVerdict::Restart:
            return L"restart";
        case ConnectionLostPlanVerdict::NothingToTakeBack:
            return L"nothing-to-take-back";
        case ConnectionLostPlanVerdict::NotTakenBack:
            return L"not-taken-back";
        case ConnectionLostPlanVerdict::NotThisError:
            return L"not-this-error";
        case ConnectionLostPlanVerdict::WouldNotContinue:
            return L"would-not-continue";
        case ConnectionLostPlanVerdict::AlreadyRestarted:
            return L"already-restarted";
        case ConnectionLostPlanVerdict::OverBudget:
            return L"over-budget";
        }
        return L"unknown";
    }

    // The transcript gate's answer: the verdict and a sentence for the log (empty for Restart).
    struct ConnectionLostPlanRuling
    {
        ConnectionLostPlanVerdict verdict{ ConnectionLostPlanVerdict::NothingToTakeBack };
        std::wstring reason;
    };

    // THE TRANSCRIPT GATE: once PlanApiErrorRewindForSession has read the session's tail, may the window restart it? The
    // restart must HEAL — the planner must take this error back onto a row Claude Code continues from — and it must not
    // loop: never twice for one error row, never more than kConnectionLostRestartBudget times in the window. Pure.
    //   plan                    — the take-back plan of the session's transcript (ApiErrorRewind.h).
    //   lastRestartedErrorUuid  — the error row the previous automatic restart of this session was made for ("" when
    //                             none: ConnectionLostRestartLedger::Look).
    //   restartsInWindow        — the automatic restarts of this session within kConnectionLostRestartWindowMs.
    // Returns the ruling.
    // Throws std::bad_alloc only, when the reason sentence cannot be allocated (the caller's catch logs it, Rule #18).
    inline ConnectionLostPlanRuling DecideConnectionLostRestartFromPlan(const ApiErrorRewindPlan& plan, std::wstring_view lastRestartedErrorUuid, size_t restartsInWindow)
    {
        if (plan.verdict == ApiErrorRewindVerdict::NotApplicable)
        {
            return { ConnectionLostPlanVerdict::NothingToTakeBack, L"the transcript's active branch no longer ends in an API error" };
        }
        if (plan.verdict != ApiErrorRewindVerdict::Rewind)
        {
            return { ConnectionLostPlanVerdict::NotTakenBack, L"the take-back planner leaves this error alone: " + (plan.reason.empty() ? std::wstring{ L"(no reason given)" } : plan.reason) };
        }
        if (!IsConnectionLostMidResponseError(plan.errorText))
        {
            return { ConnectionLostPlanVerdict::NotThisError, L"the transcript's error is \"" + plan.errorText + L"\", not a lost connection" };
        }
        if (!ApiErrorTakeBackContinues(plan))
        {
            return { ConnectionLostPlanVerdict::WouldNotContinue, L"the take-back lands on a " + plan.targetKind + L" row, which Claude Code does not continue by itself" };
        }
        if (!plan.errorUuid.empty() && plan.errorUuid == lastRestartedErrorUuid)
        {
            return { ConnectionLostPlanVerdict::AlreadyRestarted, L"this error row was restarted for already, and that restart did not take it back" };
        }
        if (restartsInWindow >= kConnectionLostRestartBudget)
        {
            return { ConnectionLostPlanVerdict::OverBudget,
                     std::to_wstring(restartsInWindow) + L" automatic restarts of this session in the last " + std::to_wstring(kConnectionLostRestartWindowMs / 60000) + L" minutes" };
        }
        return { ConnectionLostPlanVerdict::Restart, std::wstring{} };
    }

    // What became of a session the window restarted automatically.
    enum class ConnectionLostRestartOutcome
    {
        // Not decided yet: the restart's held start, the take-back line, the claude's start and its first request take a
        // few seconds, and an Error the scanner asserts again meanwhile (the error stays the active leaf until the line is
        // written) is part of that, not a failure.
        Waiting,
        // The turn went on: it runs again (Running, or NeedsApproval — the continuation asked for a permission), or a turn
        // ENDED after the restart with the session out of Error (a continuation answered so fast that no scan saw it run —
        // measured with the e2e mock, flows/06 INT-01).
        Healed,
        // The restart did not heal it: kConnectionLostRestartConfirmMs passed with no turn going on, or a turn ended after
        // the restart with the session in Error again (the continuation itself failed).
        NotHealed,
        // The session left the fleet (its tab closed): nothing to watch.
        Gone,
    };

    // AFTER THE RESTART: did it heal the session? Pure.
    //   live                  — SessionInfo::live now.
    //   state                 — SessionInfo::state now.
    //   turnEndedSinceRestart — SessionInfo::turnEndUnixMs is newer than the restart (wall clock). Only a REAL Stop hook or a
    //                           transcript line with a terminal stop_reason stamps it — never a synthesized Stop — and the
    //                           old claude's hooks are dropped once the restart retired its host token, so a newer stamp is
    //                           the new claude finishing a turn: the continuation.
    //   sinceRestartMs        — milliseconds since the restart ran (a monotonic clock; negative reads as 0).
    // Returns the outcome. Never throws.
    inline ConnectionLostRestartOutcome DecideConnectionLostRestartOutcome(bool live, SessionState state, bool turnEndedSinceRestart, int64_t sinceRestartMs) noexcept
    {
        if (!live)
        {
            return ConnectionLostRestartOutcome::Gone;
        }
        if (state == SessionState::Running || state == SessionState::NeedsApproval)
        {
            return ConnectionLostRestartOutcome::Healed;
        }
        if (turnEndedSinceRestart)
        {
            // The continuation ran and ended. In Error it died again (an Error before that end is the held start's, which
            // ends no turn); anywhere else it finished, too fast for a scan to have seen it run.
            return state == SessionState::Error ? ConnectionLostRestartOutcome::NotHealed : ConnectionLostRestartOutcome::Healed;
        }
        if (sinceRestartMs >= kConnectionLostRestartConfirmMs)
        {
            return ConnectionLostRestartOutcome::NotHealed;
        }
        return ConnectionLostRestartOutcome::Waiting;
    }

    // Should a session the automatic restart HEALED get back the Prompt Queue Runner mode that the stop-on-error backstop
    // took away when the lost connection put it in Error? Only when the record of that pause belongs to the very Error
    // episode that was healed, nothing changed the mode since (it is still Off), and the pause took a real mode away (an
    // interrupt hold's Off is not given back). Pure.
    //   a                — the session's runner state now (AutorunnerState::stopOnErrorPausedMode / …EpisodeMs are set by
    //                      Scheduler::OnObserved and forgotten by ClearInterruptHold — every later mode decision).
    //   healedEpisodeMs  — SessionInfo::stateChangedAtUnixMs of the Error episode the automatic restart was made for.
    // Returns true when the runner should go back to a.stopOnErrorPausedMode. Never throws.
    inline bool ShouldGiveRunnerBackAfterRestart(const AutorunnerState& a, int64_t healedEpisodeMs) noexcept
    {
        return a.mode == AutorunnerMode::Off && a.stopOnErrorPausedMode != AutorunnerMode::Off && a.stopOnErrorPausedEpisodeMs != 0 &&
               a.stopOnErrorPausedEpisodeMs == healedEpisodeMs;
    }

    // The process-wide memory of the automatic restarts: per session, the error row the last one was made for and when each
    // one ran. One instance per process (SharedConnectionLostRestartLedger), because one engine serves every window (Rule
    // #10) and a tab torn out into another window must keep its budget. Thread-safe (every window's UI thread uses it).
    // In memory only: an app restart starts every budget afresh, which is fine — the app's own restart already resumed
    // every tab once.
    class ConnectionLostRestartLedger
    {
    public:
        // What the ledger knows about one session.
        struct Entry
        {
            std::wstring lastErrorUuid; // the error row of the newest automatic restart ("" when none)
            size_t restartsInWindow{ 0 }; // automatic restarts within kConnectionLostRestartWindowMs of the asked time
        };

        // Look a session up.
        //   sessionId — the session.
        //   nowMs     — the caller's monotonic clock (the same clock as Note's).
        // Returns its entry (empty for a session never restarted).
        // Throws std::system_error when the mutex cannot be locked, std::bad_alloc when the copy cannot be allocated.
        Entry Look(std::wstring_view sessionId, int64_t nowMs) const
        {
            std::lock_guard lock{ _mtx };
            Entry out;
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return out;
            }
            out.lastErrorUuid = it->second.lastErrorUuid;
            for (const int64_t t : it->second.restartsMs)
            {
                if (_InWindow(t, nowMs))
                {
                    ++out.restartsInWindow;
                }
            }
            return out;
        }

        // Record an automatic restart, BEFORE it runs (so nothing that runs inside it can start a second one).
        //   sessionId — the session restarted.
        //   errorUuid — the error row it is made for (ApiErrorRewindPlan::errorUuid; "" when unknown).
        //   nowMs     — the caller's monotonic clock.
        // Throws std::system_error when the mutex cannot be locked, std::bad_alloc when the record cannot grow.
        void Note(std::wstring_view sessionId, std::wstring_view errorUuid, int64_t nowMs)
        {
            std::lock_guard lock{ _mtx };
            auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                it = _sessions.emplace(std::wstring{ sessionId }, Record{}).first;
            }
            auto& times = it->second.restartsMs;
            // Keep only the restarts that still count, so a session restarted every few hours never grows its record.
            times.erase(std::remove_if(times.begin(), times.end(), [nowMs](int64_t t) { return !_InWindow(t, nowMs); }), times.end());
            times.push_back(nowMs);
            it->second.lastErrorUuid.assign(errorUuid);
        }

    private:
        // One session's history.
        struct Record
        {
            std::wstring lastErrorUuid;
            std::vector<int64_t> restartsMs;
        };

        // Does a restart at `t` still count at `nowMs`? A time in the future (a clock that went back) counts, so a clock
        // jump can only make the budget stricter, never looser.
        static bool _InWindow(int64_t t, int64_t nowMs) noexcept
        {
            return nowMs - t < kConnectionLostRestartWindowMs;
        }

        mutable std::mutex _mtx;
        std::map<std::wstring, Record, std::less<>> _sessions; // std::less<> — Look / Note take a wstring_view without a copy
    };

    // The one ledger of this process (a function-local static: built on first use, thread-safe by the language).
    // Returns the ledger. Never throws.
    inline ConnectionLostRestartLedger& SharedConnectionLostRestartLedger() noexcept
    {
        static ConnectionLostRestartLedger ledger;
        return ledger;
    }
}
