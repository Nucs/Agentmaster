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
            e->scanner->Start();
            // Arm transcript discovery: from now on the scanner indexes every NEW Claude transcript
            // (a hand-typed `claude` whose hooks never wired). Each window's probe correlates the
            // index to its own tabs by working directory — the one detection path no shell can shadow.
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
            if (live.find(records[i].windowId) == live.end())
            {
                out.push_back({ i, records[i] });
            }
        }
        return out;
    }
}
