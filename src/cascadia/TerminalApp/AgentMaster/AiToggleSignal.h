// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster (SIDE_PANEL.md §2h "The AI toggle's signals", SESSIONS.md §9.1): what an AI search toggle — the side panel's
// "AI" in its filter box, the Sessions page's "AI" in its search box — tells about the process's ONE search conversation
// while its OWN AI view is off (the user's request of 2026-11-19: "when you deselect the 'AI' button and you now see the
// regular cards; while the AI session is running, I want the AI to have two sets of animations, one while running and one
// when it's done", and their picks from live mock-ups the same day):
//   - RUNNING: a request runs. The toggle's letters turn the working blue and a light with a fading tail orbits its
//     outline (AgentAiToggleSignal in AgentAmbientAnimation.h draws it).
//   - ANSWERED / FAILED: the newest request ended while NO AI view showed the conversation. The toggle's outline blinks on
//     the unread flash rings' own clock, green or red, until any AI view shows the conversation ("until you look"), a new
//     request starts, or the conversation is reset.
//   - Every toggle of every window shows it (the conversation is one, shared by every side panel and Sessions page);
//     opening ANY AI view — or having one on screen when the request ends — counts as reading the answer, everywhere.
// This header is the two pure rules behind it, so the harness pins them (TestAiToggleSignal): what the agent records when a
// request ends (AiSearchUnseenAtTurnEnd, SessionSearchAgent.cpp), and what one toggle shows (DecideAiToggleSignal, both
// surfaces). Plain C++, no WinRT, no allocation.
#pragma once

#include <cstdint>

namespace Agentmaster
{
    /// <summary>
    /// The outcome of the conversation's newest request when it ended while NO AI view showed the conversation — the answer
    /// "waiting for you to look" that every AI toggle signals until a view shows it. Carried in SessionSearchSnapshot::unseen.
    /// </summary>
    enum class AiSearchUnseen : uint8_t
    {
        None, // nothing waits: no request ended unseen, or a view showed it since, or a new request or a reset came after it
        Answered, // the newest request was answered while every AI view was off or off screen
        Failed, // the newest request failed (an API error, a process that could not start) while every AI view was off or off screen
    };

    /// <summary>
    /// What the agent records when the conversation's NEWEST request ends (SessionSearchAgent, its TurnCompleted): an
    /// outcome waits for the user only when no AI view showed the conversation at that moment and the request was not
    /// stopped.
    /// </summary>
    /// <param name="aViewShowsIt">True when at least one surface reported, through SessionSearchAgent::SetListenerShowing,
    /// that its AI view is on and on screen at the moment the request ended: the user saw the answer arrive, so nothing waits
    /// (the user's pick, "having one open when the answer lands counts as read").</param>
    /// <param name="isError">The request ended in a failure (an API error, the process could not start, a silent turn).</param>
    /// <param name="interrupted">The request was stopped before it finished: Esc in a request box (which only works while
    /// its view shows the conversation), or a newer request that replaced it (that one runs now and owns the signal). A
    /// stopped request is never a result anyone waits for.</param>
    /// <returns>None, Answered or Failed.</returns>
    /// <remarks>
    /// ⚠ A view that is ON but OFF SCREEN (a collapsed side panel, a Sessions page behind a tab) does not count as showing:
    /// its owner reports false for it, so the answer waits for it like for every other toggle. Never throws.
    /// </remarks>
    constexpr AiSearchUnseen AiSearchUnseenAtTurnEnd(bool aViewShowsIt, bool isError, bool interrupted) noexcept
    {
        if (aViewShowsIt || interrupted)
        {
            return AiSearchUnseen::None;
        }
        return isError ? AiSearchUnseen::Failed : AiSearchUnseen::Answered;
    }

    /// <summary>The look one AI toggle wears (AgentAiToggleSignal::Show draws it).</summary>
    enum class AiToggleSignal : uint8_t
    {
        Rest, // the plain toggle: its view is on, it is off screen, or nothing runs and nothing waits
        Running, // a request runs: blue letters, a light orbiting the outline
        Answered, // the newest request was answered unseen: the outline blinks green with the unread rings
        Failed, // the newest request failed unseen: the outline blinks red with the unread rings
    };

    /// <summary>
    /// The signal ONE toggle shows, from its own surface's state and the conversation's.
    /// </summary>
    /// <param name="viewOn">The toggle's own AI view is on (the toggle is checked): its strip shows the conversation, so the
    /// toggle stays quiet — the strip's breath, sweep and loading icon say "running", its dot says "answered".</param>
    /// <param name="toggleShown">The toggle can be seen: the side panel expanded (a collapsed panel hides its header), the
    /// Sessions page on screen (not closed, not behind a tab). A toggle nobody can see animates nothing (the status icons'
    /// host rule: a running Composition animation keeps the compositor working for nobody otherwise).</param>
    /// <param name="requestRuns">A request of the conversation runs (SessionSearchSnapshot::busy).</param>
    /// <param name="unseen">The conversation's unseen outcome (SessionSearchSnapshot::unseen).</param>
    /// <returns>The signal. A running request outranks an unseen outcome (a new request clears the outcome anyway; the
    /// order only matters for a snapshot caught between the two).</returns>
    /// <remarks>Never throws.</remarks>
    constexpr AiToggleSignal DecideAiToggleSignal(bool viewOn, bool toggleShown, bool requestRuns, AiSearchUnseen unseen) noexcept
    {
        if (viewOn || !toggleShown)
        {
            return AiToggleSignal::Rest;
        }
        if (requestRuns)
        {
            return AiToggleSignal::Running;
        }
        switch (unseen)
        {
        case AiSearchUnseen::Answered:
            return AiToggleSignal::Answered;
        case AiSearchUnseen::Failed:
            return AiToggleSignal::Failed;
        case AiSearchUnseen::None:
        default:
            return AiToggleSignal::Rest;
        }
    }

    /// <summary>
    /// The toggle's UI Automation ItemStatus for a signal — what a screen reader says after "AI", and what the e2e driver
    /// reads to tell the signals apart (E2E_TESTING.md §6.3). One spelling for both surfaces.
    /// </summary>
    /// <param name="look">The signal on screen. (Not named "signal": that would hide the C runtime's global signal(), a
    /// C4459 for every /W4 unit that includes this header.)</param>
    /// <returns>"request running", "answer waiting", "failure waiting", or "" at rest (an empty status is cleared).</returns>
    /// <remarks>A static string: never null, never throws.</remarks>
    constexpr const wchar_t* AiToggleSignalStatus(AiToggleSignal look) noexcept
    {
        switch (look)
        {
        case AiToggleSignal::Running:
            return L"request running";
        case AiToggleSignal::Answered:
            return L"answer waiting";
        case AiToggleSignal::Failed:
            return L"failure waiting";
        case AiToggleSignal::Rest:
        default:
            return L"";
        }
    }
}
