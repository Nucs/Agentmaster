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
        TurnAccounting t;
        auto r1 = NextSessionStateOrdered(SessionState::Idle, at(HookEvent::UserPromptSubmit, 1000), t);
        auto r2 = NextSessionStateOrdered(r1.state, at(HookEvent::Stop, 1500), t); // turn 1 done
        CHECK(r2.state == SessionState::WaitingForInput, "ordered: clean Stop -> Waiting");
        auto r3 = NextSessionStateOrdered(r2.state, at(HookEvent::UserPromptSubmit, 2000), t); // turn 2
        CHECK(r3.state == SessionState::Running, "ordered: turn-2 UPS -> Running");
        auto r4 = NextSessionStateOrdered(r3.state, at(HookEvent::Stop, 1400), t); // turn 1's LATE duplicate
        CHECK(r4.state == SessionState::Running && r4.staleStop && !r4.turnComplete, "ordered: stale Stop (ts < newest prompt) keeps Running");
        auto r5 = NextSessionStateOrdered(r4.state, at(HookEvent::Stop, 2500), t); // turn 2's real end
        CHECK(r5.state == SessionState::WaitingForInput && r5.turnComplete, "ordered: fresh Stop after the stale one completes the turn");
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
    { // The cap bounds a pathological type-ahead burst (collapse-on-Stop bounds drift anyway).
        TurnAccounting t;
        for (int i = 0; i < kMaxQueuedPrompts + 5; ++i)
        {
            NextSessionStateOrdered(SessionState::Running, at(HookEvent::UserPromptSubmit, 1000 + i), t);
        }
        CHECK(t.queuedPrompts == kMaxQueuedPrompts, "ordered: queuedPrompts capped");
    }
    { // API error: -> Error, settles type-ahead like a Stop, but is NOT a clean turn boundary
      // (turnComplete stays false -> no autopilot advance off an error; the scheduler's stopOnError pauses).
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

        auto stop2 = at(HookEvent::Stop, 4000);
        stop2.sessionId = L"ord-1";
        stop2.lastMessageIsQuestion = true;
        reg.OnHookEvent(stop2);
        CHECK(reg.Get(L"ord-1")->state == SessionState::WaitingForInput, "registry: final Stop -> Waiting");
        CHECK(reg.Get(L"ord-1")->lastMessageWasQuestion, "registry: a FRESH Stop's question bit applies");
        CHECK(advances.load() == 1, "registry: exactly one advance for the whole batch");
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
        CHECK(reg.SetPendingInput(L"s1", L""), "pending: clear flips (non-empty -> empty)");
        CHECK(reg.Get(L"s1")->pendingInput.empty(), "pending: cleared");
        CHECK(observed.load() == base + 2, "pending: clear NOTIFIES (boolean flip)");
        CHECK(!reg.SetPendingInput(L"nope", L"x"), "pending: unknown id no-op");
        CHECK(observed.load() == base + 2, "pending: unknown id does not notify");
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
// on the SAME ConPTY (the eagerly-stamped tabToken). It is one-shot: cleared once the fork emits under
// its own id, so a LATER deliberate /resume back to <src> re-homes normally.
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
    reg.OnHookEvent(Msg(L"fork2-new", HookEvent::UserPromptSubmit)); // own-id hook -> retires the guard
    CHECK(reg.Get(L"fork2-new") && reg.Get(L"fork2-new")->forkParentId.empty(), "fork-echo: own-id hook clears the one-shot guard");
    HookMessage late = Msg(L"src2-parent", HookEvent::SessionStart);
    late.cwd = L"K:/x";
    late.tabToken = L"wt-fork2";
    reg.OnHookEvent(late);
    CHECK(reg.Get(L"src2-parent").has_value(), "fork-echo: after the guard retires a source-id SessionStart is processed");
}

void TestTypedCapture()
{
    std::wprintf(L"Flight Plan: record typed messages + suppress our own echoes:\n");
    SessionRegistry reg;
    reg.Upsert(MakeSession(L"s1"));

    // 1. A prompt typed straight into the ConPTY (no matching queued prompt) is recorded as a
    //    Sent/Typed Flight-Plan entry, so the "messages already sent" summary is complete.
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
        p.origin = PromptOrigin::Flight;
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
        p.origin = PromptOrigin::Flight;
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
}

// Agentmaster (one ConPTY = one live conversation; Rule #14 / session-id divergence): a single
// claude.exe — one pid on one ConPTY (tabToken) — hosts exactly ONE conversation at a time. When it
// switches conversation id in-session (/clear, /compact, /resume), the OLD id lingered `live`, so two
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

