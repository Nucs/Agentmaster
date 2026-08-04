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
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
// ★ m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration / updater version+prefs
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color / engine window lifecycle
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
//   tests_commands.cpp        - COMMANDS.md: slash-command bindings (ParseCommandEcho / CommandWatch / DeriveSuffixedTitle / initial-prompt arg / handover command file)
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
#include "../ModelCatalog.h" // the published model-list parsers behind "Specify a model..." - pure, header-only
#include "../Persistence.h"
#include "../PendingInput.h" // the unsent-draft detector (PENDING_INPUT.md) - pure, header-only
#include "../PendingPaste.h" // the paste-cache marker resolver (PENDING_INPUT.md §2b) - pure, header-only
#include "../ProcessInspect.h" // SnapshotProcesses / ReadClaudeFacts / ResolveSessionId (Observer O1)
#include "../PromptAnchor.h" // the summary-panel JUMP resolver (SUMMARY_JUMP.md) - pure, benchmarked here
#include "../ProfileBootstrap.h" // the per-install state PROFILE (choice file / resolution / migrate)
#include "../RegexUtil.h" // the ONE guarded regex component (COMMANDS.md §6b) - pure, header-only
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"
#include "../SessionScanner.h" // ParseTranscriptDelta (pure)
#include "../SessionSearch.h" // the Sessions page's two-phase search (SESSIONS.md §6)
#include "../SessionStore.h" // the generalized DURABLE per-session key/value store (titles, ...)
#include "../Sha256.h" // SHA-256 (the shipped command definitions' version identity) - pure, header-only
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
void TestQueueHistoryTrim(); // bounded queue history: TrimQueueHistory (pure) + the registry append-seam cap
void TestObserveClaude();
void TestSupersedeStaleTabSiblings();
// tests_spawn_sched.cpp
void TestSpawnBuilders();
void TestProfileBootstrap();
void TestWorkspaceTrust(); // workspace-trust seed: ClaudeWorkspaceTrustKey normalization + the SURGICAL SpliceWorkspaceTrust (already-trusted no-op, flip-in-place, insert-member, insert-projects, byte-preservation, no-clobber refusals)
void TestBridgeRoundTrip();
void TestScheduler();
void TestEnterRetry();
void TestDeliveryGate(); // DELIVERY.md: the per-session delivery gate — open/decline/owner-close/reclaim, the close-notify wake-up, DecideAdvance's hold, DecideEnterRetry's gate+dormant refusals, SubmitPrompt's lifecycles, the RC6 newline-folded echo consume
void TestLostSendReconciler(); // DELIVERY_PLAN.md R1: the PULL echo-consume (NoteExternalPrompt marks a fold-matched transcript line as `echoed` evidence, line-ts replay-guarded) + the pure DecideLostSend verdict matrix + the `#6` shape end-to-end (Failed + autorunner paused, never resent)
void TestBuildPromptSubmission();
void TestSchedulerIntegration();
void TestUpdaterVersionLogic(); // Updater.h: ParseVersion/CompareVersion (4th part = build metadata, ignored) + unpackaged facts + the settings.json skip/postpone RMW (preserve-other-keys, replace-not-append)
// tests_persistence.cpp
void TestPersistence();
void TestManagerLayout();
void TestWindowRecord();
void TestAppSettings();
void TestTabNamingAndColor();
void TestTabColorModes(); // tab color modes: enum/JSON round-trips + ChooseSessionAutoColor + SessionColorKeyDir/ResolveSessionColorHex
void TestEngineWindowLifecycle(); // Engine window lifecycle (local-Engine `...In` seams — NEVER SharedEngine() in tests): claim-by-id vs front-pop + the open-at-exit manifest's skip-empty rule + the reclaimable pool + Manager-only record deletion + ReserveManagerOnlyClose (PERSISTENCE.md 13.5 / Rule #16)
// tests_transcript.cpp
void TestTranscriptScan();
void TestCurrentModel(); // current-model adornment: ShortModelName / SessionDisplayModel (pure) + the delta's assistant message.model capture + TailFactsFromTranscriptChunk (recap+model, one pass)
void TestBlockedAndInterruptedStates();
void TestProcessInspectTree();
void TestProcessInspectParse();
void TestTranscriptResolve();
void TestCodexObserve();
void TestTranscriptStore();
void TestSessionTags(); // bookmark tags: normalize/fold/encode/decode + the store CRUD + the tag-colors store (tag-colors.json) + the known-tag registry (tags.json — a tag survives 0 carriers) + CollectGlobalTags (incl. the registry overload) + the maxTags clamp/round-trip
void TestExtractPathsFromText(); // tab color modes: absolute-path mining from shell command strings (quoted free-form; unquoted 1-space-in-folder-names) + the ClassifyTranscriptLine command wiring
void TestInferWorkingDirectory(); // tab color modes: the pure inferred-workdir picker (majority-deepest over tool-touched paths)
void TestConversationLineage();
void TestAnalyzeFootprint(); // analyze footprint: ScrubLargeBase64Payloads (pure) + the AnalyzeSessionTranscript (size,mtime) cache hit/invalidation/copy semantics
void TestSessionSearch();
void TestProcessInspectLive();
void TestTeammateLiveCorpus(); // teammates/background work: the outlived-turn promotion + hold, the cache-hint split, and the wrapper noise gate replayed against REAL ~/.claude sessions (guarded, [info]-skips)
void TestBringToFrontHeuristics();
// tests_summary_anchor.cpp
void TestSummaryTableTrim();
void TestPromptAnchor();
void TestPromptAnchorEdgeCases();
void TestPromptAnchorCollisions();
void TestPromptAnchorFloorIndex();
void TestPromptAnchorRealCorpus();
void TestSummaryUserMsgNoise();
void TestPendingInput();
// tests_tabdrag.cpp — TabDragMath.h: the pure brain of the pointer-owned tab reorder/tear-out
// gesture (the native-MUX-drag replacement — CLAUDE.md "MUX TabView drag-start null-deref"
// gotcha): arming threshold, midpoint insertion-slot rule over realized bands, caret boundary,
// slot->_TryMoveTab target conversion (manager floor), release classification (reorder vs
// tear-out slack), and edge auto-scroll steps
void TestTabDragMath();
void TestPendingPaste();

// tests_commands.cpp — COMMANDS.md: slash-command bindings (ParseCommandEcho + the parser's
// Command/fileWritePaths events + the CommandWatch state machine incl. MULTI-FILE collect +
// seal-at-turn-end + settle fallback + the SAME-FAMILY SUPERSEDE race guard [no family command
// ever races another's files] + the durable per-session progress [fired
// watermark / armed-marker revival / encode-decode] + DeriveSuffixedTitle + the initial-prompt
// commandline arg + EnsureHandoverCommandFileIn + the safeguard belts: sane-path gate incl. '|',
// Join/SplitWatchPaths, throwing-handler/probe containment; plus the /handover-here twin units —
// hyphenated echo, name-exact binding isolation, EnsureHandoverHereCommandFileIn's own file —
// and the /handover-standby §5b units: EnsureHandoverStandbyCommandFileIn's own file + digest
// gate + fill-not-send sentinels, THREE-way binding isolation + the supersede pivot onto the
// standby path, and BuildPromptFill [no trailing CR ever; submit == fill + "\r"])
void TestCommandWatch();
// tests_commands.cpp — the FABRICATED /handover session: the design's expected transcript shape
// (echo -> Write -> tool_result -> end_turn, real ISO timestamps) driven end-to-end through the
// REAL parser + the _readDelta feed mapping + the DEFAULT disk probe; scenarios: happy path,
// clarification round, no-md expiry, two handovers in one conversation, stale restart replay,
// chunked scanner-style parse equivalence
void TestCommandHandoverE2E();
// tests_commands.cpp — REAL-corpus replay (guarded, [info]-skips without a corpus): every command
// echo in the newest ~120 on-disk transcripts parses to a Command event, leaks ZERO turn events
// (the /model false-Running invariant corpus-wide), carries timestamps; real Write tool_use lines
// yield their file_path in fileWritePaths
void TestCommandEchoRealCorpus();
void BenchPromptAnchor();
void BenchPromptAnchorRealSession();
