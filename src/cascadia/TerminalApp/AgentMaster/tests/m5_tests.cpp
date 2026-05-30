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
#include <string>
#include <thread>

// Headers only; the engine .cpp TUs are compiled separately and linked (see
// run-m5-tests.bat) so each keeps its own anonymous-namespace helpers.
#include "../ClaudeSpawn.h"
#include "../HookWire.h"
#include "../HooksBridge.h"
#include "../Json.h"
#include "../Persistence.h"
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"

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

int wmain()
{
    std::wprintf(L"=== Agentmaster engine tests ===\n");
    TestStateMachine();
    TestWire();
    TestRegistry();
    TestRegistryFanout();
    TestTypedCapture();
    TestSpawnBuilders();
    TestScheduler();
    TestPersistence();
    TestManagerLayout();
    TestAppSettings();
    TestBridgeRoundTrip();

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
