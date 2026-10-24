// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 harness — the AI search's WINDOW AND TAB TOOLS and its new system prompts (SESSIONS.md §9.14).
//
// What is pinned here:
//   * the pure window rules (SessionSearchTools.h): WindowHandleOf, DescribeWindowsForModel and ResolveWindowArgument —
//     one window needs no choice; with several and none named the call REFUSES with the instruction to ask the user
//     and every window described (the one the request came from, the one used last); numbers, "#2", "window 2", short
//     and whole ids, an ambiguous prefix, an unknown window;
//   * the tabs server on a toolset with fake windows (no window, no registry): the server's tools and read-only flags;
//     list (short ids shared with every other tool, dormant / adopted / works_in / selected flags, the window / dir /
//     title filters, the limit that keeps the selected tab and the most recently active ones in strip order, asked_from
//     and last_used); resume (an open session is never opened twice; the multi-window refusal; `after` by pos and by
//     session id, both validated against the chosen window; the result's wording); fork (the multi-window refusal names
//     the source's tab, the default placement after the source, model); the per-request caps (5 opens, 1 deactivate,
//     reset by the request generation); a window that did not answer (the slot stays spent) vs one that refused (the
//     slot comes back); deactivate (one id, no tab, an adopted tab, a dormant tab, a turn in flight without and with
//     cut_off_turn, one per request);
//   * the engine's ANSWERED fan-out on a local Engine (Engine.h ...In): the census in registration order with the sink's
//     own window id and the last-active flag, a window that never answers (left out after the timeout), a sink that
//     throws (counted at once), a double reply, RunWindowTool to one window, RunWindowToolOnHost to the host;
//   * the two system prompts' structure: the three headings in order, the plan rule, ten behaviors and nineteen
//     workflows, every tool named, no database tool in the page prompt, no `deep` in the database prompt, never
//     "on the screen", ASCII, and under MSVC's literal limit;
//   * what "## Tools" teaches (the user's review of 2026-10-18: "grep various forms of what we are looking for"), and
//     that it is TRUE of the code: the matching rules (any case, a word inside longer words, every word in ONE kind of
//     place, the page's query never matching the branch) against MatchesQueryText / SearchIndexFast; each regex example
//     of "Patterns for grep" against GrepRequiredLiteral (the fixed piece the prompt promises is the one the grep
//     narrows by, and the bad examples really have none); the per-mode costs, the workflows that use the patterns,
//     and the tabs advice; and the page tools' schemas that repeat it;
//   * MESSAGING.md §21.23, the REMOTE LAUNCH's answer (a sibling of the answered fan-out above, on the same local Engine
//     seam): RemoteLaunch.h's outcome rules (a default answer is a failure; which results count as opened; one stable log
//     word per result; the phone's reason for each refusal, the PC's own words trimmed and ended with a period, the preset
//     rule's tail) and RemoteLaunchSubject (a one-line title, the short id, a folder's last name); and the engine's ANSWERED
//     launch routing (RequestLaunchInWindowIn): no window -> false and no answer; an inline answer; a late answer from
//     another thread, given twice, delivered once; a window that drops the request unanswered -> WindowClosed from the
//     engine; the last-activated window first, a refusing or throwing sink skipped for the next; every sink refusing ->
//     false, and no answer even when a refusing sink's copy of the reply dies later; a throwing requester contained; an
//     empty reply; and (§21.25) a request that NAMES its window goes to that window only — with its anchor session — and a
//     named window that is not open answers false with no other window asked.

#include "m5_tests.h"

#include "../Engine.h"
#include "../SessionDbToolDefs.h"
#include "../SessionSearch.h"
#include "../SessionSearchTools.h"
#include "../WindowTabTools.h"

#include <algorithm>
#include <atomic> // MESSAGING.md §21.23: the remote launch tests count answers delivered on other threads
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    // The fake fleet: two windows, the second used last, the request asked from the first.
    const std::wstring kWinA = L"{AAAA1111-0000-4000-8000-000000000001}";
    const std::wstring kWinB = L"{BBBB2222-0000-4000-8000-000000000002}";
    const std::wstring kS1 = L"11111111-aaaa-4000-8000-000000000001"; // window 1, tab 1: waiting, selected
    const std::wstring kS2 = L"22222222-aaaa-4000-8000-000000000002"; // window 1, tab 3: running, works elsewhere
    const std::wstring kS3 = L"33333333-aaaa-4000-8000-000000000003"; // window 1, tab 4: dormant
    const std::wstring kS4 = L"44444444-aaaa-4000-8000-000000000004"; // window 1, tab 5: adopted (external)
    const std::wstring kS5 = L"55555555-aaaa-4000-8000-000000000005"; // window 2, tab 1: needs approval
    const std::wstring kS6 = L"66666666-aaaa-4000-8000-000000000006"; // closed: no tab anywhere
    const std::wstring kS7 = L"77777777-aaaa-4000-8000-000000000007"; // the id a fake fork / fresh resume opens

    /// <summary>The model's argument text as an object (an unparseable text is an empty object, and a CHECK fails).</summary>
    json::Value Args(const std::wstring& text)
    {
        auto parsed = json::Parse(text);
        CHECK(parsed.has_value(), "tab tools test: the argument text parses");
        return parsed ? *parsed : json::Value::MkObj();
    }

    /// <summary>A tool of a server list by its server and tool names, or nullptr.</summary>
    const HarnessTool* FindTool(const std::vector<HarnessToolServer>& servers, const std::wstring& server, const std::wstring& name)
    {
        for (const auto& s : servers)
        {
            if (s.name != server)
            {
                continue;
            }
            for (const auto& t : s.tools)
            {
                if (t.name == name)
                {
                    return &t;
                }
            }
        }
        return nullptr;
    }

    /// <summary>One tab of the fake census.</summary>
    WindowTabInfo Tab(uint32_t index, const std::wstring& title, const std::wstring& sessionId, const std::wstring& state, bool started, int64_t lastActivityMs)
    {
        WindowTabInfo t;
        t.index = index;
        t.title = title;
        t.sessionId = sessionId;
        t.state = state;
        t.started = started;
        t.lastActivityUnixMs = lastActivityMs;
        return t;
    }

    /// <summary>The fake fleet's census, as the agent would hand it to the toolset (strip + registry halves).</summary>
    std::vector<WindowCensus> FakeCensus(int64_t now)
    {
        WindowCensus a;
        a.windowId = kWinA;
        a.title = L"Agentmaster";
        auto s1 = Tab(0, L"Agentmaster", kS1, L"waiting for you", true, now - 5 * 60000);
        s1.selected = true;
        s1.dir = L"K:\\source\\Agentmaster";
        s1.branch = L"agentmaster";
        s1.lastTurnUnixMs = now - 10 * 60000;
        // The overview's columns (SESSIONS.md 9.16) on every session tab; `list` prints none of them.
        s1.mostRecentUnixMs = now - 5 * 60000;
        s1.turns = 41;
        s1.contextTokens = 427312;
        s1.model = L"claude-opus-5-5";
        s1.effort = L"max";
        s1.queued = 2;
        s1.runner = L"auto";
        a.tabs.push_back(s1);
        a.tabs.push_back(Tab(1, L"pwsh", L"", L"", false, 0));
        auto s2 = Tab(2, L"catalog", kS2, L"running", true, now - 60000);
        s2.dir = L"K:\\catalog";
        s2.worksIn = L"K:\\catalog\\sysd";
        s2.mostRecentUnixMs = now - 60000;
        s2.turns = 9;
        s2.contextTokens = 88000;
        s2.model = L"claude-sonnet-5-5";
        s2.draft = true;
        s2.runner = L"auto";
        a.tabs.push_back(s2);
        auto s3 = Tab(3, L"old work", kS3, L"idle", false, now - 30LL * 86400000);
        s3.dir = L"K:\\source\\old";
        s3.mostRecentUnixMs = now - 30LL * 86400000;
        s3.turns = 3;
        s3.runner = L"paused";
        a.tabs.push_back(s3);
        auto s4 = Tab(4, L"hand-typed", kS4, L"waiting for you", true, now - 2 * 3600000);
        s4.external = true;
        s4.dir = L"C:\\Users\\ELI";
        s4.mostRecentUnixMs = now - 2 * 3600000;
        s4.runner = L"auto";
        a.tabs.push_back(s4);

        WindowCensus b;
        b.windowId = kWinB;
        b.title = L"notes";
        b.lastActive = true;
        auto s5 = Tab(0, L"release", kS5, L"needs approval", true, now - 3 * 60000);
        s5.dir = L"K:\\source\\Agentmaster";
        s5.mostRecentUnixMs = now - 3 * 60000;
        s5.turns = 12;
        s5.contextTokens = 1234567;
        s5.model = L"claude-fable-5";
        s5.unread = true;
        s5.runner = L"auto";
        b.tabs.push_back(s5);
        auto shell = Tab(1, L"notes", L"", L"", false, 0);
        shell.selected = true;
        b.tabs.push_back(shell);
        return { a, b };
    }

    /// <summary>The pure window rules.</summary>
    void TestWindowRules()
    {
        CHECK(WindowHandleOf(kWinA) == L"aaaa1111" && WindowHandleOf(L"{AB}") == L"ab", "WindowHandleOf: braces dropped, lower case, 8 characters (a short id stays whole)");
        const auto windows = FakeCensus(1'800'000'000'000LL);
        const std::wstring described = DescribeWindowsForModel(windows, kWinA);
        CHECK(described.find(L"window 1 (id aaaa1111): 5 tabs, \"Agentmaster\" selected; this request came from it\n") != std::wstring::npos &&
                  described.find(L"window 2 (id bbbb2222): 2 tabs, \"notes\" selected; the user used it last\n") != std::wstring::npos,
              "DescribeWindowsForModel: one line per window, its selected tab, the origin and the last-used window marked");

        std::wstring error;
        CHECK(!ResolveWindowArgument({}, L"1", L"", error) && error.find(L"No Agentmaster window answered") != std::wstring::npos, "no window: a refusal that says so");
        const std::vector<WindowCensus> one{ windows[0] };
        CHECK(ResolveWindowArgument(one, L"", L"", error) == size_t{ 0 }, "one window: an empty argument means it (nothing to ask)");
        error.clear();
        CHECK(!ResolveWindowArgument(windows, L"", kWinA, error), "two windows and none named: refused");
        CHECK(error.find(L"Ask the user which window") != std::wstring::npos && error.find(L"Do not choose one yourself") != std::wstring::npos &&
                  error.find(L"window 1 (id aaaa1111)") != std::wstring::npos && error.find(L"window 2 (id bbbb2222)") != std::wstring::npos &&
                  error.find(L"this request came from it") != std::wstring::npos,
              "the multi-window refusal tells the model to ask the user, and describes every window (the user's rule of 2026-10-17)");
        CHECK(ResolveWindowArgument(windows, L"2", L"", error) == size_t{ 1 } && ResolveWindowArgument(windows, L" #2 ", L"", error) == size_t{ 1 } &&
                  ResolveWindowArgument(windows, L"Window 1", L"", error) == size_t{ 0 },
              "a number, \"#2\" and \"window 1\" name windows by their 1-based place");
        error.clear();
        CHECK(!ResolveWindowArgument(windows, L"3", L"", error) && error.find(L"There is no window 3") != std::wstring::npos, "a number past the windows is refused, with the list");
        CHECK(ResolveWindowArgument(windows, L"BBBB2222", L"", error) == size_t{ 1 } && ResolveWindowArgument(windows, L"{aaaa1111-0000-4000-8000-000000000001}", L"", error) == size_t{ 0 } &&
                  ResolveWindowArgument(windows, L"bbbb", L"", error) == size_t{ 1 },
              "a short id, a whole id with braces, and a 4-character prefix name a window, in any case");
        error.clear();
        CHECK(!ResolveWindowArgument(windows, L"abc", L"", error) && error.find(L"No open window has the id abc") != std::wstring::npos, "a prefix under 4 characters never matches");
        auto twins = windows;
        twins[1].windowId = L"{AAAA1111-9999-4000-8000-000000000009}";
        error.clear();
        CHECK(!ResolveWindowArgument(twins, L"aaaa1111", L"", error) && error.find(L"fits more than one window") != std::wstring::npos, "an id prefix two windows share is refused: pass the number");
    }

    /// <summary>The tabs server on a toolset with fake windows.</summary>
    void TestTabsServer()
    {
        const int64_t now = 1'800'000'000'000LL;
        uint64_t generation = 1;
        std::vector<std::pair<std::wstring, WindowToolRequest>> opens; // (windowId, request) the toolset sent
        std::vector<std::wstring> parks;
        std::vector<std::wstring> focused;
        WindowToolOutcome openAnswer; // what the fake window answers an open with
        openAnswer.handled = true;
        openAnswer.ok = true;
        openAnswer.sessionId = kS7;
        openAnswer.index = 1;
        WindowToolOutcome parkAnswer;
        parkAnswer.handled = true;
        parkAnswer.ok = true;

        SessionSearchToolset::Callbacks cb;
        cb.catalog = [](int64_t, const std::function<bool()>&) { return std::shared_ptr<const SessionCatalog>{}; };
        cb.onShow = [](const std::vector<std::wstring>&, const std::wstring&, const std::vector<std::wstring>&) {};
        cb.generation = [&generation] { return generation; };
        cb.now = [now] { return now; };
        cb.tabs.census = [now] { return FakeCensus(now); };
        cb.tabs.open = [&opens, &openAnswer](const std::wstring& windowId, const WindowToolRequest& request) {
            opens.emplace_back(windowId, request);
            return openAnswer;
        };
        cb.tabs.deactivate = [&parks, &parkAnswer](const std::wstring& sessionId) {
            parks.push_back(sessionId);
            return parkAnswer;
        };
        cb.tabs.focus = [&focused](const std::wstring& sessionId) {
            focused.push_back(sessionId);
            return true;
        };
        cb.tabs.originWindowId = [] { return kWinA; };
        SessionSearchToolset toolset{ cb };
        CHECK(toolset.UsesTabs(), "the toolset serves the tabs server when the census callback is set");

        const auto servers = toolset.Servers();
        CHECK(servers.size() == 3 && servers[0].name == L"sessions" && servers[1].name == L"display" && servers[2].name == L"tabs", "the servers: sessions, display, then tabs (last, so the search tools keep their places)");
        const HarnessTool* list = FindTool(servers, L"tabs", L"list");
        const HarnessTool* resume = FindTool(servers, L"tabs", L"resume");
        const HarnessTool* fork = FindTool(servers, L"tabs", L"fork");
        const HarnessTool* deactivate = FindTool(servers, L"tabs", L"deactivate");
        CHECK(list && resume && fork && deactivate, "list, resume, fork and deactivate are there");
        // SESSIONS.md 9.16: overview needs only the census; status needs the facts callback, which this toolset lacks.
        const HarnessTool* overviewTool = FindTool(servers, L"tabs", L"overview");
        CHECK(overviewTool && overviewTool->readOnly && FindTool(servers, L"tabs", L"status") == nullptr && servers.size() == 3 && !servers[2].tools.empty() &&
                  servers[2].tools.front().name == L"overview",
              "overview is served first (read-only); without the facts callback there is no status tool");
        if (!list || !resume || !fork || !deactivate)
        {
            return;
        }
        CHECK(list->readOnly && !resume->readOnly && !fork->readOnly && !deactivate->readOnly, "list only reads (parallel with a search); the three that change the workspace are not read-only");
        bool clean = true;
        for (const auto* t : { list, resume, fork, deactivate })
        {
            clean = clean && json::Parse(t->inputSchemaJson).has_value() && t->description.find(L"screen") == std::wstring::npos;
        }
        CHECK(clean, "every tabs schema is JSON, and no description speaks of a screen (a phone has none)");
        CHECK(resume->description.find(L"ask the user which window") != std::wstring::npos && deactivate->description.find(L"ONE tab") != std::wstring::npos,
              "the descriptions carry the two rules: ask which window, one tab");

        // ---- list ----
        HarnessToolResult r = list->handler(Args(L"{}"));
        CHECK(!r.isError, "list: runs");
        auto parsed = json::Parse(r.text);
        const json::Value* windows = parsed ? parsed->Find(L"list") : nullptr;
        CHECK(parsed && parsed->I64At(L"windows_open") == 2 && windows && windows->arr.size() == 2, "list: both windows");
        if (windows && windows->arr.size() == 2)
        {
            const auto& w1 = windows->arr[0];
            const auto& w2 = windows->arr[1];
            CHECK(w1.I64At(L"window") == 1 && w1.StrAt(L"id") == L"aaaa1111" && w1.I64At(L"tabs") == 5 && w1.BoolAt(L"asked_from") && !w1.BoolAt(L"last_used"), "list: window 1, its short id, its tab count, asked from it");
            CHECK(w2.BoolAt(L"last_used") && !w2.BoolAt(L"asked_from"), "list: window 2 is the one used last");
            const json::Value* tabs = w1.Find(L"tab_list");
            CHECK(tabs && tabs->arr.size() == 5, "list: every tab of window 1 under the default limit");
            if (tabs && tabs->arr.size() == 5)
            {
                CHECK(tabs->arr[0].I64At(L"pos") == 1 && tabs->arr[0].StrAt(L"id") == L"11111111" && tabs->arr[0].BoolAt(L"selected") && tabs->arr[0].StrAt(L"state") == L"waiting for you" &&
                          tabs->arr[0].StrAt(L"branch") == L"agentmaster" && !tabs->arr[0].StrAt(L"last_turn").empty() && tabs->arr[0].StrAt(L"active").find(L"(5m ago)") != std::wstring::npos,
                      "list: a tab's pos, short id, selected flag, state, branch, last turn and last activity");
                CHECK(tabs->arr[1].StrAt(L"kind") == L"shell" && tabs->arr[1].Find(L"id") == nullptr, "list: a shell tab has a kind and no session id");
                CHECK(tabs->arr[2].StrAt(L"works_in") == L"K:\\catalog\\sysd" && tabs->arr[3].BoolAt(L"dormant") && tabs->arr[4].BoolAt(L"adopted"), "list: works_in, dormant and adopted are reported");
            }
        }
        CHECK(toolset.ResolveHandle(L"22222222") == kS2, "list: its short ids are the conversation's (read, show, fork take them)");
        r = list->handler(Args(LR"({"window":"2"})"));
        parsed = json::Parse(r.text);
        windows = parsed ? parsed->Find(L"list") : nullptr;
        CHECK(!r.isError && windows && windows->arr.size() == 1 && windows->arr[0].I64At(L"window") == 2, "list: window narrows to one window, which keeps its number");
        r = list->handler(Args(LR"({"dir":"k:/CATALOG"})"));
        parsed = json::Parse(r.text);
        windows = parsed ? parsed->Find(L"list") : nullptr;
        CHECK(windows && windows->arr.size() == 2 && windows->arr[0].Find(L"tab_list")->arr.size() == 1 && windows->arr[0].I64At(L"listed") == 1 && windows->arr[1].Find(L"tab_list")->arr.empty(),
              "list: dir matches the folder or works_in, any case, '/' as '\\'");
        r = list->handler(Args(LR"({"title":"RELEASE"})"));
        parsed = json::Parse(r.text);
        windows = parsed ? parsed->Find(L"list") : nullptr;
        CHECK(windows && windows->arr[1].Find(L"tab_list")->arr.size() == 1 && windows->arr[0].Find(L"tab_list")->arr.empty(), "list: title matches any case");
        r = list->handler(Args(LR"({"window":"1","limit":2})"));
        parsed = json::Parse(r.text);
        windows = parsed ? parsed->Find(L"list") : nullptr;
        const json::Value* cut = windows && !windows->arr.empty() ? windows->arr[0].Find(L"tab_list") : nullptr;
        CHECK(cut && cut->arr.size() == 2 && cut->arr[0].I64At(L"pos") == 1 && cut->arr[1].I64At(L"pos") == 3 && !parsed->StrAt(L"note").empty(),
              "list: over the limit it keeps the selected tab and the most recently active one, in strip order, and says so");
        CHECK(list->handler(Args(LR"({"window":"9"})")).isError, "list: an unknown window is an error");

        // ---- resume ----
        r = resume->handler(Args(L"{\"id\":\"" + kS6 + L"\"}"));
        CHECK(r.isError && r.text.find(L"Ask the user which window") != std::wstring::npos && opens.empty(), "resume: two windows and none named -> refused, nothing opened");
        r = resume->handler(Args(L"{\"id\":\"" + kS6 + L"\",\"window\":\"2\",\"after\":\"1\"}"));
        CHECK(!r.isError && opens.size() == 1 && opens[0].first == kWinB && opens[0].second.kind == WindowToolRequest::Kind::Resume && opens[0].second.sessionId == kS6 &&
                  opens[0].second.afterIndex == 0 && opens[0].second.afterSessionId.empty() && !opens[0].second.focus,
              "resume: window 2, after its tab 1 -> that window, afterIndex 0, a background tab");
        CHECK(r.text.find(L"Resumed in window 2 (id bbbb2222) as tab 2, right after \"release\"") != std::wstring::npos && r.text.find(L"FRESH") != std::wstring::npos,
              "resume: the result says where, after which tab, and that a fresh session started when the window opened another id");
        r = resume->handler(Args(L"{\"id\":\"" + kS6 + L"\",\"window\":1,\"after\":\"22222222\"}"));
        CHECK(!r.isError && opens.size() == 2 && opens[1].first == kWinA && opens[1].second.afterSessionId == kS2 && opens[1].second.afterIndex == -1,
              "resume: window as a JSON number; after as a session id of that window -> afterSessionId");
        r = resume->handler(Args(L"{\"id\":\"" + kS6 + L"\",\"window\":\"1\",\"after\":9}"));
        CHECK(r.isError && r.text.find(L"has no tab 9") != std::wstring::npos && opens.size() == 2, "resume: a pos past the window's tabs is refused before anything opens");
        r = resume->handler(Args(L"{\"id\":\"" + kS6 + L"\",\"window\":\"1\",\"after\":\"55555555\"}"));
        CHECK(r.isError && r.text.find(L"is not in window 1") != std::wstring::npos && opens.size() == 2, "resume: after a tab of ANOTHER window is refused");
        r = resume->handler(Args(L"{\"id\":\"" + kS1 + L"\"}"));
        CHECK(!r.isError && r.text.find(L"Already open: window 1, tab 1 \"Agentmaster\"") != std::wstring::npos && opens.size() == 2 && focused.empty(),
              "resume: an open session is never opened twice, and needs no window");
        r = resume->handler(Args(L"{\"id\":\"" + kS3 + L"\",\"focus\":true}"));
        CHECK(!r.isError && r.text.find(L"dormant") != std::wstring::npos && focused.size() == 1 && focused[0] == kS3, "resume: focus:true brings an open tab to the front (a dormant one says so)");
        CHECK(resume->handler(Args(LR"({"id":"nope"})")).isError && resume->handler(Args(L"{}")).isError, "resume: an unknown or missing id is an error");

        // ---- fork ----
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\"}"));
        CHECK(r.isError && r.text.find(L"Ask the user which window") != std::wstring::npos && r.text.find(L"window 1, tab 3 \"catalog\": a fork usually goes right after it") != std::wstring::npos && opens.size() == 2,
              "fork: two windows and none named -> refused, and the refusal names where the source is");
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\",\"model\":\" opus \"}"));
        CHECK(!r.isError && opens.size() == 3 && opens[2].second.kind == WindowToolRequest::Kind::Fork && opens[2].second.afterIndex == -1 && opens[2].second.afterSessionId.empty() &&
                  opens[2].second.model == L"opus",
              "fork: no after -> the window's default (next to its source), with the model trimmed");
        CHECK(r.text.find(L"right after \"catalog\"") != std::wstring::npos && r.text.find(L"The fork's id is 77777777") != std::wstring::npos && r.text.find(L"background tab") != std::wstring::npos,
              "fork: the result names the source's tab it went after, the fork's short id, and that it is a background tab");

        // ---- the per-request caps ----
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\"}"));
        CHECK(!r.isError && opens.size() == 4, "caps: the 4th open of the request runs");
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\"}"));
        CHECK(!r.isError && opens.size() == 5, "caps: the 5th open of the request runs");
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\"}"));
        CHECK(r.isError && r.text.find(L"already opened 5 tabs") != std::wstring::npos && opens.size() == 5, "caps: the 6th is refused (kMaxTabOpensPerRequest)");
        ++generation; // the next request
        openAnswer.handled = false; // the window did not answer in time
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\"}"));
        CHECK(r.isError && r.text.find(L"did not answer in time") != std::wstring::npos && r.text.find(L"may still open") != std::wstring::npos && opens.size() == 6,
              "a new request opens again; a window that did not answer is reported as such (the tab may still open)");
        openAnswer.handled = true;
        openAnswer.ok = false;
        openAnswer.message = L"no tab opened: Claude Code may be missing.";
        for (int i = 0; i < 4; ++i)
        {
            fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\"}"));
        }
        openAnswer.ok = true;
        openAnswer.message.clear();
        r = fork->handler(Args(L"{\"id\":\"" + kS2 + L"\",\"window\":\"1\"}"));
        CHECK(!r.isError && opens.size() == 11, "caps: refused opens give their slot back, an unanswered one keeps it spent (1 spent + 4 returned + this one = 2 of 5)");

        // ---- deactivate ----
        CHECK(deactivate->handler(Args(L"{\"id\":[\"" + kS1 + L"\"]}")).isError && parks.empty(), "deactivate: a list of ids is refused (one tab at a time)");
        r = deactivate->handler(Args(L"{\"id\":\"" + kS6 + L"\"}"));
        CHECK(r.isError && r.text.find(L"no open tab") != std::wstring::npos && parks.empty(), "deactivate: a session without a tab is refused");
        r = deactivate->handler(Args(L"{\"id\":\"" + kS4 + L"\"}"));
        CHECK(r.isError && r.text.find(L"started by hand") != std::wstring::npos && parks.empty(), "deactivate: an adopted tab (the user's own shell) is refused");
        r = deactivate->handler(Args(L"{\"id\":\"" + kS3 + L"\"}"));
        CHECK(!r.isError && r.text.find(L"Nothing to do") != std::wstring::npos && parks.empty(), "deactivate: a dormant tab is reported, not parked again");
        r = deactivate->handler(Args(L"{\"id\":\"" + kS5 + L"\"}"));
        CHECK(r.isError && r.text.find(L"needs approval") != std::wstring::npos && r.text.find(L"cut_off_turn:true") != std::wstring::npos && parks.empty(),
              "deactivate: a turn in flight is refused without the user's confirmation (cut_off_turn)");
        r = deactivate->handler(Args(L"{\"id\":\"" + kS5 + L"\",\"cut_off_turn\":true}"));
        CHECK(!r.isError && parks.size() == 1 && parks[0] == kS5 && r.text.find(L"Deactivated window 2, tab 1 \"release\"") != std::wstring::npos,
              "deactivate: confirmed -> the host parks it, and the result names the tab");
        r = deactivate->handler(Args(L"{\"id\":\"" + kS1 + L"\"}"));
        CHECK(r.isError && r.text.find(L"only one tab may be deactivated per request") != std::wstring::npos && parks.size() == 1, "deactivate: a second park in the same request is refused");
        ++generation;
        parkAnswer.ok = false;
        parkAnswer.message = L"a prompt is being typed into that tab right now; try again in a few seconds.";
        r = deactivate->handler(Args(L"{\"id\":\"" + kS1 + L"\"}"));
        CHECK(r.isError && r.text.find(L"being typed") != std::wstring::npos && parks.size() == 2, "deactivate: the host's refusal reaches the model");
        parkAnswer.ok = true;
        parkAnswer.message.clear();
        r = deactivate->handler(Args(L"{\"id\":\"" + kS1 + L"\"}"));
        CHECK(!r.isError && parks.size() == 3, "deactivate: a refused park gave its slot back, so the next one of the request runs");

        // ---- the database mode keeps the tabs server ----
        SessionSearchToolset::Callbacks db = cb;
        db.catalog = {};
        db.database.call = [](const std::string&, const json::Value&) { return HarnessToolResult{ L"{}", false }; };
        SessionSearchToolset dbToolset{ db };
        const auto dbServers = dbToolset.Servers();
        CHECK(dbServers.size() == 3 && dbServers[0].name == L"agentmaster" && dbServers[1].name == L"display" && dbServers[2].name == L"tabs", "the database mode: agentmaster, display, then tabs");
        SessionSearchToolset::Callbacks none = cb;
        none.tabs = {};
        SessionSearchToolset noTabs{ none };
        CHECK(!noTabs.UsesTabs() && noTabs.Servers().size() == 2, "without the census callback there is no tabs server");
    }

    /// <summary>
    /// The live-facts rules (SESSIONS.md 9.16): the state words and the one-phrase status, the unread and runner rules, the
    /// queue count, why a queued prompt waits (DecideAdvance in plain words), and the facts of a registry record.
    /// </summary>
    void TestLiveFactsRules()
    {
        const int64_t now = 1'800'000'000'000LL;
        CHECK(std::wstring{ SessionStateWord(SessionState::WaitingForInput) } == L"waiting for you" && std::wstring{ SessionStateWord(SessionState::NeedsApproval) } == L"needs approval" &&
                  std::wstring{ SessionStateWord(SessionState::Running) } == L"running" && std::wstring{ SessionStateWord(SessionState::Idle) } == L"idle",
              "SessionStateWord: the Triage Board's words");
        CHECK(SessionStatusPhrase(L"idle", true, false, false) == L"idle (dormant)" && SessionStatusPhrase(L"waiting for you", true, false, true) == L"waiting for you" &&
                  SessionStatusPhrase(L"running", false, true, false) == L"closed" && SessionStatusPhrase(L"", true, true, false) == L"open" && SessionStatusPhrase(L"error", true, true, false) == L"error",
              "SessionStatusPhrase: a dormant tab marked, an adopted one never dormant, no tab = closed, an unknown state = open");

        SessionInfo s;
        s.id = L"rules-1";
        s.live = true;
        s.started = true;
        s.state = SessionState::WaitingForInput;
        s.lastActivityUnixMs = now - 60000;
        s.readUnixMs = now - 120000;
        CHECK(SessionLooksUnread(s), "unread: waiting for you, with activity after the last visit of its tab");
        s.readUnixMs = now;
        CHECK(!SessionLooksUnread(s), "unread: not after a visit");
        s.manualUnread = true;
        CHECK(SessionLooksUnread(s), "unread: Mark Unread always");
        s.manualUnread = false;
        s.state = SessionState::Idle;
        s.readUnixMs = 0;
        CHECK(!SessionLooksUnread(s), "unread: never for an idle session (only a state that needs the user)");

        s.autorunner.mode = AutorunnerMode::Full;
        CHECK(SessionRunnerWord(s, false) == L"auto" && SessionRunnerWord(s, true) == L"auto (Pause all on)", "runner: auto, and Pause all named");
        s.autorunner.mode = AutorunnerMode::Off;
        CHECK(SessionRunnerWord(s, false) == L"paused", "runner: paused");
        s.autorunner.interruptHeldMode = AutorunnerMode::Full;
        s.autorunner.interruptHeldUnixMs = now - 1000;
        CHECK(SessionRunnerWord(s, false) == L"held", "runner: held by an interrupt until the user's next message");
        s.autorunner.interruptHeldMode = AutorunnerMode::Off;
        s.autorunner.interruptHeldUnixMs = 0;

        // ---- why the next queued prompt waits ----
        CHECK(SessionQueuedCount(s) == 0 && DescribeQueueWait(s, now, 0, false).empty(), "queue wait: nothing queued -> nothing to say");
        QueuedPrompt q;
        q.id = L"q1";
        q.text = L"run the suite\nand report";
        q.status = PromptStatus::Pending;
        q.queuedAtUnixMs = now - 120000;
        s.queue.push_back(q);
        QueuedPrompt done = q;
        done.id = L"q0";
        done.status = PromptStatus::Sent;
        done.origin = PromptOrigin::Typed;
        s.queue.insert(s.queue.begin(), done);
        CHECK(SessionQueuedCount(s) == 1, "queue count: Pending prompts only (a sent one is history)");
        s.state = SessionState::WaitingForInput;
        s.stateChangedAtUnixMs = now - 120000;
        CHECK(DescribeQueueWait(s, now, 0, false).find(L"paused for this session") != std::wstring::npos, "queue wait: the runner paused for this session");
        s.autorunner.mode = AutorunnerMode::Full;
        CHECK(DescribeQueueWait(s, now, 0, true).find(L"Pause all is on") != std::wstring::npos, "queue wait: Pause all");
        CHECK(DescribeQueueWait(s, now, 0, false).find(L"it goes out now") != std::wstring::npos, "queue wait: a settled, ready session sends it now");
        s.stateChangedAtUnixMs = now - 1000;
        CHECK(DescribeQueueWait(s, now, 0, false).find(L"held for 40 seconds") != std::wstring::npos, "queue wait: the 40-second status settle");
        s.stateChangedAtUnixMs = now - 120000;
        s.lastMessageWasQuestion = true;
        CHECK(DescribeQueueWait(s, now, 0, false).find(L"ended with a question") != std::wstring::npos, "queue wait: the question guard");
        s.lastMessageWasQuestion = false;
        s.state = SessionState::Running;
        CHECK(DescribeQueueWait(s, now, 0, false) == L"after the running turn ends", "queue wait: a running turn");
        s.state = SessionState::WaitingForInput;
        s.started = false;
        CHECK(DescribeQueueWait(s, now, 0, false).find(L"dormant") != std::wstring::npos, "queue wait: a dormant tab");
        s.started = true;

        // ---- the facts of a record ----
        s.title = L"Rules";
        s.workingDir = L"K:\\source\\Agentmaster";
        s.model = L"sonnet";
        s.currentModel = L"claude-opus-5-5";
        s.turnStartUnixMs = now - 300000;
        s.turnEndUnixMs = now - 240000;
        s.errorMessage = L"API Error: overloaded";
        s.pendingInput = L"half a thought";
        s.pendingInputUnixMs = now - 30000;
        s.lastAssistantText = L"Done.\nAll tests pass.";
        s.forkParentId = kS2;
        SessionLiveFacts f = SessionLiveFactsOf(s, now, 0, false);
        CHECK(f.known && f.managed && f.live && f.started && f.state == L"waiting for you" && f.mostRecentUnixMs == now - 240000 && f.model == L"claude-opus-5-5" &&
                  f.errorMessage.empty() && f.draft == L"half a thought" && f.draftUnixMs == now - 30000 && f.lastReply == L"Done. All tests pass." && f.forkedFrom == kS2 && f.turns == -1,
              "facts: the MOST RECENT key, the model of the newest reply over the launch model, no error outside Error, the draft and the reply on one line, the fork source; turns left to the index");
        CHECK(f.queued == 1 && f.nextPrompt == L"run the suite ..." && f.runner == L"auto" && f.nextWait.find(L"goes out now") != std::wstring::npos,
              "facts: the queue - its count, the next prompt's first line (more lines marked), the runner, and why it waits");
        s.state = SessionState::Error;
        s.errorStatus = 529;
        f = SessionLiveFactsOf(s, now, 0, false);
        CHECK(f.errorMessage == L"API Error: overloaded" && f.errorStatus == 529 && f.state == L"error", "facts: an error's message and HTTP status, in Error only");
        s.kind = AgentKind::Codex;
        f = SessionLiveFactsOf(s, now, 0, false);
        CHECK(f.codex && f.runner.empty() && f.nextWait.empty(), "facts: a managed Codex has no Prompt Queue Runner");
    }

    /// <summary>
    /// The overview and status tools (SESSIONS.md 9.16) on a toolset with fake windows and fake facts: the overview's page
    /// (the head, the window sections, the folder groups in MOST RECENT order, the rows, the shells), each filter as one value
    /// and as a list, the fold levels and the size limit; status as one id (one object) and as a list (a list), the tab's
    /// place, the queue, the lineage, unknown ids, the 32-id cap, the census asked only for an open session; the database
    /// mode's rows with status and age; the schemas.
    /// </summary>
    void TestOverviewAndStatus()
    {
        const int64_t now = 1'800'000'000'000LL;
        int censusCalls = 0;
        int factsCalls = 0;
        bool lastFull = false;
        SessionSearchToolset::Callbacks cb;
        cb.catalog = [](int64_t, const std::function<bool()>&) { return std::shared_ptr<const SessionCatalog>{}; };
        cb.onShow = [](const std::vector<std::wstring>&, const std::wstring&, const std::vector<std::wstring>&) {};
        cb.now = [now] { return now; };
        cb.tabs.census = [now, &censusCalls] {
            ++censusCalls;
            return FakeCensus(now);
        };
        cb.tabs.open = [](const std::wstring&, const WindowToolRequest&) { return WindowToolOutcome{}; };
        cb.tabs.deactivate = [](const std::wstring&) { return WindowToolOutcome{}; };
        cb.tabs.originWindowId = [] { return kWinA; };
        // The facts of kS1 (open, waiting, a question, a queue), kS2 (open, running), kS6 (closed, a fork that continues
        // another session); any other id names no session.
        cb.tabs.facts = [now, &factsCalls, &lastFull](const std::vector<std::wstring>& ids, bool full) {
            ++factsCalls;
            lastFull = full;
            std::vector<SessionLiveFacts> out;
            for (const auto& id : ids)
            {
                SessionLiveFacts f;
                f.sessionId = id;
                if (id == kS1)
                {
                    f.known = f.managed = f.live = f.started = true;
                    f.state = L"waiting for you";
                    f.stateSinceUnixMs = now - 4 * 60000;
                    f.mostRecentUnixMs = now - 5 * 60000;
                    f.title = L"Agentmaster";
                    f.dir = L"K:\\source\\Agentmaster";
                    f.branch = L"agentmaster";
                    f.turns = 41;
                    f.contextTokens = 427312;
                    f.model = L"claude-opus-5-5";
                    f.effort = L"max";
                    f.heartbeat = L"waiting";
                    f.waitingFor = L"input needed";
                    f.unread = true;
                    f.askedQuestion = true;
                    f.lastReply = L"Shall I run the suite?";
                    f.queued = 2;
                    f.nextPrompt = L"run the suite";
                    f.runner = L"auto";
                    f.nextWait = L"its last turn ended with a question: it waits until the user answers";
                }
                else if (id == kS2)
                {
                    f.known = f.managed = f.live = f.started = true;
                    f.state = L"running";
                    f.mostRecentUnixMs = now - 60000;
                    f.title = L"catalog";
                    f.runner = L"auto";
                }
                else if (id == kS6)
                {
                    f.known = f.managed = true;
                    f.state = L"idle";
                    f.mostRecentUnixMs = now - 3LL * 86400000;
                    f.title = L"closed work";
                    f.dir = L"K:\\source\\closed";
                    f.turns = 7;
                    f.forkedFrom = kS2;
                    f.continuedFrom = kS3;
                    f.continuedVia = L"clear";
                    f.createdUnixMs = now - 10LL * 86400000;
                    f.tags = { L"release" };
                    f.favorite = true;
                }
                out.push_back(std::move(f));
            }
            return out;
        };
        SessionSearchToolset toolset{ cb };
        const auto servers = toolset.Servers();
        const HarnessTool* overview = FindTool(servers, L"tabs", L"overview");
        const HarnessTool* status = FindTool(servers, L"tabs", L"status");
        CHECK(overview && status && overview->readOnly && status->readOnly && servers.size() == 3 && servers[2].tools.size() == 6 && servers[2].tools[1].name == L"status",
              "the tabs server: overview, status (with the facts callback), list, resume, fork, deactivate; both new tools only read");
        if (!overview || !status)
        {
            return;
        }
        CHECK(json::Parse(overview->inputSchemaJson).has_value() && json::Parse(status->inputSchemaJson).has_value() && overview->description.find(L"screen") == std::wstring::npos &&
                  status->description.find(L"screen") == std::wstring::npos && overview->inputSchemaJson.find(L"anyOf") != std::wstring::npos && status->inputSchemaJson.find(L"anyOf") != std::wstring::npos &&
                  overview->description.find(L"whenever the user mentions a window, a tab or an open session") != std::wstring::npos && status->description.find(L"up to 32 sessions") != std::wstring::npos,
              "the schemas are JSON and take one value or a list (anyOf); the descriptions say when (a window, a tab, an open session) and how many (32), never a screen");

        // ---- overview: the whole workspace ----
        HarnessToolResult r = overview->handler(Args(L"{}"));
        const std::wstring& page = r.text;
        CHECK(!r.isError && page.rfind(L"# Agentmaster windows and tabs (now ", 0) == 0 && page.find(L"2 windows open, 7 tabs in all.") != std::wstring::npos,
              "overview: the head names the time, the windows and the tabs");
        CHECK(page.find(L"Listed: 5 sessions (1 running, 2 waiting for you, 1 needs approval, 1 idle; 1 dormant; 1 unread; 1 with prompts queued) and 2 shell tabs.") != std::wstring::npos,
              "overview: the listed sessions counted by status, dormant, unread and queued; the shells apart");
        CHECK(page.find(L"## Window 1 (id aaaa1111): 5 tabs; \"Agentmaster\" (pos 1) selected; this request came from it\n") != std::wstring::npos &&
                  page.find(L"## Window 2 (id bbbb2222): 2 tabs; \"notes\" (pos 2) selected; the user used it last\n") != std::wstring::npos,
              "overview: one section per window - number, short id, tabs, the selected tab, the origin, the window used last");
        const size_t gCatalog = page.find(L"### K:\\catalog\\sysd: 1 tab (1 running)");
        const size_t gAgentmaster = page.find(L"### K:\\source\\Agentmaster: 1 tab (1 waiting for you)");
        const size_t gHome = page.find(L"### C:\\Users\\ELI: 1 tab (1 waiting for you)");
        const size_t gOld = page.find(L"### K:\\source\\old: 1 tab (1 idle)");
        CHECK(gCatalog != std::wstring::npos && gAgentmaster != std::wstring::npos && gHome != std::wstring::npos && gOld != std::wstring::npos && gCatalog < gAgentmaster &&
                  gAgentmaster < gHome && gHome < gOld,
              "overview: the folder groups (works_in over dir) with the most recent work first (MOST RECENT)");
        CHECK(page.find(L"| pos | title | id | status | age | turns | ctx | model | queue | notes |") != std::wstring::npos &&
                  page.find(L"| 1 | Agentmaster | 11111111 | waiting for you | 5m | 41 | 427K | opus-5-5 max | 2 auto | selected |") != std::wstring::npos,
              "overview: a row - pos, title, short id, status, age, turns, ctx, the model without claude-, the queue with its runner, notes");
        CHECK(page.find(L"| 3 | catalog | 22222222 | running | 1m | 9 | 88K | sonnet-5-5 | - | draft |") != std::wstring::npos &&
                  page.find(L"| 4 | old work | 33333333 | idle (dormant) | 1mo | 3 | - | - | - | - |") != std::wstring::npos &&
                  page.find(L"| 5 | hand-typed | 44444444 | waiting for you | 2h | - | - | - | - | adopted |") != std::wstring::npos &&
                  page.find(L"| 1 | release | 55555555 | needs approval | 3m | 12 | 1.2M | fable-5 | - | unread |") != std::wstring::npos,
              "overview: a draft, a dormant tab, an adopted one (never dormant), unknown values as -, a million tokens as 1.2M, unread");
        CHECK(page.find(L"Shell tabs (1, no session): 2 \"pwsh\"\n") != std::wstring::npos && page.find(L"Shell tabs (1, no session): 2 \"notes\" (selected)\n") != std::wstring::npos,
              "overview: each window's shell tabs on one line, last");
        CHECK(toolset.ResolveHandle(L"33333333") == kS3 && toolset.ResolveHandle(L"55555555") == kS5, "overview: its short ids are the conversation's (status, read, show take them)");

        // ---- overview: each filter, one value or a list ----
        r = overview->handler(Args(LR"({"window":2})"));
        CHECK(!r.isError && r.text.find(L"## Window 2") != std::wstring::npos && r.text.find(L"## Window 1") == std::wstring::npos && r.text.find(L"Filter: window 2.") != std::wstring::npos,
              "overview: window as one number -> that window only, the filter named");
        r = overview->handler(Args(LR"({"window":["2","aaaa1111"]})"));
        const size_t w2 = r.text.find(L"## Window 2");
        const size_t w1 = r.text.find(L"## Window 1");
        CHECK(!r.isError && w2 != std::wstring::npos && w1 != std::wstring::npos && w2 < w1 && r.text.find(L"Filter: windows 2, 1.") != std::wstring::npos,
              "overview: window as a list (a number and an id) -> those windows, in the order asked");
        r = overview->handler(Args(LR"({"window":"[1]"})"));
        CHECK(!r.isError && r.text.find(L"## Window 1") != std::wstring::npos && r.text.find(L"## Window 2") == std::wstring::npos, "overview: a list written as a string is read as the list");
        r = overview->handler(Args(LR"({"state":["needs approval","running"]})"));
        CHECK(!r.isError && r.text.find(L" 22222222 ") != std::wstring::npos && r.text.find(L" 55555555 ") != std::wstring::npos &&
                  r.text.find(L" 11111111 ") == std::wstring::npos && r.text.find(L"5 tabs, 1 listed") != std::wstring::npos,
              "overview: state as a list -> any of them; a window says how many of its tabs are listed");
        r = overview->handler(Args(LR"({"state":"waiting"})"));
        CHECK(!r.isError && r.text.find(L" 11111111 ") != std::wstring::npos && r.text.find(L" 44444444 ") != std::wstring::npos && r.text.find(L" 22222222 ") == std::wstring::npos,
              "overview: state as one value, part of a state's words (waiting -> waiting for you)");
        r = overview->handler(Args(LR"({"state":"dormant"})"));
        CHECK(!r.isError && r.text.find(L" 33333333 ") != std::wstring::npos && r.text.find(L" 11111111 ") == std::wstring::npos && r.text.find(L"## Window 2") == std::wstring::npos,
              "overview: state dormant; a window with nothing listed is left out when no window was asked");
        r = overview->handler(Args(LR"({"state":"shell"})"));
        CHECK(!r.isError && r.text.find(L"Shell tabs (1, no session)") != std::wstring::npos && r.text.find(L"| pos |") == std::wstring::npos, "overview: state shell -> only the shell tabs");
        r = overview->handler(Args(LR"({"dir":["CATALOG","k:/source/old"]})"));
        CHECK(!r.isError && r.text.find(L" 22222222 ") != std::wstring::npos && r.text.find(L" 33333333 ") != std::wstring::npos && r.text.find(L" 11111111 ") == std::wstring::npos &&
                  r.text.find(L"Shell tabs") == std::wstring::npos,
              "overview: dir as a list, any case, either slash, works_in included; a folder filter leaves the shells out");
        r = overview->handler(Args(LR"({"state":"zz"})"));
        CHECK(!r.isError && r.text.find(L"No tab matches the filter.") != std::wstring::npos && r.text.find(L"## Window") == std::wstring::npos,
              "overview: a value of 2 characters must be a whole word; nothing listed says so");
        r = overview->handler(Args(LR"({"window":["1",9]})"));
        CHECK(r.isError && r.text.find(L"There is no window 9") != std::wstring::npos, "overview: a window that is not open is an error, with the windows described");
        r = overview->handler(Args(LR"({"state":true})"));
        CHECK(r.isError && r.text.find(L"Pass state as one value") != std::wstring::npos, "overview: a value of another type is an error that says how to pass it");

        // ---- overview: the fold levels and the size limit (RenderWorkspaceOverview directly) ----
        {
            WindowCensus big;
            big.windowId = kWinA;
            for (uint32_t i = 0; i < 62; ++i)
            {
                WindowTabInfo t = Tab(i, L"tab " + std::to_wstring(i), L"aaaa" + std::to_wstring(1000 + i) + L"-0000-4000-8000-000000000000", i < 2 ? L"running" : L"idle", true, now - 3LL * 86400000);
                t.dir = L"K:\\source\\Big";
                t.mostRecentUnixMs = i < 2 ? now - 60000 : now - 3LL * 86400000;
                big.tabs.push_back(t);
            }
            const auto shortId = [](const std::wstring& id) { return id.substr(0, 8); };
            const std::wstring full = RenderWorkspaceOverview({ big }, OverviewFilter{}, L"", now, shortId, 100000);
            const std::wstring folded = RenderWorkspaceOverview({ big }, OverviewFilter{}, L"", now, shortId, 3000);
            CHECK(full.find(L"Quiet") == std::wstring::npos && full.find(L"| 62 | tab 61 |") != std::wstring::npos, "fold: a page within its budget folds nothing");
            CHECK(folded.size() <= 3000 && folded.find(L"Quiet for over a day (60): 3 tab 2 [aaaa1002] 3d; ") != std::wstring::npos && folded.find(L"| 1 | tab 0 |") != std::wstring::npos,
                  "fold level 1: the tabs quiet for over a day on one line (pos, title, id, age); the running ones keep their rows");
            for (auto& t : big.tabs)
            {
                t.mostRecentUnixMs = now - 3600000; // quiet, but within the day
            }
            const std::wstring recent = RenderWorkspaceOverview({ big }, OverviewFilter{}, L"", now, shortId, 3000);
            CHECK(recent.size() <= 3000 && recent.find(L"Quiet, idle or done (60): ") != std::wstring::npos, "fold level 2: every quiet tab, when the day-old ones are not enough");
            WindowCensus groups;
            groups.windowId = kWinA;
            for (uint32_t i = 0; i < 40; ++i)
            {
                WindowTabInfo t = Tab(i, L"busy " + std::to_wstring(i), L"bbbb" + std::to_wstring(1000 + i) + L"-0000-4000-8000-000000000000", L"running", true, now - 60000);
                t.dir = L"K:\\work\\folder" + std::to_wstring(i);
                t.mostRecentUnixMs = now - 60000 - i;
                groups.tabs.push_back(t);
            }
            const std::wstring cut = RenderWorkspaceOverview({ groups }, OverviewFilter{}, L"", now, shortId, 1600);
            CHECK(cut.size() <= 1600 && cut.find(L"more tabs are not listed: this page has a size limit. Call overview again with window, dir or state") != std::wstring::npos &&
                      cut.find(L"### K:\\work\\folder0:") != std::wstring::npos,
                  "the size limit: nothing left to fold -> the page stops at its budget, the most recent folders first, and says how many tabs it left out and how to see them");
        }

        // ---- status: one id -> one object ----
        censusCalls = 0;
        r = status->handler(Args(LR"({"ids":"11111111"})"));
        auto parsed = json::Parse(r.text);
        CHECK(!r.isError && parsed && parsed->Find(L"sessions") == nullptr && !parsed->StrAt(L"now").empty() && parsed->StrAt(L"id") == L"11111111" && parsed->StrAt(L"status") == L"waiting for you" &&
                  parsed->StrAt(L"age") == L"5m" && parsed->StrAt(L"since").find(L"(4m ago)") != std::wstring::npos && lastFull && censusCalls == 1,
              "status: one id answers ONE object - its status, since when, its age by the MOST RECENT key - from the FULL facts, with one census for its tab");
        const json::Value* tab = parsed ? parsed->Find(L"tab") : nullptr;
        CHECK(tab && tab->I64At(L"window") == 1 && tab->I64At(L"pos") == 1 && tab->BoolAt(L"selected") && tab->Find(L"dormant") == nullptr, "status: where its tab is (window, pos, selected)");
        const json::Value* queue = parsed ? parsed->Find(L"queue") : nullptr;
        CHECK(parsed && parsed->I64At(L"turns") == 41 && parsed->StrAt(L"ctx") == L"427K" && parsed->StrAt(L"model") == L"opus-5-5" && parsed->StrAt(L"effort") == L"max" &&
                  parsed->StrAt(L"heartbeat") == L"waiting (input needed)" && parsed->BoolAt(L"unread") && parsed->BoolAt(L"asked_question") && parsed->StrAt(L"last_reply") == L"Shall I run the suite?" &&
                  queue && queue->I64At(L"waiting") == 2 && queue->StrAt(L"next") == L"run the suite" && queue->StrAt(L"runner") == L"auto" && queue->StrAt(L"next_goes").find(L"question") != std::wstring::npos,
              "status: turns, ctx, model and effort, Claude's heartbeat, unread, the question, the last reply, and the queue with why its next prompt waits");
        r = status->handler(Args(LR"({"id":"11111111"})"));
        CHECK(!r.isError && json::Parse(r.text) && json::Parse(r.text)->StrAt(L"id") == L"11111111", "status: `id` is read like `ids`");

        // ---- status: a list -> a list, in the order asked ----
        censusCalls = 0;
        r = status->handler(Args(L"{\"ids\":[\"" + kS6 + L"\",\"11111111\",\"nope\"]}"));
        parsed = json::Parse(r.text);
        const json::Value* list = parsed ? parsed->Find(L"sessions") : nullptr;
        CHECK(!r.isError && list && list->arr.size() == 2 && parsed->I64At(L"count") == 2 && list->arr[0].StrAt(L"id") == L"66666666" && list->arr[1].StrAt(L"id") == L"11111111" &&
                  list->arr[1].Find(L"now") == nullptr && censusCalls == 1,
              "status: a list answers a list in the order asked, `now` once at the top");
        const json::Value* unknownIds = parsed ? parsed->Find(L"unknown") : nullptr;
        CHECK(unknownIds && unknownIds->arr.size() == 1 && unknownIds->arr[0].str == L"nope", "status: an id that names no session is listed as unknown, never guessed");
        if (list && list->arr.size() == 2)
        {
            const auto& closed = list->arr[0];
            const json::Value* continues = closed.Find(L"continues");
            CHECK(closed.StrAt(L"status") == L"closed" && closed.Find(L"tab") == nullptr && closed.Find(L"since") == nullptr && closed.StrAt(L"age") == L"3d" && closed.I64At(L"turns") == 7 &&
                      closed.StrAt(L"forked_from") == L"22222222" && continues && continues->StrAt(L"id") == L"33333333" && continues->StrAt(L"via") == L"clear" && closed.BoolAt(L"fav") &&
                      closed.Find(L"tags") && closed.Find(L"tags")->arr.size() == 1 && closed.StrAt(L"created").size() == 10,
                  "status: a closed session - no tab, no since; its age, turns, lineage (short ids), star, tags and creation day");
        }
        censusCalls = 0;
        r = status->handler(Args(L"{\"ids\":[\"" + kS6 + L"\"]}"));
        parsed = json::Parse(r.text);
        CHECK(!r.isError && parsed && parsed->Find(L"sessions") && parsed->Find(L"sessions")->arr.size() == 1 && censusCalls == 0,
              "status: a one-element LIST still answers a list; no census when none of them is open");
        r = status->handler(Args(LR"({"ids":"11111111, 22222222"})"));
        parsed = json::Parse(r.text);
        CHECK(!r.isError && parsed && parsed->Find(L"sessions") && parsed->Find(L"sessions")->arr.size() == 2 && parsed->Find(L"sessions")->arr[1].StrAt(L"status") == L"running",
              "status: several ids in one string are a list too");
        std::wstring many = L"{\"ids\":[";
        for (int i = 0; i < 33; ++i)
        {
            many += (i ? L"," : L"") + std::wstring{ L"\"" } + std::to_wstring(10000000 + i) + L"-aaaa-4000-8000-000000000000\"";
        }
        many += L"]}";
        r = status->handler(Args(many));
        parsed = json::Parse(r.text);
        CHECK(!r.isError && parsed && parsed->StrAt(L"not_reported").find(L"1 more ids were not reported: at most 32 per call") != std::wstring::npos && parsed->I64At(L"count") == 0 &&
                  parsed->Find(L"unknown") && parsed->Find(L"unknown")->arr.size() == 32,
              "status: at most 32 ids per call; the rest named as not reported");
        r = status->handler(Args(LR"({"ids":"99999999-aaaa-4000-8000-000000000009"})"));
        CHECK(r.isError && r.text.find(L"No session has the id") != std::wstring::npos, "status: one id that names no session is an error");
        CHECK(status->handler(Args(L"{}")).isError && status->handler(Args(LR"({"ids":{"a":1}})")).isError, "status: no ids, or ids of another type, are errors");

        // ---- the database mode: its search rows carry status and age too ----
        SessionSearchToolset::Callbacks dbcb = cb;
        dbcb.catalog = {};
        dbcb.database.call = [](const std::string& tool, const json::Value&) {
            if (tool == "search")
            {
                return HarnessToolResult{ LR"({"matched":2,"rows":[{"id":"11111111","title":"a","dir":"K:\\x","active":"2026-10-24 10:00","msgs":3},{"id":"66666666","title":"b","dir":"K:\\y","active":"2026-10-20 10:00","msgs":1}]})", false };
            }
            return HarnessToolResult{ L"{}", false };
        };
        dbcb.database.resolve = [](const std::vector<std::wstring>& handles) {
            std::vector<std::wstring> out;
            for (const auto& h : handles)
            {
                out.push_back(h == L"11111111" ? kS1 : (h == L"66666666" ? kS6 : std::wstring{}));
            }
            return out;
        };
        SessionSearchToolset dbToolset{ dbcb };
        const auto dbServers = dbToolset.Servers();
        const HarnessTool* dbSearch = FindTool(dbServers, L"agentmaster", L"search");
        CHECK(dbSearch != nullptr, "the database mode serves its search");
        if (dbSearch)
        {
            factsCalls = 0;
            r = dbSearch->handler(Args(LR"({"query":"x"})"));
            parsed = json::Parse(r.text);
            const json::Value* rows = parsed ? parsed->Find(L"rows") : nullptr;
            CHECK(!r.isError && rows && rows->arr.size() == 2 && !lastFull && factsCalls == 1, "the database's rows: one BRIEF facts read for all of them");
            if (rows && rows->arr.size() == 2 && rows->arr[0].members.size() >= 6)
            {
                const auto& m = rows->arr[0].members;
                CHECK(m[3].first == L"status" && m[3].second.str == L"waiting for you" && m[4].first == L"age" && m[4].second.str == L"5m" && m[5].first == L"active",
                      "the database's rows: an open session's status and MOST RECENT age, put in before its `active` time");
                CHECK(rows->arr[1].StrAt(L"status") == L"closed" && rows->arr[1].Find(L"age") == nullptr && rows->arr[1].I64At(L"msgs") == 1,
                      "the database's rows: a closed session says closed (the database's own `active` is its time), every other field kept");
            }
        }
    }

    /// <summary>The engine's answered fan-out on a local Engine.</summary>
    void TestEngineFanOut()
    {
        Engine e;
        // Window A answers from another thread, late and twice; window B answers at once.
        const auto answer = [](const std::wstring& name, bool hostsDeactivate) {
            return [name, hostsDeactivate](const WindowToolRequest& request, std::function<void(WindowToolOutcome)> reply) {
                WindowToolOutcome o;
                o.ok = true;
                switch (request.kind)
                {
                case WindowToolRequest::Kind::Census:
                    o.handled = true;
                    o.census.windowId = L"wrong"; // the engine must use the sink's own id
                    o.census.title = name;
                    break;
                case WindowToolRequest::Kind::Deactivate:
                    o.handled = hostsDeactivate;
                    o.message = name;
                    break;
                default:
                    o.handled = true;
                    o.sessionId = name;
                    break;
                }
                if (name == L"A")
                {
                    std::thread([reply, o] {
                        ::Sleep(30);
                        reply(o);
                        reply(o); // a second answer is ignored
                    }).detach();
                }
                else
                {
                    reply(o);
                }
            };
        };
        const uint64_t tokA = RegisterWindowAgentToolHandlerIn(e, kWinA, answer(L"A", false));
        const uint64_t tokB = RegisterWindowAgentToolHandlerIn(e, kWinB, answer(L"B", true));
        CHECK(tokA != 0 && tokB != 0 && tokA != tokB && RegisterWindowAgentToolHandlerIn(e, L"x", {}) == 0, "register: non-zero distinct tokens; an empty handler registers nothing");
        {
            std::lock_guard<std::mutex> lk(e.launchMutex);
            e.lastActiveWindowId = kWinB;
        }
        auto census = CollectWindowCensusIn(e, 3000);
        CHECK(census.size() == 2 && census[0].windowId == kWinA && census[0].title == L"A" && census[1].windowId == kWinB && census[1].title == L"B",
              "census: both windows, in registration order, each with its sink's own window id");
        CHECK(census.size() == 2 && !census[0].lastActive && census[1].lastActive, "census: the last-activated window is flagged by the engine");

        // A window that never answers is left out after the timeout; the others are not lost.
        const uint64_t tokC = RegisterWindowAgentToolHandlerIn(e, L"{C}", [](const WindowToolRequest&, std::function<void(WindowToolOutcome)>) {});
        auto t0 = std::chrono::steady_clock::now();
        census = CollectWindowCensusIn(e, 300);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(census.size() == 2 && ms >= 250 && ms < 2500, "census: a silent window costs the timeout and is left out");
        UnregisterWindowAgentToolHandlerIn(e, tokC);
        UnregisterWindowAgentToolHandlerIn(e, tokC); // a second detach is a no-op

        // A sink that throws counts as answered at once.
        const uint64_t tokD = RegisterWindowAgentToolHandlerIn(e, L"{D}", [](const WindowToolRequest&, std::function<void(WindowToolOutcome)>) { throw std::runtime_error("dead dispatcher"); });
        t0 = std::chrono::steady_clock::now();
        census = CollectWindowCensusIn(e, 5000);
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(census.size() == 2 && ms < 2000, "census: a throwing sink is counted at once (no wait for its timeout)");
        UnregisterWindowAgentToolHandlerIn(e, tokD);

        WindowToolRequest open;
        open.kind = WindowToolRequest::Kind::Fork;
        auto one = RunWindowToolIn(e, kWinA, open, 3000);
        CHECK(one && one->sessionId == L"A", "RunWindowTool: only the named window answers (a late answer from another thread is waited for)");
        CHECK(!RunWindowToolIn(e, L"{gone}", open, 300) && !RunWindowToolIn(e, L"", open, 300), "RunWindowTool: an unknown or empty window is nullopt, never a broadcast");

        WindowToolRequest park;
        park.kind = WindowToolRequest::Kind::Deactivate;
        park.sessionId = kS1;
        auto host = RunWindowToolOnHostIn(e, park, 3000);
        CHECK(host && host->handled && host->message == L"B", "RunWindowToolOnHost: the hosting window's answer");
        UnregisterWindowAgentToolHandlerIn(e, tokB);
        t0 = std::chrono::steady_clock::now();
        host = RunWindowToolOnHostIn(e, park, 3000);
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!host && ms < 2500, "RunWindowToolOnHost: nobody hosts it -> nullopt as soon as every window said so");
        UnregisterWindowAgentToolHandlerIn(e, tokA);
        CHECK(CollectWindowCensusIn(e, 300).empty(), "census: no window registered -> empty");
        ::Sleep(80); // let A's detached answer threads finish before the Engine goes away (they hold only the fan-out state)
    }

    /// <summary>The section of `prompt` between `from` and `to` ("" when either is missing or out of order).</summary>
    std::wstring Section(const std::wstring& prompt, const std::wstring& from, const std::wstring& to)
    {
        const size_t a = prompt.find(from);
        const size_t b = to.empty() ? prompt.size() : prompt.find(to);
        return (a == std::wstring::npos || b == std::wstring::npos || b < a) ? std::wstring{} : prompt.substr(a, b - a);
    }

    /// <summary>How many numbered items "\nN. " a section has, counted 1, 2, 3, ... until the first one missing.</summary>
    int CountNumbered(const std::wstring& section)
    {
        int n = 0;
        while (section.find(L"\n" + std::to_wstring(n + 1) + L". ") != std::wstring::npos)
        {
            ++n;
        }
        return n;
    }

    /// <summary>The two system prompts' structure (the user's request of 2026-10-17).</summary>
    void TestPrompts()
    {
        const std::wstring page = SessionSearchToolset::SystemPrompt();
        const std::wstring db = SessionSearchToolset::DatabaseSystemPrompt();
        for (const auto* prompt : { &page, &db })
        {
            const std::wstring& p = *prompt;
            const size_t h1 = p.find(L"\n# Instructional Behaviors\n");
            const size_t hp = p.find(L"\n## The Search Procedure\n");
            const size_t h2 = p.find(L"\n## Tools\n");
            const size_t h3 = p.find(L"\n## Workflow Guidelines\n");
            CHECK(h1 != std::wstring::npos && hp != std::wstring::npos && h2 != std::wstring::npos && h3 != std::wstring::npos && h1 < hp && hp < h2 && h2 < h3,
                  "prompt: # Instructional Behaviors, then ## The Search Procedure (2026-10-18), then ## Tools, then ## Workflow Guidelines");
            const std::wstring behaviors = Section(p, L"# Instructional Behaviors", L"## The Search Procedure");
            const std::wstring procedure = Section(p, L"## The Search Procedure", L"## Tools");
            const std::wstring workflows = Section(p, L"## Workflow Guidelines", L"");
            CHECK(CountNumbered(behaviors) == 10 && behaviors.find(L"\n11. ") == std::wstring::npos, "prompt: ten numbered behaviors");
            // The procedure the prompt eval of 2026-10-18 tuned (SESSIONS.md 9.15): seven steps, completeness first.
            CHECK(CountNumbered(procedure) == 7 && procedure.find(L"\n8. ") == std::wstring::npos, "prompt: seven numbered steps of the Search Procedure");
            CHECK(p.find(L"Your first duty is COMPLETENESS") != std::wstring::npos && procedure.find(L"Cast a wide net") != std::wstring::npos &&
                      procedure.find(L"Make every list complete before you judge it") != std::wstring::npos && procedure.find(L"Sort the candidates by their evidence") != std::wstring::npos &&
                      procedure.find(L"Before you show, check:") != std::wstring::npos && procedure.find(L"in ONE show call") != std::wstring::npos,
                  "prompt: the procedure's steps - read the facets, cast a wide net, make every list complete, sort by evidence, check, show all in one call, reply");
            CHECK(procedure.find(L"never leave a candidate out on a guess") != std::wstring::npos && procedure.find(L"Your candidates are the union of EVERY list you got") != std::wstring::npos &&
                      procedure.find(L"never a sample or one representative") != std::wstring::npos && procedure.find(L"The user's words count in whatever form they wrote them") != std::wstring::npos,
                  "prompt: the eval's rules - no candidate left out on a guess, the union of every list, never a sample of similar sessions, the user's words in any form");
            CHECK(CountNumbered(workflows) == 19 && workflows.find(L"\n20. ") == std::wstring::npos, "prompt: nineteen numbered workflows");
            CHECK(behaviors.find(L"\n1. Be descriptive and helpful, never hasty.") != std::wstring::npos && behaviors.find(L"before your first tool call and in the same message") != std::wstring::npos &&
                      behaviors.find(L"I'll 1. ") != std::wstring::npos,
                  "prompt: behavior 1 is the plan: \"I'll 1. ... 2. ... 3.\" before the first tool call, in the same message (no extra model request)");
            CHECK(behaviors.find(L"refer to them by number") != std::wstring::npos && behaviors.find(L"Offer the likely next step") != std::wstring::npos && behaviors.find(L"only one tab per request") != std::wstring::npos,
                  "prompt: the reply refers to the shown sessions by number, offers the next step; one deactivate per request");
            CHECK(workflows.find(L"\n12. The window:") != std::wstring::npos && workflows.find(L"\n13. The place in the window:") != std::wstring::npos && workflows.find(L"\n14. Deactivate a tab") != std::wstring::npos,
                  "prompt: the window choice, the placement and deactivate have workflows of their own");
            bool tabsNamed = true;
            for (const auto* name : { L"mcp__tabs__overview", L"mcp__tabs__status", L"mcp__tabs__list", L"mcp__tabs__resume", L"mcp__tabs__fork", L"mcp__tabs__deactivate", L"mcp__display__show" })
            {
                tabsNamed = tabsNamed && p.find(name) != std::wstring::npos;
            }
            CHECK(tabsNamed, "prompt: names show and the six tabs tools");
            // SESSIONS.md 9.16 (the user's rule of 2026-10-24): overview whenever a window, a tab or an open session is
            // mentioned; status for up to 32 sessions; both take one value or a list; search rows carry status and age.
            CHECK(p.find(L"Call it FIRST whenever the user mentions a window, a tab or an open session.") != std::wstring::npos && p.find(L"up to 32 sessions") != std::wstring::npos &&
                      p.find(L"each take one value or a list") != std::wstring::npos && p.find(L"ids takes one id (one object back) or a list (a list back)") != std::wstring::npos &&
                      workflows.find(L"\n15. A request that mentions a window, a tab or an open session") != std::wstring::npos && workflows.find(L"call overview (narrowed with window, dir or state") != std::wstring::npos &&
                      workflows.find(L"call status for the sessions your answer is about") != std::wstring::npos && workflows.find(L"right now, call overview (workflow 15)") != std::wstring::npos &&
                      p.find(L"its status (overview's words for a session open in a tab, else closed)") != std::wstring::npos,
                  "prompt: overview first for a window, a tab or an open session (workflows 5 and 15), status for up to 32, one value or a list, the rows' status");
            bool ascii = !p.empty();
            for (const wchar_t c : p)
            {
                ascii = ascii && (c == L'\n' || (c >= 0x20 && c < 0x7F));
            }
            CHECK(ascii && p.size() < 30000 && p.find(L"on the screen") == std::wstring::npos && p.find(L"indexing") != std::wstring::npos,
                  "prompt: ASCII, under MSVC's wide-literal limit, never \"on the screen\" (a phone has none), the indexing note kept");
        }
        CHECK(page.find(L"mcp__sessions__search") != std::wstring::npos && page.find(L"mcp__sessions__grep") != std::wstring::npos && page.find(L"mcp__sessions__read") != std::wstring::npos &&
                  page.find(L"deep:true") != std::wstring::npos && page.find(L"mcp__agentmaster__") == std::wstring::npos,
              "page prompt: the sessions server's tools and deep:true, no database tool");
        bool allNamed = true;
        for (const auto& t : SessionDbToolDefs::kTools)
        {
            std::wstring name;
            for (const char* c = t.name; *c; ++c)
            {
                name.push_back(static_cast<wchar_t>(*c));
            }
            allNamed = allNamed && db.find(L"mcp__agentmaster__" + name) != std::wstring::npos;
        }
        CHECK(allNamed && db.find(L"mcp__agentmaster__read") != std::wstring::npos && db.find(L"mcp__sessions__") == std::wstring::npos && db.find(L"deep") == std::wstring::npos,
              "database prompt: every database tool and its own read, no page-search tool, and never the word deep");
        CHECK(db.find(L"who_touched with") != std::wstring::npos && db.find(L"timeline for the period") != std::wstring::npos && page.find(L"who_touched") == std::wstring::npos,
              "the workflows that differ by mode use each mode's own tools");
    }

    /// <summary>
    /// What "## Tools" teaches about matching and patterns (the user's review of 2026-10-18: "grep various forms of what
    /// we are looking for"), and that it is TRUE of the code: the rules against MatchesQueryText / SearchIndexFast, each
    /// regex example against GrepRequiredLiteral (the page grep's narrowing key), the per-mode costs, the workflows and
    /// the tabs advice that use them, and the page tools' schemas that repeat them at call time.
    /// </summary>
    void TestToolsTeaching()
    {
        constexpr auto npos = std::wstring::npos;
        const std::wstring page = SessionSearchToolset::SystemPrompt();
        const std::wstring db = SessionSearchToolset::DatabaseSystemPrompt();
        for (const auto* prompt : { &page, &db })
        {
            const std::wstring tools = Section(*prompt, L"\n## Tools\n", L"\n## Workflow Guidelines\n");
            const std::wstring workflows = Section(*prompt, L"\n## Workflow Guidelines\n", L"");
            CHECK(tools.find(L"a word also matches inside longer words") != npos && tools.find(L"drag finds dragging") != npos && tools.find(L"separate searches") != npos &&
                      tools.find(L"in the same step") != npos,
                  "tools: any case, a word inside longer words (one stem covers its forms), other words for it as separate searches of one step");
            CHECK(tools.find(L"ONE kind of place") != npos && tools.find(L"put a folder in dir") != npos,
                  "tools: a query's words all match in ONE kind of place, so a folder goes in dir");
            CHECK(tools.find(L"Patterns for grep.") != npos && tools.find(L"drag(ged|ging|s)?") != npos && tools.find(L"tab[ _-]?drag") != npos && tools.find(L"drag.{0,40}crash") != npos &&
                      tools.find(L"source.agentmaster") != npos,
                  "tools: Patterns for grep, with the forms (endings and spellings, separators, nearness, either slash)");
            CHECK(tools.find(L"outside every group and alternative") != npos && tools.find(L"(dragged|dragging)") != npos && tools.find(L"separate grep calls in the same step") != npos &&
                      tools.find(L"0\\.6\\.8") != npos,
                  "tools: the plain piece outside every group, the examples that have none, separate greps for unrelated words, escaping");
            CHECK(tools.find(L"pass after as a session id") != npos && tools.find(L"Before you open a tab, list the window it goes in") != npos,
                  "tools: list the target window before an open, and pass after as a session id when opening several (every pos after a new tab shifts)");
            CHECK(workflows.find(L"in the same step with other words for it") != npos && workflows.find(L"one regex for all its forms") != npos && workflows.find(L"try a shorter stem") != npos,
                  "workflows 1, 2 and 8 use the matching rules and the patterns");
        }
        const std::wstring pageTools = Section(page, L"\n## Tools\n", L"\n## Workflow Guidelines\n");
        const std::wstring dbTools = Section(db, L"\n## Tools\n", L"\n## Workflow Guidelines\n");
        CHECK(pageTools.find(L"The git branch is not among them") != npos && pageTools.find(L"20 s budget") != npos && pageTools.find(L"narrow it with dir or a period") != npos,
              "page tools: the branch is only a filter, a pattern with no plain piece reads every session (20 s budget), deep is narrowed first");
        CHECK(dbTools.find(L"is refused unless you name the sessions in ids") != npos && dbTools.find(L"RE2") != npos && dbTools.find(L"fuzzy:true") != npos &&
                  dbTools.find(L"cursor") != npos && dbTools.find(L"case_sensitive:true") != npos,
              "database tools: a regex with no plain piece is refused without ids; RE2, fuzzy, cursor and case_sensitive are taught");

        // The code behind the matching rules (the page search's matcher and index; the database's search folds and
        // matches inside index terms the same way, SessionDbSearch.cpp, which the service's own tests cover).
        CHECK(MatchesQueryText(L"dragging the tab", L"drag", false) && MatchesQueryText(FoldLower(L"SessionScanner.cpp"), L"scanner", false) &&
                  !MatchesQueryText(L"tab drag", L"tab-drag", false),
              "code: a word matches inside longer words, case folded; a phrase matches only as written");
        {
            std::vector<SessionIndexEntry> entries(1);
            entries[0].sessionId = L"s-branch";
            entries[0].stats.customTitle = L"Polish the overlay";
            entries[0].stats.cwd = L"K:\\source\\Agentmaster";
            entries[0].stats.gitBranch = L"feature-tooltip";
            SessionQuery q; // the page search's index query, as RunSessionSearch builds it
            q.scopeTitle = true;
            q.scopeDirs = true;
            q.scopeFiles = true;
            q.text = L"tooltip";
            CHECK(SearchIndexFast(entries, q).empty(), "code: the page's query never matches the git branch (only the branch filter does), as its Tools text says");
            q.text = L"agentmaster overlay";
            CHECK(SearchIndexFast(entries, q).size() == 1, "code: the words of one query may hit different names and places (the folder and the title)");
            q.text = L"agentmaster tooltip";
            CHECK(SearchIndexFast(entries, q).empty(), "code: ... but every word must hit one of them: \"agentmaster tooltip\" misses, the prompt's own example");
        }
        // Each regex example of "Patterns for grep" against the page grep's own reader: the fixed piece the prompt
        // promises ("drag(ged|ging)? has one (drag)") is the key the grep narrows by, and the bad examples have none.
        CHECK(GrepRequiredLiteral(L"drag(ged|ging|s)?", true) == L"drag" && GrepRequiredLiteral(L"(re)?compil(e|ed|er|ing)", true) == L"compil" &&
                  GrepRequiredLiteral(L"colou?r", true) == L"colo" && GrepRequiredLiteral(L"tab[ _-]?drag", true) == L"drag" &&
                  GrepRequiredLiteral(L"drag.{0,40}crash", true) == L"crash" && GrepRequiredLiteral(L"source.agentmaster", true) == L"agentmaster" &&
                  GrepRequiredLiteral(LR"(0\.6\.8)", true) == L"0.6.8",
              "code: every good example of Patterns for grep keeps a plain piece that the grep narrows by");
        CHECK(GrepRequiredLiteral(L"(dragged|dragging)", true).empty() && GrepRequiredLiteral(L"dragged|dragging", true).empty(),
              "code: ... and the bad examples (a group alone, a top-level |) have none, so they read every session");

        // The page tools' schemas and descriptions repeat the rules where the model reads them at call time.
        SessionSearchToolset::Callbacks cb;
        cb.catalog = [](int64_t, const std::function<bool()>&) { return std::shared_ptr<const SessionCatalog>{}; };
        cb.onShow = [](const std::vector<std::wstring>&, const std::wstring&, const std::vector<std::wstring>&) {};
        SessionSearchToolset toolset{ cb };
        const auto servers = toolset.Servers();
        const HarnessTool* search = FindTool(servers, L"sessions", L"search");
        const HarnessTool* grep = FindTool(servers, L"sessions", L"grep");
        CHECK(search && grep, "the page toolset has search and grep");
        if (search && grep)
        {
            CHECK(search->inputSchemaJson.find(L"also matches inside longer words") != npos && search->inputSchemaJson.find(L"put a folder in dir and a branch in branch") != npos &&
                      search->description.find(L"inside longer words") != npos,
                  "schema: search's query states the stem, the one-place and the folder / branch rules");
            CHECK(grep->inputSchemaJson.find(L"drag(ged|ging)?") != npos && grep->inputSchemaJson.find(L"A pattern with an alternative that has no such piece is slow") != npos &&
                      grep->inputSchemaJson.find(L"(0xC0000374|heap.?corrupt)") != npos && grep->description.find(L"every form of a word") != npos,
                  "schema: grep's regex and its description state the forms rule and its cost (an alternation whose every alternative keeps a piece is fast)");
            CHECK(search->description.find(L"every session by default") != npos && grep->description.find(L"in:edits") != npos && grep->description.find(L"`also`") != npos,
                  "description: search covers every session by default; grep's in:edits and its `also` list");
        }
    }

    /// <summary>
    /// What the remote launch tests saw: every answer a reply delivered, in order, and how many there were. Shared by the reply
    /// closures through a shared_ptr, because an answer may come from a thread that outlives the test's frame.
    /// </summary>
    struct LaunchAnswersSeen
    {
        std::mutex mutex;
        std::vector<RemoteLaunchOutcome> answers;
        std::atomic<int> count{ 0 };
    };

    /// <summary>A reply that records every answer it is given into `seen` (it never throws).</summary>
    RemoteLaunchReply RecordingReply(const std::shared_ptr<LaunchAnswersSeen>& seen)
    {
        return [seen](const RemoteLaunchOutcome& outcome) {
            std::lock_guard<std::mutex> lk(seen->mutex);
            seen->answers.push_back(outcome);
            seen->count.fetch_add(1);
        };
    }

    /// <summary>Waits up to `timeoutMs` for `seen` to hold at least `atLeast` answers; true when it does.</summary>
    bool WaitForAnswers(const std::shared_ptr<LaunchAnswersSeen>& seen, int atLeast, int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ timeoutMs };
        while (seen->count.load() < atLeast && std::chrono::steady_clock::now() < deadline)
        {
            ::Sleep(5);
        }
        return seen->count.load() >= atLeast;
    }

    /// <summary>The answer `seen` recorded at `index`, copied under its lock (a default answer when there is none).</summary>
    RemoteLaunchOutcome AnswerAt(const std::shared_ptr<LaunchAnswersSeen>& seen, size_t index)
    {
        std::lock_guard<std::mutex> lk(seen->mutex);
        return index < seen->answers.size() ? seen->answers[index] : RemoteLaunchOutcome{};
    }

    /// <summary>
    /// MESSAGING.md §21.23: the remote launch's ANSWER — RemoteLaunch.h's outcome rules and subject, and the engine's answered
    /// launch routing on a local Engine (RequestLaunchInWindowIn), every window a fake sink.
    /// </summary>
    void TestRemoteLaunchAnswers()
    {
        std::wprintf(L"Remote launch answers (MESSAGING.md 21.23 - the outcome rules, the phone's reasons, the subject, the engine's answered routing):\n");
        using R = RemoteLaunchResult;

        // ----- the outcome rules (RemoteLaunch.h, pure) -----
        RemoteLaunchOutcome o;
        CHECK(o.result == R::Failed && !RemoteLaunchOpened(o), "outcome: a default answer is a failure, never a success");
        o.result = R::Opened;
        CHECK(RemoteLaunchOpened(o) && RemoteLaunchRefusalText(o).empty(), "outcome: Opened is a success and needs no reason");
        o.result = R::AlreadyOpen;
        CHECK(RemoteLaunchOpened(o) && RemoteLaunchRefusalText(o).empty(), "outcome: AlreadyOpen is a success and needs no reason");
        {
            const R all[] = { R::Opened, R::AlreadyOpen, R::WindowClosed, R::ClaudeMissing, R::PresetRefused, R::NoFolder, R::Failed };
            std::vector<std::wstring> names;
            bool refusalsClosed = true;
            for (const R r : all)
            {
                names.push_back(RemoteLaunchResultName(r));
                if (r != R::Opened && r != R::AlreadyOpen)
                {
                    RemoteLaunchOutcome x;
                    x.result = r;
                    refusalsClosed = refusalsClosed && !RemoteLaunchOpened(x);
                }
            }
            std::vector<std::wstring> sorted = names;
            std::sort(sorted.begin(), sorted.end());
            CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end() && names[0] == L"opened" && names[1] == L"already-open" && names[3] == L"claude-missing" && names[6] == L"failed",
                  "outcome: one distinct, stable log word per result");
            CHECK(refusalsClosed, "outcome: every result but Opened and AlreadyOpen means nothing opened");
        }
        o = RemoteLaunchOutcome{};
        o.result = R::ClaudeMissing;
        std::wstring why = RemoteLaunchRefusalText(o);
        CHECK(why.find(L"claude.exe") != std::wstring::npos && why.find(L"Settings") != std::wstring::npos && why.back() == L'.',
              "reason: no claude.exe names the binary and the two fixes, as a whole sentence");
        o.result = R::PresetRefused;
        o.detail = L"  The preset \x201CKimi K3\x201D has no API key  \r\n";
        why = RemoteLaunchRefusalText(o);
        CHECK(why.rfind(L"The preset \x201CKimi K3\x201D has no API key. Nothing was started", 0) == 0 && why.find(L"never falls back to your Claude login.") != std::wstring::npos,
              "reason: a refused preset gives the preset's own words (trimmed, ended with a period) and the PC dialog's rule");
        o.detail.clear();
        CHECK(RemoteLaunchRefusalText(o).rfind(L"Its launch preset cannot start. Nothing was started", 0) == 0, "reason: a refused preset without words still says what refused");
        o.result = R::Failed;
        CHECK(RemoteLaunchRefusalText(o).find(L"hooks.log") != std::wstring::npos, "reason: a failure without words points at the PC's hooks.log");
        o.detail = L"The PC could not create the terminal tab.";
        CHECK(RemoteLaunchRefusalText(o) == L"The PC could not create the terminal tab.", "reason: a failure with words is those words, as they are");
        o.detail = L"Is it there?";
        CHECK(RemoteLaunchRefusalText(o) == L"Is it there?", "reason: words that end a sentence get no extra period");
        o.detail = L"half a sentence";
        CHECK(RemoteLaunchRefusalText(o) == L"half a sentence.", "reason: words without an end get a period");
        o = RemoteLaunchOutcome{};
        o.result = R::WindowClosed;
        CHECK(RemoteLaunchRefusalText(o).find(L"closed") != std::wstring::npos, "reason: a window that closed says so");
        o.result = R::NoFolder;
        CHECK(RemoteLaunchRefusalText(o).find(L"folder") != std::wstring::npos, "reason: no folder says so");

        // ----- the subject (RemoteLaunchSubject) -----
        RemoteLaunchRequest q;
        q.kind = RemoteLaunchRequest::Kind::Resume;
        q.sessionId = kS1;
        q.title = L"BetterClipboard\ron-launch"; // the lone CR of the tab's rename box
        CHECK(RemoteLaunchSubject(q, true) == L"The resume of \x201C" L"BetterClipboard on-launch\x201D", "subject: a resume, its title on one line (a lone CR is a space)");
        q.kind = RemoteLaunchRequest::Kind::Fork;
        q.title = L"\r\na \t\r\n\r\n  b\n";
        CHECK(RemoteLaunchSubject(q, false) == L"the fork of \x201C" L"a b\x201D", "subject: a fork mid-sentence; a run of breaks with its blanks is one space, a break at an end is nothing");
        q.title.clear();
        CHECK(RemoteLaunchSubject(q, true) == L"The fork of \x201C" L"11111111\x201D", "subject: no title -> the session's short id");
        q.kind = RemoteLaunchRequest::Kind::NewSession;
        q.dir = L"K:\\source\\BetterClipboard\\";
        CHECK(RemoteLaunchSubject(q, true) == L"The new session in BetterClipboard", "subject: a new session names its folder's last name (a trailing separator dropped)");
        q.dir = L"/home/eli/proj";
        CHECK(RemoteLaunchSubject(q, false) == L"the new session in proj", "subject: forward slashes too");
        q.dir = L"C:\\";
        CHECK(RemoteLaunchSubject(q, true) == L"The new session in C:\\", "subject: a drive root keeps its path");
        q.dir.clear();
        CHECK(RemoteLaunchSubject(q, true) == L"The new session in its folder", "subject: no folder at all");

        // ----- the engine's answered routing (RequestLaunchInWindowIn on a local Engine) -----
        Engine e;
        RemoteLaunchRequest req;
        req.kind = RemoteLaunchRequest::Kind::NewSession;
        req.dir = L"K:\\source\\BetterClipboard";
        req.newSessionId = kS7;

        // No window at all: false, and no answer ever.
        auto seen = std::make_shared<LaunchAnswersSeen>();
        CHECK(!RequestLaunchInWindowIn(e, req, RecordingReply(seen)) && seen->count.load() == 0, "routing: no window -> false, and no answer");

        // An inline answer (the window answers before the call returns).
        const auto answerWith = [](R result, const std::wstring& sessionId, const std::wstring& windowId) {
            return [result, sessionId, windowId](const RemoteLaunchRequest&, RemoteLaunchReply reply) -> bool {
                RemoteLaunchOutcome out;
                out.result = result;
                out.sessionId = sessionId;
                out.windowId = windowId;
                reply(out);
                return true;
            };
        };
        const uint64_t tokInline = RegisterWindowLaunchHandlerIn(e, kWinA, answerWith(R::Opened, kS7, kWinA));
        CHECK(tokInline != 0 && RegisterWindowLaunchHandlerIn(e, kWinB, {}) == 0, "routing: a sink registers with a token; an empty handler registers nothing");
        seen = std::make_shared<LaunchAnswersSeen>();
        CHECK(RequestLaunchInWindowIn(e, req, RecordingReply(seen)) && seen->count.load() == 1 && AnswerAt(seen, 0).result == R::Opened && AnswerAt(seen, 0).sessionId == kS7,
              "routing: an inline answer is delivered once, before the call returns");
        UnregisterWindowLaunchHandlerIn(e, tokInline);
        UnregisterWindowLaunchHandlerIn(e, tokInline); // a second detach is a no-op

        // A late answer from another thread, given twice: only the first is delivered.
        const uint64_t tokLate = RegisterWindowLaunchHandlerIn(e, kWinA, [](const RemoteLaunchRequest&, RemoteLaunchReply reply) -> bool {
            std::thread([reply] {
                ::Sleep(30);
                RemoteLaunchOutcome first;
                first.result = R::Opened;
                first.sessionId = kS7;
                reply(first);
                RemoteLaunchOutcome second;
                second.result = R::Failed;
                reply(second); // ignored: the answer is settled
            }).detach();
            return true;
        });
        seen = std::make_shared<LaunchAnswersSeen>();
        CHECK(RequestLaunchInWindowIn(e, req, RecordingReply(seen)), "routing: a window that answers later takes the request");
        CHECK(WaitForAnswers(seen, 1, 3000) && AnswerAt(seen, 0).result == R::Opened, "routing: the late answer arrives from the window's thread");
        ::Sleep(80);
        CHECK(seen->count.load() == 1, "routing: a second answer to the same request is ignored");
        UnregisterWindowLaunchHandlerIn(e, tokLate);

        // A window that drops the request without running it (a closing window's queue): the engine answers WindowClosed when the
        // last copy of the reply dies.
        const uint64_t tokDrop = RegisterWindowLaunchHandlerIn(e, kWinA, [](const RemoteLaunchRequest&, RemoteLaunchReply reply) -> bool {
            auto kept = std::make_shared<RemoteLaunchReply>(std::move(reply)); // the "work item"
            std::thread([kept]() mutable {
                ::Sleep(30);
                kept.reset(); // dropped unrun: the last copy dies here
            }).detach();
            return true;
        });
        seen = std::make_shared<LaunchAnswersSeen>();
        CHECK(RequestLaunchInWindowIn(e, req, RecordingReply(seen)), "routing: a window that will drop the request still took it");
        CHECK(WaitForAnswers(seen, 1, 3000) && AnswerAt(seen, 0).result == R::WindowClosed && seen->count.load() == 1,
              "routing: a request dropped unanswered is answered WindowClosed by the engine, once");
        UnregisterWindowLaunchHandlerIn(e, tokDrop);

        // The last-activated window first; a sink that refuses, and one that throws, are skipped for the next window.
        auto asked = std::make_shared<std::vector<std::wstring>>();
        auto askedMutex = std::make_shared<std::mutex>();
        const auto noteAsked = [asked, askedMutex](const std::wstring& who) {
            std::lock_guard<std::mutex> lk(*askedMutex);
            asked->push_back(who);
        };
        const uint64_t tokA = RegisterWindowLaunchHandlerIn(e, kWinA, [noteAsked](const RemoteLaunchRequest&, RemoteLaunchReply reply) -> bool {
            noteAsked(L"A");
            RemoteLaunchOutcome out;
            out.result = R::Opened;
            out.sessionId = kS7;
            out.windowId = kWinA;
            reply(out);
            return true;
        });
        const uint64_t tokThrow = RegisterWindowLaunchHandlerIn(e, L"{THROW}", [noteAsked](const RemoteLaunchRequest&, RemoteLaunchReply) -> bool {
            noteAsked(L"throw");
            throw std::runtime_error("dead dispatcher");
        });
        const uint64_t tokRefuse = RegisterWindowLaunchHandlerIn(e, kWinB, [noteAsked](const RemoteLaunchRequest&, RemoteLaunchReply reply) -> bool {
            noteAsked(L"B");
            // A refusing sink that keeps a copy of the reply and drops it later: the attempt is disarmed, so it never answers.
            auto kept = std::make_shared<RemoteLaunchReply>(std::move(reply));
            std::thread([kept]() mutable {
                ::Sleep(20);
                kept.reset();
            }).detach();
            return false;
        });
        {
            std::lock_guard<std::mutex> lk(e.launchMutex);
            e.lastActiveWindowId = kWinB; // the refusing window was used last: it is asked first
        }
        seen = std::make_shared<LaunchAnswersSeen>();
        CHECK(RequestLaunchInWindowIn(e, req, RecordingReply(seen)), "routing: the request is taken by a window after the last-active one refused");
        ::Sleep(80); // the refusing sink's copy dies meanwhile: it must not answer
        {
            std::lock_guard<std::mutex> lk(*askedMutex);
            CHECK(asked->size() == 2 && (*asked)[0] == L"B" && (*asked)[1] == L"A", "routing: the last-activated window first, then the others in registration order");
        }
        CHECK(seen->count.load() == 1 && AnswerAt(seen, 0).windowId == kWinA, "routing: exactly one answer, from the window that took it; the refusal never answers");

        // Every sink refusing or throwing: false, and no answer even after the refusing sink's copy of the reply died.
        UnregisterWindowLaunchHandlerIn(e, tokA);
        {
            std::lock_guard<std::mutex> lk(*askedMutex);
            asked->clear();
        }
        seen = std::make_shared<LaunchAnswersSeen>();
        CHECK(!RequestLaunchInWindowIn(e, req, RecordingReply(seen)), "routing: every window refusing or throwing -> false");
        ::Sleep(80);
        {
            std::lock_guard<std::mutex> lk(*askedMutex);
            CHECK(asked->size() == 2 && (*asked)[0] == L"B" && (*asked)[1] == L"throw", "routing: every window is asked before false");
        }
        CHECK(seen->count.load() == 0, "routing: a launch no window took is never answered");
        UnregisterWindowLaunchHandlerIn(e, tokThrow);
        UnregisterWindowLaunchHandlerIn(e, tokRefuse);

        // A requester whose reply throws is contained (Rule #18: logged into the scratch profile's hooks.log), and an empty reply
        // is allowed (the launch runs, its answer goes nowhere).
        const uint64_t tokOk = RegisterWindowLaunchHandlerIn(e, kWinA, answerWith(R::ClaudeMissing, L"", kWinA));
        bool threw = false;
        try
        {
            CHECK(RequestLaunchInWindowIn(e, req, [](const RemoteLaunchOutcome&) { throw std::runtime_error("a requester that throws"); }),
                  "routing: a throwing requester still gets its request taken");
            CHECK(RequestLaunchInWindowIn(e, req, RemoteLaunchReply{}), "routing: an empty reply is allowed");
        }
        catch (...)
        {
            threw = true;
        }
        CHECK(!threw, "routing: nothing a requester's reply throws reaches the caller");
        UnregisterWindowLaunchHandlerIn(e, tokOk);
        CHECK(!RequestLaunchInWindowIn(e, req, RecordingReply(std::make_shared<LaunchAnswersSeen>())), "routing: no window left -> false");

        // ----- MESSAGING.md §21.25: a request that NAMES its window (the new-thread wizard's window step) -----
        {
            auto namedAsked = std::make_shared<std::vector<std::wstring>>();
            auto namedMutex = std::make_shared<std::mutex>();
            // A sink that records who was asked and with which anchor session, and answers Opened from its own window.
            const auto recordingSink = [namedAsked, namedMutex](const std::wstring& who) {
                return [namedAsked, namedMutex, who](const RemoteLaunchRequest& r, RemoteLaunchReply reply) -> bool {
                    {
                        std::lock_guard<std::mutex> lk(*namedMutex);
                        namedAsked->push_back(who + L"|" + r.afterSessionId);
                    }
                    RemoteLaunchOutcome out;
                    out.result = R::Opened;
                    out.sessionId = kS7;
                    out.windowId = who;
                    reply(out);
                    return true;
                };
            };
            const uint64_t tokNamedA = RegisterWindowLaunchHandlerIn(e, kWinA, recordingSink(kWinA));
            const uint64_t tokNamedB = RegisterWindowLaunchHandlerIn(e, kWinB, recordingSink(kWinB));
            {
                std::lock_guard<std::mutex> lk(e.launchMutex);
                e.lastActiveWindowId = kWinA; // the OTHER window was used last
            }
            RemoteLaunchRequest named = req;
            named.windowId = kWinB;
            named.afterSessionId = kS1;
            seen = std::make_shared<LaunchAnswersSeen>();
            CHECK(RequestLaunchInWindowIn(e, named, RecordingReply(seen)) && seen->count.load() == 1 && AnswerAt(seen, 0).windowId == kWinB,
                  "named window: that window takes it, though another one was used last");
            {
                std::lock_guard<std::mutex> lk(*namedMutex);
                CHECK(namedAsked->size() == 1 && (*namedAsked)[0] == kWinB + L"|" + kS1, "named window: only it was asked, and the request reached it whole (its anchor session too)");
            }
            named.windowId = L"{GONE-0000}";
            seen = std::make_shared<LaunchAnswersSeen>();
            CHECK(!RequestLaunchInWindowIn(e, named, RecordingReply(seen)) && seen->count.load() == 0,
                  "named window that is not open: false and no answer (the wizard asks again) - never another window");
            {
                std::lock_guard<std::mutex> lk(*namedMutex);
                CHECK(namedAsked->size() == 1, "named window that is not open: no other window was asked");
            }
            named.windowId.clear();
            seen = std::make_shared<LaunchAnswersSeen>();
            CHECK(RequestLaunchInWindowIn(e, named, RecordingReply(seen)) && AnswerAt(seen, 0).windowId == kWinA, "no name: the window used last, as before");
            UnregisterWindowLaunchHandlerIn(e, tokNamedA);
            UnregisterWindowLaunchHandlerIn(e, tokNamedB);
        }
        ::Sleep(60); // let the detached sink threads finish before the Engine goes away (they hold only their own copies)
    }
}

void TestTabTools()
{
    // The section header every suite prints, so a [FAIL] line in the run's output reads under its suite.
    std::wprintf(L"Window and tab tools (SESSIONS.md 9.14 / 9.16 - the window rules, the tabs server, the live facts, overview and status, the answered fan-out, the system prompts, what ## Tools teaches):\n");
    TestWindowRules();
    TestTabsServer();
    TestLiveFactsRules(); // SESSIONS.md 9.16: the live-facts rules behind status, overview and the search rows
    TestOverviewAndStatus(); // SESSIONS.md 9.16: the overview and status tools, and the rows' status and age in the database mode
    TestEngineFanOut();
    TestPrompts();
    TestToolsTeaching();
    TestRemoteLaunchAnswers(); // MESSAGING.md §21.23: the remote launch's answer (it prints its own header)
}
