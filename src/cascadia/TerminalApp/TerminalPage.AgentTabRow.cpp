// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TerminalPage, the AUTO-HIDING TAB ROW partial (TAB_STRIP.md "Auto-hiding the tab row").
//
// The user's request of 2026-11-15: "I want to add a autohide of the top row with tabs row and home button and + and V and
// minimize, maximize, close buttons. This row needs to be autohiding and functioning similar to windows taskbar autohide.
// Add a new configuration in settings too, default false. synced to the V menu item. It should bo toggleable via the V
// icon". Their picks the same day (AskUserHtmlQuestion, live mockups):
//   - the toggle: "▾ beside + again" — a small ▾ MENU BUTTON right of + (the launcher commit 777ce6b25 had removed the
//     SplitButton's ▾ on their earlier pick, "One + panel, no ▾"); its menu holds the toggle, then Sessions…, Agentmaster
//     settings and Windows Terminal ▸ (Settings, Command palette, About);
//   - the behavior: "Like the taskbar" — "but no 2px line stays, 0": rest 0.15 s in the top 4 pixels and the row slides out
//     over the terminal in 0.15 s; 0.6 s after the pointer leaves it slides away (AgentMaster/TabRowAutoHide.h);
//   - both extras: "Peek on keyboard tab switch" (Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+Alt+1…9 bring it out for 1.5 s) and
//     "Stay out on the Manager page" (the row stays while the window shows its Agent Manager page).
//
// Who does what:
//   - THE WINDOW HOST (WindowsTerminal.exe's NonClientIslandWindow) owns the title bar — the tab row, the drag bar and the
//     caption buttons — so it floats it over the content, slides it, keeps its drag-bar input window and hit test in step,
//     and runs the rule on the pointer, the buttons, the keyboard focus and the open popups.
//   - THIS PAGE owns the setting (AppSettings::autoHideTabRow — written only by _SetTabRowAutoHide: the ▾ menu item and the
//     cog's switch, both instant), the HOLD (the Agent Manager page on screen) and the PEEKS (a keyboard tab switch, the
//     launcher opened by its keys). It tells the window host through two events the AppHost relays: TabRowAutoHideChanged
//     (re-read TabRowAutoHide + TabRowAutoHideHold) and TabRowPeekRequested (re-read TabRowPeekMs + TabRowPeekInstant).
//   - The setting needs tabs in the title bar (Windows Terminal's showTabsInTitlebar, its default): with the system title
//     bar there is no row the window could slide, so this page tells the host "off" and the ▾ item is disabled with why.

#include "pch.h"
#include "TerminalPage.h"

#include "AgentAutomation.h" // AgentSetAutomationId — am.strip.menu, am.strip.menu.autoHide (E2E_TESTING.md §6.3)
#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentManagerContent.h" // ShowSettingsOverlay, SyncTabRowAutoHideSwitch — the cog's matching switch
#include "AgentTipHelpers.h" // AgentSetTip / AgentSetTitledTip — the shared tooltip host (never a per-element ToolTip)
#include "AgentMaster/ClaudeSpawn.h" // LogNav
#include "AgentMaster/Engine.h" // BroadcastSettingsChanged
#include "AgentMaster/Persistence.h" // LoadAppSettings / SaveAppSettings — the freshest-disk read-modify-write
#include "AgentMaster/TabRowAutoHide.h" // kTabRowPeekMs, kTabRowLauncherPeekMs

#include <array> // kTabRowMenuColorKeys

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace
{
    // The ▾ menu's glyphs, Segoe Fluent Icons (the strip's own icon font: SymbolThemeFontFamily).
    constexpr const wchar_t* kTabRowMenuAutoHideGlyph = L"\xE70E"; // ChevronUp: the row slides up out of the way
    constexpr const wchar_t* kTabRowMenuSessionsGlyph = L"\xE81C"; // History — the launcher footer's Sessions… glyph
    constexpr const wchar_t* kTabRowMenuSettingsGlyph = L"\xE713"; // Setting — the launcher footer's Agentmaster settings glyph
    constexpr const wchar_t* kTabRowMenuTerminalGlyph = L"\xE756"; // CommandPrompt — the launcher footer's Windows Terminal glyph

    // A ▾ menu item's icon: `glyph` in the strip's icon font.
    //   glyph   - one of the kTabRowMenu*Glyph code points.
    //   returns - a new FontIcon for MenuFlyoutItem::Icon (the item's 16 DIP icon box sizes it).
    //   throws  - hresult_error / bad_alloc from creating the element; the menu's Opening handler contains it.
    FontIcon TabRowMenuGlyph(const wchar_t* glyph)
    {
        FontIcon icon{};
        icon.Glyph(glyph);
        return icon;
    }
}

namespace winrt::TerminalApp::implementation
{
    // ---- What the AppHost reads (member loads, cached by _ApplyTabRowAutoHide / _PeekTabRow) ---------------------------

    // Returns: whether THIS window auto-hides its title bar, as last told to the AppHost. Never throws.
    bool TerminalPage::TabRowAutoHide() const noexcept
    {
        return _tabRowAutoHideApplied;
    }

    // Returns: whether the auto-hidden row is held out (the Agent Manager page is on screen), as last told. Never throws.
    bool TerminalPage::TabRowAutoHideHold() const noexcept
    {
        return _tabRowAutoHideHoldApplied;
    }

    // Returns: the last peek's length in ms (0 before any peek). Never throws.
    uint32_t TerminalPage::TabRowPeekMs() const noexcept
    {
        return _tabRowPeekMs;
    }

    // Returns: whether the last peek places the row at once, without the slide. Never throws.
    bool TerminalPage::TabRowPeekInstant() const noexcept
    {
        return _tabRowPeekInstant;
    }

    // ---- The setting, the hold, the peeks --------------------------------------------------------------------------

    // See TerminalPage.h. On = the setting AND tabs in the title bar (the only window that has a row to slide); held = on AND
    // the Agent Manager page on screen. The raise reaches the AppHost synchronously, which reads both back and calls
    // NonClientIslandWindow::SetTitlebarAutoHide — XAML and Win32 calls on this window only, never back into this page.
    void TerminalPage::_ApplyTabRowAutoHide() noexcept
    {
        try
        {
            const bool inTitlebar = _settings && _settings.GlobalSettings().ShowTabsInTitlebar();
            const bool on = _appSettings.autoHideTabRow && inTitlebar;
            const bool hold = on && _managerPageVisible;
            if (on == _tabRowAutoHideApplied && hold == _tabRowAutoHideHoldApplied)
            {
                return; // nothing changed for the window host
            }
            _tabRowAutoHideApplied = on;
            _tabRowAutoHideHoldApplied = hold;
            TabRowAutoHideChanged.raise(*this, nullptr);
        }
        catch (...)
        {
            // The window host keeps what it had (the row as it was); the next apply retries (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"_ApplyTabRowAutoHide");
        }
    }

    // See TerminalPage.h. The recipe of the side panel's ⌖ toggle (_SidePanelToggleScrollToTab): a freshest-disk RMW of the
    // ONE field — another window may have saved other settings since this one loaded, and writing this window's whole copy
    // back would regress them — then adopted here and BROADCAST live (the struct just read from disk, never this window's
    // copy, which can be stale on other out-of-cog fields). The broadcast reaches every OTHER window, which adopts it in
    // _ApplyBroadcastSettings (its row floats or comes back, its cog's switch follows).
    //   on     - the new value.
    //   source - who flipped it ("strip menu", "cog"), for the [nav] line.
    void TerminalPage::_SetTabRowAutoHide(const bool on, const std::wstring_view source) noexcept
    {
        try
        {
            if (_appSettings.autoHideTabRow != on)
            {
                auto s = ::Agentmaster::LoadAppSettings();
                s.autoHideTabRow = on;
                ::Agentmaster::SaveAppSettings(s);
                _appSettings.autoHideTabRow = on;
                ::Agentmaster::LogNav(std::wstring{ L"tab row auto-hide -> " } + (on ? L"on" : L"off") + L" (" + std::wstring{ source } + L")");
                try
                {
                    ::Agentmaster::BroadcastSettingsChanged(s, _windowId);
                }
                catch (...)
                {
                    // Saved and applied here; another window adopts it at its next broadcast or launch (Rule #18: logged).
                    ::Agentmaster::AgentLogCaughtException(L"_SetTabRowAutoHide broadcast");
                }
            }
            _ApplyTabRowAutoHide();
            // This window's cog, if it is open, shows the value now (its switch is the other writer).
            if (const auto ipc = _agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->SyncTabRowAutoHideSwitch(on);
                }
            }
        }
        catch (...)
        {
            // The flip stopped part-way: what was saved stays saved, and the next apply / cog open re-reads it (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_SetTabRowAutoHide");
        }
    }

    // See TerminalPage.h. The values ride two members because the event carries none (the AppHost re-reads them, the
    // TaskbarUnreadFrame idiom).
    //   durationMs - how long the peek keeps the row out.
    //   instant    - place it without the slide.
    void TerminalPage::_PeekTabRow(const uint32_t durationMs, const bool instant) noexcept
    {
        if (!_tabRowAutoHideApplied)
        {
            return; // the row is always out (or there is none to slide): nothing to peek
        }
        try
        {
            _tabRowPeekMs = durationMs;
            _tabRowPeekInstant = instant;
            TabRowPeekRequested.raise(*this, nullptr);
        }
        catch (...)
        {
            // The row simply does not peek this time (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"_PeekTabRow");
        }
    }

    // ---- The ▾ menu button --------------------------------------------------------------------------------------------

    // The Button resource keys the ▾ wears "+"'s colors through (the SplitButton* keys "+" itself uses, for a plain Button).
    constexpr std::array<const wchar_t*, 6> kTabRowMenuColorKeys{
        L"ButtonBackground",
        L"ButtonBackgroundPointerOver",
        L"ButtonBackgroundPressed",
        L"ButtonForeground",
        L"ButtonForegroundPointerOver",
        L"ButtonForegroundPressed",
    };

    // See TerminalPage.h. The keys go into the Button's own Resources (they win over its XAML theme dictionaries, the way
    // _SetNewTabButtonColor's keys win over the SplitButton's), then a PointerOver -> Normal toggle makes the template's
    // visual state setters re-read them (Tab::_RefreshVisualState's trick; without transitions: a theme pass is no gesture).
    //   background, hover, pressed - the button's background at rest, under the pointer, pressed.
    //   foreground                 - its glyph's color in every state.
    void TerminalPage::_TabRowMenuButtonApplyColors(const winrt::Windows::UI::Xaml::Media::SolidColorBrush& background,
                                                    const winrt::Windows::UI::Xaml::Media::SolidColorBrush& hover,
                                                    const winrt::Windows::UI::Xaml::Media::SolidColorBrush& pressed,
                                                    const winrt::Windows::UI::Xaml::Media::SolidColorBrush& foreground) noexcept
    {
        if (!_tabRowMenuButton)
        {
            return;
        }
        try
        {
            const auto resources = _tabRowMenuButton.Resources();
            const winrt::Windows::UI::Xaml::Media::SolidColorBrush values[]{ background, hover, pressed, foreground, foreground, foreground };
            for (size_t i = 0; i < kTabRowMenuColorKeys.size(); ++i)
            {
                resources.Insert(winrt::box_value(kTabRowMenuColorKeys[i]), values[i]);
            }
            _tabRowMenuButton.Background(background);
            _tabRowMenuButton.Foreground(foreground);
            VisualStateManager::GoToState(_tabRowMenuButton, L"PointerOver", false);
            VisualStateManager::GoToState(_tabRowMenuButton, L"Normal", false);
        }
        catch (...)
        {
            // The ▾ keeps its previous colors until the next theme pass (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"_TabRowMenuButtonApplyColors");
        }
    }

    // See TerminalPage.h.
    //   background, foreground - what "+" was just given back (either may be null).
    void TerminalPage::_TabRowMenuButtonClearColors(const winrt::Windows::UI::Xaml::Media::SolidColorBrush& background,
                                                    const winrt::Windows::UI::Xaml::Media::SolidColorBrush& foreground) noexcept
    {
        if (!_tabRowMenuButton)
        {
            return;
        }
        try
        {
            const auto resources = _tabRowMenuButton.Resources();
            for (const auto key : kTabRowMenuColorKeys)
            {
                const auto boxed = winrt::box_value(key);
                if (resources.HasKey(boxed))
                {
                    resources.Remove(boxed);
                }
            }
            _tabRowMenuButton.Background(background);
            _tabRowMenuButton.Foreground(foreground);
        }
        catch (...)
        {
            // The ▾ keeps the last colors until the next theme pass (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"_TabRowMenuButtonClearColors");
        }
    }

    // See TerminalPage.h. The side panel ⋯ menu's recipe (_SidePanelRunMenuCommand), with this menu's name in its log lines.
    //   what    - the command's name, for the [exc] line of a failure.
    //   command - the work, run on the UI thread one tick later with the page (if it still exists).
    void TerminalPage::_TabRowRunMenuCommand(const std::wstring_view what, std::function<void(TerminalPage&)> command) noexcept
    {
        try
        {
            Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis = get_weak(), whatText = std::wstring(what), run = std::move(command)]() {
                const auto page = weakThis.get();
                if (!page)
                {
                    return; // the window closed before the tick: nothing left to change
                }
                try
                {
                    run(*page);
                }
                catch (...)
                {
                    // The command stopped part-way; what it changed before the throw stays (the toggle saves first).
                    // Recorded with the command's name (Rule #18).
                    ::Agentmaster::AgentLogCaughtException((L"tab row menu command: " + whatText).c_str());
                }
            });
        }
        catch (...)
        {
            // The dispatcher refused the hop (the window is tearing down): the click is lost (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"tab row menu: queue a command");
        }
    }

    // See TerminalPage.h. The menu is filled at EVERY open, so its check can never be stale (another window, or the cog, may
    // have flipped the setting since the last open). The Opening handler reaches the page weakly and the menu through
    // `sender` — never by capture: a handler that captured its own element would keep it alive forever (CLAUDE.md, Gotchas
    // "A handler that captures its own element is never freed").
    void TerminalPage::_TabRowMenuDecorate() noexcept
    {
        if (!_tabRowMenuButton)
        {
            return;
        }
        try
        {
            AgentSetAutomationId(_tabRowMenuButton, L"am.strip.menu");
            AgentSetTitledTip(_tabRowMenuButton,
                              L"Tab row menu",
                              L"Auto-hide the tab row (the tabs, Home, + and \x25BE and the window buttons), the Sessions browser, Agentmaster's settings, and Windows Terminal's own Settings, Command palette and About.");
            MenuFlyout menu{};
            menu.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
            menu.Opening([weakThis = get_weak()](const IInspectable& sender, const IInspectable&) {
                const auto self = weakThis.get();
                const auto opened = sender.try_as<MenuFlyout>();
                if (!self || !opened)
                {
                    return;
                }
                try
                {
                    opened.Items().Clear();
                    self->_TabRowMenuFill(opened);
                }
                catch (...)
                {
                    // The menu opens with the items built so far (Rule #18: logged).
                    ::Agentmaster::AgentLogCaughtException(L"tab row menu: filling");
                }
            });
            _tabRowMenuButton.Flyout(menu);
        }
        catch (...)
        {
            // The button then opens nothing; the cog's switch still reaches the setting (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"_TabRowMenuDecorate");
        }
    }

    // See TerminalPage.h. Each command runs one dispatcher tick after its click (_TabRowRunMenuCommand: the menu is
    // closing, and a command that moves focus or opens a page must not run inside the click), and the toggle first
    // re-checks the state its item showed, so a menu left open while another window flipped the setting acts on what it
    // showed and never inverts it. Windows Terminal's Settings runs AT the click instead: it reads the click's Alt / Shift.
    //   menu - the ▾ button's MenuFlyout, emptied by the Opening handler.
    void TerminalPage::_TabRowMenuFill(const MenuFlyout& menu)
    {
        auto weakThis = get_weak();
        const bool inTitlebar = _settings && _settings.GlobalSettings().ShowTabsInTitlebar();
        const bool autoHideOn = _appSettings.autoHideTabRow;

        // ✓ Auto-hide the tab row — the setting, a check item.
        ToggleMenuFlyoutItem autoHide{};
        autoHide.Text(L"Auto-hide the tab row");
        autoHide.IsChecked(autoHideOn);
        autoHide.Icon(TabRowMenuGlyph(kTabRowMenuAutoHideGlyph));
        AgentSetAutomationId(autoHide, L"am.strip.menu.autoHide");
        std::wstring tip{ L"Hide this row \x2014 the tabs, Home, + and \x25BE, and the minimize / maximize / close buttons \x2014 like the Windows taskbar's auto-hide: rest the pointer at the window's top edge and it slides out over the terminal (which never resizes); it slides away 0.6 s after the pointer leaves it. It stays while a menu from it is open, and while the Agent Manager page is shown; Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+Alt+1\x2026" L"9 show it for a moment.\nThe same switch as Agentmaster settings \x203A Tabs & Overlay \x203A TAB STRIP. Saved globally \x2014 every window follows it at once." };
        if (!inTitlebar)
        {
            // Without tabs in the title bar the window wears the system title bar, which cannot slide away.
            autoHide.IsEnabled(false);
            tip += L"\nUnavailable: Windows Terminal shows the system title bar in this window (its \x201CHide the title bar\x201D setting, showTabsInTitlebar, is off). Turn it on and restart.";
        }
        AgentSetTitledTip(autoHide, L"Auto-hide the tab row", winrt::hstring{ tip });
        autoHide.Click([weakThis, autoHideOn](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_TabRowRunMenuCommand(L"tab row auto-hide", [autoHideOn](TerminalPage& page) {
                    // The item flipped its own check before Click; the setting is the truth. Flip only from the state the
                    // check showed, so a change made elsewhere while the menu was open is not undone.
                    if (page._appSettings.autoHideTabRow == autoHideOn)
                    {
                        page._SetTabRowAutoHide(!autoHideOn, L"strip menu");
                    }
                });
            }
        });
        menu.Items().Append(autoHide);
        menu.Items().Append(MenuFlyoutSeparator{});

        // Sessions… — the Sessions browser (the launcher footer's first link).
        MenuFlyoutItem sessions{};
        sessions.Text(L"Sessions\x2026");
        sessions.Icon(TabRowMenuGlyph(kTabRowMenuSessionsGlyph));
        AgentSetAutomationId(sessions, L"am.strip.menu.sessions");
        AgentSetTip(sessions, L"Open the Sessions browser: every Claude Code session on disk, to resume or fork a closed one.");
        sessions.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_TabRowRunMenuCommand(L"tab row menu: sessions", [](TerminalPage& page) { page._ShowSessionsPage(); });
            }
        });
        menu.Items().Append(sessions);

        // Agentmaster settings — the cog on the Agent Manager page (the launcher footer's second link).
        MenuFlyoutItem settings{};
        settings.Text(L"Agentmaster settings");
        settings.Icon(TabRowMenuGlyph(kTabRowMenuSettingsGlyph));
        AgentSetAutomationId(settings, L"am.strip.menu.settings");
        AgentSetTip(settings, L"Open Agentmaster's settings (the \x2699 of the Agent Manager page): sessions, presets, the Prompt Queue Runner, tabs, messaging\x2026");
        settings.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_TabRowRunMenuCommand(L"tab row menu: agentmaster settings", [](TerminalPage& page) {
                    // The cog lives on the Agent Manager page: show the page, then open the cog over it.
                    page._ShowManagerPage(L"tab row menu: agentmaster settings");
                    if (const auto ipc = page._agentManagerContent.get())
                    {
                        if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                        {
                            mgr->ShowSettingsOverlay();
                        }
                    }
                });
            }
        });
        menu.Items().Append(settings);

        // Windows Terminal ▸ — its Settings, Command palette and About (what the old ▾ held; the launcher footer has them too).
        MenuFlyoutSubItem terminal{};
        terminal.Text(L"Windows Terminal");
        terminal.Icon(TabRowMenuGlyph(kTabRowMenuTerminalGlyph));
        AgentSetAutomationId(terminal, L"am.strip.menu.terminal");
        AgentSetTip(terminal, L"Windows Terminal's own settings (profiles, color schemes, keys), its command palette and About.");
        MenuFlyoutItem openSettings{};
        openSettings.Text(L"Settings");
        AgentSetAutomationId(openSettings, L"am.strip.menu.terminal.settings");
        openSettings.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                // Windows Terminal's settings UI (Alt / Shift: its JSON files), read at the click: the modifiers are the click's.
                self->_SettingsButtonOnClick(nullptr, nullptr);
            }
        });
        terminal.Items().Append(openSettings);
        MenuFlyoutItem palette{};
        palette.Text(L"Command palette");
        AgentSetAutomationId(palette, L"am.strip.menu.terminal.palette");
        palette.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_TabRowRunMenuCommand(L"tab row menu: command palette", [](TerminalPage& page) { page._CommandPaletteButtonOnClick(nullptr, nullptr); });
            }
        });
        terminal.Items().Append(palette);
        MenuFlyoutItem about{};
        about.Text(L"About");
        AgentSetAutomationId(about, L"am.strip.menu.terminal.about");
        about.Click([weakThis](const IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakThis.get())
            {
                self->_TabRowRunMenuCommand(L"tab row menu: about", [](TerminalPage& page) { page._AboutButtonOnClick(nullptr, nullptr); });
            }
        });
        terminal.Items().Append(about);
        menu.Items().Append(terminal);
    }
}
