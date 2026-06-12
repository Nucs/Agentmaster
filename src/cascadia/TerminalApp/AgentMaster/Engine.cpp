// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing). The
// process-singleton wiring is exactly what TerminalPage::_InitAgentmasterEngine did inline
// before M9; hoisted here so it runs once for the whole WindowEmperor process.
#include "Engine.h"

#include "ClaudeSpawn.h"
#include "HookWire.h"
#include "HooksBridge.h"
#include "Persistence.h"
#include "ProcessObserver.h"
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionScanner.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace Agentmaster
{
    Engine& SharedEngine()
    {
        // Function-local static: C++ guarantees this initializer runs exactly once even if two
        // window threads call SharedEngine() concurrently. Heap-allocated and never deleted on
        // purpose (process lifetime) — see Engine.h.
        static Engine* const g = []() -> Engine* {
            auto* e = new Engine{};
            e->registry = std::make_shared<SessionRegistry>();

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
                AppendStateLog(L"hooks.log", line);
            });

            // Autopilot scheduler (M7): owns its own worker thread and drives Flight Plan
            // queues. A clean turn-complete (Stop -> WaitingForInput) lands on the advance seam
            // and is forwarded to the scheduler; a separate observer feeds the stopOnError
            // backstop.
            e->scheduler = std::make_shared<Scheduler>(e->registry);
            e->scheduler->Start();
            {
                auto sched = e->scheduler;
                e->registry->SetAdvanceHandler([sched](const std::wstring& id) {
                    sched->RequestAdvance(id);
                });
                e->registry->AddObserver([sched](const SessionInfo& s, HookEvent) {
                    sched->OnObserved(s);
                });
            }

            // Persistence (M8): autosave the registry (queue + autopilot + metadata) to
            // sessions.json on every change, so an in-progress plan survives a crash. Restore
            // never replays Sent prompts (statuses are preserved).
            {
                auto reg = e->registry;
                e->registry->AddObserver([reg](const SessionInfo&, HookEvent) {
                    SaveSessions(reg->Snapshot());
                });
            }

            // Interval reconciler (M11; the PULL half — the bridge is PUSH). A low-priority
            // worker tails each live session's transcript to recover what a dropped hook missed
            // (a missed Stop strands a session in Running; assistant text is hook-invisible) and
            // fans out a periodic liveness sweep so a dead claude.exe (crash / exit with no
            // SessionEnd) is archived. Adaptive cadence + idle-sleep keep it ~free when nothing is
            // live; the observer below Wake()s it the instant a session goes live. The liveness
            // CHECK is WinRT (walks tabs), so it is delegated to per-window probes the scanner ticks.
            e->scanner = std::make_shared<SessionScanner>(e->registry);
            // Cache-aware Waiting decay: seed the WaitingForInput -> Idle window from settings.json
            // BEFORE the worker starts (the Settings cog re-pushes it on save). Default 5 minutes ==
            // Claude's server-side prompt-cache lifetime; 0 disables. (A second tiny LoadAppSettings
            // read happens below for the hook files — both are one small-file read at process init.)
            e->scanner->SetWaitingDecayMinutes(LoadAppSettings().waitingDecayMinutes);
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
            //    hit. Launch's direct CreateProcessW("claude ...") resolves claude.exe (no PATHEXT)
            //    and bypasses the .cmd shim — so no double-wiring.
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
                const auto hookFiles = MaterializeSharedHookFiles(stateDir, LoadAppSettings());
                // Resolve real claude + author the shim BEFORE touching PATH (so it never finds
                // our own shim), then prepend the shim dir.
                const auto shimDir = MaterializeClaudeShim(stateDir, hookFiles.first);
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
            e->observer = std::make_shared<ProcessObserver>(e->registry, e->amSession);
            e->observer->Start();

            AppendStateLog(L"hooks.log", L"[engine] bridge listening on " + pipeName + L"\n");
            return e;
        }();

        return *g;
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
