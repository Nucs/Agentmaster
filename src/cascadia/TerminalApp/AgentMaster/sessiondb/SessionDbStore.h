// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database STORE (SESSIONS_DB.md §7): the open DuckDB file, the in-memory
// maps of files and sessions, and every operation that WRITES — the refresh tick's upserts (D16), the
// resync sweep, the cold build, Agentmaster's facts, history.jsonl, and the index maintenance.
//
// Threading contract: every write method runs on ONE thread (the service's writer thread). Readers
// (query threads) open their own Connection on Db() and use the const lookups below, which take the
// maps' shared lock. DuckDB's MVCC gives each reader a consistent snapshot while the writer commits.
//
// Atomicity (§7.6.3): one request = one or more transactions, each covering whole sessions: a session's
// new lines, its derived rows (section rows, blocks, paths, usage, the live-branch flags, the summary
// and session rows) and its file cursors commit TOGETHER. A failure rolls the batch back and the cursor
// stays where it was, so the next tick re-reads the same bytes: nothing is lost, nothing is doubled.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DuckDbWrap.h"
#include "SessionDbFold.h"
#include "SessionDbIndex.h"
#include "SessionDbSchema.h"

namespace Agentmaster::SessionDb
{
    using CancelFn = std::function<bool()>; // true = stop as soon as possible
    using LogFn = std::function<void(const std::wstring&)>;

    struct StoreConfig
    {
        std::wstring profileDir; // the Agentmaster profile (the database lives in <profile>\sessiondb)
        std::wstring projectsDir; // <claude config>\projects
        std::wstring historyPath; // <claude config>\history.jsonl
        DbOptions db; // threads, memory limit, temp folder
        bool inferGitRoot{ true }; // AppSettings::inferGitRoot (the inferred folder's git snap)
        LogFn log; // the service log ([sessdb] lines); may be empty
    };

    // A session named by a refresh tick (upsertSessions): its id and the transcript path the app's
    // watcher indexed ("" = the store resolves it from its own maps or the projects folder).
    struct SessionRef
    {
        std::wstring id;
        std::wstring path;
    };

    // The outcome of one named session.
    struct UpsertResult
    {
        std::wstring id;
        int64_t cursor{ 0 }; // the transcript's committed cursor after the request (bytes)
        int64_t lines{ 0 }; // lines ingested by this request (top-level + side files)
        bool missing{ false }; // the transcript is gone (kept, marked deleted — D7)
        bool ok{ true };
        std::wstring error; // when !ok
    };

    // A sweep's outcome.
    struct ResyncResult
    {
        int64_t filesSeen{ 0 };
        int64_t filesChanged{ 0 };
        int64_t sessionsUpserted{ 0 };
        int64_t linesIngested{ 0 };
        int64_t bytesRead{ 0 };
        int64_t ms{ 0 };
        bool cancelled{ false };
    };

    // The status answer (`status` / `hello`).
    struct StoreStatus
    {
        int64_t sessions{ 0 };
        int64_t files{ 0 };
        int64_t lines{ 0 };
        int64_t hotLines{ 0 };
        int64_t docs{ 0 };
        int64_t hotDocs{ 0 };
        int64_t terms{ 0 };
        int64_t dbBytes{ 0 };
        int64_t lastWriteMs{ 0 }; // when the last request committed (Unix ms)
        bool coldBuildDone{ false };
        int64_t coveredSinceMs{ 0 }; // while a cold build runs: the oldest session's activity indexed so far
    };

    // A resolved session.
    struct SessionKey
    {
        int32_t sidKey{ 0 };
        int32_t fileId{ 0 };
        std::wstring sessionId;
        std::wstring shortId;
    };

    class Store
    {
    public:
        explicit Store(StoreConfig config);
        ~Store();
        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;

        // Open (create) the database, check the schema, load the maps and the dictionary.
        // Returns the schema state (TooNew: the file belongs to a newer service — nothing was written and
        // the store stays closed). Throws DbError on an open failure (the caller renames a broken file
        // aside and retries, §11).
        SchemaState Open();
        // Close the file (checkpoint). Never throws.
        void Close() noexcept;
        bool IsOpen() const noexcept
        {
            return static_cast<bool>(_db);
        }
        // The database (readers connect to it). Valid while open.
        Database& Db()
        {
            return _db;
        }
        TextIndex& Index()
        {
            return _index;
        }

        // ===== writer-thread operations =====

        // The refresh tick (D16): ingest each named session from its cursors, side folders included.
        //   refs   — the sessions the app's watcher counted; a ref with an empty path is resolved here.
        //   cancel — polled between sessions; what committed stays committed.
        // Returns one result per ref (in order). Throws DbError only on a failure outside a session's
        // own transaction (the database became unusable); a failing session reports ok=false instead.
        std::vector<UpsertResult> UpsertSessions(const std::vector<SessionRef>& refs, const CancelFn& cancel);

        // The sweep (§7.6.1 belts, and the cold build on an empty database): enumerate every file under
        // the projects folder, ingest what changed against its cursor (newest sessions first), mark what
        // vanished (D7). `progress` receives (sessions done, sessions to do). `maxSessions` > 0 limits the
        // sweep to that many newest changed sessions (the one-shot diagnostic modes; the service passes 0).
        ResyncResult Resync(const CancelFn& cancel, const std::function<void(int64_t, int64_t)>& progress, size_t maxSessions = 0);

        // Mirror Agentmaster's own facts (titles, favorites, tags, the /clear and /handover links) from the
        // profile's files into am_*. Cheap (a few thousand rows); runs at open and on factsChanged.
        void RefreshFacts();
        // Replace the open-session set (rt_open) — the GUI's openChanged.
        void SetOpenSessions(const std::vector<std::wstring>& ids);
        // Ingest new lines of ~/.claude/history.jsonl (typed prompts that outlive a swept transcript).
        void IngestHistory();

        // Index maintenance: true when the hot tier is big enough (or old enough) to merge.
        bool MergeDue() const;
        // Rebuild summary rows with this build's rules (SummaryBuildSql), in batches of 256 sessions, each its own
        // transaction. The service runs it once after a complete sweep when meta `summary_version` differs from
        // kSummaryVersion (then records it); verify-summary runs it for the sessions it compares.
        //   sidKeys — the sessions; empty = every session that has a top-level transcript.
        //   cancel  — polled between batches; may be empty.
        // Returns the sessions rebuilt (fewer when cancelled). Writer thread. Throws DbError (the failing batch
        // rolled back; the batches before it stay rebuilt).
        size_t RebuildSummaries(const std::vector<int32_t>& sidKeys, const CancelFn& cancel);

        // Run a merge (see TextIndex::Merge): the hot blocks slice by slice, then the hot lines. A cancel keeps the
        // merged slices and leaves the rest hot; the MergeDue estimates are re-read then (and after a failure).
        //   cancel — polled before each slice (the service passes "a write job is waiting"); may be empty.
        // Returns the blocks merged. Writer thread. Throws DbError (the failing slice rolled back).
        size_t Merge(const CancelFn& cancel);
        // Run the parts of a re-cluster that are due (see ReclusterDue), each in its own transaction:
        //   1. the text index (TextIndex::ReclusterPostings), recording when and over how many blocks it ran
        //      (meta last_recluster_ms / last_recluster_docs), so a later failure of part 2 never re-runs it;
        //   2. the line store (TextIndex::ReclusterLines), minutes on a large store, slice by slice: a cancel keeps
        //      the slices moved so far and leaves it due, and the next idle period resumes it.
        //   cancel — polled before each part and between the line store's slices (the service passes "a write
        //            job is waiting"); may be empty.
        //   force  — run both parts whether due or not (the one-shot `recluster` mode).
        // Writer thread. Throws DbError (the failing part rolled back; the parts before it stay done).
        void Recluster(const CancelFn& cancel, bool force = false);
        // Whether a part of the re-cluster is worth its cost now (§6.4):
        //   - the text index: the blocks merged since its last re-cluster are at least a quarter of the index (and
        //     at least 20,000), or a day passed and new blocks arrived;
        //   - the line store: it holds at least kLineRunsForResort sorted runs (TextIndex::LineRuns), or a re-sort of
        //     it is under way (TextIndex::LineResortInProgress).
        // ⚠ A re-cluster rewrites big tables: DuckDB reuses the freed blocks but does not shrink the file at once,
        // so the file peaks at about twice the rewritten data around one (measured: 184 -> 343 -> 183 MB).
        // Writer thread. Throws DbError.
        bool ReclusterDue();
        // CHECKPOINT the write-ahead log into the file.
        void Checkpoint();

        // ===== reads (any thread) =====

        // Resolve a session from a short id, a longer prefix or a whole id (case-insensitive).
        //   idOrPrefix — at least 8 characters, or a whole UUID.
        //   matches    — out: how many sessions share the prefix (0 = unknown, >1 = ambiguous).
        // Returns the session when exactly one matches.
        std::optional<SessionKey> ResolveSession(std::wstring_view idOrPrefix, int& matches) const;
        // The short id of a session key ("" when unknown).
        std::wstring ShortIdOf(int32_t sidKey) const;
        // The session id of a session key ("" when unknown).
        std::wstring SessionIdOf(int32_t sidKey) const;
        // A source file's subagent id ("" for a top-level transcript, history.jsonl or an unknown id): the
        // part of a block's ref that names a subagent's transcript (`<id>/<agent>:<line>`).
        std::wstring AgentIdOfFile(int32_t fileId) const;
        // The file id of a session's subagent transcript with that agent id (0 when unknown): the reverse of
        // AgentIdOfFile, for resolving a ref.
        int32_t SubagentFileOf(int32_t sidKey, std::wstring_view agentId) const;
        // Every session key with its id (a snapshot).
        std::vector<SessionKey> AllSessions() const;
        // The open-session set the GUI last pushed (lowercase ids).
        std::unordered_set<std::wstring> OpenSessions() const;
        // A status snapshot (counts read on `con`, a reader connection). Throws DbError.
        StoreStatus Status(Connection& con) const;
        // The Unix ms of the last committed write (0 = none this run).
        int64_t LastWriteMs() const noexcept
        {
            return _lastWriteMs.load();
        }

    private:
        // One source file (src_file) in memory.
        struct FileRow
        {
            int32_t fileId{ 0 };
            std::wstring path;
            std::wstring pathKey; // lower-case path
            uint8_t kind{ 0 }; // 0 transcript, 1 subagent transcript, 2 subagent meta, 3 spill, 4 history
            int32_t sidKey{ 0 };
            std::wstring agentId;
            int64_t sizeBytes{ 0 };
            int64_t mtimeMs{ 0 };
            int64_t birthMs{ 0 };
            int64_t parsedBytes{ 0 };
            int32_t lineCount{ 0 };
            int64_t headHash{ 0 };
            int16_t segCount{ 0 };
            int64_t missingSince{ 0 };
        };
        // One session in memory.
        struct SessionRow
        {
            int32_t sidKey{ 0 };
            std::wstring sessionId; // lower-case
            int32_t fileId{ 0 }; // its top-level transcript
            std::wstring shortId;
        };
        // A file found on disk by a listing (the sweep) or a stat (a tick).
        struct DiskFile
        {
            std::wstring path;
            uint8_t kind{ 0 };
            std::wstring sessionId; // the owning session (lower-case)
            std::wstring agentId;
            int64_t size{ 0 };
            int64_t mtimeMs{ 0 };
            int64_t birthMs{ 0 };
        };
        // The working set of one top-level transcript during an ingest (kept across ticks, LRU).
        struct FileWork
        {
            SessionFold fold;
            std::unordered_set<std::wstring> uuids; // uuids seen in this file (the analyzer's copy rule)
            int64_t lastUse{ 0 };
        };
        // Everything one ingest batch appends (flushed at the end of the batch). Defined in the .cpp.
        struct Batch;

        // --- open / maps ---
        void _loadMaps(Connection& con);
        int32_t _sessionKeyFor(const std::wstring& sessionIdLower, bool create);
        void _assignShortId(const std::wstring& sessionIdLower);
        FileRow* _fileByPath(const std::wstring& path);
        FileRow& _fileFor(const DiskFile& df, int32_t sidKey);

        // --- discovery ---
        // Stat one path into a DiskFile (false: gone).
        bool _statFile(const std::wstring& path, uint8_t kind, const std::wstring& sid, const std::wstring& agentId, DiskFile& out) const;
        // The side files of one session (subagents/, tool-results/) next to its transcript.
        std::vector<DiskFile> _listSideFiles(const std::wstring& transcriptPath, const std::wstring& sid) const;
        // Every transcript + side file under the projects folder.
        std::vector<DiskFile> _listAllFiles(const CancelFn& cancel) const;
        // The transcript path of a session the maps or a folder walk can find ("" when none).
        std::wstring _resolveTranscript(const std::wstring& sid) const;

        // --- ingest ---
        // Ingest the given sessions (each: its top-level file + its side files) in ONE transaction.
        // Returns per-session (cursor, lines). Throws DbError (the transaction rolled back).
        std::vector<std::pair<int64_t, int64_t>> _ingestSessions(const std::vector<std::pair<DiskFile, std::vector<DiskFile>>>& work);
        // Read one file's new complete lines (from its cursor) and append their rows to `batch`.
        //   batch         — the open batch (its transaction is the caller's).
        //   file          — the file's in-memory row; NOT modified (the batch carries a copy with the new
        //                   cursor, applied to the maps only after COMMIT).
        //   df            — the file as found on disk (size, times).
        //   work          — the top-level transcript's working set (fold + uuids); nullptr for side files.
        //   forceFromZero — re-ingest the whole file (its session's transcript was replaced).
        //   replacedOut   — out (optional): the file had been replaced and was re-ingested from 0.
        // Returns the lines ingested. Throws DbError (an append failed; the caller's batch rolls back).
        int64_t _ingestFile(Batch& batch, FileRow& file, const DiskFile& df, FileWork* work, bool forceFromZero, bool* replacedOut);
        // Forget everything a replaced / truncated file contributed (its rows, its cursor).
        void _forgetFile(Connection& con, FileRow& file);
        FileWork& _workFor(Connection& con, FileRow& top);
        // Write the batch's rows, then the derived rows of its sessions (walk, summary, session rows).
        void _flushBatch(Batch& batch);
        void _updateLiveBranch(Connection& con, const std::vector<int32_t>& fileIds);
        void _buildSummaries(Connection& con, const std::vector<std::pair<int32_t, int32_t>>& fileSids);
        void _writeSessionRow(Connection& con, int32_t sidKey, const FileRow& top, const SessionFold& fold);
        // Stage one file's cursor (and a transcript's fold) for the batch's set-based write (_flushStaged).
        void _saveCursor(Connection& con, const FileRow& file, const SessionFold* fold);
        // Write the staged session rows and cursors (one INSERT OR REPLACE each) and empty the staging tables.
        void _flushStaged(Connection& con);

        // --- filesystem probes (D15) ---
        // May `dir` be probed (git roots, worktrees)? Only a path on a LOCAL drive that is ready: a UNC path
        // (\\server\share — or a regex fragment a shell command mined as one) would make Windows try the
        // network, which measured 10 s for ONE session's paths; a drive letter that is gone or remote
        // likewise. The per-drive verdict is cached for a few minutes.
        bool _probeable(const std::wstring& dir);
        // FindGitRootForDir, memoized per directory key and gated by _probeable ("" when not probeable).
        std::wstring _gitRootOf(const std::wstring& dir);

        // --- maintenance ---
        // The text index's part of ReclusterDue: the blocks merged since its last re-cluster are at least a
        // quarter of the index (and at least 20,000), or a day passed and new blocks arrived. Throws DbError.
        bool _postingsReclusterDue();
        // Re-read the hot tier's size into the MergeDue estimates (after a merge that stopped early or failed:
        // the blocks it did not reach are still hot). Never throws (a failed read leaves the estimate non-zero).
        void _recountHotTier() noexcept;

        // --- logging ---
        void _log(const std::wstring& line) const;

        StoreConfig _cfg;
        Database _db;
        std::unique_ptr<Connection> _w; // the writer connection (writer thread only)
        // Statements the writer runs per session / per file, compiled once (preparing the 111-column session
        // upsert cost ~0.2 s each time).
        std::unique_ptr<Prepared> _sessionUpsert;
        std::unique_ptr<Prepared> _cursorUpsert;
        TextIndex _index;

        mutable std::shared_mutex _mapMtx; // guards the maps below
        std::unordered_map<std::wstring, int32_t> _fileIdByKey; // path key -> file id
        std::unordered_map<int32_t, FileRow> _files;
        std::unordered_map<std::wstring, int32_t> _sidByid; // lower-case session id -> sid key
        std::unordered_map<int32_t, SessionRow> _sessions;
        std::map<std::wstring, int32_t> _idsSorted; // lower-case id -> sid key (short-id neighbors)
        std::unordered_set<std::wstring> _open; // the GUI's open sessions (lower-case)
        std::vector<int32_t> _shortIdDirty; // sessions whose stored short id is stale (a new neighbor lengthened it)
        int32_t _nextFileId{ 1 };
        int32_t _nextSidKey{ 1 };
        uint32_t _nextDocId{ 1 };

        std::unordered_map<int32_t, FileWork> _work; // per top-level file (LRU-bounded)
        int64_t _workClock{ 0 };

        std::unordered_map<std::wstring, std::wstring> _gitRootCache; // dir key -> git root ("" = none)
        int8_t _driveOk[26]{}; // per drive letter: 0 unknown, 1 probeable, -1 not (refreshed every few minutes)
        int64_t _driveCheckedMs[26]{};
        std::vector<std::wstring> _tempRoots; // machine temp roots (excluded from the inferred-dir vote)
        std::wstring _homeKey; // NormDirKey(%USERPROFILE%)

        // Where a sweep's time goes (ms per step, reset by Resync and logged at its end): the read + extract +
        // append loop, then each derived step of the batches' flushes, and the commits.
        struct StepTimes
        {
            int64_t ingest{ 0 }, appenders{ 0 }, paths{ 0 }, usage{ 0 }, walk{ 0 }, summary{ 0 }, sessionRows{ 0 }, cursors{ 0 }, commit{ 0 };
        } _times;

        std::atomic<int64_t> _lastWriteMs{ 0 };
        int64_t _lastMergeMs{ 0 };
        int64_t _hotDocsEstimate{ 0 };
        int64_t _hotBytesEstimate{ 0 };
    };
}
