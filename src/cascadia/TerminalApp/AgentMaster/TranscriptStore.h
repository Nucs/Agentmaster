// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — TranscriptStore: the on-disk Claude-session store API (SESSIONS.md §6).
//
// Everything the Sessions browser (and any other consumer) needs to ENUMERATE, TIME, TITLE,
// COUNT, and SEARCH-INDEX the Claude Code sessions under the global `~/.claude/projects/`,
// grounded in the corpus survey (SESSIONS.md §2-§5; 1,817 uuid transcripts, versions 2.0.50 →
// 2.1.170 — the API tolerates every stratum but is tuned for the recent 1-3 months):
//
//   - A session == a TOP-LEVEL `<uuid>.jsonl` under `projects/<enc-cwd>/` (filename stem ==
//     every line's sessionId). Legacy `agent-*.jsonl`, `memory/`, and `<sid>/` subdirs are not
//     sessions and are excluded by the uuid-stem filter.
//   - File mtime is a SUPERSET signal only (mtime >= last-activity always holds, but the gap's
//     median is ~1 h and the max ~43 days — background `away_summary` forks, exit-time flushes
//     of untimestamped state lines, and bulk metadata passes all touch the file without a
//     turn). So: `(size, mtime)` is the cache-invalidation key; the DISPLAYED created /
//     last-activity come from the LINES' own `timestamp` fields (ReadTranscriptQuickFacts).
//   - The literal last line is almost never a timestamped message (system/permission-mode/
//     last-prompt tails dominate), and single lines reach >1 MiB — the tail scan reads a
//     growing window and walks backwards.
//   - A FORK (`/branch`, `/rewind` restore, `--fork-session`, in-session /resume-to-a-message)
//     duplicates the parent's subtree VERBATIM (same uuids + timestamps) with
//     `forkedFrom:{sessionId, messageUuid}` stamped on every copied line — so a fork's
//     line-timestamps lie about its creation (use file birth) and search hits dedupe by uuid.
//   - Title precedence: customTitle (last `custom-title` line) > aiTitle (last `ai-title`) >
//     legacy `summary` (v2.0.75-2.1.25 stratum only) > the first REAL human prompt.
//   - "REAL human prompt" excludes the control/noise markers that masquerade as user lines:
//     `<command-name>`/`<command-message>`/`<local-command`/`<bash-*>`/`<task-notification>`/
//     `<system-reminder`/`<teammate-message`/`Caveat:`/`[Request interrupted` (IsNoiseUserPrompt)
//     plus isMeta / isCompactSummary / isSidechain lines.
//
// Plain C++ + Win32, no WinRT (the .cpp is <PrecompiledHeader>NotUsing and links into the
// standalone test harness). The line-level primitives are PURE (string -> values) so the
// harness exercises them with canned lines; only Enumerate*/Scan*/Read* touch the filesystem.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // ===== enumeration =======================================================================

    // One enumerated on-disk Claude session (a top-level `<uuid>.jsonl`).
    struct TranscriptRef
    {
        std::wstring sessionId; // the filename stem (== every line's sessionId)
        std::wstring path; // full path to the .jsonl
        std::wstring projectDirLeaf; // the encoded project-dir leaf (e.g. "K--source-Agentmaster") — LOSSY; the real cwd comes from the lines
        int64_t sizeBytes{};
        int64_t mtimeMs{}; // last write — the SUPERSET window filter + half the (size,mtime) invalidation key
        int64_t birthMs{}; // file creation time (a FORK's true creation; line timestamps lie for forks)
    };

    // True iff `stem` is a 36-char hyphenated UUID — the session-transcript filename shape.
    // (Excludes legacy `agent-<7hex>` files and anything else.) Pure.
    bool IsSessionIdStem(std::wstring_view stem);

    // Enumerate every session transcript across ALL project dirs under `projectsDir` whose
    // mtime >= sinceUnixMs (0 == everything). mtime >= last-activity always holds, so this never
    // misses a session active inside the window (it can only over-include; refine with
    // ReadTranscriptQuickFacts). Newest-mtime first. Tolerates the sweep deleting files mid-walk
    // (a vanished file is simply skipped). ~0.15 s for ~2,600 files across ~170 dirs.
    std::vector<TranscriptRef> EnumerateTranscriptsIn(std::wstring_view projectsDir, int64_t sinceUnixMs);
    // Against the live Claude projects root (ClaudeProjectsDir()).
    std::vector<TranscriptRef> EnumerateTranscripts(int64_t sinceUnixMs);

    // ===== pure line-level primitives ========================================================

    // A transcript line's `timestamp` ("2026-06-10T12:34:56.789Z" — every recent line is ISO-Z;
    // the oldest strata carried epoch numbers) -> Unix ms. Accepts ISO-8601 with optional
    // fractional seconds + optional trailing 'Z', or a pure-digit epoch (>= 12 digits == ms,
    // 9-11 digits == seconds). 0 on anything else. Pure.
    int64_t ParseTranscriptTimestamp(std::wstring_view ts);

    // True iff a user-line prompt body is a control/noise marker, not a human message: command
    // echoes (`<command-name>` / `<command-message>` / `<local-command`), bash-tool echoes
    // (`<bash-input>` / `<bash-stdout>` / `<bash-stderr>`), background-task notifications
    // (`<task-notification>`), injected reminders (`<system-reminder`, `Caveat:`), teammate
    // traffic (`<teammate-message`), and interrupt markers (`[Request interrupted`). The
    // noise-suppression rule set proven by the user's own exit hook + corpus survey
    // (SESSIONS.md §6a). Leading whitespace is ignored. Pure.
    bool IsNoiseUserPrompt(std::wstring_view prompt);

    // The display-title precedence in ONE place (SESSIONS.md §6.3): customTitle > aiTitle >
    // legacy summary > the first REAL prompt (already collapsed to a display line by the
    // extractors). Returns the first non-empty, else empty. Pure.
    std::wstring PickDisplayTitle(std::wstring_view customTitle, std::wstring_view aiTitle, std::wstring_view summary, std::wstring_view firstPrompt);

    enum class TranscriptLineKind
    {
        Other, // unrecognized / state lines (mode, permission-mode, last-prompt, progress, pr-link, ...)
        UserPrompt, // a human-prompt-SHAPED user line (string / pure-text content); REAL iff !meta && !sidechain
        UserToolResult, // a user line carrying tool_result blocks (a tool turn, not a human message)
        Assistant, // an assistant line (text / thinking / tool_use blocks)
        System, // system lines (away_summary, api_error, compact_boundary, turn_duration, ...)
        Attachment, // injected context (task_reminder, file snippets, ...) — never indexed as conversation text
        CustomTitle, // {"type":"custom-title","customTitle":...} — the user-set conversation name
        AiTitle, // {"type":"ai-title","aiTitle":...} — the async picker title
        Summary, // {"type":"summary","summary":...} — LEGACY (v2.0.75-2.1.25); kept for old strata
        QueueOperation, // prompt-queue ops (a queue-op FIRST line marks a task file, §6a noise rule)
        FileHistorySnapshot, // checkpoint index lines
    };

    // Everything one classified transcript line contributes to browse/search/stats. The text
    // fields are CAPPED by the caller-supplied budgets (a single line can exceed 1 MiB):
    //   maxUserTextChars — cap for `userText` (the human prompt body; 0 = don't extract)
    //   maxAgentTextChars — cap for `agentText` (assistant text + thinking + tool_use name/input,
    //                       tool_result bodies, toolUseResult stdout/stderr/content, system
    //                       content; 0 = don't extract). Attachment payloads are deliberately
    //                       NOT extracted (injected context, not conversation — §6.4).
    struct TranscriptLineFacts
    {
        TranscriptLineKind kind{ TranscriptLineKind::Other };
        int64_t timestampMs{}; // 0 when the line carries no timestamp (state/tail lines)
        bool sidechain{}; // isSidechain:true (inline subagent lines, old strata) — never main-chain state/stats
        bool meta{}; // isMeta / isCompactSummary / a noise prompt — not a human message
        int toolUses{}; // # of assistant tool_use blocks on this line
        std::wstring userText; // UserPrompt only (REAL prompts only), capped
        std::wstring agentText; // Assistant / UserToolResult / System searchable text, capped
        std::wstring title; // CustomTitle / AiTitle / Summary payload (collapsed to one line)
        std::wstring forkedFromId; // forkedFrom.sessionId when stamped (=> the file is a fork copy)
        std::wstring cwd; // the line's cwd field, when present
        std::wstring gitBranch; // the line's gitBranch field, when present
    };

    // Classify + extract ONE transcript line (any stratum; tolerant of unknown types). Pure.
    TranscriptLineFacts ClassifyTranscriptLine(std::wstring_view line, size_t maxUserTextChars = 0, size_t maxAgentTextChars = 0);

    // ===== streaming scan + stats (the incremental-index primitives) =========================

    // Stream the file's COMPLETE lines from byte offset `fromOffset`, invoking `sink` per line's
    // facts, bounded memory (chunked reads; a partial trailing line is left for the next call —
    // the SessionScanner tail-cursor pattern, which is exactly what makes the page's per-session
    // index INCREMENTAL: re-call from the cached offset after an append). Returns the new offset
    // (>= fromOffset; == file size when fully consumed), or -1 when the file can't be opened.
    // Shares read/write/delete so a live claude is never blocked.
    using TranscriptLineSink = std::function<void(const TranscriptLineFacts&)>;
    int64_t ScanTranscript(const std::wstring& path, int64_t fromOffset, size_t maxUserTextChars, size_t maxAgentTextChars, const TranscriptLineSink& sink);

    // Accumulated per-session stats — ONE struct that doubles as the resume cursor: feed the
    // same instance back after an append and only the suffix past `parsedBytes` is read. A size
    // SHRINK means the file was replaced — the accumulate resets the struct and rebuilds from 0
    // automatically. All time/activity fields derive from the LINES, never file mtime.
    struct TranscriptStats
    {
        bool found{}; // the file existed and was readable on the last accumulate
        int64_t parsedBytes{}; // resume cursor: bytes of COMPLETE lines consumed so far
        int64_t firstTimestampMs{}; // first timestamped line (created — but a FORK's is copied; prefer birth, see quick facts)
        int64_t lastTimestampMs{}; // last non-sidechain user/assistant timestamp == the REAL last activity
        int userPrompts{}; // REAL human prompts (noise/meta/sidechain filtered)
        int assistantLines{}; // assistant lines (NOT turns — one turn can span several lines)
        int toolUses{}; // total assistant tool_use blocks
        std::wstring customTitle; // last custom-title (newer retitles win)
        std::wstring aiTitle; // last ai-title
        std::wstring summary; // last legacy summary
        std::wstring firstUserPrompt; // the title fallback (first REAL prompt, display-collapsed)
        std::wstring forkedFromId; // non-empty => this file is a fork copy of that session
        std::wstring cwd; // first cwd seen on a line (the REAL dir; the folder name is lossy)
        std::wstring gitBranch; // first gitBranch seen
    };

    // One incremental accumulate: scan [stats.parsedBytes, EOF) and fold into `stats`. Returns
    // false (stats.found=false) when the file is gone — the sweep deletes mid-listing; callers
    // drop the row. The display title is PickDisplayTitle(customTitle, aiTitle, summary,
    // firstUserPrompt).
    bool AccumulateTranscriptStats(const std::wstring& path, TranscriptStats& stats);

    // ===== cheap row facts: head + growing-tail windows, never a full read ===================

    // The browse-row essentials, extracted without reading the body: `created` from the FIRST
    // timestamped line (head window, grown when a giant first line — e.g. a file-history-snapshot
    // — overflows it), EXCEPT a fork (forkedFrom in the head), whose copied timestamps lie —
    // there `fileBirthMs` (the enumerated TranscriptRef::birthMs) is the truth; `lastActivity`
    // from the LAST non-sidechain user/assistant timestamped line (tail window, grown ×4 past
    // >1 MiB single lines, capped — 0 when the transcript has no timestamped message at all,
    // e.g. a never-prompted prefix-only file; callers fall back to birth/mtime).
    struct TranscriptQuickFacts
    {
        bool found{};
        int64_t createdMs{}; // first line timestamp; forks: fileBirthMs; 0 when nothing timestamped
        int64_t lastActivityMs{}; // last user/assistant timestamp; 0 when none
        bool fork{}; // forkedFrom seen in the head
        std::wstring forkedFromId; // the fork's parent session id (group rows under the root)
        std::wstring cwd; // from the head window (the REAL dir for grouping; PathEq it)
    };
    TranscriptQuickFacts ReadTranscriptQuickFacts(const std::wstring& path, int64_t fileBirthMs);
}
