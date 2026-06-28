// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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
    TestPromptAnchorRealCorpus();
    TestSummaryUserMsgNoise();
    TestSummaryTableTrim();
    TestStateMachine();
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
