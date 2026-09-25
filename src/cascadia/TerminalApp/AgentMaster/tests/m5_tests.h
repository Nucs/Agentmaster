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
//   tests_invisible.cpp       - DELIVERY.md §15: claude's invisible-character rule mirrored (the generated wZe oracle table, the paste sanitize, the prompt compare key, Hebrew, echo/pull-echo/dedupe, the draft guard, the forwarder's UTF-8 + BOM)
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
#include "../DraftCarry.h" // the fork carry + paste journal core (PENDING_INPUT.md §11 / §2c) - pure, header-only
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
void TestRegistrySnapshotLive();
void TestForkSourceIdEcho();
void TestTypedCapture();
void TestQueueHistoryTrim(); // bounded queue history: TrimQueueHistory (pure) + the registry append-seam cap
void TestTakeLastPendingPrompt(); // queue pop (Ctrl+Shift+Up on the empty compose box): TakeLastPendingPrompt (pure) — last-in-queue-order Pending/legacy-Held pops whole, completed history never, LIFO drain
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
void TestLostSendReconciler(); // DELIVERY_PLAN.md R1 + DELIVERY.md §12: the PULL echo-consume (NoteExternalPrompt marks a fold-matched transcript line as `echoed` evidence, line-ts replay-guarded) + the pure DecideLostSend verdict matrix (injection-evidence-gated, settle from max(sentAt, injectedAt)) + DecideUndeliveredReclaim (never-injected + lapsed gate -> Pending retry) + MarkPromptInjected / RevalidateDeliveryGate / the per-attempt submitNonce gate tags + the `#6` shape end-to-end (Failed + autorunner paused, never resent)
void TestVerifiedPlacement(); // DELIVERY_PLAN.md Part 2 (R4-R8): the tri-state box verdict (InputBoxState), VerifyFillAgainstPrompt (exact / wrap-tolerant / collapse-aware / Partial / Foreign incl. the Incident-3 placeholder hazard), DecideSendUndo, the R7 merge classifier (push + pull; Failed + autorunner paused), the DecideAdvance/DecideEnterRetry box-state holds, ShouldWarnOnBoxNotVisible (the defanged R5 escalation - DELIVERY.md §12 warn-not-pause + the re-arm re-stamp window), SetPendingBoxState notify semantics, the verifySendBeforeSubmit round-trip
void TestBuildPromptSubmission();
void TestSchedulerIntegration();
void TestInterruptHold(); // DELIVERY.md §14: the INTERRUPT HOLD — Esc parks a Semi/Full autorunner (NoteInterrupt: fresh-marker belt, idempotent, live-only, notifies), the interrupt's synthesized Stop advances into "autorunner off (interrupt hold)", the user's next message resumes the held mode (push UPS — typed or a Send-now echo; never noise / empty / a UPS stamped before the marker; the pull twin NoteExternalPrompt by line stamp, deduped lines included), a user-set mode cancels the resume, and every pause backstop (stop-on-error / merge verdict) clears the hold
void TestUpdaterVersionLogic(); // Updater.h: ParseVersion/CompareVersion (4th part = build metadata, ignored) + unpackaged facts + the settings.json skip/postpone RMW (preserve-other-keys, replace-not-append)
void TestCmdPathLimit(); // CmdPathLimit.h (doc/agentmaster/CMD_PATH_LIMIT.md — the Ctrl+G fix): the PATH-length guard's pure rule (untouched at/under the trigger, dedupe first, missing-dir pass only when needed, verbatim order, quote-aware split, the key, the tail sample, never a live drop), the Win32 probe's safety edges, the process-env apply, and the incident end to end through a real cmd.exe
void TestPortableInstall(); // PortableInstall.h: the portable first-launch INSTALL question — the install.path decision file (encode/decode/round-trip), DecideStartup (the prelude's plan matrix), the PATH-list edits, the copy/preserve leaf rules, the right-click command, the dest guards, the version/package-name parsers, GatherStartupFacts on a scratch dir
// tests_persistence.cpp
void TestPersistence();
void TestManagerLayout();
void TestWindowRecord();
void TestAppSettings();
void TestTabNamingAndColor();
void TestTabColorModes(); // tab color modes: enum/JSON round-trips + ChooseSessionAutoColor + SessionColorKeyDir/ResolveSessionColorHex
void TestEngineWindowLifecycle(); // Engine window lifecycle (local-Engine `...In` seams — NEVER SharedEngine() in tests): claim-by-id vs front-pop + the open-at-exit manifest's skip-empty rule + the reclaimable pool + Manager-only record deletion + ReserveManagerOnlyClose (PERSISTENCE.md 13.5 / Rule #16)
void TestUiStallEscalation(); // the ui-stall rescue/restart ladder's pure decision (Engine.h escalation block — the 2026-08-22 CoreMessaging dispatch wedge): declare/rescue/retry/restart gates + the never-fire belts (wake-from-sleep, pump-dead, half-alive, settings veto, once-latch)
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
void TestScannerCatchUp(); // the 2026-09-25 stuck-Running stall (session d1f3f198) END TO END — a REAL SessionScanner thread over staged transcripts: (a) a restored >1 MiB no-output turn + heartbeat idle -> released via the first-sight tail seek, (b) a >1 MiB append burst then quiet -> the backlog keeps reading on an unchanged size until end_turn, (c) a transcript ending in one >1 MiB line -> the alignment fallback replays from 0 without looping
void TestCompactionLifecycle(); // HOOKS.md "Compaction": CompactionInFlight (the silent-window hold), the registry's PreCompact/PostCompact/SessionStart(compact) window, the delta's compact_boundary postTokens, ActiveBranchUuids crossing every compaction shape (logicalParentUuid, rewind exclusion, stale marker, two compactions, no-logical-parent boundary, cycle guard, the preserved tail written after the summary, a "summarize from here" partial compaction, a never-written logical parent, sidechain skip, a legacy re-append), the display readers over a compacted plan-restart child (parent link, duration, files, current-segment Messages, head read, Transcript/Followup, title) and over a re-appended history (each message once), and the presence procStart identity (decode + PresenceRowMatchesProcess + the raw read)
void TestConversationFollowup(); // the "Transcript Followup" copy (ReadConversationFollowupText): legend + per-turn folding (last assistant text wins), indents, batch glue, Codex, revert-aware lockstep, degenerate inputs, + a guarded live pin on the c0724e15 reference session
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
// TabDragMath.h DecideTabStripRevealOffset: the strip REVEAL decision behind every tab selection —
// a fully visible tab never moves under the funnel's leave-in-place rule, a clipped / far-off one is
// brought in with the pad, the card-click reveal always lands inside the pads, clamping to
// [0, ScrollableWidth], the even pad split on a narrow strip, wider-than-viewport headers, and
// degenerate / non-finite inputs that must never reach ChangeView
void TestTabStripReveal();
// TabCloseSuccessor.h (FAVORITES.md §4a): the tab focused after the FOCUSED tab closes — upstream's pick
// kept when it starts nothing; a DORMANT successor replaced by the nearest (in-order) / most recently
// used (MRU) tab that starts nothing; right-first ties, the Manager fallback, hostile/empty inputs
void TestTabCloseSuccessor();
void TestOrphanConsoleReaper(); // OBSERVER.md §13a: the dead-console reap decision + group walk (tests_transcript.cpp)
void TestPendingPaste();
// tests_summary_anchor.cpp — PENDING_INPUT.md §11 (the FORK CARRY) + §2c (the PASTE JOURNAL): DraftCarry.h's claude-mirrored
// paste normalization / newline count / collapse threshold, the wrap-tolerant placeholder scan (bare "[Pasted text #N]"
// included), the journal-first resolver (chronological same-M assignment, whole-line cache tier, live-vs-strict unresolved
// policy, image + duplicate-id refusals, the truncated middle), the piecewise fill plan, the fill verifier, the registry
// journal (normalize / collapse-only / de-dupe / per-session cap / move / clear / remove) and the ResolvePendingDraftCarryIn adapter
void TestDraftCarry();

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
// tests_invisible.cpp — DELIVERY.md §15: Claude Code 2.1.277+ CANCELS an Enter whose text holds an
// "invisible" character (the review prompt), which stranded every pasted delivery that carried one;
// ClaudeInvisibleChars mirrors claude's own rule so the paste is pre-cleaned, and folds the same
// characters (+ NFC) out of both sides of every prompt compare. Hebrew is covered throughout: the RLM/LRM
// keep rules, NFC-reordered niqqud, and the forwarder's UTF-8 decoding of a Hebrew hook payload.
void TestClaudeInvisibleOracle(); // the mirror vs Claude 2.1.280's OWN outputs (a generated table, field for field) + idempotence + the delivery invariant (a sanitized paste never trips the review cancel)
void TestClaudeInputSanitize(); // the pure tables, SanitizeForClaudeInput (VS16/RLM kept, ZWSP/ESC/tags gone, NFC niqqud), PromptFillBody/BuildPromptFill, NormalizePromptTextForCompare, the compares (draft guard / merge / verify / undo) and CountGraphemeClusters (the undo budget)
void TestInvisibleEchoAndDedupe(); // the registry half: push echo + NFC echo + pull-echo consume a sanitized delivery, the pull twin dedupes a Hebrew push row, the merge classifier sees through invisibles, and DecideEnterRetry presses for claude's cleaned box
void TestForwarderEncoding(); // HOOKS.md "Encoding": the forwarder decodes stdin as UTF-8 (never the console code page), reads bridge.json + the transcript tail as UTF-8, stays pure ASCII, and is written with a UTF-8 BOM
void BenchPromptAnchor();
void BenchPromptAnchorRealSession();
