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
#include "../ProcessInspect.h" // SnapshotProcesses / ReadClaudeFacts / ResolveSessionId (Observer O1)
#include "../ProfileBootstrap.h" // the per-install state PROFILE (choice file / resolution / migrate)
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"
#include "../SessionScanner.h" // ParseTranscriptDelta (pure)
#include "../SessionSearch.h" // the Sessions page's two-phase search (SESSIONS.md §6)
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

    // PsSingleQuote: PowerShell single-quoted literal (only escape = doubled quote).
    CHECK(PsSingleQuote(L"C:\\Users\\x\\.agentmaster") == L"'C:\\Users\\x\\.agentmaster'", "ps quote: plain path verbatim");
    CHECK(PsSingleQuote(L"C:\\Users\\o'brien") == L"'C:\\Users\\o''brien'", "ps quote: embedded quote doubled");
    CHECK(PsSingleQuote(L"$env:FOO") == L"'$env:FOO'", "ps quote: $ stays inert");

    const auto fwd = BuildForwarderScript(L"C:\\Users\\x\\.agentmaster-dev");
    CHECK(fwd.find(L"NamedPipeClientStream") != std::wstring::npos, "forwarder uses NamedPipeClientStream");
    CHECK(fwd.find(L"CCMGR_SESSION_ID") != std::wstring::npos, "forwarder reads CCMGR_SESSION_ID");
    CHECK(fwd.find(L"CCMGR_HOOK_PIPE") != std::wstring::npos, "forwarder reads CCMGR_HOOK_PIPE");
    CHECK(fwd.find(L"session_id") != std::wstring::npos, "forwarder falls back to payload session_id");
    CHECK(fwd.find(L"WT_SESSION") != std::wstring::npos, "forwarder emits WT_SESSION tabToken");
    // The bridge-discovery fallback is PER-PROFILE: the stateDir is baked in (PS-single-quoted);
    // no profile-blind $env:USERPROFILE\.agentmaster path and no unexpanded placeholder remain.
    CHECK(fwd.find(L"$disc = 'C:\\Users\\x\\.agentmaster-dev\\bridge.json'") != std::wstring::npos, "forwarder discovery is the per-profile bridge.json");
    CHECK(fwd.find(L"Join-Path $env:USERPROFILE") == std::wstring::npos, "forwarder discovery is not profile-blind");
    CHECK(fwd.find(L"{{AM_BRIDGE_JSON}}") == std::wstring::npos, "forwarder placeholder fully substituted");

    const auto id = NewSessionId();
    CHECK(id.size() == 36, "uuid length 36");
    CHECK(id[8] == L'-' && id[13] == L'-' && id[18] == L'-' && id[23] == L'-', "uuid hyphens");
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
        auto s = mk(AutopilotMode::Full, SessionState::WaitingForInput);
        s.lastMessageWasQuestion = true;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::Hold, "question -> hold");
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
            CHECK(r.queue.size() == 2, "queue size");
            CHECK(r.queue.size() == 2 && r.queue[0].status == PromptStatus::Sent && r.queue[0].sentAtUnixMs == 999, "Sent status preserved (no replay)");
            CHECK(r.queue.size() == 2 && r.queue[0].origin == PromptOrigin::Typed && r.queue[1].origin == PromptOrigin::Flight, "prompt origin preserved (Typed vs Flight)");
            CHECK(r.queue.size() == 2 && r.queue[1].gate == PromptGate::Manual && r.queue[1].guardPattern == L"answers-a-question:ok", "prompt gate+guard preserved");
            CHECK(r.autopilot.mode == AutopilotMode::Full && r.autopilot.throttleMs == 750 && !r.autopilot.stopOnError && r.autopilot.maxAutoSends == 7, "autopilot preserved");
            CHECK(!r.autopilot.approval.pauseForHuman && r.autopilot.approval.autoApproveTools.size() == 2, "approval policy preserved");
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

    // Selected tab persisted by stable identity (Claude conversation id preferred; index is the shell
    // fallback). Both set to non-default values so a dropped field FAILS the round-trip (a -1 default
    // would otherwise mask a missing index).
    in.selectedSessionId = L"conv-abc";
    in.selectedTabIndex = 1;

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

    CHECK(out.tabs.size() == 2, "tab count + order preserved");
    if (out.tabs.size() == 2)
    {
        CHECK(out.tabs[0].kind == TabKind::Claude, "tab[0] is Claude");
        CHECK(out.tabs[0].tabColor == L"#FFD700", "Claude tab color round-trip");
        CHECK(out.tabs[0].sessionId == L"conv-abc", "Claude tab sessionId reference round-trip");
        CHECK(out.tabs[1].kind == TabKind::Other, "tab[1] is Other");
        CHECK(out.tabs[1].actionsJson == L"[{\"action\":\"newTab\",\"profile\":\"pwsh\"}]", "Other tab actionsJson (nested JSON) round-trip");
    }

    CHECK(out.selectedSessionId == L"conv-abc", "selected tab persisted by stable Claude id (round-trip)");
    CHECK(out.selectedTabIndex == 1, "selectedTabIndex shell fallback (round-trip)");

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
        in.defaultLaunchDir = L"K:/work";
        in.env = L"FOO=bar;BAZ=qux";
        in.archiveSplitFraction = 0.33;
        in.waitingDecayMinutes = 0; // 0 = never decay — MUST round-trip as 0, not fall back to 5
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
        CHECK(out.defaultLaunchDir == L"K:/work", "settings defaultLaunchDir round-trip");
        CHECK(out.archiveSplitFraction > 0.329 && out.archiveSplitFraction < 0.331, "settings archiveSplitFraction round-trip");
        CHECK(out.waitingDecayMinutes == 0u, "settings waitingDecayMinutes stored 0 (= never) round-trips as 0");
    }

    // Empty / garbage -> all defaults (a missing settings.json must change nothing).
    {
        const auto out = DeserializeAppSettings(L"");
        CHECK(out.skipPermissions == true && out.includeCoAuthoredBy == true, "settings defaults on empty");
        CHECK(out.defaultAutopilotMode == AutopilotMode::Off && out.maxAutoSends == 100u, "settings autopilot defaults on empty");
        CHECK(out.archiveSplitFraction > 0.499 && out.archiveSplitFraction < 0.501, "settings archiveSplitFraction default 0.5 on empty");
        CHECK(out.waitingDecayMinutes == 5u, "settings waitingDecayMinutes default 5 (cache lifetime) on empty");
        const auto out2 = DeserializeAppSettings(L"not json");
        CHECK(out2.skipPermissions == true && out2.confirmBeforeKill == true, "settings defaults on garbage");
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

    // --- IsGenericDirName (case-folded) ---
    CHECK(IsGenericDirName(L"bin") && IsGenericDirName(L"BIN") && IsGenericDirName(L"Obj"), "generic names case-insensitive");
    CHECK(!IsGenericDirName(L"NumSharp"), "project name not generic");

    // --- NormDirKey: slash + trailing + (windows) case fold collapse to one key ---
    CHECK(NormDirKey(L"K:\\A\\B\\") == NormDirKey(L"K:/a/b"), "case/slash/trailing variants share a key");

    // --- AutoDirColorHex: deterministic, palette form, stable across path spelling ---
    const auto c1 = AutoDirColorHex(L"K:\\source\\NumSharp");
    CHECK(c1.size() == 7 && c1[0] == L'#', "auto color is #RRGGBB");
    CHECK(c1 == AutoDirColorHex(L"K:\\source\\NumSharp"), "auto color deterministic");
    CHECK(c1 == AutoDirColorHex(L"k:/source/numsharp"), "auto color stable across spelling");

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
    // user line: tool_result array -> NOT a human prompt (no false positive)
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"x","content":"ok"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "tool_result user line -> no prompt");
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
    // mirror of the missed-Stop synthesis: fires only off a freshly-appended turn event whose tail
    // says a turn is in progress, and only out of the two states a missed prompt strands a session in.
    {
        CHECK(ShouldSynthesizeRunning(SessionState::WaitingForInput, true, L"", 500), "run-repair: fresh user line + Waiting -> synthesize");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, L"tool_use", 500), "run-repair: assistant mid-turn line + Idle -> synthesize");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, L"", -200), "run-repair: future mtime (clock skew) counts as fresh");
        CHECK(!ShouldSynthesizeRunning(SessionState::Running, true, L"", 500), "run-repair: already Running -> no-op");
        CHECK(!ShouldSynthesizeRunning(SessionState::NeedsApproval, true, L"", 500), "run-repair: NeedsApproval never cleared by a transcript line");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, true, L"", 500), "run-repair: Error never cleared by inference");
        CHECK(!ShouldSynthesizeRunning(SessionState::Done, true, L"", 500), "run-repair: Done never revived");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, L"end_turn", 500), "run-repair: end_turn tail is missed-Stop territory, not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, false, L"", 500), "run-repair: no new turn event this pass -> no synthesis");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, L"", kScanRunRepairFreshMs + 1), "run-repair: stale write (history replay) -> no synthesis");
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

// ===== Fleet Observer O1 (ProcessInspect primitives; doc/agentmaster/OBSERVER.md §6, §8b) =====

// A canned snapshot modelling one WindowsTerminal hosting three tabs: pwsh->claude (tab A),
// cmd->cmd-shim->claude (tab B, 2 levels deep), pwsh->git (tab C, no claude). 201 (claude) also
// has a node child, so a claude-rooted search must still exclude the root.
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
    CHECK(FindDescendantByImage(snap, 201, L"claude.exe") == 0, "descendant search excludes the root itself");
    CHECK(FindDescendantByImage(snap, 201, L"node.exe") == 202, "descendant of a claude found");

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
            "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:09Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"on it\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"build\"}}]}}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:20Z\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"ok\"}]}}\n"
            "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:30Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"done\"}],\"stop_reason\":\"end_turn\"}}\n"
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

int wmain()
{
    std::wprintf(L"=== Agentmaster engine tests ===\n");
    TestStateMachine();
    TestOrderedStateMachine();
    TestWire();
    TestRegistry();
    TestRegistryFanout();
    TestTypedCapture();
    TestObserveClaude();
    TestSpawnBuilders();
    TestProfileBootstrap();
    TestScheduler();
    TestTranscriptScan();
    TestPersistence();
    TestManagerLayout();
    TestWindowRecord();
    TestAppSettings();
    TestTabNamingAndColor();
    TestProcessInspectTree();
    TestProcessInspectParse();
    TestTranscriptResolve();
    TestTranscriptStore();
    TestSessionSearch();
    TestProcessInspectLive();
    TestBringToFrontHeuristics();
    TestBridgeRoundTrip();

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
