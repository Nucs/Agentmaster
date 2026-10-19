// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the RESTORE WAKE (the user's request of 2026-10-19; doc/agentmaster/PERSISTENCE.md §14).
//
// A window restore re-homes every tab of a saved window DORMANT: the tab's terminal starts its claude only on the
// tab's first layout, so after a restart only the tab each window opens to starts (TerminalPage::_RestoreWindowTabs).
// That is right for the hundred tabs that sat idle, and wrong for the few that were in the middle of something when
// Agentmaster went down: they sit dormant and stuck until someone clicks them. The restore wake starts those tabs by
// itself — AFTER the tab every reopened window opens to has finished launching, so the tabs the user looks at get the
// machine first, and paced ACROSS EVERY WINDOW (the "Activate All Tabs" drip's 500 ms apart, at most 4 per 10 s, but
// process-wide: the barrier releases all windows at once).
//
// This header is the decision half, plain C++ and free of WinRT so the m5 harness tests all of it:
//   * RestoreWakeWantsSession — WHICH restored tabs to wake: the sessions whose state at the restart was anything but
//     Idle / Waiting-for-you / Done (persisted Running, NeedsApproval or Error — SessionInfo::midTurnAtRestore), minus
//     the ones the scanner has since proved at rest.
//   * the per-window FOREGROUND gate (RestoreWakeForegroundStep) — WHEN a window's own share is ready: the tab it opened
//     to has finished launching (its claude fired a hook after the restore, or a terminal that hosts no claude started,
//     or there is no terminal to wait for), and later (RestoreWakeDecideWake) the UI thread has been idle for a moment.
//   * RestoreWakeBarrier — the process-wide join across windows: every window reopened together waits until EVERY one
//     of them has its foreground launched, then all of them wake their busy tabs. Every wait is capped, so a window that
//     never comes up delays the others by minutes at most, never forever.
//   * RestoreWakePacer — the process-wide pace of the wakes themselves (one wake per slot, slots 500 ms apart and at
//     most 4 per 10 s), so the windows the barrier releases together take turns instead of bursting.
// The glue (reading a window's foreground, the timer, the wake itself) is TerminalPage's (TerminalPage.AgentWindowRecord.cpp).
// The process-wide instances are SharedRestoreWakeBarrier() and SharedRestoreWakePacer() — header-inline, so each is one
// instance per module (TerminalApp.dll holds every window of the process; the harness tests local instances instead).

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SessionModels.h" // SessionInfo / SessionState — the session predicate's input

namespace Agentmaster
{
    // ------------------------------------------------------------------------------------------------------------------
    // Timing. Every value is a cap or a cadence of the startup wait only: none of them delays a tab the user opens.
    // ------------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// How often a window's restore-wake timer ticks (ms). Short enough that a punctual streak of
    /// <see cref="kRestoreWakeIdleTicks"/> ticks reads "the UI thread is idle" within 1.5 s — the splash watch's own bar
    /// (TerminalPage::_TickSplashDismiss) — and cheap: the timer runs only from a window's restore until its wake.
    /// </summary>
    inline constexpr uint32_t kRestoreWakeTickMs = 250;

    /// <summary>
    /// A tick that arrives within this gap of the previous one (ms) counts as PUNCTUAL: the UI thread pumped the timer on
    /// time, so it is not jammed by restore / layout work. A later tick resets the streak. Same value as the splash watch.
    /// </summary>
    inline constexpr uint64_t kRestoreWakePunctualGapMs = 600;

    /// <summary>
    /// How many punctual ticks in a row mean "the UI thread is idle" (6 × 250 ms = 1.5 s). The wake waits for it, because
    /// waking a tab initializes its terminal ON the UI thread, and piling that onto a window that is still binding its
    /// restored fleet only makes every window slower.
    /// </summary>
    inline constexpr int kRestoreWakeIdleTicks = 6;

    /// <summary>
    /// How long (ms) a STARTED foreground claude may stay silent before it counts as launched anyway. Normally its
    /// SessionStart hook proves it up within seconds; this fallback covers a claude whose hooks never arrive (a broken
    /// forwarder, or a re-forked never-messaged fork, whose startup SessionStart arrives under its SOURCE id and is dropped
    /// by the fork-echo guard). Too short would wake the busy tabs while the focused claude is still booting.
    /// </summary>
    inline constexpr uint64_t kRestoreWakeHooklessSettleMs = 15'000;

    /// <summary>
    /// The cap (ms after the window's re-home ended) past which the window's foreground counts as launched whatever it
    /// shows — a focused tab that never starts (the user switched away before its first layout) or a claude stuck on a
    /// startup modal must not hold the busy tabs of every window forever. Generous, because a big restore keeps the UI
    /// thread busy for ~20 s before the focused terminal is even laid out (release 2026-10-19: 18–26 s).
    /// </summary>
    inline constexpr uint64_t kRestoreWakeForegroundCapMs = 90'000;

    /// <summary>
    /// The barrier's cap (ms after its OLDEST still-waiting window entered it): past it every waiting window is released,
    /// ready or not. It bounds the cost of a window that was announced but never restores (destroyed before its first
    /// layout, or a restore that failed without leaving) to a late wake of the others, never a lost one.
    /// </summary>
    inline constexpr uint64_t kRestoreWakeBarrierCapMs = 300'000;

    /// <summary>
    /// After the barrier released a window, how long (ms) the window waits for an idle UI thread before it wakes its busy
    /// tabs anyway. The pacer below spaces the wakes either way; this only stops a permanently busy UI thread (a huge
    /// fleet's steady churn) from holding the wake back for good.
    /// </summary>
    inline constexpr uint64_t kRestoreWakeIdleWaitCapMs = 30'000;

    /// <summary>
    /// The PACER's minimum gap (ms) between two wakes ACROSS EVERY WINDOW — the "Activate All Tabs" drip's spacing, applied
    /// process-wide. Waking a tab initializes its terminal on the one UI thread and spawns pwsh + claude.exe; all windows
    /// are released by the barrier at the same moment, so per-window pacing alone would let four windows start sixteen
    /// claudes within 1.5 s — the burst the drip exists to prevent.
    /// </summary>
    inline constexpr uint64_t kRestoreWakeSpacingMs = 500;

    /// <summary>The pacer's batch: at most this many wakes in any <see cref="kRestoreWakeBatchPeriodMs"/> window, process-wide.</summary>
    inline constexpr size_t kRestoreWakeBatchSize = 4;

    /// <summary>
    /// The pacer's period (ms): with <see cref="kRestoreWakeBatchSize"/> and <see cref="kRestoreWakeSpacingMs"/>, wakes run at
    /// 0 / 0.5 / 1 / 1.5 s, then 10 / 10.5 / 11 / 11.5 s, … — the drip's own timeline. Five busy tabs take ~10 s, thirty ~75 s.
    /// </summary>
    inline constexpr uint64_t kRestoreWakeBatchPeriodMs = 10'000;

    /// <summary>
    /// The wall clock every SessionInfo timestamp uses — Unix-epoch milliseconds from the system clock — so a hook's
    /// wire ts (SessionInfo::lastHookUnixMs) can be compared with the moment a window's restore ended. A steady_clock
    /// reading would live in another epoch and make that comparison meaningless.
    /// </summary>
    /// <returns>Milliseconds since 1970-01-01 UTC.</returns>
    inline int64_t RestoreWakeUnixNowMs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ------------------------------------------------------------------------------------------------------------------
    // WHICH tabs: the session predicate.
    // ------------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// Should the restore wake start this restored session's tab? True for a DORMANT tab (live, never started, ours)
    /// whose record said, at the restart, that the session was in the middle of something — its persisted state was
    /// Running, NeedsApproval or Error (anything but Idle / Waiting-for-you / Done: SessionInfo::midTurnAtRestore, set by
    /// the restore seams through RestoredMidTurnFlag) — and that nothing has proved at rest since.
    /// </summary>
    /// <param name="s">The session's current registry record (a lean copy without the queue is enough).</param>
    /// <returns>True when waking its tab now is wanted.</returns>
    /// <remarks>
    /// <para>The state is read at WAKE time on purpose: the scanner refines a restored record while it waits. By state:</para>
    /// <para>Idle — a persisted Running or Error, normalized by RestoredSessionState: wake (the scanner may also not have
    /// classified a cut-off yet).</para>
    /// <para>NeedsApproval — kept as is by the restore (a question or a permission prompt that died with its process): wake,
    /// so the conversation is up again and can be answered or continued; what the resumed claude does with the open
    /// question is Claude Code's (under CLAUDE_CODE_RESUME_INTERRUPTED_TURN it continues the turn — not verified for an
    /// unanswered question). The scanner usually re-reads such a dormant tab as a cut-off Error before the wake.</para>
    /// <para>Error — the scanner's cut-off / API-error verdict since the restore: wake; a resumed claude continues the turn
    /// (CLAUDE_CODE_RESUME_INTERRUPTED_TURN, and the API-error rewind of HOOKS.md applied before the tab could start).</para>
    /// <para>WaitingForInput — the scanner's recon-stop proved the turn ended: no wake — EXCEPT for a PINNED session,
    /// whose restore normalization to Idle the registry's pin hold turns into WaitingForInput (ApplyPinHold), so there it
    /// still means "was in the middle of something".</para>
    /// <para>Running — a turn runs in a live process somewhere (another tab's claude on the same conversation): starting
    /// this tab would put a second writer on the transcript, so no wake. Done — no wake.</para>
    /// <para>⚠ A record that was Idle / Waiting-for-you / Done at the restart is never woken, even when the scanner later
    /// reads it as Error (an error the user had dismissed comes back as Error after a restart): the user's rule is the
    /// state AT THE RESTART, and re-running a turn the user abandoned would spend tokens on it.</para>
    /// </remarks>
    inline bool RestoreWakeWantsSession(const SessionInfo& s) noexcept
    {
        if (!s.live || s.started || s.external || !s.midTurnAtRestore)
        {
            return false; // closed, already running, not ours, or at rest when Agentmaster went down
        }
        switch (s.state)
        {
        case SessionState::Idle:
        case SessionState::NeedsApproval:
        case SessionState::Error:
            return true;
        case SessionState::WaitingForInput:
            return s.pinned; // the pin hold's Waiting is a normalized Idle; an unpinned one is a proved turn end
        case SessionState::Running:
        case SessionState::Done:
        default:
            return false;
        }
    }

    // ------------------------------------------------------------------------------------------------------------------
    // WHEN, per window: the foreground gate + the idle UI.
    // ------------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// What a window's FOREGROUND looks like on one tick — the tab the restore focused when that is a session tab this
    /// window still hosts, else whatever the window shows now. Read by TerminalPage::_ReadRestoreWakeForeground; all
    /// false / 0 means "nothing to wait for" (the Agent Manager page, no tab, a Settings tab, or a failed read).
    /// </summary>
    struct RestoreWakeForeground
    {
        bool terminal{ false }; // a terminal control is what the window opens to; false: nothing launches there, so nothing to wait for
        bool started{ false }; // that terminal's connection left NotConnected — its process was spawned
        bool claudeSession{ false }; // it hosts a managed CLAUDE session, whose claude must come up (a hook) before it counts as launched
        int64_t lastHookUnixMs{ 0 }; // that session's newest hook (SessionInfo::lastHookUnixMs, wall clock); 0 = none this run
    };

    /// <summary>Why a window's foreground counts as LAUNCHED — the [restore-wake] log names it.</summary>
    enum class RestoreWakeForegroundReason
    {
        NoTerminal, // the window opened to no terminal (the Agent Manager page, nothing): nothing launches there
        TerminalStarted, // its terminal hosts no Claude session (a shell, a Codex) and its connection started
        ClaudeUp, // its claude fired a hook after the restore ended (its SessionStart, normally) — it is up
        ClaudeSilent, // its claude started but no hook came within kRestoreWakeHooklessSettleMs
        TimedOut, // kRestoreWakeForegroundCapMs passed since the restore without any of the above
    };

    /// <summary>
    /// The [restore-wake] log's words for a <see cref="RestoreWakeForegroundReason"/>. Free of the caps' numbers, so a
    /// changed constant can never leave a lying log line (the line carries the elapsed time itself).
    /// </summary>
    /// <param name="reason">The reason.</param>
    /// <returns>A static, never-null text.</returns>
    inline const wchar_t* RestoreWakeForegroundReasonText(RestoreWakeForegroundReason reason) noexcept
    {
        switch (reason)
        {
        case RestoreWakeForegroundReason::NoTerminal:
            return L"it opened to no terminal";
        case RestoreWakeForegroundReason::TerminalStarted:
            return L"its terminal started (no Claude session there)";
        case RestoreWakeForegroundReason::ClaudeUp:
            return L"its claude is up (a hook arrived)";
        case RestoreWakeForegroundReason::ClaudeSilent:
            return L"its claude started but sent no hook in time";
        case RestoreWakeForegroundReason::TimedOut:
        default:
            return L"the wait for it timed out";
        }
    }

    /// <summary>Why a released window wakes its busy tabs now — the [restore-wake] log names it.</summary>
    enum class RestoreWakeWakeReason
    {
        UiIdle, // the UI thread pumped kRestoreWakeIdleTicks ticks on time in a row
        UiBusyTimedOut, // kRestoreWakeIdleWaitCapMs passed since the release without such a streak
    };

    /// <summary>The [restore-wake] log's words for a <see cref="RestoreWakeWakeReason"/> (number-free, like the others).</summary>
    /// <param name="reason">The reason.</param>
    /// <returns>A static, never-null text.</returns>
    inline const wchar_t* RestoreWakeWakeReasonText(RestoreWakeWakeReason reason) noexcept
    {
        return reason == RestoreWakeWakeReason::UiIdle ? L"the UI is idle" : L"the UI stayed busy too long; waking anyway";
    }

    /// <summary>
    /// One window's memory of its restore-wake wait, owned by its TerminalPage and advanced by its timer ticks. A
    /// default-constructed value is "not waiting"; TerminalPage::_BeginRestoreWake stamps the two restore instants.
    /// </summary>
    struct RestoreWakeWindowState
    {
        int64_t restoredAtUnixMs{ 0 }; // wall clock when the re-home ended: a hook at or after it came from a claude this restore started
        uint64_t restoredAtTickMs{ 0 }; // the monotonic clock (GetTickCount64) at the same moment: every cap is measured on it
        uint64_t lastTickMs{ 0 }; // the previous tick (monotonic), for the punctuality streak; the restore instant before the first tick
        int punctualTicks{ 0 }; // punctual ticks in a row (RestoreWakeNoteTick); kRestoreWakeIdleTicks of them = an idle UI thread
        uint64_t startedSeenTickMs{ 0 }; // the first tick that saw the foreground terminal started (0 = not yet): the hookless settle counts from it
        bool foregroundLaunched{ false }; // LATCHED once the foreground counts as launched; the barrier is told once
        uint64_t releasedSeenTickMs{ 0 }; // the first tick that saw the barrier release this window (0 = not yet): the idle wait counts from it
    };

    /// <summary>
    /// Fold one timer tick into the window's punctuality streak: a tick within <see cref="kRestoreWakePunctualGapMs"/> of
    /// the previous one extends it, a later one resets it to 0. Call it first on every tick.
    /// </summary>
    /// <param name="state">The window's wait state; its lastTickMs and punctualTicks are updated.</param>
    /// <param name="nowTickMs">The monotonic clock now (GetTickCount64). A clock that went backwards (only a test can do
    /// that) counts as a late tick.</param>
    inline void RestoreWakeNoteTick(RestoreWakeWindowState& state, uint64_t nowTickMs) noexcept
    {
        const bool punctual = nowTickMs >= state.lastTickMs && nowTickMs - state.lastTickMs < kRestoreWakePunctualGapMs;
        state.punctualTicks = punctual ? state.punctualTicks + 1 : 0;
        state.lastTickMs = nowTickMs;
    }

    /// <summary>
    /// Advance the window's FOREGROUND gate by one tick: has the tab the window opened to finished launching? Latches
    /// state.foregroundLaunched the first time the answer is yes, and returns the reason exactly that once, so the caller
    /// tells the barrier (RestoreWakeBarrier::NoteForegroundLaunched) and logs once.
    /// </summary>
    /// <param name="state">The window's wait state (restoredAt* stamped); startedSeenTickMs and foregroundLaunched are
    /// updated.</param>
    /// <param name="foreground">What the window's foreground looks like now.</param>
    /// <param name="nowTickMs">The monotonic clock now.</param>
    /// <returns>The reason when the foreground counts as launched as of THIS tick; nullopt while it is still launching and
    /// on every tick after the latch.</returns>
    /// <remarks>
    /// The order of the rules: no terminal → launched; a started terminal without a Claude session → launched; a started
    /// Claude terminal → launched once a hook newer than the restore arrived, or <see cref="kRestoreWakeHooklessSettleMs"/>
    /// after it was first seen started; and in every case <see cref="kRestoreWakeForegroundCapMs"/> after the restore.
    /// A hook OLDER than the restore (the session's earlier life in this same run — a window closed and reopened) proves
    /// nothing about the claude this restore started, hence the comparison with restoredAtUnixMs.
    /// </remarks>
    inline std::optional<RestoreWakeForegroundReason> RestoreWakeForegroundStep(RestoreWakeWindowState& state, const RestoreWakeForeground& foreground, uint64_t nowTickMs) noexcept
    {
        if (state.foregroundLaunched)
        {
            return std::nullopt; // latched on an earlier tick: the barrier already knows
        }
        std::optional<RestoreWakeForegroundReason> why;
        if (!foreground.terminal)
        {
            why = RestoreWakeForegroundReason::NoTerminal;
        }
        else if (foreground.started)
        {
            if (state.startedSeenTickMs == 0)
            {
                state.startedSeenTickMs = nowTickMs; // the hookless settle counts from the first tick that saw it started
            }
            if (!foreground.claudeSession)
            {
                why = RestoreWakeForegroundReason::TerminalStarted;
            }
            else if (foreground.lastHookUnixMs != 0 && foreground.lastHookUnixMs >= state.restoredAtUnixMs)
            {
                why = RestoreWakeForegroundReason::ClaudeUp;
            }
            else if (nowTickMs >= state.startedSeenTickMs && nowTickMs - state.startedSeenTickMs >= kRestoreWakeHooklessSettleMs)
            {
                why = RestoreWakeForegroundReason::ClaudeSilent;
            }
        }
        if (!why && nowTickMs >= state.restoredAtTickMs && nowTickMs - state.restoredAtTickMs >= kRestoreWakeForegroundCapMs)
        {
            why = RestoreWakeForegroundReason::TimedOut;
        }
        if (why)
        {
            state.foregroundLaunched = true;
        }
        return why;
    }

    /// <summary>
    /// Decide on one tick whether the window wakes its busy tabs now: only after its own foreground launched AND the
    /// barrier released it, and then once the UI thread is idle — or <see cref="kRestoreWakeIdleWaitCapMs"/> after the
    /// window first saw the release, whichever comes first.
    /// </summary>
    /// <param name="state">The window's wait state; releasedSeenTickMs is stamped on the first released tick.</param>
    /// <param name="nowTickMs">The monotonic clock now.</param>
    /// <param name="barrierReleased">RestoreWakeBarrier::IsReleased for this window, asked on this tick.</param>
    /// <returns>The reason to wake now; nullopt to keep waiting.</returns>
    inline std::optional<RestoreWakeWakeReason> RestoreWakeDecideWake(RestoreWakeWindowState& state, uint64_t nowTickMs, bool barrierReleased) noexcept
    {
        if (!state.foregroundLaunched || !barrierReleased)
        {
            return std::nullopt; // its own tab first, then every other window's
        }
        if (state.releasedSeenTickMs == 0)
        {
            state.releasedSeenTickMs = nowTickMs;
        }
        if (state.punctualTicks >= kRestoreWakeIdleTicks)
        {
            return RestoreWakeWakeReason::UiIdle;
        }
        if (nowTickMs >= state.releasedSeenTickMs && nowTickMs - state.releasedSeenTickMs >= kRestoreWakeIdleWaitCapMs)
        {
            return RestoreWakeWakeReason::UiBusyTimedOut;
        }
        return std::nullopt;
    }

    /// <summary>
    /// One window's whole restore-wake job, held by value in its TerminalPage: its barrier key, the tabs to wake, the
    /// session the restore focused, the gate's memory, and — once the barrier released it — the queue its timer drains
    /// through the process-wide pacer. A default-constructed value is "no job" (key empty, not waking).
    /// </summary>
    struct RestoreWakeWindowJob
    {
        std::wstring key; // this window's key in the barrier: the Emperor-assigned record id when announced, else the window id; "" = not in it
        std::vector<std::wstring> candidates; // the re-homed sessions RestoreWakeWantsSession picked at the restore, in strip order (re-checked at wake time)
        std::wstring focusSessionId; // the managed session the restore focused, "" = a shell / the Agent Manager page / nothing (then the current foreground counts)
        RestoreWakeWindowState state; // the per-window gate's memory
        bool waking{ false }; // the WAKING phase: the barrier released this window (it left it) and its timer now drains wakeQueue
        std::vector<std::wstring> wakeQueue; // the candidates that still wanted a wake when the window was released, in wake order; each is checked once more just before its wake
        size_t woken{ 0 }; // wakes done so far (the done line counts them)
        size_t skippedLate{ 0 }; // queued sessions that no longer wanted a wake by their turn (the user opened one meanwhile): no pacing slot spent
    };

    // ------------------------------------------------------------------------------------------------------------------
    // WHEN, across windows: the barrier.
    // ------------------------------------------------------------------------------------------------------------------

    /// <summary>One window's place in the <see cref="RestoreWakeBarrier"/>.</summary>
    struct RestoreWakeEntry
    {
        std::wstring key; // the window's key (unique among the entries)
        uint64_t sinceTickMs{ 0 }; // when it entered (announce or join, whichever came first): the barrier cap counts from the oldest waiting one
        bool joined{ false }; // its re-home finished (RestoreWakeBarrier::Join)
        bool foregroundLaunched{ false }; // its foreground finished launching (RestoreWakeBarrier::NoteForegroundLaunched)
        bool released{ false }; // the barrier let it go; it wakes its busy tabs on its next idle tick
    };

    /// <summary>What the barrier's release rule decides about its WAITING (not yet released) entries.</summary>
    enum class RestoreWakeRelease
    {
        None, // keep waiting (or nothing waits)
        AllLaunched, // every waiting window restored and launched its foreground: release them all
        CapExpired, // the oldest waiting entry has waited kRestoreWakeBarrierCapMs: release them all, ready or not
    };

    /// <summary>
    /// The barrier's release rule, pure: release every waiting entry when each of them has joined and launched its
    /// foreground, or when the oldest of them has waited <see cref="kRestoreWakeBarrierCapMs"/>.
    /// </summary>
    /// <param name="entries">The barrier's entries (released ones are ignored).</param>
    /// <param name="nowTickMs">The monotonic clock now.</param>
    /// <returns>The verdict for the waiting entries; None when no entry waits.</returns>
    /// <remarks>
    /// "Waiting" includes an entry that was only ANNOUNCED (its window was created to restore a saved record and has not
    /// re-homed its tabs yet): that is what keeps the first window to finish from waking its tabs while a second window
    /// is still restoring. Windows reopened later (the "Reopen Windows (N)" button) form a new round of their own, because
    /// the released entries of the earlier round no longer count.
    /// </remarks>
    inline RestoreWakeRelease RestoreWakeReleaseVerdict(const std::vector<RestoreWakeEntry>& entries, uint64_t nowTickMs) noexcept
    {
        bool anyWaiting = false;
        bool allReady = true;
        uint64_t oldest = UINT64_MAX;
        for (const auto& e : entries)
        {
            if (e.released)
            {
                continue;
            }
            anyWaiting = true;
            allReady = allReady && e.joined && e.foregroundLaunched;
            oldest = e.sinceTickMs < oldest ? e.sinceTickMs : oldest;
        }
        if (!anyWaiting)
        {
            return RestoreWakeRelease::None;
        }
        if (allReady)
        {
            return RestoreWakeRelease::AllLaunched;
        }
        if (nowTickMs >= oldest && nowTickMs - oldest >= kRestoreWakeBarrierCapMs)
        {
            return RestoreWakeRelease::CapExpired;
        }
        return RestoreWakeRelease::None;
    }

    /// <summary>The [restore-wake] log's words for a <see cref="RestoreWakeRelease"/> (number-free, like the others).</summary>
    /// <param name="release">The verdict.</param>
    /// <returns>A static, never-null text.</returns>
    inline const wchar_t* RestoreWakeReleaseText(RestoreWakeRelease release) noexcept
    {
        switch (release)
        {
        case RestoreWakeRelease::AllLaunched:
            return L"every reopened window launched its focused tab";
        case RestoreWakeRelease::CapExpired:
            return L"the barrier's cap expired";
        case RestoreWakeRelease::None:
        default:
            return L"not released";
        }
    }

    /// <summary>A snapshot of the barrier's population, for the [restore-wake] log.</summary>
    struct RestoreWakeBarrierCounts
    {
        size_t waiting{ 0 }; // entries not released yet
        size_t joined{ 0 }; // of those, the ones whose re-home finished
        size_t launched{ 0 }; // of those, the ones whose foreground finished launching
    };

    /// <summary>
    /// The process-wide join of the restore wake across windows: the windows that restore together each wake their busy
    /// tabs only once EVERY one of them has its foreground launched (or the cap expired). Thread-safe: every method takes
    /// the barrier's own mutex, never calls out while holding it, and touches no XAML — so a TerminalPage destructor
    /// running on a pool thread (the fire_and_forget teardown class) may call <see cref="Leave"/>.
    /// </summary>
    /// <remarks>
    /// The life of an entry: <see cref="Announce"/> when the window is created to restore a saved record (the Emperor
    /// dispatches every reopened window BEFORE the first one lays out, so all of them are known before any restore
    /// finishes) → <see cref="Join"/> when its re-home ended (a window restored without an announce — the one default
    /// window that front-pops a record — enters here) → <see cref="NoteForegroundLaunched"/> → released by the rule →
    /// <see cref="Leave"/> when the window woke its tabs, gave up, or closed. Every mutating call re-evaluates the rule, and
    /// so does <see cref="IsReleased"/>, which each window asks on its ticks; the time-based cap needs no thread of its own.
    /// Only std::mutex::lock can throw (std::system_error, which a healthy process never sees); callers that must not
    /// throw (a destructor, a scope guard) wrap the call.
    /// </remarks>
    class RestoreWakeBarrier
    {
    public:
        /// <summary>
        /// A window is being created to restore a saved record: make it a WAITING entry now, so a window that finishes
        /// earlier cannot wake its tabs before this one restored. Idempotent for a key that is already in.
        /// </summary>
        /// <param name="key">The window's key (its Emperor-assigned record id); an empty key is ignored.</param>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        /// <exception cref="std::bad_alloc">If the entry cannot be stored.</exception>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        void Announce(const std::wstring& key, uint64_t nowTickMs)
        {
            if (key.empty())
            {
                return;
            }
            std::lock_guard guard{ _mutex };
            if (_FindLocked(key) == nullptr)
            {
                _entries.push_back(RestoreWakeEntry{ key, nowTickMs, false, false, false });
            }
        }

        /// <summary>
        /// The window's re-home finished: mark its entry joined (adding it when it was never announced).
        /// </summary>
        /// <param name="key">The window's key; an empty key is ignored.</param>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        /// <exception cref="std::bad_alloc">If a new entry cannot be stored.</exception>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        void Join(const std::wstring& key, uint64_t nowTickMs)
        {
            if (key.empty())
            {
                return;
            }
            std::lock_guard guard{ _mutex };
            if (auto* const e = _FindLocked(key))
            {
                e->joined = true;
            }
            else
            {
                _entries.push_back(RestoreWakeEntry{ key, nowTickMs, true, false, false });
            }
            _EvaluateLocked(nowTickMs);
        }

        /// <summary>The window's foreground finished launching: mark its entry, then re-evaluate the release rule.</summary>
        /// <param name="key">The window's key; an unknown key is a no-op.</param>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        void NoteForegroundLaunched(const std::wstring& key, uint64_t nowTickMs)
        {
            std::lock_guard guard{ _mutex };
            if (auto* const e = _FindLocked(key))
            {
                e->foregroundLaunched = true;
            }
            _EvaluateLocked(nowTickMs);
        }

        /// <summary>
        /// Has the barrier released this window? Re-evaluates the release rule first (the cap is time-based).
        /// </summary>
        /// <param name="key">The window's key.</param>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        /// <returns>True when the window may wake its busy tabs (as far as other windows are concerned); TRUE for an unknown
        /// key too, because a window that is not in the barrier must never wait on it.</returns>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        bool IsReleased(const std::wstring& key, uint64_t nowTickMs)
        {
            std::lock_guard guard{ _mutex };
            _EvaluateLocked(nowTickMs);
            const auto* const e = _FindLocked(key);
            return e == nullptr || e->released;
        }

        /// <summary>
        /// The window is out: it woke its tabs, gave up, turned out to restore nothing, or is closing. Removes its entry and
        /// re-evaluates the rule, so the windows it was holding back are released on their next tick.
        /// </summary>
        /// <param name="key">The window's key; an unknown or empty key is a no-op.</param>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        void Leave(const std::wstring& key, uint64_t nowTickMs)
        {
            std::lock_guard guard{ _mutex };
            std::erase_if(_entries, [&](const RestoreWakeEntry& e) { return e.key == key; });
            _EvaluateLocked(nowTickMs);
        }

        /// <summary>How many windows wait, and how far they got — for the [restore-wake] log.</summary>
        /// <returns>The counts over the entries not released yet.</returns>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        RestoreWakeBarrierCounts Counts() const
        {
            std::lock_guard guard{ _mutex };
            RestoreWakeBarrierCounts c;
            for (const auto& e : _entries)
            {
                if (e.released)
                {
                    continue;
                }
                ++c.waiting;
                c.joined += e.joined ? 1 : 0;
                c.launched += e.foregroundLaunched ? 1 : 0;
            }
            return c;
        }

        /// <summary>Why the barrier last released its waiting windows — for the [restore-wake] log.</summary>
        /// <returns>None until a first release.</returns>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        RestoreWakeRelease LastRelease() const
        {
            std::lock_guard guard{ _mutex };
            return _lastRelease;
        }

    private:
        /// <summary>The entry with this key, or null. Caller holds the mutex.</summary>
        /// <param name="key">The window's key.</param>
        /// <returns>A pointer into _entries, valid until the next change of the vector.</returns>
        RestoreWakeEntry* _FindLocked(const std::wstring& key) noexcept
        {
            for (auto& e : _entries)
            {
                if (e.key == key)
                {
                    return &e;
                }
            }
            return nullptr;
        }

        /// <summary>The const twin of <see cref="_FindLocked(const std::wstring&amp;)"/>. Caller holds the mutex.</summary>
        /// <param name="key">The window's key.</param>
        /// <returns>A pointer into _entries, or null.</returns>
        const RestoreWakeEntry* _FindLocked(const std::wstring& key) const noexcept
        {
            for (const auto& e : _entries)
            {
                if (e.key == key)
                {
                    return &e;
                }
            }
            return nullptr;
        }

        /// <summary>
        /// Apply the release rule (RestoreWakeReleaseVerdict): on a release, mark EVERY waiting entry released — the ready
        /// ones and, on a cap expiry, the stragglers too (a straggler that joins later is then released already and waits
        /// only for its own foreground). Caller holds the mutex.
        /// </summary>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        void _EvaluateLocked(uint64_t nowTickMs) noexcept
        {
            const auto verdict = RestoreWakeReleaseVerdict(_entries, nowTickMs);
            if (verdict == RestoreWakeRelease::None)
            {
                return;
            }
            for (auto& e : _entries)
            {
                e.released = true;
            }
            _lastRelease = verdict;
        }

        mutable std::mutex _mutex; // guards every member below; never held while calling out
        std::vector<RestoreWakeEntry> _entries; // one per window in the barrier, in entry order; a handful at most
        RestoreWakeRelease _lastRelease{ RestoreWakeRelease::None }; // the reason of the latest release, for the log
    };

    /// <summary>
    /// The process's one barrier (every window of the process is in TerminalApp.dll, so one per module is one per
    /// process). Constructed on first use, never destroyed before the process exits.
    /// </summary>
    /// <returns>The shared barrier.</returns>
    /// <remarks>A header-inline function's static is per MODULE: only TerminalApp.dll may use this (the m5 harness tests
    /// local instances). Never use it from the WindowsTerminal EXE, which would get a second, unconnected barrier.</remarks>
    inline RestoreWakeBarrier& SharedRestoreWakeBarrier() noexcept
    {
        static RestoreWakeBarrier barrier; // a std::mutex + an empty vector: its construction cannot throw
        return barrier;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // HOW FAST, across windows: the pacer.
    // ------------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// The pacer's rule, pure: may one more wake happen now, given the instants of the wakes already taken? Yes when no
    /// wake happened in the last <see cref="kRestoreWakeSpacingMs"/> and fewer than <see cref="kRestoreWakeBatchSize"/>
    /// happened in the last <see cref="kRestoreWakeBatchPeriodMs"/> — a sliding window, which reproduces the "Activate All
    /// Tabs" drip's batches (0 / 0.5 / 1 / 1.5 s, then 10 s …) when the slots are taken as soon as they open.
    /// </summary>
    /// <param name="takenTickMs">The monotonic instants of earlier wakes (any order; ones older than the period are ignored).</param>
    /// <param name="nowTickMs">The monotonic clock now.</param>
    /// <returns>True when a wake may be taken now.</returns>
    inline bool RestoreWakePacerAllows(const std::vector<uint64_t>& takenTickMs, uint64_t nowTickMs) noexcept
    {
        size_t inPeriod = 0;
        for (const auto t : takenTickMs)
        {
            if (t > nowTickMs)
            {
                return false; // a slot stamped in the future (only a test can do that): be conservative
            }
            const uint64_t age = nowTickMs - t;
            if (age < kRestoreWakeSpacingMs)
            {
                return false; // too close to the previous wake
            }
            if (age < kRestoreWakeBatchPeriodMs)
            {
                ++inPeriod;
            }
        }
        return inPeriod < kRestoreWakeBatchSize;
    }

    /// <summary>
    /// The process-wide pace of the restore wake's wakes: every window's timer asks for a slot before it wakes one tab, so
    /// the windows the barrier released together take turns (first come, first served, on their 250 ms ticks) and the
    /// whole process starts at most <see cref="kRestoreWakeBatchSize"/> claudes per <see cref="kRestoreWakeBatchPeriodMs"/>.
    /// Thread-safe (its own mutex, no call-outs while held). Only the restore wake takes slots: a user's "Activate All
    /// Tabs" keeps its own per-window drip, so the two together can briefly exceed the pace — the restore wake's later
    /// candidates then read started and are skipped without a slot.
    /// </summary>
    class RestoreWakePacer
    {
    public:
        /// <summary>Take a wake slot when the rule allows one now (the caller then wakes exactly one tab).</summary>
        /// <param name="nowTickMs">The monotonic clock now.</param>
        /// <returns>True when the slot was taken; false to try again on a later tick.</returns>
        /// <exception cref="std::bad_alloc">If the slot list cannot grow (it never exceeds the batch size).</exception>
        /// <exception cref="std::system_error">If the mutex cannot be locked.</exception>
        bool TryTake(uint64_t nowTickMs)
        {
            std::lock_guard guard{ _mutex };
            // Forget the slots that left the period (and anything stamped in the future, which a test may feed).
            std::erase_if(_taken, [&](uint64_t t) { return t > nowTickMs || nowTickMs - t >= kRestoreWakeBatchPeriodMs; });
            if (!RestoreWakePacerAllows(_taken, nowTickMs))
            {
                return false;
            }
            _taken.push_back(nowTickMs);
            return true;
        }

    private:
        std::mutex _mutex; // guards _taken; never held while calling out
        std::vector<uint64_t> _taken; // the monotonic instants of the wakes in the current period (at most kRestoreWakeBatchSize)
    };

    /// <summary>
    /// The process's one pacer (per module, like SharedRestoreWakeBarrier — TerminalApp.dll only). Constructed on first use,
    /// never destroyed before the process exits.
    /// </summary>
    /// <returns>The shared pacer.</returns>
    inline RestoreWakePacer& SharedRestoreWakePacer() noexcept
    {
        static RestoreWakePacer pacer; // a std::mutex + an empty vector: its construction cannot throw
        return pacer;
    }
}
