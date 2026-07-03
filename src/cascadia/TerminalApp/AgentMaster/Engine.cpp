// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing). The
// process-singleton wiring is exactly what TerminalPage::_InitAgentmasterEngine did inline
// before M9; hoisted here so it runs once for the whole WindowEmperor process.
#include "Engine.h"

#include "ClaudeSpawn.h"
#include "HookWire.h"
#include "HooksBridge.h"
#include "Persistence.h"
#include "ProcessObserver.h"
#include "ProfileBootstrap.h" // Profiles::IsDevPackage — Auto Testing / Tests Autorunner is a DEV-ONLY feature
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionScanner.h"
#include "SessionStore.h"
#include "StartupTiming.h" // [startup] phase timing — find where launch spends its time

#include <windows.h>

#include <cstdio>
#include <mutex> // Agentmaster: guards the log observer's per-id dedup map (notify fires from several threads)
#include <random>
#include <string>
#include <unordered_map> // Agentmaster: per-id last-logged [Unknown] line, to dedup the pull/transient notify flood

namespace Agentmaster
{
    Engine& SharedEngine()
    {
        // Function-local static: C++ guarantees this initializer runs exactly once even if two
        // window threads call SharedEngine() concurrently. Heap-allocated and never deleted on
        // purpose (process lifetime) — see Engine.h.
        static Engine* const g = []() -> Engine* {
            // [startup] timing: this process-once wiring runs on the FIRST window's _InitAgentmaster-
            // Engine, so its cost lands inside that window's "engine-init" phase. Break it down here —
            // the Resolve* PATH scans + the file materializations are the usual launch-time hogs.
            Startup::ScopedPhase _wiring{ L"shared-engine wiring (process-once)" };
            auto* e = new Engine{};
            e->registry = std::make_shared<SessionRegistry>();

            // Per-startup tab-color randomization + one-time dir-colors migration (Agentmaster). A
            // fresh random seed each launch re-rolls the auto palette assignment (so the fleet looks
            // different every run, and two open dirs never share a color — collision-avoided in
            // AssignDirAutoColor), and the migration upgrades a v1 dir-colors.json (which mixed user
            // picks with the old colliding auto colors) to v2 = user picks only. Process-global state.
            {
                Startup::ScopedPhase _t{ L"  seed+migrate dir-colors" };
                std::random_device rd;
                SeedDirColors((static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()));
                MigrateDirColorsToV2IfNeeded();
            }

            // Agentmaster (ENV_VARS.md §8): one-time shipped defaults, for NEW installs AND updaters
            // alike. CLAUDE_CODE_MAX_RETRIES=50000 is appended to the global env, and cleanupPeriodDays=
            // 36500 is written into the user's ~/.claude/settings.json (so Claude never purges global
            // history). Both are marker-gated, so a user who edits or deletes either keeps it gone.
            // Process-once, before any window seeds the cog or a session spawns.
            {
                Startup::ScopedPhase _t{ L"  seed session/claude defaults" };
                SeedSessionEnvDefaults();
                SeedClaudeCleanupPeriodDaysIfNeeded();
            }

            // Observer: record every state change to a log file (and the debugger). Runs on a
            // bridge thread, so it must touch no XAML.
            e->registry->AddObserver([](const SessionInfo& s, HookEvent ev) {
                wchar_t line[600];
                ::swprintf(line,
                           600,
                           L"[%s] %s state=%d question=%d dir=%s\n",
                           HookEventName(ev),
                           s.id.c_str(),
                           static_cast<int>(s.state),
                           s.lastMessageWasQuestion ? 1 : 0,
                           s.workingDir.c_str());
                ::OutputDebugStringW(line);
                // Agentmaster: the PUSH event stream (named hooks — SessionStart / UserPromptSubmit /
                // Stop / Notification / ...) is ALWAYS logged; it is the state machine's audit trail.
                // But a HookEvent::Unknown notify carries no event semantics — it is "some fact changed"
                // from a PULL / transient path (a pending-input draft flip, a rename, an enrichment
                // refresh, a triage move), and those paths already log their OWN contextual tag
                // ([pending] / [nav] / ...). Logging an [Unknown] line here TOO produced a flood: ~98%
                // of [Unknown] lines were byte-identical consecutive repeats of the same id's same
                // (state,question,dir) — a pending-draft appear/clear alone re-emitted the identical
                // line — which grew hooks.log by hundreds of MB. So DEDUPE Unknown: skip it when this
                // id's line is unchanged since the last [Unknown] we logged for it (the first [Unknown]
                // after any real state/dir change still lands, so pull-driven transitions are recorded
                // once). Thread-safe — notify is raised from several engine threads.
                if (ev == HookEvent::Unknown)
                {
                    static std::mutex s_unknownMtx;
                    static std::unordered_map<std::wstring, std::wstring> s_lastUnknownLine; // id -> last [Unknown] body
                    std::lock_guard lk{ s_unknownMtx };
                    auto& prev = s_lastUnknownLine[s.id];
                    if (prev == line)
                    {
                        return; // identical to the last [Unknown] for this id -> nothing new to record
                    }
                    prev = line;
                }
                AppendStateLog(L"hooks.log", line);
            });

            // Autorunner scheduler (M7): owns its own worker thread and drives Auto Testing
            // queues. A clean turn-complete (Stop -> WaitingForInput) lands on the advance seam
            // and is forwarded to the scheduler; a separate observer feeds the stopOnError
            // backstop.
            //
            // Agentmaster (Auto Testing is a DEV-ONLY feature): the autorunner that auto-sends
            // queued prompts runs ONLY under the AgentmasterDev package. In a RELEASE install we
            // create the Scheduler object (so callers never null-deref) but DO NOT start its worker
            // and DO NOT wire the advance seam — so no session ever auto-sends. The Auto Testing UI
            // (the Manager pane tab, the autorunner toggles, the queue/compose, the board badge, the
            // overlay queue rows, the cog tab, the Pause button) is hidden in release to match, so
            // there is no way to queue a prompt either. Send-now is a direct registry->Inject (it
            // does not go through the scheduler), and it too is gated to dev in the UI.
            e->scheduler = std::make_shared<Scheduler>(e->registry);
            if (::Agentmaster::Profiles::IsDevPackage())
            {
                e->scheduler->Start();
                auto sched = e->scheduler;
                e->registry->SetAdvanceHandler([sched](const std::wstring& id) {
                    sched->RequestAdvance(id);
                });
                e->registry->AddObserver([sched](const SessionInfo& s, HookEvent) {
                    sched->OnObserved(s);
                });
            }
            else
            {
                AppendStateLog(L"hooks.log", L"[engine] release build: Auto Testing autorunner disabled (scheduler not started)\n");
            }

            // Persistence (M8): autosave the registry (queue + autorunner + metadata) to
            // sessions.json on every change, so an in-progress plan survives a crash. Restore
            // never replays Sent prompts (statuses are preserved).
            {
                auto reg = e->registry;
                e->registry->AddObserver([reg](const SessionInfo&, HookEvent) {
                    SaveSessions(reg->Snapshot());
                });
            }

            // Per-session DURABLE store (SessionStore): mirror the authoritative TITLE — the ONE
            // value Explorer/tab/persistence share (Rule #11) — to <profile>/session-store/<sid>.json
            // on every change, so a title known in ANY window persists across windows and OUTLIVES
            // the live session. The Sessions browser reads it (O(1) by id) for closed/historical rows,
            // and it is the generalized layer for any future per-session datum. SPARSE by design:
            // ObserveClaude never sets a title (Rule #13), so only sessions we actually launch / adopt
            // / rename / restore get a file; the write is deduped (skipped when the stored title is
            // already equal), so a steady-state enrichment re-notify costs one small read, not a write.
            // Loading the fleet at startup (each Upsert notifies) backfills the store for free.
            e->registry->AddObserver([](const SessionInfo& s, HookEvent) {
                if (!s.id.empty() && !s.title.empty())
                {
                    SetStoredSessionTitle(s.id, s.title);
                }
            });

            // Interval reconciler (M11; the PULL half — the bridge is PUSH). A low-priority
            // worker tails each live session's transcript to recover what a dropped hook missed
            // (a missed Stop strands a session in Running; assistant text is hook-invisible) and
            // fans out a periodic liveness sweep so a dead claude.exe (crash / exit with no
            // SessionEnd) is archived. Adaptive cadence + idle-sleep keep it ~free when nothing is
            // live; the observer below Wake()s it the instant a session goes live. The liveness
            // CHECK is WinRT (walks tabs), so it is delegated to per-window probes the scanner ticks.
            e->scanner = std::make_shared<SessionScanner>(e->registry);
            // Waiting-for-you "unread" model: seed the WaitingForInput -> Idle timeout from settings.json
            // BEFORE the worker starts (the Settings cog re-pushes it on save). Default 60 minutes (1h);
            // 0 disables (the cog's "Never"). (A second tiny LoadAppSettings read happens below for the
            // hook files — both are one small-file read at process init.)
            e->scanner->SetWaitingDecayMinutes(LoadAppSettings().waitingForYouTimeoutMinutes);
            e->scanner->Start();
            // Keep the scanner ticking even with nothing live, so each window's liveness probe — which
            // also drives the Fleet Observer's per-window roster publish — keeps running (a hand-typed
            // `claude` in a fresh tab is then correlated out-of-band by the observer). The transcript-
            // discovery ENUMERATION this used to also start is retired (O7); the observer subsumes it.
            e->scanner->ArmDiscovery();
            {
                auto scan = e->scanner;
                e->registry->AddObserver([scan](const SessionInfo&, HookEvent) { scan->Wake(); });
            }

            const auto pipeName = HookPipeName(::GetCurrentProcessId());
            {
                auto reg = e->registry; // shared, captured by the sink
                e->bridge = std::make_shared<HooksBridge>(
                    pipeName,
                    [reg](const HookMessage& m) { reg->OnHookEvent(m); },
                    4);
            }
            e->bridge->Start();

            // (B+D+C — observe & control sessions we did NOT Launch): make a hand-typed `claude`
            // in any `+` tab self-wire for hooks and get adopted.
            //  * publish the live pipe to bridge.json (a forwarder that didn't inherit
            //    CCMGR_HOOK_PIPE can still find it);
            //  * export CCMGR_HOOK_PIPE on OUR process and prepend a transparent `claude` PATH
            //    shim — every `+` tab inherits our live process env block, so a bare `claude` there
            //    runs the shim, which adds `--settings <ourHooks>`. This inheritance ONLY holds
            //    because we force the profiles.defaults `reloadEnvironmentVariables` OFF (see
            //    CascadiaSettingsSerialization FixupUserSettings — [Agentmaster]): with WT's default
            //    env-reload ON, ConptyConnection rebuilds a child's env from the REGISTRY
            //    (til::env::regenerate), which DROPS these runtime-only vars and the shim is never
            //    hit. Launch/Restore does NOT go through the shim: it spawns the NATIVE claude.exe BY
            //    FULL PATH (resolved into e->claudeExePath below, before this PATH prepend, so a
            //    claude.cmd on PATH is followed to its real binary, not our shim). exe-only policy: a
            //    pure-Node `claude` resolves to empty and GATES the Manager. (CreateProcessW needs the
            //    full path — it appends only ".exe" and ignores PATHEXT, the 0x80070002 bug.)
            try
            {
                // Fleet Observer (OBSERVER.md §7): mint the per-process ownership stamp and export
                // it as AM_SESSION FIRST — before the best-effort shim/discovery setup below (which
                // can throw) — so EVERY ConPTY child is stamped even if the shim author fails. Every
                // tab (Launched or a hand-typed `+`) inherits our process env block (the same
                // mechanism that delivers CCMGR_HOOK_PIPE to a hand-typed claude — reloadEnviron-
                // mentVariables forced OFF), so its claude.exe carries AM_SESSION and the observer
                // classifies it RunningApp::Agentmaster. NewSessionId() is just a plain lowercase
                // hyphenated GUID generator (CoCreateGuid).
                e->amSession = NewSessionId();
                ::SetEnvironmentVariableW(L"AM_SESSION", e->amSession.c_str());
                AppendStateLog(L"hooks.log", L"[engine] AM_SESSION " + e->amSession + L"\n");

                WriteBridgeDiscovery(pipeName);
                const auto stateDir = AgentmasterStateDir();
                uint64_t _ts = ::GetTickCount64();
                const auto hookFiles = MaterializeSharedHookFiles(stateDir, LoadAppSettings());
                Startup::Phase(L"  materialize hook files", ::GetTickCount64() - _ts);
                // Resolve the NATIVE claude.exe NOW (native-exe-only policy), while PATH is still
                // un-mutated so a claude.cmd on PATH is followed to its REAL binary, not the adoption
                // shim we author next. Honors the Settings override; ALWAYS a real claude.exe (or
                // empty). Empty => "Claude not detected" and the Manager gates every claude
                // interaction. Launched by full path (CreateProcessW appends only ".exe", ignores
                // PATHEXT — a bare `claude` would miss the npm install, the 0x80070002 bug).
                _ts = ::GetTickCount64();
                e->claudeExePath = ResolveClaudeExe(LoadAppSettings().claudeExePath);
                Startup::Phase(L"  ResolveClaudeExe (PATH scan)", ::GetTickCount64() - _ts);
                AppendStateLog(L"hooks.log", L"[engine] claude.exe: " + (e->claudeExePath.empty() ? std::wstring{ L"<not detected>" } : e->claudeExePath) + L"\n");
                // Resolve the codex launcher too (managed-Codex support), while PATH is still pristine.
                // Same CreateProcessW/PATHEXT hazard as claude (a bare `codex` misses an npm codex.cmd ->
                // 0x80070002), but Codex is NOT native-exe-only — the observer finds codex.exe as a
                // descendant — so a .cmd/.bat launcher is accepted (BuildCodexCommandline runs it via
                // `cmd /c`). Empty => not found; the spawn falls back to the bare token and surfaces the error.
                _ts = ::GetTickCount64();
                e->codexExePath = ResolveCodexLauncher();
                Startup::Phase(L"  ResolveCodexLauncher (PATH scan)", ::GetTickCount64() - _ts);
                AppendStateLog(L"hooks.log", L"[engine] codex: " + (e->codexExePath.empty() ? std::wstring{ L"<not detected>" } : e->codexExePath) + L"\n");
                // Resolve the PowerShell host that wraps every managed agent session (so quitting the
                // agent drops to a live pwsh prompt at the cwd — BuildPwshHostedCommandline). pwsh.exe
                // (PS7) on PATH, else Windows PowerShell; empty => the spawn falls back to the bare token.
                _ts = ::GetTickCount64();
                e->pwshExePath = ResolvePwshLauncher();
                Startup::Phase(L"  ResolvePwshLauncher (PATH scan)", ::GetTickCount64() - _ts);
                AppendStateLog(L"hooks.log", L"[engine] pwsh host: " + (e->pwshExePath.empty() ? std::wstring{ L"<not detected>" } : e->pwshExePath) + L"\n");
                // Author the adoption shim BEFORE touching PATH (so ResolveRealClaude inside it never
                // finds our own shim), then prepend the shim dir for hand-typed `+`-tab self-wiring.
                _ts = ::GetTickCount64();
                const auto shimDir = MaterializeClaudeShim(stateDir, hookFiles.first);
                Startup::Phase(L"  materialize claude shim", ::GetTickCount64() - _ts);
                ::SetEnvironmentVariableW(L"CCMGR_HOOK_PIPE", pipeName.c_str());
                if (!shimDir.empty())
                {
                    std::wstring path;
                    const DWORD need = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
                    if (need > 1)
                    {
                        path.resize(need);
                        const DWORD got = ::GetEnvironmentVariableW(L"PATH", path.data(), need);
                        path.resize(got);
                    }
                    const std::wstring newPath = shimDir + L";" + path;
                    ::SetEnvironmentVariableW(L"PATH", newPath.c_str());
                    AppendStateLog(L"hooks.log", L"[engine] claude shim on PATH: " + shimDir + L"\n");
                }
            }
            catch (...)
            {
            }

            // Fleet Observer S-lane (OBSERVER.md §8): the process-wide PULL census/correlation
            // worker, next to the scanner. Reads each claude.exe's PEB out-of-band, classifies
            // ownership via the AM_SESSION minted above, and (once a window publishes its tab roster)
            // correlates + feeds the registry — the always-correct floor beneath the lossy hook push.
            // Constructed AFTER the try block so it gets the real e->amSession (set at the top of the
            // try, before the throwing shim I/O). Never torn down (process lifetime), like the rest.
            {
                Startup::ScopedPhase _t{ L"  observer start (S-lane)" };
                e->observer = std::make_shared<ProcessObserver>(e->registry, e->amSession);
                e->observer->Start();
            }

            AppendStateLog(L"hooks.log", L"[engine] bridge listening on " + pipeName + L"\n");
            return e;
        }();

        return *g;
    }

    bool ClaudeAvailable()
    {
        return !SharedEngine().claudeExePath.empty();
    }

    bool EnsureClaudeAvailable()
    {
        auto& e = SharedEngine();
        if (!e.claudeExePath.empty())
        {
            return true; // already detected — cheap cache hit, no re-scan
        }
        // Cache is empty ("Claude not detected"). The user may have just run `claude install` (or put a
        // claude.exe on PATH) while the app stayed open, so re-resolve ONCE before we conclude it's
        // missing — honoring the Settings override exactly like RefreshClaudeExe. If it now resolves,
        // the caller proceeds and the install prompt never shows; the gate has self-healed. UI-thread-
        // called (every launch handler is), so this shares RefreshClaudeExe's single-writer contract.
        e.claudeExePath = ResolveClaudeExe(LoadAppSettings().claudeExePath);
        if (!e.claudeExePath.empty())
        {
            AppendStateLog(L"hooks.log", L"[engine] claude.exe found on re-check: " + e.claudeExePath + L"\n");
        }
        return !e.claudeExePath.empty();
    }

    std::wstring RefreshClaudeExe(std::wstring_view overridePath)
    {
        // Re-resolve when the Settings override changes, so a Browse/override takes effect without a
        // restart. UI-thread-called (settings save); claudeExePath has no other writer after init.
        auto& e = SharedEngine();
        e.claudeExePath = ResolveClaudeExe(overridePath);
        AppendStateLog(L"hooks.log", L"[engine] claude.exe refreshed: " + (e.claudeExePath.empty() ? std::wstring{ L"<not detected>" } : e.claudeExePath) + L"\n");
        return e.claudeExePath;
    }

    std::optional<WindowRecord> ClaimWindowRecord()
    {
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.windowMutex);
        if (!e.windowRecordsLoaded)
        {
            // Load every windows/*.json once. Front-to-back claim order is whatever the
            // directory iterator yields; multi-window restore (PERSISTENCE.md §13.0) will impose
            // a deterministic order when it assigns records to windows.
            e.unclaimedWindowRecords = LoadWindowRecords();
            e.windowRecordsLoaded = true;
        }
        if (e.unclaimedWindowRecords.empty())
        {
            return std::nullopt;
        }
        auto rec = std::move(e.unclaimedWindowRecords.front());
        e.unclaimedWindowRecords.erase(e.unclaimedWindowRecords.begin());
        return rec;
    }

    std::optional<WindowRecord> ClaimWindowRecord(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return std::nullopt;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.windowMutex);
        if (!e.windowRecordsLoaded)
        {
            e.unclaimedWindowRecords = LoadWindowRecords();
            e.windowRecordsLoaded = true;
        }
        // 1) The startup claim pool (records present at launch, not yet claimed) — the decline-at-startup
        //    recover path lands here.
        for (auto it = e.unclaimedWindowRecords.begin(); it != e.unclaimedWindowRecords.end(); ++it)
        {
            if (it->windowId == windowId)
            {
                auto rec = std::move(*it);
                e.unclaimedWindowRecords.erase(it);
                return rec;
            }
        }
        // 2) The reclaimable pool (records claimed-then-closed THIS session, returned by
        //    UnregisterLiveWindow). This is what lets the in-session "Reopen Windows" button re-claim a
        //    window it earlier closed — restoring its real id + lens — instead of minting a fresh,
        //    lens-less duplicate. find+erase is atomic under the lock, so two windows never share a
        //    record (a double-dispatch's second claim misses and mints, as intended).
        for (auto it = e.reclaimableWindowRecords.begin(); it != e.reclaimableWindowRecords.end(); ++it)
        {
            if (it->windowId == windowId)
            {
                auto rec = std::move(*it);
                e.reclaimableWindowRecords.erase(it);
                return rec;
            }
        }
        return std::nullopt;
    }

    void RegisterLiveWindow(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.windowMutex);
        e.liveWindowIds.insert(windowId);
        // A live window always has a record on disk (the page's autosave), so writing the manifest now
        // means even a single-window session (which never triggers a remove-with-remaining) is recorded
        // — so it reopens at its geometry/lens next run (the shipped Increment-2 behavior).
        SaveOpenWindows({ e.liveWindowIds.begin(), e.liveWindowIds.end() });
    }

    void UnregisterLiveWindow(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Re-read the closing window's record from disk BEFORE the lock (disk I/O off the mutex). It is
        // returned to the reclaimable pool below so the in-session recover button can re-claim it.
        // Absent on disk (a window closed before its first autosave) => nothing to re-claim, fine.
        auto reclaim = LoadWindowRecord(windowId);

        std::lock_guard<std::mutex> lk(e.windowMutex);
        e.liveWindowIds.erase(windowId);
        // Agentmaster (discard Manager-only windows): clear any Manager-only self-close reservation this
        // window held, so the set never accumulates stale ids across the process lifetime.
        e.closingWindowIds.erase(windowId);
        // Skip-empty: the LAST window's teardown must NOT clear the manifest, or "open at exit" would
        // always be empty. Leaving the prior snapshot means the next run reopens what was open when the
        // app exited — for a one-by-one close that is the final window; a hard shutdown that kills the
        // window threads before they unregister leaves the full set on disk (both are correct). §13.5.
        if (!e.liveWindowIds.empty())
        {
            SaveOpenWindows({ e.liveWindowIds.begin(), e.liveWindowIds.end() });
        }
        // Agentmaster (discard Manager-only windows): a mid-session-closed window whose record has NO
        // tab refs held nothing but the Manager tab — reopening it would reconstruct exactly what
        // "+ new window" gives (every window auto-creates the Manager tab), so the record is pure
        // noise: DELETE it instead of pooling it, and it stops accumulating in windows/ (and polluting
        // the next launch's front-pop claim order). Mirrors the manifest's skip-empty above: the LAST
        // window out (quit / single-window close, live set now empty) KEEPS its record even when
        // empty — that record is the open-at-exit snapshot the next launch claims for geometry + lens,
        // so a pure-Manager single-window workflow still reopens at its position. (RecoverableWindows
        // filters empty records anyway, so a kept-empty final record is never OFFERED for reopen.)
        if (reclaim && reclaim->tabs.empty() && !e.liveWindowIds.empty())
        {
            DeleteWindowRecord(windowId);
            reclaim.reset();
        }
        // Return the record to the reclaimable pool so a later in-session reopen (the recover button)
        // re-claims THIS record (real id + lens) rather than minting a duplicate. Dedup the re-add so a
        // double teardown can't stack two copies. The startup pool is left alone — reclaim is by id only.
        if (reclaim)
        {
            bool present = false;
            for (const auto& r : e.reclaimableWindowRecords)
            {
                if (r.windowId == windowId)
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                e.reclaimableWindowRecords.push_back(std::move(*reclaim));
            }
        }
    }

    std::vector<std::wstring> LiveWindowIds()
    {
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.windowMutex);
        return { e.liveWindowIds.begin(), e.liveWindowIds.end() };
    }

    bool ReserveManagerOnlyClose(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return false;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.windowMutex);
        // Effective remaining-live = live windows MINUS those already reserved to self-close this tick.
        // Reserve THIS window's close only while >1 would remain — so concurrent Manager-only windows on
        // different threads can never all close (each reserves under the lock, and the one that finds
        // remaining==1 stays). A window that reserved then closes clears its id in UnregisterLiveWindow.
        size_t remaining = 0;
        for (const auto& id : e.liveWindowIds)
        {
            if (e.closingWindowIds.find(id) == e.closingWindowIds.end())
            {
                ++remaining;
            }
        }
        if (remaining > 1)
        {
            e.closingWindowIds.insert(windowId);
            return true;
        }
        return false;
    }

    uint64_t RegisterWindowActivateHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateMutex);
        const auto token = e.nextActivateToken++;
        e.activateSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowActivateHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateMutex);
        for (auto it = e.activateSinks.begin(); it != e.activateSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.activateSinks.erase(it);
                return;
            }
        }
    }

    void ActivateSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the registry's _notify pattern): each sink
        // hops into its own window's dispatcher, and holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard.
        std::vector<std::function<void(const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.activateMutex);
            sinks.reserve(e.activateSinks.size());
            for (const auto& s : e.activateSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId); // fire-and-forget; the (single) hosting window selects + foregrounds, the rest miss
        }
    }

    uint64_t RegisterActivateAllDormantHandler(const std::wstring& windowId, std::function<void()> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateAllMutex);
        const auto token = e.nextActivateAllToken++;
        e.activateAllSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterActivateAllDormantHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateAllMutex);
        for (auto it = e.activateAllSinks.begin(); it != e.activateAllSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.activateAllSinks.erase(it);
                return;
            }
        }
    }

    void ActivateAllDormantInOtherWindows(const std::wstring& sourceWindowId)
    {
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher to eager-init that window's dormant controls.
        std::vector<std::function<void()>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.activateAllMutex);
            sinks.reserve(e.activateAllSinks.size());
            for (const auto& s : e.activateAllSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(); // fire-and-forget; each other window wakes its own dormant tabs
        }
    }

    uint64_t RegisterWindowRestartHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.restartMutex);
        const auto token = e.nextRestartToken++;
        e.restartSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowRestartHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.restartMutex);
        for (auto it = e.restartSinks.begin(); it != e.restartSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.restartSinks.erase(it);
                return;
            }
        }
    }

    void RestartSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher, so holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard.
        std::vector<std::function<void(const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.restartMutex);
            sinks.reserve(e.restartSinks.size());
            for (const auto& s : e.restartSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId); // fire-and-forget; the (single) hosting window restarts the session, the rest miss
        }
    }

    uint64_t RegisterSettingsChangedHandler(const std::wstring& windowId, std::function<void(const AppSettings&)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.settingsMutex);
        const auto token = e.nextSettingsToken++;
        e.settingsSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterSettingsChangedHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.settingsMutex);
        for (auto it = e.settingsSinks.begin(); it != e.settingsSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.settingsSinks.erase(it);
                return;
            }
        }
    }

    void BroadcastSettingsChanged(const AppSettings& settings, const std::wstring& sourceWindowId)
    {
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher, so holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard. The source window is excluded — it already
        // applied + persisted the change (and its own toggle/cog re-rendered locally).
        std::vector<std::function<void(const AppSettings&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.settingsMutex);
            sinks.reserve(e.settingsSinks.size());
            for (const auto& s : e.settingsSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(settings); // fire-and-forget; each OTHER window re-applies on its own UI thread
        }
    }

    std::vector<RecoverableWindow> RecoverableWindows()
    {
        auto& e = SharedEngine();
        // Snapshot the live set under the lock, then read records OUTSIDE the lock (disk IO).
        std::set<std::wstring> live;
        {
            std::lock_guard<std::mutex> lk(e.windowMutex);
            live = e.liveWindowIds;
        }
        std::vector<RecoverableWindow> out;
        const auto records = LoadWindowRecords(); // canonical sorted order -> the index IS the `-s <idx>`
        for (int i = 0; i < static_cast<int>(records.size()); ++i)
        {
            // Agentmaster (discard Manager-only windows): a record with NO tab refs is a window that
            // held nothing but the Manager tab — and every window auto-creates the Manager tab at
            // index 0, so reopening it reconstructs exactly what "+ new window" gives (plus stale
            // geometry/lens). Offering it is noise: skip it from every recover surface (the "Reopen
            // Windows (N)" count, the Archive page's "Saved window" rows, reopen-all, the per-row
            // windowId re-resolution — they all flow through here). The loop keeps `i` as the
            // CANONICAL LoadWindowRecords index, so the surviving entries' `-s <idx>` still addresses
            // the right record. (The Emperor's startup open-at-exit reopen reads windows/*.json
            // directly and is deliberately untouched — restoring what was open at exit is faithful.)
            if (records[i].tabs.empty())
            {
                continue;
            }
            if (live.find(records[i].windowId) == live.end())
            {
                out.push_back({ i, records[i] });
            }
        }
        return out;
    }
}
