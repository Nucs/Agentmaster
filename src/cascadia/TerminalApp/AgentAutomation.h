// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ONE recipe for the ACCESSIBILITY METADATA of Agentmaster's own surfaces (E2E_TESTING.md §6.3,
// 2026-10-23): AutomationIds in one scheme, an accessible NAME for an element that has none of its own, and an
// ITEM STATUS string for state that has no automation peer of its own (a card's selection, a tab's selection pill,
// an unsent draft, the unread ring). UI Automation reads all three:
//   - screen readers (Narrator, NVDA) announce the name, and the item status after it ("Settings, button";
//     "t1, button, running; unsent draft; selected"), where before they met nameless glyph buttons and silent cards;
//   - the e2e suite (e2e/amdriver) finds elements by AutomationId and reads state from ItemStatus, where on 0.8.0 it
//     had to identify the compose icons by their position, cards by their texts, and a selection by its side effects.
//
// ⚠ The rules (every surface follows them; a new element follows them too):
//   - IDS: `am.<surface>.<element>[.<key>]`, `key` = AgentAutomationKey(sessionId) (the id's first 8 characters,
//     the engine's ShortId), so a test joins an element to the engine's records and the mock's requests. Ids are
//     never localized and never renamed once shipped (tests and tools depend on them): add new ones, list each in
//     E2E_TESTING.md §6.3. An element with an x:Name keeps its x:Name as its id (XAML's default) — never override it.
//   - NAMES: only for an element that has NO name of its own — a glyph-icon button (FontIcon content), a button whose
//     content is a panel (a board card, a tree row), a glyph-string button whose glyph reads as noise (↻, ⋯). NEVER a
//     TextBox (its name is its header / placeholder, which the suite reads) and never a control whose content is a
//     word: that word IS its name, and often a VALUE the user must hear ("LOCAL", "MOST ACTIVE", "Auto").
//   - ITEM STATUS: facets joined by "; " in a fixed order, the state word first (AgentItemStatus). The words are the
//     ones the surfaces already show (AgentStateWord = the Explorer Tree's and the side panel's state labels).
//   - COST: all three are plain DependencyProperty values on the element — no automation peer is created by setting
//     them and nothing is registered anywhere (the per-element ToolTip leak of AgentTipHelpers.h cannot happen here);
//     every setter reads the current value first and writes only a change, so re-asserting on every rebuild is free.
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine. Header-only; include from
// .cpp TUs (relies on the pch projections, like the rest of TerminalApp). UI thread only (XAML elements).

#pragma once

#include <algorithm> // std::min (AgentAutomationKey)
#include <string>
#include <string_view>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h> // AutomationProperties (the pch's Automation.Peers.h includes it too; explicit here)

#include "AgentMaster/SessionModels.h" // ::Agentmaster::SessionState

namespace winrt::TerminalApp::implementation
{
    // The key a session's ids carry: its first 8 characters — the engine's ShortId convention (hooks.log, [nav]),
    // so an id read off the UI matches the log lines and the records by prefix. Shorter ids are returned whole.
    //   sessionId - the session's id (a Claude conversation id or a managed Codex handle).
    // Returns the key ("" for an empty id). Never throws.
    inline std::wstring AgentAutomationKey(std::wstring_view sessionId)
    {
        return std::wstring{ sessionId.substr(0, (std::min)(sessionId.size(), static_cast<size_t>(8))) };
    }

    // `prefix` + the session's key: AgentAutomationIdFor(L"am.mgr.board.card.", id) = "am.mgr.board.card.0391efeb".
    //   prefix    - the id up to the key, with its trailing dot.
    //   sessionId - the session the element shows.
    // Returns the id. Never throws (beyond std::bad_alloc).
    inline std::wstring AgentAutomationIdFor(std::wstring_view prefix, std::wstring_view sessionId)
    {
        std::wstring out{ prefix };
        out += AgentAutomationKey(sessionId);
        return out;
    }

    // Give `el` its AutomationId (change-gated: an equal id is not written again). An empty id CLEARS the local value,
    // so the element falls back to XAML's default (its x:Name, if any) — the right thing for a tab that stops hosting
    // a session. Used for elements built in code, which have no x:Name.
    //   el - the element; null is ignored.
    //   id - the id per the scheme above.
    // Throws what a DependencyProperty read/write on a live element throws (never in practice on the UI thread).
    inline void AgentSetAutomationId(const winrt::Windows::UI::Xaml::DependencyObject& el, std::wstring_view id)
    {
        using winrt::Windows::UI::Xaml::Automation::AutomationProperties;
        if (!el)
        {
            return;
        }
        if (id.empty())
        {
            if (!AutomationProperties::GetAutomationId(el).empty())
            {
                el.ClearValue(AutomationProperties::AutomationIdProperty());
            }
            return;
        }
        if (std::wstring_view{ AutomationProperties::GetAutomationId(el) } != id)
        {
            AutomationProperties::SetAutomationId(el, winrt::hstring{ id });
        }
    }

    // Give a NAMELESS element its accessible name (change-gated). See the NAMES rule above: never for a TextBox or a
    // control whose content is a word. An empty name clears the local value.
    //   el   - the element; null is ignored.
    //   name - what a screen reader should say for it ("Settings", "More actions", the session's title for a card).
    // Throws what a DependencyProperty read/write throws.
    inline void AgentSetAccessibleName(const winrt::Windows::UI::Xaml::DependencyObject& el, std::wstring_view name)
    {
        using winrt::Windows::UI::Xaml::Automation::AutomationProperties;
        if (!el)
        {
            return;
        }
        if (name.empty())
        {
            if (!AutomationProperties::GetName(el).empty())
            {
                el.ClearValue(AutomationProperties::NameProperty());
            }
            return;
        }
        if (std::wstring_view{ AutomationProperties::GetName(el) } != name)
        {
            AutomationProperties::SetName(el, winrt::hstring{ name });
        }
    }

    // Set `el`'s ItemStatus (change-gated; an empty status clears it). UI Automation raises a property-changed event
    // for a change only while a client listens, so an unchanged re-assert on every rebuild costs one read.
    //   el     - the element (the peer-bearing one: a Button, a TextBlock, a TabViewItem — a Border or a Grid has no
    //            automation peer, so a status set on one would never be read).
    //   status - the composed status (AgentItemStatus::Text()).
    // Throws what a DependencyProperty read/write throws.
    inline void AgentSetItemStatus(const winrt::Windows::UI::Xaml::DependencyObject& el, std::wstring_view status)
    {
        using winrt::Windows::UI::Xaml::Automation::AutomationProperties;
        if (!el)
        {
            return;
        }
        if (status.empty())
        {
            if (!AutomationProperties::GetItemStatus(el).empty())
            {
                el.ClearValue(AutomationProperties::ItemStatusProperty());
            }
            return;
        }
        if (std::wstring_view{ AutomationProperties::GetItemStatus(el) } != status)
        {
            AutomationProperties::SetItemStatus(el, winrt::hstring{ status });
        }
    }

    // The words of the item status facets — ONE spelling for every surface, so a test reads "unsent draft" the same on
    // a tab, a card and a tree row. The state words (AgentStateWord) come first; these follow in the order listed.
    inline constexpr std::wstring_view kAgentStatusNotStarted = L"not started"; // live but its claude has not started (a restored or parked tab: the half-hollow dot)
    inline constexpr std::wstring_view kAgentStatusUnread = L"unread"; // the tab's red flash ring (left Running for a needs-you state, not visited since)
    inline constexpr std::wstring_view kAgentStatusDraft = L"unsent draft"; // the "3 dots": text typed into claude's input box and not sent
    inline constexpr std::wstring_view kAgentStatusFavorite = L"favorite"; // the crown / star marker
    inline constexpr std::wstring_view kAgentStatusCacheWarm = L"cache warm"; // the spark crown / the board's ⚡: Claude's prompt cache is still warm
    inline constexpr std::wstring_view kAgentStatusCodex = L"codex"; // a managed Codex session (the teal pill)
    inline constexpr std::wstring_view kAgentStatusExternal = L"external"; // observed, not managed (the External column / scope)
    inline constexpr std::wstring_view kAgentStatusOtherWindow = L"in another window"; // the tree's GLOBAL row whose tab another window hosts
    inline constexpr std::wstring_view kAgentStatusSelected = L"selected"; // the surface's own selection (the board card, the tree row, the side panel's card)
    inline constexpr std::wstring_view kAgentStatusSelectedOnPage = L"selected on the Agent Manager page"; // a tab's selection pill (MGR-05)

    // A session state as the word the surfaces show — the Explorer Tree's row label and the side panel's card label
    // (StateLabel in AgentManagerContent.Internal.h: keep the two in step), so an item status reads like the screen.
    //   s - the state.
    // Returns "running", "waiting-for-you", "needs-approval", "error", "done" or "idle". Never throws.
    inline std::wstring_view AgentStateWord(::Agentmaster::SessionState s) noexcept
    {
        using ::Agentmaster::SessionState;
        switch (s)
        {
        case SessionState::Running:
            return L"running";
        case SessionState::WaitingForInput:
            return L"waiting-for-you";
        case SessionState::NeedsApproval:
            return L"needs-approval";
        case SessionState::Error:
            return L"error";
        case SessionState::Done:
            return L"done";
        case SessionState::Idle:
        default:
            return L"idle";
        }
    }

    // An item status being composed: facets appended in the caller's order, joined by "; " — "running; unsent draft;
    // selected". A facet that is empty or whose condition is false is skipped, so a caller lists every facet with its
    // condition and gets exactly the ones that hold:
    //     AgentItemStatus st;
    //     st.Add(AgentStateWord(s.state)).Add(kAgentStatusNotStarted, dormant).Add(kAgentStatusSelected, selected);
    //     AgentSetItemStatus(card, st.Text());
    class AgentItemStatus
    {
    public:
        // Append a facet.
        //   facet - its words (one of the kAgentStatus* constants, a state word, or a short phrase like "tags: a, b").
        //   when  - false skips it (the facet does not hold).
        // Returns *this, for chaining. Never throws (beyond std::bad_alloc).
        AgentItemStatus& Add(std::wstring_view facet, bool when = true)
        {
            if (!when || facet.empty())
            {
                return *this;
            }
            if (!_text.empty())
            {
                _text += L"; ";
            }
            _text += facet;
            return *this;
        }

        // The composed status ("" when no facet held).
        const std::wstring& Text() const noexcept
        {
            return _text;
        }

    private:
        std::wstring _text;
    };
}
