// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — M5 standalone test harness.
//
// Not part of the msbuild. Compiles the engine TUs unity-style WITHOUT the TerminalApp
// PCH (AGENTMASTER_STANDALONE_TEST) so the pure logic + the real named-pipe transport can
// be verified from the command line:
//
//   cl /std:c++20 /EHsc /nologo /Fe:m5_tests.exe m5_tests.cpp ole32.lib && ./m5_tests.exe
//
// Covers: the hook state machine (Correctness Rule #1), wire parsing/round-trip, the
// SessionRegistry transitions + observer/advance seams + injector, the spawn-recipe pure
// builders, and an end-to-end HooksBridge pipe round-trip.

#define AGENTMASTER_STANDALONE_TEST
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

// Headers only; the engine .cpp TUs are compiled separately and linked (see
// run-m5-tests.bat) so each keeps its own anonymous-namespace helpers.
#include "../Activity.h"
#include "../ClaudeSpawn.h"
#include "../HookWire.h"
#include "../HooksBridge.h"
#include "../Json.h"
#include "../Persistence.h"
#include "../PendingInput.h" // the unsent-draft detector (PENDING_INPUT.md) — pure, header-only
#include "../ProcessInspect.h" // SnapshotProcesses / ReadClaudeFacts / ResolveSessionId (Observer O1)
#include "../PromptAnchor.h" // the summary-panel JUMP resolver (SUMMARY_JUMP.md) — pure, benchmarked here
#include "../ProfileBootstrap.h" // the per-install state PROFILE (choice file / resolution / migrate)
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"
#include "../SessionScanner.h" // ParseTranscriptDelta (pure)
#include "../SessionSearch.h" // the Sessions page's two-phase search (SESSIONS.md §6)
#include "../SessionStore.h" // the generalized DURABLE per-session key/value store (titles, ...)
#include "../TranscriptStore.h" // the on-disk Claude-session store API (SESSIONS.md §6)

using namespace Agentmaster;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                      \
    do                                                                        \
    {                                                                         \
        ++g_checks;                                                           \
        if (!(cond))                                                          \
        {                                                                     \
            ++g_failures;                                                     \
            std::wprintf(L"  [FAIL] %S\n", msg);                              \
        }                                                                     \
    } while (0)

static SessionInfo MakeSession(const std::wstring& id, SessionState st = SessionState::Idle)
{
    SessionInfo s;
    s.id = id;
    s.title = L"t";
    s.workingDir = L"K:/x";
    s.state = st;
    return s;
}

static HookMessage Msg(const std::wstring& id, HookEvent ev)
{
    HookMessage m;
    m.sessionId = id;
    m.event = ev;
    return m;
}

static HookMessage UPS(const std::wstring& id, const std::wstring& prompt)
{
    HookMessage m;
    m.sessionId = id;
    m.event = HookEvent::UserPromptSubmit;
    m.promptText = prompt;
    return m;
}

static int64_t NowMsTest()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static void TestStateMachine()
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
static void TestOrderedStateMachine()
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

static void TestWire()
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

static void TestRegistry()
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
static void TestRegistryFanout()
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
static void TestForkSourceIdEcho()
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

static void TestTypedCapture()
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
static void TestObserveClaude()
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
static void TestSupersedeStaleTabSiblings()
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

static void TestSpawnBuilders()
{
    std::wprintf(L"ClaudeSpawn builders:\n");
    CHECK(ToForwardSlashes(L"C:\\a\\b") == L"C:/a/b", "to forward slashes");
    CHECK(JsonEscape(L"a\"b\\c") == L"a\\\"b\\\\c", "json escape quote+backslash");

    const auto cmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true);
    CHECK(cmd == L"claude --dangerously-skip-permissions --settings \"C:/x/s.json\" --session-id abc-123", "claude commandline (fresh, bypass)");
    const auto rcmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true);
    CHECK(rcmd == L"claude --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\"", "claude commandline (resume, bypass)");
    // skipPermissions OFF: no flag (the settings file pins permissions.defaultMode instead).
    const auto cmdNB = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, false);
    CHECK(cmdNB == L"claude --settings \"C:/x/s.json\" --session-id abc-123", "claude commandline (fresh, no bypass)");
    const auto rcmdNB = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, false);
    CHECK(rcmdNB == L"claude --resume abc-123 --settings \"C:/x/s.json\"", "claude commandline (resume, no bypass)");

    // Launcher resolution (Agentmaster — the "my friend couldn't run it" 0x80070002 fix). ConPTY's
    // CreateProcessW appends only ".exe" and ignores PATHEXT, so a bare `claude` token misses the npm
    // `claude.cmd`. Given the REAL launcher full path we emit it directly: a .exe as the quoted leading
    // token; a .cmd/.bat wrapped in `cmd /c` (CreateProcessW cannot exec a batch file directly).
    const auto exeCmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\Users\\me\\.local\\bin\\claude.exe");
    CHECK(exeCmd == L"\"C:\\Users\\me\\.local\\bin\\claude.exe\" --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\"", "claude commandline (resume, resolved .exe by full path)");
    const auto cmdResume = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\npm\\claude.cmd");
    CHECK(cmdResume == L"cmd /c \"\"C:\\npm\\claude.cmd\" --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\"\"", "claude commandline (resume, npm .cmd via cmd /c)");
    const auto cmdFresh = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true, L"", L"C:\\npm\\claude.cmd");
    CHECK(cmdFresh == L"cmd /c \"\"C:\\npm\\claude.cmd\" --dangerously-skip-permissions --settings \"C:/x/s.json\" --session-id abc-123\"", "claude commandline (fresh, npm .cmd via cmd /c, keeps --session-id)");
    // .bat resolves like .cmd, extension match is case-insensitive; no-bypass omits the flag.
    const auto batCmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, false, L"", L"C:\\tools\\Claude.BAT");
    CHECK(batCmd == L"cmd /c \"\"C:\\tools\\Claude.BAT\" --resume abc-123 --settings \"C:/x/s.json\"\"", "claude commandline (resume, .BAT case-insensitive, no bypass)");
    // Fork via a resolved .exe: full path leads, source conversation forked into the new id.
    const auto forkExe = BuildClaudeCommandline(L"C:/x/s.json", L"new-id", false, true, L"src-id", L"C:\\bin\\claude.exe");
    CHECK(forkExe == L"\"C:\\bin\\claude.exe\" --dangerously-skip-permissions --resume src-id --fork-session --session-id new-id --settings \"C:/x/s.json\"", "claude commandline (fork, resolved .exe)");
    // Empty launcher => the bare-token fallback (unchanged from the original behavior).
    CHECK(BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"") == rcmd, "empty launcher falls back to the bare `claude` token");

    // BuildClaudeRestartSpec (Agentmaster — the "Restart session" fix). Relaunch an EXISTING conversation
    // IN PLACE instead of replaying the launch commandline. The invariants vs the old behavior: it KEEPS
    // the conversation id (never re-keys), it NEVER forks, and it RESUMES when a transcript exists else
    // starts FRESH reusing the id. A freshly-minted random id has no transcript, so this is the
    // deterministic FRESH form — `--session-id <id>`, NOT `--resume`, NOT `--fork-session`, SAME id. (Like
    // BuildClaudeSpawn it materializes the hook files in the active profile dir; we assert only the
    // profile-dir-independent parts, so the embedded --settings path is irrelevant to the checks.)
    {
        AppSettings rst;
        rst.skipPermissions = true;
        const std::wstring rid = NewSessionId(); // random => guaranteed no transcript => the fresh form
        const auto rspec = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", rid, rst, L"C:\\bin\\claude.exe");
        CHECK(rspec.sessionId == rid, "restart spec KEEPS the conversation id (never re-keys)");
        CHECK(rspec.commandline.find(L"--session-id " + rid) != std::wstring::npos, "restart of a transcript-less id uses the FRESH form (--session-id <id>)");
        CHECK(rspec.commandline.find(L"--resume") == std::wstring::npos, "restart (no transcript) does NOT --resume");
        CHECK(rspec.commandline.find(L"--fork-session") == std::wstring::npos, "restart NEVER forks");
        CHECK(rspec.commandline.rfind(L"\"C:\\bin\\claude.exe\"", 0) == 0, "restart launches the resolved claude.exe by full path");
        bool hasSid = false, hasPipe = false;
        for (const auto& [k, v] : rspec.env)
        {
            if (k == L"CCMGR_SESSION_ID" && v == rid)
            {
                hasSid = true;
            }
            if (k == L"CCMGR_HOOK_PIPE" && v == L"\\\\.\\pipe\\agentmaster.42")
            {
                hasPipe = true;
            }
        }
        CHECK(hasSid, "restart spec env carries CCMGR_SESSION_ID = the kept id");
        CHECK(hasPipe, "restart spec env carries CCMGR_HOOK_PIPE = the bridge pipe");
    }

    // BuildClaudeSpawn fork id selection (Agentmaster — restoring a never-messaged fork). A GENUINE
    // fork mints a FRESH target id (!= the source); a RESTORE re-fork passes forkIntoSessionId so the
    // fork branches back into its EXISTING id (preserving identity across a restart instead of churning
    // a new id every reopen). Both emit the same `--resume <src> --fork-session --session-id <target>`
    // shape — only the target id differs. (Materializes hook files in the active profile dir; we assert
    // only the profile-independent parts.)
    {
        AppSettings fst;
        fst.skipPermissions = true;
        const std::wstring src = NewSessionId();
        // Genuine fork: forkIntoSessionId omitted => a fresh, distinct id is minted as the target.
        const auto genuine = BuildClaudeSpawn(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", L"", fst, src, L"C:\\bin\\claude.exe");
        CHECK(genuine.sessionId != src && !genuine.sessionId.empty(), "genuine fork mints a fresh target id (!= source)");
        CHECK(genuine.commandline.find(L"--resume " + src + L" --fork-session --session-id " + genuine.sessionId) != std::wstring::npos, "genuine fork commandline forks the source into the minted id");
        // Re-fork: forkIntoSessionId == the fork's existing id => spec targets THAT id (no churn).
        const std::wstring keep = NewSessionId();
        const auto refork = BuildClaudeSpawn(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", L"", fst, src, L"C:\\bin\\claude.exe", keep);
        CHECK(refork.sessionId == keep, "re-fork targets the fork's existing id (forkIntoSessionId), preserving identity");
        CHECK(refork.commandline.find(L"--resume " + src + L" --fork-session --session-id " + keep) != std::wstring::npos, "re-fork commandline forks the source into the SAME id");
        // forkIntoSessionId is honored only when forking: a plain resume ignores it (reuses the resume id).
        const auto resumeIgnores = BuildClaudeSpawn(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", keep, fst, L"", L"C:\\bin\\claude.exe", L"some-other-id");
        CHECK(resumeIgnores.sessionId == keep, "forkIntoSessionId is ignored on a non-fork (resume reuses its id)");
    }

    // ResolveClaudeExeIn (native-exe-only policy): resolve a real claude.exe; a .cmd is only a
    // breadcrumb to its npm binary; a pure-Node .cmd (no binary) resolves to empty (gated). Temp fixture.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::wstring root = (fs::temp_directory_path(ec) / (L"am-claude-exe-" + NewSessionId())).wstring();
        const std::wstring binExe = root + L"\\binexe";
        const std::wstring binCmd = root + L"\\bincmd";
        const std::wstring binNode = root + L"\\binnode";
        const std::wstring home = root + L"\\home";
        const std::wstring exeDirect = binExe + L"\\claude.exe";
        const std::wstring npmExe = binCmd + L"\\node_modules\\@anthropic-ai\\claude-code-win32-x64\\claude.exe";
        const std::wstring homeExe = home + L"\\.local\\bin\\claude.exe";
        auto touch = [](const std::wstring& p) {
            std::error_code e2;
            fs::create_directories(fs::path{ p }.parent_path(), e2);
            std::ofstream f{ fs::path{ p }, std::ios::binary };
            f << "x";
        };
        touch(exeDirect);
        touch(binCmd + L"\\claude.cmd");
        touch(npmExe);
        touch(binNode + L"\\claude.cmd"); // a pure-Node install: a .cmd but NO native binary anywhere
        touch(homeExe);

        // 1. A valid override (an existing .exe) wins outright.
        CHECK(ResolveClaudeExeIn(exeDirect, {}, L"") == exeDirect, "ResolveClaudeExe: valid .exe override");
        // 2. A .cmd override is rejected (exe-only) -> empty.
        CHECK(ResolveClaudeExeIn(binCmd + L"\\claude.cmd", {}, L"").empty(), "ResolveClaudeExe: .cmd override rejected");
        // 3. A nonexistent .exe override -> empty.
        CHECK(ResolveClaudeExeIn(root + L"\\nope.exe", {}, L"").empty(), "ResolveClaudeExe: missing .exe override -> empty");
        // 4. A direct claude.exe on PATH beats an earlier dir's .cmd (step 2 runs before the .cmd follow).
        CHECK(ResolveClaudeExeIn(L"", { binCmd, binExe }, L"") == exeDirect, "ResolveClaudeExe: PATH claude.exe preferred over a .cmd");
        // 5. Follow a claude.cmd to its npm native binary (home empty so the ~/.local/bin step is skipped).
        CHECK(ResolveClaudeExeIn(L"", { binCmd }, L"") == npmExe, "ResolveClaudeExe: follow .cmd -> npm node_modules claude.exe");
        // 6. A pure-Node .cmd (no native binary anywhere) -> empty (gated, by design).
        CHECK(ResolveClaudeExeIn(L"", { binNode }, L"").empty(), "ResolveClaudeExe: pure-node .cmd -> not detected");
        // 7. The ~/.local/bin native install (step 3) beats chasing a .cmd's node_modules (step 4).
        CHECK(ResolveClaudeExeIn(L"", { binNode }, home) == homeExe, "ResolveClaudeExe: ~/.local/bin claude.exe fallback");

        fs::remove_all(fs::path{ root }, ec);
    }
    // Codex (managed-session support): with no resolved launcher, the bare token (back-compat +
    // the not-found path): bare `codex` fresh; `codex resume <uuid>` to continue a rollout; and
    // `codex fork <uuid>` to branch one (the adopt-a-live-external two-writers fix). Signature is
    // (resumeUuid, forkUuid, launcher) — fork WINS over resume when both are set.
    CHECK(BuildCodexCommandline(L"") == L"codex", "codex commandline (fresh, no launcher)");
    CHECK(BuildCodexCommandline(L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a") == L"codex resume 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (resume by rollout uuid, no launcher)");
    CHECK(BuildCodexCommandline(L"", L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a") == L"codex fork 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (fork by rollout uuid, no launcher)");
    CHECK(BuildCodexCommandline(L"aaaa", L"bbbb") == L"codex fork bbbb", "codex commandline (fork wins over resume)");
    // With a resolved launcher, codex is invoked BY FULL PATH — ConPTY's CreateProcessW ignores
    // PATHEXT, so a bare `codex` misses an npm codex.cmd and dies 0x80070002 (ERROR_FILE_NOT_FOUND).
    // A .exe runs directly (quoted, so a space in the path is safe).
    CHECK(BuildCodexCommandline(L"", L"", L"C:\\bin\\codex.exe") == L"\"C:\\bin\\codex.exe\"", "codex commandline (fresh, .exe full path)");
    CHECK(BuildCodexCommandline(L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"", L"C:\\bin\\codex.exe") == L"\"C:\\bin\\codex.exe\" resume 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (resume, .exe full path)");
    CHECK(BuildCodexCommandline(L"", L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"C:\\bin\\codex.exe") == L"\"C:\\bin\\codex.exe\" fork 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (fork, .exe full path)");
    // A .cmd/.bat is a batch script CreateProcessW cannot execute directly, so it goes via `cmd /c`,
    // wrapped in ONE outer quote pair so the inner launcher quote survives (cmd strips only the
    // first+last quote of the remainder — the >2-quotes case; mirrors BuildClaudeCommandline).
    CHECK(BuildCodexCommandline(L"", L"", L"C:\\npm\\codex.cmd") == L"cmd /c \"\"C:\\npm\\codex.cmd\"\"", "codex commandline (fresh, .cmd via cmd /c)");
    CHECK(BuildCodexCommandline(L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"", L"C:\\npm\\codex.cmd") == L"cmd /c \"\"C:\\npm\\codex.cmd\" resume 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a\"", "codex commandline (resume, .cmd via cmd /c)");
    CHECK(BuildCodexCommandline(L"", L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"C:\\npm\\codex.cmd") == L"cmd /c \"\"C:\\npm\\codex.cmd\" fork 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a\"", "codex commandline (fork, .cmd via cmd /c)");
    CHECK(BuildCodexCommandline(L"", L"", L"C:\\path with space\\codex.bat") == L"cmd /c \"\"C:\\path with space\\codex.bat\"\"", "codex commandline (.bat with spaces via cmd /c)");

    const auto json = BuildHooksSettingsJson(L"C:/x/agentmaster-hook.ps1", L"", true, true);
    CHECK(json.find(L"\"hooks\"") != std::wstring::npos, "settings has hooks");
    CHECK(json.find(L"SessionStart") != std::wstring::npos, "settings has SessionStart");
    CHECK(json.find(L"UserPromptSubmit") != std::wstring::npos, "settings has UserPromptSubmit");
    CHECK(json.find(L"Notification") != std::wstring::npos, "settings has Notification");
    CHECK(json.find(L"Stop") != std::wstring::npos, "settings has Stop");
    CHECK(json.find(L"SubagentStop") != std::wstring::npos, "settings has SubagentStop");
    CHECK(json.find(L"SessionEnd") != std::wstring::npos, "settings has SessionEnd");
    CHECK(json.find(L"-Event Stop") != std::wstring::npos, "settings wires -Event Stop");
    CHECK(json.find(L"C:/x/agentmaster-hook.ps1") != std::wstring::npos, "settings references forwarder");
    // All-defaults (bypass on, model empty, co-authored on) => hooks-only, no extra keys.
    CHECK(json.find(L"\"model\"") == std::wstring::npos, "default settings omit model");
    CHECK(json.find(L"includeCoAuthoredBy") == std::wstring::npos, "default settings omit includeCoAuthoredBy");
    CHECK(json.find(L"permissions") == std::wstring::npos, "bypass settings omit permissions.defaultMode");
    // Non-default Claude fields are emitted; no-bypass pins permissions.defaultMode ("other variation").
    const auto json2 = BuildHooksSettingsJson(L"C:/x/agentmaster-hook.ps1", L"sonnet", false, false);
    CHECK(json2.find(L"\"model\": \"sonnet\"") != std::wstring::npos, "settings emit model when set");
    CHECK(json2.find(L"\"includeCoAuthoredBy\": false") != std::wstring::npos, "settings emit includeCoAuthoredBy:false");
    CHECK(json2.find(L"\"defaultMode\": \"default\"") != std::wstring::npos, "no-bypass pins permissions.defaultMode");

    // ParseEnvAssignments: ';'-delimited NAME=VALUE -> pairs (for AppSettings.env).
    {
        const auto e = ParseEnvAssignments(L" FOO=bar ; HTTPS_PROXY=http://h:8080 ; PATH=a=b=c ");
        CHECK(e.size() == 3, "env parse: three entries");
        CHECK(e[0].first == L"FOO" && e[0].second == L"bar", "env parse: trims + splits FOO=bar");
        CHECK(e[1].first == L"HTTPS_PROXY" && e[1].second == L"http://h:8080", "env parse: value keeps :// ");
        CHECK(e[2].first == L"PATH" && e[2].second == L"a=b=c", "env parse: value keeps later '='");
        const auto empty = ParseEnvAssignments(L"");
        CHECK(empty.empty(), "env parse: empty -> none");
        const auto bad = ParseEnvAssignments(L"NOEQUALS;=noname;;GOOD=1");
        CHECK(bad.size() == 1 && bad[0].first == L"GOOD", "env parse: skips malformed/empty entries");
    }

    // ParseEnvAssignments: newline-delimited (the multi-line editor) + '#' comments + mixed separators.
    {
        const auto e = ParseEnvAssignments(L"FOO=bar\nHTTPS_PROXY=http://h:8080\n# a comment\n\nBAZ=1");
        CHECK(e.size() == 3, "env parse: newline split, blank + '#' comment skipped");
        CHECK(e[0].first == L"FOO" && e[0].second == L"bar", "env parse: newline FOO=bar");
        CHECK(e[1].first == L"HTTPS_PROXY" && e[1].second == L"http://h:8080", "env parse: newline keeps ://");
        CHECK(e[2].first == L"BAZ" && e[2].second == L"1", "env parse: entry after comment + blank");
        const auto crlf = ParseEnvAssignments(L"A=1\r\nB=2\r\n");
        CHECK(crlf.size() == 2 && crlf[0].first == L"A" && crlf[1].first == L"B", "env parse: CRLF lines");
        const auto mixed = ParseEnvAssignments(L"A=1;B=2\nC=3");
        CHECK(mixed.size() == 3, "env parse: ';' and newline both separate (back-compat)");
        const auto comment = ParseEnvAssignments(L"#FOO=bar");
        CHECK(comment.empty(), "env parse: a '#'-led line is a comment even with '='");
    }

    // MergeSessionEnv: per-dir overrides global (case-insensitive name match), CCMGR_* dropped, last-wins.
    {
        const auto m = MergeSessionEnv(L"FOO=global\nBAR=keep\nCCMGR_X=nope", L"foo=perdir\nNEW=1");
        CHECK(m.size() == 3, "merge: FOO/BAR/NEW (CCMGR_ dropped)");
        bool fooOk = false, barOk = false, newOk = false, ccmgr = false;
        for (const auto& [k, v] : m)
        {
            if (k == L"FOO")
            {
                fooOk = (v == L"perdir"); // per-dir wins; first-seen NAME spelling "FOO" kept
            }
            if (k == L"BAR")
            {
                barOk = (v == L"keep");
            }
            if (k == L"NEW")
            {
                newOk = (v == L"1");
            }
            if (k.rfind(L"CCMGR_", 0) == 0)
            {
                ccmgr = true;
            }
        }
        CHECK(fooOk, "merge: per-dir 'foo' overrides global 'FOO' (case-insensitive)");
        CHECK(barOk, "merge: global-only BAR kept");
        CHECK(newOk, "merge: per-dir-only NEW added");
        CHECK(!ccmgr, "merge: CCMGR_* dropped");
        const auto dup = MergeSessionEnv(L"A=1\nA=2", L"");
        CHECK(dup.size() == 1 && dup[0].second == L"2", "merge: duplicate name -> last value wins");
        CHECK(MergeSessionEnv(L"", L"").empty(), "merge: empty -> none");
    }

    // LexEnvText: per-line verdicts + worst level + first-issue (drives the cog border + status line).
    {
        const auto okr = LexEnvText(L"FOO=bar\nBAZ=1");
        CHECK(okr.ok == 2 && okr.warn == 0 && okr.error == 0, "lex: two valid vars");
        CHECK(okr.worst == EnvLineKind::Ok, "lex: all-ok worst == Ok");

        const auto err = LexEnvText(L"FOO=bar\nNOEQUALS\n=noname\n1BAD=x");
        CHECK(err.error == 3, "lex: missing '=' + empty name + invalid name => 3 errors");
        CHECK(err.worst == EnvLineKind::Error, "lex: worst == Error");
        CHECK(err.firstIssueLine == 2, "lex: first issue on line 2 (NOEQUALS)");

        const auto warn = LexEnvText(L"FOO=1\nCCMGR_X=2\nAM_SESSION=3\nFOO=4");
        CHECK(warn.error == 0 && warn.warn == 3, "lex: reserved + owned + duplicate => 3 warns");
        CHECK(warn.worst == EnvLineKind::Warn, "lex: worst == Warn (no errors)");
        CHECK(warn.ok == 1, "lex: only the first FOO counts as a valid var");

        const auto ign = LexEnvText(L"\n# comment\n   ");
        CHECK(ign.ok == 0 && ign.warn == 0 && ign.error == 0, "lex: blanks + comment => nothing");
    }

    // dir-env.json round-trip (Serialize/Deserialize; pure — no disk).
    {
        std::vector<std::pair<std::wstring, std::wstring>> entries{
            { L"c:\\work\\proj", L"AWS_PROFILE=dev\nFOO=bar" },
            { L"c:\\work\\other", L"X=1" },
        };
        const auto back = DeserializeDirEnv(SerializeDirEnv(entries));
        CHECK(back.size() == 2, "dir-env: round-trip two entries");
        CHECK(back[0].first == L"c:\\work\\proj" && back[0].second == L"AWS_PROFILE=dev\nFOO=bar", "dir-env: multi-line env survives");
        std::vector<std::pair<std::wstring, std::wstring>> withBlank{ { L"c:\\d", L"   " }, { L"c:\\e", L"Y=2" } };
        const auto back2 = DeserializeDirEnv(SerializeDirEnv(withBlank));
        CHECK(back2.size() == 1 && back2[0].first == L"c:\\e", "dir-env: blank-env entry dropped on load");
    }

    // ApplyEnvDefaults (ENV_VARS.md §8): one-time seed of the shipped GLOBAL env defaults, version-gated.
    {
        // Fresh install (version 0, empty env): CLAUDE_CODE_MAX_RETRIES=50000 appended; version -> current.
        auto [env1, ver1] = ApplyEnvDefaults(L"", 0);
        CHECK(ver1 == kEnvDefaultsVersion, "env-defaults: version bumped to current");
        CHECK(env1.find(L"CLAUDE_CODE_MAX_RETRIES=50000") != std::wstring::npos, "env-defaults: seeds CLAUDE_CODE_MAX_RETRIES=50000");

        // Already current: nothing re-added (a user who deleted it keeps it gone).
        auto [env2, ver2] = ApplyEnvDefaults(L"", kEnvDefaultsVersion);
        CHECK(env2.empty() && ver2 == kEnvDefaultsVersion, "env-defaults: no re-seed once current version reached");

        // User already has the NAME (any case): never duplicated; version still advances.
        auto [env3, ver3] = ApplyEnvDefaults(L"claude_code_max_retries=7", 0);
        CHECK(env3 == L"claude_code_max_retries=7" && ver3 == kEnvDefaultsVersion, "env-defaults: existing NAME (case-insensitive) not duplicated");

        // Non-empty env without the default: appended on its OWN line (no run-together).
        auto [env4, ver4] = ApplyEnvDefaults(L"FOO=bar", 0);
        CHECK(env4 == L"FOO=bar\nCLAUDE_CODE_MAX_RETRIES=50000", "env-defaults: appended on a fresh line");
    }

    // UpsertJsonNumberKey (ENV_VARS.md §8): the PURE RMW core of the Claude user settings.json repository.
    {
        // Empty input + set => a fresh object carrying the key.
        const auto fresh = UpsertJsonNumberKey(L"", L"cleanupPeriodDays", 36500.0);
        const auto p1 = fresh ? json::Parse(*fresh) : std::nullopt;
        CHECK(p1 && p1->I64At(L"cleanupPeriodDays", 0) == 36500, "upsert: sets the number on an empty doc");

        // Preserve other keys; upsert (no duplicate) when the key already exists.
        const auto merged = UpsertJsonNumberKey(L"{\"a\":1,\"b\":\"x\",\"cleanupPeriodDays\":30}", L"cleanupPeriodDays", 36500.0);
        const auto p2 = merged ? json::Parse(*merged) : std::nullopt;
        CHECK(p2 && p2->I64At(L"a", 0) == 1 && p2->StrAt(L"b") == L"x", "upsert: preserves other keys");
        CHECK(p2 && p2->I64At(L"cleanupPeriodDays", 0) == 36500, "upsert: updates the existing key value");
        int dupCount = 0;
        if (p2)
        {
            for (const auto& kv : p2->members)
            {
                if (kv.first == L"cleanupPeriodDays")
                {
                    ++dupCount;
                }
            }
        }
        CHECK(dupCount == 1, "upsert: exactly one cleanupPeriodDays member (no duplicate appended)");

        // nullopt removes the key, keeping the rest.
        const auto removed = UpsertJsonNumberKey(L"{\"a\":1,\"cleanupPeriodDays\":30}", L"cleanupPeriodDays", std::nullopt);
        const auto p3 = removed ? json::Parse(*removed) : std::nullopt;
        CHECK(p3 && p3->Find(L"cleanupPeriodDays") == nullptr && p3->I64At(L"a", 0) == 1, "upsert: nullopt removes the key, keeps the rest");

        // A non-empty, non-object file is NEVER clobbered (caller must not overwrite).
        CHECK(!UpsertJsonNumberKey(L"this is not json", L"cleanupPeriodDays", 36500.0).has_value(), "upsert: refuses an unparseable non-empty file");
        CHECK(!UpsertJsonNumberKey(L"[1,2,3]", L"cleanupPeriodDays", 36500.0).has_value(), "upsert: refuses a non-object JSON (array)");
    }

    // PsSingleQuote: PowerShell single-quoted literal (only escape = doubled quote).
    CHECK(PsSingleQuote(L"C:\\Users\\x\\.agentmaster") == L"'C:\\Users\\x\\.agentmaster'", "ps quote: plain path verbatim");
    CHECK(PsSingleQuote(L"C:\\Users\\o'brien") == L"'C:\\Users\\o''brien'", "ps quote: embedded quote doubled");
    CHECK(PsSingleQuote(L"$env:FOO") == L"'$env:FOO'", "ps quote: $ stays inert");

    const auto fwd = BuildForwarderScript(L"C:\\Users\\x\\.agentmaster-dev");
    CHECK(fwd.find(L"NamedPipeClientStream") != std::wstring::npos, "forwarder uses NamedPipeClientStream");
    CHECK(fwd.find(L"CCMGR_SESSION_ID") != std::wstring::npos, "forwarder reads CCMGR_SESSION_ID (launch-id fallback)");
    CHECK(fwd.find(L"CCMGR_HOOK_PIPE") != std::wstring::npos, "forwarder reads CCMGR_HOOK_PIPE");
    CHECK(fwd.find(L"session_id") != std::wstring::npos, "forwarder uses payload session_id");
    // Payload-FIRST precedence: the hook's session_id (Claude's CURRENT conversation, which follows
    // /resume,/clear,/compact) must be chosen BEFORE the launch-time env fallback. Env-first
    // stranded a diverged managed session as a phantom record stuck in the last mis-attributed
    // state (no transcript under the dead launch id => the scanner could never reconcile it).
    {
        const auto payloadAt = fwd.find(L"$sid = [string]$j.session_id");
        const auto envAt = fwd.find(L"$sid = $env:CCMGR_SESSION_ID");
        CHECK(payloadAt != std::wstring::npos && envAt != std::wstring::npos && payloadAt < envAt,
              "forwarder prefers payload session_id over the launch env (id follows /resume,/clear,/compact)");
    }
    CHECK(fwd.find(L"WT_SESSION") != std::wstring::npos, "forwarder emits WT_SESSION tabToken");
    // The bridge-discovery fallback is PER-PROFILE: the stateDir is baked in (PS-single-quoted);
    // no profile-blind $env:USERPROFILE\.agentmaster path and no unexpanded placeholder remain.
    CHECK(fwd.find(L"$disc = 'C:\\Users\\x\\.agentmaster-dev\\bridge.json'") != std::wstring::npos, "forwarder discovery is the per-profile bridge.json");
    CHECK(fwd.find(L"Join-Path $env:USERPROFILE") == std::wstring::npos, "forwarder discovery is not profile-blind");
    CHECK(fwd.find(L"{{AM_BRIDGE_JSON}}") == std::wstring::npos, "forwarder placeholder fully substituted");
    // #5 (forwarder silent failures): every dropped delivery leaves a local trace — the bridge's
    // hooks.log only sees lines that ARRIVED, so a dead pipe / stale bridge.json was invisible.
    CHECK(fwd.find(L"$AmErrLog = 'C:\\Users\\x\\.agentmaster-dev\\forwarder-errors.log'") != std::wstring::npos, "forwarder error log is per-profile");
    CHECK(fwd.find(L"{{AM_FWD_ERRLOG}}") == std::wstring::npos, "error-log placeholder fully substituted");
    CHECK(fwd.find(L"NoteFwdDrop \"drop: no session id") != std::wstring::npos, "no-sid drop is noted");
    CHECK(fwd.find(L"NoteFwdDrop \"drop: no pipe") != std::wstring::npos, "no-pipe drop is noted");
    CHECK(fwd.find(L"NoteFwdDrop (\"drop: \" + $_.Exception.Message)") != std::wstring::npos, "pipe connect/write failure is noted");

    const auto id = NewSessionId();
    CHECK(id.size() == 36, "uuid length 36");
    CHECK(id[8] == L'-' && id[13] == L'-' && id[18] == L'-' && id[23] == L'-', "uuid hyphens");

    // BuildPwshHostedCommandline (Agentmaster — "claude run from a pwsh terminal"). Wrap an inner agent
    // command line so the ConPTY root is an interactive pwsh that -NoExit's to a live prompt at the cwd
    // when the agent quits, instead of the connection dying into a dead "press Enter to restart" pane.
    // -EncodedCommand carries the script as base64 UTF-16LE, side-stepping ALL nested-quote escaping —
    // so we decode the payload here and assert it is exactly the call-operator (&) invocation of inner.
    {
        const std::wstring inner = L"\"C:\\bin\\claude.exe\" --resume abc-123 --settings \"C:/x/s.json\"";
        const auto hosted = BuildPwshHostedCommandline(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe", inner);
        CHECK(hosted.rfind(L"\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo -NoExit -EncodedCommand ", 0) == 0,
              "pwsh host: quoted full path + -NoLogo -NoExit -EncodedCommand");
        // Decode the base64 payload (the last space-delimited token; base64 has no spaces) back to a
        // wstring (its bytes are UTF-16LE) and confirm it round-trips to '& <inner>'.
        auto b64decodeToWide = [](const std::wstring& b64) -> std::wstring {
            auto val = [](wchar_t c) -> int {
                if (c >= L'A' && c <= L'Z')
                    return c - L'A';
                if (c >= L'a' && c <= L'z')
                    return c - L'a' + 26;
                if (c >= L'0' && c <= L'9')
                    return c - L'0' + 52;
                if (c == L'+')
                    return 62;
                if (c == L'/')
                    return 63;
                return -1; // '=' padding / anything else
            };
            std::vector<unsigned char> bytes;
            int acc = 0, accBits = 0;
            for (const wchar_t c : b64)
            {
                const int v = val(c);
                if (v < 0)
                    continue;
                acc = (acc << 6) | v;
                accBits += 6;
                if (accBits >= 8)
                {
                    accBits -= 8;
                    bytes.push_back(static_cast<unsigned char>((acc >> accBits) & 0xFF));
                }
            }
            std::wstring out;
            for (size_t i = 0; i + 1 < bytes.size(); i += 2)
            {
                out.push_back(static_cast<wchar_t>(bytes[i] | (static_cast<unsigned>(bytes[i + 1]) << 8)));
            }
            return out;
        };
        const auto enc = hosted.substr(hosted.rfind(L' ') + 1);
        CHECK(!enc.empty(), "pwsh host: non-empty -EncodedCommand payload");
        CHECK(b64decodeToWide(enc) == L"& " + inner, "pwsh host: payload decodes to '& <inner>' (UTF-16LE, & call operator)");
        // Empty launcher => the bare `pwsh.exe` token (the not-found fallback).
        const auto hostedBare = BuildPwshHostedCommandline(L"", inner);
        CHECK(hostedBare.rfind(L"\"pwsh.exe\" -NoLogo -NoExit -EncodedCommand ", 0) == 0, "pwsh host: empty launcher -> quoted bare pwsh.exe token");
        // A bare inner token (codex fresh, or claude's empty-launcher fallback) is still invoked via &.
        const auto hostedCodex = BuildPwshHostedCommandline(L"C:\\bin\\pwsh.exe", L"codex resume 019ec0c7");
        CHECK(b64decodeToWide(hostedCodex.substr(hostedCodex.rfind(L' ') + 1)) == L"& codex resume 019ec0c7", "pwsh host: bare inner token invoked via &");
    }
}

// Agentmaster: the per-install state PROFILE (ProfileBootstrap.h) — the pure pieces: the choice
// file round-trip, the resolution precedence's env override, and the per-identity defaults.
// (The picker itself is UI; the packaged-PFN branches need a package context — both untestable
// headless. Tests run unpackaged, so PackageKey() must be "Unpackaged" and the silent default
// must be the HISTORICAL ~/.agentmaster — that invariant is what keeps this harness writing to
// the same state dir it always did.)
static void TestProfileBootstrap()
{
    namespace P = ::Agentmaster::Profiles;
    std::wprintf(L"ProfileBootstrap (profiles):\n");

    CHECK(P::PackageFamilyName().empty(), "unpackaged: no package family");
    CHECK(!P::IsDevPackage(), "unpackaged: not the dev package");
    CHECK(P::PackageKey() == L"Unpackaged", "unpackaged: choice key");

    const auto rel = P::DefaultReleaseProfileDir();
    const auto dev = P::DefaultDevProfileDir();
    CHECK(rel.size() > 12 && rel.compare(rel.size() - 12, 12, L".agentmaster") == 0, "release default ends .agentmaster");
    CHECK(dev.size() > 16 && dev.compare(dev.size() - 16, 16, L".agentmaster-dev") == 0, "dev default ends .agentmaster-dev");
    CHECK(P::DefaultProfileDir() == rel, "unpackaged silent default == the historical ~/.agentmaster");

    // Choice-file round-trip on a temp path (never the real %USERPROFILE% map).
    wchar_t tmpDir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring file = std::wstring{ tmpDir } + L"am-profiles-test-" + NewSessionId() + L".txt";
    {
        CHECK(P::ReadChoiceFile(file).empty(), "choice file: missing -> empty map");
        std::map<std::wstring, std::wstring> m;
        m[L"Agentmaster_56k4f06dsfp9r"] = L"C:\\Users\\x\\.agentmaster";
        m[L"AgentmasterDev_56k4f06dsfp9r"] = L"C:\\Users\\x\\.agentmaster-dev";
        m[L"Unpackaged"] = L"Q:\\profiles\\portable one"; // space survives (no quoting needed)
        CHECK(P::WriteChoiceFile(file, m), "choice file: write");
        const auto r = P::ReadChoiceFile(file);
        CHECK(r.size() == 3, "choice file: three entries back");
        CHECK(r.at(L"Agentmaster_56k4f06dsfp9r") == L"C:\\Users\\x\\.agentmaster", "choice file: release slot");
        CHECK(r.at(L"AgentmasterDev_56k4f06dsfp9r") == L"C:\\Users\\x\\.agentmaster-dev", "choice file: dev slot");
        CHECK(r.at(L"Unpackaged") == L"Q:\\profiles\\portable one", "choice file: path with space");
        // Read-modify-write keeps other installs' slots (the SaveChoice contract).
        auto r2 = r;
        r2[L"Unpackaged"] = L"D:\\elsewhere";
        CHECK(P::WriteChoiceFile(file, r2), "choice file: rewrite");
        const auto r3 = P::ReadChoiceFile(file);
        CHECK(r3.size() == 3 && r3.at(L"Unpackaged") == L"D:\\elsewhere" &&
                  r3.at(L"Agentmaster_56k4f06dsfp9r") == L"C:\\Users\\x\\.agentmaster",
              "choice file: update preserves other slots");
        ::DeleteFileW(file.c_str());
        ::DeleteFileW((file + L".tmp").c_str());
    }

    // Resolution precedence: the env override beats everything (and is what the WindowEmperor
    // bootstrap exports, so dll-side resolution always agrees with the exe).
    {
        const std::wstring fake = std::wstring{ tmpDir } + L"am-profile-env-" + NewSessionId();
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", fake.c_str());
        CHECK(P::ResolveProfileDirUncached() == fake, "resolve: env override wins");
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", nullptr);
        const auto silent = P::ResolveProfileDirUncached();
        CHECK(!silent.empty(), "resolve: silent resolution non-empty");
        ::RemoveDirectoryW(fake.c_str());
    }

    // SamePath: the filesystem-aware-enough comparison the migrate guard uses.
    CHECK(P::detail::SamePath(L"C:\\A\\b\\", L"c:/a/B"), "SamePath: case/slash/trailing-insensitive");
    CHECK(!P::detail::SamePath(L"C:\\A\\b", L"C:\\A\\b2"), "SamePath: distinct dirs differ");

    // MigrateProfileData: copies content, skips locks/shim/bridge.json, never clobbers.
    {
        const std::wstring src = std::wstring{ tmpDir } + L"am-mig-src-" + NewSessionId();
        const std::wstring dst = std::wstring{ tmpDir } + L"am-mig-dst-" + NewSessionId();
        std::filesystem::create_directories(std::filesystem::path{ src } / L"windows");
        std::filesystem::create_directories(std::filesystem::path{ src } / L"locks");
        std::filesystem::create_directories(std::filesystem::path{ src } / L"shim");
        auto put = [](const std::filesystem::path& p, const char* bytes) {
            std::ofstream f{ p, std::ios::binary };
            f << bytes;
        };
        put(std::filesystem::path{ src } / L"sessions.json", "{\"v\":1}");
        put(std::filesystem::path{ src } / L"bridge.json", "{\"pipe\":\"stale\"}");
        put(std::filesystem::path{ src } / L"windows" / L"w1.json", "{\"id\":\"w1\"}");
        put(std::filesystem::path{ src } / L"locks" / L"x", "lock");
        put(std::filesystem::path{ src } / L"shim" / L"claude.cmd", "rem old");
        std::filesystem::create_directories(std::filesystem::path{ dst });
        put(std::filesystem::path{ dst } / L"settings.json", "{\"keep\":true}");
        put(std::filesystem::path{ src } / L"settings.json", "{\"keep\":false}");

        P::MigrateProfileData(src, dst);
        namespace fs = std::filesystem;
        CHECK(fs::exists(fs::path{ dst } / L"sessions.json"), "migrate: copies sessions.json");
        CHECK(fs::exists(fs::path{ dst } / L"windows" / L"w1.json"), "migrate: copies windows/ recursively");
        CHECK(!fs::exists(fs::path{ dst } / L"bridge.json"), "migrate: skips bridge.json (stale pipe)");
        CHECK(!fs::exists(fs::path{ dst } / L"locks"), "migrate: skips locks/");
        CHECK(!fs::exists(fs::path{ dst } / L"shim"), "migrate: skips shim/ (regenerated)");
        {
            std::ifstream f{ fs::path{ dst } / L"settings.json", std::ios::binary };
            std::string body{ std::istreambuf_iterator<char>{ f }, std::istreambuf_iterator<char>{} };
            CHECK(body == "{\"keep\":true}", "migrate: never clobbers existing target files");
        }
        // Same-path call is a no-op (the guard the picker's checkbox relies on).
        P::MigrateProfileData(src, src + L"\\");
        CHECK(fs::exists(fs::path{ src } / L"bridge.json"), "migrate: same-path no-op leaves source intact");

        std::error_code ec;
        fs::remove_all(src, ec);
        fs::remove_all(dst, ec);
    }
}

static bool WriteLineToPipe(const std::wstring& pipeName, const std::string& utf8Line)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        const HANDLE h = ::CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, utf8Line.data(), static_cast<DWORD>(utf8Line.size()), &written, nullptr);
            ::CloseHandle(h);
            return ok && written == utf8Line.size();
        }
        if (::GetLastError() == ERROR_PIPE_BUSY)
        {
            ::WaitNamedPipeW(pipeName.c_str(), 1000);
            continue;
        }
        ::Sleep(20);
    }
    return false;
}

static void TestBridgeRoundTrip()
{
    std::wprintf(L"HooksBridge pipe round-trip (end-to-end):\n");
    SessionRegistry reg;
    reg.Upsert(MakeSession(L"pipe-1"));

    const auto pipeName = HookPipeName(::GetCurrentProcessId());
    HooksBridge bridge(pipeName, [&](const HookMessage& m) { reg.OnHookEvent(m); }, 2);
    CHECK(bridge.Start(), "bridge started");

    // Give the listener threads a moment to create their pipe instances.
    ::Sleep(100);

    const bool wrote = WriteLineToPipe(pipeName, std::string{ "Stop\tpipe-1\tK:/api\t1\t0\t\n" });
    CHECK(wrote, "client wrote wire line to pipe");

    // Poll for the state change (the dispatch happens on a bridge thread).
    bool became = false;
    for (int i = 0; i < 200; ++i)
    {
        const auto s = reg.Get(L"pipe-1");
        if (s && s->state == SessionState::WaitingForInput)
        {
            became = true;
            break;
        }
        ::Sleep(10);
    }
    CHECK(became, "registry reached WaitingForInput via pipe");
    CHECK(reg.Get(L"pipe-1") && reg.Get(L"pipe-1")->lastMessageWasQuestion, "question flag carried over the pipe");
    CHECK(reg.Get(L"pipe-1") && reg.Get(L"pipe-1")->workingDir == L"K:/x", "existing workingDir preserved");

    bridge.Stop();
    CHECK(!bridge.Running(), "bridge stopped");
}

static void TestScheduler()
{
    std::wprintf(L"Autopilot DecideAdvance (Correctness Rule #1 + backstops):\n");
    auto mk = [](AutopilotMode m, SessionState st) {
        SessionInfo s;
        s.id = L"a";
        s.state = st;
        s.autopilot.mode = m;
        QueuedPrompt p;
        p.id = L"p1";
        p.text = L"do it";
        s.queue.push_back(p);
        return s;
    };
    const int64_t now = 100000;

    {
        auto s = mk(AutopilotMode::Off, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "off -> none");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::Running);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "not turn-complete -> none");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && p.promptIndex == 0, "full+waiting -> send #0");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, 0, true).action == AdvanceAction::None, "global pause -> none");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        s.autopilot.maxAutoSends = 2;
        s.autopilot.autoSendsThisRun = 2;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "maxAutoSends -> none");
    }
    {
        // A pending question / "needs you" state is treated like a mid-turn Running state: the
        // prompt stays Pending and waits for the next (non-question) turn-complete — it is NOT
        // parked in Held, and autopilot is NOT paused (the "reacted to needs-approval" bug).
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        s.lastMessageWasQuestion = true;
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::None, "question -> none (stay queued, wait like running)");
        CHECK(s.queue[0].status == PromptStatus::Pending, "question: prompt is left Pending (never moved to Held)");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        s.lastMessageWasQuestion = true;
        s.queue[0].guardPattern = std::wstring{ kAnswersQuestionOk };
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::Send, "question + override -> send");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        s.queue[0].gate = PromptGate::Manual;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "manual gate -> none");
    }
    {
        auto s = mk(AutopilotMode::SemiAuto, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::AwaitConfirm, "semi-auto -> await confirm");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        s.queue[0].status = PromptStatus::Sent;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::PlanDone, "no pending -> plan done");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, now - 500, false).action == AdvanceAction::None, "human typing -> none");
    }
    {
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, now - 5000, false).action == AdvanceAction::Send, "human idle -> send");
    }

    // --- Idle bootstrap: a just-resumed / freshly-launched session must START its plan, not
    //     wait for a Stop it will never emit (the "ddd is Idle + auto but won't fire" bug). ---
    {
        auto s = mk(AutopilotMode::Full, SessionState::Idle);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && p.promptIndex == 0, "full + idle -> send (bootstrap)");
    }
    {
        auto s = mk(AutopilotMode::SemiAuto, SessionState::Idle);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::AwaitConfirm, "semi-auto + idle -> await confirm");
    }
    {
        auto s = mk(AutopilotMode::Off, SessionState::Idle);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "off + idle -> none");
    }
    {
        // Not-ready states stay rejected (mid-turn / awaiting you).
        auto s = mk(AutopilotMode::Full, SessionState::NeedsApproval);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "needs-approval -> none");
        s.state = SessionState::Done;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "done -> none");
    }

    // --- Pickup guard: one prompt per turn even though advances can now be change-driven. ---
    auto withLead = [&](PromptStatus st, bool echoed, int64_t sentAt, SessionState state) {
        auto s = mk(AutopilotMode::Full, state);
        QueuedPrompt lead; // a prior Flight prompt at the FRONT; p1 (Pending) follows
        lead.id = L"p0";
        lead.text = L"already sent";
        lead.status = st;
        lead.origin = PromptOrigin::Flight;
        lead.echoed = echoed;
        lead.sentAtUnixMs = sentAt;
        s.queue.insert(s.queue.begin(), lead);
        return s;
    };
    {
        // Just injected (Sent, recent, not echoed) -> hold the next prompt until Claude picks it up.
        auto s = withLead(PromptStatus::Sent, false, now - 500, SessionState::Idle);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "pickup guard: awaiting injection -> none");
    }
    {
        // Echoed (Claude moved to Running and back) -> the next Pending proceeds.
        auto s = withLead(PromptStatus::Sent, true, now - 500, SessionState::WaitingForInput);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && s.queue[p.promptIndex].id == L"p1", "echoed lead -> next pending sends");
    }
    {
        // Stale un-echoed (echo lost long ago) -> window expired, don't stall the plan forever.
        auto s = withLead(PromptStatus::Sent, false, now - 60000, SessionState::WaitingForInput);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && s.queue[p.promptIndex].id == L"p1", "stale un-echoed -> window expired, proceed");
    }
}

// Agentmaster — the Enter-retry decision (DecideEnterRetry): re-press Enter when the TUI ate the
// submit Enter of a just-injected Flight prompt (the prompt is typed but never sent, so the turn
// never starts). Pure + deterministic, like DecideAdvance.
static void TestEnterRetry()
{
    std::wprintf(L"Autopilot DecideEnterRetry (the TUI-ate-my-Enter backstop):\n");
    const int64_t T = 1000000;
    // A live, managed session with one Sent (Flight, not echoed) prompt at sentAt.
    auto mk = [](SessionState st, int64_t sentAt, bool echoed, uint32_t retries) {
        SessionInfo s;
        s.id = L"r";
        s.live = true;
        s.external = false;
        s.state = st;
        QueuedPrompt p;
        p.id = L"rp";
        p.text = L"go";
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Flight;
        p.sentAtUnixMs = sentAt;
        p.echoed = echoed;
        p.enterRetries = retries;
        s.queue.push_back(p);
        return s;
    };

    {
        // Just sent -> watching, not yet due.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        const auto r = DecideEnterRetry(s, T);
        CHECK(r.action == EnterRetryAction::Waiting && r.promptId == L"rp", "fresh send -> waiting");
    }
    {
        // One ms before the FIRST-retry delay -> still waiting. A fresh send (enterRetries==0) is due
        // after the SHORT kEnterRetryFirstMs (the snappy first re-press), not the longer interval.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs - 1).action == EnterRetryAction::Waiting, "just under first delay -> waiting");
    }
    {
        // First-retry delay elapsed, turn never started -> re-press Enter (attempt 0).
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        const auto r = DecideEnterRetry(s, T + kEnterRetryFirstMs);
        CHECK(r.action == EnterRetryAction::Retry && r.promptId == L"rp" && r.attempt == 0, "first delay elapsed -> retry");
    }
    {
        // Escalating: AFTER the first press (enterRetries==1) the next press waits the LONGER
        // kEnterRetryIntervalMs, not the short first delay. (The worker refreshes sentAtUnixMs to the
        // last press, so the comparison is "since the last press".)
        auto s = mk(SessionState::WaitingForInput, T, false, 1);
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::Waiting, "2nd press waits longer than the first");
        const auto r2 = DecideEnterRetry(s, T + kEnterRetryIntervalMs);
        CHECK(r2.action == EnterRetryAction::Retry && r2.attempt == 1, "2nd press due at the longer interval");
    }
    {
        // Same, from a freshly-resumed Idle session (it never emits a Stop).
        auto s = mk(SessionState::Idle, T, false, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry, "idle + due -> retry");
    }
    {
        // The UserPromptSubmit echo arrived (managed/hook path) -> the turn started, stop watching.
        auto s = mk(SessionState::WaitingForInput, T, true, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "echoed -> none");
    }
    {
        // State left the ready set (Running) -> the turn started (covers a hook-less adopted session
        // whose state is driven by the transcript tail), stop watching.
        auto s = mk(SessionState::Running, T, false, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "running -> none");
        s.state = SessionState::NeedsApproval;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "needs-approval -> none");
    }
    {
        // No-hook fast-turn fallback: echo never set + state back to Waiting, but the transcript
        // advanced past the send -> it was picked up, stop watching.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.convLastActivityUnixMs = T + kEnterRetryActivityMarginMs + 1;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "transcript advanced -> none");
    }
    {
        // Transcript moved only WITHIN the margin (a send fired right after the prior turn's tail) ->
        // NOT counted as started; still retry.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.convLastActivityUnixMs = T + kEnterRetryActivityMarginMs - 1;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry, "transcript within margin -> retry");
    }
    {
        // Exhausted the retry budget -> give up (and the caller stops watching + logs).
        auto s = mk(SessionState::WaitingForInput, T, false, kEnterRetryMax);
        const auto r = DecideEnterRetry(s, T + kEnterRetryIntervalMs);
        CHECK(r.action == EnterRetryAction::GiveUp && r.attempt == kEnterRetryMax, "max retries -> give up");
    }
    {
        // Not controllable (no bound injector) -> nothing to re-press Enter on, never retry. This is
        // the observe-only case: an external census claude we hold no stdin for.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.external = true;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs, /*controllable*/ false).action == EnterRetryAction::None, "uncontrollable -> none");
    }
    {
        // Agentmaster (autopilot-on-adopted): an ADOPTED external (external=true) that IS controllable
        // (an injector was bound on adoption) MUST be driven — provenance != controllability. The pure
        // decider keys on `controllable`, not s.external, so an adopted session retries like a launched one.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.external = true;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs, /*controllable*/ true).action == EnterRetryAction::Retry, "adopted + controllable -> retry");
    }
    {
        // Archived (!live) session -> no live claude, never retry.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.live = false;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "archived -> none");
    }
    {
        // A Typed prompt (the human's own keystroke, captured) is never our send to re-press.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.queue[0].origin = PromptOrigin::Typed;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "typed origin -> none");
    }
    {
        // A Pending (not-yet-sent) prompt isn't awaiting pickup.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.queue[0].status = PromptStatus::Pending;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "pending -> none");
    }
    {
        // The LATEST un-acknowledged send is the one watched (a later send supersedes an earlier).
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        QueuedPrompt p2;
        p2.id = L"rp2";
        p2.text = L"go2";
        p2.status = PromptStatus::Sent;
        p2.origin = PromptOrigin::Flight;
        p2.sentAtUnixMs = T + 2000; // sent later
        p2.echoed = false;
        s.queue.push_back(p2);
        const auto r = DecideEnterRetry(s, T + 2000 + kEnterRetryIntervalMs);
        CHECK(r.action == EnterRetryAction::Retry && r.promptId == L"rp2", "latest send is the watched one");
    }
}

// Agentmaster (#6) — BuildPromptSubmission wraps a prompt body in a bracketed paste + ONE trailing CR
// so a multi-line body submits as a single message (not line-by-line, the "submit on first CR" bug).
// Pure + deterministic.
static void TestBuildPromptSubmission()
{
    std::wprintf(L"BuildPromptSubmission (bracketed-paste submit, #6):\n");
    const std::wstring B = L"\x1b[200~"; // bracketed-paste begin
    const std::wstring E = L"\x1b[201~"; // bracketed-paste end

    CHECK(BuildPromptSubmission(L"test 1") == B + L"test 1" + E + L"\r", "single line wrapped + one CR");
    CHECK(BuildPromptSubmission(L"line1\rline2") == B + L"line1\nline2" + E + L"\r", "CR line breaks -> LF inside the paste");
    CHECK(BuildPromptSubmission(L"a\r\nb") == B + L"a\nb" + E + L"\r", "CRLF -> a single LF");
    CHECK(BuildPromptSubmission(L"a\nb") == B + L"a\nb" + E + L"\r", "a bare LF body is preserved");
    CHECK(BuildPromptSubmission(L"") == B + E + L"\r", "empty body is still well-formed");
    {
        // The whole point of #6: exactly ONE submit CR (the trailing one) regardless of line count.
        const auto out = BuildPromptSubmission(L"x\ry\rz");
        size_t crs = 0;
        for (const wchar_t c : out)
        {
            if (c == L'\r')
            {
                ++crs;
            }
        }
        CHECK(crs == 1, "a multi-line body yields exactly one submit CR");
    }
}

// Agentmaster — Scheduler INTEGRATION (the threaded change-driven advance, wired exactly like
// Engine.cpp). The pure DecideAdvance above never exercised the OnObserved -> RequestAdvance ->
// worker -> Inject chain, which is precisely where the "toggle Autopilot Off and back -> pending
// not sent" bug lived: OnObserved gated the change-driven advance on !s.external (provenance)
// instead of HasInjector (controllability), so an ADOPTED session (external=true but injector-bound)
// was never driven. This test drives the real Scheduler thread and polls for the injected result.
static void TestSchedulerIntegration()
{
    std::wprintf(L"Autopilot Scheduler integration (toggle Off->Full; controllability != provenance):\n");

    // Wire a registry + scheduler like Engine.cpp (advance handler + OnObserved observer), seed a
    // live Waiting session with one Pending Flight prompt (throttleMs 0 so the worker injects at
    // once), optionally bind an injector, then toggle Autopilot Off->Full via the registry exactly
    // as AgentManagerContent::_OnAutopilotChanged does. Returns true iff the prompt reached Sent
    // within the poll budget. Each case gets its OWN registry+scheduler so they can't cross-talk.
    auto runToggleCase = [](bool external, bool bindInjector) -> bool {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });

        const std::wstring id = L"sess";
        std::atomic<int> injected{ 0 };
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput; // turn already complete; no future Stop hook fires
            s.live = true;
            s.external = external;
            s.autopilot.mode = AutopilotMode::Off;
            s.autopilot.throttleMs = 0; // inject immediately (no 500ms throttle) -> a short poll suffices
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the pending prompt";
            p.status = PromptStatus::Pending;
            p.origin = PromptOrigin::Flight;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        if (bindInjector)
        {
            reg->SetInjector(id, [&injected](const std::wstring&) { injected.fetch_add(1); });
        }

        // Toggle Off->Full (the user re-enabling Autopilot) — the exact _OnAutopilotChanged write.
        reg->Update(id, [](SessionInfo& s) {
            s.autopilot.mode = AutopilotMode::Full;
            s.autopilot.autoSendsThisRun = 0;
            s.pendingConfirmPromptId.clear();
        });

        // Poll up to ~2s for the worker to pick up + inject (deterministic outcome, bounded wait).
        bool sent = false;
        for (int i = 0; i < 200 && !sent; ++i)
        {
            const auto snap = reg->Get(id);
            sent = snap && !snap->queue.empty() && snap->queue[0].status == PromptStatus::Sent;
            if (!sent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        reg->RemoveObserver(obsTok);
        sched.Stop();
        // A real send marks Sent AND delivers to the injector (when one is bound).
        return sent && (!bindInjector || injected.load() > 0);
    };

    // Manager-Launched (external=false, injector bound): always worked — the regression baseline.
    CHECK(runToggleCase(/*external*/ false, /*injector*/ true), "launched: toggle Off->Full sends pending");
    // Adopted (external=true, injector bound): THE fix — provenance != controllability, must send.
    CHECK(runToggleCase(/*external*/ true, /*injector*/ true), "adopted: toggle Off->Full sends pending");
    // Observe-only external (external=true, NO injector): must NOT send (nothing to drive, no churn).
    CHECK(!runToggleCase(/*external*/ true, /*injector*/ false), "observe-only: toggle does not send (no injector)");

    // --- Question-guard treats a pending question / "needs you" status like a Running mid-turn: the
    //     queued prompt STAYS Pending (never parked in Held, autopilot never paused) and fires only
    //     when the question clears (a later non-question turn-complete). The "reacted to
    //     needs-approval" report (session 2de51dd0): two Flight prompts were Held + stranded. ---
    {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });

        const std::wstring id = L"q-sess";
        std::atomic<int> injected{ 0 };
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput; // turn complete, but...
            s.live = true;
            s.lastMessageWasQuestion = true; // ...the agent ended it asking the user something
            s.autopilot.mode = AutopilotMode::Full;
            s.autopilot.throttleMs = 0;
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the queued prompt";
            p.status = PromptStatus::Pending;
            p.origin = PromptOrigin::Flight;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        reg->SetInjector(id, [&injected](const std::wstring&) { injected.fetch_add(1); });

        // An observed change while the question stands must NOT send — the prompt stays Pending.
        reg->Update(id, [](SessionInfo&) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            const auto snap = reg->Get(id);
            CHECK(snap && snap->queue[0].status == PromptStatus::Pending, "question pending: prompt stays Pending (not Held), not sent");
            CHECK(injected.load() == 0, "question pending: nothing injected while the question stands");
        }
        // The question clears (a non-question turn-complete) -> the queued prompt now fires.
        reg->Update(id, [](SessionInfo& s) { s.lastMessageWasQuestion = false; });
        bool sent = false;
        for (int i = 0; i < 200 && !sent; ++i)
        {
            const auto snap = reg->Get(id);
            sent = snap && snap->queue[0].status == PromptStatus::Sent;
            if (!sent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(sent && injected.load() > 0, "question cleared: the queued prompt fires (was waiting like running)");
        reg->RemoveObserver(obsTok);
        sched.Stop();
    }

    // --- A Held prompt persisted by an OLDER build must rehabilitate, not strand: an observed change
    //     wakes the advance (OnObserved counts Held as work), _process un-holds it to Pending (the
    //     question is gone), and it sends. Without the OnObserved Held-as-work fix it sat dead. ---
    {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });

        const std::wstring id = L"held-sess";
        std::atomic<int> injected{ 0 };
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput;
            s.live = true;
            s.lastMessageWasQuestion = false; // the question is gone — the Held prompt should recover
            s.autopilot.mode = AutopilotMode::Full;
            s.autopilot.throttleMs = 0;
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the held prompt";
            p.status = PromptStatus::Held; // a legacy hold loaded from disk
            p.origin = PromptOrigin::Flight;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        reg->SetInjector(id, [&injected](const std::wstring&) { injected.fetch_add(1); });

        reg->Update(id, [](SessionInfo&) {}); // an enrich/observe tick wakes the advance
        bool sent = false;
        for (int i = 0; i < 200 && !sent; ++i)
        {
            const auto snap = reg->Get(id);
            sent = snap && snap->queue[0].status == PromptStatus::Sent;
            if (!sent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(sent && injected.load() > 0, "legacy Held prompt rehabilitated to Pending and sent (not stranded)");
        reg->RemoveObserver(obsTok);
        sched.Stop();
    }
}

static void TestPersistence()
{
    std::wprintf(L"Persistence (JSON + sessions + templates):\n");

    // JSON round-trip of a small document.
    {
        auto o = json::Value::MkObj();
        o.Set(L"a", json::Value::MkStr(L"he\"llo\n"));
        o.Set(L"n", json::Value::MkNum(42));
        o.Set(L"b", json::Value::MkBool(true));
        auto arr = json::Value::MkArr();
        arr.Push(json::Value::MkNum(1));
        arr.Push(json::Value::MkStr(L"two"));
        o.Set(L"arr", std::move(arr));
        const auto text = json::Dump(o);
        const auto rt = json::Parse(text);
        CHECK(rt.has_value(), "json parses its own dump");
        CHECK(rt && rt->StrAt(L"a") == L"he\"llo\n", "json string escape round-trip");
        CHECK(rt && rt->I64At(L"n") == 42, "json number round-trip");
        CHECK(rt && rt->BoolAt(L"b") == true, "json bool round-trip");
        CHECK(rt && rt->Find(L"arr") && rt->Find(L"arr")->arr.size() == 2, "json array round-trip");
    }

    // enum round-trips.
    CHECK(SessionStateFromString(ToString(SessionState::NeedsApproval)) == SessionState::NeedsApproval, "state enum round-trip");
    CHECK(AutopilotModeFromString(ToString(AutopilotMode::SemiAuto)) == AutopilotMode::SemiAuto, "mode enum round-trip");
    CHECK(PromptStatusFromString(ToString(PromptStatus::Held)) == PromptStatus::Held, "status enum round-trip");
    CHECK(PromptGateFromString(ToString(PromptGate::Manual)) == PromptGate::Manual, "gate enum round-trip");

    // Session round-trip (queue + autopilot preserved, incl. Sent status — no replay).
    {
        SessionInfo s;
        s.id = L"sid-1";
        s.title = L"My Task";
        s.workingDir = L"K:/api";
        s.state = SessionState::WaitingForInput;
        s.lastActivityUnixMs = 123456789;
        s.external = true; // adopted session: must survive the round-trip
        s.forkParentId = L"src-conv-7"; // a never-messaged fork remembers its source across restart (PERSISTED)
        QueuedPrompt a;
        a.id = L"p1";
        a.label = L"add tests";
        a.text = L"please add unit tests";
        a.status = PromptStatus::Sent;
        a.sentAtUnixMs = 999;
        a.origin = PromptOrigin::Typed; // a directly-typed message must survive the round-trip
        QueuedPrompt b;
        b.id = L"p2";
        b.label = L"commit";
        b.text = L"commit it";
        b.gate = PromptGate::Manual;
        b.guardPattern = L"answers-a-question:ok";
        s.queue = { a, b };
        s.autopilot.mode = AutopilotMode::Full;
        s.autopilot.throttleMs = 750;
        s.autopilot.stopOnError = false;
        s.autopilot.maxAutoSends = 7;
        s.autopilot.approval.pauseForHuman = false;
        s.autopilot.approval.autoApproveTools = { L"Read", L"Bash(git *)" };

        const auto text = SerializeSessions({ s });
        const auto back = DeserializeSessions(text);
        CHECK(back.size() == 1, "sessions round-trip count");
        if (back.size() == 1)
        {
            const auto& r = back[0];
            CHECK(r.id == L"sid-1" && r.title == L"My Task" && r.workingDir == L"K:/api", "session metadata");
            CHECK(r.state == SessionState::WaitingForInput, "session state");
            CHECK(r.external, "external flag preserved");
            CHECK(r.forkParentId == L"src-conv-7", "forkParentId preserved (PERSISTED: restores a never-messaged fork)");
            CHECK(r.queue.size() == 2, "queue size");
            CHECK(r.queue.size() == 2 && r.queue[0].status == PromptStatus::Sent && r.queue[0].sentAtUnixMs == 999, "Sent status preserved (no replay)");
            CHECK(r.queue.size() == 2 && r.queue[0].origin == PromptOrigin::Typed && r.queue[1].origin == PromptOrigin::Flight, "prompt origin preserved (Typed vs Flight)");
            CHECK(r.queue.size() == 2 && r.queue[1].gate == PromptGate::Manual && r.queue[1].guardPattern == L"answers-a-question:ok", "prompt gate+guard preserved");
            CHECK(r.autopilot.mode == AutopilotMode::Full && r.autopilot.throttleMs == 750 && !r.autopilot.stopOnError && r.autopilot.maxAutoSends == 7, "autopilot preserved");
            CHECK(!r.autopilot.approval.pauseForHuman && r.autopilot.approval.autoApproveTools.size() == 2, "approval policy preserved");
        }
    }

    // Codex managed session: the agent kind + the rollout resume target (codexSessionId) round-trip,
    // and a default (Claude) session carries NEITHER key, so an all-Claude sessions.json is unchanged.
    {
        SessionInfo cx;
        cx.id = L"codex-handle-1"; // OUR durable handle (the registry / persistence / tab-map key)
        cx.title = L"proxmox homelab";
        cx.workingDir = L"K:/source/proxmox";
        cx.kind = AgentKind::Codex;
        cx.codexSessionId = L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a"; // the codex resume uuid
        SessionInfo claudeDefault;
        claudeDefault.id = L"claude-1";
        const auto back = DeserializeSessions(SerializeSessions({ cx, claudeDefault }));
        CHECK(back.size() == 2, "codex+claude sessions round-trip count");
        if (back.size() == 2)
        {
            CHECK(back[0].kind == AgentKind::Codex, "codex session kind preserved");
            CHECK(back[0].codexSessionId == L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex resume uuid (codexSessionId) preserved");
            CHECK(back[1].kind == AgentKind::Claude && back[1].codexSessionId.empty(), "default session is Claude with no codex uuid");
            CHECK(back[0].forkParentId.empty() && back[1].forkParentId.empty(), "a non-fork session carries no forkParentId (key omitted when empty)");
        }
    }

    // Templates: capture-from-queue resets ids/status; apply assigns fresh ids + Pending.
    {
        std::vector<QueuedPrompt> queue;
        QueuedPrompt a;
        a.id = L"x";
        a.label = L"impl";
        a.text = L"implement";
        a.status = PromptStatus::Sent; // should NOT carry into the template/apply
        queue.push_back(a);

        const auto tmpl = MakeTemplateFromQueue(L"impl-test-fix", queue);
        CHECK(tmpl.name == L"impl-test-fix" && tmpl.prompts.size() == 1, "template captured");
        CHECK(tmpl.prompts.size() == 1 && tmpl.prompts[0].id.empty(), "template prompt id reset");

        const auto ttext = SerializeTemplates({ tmpl });
        const auto tback = DeserializeTemplates(ttext);
        CHECK(tback.size() == 1 && tback[0].name == L"impl-test-fix", "templates round-trip");

        std::vector<QueuedPrompt> target;
        AppendTemplateToQueue(target, tmpl);
        CHECK(target.size() == 1, "apply appends");
        CHECK(target.size() == 1 && !target[0].id.empty() && target[0].status == PromptStatus::Pending, "apply: fresh id + Pending");
        AppendTemplateToQueue(target, tmpl);
        CHECK(target.size() == 2 && target[0].id != target[1].id, "apply twice: distinct ids");
    }
}

static void TestManagerLayout()
{
    std::wprintf(L"Manager layout (splitter geometry persistence):\n");
    auto approx = [](double a, double b) { return (a > b ? a - b : b - a) < 1e-9; };

    // Round-trip of in-band fractions.
    {
        ManagerLayout in;
        in.boardFraction = 0.55;
        in.treeFraction = 0.62;
        const auto out = DeserializeLayout(SerializeLayout(in));
        CHECK(approx(out.boardFraction, 0.55), "layout boardFraction round-trip");
        CHECK(approx(out.treeFraction, 0.62), "layout treeFraction round-trip");
    }

    // Empty / unparseable text -> defaults (no throw, no zero that would vanish a pane).
    {
        const auto out = DeserializeLayout(L"");
        CHECK(approx(out.boardFraction, 0.4) && approx(out.treeFraction, 0.4), "layout defaults on empty");
        const auto out2 = DeserializeLayout(L"not json at all");
        CHECK(approx(out2.boardFraction, 0.4) && approx(out2.treeFraction, 0.4), "layout defaults on garbage");
    }

    // Out-of-band values are rejected back to the default (a corrupt file can't collapse a pane).
    {
        const auto out = DeserializeLayout(L"{\"boardFraction\":0.99,\"treeFraction\":0.0}");
        CHECK(approx(out.boardFraction, 0.4), "layout rejects too-large board -> default");
        CHECK(approx(out.treeFraction, 0.4), "layout rejects zero tree -> default");
        const auto out2 = DeserializeLayout(L"{\"boardFraction\":-3,\"treeFraction\":0.5}");
        CHECK(approx(out2.boardFraction, 0.4), "layout rejects negative board -> default");
        CHECK(approx(out2.treeFraction, 0.5), "layout keeps in-band tree alongside a bad board");
    }

    // A missing field keeps its default; a present in-band field is honored.
    {
        const auto out = DeserializeLayout(L"{\"treeFraction\":0.3}");
        CHECK(approx(out.boardFraction, 0.4), "layout missing board -> default");
        CHECK(approx(out.treeFraction, 0.3), "layout present tree honored");
    }
}

// Agentmaster M10: the per-window WindowRecord — per-window UI state (geometry + lens + ORDERED
// tab refs) layered OVER the session-archive model (sessions.json stays the session source of
// truth, so a Claude tab here is just a sessionId reference, never a copy). Verify the whole
// tree round-trips: geometry (incl. a negative coordinate from a 2nd monitor), ordered Claude
// (ref) + Other (opaque actionsJson) tabs, and the Manager lens. Plus the one-file-per-window
// disk path (Save/Load/Delete), self-cleaning.
static void TestWindowRecord()
{
    std::wprintf(L"Window record (per-window workspace persistence, M10):\n");
    auto approx = [](double a, double b) { return (a > b ? a - b : b - a) < 1e-9; };

    WindowRecord in;
    in.windowId = L"win-guid-1";
    in.geometry.hasPosition = true;
    in.geometry.x = 120;
    in.geometry.y = -40; // a real negative coordinate (2nd monitor) must survive
    in.geometry.hasSize = true;
    in.geometry.width = 1280;
    in.geometry.height = 800;
    in.geometry.launchMode = L"maximized";

    // A Claude tab is a REFERENCE — just the session id (its full record lives in sessions.json).
    TabEntry claude;
    claude.kind = TabKind::Claude;
    claude.tabColor = L"#FFD700";
    claude.sessionId = L"conv-abc";
    in.tabs.push_back(claude);

    // An Other tab is opaque: actionsJson is WT ActionAndArgs — itself JSON, so this also tests
    // nested-JSON-as-a-string escaping survives the round-trip.
    TabEntry other;
    other.kind = TabKind::Other;
    other.actionsJson = L"[{\"action\":\"newTab\",\"profile\":\"pwsh\"}]";
    in.tabs.push_back(other);

    // A Codex tab is a REFERENCE like Claude (the resume uuid lives on the referenced SessionInfo).
    TabEntry codex;
    codex.kind = TabKind::Codex;
    codex.sessionId = L"codex-handle-1";
    in.tabs.push_back(codex);

    // Selected tab persisted by stable identity (Claude conversation id preferred; index is the shell
    // fallback). Both set to non-default values so a dropped field FAILS the round-trip (a -1 default
    // would otherwise mask a missing index).
    in.selectedSessionId = L"conv-abc";
    in.selectedTabIndex = 1;
    in.managerTabColor = L"#3A6EA5"; // the pinned Manager tab's per-window color (non-default so a dropped field fails the round-trip)

    in.manager.selectedId = L"conv-abc";
    in.manager.scopeDir = L"K:/api";
    in.manager.selectedPromptId = L"q1";
    in.manager.collapsedDirs.push_back(L"K:/old");
    in.manager.layout.boardFraction = 0.5;
    in.manager.layout.treeFraction = 0.45;
    in.manager.treeScope = 2; // EXTERNAL — non-default so a dropped field fails the round-trip

    const auto out = DeserializeWindowRecord(SerializeWindowRecord(in));

    CHECK(out.windowId == L"win-guid-1", "record windowId round-trip");
    CHECK(out.geometry.hasPosition && approx(out.geometry.x, 120) && approx(out.geometry.y, -40), "geometry position (negative y) round-trip");
    CHECK(out.geometry.hasSize && approx(out.geometry.width, 1280) && approx(out.geometry.height, 800), "geometry size round-trip");
    CHECK(out.geometry.launchMode == L"maximized", "geometry launchMode round-trip");

    CHECK(out.tabs.size() == 3, "tab count + order preserved");
    if (out.tabs.size() == 3)
    {
        CHECK(out.tabs[0].kind == TabKind::Claude, "tab[0] is Claude");
        CHECK(out.tabs[0].tabColor == L"#FFD700", "Claude tab color round-trip");
        CHECK(out.tabs[0].sessionId == L"conv-abc", "Claude tab sessionId reference round-trip");
        CHECK(out.tabs[1].kind == TabKind::Other, "tab[1] is Other");
        CHECK(out.tabs[1].actionsJson == L"[{\"action\":\"newTab\",\"profile\":\"pwsh\"}]", "Other tab actionsJson (nested JSON) round-trip");
        CHECK(out.tabs[2].kind == TabKind::Codex, "tab[2] is Codex");
        CHECK(out.tabs[2].sessionId == L"codex-handle-1", "Codex tab sessionId reference round-trip");
    }

    CHECK(out.selectedSessionId == L"conv-abc", "selected tab persisted by stable Claude id (round-trip)");
    CHECK(out.selectedTabIndex == 1, "selectedTabIndex shell fallback (round-trip)");
    CHECK(out.managerTabColor == L"#3A6EA5", "managerTabColor (Manager tab's per-window color) round-trip");

    CHECK(out.manager.selectedId == L"conv-abc", "lens selectedId round-trip");
    CHECK(out.manager.scopeDir == L"K:/api", "lens scopeDir round-trip");
    CHECK(out.manager.selectedPromptId == L"q1", "lens selectedPromptId round-trip");
    CHECK(out.manager.collapsedDirs.size() == 1 && out.manager.collapsedDirs[0] == L"K:/old", "lens collapsedDirs round-trip");
    CHECK(approx(out.manager.layout.boardFraction, 0.5) && approx(out.manager.layout.treeFraction, 0.45), "lens splitter fractions round-trip");
    CHECK(out.manager.treeScope == 2, "lens treeScope (shared tree/board scope) round-trip");

    // Tolerant of a missing / corrupt document.
    {
        const auto empty = DeserializeWindowRecord(L"");
        CHECK(empty.windowId.empty() && empty.tabs.empty(), "empty text -> empty record (no throw)");
        const auto garbage = DeserializeWindowRecord(L"}{not json");
        CHECK(garbage.windowId.empty(), "garbage text -> empty record (no throw)");
        // An older record (no treeScope key) and an out-of-range value both land on LOCAL (0).
        const auto legacy = DeserializeWindowRecord(L"{\"windowId\":\"w\",\"manager\":{\"selectedId\":\"s\"}}");
        CHECK(legacy.manager.treeScope == 0, "missing treeScope -> LOCAL (older record)");
        CHECK(legacy.managerTabColor.empty(), "missing managerTabColor -> empty (older record, no Manager-tab color)");
        const auto outOfRange = DeserializeWindowRecord(L"{\"windowId\":\"w\",\"manager\":{\"treeScope\":7}}");
        CHECK(outOfRange.manager.treeScope == 0, "out-of-range treeScope clamps to LOCAL");
    }

    // Disk round-trip (one file per window under .agentmaster\windows\). Uses a sentinel id and
    // cleans up, so it neither collides with nor leaves residue among real window records.
    {
        WindowRecord rec;
        rec.windowId = L"__m5test_window__";
        rec.geometry.hasSize = true;
        rec.geometry.width = 640;
        rec.tabs.push_back(TabEntry{}); // one default (Claude) tab
        SaveWindowRecord(rec);
        bool found = false;
        for (const auto& w : LoadWindowRecords())
        {
            if (w.windowId == L"__m5test_window__")
            {
                found = true;
                CHECK(w.geometry.hasSize && approx(w.geometry.width, 640), "disk record geometry survives Save/Load");
                CHECK(w.tabs.size() == 1, "disk record tabs survive Save/Load");
            }
        }
        CHECK(found, "SaveWindowRecord then LoadWindowRecords finds it");
        DeleteWindowRecord(L"__m5test_window__");
        bool stillThere = false;
        for (const auto& w : LoadWindowRecords())
        {
            stillThere = stillThere || (w.windowId == L"__m5test_window__");
        }
        CHECK(!stillThere, "DeleteWindowRecord prunes the file");
    }
}

static void TestAppSettings()
{
    std::wprintf(L"App settings (the Settings cog):\n");

    // Round-trip every field with non-default values.
    {
        AppSettings in;
        in.skipPermissions = false;
        in.model = L"opus";
        in.includeCoAuthoredBy = false;
        in.defaultAutopilotMode = AutopilotMode::Full;
        in.maxAutoSends = 7;
        in.stopOnError = false;
        in.pauseOnHumanInput = false;
        in.confirmBeforeKill = false;
        in.tabRenameCommitMode = TabRenameCommitMode::ClickAwayOrEnter; // non-default (default is ClickAwayOrShiftEnter)
        in.favoriteIcon = FavoriteIcon::Star; // non-default (default is Crown)
        in.flashRingColor = L"#8000FF00"; // non-default (default #FFFF0000) — 50%-opaque green flash ring (alpha byte = opacity)
        in.defaultLaunchDir = L"K:/work";
        in.env = L"FOO=bar;BAZ=qux";
        in.archiveSplitFraction = 0.33;
        in.summaryPanelWidthFraction = 0.4; // in-band (0.08..0.5)
        in.summaryPanelHeightFraction = 0.6; // in-band (0.06..0.75)
        in.summaryPanelWrapNewlines = true; // non-default (default false = the literal-\n look)
        in.summaryPanelTruncate = false; // non-default (default true = truncate long messages)
        in.showTabCloseButton = false; // non-default (default true = show the X / theme-driven)
        in.closeTabOnMiddleClick = false; // non-default (default true = middle-click closes a tab)
        in.waitingForYouTimeoutMinutes = 0; // 0 = never decay — MUST round-trip as 0, not fall back to the default
        in.serverCacheMinutes = 17; // non-default (default 5) — the ⚡ "still cached" window
        in.treeSort = ExplorerSort::ByPid; // non-default (default Newest) — Explorer Tree sort
        in.boardSort = ExplorerSort::Newest; // non-default (default MostActive) — Triage Board sort
        in.flightPlanShowsSummary = false; // non-default (default true = Summary) — Manager Flight-Plan pane tab
        in.hiddenSessionIds = { L"11111111-1111-1111-1111-111111111111", L"22222222-2222-2222-2222-222222222222" };
        const auto out = DeserializeAppSettings(SerializeAppSettings(in));
        CHECK(out.skipPermissions == false, "settings skipPermissions round-trip");
        CHECK(out.env == L"FOO=bar;BAZ=qux", "settings env round-trip");
        CHECK(out.model == L"opus", "settings model round-trip");
        CHECK(out.includeCoAuthoredBy == false, "settings includeCoAuthoredBy round-trip");
        CHECK(out.defaultAutopilotMode == AutopilotMode::Full, "settings defaultAutopilotMode round-trip");
        CHECK(out.maxAutoSends == 7u, "settings maxAutoSends round-trip");
        CHECK(out.stopOnError == false, "settings stopOnError round-trip");
        CHECK(out.pauseOnHumanInput == false, "settings pauseOnHumanInput round-trip");
        CHECK(out.confirmBeforeKill == false, "settings confirmBeforeKill round-trip");
        CHECK(out.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter, "settings tabRenameCommitMode round-trip");
        CHECK(out.favoriteIcon == FavoriteIcon::Star, "settings favoriteIcon round-trip");
        CHECK(out.flashRingColor == L"#8000FF00", "settings flashRingColor round-trip");
        CHECK(out.defaultLaunchDir == L"K:/work", "settings defaultLaunchDir round-trip");
        CHECK(out.archiveSplitFraction > 0.329 && out.archiveSplitFraction < 0.331, "settings archiveSplitFraction round-trip");
        CHECK(out.summaryPanelWidthFraction > 0.399 && out.summaryPanelWidthFraction < 0.401, "settings summaryPanelWidthFraction round-trip");
        CHECK(out.summaryPanelHeightFraction > 0.599 && out.summaryPanelHeightFraction < 0.601, "settings summaryPanelHeightFraction round-trip");
        CHECK(out.summaryPanelWrapNewlines == true, "settings summaryPanelWrapNewlines round-trip");
        CHECK(out.summaryPanelTruncate == false, "settings summaryPanelTruncate round-trip");
        CHECK(out.showTabCloseButton == false, "settings showTabCloseButton round-trip");
        CHECK(out.closeTabOnMiddleClick == false, "settings closeTabOnMiddleClick round-trip");
        CHECK(out.waitingForYouTimeoutMinutes == 0u, "settings waitingForYouTimeoutMinutes stored 0 (= never) round-trips as 0");
        CHECK(out.serverCacheMinutes == 17u, "settings serverCacheMinutes round-trip");
        CHECK(out.treeSort == ExplorerSort::ByPid, "settings treeSort round-trip");
        CHECK(out.boardSort == ExplorerSort::Newest, "settings boardSort round-trip");
        CHECK(out.flightPlanShowsSummary == false, "settings flightPlanShowsSummary round-trip");
        CHECK(out.hiddenSessionIds.size() == 2 &&
                  out.hiddenSessionIds[0] == L"11111111-1111-1111-1111-111111111111" &&
                  out.hiddenSessionIds[1] == L"22222222-2222-2222-2222-222222222222",
              "settings hiddenSessionIds round-trip (order preserved)");
    }

    // Empty / garbage -> all defaults (a missing settings.json must change nothing).
    {
        const auto out = DeserializeAppSettings(L"");
        CHECK(out.skipPermissions == true && out.includeCoAuthoredBy == true, "settings defaults on empty");
        CHECK(out.defaultAutopilotMode == AutopilotMode::Full && out.maxAutoSends == 100u, "settings autopilot default Full on empty");
        CHECK(out.archiveSplitFraction > 0.499 && out.archiveSplitFraction < 0.501, "settings archiveSplitFraction default 0.5 on empty");
        CHECK(out.summaryPanelWidthFraction == 0.0 && out.summaryPanelHeightFraction == 0.0, "settings summaryPanel size fractions default 0 (auto) on empty");
        CHECK(out.summaryPanelWrapNewlines == false, "settings summaryPanelWrapNewlines default false (literal-\\n look) on empty");
        CHECK(out.summaryPanelTruncate == true, "settings summaryPanelTruncate default true (truncate) on empty");
        CHECK(out.showTabCloseButton == true, "settings showTabCloseButton default true (show X) on empty");
        CHECK(out.closeTabOnMiddleClick == true, "settings closeTabOnMiddleClick default true (middle-click closes) on empty");
        CHECK(out.waitingForYouTimeoutMinutes == 60u, "settings waitingForYouTimeoutMinutes default 60 (1h Waiting-for-you timeout) on empty");
        CHECK(out.serverCacheMinutes == 5u, "settings serverCacheMinutes default 5 (server cache lifetime) on empty");
        CHECK(out.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode default (Shift+Enter) on empty");
        CHECK(out.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon default (Crown) on empty");
        CHECK(out.flashRingColor == L"#FFFF0000", "settings flashRingColor default (opaque red) on empty");
        CHECK(out.treeSort == ExplorerSort::Newest, "settings treeSort default (Newest) on empty");
        CHECK(out.boardSort == ExplorerSort::MostActive, "settings boardSort default (MostActive) on empty");
        CHECK(out.flightPlanShowsSummary == true, "settings flightPlanShowsSummary default (Summary) on empty");
        CHECK(out.hiddenSessionIds.empty(), "settings hiddenSessionIds empty on empty");
        const auto out2 = DeserializeAppSettings(L"not json");
        CHECK(out2.skipPermissions == true && out2.confirmBeforeKill == true, "settings defaults on garbage");
    }

    // Agentmaster: the legacy "waitingDecayMinutes" key was RENAMED to "waitingForYouTimeoutMinutes"
    // because the Waiting-for-you behavior changed (a 5-minute cache window -> a read-gated unread
    // timeout). A pre-existing settings.json carries the OLD key with a value tuned for the old
    // behavior (often 5); it must be INVALIDATED — ignored, falling back to the new 60 (1h) default,
    // NOT carried over as a 5-minute unread timeout. The new key, when present, reads normally.
    {
        const auto legacy = DeserializeAppSettings(L"{\"settings\":{\"waitingDecayMinutes\":5}}");
        CHECK(legacy.waitingForYouTimeoutMinutes == 60u, "settings legacy waitingDecayMinutes key is IGNORED -> 60 (1h) default (rename invalidates the stale value)");
        const auto fresh = DeserializeAppSettings(L"{\"settings\":{\"waitingForYouTimeoutMinutes\":120}}");
        CHECK(fresh.waitingForYouTimeoutMinutes == 120u, "settings new waitingForYouTimeoutMinutes key is read");
    }

    // ExplorerSort token parsing for treeSort/boardSort: each token -> its mode; an unknown token falls
    // back to Newest. boardSort additionally defaults to MostActive when the KEY is absent (above).
    {
        const auto a = DeserializeAppSettings(L"{\"settings\":{\"boardSort\":\"alpha\",\"treeSort\":\"active\"}}");
        CHECK(a.boardSort == ExplorerSort::Alpha, "settings boardSort 'alpha' honored");
        CHECK(a.treeSort == ExplorerSort::MostActive, "settings treeSort 'active' honored");
        const auto act = DeserializeAppSettings(L"{\"settings\":{\"boardSort\":\"active\"}}");
        CHECK(act.boardSort == ExplorerSort::MostActive, "settings boardSort 'active' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"boardSort\":\"bogus\"}}");
        CHECK(bad.boardSort == ExplorerSort::Newest, "settings boardSort unknown token -> Newest");
    }

    // tabRenameCommitMode: each token parses to its mode; an unknown token falls back to the default.
    {
        const auto none = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"clickAway\"}}");
        CHECK(none.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOnly, "settings tabRenameCommitMode 'clickAway' honored");
        const auto ent = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"enter\"}}");
        CHECK(ent.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter, "settings tabRenameCommitMode 'enter' honored");
        const auto se = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"shiftEnter\"}}");
        CHECK(se.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode 'shiftEnter' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"bogus\"}}");
        CHECK(bad.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode unknown -> default (Shift+Enter)");
    }

    // favoriteIcon (FAVORITES.md §5a): each token parses to its glyph; an unknown/absent token falls
    // back to Crown (the prior behavior).
    {
        const auto crown = DeserializeAppSettings(L"{\"settings\":{\"favoriteIcon\":\"crown\"}}");
        CHECK(crown.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon 'crown' honored");
        const auto star = DeserializeAppSettings(L"{\"settings\":{\"favoriteIcon\":\"star\"}}");
        CHECK(star.favoriteIcon == FavoriteIcon::Star, "settings favoriteIcon 'star' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"favoriteIcon\":\"bogus\"}}");
        CHECK(bad.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon unknown -> default (Crown)");
    }

    // A present subset is honored; the rest keep defaults.
    {
        const auto out = DeserializeAppSettings(L"{\"settings\":{\"model\":\"sonnet\",\"maxAutoSends\":3}}");
        CHECK(out.model == L"sonnet", "settings present model honored");
        CHECK(out.maxAutoSends == 3u, "settings present maxAutoSends honored");
        CHECK(out.skipPermissions == true, "settings missing skipPermissions -> default");
    }

    // archiveSplitFraction: a sane value is honored; an extreme/corrupt one falls back to 0.5
    // (the same sane-band rule as the Manager layout fractions — a pane must never collapse).
    {
        const auto ok = DeserializeAppSettings(L"{\"settings\":{\"archiveSplitFraction\":0.7}}");
        CHECK(ok.archiveSplitFraction > 0.699 && ok.archiveSplitFraction < 0.701, "settings archiveSplitFraction honored in-band");
        const auto lo = DeserializeAppSettings(L"{\"settings\":{\"archiveSplitFraction\":0.001}}");
        CHECK(lo.archiveSplitFraction > 0.499 && lo.archiveSplitFraction < 0.501, "settings archiveSplitFraction clamped (too small)");
        const auto hi = DeserializeAppSettings(L"{\"settings\":{\"archiveSplitFraction\":1.5}}");
        CHECK(hi.archiveSplitFraction > 0.499 && hi.archiveSplitFraction < 0.501, "settings archiveSplitFraction clamped (too large)");
    }

    // summaryPanel size fractions (TAB_OVERLAY.md resize): an in-band value is honored; 0 (auto) or an
    // out-of-band/corrupt value falls back to 0 (auto), so a bad value can't wedge the panel at a
    // degenerate size. Bands: width (0.08, 0.5], height (0.06, 0.75].
    {
        const auto ok = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0.45,\"summaryPanelHeightFraction\":0.7}}");
        CHECK(ok.summaryPanelWidthFraction > 0.449 && ok.summaryPanelWidthFraction < 0.451, "settings summaryPanelWidthFraction honored in-band");
        CHECK(ok.summaryPanelHeightFraction > 0.699 && ok.summaryPanelHeightFraction < 0.701, "settings summaryPanelHeightFraction honored in-band");
        const auto zero = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0,\"summaryPanelHeightFraction\":0}}");
        CHECK(zero.summaryPanelWidthFraction == 0.0 && zero.summaryPanelHeightFraction == 0.0, "settings summaryPanel size 0 (auto) preserved");
        const auto wide = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0.9}}");
        CHECK(wide.summaryPanelWidthFraction == 0.0, "settings summaryPanelWidthFraction out-of-band (too wide) -> 0 (auto)");
        const auto tall = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelHeightFraction\":0.95}}");
        CHECK(tall.summaryPanelHeightFraction == 0.0, "settings summaryPanelHeightFraction out-of-band (too tall) -> 0 (auto)");
        const auto tiny = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0.02}}");
        CHECK(tiny.summaryPanelWidthFraction == 0.0, "settings summaryPanelWidthFraction out-of-band (too thin) -> 0 (auto)");
    }
}

static void TestTabNamingAndColor()
{
    std::wprintf(L"[tab naming + per-dir color]\n");

    // --- DeriveSessionTitle: walk up past generic segments, then length/case rules ---
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp") == L"NumSharp", "short non-generic leaf as-is");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\bin\\Debug") == L"NumSharp", "walk up past bin/Debug");
    CHECK(DeriveSessionTitle(L"K:\\proj\\obj\\x64\\Release") == L"proj", "walk up past obj/x64/Release");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\") == L"NumSharp", "trailing slash tolerated");
    CHECK(DeriveSessionTitle(L"K:/source/NumSharp") == L"NumSharp", "forward slashes tolerated");
    CHECK(DeriveSessionTitle(L"C:\\bin") == L"bin", "all-generic falls back to the leaf");
    CHECK(DeriveSessionTitle(L"") == L"claude", "empty dir -> claude");

    // length rules
    CHECK(DeriveSessionTitle(L"C:\\x\\MyProject") == L"MyProject", "<=16 used as-is (mixed case)");
    CHECK(DeriveSessionTitle(L"C:\\x\\abcdefghijklmnop") == L"abcdefghijklmnop", "16 chars used as-is");
    CHECK(DeriveSessionTitle(L"C:\\x\\MyVeryLongProjectName") == L"MVLPN", ">16 mixed-case -> capitals only");
    CHECK(DeriveSessionTitle(L"C:\\x\\myreasonablylongname") == L"myreasonablylongname", ">16 all-lower <=30 as-is");
    {
        const std::wstring leaf(35, L'a');
        CHECK(DeriveSessionTitle(L"C:\\x\\" + leaf) == std::wstring(30, L'a') + L"...", ">30 all-lower truncated with ...");
    }

    // --- DeriveForkTitle: first fork appends " (fork)", forking a fork BUMPS the counter ---
    CHECK(DeriveForkTitle(L"NumSharp") == L"NumSharp (fork)", "first fork appends (fork)");
    CHECK(DeriveForkTitle(L"NumSharp (fork)") == L"NumSharp (fork 2)", "fork of a fork -> (fork 2), not (fork) (fork)");
    CHECK(DeriveForkTitle(L"NumSharp (fork 2)") == L"NumSharp (fork 3)", "(fork 2) -> (fork 3)");
    CHECK(DeriveForkTitle(L"NumSharp (fork 9)") == L"NumSharp (fork 10)", "multi-digit counter increments");
    CHECK(DeriveForkTitle(L"Foo (bar)") == L"Foo (bar) (fork)", "non-fork trailing paren is preserved, gets (fork)");
    CHECK(DeriveForkTitle(L"Foo (1.0)") == L"Foo (1.0) (fork)", "non-numeric-after-fork trailer untouched");
    CHECK(DeriveForkTitle(L"My App (beta) (fork)") == L"My App (beta) (fork 2)", "only the trailing fork group is bumped (nested paren safe)");
    CHECK(DeriveForkTitle(L"") == L" (fork)", "empty source -> ' (fork)' (degenerate; caller derives a base first)");
    CHECK(DeriveForkTitle(L"(fork)") == L"(fork) (fork)", "no prefix before (fork) -> append (not bump; degenerate)");
    CHECK(DeriveForkTitle(L"x (fork )") == L"x (fork ) (fork)", "trailing space inside the group is not a counter -> append");

    // --- IsGenericDirName (case-folded) ---
    CHECK(IsGenericDirName(L"bin") && IsGenericDirName(L"BIN") && IsGenericDirName(L"Obj"), "generic names case-insensitive");
    CHECK(!IsGenericDirName(L"NumSharp"), "project name not generic");

    // --- NormDirKey: slash + trailing + (windows) case fold collapse to one key ---
    CHECK(NormDirKey(L"K:\\A\\B\\") == NormDirKey(L"K:/a/b"), "case/slash/trailing variants share a key");

    // --- AutoDirColorHex (preview): deterministic, palette form, stable across path spelling ---
    SeedDirColors(0xA11CE5EEull); // fix the seed so probe orders are deterministic in this test
    const auto c1 = AutoDirColorHex(L"K:\\source\\NumSharp");
    CHECK(c1.size() == 7 && c1[0] == L'#', "auto color is #RRGGBB");
    CHECK(c1 == AutoDirColorHex(L"K:\\source\\NumSharp"), "auto color deterministic");
    CHECK(c1 == AutoDirColorHex(L"k:/source/numsharp"), "auto color stable across spelling");

    // --- ChooseDirColor (PURE permanent allocator; no disk): permanence, uniqueness, exhaustion reset ---
    {
        const uint64_t seed = 0xA11CE5EEull;
        using Map = std::vector<std::pair<std::wstring, std::wstring>>;

        // (0) An already-assigned folder keeps its color — permanence (independent of seed/open set).
        const Map one = { { L"k:\\a", L"#123456" } };
        CHECK(ChooseDirColor(L"k:\\a", one, {}, seed) == L"#123456", "assigned folder keeps its color (permanent)");
        CHECK(ChooseDirColor(L"k:\\a", one, {}, 999ull) == L"#123456", "permanence is seed-independent");

        // (1) Assigning palette-many folders in turn (each appended to the map) yields all-DISTINCT
        // colors — the "two folders, same color" fix: a new folder avoids every color already assigned.
        const size_t n = 14; // == kAutoPalette size in Persistence.cpp
        Map existing;
        std::unordered_set<std::wstring> seen;
        bool distinct = true;
        for (size_t i = 0; i < n; ++i)
        {
            const std::wstring key = NormDirKey(L"k:\\fleet\\dir" + std::to_wstring(i));
            const auto c = ChooseDirColor(key, existing, {}, seed);
            existing.emplace_back(key, c);
            if (!seen.insert(c).second)
            {
                distinct = false;
            }
        }
        CHECK(distinct && seen.size() == n, "palette-many folders get distinct colors (collision-free)");

        // (2) Palette exhausted (all 14 assigned): a new folder RESETS the collection and reuses, but
        // avoids the colors open tabs are actively showing.
        const std::wstring activeHex = existing.front().second;
        const auto reused = ChooseDirColor(L"k:\\fleet\\extra", existing, { activeHex }, seed);
        CHECK(reused.size() == 7 && reused[0] == L'#', "exhausted -> still a palette color");
        CHECK(reused != activeHex, "exhausted reuse avoids an actively-shown color");

        // (3) Every palette color is active (more open dirs than colors): reuse is unavoidable, but the
        // result is still a valid palette color.
        std::unordered_set<std::wstring> allActive;
        for (const auto& [k, h] : existing)
        {
            allActive.insert(h);
        }
        const auto forced = ChooseDirColor(L"k:\\fleet\\extra2", existing, allActive, seed);
        CHECK(forced.size() == 7 && forced[0] == L'#', "all-active -> fallback is still a palette color");
    }

    // --- DeCollideDirColors (v1->v2 migration core): fix duplicate palette colors, keep user picks ---
    {
        const std::vector<std::pair<std::wstring, std::wstring>> in = {
            { L"k:\\desktop", L"#E06C75" }, // palette
            { L"k:\\eli", L"#E06C75" }, // dup of desktop -> reassign
            { L"k:\\numsharp", L"#2BBAC5" }, // palette
            { L"k:\\grimes", L"#2BBAC5" }, // dup of numsharp -> reassign
            { L"k:\\agentmaster", L"#9E696A" }, // off-palette user pick -> keep verbatim
        };
        const auto out = DeCollideDirColors(in, 0xD3C0DEull);
        CHECK(out.size() == in.size(), "de-collide keeps every folder");
        const auto colorOf = [&](const std::wstring& key) -> std::wstring {
            for (const auto& [k, h] : out)
            {
                if (k == key)
                {
                    return h;
                }
            }
            return L"";
        };
        CHECK(colorOf(L"k:\\desktop") == L"#E06C75", "first occurrence keeps its palette color");
        CHECK(colorOf(L"k:\\numsharp") == L"#2BBAC5", "first occurrence keeps its palette color (2)");
        CHECK(colorOf(L"k:\\agentmaster") == L"#9E696A", "off-palette user pick preserved verbatim");
        CHECK(colorOf(L"k:\\eli") != L"#E06C75", "duplicate reassigned away from desktop");
        CHECK(colorOf(L"k:\\grimes") != L"#2BBAC5", "duplicate reassigned away from numsharp");
        std::unordered_set<std::wstring> palSeen;
        bool allUnique = true;
        for (const auto& [k, h] : out)
        {
            if (h == L"#9E696A")
            {
                continue; // off-palette user pick — not a palette slot
            }
            if (!palSeen.insert(h).second)
            {
                allUnique = false;
            }
        }
        CHECK(allUnique, "de-collide leaves no two folders sharing a palette color");
    }
    CHECK(SerializeDirColors({}).find(L"\"version\":2") != std::wstring::npos, "dir-colors serializes at version 2");

    // --- DirColors serialize round-trip (pure; no disk) ---
    {
        std::vector<std::pair<std::wstring, std::wstring>> in = {
            { L"k:\\a", L"#112233" }, { L"k:\\b", L"#AABBCC" }
        };
        const auto out = DeserializeDirColors(SerializeDirColors(in));
        CHECK(out.size() == 2, "dir-colors round-trip count");
        CHECK(out.size() == 2 && out[0].first == L"k:\\a" && out[0].second == L"#112233", "dir-colors round-trip [0]");
        CHECK(out.size() == 2 && out[1].first == L"k:\\b" && out[1].second == L"#AABBCC", "dir-colors round-trip [1]");
    }
    CHECK(DeserializeDirColors(L"").empty(), "empty dir-colors -> empty");
    CHECK(DeserializeDirColors(L"not json").empty(), "garbage dir-colors -> empty");
}

static void TestTranscriptScan()
{
    std::wprintf(L"Transcript reconciler (interval scan):\n");

    // assistant line: content array with a text block + stop_reason -> one Assistant event
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"All done."}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1, "assistant line -> 1 event");
        CHECK(!r.events.empty() && r.events[0].kind == TranscriptEvent::Kind::Assistant, "assistant kind");
        CHECK(!r.events.empty() && r.events[0].text == L"All done.", "assistant text collected");
        CHECK(!r.events.empty() && r.events[0].stopReason == L"end_turn", "assistant stop_reason captured");
        CHECK(r.consumed == line.size(), "consumed the full complete line");
        CHECK(!r.events.empty() && !r.events[0].apiError, "normal assistant line is NOT an apiError");
    }
    // Agentmaster: the synthetic API-error turn-ender — top-level isApiErrorMessage:true, model
    // "<synthetic>", a terminal stop_reason. The parser flags ev.apiError so the scanner can produce
    // SessionState::Error instead of reading the terminal stop as a clean turn-complete. (Mirrors the
    // real c66ec7c8 "Server is temporarily limiting requests" rate-limit shape.)
    {
        const std::wstring line = LR"j({"type":"assistant","isApiErrorMessage":true,"apiErrorStatus":429,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"API Error: Server is temporarily limiting requests (not your usage limit) · Rate limited"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::Assistant, "apiError line -> 1 assistant event");
        CHECK(!r.events.empty() && r.events[0].apiError, "isApiErrorMessage:true -> ev.apiError");
        CHECK(!r.events.empty() && r.events[0].apiErrorStatus == 429, "apiErrorStatus captured (the HTTP code)");
        CHECK(!r.events.empty() && r.events[0].text.find(L"Rate limited") != std::wstring::npos, "apiError text captured (the reason)");
    }
    // A client-side error (no HTTP status, e.g. "Prompt is too long") -> apiError true, status 0.
    {
        const std::wstring line = LR"j({"type":"assistant","isApiErrorMessage":true,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"Prompt is too long"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(!r.events.empty() && r.events[0].apiError && r.events[0].apiErrorStatus == 0, "client-side apiError -> status 0 (no HTTP code)");
    }
    // Agentmaster (active-leaf tracking): a {"type":"last-prompt","leafUuid":…} marker -> a LeafMarker
    // event carrying the leafUuid (the active branch head). NOT a turn event — it never affects the
    // missed-Stop / run-repair logic; it lets the scanner tell that an API error rewound off the leaf.
    {
        const std::wstring line = LR"j({"type":"last-prompt","lastPrompt":"audit the diff","leafUuid":"cdb0b74e-d0ae-47b6-a060-fa389e4675e7"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::LeafMarker, "last-prompt -> 1 LeafMarker event");
        CHECK(!r.events.empty() && r.events[0].text == L"cdb0b74e-d0ae-47b6-a060-fa389e4675e7", "LeafMarker carries the leafUuid in text");
    }
    // A last-prompt with NO leafUuid (the trust-dialog prompt) carries no leaf -> emit nothing (so it
    // never clobbers the tracked active leaf to empty).
    {
        const std::wstring line = LR"j({"type":"last-prompt","lastPrompt":"Accessing workspace: trust?","sessionId":"x"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "last-prompt without leafUuid -> no LeafMarker event (never clobbers the leaf)");
    }
    // assistant tool_use turn: stop_reason tool_use, no text (turn NOT complete -> no synth Stop)
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].stopReason == L"tool_use", "tool_use stop_reason");
        CHECK(r.events.size() == 1 && r.events[0].text.empty(), "tool_use line has no text");
    }
    // user line: plain string content -> a UserPrompt
    {
        const std::wstring line = LR"j({"type":"user","message":{"role":"user","content":"hello there"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::UserPrompt, "user string -> prompt");
        CHECK(r.events.size() == 1 && r.events[0].text == L"hello there", "user prompt text");
    }
    // user line: tool_result array -> NOT a human prompt (no false positive); it is a ToolResult
    // marker (a tool completed -> answers a pending interactive tool_use), never a UserPrompt.
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"x","content":"ok"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::ToolResult, "tool_result user line -> ToolResult marker, not a prompt");
    }
    // user line: isMeta -> skipped
    {
        const std::wstring line = LR"j({"type":"user","isMeta":true,"message":{"content":"<command-reminder>"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "meta user line -> skipped");
    }
    // partial trailing line: only the complete line is parsed; consumed stops at the last newline
    {
        const std::wstring chunk = LR"j({"type":"user","message":{"content":"first"}})j" L"\n" LR"j({"type":"user","message":{"content":"par)j";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.events.size() == 1 && r.events[0].text == L"first", "partial tail: only the complete line parsed");
        CHECK(r.consumed == chunk.find(L'\n') + 1, "partial tail: consumed stops at the last newline");
    }
    // CRLF + leading blank line tolerated
    {
        const std::wstring chunk = std::wstring{ L"\r\n" } + LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok?"}]}})j" + L"\r\n";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.events.size() == 1 && r.events[0].text == L"ok?", "CRLF + blank line tolerated");
    }
    // garbage / non-JSON lines are skipped, not fatal
    {
        const auto r = ParseTranscriptDelta(L"not json at all\n{bad\n");
        CHECK(r.events.empty(), "garbage transcript lines -> no events (no throw)");
    }
    // ORDER contract: events come back in transcript order. The scanner's missed-Stop fix relies
    // on processing an assistant(end_turn) THEN a following user(prompt) in order, so the new
    // human turn clears the stale end_turn before it could trigger a premature synthesized Stop.
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]}})j" } + L"\n" +
            LR"j({"type":"user","message":{"content":"next please"}})j" + L"\n";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.events.size() == 2, "mixed delta -> 2 events");
        CHECK(r.events.size() == 2 && r.events[0].kind == TranscriptEvent::Kind::Assistant && r.events[0].stopReason == L"end_turn", "order: assistant end_turn first");
        CHECK(r.events.size() == 2 && r.events[1].kind == TranscriptEvent::Kind::UserPrompt && r.events[1].text == L"next please", "order: user prompt second (clears stale end_turn)");
    }

    // away_summary (Claude Code's idle RECAP): a system line is captured into .recap (normalized —
    // the "(disable recaps in /config)" hint stripped), is NOT a turn event, and never strands the
    // state machine. The LAST recap in a chunk wins; a recap-less chunk leaves .recap empty so the
    // scanner can never clear a good recap with a blank.
    {
        const std::wstring line = LR"j({"type":"system","subtype":"away_summary","content":"We fixed the build. Next: deploy. (disable recaps in /config)"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "recap: away_summary is NOT a turn event");
        CHECK(r.recap == L"We fixed the build. Next: deploy.", "recap: away_summary captured + disable hint stripped");
    }
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"system","subtype":"away_summary","content":"first recap"})j" } + L"\n" +
            LR"j({"type":"system","subtype":"turn_duration","content":"ignored"})j" + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"second recap"})j" + L"\n";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.recap == L"second recap", "recap: the LAST away_summary in a chunk wins; a non-away system subtype is ignored");
    }
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]}})j" L"\n";
        CHECK(ParseTranscriptDelta(line).recap.empty(), "recap: a recap-less chunk yields an empty .recap (never clears a stored recap)");
    }

    // NoteExternalPrompt: dedups against an existing Sent (the hook already recorded the message),
    // but records a genuinely missed one, tagged Typed/Sent.
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"s1"));
        reg.OnHookEvent(UPS(L"s1", L"typed by human")); // hook path records a Typed (Sent) entry
        const auto before = reg.Get(L"s1");
        const size_t n0 = before ? before->queue.size() : 0;
        reg.NoteExternalPrompt(L"s1", L"typed by human"); // reconciler sees the same line -> skip
        const auto after = reg.Get(L"s1");
        CHECK(after && after->queue.size() == n0, "NoteExternalPrompt dedups vs an existing Sent");
        reg.NoteExternalPrompt(L"s1", L"a dropped-hook message"); // genuinely new -> recorded
        const auto after2 = reg.Get(L"s1");
        CHECK(after2 && after2->queue.size() == n0 + 1, "NoteExternalPrompt records a missed message");
        bool taggedTyped = false;
        if (after2)
        {
            for (const auto& p : after2->queue)
            {
                if (p.text == L"a dropped-hook message")
                {
                    taggedTyped = (p.origin == PromptOrigin::Typed && p.status == PromptStatus::Sent);
                }
            }
        }
        CHECK(taggedTyped, "reconciled prompt tagged Typed/Sent");
        reg.NoteExternalPrompt(L"s1", L""); // empty -> no-op
        const auto after3 = reg.Get(L"s1");
        CHECK(after3 && after3->queue.size() == n0 + 1, "NoteExternalPrompt ignores empty text");
    }

    // Missed/folded-UserPromptSubmit repair — the PURE gate (ShouldSynthesizeRunning), the Running
    // mirror of the missed-Stop synthesis: fires only off a freshly-appended turn event consumed by
    // an already-PRIMED cursor (the initial history replay never counts) whose tail says a turn is
    // in progress, and only out of the two states a missed prompt strands a session in.
    {
        CHECK(ShouldSynthesizeRunning(SessionState::WaitingForInput, true, true, L"", 500), "run-repair: fresh user line + Waiting -> synthesize");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, true, L"tool_use", 500), "run-repair: assistant mid-turn line + Idle -> synthesize");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, true, L"", -200), "run-repair: future mtime (clock skew) counts as fresh");
        CHECK(!ShouldSynthesizeRunning(SessionState::Running, true, true, L"", 500), "run-repair: already Running -> no-op");
        CHECK(!ShouldSynthesizeRunning(SessionState::NeedsApproval, true, true, L"", 500), "run-repair: NeedsApproval never cleared by a transcript line");
        // Agentmaster (API-error recovery — the PULL half of "come out of Error on first change"): a
        // fresh in-progress turn event after an API error IS the user retrying, so Error recovers to
        // Running. But a TERMINAL tail (the error line's own stop_sequence) must NOT self-recover, an
        // unprimed history replay must not, and a quiet pass (no new event) must keep it Error.
        CHECK(ShouldSynthesizeRunning(SessionState::Error, true, true, L"", 500), "run-repair: Error + fresh in-progress turn event -> recovers to Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, true, true, L"end_turn", 500), "run-repair: Error + terminal tail -> NOT Running (the error's own stop_reason must not self-recover)");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, false, true, L"", 500), "run-repair: Error + no new event this pass -> stays Error");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, true, false, L"", 500), "run-repair: Error + unprimed (history replay) -> no spurious recovery");
        CHECK(!ShouldSynthesizeRunning(SessionState::Done, true, true, L"", 500), "run-repair: Done never revived");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, true, L"end_turn", 500), "run-repair: end_turn tail is missed-Stop territory, not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, false, true, L"", 500), "run-repair: no new turn event this pass -> no synthesis");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, true, L"", kScanRunRepairFreshMs + 1), "run-repair: stale write (late scan) -> no synthesis");
        // THE window-restore bug: a session closed MID-TURN and resumed leaves a transcript whose
        // history ends "turn in progress" with a FRESH mtime; the first scanner pass replays it
        // from offset 0 (cursor not yet primed) — that replay must NOT light the idle, just-resumed
        // claude Running (it then STUCK blue: recon-stop needs a terminal-stop tail to clear it).
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, false, L"", 500), "run-repair: unprimed cursor (history replay) -> no synthesis even when FRESH");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, false, L"tool_use", 500), "run-repair: unprimed replay of a mid-turn tail (killed mid-turn) -> no synthesis");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, false, L"", -200), "run-repair: unprimed beats even a future mtime");
        // #3 (recon gates were end_turn-ONLY): every TERMINAL stop_reason ends the turn, not just
        // end_turn — a stop_sequence/max_tokens/refusal-ended turn previously read as "in
        // progress" here (wrongly re-lighting Running off its own tail) and never qualified for
        // the missed-Stop synthesis (stuck Running forever). An UNKNOWN reason stays in-flight —
        // the pre-existing default (everything != end_turn), e.g. pause_turn genuinely continues.
        CHECK(IsTerminalStopReason(L"end_turn") && IsTerminalStopReason(L"stop_sequence") && IsTerminalStopReason(L"max_tokens") && IsTerminalStopReason(L"refusal"), "stop-reason: all four terminal reasons recognized");
        CHECK(!IsTerminalStopReason(L"tool_use") && !IsTerminalStopReason(L"") && !IsTerminalStopReason(L"pause_turn"), "stop-reason: mid-turn / empty / unknown read as in-flight");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, true, L"stop_sequence", 500), "run-repair: stop_sequence tail ended the turn -> not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, true, L"max_tokens", 500), "run-repair: max_tokens tail ended the turn -> not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, true, L"refusal", 500), "run-repair: refusal tail ended the turn -> not Running");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, true, L"pause_turn", 500), "run-repair: an unknown stop_reason stays in-flight (old default)");
    }
    // Agentmaster (API-error reconciliation — ShouldSynthesizeError): the tail is an unrecovered API
    // error (lastWasApiError) and the transcript has settled -> Error. Idempotent (never re-fires from
    // Error), never from Done, never without the error tail, and gated on the same quiescence as the
    // missed-Stop (a fast retry clears the tail first). Fires defensively from Waiting/Idle too (the
    // real Stop hook for the errored turn, or an earlier pass, may already have moved it there).
    {
        CHECK(ShouldSynthesizeError(SessionState::Running, true, kScanStopQuiescenceMs), "api-error: Running + error tail + quiet -> Error");
        CHECK(ShouldSynthesizeError(SessionState::WaitingForInput, true, kScanStopQuiescenceMs), "api-error: Waiting (push Stop landed) + error tail -> Error");
        CHECK(ShouldSynthesizeError(SessionState::Idle, true, kScanStopQuiescenceMs), "api-error: Idle + error tail -> Error (defensive)");
        CHECK(ShouldSynthesizeError(SessionState::NeedsApproval, true, kScanStopQuiescenceMs), "api-error: NeedsApproval + error tail -> Error");
        CHECK(!ShouldSynthesizeError(SessionState::Running, true, kScanStopQuiescenceMs - 1), "api-error: not quiet long enough -> wait (a fast retry clears the tail first)");
        CHECK(!ShouldSynthesizeError(SessionState::Running, false, kScanStopQuiescenceMs), "api-error: not the active leaf -> no Error");
        CHECK(!ShouldSynthesizeError(SessionState::Error, true, kScanStopQuiescenceMs), "api-error: already Error -> idempotent (never re-fires)");
        CHECK(!ShouldSynthesizeError(SessionState::Done, true, kScanStopQuiescenceMs), "api-error: a cleanly-ended (Done) session never flips to Error");
    }
    // Agentmaster (active-leaf distinction — ApiErrorIsActiveLeaf): the error is "the last message" only
    // when it is positionally newest (lastWasApiError) AND the active leaf has not moved since it
    // appended (activeLeafUuid == errorEpochLeaf). The user's exact ask: a double-ESC REWIND past the
    // error repoints the leaf (a last-prompt naming a different uuid) with NO turn event, so the error is
    // off the active branch even though it stays physically last.
    {
        // Normal error tail: leaf unchanged since the error -> the active leaf.
        CHECK(ApiErrorIsActiveLeaf(true, L"p", L"p"), "leaf: error is the active leaf (leaf unmoved since the error)");
        // Rewind past the error: a later last-prompt repointed the leaf -> NOT the active leaf.
        CHECK(!ApiErrorIsActiveLeaf(true, L"p0", L"p"), "leaf: rewind moved the leaf off the error -> not the active leaf");
        // No turn-positional error at all -> never the active leaf (a later turn event cleared it).
        CHECK(!ApiErrorIsActiveLeaf(false, L"p", L"p"), "leaf: positional flag cleared (a turn event) -> not the active leaf");
        // No last-prompt marker ever seen (older/subagent transcript): both "" -> positional governs.
        CHECK(ApiErrorIsActiveLeaf(true, L"", L""), "leaf: no marker seen -> falls back to positional (both empty)");
        // The full ShouldSynthesizeError gate now keys on the leaf-refined bool: a rewound error never
        // enters Error even when positionally newest + quiet.
        CHECK(!ShouldSynthesizeError(SessionState::Running, ApiErrorIsActiveLeaf(true, L"p0", L"p"), kScanStopQuiescenceMs),
              "leaf: a rewound error (positionally newest but off-leaf) does NOT enter Error");
        CHECK(ShouldSynthesizeError(SessionState::Running, ApiErrorIsActiveLeaf(true, L"p", L"p"), kScanStopQuiescenceMs),
              "leaf: an on-leaf error DOES enter Error");
    }
    // Agentmaster (Error RELEASE on a leaf move — ShouldReleaseErrorOnLeafMove): a session stuck in Error
    // leaves when the leaf rewinds off the error WITHOUT a turn event (the pure rewind-and-sit). Distinct
    // from recon-run / push (which own the user-RETRIED case, where lastWasApiError is already cleared).
    {
        // The rewind case: in Error, error still positionally last, but the leaf moved + quiet -> release.
        CHECK(ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"p0", L"p", kScanStopQuiescenceMs),
              "release: Error + rewind (leaf moved) + quiet -> release to Waiting");
        // Not quiet yet (the rewind just wrote the marker) -> wait for the settle window.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"p0", L"p", kScanStopQuiescenceMs - 1),
              "release: not quiet long enough -> wait");
        // Leaf has NOT moved (error still the active leaf) -> stay Error.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"p", L"p", kScanStopQuiescenceMs),
              "release: error still the active leaf -> stay Error");
        // Positional flag cleared (a turn event) -> recon-run / push owns recovery, not this path.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, false, L"p0", L"p", kScanStopQuiescenceMs),
              "release: positional cleared (turn event) -> recon-run/push owns it, not the leaf release");
        // Only from Error: a non-Error state has nothing to release here.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Running, true, L"p0", L"p", kScanStopQuiescenceMs),
              "release: only from Error (a Running session is not stuck)");
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::WaitingForInput, true, L"p0", L"p", kScanStopQuiescenceMs),
              "release: already left Error (Waiting) -> no-op (no re-fire)");
    }
    // The synthesized event's effect through the ONE state machine: UserPromptSubmit-shaped, ts
    // stamped (refreshes the decay anchor), EMPTY promptText (no Flight-Plan side effects — the
    // prompt back-fill stays NoteExternalPrompt's job).
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"run1", SessionState::WaitingForInput));
        HookMessage synth = Msg(L"run1", HookEvent::UserPromptSubmit);
        synth.ts = 777;
        reg.OnHookEvent(synth);
        const auto got = reg.Get(L"run1");
        CHECK(got && got->state == SessionState::Running, "synthesized UserPromptSubmit -> Running (one state machine)");
        CHECK(got && got->lastActivityUnixMs == 777, "synthesized ts stamps lastActivityUnixMs (decay anchor refreshed)");
        CHECK(got && got->queue.empty(), "empty promptText -> no Flight-Plan entry recorded");
    }
    // Agentmaster (API-error synth through the registry): the scanner's [recon-error] event (a
    // Notification carrying apiError) lands SessionState::Error, and the session then COMES OUT of Error
    // on the first new turn event (a UserPromptSubmit -> Running) — the full produce + recover cycle.
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"err1", SessionState::Running));
        HookMessage synthErr = Msg(L"err1", HookEvent::Notification);
        synthErr.apiError = true;
        synthErr.errorMessage = L"API Error: Server is temporarily limiting requests · Rate limited";
        synthErr.errorStatus = 429;
        synthErr.ts = 1000;
        reg.OnHookEvent(synthErr);
        const auto errored = reg.Get(L"err1");
        CHECK(errored && errored->state == SessionState::Error, "recon-error synth -> Error (through the one state machine)");
        // The reason is PRESERVED on the record for the Triage-Board Error card (message + HTTP code).
        CHECK(errored && errored->errorMessage.find(L"Rate limited") != std::wstring::npos && errored->errorStatus == 429,
              "Error preserves errorMessage + errorStatus for the card");
        // First change: the user retries. UserPromptSubmit -> Running (come out of Error on first change).
        HookMessage retry = Msg(L"err1", HookEvent::UserPromptSubmit);
        retry.ts = 2000;
        reg.OnHookEvent(retry);
        const auto recovered = reg.Get(L"err1");
        CHECK(recovered && recovered->state == SessionState::Running, "Error leaves on the first new turn event (UserPromptSubmit -> Running)");
        // Recovery CLEARS the preserved reason, so a recovered card never shows a stale error.
        CHECK(recovered && recovered->errorMessage.empty() && recovered->errorStatus == 0, "recovery clears errorMessage + errorStatus");
    }
    // UpdateQuiet mutates the record (and, by contract, fires no observer — exercised here for the mutation)
    {
        SessionRegistry reg;
        int observed = 0;
        reg.AddObserver([&](const SessionInfo&, HookEvent) { ++observed; });
        reg.Upsert(MakeSession(L"s2")); // Upsert notifies -> observed == 1
        const int afterUpsert = observed;
        reg.UpdateQuiet(L"s2", [](SessionInfo& s) { s.lastAssistantText = L"peek"; });
        const auto s = reg.Get(L"s2");
        CHECK(s && s->lastAssistantText == L"peek", "UpdateQuiet mutates the record");
        CHECK(observed == afterUpsert, "UpdateQuiet fires NO observer (no persist/UI churn)");
    }
}

// Agentmaster — the stuck-state reconcilers (proved against 4 live Desktop sessions):
//   #1 an unanswered AskUserQuestion left the session BLOCKED on the user but showing Running
//      forever (a "tool_use" tail is non-terminal, so the missed-Stop backstop never fired);
//   #2 an INTERRUPTED turn (Esc) showed Running forever (no clean Stop hook, and the interrupt
//      marker cleared the stop_reason, disarming the backstop);
//   and the original report: a NeedsApproval whose post-approval Stop hook was DROPPED stayed
//   NeedsApproval (the missed-Stop backstop was Running-ONLY). All three now reconcile.
static void TestBlockedAndInterruptedStates()
{
    std::wprintf(L"Blocked-on-user / interrupted / needs-approval-exit reconcilers:\n");

    // --- pure decision helpers ---
    CHECK(IsUserInterruptMarker(L"[Request interrupted by user for tool use]"), "interrupt: tool-use variant recognized");
    CHECK(IsUserInterruptMarker(L"[Request interrupted by user]"), "interrupt: bare variant recognized");
    CHECK(!IsUserInterruptMarker(L"please don't interrupt me"), "interrupt: ordinary prompt is not a marker");
    CHECK(IsInteractiveTool(L"AskUserQuestion"), "interactive: AskUserQuestion blocks on the user");
    CHECK(!IsInteractiveTool(L"Bash") && !IsInteractiveTool(L""), "interactive: Bash / none do not block");

    // recon-stop: fires from Running OR NeedsApproval, on a terminal stop_reason OR an interrupt,
    // only once quiescent.
    CHECK(ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 5000), "stop: Running + terminal tail + quiet -> stop");
    CHECK(ShouldSynthesizeStop(SessionState::Running, L"", true, 5000), "stop: Running + interrupt + quiet -> stop (#2 fix)");
    CHECK(ShouldSynthesizeStop(SessionState::NeedsApproval, L"end_turn", false, 5000), "stop: NeedsApproval + terminal tail -> stop (original fix)");
    CHECK(!ShouldSynthesizeStop(SessionState::Running, L"tool_use", false, 5000), "stop: a pending tool_use tail is NOT the turn's end");
    CHECK(!ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 1000), "stop: not quiescent yet -> hold");
    CHECK(!ShouldSynthesizeStop(SessionState::Idle, L"end_turn", false, 5000), "stop: an Idle session has no turn to end");
    CHECK(!ShouldSynthesizeStop(SessionState::WaitingForInput, L"end_turn", false, 5000), "stop: already settled -> no-op");

    // recon-block: an unanswered interactive tool_use, only from Running, only once quiescent.
    CHECK(ShouldSynthesizeBlockedOnUser(SessionState::Running, L"AskUserQuestion", false, 5000), "block: Running + unanswered question + quiet -> NeedsApproval (#1 fix)");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"", false, 5000), "block: no pending interactive tool -> no-op");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"Bash", false, 5000), "block: a pending Bash is WORKING, not blocked");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"AskUserQuestion", true, 5000), "block: an interrupt takes precedence (-> stop)");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::NeedsApproval, L"AskUserQuestion", false, 5000), "block: idempotent — never re-fires while already NeedsApproval");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"AskUserQuestion", false, 1000), "block: not quiescent yet -> hold");

    // recon-resume: the NeedsApproval -> Running edge that was MISSING — a session answered MID-turn
    // (the question/approval resolved + the agent working again) used to show "needs you" (orange)
    // until end-of-turn. Fires only from NeedsApproval, off a fresh primed turn event, with the
    // pending interactive tool cleared and a non-terminal/non-interrupted tail (mutually exclusive
    // with recon-stop). Signature: (state, consumedTurnEvent, primedBeforePass, pendingTool, lastStop, interrupted, sinceWriteMs).
    CHECK(ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"tool_use", false, 500), "resume: answered + working again (pending tool) -> Running");
    CHECK(ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"", false, 500), "resume: answered + plain assistant text (no stop_reason) -> Running");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"AskUserQuestion", L"tool_use", false, 500), "resume: a NEW unanswered question still pending -> stay NeedsApproval");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, false, true, L"", L"tool_use", false, 500), "resume: no new turn event this pass -> no synthesis");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, false, L"", L"tool_use", false, 500), "resume: unprimed cursor (history replay) -> no synthesis");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"end_turn", false, 500), "resume: terminal tail = turn ended -> recon-stop's job (Waiting), not Running");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"", true, 500), "resume: an interrupt ended the turn -> recon-stop (Waiting), not a resume");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"tool_use", false, kScanRunRepairFreshMs + 1), "resume: stale write (late scan) -> no synthesis");
    CHECK(!ShouldSynthesizeResumed(SessionState::Running, true, true, L"", L"tool_use", false, 500), "resume: already Running -> no-op");
    CHECK(!ShouldSynthesizeResumed(SessionState::Idle, true, true, L"", L"tool_use", false, 500), "resume: Idle is recon-run's domain, not resume");
    CHECK(!ShouldSynthesizeResumed(SessionState::WaitingForInput, true, true, L"", L"tool_use", false, 500), "resume: Waiting is recon-run's domain, not resume");

    // --- Subagent/fork activity: presence "busy" + external-work Running promotion (the recon-subagent gate) ---
    CHECK(PresenceIsBusy(L"busy"), "presence: 'busy' == working");
    CHECK(!PresenceIsBusy(L"idle"), "presence: 'idle' is not working");
    CHECK(!PresenceIsBusy(L"waiting"), "presence: 'waiting' (for the user) is not working");
    CHECK(!PresenceIsBusy(L"shell"), "presence: 'shell' is not working");
    CHECK(!PresenceIsBusy(L""), "presence: no heartbeat is not working");
    // subagentActive arm — a subagent transcript is actively growing (the parent's tail is the pending Task tool_use, non-terminal):
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, true, false, L"", false), "ext-work: Idle + subagent writing (no parent stop_reason) -> Running");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"tool_use", false), "ext-work: subagent writing + in-flight (tool_use) tail -> Running");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"end_turn", false), "ext-work: turn ENDED (terminal tail) — a subagent's final write lands us before end_turn, so 'fresh subagent' here is the just-finished turn, NOT new work: do NOT bounce Waiting->Running");
    // presenceBusy arm — gated on a NON-terminal tail (no post-Stop flicker):
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"", false), "ext-work: Idle + presence busy + in-flight tail -> Running (e.g. a freshly /fork'd conversation)");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"tool_use", false), "ext-work: presence busy + mid-turn tail -> Running");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, false, true, L"end_turn", false), "ext-work: stale 'busy' right after a real Stop (terminal tail) does NOT bounce Waiting back to Running");
    // interrupt guard (the Running<->Waiting oscillation fix) — an INTERRUPTED turn is recon-stop's job
    // (-> Waiting); after an Esc the dying subagents keep flushing side files + the heartbeat lingers
    // "busy", so promoting here would flip-flop with ShouldSynthesizeStop every tick (the field freeze that
    // flooded hooks.log to ~291 MB). The latch self-clears on a real resume (parser, fresh-append), so this
    // only suppresses while the tail IS still the interrupt marker:
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"", true), "ext-work: INTERRUPTED + subagent still flushing -> do NOT promote Waiting->Running (recon-stop owns the interrupt; else oscillation)");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"", true), "ext-work: INTERRUPTED + lingering 'busy' heartbeat -> no promotion (the turn was aborted, not resumed)");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, true, L"tool_use", true), "ext-work: INTERRUPTED wins even over a non-terminal tool_use tail with both activity signals set");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"", false), "ext-work: NON-interrupted subagent work still promotes (the latch self-clears on a real resume)");
    // state gate — only Idle/Waiting are repairable:
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Running, true, true, L"tool_use", false), "ext-work: already Running -> no-op");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::NeedsApproval, true, true, L"", false), "ext-work: NeedsApproval ('needs you') never cleared by activity");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Error, true, true, L"", false), "ext-work: Error never cleared by inference");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Done, true, true, L"", false), "ext-work: Done never revived");
    // neither signal -> unchanged (a genuinely idle session):
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, false, L"tool_use", false), "ext-work: no subagent + not busy -> no synthesis (parent-only path owns it)");

    // --- presence-IDLE release: claude's OWN heartbeat says "idle" while we are stuck Running on a
    //     NON-terminal tail (a trailing user prompt that produced no assistant output + a dropped/absent
    //     Stop). The IDLE mirror of PresenceIsBusy; it covers the exact gap ShouldSynthesizeStop cannot
    //     (no terminal stop_reason, no interrupt). PresenceIsAtRest is deliberately "idle"-only.
    CHECK(PresenceIsAtRest(L"idle"), "presence-rest: 'idle' == at rest");
    CHECK(!PresenceIsAtRest(L"busy"), "presence-rest: 'busy' is working, not at rest");
    CHECK(!PresenceIsAtRest(L"waiting"), "presence-rest: 'waiting' (needs-you) left to recon-block, not released here");
    CHECK(!PresenceIsAtRest(L"shell"), "presence-rest: 'shell' is not a claude turn-rest signal");
    CHECK(!PresenceIsAtRest(L""), "presence-rest: no heartbeat is not 'at rest'");
    // the no-op / stuck-Running case: Running + 'idle' heartbeat + cleared (non-terminal) tail, quiet for
    // the LONG floor -> release to WaitingForInput. Signature: (state, presence, lastStop, interrupted, quietForMs).
    CHECK(ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: Running + idle + cleared tail + quiet for the LONG floor -> stop (the no-op / stuck-Running case)");
    // Agentmaster (idle<->running flap fix): a Running cleared tail quiet for only the SHORT base window is
    // NOT released — that shape is indistinguishable from a turn paused behind a "No response from API ·
    // Retrying" backoff / a slow first token, and releasing at 5s oscillated Running<->WaitingForInput
    // against recon-run every scan (the reported "card bg" flap + "jumps to idle while the API retries").
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", false, kScanPresenceIdleQuiescenceMs), "presence-idle (flap fix): Running + idle + cleared tail quiet only the SHORT 5s window -> HOLD (a transient API-retry / stream pause, not a finished turn)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs - 1), "presence-idle: Running just under the long floor -> hold");
    CHECK(ShouldSynthesizeStopFromPresenceIdle(SessionState::NeedsApproval, L"idle", L"tool_use", false, kScanPresenceIdleQuiescenceMs), "presence-idle: a NeedsApproval stranded by a dropped post-answer Stop is released at the SHORT base floor (no API-retry ambiguity)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::NeedsApproval, L"idle", L"tool_use", false, kScanPresenceIdleQuiescenceMs - 1), "presence-idle: NeedsApproval not quiet long enough -> hold");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"busy", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: heartbeat 'busy' -> never release");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: no heartbeat -> no signal, no release");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"waiting", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: 'waiting' is not released here (recon-block owns needs-you)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"end_turn", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: a TERMINAL tail is plain recon-stop's job (mutually exclusive)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", true, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: an interrupt is plain recon-stop's job (mutually exclusive)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Idle, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: an Idle session has no turn to end");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::WaitingForInput, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: already settled -> no-op");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Done, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: Done is never re-ended/revived");
    // Agentmaster (idle<->running flap fix): a RUNNING session whose tail is a PENDING tool_use is
    // mid-turn — claude is running a tool (a long Bash/build) or waiting/retrying the next API call
    // ("No response from API · Retrying in …"), during which it is not generating so its heartbeat
    // reads "idle" and the transcript sits quiet. Releasing it would flap Running<->Waiting against
    // recon-run every scan (the "card bg" report). It must NOT release regardless of how long quiet;
    // only the cleared-tail no-op turn (above), and only past the long floor, releases for Running.
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"tool_use", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: Running + idle + PENDING tool_use tail (mid-tool / API-retry backoff) -> NOT released (would flap against recon-run)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"tool_use", false, kScanPresenceIdleRunningQuiescenceMs * 100), "presence-idle: a Running pending tool_use stays held no matter how long quiet (a long Bash/build or multi-minute API retry is still the same turn)");

    // --- Waiting-for-you "unread" decay gate (ShouldDecayWaitingToIdle) ---
    // A WaitingForInput card demotes to Idle ONLY once the timeout has elapsed AND it has been READ
    // (readUnixMs >= lastActivity). It waits the FULL timeout regardless of reading, and past the
    // timeout it keeps waiting while still unread. A manual Mark Unread (or a busy heartbeat) never
    // time-decays; 0 minutes == never. Signature: (state, lastActivityMs, readUnixMs, manualUnread,
    // presenceBusy, minutes, nowMs).
    {
        using S = SessionState;
        constexpr uint32_t kMin = 60; // 1h timeout
        constexpr int64_t kTimeout = 60LL * 60000; // 3,600,000 ms
        constexpr int64_t last = 1'000'000; // last activity anchor
        constexpr int64_t pastNow = last + kTimeout; // exactly at the timeout edge
        constexpr int64_t withinNow = last + kTimeout - 1; // 1ms inside the window
        // within the timeout: ALWAYS waits, even when already read
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, false, kMin, withinNow), "decay-waiting: within the timeout -> waits the full window (even when read)");
        // past the timeout + read -> decays
        CHECK(ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, false, kMin, pastNow), "decay-waiting: past timeout + read (readUnixMs == lastActivity) -> Idle");
        CHECK(ShouldDecayWaitingToIdle(S::WaitingForInput, last, last + 5, false, false, kMin, pastNow + 1000), "decay-waiting: past timeout + read AFTER the activity -> Idle");
        // past the timeout but UNREAD -> keeps waiting until read
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last - 1, false, false, kMin, pastNow), "decay-waiting: past timeout but UNREAD (readUnixMs < lastActivity) -> keep waiting until read");
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, 0, false, false, kMin, pastNow + 999999), "decay-waiting: never read (readUnixMs 0) -> never decays however long");
        // manual Mark Unread is sticky — never time-decays even past timeout + read
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, true, false, kMin, pastNow), "decay-waiting: manualUnread sticky -> never time-decays");
        // a busy heartbeat (a long Task/Agent subagent) holds it out of Idle
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, true, kMin, pastNow), "decay-waiting: presence 'busy' (subagent) -> never raced to Idle");
        // 0 minutes == never; wrong state / no anchor are no-ops
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, false, 0, pastNow), "decay-waiting: 0 minutes == never decay");
        CHECK(!ShouldDecayWaitingToIdle(S::Running, last, last, false, false, kMin, pastNow), "decay-waiting: not WaitingForInput -> no-op");
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, 0, last, false, false, kMin, pastNow), "decay-waiting: no activity anchor (lastActivity 0) -> no-op");
    }

    // --- ParseTranscriptDelta now surfaces the interactive tool name + a ToolResult marker ---
    {
        const auto p = ParseTranscriptDelta(
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"hold on\"},{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(p.events.size() == 1 && p.events[0].kind == TranscriptEvent::Kind::Assistant, "parse: assistant tool_use line");
        CHECK(p.events[0].toolName == L"AskUserQuestion", "parse: interactive tool name surfaced");
        CHECK(p.events[0].text == L"hold on", "parse: text alongside the tool_use still collected");
    }
    {
        const auto p = ParseTranscriptDelta(
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{}}]}}\n");
        CHECK(p.events.size() == 1 && p.events[0].toolName.empty(), "parse: a NON-interactive tool_use sets no toolName");
    }
    {
        const auto p = ParseTranscriptDelta(
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"ok\"}]}}\n");
        CHECK(p.events.size() == 1 && p.events[0].kind == TranscriptEvent::Kind::ToolResult, "parse: tool_result -> ToolResult marker (not a typed prompt)");
    }

    // --- end-to-end: derive the tail (mirroring the scanner) from the REAL transcript tails and
    //     drive the real SessionRegistry through one reconcile pass on a quiescent transcript. ---
    auto deriveTail = [](std::wstring_view chunk) {
        std::wstring lastStop, pendingTool;
        bool interrupted = false;
        for (const auto& ev : ParseTranscriptDelta(chunk).events)
        {
            if (ev.kind == TranscriptEvent::Kind::Assistant) { lastStop = ev.stopReason; interrupted = false; pendingTool = ev.toolName; }
            else if (ev.kind == TranscriptEvent::Kind::ToolResult) { pendingTool.clear(); }
            else { lastStop.clear(); pendingTool.clear(); interrupted = IsUserInterruptMarker(ev.text); }
        }
        return std::make_tuple(lastStop, pendingTool, interrupted);
    };
    auto reconcileQuiescent = [&](SessionRegistry& reg, const std::wstring& id, std::wstring_view chunk) {
        const auto [lastStop, pendingTool, interrupted] = deriveTail(chunk);
        const auto st = reg.Get(id)->state;
        if (ShouldSynthesizeStop(st, lastStop, interrupted, 3000))
        {
            HookMessage stop = Msg(id, HookEvent::Stop); stop.ts = 9000; stop.quiescentStop = true;
            reg.OnHookEvent(stop);
        }
        else if (ShouldSynthesizeBlockedOnUser(st, pendingTool, interrupted, 3000))
        {
            HookMessage n = Msg(id, HookEvent::Notification); n.permissionRequest = true; n.ts = 9000;
            reg.OnHookEvent(n);
        }
    };

    { // #1 oldest (4f2ea3bc): unanswered AskUserQuestion -> the "needs you" column, NOT Running
        SessionRegistry reg; reg.Upsert(MakeSession(L"e1", SessionState::Running));
        reconcileQuiescent(reg, L"e1",
            L"{\"type\":\"user\",\"message\":{\"content\":\"q\"}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"e1")->state == SessionState::NeedsApproval, "e2e #1: unanswered AskUserQuestion -> NeedsApproval (was: stuck Running)");
    }
    { // #2 mid (3de852d6): question rejected then user interrupt -> turn over -> Waiting
        SessionRegistry reg; reg.Upsert(MakeSession(L"e2", SessionState::Running));
        reconcileQuiescent(reg, L"e2",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n"
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"rejected\"}]}}\n"
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"[Request interrupted by user for tool use]\"}]}}\n");
        CHECK(reg.Get(L"e2")->state == SessionState::WaitingForInput, "e2e #2: interrupted turn -> WaitingForInput (was: stuck Running)");
    }
    { // original: NeedsApproval whose post-approval Stop was dropped -> a terminal tail releases it
        SessionRegistry reg; reg.Upsert(MakeSession(L"e3", SessionState::Running));
        reg.OnHookEvent([] { HookMessage m = Msg(L"e3", HookEvent::Notification); m.permissionRequest = true; return m; }());
        CHECK(reg.Get(L"e3")->state == SessionState::NeedsApproval, "e2e orig: permission Notification -> NeedsApproval");
        reconcileQuiescent(reg, L"e3",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"All done.\"}]}}\n");
        CHECK(reg.Get(L"e3")->state == SessionState::WaitingForInput, "e2e orig: dropped post-approval Stop -> terminal tail releases NeedsApproval -> Waiting");
    }
    { // CONTROL: a genuinely-working session (pending NON-interactive Bash) must STAY Running
        SessionRegistry reg; reg.Upsert(MakeSession(L"e4", SessionState::Running));
        reconcileQuiescent(reg, L"e4",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"e4")->state == SessionState::Running, "e2e control: pending Bash (working) stays Running — no false positive");
    }

    // --- recon-resume e2e: NeedsApproval -> Running when the user answers + the agent works again
    //     (a FRESH live append, the recon-run-family path — mirrors _reconcileSession). ---
    auto reconcileResume = [&](SessionRegistry& reg, const std::wstring& id, std::wstring_view chunk) {
        const auto [lastStop, pendingTool, interrupted] = deriveTail(chunk);
        bool consumedTurnEvent = false; // an assistant / human line (a bare tool_result never counts — mirrors _readDelta)
        for (const auto& ev : ParseTranscriptDelta(chunk).events)
        {
            if (ev.kind != TranscriptEvent::Kind::ToolResult) { consumedTurnEvent = true; }
        }
        if (ShouldSynthesizeResumed(reg.Get(id)->state, consumedTurnEvent, /*primed*/ true, pendingTool, lastStop, interrupted, /*fresh*/ 500))
        {
            HookMessage r = Msg(id, HookEvent::PostToolUse); r.ts = 9000;
            reg.OnHookEvent(r);
        }
    };

    { // THE bug: AskUserQuestion answered, agent KEEPS WORKING -> Running (was: stuck NeedsApproval/orange until end-of-turn)
        SessionRegistry reg; reg.Upsert(MakeSession(L"r1", SessionState::Running));
        reconcileQuiescent(reg, L"r1", // unanswered question goes quiescent -> NeedsApproval
            L"{\"type\":\"user\",\"message\":{\"content\":\"q\"}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"r1")->state == SessionState::NeedsApproval, "e2e resume #1a: unanswered AskUserQuestion -> NeedsApproval");
        reconcileResume(reg, L"r1", // the answer (tool_result) + the agent resumes working (fresh assistant line, turn not over)
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"Option A\"}]}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"Great, doing it.\"}]}}\n");
        CHECK(reg.Get(L"r1")->state == SessionState::Running, "e2e resume #1b: answered + working again -> Running (THE FIX: was stuck NeedsApproval)");
    }
    { // "not limited to AskUserQuestion": a real permission approval, then the agent works again -> Running
        SessionRegistry reg; reg.Upsert(MakeSession(L"r2", SessionState::Running));
        reg.OnHookEvent([] { HookMessage m = Msg(L"r2", HookEvent::Notification); m.permissionRequest = true; return m; }());
        CHECK(reg.Get(L"r2")->state == SessionState::NeedsApproval, "e2e resume #2a: permission Notification -> NeedsApproval");
        reconcileResume(reg, L"r2", // approved: the tool ran (a fresh assistant line, mid-turn)
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"r2")->state == SessionState::Running, "e2e resume #2b: approved + working again -> Running (NOT limited to AskUserQuestion)");
    }
    { // CONTROL: a NEW question in the resume window keeps it blocked (must NOT flip to Running)
        SessionRegistry reg; reg.Upsert(MakeSession(L"r3", SessionState::Running));
        reconcileQuiescent(reg, L"r3",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        reconcileResume(reg, L"r3", // answered the first, but the agent immediately asks ANOTHER
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"A\"}]}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"r3")->state == SessionState::NeedsApproval, "e2e resume control: a NEW pending question stays NeedsApproval");
    }
    { // CONTROL: an answer that ENDS the turn is recon-stop's job (-> Waiting), never a Running blip
        SessionRegistry reg; reg.Upsert(MakeSession(L"r4", SessionState::Running));
        reg.OnHookEvent([] { HookMessage m = Msg(L"r4", HookEvent::Notification); m.permissionRequest = true; return m; }());
        reconcileResume(reg, L"r4", // a terminal tail: resume declines (mutually exclusive with recon-stop)
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"All done.\"}]}}\n");
        CHECK(reg.Get(L"r4")->state == SessionState::NeedsApproval, "e2e resume control: terminal tail does NOT resume (recon-stop territory)");
        reconcileQuiescent(reg, L"r4", // and the quiescent recon-stop then releases it the right way
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"All done.\"}]}}\n");
        CHECK(reg.Get(L"r4")->state == SessionState::WaitingForInput, "e2e resume control: end-of-turn -> recon-stop -> WaitingForInput");
    }
}

// ===== Fleet Observer O1 (ProcessInspect primitives; doc/agentmaster/OBSERVER.md §6, §8b) =====

// A canned snapshot modelling one WindowsTerminal hosting three tabs: pwsh->claude (tab A),
// cmd->cmd-shim->claude (tab B, 2 levels deep), pwsh->git (tab C, no claude). 201 (claude) also
// has a node child; a claude-rooted search must match the ROOT itself (descendant-or-self —
// the Manager-launched shape: claude IS the ConPTY root, no shell in between).
static std::vector<ProcEntry> CannedSnapshot()
{
    return {
        { 10, 1, L"explorer.exe" },
        { 100, 10, L"WindowsTerminal.exe" },
        { 200, 100, L"pwsh.exe" }, // tab A shell
        { 201, 200, L"claude.exe" }, // claude under tab A (direct)
        { 202, 201, L"node.exe" }, // a tool spawned by claude A
        { 300, 100, L"cmd.exe" }, // tab B shell
        { 301, 300, L"cmd.exe" }, // the .cmd shim
        { 302, 301, L"claude.exe" }, // claude under tab B (2 levels deep)
        { 400, 100, L"pwsh.exe" }, // tab C shell (no claude)
        { 401, 400, L"git.exe" },
    };
}

static void TestProcessInspectTree()
{
    std::wprintf(L"ProcessInspect tree helpers (over a canned snapshot):\n");
    const auto snap = CannedSnapshot();

    CHECK(ImageNameEq(L"Claude.exe", L"claude.exe"), "ImageNameEq case-insensitive");
    CHECK(!ImageNameEq(L"claude.exe", L"claude"), "ImageNameEq length-strict");
    CHECK(!ImageNameEq(L"claude.exe", L"codex.exe"), "ImageNameEq distinct names");

    CHECK(FindDescendantByImage(snap, 200, L"claude.exe") == 201, "claude direct child of pwsh");
    CHECK(FindDescendantByImage(snap, 300, L"claude.exe") == 302, "claude 2 levels under cmd shim");
    CHECK(FindDescendantByImage(snap, 400, L"claude.exe") == 0, "no claude under a git tab");
    CHECK(FindDescendantByImage(snap, 999, L"claude.exe") == 0, "unknown root -> 0");
    CHECK(FindDescendantByImage(snap, 100, L"claude.exe") == 201, "BFS finds the shallowest claude (tab A)");
    CHECK(FindDescendantByImage(snap, 201, L"claude.exe") == 201, "descendant-or-self: the root itself matches (a Manager-launched claude IS the ConPTY root)");
    CHECK(FindDescendantByImage(snap, 201, L"node.exe") == 202, "descendant of a claude found");
    CHECK(FindDescendantByImage(snap, 201, L"pwsh.exe") == 0, "never matches upward (the root's parent shell is out of scope)");

    const auto kids = ChildrenOf(snap, 100);
    CHECK(kids.size() == 3, "ChildrenOf(WT) count");
    CHECK(kids.size() == 3 && kids[0] == 200 && kids[1] == 300 && kids[2] == 400, "ChildrenOf preserves snapshot order");
    CHECK(ChildrenOf(snap, 200).size() == 1 && ChildrenOf(snap, 200)[0] == 201, "ChildrenOf(pwsh A) == {claude}");
    CHECK(ChildrenOf(snap, 401).empty(), "ChildrenOf of a leaf is empty");

    // --- O6 busy heuristics: IsShellImage / HasActiveChild (claude) / HasNonShellChild (shell) ---
    CHECK(IsShellImage(L"pwsh.exe") && IsShellImage(L"PowerShell.exe") && IsShellImage(L"cmd.exe"), "IsShellImage: shells");
    CHECK(!IsShellImage(L"claude.exe") && !IsShellImage(L"codex.exe") && !IsShellImage(L"git.exe") && !IsShellImage(L""), "IsShellImage: non-shells");

    CHECK(HasActiveChild(snap, 201), "claude A is busy: has a tool child (node)");
    CHECK(!HasActiveChild(snap, 302), "claude B is idle: no children");
    CHECK(!HasActiveChild(snap, 401), "a leaf process has no active child");
    CHECK(!HasActiveChild(snap, 999), "unknown pid -> no active child");

    CHECK(HasNonShellChild(snap, 400), "pwsh C is busy: running git (a non-shell command)");
    CHECK(!HasNonShellChild(snap, 300), "cmd B's only direct child is the cmd shim (a shell) -> not busy");
    CHECK(HasNonShellChild(snap, 200), "pwsh A has a non-shell child (claude)");

    // console infrastructure (conhost / OpenConsole) is NOT "a command in progress"
    const std::vector<ProcEntry> infra = {
        { 500, 1, L"pwsh.exe" },
        { 501, 500, L"conhost.exe" },
    };
    CHECK(!HasActiveChild(infra, 500), "HasActiveChild ignores a conhost child");
    CHECK(!HasNonShellChild(infra, 500), "HasNonShellChild ignores a conhost child");
    const std::vector<ProcEntry> infra2 = {
        { 600, 1, L"pwsh.exe" },
        { 601, 600, L"OpenConsole.exe" },
        { 602, 600, L"rg.exe" },
    };
    CHECK(HasActiveChild(infra2, 600), "HasActiveChild sees a real (rg) child past OpenConsole");
    CHECK(HasNonShellChild(infra2, 600), "HasNonShellChild sees rg (a non-shell command)");

    // --- CommandChildrenOf: the shell's real command children (the out-of-band cwd source) ---
    // A shell's NATIVE children inherit its live cwd at spawn, so they recover a pwsh tab's cwd
    // (pwsh freezes its own process cwd). Console infra (conhost / OpenConsole) is excluded — it's
    // OS plumbing, not a command, and runs in C:\WINDOWS, which would poison the reading.
    CHECK(CommandChildrenOf(snap, 200).size() == 1 && CommandChildrenOf(snap, 200)[0] == 201, "CommandChildrenOf(pwsh A) == {claude} (a native child)");
    CHECK(CommandChildrenOf(snap, 400).size() == 1 && CommandChildrenOf(snap, 400)[0] == 401, "CommandChildrenOf(pwsh C) == {git}");
    CHECK(CommandChildrenOf(snap, 401).empty(), "CommandChildrenOf of a leaf is empty");
    CHECK(CommandChildrenOf(snap, 999).empty(), "CommandChildrenOf of an unknown pid is empty");
    CHECK(CommandChildrenOf(infra, 500).empty(), "CommandChildrenOf excludes a conhost-only child (would read C:\\WINDOWS)");
    {
        const auto cc = CommandChildrenOf(infra2, 600);
        CHECK(cc.size() == 1 && cc[0] == 602, "CommandChildrenOf skips OpenConsole, keeps the rg command child");
    }

    // --- FindTerminalHostPid: walk a claude up to its hosting WindowsTerminal.exe (the host-label core) ---
    // The label that follows (real WT vs Agentmaster vs Agentmaster Dev) keys on this host process's
    // package family; here we just verify the ancestor walk finds the right terminal (or none).
    CHECK(FindTerminalHostPid(snap, 201) == 100, "claude A -> its hosting WindowsTerminal (via pwsh)");
    CHECK(FindTerminalHostPid(snap, 302) == 100, "claude B -> the hosting WindowsTerminal (past cmd + shim)");
    CHECK(FindTerminalHostPid(snap, 100) == 0, "WindowsTerminal itself has no WT ancestor (skips self)");
    CHECK(FindTerminalHostPid(snap, 999) == 0, "unknown pid -> no host");
    {
        // An orphan: a claude whose parent terminal already exited (not in the snapshot) -> 0, so the
        // label falls back to the AM_SESSION stamp ("Agentmaster") rather than a live host.
        const std::vector<ProcEntry> orphan = { { 700, 690 /*gone*/, L"claude.exe" } };
        CHECK(FindTerminalHostPid(orphan, 700) == 0, "orphaned claude (dead host) -> no terminal host pid");
    }
}

static void TestProcessInspectParse()
{
    std::wprintf(L"ProcessInspect cmdline/env parse + classify:\n");
    using Env = std::unordered_map<std::wstring, std::wstring>;

    // --- ExtractCmdlineArg: "--flag value", "--flag=value", quoting, absent/dangling ---
    CHECK(ExtractCmdlineArg(L"claude --model opus --effort high", L"--model") == std::optional<std::wstring>(L"opus"), "extract --model value");
    CHECK(ExtractCmdlineArg(L"claude --model=sonnet", L"--model") == std::optional<std::wstring>(L"sonnet"), "extract --model=value");
    CHECK(ExtractCmdlineArg(L"claude --settings \"C:/a b/s.json\" --x", L"--settings") == std::optional<std::wstring>(L"C:/a b/s.json"), "extract quoted value with a space");
    CHECK(!ExtractCmdlineArg(L"claude --resume", L"--resume").has_value(), "dangling flag -> nullopt");
    CHECK(!ExtractCmdlineArg(L"claude --model opus", L"--effort").has_value(), "absent flag -> nullopt");

    // --- ParseClaudeFacts: a full fresh launch line ---
    {
        ClaudeProcessFacts f;
        Env env{ { L"WT_SESSION", L"wt-1" }, { L"AM_SESSION", L"am-1" } };
        ParseClaudeFacts(L"claude --dangerously-skip-permissions --model opus --effort high --permission-mode plan --settings \"C:/x/s.json\" --session-id abc-123", env, f);
        CHECK(f.model == L"opus", "facts model from --model");
        CHECK(f.effort == L"high", "facts effort from --effort");
        CHECK(f.permissionMode == L"plan", "facts permission-mode");
        CHECK(f.sessionIdArg == L"abc-123", "facts --session-id");
        CHECK(f.resumeTarget.empty(), "fresh launch has no resume target");
        CHECK(f.wtSession == L"wt-1" && f.amSession == L"am-1", "facts WT_SESSION + AM_SESSION from env");
        CHECK(!f.background, "interactive launch is not background");
    }

    // --- resume line + env-derived model/effort fallback ---
    {
        ClaudeProcessFacts f;
        Env env{ { L"CLAUDE_CODE_MODEL", L"haiku" }, { L"CLAUDE_CODE_EFFORT_LEVEL", L"low" } };
        ParseClaudeFacts(L"claude --resume conv-xyz --settings \"C:/x/s.json\"", env, f);
        CHECK(f.resumeTarget == L"conv-xyz", "facts --resume target");
        CHECK(f.model == L"haiku", "facts model falls back to CLAUDE_CODE_MODEL");
        CHECK(f.effort == L"low", "facts effort falls back to CLAUDE_CODE_EFFORT_LEVEL");
    }
    {
        ClaudeProcessFacts f;
        Env env{ { L"ANTHROPIC_MODEL", L"claude-x" } };
        ParseClaudeFacts(L"claude", env, f);
        CHECK(f.model == L"claude-x", "facts model falls back to ANTHROPIC_MODEL");
    }

    // --- background detection (env kind / CLAUDE_BG_* / daemon-run cmdline) ---
    {
        ClaudeProcessFacts f;
        Env env{ { L"CLAUDE_CODE_SESSION_KIND", L"bg" }, { L"CLAUDE_CODE_SESSION_NAME", L"nightly" } };
        ParseClaudeFacts(L"claude", env, f);
        CHECK(f.background, "background via CLAUDE_CODE_SESSION_KIND=bg");
        CHECK(f.sessionName == L"nightly", "background session name");
    }
    {
        ClaudeProcessFacts f;
        Env env{ { L"CLAUDE_BG_HOST", L"x" } };
        ParseClaudeFacts(L"claude", env, f);
        CHECK(f.background, "background via any CLAUDE_BG_* env");
    }
    {
        ClaudeProcessFacts f;
        Env env{};
        ParseClaudeFacts(L"claude daemon run --bg-pty-host", env, f);
        CHECK(f.background, "background via daemon-run cmdline");
    }

    // --- EnvLookup is case-insensitive (Windows env names ignore case) ---
    {
        Env env{ { L"wt_session", L"low-key" } };
        CHECK(EnvLookup(env, L"WT_SESSION") == L"low-key", "EnvLookup case-insensitive key match");
        CHECK(EnvLookup(env, L"missing").empty(), "EnvLookup miss -> empty");
    }

    // --- ClassifyRunningApp truth table (OBSERVER.md §7) ---
    CHECK(ClassifyRunningApp(L"am-1", L"wt-1", L"am-1") == RunningApp::Agentmaster, "classify ours -> Agentmaster");
    CHECK(ClassifyRunningApp(L"am-1", L"", L"am-1") == RunningApp::Agentmaster, "classify ours even without WT_SESSION");
    CHECK(ClassifyRunningApp(L"", L"wt-1", L"am-1") == RunningApp::WindowsTerminal, "classify bare WT_SESSION -> WindowsTerminal");
    CHECK(ClassifyRunningApp(L"", L"", L"am-1") == RunningApp::Other, "classify neither -> Other");
    CHECK(ClassifyRunningApp(L"am-2", L"wt-1", L"am-1") == RunningApp::Other, "classify a FOREIGN AM_SESSION -> Other (never ours)");
    CHECK(ClassifyRunningApp(L"", L"wt-1", L"") == RunningApp::WindowsTerminal, "classify with our stamp unminted: empty am + WT -> WindowsTerminal");
    CHECK(ClassifyRunningApp(L"", L"", L"") == RunningApp::Other, "classify all-empty -> Other (no false Agentmaster)");

    // §19-Q1: AM_SESSION may be "<guid>:<windowId>" (a Launched session) — classify on the GUID prefix.
    CHECK(ClassifyRunningApp(L"am-1:win-9", L"wt-1", L"am-1") == RunningApp::Agentmaster, "classify ours with :<windowId> suffix -> Agentmaster (prefix match)");
    CHECK(ClassifyRunningApp(L"am-2:win-9", L"wt-1", L"am-1") == RunningApp::Other, "classify a FOREIGN am with :<windowId> -> Other");
    CHECK(WindowIdFromAmSession(L"am-1:win-9") == L"win-9", "extract windowId from <guid>:<windowId>");
    CHECK(WindowIdFromAmSession(L"am-1").empty(), "bare <guid> has no windowId");
    CHECK(WindowIdFromAmSession(L"").empty(), "empty AM_SESSION -> no windowId");

    // --- IsClaudeDesktopGuiApp: tell the Claude Code CLI (console) from the Claude desktop app (GUI) ---
    // The desktop Electron app (and its renderer/gpu/utility children) share the leaf name Claude.exe
    // and run with cwd C:\WINDOWS\system32; only the PE subsystem separates them. Excluding them keeps
    // the External census free of bogus "system32 sessions".
    {
        ClaudeProcessFacts cli;
        cli.subsystem = 3; // IMAGE_SUBSYSTEM_WINDOWS_CUI
        CHECK(!IsClaudeDesktopGuiApp(cli), "console-subsystem claude is the CLI -> not the desktop app");
        ClaudeProcessFacts desktop;
        desktop.subsystem = 2; // IMAGE_SUBSYSTEM_WINDOWS_GUI
        desktop.cwd = L"C:\\WINDOWS\\system32";
        CHECK(IsClaudeDesktopGuiApp(desktop), "GUI-subsystem Claude.exe is the desktop app -> excluded");
        ClaudeProcessFacts unknown; // 0 == undeterminable (denied/elevated/WOW64): never hide a real session
        CHECK(!IsClaudeDesktopGuiApp(unknown), "undeterminable subsystem -> treated as CLI (not hidden)");
    }
}

static FILETIME UnixMsToFileTime(int64_t ms)
{
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<uint64_t>(ms) * 10000ull + 116444736000000000ull;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

static void MakeJsonl(const std::wstring& path, const std::string& content, int64_t mtimeMs, int64_t ctimeMs)
{
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    if (!content.empty())
    {
        DWORD w = 0;
        ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
    }
    const FILETIME c = UnixMsToFileTime(ctimeMs);
    const FILETIME m = UnixMsToFileTime(mtimeMs);
    ::SetFileTime(h, &c, nullptr, &m);
    ::CloseHandle(h);
}

static void TestTranscriptResolve()
{
    std::wprintf(L"ProcessInspect transcript resolution (encode + newest/tie-break):\n");

    // --- EncodeCwdToProjectDir: every non-[A-Za-z0-9] -> '-', no case folding ---
    CHECK(EncodeCwdToProjectDir(L"C:\\Users\\ELI") == L"C--Users-ELI", "encode plain path");
    CHECK(EncodeCwdToProjectDir(L"C:\\Users\\ELI\\.claude") == L"C--Users-ELI--claude", "encode dotted segment (\\. -> --)");
    CHECK(EncodeCwdToProjectDir(L"C:\\Program Files (x86)\\X") == L"C--Program-Files--x86--X", "encode spaces + parens");
    CHECK(EncodeCwdToProjectDir(L"K:/source/NumSharp") == L"K--source-NumSharp", "encode forward slashes, keep case");

    // --- ExtractCwdFromTranscriptHead: pull the cwd off the user/assistant lines ---
    {
        const std::wstring head =
            LR"j({"type":"mode","mode":"x"})j" L"\n"
            LR"j({"type":"user","cwd":"C:\\Users\\ELI","message":{"content":"hi"}})j" L"\n";
        CHECK(ExtractCwdFromTranscriptHead(head) == L"C:\\Users\\ELI", "extract cwd (backslash-unescaped) from head");
        CHECK(ExtractCwdFromTranscriptHead(LR"j({"type":"mode"})j").empty(), "no cwd in head -> empty");
    }

    // --- PickNewestTranscript (PURE): newest mtime; tie-break by ctime ~ start ---
    {
        std::vector<TranscriptCandidate> c{
            { L"old", 1000, 1000 },
            { L"new", 9000, 9000 },
        };
        CHECK(PickNewestTranscript(c, 0) == L"new", "newest mtime wins (no start hint)");
        CHECK(PickNewestTranscript({}, 0).empty(), "no candidates -> empty");
    }
    {
        // Two written within the tie window of each other; the one created closest to the claude's
        // start time wins (the same-cwd, two-claudes disambiguation).
        std::vector<TranscriptCandidate> c{
            { L"a", 5000, 100 },
            { L"b", 5000, 4000 },
        };
        CHECK(PickNewestTranscript(c, 4200) == L"b", "tie-break: ctime closest to start");
        CHECK(PickNewestTranscript(c, 50) == L"a", "tie-break: other start picks the other");
    }
    {
        // IDENTITY is by creation time, not activity: a claude resolves to the transcript CREATED
        // near its start (its own), even when ANOTHER transcript in the cwd is more recently written.
        std::vector<TranscriptCandidate> c{
            { L"mine", 5200, 5000 }, // created at my start (5000), lightly written
            { L"other", 9000, 100 }, // created long before me (100), very active (mtime 9000)
        };
        CHECK(PickNewestTranscript(c, 5000) == L"mine", "resolve to the transcript created near MY start, not the busiest other");
    }
    {
        // Never-prompted: only transcripts predating this claude's start exist -> resolve to "" (no
        // collapse onto a stale / another claude's transcript). The same-cwd, 0-message case (§11d).
        std::vector<TranscriptCandidate> c{
            { L"old1", 1000, 100 },
            { L"old2", 2000, 500 },
        };
        CHECK(PickNewestTranscript(c, 8000).empty(), "no transcript created at/after start -> empty (never-prompted; no stale collapse)");
    }

    // --- ResolveSessionIdIn over a temp projects root (real glob) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_obs_test_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring projRoot = root + L"\\projects";
        const std::wstring cwd = L"C:\\AmObsTest\\proj";
        const std::wstring dir = projRoot + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
        MakeJsonl(dir + L"\\older-id.jsonl", "{}", 1000, 1000);
        MakeJsonl(dir + L"\\newer-id.jsonl", "{}", 9000, 9000);

        CHECK(ResolveSessionIdIn(projRoot, cwd, 0) == L"newer-id", "ResolveSessionIdIn picks the newest transcript in the encoded dir (no start hint)");
        CHECK(ResolveSessionIdIn(projRoot, L"C:\\Nope\\missing", 0).empty(), "ResolveSessionIdIn empty when the encoded dir has no transcripts");
        CHECK(ResolveSessionIdIn(L"", cwd, 0).empty(), "ResolveSessionIdIn empty for an empty projects root");
        // start-aware (the same-cwd disambiguation over a real glob): resolve to the transcript
        // created nearest the claude's start; "" when none is at/after it.
        CHECK(ResolveSessionIdIn(projRoot, cwd, 1000) == L"older-id", "start near older's ctime -> older-id");
        CHECK(ResolveSessionIdIn(projRoot, cwd, 9000) == L"newer-id", "start near newer's ctime -> newer-id");
        CHECK(ResolveSessionIdIn(projRoot, cwd, 50000).empty(), "start after all transcripts -> empty (never-prompted)");

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- SessionStore: the generalized DURABLE per-session key/value store (titles + future data) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring store = std::wstring{ tmp } + L"am_sstore_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ store }, ec); // a clean slate

        const std::wstring a = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring b = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

        // absent -> empty; set -> get (durability across "windows" is just re-reading the same dir).
        CHECK(GetSessionStoreFieldIn(store, a, L"title").empty(), "store: a missing field reads empty");
        CHECK(SetSessionStoreFieldIn(store, a, L"title", L"My Renamed Session"), "store: set title ok");
        CHECK(GetSessionStoreFieldIn(store, a, L"title") == L"My Renamed Session", "store: get returns the set title");

        // generalized: a second arbitrary key on the same session coexists with the first.
        CHECK(SetSessionStoreFieldIn(store, a, L"note", L"hello"), "store: set a second arbitrary field");
        const auto recA = LoadSessionStoreIn(store, a);
        CHECK(recA.size() == 2 && recA.at(L"title") == L"My Renamed Session" && recA.at(L"note") == L"hello", "store: record holds both keys");

        // dedup: setting the same value again still reports success, value unchanged.
        CHECK(SetSessionStoreFieldIn(store, a, L"title", L"My Renamed Session"), "store: redundant set is a no-op success");
        CHECK(GetSessionStoreFieldIn(store, a, L"title") == L"My Renamed Session", "store: value stable after a redundant set");

        // an empty value REMOVES a key; removing the last key deletes the file (record empty).
        CHECK(SetSessionStoreFieldIn(store, a, L"note", L""), "store: empty value removes the key");
        CHECK(GetSessionStoreFieldIn(store, a, L"note").empty(), "store: a removed key reads empty");
        CHECK(LoadSessionStoreIn(store, a).size() == 1, "store: only the title remains");

        // a second session is independent (per-session files; O(1) by id).
        CHECK(SetSessionStoreFieldIn(store, b, L"title", L"Other Session"), "store: set title on a second session");

        // bulk: every session that has the field, in ONE scan (sparse — only titled sessions).
        const auto all = LoadAllSessionStoreFieldIn(store, L"title");
        CHECK(all.size() == 2 && all.at(a) == L"My Renamed Session" && all.at(b) == L"Other Session", "store: LoadAll gathers both titles");
        CHECK(LoadAllSessionStoreFieldIn(store, L"note").empty(), "store: LoadAll for a field no session has is empty");

        // a malformed session id never escapes the store dir; an empty id is inert.
        CHECK(!SetSessionStoreFieldIn(store, L"..\\evil", L"title", L"x"), "store: a path-bearing id is rejected");
        CHECK(GetSessionStoreFieldIn(store, L"", L"title").empty(), "store: an empty id reads empty");

        // --- the FAVORITE key (FAVORITES.md): the durable star, the same store + mechanism. ---
        CHECK(std::wstring{ kSessionStoreFavoriteKey } == L"favorite", "store: the favorite key is \"favorite\"");
        // not favorited until set; a star coexists with a title on the same session.
        CHECK(GetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey).empty(), "store: a session is not favorite by default");
        CHECK(SetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey, L"1"), "store: favorite a");
        const auto recFav = LoadSessionStoreIn(store, a);
        CHECK(recFav.size() == 2 && recFav.at(L"title") == L"My Renamed Session" && recFav.at(kSessionStoreFavoriteKey) == L"1", "store: favorite coexists with the title");
        // LoadAll(favorite) gathers ONLY favorited sessions (b has a title but no star).
        const auto favs = LoadAllSessionStoreFieldIn(store, kSessionStoreFavoriteKey);
        CHECK(favs.size() == 1 && favs.count(a) == 1 && favs.count(b) == 0, "store: LoadAll favorites returns only the starred session");
        // un-favorite removes the key (back to title-only) but the second-session star is independent.
        CHECK(SetSessionStoreFieldIn(store, b, kSessionStoreFavoriteKey, L"1"), "store: favorite b too");
        CHECK(SetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey, L""), "store: un-favorite a (empty removes the key)");
        CHECK(GetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey).empty(), "store: a is no longer favorite");
        const auto favs2 = LoadAllSessionStoreFieldIn(store, kSessionStoreFavoriteKey);
        CHECK(favs2.size() == 1 && favs2.count(b) == 1 && favs2.count(a) == 0, "store: only b remains favorited");
        CHECK(LoadSessionStoreIn(store, a).size() == 1, "store: un-favorite left the title intact");

        std::filesystem::remove_all(std::filesystem::path{ store }, ec);
    }

    // --- AnalyzeSessionTranscript: a real user prompt that BEGINS WITH A NEWLINE is kept ----------
    // Regression (the empty-summary bug): a pasted prompt (e.g. a terminal-screen capture) frequently
    // starts with a leading "\n". The summary noise filter ported session-end.js's startsWith('\n')
    // skip rule, which a full-corpus scan (2765 transcripts) proved a 100% false positive — every
    // message it dropped was real content, so a session whose only human input was such a paste showed
    // an EMPTY summary panel. The leading whitespace is now trimmed first and that rule is gone, so the
    // message is KEPT (and its leading newline stripped from the stored text). A leading-newline-THEN-
    // noise message (e.g. "\nCaveat:") is still dropped — the surviving startsWith prefixes see past
    // the trimmed whitespace — and a whitespace-only message still drops out entirely.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_nlmsg_" + std::to_wstring(::GetCurrentProcessId());

        const std::wstring pNl = base + L"_nl.jsonl";
        MakeJsonl(pNl,
                  // (1) a REAL prompt that begins with "\n" -> KEPT (leading newline trimmed off)
                  R"j({"type":"user","userType":"external","message":{"content":"\nrecap: pick up the non-cast task"},"timestamp":"2026-01-24T20:00:00.000Z"})j" "\n"
                  // (2) leading "\n" THEN a Caveat: noise prefix -> still DROPPED (prefix seen post-trim)
                  R"j({"type":"user","userType":"external","message":{"content":"\nCaveat: generated while running a local command"},"timestamp":"2026-01-24T20:01:00.000Z"})j" "\n"
                  // (3) only whitespace/newlines -> DROPPED (trims to empty)
                  R"j({"type":"user","userType":"external","message":{"content":"\n   \n"},"timestamp":"2026-01-24T20:02:00.000Z"})j" "\n",
                  1000, 1000);
        const auto a = AnalyzeSessionTranscript(pNl, 0);
        CHECK(a.userMsgs.size() == 1, "AnalyzeSessionTranscript: a leading-newline real prompt is kept; the Caveat-after-newline + whitespace-only messages are dropped");
        CHECK(!a.userMsgs.empty() && a.userMsgs[0] == L"recap: pick up the non-cast task", "AnalyzeSessionTranscript: the kept message has its leading newline trimmed from the stored text");
        CHECK(a.lastUserTs == L"2026-01-24T20:00:00.000Z", "AnalyzeSessionTranscript: the kept leading-newline prompt advances last-user time (was empty when it was wrongly filtered)");

        std::error_code ecNl;
        std::filesystem::remove(std::filesystem::path{ pNl }, ecNl);
    }

    // --- AnalyzeSessionTranscript: answering an AskUserQuestion advances "last user msg" ----------
    // Regression: the user's answer to AskUserQuestion is a tool_result block (not a typed text
    // prompt), so it once never moved lastUserTs and "last user msg" stayed pinned to the older typed
    // prompt. The answer is now correlated back to the interactive tool_use id and DOES advance the
    // timestamp, while its synthetic "User has answered…" text stays OUT of the Messages list. A
    // NON-interactive tool_result (a Bash result) must NOT advance it.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_lastuser_" + std::to_wstring(::GetCurrentProcessId());

        // Case A: typed prompt (18:00) -> AskUserQuestion (18:06) -> the user's answer (18:10).
        const std::wstring pAsk = base + L"_ask.jsonl";
        MakeJsonl(pAsk,
                  R"j({"type":"user","userType":"external","message":{"content":"do the thing"},"timestamp":"2026-01-24T18:00:00.000Z"})j" "\n"
                  R"j({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_ASK1","name":"AskUserQuestion","input":{"questions":[]}}]},"timestamp":"2026-01-24T18:06:41.211Z"})j" "\n"
                  R"j({"type":"user","userType":"external","message":{"content":[{"type":"tool_result","content":"User has answered your questions.","tool_use_id":"toolu_ASK1"}]},"timestamp":"2026-01-24T18:10:27.815Z"})j" "\n",
                  1000, 1000);
        const auto a = AnalyzeSessionTranscript(pAsk, 0);
        CHECK(a.lastUserTs == L"2026-01-24T18:10:27.815Z", "AnalyzeSessionTranscript: answering AskUserQuestion advances last-user time to the ANSWER (not the older typed prompt)");
        CHECK(a.userMsgs.size() == 1 && a.userMsgs[0] == L"do the thing", "AnalyzeSessionTranscript: the synthetic answer text stays OUT of the Messages list (only the typed prompt)");

        // Case B (control): typed prompt (19:00) -> Bash tool_use -> a Bash tool_result (19:05). A
        // non-interactive tool result must NOT advance last-user time — it stays at the typed prompt.
        const std::wstring pBash = base + L"_bash.jsonl";
        MakeJsonl(pBash,
                  R"j({"type":"user","userType":"external","message":{"content":"run a build"},"timestamp":"2026-01-24T19:00:00.000Z"})j" "\n"
                  R"j({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_BASH1","name":"Bash","input":{"command":"echo hi"}}]},"timestamp":"2026-01-24T19:01:00.000Z"})j" "\n"
                  R"j({"type":"user","userType":"external","message":{"content":[{"type":"tool_result","content":"hi","tool_use_id":"toolu_BASH1"}]},"timestamp":"2026-01-24T19:05:00.000Z"})j" "\n",
                  1000, 1000);
        const auto b = AnalyzeSessionTranscript(pBash, 0);
        CHECK(b.lastUserTs == L"2026-01-24T19:00:00.000Z", "AnalyzeSessionTranscript: a non-interactive (Bash) tool_result does NOT advance last-user time");

        std::error_code ec2;
        std::filesystem::remove(std::filesystem::path{ pAsk }, ec2);
        std::filesystem::remove(std::filesystem::path{ pBash }, ec2);
    }

    // --- Revert-aware DISPLAY: a double-ESC rewind orphans a branch; summary/title must skip it -----
    // Claude stores a conversation as a TREE (each message line carries uuid + parentUuid; a root's
    // parentUuid is null). A double-ESC REWIND (or /rewind) repoints the trailing `leafUuid` marker to
    // an EARLIER node, ORPHANING the abandoned branch — whose lines STAY in the .jsonl, INTERLEAVED
    // with the live ones (in-flight tool results land AFTER the new branch starts, so the discarded set
    // is NOT a contiguous prefix). DISPLAY surfaces (summary panel + title/prompt list) must show only
    // the chain from the current leaf to root; SEARCH/index keeps every line (a reverted message stays
    // findable). Mirrors the real session 1adaa37c, where a typed "test" + a follow-up were rewound away.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_revert_" + std::to_wstring(::GetCurrentProcessId());

        // File order: a discarded "test" branch, then the LIVE branch, with a discarded follow-up
        // ("u_orphan") INTERLEAVED *after* the live root — so a naive file-order parser keeps it.
        const std::string revert =
            R"j({"type":"user","userType":"external","uuid":"u_test","parentUuid":null,"message":{"content":"test"},"timestamp":"2026-06-26T10:00:00.000Z"})j" "\n"
            R"j({"type":"assistant","uuid":"u_a1","parentUuid":"u_test","message":{"content":[{"type":"text","text":"hi"}]},"timestamp":"2026-06-26T10:00:01.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"u_live1","parentUuid":null,"message":{"content":"What if my window lags"},"timestamp":"2026-06-26T10:05:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"u_orphan","parentUuid":"u_a1","message":{"content":"ORPHANED follow-up"},"timestamp":"2026-06-26T10:05:30.000Z"})j" "\n"
            R"j({"type":"assistant","uuid":"u_a2","parentUuid":"u_live1","message":{"content":[{"type":"text","text":"here is how"}]},"timestamp":"2026-06-26T10:06:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"u_live2","parentUuid":"u_a2","message":{"content":"audit b and then a"},"timestamp":"2026-06-26T10:10:00.000Z"})j" "\n"
            R"j({"type":"last-prompt","lastPrompt":"audit b and then a","leafUuid":"u_live2"})j" "\n";

        // (1) the pure resolver: active == the leaf->root chain; both discarded roots AND the
        // interleaved orphan are excluded.
        const auto active = ActiveBranchUuids(std::wstring{ revert.begin(), revert.end() });
        CHECK(active.size() == 3 && active.count(L"u_live1") && active.count(L"u_a2") && active.count(L"u_live2"),
              "ActiveBranchUuids: exactly the 3 live leaf->root nodes are included");
        CHECK(active.count(L"u_test") == 0 && active.count(L"u_a1") == 0 && active.count(L"u_orphan") == 0,
              "ActiveBranchUuids: the rewound-away branch (incl. the INTERLEAVED orphan after the live root) is excluded");

        // (1b) the per-message property: ClassifyTranscriptLines stamps onActiveBranch ("IsActiveLeaf")
        // on every message; ClassifyTranscriptLine alone captures uuid + defaults onActiveBranch true.
        {
            const auto facts = ClassifyTranscriptLines(std::wstring{ revert.begin(), revert.end() }, 4096, 0);
            auto branchOf = [&facts](const wchar_t* id) -> int {
                for (const auto& f : facts)
                {
                    if (f.uuid == id)
                    {
                        return f.onActiveBranch ? 1 : 0;
                    }
                }
                return -1; // not found
            };
            CHECK(branchOf(L"u_live1") == 1 && branchOf(L"u_a2") == 1 && branchOf(L"u_live2") == 1,
                  "ClassifyTranscriptLines: live nodes carry onActiveBranch=true");
            CHECK(branchOf(L"u_test") == 0 && branchOf(L"u_a1") == 0 && branchOf(L"u_orphan") == 0,
                  "ClassifyTranscriptLines: rewound-away nodes (incl. the interleaved orphan) carry onActiveBranch=false");

            const auto one = ClassifyTranscriptLine(LR"j({"type":"user","uuid":"solo","parentUuid":null,"message":{"content":"hi"}})j", 100, 0);
            CHECK(one.uuid == L"solo" && one.onActiveBranch,
                  "ClassifyTranscriptLine: captures uuid; onActiveBranch defaults true (a single line has no tree context)");

            // markActiveBranch=false (a partial / HEAD read) => nothing is marked inactive (keep-all).
            const auto noMark = ClassifyTranscriptLines(std::wstring{ revert.begin(), revert.end() }, 4096, 0, false);
            bool anyInactive = false;
            for (const auto& f : noMark)
            {
                if (!f.onActiveBranch)
                {
                    anyInactive = true;
                }
            }
            CHECK(!anyInactive, "ClassifyTranscriptLines: markActiveBranch=false leaves every message active (partial/head read)");
        }

        // (2) the summary panel (AnalyzeSessionTranscript, full read): only the live typed prompts; the
        // discarded "test" + the interleaved "ORPHANED follow-up" are gone, and first-activity is the
        // LIVE root's time (the discarded earlier turn never sets it).
        const std::wstring pRevert = base + L"_summary.jsonl";
        MakeJsonl(pRevert, revert, 2000, 1000);
        const auto a = AnalyzeSessionTranscript(pRevert, 0);
        CHECK(a.userMsgs.size() == 2 && a.userMsgs[0] == L"What if my window lags" && a.userMsgs[1] == L"audit b and then a",
              "AnalyzeSessionTranscript: only the LIVE branch's typed prompts (discarded 'test' + interleaved orphan excluded)");
        CHECK(a.firstTs == L"2026-06-26T10:05:00.000Z",
              "AnalyzeSessionTranscript: first activity is the live root's time, not the rewound-away earlier turn");

        // (2b) the "Transcript" COPY (ReadConversationText, full read): the copied conversation is the
        // LIVE branch only — the discarded "test" turn + its reply + the interleaved orphan are excluded.
        const std::wstring convo = ReadConversationText(pRevert, false, 0);
        CHECK(convo.find(L"What if my window lags") != std::wstring::npos &&
                  convo.find(L"here is how") != std::wstring::npos &&
                  convo.find(L"audit b and then a") != std::wstring::npos,
              "ReadConversationText: the live User+Assistant turns are present in the copied transcript");
        CHECK(convo.find(L"test") == std::wstring::npos && convo.find(L"ORPHANED") == std::wstring::npos,
              "ReadConversationText: the rewound-away 'test' branch + the interleaved orphan are excluded");

        // (3) the title + prompt list (ReadTranscriptInfoIn, full read): title is the live first
        // prompt, not the discarded "test"; the prompt list excludes the orphan.
        const std::wstring projectsDir = base + L"_proj";
        const std::wstring cwd = L"K:\\some\\where";
        const std::wstring sub = projectsDir + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ecMk;
        std::filesystem::create_directories(std::filesystem::path{ sub }, ecMk);
        const std::wstring sid = L"11111111-2222-3333-4444-555555555555";
        MakeJsonl(sub + L"\\" + sid + L".jsonl", revert, 2000, 1000);
        const auto ti = ReadTranscriptInfoIn(projectsDir, cwd, sid, 0, 1000);
        CHECK(TranscriptDisplayTitle(ti) == L"What if my window lags",
              "ReadTranscriptInfoIn: the title is the LIVE first prompt, not the rewound-away 'test'");
        CHECK(ti.userPrompts.size() == 2 && ti.userPrompts[0] == L"What if my window lags" && ti.userPrompts[1] == L"audit b and then a",
              "ReadTranscriptInfoIn: the prompt list is the live branch only (no 'test', no interleaved orphan)");

        // (4) backward-compat: a transcript with NO leaf marker => empty set => EVERY line kept (the
        // legacy all-messages behavior); and a leaf naming an ABSENT node likewise degrades to keep-all
        // (never orphan the whole file off a bad pointer).
        const std::string noMarker =
            R"j({"type":"user","userType":"external","uuid":"x1","parentUuid":null,"message":{"content":"alpha"},"timestamp":"2026-06-26T11:00:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"x2","parentUuid":"x1","message":{"content":"beta"},"timestamp":"2026-06-26T11:01:00.000Z"})j" "\n";
        CHECK(ActiveBranchUuids(std::wstring{ noMarker.begin(), noMarker.end() }).empty(),
              "ActiveBranchUuids: no leafUuid marker => empty (caller keeps every line)");
        const std::string badLeaf = noMarker + R"j({"type":"last-prompt","leafUuid":"NONEXISTENT"})j" "\n";
        CHECK(ActiveBranchUuids(std::wstring{ badLeaf.begin(), badLeaf.end() }).empty(),
              "ActiveBranchUuids: a leaf naming an absent node => empty (never orphan the whole file)");
        const std::wstring pNoMarker = base + L"_nomarker.jsonl";
        MakeJsonl(pNoMarker, noMarker, 2000, 1000);
        const auto an = AnalyzeSessionTranscript(pNoMarker, 0);
        CHECK(an.userMsgs.size() == 2, "AnalyzeSessionTranscript: with no leaf marker, every message is kept (legacy all-messages behavior)");

        std::error_code ecR;
        std::filesystem::remove(std::filesystem::path{ pRevert }, ecR);
        std::filesystem::remove(std::filesystem::path{ pNoMarker }, ecR);
        std::filesystem::remove_all(std::filesystem::path{ projectsDir }, ecR);
    }

    // --- Conversation lineage: in-file /compact splits into previous + current segments -----------
    // A /compact writes a system/compact_boundary (parentUuid:null => a NEW root, so the active leaf
    // chain STOPS there) + an isCompactSummary "continued from a previous conversation" bridge. The
    // pre-compaction turns stay in the file but OFF the active chain. The current Messages list is the
    // post-compaction segment (leaf-filtered, the summary bridge excluded); the pre-compaction segment
    // surfaces as a numbered "previous session". Mirrors the real session 3751c455.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_compact_" + std::to_wstring(::GetCurrentProcessId());
        const std::string compacted =
            R"j({"type":"user","userType":"external","uuid":"o1","parentUuid":null,"message":{"content":"old prompt one"},"timestamp":"2026-06-26T09:00:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"o2","parentUuid":"o1","message":{"content":"old prompt two"},"timestamp":"2026-06-26T09:05:00.000Z"})j" "\n"
            R"j({"type":"system","subtype":"compact_boundary","uuid":"B","parentUuid":null,"logicalParentUuid":"o2","compactMetadata":{"trigger":"manual","preTokens":409797,"postTokens":5012},"timestamp":"2026-06-26T09:06:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","isCompactSummary":true,"uuid":"S","parentUuid":"B","message":{"content":"This session is being continued from a previous conversation..."},"timestamp":"2026-06-26T09:06:01.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"n1","parentUuid":"S","message":{"content":"new prompt one"},"timestamp":"2026-06-26T09:10:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"n2","parentUuid":"n1","message":{"content":"new prompt two"},"timestamp":"2026-06-26T09:15:00.000Z"})j" "\n"
            R"j({"type":"last-prompt","lastPrompt":"new prompt two","leafUuid":"n2"})j" "\n";

        // (1) the pure segment collector: 2 segments; seg0 = pre-compaction (labeled), seg1 = current.
        const auto segs = CollectConversationSegments(std::wstring{ compacted.begin(), compacted.end() });
        CHECK(segs.size() == 2, "CollectConversationSegments: a /compact boundary splits into 2 segments");
        CHECK(segs[0].userMsgs.size() == 2 && segs[0].userMsgs[0] == L"old prompt one" && segs[0].userMsgs[1] == L"old prompt two",
              "CollectConversationSegments: segment 0 = the pre-compaction prompts");
        CHECK(segs[0].label.find(L"compacted") != std::wstring::npos && segs[0].label.find(L"manual") != std::wstring::npos && segs[0].label.find(L"409k") != std::wstring::npos,
              "CollectConversationSegments: segment 0's label carries trigger + token counts");
        CHECK(segs[1].userMsgs.size() == 2 && segs[1].userMsgs[0] == L"new prompt one",
              "CollectConversationSegments: segment 1 = the current (post-compaction) prompts");

        // (2) AnalyzeSessionTranscript: current Messages = post-compaction only (leaf-filtered, summary
        // bridge excluded); the pre-compaction segment surfaces as exactly one previous session.
        const std::wstring pComp = base + L"_c.jsonl";
        MakeJsonl(pComp, compacted, 2000, 1000);
        const auto a = AnalyzeSessionTranscript(pComp, 0);
        CHECK(a.compacted, "AnalyzeSessionTranscript: a /compact session is flagged compacted");
        CHECK(a.userMsgs.size() == 2 && a.userMsgs[0] == L"new prompt one" && a.userMsgs[1] == L"new prompt two",
              "AnalyzeSessionTranscript: current Messages = post-compaction only (leaf-filtered; isCompactSummary bridge excluded)");
        bool leak = false;
        for (const auto& m : a.userMsgs)
        {
            if (m.find(L"continued from a previous") != std::wstring::npos)
            {
                leak = true;
            }
        }
        CHECK(!leak, "AnalyzeSessionTranscript: the isCompactSummary bridge does NOT leak into the Messages list");
        CHECK(a.previousSegments.size() == 1 && a.previousSegments[0].userMsgs.size() == 2 && a.previousSegments[0].userMsgs[0] == L"old prompt one",
              "AnalyzeSessionTranscript: the pre-compaction segment surfaces as one numbered previous session");

        // (3) a non-compacted session: one segment, not flagged, no previous.
        const std::string plain =
            R"j({"type":"user","userType":"external","uuid":"p1","parentUuid":null,"message":{"content":"hello there"},"timestamp":"2026-06-26T09:00:00.000Z"})j" "\n"
            R"j({"type":"last-prompt","leafUuid":"p1"})j" "\n";
        CHECK(CollectConversationSegments(std::wstring{ plain.begin(), plain.end() }).size() == 1, "CollectConversationSegments: a non-compacted session is one segment");
        const std::wstring pPlain = base + L"_p.jsonl";
        MakeJsonl(pPlain, plain, 2000, 1000);
        const auto ap = AnalyzeSessionTranscript(pPlain, 0);
        CHECK(!ap.compacted && ap.previousSegments.empty(), "AnalyzeSessionTranscript: a non-compacted session has no previous segments");

        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{ pComp }, ec);
        std::filesystem::remove(std::filesystem::path{ pPlain }, ec);
    }

    // --- NormalizeRecapText: the one-true recap normalizer (shared by every recap reader) ----------
    CHECK(NormalizeRecapText(L"Did X. Next: Y. (disable recaps in /config)") == L"Did X. Next: Y.", "NormalizeRecapText: trailing disable hint + the space before it are stripped");
    CHECK(NormalizeRecapText(L"  spaced recap \n") == L"spaced recap", "NormalizeRecapText: surrounding whitespace/newlines trimmed");
    CHECK(NormalizeRecapText(L"no hint here") == L"no hint here", "NormalizeRecapText: a recap without the hint is unchanged");
    CHECK(NormalizeRecapText(L"(disable recaps in /config)") == L"", "NormalizeRecapText: a hint-only body normalizes to empty");
    CHECK(NormalizeRecapText(L"") == L"", "NormalizeRecapText: empty stays empty");

    // --- RecapFromTranscriptChunk: the PURE idle-recap extractor the observer's TAIL reader uses ------
    // The Fleet Observer reads an EXTERNAL session's recap from the transcript TAIL (the SAME region the
    // SessionScanner pulls a managed session's recap from — an external has no scanner cursor); this pure
    // helper does the per-chunk extraction (ReadTranscriptRecapTail = ReadFileTail + this). Tested with no
    // file IO. Mirrors ParseTranscriptDelta.recap / AnalyzeSessionTranscript semantics (shared
    // NormalizeRecapText, "last wins", "empty never clears"), PLUS the tail-specific tolerance of a
    // PARTIAL leading line (a tail read can begin mid-line).
    {
        const std::wstring one = LR"j({"type":"system","subtype":"away_summary","content":"Shipped the fix. Next: deploy. (disable recaps in /config)"})j" L"\n";
        CHECK(RecapFromTranscriptChunk(one) == L"Shipped the fix. Next: deploy.", "RecapFromTranscriptChunk: single away_summary captured + disable hint stripped");

        const std::wstring multi =
            std::wstring{ LR"j({"type":"system","subtype":"away_summary","content":"older recap"})j" } + L"\n" +
            LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]}})j" + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"newer recap"})j" + L"\n";
        CHECK(RecapFromTranscriptChunk(multi) == L"newer recap", "RecapFromTranscriptChunk: the LAST away_summary in the chunk wins (newer supersedes)");

        CHECK(RecapFromTranscriptChunk(LR"j({"type":"assistant","message":{"stop_reason":"end_turn"}})j" L"\n").empty(),
              "RecapFromTranscriptChunk: a recap-less chunk yields \"\" (so an empty tail never clears a stored recap)");

        // A TAIL read can begin MID-LINE: the leading partial JSON fails json::Parse and is skipped, but
        // a COMPLETE away_summary after the first newline is still captured.
        const std::wstring partialLead =
            std::wstring{ LR"j(xt","text":"...a truncated assistant line from before the tail window..."}]}})j" } + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"recap after a partial leading line"})j" + L"\n";
        CHECK(RecapFromTranscriptChunk(partialLead) == L"recap after a partial leading line",
              "RecapFromTranscriptChunk: a partial leading line (tail starting mid-line) is skipped; a complete recap after it is captured");

        CHECK(RecapFromTranscriptChunk(L"").empty(), "RecapFromTranscriptChunk: empty chunk -> empty");
    }

    // --- LastActivityMsFromTranscriptChunk: line-derived last-activity (ignores untimestamped state) --
    // The Fleet Observer feeds SessionInfo.convLastActivityUnixMs from THIS, not the file mtime: a
    // `claude --resume` + a /model / permission-mode / shell-cwd change APPEND UNTIMESTAMPED trailer
    // lines (last-prompt/mode/permission-mode) that bump the file mtime WITHOUT being conversation
    // activity — so a restored tab focused after a restart would otherwise read "active just now" (it
    // only resumed; measured live: 7–32 h gaps between the last real line and the mtime). This is the
    // PURE extractor (no file IO): the NEWEST timestamp among non-meta/compact/sidechain user/assistant
    // lines. Mirrors TranscriptStore::QuickRowFacts (the Sessions browser's line-derived last-activity).
    {
        const std::wstring userEarly = LR"j({"type":"user","userType":"external","message":{"content":"hi"},"timestamp":"2026-02-01T10:00:00.000Z"})j" L"\n";
        const std::wstring asstLate = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]},"timestamp":"2026-02-01T10:05:00.000Z"})j" L"\n";
        // The untimestamped trailer/state lines `claude --resume` and mode/permission changes append:
        const std::wstring trailers =
            LR"j({"type":"last-prompt","sessionId":"x"})j" L"\n"
            LR"j({"type":"mode","mode":"default","sessionId":"x"})j" L"\n"
            LR"j({"type":"permission-mode","permissionMode":"bypassPermissions","sessionId":"x"})j" L"\n";

        const int64_t both = LastActivityMsFromTranscriptChunk(userEarly + asstLate);
        const int64_t early = LastActivityMsFromTranscriptChunk(userEarly);
        const int64_t late = LastActivityMsFromTranscriptChunk(asstLate);
        CHECK(both > 0 && early > 0 && late > 0, "LastActivityMsFromTranscriptChunk: timestamped user/assistant lines yield a positive ms");
        CHECK(both == late, "LastActivityMsFromTranscriptChunk: the NEWEST conversation timestamp wins (assistant @10:05 > user @10:00)");
        CHECK(late > early, "LastActivityMsFromTranscriptChunk: a later ISO timestamp parses to a greater ms (sanity on the parse)");

        // THE BUG: the untimestamped resume/mode/permission trailer lines must NOT change the answer (the
        // file mtime would jump to resume-time; the line-derived value must stay at the last REAL line).
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + asstLate + trailers) == both,
              "LastActivityMsFromTranscriptChunk: untimestamped resume/mode/permission trailer lines are IGNORED (the fix)");
        CHECK(LastActivityMsFromTranscriptChunk(trailers) == 0,
              "LastActivityMsFromTranscriptChunk: a chunk of only untimestamped state lines yields 0 (-> caller falls back to mtime)");

        // A fork copies its parent's tail VERBATIM (old stamps); a newer real line still wins (max, not last-seen).
        CHECK(LastActivityMsFromTranscriptChunk(asstLate + userEarly) == late,
              "LastActivityMsFromTranscriptChunk: newest wins even when an OLDER stamp appears last (fork-copied tail)");

        // The away_summary RECAP (type "system") is written WHILE idle (~5 min after the last real line):
        // it is NOT conversation activity, so a timestamped system line is excluded.
        const std::wstring recapSys = LR"j({"type":"system","subtype":"away_summary","content":"recap","timestamp":"2026-02-01T10:30:00.000Z"})j" L"\n";
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + recapSys) == early,
              "LastActivityMsFromTranscriptChunk: a timestamped away_summary system line does NOT count as activity");

        // Meta / sidechain (subagent) lines are excluded too (mirrors ReadTranscriptInfo / ParseTranscriptDelta).
        const std::wstring metaLine = LR"j({"type":"user","isMeta":true,"message":{"content":"<command>"},"timestamp":"2026-02-01T11:00:00.000Z"})j" L"\n";
        const std::wstring sideLine = LR"j({"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"sub"}]},"timestamp":"2026-02-01T11:00:00.000Z"})j" L"\n";
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + metaLine) == early, "LastActivityMsFromTranscriptChunk: isMeta lines are skipped");
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + sideLine) == early, "LastActivityMsFromTranscriptChunk: isSidechain (subagent) lines are skipped");

        // A TAIL read can begin MID-LINE: the partial leading JSON fails to parse and is skipped; a
        // complete conversation line after it is still captured.
        const std::wstring partialLead = std::wstring{ LR"j(...","text":"a truncated line"}]},"timestamp":"2020-01-01T00:00:00.000Z"})j" } + L"\n" + asstLate;
        CHECK(LastActivityMsFromTranscriptChunk(partialLead) == late,
              "LastActivityMsFromTranscriptChunk: a partial leading line (tail starting mid-line) is skipped; a complete line after it is captured");

        CHECK(LastActivityMsFromTranscriptChunk(L"") == 0, "LastActivityMsFromTranscriptChunk: empty chunk -> 0");
    }

    // --- AnalyzeSessionTranscript: the idle RECAP (away_summary) is captured into .awaySummary -------
    // The summary panel / Sessions detail / copyable Summary read SessionSummary.awaySummary. The LAST
    // away_summary wins (a session that went idle, came back, and went idle again has a fresher recap),
    // and the "(disable recaps in /config)" UI hint is stripped. A recap is NOT a user message — it must
    // never leak into the numbered Messages list.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring pRecap = std::wstring{ tmp } + L"am_recap_" + std::to_wstring(::GetCurrentProcessId()) + L".jsonl";
        MakeJsonl(pRecap,
                  R"j({"type":"user","userType":"external","message":{"content":"start the work"},"timestamp":"2026-02-01T10:00:00.000Z"})j" "\n"
                  R"j({"type":"system","subtype":"away_summary","content":"Old recap. (disable recaps in /config)","timestamp":"2026-02-01T10:30:00.000Z"})j" "\n"
                  R"j({"type":"user","userType":"external","message":{"content":"keep going"},"timestamp":"2026-02-01T11:00:00.000Z"})j" "\n"
                  R"j({"type":"system","subtype":"away_summary","content":"We did the work; next is to ship it. (disable recaps in /config)","timestamp":"2026-02-01T11:30:00.000Z"})j" "\n",
                  1000, 1000);
        const auto a = AnalyzeSessionTranscript(pRecap, 0);
        CHECK(a.awaySummary == L"We did the work; next is to ship it.", "AnalyzeSessionTranscript: the LATEST away_summary wins + the disable hint is stripped");
        CHECK(a.userMsgs.size() == 2, "AnalyzeSessionTranscript: the two recap lines stay OUT of the Messages list (only the 2 typed prompts)");

        // The shared text box renders a "Recap:" section above the Messages, with the label INLINE with
        // the prose (one line, no wasted break) — "Recap: <text>", not "Recap:\n<text>".
        const auto box = RenderSessionSummaryBox(a, L"sid-recap", L"K:\\x", pRecap, L"claude --resume sid-recap", L"", L"", L"", /*full*/ true);
        CHECK(box.find(L"Recap: We did the work; next is to ship it.") != std::wstring::npos, "RenderSessionSummaryBox: the recap label is INLINE with the prose (one line, no break)");
        CHECK(box.find(L"Recap:") < box.find(L"start the work"), "RenderSessionSummaryBox: the Recap section sits ABOVE the numbered user messages");

        std::error_code ecR;
        std::filesystem::remove(std::filesystem::path{ pRecap }, ecR);
    }

    // --- the recap is shown in FULL — never length-capped (unlike the numbered messages, which cap at
    // 240 chars in this one-line box). A recap far longer than that cap renders whole, no "...".
    {
        SessionSummary big;
        big.found = true;
        big.userMsgs.push_back(std::wstring(500, L'M')); // a long MESSAGE: still capped at 240 (control)
        big.awaySummary = std::wstring(800, L'R') + L" RECAP_TAIL_MARKER"; // a long RECAP: shown whole
        const auto box = RenderSessionSummaryBox(big, L"id", L"K:\\x", L"t.jsonl", L"claude --resume id", L"", L"", L"", /*full*/ false);
        // Isolate the recap line: from "Recap: " up to the section separator before the Messages.
        const size_t rp = box.find(L"Recap: ");
        const size_t sepAfter = rp == std::wstring::npos ? std::wstring::npos : box.find(kSummarySepMark, rp);
        const std::wstring recapLine = rp == std::wstring::npos ? L"" : box.substr(rp, sepAfter == std::wstring::npos ? std::wstring::npos : sepAfter - rp);
        CHECK(recapLine.find(L"RECAP_TAIL_MARKER") != std::wstring::npos, "RenderSessionSummaryBox: a long recap (>240) is rendered in FULL (its tail survives)");
        CHECK(!recapLine.empty() && recapLine.find(L"...") == std::wstring::npos, "RenderSessionSummaryBox: the recap line carries NO '...' truncation");
        CHECK(box.find(L"MMM...") != std::wstring::npos || box.find(L"...") != std::wstring::npos, "RenderSessionSummaryBox: a long numbered MESSAGE is still capped (control — the cap applies to messages, not the recap)");
    }
}

// "YYYY\\MM\\DD" in LOCAL time for a unix-ms instant — mirrors ProcessInspect's CodexDayDirLocal
// (same Win32 conversion, so the date dir the test CREATES matches the one the resolver GLOBS).
static std::wstring TestCodexDayDir(int64_t unixMs)
{
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<uint64_t>(unixMs) * 10000ull + 116444736000000000ull;
    FILETIME ut;
    ut.dwLowDateTime = u.LowPart;
    ut.dwHighDateTime = u.HighPart;
    FILETIME lt{};
    ::FileTimeToLocalFileTime(&ut, &lt);
    SYSTEMTIME st{};
    ::FileTimeToSystemTime(&lt, &st);
    wchar_t buf[16]{};
    ::swprintf(buf, 16, L"%04u\\%02u\\%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// Agentmaster Phase C1: Codex (OpenAI Codex CLI) observe-only enrichment. Covers the PURE
// primitives (ParseCodexFacts / CodexRolloutUuid / ParseCodexRolloutText) + the file-read
// (ReadCodexRolloutInfo) + the date-sharded resolution (ResolveCodexSessionIn) over a temp home.
static void TestCodexObserve()
{
    std::wprintf(L"Codex (Phase C1) facts/uuid/rollout parse + resolution:\n");
    using Env = std::unordered_map<std::wstring, std::wstring>;

    // --- ParseCodexFacts: flags (--model/-m, --sandbox/-s, --ask-for-approval/-a), env, resume ---
    {
        CodexProcessFacts f;
        Env env{ { L"WT_SESSION", L"wt-9" }, { L"AM_SESSION", L"am-9" }, { L"CODEX_HOME", L"D:/cx" } };
        ParseCodexFacts(L"codex --model gpt-5.5 --sandbox danger-full-access --ask-for-approval never", env, f);
        CHECK(f.model == L"gpt-5.5", "codex facts model from --model");
        CHECK(f.sandbox == L"danger-full-access", "codex facts sandbox from --sandbox");
        CHECK(f.approvalMode == L"never", "codex facts approval from --ask-for-approval");
        CHECK(f.wtSession == L"wt-9" && f.amSession == L"am-9", "codex facts WT_SESSION + AM_SESSION");
        CHECK(f.codexHome == L"D:/cx", "codex facts CODEX_HOME from env");
        CHECK(f.resumeTarget.empty(), "fresh codex launch has no resume target");
    }
    {
        CodexProcessFacts f;
        ParseCodexFacts(L"codex -m o3 -s read-only -a on-request", {}, f);
        CHECK(f.model == L"o3", "codex facts model from -m");
        CHECK(f.sandbox == L"read-only", "codex facts sandbox from -s");
        CHECK(f.approvalMode == L"on-request", "codex facts approval from -a");
    }
    {
        CodexProcessFacts f;
        ParseCodexFacts(L"codex", {}, f); // bare: config.toml carries everything -> nothing on the cmdline
        CHECK(f.model.empty() && f.sandbox.empty() && f.approvalMode.empty() && f.resumeTarget.empty(), "bare codex cmdline -> empty parsed facts");
    }
    {
        CodexProcessFacts f;
        ParseCodexFacts(L"codex resume 019d8aa3-10a5-7273-ae65-a62cba4b63de", {}, f);
        CHECK(f.resumeTarget == L"019d8aa3-10a5-7273-ae65-a62cba4b63de", "codex `resume <guid>` -> authoritative resumeTarget");
        CodexProcessFacts g;
        ParseCodexFacts(L"codex resume --last", {}, g);
        CHECK(g.resumeTarget.empty(), "codex `resume --last` -> no explicit id (cwd discovery)");
    }

    // --- CodexRolloutUuid: the trailing 36-char UUIDv7 of a rollout stem ---
    CHECK(CodexRolloutUuid(L"rollout-2026-04-14T09-17-15-019d8aa3-10a5-7273-ae65-a62cba4b63de") == L"019d8aa3-10a5-7273-ae65-a62cba4b63de", "uuid from a real rollout stem");
    CHECK(CodexRolloutUuid(L"rollout-2026-04-14T09-17-15-not-a-guid-here-xxxx-xxxxxxxxxxxx").empty(), "non-guid tail -> empty");
    CHECK(CodexRolloutUuid(L"short").empty(), "too-short stem -> empty");

    // --- ParseCodexRolloutText: model/effort/sandbox/approval (turn_context), title + prompts
    //     (event_msg/user_message), and the AGENTS.md response_item user message is IGNORED ---
    {
        const std::string roll =
            R"({"type":"session_meta","payload":{"id":"019d8aa3-10a5-7273-ae65-a62cba4b63de","cwd":"K:/AmCodexTest/proj"}})" "\n"
            R"({"type":"turn_context","payload":{"model":"gpt-5.5","approval_policy":"never","sandbox_policy":{"type":"danger-full-access"},"collaboration_mode":{"settings":{"reasoning_effort":"xhigh"}}}})" "\n"
            R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"# AGENTS.md instructions for K:/AmCodexTest/proj"}]}})" "\n"
            R"({"type":"event_msg","payload":{"type":"user_message","message":"First real question\nsecond line"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"agent_message","message":"an answer"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"user_message","message":"Second question"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"task_complete","turn_id":"t","last_agent_message":"done"}})" "\n";
        const std::wstring wide(roll.begin(), roll.end()); // ASCII content -> safe narrow->wide
        CodexRolloutInfo info;
        ParseCodexRolloutText(wide, false, 100, info);
        CHECK(info.cwd == L"K:/AmCodexTest/proj", "rollout cwd from session_meta");
        CHECK(info.model == L"gpt-5.5", "rollout model from turn_context");
        CHECK(info.effort == L"xhigh", "rollout effort from collaboration_mode.settings.reasoning_effort");
        CHECK(info.sandbox == L"danger-full-access", "rollout sandbox from sandbox_policy.type");
        CHECK(info.approvalMode == L"never", "rollout approval from approval_policy");
        CHECK(info.title == L"First real question", "title = first user_message, first line");
        CHECK(info.userPrompts.size() == 2, "two human prompts (AGENTS.md response_item ignored)");
        CHECK(info.userPrompts.size() == 2 && info.userPrompts[0] == L"First real question\nsecond line", "first prompt full text");
        CHECK(info.userPrompts.size() == 2 && info.userPrompts[1] == L"Second question", "second prompt");
    }

    // --- ReadCodexRolloutInfo over a temp FILE (path-direct; validates read + timing wiring) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_codex_file_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ root }, ec);
        const std::wstring path = root + L"\\rollout-2025-06-15T10-00-00-019d8aa3-10a5-7273-ae65-a62cba4b63de.jsonl";
        const std::string roll =
            R"({"type":"session_meta","payload":{"cwd":"K:/AmCodexTest/proj"}})" "\n"
            R"({"type":"turn_context","payload":{"model":"gpt-5.5"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"user_message","message":"hello codex"}})" "\n";
        MakeJsonl(path, roll, 7000, 3000);
        const auto info = ReadCodexRolloutInfo(path, 0, 100);
        CHECK(info.found, "ReadCodexRolloutInfo found the file");
        CHECK(info.createdUnixMs == 3000 && info.lastActivityUnixMs == 7000, "rollout ctime/mtime");
        CHECK(info.title == L"hello codex" && info.model == L"gpt-5.5", "rollout title + model from file");
        CHECK(!ReadCodexRolloutInfo(root + L"\\nope.jsonl", 0, 100).found, "missing rollout -> not found");
        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- ResolveCodexSessionIn over a temp CODEX_HOME (date-sharded glob + cwd-confirm + pick) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring home = std::wstring{ tmp } + L"am_codex_home_" + std::to_wstring(::GetCurrentProcessId());
        const int64_t startMs = 1750000000000LL; // a fixed instant; the day dir is derived locally from it
        const std::wstring sdir = home + L"\\sessions\\" + TestCodexDayDir(startMs);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ sdir }, ec);
        const std::string metaMine = R"({"type":"session_meta","payload":{"cwd":"K:/AmCodexTest/proj"}})" "\n";
        const std::string metaOther = R"({"type":"session_meta","payload":{"cwd":"K:/Other/dir"}})" "\n";
        const std::wstring uuidMine = L"019d8aa3-10a5-7273-ae65-a62cba4b63de";
        const std::wstring uuidOld = L"019d0000-0000-7000-8000-000000000000";
        const std::wstring uuidOther = L"019dffff-ffff-7fff-bfff-ffffffffffff";
        MakeJsonl(sdir + L"\\rollout-x-" + uuidMine + L".jsonl", metaMine, startMs, startMs); // ctime≈start, my cwd
        MakeJsonl(sdir + L"\\rollout-x-" + uuidOld + L".jsonl", metaMine, startMs - 3600000, startMs - 3600000); // older, my cwd
        MakeJsonl(sdir + L"\\rollout-x-" + uuidOther + L".jsonl", metaOther, startMs + 5000, startMs + 5000); // diff cwd

        const auto sess = ResolveCodexSessionIn(home, L"K:/AmCodexTest/proj", startMs);
        CHECK(sess.sessionId == uuidMine, "resolve: rollout created at my start, in my cwd (ctime≈start identity)");
        CHECK(sess.rolloutPath.find(uuidMine) != std::wstring::npos, "resolve returns the rollout path");
        CHECK(sess.createdUnixMs == startMs, "resolve carries the rollout ctime");

        // resume fallback: start far past every rollout's ctime -> newest-mtime IN my cwd (mine).
        const auto sessR = ResolveCodexSessionIn(home, L"K:/AmCodexTest/proj", startMs + 100000000LL);
        CHECK(sessR.sessionId == uuidMine, "resume fallback: newest-mtime rollout in cwd when none matches start");

        CHECK(ResolveCodexSessionIn(home, L"K:/Nope/x", startMs).sessionId.empty(), "no rollout in cwd -> empty session");
        CHECK(ResolveCodexSessionIn(L"", L"K:/AmCodexTest/proj", startMs).sessionId.empty(), "empty home -> empty session");

        // ResolveCodexRolloutPathIn: find a rollout by its known uuid (the explicit-resume path).
        const auto byId = ResolveCodexRolloutPathIn(home, uuidOther);
        CHECK(byId.find(uuidOther) != std::wstring::npos, "ResolveCodexRolloutPathIn finds a rollout by uuid");
        CHECK(ResolveCodexRolloutPathIn(home, L"019dded0-0000-7000-8000-000000000000").empty(), "ResolveCodexRolloutPathIn: unknown uuid -> empty");

        std::filesystem::remove_all(std::filesystem::path{ home }, ec);
    }

    // ===== Phase C2: rollout-tail turn state (Running / Waiting / Idle) =====================
    std::wprintf(L"Codex (Phase C2) turn-state from rollout tail:\n");

    // --- ClassifyCodexLine: the pure per-line turn-boundary verdict ---
    {
        auto cl = [](const std::string& s) { return ClassifyCodexLine(std::wstring(s.begin(), s.end())); };
        CHECK(cl(R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t1"}})").isBoundary &&
                  cl(R"({"type":"event_msg","payload":{"type":"task_started"}})").state == CodexState::Running,
              "task_started -> Running boundary");
        {
            const auto b = cl(R"({"type":"event_msg","payload":{"type":"task_complete","last_agent_message":"all done"}})");
            CHECK(b.isBoundary && b.state == CodexState::Waiting, "task_complete -> Waiting boundary");
            CHECK(b.lastAgentMessage == L"all done", "task_complete carries last_agent_message");
        }
        CHECK(cl(R"({"type":"event_msg","payload":{"type":"turn_aborted","reason":"interrupted"}})").state == CodexState::Waiting, "turn_aborted -> Waiting");
        CHECK(cl(R"({"type":"event_msg","payload":{"type":"thread_rolled_back","num_turns":1}})").state == CodexState::Waiting, "thread_rolled_back -> Waiting");
        CHECK(!cl(R"({"type":"event_msg","payload":{"type":"user_message","message":"hi"}})").isBoundary, "user_message is not a turn boundary");
        CHECK(!cl(R"({"type":"event_msg","payload":{"type":"token_count"}})").isBoundary, "token_count is not a boundary");
        CHECK(!cl(R"({"type":"response_item","payload":{"type":"function_call"}})").isBoundary, "response_item is not a boundary");
        CHECK(!cl("not json at all").isBoundary, "non-JSON line -> no boundary");
        CHECK(!cl("").isBoundary, "empty line -> no boundary");
    }

    // --- ReadCodexStateDelta: forward byte-cursor + first-sight tail-seek + partial-line safety ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_codex_state_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ root }, ec);
        const std::wstring path = root + L"\\rollout-state.jsonl";
        const auto writeFile = [](const std::wstring& p, const std::string& b) {
            const HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD wr = 0;
            ::WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr);
            ::CloseHandle(h);
        };

        const std::string turn1 =
            R"({"type":"session_meta","payload":{"cwd":"K:/x"}})" "\n"
            R"({"type":"turn_context","payload":{"model":"gpt-5.5"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t1"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"agent_message","message":"working"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"task_complete","turn_id":"t1","last_agent_message":"done 1"}})" "\n";
        writeFile(path, turn1);
        int64_t off = 0;
        std::wstring lastMsg;
        CHECK(ReadCodexStateDelta(path, off, CodexState::Unknown, &lastMsg) == CodexState::Waiting, "first read: tail ends on task_complete -> Waiting");
        CHECK(lastMsg == L"done 1", "delta surfaces last_agent_message");
        CHECK(off == static_cast<int64_t>(turn1.size()), "cursor advanced to EOF (whole file consumed)");

        const int64_t offAtEof = off;
        CHECK(ReadCodexStateDelta(path, off, CodexState::Waiting) == CodexState::Waiting && off == offAtEof, "no new bytes -> sticky state, cursor steady");

        // A new turn OPENS (append task_started, no complete) -> Running, reading only the delta.
        const std::string turn2open = turn1 +
            R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t2"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"agent_message","message":"thinking"}})" "\n";
        writeFile(path, turn2open);
        CHECK(ReadCodexStateDelta(path, off, CodexState::Waiting) == CodexState::Running, "appended task_started (open turn) -> Running");

        const std::string turn2done = turn2open +
            R"({"type":"event_msg","payload":{"type":"task_complete","turn_id":"t2","last_agent_message":"done 2"}})" "\n";
        writeFile(path, turn2done);
        CHECK(ReadCodexStateDelta(path, off, CodexState::Running) == CodexState::Waiting, "appended task_complete -> Waiting");

        // First-sight (offset 0) tail-seek: a fresh reader derives state from the file end.
        int64_t offRest = 0;
        CHECK(ReadCodexStateDelta(path, offRest, CodexState::Unknown) == CodexState::Waiting, "fresh reader, at-rest file -> Waiting");
        writeFile(path, turn2open); // a file whose last boundary is an OPEN turn
        int64_t offMid = 0;
        CHECK(ReadCodexStateDelta(path, offMid, CodexState::Unknown) == CodexState::Running, "fresh reader, mid-turn file -> Running");

        // A partial trailing line (append in flight, no newline) is NOT consumed — the last COMPLETE
        // boundary wins, and the cursor stops before the partial so the next read re-sees it whole.
        writeFile(path, turn2done + R"({"type":"event_msg","payload":{"type":"task_started")"); // truncated, no newline
        int64_t offPartial = 0;
        CHECK(ReadCodexStateDelta(path, offPartial, CodexState::Unknown) == CodexState::Waiting, "partial trailing line ignored -> last complete boundary (Waiting) wins");

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }
}

static void TestProcessInspectLive()
{
    std::wprintf(L"ProcessInspect live PEB self-read (our own process):\n");
    const uint32_t self = ::GetCurrentProcessId();

    const auto snap = SnapshotProcesses();
    CHECK(!snap.empty(), "SnapshotProcesses returns a non-empty census");
    bool foundSelf = false;
    std::wstring selfImage;
    for (const auto& e : snap)
    {
        if (e.pid == self)
        {
            foundSelf = true;
            selfImage = e.image;
        }
    }
    CHECK(foundSelf, "snapshot contains our own pid");
    CHECK(foundSelf && ImageNameEq(selfImage, L"m5_tests.exe"), "snapshot image leaf for self is m5_tests.exe");

    const auto cwd = ReadProcessCwd(self);
    CHECK(!cwd.empty(), "ReadProcessCwd(self) non-empty");
    wchar_t cur[MAX_PATH]{};
    const DWORD n = ::GetCurrentDirectoryW(MAX_PATH, cur);
    std::wstring curw(cur, n);
    while (!curw.empty() && (curw.back() == L'\\' || curw.back() == L'/'))
    {
        curw.pop_back();
    }
    CHECK(NormDirKey(cwd) == NormDirKey(curw), "ReadProcessCwd(self) matches GetCurrentDirectory");

    const auto cl = ReadProcessCommandLine(self);
    CHECK(!cl.empty(), "ReadProcessCommandLine(self) non-empty");
    CHECK(cl.find(L"m5_tests") != std::wstring::npos, "command line carries our exe name");

    const auto env = ReadProcessEnv(self);
    CHECK(!env.empty(), "ReadProcessEnv(self) non-empty");
    CHECK(!EnvLookup(env, L"SystemRoot").empty(), "live env carries SystemRoot");
    CHECK(!EnvLookup(env, L"systemroot").empty(), "EnvLookup case-insensitive against a live env");

    CHECK(ProcessStartUnixMs(self) > 0, "ProcessStartUnixMs(self) > 0");
    CHECK(ProcessAlive(self), "ProcessAlive(self) true");
    CHECK(!ProcessAlive(0), "ProcessAlive(0) false");

    // Full facts read for self (exercises the OS read + parse path end-to-end). We can't assert
    // claude-specific fields, but the PEB-derived ones must be populated.
    const auto facts = ReadClaudeFacts(self);
    CHECK(facts.pid == self, "ReadClaudeFacts carries the pid");
    CHECK(!facts.cwd.empty() && !facts.commandline.empty(), "ReadClaudeFacts filled cwd + commandline from the PEB");
    CHECK(facts.startUnixMs > 0, "ReadClaudeFacts filled start time");
}

static void TestBringToFrontHeuristics()
{
    std::wprintf(L"Bring Window To Front heuristics (PathLeaf + tab scorers, PURE):\n");

    // --- PathLeaf: last segment, separator-agnostic, trailing-slash tolerant ---
    CHECK(PathLeaf(L"K:\\source\\Agentmaster") == L"Agentmaster", "PathLeaf backslash path");
    CHECK(PathLeaf(L"K:\\source\\Agentmaster\\") == L"Agentmaster", "PathLeaf trailing backslash");
    CHECK(PathLeaf(L"K:/source/api/") == L"api", "PathLeaf forward slashes");
    CHECK(PathLeaf(L"api") == L"api", "PathLeaf bare leaf");
    CHECK(PathLeaf(L"").empty(), "PathLeaf empty");

    // --- ScoreClaudeTabName tiers: exact (100) > claude word (90) > glyph (80) > containment (70)
    //     > title head (60) > cwd leaf (40) ---
    CHECK(ScoreClaudeTabName(L"npyiter perf?", L"npyiter perf?", L"") == 100, "tab score: exact title match");
    CHECK(ScoreClaudeTabName(L"  npyiter perf? ", L"npyiter PERF?", L"") == 100, "tab score: exact is trimmed + case-insensitive");
    CHECK(ScoreClaudeTabName(L"Claude Code", L"", L"") == 90, "tab score: the word claude");
    CHECK(ScoreClaudeTabName(L"my CLAUDE session", L"", L"") == 90, "tab score: claude case-insensitive");
    CHECK(ScoreClaudeTabName(L"\x2733 Fixing the build", L"", L"") == 80, "tab score: leading OSC status glyph");
    CHECK(ScoreClaudeTabName(L"resume flow", L"the resume flow", L"") == 70, "tab score: name contained in hint");
    CHECK(ScoreClaudeTabName(L"[1] the resume flow (wt)", L"the resume flow", L"") == 70, "tab score: hint contained in name");
    CHECK(ScoreClaudeTabName(L"fixing the build error", L"Fixing the build error in CI", L"") == 70, "tab score: name is a prefix of the hint (containment)");
    CHECK(ScoreClaudeTabName(L"\x2734 fixing the build error CI-side", L"Fixing the build error in CI", L"") == 60, "tab score: title head past a foreign prefix");
    CHECK(ScoreClaudeTabName(L"short", L"shor", L"") == 0, "tab score: tiny hints never match");
    CHECK(ScoreClaudeTabName(L"ELI: Agentmaster", L"", L"Agentmaster") == 40, "tab score: cwd leaf");
    CHECK(ScoreClaudeTabName(L"PowerShell", L"a long enough hint", L"src") == 0, "tab score: no signal");
    CHECK(ScoreClaudeTabName(L"", L"whatever hint", L"dir") == 0, "tab score: empty name");
    // priority: a claude-word name keeps 90 even when weaker tiers also match
    CHECK(ScoreClaudeTabName(L"claude \x2014 Agentmaster", L"claude \x2014 Agentmaster and more", L"Agentmaster") == 90, "tab score: strongest signal wins");

    // --- ScoreTabNameTokens: hand-renamed tab labels vs the conversation corpus (whole words) ---
    const auto corpus = TokenizeTextLower(L"Fix the --resume path so the archived session restores; also the npyiter migration plan.");
    CHECK(ScoreTabNameTokens(L"am resume", corpus) == 50, "token overlap: every >=3-char token present ('am' dropped as noise)");
    CHECK(ScoreTabNameTokens(L"npyiter migration", corpus) == 50, "token overlap: two tokens, both present");
    CHECK(ScoreTabNameTokens(L"am runner", corpus) == 0, "token overlap: one missing token kills the match");
    CHECK(ScoreTabNameTokens(L"npyiter perf", corpus) == 0, "token overlap: all-or-nothing");
    CHECK(ScoreTabNameTokens(L"resumes", corpus) == 0, "token overlap: longer-than-corpus-word is a miss ('resumes' !~ 'resume')");
    CHECK(ScoreTabNameTokens(L"am gh act", TokenizeTextLower(L"do we have gh actions publishes?")) == 50, "token overlap: tab token as a PREFIX of a corpus word (act ~ actions)");
    CHECK(ScoreTabNameTokens(L"gh am", corpus) == 0, "token overlap: only short tokens -> no signal");
    CHECK(ScoreTabNameTokens(L"am resume", {}) == 0, "token overlap: empty corpus");
    CHECK(TokenizeTextLower(L"").empty(), "tokenize: empty text");
    CHECK(TokenizeTextLower(L"a bb ccc").size() == 1, "tokenize: drops tokens under 3 chars");

    // --- PickClaudeTab: the full per-window pick (subset disqualifier + head/full corpus tiers +
    //     unique guard), over the real-world shape that motivated it: a WT window of hand-renamed
    //     claude tabs. ---
    {
        const std::vector<std::wstring> tabs{
            L"npyiter test all x all", L"npyiter perf?", L"npyiter multithread",
            L"npyiter migrate", L"am resume", L"npyiter pr"
        };
        int score = 0;
        bool unique = false;
        // The resume conversation: its tab's one meaningful token is unique among the tabs.
        auto corpus = TokenizeTextLower(L"please fix the --resume path so archived sessions restore");
        int idx = PickClaudeTab(tabs, {}, L"", corpus, corpus, score, unique);
        CHECK(idx == 4 && score == 50 && unique, "pick: token-unique renamed tab wins");
        // The perf conversation mentions npyiter but never 'perf' — the generic-prefix tab
        // ("npyiter pr" == {npyiter}, a strict subset of its siblings) must NOT win by tokens.
        corpus = TokenizeTextLower(L"Can NpyIter make matmul or dot or argsort faster?");
        idx = PickClaudeTab(tabs, {}, L"", corpus, corpus, score, unique);
        CHECK(score == 0, "pick: generic-prefix tab disqualified (strict subset of a sibling)");
        // An exact title hint wins outright.
        idx = PickClaudeTab(tabs, { L"npyiter migrate" }, L"", {}, {}, score, unique);
        CHECK(idx == 3 && score == 100 && unique, "pick: exact title hint wins uniquely");
        // Tied scorers are reported non-unique (the caller must not select).
        const std::vector<std::wstring> twins{ L"PowerShell", L"PowerShell", L"claude one", L"claude two" };
        idx = PickClaudeTab(twins, {}, L"", {}, {}, score, unique);
        CHECK(idx == 2 && score == 90 && !unique, "pick: tied claude tabs -> not unique");
        // Equal token sets stay eligible and tie out (vs the strict-subset disqualifier).
        const std::vector<std::wstring> same{ L"npyiter perf", L"npyiter perf?" };
        corpus = TokenizeTextLower(L"npyiter perf work");
        idx = PickClaudeTab(same, {}, L"", corpus, corpus, score, unique);
        CHECK(score == 50 && !unique, "pick: equal token sets tie (not subset-disqualified)");
        // Head corpus (title + FIRST prompt) outranks the full corpus: a tab named for the
        // conversation's PURPOSE ("am gh act" ~ "gh actions" in the first prompt) beats a tab
        // matching an incidental later-prompt word ("am changes" ~ "changes" said much later).
        const std::vector<std::wstring> mixed{ L"am gh act", L"am changes" };
        const auto head = TokenizeTextLower(L"Do we have publishes in the gh actions?");
        const auto full = TokenizeTextLower(L"Do we have publishes in the gh actions? later: apply the changes to the workflow");
        idx = PickClaudeTab(mixed, {}, L"", head, full, score, unique);
        CHECK(idx == 0 && score == 50 && unique, "pick: head-corpus match (50) outranks full-corpus match (45)");
    }

    // --- ReadTranscriptInfoIn: custom-title lines (the LAST one wins) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_bringfront_test_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring projRoot = root + L"\\projects";
        const std::wstring cwd = L"C:\\AmBringFront\\proj";
        const std::wstring dir = projRoot + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
        const std::string content =
            "{\"type\":\"custom-title\",\"customTitle\":\"old label\",\"sessionId\":\"ct\"}\n"
            "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"hello world prompt\"},\"gitBranch\":\"main\"}\n"
            "{\"type\":\"custom-title\",\"customTitle\":\"am resume\",\"sessionId\":\"ct\"}\n";
        MakeJsonl(dir + L"\\ct.jsonl", content, 5000, 1000);
        const auto ti = ReadTranscriptInfoIn(projRoot, cwd, L"ct", 0, 10);
        CHECK(ti.found, "custom-title: transcript read");
        CHECK(ti.customTitle == L"am resume", "custom-title: the LAST custom-title line wins");
        CHECK(ti.title == L"hello world prompt", "custom-title does not displace the first-prompt title field");
        CHECK(ti.gitBranch == L"main", "custom-title lines don't break gitBranch capture");
        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }
}

static void TestTranscriptStore()
{
    std::wprintf(L"TranscriptStore (enumerate + classify + stats + quick facts):\n");

    // --- IsSessionIdStem: the uuid filename filter (excludes legacy agent-* files) ---
    CHECK(IsSessionIdStem(L"3f98f88e-b122-41b5-9048-4b93a39fe5fa"), "uuid stem accepted");
    CHECK(!IsSessionIdStem(L"agent-a05bb6b"), "legacy agent-* stem rejected");
    CHECK(!IsSessionIdStem(L"3f98f88e-b122-41b5-9048-4b93a39fe5f"), "35 chars rejected");
    CHECK(!IsSessionIdStem(L"3f98f88e-b122-41b5-9048_4b93a39fe5fa"), "wrong separator rejected");

    // --- ParseTranscriptTimestamp: ISO-Z (the entire recent corpus) + epoch strata ---
    CHECK(ParseTranscriptTimestamp(L"1970-01-01T00:00:00.000Z") == 0, "epoch-zero ISO -> 0 (degenerate, treated as unset)");
    CHECK(ParseTranscriptTimestamp(L"2026-06-10T00:00:00Z") == 1781049600000LL, "ISO without millis");
    CHECK(ParseTranscriptTimestamp(L"2026-06-10T00:00:00.250Z") == 1781049600250LL, "ISO with millis");
    CHECK(ParseTranscriptTimestamp(L"2026-06-10T00:00:00.2Z") == 1781049600200LL, "ISO with 1-digit fraction scales to ms");
    CHECK(ParseTranscriptTimestamp(L"1781049600000") == 1781049600000LL, "pure-digit epoch ms");
    CHECK(ParseTranscriptTimestamp(L"1781049600") == 1781049600000LL, "pure-digit epoch seconds -> ms");
    CHECK(ParseTranscriptTimestamp(L"garbage") == 0, "garbage -> 0");
    CHECK(ParseTranscriptTimestamp(L"") == 0, "empty -> 0");

    // --- IsNoiseUserPrompt: the §6a control-marker rule set ---
    CHECK(IsNoiseUserPrompt(L"<command-name>/clear</command-name>"), "command-name echo is noise");
    CHECK(IsNoiseUserPrompt(L"  <command-message>x</command-message>"), "command-message (leading ws) is noise");
    CHECK(IsNoiseUserPrompt(L"<local-command-stdout>x"), "local-command wrapper is noise");
    CHECK(IsNoiseUserPrompt(L"<bash-input>ls</bash-input>"), "bash-input echo is noise");
    CHECK(IsNoiseUserPrompt(L"<task-notification>done</task-notification>"), "task-notification is noise");
    CHECK(IsNoiseUserPrompt(L"<system-reminder>r</system-reminder>"), "system-reminder is noise");
    CHECK(IsNoiseUserPrompt(L"Caveat: the messages below were generated"), "Caveat preamble is noise");
    CHECK(IsNoiseUserPrompt(L"[Request interrupted by user]"), "interrupt marker is noise");
    CHECK(IsNoiseUserPrompt(L"[Request interrupted by user for tool use]"), "tool interrupt marker is noise");
    CHECK(!IsNoiseUserPrompt(L"fix the build please"), "a real prompt is NOT noise");
    CHECK(!IsNoiseUserPrompt(L"explain <command-name> semantics"), "marker NOT at start is not noise");

    // --- PickDisplayTitle precedence ---
    CHECK(PickDisplayTitle(L"c", L"a", L"s", L"f") == L"c", "customTitle wins");
    CHECK(PickDisplayTitle(L"", L"a", L"s", L"f") == L"a", "aiTitle second");
    CHECK(PickDisplayTitle(L"", L"", L"s", L"f") == L"s", "legacy summary third");
    CHECK(PickDisplayTitle(L"", L"", L"", L"f") == L"f", "first prompt last");

    // --- ClassifyTranscriptLine: canned lines from the real schema ---
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","timestamp":"2026-06-10T00:00:00Z","cwd":"K:\\x","gitBranch":"main","message":{"role":"user","content":"hello there"}})", 100, 100);
        CHECK(f.kind == TranscriptLineKind::UserPrompt && !f.meta, "user string content -> REAL UserPrompt");
        CHECK(f.userText == L"hello there", "userText extracted");
        CHECK(f.timestampMs == 1781049600000LL, "timestamp parsed");
        CHECK(f.cwd == L"K:\\x" && f.gitBranch == L"main", "cwd + gitBranch extracted");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":"[Request interrupted by user]"}})", 100, 100);
        CHECK(f.kind == TranscriptLineKind::UserPrompt && f.meta && f.userText.empty(), "interrupt marker -> meta, no userText");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","isCompactSummary":true,"message":{"content":"This session is being continued"}})", 100, 100);
        CHECK(f.meta, "isCompactSummary -> meta");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"42 passed"}]},"toolUseResult":{"stdout":"ok!","stderr":""}})", 100, 100);
        CHECK(f.kind == TranscriptLineKind::UserToolResult, "tool_result blocks -> UserToolResult");
        CHECK(f.agentText.find(L"42 passed") != std::wstring::npos, "tool_result content in agentText");
        CHECK(f.agentText.find(L"ok!") != std::wstring::npos, "toolUseResult stdout in agentText");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"assistant","timestamp":"2026-06-10T00:00:01Z","message":{"role":"assistant","content":[{"type":"thinking","thinking":"hmm secret"},{"type":"text","text":"done."},{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"git log"}},{"type":"tool_use","id":"t2","name":"Read","input":{"file_path":"a.cpp"}}]}})", 0, 200);
        CHECK(f.kind == TranscriptLineKind::Assistant, "assistant line classified");
        CHECK(f.toolUses == 2, "two tool_use blocks counted");
        CHECK(f.agentText.find(L"done.") != std::wstring::npos && f.agentText.find(L"hmm secret") != std::wstring::npos, "text + thinking in agentText");
        CHECK(f.agentText.find(L"Bash") != std::wstring::npos && f.agentText.find(L"git log") != std::wstring::npos, "tool name + input in agentText");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"assistant","isSidechain":true,"timestamp":"2026-06-10T00:00:02Z","message":{"content":[{"type":"text","text":"sub"}]}})", 0, 50);
        CHECK(f.sidechain, "isSidechain flagged");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"custom-title","customTitle":"am resume"})");
        CHECK(f.kind == TranscriptLineKind::CustomTitle && f.title == L"am resume", "custom-title payload");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"ai-title","aiTitle":"Fixing the build"})");
        CHECK(f.kind == TranscriptLineKind::AiTitle && f.title == L"Fixing the build", "ai-title payload");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"summary","summary":"Legacy summary","leafUuid":"x"})");
        CHECK(f.kind == TranscriptLineKind::Summary && f.title == L"Legacy summary", "legacy summary payload");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","forkedFrom":{"sessionId":"f100e8de-b6c5-4fb8-bbe3-9f98eb63ab11","messageUuid":"m1"},"message":{"content":"copied"}})", 50, 0);
        CHECK(f.forkedFromId == L"f100e8de-b6c5-4fb8-bbe3-9f98eb63ab11", "forkedFrom.sessionId extracted");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"system","subtype":"away_summary","timestamp":"2026-06-10T01:00:00Z","content":"While you were away..."})", 0, 100);
        CHECK(f.kind == TranscriptLineKind::System && f.agentText.find(L"away") != std::wstring::npos, "system content in agentText");
    }
    {
        const auto f = ClassifyTranscriptLine(L"not json at all");
        CHECK(f.kind == TranscriptLineKind::Other, "non-JSON line tolerated as Other");
    }
    {
        // Caps respected: a 100-char budget truncates, 0 budget extracts nothing.
        const std::wstring big(500, L'x');
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":")" + big + LR"("}})", 100, 0);
        CHECK(f.userText.size() <= 100, "maxUserTextChars cap respected");
        const auto f0 = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":"hi"}})", 0, 0);
        CHECK(f0.userText.empty(), "0 budget -> no text extraction");
    }

    // --- Enumerate + Scan + Stats + QuickFacts over a temp projects root ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_store_test_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring projRoot = root + L"\\projects";
        const std::wstring dirA = projRoot + L"\\K--source-AmStoreA";
        const std::wstring dirB = projRoot + L"\\K--source-AmStoreB";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dirA + L"\\11111111-1111-1111-1111-111111111111" }, ec); // a <sid>/ subdir (subagents home) — must NOT enumerate
        std::filesystem::create_directories(std::filesystem::path{ dirB }, ec);

        const std::string sessA = // a realistic mini-transcript
            "{\"type\":\"permission-mode\",\"permissionMode\":\"bypassPermissions\"}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"cwd\":\"K:\\\\source\\\\AmStoreA\",\"gitBranch\":\"dev\",\"message\":{\"content\":\"Caveat: injected preamble\"}}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"cwd\":\"K:\\\\source\\\\AmStoreA\",\"message\":{\"content\":\"build the thing\"}}\n"
            "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:09Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"on it\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"build\"}}],\"usage\":{\"input_tokens\":50,\"cache_creation_input_tokens\":10,\"cache_read_input_tokens\":500,\"output_tokens\":10}}}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:20Z\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"ok\"}]}}\n"
            "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:30Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"done\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":100,\"cache_creation_input_tokens\":20,\"cache_read_input_tokens\":2000,\"output_tokens\":30}}}\n"
            "{\"type\":\"system\",\"subtype\":\"away_summary\",\"timestamp\":\"2026-06-01T10:30:00Z\",\"content\":\"away note\"}\n"
            "{\"type\":\"custom-title\",\"customTitle\":\"store test A\"}\n"
            "{\"type\":\"permission-mode\",\"permissionMode\":\"bypassPermissions\"}\n";
        const std::wstring sidA = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        MakeJsonl(dirA + L"\\" + sidA + L".jsonl", sessA, 5000, 1000);
        MakeJsonl(dirA + L"\\agent-a05bb6b.jsonl", "{}", 6000, 1000); // legacy subagent file — must NOT enumerate
        const std::wstring sidB = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        MakeJsonl(dirB + L"\\" + sidB + L".jsonl", "{\"type\":\"mode\",\"mode\":\"normal\"}\n", 9000, 8000); // never-prompted prefix-only

        const auto all = EnumerateTranscriptsIn(projRoot, 0);
        CHECK(all.size() == 2, "enumerate: exactly the 2 uuid transcripts (agent-*/subdirs excluded)");
        CHECK(all.size() == 2 && all[0].sessionId == sidB && all[1].sessionId == sidA, "enumerate: newest-mtime first");
        CHECK(all.size() == 2 && all[1].projectDirLeaf == L"K--source-AmStoreA" && all[1].sizeBytes > 0, "enumerate: leaf + size filled");
        const auto windowed = EnumerateTranscriptsIn(projRoot, 7000);
        CHECK(windowed.size() == 1 && windowed[0].sessionId == sidB, "enumerate: sinceUnixMs window filters by mtime");
        CHECK(EnumerateTranscriptsIn(projRoot + L"\\missing", 0).empty(), "enumerate: missing root -> empty");

        // Stats: one accumulate over session A.
        TranscriptStats st;
        CHECK(AccumulateTranscriptStats(dirA + L"\\" + sidA + L".jsonl", st), "stats: accumulate ok");
        CHECK(st.userPrompts == 1, "stats: ONE real prompt (Caveat + tool_result filtered)");
        CHECK(st.firstUserPrompt == L"build the thing", "stats: first REAL prompt captured");
        CHECK(st.assistantLines == 2 && st.toolUses == 1, "stats: assistant lines + tool uses counted");
        CHECK(st.contextTokens == 2150, "stats: contextTokens = NEWEST assistant usage (100+20+2000+30), not the first (570)");
        CHECK(st.customTitle == L"store test A", "stats: custom title captured");
        CHECK(st.firstTimestampMs == 1780308000000LL, "stats: first timestamp (2026-06-01T10:00:00Z)");
        CHECK(st.lastTimestampMs == 1780308030000LL, "stats: last activity is the LAST MESSAGE (10:00:30), NOT the away_summary");
        CHECK(st.cwd == L"K:\\source\\AmStoreA" && st.gitBranch == L"dev", "stats: cwd + branch from the lines");
        CHECK(st.parsedBytes == static_cast<int64_t>(sessA.size()), "stats: cursor consumed the whole file");
        CHECK(PickDisplayTitle(st.customTitle, st.aiTitle, st.summary, st.firstUserPrompt) == L"store test A", "stats: display title = custom title");

        // Incremental: append a new turn; re-accumulate picks up ONLY the suffix.
        {
            const std::string more =
                "{\"type\":\"user\",\"timestamp\":\"2026-06-01T11:00:00Z\",\"message\":{\"content\":\"and another thing\"}}\n";
            const HANDLE h = ::CreateFileW((dirA + L"\\" + sidA + L".jsonl").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0;
            ::WriteFile(h, more.data(), static_cast<DWORD>(more.size()), &w, nullptr);
            ::CloseHandle(h);
            CHECK(AccumulateTranscriptStats(dirA + L"\\" + sidA + L".jsonl", st), "stats: incremental accumulate ok");
            CHECK(st.userPrompts == 2, "stats: appended prompt counted once");
            CHECK(st.lastTimestampMs == 1780311600000LL, "stats: last activity advanced to the appended turn (11:00:00)");
            CHECK(st.firstUserPrompt == L"build the thing", "stats: first prompt unchanged across resume");
        }
        // Replaced (shrunk) file: stats self-reset and rebuild.
        {
            MakeJsonl(dirA + L"\\" + sidA + L".jsonl", "{\"type\":\"user\",\"timestamp\":\"2026-06-02T00:00:00Z\",\"message\":{\"content\":\"fresh\"}}\n", 9500, 9500);
            CHECK(AccumulateTranscriptStats(dirA + L"\\" + sidA + L".jsonl", st), "stats: shrink-rebuild ok");
            CHECK(st.userPrompts == 1 && st.firstUserPrompt == L"fresh", "stats: shrink resets and rebuilds from 0");
            CHECK(st.contextTokens == 0, "stats: shrink-rebuild clears contextTokens (replaced file has no assistant usage)");
        }
        TranscriptStats gone;
        CHECK(!AccumulateTranscriptStats(dirA + L"\\missing.jsonl", gone) && !gone.found, "stats: vanished file -> found=false");

        // QuickFacts: created from the first timestamped line; last activity skips the
        // trailing away_summary + state lines; fork detection flips created to file birth.
        MakeJsonl(dirA + L"\\" + sidA + L".jsonl", sessA, 5000, 1000); // restore the full fixture
        {
            const auto q = ReadTranscriptQuickFacts(dirA + L"\\" + sidA + L".jsonl", 1000);
            CHECK(q.found, "quick: read ok");
            CHECK(q.createdMs == 1780308000000LL, "quick: created = first line timestamp (not file birth)");
            CHECK(q.lastActivityMs == 1780308030000LL, "quick: last activity = last MESSAGE ts (away_summary + custom-title + permission-mode tail skipped)");
            CHECK(!q.fork && q.cwd == L"K:\\source\\AmStoreA", "quick: not a fork; cwd from head");
        }
        {
            const std::string fork =
                "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"forkedFrom\":{\"sessionId\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\",\"messageUuid\":\"m1\"},\"message\":{\"content\":\"build the thing\"}}\n";
            const std::wstring sidF = L"cccccccc-cccc-4ccc-8ccc-cccccccccccc";
            MakeJsonl(dirA + L"\\" + sidF + L".jsonl", fork, 9990, 9990);
            const auto q = ReadTranscriptQuickFacts(dirA + L"\\" + sidF + L".jsonl", 9990);
            CHECK(q.fork && q.forkedFromId == sidA, "quick: fork detected with parent id");
            CHECK(q.createdMs == 9990, "quick: a fork's created = file birth (copied line timestamps lie)");
        }
        {
            const auto q = ReadTranscriptQuickFacts(dirB + L"\\" + sidB + L".jsonl", 8000);
            CHECK(q.found && q.createdMs == 0 && q.lastActivityMs == 0, "quick: prefix-only (never-prompted) file -> zeros (caller falls back to birth/mtime)");
        }

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- live smoke over the REAL corpus (guarded: skips silently when absent) ---
    {
        const auto recent = EnumerateTranscripts(NowMsTest() - 92LL * 24 * 3600 * 1000);
        if (!recent.empty())
        {
            bool allUuid = true;
            for (const auto& r : recent)
            {
                if (!IsSessionIdStem(r.sessionId))
                {
                    allUuid = false;
                }
            }
            CHECK(allUuid, "live: every enumerated stem is a session uuid");
            const auto q = ReadTranscriptQuickFacts(recent[0].path, recent[0].birthMs);
            CHECK(q.found, "live: quick facts read the newest real transcript");
            CHECK(q.lastActivityMs == 0 || q.lastActivityMs <= recent[0].mtimeMs + 60000, "live: last-activity <= mtime (+clock slack) — mtime is the superset");
            std::wprintf(L"  [info] live corpus: %zu transcripts in 92d window; newest lastActivity-vs-mtime gap %lld s\n",
                         recent.size(), q.lastActivityMs ? (recent[0].mtimeMs - q.lastActivityMs) / 1000 : -1);
        }
    }
}

static void TestContinuationChain()
{
    std::wprintf(L"ContinuationChain (/clear + plan-restart tail resolution — pure resolver):\n");
    // A node builder: (id, cwd, created, lastActivity, [fork]). Times in ms.
    const auto N = [](const wchar_t* id, const wchar_t* cwd, int64_t created, int64_t last, bool fork = false) {
        SessionChainNode n;
        n.sessionId = id;
        n.cwd = cwd;
        n.createdMs = created;
        n.lastActivityMs = last;
        n.fork = fork;
        return n;
    };

    // The real DwhGateway scenario: head (7b2eab70) -> mid (28b301d9, +24s) -> tail (aee7610f, +11min).
    {
        std::vector<SessionChainNode> v{
            N(L"head", L"K:\\proj\\a", 0, 1000000),
            N(L"mid", L"K:\\proj\\a", 1024000, 2000000), // +24s after head ended
            N(L"tail", L"K:\\proj\\a", 2660000, 3000000), // +11min after mid ended
        };
        auto r = ResolveContinuationChainTail(v, L"head");
        CHECK(r.tailId == L"tail" && r.hops == 2, "chain: head follows two /clear links to the tail");
        CHECK(ResolveContinuationChainTail(v, L"mid").tailId == L"tail", "chain: from the MIDDLE link, still reach the tail");
        CHECK(ResolveContinuationChainTail(v, L"mid").hops == 1, "chain: middle->tail is one hop");
        auto t = ResolveContinuationChainTail(v, L"tail");
        CHECK(t.tailId == L"tail" && t.hops == 0, "chain: the tail resolves to itself (no redirect)");
    }

    // Gap too large => a NEW conversation, not a continuation: tail link starts >15min after mid.
    {
        std::vector<SessionChainNode> v{
            N(L"head", L"K:\\proj\\a", 0, 1000000),
            N(L"mid", L"K:\\proj\\a", 1024000, 2000000),
            N(L"far", L"K:\\proj\\a", 2000000 + 16 * 60 * 1000, 4000000), // 16min after mid ended
        };
        auto r = ResolveContinuationChainTail(v, L"head");
        CHECK(r.tailId == L"mid" && r.hops == 1, "chain: a >15min gap stops the chain (mid is the tail)");
    }

    // Parallel/overlap => ambiguous => bail (never silently merge two same-dir claudes).
    {
        std::vector<SessionChainNode> v{
            N(L"a", L"K:\\proj\\a", 0, 1000000),
            N(L"b", L"K:\\proj\\a", 1024000, 2000000),
            N(L"par", L"K:\\proj\\a", 1100000, 2200000), // started while 'b' was still active
        };
        auto r = ResolveContinuationChainTail(v, L"a");
        CHECK(r.tailId == L"a" && r.hops == 0, "chain: overlapping parallel successors => no redirect");
    }

    // A fork is a BRANCH, never a continuation target.
    {
        std::vector<SessionChainNode> v{
            N(L"a", L"K:\\proj\\a", 0, 1000000),
            N(L"forked", L"K:\\proj\\a", 1024000, 2000000, /*fork*/ true),
        };
        CHECK(ResolveContinuationChainTail(v, L"a").tailId == L"a", "chain: a fork successor is skipped (branch, not continuation)");
    }

    // Different cwd is a different conversation; a case/slash-variant of the SAME dir still chains (Rule #8).
    {
        std::vector<SessionChainNode> v{
            N(L"a", L"K:\\Proj\\A", 0, 1000000),
            N(L"other", L"K:\\proj\\b", 1024000, 2000000), // different dir — must NOT chain
            N(L"cont", L"k:/proj/a", 1100000, 2000000), // same dir, case+slash variant — MUST chain
        };
        auto r = ResolveContinuationChainTail(v, L"a");
        CHECK(r.tailId == L"cont" && r.hops == 1, "chain: case/slash-variant same dir chains; a different dir does not");
    }

    // A custom (tighter) gapMax is honored.
    {
        std::vector<SessionChainNode> v{
            N(L"head", L"K:\\proj\\a", 0, 1000000),
            N(L"mid", L"K:\\proj\\a", 1024000, 2000000), // +24s (within 30s)
            N(L"tail", L"K:\\proj\\a", 2660000, 3000000), // +11min (beyond 30s)
        };
        auto r = ResolveContinuationChainTail(v, L"head", /*gapMaxMs*/ 30000);
        CHECK(r.tailId == L"mid" && r.hops == 1, "chain: a tight gapMax stops after the 24s link");
    }

    // Robustness: an unknown start id returns itself; an empty node set is safe.
    {
        std::vector<SessionChainNode> v{ N(L"a", L"K:\\proj\\a", 0, 1000000) };
        CHECK(ResolveContinuationChainTail(v, L"ghost").tailId == L"ghost", "chain: unknown start id => returns itself");
        CHECK(ResolveContinuationChainTail({}, L"a").tailId == L"a", "chain: empty nodes => returns the start id");
    }

    // --- ResolveContinuationPredecessor: the EXACT inverse of the forward edge (cross-file lineage) ---
    // The reverse walk ("where did this conversation COME FROM?") must agree with the forward redirect
    // hop-for-hop — they share the single ContinuationNext edge.
    {
        // The same head -> mid -> tail chain, queried BACKWARDS.
        std::vector<SessionChainNode> v{
            N(L"head", L"K:\\proj\\a", 0, 1000000),
            N(L"mid", L"K:\\proj\\a", 1024000, 2000000),
            N(L"tail", L"K:\\proj\\a", 2660000, 3000000),
        };
        CHECK(ResolveContinuationPredecessor(v, L"tail") == L"mid", "predecessor: tail's predecessor is mid (inverse of mid->tail)");
        CHECK(ResolveContinuationPredecessor(v, L"mid") == L"head", "predecessor: mid's predecessor is head");
        CHECK(ResolveContinuationPredecessor(v, L"head").empty(), "predecessor: the origin (head) has no predecessor");
        CHECK(ResolveContinuationPredecessor(v, L"ghost").empty(), "predecessor: an unknown id has no predecessor");
        CHECK(ResolveContinuationPredecessor({}, L"x").empty(), "predecessor: empty nodes => empty");
    }

    // Ambiguity: TWO sessions both forward-continue into the same target (their timelines don't see
    // each OTHER as candidates, but both see T) => the predecessor is ambiguous => empty (never guess).
    {
        std::vector<SessionChainNode> v{
            N(L"A", L"K:\\proj\\a", 0, 990000), // ends just before T; created too early to be B's candidate
            N(L"B", L"K:\\proj\\a", 0, 995000), // ends just before T; created too early to be A's candidate
            N(L"T", L"K:\\proj\\a", 1000000, 1100000),
        };
        CHECK(ResolveContinuationPredecessor(v, L"A").empty() && ResolveContinuationPredecessor(v, L"B").empty(),
              "predecessor: A and B are origins (nothing continues into them)");
        CHECK(ResolveContinuationPredecessor(v, L"T").empty(), "predecessor: two sessions continue into T => ambiguous => empty (no silent merge)");
    }

    // A fork target has no continuation predecessor (a fork is a BRANCH, never a continuation).
    {
        std::vector<SessionChainNode> v{
            N(L"a", L"K:\\proj\\a", 0, 1000000),
            N(L"forked", L"K:\\proj\\a", 1024000, 2000000, /*fork*/ true),
        };
        CHECK(ResolveContinuationPredecessor(v, L"forked").empty(), "predecessor: a fork has no continuation predecessor");
    }
}

// Cross-file conversation lineage on disk: CollectConversationLineage walks a session's predecessors
// — a /clear continuation (a NEW same-cwd session) and a plan-restart parent (the "read the full
// transcript at:" link) — and returns each parent's prompts as a "previous session" segment. Stages a
// throwaway CLAUDE_CONFIG_DIR so the resolvers (ResolveClaudeTranscriptPath / the predecessor scan)
// find the fixtures. [Agentmaster]
static void TestConversationLineage()
{
    std::wprintf(L"ConversationLineage (cross-file /clear + plan-restart previous sessions — disk walk):\n");
    const auto narrow = [](const std::wstring& w) { std::string s; s.reserve(w.size()); for (wchar_t c : w) { s.push_back(static_cast<char>(c)); } return s; }; // ASCII ids only (explicit cast => no C4244)

    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring cfg = std::wstring{ tmp } + L"am_lineage_cfg_" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring projects = cfg + L"\\projects";

    // Point Claude's transcript root at our temp dir for the duration of this test, restore after.
    wchar_t prevBuf[2048]{};
    const DWORD prevN = ::GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", prevBuf, 2048);
    const std::wstring prevCfg{ prevBuf, prevN };
    ::SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", cfg.c_str());

    // --- (1) /clear continuation: B continues A (same cwd, B created shortly after A ended) ---
    {
        const std::wstring cwd = L"K:\\am_lin\\clearcase";
        const std::wstring dir = projects + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
        const std::wstring idA = L"aaaaaaaa-1111-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring idB = L"bbbbbbbb-2222-4bbb-8bbb-bbbbbbbbbbbb";
        const std::string aJson =
            R"j({"type":"user","userType":"external","uuid":"a1","parentUuid":null,"cwd":"K:\\am_lin\\clearcase","message":{"content":"alpha one"},"timestamp":"2026-06-26T09:00:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"a2","parentUuid":"a1","cwd":"K:\\am_lin\\clearcase","message":{"content":"alpha two"},"timestamp":"2026-06-26T09:05:00.000Z"})j" "\n";
        const std::string bJson =
            R"j({"type":"user","userType":"external","uuid":"b1","parentUuid":null,"cwd":"K:\\am_lin\\clearcase","message":{"content":"beta one"},"timestamp":"2026-06-26T09:06:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"b2","parentUuid":"b1","cwd":"K:\\am_lin\\clearcase","message":{"content":"beta two"},"timestamp":"2026-06-26T09:10:00.000Z"})j" "\n";
        MakeJsonl(dir + L"\\" + idA + L".jsonl", aJson, 100000, 90000);
        MakeJsonl(dir + L"\\" + idB + L".jsonl", bJson, 200000, 190000);

        const auto pre = ResolveContinuationPredecessorOnDisk(idB, cwd);
        CHECK(pre.predId == idA, "lineage/disk: B's continuation predecessor is A");

        const auto lin = CollectConversationLineage(idB, cwd, 16);
        CHECK(lin.size() == 1, "lineage/disk: the /clear case yields ONE previous session (A)");
        CHECK(lin.size() == 1 && lin[0].userMsgs.size() == 2 && lin[0].userMsgs[0] == L"alpha one" && lin[0].userMsgs[1] == L"alpha two",
              "lineage/disk: the previous session carries A's prompts in order");
        CHECK(lin.size() == 1 && lin[0].label.empty(), "lineage/disk: a plain cross-file join has no /compact label");

        CHECK(CollectConversationLineage(idA, cwd, 16).empty(), "lineage/disk: the origin session A has no previous session");
    }

    // --- (2) plan-restart parent in a DIFFERENT dir, which itself has a /clear predecessor ---
    // C (child dir) --plan--> P (parent dir) --/clear--> Q (parent dir). The plan hop is cwd-independent
    // (resolved by id), but reaching Q requires the walk to ADVANCE curCwd to P's REAL dir — so this
    // exercises both the plan link AND the cross-dir cwd advance (a plan parent's own /clear lineage).
    {
        const std::wstring childCwd = L"K:\\am_lin\\planchild";
        const std::wstring parentCwd = L"K:\\am_lin\\planparent";
        const std::wstring childDir = projects + L"\\" + EncodeCwdToProjectDir(childCwd);
        const std::wstring parentDir = projects + L"\\" + EncodeCwdToProjectDir(parentCwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ childDir }, ec);
        std::filesystem::create_directories(std::filesystem::path{ parentDir }, ec);
        const std::wstring idQ = L"eeeeeeee-5555-4eee-8eee-eeeeeeeeeeee"; // P's /clear predecessor (parent dir)
        const std::wstring idP = L"cccccccc-3333-4ccc-8ccc-cccccccccccc"; // plan parent (parent dir)
        const std::wstring idC = L"dddddddd-4444-4ddd-8ddd-dddddddddddd"; // plan child (child dir)
        const std::string qJson =
            R"j({"type":"user","userType":"external","uuid":"q1","parentUuid":null,"cwd":"K:\\am_lin\\planparent","message":{"content":"earlier groundwork"},"timestamp":"2026-06-26T09:50:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"q2","parentUuid":"q1","cwd":"K:\\am_lin\\planparent","message":{"content":"more groundwork"},"timestamp":"2026-06-26T09:58:00.000Z"})j" "\n";
        const std::string pJson =
            R"j({"type":"user","userType":"external","uuid":"p1","parentUuid":null,"cwd":"K:\\am_lin\\planparent","message":{"content":"plan the feature"},"timestamp":"2026-06-26T10:00:00.000Z"})j" "\n";
        // The breadcrumb only needs a token whose basename is "<idP>.jsonl" — the parent is then resolved
        // by GLOB on that id (forward slashes keep the embedded path valid JSON), so the link spans dirs.
        const std::string cJson =
            std::string(R"j({"type":"user","userType":"external","uuid":"c1","parentUuid":null,"cwd":"K:\\am_lin\\planchild","message":{"content":"read the full transcript at: /plans/x/)j") +
            narrow(idP) + R"j(.jsonl and continue"},"timestamp":"2026-06-26T10:10:00.000Z"})j" "\n";
        MakeJsonl(parentDir + L"\\" + idQ + L".jsonl", qJson, 100000, 80000);
        MakeJsonl(parentDir + L"\\" + idP + L".jsonl", pJson, 110000, 90000);
        MakeJsonl(childDir + L"\\" + idC + L".jsonl", cJson, 200000, 190000);

        const auto lin = CollectConversationLineage(idC, childCwd, 16);
        CHECK(lin.size() == 2, "lineage/disk: plan parent (other dir) + its /clear predecessor both surface");
        CHECK(lin.size() == 2 && lin[0].userMsgs.size() == 2 && lin[0].userMsgs[0] == L"earlier groundwork",
              "lineage/disk: OLDEST first — P's /clear predecessor Q leads (cwd advanced to P's real dir)");
        CHECK(lin.size() == 2 && lin[1].userMsgs.size() == 1 && lin[1].userMsgs[0] == L"plan the feature",
              "lineage/disk: the plan PARENT P follows Q, before the current session");
    }

    ::SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", prevCfg.empty() ? nullptr : prevCfg.c_str());
    std::error_code ecCleanup;
    std::filesystem::remove_all(std::filesystem::path{ cfg }, ecCleanup);
}

static void TestSessionSearch()
{
    std::wprintf(L"SessionSearch (regex/match/snippet + fast phase + history + presence + index):\n");

    // --- BuildSearchRegex: literal escape + fuzzy lazy-gap join ---
    CHECK(BuildSearchRegex(L"a.b", false) == LR"(a\.b)", "regex: literal dot escaped");
    CHECK(BuildSearchRegex(L"abc", true) == LR"(a.*?b.*?c)", "regex: fuzzy lazy-gap join");
    CHECK(BuildSearchRegex(L"a c", true) == LR"(a.*?c)", "regex: fuzzy drops whitespace");
    CHECK(BuildSearchRegex(L"x(y)", false) == LR"(x\(y\))", "regex: parens escaped");

    // --- MatchesQueryText (pre-folded inputs) ---
    CHECK(MatchesQueryText(L"the quick brown fox", L"quick", false), "match: substring");
    CHECK(!MatchesQueryText(L"the quick brown fox", L"quirk", false), "match: non-substring misses");
    CHECK(MatchesQueryText(L"the quick brown fox", L"qbf", true), "match: fuzzy subsequence");
    CHECK(!MatchesQueryText(L"the quick brown fox", L"fbq", true), "match: fuzzy order matters");
    CHECK(MatchesQueryText(L"anything", L"", true), "match: empty query matches");

    // --- MatchesPathQuery: '/' and '\' equivalent for directory haystacks (pre-folded) ---
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"src/foo", false), "pathmatch: forward-slash query finds a backslash dir");
    CHECK(MatchesPathQuery(L"c:/users/src/foo", L"src\\foo", false), "pathmatch: backslash query finds a forward-slash dir");
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"src\\foo", false), "pathmatch: backslash query, backslash dir (unchanged)");
    CHECK(MatchesPathQuery(L"c:/users/src/foo", L"src/foo", false), "pathmatch: forward query, forward dir (unchanged)");
    CHECK(!MatchesPathQuery(L"c:\\users\\src\\foo", L"src/bar", false), "pathmatch: a non-substring still misses");
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"users/src/foo", true), "pathmatch: fuzzy honors slash equivalence");
    // A separator-free needle behaves EXACTLY like MatchesQueryText (the allocation-free fast path).
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"src", false) == MatchesQueryText(L"c:\\users\\src\\foo", L"src", false), "pathmatch: separator-free needle == MatchesQueryText");
    CHECK(MatchesPathQuery(L"the quick brown fox", L"", false), "pathmatch: empty query matches");

    // --- MakeSnippet ---
    {
        const std::wstring text = L"prefix prefix prefix prefix prefix prefix THE-NEEDLE suffix\nsecond line";
        const auto s = MakeSnippet(text, L"the-needle", false, 60);
        CHECK(s.find(L"THE-NEEDLE") != std::wstring::npos, "snippet: contains the match");
        CHECK(s.find(L'\n') == std::wstring::npos, "snippet: single-line collapsed");
        CHECK(!s.empty() && s.front() == L'…', "snippet: left-context ellipsis when clipped");
    }

    // --- IsGuidToken / ParseSessionQuery: the term grammar ("quoted" exact + whole-guid id terms) ---
    {
        CHECK(IsGuidToken(L"dddddddd-dddd-4ddd-8ddd-dddddddddddd"), "guid: bare uuid accepted");
        CHECK(IsGuidToken(L"{DDDDDDDD-DDDD-4DDD-8DDD-DDDDDDDDDDDD}"), "guid: braced uppercase accepted");
        CHECK(!IsGuidToken(L"dddddddd-dddd-4ddd-8ddd-ddddddddddd"), "guid: 35 chars rejected");
        CHECK(!IsGuidToken(L"ddddddddxdddd-4ddd-8ddd-dddddddddddd"), "guid: misplaced dash rejected");
        CHECK(!IsGuidToken(L"gddddddd-dddd-4ddd-8ddd-dddddddddddd"), "guid: non-hex rejected");
        CHECK(!IsGuidToken(L""), "guid: empty rejected");

        auto t = ParseSessionQuery(L"  foo   bar ");
        CHECK(t.size() == 2 && t[0].text == L"foo" && t[1].text == L"bar" && !t[0].exact && !t[0].isGuid, "parse: whitespace-split plain terms");
        t = ParseSessionQuery(L"\"foo bar\" baz");
        CHECK(t.size() == 2 && t[0].text == L"foo bar" && t[0].exact && t[1].text == L"baz" && !t[1].exact, "parse: quoted phrase is ONE exact term");
        t = ParseSessionQuery(L"\"unterminated tail");
        CHECK(t.size() == 1 && t[0].text == L"unterminated tail" && t[0].exact, "parse: unterminated quote runs to the end");
        CHECK(ParseSessionQuery(L"\"\"").empty(), "parse: empty quotes drop (no terms)");
        CHECK(ParseSessionQuery(L"   ").empty(), "parse: whitespace-only -> no terms");
        t = ParseSessionQuery(L"{dddddddd-dddd-4ddd-8ddd-dddddddddddd}");
        CHECK(t.size() == 1 && t[0].isGuid && t[0].text == L"dddddddd-dddd-4ddd-8ddd-dddddddddddd", "parse: braced guid term, braces stripped");
        t = ParseSessionQuery(L"\"dddddddd-dddd-4ddd-8ddd-dddddddddddd\"");
        CHECK(t.size() == 1 && !t[0].isGuid && t[0].exact, "parse: QUOTED guid is a pure text term");
        t = ParseSessionQuery(L"DDDDDDDD-DDDD-4DDD-8DDD-DDDDDDDDDDDD");
        CHECK(t.size() == 1 && t[0].isGuid && t[0].textLower == L"dddddddd-dddd-4ddd-8ddd-dddddddddddd", "parse: guid term pre-folded for the id compare");

        const SearchTerm plain{ L"a", L"a", false, false };
        const SearchTerm exact{ L"a", L"a", true, false };
        const SearchTerm guid{ L"a", L"a", false, true };
        CHECK(TermIsFuzzy(plain, true) && !TermIsFuzzy(exact, true) && !TermIsFuzzy(guid, true) && !TermIsFuzzy(plain, false), "terms: (F) applies to plain terms only");
    }

    // --- SearchIndexFast: toggle semantics over canned entries ---
    {
        std::vector<SessionIndexEntry> entries(3);
        entries[0].sessionId = L"s-title";
        entries[0].stats.customTitle = L"Fix the BUILD pipeline";
        entries[0].stats.cwd = L"K:\\source\\alpha";
        entries[1].sessionId = L"s-dir";
        entries[1].stats.firstUserPrompt = L"unrelated";
        entries[1].stats.cwd = L"K:\\source\\BravoProj";
        entries[1].stats.pathsAccessed = { L"K:\\source\\BravoProj\\src\\widget.cpp" };
        entries[2].sessionId = L"s-file";
        entries[2].stats.cwd = L"K:\\elsewhere";
        entries[2].stats.pathsAccessed = { L"C:\\temp\\notes\\Findings.md" };

        SessionQuery q;
        q.text = L"build";
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-title", "fast: title matches (case-insensitive), both scopes off");

        q.text = L"bravoproj";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-dir", "fast: cwd matches in the baseline");

        q.text = L"findings.md";
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: an accessed FILE does not match without the file scope");
        q.scopeFiles = true;
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-file", "fast: file scope matches the path LEAF");

        q = {};
        q.text = L"temp\\notes";
        q.scopeDirs = true;
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-file", "fast: dir scope matches the path's DIRECTORY part");
        q.scopeDirs = false;
        q.scopeFiles = true;
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: dir text does not match the file scope's leaf");

        q = {};
        q.text = L"";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 3, "fast: empty text lists everything (window-only)");

        q.text = L"fbp";
        q.fuzzy = true;
        r = SearchIndexFast(entries, q);
        CHECK(!r.empty() && r[0] == L"s-title", "fast: fuzzy subsequence over the title");
    }

    // --- SearchIndexFast: directory matching is slash-insensitive (cwd + 📁), both directions ---
    {
        std::vector<SessionIndexEntry> entries(2);
        entries[0].sessionId = L"s-win"; // backslash-stored cwd (the Windows norm)
        entries[0].stats.cwd = L"K:\\source\\BravoProj";
        entries[0].stats.pathsAccessed = { L"K:\\source\\BravoProj\\src\\widget.cpp" };
        entries[1].sessionId = L"s-posix"; // forward-slash-stored cwd (a transcript can carry either)
        entries[1].stats.cwd = L"/home/user/AlphaProj";
        entries[1].stats.pathsAccessed = { L"/home/user/AlphaProj/lib/core.ts" };

        SessionQuery q;
        q.text = L"source/bravoproj"; // forward-slash query against the backslash cwd
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-win", "fast: forward-slash cwd query matches the backslash-stored cwd");

        q.text = L"home\\user\\alphaproj"; // backslash query against the forward-slash cwd
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-posix", "fast: backslash cwd query matches the forward-slash-stored cwd");

        q = {};
        q.scopeDirs = true;
        q.text = L"bravoproj/src"; // forward-slash 📁 query against the backslash path's dir part
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-win", "fast: 📁 dir scope is slash-insensitive (forward query, backslash path)");

        q = {};
        q.scopeDirs = true;
        q.text = L"alphaproj\\lib"; // backslash 📁 query against the forward-slash path's dir part
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-posix", "fast: 📁 dir scope is slash-insensitive (backslash query, forward path)");

        q = {};
        q.text = L"source/zeta"; // a genuinely-absent path term still misses (no false positives)
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: slash-insensitivity does not loosen a non-substring miss");
    }

    // --- SearchIndexFast: the 🏷 title scope gates title matching; liveTitle overlay is searchable ---
    {
        std::vector<SessionIndexEntry> entries(2);
        entries[0].sessionId = L"s-titled";
        entries[0].stats.customTitle = L"Refactor the SCHEDULER";
        entries[0].stats.cwd = L"K:\\source\\gamma";
        entries[1].sessionId = L"s-live";
        entries[1].stats.firstUserPrompt = L"do a thing";
        entries[1].stats.cwd = L"K:\\source\\delta";
        entries[1].liveTitle = L"My Renamed Tab"; // an OPEN session's real tab title (UI overlay)

        SessionQuery q; // scopeTitle defaults ON
        q.text = L"scheduler";
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-titled", "fast: title matches with scopeTitle ON (default)");
        q.scopeTitle = false;
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: scopeTitle OFF excludes a title-only match");

        q = {}; // DMI restores scopeTitle = true
        q.text = L"gamma"; // the cwd — must match even with titles off (cwd is the always-on baseline)
        q.scopeTitle = false;
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-titled", "fast: cwd stays matched with scopeTitle OFF");

        q = {};
        q.text = L"renamed tab"; // present only in the liveTitle overlay
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-live", "fast: liveTitle (open tab name) is searchable under scopeTitle");
        q.scopeTitle = false;
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: liveTitle is gated by scopeTitle too");
    }

    // --- SearchIndexFast: guid -> session-identity terms, quoted-exact, AND semantics ---
    {
        const std::wstring gidA = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring gidB = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        std::vector<SessionIndexEntry> entries(3);
        entries[0].sessionId = gidA;
        entries[0].stats.customTitle = L"Fix the BUILD pipeline";
        entries[0].stats.cwd = L"K:\\source\\alpha";
        entries[1].sessionId = gidB;
        entries[1].stats.customTitle = L"fork child";
        entries[1].stats.forkedFromId = gidA;
        entries[1].stats.cwd = L"K:\\source\\alpha";
        entries[2].sessionId = L"cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        entries[2].stats.customTitle = L"unrelated";
        entries[2].stats.cwd = L"K:\\elsewhere";

        SessionQuery q;
        q.text = gidA;
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 2 && r[0] == gidA && r[1] == gidB, "fast: whole-guid query matches the session id AND its fork");

        q.text = L"{AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA}";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 2 && r[0] == gidA, "fast: guid id match is case-insensitive, braces tolerated");

        q.text = gidA + L" build";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: guid + word AND — the fork lacks the word");

        q.text = L"build alpha";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: AND terms may hit DIFFERENT fields (title + cwd)");

        q.text = L"\"the build\"";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: quoted phrase exact-matches across a space");
        q.text = L"\"build the\"";
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: quoted phrase is order-exact (no token shuffle)");

        q = {};
        q.fuzzy = true;
        q.text = L"fxbld";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: plain term fuzzy-matches under (F)");
        q.text = L"\"fxbld\"";
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: quotes suppress (F) for that term");
    }

    // --- temp-root fixtures: history accelerator + presence + slow phase + index sidecar ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_search_test_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ root }, ec);

        // history.jsonl: two sids, one ancient line without a sessionId.
        const std::wstring hist = root + L"\\history.jsonl";
        MakeJsonl(hist,
                  "{\"display\":\"please fix the flaky test\",\"project\":\"K:\\\\a\",\"sessionId\":\"sid-1\",\"timestamp\":1}\n"
                  "{\"display\":\"deploy the build\",\"project\":\"K:\\\\a\",\"sessionId\":\"sid-2\",\"timestamp\":2}\n"
                  "{\"display\":\"fix it again (flaky)\",\"project\":\"K:\\\\a\",\"sessionId\":\"sid-1\",\"timestamp\":3}\n"
                  "{\"display\":\"ancient flaky line, no sid\",\"timestamp\":4}\n",
                  1000, 1000);
        SessionQuery q;
        q.text = L"FLAKY";
        q.scopeUser = true;
        const auto hh = SearchHistoryPrompts(hist, q, 3);
        CHECK(hh.size() == 1 && hh.count(L"sid-1") == 1, "history: per-sid aggregation, case-insensitive, no-sid lines skipped");
        CHECK(hh.count(L"sid-1") && hh.at(L"sid-1").hitCount == 2 && hh.at(L"sid-1").snippets.size() == 2, "history: hit count + snippets");

        // history2.jsonl: guid-sid lines — a guid term scopes hits to that session; "" = exact.
        const std::wstring gid1 = L"11111111-1111-4111-8111-111111111111";
        const std::wstring gid2 = L"22222222-2222-4222-8222-222222222222";
        const std::wstring hist2 = root + L"\\history2.jsonl";
        MakeJsonl(hist2,
                  "{\"display\":\"refactor the parser module\",\"sessionId\":\"11111111-1111-4111-8111-111111111111\",\"timestamp\":1}\n"
                  "{\"display\":\"refactor the lexer\",\"sessionId\":\"22222222-2222-4222-8222-222222222222\",\"timestamp\":2}\n",
                  1000, 1000);
        SessionQuery hq;
        hq.scopeUser = true;
        hq.text = gid1 + L" refactor";
        auto hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.size() == 1 && hg.count(gid1) == 1 && hg.at(gid1).hitCount == 1, "history: a guid term scopes hits to that session");
        hq.text = gid1;
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.empty(), "history: guid-only query -> no content hits (identity is the fast phase's job)");
        hq.text = L"\"the parser\"";
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.size() == 1 && hg.count(gid1) == 1, "history: quoted phrase exact-matches");
        hq.text = L"\"parser the\"";
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.empty(), "history: quoted phrase is order-exact");
        hq.text = L"refactor " + gid2;
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.size() == 1 && hg.count(gid2) == 1, "history: guid term position-independent (word + guid)");

        // presence: one row per file; bad/empty files tolerated.
        const std::wstring presDir = root + L"\\sessions";
        std::filesystem::create_directories(std::filesystem::path{ presDir }, ec);
        MakeJsonl(presDir + L"\\123.json", "{\"pid\":123,\"sessionId\":\"sid-1\",\"cwd\":\"K:\\\\a\",\"status\":\"busy\",\"version\":\"2.1.170\",\"startedAt\":111,\"updatedAt\":222}", 1, 1);
        MakeJsonl(presDir + L"\\999.json", "not json", 1, 1);
        const auto pres = ReadSessionPresenceIn(presDir);
        CHECK(pres.size() == 1 && pres[0].pid == 123 && pres[0].sessionId == L"sid-1" && pres[0].status == L"busy" && pres[0].updatedAtMs == 222, "presence: parsed row; garbage tolerated");

        // slow phase over a transcript (in-process fallback semantics; rg, when present, only
        // pre-filters files — same results either way since this file DOES match).
        const std::wstring projDir = root + L"\\projects\\K--a";
        std::filesystem::create_directories(std::filesystem::path{ projDir }, ec);
        const std::wstring sidT = L"dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        MakeJsonl(projDir + L"\\" + sidT + L".jsonl",
                  "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"message\":{\"content\":\"the magic word is xyzzy\"}}\n"
                  "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"plugh and xyzzy acknowledged\"}]}}\n",
                  2000, 2000);
        TranscriptRef ref;
        ref.sessionId = sidT;
        ref.path = projDir + L"\\" + sidT + L".jsonl";
        ref.sizeBytes = 0;
        ref.mtimeMs = 2000;
        ref.birthMs = 2000;

        SessionQuery sq;
        sq.text = L"xyzzy";
        sq.scopeUser = true;
        auto hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: user scope hits ONLY the prompt line");
        sq.scopeAgent = true;
        hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 2, "slow: both scopes hit prompt + assistant");
        sq.scopeUser = false;
        sq.text = L"plugh";
        hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1 && !hits[0].snippets.empty(), "slow: agent-only scope with snippet");
        sq.scopeAgent = false;
        hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.empty(), "slow: no message scope -> no content search");
        const auto cancelledImmediately = []() { return true; };
        sq.scopeAgent = true;
        hits = SearchTranscriptsSlow({ ref }, sq, 4, cancelledImmediately);
        CHECK(hits.empty(), "slow: cancellation respected");

        // slow phase, term grammar: guid = session-identity scope; "" = exact; per-MESSAGE AND.
        SessionQuery gq;
        gq.scopeUser = true;
        gq.text = sidT + L" xyzzy";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: guid+word — the file's own id satisfies the guid term");
        gq.text = L"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee xyzzy";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: a DIFFERENT guid excludes the session (id mismatch, not in text)");
        gq.text = sidT;
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: guid-only query -> no content scan (identity is the fast phase's job)");
        gq.text = L"\"magic word\"";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: quoted phrase exact-matches the prompt");
        gq.fuzzy = true;
        gq.text = L"\"mgc wrd\"";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: quotes suppress (F) inside the phrase");
        gq.text = L"mgc wrd";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: the same terms unquoted DO fuzzy-match");
        gq.fuzzy = false;
        gq.scopeAgent = true;
        gq.text = L"magic plugh";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: AND is per-MESSAGE — terms split across two messages don't hit");
        gq.text = L"plugh xyzzy";
        gq.scopeUser = false;
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: both terms in ONE assistant message hit");

        // index sidecar: refresh -> hit -> incremental refresh.
        const std::wstring idxDir = root + L"\\sessions-index";
        std::filesystem::create_directories(std::filesystem::path{ idxDir }, ec);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        ::GetFileAttributesExW(ref.path.c_str(), GetFileExInfoStandard, &fad);
        ref.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

        auto e1 = LoadOrRefreshSessionIndexIn(idxDir, ref);
        CHECK(e1.valid && e1.stats.userPrompts == 1 && e1.stats.assistantLines == 1, "index: first refresh builds stats");
        CHECK(std::filesystem::exists(std::filesystem::path{ idxDir + L"\\" + sidT + L".json" }), "index: sidecar written");
        auto e2 = LoadOrRefreshSessionIndexIn(idxDir, ref);
        CHECK(e2.valid && e2.stats.userPrompts == 1 && e2.stats.parsedBytes == e1.stats.parsedBytes, "index: (size,mtime) hit loads the sidecar only");
        {
            const std::string more = "{\"type\":\"user\",\"timestamp\":\"2026-06-01T11:00:00Z\",\"message\":{\"content\":\"second prompt\"}}\n";
            const HANDLE h = ::CreateFileW(ref.path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0;
            ::WriteFile(h, more.data(), static_cast<DWORD>(more.size()), &w, nullptr);
            ::CloseHandle(h);
            ::GetFileAttributesExW(ref.path.c_str(), GetFileExInfoStandard, &fad);
            ref.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            ref.mtimeMs = 3000; // changed key
            auto e3 = LoadOrRefreshSessionIndexIn(idxDir, ref);
            CHECK(e3.valid && e3.stats.userPrompts == 2, "index: stale key resumes the accumulate incrementally");
        }

        // contextTokens: parsed from message.usage (newest assistant wins), round-trips the sidecar,
        // and an OLD sidecar (written before the ctxTokens key existed) is one-time-backfilled on load.
        {
            const std::wstring sidC = L"ffffffff-ffff-4fff-8fff-ffffffffffff";
            const std::string sessC =
                "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"message\":{\"content\":\"go\"}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"a\"}],\"usage\":{\"input_tokens\":50,\"cache_creation_input_tokens\":10,\"cache_read_input_tokens\":500,\"output_tokens\":10}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:20Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"b\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":100,\"cache_creation_input_tokens\":20,\"cache_read_input_tokens\":2000,\"output_tokens\":30}}}\n";
            const std::wstring pathC = projDir + L"\\" + sidC + L".jsonl";
            MakeJsonl(pathC, sessC, 5000, 5000);
            TranscriptRef rc;
            rc.sessionId = sidC;
            rc.path = pathC;
            ::GetFileAttributesExW(pathC.c_str(), GetFileExInfoStandard, &fad);
            rc.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            rc.mtimeMs = 5000;
            rc.birthMs = 5000;
            auto ecx1 = LoadOrRefreshSessionIndexIn(idxDir, rc);
            CHECK(ecx1.valid && ecx1.stats.contextTokens == 2150, "index: contextTokens = newest assistant usage (2150), persisted to the sidecar");
            auto ecx2 = LoadOrRefreshSessionIndexIn(idxDir, rc);
            CHECK(ecx2.valid && ecx2.stats.contextTokens == 2150, "index: contextTokens round-trips a (size,mtime) cache hit");

            // Overwrite the sidecar with an OLD-format one (no ctxTokens key) but a matching
            // (size,mtime) cache key + assistantLines>0 -> the next load BACKFILLS from the transcript
            // instead of trusting the hit with 0 context.
            auto o = json::Value::MkObj();
            o.Set(L"sid", json::Value::MkStr(sidC));
            o.Set(L"size", json::Value::MkNum(static_cast<double>(rc.sizeBytes)));
            o.Set(L"mtime", json::Value::MkNum(5000.0));
            o.Set(L"parsedBytes", json::Value::MkNum(static_cast<double>(sessC.size())));
            o.Set(L"assistantLines", json::Value::MkNum(2));
            const std::wstring dumped = json::Dump(o); // ASCII content -> narrows to valid UTF-8
            MakeJsonl(idxDir + L"\\" + sidC + L".json", std::string(dumped.begin(), dumped.end()), 5000, 5000);
            auto ecx3 = LoadOrRefreshSessionIndexIn(idxDir, rc);
            CHECK(ecx3.valid && ecx3.stats.contextTokens == 2150, "index: an old (no-ctxTokens) sidecar is backfilled from the transcript on load");
        }

        // pathsAccessed flow into the index + the file/dir scopes end-to-end.
        {
            const std::wstring sidP = L"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
            MakeJsonl(projDir + L"\\" + sidP + L".jsonl",
                      "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"Read\",\"input\":{\"file_path\":\"K:\\\\proj\\\\deep\\\\Widget.xaml\"}}]}}\n",
                      4000, 4000);
            TranscriptRef rp;
            rp.sessionId = sidP;
            rp.path = projDir + L"\\" + sidP + L".jsonl";
            ::GetFileAttributesExW(rp.path.c_str(), GetFileExInfoStandard, &fad);
            rp.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            rp.mtimeMs = 4000;
            const auto ep = LoadOrRefreshSessionIndexIn(idxDir, rp);
            CHECK(ep.valid && ep.stats.pathsAccessed.size() == 1 && ep.stats.pathsAccessed[0] == L"K:\\proj\\deep\\Widget.xaml", "index: tool-touched path captured");
            SessionQuery fq;
            fq.text = L"widget.xaml";
            fq.scopeFiles = true;
            const auto fr = SearchIndexFast({ ep }, fq);
            CHECK(fr.size() == 1, "index->fast: file scope finds the accessed file");
        }

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- rg integration smoke (only when ripgrep is on PATH) ---
    if (!ResolveRipgrep().empty())
    {
        std::wprintf(L"  [info] rg resolved: %ls\n", ResolveRipgrep().c_str());
        CHECK(ResolveRipgrep().find(L"rg") != std::wstring::npos, "rg: resolved path names rg");
    }
    else
    {
        std::wprintf(L"  [info] rg not on PATH — in-process fallback paths exercised above\n");
    }
}

// --- StripSummaryTableRules: COLLAPSE a one-line message's embedded tables (drop rules + de-frame) ---
// When a summary message is flattened to one line (wrap-off panel / the always-one-line Sessions
// detail), an embedded table is pure noise. The collapser (1) DROPS box-drawing "├──┼──┤" / markdown
// "|---|---|" rule rows, and (2) DE-FRAMES data rows: "│ Name │ Age │" -> "Name · Age" (strip the
// │/| bars + padding, rejoin cells with " · " = U+00B7, drop empty cells). Box-drawing chars + the dot
// are written as \x escapes / a built separator so the test is source-encoding independent.
static void TestSummaryTableTrim()
{
    std::wprintf(L"[TestSummaryTableTrim]\n");
    std::wstring D = L" "; D += static_cast<wchar_t>(0x00B7); D += L" "; // " · " de-framed cell separator

    // 1) Box-drawing table: the top/sep/bottom rules (┌──┬──┐ / ├──┼──┤ / └──┴──┘) drop; the two data
    //    rows de-frame to "Name · Age" / "Bob · 30", joined by '\n'.
    {
        const std::wstring in =
            L"\x250C\x2500\x2500\x252C\x2500\x2500\x2510\n" // ┌──┬──┐
            L"\x2502 Name \x2502 Age \x2502\n" // │ Name │ Age │
            L"\x251C\x2500\x2500\x253C\x2500\x2500\x2524\n" // ├──┼──┤
            L"\x2502 Bob \x2502 30 \x2502\n" // │ Bob │ 30 │
            L"\x2514\x2500\x2500\x2534\x2500\x2500\x2518"; // └──┴──┘
        const std::wstring want = L"Name" + D + L"Age\nBob" + D + L"30";
        CHECK(StripSummaryTableRules(in) == want, "box-drawing table: rule rows dropped, data rows de-framed to cell · cell");
    }

    // 2) The user's actual long box-drawing separator row, between two data rows -> rule drops, data de-frames.
    {
        const std::wstring rule =
            L"\x251C\x2500\x2500\x2500\x2500\x2500\x253C\x2500\x2500\x2500\x2500\x253C\x2500\x2500\x2500\x2524"; // ├────┼───┼──┤
        const std::wstring in = L"\x2502 a \x2502 b \x2502 c \x2502\n" + rule + L"\n\x2502 1 \x2502 2 \x2502 3 \x2502";
        const std::wstring want = L"a" + D + L"b" + D + L"c\n1" + D + L"2" + D + L"3";
        CHECK(StripSummaryTableRules(in) == want, "long ├────┼────┤ rule row dropped; flanking rows de-framed");
        CHECK(StripSummaryTableRules(rule) == rule, "a lone all-rule message is returned UNCHANGED (never blanked)");
    }

    // 3) Markdown table: the |---|---| separator drops; header + data de-frame.
    {
        const std::wstring in = L"| Name | Age |\n|------|-----|\n| Bob | 30 |";
        const std::wstring want = L"Name" + D + L"Age\nBob" + D + L"30";
        CHECK(StripSummaryTableRules(in) == want, "markdown table: |---| separator dropped, header+data de-framed");
        CHECK(StripSummaryTableRules(L"|:---|:---:|---:|") == L"|:---|:---:|---:|",
              "a markdown alignment rule alone -> unchanged (all-rules guard)");
    }

    // 4) Data rows de-frame to their CELL content (the bars are UI, the cells are the data).
    {
        CHECK(StripSummaryTableRules(L"\x2502 :) \x2502 :( \x2502") == L":)" + D + L":(",
              "a box row with punctuation cells de-frames to ':) · :('");
        CHECK(StripSummaryTableRules(L"| -1 | +2 |") == L"-1" + D + L"+2",
              "a markdown data row of signed numbers de-frames (digits kept)");
        CHECK(StripSummaryTableRules(L"| - | + |") == L"-" + D + L"+",
              "a markdown row of single -/+ cells de-frames (it is bar-framed)");
    }

    // 5) A fully-EMPTY box row (│   │) is a contentless rule -> dropped; flanking rows de-frame.
    {
        const std::wstring in = L"\x2502 a \x2502\n\x2502   \x2502\n\x2502 b \x2502";
        CHECK(StripSummaryTableRules(in) == L"a\nb", "an empty │   │ row drops; 'a' / 'b' single-cell rows de-frame");
    }

    // 5b) De-frame mechanics: any │-bearing row (even border-less), single-cell, and internal empty cells.
    {
        CHECK(StripSummaryTableRules(L"Name \x2502 Age") == L"Name" + D + L"Age", "a border-less box row (│ present) de-frames");
        CHECK(StripSummaryTableRules(L"\x2502 MENU \x2502") == L"MENU", "a single-cell box row de-frames to just the cell");
        CHECK(StripSummaryTableRules(L"\x2502 a \x2502   \x2502 b \x2502") == L"a" + D + L"b", "an internal empty cell is dropped");
    }

    // 5c) SAFETY: a stray prose/code pipe is NOT a table row (markdown needs a leading AND trailing bar).
    {
        CHECK(StripSummaryTableRules(L"run foo | grep bar") == L"run foo | grep bar", "an un-framed '|' (shell pipe) is left verbatim");
        CHECK(StripSummaryTableRules(L"| head -5") == L"| head -5", "a leading-only '|' is not a framed row -> verbatim");
        CHECK(StripSummaryTableRules(L"a | b | c") == L"a | b | c", "a border-less '|' row is left verbatim (ambiguous with prose)");
    }

    // 6) ASCII thematic break (>=3 of the fill set -/=/~/#/*/_) drops; a short 2-char run does NOT.
    //    The fill set was widened to # * _ after a 1.4 GB corpus scan turned up bare "######" rule
    //    lines (the only separator shape the original -/=/~ set missed).
    {
        CHECK(StripSummaryTableRules(L"intro\n---\noutro") == L"intro\noutro", "a --- thematic break row drops");
        CHECK(StripSummaryTableRules(L"intro\n--\noutro") == L"intro\n--\noutro", "a 2-dash line is kept (run < 3, no box char)");
        CHECK(StripSummaryTableRules(L"a\n====\nb") == L"a\nb", "a ==== underline rule drops");
        CHECK(StripSummaryTableRules(L"a\n######\nb") == L"a\nb", "a ###### rule line drops (corpus: the real miss)");
        CHECK(StripSummaryTableRules(L"a\n###\nb") == L"a\nb", "a bare ### (run==3) drops");
        CHECK(StripSummaryTableRules(L"a\n***\nb") == L"a\nb", "a *** thematic break drops");
        CHECK(StripSummaryTableRules(L"a\n___\nb") == L"a\nb", "a ___ thematic break drops");
    }

    // 6b) The all-structural GUARD protects real content built from the same fill chars: a markdown
    //     heading, a bullet item, an emphasis marker, a titled separator (corpus had 80 of these), and
    //     a "::: fence" all carry letters / a sub-3 run, so they are KEPT verbatim.
    {
        CHECK(StripSummaryTableRules(L"### Heading") == L"### Heading", "### Heading kept (has letters)");
        CHECK(StripSummaryTableRules(L"* item") == L"* item", "a '* item' bullet kept (has letters)");
        CHECK(StripSummaryTableRules(L"**") == L"**", "a lone ** kept (run < 3, no box)");
        CHECK(StripSummaryTableRules(L"=== first GET ===") == L"=== first GET ===", "a titled separator keeps its label");
        CHECK(StripSummaryTableRules(L":::") == L":::", "a ::: fence kept (colon is glue, not fill -> no run)");
        CHECK(StripSummaryTableRules(L"1K \x2588\x2588\x2588 1.96x") == L"1K \x2588\x2588\x2588 1.96x",
              "a bar-chart data row (block chars + numbers) is kept as data");
    }

    // 7) No table at all -> returned unchanged (cheap no-op path).
    {
        const std::wstring in = L"just some prose\nwith two lines";
        CHECK(StripSummaryTableRules(in) == in, "no rule rows -> message unchanged");
        CHECK(StripSummaryTableRules(L"") == L"", "empty message -> empty");
        CHECK(StripSummaryTableRules(L"single line") == L"single line", "single non-rule line -> unchanged");
    }

    // 8) CRLF on kept lines is normalized to LF; the rule row (with its CR) still drops; data de-frames.
    {
        const std::wstring in = L"| a | b |\r\n|---|---|\r\n| 1 | 2 |";
        const std::wstring want = L"a" + D + L"b\n1" + D + L"2";
        CHECK(StripSummaryTableRules(in) == want, "CRLF data rows de-frame (-> LF), the |---| rule (with CR) dropped");
    }
}

// ---- Summary-panel JUMP resolver (PromptAnchor.h / SUMMARY_JUMP.md) ----

static bool SpanHolds(const std::wstring& hay, const Agentmaster::AnchorMatch& m, const std::wstring& wantSub)
{
    if (!m.found || m.offset + m.length > hay.size())
    {
        return false;
    }
    const auto slice = hay.substr(m.offset, m.length);
    return Agentmaster::NormalizeForMatch(slice).find(Agentmaster::NormalizeForMatch(wantSub)) != std::wstring::npos;
}

static void TestPromptAnchor()
{
    using namespace Agentmaster;
    std::wprintf(L"Summary-panel jump resolver (SUMMARY_JUMP.md):\n");

    // Normalize: ASCII-lower, collapse whitespace runs, trim.
    CHECK(NormalizeForMatch(L"  Fix   The\tBuild \n") == L"fix the build", "normalize: lower+collapse+trim");
    CHECK(NormalizeForMatch(L"\n\n  ") == L"", "normalize: all-whitespace -> empty");

    // Needle: first non-trivial line, capped to maxLen.
    CHECK(PickAnchorNeedle(L"\n\n  Run the tests\nsecond line", 64) == L"run the tests", "needle: first non-trivial line");
    CHECK(PickAnchorNeedle(L"abcdefghij", 4) == L"abcd", "needle: capped to maxLen");
    CHECK(PickAnchorNeedle(L"   \n  ", 64) == L"", "needle: empty when no content");

    // Basic in-order resolve over a synthetic scrollback ('>' prompt + assistant filler).
    {
        const std::wstring hay = L"> fix the build\nassistant: working on it\n> run the tests\nassistant: done\n";
        auto r = ResolvePromptAnchors(hay, { L"fix the build", L"run the tests" });
        CHECK(r.size() == 2 && r[0].found && r[1].found, "resolve: both prompts found");
        CHECK(r[0].offset < r[1].offset, "resolve: in buffer order");
        CHECK(!r[0].partial && r[0].quality > 0.99, "resolve: full match quality ~1");
        CHECK(SpanHolds(hay, r[0], L"fix the build") && SpanHolds(hay, r[1], L"run the tests"), "resolve: spans land on the text");
        CHECK(!r[0].outOfOrder && !r[1].outOfOrder, "resolve: in-order, not flagged");
    }

    // Whitespace-tolerant: a re-indent / odd transcript whitespace still matches the rendered line
    // (this is also what makes a SOFT-WRAPPED prompt — continuous in the haystack — resolve).
    {
        const std::wstring hay = L"> fix the build now\n";
        auto m = ResolveOnePromptAnchor(hay, L"fix   the\tbuild now", 0);
        CHECK(m.found && m.quality > 0.99, "resolve: whitespace differences tolerated");
    }

    // Render prefix ("> ") is skipped naturally (substring search).
    {
        const std::wstring hay = L"  > implement the centering primitive\n";
        auto m = ResolveOnePromptAnchor(hay, L"implement the centering primitive", 0);
        CHECK(m.found && !m.partial, "resolve: '> ' render prefix skipped");
    }

    // Duplicates: the i-th send maps to the i-th surviving occurrence (order-preserving greedy).
    {
        const std::wstring hay = L"> deploy now\nout1\n> deploy now\nout2\n";
        auto r = ResolvePromptAnchors(hay, { L"deploy now", L"deploy now" });
        CHECK(r[0].found && r[1].found && r[0].offset < r[1].offset, "duplicates: first then second occurrence");
        CHECK(!r[0].outOfOrder && !r[1].outOfOrder, "duplicates: both in order");
    }

    // Partial: a truncated/reflowed render resolves at quality < 1 and is flagged partial.
    {
        const std::wstring full = L"please refactor the entire authentication subsystem to use tokens";
        const std::wstring hay = L"> please refactor the entire authentication subsy\nok\n"; // render cut short
        auto m = ResolveOnePromptAnchor(hay, full, 0);
        CHECK(m.found && m.partial && m.quality < 1.0 && m.quality > 0.5, "partial: truncated render -> partial match");
    }

    // Not found: a prompt not on screen resolves to nothing.
    {
        const std::wstring hay = L"> something else entirely\n";
        auto m = ResolveOnePromptAnchor(hay, L"this prompt is absent from the buffer xyzzy", 0);
        CHECK(!m.found, "not found: absent prompt -> no match");
    }

    // Out-of-order fallback: no in-order candidate -> last global occurrence, flagged outOfOrder.
    {
        const std::wstring hay = L"> beta task here\n> alpha task here\n";
        auto r = ResolvePromptAnchors(hay, { L"alpha task here", L"beta task here" });
        CHECK(r[0].found, "outOfOrder: first prompt still found");
        CHECK(r[1].found && r[1].outOfOrder, "outOfOrder: second prompt falls back, flagged");
    }

    // RTL (Hebrew/Arabic): a terminal renders an RTL line CHARACTER-REVERSED (visual order) while the
    // transcript stores it logical, so the prompt appears reversed in the buffer. The resolver must still
    // find it via the reversed orientation (the matched ROW is the same, so centering works). The fix is
    // gated on ContainsRtl so LTR matching is never perturbed. (SUMMARY_JUMP.md §5.)
    {
        // logical "write a program" in Hebrew, built from code points so the source stays pure ASCII
        // (cl without /utf-8 would mis-decode a raw Hebrew literal). U+05D0..U+05EA = Hebrew letters.
        std::wstring logical;
        for (const int c : { 0x05DB, 0x05EA, 0x05D5, 0x05D1, 0x0020, 0x05EA, 0x05D5, 0x05DB, 0x05E0, 0x05D9, 0x05EA })
        {
            logical.push_back(static_cast<wchar_t>(c));
        }
        const std::wstring visual(logical.rbegin(), logical.rend()); // what the terminal buffer holds
        CHECK(ResolveOnePromptAnchor(L"> " + visual + L"\n", logical, 0).found, "RTL: reversed-in-buffer Hebrew prompt resolves");
        CHECK(ResolveOnePromptAnchor(L"> " + logical + L"\n", logical, 0).found, "RTL: forward orientation still resolves");
        // An LTR prompt must NOT be matched by its own reversal (no RTL char => no reversed pass).
        CHECK(!ResolveOnePromptAnchor(L"> dlrow olleh now\n", L"hello world now is the prompt", 0).found, "LTR: reversed text is NOT spuriously matched");
        // Batch: a reversed-in-buffer RTL prompt resolves in the greedy pass too.
        auto rb = ResolvePromptAnchors(L"> " + visual + L"\nout\n", { logical });
        CHECK(rb.size() == 1 && rb[0].found, "RTL: batch resolve finds the reversed prompt");
    }

    // Cheap validate: true at the resolved offset, false at a bogus one / past end.
    {
        const std::wstring hay = L"> fix the build\nout\n";
        auto m = ResolveOnePromptAnchor(hay, L"fix the build", 0);
        CHECK(m.found && ValidatePromptAnchor(hay, L"fix the build", m.offset), "validate: true at resolved offset");
        CHECK(!ValidatePromptAnchor(hay, L"fix the build", m.offset + 8), "validate: false at a wrong offset");
        CHECK(!ValidatePromptAnchor(hay, L"fix the build", hay.size() + 100), "validate: false past end");
    }

    // Agentmaster (SUMMARY_JUMP.md §5): prompt-MARKER validation. Claude Code prefixes a SENT prompt's
    // rendered line with a marker glyph (U+276F); requiring a match to sit right after one binds it to the
    // real user-prompt render instead of an assistant ECHO of the same words. The glyph is built from its
    // code point so this TU stays pure-ASCII (it compiles without /utf-8); the marker SET passed to the
    // resolver is the shipping constant kClaudePromptMarkers (already \u-escaped in the header).
    {
        const std::wstring caret(1, static_cast<wchar_t>(0x276F)); // the heavy right-angle prompt ornament
        AnchorOptions mopts;
        mopts.promptMarkers = std::wstring{ kClaudePromptMarkers };

        // (1) An echo PRECEDES the real render. Legacy (no markers) binds to the earlier echo; marker
        //     validation binds to the caret-marked render instead. This is the core win.
        {
            const std::wstring hay = L"assistant: ill fix the bug now\n" + caret + L" fix the bug\nout\n";
            const size_t echoPos = hay.find(L"fix the bug");
            const size_t markedPos = hay.find(L"fix the bug", hay.find(caret));
            CHECK(echoPos != std::wstring::npos && markedPos != std::wstring::npos && echoPos < markedPos, "marker: fixture sane (echo before render)");

            auto noMark = ResolvePromptAnchors(hay, { L"fix the bug" });
            CHECK(noMark[0].found && noMark[0].offset == echoPos, "marker: legacy binds the earlier echo");

            auto withMark = ResolvePromptAnchors(hay, { L"fix the bug" }, mopts);
            CHECK(withMark[0].found && withMark[0].offset == markedPos, "marker: validation binds the caret-marked render, not the echo");
        }

        // (2) Soft fallback (regression-proofing): when markers ARE in use (present for another prompt) but
        //     a prompt's text appears ONLY unmarked (no caret render — its own scrolled off, or the marker
        //     glyph isn't on sent lines in this build), that prompt STILL resolves via a legacy fallback.
        //     Marker enforcement never makes a prompt that legacy would resolve disappear.
        {
            const std::wstring hay = caret + L" unrelated heading line\nassistant: please refactor the auth module now\n";
            auto r = ResolvePromptAnchors(hay, { L"unrelated heading line", L"refactor the auth module now" }, mopts);
            CHECK(r[0].found, "marker: the caret-marked prompt resolves");
            CHECK(r[1].found, "marker(soft fallback): an unmarked-only prompt still resolves (never regresses)");
        }

        // (3) Self-adapting: markers requested but NONE present (a '>'-rendering build) => enforcement
        //     auto-disables and falls back to legacy text matching (never regresses).
        {
            const std::wstring hay = L"> fix the build\nout\n"; // '>' is not a configured marker; no U+276F anywhere
            auto r = ResolvePromptAnchors(hay, { L"fix the build" }, mopts);
            CHECK(r[0].found, "marker: no marker in buffer => enforcement disabled, legacy match still resolves");
        }

        // (4) Duplicate disambiguation: two caret-marked sends with an assistant echo between them resolve
        //     to the TWO real renders, in order (the unmarked echo is skipped by the gate).
        {
            const std::wstring hay = caret + L" deploy now\nassistant: ok i will deploy now\n" + caret + L" deploy now\nout\n";
            const size_t firstMarked = hay.find(L"deploy now");
            const size_t lastMarked = hay.rfind(L"deploy now");
            auto r = ResolvePromptAnchors(hay, { L"deploy now", L"deploy now" }, mopts);
            CHECK(r[0].found && r[1].found && r[0].offset == firstMarked && r[1].offset == lastMarked, "marker: duplicates map to the two caret renders, echo skipped");
            CHECK(!r[0].outOfOrder && !r[1].outOfOrder, "marker: duplicates resolved in order");
        }

        // (5) Proximity, not just same-line: with TWO caret lines — one where the text appears FAR down a
        //     long marked line (beyond markerLookback), another where it starts right AT the marker — the
        //     match binds to the proximate (real-render) one, skipping the distant in-line occurrence. (A
        //     real prompt's needle is its first line and so starts right at the marker; an incidental
        //     occurrence lands mid-line, too far to validate.)
        {
            const std::wstring hay = caret + L" heading XXXXXXXXXXXXXXXXXXXXXX run the migration\n" + caret + L" run the migration\nout\n";
            const size_t proximate = hay.rfind(L"run the migration"); // second caret line: text right after the marker
            auto r = ResolvePromptAnchors(hay, { L"run the migration" }, mopts);
            CHECK(r[0].found && r[0].offset == proximate, "marker: binds the occurrence right after the marker, not a distant in-line one");
        }
    }
}

// Agentmaster (SUMMARY_JUMP.md §5): every found span must be in-bounds of the haystack it was resolved
// against — an OOB offset/length would AV when the adapter maps it back to a buffer row.
static bool AnchorSpansValid(const std::wstring& hay, const std::vector<Agentmaster::AnchorMatch>& r)
{
    for (const auto& m : r)
    {
        if (!m.found)
        {
            continue;
        }
        if (m.offset > hay.size() || m.offset + m.length > hay.size())
        {
            return false;
        }
    }
    return true;
}

// Agentmaster (SUMMARY_JUMP.md §5): edge-case + crash-safety fuzz for the prompt resolver with marker
// validation ON. The resolver feeds alt-nav + jump on the UI thread, so a pathological haystack/needle must
// never AV / read OOB / infinite-loop / throw -- a crash here takes the whole app. Each case asserts it
// RETURNS, the result size matches the prompt count, and every found span is in-bounds.
static void TestPromptAnchorEdgeCases()
{
    using namespace Agentmaster;
    std::wprintf(L"Prompt resolver edge cases + crash-safety (marker validation on):\n");
    const std::wstring caret(1, static_cast<wchar_t>(0x276F)); // the U+276F prompt ornament
    AnchorOptions mk;
    mk.promptMarkers = std::wstring{ kClaudePromptMarkers };

    // Empty haystack / empty prompt list / empty + whitespace-only prompts (slots preserved, not found).
    CHECK(ResolvePromptAnchors(L"", { L"hello world here" }, mk).size() == 1, "edge: empty haystack -> one not-found slot");
    CHECK(ResolvePromptAnchors(caret + L" hello world here\n", {}, mk).empty(), "edge: empty prompt list -> empty result");
    {
        const std::wstring hay = caret + L" hello world here\n";
        auto r = ResolvePromptAnchors(hay, { L"", L"   \t  ", L"hello world here" }, mk);
        CHECK(r.size() == 3 && !r[0].found && !r[1].found && r[2].found, "edge: empty/ws prompts keep their slot, not found");
        CHECK(AnchorSpansValid(hay, r), "edge: spans valid with empty/ws prompts");
    }

    // A marker-only haystack; a prompt that IS a marker glyph; a needle longer than the whole haystack.
    {
        const std::wstring hay = caret + caret + caret;
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { caret }, mk)), "edge: all-marker haystack + marker-glyph prompt");
    }
    {
        const std::wstring hay = caret + L" ab\n";
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { L"this needle is far longer than the whole tiny haystack xyz" }, mk)), "edge: needle longer than haystack");
    }

    // markerLookback extremes: 0 (no occurrence can ever be "marked" -> everything falls back to legacy) and
    // huge (the lookback window is clamped to the string start, never reads before index 0).
    {
        const std::wstring hay = caret + L" do the thing now\n";
        AnchorOptions z = mk;
        z.markerLookback = 0;
        auto r0 = ResolvePromptAnchors(hay, { L"do the thing now" }, z);
        CHECK(r0[0].found && AnchorSpansValid(hay, r0), "edge: lookback=0 -> soft fallback still resolves");
        AnchorOptions big = mk;
        big.markerLookback = 100000;
        auto rb = ResolvePromptAnchors(hay, { L"do the thing now" }, big);
        CHECK(rb[0].found && AnchorSpansValid(hay, rb), "edge: huge lookback does not read before the buffer start");
    }

    // Embedded NUL + a lone (unpaired) UTF-16 surrogate, in BOTH haystack and needle (wstring holds them) —
    // normalization + the marker scan must treat them as ordinary code units, never crash.
    {
        std::wstring hay = caret + L" abc";
        hay.push_back(L'\0');
        hay += L"def ghi jkl\n";
        std::wstring needle = L"abc";
        needle.push_back(L'\0');
        needle += L"def ghi jkl";
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { needle }, mk)), "edge: embedded NUL + control chars no crash");
    }
    {
        std::wstring hay = caret + L" pre ";
        hay.push_back(static_cast<wchar_t>(0xD800)); // lone high surrogate
        hay += L" post text here\n";
        std::wstring needle = L"pre ";
        needle.push_back(static_cast<wchar_t>(0xD800));
        needle += L" post text here";
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { needle }, mk)), "edge: lone UTF-16 surrogate no crash");
    }

    // Huge marker-only haystack + a huge needle (the FindAcceptable inner loop / normalization must stay
    // bounded and RETURN — a regression here would hang the UI thread, not crash, but is just as fatal).
    {
        const std::wstring hay(200000, static_cast<wchar_t>(0x276F)); // 200k markers, no text
        const std::wstring needle(50000, L'z');
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { needle }, mk)), "edge: 200k-marker haystack + 50k needle returns");
    }

    // Thousands of UNMARKED duplicate occurrences before ONE marked render: the marker scan must skip every
    // unmarked hit (the FindAcceptable forward loop) and terminate on the single marked one.
    {
        std::wstring hay;
        for (int i = 0; i < 2000; ++i)
        {
            hay += L"assistant ctx repeat token here\n"; // 2000 UNMARKED occurrences
        }
        hay += caret + L" repeat token here\n"; // exactly one MARKED render (the last occurrence)
        auto r = ResolvePromptAnchors(hay, { L"repeat token here" }, mk);
        CHECK(r[0].found && AnchorSpansValid(hay, r), "edge: 2000 unmarked + 1 marked -> resolves, terminates");
        CHECK(r[0].offset == hay.rfind(L"repeat token here"), "edge: skipped all unmarked, landed on the marked render");
    }

    // ValidatePromptAnchor crash-safety with marker opts passed (it ignores them, but must not choke).
    {
        const std::wstring hay = caret + L" validate me please\n";
        CHECK(!ValidatePromptAnchor(hay, L"validate me please", hay.size() + 999, mk), "edge: validate past end is safe");
        auto m = ResolveOnePromptAnchor(hay, L"validate me please", 0, mk);
        CHECK(m.found && ValidatePromptAnchor(hay, L"validate me please", m.offset, mk), "edge: validate at the resolved offset");
    }
}

// Agentmaster (SUMMARY_JUMP.md §5): exercise the resolver + marker validation against the REAL on-disk
// Claude session corpus. For each session we extract its real sent prompts (the SAME list the panel /
// alt-nav resolve) and synthesize a realistic rendered buffer -- each prompt as a MARKED `<U+276F> <prompt>`
// render preceded by an UNMARKED assistant echo of its first line -- then resolve with markers AND legacy.
// This stresses the marker code with real-world prompt strings (emoji, RTL, code, huge / multi-line prompts)
// and verifies the two invariants that must ALWAYS hold: (1) every found span is in-bounds (no OOB -> the
// adapter's offset->row map would AV otherwise), and (2) markers NEVER make a prompt that legacy resolved
// disappear (the soft-fallback guarantee). Disambiguation (markers steering off the echo onto the real
// render) is reported + asserted non-zero. Skips silently when no corpus is present (CI / other machines).
static void TestPromptAnchorRealCorpus()
{
    using namespace Agentmaster;
    std::wprintf(L"Prompt resolver over the REAL session corpus (markers; crash + never-regress + disambiguation):\n");
    const auto sessions = EnumerateTranscripts(NowMsTest() - 92LL * 24 * 3600 * 1000);
    if (sessions.empty())
    {
        std::wprintf(L"  [info] no live corpus -> skipped\n");
        return;
    }
    const std::wstring caret(1, static_cast<wchar_t>(0x276F));
    AnchorOptions mk;
    mk.promptMarkers = std::wstring{ kClaudePromptMarkers };

    size_t sessionsUsed = 0, totalPrompts = 0, foundMk = 0, foundLegacy = 0;
    size_t regressions = 0, oobSpans = 0, markedHit = 0, improvedByMarker = 0;
    const size_t kMaxSessions = 120, kMaxPromptsPerSession = 40, kReadCapBytes = 2u * 1024 * 1024;

    for (const auto& s : sessions)
    {
        if (sessionsUsed >= kMaxSessions)
        {
            break;
        }
        std::vector<std::wstring> prompts;
        try
        {
            prompts = AnalyzeSessionTranscript(s.path, kReadCapBytes).userMsgs;
        }
        catch (...)
        {
            continue; // a parse throw here is itself a finding, but the never-throw wrappers should prevent it
        }
        if (prompts.empty())
        {
            continue;
        }
        if (prompts.size() > kMaxPromptsPerSession)
        {
            prompts.resize(kMaxPromptsPerSession);
        }

        // Build a realistic rendered buffer; track each prompt's MARKED render + UNMARKED echo offsets.
        std::wstring hay;
        std::vector<size_t> markedPos(prompts.size(), std::wstring::npos);
        std::vector<size_t> echoPos(prompts.size(), std::wstring::npos);
        for (size_t i = 0; i < prompts.size(); ++i)
        {
            const auto needle = PickAnchorNeedle(prompts[i], mk.maxNeedle);
            if (needle.empty())
            {
                continue; // unbuildable (no non-trivial line) -> resolves not-found in BOTH (no regression)
            }
            hay += L"assistant ctx: "; // an UNMARKED echo of the first line (where legacy binds)
            echoPos[i] = hay.size();
            hay += needle;
            hay += L"\n";
            hay += caret; // the REAL marked render
            hay += L" ";
            markedPos[i] = hay.size();
            hay += prompts[i];
            hay += L"\n... assistant reply filler ...\n";
        }

        std::vector<AnchorMatch> rMk, rLeg;
        try
        {
            rMk = ResolvePromptAnchors(hay, prompts, mk);
            rLeg = ResolvePromptAnchors(hay, prompts); // legacy (no markers)
        }
        catch (...)
        {
            continue;
        }

        ++sessionsUsed;
        for (size_t i = 0; i < prompts.size(); ++i)
        {
            ++totalPrompts;
            if (rMk[i].found)
            {
                ++foundMk;
            }
            if (rLeg[i].found)
            {
                ++foundLegacy;
            }
            if (rMk[i].found && (rMk[i].offset > hay.size() || rMk[i].offset + rMk[i].length > hay.size()))
            {
                ++oobSpans;
            }
            if (rLeg[i].found && !rMk[i].found)
            {
                ++regressions; // markers made a legacy-found prompt vanish -> the soft fallback failed
            }
            if (markedPos[i] != std::wstring::npos && rMk[i].found && rMk[i].offset == markedPos[i])
            {
                ++markedHit;
                if (rLeg[i].found && echoPos[i] != std::wstring::npos && rLeg[i].offset == echoPos[i])
                {
                    ++improvedByMarker; // legacy bound the echo; markers bound the real render
                }
            }
        }
    }

    CHECK(oobSpans == 0, "real corpus: no out-of-bounds spans (offset+length <= haystack)");
    CHECK(regressions == 0, "real corpus: markers never make a legacy-found prompt vanish (soft fallback holds)");
    CHECK(sessionsUsed > 0, "real corpus: exercised at least one real session");
    CHECK(totalPrompts == 0 || markedHit > 0, "real corpus: at least one real prompt binds its marked render");
    std::wprintf(L"  [info] sessions=%zu prompts=%zu | found markers=%zu legacy=%zu | marked-hit=%zu improved-by-marker=%zu | oob=%zu regress=%zu\n",
                 sessionsUsed, totalPrompts, foundMk, foundLegacy, markedHit, improvedByMarker, oobSpans, regressions);
}

// Build a synthetic Claude scrollback: `rows` lines of filler with `prompts` "> <prompt>" lines
// evenly spaced. `present`==false makes each summary message carry an absent suffix (the pathological
// all-miss case: every needle forces a full backoff + global rfind scan).
static std::wstring BuildBenchHaystack(int rows, int prompts, std::vector<std::wstring>& outMsgs, bool present)
{
    std::wstring hay;
    hay.reserve(static_cast<size_t>(rows) * 64);
    outMsgs.clear();
    uint64_t lcg = 0x9E3779B97F4A7C15ull;
    auto rnd = [&]() { lcg = lcg * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(lcg >> 33); };
    const int step = (std::max)(1, rows / (std::max)(1, prompts));
    int made = 0;
    for (int i = 0; i < rows; ++i)
    {
        if (i % step == 0 && made < prompts)
        {
            const std::wstring p = L"benchmark prompt number " + std::to_wstring(made) + L" do the thing carefully and well";
            hay += L"> " + p + L"\n";
            // present: the exact text. miss: a prompt whose LEADING chars are absent (e.g. it scrolled
            // off the top) -> the true not-found path the membership pre-check short-circuits.
            outMsgs.push_back(present ? p : (L"zzqx-absent-" + std::to_wstring(rnd()) + L" " + p));
            ++made;
        }
        else
        {
            hay += L"assistant output line filler tokens ";
            hay += std::to_wstring(rnd());
            hay += L" lorem ipsum dolor sit amet consectetur adipiscing\n";
        }
    }
    while (made < prompts)
    {
        const std::wstring p = L"benchmark prompt number " + std::to_wstring(made) + L" do the thing carefully and well";
        hay += L"> " + p + L"\n";
        outMsgs.push_back(present ? p : (L"zzqx-absent " + p));
        ++made;
    }
    return hay;
}

static volatile size_t g_benchSink = 0;

template<class F>
static double TimeMsAvg(int iters, F&& fn)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
    {
        fn();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / (std::max)(1, iters);
}

static void BenchPromptAnchor()
{
    using namespace Agentmaster;
    std::wprintf(L"\n--- PromptAnchor benchmark (resolve = O(haystack); gated + off-thread in prod) ---\n");
    struct Case { const wchar_t* name; int rows; int prompts; int iters; };
    const Case cases[] = {
        { L"small  ~1k rows ", 1000, 20, 200 },
        { L"medium ~10k rows", 10000, 50, 40 },
        { L"large  ~50k rows", 50000, 200, 6 },
    };
    for (const auto& c : cases)
    {
        std::vector<std::wstring> msgsPresent, msgsMiss;
        const auto hay = BuildBenchHaystack(c.rows, c.prompts, msgsPresent, true);
        BuildBenchHaystack(c.rows, c.prompts, msgsMiss, false);
        const double mb = static_cast<double>(hay.size()) * sizeof(wchar_t) / (1024.0 * 1024.0);

        // Correctness sanity inside the bench: every present prompt resolves cleanly.
        const auto rr = ResolvePromptAnchors(hay, msgsPresent);
        int hit = 0;
        for (const auto& m : rr)
        {
            if (m.found && !m.partial && !m.outOfOrder)
            {
                ++hit;
            }
        }
        CHECK(hit == c.prompts, "bench: all present prompts resolve cleanly");

        const double tPresent = TimeMsAvg(c.iters, [&] { const auto r = ResolvePromptAnchors(hay, msgsPresent); g_benchSink += r.size() + (r.empty() ? 0 : r[0].offset); });
        const double tMiss = TimeMsAvg(c.iters, [&] { const auto r = ResolvePromptAnchors(hay, msgsMiss); g_benchSink += r.size(); });
        const double tNorm = TimeMsAvg(c.iters, [&] { const auto n = NormalizeForMatch(hay); g_benchSink += n.size(); });
        const int vIters = 200000;
        const double tValTotal = TimeMsAvg(1, [&] { for (int i = 0; i < vIters; ++i) { g_benchSink += ValidatePromptAnchor(hay, msgsPresent[0], rr[0].offset) ? 1u : 0u; } });

        std::wprintf(L"  %s : haystack=%6.2f MB, prompts=%3d\n", c.name, mb, c.prompts);
        std::wprintf(L"      resolve(present)=%8.3f ms   resolve(all-miss)=%8.3f ms   normalize-only=%8.3f ms\n", tPresent, tMiss, tNorm);
        std::wprintf(L"      validate-one    =%8.4f us   (epoch-unchanged fast path = 0)\n", (tValTotal / vIters) * 1000.0);
    }
    std::wprintf(L"  [sink %zu]\n", static_cast<size_t>(g_benchSink));
}

// Agentmaster (SUMMARY_JUMP.md §4 / point 2 "guarantee full batch resolve only"): measure the FULL batch
// resolve on a REAL, heavy on-disk session. Production resolves the ENTIRE prompt list on EVERY scan (no
// cache, by design), so a heavy session is the cost ceiling we commit to. Gated on AM_BENCH_SESSION (full
// path to a .jsonl): it SKIPS (does not fail) when unset, so CI / other machines never depend on a local
// file. "mixed" = the real, noise-filtered prompt list resolved against the trailing kAnchorRecentWindowChars
// of the transcript (recent prompts present, older ones a fast absence) — exactly what ControlCore caps +
// resolves; "all-miss" = the same N with guaranteed-absent needles (each forces a full-haystack scan).
static void BenchPromptAnchorRealSession()
{
    using namespace Agentmaster;
    wchar_t envbuf[1024]{};
    const DWORD got = GetEnvironmentVariableW(L"AM_BENCH_SESSION", envbuf, 1024);
    if (got == 0 || got >= 1024)
    {
        std::wprintf(L"\n--- PromptAnchor heavy-session bench: SKIPPED (set AM_BENCH_SESSION=<path-to-.jsonl>) ---\n");
        return;
    }
    const std::wstring path(envbuf, got);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        std::wprintf(L"\n--- PromptAnchor heavy-session bench: file not found (%ls) ---\n", path.c_str());
        return;
    }

    // The real, noise-filtered prompt list the panel / alt-nav resolve (production parity).
    const auto info = AnalyzeSessionTranscript(path, 0);
    const std::vector<std::wstring>& prompts = info.userMsgs;

    // Raw transcript -> wide, used as a haystack stand-in; cap to the production recent window so recent
    // prompts are present and older ones have "scrolled off" (the realistic mix ControlCore resolves).
    std::wstring raw;
    {
        std::FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") == 0 && fp)
        {
            std::fseek(fp, 0, SEEK_END);
            const long sz = std::ftell(fp);
            std::fseek(fp, 0, SEEK_SET);
            std::string bytes(sz > 0 ? static_cast<size_t>(sz) : 0u, '\0');
            if (!bytes.empty())
            {
                const size_t rd = std::fread(bytes.data(), 1, bytes.size(), fp);
                bytes.resize(rd);
            }
            std::fclose(fp);
            if (!bytes.empty())
            {
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                if (wlen > 0)
                {
                    raw.resize(static_cast<size_t>(wlen));
                    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), raw.data(), wlen);
                }
            }
        }
    }
    std::wstring hay = raw.size() > kAnchorRecentWindowChars ? raw.substr(raw.size() - kAnchorRecentWindowChars) : raw;
    const double mb = static_cast<double>(hay.size()) * sizeof(wchar_t) / (1024.0 * 1024.0);

    const auto rr = ResolvePromptAnchors(hay, prompts);
    int found = 0;
    for (const auto& m : rr)
    {
        if (m.found)
        {
            ++found;
        }
    }

    std::vector<std::wstring> miss;
    miss.reserve(prompts.size());
    for (size_t i = 0; i < prompts.size(); ++i)
    {
        miss.push_back(L"zqxjw9-absent-prompt-" + std::to_wstring(i)); // 8+ chars, nowhere in the haystack
    }

    const int iters = 10;
    const double tMixed = TimeMsAvg(iters, [&] { const auto r = ResolvePromptAnchors(hay, prompts); g_benchSink += r.size(); });
    const double tMiss = TimeMsAvg(iters, [&] { const auto r = ResolvePromptAnchors(hay, miss); g_benchSink += r.size(); });

    std::wprintf(L"\n--- PromptAnchor heavy-session bench (REAL transcript; full batch resolve, no cache) ---\n");
    std::wprintf(L"  session : %ls\n", path.c_str());
    std::wprintf(L"  prompts=%zu  resolved-in-tail=%d  haystack=%.2f MB (capped at kAnchorRecentWindowChars)\n",
                 prompts.size(), found, mb);
    std::wprintf(L"  resolve(mixed/realistic)=%8.3f ms   resolve(all-miss/worst)=%8.3f ms   [one scan, UI thread, x%d]\n",
                 tMixed, tMiss, iters);
    std::wprintf(L"  [sink %zu]\n", static_cast<size_t>(g_benchSink));
}

static void TestSummaryUserMsgNoise()
{
    using namespace Agentmaster;
    std::wprintf(L"Summary user-message noise filter (only human prompts numbered):\n");
    const std::wstring bullet(1, static_cast<wchar_t>(0x25CF)); // ● assistant/tool bullet
    const std::wstring rec(1, static_cast<wchar_t>(0x23FA)); // ⏺
    const std::wstring branch(1, static_cast<wchar_t>(0x23BF)); // ⎿ tool-result branch

    // Genuine human prompts: NOT noise (stay in the numbered list).
    CHECK(!SeIsCommandNoise(L"fix the build please"), "real prompt is not noise");
    CHECK(!SeIsCommandNoise(L"[Image #1] make this the README header"), "image paste is a real user msg");
    CHECK(!SeIsCommandNoise(L"### Results Output - are they correct?"), "markdown-header prompt kept");
    CHECK(!SeIsCommandNoise(L"use a " + bullet + L" bullet in the list"), "marker MID-text is not noise (only leading)");

    // Verbatim-pasted Claude-Code TUI output: noise (the reported "assistant message in my summary").
    CHECK(SeIsCommandNoise(bullet + L" Bug Summary - Launch crash-loop fixed"), "pasted assistant bullet (U+25CF) is noise");
    CHECK(SeIsCommandNoise(L"  " + bullet + L" Plan: six functions"), "leading whitespace + bullet still noise");
    CHECK(SeIsCommandNoise(rec + L" recording-marker paste"), "U+23FA marker is noise");
    CHECK(SeIsCommandNoise(branch + L" Wrote 13 lines to file"), "U+23BF tool-result branch is noise");

    // Pre-existing system-injected noise still caught (regression guard).
    CHECK(SeIsCommandNoise(L"<system-reminder>do x</system-reminder>"), "system-reminder still noise");
    CHECK(SeIsCommandNoise(L"<command-name>/clear</command-name>"), "command echo still noise");
}

// Agentmaster (PENDING_INPUT.md): the pure unsent-draft detector. Marker/rule glyphs are built from
// code points (this TU compiles without /utf-8, so the source stays pure-ASCII -- no raw glyph, no \u
// in a literal). Covers box identification, single/multi-line extraction, the empty box, a sent
// prompt vs the live box, menu-selection rejection, and the rule classifier.
static void TestPendingInput()
{
    std::wprintf(L"-- PendingInput (draft detection) --\n");
    const wchar_t MARK = static_cast<wchar_t>(0x276F); // the heavy right-angle prompt ornament
    const wchar_t MARK2 = static_cast<wchar_t>(0x203A); // the secondary single right-angle quote
    const wchar_t DASH = static_cast<wchar_t>(0x2500); // box-drawing light horizontal (the rule char)
    const std::wstring NL(1, static_cast<wchar_t>(10)); // a literal newline, sans a \n escape in source
    const std::wstring rule(60, DASH);
    const std::wstring marker = std::wstring(1, MARK) + L" "; // "> "
    auto V = [](std::initializer_list<std::wstring> r) { return std::vector<std::wstring>(r); };

    // 1. single-line draft
    {
        const auto d = DetectPendingInput(V({ rule, marker + L"hello world", rule }));
        CHECK(d.boxFound, "pending single: box found");
        CHECK(d.text == L"hello world", "pending single: text extracted");
    }
    // 2. multi-line draft -- continuation indent stripped, an internal blank line preserved
    {
        const auto d = DetectPendingInput(V({ rule, marker + L"line one", L"  line two", L"", L"  123", rule }));
        CHECK(d.boxFound, "pending multi: box found");
        CHECK(d.text == (L"line one" + NL + L"line two" + NL + NL + L"123"), "pending multi: lines joined + indent stripped + blank kept");
    }
    // 3. empty box -> box found, no draft
    {
        const auto d = DetectPendingInput(V({ rule, marker, rule }));
        CHECK(d.boxFound, "pending empty: box found");
        CHECK(d.text.empty(), "pending empty: no draft text");
    }
    // 4. no box at all
    {
        const auto d = DetectPendingInput(V({ L"assistant text", L"more output" }));
        CHECK(!d.boxFound, "pending none: no box");
    }
    // 5. a SENT prompt in scrollback + an empty input box below -> only the bottom box (empty)
    {
        const auto d = DetectPendingInput(V({ std::wstring(1, MARK) + L" previously sent", L"assistant replied", rule, marker, rule }));
        CHECK(d.boxFound, "pending sent+empty: box found");
        CHECK(d.text.empty(), "pending sent+empty: scrollback prompt ignored, box empty");
        CHECK(d.caretRow == 3, "pending sent+empty: caret is the bottom box, not the scrollback prompt");
    }
    // 6. menu selection (question directly above the marker) -> NOT the input box
    {
        const auto d = DetectPendingInput(V({ L"Do you want to proceed?", marker + L"1. Yes", L"  2. No" }));
        CHECK(!d.boxFound, "pending menu: not detected as input box");
    }
    // 7. menu wrapped in rules but with a question line above the marker -> still NOT detected
    {
        const auto d = DetectPendingInput(V({ rule, L"Select an option:", marker + L"1. Yes", L"  2. No", rule }));
        CHECK(!d.boxFound, "pending menu-in-rules: question above marker rejects false box");
    }
    // 8. rule-row classification
    {
        CHECK(IsPendingRuleRow(rule), "pending rule: pure rule is a rule");
        CHECK(!IsPendingRuleRow(std::wstring(3, DASH) + L" 3 files " + std::wstring(3, DASH)), "pending rule: labeled divider is NOT a rule");
        CHECK(!IsPendingRuleRow(L"just some text here"), "pending rule: text is not a rule");
        CHECK(!IsPendingRuleRow(std::wstring(3, DASH)), "pending rule: <6 box chars is not a rule");
    }
    // 9. marker with no following space
    {
        const auto d = DetectPendingInput(V({ rule, std::wstring(1, MARK) + L"text", rule }));
        CHECK(d.boxFound, "pending no-space: box found");
        CHECK(d.text == L"text", "pending no-space: marker stripped without a trailing space");
    }
    // 10. secondary marker U+203A
    {
        const auto d = DetectPendingInput(V({ rule, std::wstring(1, MARK2) + L" hi there", rule }));
        CHECK(d.boxFound, "pending marker2: U+203A recognized");
        CHECK(d.text == L"hi there", "pending marker2: text extracted");
    }
    // 11. trailing blank lines inside the box are trimmed
    {
        const auto d = DetectPendingInput(V({ rule, marker + L"only line", L"", L"", rule }));
        CHECK(d.text == L"only line", "pending trailing-blank: trimmed");
    }
    // 12. one blank row between the top rule and the marker -> still detected
    {
        const auto d = DetectPendingInput(V({ rule, L"", marker + L"padded", rule }));
        CHECK(d.boxFound, "pending blank-after-top-rule: detected");
        CHECK(d.text == L"padded", "pending blank-after-top-rule: text extracted");
    }
    // 13. empty rows
    {
        const auto d = DetectPendingInput(std::vector<std::wstring>{});
        CHECK(!d.boxFound, "pending empty-rows: nothing");
    }
}

int wmain()
{
    std::wprintf(L"=== Agentmaster engine tests ===\n");
    TestPendingInput();
    TestPromptAnchor();
    TestPromptAnchorEdgeCases();
    TestPromptAnchorRealCorpus();
    TestSummaryUserMsgNoise();
    TestSummaryTableTrim();
    TestStateMachine();
    TestOrderedStateMachine();
    TestWire();
    TestRegistry();
    TestRegistryFanout();
    TestForkSourceIdEcho();
    TestTypedCapture();
    TestObserveClaude();
    TestSupersedeStaleTabSiblings();
    TestSpawnBuilders();
    TestProfileBootstrap();
    TestScheduler();
    TestEnterRetry();
    TestBuildPromptSubmission();
    TestSchedulerIntegration();
    TestTranscriptScan();
    TestBlockedAndInterruptedStates();
    TestPersistence();
    TestManagerLayout();
    TestWindowRecord();
    TestAppSettings();
    TestTabNamingAndColor();
    TestProcessInspectTree();
    TestProcessInspectParse();
    TestTranscriptResolve();
    TestCodexObserve();
    TestTranscriptStore();
    TestContinuationChain();
    TestConversationLineage();
    TestSessionSearch();
    TestProcessInspectLive();
    TestBringToFrontHeuristics();
    TestBridgeRoundTrip();

    BenchPromptAnchor();
    BenchPromptAnchorRealSession(); // SUMMARY_JUMP.md §4: full batch resolve on a real heavy session (AM_BENCH_SESSION)

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
