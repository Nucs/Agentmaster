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

namespace Agentmaster
{
    class SessionRegistry;
    class HooksBridge;
    class Scheduler;

    // The shared engine's three long-lived owners. Held by the process singleton; windows copy
    // the shared_ptrs into their TerminalPage so the registry/bridge/scheduler outlive any one
    // window.
    struct Engine
    {
        std::shared_ptr<SessionRegistry> registry;
        std::shared_ptr<HooksBridge> bridge;
        std::shared_ptr<Scheduler> scheduler;

        // Restore (loading sessions.json + re-launching the saved fleet) is a PROCESS-once
        // action — the registry is now shared, so if every window's _OnFirstLayout restored,
        // a second window would re-launch the same conversations into the one registry (dup
        // tabs + a blind `claude --resume` on an already-running id). The first window to
        // restore flips this; later windows skip. (M9; superseded by per-window records in M10.)
        std::atomic<bool> restored{ false };
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
}
