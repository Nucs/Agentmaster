// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — WHEN the side panel's AI view changes its list of sessions (SIDE_PANEL.md §2h).
//
// The user's rule of 2026-11-05: "the ai assistant should update the list only at the end when the result is final".
// A request of the AI search runs for seconds and goes through steps that each carry a list: every search, grep or read
// returns candidates (SessionSearchEventKind::Candidates), and the model's show picks the sessions to list (Shown) —
// possibly more than once, and while the model still writes its answer after it. The view used to draw each of those
// as it came (the candidates at once, the pick a second later), so the list changed under the user several times per
// request. Now the list on screen changes at two moments only:
//   - when a request ENDS (Answer — an interrupted request too — or Failed): the conversation's list is final then, the
//     one its last show picked (this request's, or an earlier request's when this one showed nothing). It is taken only
//     when it differs from the list on screen, so an answer in words leaves the cards, and their scroll, as they are;
//   - when the conversation is RESET (⊘ anywhere): everything shown is thrown away at once.
// Everything a running request sends in between — its candidates, its shows — never reaches the list. The view still
// says that something runs (its loading icon, the strip's plan and step, a count of the newest search's candidates) and
// keeps the last answer's list on screen, dimmed, until the request ends.
//
// This header is that rule as two pure functions, so the harness can pin it (TestSidePanelAiList, tests_sidepanel.cpp)
// and the view (TerminalPage::_OnSidePanelAiEvent) only applies it. The Sessions page's AI view has its own rule and
// still previews a search's candidates (SESSIONS.md §9.5): this header is the side panel's alone.
//
// Plain C++, header-only (the engine idiom of StatusIconMotion.h / PointerCardPlacement.h): no WinRT, no XAML, so the
// m5 harness compiles it without the app.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "SessionSearchAgent.h" // SessionSearchEventKind / SessionSearchSnapshot — the agent's events, read here

namespace Agentmaster
{
    // What one agent event does to the list the side panel's AI view shows.
    enum class SidePanelAiListStep
    {
        // Leave the list on screen as it is: every event of a running request (its candidates and shows included) and every
        // event that changes only the strip (a state change, a request accepted, a search step, a compaction, a plan line).
        Keep,
        // The request ended: take the conversation's list (the event's snapshot) when it differs from the list on screen
        // (SidePanelAiListDiffers), else keep the cards untouched.
        AdoptIfChanged,
        // The conversation was thrown away: take its (empty) state at once, whatever is on screen.
        AdoptNow,
    };

    // The step for one event.
    //   kind    - the event's kind.
    //   busy    - the event's snapshot.busy: a request still runs (or waits for the process to start) after this event.
    //   returns - the step (see SidePanelAiListStep).
    //   throws  - nothing.
    // A Shown event is held while its request runs: the model may still show another list, and the answer that ends the
    // request is the moment the list is final. One that arrives when NOTHING runs (not seen in practice; a show is a tool
    // call inside a request) has no end left to wait for, so it is final at once. A Candidates event never changes the
    // list, running or not: a search's rows were read by nobody yet.
    [[nodiscard]] inline SidePanelAiListStep SidePanelAiListStepFor(SessionSearchEventKind kind, bool busy) noexcept
    {
        switch (kind)
        {
        case SessionSearchEventKind::Reset:
            return SidePanelAiListStep::AdoptNow;
        case SessionSearchEventKind::Answer:
        case SessionSearchEventKind::Failed:
            return SidePanelAiListStep::AdoptIfChanged;
        case SessionSearchEventKind::Shown:
            return busy ? SidePanelAiListStep::Keep : SidePanelAiListStep::AdoptIfChanged;
        case SessionSearchEventKind::StateChanged:
        case SessionSearchEventKind::Asked:
        case SessionSearchEventKind::Searching:
        case SessionSearchEventKind::Candidates:
        case SessionSearchEventKind::Compacting:
        case SessionSearchEventKind::Narration:
        default:
            // An event kind added to the agent later falls here too: it changes nothing on screen until this rule says so.
            return SidePanelAiListStep::Keep;
        }
    }

    // Whether the conversation's list differs from the list on screen — the AdoptIfChanged test.
    //   hasShownOnScreen - the view's "the model showed a list" flag (an EMPTY list then reads "nothing fits").
    //   idsOnScreen      - the view's list, in the model's order (empty while hasShownOnScreen is false).
    //   reasonsOnScreen  - the view's whys by session id; a session without one is absent (the view never stores "").
    //   conversation     - the event's snapshot: hasShown, shownIds and the index-aligned shownReasons ("" = none given;
    //                      a shorter vector reads as "" for the rest).
    //   returns          - true when taking the conversation's list would change what the cards show: the flag, a session,
    //                      the order, or one why differs; false when they are the same, or neither has a list.
    //   throws           - nothing (it only reads its arguments).
    // The whys count because a show can list the same sessions again with new evidence. A list that names a session twice
    // (never sent by the toolset) keeps one why per id on screen, so its comparison can read "differs" and cost one
    // rebuild of the same cards — never a missed change.
    [[nodiscard]] inline bool SidePanelAiListDiffers(bool hasShownOnScreen,
                                                     const std::vector<std::wstring>& idsOnScreen,
                                                     const std::unordered_map<std::wstring, std::wstring>& reasonsOnScreen,
                                                     const SessionSearchSnapshot& conversation) noexcept
    {
        if (conversation.hasShown != hasShownOnScreen)
        {
            return true;
        }
        if (!conversation.hasShown)
        {
            return false; // no list on either side: nothing to take
        }
        if (conversation.shownIds != idsOnScreen)
        {
            return true;
        }
        for (size_t i = 0; i < idsOnScreen.size(); ++i)
        {
            const std::wstring* const want = i < conversation.shownReasons.size() ? &conversation.shownReasons[i] : nullptr;
            const auto have = reasonsOnScreen.find(idsOnScreen[i]);
            const bool wantNone = want == nullptr || want->empty();
            const bool haveNone = have == reasonsOnScreen.end() || have->second.empty();
            if (wantNone != haveNone || (!wantNone && *want != have->second))
            {
                return true;
            }
        }
        return false;
    }
}
