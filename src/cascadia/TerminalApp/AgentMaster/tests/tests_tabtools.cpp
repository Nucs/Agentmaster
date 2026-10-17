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
//     and the tabs advice; and the page tools' schemas that repeat it.

#include "m5_tests.h"

#include "../Engine.h"
#include "../SessionDbToolDefs.h"
#include "../SessionSearch.h"
#include "../SessionSearchTools.h"
#include "../WindowTabTools.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
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
        a.tabs.push_back(s1);
        a.tabs.push_back(Tab(1, L"pwsh", L"", L"", false, 0));
        auto s2 = Tab(2, L"catalog", kS2, L"running", true, now - 60000);
        s2.dir = L"K:\\catalog";
        s2.worksIn = L"K:\\catalog\\sysd";
        a.tabs.push_back(s2);
        auto s3 = Tab(3, L"old work", kS3, L"idle", false, now - 30LL * 86400000);
        s3.dir = L"K:\\source\\old";
        a.tabs.push_back(s3);
        auto s4 = Tab(4, L"hand-typed", kS4, L"waiting for you", true, now - 2 * 3600000);
        s4.external = true;
        s4.dir = L"C:\\Users\\ELI";
        a.tabs.push_back(s4);

        WindowCensus b;
        b.windowId = kWinB;
        b.title = L"notes";
        b.lastActive = true;
        auto s5 = Tab(0, L"release", kS5, L"needs approval", true, now - 3 * 60000);
        s5.dir = L"K:\\source\\Agentmaster";
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
            const size_t h2 = p.find(L"\n## Tools\n");
            const size_t h3 = p.find(L"\n## Workflow Guidelines\n");
            CHECK(h1 != std::wstring::npos && h2 != std::wstring::npos && h3 != std::wstring::npos && h1 < h2 && h2 < h3, "prompt: # Instructional Behaviors, then ## Tools, then ## Workflow Guidelines");
            const std::wstring behaviors = Section(p, L"# Instructional Behaviors", L"## Tools");
            const std::wstring workflows = Section(p, L"## Workflow Guidelines", L"");
            CHECK(CountNumbered(behaviors) == 10 && behaviors.find(L"\n11. ") == std::wstring::npos, "prompt: ten numbered behaviors");
            CHECK(CountNumbered(workflows) == 19 && workflows.find(L"\n20. ") == std::wstring::npos, "prompt: nineteen numbered workflows");
            CHECK(behaviors.find(L"\n1. Be descriptive and helpful, never hasty.") != std::wstring::npos && behaviors.find(L"before your first tool call and in the same message") != std::wstring::npos &&
                      behaviors.find(L"I'll 1. ") != std::wstring::npos,
                  "prompt: behavior 1 is the plan: \"I'll 1. ... 2. ... 3.\" before the first tool call, in the same message (no extra model request)");
            CHECK(behaviors.find(L"refer to them by number") != std::wstring::npos && behaviors.find(L"Offer the likely next step") != std::wstring::npos && behaviors.find(L"only one tab per request") != std::wstring::npos,
                  "prompt: the reply refers to the shown sessions by number, offers the next step; one deactivate per request");
            CHECK(workflows.find(L"\n12. The window:") != std::wstring::npos && workflows.find(L"\n13. The place in the window:") != std::wstring::npos && workflows.find(L"\n14. Deactivate a tab") != std::wstring::npos,
                  "prompt: the window choice, the placement and deactivate have workflows of their own");
            bool tabsNamed = true;
            for (const auto* name : { L"mcp__tabs__list", L"mcp__tabs__resume", L"mcp__tabs__fork", L"mcp__tabs__deactivate", L"mcp__display__show" })
            {
                tabsNamed = tabsNamed && p.find(name) != std::wstring::npos;
            }
            CHECK(tabsNamed, "prompt: names show and the four tabs tools");
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
            CHECK(grep->inputSchemaJson.find(L"drag(ged|ging)?") != npos && grep->inputSchemaJson.find(L"outside its groups and alternatives is slow") != npos &&
                      grep->description.find(L"every form of a word") != npos,
                  "schema: grep's regex and its description state the forms rule and its cost");
        }
    }
}

void TestTabTools()
{
    // The section header every suite prints, so a [FAIL] line in the run's output reads under its suite.
    std::wprintf(L"Window and tab tools (SESSIONS.md 9.14 - the window rules, the tabs server, the answered fan-out, the system prompts, what ## Tools teaches):\n");
    TestWindowRules();
    TestTabsServer();
    TestEngineFanOut();
    TestPrompts();
    TestToolsTeaching();
}
