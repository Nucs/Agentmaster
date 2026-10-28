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
//   tests_taskbar.cpp         - NOTIFICATIONS.md sections 8 + 9: the taskbar unread badge (TaskbarBadge.h's label / size / disc + digit composition / GDI icon, drawn like the shell draws it) + the engine's process-wide unread census + the unread frame on a window's taskbar preview (TaskbarThumbnail.h)
//   tests_turns.cpp           - the Copy > Transcript levels: TranscriptTurns (the C++ port of the am-agentmaster-cli skill's transcript_turns.py v9) vs the reference's own goldens in turns\expected, byte for byte; + the summary's whole-conversation chain (CollectConversationUpstream / BuildSummaryConversation)
//   tests_markdown.cpp        - the transcript overlay's Markdown flattener (MarkdownBlocks — cmark -> flat render blocks); compiled only when the vcpkg cmark of a full build is present (AM_HAVE_CMARK), else it reports a skip
//   tests_sidepanel.cpp       - SIDE_PANEL.md §2c: the side panel's in-place card list — the order planner (KeyedListDiff.h): reaches the target, indexes in range, minimal; + the cards' and rows' timing line (SessionTimingText.h)
//   tests_harness.cpp         - SDK_HARNESS.md + SESSIONS.md section 9: the agent harness's model-free half (AgentHarnessCore — the record, the digest, the --settings text, the child's environment, the readers of Claude Code's lines) and the Sessions page's AI search tools (SessionSearchTools — arguments, one search, the result text, the two tool handlers, the stepwise catalog gather)
//   tests_presets.cpp         - HARNESSES.md: PRESETS (LaunchPresets.h, pure) — the empty initial list + "Add presets…", the one-click template adds, the 0.6.8.18 cleanup, launch-presets.json, the value tiers, the session snapshot (launchArgs), every OpenRouter template's compile vs its launcher profile (MiMo, Kimi, Qwen, GLM), effort, top_k / require_parameters, refusals, the menus, the /handover hint's preset rule
//   tests_tabtools.cpp        - SESSIONS.md 9.14: the AI search's window and tab tools (the window choice, list / resume / fork / deactivate on fake windows, the per-request caps, the engine's answered fan-out) and the system prompts' structure
//   tests_rewind.cpp          - HOOKS.md "API-error turns": the API-ERROR REWIND (ApiErrorRewind.{h,cpp}) — the planner on Claude Code 2.1.291's transcript shapes (the 2026-10-17 mid-response drop, errors before output, a resume that died again, refusals, every refusal-to-act), the marker line, and the file-level append on temp files
//   tests_restorewake.cpp     - PERSISTENCE.md §14: the RESTORE WAKE (RestoreWake.h) — which restored tabs start by themselves after a restart (state AT the restart: anything but Idle / Waiting-for-you / Done, end to end through the restore seams + the registry), the per-window "its focused tab finished launching" gate, the idle-UI wake decision, the cross-window barrier, and the process-wide pacer
//   tests_voice.cpp           - MESSAGING.md §21.43 / VOICE.md §5.5: VOICE NOTES — the session vocabulary (VoiceVocabulary.h) vs the POC's goldens in voice\ (rules, context terms, prompt, hotwords, the dictionary pass) and the voice worker protocol (VoiceProtocol.h)
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
#include "../ConversationHosts.h" // one host tab per conversation that runs in two or more tabs (OBSERVER.md §20) - pure, header-only
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
#include "../RefreshPacing.h" // the Agent Manager page's registry-driven rebuild gap (UI_THREAD.md §12) - pure, header-only
#include "../RegexUtil.h" // the ONE guarded regex component (COMMANDS.md §6b) - pure, header-only
#include "../Scheduler.h" // DecideAdvance (pure)
#include "../SessionRegistry.h"
#include "../SessionScanner.h" // ParseTranscriptDelta (pure)
#include "../SessionSearch.h" // the Sessions page's two-phase search (SESSIONS.md §6)
#include "../SessionStore.h" // the generalized DURABLE per-session key/value store (titles, ...)
#include "../Sha256.h" // SHA-256 (the shipped command definitions' version identity) - pure, header-only
#include "../TranscriptStore.h" // the on-disk Claude-session store API (SESSIONS.md §6)
#include "../TranscriptWatcher.h" // the scanner's ReadDirectoryChangesW change feed (HOOKS.md "Transcript watcher")

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
void TestRegistryLeanReads(); // UI_THREAD.md §10a #4 — the lean registry reads: GetWithoutQueue (no queue, everything else), GetTabToken, LiveTabBindings (the SnapshotLive set + order, three fields), ManagedCodexTabTokens (live + ours + tokened Codex only), SnapshotLiveForScan (open prompts only, the four display texts empty, the scanner predicates unchanged), SnapshotLiveForCards (SIDE_PANEL.md §2c: no queue, lastAssistantText / recap / pendingPasteRefs empty, pendingInput KEPT); every read leaves the stored record byte-for-byte intact
void TestForkSourceIdEcho();
void TestSummaryChainLinks(); // the summary's conversation chain: a /clear's SessionStart(source "clear") paired with the conversation whose SessionEnd(clear) arrived on its ConPTY (beats a fresher child record), the ConPTY fallback (live before archived), other sources / no candidate / no token => no link, per-ConPTY + consumed entries, an unknown ended id, first writer wins
void TestRetiredHostToken(); // RETIRED HOSTS (SessionRegistry::RetireHostToken — the 2026-08-26 deactivate unbind + the restart archives): an observation / any hook from a replaced connection changes NOTHING on the record (token, pid, heartbeat, state, provenance); case-insensitive; per session; the new host still heard; Remove forgets; the per-session bound
void TestConversationHosts(); // DUPLICATE CONVERSATIONS (OBSERVER.md §20, ConversationHosts.h — the 2026-10-11..14 re-bind loop): PickConversationHost's rules (bound > recorded host > earliest known start > lowest WT_SESSION) and their stability under any claim order; FindDuplicateConversations' grouping (one tab = no entry, a tab listed twice = one tab, empty ids / tokens ignored, the recorded-host lookup asked once per duplicated conversation only, case-insensitive matches, sorted output); the incident's three-tab shape
void TestDuplicateConversationHosts(); // DUPLICATE CONVERSATIONS (SessionRegistry::SetDuplicateConversationHosts): a duplicate tab's hooks (SessionStart / UserPromptSubmit / Stop / Notification / SessionEnd) and observations change NOTHING on the record; the host is heard; another session's hook from the same tab is applied; case-insensitive; a host listed as its own duplicate and an empty id are ignored; the set is REPLACED, so a resolved duplicate is heard again
void TestRefreshPacing(); // UI_THREAD.md §12 (RefreshPacing.h): the Agent Manager page's registry-driven rebuild gap — 2× the last rebuild's cost, clamped to [250 ms, 5 s], saturating; the slow-rebuild log once a minute from 1 s
void TestTypedCapture();
void TestQueueHistoryTrim(); // bounded queue history: TrimQueueHistory (pure) + the registry append-seam cap
void TestTakeLastPendingPrompt(); // queue pop (Ctrl+Shift+Up on the empty compose box): TakeLastPendingPrompt (pure) — last-in-queue-order Pending/legacy-Held pops whole, completed history never, LIFO drain
void TestObserveClaude();
void TestSupersedeStaleTabSiblings();
// tests_spawn_sched.cpp
void TestSpawnBuilders();
void TestProfileBootstrap();
void TestTranscriptLookupLongPath(); // PERSISTENCE.md §16 (the 2026-10-28 audit): ResolveClaudeTranscriptPathIn finds a transcript whose path passes MAX_PATH (a deep working folder's project name), returning the plain form; short paths, missing ids, empty inputs and a missing projects folder as before
void TestWorkspaceTrust(); // workspace-trust seed: ClaudeWorkspaceTrustKey normalization + the SURGICAL SpliceWorkspaceTrust (already-trusted no-op, flip-in-place, insert-member, insert-projects, byte-preservation, no-clobber refusals)
void TestBridgeRoundTrip();
void TestScheduler();
void TestEnterRetry();
void TestDeliveryGate(); // DELIVERY.md: the per-session delivery gate — open/decline/owner-close/reclaim, the close-notify wake-up, DecideAdvance's hold, DecideEnterRetry's gate+dormant refusals, SubmitPrompt's lifecycles, the RC6 newline-folded echo consume
void TestLostSendReconciler(); // DELIVERY_PLAN.md R1 + DELIVERY.md §12: the PULL echo-consume (NoteExternalPrompt marks a fold-matched transcript line as `echoed` evidence, line-ts replay-guarded) + the pure DecideLostSend verdict matrix (injection-evidence-gated, settle from max(sentAt, injectedAt)) + DecideUndeliveredReclaim (never-injected + lapsed gate -> Pending retry) + MarkPromptInjected / RevalidateDeliveryGate / the per-attempt submitNonce gate tags + the `#6` shape end-to-end (Failed + autorunner paused, never resent)
void TestVerifiedPlacement(); // DELIVERY_PLAN.md Part 2 (R4-R8): the tri-state box verdict (InputBoxState), VerifyFillAgainstPrompt (exact / wrap-tolerant / collapse-aware / Partial / Foreign incl. the Incident-3 placeholder hazard), DecideSendUndo, the R7 merge classifier (push + pull; Failed + autorunner paused), the DecideAdvance/DecideEnterRetry box-state holds, ShouldWarnOnBoxNotVisible (the defanged R5 escalation - DELIVERY.md §12 warn-not-pause + the re-arm re-stamp window), SetPendingBoxState notify semantics, the verifySendBeforeSubmit round-trip
void TestBuildPromptSubmission();
void TestSchedulerIntegration();
void TestStateSettle(); // DELIVERY.md §16: the STATE SETTLE hold — StateSettleRemainingMs (both anchors, disabled, fail-open), DecideAdvance's placement/reasons/timed re-check (the gates that answer first - the dormant-tab gate included - and the send-spacing hand-off), the registry stamping at every seam (creation, keep on no-op, live/started flips, real vs stale Stop, ObserveClaude, queue insert, rollback keep), and the real scheduler end to end (hold, mid-window postpone, timed self-release, the queue anchor)
void TestInterruptHold(); // DELIVERY.md §14: the INTERRUPT HOLD — Esc parks an Auto (Full) autorunner (NoteInterrupt: fresh-marker belt, idempotent, live-only, notifies), the interrupt's synthesized Stop advances into "autorunner off (interrupt hold)", the user's next message resumes the held mode (push UPS — typed or a Send-now echo; never noise / empty / a UPS stamped before the marker; the pull twin NoteExternalPrompt by line stamp, deduped lines included), a user-set mode cancels the resume, and every pause backstop (stop-on-error / merge verdict) clears the hold
void TestUpdaterVersionLogic(); // Updater.h: ParseVersion/CompareVersion (4th part = build metadata, ignored) + unpackaged facts + the settings.json skip/postpone RMW (preserve-other-keys, replace-not-append)
void TestCmdPathLimit(); // CmdPathLimit.h (doc/agentmaster/CMD_PATH_LIMIT.md — the Ctrl+G fix): the PATH-length guard's pure rule (untouched at/under the trigger, dedupe first, missing-dir pass only when needed, verbatim order, quote-aware split, the key, the tail sample, never a live drop), the Win32 probe's safety edges, the process-env apply, and the incident end to end through a real cmd.exe
void TestHookInvocation(); // HOOKS.md "Hook invocation": the exec-form version gate (>= 2.1.139), the claude.exe PE version read, the exec host pick, BOTH hooks-settings shapes (exec: command+args, no shell string; shell: the classic string) with the per-event timeouts (SessionEnd 5 s, the rest 10 s), the forwarder's last_assistant_message Stop fast path ordered BEFORE the transcript tail read, and the materializer staying shell-form for an unreadable launcher
void TestStartupGuard(); // StartupGuard.h (doc/agentmaster/STARTUP_GUARD.md - the 2026-09-30 startup deadlock): the local FOLDERID constants resolve to the folders they claim, the known-folder pre-warm's counting + first-failure report, the kill policy (never a debuggee, never twice in a row), the marker round-trip, the log texts, and the watchdog's one-shot Armed -> Disarmed | Fired state machine incl. the deadline/disarm race
void TestPortableInstall(); // PortableInstall.h: the portable first-launch INSTALL question — the install.path decision file (encode/decode/round-trip, `machine` included), DecideStartup (the prelude's plan matrix, machine copies included), the account plan + the prompt default + the uninstall target (pure matrices), the manifest (safe names, parsing, the upgrade's shipped-only removal on a scratch tree), the PATH-list edits, the copy/preserve leaf rules, the right-click command, the dest guards, the version/package-name parsers, the live identity, GatherStartupFacts on a scratch dir, and a REAL unwritable folder (a deny ACL) read as a machine copy with home-map routing
void TestInstallKeepsPackageDataAndMap(); // PROFILES.md 2d (the 2026-10-26 install audit): the home map fails closed (ParseChoiceBytes with a BOM, ReadChoiceFileChecked missing vs unreadable vs a ridden-out lock, SaveChoiceIn refusing an unreadable map, PackageProfileDirFor / TryUserInstallProfileDir never guessing) + a package removal keeps the package's own files (PackageFamilyFromFullName, PackageDataFolderLeaf, KeepPackageDataIn: empty storage, nested files, terminal seeding, a failed copy)
void TestDialogEcho(); // CallerSink.h + DialogEcho.h (CLI.md §2b — every dialog we open is written to the caller's console): AGENTMASTER_CALLER_SINK's two forms (a console as a list of up to 8 pids) and every refusal, the launcher's console candidates (its parent first, itself / 0 / a repeat skipped, eight at most) round-tripping through the variable, the inherited-handle check, the sink's real UTF-8 write through a pipe with a Hold deferring the startup's release and a broken pipe dropping it, the launcher hand-over leaving nothing behind, the hand-off reply (encode / decode, the cut, a wrong magic, lying lengths, a foreign name refused, a real named section end to end), and what a message box / task dialog / command link / progress dialog prints and answers
void TestDialogEchoCoverage(); // CLI.md §2b's "make sure": the comment-and-literal stripper (line / block comments, strings, char literals, digit separators, raw strings), the direct-call finder with a negative control, and a scan of src\cascadia (from the exe's repository) that fails on any MessageBox / TaskDialog / IFileDialog::Show call outside DialogEcho.h and the one allowlisted upstream picker, and on a call of any other Win32 dialog family (common file / folder / color / font / print dialogs, shell message boxes, resource dialogs, credential prompts), which needs a wrapper first
void TestCliDispatch(); // CliDispatch.h (CLI.md §2a — the 2026-10-23 "profile in use" dialogs): the CLI-call rule shared by the launcher shim and the app exe (every verb and CLI-only flag routes, exact matches only; the app's command lines — bare, the reopen, -Embedding, the toast click, --register-user, --uninstall-portable, nt — stay the app's), CommandlineTail = the C runtime's lpCmdLine (quoted / bare / tab / glued quotes / quoting kept), the CLI's path and command line, the GUI exe's launch plan (caller handles / caller console / own console + creation flags), and TryRunCliCall answering nothing for a non-CLI command line
// tests_persistence.cpp
void TestPersistence();
void TestManagerLayout();
void TestWindowRecord();
void TestAppSettings();
void TestSummaryHistoryPersistence(); // the summary's conversation chain: continuedFromId/continuedVia round-trip (omitted when empty) + summaryPanelShowHistory (default ON, explicit OFF kept, the replaced summaryPanelShowPrevious neither read nor written)
void TestTabNamingAndColor();
void TestTabColorModes(); // tab color modes: enum/JSON round-trips + ChooseSessionAutoColor + SessionColorKeyDir/ResolveSessionColorHex
void TestEngineWindowLifecycle(); // Engine window lifecycle (local-Engine `...In` seams — NEVER SharedEngine() in tests): claim-by-id vs front-pop + the open-at-exit manifest's skip-empty rule + the reclaimable pool + Manager-only record deletion + ReserveManagerOnlyClose (PERSISTENCE.md 13.5 / Rule #16)
void TestUiStallEscalation(); // the ui-stall rescue/restart ladder's pure decision (Engine.h escalation block — the 2026-08-22 CoreMessaging dispatch wedge): declare/rescue/retry/restart gates + the never-fire belts (wake-from-sleep, pump-dead, half-alive, settings veto, once-latch)
void TestStateFileOverrides(); // PERSISTENCE.md §16 (the 2026-10-28 audit): a read that found a file unreadable marks it, so the save after the lock ended keeps a copy (once); tags.json / tag-colors.json refuse a read-modify-write over a file they cannot read and keep every tag and color - and a lock that ENDS during an add or a pick (the real race: a 150 ms release thread) keeps every tag and color; the session store keeps a record holding a member it cannot carry before a write or the emptied record's delete, and never deletes a record it cannot read
void TestUnreadableStateFiles(); // PERSISTENCE.md "Unreadable state files" (2026-10-26): the JSON reader (BOM, NUL / garbage roots, non-finite tokens), ReadStateFileUtf8 (missing vs unreadable, retries, quiet window), the replace guard (refuse / keep a copy / keep-before-replace), LoadSessionsForStartup + PreserveWorkspaceForRecovery, the session store's read-modify-write (over 1 MiB, torn, locked)
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
void TestSessionStoreScanCache(); // tags perf (2026-10-10) — the session store's SCAN CACHE, proven by counting file opens (SessionStoreRecordFileReads): the first whole-directory read opens each record once, a repeated one opens none, one changed record costs one open; an external change / add / delete is seen; a write of this process is written through; a read-modify-write (record, tag-colors.json, tags.json) reads the FILE, never a stale cached copy; the warm flag; whole-directory readers on four threads beside a writer
void TestExtractPathsFromText(); // tab color modes: absolute-path mining from shell command strings (quoted free-form; unquoted 1-space-in-folder-names) + the ClassifyTranscriptLine command wiring
void TestInferWorkingDirectory(); // tab color modes: the pure inferred-workdir picker (majority-deepest over tool-touched paths)
void TestSessionFolderFilter(); // SESSIONS.md section 10 - the Sessions page's folder filter (SessionFolderFilter.h): the folder universe (launch + inferred, once per session), RECENT / MOST USED orders, the find box, typed paths, the OR predicate (exact / subfolders at a segment boundary), GitRootMemo vs a counting probe and vs the real FindGitRootForDir
void TestConversationLineage();
void TestScannerCatchUp(); // the 2026-09-25 stuck-Running stall (session d1f3f198) END TO END — a REAL SessionScanner thread over staged transcripts: (a) a restored >1 MiB no-output turn + heartbeat idle -> released via the first-sight tail seek, (b) a >1 MiB append burst then quiet -> the backlog keeps reading on an unchanged size until end_turn, (c) a transcript ending in one >1 MiB line -> the alignment fallback replays from 0 without looping
void TestTurnStartRaceGuard(); // the 2026-10-01 TURN-START RACE (HOOKS.md "State machine"): NewerPromptNotYetInTranscript's truth table (hold / landed / equal / hookless / unknown tail / the grace edge / the legacy no-clock call) and its wiring into recon-stop, recon-stop-idle and recon-error; then END TO END on a REAL scanner thread woken on every notify like Engine.cpp: (a) the race holds Running, before and after the prompt line lands; (b) a genuine missed Stop still releases; (c) a prompt that never lands releases after the grace; (d) the race after a no-op turn (recon-stop-idle) holds
void TestScannerDormantHold(); // DEACTIVATE == the window-restored state (ScanSessionIsDormant): the predicate's truth table + END TO END on a REAL scanner — a parked (started=false) session's residue (a fresh mid-turn line on a primed cursor + a fresh subagent file) never promotes it to Running; the same activity once started does
void TestCutOffTurn(); // CUT-OFF TURNS (HOOKS.md "Cut-off turns" — the 2026-10-09 incident, session c47e90a2): the turnCutOff synth through the state machine (Error from every state, no turn-complete, no API-turn evidence), SessionStart(resume) keeping Error, RestoredMidTurnFlag, the parser's resume placeholder + interruptedByShutdown flag, ClassifyCutOffTurn's truth table (dormant / placeholder / seen-dormant / restore hint; the bare-prompt ambiguity; a fork's copied tail), the synth gate (never from Running; the turn-start guard), the stale-Error release, the heartbeat promotion's cut-off arm, the registry (message, no cache stamp, the hint spent by Running only), and END TO END on a REAL scanner over thirteen staged sessions: the incident reads Error and never ready, the autorunner does not send, and the three ways back to Running (the user's message, claude's heartbeat, fresh transcript lines); a live turn, a bare prompt, a fork, a user interrupt and a dismissed card are NOT cut off
void TestTranscriptWatcher(); // HOOKS.md "Transcript watcher": ClassifyProjectsRelativePath, the TranscriptIndex bookkeeping (add / modify / unknown-modify / side files / duplicate ids across folders / remove fallback / renames / folder add-remove-rename, case-insensitive; ClearPaths keeps sequences monotonic), the scanner's stat/side refresh gates + the hot predicate, and a LIVE ReadDirectoryChangesW watcher over a scratch projects tree (initial index, callback, new folder + transcript, append, subagent side file, delete, folder rename, stop) + a missing root that stays unhealthy and stops promptly
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
// tests_sidepanel.cpp — SIDE_PANEL.md §2c: the card list's ORDER planner (KeyedListDiff.h) — every planned
// Remove / Insert / Move applied in order reaches the target exactly, each index in range when it runs (the XAML
// panel's own preconditions), and the plan is MINIMAL (one remove per vanished card, one insert per new card,
// |survivors| - LIS moves against an independent O(n^2) LIS) — the named shapes (first build, emptying, a card to
// the top / bottom, a swap, a reversal, launch / close / filter), all 120 permutations of 5, a seeded random sweep
// with removals and insertions, and the release's 466-card list
void TestKeyedListDiff();
// SessionTimingText.h (SIDE_PANEL.md §2): the timing line of the side-panel cards, the Triage Board's cards and the
// Explorer tree's rows — "created 4d1h, active 2m4s ago": the user's examples, "created now" (< 5 min) / "created
// recently" (< 1 h), y / d / h for created, + m / s for active (seconds only under an hour, no month unit), the active
// part left out when it is 0, the unknown / clock-skew edges, and a 3-year sweep of the units (tests_sidepanel.cpp)
void TestSessionTimingText();
// PointerCardPlacement.h (SIDE_PANEL.md §2e): where the side panel's "+" launcher card opens — its corner at the click,
// extending toward the window's center (each axis by the click's half; the exact middle counts as left / top), clamped
// 8 DIP inside the window, a card larger than the window pinned to the near margin, an unmeasured window never
// clamped, NaN / infinite / negative inputs neutralized, and a seeded 20000-case sweep of the invariants
// (tests_sidepanel.cpp)
void TestPointerCardPlacement();
// VirtualListWindow.h (SIDE_PANEL.md §2f): which rows of the VIRTUALIZED side-panel card list get cards — the row
// arithmetic at exact boundaries and outside the content, the viewport clamp of a list that shrank, the NEAR / FAR
// hysteresis (kept while NEAR is covered, FAR otherwise, trimmed past twice FAR), the reveal range (it holds the row,
// and the plan after either minimum scroll keeps it), the spacer heights, the median stride, and seeded sweeps:
// every visible row has its card, every plan is a fixed point, a one-row-per-step scroll re-plans at most every 4
// rows (tests_sidepanel.cpp)
void TestVirtualListWindow();
// StatusIconMotion.h (SIDE_PANEL.md §2g): when the status icon's motions may run — the tab header's rule, shared with the
// side panel's card icon: nothing moves out of the live tree or in a hidden host (a collapsed panel); the 3 dots pulse only
// on a STARTED tab and the spark crown only with Windows' animation effects on (each still SHOWS standing still); the flash
// ring blinks whenever shown — an alert that ignores the animation switch and dormancy; plus the exhaustive 128-case
// sweep against an independent statement of each rule (tests_sidepanel.cpp)
void TestStatusIconMotion();
// tests_presets.cpp — HARNESSES.md, PRESETS (LaunchPresets.h, the pure core): the list starts EMPTY (no template is added
// by itself) and an empty list offers "Add presets…" with help text naming the way to the Presets tab (a list of hidden
// presets does not); one click adds a template as a linked, visible "p-" preset and "Add all tested" adds the five tested
// ones in order; a 0.6.8.18 file loses exactly the untouched presets that build added by itself; launch-presets.json
// round-trips and a malformed file reads as an empty config; the value tiers (box > Agentmaster env > Windows env > template / provider default) for a placeholder
// AND a snapshot; the snapshot freezes tiers 1-2, records only the NAME of a Windows variable, never stores a key, and
// round-trips as JSON; the MiMo Pro template compiles to the reference launcher's env block, and every OpenRouter
// template (MiMo Pro / Flash, Kimi K3, Qwen3.8, GLM-5.3) to its launcher profile's request body, limits and tested
// effort; effort as CLAUDE_CODE_EFFORT_LEVEL + --effort ("none" = neither); top_k / require_parameters validation +
// scope; the refusals (no key / no Base URL / a removed custom provider); reserved env names, forbidden claude flags,
// the key mask; the menu entries, the /handover hint spec, and the hint's bracketed-name-prefix rule for presets
void TestLaunchPresets();
// AgentHarnessCore.{h,cpp} (SDK_HARNESS.md), the model-free half of the agent harness: the conversation record (item
// caps, JSON round trip, a corrupt file = no conversation, atomic save / load / delete), the cold-start digest (which
// turns and lines, no tool results, the size bound), the --settings text (auto-compaction ON beside its window, the
// retry bound, the effort), the child's environment (the AM_HARNESS mark + the removals), the readers of Claude Code's
// lines over shapes captured from 2.1.291 (init, the three compaction lines, result with is_error), and the Fleet
// Observer's read of the harness mark (ParseClaudeFacts)
void TestAgentHarnessCore();
// SessionSearchTools.{h,cpp} (SESSIONS.md section 9), the Sessions page's AI search tools: the search arguments
// (defaults, clamps, errors written for the model), one search over a hand-built catalog with an injected content
// search (scope, the four match tiers, sorts, limit, deep only on request, a cancelled search), the result text
// (row fields, the "indexing" note of a partial catalog), the two tool handlers (short ids + collisions, show by
// ids / top / nothing, unknown ids, a cancelled request leaving no trace, no index at all), and GatherSessionCatalog
// on scratch folders (a gather that stops early says how far it got; steps always move forward; a refresh reads
// only what changed)
void TestSessionSearchTools();
// tests_sessiondb.cpp
void TestSessionDb(); // SESSIONS_DB.md — the session database's shared parts: protocol names + framing, the tool definitions, the settings clamps, the watcher's change set (D16), the facts epoch, the AI search's database mode
void TestMessagingStateFileLoad(); // PERSISTENCE.md §16 (the 2026-10-28 audit): LoadMessagingStateFile - a missing state.json is a first start; a good one loads with its bytes; a torn one, JSON that is not a state object, and a LOCKED one fall back to state.json.prev (or an empty map, said so) and are kept aside before the first save; no save while the lock lasts
void TestMessaging(); // MESSAGING.md — the fleet in Telegram, the pure core (tests_messaging.cpp): Telegram HTML escaping / Markdown conversion / chunking, the transcript-line classifier, the topics = tabs sync decider with the idle grace, the /command router with the refusal tables, the state file, the card, the remote prompt's runner bypass (D30) and the pin hold (§5.3)
void TestMessagingSearch(); // MESSAGING.md §5.4 / §21.7 — the 🔎 Search topic's pure rules (tests_messaging_search.cpp): /clear and /reset clear the search conversation, a session's command is refused there (it is not a session), the pick buttons' stateless callback data, the pick keyboard, where a picked session stands
void TestMessagingNewThread(); // MESSAGING.md §21.17 (tests_messaging_newthread.cpp): every title links to its topic (the token's THREAD part, TgTopicUrl, the linked classic render, the URL chip; the card, the change line and the Search cards name their thread; the hashed render unchanged), the "You" lines' title row (UserLineTitleRow, the three builders, RenderHistory / PlanHistoryPosts), the topic record's cardLinked + historyPromptMessageId, and the new-thread picker (the two lists from the registry's facts with the scratch roots and gone folders left out, pages, the buttons' data, labels, the text, the keyboard, a typed folder, a topic name without its decoration) + SessionRegistry::PlaceSamples
void TestMessagingCard(); // MESSAGING.md §21.24 (tests_messaging_card.cpp): the sticky card's two pages of buttons (BuildCardButtonRows / CardButtonFactsFor: Main and Session, the state's own action first — 🔧 Resume Session in Error, ▶ Reactivate when parked or exited —, every callback within Telegram's 64 bytes), §21.41's ⎋ Esc and 🌙 Deactivate (MessagingSessionControl.h: who gets which button, the hosting window's two decisions, the answers' words and texts, the Esc gap), the card's Error line, the styled screen snapshot (ScreenSnapshot.h: round trips with wide, surrogate and combining cells, trailing blanks, every malformed form refused) and the Screen Image renderer (ScreenImage.h: the grid arithmetic, the stretched box glyphs, a real PNG drawn with Direct2D and decoded again with WIC — its size, the screen's background, a colored cell, the padding)
void TestConversationImage(); // MESSAGING.md §21.39 (tests_conversation_image.cpp): 📜 All Screen, the whole conversation as tall pictures — the part planner (PlanTallImageParts), the tall renderer (REAL PNGs in parts: sizes, rows per part, no seam between strips, onPart, ASCII runs == the screen renderer's cells), the Claude Code-like layout on the turns goldens (BuildConversationSheet; cmark only), a ```diff block as "File diffs" draws it (numbers from the hunk headers, the green / red bands, the ellipsis between hunks, a git header vs a lone "--- " removed line, notes, wrapping) and RenderConversationImages end to end (File diffs = two more rows on the fixture)
void TestMessagingDirectory(); // MESSAGING.md §21.16 — the pinned directory (tests_messaging_directory.cpp): the topic links, the two stats rows, the tags x open tabs join, the rich and classic renders, the content hash, the post / edit decision, the state fields, the census kind, and the tag palette shared with the app (Rule #17)
void TestAskQuestion(); // MESSAGING.md §21.30 / INTERACTIVE_TOOLS.md §6 (tests_ask.cpp): Claude Code's AskUserQuestion answered from Telegram (AskQuestion.h, HooksBridge.h's ask pipe) — the answers composed as the PC composes them (the probe's goldens D / E / G, a plain pick, Tct's multi-select quoting, an own answer, the note's JS trim, the remote bounds), the allow / deny lines (ASCII, the input kept, answers + annotations replaced once), the 2.1.291 preview rules (None / Full / Withheld, the echo's TABs, controls, bidi, VS16, invisibles, the cell widths), the request line and its signature == the mirror's question item, the answer item WITH its notes, the card's callback data, text routing, steps, texts and buttons in every phase and within the budget, the previews' sheet drawn into a real PNG, Chat about this, the hooks settings' PermissionRequest entry (exec form only) and the hook script, and the ask pipe end to end (refused, answered, released, a helper that leaves, Stop)
void TestMessagingFiles(); // MESSAGING.md §21.32.4 (tests_messaging_files.cpp): the Telegram FILE LINKS — the session key (the custom SHA-256 against an independent implementation's answers, the secret's hex), the file URL forms and every refusal (a climb, an escaped separator, a stream, a share, broken UTF-8, a device name, a trailing dot or space), the request target, the Markdown rewrite (code skipped), the files never linked and the excluded folders, the settings' readers, the HTTP pieces, the linked set (dedupe, prune, JSON), the LAN choices and the peer filter, /download's argument, the server END TO END on loopback (whole, HEAD, a range, every refusal, a "secret" final path, the totals, a stop and a restart, a port in use) and the secret in Credential Manager (a scratch target)
void TestMessagingSteer(); // MESSAGING.md §21.33 (tests_messaging_steer.cpp): /steer, the words sent NOW (MessagingSteer.h) — the command's reading (any case, a @bot suffix, every line, /cc forcing it through, a longer word not taken, refused in the Search topic), the decision on every fact and state and the order of the facts, the texts (the queued cases, the usage line, the declined notice by its reason, the reactions), the prompt sent and queued and its place in the queue, and the watch's verdict on every status
void TestTuiMenus(); // MESSAGING.md §21.34 (tests_tuimenus.cpp): /add-dir's dialog answered from Telegram (TuiMenus.h) and the delivery fixes beside it (Scheduler.h) — the queued text read as the command, the screen parser against the probe's own captures (the dialog, the bare path input, the screen after an answer) and a terminal's other shapes (wrapped path and note, a cut label, another focus, a frame left behind, another menu, two dialogs, CRLF), the input box's detector reading the dialog as a menu, the paths, the window's press (argument, reply words, DecideMenuAnswer), terminal controls stripped, the echo and result lines, every /add-dir message classified, the card's data, text in every phase and buttons, the command echo matched to a delivered prompt, and the lost-send verdict held while a menu is open
void TestMessagingTaps(); // MESSAGING.md §21.28 (tests_messaging_taps.cpp): a tapped button's answer HELD while its work runs (MessagingTaps.h) — the toast's cut (never inside a surrogate pair), the answer's text (the handler's toast, the still-working note at the hold's limit), the log label, one tap's deadline and single answer, the holds (the sink told once, at the LAST hold, never through a sink that is gone), the scopes (nesting, a tap counted once, per thread, no new hold for an answered tap), and the whole path as the service runs it (a handler, two items, a callback's fallback, a replaced edit)
void TestClaudeStatus(); // MESSAGING.md §21.40 (tests_claude_status.cpp): /status shows what Claude Code's own /status shows — the headless /usage version gate, `claude auth status --text`'s lines, the `-p /usage` JSON answer (the result, a verbose array, an error, a MODEL turn that latches), the /usage limit lines and the contributions heading, the token and count wording, the MCP tool names, the transcript fold on 2.1.310's real line shapes (tokens once per response, the MCP fold reset at a compact boundary, the instruction files' full list and updates, the newest model / effort / mode / titles), the raw-byte filter, the report's sections and fallbacks and its split under Telegram's limit, the transcript FILE read (chunks, CRLF, a BOM, a torn last line), and the hidden child runner (cmd.exe: its output, exit code, timeout, environment)
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
// tests_taskbar.cpp — NOTIFICATIONS.md section 8, the TASKBAR UNREAD BADGE: the overlay renderer and the count behind it.
void TestTaskbarBadge(); // TaskbarBadge.h: Label (99+ cap) / Description (exact count) / IconSizeForDpi (SM_CXSMICON scaling, clamps), the disc coverage (centered, full-bleed, symmetric), ComposePixels (straight-alpha BGRA, white under ink, ink outside the disc dropped), FindInk, the real GDI glyph pass (inked, optically centered, never clipped by the disc) and CreateBadgeIcon drawn with DrawIconEx over a green canvas (transparent corners, opaque disc, white digits)
void TestUnreadCensus(); // Engine.h's process-wide unread census on a LOCAL Engine: the distinct-session union, fan-out to EVERY sink (the source included), silence on an unchanged count, empty-publish withdrawal, unregister, and a throwing sink contained without starving the rest; plus the union as a sorted id list (UnreadSessionUnion / CurrentUnreadSessionIdsIn — the side panel cards' flash-ring set, SIDE_PANEL.md §2g), including a same-count change no sink hears
void TestTaskbarThumbnail(); // TaskbarThumbnail.h, NOTIFICATIONS.md section 9 (the unread FRAME on a window's taskbar / Alt+Tab preview): FitInside never exceeds DWM's box (the probe's 704x381 -> 199x108, a seeded fuzz), EstimatedBox per DPI, FrameThickness / FrameColor (alpha floor), ForceOpaque, the box-filter DownscaleBgra (rounding, identity, growing, exact tiling), DrawFrame (band, alpha blend, clamp, no-ops), LooksBlank, SolidCard, the JobBook worker coalescer, and the GDI half on real DCs (DrawIconCentered, the top-down CreateTopDownDib, CaptureClient's refusals)
// tests_tabtools.cpp — SESSIONS.md 9.14: the AI search's window and tab tools, and its system prompts' structure.
void TestTabTools(); // WindowHandleOf / DescribeWindowsForModel / ResolveWindowArgument (one window = no choice; several + none named = refused with "ask the user" and every window described); the tabs server on fake windows: list (short ids, flags, the window / dir / title filters, the limit), resume (never twice, the window refusal, after by pos and id, validated), fork (the refusal names the source, the default placement, model), the caps (5 opens, 1 deactivate, per request), an unanswered vs a refused open, deactivate (one id, no tab, adopted, dormant, a turn in flight w/o + with cut_off_turn); the engine's answered fan-out on a LOCAL Engine (order, the sink's id, last-active, a silent window, a throwing sink, a double reply, one window, the host); both prompts' headings, plan rule, counts, tool names, no deep, ASCII
// tests_rewind.cpp — HOOKS.md "API-error turns": a session whose turn died on an API error resumes INTO that turn.
void TestApiErrorRewind(); // ApiErrorRewind.{h,cpp}: PlanApiErrorRewind on the incident's shape (half-streamed thinking + text above the synthetic isApiErrorMessage row, its turn_duration, cost-state and last-prompt lines) -> rewind to the tool result; an error before any output; a prompt's first response; a resume that died again (Claude Code's "Continue from where you left off." row stepped over); hook attachments after the error; a meta (skill) target; refusal / DLP / safety blocks, a conversation that went on, a user rewind, an already rewound file, a cleared conversation, a torn line, two responses above the error, a short window (NeedMoreHistory) vs the whole file (UnknownShape) — all left alone; the marker (valid JSON, Claude Code's key order, explicit, no "rewound", the newest lastPrompt incl. Hebrew + emoji, the session id); RewindApiErrorTail on temp files (one line appended at the old end, a second run writes nothing, torn / healthy / refusal files untouched, the 256 KiB -> 16 MiB widening, a missing file), the session-id front doors, and ProcessExitWait for an id nobody runs
// tests_rewind.cpp (second half) — HOOKS.md "Interrupted turns": the restart and fork questions of 2026-10-22.
void TestInterruptedTurns(); // ApiErrorRewind.{h,cpp} + InterruptedTurns.h: PlanInterruptRewind on the live probes' shapes (an Esc during a reply -> the prompt; during the second tool call -> the first call's result; during the first -> the prompt; a stopped fork past its own continuation row; a transient API error stepped over) and every refusal (a shutdown interrupt, a conversation that went on, an older interrupt, idempotent after the line, torn, two replies, a refused response, an earlier interrupt, short window vs whole file, meta / local-command / denied / toolDenialEndsTurn targets = WouldNotContinue); ClassifyResumeContinuation (tool-result tail, a call in flight dropped + named, its orphan thinking, a reply's text above it, finished replies, user vs shutdown interrupts, meta tails vs under an attachment, compaction, rejected results, local commands, a resent prompt, transient vs refused errors, Claude Code's own rewind, a partial last line); IsClaudeEnvTruthy + ResolveClaudeAutoContinue's precedence on scratch settings files (managed > local > project > user > spawn env > process env, a BOM, a JSON boolean) + the session front doors (AssessForkContinuation, the plan + one appended line + idempotence through the session id); DecideForkTurnStop's whole table; the dialogs' words
// tests_restorewake.cpp — PERSISTENCE.md §14: after a restart, the restored tabs that were busy wake by themselves.
void TestHeldTabRefs(); // PERSISTENCE.md §16 (the 2026-10-28 audit): HeldTabRefs.h - ShouldHoldRestoreRef's table (a refused launch and an unreadable shell are held; an unknown session only after a failed fleet load; restored / duplicate / live elsewhere / empty never), MergeHeldTabRefs (old index clamped, the focused shell's index follows, every save, a resolved ref leaves, an all-held window saves every ref), HeldSessionIds
void TestEngineHeldSessions(); // PERSISTENCE.md §16 (the 2026-10-28 audit): the engine's held-session set on a LOCAL engine (tests_persistence.cpp) - NoteWindowHeldSessionsIn replaces a window's list, an empty list clears it, empty ids and an empty window id are ignored, IsSessionHeldByAWindowIn answers across windows, and a closed window's set goes with it (UnregisterLiveWindowIn)
void TestRestoreWake(); // RestoreWake.h: RestoreWakeWantsSession's truth table (Idle / NeedsApproval / Error with the restart's mid-turn flag wake; an unpinned Waiting, Done and Running never; a pinned Waiting does — the pin hold's normalized Idle; started / closed / external / unflagged never) and END TO END through the restore seams + the registry (persisted Running / NeedsApproval / Error wake, Idle / Waiting / Done do not, the cut-off synth keeps the want, a started tab or a real turn ends it); the foreground gate (no terminal at once, a started non-Claude terminal, a Claude terminal only by a hook newer than the restore or the 15 s hookless settle, the 90 s cap, the once-only latch); the punctuality streak + the wake decision (never before the own foreground or the release; idle UI, else the 30 s cap); the barrier (announced windows hold the rest, all released at once, Leave unblocks, the 5-minute cap releases stragglers too, rounds, unknown keys never wait, idempotent Announce, Join without Announce, counts, the release reason); the pacer (500 ms spacing, 4 per 10 s, the drip's timeline on 250 ms ticks, four windows sharing one pace)
void TestVoiceVocabulary(); // VoiceVocabulary.h vs tools\voice-poc\vocab.py's goldens (tests\voice\): per-term squash / case split / aliases / distinctive / hard; the hard words mined from a sample CLAUDE.md (every filter); three vocabularies' terms, weights, sources and respell flags, Whisper's prompt at two budgets, the hotwords at two limits, the dictionary pass over 18 transcripts (text + respellings, idempotent); the owner's dictionary text; properties (an everyday sentence untouched, punctuation kept, the owner's trigger, blanks, a drive root); and no word asked the reference never asked
void TestVoiceNoteRoute(); // MessagingVoice.h: a voice note goes where a typed message of its thread goes (a new thread's wizard holds it, a binding topic's bind holds it, the main view / Search / an orphan refuse), the refusal texts, why a note cannot be read (in order), which messages are notes
void TestVoiceCorrection(); // VoiceCorrect.h vs llm_fix.py's goldens: the system prompt verbatim, the glossary line, the user prompt with and without other engines' hearings, the plausibility guard, every quoted argument read back by CommandLineToArgvW (the whole correction command line too), and the output reader (an object, a verbose array, an error result, not JSON)
void TestVoiceProtocol(); // VoiceProtocol.h: the hello / transcribe / quit lines (one line each, escaped paths and prompts), the answer reader (ok, failures with and without a reason, the hello's facts, a non-protocol line skipped, an answer without an id), and the transcript's prompt text (white space folded)
// tests_turns.cpp — the session copy menus' Copy > Transcript > Default / Max / Full / Compact / Minimal.
void TestSummaryConversation(); // SummaryConversation.h (pure): the summary's numbered conversation list — earlier sessions first, one separator per earlier session (titleOf, else the transcript's title), each prompt once (a fork's copy under its parent, keeping its jump index), own pre-/compact parts without one, the toggle OFF = the classic list, short uuid lists, Codex rows
void TestConversationUpstream(); // TranscriptTurns.h section 8 end to end on synthetic transcripts: a fork family three deep, a plan restart, recorded /clear + cross-folder /handover links (self / gone / throwing lookups), never downstream, the analyzer's uuid lockstep, G's whole numbered list, and the cache keeping only FINAL answers (no first prompt yet / young head / settled)
void TestTranscriptTurns(); // TranscriptTurns.{h,cpp}: the level ladder (TurnsOptionsForLevel == the reference's LEVELS; no level turns on Agentmaster's editDiffs), the timestamp + JSONL readers, a missing transcript -> nullopt, and all 11 skill fixtures x 5 levels rendered byte-identical to turns\expected (written by the reference script, turns\make_goldens.py) — chains + agents through RenderTranscriptTurnsFile with births.json's pinned creation times
void TestTranscriptTurnsEditDiffs(); // TurnsOptions::editDiffs (MESSAGING.md §21.39, All Screen's "File diffs"; not part of the port): off = every level's own text; on = each file change that went through as a ```diff block under its call (Claude Code's structuredPatch hunks; a created file as added lines from 1; old_string / new_string without hunks, a MultiEdit's edits in order; a NotebookEdit's new source; none for a refused or unanswered call), only ADDED lines below Max, Minimal / Compact listing just those calls (never an empty timeline), Max's Write content block replaced by its diff, and the reference's session fixture = its Default golden + one diff
// tests_markdown.cpp — the transcript overlay ("Show Transcript") renders a level's Markdown through MarkdownBlocks.
void TestMarkdownBlocks(); // MarkdownBlocks.{h,cpp} (cmark): block kinds + order, the container chain + ids (one quote's paragraphs share it, two quotes don't, a list inside a quote nests outermost-first), list markers (first block only, numbers kept, one marker width per list), MdGapAbove (heading / label / tight / loose / code), MdIsLabelParagraph (incl. the folded Full/Max stats line), raw HTML kept as text, soft breaks kept as line breaks, images never fetched, the block / character caps, a real transcript_turns golden end to end, and the overlay's TOOL AREAS (MdFindToolAreas: which lists are areas — only a top-level list right under a bold "●" speaker header — their item counts, the "(no tool calls)" empty area, MdToolAreaFoldable, the MdToolAreaSummary text) over an inline fixture and five goldens, and the SESSION CHAIN LINKS (MdFindSessionChain: the list under the bold "Session chain" intro + each session's first top-level "Session N of M" heading; MdFindSessionRefs: the "session N" mentions — not inside a word / a code span / a quoted title, 1..4096; the overlay's rule: link only where the session has a heading) over an inline fixture, the edge cases and four chain goldens. A skip (not a failure) when the build has no vcpkg cmark
void BenchPromptAnchor();
void BenchPromptAnchorRealSession();
