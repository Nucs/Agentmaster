// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database store (see SessionDbStore.h): open, maps, discovery, the
// ingest of the refresh tick and the sweep, the derived rows, Agentmaster's facts, history.

#include "SessionDbStore.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <tuple>
#include <variant>

#include "SessionDbExtract.h"
#include "SessionDbSummary.h"
#include "SessionDbWalk.h"

#include "../ClaudeSpawn.h" // ResolveClaudeTranscriptPath, LogSwallowedException
#include "../Json.h"
#include "../Persistence.h" // NormDirKey, LoadSessions
#include "../ProcessInspect.h" // ResolveWorktreeMainRoot
#include "../SessionDbProtocol.h"
#include "../SessionStore.h" // LoadAllStoredSessionTitles / Favorites / Tags
#include "../TranscriptStore.h" // IsSessionIdStem, InferWorkingDirectory, FindGitRootForDir, ...

namespace Agentmaster::SessionDb
{
    namespace
    {
        // A request reads at most this many bytes of one file before it commits and moves on; the next
        // tick (or the sweep) continues from the committed cursor. Bounds a first ingest of a huge
        // transcript to a few hundred MB of transient memory.
        constexpr int64_t kMaxBytesPerFilePerBatch = 96ll * 1024 * 1024;
        // A batch (one transaction) closes after this many bytes across its sessions.
        constexpr int64_t kBatchBytes = 64ll * 1024 * 1024;
        // How many top-level files keep their working set (fold + uuids) in memory between ticks.
        constexpr size_t kMaxWorkingFiles = 96;
        // A merge is due past this many hot blocks or bytes, or this long after the last merge.
        constexpr int64_t kMergeDocs = 20000;
        constexpr int64_t kMergeBytes = 64ll * 1024 * 1024;
        constexpr int64_t kMergeAgeMs = 30ll * 60 * 1000;
        // The line store is re-sorted (TextIndex::ReclusterLines) once it holds this many sorted runs: a merge
        // that interleaves files adds one, so at the 30-minute merge cadence about a day and a half of steady
        // activity; a cold build that appended its batches out of file order passes it at once (wanted: the
        // sorted store is what lets the upserts' per-file reads prune). A rewrite costs minutes on a large
        // store, so the threshold keeps it rare.
        constexpr int64_t kLineRunsForResort = 64;
        // The memory budget of one sorting statement over the line store, as a share of the database's memory
        // limit (TextIndex::sortChunkBytes): a quarter, measured to pass where half the 1.6 GB store already did.
        constexpr int64_t kSortChunkShareOfMemory = 4;
        // The block text one merge transaction handles, as a share of the memory limit (TextIndex::mergeSliceBytes):
        // the slice is read, sorted and moved twice (doc / doc_tool) inside one transaction.
        constexpr int64_t kMergeSliceShareOfMemory = 16;

        // DuckDB's memory_limit text ("1024MB", "1GB", "512MiB") in bytes, the way DuckDB reads it (KB / MB / GB
        // decimal, KiB / MiB / GiB binary, a bare number = bytes). 0 when it cannot be read. Pure.
        int64_t MemoryLimitBytes(const std::string& text) noexcept
        {
            size_t i = 0;
            while (i < text.size() && text[i] == ' ')
            {
                ++i;
            }
            int64_t n = 0;
            bool digits = false;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9' && n < (1ll << 40))
            {
                n = n * 10 + (text[i] - '0');
                digits = true;
                ++i;
            }
            if (!digits)
            {
                return 0;
            }
            std::string unit;
            for (; i < text.size(); ++i)
            {
                if (text[i] != ' ')
                {
                    unit.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(text[i]))));
                }
            }
            if (unit.empty() || unit == "b" || unit == "bytes")
            {
                return n;
            }
            if (unit == "kb")
            {
                return n * 1000;
            }
            if (unit == "mb")
            {
                return n * 1000 * 1000;
            }
            if (unit == "gb")
            {
                return n * 1000 * 1000 * 1000;
            }
            if (unit == "kib")
            {
                return n << 10;
            }
            if (unit == "mib")
            {
                return n << 20;
            }
            if (unit == "gib")
            {
                return n << 30;
            }
            return 0;
        }
        // The inferred-folder vote reads at most this many distinct paths (the sidecar's kMaxPathsAccessed).
        constexpr int kMaxInferPaths = 512;

        int64_t FileTimeToMs(const FILETIME& ft)
        {
            const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            constexpr uint64_t kEpochDiff = 116444736000000000ull; // 1601 -> 1970 in 100 ns
            return t < kEpochDiff ? 0 : static_cast<int64_t>((t - kEpochDiff) / 10000);
        }

        // A monotonic millisecond clock for the step timings.
        int64_t TickMs()
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        int64_t NowMs()
        {
            FILETIME ft{};
            ::GetSystemTimeAsFileTime(&ft);
            return FileTimeToMs(ft);
        }

        // A tool-touched path as stored: every run of separators collapsed into one, except a leading `\\`
        // (a UNC share). A path mined from a shell command keeps the escaping it was typed with
        // ("K:\\source\\x.cpp" inside a quoted command), which would otherwise split one file into two keys.
        std::wstring CollapseSeparators(const std::wstring& p)
        {
            std::wstring out;
            out.reserve(p.size());
            size_t i = 0;
            if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/'))
            {
                out.append(p, 0, 2); // keep the UNC prefix as written
                i = 2;
                while (i < p.size() && (p[i] == L'\\' || p[i] == L'/'))
                {
                    ++i;
                }
            }
            for (; i < p.size(); ++i)
            {
                const bool sep = p[i] == L'\\' || p[i] == L'/';
                if (sep && !out.empty() && (out.back() == L'\\' || out.back() == L'/'))
                {
                    continue;
                }
                out += p[i];
            }
            return out;
        }

        // A path's lookup key: '/' -> '\', lower-cased (Windows paths compare case-insensitively).
        std::wstring PathKey(const std::wstring& p)
        {
            std::wstring k = p;
            for (auto& c : k)
            {
                if (c == L'/')
                {
                    c = L'\\';
                }
            }
            if (!k.empty())
            {
                ::CharLowerBuffW(k.data(), static_cast<DWORD>(k.size()));
            }
            return k;
        }

        std::wstring LowerAscii(std::wstring s)
        {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        }

        // The folder search's FOLD: lower-case with '/' separators (SESSIONS_DB.md §5.6 "two forms").
        std::wstring DirFold(std::wstring s)
        {
            for (auto& c : s)
            {
                if (c == L'\\')
                {
                    c = L'/';
                }
            }
            if (!s.empty())
            {
                ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
            }
            return s;
        }

        // FNV-1a 64 over bytes (the head hash that detects a replaced file).
        int64_t Fnv1a(std::string_view bytes)
        {
            uint64_t h = 1469598103934665603ull;
            for (const char c : bytes)
            {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            return static_cast<int64_t>(h);
        }

        // RAII file handle that shares read / write / delete (a live claude is never blocked).
        struct File
        {
            HANDLE h{ INVALID_HANDLE_VALUE };
            explicit File(const std::wstring& path)
            {
                h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            }
            ~File()
            {
                if (h != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(h);
                }
            }
            bool Ok() const
            {
                return h != INVALID_HANDLE_VALUE;
            }
            // Read up to `want` bytes at `offset`. Returns the bytes read (short at EOF).
            std::string ReadAt(int64_t offset, size_t want) const
            {
                std::string out;
                LARGE_INTEGER li{};
                li.QuadPart = offset;
                if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
                {
                    return out;
                }
                out.resize(want);
                size_t got = 0;
                while (got < want)
                {
                    DWORD n = 0;
                    const DWORD ask = static_cast<DWORD>(std::min<size_t>(want - got, 16u * 1024u * 1024u));
                    if (!::ReadFile(h, out.data() + got, ask, &n, nullptr) || n == 0)
                    {
                        break;
                    }
                    got += n;
                }
                out.resize(got);
                return out;
            }
        };

        // The session-row writer's value: SQL NULL, a number, a bool or UTF-8 text.
        using Val = std::variant<std::monostate, int64_t, double, bool, std::string>;

        // Bind a value list to a prepared statement, 1-based.
        void BindAll(Prepared& p, const std::vector<Val>& vals)
        {
            for (size_t i = 0; i < vals.size(); ++i)
            {
                const size_t idx = i + 1;
                const Val& v = vals[i];
                if (std::holds_alternative<std::monostate>(v))
                {
                    p.BindNull(idx);
                }
                else if (const auto* n = std::get_if<int64_t>(&v))
                {
                    p.Bind(idx, *n);
                }
                else if (const auto* d = std::get_if<double>(&v))
                {
                    p.Bind(idx, *d);
                }
                else if (const auto* b = std::get_if<bool>(&v))
                {
                    p.BindBool(idx, *b);
                }
                else
                {
                    p.Bind(idx, std::string_view{ std::get<std::string>(v) });
                }
            }
        }

        // UTF-8 of a wide string, or NULL when empty (the "a field only when it says something" rule).
        Val TextOrNull(const std::wstring& s)
        {
            if (s.empty())
            {
                return std::monostate{};
            }
            return ToUtf8(s);
        }

        Val NumOrNull(int64_t n)
        {
            if (n == 0)
            {
                return std::monostate{};
            }
            return n;
        }

        // A JSON array of strings, as text.
        std::string JsonArray(const std::vector<std::wstring>& v)
        {
            std::string out = "[";
            for (size_t i = 0; i < v.size(); ++i)
            {
                if (i)
                {
                    out += ',';
                }
                out += '"';
                for (const char c : ToUtf8(v[i]))
                {
                    switch (c)
                    {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20)
                        {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                            out += buf;
                        }
                        else
                        {
                            out += c;
                        }
                    }
                }
                out += '"';
            }
            out += ']';
            return out;
        }

        // The column order of the session row (kept in ONE place: the INSERT is generated from it).
        constexpr const char* kSessionColumns[] = {
            "sid_key", "session_id", "short_id", "file_id", "project_dir", "kind", "entrypoint",
            "size_bytes", "birth_ms", "lines", "transcript",
            "cwd", "cwd_key", "cwd_fold", "cwd_last", "cwd_last_key", "cwd_count",
            "relocated_cwd", "relocated_cwd_key",
            "worktree_path", "worktree_key", "worktree_name", "worktree_branch", "worktree_original_cwd",
            "env_is_git_repo", "env_is_worktree", "env_shell", "env_extra_dirs",
            "git_root", "git_root_key", "color_key",
            "inferred_dir", "inferred_dir_key", "inferred_git",
            "home_launch", "fs_checked_ms",
            "branch_first", "branch_last", "branch_count",
            "version_first", "version_last",
            "models", "model_last", "effort_last", "permission_mode_last", "plan_mode_marks", "agent_name",
            "created_ms", "first_ts_ms", "last_activity_ms", "last_typed_ms", "last_turn_end_ms",
            "turns_timed", "turn_ms_total", "turn_ms_max",
            "cost_usd", "api_ms", "api_ms_no_retries", "tool_ms", "wall_ms",
            "lines_added", "lines_removed", "cost_unknown_model", "has_cost_state",
            "api_messages", "in_tokens", "out_tokens", "cache_read_tokens", "cache_write_tokens",
            "web_searches", "web_fetches", "context_max", "context_tokens",
            "api_errors", "api_error_status_last", "refusals", "max_token_stops", "aborted_mid_stream",
            "interrupts", "end_state", "stop_hook_ms_total", "stop_hook_ms_max",
            "prompts_typed", "prompts_queued", "assistant_lines", "tool_calls",
            "tool_calls_by_tool", "skills_used", "mcp_servers_used",
            "subagents_spawned", "subagent_files", "images_pasted",
            "compactions", "compactions_auto", "compaction_pre_tokens_max",
            "recaps", "recap_last",
            "artifacts_published", "artifact_last_title", "artifact_last_url",
            "custom_title", "ai_title", "legacy_summary", "first_prompt", "last_prompt", "last_reply",
            "forked_from", "fork_point_line", "leaf_uuid", "off_branch_lines",
            "updated_ms",
        };
        constexpr size_t kSessionColumnCount = sizeof(kSessionColumns) / sizeof(kSessionColumns[0]);

        std::string SessionUpsertSql()
        {
            std::string cols, qs;
            for (size_t i = 0; i < kSessionColumnCount; ++i)
            {
                if (i)
                {
                    cols += ", ";
                    qs += ", ";
                }
                cols += kSessionColumns[i];
                qs += "?";
            }
            return "INSERT INTO stg_session (" + cols + ") VALUES (" + qs + ")";
        }
    }

    // ===== the batch =================================================================================

    // One path_access aggregate (a batch's rows for one (session, path, op)).
    struct PathAgg
    {
        std::wstring path;
        int64_t n{ 0 };
        int64_t firstMs{ 0 };
        int64_t lastMs{ 0 };
        int32_t firstLine{ 0 };
    };

    // Everything one transaction appends. The appenders are opened when the batch starts and closed (so
    // their rows are visible to the batch's own SQL) before the derived rows are computed.
    struct Store::Batch
    {
        Connection& con;
        Appender line, edge, doc, prompt, secPrompt, secTool, secResult, secRecap, secCompact, secUser, side;
        std::map<std::tuple<int32_t, std::wstring, uint8_t>, PathAgg> paths; // (sid, path key, op)
        std::map<std::pair<int32_t, std::wstring>, std::pair<int64_t, UsageRec>> usage; // (sid, message id) -> (ts, usage)
        std::vector<FileRow> pendingFiles; // copies carrying the new cursors (applied after COMMIT)
        std::vector<std::pair<int32_t, int32_t>> tops; // (top-level file id, sid key) touched
        std::unordered_map<int32_t, int32_t> fileOfSid; // sid key -> top-level file id
        std::vector<int32_t> shortIdDirty; // sessions whose short id changed (a new neighbor)
        std::vector<int32_t> touchedFiles; // every file id the batch read (evicted from _work on failure)
        int64_t lines{ 0 };
        int64_t bytes{ 0 };
        int64_t hotDocs{ 0 };
        int64_t hotBytes{ 0 };
        uint32_t firstDocId{ 0 };

        explicit Batch(Connection& c) :
            con(c),
            line(c.MakeAppender("line_hot")),
            edge(c.MakeAppender("line_edge")),
            doc(c.MakeAppender("doc_hot")),
            prompt(c.MakeAppender("prompt")),
            secPrompt(c.MakeAppender("sec_prompt")),
            secTool(c.MakeAppender("sec_tool")),
            secResult(c.MakeAppender("sec_result")),
            secRecap(c.MakeAppender("sec_recap")),
            secCompact(c.MakeAppender("sec_compact")),
            secUser(c.MakeAppender("sec_user")),
            side(c.MakeAppender("side_file"))
        {
        }

        // Close every appender (their rows become visible to SQL in this transaction). Throws DbError.
        void CloseAppenders()
        {
            for (Appender* a : { &line, &edge, &doc, &prompt, &secPrompt, &secTool, &secResult, &secRecap, &secCompact, &secUser, &side })
            {
                a->Close();
            }
        }
    };

    // ===== construction / open =======================================================================

    Store::Store(StoreConfig config) :
        _cfg(std::move(config))
    {
    }

    Store::~Store()
    {
        Close();
    }

    void Store::_log(const std::wstring& line) const
    {
        if (_cfg.log)
        {
            _cfg.log(line);
        }
    }

    SchemaState Store::Open()
    {
        const std::wstring dir = SessionDbProtocol::ServiceDir(_cfg.profileDir);
        ::CreateDirectoryW(_cfg.profileDir.c_str(), nullptr);
        ::CreateDirectoryW(dir.c_str(), nullptr);
        ::CreateDirectoryW(SessionDbProtocol::TempDir(_cfg.profileDir).c_str(), nullptr);
        DbOptions opts = _cfg.db;
        if (opts.tempDirectory.empty())
        {
            opts.tempDirectory = ToUtf8(SessionDbProtocol::TempDir(_cfg.profileDir));
        }
        _db = Database::Open(ToUtf8(SessionDbProtocol::DatabasePath(_cfg.profileDir)), opts);
        _w = std::make_unique<Connection>(_db);
        const SchemaState state = EnsureSchema(*_w);
        if (state == SchemaState::TooNew)
        {
            _log(L"[sessdb] the database was written by a newer service (schema " + FromUtf8(GetMeta(*_w, "schema_version")) +
                 L"); this service leaves it alone");
            _w.reset();
            _db = Database{};
            return state;
        }
        // Staging tables for the set-based upserts (regular tables, always emptied before use: an
        // appender targets them by name, which a TEMP table would complicate).
        _w->Execute(R"SQL(
CREATE TABLE IF NOT EXISTS stg_path (sid_key INTEGER, path VARCHAR, path_key VARCHAR, dir_fold VARCHAR, leaf_fold VARCHAR, op UTINYINT,
                                     n INTEGER, first_ms BIGINT, last_ms BIGINT, first_line INTEGER);
CREATE TABLE IF NOT EXISTS stg_usage (sid_key INTEGER, message_id VARCHAR, ts_ms BIGINT, model VARCHAR,
                                      in_tok BIGINT, out_tok BIGINT, cache_read BIGINT, cache_write BIGINT, web_search INTEGER, web_fetch INTEGER);
DELETE FROM stg_path; DELETE FROM stg_usage;
)SQL");
        // The session rows and the file cursors are staged too, then written with ONE set-based
        // INSERT OR REPLACE per batch: a row-at-a-time INSERT OR REPLACE into a wide keyed table measured
        // 47-200 ms per row, the staged form ~4 ms. The staging tables copy the targets' columns (and are
        // re-created at every open, so a schema change can never leave them stale).
        _w->Execute(R"SQL(
DROP TABLE IF EXISTS stg_session; CREATE TABLE stg_session AS SELECT * FROM session LIMIT 0;
DROP TABLE IF EXISTS stg_src; CREATE TABLE stg_src AS SELECT * FROM src_file LIMIT 0;
)SQL");
        _loadMaps(*_w);
        _index.log = _cfg.log;
        // A quarter of the memory limit per sorting statement over the line store (32 MB at least; the default
        // when the limit cannot be read).
        if (const int64_t limit = MemoryLimitBytes(_cfg.db.memoryLimit); limit > 0)
        {
            _index.sortChunkBytes = std::max<int64_t>(limit / kSortChunkShareOfMemory, 32ll * 1024 * 1024);
            // A merge slice: a sixteenth (64 MB of text at 1 GB), 16-256 MB. Its tokenizing also holds the
            // slice's postings in this process's own memory (~1.5 bytes of postings per byte of text).
            _index.mergeSliceBytes = std::clamp<int64_t>(limit / kMergeSliceShareOfMemory, 16ll * 1024 * 1024, 256ll * 1024 * 1024);
        }
        _index.Load(*_w);
        try
        {
            _tempRoots = CollectMachineTempRoots();
        }
        catch (...)
        {
            LogSwallowedException(L"sessiondb Open (temp roots)");
        }
        wchar_t home[MAX_PATH]{};
        if (const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH); n > 0 && n < MAX_PATH)
        {
            _homeKey = NormDirKey(home);
        }
        try
        {
            RefreshFacts();
        }
        catch (...)
        {
            LogSwallowedException(L"sessiondb Open (facts)"); // the facts mirror is a convenience; never fatal
        }
        return state;
    }

    void Store::Close() noexcept
    {
        try
        {
            if (_w)
            {
                _w->Execute("CHECKPOINT");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"sessiondb Close (checkpoint)");
        }
        _sessionUpsert.reset();
        _cursorUpsert.reset();
        _w.reset();
        _db = Database{};
    }

    void Store::_loadMaps(Connection& con)
    {
        std::unique_lock lk{ _mapMtx };
        _files.clear();
        _fileIdByKey.clear();
        _sessions.clear();
        _sidByid.clear();
        _idsSorted.clear();
        {
            auto r = con.Query("SELECT file_id, path, kind, coalesce(sid_key, 0), coalesce(agent_id, ''), coalesce(size_bytes, 0), "
                               "coalesce(mtime_ms, 0), coalesce(birth_ms, 0), parsed_bytes, line_count, coalesce(head_hash, 0), "
                               "seg_count, coalesce(missing_since, 0) FROM src_file");
            while (r.Next())
            {
                FileRow f;
                f.fileId = static_cast<int32_t>(r.Int64(0));
                f.path = FromUtf8(r.Str(1));
                f.pathKey = PathKey(f.path);
                f.kind = static_cast<uint8_t>(r.Int64(2));
                f.sidKey = static_cast<int32_t>(r.Int64(3));
                f.agentId = FromUtf8(r.Str(4));
                f.sizeBytes = r.Int64(5);
                f.mtimeMs = r.Int64(6);
                f.birthMs = r.Int64(7);
                f.parsedBytes = r.Int64(8);
                f.lineCount = static_cast<int32_t>(r.Int64(9));
                f.headHash = r.Int64(10);
                f.segCount = static_cast<int16_t>(r.Int64(11));
                f.missingSince = r.Int64(12);
                _nextFileId = std::max(_nextFileId, f.fileId + 1);
                _fileIdByKey[f.pathKey] = f.fileId;
                _files.emplace(f.fileId, std::move(f));
            }
        }
        {
            auto r = con.Query("SELECT sid_key, session_id, coalesce(file_id, 0), coalesce(short_id, '') FROM session");
            while (r.Next())
            {
                SessionRow s;
                s.sidKey = static_cast<int32_t>(r.Int64(0));
                s.sessionId = LowerAscii(FromUtf8(r.Str(1)));
                s.fileId = static_cast<int32_t>(r.Int64(2));
                s.shortId = FromUtf8(r.Str(3));
                _nextSidKey = std::max(_nextSidKey, s.sidKey + 1);
                _sidByid[s.sessionId] = s.sidKey;
                _idsSorted[s.sessionId] = s.sidKey;
                _sessions.emplace(s.sidKey, std::move(s));
            }
        }
        // A session key may have been handed to a src_file row whose session row is not written yet (its
        // first ingest failed): never reuse it.
        for (const auto& [id, f] : _files)
        {
            _nextSidKey = std::max(_nextSidKey, f.sidKey + 1);
        }
        const int64_t maxDoc = con.ScalarInt64("SELECT greatest(coalesce((SELECT max(doc_id) FROM doc), 0), coalesce((SELECT max(doc_id) FROM doc_hot), 0))");
        _nextDocId = static_cast<uint32_t>(maxDoc + 1);
        _hotDocsEstimate = con.ScalarInt64("SELECT count(*) FROM doc_hot");
        _hotBytesEstimate = con.ScalarInt64("SELECT coalesce(sum(text_len), 0) FROM doc_hot") * 2;
        _lastMergeMs = NowMs();
    }

    int32_t Store::_sessionKeyFor(const std::wstring& sessionIdLower, bool create)
    {
        {
            std::shared_lock lk{ _mapMtx };
            if (const auto it = _sidByid.find(sessionIdLower); it != _sidByid.end())
            {
                return it->second;
            }
        }
        if (!create)
        {
            return 0;
        }
        std::unique_lock lk{ _mapMtx };
        if (const auto it = _sidByid.find(sessionIdLower); it != _sidByid.end())
        {
            return it->second;
        }
        SessionRow s;
        s.sidKey = _nextSidKey++;
        s.sessionId = sessionIdLower;
        _sidByid[sessionIdLower] = s.sidKey;
        _idsSorted[sessionIdLower] = s.sidKey;
        const int32_t key = s.sidKey;
        _sessions.emplace(key, std::move(s));
        lk.unlock();
        _assignShortId(sessionIdLower);
        return key;
    }

    void Store::_assignShortId(const std::wstring& sessionIdLower)
    {
        // The short id (SESSIONS_DB.md §8.4.2): the first 8 characters, longer when another session
        // shares them — the shortest prefix that is unique against BOTH sorted neighbors. A new id can
        // lengthen a neighbor's short id too; those are re-assigned here and marked dirty by the caller's
        // session-row write (every session row carries its short id).
        std::unique_lock lk{ _mapMtx };
        const auto it = _idsSorted.find(sessionIdLower);
        if (it == _idsSorted.end())
        {
            return;
        }
        const auto commonPrefix = [](const std::wstring& a, const std::wstring& b) {
            size_t n = 0;
            while (n < a.size() && n < b.size() && a[n] == b[n])
            {
                ++n;
            }
            return n;
        };
        const auto shortFor = [&](std::map<std::wstring, int32_t>::const_iterator at) {
            size_t need = 8;
            if (at != _idsSorted.begin())
            {
                need = std::max(need, commonPrefix(at->first, std::prev(at)->first) + 1);
            }
            if (const auto nx = std::next(at); nx != _idsSorted.end())
            {
                need = std::max(need, commonPrefix(at->first, nx->first) + 1);
            }
            return at->first.substr(0, std::min(need, at->first.size()));
        };
        for (auto at : { it, it != _idsSorted.begin() ? std::prev(it) : it, std::next(it) != _idsSorted.end() ? std::next(it) : it })
        {
            if (const auto s = _sessions.find(at->second); s != _sessions.end())
            {
                std::wstring next = shortFor(at);
                if (next != s->second.shortId)
                {
                    s->second.shortId = std::move(next);
                    // A NEIGHBOR whose short id grew must have its stored row corrected too (the next batch
                    // writes it; the new session's own row is written by its ingest anyway).
                    if (at != it)
                    {
                        _shortIdDirty.push_back(at->second);
                    }
                }
            }
        }
    }

    Store::FileRow* Store::_fileByPath(const std::wstring& path)
    {
        std::shared_lock lk{ _mapMtx };
        const auto it = _fileIdByKey.find(PathKey(path));
        if (it == _fileIdByKey.end())
        {
            return nullptr;
        }
        const auto f = _files.find(it->second);
        return f == _files.end() ? nullptr : &f->second;
    }

    Store::FileRow& Store::_fileFor(const DiskFile& df, int32_t sidKey)
    {
        const std::wstring key = PathKey(df.path);
        std::unique_lock lk{ _mapMtx };
        if (const auto it = _fileIdByKey.find(key); it != _fileIdByKey.end())
        {
            FileRow& f = _files[it->second];
            f.path = df.path; // the original spelling as found now
            return f;
        }
        FileRow f;
        f.fileId = _nextFileId++;
        f.path = df.path;
        f.pathKey = key;
        f.kind = df.kind;
        f.sidKey = sidKey;
        f.agentId = df.agentId;
        f.birthMs = df.birthMs;
        const int32_t id = f.fileId;
        _fileIdByKey[key] = id;
        return _files.emplace(id, std::move(f)).first->second;
    }

    // ===== discovery =================================================================================

    bool Store::_statFile(const std::wstring& path, uint8_t kind, const std::wstring& sid, const std::wstring& agentId, DiskFile& out) const
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return false;
        }
        out.path = path;
        out.kind = kind;
        out.sessionId = sid;
        out.agentId = agentId;
        out.size = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        out.mtimeMs = FileTimeToMs(fad.ftLastWriteTime);
        out.birthMs = FileTimeToMs(fad.ftCreationTime);
        return true;
    }

    namespace
    {
        // List one folder (files only), calling `fn(name, size, mtimeMs, birthMs)`.
        template<typename Fn>
        void ListFiles(const std::wstring& dir, Fn&& fn)
        {
            WIN32_FIND_DATAW fd{};
            const HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                const int64_t size = (static_cast<int64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                fn(std::wstring{ fd.cFileName }, size, FileTimeToMs(fd.ftLastWriteTime), FileTimeToMs(fd.ftCreationTime));
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }

        // List one folder's sub-folders.
        std::vector<std::wstring> ListDirs(const std::wstring& dir)
        {
            std::vector<std::wstring> out;
            WIN32_FIND_DATAW fd{};
            const HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE)
            {
                return out;
            }
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                {
                    out.emplace_back(fd.cFileName);
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
            return out;
        }

        bool EndsWithI(const std::wstring& s, std::wstring_view suffix)
        {
            return s.size() >= suffix.size() && _wcsnicmp(s.c_str() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
        }
    }

    std::vector<Store::DiskFile> Store::_listSideFiles(const std::wstring& transcriptPath, const std::wstring& sid) const
    {
        std::vector<DiskFile> out;
        const size_t slash = transcriptPath.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return out;
        }
        std::wstring stem = transcriptPath.substr(slash + 1);
        if (EndsWithI(stem, L".jsonl"))
        {
            stem.resize(stem.size() - 6);
        }
        const std::wstring sideDir = transcriptPath.substr(0, slash) + L"\\" + stem;
        ListFiles(sideDir + L"\\subagents", [&](const std::wstring& name, int64_t size, int64_t mtime, int64_t birth) {
            DiskFile df;
            df.path = sideDir + L"\\subagents\\" + name;
            df.sessionId = sid;
            df.size = size;
            df.mtimeMs = mtime;
            df.birthMs = birth;
            if (EndsWithI(name, L".jsonl"))
            {
                df.kind = 1;
                df.agentId = name.substr(0, name.size() - 6);
                if (df.agentId.rfind(L"agent-", 0) == 0)
                {
                    df.agentId = df.agentId.substr(6);
                }
                out.push_back(std::move(df));
            }
            else if (EndsWithI(name, L".meta.json"))
            {
                df.kind = 2;
                df.agentId = name.substr(0, name.size() - 10);
                if (df.agentId.rfind(L"agent-", 0) == 0)
                {
                    df.agentId = df.agentId.substr(6);
                }
                out.push_back(std::move(df));
            }
        });
        ListFiles(sideDir + L"\\tool-results", [&](const std::wstring& name, int64_t size, int64_t mtime, int64_t birth) {
            DiskFile df;
            df.path = sideDir + L"\\tool-results\\" + name;
            df.sessionId = sid;
            df.kind = 3;
            df.size = size;
            df.mtimeMs = mtime;
            df.birthMs = birth;
            out.push_back(std::move(df));
        });
        return out;
    }

    std::vector<Store::DiskFile> Store::_listAllFiles(const CancelFn& cancel) const
    {
        std::vector<DiskFile> out;
        for (const auto& proj : ListDirs(_cfg.projectsDir))
        {
            if (cancel && cancel())
            {
                break;
            }
            const std::wstring pdir = _cfg.projectsDir + L"\\" + proj;
            ListFiles(pdir, [&](const std::wstring& name, int64_t size, int64_t mtime, int64_t birth) {
                if (!EndsWithI(name, L".jsonl"))
                {
                    return;
                }
                const std::wstring stem = name.substr(0, name.size() - 6);
                if (!IsSessionIdStem(stem))
                {
                    return; // legacy agent-*.jsonl and other non-session files
                }
                DiskFile df;
                df.path = pdir + L"\\" + name;
                df.kind = 0;
                df.sessionId = LowerAscii(stem);
                df.size = size;
                df.mtimeMs = mtime;
                df.birthMs = birth;
                out.push_back(std::move(df));
            });
            for (const auto& sub : ListDirs(pdir))
            {
                if (!IsSessionIdStem(sub))
                {
                    continue;
                }
                auto sides = _listSideFiles(pdir + L"\\" + sub + L".jsonl", LowerAscii(sub));
                out.insert(out.end(), std::make_move_iterator(sides.begin()), std::make_move_iterator(sides.end()));
            }
        }
        return out;
    }

    std::wstring Store::_resolveTranscript(const std::wstring& sid) const
    {
        {
            std::shared_lock lk{ _mapMtx };
            if (const auto s = _sidByid.find(sid); s != _sidByid.end())
            {
                if (const auto row = _sessions.find(s->second); row != _sessions.end())
                {
                    if (const auto f = _files.find(row->second.fileId); f != _files.end())
                    {
                        if (::GetFileAttributesW(f->second.path.c_str()) != INVALID_FILE_ATTRIBUTES)
                        {
                            return f->second.path;
                        }
                    }
                }
            }
        }
        // A session never ingested, or whose file moved (/cd relocates a transcript): look it up on disk.
        return ResolveClaudeTranscriptPath(sid);
    }

    // ===== reads =====================================================================================

    std::optional<SessionKey> Store::ResolveSession(std::wstring_view idOrPrefix, int& matches) const
    {
        matches = 0;
        std::wstring key = LowerAscii(std::wstring{ idOrPrefix });
        // Tolerate a pasted {guid} and surrounding whitespace.
        while (!key.empty() && (key.front() == L'{' || key.front() == L' '))
        {
            key.erase(key.begin());
        }
        while (!key.empty() && (key.back() == L'}' || key.back() == L' '))
        {
            key.pop_back();
        }
        if (key.size() < 4)
        {
            return std::nullopt;
        }
        std::shared_lock lk{ _mapMtx };
        std::optional<SessionKey> found;
        for (auto it = _idsSorted.lower_bound(key); it != _idsSorted.end() && it->first.compare(0, key.size(), key) == 0; ++it)
        {
            ++matches;
            if (const auto s = _sessions.find(it->second); s != _sessions.end())
            {
                found = SessionKey{ s->second.sidKey, s->second.fileId, s->second.sessionId, s->second.shortId };
            }
            if (it->first == key)
            {
                matches = 1; // an exact whole id wins over longer ids it prefixes (impossible for UUIDs, kept for safety)
                break;
            }
        }
        if (matches != 1)
        {
            return std::nullopt;
        }
        return found;
    }

    std::wstring Store::ShortIdOf(int32_t sidKey) const
    {
        std::shared_lock lk{ _mapMtx };
        const auto s = _sessions.find(sidKey);
        return s == _sessions.end() ? std::wstring{} : s->second.shortId;
    }

    std::wstring Store::SessionIdOf(int32_t sidKey) const
    {
        std::shared_lock lk{ _mapMtx };
        const auto s = _sessions.find(sidKey);
        return s == _sessions.end() ? std::wstring{} : s->second.sessionId;
    }

    std::wstring Store::AgentIdOfFile(int32_t fileId) const
    {
        std::shared_lock lk{ _mapMtx };
        const auto f = _files.find(fileId);
        return f == _files.end() || f->second.kind != 1 ? std::wstring{} : f->second.agentId;
    }

    int32_t Store::SubagentFileOf(int32_t sidKey, std::wstring_view agentId) const
    {
        std::shared_lock lk{ _mapMtx };
        for (const auto& [id, f] : _files)
        {
            // A linear walk: a ref names a subagent rarely, and the file map holds a few thousand rows.
            if (f.kind == 1 && f.sidKey == sidKey && _wcsnicmp(f.agentId.c_str(), agentId.data(), agentId.size()) == 0 &&
                f.agentId.size() == agentId.size())
            {
                return id;
            }
        }
        return 0;
    }

    std::vector<SessionKey> Store::AllSessions() const
    {
        std::shared_lock lk{ _mapMtx };
        std::vector<SessionKey> out;
        out.reserve(_sessions.size());
        for (const auto& [k, s] : _sessions)
        {
            out.push_back(SessionKey{ s.sidKey, s.fileId, s.sessionId, s.shortId });
        }
        return out;
    }

    std::unordered_set<std::wstring> Store::OpenSessions() const
    {
        std::shared_lock lk{ _mapMtx };
        return _open;
    }

    StoreStatus Store::Status(Connection& con) const
    {
        StoreStatus st;
        auto r = con.Query("SELECT (SELECT count(*) FROM session), (SELECT count(*) FROM src_file), (SELECT count(*) FROM line), "
                           "(SELECT count(*) FROM line_hot), (SELECT count(*) FROM doc), (SELECT count(*) FROM doc_hot), (SELECT count(*) FROM dict), "
                           "(SELECT coalesce(min(last_activity_ms), 0) FROM session WHERE last_activity_ms > 0)");
        if (r.Next())
        {
            st.sessions = r.Int64(0);
            st.files = r.Int64(1);
            st.lines = r.Int64(2);
            st.hotLines = r.Int64(3);
            st.docs = r.Int64(4);
            st.hotDocs = r.Int64(5);
            st.terms = r.Int64(6);
            st.coveredSinceMs = r.Int64(7);
        }
        st.coldBuildDone = !GetMeta(con, "cold_build_done").empty();
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const std::wstring path = SessionDbProtocol::DatabasePath(_cfg.profileDir);
        if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        {
            st.dbBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
        if (::GetFileAttributesExW((path + L".wal").c_str(), GetFileExInfoStandard, &fad))
        {
            st.dbBytes += (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
        st.lastWriteMs = _lastWriteMs.load();
        return st;
    }

    // ===== the refresh tick and the sweep ============================================================

    std::vector<UpsertResult> Store::UpsertSessions(const std::vector<SessionRef>& refs, const CancelFn& cancel)
    {
        const int64_t started = TickMs();
        _times = StepTimes{};
        std::vector<UpsertResult> results(refs.size());
        // Resolve and stat every named session first (cheap), then ingest them in byte-bounded batches.
        std::vector<std::pair<DiskFile, std::vector<DiskFile>>> work;
        std::vector<size_t> workIndex; // work[i] answers results[workIndex[i]]
        for (size_t i = 0; i < refs.size(); ++i)
        {
            results[i].id = refs[i].id;
            const std::wstring sid = LowerAscii(refs[i].id);
            if (!IsSessionIdStem(sid))
            {
                results[i].ok = false;
                results[i].error = L"not a session id";
                continue;
            }
            const std::wstring path = refs[i].path.empty() ? _resolveTranscript(sid) : refs[i].path;
            DiskFile top;
            if (!_statFile(path, 0, sid, {}, top))
            {
                // Gone (Claude's sweep, or a /cd move whose new path the app will send next tick): keep
                // the rows, mark the transcript deleted (D7).
                results[i].missing = true;
                if (FileRow* f = path.empty() ? nullptr : _fileByPath(path); f && f->missingSince == 0)
                {
                    try
                    {
                        auto p = _w->Prepare("UPDATE src_file SET missing_since = ? WHERE file_id = ?");
                        p.Bind(1, NowMs()).Bind(2, static_cast<int64_t>(f->fileId));
                        p.Execute();
                        auto q = _w->Prepare("UPDATE session SET transcript = 1 WHERE sid_key = ?");
                        q.Bind(1, static_cast<int64_t>(f->sidKey));
                        q.Execute();
                        std::unique_lock lk{ _mapMtx };
                        f->missingSince = NowMs();
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"sessiondb UpsertSessions (mark missing)");
                    }
                }
                continue;
            }
            work.emplace_back(std::move(top), _listSideFiles(path, sid));
            workIndex.push_back(i);
        }
        // Batches of whole sessions, each one transaction (§7.6.3).
        size_t at = 0;
        while (at < work.size())
        {
            if (cancel && cancel())
            {
                for (size_t k = at; k < work.size(); ++k)
                {
                    results[workIndex[k]].ok = false;
                    results[workIndex[k]].error = L"cancelled";
                }
                break;
            }
            size_t end = at;
            int64_t bytes = 0;
            while (end < work.size() && (end == at || bytes < kBatchBytes))
            {
                const FileRow* f = _fileByPath(work[end].first.path);
                bytes += work[end].first.size - (f ? f->parsedBytes : 0);
                ++end;
            }
            std::vector<std::pair<DiskFile, std::vector<DiskFile>>> slice(work.begin() + at, work.begin() + end);
            try
            {
                const auto outcomes = _ingestSessions(slice);
                for (size_t k = 0; k < outcomes.size(); ++k)
                {
                    results[workIndex[at + k]].cursor = outcomes[k].first;
                    results[workIndex[at + k]].lines = outcomes[k].second;
                }
            }
            catch (const std::exception& e)
            {
                LogSwallowedException(L"sessiondb UpsertSessions (batch)");
                for (size_t k = at; k < end; ++k)
                {
                    results[workIndex[k]].ok = false;
                    results[workIndex[k]].error = FromUtf8(e.what());
                }
            }
            at = end;
        }
        try
        {
            IngestHistory(); // history.jsonl is not under projects/: a tick that sends anything stats it (§7.6.1)
        }
        catch (...)
        {
            LogSwallowedException(L"sessiondb UpsertSessions (history)");
        }
        // A slow tick says where its time went (a tick runs every few seconds, so only the slow ones are logged).
        const int64_t took = TickMs() - started;
        if (took >= 250)
        {
            int64_t lines = 0;
            for (const auto& r : results)
            {
                lines += r.lines;
            }
            _log(L"[sessdb] upsert: " + std::to_wstring(refs.size()) + L" session(s), " + std::to_wstring(lines) + L" line(s) in " + std::to_wstring(took) +
                 L" ms; steps (ms): ingest " + std::to_wstring(_times.ingest) + L", walk " + std::to_wstring(_times.walk) + L", summary " +
                 std::to_wstring(_times.summary) + L", session rows " + std::to_wstring(_times.sessionRows) + L", cursors " + std::to_wstring(_times.cursors) +
                 L", commit " + std::to_wstring(_times.commit));
        }
        return results;
    }

    ResyncResult Store::Resync(const CancelFn& cancel, const std::function<void(int64_t, int64_t)>& progress, size_t maxSessions)
    {
        ResyncResult rr;
        const int64_t started = NowMs();
        _times = StepTimes{};
        const auto all = _listAllFiles(cancel);
        rr.filesSeen = static_cast<int64_t>(all.size());

        // Group by session: the top-level transcript and its side files.
        std::unordered_map<std::wstring, std::pair<std::optional<DiskFile>, std::vector<DiskFile>>> bySession;
        std::unordered_set<std::wstring> seenKeys;
        for (const auto& df : all)
        {
            seenKeys.insert(PathKey(df.path));
            auto& slot = bySession[df.sessionId];
            if (df.kind == 0)
            {
                slot.first = df;
            }
            else
            {
                slot.second.push_back(df);
            }
        }
        // A file changed when its size or modification time differs from what the last ingest recorded
        // (append-only transcripts; a spill or meta file is written once).
        const auto changed = [this](const DiskFile& df) {
            const FileRow* f = _fileByPath(df.path);
            return !f || f->sizeBytes != df.size || f->mtimeMs != df.mtimeMs || f->missingSince != 0;
        };
        std::vector<std::pair<DiskFile, std::vector<DiskFile>>> todo;
        for (auto& [sid, slot] : bySession)
        {
            if (!slot.first)
            {
                continue; // side files of a session whose transcript is gone: nothing to anchor them to
            }
            bool any = changed(*slot.first);
            for (const auto& s : slot.second)
            {
                any = any || changed(s);
            }
            if (any)
            {
                todo.emplace_back(*slot.first, slot.second);
            }
        }
        rr.filesChanged = static_cast<int64_t>(todo.size());
        // Newest sessions first (§7.3): the page is useful while the older ones load.
        std::sort(todo.begin(), todo.end(), [](const auto& a, const auto& b) { return a.first.mtimeMs > b.first.mtimeMs; });
        const bool limited = maxSessions != 0 && todo.size() > maxSessions;
        if (limited)
        {
            todo.resize(maxSessions);
        }
        _log(L"[sessdb] resync: " + std::to_wstring(all.size()) + L" file(s), " + std::to_wstring(todo.size()) + L" session(s) to ingest");

        // BULK mode for a big sweep (a cold build, or a catch-up after a long downtime): DuckDB's automatic
        // checkpoint (every 16 MB of write-ahead log) would compress the hot tables at nearly every commit and
        // then again after each merge moved their rows; with a large threshold the hot rows live in the log
        // only, the merge after every batch moves them into the final tables, and ONE checkpoint at the end
        // compresses what stays. Measured on 468 MB: the commits went from ~18 s to (see the log) — and the
        // threshold is restored whatever happens.
        int64_t todoBytes = 0;
        for (const auto& item : todo)
        {
            const FileRow* f = _fileByPath(item.first.path);
            todoBytes += std::max<int64_t>(0, item.first.size - (f ? f->parsedBytes : 0));
        }
        const bool bulk = todo.size() > 20 || todoBytes > 256ll * 1024 * 1024;
        if (bulk)
        {
            _w->Execute("SET checkpoint_threshold = '8GB'");
        }
        struct RestoreThreshold
        {
            Connection* con;
            bool active;
            ~RestoreThreshold()
            {
                if (active)
                {
                    try
                    {
                        con->Execute("RESET checkpoint_threshold");
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"sessiondb Resync (restore the checkpoint threshold)");
                    }
                }
            }
        } restoreThreshold{ _w.get(), bulk };

        size_t at = 0;
        int64_t done = 0;
        int64_t batches = 0;
        int64_t mergeMs = 0; // the merges after the batches (bulk mode), for the progress line
        int64_t lastProgressMs = NowMs();
        while (at < todo.size())
        {
            if (cancel && cancel())
            {
                rr.cancelled = true;
                break;
            }
            // A long sweep (the cold build) says where it is and where its time goes, once a minute: the steps
            // that grow with the database show up here long before the sweep ends.
            if (NowMs() - lastProgressMs >= 60000)
            {
                lastProgressMs = NowMs();
                const int64_t secs = std::max<int64_t>(1, (NowMs() - started) / 1000);
                _log(L"[sessdb] resync progress: " + std::to_wstring(done) + L" / " + std::to_wstring(todo.size()) + L" session(s), " +
                     std::to_wstring(rr.bytesRead / (1024 * 1024)) + L" MB in " + std::to_wstring(secs) + L" s (" +
                     std::to_wstring(rr.bytesRead / (1024 * 1024) / secs) + L" MB/s), " + std::to_wstring(batches) + L" batch(es); steps (ms): ingest " +
                     std::to_wstring(_times.ingest) + L", appenders " + std::to_wstring(_times.appenders) + L", paths " + std::to_wstring(_times.paths) +
                     L", usage " + std::to_wstring(_times.usage) + L", walk " + std::to_wstring(_times.walk) + L", summary " + std::to_wstring(_times.summary) +
                     L", session rows " + std::to_wstring(_times.sessionRows) + L", cursors " + std::to_wstring(_times.cursors) + L", commit " +
                     std::to_wstring(_times.commit) + L", merges " + std::to_wstring(mergeMs));
            }
            size_t end = at;
            int64_t bytes = 0;
            while (end < todo.size() && (end == at || bytes < kBatchBytes))
            {
                const FileRow* f = _fileByPath(todo[end].first.path);
                bytes += std::max<int64_t>(0, todo[end].first.size - (f ? f->parsedBytes : 0));
                ++end;
            }
            std::vector<std::pair<DiskFile, std::vector<DiskFile>>> slice(todo.begin() + at, todo.begin() + end);
            try
            {
                const auto outcomes = _ingestSessions(slice);
                for (const auto& o : outcomes)
                {
                    rr.linesIngested += o.second;
                }
                rr.sessionsUpserted += static_cast<int64_t>(outcomes.size());
                rr.bytesRead += bytes;
            }
            catch (...)
            {
                // One bad batch must not stop the sweep: log it, skip it (its cursors did not move, so the
                // next sweep retries it).
                LogSwallowedException(L"sessiondb Resync (batch)");
            }
            done += static_cast<int64_t>(end - at);
            at = end;
            ++batches;
            if (progress)
            {
                progress(done, static_cast<int64_t>(todo.size()));
            }
            if (bulk ? _hotDocsEstimate > 0 : MergeDue())
            {
                try
                {
                    const int64_t m0 = NowMs();
                    Merge(cancel);
                    mergeMs += NowMs() - m0;
                }
                catch (...)
                {
                    LogSwallowedException(L"sessiondb Resync (merge)");
                }
            }
        }
        if (bulk)
        {
            try
            {
                if (_hotDocsEstimate > 0)
                {
                    Merge(cancel);
                }
                const int64_t cp = TickMs();
                _w->Execute("CHECKPOINT");
                _log(L"[sessdb] resync: checkpoint " + std::to_wstring(TickMs() - cp) + L" ms");
            }
            catch (...)
            {
                LogSwallowedException(L"sessiondb Resync (final merge + checkpoint)");
            }
        }
        // Transcripts the maps know but the listing no longer has: deleted by Claude's sweep (D7) or moved.
        // (A limited sweep saw everything but ingested part of it: it marks nothing and claims no cold build.)
        if (!rr.cancelled && !limited)
        {
            std::vector<FileRow> vanished;
            {
                std::shared_lock lk{ _mapMtx };
                for (const auto& [id, f] : _files)
                {
                    if (f.kind == 0 && f.missingSince == 0 && seenKeys.find(f.pathKey) == seenKeys.end())
                    {
                        vanished.push_back(f);
                    }
                }
            }
            if (!vanished.empty())
            {
                try
                {
                    Transaction tx{ *_w };
                    auto p = _w->Prepare("UPDATE src_file SET missing_since = ? WHERE file_id = ?");
                    auto q = _w->Prepare("UPDATE session SET transcript = 1 WHERE sid_key = ?");
                    const int64_t now = NowMs();
                    for (const auto& f : vanished)
                    {
                        p.Bind(1, now).Bind(2, static_cast<int64_t>(f.fileId));
                        p.Execute();
                        q.Bind(1, static_cast<int64_t>(f.sidKey));
                        q.Execute();
                    }
                    tx.Commit();
                    std::unique_lock lk{ _mapMtx };
                    for (const auto& f : vanished)
                    {
                        if (const auto it = _files.find(f.fileId); it != _files.end())
                        {
                            it->second.missingSince = now;
                        }
                    }
                    _log(L"[sessdb] resync: " + std::to_wstring(vanished.size()) + L" transcript(s) gone from disk, kept and marked deleted");
                }
                catch (...)
                {
                    LogSwallowedException(L"sessiondb Resync (vanished)");
                }
            }
            try
            {
                IngestHistory();
            }
            catch (...)
            {
                LogSwallowedException(L"sessiondb Resync (history)");
            }
            if (GetMeta(*_w, "cold_build_done").empty())
            {
                SetMeta(*_w, "cold_build_done", std::to_string(NowMs()));
            }
        }
        rr.ms = NowMs() - started;
        _log(L"[sessdb] resync done: " + std::to_wstring(rr.sessionsUpserted) + L" session(s), " + std::to_wstring(rr.linesIngested) +
             L" line(s), " + std::to_wstring(rr.bytesRead / (1024 * 1024)) + L" MB in " + std::to_wstring(rr.ms) + L" ms" +
             (rr.cancelled ? L" (cancelled)" : L""));
        _log(L"[sessdb] resync steps (ms): ingest " + std::to_wstring(_times.ingest) + L", appenders " + std::to_wstring(_times.appenders) +
             L", paths " + std::to_wstring(_times.paths) + L", usage " + std::to_wstring(_times.usage) + L", walk " + std::to_wstring(_times.walk) +
             L", summary " + std::to_wstring(_times.summary) + L", session rows " + std::to_wstring(_times.sessionRows) + L", cursors " +
             std::to_wstring(_times.cursors) + L", commit " + std::to_wstring(_times.commit));
        return rr;
    }

    // ===== the ingest ================================================================================

    Store::FileWork& Store::_workFor(Connection& con, FileRow& top)
    {
        if (const auto it = _work.find(top.fileId); it != _work.end())
        {
            it->second.lastUse = ++_workClock;
            return it->second;
        }
        // Evict the least recently used working set past the bound.
        if (_work.size() >= kMaxWorkingFiles)
        {
            auto victim = _work.begin();
            for (auto it = _work.begin(); it != _work.end(); ++it)
            {
                if (it->second.lastUse < victim->second.lastUse)
                {
                    victim = it;
                }
            }
            _work.erase(victim);
        }
        FileWork w;
        w.lastUse = ++_workClock;
        if (top.parsedBytes > 0)
        {
            // The fold from the database (committed with the cursor), and the uuids this file already holds.
            auto p = con.Prepare("SELECT coalesce(fold, '') FROM src_file WHERE file_id = ?");
            p.Bind(1, static_cast<int64_t>(top.fileId));
            auto r = p.Execute();
            const std::string foldJson = r.Next() ? r.String(0) : std::string{};
            if (!FoldFromJson(foldJson, w.fold))
            {
                // A fold that cannot be read (another version wrote it): rebuild the file from 0.
                _log(L"[sessdb] " + ShortIdOf(top.sidKey) + L": fold unreadable, re-ingesting the transcript");
                _forgetFile(con, top);
            }
            else
            {
                auto q = con.Prepare("SELECT uuid FROM line WHERE file_id = ? AND uuid IS NOT NULL UNION ALL "
                                     "SELECT uuid FROM line_hot WHERE file_id = ? AND uuid IS NOT NULL");
                q.Bind(1, static_cast<int64_t>(top.fileId)).Bind(2, static_cast<int64_t>(top.fileId));
                auto u = q.Execute();
                while (u.Next())
                {
                    w.uuids.insert(FromUtf8(u.Str(0)));
                }
            }
        }
        return _work.emplace(top.fileId, std::move(w)).first->second;
    }

    void Store::_forgetFile(Connection& con, FileRow& file)
    {
        // Every row this file contributed goes; its blocks already merged into the index are tombstoned
        // (the next re-cluster drops their postings, §6.4). Runs inside the caller's transaction.
        const int64_t id = file.fileId;
        const auto exec = [&con, id](const char* sql, int params) {
            auto p = con.Prepare(sql);
            for (int i = 1; i <= params; ++i)
            {
                p.Bind(static_cast<size_t>(i), id);
            }
            p.Execute();
        };
        exec("INSERT INTO tomb SELECT doc_id FROM doc WHERE file_id = ?", 1);
        exec("DELETE FROM prompt WHERE doc_id IN (SELECT doc_id FROM doc WHERE file_id = ? UNION ALL SELECT doc_id FROM doc_hot WHERE file_id = ?)", 2);
        exec("DELETE FROM doc_hot WHERE file_id = ?", 1);
        exec("DELETE FROM line WHERE file_id = ?", 1);
        exec("DELETE FROM line_hot WHERE file_id = ?", 1);
        exec("DELETE FROM line_edge WHERE file_id = ?", 1);
        exec("DELETE FROM side_file WHERE file_id = ?", 1);
        for (const char* t : { "sec_prompt", "sec_tool", "sec_result", "sec_recap", "sec_compact", "sec_user" })
        {
            exec((std::string("DELETE FROM ") + t + " WHERE file_id = ?").c_str(), 1);
        }
        file.parsedBytes = 0;
        file.lineCount = 0;
        file.segCount = 0;
        file.headHash = 0;
        if (file.kind == 0)
        {
            // The session-level rows were built from the whole session (top + sides): they are rebuilt from 0
            // too, so the side files' cursors reset with it (the caller re-reads them in the same batch).
            auto p = con.Prepare("DELETE FROM path_access WHERE sid_key = ?");
            p.Bind(1, static_cast<int64_t>(file.sidKey));
            p.Execute();
            auto q = con.Prepare("DELETE FROM usage WHERE sid_key = ?");
            q.Bind(1, static_cast<int64_t>(file.sidKey));
            q.Execute();
            if (const auto it = _work.find(file.fileId); it != _work.end())
            {
                it->second.fold = SessionFold{};
                it->second.uuids.clear();
            }
        }
    }

    int64_t Store::_ingestFile(Batch& batch, FileRow& fileIn, const DiskFile& df, FileWork* work, bool forceFromZero, bool* replacedOut)
    {
        // Work on a COPY of the file row: the in-memory row changes only after the batch commits (a
        // rolled-back batch must leave the cursor where the database has it).
        FileRow file = fileIn;
        file.path = df.path;
        file.sizeBytes = df.size;
        file.mtimeMs = df.mtimeMs;
        if (file.birthMs == 0)
        {
            file.birthMs = df.birthMs;
        }
        file.missingSince = 0;
        batch.touchedFiles.push_back(file.fileId);
        if (replacedOut)
        {
            *replacedOut = false;
        }

        File fh{ df.path };
        if (!fh.Ok())
        {
            return 0; // unreadable right now (locked, or gone since the stat): the cursor stays, the next tick retries
        }

        // A file that shrank, or whose head changed, was REPLACED (or the caller forces a rebuild because
        // the session's transcript was): everything it contributed is rebuilt from offset 0.
        const std::string head = fh.ReadAt(0, 4096);
        const int64_t headHash = Fnv1a(head);
        const bool replaced = file.parsedBytes > 0 &&
                              (forceFromZero || df.size < file.parsedBytes || (file.headHash != 0 && file.headHash != headHash && head.size() >= 4096));
        if (replaced)
        {
            _log(L"[sessdb] " + ShortIdOf(file.sidKey) + L": " + df.path + L" changed under its cursor (size " + std::to_wstring(df.size) +
                 L", cursor " + std::to_wstring(file.parsedBytes) + L"): re-ingesting it");
            _forgetFile(batch.con, file);
            if (work)
            {
                work->fold = SessionFold{};
                work->uuids.clear();
            }
            if (replacedOut)
            {
                *replacedOut = true;
            }
        }
        // The head hash covers the first 4 KB once the file has that many bytes (a shorter head keeps
        // growing, and must not read as a replacement).
        if (head.size() >= 4096 || file.headHash == 0)
        {
            file.headHash = headHash;
        }

        // Spills and subagent meta files are written once: a change replaces the stored copy whole.
        if (file.kind == 2 || file.kind == 3)
        {
            if (file.parsedBytes == df.size && file.parsedBytes != 0)
            {
                batch.pendingFiles.push_back(file);
                return 0;
            }
            const std::string bytes = fh.ReadAt(0, static_cast<size_t>(std::min<int64_t>(df.size, 256ll * 1024 * 1024)));
            if (file.parsedBytes != 0 && !replaced)
            {
                _forgetFile(batch.con, file);
            }
            batch.side.Int32(file.fileId).Int64(static_cast<int64_t>(bytes.size())).Blob(bytes.data(), bytes.size()).EndRow();
            if (file.kind == 3)
            {
                std::wstring stem = df.path.substr(df.path.find_last_of(L"\\/") + 1);
                if (const size_t dot = stem.rfind(L'.'); dot != std::wstring::npos)
                {
                    stem.resize(dot);
                }
                if (const auto block = ExtractSpillBlock(bytes, stem))
                {
                    const uint32_t docId = _nextDocId++;
                    const std::string text = ToUtf8(block->text);
                    const std::string fold = ToUtf8(FoldForIndex(block->text));
                    batch.doc.UInt32(docId).Int32(file.sidKey).Int32(file.fileId).Int32(0).UInt16(0).Int64(df.mtimeMs)
                        .UInt8(block->role).UInt8(block->flags).Null().StrOrNull(ToUtf8(block->toolUseId))
                        .Int32(static_cast<int32_t>(block->text.size())).Int32(0).Str(text).Str(fold).EndRow();
                    ++batch.hotDocs;
                    batch.hotBytes += static_cast<int64_t>(text.size() + fold.size());
                }
            }
            file.parsedBytes = static_cast<int64_t>(bytes.size());
            file.lineCount = 0;
            batch.bytes += static_cast<int64_t>(bytes.size());
            batch.pendingFiles.push_back(file);
            return 0;
        }

        // A transcript (top-level or subagent).
        const bool top = file.kind == 0;
        SessionFold* fold = (top && work) ? &work->fold : nullptr;
        int64_t linesIn = 0;

        // One complete line (without its '\n') -> every row it contributes.
        const auto processLine = [&](std::string_view raw, bool cut) {
            const int32_t lineNo = ++file.lineCount;
            const int64_t lineId = (static_cast<int64_t>(file.fileId) << 32) | static_cast<uint32_t>(lineNo);
            ++linesIn;
            const LineFacts facts = cut ? LineFacts{} : ExtractLine(raw, !top);
            // The analyzer's copy rule: a uuid already seen in THIS file is a re-written copy.
            const bool dup = !facts.uuid.empty() && work && !work->uuids.insert(facts.uuid).second;
            uint8_t flags = facts.flags;
            if (dup)
            {
                flags |= kLfDupInFile;
            }
            if (!dup && (flags & kLfCompactBoundary))
            {
                ++file.segCount; // the boundary itself carries the new segment number
            }
            const int16_t seg = file.segCount;
            if (fold)
            {
                ApplyLine(*fold, facts, lineNo, &NormDirKey);
            }

            // --- the lossless store (a line that is not UTF-8, or was cut at the cap, keeps raw bytes) ---
            batch.line.Int64(lineId).Int32(file.fileId).Int32(lineNo)
                .StrOrNull(ToUtf8(facts.uuid)).StrOrNull(ToUtf8(facts.parentUuid)).StrOrNull(ToUtf8(facts.leafUuid))
                .StrOrNull(ToUtf8(facts.type)).StrOrNull(ToUtf8(facts.subtype)).Int64(facts.tsMs).StrOrNull(ToUtf8(facts.branch))
                .UInt8(flags).Int16(seg).Bool(true).Int32(static_cast<int32_t>(raw.size()));
            if (!cut && IsValidUtf8(raw))
            {
                batch.line.Str(raw).Null();
            }
            else
            {
                batch.line.Null().Blob(raw.data(), raw.size());
            }
            batch.line.EndRow();
            for (const auto& t : facts.extraEdges)
            {
                batch.edge.Int32(file.fileId).Int32(lineNo).Str(ToUtf8(t)).EndRow();
            }
            if (!facts.json)
            {
                return;
            }

            // --- tool results name their tool (through the fold's calls awaiting a result) ---
            std::vector<std::wstring> resultTools;
            resultTools.reserve(facts.toolResults.size());
            for (const auto& r : facts.toolResults)
            {
                resultTools.push_back(fold ? TakeToolName(*fold, r.id) : std::wstring{});
            }

            // --- the summary's section rows (top-level, non-copy lines only; D14) ---
            if (top && !dup)
            {
                if (!facts.summaryPrompt.empty())
                {
                    batch.secPrompt.Int32(file.fileId).Int32(lineNo).Int16(seg).UInt8(facts.summaryPromptKind)
                        .StrOrNull(ToUtf8(facts.uuid)).Str(ToUtf8(facts.summaryPrompt)).EndRow();
                }
                for (const auto& tu : facts.toolUses)
                {
                    if (tu.name == L"Read" || tu.name == L"Edit" || tu.name == L"Write" || tu.name == L"Skill" || tu.name == L"TodoWrite" ||
                        tu.name == L"ExitPlanMode" || tu.name == L"AskUserQuestion")
                    {
                        batch.secTool.Int32(file.fileId).Int32(lineNo).StrOrNull(ToUtf8(tu.id)).Str(ToUtf8(tu.name))
                            .StrOrNull(ToUtf8(tu.path)).StrOrNull(ToUtf8(tu.label)).Bool(tu.plans).StrOrNull(ToUtf8(tu.skill));
                        if (tu.todosDone >= 0)
                        {
                            batch.secTool.Int16(tu.todosDone).Int16(tu.todosOpen);
                        }
                        else
                        {
                            batch.secTool.Null().Null();
                        }
                        batch.secTool.EndRow();
                    }
                }
                for (size_t k = 0; k < facts.toolResults.size(); ++k)
                {
                    const std::wstring& name = resultTools[k];
                    if (name == L"Write" || name == L"AskUserQuestion")
                    {
                        batch.secResult.Int32(file.fileId).Int32(lineNo).Str(ToUtf8(facts.toolResults[k].id))
                            .Bool(name == L"AskUserQuestion").Bool(facts.toolResults[k].created).EndRow();
                    }
                }
                if (!facts.recap.empty())
                {
                    batch.secRecap.Int32(file.fileId).Int32(lineNo).Str(ToUtf8(facts.recap)).EndRow();
                }
                if (!facts.compactLabel.empty())
                {
                    batch.secCompact.Int32(file.fileId).Int32(lineNo).Int16(seg).Str(ToUtf8(facts.compactLabel)).EndRow();
                }
                if (facts.planContent || !facts.parentLink.empty())
                {
                    batch.secUser.Int32(file.fileId).Int32(lineNo).Bool(facts.planContent).StrOrNull(ToUtf8(facts.parentLink)).EndRow();
                }
            }

            // --- search blocks (every block of every line; copies included, as today's search lists forks) ---
            size_t resultIndex = 0;
            for (const auto& b : facts.blocks)
            {
                std::wstring tool = b.tool;
                if (b.role == kRoleToolResult && resultIndex < resultTools.size())
                {
                    tool = resultTools[resultIndex++];
                }
                const uint32_t docId = _nextDocId++;
                const std::string text = ToUtf8(b.text);
                const std::string fold8 = ToUtf8(FoldForIndex(b.text));
                batch.doc.UInt32(docId).Int32(file.sidKey).Int32(file.fileId).Int32(lineNo).UInt16(b.block).Int64(facts.tsMs)
                    .UInt8(b.role).UInt8(b.flags).StrOrNull(ToUtf8(tool)).StrOrNull(ToUtf8(b.toolUseId))
                    .Int32(static_cast<int32_t>(b.text.size())).Int32(0).Str(text).Str(fold8).EndRow();
                ++batch.hotDocs;
                batch.hotBytes += static_cast<int64_t>(text.size() + fold8.size());
                // The typed-prompt tier: real prompts (not meta / noise / sidechain / subagent) and queued messages.
                const bool realPrompt = b.role == kRolePrompt && (b.flags & (kDfMeta | kDfNoise | kDfSidechain | kDfSubagent)) == 0;
                if (realPrompt || b.role == kRoleQueued)
                {
                    batch.prompt.UInt32(docId).Int32(file.sidKey).Int64(facts.tsMs).UInt8(b.role).Str(fold8).EndRow();
                }
            }

            // --- paths (top-level and subagent files: "who touched" includes a session's subagents) ---
            for (const auto& pr : facts.paths)
            {
                const std::wstring path = CollapseSeparators(pr.path);
                const std::wstring key = PathKey(path);
                auto& agg = batch.paths[{ file.sidKey, key, pr.op }];
                if (agg.n == 0)
                {
                    agg.path = path;
                    agg.firstMs = facts.tsMs;
                    agg.firstLine = top ? lineNo : 0x7FFFFFFF; // subagent paths never lead the inferred-folder order
                }
                ++agg.n;
                agg.lastMs = std::max(agg.lastMs, facts.tsMs);
            }

            // --- usage (one row per API message; a later line of the message carries its final usage) ---
            if (facts.usage)
            {
                batch.usage[{ file.sidKey, facts.usage->messageId }] = { facts.tsMs, *facts.usage };
            }
        };

        // Read from the cursor to the last complete line, bounded per batch. `carry` holds the bytes of a
        // line that has not ended within the bytes read so far.
        constexpr size_t kChunk = 8u * 1024u * 1024u;
        constexpr size_t kMaxLine = 128u * 1024u * 1024u; // a longer line is stored cut (§7.5: the largest seen is 18.5 MB)
        const int64_t limit = std::min<int64_t>(df.size, file.parsedBytes + kMaxBytesPerFilePerBatch);
        int64_t committed = file.parsedBytes; // the cursor: bytes of COMPLETE lines processed
        int64_t readPos = file.parsedBytes;
        std::string carry;
        while (readPos < df.size)
        {
            if (readPos >= limit && carry.empty())
            {
                break; // this batch's share of the file is done; the next request continues from the cursor
            }
            std::string chunk = fh.ReadAt(readPos, kChunk);
            if (chunk.empty())
            {
                break;
            }
            readPos += static_cast<int64_t>(chunk.size());
            std::string buf;
            std::string_view data;
            if (carry.empty())
            {
                data = chunk;
            }
            else
            {
                buf = std::move(carry);
                buf += chunk;
                carry.clear();
                data = buf;
            }
            size_t start = 0;
            for (size_t i = 0; i < data.size(); ++i)
            {
                if (data[i] != '\n')
                {
                    continue;
                }
                processLine(data.substr(start, i - start), false);
                committed += static_cast<int64_t>(i - start + 1);
                start = i + 1;
            }
            if (start < data.size())
            {
                if (data.size() - start > kMaxLine)
                {
                    // A line past the cap: skip to its end (when it has one), keeping only its first
                    // kMaxLine bytes, stored as bytes and never classified.
                    const std::string prefix{ data.substr(start, kMaxLine) };
                    int64_t scan = readPos;
                    bool ended = false;
                    while (scan < df.size)
                    {
                        const std::string more = fh.ReadAt(scan, kChunk);
                        if (more.empty())
                        {
                            break;
                        }
                        const size_t nl = more.find('\n');
                        if (nl != std::string::npos)
                        {
                            scan += static_cast<int64_t>(nl + 1);
                            ended = true;
                            break;
                        }
                        scan += static_cast<int64_t>(more.size());
                    }
                    if (!ended)
                    {
                        break; // still being written: leave it for a later tick
                    }
                    _log(L"[sessdb] " + ShortIdOf(file.sidKey) + L": a line of " + std::to_wstring(scan - committed) + L" bytes in " + df.path +
                         L" is stored cut at " + std::to_wstring(kMaxLine) + L" bytes");
                    processLine(prefix, true);
                    committed = scan;
                    readPos = scan;
                }
                else
                {
                    carry.assign(data.substr(start)); // an unfinished line: read on
                }
            }
            if (carry.empty() && committed >= limit)
            {
                break;
            }
        }
        // A trailing partial line (no '\n' yet) is never consumed (§7.5): the cursor stops before it.
        batch.bytes += committed - file.parsedBytes;
        file.parsedBytes = committed;
        batch.lines += linesIn;
        batch.pendingFiles.push_back(file);
        return linesIn;
    }

    std::vector<std::pair<int64_t, int64_t>> Store::_ingestSessions(const std::vector<std::pair<DiskFile, std::vector<DiskFile>>>& work)
    {
        std::vector<std::pair<int64_t, int64_t>> out;
        out.reserve(work.size());
        const uint32_t docIdBefore = _nextDocId;
        std::vector<int32_t> topFiles; // the working sets this batch advances (evicted if it rolls back)
        try
        {
            Transaction tx{ *_w };
            Batch batch{ *_w };
            batch.firstDocId = _nextDocId;
            const int64_t ingestStart = TickMs();
            for (const auto& [topDf, sides] : work)
            {
                const int32_t sidKey = _sessionKeyFor(topDf.sessionId, true);
                FileRow& top = _fileFor(topDf, sidKey);
                if (top.sidKey == 0)
                {
                    top.sidKey = sidKey; // the key is only allocated here; nothing reads it before the commit
                }
                {
                    std::unique_lock lk{ _mapMtx };
                    if (auto s = _sessions.find(sidKey); s != _sessions.end())
                    {
                        s->second.fileId = top.fileId;
                    }
                }
                topFiles.push_back(top.fileId);
                FileWork& fw = _workFor(*_w, top);
                bool topReplaced = false;
                int64_t lines = _ingestFile(batch, top, topDf, &fw, false, &topReplaced);
                // A replaced top-level transcript rebuilds the whole session: the session-level rows
                // (paths, usage) were just cleared, so its side files are re-read from 0 too.
                for (const auto& side : sides)
                {
                    FileRow& sf = _fileFor(side, sidKey);
                    lines += _ingestFile(batch, sf, side, nullptr, topReplaced, nullptr);
                }
                // A session whose files gave no new line keeps its derived rows: the live-branch walk, the
                // summary and the session row read the WHOLE session (a 5,000-line transcript costs ~300 ms),
                // and nothing they read changed. Its cursors are still saved (size / time may have moved).
                if (lines > 0 || topReplaced)
                {
                    batch.tops.emplace_back(top.fileId, sidKey);
                    batch.fileOfSid[sidKey] = top.fileId;
                }
                out.emplace_back(0, lines);
            }
            _times.ingest += TickMs() - ingestStart;
            _flushBatch(batch);
            const int64_t commitStart = TickMs();
            tx.Commit();
            _times.commit += TickMs() - commitStart;
            // Committed: the cursors and counters in memory follow.
            {
                std::unique_lock lk{ _mapMtx };
                for (const auto& f : batch.pendingFiles)
                {
                    _files[f.fileId] = f;
                }
                _shortIdDirty.clear();
            }
            for (size_t i = 0; i < work.size(); ++i)
            {
                if (const FileRow* f = _fileByPath(work[i].first.path))
                {
                    out[i].first = f->parsedBytes;
                }
            }
            _hotDocsEstimate += batch.hotDocs;
            _hotBytesEstimate += batch.hotBytes;
            _lastWriteMs = NowMs();
            return out;
        }
        catch (...)
        {
            // Rolled back (the Transaction's destructor ran): nothing of the batch is in the database, so the
            // working sets it advanced are dropped (they reload from the committed state on the next use) and
            // the doc ids it handed out are free again. The caller logs the failure.
            for (const int32_t id : topFiles)
            {
                _work.erase(id);
            }
            _nextDocId = docIdBefore;
            throw;
        }
    }

    void Store::_flushBatch(Batch& batch)
    {
        Connection& con = batch.con;
        int64_t t0 = TickMs();
        batch.CloseAppenders();
        _times.appenders += TickMs() - t0;
        t0 = TickMs();

        // path_access: one upsert of the batch's aggregates (unique keys by construction).
        if (!batch.paths.empty())
        {
            con.Execute("DELETE FROM stg_path");
            {
                auto a = con.MakeAppender("stg_path");
                for (const auto& [key, agg] : batch.paths)
                {
                    const auto& [sid, pathKey, op] = key;
                    const std::wstring dir = agg.path.substr(0, agg.path.find_last_of(L"\\/") == std::wstring::npos ? 0 : agg.path.find_last_of(L"\\/"));
                    const std::wstring leaf = agg.path.find_last_of(L"\\/") == std::wstring::npos ? agg.path : agg.path.substr(agg.path.find_last_of(L"\\/") + 1);
                    a.Int32(sid).Str(ToUtf8(agg.path)).Str(ToUtf8(pathKey)).Str(ToUtf8(DirFold(dir))).Str(ToUtf8(DirFold(leaf))).UInt8(op)
                        .Int32(static_cast<int32_t>(agg.n)).Int64(agg.firstMs).Int64(agg.lastMs).Int32(agg.firstLine).EndRow();
                }
                a.Close();
            }
            con.Execute(R"SQL(
INSERT INTO path_access SELECT * FROM stg_path
ON CONFLICT (sid_key, path_key, op) DO UPDATE SET
    n = path_access.n + excluded.n,
    first_ms = CASE WHEN coalesce(path_access.first_ms, 0) = 0 THEN excluded.first_ms
                    WHEN coalesce(excluded.first_ms, 0) = 0 THEN path_access.first_ms
                    ELSE least(path_access.first_ms, excluded.first_ms) END,
    last_ms = greatest(path_access.last_ms, excluded.last_ms),
    first_line = least(path_access.first_line, excluded.first_line);
DELETE FROM stg_path;
)SQL");
        }

        _times.paths += TickMs() - t0;
        t0 = TickMs();
        // usage: replace the batch's API messages (a message split across two ticks keeps its last line).
        if (!batch.usage.empty())
        {
            con.Execute("DELETE FROM stg_usage");
            {
                auto a = con.MakeAppender("stg_usage");
                for (const auto& [key, v] : batch.usage)
                {
                    const auto& u = v.second;
                    a.Int32(key.first).Str(ToUtf8(key.second)).Int64(v.first).Str(ToUtf8(u.model)).Int64(u.inputTokens).Int64(u.outputTokens)
                        .Int64(u.cacheReadTokens).Int64(u.cacheWriteTokens).Int32(u.webSearches).Int32(u.webFetches).EndRow();
                }
                a.Close();
            }
            con.Execute(R"SQL(
DELETE FROM usage WHERE (sid_key, message_id) IN (SELECT sid_key, message_id FROM stg_usage);
INSERT INTO usage SELECT * FROM stg_usage;
DELETE FROM stg_usage;
)SQL");
        }

        _times.usage += TickMs() - t0;
        // Neighbors whose short id grew because a new session shares their first characters.
        std::vector<std::pair<int32_t, std::wstring>> shortIds;
        {
            std::shared_lock lk{ _mapMtx };
            for (const int32_t sid : _shortIdDirty)
            {
                if (const auto s = _sessions.find(sid); s != _sessions.end())
                {
                    shortIds.emplace_back(sid, s->second.shortId);
                }
            }
        }
        if (!shortIds.empty())
        {
            auto p = con.Prepare("UPDATE session SET short_id = ? WHERE sid_key = ?");
            for (const auto& [sid, shortId] : shortIds)
            {
                p.Bind(1, std::string_view{ ToUtf8(shortId) }).Bind(2, static_cast<int64_t>(sid));
                p.Execute();
            }
        }

        if (batch.tops.empty())
        {
            return;
        }
        std::vector<int32_t> fileIds;
        fileIds.reserve(batch.tops.size());
        for (const auto& [fid, sid] : batch.tops)
        {
            fileIds.push_back(fid);
        }
        t0 = TickMs();
        _updateLiveBranch(con, fileIds);
        _times.walk += TickMs() - t0;
        t0 = TickMs();
        _buildSummaries(con, batch.tops);
        _times.summary += TickMs() - t0;
        t0 = TickMs();
        for (const auto& [fid, sid] : batch.tops)
        {
            const FileRow* top = nullptr;
            for (const auto& f : batch.pendingFiles)
            {
                if (f.fileId == fid)
                {
                    top = &f;
                    break;
                }
            }
            if (!top)
            {
                continue;
            }
            const auto w = _work.find(fid);
            const SessionFold empty{};
            _writeSessionRow(con, sid, *top, w != _work.end() ? w->second.fold : empty);
        }
        _times.sessionRows += TickMs() - t0;
        t0 = TickMs();
        for (const auto& f : batch.pendingFiles)
        {
            const SessionFold* fold = nullptr;
            if (f.kind == 0)
            {
                if (const auto w = _work.find(f.fileId); w != _work.end())
                {
                    fold = &w->second.fold;
                }
            }
            _saveCursor(con, f, fold);
        }
        _flushStaged(con);
        _times.cursors += TickMs() - t0;
    }

    void Store::_flushStaged(Connection& con)
    {
        // The staged session rows and cursors, written in one statement each (see the staging note in Open).
        con.Execute(R"SQL(
INSERT OR REPLACE INTO session SELECT * FROM stg_session; DELETE FROM stg_session;
INSERT OR REPLACE INTO src_file SELECT * FROM stg_src; DELETE FROM stg_src;
)SQL");
    }

    void Store::_saveCursor(Connection& con, const FileRow& f, const SessionFold* fold)
    {
        if (!_cursorUpsert)
        {
            _cursorUpsert = std::make_unique<Prepared>(
                con.Prepare("INSERT INTO stg_src (file_id, path, kind, sid_key, agent_id, size_bytes, mtime_ms, birth_ms, parsed_bytes, "
                            "line_count, head_hash, seg_count, missing_since, fold, ingested_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        }
        Prepared& p = *_cursorUpsert;
        p.ClearBindings();
        p.Bind(1, static_cast<int64_t>(f.fileId));
        p.Bind(2, std::string_view{ ToUtf8(f.path) });
        p.Bind(3, static_cast<int64_t>(f.kind));
        if (f.sidKey)
        {
            p.Bind(4, static_cast<int64_t>(f.sidKey));
        }
        else
        {
            p.BindNull(4);
        }
        if (f.agentId.empty())
        {
            p.BindNull(5);
        }
        else
        {
            p.Bind(5, std::string_view{ ToUtf8(f.agentId) });
        }
        p.Bind(6, f.sizeBytes).Bind(7, f.mtimeMs).Bind(8, f.birthMs).Bind(9, f.parsedBytes);
        p.Bind(10, static_cast<int64_t>(f.lineCount)).Bind(11, f.headHash).Bind(12, static_cast<int64_t>(f.segCount));
        if (f.missingSince)
        {
            p.Bind(13, f.missingSince);
        }
        else
        {
            p.BindNull(13);
        }
        if (fold)
        {
            const std::string j = FoldToJson(*fold);
            p.Bind(14, std::string_view{ j });
        }
        else
        {
            p.BindNull(14);
        }
        p.Bind(15, NowMs());
        p.Execute();
    }

    void Store::_updateLiveBranch(Connection& con, const std::vector<int32_t>& fileIds)
    {
        std::vector<int64_t> ids(fileIds.begin(), fileIds.end());
        // The rows the walk reads, every file of the batch in one query (file order inside each file).
        std::unordered_map<int32_t, std::vector<WalkRow>> rows;
        std::unordered_map<int32_t, std::vector<std::pair<int32_t, bool>>> stored; // file -> (line, on_branch)
        {
            auto p = con.Prepare(R"SQL(
SELECT file_id, line_no, coalesce(uuid, ''), coalesce(parent_uuid, ''), coalesce(leaf_uuid, ''), flags, on_branch FROM (
    SELECT file_id, line_no, uuid, parent_uuid, leaf_uuid, flags, on_branch FROM line WHERE file_id IN (SELECT unnest(?))
    UNION ALL
    SELECT file_id, line_no, uuid, parent_uuid, leaf_uuid, flags, on_branch FROM line_hot WHERE file_id IN (SELECT unnest(?)))
ORDER BY file_id, line_no)SQL");
            p.BindInt64List(1, ids).BindInt64List(2, ids);
            auto r = p.Execute();
            while (r.Next())
            {
                const int32_t fid = static_cast<int32_t>(r.Int64(0));
                WalkRow w;
                w.lineNo = static_cast<int32_t>(r.Int64(1));
                w.uuid = FromUtf8(r.Str(2));
                w.parent = FromUtf8(r.Str(3));
                w.leaf = FromUtf8(r.Str(4));
                const uint8_t flags = static_cast<uint8_t>(r.Int64(5));
                w.sidechain = (flags & kLfSidechain) != 0;
                w.compactRoot = (flags & kLfCompactRoot) != 0;
                stored[fid].emplace_back(w.lineNo, r.Bool(6));
                rows[fid].push_back(std::move(w));
            }
        }
        std::unordered_map<int32_t, std::unordered_map<int32_t, std::vector<std::wstring>>> edges;
        {
            auto p = con.Prepare("SELECT file_id, line_no, target FROM line_edge WHERE file_id IN (SELECT unnest(?))");
            p.BindInt64List(1, ids);
            auto r = p.Execute();
            while (r.Next())
            {
                edges[static_cast<int32_t>(r.Int64(0))][static_cast<int32_t>(r.Int64(1))].push_back(FromUtf8(r.Str(2)));
            }
        }
        std::vector<int64_t> setTrue, setFalse;
        for (const int32_t fid : fileIds)
        {
            const auto& fr = rows[fid];
            const auto active = ActiveBranchFromRows(fr, edges[fid]);
            const auto& st = stored[fid];
            for (size_t i = 0; i < fr.size(); ++i)
            {
                const bool want = active.empty() || fr[i].uuid.empty() || active.count(fr[i].uuid) != 0;
                if (want != st[i].second)
                {
                    const int64_t lineId = (static_cast<int64_t>(fid) << 32) | static_cast<uint32_t>(fr[i].lineNo);
                    (want ? setTrue : setFalse).push_back(lineId);
                }
            }
        }
        for (const auto& [list, value] : { std::pair<const std::vector<int64_t>*, const char*>{ &setTrue, "true" },
                                           std::pair<const std::vector<int64_t>*, const char*>{ &setFalse, "false" } })
        {
            if (list->empty())
            {
                continue;
            }
            for (const char* table : { "line", "line_hot" })
            {
                auto p = con.Prepare(std::string("UPDATE ") + table + " SET on_branch = " + value + " WHERE line_id IN (SELECT unnest(?))");
                p.BindInt64List(1, *list);
                p.Execute();
            }
        }
    }

    void Store::_buildSummaries(Connection& con, const std::vector<std::pair<int32_t, int32_t>>& fileSids)
    {
        std::vector<int64_t> files, sids;
        for (const auto& [f, s] : fileSids)
        {
            files.push_back(f);
            sids.push_back(s);
        }
        auto p = con.Prepare(SummaryBuildSql());
        p.BindInt64List(1, files).BindInt64List(2, sids).Bind(3, NowMs());
        p.Execute();
    }

    bool Store::_probeable(const std::wstring& dir)
    {
        // A drive-letter path only ("X:\..." or "X:/..."): never a UNC or a relative path (no network I/O).
        if (dir.size() < 3 || dir[1] != L':' || (dir[2] != L'\\' && dir[2] != L'/'))
        {
            return false;
        }
        const wchar_t letter = static_cast<wchar_t>(std::towupper(dir[0]));
        if (letter < L'A' || letter > L'Z')
        {
            return false;
        }
        const int i = letter - L'A';
        const int64_t now = NowMs();
        if (_driveOk[i] == 0 || now - _driveCheckedMs[i] > 5 * 60 * 1000)
        {
            const wchar_t root[4] = { letter, L':', L'\\', 0 };
            const UINT type = ::GetDriveTypeW(root);
            const bool local = type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
            _driveOk[i] = (local && ::GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES) ? 1 : -1;
            _driveCheckedMs[i] = now;
        }
        return _driveOk[i] == 1;
    }

    std::wstring Store::_gitRootOf(const std::wstring& dir)
    {
        const std::wstring key = NormDirKey(dir);
        if (const auto it = _gitRootCache.find(key); it != _gitRootCache.end())
        {
            return it->second;
        }
        std::wstring root = _probeable(dir) ? FindGitRootForDir(dir) : std::wstring{};
        if (_gitRootCache.size() > 100000)
        {
            _gitRootCache.clear(); // bounded: a repository that appeared or vanished is re-probed after a reset anyway
        }
        _gitRootCache.emplace(key, root);
        return root;
    }

    void Store::_writeSessionRow(Connection& con, int32_t sidKey, const FileRow& top, const SessionFold& fold)
    {
        const int64_t tw0 = TickMs();
        // Usage sums (deduplicated per API message by the usage table).
        int64_t apiMessages = 0, inTok = 0, outTok = 0, cacheRead = 0, cacheWrite = 0, webSearch = 0, webFetch = 0;
        {
            auto p = con.Prepare("SELECT count(*), coalesce(sum(in_tok), 0), coalesce(sum(out_tok), 0), coalesce(sum(cache_read), 0), "
                                 "coalesce(sum(cache_write), 0), coalesce(sum(web_search), 0), coalesce(sum(web_fetch), 0) FROM usage WHERE sid_key = ?");
            p.Bind(1, static_cast<int64_t>(sidKey));
            auto r = p.Execute();
            if (r.Next())
            {
                apiMessages = r.Int64(0);
                inTok = r.Int64(1);
                outTok = r.Int64(2);
                cacheRead = r.Int64(3);
                cacheWrite = r.Int64(4);
                webSearch = r.Int64(5);
                webFetch = r.Int64(6);
            }
        }
        const int64_t tw1 = TickMs();
        // The live-branch facts the walk left (the leaf, the abandoned lines).
        int64_t offBranch = 0;
        std::wstring leaf;
        {
            auto p = con.Prepare(R"SQL(
SELECT count(*) FILTER (WHERE NOT on_branch AND uuid IS NOT NULL AND (flags & 16) = 0),
       arg_max(leaf_uuid, line_no) FILTER (WHERE leaf_uuid IS NOT NULL)
FROM (SELECT line_no, uuid, leaf_uuid, flags, on_branch FROM line WHERE file_id = ?
      UNION ALL SELECT line_no, uuid, leaf_uuid, flags, on_branch FROM line_hot WHERE file_id = ?))SQL");
            p.Bind(1, static_cast<int64_t>(top.fileId)).Bind(2, static_cast<int64_t>(top.fileId));
            auto r = p.Execute();
            if (r.Next())
            {
                offBranch = r.Int64(0);
                leaf = FromUtf8(r.Str(1));
            }
        }
        const int64_t tw2 = TickMs();
        // The filesystem columns (D15): the git root, the worktree's main root (the color key), the
        // inferred working directory (InferWorkingDirectory over the session's tool-touched paths).
        const std::wstring cwd = fold.cwdFirst;
        const std::wstring cwdKey = cwd.empty() ? std::wstring{} : NormDirKey(cwd);
        const std::wstring gitRoot = cwdKey.empty() ? std::wstring{} : _gitRootOf(cwd);
        std::wstring colorKey = cwdKey;
        if (!cwd.empty() && _probeable(cwd))
        {
            if (const std::wstring mainRoot = ResolveWorktreeMainRoot(cwd); !mainRoot.empty())
            {
                colorKey = NormDirKey(mainRoot);
            }
        }
        const int64_t tw3 = TickMs();
        std::wstring inferred;
        size_t inferPaths = 0;
        {
            std::vector<std::wstring> paths;
            auto p = con.Prepare("SELECT any_value(path) FROM path_access WHERE sid_key = ? GROUP BY path_key ORDER BY min(first_line), min(first_ms) LIMIT ?");
            p.Bind(1, static_cast<int64_t>(sidKey)).Bind(2, static_cast<int64_t>(kMaxInferPaths));
            auto r = p.Execute();
            while (r.Next())
            {
                paths.push_back(FromUtf8(r.Str(0)));
            }
            inferPaths = paths.size();
            if (!paths.empty())
            {
                ExcludePathsUnderRoots(paths, _tempRoots);
                std::function<std::wstring(const std::wstring&)> gitRootOf;
                if (_cfg.inferGitRoot)
                {
                    gitRootOf = [this](const std::wstring& dir) { return _gitRootOf(dir); };
                }
                const std::wstring picked = InferWorkingDirectory(paths, cwd, gitRootOf);
                if (!picked.empty() && NormDirKey(picked) != cwdKey)
                {
                    inferred = picked; // stored only when it differs from the cwd (SessionInfo::inferredWorkingDir's rule)
                }
            }
        }
        const int64_t tw4 = TickMs();
        // Subagent transcripts of this session.
        int64_t subagentFiles = 0;
        {
            std::shared_lock lk{ _mapMtx };
            for (const auto& [id, f] : _files)
            {
                if (f.kind == 1 && f.sidKey == sidKey)
                {
                    ++subagentFiles;
                }
            }
        }
        // created: a fork's file birth (its copied lines carry the parent's old stamps), else the first stamp.
        const int64_t created = !fold.forkedFrom.empty() && top.birthMs ? top.birthMs : (fold.firstTsMs ? fold.firstTsMs : top.birthMs);
        std::wstring projectDir;
        if (const size_t slash = top.path.find_last_of(L"\\/"); slash != std::wstring::npos)
        {
            const std::wstring parent = top.path.substr(0, slash);
            projectDir = parent.substr(parent.find_last_of(L"\\/") + 1);
        }
        std::string byTool = "{";
        bool firstTool = true;
        for (const auto& [name, n] : fold.toolCallsByTool)
        {
            if (!firstTool)
            {
                byTool += ',';
            }
            firstTool = false;
            byTool += JsonArray({ name }).substr(1);
            byTool.pop_back();
            byTool += ':' + std::to_string(n);
        }
        byTool += '}';

        const std::wstring sessionId = SessionIdOf(sidKey);
        const std::wstring shortId = ShortIdOf(sidKey);
        const std::vector<Val> v = {
            static_cast<int64_t>(sidKey), ToUtf8(sessionId), ToUtf8(shortId), static_cast<int64_t>(top.fileId), TextOrNull(projectDir),
            std::string{ "claude" }, TextOrNull(fold.entrypoint),
            top.sizeBytes, NumOrNull(top.birthMs), fold.lines, int64_t{ 0 },
            TextOrNull(cwd), TextOrNull(cwdKey), TextOrNull(cwd.empty() ? std::wstring{} : DirFold(cwd)),
            TextOrNull(fold.cwdLast), TextOrNull(fold.cwdLast.empty() ? std::wstring{} : NormDirKey(fold.cwdLast)),
            static_cast<int64_t>(fold.cwdKeys.size()),
            TextOrNull(fold.relocatedCwd), TextOrNull(fold.relocatedCwd.empty() ? std::wstring{} : NormDirKey(fold.relocatedCwd)),
            TextOrNull(fold.worktreePath), TextOrNull(fold.worktreePath.empty() ? std::wstring{} : NormDirKey(fold.worktreePath)),
            TextOrNull(fold.worktreeName), TextOrNull(fold.worktreeBranch), TextOrNull(fold.worktreeOriginalCwd),
            fold.env ? Val{ fold.envIsGitRepo } : Val{}, fold.env ? Val{ fold.envIsWorktree } : Val{}, TextOrNull(fold.envShell),
            fold.env ? Val{ static_cast<int64_t>(fold.envExtraDirs) } : Val{},
            TextOrNull(gitRoot), TextOrNull(gitRoot.empty() ? std::wstring{} : NormDirKey(gitRoot)), TextOrNull(colorKey),
            TextOrNull(inferred), TextOrNull(inferred.empty() ? std::wstring{} : NormDirKey(inferred)), _cfg.inferGitRoot,
            !cwdKey.empty() && cwdKey == _homeKey, NowMs(),
            TextOrNull(fold.branchFirst), TextOrNull(fold.branchLast), static_cast<int64_t>(fold.branches.size()),
            TextOrNull(fold.versionFirst), TextOrNull(fold.versionLast),
            fold.models.empty() ? Val{} : Val{ JsonArray(fold.models) }, TextOrNull(fold.modelLast), TextOrNull(fold.effortLast),
            TextOrNull(fold.permissionModeLast), static_cast<int64_t>(fold.planModeMarks), TextOrNull(fold.agentName),
            NumOrNull(created), NumOrNull(fold.firstTsMs), NumOrNull(fold.lastActivityMs), NumOrNull(fold.lastTypedMs), NumOrNull(fold.lastTurnEndMs),
            static_cast<int64_t>(fold.turnsTimed), fold.turnMsTotal, fold.turnMsMax,
            fold.costState ? Val{ fold.costUsd } : Val{}, fold.costState ? Val{ fold.apiMs } : Val{}, fold.costState ? Val{ fold.apiMsNoRetries } : Val{},
            fold.costState ? Val{ fold.toolMs } : Val{}, fold.costState ? Val{ fold.wallMs } : Val{},
            fold.costState ? Val{ fold.linesAdded } : Val{}, fold.costState ? Val{ fold.linesRemoved } : Val{},
            fold.costState ? Val{ fold.costUnknownModel } : Val{}, fold.costState,
            apiMessages, inTok, outTok, cacheRead, cacheWrite, webSearch, webFetch, fold.contextMax, fold.contextTokens,
            static_cast<int64_t>(fold.apiErrors), TextOrNull(fold.apiErrorStatusLast), static_cast<int64_t>(fold.refusals),
            static_cast<int64_t>(fold.maxTokenStops), static_cast<int64_t>(fold.aborted),
            static_cast<int64_t>(fold.interrupts), TextOrNull(EndState(fold)), fold.stopHookMsTotal, fold.stopHookMsMax,
            static_cast<int64_t>(fold.promptsTyped), static_cast<int64_t>(fold.promptsQueued), static_cast<int64_t>(fold.assistantLines),
            static_cast<int64_t>(fold.toolCalls),
            fold.toolCallsByTool.empty() ? Val{} : Val{ byTool }, fold.skillsUsed.empty() ? Val{} : Val{ JsonArray(fold.skillsUsed) },
            fold.mcpServers.empty() ? Val{} : Val{ JsonArray(fold.mcpServers) },
            static_cast<int64_t>(fold.subagentsSpawned), subagentFiles, static_cast<int64_t>(fold.imagesPasted),
            static_cast<int64_t>(fold.compactions), static_cast<int64_t>(fold.compactionsAuto), fold.compactionPreTokensMax,
            static_cast<int64_t>(fold.recaps), TextOrNull(fold.recapLast),
            static_cast<int64_t>(fold.artifacts), TextOrNull(fold.artifactTitle), TextOrNull(fold.artifactUrl),
            TextOrNull(fold.customTitle), TextOrNull(fold.aiTitle), TextOrNull(fold.legacySummary), TextOrNull(fold.firstPrompt),
            TextOrNull(fold.lastPrompt), TextOrNull(fold.lastReply),
            TextOrNull(fold.forkedFrom), NumOrNull(fold.forkPointLine), TextOrNull(leaf), offBranch,
            NowMs(),
        };
        static_assert(kSessionColumnCount == 111, "the session column list and the value list must stay in step");
        if (v.size() != kSessionColumnCount)
        {
            throw DbError("session row: " + std::to_string(v.size()) + " values for " + std::to_string(kSessionColumnCount) + " columns");
        }
        if (!_sessionUpsert)
        {
            _sessionUpsert = std::make_unique<Prepared>(con.Prepare(SessionUpsertSql()));
        }
        Prepared& p = *_sessionUpsert;
        p.ClearBindings();
        BindAll(p, v);
        p.Execute();
        const int64_t tw5 = TickMs();
        if (tw5 - tw0 > 200)
        {
            _log(L"[sessdb] slow session row " + shortId + L": usage " + std::to_wstring(tw1 - tw0) + L", branch facts " + std::to_wstring(tw2 - tw1) +
                 L", git/color " + std::to_wstring(tw3 - tw2) + L", inferred (" + std::to_wstring(inferPaths) + L" paths) " + std::to_wstring(tw4 - tw3) +
                 L", upsert " + std::to_wstring(tw5 - tw4) + L" ms");
        }
    }

    // ===== Agentmaster's facts, the open set, history ================================================

    void Store::RefreshFacts()
    {
        const auto titles = LoadAllStoredSessionTitles();
        const auto favorites = LoadAllFavoriteSessions();
        const auto tags = LoadAllSessionTags();
        std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> links;
        try
        {
            for (const auto& s : LoadSessions())
            {
                if (!s.continuedFromId.empty())
                {
                    links.emplace_back(s.id, s.continuedFromId, s.continuedVia);
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"sessiondb RefreshFacts (sessions.json)");
        }
        Transaction tx{ *_w };
        _w->Execute("DELETE FROM am_title; DELETE FROM am_favorite; DELETE FROM am_tag; DELETE FROM am_link;");
        {
            auto a = _w->MakeAppender("am_title");
            for (const auto& [id, title] : titles)
            {
                if (!title.empty())
                {
                    a.Str(ToUtf8(LowerAscii(id))).Str(ToUtf8(title)).EndRow();
                }
            }
            a.Close();
        }
        {
            auto a = _w->MakeAppender("am_favorite");
            for (const auto& id : favorites)
            {
                a.Str(ToUtf8(LowerAscii(id))).EndRow();
            }
            a.Close();
        }
        {
            auto a = _w->MakeAppender("am_tag");
            for (const auto& [id, list] : tags)
            {
                for (const auto& tag : list)
                {
                    std::wstring fold = tag;
                    if (!fold.empty())
                    {
                        ::CharLowerBuffW(fold.data(), static_cast<DWORD>(fold.size()));
                    }
                    a.Str(ToUtf8(LowerAscii(id))).Str(ToUtf8(tag)).Str(ToUtf8(fold)).EndRow();
                }
            }
            a.Close();
        }
        {
            auto a = _w->MakeAppender("am_link");
            for (const auto& [id, from, via] : links)
            {
                a.Str(ToUtf8(LowerAscii(id))).Str(ToUtf8(LowerAscii(from))).StrOrNull(ToUtf8(via)).EndRow();
            }
            a.Close();
        }
        tx.Commit();
    }

    void Store::SetOpenSessions(const std::vector<std::wstring>& ids)
    {
        std::unordered_set<std::wstring> set;
        for (const auto& id : ids)
        {
            set.insert(LowerAscii(id));
        }
        Transaction tx{ *_w };
        _w->Execute("DELETE FROM rt_open");
        auto a = _w->MakeAppender("rt_open");
        for (const auto& id : set)
        {
            a.Str(ToUtf8(id)).EndRow();
        }
        a.Close();
        tx.Commit();
        std::unique_lock lk{ _mapMtx };
        _open = std::move(set);
    }

    void Store::IngestHistory()
    {
        if (_cfg.historyPath.empty())
        {
            return;
        }
        DiskFile df;
        if (!_statFile(_cfg.historyPath, 4, {}, {}, df))
        {
            return;
        }
        FileRow& f = _fileFor(df, 0);
        if (f.sizeBytes == df.size && f.mtimeMs == df.mtimeMs && f.parsedBytes != 0)
        {
            return; // unchanged since the last read: one stat
        }
        FileRow row = f;
        row.kind = 4;
        File fh{ df.path };
        if (!fh.Ok())
        {
            return;
        }
        Transaction tx{ *_w };
        if (df.size < row.parsedBytes)
        {
            _w->Execute("DELETE FROM history_prompt"); // rewritten (a manual cleanup): read it again from 0
            row.parsedBytes = 0;
            row.lineCount = 0;
        }
        auto a = _w->MakeAppender("history_prompt");
        int64_t offset = row.parsedBytes;
        while (offset < df.size)
        {
            const std::string chunk = fh.ReadAt(offset, static_cast<size_t>(std::min<int64_t>(df.size - offset, 8ll * 1024 * 1024)));
            if (chunk.empty())
            {
                break;
            }
            size_t start = 0, consumed = 0;
            for (size_t i = 0; i < chunk.size(); ++i)
            {
                if (chunk[i] != '\n')
                {
                    continue;
                }
                const std::string_view raw{ chunk.data() + start, i - start };
                start = i + 1;
                consumed = start;
                ++row.lineCount;
                // {display, pastedContents, timestamp (ms), project, sessionId} — SESSIONS.md §6.4.
                const std::wstring wide = FromUtf8(raw);
                const auto parsed = json::Parse(wide);
                if (!parsed || parsed->type != json::Value::Type::Obj)
                {
                    continue;
                }
                const std::wstring display = parsed->StrAt(L"display");
                if (display.empty())
                {
                    continue;
                }
                int64_t ts = 0;
                if (const auto* t = parsed->Find(L"timestamp"); t && t->type == json::Value::Type::Num)
                {
                    ts = static_cast<int64_t>(t->num);
                }
                a.StrOrNull(ToUtf8(LowerAscii(parsed->StrAt(L"sessionId")))).Int64(ts).StrOrNull(ToUtf8(parsed->StrAt(L"project")))
                    .Str(ToUtf8(display)).Str(ToUtf8(FoldForIndex(display))).EndRow();
            }
            if (consumed == 0)
            {
                break; // a torn tail: wait for the next tick
            }
            offset += static_cast<int64_t>(consumed);
        }
        a.Close();
        row.parsedBytes = offset;
        row.sizeBytes = df.size;
        row.mtimeMs = df.mtimeMs;
        _saveCursor(*_w, row, nullptr);
        _flushStaged(*_w);
        tx.Commit();
        std::unique_lock lk{ _mapMtx };
        _files[row.fileId] = row;
    }

    // ===== index maintenance =========================================================================

    bool Store::MergeDue() const
    {
        if (_hotDocsEstimate <= 0)
        {
            return false;
        }
        return _hotDocsEstimate >= kMergeDocs || _hotBytesEstimate >= kMergeBytes || NowMs() - _lastMergeMs >= kMergeAgeMs;
    }

    size_t Store::Merge(const CancelFn& cancel)
    {
        const int64_t started = NowMs();
        bool finished = false;
        size_t n = 0;
        try
        {
            n = _index.Merge(*_w, cancel, &finished);
        }
        catch (...)
        {
            // A slice failed (rolled back); the slices before it stay merged. Re-count what is still hot so
            // MergeDue keeps asking (the estimate would otherwise still hold the whole tier), then rethrow.
            _recountHotTier();
            throw;
        }
        if (finished)
        {
            _hotDocsEstimate = 0;
            _hotBytesEstimate = 0;
        }
        else
        {
            _recountHotTier(); // a cancel left the later slices hot
        }
        _lastMergeMs = NowMs();
        if (n > 0 || !finished)
        {
            _log(L"[sessdb] merge: " + std::to_wstring(n) + L" block(s) into the index in " + std::to_wstring(NowMs() - started) + L" ms" +
                 (finished ? L"" : L" (paused for a write job; " + std::to_wstring(_hotDocsEstimate) + L" block(s) stay hot)"));
        }
        return n;
    }

    void Store::_recountHotTier() noexcept
    {
        try
        {
            _hotDocsEstimate = _w->ScalarInt64("SELECT count(*) FROM doc_hot");
            _hotBytesEstimate = _w->ScalarInt64("SELECT coalesce(sum(text_len), 0) FROM doc_hot") * 2;
        }
        catch (...)
        {
            // The estimate only paces merges: keep it non-zero so the next merge reads the table itself.
            LogSwallowedException(L"sessiondb Merge (recount the hot tier)");
            _hotDocsEstimate = std::max<int64_t>(_hotDocsEstimate, 1);
        }
    }

    void Store::Recluster(const CancelFn& cancel, bool force)
    {
        const bool postings = force || _postingsReclusterDue();
        const int64_t runs = _index.LineRuns(*_w);
        const bool lines = force || runs >= kLineRunsForResort || _index.LineResortInProgress(*_w);
        std::wstring done;
        if (postings)
        {
            const int64_t t0 = NowMs();
            if (!_index.ReclusterPostings(*_w, cancel))
            {
                _log(L"[sessdb] re-cluster: interrupted before the text index (a write job is waiting)");
                return;
            }
            SetMeta(*_w, "last_recluster_ms", std::to_string(NowMs()));
            SetMeta(*_w, "last_recluster_docs", std::to_string(_w->ScalarInt64("SELECT count(*) FROM doc")));
            done += L"text index " + std::to_wstring(NowMs() - t0) + L" ms";
        }
        if (lines)
        {
            const int64_t t0 = NowMs();
            LineResortTally tally;
            const bool finished = _index.ReclusterLines(*_w, cancel, &tally);
            done += (done.empty() ? L"" : L", ") + std::wstring{ L"line store (" } + std::to_wstring(runs) + L" run(s)" +
                    (tally.resumed ? L", resumed" : L"") + L"): " + std::to_wstring(tally.moved) + L" of " + std::to_wstring(tally.slices) +
                    L" slice(s) moved (" + std::to_wstring(tally.movedBytes / (1024 * 1024)) + L" MB), " + std::to_wstring(tally.inPlace) +
                    L" in place, " + std::to_wstring(NowMs() - t0) + L" ms" + (finished ? L"" : L"; paused for a write job, resumes when idle");
            if (finished)
            {
                SetMeta(*_w, "last_line_sort_ms", std::to_string(NowMs()));
            }
        }
        if (!done.empty())
        {
            _log(L"[sessdb] re-cluster: " + done);
        }
    }

    bool Store::_postingsReclusterDue()
    {
        const int64_t docs = _w->ScalarInt64("SELECT count(*) FROM doc");
        const int64_t lastDocs = std::strtoll(GetMeta(*_w, "last_recluster_docs").c_str(), nullptr, 10);
        const int64_t lastMs = std::strtoll(GetMeta(*_w, "last_recluster_ms").c_str(), nullptr, 10);
        const int64_t grown = docs - lastDocs;
        if (grown <= 0)
        {
            return false;
        }
        if (grown >= std::max<int64_t>(20000, lastDocs / 4))
        {
            return true;
        }
        return NowMs() - lastMs >= 24LL * 3600 * 1000;
    }

    bool Store::ReclusterDue()
    {
        return _postingsReclusterDue() || _index.LineRuns(*_w) >= kLineRunsForResort || _index.LineResortInProgress(*_w);
    }

    void Store::Checkpoint()
    {
        _w->Execute("CHECKPOINT");
    }
}
