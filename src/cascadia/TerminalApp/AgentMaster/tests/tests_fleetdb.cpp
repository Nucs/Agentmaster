// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness — tests_fleetdb.cpp
// SESSIONS_STORE.md: the FLEET STORE (sessions.db, FleetDb.{h,cpp} + FleetWriter.cpp), against real SQLite databases in
// scratch folders under %TEMP% (never the harness's profile, never a live one):
//   TestFleetDbRows      - the database: the row round trip (texts, NULs, lone surrogates, a large prompt, duplicate and
//                          empty prompt ids), the diff (exactly the changed rows), the prompts' order numbers, removals,
//                          the residue per row, the writer lock, the format guard, unreadable and orphan rows kept aside,
//                          a reader beside the writer and after it.
//   TestFleetStoreStartup - OpenFleetForStartupIn: a fresh profile, the one-time migration (backup, verification, the
//                          record, sessions.json removed last), a planted mismatch that must stop it (negative control), an
//                          interrupted last step, a sessions.json written after the move, an unusable store (never written
//                          over: the run saves sessions.json beside it), an unusable sessions.json, the format guard.
//   TestFleetWriter      - the writer over a test registry: every mutator marks its record, the commits, the bulk scope,
//                          the critical commits' answers, the Json and read-only modes.
//   TestFleetKills       - REAL kills of child processes (this exe in a child mode): mid-commit (every acknowledged commit
//                          survives, each commit whole) and mid-migration (at each step boundary and at random moments: the
//                          fleet is never lost, and sessions.json goes only once the store holds it verified).
// RunFleetTestChild is the child side (m5_tests.cpp's wmain dispatches "--fleet-child" to it).
// ======================================================================================

#include "m5_tests.h"

#include "../FleetDb.h"
#include "../Sha256.h"
#include "../StateBackups.h"

#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <set>

namespace
{
    namespace fs = std::filesystem;

    /// <summary>
    /// A scratch folder for one block, removed when the block ends. Its destructor first forgets the process-wide store
    /// (ResetFleetStoreForTests closes the connection and releases sessions.db.lock), so the folder can be removed; declare it
    /// FIRST in a block, so every FleetDb the block opens itself is closed before it.
    /// </summary>
    struct FleetScratch
    {
        std::wstring dir;

        /// <summary>Creates an empty folder %TEMP%\&lt;tag&gt;&lt;pid&gt;-&lt;n&gt;.</summary>
        /// <param name="tag">The folder name's prefix.</param>
        explicit FleetScratch(const wchar_t* tag)
        {
            static std::atomic<int> seq{ 0 };
            wchar_t tmp[MAX_PATH]{};
            ::GetTempPathW(MAX_PATH, tmp);
            dir = std::wstring{ tmp } + tag + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++seq);
            std::error_code ec;
            fs::remove_all(fs::path{ dir }, ec);
            fs::create_directories(fs::path{ dir }, ec);
        }
        ~FleetScratch()
        {
            ResetFleetStoreForTests();
            SetFleetMigrationStepHookForTests(nullptr);
            std::error_code ec;
            fs::remove_all(fs::path{ dir }, ec);
        }
        FleetScratch(const FleetScratch&) = delete;
        FleetScratch& operator=(const FleetScratch&) = delete;
    };

    /// <summary>A file's bytes ("" when it cannot be read).</summary>
    /// <param name="path">The file.</param>
    /// <returns>Its bytes.</returns>
    std::string FtReadBytes(const std::wstring& path)
    {
        std::ifstream f(fs::path{ path }, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    /// <summary>Writes a file's bytes (replacing it).</summary>
    /// <param name="path">The file.</param>
    /// <param name="bytes">Its new bytes.</param>
    void FtWriteBytes(const std::wstring& path, const std::string& bytes)
    {
        std::ofstream f(fs::path{ path }, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    /// <summary>UTF-16 → UTF-8 (a lone surrogate becomes U+FFFD, as every state writer makes it).</summary>
    /// <param name="w">The text.</param>
    /// <returns>Its UTF-8 bytes.</returns>
    std::string FtUtf8(const std::wstring& w)
    {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        }
        return out;
    }

    /// <summary>Whether a file exists.</summary>
    bool FtExists(const std::wstring& path)
    {
        std::error_code ec;
        return fs::exists(fs::path{ path }, ec);
    }

    /// <summary>A session's canonical text (sorted keys): two records are equal exactly when these are.</summary>
    /// <param name="s">The record.</param>
    /// <returns>Its canonical JSON.</returns>
    std::wstring FtCanon(const SessionInfo& s)
    {
        return CanonicalJsonText(ToJson(s));
    }

    /// <summary>
    /// A session with `prompts` queued prompts: ids "&lt;id&gt;-p&lt;k&gt;", texts that hold a newline, a quote and non-ASCII
    /// characters, the first half Sent and the rest Pending.
    /// </summary>
    /// <param name="id">The session id.</param>
    /// <param name="title">Its title.</param>
    /// <param name="prompts">How many prompts.</param>
    /// <returns>The record.</returns>
    SessionInfo FtSession(const std::wstring& id, const std::wstring& title, int prompts)
    {
        SessionInfo s = MakeSession(id);
        s.title = title;
        s.workingDir = L"C:\\work\\" + id;
        s.createdUnixMs = 1763600000123;
        for (int k = 0; k < prompts; ++k)
        {
            QueuedPrompt p;
            p.id = id + L"-p" + std::to_wstring(k);
            p.label = L"prompt " + std::to_wstring(k);
            p.text = L"line one of " + std::to_wstring(k) + L"\nsecond \"line\" \x05D0\x05D1 \U0001F600 end";
            p.status = k < prompts / 2 ? PromptStatus::Sent : PromptStatus::Pending;
            p.sentAtUnixMs = k < prompts / 2 ? 1763600000000 + k : 0;
            s.queue.push_back(std::move(p));
        }
        return s;
    }

    /// <summary>One FleetChange that writes `s`.</summary>
    FleetChange FtPut(const SessionInfo& s)
    {
        return FleetChange{ s.id, s };
    }

    /// <summary>One FleetChange that removes the session `id`.</summary>
    FleetChange FtRemove(const std::wstring& id)
    {
        return FleetChange{ id, std::nullopt };
    }

    /// <summary>Opens a database ReadWrite (creating it), or null.</summary>
    std::unique_ptr<FleetDb> FtOpen(const std::wstring& path, FleetDbAccess access = FleetDbAccess::ReadWrite, bool create = true)
    {
        std::wstring problem;
        return FleetDb::Open(path, access, create, problem);
    }

    /// <summary>The whole fleet a NEW connection reads from `path` (its own read, not the writer's cache).</summary>
    /// <param name="path">The database file.</param>
    /// <param name="problem">Receives what LoadAll reported (skipped rows).</param>
    /// <returns>The sessions (empty when the open failed).</returns>
    std::vector<SessionInfo> FtLoadFresh(const std::wstring& path, std::wstring* problem = nullptr)
    {
        std::vector<SessionInfo> out;
        auto db = FtOpen(path, FleetDbAccess::ReadOnly, false);
        std::wstring p;
        if (db)
        {
            db->LoadAll(out, p);
        }
        if (problem)
        {
            *problem = p;
        }
        return out;
    }

    /// <summary>
    /// The session with `id` in a list (nullptr when absent). ⚠ The pointer is into `fleet`: never keep it past a temporary
    /// list (FtTitleIn copies the title out instead).
    /// </summary>
    /// <param name="fleet">The list.</param>
    /// <param name="id">The session id.</param>
    /// <returns>The session, or nullptr.</returns>
    const SessionInfo* FtFind(const std::vector<SessionInfo>& fleet, const std::wstring& id)
    {
        for (const auto& s : fleet)
        {
            if (s.id == id)
            {
                return &s;
            }
        }
        return nullptr;
    }

    /// <summary>The title of the session `id` in a list, copied out ("&lt;absent&gt;" when the list does not hold it).</summary>
    /// <param name="fleet">The list (a temporary is fine: nothing points into it afterwards).</param>
    /// <param name="id">The session id.</param>
    /// <returns>The title.</returns>
    std::wstring FtTitleIn(const std::vector<SessionInfo>& fleet, const std::wstring& id)
    {
        const SessionInfo* s = FtFind(fleet, id);
        return s ? s->title : std::wstring{ L"<absent>" };
    }

    /// <summary>Runs SQL on a database file through SQLite directly (the tests' way to plant damage). True on success.</summary>
    bool FtRawExec(const std::wstring& path, const char* sql)
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(FtUtf8(path).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        {
            sqlite3_close(db);
            return false;
        }
        sqlite3_busy_timeout(db, 5000);
        const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(db);
        return ok;
    }

    /// <summary>The first two text columns of every row a query returns (read through SQLite directly).</summary>
    std::vector<std::pair<std::string, std::string>> FtRawRows(const std::wstring& path, const char* sql)
    {
        std::vector<std::pair<std::string, std::string>> out;
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(FtUtf8(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            sqlite3_close(db);
            return out;
        }
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step(s) == SQLITE_ROW)
            {
                const auto col = [&](int c) {
                    const unsigned char* t = sqlite3_column_text(s, c);
                    return t ? std::string{ reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s, c)) } : std::string{};
                };
                out.emplace_back(col(0), sqlite3_column_count(s) > 1 ? col(1) : std::string{});
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        return out;
    }

    /// <summary>
    /// The source document of a migration test: `count` sessions (FtSession, `prompts` each) serialized by the app's own
    /// writer, plus what a real file can hold that this build must still carry over unchanged: a newer build's member on a
    /// session and on a prompt, a non-object element, a session repeating an earlier id, and one with an empty id.
    /// </summary>
    /// <param name="count">The ordinary sessions.</param>
    /// <param name="prompts">Prompts per session.</param>
    /// <returns>The file's bytes (UTF-8, no BOM).</returns>
    std::string FtMigrationSource(int count, int prompts)
    {
        std::vector<SessionInfo> fleet;
        for (int i = 0; i < count; ++i)
        {
            fleet.push_back(FtSession(L"mig-" + std::to_wstring(i), L"Session " + std::to_wstring(i), prompts));
        }
        auto doc = json::Parse(SerializeSessions(fleet));
        if (!doc)
        {
            return {};
        }
        for (auto& [key, value] : doc->members)
        {
            if (key != L"sessions" || value.type != json::Value::Type::Arr || value.arr.empty())
            {
                continue;
            }
            value.arr[0].Set(L"futureMember", json::Value::MkStr(L"kept by the store"));
            for (auto& [k2, v2] : value.arr[0].members)
            {
                if (k2 == L"queue" && v2.type == json::Value::Type::Arr && !v2.arr.empty())
                {
                    v2.arr[0].Set(L"futurePromptMember", json::Value::MkNum(42));
                }
            }
            value.arr.push_back(json::Value::MkNum(7)); // a non-object element
            json::Value dup = value.arr[0]; // a repeated id (a damaged file; the restore keeps the last)
            dup.Set(L"note", json::Value::MkStr(L"the duplicate"));
            value.arr.push_back(std::move(dup));
            json::Value empty = json::Value::MkObj();
            empty.Set(L"id", json::Value::MkStr(L""));
            empty.Set(L"title", json::Value::MkStr(L"no id"));
            value.arr.push_back(std::move(empty));
        }
        return FtUtf8(json::Dump(*doc));
    }

    /// <summary>The sessions a sessions.json holds, with an id, in file order (what the migrated store must equal).</summary>
    std::vector<SessionInfo> FtSessionsWithIds(std::vector<SessionInfo> fleet)
    {
        fleet.erase(std::remove_if(fleet.begin(), fleet.end(), [](const SessionInfo& s) { return s.id.empty(); }), fleet.end());
        return fleet;
    }

    /// <summary>Whether two fleets hold the same records in the same order (canonical comparison).</summary>
    bool FtSameFleet(const std::vector<SessionInfo>& a, const std::vector<SessionInfo>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (FtCanon(a[i]) != FtCanon(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>The migration backup folders under &lt;profile&gt;\backups (README first line = the migration marker).</summary>
    std::vector<std::wstring> FtMigrationBackups(const std::wstring& profile)
    {
        std::vector<std::wstring> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator{ fs::path{ profile + L"\\backups" }, ec })
        {
            const std::string readme = FtReadBytes(e.path().wstring() + L"\\README.txt");
            if (readme.rfind(FtUtf8(std::wstring{ kFleetMigrationBackupMarker }), 0) == 0)
            {
                out.push_back(e.path().wstring());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /// <summary>Copies every file of a folder (not recursive) into another, created.</summary>
    void FtCopyFiles(const std::wstring& from, const std::wstring& to)
    {
        std::error_code ec;
        fs::create_directories(fs::path{ to }, ec);
        for (const auto& e : fs::directory_iterator{ fs::path{ from }, ec })
        {
            if (e.is_regular_file(ec))
            {
                fs::copy_file(e.path(), fs::path{ to } / e.path().filename(), fs::copy_options::overwrite_existing, ec);
            }
        }
    }

    /// <summary>This harness exe's own path (the child processes run it).</summary>
    std::wstring FtSelfExe()
    {
        wchar_t buf[MAX_PATH * 4]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        return std::wstring{ buf, n };
    }

    /// <summary>A started child process with its stdout piped to the parent.</summary>
    struct FtChild
    {
        HANDLE process{ nullptr };
        HANDLE out{ nullptr }; // the read end of the child's stdout
        std::string pending; // a line read only in part, carried to the next FtReadAcks (an ack split across two reads)
        ~FtChild()
        {
            if (out)
            {
                ::CloseHandle(out);
            }
            if (process)
            {
                ::CloseHandle(process);
            }
        }
    };

    /// <summary>Starts this exe as "--fleet-child &lt;args&gt;", stdout piped. False when it could not start.</summary>
    bool FtStartChild(const std::wstring& args, FtChild& child)
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0))
        {
            return false;
        }
        ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0); // only the write end goes to the child
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + FtSelfExe() + L"\" --fleet-child " + args;
        const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        ::CloseHandle(writeEnd); // the parent keeps no write end: the pipe ends when the child does
        if (!ok)
        {
            ::CloseHandle(readEnd);
            return false;
        }
        ::CloseHandle(pi.hThread);
        child.process = pi.hProcess;
        child.out = readEnd;
        return true;
    }

    /// <summary>
    /// Reads the child's "ack N" lines until `wantAcks` were seen (or the child ended). A line read only in part waits in
    /// child.pending for the next call, so no ack is lost at a read boundary. Returns the last N read (0 = none).
    /// </summary>
    int64_t FtReadAcks(FtChild& child, int64_t wantAcks)
    {
        std::string& buf = child.pending;
        int64_t last = 0;
        int64_t seen = 0;
        char chunk[512];
        while (seen < wantAcks)
        {
            DWORD got = 0;
            if (!::ReadFile(child.out, chunk, sizeof(chunk), &got, nullptr) || got == 0)
            {
                break; // the child ended
            }
            buf.append(chunk, got);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos)
            {
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.rfind("ack ", 0) == 0)
                {
                    last = std::strtoll(line.c_str() + 4, nullptr, 10);
                    ++seen;
                }
            }
        }
        return last;
    }

    /// <summary>Kills a child at once (as a crash would) and waits for it. Returns its exit code.</summary>
    DWORD FtKill(FtChild& child, UINT code)
    {
        ::TerminateProcess(child.process, code);
        ::WaitForSingleObject(child.process, 10000);
        DWORD exitCode = 0;
        ::GetExitCodeProcess(child.process, &exitCode);
        return exitCode;
    }

    /// <summary>Waits for a child to end by itself (up to `ms`). Returns its exit code (STILL_ACTIVE on a timeout).</summary>
    DWORD FtWait(FtChild& child, DWORD ms)
    {
        ::WaitForSingleObject(child.process, ms);
        DWORD exitCode = STILL_ACTIVE;
        ::GetExitCodeProcess(child.process, &exitCode);
        return exitCode;
    }

    /// <summary>
    /// The kill test's record at commit `i`: the title says i, and the queue holds prompts p(max(1, i-19))..p(i) — so a
    /// commit changes the doc row, adds a prompt and (past 20) removes the oldest: a torn commit would show at once.
    /// </summary>
    SessionInfo FtKillRecord(int64_t i)
    {
        SessionInfo s = MakeSession(L"kill-0000");
        s.title = L"commit " + std::to_wstring(i);
        s.workingDir = L"C:\\kill";
        for (int64_t j = (std::max)(int64_t{ 1 }, i - 19); j <= i; ++j)
        {
            QueuedPrompt p;
            p.id = L"p" + std::to_wstring(j);
            p.text = L"text of " + std::to_wstring(j) + std::wstring(300, L'x'); // spills past a 512-byte page
            p.status = j < i ? PromptStatus::Sent : PromptStatus::Pending;
            s.queue.push_back(std::move(p));
        }
        return s;
    }
}

// ===================================================================================================================
// The child side.
// ===================================================================================================================

int RunFleetTestChild(int argc, wchar_t** argv)
{
    // argv: <exe> --fleet-child <mode> ...
    if (argc < 4)
    {
        return 2;
    }
    const std::wstring mode = argv[2];
    if (mode == L"commit")
    {
        // Commits FtKillRecord(1), (2), ... durably, printing "ack N" after each commit returned, until it is killed.
        std::wstring problem;
        auto db = FleetDb::Open(argv[3], FleetDbAccess::ReadWrite, true, problem);
        if (!db)
        {
            return 3;
        }
        for (int64_t i = 1; i < 1000000; ++i)
        {
            if (!db->Write({ FtPut(FtKillRecord(i)) }, true).ok)
            {
                return 4;
            }
            std::printf("ack %lld\n", static_cast<long long>(i));
            std::fflush(stdout);
        }
        return 0;
    }
    if (mode == L"migrate" && argc >= 5)
    {
        // The startup open of a profile, killed (TerminateProcess: no unwinding, no flush — a crash) at the migration step
        // named, or by the parent at a random moment (step 0).
        const int step = static_cast<int>(std::wcstol(argv[4], nullptr, 10));
        if (step > 0)
        {
            SetFleetMigrationStepHookForTests([step](int at) {
                if (at == step)
                {
                    ::TerminateProcess(::GetCurrentProcess(), 77);
                }
            });
        }
        const FleetLoad load = OpenFleetForStartupIn(argv[3]);
        return load.usable ? 0 : 5;
    }
    return 2;
}

// ===================================================================================================================
// TestFleetDbRows
// ===================================================================================================================

void TestFleetDbRows()
{
    std::wprintf(L"Fleet store rows (sessions.db: round trip, the diff, order numbers, residue, lock, format guard, damage):\n");

    // ---- the prompts' order numbers (pure) ----
    {
        const int64_t g = int64_t{ 1 } << 16;
        using O = std::optional<int64_t>;
        const auto rising = [](const std::vector<int64_t>& v) {
            for (size_t i = 1; i < v.size(); ++i)
            {
                if (v[i] <= v[i - 1])
                {
                    return false;
                }
            }
            return true;
        };
        CHECK(PlanFleetPromptOrdsForTests({}).empty(), "ords: an empty queue");
        CHECK((PlanFleetPromptOrdsForTests({ O{}, O{}, O{} }) == std::vector<int64_t>{ g, 2 * g, 3 * g }), "ords: a new queue is spaced by the gap");
        CHECK((PlanFleetPromptOrdsForTests({ O{ 2 * g }, O{ 3 * g } }) == std::vector<int64_t>{ 2 * g, 3 * g }),
              "ords: the oldest removed (the history cap) - every other prompt keeps its number (no rewrite)");
        CHECK((PlanFleetPromptOrdsForTests({ O{ g }, O{ 2 * g }, O{} }) == std::vector<int64_t>{ g, 2 * g, 3 * g }), "ords: an appended prompt goes a gap past the last");
        const auto front = PlanFleetPromptOrdsForTests({ O{}, O{ g }, O{ 2 * g } });
        CHECK(front.size() == 3 && front[1] == g && front[2] == 2 * g && front[0] < g, "ords: a prompt put first takes a number below the first, the rest keep theirs");
        const auto moved = PlanFleetPromptOrdsForTests({ O{ g }, O{ 3 * g }, O{ 2 * g }, O{ 4 * g } }); // a Move Up of the third
        int keptCount = 0;
        for (size_t i = 0; i < moved.size(); ++i)
        {
            const int64_t before[] = { g, 3 * g, 2 * g, 4 * g };
            keptCount += moved[i] == before[i] ? 1 : 0;
        }
        CHECK(rising(moved) && keptCount == 3, "ords: a Move Up renumbers ONE prompt, between its new neighbours");
        const auto tight = PlanFleetPromptOrdsForTests({ O{ 10 }, O{}, O{ 11 } });
        CHECK((tight == std::vector<int64_t>{ g, 2 * g, 3 * g }), "ords: no room between two neighbours - the queue is renumbered by the gap");
        const auto huge = PlanFleetPromptOrdsForTests({ O{ int64_t{ 1 } << 62 }, O{} });
        CHECK((huge == std::vector<int64_t>{ g, 2 * g }), "ords: a stored number out of range (damage) renumbers the queue");
        // Fuzz: random edits of a stored queue (removals, inserts, moves); the plan always rises and keeps as many stored
        // numbers as the longest rising run of them (nothing renumbered without need).
        std::mt19937 rng{ 20261120 };
        bool allRise = true;
        bool keptMost = true;
        for (int round = 0; round < 400; ++round)
        {
            std::vector<O> cur;
            const int n = static_cast<int>(rng() % 30);
            for (int i = 0; i < n; ++i)
            {
                cur.push_back(O{ (i + 1) * g });
            }
            for (int e = 0; e < 4; ++e)
            {
                const int op = static_cast<int>(rng() % 3);
                if (op == 0 && !cur.empty())
                {
                    cur.erase(cur.begin() + static_cast<std::ptrdiff_t>(rng() % cur.size()));
                }
                else if (op == 1)
                {
                    cur.insert(cur.begin() + static_cast<std::ptrdiff_t>(cur.empty() ? 0 : rng() % (cur.size() + 1)), O{});
                }
                else if (cur.size() > 1)
                {
                    const size_t a = rng() % cur.size();
                    const size_t b = rng() % cur.size();
                    std::swap(cur[a], cur[b]);
                }
            }
            const auto plan = PlanFleetPromptOrdsForTests(cur);
            allRise = allRise && plan.size() == cur.size() && rising(plan);
            // The longest strictly rising subsequence of the stored numbers, by brute force (n <= 34).
            std::vector<int64_t> stored;
            std::vector<size_t> at;
            for (size_t i = 0; i < cur.size(); ++i)
            {
                if (cur[i])
                {
                    stored.push_back(*cur[i]);
                    at.push_back(i);
                }
            }
            std::vector<int> best(stored.size(), 1);
            int lis = 0;
            for (size_t i = 0; i < stored.size(); ++i)
            {
                for (size_t j = 0; j < i; ++j)
                {
                    if (stored[j] < stored[i])
                    {
                        best[i] = (std::max)(best[i], best[j] + 1);
                    }
                }
                lis = (std::max)(lis, best[i]);
            }
            int kept = 0;
            for (size_t k = 0; k < at.size(); ++k)
            {
                kept += plan[at[k]] == stored[k] ? 1 : 0;
            }
            const bool renumbered = !plan.empty() && plan[0] == g && (plan.size() < 2 || plan[1] == 2 * g);
            keptMost = keptMost && (kept >= lis || renumbered);
        }
        CHECK(allRise, "ords fuzz: 400 random edits - the planned numbers always rise strictly");
        CHECK(keptMost, "ords fuzz: ...and keep every number of the longest rising run (or renumber only when there is no room)");
    }

    // ---- canonical JSON ----
    {
        const auto a = json::Parse(L"{\"b\":1,\"a\":[0.1,1e300,-0.5,123456789012345678],\"c\":{\"y\":\"\\u0001\",\"x\":null}}");
        const auto b = json::Parse(L"{\"c\":{\"x\":null,\"y\":\"\\u0001\"},\"a\":[0.1,1e300,-0.5,123456789012345678],\"b\":1}");
        CHECK(a && b && CanonicalJsonText(*a) == CanonicalJsonText(*b), "canonical: member order does not matter");
        const auto c = json::Parse(CanonicalJsonText(*a));
        CHECK(c && CanonicalJsonText(*c) == CanonicalJsonText(*a), "canonical: its text parses back to the same value (17 significant digits)");
        const auto d = json::Parse(L"{\"b\":2,\"a\":[0.1,1e300,-0.5,123456789012345678],\"c\":{\"y\":\"\\u0001\",\"x\":null}}");
        CHECK(d && CanonicalJsonText(*d) != CanonicalJsonText(*a), "canonical: a changed value differs");
    }

    // ---- create, write, read back in a NEW connection ----
    {
        FleetScratch sp(L"am-fleet-rows-");
        const std::wstring path = sp.dir + L"\\sessions.db";
        auto db = FtOpen(path);
        CHECK(db && !db->ReadOnly() && db->Meta(L"format") == std::to_wstring(kFleetDbFormat), "open: a new database is created, writable, with its format record");
        CHECK(FtExists(path + L".lock"), "open: the writer holds sessions.db.lock");
        SessionInfo a = FtSession(L"aaaa-1", L"Alpha \x05D0 \U0001F600", 6);
        SessionInfo b = FtSession(L"bbbb-2", L"Beta", 0);
        SessionInfo c = FtSession(L"cccc-3", L"lone \xD83D", 2); // a title cut inside an emoji (lone high surrogate, at the END)
        c.queue[0].text = std::wstring{ L"nul:" } + L'\0' + L":after"; // an embedded NUL
        c.queue[1].text = L"x\xD800y"; // a lone surrogate inside a prompt text (followed by a character it must not swallow)
        SessionInfo big = FtSession(L"dddd-4", L"Big", 1);
        big.queue[0].text = std::wstring(1500 * 1024, L'\x05D0'); // ~3 MB of UTF-8
        SessionInfo dupIds = FtSession(L"eeee-5", L"Dup prompt ids", 3);
        dupIds.queue[1].id = dupIds.queue[0].id; // two prompts with one id
        dupIds.queue[2].id.clear(); // and one with none
        const auto r = db ? db->Write({ FtPut(a), FtPut(b), FtPut(c), FtPut(big), FtPut(dupIds) }, true) : FleetWriteResult{ false };
        CHECK(r.ok && r.rowsWritten == 5 + 2 * (6 + 0 + 2 + 1 + 3), "write: five new sessions = a row each + a state and a text row per prompt");
        std::wstring problem;
        const auto back = FtLoadFresh(path, &problem);
        CHECK(back.size() == 5 && problem.empty(), "read: a new connection reads all five back");
        const SessionInfo* ra = FtFind(back, L"aaaa-1");
        CHECK(ra && FtCanon(*ra) == FtCanon(a), "read: a session with its queue comes back equal (texts with newlines, quotes, Hebrew, an emoji)");
        CHECK(FtFind(back, L"bbbb-2") && FtCanon(*FtFind(back, L"bbbb-2")) == FtCanon(b), "read: a session with an empty queue comes back equal");
        const SessionInfo* rc = FtFind(back, L"cccc-3");
        CHECK(rc && rc->title == L"lone \xFFFD", "read: a lone surrogate at a title's end is stored as U+FFFD (as sessions.json stored it) - and the row still parses");
        CHECK(rc && rc->queue.size() == 2 && rc->queue[0].text == c.queue[0].text, "read: an embedded NUL in a prompt text survives");
        CHECK(rc && rc->queue.size() == 2 && rc->queue[1].text == L"x\xFFFDy", "read: a lone surrogate inside a text becomes U+FFFD and the next character is kept");
        CHECK(FtFind(back, L"dddd-4") && FtFind(back, L"dddd-4")->queue.size() == 1 && FtFind(back, L"dddd-4")->queue[0].text == big.queue[0].text,
              "read: a 1.5-million-character prompt comes back whole");
        const SessionInfo* re = FtFind(back, L"eeee-5");
        CHECK(re && FtCanon(*re) == FtCanon(dupIds), "read: duplicate and empty prompt ids keep every prompt, in order");
        CHECK(back.size() == 5 && back[0].id == L"aaaa-1" && back[4].id == L"eeee-5", "read: sessions come back in the order they were first written");

        // ---- the diff: exactly the changed rows ----
        CHECK(db && db->Write({ FtPut(a), FtPut(b), FtPut(c), FtPut(big), FtPut(dupIds) }, true).rowsWritten == 0, "diff: an unchanged fleet writes no row");
        a.queue[3].status = PromptStatus::Failed;
        auto d1 = db->Write({ FtPut(a) }, true);
        CHECK(d1.ok && d1.rowsWritten == 1 && d1.rowsDeleted == 0, "diff: one prompt's status = one state row");
        a.title = L"Alpha renamed";
        d1 = db->Write({ FtPut(a) }, true);
        CHECK(d1.ok && d1.rowsWritten == 1 && d1.rowsDeleted == 0, "diff: a title = the session's doc row only");
        a.queue.erase(a.queue.begin());
        d1 = db->Write({ FtPut(a) }, true);
        CHECK(d1.ok && d1.rowsWritten == 0 && d1.rowsDeleted == 2, "diff: the oldest prompt removed = its two rows deleted, NO other prompt rewritten");
        QueuedPrompt added;
        added.id = L"aaaa-1-new";
        added.text = L"appended";
        a.queue.push_back(added);
        d1 = db->Write({ FtPut(a) }, true);
        CHECK(d1.ok && d1.rowsWritten == 2 && d1.rowsDeleted == 0, "diff: an appended prompt = its state row + its text row");
        std::swap(a.queue[1], a.queue[2]); // a Move Up
        d1 = db->Write({ FtPut(a) }, true);
        CHECK(d1.ok && d1.rowsWritten == 1, "diff: a Move Up rewrites the one moved prompt's state row");
        a.queue[0].text += L" edited";
        d1 = db->Write({ FtPut(a) }, true);
        CHECK(d1.ok && d1.rowsWritten == 1, "diff: an edited text = its text row only");
        big.queue[0].status = PromptStatus::Sent;
        d1 = db->Write({ FtPut(big) }, true);
        CHECK(d1.ok && d1.rowsWritten == 1, "diff: a status change of a 3 MB prompt writes its small state row, never the text again");
        const auto afterDiff = FtLoadFresh(path);
        CHECK(FtFind(afterDiff, L"aaaa-1") && FtCanon(*FtFind(afterDiff, L"aaaa-1")) == FtCanon(a), "diff: after all of it a fresh read equals the record (order included)");
        // The history cap's steady state: a full queue that gains one prompt and loses its oldest at every send.
        SessionInfo capped = FtSession(L"ffff-6", L"Capped", 200);
        db->Write({ FtPut(capped) }, true);
        size_t worst = 0;
        for (int k = 0; k < 10; ++k)
        {
            capped.queue.erase(capped.queue.begin());
            QueuedPrompt p;
            p.id = L"ffff-6-n" + std::to_wstring(k);
            p.text = L"new " + std::to_wstring(k);
            capped.queue.push_back(p);
            const auto step = db->Write({ FtPut(capped) }, true);
            worst = (std::max)(worst, step.rowsWritten + step.rowsDeleted);
        }
        CHECK(worst == 4, "diff: a capped queue's send (one in, the oldest out) writes 2 rows and deletes 2 - not the 200-row queue");
        CHECK(FtFind(FtLoadFresh(path), L"ffff-6") && FtCanon(*FtFind(FtLoadFresh(path), L"ffff-6")) == FtCanon(capped), "diff: ...and the queue reads back exact");

        // ---- removal; a list naming a session twice writes its last record ----
        d1 = db->Write({ FtRemove(L"bbbb-2") }, true);
        CHECK(d1.ok && d1.rowsDeleted == 1 && !FtFind(FtLoadFresh(path), L"bbbb-2"), "remove: the session's rows are deleted");
        CHECK(db->Write({ FtRemove(L"never-stored") }, true).ok, "remove: an id never stored is a no-op");
        SessionInfo twiceA = FtSession(L"gggg-7", L"first", 1);
        SessionInfo twiceB = FtSession(L"gggg-7", L"second", 1);
        db->Write({ FtPut(twiceA), FtPut(twiceB) }, true);
        CHECK(FtFind(FtLoadFresh(path), L"gggg-7") && FtFind(FtLoadFresh(path), L"gggg-7")->title == L"second", "write: a session named twice in one list writes its LAST record");
        CHECK(db->Write({ FtPut(twiceA), FtRemove(L"gggg-7") }, true).ok && !FtFind(FtLoadFresh(path), L"gggg-7"), "write: ...a removal after a put removes");

        // ---- the writer lock and a reader beside the writer ----
        {
            std::wstring why;
            auto second = FleetDb::Open(path, FleetDbAccess::ReadWrite, false, why);
            CHECK(second && second->ReadOnly() && second->ReadOnlyReason().find(L"sessions.db.lock") != std::wstring::npos,
                  "lock: a second writer opens READ-ONLY (another process holds sessions.db.lock)");
            CHECK(second && second->Write({ FtPut(FtSession(L"hhhh-8", L"from the second", 0)) }, true).rowsWritten == 0 && !FtFind(FtLoadFresh(path), L"hhhh-8"),
                  "lock: ...and writes nothing");
        }
        a.title = L"seen by a reader";
        db->Write({ FtPut(a) }, false); // NORMAL: in the WAL, not checkpointed
        CHECK(FtExists(path + L"-wal") && db->WalBytes() > 0, "reader: (setup) the commit sits in the WAL");
        const auto viaCli = LoadFleetReadOnlyIn(sp.dir);
        CHECK(FtFind(viaCli, L"aaaa-1") && FtFind(viaCli, L"aaaa-1")->title == L"seen by a reader", "reader: another process's read (LoadFleetReadOnlyIn) sees the WAL's commit");
        CHECK(db->FlushWal(), "durability: the WAL's own fsync answers ok");
        CHECK(db->Checkpoint(true) && db->WalBytes() == 0, "checkpoint: TRUNCATE folds the WAL into the file and empties it");
        db.reset(); // the writer closes: the lock file goes
        CHECK(!FtExists(path + L".lock"), "lock: closing the writer removes sessions.db.lock");
        const auto afterClose = LoadFleetReadOnlyIn(sp.dir);
        CHECK(FtFind(afterClose, L"aaaa-1") && FtFind(afterClose, L"aaaa-1")->title == L"seen by a reader", "reader: the file alone holds everything after the writer closed");
        std::wstring check;
        auto again = FtOpen(path, FleetDbAccess::ReadWrite, false);
        CHECK(again && !again->ReadOnly() && again->Check(true, check), "reopen: the writer opens again, and the full integrity check answers ok");
    }

    // ---- the residue per row: a newer build's members are kept through this build's writes ----
    {
        FleetScratch sp(L"am-fleet-residue-");
        const std::wstring path = sp.dir + L"\\sessions.db";
        {
            auto db = FtOpen(path);
            json::Value s = ToJson(FtSession(L"res-1", L"Residue", 2));
            s.Set(L"futureMember", json::Value::MkStr(L"newer"));
            for (auto& [k, v] : s.members)
            {
                if (k == L"queue")
                {
                    v.arr[1].Set(L"futurePromptMember", json::Value::MkStr(L"newer prompt"));
                }
            }
            std::vector<std::wstring> imported;
            std::wstring problem;
            CHECK(db && db->ImportRaw({ &s }, {}, imported, problem) && imported.size() == 1, "residue: (setup) a record a newer build wrote");
        }
        auto db = FtOpen(path, FleetDbAccess::ReadWrite, false);
        std::vector<SessionInfo> fleet;
        std::wstring problem;
        CHECK(db && db->LoadAll(fleet, problem) && fleet.size() == 1, "residue: the load reads it");
        fleet[0].title = L"Renamed by this build";
        fleet[0].queue[1].status = PromptStatus::Failed;
        CHECK(db->Write({ FtPut(fleet[0]) }, true).ok, "residue: this build's change is written");
        std::vector<std::pair<std::wstring, json::Value>> raw;
        CHECK(db->ReadRaw(raw, problem) && raw.size() == 1, "residue: the rows read back raw");
        const json::Value* q = raw.empty() ? nullptr : raw[0].second.Find(L"queue");
        CHECK(!raw.empty() && raw[0].second.StrAt(L"futureMember") == L"newer" && raw[0].second.StrAt(L"title") == L"Renamed by this build",
              "residue: the newer build's session member is kept, and this build's change is saved");
        CHECK(q && q->arr.size() == 2 && q->arr[1].StrAt(L"futurePromptMember") == L"newer prompt", "residue: ...and its prompt member too (matched by the prompt's id)");
    }

    // ---- the format guard ----
    {
        FleetScratch sp(L"am-fleet-format-");
        const std::wstring path = sp.dir + L"\\sessions.db";
        {
            auto db = FtOpen(path);
            CHECK(db && db->Write({ FtPut(FtSession(L"fmt-1", L"Kept", 1)) }, true).ok, "format: (setup) a stored session");
        }
        CHECK(FtRawExec(path, "UPDATE meta SET value = '2' WHERE key = 'format'"), "format: (setup) a newer build's format number");
        auto db = FtOpen(path, FleetDbAccess::ReadWrite, false);
        CHECK(db && db->ReadOnly() && db->ReadOnlyReason().find(L"newer") != std::wstring::npos, "format: a newer format opens READ-ONLY");
        SessionInfo changed = FtSession(L"fmt-1", L"Changed", 1);
        CHECK(db && db->Write({ FtPut(changed), FtPut(FtSession(L"fmt-2", L"New", 0)) }, true).rowsWritten == 0, "format: ...writes nothing");
        const auto rows = FtRawRows(path, "SELECT id, doc FROM sessions");
        CHECK(rows.size() == 1 && rows[0].second.find("\"Kept\"") != std::string::npos, "format: ...and the newer build's data is exactly as it was");
    }

    // ---- damage: unreadable rows and orphan prompt rows are kept aside, never mixed into a new record ----
    {
        FleetScratch sp(L"am-fleet-damage-");
        const std::wstring path = sp.dir + L"\\sessions.db";
        {
            auto db = FtOpen(path);
            CHECK(db && db->Write({ FtPut(FtSession(L"ok-1", L"Fine", 1)) }, true).ok, "damage: (setup) a good session");
        }
        CHECK(FtRawExec(path, "INSERT INTO sessions(id, ord, has_queue, doc) VALUES('bad-1', 99, 0, '{broken')") &&
                  FtRawExec(path, "INSERT INTO prompt_state(session_id, prompt_key, ord, state) VALUES('orphan-1', 'stray', 1, '{\"id\":\"stray\",\"status\":\"pending\"}')") &&
                  FtRawExec(path, "INSERT INTO prompt_text(session_id, prompt_key, text) VALUES('orphan-1', 'stray', 'a stray prompt')"),
              "damage: (setup) a row that does not parse, and prompt rows with no session row");
        auto db = FtOpen(path, FleetDbAccess::ReadWrite, false);
        std::vector<SessionInfo> fleet;
        std::wstring problem;
        CHECK(db && db->LoadAll(fleet, problem) && fleet.size() == 1 && problem.find(L"1 stored session") != std::wstring::npos,
              "damage: the load skips the unreadable row, says so, and keeps the rest");
        CHECK(db && db->Write({ FtPut(FtSession(L"bad-1", L"New record, same id", 1)), FtPut(FtSession(L"orphan-1", L"New record over orphans", 1)) }, true).ok,
              "damage: new records with those ids are written");
        const auto sessions = FtRawRows(path, "SELECT id, doc FROM sessions ORDER BY ord");
        bool keptBroken = false;
        for (const auto& [id, doc] : sessions)
        {
            keptBroken = keptBroken || (id.rfind("bad-1\x1F" "kept-", 0) == 0 && doc == "{broken");
        }
        CHECK(keptBroken, "damage: the unreadable row is KEPT, byte for byte, under a key of its own");
        const auto again = FtLoadFresh(path);
        const SessionInfo* orphanNew = FtFind(again, L"orphan-1");
        CHECK(orphanNew && orphanNew->queue.size() == 1 && orphanNew->queue[0].id == L"orphan-1-p0",
              "damage: the orphan prompt rows never join the new record's queue (they were moved aside)");
        CHECK(FtRawRows(path, "SELECT session_id, prompt_key FROM prompt_state WHERE prompt_key = 'stray'").size() == 1 &&
                  FtRawRows(path, "SELECT session_id, prompt_key FROM prompt_state WHERE prompt_key = 'stray'")[0].first.rfind("orphan-1\x1F" "kept-", 0) == 0,
              "damage: ...and they are kept too");
        CHECK(FtFind(again, L"bad-1") && FtFind(again, L"bad-1")->title == L"New record, same id" && FtFind(again, L"ok-1"), "damage: every readable record reads back");
    }
}

// ===================================================================================================================
// TestFleetStoreStartup
// ===================================================================================================================

void TestFleetStoreStartup()
{
    std::wprintf(L"Fleet store startup (fresh, the migration, its verification, interrupted steps, older builds, unusable files):\n");

    // ---- a fresh profile ----
    {
        FleetScratch sp(L"am-fleet-fresh-");
        ResetFleetStoreForTests();
        const FleetLoad load = OpenFleetForStartupIn(sp.dir);
        CHECK(load.usable && load.sessions.empty() && load.fileStatus == StateFileStatus::Missing, "fresh: usable and empty (nothing to lose)");
        CHECK(CurrentFleetMode() == FleetMode::Sqlite && FtExists(sp.dir + L"\\sessions.db") && !FtExists(sp.dir + L"\\sessions.json"),
              "fresh: an empty sessions.db is the store; no sessions.json is written");
        CHECK(!FtExists(sp.dir + L"\\sessions.db.migrating"), "fresh: the store was built aside and renamed into place (no temp file left)");
    }

    // ---- the migration ----
    {
        FleetScratch sp(L"am-fleet-migrate-");
        const std::string source = FtMigrationSource(40, 5);
        FtWriteBytes(sp.dir + L"\\sessions.json", source);
        // What the JSON reader makes of the file (the fleet the store must equal), read from a copy so the read marks nothing
        // in the profile that is migrated.
        FleetScratch copy(L"am-fleet-migrate-src-");
        FtWriteBytes(copy.dir + L"\\sessions.json", source);
        const auto fromJson = FtSessionsWithIds(ReadSessionsJsonForStartupIn(copy.dir).sessions);
        ResetStateFileGuardForTests();
        ResetFleetStoreForTests();
        const FleetLoad load = OpenFleetForStartupIn(sp.dir);
        CHECK(load.usable && CurrentFleetMode() == FleetMode::Sqlite, "migrate: the start is usable, in the store");
        CHECK(FtSameFleet(FtSessionsWithIds(load.sessions), fromJson) && fromJson.size() == 41, "migrate: the loaded fleet equals what sessions.json held (every session, in order)");
        CHECK(!FtExists(sp.dir + L"\\sessions.json") && FtExists(sp.dir + L"\\sessions.db") && !FtExists(sp.dir + L"\\sessions.db.migrating"),
              "migrate: sessions.json is gone, sessions.db is the store, no temp file is left");
        CHECK(!FtExists(sp.dir + L"\\shadow\\sessions.json"), "migrate: sessions.json's shadow is gone with it");
        const auto backups = FtMigrationBackups(sp.dir);
        CHECK(backups.size() == 1 && FtReadBytes(backups[0] + L"\\sessions.json") == source, "migrate: ONE verified backup holds sessions.json byte for byte");
        FleetDb* db = FleetDbForTests();
        const std::optional<json::Value> record = db ? json::Parse(db->Meta(L"migration")) : std::optional<json::Value>{};
        const std::string sourceHex = Sha256Hex(source);
        const std::wstring sourceSha{ sourceHex.begin(), sourceHex.end() };
        // 43 elements = 40 sessions + the non-object one + the repeated id + the empty id; prompts = 40×5 + the repeat's 5.
        CHECK(record && record->StrAt(L"sha256") == sourceSha && record->NumAt(L"sessions") == 43 && record->NumAt(L"prompts") == 40 * 5 + 5,
              "migrate: meta 'migration' records the source's SHA-256 and its counts (every element, the odd ones included)");
        std::vector<std::pair<std::wstring, json::Value>> raw;
        std::wstring problem;
        CHECK(db && db->ReadRaw(raw, problem) && raw.size() == 43, "migrate: every element of the source is in the store (the non-object one and the repeated id too)");
        bool kept = false;
        for (const auto& [key, obj] : raw)
        {
            kept = kept || obj.StrAt(L"futureMember") == L"kept by the store";
        }
        CHECK(kept, "migrate: a member this build does not know moved over unchanged");
        // The 12-snapshot pruner never removes the migration backup.
        for (int i = 0; i < 14; ++i)
        {
            const std::wstring snap = sp.dir + L"\\backups\\20990101-0000" + (i < 10 ? L"0" : L"") + std::to_wstring(i) + L" fake";
            std::error_code ec;
            fs::create_directories(fs::path{ snap }, ec);
            FtWriteBytes(snap + L"\\README.txt", FtUtf8(std::wstring{ StateBackups::kSnapshotMarker }) + " - fake\r\n");
        }
        StateBackups::PruneSnapshots(sp.dir, StateBackups::kKeepSnapshots);
        CHECK(FtMigrationBackups(sp.dir).size() == 1, "migrate: the snapshot pruner never removes the migration backup");
        // The other processes' read.
        CHECK(FtSameFleet(FtSessionsWithIds(LoadFleetReadOnlyIn(sp.dir)), fromJson), "migrate: the CLI's read (LoadFleetReadOnlyIn) gives the same fleet");
        // A second start: no migration, the same fleet.
        ResetFleetStoreForTests();
        const FleetLoad second = OpenFleetForStartupIn(sp.dir);
        CHECK(second.usable && FtSameFleet(FtSessionsWithIds(second.sessions), fromJson) && FtMigrationBackups(sp.dir).size() == 1,
              "migrate: the next start opens the store - same fleet, no second migration");

        // ---- the interrupted last step: sessions.json still there with the bytes migrated ----
        ResetFleetStoreForTests();
        FtWriteBytes(sp.dir + L"\\sessions.json", source);
        const FleetLoad finish = OpenFleetForStartupIn(sp.dir);
        CHECK(finish.usable && !FtExists(sp.dir + L"\\sessions.json") && FtSameFleet(FtSessionsWithIds(finish.sessions), fromJson),
              "interrupted: sessions.json with the migrated bytes is removed at the next start (step 7 finished), nothing imported twice");

        // ---- a sessions.json written after the move (an older build, or a run that could not use the store) ----
        ResetFleetStoreForTests();
        SessionInfo known = fromJson[1];
        known.title = L"an older build's rename";
        SessionInfo fresh = FtSession(L"after-move-1", L"Made by an older build", 2);
        FtWriteBytes(sp.dir + L"\\sessions.json", FtUtf8(SerializeSessions({ known, fresh })));
        const FleetLoad after = OpenFleetForStartupIn(sp.dir);
        const SessionInfo* gotFresh = FtFind(after.sessions, L"after-move-1");
        const SessionInfo* gotKnown = FtFind(after.sessions, known.id);
        CHECK(after.usable && gotFresh && FtCanon(*gotFresh) == FtCanon(fresh), "after the move: a session the store did not know is imported");
        CHECK(gotKnown && gotKnown->title == fromJson[1].title, "after the move: a session the store knows keeps the store's record (never overwritten by an import)");
        bool aside = false;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator{ fs::path{ sp.dir }, ec })
        {
            aside = aside || e.path().filename().wstring().rfind(L"sessions.json.after-sqlite-", 0) == 0;
        }
        CHECK(aside && !FtExists(sp.dir + L"\\sessions.json"), "after the move: the file is kept aside as sessions.json.after-sqlite-<stamp>");
    }

    // ---- a planted mismatch must stop the migration (the verification's negative control) ----
    for (int variant = 0; variant < 2; ++variant)
    {
        FleetScratch sp(L"am-fleet-verify-");
        const std::string source = FtMigrationSource(10, 3);
        FtWriteBytes(sp.dir + L"\\sessions.json", source);
        const std::wstring temp = sp.dir + L"\\sessions.db.migrating";
        SetFleetMigrationStepHookForTests([&](int step) {
            if (step == 4) // the temp database is built and closed, not verified yet
            {
                FtRawExec(temp, variant == 0 ? "UPDATE sessions SET doc = replace(doc, 'Session 3', 'Session X') WHERE id = 'mig-3'" :
                                               "UPDATE prompt_text SET text = text || '!' WHERE session_id = 'mig-5' AND prompt_key = 'mig-5-p1'");
            }
        });
        ResetFleetStoreForTests();
        ResetStateFileGuardForTests();
        const FleetLoad load = OpenFleetForStartupIn(sp.dir);
        SetFleetMigrationStepHookForTests(nullptr);
        CHECK(CurrentFleetMode() == FleetMode::Json && load.usable && FtReadBytes(sp.dir + L"\\sessions.json") == source,
              variant == 0 ? "verify: a changed session doc stops the migration - sessions.json untouched, the run on sessions.json" :
                             "verify: a changed prompt text stops the migration - sessions.json untouched, the run on sessions.json");
        CHECK(!FtExists(sp.dir + L"\\sessions.db") && !FtExists(temp) && !FtExists(temp + L"-wal"), "verify: ...no sessions.db, no temp file left");
        CHECK(FtSessionsWithIds(load.sessions).size() == 11, "verify: ...and the fleet still loads from sessions.json");
    }

    // ---- an unusable sessions.json: no migration, nothing created ----
    {
        FleetScratch sp(L"am-fleet-badjson-");
        const std::string damaged(2048, '\0');
        FtWriteBytes(sp.dir + L"\\sessions.json", damaged);
        ResetFleetStoreForTests();
        ResetStateFileGuardForTests();
        const FleetLoad load = OpenFleetForStartupIn(sp.dir);
        CHECK(!load.usable && CurrentFleetMode() == FleetMode::Json && !FtExists(sp.dir + L"\\sessions.db") && FtReadBytes(sp.dir + L"\\sessions.json") == damaged,
              "unusable json: not migrated, no store created, the file untouched (the next start tries again)");
        CHECK(FtMigrationBackups(sp.dir).empty(), "unusable json: no backup of a file that was not migrated");
        ResetStateFileGuardForTests();
    }

    // ---- an unusable store: never written over; the run saves sessions.json beside it; the next good start imports it ----
    {
        FleetScratch sp(L"am-fleet-baddb-");
        std::string garbage(64 * 1024, 'G');
        FtWriteBytes(sp.dir + L"\\sessions.db", garbage);
        ResetFleetStoreForTests();
        ResetStateFileGuardForTests();
        const FleetLoad load = OpenFleetForStartupIn(sp.dir);
        CHECK(!load.usable && load.problem.find(L"sessions.db") != std::wstring::npos && CurrentFleetMode() == FleetMode::Json,
              "unusable store: the start says the store cannot be used (the caller keeps a recovery copy and holds tab refs)");
        const std::wstring kept = PreserveWorkspaceForRecoveryIn(sp.dir, load.problem);
        CHECK(!kept.empty() && FtReadBytes(kept + L"\\sessions.db") == garbage, "unusable store: the recovery copy holds sessions.db byte for byte");
        auto reg = std::make_shared<SessionRegistry>();
        BindFleetWriterRegistryForTests(reg);
        const SessionInfo made = FtSession(L"made-while-unusable", L"Made while the store was unusable", 1);
        reg->Upsert(made);
        CHECK(FlushFleetDirtyForTests(), "unusable store: the run's change is saved");
        CHECK(FtReadBytes(sp.dir + L"\\sessions.db") == garbage, "unusable store: ...never into the unusable sessions.db (byte for byte as it was)");
        const auto savedJson = DeserializeSessions(ReadStateFileUtf8(sp.dir + L"\\sessions.json").text);
        CHECK(FtFind(savedJson, L"made-while-unusable"), "unusable store: ...but into sessions.json beside it");
        // The next start whose store can be used (here: the damage repaired by hand) imports that file.
        ResetFleetStoreForTests();
        std::error_code ec;
        fs::remove(fs::path{ sp.dir + L"\\sessions.db" }, ec);
        {
            FleetScratch good(L"am-fleet-gooddb-");
            ResetFleetStoreForTests();
            OpenFleetForStartupIn(good.dir); // a fresh, empty store
            ResetFleetStoreForTests();
            fs::copy_file(fs::path{ good.dir + L"\\sessions.db" }, fs::path{ sp.dir + L"\\sessions.db" }, ec);
        }
        ResetFleetStoreForTests();
        const FleetLoad repaired = OpenFleetForStartupIn(sp.dir);
        CHECK(repaired.usable && FtFind(repaired.sessions, L"made-while-unusable") && !FtExists(sp.dir + L"\\sessions.json"),
              "unusable store: the next start that can use the store imports what the run saved beside it");
        BindFleetWriterRegistryForTests(nullptr);
        ResetStateFileGuardForTests();
    }

    // ---- the format guard at the start: a newer build's store is loaded, never written ----
    {
        FleetScratch sp(L"am-fleet-newer-");
        {
            auto db = FtOpen(sp.dir + L"\\sessions.db");
            db->Write({ FtPut(FtSession(L"newer-1", L"From a newer build", 1)) }, true);
            db->Checkpoint(true);
        }
        FtRawExec(sp.dir + L"\\sessions.db", "UPDATE meta SET value = '9' WHERE key = 'format'");
        const std::string before = FtReadBytes(sp.dir + L"\\sessions.db");
        ResetFleetStoreForTests();
        const FleetLoad load = OpenFleetForStartupIn(sp.dir);
        CHECK(load.usable && FtFind(load.sessions, L"newer-1") && CurrentFleetMode() == FleetMode::SqliteReadOnly, "newer format: loaded, read-only");
        auto reg = std::make_shared<SessionRegistry>();
        BindFleetWriterRegistryForTests(reg);
        reg->Upsert(FtSession(L"newer-2", L"Made by this build", 0));
        CHECK(FlushFleetDirtyForTests() && PersistFleetNowDurable(), "newer format: a commit answers (nothing to write in a read-only run)");
        ResetFleetStoreForTests();
        CHECK(FtReadBytes(sp.dir + L"\\sessions.db") == before, "newer format: the store is byte for byte as the newer build left it");
        BindFleetWriterRegistryForTests(nullptr);
    }
}

// ===================================================================================================================
// TestFleetWriter
// ===================================================================================================================

void TestFleetWriter()
{
    std::wprintf(L"Fleet store writer (the registry's marks, the commits, the bulk scope, the critical commits):\n");
    FleetScratch sp(L"am-fleet-writer-");
    auto reg = std::make_shared<SessionRegistry>();
    BindFleetWriterRegistryForTests(reg);
    ResetFleetStoreForTests();
    const std::wstring id = L"writer-1";
    reg->Upsert(FtSession(id, L"Writer", 2));
    CHECK(PersistFleetNowDurable() && FleetDirtyCountForTests() == 1,
          "before the open: a critical commit answers true (nothing is live yet; refusing would stall a run whose open failed) and the mark WAITS");
    CHECK(OpenFleetForStartupIn(sp.dir).usable, "writer: (setup) a fresh store");
    CHECK(FleetDirtyCountForTests() == 1, "mark: Upsert marked its record, and the mark waited for the store");
    CHECK(FlushFleetDirtyForTests() && FleetDirtyCountForTests() == 0 && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"Writer",
          "commit: the waiting mark is committed once the store is open, and a reader sees the record");

    // Every mutator marks its record (the plan's dirty-mark audit): a change that marked nothing would wait for the sweep.
    struct Mutation
    {
        const char* what;
        std::function<void()> run;
    };
    const std::vector<Mutation> mutations = {
        { "mark: Update", [&] { reg->Update(id, [](SessionInfo& s) { s.title = L"Updated"; }); } },
        { "mark: SetPendingInput (a draft appears)", [&] { reg->SetPendingInput(id, L"a draft"); } },
        { "mark: SetPendingInput (a text-only edit, the quiet path)", [&] { reg->SetPendingInput(id, L"a longer draft"); } },
        { "mark: SetPendingPasteRefs (the quiet setter)", [&] { reg->SetPendingPasteRefs(id, L"[Pasted text #1]\x1Fhello"); } },
        { "mark: OnHookEvent (a prompt)", [&] { reg->OnHookEvent(UPS(id, L"hello from the hook")); } },
        { "mark: RollbackPromptToPending", [&] { reg->RollbackPromptToPending(id, id + L"-p0", false); } },
    };
    for (const auto& m : mutations)
    {
        FlushFleetDirtyForTests();
        m.run();
        CHECK(FleetDirtyCountForTests() >= 1, m.what);
    }
    CHECK(FlushFleetDirtyForTests(), "commit: the mutations are committed");
    CHECK(FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"Updated", "commit: a reader sees the mutations");

    // The UI thread's critical commit: NORMAL now, the writer's fsync later (no thread here: the fsync request just waits).
    reg->Update(id, [](SessionInfo& s) { s.title = L"Critical"; });
    PersistFleetNow();
    CHECK(FleetDirtyCountForTests() == 0 && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"Critical", "critical (UI): PersistFleetNow commits at once - a reader sees it on return");
    CHECK(PersistFleetNowDurable(), "critical (worker): with nothing dirty it fsyncs the WAL the UI commit left in the OS cache, and answers true");
    reg->Update(id, [](SessionInfo& s) { s.title = L"Durable"; });
    CHECK(PersistFleetNowDurable() && FleetDirtyCountForTests() == 0 && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"Durable",
          "critical (worker): PersistFleetNowDurable commits the dirty record and answers true");

    // The restore's bulk scope: no commit until it ends.
    {
        FleetBulkScope bulk;
        reg->Update(id, [](SessionInfo& s) { s.title = L"In the bulk scope"; });
        PersistFleetNow();
        CHECK(FleetDirtyCountForTests() == 1 && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"Durable",
              "bulk: PersistFleetNow writes nothing while a FleetBulkScope is open (the restore commits once, at its end)");
    }
    CHECK(FlushFleetDirtyForTests() && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"In the bulk scope", "bulk: ...and the batch is committed after it");

    // A batch keeps the registry's insertion order for the sessions new in it (the next start restores in that order).
    for (int k = 0; k < 6; ++k)
    {
        reg->Upsert(FtSession(L"order-" + std::to_wstring(k), L"Ordered", 0));
    }
    CHECK(FlushFleetDirtyForTests(), "order: (setup) six new sessions in one batch");
    {
        const auto fleet = LoadFleetReadOnlyIn(sp.dir);
        std::vector<std::wstring> ordered;
        for (const auto& s : fleet)
        {
            if (s.id.rfind(L"order-", 0) == 0)
            {
                ordered.push_back(s.id);
            }
        }
        CHECK((ordered == std::vector<std::wstring>{ L"order-0", L"order-1", L"order-2", L"order-3", L"order-4", L"order-5" }),
              "order: one batch's new sessions are stored in the registry's insertion order (the dirty set itself has none)");
    }

    // A removal reaches the store.
    reg->Remove(id);
    CHECK(FleetDirtyCountForTests() == 1 && FlushFleetDirtyForTests() && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), id) == L"<absent>", "mark: Remove marks, and the commit deletes the rows");

    // The routine commit lands in the WAL (checkpoints are the writer thread's); the app's close folds it into the file.
    reg->Upsert(FtSession(L"writer-2", L"Second", 1));
    CHECK(FlushFleetDirtyForTests() && FleetDbForTests() && FleetDbForTests()->WalBytes() > 0, "commit: the routine commit lands in the WAL (checkpoints are the writer thread's)");
    FlushFleetAtAppClose();
    CHECK(FleetDbForTests() && FleetDbForTests()->WalBytes() == 0 && FtTitleIn(LoadFleetReadOnlyIn(sp.dir), L"writer-2") == L"Second",
          "close: the app's close commits and checkpoints - the file alone holds the fleet");
    BindFleetWriterRegistryForTests(nullptr);
}

// ===================================================================================================================
// TestFleetKills
// ===================================================================================================================

void TestFleetKills()
{
    std::wprintf(L"Fleet store kills (child processes killed mid-commit and mid-migration):\n");
    std::mt19937 rng{ static_cast<unsigned>(::GetTickCount64()) };

    // ---- mid-commit: every acknowledged commit survives, and every commit is whole ----
    {
        FleetScratch sp(L"am-fleet-kill-commit-");
        const std::wstring path = sp.dir + L"\\sessions.db";
        int rounds = 0;
        int survived = 0;
        int whole = 0;
        int64_t totalAcks = 0;
        for (int round = 0; round < 30; ++round)
        {
            std::error_code ec;
            for (const wchar_t* suffix : { L"", L"-wal", L"-shm", L".lock" })
            {
                fs::remove(fs::path{ path + suffix }, ec);
            }
            FtChild child;
            if (!FtStartChild(L"commit \"" + path + L"\"", child))
            {
                break;
            }
            const int64_t want = 1 + static_cast<int64_t>(rng() % 60);
            const int64_t readAck = FtReadAcks(child, want);
            FtKill(child, 99); // right after an acknowledgement: usually in the middle of the next commit
            // The child may have run ahead of what was read: drain the pipe (it ends with the child) for its LAST ack. Every
            // commit up to it returned; at most the one after it committed without its ack being printed.
            const int64_t drained = FtReadAcks(child, INT64_MAX);
            const int64_t lastAck = (std::max)(readAck, drained);
            ++rounds;
            totalAcks += lastAck;
            const auto fleet = FtLoadFresh(path);
            const SessionInfo* rec = FtFind(fleet, L"kill-0000");
            const int64_t got = rec && rec->title.rfind(L"commit ", 0) == 0 ? std::wcstoll(rec->title.c_str() + 7, nullptr, 10) : 0;
            survived += (lastAck > 0 && got >= lastAck && got <= lastAck + 1) ? 1 : 0;
            whole += (rec && FtCanon(*rec) == FtCanon(FtKillRecord(got))) ? 1 : 0;
        }
        CHECK(rounds == 30 && totalAcks > 0, "kill: (setup) 30 children committed and were killed");
        CHECK(survived == rounds, "kill: every acknowledged commit is in the store after the kill (and at most the one in flight beyond it)");
        CHECK(whole == rounds, "kill: every commit is WHOLE - the title, the new prompt and the removed one together, never part of it");
    }

    // ---- mid-migration: killed at each step boundary ----
    const std::string source = FtMigrationSource(150, 4);
    FleetScratch ref(L"am-fleet-kill-ref-");
    FtWriteBytes(ref.dir + L"\\sessions.json", source);
    const auto expected = FtSessionsWithIds(ReadSessionsJsonForStartupIn(ref.dir).sessions);
    const auto checkProfile = [&](const std::wstring& dir, const char* invariantMsg, const char* recoveredMsg) {
        // Right after the kill, before anything repairs it: sessions.json may be gone ONLY when sessions.db holds the fleet.
        const bool jsonThere = FtExists(dir + L"\\sessions.json");
        bool invariant = jsonThere && FtReadBytes(dir + L"\\sessions.json") == source;
        if (!jsonThere)
        {
            invariant = FtSameFleet(FtSessionsWithIds(FtLoadFresh(dir + L"\\sessions.db")), expected);
        }
        CHECK(invariant, invariantMsg);
        // The next start recovers the whole fleet, as the store.
        ResetFleetStoreForTests();
        ResetStateFileGuardForTests();
        const FleetLoad load = OpenFleetForStartupIn(dir);
        const bool ok = load.usable && FtSameFleet(FtSessionsWithIds(load.sessions), expected) && !FtExists(dir + L"\\sessions.json") &&
                        FtExists(dir + L"\\sessions.db") && !FtExists(dir + L"\\sessions.db.migrating") && !FtMigrationBackups(dir).empty();
        CHECK(ok, recoveredMsg);
        ResetFleetStoreForTests();
    };
    for (int step = 1; step <= 7; ++step)
    {
        FleetScratch sp(L"am-fleet-kill-step-");
        FtWriteBytes(sp.dir + L"\\sessions.json", source);
        FtChild child;
        const bool started = FtStartChild(L"migrate \"" + sp.dir + L"\" " + std::to_wstring(step), child);
        const DWORD code = started ? FtWait(child, 60000) : 0;
        const std::string tag = "kill at step " + std::to_string(step);
        CHECK(started && code == 77, (tag + ": (setup) the migrating child was killed at that step").c_str());
        checkProfile(sp.dir, (tag + ": sessions.json is gone only once sessions.db holds the whole fleet").c_str(),
                     (tag + ": the next start ends with the whole fleet in the store, sessions.json gone, a backup kept").c_str());
    }

    // ---- mid-migration: killed at random moments (inside the import, the verification, the rename...) ----
    {
        // How long one migration of the source takes in a child (no kill), to aim the random kills inside it.
        FleetScratch timing(L"am-fleet-kill-timing-");
        FtWriteBytes(timing.dir + L"\\sessions.json", source);
        FtChild t;
        const ULONGLONG t0 = ::GetTickCount64();
        const bool started = FtStartChild(L"migrate \"" + timing.dir + L"\" 0", t);
        const DWORD code = started ? FtWait(t, 60000) : 1;
        const ULONGLONG span = (std::max)(ULONGLONG{ 50 }, ::GetTickCount64() - t0);
        CHECK(started && code == 0, "random kills: (setup) an unkilled child migrates the source");
        int checked = 0;
        for (int round = 0; round < 10; ++round)
        {
            FleetScratch sp(L"am-fleet-kill-rand-");
            FtWriteBytes(sp.dir + L"\\sessions.json", source);
            FtChild child;
            if (!FtStartChild(L"migrate \"" + sp.dir + L"\" 0", child))
            {
                continue;
            }
            ::Sleep(static_cast<DWORD>(rng() % span));
            FtKill(child, 99);
            ++checked;
            checkProfile(sp.dir, "random kill: sessions.json is gone only once sessions.db holds the whole fleet",
                         "random kill: the next start ends with the whole fleet in the store");
        }
        CHECK(checked == 10, "random kills: (setup) ten children killed at random moments of their migration");
    }
}
