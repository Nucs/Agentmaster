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
    TestDraftCarry(); // PENDING_INPUT.md §11 / §2c — the fork carry + paste journal
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
    TestRegistrySnapshotLive();
    TestRegistryLeanReads(); // UI_THREAD.md §10a #4 — the lean registry reads (tests_state.cpp)
    TestForkSourceIdEcho();
    TestSummaryChainLinks(); // the summary's conversation chain: the /clear pairing (tests_state.cpp)
    TestRetiredHostToken(); // Deactivate/Restart: the closing claude of a replaced connection cannot re-stamp the record (tests_state.cpp)
    TestConversationHosts(); // OBSERVER.md §20: one host tab per conversation that runs in two or more tabs (tests_state.cpp)
    TestDuplicateConversationHosts(); // OBSERVER.md §20: the registry ignores a duplicate tab's hooks and observations (tests_state.cpp)
    TestRefreshPacing(); // UI_THREAD.md §12: the Agent Manager page's adaptive rebuild gap (tests_state.cpp)
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
    TestClaudeInvisibleOracle(); // DELIVERY.md §15 (tests_invisible.cpp)
    TestClaudeInputSanitize();
    TestInvisibleEchoAndDedupe();
    TestForwarderEncoding();
    TestHookInvocation(); // exec-form hooks + per-event timeouts + the Stop fast path (tests_spawn_sched.cpp)
    TestSchedulerIntegration();
    TestStateSettle(); // DELIVERY.md §16 — the STATE SETTLE hold (tests_spawn_sched.cpp)
    TestInterruptHold();
    TestUpdaterVersionLogic();
    TestCmdPathLimit();
    TestStartupGuard(); // the startup-deadlock fix (StartupGuard.h, tests_spawn_sched.cpp)
    TestPortableInstall();
    TestTranscriptScan();
    TestCurrentModel();
    TestBlockedAndInterruptedStates();
    TestPersistence();
    TestManagerLayout();
    TestWindowRecord();
    TestAppSettings();
    TestSummaryHistoryPersistence(); // the summary's conversation chain: the persisted link + toggle (tests_persistence.cpp)
    TestTabNamingAndColor();
    TestTabColorModes();
    TestEngineWindowLifecycle();
    TestUiStallEscalation();
    TestProcessInspectTree();
    TestOrphanConsoleReaper();
    TestProcessInspectParse();
    TestTranscriptResolve();
    TestCodexObserve();
    TestTranscriptStore();
    TestSessionTags();
    TestSessionStoreScanCache(); // tags perf: the session store's scan cache, by counting file opens (tests_transcript.cpp)
    TestExtractPathsFromText();
    TestInferWorkingDirectory();
    TestSessionFolderFilter(); // SESSIONS.md section 10 - the Sessions page's folder filter, pure + a scratch git tree (tests_transcript.cpp)
    TestConversationLineage();
    TestScannerCatchUp(); // the 2026-09-25 catch-up stall end to end (tests_transcript.cpp)
    TestTurnStartRaceGuard(); // the 2026-10-01 turn-start race: the prompt-supersession guard, pure + end to end (tests_transcript.cpp)
    TestScannerDormantHold(); // a parked/restored (dormant) session is never promoted to Running, end to end (tests_transcript.cpp)
    TestCutOffTurn(); // a turn that a shutdown cut off reads Error and comes back to Running when it resumes - the 2026-10-09 incident, pure + end to end (tests_transcript.cpp)
    TestTranscriptWatcher(); // the scanner's ReadDirectoryChangesW change feed (tests_transcript.cpp)
    TestCompactionLifecycle();
    TestConversationFollowup();
    TestTranscriptTurns(); // the Copy > Transcript levels vs the transcript_turns.py goldens (tests_turns.cpp)
    TestSummaryConversation(); // the summary's numbered conversation list, pure (tests_turns.cpp)
    TestConversationUpstream(); // the summary's whole conversation: the chain walk end to end (tests_turns.cpp)
    TestMarkdownBlocks(); // the transcript overlay's Markdown flattener — a skip without the vcpkg cmark (tests_markdown.cpp)
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
    TestTabStripReveal();
    TestKeyedListDiff(); // SIDE_PANEL.md §2c — the side panel card list's order planner (tests_sidepanel.cpp)
    TestSessionTimingText(); // the cards' and rows' "created 4d1h, active 2m4s ago" timing line (tests_sidepanel.cpp)
    TestPointerCardPlacement(); // SIDE_PANEL.md §2e — where the "+" launcher card opens (tests_sidepanel.cpp)
    TestAgentHarnessCore(); // SDK_HARNESS.md — the agent harness's model-free half: record, digest, settings, environment, readers (tests_harness.cpp)
    TestSessionSearchTools(); // SESSIONS.md section 9 — the AI search's tools: arguments, one search, result text, handlers, the stepwise gather (tests_harness.cpp)
    TestSessionDb(); // SESSIONS_DB.md — the session database's shared parts (tests_sessiondb.cpp)
    TestMessaging(); // MESSAGING.md — the fleet in Telegram, the pure core (tests_messaging.cpp)
    TestMessagingSearch(); // MESSAGING.md §5.4 / §21.7 — the Search topic's verbs, pick buttons and pick states (tests_messaging_search.cpp)
    TestLaunchPresets(); // HARNESSES.md — PRESETS: templates, tiers, the launchArgs snapshot, the compile, refusals, menus (tests_presets.cpp)
    TestTabCloseSuccessor();
    TestTaskbarBadge(); // NOTIFICATIONS.md section 8 — the taskbar unread badge's renderer (tests_taskbar.cpp)
    TestUnreadCensus(); // ...and the process-wide unread count behind it
    TestTaskbarThumbnail(); // NOTIFICATIONS.md section 9 — the unread frame on a window's taskbar / Alt+Tab preview (tests_taskbar.cpp)

    BenchPromptAnchor();
    BenchPromptAnchorRealSession(); // SUMMARY_JUMP.md §4: full batch resolve on a real heavy session (AM_BENCH_SESSION)

    std::wprintf(L"\n%d checks, %d failures - %S\n", g_checks, g_failures, g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
