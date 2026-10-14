// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — AgentHarness: a Claude conversation that Agentmaster hosts itself
// (SDK_HARNESS.md).
//
// A HarnessConversation owns ONE windowless Claude Code process, driven through the Claude Agent
// SDK (refs/claude-agent-sdk-cpp): stream-json in both directions, the control protocol, and
// tools that run INSIDE Agentmaster (in-process MCP servers). It is what `claude -p` is for a
// script, kept open: the process stays up between messages, so a follow-up message costs no
// process start and keeps its context.
//
//   caller thread            worker thread                  SDK reader thread
//   -------------            -------------                  -----------------
//   Send(text)  ──────────►  start the process if needed
//                            send the user message  ──────► claude.exe
//                            read the stream        ◄────── assistant / system / result lines
//                                                           a tool call: HarnessTool::handler runs HERE
//   EventSink(event) ◄────── state, text, the turn's end    ToolCall / ToolResult events
//
// What the harness decides for every conversation (the launch policy, AgentHarnessCore.h):
//   - no built-in tools: the model can call ONLY the tools of HarnessOptions::toolServers;
//   - no settings, no CLAUDE.md, no hooks, no MCP servers of the user (--setting-sources "",
//     --strict-mcp-config), and a system prompt that REPLACES Claude Code's own;
//   - nothing written under ~/.claude (--no-session-persistence): the conversation's durable
//     form is Agentmaster's own record (HarnessRecord), in the profile;
//   - BYPASS PERMISSIONS, always; auto mode when Claude Code does not allow bypass; "dontAsk"
//     when it allows neither. A windowless process must never wait for a person's answer. The
//     mode in effect is read from the process itself (HarnessConversation::Inventory).
//     ⚠ With bypass, NOTHING refuses a tool call. What the model can do is decided by the tools
//     it is given (--tools "", the tool servers) — never by the permission mode. Give a
//     conversation a tool only when it may use that tool without limit.
//
// This header needs neither the SDK nor nlohmann/json: only AgentHarness.cpp includes them.
// Plain C++, no WinRT (the .cpp is <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AgentHarnessCore.h" // HarnessRecord / HarnessUsage / HarnessInventory
#include "Json.h" // json::Value — a tool's arguments

namespace Agentmaster
{
    // ===== tools ============================================================================

    // What a tool answers. The model reads `text`; `isError` tells it that the call failed, so it
    // can correct its arguments instead of trusting the text.
    struct HarnessToolResult
    {
        std::wstring text;
        bool isError{ false };
    };

    // One tool the model can call.
    struct HarnessTool
    {
        std::wstring name; // letters, digits, '_' and '-'; the model sees mcp__<server>__<name>
        std::wstring description; // what the tool does and when to call it — the model's ONLY manual
        // A JSON Schema object for the arguments, e.g.
        //   {"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}
        // ⚠ Claude Code does not validate a call against it: `handler` must check what it reads.
        std::wstring inputSchemaJson;
        // True: the tool only reads. Claude Code may then run several calls of one model turn at
        // the same time, so `handler` must be safe to run concurrently with itself.
        bool readOnly{ false };
        // Runs the call. Called on the SDK's reader thread, with the conversation's process
        // waiting: a slow handler delays the model, and nothing else of this conversation is
        // read meanwhile.
        //   arguments — the call's arguments, an object (an empty object when the model sent none).
        //   returns   — the result text for the model.
        //   throws    — anything: the harness catches it, logs it (Rule #18) and answers the model
        //               with an error result that carries the exception's message.
        // ⚠ It must not call the conversation's Shutdown / destructor (they wait for this thread).
        std::function<HarnessToolResult(const json::Value& arguments)> handler;
    };

    // A named group of tools: one in-process MCP server. The name is part of every tool's full
    // name. The harness also puts `mcp__<name>` on the --allowedTools list: with bypass
    // permissions that list decides nothing, but in the two fallback modes it is what lets the
    // model call the tools — at once in auto mode (no classifier request, measured 0.3 to 0.6 s
    // per call without it), and at all in "dontAsk".
    struct HarnessToolServer
    {
        std::wstring name; // letters, digits, '_' and '-'
        std::wstring version{ L"1.0.0" };
        std::vector<HarnessTool> tools;
    };

    // ===== options ==========================================================================

    // How the model may think before it answers. The THINKING POLICY of AgentHarnessCore.h says
    // how each is carried out; in short:
    enum class HarnessThinking
    {
        // Pass nothing: Claude Code decides (adaptive on current models). A process started this
        // way cannot be switched live: a later SetThinking restarts it.
        CliDefault,
        // No thinking before the answer (the fastest; measured 0 thinking blocks on Sonnet 5.5).
        // The process is started with --thinking adaptive and switched off with the control
        // request set_max_thinking_tokens 0 before the first message, so that a later SetThinking
        // can turn thinking on again without a restart. On a model that refuses the API's
        // "disabled" (Sonnet 5.5 and every current model) that alone is not off — Claude Code then
        // leaves the field out and the API thinks by default —, so the harness also puts the
        // API's own off, `"thinking": {"type": "between_tools"}`, into the request body
        // (CLAUDE_CODE_EXTRA_BODY, changed live too). Nothing is thought before the answer; per
        // the API, the short updates the model writes BETWEEN tool calls may come back as
        // thinking blocks (the search agent measured none: its notes came as text).
        Disabled,
        // --thinking adaptive: the model decides when and how much to think; the effort level
        // (HarnessOptions::effort, SetThinking) sets how deep.
        Adaptive,
    };

    // TEST SEAM of the permission policy (AgentHarnessCore.h). No product code sets a field.
    // Each field makes Claude Code REFUSE a permission mode for this one conversation, which is
    // the only way to run the "bypass is not allowed" paths on a machine whose policy allows
    // everything. The launch still asks for bypass first: the refusal is Claude Code's, exactly
    // as with a real policy.
    struct HarnessPermissionProbe
    {
        // --settings permissions.disableBypassPermissionsMode. Claude Code then starts in "auto"
        // by itself and says nothing (the SOFT refusal).
        bool bypassDisabledBySettings{ false };
        // --settings permissions.disableAutoMode. Together with the field above, Claude Code
        // starts in "default" and refuses a switch to auto; the harness then switches to "dontAsk".
        bool autoDisabledBySettings{ false };
        // --restricted. Claude Code then ENDS at start with an error when it is asked for bypass
        // (the HARD refusal); the harness starts it again with auto.
        bool restricted{ false };
    };

    // Everything that defines a conversation. Read once, by the constructor — except the
    // reasoning (`effort`, `thinking`), whose start values HarnessConversation::SetThinking
    // replaces while the conversation lives.
    struct HarnessOptions
    {
        // The conversation's key (IsValidHarnessName): the record file, the working folder, the
        // AM_HARNESS mark of the child process and the log tag all carry it. Two live
        // conversations must not share one.
        std::wstring name;

        // Returns the native claude.exe to start, or empty when there is none. Called on the
        // worker thread at EVERY process start, so a path that changes while the app runs (the
        // Settings override, an install) is picked up without a new conversation object.
        // Must be thread-safe and must not throw.
        std::function<std::wstring()> claudeExe;

        std::wstring model{ L"claude-sonnet-5-5" }; // the --model value
        // The START values of the model's reasoning. HarnessConversation::SetThinking replaces both
        // later; every process start (also after a park or a reset) uses the newest values.
        std::wstring effort{ L"low" }; // low | medium | high | xhigh | max; empty or unknown = Claude Code's default
        HarnessThinking thinking{ HarnessThinking::Disabled };

        // The whole system prompt (it REPLACES Claude Code's). Passed through a file, so its
        // length and characters are not limited by the command line.
        std::wstring systemPrompt;

        std::vector<HarnessToolServer> toolServers;

        // Compact the context when it reaches this many tokens (Claude Code's range: 100,000 to
        // 1,000,000; other values are clamped). 0 = no window is set and auto-compaction is left
        // as Claude Code decides.
        int64_t autoCompactTokens{ 500000 };

        // false (the default): --no-session-persistence — Claude Code writes no transcript, so
        // the conversation never appears among the user's sessions. true: Claude Code keeps its
        // own transcript under ~/.claude/projects/<the working folder>.
        bool claudePersistsSession{ false };

        // true: keep the HarnessRecord in <profile>\harness\<name>.json, load it at construction
        // and give a new process a digest of it. false: the record lives in memory only.
        bool persistRecord{ true };

        // Empty = the defaults under the profile: <profile>\harness\<name> (the child's working
        // folder, created on demand) and <profile>\harness\<name>.json. Set them only in tests.
        std::wstring workDir;
        std::wstring recordPath;

        int maxTurnsPerMessage{ 0 }; // the most model requests one user message may cause (--max-turns); 0 = no limit
        int apiMaxRetries{ 3 }; // retries of a failed API request before the turn fails
        // Report text as it is generated (AssistantDelta events). Costs one stream line per
        // token group; leave it off when only whole messages are shown.
        bool partialMessages{ false };

        // The cold-start digest (BuildHarnessDigest): how much of the record a NEW process is
        // told when it gets its first message.
        size_t digestTurns{ 6 };
        size_t digestChars{ 6000 };

        // A running turn that produces no stream line for this long (tool time not counted) is
        // taken for hung: the process is ended and the turn fails. 0 = never.
        int64_t turnSilenceTimeoutMs{ 180000 };

        // Tests only (see HarnessPermissionProbe). The default refuses nothing.
        HarnessPermissionProbe permissionProbe;
    };

    // ===== events ===========================================================================

    // Where a conversation is.
    enum class HarnessState
    {
        Stopped, // no process (never started, reset, or shut down)
        Starting, // the process is being started
        Ready, // the process is up and waits for a message
        Running, // a user message is being answered
        Failed, // the last start or turn failed; the next Send / Prewarm tries again
    };

    // What an event reports.
    enum class HarnessEventKind
    {
        State, // `state` changed (nothing else to say)
        Ready, // the process answered its first turn's start: `model`, `cliVersion`, `permissionMode`
        UserMessage, // a message went to the model: `text` (the user's words, without the digest)
        AssistantDelta, // partialMessages only: `text` is the next piece of the answer
        AssistantText, // `text` is one complete text block of the answer
        ToolCall, // the model called `toolName` with `argumentsJson`
        ToolResult, // that call's answer: `toolName`, `text`, `isError`
        Compacting, // Claude Code started to compact the context (the turn pauses for seconds)
        Compacted, // it finished: `preTokens` -> `postTokens`, `durationMs`
        TurnCompleted, // the user message is answered: `text`, `isError`, `interrupted`, `durationMs`, `usage`
        Notice, // `text`: something the user should know (the process was restarted, ...)
        Error, // `text`: a start or protocol failure; `state` is Failed
    };

    // One event. Only the fields its kind names are meaningful; `epoch`, `state` and `usage` are
    // set on every event.
    struct HarnessEvent
    {
        HarnessEventKind kind{ HarnessEventKind::State };
        // Counts the conversation's Resets, starting at 1. A listener that shows a conversation
        // drops every event whose epoch is older than the newest it saw: such an event belongs
        // to a conversation the user already threw away.
        uint64_t epoch{ 0 };
        uint64_t turn{ 0 }; // the user message it belongs to (the id Send returned); 0 = none
        HarnessState state{ HarnessState::Stopped }; // the state AFTER this event
        std::wstring text;
        std::wstring toolName; // the full name, mcp__<server>__<tool>
        std::wstring argumentsJson;
        bool isError{ false };
        bool interrupted{ false }; // TurnCompleted: stopped by Interrupt, by a newer Send, or by a Reset
        int64_t durationMs{ 0 }; // TurnCompleted: from Send to the result, as the harness measured it; Compacted: the compaction
        // TurnCompleted: how many thinking blocks the model wrote in this turn, and Claude Code's
        // ESTIMATE of their tokens (its system/thinking_tokens lines, summed) — the signs of
        // whether the turn thought: both are 0 with thinking off in a turn without tool calls
        // (with between_tools, per the API, the short updates between tool calls may come back as
        // thinking blocks), and adaptive thinking may also choose not to think about an easy request (or
        // one it answered before). The blocks arrive with an empty text (stream-json omits the
        // thinking itself), so the estimate is the only measure of HOW MUCH the model thought.
        int thinkingBlocks{ 0 };
        int64_t thinkingTokens{ 0 };
        int64_t preTokens{ 0 };
        int64_t postTokens{ 0 };
        std::wstring model;
        std::wstring cliVersion;
        // Ready: the permission mode the process runs in ("bypassPermissions"; "auto" or
        // "dontAsk" when Claude Code did not allow bypass).
        std::wstring permissionMode;
        HarnessUsage usage;
    };

    // ===== the conversation =================================================================

    // One hosted conversation. Create it once and keep it; it starts no process until Prewarm or
    // Send asks for one.
    //
    // Threads: every method may be called from any thread and returns at once (the work happens
    // on the conversation's worker thread) — EXCEPT Shutdown and the destructor, which wait for
    // the worker and for the process to end (a kill, so normally well under a second).
    //
    // The EventSink is called on the worker thread, on the SDK reader thread (tool events), or on
    // the thread that called Send / Reset (for the message that call replaced) — one call at a
    // time, never while the conversation holds its state lock. It may call Send / Interrupt /
    // Reset / State / Record. It must NOT call Shutdown or destroy the conversation, and it
    // should return fast: marshal to a UI thread instead of doing work in it.
    class HarnessConversation
    {
    public:
        using EventSink = std::function<void(const HarnessEvent&)>;

        // Create the conversation and (with persistRecord) load its record from the profile.
        //   options — see HarnessOptions; copied.
        //   sink    — receives every event; may be empty (then nothing is reported).
        //   throws  — std::invalid_argument when options.name is not a valid key or a tool
        //             server / tool has no name; std::system_error when the worker thread cannot
        //             start; std::bad_alloc.
        HarnessConversation(HarnessOptions options, EventSink sink);

        // Ends the process and the worker. Blocks like Shutdown.
        ~HarnessConversation();

        HarnessConversation(const HarnessConversation&) = delete;
        HarnessConversation& operator=(const HarnessConversation&) = delete;

        // Start the process now, so that the first Send finds it ready (a start takes about one
        // second). Does nothing when a process is up or starting. Call it when the user shows
        // intent (opens the surface that hosts the conversation), not at app start: an idle
        // process holds memory.
        void Prewarm();

        // Send a user message.
        //   text    — the message; an empty or whitespace-only text is ignored (returns 0).
        //   returns — the turn's id (1, 2, ...), carried by every event of the turn.
        // NEWEST WINS: a Send while a turn runs interrupts that turn (its TurnCompleted has
        // `interrupted` true), and a Send that replaces a message which did not start yet drops
        // that message (it gets a TurnCompleted with `interrupted` true and no text). So only
        // the newest message is ever answered in full — the behavior a search box needs.
        uint64_t Send(std::wstring text);

        // Stop the running turn (the model's request is cancelled; the context keeps what was
        // said so far). Does nothing when no turn runs.
        void Interrupt();

        // Change how the model thinks, from the NEXT message on: thinking off, or adaptive
        // thinking, at an effort level. Returns at once.
        //   thinking — Disabled or Adaptive. CliDefault (Claude Code's own default) can only be
        //              reached by a new process: the running one is restarted before the next
        //              message.
        //   effort   — "low" | "medium" | "high" | "xhigh" | "max"; an empty or unknown word keeps
        //              the current one.
        // HOW it is applied (the thinking policy, AgentHarnessCore.h): before the next message the
        // worker changes the RUNNING process with Claude Code's control requests — the thinking,
        // the request body (between_tools, for "off" on a model that refuses "disabled") and the
        // effort —, so the conversation keeps its process and its whole context; get_settings then
        // reports the effort in effect (Inventory().effort, and Inventory().thinking names the
        // way thinking runs), and hooks.log has one "[harness] <name>: thinking ..." line. A
        // process that cannot take the change live — Claude Code refused a request, or the change
        // involves CliDefault — is restarted with the new values, and the conversation continues
        // from its digest. A turn that runs now keeps its settings; a process that is not up yet
        // simply starts with the new values.
        // Never throws.
        void SetThinking(HarnessThinking thinking, std::wstring effort) noexcept;

        // Throw the conversation away: end the process, clear the record (and delete its file),
        // and raise the epoch. A turn in flight ends as interrupted. If a process was up, a new
        // one is started at once, so the next Send is fast.
        void Reset();

        // PARK the conversation: end the process `delayMs` from now, unless a Prewarm, a Send or
        // a Reset comes first (each cancels a pending park). The conversation is NOT thrown away:
        // the record stays, and the next process starts with a digest of it. Call it when nobody
        // looks at the surface that hosts the conversation any more — an idle claude.exe holds a
        // few hundred megabytes for as long as it lives.
        //   delayMs — 0 or less = at once (after the turn that runs now, if one does).
        // ⚠ What a park costs: the model's own context. The new process knows only the digest
        //   (HarnessOptions::digestTurns / digestChars), not the whole conversation, so a
        //   follow-up that refers to something old may miss it. Choose a delay long enough that
        //   a user who comes back within minutes still finds the same process.
        // A second call replaces the delay of the first. Does nothing when no process is up when
        // the delay ends.
        void ParkAfter(int64_t delayMs);

        // End the process and the worker for good; every later call does nothing. Blocks until
        // both are gone. Never throws.
        void Shutdown() noexcept;

        HarnessState State() const;
        uint64_t Epoch() const;

        // A copy of the record as it is now (the UI's history after a restart).
        //   throws — std::bad_alloc.
        HarnessRecord Record() const;

        // What the newest process reported about itself: the permission mode in effect (known
        // as soon as the process is Ready, before any model request) and, after the first turn of
        // that process, every tool, tool server, slash command, skill, agent type and plugin the
        // model in it has. Use it to CHECK a conversation ("which mode does it run in", "what can
        // the model reach") instead of trusting the options it was created with.
        //   returns — a copy; `known` is false when no process answered yet (and after a failed
        //             start), `complete` is false until the first turn.
        //   throws  — std::bad_alloc.
        // A park or a reset keeps the last report until the next process answers.
        HarnessInventory Inventory() const;

        // The command line the SDK builds for these options, one argument per entry, without the
        // program name. For tests and diagnostics: it proves which flags a conversation runs
        // with, and starts nothing.
        //   returns — the arguments of the NEXT start. They ask for bypass permissions unless an
        //             earlier start of this conversation was refused with it (then for the mode
        //             that start ended with).
        //   throws  — std::invalid_argument (an invalid tool schema); std::bad_alloc.
        std::vector<std::wstring> DescribeCommandLine() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    // The display word of a state ("starting", "ready", ...), for logs and status lines.
    const wchar_t* HarnessStateName(HarnessState state) noexcept;
}
