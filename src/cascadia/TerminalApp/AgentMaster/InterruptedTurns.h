// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — INTERRUPTED TURNS (HOOKS.md "Interrupted turns"): what the user chooses when a Restart meets a
// turn they interrupted, or a Fork meets a turn that is still running — and the pure rules that carry the choice
// out. Header-only and pure (no Windows, no WinRT): the engine (Engine.h, Messaging.h, RemoteLaunch.h), the
// TerminalPage glue and the m5 harness (TestInterruptedTurns) include it.
//
// THE TWO QUESTIONS (the user's request of 2026-10-22):
//   * RESTART — "When restarting a session and user has interrupted the session, ask user if he/she would like
//     to uninterrupt (we edit the session transcripts on disk before restart)." A turn the user stopped with Esc
//     ends in Claude Code's interrupt row ("[Request interrupted by user]" / "... for tool use"), which Claude
//     Code's resume counts as a FINISHED turn: a restarted claude just waits. Uninterrupting appends ONE
//     last-prompt line before the new claude starts (ApiErrorRewind.h, PlanInterruptRewind): the active leaf
//     moves to the user row below the interrupted reply, the restarted claude sees an unfinished turn and
//     continues it by itself. Nothing is deleted.
//   * FORK — "A fork that is in the middle of a run should be prompted if to fork as interrupted or
//     uninterrupted and thus user chooses if agentmaster should lock the tab and press esc once the tab starts
//     running as its supposed to be stopped from the auto resume." A fork is `claude --resume <source>
//     --fork-session`; Claude Code's resume continues an unfinished turn by itself (measured on 2.1.291: the
//     fork's heartbeat went busy ~5 s after launch and its first request followed ~1 s later). "Fork
//     interrupted" = Agentmaster locks the fork's tab (read-only) and presses ONE Esc the moment the fork's own
//     heartbeat says its turn runs (DecideForkTurnStop below); "Fork uninterrupted" = the fork runs on.
//
// Every value-initialized choice (0) is today's behavior: KeepInterrupted (no transcript edit, no question) and
// Uninterrupted (no Esc). A caller that never heard of this feature therefore behaves exactly as before.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Agentmaster
{
    // What a Restart does about a turn the user interrupted (HOOKS.md "Interrupted turns").
    enum class RestartTurnChoice : int32_t
    {
        // Restart as before: the transcript is not touched, and the restarted claude waits at the interrupt.
        // Also the answer for every non-interactive restart (a Telegram restart of a running session, the
        // Reactivate of an errored one) and for a restart with nothing to ask about.
        KeepInterrupted = 0,
        // Take the interrupt back before the restarted claude starts (the user said so): the restart plans the
        // rewind line, holds the new connection's Start until the old claude has exited (it would re-stamp its
        // leaf at exit — ApiErrorRewind.h, TIMING), appends the line, and starts the claude with
        // CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 in its environment so it continues by itself.
        Uninterrupt = 1,
        // Interactive: plan first, and when the active branch ends in a user interrupt that Claude Code would
        // continue after a rewind, ASK (the PC's dialog in the window the user clicked in; Telegram asks with
        // its own buttons and passes one of the two answers). Nothing to ask about == KeepInterrupted.
        Ask = 2,
    };

    // What a Fork does about the source's turn it would continue by itself (HOOKS.md "Interrupted turns").
    enum class ForkTurnChoice : int32_t
    {
        // Fork as before: the fork continues the source's unfinished turn on its own (Claude Code's resume).
        // Also the answer for every programmatic fork (the AI search's fork tool) and for a source at rest.
        Uninterrupted = 0,
        // Stop the fork's continued turn: Agentmaster arms the FORK STOP WATCH on the new session — the fork's
        // tab is locked read-only, and ONE Esc is pressed the moment the fork's claude reports a running turn;
        // the tab is unlocked once the turn stopped (or nothing ran). The intent is persisted on the fork's
        // record (SessionInfo::stopForkedTurn) until a resume of the fork would no longer continue anything.
        Interrupted = 1,
        // Interactive: decide whether a fork of the source would continue a turn (the source's transcript tail
        // + CLAUDE_CODE_RESUME_INTERRUPTED_TURN + the source's live state) and, when it would, ASK. Nothing to
        // ask about == Uninterrupted.
        Ask = 2,
    };

    // The log word of a restart choice ("keep-interrupted" / "uninterrupt" / "ask"). Pure; never throws.
    //   c — the choice.
    // Returns a static string.
    inline const wchar_t* RestartTurnChoiceName(RestartTurnChoice c) noexcept
    {
        switch (c)
        {
        case RestartTurnChoice::KeepInterrupted:
            return L"keep-interrupted";
        case RestartTurnChoice::Uninterrupt:
            return L"uninterrupt";
        case RestartTurnChoice::Ask:
            return L"ask";
        }
        return L"unknown";
    }

    // The log word of a fork choice ("uninterrupted" / "interrupted" / "ask"). Pure; never throws.
    //   c — the choice.
    // Returns a static string.
    inline const wchar_t* ForkTurnChoiceName(ForkTurnChoice c) noexcept
    {
        switch (c)
        {
        case ForkTurnChoice::Uninterrupted:
            return L"uninterrupted";
        case ForkTurnChoice::Interrupted:
            return L"interrupted";
        case ForkTurnChoice::Ask:
            return L"ask";
        }
        return L"unknown";
    }

    // Claude Code's truthiness rule for its environment switches (2.1.291: `["1","true","yes","on"]
    // .includes(v.toLowerCase().trim())`), so "On", " yes " and "1" count and "0", "false", "" do not. ASCII
    // case folding and trimming only — the accepted words are ASCII. Pure; never throws.
    //   value — the variable's raw value.
    // Returns true when Claude Code would read the switch as ON.
    inline bool IsClaudeEnvTruthy(std::wstring_view value) noexcept
    {
        // JavaScript's trim() also removes Unicode spaces; a value written with one is not worth a table — an
        // ASCII space, tab, CR or LF is what a settings file or a `set` line realistically carries.
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' || value.front() == L'\r' || value.front() == L'\n'))
        {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r' || value.back() == L'\n'))
        {
            value.remove_suffix(1);
        }
        if (value.size() > 4)
        {
            return false; // longer than "true": none of the four words
        }
        wchar_t lower[5]{};
        for (size_t i = 0; i < value.size(); ++i)
        {
            const wchar_t c = value[i];
            lower[i] = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }
        const std::wstring_view v{ lower, value.size() };
        return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
    }

    // ===== the FORK STOP WATCH — the pure decision (TerminalPage::_ForkTurnStopTick runs it) =================
    // A fork whose user chose "Fork interrupted" is watched from its launch until its continued turn is stopped,
    // or until it is clear that nothing will run. The tab is read-only (locked) meanwhile. The signal is the
    // fork's OWN heartbeat (~/.claude/sessions/<pid>.json of the claude.exe under the fork's ConPTY): Claude Code
    // rewrites it at every status change, and its "busy" was the first sign of the auto-continue in every probe
    // (~5 s after launch; the request followed ~0.4–1.1 s later). A turn that re-runs a TOOL starts executing
    // it almost at once, so the watch polls fast (kForkTurnStopFastTickMs) as soon as the fork's claude exists.

    // The watch's tick while some watched fork has STARTED (its claude runs): the Esc must land within the first
    // ~100 ms of the turn, before a re-issued tool call can do anything.
    inline constexpr int64_t kForkTurnStopFastTickMs = 100;
    // The tick while every watched fork is still dormant (a background tab starts on a human's first visit, which
    // can be hours away): cheap, and still well inside the ~4 s between a claude's start and its auto-continue.
    inline constexpr int64_t kForkTurnStopSlowTickMs = 1000;
    // How long the fork may sit idle with its input box on screen before the watch concludes that the fork did
    // NOT continue a turn (the source had finished by the time the fork started, or the switch was off) and
    // unlocks the tab. Claude Code submits the continuation as the REPL mounts, so a few seconds of idle with
    // the box up is conclusive.
    inline constexpr int64_t kForkTurnStopIdleSettleMs = 6000;
    // The belt behind the idle settle (a heartbeat or a box that cannot be read): this long after the fork's
    // claude started with no turn seen, the watch gives the tab back. Generous next to the ~5 s a start took in the
    // probe (a start that loads many MCP servers takes longer), short enough that a startup dialog the lock keeps the
    // user from answering is not held for long.
    inline constexpr int64_t kForkTurnStopStartCapMs = 45000;
    // How often the watch reads the fork's input box while the heartbeat says idle (a scan of the terminal buffer,
    // not needed at the 100 ms tick).
    inline constexpr int64_t kForkTurnStopBoxProbeMs = 500;
    // After an Esc: the turn must stop within this, else the next Esc goes (an Esc can be swallowed while the
    // TUI is busy). Generous on purpose: two Esc presses within Claude Code's double-press window on an idle,
    // empty box would open its rewind menu.
    inline constexpr int64_t kForkTurnStopVerifyMs = 1500;
    // Esc presses before the watch gives up (unlocks and logs; the user stops it by hand).
    inline constexpr int32_t kForkTurnStopMaxPresses = 3;
    // How often the watch may look for the fork's claude.exe in its ConPTY's process tree (a Toolhelp snapshot,
    // ~10 ms) while it is not found yet.
    inline constexpr int64_t kForkTurnStopPidLookupMs = 400;

    // What one tick knows about a watched fork. Built by the page from its control, the fork's heartbeat and the
    // registry; every time is a tick-clock value (GetTickCount64) except where a field says otherwise.
    struct ForkTurnStopFacts
    {
        // The fork's tab, control or connection is gone, or the connection CLOSED (the claude exited): the watch
        // ends without pressing anything.
        bool connectionGone{ false };
        // The fork's connection is Connected: its claude was spawned (a background tab stays NotConnected until
        // its first visit).
        bool started{ false };
        // The tick at which the watch first saw `started` (0 while not started).
        int64_t startedAtMs{ 0 };
        // The fork's claude heartbeat was read THIS tick (its pid known and its file parsed for that process).
        bool heartbeatKnown{ false };
        // The heartbeat's status is "busy" or "waiting": a turn runs (or waits on a question it re-asked).
        bool turnRunning{ false };
        // The registry's derived state is Running or NeedsApproval — the scanner's belt for a heartbeat that
        // cannot be read (seconds late, so it only matters when the heartbeat never answers).
        bool registryRunning{ false };
        // The first tick of the current stretch in which the heartbeat said "idle" AND the fork's input box was
        // on screen (0 = not in such a stretch). Reset whenever either stops holding.
        int64_t idleBoxSinceMs{ 0 };
        // Esc presses made so far for this connection, and the tick of the last one.
        int32_t presses{ 0 };
        int64_t lastPressMs{ 0 };
    };

    // What the watch does after a tick.
    enum class ForkTurnStopAction
    {
        Wait, // nothing yet: keep watching (and keep the tab locked once it started)
        PressEsc, // press ONE Esc into the fork's input (a turn runs, or the last press did not stop it yet)
        Stopped, // a press stopped the turn: unlock, done
        NothingToStop, // the fork started and ran no turn: unlock, done
        GaveUp, // presses ran out and the turn still runs: unlock, log — the user must stop it by hand
        Gone, // the tab / connection went away: drop the watch (the persisted intent stays for the next launch)
    };

    // The decision for one tick. Pure and total; never throws.
    //   f     — what this tick knows (see ForkTurnStopFacts).
    //   nowMs — the tick clock now (the same clock as every time in `f`).
    // Returns the action. The rules, in order: a gone connection ends the watch; a dormant one waits (unbounded —
    // a background tab may be visited hours later); before any press a running turn is pressed at once, and an
    // idle fork with its box up for kForkTurnStopIdleSettleMs (or any fork kForkTurnStopStartCapMs after its
    // start) had nothing to stop; after a press a heartbeat that no longer says busy/waiting means stopped (with
    // no heartbeat: the registry not Running once the verify window passed), a turn that still runs after
    // kForkTurnStopVerifyMs is pressed again, and after kForkTurnStopMaxPresses the watch gives up.
    inline ForkTurnStopAction DecideForkTurnStop(const ForkTurnStopFacts& f, int64_t nowMs) noexcept
    {
        if (f.connectionGone)
        {
            return ForkTurnStopAction::Gone;
        }
        if (!f.started)
        {
            return ForkTurnStopAction::Wait;
        }
        // The heartbeat is the authority whenever it answers; the registry only stands in for a heartbeat that
        // cannot be read (an older claude without the file, a denied read).
        const bool running = f.heartbeatKnown ? f.turnRunning : f.registryRunning;
        if (f.presses == 0)
        {
            if (running)
            {
                return ForkTurnStopAction::PressEsc;
            }
            if (f.idleBoxSinceMs != 0 && nowMs - f.idleBoxSinceMs >= kForkTurnStopIdleSettleMs)
            {
                return ForkTurnStopAction::NothingToStop;
            }
            if (f.startedAtMs != 0 && nowMs - f.startedAtMs >= kForkTurnStopStartCapMs)
            {
                return ForkTurnStopAction::NothingToStop;
            }
            return ForkTurnStopAction::Wait;
        }
        if (f.heartbeatKnown)
        {
            if (!f.turnRunning)
            {
                return ForkTurnStopAction::Stopped;
            }
        }
        else if (!f.registryRunning && nowMs - f.lastPressMs >= kForkTurnStopVerifyMs)
        {
            // No heartbeat to ask: the registry is seconds late, so it is believed only after the verify window.
            return ForkTurnStopAction::Stopped;
        }
        if (nowMs - f.lastPressMs < kForkTurnStopVerifyMs)
        {
            return ForkTurnStopAction::Wait; // the TUI needs a moment to stop and rewrite its heartbeat
        }
        return f.presses < kForkTurnStopMaxPresses ? ForkTurnStopAction::PressEsc : ForkTurnStopAction::GaveUp;
    }

    // The log word of an action ("wait" / "press-esc" / "stopped" / "nothing-to-stop" / "gave-up" / "gone").
    // Pure; never throws.
    //   a — the action.
    // Returns a static string.
    inline const wchar_t* ForkTurnStopActionName(ForkTurnStopAction a) noexcept
    {
        switch (a)
        {
        case ForkTurnStopAction::Wait:
            return L"wait";
        case ForkTurnStopAction::PressEsc:
            return L"press-esc";
        case ForkTurnStopAction::Stopped:
            return L"stopped";
        case ForkTurnStopAction::NothingToStop:
            return L"nothing-to-stop";
        case ForkTurnStopAction::GaveUp:
            return L"gave-up";
        case ForkTurnStopAction::Gone:
            return L"gone";
        }
        return L"unknown";
    }

    // ===== the questions' words (the PC's dialogs; Telegram builds its own HTML from the same facts) ========

    // How Claude Code's CLAUDE_CODE_RESUME_INTERRUPTED_TURN stands for the claude a restart starts — the switch that
    // makes a resumed claude SEND the continuation by itself instead of only preparing it.
    enum class AutoContinueAtRestart : int32_t
    {
        // On where the session runs (a settings file's env, or the environment): the restarted claude continues.
        On = 0,
        // Set nowhere: the restart adds it to its one claude's environment (ClaudeSpawnSpec env), so it continues.
        TurnedOnForThisRestart = 1,
        // A settings file sets it OFF, and a settings file's env overrides a process environment: the restart cannot
        // turn it on. The interrupt is still taken back; the restarted claude then waits, and the user's next
        // message continues the turn.
        OffInSettings = 2,
    };

    // The value of AutoContinueAtRestart for a switch resolved by ResolveClaudeAutoContinue (ApiErrorRewind.h).
    // Pure; never throws.
    //   on             — the switch resolved truthy.
    //   fromSettings   — the deciding value came from a settings file (not the spawn / process environment, and
    //                    not "nowhere").
    // Returns On, TurnedOnForThisRestart (off and not decided by a settings file — the spawn env wins) or
    // OffInSettings.
    inline AutoContinueAtRestart ClassifyAutoContinueAtRestart(bool on, bool fromSettings) noexcept
    {
        if (on)
        {
            return AutoContinueAtRestart::On;
        }
        return fromSettings ? AutoContinueAtRestart::OffInSettings : AutoContinueAtRestart::TurnedOnForThisRestart;
    }

    // The body of the RESTART question ("Continue the interrupted turn?"). Pure.
    //   title         — the session's title ("" = say "this session").
    //   duringToolUse — the interrupt came during a tool call ("[Request interrupted by user for tool use]").
    //   resendsPrompt — the rewind lands on the user's own prompt (Claude Code sends it again), else on a finished
    //                   tool result (it carries on from there).
    //   promptText    — resendsPrompt: the prompt's first line (capped by the caller; "" = not shown).
    //   autoContinue  — how CLAUDE_CODE_RESUME_INTERRUPTED_TURN stands for the restarted claude (the text says what
    //                   that means when it is not simply on).
    //   offSource     — OffInSettings: the settings file that turns it off ("" = not named).
    // Returns the text (paragraphs separated by a blank line).
    // Throws std::bad_alloc only.
    inline std::wstring UninterruptQuestionBody(std::wstring_view title, bool duringToolUse, bool resendsPrompt, std::wstring_view promptText, AutoContinueAtRestart autoContinue, std::wstring_view offSource)
    {
        std::wstring s = L"You interrupted ";
        s += title.empty() ? std::wstring{ L"this session" } : (L"\x201C" + std::wstring{ title } + L"\x201D");
        s += duringToolUse ? L" during a tool call." : L" in the middle of a reply.";
        s += L"\n\nContinue the turn: before the restart, Agentmaster adds one line to the session's transcript that takes the interruption back. ";
        if (autoContinue == AutoContinueAtRestart::OffInSettings)
        {
            // The switch is off in a settings file, which outranks the environment the restart could set: say what
            // happens instead of promising an automatic continuation.
            s += L"Claude Code's CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off";
            if (!offSource.empty())
            {
                s += L" (in " + std::wstring{ offSource } + L")";
            }
            s += L", so the restarted Claude waits with the turn unfinished, and your next message continues it \x2014 ";
        }
        else
        {
            s += L"The restarted Claude then continues the turn by itself \x2014 ";
        }
        if (resendsPrompt)
        {
            s += promptText.empty() ? std::wstring{ L"from your prompt, sent again." } : (L"from your prompt \x201C" + std::wstring{ promptText } + L"\x201D, sent again.");
        }
        else
        {
            s += L"from the last step that finished.";
        }
        s += L" Nothing is deleted: the interrupted reply stays in the file, on a branch the conversation no longer follows.";
        if (autoContinue == AutoContinueAtRestart::TurnedOnForThisRestart)
        {
            s += L" (Claude Code's CLAUDE_CODE_RESUME_INTERRUPTED_TURN is not set here; this one restart sets it.)";
        }
        s += L"\n\nKeep it interrupted: the restarted session waits for you, as it does today.";
        return s;
    }

    // The body of the FORK question ("Fork a session in the middle of a turn?"). Pure.
    //   title       — the source's title ("" = say "this session").
    //   resendsPrompt — the fork would send the source's last prompt again (the turn had produced no step yet),
    //                 else it continues from the last finished step.
    //   rerunTool   — the source was in this tool call (its name), which the fork re-runs ("" = none known).
    //   sourceRunning — the source is running right now (vs. a turn left unfinished on disk — a crash, an error).
    // Returns the text (paragraphs separated by a blank line).
    // Throws std::bad_alloc only.
    inline std::wstring ForkTurnQuestionBody(std::wstring_view title, bool resendsPrompt, std::wstring_view rerunTool, bool sourceRunning)
    {
        std::wstring s = title.empty() ? std::wstring{ L"This session" } : (L"\x201C" + std::wstring{ title } + L"\x201D");
        s += sourceRunning ? L" is in the middle of a turn." : L" stopped in the middle of a turn.";
        s += L" A fork starts from the same conversation, and Claude Code continues that turn in the fork by itself as soon as the fork's Claude starts";
        if (!rerunTool.empty())
        {
            s += L" \x2014 the " + std::wstring{ rerunTool } + L" call it was running runs again in the fork";
        }
        else if (resendsPrompt)
        {
            s += L" \x2014 the last prompt is sent again";
        }
        s += L".\n\nFork interrupted: Agentmaster locks the fork's tab and presses Esc the moment the fork starts running, so the fork waits for you.";
        s += L"\n\nFork uninterrupted: the fork runs the turn on its own";
        s += sourceRunning ? L", beside the original." : L".";
        return s;
    }
}
