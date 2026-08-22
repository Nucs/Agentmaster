// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The tests + benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
// ★ m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration / updater version+prefs
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color / engine window lifecycle
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
    // Agentmaster (state isolation): unless the caller already pointed AGENTMASTER_PROFILE
    // somewhere (run-m5-tests.bat exports a WIPED %TEMP% scratch profile), default to that same
    // scratch dir — BEFORE the first AgentmasterStateDir() call anywhere, since the profile
    // resolution caches process-wide. Without this a direct m5_tests.exe run would append its
    // engine traces ([fork-echo]/[send]/[recon-*]) to the LIVE release install's ~/.agentmaster
    // logs and park test window records beside real ones (a crash mid-test would then leave a
    // phantom "Reopen Windows (N)" entry in the production app).
    {
        wchar_t cur[8];
        if (::GetEnvironmentVariableW(L"AGENTMASTER_PROFILE", cur, 8) == 0)
        {
            wchar_t tmp[MAX_PATH]{};
            ::GetTempPathW(MAX_PATH, tmp);
            const std::wstring scratch = std::wstring{ tmp } + L"agentmaster-m5-tests";
            ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", scratch.c_str());
        }
    }

    std::wprintf(L"=== Agentmaster engine tests ===\n");
    TestPendingInput();
    TestPendingPaste();
    TestPromptAnchor();
    TestPromptAnchorEdgeCases();
    TestPromptAnchorCollisions();
    TestPromptAnchorFloorIndex();
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
    TestQueueHistoryTrim();
    TestTakeLastPendingPrompt();
    TestObserveClaude();
    TestSupersedeStaleTabSiblings();
    TestSpawnBuilders();
    TestProfileBootstrap();
    TestWorkspaceTrust();
    TestScheduler();
    TestEnterRetry();
    TestDeliveryGate();
    TestLostSendReconciler();
    TestVerifiedPlacement();
    TestBuildPromptSubmission();
    TestSchedulerIntegration();
    TestUpdaterVersionLogic();
    TestTranscriptScan();
    TestCurrentModel();
    TestBlockedAndInterruptedStates();
    TestPersistence();
    TestManagerLayout();
    TestWindowRecord();
    TestAppSettings();
    TestTabNamingAndColor();
    TestTabColorModes();
    TestEngineWindowLifecycle();
    TestUiStallEscalation();
    TestProcessInspectTree();
    TestProcessInspectParse();
    TestTranscriptResolve();
    TestCodexObserve();
    TestTranscriptStore();
    TestSessionTags();
    TestExtractPathsFromText();
    TestInferWorkingDirectory();
    TestConversationLineage();
    TestAnalyzeFootprint();
    TestCommandWatch();
    TestCommandHandoverE2E();
    TestCommandEchoRealCorpus();
    TestSessionSearch();
    TestProcessInspectLive();
    TestTeammateLiveCorpus();
    TestBringToFrontHeuristics();
    TestBridgeRoundTrip();
    TestTabDragMath();

    BenchPromptAnchor();
    BenchPromptAnchorRealSession(); // SUMMARY_JUMP.md §4: full batch resolve on a real heavy session (AM_BENCH_SESSION)

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
