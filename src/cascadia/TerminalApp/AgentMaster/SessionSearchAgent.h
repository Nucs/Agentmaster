// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionSearchAgent: the Sessions browser's AI search (SESSIONS.md §9).
//
// The user writes what they look for ("the session where I fixed the tab drag crash"); a hosted
// Sonnet conversation understands it, searches with the page's own search (SessionSearchTools.h)
// and tells the page which sessions to show. This class is that conversation plus what a page
// needs to show it:
//
//   - ONE conversation for the whole process. Every window's Sessions page is a view of it (the
//     engine's rule: one fleet, many lenses), so two windows never run two model conversations.
//   - It is AGENTIC: the conversation keeps its context, so a follow-up ("only the open ones")
//     refers to what was shown. Claude Code compacts the context at 500,000 tokens by itself.
//   - RESET throws the conversation away (the page's crossed-circle button): the process ends, a
//     fresh one starts, the list goes back to normal.
//   - Nothing is written under ~/.claude (--no-session-persistence), so the search conversation
//     never shows up among the sessions it searches. What survives a restart is Agentmaster's own
//     record of it (the harness record) and the last shown list, both in <profile>\harness\.
//
// The model and the tools are fixed here on purpose — the feature is "the fastest useful
// answer", not a configurable chat: Sonnet 5.5, no built-in tools, two in-process tool servers.
// The one thing the user picks is the THINKING LEVEL (the strip's "thinking off" toggle; the
// user's request of 2026-10-14): off (the default, and the fastest) or adaptive thinking at one of
// Claude Code's effort levels. A change applies from the next request, to the running process
// (HarnessConversation::SetThinking), so the conversation keeps its context.
//
// Threads: every method may be called from any thread and returns at once. Listeners are called
// on the conversation's threads (never a UI thread): a page marshals to its own dispatcher.
// Plain C++, no WinRT (the .cpp is <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AgentHarness.h" // HarnessState / HarnessUsage
#include "SessionSearchTools.h" // SessionCatalog
#include "TranscriptStore.h" // SessionIndexEntry

namespace Agentmaster
{
    // What the agent needs from its host. Every member is optional for a test.
    struct SessionSearchAgentOptions
    {
        // The sessions that have a tab open now, as (id, tab title): the `open` filter and the
        // live titles of a search. Called on the search thread; must be thread-safe.
        std::function<std::vector<std::pair<std::wstring, std::wstring>>()> openSessions;

        // The native claude.exe, or empty when there is none. Called on the conversation's
        // worker thread at every process start. Empty function = the path given to
        // SetClaudeExePath (the app's way: the path is owned by the UI thread).
        std::function<std::wstring()> claudeExe;

        // false: keep nothing in the profile (tests). true: the conversation's record and the
        // last shown list survive a restart.
        bool persist{ true };

        std::wstring name{ L"sessions-search" }; // the conversation key (file names, the child's AM_HARNESS mark)
        std::wstring model{ L"claude-sonnet-5-5" };
        // The thinking level the agent STARTS with (SetThinkingLevel changes it). Off = no
        // thinking at effort low: the fastest answer, and the agent's only setting before the
        // levels existed. The app seeds the persisted level right after the agent exists
        // (AppSettings::sessionsAiThinkingLevel), before its first process starts.
        HarnessThinkingLevel thinkingLevel{ HarnessThinkingLevel::Off };
        int64_t autoCompactTokens{ 500000 };
    };

    // What an event reports.
    enum class SessionSearchEventKind
    {
        StateChanged, // the conversation's state changed (starting, ready, ...)
        Asked, // a request was accepted: `text`
        Searching, // the model runs a search: `text` describes it
        Candidates, // that search returned these sessions (a preview; the model may still narrow them)
        Shown, // the model put these sessions on the screen: `sessionIds`, `entries`, `text` = the caption
        Answer, // the request is answered: `text` (also for an interrupted request, with its partial text)
        Failed, // the request or the start failed: `text` says why
        Compacting, // Claude Code compacts the conversation (the request pauses for seconds)
        Reset, // the conversation was thrown away: clear everything shown
    };

    // The conversation as a page shows it. One consistent copy (Snapshot()).
    struct SessionSearchSnapshot
    {
        uint64_t epoch{ 1 }; // counts resets; a page drops what belongs to an older epoch
        HarnessState state{ HarnessState::Stopped };
        bool busy{ false }; // a request is being answered (or waits for the process to start)
        uint64_t turns{ 0 }; // requests made in this conversation
        std::wstring question; // the newest request
        std::wstring activity; // what happens now ("Searching ..."); empty at rest
        // True once the model called show in this conversation. An EMPTY shownIds then means
        // "nothing fits" (show an empty list), not "no result yet" (leave the list alone).
        bool hasShown{ false };
        std::vector<std::wstring> shownIds; // full session ids, in the model's order
        std::wstring note; // the model's caption for the shown list
        std::wstring answer; // the newest reply, or the failure text
        bool answerIsError{ false };
        std::wstring model; // the model that answers (known after the first request)
        int compactions{ 0 };
        HarnessUsage usage;
        // The thinking level the next request runs with (SetThinkingLevel). A page shows it as
        // "thinking <word>" and rotates it on a click.
        HarnessThinkingLevel thinkingLevel{ HarnessThinkingLevel::Off };
        // The model's effort words as its process reported them (HarnessInventory::
        // supportedEfforts); empty until a process answered. The page's rotation skips a level
        // that is not in a non-empty list (NextHarnessThinkingLevel).
        std::vector<std::wstring> supportedEfforts;
    };

    // One event, with the state after it.
    struct SessionSearchEvent
    {
        SessionSearchEventKind kind{ SessionSearchEventKind::StateChanged };
        uint64_t turn{ 0 }; // the request it belongs to; 0 = none
        std::wstring text;
        std::vector<std::wstring> sessionIds; // Candidates / Shown: full ids, in order
        // Candidates / Shown: the index entry of each id the agent knows, in the same order (an id
        // without an entry is left out). A page uses them for rows that are outside its own time
        // window. Their stats.pathsAccessed is EMPTY: only the row facts are carried.
        std::vector<SessionIndexEntry> entries;
        SessionSearchSnapshot snapshot;
    };

    class SessionSearchAgent
    {
    public:
        using Listener = std::function<void(const SessionSearchEvent&)>;

        // Create the agent. Starts no process and reads no session; with options.persist it
        // loads the stored record and the last shown list.
        //   throws — std::invalid_argument for an invalid options.name; std::system_error when
        //            the conversation's worker thread cannot start; std::bad_alloc.
        explicit SessionSearchAgent(SessionSearchAgentOptions options = {});

        // Ends the process. Blocks like Shutdown.
        ~SessionSearchAgent();

        SessionSearchAgent(const SessionSearchAgent&) = delete;
        SessionSearchAgent& operator=(const SessionSearchAgent&) = delete;

        // Register a listener for every event. The registry-token pattern of the engine (Rule
        // #10): keep the token and pass it to RemoveListener when the page goes away.
        //   returns — a non-zero token; 0 for an empty listener (nothing was registered).
        //   throws  — std::bad_alloc.
        uint64_t AddListener(Listener listener);

        // Detach a listener. A stale or 0 token is a no-op. After it returns, the listener is not
        // called again — except by a call that was already running on another thread.
        void RemoveListener(uint64_t token) noexcept;

        // The native claude.exe to run (the engine's claudeExePath). Call it from the UI thread
        // before Prewarm / Ask, and again when the path changes; it is read at each process start.
        //   throws — std::bad_alloc.
        void SetClaudeExePath(std::wstring path);

        // Get ready for a request: start the process and read the session index, both in the
        // background. Call it when the user turns the AI search on. Does nothing when both are
        // already up.
        //   readIndex — false: start the process only. For a caller that is reading the same
        //               sidecar files itself right now (the Sessions page's own gather): two
        //               readers of a transcript that has no sidecar yet both parse it, so the
        //               page calls Prewarm(false) at once and Prewarm(true) when its gather
        //               is done. A request (Ask) reads the index in any case.
        void Prewarm(bool readIndex = true);

        // Ask. The newest request wins: a request that is still running is interrupted.
        //   text    — what the user wrote; an empty text is ignored.
        //   returns — the request's turn id (1, 2, ...); 0 when it was ignored.
        //   throws  — std::bad_alloc.
        uint64_t Ask(std::wstring text);

        // Stop the running request. The conversation keeps what was said so far.
        void Interrupt();

        // Set how much the model thinks, from the next request on (a request that runs now keeps
        // its level). The running process is changed live, so the conversation keeps its context
        // (HarnessConversation::SetThinking); a process that is not up starts with the level.
        //   level — Off, or adaptive thinking at one of the effort levels.
        // Listeners get a StateChanged event whose snapshot carries the new level, so every
        // window's strip shows it. Setting the level the agent already has does nothing (no event,
        // no log line). The caller persists the level (AppSettings::sessionsAiThinkingLevel): the
        // agent keeps it in memory only.
        // Never throws.
        void SetThinkingLevel(HarnessThinkingLevel level) noexcept;

        // Throw the conversation away: the context, the record, the shown list. A fresh process
        // is started when one was up. Listeners get a Reset event with the new epoch.
        void Reset();

        // Free the in-memory session index (tens of megabytes for a busy month) and stop a
        // background build of it. Call it when no page shows the AI search any more; the next
        // Prewarm / Ask reads it again. A build that was stopped continues from where it was:
        // what it indexed is on disk.
        void ReleaseCatalog();

        // End the idle claude.exe `delayMs` from now unless a Prewarm, an Ask or a Reset comes
        // first (HarnessConversation::ParkAfter). Call it together with ReleaseCatalog when no
        // page shows the AI search any more. The conversation is kept: the next process starts
        // with a digest of it, so the delay should be minutes — a user who comes back sooner
        // finds the same process and the model's whole context.
        void ParkProcessAfter(int64_t delayMs);

        // End the process and the worker for good. Blocks until both are gone. Never throws.
        void Shutdown() noexcept;

        // The conversation as it is now.
        //   throws — std::bad_alloc.
        SessionSearchSnapshot Snapshot() const;

        // The index entries of `ids` that the agent's current index holds, in the order asked
        // (pathsAccessed empty, like an event's entries). For a page that opens after a Shown
        // event passed.
        //   throws — std::bad_alloc.
        std::vector<SessionIndexEntry> EntriesFor(const std::vector<std::wstring>& ids) const;

        // ----- diagnostics: what a review of the agent reads (the live test prints all three) -----

        // The conversation's record: every request, every tool call with the arguments the model
        // sent, every tool result (a preview, cut to kHarnessMaxToolResultChars) and every reply,
        // each with the time the harness saw it. It is HOW a request was answered; the events
        // only say what to show.
        //   throws — std::bad_alloc.
        HarnessRecord Record() const;

        // What the hosted process reported about itself: the permission mode in effect, and
        // (after the first request of a process) every tool, tool server, slash command, skill,
        // agent type and plugin its model has (HarnessConversation::Inventory).
        //   throws — std::bad_alloc.
        HarnessInventory Inventory() const;

        // The arguments the next process start uses, without the program name
        // (HarnessConversation::DescribeCommandLine). Starts nothing.
        //   throws — std::bad_alloc.
        std::vector<std::wstring> DescribeCommandLine() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    // The process's one agent, created on first use and never destroyed (like the engine: the
    // app ends with TerminateProcess, and the child process is ended by its job object). It
    // takes the open sessions from the shared engine's registry and its start thinking level
    // from settings.json (AppSettings::sessionsAiThinkingLevel, read once, here); the claude.exe
    // path is pushed by the pages (SetClaudeExePath).
    //   throws — what the constructor throws, on the first call only.
    // ⚠ Do not call it from the standalone test harness or the CLI: the first call touches
    //   SharedEngine(), which starts the hooks bridge and the observers. Tests construct their
    //   own SessionSearchAgent.
    SessionSearchAgent& SharedSessionSearchAgent();
}
