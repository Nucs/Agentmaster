// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionSearchLog: the AI search's CONVERSATION LOG (SESSIONS.md §9.17), debug-only.
//
// WHAT: every request of ONE AI search conversation written down whole, for a review afterwards — the request and the window
// it came from, the model's plan, every tool call with its arguments and its WHOLE result (the text the model read), the
// sessions the model showed with their whys, the reply, how long each step took, and the request's tokens and cost. The
// user's question of 2026-11-05 ("do we have logs of the sessions of AI so we can review and analyze? say only enabled in
// debug mode"): the conversation's own record (HarnessRecord, <profile>\harness\<name>.json) keeps 400 items with a
// 2,000-character preview of each tool result and is deleted by a reset, and hooks.log has one line per request — neither
// tells WHY a search missed a session or took 50 s. The eval kit (tools/ai-search-eval) records whole calls, but only for its
// own queries in a scratch run; this is the same evidence for the requests the user really makes.
//
// WHERE: <dir>\<conversation>-<yyyy-MM-dd>.jsonl — one file per conversation per LOCAL day, one JSON object per line (JSON
// Lines: append-only, each line written whole under a lock, so a crash costs at most the line being written, and a request
// that never ended still shows every step it took). The app's dir is <profile>\harness\logs (SessionSearchLogDirIfDebug).
// A conversation's files older than Options::keepDays are deleted at its first write of a process; nothing else is deleted.
//
// WHEN: only in DEBUG MODE. The app creates a log for a conversation only when Profiles::IsDevOrDebugPackage() — the dev
// package always, a release with Settings ▸ About ▸ "Enable Debug Mode", `--debug` or AGENTMASTER_DEBUG (the one gate of
// developer-only features) — because the lines hold the user's own words and whole transcript excerpts, and a busy day of
// requests writes megabytes.
//
// THE LINES — every one has "v" (kSessionSearchLogFormat), "kind", "at" (the local time, "2026-11-05 09:31:12.345"), "t"
// (unix ms), "conv" (the conversation), "epoch" (the conversation's reset count) and "turn" (the request; 0 = none):
//   start     — this log's first line in this process: "build" (the running build, "0.0.1.42 dev"), "pid", "keep_days"
//   ask       — a request: "text" (whole), "origin" (the asking window's id; "" = none, e.g. Telegram), "thinking", "model"
//   narration — "text": what the model wrote BEFORE a tool call (its "I'll 1. ... 2. ..." plan, a later "now I'll ...")
//   call      — a tool call starts: "seq" (1, 2, ... within the request), "tool", "arguments" (the JSON text the model sent),
//               "at_ms" (since the ask)
//   result    — that call's answer: "seq" (its call's), "tool", "at_ms", "ms" (the call's own duration), "error", "chars",
//               "result" (WHOLE: the text the model read)
//   show      — the model showed sessions: "ids" (whole ids, its order), "whys" (index-aligned), "note", "at_ms"
//   end       — the request ended: "status" (answered | failed | interrupted | reset), "answer" (whole), "elapsed_ms" (since
//               the ask), "turn_ms" (the harness's own measure), "thinking_blocks", "thinking_tokens", "usage" — "input",
//               "output", "cache_read", "cache_write", "cost_usd": this request's share (the conversation's totals at the
//               end less those at the ask; approximate when two requests overlap); "context", "window": the
//               conversation's size after it
//   process   — a claude.exe came up: "model", "cli" (Claude Code's version), "permission" (the permission mode in effect)
//   compacted — Claude Code compacted the conversation: "pre", "post" (tokens), "ms"
//   failure   — "text": a start, protocol or turn failure the harness reported
//   reset     — the conversation was thrown away ("epoch" = the NEW epoch); every request still open ended first ("reset")
//   thinking  — the thinking level for the next request changed: "level"
// A call's "seq" pairs it with its result: calls of one tool are answered in the order they were made (the SDK's reader
// thread runs the handlers), so the oldest open call of that tool gets the result. tools/ai-search-eval/review_log.py reads the
// files back: a review per request, a summary, and the eval's results format.
//
// THREADS: every method may be called from any thread (the agent calls it from the UI thread at Ask, from the conversation's
// worker and from the SDK's reader thread) and never throws; one mutex keeps every line whole and the lines in order. The
// file IO runs under that mutex only — never under the agent's own lock, so a big tool result never delays a page's
// Snapshot().
//
// Plain C++, no WinRT. Compiled into the app (TerminalAppLib) and the m5 harness (TestSessionSearchLog, tests_harness.cpp).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "AgentHarnessCore.h" // HarnessUsage — a request's tokens and cost

namespace Agentmaster
{
    struct HarnessEvent; // AgentHarness.h: the conversation's events (only a const reference crosses this header)

    /// <summary>The format of the lines ("v" on every line). Raised when a field changes its meaning; a new field does not.</summary>
    inline constexpr int kSessionSearchLogFormat = 1;

    /// <summary>
    /// How many days of a conversation's log files are kept: at the first write of a process, its files dated before today
    /// less this many days are deleted. 30 days of a busy debug profile is a few hundred megabytes at most.
    /// </summary>
    inline constexpr int kSessionSearchLogKeepDays = 30;

    /// <summary>The file name a line written at `unixMs` goes to: "&lt;conversation&gt;-&lt;yyyy-MM-dd&gt;.jsonl", by the LOCAL date.</summary>
    /// <param name="conversation">The conversation's name ("sessions-search").</param>
    /// <param name="unixMs">The line's time.</param>
    /// <returns>The file name, without a folder.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    std::wstring SessionSearchLogFileName(std::wstring_view conversation, int64_t unixMs);

    /// <summary>
    /// The conversation log of ONE AI search conversation (see the file header). Create it only when the log is wanted (the
    /// app: debug mode); every method is best-effort and noexcept — a log that cannot be written says so once in hooks.log and
    /// never disturbs the search.
    /// </summary>
    class SessionSearchLog
    {
    public:
        /// <summary>What a log writes, where, and by which clock.</summary>
        struct Options
        {
            /// <summary>The folder of the files (the app: &lt;profile&gt;\harness\logs). Created at the first write.</summary>
            std::wstring dir;
            /// <summary>The conversation's name: the files' prefix and every line's "conv".</summary>
            std::wstring conversation;
            /// <summary>Days of files kept (kSessionSearchLogKeepDays); 0 or less = keep every file.</summary>
            int keepDays{ kSessionSearchLogKeepDays };
            /// <summary>The clock (unix ms); empty = the system clock. A test passes its own, to control durations and dates.</summary>
            std::function<int64_t()> nowUnixMs;
        };

        /// <summary>Make a log. Writes nothing and touches no file until the first line.</summary>
        /// <param name="options">See Options.</param>
        /// <exception cref="std::bad_alloc">If the log cannot be allocated.</exception>
        explicit SessionSearchLog(Options options);

        /// <summary>Ends the log. Requests still open get no "end" line: the file then shows where each one stopped.</summary>
        ~SessionSearchLog();

        SessionSearchLog(const SessionSearchLog&) = delete;
        SessionSearchLog& operator=(const SessionSearchLog&) = delete;

        /// <summary>A request was sent to the conversation: writes the "ask" line and opens the request (its start time and usage).</summary>
        /// <param name="epoch">The conversation's epoch.</param>
        /// <param name="turn">The request's turn (Send's id); 0 is not logged.</param>
        /// <param name="text">The request, whole.</param>
        /// <param name="origin">The asking window's id ("" = none).</param>
        /// <param name="thinking">The thinking level it runs with ("low" ... "max").</param>
        /// <param name="model">The model, when known ("" before the first process answered).</param>
        /// <param name="usageNow">The conversation's usage at the ask: the base of the request's share in its "end" line.</param>
        void Asked(uint64_t epoch, uint64_t turn, std::wstring_view text, std::wstring_view origin, std::wstring_view thinking, std::wstring_view model, const HarnessUsage& usageNow) noexcept;

        /// <summary>The model wrote text before a tool call: the "narration" line.</summary>
        /// <param name="epoch">The conversation's epoch.</param>
        /// <param name="turn">The request it belongs to.</param>
        /// <param name="text">The text, whole.</param>
        void Narration(uint64_t epoch, uint64_t turn, std::wstring_view text) noexcept;

        /// <summary>
        /// One of the conversation's own events: ToolCall ("call"), ToolResult ("result"), TurnCompleted ("end"), Ready
        /// ("process"), Compacted ("compacted"), Error and an error Notice ("failure"). Every other kind is ignored (the
        /// agent reports the request, the narration and the show through the other methods).
        /// </summary>
        /// <param name="event">The event as the harness emitted it (its epoch, turn and fields).</param>
        void Harness(const HarnessEvent& event) noexcept;

        /// <summary>The model showed sessions: the "show" line.</summary>
        /// <param name="epoch">The conversation's epoch.</param>
        /// <param name="turn">The request it belongs to.</param>
        /// <param name="ids">The shown sessions' whole ids, in the model's order.</param>
        /// <param name="whys">The model's why per session, index-aligned with ids ("" = none given; a shorter list is fine).</param>
        /// <param name="note">The model's caption for the list.</param>
        void Shown(uint64_t epoch, uint64_t turn, const std::vector<std::wstring>& ids, const std::vector<std::wstring>& whys, std::wstring_view note) noexcept;

        /// <summary>
        /// The conversation was thrown away: every request still open ends ("end", status "reset"), then the "reset" line. Events
        /// of an older epoch that arrive later belong to a conversation that is gone (the agent drops them before they reach
        /// the log).
        /// </summary>
        /// <param name="newEpoch">The conversation's epoch after the reset.</param>
        void Reset(uint64_t newEpoch) noexcept;

        /// <summary>The thinking level of the next request changed: the "thinking" line.</summary>
        /// <param name="level">The new level's word ("low" ... "max").</param>
        void ThinkingChanged(std::wstring_view level) noexcept;

        /// <summary>The folder the log writes into (Options::dir), for the agent's one hooks.log line about it.</summary>
        /// <returns>The folder.</returns>
        const std::wstring& Directory() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
