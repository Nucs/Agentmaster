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
//     form is Agentmaster's own record (HarnessRecord), in the profile.
//
// This header needs neither the SDK nor nlohmann/json: only AgentHarness.cpp includes them.
// Plain C++, no WinRT (the .cpp is <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AgentHarnessCore.h" // HarnessRecord / HarnessUsage
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
    // name, and `--allowedTools mcp__<name>` is what lets the model call them without a prompt.
    struct HarnessToolServer
    {
        std::wstring name; // letters, digits, '_' and '-'
        std::wstring version{ L"1.0.0" };
        std::vector<HarnessTool> tools;
    };

    // ===== options ==========================================================================

    // How the model may think before it answers.
    enum class HarnessThinking
    {
        CliDefault, // pass nothing: Claude Code decides (adaptive on current models)
        Disabled, // --thinking disabled: no thinking blocks at all (the fastest; measured 0 thinking tokens on Sonnet 5.5)
        Adaptive, // --thinking adaptive
    };

    // Everything that defines a conversation. Read once, by the constructor.
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
        Ready, // the process answered: `model`, `cliVersion`
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
        int64_t preTokens{ 0 };
        int64_t postTokens{ 0 };
        std::wstring model;
        std::wstring cliVersion;
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

        // The command line the SDK builds for these options, one argument per entry, without the
        // program name. For tests and diagnostics: it proves which flags a conversation runs
        // with, and starts nothing.
        //   throws — std::invalid_argument (an invalid tool schema); std::bad_alloc.
        std::vector<std::wstring> DescribeCommandLine() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    // The display word of a state ("starting", "ready", ...), for logs and status lines.
    const wchar_t* HarnessStateName(HarnessState state) noexcept;
}
