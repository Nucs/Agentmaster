// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The 38 tests + 2 benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
// ★ m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all 40 test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: SHARED declarations.
//
// The harness is split across themed TUs (tests_state.cpp, tests_spawn_sched.cpp,
// tests_persistence.cpp, tests_transcript.cpp, tests_summary_anchor.cpp) that each include
// this header for the CHECK macro, the shared check counters, the small fixture helpers, and
// the test entry-point declarations the runner (m5_tests.cpp) calls. Not part of the msbuild;
// run-m5-tests.bat compiles every TU unity-style (no PCH/WinRT) and links them. Splitting the
// formerly-6000-line m5_tests.cpp keeps each themed TU navigable + independently editable.

#pragma once

#ifndef AGENTMASTER_STANDALONE_TEST
#define AGENTMASTER_STANDALONE_TEST
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

// Headers only; the engine .cpp TUs are compiled separately and linked (see
// run-m5-tests.bat) so each keeps its own anonymous-namespace helpers.
#include "../Activity.h"
#include "../ClaudeSpawn.h"
#include "../HookWire.h"
#include "../HooksBridge.h"
#include "../Json.h"
#include "../Persistence.h"
#include "../PendingInput.h" // the unsent-draft detector (PENDING_INPUT.md) - pure, header-only
#include "../ProcessInspect.h" // SnapshotProcesses / ReadClaudeFacts / ResolveSessionId (Observer O1)
#include "../PromptAnchor.h" // the summary-panel JUMP resolver (SUMMARY_JUMP.md) - pure, benchmarked here
#include "../ProfileBootstrap.h" // the per-install state PROFILE (choice file / resolution / migrate)
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"
#include "../SessionScanner.h" // ParseTranscriptDelta (pure)
#include "../SessionSearch.h" // the Sessions page's two-phase search (SESSIONS.md §6)
#include "../SessionStore.h" // the generalized DURABLE per-session key/value store (titles, ...)
#include "../TranscriptStore.h" // the on-disk Claude-session store API (SESSIONS.md §6)

using namespace Agentmaster;

// Shared check counters - DEFINED in m5_tests.cpp (the runner TU), referenced by CHECK
// from every themed test TU.
extern int g_failures;
extern int g_checks;

#define CHECK(cond, msg)                                                      \
    do                                                                        \
    {                                                                         \
        ++g_checks;                                                           \
        if (!(cond))                                                          \
        {                                                                     \
            ++g_failures;                                                     \
            std::wprintf(L"  [FAIL] %S\n", msg);                              \
        }                                                                     \
    } while (0)

// ---- shared fixtures (used across multiple themed test TUs) ----
inline SessionInfo MakeSession(const std::wstring& id, SessionState st = SessionState::Idle)
{
    SessionInfo s;
    s.id = id;
    s.title = L"t";
    s.workingDir = L"K:/x";
    s.state = st;
    return s;
}

inline HookMessage Msg(const std::wstring& id, HookEvent ev)
{
    HookMessage m;
    m.sessionId = id;
    m.event = ev;
    return m;
}

inline HookMessage UPS(const std::wstring& id, const std::wstring& prompt)
{
    HookMessage m;
    m.sessionId = id;
    m.event = HookEvent::UserPromptSubmit;
    m.promptText = prompt;
    return m;
}

inline int64_t NowMsTest()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---- test entry points (each DEFINED in its themed TU; called by wmain in m5_tests.cpp) ----
// tests_state.cpp
void TestStateMachine();
void TestRestoredSessionState();
void TestOrderedStateMachine();
void TestWire();
void TestRegistry();
void TestRegistryFanout();
void TestForkSourceIdEcho();
void TestTypedCapture();
void TestObserveClaude();
void TestSupersedeStaleTabSiblings();
// tests_spawn_sched.cpp
void TestSpawnBuilders();
void TestProfileBootstrap();
void TestBridgeRoundTrip();
void TestScheduler();
void TestEnterRetry();
void TestBuildPromptSubmission();
void TestSchedulerIntegration();
// tests_persistence.cpp
void TestPersistence();
void TestManagerLayout();
void TestWindowRecord();
void TestAppSettings();
void TestTabNamingAndColor();
void TestTabColorModes(); // tab color modes: enum/JSON round-trips + ChooseSessionAutoColor + SessionColorKeyDir/ResolveSessionColorHex
// tests_transcript.cpp
void TestTranscriptScan();
void TestBlockedAndInterruptedStates();
void TestProcessInspectTree();
void TestProcessInspectParse();
void TestTranscriptResolve();
void TestCodexObserve();
void TestTranscriptStore();
void TestInferWorkingDirectory(); // tab color modes: the pure inferred-workdir picker (majority-deepest over tool-touched paths)
void TestConversationLineage();
void TestSessionSearch();
void TestProcessInspectLive();
void TestBringToFrontHeuristics();
// tests_summary_anchor.cpp
void TestSummaryTableTrim();
void TestPromptAnchor();
void TestPromptAnchorEdgeCases();
void TestPromptAnchorCollisions();
void TestPromptAnchorRealCorpus();
void TestSummaryUserMsgNoise();
void TestPendingInput();
void BenchPromptAnchor();
void BenchPromptAnchorRealSession();
