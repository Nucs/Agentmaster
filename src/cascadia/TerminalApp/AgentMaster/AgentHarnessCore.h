// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — AgentHarnessCore: the SDK-free half of the agent harness (SDK_HARNESS.md).
//
// The agent harness lets Agentmaster HOST a Claude conversation itself: a windowless Claude Code
// process in stream-json mode, driven through the Claude Agent SDK (refs/claude-agent-sdk-cpp),
// with tools that run inside Agentmaster. That is a different thing from a MANAGED session (a
// claude.exe in a terminal tab that the user talks to), and from a PRESET (HARNESSES.md: a tab
// session on another model provider).
//
// This file holds everything of the harness that needs neither the SDK nor nlohmann/json, so it
// compiles into the standalone test harness and can be unit-tested without a model call:
//
//   - the PERSISTENCE layer: a conversation's record (what the user asked, what the model said,
//     which tools it called), its JSON form, the file in the profile, and the DIGEST that carries
//     the conversation into a new process (a harness process keeps its context in memory only,
//     because it runs with --no-session-persistence);
//   - the launch POLICY: the --settings JSON, the environment a harness child gets, the path
//     encoding the SDK needs;
//   - the READERS of what the Claude Code process reports: system lines (start, compaction),
//     the result line of a turn, the usage block of a request.
//
// AgentHarness.h/.cpp is the other half: the conversation object that owns the SDK client.
//
// Plain C++ + Win32, no WinRT (the .cpp is <PrecompiledHeader>NotUsing</PrecompiledHeader>).
// Every function is thread-safe unless it says otherwise: none keeps state.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Activity.h" // kHarnessEnvMarker — the mark of a harness child, shared with the Fleet Observer

namespace Agentmaster
{
    // ===== usage ============================================================================

    // Token and cost figures of one harness conversation.
    //   contextTokens / contextWindow describe NOW (how full the model's context is);
    //   the four token totals and costUsd are sums since the record was created.
    struct HarnessUsage
    {
        int64_t contextTokens{}; // the newest request's size: input + cache read + cache creation + output; after a compaction, what the compaction left
        int64_t contextWindow{}; // the window the conversation may fill before it is compacted (0 = not reported yet)
        int64_t inputTokens{}; // uncached input tokens, summed over every request
        int64_t outputTokens{};
        int64_t cacheReadTokens{};
        int64_t cacheCreationTokens{};
        double costUsd{}; // the list-price cost Claude Code reports; a subscription is not billed this amount
    };

    // ===== the record (persistence) =========================================================

    // What one record item is.
    enum class HarnessItemKind
    {
        User, // a message the user sent
        Assistant, // text the model wrote
        ToolCall, // the model called a tool: toolName + argumentsJson
        ToolResult, // what that tool answered: text (cut to kHarnessMaxToolResultChars) + isError
        Notice, // something the harness itself reports (a restart, a compaction, an error)
    };

    // One entry of a conversation record, in the order things happened.
    struct HarnessItem
    {
        HarnessItemKind kind{ HarnessItemKind::Notice };
        uint64_t turn{}; // which user message it belongs to (1, 2, ...); 0 = outside any turn
        int64_t atUnixMs{}; // when the harness saw it
        std::wstring text; // User / Assistant / Notice text, or a ToolResult's text
        std::wstring toolName; // ToolCall / ToolResult: the full name, "mcp__<server>__<tool>"
        std::wstring argumentsJson; // ToolCall: the arguments exactly as the model sent them
        bool isError{}; // ToolResult: the tool reported a failure; Notice: it reports an error
    };

    // Caps of a record. A record is a UI history and a digest source, not an archive: it must stay
    // small enough to rewrite after every turn.
    inline constexpr size_t kHarnessMaxItems = 400; // older items are dropped first
    inline constexpr size_t kHarnessMaxItemTextChars = 8000; // User / Assistant / Notice text, and ToolCall arguments
    inline constexpr size_t kHarnessMaxToolResultChars = 2000; // a tool result is kept only as a preview

    // The durable record of ONE harness conversation: what a later run needs to show the
    // conversation again and to give a new process its context back.
    struct HarnessRecord
    {
        std::wstring name; // the conversation's key (HarnessOptions::name)
        std::wstring model; // the model that answered last
        int64_t createdUnixMs{}; // 0 = an empty record (nothing was ever asked)
        int64_t updatedUnixMs{};
        uint64_t turns{}; // user messages sent so far; the next turn is turns + 1
        int compactions{}; // how many times Claude Code compacted the context
        HarnessUsage usage;
        std::vector<HarnessItem> items;

        // True when nothing was ever asked: such a record is not written to disk.
        bool Empty() const noexcept { return turns == 0 && items.empty(); }
    };

    // Append `item` to `record`, applying the caps: the text and the arguments are cut to
    // kHarnessMaxItemTextChars (a ToolResult's text to kHarnessMaxToolResultChars), and the oldest
    // items are dropped while the record holds more than kHarnessMaxItems.
    //   record — the record to grow; its updatedUnixMs becomes item.atUnixMs when that is newer.
    //   item   — the entry; taken by value so a caller can move it in.
    //   throws — std::bad_alloc.
    // A cut text ends in "..." so a reader can tell that it was cut.
    void AppendHarnessItem(HarnessRecord& record, HarnessItem item);

    // The record as JSON text (the file format, version 1). Pure.
    //   returns — one JSON object; never empty.
    //   throws  — std::bad_alloc.
    std::wstring SerializeHarnessRecord(const HarnessRecord& record);

    // Read a record back. Pure and total: any text that is not a version-1 record object gives an
    // EMPTY record (Empty() == true), never an exception and never a half-read record — a corrupt
    // file must read as "no conversation", not as a wrong one.
    //   text    — a file's content.
    //   returns — the record, or an empty one.
    //   throws  — std::bad_alloc.
    HarnessRecord DeserializeHarnessRecord(std::wstring_view text);

    // The cold-start DIGEST: a short plain-text account of the last turns, for a NEW Claude Code
    // process that has none of the conversation in its context (after an app restart, or after
    // the process ended). It lists, oldest first, each kept turn's user message, the tool calls
    // (name + arguments) and the assistant's text; tool RESULTS are left out (they are large and
    // the model can call the tool again).
    //   record   — the conversation so far.
    //   maxTurns — keep at most this many of the newest turns.
    //   maxChars — an upper bound for the whole digest; older turns are dropped to fit, and a
    //              single turn that is still too long is cut.
    //   returns  — the digest, or empty when the record has no user message.
    //   throws   — std::bad_alloc.
    // ⚠ It is a summary, not the context: the model does not get the tool results or its own
    //   reasoning back. Use it so that a follow-up ("and the other one") still makes sense.
    std::wstring BuildHarnessDigest(const HarnessRecord& record, size_t maxTurns, size_t maxChars);

    // The first user message of a NEW process when earlier turns exist: `digest` in a marked
    // block, then the user's own text. With an empty digest the text is returned unchanged.
    //   throws — std::bad_alloc.
    std::wstring ComposeColdStartMessage(std::wstring_view digest, std::wstring_view userText);

    // True when `name` can be a conversation key: 1 to 48 characters of a-z, 0-9 and '-'. The
    // key becomes a file name, a folder name, an environment value and a log tag, so it is ASCII.
    bool IsValidHarnessName(std::wstring_view name) noexcept;

    // <profile>\harness — where records and per-conversation working folders live. Created on
    // demand by the callers that write. Resolves the ACTIVE profile (AgentmasterStateDir()).
    //   throws — std::bad_alloc.
    std::wstring HarnessStateDir();

    // The record file of the conversation `name` under `dir`: <dir>\<name>.json.
    //   throws — std::bad_alloc.
    std::wstring HarnessRecordPathIn(const std::wstring& dir, std::wstring_view name);

    // Write `record` to `path`, atomically (temp file + flush + rename: a crash leaves the old
    // file or the new one, never a torn one). The folder is created when it is missing. An EMPTY
    // record deletes the file instead, so "no conversation" has one representation on disk.
    //   returns — true when the file now matches the record.
    // Never throws: a failure is logged ([harness] / [persist-fail] in hooks.log) and reported.
    bool SaveHarnessRecordTo(const std::wstring& path, const HarnessRecord& record) noexcept;

    // Read the record at `path`.
    //   returns — the record; an empty one when the file is missing, unreadable or not a record.
    // Never throws: a failure is logged and reads as "no conversation".
    HarnessRecord LoadHarnessRecordFrom(const std::wstring& path) noexcept;

    // Delete the record file. A missing file is success.
    //   returns — true when no file is left at `path`.
    // Never throws.
    bool DeleteHarnessRecordAt(const std::wstring& path) noexcept;

    // ===== launch policy ====================================================================

    // What BuildHarnessSettingsJson needs to know.
    struct HarnessSettingsSpec
    {
        int64_t autoCompactTokens{}; // 0 = leave auto-compaction as Claude Code decides; else the window in tokens (clamped to 100,000..1,000,000)
        std::wstring effort; // "low" | "medium" | "high" | "xhigh" | "max"; anything else (or empty) sets no effort
        int apiMaxRetries{ 3 }; // how often Claude Code retries a failed API request (clamped to 0..15)
    };

    // The JSON object the harness passes as `--settings`. It is the HIGHEST-priority settings a
    // launch can give, and the harness loads no other (it starts Claude Code with
    // `--setting-sources ""`), so this text decides:
    //   - "verbose": false — the legacy config (~/.claude.json) may say true.
    //   - "autoCompactEnabled": true + "autoCompactWindow" — ⚠ WITHOUT the first key Claude Code
    //     falls back to the legacy config, where a user may have turned auto-compaction OFF; the
    //     --autocompact flag alone then sets a window that never compacts (measured on Claude
    //     Code 2.1.291: isAutoCompactEnabled false). Both keys are written only when
    //     spec.autoCompactTokens is not 0.
    //   - "env": bounded API retries and no retry watchdog (a user's settings can make a failed
    //     request retry for hours), the effort level (the CLAUDE_CODE_EFFORT_LEVEL variable beats
    //     the --effort flag), and the switches that keep a background child quiet and fast: no
    //     auto-update, no nonessential traffic, no terminal-title model call, no CLAUDE.md, no
    //     auto memory, no background tasks.
    //   returns — ASCII JSON text (safe on any command line).
    //   throws  — std::bad_alloc.
    std::wstring BuildHarnessSettingsJson(const HarnessSettingsSpec& spec);

    // The clamp BuildHarnessSettingsJson and the --autocompact flag share: 0 stays 0 (no window),
    // any other value is brought into Claude Code's accepted range, 100,000..1,000,000.
    int64_t ClampHarnessAutoCompactTokens(int64_t tokens) noexcept;

    // The environment changes for a harness child, as (name -> value) with UTF-8 values, in the
    // form the SDK's options take. A NON-empty value sets the variable; an EMPTY value REMOVES it
    // from the child (the convention of Agentmaster's process backend, ClaudeSdkProcessWin32.cpp).
    //   name    — the conversation key; it becomes AM_HARNESS, the mark by which the Fleet
    //             Observer and the CLI recognize (and skip) a harness child.
    //   returns — removals for every variable of a parent Claude Code session or terminal tab
    //             (CLAUDECODE, CLAUDE_CODE_SESSION_ID, WT_SESSION, AM_SESSION, the hook pipe, the
    //             effort variables, DEBUG, ...), plus AM_HARNESS=<name>.
    //   throws  — std::bad_alloc.
    // Why removals matter: Agentmaster can itself be started from inside a Claude Code session or
    // a terminal tab; a child that inherits those variables reports the parent's session and tab
    // as its own, and can reach the parent's messaging socket.
    // The mark's name is kHarnessEnvMarker (Activity.h, beside the process facts that read it).
    std::map<std::string, std::string> BuildHarnessEnvironment(std::wstring_view name);

    // A path as the narrow string the SDK can use. The SDK checks its CLI path with
    // std::filesystem, which reads a narrow string in the ANSI code page, so:
    //   - a path the ANSI code page can hold exactly is returned in that code page;
    //   - else its 8.3 short form, when the volume has one and it is ASCII;
    //   - else nothing.
    //   returns — the narrow path, or nullopt when the path has no exact narrow form.
    //   throws  — std::bad_alloc.
    // Pure ASCII paths (the usual case) come back unchanged.
    std::optional<std::string> NarrowPathForSdk(const std::wstring& path);

    // "mcp__<server>__<tool>" — the name Claude Code gives a tool of an MCP server, and the name
    // the model calls it by.
    //   throws — std::bad_alloc.
    std::wstring HarnessToolFullName(std::wstring_view server, std::wstring_view tool);

    // UTF-16 <-> UTF-8 for text that crosses to the SDK (which holds UTF-8 in std::string).
    //   returns — the converted text; empty for empty input or text that cannot be converted.
    //   throws  — std::bad_alloc.
    std::string HarnessToUtf8(std::wstring_view text);
    std::wstring HarnessFromUtf8(std::string_view text);

    // ===== readers of what Claude Code reports ==============================================

    // What a `system` line of the stream means to the harness.
    enum class HarnessSystemKind
    {
        Other, // not one of the below (or not a system line at all)
        Init, // the start of a turn: model, session id, version, the tools and MCP servers in effect
        Compacting, // Claude Code started to compact the context
        CompactFinished, // the compaction ended; compactOk says how
        CompactBoundary, // the compaction's record: preTokens -> postTokens
    };

    // The facts of one `system` line. Fields that the line does not carry stay empty / 0.
    struct HarnessSystemFacts
    {
        HarnessSystemKind kind{ HarnessSystemKind::Other };
        std::wstring model; // Init
        std::wstring sessionId; // Init
        std::wstring cliVersion; // Init: e.g. "2.1.291"
        std::wstring permissionMode; // Init
        std::vector<std::wstring> tools; // Init: the tool names the model can call
        std::vector<std::pair<std::wstring, std::wstring>> mcpServers; // Init: (name, status) — status "connected" is the good one
        bool compactOk{}; // CompactFinished: the compaction succeeded
        std::wstring compactTrigger; // CompactBoundary: "auto" | "manual"
        int64_t preTokens{}; // CompactBoundary: the context before
        int64_t postTokens{}; // CompactBoundary: the context after
        int64_t compactDurationMs{}; // CompactBoundary
    };

    // Read one `system` line (the line's whole JSON object). Pure and total.
    //   throws — std::bad_alloc.
    HarnessSystemFacts ClassifyHarnessSystemMessage(std::wstring_view rawJson);

    // The facts of a `result` line: the end of one user message's turn.
    struct HarnessResultFacts
    {
        bool valid{}; // the text was a result line
        // ⚠ The FAILURE flag is `is_error`, not `subtype`: a wrong model name or an unreachable
        // API ends with subtype "success" and is_error true (measured on Claude Code 2.1.291).
        bool isError{};
        std::wstring subtype; // "success", "error_during_execution", "error_max_turns", ...
        std::wstring text; // the turn's final text (the `result` field); may be empty on an error
        std::wstring stopReason;
        std::vector<std::wstring> errors; // the `errors` list, when the line has one
        int apiErrorStatus{}; // the failing request's HTTP status, when reported
        double totalCostUsd{}; // cumulative for the PROCESS, not for this turn
        int64_t durationMs{};
        int64_t durationApiMs{};
        int numTurns{}; // model requests in this turn (a tool call makes one more)
        int64_t contextWindow{}; // the model's window (modelUsage.<model>.contextWindow); 0 when not reported
    };

    // Read one `result` line. Pure and total.
    //   throws — std::bad_alloc.
    HarnessResultFacts ParseHarnessResultMessage(std::wstring_view rawJson);

    // One request's usage block (an assistant line's `message.usage`).
    struct HarnessRequestUsage
    {
        int64_t inputTokens{};
        int64_t outputTokens{};
        int64_t cacheReadTokens{};
        int64_t cacheCreationTokens{};

        // The context this request occupied — the figure the app shows as "ctx N" everywhere.
        int64_t Context() const noexcept { return inputTokens + cacheReadTokens + cacheCreationTokens + outputTokens; }
    };

    // Read a usage object. Pure and total: missing numbers are 0.
    //   throws — std::bad_alloc.
    HarnessRequestUsage ParseHarnessRequestUsage(std::wstring_view usageJson);
}
