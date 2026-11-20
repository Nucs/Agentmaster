// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SESSIONS_STORE.md §4.3, §4.4): the fleet store's WRITER — what replaced the SessionAutosaver's whole-file
// rewrites of sessions.json.
//
//   - The registry marks a record dirty when it changes (SessionRegistry::SetRecordChangeSink → NoteFleetRecordChanged).
//   - One thread commits the dirty records 250 ms after the first mark of a batch, in ONE transaction with
//     synchronous=FULL: a power cut loses at most that window of routine changes, a kill nothing that was committed.
//   - The critical commits run on their callers' threads: PersistFleetNow (a UI thread: NORMAL, so it never fsyncs, then
//     the writer fsyncs the WAL at once) and PersistFleetNowDurable (a worker thread: on the disk before it returns —
//     Rule #4's "Sent is saved before the prompt is typed").
//   - A rolling sweep re-checks about 1/30 of the fleet every two seconds, so a change that marked nothing (the draft's
//     "last seen" stamp, by design) is saved within about a minute; an unchanged record costs a serialization, no SQL.
//   - Checkpoints run here only: PASSIVE when the WAL passes 4 MB or every 5 minutes, TRUNCATE at the app's close.
//
// Every commit — the thread's and the critical ones — runs under one flush lock: a record is taken from the dirty set,
// read from the registry and written as one step, so an older copy can never be committed after a newer one.
//
// What is on the disk is tracked by two sequence numbers: every NORMAL commit that wrote rows takes the next `normalSeq`;
// a FULL commit, or an fsync of the WAL, that STARTED after commit N makes `syncedSeq` at least N (an fsync writes every
// frame already in the OS cache). normalSeq > syncedSeq means a UI-thread commit's frames may still be only in the cache.

#include "FleetDb.h"

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "SessionRegistry.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace Agentmaster;

    // The routine group commit's window: a burst of changes within it is one transaction.
    constexpr int64_t kFleetCoalesceMs = 250;
    // After a failed commit (a full disk, a failing drive), the next attempt waits this long: the marks stay, nothing is
    // dropped, and a disk in trouble is not hammered four times a second.
    constexpr int64_t kFleetRetryAfterFailureMs = 2000;
    // Before the store is open (the engine starts before the first window's restore opens it), the thread looks again
    // this often.
    constexpr int64_t kFleetWaitForStoreMs = 200;
    // The rolling sweep: a slice every 2 s, the whole fleet in about 30 slices (~1 minute).
    constexpr int64_t kFleetSweepEveryMs = 2000;
    constexpr size_t kFleetSweepSlices = 30;
    // Checkpoints: when the WAL passes this size, or this long after the last one.
    constexpr uint64_t kFleetCheckpointWalBytes = 4ULL * 1024 * 1024;
    constexpr int64_t kFleetCheckpointEveryMs = 5 * 60 * 1000;
    // A WAL this large means a checkpoint cannot get past a reader (a CLI or a test holding a read transaction): logged.
    constexpr uint64_t kFleetLongWalBytes = 64ULL * 1024 * 1024;
    // The summary line in hooks.log, at most once per this period (and only when something was committed).
    constexpr int64_t kFleetSummaryEveryMs = 60 * 1000;
    // A failure's log line, at most once per this period.
    constexpr int64_t kFleetFailureLogEveryMs = 30 * 1000;

    // The writer's state. Leaked on purpose (the app ends with TerminateProcess; the thread never stops).
    struct FleetWriterState
    {
        // The dirty set: a LEAF lock (code holding it calls nothing that takes another lock).
        std::mutex dirtyMtx;
        std::condition_variable cv;
        std::unordered_set<std::wstring> dirty;
        int64_t firstDirtyMs{ 0 }; // when the oldest mark of the current batch arrived (0 = none)

        // Every commit runs under this lock (see the file comment).
        std::mutex flushMtx;

        std::atomic<int> bulk{ 0 }; // open FleetBulkScopes
        std::atomic<bool> fsyncWanted{ false }; // a UI-thread commit asked for the WAL to be fsynced
        std::atomic<bool> started{ false }; // StartFleetWriter ran
        std::atomic<uint64_t> normalSeq{ 0 }; // NORMAL commits that wrote rows (see the file comment)
        std::atomic<uint64_t> syncedSeq{ 0 }; // the NORMAL commits known to be on the disk

        std::mutex registryMtx; // guards `registry` (set once in the app; replaced by the tests' seam)
        std::shared_ptr<SessionRegistry> registry;

        // The summary and the failure log's throttles.
        std::atomic<uint64_t> commits{ 0 };
        std::atomic<uint64_t> records{ 0 };
        std::atomic<uint64_t> rowsWritten{ 0 };
        std::atomic<uint64_t> rowsDeleted{ 0 };
        std::atomic<uint64_t> failures{ 0 };
        std::atomic<int64_t> lastFailureLogMs{ 0 };
        size_t sweepCursor{ 0 }; // the writer thread's own
    };

    FleetWriterState& FleetW() noexcept
    {
        static FleetWriterState* w = new FleetWriterState();
        return *w;
    }

    int64_t FleetTickMs() noexcept
    {
        return static_cast<int64_t>(::GetTickCount64());
    }

    // Raises an atomic to at least `value`. Never throws.
    void FleetStoreMax(std::atomic<uint64_t>& a, uint64_t value) noexcept
    {
        uint64_t seen = a.load();
        while (seen < value && !a.compare_exchange_weak(seen, value))
        {
        }
    }

    // Logs a line to hooks.log. Never throws.
    void FleetWriterLog(const std::wstring& line) noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", line + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE: the line could not be built (std::bad_alloc); there is nothing to recover.
        }
    }

    std::shared_ptr<SessionRegistry> FleetRegistry() noexcept
    {
        std::lock_guard lk{ FleetW().registryMtx };
        return FleetW().registry;
    }

    // Takes every dirty id (the set is emptied). Never throws (an allocation failure takes nothing: the marks stay).
    std::vector<std::wstring> FleetTakeDirty() noexcept
    {
        auto& w = FleetW();
        std::lock_guard lk{ w.dirtyMtx };
        std::vector<std::wstring> ids;
        try
        {
            ids.assign(w.dirty.begin(), w.dirty.end());
            w.dirty.clear();
            w.firstDirtyMs = 0;
        }
        catch (...)
        {
            ids.clear();
        }
        return ids;
    }

    // Puts ids back into the dirty set (a commit that failed): the next pass retries them. Never throws.
    void FleetRemark(const std::vector<std::wstring>& ids) noexcept
    {
        auto& w = FleetW();
        {
            std::lock_guard lk{ w.dirtyMtx };
            try
            {
                for (const auto& id : ids)
                {
                    w.dirty.insert(id);
                }
                if (!w.dirty.empty() && w.firstDirtyMs == 0)
                {
                    w.firstDirtyMs = FleetTickMs();
                }
            }
            catch (...)
            {
                // std::bad_alloc: some ids are not re-marked; the rolling sweep finds them within about a minute.
            }
        }
        w.cv.notify_one();
    }

    // Commits every dirty record: the writer's pass and the critical commits (see the file comment).
    //   durable - synchronous=FULL (true) or NORMAL (false).
    //   returns - false when a commit failed (its records are marked again); true when everything taken was written, or
    //             nothing was dirty, or the run never writes (read-only; the store not open yet: the marks then wait).
    // Never throws.
    bool FleetFlushDirty(bool durable) noexcept
    {
        auto& w = FleetW();
        std::lock_guard flush{ w.flushMtx };
        FleetMode mode = FleetMode::None;
        FleetDb* db = nullptr;
        GetFleetStore(mode, db);
        if (mode == FleetMode::None)
        {
            return true; // the store is not open yet (before the first window's restore): the marks wait
        }
        auto ids = FleetTakeDirty();
        if (ids.empty())
        {
            return true;
        }
        if (mode == FleetMode::SqliteReadOnly)
        {
            return true; // this run never writes (the reason was logged at the open): the marks are dropped
        }
        const auto registry = FleetRegistry();
        if (!registry)
        {
            FleetRemark(ids);
            return false;
        }
        try
        {
            if (mode == FleetMode::Json)
            {
                // The legacy whole-file save (the start could not migrate, or sessions.db cannot be used): exactly what
                // the autosaver did — temp file, flush, write-through rename, so it is durable whatever `durable` says —
                // into the folder the store was opened in (the app's profile).
                SaveSessionsIn(FleetStoreProfileDir(), registry->Snapshot());
                w.commits.fetch_add(1);
                return true;
            }
            if (ids.size() > 1)
            {
                // The dirty set has no order: write a batch in the registry's insertion order, so sessions new in it take
                // their stored order as the registry holds them (the next start restores the fleet in that order). One
                // pass over the registry's ids, only for a batch of more than one record; ids no longer in the registry
                // (removed) go last.
                std::unordered_map<std::wstring, size_t> rank;
                rank.reserve(ids.size());
                for (const auto& id : ids)
                {
                    rank.emplace(id, SIZE_MAX);
                }
                const auto order = registry->Ids();
                for (size_t k = 0; k < order.size(); ++k)
                {
                    if (const auto it = rank.find(order[k]); it != rank.end())
                    {
                        it->second = k;
                    }
                }
                std::stable_sort(ids.begin(), ids.end(), [&](const std::wstring& a, const std::wstring& b) { return rank[a] < rank[b]; });
            }
            std::vector<FleetChange> changes;
            changes.reserve(ids.size());
            for (const auto& id : ids)
            {
                changes.push_back(FleetChange{ id, registry->Get(id) }); // nullopt: the session left the registry
            }
            // Every NORMAL commit before this one is complete (this lock), so a FULL commit that writes makes them durable.
            const uint64_t normalBefore = w.normalSeq.load();
            const FleetWriteResult r = db ? db->Write(changes, durable) : FleetWriteResult{ false, 0, 0, L"no connection" };
            if (!r.ok)
            {
                FleetRemark(ids);
                w.failures.fetch_add(1);
                const int64_t now = FleetTickMs();
                const int64_t last = w.lastFailureLogMs.load();
                if (last == 0 || now - last >= kFleetFailureLogEveryMs)
                {
                    w.lastFailureLogMs.store(now);
                    FleetWriterLog(L"[fleet-fail] a commit of " + std::to_wstring(ids.size()) + L" record(s) failed (" + r.error +
                                   L"); they stay marked and are written again at the next pass");
                }
                return false;
            }
            if (r.rowsWritten || r.rowsDeleted)
            {
                if (durable)
                {
                    FleetStoreMax(w.syncedSeq, normalBefore); // the WAL's fsync wrote the earlier NORMAL frames too
                }
                else
                {
                    w.normalSeq.fetch_add(1);
                }
            }
            w.commits.fetch_add(1);
            w.records.fetch_add(ids.size());
            w.rowsWritten.fetch_add(r.rowsWritten);
            w.rowsDeleted.fetch_add(r.rowsDeleted);
            return true;
        }
        catch (...)
        {
            // Rule #18. Recovery: the records are marked again; the next pass writes them.
            LogSwallowedException(L"FleetFlushDirty");
            FleetRemark(ids);
            return false;
        }
    }

    // Fsyncs the WAL when a NORMAL commit may still be only in the OS cache. Returns false when that fsync failed (true when
    // there was nothing to do, or the run does not write the database). Never throws.
    bool FleetSyncNormalCommits() noexcept
    {
        auto& w = FleetW();
        const uint64_t wanted = w.normalSeq.load(); // read BEFORE the fsync: every commit up to it is in the cache already
        if (wanted <= w.syncedSeq.load())
        {
            return true;
        }
        FleetMode mode = FleetMode::None;
        FleetDb* db = nullptr;
        GetFleetStore(mode, db);
        if (!db || mode != FleetMode::Sqlite)
        {
            return true;
        }
        if (!db->FlushWal())
        {
            return false;
        }
        FleetStoreMax(w.syncedSeq, wanted);
        return true;
    }

    // Marks the next slice of the fleet dirty (the rolling sweep). Never throws.
    void FleetSweepSlice() noexcept
    {
        try
        {
            const auto registry = FleetRegistry();
            if (!registry)
            {
                return;
            }
            const auto ids = registry->Ids();
            if (ids.empty())
            {
                return;
            }
            auto& w = FleetW();
            const size_t slice = (std::max)(size_t{ 1 }, (ids.size() + kFleetSweepSlices - 1) / kFleetSweepSlices);
            {
                std::lock_guard lk{ w.dirtyMtx };
                for (size_t k = 0; k < slice && k < ids.size(); ++k)
                {
                    w.dirty.insert(ids[(w.sweepCursor + k) % ids.size()]);
                }
                if (w.firstDirtyMs == 0)
                {
                    w.firstDirtyMs = FleetTickMs();
                }
                w.sweepCursor = (w.sweepCursor + slice) % ids.size();
            }
        }
        catch (...)
        {
            LogSwallowedException(L"FleetSweepSlice"); // Rule #18. Recovery: this slice is swept again next round.
        }
    }

    // The writer thread (see the file comment). Never returns; never throws.
    void FleetWriterLoop() noexcept
    {
        auto& w = FleetW();
        int64_t lastSweep = FleetTickMs();
        int64_t lastCheckpoint = FleetTickMs();
        int64_t lastSummary = FleetTickMs();
        int64_t retryAt = 0; // after a failed commit: the next routine attempt not before this tick
        bool longWalLogged = false;
        uint64_t summarized = 0;
        for (;;)
        {
            try
            {
                FleetMode mode = FleetMode::None;
                FleetDb* db = nullptr;
                GetFleetStore(mode, db);
                if (mode == FleetMode::None)
                {
                    // The store is not open yet: the marks wait, and so does this thread. (Without this, a mark made before
                    // the open would keep the wait's predicate true and spin the loop.)
                    std::unique_lock lk{ w.dirtyMtx };
                    w.cv.wait_for(lk, std::chrono::milliseconds(kFleetWaitForStoreMs));
                    continue;
                }
                bool flush = false;
                bool fsync = false;
                {
                    std::unique_lock lk{ w.dirtyMtx };
                    w.cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return (!w.dirty.empty() && w.bulk.load() == 0) || w.fsyncWanted.load(); });
                    if (!w.dirty.empty() && w.bulk.load() == 0)
                    {
                        // The group commit: 250 ms after the first mark of the batch (later after a failure) — unless a
                        // UI-thread commit wants the WAL fsynced now, which a FULL commit does anyway.
                        const int64_t due = (std::max)(w.firstDirtyMs + kFleetCoalesceMs, retryAt);
                        const int64_t now = FleetTickMs();
                        if (now < due && !w.fsyncWanted.load())
                        {
                            w.cv.wait_for(lk, std::chrono::milliseconds(due - now), [&] { return w.fsyncWanted.load() || w.bulk.load() != 0; });
                        }
                        flush = w.bulk.load() == 0 && (FleetTickMs() >= retryAt || w.fsyncWanted.load());
                    }
                    fsync = w.fsyncWanted.exchange(false);
                }
                if (flush)
                {
                    retryAt = FleetFlushDirty(true) ? 0 : FleetTickMs() + kFleetRetryAfterFailureMs;
                }
                if (fsync)
                {
                    // A UI-thread commit's second half (PersistFleetNow): its frames are in the OS cache; this puts them on
                    // the disk. Nothing to do when the FULL commit above already did.
                    FleetSyncNormalCommits();
                }
                const int64_t now = FleetTickMs();
                if (now - lastSweep >= kFleetSweepEveryMs)
                {
                    lastSweep = now;
                    FleetSweepSlice();
                }
                if (db && mode == FleetMode::Sqlite)
                {
                    const uint64_t wal = db->WalBytes();
                    if (wal > kFleetCheckpointWalBytes || (wal > 0 && now - lastCheckpoint >= kFleetCheckpointEveryMs))
                    {
                        lastCheckpoint = now;
                        db->Checkpoint(false); // PASSIVE: never waits for a reader (the CLI, a test)
                    }
                    if (wal > kFleetLongWalBytes && !longWalLogged)
                    {
                        longWalLogged = true;
                        FleetWriterLog(L"[fleet] the WAL is " + std::to_wstring(wal / (1024 * 1024)) +
                                       L" MB: a reader holds a read transaction open, so checkpoints cannot get past it");
                    }
                    else if (wal <= kFleetCheckpointWalBytes)
                    {
                        longWalLogged = false;
                    }
                    const uint64_t commits = w.commits.load();
                    if (commits != summarized && now - lastSummary >= kFleetSummaryEveryMs)
                    {
                        lastSummary = now;
                        summarized = commits;
                        FleetWriterLog(L"[fleet] totals: " + std::to_wstring(commits) + L" commit(s), " + std::to_wstring(w.records.load()) + L" record(s), " +
                                       std::to_wstring(w.rowsWritten.load()) + L" row(s) written, " + std::to_wstring(w.rowsDeleted.load()) +
                                       L" deleted, " + std::to_wstring(w.failures.load()) + L" failure(s); WAL " + std::to_wstring(db->WalBytes() / 1024) + L" KB");
                    }
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the loop goes on (the marks stay; the next round retries).
                LogSwallowedException(L"FleetWriterLoop");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
}

namespace Agentmaster
{
    void StartFleetWriter(std::shared_ptr<SessionRegistry> registry) noexcept
    {
        auto& w = FleetW();
        if (w.started.exchange(true))
        {
            return;
        }
        try
        {
            {
                std::lock_guard lk{ w.registryMtx };
                w.registry = registry;
            }
            if (registry)
            {
                registry->SetRecordChangeSink([](const std::wstring& id) { NoteFleetRecordChanged(id); });
            }
            std::thread{ FleetWriterLoop }.detach(); // runs for the process's life (TerminateProcess ends it)
        }
        catch (...)
        {
            // Rule #18. Recovery: no thread — the critical commits still write on their callers' threads, and the marks
            // wait for them (logged so the missing routine commits are explained).
            LogSwallowedException(L"StartFleetWriter");
        }
    }

    void NoteFleetRecordChanged(const std::wstring& id) noexcept
    {
        auto& w = FleetW();
        {
            std::lock_guard lk{ w.dirtyMtx };
            try
            {
                if (w.dirty.empty())
                {
                    w.firstDirtyMs = FleetTickMs();
                }
                w.dirty.insert(id);
            }
            catch (...)
            {
                // std::bad_alloc: not marked; the rolling sweep finds the change within about a minute.
            }
        }
        w.cv.notify_one();
    }

    void PersistFleetNow() noexcept
    {
        auto& w = FleetW();
        if (w.bulk.load() > 0)
        {
            return; // the restore's Upsert loop: one batch at its end, not one commit per restored session
        }
        FleetFlushDirty(false); // NORMAL: committed into the OS cache at once — a kill loses nothing
        w.fsyncWanted.store(true); // the writer puts it on the disk right away
        w.cv.notify_one();
    }

    bool PersistFleetNowDurable() noexcept
    {
        if (CurrentFleetMode() == FleetMode::None)
        {
            // The store is not open: in the app only before the first window's restore opens it, when no session is live
            // and nothing can be sent (and in the harness, whose scheduler tests run with no store). The marks wait for the
            // open. Answering false here would refuse every send of a run whose open never happened (it threw, and the
            // restore barrier retries in the next window), which is worse than the window it closes.
            return true;
        }
        if (!FleetFlushDirty(true))
        {
            return false;
        }
        // The record may have been taken by a UI thread's NORMAL commit an instant earlier (then nothing was left for the
        // FULL one above): its frames must be on the disk too before the caller acts.
        return FleetSyncNormalCommits();
    }

    void FlushFleetAtAppClose() noexcept
    {
        FleetFlushDirty(true);
        FleetSyncNormalCommits();
        FleetMode mode = FleetMode::None;
        FleetDb* db = nullptr;
        GetFleetStore(mode, db);
        if (db && mode == FleetMode::Sqlite)
        {
            // The database file alone holds the fleet afterwards (the WAL folded in and emptied), which is also what a
            // backup copy of the file needs.
            db->Checkpoint(true);
        }
    }

    FleetBulkScope::FleetBulkScope() noexcept
    {
        FleetW().bulk.fetch_add(1);
    }

    FleetBulkScope::~FleetBulkScope()
    {
        if (FleetW().bulk.fetch_sub(1) == 1)
        {
            FleetW().cv.notify_all(); // the outermost scope: the writer commits what accumulated, as one batch
        }
    }

    bool FlushFleetDirtyForTests() noexcept
    {
        return FleetFlushDirty(true);
    }

    void BindFleetWriterRegistryForTests(std::shared_ptr<SessionRegistry> registry) noexcept
    {
        auto& w = FleetW();
        {
            std::lock_guard lk{ w.registryMtx };
            w.registry = registry;
        }
        if (registry)
        {
            registry->SetRecordChangeSink([](const std::wstring& id) { NoteFleetRecordChanged(id); });
        }
        FleetTakeDirty(); // a new test starts with no marks
    }

    size_t FleetDirtyCountForTests() noexcept
    {
        std::lock_guard lk{ FleetW().dirtyMtx };
        return FleetW().dirty.size();
    }
}
