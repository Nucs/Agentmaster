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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
}
