// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The 38 tests + 2 benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all 40 test entry-point declarations
// ★ tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: state tests. Shared CHECK/fixtures/decls
// live in m5_tests.h; the runner (m5_tests.cpp) calls each entry point. See run-m5-tests.bat.
#include "m5_tests.h"

void TestStateMachine()
{
    std::wprintf(L"State machine (Correctness Rule #1):\n");
    CHECK(NextSessionState(SessionState::Idle, Msg(L"a", HookEvent::SessionStart)) == SessionState::Idle, "SessionStart -> Idle");

    // Agentmaster (the "Move to Waiting-for-you then activate the inactive tab resets to Idle/Done" bug):
    // a SessionStart (fresh launch / --resume / a re-homed or background tab's LAZY claude start on FIRST
    // activation / /clear / /compact) must PRESERVE an at-rest "needs-you" triage rather than clobber it
    // to Idle. A resume/lazy-start reloads the conversation but does NOT continue the turn, so the tail
    // still says Waiting/NeedsApproval — and a manual "Move to Waiting-for-you"/"Mark Unread" is an
    // explicit cue the user set. A genuine new turn (UserPromptSubmit -> Running) still clears it.
    CHECK(NextSessionState(SessionState::WaitingForInput, Msg(L"a", HookEvent::SessionStart)) == SessionState::WaitingForInput, "SessionStart PRESERVES WaitingForInput (a Move-to-Waiting / restored card survives activating the tab)");
    CHECK(NextSessionState(SessionState::NeedsApproval, Msg(L"a", HookEvent::SessionStart)) == SessionState::NeedsApproval, "SessionStart PRESERVES NeedsApproval (survives a lazy-start/resume)");
    CHECK(NextSessionState(SessionState::Running, Msg(L"a", HookEvent::SessionStart)) == SessionState::Idle, "SessionStart from Running -> Idle (a /clear or restart mid-run resets)");
    CHECK(NextSessionState(SessionState::Error, Msg(L"a", HookEvent::SessionStart)) == SessionState::Idle, "SessionStart from Error -> Idle (restart clears the error; scanner re-derives if still active)");
    CHECK(NextSessionState(SessionState::Done, Msg(L"a", HookEvent::SessionStart)) == SessionState::Idle, "SessionStart from Done -> Idle (resumed => alive again)");
    // Invariant: the SessionStart-preserve set MUST equal RestoredSessionState's preserved set — a
    // lazy-start SessionStart IS a resume, so the two mappings must never drift (else the restore-seeded
    // triage that RestoredSessionState kept would be undone on the first tab visit — the original bug).
    for (auto st : { SessionState::Idle, SessionState::Running, SessionState::WaitingForInput, SessionState::NeedsApproval, SessionState::Error, SessionState::Done })
    {
        CHECK(NextSessionState(st, Msg(L"a", HookEvent::SessionStart)) == RestoredSessionState(st), "SessionStart mapping == RestoredSessionState mapping (a lazy-start SessionStart IS a resume)");
    }

    // Agentmaster (HOOKS.md "Compaction"): a SessionStart(source="compact") is NOT a (re)start — the SAME
    // conversation continues, and after an AUTO-compaction the very same turn carries on — so a Running
    // turn stays Running instead of the Running->Idle blip every auto-compaction used to cause (proven
    // live: NumSharp a3d777bf, `[SessionStart] ... state=0` at each of 10 boundaries). Every other state
    // maps exactly as a plain SessionStart (the invariant above stays about resumes, not compactions).
    {
        HookMessage compactStart = Msg(L"a", HookEvent::SessionStart);
        compactStart.source = L"compact";
        CHECK(NextSessionState(SessionState::Running, compactStart) == SessionState::Running, "SessionStart(compact) keeps a Running turn Running (the auto-compaction blip fix)");
        CHECK(NextSessionState(SessionState::WaitingForInput, compactStart) == SessionState::WaitingForInput, "SessionStart(compact) keeps WaitingForInput");
        CHECK(NextSessionState(SessionState::NeedsApproval, compactStart) == SessionState::NeedsApproval, "SessionStart(compact) keeps NeedsApproval");
        CHECK(NextSessionState(SessionState::Idle, compactStart) == SessionState::Idle, "SessionStart(compact) keeps Idle");
        CHECK(NextSessionState(SessionState::Error, compactStart) == SessionState::Idle, "SessionStart(compact) from Error -> Idle (a completed compaction made the conversation usable again)");
        CHECK(NextSessionState(SessionState::Done, compactStart) == SessionState::Idle, "SessionStart(compact) from Done -> Idle");
        // Every OTHER source keeps the historical mapping: a /clear, resume, startup or fork mid-run resets.
        for (const wchar_t* src : { L"startup", L"resume", L"clear", L"fork", L"" })
        {
            HookMessage other = Msg(L"a", HookEvent::SessionStart);
            other.source = src;
            CHECK(NextSessionState(SessionState::Running, other) == SessionState::Idle, "SessionStart(non-compact source) from Running -> Idle (unchanged)");
        }
    }
    // The compaction WINDOW (PreCompact -> PostCompact): claude is summarizing for 1-3+ minutes — Running
    // for the whole window; a MANUAL compaction then returns to the prompt (WaitingForInput); an AUTO one
    // continues its turn (state kept); an unknown trigger (an old forwarder) keeps the state.
    {
        HookMessage pre = Msg(L"a", HookEvent::PreCompact);
        pre.source = L"manual";
        CHECK(NextSessionState(SessionState::WaitingForInput, pre) == SessionState::Running, "PreCompact(manual) at rest -> Running (a /compact is claude WORKING)");
        CHECK(NextSessionState(SessionState::Idle, pre) == SessionState::Running, "PreCompact(manual) from Idle -> Running");
        CHECK(NextSessionState(SessionState::Error, pre) == SessionState::Running, "PreCompact from Error ('Prompt is too long' answered by /compact) -> Running");
        HookMessage preAuto = Msg(L"a", HookEvent::PreCompact);
        preAuto.source = L"auto";
        CHECK(NextSessionState(SessionState::Running, preAuto) == SessionState::Running, "PreCompact(auto) mid-turn stays Running");
        HookMessage post = Msg(L"a", HookEvent::PostCompact);
        post.source = L"manual";
        CHECK(NextSessionState(SessionState::Running, post) == SessionState::WaitingForInput, "PostCompact(manual) -> WaitingForInput (back at the prompt)");
        HookMessage postAuto = Msg(L"a", HookEvent::PostCompact);
        postAuto.source = L"auto";
        CHECK(NextSessionState(SessionState::Running, postAuto) == SessionState::Running, "PostCompact(auto) keeps the continuing turn Running");
        CHECK(NextSessionState(SessionState::Running, Msg(L"a", HookEvent::PostCompact)) == SessionState::Running, "PostCompact with no trigger keeps the current state (the scanner settles it)");
        // A compaction is never ⚡ evidence: it REPLACES the prefix, so the next request's cache is cold.
        CHECK(!IsApiTurnEvidence(pre) && !IsApiTurnEvidence(post) && !IsApiTurnEvidence(postAuto), "Pre/PostCompact never stamp the server-cache turn evidence");
    }

    CHECK(NextSessionState(SessionState::Idle, Msg(L"a", HookEvent::UserPromptSubmit)) == SessionState::Running, "UserPromptSubmit -> Running");
    CHECK(NextSessionState(SessionState::Running, Msg(L"a", HookEvent::PreToolUse)) == SessionState::Running, "PreToolUse -> Running");
    CHECK(NextSessionState(SessionState::Running, Msg(L"a", HookEvent::Stop)) == SessionState::WaitingForInput, "Stop -> WaitingForInput");

    // Stop with a question is STILL WaitingForInput (the guard lives in the scheduler).
    HookMessage q = Msg(L"a", HookEvent::Stop);
    q.lastMessageIsQuestion = true;
    CHECK(NextSessionState(SessionState::Running, q) == SessionState::WaitingForInput, "Stop(question) -> WaitingForInput (state-wise)");

    HookMessage perm = Msg(L"a", HookEvent::Notification);
    perm.permissionRequest = true;
    CHECK(NextSessionState(SessionState::Running, perm) == SessionState::NeedsApproval, "Notification(perm) -> NeedsApproval");

    HookMessage idle = Msg(L"a", HookEvent::Notification);
    idle.permissionRequest = false;
    CHECK(NextSessionState(SessionState::Running, idle) == SessionState::Running, "Notification(non-perm) leaves state");

    CHECK(NextSessionState(SessionState::Running, Msg(L"a", HookEvent::SubagentStop)) == SessionState::Running, "SubagentStop leaves state");
    CHECK(NextSessionState(SessionState::WaitingForInput, Msg(L"a", HookEvent::SessionEnd)) == SessionState::Done, "SessionEnd -> Done");

    // Agentmaster (API-error state): the engine-internal apiError flag -> Error, OVERRIDING the carrier
    // event's normal mapping (the synthetic isApiErrorMessage line carries a terminal stop_reason).
    HookMessage err = Msg(L"a", HookEvent::Notification);
    err.apiError = true;
    CHECK(NextSessionState(SessionState::Running, err) == SessionState::Error, "apiError -> Error (from Running)");
    HookMessage errStop = Msg(L"a", HookEvent::Stop);
    errStop.apiError = true;
    CHECK(NextSessionState(SessionState::Running, errStop) == SessionState::Error, "apiError -> Error (overrides a Stop carrier, no WaitingForInput leak)");
    // Recovery: the FIRST new turn event leaves Error — a real UserPromptSubmit (the user retrying) -> Running.
    CHECK(NextSessionState(SessionState::Error, Msg(L"a", HookEvent::UserPromptSubmit)) == SessionState::Running, "Error + UserPromptSubmit -> Running (come out of Error on first change)");
    CHECK(NextSessionState(SessionState::Error, Msg(L"a", HookEvent::PostToolUse)) == SessionState::Running, "Error + tool activity -> Running (resumed)");

    CHECK(ParseHookEvent(L"Stop") == HookEvent::Stop, "ParseHookEvent(Stop)");
    CHECK(ParseHookEvent(L"bogus") == HookEvent::Unknown, "ParseHookEvent(bogus) -> Unknown");
}

// Agentmaster (crash/restore state fidelity — Correctness Rule #16): RestoredSessionState maps a
// PERSISTED state to the state a reopened session seeds with. A crash persists the LIVE state; the
// reopen used to force Idle, dropping "which sessions need me". The mapping keeps the AT-REST "needs
// you" states (they survive a --resume) and normalizes in-flight/ended ones to Idle.
void TestRestoredSessionState()
{
    std::wprintf(L"Restored session state (crash/restore fidelity, Rule #16):\n");
    // AT-REST "needs you" states survive a reopen — this is the whole fix (Waiting/NeedsApproval were
    // being lost to Idle on a crash restore).
    CHECK(RestoredSessionState(SessionState::WaitingForInput) == SessionState::WaitingForInput, "restore keeps WaitingForInput (the reported waiting-for-you loss)");
    CHECK(RestoredSessionState(SessionState::NeedsApproval) == SessionState::NeedsApproval, "restore keeps NeedsApproval");
    // In-flight / transient / ended states normalize to Idle — a --resume'd claude does not continue an
    // interrupted turn (Running would stick behind the scanner's primed-cursor gate), Error is transient
    // (the scanner re-derives it from the tail), and Done means ended (a reopened session is alive again).
    CHECK(RestoredSessionState(SessionState::Running) == SessionState::Idle, "restore normalizes Running -> Idle (turn was interrupted; resume waits)");
    CHECK(RestoredSessionState(SessionState::Error) == SessionState::Idle, "restore normalizes Error -> Idle (scanner re-derives if still active)");
    CHECK(RestoredSessionState(SessionState::Done) == SessionState::Idle, "restore normalizes Done -> Idle (reopened => alive)");
    CHECK(RestoredSessionState(SessionState::Idle) == SessionState::Idle, "restore keeps Idle");
    // Idempotent: the mapping is applied at BOTH clobber sites (_RestoreClaudeSessions load AND
    // _LaunchClaudeSession re-home), so mapping a mapped value must be a fixed point — else the second
    // application could drift the state.
    for (auto st : { SessionState::Idle, SessionState::Running, SessionState::WaitingForInput, SessionState::NeedsApproval, SessionState::Error, SessionState::Done })
    {
        CHECK(RestoredSessionState(RestoredSessionState(st)) == RestoredSessionState(st), "RestoredSessionState is idempotent (applied at two restore seams)");
    }

    // RestoredQuestionFlag (Finding A fix): the PERSISTED question-guard flag rides ONLY a preserved
    // needs-you state, so a crash while WaitingForInput on a clarifying question keeps the guard (the
    // Autorunner can't auto-answer it), while a stale flag from an interrupted Running turn is dropped.
    CHECK(RestoredQuestionFlag(SessionState::WaitingForInput, true), "question flag kept for a restored WaitingForInput (the guard survives a crash)");
    CHECK(!RestoredQuestionFlag(SessionState::WaitingForInput, false), "no question flag stays no question flag (Waiting)");
    CHECK(RestoredQuestionFlag(SessionState::NeedsApproval, true), "question flag kept for NeedsApproval (harmless — not Autorunner-ready)");
    // The key guard: RestoredSessionState maps Running/Error/Done -> Idle, and the (now-stale) flag must
    // be dropped there or it would falsely hold the Autorunner queue on a session that already answered.
    CHECK(!RestoredQuestionFlag(RestoredSessionState(SessionState::Running), true), "STALE question flag dropped when a Running-interrupted turn normalizes to Idle (no false queue hold)");
    CHECK(!RestoredQuestionFlag(SessionState::Idle, true), "question flag dropped for Idle (an Idle session is never 'waiting on a question')");
    CHECK(!RestoredQuestionFlag(SessionState::Idle, false), "Idle + no flag stays no flag");
}

// Agentmaster (event ordering + turn identity): the layer over NextSessionState that makes the
// machine immune to out-of-order Stops (the Stop forwarder runs slow — transcript work — so turn
// N's Stop can land after turn N+1's UserPromptSubmit) and aware of TYPE-AHEAD (a prompt typed
// mid-turn fires UserPromptSubmit at Enter-time; the queued batch then runs as the next turn with
// NO further hook). See NextSessionStateOrdered (HookEvents.h) + TurnAccounting (SessionModels.h).
void TestOrderedStateMachine()
{
    std::wprintf(L"Ordered state machine (event ordering + turn identity):\n");

    const auto at = [](HookEvent ev, int64_t ts) {
        HookMessage m;
        m.sessionId = L"a";
        m.event = ev;
        m.ts = ts;
        // A REAL UserPromptSubmit always carries its prompt text (claude refuses an empty submit —
        // Enter on an empty box is a no-op), and the ordered machine's turn ACCOUNTING now keys on
        // that (DELIVERY_PLAN.md R3: an empty phantom twin gets the state effect only). The fixture
        // models the real shape; the R3 block below covers the empty-prompt variants explicitly.
        // Distinct per ts so the registry-integration block's rows never fold-collide.
        if (ev == HookEvent::UserPromptSubmit)
        {
            m.promptText = L"prompt@" + std::to_wstring(ts);
        }
        return m;
    };

    { // Type-ahead: a UPS mid-turn queues; the next Stop consumes the batch and STAYS Running.
        TurnAccounting t;
        auto r1 = NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        CHECK(r1.state == SessionState::Running && t.queuedPrompts == 0, "ordered: first UPS -> Running, nothing queued");
        auto r2 = NextSessionStateOrdered(r1.state, at(HookEvent::UserPromptSubmit, 2000), t);
        CHECK(r2.state == SessionState::Running && t.queuedPrompts == 1, "ordered: UPS mid-turn queues (type-ahead)");
        auto r3 = NextSessionStateOrdered(r2.state, at(HookEvent::Stop, 3000), t);
        CHECK(r3.state == SessionState::Running && !r3.turnComplete && t.queuedPrompts == 0, "ordered: Stop behind a queued prompt stays Running (batch turn), no advance");
        auto r4 = NextSessionStateOrdered(r3.state, at(HookEvent::Stop, 4000), t);
        CHECK(r4.state == SessionState::WaitingForInput && r4.turnComplete, "ordered: final Stop -> WaitingForInput + advance");
    }
    { // Type-ahead behind a permission request: NeedsApproval queues too; Stop resumes Running.
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        auto r2 = NextSessionStateOrdered(SessionState::NeedsApproval, at(HookEvent::UserPromptSubmit, 2000), t);
        CHECK(r2.state == SessionState::Running && t.queuedPrompts == 1, "ordered: UPS while NeedsApproval queues (turn still in flight)");
        auto r3 = NextSessionStateOrdered(r2.state, at(HookEvent::Stop, 3000), t);
        CHECK(r3.state == SessionState::Running && t.queuedPrompts == 0, "ordered: the approval-interlude turn's Stop still honors the queue");
    }
    { // Stale Stop: FIRED before the newest prompt -> it ends an older turn -> keep state, no advance.
      // (Gaps are realistic — a real turn spans seconds — because a clean Stop must also clear the
      // kMinRealTurnSpanMs floor below.)
        TurnAccounting t;
        auto r1 = NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        auto r2 = NextSessionStateOrdered(r1.state, at(HookEvent::Stop, 6000), t); // turn 1 done (5s turn)
        CHECK(r2.state == SessionState::WaitingForInput, "ordered: clean Stop -> Waiting");
        auto r3 = NextSessionStateOrdered(r2.state, at(HookEvent::UserPromptSubmit, 10000), t); // turn 2
        CHECK(r3.state == SessionState::Running, "ordered: turn-2 UPS -> Running");
        auto r4 = NextSessionStateOrdered(r3.state, at(HookEvent::Stop, 9000), t); // turn 1's LATE duplicate
        CHECK(r4.state == SessionState::Running && r4.staleStop && !r4.turnComplete, "ordered: stale Stop (ts < newest prompt) keeps Running");
        auto r5 = NextSessionStateOrdered(r4.state, at(HookEvent::Stop, 16000), t); // turn 2's real end (6s turn)
        CHECK(r5.state == SessionState::WaitingForInput && r5.turnComplete, "ordered: fresh Stop after the stale one completes the turn");
    }
    { // The kMinRealTurnSpanMs FLOOR (DELIVERY.md §9 — the b5f766fc incident): a duplicate Stop whose
      // slow forwarder stamped its ts AFTER the next prompt's UPS beats the strict `ts < lastPrompt`
      // test — but no real turn completes in milliseconds, so a Stop inside the floor reads STALE:
      // state kept, no question-bit, and CRUCIALLY no turnComplete (the advance that delivered the
      // next prompt into claude's running turn came from exactly this).
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 100000), t);
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::Stop, 110000), t); // turn 1 done
        auto r2 = NextSessionStateOrdered(SessionState::WaitingForInput, at(HookEvent::UserPromptSubmit, 120000), t); // turn 2 (our delivery's echo)
        CHECK(r2.state == SessionState::Running, "floor: turn-2 UPS -> Running");
        // The duplicate: turn 1's second Stop process, ts-stamped 15ms AFTER turn 2's UPS (the
        // incident's exact shape — 08:44:53.360 UPS, 08:44:53.375 Stop).
        auto dup = NextSessionStateOrdered(r2.state, at(HookEvent::Stop, 120015), t);
        CHECK(dup.state == SessionState::Running && dup.staleStop && !dup.turnComplete,
              "floor: a Stop 15ms after the newest UPS reads STALE (no advance, no state flip)");
        // Still under the floor near its edge:
        auto edge = NextSessionStateOrdered(SessionState::Running, at(HookEvent::Stop, 120000 + kMinRealTurnSpanMs - 1), t);
        CHECK(edge.staleStop && !edge.turnComplete, "floor: just under kMinRealTurnSpanMs still reads stale");
        // At/after the floor: a real completion.
        auto real = NextSessionStateOrdered(SessionState::Running, at(HookEvent::Stop, 120000 + kMinRealTurnSpanMs), t);
        CHECK(real.state == SessionState::WaitingForInput && real.turnComplete && !real.staleStop,
              "floor: at kMinRealTurnSpanMs the Stop is a real turn-complete");
        // The floor never gates the scanner's quiescent Stop (transcript-proven idle) — the
        // self-heal path for a genuinely-faster-than-floor turn.
        TurnAccounting t2;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 200000), t2);
        HookMessage q = at(HookEvent::Stop, 200010); // 10ms later, but transcript-proven
        q.quiescentStop = true;
        auto qs = NextSessionStateOrdered(SessionState::Running, q, t2);
        CHECK(qs.state == SessionState::WaitingForInput && qs.turnComplete, "floor: a quiescent Stop is exempt (the self-heal)");
        // The floor never gates the TYPE-AHEAD consume either (a queued batch's Stop legitimately
        // lands close behind the type-ahead prompt).
        TurnAccounting t3;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 300000), t3);
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 309000), t3); // type-ahead
        CHECK(t3.queuedPrompts == 1, "floor: type-ahead queued");
        auto ta = NextSessionStateOrdered(SessionState::Running, at(HookEvent::Stop, 309500), t3); // 500ms after the type-ahead UPS
        CHECK(ta.state == SessionState::Running && !ta.staleStop && t3.queuedPrompts == 0,
              "floor: the type-ahead consume stays floor-free (batch turn starts, still Running)");
    }
    { // Quiescent (scanner-synthesized) Stop overrides everything: the transcript is provably idle.
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 2000), t); // queue one
        CHECK(t.queuedPrompts == 1, "ordered: type-ahead recorded before the quiescent stop");
        HookMessage q = at(HookEvent::Stop, 500); // even with an ANCIENT ts...
        q.quiescentStop = true;
        auto r3 = NextSessionStateOrdered(SessionState::Running, q, t);
        CHECK(r3.state == SessionState::WaitingForInput && r3.turnComplete && !r3.staleStop && t.queuedPrompts == 0,
              "ordered: quiescent Stop always lands WaitingForInput + zeroes the queue");
    }
    { // ts==0 (an old forwarder): the stale check is off — arrival order, the pre-ordering behavior.
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 0), t);
        auto r = NextSessionStateOrdered(SessionState::Running, at(HookEvent::Stop, 0), t);
        CHECK(r.state == SessionState::WaitingForInput && r.turnComplete, "ordered: ts-less events fall back to arrival order");
    }
    { // SessionStart resets the accounting (a /resume must not inherit stale turn identity).
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 2000), t);
        CHECK(t.queuedPrompts == 1 && t.lastPromptUnixMs == 2000, "ordered: accounting accumulated");
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::SessionStart, 3000), t);
        CHECK(t.queuedPrompts == 0 && t.lastPromptUnixMs == 0, "ordered: SessionStart resets turn accounting");
    }
    { // Agentmaster (HOOKS.md "Compaction"): a compaction's SessionStart does NOT reset the accounting — the
      // same conversation (and, after an AUTO-compaction, the same TURN) continues. Wiping it dropped a
      // type-ahead count, so the batch's Stop read as a turn-complete (the autorunner advancing into the
      // running batch) and the stale/duplicate-Stop guards lost their newest-prompt reference.
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 2000), t); // type-ahead
        CHECK(t.queuedPrompts == 1 && t.lastPromptUnixMs == 2000, "compact: accounting accumulated");
        HookMessage pre = at(HookEvent::PreCompact, 60000);
        pre.source = L"auto";
        const auto rp = NextSessionStateOrdered(SessionState::Running, pre, t);
        HookMessage post = at(HookEvent::PostCompact, 150000);
        post.source = L"auto";
        const auto rq = NextSessionStateOrdered(rp.state, post, t);
        HookMessage start = at(HookEvent::SessionStart, 150100);
        start.source = L"compact";
        const auto rs = NextSessionStateOrdered(rq.state, start, t);
        CHECK(rp.state == SessionState::Running && rq.state == SessionState::Running && rs.state == SessionState::Running,
              "compact: an auto-compaction window keeps the turn Running end to end");
        CHECK(t.queuedPrompts == 1 && t.lastPromptUnixMs == 2000, "compact: SessionStart(compact) + the window leave the accounting intact");
        CHECK(!rp.turnComplete && !rq.turnComplete && !rs.turnComplete, "compact: no turn boundary inside the window (no advance)");
        // ...so the batch's Stop is still a type-ahead consume (stays Running), exactly as without a compaction.
        const auto rb = NextSessionStateOrdered(rs.state, at(HookEvent::Stop, 200000), t);
        CHECK(rb.state == SessionState::Running && !rb.turnComplete && t.queuedPrompts == 0, "compact: the type-ahead batch survives the compaction");
    }
    { // A MANUAL compaction at rest: Running for the window, then WaitingForInput — NOT a turn completion.
        TurnAccounting t;
        HookMessage pre = at(HookEvent::PreCompact, 1000);
        pre.source = L"manual";
        const auto rp = NextSessionStateOrdered(SessionState::WaitingForInput, pre, t);
        CHECK(rp.state == SessionState::Running && !rp.turnComplete, "manual compact: PreCompact -> Running");
        HookMessage post = at(HookEvent::PostCompact, 120000);
        post.source = L"manual";
        const auto rq = NextSessionStateOrdered(rp.state, post, t);
        CHECK(rq.state == SessionState::WaitingForInput && !rq.turnComplete && !rq.staleStop,
              "manual compact: PostCompact -> WaitingForInput without a turnComplete (no question-guard, no Stop-seam advance)");
        HookMessage start = at(HookEvent::SessionStart, 120050);
        start.source = L"compact";
        const auto rs = NextSessionStateOrdered(rq.state, start, t);
        CHECK(rs.state == SessionState::WaitingForInput, "manual compact: the trailing SessionStart(compact) keeps WaitingForInput");
    }
    { // Agentmaster (HOOKS.md "Compaction"): a prompt typed DURING a manual compaction — PreCompact set
      // Running, so it counted as type-ahead — runs as the NEXT turn right after the compaction with no
      // further UserPromptSubmit. PostCompact consumes that batch and stays Running (not READY), exactly
      // like the type-ahead consume a Stop performs; the batch's own Stop then completes normally.
        TurnAccounting t;
        HookMessage pre = at(HookEvent::PreCompact, 1000);
        pre.source = L"manual";
        const auto rp = NextSessionStateOrdered(SessionState::WaitingForInput, pre, t);
        const auto ru = NextSessionStateOrdered(rp.state, at(HookEvent::UserPromptSubmit, 5000), t);
        CHECK(ru.state == SessionState::Running && t.queuedPrompts == 1, "manual compact: a prompt typed during the window is type-ahead");
        HookMessage post = at(HookEvent::PostCompact, 120000);
        post.source = L"manual";
        const auto rq = NextSessionStateOrdered(ru.state, post, t);
        CHECK(rq.state == SessionState::Running && !rq.turnComplete && t.queuedPrompts == 0,
              "manual compact: PostCompact consumes the typed batch and stays Running (claude runs it next) instead of reading READY");
        HookMessage start = at(HookEvent::SessionStart, 120050);
        start.source = L"compact";
        const auto rs = NextSessionStateOrdered(rq.state, start, t);
        CHECK(rs.state == SessionState::Running && t.lastPromptUnixMs == 5000, "manual compact: the trailing SessionStart(compact) keeps the batch Running and the prompt stamp");
        const auto rb = NextSessionStateOrdered(rs.state, at(HookEvent::Stop, 180000), t);
        CHECK(rb.state == SessionState::WaitingForInput && rb.turnComplete, "manual compact: the typed batch's own Stop completes the turn");
    }
    { // The cap bounds a pathological type-ahead burst (collapse-on-Stop bounds drift anyway).
        TurnAccounting t;
        for (int i = 0; i < kMaxQueuedPrompts + 5; ++i)
        {
            NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 1000 + i), t);
        }
        CHECK(t.queuedPrompts == kMaxQueuedPrompts, "ordered: queuedPrompts capped");
    }
    { // API error: -> Error, settles type-ahead like a Stop, but is NOT a clean turn boundary
      // (turnComplete stays false -> no autorunner advance off an error; the scheduler's stopOnError pauses).
        TurnAccounting t;
        NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 2000), t); // queue one
        CHECK(t.queuedPrompts == 1, "ordered: type-ahead recorded before the error");
        HookMessage e = at(HookEvent::Notification, 3000);
        e.apiError = true;
        auto re = NextSessionStateOrdered(SessionState::Running, e, t);
        CHECK(re.state == SessionState::Error && !re.turnComplete && !re.staleStop && t.queuedPrompts == 0,
              "ordered: apiError -> Error, no turnComplete, queue voided");
        // Recovery through the ordered machine: a fresh prompt from Error -> Running, no spurious queue bump.
        auto rr = NextSessionStateOrdered(SessionState::Error, at(HookEvent::UserPromptSubmit, 4000), t);
        CHECK(rr.state == SessionState::Running && t.queuedPrompts == 0, "ordered: Error + new prompt -> Running (fresh turn, not queued)");
    }
    { // Agentmaster (DELIVERY_PLAN.md R3 — the EMPTY-prompt UserPromptSubmit gate). The live phantom
      // twins (b5f766fc: 08:42:28.904 / 08:44:53.902 / 08:50:14.515 — late duplicate hook deliveries
      // whose payload carried NO prompt; each matched no transcript user message and recorded no
      // Typed row) and the scanner's recon-run synth are STATE-ONLY: -> Running (self-healing if
      // phantom — the machine's documented property), but they neither stamp turns.lastPromptUnixMs
      // (a twin's late ts floor-suppressed the next REAL Stop and spuriously released the pickup
      // guard's prompt-stamp clause) nor count type-ahead (a phantom count made the real Stop read
      // as a batch consume and strand Running until the quiescent heal).
        const auto emptyUps = [](int64_t ts) {
            HookMessage m;
            m.sessionId = L"a";
            m.event = HookEvent::UserPromptSubmit;
            m.ts = ts;
            return m; // promptText deliberately empty — the phantom-twin / recon-run-synth shape
        };
        TurnAccounting t;
        auto r1 = NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 400000), t); // the real submit
        CHECK(r1.state == SessionState::Running && t.lastPromptUnixMs == 400000, "R3: a prompt-carrying UPS stamps");
        auto ph = NextSessionStateOrdered(r1.state, emptyUps(402600), t); // the empty twin, +2.6s (the measured skew)
        CHECK(ph.state == SessionState::Running, "R3: an empty UPS keeps the state effect (Running self-heals if phantom)");
        CHECK(t.lastPromptUnixMs == 400000, "R3: an empty UPS never stamps lastPromptUnixMs");
        CHECK(t.queuedPrompts == 0, "R3: an empty UPS mid-turn never counts as type-ahead");
        auto realStop = NextSessionStateOrdered(ph.state, at(HookEvent::Stop, 404500), t); // the real 4.5s turn end
        CHECK(realStop.state == SessionState::WaitingForInput && realStop.turnComplete && !realStop.staleStop,
              "R3: the real Stop after an empty twin completes CLEANLY (pre-gate its 1.9s span vs the twin's stamp read floor-stale)");
        // The 08:42 window replay (the plan's acceptance shape): echo UPS -> dup Stop (floor-stale)
        // -> empty dup UPS (accounting-inert) -> real Stop = exactly ONE clean turn.
        TurnAccounting w;
        auto e1 = NextSessionStateOrdered(SessionState::WaitingForInput, at(HookEvent::UserPromptSubmit, 500000), w); // "asdasd"'s echo
        CHECK(e1.state == SessionState::Running, "R3 replay: the echo starts the turn");
        auto d1 = NextSessionStateOrdered(e1.state, at(HookEvent::Stop, 500015), w); // the dup Stop, 15ms later
        CHECK(d1.staleStop && !d1.turnComplete, "R3 replay: the dup Stop reads stale (the §9 floor)");
        NextSessionStateOrdered(d1.state, emptyUps(506400), w); // the empty dup UPS (+6.4s, the incident's skew)
        CHECK(w.lastPromptUnixMs == 500000 && w.queuedPrompts == 0, "R3 replay: the empty dup UPS is accounting-inert");
        auto realEnd = NextSessionStateOrdered(SessionState::Running, at(HookEvent::Stop, 508290), w); // the real end (~8.3s turn)
        CHECK(realEnd.state == SessionState::WaitingForInput && realEnd.turnComplete && !realEnd.staleStop,
              "R3 replay: one clean turn-complete for the whole window (pre-gate: 508290-506400=1.9s < floor -> wrongly suppressed)");
    }
    { // Registry integration: the full OnHookEvent path applies the ordered machine + advance seam.
        SessionRegistry reg;
        auto s = MakeSession(L"ord-1");
        s.live = true;
        reg.Upsert(s);
        std::atomic<int> advances{ 0 };
        reg.SetAdvanceHandler([&](const std::wstring&) { advances.fetch_add(1); });

        auto ups = at(HookEvent::UserPromptSubmit, 1000);
        ups.sessionId = L"ord-1";
        reg.OnHookEvent(ups); // turn 1
        auto ups2 = at(HookEvent::UserPromptSubmit, 2000);
        ups2.sessionId = L"ord-1";
        reg.OnHookEvent(ups2); // type-ahead, queued behind turn 1
        auto stop1 = at(HookEvent::Stop, 3000);
        stop1.sessionId = L"ord-1";
        reg.OnHookEvent(stop1); // consumed by the queued prompt
        CHECK(reg.Get(L"ord-1")->state == SessionState::Running, "registry: Stop behind a queued prompt stays Running");
        CHECK(advances.load() == 0, "registry: no advance mid-batch (Rule #1, one prompt per turn)");
        CHECK(reg.Get(L"ord-1")->lastActivityUnixMs == 3000, "registry: wire ts drives the activity/decay anchor");

        auto stale = at(HookEvent::Stop, 900); // fired before the newest prompt
        stale.sessionId = L"ord-1";
        stale.lastMessageIsQuestion = true;
        reg.OnHookEvent(stale);
        const auto after = reg.Get(L"ord-1");
        CHECK(after && after->state == SessionState::Running, "registry: stale Stop ignored (state kept)");
        CHECK(after && !after->lastMessageWasQuestion, "registry: stale Stop's question bit suppressed");
        CHECK(after && after->lastActivityUnixMs == 3000, "registry: a stale ts does not regress the anchor");

        auto stop2 = at(HookEvent::Stop, 30000); // a realistic turn span past the ts-2000 prompt (clears the kMinRealTurnSpanMs floor)
        stop2.sessionId = L"ord-1";
        stop2.lastMessageIsQuestion = true;
        reg.OnHookEvent(stop2);
        CHECK(reg.Get(L"ord-1")->state == SessionState::WaitingForInput, "registry: final Stop -> Waiting");
        CHECK(reg.Get(L"ord-1")->lastMessageWasQuestion, "registry: a FRESH Stop's question bit applies");
        CHECK(advances.load() == 1, "registry: exactly one advance for the whole batch");
    }
    { // ⚡ server-cache hint (IsApiTurnEvidence + lastTurnUnixMs + ServerCacheStillWarm): the hint keys
      // on REAL API-turn evidence only — never the decay anchor, which SessionStart (launch / --resume /
      // ADOPT / /clear) and the UI's "Move to Waiting-for-you" triage promote stamp "now" with zero API
      // traffic (the reported false positives).
        HookMessage probe;
        probe.event = HookEvent::SubagentStop;
        CHECK(!IsApiTurnEvidence(probe), "cache: SubagentStop is NOT turn evidence — a subagent/teammate turn runs in its OWN context and never re-warms the LEAD's cache (in-process teammates fire it for hours post-turn)");
        probe.event = HookEvent::PostToolUse;
        CHECK(IsApiTurnEvidence(probe), "cache: a real PostToolUse (parent mid-turn) is turn evidence");
        probe.externalWorkActivity = true;
        CHECK(!IsApiTurnEvidence(probe), "cache: the scanner's recon-subagent external-work PostToolUse synth is NOT evidence (side-file/heartbeat work, not a lead API turn)");
        probe.externalWorkActivity = false;
        probe.event = HookEvent::Notification;
        CHECK(IsApiTurnEvidence(probe), "cache: Notification (permission ask / recon-error carrier) is turn evidence");
        probe.event = HookEvent::Stop;
        probe.quiescentStop = true;
        CHECK(!IsApiTurnEvidence(probe), "cache: a synthesized quiescent Stop is reconciliation-timed, NOT turn evidence");
        probe.quiescentStop = false;
        CHECK(IsApiTurnEvidence(probe), "cache: a REAL Stop (clean turn end) is turn evidence");
        probe.event = HookEvent::Unknown;
        CHECK(!IsApiTurnEvidence(probe), "cache: Unknown is not turn evidence");

        SessionRegistry reg;
        auto s = MakeSession(L"cache-1");
        s.live = true;
        reg.Upsert(s);

        auto start = at(HookEvent::SessionStart, 1000);
        start.sessionId = L"cache-1";
        reg.OnHookEvent(start);
        auto g = reg.Get(L"cache-1");
        CHECK(g && g->lastActivityUnixMs == 1000, "cache: SessionStart still drives the decay anchor (unchanged)");
        CHECK(g && g->lastTurnUnixMs == 0, "cache: SessionStart never stamps turn evidence (launch/resume/adopt/clear send no API request)");
        CHECK(g && !ServerCacheStillWarm(*g, 5, 1500), "cache: a just-launched/adopted/resumed session shows NO warm-cache hint");

        auto ups = at(HookEvent::UserPromptSubmit, 2000);
        ups.sessionId = L"cache-1";
        reg.OnHookEvent(ups);
        g = reg.Get(L"cache-1");
        CHECK(g && g->lastTurnUnixMs == 2000, "cache: UserPromptSubmit stamps turn evidence (the API request fires on submit)");
        // ... but the hint stays DARK while that turn runs: UserPromptSubmit -> Running, and the hint is
        // AT-REST-only. Mid-turn it would be both uninformative (nothing to decide) and permanently lit
        // (a running turn keeps refreshing the very timestamps the window is measured from).
        CHECK(g && g->state == SessionState::Running, "cache: (UserPromptSubmit left the session Running — the precondition for the next check)");
        CHECK(g && !ServerCacheStillWarm(*g, 5, 2000 + 4 * 60000), "cache: NEVER warm while Running, even with fresh turn evidence inside the window");
        // The SAME evidence, once the session comes to rest, DOES light it — i.e. the gate above is the
        // state, not the timestamp.
        {
            auto atRest = *g;
            atRest.state = SessionState::WaitingForInput;
            CHECK(ServerCacheStillWarm(atRest, 5, 2000 + 4 * 60000), "cache: warm inside the serverCacheMinutes window once at rest");
            CHECK(!ServerCacheStillWarm(atRest, 5, 2000 + 5 * 60000), "cache: lapses once the window elapses");
            // Every other at-rest state qualifies too (the user-visible rule: Waiting-for-you /
            // Needs-approval / Error / Idle / Done — everything except Running).
            for (const auto st : { SessionState::WaitingForInput, SessionState::NeedsApproval, SessionState::Error, SessionState::Idle, SessionState::Done })
            {
                auto v = *g;
                v.state = st;
                CHECK(ServerCacheStillWarm(v, 5, 2000 + 60000), "cache: warm in every AT-REST state");
            }
        }

        auto qstop = at(HookEvent::Stop, 300000);
        qstop.sessionId = L"cache-1";
        qstop.quiescentStop = true;
        reg.OnHookEvent(qstop);
        g = reg.Get(L"cache-1");
        CHECK(g && g->lastTurnUnixMs == 2000, "cache: a quiescent recon Stop refreshes the anchor but NOT the turn evidence");
        CHECK(g && g->lastActivityUnixMs == 300000, "cache: (the anchor did move — decay semantics intact)");

        auto realStop = at(HookEvent::Stop, 300001);
        realStop.sessionId = L"cache-1";
        reg.OnHookEvent(realStop);
        g = reg.Get(L"cache-1");
        CHECK(g && g->lastTurnUnixMs == 300001, "cache: a REAL Stop stamps turn evidence (cache just re-written at turn end)");

        auto end = at(HookEvent::SessionEnd, 400000);
        end.sessionId = L"cache-1";
        reg.OnHookEvent(end);
        g = reg.Get(L"cache-1");
        CHECK(g && g->lastTurnUnixMs == 300001, "cache: SessionEnd never stamps turn evidence");

        // The "Move to Waiting-for-you" triage promote (both UI surfaces) restarts ONLY the decay
        // anchor — the ⚡ must not light off it (the exact reported false positive).
        reg.Update(L"cache-1", [](SessionInfo& si) {
            si.state = SessionState::WaitingForInput;
            si.manualUnread = false;
            si.lastActivityUnixMs = 10000000; // the triage move stamps "now"
            si.readUnixMs = 0;
        });
        g = reg.Get(L"cache-1");
        CHECK(g && !ServerCacheStillWarm(*g, 5, 10000000 + 1000), "cache: a triage 'Move to Waiting-for-you' never lights the warm-cache hint");

        // Pure-gate edges: transcript-derived signal / archived / Codex / anchor-only.
        SessionInfo pure;
        pure.live = true;
        pure.kind = AgentKind::Claude;
        pure.convApiActivityUnixMs = 100000; // the observer's PARENT-line-derived API activity
        CHECK(ServerCacheStillWarm(pure, 5, 100000 + 60000), "cache: transcript-derived API activity lights the hint (hook-less sessions)");
        pure.live = false;
        CHECK(!ServerCacheStillWarm(pure, 5, 100000 + 60000), "cache: an archived (!live) session never shows it");
        pure.live = true;
        pure.kind = AgentKind::Codex;
        CHECK(!ServerCacheStillWarm(pure, 5, 100000 + 60000), "cache: a managed Codex never shows the Claude cache hint");
        pure.kind = AgentKind::Claude;
        pure.convApiActivityUnixMs = 0;
        pure.lastTurnUnixMs = 0;
        pure.lastActivityUnixMs = 100000; // decay-anchor-only (the SessionStart / triage-move shape)
        CHECK(!ServerCacheStillWarm(pure, 5, 100000 + 1), "cache: the decay anchor alone never lights it (the old false positive)");
        pure.convLastActivityUnixMs = 100000; // the FOLDED display activity (subagent/teammate side files ride it)
        CHECK(!ServerCacheStillWarm(pure, 5, 100000 + 1), "cache: the folded display activity alone never lights it — a teammate/subagent side-file write is not a lead API turn");
        pure.convLastActivityUnixMs = 0;
        pure.lastTurnUnixMs = 100000;
        pure.convApiActivityUnixMs = 200000;
        CHECK(ServerCacheStillWarm(pure, 5, 200000 + 60000) && !ServerCacheStillWarm(pure, 5, 200000 + 5 * 60000),
              "cache: the freshest of the two turn signals wins");
        CHECK(!ServerCacheStillWarm(pure, 0, 200000 + 1), "cache: 0 minutes -> never (callers normalize, belt anyway)");
        // The teammate shape end-to-end on the pure gate: folded display activity FRESH (a background
        // team writing "just now") while the lead's own last API line is ~1 h old -> ⚡ stays COLD.
        SessionInfo team;
        team.live = true;
        team.kind = AgentKind::Claude;
        team.convApiActivityUnixMs = 100000; // the lead's own last API turn
        team.convLastActivityUnixMs = 100000 + 55 * 60000; // teammates still writing side files an hour later
        CHECK(!ServerCacheStillWarm(team, 5, 100000 + 55 * 60000 + 1000), "cache: teammate/subagent side-file activity alone never keeps ⚡ warm (their turns run in their own context)");
    }
}

void TestWire()
{
    std::wprintf(L"Wire format:\n");
    {
        auto m = ParseWireLine(L"Stop\tabc\tK:/api\t1\t0\t");
        CHECK(m.has_value(), "parse full line");
        CHECK(m && m->event == HookEvent::Stop, "wire event");
        CHECK(m && m->sessionId == L"abc", "wire sid");
        CHECK(m && m->cwd == L"K:/api", "wire cwd");
        CHECK(m && m->lastMessageIsQuestion, "wire isQ");
        CHECK(m && !m->permissionRequest, "wire perm false");
    }
    {
        auto m = ParseWireLine(L"Stop\tabc\r\n");
        CHECK(m.has_value(), "parse minimal + CRLF");
        CHECK(m && m->cwd.empty(), "missing cwd ok");
    }
    {
        CHECK(!ParseWireLine(L"").has_value(), "empty -> nullopt");
        CHECK(!ParseWireLine(L"Stop").has_value(), "no sid -> nullopt");
        CHECK(!ParseWireLine(L"Stop\t").has_value(), "empty sid -> nullopt");
    }
    {
        HookMessage m;
        m.event = HookEvent::Notification;
        m.sessionId = L"sid-xyz";
        m.cwd = L"K:/ui";
        m.permissionRequest = true;
        m.tool = L"Bash";
        m.tabToken = L"wt-9f3";
        auto rt = ParseWireLine(BuildWireLine(m));
        CHECK(rt.has_value(), "round-trip parses");
        CHECK(rt && rt->event == HookEvent::Notification && rt->sessionId == L"sid-xyz" && rt->cwd == L"K:/ui" && rt->permissionRequest && rt->tool == L"Bash", "round-trip fields");
        CHECK(rt && rt->tabToken == L"wt-9f3", "round-trip tabToken (WT_SESSION)");
    }
    {
        // 7-field line: the trailing tabToken (WT_SESSION) parses.
        auto m = ParseWireLine(L"SessionStart\tsid\tK:/api\t0\t0\tBash\twt-guid-123");
        CHECK(m && m->tabToken == L"wt-guid-123", "wire tabToken parsed");
    }
    {
        // Backward-compat: a 6-field line (pre-tabToken forwarder) still parses; token empty.
        auto m = ParseWireLine(L"Stop\tabc\tK:/api\t1\t0\tBash");
        CHECK(m && m->tabToken.empty(), "missing tabToken ok");
    }
    {
        // The trailing prompt field carries the submitted message (UserPromptSubmit), escaped
        // so embedded TAB/newline survive a single-line, TAB-split record.
        HookMessage m;
        m.event = HookEvent::UserPromptSubmit;
        m.sessionId = L"sid";
        m.cwd = L"K:/api";
        m.tabToken = L"wt-1";
        m.promptText = L"line1\tcol\nline2 \\ end";
        const auto wire = BuildWireLine(m);
        CHECK(wire.find(L'\n') == std::wstring::npos, "escaped prompt keeps the record single-line");
        auto rt = ParseWireLine(wire);
        CHECK(rt && rt->promptText == L"line1\tcol\nline2 \\ end", "prompt round-trips TAB/newline/backslash");
        CHECK(rt && rt->event == HookEvent::UserPromptSubmit && rt->tabToken == L"wt-1", "prompt line keeps other fields");
    }
    {
        // Escape helpers are exact inverses (what the PowerShell forwarder mirrors).
        const std::wstring raw = L"a\\b\tc\r\nd";
        CHECK(WireEscape(raw) == L"a\\\\b\\tc\\r\\nd", "WireEscape \\ tab CR LF");
        CHECK(WireUnescape(WireEscape(raw)) == raw, "WireUnescape inverts WireEscape");
        // A line literally carrying an escaped prompt decodes back to TAB + newline.
        auto m = ParseWireLine(L"UserPromptSubmit\tsid\t\t0\t0\t\t\tfoo\\tbar\\nbaz");
        CHECK(m && m->promptText == L"foo\tbar\nbaz", "literal escaped prompt decodes");
    }
    {
        // 9th field: `ts` — the hook's FIRE time (unix ms) — appended LAST (after the escaped,
        // TAB-free prompt) so both directions stay compatible across forwarder versions.
        HookMessage m;
        m.event = HookEvent::Stop;
        m.sessionId = L"sid";
        m.ts = 1718000000123;
        auto rt = ParseWireLine(BuildWireLine(m));
        CHECK(rt && rt->ts == 1718000000123, "wire ts round-trips");
        // The exact line shape the PowerShell forwarder emits.
        auto p = ParseWireLine(L"Stop\tabc\tK:/api\t0\t0\t\twt-1\t\t1718000000456");
        CHECK(p && p->ts == 1718000000456, "wire ts parsed from a forwarder-shaped line");
        // An old 8-field line (pre-ts forwarder) and garbage both land on 0 (arrival-order fallback).
        auto old8 = ParseWireLine(L"Stop\tabc\tK:/api\t0\t0\t\twt-1\tprompt");
        CHECK(old8 && old8->ts == 0 && old8->promptText == L"prompt", "8-field line (old forwarder) -> ts 0, prompt intact");
        auto bad = ParseWireLine(L"Stop\tabc\tK:/api\t0\t0\t\twt-1\t\t17abc");
        CHECK(bad && bad->ts == 0, "non-numeric ts -> 0");
        auto huge = ParseWireLine(L"Stop\tabc\tK:/api\t0\t0\t\twt-1\t\t9999999999999999999999");
        CHECK(huge && huge->ts == 0, "over-long ts -> 0 (overflow guard)");
        // A ts-bearing UserPromptSubmit keeps the escaped prompt unambiguous (prompt is TAB-free
        // on the wire, so the trailing ts field can never be mistaken for prompt text).
        HookMessage up;
        up.event = HookEvent::UserPromptSubmit;
        up.sessionId = L"sid";
        up.promptText = L"a\tb\nc";
        up.ts = 42;
        auto urt = ParseWireLine(BuildWireLine(up));
        CHECK(urt && urt->promptText == L"a\tb\nc" && urt->ts == 42, "escaped prompt + trailing ts round-trip together");
    }
    {
        // 10th field: `source` — the hook payload's own discriminator (SessionStart source / SessionEnd
        // reason / Pre-PostCompact trigger), appended LAST after `ts` (HOOKS.md "Compaction").
        HookMessage m;
        m.event = HookEvent::SessionStart;
        m.sessionId = L"sid";
        m.ts = 1718000000999;
        m.source = L"compact";
        auto rt = ParseWireLine(BuildWireLine(m));
        CHECK(rt && rt->source == L"compact" && rt->ts == 1718000000999, "wire source round-trips beside ts");
        // The exact 10-field lines the forwarder emits for the compaction window.
        auto pre = ParseWireLine(L"PreCompact\tsid\tK:/api\t0\t0\t\twt-1\t\t1718000000456\tmanual");
        CHECK(pre && pre->event == HookEvent::PreCompact && pre->source == L"manual" && pre->ts == 1718000000456, "PreCompact line parses with its trigger");
        auto post = ParseWireLine(L"PostCompact\tsid\t\t0\t0\t\t\t\t1\tauto");
        CHECK(post && post->event == HookEvent::PostCompact && post->source == L"auto", "PostCompact line parses with its trigger");
        auto end = ParseWireLine(L"SessionEnd\tsid\t\t0\t0\t\t\t\t1\tprompt_input_exit");
        CHECK(end && end->source == L"prompt_input_exit", "SessionEnd reason rides the same field");
        // A 9-field line (a pre-source forwarder) parses with source "" — the historical path.
        auto old9 = ParseWireLine(L"SessionStart\tsid\tK:/api\t0\t0\t\twt-1\t\t1718000000456");
        CHECK(old9 && old9->source.empty() && old9->ts == 1718000000456, "9-field line (old forwarder) -> source empty, ts intact");
        // Anything that is not a short identifier token degrades to "" — never a partial match.
        CHECK(SanitizeWireSource(L"compact") == L"compact", "sanitize keeps a plain token");
        CHECK(SanitizeWireSource(L"prompt_input_exit") == L"prompt_input_exit", "sanitize keeps underscores");
        CHECK(SanitizeWireSource(L"com pact").empty(), "sanitize rejects a space");
        CHECK(SanitizeWireSource(L"compact\r").empty(), "sanitize rejects a control char");
        CHECK(SanitizeWireSource(std::wstring(kWireSourceMaxChars + 1, L'a')).empty(), "sanitize rejects an over-long value");
        CHECK(SanitizeWireSource(std::wstring(kWireSourceMaxChars, L'a')).size() == kWireSourceMaxChars, "sanitize keeps a max-length value");
        auto junk = ParseWireLine(L"SessionStart\tsid\t\t0\t0\t\t\t\t1\tcompäct");
        CHECK(junk && junk->source.empty(), "a non-ASCII source reads empty");
        // BuildWireLine sanitizes too, so an unsanitary in-memory value can never split the record.
        HookMessage h;
        h.event = HookEvent::SessionStart;
        h.sessionId = L"sid";
        h.source = L"a\tb";
        auto hr = ParseWireLine(BuildWireLine(h));
        CHECK(hr && hr->source.empty() && hr->sessionId == L"sid", "BuildWireLine drops an unsanitary source");
        // The two new event names round-trip.
        CHECK(ParseHookEvent(L"PreCompact") == HookEvent::PreCompact && std::wstring{ HookEventName(HookEvent::PreCompact) } == L"PreCompact", "PreCompact name round-trips");
        CHECK(ParseHookEvent(L"PostCompact") == HookEvent::PostCompact && std::wstring{ HookEventName(HookEvent::PostCompact) } == L"PostCompact", "PostCompact name round-trips");
    }
    {
        CHECK(HookPipeName(1234) == L"\\\\.\\pipe\\agentmaster.1234", "pipe name format");
    }
}

void TestRegistry()
{
    std::wprintf(L"SessionRegistry:\n");
    SessionRegistry reg;
    std::atomic<int> observed{ 0 };
    std::atomic<int> advanced{ 0 };
    std::wstring lastAdvancedId;

    reg.AddObserver([&](const SessionInfo&, HookEvent) { observed.fetch_add(1); });
    reg.SetAdvanceHandler([&](const std::wstring& id) { advanced.fetch_add(1); lastAdvancedId = id; });

    reg.Upsert(MakeSession(L"s1"));
    CHECK(reg.Count() == 1, "count after upsert");
    CHECK(reg.Get(L"s1").has_value(), "get s1");

    reg.OnHookEvent(Msg(L"s1", HookEvent::UserPromptSubmit));
    CHECK(reg.Get(L"s1")->state == SessionState::Running, "UPS -> Running");

    reg.OnHookEvent(Msg(L"s1", HookEvent::Stop));
    CHECK(reg.Get(L"s1")->state == SessionState::WaitingForInput, "Stop -> WaitingForInput");
    CHECK(advanced.load() == 1, "advance fired on Stop");
    CHECK(lastAdvancedId == L"s1", "advance carried id");
    CHECK(!reg.Get(L"s1")->lastMessageWasQuestion, "no question flag");

    HookMessage stopQ = Msg(L"s1", HookEvent::Stop);
    stopQ.lastMessageIsQuestion = true;
    reg.OnHookEvent(stopQ);
    CHECK(reg.Get(L"s1")->lastMessageWasQuestion, "question flag set from Stop");
    CHECK(advanced.load() == 2, "advance fired again on Stop");

    HookMessage perm = Msg(L"s1", HookEvent::Notification);
    perm.permissionRequest = true;
    reg.OnHookEvent(perm);
    CHECK(reg.Get(L"s1")->state == SessionState::NeedsApproval, "perm -> NeedsApproval");
    CHECK(advanced.load() == 2, "advance NOT fired on approval (Rule #1)");

    // Unknown session: non-SessionStart ignored; SessionStart auto-creates.
    reg.OnHookEvent(Msg(L"ghost", HookEvent::Stop));
    CHECK(reg.Count() == 1, "unknown non-start ignored");
    reg.OnHookEvent(Msg(L"ghost", HookEvent::SessionStart));
    CHECK(reg.Count() == 2, "SessionStart auto-creates");

    // Adoption seam (observe+control of a session we did NOT Launch): a SessionStart for a
    // brand-new id fires the adoption handler with cwd + tabToken, and flags the record
    // external. A later hook for the same id must NOT re-adopt.
    int adopted = 0;
    std::wstring adoptId, adoptCwd, adoptTab;
    reg.AddAdoptionHandler([&](const std::wstring& aid, const std::wstring& acwd, const std::wstring& atab) {
        adopted++;
        adoptId = aid;
        adoptCwd = acwd;
        adoptTab = atab;
    });
    HookMessage ext = Msg(L"ext-1", HookEvent::SessionStart);
    ext.cwd = L"K:/ext";
    ext.tabToken = L"wt-77";
    reg.OnHookEvent(ext);
    CHECK(adopted == 1, "adoption handler fired for new session");
    CHECK(adoptId == L"ext-1" && adoptCwd == L"K:/ext" && adoptTab == L"wt-77", "adoption carried id+cwd+tabToken");
    CHECK(reg.Get(L"ext-1") && reg.Get(L"ext-1")->external, "adopted session flagged external");
    reg.OnHookEvent(Msg(L"ext-1", HookEvent::UserPromptSubmit));
    CHECK(adopted == 1, "no re-adoption for an already-known session");

    // Injector seam.
    std::wstring injected;
    reg.SetInjector(L"s1", [&](const std::wstring& t) { injected = t; });
    CHECK(reg.Inject(L"s1", L"hello\r"), "inject returns true when bound");
    CHECK(injected == L"hello\r", "inject delivered text");
    CHECK(!reg.Inject(L"s2-unbound", L"x"), "inject false when unbound");

    CHECK(observed.load() > 0, "observer fired");

    // Agentmaster (PENDING_INPUT.md): SetPendingInput updates the draft every change, but NOTIFIES only
    // on the BOOLEAN hasPending FLIP (empty<->non-empty) — the "yes pending / no pending" transition the
    // animations key on. A text-only edit (still non-empty) is QUIET; the flip return == the notify.
    {
        const int base = observed.load();
        CHECK(reg.SetPendingInput(L"s1", L"hello"), "pending: appear flips (empty -> non-empty)");
        CHECK(reg.Get(L"s1")->pendingInput == L"hello", "pending: stored on the record");
        CHECK(observed.load() == base + 1, "pending: appear NOTIFIES (boolean flip)");
        CHECK(!reg.SetPendingInput(L"s1", L"hello world"), "pending: text-only edit does NOT flip");
        CHECK(reg.Get(L"s1")->pendingInput == L"hello world", "pending: text updated quietly");
        CHECK(observed.load() == base + 1, "pending: text-only edit is QUIET (no notify)");
        CHECK(!reg.SetPendingInput(L"s1", L"hello world"), "pending: identical set is a no-op");
        CHECK(observed.load() == base + 1, "pending: no-op does not notify");
        // The OBSERVATION stamp (PENDING_INPUT.md §5): stamped on appear, quietly REFRESHED on a
        // re-observation of unchanged non-empty text (that is how consumers tell a live draft — age
        // within a few ticks — from a carried restored MEMORY), zeroed on clear.
        CHECK(reg.Get(L"s1")->pendingInputUnixMs > 0, "pending: observation stamp set on appear");
        reg.Update(L"s1", [](SessionInfo& s) { s.pendingInputUnixMs = 42; }); // plant an old stamp (Update notifies — re-baseline below)
        const int base2 = observed.load();
        CHECK(!reg.SetPendingInput(L"s1", L"hello world"), "pending: unchanged re-observation is a no-op return");
        CHECK(reg.Get(L"s1")->pendingInputUnixMs > 42, "pending: unchanged re-observation REFRESHES the stamp");
        CHECK(observed.load() == base2, "pending: the stamp refresh is QUIET (no notify)");
        // Paste-refs (PENDING_INPUT.md §2b): quiet, change-gated, draft-guarded.
        reg.SetPendingPasteRefs(L"s1", L"paste #1 (+273 lines) -> abc.txt");
        CHECK(reg.Get(L"s1")->pendingPasteRefs == L"paste #1 (+273 lines) -> abc.txt", "pending: paste refs recorded");
        CHECK(observed.load() == base2, "pending: paste refs are QUIET (no notify)");
        CHECK(reg.SetPendingInput(L"s1", L""), "pending: clear flips (non-empty -> empty)");
        CHECK(reg.Get(L"s1")->pendingInput.empty(), "pending: cleared");
        CHECK(reg.Get(L"s1")->pendingInputUnixMs == 0, "pending: clear zeroes the observation stamp");
        CHECK(reg.Get(L"s1")->pendingPasteRefs.empty(), "pending: clear drops the paste refs");
        reg.SetPendingPasteRefs(L"s1", L"late resolve");
        CHECK(reg.Get(L"s1")->pendingPasteRefs.empty(), "pending: a late resolve can't land on a cleared draft");
        CHECK(observed.load() == base2 + 1, "pending: clear NOTIFIES (boolean flip)");
        CHECK(!reg.SetPendingInput(L"nope", L"x"), "pending: unknown id no-op");
        CHECK(observed.load() == base2 + 1, "pending: unknown id does not notify");
    }

    reg.Remove(L"s1");
    CHECK(!reg.Get(L"s1").has_value(), "remove s1");
    CHECK(!reg.Inject(L"s1", L"x"), "inject false after remove");
}

// Agentmaster M9: the registry is a process-wide singleton with MANY observers/adopters — one
// per window's Manager lens. Verify AddObserver returns a removable token (RemoveObserver
// detaches exactly it, stale tokens no-op), and that adoption FANS OUT to every registered
// handler and detaches by token. This is the seam that keeps a closed window from dangling its
// observer (a strong DispatcherQueue ref) or its adoption handler on the shared registry.
void TestRegistryFanout()
{
    std::wprintf(L"SessionRegistry M9 (token observers + multi-adoption fan-out):\n");
    SessionRegistry reg;

    // Two observers; both fire on a change.
    std::atomic<int> a{ 0 };
    std::atomic<int> b{ 0 };
    const auto tokA = reg.AddObserver([&](const SessionInfo&, HookEvent) { a.fetch_add(1); });
    reg.AddObserver([&](const SessionInfo&, HookEvent) { b.fetch_add(1); });
    reg.Upsert(MakeSession(L"s1"));
    CHECK(a.load() == 1 && b.load() == 1, "both observers fired");

    // Remove the first; only the second fires next time.
    reg.RemoveObserver(tokA);
    reg.Update(L"s1", [](SessionInfo&) {});
    CHECK(a.load() == 1, "removed observer no longer fires");
    CHECK(b.load() == 2, "remaining observer still fires");

    // Removing a stale or unknown token is a harmless no-op (tokens are never reused).
    reg.RemoveObserver(tokA);
    reg.RemoveObserver(99999);
    reg.Update(L"s1", [](SessionInfo&) {});
    CHECK(b.load() == 3, "stale/unknown RemoveObserver is a no-op");

    // Multi-adoption: EVERY registered adopter is invoked for a session we didn't Launch.
    std::atomic<int> ad1{ 0 };
    std::atomic<int> ad2{ 0 };
    std::wstring seen1;
    std::wstring seen2;
    reg.AddAdoptionHandler([&](const std::wstring& id, const std::wstring&, const std::wstring&) { ad1.fetch_add(1); seen1 = id; });
    const auto adTok2 = reg.AddAdoptionHandler([&](const std::wstring& id, const std::wstring&, const std::wstring&) { ad2.fetch_add(1); seen2 = id; });
    reg.OnHookEvent(Msg(L"ext-a", HookEvent::SessionStart));
    CHECK(ad1.load() == 1 && ad2.load() == 1, "both adopters fired for a new session");
    CHECK(seen1 == L"ext-a" && seen2 == L"ext-a", "both adopters got the id");

    // Detach the second adopter; only the first fires for the next new session.
    reg.RemoveAdoptionHandler(adTok2);
    reg.OnHookEvent(Msg(L"ext-b", HookEvent::SessionStart));
    CHECK(ad1.load() == 2, "remaining adopter fired for the second new session");
    CHECK(ad2.load() == 1, "removed adopter did not fire");
}

// Agentmaster: a `claude --resume <src> --fork-session --session-id <new>` fork (the Sessions-page /
// duplicate-tab / adopt-external fork) fires its FIRST SessionStart hook under the SOURCE id <src>,
// NOT the minted <new> we registered + bound to its ConPTY at launch. The registry must IGNORE that
// source-id echo — otherwise the bind/re-home path mistakes it for an in-session /resume, re-homes the
// fork's tab off <new> onto the inactive <src>, and orphans the real fork (which gets every later
// hook). The guard recognizes the echo precisely: a LIVE fork session carrying forkParentId == <src>
// on the SAME ConPTY (the eagerly-stamped tabToken). It is one-shot via the TRANSIENT forkEchoConsumed
// latch (consumed by the suppressed echo or any own-id hook), so a LATER deliberate /resume back to
// <src> re-homes normally — while forkParentId (the never-messaged fork's RE-FORK link) survives until
// an own-id hook proves the fork produced content (never on SessionStart/SessionEnd).
void TestForkSourceIdEcho()
{
    std::wprintf(L"SessionRegistry: --fork-session source-id SessionStart echo is ignored:\n");
    SessionRegistry reg;

    int adopted = 0;
    std::wstring lastAdopt;
    reg.AddAdoptionHandler([&](const std::wstring& id, const std::wstring&, const std::wstring&) { adopted++; lastAdopt = id; });

    // A launched fork: live, its ConPTY's WT_SESSION stamped eagerly, forkParentId = the SOURCE it
    // branched from (exactly what _LaunchClaudeSession records for a fork).
    {
        SessionInfo fork = MakeSession(L"fork-new");
        fork.live = true;
        fork.tabToken = L"wt-fork";
        fork.forkParentId = L"src-parent";
        reg.Upsert(fork);
    }
    const auto baseCount = reg.Count();

    // The fork's startup SessionStart echoes the SOURCE id on the fork's OWN ConPTY -> IGNORED whole.
    HookMessage echo = Msg(L"src-parent", HookEvent::SessionStart);
    echo.cwd = L"K:/x";
    echo.tabToken = L"wt-fork";
    reg.OnHookEvent(echo);
    CHECK(reg.Count() == baseCount, "fork-echo: source-id SessionStart created no record");
    CHECK(!reg.Get(L"src-parent").has_value(), "fork-echo: source id not adopted");
    CHECK(adopted == 0, "fork-echo: no adoption fired for the source-id echo");
    CHECK(reg.Get(L"fork-new") && reg.Get(L"fork-new")->live, "fork-echo: the fork stays live + bound");
    CHECK(reg.Get(L"fork-new")->forkParentId == L"src-parent", "fork-echo: guard still armed (echo never reached the own-id path)");

    // The SAME source id on a DIFFERENT ConPTY (a genuinely separate session in another tab) is NOT
    // the echo -> processed normally (created + adopted). The tabToken match is the discriminator.
    HookMessage other = Msg(L"src-parent", HookEvent::SessionStart);
    other.cwd = L"K:/x";
    other.tabToken = L"wt-other";
    reg.OnHookEvent(other);
    CHECK(reg.Get(L"src-parent").has_value(), "fork-echo: same id on a different tabToken IS adopted");
    CHECK(adopted == 1 && lastAdopt == L"src-parent", "fork-echo: adoption fired for the non-echo SessionStart");

    // One-shot: once a fork emits under its OWN id the guard retires, so a later deliberate /resume
    // back to the source re-homes normally. Fresh ids to stay independent of src-parent created above.
    {
        SessionInfo fork2 = MakeSession(L"fork2-new");
        fork2.live = true;
        fork2.tabToken = L"wt-fork2";
        fork2.forkParentId = L"src2-parent";
        reg.Upsert(fork2);
    }
    reg.OnHookEvent(Msg(L"fork2-new", HookEvent::UserPromptSubmit)); // own-id CONTENT hook -> retires the guard AND the re-fork link
    CHECK(reg.Get(L"fork2-new") && reg.Get(L"fork2-new")->forkParentId.empty(), "fork-echo: own-id content hook clears the re-fork link");
    HookMessage late = Msg(L"src2-parent", HookEvent::SessionStart);
    late.cwd = L"K:/x";
    late.tabToken = L"wt-fork2";
    reg.OnHookEvent(late);
    CHECK(reg.Get(L"src2-parent").has_value(), "fork-echo: after the guard retires a source-id SessionStart is processed");

    // --- restart-refork hardening: the echo one-shot is the TRANSIENT forkEchoConsumed latch, NOT
    // forkParentId — retiring the echo no longer destroys the re-fork link a never-messaged fork needs,
    // and the content-less lifecycle hooks (SessionStart on a relaunch, SessionEnd at death) keep it. ---

    // (1) the suppressed echo consumes the latch; a SECOND source-id SessionStart on the SAME ConPTY is
    // a DELIBERATE in-session /resume and is processed — while the re-fork link survives.
    {
        SessionInfo fork3 = MakeSession(L"fork3-new");
        fork3.live = true;
        fork3.tabToken = L"wt-fork3";
        fork3.forkParentId = L"src3-parent";
        reg.Upsert(fork3);
    }
    HookMessage echo3 = Msg(L"src3-parent", HookEvent::SessionStart);
    echo3.cwd = L"K:/x";
    echo3.tabToken = L"wt-fork3";
    reg.OnHookEvent(echo3);
    CHECK(!reg.Get(L"src3-parent").has_value(), "fork-echo/latch: the startup echo is suppressed");
    CHECK(reg.Get(L"fork3-new") && reg.Get(L"fork3-new")->forkEchoConsumed, "fork-echo/latch: suppression consumed the one-shot latch");
    CHECK(reg.Get(L"fork3-new")->forkParentId == L"src3-parent", "fork-echo/latch: the re-fork link SURVIVES the suppressed echo");
    reg.OnHookEvent(echo3); // the same shape again == a deliberate /resume <src> typed in the fork's tab
    CHECK(reg.Get(L"src3-parent").has_value(), "fork-echo/latch: a post-echo source-id SessionStart is processed (deliberate /resume)");

    // (2) an own-id SessionEnd (the process dying — e.g. killed by an in-place restart) KEEPS the
    // re-fork link: the restart seam consults it moments later to re-fork the never-messaged fork
    // instead of silently replacing the branch with an empty fresh conversation.
    {
        SessionInfo fork4 = MakeSession(L"fork4-new");
        fork4.live = true;
        fork4.tabToken = L"wt-fork4";
        fork4.forkParentId = L"src4-parent";
        reg.Upsert(fork4);
    }
    HookMessage end4 = Msg(L"fork4-new", HookEvent::SessionEnd);
    end4.tabToken = L"wt-fork4"; // from the CURRENT host -> applies normally
    reg.OnHookEvent(end4);
    CHECK(reg.Get(L"fork4-new") && reg.Get(L"fork4-new")->state == SessionState::Done, "fork-refork: a current-host SessionEnd still applies (state Done)");
    CHECK(reg.Get(L"fork4-new")->forkParentId == L"src4-parent", "fork-refork: SessionEnd does NOT clear the re-fork link");

    // (3) an own-id SessionStart (a relaunch of the still-transcript-less fork) keeps the link too;
    // (4) a CONTENT hook retires it (the fork's transcript exists now -> resume, never re-fork).
    {
        SessionInfo fork5 = MakeSession(L"fork5-new");
        fork5.live = true;
        fork5.tabToken = L"wt-fork5";
        fork5.forkParentId = L"src5-parent";
        reg.Upsert(fork5);
    }
    HookMessage start5 = Msg(L"fork5-new", HookEvent::SessionStart);
    start5.tabToken = L"wt-fork5";
    reg.OnHookEvent(start5);
    CHECK(reg.Get(L"fork5-new") && reg.Get(L"fork5-new")->forkParentId == L"src5-parent", "fork-refork: an own-id SessionStart does NOT clear the re-fork link");
    reg.OnHookEvent(Msg(L"fork5-new", HookEvent::UserPromptSubmit));
    CHECK(reg.Get(L"fork5-new")->forkParentId.empty(), "fork-refork: a content hook (UserPromptSubmit) retires the re-fork link");

    // (5) restart supersede: a SessionEnd from a ConPTY that is NO LONGER the session's host (the
    // restart re-stamped tabToken to the NEW connection before the old process died) is DROPPED whole —
    // it must neither mark the freshly-restarted session Done nor steal tabToken back to the dead
    // ConPTY (which made the liveness sweep archive the live session seconds after its restart).
    {
        SessionInfo re = MakeSession(L"re-started", SessionState::Idle);
        re.live = true;
        re.tabToken = L"wt-new";
        reg.Upsert(re);
    }
    HookMessage lateEnd = Msg(L"re-started", HookEvent::SessionEnd);
    lateEnd.tabToken = L"wt-old"; // the SUPERSEDED host's dying breath
    reg.OnHookEvent(lateEnd);
    CHECK(reg.Get(L"re-started") && reg.Get(L"re-started")->state == SessionState::Idle, "stale-end: a superseded ConPTY's SessionEnd does not change state");
    CHECK(reg.Get(L"re-started")->tabToken == L"wt-new", "stale-end: tabToken is not stolen back by the dead host");
    HookMessage curEnd = Msg(L"re-started", HookEvent::SessionEnd);
    curEnd.tabToken = L"WT-NEW"; // the CURRENT host really died (case-insensitive token match)
    reg.OnHookEvent(curEnd);
    CHECK(reg.Get(L"re-started")->state == SessionState::Done, "stale-end: the current host's SessionEnd applies normally");
}

// Agentmaster (the summary's conversation chain — SessionInfo::continuedFromId): a /clear leaves no trace in
// any transcript, so SessionRegistry::OnHookEvent records it: the SessionStart whose source is "clear" is
// paired with the conversation whose SessionEnd(reason "clear") just arrived on the SAME ConPTY — else (that
// SessionEnd lost) with the ConPTY's other record, live before archived. Pins: the exact pairing beats a
// FRESHER record on the same tab (a child `claude -p` that inherited WT_SESSION); the fallback; a /resume, a
// fresh start and a /compact continue nothing; no candidate => no link; a ConPTY's /clear never pairs on
// another tab; an entry is consumed by its pairing; a SessionEnd for an id the registry never knew still
// pairs; the first writer wins (a /handover origin recorded before the hook stays).
void TestSummaryChainLinks()
{
    std::wprintf(L"SessionRegistry: /clear pairing for the summary's conversation chain:\n");
    const auto hook = [](const std::wstring& id, HookEvent ev, const std::wstring& source, const std::wstring& token) {
        HookMessage m = Msg(id, ev);
        m.cwd = L"K:/x";
        m.source = source;
        m.tabToken = token;
        return m;
    };
    const auto linkOf = [](const SessionRegistry& reg, const std::wstring& id) {
        const auto s = reg.Get(id);
        return s ? std::make_pair(s->continuedFromId, s->continuedVia) : std::make_pair(std::wstring{ L"<missing>" }, std::wstring{});
    };

    // (1) EXACT: the conversation the /clear ended — even beside a FRESHER live record on the same ConPTY.
    {
        SessionRegistry reg;
        SessionInfo a = MakeSession(L"clr-a");
        a.live = true;
        a.tabToken = L"wt-c1";
        reg.Upsert(a);
        reg.OnHookEvent(hook(L"clr-a", HookEvent::SessionEnd, L"clear", L"wt-c1"));
        SessionInfo child = MakeSession(L"clr-child"); // a `claude -p` a hook ran: same WT_SESSION, newest hook
        child.live = true;
        child.tabToken = L"wt-c1";
        child.lastHookUnixMs = NowMsTest() + 60'000;
        reg.Upsert(child);
        reg.OnHookEvent(hook(L"clr-b", HookEvent::SessionStart, L"clear", L"wt-c1"));
        CHECK(linkOf(reg, L"clr-b") == std::make_pair(std::wstring{ L"clr-a" }, std::wstring{ L"clear" }),
              "/clear pairing: the new conversation continues the one whose SessionEnd(clear) arrived on its ConPTY");
        CHECK(linkOf(reg, L"clr-a").first.empty() && linkOf(reg, L"clr-child").first.empty(),
              "/clear pairing: only the NEW conversation records a link");
        // The entry was consumed: a duplicate (twin) delivery of the same SessionStart changes nothing, and a
        // LATER /clear on that ConPTY with no SessionEnd seen falls back to the ConPTY's records.
        reg.OnHookEvent(hook(L"clr-b", HookEvent::SessionStart, L"clear", L"wt-c1"));
        CHECK(linkOf(reg, L"clr-b").first == L"clr-a", "/clear pairing: a duplicate SessionStart keeps the first link");
    }

    // (2) FALLBACK (the SessionEnd was lost): the ConPTY's LIVE record beats an archived earlier conversation.
    {
        SessionRegistry reg;
        SessionInfo old = MakeSession(L"fb-old");
        old.live = false;
        old.tabToken = L"wt-c2";
        old.lastHookUnixMs = NowMsTest() + 60'000; // fresher, yet archived
        reg.Upsert(old);
        SessionInfo cur = MakeSession(L"fb-cur");
        cur.live = true;
        cur.tabToken = L"wt-c2";
        reg.Upsert(cur);
        reg.OnHookEvent(hook(L"fb-new", HookEvent::SessionStart, L"clear", L"WT-C2")); // tokens match case-insensitively
        CHECK(linkOf(reg, L"fb-new") == std::make_pair(std::wstring{ L"fb-cur" }, std::wstring{ L"clear" }),
              "/clear pairing: with no SessionEnd(clear) seen, the ConPTY's live record (an archived one loses)");
    }

    // (3) Other sources continue nothing; no candidate => no link.
    {
        SessionRegistry reg;
        SessionInfo a = MakeSession(L"src-a");
        a.live = true;
        a.tabToken = L"wt-c3";
        reg.Upsert(a);
        reg.OnHookEvent(hook(L"src-resume", HookEvent::SessionStart, L"resume", L"wt-c3"));
        reg.OnHookEvent(hook(L"src-startup", HookEvent::SessionStart, L"startup", L"wt-c3"));
        reg.OnHookEvent(hook(L"src-compact", HookEvent::SessionStart, L"compact", L"wt-c3"));
        reg.OnHookEvent(hook(L"src-empty", HookEvent::SessionStart, L"", L"wt-c3"));
        CHECK(linkOf(reg, L"src-resume").first.empty() && linkOf(reg, L"src-startup").first.empty() &&
                  linkOf(reg, L"src-compact").first.empty() && linkOf(reg, L"src-empty").first.empty(),
              "/clear pairing: a /resume, a fresh start, a /compact and an old forwarder's empty source record no link");
        reg.OnHookEvent(hook(L"src-lonely", HookEvent::SessionStart, L"clear", L"wt-nobody"));
        CHECK(linkOf(reg, L"src-lonely").first.empty(), "/clear pairing: no record on the ConPTY => no link (never guess)");
        reg.OnHookEvent(hook(L"src-tokenless", HookEvent::SessionStart, L"clear", L""));
        CHECK(linkOf(reg, L"src-tokenless").first.empty(), "/clear pairing: a hook without a ConPTY token pairs with nothing");
    }

    // (4) Per ConPTY: one tab's /clear never pairs on another tab; a SessionEnd for an id the registry never
    // knew still pairs (the chain walk finds its transcript by id, not through the registry).
    {
        SessionRegistry reg;
        reg.OnHookEvent(hook(L"pc-unknown", HookEvent::SessionEnd, L"clear", L"wt-c4a")); // never registered
        reg.OnHookEvent(hook(L"pc-other-tab", HookEvent::SessionStart, L"clear", L"wt-c4b"));
        CHECK(linkOf(reg, L"pc-other-tab").first.empty(), "/clear pairing: a /clear on another ConPTY never takes this one's entry");
        reg.OnHookEvent(hook(L"pc-next", HookEvent::SessionStart, L"clear", L"wt-c4a"));
        CHECK(linkOf(reg, L"pc-next").first == L"pc-unknown", "/clear pairing: a SessionEnd(clear) pairs even for an id the registry never knew");
        // The entry is consumed; the next /clear on the tab without its SessionEnd falls back to the record the
        // previous /clear created (pc-next holds the ConPTY now).
        reg.OnHookEvent(hook(L"pc-after", HookEvent::SessionStart, L"clear", L"wt-c4a"));
        CHECK(linkOf(reg, L"pc-after").first == L"pc-next", "/clear pairing: a consumed entry is never reused; the fallback takes the ConPTY's record");
    }

    // (5) First writer wins: a /handover successor's origin, recorded at spawn, survives a later /clear hook.
    {
        SessionRegistry reg;
        SessionInfo a = MakeSession(L"fw-a");
        a.live = true;
        a.tabToken = L"wt-c5";
        reg.Upsert(a);
        SessionInfo b = MakeSession(L"fw-b");
        b.live = true;
        b.continuedFromId = L"fw-origin";
        b.continuedVia = L"handover";
        reg.Upsert(b);
        reg.OnHookEvent(hook(L"fw-a", HookEvent::SessionEnd, L"clear", L"wt-c5"));
        reg.OnHookEvent(hook(L"fw-b", HookEvent::SessionStart, L"clear", L"wt-c5"));
        CHECK(linkOf(reg, L"fw-b") == std::make_pair(std::wstring{ L"fw-origin" }, std::wstring{ L"handover" }),
              "/clear pairing: an existing link is never overwritten (the first writer wins)");
    }
}

// Agentmaster (RETIRED HOSTS — SessionRegistry::RetireHostToken; the 2026-08-26 deactivate unbind and the
// restart archives): once Deactivate/Restart replaces a session's connection, the CLOSING claude of the old
// one keeps reporting for a few seconds — the Fleet Observer still correlates it to the session through the
// not-yet-republished roster, and its hooks (a SessionEnd above all) still arrive. Every such report must be
// dropped: it would put the dead ConPTY's token, pid and heartbeat back on the record (the liveness sweep then
// archives the session) or move its state (SessionEnd -> Done). Pins: an observation and every hook kind from
// a retired (session, token) pair change NOTHING (not even provenance); the case-insensitive match; the
// retirement is per session; the NEW host is still heard; an empty token is never retired; Remove forgets
// the retirements; the per-session bound drops the oldest.
void TestRetiredHostToken()
{
    std::wprintf(L"SessionRegistry retired hosts (Deactivate/Restart: the closing claude cannot re-stamp the record):\n");
    SessionRegistry reg;
    {
        SessionInfo s = MakeSession(L"parked", SessionState::WaitingForInput);
        s.live = true;
        s.started = false; // the Deactivate park: a never-started connection
        s.tabToken = L"wt-new"; // the record already names the NEW connection
        s.pid = 0;
        reg.Upsert(s);
    }
    reg.RetireHostToken(L"parked", L"wt-old");
    CHECK(reg.IsRetiredHostToken(L"parked", L"wt-old"), "retired: the replaced host is retired");
    CHECK(reg.IsRetiredHostToken(L"parked", L"WT-OLD"), "retired: the token match is case-insensitive (hooks, the PEB and SessionId() format it differently)");
    CHECK(!reg.IsRetiredHostToken(L"parked", L"wt-new"), "retired: the current host is not retired");
    CHECK(!reg.IsRetiredHostToken(L"other", L"wt-old"), "retired: a retirement belongs to ONE session");
    CHECK(!reg.IsRetiredHostToken(L"parked", L""), "retired: an empty token is never retired");

    // (1) THE INCIDENT: the observer's survey, still on the pre-swap roster, reports the dying claude under
    // the OLD token. Merged, it put wt-old / the dead pid / its heartbeat back on the record.
    ObservedClaude o;
    o.sessionId = L"parked";
    o.tabToken = L"wt-old";
    o.pid = 4242;
    o.cwd = L"K:/x";
    o.runningApp = RunningApp::Agentmaster;
    o.presenceStatus = L"busy";
    o.observedUnixMs = NowMsTest();
    reg.ObserveClaude(o);
    auto s = reg.Get(L"parked");
    CHECK(s && s->tabToken == L"wt-new", "retired: the closing claude's observation does NOT re-stamp tabToken (the liveness sweep keeps finding the parked pane)");
    CHECK(s && s->pid == 0, "retired: ...nor its pid");
    CHECK(s && s->presenceStatus.empty(), "retired: ...nor its heartbeat (a stale \"busy\" would read the parked session as working)");

    // (2) its hooks — a SessionEnd would mark the parked tab Done, a prompt would light Running.
    for (const auto ev : { HookEvent::SessionEnd, HookEvent::Stop, HookEvent::Notification, HookEvent::UserPromptSubmit, HookEvent::PostToolUse })
    {
        HookMessage m = Msg(L"parked", ev);
        m.tabToken = L"wt-old";
        reg.OnHookEvent(m);
    }
    s = reg.Get(L"parked");
    CHECK(s && s->state == SessionState::WaitingForInput, "retired: no hook from the retired host moves the parked state (SessionEnd / Stop / Notification / UserPromptSubmit / PostToolUse all dropped)");
    CHECK(s && s->tabToken == L"wt-new", "retired: no hook from the retired host steals tabToken back");
    CHECK(s && !s->hookWired, "retired: a dropped hook leaves no provenance either (it was never applied)");

    // (3) the NEW host — the woken claude — is heard normally.
    HookMessage start = Msg(L"parked", HookEvent::SessionStart);
    start.tabToken = L"wt-new";
    reg.OnHookEvent(start);
    s = reg.Get(L"parked");
    CHECK(s && s->hookWired && s->tabToken == L"wt-new", "retired: the new host's hook is applied");
    o.tabToken = L"WT-NEW";
    o.pid = 5151;
    o.presenceStatus = L"idle";
    reg.ObserveClaude(o);
    s = reg.Get(L"parked");
    CHECK(s && s->pid == 5151, "retired: the new host's observation enriches normally");

    // (4) per session: the same token reported for ANOTHER session is not vetoed.
    {
        SessionInfo other = MakeSession(L"other", SessionState::Idle);
        other.live = true;
        other.tabToken = L"wt-elsewhere";
        reg.Upsert(other);
    }
    HookMessage otherStop = Msg(L"other", HookEvent::Stop);
    otherStop.tabToken = L"wt-old";
    reg.OnHookEvent(otherStop);
    CHECK(reg.Get(L"other") && reg.Get(L"other")->hookWired && reg.Get(L"other")->tabToken == L"wt-old", "retired: a token retired for one session still speaks for another");

    // (5) Remove forgets the session's retirements; an empty token retires nothing.
    reg.Remove(L"parked");
    CHECK(!reg.IsRetiredHostToken(L"parked", L"wt-old"), "retired: Remove forgets the session's retirements");
    reg.RetireHostToken(L"other", L"");
    CHECK(!reg.IsRetiredHostToken(L"other", L""), "retired: retiring an empty token is a no-op");

    // (6) the per-session bound: past kRetiredHostTokensPerSession the OLDEST retirement goes first.
    for (size_t i = 0; i < kRetiredHostTokensPerSession + 3; ++i)
    {
        reg.RetireHostToken(L"other", L"wt-r" + std::to_wstring(i));
    }
    CHECK(!reg.IsRetiredHostToken(L"other", L"wt-r0"), "retired: the oldest retirement is dropped past the per-session bound");
    CHECK(reg.IsRetiredHostToken(L"other", L"wt-r" + std::to_wstring(kRetiredHostTokensPerSession + 2)), "retired: the newest retirement is kept");
}

// Agentmaster (duplicate conversations, OBSERVER.md §20 — the 2026-10-11..14 re-bind loop): the pure host
// choice. One conversation ran in three tabs of the release (a /resume of an open conversation plus two
// Restart copies); the survey correlated all three to the session, the UI lane bound whichever it saw last,
// and the binding moved ~175,000 times in 2.8 days. The rules pinned here are what stop the move: the tab
// already bound keeps the conversation, and the same facts always give the same host, whatever order the
// survey lists the tabs in.
void TestConversationHosts()
{
    std::wprintf(L"Duplicate conversations: one host tab per conversation (ConversationHosts.h):\n");

    // ---- PickConversationHost ----------------------------------------------------------------------------
    {
        const std::vector<ConversationHostCandidate> none;
        CHECK(PickConversationHost(none) == 0, "pick: no candidate answers size() (0)");

        const std::vector<ConversationHostCandidate> one{ { L"wt-a", false, false, 0 } };
        CHECK(PickConversationHost(one) == 0, "pick: a single candidate is the host, whatever it knows");

        const std::vector<ConversationHostCandidate> boundVsRecorded{ { L"wt-a", false, true, 100 }, { L"wt-b", true, false, 200 } };
        CHECK(PickConversationHost(boundVsRecorded) == 1, "pick: the tab BOUND to the conversation beats the recorded host and an earlier start (rule 1)");

        const std::vector<ConversationHostCandidate> recordedVsEarlier{ { L"wt-a", false, false, 100 }, { L"wt-b", false, true, 200 } };
        CHECK(PickConversationHost(recordedVsEarlier) == 1, "pick: the registry's recorded host beats an earlier start (rule 2)");

        const std::vector<ConversationHostCandidate> earlier{ { L"wt-a", false, false, 300 }, { L"wt-b", false, false, 200 } };
        CHECK(PickConversationHost(earlier) == 1, "pick: the earlier-started claude wins when nothing is bound or recorded (rule 3)");

        const std::vector<ConversationHostCandidate> unknownLast{ { L"wt-a", false, false, 0 }, { L"wt-b", false, false, 500 } };
        CHECK(PickConversationHost(unknownLast) == 1, "pick: an unknown start (a denied PEB read) sorts after every known one");

        const std::vector<ConversationHostCandidate> tie{ { L"wt-b", false, false, 100 }, { L"wt-a", false, false, 100 } };
        CHECK(PickConversationHost(tie) == 1, "pick: a full tie goes to the lower WT_SESSION (deterministic)");

        const std::vector<ConversationHostCandidate> bothUnknown{ { L"wt-z", false, false, 0 }, { L"wt-m", false, false, 0 } };
        CHECK(PickConversationHost(bothUnknown) == 1, "pick: two unknown starts go to the lower WT_SESSION");

        // Order independence: every permutation of the same facts picks the same tab.
        std::vector<ConversationHostCandidate> base{ { L"wt-c", false, false, 300 }, { L"wt-a", false, true, 0 }, { L"wt-b", false, false, 100 } };
        std::vector<size_t> order{ 0, 1, 2 };
        bool allSame = true;
        do
        {
            std::vector<ConversationHostCandidate> perm;
            for (const auto i : order)
            {
                perm.push_back(base[i]);
            }
            const size_t host = PickConversationHost(perm);
            allSame = allSame && host < perm.size() && perm[host].wtSession == L"wt-a";
        } while (std::next_permutation(order.begin(), order.end()));
        CHECK(allSame, "pick: every order of the same candidates picks the same host (the recorded one here)");
    }

    // ---- FindDuplicateConversations ----------------------------------------------------------------------
    int lookups = 0;
    std::wstring recorded;
    const auto recordedHostOf = [&](const std::wstring&) {
        ++lookups;
        return recorded;
    };

    {
        lookups = 0;
        const auto out = FindDuplicateConversations(std::vector<ConversationClaim>{}, recordedHostOf);
        CHECK(out.empty() && lookups == 0, "find: no claims, no duplicates, and the registry is not asked");
    }
    {
        lookups = 0;
        const std::vector<ConversationClaim> claims{ { L"wt-a", L"s1", L"s1", 100 }, { L"wt-b", L"s2", L"", 200 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.empty() && lookups == 0, "find: one tab per conversation (the normal case) gives no entry and asks the registry nothing");
    }
    {
        lookups = 0;
        const std::vector<ConversationClaim> claims{ { L"wt-a", L"", L"", 100 }, { L"wt-b", L"", L"", 200 } };
        CHECK(FindDuplicateConversations(claims, recordedHostOf).empty(), "find: claims with no conversation id (never-prompted claudes) are never grouped");
    }
    {
        const std::vector<ConversationClaim> claims{ { L"", L"s1", L"", 100 }, { L"wt-b", L"s1", L"", 200 } };
        CHECK(FindDuplicateConversations(claims, recordedHostOf).empty(), "find: a claim with no WT_SESSION is ignored");
    }
    {
        const std::vector<ConversationClaim> claims{ { L"wt-a", L"s1", L"", 100 }, { L"WT-A", L"s1", L"", 100 } };
        CHECK(FindDuplicateConversations(claims, recordedHostOf).empty(), "find: the same tab listed twice (any case) is one tab, not a duplicate");
    }

    // The incident's shape: one conversation in three tabs; tab a is the one this window binds.
    const std::wstring sid = L"97eb07dd-0000-4000-8000-000000000000";
    {
        lookups = 0;
        recorded = L"wt-c"; // the record's token flipped between the copies; it last named c
        const std::vector<ConversationClaim> claims{ { L"wt-b", sid, L"", 100 }, { L"wt-c", sid, L"", 200 }, { L"wt-a", sid, sid, 300 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.size() == 1, "find: the incident gives one duplicated conversation");
        CHECK(out.size() == 1 && out[0].sessionId == sid, "find: ...naming its session id");
        CHECK(out.size() == 1 && out[0].hostWt == L"wt-a", "find: the tab already BOUND stays the host (no move while its claude runs the conversation)");
        CHECK(out.size() == 1 && out[0].duplicateWts == std::vector<std::wstring>({ L"wt-b", L"wt-c" }), "find: the other two tabs are its duplicates, in ascending order");
        CHECK(lookups == 1, "find: the registry is asked once per duplicated conversation");
    }
    {
        recorded = L"WT-C"; // a hook's token can differ in case from the roster's
        const std::vector<ConversationClaim> claims{ { L"wt-b", sid, L"", 100 }, { L"wt-c", sid, L"", 200 }, { L"wt-a", sid, L"", 300 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.size() == 1 && out[0].hostWt == L"wt-c", "find: with nothing bound, the recorded host wins (case-insensitive token match)");
        CHECK(out.size() == 1 && out[0].duplicateWts == std::vector<std::wstring>({ L"wt-a", L"wt-b" }), "find: ...and the rest are duplicates");
    }
    {
        recorded.clear();
        const std::vector<ConversationClaim> claims{ { L"wt-c", sid, L"", 200 }, { L"wt-b", sid, L"", 100 }, { L"wt-a", sid, L"", 300 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.size() == 1 && out[0].hostWt == L"wt-b", "find: with nothing bound or recorded, the earliest claude hosts");
    }
    {
        recorded.clear();
        const std::vector<ConversationClaim> claims{ { L"wt-a", L"s1", L"S1", 300 }, { L"wt-b", L"s1", L"", 100 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.size() == 1 && out[0].hostWt == L"wt-a", "find: the bound id matches the conversation case-insensitively");
    }
    {
        recorded.clear();
        const std::vector<ConversationClaim> claims{ { L"wt-a", L"s1", L"other", 100 }, { L"wt-b", L"s1", L"", 50 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.size() == 1 && out[0].hostWt == L"wt-b", "find: a tab bound to ANOTHER session does not count as bound to this one");
    }
    {
        recorded.clear();
        lookups = 0;
        const std::vector<ConversationClaim> claims{ { L"wt-1", L"zz", L"", 1 }, { L"wt-2", L"zz", L"", 2 }, { L"wt-3", L"aa", L"", 3 }, { L"wt-4", L"aa", L"", 4 }, { L"wt-5", L"mm", L"", 5 } };
        const auto out = FindDuplicateConversations(claims, recordedHostOf);
        CHECK(out.size() == 2, "find: two duplicated conversations beside a single one give two entries");
        CHECK(out.size() == 2 && out[0].sessionId == L"aa" && out[1].sessionId == L"zz", "find: the entries come in ascending session-id order (the log lines never depend on the roster's order)");
        CHECK(lookups == 2, "find: one registry lookup per duplicated conversation, none for the single one");
    }
}

// Agentmaster (duplicate conversations, OBSERVER.md §20): what the registry does with the survey's set. A
// duplicate tab's hooks and observations would write its token, pid and heartbeat over the host's — the
// liveness sweep then looked for the session in the wrong tab — and a SessionStart from it would move the
// session there. All of it is dropped; the host and every other session are heard as before.
void TestDuplicateConversationHosts()
{
    std::wprintf(L"SessionRegistry duplicate hosts (one conversation in two or more tabs):\n");
    SessionRegistry reg;
    {
        SessionInfo s = MakeSession(L"conv", SessionState::WaitingForInput);
        s.live = true;
        s.tabToken = L"wt-host";
        s.pid = 100;
        reg.Upsert(s);
    }
    reg.SetDuplicateConversationHosts({ DuplicateConversation{ L"conv", L"wt-host", { L"wt-dup" } } });
    CHECK(reg.IsDuplicateConversationHost(L"conv", L"wt-dup"), "dup: the duplicate tab is listed");
    CHECK(reg.IsDuplicateConversationHost(L"conv", L"WT-DUP"), "dup: the token match is case-insensitive");
    CHECK(!reg.IsDuplicateConversationHost(L"conv", L"wt-host"), "dup: the host is not a duplicate");
    CHECK(!reg.IsDuplicateConversationHost(L"other", L"wt-dup"), "dup: a duplicate belongs to ONE conversation");
    CHECK(!reg.IsDuplicateConversationHost(L"conv", L""), "dup: an empty token is never a duplicate");

    // (1) the duplicate's observation changes nothing.
    ObservedClaude o;
    o.sessionId = L"conv";
    o.tabToken = L"wt-dup";
    o.pid = 999;
    o.cwd = L"K:/x";
    o.runningApp = RunningApp::Agentmaster;
    o.presenceStatus = L"busy";
    o.observedUnixMs = NowMsTest();
    reg.ObserveClaude(o);
    auto s = reg.Get(L"conv");
    CHECK(s && s->tabToken == L"wt-host", "dup: the duplicate's observation does not move tabToken");
    CHECK(s && s->pid == 100, "dup: ...nor its pid");
    CHECK(s && s->presenceStatus.empty(), "dup: ...nor its heartbeat");

    // (2) none of its hooks applies: no state change, no token, no Typed row, no provenance.
    for (const auto ev : { HookEvent::SessionStart, HookEvent::Stop, HookEvent::Notification, HookEvent::PostToolUse, HookEvent::SessionEnd })
    {
        HookMessage m = Msg(L"conv", ev);
        m.tabToken = L"wt-dup";
        reg.OnHookEvent(m);
    }
    {
        HookMessage m = UPS(L"conv", L"typed in the duplicate tab");
        m.tabToken = L"WT-DUP";
        reg.OnHookEvent(m);
    }
    s = reg.Get(L"conv");
    CHECK(s && s->state == SessionState::WaitingForInput, "dup: no hook from the duplicate moves the state (SessionStart / Stop / Notification / PostToolUse / SessionEnd / UserPromptSubmit)");
    CHECK(s && s->tabToken == L"wt-host", "dup: no hook from the duplicate moves tabToken (a SessionStart would have re-homed the session)");
    CHECK(s && s->queue.empty(), "dup: the duplicate's prompt is not recorded as a Typed row");
    CHECK(s && !s->hookWired, "dup: a dropped hook leaves no provenance (it was never applied)");

    // (3) the host is heard.
    o.tabToken = L"wt-host";
    o.pid = 101;
    o.presenceStatus = L"idle";
    reg.ObserveClaude(o);
    s = reg.Get(L"conv");
    CHECK(s && s->pid == 101, "dup: the host's observation enriches normally");
    {
        HookMessage m = UPS(L"conv", L"typed in the host tab");
        m.tabToken = L"wt-host";
        reg.OnHookEvent(m);
    }
    s = reg.Get(L"conv");
    CHECK(s && s->state == SessionState::Running && s->hookWired, "dup: the host's UserPromptSubmit is applied (Running)");
    CHECK(s && s->queue.size() == 1, "dup: ...and recorded");

    // (4) the same tab speaks for ANOTHER conversation (it ran /clear or /resume into a session of its own).
    {
        SessionInfo other = MakeSession(L"other", SessionState::Idle);
        other.live = true;
        other.tabToken = L"wt-elsewhere";
        reg.Upsert(other);
    }
    {
        HookMessage m = Msg(L"other", HookEvent::SessionStart);
        m.tabToken = L"wt-dup";
        reg.OnHookEvent(m);
    }
    auto other = reg.Get(L"other");
    CHECK(other && other->hookWired && other->tabToken == L"wt-dup", "dup: a duplicate of one conversation still speaks for another");

    // (5) the published set is cleaned: the host listed as its own duplicate, and an empty id, are ignored.
    reg.SetDuplicateConversationHosts({ DuplicateConversation{ L"conv", L"wt-host", { L"wt-host", L"wt-dup" } }, DuplicateConversation{ L"", L"wt-x", { L"wt-y" } } });
    CHECK(!reg.IsDuplicateConversationHost(L"conv", L"wt-host"), "dup: a host listed among its own duplicates stays the host");
    CHECK(reg.IsDuplicateConversationHost(L"conv", L"wt-dup"), "dup: ...while the real duplicate is listed");
    CHECK(!reg.IsDuplicateConversationHost(L"", L"wt-y"), "dup: an entry with no session id is ignored");

    // (6) the set is REPLACED by every survey: once the duplicate is gone from it, its hooks count again.
    reg.SetDuplicateConversationHosts({});
    CHECK(!reg.IsDuplicateConversationHost(L"conv", L"wt-dup"), "dup: an empty set clears every duplicate");
    {
        HookMessage m = Msg(L"conv", HookEvent::SessionStart);
        m.tabToken = L"wt-dup";
        reg.OnHookEvent(m);
    }
    s = reg.Get(L"conv");
    CHECK(s && s->tabToken == L"wt-dup", "dup: a tab that is no duplicate any more is heard again (its SessionStart re-homes the session)");

    // (7) end to end with the pure half: the incident's set, published as the survey publishes it.
    const std::vector<ConversationClaim> claims{ { L"wt-b", L"conv", L"", 100 }, { L"wt-c", L"conv", L"", 200 }, { L"wt-a", L"conv", L"conv", 300 } };
    reg.SetDuplicateConversationHosts(FindDuplicateConversations(claims, [&reg](const std::wstring& id) { return reg.GetTabToken(id); }));
    CHECK(!reg.IsDuplicateConversationHost(L"conv", L"wt-a"), "dup e2e: the bound tab hosts");
    CHECK(reg.IsDuplicateConversationHost(L"conv", L"wt-b") && reg.IsDuplicateConversationHost(L"conv", L"wt-c"), "dup e2e: both copies are duplicates");
}

// Agentmaster (UI_THREAD.md §12): the Agent Manager page's registry-driven rebuild gap (RefreshPacing.h).
// The frozen release of 2026-10-14 rebuilt in ~4 s with a fixed 250 ms gap, so the UI thread was ~94% busy;
// the gap now follows the rebuild's cost.
void TestRefreshPacing()
{
    std::wprintf(L"Agent Manager refresh pacing (the adaptive gap):\n");
    static_assert(ManagerRefreshGapMs(4000) == kManagerRefreshMaxGapMs, "the gap is computable at compile time");
    CHECK(ManagerRefreshGapMs(0) == 250, "pacing: no rebuild yet -> the old 250 ms");
    CHECK(ManagerRefreshGapMs(-5) == 250, "pacing: a negative cost (a clock oddity) -> 250 ms");
    CHECK(ManagerRefreshGapMs(1) == 250 && ManagerRefreshGapMs(125) == 250, "pacing: a cheap rebuild keeps the 250 ms floor");
    CHECK(ManagerRefreshGapMs(126) == 252, "pacing: past the floor the gap is twice the cost");
    CHECK(ManagerRefreshGapMs(200) == 400, "pacing: 200 ms -> 400 ms");
    CHECK(ManagerRefreshGapMs(2499) == 4998, "pacing: just under the cap");
    CHECK(ManagerRefreshGapMs(2500) == 5000 && ManagerRefreshGapMs(4000) == 5000, "pacing: the frozen release's ~4 s rebuild -> the 5 s cap");
    CHECK(ManagerRefreshGapMs(INT64_MAX) == 5000, "pacing: a huge cost saturates instead of overflowing");
    CHECK(ManagerRefreshGapMs(INT64_MIN) == 250, "pacing: the most negative cost reads as no rebuild");
    bool monotonic = true;
    for (int64_t cost = 0; cost <= 10000; ++cost)
    {
        monotonic = monotonic && ManagerRefreshGapMs(cost) <= ManagerRefreshGapMs(cost + 1);
    }
    CHECK(monotonic, "pacing: a costlier rebuild never gets a shorter gap");

    CHECK(!ShouldLogSlowManagerRebuild(999, 100000, 0), "slow log: under 1 s is not slow");
    CHECK(ShouldLogSlowManagerRebuild(1000, 100000, 0), "slow log: 1 s is slow, and the first one is logged");
    CHECK(!ShouldLogSlowManagerRebuild(1500, 100000, 50000), "slow log: a second slow rebuild within the minute is not logged");
    CHECK(!ShouldLogSlowManagerRebuild(1500, 109999, 50000), "slow log: ...up to the last millisecond of the minute");
    CHECK(ShouldLogSlowManagerRebuild(1500, 110000, 50000), "slow log: a minute after the last line, the next slow rebuild logs again");
}

void TestTypedCapture()
{
    std::wprintf(L"Auto Testing: record typed messages + suppress our own echoes:\n");
    SessionRegistry reg;
    reg.Upsert(MakeSession(L"s1"));

    // 1. A prompt typed straight into the ConPTY (no matching queued prompt) is recorded as a
    //    Sent/Typed Auto-Testing entry, so the "messages already sent" summary is complete.
    reg.OnHookEvent(UPS(L"s1", L"hello there"));
    auto s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 1, "typed prompt recorded as a queue entry");
    CHECK(s && s->queue.size() == 1 && s->queue[0].origin == PromptOrigin::Typed, "typed prompt tagged Typed");
    CHECK(s && s->queue.size() == 1 && s->queue[0].status == PromptStatus::Sent, "typed prompt marked Sent");
    CHECK(s && s->queue.size() == 1 && s->queue[0].text == L"hello there", "typed prompt text captured");
    CHECK(s && s->queue.size() == 1 && s->queue[0].sentAtUnixMs != 0, "typed prompt timestamped");
    CHECK(s && s->state == SessionState::Running, "UserPromptSubmit still drives state -> Running");

    // 2. A Flight prompt WE injected (Sent, recent, not yet echoed) is NOT re-recorded when its
    //    UserPromptSubmit echo arrives — it is just marked echoed.
    reg.Update(L"s1", [&](SessionInfo& ss) {
        QueuedPrompt p;
        p.id = L"f1";
        p.text = L"run the build";
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Autorun;
        p.echoed = false;
        p.sentAtUnixMs = NowMsTest();
        ss.queue.push_back(p);
    });
    reg.OnHookEvent(UPS(L"s1", L"run the build")); // the echo of our injection
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 2, "echo of our injection NOT recorded as a new entry");
    bool f1Echoed = false;
    for (const auto& p : s->queue)
    {
        if (p.id == L"f1")
        {
            f1Echoed = p.echoed;
        }
    }
    CHECK(f1Echoed, "injected prompt marked echoed once consumed");

    // 3. The echo was consumed, so a SECOND identical submit (a human re-typing the text) IS
    //    recorded as Typed — the one-echo-per-injection guard does not over-swallow.
    reg.OnHookEvent(UPS(L"s1", L"run the build"));
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 3, "second identical submit recorded as a typed message");

    // 4. A stale Flight prompt (sent long ago) does not swallow a fresh identical human message.
    reg.Update(L"s1", [&](SessionInfo& ss) {
        QueuedPrompt p;
        p.id = L"old";
        p.text = L"stale text";
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Autorun;
        p.echoed = false;
        p.sentAtUnixMs = NowMsTest() - 60000; // a minute ago, outside the echo window
        ss.queue.push_back(p);
    });
    reg.OnHookEvent(UPS(L"s1", L"stale text"));
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 5, "fresh message past a stale prompt's window is recorded");

    // 5. An empty prompt body records nothing (defensive: a UPS with no text).
    reg.OnHookEvent(UPS(L"s1", L""));
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 5, "empty prompt body records nothing");

    // 6. Teammate/control PROTOCOL traffic is NOT recorded as Typed (the push-path noise gate —
    //    IsNoiseUserPrompt, the SAME filter the scanner's back-fill applies, so the two paths
    //    agree). A teammate-message delivery fires a REAL UserPromptSubmit on the lead (the exact
    //    on-disk wrapper shape, session bd0d5b2f repro), so the STATE transition must still run —
    //    only the Typed record is filtered.
    const std::wstring kTeammateWake =
        L"Another Claude session sent a message:\n"
        L"<teammate-message teammate_id=\"P3-docs\" color=\"yellow\">\n"
        L"{\"type\":\"idle_notification\",\"from\":\"P3-docs\",\"timestamp\":\"2026-07-07T05:11:33.000Z\"}\n"
        L"</teammate-message>";
    reg.Update(L"s1", [](SessionInfo& ss) { ss.state = SessionState::WaitingForInput; });
    reg.OnHookEvent(UPS(L"s1", kTeammateWake));
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 5, "teammate-message delivery NOT recorded as a Typed prompt");
    CHECK(s && s->state == SessionState::Running, "teammate-wake turn still drives state -> Running (a REAL turn; only the record is filtered)");

    // 7. The rest of the shared noise set is filtered at this seam too — the AGENT wake shapes
    //    included (<task-notification> = a background task/agent completing, 79 rows fingerprinted
    //    in the prod registry pre-gate; <agent-message from=...> = a background Agent reporting
    //    back, 2 rows fingerprinted in the dev registry; bare <teammate-message> = the older
    //    delivery strata, 486 corpus-wide).
    reg.OnHookEvent(UPS(L"s1", L"<command-name>/model</command-name>"));
    reg.OnHookEvent(UPS(L"s1", L"<task-notification>background task done</task-notification>"));
    reg.OnHookEvent(UPS(L"s1", L"<agent-message from=\"finder-reuse\">\n[{\"file\": \"a.h\", \"summary\": \"x\"}]"));
    reg.OnHookEvent(UPS(L"s1", L"<teammate-message teammate_id=\"system\">\n{\"type\":\"teammate_terminated\"}"));
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 5, "slash-command echo + task-notification + agent-message + bare teammate-message are all filtered from the Typed record");

    // 8. ...and a real human prompt right after the noise still records normally.
    reg.OnHookEvent(UPS(L"s1", L"now fix the flaky test"));
    s = reg.Get(L"s1");
    CHECK(s && s->queue.size() == 6 && s->queue.back().origin == PromptOrigin::Typed && s->queue.back().text == L"now fix the flaky test",
          "a real human prompt after teammate traffic is still recorded as Typed");
}

// Agentmaster (bounded queue history): TrimQueueHistory caps the RECORDED history without ever
// dropping queued WORK — only completed (Sent/Skipped/Failed) entries go, oldest first; Pending
// entries and entries some remaining entry still dependsOn are exempt (so the trim may leave the
// queue above the cap). The registry applies it at every append seam (typed capture / reconciler
// back-fill) and on Upsert, so an oversized persisted queue trims on load.
void TestQueueHistoryTrim()
{
    std::wprintf(L"TrimQueueHistory (bounded Auto-Testing history):\n");

    const auto mkSent = [](size_t n) {
        std::vector<QueuedPrompt> q(n);
        for (size_t i = 0; i < n; ++i)
        {
            q[i].id = L"p" + std::to_wstring(i);
            q[i].status = PromptStatus::Sent;
        }
        return q;
    };

    // 1. Below the cap: untouched.
    {
        auto q = mkSent(5);
        TrimQueueHistory(q, 10);
        CHECK(q.size() == 5, "below-cap queue untouched");
    }
    // 2. Over the cap: oldest COMPLETED entries drop first, down to the cap.
    {
        auto q = mkSent(8);
        TrimQueueHistory(q, 5);
        CHECK(q.size() == 5, "over-cap queue trimmed to the cap");
        CHECK(!q.empty() && q.front().id == L"p3", "oldest completed entries dropped first");
        CHECK(!q.empty() && q.back().id == L"p7", "newest entries kept");
    }
    // 3. Pending is WORK, not history — never dropped, even when that leaves the queue above the cap.
    {
        auto q = mkSent(6);
        for (auto& p : q)
        {
            p.status = PromptStatus::Pending;
        }
        q[1].status = PromptStatus::Sent; // the single trimmable entry
        TrimQueueHistory(q, 3);
        CHECK(q.size() == 5, "only the completed entry dropped; Pending never");
        bool p1Gone = true;
        for (const auto& p : q)
        {
            if (p.id == L"p1")
            {
                p1Gone = false;
            }
        }
        CHECK(p1Gone, "the one Sent entry was the one dropped");
    }
    // 4. dependsOn pins its target: a completed entry a remaining entry depends on survives.
    {
        auto q = mkSent(4);
        q[3].status = PromptStatus::Pending;
        q[3].dependsOn = L"p0";
        TrimQueueHistory(q, 2);
        bool p0Alive = false;
        for (const auto& p : q)
        {
            if (p.id == L"p0")
            {
                p0Alive = true;
            }
        }
        CHECK(p0Alive, "a depended-on entry survives the trim");
        CHECK(q.size() == 2, "the un-pinned completed entries (p1, p2) dropped");
    }
    // 5. The registry's typed-capture seam trims live: flood past the cap and the queue holds at the
    //    cap (every recorded entry is Sent/Typed history, so nothing is exempt).
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"trim"));
        const int total = static_cast<int>(kMaxQueueHistoryEntries) + 25;
        for (int i = 0; i < total; ++i)
        {
            reg.OnHookEvent(UPS(L"trim", L"prompt #" + std::to_wstring(i)));
        }
        const auto s = reg.Get(L"trim");
        CHECK(s && s->queue.size() == kMaxQueueHistoryEntries, "typed-capture seam holds the queue at the cap");
        CHECK(s && !s->queue.empty() && s->queue.back().text == L"prompt #" + std::to_wstring(total - 1), "newest capture kept");
        CHECK(s && !s->queue.empty() && s->queue.front().text == L"prompt #25", "oldest captures dropped");
    }
}

// Agentmaster (queue pop — Ctrl+Shift+Up on the empty compose box): TakeLastPendingPrompt pops the
// LAST still-queued (Pending / legacy Held) prompt out of the queue and returns it, skipping
// completed history (Sent/Skipped/Failed are records of delivered messages — never popped, so a
// racing send between keystroke and lock finds nothing). The UI runs it inside ONE registry Update;
// here the pure pop is pinned: order (last in QUEUE order), history preservation, LIFO drain, the
// Held belt, and the nothing-to-pop cases.
void TestTakeLastPendingPrompt()
{
    std::wprintf(L"TakeLastPendingPrompt (queue pop — the envelope's inverse):\n");

    const auto mk = [](const std::wstring& id, PromptStatus st, const std::wstring& text) {
        QueuedPrompt p;
        p.id = id;
        p.label = text.substr(0, 8);
        p.text = text;
        p.status = st;
        return p;
    };

    // 1. Empty queue: nothing to pop, queue untouched.
    {
        std::vector<QueuedPrompt> q;
        CHECK(!TakeLastPendingPrompt(q).has_value(), "empty queue pops nothing");
        CHECK(q.empty(), "empty queue stays empty");
    }
    // 2. The LAST Pending pops (queue order), with its text/id intact; earlier rows survive.
    {
        std::vector<QueuedPrompt> q{ mk(L"s0", PromptStatus::Sent, L"done"),
                                     mk(L"a", PromptStatus::Pending, L"first queued"),
                                     mk(L"b", PromptStatus::Pending, L"last queued") };
        const auto taken = TakeLastPendingPrompt(q);
        CHECK(taken && taken->id == L"b" && taken->text == L"last queued", "the LAST Pending pops, text whole");
        CHECK(q.size() == 2 && q[0].id == L"s0" && q[1].id == L"a", "history + the earlier Pending survive in order");
    }
    // 3. Completed history at the BACK is skipped over — the pop reaches the last real Pending.
    {
        std::vector<QueuedPrompt> q{ mk(L"a", PromptStatus::Pending, L"work"),
                                     mk(L"x", PromptStatus::Sent, L"sent"),
                                     mk(L"y", PromptStatus::Failed, L"failed") };
        const auto taken = TakeLastPendingPrompt(q);
        CHECK(taken && taken->id == L"a", "trailing Sent/Failed history is never popped — the Pending behind it is");
        CHECK(q.size() == 2 && q[0].id == L"x" && q[1].id == L"y", "the delivered-message records survive");
    }
    // 4. All-completed queue: nothing to pop (the race shape — the row went Sent under our feet).
    {
        std::vector<QueuedPrompt> q{ mk(L"x", PromptStatus::Sent, L"sent"),
                                     mk(L"y", PromptStatus::Skipped, L"skipped"),
                                     mk(L"z", PromptStatus::Failed, L"failed") };
        CHECK(!TakeLastPendingPrompt(q).has_value(), "an all-history queue pops nothing");
        CHECK(q.size() == 3, "nothing removed when nothing pops");
    }
    // 5. A legacy Held row (pre-rehabilitation sessions.json) still counts as queued work.
    {
        std::vector<QueuedPrompt> q{ mk(L"x", PromptStatus::Sent, L"sent"),
                                     mk(L"h", PromptStatus::Held, L"held work") };
        const auto taken = TakeLastPendingPrompt(q);
        CHECK(taken && taken->id == L"h", "legacy Held pops like Pending (still-queued work)");
    }
    // 6. Repeated pops drain LIFO — b, then a, then nothing (the multi-undo shape).
    {
        std::vector<QueuedPrompt> q{ mk(L"a", PromptStatus::Pending, L"one"),
                                     mk(L"b", PromptStatus::Pending, L"two") };
        const auto first = TakeLastPendingPrompt(q);
        const auto second = TakeLastPendingPrompt(q);
        CHECK(first && first->id == L"b" && second && second->id == L"a", "repeated pops walk newest -> oldest");
        CHECK(!TakeLastPendingPrompt(q).has_value() && q.empty(), "a drained queue pops nothing");
    }
}

// Fleet Observer O3 (OBSERVER.md §9): the provenance-aware PULL upsert. A claude observed
// out-of-band enriches its record (facts) but NEVER overrides hook-owned state; first sight
// creates an external+live record and fires adoption; a steady-state re-observe is a no-op.
void TestObserveClaude()
{
    std::wprintf(L"SessionRegistry::ObserveClaude (Fleet Observer pull-upsert + provenance):\n");
    SessionRegistry reg;
    std::atomic<int> observed{ 0 };
    std::atomic<int> adopted{ 0 };
    reg.AddObserver([&](const SessionInfo&, HookEvent) { observed.fetch_add(1); });
    reg.AddAdoptionHandler([&](const std::wstring&, const std::wstring&, const std::wstring&) { adopted.fetch_add(1); });

    // 1. First observe of an unknown id CREATES an external + live record and enriches it.
    ObservedClaude o;
    o.sessionId = L"obs-1";
    o.tabToken = L"wt-1";
    o.amSession = L"am-1";
    o.cwd = L"K:/proj";
    o.pid = 1234;
    o.runningApp = RunningApp::Agentmaster;
    o.model = L"opus";
    o.effort = L"high";
    o.ownerWindowId = L"win-7";
    o.observedUnixMs = 1000;
    reg.ObserveClaude(o);
    auto s = reg.Get(L"obs-1");
    CHECK(s.has_value(), "ObserveClaude creates a record on first sight");
    CHECK(s && s->external, "observed-into-existence record is external");
    CHECK(s && s->live, "observed record is live");
    CHECK(s && s->ownerWindowId == L"win-7", "ownerWindowId enriched (window attribution)");
    CHECK(s && s->pid == 1234 && s->liveCwd == L"K:/proj" && s->tabToken == L"wt-1", "facts enriched (pid/liveCwd/tabToken)");
    CHECK(s && s->model == L"opus" && s->effort == L"high" && s->amSession == L"am-1", "facts enriched (model/effort/amSession)");
    CHECK(s && s->runningApp == RunningApp::Agentmaster, "runningApp enriched");
    CHECK(s && s->workingDir == L"K:/proj", "workingDir filled from cwd when empty");
    CHECK(s && s->state == SessionState::Idle, "ObserveClaude does NOT set state (record defaults Idle)");
    CHECK(s && s->lastObservedUnixMs == 1000, "lastObservedUnixMs stamped");
    CHECK(adopted.load() == 1, "adoption fired for a newly-observed (un-Launched) session");
    const int obsAfterCreate = observed.load();
    CHECK(obsAfterCreate >= 1, "observer fired on creation");

    // 2. Re-observing identical facts (only the timestamp moves) is a NO-OP — no churn.
    o.observedUnixMs = 2000;
    reg.ObserveClaude(o);
    CHECK(adopted.load() == 1, "no re-adoption on an identical re-observe");
    CHECK(observed.load() == obsAfterCreate, "identical re-observe fires NO observer (no persist/UI churn)");
    CHECK(reg.Get(L"obs-1") && reg.Get(L"obs-1")->lastObservedUnixMs == 2000, "lastObservedUnixMs advances quietly even with no notify");

    // 3. Provenance: a hook drives state; a later observation must NOT clobber it.
    reg.OnHookEvent(UPS(L"obs-1", L"do the thing")); // push -> Running
    CHECK(reg.Get(L"obs-1") && reg.Get(L"obs-1")->state == SessionState::Running, "push hook set state Running");
    CHECK(reg.Get(L"obs-1") && reg.Get(L"obs-1")->hookWired, "hookWired set once a hook arrived");
    CHECK(reg.Get(L"obs-1") && reg.Get(L"obs-1")->lastHookUnixMs > 0, "lastHookUnixMs set by OnHookEvent");
    const int obsBeforeChange = observed.load();
    o.cwd = L"K:/proj2"; // a `cd` + relaunch
    o.model = L"sonnet";
    o.observedUnixMs = 3000;
    reg.ObserveClaude(o);
    s = reg.Get(L"obs-1");
    CHECK(s && s->liveCwd == L"K:/proj2" && s->model == L"sonnet", "a real fact change is merged");
    CHECK(s && s->workingDir == L"K:/proj", "workingDir (M-axis) is NOT moved by a live cd (liveCwd tracks it)");
    CHECK(s && s->state == SessionState::Running, "ObserveClaude did NOT clobber the hook-owned state (push wins)");
    CHECK(observed.load() > obsBeforeChange, "a genuine fact change fires the observer");

    // 4. An empty sessionId is a no-op (a correlated-but-never-prompted claude has no id yet).
    const auto countBefore = reg.Count();
    ObservedClaude noId;
    noId.cwd = L"K:/x";
    reg.ObserveClaude(noId);
    CHECK(reg.Count() == countBefore, "ObserveClaude no-ops for an empty sessionId");

    // 5. Adopt/re-home autorunner default (SetDefaultAutorunnerMode): a record the registry mints
    //    ITSELF — the observer first-sight create AND the hook SessionStart adopt create — carries the
    //    wired default MODE, so an adopted hand-typed claude / a /clear-minted re-home conversation is
    //    driven like any launched session instead of silently defaulting Off ("queued prompts stuck
    //    Pending, never sent"). CREATION-only: an existing record's mode is never touched.
    {
        // Un-wired (the harness / the CLI — and, until 2026-10-09, a plain release, whose engine did
        // not call SetDefaultAutorunnerMode): both create paths still mint Off.
        CHECK(reg.Get(L"obs-1") && reg.Get(L"obs-1")->autorunner.mode == AutorunnerMode::Off,
              "default-mode: un-wired registry minted Off (harness / CLI behavior unchanged)");

        reg.SetDefaultAutorunnerMode(AutorunnerMode::Full);

        ObservedClaude nu; // observer first-sight create AFTER wiring
        nu.sessionId = L"obs-def-full";
        nu.tabToken = L"wt-9";
        nu.cwd = L"K:/proj";
        nu.pid = 4321;
        nu.observedUnixMs = 9000;
        reg.ObserveClaude(nu);
        CHECK(reg.Get(L"obs-def-full") && reg.Get(L"obs-def-full")->autorunner.mode == AutorunnerMode::Full,
              "default-mode: observer first-sight create carries the wired default (Full)");

        // Hook SessionStart create (the adopt / re-home NEW-conversation-id path).
        reg.OnHookEvent(Msg(L"hook-def-full", HookEvent::SessionStart));
        CHECK(reg.Get(L"hook-def-full") && reg.Get(L"hook-def-full")->autorunner.mode == AutorunnerMode::Full,
              "default-mode: hook SessionStart create carries the wired default (Full)");

        // CREATION-only: obs-1 existed before the wiring (mode Off) — a later observe/enrich of it
        // must NOT re-stamp (the per-session toggle is the user's; the default applies at mint time).
        o.observedUnixMs = 9500;
        o.model = L"haiku"; // a genuine fact change so the merge path runs fully
        reg.ObserveClaude(o);
        CHECK(reg.Get(L"obs-1") && reg.Get(L"obs-1")->autorunner.mode == AutorunnerMode::Off,
              "default-mode: an EXISTING record's mode is never re-stamped by an observe");
    }
}

// Agentmaster (one ConPTY = one live conversation; Rule #14 / session-id divergence): a single
// claude.exe — one pid on one ConPTY (tabToken) — hosts exactly ONE conversation at a time. When it
// switches conversation id in-session (/clear, /resume — a /compact keeps its id, it is in-place), the OLD id lingered `live`, so two
// live records claimed one tab and the reconciler churned re-homing between them — flickering the tab
// header between the managed name and claude's live OSC title. ObserveClaude (which resolves the
// CURRENT conversation from claude's pid-keyed presence heartbeat) must archive the stale same-process
// sibling so the tab binds exactly one session: the newest.
void TestSupersedeStaleTabSiblings()
{
    std::wprintf(L"SessionRegistry: one ConPTY = one live conversation (session-id divergence):\n");
    const std::wstring token = L"WT-5cea3919";
    const uint32_t pid = 21680;

    // (1) Divergence: the SAME claude (pid 21680 on one ConPTY) switched conversation id. Observing the
    //     new (fresher) conversation archives the stale prior one that shared its ConPTY + process.
    {
        SessionRegistry reg;
        SessionInfo oldC;
        oldC.id = L"old-conv";
        oldC.live = true;
        oldC.pid = pid;
        oldC.tabToken = token;
        oldC.lastActivityUnixMs = 1000; // freshness 1000
        reg.Upsert(oldC);

        ObservedClaude o; // the new conversation, observed as current (newer transcript activity)
        o.sessionId = L"new-conv";
        o.tabToken = token;
        o.pid = pid;
        o.cwd = L"K:/dwh";
        o.lastActivityUnixMs = 2000; // freshness 2000 > 1000
        o.observedUnixMs = 5000;
        reg.ObserveClaude(o);

        CHECK(reg.Get(L"new-conv") && reg.Get(L"new-conv")->live, "divergence: the newest conversation stays live");
        CHECK(reg.Get(L"old-conv") && !reg.Get(L"old-conv")->live, "divergence: the stale same-pid conversation on the same ConPTY is archived");
    }

    // (2) A child sub-claude (a DIFFERENT pid that inherited the parent's WT_SESSION via env) is NEVER
    //     superseded — it is a real, concurrent process, not a stale conversation of the tab's claude.
    {
        SessionRegistry reg;
        SessionInfo child;
        child.id = L"sub-claude";
        child.live = true;
        child.pid = 99999; // a different process
        child.tabToken = token; // inherited the parent's WT_SESSION
        child.lastActivityUnixMs = 500;
        reg.Upsert(child);

        ObservedClaude o;
        o.sessionId = L"parent-conv";
        o.tabToken = token;
        o.pid = pid; // the tab's real claude
        o.lastActivityUnixMs = 2000;
        reg.ObserveClaude(o);

        CHECK(reg.Get(L"sub-claude") && reg.Get(L"sub-claude")->live, "a different-pid sub-claude sharing the WT_SESSION is NOT archived");
    }

    // (3) Newest wins: a mis-resolved / stale observation can NEVER archive a genuinely-FRESHER sibling.
    {
        SessionRegistry reg;
        SessionInfo current;
        current.id = L"actually-current";
        current.live = true;
        current.pid = pid;
        current.tabToken = token;
        current.lastActivityUnixMs = 9000; // genuinely fresher (its transcript is the one being written)
        reg.Upsert(current);

        ObservedClaude o; // the observer happened to resolve a STALE id this tick
        o.sessionId = L"stale-resolved";
        o.tabToken = token;
        o.pid = pid;
        o.lastActivityUnixMs = 2000; // older than the current
        reg.ObserveClaude(o);

        CHECK(reg.Get(L"actually-current") && reg.Get(L"actually-current")->live, "freshness guard: a stale observation does NOT archive the fresher current conversation");
    }

    // (4) The common case is untouched: observing the sole conversation of a tab archives nothing.
    {
        SessionRegistry reg;
        ObservedClaude o;
        o.sessionId = L"solo";
        o.tabToken = token;
        o.pid = pid;
        o.lastActivityUnixMs = 1000;
        reg.ObserveClaude(o);
        CHECK(reg.Get(L"solo") && reg.Get(L"solo")->live, "the sole conversation on a ConPTY stays live (no false supersede)");
        CHECK(reg.Count() == 1, "no spurious records created by a single observe");
    }
}

// Agentmaster (perf): SnapshotLive is the Manager lens / per-tick reconcile / dormant-count /
// keep-awake read — the LIVE subset in insertion order, byte-identical to filtering Snapshot() on
// s.live. The registry holds every archived record too (800+ in a real profile, each with its queue
// history), so the full snapshot's deep copy was pure waste on those paths.
void TestRegistrySnapshotLive()
{
    std::wprintf(L"SessionRegistry::SnapshotLive:\n");
    SessionRegistry reg;
    {
        SessionInfo a;
        a.id = L"arch-1";
        a.workingDir = L"C:\\a";
        a.live = false;
        QueuedPrompt p;
        p.id = L"p1";
        p.text = L"history";
        p.status = PromptStatus::Sent;
        a.queue.push_back(p);
        reg.Upsert(a);
    }
    {
        SessionInfo b;
        b.id = L"live-1";
        b.workingDir = L"C:\\b";
        b.live = true;
        b.started = true;
        reg.Upsert(b);
    }
    {
        SessionInfo c;
        c.id = L"arch-2";
        c.workingDir = L"C:\\c";
        c.live = false;
        reg.Upsert(c);
    }
    {
        SessionInfo d;
        d.id = L"live-2";
        d.workingDir = L"C:\\d";
        d.live = true;
        QueuedPrompt p;
        p.id = L"p2";
        p.text = L"queued";
        d.queue.push_back(p);
        reg.Upsert(d);
    }

    const auto all = reg.Snapshot();
    const auto live = reg.SnapshotLive();
    CHECK(all.size() == 4, "Snapshot returns every record (archived included)");
    CHECK(live.size() == 2, "SnapshotLive returns only the live records");
    CHECK(live.size() == 2 && live[0].id == L"live-1" && live[1].id == L"live-2", "SnapshotLive keeps insertion order");
    CHECK(live.size() == 2 && live[1].queue.size() == 1 && live[1].queue[0].text == L"queued", "SnapshotLive copies the live record's queue verbatim");
    {
        // equivalence with a filtered full snapshot
        std::vector<std::wstring> filtered;
        for (const auto& s : all)
        {
            if (s.live)
            {
                filtered.push_back(s.id);
            }
        }
        bool same = filtered.size() == live.size();
        for (size_t i = 0; same && i < live.size(); ++i)
        {
            same = (filtered[i] == live[i].id);
        }
        CHECK(same, "SnapshotLive == Snapshot filtered on live, same order");
    }

    // a flip is reflected by the next call (no caching inside the registry)
    reg.Update(L"arch-2", [](SessionInfo& s) { s.live = true; });
    CHECK(reg.SnapshotLive().size() == 3, "a record flipped live shows up in SnapshotLive");
    reg.Update(L"live-1", [](SessionInfo& s) { s.live = false; });
    const auto after = reg.SnapshotLive();
    CHECK(after.size() == 2 && after[0].id == L"arch-2" && after[1].id == L"live-2", "a record archived drops out, order preserved");
    CHECK(reg.Snapshot().size() == 4, "Snapshot still returns every record");
    CHECK(SessionRegistry{}.SnapshotLive().empty(), "empty registry -> empty live snapshot");
}

// Agentmaster (UI_THREAD.md §10a #4 — the lean registry reads): each lean read must return what its periodic
// caller reads and nothing it does not, and none may change the stored record. GetWithoutQueue and
// SnapshotLiveForScan move members OUT of the stored record for the length of a copy and back, so this test
// also proves the record reads the same after many such reads. The scanner predicates that read the queue
// (ScanNeedsFreshTail, ShouldWarnOnBoxNotVisible) must answer the same on the lean copy as on the full one.
void TestRegistryLeanReads()
{
    std::wprintf(L"SessionRegistry lean reads (UI_THREAD.md 10a #4):\n");
    // One queued prompt; a Sent one gets a send time, as the real send path stamps it.
    const auto prompt = [](const wchar_t* id, PromptStatus status, bool echoed, PromptOrigin origin) {
        QueuedPrompt p;
        p.id = id;
        p.label = id;
        p.text = std::wstring{ L"text of " } + id;
        p.status = status;
        p.echoed = echoed;
        p.origin = origin;
        p.sentAtUnixMs = (status == PromptStatus::Sent) ? 1000 : 0;
        return p;
    };
    // A Codex session record for the ManagedCodexTabTokens filter.
    const auto codex = [](const wchar_t* id, bool live, bool external, const wchar_t* token) {
        SessionInfo s;
        s.id = id;
        s.kind = AgentKind::Codex;
        s.live = live;
        s.external = external;
        s.tabToken = token;
        return s;
    };

    SessionRegistry reg;
    {
        SessionInfo a;
        a.id = L"arch";
        a.live = false;
        a.tabToken = L"TOK-ARCH";
        a.queue.push_back(prompt(L"h0", PromptStatus::Sent, true, PromptOrigin::Autorun));
        reg.Upsert(a);
    }
    {
        SessionInfo s;
        s.id = L"live-a";
        s.title = L"Title A";
        s.workingDir = L"C:\\a";
        s.tabToken = L"TOK-A";
        s.live = true;
        s.started = true;
        s.state = SessionState::WaitingForInput;
        s.lastAssistantText = L"a long assistant reply";
        s.recap = L"the recap";
        s.pendingInput = L"an unsent draft";
        s.pendingInputUnixMs = 1234;
        s.pendingPasteRefs = L"paste #1 -> unresolved";
        s.presenceStatus = L"idle";
        s.pid = 4242;
        s.autorunner.mode = AutorunnerMode::Full;
        s.pendingBoxState = InputBoxState::NoBox;
        s.pendingBoxStateUnixMs = 1; // long ago: the box warning's window has elapsed
        s.queue.push_back(prompt(L"done", PromptStatus::Sent, true, PromptOrigin::Autorun)); // completed history
        s.queue.push_back(prompt(L"typed", PromptStatus::Sent, true, PromptOrigin::Typed)); // completed history
        s.queue.push_back(prompt(L"pend", PromptStatus::Pending, false, PromptOrigin::Autorun)); // open
        s.queue.push_back(prompt(L"held", PromptStatus::Held, false, PromptOrigin::Autorun)); // open (legacy)
        s.queue.push_back(prompt(L"flight", PromptStatus::Sent, false, PromptOrigin::Autorun)); // open: no echo yet
        s.queue.push_back(prompt(L"failed", PromptStatus::Failed, false, PromptOrigin::Autorun)); // completed history
        s.queue.push_back(prompt(L"skipped", PromptStatus::Skipped, false, PromptOrigin::Autorun)); // completed history
        reg.Upsert(s);
    }
    {
        SessionInfo b;
        b.id = L"live-b";
        b.workingDir = L"C:\\b";
        b.live = true; // no host token yet
        b.queue.push_back(prompt(L"b-done", PromptStatus::Sent, true, PromptOrigin::Typed));
        reg.Upsert(b);
    }
    reg.Upsert(codex(L"codex-managed", true, false, L"TOK-C"));
    reg.Upsert(codex(L"codex-external", true, true, L"TOK-X"));
    reg.Upsert(codex(L"codex-archived", false, false, L"TOK-Z"));
    reg.Upsert(codex(L"codex-notoken", true, false, L""));

    const auto fullA = reg.Get(L"live-a");
    CHECK(fullA && fullA->queue.size() == 7, "fixture: the stored record holds all 7 prompts");

    // --- GetWithoutQueue: everything but the queue ---
    const auto noQueue = reg.GetWithoutQueue(L"live-a");
    CHECK(noQueue && noQueue->queue.empty(), "GetWithoutQueue: the queue comes back empty");
    CHECK(noQueue && noQueue->title == L"Title A" && noQueue->workingDir == L"C:\\a" && noQueue->tabToken == L"TOK-A" &&
              noQueue->state == SessionState::WaitingForInput && noQueue->pid == 4242 && noQueue->started,
          "GetWithoutQueue: every other field as Get copies it");
    CHECK(noQueue && noQueue->lastAssistantText == L"a long assistant reply" && noQueue->recap == L"the recap" &&
              noQueue->pendingInput == L"an unsent draft" && noQueue->pendingPasteRefs == L"paste #1 -> unresolved",
          "GetWithoutQueue: the display texts are KEPT (only the scan read leaves them out)");
    CHECK(!reg.GetWithoutQueue(L"nope").has_value(), "GetWithoutQueue: unknown id -> nullopt");

    // --- GetTabToken: the token alone ---
    CHECK(reg.GetTabToken(L"live-a") == L"TOK-A", "GetTabToken: the stored token");
    CHECK(reg.GetTabToken(L"live-b").empty(), "GetTabToken: a session no hook tied to a ConPTY reads empty");
    CHECK(reg.GetTabToken(L"nope").empty(), "GetTabToken: an unknown id reads empty (like the null Get it replaced)");
    CHECK(reg.GetTabToken(L"arch") == L"TOK-ARCH", "GetTabToken: an archived record still answers (Get did too)");

    // --- LiveTabBindings: the SnapshotLive set and order, three fields ---
    const auto liveFull = reg.SnapshotLive();
    const auto bindings = reg.LiveTabBindings();
    bool sameBindings = bindings.size() == liveFull.size();
    for (size_t i = 0; sameBindings && i < bindings.size(); ++i)
    {
        sameBindings = bindings[i].id == liveFull[i].id && bindings[i].workingDir == liveFull[i].workingDir &&
                       bindings[i].tabToken == liveFull[i].tabToken;
    }
    CHECK(sameBindings, "LiveTabBindings: one binding per live session, SnapshotLive order, the same three fields");
    CHECK(bindings.size() == 5 && bindings[0].id == L"live-a" && bindings[1].id == L"live-b" && bindings[1].tabToken.empty(),
          "LiveTabBindings: a live session without a token is included, so the size is the live count");
    bool bindsArchived = false;
    for (const auto& b : bindings)
    {
        bindsArchived = bindsArchived || b.id == L"arch" || b.id == L"codex-archived";
    }
    CHECK(!bindsArchived, "LiveTabBindings: archived records are left out");

    // --- ManagedCodexTabTokens: live + ours + tokened Codex only ---
    const auto codexTokens = reg.ManagedCodexTabTokens();
    CHECK(codexTokens.size() == 1 && codexTokens[0] == L"TOK-C",
          "ManagedCodexTabTokens: an external, an archived, a token-less Codex and every Claude session are left out");

    // --- SnapshotLiveForScan: open prompts only, four texts empty, the rest as SnapshotLive ---
    const auto scan = reg.SnapshotLiveForScan();
    bool sameOrder = scan.size() == liveFull.size();
    for (size_t i = 0; sameOrder && i < scan.size(); ++i)
    {
        sameOrder = scan[i].id == liveFull[i].id;
    }
    CHECK(sameOrder, "SnapshotLiveForScan: the SnapshotLive set and order");
    const SessionInfo* leanA = nullptr;
    const SessionInfo* leanB = nullptr;
    for (const auto& s : scan)
    {
        leanA = (s.id == L"live-a") ? &s : leanA;
        leanB = (s.id == L"live-b") ? &s : leanB;
    }
    const SessionInfo* fullLiveA = nullptr;
    for (const auto& s : liveFull)
    {
        fullLiveA = (s.id == L"live-a") ? &s : fullLiveA;
    }
    CHECK(leanA && leanA->queue.size() == 3 && leanA->queue[0].id == L"pend" && leanA->queue[1].id == L"held" &&
              leanA->queue[2].id == L"flight",
          "SnapshotLiveForScan: the queue keeps the open prompts only (Pending, Held, Sent-unechoed), in queue order");
    CHECK(leanA && leanA->queue.size() == 3 && leanA->queue[2].text == L"text of flight" && leanA->queue[2].sentAtUnixMs == 1000 &&
              !leanA->queue[2].echoed && leanA->queue[2].origin == PromptOrigin::Autorun,
          "SnapshotLiveForScan: an open prompt is copied whole");
    CHECK(leanA && leanA->lastAssistantText.empty() && leanA->recap.empty() && leanA->pendingInput.empty() && leanA->pendingPasteRefs.empty(),
          "SnapshotLiveForScan: lastAssistantText, recap, pendingInput and pendingPasteRefs come back empty");
    CHECK(leanA && leanA->title == L"Title A" && leanA->tabToken == L"TOK-A" && leanA->presenceStatus == L"idle" && leanA->pid == 4242 &&
              leanA->pendingInputUnixMs == 1234 && leanA->autorunner.mode == AutorunnerMode::Full &&
              leanA->pendingBoxState == InputBoxState::NoBox && leanA->state == SessionState::WaitingForInput,
          "SnapshotLiveForScan: every other field as SnapshotLive copies it");
    CHECK(leanA && fullLiveA && ScanNeedsFreshTail(*leanA) && ScanNeedsFreshTail(*leanA) == ScanNeedsFreshTail(*fullLiveA),
          "ScanNeedsFreshTail reads the same on the lean copy (the unechoed delivery is kept)");
    const int64_t now = NowMsTest();
    CHECK(leanA && fullLiveA && ShouldWarnOnBoxNotVisible(*leanA, now) && ShouldWarnOnBoxNotVisible(*leanA, now) == ShouldWarnOnBoxNotVisible(*fullLiveA, now),
          "ShouldWarnOnBoxNotVisible reads the same on the lean copy (the waiting prompt is kept)");
    CHECK(leanB && leanB->queue.empty() && !ScanNeedsFreshTail(*leanB),
          "SnapshotLiveForScan: a queue of completed history only scans as empty, and not hot");

    // --- SnapshotLiveForCards (SIDE_PANEL.md 2c): no queue, no transcript texts, the draft KEPT ---
    const auto cards = reg.SnapshotLiveForCards();
    bool cardsOrder = cards.size() == liveFull.size();
    for (size_t i = 0; cardsOrder && i < cards.size(); ++i)
    {
        cardsOrder = cards[i].id == liveFull[i].id;
    }
    CHECK(cardsOrder, "SnapshotLiveForCards: the SnapshotLive set and order");
    const SessionInfo* cardA = nullptr;
    for (const auto& s : cards)
    {
        cardA = (s.id == L"live-a") ? &s : cardA;
    }
    CHECK(cardA && cardA->queue.empty(), "SnapshotLiveForCards: the queue comes back empty (a card shows no queue)");
    CHECK(cardA && cardA->lastAssistantText.empty() && cardA->recap.empty() && cardA->pendingPasteRefs.empty(),
          "SnapshotLiveForCards: lastAssistantText, recap and pendingPasteRefs come back empty");
    CHECK(cardA && cardA->pendingInput == L"an unsent draft",
          "SnapshotLiveForCards: pendingInput is KEPT (a card shows the unsent-draft dots)");
    CHECK(cardA && cardA->title == L"Title A" && cardA->workingDir == L"C:\\a" && cardA->tabToken == L"TOK-A" &&
              cardA->state == SessionState::WaitingForInput && cardA->started && cardA->pid == 4242 &&
              cardA->pendingInputUnixMs == 1234 && cardA->presenceStatus == L"idle",
          "SnapshotLiveForCards: every other field as SnapshotLive copies it");
    bool cardsHoldArchived = false;
    for (const auto& s : cards)
    {
        cardsHoldArchived = cardsHoldArchived || s.id == L"arch" || s.id == L"codex-archived";
    }
    CHECK(!cardsHoldArchived, "SnapshotLiveForCards: archived records are left out");

    // --- every lean read leaves the stored record intact ---
    for (int i = 0; i < 3; ++i)
    {
        (void)reg.SnapshotLiveForScan();
        (void)reg.SnapshotLiveForCards();
        (void)reg.GetWithoutQueue(L"live-a");
    }
    const auto afterA = reg.Get(L"live-a");
    bool queueIntact = afterA && fullA && afterA->queue.size() == fullA->queue.size();
    for (size_t i = 0; queueIntact && i < afterA->queue.size(); ++i)
    {
        queueIntact = afterA->queue[i].id == fullA->queue[i].id && afterA->queue[i].text == fullA->queue[i].text &&
                      afterA->queue[i].status == fullA->queue[i].status && afterA->queue[i].echoed == fullA->queue[i].echoed;
    }
    CHECK(queueIntact, "after repeated lean reads the stored queue is intact (parked and put back each time)");
    CHECK(afterA && afterA->lastAssistantText == L"a long assistant reply" && afterA->recap == L"the recap" &&
              afterA->pendingInput == L"an unsent draft" && afterA->pendingPasteRefs == L"paste #1 -> unresolved",
          "after repeated lean reads the stored display texts are intact");
    const auto liveAgain = reg.SnapshotLive();
    CHECK(!liveAgain.empty() && liveAgain[0].id == L"live-a" && liveAgain[0].queue.size() == 7 && liveAgain[0].pendingInput == L"an unsent draft",
          "SnapshotLive still copies whole records");

    // --- HeadlinesOf (tags perf, 2026-10-10; started / external since 2026-10-24): seven fields per ASKED id, live and archived alike ---
    // Last among the reads on this fixture: it stamps activity onto the records first, and the checks
    // above compare a lean copy with a full one taken earlier.
    {
        // Two activity stamps on the live record, the transcript-derived one the newer; the archived
        // record gets only the hook-side one.
        reg.Update(L"live-a", [](SessionInfo& s) {
            s.lastActivityUnixMs = 5000;
            s.convLastActivityUnixMs = 9000;
        });
        reg.Update(L"arch", [](SessionInfo& s) {
            s.title = L"Archived One";
            s.lastActivityUnixMs = 7000;
        });
        const std::vector<std::wstring> asked{ L"arch", L"nope", L"live-a", L"live-b", L"live-a" };
        const auto heads = reg.HeadlinesOf(asked);
        CHECK(heads.size() == 4, "HeadlinesOf: one headline per KNOWN asked id (the unknown id is left out, a repeated id answered twice)");
        CHECK(heads.size() == 4 && heads[0].id == L"arch" && heads[1].id == L"live-a" && heads[2].id == L"live-b" && heads[3].id == L"live-a",
              "HeadlinesOf: in the order asked");
        CHECK(heads.size() == 4 && heads[0].title == L"Archived One" && !heads[0].live && heads[0].lastActivityUnixMs == 7000,
              "HeadlinesOf: an ARCHIVED record answers too (a closed session can still carry a tag), with its hook-side activity");
        CHECK(heads.size() == 4 && heads[1].title == L"Title A" && heads[1].live && heads[1].state == SessionState::WaitingForInput,
              "HeadlinesOf: title, live and state as stored");
        CHECK(heads.size() == 4 && heads[1].lastActivityUnixMs == 9000,
              "HeadlinesOf: the activity is the NEWER of the hook-side and the transcript-derived stamp");
        CHECK(heads.size() == 4 && heads[2].title.empty() && heads[2].lastActivityUnixMs == 0,
              "HeadlinesOf: an untitled session with no recorded activity reads empty / 0");
        // started / external (2026-10-24, "Deactivate All Idle Tabs"): live-a's terminal started, live-b has none yet (a
        // dormant tab), and codex-external is an adopted session — the three cases the idle scope's filter tells apart.
        const std::vector<std::wstring> askedFlags{ L"live-a", L"live-b", L"codex-external" };
        const auto flags = reg.HeadlinesOf(askedFlags);
        CHECK(flags.size() == 3 && flags[0].started && !flags[0].external, "HeadlinesOf: started as stored (a running tab), not adopted");
        CHECK(flags.size() == 3 && !flags[1].started && !flags[1].external, "HeadlinesOf: a dormant tab reads started == false");
        CHECK(flags.size() == 3 && flags[2].external, "HeadlinesOf: an ADOPTED session reads external == true");
        const std::vector<std::wstring> nothing;
        const std::vector<std::wstring> unknownOnly{ L"nope" };
        CHECK(reg.HeadlinesOf(nothing).empty() && reg.HeadlinesOf(unknownOnly).empty(), "HeadlinesOf: nothing asked, or nothing known -> empty");
        const auto stillFull = reg.Get(L"live-a");
        CHECK(stillFull && stillFull->queue.size() == 7 && stillFull->pendingInput == L"an unsent draft",
              "HeadlinesOf: the stored record is untouched");
    }

    // --- an empty registry ---
    SessionRegistry empty;
    CHECK(empty.LiveTabBindings().empty() && empty.ManagedCodexTabTokens().empty() && empty.SnapshotLiveForScan().empty() &&
              empty.SnapshotLiveForCards().empty() && !empty.GetWithoutQueue(L"x").has_value() && empty.GetTabToken(L"x").empty(),
          "empty registry: every lean read is empty");
    const std::vector<std::wstring> askedOfEmpty{ L"x", L"live-a" };
    CHECK(empty.HeadlinesOf(askedOfEmpty).empty(), "empty registry: HeadlinesOf is empty too");
}

