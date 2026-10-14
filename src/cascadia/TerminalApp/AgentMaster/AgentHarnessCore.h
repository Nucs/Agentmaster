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

    // ----- the permission policy -----------------------------------------------------------
    //
    // THE RULE (the user's, 2026-10-12): a harness process ALWAYS runs with bypass permissions,
    // and in auto mode when bypass is not allowed. The harness adds one last step for the case
    // that auto is not allowed either: "dontAsk".
    //
    // Why a rule is necessary: a harness process has no window, so nobody can answer a permission
    // question. Each of the three modes decides by itself (HarnessPermissionModeNeverAsks):
    //   - "bypassPermissions": every tool call runs. Nothing is asked and nothing is classified.
    //   - "auto": a classifier model decides for each tool call that no allow rule covers. That
    //     costs 0.3 to 0.6 s per call (measured, Claude Code 2.1.291); a call that an allow rule
    //     covers runs at once. So the harness keeps its --allowedTools list in every mode.
    //   - "dontAsk": a tool call that no allow rule covers is refused.
    // Every other mode ("default" — the wire word of "manual" —, "acceptEdits", "plan") can ask.
    //
    // HOW "NOT ALLOWED" SHOWS (measured on 2.1.291; nothing of it is printed):
    //   - SOFT: Claude Code starts, in a lower mode. With the settings switch
    //     permissions.disableBypassPermissionsMode it starts in "auto"; with
    //     permissions.disableAutoMode too, in "default". The one sign is the mode it REPORTS:
    //     `current_permission_mode` in its answer to `initialize` (before any model request), and
    //     `permissionMode` in each turn's system/init line.
    //   - HARD: Claude Code ends at start with an error text. `--restricted` does that for bypass
    //     ("Error: bypassPermissions not supported in restricted mode"), and a Claude Code that
    //     does not know a mode word does it for that word ("error: option '--permission-mode
    //     <mode>' argument '...' is invalid").
    //   - A switch to a refused mode (the control request set_permission_mode) is answered with
    //     an error ("Cannot set permission mode to auto: auto mode disabled by settings").
    //
    // The functions below are the DECISIONS; AgentHarness.cpp carries them out
    // (EnsureProcess, SettlePermissionMode).

    // The permission modes a harness process may end in, in the order the harness prefers them.
    enum class HarnessPermissionMode
    {
        Bypass, // "bypassPermissions" (what --dangerously-skip-permissions selects)
        Auto, // "auto"
        DontAsk, // "dontAsk"
    };

    // The word Claude Code uses for `mode`: the --permission-mode value, and what it reports.
    const wchar_t* HarnessPermissionModeWord(HarnessPermissionMode mode) noexcept;

    // The mode to ASK for after Claude Code refused `mode` with an error at start:
    // Bypass -> Auto -> DontAsk.
    //   returns — nullopt after DontAsk: nothing lower exists, so that start failure is final.
    std::optional<HarnessPermissionMode> NextHarnessPermissionMode(HarnessPermissionMode mode) noexcept;

    // True when the text of a FAILED START says that Claude Code refused the permission mode it
    // was asked for (the HARD refusal above), so that a start with the next lower mode can work.
    //   failureText — the start's failure text: the SDK's message plus the child's last stderr
    //                 lines. Compared without regard to ASCII case.
    //   returns     — true when it holds one of: "permission-mode", "permission mode",
    //                 "bypasspermissions", "bypass permissions", "skip-permissions", "auto mode".
    // ⚠ "permission" alone is not a sign: "permission denied" is what a file error says.
    bool HarnessStartFailureNamesPermissionMode(std::wstring_view failureText) noexcept;

    // True when `reportedMode` is one of the three modes that decide by themselves
    // ("bypassPermissions", "auto", "dontAsk"). False for every other word and for an empty one.
    bool HarnessPermissionModeNeverAsks(std::wstring_view reportedMode) noexcept;

    // What to do after a process reported the mode it runs in.
    enum class HarnessPermissionStep
    {
        Keep, // the mode decides by itself, or it is not known yet (an empty report)
        SwitchToAuto, // send set_permission_mode "auto"; when that is refused, SwitchToDontAsk
        SwitchToDontAsk, // send set_permission_mode "dontAsk"
    };

    // The step for a process that was ASKED for `asked` and REPORTS `reportedMode`.
    //   asked        — the mode the launch asked for.
    //   reportedMode — `current_permission_mode` / `permissionMode` as Claude Code reported it;
    //                  empty when this Claude Code version reports none at start.
    //   returns      — Keep for a mode that never asks and for an empty report (the mode is then
    //                  read from the first turn's system/init, and this function is called again);
    //                  SwitchToAuto when bypass was asked and a mode that can ask is in effect;
    //                  SwitchToDontAsk when auto (or dontAsk) was asked and such a mode is in
    //                  effect — auto was refused already, so asking for it again cannot work.
    // ⚠ "auto" after asking for bypass is Keep, not an error: it IS the rule's second half
    //   (Claude Code chose it because bypass is not allowed).
    HarnessPermissionStep DecideHarnessPermissionStep(HarnessPermissionMode asked, std::wstring_view reportedMode) noexcept;

    // ----- the thinking policy -------------------------------------------------------------
    //
    // How much the model reasons before it answers is a LEVEL: no thinking at all, or Claude
    // Code's ADAPTIVE thinking (the model decides when and how much to think) at one of Claude
    // Code's effort levels. On Sonnet 5.5 the effort is what defines the thinking level (the
    // user's words, 2026-10-14), so the levels are "off" plus the effort words.
    //
    // A level can change while the process runs, with no restart — the conversation keeps its
    // process and its whole context (measured on Claude Code 2.1.291 with control requests only,
    // no model request):
    //   - THINKING: the control request set_max_thinking_tokens. 0 turns thinking off; null
    //     brings back the thinking config the process was STARTED with — but only when that
    //     config was given explicitly: Claude Code keeps an explicit start config as its reset
    //     anchor (`new obr(thinkingConfig, thinkingConfigExplicit ? thinkingConfig : undefined)`
    //     in its bundle). A process started with `--thinking disabled` therefore has no way back
    //     to adaptive. So a harness process always starts with `--thinking adaptive`, and an
    //     "off" level is applied with set_max_thinking_tokens 0 before the first message.
    //   - EFFORT: the control request apply_flag_settings {"effortLevel": <word>}. The request
    //     get_settings then reports the effort in effect (applied.effort: after Claude Code's own
    //     caps and the model's step-down of a level it does not support).
    //     ⚠ The variable CLAUDE_CODE_EFFORT_LEVEL OVERRIDES a live change, and Claude Code still
    //     answers "success": measured, every change then stayed "low". So the harness never sets
    //     it (BuildHarnessSettingsJson) and removes an inherited one (BuildHarnessEnvironment);
    //     the start effort rides the --effort flag, which nothing then overrides.
    //   - OFF, on a model that REFUSES `"thinking": {"type": "disabled"}` (Sonnet 5.5 and every
    //     current model; HarnessModelRefusesDisabledThinking). set_max_thinking_tokens 0 alone
    //     does NOT turn thinking off there: Claude Code then LEAVES OUT the request's `thinking`
    //     field (its rule: send "disabled" only to a model that takes it), and the API runs its
    //     default, adaptive thinking (measured: with --thinking disabled, Sonnet 5.5 still thought,
    //     ~186 tokens on a small puzzle at effort high). Sent anyway, "disabled" gets an HTTP 400:
    //     "To turn thinking off on this model, send "thinking": {"type": "between_tools"} instead
    //     of {"type": "disabled"}. The model does not think before responding. The short updates
    //     it writes between tool calls come back as thinking blocks." Claude Code 2.1.291 does not
    //     know that type, so the harness sends it ITSELF: Claude Code spreads the JSON object in
    //     the variable CLAUDE_CODE_EXTRA_BODY over every request body AFTER its own `thinking`
    //     field, and reads the variable from its process environment at every request. So "off"
    //     on such a model is TWO live changes, in this order:
    //       1. set_max_thinking_tokens 0 — Claude Code now believes thinking is off, so it also
    //          leaves out its clear_thinking context strategy, which the API refuses next to
    //          between_tools ("`clear_thinking_20251015` strategy requires `thinking` to be
    //          enabled or adaptive" — measured with the variable alone);
    //       2. apply_flag_settings {"env": {... "CLAUDE_CODE_EXTRA_BODY": <between_tools>}}.
    //     Back on: the variable is cleared FIRST, then set_max_thinking_tokens null. Both orders
    //     keep the one combination the API refuses (Claude Code adaptive + between_tools) from
    //     ever existing between two requests.
    //     Measured in ONE process (Claude Code 2.1.291, Sonnet 5.5, 2026-10-14): off at effort
    //     high and at effort low — 0 thinking blocks, no error, the reasoning written in the
    //     visible answer instead; adaptive at effort max — 1 block, ~354 tokens; off again — 0.
    //     ⚠ apply_flag_settings merges SHALLOW: a patch's "env" REPLACES the whole env object of
    //     the --settings layer. So every patch carries the COMPLETE env object
    //     (BuildHarnessEnvPatchJson), never the one variable alone.
    //     ⚠ Claude Code ignores CLAUDE_CODE_EXTRA_BODY under an organization policy that turns
    //     experimental betas off ("ignored: experimental betas are disabled by your
    //     organization's policy"). "Off" then runs as the API's default (adaptive thinking).
    //     On a model that TAKES "disabled" (the older models Claude Code lists), "off" is the
    //     first change alone: Claude Code sends "disabled" itself, and the variable stays empty.
    //
    // The functions below are the DECISIONS; AgentHarness.cpp carries them out
    // (StartOnce, EnsureProcess, ApplyThinkingLive).

    // A thinking level, in the order the Sessions page's toggle rotates through them.
    enum class HarnessThinkingLevel
    {
        // No thinking before the answer, at effort "low": the fastest answer. (On a model that
        // refuses "disabled" this is the API's own "between_tools" off, the policy above: per the
        // API, the short updates the model writes BETWEEN tool calls may come back as thinking
        // blocks — the search agent's 4-request turn measured none, its notes came as text.)
        Off,
        Low, // adaptive thinking at effort "low"
        Medium, // adaptive thinking at effort "medium"
        High, // adaptive thinking at effort "high"
        XHigh, // adaptive thinking at effort "xhigh"
        Max, // adaptive thinking at effort "max": the deepest, and the slowest
    };

    // The level's word: "off", "low", "medium", "high", "xhigh" or "max". It is the persisted
    // form (AppSettings::sessionsAiThinkingLevel) and the word a page shows; for every level but
    // Off it is also Claude Code's effort word.
    const wchar_t* HarnessThinkingLevelWord(HarnessThinkingLevel level) noexcept;

    // Read a level word back.
    //   word    — compared without regard to ASCII case; spaces and tabs around it are ignored.
    //   returns — the level; nullopt for any other word (a hand-edited or a future setting), which
    //             a caller treats as Off.
    std::optional<HarnessThinkingLevel> ParseHarnessThinkingLevel(std::wstring_view word) noexcept;

    // True for every level but Off: the level runs adaptive thinking.
    bool HarnessThinkingLevelThinks(HarnessThinkingLevel level) noexcept;

    // The effort Claude Code gets at `level`: the level's own word for Low..Max, and "low" for
    // Off — no thinking keeps the lowest effort, the fastest answer, which was the search agent's
    // only setting before the levels existed.
    const wchar_t* HarnessThinkingLevelEffort(HarnessThinkingLevel level) noexcept;

    // The level after `level` in the toggle's rotation, Off -> Low -> Medium -> High -> XHigh ->
    // Max -> Off, skipping a level whose effort the model does not support.
    //   level            — the current level.
    //   supportedEfforts — the model's effort words as Claude Code reports them
    //                      (HarnessModelReasoning::supportedEfforts). EMPTY = not known yet
    //                      (no process answered): every level counts as supported. Off is
    //                      always offered, so the rotation always ends.
    //   returns          — the next offered level; Off after the last one.
    HarnessThinkingLevel NextHarnessThinkingLevel(HarnessThinkingLevel level, const std::vector<std::wstring>& supportedEfforts) noexcept;

    // True when `effort` is one of Claude Code's effort words ("low", "medium", "high", "xhigh",
    // "max"). Anything else must not reach the command line or a control request: Claude Code
    // ignores an unknown --effort with a warning, and would hand a wrong value to the API.
    bool IsHarnessEffortWord(std::wstring_view effort) noexcept;

    // True when the API refuses `"thinking": {"type": "disabled"}` for `model`, so that "off"
    // has to be the API's own `{"type": "between_tools"}` (the thinking policy above). It mirrors
    // the rule Claude Code 2.1.291 decides by for the first-party API (qEe in its bundle): a
    // model TAKES "disabled" when Claude Code lists it — Claude 3, Opus 4.0, 4.1 and 4.5 to 4.8,
    // Opus 5, Sonnet 4.0, 4.5, 4.6 and 5, Haiku 4.5 — and refuses it otherwise. That way the
    // harness adds between_tools exactly where Claude Code leaves the field out.
    //   model   — a model id as --model takes it or as Claude Code reports it (resolvedModel).
    //             Compared without regard to ASCII case, after dropping what does not name the
    //             model: the "[1m]" context marker, a provider prefix up to "anthropic." (Bedrock),
    //             an "@..." suffix (Vertex), a "-v<n>[:<n>]" suffix (Bedrock), and a trailing
    //             "-YYYYMMDD" date.
    //   returns — false for the listed models; true for every other name, an alias such as
    //             "sonnet" and an empty name included (Claude Code's own default for a model it
    //             does not list: a current model). A caller that knows the alias's resolved id
    //             (HarnessModelReasoning::resolvedModel) passes that instead.
    // ⚠ A new Claude Code version can change the list. When "off" stops working (an API 400 that
    //   names between_tools, or thinking in an "off" turn without tools), compare with its bundle.
    bool HarnessModelRefusesDisabledThinking(std::wstring_view model) noexcept;

    // The request-body object that turns thinking off on a model that refuses "disabled":
    // CLAUDE_CODE_EXTRA_BODY's value for an "off" level there (the thinking policy above).
    inline constexpr std::wstring_view kHarnessBetweenToolsRequestBody = LR"({"thinking":{"type":"between_tools"}})";

    // The reasoning a process runs with, as far as the harness sets it: the three things a live
    // change can move.
    struct HarnessReasoningState
    {
        // Claude Code's own thinking config: true = adaptive, false = off
        // (set_max_thinking_tokens 0).
        bool thinks{ true };
        // The request body carries kHarnessBetweenToolsRequestBody (CLAUDE_CODE_EXTRA_BODY): only
        // ever with `thinks` false, and only for a model that refuses "disabled".
        bool betweenTools{ false };
        // The effort word; empty = none was set (Claude Code's default).
        std::wstring effort;
    };

    // What has to change in a RUNNING process to go from the reasoning it has to the one wanted.
    // Only for a process that was started with `--thinking adaptive` (the thinking policy above):
    // that is the one kind whose thinking can be turned off and on again live.
    // SEND ORDER (ApplyThinkingLive, AgentHarness.cpp): betweenToolsOff, thinkingOff, thinkingOn,
    // betweenToolsOn, effort. So a change toward "on" clears the body before Claude Code thinks
    // again, and a change toward "off" sets the body only after Claude Code stopped thinking: the
    // API refuses between_tools next to Claude Code's adaptive request.
    struct HarnessThinkingChange
    {
        bool thinkingOff{}; // send set_max_thinking_tokens 0
        bool thinkingOn{}; // send set_max_thinking_tokens null (back to the adaptive start config)
        bool betweenToolsOn{}; // send apply_flag_settings with the env object whose CLAUDE_CODE_EXTRA_BODY is between_tools
        bool betweenToolsOff{}; // send apply_flag_settings with the env object whose CLAUDE_CODE_EXTRA_BODY is empty
        std::wstring effort; // non-empty: send apply_flag_settings {"effortLevel": effort}

        // True when the process already runs as wanted: no request is to be sent.
        bool Empty() const noexcept { return !thinkingOff && !thinkingOn && !betweenToolsOn && !betweenToolsOff && effort.empty(); }
    };

    // Plan the control requests for one change. Pure.
    //   applied — what the process runs with now, as the harness last set it.
    //   wanted  — what is wanted. An empty or unknown wanted.effort keeps the applied effort (no
    //             effort request). A wanted.betweenTools with wanted.thinks true is a caller error
    //             and is read as false (the API refuses that pair).
    //   returns — the requests; Empty() when nothing differs.
    //   throws  — std::bad_alloc.
    HarnessThinkingChange PlanHarnessThinkingChange(const HarnessReasoningState& applied, const HarnessReasoningState& wanted);

    // What Claude Code says one model can do in reasoning: the model's entry in the `models` list
    // of its answer to `initialize`.
    struct HarnessModelReasoning
    {
        bool known{}; // the list had an entry for the model
        // The entry's wire id: its resolvedModel ("claude-sonnet-5-5" for the "sonnet" row), else
        // its value. What HarnessModelRefusesDisabledThinking is to be asked about, since --model
        // may be an alias.
        std::wstring resolvedModel;
        std::vector<std::wstring> supportedEfforts; // supportedEffortLevels, in Claude Code's order (lower-cased); empty when the entry lists none
        bool adaptiveThinking{}; // supportsAdaptiveThinking
    };

    // Find `model` in the `models` array of an initialize answer. Pure and total.
    //   modelsJson — the ARRAY's JSON text, and only that: the answer also holds the user's
    //                ACCOUNT object, which must never be passed around or logged.
    //   model      — the --model value. An entry matches when its `value` or its `resolvedModel`
    //                is this text (ASCII case ignored): Claude Code lists Sonnet 5.5 as
    //                {"value": "sonnet", "resolvedModel": "claude-sonnet-5-5", ...}.
    //   returns    — the entry's facts; `known` false when no entry matches or the text is not an
    //                array.
    //   throws     — std::bad_alloc.
    // Measured on Claude Code 2.1.291: Sonnet 5.5 lists low, medium, high, xhigh and max, and
    // supports adaptive thinking.
    HarnessModelReasoning ReadHarnessModelReasoning(std::wstring_view modelsJson, std::wstring_view model);

    // What BuildHarnessSettingsJson needs to know.
    struct HarnessSettingsSpec
    {
        int64_t autoCompactTokens{}; // 0 = leave auto-compaction as Claude Code decides; else the window in tokens (clamped to 100,000..1,000,000)
        int apiMaxRetries{ 3 }; // how often Claude Code retries a failed API request (clamped to 0..15)
        // The value of CLAUDE_CODE_EXTRA_BODY: a JSON object's text that Claude Code spreads over
        // every request body (kHarnessBetweenToolsRequestBody for an "off" level on a model that
        // refuses "disabled", the thinking policy above), or empty for none. The variable is
        // written EITHER WAY: the harness owns it, so a value inherited from the user's
        // environment never reaches a request.
        std::wstring extraRequestBody;
        // TEST SEAM (HarnessPermissionProbe, AgentHarness.h): write the settings switch with which
        // an administrator forbids a permission mode. Claude Code then refuses that mode for this
        // process. No product code sets either.
        bool disableBypassPermissions{ false }; // "permissions": { "disableBypassPermissionsMode": "disable" }
        bool disableAutoMode{ false }; // "permissions": { "disableAutoMode": "disable" }
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
    //     request retry for hours), and the switches that keep a background child quiet and fast:
    //     no auto-update, no nonessential traffic, no terminal-title model call, no CLAUDE.md, no
    //     auto memory, no background tasks; and CLAUDE_CODE_EXTRA_BODY = spec.extraRequestBody
    //     (empty included). A live change of that variable sends this same env object again
    //     (BuildHarnessEnvPatchJson), so the two never disagree.
    //     ⚠ NOT the effort variable CLAUDE_CODE_EFFORT_LEVEL (it was written here until
    //     2026-10-14): it overrides a live effort change (the thinking policy above), so the
    //     effort rides the --effort flag alone.
    //   - "permissions": ONLY for the test seam (spec.disableBypassPermissions /
    //     spec.disableAutoMode). A product launch writes no "permissions" key: the permission mode
    //     is asked for on the command line, and the tools are limited by --tools and
    //     --allowedTools.
    //   returns — ASCII JSON text (safe on any command line).
    //   throws  — std::bad_alloc.
    std::wstring BuildHarnessSettingsJson(const HarnessSettingsSpec& spec);

    // The `settings` object of an apply_flag_settings request that changes CLAUDE_CODE_EXTRA_BODY
    // live: {"env": <the COMPLETE env object of BuildHarnessSettingsJson(spec)>}. Complete,
    // because the request merges shallow and its "env" replaces the whole env object of the
    // --settings layer (the thinking policy above); a patch with the one variable alone would
    // drop the retry bound and the quiet switches from the settings Claude Code reports.
    //   spec    — the spec of the running process, with the new extraRequestBody.
    //   returns — ASCII JSON text.
    //   throws  — std::bad_alloc.
    std::wstring BuildHarnessEnvPatchJson(const HarnessSettingsSpec& spec);

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
    //             effort variables, DEBUG, ...) and for CLAUDE_CODE_EXTRA_BODY (the harness sets
    //             that one itself, through its settings: the thinking policy), plus
    //             AM_HARNESS=<name>.
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
        Init, // the start of a turn: model, session id, version, and everything the model can use
        Compacting, // Claude Code started to compact the context
        CompactFinished, // the compaction ended; compactOk says how
        CompactBoundary, // the compaction's record: preTokens -> postTokens
        PermissionDenied, // Claude Code refused a tool call of the model: deniedTool, deniedReason
        // Claude Code's running ESTIMATE of the thinking of the request in flight:
        // thinkingTokens (so far) and thinkingTokensDelta (since the line before). Sent only
        // while the model thinks, so it is the stream's own sign that a turn thought — also when
        // the thinking text itself is omitted (the default thinking display of stream-json:
        // the thinking block then arrives with an empty text and only a signature).
        ThinkingTokens,
    };

    // The facts of one `system` line. Fields that the line does not carry stay empty / 0.
    struct HarnessSystemFacts
    {
        HarnessSystemKind kind{ HarnessSystemKind::Other };
        std::wstring model; // Init
        std::wstring sessionId; // Init
        std::wstring cliVersion; // Init: e.g. "2.1.291"
        std::wstring permissionMode; // Init: the mode in effect ("bypassPermissions", "auto", "dontAsk", "default", ...)
        std::wstring apiKeySource; // Init: where the credentials come from; "none" = the Claude login, no API key
        std::wstring outputStyle; // Init: e.g. "default"
        std::vector<std::wstring> tools; // Init: the tool names the model can call
        std::vector<std::pair<std::wstring, std::wstring>> mcpServers; // Init: (name, status) — status "connected" is the good one
        std::vector<std::wstring> slashCommands; // Init: the commands a message that starts with '/' can run
        std::vector<std::wstring> skills; // Init: the skills the model can load
        // Init: the subagent types Claude Code knows. ⚠ Listed even when the model has no tool to
        // start one (a harness process has none): this is what EXISTS, not what can be called.
        std::vector<std::wstring> agents;
        std::vector<std::wstring> plugins; // Init: the loaded plugins, by name
        std::wstring deniedTool; // PermissionDenied: the tool's full name
        std::wstring deniedReason; // PermissionDenied: Claude Code's text (it names the permission mode or the missing grant)
        bool compactOk{}; // CompactFinished: the compaction succeeded
        std::wstring compactTrigger; // CompactBoundary: "auto" | "manual"
        int64_t preTokens{}; // CompactBoundary: the context before
        int64_t postTokens{}; // CompactBoundary: the context after
        int64_t compactDurationMs{}; // CompactBoundary
        int64_t thinkingTokens{}; // ThinkingTokens: estimated_tokens, the request's thinking so far
        int64_t thinkingTokensDelta{}; // ThinkingTokens: estimated_tokens_delta, since the line before
    };

    // Read one `system` line (the line's whole JSON object). Pure and total.
    //   throws — std::bad_alloc.
    HarnessSystemFacts ClassifyHarnessSystemMessage(std::wstring_view rawJson);

    // What a harness process reported about ITSELF: everything the model in it can use, and the
    // rules it runs under. It is the process's own account (its answer to `initialize` and the
    // first turn's system/init line), not what the launch asked for — the two differ exactly when
    // something was refused or added, which is what a review looks for.
    struct HarnessInventory
    {
        // False until a process answered `initialize` (no process was started yet, or the last
        // start failed). Everything below is then empty.
        bool known{};
        // False until the first turn of the process: Claude Code sends the lists (tools, servers,
        // commands, skills, agents, plugins) only with a turn's system/init line. Until then
        // only the two permission fields are filled.
        bool complete{};
        std::wstring cliVersion; // e.g. "2.1.291"
        std::wstring model;
        std::wstring askedPermissionMode; // what the launch asked for ("bypassPermissions" unless a start with it was refused)
        std::wstring permissionMode; // the mode in effect, after the harness's own correction
        // The model's reasoning, as the process reported it and as the harness set it (the
        // thinking policy). Filled when the process answered `initialize`, and again after every
        // live change.
        // What the harness set: "adaptive"; "disabled" (thinking off through Claude Code's own
        // "disabled", for a model that takes it); "between_tools" (thinking off through the API's
        // between_tools, for a model that refuses "disabled"); "default" for a process left to
        // Claude Code's own default.
        std::wstring thinking;
        std::wstring effort; // the effort IN EFFECT, as get_settings reports it (after Claude Code's caps and the model's step-down); empty when it could not be read
        std::vector<std::wstring> supportedEfforts; // the model's effort words (initialize answer); empty when not reported
        bool adaptiveThinking{}; // the model supports adaptive thinking (initialize answer)
        std::wstring apiKeySource; // "none" = the Claude login
        std::wstring outputStyle;
        std::vector<std::wstring> tools; // every tool the model can call, by full name
        std::vector<std::pair<std::wstring, std::wstring>> mcpServers; // (name, status)
        std::vector<std::wstring> slashCommands;
        std::vector<std::wstring> skills;
        std::vector<std::wstring> agents; // subagent types that exist (see HarnessSystemFacts::agents)
        std::vector<std::wstring> plugins;
    };

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
