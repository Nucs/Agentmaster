// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (TCM-K2, doc/agentmaster/flows/23-tab-context-menu.md): who holds the KEYBOARD while a panel that a
// menu item opened is up, and where the keyboard goes when that panel closes. The panels are the Tags panel and the
// "Specify a model" prompt, both plain Popups parented into TerminalPage::Root(). Header-only, UI thread only.
// Included by Tab.cpp (the tab menu's Closed hand-back) and by the TerminalPage TUs that host the two panels
// (TerminalPage.AgentObserver.cpp, TerminalPage.AgentSessions.cpp).
//
// Three facts of the system XAML this app runs on (read in microsoft-ui-xaml's source, 2026-10-23) make it needed:
//   1. A MenuFlyoutItem raises Click FIRST and closes its menu after it (MenuFlyoutItem::Invoke). The menu's popup
//      closes in that same call and, since it holds the focus, returns the focus to the element that had it before
//      the menu opened (CPopup::Close, its light-dismiss branch). The menu's Closed EVENT comes much later:
//      FlyoutBase raises it from its presenter's Unloaded, after the close animation (FlyoutBase::OnPresenterUnloaded).
//   2. Upstream's tab menu sends the keyboard to the terminal from that late Closed (Tab.cpp, GH#5750). A panel that
//      the Click opened a tick later, and that had already focused its own box, lost the keyboard to the terminal
//      again: typed text went into Claude's input box, and Escape went to claude, where it interrupts a running turn
//      (TCM-K2). AgentFocusInOpenPopup lets that hand-back stand aside while the keyboard is inside an open popup.
//   3. The panels have no light dismiss, so closing one that holds the focus only removes the focused element. The
//      focus manager then moves the focus to the next focusable element it finds, anywhere in the window
//      (CFocusManager::SetFocusOnNextFocusableElement). A panel therefore gives the keyboard back itself:
//      AgentCaptureFocusReturn (at open) says to whom, and AgentPanelHoldsKeyboard (at close) says whether. The
//      panel also closes on the Escape KEY-UP, not the key-down: by the key-up, the keystroke's ESC character has
//      already gone to the panel's own box, so the terminal that gets the keyboard back receives none of it (the
//      Specify prompt's rule, AgentModelPrompt.h; upstream's tab renamer closes on key-up for the same reason).
//
// Relies on the TerminalApp pch for the winrt XAML headers (the AgentTipHelpers.h idiom).
#pragma once

#include <algorithm> // std::find over the open popups' children
#include <vector>

#include "AgentCatchLog.h" // AgentLogCaughtException — a contained failure stays diagnosable in hooks.log (Rule #18)

namespace AgentPanelFocusDetail
{
    // The COM identity of a projected object, for comparisons only. Two projections of one XAML element can be
    // different interface pointers, so `==` on the projections is not an identity test; IUnknown is.
    // Parameters:
    //   o - the object (any projected type); null gives null.
    // Returns: the identity pointer. No reference is kept, so it is valid only for comparing while `o` lives.
    // Exceptions: winrt::hresult_error when the object refuses IUnknown (never for a XAML element); callers contain it.
    inline void* Identity(const winrt::Windows::Foundation::IInspectable& o)
    {
        return o ? winrt::get_abi(o.as<winrt::Windows::Foundation::IUnknown>()) : nullptr;
    }

    // The visual a focus walk starts from. A TextElement (a Run, a Hyperlink inside a panel's text) is not a visual
    // and has no visual parent, so the walk starts from the control that hosts its text instead (the rule of the side
    // panel's SidePanelLaunchIsUnder and of AgentTranscriptOverlay::_IsUnder).
    // Parameters:
    //   element - the focused element; null gives null.
    // Returns: `element` itself, or its text host's visual for a TextElement (null when the host is gone).
    // Exceptions: what the TextElement read throws (winrt::hresult_error); callers contain it.
    inline winrt::Windows::UI::Xaml::DependencyObject VisualStart(const winrt::Windows::UI::Xaml::DependencyObject& element)
    {
        if (const auto textElement = element.try_as<winrt::Windows::UI::Xaml::Documents::TextElement>())
        {
            return textElement.ElementStart().VisualParent();
        }
        return element;
    }
}

// The element that holds the keyboard focus in the window of `root`, or null when nothing holds it.
// Parameters:
//   root - the window's XamlRoot. Null answers null (an element that is not in a live tree has no XamlRoot).
// Returns: the focused DependencyObject exactly as FocusManager reports it — a Hyperlink comes back as itself, not as
//   the control that hosts it.
// Exceptions: throws what FocusManager::GetFocusedElement throws (winrt::hresult_error). Every caller in this header
//   contains it; a direct caller must too.
inline winrt::Windows::UI::Xaml::DependencyObject AgentFocusedElement(const winrt::Windows::UI::Xaml::XamlRoot& root)
{
    if (!root)
    {
        return nullptr;
    }
    return winrt::Windows::UI::Xaml::Input::FocusManager::GetFocusedElement(root).try_as<winrt::Windows::UI::Xaml::DependencyObject>();
}

// True when `element` is `ancestor` or lies below it in the visual tree. The walk is bounded at 1024 steps, so a
// broken parent chain can never hang the UI thread; a TextElement starts from its text host (VisualStart).
// Parameters:
//   element  - where the walk starts (usually the focused element). Null gives false.
//   ancestor - the element to find. Null gives false.
// Returns: true when found within the bound.
// Exceptions: throws what VisualTreeHelper::GetParent / the TextElement read / QueryInterface throw
//   (winrt::hresult_error); callers contain it (AgentPanelHoldsKeyboard, AgentCaptureFocusReturn).
inline bool AgentIsUnderElement(const winrt::Windows::UI::Xaml::DependencyObject& element, const winrt::Windows::UI::Xaml::DependencyObject& ancestor)
{
    if (!element || !ancestor)
    {
        return false;
    }
    const void* const target = AgentPanelFocusDetail::Identity(ancestor);
    auto current = AgentPanelFocusDetail::VisualStart(element);
    for (int guard = 0; current && guard < 1024; ++guard)
    {
        if (AgentPanelFocusDetail::Identity(current) == target)
        {
            return true;
        }
        current = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(current);
    }
    return false;
}

// Agentmaster (TCM-K2): is the keyboard focus inside a Popup that is open in `root`'s window right now — a panel a
// menu item just opened (the Tags panel, the "Specify a model" prompt) or a dialog's popup? The tab menu's late
// Closed hand-back (Tab.cpp, GH#5750) asks this and stands aside on yes: the keyboard is where a menu action put it
// on purpose, and sending it to the terminal is exactly the TCM-K2 defect.
// What counts: any open popup whose Child is the focused element or one of its ancestors. A popup that holds nothing
// focusable (a tooltip, the tab header's always-open tag-badge row) can never answer yes, because the focus is never
// inside it.
// Cost: one GetOpenPopupsForXamlRoot, one QueryInterface per open popup's child and per ancestor of the focused
//   element (a window holds one open popup per tagged tab, so the children are collected once rather than walked one
//   by one). It runs once per closed tab menu.
// Parameters:
//   root         - the window's XamlRoot. Null answers false.
//   focusedClass - optional; on a yes, receives the focused element's runtime class name (for the log line).
// Returns: true on a yes. False when the focus is elsewhere, nothing holds it, or the answer cannot be read — a failure
//   is logged ([exc] in hooks.log) and answers false, which keeps upstream's behavior (the keyboard goes to the
//   terminal).
// Exceptions: none.
inline bool AgentFocusInOpenPopup(const winrt::Windows::UI::Xaml::XamlRoot& root, winrt::hstring* focusedClass = nullptr) noexcept
{
    try
    {
        const auto focused = AgentFocusedElement(root);
        if (!focused)
        {
            return false;
        }
        const auto popups = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root);
        if (!popups || popups.Size() == 0)
        {
            return false;
        }
        std::vector<void*> children;
        children.reserve(popups.Size());
        for (const auto& popup : popups)
        {
            if (popup)
            {
                if (const auto child = popup.Child())
                {
                    children.push_back(AgentPanelFocusDetail::Identity(child));
                }
            }
        }
        if (children.empty())
        {
            return false;
        }
        auto current = AgentPanelFocusDetail::VisualStart(focused);
        for (int guard = 0; current && guard < 1024; ++guard)
        {
            if (std::find(children.begin(), children.end(), AgentPanelFocusDetail::Identity(current)) != children.end())
            {
                if (focusedClass)
                {
                    *focusedClass = winrt::get_class_name(focused);
                }
                return true;
            }
            current = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(current);
        }
    }
    catch (...)
    {
        ::Agentmaster::AgentLogCaughtException(L"AgentFocusInOpenPopup (the tab menu's hand-back then runs as upstream)");
    }
    return false;
}

// Agentmaster (TCM-K2): would closing `panel` now take the keyboard with it? Yes when the focus is inside the panel,
// or when nothing in the window holds the focus (the keyboard is nowhere, so giving it back takes it from no one).
// Call it right BEFORE the panel's Popup closes: the close moves the focus at once (fact 3 in the header), and the
// answer is then lost.
// No when anything outside the panel holds the focus: a press that closed the panel had already focused what it hit
// (the terminal, a button), and a tab switch had already focused the new tab — the close must leave that focus alone.
// Parameters:
//   root  - the window's XamlRoot. Null answers false.
//   panel - the panel's card (its Popup's Child). Null answers false.
// Returns: as above. False when it cannot tell (a failure is logged): the close then moves no focus on a guess, which
//   is the behavior before this fix.
// Exceptions: none.
inline bool AgentPanelHoldsKeyboard(const winrt::Windows::UI::Xaml::XamlRoot& root, const winrt::Windows::UI::Xaml::DependencyObject& panel) noexcept
{
    if (!root || !panel)
    {
        return false;
    }
    try
    {
        const auto focused = AgentFocusedElement(root);
        return !focused || AgentIsUnderElement(focused, panel);
    }
    catch (...)
    {
        ::Agentmaster::AgentLogCaughtException(L"AgentPanelHoldsKeyboard (the closing panel gives no focus back)");
    }
    return false;
}

// Agentmaster (TCM-K2): where the keyboard goes back to when a panel that is about to take it closes — the Control
// that holds it now. Call it right BEFORE the panel focuses its own box (later, the box already holds it).
// The answer is an EMPTY reference, and the closing panel then hands the keyboard to the current tab instead
// (TerminalPage::_FocusCurrentTab), when:
//   - nothing holds the focus, or something that is not a Control does (a Hyperlink: focusing a text element back is
//     not reliable, and the current tab is a safe place for the keyboard);
//   - the focus is inside `panel` itself (an open while it is open: the panel is not a place to go back to);
//   - the focus is inside `handBackToCurrentTabUnder` (the tab strip). A tab's menu returns the focus to the tab
//     header it was opened on (fact 1 in the header), and GH#5750's rule is that after a tab's menu the keyboard
//     belongs to the terminal, not to the tab header; arrow keys there would move between tabs.
// The reference is weak: the target may be rebuilt or closed while the panel is up (a board card recycled by a
// rebuild, a tab closed), and the close then falls back to the current tab.
// Parameters:
//   root                      - the window's XamlRoot. Null gives an empty reference.
//   panel                     - the panel's card (its Popup's Child); may be null.
//   handBackToCurrentTabUnder - the tab strip (TerminalPage::_tabView); may be null.
// Returns: the weak reference, or an empty one as listed above, or when it cannot tell (a failure is logged).
// Exceptions: none.
inline winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::Control> AgentCaptureFocusReturn(
    const winrt::Windows::UI::Xaml::XamlRoot& root,
    const winrt::Windows::UI::Xaml::DependencyObject& panel,
    const winrt::Windows::UI::Xaml::DependencyObject& handBackToCurrentTabUnder) noexcept
{
    try
    {
        const auto focused = AgentFocusedElement(root);
        if (!focused)
        {
            return {};
        }
        const auto control = focused.try_as<winrt::Windows::UI::Xaml::Controls::Control>();
        if (!control || AgentIsUnderElement(focused, panel) || AgentIsUnderElement(focused, handBackToCurrentTabUnder))
        {
            return {};
        }
        return winrt::make_weak(control);
    }
    catch (...)
    {
        ::Agentmaster::AgentLogCaughtException(L"AgentCaptureFocusReturn (the closing panel hands the keyboard to the current tab)");
    }
    return {};
}
