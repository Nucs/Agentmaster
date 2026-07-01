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
// ★ m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all 40 test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: the RUNNER.
//
// Not part of the msbuild. Compiles the engine TUs unity-style WITHOUT the TerminalApp
// PCH (AGENTMASTER_STANDALONE_TEST) so the pure logic + the real named-pipe transport can
// be verified from the command line (see run-m5-tests.bat). The tests themselves live in
// the themed tests_*.cpp TUs; the shared CHECK macro / counters / fixtures / entry-point
// declarations are in m5_tests.h. This file owns only the shared counters + wmain.

#include "m5_tests.h"

int g_failures = 0;
int g_checks = 0;

int wmain()
{
    std::wprintf(L"=== Agentmaster engine tests ===\n");
    TestPendingInput();
    TestPromptAnchor();
    TestPromptAnchorEdgeCases();
    TestPromptAnchorCollisions();
    TestPromptAnchorRealCorpus();
    TestSummaryUserMsgNoise();
    TestSummaryTableTrim();
    TestStateMachine();
    TestRestoredSessionState();
    TestOrderedStateMachine();
    TestWire();
    TestRegistry();
    TestRegistryFanout();
    TestForkSourceIdEcho();
    TestTypedCapture();
    TestObserveClaude();
    TestSupersedeStaleTabSiblings();
    TestSpawnBuilders();
    TestProfileBootstrap();
    TestScheduler();
    TestEnterRetry();
    TestBuildPromptSubmission();
    TestSchedulerIntegration();
    TestTranscriptScan();
    TestBlockedAndInterruptedStates();
    TestPersistence();
    TestManagerLayout();
    TestWindowRecord();
    TestAppSettings();
    TestTabNamingAndColor();
    TestProcessInspectTree();
    TestProcessInspectParse();
    TestTranscriptResolve();
    TestCodexObserve();
    TestTranscriptStore();
    TestConversationLineage();
    TestSessionSearch();
    TestProcessInspectLive();
    TestBringToFrontHeuristics();
    TestBridgeRoundTrip();

    BenchPromptAnchor();
    BenchPromptAnchorRealSession(); // SUMMARY_JUMP.md §4: full batch resolve on a real heavy session (AM_BENCH_SESSION)

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
