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
            //    shim — every `+` tab inherits our live env (ConptyConnection regenerates from
            //    the current process block), so a bare `claude` there runs the shim, which adds
            //    `--settings <ourHooks>`. Launch's direct CreateProcessW("claude ...") resolves
            //    claude.exe (no PATHEXT) and bypasses the .cmd shim — so no double-wiring.
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
}
