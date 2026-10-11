// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the LIVE test of the agent harness (SDK_HARNESS.md §8).
//
// Unlike m5_tests.exe this program starts the real claude.exe and makes real model calls, so it
// needs a logged-in Claude Code, a network, and it uses a little of the account's quota (about
// twelve short Sonnet requests per run). It is NOT part of the m5 suite; run it by hand with
// run-harness-live.bat after a change to the harness, to the process backend, or to the pin of
// the SDK submodule.
//
// What it proves, end to end, with the same engine files the app compiles:
//   1. the SDK builds the expected command line (DescribeCommandLine), and it asks for BYPASS
//      PERMISSIONS;
//   2. a conversation starts, the process REPORTS bypass before any message, the model calls
//      in-process tools, the turn's events arrive, and the process's own inventory lists exactly
//      the harness tools;
//   3. a follow-up message keeps its context in the same process;
//   4. an idle conversation uses (almost) no CPU — the SDK's own Windows backend spins a core;
//   5. NEWEST WINS: a message sent while a turn runs interrupts that turn;
//   6. the record is written, read back by a new conversation object, and the new process gets
//      the digest (a follow-up still makes sense after a "restart"); a PARK ends the idle
//      process, a Prewarm brings one back, and a Prewarm cancels a pending park;
//   7. Reset raises the epoch, empties the record and deletes its file;
//   8. a missing claude.exe fails fast, with an Error event;
//   P. THE PERMISSION POLICY when bypass is not allowed (made with the test seam, refused by
//      Claude Code itself): P1 a policy switch -> auto; P2 both modes switched off -> dontAsk;
//      P3 an error at start (--restricted) -> a second start with auto, remembered;
//   9. (with --agent) the Sessions search agent finds real sessions of this machine — and the
//      run prints HOW: the command line, the system prompt, the tools as the model reads them,
//      each request step by step (tool calls with their arguments, results, replies, times),
//      and what the process reported about itself (its inventory).
//
// Usage: harness_live.exe [--claude <path to claude.exe>] [--model <id>] [--agent "<request>"]
//                         [--only-agent] [--only-permissions] [--verbose]
//        harness_live.exe --index [days]     measure the session index alone (no model call)
//   --verbose prints each tool result in full (as far as the record keeps it).
// Exit code: the number of failed checks.

#include "../AgentHarness.h"
#include "../ClaudeSpawn.h"
#ifndef AM_LIVE_NO_AGENT // define it to build the harness scenarios alone, without the search agent
#include "../SessionSearchAgent.h"
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace Agentmaster;

namespace
{
    const auto g_start = std::chrono::steady_clock::now();
    int g_failures = 0;
    int g_checks = 0;
    bool g_verbose = false;

    double Seconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    }

    // Print a wide line as UTF-8 (the console and a redirected file both take it).
    void Say(const std::wstring& line)
    {
        const std::string bytes = HarnessToUtf8(line);
        std::printf("%8.3fs  %s\n", Seconds(), bytes.c_str());
        std::fflush(stdout);
    }

    void Check(bool ok, const std::wstring& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
        }
        Say(std::wstring{ ok ? L"PASS  " : L"FAIL  " } + what);
    }

    std::wstring Short(std::wstring text, size_t max = 220)
    {
        for (auto& c : text)
        {
            if (c == L'\n' || c == L'\r')
            {
                c = L' ';
            }
        }
        if (text.size() > max)
        {
            text.resize(max);
            text += L"...";
        }
        return text;
    }

    // "a, b, c" — or "(none)" for an empty list, so that an empty kind is said, not left out.
    std::wstring Join(const std::vector<std::wstring>& items)
    {
        if (items.empty())
        {
            return L"(none)";
        }
        std::wstring out;
        for (const auto& item : items)
        {
            out += (out.empty() ? L"" : L", ") + item;
        }
        return out;
    }

    // Print what a harness process reported about ITSELF (HarnessConversation::Inventory): the
    // rules it runs under and everything its model can use. One line per kind.
    void SayInventory(const HarnessInventory& inventory)
    {
        if (!inventory.known)
        {
            Say(L"  inventory: no process answered yet");
            return;
        }
        Say(L"  inventory  permission mode : " + inventory.permissionMode + L" (asked: " + inventory.askedPermissionMode + L")");
        if (!inventory.complete)
        {
            Say(L"  inventory  (the lists come with the first turn of the process)");
            return;
        }
        std::wstring servers;
        for (const auto& [name, status] : inventory.mcpServers)
        {
            servers += (servers.empty() ? L"" : L", ") + name + L"=" + status;
        }
        Say(L"  inventory  claude code     : " + inventory.cliVersion + L", model " + inventory.model + L", credentials: " + (inventory.apiKeySource == L"none" ? std::wstring{ L"the Claude login (no API key)" } : inventory.apiKeySource) + L", output style: " + inventory.outputStyle);
        Say(L"  inventory  tools (" + std::to_wstring(inventory.tools.size()) + L")       : " + Join(inventory.tools));
        Say(L"  inventory  mcp servers (" + std::to_wstring(inventory.mcpServers.size()) + L") : " + (servers.empty() ? std::wstring{ L"(none)" } : servers));
        Say(L"  inventory  slash commands  : " + Join(inventory.slashCommands));
        Say(L"  inventory  skills          : " + Join(inventory.skills));
        Say(L"  inventory  agent types (" + std::to_wstring(inventory.agents.size()) + L") : " + Join(inventory.agents) + L"   <- they exist; the model can start one only with an Agent tool");
        Say(L"  inventory  plugins (" + std::to_wstring(inventory.plugins.size()) + L")     : " + Join(inventory.plugins));
    }

    // The size of the test profile's hooks.log now: the mark for SayHarnessLogSince.
    uintmax_t HooksLogSize()
    {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(AgentmasterStateDir() + L"\\hooks.log", ec);
        return ec ? 0 : size;
    }

    // Print the [harness] lines that the engine wrote into the test profile's hooks.log after
    // `from` — the harness's own account of what it did (a start, a refused mode, a switch).
    //   from   — a HooksLogSize() taken before the step.
    //   needle — only lines that hold this text; empty = every [harness] line.
    //   returns — the printed lines, joined by '\n' (for a check on what was logged).
    std::wstring SayHarnessLogSince(uintmax_t from, const std::wstring& needle = {})
    {
        std::wstring printed;
        const std::wstring path = AgentmasterStateDir() + L"\\hooks.log";
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return printed;
        }
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(from);
        std::string bytes;
        if (::SetFilePointerEx(h, at, nullptr, FILE_BEGIN))
        {
            char buffer[8192];
            DWORD got = 0;
            while (::ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            {
                bytes.append(buffer, got);
            }
        }
        ::CloseHandle(h);
        const std::wstring text = HarnessFromUtf8(bytes);
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos)
            {
                end = text.size();
            }
            std::wstring line = text.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            if (line.find(L"[harness]") == std::wstring::npos || (!needle.empty() && line.find(needle) == std::wstring::npos))
            {
                continue;
            }
            Say(L"  log  " + line);
            printed += line + L"\n";
        }
        return printed;
    }

    // Wait until `conversation` is Ready or Failed.
    //   returns — true for Ready.
    bool WaitReady(HarnessConversation& conversation, int seconds = 20)
    {
        for (int i = 0; i < seconds * 20; ++i)
        {
            const HarnessState state = conversation.State();
            if (state == HarnessState::Ready)
            {
                return true;
            }
            if (state == HarnessState::Failed)
            {
                return false;
            }
            ::Sleep(50);
        }
        return false;
    }

    // Collects a conversation's events and lets the test wait for a turn's end.
    struct Collector
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<HarnessEvent> events;

        void operator()(const HarnessEvent& e)
        {
            {
                std::lock_guard lock{ mutex };
                events.push_back(e);
            }
            changed.notify_all();
            switch (e.kind)
            {
            case HarnessEventKind::State:
                Say(L"  event state=" + std::wstring{ HarnessStateName(e.state) } + L" epoch=" + std::to_wstring(e.epoch));
                break;
            case HarnessEventKind::Ready:
                Say(L"  event ready model=" + e.model + L" claude=" + e.cliVersion + L" permission=" + e.permissionMode);
                break;
            case HarnessEventKind::UserMessage:
                Say(L"  event user[" + std::to_wstring(e.turn) + L"]: " + Short(e.text));
                break;
            case HarnessEventKind::AssistantText:
                Say(L"  event assistant[" + std::to_wstring(e.turn) + L"]: " + Short(e.text));
                break;
            case HarnessEventKind::ToolCall:
                Say(L"  event tool-call[" + std::to_wstring(e.turn) + L"] " + e.toolName + L" " + Short(e.argumentsJson));
                break;
            case HarnessEventKind::ToolResult:
                Say(L"  event tool-result[" + std::to_wstring(e.turn) + L"] " + e.toolName + (e.isError ? L" ERROR " : L" ") + Short(e.text, 160));
                break;
            case HarnessEventKind::TurnCompleted:
                Say(L"  event turn-completed[" + std::to_wstring(e.turn) + L"] " + (e.isError ? L"ERROR " : L"") + (e.interrupted ? L"INTERRUPTED " : L"") + std::to_wstring(e.durationMs) + L" ms ctx=" + std::to_wstring(e.usage.contextTokens) + L"/" + std::to_wstring(e.usage.contextWindow) + L": " + Short(e.text));
                break;
            case HarnessEventKind::Compacting:
                Say(L"  event compacting");
                break;
            case HarnessEventKind::Compacted:
                Say(L"  event compacted " + std::to_wstring(e.preTokens) + L" -> " + std::to_wstring(e.postTokens));
                break;
            case HarnessEventKind::Notice:
                Say(L"  event notice: " + Short(e.text));
                break;
            case HarnessEventKind::Error:
                Say(L"  event ERROR: " + Short(e.text, 400));
                break;
            case HarnessEventKind::AssistantDelta:
            default:
                break;
            }
        }

        // Wait for the TurnCompleted of `turn`.
        //   returns — that event, or one with kind State when the time ran out.
        HarnessEvent WaitTurn(uint64_t turn, int seconds = 120)
        {
            std::unique_lock lock{ mutex };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ seconds };
            for (;;)
            {
                for (const auto& e : events)
                {
                    if (e.kind == HarnessEventKind::TurnCompleted && e.turn == turn)
                    {
                        return e;
                    }
                }
                if (changed.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    return HarnessEvent{};
                }
            }
        }

        // Wait until an event of `kind` arrived after index `from`.
        bool WaitKind(HarnessEventKind kind, size_t from, int seconds)
        {
            std::unique_lock lock{ mutex };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ seconds };
            for (;;)
            {
                for (size_t i = from; i < events.size(); ++i)
                {
                    if (events[i].kind == kind)
                    {
                        return true;
                    }
                }
                if (changed.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    return false;
                }
            }
        }

        size_t Count()
        {
            std::lock_guard lock{ mutex };
            return events.size();
        }

        int CountKind(HarnessEventKind kind, uint64_t turn)
        {
            std::lock_guard lock{ mutex };
            int n = 0;
            for (const auto& e : events)
            {
                if (e.kind == kind && e.turn == turn)
                {
                    ++n;
                }
            }
            return n;
        }
    };

    // This process's CPU time (kernel + user) so far, in milliseconds.
    int64_t ProcessCpuMs()
    {
        FILETIME create{}, exit{}, kernel{}, user{};
        if (!::GetProcessTimes(::GetCurrentProcess(), &create, &exit, &kernel, &user))
        {
            return 0;
        }
        const auto ticks = [](const FILETIME& ft) {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        return static_cast<int64_t>((ticks(kernel) + ticks(user)) / 10000);
    }

    // The two fake tool servers of the harness scenarios: a search over five fixed "sessions" and
    // a display that remembers what it was told to show.
    struct FakeTools
    {
        std::mutex mutex;
        std::vector<std::wstring> searches; // every query, in order
        std::vector<std::wstring> shown; // the ids of the newest show call
        int showCalls{ 0 };

        std::vector<HarnessToolServer> Servers()
        {
            static const struct
            {
                const wchar_t* id;
                const wchar_t* title;
                const wchar_t* dir;
            } kRows[] = {
                { L"aaaa1111", L"Fix tab drag crash in the tab strip", L"K:/source/Agentmaster" },
                { L"bbbb2222", L"Sessions page tag chips performance", L"K:/source/Agentmaster" },
                { L"cccc3333", L"NumSharp polyfit for six bases", L"K:/source/NumSharp" },
                { L"dddd4444", L"Tab strip reveal after a drag reorder", L"K:/source/Agentmaster" },
                { L"eeee5555", L"catalog deploy script", L"K:/source/proxmox/catalog" },
            };
            HarnessTool find;
            find.name = L"find_sessions";
            find.description = L"Search the user's sessions. Every word of the query must appear in a session's title or folder. Returns matching sessions as JSON.";
            find.inputSchemaJson = LR"({"type":"object","properties":{"query":{"type":"string","description":"Words to match. Use few, distinctive words."}},"required":["query"]})";
            find.readOnly = true;
            find.handler = [this](const json::Value& args) -> HarnessToolResult {
                const std::wstring query = args.StrAt(L"query");
                {
                    std::lock_guard lock{ mutex };
                    searches.push_back(query);
                }
                std::vector<std::wstring> words;
                std::wstring word;
                for (const wchar_t c : query + L" ")
                {
                    if (c == L' ')
                    {
                        if (!word.empty())
                        {
                            words.push_back(word);
                        }
                        word.clear();
                    }
                    else
                    {
                        word.push_back(static_cast<wchar_t>(::towlower(c)));
                    }
                }
                std::wstring out = L"[";
                bool first = true;
                for (const auto& row : kRows)
                {
                    std::wstring hay = std::wstring{ row.title } + L" " + row.dir;
                    for (auto& c : hay)
                    {
                        c = static_cast<wchar_t>(::towlower(c));
                    }
                    bool all = true;
                    for (const auto& w : words)
                    {
                        all = all && hay.find(w) != std::wstring::npos;
                    }
                    if (!all)
                    {
                        continue;
                    }
                    out += std::wstring{ first ? L"" : L"," } + L"{\"id\":\"" + row.id + L"\",\"title\":\"" + row.title + L"\",\"dir\":\"" + row.dir + L"\"}";
                    first = false;
                }
                out += L"]";
                return { out, false };
            };
            HarnessTool show;
            show.name = L"show_sessions";
            show.description = L"Show sessions to the user, in the given order. Replaces what is shown.";
            show.inputSchemaJson = LR"({"type":"object","properties":{"ids":{"type":"array","items":{"type":"string"},"description":"Session ids, best match first."}},"required":["ids"]})";
            show.handler = [this](const json::Value& args) -> HarnessToolResult {
                std::vector<std::wstring> ids;
                if (const auto* arr = args.Find(L"ids"); arr && arr->type == json::Value::Type::Arr)
                {
                    for (const auto& v : arr->arr)
                    {
                        ids.push_back(v.AsStr());
                    }
                }
                {
                    std::lock_guard lock{ mutex };
                    shown = ids;
                    ++showCalls;
                }
                return { L"Shown " + std::to_wstring(ids.size()) + L" session(s). Reply with one short sentence.", false };
            };
            HarnessToolServer search;
            search.name = L"search";
            search.tools.push_back(std::move(find));
            HarnessToolServer display;
            display.name = L"display";
            display.tools.push_back(std::move(show));
            return { std::move(search), std::move(display) };
        }

        bool Shown(const std::wstring& id)
        {
            std::lock_guard lock{ mutex };
            return std::find(shown.begin(), shown.end(), id) != shown.end();
        }
    };

    const wchar_t kFakePrompt[] =
        L"You are a search assistant for the user's past coding sessions. For every request: call "
        L"mcp__search__find_sessions (one or more calls, each with few distinctive words), choose the "
        L"sessions that fit, then call mcp__display__show_sessions once with their ids, best first. "
        L"After that tool returns, reply with one short sentence. Never answer without the tools.";

    HarnessOptions BaseOptions(const std::wstring& claudeExe, const std::wstring& model, const std::wstring& dir, FakeTools& tools)
    {
        HarnessOptions o;
        o.name = L"live-test";
        o.claudeExe = [claudeExe] { return claudeExe; };
        o.model = model;
        o.systemPrompt = kFakePrompt;
        o.toolServers = tools.Servers();
        o.workDir = dir + L"\\work";
        o.recordPath = dir + L"\\live-test.json";
        o.turnSilenceTimeoutMs = 90000;
        return o;
    }

    void RunHarnessScenarios(const std::wstring& claudeExe, const std::wstring& model, const std::wstring& dir)
    {
        FakeTools tools;
        Collector events;

        Say(L"=== 1. the command line the SDK builds ===");
        {
            HarnessConversation conversation{ BaseOptions(claudeExe, model, dir, tools), {} };
            std::wstring line;
            bool hasNoPersistence = false, hasAutocompact = false, hasNoTools = false, hasStrict = false, hasThinking = false, hasSources = false, hasPromptFile = false;
            bool hasBypassMode = false, hasDangerously = false, hasAllowedTools = false, hasRestricted = false, hasPermissionsKey = false;
            const auto args = conversation.DescribeCommandLine();
            for (size_t i = 0; i < args.size(); ++i)
            {
                line += (args[i].empty() ? L"\"\"" : args[i]) + L" ";
                hasNoPersistence = hasNoPersistence || args[i] == L"--no-session-persistence";
                hasStrict = hasStrict || args[i] == L"--strict-mcp-config";
                hasPromptFile = hasPromptFile || args[i] == L"--system-prompt-file";
                hasDangerously = hasDangerously || args[i] == L"--dangerously-skip-permissions";
                hasRestricted = hasRestricted || args[i] == L"--restricted";
                if (i + 1 < args.size())
                {
                    hasAutocompact = hasAutocompact || (args[i] == L"--autocompact" && args[i + 1] == L"500000");
                    hasNoTools = hasNoTools || (args[i] == L"--tools" && args[i + 1].empty());
                    hasThinking = hasThinking || (args[i] == L"--thinking" && args[i + 1] == L"disabled");
                    hasSources = hasSources || (args[i] == L"--setting-sources" && args[i + 1].empty());
                    hasBypassMode = hasBypassMode || (args[i] == L"--permission-mode" && args[i + 1] == L"bypassPermissions");
                    hasAllowedTools = hasAllowedTools || (args[i] == L"--allowedTools" && args[i + 1].find(L"mcp__search") != std::wstring::npos && args[i + 1].find(L"mcp__display") != std::wstring::npos);
                    hasPermissionsKey = hasPermissionsKey || (args[i] == L"--settings" && args[i + 1].find(L"\"permissions\"") != std::wstring::npos);
                }
            }
            Say(L"  " + line);
            Check(hasNoPersistence, L"--no-session-persistence is passed");
            Check(hasAutocompact, L"--autocompact 500000 is passed");
            Check(hasNoTools, L"--tools \"\" is passed (no built-in tools)");
            Check(hasStrict, L"--strict-mcp-config is passed");
            Check(hasThinking, L"--thinking disabled is passed");
            Check(hasSources, L"--setting-sources \"\" is passed");
            Check(hasPromptFile, L"the system prompt goes through --system-prompt-file");
            Check(hasBypassMode && hasDangerously, L"BYPASS PERMISSIONS is asked for: --permission-mode bypassPermissions and --dangerously-skip-permissions");
            Check(hasAllowedTools, L"the tool servers stay on --allowedTools (what keeps the two fallback modes working, and auto fast)");
            Check(!hasRestricted && !hasPermissionsKey, L"a product launch passes neither --restricted nor a permissions settings key (both are test seams)");
            const HarnessInventory before = conversation.Inventory();
            Check(!before.known && !before.complete, L"before a process answered, the inventory says that nothing is known");
        }

        Say(L"=== 2. a conversation: start, tools, events ===");
        {
            HarnessConversation conversation{ BaseOptions(claudeExe, model, dir, tools), std::ref(events) };
            const double t0 = Seconds();
            conversation.Prewarm();
            // Wait for Ready (or Failed).
            for (int i = 0; i < 300 && conversation.State() != HarnessState::Ready && conversation.State() != HarnessState::Failed; ++i)
            {
                ::Sleep(50);
            }
            Check(conversation.State() == HarnessState::Ready, L"the process is ready after Prewarm (" + std::to_wstring(static_cast<int>((Seconds() - t0) * 1000)) + L" ms)");
            if (conversation.State() != HarnessState::Ready)
            {
                return; // nothing else can pass
            }
            // The permission mode is the process's own word, known BEFORE any model request.
            const HarnessInventory atStart = conversation.Inventory();
            SayInventory(atStart);
            Check(atStart.known && atStart.permissionMode == L"bypassPermissions" && atStart.askedPermissionMode == L"bypassPermissions", L"the process REPORTS bypassPermissions at start, before any message (" + atStart.permissionMode + L")");
            Check(!atStart.complete, L"the lists are not known before the first turn");

            const uint64_t turn1 = conversation.Send(L"find the session where I fixed the tab drag crash");
            const HarnessEvent done1 = events.WaitTurn(turn1);
            Check(done1.kind == HarnessEventKind::TurnCompleted && !done1.isError, L"turn 1 completed without an error");
            Check(events.CountKind(HarnessEventKind::ToolCall, turn1) >= 2, L"turn 1 called the tools (search, then show)");
            Check(tools.Shown(L"aaaa1111"), L"turn 1 showed the tab drag crash session");
            // What the model in this process can use, as the process itself listed it.
            const HarnessInventory afterTurn = conversation.Inventory();
            SayInventory(afterTurn);
            bool onlyOurTools = afterTurn.tools.size() == 2;
            for (const auto& tool : afterTurn.tools)
            {
                onlyOurTools = onlyOurTools && (tool == L"mcp__search__find_sessions" || tool == L"mcp__display__show_sessions");
            }
            Check(afterTurn.complete && onlyOurTools, L"the model has EXACTLY the two harness tools (" + Join(afterTurn.tools) + L") - no built-in tool, no MCP resource tool");
            Check(afterTurn.mcpServers.size() == 2 && afterTurn.slashCommands.empty() && afterTurn.skills.empty(), L"two tool servers, no slash command, no skill");
            Check(afterTurn.permissionMode == L"bypassPermissions", L"the first turn confirms the mode: " + afterTurn.permissionMode);
            bool readySaysBypass = false;
            {
                std::lock_guard lock{ events.mutex };
                for (const auto& e : events.events)
                {
                    readySaysBypass = readySaysBypass || (e.kind == HarnessEventKind::Ready && e.permissionMode == L"bypassPermissions");
                }
            }
            Check(readySaysBypass, L"the Ready event carries the permission mode");
            Check(done1.usage.contextTokens > 0 && done1.usage.contextWindow == 500000, L"usage: context " + std::to_wstring(done1.usage.contextTokens) + L" of a 500000 window");
            Check(!done1.text.empty(), L"turn 1 has a final sentence");

            Say(L"=== 3. a follow-up keeps its context ===");
            const uint64_t turn2 = conversation.Send(L"and the catalog one");
            const HarnessEvent done2 = events.WaitTurn(turn2);
            Check(done2.kind == HarnessEventKind::TurnCompleted && !done2.isError, L"turn 2 completed without an error");
            Check(tools.Shown(L"eeee5555"), L"turn 2 showed the catalog session");

            Say(L"=== 4. an idle conversation uses no CPU ===");
            const int64_t cpu0 = ProcessCpuMs();
            ::Sleep(3000);
            const int64_t cpuIdle = ProcessCpuMs() - cpu0;
            // A spinning reader thread costs about 3000 ms of CPU in 3 s; the timed waits cost a few.
            Check(cpuIdle < 300, L"3 s idle cost " + std::to_wstring(cpuIdle) + L" ms of CPU in this process (a spinning reader would cost about 3000)");

            Say(L"=== 5. newest wins ===");
            const uint64_t turn3 = conversation.Send(L"find sessions about NumSharp");
            ::Sleep(250); // let turn 3 start
            const uint64_t turn4 = conversation.Send(L"no: find the sessions page tag chips session");
            const HarnessEvent done3 = events.WaitTurn(turn3);
            const HarnessEvent done4 = events.WaitTurn(turn4);
            Check(done3.kind == HarnessEventKind::TurnCompleted && done3.interrupted, L"turn 3 ended as interrupted");
            Check(done4.kind == HarnessEventKind::TurnCompleted && !done4.isError && !done4.interrupted, L"turn 4 completed");
            Check(tools.Shown(L"bbbb2222"), L"turn 4 showed the tag chips session");

            const HarnessRecord record = conversation.Record();
            Check(record.turns == 4 && !record.items.empty(), L"the record holds 4 turns and " + std::to_wstring(record.items.size()) + L" items");
        } // the conversation object goes away here: the process ends, the record file stays

        Say(L"=== 6. the record survives; a new process gets the digest ===");
        {
            Check(std::filesystem::exists(dir + L"\\live-test.json"), L"the record file exists");
            Collector events2;
            HarnessConversation conversation{ BaseOptions(claudeExe, model, dir, tools), std::ref(events2) };
            const HarnessRecord loaded = conversation.Record();
            Check(loaded.turns == 4, L"a new conversation object loaded the record (" + std::to_wstring(loaded.turns) + L" turns)");
            // "that one" only makes sense with the digest: the new process never saw turn 4.
            const uint64_t turn5 = conversation.Send(L"Which session did you show me last? Show that one again.");
            const HarnessEvent done5 = events2.WaitTurn(turn5);
            Check(turn5 == 5, L"turn ids go on after the stored turns (" + std::to_wstring(turn5) + L")");
            Check(done5.kind == HarnessEventKind::TurnCompleted && !done5.isError, L"turn 5 completed in a new process");
            Check(tools.Shown(L"bbbb2222"), L"turn 5 showed the session of turn 4 again (the digest carried the context)");

            Say(L"=== 6b. park: the idle process ends, and comes back on demand ===");
            // No model call here: a park is about the process, the record carries the conversation.
            conversation.ParkAfter(0);
            bool parked = false;
            for (int i = 0; i < 200 && !parked; ++i)
            {
                ::Sleep(50);
                parked = conversation.State() == HarnessState::Stopped;
            }
            Check(parked, L"ParkAfter(0) ended the idle process");
            Check(conversation.Record().turns == 5, L"the record survived the park (" + std::to_wstring(conversation.Record().turns) + L" turns)");
            conversation.Prewarm();
            bool back = false;
            for (int i = 0; i < 300 && !back; ++i)
            {
                ::Sleep(50);
                back = conversation.State() == HarnessState::Ready;
            }
            Check(back, L"Prewarm started a process again after the park");
            conversation.ParkAfter(1500);
            conversation.Prewarm(); // somebody came back before the delay ended
            ::Sleep(2500);
            Check(conversation.State() == HarnessState::Ready, L"a Prewarm cancelled a pending park");

            Say(L"=== 7. reset ===");
            const uint64_t epochBefore = conversation.Epoch();
            const size_t mark = events2.Count();
            conversation.Reset();
            Check(conversation.Epoch() == epochBefore + 1, L"Reset raised the epoch at once");
            Check(conversation.Record().Empty(), L"Reset emptied the record at once");
            // The worker ends the old process, deletes the file and starts a fresh process.
            bool ready = false;
            for (int i = 0; i < 300 && !ready; ++i)
            {
                ::Sleep(50);
                ready = conversation.State() == HarnessState::Ready && events2.Count() > mark;
            }
            Check(ready, L"a fresh process is ready after the reset");
            Check(!std::filesystem::exists(dir + L"\\live-test.json"), L"the record file is gone");
            const uint64_t turn6 = conversation.Send(L"Which session did you show me last?");
            const HarnessEvent done6 = events2.WaitTurn(turn6);
            Check(turn6 == 1, L"turn ids start again at 1 after a reset");
            Check(done6.kind == HarnessEventKind::TurnCompleted && !done6.isError, L"a turn after the reset completed: " + Short(done6.text, 120));
        }

        Say(L"=== 8. a missing claude.exe fails fast ===");
        {
            Collector events3;
            HarnessOptions o = BaseOptions(dir + L"\\no-such-claude.exe", model, dir, tools);
            o.persistRecord = false;
            HarnessConversation conversation{ std::move(o), std::ref(events3) };
            const double t0 = Seconds();
            const uint64_t turn = conversation.Send(L"hello");
            const HarnessEvent done = events3.WaitTurn(turn, 30);
            const double took = Seconds() - t0;
            Check(done.kind == HarnessEventKind::TurnCompleted && done.isError, L"the turn failed: " + Short(done.text, 160));
            Check(took < 5.0, L"it failed in " + std::to_wstring(static_cast<int>(took * 1000)) + L" ms (not after a 60 s timeout)");
            Check(conversation.State() == HarnessState::Failed, L"the state is failed");
        }
    }

    // The PERMISSION POLICY, against the real claude.exe: bypass always; auto when Claude Code
    // does not allow bypass; dontAsk when it allows neither. This machine's policy allows
    // everything, so each "not allowed" is made with the test seam (HarnessPermissionProbe) —
    // the refusal itself is Claude Code's own, exactly as with an administrator's policy.
    // Three starts cost no model request; two short turns prove that the tools still run in each
    // fallback mode.
    void RunPermissionScenarios(const std::wstring& claudeExe, const std::wstring& model, const std::wstring& dir)
    {
        const auto options = [&](FakeTools& tools, const wchar_t* name) {
            HarnessOptions o = BaseOptions(claudeExe, model, dir, tools);
            o.name = name; // its own log tag, so each scenario's lines can be told apart
            o.persistRecord = false;
            o.workDir = dir + L"\\" + name;
            return o;
        };

        Say(L"=== P1. bypass is NOT ALLOWED (a policy switch): Claude Code chooses auto, the harness keeps it ===");
        {
            FakeTools tools;
            Collector events;
            HarnessOptions o = options(tools, L"live-perm-soft");
            o.permissionProbe.bypassDisabledBySettings = true;
            const uintmax_t mark = HooksLogSize();
            HarnessConversation conversation{ std::move(o), std::ref(events) };
            conversation.Prewarm();
            Check(WaitReady(conversation), L"the process started although bypass is refused");
            const HarnessInventory inventory = conversation.Inventory();
            SayInventory(inventory);
            Check(inventory.permissionMode == L"auto" && inventory.askedPermissionMode == L"bypassPermissions", L"asked bypassPermissions, runs in auto (" + inventory.permissionMode + L")");
            const std::wstring logged = SayHarnessLogSince(mark, L"permission mode");
            Check(logged.find(L"NOT ALLOWED") != std::wstring::npos, L"hooks.log says that bypass is not allowed here");
            const double t0 = Seconds();
            const uint64_t turn = conversation.Send(L"find the catalog deploy script session");
            const HarnessEvent done = events.WaitTurn(turn);
            Check(done.kind == HarnessEventKind::TurnCompleted && !done.isError && tools.Shown(L"eeee5555"), L"in auto mode the tools run (the allowed list: no classifier request for them): " + std::to_wstring(static_cast<int>((Seconds() - t0) * 1000)) + L" ms");
            Check(conversation.Inventory().permissionMode == L"auto", L"the first turn confirms auto");
        }

        Say(L"=== P2. NEITHER bypass nor auto is allowed: Claude Code starts in 'default', the harness switches to dontAsk ===");
        {
            FakeTools tools;
            Collector events;
            HarnessOptions o = options(tools, L"live-perm-neither");
            o.permissionProbe.bypassDisabledBySettings = true;
            o.permissionProbe.autoDisabledBySettings = true;
            const uintmax_t mark = HooksLogSize();
            HarnessConversation conversation{ std::move(o), std::ref(events) };
            conversation.Prewarm();
            Check(WaitReady(conversation), L"the process started although both modes are refused");
            const HarnessInventory inventory = conversation.Inventory();
            SayInventory(inventory);
            Check(inventory.permissionMode == L"dontAsk", L"the harness ended in dontAsk (" + inventory.permissionMode + L"): a hidden process must not run in a mode that can ask");
            const std::wstring logged = SayHarnessLogSince(mark, L"permission mode");
            Check(logged.find(L"auto was REFUSED") != std::wstring::npos && logged.find(L"dontAsk") != std::wstring::npos, L"hooks.log shows both steps: auto refused, then dontAsk");
            const uint64_t turn = conversation.Send(L"find the NumSharp session");
            const HarnessEvent done = events.WaitTurn(turn);
            Check(done.kind == HarnessEventKind::TurnCompleted && !done.isError && tools.Shown(L"cccc3333"), L"in dontAsk the allowed tools still run");
            const std::wstring modeAfter = conversation.Inventory().permissionMode;
            Check(modeAfter == L"dontAsk", L"the first turn's own report is dontAsk too (" + modeAfter + L"): the switch held");
        }

        Say(L"=== P3. bypass is REFUSED WITH AN ERROR at start (--restricted): the harness starts again with auto ===");
        {
            FakeTools tools;
            Collector events;
            HarnessOptions o = options(tools, L"live-perm-hard");
            o.permissionProbe.restricted = true;
            const uintmax_t mark = HooksLogSize();
            HarnessConversation conversation{ std::move(o), std::ref(events) };
            const double t0 = Seconds();
            conversation.Prewarm();
            const bool ready = WaitReady(conversation, 30);
            Check(ready, L"the second start worked (" + std::to_wstring(static_cast<int>((Seconds() - t0) * 1000)) + L" ms for both starts)");
            const HarnessInventory inventory = conversation.Inventory();
            SayInventory(inventory);
            Check(inventory.permissionMode == L"auto" && inventory.askedPermissionMode == L"auto", L"it runs in auto, and auto is what the working start asked for");
            const std::wstring logged = SayHarnessLogSince(mark);
            Check(logged.find(L"was REFUSED") != std::wstring::npos && logged.find(L"restricted mode") != std::wstring::npos, L"hooks.log holds Claude Code's own reason for the refused start");
            bool errorEvent = false;
            {
                std::lock_guard lock{ events.mutex };
                for (const auto& e : events.events)
                {
                    errorEvent = errorEvent || e.kind == HarnessEventKind::Error;
                }
            }
            Check(!errorEvent, L"the refused first start raised no Error event: the user sees one start");
            // The refusal is remembered: the next start of this conversation asks for auto at once.
            bool asksAuto = false, asksBypass = false;
            const auto args = conversation.DescribeCommandLine();
            for (size_t i = 0; i < args.size(); ++i)
            {
                asksAuto = asksAuto || (args[i] == L"--permission-mode" && i + 1 < args.size() && args[i + 1] == L"auto");
                asksBypass = asksBypass || args[i] == L"--dangerously-skip-permissions";
            }
            Check(asksAuto && !asksBypass, L"later starts of this conversation ask for auto directly (no failed start each time)");
        }
    }

#ifndef AM_LIVE_NO_AGENT
    // --index: how long this machine's session index takes to read, and which sessions are the
    // slow ones. No model call. It reads through the same two functions the gather uses
    // (EnumerateTranscripts + LoadOrRefreshSessionIndex), one session at a time, so each read
    // can be timed. It writes sidecar files into the test profile, like any gather does.
    //   days — the period to read (the search tool's default when 0).
    void RunIndexTiming(int days)
    {
        if (days <= 0)
        {
            days = kSessionSearchDefaultDays;
        }
        Say(L"=== index timing: the last " + std::to_wstring(days) + L" day(s) ===");
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const double t0 = Seconds();
        const auto refs = EnumerateTranscripts(now - static_cast<int64_t>(days) * 86400000LL);
        const double listed = Seconds() - t0;
        int64_t bytes = 0;
        for (const auto& ref : refs)
        {
            bytes += ref.sizeBytes;
        }
        Say(L"  listing: " + std::to_wstring(refs.size()) + L" transcript(s), " + std::to_wstring(bytes / (1024 * 1024)) + L" MB, in " + std::to_wstring(static_cast<int>(listed * 1000)) + L" ms");

        struct Slow
        {
            double ms{};
            int64_t sizeBytes{};
            size_t rank{}; // position in the listing (0 = the newest)
            std::wstring id;
        };
        std::vector<Slow> slow;
        const double t1 = Seconds();
        double over50 = 0;
        size_t over50Count = 0;
        for (size_t i = 0; i < refs.size(); ++i)
        {
            const double a = Seconds();
            const SessionIndexEntry entry = LoadOrRefreshSessionIndex(refs[i]);
            const double ms = (Seconds() - a) * 1000.0;
            if (ms >= 50.0)
            {
                over50 += ms;
                ++over50Count;
                slow.push_back(Slow{ ms, refs[i].sizeBytes, i, refs[i].sessionId });
            }
            (void)entry;
        }
        const double read = Seconds() - t1;
        Say(L"  sidecar reads: " + std::to_wstring(static_cast<int>(read * 1000)) + L" ms in total; " + std::to_wstring(over50Count) + L" read(s) of 50 ms or more took " + std::to_wstring(static_cast<int>(over50)) + L" ms of it");
        std::sort(slow.begin(), slow.end(), [](const Slow& a, const Slow& b) { return a.ms > b.ms; });
        for (size_t i = 0; i < slow.size() && i < 12; ++i)
        {
            Say(L"      " + std::to_wstring(static_cast<int>(slow[i].ms)) + L" ms  #" + std::to_wstring(slow[i].rank) + L"  " + slow[i].id.substr(0, 8) + L"  " + std::to_wstring(slow[i].sizeBytes / 1024) + L" KB");
        }
        // The same read again: every sidecar is current now, so this is the cost a warm index has.
        const double t2 = Seconds();
        for (const auto& ref : refs)
        {
            (void)LoadOrRefreshSessionIndex(ref);
        }
        Say(L"  the same reads again (every sidecar current): " + std::to_wstring(static_cast<int>((Seconds() - t2) * 1000)) + L" ms");
        // And the whole gather, as the keeper's first step would run it with no budget.
        const double t3 = Seconds();
        const auto catalog = GatherSessionCatalog(now - static_cast<int64_t>(days) * 86400000LL, SessionCatalogSources{});
        const double gathered = Seconds() - t3;
        Say(L"  a whole gather (listing + sidecars + the session store): " + std::to_wstring(static_cast<int>(gathered * 1000)) + L" ms, " + std::to_wstring(catalog->entries.size()) + L" session(s)");
        SessionCatalogGatherOptions again;
        again.previous = catalog;
        const double t4 = Seconds();
        const auto refreshed = GatherSessionCatalog(now - static_cast<int64_t>(days) * 86400000LL, SessionCatalogSources{}, again);
        Say(L"  a refresh of it (unchanged sessions copied): " + std::to_wstring(static_cast<int>((Seconds() - t4) * 1000)) + L" ms, " + std::to_wstring(refreshed->entries.size()) + L" session(s)");
    }

    // Print one turn of a conversation record as a TIMELINE: what the user asked, every tool call
    // with the arguments the model sent, every tool result, and what the model said — each with
    // its time since the request. This is HOW a request was answered.
    //   record      — the conversation's record (SessionSearchAgent::Record).
    //   turn        — the turn to print.
    //   resultChars — how much of each tool result to print (the record keeps at most
    //                 kHarnessMaxToolResultChars of one).
    void SayTurnTrace(const HarnessRecord& record, uint64_t turn, size_t resultChars)
    {
        int64_t t0 = 0;
        for (const auto& item : record.items)
        {
            if (item.turn != turn)
            {
                continue;
            }
            if (t0 == 0)
            {
                t0 = item.atUnixMs;
            }
            wchar_t at[32]{};
            swprintf_s(at, L"+%5lld ms", static_cast<long long>(item.atUnixMs - t0));
            switch (item.kind)
            {
            case HarnessItemKind::User:
                Say(std::wstring{ L"  trace " } + at + L"  USER         " + Short(item.text, 400));
                break;
            case HarnessItemKind::ToolCall:
                Say(std::wstring{ L"  trace " } + at + L"  TOOL CALL    " + item.toolName + L" " + Short(item.argumentsJson, 600));
                break;
            case HarnessItemKind::ToolResult:
                Say(std::wstring{ L"  trace " } + at + L"  TOOL RESULT  " + (item.isError ? L"(ERROR) " : L"") + Short(item.text, resultChars));
                break;
            case HarnessItemKind::Assistant:
                Say(std::wstring{ L"  trace " } + at + L"  MODEL SAYS   " + Short(item.text, 600));
                break;
            case HarnessItemKind::Notice:
            default:
                Say(std::wstring{ L"  trace " } + at + L"  NOTICE       " + Short(item.text, 400));
                break;
            }
        }
    }

    // Print what the search agent's model is GIVEN before any request: the command line of its
    // process, its whole system prompt, and each tool with the description and the argument
    // schema the model reads. Starts nothing.
    void SayAgentSetup(const SessionSearchAgent& agent)
    {
        Say(L"  --- the process command line (claude.exe + these arguments) ---");
        const auto args = agent.DescribeCommandLine();
        for (size_t i = 0; i < args.size(); ++i)
        {
            // An option and its value on one line; a value that is itself JSON is long.
            std::wstring line = args[i].empty() ? L"\"\"" : args[i];
            if (args[i].rfind(L"--", 0) == 0 && i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0)
            {
                line += L" " + (args[i + 1].empty() ? std::wstring{ L"\"\"" } : args[i + 1]);
                ++i;
            }
            Say(L"    " + Short(line, 1200));
        }
        Say(L"  --- the system prompt (it REPLACES Claude Code's own) ---");
        const std::wstring prompt = SessionSearchToolset::SystemPrompt();
        size_t start = 0;
        while (start < prompt.size())
        {
            size_t end = prompt.find(L'\n', start);
            if (end == std::wstring::npos)
            {
                end = prompt.size();
            }
            Say(L"    " + prompt.substr(start, end - start));
            start = end + 1;
        }
        Say(L"  --- the tools the model is told about ---");
        // A toolset of its own, only to list what it declares: its handlers are never called.
        SessionSearchToolset::Callbacks listOnly;
        listOnly.catalog = [](int64_t, const std::function<bool()>&) { return std::shared_ptr<const SessionCatalog>{}; };
        listOnly.onShow = [](const std::vector<std::wstring>&, const std::wstring&) {};
        SessionSearchToolset toolset{ std::move(listOnly) };
        for (const auto& server : toolset.Servers())
        {
            for (const auto& tool : server.tools)
            {
                Say(L"    " + HarnessToolFullName(server.name, tool.name) + (tool.readOnly ? L"  (read-only: calls of one model turn may run at the same time)" : L""));
                Say(L"      description: " + Short(tool.description, 1500));
                Say(L"      arguments  : " + Short(tool.inputSchemaJson, 2500));
            }
        }
    }

    void RunAgentScenario(const std::wstring& claudeExe, const std::wstring& request)
    {
        Say(L"=== 9. the Sessions search agent, on this machine's real sessions ===");
        SessionSearchAgentOptions options;
        options.claudeExe = [claudeExe] { return claudeExe; };
        options.persist = false; // a test must not leave a conversation in the profile
        SessionSearchAgent agent{ options };
        SayAgentSetup(agent);
        const uintmax_t logMark = HooksLogSize();

        std::mutex mutex;
        std::condition_variable changed;
        std::vector<SessionSearchEvent> seen;
        const uint64_t token = agent.AddListener([&](const SessionSearchEvent& e) {
            {
                std::lock_guard lock{ mutex };
                seen.push_back(e);
            }
            changed.notify_all();
            switch (e.kind)
            {
            case SessionSearchEventKind::Searching:
                Say(L"  agent searching: " + Short(e.text));
                break;
            case SessionSearchEventKind::Candidates:
                Say(L"  agent candidates: " + std::to_wstring(e.sessionIds.size()));
                break;
            case SessionSearchEventKind::Shown:
                Say(L"  agent SHOWN " + std::to_wstring(e.sessionIds.size()) + L" session(s): " + Short(e.text));
                for (size_t i = 0; i < e.entries.size() && i < 8; ++i)
                {
                    Say(L"      " + e.entries[i].sessionId.substr(0, 8) + L"  " + Short(SessionSearchTitleOf(e.entries[i]), 90));
                }
                break;
            case SessionSearchEventKind::Answer:
                Say(L"  agent answer: " + Short(e.text, 300));
                break;
            case SessionSearchEventKind::Failed:
                Say(L"  agent FAILED: " + Short(e.text, 300));
                break;
            case SessionSearchEventKind::StateChanged:
            default:
                if (g_verbose)
                {
                    Say(L"  agent state: " + std::wstring{ HarnessStateName(e.snapshot.state) });
                }
                break;
            }
        });

        const double t0 = Seconds();
        const uint64_t turn = agent.Ask(request);
        Check(turn != 0, L"the request was accepted: " + request);
        double shownAt = 0;
        bool shown = false, answered = false, failed = false;
        {
            std::unique_lock lock{ mutex };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 120 };
            size_t next = 0;
            while (!answered && !failed)
            {
                for (; next < seen.size(); ++next)
                {
                    const auto& e = seen[next];
                    if (e.kind == SessionSearchEventKind::Shown && !shown)
                    {
                        shown = true;
                        shownAt = Seconds() - t0;
                    }
                    answered = answered || e.kind == SessionSearchEventKind::Answer;
                    failed = failed || e.kind == SessionSearchEventKind::Failed;
                }
                if (answered || failed)
                {
                    break;
                }
                if (changed.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    break;
                }
            }
        }
        const double total = Seconds() - t0;
        Check(shown, L"the agent showed results after " + std::to_wstring(static_cast<int>(shownAt * 1000)) + L" ms (process start included)");
        Check(answered && !failed, L"the agent answered after " + std::to_wstring(static_cast<int>(total * 1000)) + L" ms");
        const SessionSearchSnapshot snapshot = agent.Snapshot();
        // With an index that is still being built (a new profile: every transcript is parsed
        // once), "nothing found yet, older sessions are still being indexed" is the right
        // answer, and an empty list with it is not a failure.
        const bool saidIndexing = snapshot.answer.find(L"index") != std::wstring::npos;
        Check(!snapshot.shownIds.empty() || saidIndexing,
              L"the snapshot holds " + std::to_wstring(snapshot.shownIds.size()) + L" shown id(s)" + (saidIndexing ? std::wstring{ L" (the answer says that the index is still being built)" } : std::wstring{}) + L"; note: " + Short(snapshot.note, 120));

        // HOW it was answered: the turn as the conversation's record holds it, the process's own
        // account of what its model can use, and the harness's log lines of this run.
        Say(L"  --- request 1, step by step (times since the request reached the model's process) ---");
        SayTurnTrace(agent.Record(), turn, g_verbose ? kHarnessMaxToolResultChars : 900);
        Say(L"  --- what the process reported about itself ---");
        const HarnessInventory inventory = agent.Inventory();
        SayInventory(inventory);
        Check(inventory.complete && inventory.permissionMode == L"bypassPermissions", L"the agent's process runs with bypass permissions (" + inventory.permissionMode + L")");
        bool onlySearchTools = inventory.tools.size() == 2;
        for (const auto& tool : inventory.tools)
        {
            onlySearchTools = onlySearchTools && (tool == L"mcp__sessions__search" || tool == L"mcp__display__show");
        }
        Check(onlySearchTools, L"its model has exactly two tools: " + Join(inventory.tools));
        Say(L"  --- usage so far: context " + std::to_wstring(snapshot.usage.contextTokens) + L" of " + std::to_wstring(snapshot.usage.contextWindow) + L" tokens; input " + std::to_wstring(snapshot.usage.inputTokens) + L", output " + std::to_wstring(snapshot.usage.outputTokens) + L", cache read " + std::to_wstring(snapshot.usage.cacheReadTokens) + L", cache write " + std::to_wstring(snapshot.usage.cacheCreationTokens) + L" ---");
        Say(L"  --- the harness's own log lines of this run ---");
        SayHarnessLogSince(logMark);

        // A second request in the same conversation: no process start this time.
        const double t1 = Seconds();
        {
            std::lock_guard lock{ mutex };
            seen.clear();
        }
        const uint64_t turn2 = agent.Ask(L"only the three newest of those");
        bool answered2 = false;
        double shown2At = 0;
        {
            std::unique_lock lock{ mutex };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 120 };
            size_t next = 0;
            bool failed2 = false;
            while (!answered2 && !failed2)
            {
                for (; next < seen.size(); ++next)
                {
                    const auto& e = seen[next];
                    if (e.kind == SessionSearchEventKind::Shown && shown2At == 0)
                    {
                        shown2At = Seconds() - t1;
                    }
                    answered2 = answered2 || e.kind == SessionSearchEventKind::Answer;
                    failed2 = failed2 || e.kind == SessionSearchEventKind::Failed;
                }
                if (answered2 || failed2)
                {
                    break;
                }
                if (changed.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    break;
                }
            }
        }
        Check(answered2, L"a follow-up was answered; results shown after " + std::to_wstring(static_cast<int>(shown2At * 1000)) + L" ms (warm process)");
        Check(agent.Snapshot().shownIds.size() <= 3, L"the follow-up narrowed the list to " + std::to_wstring(agent.Snapshot().shownIds.size()) + L" session(s)");
        Say(L"  --- request 2 (the follow-up), step by step ---");
        SayTurnTrace(agent.Record(), turn2, g_verbose ? kHarnessMaxToolResultChars : 900);

        agent.Reset();
        Check(agent.Snapshot().shownIds.empty() && agent.Snapshot().turns == 0, L"Reset cleared the agent's conversation");
        agent.RemoveListener(token);
        agent.Shutdown();
    }
#endif // AM_LIVE_NO_AGENT
}

int wmain(int argc, wchar_t** argv)
{
    // FIRST, before any engine call resolves the profile (the resolution is cached for the
    // process): this test must never run in a real profile. A shell hosted by a running
    // Agentmaster carries AGENTMASTER_PROFILE = that instance's LIVE profile, so an inherited
    // value is IGNORED here — the test would otherwise write its traces into the live hooks.log,
    // refresh the live session index and leave a harness folder there (it did, once, when the
    // exe was run without its script). AM_LIVE_PROFILE is the one deliberate override.
    {
        wchar_t chosen[MAX_PATH]{};
        std::wstring profile;
        if (const DWORD n = ::GetEnvironmentVariableW(L"AM_LIVE_PROFILE", chosen, MAX_PATH); n > 0 && n < MAX_PATH)
        {
            profile = chosen;
        }
        else
        {
            wchar_t temp[MAX_PATH]{};
            ::GetTempPathW(MAX_PATH, temp);
            profile = std::wstring{ temp } + L"agentmaster-harness-live-profile";
        }
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", profile.c_str());
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);
    }
    ::SetConsoleOutputCP(CP_UTF8);
    std::wstring claudeExe;
    std::wstring model = L"claude-sonnet-5-5";
    std::wstring agentRequest;
    bool onlyAgent = false;
    bool onlyPermissions = false; // --only-permissions: the permission scenarios alone (two short turns)
    int indexDays = -1; // --index [days]: -1 = not asked, 0 = the default period
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--index")
        {
            indexDays = 0;
            if (i + 1 < argc && argv[i + 1][0] >= L'0' && argv[i + 1][0] <= L'9')
            {
                indexDays = _wtoi(argv[++i]);
            }
            continue;
        }
        if (a == L"--claude" && i + 1 < argc)
        {
            claudeExe = argv[++i];
        }
        else if (a == L"--model" && i + 1 < argc)
        {
            model = argv[++i];
        }
        else if (a == L"--agent" && i + 1 < argc)
        {
            agentRequest = argv[++i];
        }
        else if (a == L"--only-agent")
        {
            onlyAgent = true;
        }
        else if (a == L"--only-permissions")
        {
            onlyPermissions = true;
        }
        else if (a == L"--verbose")
        {
            g_verbose = true;
        }
    }
    if (claudeExe.empty())
    {
        claudeExe = ResolveClaudeExe(L""); // the app's own resolution: PATH claude.exe, ~/.local/bin, an npm binary
    }
    Say(L"claude.exe: " + (claudeExe.empty() ? std::wstring{ L"(not found)" } : claudeExe));
    Say(L"profile:    " + AgentmasterStateDir());
    if (claudeExe.empty())
    {
        Say(L"FAIL  no claude.exe: pass --claude <path>");
        return 1;
    }

    // A scratch folder for the harness scenarios' record and working folder.
    wchar_t temp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring{ temp } + L"agentmaster-harness-live";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

#ifndef AM_LIVE_NO_AGENT
    if (indexDays >= 0)
    {
        // A measurement, not a scenario: nothing else runs with it.
        RunIndexTiming(indexDays);
        std::filesystem::remove_all(dir, ec);
        return 0;
    }
#endif
    if (!onlyAgent && !onlyPermissions)
    {
        RunHarnessScenarios(claudeExe, model, dir);
    }
    if (!onlyAgent)
    {
        RunPermissionScenarios(claudeExe, model, dir);
    }
#ifndef AM_LIVE_NO_AGENT
    if (!agentRequest.empty())
    {
        RunAgentScenario(claudeExe, agentRequest);
    }
#endif

    std::filesystem::remove_all(dir, ec);
    Say(L"");
    Say(std::to_wstring(g_checks) + L" checks, " + std::to_wstring(g_failures) + L" failed");
    return g_failures;
}
