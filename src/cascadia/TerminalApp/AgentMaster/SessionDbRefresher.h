// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the APP's half of the session database (SESSIONS_DB.md §7.6, D16, decided by the user on
// 2026-10-14): "Duckdb will be refreshed by counting changes from the observer and if we had changes within
// the refresh rate range then we update or rather upsert."
//
// While AppSettings::sessionDbMode is Shadow or On, one thread:
//   - starts the profile's service (copying agentmaster-sessiondb.exe and duckdb.dll out of the package into
//     <profile>\sessiondb\bin first, §4.2) and keeps ONE connection open — which also keeps the service alive;
//   - every refresh tick (AppSettings::sessionDbRefreshSeconds, default 5 s) takes the sessions the transcript
//     watcher saw change (SessionScanner::TakeChangedTranscripts) and sends them as ONE `upsertSessions`;
//     a failed tick keeps them for the next one;
//   - sends a `resync` (a sweep) after it connects, after the watcher re-indexed (its epoch moved), every 30 s
//     while the watch is unhealthy, and when the Sessions page asks (↻) — on a second connection, so a long
//     sweep (the cold build) never holds the tick's upserts back;
//   - pushes Agentmaster's own facts when they change (`factsChanged`: titles, tags, favorites — the session
//     store's facts epoch moved — and every 5 minutes for the /clear and /handover links, which live in
//     sessions.json) and the set of open sessions when it changes (`openChanged`).
// It never reads DuckDB: the GUI process never loads duckdb.dll (§4.2).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "SessionModels.h" // AppSettings, SessionDbMode

namespace Agentmaster
{
    class SessionRegistry;
    class SessionScanner;
    namespace SessionDb
    {
        class ServiceClient; // SessionDbClient.h (the destructor, defined in the .cpp, sees the complete type)
    }

    class SessionDbRefresher
    {
    public:
        // scanner    — owns the transcript watcher whose change feed this drains (must outlive Stop()).
        // registry   — the open-session set comes from its live sessions.
        // profileDir — the active profile (the service, its pipe and its files derive from it).
        SessionDbRefresher(std::shared_ptr<SessionScanner> scanner, std::shared_ptr<SessionRegistry> registry, std::wstring profileDir);
        // Stop(). Never throws.
        ~SessionDbRefresher();
        SessionDbRefresher(const SessionDbRefresher&) = delete;
        SessionDbRefresher& operator=(const SessionDbRefresher&) = delete;

        // Apply the settings: Shadow / On start the thread (idempotent), Off stops it; the refresh interval
        // applies from the next tick. Called at engine init and on every Settings save. Never throws (a thread
        // that cannot start is logged).
        void Apply(const AppSettings& settings) noexcept;
        // Stop the thread (and the sweep's), switch the watcher's change set off. The service is left to
        // exit by itself after 10 minutes without a client. Idempotent. Never throws.
        void Stop() noexcept;

        // One of Agentmaster's own facts changed (a title, a tag, a favorite, a /clear or /handover link): the
        // next tick sends `factsChanged`. Any thread; cheap.
        void NoteFactsChanged() noexcept;
        // Ask for a sweep on the next tick (the Sessions page's ↻). Any thread; cheap.
        void RequestResync() noexcept;
        // True once the service answered a first complete sweep in this run: from then on its answers cover
        // every session (the Sessions page may switch to it). Any thread.
        bool Ready() const noexcept
        {
            return _ready.load();
        }
        // The current mode (Off when the thread does not run). Any thread.
        SessionDbMode Mode() const noexcept
        {
            return static_cast<SessionDbMode>(_mode.load());
        }
        // One line for the Settings cog: what the refresher last did or why it waits. Any thread.
        std::wstring StatusText() const;

    private:
        void _run() noexcept;
        void _tick();
        void _startSweep();
        void _setStatus(std::wstring text);

        std::shared_ptr<SessionScanner> _scanner;
        std::shared_ptr<SessionRegistry> _registry;
        std::wstring _profile;

        std::thread _thread;
        // The sweep (`resync`) in flight: its own thread and connection, owned through this shared state so
        // Stop() can abort its call and let the thread end by itself (detached) instead of waiting out a long
        // sweep. The tick reads the outcome from it.
        struct SweepState;
        std::shared_ptr<SweepState> _sweep;
        mutable std::mutex _mtx; // guards the wait and _status
        // Guards the two pointers Stop() reads from another thread (_client, _sweep): the tick thread swaps them.
        mutable std::mutex _ptrMtx;
        std::condition_variable _cv;
        std::atomic<bool> _stop{ false };
        std::atomic<bool> _running{ false };
        std::atomic<int> _mode{ 0 }; // SessionDbMode
        std::atomic<int> _refreshSeconds{ 5 };
        std::atomic<bool> _factsDirty{ true }; // the first tick sends the facts once
        std::atomic<bool> _resyncWanted{ true }; // the first tick after a connect sweeps
        std::atomic<bool> _ready{ false };

        // Ticker-thread state.
        std::unique_ptr<SessionDb::ServiceClient> _client; // the tick's connection (it also keeps the service alive)
        uint64_t _lastEpoch{ 0 };
        bool _epochKnown{ false };
        int64_t _lastUnhealthySweepTick{ 0 };
        int64_t _lastConnectFailTick{ 0 };
        std::unordered_map<std::wstring, std::wstring> _pending; // id -> transcript path, kept across failed ticks
        std::unordered_set<std::wstring> _lastOpen; // the open set the service last got
        uint64_t _lastFactsEpoch{ ~0ull }; // SessionFactsEpoch when the facts were last sent (~0: never)
        int64_t _lastFactsTick{ 0 }; // when the facts were last sent (the links' catch-all)
        std::wstring _status;
    };
}
