// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SESSIONS_STORE.md) — the FLEET STORE: every saved session in a SQLite database, <profile>\sessions.db,
// instead of the whole-file rewrites of sessions.json.
//
// Why it exists: sessions.json held the whole fleet (13 MB on production, 92 % of it already-sent prompts) and every
// saved change rewrote all of it, temp file + flush + rename, up to once a second — 19.8 GB in 11.3 hours, and 2,186
// rewrites of a growing file during each start's restore (4.6 minutes, a partial fleet on disk the whole time). Here a
// change writes only the rows it touched: a session's own members in one small row, each queued prompt's mutable state in
// one small row, each prompt's text once (§4.2). Pages are 512 bytes, so a routine change costs about a kilobyte of WAL.
//
// The pieces, in the order a run meets them:
//   1. OpenFleetForStartupIn — the startup open (TerminalPage::_RestoreClaudeSessions through LoadSessionsForStartup): the
//      one-time migration of sessions.json (§4.9), the import of a sessions.json an older build wrote after it (§4.8), the
//      database's checks, and the load of the fleet. It decides the run's FleetMode.
//   2. The writer (StartFleetWriter, from the engine): the registry marks a record dirty when it changes
//      (SessionRegistry::SetRecordChangeSink → NoteFleetRecordChanged); one thread commits the dirty records every
//      250 ms (synchronous=FULL), a rolling sweep re-checks every record about once a minute, checkpoints run on that
//      thread only. PersistFleetNow / PersistFleetNowDurable are the critical commits (§4.4).
//   3. LoadFleetReadOnlyIn — the other processes' read (the CLI, the session database service): the database when it
//      exists, else sessions.json.
//
// Thread model: FleetDb is internally locked (one mutex around its connection and its row cache), so any thread may call
// it; the process-wide store (the functions below) has its own locks, ordered fleet store → FleetDb → registry; the dirty
// set's lock is a leaf. Plain C++ + Win32 + SQLite: no WinRT, so the m5 harness, the CLI and the session database service
// compile it.

#pragma once

#include "Persistence.h" // FleetLoad, SessionInfo (SessionModels.h)

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3; // sqlite3.h (vcpkg's sqlite3); kept out of this header so its includers need no SQLite include path

namespace Agentmaster
{
    class SessionRegistry;
    namespace json
    {
        struct Value;
    }
    namespace StateCompat
    {
        struct ResidueNode;
    }

    /// <summary>The database file's name inside the profile.</summary>
    inline constexpr const wchar_t* kFleetDbFile = L"sessions.db";
    /// <summary>The JSON file the store replaces (read once by the migration, never written in SQLite mode).</summary>
    inline constexpr const wchar_t* kFleetJsonFile = L"sessions.json";
    /// <summary>
    /// The store's format (meta 'format'). A build that finds a GREATER number opens the database read-only: a newer build's
    /// data is never overwritten (§17's format guard, SESSIONS_STORE.md §4.7). Raise it only for a change older builds would
    /// misread; adding a member to a row's JSON doc is not one (the residue keeps it).
    /// </summary>
    inline constexpr int kFleetDbFormat = 1;
    /// <summary>
    /// The page size (D1, SESSIONS_STORE.md §3.2): the smallest SQLite allows, so a routine change writes ~1 KB of WAL. Fixed
    /// for the file's life once its first table exists.
    /// </summary>
    inline constexpr int kFleetDbPageSize = 512;
    /// <summary>The first line of the migration backup's README: StateBackups.h's pruner never matches it (kept for good).</summary>
    inline constexpr std::wstring_view kFleetMigrationBackupMarker = L"Agentmaster sessions.json migration backup";

    /// <summary>How a FleetDb connection is opened.</summary>
    enum class FleetDbAccess
    {
        ReadWrite, // the app's one writer: the lock file held, checkpoints allowed
        ReadOnly, // another process (the CLI, the session database service) or a second instance: never writes
        Snapshot, // the file ALONE, immutable (no lock, no WAL read): only when no writer can exist — the migration's
                  // verification reads its temp database this way after the TRUNCATE checkpoint, so it proves the data is in
                  // the database file itself
    };

    /// <summary>What a run persists through, decided by OpenFleetForStartupIn.</summary>
    enum class FleetMode
    {
        None, // not opened yet (before the first window's restore): dirty marks wait
        Sqlite, // the database, read-write: the normal mode
        SqliteReadOnly, // the database could be read but not written (a newer format, another writer holds the lock): the
                        // run's changes are not saved
        Json, // the legacy whole-file sessions.json: the migration could not run this start (it retries at the next), or
              // sessions.db exists but cannot be used — then it is never written over, the run saves sessions.json beside
              // it, and the next start that can use the database imports that file (FleetReconcileJson)
    };

    /// <summary>One record for FleetDb::Write: a session's current record, or its removal (nullopt).</summary>
    struct FleetChange
    {
        std::wstring id; // the session id (the registry's key)
        std::optional<SessionInfo> record; // nullopt = the session left the registry: its rows are deleted
    };

    /// <summary>What a FleetDb::Write did.</summary>
    struct FleetWriteResult
    {
        bool ok{ true }; // false: the transaction rolled back (nothing of this write is on disk)
        size_t rowsWritten{ 0 }; // rows inserted or updated
        size_t rowsDeleted{ 0 }; // rows deleted
        std::wstring error; // SQLite's message when !ok
    };

    /// <summary>
    /// One SQLite connection to a fleet database, with the row cache that makes a write touch only changed rows and the
    /// per-session residue that keeps what a newer build wrote (§17). Internally locked: any thread may call it.
    /// </summary>
    /// <remarks>
    /// The schema (SESSIONS_STORE.md §4.2): meta(key, value); sessions(id, ord, doc) — every member of the session's JSON
    /// record except "queue"; prompt_state(session_id, prompt_key, ord, state) — every member of a queued prompt except
    /// "text"; prompt_text(session_id, prompt_key, text) — written when the prompt is new or its text changed. Rowid tables:
    /// a 512-byte rowid leaf keeps ~477 bytes of a row in the page, a WITHOUT ROWID one only ~100.
    /// </remarks>
    class FleetDb
    {
    public:
        /// <summary>Opens (or creates) a fleet database.</summary>
        /// <param name="path">The database file.</param>
        /// <param name="access">ReadWrite takes the lock file (sessions.db.lock); a lock another process holds opens
        /// read-only instead (the caller sees ReadOnly()).</param>
        /// <param name="create">True: create the file and its schema when missing (a fresh profile, the migration's temp
        /// file). False: a missing file fails.</param>
        /// <param name="problem">Receives why the open failed (empty on success).</param>
        /// <returns>The connection, or null (problem says why).</returns>
        /// <remarks>Never throws.</remarks>
        static std::unique_ptr<FleetDb> Open(const std::wstring& path, FleetDbAccess access, bool create, std::wstring& problem) noexcept;

        /// <summary>Closes the connection (and releases the lock file).</summary>
        ~FleetDb();
        FleetDb(const FleetDb&) = delete;
        FleetDb& operator=(const FleetDb&) = delete;

        /// <summary>
        /// Reads the whole fleet (sessions in their stored order, each with its queue in order) and primes the row cache and
        /// the residues, so the first write of an unchanged record writes nothing.
        /// </summary>
        /// <param name="out">Receives the sessions (cleared first).</param>
        /// <param name="problem">Receives what was wrong when the read failed or a row could not be used.</param>
        /// <returns>False when the read failed (a SQLite error); a row that cannot be parsed is skipped, counted in
        /// <paramref name="problem"/>, and kept on disk untouched.</returns>
        /// <exception cref="std::bad_alloc">Out of memory.</exception>
        bool LoadAll(std::vector<SessionInfo>& out, std::wstring& problem);

        /// <summary>
        /// Writes the given records in ONE transaction: each session's changed rows only (compared with what this
        /// connection last read or wrote), its gone prompts deleted, a removed session's rows deleted.
        /// </summary>
        /// <param name="changes">The records to write (a record whose rows did not change costs a serialization, no SQL).</param>
        /// <param name="durable">True: synchronous=FULL (the WAL is fsynced before the commit returns). False: NORMAL (the
        /// commit is in the OS cache: a kill loses nothing, a power cut may; FlushWal makes it durable).</param>
        /// <returns>What was written; on failure the transaction rolled back and the cache is unchanged, so the same
        /// records written again produce the same rows.</returns>
        /// <remarks>Never throws (a failure is a result). A read-only connection writes nothing and returns ok.</remarks>
        FleetWriteResult Write(const std::vector<FleetChange>& changes, bool durable) noexcept;

        /// <summary>
        /// Imports raw session objects (as a JSON file holds them: queue and texts inside) in ONE transaction, each split into
        /// its rows exactly as the file had its members — so even members this build does not know move over unchanged.
        /// Sessions whose id is already stored are skipped.
        /// </summary>
        /// <param name="sessions">The session objects (each an object with an "id").</param>
        /// <param name="meta">Meta rows to set in the same transaction (key → value), e.g. the migration record.</param>
        /// <param name="imported">Receives the ids imported.</param>
        /// <param name="problem">Receives SQLite's message on failure.</param>
        /// <returns>True when the transaction committed (synchronous=FULL).</returns>
        /// <remarks>Never throws.</remarks>
        bool ImportRaw(const std::vector<const json::Value*>& sessions, const std::vector<std::pair<std::wstring, std::wstring>>& meta,
                       std::vector<std::wstring>& imported, std::wstring& problem) noexcept;

        /// <summary>
        /// Reads every session back as a raw JSON object (its doc with the queue rebuilt from its prompt rows), in stored
        /// order — the migration's verification and the tests' view of what is on disk.
        /// </summary>
        /// <param name="out">Receives (id, object) pairs.</param>
        /// <param name="problem">Receives what was wrong.</param>
        /// <returns>False when a read failed or a row did not parse.</returns>
        /// <remarks>Never throws.</remarks>
        bool ReadRaw(std::vector<std::pair<std::wstring, json::Value>>& out, std::wstring& problem) const noexcept;

        /// <summary>The ids stored (any order).</summary>
        /// <returns>The ids; empty on a read failure.</returns>
        /// <remarks>Never throws.</remarks>
        std::vector<std::wstring> StoredIds() const noexcept;

        /// <summary>
        /// Makes every committed transaction durable: FlushFileBuffers on the WAL through a handle of its own (a flush writes
        /// the file's cached data whoever wrote it — MESSAGING_OUTBOX.md §5.5). The UI-thread critical commit's second half.
        /// </summary>
        /// <returns>True when flushed, or when there is no WAL (everything is in the database file).</returns>
        /// <remarks>Never throws.</remarks>
        bool FlushWal() noexcept;

        /// <summary>Copies the WAL's committed frames into the database file.</summary>
        /// <param name="truncate">True: TRUNCATE (the WAL emptied; at the app's close). False: PASSIVE (never waits for a
        /// reader).</param>
        /// <returns>True when SQLite reported success.</returns>
        /// <remarks>Never throws.</remarks>
        bool Checkpoint(bool truncate) noexcept;

        /// <summary>The WAL file's size in bytes (0 when there is none).</summary>
        /// <returns>The size.</returns>
        /// <remarks>Never throws.</remarks>
        uint64_t WalBytes() const noexcept;

        /// <summary>PRAGMA quick_check (or integrity_check).</summary>
        /// <param name="full">True: integrity_check (every index entry checked: the migration's verification).</param>
        /// <param name="problem">Receives SQLite's answer when it is not "ok".</param>
        /// <returns>True when the answer is "ok".</returns>
        /// <remarks>Never throws.</remarks>
        bool Check(bool full, std::wstring& problem) const noexcept;

        /// <summary>A meta row's value.</summary>
        /// <param name="key">The key.</param>
        /// <returns>The value, or "" when absent or unreadable.</returns>
        /// <remarks>Never throws.</remarks>
        std::wstring Meta(const std::wstring& key) const noexcept;

        /// <summary>Whether this connection never writes (opened ReadOnly, the lock held elsewhere, or a newer format).</summary>
        bool ReadOnly() const noexcept { return _readOnly; }
        /// <summary>Why the connection is read-only ("" for a writer): for the log line.</summary>
        const std::wstring& ReadOnlyReason() const noexcept { return _readOnlyReason; }
        /// <summary>The database file.</summary>
        const std::wstring& Path() const noexcept { return _path; }
        /// <summary>
        /// Closes the connection now (a checkpoint and close before the migration's rename; tests). Every later call fails
        /// or writes nothing.
        /// </summary>
        /// <remarks>Never throws.</remarks>
        void Close() noexcept;

        // The row cache of one stored prompt (public only for the implementation file's helpers).
        struct CachedPrompt
        {
            int64_t ord{ 0 }; // its position in the queue
            std::wstring state; // the stored state doc text
            bool hasText{ false }; // the stored text row holds a string (NULL = the prompt had no "text" member)
            uint64_t textHash{ 0 }; // FNV-1a 64 of the text
            size_t textLength{ 0 }; // its length (a second check beside the hash)
        };
        // The row cache of one stored session.
        struct CachedSession
        {
            int64_t ord{ 0 }; // its position in the fleet
            bool hasQueue{ false }; // its "queue" was an array (split into prompt rows); false = no queue member, or a
                                    // non-array one left in the doc
            std::wstring doc; // the stored doc text
            std::unordered_map<std::wstring, CachedPrompt> prompts; // prompt key → its rows
        };

    private:
        FleetDb() = default;

        mutable std::mutex _mtx; // the connection and the caches
        sqlite3* _db{ nullptr };
        void* _lock{ nullptr }; // the lock file's HANDLE (ReadWrite), or null
        std::wstring _path;
        bool _readOnly{ false };
        std::wstring _readOnlyReason;
        int64_t _nextOrd{ 0 }; // the next session's ord
        uint64_t _keptSeq{ 0 }; // makes each "kept" key unique within a millisecond (Write's move-aside of unknown rows)
        std::unordered_map<std::wstring, CachedSession> _cache; // session id → its stored rows
        std::unordered_map<std::wstring, std::shared_ptr<const StateCompat::ResidueNode>> _residue; // id → what a newer build wrote
    };

    // ---- the process-wide store ------------------------------------------------------------------------------------------

    /// <summary>
    /// The STARTUP open of the profile's fleet (SESSIONS_STORE.md §4.5, §4.8, §4.9): leftovers of an interrupted migration
    /// are deleted; an existing database is checked, finishes an interrupted migration's last step, imports what an older
    /// build wrote to sessions.json after the move, and loads; without a database, sessions.json is migrated (a verified
    /// permanent backup, one transaction into a temp database, a full verification, an atomic rename, sessions.json deleted
    /// last). Decides the run's mode (CurrentFleetMode). Called once per process, under the engine's restore barrier.
    /// </summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <returns>
    /// The fleet and how the read went, as LoadSessionsForStartupIn always returned it: `usable` false when neither the
    /// database nor sessions.json could be used (the caller then keeps the workspace in recovery\ and holds tab refs).
    /// </returns>
    /// <exception cref="std::bad_alloc">Out of memory (every other failure is a FleetLoad).</exception>
    FleetLoad OpenFleetForStartupIn(const std::wstring& profileDir);

    /// <summary>The run's mode (None before OpenFleetForStartupIn).</summary>
    /// <returns>The mode.</returns>
    /// <remarks>Never throws.</remarks>
    FleetMode CurrentFleetMode() noexcept;

    /// <summary>
    /// The process store as the writer sees it (FleetWriter.cpp's door into this file): the mode and the open connection.
    /// The connection, once set by OpenFleetForStartupIn, lives for the rest of the process (only ResetFleetStoreForTests
    /// drops it), so the pointer may be used after this returns; FleetDb's own lock serializes its use.
    /// </summary>
    /// <param name="mode">Receives the mode.</param>
    /// <param name="db">Receives the connection (null unless Sqlite / SqliteReadOnly).</param>
    /// <remarks>Never throws.</remarks>
    void GetFleetStore(FleetMode& mode, FleetDb*& db) noexcept;

    /// <summary>
    /// The profile folder the run's store was opened in (OpenFleetForStartupIn's argument; "" before it): where the writer's
    /// Json mode saves sessions.json, beside the store it fell back from.
    /// </summary>
    /// <returns>The folder.</returns>
    /// <exception cref="std::bad_alloc">Out of memory (the copy).</exception>
    std::wstring FleetStoreProfileDir();

    /// <summary>
    /// The read of another process (the CLI, the session database service): sessions.db read-only when it exists (a
    /// consistent snapshot while the app writes: WAL), else sessions.json as before.
    /// </summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <returns>The fleet; empty when nothing could be read.</returns>
    /// <exception cref="std::bad_alloc">Out of memory.</exception>
    std::vector<SessionInfo> LoadFleetReadOnlyIn(const std::wstring& profileDir);

    /// <summary>
    /// Starts the process's writer thread over the registry and wires the registry's change sink to it. Once per process,
    /// at engine init (before any window restores); a second call does nothing.
    /// </summary>
    /// <param name="registry">The process's registry.</param>
    /// <remarks>Never throws (a failure to start is logged; the critical commits still write on their callers' threads).</remarks>
    void StartFleetWriter(std::shared_ptr<SessionRegistry> registry) noexcept;

    /// <summary>Marks one session's record dirty: the writer commits it within ~250 ms. Any thread; cheap.</summary>
    /// <param name="id">The session id.</param>
    /// <remarks>Never throws.</remarks>
    void NoteFleetRecordChanged(const std::wstring& id) noexcept;

    /// <summary>
    /// The UI-THREAD critical commit (close / archive / bind / teardown, a !live notify): every dirty record committed NOW on
    /// the calling thread with synchronous=NORMAL (kill-safe at return: the frames are in the OS cache), then the writer
    /// fsyncs the WAL at once. It never fsyncs itself; it can wait for a commit already running on the writer thread (the
    /// flush lock — milliseconds). Skipped while a FleetBulkScope is open (the restore). In the legacy Json mode it is the
    /// whole-file save, as before the store.
    /// </summary>
    /// <remarks>Never throws.</remarks>
    void PersistFleetNow() noexcept;

    /// <summary>
    /// The WORKER-THREAD critical commit (Rule #4: a prompt marked Sent before it is typed): every dirty record committed NOW
    /// with synchronous=FULL, and any UI-thread commit still only in the OS cache fsynced — on the disk when this returns
    /// (~3 ms). Never call it on a UI thread.
    /// </summary>
    /// <returns>
    /// True when the records are saved (or nothing was dirty). False when a commit or the fsync failed: the caller must not
    /// act as if the change were saved (the scheduler rolls the prompt back). Two runs answer true without saving, because
    /// refusing every send would not make them safer: a read-only one (SqliteReadOnly: a newer format, or another process
    /// holds the store — logged as unsaved at its open), and one whose store is not open yet (FleetMode::None: before the
    /// first window's restore, when nothing is live; the marks wait for the open).
    /// </returns>
    /// <remarks>Never throws.</remarks>
    bool PersistFleetNowDurable() noexcept;

    /// <summary>
    /// The app's last window closed (Engine's FlushStateShadows moment): every dirty record committed with FULL, then a
    /// TRUNCATE checkpoint, so the database file alone holds the fleet.
    /// </summary>
    /// <remarks>Never throws.</remarks>
    void FlushFleetAtAppClose() noexcept;

    /// <summary>
    /// RAII: while one is open (the startup restore's Upsert loop), dirty marks accumulate and no immediate commit runs; the
    /// last scope's end lets the writer commit them as one batch. Nested scopes count.
    /// </summary>
    class FleetBulkScope
    {
    public:
        /// <summary>Opens the scope.</summary>
        FleetBulkScope() noexcept;
        /// <summary>Closes it; the outermost one wakes the writer.</summary>
        ~FleetBulkScope();
        FleetBulkScope(const FleetBulkScope&) = delete;
        FleetBulkScope& operator=(const FleetBulkScope&) = delete;
    };

    // ---- test seams (the m5 harness; never called by the app) -------------------------------------------------------------

    /// <summary>
    /// TEST SEAM: called at each migration step boundary (1 = decided, 2 = backed up, 3 = parsed, 4 = built, 5 = verified,
    /// 6 = renamed, 7 = sessions.json deleted). The crash tests' child kills itself there to prove every step is safe.
    /// </summary>
    /// <param name="hook">The hook, or null to remove it.</param>
    void SetFleetMigrationStepHookForTests(std::function<void(int step)> hook) noexcept;

    /// <summary>TEST SEAM: forgets the process-wide store (mode None, the database closed, the writer left running), so the
    /// next OpenFleetForStartupIn plays a new process start.</summary>
    void ResetFleetStoreForTests() noexcept;

    /// <summary>TEST SEAM: the open store's connection (null unless the mode is Sqlite / SqliteReadOnly).</summary>
    /// <returns>The connection (owned by the store).</returns>
    FleetDb* FleetDbForTests() noexcept;

    /// <summary>TEST SEAM: commits every dirty record now from the calling thread, as the writer's 250-ms pass does.</summary>
    /// <returns>True when nothing failed.</returns>
    bool FlushFleetDirtyForTests() noexcept;

    /// <summary>
    /// TEST SEAM: points the writer at a registry WITHOUT starting its thread (the registry's change sink wired), so a test
    /// drives the commits itself with FlushFleetDirtyForTests. A later call replaces the registry (each test its own).
    /// </summary>
    /// <param name="registry">The test's registry.</param>
    void BindFleetWriterRegistryForTests(std::shared_ptr<SessionRegistry> registry) noexcept;

    /// <summary>TEST SEAM: how many records are marked dirty now.</summary>
    /// <returns>The count.</returns>
    size_t FleetDirtyCountForTests() noexcept;

    /// <summary>
    /// TEST SEAM: the order numbers FleetDb::Write stores for a queue whose k-th prompt (in the new order) is stored with
    /// cached[k] (nullopt = a new prompt): the stored numbers that already rise keep theirs, the others fit between them.
    /// </summary>
    /// <param name="cached">The stored numbers in the new order.</param>
    /// <returns>The numbers to store, strictly rising.</returns>
    /// <exception cref="std::bad_alloc">Out of memory.</exception>
    std::vector<int64_t> PlanFleetPromptOrdsForTests(const std::vector<std::optional<int64_t>>& cached);

    /// <summary>
    /// A JSON value as its canonical text: object members sorted by key, numbers that are not integers with 17 significant
    /// digits (exact for a double). Two values are equal exactly when their canonical texts are (the migration's
    /// verification).
    /// </summary>
    /// <param name="v">The value.</param>
    /// <returns>The canonical text.</returns>
    /// <exception cref="std::bad_alloc">Out of memory.</exception>
    std::wstring CanonicalJsonText(const json::Value& v);
}
