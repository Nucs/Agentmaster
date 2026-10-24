// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — WindowTabTools: what the AI search's WINDOW AND TAB TOOLS (SESSIONS.md §9.14) exchange with the
// app's windows.
//
// The two search conversations (the Sessions page's and the Telegram Search topic's) are process-wide and run their
// tool calls on the SDK's reader thread. The tabs live in the windows, and only a window's own UI thread may read its
// tab strip or open, fork and park its tabs. So every tab tool call becomes a WindowToolRequest that the engine hands
// to one window, or to every window (Engine.h CollectWindowCensus / RunWindowTool / RunWindowToolOnHost), and the
// window answers with a WindowToolOutcome from its UI thread (TerminalPage.AgentSessionsAi.cpp _RunAgentWindowTool).
//
// Header-only plain C++ (no WinRT, no Win32), on purpose: Engine.h, SessionSearchTools.h and TerminalPage.h include
// it, and the standalone test harness builds the tools against it with no window at all (tests_tabtools.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Agentmaster
{
    /// <summary>
    /// One tab of a window, as the census reports it to the model.
    /// </summary>
    /// <remarks>
    /// Two halves, filled in two places: the strip half (index, title, session, selected) only the window's UI thread
    /// can read, and it is all a window reports; the session half is read from the shared registry by the census's
    /// caller OFF the UI thread (SessionSearchAgent's engine tab tools), so a census of hundreds of tabs costs the UI
    /// thread one pass over its strip and no registry copies. A shell (or Settings) tab has no session: its session
    /// half stays empty / 0 / false.
    /// </remarks>
    struct WindowTabInfo
    {
        uint32_t index{ 0 }; // 0-based position in the window's tab strip (the model reads index + 1 as "pos")
        std::wstring title; // the title the strip shows (the pinned session title for a managed tab)
        std::wstring sessionId; // the managed session the tab hosts (Claude or Codex, the registry's id); "" = a shell / settings tab
        bool selected{ false }; // the window's selected tab; false for every tab while the window's Agent Manager page is on screen

        // ---- the session half (registry facts; empty / 0 / false for a shell tab) ----
        bool codex{ false }; // a managed Codex session (a fork of it is `codex fork`, it takes no --model)
        // The Triage Board's state word: "running", "waiting for you", "needs approval", "error", "idle" or "done".
        // A deactivate of a "running" or "needs approval" tab cuts its turn off, which is why the deactivate tool
        // reads it before it acts.
        std::wstring state;
        bool started{ false }; // its agent process runs; false = a dormant tab (window-restored or deactivated) that starts when it is entered
        bool external{ false }; // a claude the user started by hand in a shell tab (adopted): its terminal is the user's own shell, never deactivated
        std::wstring dir; // the launch working directory (where its claude was started)
        std::wstring worksIn; // the inferred working directory (where its tool calls concentrate) when it differs from `dir`; "" = the same
        std::wstring branch; // the git branch it runs on; "" = unknown
        int64_t lastTurnUnixMs{ 0 }; // the most recent turn START (the user's newest prompt); 0 = no turn yet
        int64_t lastActivityUnixMs{ 0 }; // the most recent activity of any kind (a turn's end, a tool call, a hook); 0 = none recorded

        // ---- the overview's columns (SESSIONS.md §9.16; filled with the session half, empty / 0 / false for a shell) ----
        // The MOST RECENT key (SessionModels.h MostRecentSortKeyMs): the newer of its last turn start and end, else the
        // decay anchor (a new session's creation instant). The overview's `age` and the order of its folder groups: the
        // order the Manager's tree and board and the side panel show under MOST RECENT. 0 = nothing known.
        int64_t mostRecentUnixMs{ 0 };
        // Messages the user sent in its conversation (the transcript index's real prompts, which a fork shares with its
        // source). -1 = not known: the session index is not read yet, or the session has no transcript of its own yet.
        int32_t turns{ -1 };
        int64_t contextTokens{ 0 }; // its context size in tokens (the newest reply's usage, else the index's); 0 = not known yet
        std::wstring model; // the model its newest reply came from, else the launch model; "" = not known
        std::wstring effort; // its effort level; "" = the default
        uint32_t queued{ 0 }; // prompts waiting in its Prompt Queue (Pending)
        // Its Prompt Queue Runner (SessionRunnerWord): "auto", "paused", "held" (paused by an interrupt until the user's
        // next message), each with " (Pause all on)" while the global pause holds every session.
        std::wstring runner;
        bool unread{ false }; // it finished or asked something the user has not looked at yet (SessionLooksUnread)
        bool draft{ false }; // an unsent draft sits in its input box
        bool pinned{ false }; // pinned: it never decays to Idle
        bool compacting{ false }; // a compaction runs now
        std::wstring errorMessage; // in Error: what Claude Code said, cut; "" otherwise
    };

    /// <summary>
    /// What the AI search's `status` tool reports about one session (SESSIONS.md §9.16), and — for a search row — the
    /// brief part of it: Agentmaster's live facts from its registry, the counts of the transcript index, and the texts a
    /// tab row leaves out (the last reply, Claude's recap, the unsent draft, why the next queued prompt waits).
    /// </summary>
    /// <remarks>
    /// Filled by SessionSearchToolset::Callbacks::TabTools::facts: the app builds it with SessionLiveFactsOf from a
    /// registry record (open or closed), or from the transcript index for a session Agentmaster never ran; a test fills
    /// it directly. A BRIEF answer (the callback's `full` false, for search rows) fills only sessionId, live, started,
    /// external, codex, state and mostRecentUnixMs, and only for a session open in a tab; every other field stays empty.
    /// Plain data, so the standalone test harness builds the tools with no registry at all.
    /// </remarks>
    struct SessionLiveFacts
    {
        std::wstring sessionId; // the whole id that was asked
        bool known{ false }; // a registry record or a transcript of it exists; false = no such session (the rest stays empty)
        bool managed{ false }; // Agentmaster's registry has a record of it (open or closed); false = only its transcript is known
        bool live{ false }; // it has a tab open in some window
        bool started{ false }; // its agent process runs; false while live = a dormant tab (it starts when the tab is entered)
        bool external{ false }; // adopted: a claude the user started by hand in a shell tab
        bool codex{ false }; // a managed Codex session
        std::wstring state; // the Triage Board's word (SessionStateWord); "" when not managed
        int64_t stateSinceUnixMs{ 0 }; // when its status, its tab or its start last changed THIS run; 0 = not known (e.g. before a restart)
        // The MOST RECENT key (MostRecentSortKeyMs) of a managed session; for a session Agentmaster never ran, its
        // transcript's newest message. The status tool's `age`. 0 = nothing known.
        int64_t mostRecentUnixMs{ 0 };
        int64_t createdUnixMs{ 0 }; // when it was created; 0 = not known
        std::wstring title; // its title (the tab's pinned title for a managed session)
        std::wstring dir; // the launch working directory
        std::wstring worksIn; // the inferred working directory when it differs from `dir`; "" = the same
        std::wstring branch; // the git branch; "" = unknown
        int32_t turns{ -1 }; // messages the user sent (WindowTabInfo::turns); -1 = not known
        int64_t contextTokens{ 0 }; // context size in tokens; 0 = not known
        std::wstring model; // the model its newest reply came from, else the launch model
        std::wstring effort; // its effort level; "" = the default
        std::wstring heartbeat; // Claude's own heartbeat status (busy, idle, waiting, shell); "" = no running claude reports one
        std::wstring waitingFor; // what the heartbeat says it waits for ("input needed"), when it waits
        bool compacting{ false }; // a compaction runs now
        bool unread{ false }; // SessionLooksUnread
        bool pinned{ false }; // pinned: it never decays to Idle
        bool favorite{ false }; // starred
        bool askedQuestion{ false }; // its last turn ended with a question to the user
        std::vector<std::wstring> tags; // its bookmark tags
        std::wstring errorMessage; // in Error: what Claude Code said, cut; "" otherwise
        int32_t errorStatus{ 0 }; // the API error's HTTP status; 0 = none or not an API error
        std::wstring lastReply; // Claude's newest reply, cut
        std::wstring recap; // Claude Code's own recap of the session (its away summary), cut
        std::wstring draft; // the unsent draft in its input box, cut; "" = none
        int64_t draftUnixMs{ 0 }; // when the draft was last seen
        uint32_t queued{ 0 }; // prompts waiting in its Prompt Queue (Pending)
        std::wstring nextPrompt; // the first line of the next one, cut; "" = nothing queued
        std::wstring runner; // SessionRunnerWord; "" when not managed
        // Why the next queued prompt has not gone out (DescribeQueueWait: the runner's own decision in plain words), or
        // that it goes out now; "" = nothing queued.
        std::wstring nextWait;
        std::wstring forkedFrom; // the whole id of the session it was forked from; "" = not a fork (or not known)
        std::wstring continuedFrom; // the whole id of the session it continues after a /clear or a /handover; ""
        std::wstring continuedVia; // how: "clear" or "handover"; "" with continuedFrom
    };

    /// <summary>
    /// One window and its tabs, in strip order — what a Census request answers.
    /// </summary>
    struct WindowCensus
    {
        std::wstring windowId; // the window's record id ("{guid}", TerminalPage::_windowId): what a request names
        std::wstring title; // the window's title (the selected tab's title, or "Agent Manager")
        bool managerPageVisible{ false }; // the window shows its Agent Manager page: no tab is selected
        // The window the user activated last (Engine NoteWindowActivated). Set by the engine when it gathers the
        // census, never by the window: a window cannot know whether another one was activated after it.
        bool lastActive{ false };
        std::vector<WindowTabInfo> tabs; // strip order
    };

    /// <summary>
    /// What the AI search asks ONE window (Census, Resume, Fork) or the window that hosts a session (Deactivate) to do.
    /// </summary>
    /// <remarks>
    /// Resume and Fork run the very seams the user's own clicks run: the tab menu's "Fork session"
    /// (_ForkManagedSessionById) for a session the registry knows, the Sessions page's "Fork here" / "Resume here"
    /// (_ForkSessionFromDisk / _ResumeSessionFromDisk) for one it does not — so a fork carries the source's unsent
    /// draft, a resume is transcript-gated (Rule #6), and both pin the title and the folder color exactly as a click
    /// would. Deactivate runs the tab menu's "Deactivate ▸ Deactivate Tab" (_DeactivateTabsFromMenu, this-tab scope),
    /// focused-tab rule included.
    /// </remarks>
    struct WindowToolRequest
    {
        enum class Kind
        {
            Census, // report this window's strip (WindowToolOutcome::census)
            Resume, // open `sessionId` (a closed session, or one the registry never knew) as a tab of this window
            Fork, // open a FORK of `sessionId` (a NEW conversation; the source's transcript is untouched) as a tab of this window
            Deactivate, // park `sessionId`'s tab, if this window hosts it; every other window answers handled == false
        };
        Kind kind{ Kind::Census };
        std::wstring sessionId; // Resume / Fork / Deactivate: the whole session id
        // Resume / Fork: the session's working directory and title. Required for a session the registry does not know
        // (the launch needs the folder; "" title = the window derives one); a known session's own record wins.
        std::wstring dir;
        std::wstring title;
        std::wstring model; // Fork: a per-launch `--model <id>` ("" = the settings model); a Codex fork ignores it
        // Resume / Fork placement, first match wins:
        //   afterSessionId non-empty — right after the tab of that session (it must be in THIS window);
        //   afterIndex >= 0          — right after the tab at that 0-based index (past the end = the end);
        //   neither                  — a fork right after its source's tab when the source is in this window (the tab
        //                              menu's "Fork session" placement), anything else at the END of the strip.
        std::wstring afterSessionId;
        int32_t afterIndex{ -1 };
        // Resume / Fork: select the new tab and bring the window to the front. False (the default) opens a BACKGROUND
        // tab and changes nothing on the screen — the right thing for a request from a phone, and for a request
        // from the Sessions page that is still talking to the user.
        bool focus{ false };
    };

    /// <summary>
    /// A window's answer to a WindowToolRequest, sent from its UI thread through the engine's reply callback.
    /// </summary>
    struct WindowToolOutcome
    {
        // This window answered a Census, took a Resume / Fork, or hosts the session of a Deactivate. False = not this
        // window's business (a Deactivate's other windows): the engine keeps waiting for the host.
        bool handled{ false };
        bool ok{ false }; // it did what was asked
        std::wstring message; // one sentence written FOR THE MODEL: what happened, or why not and what to do
        // Resume / Fork: the session whose tab is open now — a fork's NEW id; a resume whose transcript was gone
        // started a FRESH session with a new id (Rule #6), and that new id is here.
        std::wstring sessionId;
        int32_t index{ -1 }; // Resume / Fork: the 0-based strip position of that tab; -1 = not known
        WindowCensus census; // Census: this window
    };
}
