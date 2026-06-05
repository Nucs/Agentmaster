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
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"
#include "../SessionScanner.h" // ParseTranscriptDelta (pure)

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

    const auto fwd = BuildForwarderScript();
    CHECK(fwd.find(L"NamedPipeClientStream") != std::wstring::npos, "forwarder uses NamedPipeClientStream");
    CHECK(fwd.find(L"CCMGR_SESSION_ID") != std::wstring::npos, "forwarder reads CCMGR_SESSION_ID");
    CHECK(fwd.find(L"CCMGR_HOOK_PIPE") != std::wstring::npos, "forwarder reads CCMGR_HOOK_PIPE");
    CHECK(fwd.find(L"session_id") != std::wstring::npos, "forwarder falls back to payload session_id");
    CHECK(fwd.find(L"WT_SESSION") != std::wstring::npos, "forwarder emits WT_SESSION tabToken");
    CHECK(fwd.find(L"bridge.json") != std::wstring::npos, "forwarder falls back to bridge.json discovery");

    const auto id = NewSessionId();
    CHECK(id.size() == 36, "uuid length 36");
    CHECK(id[8] == L'-' && id[13] == L'-' && id[18] == L'-' && id[23] == L'-', "uuid hyphens");
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

    in.manager.selectedId = L"conv-abc";
    in.manager.scopeDir = L"K:/api";
    in.manager.selectedPromptId = L"q1";
    in.manager.collapsedDirs.push_back(L"K:/old");
    in.manager.layout.boardFraction = 0.5;
    in.manager.layout.treeFraction = 0.45;

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

    CHECK(out.manager.selectedId == L"conv-abc", "lens selectedId round-trip");
    CHECK(out.manager.scopeDir == L"K:/api", "lens scopeDir round-trip");
    CHECK(out.manager.selectedPromptId == L"q1", "lens selectedPromptId round-trip");
    CHECK(out.manager.collapsedDirs.size() == 1 && out.manager.collapsedDirs[0] == L"K:/old", "lens collapsedDirs round-trip");
    CHECK(approx(out.manager.layout.boardFraction, 0.5) && approx(out.manager.layout.treeFraction, 0.45), "lens splitter fractions round-trip");

    // Tolerant of a missing / corrupt document.
    {
        const auto empty = DeserializeWindowRecord(L"");
        CHECK(empty.windowId.empty() && empty.tabs.empty(), "empty text -> empty record (no throw)");
        const auto garbage = DeserializeWindowRecord(L"}{not json");
        CHECK(garbage.windowId.empty(), "garbage text -> empty record (no throw)");
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
    }

    // Empty / garbage -> all defaults (a missing settings.json must change nothing).
    {
        const auto out = DeserializeAppSettings(L"");
        CHECK(out.skipPermissions == true && out.includeCoAuthoredBy == true, "settings defaults on empty");
        CHECK(out.defaultAutopilotMode == AutopilotMode::Off && out.maxAutoSends == 100u, "settings autopilot defaults on empty");
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

int wmain()
{
    std::wprintf(L"=== Agentmaster engine tests ===\n");
    TestStateMachine();
    TestWire();
    TestRegistry();
    TestRegistryFanout();
    TestTypedCapture();
    TestObserveClaude();
    TestSpawnBuilders();
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
    TestProcessInspectLive();
    TestBridgeRoundTrip();

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
