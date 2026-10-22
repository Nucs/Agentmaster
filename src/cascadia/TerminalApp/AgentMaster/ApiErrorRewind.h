// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the API-ERROR REWIND (HOOKS.md "API-error turns"): a session whose turn died on an API
// error is resumed so that Claude Code CONTINUES that turn by itself, exactly as it continues a turn that
// was merely cut off (HOOKS.md "Cut-off turns").
//
// THE INCIDENT (2026-10-17, release 0.6.8.21, session 0391efeb "char plan"). A connection drop ended a
// 40-minute turn with "API Error: Connection lost mid-response. The response above may be incomplete."
// "Restart session" resumed the conversation and the new claude just sat there; a fork did the same.
//
// WHY (read off Claude Code 2.1.291's own resume code, then reproduced live): on --resume Claude Code
// classifies the tail of the active branch. It already SKIPS an API-error row (isApiErrorMessage — every
// kind except a refusal, a DLP denial and a safety-monitor block) and, when a tool result or a prompt is
// below it, appends its own meta "Continue from where you left off." and — with
// CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 in the user's settings — sends it. A drop AFTER output started
// (the error row's truncatedAfterOutput) leaves the half-streamed response's rows (thinking, text, …)
// ABOVE the error; the classifier stops at that assistant row and calls the turn complete. So an error
// before any output resumes on its own, and an error mid-response never does.
//
// THE FIX: append ONE line,
//     {"type":"last-prompt","lastPrompt":…,"leafUuid":"<the user row below the failed response>","explicit":true,"sessionId":…}
// — the very line Claude Code's own rewind (double-Esc) writes, minus its "rewound" flag (that flag
// would make the next resume refuse to continue). The loader takes that row as the active leaf; the
// half-response and the error stay in the file on a dead branch (NOTHING is deleted or rewritten); the
// classifier sees a tool result / a prompt as the tail; the resumed claude continues the turn. Verified
// on a throwaway session: without the line no continue, with it "Continue from where you left off." and
// the reply (the experiment is in this change's commit message).
//
// ⚠ TIMING — append only while NO claude.exe runs the conversation. A claude that ran the failed turn
// remembers the error branch as its leaf and RE-STAMPS it at exit (process.on("exit") ->
// reAppendSessionMetadata writes a non-explicit last-prompt naming its own leaf). Measured: a line
// appended while that claude was alive was overridden by the line its exit wrote. RewindApiErrorTail
// therefore refuses while any live claude's heartbeat names the conversation, and the seams that END a
// running claude first (Restart, Deactivate) wait for its exit with ProcessExitWait.
//
// Pure planner (PlanApiErrorRewind) + one file-level apply (RewindApiErrorTail) + the holder wait. Plain
// C++ + Win32, no WinRT: compiled into TerminalAppLib and the standalone m5 harness (TestApiErrorRewind).
//
// THE SECOND REWIND — a turn the USER interrupted (HOOKS.md "Interrupted turns", 2026-10-22; the user's request:
// "When restarting a session and user has interrupted the session, ask user if he/she would like to uninterrupt
// (we edit the session transcripts on disk before restart)"). Esc mid-turn makes Claude Code write an interrupt
// row ("[Request interrupted by user]", or "... for tool use" during a tool call) under the cut-off reply, and
// its resume classifier counts such a row as a FINISHED turn (2.1.291's _Mt: only an interruptedByShutdown row
// is continued). The same one line, aimed at the user row below the interrupted reply, makes the resumed claude
// continue — measured live against the e2e mock (tools/e2e-spike) with the real 2.1.291: an interrupt during a
// reply resent the prompt, one during the second tool call continued from the first tool's result, and an
// interrupted fork continued after its own "Continue from where you left off." row was stepped over. It is
// never automatic: the Restart seam asks (InterruptedTurns.h, RestartTurnChoice). Same TIMING rule: append only
// once the old claude has exited.
//
// THE CLASSIFIER — would a resume (a fork) continue the turn by itself? (ClassifyResumeContinuation, a mirror of
// 2.1.291's bfo over the active branch's tail.) The Fork seams ask "fork interrupted or uninterrupted?" only when
// it says yes (or the source runs right now) and Claude Code's CLAUDE_CODE_RESUME_INTERRUPTED_TURN is on where
// the fork runs (ResolveClaudeAutoContinue).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "InterruptedTurns.h" // IsClaudeEnvTruthy (ResolveClaudeAutoContinue) + the restart / fork choices these planners serve

namespace Agentmaster
{
    // What PlanApiErrorRewind found at the tail of a transcript. Only Rewind leads to a write.
    enum class ApiErrorRewindVerdict
    {
        // The active branch does not end in an API error (the usual case: a finished turn, a running
        // one, an already-rewound one). Nothing to do — and nothing logged by the seams.
        NotApplicable,
        // Append ApiErrorRewindPlan::markerLine: the active leaf moves to targetUuid.
        Rewind,
        // The active branch ends in a refusal, a DLP denial or a safety-monitor block. Never rewound:
        // resending would repeat a request Anthropic refused, and Claude Code itself counts these turns
        // as complete.
        RefusalError,
        // An API error ends the branch, but the rows above it are not "one cut-off response over a
        // user row" (two different responses with no user row between them, an error at the very root).
        // Left alone rather than guessed at.
        UnknownShape,
        // The walk up from the error left the bytes the caller read before it reached a user row (or the
        // active-leaf marker names a row older than the window). Re-plan with a larger window.
        NeedMoreHistory,
        // The file does not end with a newline: a writer is mid-line, or one died mid-line. Appending
        // would glue the marker onto that torn line, so nothing is appended.
        TornTail,
    };

    // The planner's verdict plus everything the seams log about it.
    struct ApiErrorRewindPlan
    {
        ApiErrorRewindVerdict verdict{ ApiErrorRewindVerdict::NotApplicable };
        // Rewind only: the UTF-8 JSON line to append, WITHOUT its trailing newline (the writer adds it).
        std::string markerLine;
        // Rewind only: the uuid of the user row the conversation is rewound to (the new active leaf).
        std::wstring targetUuid;
        // Rewind only: what that row is — "tool result" or "prompt" (Claude Code continues the turn /
        // resends the prompt) or "meta" / "compact summary" (it will NOT continue by itself; the rewind
        // still takes the error and the half-response off the active branch). Log text.
        std::wstring targetKind;
        // The error row's uuid, its text (first line, at most 160 characters), its `error` field
        // ("server_error", "rate_limit", "overloaded", … — empty when absent) and its timestamp.
        std::wstring errorUuid;
        std::wstring errorText;
        std::wstring errorKind;
        std::wstring errorTimestamp;
        // Rows of the cut-off response stepped over (its thinking / text / tool_use rows).
        size_t partialRows{ 0 };
        // "Continue from where you left off." rows stepped over (a resume that died again).
        size_t resumePromptRows{ 0 };
        // Transcript rows written after the error (its turn_duration, hook attachments of later resumes).
        size_t rowsAfterError{ 0 };
        // Why a non-Rewind verdict was reached, for the log ("" for NotApplicable / Rewind).
        std::wstring reason;
    };

    // PURE: plan the rewind from the LAST bytes of a transcript.
    //   tailUtf8          — the bytes from some offset to the END of the file (UTF-8 JSON lines).
    //   windowIsWholeFile — true when tailUtf8 starts at byte 0. False: the first line may be partial and
    //                       is dropped, and a walk that leaves the window answers NeedMoreHistory instead
    //                       of UnknownShape.
    //   sessionId         — written into the marker's "sessionId"; empty takes the error row's own.
    // Returns the plan; never throws (a line that fails to parse is skipped, any internal exception
    // degrades to NotApplicable with the reason "planner failed").
    // The active leaf follows Claude Code's loader: the last last-prompt marker's leafUuid when that row
    // exists — replaced by the last transcript row when that row descends from it and the marker was not
    // an explicit one at the very end — else the last transcript row; then up through system /
    // attachment / progress rows to the first user / assistant row.
    ApiErrorRewindPlan PlanApiErrorRewind(std::string_view tailUtf8, bool windowIsWholeFile, std::wstring_view sessionId) noexcept;

    // How a file-level rewind ended.
    enum class ApiErrorRewindOutcome
    {
        NotNeeded, // the plan was NotApplicable (or the transcript does not exist yet): nothing written
        Appended, // the marker line was appended
        Skipped, // a rewind was due or possible but not done: see detail (a live claude, a torn tail, …)
        Failed, // an I/O error: see detail (nothing, or at most nothing usable, was written)
    };

    // A file-level rewind's result.
    struct ApiErrorRewindResult
    {
        ApiErrorRewindOutcome outcome{ ApiErrorRewindOutcome::NotNeeded };
        ApiErrorRewindPlan plan;
        // Appended only: the file size before the append (the marker's byte offset).
        uint64_t appendedAtByte{ 0 };
        // Skipped / Failed: why, in one sentence. Empty otherwise.
        std::wstring detail;
    };

    // Plan + append, for one transcript file. Reads the last 1 MiB, then 16 MiB, then the whole file
    // (at most 512 MiB) while the planner asks for more history; refuses while a live claude's heartbeat
    // names `sessionId` (its exit re-stamp would undo the line — see the header comment); re-checks that
    // the size did not change between the read and the append; appends the marker plus "\n" with ONE
    // FILE_APPEND_DATA write, the way claude itself appends.
    //   transcriptPath — the conversation's .jsonl.
    //   sessionId      — the conversation's id (the holder check and the marker's sessionId).
    // Returns the outcome; never throws (every failure is a Failed / Skipped result with its detail).
    // ⚠ Blocking file I/O (normally one read of <= 1 MiB): fine on the UI thread for a single explicit
    // action, never inside a per-tab loop without a cheap pre-filter.
    ApiErrorRewindResult RewindApiErrorTail(const std::wstring& transcriptPath, std::wstring_view sessionId) noexcept;

    // RewindApiErrorTail for a session id: resolves the transcript (ResolveClaudeTranscriptPath). A
    // session with no transcript yet (never prompted) is NotNeeded.
    //   sessionId — the conversation to rewind.
    // Returns the outcome; never throws.
    ApiErrorRewindResult RewindApiErrorTailForSession(std::wstring_view sessionId) noexcept;

    // Only the planning half of RewindApiErrorTailForSession — no holder check, no write. The Restart
    // seam asks it BEFORE it ends the old claude, to know whether it must hold the new connection's
    // Start until the old one has exited.
    //   sessionId — the conversation to look at.
    // Returns the plan (NotApplicable when there is no transcript); never throws.
    ApiErrorRewindPlan PlanApiErrorRewindForSession(std::wstring_view sessionId) noexcept;

    // One hooks.log line (no "\n") for a result: "[api-error-rewind] <sid8> <seam>: …". `seam` names the
    // caller ("restart", "deactivate", "restore").
    std::wstring DescribeApiErrorRewind(const ApiErrorRewindResult& r, std::wstring_view sessionId, std::wstring_view seam);

    // Handles on the live claude.exe processes running one conversation, opened NOW — before the caller
    // tells them to close — so the later wait can never be fooled by a recycled PID. A process counts
    // when its own heartbeat (~/.claude/sessions/<pid>.json) names the conversation and the file's
    // procStart / startedAt matches the process (PresenceRowMatchesProcess). Move-only; the destructor
    // closes the handles.
    class ProcessExitWait
    {
    public:
        ProcessExitWait() = default;
        ~ProcessExitWait();
        ProcessExitWait(ProcessExitWait&& other) noexcept;
        ProcessExitWait& operator=(ProcessExitWait&& other) noexcept;
        ProcessExitWait(const ProcessExitWait&) = delete;
        ProcessExitWait& operator=(const ProcessExitWait&) = delete;

        // Open the live holders of `sessionId`. An empty result (none running, or none we may open) is
        // valid: WaitAll then returns true at once. Never throws.
        static ProcessExitWait ForSession(std::wstring_view sessionId) noexcept;

        // Wait until every held process has exited, at most `timeoutMs` in total.
        // Returns true when all of them exited (true at once with nothing held); false on the timeout.
        // Never throws. Blocking — call it off the UI thread.
        bool WaitAll(uint32_t timeoutMs) noexcept;

        // The held processes' ids (for the log), in the order they were found.
        std::vector<uint32_t> Pids() const;
        size_t Count() const noexcept { return _procs.size(); }

    private:
        void _CloseAll() noexcept;
        std::vector<std::pair<uint32_t, void*>> _procs; // (pid, process HANDLE opened with SYNCHRONIZE)
    };

    // The pids of the live claude.exe processes whose heartbeat names `sessionId` (ProcessExitWait's
    // rule, handles closed again). Empty when none. Never throws.
    std::vector<uint32_t> LiveClaudeProcessesForSession(std::wstring_view sessionId) noexcept;

    // How long the Restart / Deactivate seams wait for the old claude to exit before they give up on the
    // rewind (Windows ends a console client that ignores CTRL_CLOSE after 5 s; a graceful exit measured
    // ~1 s, a 72-tab drain up to ~3 s per host). The restart still starts after a timeout, un-rewound.
    inline constexpr uint32_t kApiErrorRewindExitWaitMs = 15000;

    // ===== the USER-INTERRUPT rewind (HOOKS.md "Interrupted turns") =========================================

    // What PlanInterruptRewind found at the tail of a transcript. Only Rewind leads to a write (and only after
    // the user said so — the planner itself decides nothing).
    enum class InterruptRewindVerdict
    {
        // The active branch does not end in a user interrupt: a finished turn, a running one, a conversation
        // that went on after the Esc, or an interrupt Claude Code made itself at a shutdown
        // (interruptedByShutdown — continued by Claude Code with no help). Nothing to ask, nothing logged.
        NotApplicable,
        // Append InterruptRewindPlan::markerLine: the active leaf moves to targetUuid and a resumed claude
        // continues the turn (targetKind "prompt": the prompt is sent again; "tool result": it carries on).
        Rewind,
        // The interrupt is the tail, but the row the rewind would land on is one Claude Code never continues
        // from (a meta row — a slash command's expanded text —, a compact summary, a local command's echo, a
        // tool result the user denied): a rewind would only hide the interrupted reply. Left alone, not asked.
        WouldNotContinue,
        // The rows above the interrupt are not "one cut-off reply over a user row" (two replies with no user
        // row between, a refused response, an earlier interrupt, a parent cycle, the root). Left alone.
        UnknownShape,
        // The walk left the bytes read before it reached a user row (or the active-leaf marker names an older
        // row): re-plan with a larger window.
        NeedMoreHistory,
        // The file does not end with a newline (a writer is mid-line): nothing may be appended to it.
        TornTail,
    };

    // The interrupt planner's verdict plus everything the seams show and log about it.
    struct InterruptRewindPlan
    {
        InterruptRewindVerdict verdict{ InterruptRewindVerdict::NotApplicable };
        // Rewind only: the UTF-8 JSON line to append, WITHOUT its trailing newline (the writer adds it) —
        // byte for byte the API-error rewind's line shape (type, lastPrompt, leafUuid, explicit, sessionId).
        std::string markerLine;
        // Rewind / WouldNotContinue: the uuid of the user row the conversation would be rewound to.
        std::wstring targetUuid;
        // What that row is: "prompt" or "tool result" (continued) — or "meta", "compact summary", "local
        // command", "denied tool result" (never continued: WouldNotContinue).
        std::wstring targetKind;
        // "prompt" only: the prompt's first line, at most 160 characters (the restart question quotes it).
        std::wstring targetText;
        // The interrupt row: its uuid, its timestamp, and whether it was written during a tool call
        // ("[Request interrupted by user for tool use]") rather than during a reply.
        std::wstring interruptUuid;
        std::wstring interruptTimestamp;
        bool duringToolUse{ false };
        // Rows of the interrupted reply stepped over (its thinking / text / tool_use rows).
        size_t partialRows{ 0 };
        // Results of the interrupted tool call(s) stepped over (the "doesn't want to proceed" rows of the batch
        // the Esc cut — the rewind re-does that step).
        size_t discardedResults{ 0 };
        // "Continue from where you left off." rows stepped over (a resume's or a fork's own continuation that
        // the Esc then stopped — the probe's interrupted fork).
        size_t resumePromptRows{ 0 };
        // Transient API-error rows stepped over (a retried request the Esc ended).
        size_t errorRows{ 0 };
        // Transcript rows written after the interrupt (hook attachments, turn bookkeeping).
        size_t rowsAfterInterrupt{ 0 };
        // Why a non-Rewind verdict was reached, for the log ("" for NotApplicable without a story / Rewind).
        std::wstring reason;
    };

    // PURE: plan the interrupt rewind from the LAST bytes of a transcript (PlanApiErrorRewind's contract: the
    // bytes from some offset to the end; windowIsWholeFile says whether they start at byte 0; sessionId goes
    // into the marker, empty takes the interrupt row's own).
    //   tailUtf8          — the UTF-8 JSON lines.
    //   windowIsWholeFile — true when tailUtf8 starts at byte 0 (else the first line may be partial and a walk
    //                       leaving the window asks for more history).
    //   sessionId         — the conversation id written into the marker line.
    // Returns the plan; never throws (any internal exception degrades to NotApplicable "planner failed").
    // The walk, from the interrupt row up: bookkeeping rows (system / attachment / progress) are stepped over;
    // assistant rows of ONE response are the interrupted reply (a second response id = UnknownShape); a
    // transient API-error row is stepped over (a refusal-class one = UnknownShape); Claude Code's own
    // "Continue from where you left off." meta row is stepped over; during a tool call, the tool results above
    // the interrupt and below the reply are the cut batch and are stepped over; the first other user row is
    // the target, continued only when it is a prompt or a tool result Claude Code's classifier continues (not
    // denied, not backgrounded by the abort, no toolDenialEndsTurn in its batch).
    InterruptRewindPlan PlanInterruptRewind(std::string_view tailUtf8, bool windowIsWholeFile, std::wstring_view sessionId) noexcept;

    // A file-level interrupt rewind's result (the outcomes are the API-error rewind's four).
    struct InterruptRewindResult
    {
        ApiErrorRewindOutcome outcome{ ApiErrorRewindOutcome::NotNeeded };
        InterruptRewindPlan plan;
        // Appended only: the file size before the append (the marker's byte offset).
        uint64_t appendedAtByte{ 0 };
        // Skipped / Failed: why, in one sentence. Empty otherwise.
        std::wstring detail;
    };

    // Plan + append for one transcript file — RewindApiErrorTail's mechanics exactly (the widening reads, the
    // live-holder refusal, the size re-check, ONE FILE_APPEND_DATA write).
    //   transcriptPath — the conversation's .jsonl.
    //   sessionId      — the conversation's id (the holder check and the marker's sessionId).
    // Returns the outcome; never throws. ⚠ Blocking file I/O: call it off the UI thread, after the old claude
    // exited (the Restart seam's _StartAfterInterruptRewind).
    InterruptRewindResult RewindInterruptTail(const std::wstring& transcriptPath, std::wstring_view sessionId) noexcept;

    // RewindInterruptTail for a session id (ResolveClaudeTranscriptPath). No transcript = NotNeeded.
    //   sessionId — the conversation to rewind.
    // Returns the outcome; never throws.
    InterruptRewindResult RewindInterruptTailForSession(std::wstring_view sessionId) noexcept;

    // Only the planning half of RewindInterruptTailForSession — no holder check, no write. The Restart seams ask
    // it BEFORE they end the old claude (whether to ask the user; whether to hold the new connection's Start).
    //   sessionId — the conversation to look at.
    // Returns the plan (NotApplicable when there is no transcript or it cannot be read); never throws.
    // ⚠ Reads at most 256 KiB in the common case (a bigger window only for a huge tool result): fine on the UI
    // thread for one explicit action, never in a per-tab loop.
    InterruptRewindPlan PlanInterruptRewindForSession(std::wstring_view sessionId) noexcept;

    // One hooks.log line (no "\n") for a result: "[interrupt-rewind] <sid8> <seam>: …". `seam` names the
    // caller ("restart").
    //   r         — the result.
    //   sessionId — the conversation (its short id leads the line).
    //   seam      — the caller's name.
    // Returns the line. Throws std::bad_alloc only.
    std::wstring DescribeInterruptRewind(const InterruptRewindResult& r, std::wstring_view sessionId, std::wstring_view seam);

    // ===== would a resume / a fork continue the turn by itself? =============================================

    // What Claude Code's resume would do with the active branch's tail (2.1.291's bfo, mirrored).
    enum class ResumeContinuation
    {
        None, // a finished turn (a reply, a user interrupt, a meta / compact / local-command tail): it waits
        ContinuesTurn, // "interrupted_turn": it appends "Continue from where you left off." and goes on
        ResendsPrompt, // "interrupted_prompt": it sends the tail's prompt again
        NeedMoreHistory, // the tail's walk left the bytes read: classify again with a larger window
    };

    // The classifier's answer plus what the fork question shows.
    struct ResumeContinuationPlan
    {
        ResumeContinuation verdict{ ResumeContinuation::None };
        // What the deciding row is: "reply", "tool result", "prompt", "interrupt", "meta", "compact summary",
        // "local command", "denied tool result", "task notification", "empty" — log and question text.
        std::wstring tailKind;
        // ResendsPrompt: the prompt's first line (at most 160 characters).
        std::wstring promptText;
        // A tool call in flight at the tail (an assistant tool_use with no result yet): its tool name. Claude
        // Code's loader drops such a call and continues from the row before it, so a fork RUNS IT AGAIN — the
        // probe's fork of a session inside `sleep 40` started its own `sleep 40`.
        std::wstring droppedTool;
        // Why None, for the log.
        std::wstring reason;
    };

    // PURE: classify the tail like Claude Code's resume does (the same active-leaf rules as the planners above;
    // a partial last line is ignored, so a transcript caught mid-write classifies on its complete lines).
    //   tailUtf8          — the last bytes of the transcript.
    //   windowIsWholeFile — true when they start at byte 0.
    // Returns the plan; never throws (a failure is None "classifier failed").
    // Mirrored rules: system / progress rows and transient API-error replies are stepped over from the tail; an
    // assistant tool_use row at the tail is a dropped call (and a thinking-only row right above it goes with it);
    // any other assistant row = None; a user tail continues unless it is meta, a compact summary, a local
    // command's echo, a user interrupt (a shutdown interrupt continues), or a denied / backgrounded tool result;
    // an attachment tail continues from the first user row above it under the same exceptions, and a reply above
    // it means None. A leaf set by Claude Code's own rewind (last-prompt "rewound":true) means None.
    // Unknown to the mirror (and so never predicted): Claude Code's special tool cards, max-turns endings.
    ResumeContinuationPlan ClassifyResumeContinuation(std::string_view tailUtf8, bool windowIsWholeFile) noexcept;

    // ClassifyResumeContinuation for a session id (the transcript's last 256 KiB, widening like the planners). A
    // session with no transcript is None "no transcript".
    //   sessionId — the conversation to look at.
    // Returns the plan; never throws. Blocking file I/O (one small read in the common case).
    ResumeContinuationPlan ClassifyResumeContinuationForSession(std::wstring_view sessionId) noexcept;

    // Where Claude Code's CLAUDE_CODE_RESUME_INTERRUPTED_TURN stands for a claude started in `cwd` with
    // `spawnEnv` added to Agentmaster's own environment.
    struct AutoContinueSetting
    {
        bool on{ false }; // the switch reads truthy (IsClaudeEnvTruthy)
        std::wstring source; // where the deciding value came from (a settings file's path, "spawn env", "process env", "nowhere")
        std::wstring value; // the raw value found ("" when none)
        bool fromSettingsFile{ false }; // a settings file decided (it outranks any environment a launch could set)
    };

    // Resolve the switch the way Claude Code applies it: a settings file's `env` block overrides the process
    // environment, and the first source that sets the key wins in Claude Code's precedence — the managed policy
    // (%ProgramFiles%\ClaudeCode\managed-settings.json, then %ProgramData%\ClaudeCode\…), the project's
    // .claude\settings.local.json and .claude\settings.json under `cwd`, the user's <config>\settings.json
    // (CLAUDE_CONFIG_DIR, else %USERPROFILE%\.claude) — then the spawn environment, then this process's own.
    // The --settings file Agentmaster passes (a preset's env) is not read: a preset does not set this key.
    //   cwd      — the claude's working directory ("" = no project settings).
    //   spawnEnv — the variables the launch adds (ClaudeSpawnSpec::env; empty for none).
    // Returns the setting; never throws (an unreadable file is skipped; a failure is off, source "error").
    AutoContinueSetting ResolveClaudeAutoContinue(std::wstring_view cwd, const std::vector<std::pair<std::wstring, std::wstring>>& spawnEnv) noexcept;

    // The variable's name, for the spawn environment and the logs.
    inline constexpr std::wstring_view kClaudeResumeInterruptedTurnEnv = L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN";

    // Does a fork of a session need the "fork interrupted or uninterrupted?" question? The ONE rule the PC's fork seams
    // and the Telegram bot share (HOOKS.md "Interrupted turns").
    struct ForkContinuationOutlook
    {
        bool ask{ false }; // a fork would (or may) continue the source's turn by itself: ask
        bool sourceRunning{ false }; // the caller said the source runs right now (Running / NeedsApproval)
        ResumeContinuationPlan tail; // what the source's transcript ends in (ClassifyResumeContinuationForSession)
        AutoContinueSetting autoContinue; // CLAUDE_CODE_RESUME_INTERRUPTED_TURN where the fork would run
        std::wstring why; // why not asked (for the log): no transcript, the switch is off, the turn is finished
    };

    // Assess a fork of `sourceId`: ask when CLAUDE_CODE_RESUME_INTERRUPTED_TURN is on where the fork runs (a fork that
    // cannot send the continuation by itself needs no stopping) AND either the source's transcript tail is one Claude
    // Code continues or the source runs right now (a running session's tail changes every second; its state is the
    // better guide). A source with no transcript forks into a fresh session: never asked.
    //   sourceId      — the session forked.
    //   sourceRunning — the registry says Running / NeedsApproval (false for a closed or unknown session).
    //   cwd           — where the fork runs (the project settings that may set the switch).
    //   spawnEnv      — the environment the fork's launch adds (ResolveSessionEnv for the dir).
    // Returns the outlook; never throws (a failure is "do not ask", logged).
    ForkContinuationOutlook AssessForkContinuation(std::wstring_view sourceId, bool sourceRunning, std::wstring_view cwd, const std::vector<std::pair<std::wstring, std::wstring>>& spawnEnv) noexcept;
}
