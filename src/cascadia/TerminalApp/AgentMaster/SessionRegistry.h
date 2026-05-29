// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — SessionRegistry: the single source of truth for all Claude Code
// sessions (DESIGN §6). The UI (M6) binds to it; the hooks bridge (HooksBridge) feeds it
// authoritative state; the Autopilot scheduler (M7) reacts to it.
//
// Pure C++ + the STL (no WinRT) so it is unit-testable standalone. The seam to the live
// terminal is an `Injector` callback (bound to the session's ConptyConnection by the app
// layer), which keeps WinRT out of the registry while still letting the scheduler write
// stdin (Correctness Rule #3: bind queue -> sessionId, never "the selected session").

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "HookEvents.h"
#include "SessionModels.h"

namespace Agentmaster
{
    // Notified after a hook mutates a session. Receives a snapshot (safe to read off any
    // thread) and the event that caused it. Invoked OUTSIDE the registry lock.
    using RegistryObserver = std::function<void(const SessionInfo& snapshot, HookEvent cause)>;

    // The Autopilot seam (M7): invoked when a clean `Stop` moves a session into
    // WaitingForInput. The handler decides whether to dequeue + inject the next prompt.
    using AdvanceHandler = std::function<void(const std::wstring& sessionId)>;

    // Per-session stdin writer, bound to that session's ConptyConnection by the app layer.
    using Injector = std::function<void(const std::wstring& text)>;

    class SessionRegistry
    {
    public:
        SessionRegistry() = default;
        SessionRegistry(const SessionRegistry&) = delete;
        SessionRegistry& operator=(const SessionRegistry&) = delete;

        // Add a new session or replace an existing one with the same id. Returns the id.
        std::wstring Upsert(SessionInfo info);
        void Remove(const std::wstring& id);

        bool Contains(const std::wstring& id) const;
        std::optional<SessionInfo> Get(const std::wstring& id) const;
        std::vector<SessionInfo> Snapshot() const; // ordered by insertion
        size_t Count() const;

        // The single entry point that applies an authoritative hook event and mutates
        // session state (DESIGN §7-8). Thread-safe. Fires the observer (always) and the
        // advance handler (only on a clean Stop -> WaitingForInput), both outside the lock.
        void OnHookEvent(const HookMessage& msg);

        // Mutate a session's Flight Plan / autopilot under the lock (used by the scheduler
        // and UI). The mutator runs while holding the lock; the post-change snapshot is
        // delivered to the observer afterwards. Returns false if the id is unknown.
        bool Update(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate);

        // Wiring. Multiple observers may register (e.g. a logger, the Triage Board UI, the
        // scheduler); each is invoked on every change, outside the lock. The advance handler
        // is single (the Autopilot).
        void AddObserver(RegistryObserver observer);
        void SetAdvanceHandler(AdvanceHandler handler);

        // Bind / clear a session's stdin injector.
        void SetInjector(const std::wstring& id, Injector injector);
        // Inject text into a session via its bound injector. Returns false if none bound.
        bool Inject(const std::wstring& id, const std::wstring& text) const;

        // pauseOnHumanInput support: record the last time the human typed into a session.
        void NoteHumanInput(const std::wstring& id, int64_t unixMs);
        int64_t LastHumanInputUnixMs(const std::wstring& id) const;

    private:
        // Snapshot the observer list under the lock, then invoke each outside it.
        void _notify(const SessionInfo& snapshot, HookEvent cause);

        mutable std::mutex _mtx;
        // insertion-ordered storage so the UI shows a stable order
        std::vector<std::wstring> _order;
        std::unordered_map<std::wstring, SessionInfo> _sessions;
        std::unordered_map<std::wstring, Injector> _injectors;
        std::unordered_map<std::wstring, int64_t> _lastHumanInput;
        std::vector<RegistryObserver> _observers;
        AdvanceHandler _advance;
    };
}
