// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's LINE EXTRACTOR (SESSIONS_DB.md §7.2): one transcript line in,
// everything the database derives from it out — in ONE JSON parse.
//
// What a line yields:
//   - its ENVELOPE (uuid / parentUuid / leafUuid / type / subtype / timestamp / branch, the flags the
//     live-branch walk and the summary read, the extra crossing edges of a compaction root);
//   - its SEARCH BLOCKS (one per content block: a typed prompt, a reply, thinking, a tool call, a
//     tool result, system text, a message typed mid-turn), with the rules of today's search
//     (TranscriptStore::ClassifyTranscriptLine) kept: the same fields are searchable, meta / noise
//     prompts are flagged, attachment payloads are never indexed;
//   - its SUMMARY SECTION rows, produced by the app's OWN exported rules so the database's summary
//     cannot drift from AnalyzeSessionTranscript (D14): SummaryPromptOfLine (the numbered prompts),
//     SummaryCompactionLabelOfLine, SeSummaryFileLabel, NormalizeRecapText, and the analyzer's
//     plan-content / "read the full transcript at:" tests (ported here line for line);
//   - its TOOL-TOUCHED PATHS with the operation (read / edit / write / search / shell);
//   - its USAGE (one API message's token counts; the caller dedupes by message id);
//   - the SESSION-ROW facts of §5.6 the line carries (models, effort, cost-state, worktree-state,
//     relocation, environment snapshot, titles, outcomes, turn and Stop-hook times, ...).
//
// What it does NOT decide (the caller's per-file fold does, because it needs the file's history):
// whether the line is a re-written copy of an earlier line of the same file (the analyzer skips
// those), its /compact segment number, the tool name a tool_result answers, and whether it is on the
// live branch.
//
// Plain C++ (no WinRT, no DuckDB): unit-testable on canned lines.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster::SessionDb
{
    // ===== bit sets and enums stored in the database =============================================

    // line.flags (UTINYINT).
    enum LineFlag : uint8_t
    {
        kLfSidechain = 0x01, // isSidechain: an inlined subagent chain, never the conversation's own tail
        kLfCompactBoundary = 0x02, // a system/compact_boundary line (a /compact segment ends here)
        kLfCompactRoot = 0x04, // null parentUuid + (logicalParentUuid or a boundary): the walk crosses it
        kLfUserExternal = 0x08, // type user + userType external (the analyzer's "externalUser")
        kLfDupInFile = 0x10, // its uuid appeared EARLIER in this file: a re-written copy (skipped by the summary)
        kLfForkCopy = 0x20, // stamped forkedFrom: a line a fork copied from its parent session
        kLfMeta = 0x40, // isMeta or isCompactSummary
        kLfApiError = 0x80, // isApiErrorMessage
    };

    // doc.role (UTINYINT) — the conversation roles of SESSIONS_DB.md §8.4.2.
    enum DocRole : uint8_t
    {
        kRolePrompt = 1, // typed by the user
        kRoleReply = 2, // Claude's text
        kRoleThinking = 3,
        kRoleToolCall = 4, // a tool_use: its name and input JSON
        kRoleToolResult = 5, // a tool_result (or a spilled output file)
        kRoleSystem = 6, // system lines' text, and meta / noise user lines
        kRoleQueued = 7, // a message typed while Claude worked (a human queued_command)
    };

    // doc.flags (UTINYINT).
    enum DocFlag : uint8_t
    {
        kDfMeta = 0x01, // from a meta user line (caveats, command output echoes) — not a typed prompt
        kDfNoise = 0x02, // a control marker that only looks like a prompt (IsNoiseUserPrompt)
        kDfSidechain = 0x04, // from an isSidechain line
        kDfSubagent = 0x08, // from a subagent's own transcript (<sid>/subagents/agent-*.jsonl)
        kDfError = 0x10, // an API error line, or a tool_result marked is_error
        kDfCompactSummary = 0x20, // the /compact continuation summary
        kDfSpill = 0x40, // a spilled tool output file (<sid>/tool-results/*)
        kDfForkCopy = 0x80, // from a line a fork copied from its parent
    };

    // path_access.op (UTINYINT).
    enum PathOp : uint8_t
    {
        kOpRead = 1,
        kOpEdit = 2,
        kOpWrite = 3,
        kOpSearch = 4, // Grep / Glob / LS and similar: a folder or pattern looked in
        kOpShell = 5, // an absolute path named in a shell command (mined, ExtractPathsFromText)
        kOpOther = 6, // any other tool's file_path / notebook_path / path
    };

    // The names the tools print for those values.
    const char* RoleName(uint8_t role) noexcept;
    const char* PathOpName(uint8_t op) noexcept;

    // ===== what one line yields ==================================================================

    // A tool_use block (assistant lines), with the summary's derived facts.
    struct ToolUseRec
    {
        std::wstring id; // the tool_use id (a later tool_result names it)
        std::wstring name; // the tool's name
        std::wstring path; // input.file_path ("" when absent) — what the summary's file lists read
        std::wstring label; // SeSummaryFileLabel(path) ("" when no path)
        bool plans{ false }; // the path lies under a /plans/ folder (the analyzer's SeIsPlansPath)
        std::wstring skill; // input.skill (a Skill call)
        int16_t todosDone{ -1 }; // a TodoWrite whose input.todos is an ARRAY: its completed count; -1 otherwise
        int16_t todosOpen{ -1 }; // ... and its pending + in_progress count; -1 otherwise
    };

    // A tool_result block (user lines).
    struct ToolResultRec
    {
        std::wstring id; // tool_use_id
        bool created{ false }; // its text holds "File created successfully at:" (a Write that created a file)
        bool error{ false }; // is_error
    };

    // One searchable block.
    struct BlockRec
    {
        uint16_t block{ 0 }; // the block's index inside its line (0-based, in content order)
        uint8_t role{ 0 }; // DocRole
        uint8_t flags{ 0 }; // DocFlag
        std::wstring tool; // tool calls: the tool's name; tool results: filled by the caller's fold
        std::wstring toolUseId; // pairs a call with its result
        std::wstring text; // the block's text (full length)
    };

    // One tool-touched path.
    struct PathRec
    {
        std::wstring path;
        uint8_t op{ kOpOther };
    };

    // One API message's usage (assistant lines carry it, repeated on every line of the message).
    struct UsageRec
    {
        std::wstring messageId;
        std::wstring model;
        int64_t inputTokens{ 0 };
        int64_t outputTokens{ 0 };
        int64_t cacheReadTokens{ 0 };
        int64_t cacheWriteTokens{ 0 };
        int32_t webSearches{ 0 };
        int32_t webFetches{ 0 };
    };

    // Everything ExtractLine reads from one line. Every field defaults to "absent".
    struct LineFacts
    {
        bool json{ false }; // the line parsed as a JSON object (false: stored, never classified)

        // --- envelope ---
        std::wstring type; // "user", "assistant", "system", "attachment", "custom-title", ...
        std::wstring subtype; // system lines: "compact_boundary", "away_summary", "turn_duration", ...
        std::wstring uuid;
        std::wstring parentUuid;
        std::wstring leafUuid; // a leaf marker (last-prompt lines)
        std::wstring userType;
        std::wstring cwd;
        std::wstring branch; // gitBranch
        std::wstring version; // Claude Code's version
        std::wstring entrypoint; // cli / sdk-cli / sdk-ts
        std::wstring forkedFrom; // forkedFrom.sessionId (a copied line of a fork)
        int64_t tsMs{ 0 }; // the line's timestamp (0 when absent / unparseable)
        uint8_t flags{ 0 }; // LineFlag bits (the caller adds kLfDupInFile)
        std::vector<std::wstring> extraEdges; // a compaction root's crossing edges (logical parent, preserved messages)

        // --- summary sections (D14; the app's exported rules) ---
        std::wstring summaryPrompt; // SummaryPromptOfLine: the prompt as the summary lists it, "" if none
        uint8_t summaryPromptKind{ 0 }; // 1 typed (a user line), 2 queued (an attachment)
        std::wstring compactLabel; // SummaryCompactionLabelOfLine ("" unless a compact boundary)
        std::vector<ToolUseRec> toolUses;
        std::vector<ToolResultRec> toolResults;
        std::wstring recap; // NormalizeRecapText(away_summary content) ("" otherwise)
        bool planContent{ false }; // a non-empty planContent (external user lines)
        std::wstring parentLink; // the "read the full transcript at: <parent>.jsonl" stem (external user lines)

        // --- search ---
        std::vector<BlockRec> blocks;

        // --- paths ---
        std::vector<PathRec> paths;

        // --- usage and the session-row facts (§5.6) ---
        std::optional<UsageRec> usage;
        std::wstring model; // assistant: message.model (not "<synthetic>")
        std::wstring effort; // assistant: envelope "effort"
        std::wstring stopReason; // assistant: message.stop_reason
        std::wstring apiErrorStatus; // assistant: apiErrorStatus (when isApiErrorMessage)
        std::wstring attributionSkill;
        std::wstring attributionMcpServer;
        bool abortedMidStream{ false };
        bool interruptMarker{ false }; // a "[Request interrupted …" user line
        int64_t contextTokens{ -1 }; // the context this line reports (assistant usage sum, or a compaction's postTokens); -1 none
        int64_t inputContextTokens{ -1 }; // input + cache read + cache write (context_max's measure); -1 none
        std::wstring permissionMode; // permission-mode lines
        std::wstring mode; // mode lines ("normal" / "plan")
        std::wstring customTitle, aiTitle, legacySummary, agentName; // their kinds' payloads
        std::wstring relocatedCwd; // relocated lines
        int64_t turnDurationMs{ -1 }; // system/turn_duration; -1 none
        int64_t stopHookMsTotal{ -1 }; // system/stop_hook_summary: the hooks' total; -1 none
        int64_t stopHookMsMax{ -1 };
        std::wstring compactTrigger; // a boundary's compactMetadata.trigger
        int64_t compactPreTokens{ 0 };
        // cost-state (Claude Code's own running totals; the LAST line wins)
        bool costState{ false };
        double costUsd{ 0 };
        int64_t apiMs{ 0 }, apiMsNoRetries{ 0 }, toolMs{ 0 }, wallMs{ 0 }, linesAdded{ 0 }, linesRemoved{ 0 };
        bool costUnknownModel{ false };
        // worktree-state (the LAST line wins)
        bool worktree{ false };
        std::wstring worktreePath, worktreeName, worktreeBranch, worktreeOriginalCwd;
        // the environment attachment (the FIRST one wins)
        bool environment{ false };
        bool envIsGitRepo{ false }, envIsWorktree{ false };
        std::wstring envShell;
        int32_t envExtraDirs{ 0 };
        // frame-link (artifacts published)
        bool frameLink{ false };
        std::wstring frameTitle, frameUrl;
        // prompts and work, the TranscriptStats notions
        bool typedPrompt{ false }; // a REAL human prompt (not meta / noise / sidechain, non-empty)
        std::wstring promptText; // that prompt's text
        bool queuedPrompt{ false }; // a human queued_command (summaryPromptKind == 2)
        int32_t imagesPasted{ 0 };
        std::vector<std::wstring> toolNames; // every tool_use's name (tool_calls_by_tool)
        int32_t subagentsSpawned{ 0 }; // Task / Agent tool calls
        std::wstring replyText; // the line's assistant text blocks joined (last_reply)
    };

    // Extract everything from ONE line.
    //   rawUtf8  — the line as written, WITHOUT its trailing '\n' (a trailing '\r' is tolerated).
    //   subagent — the line comes from a subagent's transcript: its blocks get kDfSubagent, and no
    //              summary or session facts are taken from it (the summary reads the top-level file).
    // Returns the facts; `json` is false for a line that is not a JSON object (stored, not classified).
    // Never throws beyond std::bad_alloc (a malformed line is "not JSON").
    LineFacts ExtractLine(std::string_view rawUtf8, bool subagent);

    // The searchable text of a spilled tool output (<sid>/tool-results/<id>.txt) as ONE tool-result block,
    // or nothing when the bytes are not text (not valid UTF-8, or NUL bytes in the first 4 KB).
    //   bytes   — the file's content.
    //   stem    — the file name without ".txt" (a tool_use id "toolu_…" or a background task id).
    // Returns the block (role tool_result, flag kDfSpill, toolUseId = stem when it is a tool_use id).
    std::optional<BlockRec> ExtractSpillBlock(std::string_view bytes, std::wstring_view stem);
}
