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

#include "SessionDbLog.h"
#include "SessionDbStore.h"
#include "SessionDbVerify.h"

#include "../ClaudeSpawn.h" // AgentmasterStateDir, ClaudeProjectsDir, InstallThrowStackCapture
#include "../Persistence.h" // LoadAppSettings
#include "../SessionDbProtocol.h"

namespace Agentmaster::SessionDb
{
    // The serve and mcp modes (SessionDbServer.cpp, SessionDbMcp.cpp).
    int RunServe(const std::wstring& profileDir);
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

    // The <claude config>\history.jsonl beside the projects folder.
    std::wstring HistoryPathFor(const std::wstring& projectsDir)
    {
        const size_t slash = projectsDir.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring{} : projectsDir.substr(0, slash) + L"\\history.jsonl";
    }

    // The store configuration for a profile: DuckDB's limits (§10), the inferred-folder setting.
    StoreConfig ConfigFor(const std::wstring& profileDir)
    {
        StoreConfig cfg;
        cfg.profileDir = profileDir;
        cfg.projectsDir = ClaudeProjectsDir();
        cfg.historyPath = HistoryPathFor(cfg.projectsDir);
        const AppSettings settings = LoadAppSettings();
        cfg.inferGitRoot = settings.inferGitRoot;
        cfg.db.threads = std::to_string(ClampSessionDbThreads(settings.sessionDbThreads));
        cfg.db.memoryLimit = std::to_string(ClampSessionDbMemoryMb(settings.sessionDbMemoryMb)) + "MB";
        cfg.log = [](const std::wstring& line) { ServiceLog(line); };
        return cfg;
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
    for (size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == L"--profile" && i + 1 < args.size())
        {
            profileDir = args[++i];
        }
        else if (args[i] == L"--limit" && i + 1 < args.size())
        {
            limit = static_cast<size_t>(_wtoi64(args[++i].c_str()));
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
    const bool oneShot = mode == L"resync" || mode == L"verify-summary";
    ::CreateDirectoryW(SessionDbProtocol::ServiceDir(profileDir).c_str(), nullptr);
    InitServiceLog(SessionDbProtocol::LogPath(profileDir), oneShot);
    try
    {
        if (mode == L"serve")
        {
            return RunServe(profileDir);
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
        Print(L"usage: agentmaster-sessiondb [serve | mcp | resync [--limit N] | verify-summary [--limit N] | --version] [--profile <dir>]\n");
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
