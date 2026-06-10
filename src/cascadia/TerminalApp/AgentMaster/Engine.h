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
#include <set>
#include <string>
#include <vector>

#include "SessionModels.h" // WindowRecord (M10 window-record claiming)

namespace Agentmaster
{
    class SessionRegistry;
    class HooksBridge;
    class Scheduler;
    class SessionScanner;
    class ProcessObserver;

    // The shared engine's three long-lived owners. Held by the process singleton; windows copy
    // the shared_ptrs into their TerminalPage so the registry/bridge/scheduler outlive any one
    // window.
    struct Engine
    {
        std::shared_ptr<SessionRegistry> registry;
        std::shared_ptr<HooksBridge> bridge;
        std::shared_ptr<Scheduler> scheduler;
        std::shared_ptr<SessionScanner> scanner; // the interval reconciler (PULL; complements the bridge's PUSH)
        std::shared_ptr<ProcessObserver> observer; // the Fleet Observer S-lane (PULL census/correlation; OBSERVER.md §8)

        // Agentmaster (Fleet Observer, OBSERVER.md §7): the per-PROCESS ownership stamp. Minted once
        // in SharedEngine() and exported as the AM_SESSION env var on our process, so every ConPTY
        // child carries it — a Manager-Launched claude AND a hand-typed `+`-tab claude both inherit
        // our env block (reloadEnvironmentVariables is forced OFF; see SharedEngine). The
        // ProcessObserver (S-lane) reads a claude's AM_SESSION to classify RunningApp: == ours ->
        // Agentmaster (correlate + bind); a bare WT_SESSION with no/foreign AM_SESSION ->
        // WindowsTerminal/Other (external, observe-only). Per-process GUID, so two Agentmaster
        // instances stay cleanly separable (each binds only its own).
        std::wstring amSession;

        // Restore (loading sessions.json + re-launching the saved fleet) is a PROCESS-once
        // action — the registry is now shared, so if every window's _OnFirstLayout restored,
        // a second window would re-launch the same conversations into the one registry (dup
        // tabs + a blind `claude --resume` on an already-running id). The first window to
        // restore flips this; later windows skip. (M9; superseded by per-window records in M10.)
        std::atomic<bool> restored{ false };

        // Agentmaster: the load BARRIER. `restored` alone (a flag the first window flips) lets a SECOND
        // window's _RestoreClaudeSessions return early — but that window then races its _RestoreWindowTabs
        // (which runs immediately after, on its own thread) against a still-loading registry: it sees its
        // sessions as "unknown", skips them all, and the end-of-startup flush writes an EMPTY record over
        // its saved workspace. This mutex turns the load into a real barrier — the loader holds it for the
        // WHOLE LoadSessions + Upsert pass, and a concurrent window BLOCKS on it until the fleet is fully
        // populated, then sees restored==true and proceeds. Guards only `restored` + the one-time load.
        std::mutex restoreMutex;

        // M10 (window-record claiming; PERSISTENCE.md §13). Each window claims at most ONE
        // persisted WindowRecord at startup so two windows never adopt the same windowId and
        // clobber each other's windows/<id>.json. The set is loaded once (lazily, under the
        // mutex); ClaimWindowRecord() pops the next unclaimed record, and when empty a window
        // mints a fresh id. Forward-compatible with multi-window restore (each window claims a
        // distinct record).
        std::mutex windowMutex;
        bool windowRecordsLoaded{ false };
        std::vector<WindowRecord> unclaimedWindowRecords;

        // M10 Increment 3 (in-session re-claim; PERSISTENCE.md §13.5). Records that were CLAIMED
        // earlier this session and then CLOSED — returned here (loaded from disk) by
        // UnregisterLiveWindow. The startup pool above only ever shrinks, so without this a window
        // reopened DURING a session (the "Reopen Windows" recover button) could not re-claim its record
        // and would mint a fresh, lens-less duplicate, orphaning the original + never clearing it from
        // the recoverable set. Drawn from by id ONLY (ClaimWindowRecord(windowId) searches both pools);
        // the no-arg front-pop claim never touches it, so a plain "+ new window" never silently adopts a
        // closed window's geometry/lens. Cross-session is unaffected (the next launch reloads the
        // startup pool from disk). Guarded by windowMutex.
        std::vector<WindowRecord> reclaimableWindowRecords;

        // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5). The set of windowIds with a
        // LIVE window in THIS process right now. Each TerminalPage registers its id at engine init and
        // unregisters at teardown; every change rewrites open-windows.json (the manifest the next run's
        // WindowEmperor reads to auto-reopen exactly the last-open windows) — EXCEPT a change that
        // empties the set is NOT written, so the file preserves the final "open at exit" snapshot rather
        // than being cleared by the last window's teardown. Guarded by windowMutex (same lock as the
        // record-claim set; the two are touched at disjoint times so there is no re-entrancy).
        std::set<std::wstring> liveWindowIds;
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

    // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5). Register/unregister this window's
    // id in the process-wide live set and refresh open-windows.json. RegisterLiveWindow adds + writes;
    // UnregisterLiveWindow removes + writes UNLESS the set is now empty (skip-empty keeps the last
    // snapshot = "windows open at exit"). LiveWindowIds() snapshots the set — used by the Manager's
    // "Reopen Windows (N)" recover button to filter records to those NOT currently open.
    void RegisterLiveWindow(const std::wstring& windowId);
    void UnregisterLiveWindow(const std::wstring& windowId);
    std::vector<std::wstring> LiveWindowIds();

    // M10 Increment 3 (recover button). A saved window record that is NOT currently open, paired with
    // its `-s <idx>` (its index in the canonical sorted LoadWindowRecords order) so the Manager can
    // reopen it the same way the Emperor does at startup (wt -w -1 -s <idx>). See PERSISTENCE.md §13.5.
    struct RecoverableWindow
    {
        int index{ 0 };
        WindowRecord record;
    };
    std::vector<RecoverableWindow> RecoverableWindows();
}
