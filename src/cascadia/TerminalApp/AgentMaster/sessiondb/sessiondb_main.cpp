// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — agentmaster-sessiondb.exe, the session database service (SESSIONS_DB.md §4).
//
// Modes:
//   (no argument) | serve      the per-profile service: owns the DuckDB file, answers on the named pipe
//   mcp                        a stdio MCP server for Claude Code (a pipe client of the service, §8.4)
//   resync [--limit N]         one-shot: open the database, sweep, merge, print the status (diagnostics)
//   verify-summary [--limit N] one-shot: compare the database's summaries with AnalyzeSessionTranscript (D14)
//   search <query> [options]   one-shot: run one search on the database as it is (no ingest) and print the
//                              page with tiers, hits, snippets and the cold / warm times (diagnostics)
//   sql <statement> [--limit N] one-shot: run one statement and print its rows as text with the time
//   recluster                  one-shot: merge the hot tier and rewrite the big tables in key order
//   tool <name> [<json>]       one-shot: run one registry tool (SessionDbTools) and print its result
//   call <method> [<json>]     a client: send one protocol request to the profile's service (started when none
//                              runs) and print the answer
//   --version                  the build, schema and tool-registry versions
//
// The profile is the one AgentmasterStateDir() resolves (AGENTMASTER_PROFILE, set by whoever starts the
// service; else the per-identity default) unless --profile <dir> names one.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "SessionDbExtract.h" // the role values (search output)
#include "SessionDbLog.h"
#include "SessionDbSearch.h"
#include "SessionDbServer.h"
#include "SessionDbStore.h"
#include "SessionDbTools.h"
#include "SessionDbVerify.h"

#include "../ClaudeSpawn.h" // AgentmasterStateDir, ClaudeProjectsDir, InstallThrowStackCapture
#include "../SessionDbClient.h"
#include "../Persistence.h" // LoadAppSettings
#include "../SessionDbProtocol.h"

namespace Agentmaster::SessionDb
{
    // The mcp mode (SessionDbMcp.cpp); the serve mode is SessionDbServer.h's RunServe.
    int RunMcp(const std::wstring& profileDir);
}

namespace
{
    using namespace Agentmaster;
    using namespace Agentmaster::SessionDb;

    // Print UTF-16 text to stdout as UTF-8 (the console's code page may be anything).
    void Print(const std::wstring& text)
    {
        const std::string utf8 = ToUtf8(text);
        std::fwrite(utf8.data(), 1, utf8.size(), stdout);
        std::fflush(stdout);
    }

    // The store configuration for a profile: the service's (SessionDbServer.h), so the one-shot modes see
    // the database exactly as the service does.
    StoreConfig ConfigFor(const std::wstring& profileDir)
    {
        return ServiceConfigFor(profileDir);
    }

    int RunResync(const std::wstring& profileDir, size_t limit)
    {
        Store store{ ConfigFor(profileDir) };
        if (store.Open() == SchemaState::TooNew)
        {
            Print(L"the database belongs to a newer service; nothing done\n");
            return 3;
        }
        const ResyncResult rr = store.Resync({}, [](int64_t done, int64_t total) {
            if (done == total || done % 50 == 0)
            {
                Print(L"  " + std::to_wstring(done) + L" / " + std::to_wstring(total) + L" sessions\n");
            }
        }, limit);
        const size_t merged = store.Merge({});
        Connection con{ store.Db() };
        const StoreStatus st = store.Status(con);
        Print(L"resync: " + std::to_wstring(rr.sessionsUpserted) + L" session(s), " + std::to_wstring(rr.linesIngested) + L" line(s), " +
              std::to_wstring(rr.bytesRead / (1024 * 1024)) + L" MB in " + std::to_wstring(rr.ms) + L" ms; merged " + std::to_wstring(merged) +
              L" block(s)\n");
        Print(L"status: sessions " + std::to_wstring(st.sessions) + L", files " + std::to_wstring(st.files) + L", lines " + std::to_wstring(st.lines) +
              L" (+" + std::to_wstring(st.hotLines) + L" hot), blocks " + std::to_wstring(st.docs) + L" (+" + std::to_wstring(st.hotDocs) + L" hot), terms " +
              std::to_wstring(st.terms) + L", " + std::to_wstring(st.dbBytes / (1024 * 1024)) + L" MB on disk\n");
        return 0;
    }

    // One-shot tool call (diagnostics): run one registry tool with JSON arguments on the database as it is and
    // print the result (pretty) with its time; an error outcome prints its message and exits 1.
    int RunTool(const std::wstring& profileDir, const std::wstring& name, const std::wstring& argsJson)
    {
        Store store{ ConfigFor(profileDir) };
        if (store.Open() == SchemaState::TooNew)
        {
            Print(L"the database belongs to a newer service; nothing done\n");
            return 3;
        }
        SearchEngine engine{ store };
        Connection con{ store.Db() };
        const nlohmann::json args = argsJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(ToUtf8(argsJson), nullptr, false);
        if (args.is_discarded())
        {
            Print(L"the arguments are not JSON\n");
            return 2;
        }
        ToolContext ctx{ store, engine, con, {}, static_cast<int64_t>(::GetTickCount64()) };
        // The request clock: the wall clock in Unix ms (the tools print local times from it).
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        ctx.nowMs = static_cast<int64_t>(((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ULL - 11644473600000ULL);
        const ULONGLONG t0 = ::GetTickCount64();
        const ToolOutcome o = CallTool(ctx, ToUtf8(name), args);
        const ULONGLONG t1 = ::GetTickCount64();
        if (o.isError)
        {
            Print(L"error " + std::to_wstring(o.code) + L": " + FromUtf8(o.text) + L"\n");
            return 1;
        }
        Print(FromUtf8(o.result.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)) + L"\n(" + std::to_wstring(t1 - t0) + L" ms, " +
              std::to_wstring(o.text.size()) + L" bytes)\n");
        return 0;
    }

    // A client call (diagnostics, and the way a script talks to the service): connect to the profile's service
    // (starting one when none runs), send one request and print the answer envelope with its time.
    int RunCall(const std::wstring& profileDir, const std::wstring& method, const std::wstring& paramsJson)
    {
        ServiceClient client{ profileDir };
        std::wstring error;
        const ULONGLONG t0 = ::GetTickCount64();
        if (!client.Connect(true, 15000, error))
        {
            Print(L"error: " + error + L"\n");
            return 1;
        }
        const ULONGLONG t1 = ::GetTickCount64();
        std::string answer;
        if (!client.Call(ToUtf8(method), paramsJson.empty() ? std::string{ "{}" } : ToUtf8(paramsJson), answer, 600000, error))
        {
            Print(L"error: " + error + L"\n");
            return 1;
        }
        const ULONGLONG t2 = ::GetTickCount64();
        const nlohmann::json env = nlohmann::json::parse(answer, nullptr, false);
        Print((env.is_discarded() ? FromUtf8(answer) : FromUtf8(env.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) + L"\n(connect " +
              std::to_wstring(t1 - t0) + L" ms, call " + std::to_wstring(t2 - t1) + L" ms)\n");
        return env.is_object() && env.contains("error") ? 1 : 0;
    }

    // One-shot re-cluster (diagnostics): merge the hot tier, rewrite the big tables in key order, checkpoint.
    int RunRecluster(const std::wstring& profileDir)
    {
        Store store{ ConfigFor(profileDir) };
        if (store.Open() == SchemaState::TooNew)
        {
            Print(L"the database belongs to a newer service; nothing done\n");
            return 3;
        }
        const ULONGLONG t0 = ::GetTickCount64();
        const size_t merged = store.Merge({});
        const ULONGLONG t1 = ::GetTickCount64();
        store.Recluster({});
        const ULONGLONG t2 = ::GetTickCount64();
        store.Checkpoint();
        const ULONGLONG t3 = ::GetTickCount64();
        Print(L"recluster: merged " + std::to_wstring(merged) + L" block(s) in " + std::to_wstring(t1 - t0) + L" ms, rewrote the tables in " +
              std::to_wstring(t2 - t1) + L" ms, checkpoint " + std::to_wstring(t3 - t2) + L" ms\n");
        return 0;
    }

    // A block role as the search output names it.
    const wchar_t* RoleName(uint8_t role)
    {
        switch (role)
        {
        case kRolePrompt:
            return L"prompt";
        case kRoleReply:
            return L"reply";
        case kRoleThinking:
            return L"thinking";
        case kRoleToolCall:
            return L"tool call";
        case kRoleToolResult:
            return L"tool result";
        case kRoleSystem:
            return L"system";
        case kRoleQueued:
            return L"queued";
        default:
            return L"?";
        }
    }

    // One-shot search (diagnostics): run the request twice — the first run builds the session cards, the
    // second shows the steady-state cost — and print the page with its tiers, hits and snippets.
    int RunSearch(const std::wstring& profileDir, const SearchRequest& req)
    {
        Store store{ ConfigFor(profileDir) };
        if (store.Open() == SchemaState::TooNew)
        {
            Print(L"the database belongs to a newer service; nothing done\n");
            return 3;
        }
        SearchEngine engine{ store };
        Connection con{ store.Db() };
        const ULONGLONG t0 = ::GetTickCount64();
        (void)engine.Search(con, req);
        const ULONGLONG t1 = ::GetTickCount64();
        const SearchResult res = engine.Search(con, req);
        const ULONGLONG t2 = ::GetTickCount64();
        static const wchar_t* const kTier[] = { L"name", L"path", L"prompt", L"text", L"listed" };
        Print(L"search \"" + req.query + L"\": " + std::to_wstring(res.matched) + L" of " + std::to_wstring(res.searched) + L" session(s) match" +
              (res.partial ? L" (partial)" : L"") + L"; first run " + std::to_wstring(t1 - t0) + L" ms, second " + std::to_wstring(t2 - t1) + L" ms\n");
        if (!res.note.empty())
        {
            Print(L"note: " + res.note + L"\n");
        }
        Print(L"steps (second run): " + res.timings + L"\n");
        size_t n = req.offset;
        for (const SearchRow& row : res.rows)
        {
            const SessionCard& c = res.cards->cards[res.cards->bySid.at(row.sidKey)];
            std::wstring title = c.title.substr(0, 70);
            for (auto& ch : title)
            {
                ch = (ch == L'\n' || ch == L'\r' || ch == L'\t') ? L' ' : ch;
            }
            wchar_t score[32];
            swprintf_s(score, L"%.2f", row.score);
            Print(L"#" + std::to_wstring(++n) + L"  " + kTier[std::min<size_t>(row.tier, 4)] + L"  hits " + std::to_wstring(row.hits) + L"  score " + score +
                  L"  " + c.shortId + L"  \"" + title + L"\"  " + c.cwd + L"\n");
            for (const SearchSnippet& s : row.snippets)
            {
                Print(L"      [" + std::wstring{ RoleName(s.role) } + (s.subagent ? L", subagent" : L"") + (s.history ? L", history.jsonl" : L"") + L" " +
                      std::to_wstring(s.lineNo) + L"." + std::to_wstring(s.block) + L"] " + s.text + L"\n");
            }
        }
        if (res.hasMore)
        {
            Print(L"(more rows past this page)\n");
        }
        return 0;
    }

    // One-shot SQL (diagnostics; it CAN write — never point it at a live profile): run one statement on the
    // database and print its rows tab-separated with the time. A query is wrapped so every column prints as
    // text (`SELECT COLUMNS(*)::VARCHAR FROM (...)`); any other statement runs as given.
    int RunSql(const std::wstring& profileDir, const std::wstring& sql, size_t limit)
    {
        Store store{ ConfigFor(profileDir) };
        if (store.Open() == SchemaState::TooNew)
        {
            Print(L"the database belongs to a newer service; nothing done\n");
            return 3;
        }
        Connection con{ store.Db() };
        std::string q = ToUtf8(sql);
        std::string head = q.substr(0, 16);
        for (auto& ch : head)
        {
            ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
        }
        // Only a query is wrapped; any other statement (EXPLAIN, PRAGMA, SET, CREATE, ...) runs as given.
        const bool raw = !(head.rfind("SELECT", 0) == 0 || head.rfind("WITH", 0) == 0 || head.rfind("FROM", 0) == 0 || head.rfind("VALUES", 0) == 0);
        if (!raw)
        {
            q = "SELECT COLUMNS(*)::VARCHAR FROM (" + q + ")";
        }
        const ULONGLONG t0 = ::GetTickCount64();
        auto r = con.Query(q);
        size_t rows = 0;
        std::wstring out;
        for (size_t c = 0; c < r.ColumnCount(); ++c)
        {
            out += (c ? L"\t" : L"") + FromUtf8(r.ColumnName(c));
        }
        out += L"\n";
        while (r.Next())
        {
            if (limit && rows >= limit)
            {
                ++rows;
                continue;
            }
            for (size_t c = 0; c < r.ColumnCount(); ++c)
            {
                out += (c ? L"\t" : L"") + (r.IsNull(c) ? std::wstring{ L"NULL" } : FromUtf8(r.Str(c)));
            }
            out += L"\n";
            ++rows;
        }
        const ULONGLONG t1 = ::GetTickCount64();
        Print(out);
        Print(L"(" + std::to_wstring(rows) + L" row(s), " + std::to_wstring(t1 - t0) + L" ms)\n");
        return 0;
    }

    int RunVerify(const std::wstring& profileDir, size_t limit)
    {
        Store store{ ConfigFor(profileDir) };
        if (store.Open() == SchemaState::TooNew)
        {
            Print(L"the database belongs to a newer service; nothing done\n");
            return 3;
        }
        const VerifyReport rep = VerifySummaries(store, limit);
        Print(rep.text);
        return rep.compared == rep.allAgree ? 0 : 1;
    }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace Agentmaster;
    using namespace Agentmaster::SessionDb;
    InstallThrowStackCapture(); // Rule #18: swallowed exceptions keep their throw-site stacks
    std::vector<std::wstring> args(argv + 1, argv + argc);
    std::wstring mode = args.empty() ? L"serve" : args[0];
    std::wstring profileDir;
    size_t limit = 0;
    SearchRequest search; // the `search` mode's request: the words that are no option form the query
    std::wstring toolName, toolArgs; // the `tool` mode's tool and JSON arguments
    for (size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == L"--profile" && i + 1 < args.size())
        {
            profileDir = args[++i];
        }
        else if (args[i] == L"--limit" && i + 1 < args.size())
        {
            limit = static_cast<size_t>(_wtoi64(args[++i].c_str()));
            search.limit = limit;
        }
        else if (args[i] == L"--fuzzy")
        {
            search.fuzzy = true;
        }
        else if (args[i] == L"--tools")
        {
            search.scopeTool = true;
        }
        else if (args[i] == L"--subagents")
        {
            search.scopeSubagent = true;
        }
        else if (args[i] == L"--no-replies")
        {
            search.scopeReply = false;
        }
        else if (args[i] == L"--no-prompts")
        {
            search.scopePrompt = false;
        }
        else if (args[i] == L"--dir" && i + 1 < args.size())
        {
            search.dir = args[++i];
        }
        else if (args[i] == L"--sort" && i + 1 < args.size())
        {
            const std::wstring s = args[++i];
            search.sort = s == L"active" ? SearchRequest::Sort::Active : s == L"created" ? SearchRequest::Sort::Created : SearchRequest::Sort::Relevance;
        }
        else if (mode == L"search" || mode == L"sql")
        {
            search.query += (search.query.empty() ? L"" : L" ") + args[i];
        }
        else if (mode == L"tool" || mode == L"call")
        {
            // tool <name> [<json arguments>]: the first free word names the tool, the rest is its JSON.
            (toolName.empty() ? toolName : toolArgs) += (toolName.empty() || toolArgs.empty() ? L"" : L" ") + args[i];
        }
    }
    if (mode == L"--version")
    {
        Print(L"agentmaster-sessiondb build " + std::to_wstring(SessionDbProtocol::kServiceBuild) + L", schema " +
              std::to_wstring(SessionDbProtocol::kSchemaVersion) + L", tools " + std::to_wstring(SessionDbProtocol::kToolRegistryVersion) +
              L", protocol " + std::to_wstring(SessionDbProtocol::kProtocolVersion) + L", duckdb " + FromUtf8(duckdb_library_version()) + L"\n");
        return 0;
    }
    if (!profileDir.empty())
    {
        // Every engine function (the logs, the settings, the session store) resolves the profile through
        // AgentmasterStateDir(), which honors AGENTMASTER_PROFILE: point it at the requested folder first.
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", profileDir.c_str());
    }
    profileDir = AgentmasterStateDir();
    const bool oneShot = mode == L"resync" || mode == L"verify-summary" || mode == L"search" || mode == L"sql" || mode == L"recluster" || mode == L"tool" ||
                         mode == L"call";
    ::CreateDirectoryW(SessionDbProtocol::ServiceDir(profileDir).c_str(), nullptr);
    InitServiceLog(SessionDbProtocol::LogPath(profileDir), oneShot);
    try
    {
        if (mode == L"serve")
        {
            return RunServe(profileDir, limit);
        }
        if (mode == L"mcp")
        {
            return RunMcp(profileDir);
        }
        if (mode == L"resync")
        {
            return RunResync(profileDir, limit);
        }
        if (mode == L"verify-summary")
        {
            return RunVerify(profileDir, limit);
        }
        if (mode == L"search")
        {
            return RunSearch(profileDir, search);
        }
        if (mode == L"sql")
        {
            return RunSql(profileDir, search.query, limit);
        }
        if (mode == L"recluster")
        {
            return RunRecluster(profileDir);
        }
        if (mode == L"tool")
        {
            return RunTool(profileDir, toolName, toolArgs);
        }
        if (mode == L"call")
        {
            return RunCall(profileDir, toolName, toolArgs);
        }
        Print(L"usage: agentmaster-sessiondb [serve | mcp | resync [--limit N] | verify-summary [--limit N] |\n"
              L"       search <query> [--fuzzy] [--tools] [--subagents] [--no-replies] [--no-prompts] [--dir D] [--sort relevance|active|created] [--limit N] |\n"
              L"       sql <statement> [--limit N] | recluster | tool <name> [<json>] | call <method> [<json>] |\n"
              L"       --version] [--profile <dir>]\n");
        return 2;
    }
    catch (const std::exception& e)
    {
        LogSwallowedException(L"agentmaster-sessiondb main");
        ServiceLog(L"[sessdb] " + mode + L" failed: " + FromUtf8(e.what()));
        Print(L"error: " + FromUtf8(e.what()) + L"\n");
        return 1;
    }
}
