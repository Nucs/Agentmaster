// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: a SUBMENU line that also answers a CLICK on itself, the one implementation behind three menus:
//   - the Filter menu's "Folder ▸" (a side panel card's and a tab's; SIDE_PANEL.md §2l "The Folder line's own click"):
//     the click picks the session's whole working folder with its subfolders;
//   - the session menu's "Deactivate ▸" (a side panel card, a board card, a tree row; AGENT_MANAGER.md *Deactivating
//     sessions*) and the tab menu's "Deactivate ▸" (TAB_STRIP.md *The tab menu*): the click parks that one tab, the same
//     as the submenu's "Deactivate Tab" (the user's request of 2026-11-14: "Deactivate-> should be clickable and not
//     only open menu. same as deactivate this tab", and their pick "Everywhere, tab strip too").
// Header-only, UI thread only. Relies on the TerminalApp pch for the winrt XAML and input headers (the AgentTipHelpers.h
// idiom).
//
// WHY a helper: ⚠ a Windows.UI.Xaml MenuFlyoutSubItem raises NO Click. Its class handlers take the left press and, at the
// release, open the submenu, marking both events handled — and a class handler always runs before an instance handler
// of the same element. So the line watches the pointer itself, with handledEventsToo:
//   - a LEFT press on it, by a mouse or a pen, ARMS (when `canClick` agrees at that moment);
//   - the release of that button ON the line, while armed, CLICKS: the whole menu is hidden (MenuFlyout::Hide takes the
//     submenu the item just opened at this release with it) and `onClick` runs;
//   - leaving the line or a cancelled pointer DISARMS, and every release ends the arm, so a press that began on another
//     line never clicks here, and a release off the line's bounds (a pointer the item may have captured reports there
//     too) does not click either. NOT a lost capture: an item that captured the pointer would lose the capture as it
//     handles the release — before our release handler — and a disarm there would eat every click.
// What stays the item's own: RESTING on the line (its hover delay opens the submenu), the KEYBOARD (Enter, Space and →
// open the submenu, whose lines include the click's own action) and a finger's TAP (a finger cannot hover, so the tap is
// its only way into the submenu: a touch press never arms).
#pragma once

#include <functional>
#include <memory> // std::shared_ptr — the arm flag shared by the line's handlers
#include <utility> // std::exchange — every release ends the arm

#include "AgentCatchLog.h" // AgentLogCaughtException — a contained failure stays diagnosable in hooks.log (Rule #18)

// Make `line` answer a mouse or pen click on itself with `onClick`, keeping its submenu (see the header comment).
//   line       - the submenu line (its Text, Icon and Items are the caller's; nothing of them is touched).
//   rootMenu   - the right-click menu the line sits in (directly or through submenus): hidden at a click. Held WEAKLY:
//                the menu owns the line through its Items, and a strong capture would be a reference cycle that
//                C++/WinRT never frees (the whole menu would leak).
//   canClick   - asked at each PRESS (so it reads the state of that moment, e.g. "Deactivate Tab" grayed for a dormant
//                session): false = the press does not arm, and the line is a plain submenu for that press. Empty = always.
//                Called on the UI thread; a throw is contained (logged, not armed).
//   onClick    - the click's action, run on the UI thread right after the menu was hidden, inside the release's input
//                handler: it must not change the visual tree or open a dialog there — defer that work one dispatcher
//                tick (every caller does, as its menu items do). A throw is contained and logged.
//   logContext - the line's name for the [exc] lines a contained failure writes (a static string, e.g. "tab menu: the
//                Deactivate line"; never null).
// Returns nothing. Throws what registering the four handlers throws (winrt::hresult_error, std::bad_alloc); the caller's
// menu build contains it. The handlers capture the flag, the two functions, the weak menu and the context — never `line`
// (each handler reads it as its sender) — so they can never keep the line or the menu alive.
inline void AgentWireSubmenuHeaderClick(const winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem& line,
                                        const winrt::Windows::UI::Xaml::Controls::MenuFlyout& rootMenu,
                                        std::function<bool()> canClick,
                                        std::function<void()> onClick,
                                        const wchar_t* logContext)
{
    using winrt::Windows::Foundation::IInspectable;
    using winrt::Windows::UI::Xaml::FrameworkElement;
    using winrt::Windows::UI::Xaml::UIElement;
    using winrt::Windows::UI::Xaml::Input::PointerEventHandler;
    using winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs;

    const auto armed = std::make_shared<bool>(false);
    const auto weakMenu = winrt::make_weak(rootMenu);

    // THE PRESS: decides afresh whether this press may click.
    line.AddHandler(
        UIElement::PointerPressedEvent(),
        winrt::box_value(PointerEventHandler{ [armed, canClick, logContext](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            *armed = false;
            try
            {
                const auto element = sender.try_as<UIElement>();
                if (!element || e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch)
                {
                    return; // a finger's tap opens the submenu: the item's own way, untouched
                }
                if (!e.GetCurrentPoint(element).Properties().IsLeftButtonPressed())
                {
                    return; // a right or middle press: never a click
                }
                if (canClick && !canClick())
                {
                    return; // the caller says no click now (e.g. nothing to deactivate): a plain submenu
                }
                *armed = true;
            }
            catch (...)
            {
                // Not armed: the release opens the submenu, the item's own behavior. Recorded (Rule #18).
                ::Agentmaster::AgentLogCaughtException(logContext);
            }
        } }),
        true /* handledEventsToo: the item marks its press handled */);

    // THE RELEASE: the click, when this line's own left press armed it and the pointer is still on the line.
    line.AddHandler(
        UIElement::PointerReleasedEvent(),
        winrt::box_value(PointerEventHandler{ [armed, weakMenu, onClick, logContext](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (!std::exchange(*armed, false))
            {
                return; // no press of ours began on this line (another line's press, a finger, a right button, a "no")
            }
            try
            {
                const auto element = sender.try_as<FrameworkElement>();
                if (!element)
                {
                    return;
                }
                const auto point = e.GetCurrentPoint(element);
                if (point.Properties().PointerUpdateKind() != winrt::Windows::UI::Input::PointerUpdateKind::LeftButtonReleased)
                {
                    return; // another button let go while the left one is still down: not the click
                }
                const auto at = point.Position();
                if (at.X < 0 || at.Y < 0 || at.X >= element.ActualWidth() || at.Y >= element.ActualHeight())
                {
                    return; // let go off the line: no click
                }
                // The whole menu closes at once, like a menu item's click closes it — with the submenu the item opened at
                // this very release (a menu's close takes its open submenus with it).
                if (const auto menu = weakMenu.get())
                {
                    menu.Hide();
                }
                if (onClick)
                {
                    onClick();
                }
            }
            catch (...)
            {
                // The click is lost (the menu may stay open, its submenu shown); the next click tries again. Recorded.
                ::Agentmaster::AgentLogCaughtException(logContext);
            }
        } }),
        true /* handledEventsToo: the item marks its release handled (it opens the submenu there) */);

    // THE DISARMS, each its own handler object. Assigning a bool cannot throw, so neither needs a catch.
    line.AddHandler(
        UIElement::PointerExitedEvent(),
        winrt::box_value(PointerEventHandler{ [armed](const IInspectable&, const PointerRoutedEventArgs&) { *armed = false; } }),
        true /* handledEventsToo: the item handles its exit (its hover delay) */);
    line.AddHandler(
        UIElement::PointerCanceledEvent(),
        winrt::box_value(PointerEventHandler{ [armed](const IInspectable&, const PointerRoutedEventArgs&) { *armed = false; } }),
        true /* handledEventsToo */);
}
