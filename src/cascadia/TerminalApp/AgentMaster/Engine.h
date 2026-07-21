// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Engine: the ONE process-wide session-management engine (M9).
//
// v1.24 Windows Terminal is a WindowEmperor — every window lives in a single process on its
// own thread. The session engine must therefore be a PROCESS singleton, not a per-window
// (per-TerminalPage) object: there is exactly one SessionRegistry (the single source of
// truth), one HooksBridge (the local named pipe `\\.\pipe\agentmaster.<pid>` — the PID is
// unambiguous only because there is one bridge), and one Scheduler (Autorunner). Every
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
#include <cstdint>
#include <functional>
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
    class CommandWatch; // COMMANDS.md — slash-command bindings + awaited follow-up activity

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
        std::shared_ptr<CommandWatch> commandWatch; // slash-command bindings + async awaits, fed by the scanner (COMMANDS.md)

        // Agentmaster (Fleet Observer, OBSERVER.md §7): the per-PROCESS ownership stamp. Minted once
        // in SharedEngine() and exported as the AM_SESSION env var on our process, so every ConPTY
        // child carries it — a Manager-Launched claude AND a hand-typed `+`-tab claude both inherit
        // our env block (reloadEnvironmentVariables is forced OFF; see SharedEngine). The
        // ProcessObserver (S-lane) reads a claude's AM_SESSION to classify RunningApp: == ours ->
        // Agentmaster (correlate + bind); a bare WT_SESSION with no/foreign AM_SESSION ->
        // WindowsTerminal/Other (external, observe-only). Per-process GUID, so two Agentmaster
        // instances stay cleanly separable (each binds only its own).
        std::wstring amSession;

        // Agentmaster (native-exe-only policy). The NATIVE claude.exe to launch, resolved at engine
        // init via ResolveClaudeExe(settings.claudeExePath) and re-resolved by RefreshClaudeExe when
        // the Settings override changes. ALWAYS a real claude.exe full path, or EMPTY when none is
        // found anywhere (PATH claude.exe / ~/.local/bin / a claude.cmd's npm binary / the override).
        // Empty == "Claude not detected": the app GATES every claude interaction (launch / new / fork
        // / resume / adopt) behind the install prompt. Threaded into the Launch/Restore command line
        // by full path (BuildClaudeSpawn -> BuildClaudeCommandline). ConPTY's CreateProcessW appends
        // only ".exe" and ignores PATHEXT, so launching by full path is mandatory (a bare `claude`
        // would miss the npm install and die 0x80070002). UI-thread-owned (set at init + on Settings
        // save); gating reads it through ClaudeAvailable().
        std::wstring claudeExePath;

        // Agentmaster (Codex managed-session support). The codex launcher to spawn, resolved once at
        // engine init via ResolveCodexLauncher(): the full path to codex.exe / codex.cmd / codex.bat
        // (on PATH or ~/.local/bin), or EMPTY when none is found. Threaded into the managed-Codex
        // launch command line by full path (BuildCodexCommandline) — ConPTY's CreateProcessW appends
        // only ".exe" and ignores PATHEXT, so a bare `codex` token would miss an npm codex.cmd and die
        // 0x80070002 (ERROR_FILE_NOT_FOUND). Unlike claudeExePath there is NO native-exe-only gate: the
        // Fleet Observer finds codex.exe as a descendant, so a .cmd/.bat (run via `cmd /c`) is fine, and
        // empty just means the spawn falls back to the bare token and surfaces the not-found error.
        std::wstring codexExePath;

        // Agentmaster. The PowerShell host that wraps every managed agent (Claude/Codex) session, so
        // quitting the agent (Ctrl+C / /exit) drops to a live `PS <cwd>>` prompt at the working dir
        // instead of the ConPTY root dying into a dead "press Enter to restart" pane (see
        // BuildPwshHostedCommandline). Resolved ONCE at engine init via ResolvePwshLauncher(): the full
        // path to pwsh.exe (PowerShell 7) on PATH, else Windows PowerShell under System32; EMPTY only if
        // neither is found (the launch then falls back to the bare `pwsh.exe` token).
        std::wstring pwshExePath;

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

        // Agentmaster (discard Manager-only windows): the windowIds that have RESERVED a Manager-only
        // self-close (ReserveManagerOnlyClose returned true for them) but haven't finished tearing down
        // yet. A window that ends up holding ONLY the pinned Manager tab self-closes UNLESS it is the
        // last Agentmaster window — but several windows can become Manager-only on their own threads at
        // once, and each one reading liveWindowIds.size()>1 independently would let them ALL close,
        // quitting the app. The reservation serializes that decision under windowMutex: the effective
        // remaining-live count subtracts the already-reserved ids, so the last window to ask sees
        // remaining==1 and STAYS. Cleared in UnregisterLiveWindow (the close completes) — a lingering
        // reservation only ever makes OTHER windows MORE likely to stay (the conservative direction).
        std::set<std::wstring> closingWindowIds;

        // Agentmaster (cross-window activate; Linked Lenses): per-window "focus this session's tab"
        // sinks. The Manager's Triage Board / Explorer Tree (GLOBAL) show the WHOLE fleet, but a
        // session's tab lives in exactly ONE window (its _claudeTabs) — so Activate (board/tree
        // double-click, tree Enter, the Auto Testing's eye) on a session hosted ELSEWHERE must reach
        // that window. Each TerminalPage registers a sink at engine init — "select this session's
        // tab + bring your window to the foreground if YOU host it"; the sink hops to its own UI
        // thread and no-ops on a miss — and detaches it at teardown (Rule #10). Guarded by its own
        // mutex, taken only to snapshot/mutate the list; sinks are invoked OUTSIDE it (the
        // registry's _notify pattern), since each one marshals into a window dispatcher.
        struct WindowActivateSink
        {
            uint64_t token{ 0 };
            std::wstring windowId;
            std::function<void(const std::wstring& sessionId)> fn;
        };
        std::mutex activateMutex;
        std::vector<WindowActivateSink> activateSinks;
        uint64_t nextActivateToken{ 1 };

        // Agentmaster (cross-window restart): per-window "restart this session's connection in place"
        // sinks — same shape + lifetime as activateSinks. A managed Claude/Codex session's "Restart
        // session" (offered on the Triage Board card / Explorer-tree row, mirroring the WT tab menu)
        // rebuilds the LIVE ConPTY connection, which can only happen in the window that HOSTS the tab.
        // The board's GLOBAL scope shows the whole fleet, so a restart there must reach the hosting
        // window: each TerminalPage registers a sink at engine init ("restart this session if YOU host
        // its tab; no-op on a miss") and detaches it at teardown (Rule #10). Snapshot-under-lock /
        // invoke-outside-it, like activateSinks (each sink marshals into its own window's dispatcher).
        struct WindowRestartSink
        {
            uint64_t token{ 0 };
            std::wstring windowId;
            std::function<void(const std::wstring& sessionId)> fn;
        };
        std::mutex restartMutex;
        std::vector<WindowRestartSink> restartSinks;
        uint64_t nextRestartToken{ 1 };

        // Agentmaster (cross-window settings broadcast): per-window "global settings changed" sinks.
        // The Settings cog AND the Explorer-Tree / Triage-Board sort toggles all write the GLOBAL
        // AppSettings (settings.json) from whichever window the user is in. To keep every OPEN window in
        // sync LIVE (not only on its next launch), the changing window broadcasts the merged settings
        // through here; each OTHER window's sink hops to its own UI thread and re-applies them (its
        // _appSettings + the sort toggles + a board/tree re-sort). Same shape + lifetime as activateSinks:
        // registered at engine init, token-detached in ~TerminalPage (Rule #10), snapshot-under-lock /
        // invoke-outside-it (each sink marshals into a foreign window's dispatcher).
        struct SettingsSink
        {
            uint64_t token{ 0 };
            std::wstring windowId;
            std::function<void(const AppSettings& settings)> fn;
        };
        std::mutex settingsMutex;
        std::vector<SettingsSink> settingsSinks;
        uint64_t nextSettingsToken{ 1 };

        // Agentmaster (eager-init / "Activate All Tabs"): per-window "wake all your DORMANT managed tabs"
        // sinks. A dormant tab (window-restored / re-homed, never focused) can only be started in the
        // window that HOSTS it (its TermControl lives on that window's UI thread). The Manager's "Activate
        // All Tabs (N)" can target the whole fleet (its board shows every window's sessions), so the
        // request must reach every window: each TerminalPage registers a sink at engine init ("eager-init
        // all of YOUR dormant controls") and detaches it at teardown (Rule #10). Same shape + lifetime as
        // activateSinks; snapshot-under-lock / invoke-outside-it (each sink hops to its own UI thread).
        struct ActivateAllSink
        {
            uint64_t token{ 0 };
            std::wstring windowId;
            std::function<void()> fn;
        };
        std::mutex activateAllMutex;
        std::vector<ActivateAllSink> activateAllSinks;
        uint64_t nextActivateAllToken{ 1 };

        // Agentmaster (COMMANDS.md — command actions): per-window "a bound slash command's await
        // resolved" sinks. A CommandWatch binding fires on the SCANNER thread with a session id +
        // a payload (the /handover binding's payload is the handover markdown's path); the ACTION
        // — spawn the successor tab beside the origin — can only run in the window that HOSTS the
        // origin session's tab. The watch's engine-level binding fans out here to EVERY window
        // (there is no source window to exclude — the fire originates off-window); each sink hops
        // to its own UI thread, checks its _claudeTabs, and only the (single) host acts. Same
        // shape + lifetime as activateSinks: registered at engine init, token-detached in
        // ~TerminalPage (Rule #10), snapshot-under-lock / invoke-outside-it.
        struct CommandActionSink
        {
            uint64_t token{ 0 };
            std::wstring windowId;
            // sessionId · the canonical command action ("handover"/"handover-here") · the fired
            // await's payload (the '|'-joined md path set) · the typed command's ARGS verbatim
            // (the §6b per-message model hint parses out of their leading words at action time).
            std::function<void(const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args)> fn;
        };
        std::mutex commandActionMutex;
        std::vector<CommandActionSink> commandActionSinks;
        uint64_t nextCommandActionToken{ 1 };
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

    // Agentmaster (native-exe-only policy). True iff a native claude.exe was resolved (== the engine
    // may launch/fork/resume) — i.e. SharedEngine().claudeExePath is non-empty. False => "Claude not
    // detected", and the Manager gates every claude interaction behind the install prompt. Reads the
    // value cached at engine init / last RefreshClaudeExe — it does NOT re-scan (use the auto-recover
    // gate EnsureClaudeAvailable() below for that).
    bool ClaudeAvailable();

    // Agentmaster: the AUTO-RECOVERING gate for every UI launch path. True if a native claude.exe is
    // already cached; otherwise re-resolves ONCE (honoring the Settings override) and reports whether
    // one is now present. This is what lets a claude installed WHILE Agentmaster is running clear the
    // gate on the user's next attempt — no manual Re-check, no restart — so the "Claude not detected"
    // prompt stops popping and the action just proceeds. The re-scan cost is paid only on the
    // not-detected path (the cache-hit case is a cheap emptiness check). UI-thread-only, like
    // RefreshClaudeExe (it is the only other writer of claudeExePath after init).
    bool EnsureClaudeAvailable();

    // Re-resolve the native claude.exe with a (possibly new) explicit override — called when the
    // Settings cog's claude-path override is saved, so a Browse/override takes effect WITHOUT a
    // restart. Updates SharedEngine().claudeExePath and returns it ("" => still not detected).
    std::wstring RefreshClaudeExe(std::wstring_view overridePath);

    // Agentmaster (native-exe-only policy — the "Claude not detected" install prompt). Which install
    // situation the user is in, so the not-found UI can offer the RIGHT one-click fix:
    //   Native    — a native claude.exe is resolved (the prompt never shows; here only defensively).
    //   LegacyNpm — no native .exe, but a real npm/Node `claude.cmd`/`.bat` is on PATH: the fix is
    //               `<that launcher> install` (npm -> native migration; claude is already on PATH).
    //   None      — no claude at all: the fix is the official claude.ai native bootstrap.
    enum class ClaudeInstallState
    {
        Native,
        LegacyNpm,
        None,
    };

    // Classify the current install situation (reads the cached claudeExePath — the not-found gates that
    // call this have already re-resolved via EnsureClaudeAvailable — then a fresh PATH scan for a real
    // npm launcher, excluding our shim). Cheap; UI-thread-called at prompt/settings render time.
    ClaudeInstallState ClaudeInstallKind();

    // Open a VISIBLE PowerShell window that runs the official install/migrate command for `state`, so the
    // user watches it + can answer any prompts, then returns to Agentmaster and clicks Re-check (the gate
    // auto-recovers on the next launch attempt anyway). LegacyNpm -> `& '<npm launcher>' install` (run by
    // full path, bypassing our --settings shim); None (and the defensive default) -> `irm
    // https://claude.ai/install.ps1 | iex` (the official claude.ai bootstrap — the ONLY online source, per
    // the setup docs). Not elevated (the installer needs no admin). Best-effort; returns false if the
    // shell couldn't be launched. Pure Win32 (ShellExecuteW) — safe to call from the UI thread.
    bool LaunchClaudeInstall(ClaudeInstallState state);

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

    // Agentmaster (discard Manager-only windows): race-safe "may THIS window self-close because it is
    // now Manager-only?" Returns true (and reserves the close) iff more than one live Agentmaster window
    // would remain after it goes — counting live windows MINUS those that have already reserved a close
    // this tick (see closingWindowIds), so two windows that become Manager-only simultaneously on
    // different threads never both close and leave the app window-less. Returns false for the LAST
    // window, which then stays open (but discards its record so it isn't restored as Manager-only).
    bool ReserveManagerOnlyClose(const std::wstring& windowId);

    // Agentmaster (cross-window activate): register THIS window's activate sink (returns a
    // monotonic token; detach with UnregisterWindowActivateHandler — removing a stale token is a
    // no-op, the registry-token pattern). ActivateSessionInOtherWindows fans `sessionId` out to
    // every registered sink EXCEPT `sourceWindowId`'s (the caller already checked its own
    // _claudeTabs): exactly one window hosts a session's tab (cross-window moves evict the old
    // binding), so at most one sink acts; with no host anywhere (archived / mid-bind) every sink
    // misses and the call is a no-op, matching the old local-only behavior.
    uint64_t RegisterWindowActivateHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler);
    void UnregisterWindowActivateHandler(uint64_t token);
    void ActivateSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId);

    // Agentmaster (eager-init / "Activate All Tabs"): register THIS window's "wake all dormant tabs" sink
    // (monotonic token; detach with UnregisterActivateAllDormantHandler — removing a stale token is a
    // no-op, the registry-token pattern). ActivateAllDormantInOtherWindows fans the request out to every
    // registered sink EXCEPT `sourceWindowId`'s (the caller already woke its own tabs), so the Manager's
    // fleet-wide "Activate All Tabs" reaches every other open window; each sink hops to its own UI thread
    // and eager-inits its dormant controls. Snapshot-under-lock / invoke-outside, like the activate sink.
    uint64_t RegisterActivateAllDormantHandler(const std::wstring& windowId, std::function<void()> handler);
    void UnregisterActivateAllDormantHandler(uint64_t token);
    void ActivateAllDormantInOtherWindows(const std::wstring& sourceWindowId);

    // Agentmaster (COMMANDS.md — command actions): register THIS window's command-action sink
    // (monotonic token; detach with UnregisterCommandActionHandler — removing a stale token is a
    // no-op, the registry-token pattern). RaiseCommandActionInWindows fans (sessionId, command,
    // payload, args) out to EVERY registered sink — the fire originates on the engine's scanner
    // thread, so there is no source window to exclude; exactly one window hosts the session's tab,
    // so at most one sink acts (each hops to its own UI thread and checks its _claudeTabs; a miss
    // is a no-op). Commands: "handover"/"handover-here" (payload == the '|'-joined absolute md
    // path set; args == the typed command's <command-args> verbatim — the §6b per-message model
    // hint parses out of their leading words at action time, PickModelFromArgsHint).
    uint64_t RegisterCommandActionHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args)> handler);
    void UnregisterCommandActionHandler(uint64_t token);
    void RaiseCommandActionInWindows(const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args = {});

    // Agentmaster (cross-window restart): register THIS window's restart sink (monotonic token; detach
    // with UnregisterWindowRestartHandler — removing a stale token is a no-op, the registry-token
    // pattern). RestartSessionInOtherWindows fans `sessionId` out to every registered sink EXCEPT
    // `sourceWindowId`'s (the caller already tried its own _claudeTabs): exactly one window hosts a
    // session's tab, so at most one sink acts; with no host anywhere (archived / mid-bind) every sink
    // misses and the call is a no-op. Used by the Triage Board / Explorer-tree "Restart session".
    uint64_t RegisterWindowRestartHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler);
    void UnregisterWindowRestartHandler(uint64_t token);
    void RestartSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId);

    // Agentmaster (cross-window settings broadcast): register THIS window's settings sink (monotonic
    // token; detach with UnregisterSettingsChangedHandler — removing a stale token is a no-op, the
    // registry-token pattern). BroadcastSettingsChanged fans the merged AppSettings out to every
    // registered sink EXCEPT `sourceWindowId`'s (the source window already applied + persisted it), so a
    // GLOBAL settings change — the cog Save, or the Explorer-Tree / Triage-Board sort toggle — reaches
    // every OTHER open window LIVE instead of only on its next launch. Each sink marshals onto its own
    // window's UI thread; snapshot-under-lock / invoke-outside, like ActivateSessionInOtherWindows.
    uint64_t RegisterSettingsChangedHandler(const std::wstring& windowId, std::function<void(const AppSettings& settings)> handler);
    void UnregisterSettingsChangedHandler(uint64_t token);
    void BroadcastSettingsChanged(const AppSettings& settings, const std::wstring& sourceWindowId);

    // M10 Increment 3 (recover button). A saved window record that is NOT currently open, paired with
    // its `-s <idx>` (its index in the canonical sorted LoadWindowRecords order) so the Manager can
    // reopen it the same way the Emperor does at startup (wt -w -1 -s <idx>). See PERSISTENCE.md §13.5.
    // Records with NO tab refs (a window that held only the Manager tab) are EXCLUDED — reopening one
    // reconstructs exactly what "+ new window" gives, so it is never offered; UnregisterLiveWindow
    // additionally DELETES such a record on a mid-session close (the last window out keeps its —
    // possibly empty — record: it is the open-at-exit snapshot the next launch claims for geometry).
    struct RecoverableWindow
    {
        int index{ 0 };
        WindowRecord record;
    };
    std::vector<RecoverableWindow> RecoverableWindows();

    // Agentmaster (test seam — the SessionStore `...In` idiom): the window-record / manifest /
    // reserve primitives above, parameterized on the Engine INSTANCE. The public functions delegate
    // here with SharedEngine(); the standalone harness (tests/run-m5-tests.bat links Engine.cpp)
    // pins the PERSISTENCE.md §13.5 / Rule #16 invariants — claim-by-id vs front-pop, the
    // open-at-exit manifest's skip-empty rule, the reclaimable pool, Manager-only record deletion,
    // ReserveManagerOnlyClose's last-window count — on a LOCAL Engine value, because
    // SharedEngine()'s first access wires + STARTS the bridge/observer/scheduler (in the test
    // process the bridge would collide with the harness's own HooksBridge round-trip on the same
    // `\\.\pipe\agentmaster.<pid>` name). Production code keeps calling the SharedEngine() forms.
    std::optional<WindowRecord> ClaimWindowRecordIn(Engine& e);
    std::optional<WindowRecord> ClaimWindowRecordIn(Engine& e, const std::wstring& windowId);
    void RegisterLiveWindowIn(Engine& e, const std::wstring& windowId);
    void UnregisterLiveWindowIn(Engine& e, const std::wstring& windowId);
    std::vector<std::wstring> LiveWindowIdsIn(Engine& e);
    bool ReserveManagerOnlyCloseIn(Engine& e, const std::wstring& windowId);
    std::vector<RecoverableWindow> RecoverableWindowsIn(Engine& e);
}
