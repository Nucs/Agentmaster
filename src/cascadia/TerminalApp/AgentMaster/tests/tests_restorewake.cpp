// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 harness — the RESTORE WAKE (doc/agentmaster/PERSISTENCE.md §14, AgentMaster/RestoreWake.h).
//
// After a restart only the tab each window opens to starts; the restore wake starts by itself every restored tab whose
// session was in the middle of something when Agentmaster went down, once the tab EVERY reopened window opens to has
// finished launching. What is pinned here (all of RestoreWake.h is pure, so no window and no clock is needed):
//   * RestoreWakeWantsSession — the user's rule "anything but idle / waiting-for-you / done": its truth table over every
//     state and flag, and END TO END through the registry the way the restore seams feed it (the sessions.json load and
//     the restored launch normalize Running / Error to Idle and keep the fact in midTurnAtRestore; a PINNED record's
//     Idle becomes the pin hold's Waiting; the scanner's cut-off synth keeps the flag; a started tab or a real turn ends
//     the want);
//   * the per-window FOREGROUND gate (RestoreWakeForegroundStep): no terminal → launched at once; a started terminal
//     without a Claude session → launched; a Claude terminal → launched only by a hook NEWER than the restore, or the
//     hookless settle; the 90 s cap; the latch (the barrier is told exactly once);
//   * the punctuality streak and the wake decision (RestoreWakeNoteTick / RestoreWakeDecideWake): never before the own
//     foreground launched or the barrier released, then the idle UI or the idle wait's cap;
//   * the barrier (RestoreWakeReleaseVerdict / RestoreWakeBarrier): announced-but-not-restored windows hold the others,
//     the release of every waiting window at once, Leave unblocking, the 5-minute cap (stragglers released too), rounds,
//     an unknown key never waits, idempotent Announce, Join without Announce, the counts and the last release's reason;
//   * the pacer (RestoreWakePacerAllows / RestoreWakePacer): the spacing, the batch per period, the drip's exact timeline
//     on 250 ms ticks, and four windows released together sharing ONE pace (4 wakes in the first 1.6 s, not 16).

#include "m5_tests.h"

#include "../HeldTabRefs.h" // TestHeldTabRefs: PERSISTENCE.md §16 — the saved tabs a restore could not bring back
// (Engine.h stays out of this unit: its `Agentmaster::Msg` namespace collides with the harness's `Msg` hook-message builder
// used above. The engine's held-session set is tested beside the other engine seams: tests_persistence.cpp,
// TestEngineHeldSessions.)
#include "../RestoreWake.h"

namespace
{
    /// <summary>A restored tab's record as the restore leaves it: live, dormant, ours, and flagged mid-turn.</summary>
    /// <param name="state">The state the record holds now.</param>
    /// <returns>The record (id "rw").</returns>
    SessionInfo RestoredDormant(SessionState state)
    {
        SessionInfo s = MakeSession(L"rw", state);
        s.live = true;
        s.started = false;
        s.external = false;
        s.midTurnAtRestore = true;
        return s;
    }

    /// <summary>
    /// Feed a PERSISTED record through the two restore seams the app applies to it — the sessions.json load (archived)
    /// and the restored launch (live, dormant) — exactly as TerminalPage does (RestoredMidTurnFlag BEFORE
    /// RestoredSessionState), then return the registry's record.
    /// </summary>
    /// <param name="reg">The registry to load into.</param>
    /// <param name="id">The session id.</param>
    /// <param name="persisted">The state sessions.json holds.</param>
    /// <param name="pinned">Whether the session is pinned (MESSAGING.md §5.3: the pin hold applies to a live Idle).</param>
    /// <returns>The record after the restored launch's Upsert.</returns>
    SessionInfo ThroughTheRestoreSeams(SessionRegistry& reg, const std::wstring& id, SessionState persisted, bool pinned)
    {
        SessionInfo s = MakeSession(id, persisted);
        s.pinned = pinned;
        // The sessions.json load (_RestoreClaudeSessions): archived, flag first, then the normalization.
        s.live = false;
        s.midTurnAtRestore = RestoredMidTurnFlag(s.state, s.midTurnAtRestore);
        s.state = RestoredSessionState(s.state);
        reg.Upsert(s);
        // The restored launch (_LaunchClaudeSession with `restored`): the same two calls on the loaded copy, then live.
        SessionInfo r = *reg.Get(id);
        r.midTurnAtRestore = RestoredMidTurnFlag(r.state, r.midTurnAtRestore);
        r.state = RestoredSessionState(r.state);
        r.live = true;
        r.started = false;
        reg.Upsert(r);
        return *reg.GetWithoutQueue(id);
    }

    void TestRestoreWakeWantsSession()
    {
        // ---- the truth table ----
        CHECK(RestoreWakeWantsSession(RestoredDormant(SessionState::Idle)), "wants: a dormant Idle that was mid-turn (a normalized Running / Error) is woken");
        CHECK(RestoreWakeWantsSession(RestoredDormant(SessionState::NeedsApproval)), "wants: a dormant NeedsApproval that was mid-turn is woken (its question died with its process)");
        CHECK(RestoreWakeWantsSession(RestoredDormant(SessionState::Error)), "wants: a dormant Error (the scanner's cut-off / API-error verdict) is woken");
        CHECK(!RestoreWakeWantsSession(RestoredDormant(SessionState::WaitingForInput)), "wants: an unpinned Waiting-for-you is a proved turn end - not woken");
        {
            auto s = RestoredDormant(SessionState::WaitingForInput);
            s.pinned = true;
            CHECK(RestoreWakeWantsSession(s), "wants: a PINNED Waiting-for-you with the flag is the pin hold's normalized Idle - woken");
        }
        CHECK(!RestoreWakeWantsSession(RestoredDormant(SessionState::Done)), "wants: Done is never woken");
        CHECK(!RestoreWakeWantsSession(RestoredDormant(SessionState::Running)), "wants: Running means a live process runs the turn somewhere - starting this tab would add a second writer");
        {
            auto s = RestoredDormant(SessionState::Idle);
            s.midTurnAtRestore = false;
            CHECK(!RestoreWakeWantsSession(s), "wants: an Idle that was at rest at the restart is not woken (the user's rule is the state AT THE RESTART)");
            s.state = SessionState::Error;
            CHECK(!RestoreWakeWantsSession(s), "wants: an Error that was NOT busy at the restart (a dismissed error the scanner re-derived) is not woken");
        }
        {
            auto s = RestoredDormant(SessionState::Idle);
            s.started = true;
            CHECK(!RestoreWakeWantsSession(s), "wants: a started tab (the focused one, or one the user opened) is running already");
        }
        {
            auto s = RestoredDormant(SessionState::Idle);
            s.live = false;
            CHECK(!RestoreWakeWantsSession(s), "wants: a closed (archived) session has no tab to wake");
        }
        {
            auto s = RestoredDormant(SessionState::Idle);
            s.external = true;
            CHECK(!RestoreWakeWantsSession(s), "wants: an external (adopted) session is not ours to start");
        }

        // ---- END TO END: the restore seams + the registry ----
        SessionRegistry reg;
        CHECK(RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-run", SessionState::Running, false)), "e2e: persisted Running -> Idle + the flag -> woken");
        CHECK(RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-na", SessionState::NeedsApproval, false)), "e2e: persisted NeedsApproval -> kept + the flag -> woken");
        CHECK(RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-err", SessionState::Error, false)), "e2e: persisted Error -> Idle + the flag -> woken");
        CHECK(!RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-idle", SessionState::Idle, false)), "e2e: persisted Idle -> not woken");
        CHECK(!RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-wait", SessionState::WaitingForInput, false)), "e2e: persisted Waiting-for-you -> not woken");
        CHECK(!RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-done", SessionState::Done, false)), "e2e: persisted Done -> not woken");
        {
            const auto pinnedRun = ThroughTheRestoreSeams(reg, L"rw-pin-run", SessionState::Running, true);
            CHECK(pinnedRun.state == SessionState::WaitingForInput, "e2e: (the pin hold turned the pinned record's normalized Idle into Waiting)");
            CHECK(RestoreWakeWantsSession(pinnedRun), "e2e: a PINNED persisted Running is still woken through the pin hold's Waiting");
            CHECK(!RestoreWakeWantsSession(ThroughTheRestoreSeams(reg, L"rw-pin-wait", SessionState::WaitingForInput, true)), "e2e: a pinned persisted Waiting-for-you (no flag) is not woken");
        }

        // The scanner's cut-off verdict keeps the want; a started tab and a real turn end it.
        HookMessage cut = Msg(L"rw-run", HookEvent::Notification);
        cut.turnCutOff = true;
        cut.ts = NowMsTest();
        cut.errorMessage = std::wstring{ kTurnCutOffErrorMessage };
        reg.OnHookEvent(cut);
        auto r = reg.GetWithoutQueue(L"rw-run");
        CHECK(r && r->state == SessionState::Error && RestoreWakeWantsSession(*r), "e2e: the scanner's cut-off synth (Error) keeps the flag - still woken");
        reg.SetStarted(L"rw-run", true);
        r = reg.GetWithoutQueue(L"rw-run");
        CHECK(r && !RestoreWakeWantsSession(*r), "e2e: once its tab started (opened by the user, or the wake itself) it is not woken again");
        reg.OnHookEvent(UPS(L"rw-err", L"continue"));
        r = reg.GetWithoutQueue(L"rw-err");
        CHECK(r && r->state == SessionState::Running && !r->midTurnAtRestore && !RestoreWakeWantsSession(*r), "e2e: a real turn (Running) spends the flag - no wake");
    }

    void TestRestoreWakeForegroundGate()
    {
        const int64_t restoredUnix = 1'700'000'000'000;
        const uint64_t restoredTick = 50'000;
        const auto fresh = [&]() {
            RestoreWakeWindowState st;
            st.restoredAtUnixMs = restoredUnix;
            st.restoredAtTickMs = restoredTick;
            st.lastTickMs = restoredTick;
            return st;
        };

        {
            auto st = fresh();
            const RestoreWakeForeground none{};
            const auto why = RestoreWakeForegroundStep(st, none, restoredTick + 250);
            CHECK(why && *why == RestoreWakeForegroundReason::NoTerminal && st.foregroundLaunched, "gate: no terminal (the Agent Manager page) -> launched at once");
            CHECK(!RestoreWakeForegroundStep(st, none, restoredTick + 500), "gate: the latch answers once - the barrier is told exactly once");
        }
        {
            auto st = fresh();
            RestoreWakeForeground shell{};
            shell.terminal = true;
            CHECK(!RestoreWakeForegroundStep(st, shell, restoredTick + 250), "gate: a terminal not started yet -> still launching");
            shell.started = true;
            const auto why = RestoreWakeForegroundStep(st, shell, restoredTick + 500);
            CHECK(why && *why == RestoreWakeForegroundReason::TerminalStarted, "gate: a started terminal without a Claude session (a shell, a Codex) -> launched");
        }
        {
            auto st = fresh();
            RestoreWakeForeground claude{};
            claude.terminal = true;
            claude.claudeSession = true;
            claude.started = true;
            claude.lastHookUnixMs = restoredUnix - 60'000; // its earlier life in this run (a window closed and reopened)
            CHECK(!RestoreWakeForegroundStep(st, claude, restoredTick + 1'000), "gate: a hook OLDER than the restore proves nothing about the claude this restore started");
            CHECK(st.startedSeenTickMs == restoredTick + 1'000, "gate: the first tick that saw it started is remembered (the hookless settle counts from it)");
            claude.lastHookUnixMs = restoredUnix + 2'500; // its SessionStart
            const auto why = RestoreWakeForegroundStep(st, claude, restoredTick + 3'000);
            CHECK(why && *why == RestoreWakeForegroundReason::ClaudeUp, "gate: a hook newer than the restore -> the claude is up");
        }
        {
            auto st = fresh();
            RestoreWakeForeground claude{};
            claude.terminal = true;
            claude.claudeSession = true;
            claude.started = true; // no hook ever (a re-forked fork's echo is dropped; a broken forwarder)
            CHECK(!RestoreWakeForegroundStep(st, claude, restoredTick + 2'000), "gate: a silent claude just started -> still launching");
            CHECK(!RestoreWakeForegroundStep(st, claude, restoredTick + 2'000 + kRestoreWakeHooklessSettleMs - 1), "gate: one ms short of the hookless settle -> still launching");
            const auto why = RestoreWakeForegroundStep(st, claude, restoredTick + 2'000 + kRestoreWakeHooklessSettleMs);
            CHECK(why && *why == RestoreWakeForegroundReason::ClaudeSilent, "gate: the hookless settle passed -> launched anyway");
        }
        {
            auto st = fresh();
            RestoreWakeForeground claude{};
            claude.terminal = true;
            claude.claudeSession = true; // never starts (the user switched away before its first layout)
            CHECK(!RestoreWakeForegroundStep(st, claude, restoredTick + kRestoreWakeForegroundCapMs - 1), "gate: a foreground that never starts holds until the cap");
            const auto why = RestoreWakeForegroundStep(st, claude, restoredTick + kRestoreWakeForegroundCapMs);
            CHECK(why && *why == RestoreWakeForegroundReason::TimedOut, "gate: the 90 s cap -> launched, so it cannot hold every window forever");
        }
        {
            auto st = fresh();
            RestoreWakeForeground claude{};
            claude.terminal = true;
            claude.claudeSession = true;
            claude.started = true;
            claude.lastHookUnixMs = restoredUnix; // a hook at the very instant of the restore counts
            const auto why = RestoreWakeForegroundStep(st, claude, restoredTick + 10);
            CHECK(why && *why == RestoreWakeForegroundReason::ClaudeUp, "gate: a hook stamped AT the restore instant counts (>=)");
        }
        // The texts never name a number, so a changed cap cannot leave a lying log line.
        for (const auto reason : { RestoreWakeForegroundReason::NoTerminal, RestoreWakeForegroundReason::TerminalStarted, RestoreWakeForegroundReason::ClaudeUp,
                                   RestoreWakeForegroundReason::ClaudeSilent, RestoreWakeForegroundReason::TimedOut })
        {
            const std::wstring text{ RestoreWakeForegroundReasonText(reason) };
            CHECK(!text.empty() && text.find_first_of(L"0123456789") == std::wstring::npos, "gate: each reason has a number-free text");
        }
    }

    void TestRestoreWakeWakeDecision()
    {
        RestoreWakeWindowState st;
        st.restoredAtTickMs = 10'000;
        st.lastTickMs = 10'000;
        uint64_t now = 10'000;
        // Punctual ticks build the streak; a late one resets it.
        for (int i = 0; i < 3; ++i)
        {
            now += kRestoreWakeTickMs;
            RestoreWakeNoteTick(st, now);
        }
        CHECK(st.punctualTicks == 3, "idle: three on-time ticks -> a streak of 3");
        now += kRestoreWakePunctualGapMs;
        RestoreWakeNoteTick(st, now);
        CHECK(st.punctualTicks == 0, "idle: a tick at the punctuality gap or later resets the streak (the UI thread was busy)");
        RestoreWakeNoteTick(st, now - 1);
        CHECK(st.punctualTicks == 0, "idle: a clock that went backwards counts as late");

        // Never before the own foreground launched, nor before the barrier released.
        for (int i = 0; i < kRestoreWakeIdleTicks; ++i)
        {
            now += kRestoreWakeTickMs;
            RestoreWakeNoteTick(st, now);
        }
        CHECK(!RestoreWakeDecideWake(st, now, true), "wake: never before this window's own foreground launched (even when released)");
        st.foregroundLaunched = true;
        CHECK(!RestoreWakeDecideWake(st, now, false), "wake: never before the barrier released (another window still launches its focused tab)");
        CHECK(st.releasedSeenTickMs == 0, "wake: the idle wait does not start before the release");
        const auto idle = RestoreWakeDecideWake(st, now, true);
        CHECK(idle && *idle == RestoreWakeWakeReason::UiIdle, "wake: released + an idle UI -> wake now");

        // A busy UI holds it, but only up to the idle wait's cap after the release was first seen.
        RestoreWakeWindowState busy;
        busy.foregroundLaunched = true;
        busy.lastTickMs = 0;
        uint64_t t = 100'000;
        RestoreWakeNoteTick(busy, t); // late (gap from 0): streak 0
        CHECK(!RestoreWakeDecideWake(busy, t, true) && busy.releasedSeenTickMs == t, "wake: released but busy -> wait; the release instant is stamped");
        CHECK(!RestoreWakeDecideWake(busy, t + kRestoreWakeIdleWaitCapMs - 1, true), "wake: busy, one ms short of the idle wait's cap -> wait");
        const auto forced = RestoreWakeDecideWake(busy, t + kRestoreWakeIdleWaitCapMs, true);
        CHECK(forced && *forced == RestoreWakeWakeReason::UiBusyTimedOut, "wake: the idle wait's cap -> wake anyway (the drip still paces it)");
        const std::wstring busyText{ RestoreWakeWakeReasonText(RestoreWakeWakeReason::UiBusyTimedOut) };
        CHECK(busyText.find_first_of(L"0123456789") == std::wstring::npos, "wake: the reason texts are number-free");
    }

    void TestRestoreWakeBarrier()
    {
        // ---- the pure verdict ----
        {
            std::vector<RestoreWakeEntry> none;
            CHECK(RestoreWakeReleaseVerdict(none, 1'000) == RestoreWakeRelease::None, "verdict: nothing waits -> None");
            std::vector<RestoreWakeEntry> one{ RestoreWakeEntry{ L"a", 1'000, true, true, false } };
            CHECK(RestoreWakeReleaseVerdict(one, 1'000) == RestoreWakeRelease::AllLaunched, "verdict: one ready window -> released");
            one[0].foregroundLaunched = false;
            CHECK(RestoreWakeReleaseVerdict(one, 1'000) == RestoreWakeRelease::None, "verdict: its foreground still launching -> wait");
            CHECK(RestoreWakeReleaseVerdict(one, 1'000 + kRestoreWakeBarrierCapMs) == RestoreWakeRelease::CapExpired, "verdict: the oldest waited the cap -> released anyway");
            one[0].released = true;
            CHECK(RestoreWakeReleaseVerdict(one, 1'000 + kRestoreWakeBarrierCapMs) == RestoreWakeRelease::None, "verdict: released entries no longer count");
            CHECK(RestoreWakeReleaseVerdict(one, 0) == RestoreWakeRelease::None, "verdict: (a clock before the entry is no cap)");
        }

        // ---- two windows reopened together ----
        {
            RestoreWakeBarrier b;
            b.Announce(L"A", 1'000);
            b.Announce(L"B", 1'000);
            b.Announce(L"A", 1'500); // idempotent
            CHECK(b.Counts().waiting == 2, "barrier: two announced windows wait (one entry each, Announce is idempotent)");
            b.Join(L"A", 2'000);
            b.NoteForegroundLaunched(L"A", 3'000);
            CHECK(!b.IsReleased(L"A", 3'000), "barrier: A is ready but B has not even restored -> A waits (B's focused tab comes first)");
            b.Join(L"B", 4'000);
            CHECK(!b.IsReleased(L"A", 4'000) && !b.IsReleased(L"B", 4'000), "barrier: B restored, its focused tab still launching -> both wait");
            const auto c = b.Counts();
            CHECK(c.waiting == 2 && c.joined == 2 && c.launched == 1, "barrier: the counts say 2 waiting, 2 restored, 1 launched");
            b.NoteForegroundLaunched(L"B", 5'000);
            CHECK(b.IsReleased(L"A", 5'000) && b.IsReleased(L"B", 5'000), "barrier: the last foreground launched -> EVERY window released at once");
            CHECK(b.LastRelease() == RestoreWakeRelease::AllLaunched, "barrier: the release names its reason (all launched)");
            CHECK(b.Counts().waiting == 0, "barrier: nothing waits after the release");
            b.Leave(L"A", 6'000);
            b.Leave(L"B", 6'000);
            b.Leave(L"B", 6'000); // a second leave is a no-op
            CHECK(b.IsReleased(L"A", 6'000), "barrier: an unknown key (it left) never waits");
        }

        // ---- a window that turns out to restore nothing leaves, and unblocks the rest ----
        {
            RestoreWakeBarrier b;
            b.Announce(L"A", 0);
            b.Announce(L"Gone", 0);
            b.Join(L"A", 100);
            b.NoteForegroundLaunched(L"A", 200);
            CHECK(!b.IsReleased(L"A", 200), "barrier: an announced window that has not restored holds the others");
            b.Leave(L"Gone", 300);
            CHECK(b.IsReleased(L"A", 300), "barrier: once it leaves (it restored nothing / closed), A is released");
        }

        // ---- the cap: a straggler that never restores ----
        {
            RestoreWakeBarrier b;
            b.Announce(L"A", 1'000);
            b.Announce(L"Stuck", 1'000);
            b.Join(L"A", 1'200);
            b.NoteForegroundLaunched(L"A", 1'300);
            CHECK(!b.IsReleased(L"A", 1'000 + kRestoreWakeBarrierCapMs - 1), "cap: one ms short of the cap -> A still waits for the straggler");
            CHECK(b.IsReleased(L"A", 1'000 + kRestoreWakeBarrierCapMs), "cap: the cap after the OLDEST waiting entry -> A released");
            CHECK(b.LastRelease() == RestoreWakeRelease::CapExpired, "cap: the release names its reason (the cap)");
            b.Join(L"Stuck", 1'000 + kRestoreWakeBarrierCapMs + 50);
            CHECK(b.IsReleased(L"Stuck", 1'000 + kRestoreWakeBarrierCapMs + 50), "cap: the straggler was released too - when it restores at last it waits only for its own focused tab");
        }

        // ---- rounds: a window reopened later does not wait for the earlier round, nor the earlier round for it ----
        {
            RestoreWakeBarrier b;
            b.Join(L"A", 0); // the one default window that front-pops a record: Join without Announce
            CHECK(b.Counts().waiting == 1 && b.Counts().joined == 1, "rounds: Join without Announce enters the barrier");
            b.NoteForegroundLaunched(L"A", 10);
            CHECK(b.IsReleased(L"A", 10), "rounds: a lone window is released once its own foreground launched");
            b.Announce(L"C", 20'000); // the "Reopen Windows (N)" button, much later; A has not left yet (waiting for an idle UI)
            CHECK(b.IsReleased(L"A", 20'000), "rounds: A stays released while the new round waits");
            CHECK(!b.IsReleased(L"C", 20'000), "rounds: C waits for its own restore");
            b.Join(L"C", 21'000);
            b.NoteForegroundLaunched(L"C", 22'000);
            CHECK(b.IsReleased(L"C", 22'000), "rounds: C is released by its own round");
        }

        // ---- empty keys are ignored ----
        {
            RestoreWakeBarrier b;
            b.Announce(L"", 0);
            b.Join(L"", 0);
            CHECK(b.Counts().waiting == 0, "barrier: an empty key never enters");
            CHECK(std::wstring{ RestoreWakeReleaseText(RestoreWakeRelease::CapExpired) }.find_first_of(L"0123456789") == std::wstring::npos, "barrier: the release texts are number-free");
        }

        // The process-wide instance is one object (the app's every window shares it).
        CHECK(&SharedRestoreWakeBarrier() == &SharedRestoreWakeBarrier(), "barrier: SharedRestoreWakeBarrier is one instance");
    }

    void TestRestoreWakePacer()
    {
        // ---- the pure rule ----
        CHECK(RestoreWakePacerAllows({}, 1'000), "pacer: no wake yet -> allowed");
        CHECK(!RestoreWakePacerAllows({ 1'000 }, 1'000 + kRestoreWakeSpacingMs - 1), "pacer: closer than the spacing to the previous wake -> not yet");
        CHECK(RestoreWakePacerAllows({ 1'000 }, 1'000 + kRestoreWakeSpacingMs), "pacer: the spacing passed -> allowed");
        const std::vector<uint64_t> fullBatch{ 0, 500, 1'000, 1'500 };
        CHECK(!RestoreWakePacerAllows(fullBatch, 2'000), "pacer: a full batch in the period -> not yet");
        CHECK(!RestoreWakePacerAllows(fullBatch, kRestoreWakeBatchPeriodMs - 1), "pacer: one ms before the first slot leaves the period -> not yet");
        CHECK(RestoreWakePacerAllows(fullBatch, kRestoreWakeBatchPeriodMs), "pacer: the first slot left the period -> allowed");
        CHECK(!RestoreWakePacerAllows({ 5'000 }, 4'000), "pacer: a slot stamped in the future is treated conservatively");

        // ---- the process-wide pacer reproduces the drip's timeline when slots are taken as soon as they open ----
        {
            RestoreWakePacer pacer;
            std::vector<uint64_t> taken;
            for (uint64_t t = 0; t <= 25'000 && taken.size() < 10; t += kRestoreWakeTickMs)
            {
                if (pacer.TryTake(t))
                {
                    taken.push_back(t);
                }
            }
            const std::vector<uint64_t> expected{ 0, 500, 1'000, 1'500, 10'000, 10'500, 11'000, 11'500, 20'000, 20'500 };
            CHECK(taken == expected, "pacer: 0 / 0.5 / 1 / 1.5 s, then 10 s ... - the Activate All drip's batches, on 250 ms ticks");
        }

        // ---- several windows ticking at once share ONE pace ----
        {
            RestoreWakePacer pacer;
            int granted = 0;
            for (uint64_t t = 0; t < 1'600; t += kRestoreWakeTickMs)
            {
                for (int window = 0; window < 4; ++window) // four windows released together, each ticking every 250 ms
                {
                    if (pacer.TryTake(t))
                    {
                        ++granted;
                    }
                }
            }
            CHECK(granted == static_cast<int>(kRestoreWakeBatchSize), "pacer: four windows released together still start only 4 claudes in the first 1.6 s, not 16");
        }
        CHECK(&SharedRestoreWakePacer() == &SharedRestoreWakePacer(), "pacer: SharedRestoreWakePacer is one instance");
    }
}

void TestRestoreWake()
{
    TestRestoreWakeWantsSession();
    TestRestoreWakeForegroundGate();
    TestRestoreWakeWakeDecision();
    TestRestoreWakeBarrier();
    TestRestoreWakePacer();
}

// ===== HELD TAB REFS (PERSISTENCE.md §16) ===============================================================================
//
// The owner's question of 2026-10-28 ("is there ANY other chance we will EVER override accidentally those files") found that
// a window record is rebuilt from the live strip at every save, so a saved tab a reopened window could not bring back —
// Claude Code not found at that start, a preset whose key is missing, a pane that failed — vanished from the workspace at the
// next save, a window that brought back nothing deleted its record as "Manager-only", and the Telegram bot deleted those
// sessions' topics. These checks pin the pure half (HeldTabRefs.h) and the engine's held set the bot reads.

void TestHeldTabRefs()
{
    std::wprintf(L"Held tab refs (PERSISTENCE.md 16: a saved tab a restore could not bring back stays in the record):\n");
    using Agentmaster::HeldTabRef;
    using Agentmaster::RestoreRefOutcome;
    using Agentmaster::TabEntry;
    using Agentmaster::TabKind;

    // ---- which refs a restore holds ----
    {
        using Agentmaster::ShouldHoldRestoreRef;
        CHECK(ShouldHoldRestoreRef(RestoreRefOutcome::LaunchRefused, true) && ShouldHoldRestoreRef(RestoreRefOutcome::LaunchRefused, false),
              "hold: a launch that made no tab (Claude Code not found, a preset that cannot launch, a pane failure) is held, fleet loaded or not");
        CHECK(ShouldHoldRestoreRef(RestoreRefOutcome::ShellUnreadable, true), "hold: a shell tab whose saved actions do not parse is held");
        CHECK(ShouldHoldRestoreRef(RestoreRefOutcome::UnknownSession, false), "hold: an unknown session is held when sessions.json could not be used at this start");
        CHECK(!ShouldHoldRestoreRef(RestoreRefOutcome::UnknownSession, true), "hold: ...and let go when it could (then the session is really gone)");
        CHECK(!ShouldHoldRestoreRef(RestoreRefOutcome::Restored, false) && !ShouldHoldRestoreRef(RestoreRefOutcome::HostedHere, false) &&
                  !ShouldHoldRestoreRef(RestoreRefOutcome::LiveElsewhere, false) && !ShouldHoldRestoreRef(RestoreRefOutcome::ShellEmpty, false),
              "hold: a restored tab, a duplicate ref, a session another window hosts and an empty shell ref are never held");
        bool words = true;
        for (const auto o : { RestoreRefOutcome::Restored, RestoreRefOutcome::HostedHere, RestoreRefOutcome::LiveElsewhere, RestoreRefOutcome::UnknownSession,
                              RestoreRefOutcome::LaunchRefused, RestoreRefOutcome::ShellUnreadable, RestoreRefOutcome::ShellEmpty })
        {
            const wchar_t* w = Agentmaster::RestoreRefOutcomeWord(o);
            words = words && w && *w;
        }
        CHECK(words, "hold: every outcome has a log word");
    }

    // The tab refs the cases below build: a Claude tab, a Codex tab, a shell tab (its actions stand for its blob).
    const auto claude = [](const wchar_t* id) {
        TabEntry t;
        t.kind = TabKind::Claude;
        t.sessionId = id;
        return t;
    };
    const auto codex = [](const wchar_t* id) {
        TabEntry t;
        t.kind = TabKind::Codex;
        t.sessionId = id;
        return t;
    };
    const auto shell = [](const wchar_t* actions) {
        TabEntry t;
        t.kind = TabKind::Other;
        t.actionsJson = actions;
        return t;
    };
    // A record's tabs as one line: "A,B,sh:S1," — the order the next start re-homes them in.
    const auto line = [](const std::vector<TabEntry>& tabs) {
        std::wstring s;
        for (const auto& t : tabs)
        {
            s += (t.kind == TabKind::Other ? (L"sh:" + t.actionsJson) : t.sessionId) + L",";
        }
        return s;
    };

    // ---- a save puts the held refs back ----
    {
        // The record was [A, B, S1, C, D]. The restore made A and C, replayed S1 (shells land after the managed tabs) and held
        // B (record index 1) and D (index 4).
        std::vector<HeldTabRef> held{ HeldTabRef{ 1, claude(L"B"), RestoreRefOutcome::LaunchRefused }, HeldTabRef{ 4, codex(L"D"), RestoreRefOutcome::LaunchRefused } };
        std::vector<TabEntry> captured{ claude(L"A"), claude(L"C"), shell(L"S1") };
        int selected = 2; // the focused shell, S1
        const bool changed = Agentmaster::MergeHeldTabRefs(captured, held, {}, selected);
        CHECK(!changed && held.size() == 2, "merge: nothing resolved -> the held set is unchanged (nothing to re-publish)");
        CHECK(line(captured) == L"A,B,C,sh:S1,D,", "merge: each held ref goes back at its old index, clamped to the strip's end");
        CHECK(selected == 3 && captured[3].actionsJson == L"S1", "merge: the focused shell's index follows the refs inserted before it");
        // The next save rebuilds the capture from the strip again: the held refs ride EVERY save, not just the first.
        std::vector<TabEntry> next{ claude(L"A"), claude(L"C"), shell(L"S1") };
        int unset = -1;
        Agentmaster::MergeHeldTabRefs(next, held, {}, unset);
        CHECK(line(next) == L"A,B,C,sh:S1,D," && unset == -1, "merge: every save puts them back; an unset focus stays unset");
    }
    {
        // B came back (resumed here, or another window hosts it now): it is let go for good; D and the unreadable shell stay.
        std::vector<HeldTabRef> held{ HeldTabRef{ 1, claude(L"B"), RestoreRefOutcome::LaunchRefused }, HeldTabRef{ 4, codex(L"D"), RestoreRefOutcome::LaunchRefused },
                                      HeldTabRef{ 5, shell(L"S2"), RestoreRefOutcome::ShellUnreadable } };
        std::vector<TabEntry> captured{ claude(L"A"), claude(L"B"), claude(L"C") };
        int selected = -1;
        const bool changed = Agentmaster::MergeHeldTabRefs(captured, held, [](const TabEntry& t) { return t.sessionId == L"B"; }, selected);
        CHECK(changed && held.size() == 2 && held[0].entry.sessionId == L"D" && held[1].entry.actionsJson == L"S2",
              "merge: a held ref whose tab exists again leaves the held set (reported, so the engine is told)");
        CHECK(line(captured) == L"A,B,C,D,sh:S2,", "merge: ...and is not inserted a second time beside its live tab; the others still are");
    }
    {
        // A start without Claude Code: no Claude tab could be made, so the strip is empty and the record IS the held refs.
        std::vector<HeldTabRef> held{ HeldTabRef{ 0, claude(L"A"), RestoreRefOutcome::LaunchRefused }, HeldTabRef{ 1, claude(L"B"), RestoreRefOutcome::LaunchRefused },
                                      HeldTabRef{ 2, codex(L"C"), RestoreRefOutcome::LaunchRefused } };
        std::vector<TabEntry> captured;
        int selected = -1;
        Agentmaster::MergeHeldTabRefs(captured, held, {}, selected);
        CHECK(line(captured) == L"A,B,C,", "merge: a window whose restore made no tab saves every held ref, in record order - never an empty record");
    }

    // ---- the session ids the engine publishes for the bot ----
    {
        const std::vector<HeldTabRef> held{ HeldTabRef{ 0, claude(L"A"), RestoreRefOutcome::LaunchRefused }, HeldTabRef{ 1, shell(L"S"), RestoreRefOutcome::ShellUnreadable },
                                            HeldTabRef{ 2, codex(L"D"), RestoreRefOutcome::LaunchRefused }, HeldTabRef{ 3, claude(L"A"), RestoreRefOutcome::LaunchRefused },
                                            HeldTabRef{ 4, claude(L""), RestoreRefOutcome::LaunchRefused } };
        CHECK((Agentmaster::HeldSessionIds(held) == std::vector<std::wstring>{ L"A", L"D" }),
              "ids: the Claude and Codex sessions, each once, in hold order; shells and empty ids left out");
    }
}
