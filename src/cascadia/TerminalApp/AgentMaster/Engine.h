// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — Engine: the ONE process-wide session-management engine (M9).
//
// v1.24 Windows Terminal is a WindowEmperor — every window lives in a single process on its
// own thread. The session engine must therefore be a PROCESS singleton, not a per-window
// (per-TerminalPage) object: there is exactly one SessionRegistry (the single source of
// truth), one HooksBridge (the local named pipe `\\.\pipe\agentmaster.<pid>` — the PID is
// unambiguous only because there is one bridge), and one Scheduler (Autopilot). Every
// window's Manager tab is an independent VIEW (lens) over this one shared fleet.
//
// Before M9 each TerminalPage stood up its own engine, so two windows raced two registries
// on one pipe and clobbered one sessions.json (last-writer-wins). SharedEngine() fixes that:
// the first window to ask constructs + wires + starts the engine; every later window receives
// the same instance.
//
// Plain C++ (no WinRT), like the rest of AgentMaster/. The wiring it performs (logging /
// scheduler / persistence observers, the bridge, bridge-discovery + shared hook files + the
// PATH shim for `+`-tab adoption) is exactly what TerminalPage::_InitAgentmasterEngine used
// to do inline — hoisted here so it happens once.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "SessionModels.h" // WindowRecord (M10 window-record claiming)

namespace Agentmaster
{
    class SessionRegistry;
    class HooksBridge;
    class Scheduler;
    class SessionScanner;

    // The shared engine's three long-lived owners. Held by the process singleton; windows copy
    // the shared_ptrs into their TerminalPage so the registry/bridge/scheduler outlive any one
    // window.
    struct Engine
    {
        std::shared_ptr<SessionRegistry> registry;
        std::shared_ptr<HooksBridge> bridge;
        std::shared_ptr<Scheduler> scheduler;
        std::shared_ptr<SessionScanner> scanner; // the interval reconciler (PULL; complements the bridge's PUSH)

        // Restore (loading sessions.json + re-launching the saved fleet) is a PROCESS-once
        // action — the registry is now shared, so if every window's _OnFirstLayout restored,
        // a second window would re-launch the same conversations into the one registry (dup
        // tabs + a blind `claude --resume` on an already-running id). The first window to
        // restore flips this; later windows skip. (M9; superseded by per-window records in M10.)
        std::atomic<bool> restored{ false };

        // M10 (window-record claiming; PERSISTENCE.md §13). Each window claims at most ONE
        // persisted WindowRecord at startup so two windows never adopt the same windowId and
        // clobber each other's windows/<id>.json. The set is loaded once (lazily, under the
        // mutex); ClaimWindowRecord() pops the next unclaimed record, and when empty a window
        // mints a fresh id. Forward-compatible with multi-window restore (each window claims a
        // distinct record).
        std::mutex windowMutex;
        bool windowRecordsLoaded{ false };
        std::vector<WindowRecord> unclaimedWindowRecords;
    };

    // The one process-wide engine. The FIRST call constructs it (creates the registry, wires
    // the logging / scheduler / persistence observers, starts the scheduler, opens the hooks
    // pipe, publishes bridge discovery + the shared hook files + the claude PATH shim) and
    // returns it; every later call returns the same instance. Thread-safe — windows run on
    // different threads, so this may be called concurrently; a function-local static makes the
    // construction race-free and happen exactly once. Intentionally never torn down (process
    // lifetime: the bridge/scheduler threads stop when the process exits), which also sidesteps
    // static-destruction-order hazards with the WinRT shutdown of the windows that observe it.
    Engine& SharedEngine();

    // M10: claim this window's persisted record (geometry + Manager lens + ordered tab refs), or
    // nullopt if none remains — in which case the window mints a fresh id. Pops from the shared
    // engine's unclaimed set under lock (loading windows/*.json once on first call). Each window
    // calls this exactly once at init. See PERSISTENCE.md §13.
    std::optional<WindowRecord> ClaimWindowRecord();

    // M10 Increment 3 (multi-window restore): claim a SPECIFIC record by windowId — the Emperor
    // assigns each restored window its record (via -s <idx> -> TerminalWindow -> TerminalPage), so
    // geometry and lens come from the same record with no cross-thread pop-order race. Removes the
    // matching record from the unclaimed set and returns it; nullopt if not present (already
    // claimed / absent) -> the window mints a fresh id. See PERSISTENCE.md §13.5.
    std::optional<WindowRecord> ClaimWindowRecord(const std::wstring& windowId);
}
