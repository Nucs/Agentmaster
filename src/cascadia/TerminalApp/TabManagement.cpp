// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.
//
// This file contains much of the code related to tab management for the
// TerminalPage. Things like opening new tabs, selecting different tabs,
// switching tabs, should all be handled in this file. Hypothetically, in the
// future, the contents of this file could be moved to a separate class
// entirely.
//

#include "pch.h"
#include "TerminalPage.h"
#include "Utils.h"
#include "../../types/inc/utils.hpp"
#include "../../inc/til/string.h"
#include <til/io.h>

#include "TabRowControl.h"
#include "DebugTapConnection.h"
#include "DesktopNotification.h"
#include "..\TerminalSettingsModel\FileUtils.h"
#include "../TerminalSettingsAppAdapterLib/TerminalSettings.h"

// Agentmaster: _DuplicateTab forks a managed Claude session instead of re-running its commandline;
// _HandleClosePaneRequested archives a managed session whose pane is explicitly closed.
#include "AgentMaster/SessionRegistry.h" // _sessionRegistry->Get()
#include "AgentMaster/ClaudeSpawn.h" // ClaudeConversationExists / AppendStateLog
#include "AgentMaster/Engine.h" // EnsureClaudeAvailable (native-exe-only launch gate)
#include "AgentMaster/Persistence.h" // DeriveSessionTitle / SaveSessions
#include "AgentMaster/SessionStore.h" // SetSessionFavorite (FAVORITES.md: "Favorite & Close All" batch branch)
#include "AgentTabOverlay.h" // _claudeOverlays.erase needs the complete com_ptr<AgentTabOverlay> type
#include "AgentManagerContent.h" // the Manager content type (its SetLensVisible call moved with the funnel's other Agentmaster steps into _ApplyAgentTabSelection, TerminalPage.AgentEngine.cpp)

#include <shlobj.h>

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::Storage::Provider;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace winrt::TerminalApp::implementation
{
    // Method Description:
    // - Open a new tab. This will create the TerminalControl hosting the
    //   terminal, and add a new Tab to our list of tabs. The method can
    //   optionally be provided a NewTerminalArgs, which will be used to create
    //   a tab using the values in that object.
    // Arguments:
    // - newTerminalArgs: An object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See TerminalSettings::CreateWithNewTerminalArgs for more details.
    // - existingConnection: An optional connection that is already established to a PTY
    //   for this tab to host instead of creating one.
    //   If not defined, the tab will create the connection.
    HRESULT TerminalPage::_OpenNewTab(const INewContentArgs& newContentArgs)
    try
    {
        if (const auto& newTerminalArgs{ newContentArgs.try_as<NewTerminalArgs>() })
        {
            const auto profile{ _settings.GetProfileForArgs(newTerminalArgs) };
            // GH#11114: GetProfileForArgs can return null if the index is higher
            // than the number of available profiles.
            if (!profile)
            {
                return S_FALSE;
            }
            const auto settings{ Settings::TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs) };

            // Try to handle auto-elevation
            if (_maybeElevate(newTerminalArgs, settings, profile))
            {
                return S_OK;
            }
            // We can't go in the other direction (elevated->unelevated)
            // unfortunately. This seems to be due to Centennial quirks. It works
            // unpackaged, but not packaged.
        }

        // This call to _MakePane won't return nullptr, we already checked that
        // case above with the _maybeElevate call.
        _CreateNewTabFromPane(_MakePane(newContentArgs, nullptr));
        return S_OK;
    }
    CATCH_RETURN();

    // Method Description:
    // - Sets up state, event handlers, etc on a tab object that was just made.
    // - Agentmaster: inserts the tab's header into the strip and — unless the tab opens in the background
    //   (_openClaudeTabInBackground, the Sessions page's bulk open) — selects it and switches to it. The
    //   strip's first layout pass with the new header (the settle, _ApplyTabDragPolicy) runs AFTER the tab
    //   is selected and BEFORE the switch: MUX's own bring-into-view then goes to the new tab instead of the
    //   tab the user was on, and the switch's strip reveal measures a header that has been laid out. A tab
    //   opened beside a tab that is on screen (the tab menu's "New Session Here" / "Fork session" /
    //   "Duplicate tab") therefore appears where it was asked for, and the strip stays on it instead of
    //   scrolling back to the tab the user was on (_holdTabSwitchForNewTab in TerminalPage.h).
    // Arguments:
    // - newTabImpl: the uninitialized tab.
    // - insertPosition: Optional parameter to indicate the position of tab (-1, the default: the end of the
    //   strip, or right after the focused tab under NewTabPosition::AfterCurrentTab).
    // Return Value:
    // - <none>
    // Exceptions:
    // - whatever a WinRT/XAML call here throws propagates to the caller (upstream behavior): the tab list or
    //   strip insert, an event registration, or the selection. A throwing selection still settles the strip
    //   before the exception leaves. A failure inside the switch itself (_OnTabSelectionChanged) does NOT
    //   propagate: it is logged ([wil] in hooks.log) and contained. The switch used to run inside XAML's
    //   SelectionChanged dispatch, never in this function's callers, so letting it out would hand every
    //   caller a failure path it never had.
    void TerminalPage::_InitializeTab(winrt::com_ptr<Tab> newTabImpl, uint32_t insertPosition)
    {
        newTabImpl->Initialize();

        // If insert position is not passed, calculate it
        if (insertPosition == -1)
        {
            insertPosition = _tabs.Size();
            if (_settings.GlobalSettings().NewTabPosition() == NewTabPosition::AfterCurrentTab)
            {
                auto currentTabIndex = _GetFocusedTabIndex();
                if (currentTabIndex.has_value())
                {
                    insertPosition = currentTabIndex.value() + 1;
                }
            }
        }

        // Add the new tab to the list of our tabs.
        _tabs.InsertAt(insertPosition, *newTabImpl);
        _mruTabs.Append(*newTabImpl);

        newTabImpl->SetDispatch(*_actionDispatch);
        newTabImpl->SetActionMap(_settings.ActionMap());

        // Give the tab its index in the _tabs vector so it can manage its own SwitchToTab command.
        _UpdateTabIndices();

        // Hookup our event handlers to the new terminal
        _RegisterTabEvents(*newTabImpl);

        // Don't capture a strong ref to the tab. If the tab is removed as this
        // is called, we don't really care anymore about handling the event.
        auto weakTab = make_weak(newTabImpl);

        // When the tab's active pane changes, we'll want to lookup a new icon
        // for it. The Title change will be propagated upwards through the tab's
        // PropertyChanged event handler.
        newTabImpl->ActivePaneChanged({ get_weak(), &TerminalPage::_activePaneChanged });

        // The RaiseVisualBell event has been bubbled up to here from the pane,
        // the next part of the chain is bubbling up to app logic, which will
        // forward it to app host.
        newTabImpl->TabRaiseVisualBell([weakTab, weakThis{ get_weak() }]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab)
            {
                page->RaiseVisualBell.raise(nullptr, nullptr);
            }
        });

        // When a tab requests a desktop toast notification, send the toast
        // and handle activation by summoning this window and switching to the tab.
        newTabImpl->TabToastNotificationRequested([weakThis{ get_weak() }, weakTab{ newTabImpl->get_weak() }](const winrt::hstring& title, const winrt::hstring& body, const winrt::TerminalApp::IPaneContent& content) {
            if (const auto page{ weakThis.get() })
            {
                if (const auto tab{ weakTab.get() })
                {
                    page->_SendDesktopNotification(title, body, tab, content);
                }
            }
        });

        auto tabViewItem = newTabImpl->TabViewItem();
        _tabView.TabItems().InsertAt(insertPosition, tabViewItem);
        // Agentmaster (tab sizing): give the new tab the cog's height / title font / width bounds before it
        // first lays out (a default-valued setting leaves the style's own value in place). The strip's first
        // layout pass with this tab is the settle at the end of this function (_ApplyTabDragPolicy).
        _ApplyTabSizingToTab(tabViewItem);
        // Agentmaster: [tab-new] timestamps the strip INSERTION exactly — the v0.6.7 MUX AV lived in
        // the insert->drag window, so the gap between this line and a [nav] tab-drag-begin is the
        // forensic measurement. Covers every tab kind (managed launches also log [spawn]/[rehome]).
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[tab-new] idx=" + std::to_wstring(insertPosition) + L" tabs=" + std::to_wstring(_tabs.Size()) + L" " + _DescribeTabForLog(*newTabImpl) + L"\n");

        // Set this tab's icon to the icon from the content
        _UpdateTabIcon(*newTabImpl);

        tabViewItem.PointerPressed({ this, &TerminalPage::_OnTabPointerPressed });

        // When the tab requests close, try to close it (prompt for approval, if required)
        newTabImpl->CloseRequested([weakTab, weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab)
            {
                page->_HandleCloseTabRequested(*tab);
            }
        });

        // When the tab is closed, remove it from our list of tabs.
        newTabImpl->Closed([weakTab, weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
            const auto page = weakThis.get();
            const auto tab = weakTab.get();

            if (page && tab)
            {
                page->_RemoveTab(*tab);
            }
        });

        // The tab might want us to toss focus into the control, especially when
        // transient UIs (like the context menu, or the renamer) are dismissed.
        newTabImpl->RequestFocusActiveControl([weakThis{ get_weak() }]() {
            if (const auto page{ weakThis.get() })
            {
                page->_FocusCurrentTab(false);
            }
        });

        // This kicks off TabView::SelectionChanged, in response to which
        // we'll attach the terminal's Xaml control to the Xaml root.
        // Agentmaster: a Sessions-page "bulk open" creates the tab in the BACKGROUND — skip the
        // selection so focus (and the tab-switch overlay-dismiss) stays put. The new tab's claude
        // then starts lazily on first focus (WT's background-tab behavior), exactly what bulk-open
        // wants. Every other caller leaves the flag false, so the normal foreground-select is intact.
        //
        // Agentmaster (tab-strip reveal): upstream's one SelectedItem call is three steps here, because the
        // strip's first layout pass with the new tab (the settle below) has to sit between selecting the tab
        // and switching to it (_holdTabSwitchForNewTab in TerminalPage.h has the full story):
        //   1. select the tab, with the switch HELD: MUX now counts the new tab as the selected one, but
        //      _OnTabSelectionChanged only records the switch;
        //   2. settle: the layout pass. MUX answers the strip's growth by scrolling its SELECTED tab into view,
        //      and that is the new tab now, not the tab the user was on — which, a few tabs away, is where the
        //      strip used to jump when a tab was opened from another tab's context menu (since 2026-10-05 the
        //      strip ignores MUX's request — _EnsureTabStripScrollViewer — so this order is a belt for a strip
        //      whose ScrollViewer is not bound yet);
        //   3. run the held switch: the content swap, focus, and the reveal of a tab the strip has now laid out
        //      (a tab opened on screen, beside the one that was right-clicked, keeps the strip where it is —
        //      the reveal moves it only to pull a tab sitting at the very edge inside its padding).
        // The switch runs only when step 1 actually raised one, as the event would have.
        bool switchHeld = false;
        if (!_openClaudeTabInBackground)
        {
            _holdTabSwitchForNewTab = true;
            _tabSwitchHeldForNewTab = false;
            // Release the hold even if the selection throws, or every later selection would be swallowed.
            auto releaseHold = wil::scope_exit([this]() noexcept {
                _holdTabSwitchForNewTab = false;
            });
            try
            {
                _tabView.SelectedItem(tabViewItem);
            }
            catch (...)
            {
                // Not swallowed: rethrown to the caller, exactly as upstream let a failed selection out of this
                // function. Only the settle below is pulled forward for this path — before the reorder it had
                // always run by this point, and the drag-AV belt needs the strip settled before the message
                // pump runs again, whatever happened here.
                _ApplyTabDragPolicy(tabViewItem);
                throw;
            }
            switchHeld = std::exchange(_tabSwitchHeldForNewTab, false);
        }

        // Agentmaster: settle the item->container mapping + pin the new tab CanDrag(false) — native
        // MUX drag is permanently OFF (the drag-AV resolution; see TerminalPage.AgentEngine.cpp). ⚠ This
        // is a whole layout pass, and it must stay AFTER step 1 above: run with the user's previous tab still
        // selected, it scrolls the strip to THAT tab wherever the strip still honors MUX's bring-into-view (see
        // the step list above).
        _ApplyTabDragPolicy(tabViewItem);

        if (switchHeld)
        {
            // Step 3: the switch exactly as the SelectionChanged event runs it — the handler reads the selected
            // tab off the TabView itself, and its event args are unused. Contained here: a step that throws used
            // to fail inside XAML's event dispatch, never in this function's callers, so it is logged ([wil] in
            // hooks.log, with the throw-site stack) and the tab stays as created — inserted, selected, the strip
            // settled.
            try
            {
                _OnTabSelectionChanged(_tabView, nullptr);
            }
            CATCH_LOG();
        }
    }

    // Method Description:
    // - Create a new tab using a specified pane as the root.
    // Arguments:
    // - pane: The pane to use as the root.
    // - insertPosition: Optional parameter to indicate the position of tab.
    TerminalApp::Tab TerminalPage::_CreateNewTabFromPane(std::shared_ptr<Pane> pane, uint32_t insertPosition)
    {
        if (pane)
        {
            auto newTabImpl = winrt::make_self<Tab>(pane);
            _InitializeTab(newTabImpl, insertPosition);
            return *newTabImpl;
        }
        return nullptr;
    }

    // Method Description:
    // - Get the icon of the currently focused terminal control, and set its
    //   tab's icon to that icon.
    // Arguments:
    // - tab: the Tab to update the title for.
    void TerminalPage::_UpdateTabIcon(Tab& tab)
    {
        if (const auto content{ tab.GetActiveContent() })
        {
            const auto& icon{ content.Icon() };
            const auto theme = _settings.GlobalSettings().CurrentTheme();
            auto iconStyle = (theme && theme.Tab()) ? theme.Tab().IconStyle() : IconStyle::Default;

            // Agentmaster: the cog's "Show icons on tabs" (AppSettings::showTabIcon, HIDDEN by default)
            // overrides the theme — when off, force Hidden so the profile icon is dropped entirely and
            // takes NO strip space, via the same IconStyle::Hidden path WT's own "tab.iconStyle":"hidden"
            // theme uses (Tab::UpdateIcon -> Icon({}) + IconSource(nullptr)). Applied to every tab
            // (incl. the pinned Manager tab); _UpdateAllTabIcons re-applies it live on a settings change.
            if (!_appSettings.showTabIcon)
            {
                iconStyle = IconStyle::Hidden;
            }

            tab.UpdateIcon(icon, iconStyle);
        }
    }

    // Agentmaster: re-apply the GLOBAL "Show icons on tabs" (AppSettings::showTabIcon) setting to EVERY
    // open tab live — the icon twin of _updateAllTabCloseButtons. _UpdateTabIcon reads the current
    // _appSettings.showTabIcon, so this just re-runs it per tab; Tab::UpdateIcon early-outs when the
    // (path, style) pair is unchanged, so a broadcast that didn't touch showTabIcon is a cheap no-op.
    // Called from the cog Save handler (this window) and _ApplyBroadcastSettings (other windows).
    void TerminalPage::_UpdateAllTabIcons()
    {
        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                _UpdateTabIcon(*tabImpl);
            }
        }
    }

    // Method Description:
    // - Handle changes to the tab width set by the user
    // - Agentmaster (tab sizing): the mode is the EFFECTIVE one — the cog's "Tab width" when set, else
    //   Terminal's own tabWidthMode (the cog's default "Windows Terminal setting" choice, under which this
    //   is upstream's line verbatim). The per-tab width bounds depend on the mode (MUX owns widths in
    //   Equal; Fit to title / Compact are bounded per tab), so a REAL change re-applies them — this also
    //   runs on a Terminal settings reload (_RefreshUIForSettingsReload), where Terminal's own mode may
    //   be what changed. An unchanged mode is a no-op, as before (MUX ignores a same-value write).
    void TerminalPage::_UpdateTabWidthMode()
    {
        const auto mode = _EffectiveTabWidthMode();
        if (_tabView.TabWidthMode() == mode)
        {
            return;
        }
        _tabView.TabWidthMode(mode);
        _ApplyTabSizingToAllTabs();
        if (mode == MUX::Controls::TabViewWidthMode::Equal)
        {
            _ClearTabStripListMaxWidth();
        }
    }

    // Method Description:
    // - Handle changes in tab layout.
    void TerminalPage::_UpdateTabView()
    {
        // The tab row should only be visible if:
        // - we're not in focus mode
        // - we're not in full screen, or the user has enabled fullscreen tabs
        // - there is more than one tab, or the user has chosen to always show tabs
        // - Agentmaster: or this window has an Agent Manager page — its Home button (the ONLY way to open
        //   the page) lives in the tab row, so the row may never auto-hide under it (with one tab, or none,
        //   and showTabsInTitlebar off, the upstream rule would strand the page). The old pinned Manager tab
        //   kept the count above 1 for the same effect.
        const auto isVisible = !_isInFocusMode &&
                               (!_isFullscreen || _showTabsFullscreen) &&
                               (_settings.GlobalSettings().ShowTabsInTitlebar() ||
                                (_tabs.Size() > 1) ||
                                _settings.GlobalSettings().AlwaysShowTabs() ||
                                static_cast<bool>(_managerPage));

        if (_tabView)
        {
            // collapse/show the tabs themselves
            _tabView.Visibility(isVisible ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_tabRow)
        {
            // collapse/show the row that the tabs are in.
            // NaN is the special value XAML uses for "Auto" sizing.
            _tabRow.Height(isVisible ? NAN : 0);
        }
    }

    // Method Description:
    // - Duplicates the current focused tab
    void TerminalPage::_DuplicateFocusedTab()
    {
        if (const auto activeTab{ _GetFocusedTabImpl() })
        {
            _DuplicateTab(*activeTab);
        }
    }

    // Method Description:
    // - Duplicates specified tab
    // Arguments:
    // - tab: tab to duplicate
    // - insertPosition: where to place the new tab. Agentmaster: -1 (the default) keeps the
    //   end/NewTabPosition behavior; a "Fork session" context-menu invoke passes clickedIndex+1 so
    //   the fork (managed agent or plain shell) lands directly next to the clicked tab.
    void TerminalPage::_DuplicateTab(const Tab& tab, uint32_t insertPosition)
    {
        // Agentmaster: a managed Claude tab must NOT be naively duplicated. WT's duplicate re-runs the
        // tab's commandline, which for a Claude session is either `claude --settings <f>` (a blank new
        // conversation — harmless but pointless) or, for a resumed tab, `claude --resume <id> …` — a
        // SECOND claude writing the SAME <id>.jsonl (two writers on one transcript -> corruption). Instead
        // FORK the conversation: `claude --resume <id> --fork-session` branches its history into a new,
        // independent session id with its own transcript (the source's is untouched), registered as a
        // normal managed session ("<title> (fork)", fresh Auto Testing). A source that was never prompted
        // has no transcript to fork -> fall back to a fresh session in the same dir. A managed Codex tab
        // forks the SAME way but via `codex fork <rolloutUuid>` (kind-aware, below) — the tab map is
        // agent-agnostic, so Codex tabs reach this seam too and must NOT take the Claude path.
        if (_sessionRegistry)
        {
            std::wstring sourceId;
            const auto* const self = &tab;
            for (const auto& [id, weakTab] : _claudeTabs)
            {
                if (const auto t = weakTab.get(); t && winrt::get_self<winrt::TerminalApp::implementation::Tab>(t) == self)
                {
                    sourceId = id;
                    break;
                }
            }
            if (!sourceId.empty())
            {
                // Fork the managed session (kind-aware: Claude `--resume <id> --fork-session`, Codex
                // `codex fork <rolloutUuid>`, both transcript/rollout-gated -> fresh) via the ONE shared
                // seam, also used by the Triage Board / Explorer-tree "Fork session" so the two can't
                // drift. ALWAYS return for a managed tab — it must never be naively duplicated (re-running
                // a `--resume <id>` commandline = two writers on one transcript -> corruption).
                _ForkManagedSessionById(sourceId, insertPosition);
                return;
            }
        }

        // Nav audit: a plain shell-tab duplicate (a managed tab took the fork path above, which logs
        // fork-managed-begin/done — this line covers the non-managed remainder).
        ::Agentmaster::LogNav(L"duplicate-tab " + _DescribeTabForLog(tab));
        try
        {
            // TODO: GH#5047 - We're duplicating the whole profile, which might
            // be a dangling reference to old settings.
            //
            // In the future, it may be preferable to just duplicate the
            // current control's live settings (which will include changes
            // made through VT).
            // Agentmaster: honor an explicit placement (a "Fork session" context-menu invoke passes
            // clickedIndex+1 so the duplicate lands next to the clicked tab); otherwise fall back to
            // the upstream default — end, or after the current tab when NewTabPosition says so.
            if (insertPosition == static_cast<uint32_t>(-1))
            {
                insertPosition = _tabs.Size();
                if (_settings.GlobalSettings().NewTabPosition() == NewTabPosition::AfterCurrentTab)
                {
                    insertPosition = tab.TabViewIndex() + 1;
                }
            }
            _CreateNewTabFromPane(_MakePane(nullptr, tab, nullptr), insertPosition);

            const auto runtimeTabText{ tab.GetTabText() };
            if (!runtimeTabText.empty())
            {
                if (auto newTab{ _GetFocusedTabImpl() })
                {
                    newTab->SetTabText(runtimeTabText);
                }
            }
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Exports the content of the Terminal Buffer inside the tab
    // Arguments:
    // - tab: tab to export
    safe_void_coroutine TerminalPage::_ExportTab(const Tab& tab, winrt::hstring filepath)
    {
        // This will be used to set up the file picker "filter", to select .txt
        // files by default.
        static constexpr COMDLG_FILTERSPEC supportedFileTypes[] = {
            { L"Text Files (*.txt)", L"*.txt" },
            { L"All Files (*.*)", L"*.*" }
        };
        // An arbitrary GUID to associate with all instances of this
        // dialog, so they all re-open in the same path as they were
        // open before:
        static constexpr winrt::guid clientGuidExportFile{ 0xF6AF20BB, 0x0800, 0x48E6, { 0xB0, 0x17, 0xA1, 0x4C, 0xD8, 0x73, 0xDD, 0x58 } };

        try
        {
            if (const auto control{ tab.GetActiveTerminalControl() })
            {
                auto path = filepath;

                if (path.empty())
                {
                    // GH#11356 - we can't use the UWP apis for writing the file,
                    // because they don't work elevated (shocker) So just use the
                    // shell32 file picker manually.
                    std::wstring filename{ tab.Title() };
                    filename = til::clean_filename(filename);
                    path = co_await SaveFilePicker(*_hostingHwnd, [filename = std::move(filename)](auto&& dialog) {
                        THROW_IF_FAILED(dialog->SetClientGuid(clientGuidExportFile));
                        try
                        {
                            // Default to the Downloads folder
                            auto folderShellItem{ winrt::capture<IShellItem>(&SHGetKnownFolderItem, FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr) };
                            dialog->SetDefaultFolder(folderShellItem.get());
                        }
                        CATCH_LOG(); // non-fatal
                        THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(supportedFileTypes), supportedFileTypes));
                        THROW_IF_FAILED(dialog->SetFileTypeIndex(1)); // the array is 1-indexed
                        THROW_IF_FAILED(dialog->SetDefaultExtension(L"txt"));

                        // Default to using the tab title as the file name
                        THROW_IF_FAILED(dialog->SetFileName((filename + L".txt").c_str()));
                    });
                }
                else
                {
                    // The file picker isn't going to give us paths with
                    // environment variables, but the user might have set one in
                    // the settings. Expand those here.

                    path = winrt::hstring{ wil::ExpandEnvironmentStringsW<std::wstring>(path.c_str()) };
                }

                if (!path.empty())
                {
                    const auto buffer = control.ReadEntireBuffer();
                    til::io::write_utf8_string_to_file_atomic(std::filesystem::path{ std::wstring_view{ path } }, til::u16u8(buffer));
                }
            }
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Record the configuration information of the last closed thing .
    // - Will occasionally prune the list so it doesn't grow infinitely.
    // Arguments:
    // - args: the list of actions to take to remake the pane/tab
    void TerminalPage::_AddPreviouslyClosedPaneOrTab(std::vector<ActionAndArgs>&& args)
    {
        // Just make sure we don't get infinitely large, but still
        // maintain a large replay buffer.
        if (const auto size = _previouslyClosedPanesAndTabs.size(); size > 150)
        {
            const auto it = _previouslyClosedPanesAndTabs.begin();
            // delete 50 at a time so that we don't have to do an erase
            // of the buffer every time when at capacity.
            _previouslyClosedPanesAndTabs.erase(it, it + (size - 100));
        }

        _previouslyClosedPanesAndTabs.emplace_back(args);
    }

    // Method Description:
    // - Removes the tab (both TerminalControl and XAML) after prompting for approval
    // Arguments:
    // - tab: the tab to remove
    // - skipConfirmClose: if true, skip the confirmOnClose check. Used when
    //   an aggregate confirmation has already been shown (i.e. close other tabs)
    winrt::Windows::Foundation::IAsyncAction TerminalPage::_HandleCloseTabRequested(winrt::TerminalApp::Tab tab, bool skipConfirmClose)
    {
        // Agentmaster: a Claude session tab ARCHIVES (shut down + keep restorable) rather than a
        // plain close. This is the single seam shared by clicking the tab's X, the Manager's
        // Delete/Archive, the tree Del key, and the Auto-Testing "Archive" button.
        // _ArchiveAndCloseClaudeTab shows the one consequence confirm, does the archive
        // bookkeeping (live=false, clear injector, drop the _claudeTabs entry, persist), then
        // closes. It erases the id from _claudeTabs FIRST, so the close it triggers (-> _RemoveTab)
        // is just a normal teardown and won't re-enter this branch.
        if (const auto archiveId = _ClaudeSessionForTab(tab); !archiveId.empty())
        {
            co_await _ArchiveAndCloseClaudeTab(tab, archiveId, skipConfirmClose);
            co_return;
        }

        winrt::com_ptr<TerminalPage> strong;

        if (tab.ReadOnly())
        {
            const auto weak = get_weak();

            auto warningResult = co_await _ShowCloseReadOnlyDialog();

            strong = weak.get();

            // If the user didn't explicitly click on close tab - leave
            if (!strong || warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        // Agentmaster: a deliberate single-tab close (the tab's X, middle-click, the context-menu
        // "Close", or the closeTab action) MUST get the user's explicit agreement first — even for a
        // plain shell tab with NO managed session (a Claude/Codex session tab already took the
        // archive-confirm branch above). Upstream only prompts per the ConfirmOnClose setting, whose
        // default (Automatic) closes a single-pane tab SILENTLY (_ShouldWarnOnCloseTab => false), so a
        // stray click discards a tab with no prompt. We therefore ALWAYS confirm here, with a
        // buttons-only ContentDialog — and deliberately NOT the shared _ShowConfirmCloseDialog: that
        // one carries a "don't ask again" checkbox that flips ConfirmOnClose to Never, which would
        // silently defeat this guarantee. Buttons-only also keeps it XAML-Islands-safe (a text/input
        // child in a ContentDialog gets no keypresses on an island). skipConfirmClose is still honored
        // so an aggregate close (_RemoveTabs) that already showed its own confirmation isn't
        // double-prompted; window-close / quit tear down without routing through here.
        if (!skipConfirmClose)
        {
            if (const auto presenter{ _dialogPresenter.get() })
            {
                const std::wstring tabTitle{ tab.Title() };

                ContentDialog dialog;
                dialog.Title(winrt::box_value(L"Close tab?"));
                dialog.Content(winrt::box_value(tabTitle.empty() ?
                                                    winrt::hstring{ L"This tab will be closed." } :
                                                    winrt::hstring{ L"“" + tabTitle + L"” will be closed." }));
                dialog.PrimaryButtonText(L"Close");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel

                const auto weak = get_weak();
                const auto result = co_await presenter.ShowDialog(dialog);
                strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
                if (!strong || result != ContentDialogResult::Primary)
                {
                    co_return; // cancelled / torn down -> leave the tab open
                }
            }
            // No presenter to confirm with -> fall through and close (we can't prompt; the tab is
            // recoverable via "reopen closed tab" — _AddPreviouslyClosedPaneOrTab below).
        }

        auto t = winrt::get_self<implementation::Tab>(tab);
        auto actions = t->BuildStartupActions(BuildStartupKind::None);
        _AddPreviouslyClosedPaneOrTab(std::move(actions));

        tab.Close();
    }

    // Removes the tab (both TerminalControl and XAML).
    // NOTE: Don't call this directly, but rather `tab.Close()`.
    // Agentmaster: when the removed tab was the FOCUSED one, the successor is chosen so that selecting
    // it starts nothing (_PickCloseSuccessorTab) and then gets the same Agentmaster selection effects a
    // normal tab switch runs (_ApplyAgentTabSelection) — the selection funnel is suppressed during a
    // removal. When no tab is left, or every remaining tab would start a process on being shown, the
    // Agent Manager page is shown instead (it took over the "never wake anything" harbor the pinned
    // Manager tab used to be) and the window stays open — the debounced Manager-only check then closes
    // it unless it is the last Agentmaster window. A removal while the page is already up keeps the page
    // and re-clears the selection MUX puts back on a neighbor. Parameters: tab — the tab being removed
    // (already gone from the strip => no-op). Returns nothing. Exceptions: the Agentmaster effects are
    // logged and swallowed (CATCH_LOG / their own guards); the upstream strip/collection mutations throw
    // only as they always did.
    void TerminalPage::_RemoveTab(const winrt::TerminalApp::Tab& tab)
    {
        uint32_t tabIndex{};
        if (!_tabs.IndexOf(tab, tabIndex))
        {
            // The tab is already removed
            return;
        }

        // We use _removing flag to suppress _OnTabSelectionChanged events
        // that might get triggered while removing
        _removing = true;
        auto unsetRemoving = wil::scope_exit([&]() noexcept { _removing = false; });

        const auto focusedTabIndex{ _GetFocusedTabIndex() };

        // Removing the tab from the collection should destroy its control and disconnect its connection,
        // but it doesn't always do so. The UI tree may still be holding the control and preventing its destruction.
        tab.Shutdown();

        uint32_t mruIndex{};
        if (_mruTabs.IndexOf(tab, mruIndex))
        {
            _mruTabs.RemoveAt(mruIndex);
        }

        if (tab == _settingsTab)
        {
            _settingsTab = nullptr;
        }

        if (_stashed.draggedTab && *_stashed.draggedTab == tab)
        {
            _stashed.draggedTab = nullptr;
        }

        _tabs.RemoveAt(tabIndex);
        _tabView.TabItems().RemoveAt(tabIndex);
        _UpdateTabIndices();
        _SettleTabStripLayout(); // Agentmaster: a remove invalidates the item->container map too (MUX drag-start AV guard)

        // To close the window here, we need to close the hosting window.
        if (_tabs.Size() == 0)
        {
            if (_managerPage)
            {
                // Agentmaster: an Agentmaster window never closes because its last tab did — it falls back
                // to its Agent Manager page, exactly as it used to fall back to the pinned Manager tab (which
                // could not be closed). The debounced Manager-only check (_tabs.VectorChanged) then decides:
                // self-close if another Agentmaster window remains, else keep this last window open with its
                // record discarded. Skipped once the window's own teardown began (the close is under way).
                if (!_windowRecordTeardownFlushed)
                {
                    _ShowManagerPage(L"last tab closed");
                }
            }
            else
            {
                // If we are supposed to save state, make sure we clear it out
                // if the user manually closed all tabs.
                // Do this only if we are the last window; the monarch will notice
                // we are missing and remove us that way otherwise.
                CloseWindowRequested.raise(*this, nullptr);
            }
        }
        else if (focusedTabIndex.has_value() && focusedTabIndex.value() == gsl::narrow_cast<uint32_t>(tabIndex))
        {
            // Manually select the new tab to get focus, rather than relying on TabView since:
            // 1. We want to customize this behavior (e.g., use MRU logic)
            // 2. In fullscreen (GH#5799) and focus (GH#7916) modes the _OnTabItemsChanged is not fired
            // 3. When rearranging tabs (GH#7916) _OnTabItemsChanged is suppressed
            const auto tabSwitchMode = _settings.GlobalSettings().TabSwitcherMode();
            // Agentmaster: the successor picked below, kept for the Agentmaster selection effects after the
            // branches (see the block following them).
            winrt::TerminalApp::Tab agentCloseSuccessor{ nullptr };
            // Agentmaster (FAVORITES.md §4a): set when even the picked successor would START a process on
            // being shown (every remaining tab is dormant) — the Agent Manager page is shown instead, so a
            // close never wakes another session (the pinned Manager tab used to be that safe harbor).
            bool agentShowManagerInstead = false;

            if (tabSwitchMode == TabSwitcherMode::MostRecentlyUsed)
            {
                // Agentmaster (FAVORITES.md §4a): never WAKE a dormant tab as a side effect of a close —
                // if the MRU head would start its claude.exe/shell on being shown, focus the most recent
                // tab that starts nothing instead (or the Agent Manager page when there is none).
                const auto newSelectedTab = _PickCloseSuccessorTab(_mruTabs.GetAt(0), tabIndex, /*mruMode*/ true);
                if (_managerPage && _TabSelectionWouldStartProcess(newSelectedTab))
                {
                    agentShowManagerInstead = true;
                }
                else
                {
                    _UpdatedSelectedTab(newSelectedTab);
                    _tabView.SelectedItem(newSelectedTab.TabViewItem());
                    agentCloseSuccessor = newSelectedTab;
                }
            }
            else
            {
                // We can't use
                //   auto selectedIndex = _tabView.SelectedIndex();
                // Because this will always return -1 in this scenario unfortunately.
                //
                // So, what we're going to try to do is move the focus to the tab
                // to the right, within the bounds of how many tabs we have.
                //
                // EX: we have 4 tabs: [A, B, C, D]. If we close:
                // * A (tabIndex=0): We'll want to focus tab B (now in index 0)
                // * B (tabIndex=1): We'll want to focus tab C (now in index 1)
                // * C (tabIndex=2): We'll want to focus tab D (now in index 2)
                // * D (tabIndex=3): We'll want to focus tab C (now in index 2)
                const auto newSelectedIndex = std::clamp<int32_t>(tabIndex, 0, _tabs.Size() - 1);
                // _UpdatedSelectedTab will do the work of setting up the new tab as
                // the focused one, and unfocusing all the others.
                // Agentmaster (FAVORITES.md §4a): ...unless that neighbor is DORMANT — showing it would
                // start its claude.exe (closing one session used to silently start the next). Then the
                // nearest tab that starts nothing takes focus — or, when every remaining tab is dormant,
                // the Agent Manager page (which starts nothing).
                auto newSelectedTab{ _PickCloseSuccessorTab(_tabs.GetAt(newSelectedIndex), tabIndex, /*mruMode*/ false) };
                if (_managerPage && _TabSelectionWouldStartProcess(newSelectedTab))
                {
                    agentShowManagerInstead = true;
                }
                else
                {
                    _UpdatedSelectedTab(newSelectedTab);

                    // Also, we need to _manually_ set the SelectedItem of the tabView
                    // here. If we don't, then the TabView will technically not have a
                    // selected item at all, which can make things like ClosePane not
                    // work correctly.
                    _tabView.SelectedItem(newSelectedTab.TabViewItem());
                    agentCloseSuccessor = newSelectedTab;
                }
            }

            // Agentmaster: the successor is now the selected tab, but _OnTabSelectionChanged skipped the
            // switch (the _removing guard), so run the same Agentmaster effects a normal switch gets —
            // _ApplyAgentTabSelection's one shared body — plus the funnel's two tail steps. Without them,
            // the Home / Jump Back buttons were computed while the closed tab still held the selection, the
            // successor's overlay never learned it was focused, and the Linked-Lenses selection kept
            // pointing at the closed session. Skipped once the window's teardown has begun (the
            // deterministic close flush already captured the record, and the window is going away).
            // Wrapped so a failure here is logged and can never abort this close's own remaining bookkeeping.
            if (agentCloseSuccessor && !_windowRecordTeardownFlushed)
            {
                try
                {
                    _ApplyAgentTabSelection(agentCloseSuccessor, nullptr);
                    _ScheduleWindowRecordSave();
                    _UpdateManagerNavButtons();
                }
                CATCH_LOG();
            }
            // Agentmaster (FAVORITES.md §4a): every remaining tab would start a process — show the Agent
            // Manager page rather than wake one. Its show clears the neighbor MUX auto-selected during the
            // removal above (never swapped in: the funnel was suppressed, so nothing laid it out or started
            // it) and runs the page's own selection effects + record save.
            else if (agentShowManagerInstead && !_windowRecordTeardownFlushed)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[close-successor] every remaining tab would start a process on being shown \x2014 showing the Agent Manager instead of waking one\n");
                _ShowManagerPage(L"close: every remaining tab is dormant");
            }
        }
        else if (_managerPageVisible)
        {
            // Agentmaster: a tab closed while the Agent Manager page is on screen (its board's Close, a tab's
            // X, a tear-out). The page stays, but MUX re-selected a neighbor when the item left the
            // selection-less strip — clear it again so no tab reads as the active one under the page. The
            // _removing guard keeps the resulting SelectionChanged from running the switch.
            _DeselectTabStripForManagerPage();
        }

        // GH#5559 - If we were in the middle of a drag/drop, end it by clearing
        // out our state.
        if (_rearranging)
        {
            _rearranging = false;
            _rearrangeFrom = std::nullopt;
            _rearrangeTo = std::nullopt;
        }
    }

    // Method Description:
    // - Sets focus to the tab to the right or left the currently selected tab.
    // - Agentmaster: an Agentmaster window can have NO tabs (its Agent Manager page is what shows then), so
    //   an empty strip is a no-op instead of the modulo-by-zero below. From the Agent Manager page (no tab
    //   focused) the keystroke lands DIRECTLY on a tab without opening the tab switcher (whose live preview
    //   would first switch to — and possibly wake — its start item): in-order / disabled modes land "next"
    //   on the FIRST tab and "previous" on the LAST (the page sits before tab 0, the way the pinned Manager
    //   tab did); most-recently-used mode lands "next" on the tab you left to open the page and "previous"
    //   on the least recently used one. The cycle never lands back ON the page: the Home button is the one
    //   way to open it.
    void TerminalPage::_SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode)
    {
        if (_tabs.Size() == 0)
        {
            return;
        }
        const auto index{ _GetFocusedTabIndex().value_or(0) };
        const auto tabSwitchMode = customTabSwitcherMode ? customTabSwitcherMode.Value() : _settings.GlobalSettings().TabSwitcherMode();
        // Agentmaster: from the Agent Manager page there is no focused tab, so there is no "current" item for
        // the tab switcher to start from. Opening it anyway would be wrong in a way the user can feel: the
        // switcher SELECTS its start item the moment it becomes visible (its SelectionChanged raises
        // SwitchToTabRequested — the live preview), so the keystroke would first switch to tab 0 (or the MRU
        // head) and only then step once more — landing one tab too far, and starting tab 0's claude.exe on the
        // way if it was dormant (FAVORITES.md §4a: nothing may wake as a side effect). Land directly instead:
        //  - most-recently-used mode: "next" = the most recently used tab (the one you left to open the page —
        //    the page itself is never in the MRU list), "previous" = the least recently used one;
        //  - in-order and disabled modes: "next" = the first tab, "previous" = the last one (the page sits
        //    before tab 0, where the pinned Manager tab used to be).
        // The page is never a stop of the cycle: the Home button is the one way to open it.
        if (_managerPageVisible)
        {
            if (tabSwitchMode == TabSwitcherMode::MostRecentlyUsed && _mruTabs.Size() > 0)
            {
                const auto target = _mruTabs.GetAt(bMoveRight ? 0u : _mruTabs.Size() - 1);
                uint32_t targetIndex{};
                if (_tabs.IndexOf(target, targetIndex))
                {
                    _SelectTab(targetIndex);
                    return;
                }
            }
            _SelectTab(bMoveRight ? 0u : _tabs.Size() - 1);
            return;
        }
        if (tabSwitchMode == TabSwitcherMode::Disabled)
        {
            auto tabCount = _tabs.Size();
            // Wraparound math. By adding tabCount and then calculating
            // modulo tabCount, we clamp the values to the range [0,
            // tabCount) while still supporting moving leftward from 0 to
            // tabCount - 1.
            const auto newTabIndex = ((tabCount + index + (bMoveRight ? 1 : -1)) % tabCount);
            _SelectTab(newTabIndex);
        }
        else
        {
            const auto p = LoadCommandPalette();
            p.SetTabs(_tabs, _mruTabs);

            // Otherwise, set up the tab switcher in the selected mode, with
            // the given ordering, and make it visible.
            p.EnableTabSwitcherMode(index, tabSwitchMode);
            p.Visibility(Visibility::Visible);
            p.SelectNextItem(bMoveRight);
        }
    }

    // Method Description:
    // - Sets focus to the desired tab. Returns false if the provided tabIndex
    //   is greater than the number of tabs we have.
    // - During startup, we'll immediately set the selected tab as focused.
    // - After startup, we'll dispatch an async method to set the selected
    //   item of the TabView, which will then also trigger a
    //   TabView::SelectionChanged, handled in
    //   TerminalPage::_OnTabSelectionChanged
    // Return Value:
    // true iff we were able to select that tab index, false otherwise
    // - Agentmaster: false on an EMPTY strip (an Agentmaster window lives on with no tabs — its Agent
    //   Manager page shows — where upstream's clamp would underflow to UINT_MAX and GetAt would throw). A
    //   selection made while the Agent Manager page is up leaves the page (see
    //   _LeaveManagerPageOntoSelectedTab for the already-selected case the funnel can't see).
    bool TerminalPage::_SelectTab(uint32_t tabIndex)
    {
        if (_tabs.Size() == 0)
        {
            return false;
        }
        // GH#9369 - if the argument is out of range, then clamp to the number
        // of available tabs. Previously, we'd just silently do nothing if the
        // value was greater than the number of tabs.
        tabIndex = std::clamp(tabIndex, 0u, _tabs.Size() - 1);

        auto tab{ _tabs.GetAt(tabIndex) };
        // Agentmaster: on the Agent Manager page with MUX still holding THIS tab selected under it, the
        // SelectedItem below changes nothing and raises no SelectionChanged — switch explicitly.
        if (_LeaveManagerPageOntoSelectedTab(tab))
        {
            return true;
        }
        // GH#11107 - Always just set the item directly first so that if
        // tab movement is done as part of multiple actions following calls
        // to _GetFocusedTab will return the correct tab.
        _tabView.SelectedItem(tab.TabViewItem());

        if (_startupState == StartupState::InStartup)
        {
            _UpdatedSelectedTab(tab);
        }
        else
        {
            _SetFocusedTab(tab);
        }

        return true;
    }

    // Method Description:
    // - This method is called once a tab was selected in tab switcher
    //   We'll use this event to select the relevant tab
    // Arguments:
    // - tab - tab to select
    // Return Value:
    // - <none>
    void TerminalPage::_OnSwitchToTabRequested(const IInspectable& /*sender*/, const winrt::TerminalApp::Tab& tab)
    {
        uint32_t index{};
        if (_tabs.IndexOf(tab, index))
        {
            _SelectTab(index);
        }
    }

    // Method Description:
    // - Returns the index in our list of tabs of the currently focused tab. If
    //      no tab is currently selected, returns nullopt.
    // Return Value:
    // - the index of the currently focused tab if there is one, else nullopt
    std::optional<uint32_t> TerminalPage::_GetFocusedTabIndex() const noexcept
    {
        // Agentmaster: while the Agent Manager page is on screen NO tab is focused — the page is. The strip
        // is deselected for it, but MUX can quietly re-select a neighbor under the page (item removal on a
        // selection-less strip), so the page flag, not the TabView, is the authority here: every
        // tab-targeting action (close, split, rename, find, move, ...) then no-ops instead of acting on a
        // tab the user can't see.
        if (_managerPageVisible)
        {
            return std::nullopt;
        }
        // GH#1117: This is a workaround because _tabView.SelectedIndex()
        //          sometimes return incorrect result after removing some tabs
        uint32_t focusedIndex;
        if (_tabView.TabItems().IndexOf(_tabView.SelectedItem(), focusedIndex))
        {
            return focusedIndex;
        }
        return std::nullopt;
    }

    // Method Description:
    // - Returns the index in our list of tabs of the currently focused tab. If
    //      no tab is currently selected, returns nullopt.
    // Return Value:
    // - the index of the currently focused tab if there is one, else nullopt
    std::optional<uint32_t> TerminalPage::_GetTabIndex(const TerminalApp::Tab& tab) const noexcept
    {
        uint32_t i;
        if (_tabs.IndexOf(tab, i))
        {
            return i;
        }
        return std::nullopt;
    }

    // Method Description:
    // - returns the currently focused tab. This might return null,
    //   so make sure to check the result!
    winrt::TerminalApp::Tab TerminalPage::_GetFocusedTab() const noexcept
    {
        if (auto index{ _GetFocusedTabIndex() })
        {
            return _tabs.GetAt(*index);
        }
        return nullptr;
    }

    // Method Description:
    // - returns a com_ptr to the currently focused tab implementation. This might return null,
    //   so make sure to check the result!
    winrt::com_ptr<Tab> TerminalPage::_GetFocusedTabImpl() const noexcept
    {
        if (auto tab{ _GetFocusedTab() })
        {
            return _GetTabImpl(tab);
        }
        return nullptr;
    }

    // Method Description:
    // - returns a tab corresponding to a view item. This might return null,
    //   so make sure to check the result!
    winrt::TerminalApp::Tab TerminalPage::_GetTabByTabViewItem(const IInspectable& tabViewItem) const noexcept
    {
        uint32_t tabIndexFromControl{};
        const auto items{ _tabView.TabItems() };
        if (items.IndexOf(tabViewItem, tabIndexFromControl) && tabIndexFromControl < _tabs.Size())
        {
            // If IndexOf returns true, we've actually got an index
            return _tabs.GetAt(tabIndexFromControl);
        }
        return nullptr;
    }

    // Method Description:
    // - An async method for changing the focused tab on the UI thread. This
    //   method will _only_ set the selected item of the TabView, which will
    //   then also trigger a TabView::SelectionChanged event, which we'll handle
    //   in TerminalPage::_OnTabSelectionChanged, where we'll mark the new tab
    //   as focused.
    // Arguments:
    // - tab: tab to focus.
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::_SetFocusedTab(const winrt::TerminalApp::Tab tab)
    {
        // GH#1117: This is a workaround because _tabView.SelectedIndex(tabIndex)
        //          sometimes set focus to an incorrect tab after removing some tabs
        auto weakThis{ get_weak() };

        if (!_tabView.Dispatcher().HasThreadAccess())
        {
            co_await winrt::resume_foreground(_tabView.Dispatcher());
        }

        if (auto page{ weakThis.get() })
        {
            // Make sure the tab was not removed
            uint32_t tabIndex{};
            if (_tabs.IndexOf(tab, tabIndex))
            {
                _tabView.SelectedItem(tab.TabViewItem());
            }
        }
    }

    // Method Description:
    // - Disables read-only mode on pane if the user wishes to close it and read-only mode is enabled.
    // Arguments:
    // - pane: the pane that is about to be closed.
    // Return Value:
    // - bool indicating whether the (read-only) pane can be closed.
    winrt::Windows::Foundation::IAsyncOperation<bool> TerminalPage::_PaneConfirmCloseReadOnly(std::shared_ptr<Pane> pane)
    {
        if (pane->ContainsReadOnly())
        {
            const auto weak = get_weak();

            auto warningResult = co_await _ShowCloseReadOnlyDialog();

            const auto strong = weak.get();

            // If the user didn't explicitly click on close tab - leave
            if (!strong || warningResult != ContentDialogResult::Primary)
            {
                co_return false;
            }

            // Clean read-only mode to prevent additional prompt if closing the pane triggers closing of a hosting tab
            pane->WalkTree([](const auto& p) {
                if (const auto control{ p->GetTerminalControl() })
                {
                    if (control.ReadOnly())
                    {
                        control.ToggleReadOnly();
                    }
                }
            });
        }
        co_return true;
    }

    // Method Description:
    // - Removes the pane from the tab it belongs to.
    // Arguments:
    // - pane: the pane to close.
    void TerminalPage::_HandleClosePaneRequested(std::shared_ptr<Pane> pane)
    {
        // Agentmaster: closing a PANE (closePane / closeOtherPanes) bypasses the tab-X archive seam
        // (_HandleCloseTabRequested -> _ArchiveAndCloseClaudeTab). If the pane being closed hosts a
        // managed Claude session, archive it HERE so it doesn't linger live=true: the per-tab liveness
        // sweep only fires once THIS session's connection is gone/Closed (a ~2s lag), and a SINGLE-pane
        // Claude tab closed via closePane is removed before the sweep can ever see it (-> a permanent
        // phantom card). Mirror _ArchiveAndCloseClaudeTab's bookkeeping minus the dialog + tab.Close()
        // (the pane->Close() below tears the pane + connection down -> claude.exe exits). Matched by
        // connection identity, so closing a shell SIBLING of a Claude pane (its WT_SESSION != any
        // session's tabToken) is a no-op — only the actual Claude pane archives.
        if (_sessionRegistry && pane)
        {
            std::vector<std::wstring> closing;
            pane->WalkTree([&](auto&& p) {
                if (const auto ctrl = p->GetTerminalControl())
                {
                    const auto id = _ClaudeSessionForConnection(ctrl.Connection());
                    if (!id.empty())
                    {
                        closing.push_back(id);
                    }
                }
            });
            for (const auto& id : closing)
            {
                _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                    s.live = false;
                });
                _sessionRegistry->SetInjector(id, nullptr);
                _sessionRegistry->SetPromptSubmitter(id, nullptr); // PENDING_INPUT.md §9 — same lifetime as the injector
                _claudeTabs.erase(id);
                _claudeOverlays.erase(id);
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[pane-close] archived " + id + L"\n");
            }
            if (!closing.empty())
            {
                ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
            }
        }

        // Build the list of actions to recreate the closed pane,
        // BuildStartupActions returns the "first" pane and the rest of
        // its actions are assuming that first pane has been created first.
        // This doesn't handle refocusing anything in particular, the
        // result will be that the last pane created is focused. In the
        // case of a single pane that is the desired behavior anyways.
        auto state = pane->BuildStartupActions(0, 1, BuildStartupKind::None);
        {
            ActionAndArgs splitPaneAction{};
            splitPaneAction.Action(ShortcutAction::SplitPane);
            SplitPaneArgs splitPaneArgs{ SplitDirection::Automatic, state.firstPane->GetTerminalArgsForPane(BuildStartupKind::None) };
            splitPaneAction.Args(splitPaneArgs);

            state.args.emplace(state.args.begin(), std::move(splitPaneAction));
        }
        _AddPreviouslyClosedPaneOrTab(std::move(state.args));

        // If specified, detach before closing to directly update the pane structure
        pane->Close();
    }

    // Method Description:
    // - Close the currently focused pane. If the pane is the last pane in the
    //   tab, the tab will also be closed. This will happen when we handle the
    //   tab's Closed event.
    safe_void_coroutine TerminalPage::_CloseFocusedPane()
    {
        if (const auto activeTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();

            if (const auto pane{ activeTab->GetActivePane() })
            {
                const auto weak = get_weak();

                // Check if we should warn before closing a single pane
                // (only triggers on Always — Automatic doesn't warn for single pane)
                const auto setting = _settings.GlobalSettings().ConfirmOnClose();
                if (setting == ConfirmOnClose::Always)
                {
                    // If this is the last pane, closing it closes the tab,
                    // so use the tab dialog text instead.
                    const auto kind = activeTab->GetLeafPaneCount() == 1 ? ConfirmCloseDialogKind::Tab : ConfirmCloseDialogKind::Pane;
                    auto warningResult = co_await _ShowConfirmCloseDialog(kind);

                    // Hold a strong reference to `this` for the rest of the
                    // method; we may be the last holder after `co_await`.
                    auto strong = weak.get();
                    if (!strong || warningResult != ContentDialogResult::Primary)
                    {
                        co_return;
                    }
                }

                if (co_await _PaneConfirmCloseReadOnly(pane))
                {
                    if (const auto strong = weak.get())
                    {
                        _HandleClosePaneRequested(pane);
                    }
                }
            }
        }
    }

    // Method Description:
    // - Close all panes with the given IDs sequentially.
    // - Shows a single aggregate confirmation dialog upfront if the confirmOnClose setting warrants it.
    // Arguments:
    // - weakTab: weak reference to the tab that the panes belong to.
    // - paneIds: collection of the IDs of the panes that are marked for removal.
    safe_void_coroutine TerminalPage::_ClosePanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds)
    {
        // Show a single aggregate confirmation for closing multiple panes.
        if (_settings.GlobalSettings().ConfirmOnClose() != ConfirmOnClose::Never)
        {
            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::MultiplePanes);

            // Hold a strong reference to `this` after the co_await; we may
            // be the last holder if the page was being torn down.
            auto strong = weak.get();
            if (!strong || warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }
        _CloseRemainingPanes(weakTab, std::move(paneIds));
    }

    // Method Description:
    // - Recursively closes panes by ID, chaining each close via the
    //   ClosedByParent callback. Called after confirmation has already
    //   been handled by _ClosePanes.
    // Arguments:
    // - weakTab: weak reference to the tab that the panes belong to
    // - paneIds: remaining pane IDs to close
    void TerminalPage::_CloseRemainingPanes(weak_ref<Tab> weakTab, std::vector<uint32_t> paneIds)
    {
        if (auto strongTab{ weakTab.get() })
        {
            // Close all unfocused panes one by one
            while (!paneIds.empty())
            {
                const auto id = paneIds.back();
                paneIds.pop_back();

                if (const auto pane{ strongTab->GetRootPane()->FindPane(id) })
                {
                    pane->ClosedByParent([ids{ std::move(paneIds) }, weakThis{ get_weak() }, weakTab]() {
                        if (auto strongThis{ weakThis.get() })
                        {
                            strongThis->_CloseRemainingPanes(weakTab, std::move(ids));
                        }
                    });
                    // Close the pane which will eventually trigger the closed by parent event
                    _HandleClosePaneRequested(pane);
                    break;
                }
            }
        }
    }

    // Method Description:
    // - Close the tab at the given index.
    void TerminalPage::_CloseTabAtIndex(uint32_t index)
    {
        if (index >= _tabs.Size())
        {
            return;
        }
        if (auto tab{ _tabs.GetAt(index) })
        {
            _HandleCloseTabRequested(tab);
        }
    }

    // Agentmaster: close every tab to the LEFT of the given tab — the left-hand twin of the
    // upstream "Close tabs to the right" (CloseTabsAfter), which shipped without a mirror.
    // Driven by the tab context-menu "Close > Close tabs to the left" item (the
    // CloseTabsBeforeRequested event). Snapshots [0, index) and hands it to _RemoveTabs, which
    // owns the aggregate close confirmation, the per-session archive bookkeeping, AND the
    // Manager-tab skip — the pinned Manager tab sits at index 0 (to the left of everything) and
    // must never be closed, so it is filtered out there.
    void TerminalPage::_CloseTabsBefore(const winrt::TerminalApp::Tab& tab)
    {
        uint32_t index{};
        if (!_tabs.IndexOf(tab, index) || index == 0)
        {
            return;
        }

        // Since _RemoveTabs is asynchronous, create a snapshot of the tabs we want to remove.
        std::vector<winrt::TerminalApp::Tab> tabsToRemove;
        std::copy(begin(_tabs), begin(_tabs) + index, std::back_inserter(tabsToRemove));
        _RemoveTabs(tabsToRemove);
    }

    // Agentmaster: close EVERY tab in this window — the whole-window close driven by the tab
    // context-menu "Close > Close all tabs" / "★ Favorite & close all tabs" items. Snapshots all tabs
    // and hands them to _RemoveTabs, which owns the aggregate close confirmation and the per-session
    // archive bookkeeping. The window survives the empty strip on its Agent Manager page (_RemoveTab's
    // last-tab fallback). favoriteFirst forwards to _RemoveTabs(forceFavorite): it stars every managed session
    // before archiving and confirms with a 2-button "★ Favorite & Close All / Cancel All" (the favorite
    // is already chosen), vs the neutral 3-way dialog for a plain close-all.
    void TerminalPage::_CloseAllTabs(const bool favoriteFirst)
    {
        std::vector<winrt::TerminalApp::Tab> tabsToRemove;
        std::copy(begin(_tabs), end(_tabs), std::back_inserter(tabsToRemove));
        _RemoveTabs(tabsToRemove, favoriteFirst);
    }

    // Agentmaster (FAVORITES.md): does this window currently host at least one managed Claude/Codex
    // session? Gates the visibility of the "★ Favorite & close all tabs" close-submenu item (there is
    // nothing to favorite when only shell tabs are open). _claudeTabs maps sessionId -> the hosting tab
    // for THIS window and entries are erased on archive/close/liveness, so a single live weak ref means
    // a managed session is open here.
    bool TerminalPage::_WindowHasManagedSession() const
    {
        for (const auto& kv : _claudeTabs)
        {
            if (kv.second.get())
            {
                return true;
            }
        }
        return false;
    }

    // Method Description:
    // - Closes provided tabs one by one
    // - Shows a single aggregate confirmation dialog upfront if the confirmOnClose setting warrants it.
    // Arguments:
    // - tabs - tabs to remove
    // - forceFavorite - Agentmaster (FAVORITES.md): the caller already chose "★ Favorite & Close All"
    //   (the "★ Favorite & close all tabs" menu item), so pre-commit the favorite disposition and show a
    //   2-button confirm instead of re-offering it. Defaulted false for every existing caller.
    safe_void_coroutine TerminalPage::_RemoveTabs(const std::vector<winrt::TerminalApp::Tab> tabs, const bool forceFavorite)
    {
        // Agentmaster: every tab in the batch is closable — the pinned Manager tab this chokepoint used to
        // filter out is gone (the Manager is a page behind the Home button, never in _tabs). The copy keeps
        // the name the confirm + close loop below read.
        const std::vector<winrt::TerminalApp::Tab> closable{ tabs };

        if (closable.empty())
        {
            co_return;
        }

        const auto weak = get_weak();

        // Agentmaster (FAVORITES.md): a bulk close that includes managed agent sessions asks ONCE for
        // the whole batch — Close All / Cancel All — instead of silently closing every session or
        // walking a train of per-tab confirms. Close All keeps each session resumable in the Sessions
        // browser (with its Auto Testing) — nothing on disk is deleted (always archive, never delete);
        // Cancel All stops the close entirely. A batch of only plain shell tabs keeps upstream's single
        // generic confirm (gated on ConfirmOnClose). The decision is then applied to each tab below
        // WITHOUT re-prompting (skipConfirm).
        size_t managedCount = 0;
        for (const auto& tab : closable)
        {
            if (!_ClaudeSessionForTab(tab).empty())
            {
                ++managedCount;
            }
        }

        bool favoriteAll = forceFavorite; // FAVORITES.md: set when the batch dialog's "Favorite & Close All" is chosen — OR pre-committed by the "★ Favorite & close all tabs" menu item (forceFavorite)
        if (managedCount > 0)
        {
            if (const auto presenter{ _dialogPresenter.get() })
            {
                const auto shellCount = closable.size() - managedCount;
                std::wstring body = std::to_wstring(managedCount) + (managedCount == 1 ? L" session" : L" sessions");
                if (shellCount > 0)
                {
                    body += L" and ";
                    body += std::to_wstring(shellCount);
                    body += (shellCount == 1 ? L" other tab" : L" other tabs");
                }
                // FAVORITES.md: Close always archives (keep the record); there is no Delete All. The
                // sessions stay in the Sessions browser, resumable anytime; nothing on disk is deleted.
                body += L" will be closed.\n\nThe sessions stay in the Sessions browser — resume any of them anytime (their conversation files on disk are kept). Other tabs are closed too.";
                if (forceFavorite)
                {
                    body += L"\n\nEvery session will be starred (★) first so you can find them under the Favorite filter.";
                }

                const auto verb = forceFavorite ? std::wstring{ L"Favorite & close " } : std::wstring{ L"Close " };
                const std::wstring titleStr = verb + std::to_wstring(closable.size()) + (closable.size() == 1 ? L" tab?" : L" tabs?");

                ContentDialog dialog;
                dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
                dialog.Title(winrt::box_value(winrt::hstring{ titleStr }));
                dialog.Content(winrt::box_value(winrt::hstring{ body }));
                if (forceFavorite)
                {
                    // The favorite disposition is already chosen ("★ Favorite & close all tabs"): a plain
                    // 2-button confirm, no redundant "Close All" vs "Favorite & Close All" re-offer.
                    dialog.PrimaryButtonText(L"★ Favorite & Close All");
                    dialog.CloseButtonText(L"Cancel All");
                    dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel All (the Close button)
                }
                else
                {
                    dialog.PrimaryButtonText(L"Close All");
                    dialog.SecondaryButtonText(L"★ Favorite & Close All"); // FAVORITES.md: star every managed session, then close the batch
                    dialog.CloseButtonText(L"Cancel All");
                    dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel All (the Close button)
                }

                const auto result = co_await presenter.ShowDialog(dialog);
                const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
                if (!strong)
                {
                    co_return;
                }
                if (result == ContentDialogResult::None)
                {
                    ::Agentmaster::LogNav(L"close-all cancelled (tabs=" + std::to_wstring(closable.size()) + L" managed=" + std::to_wstring(managedCount) + L")");
                    co_return; // Cancel All / dismiss -> stop the close
                }
                if (!forceFavorite)
                {
                    favoriteAll = (result == ContentDialogResult::Secondary); // Secondary == Favorite & Close All
                }
                // forceFavorite: favoriteAll already true; Primary (★ Favorite & Close All) == confirm.
                // Otherwise Primary (Close All) or Secondary (Favorite & Close All) -> fall through to the per-tab close below.
            }
            // No presenter to confirm with -> close anyway (non-destructive; don't strand the close).
        }
        else if (_settings.GlobalSettings().ConfirmOnClose() != ConfirmOnClose::Never)
        {
            // Pure shell-tab batch: keep upstream's single aggregate confirmation.
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::MultipleTabs);

            // Hold a strong reference to `this` after the co_await so that
            // the for-loop below can safely dispatch on us.
            auto strong = weak.get();
            if (!strong || warningResult != ContentDialogResult::Primary)
            {
                if (strong && warningResult != ContentDialogResult::Primary)
                {
                    ::Agentmaster::LogNav(L"close-all cancelled (shell batch, tabs=" + std::to_wstring(closable.size()) + L")"); // only a genuine decline, never a teardown race
                }
                co_return;
            }
        }

        // Apply the resolved decision to each tab, without re-prompting.
        // Nav audit BEGIN for the whole batch (pairs with `close-all done`; the managed tabs inside
        // the loop additionally log their own close-begin/close-done).
        ::Agentmaster::LogNav(L"close-all begin tabs=" + std::to_wstring(closable.size()) + L" managed=" + std::to_wstring(managedCount) + (favoriteAll ? L" favorite-all" : L""));
        for (auto& tab : closable)
        {
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            const auto sessionId = _ClaudeSessionForTab(tab);
            if (!sessionId.empty())
            {
                // FAVORITES.md: "Favorite & Close All" stars every managed session before archiving.
                if (favoriteAll)
                {
                    ::Agentmaster::SetSessionFavorite(sessionId, true);
                }
                // Always archive (keep the record) + close — FAVORITES.md: there is no Delete All.
                // skipConfirm: the batch dialog already ran. The session stays resumable in Sessions.
                co_await _ArchiveAndCloseClaudeTab(tab, sessionId, /*skipConfirm*/ true);
            }
            else
            {
                // Plain shell tab: reopen-buffer + close (a read-only tab still gets its guard).
                co_await _HandleCloseTabRequested(tab, /*skipConfirmClose*/ true);
            }
        }
        ::Agentmaster::LogNav(L"close-all done"); // END — an unpaired begin means the app died mid-batch
    }
    // Method Description:
    // - Responds to changes in the TabView's item list by changing the
    //   tabview's visibility.
    // - This method is also invoked when tabs are dragged / dropped as part of
    //   tab reordering and this method hands that case as well in concert with
    //   TabDragStarting and TabDragCompleted handlers that are set up in
    //   TerminalPage::Create()
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabItemsChanged(const IInspectable& /*sender*/, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs)
    {
        if (_rearranging)
        {
            if (eventArgs.CollectionChange() == Windows::Foundation::Collections::CollectionChange::ItemRemoved)
            {
                _rearrangeFrom = eventArgs.Index();
            }

            if (eventArgs.CollectionChange() == Windows::Foundation::Collections::CollectionChange::ItemInserted)
            {
                _rearrangeTo = eventArgs.Index();
            }
        }

        if (const auto p = CommandPaletteElement())
        {
            p.Visibility(Visibility::Collapsed);
        }
        _UpdateTabView();

        // Agentmaster: adding/removing tabs can close (or bring back) the tab Jump Back targets — so
        // re-evaluate the Manager nav buttons. Also a retry point for binding the internal scroller if
        // it wasn't realized at first layout.
        _UpdateManagerNavButtons();

        // Agentmaster: a tab with a multi-line title being added or removed changes the tallest tab in
        // the strip — re-reserve a consistent row height across all tabs (covers a new tab needing the
        // current reserve applied, and a closed multi-line tab letting the row shrink back).
        _UpdateReservedTabTitleLines();
    }

    void TerminalPage::_OnTabPointerPressed(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e)
    {
        const auto pointerProps = e.GetCurrentPoint(nullptr).Properties();

        // Agentmaster (Shift+Click background-activate): defensively clear any one-shot select-veto left
        // over from a prior press. In the normal case the press's own SelectionChanged already consumed
        // it (below), but a press whose selection never fired (e.g. clicking the already-selected tab)
        // could otherwise leave it armed onto this press. A fresh Shift+Click re-arms it below.
        _suppressTabSelectForActivate = false;
        _tabSelectRevertTo = nullptr;

        // Agentmaster (eager-init): Shift+Left-Click a managed agent-session tab = "Activate Tab" ONLY —
        // ACTIVATE without switching. If the session is DORMANT this starts its claude IN PLACE (a
        // background/restored tab spawns its child lazily, only when first SHOWN, so a window-restored
        // session never resumes until clicked; this wakes it where you are); it NEVER switches to the tab.
        // Activate is the WHOLE gesture — to also ENTER the tab, use a plain click (which switches + starts
        // it as a side effect). So the switch is suppressed for ANY managed session tab, dormant or already
        // running (on a running one _ActivateDormantSession no-ops and nothing visible happens — you stay
        // where you are), matching the Manager board-card / Explorer-tree Shift+Click twin
        // (AgentManagerContent), which likewise suppresses the select/jump on Shift regardless of state. A
        // NON-session tab (pwsh/cmd/Manager) has nothing to activate, so it falls through to a normal switch.
        //
        // Modifier read: use the pointer event's own KeyModifiers() — under XAML Islands
        // CoreWindow::GetForCurrentThread() can be null (see AgentManagerContent::ShiftHeld), which would
        // silently drop the gesture and let the tab switch. The event args carry the true modifier state.
        //
        // Suppressing the switch: a MUX TabViewItem (a ListViewItem) drives its selection on its OWN
        // pointer-RELEASE, and the TabView re-captures the pointer internally AFTER this bubbling
        // PointerPressed handler runs (see the middle-click note below) — so neither e.Handled(true) nor a
        // press-time CapturePointer steal reliably stops the release-driven selection (the reason this used
        // to still switch). Instead we ARM a one-shot veto that _OnTabSelectionChanged uses to snap the
        // selection back to the previously-focused tab BEFORE any content-swap side effects run.
        if (pointerProps.IsLeftButtonPressed())
        {
            const bool shiftHeld = WI_IsFlagSet(e.KeyModifiers(), winrt::Windows::System::VirtualKeyModifiers::Shift);
            if (shiftHeld)
            {
                if (const auto tab = _GetTabByTabViewItem(sender))
                {
                    if (const auto sid = _ClaudeSessionForTab(tab); !sid.empty())
                    {
                        _ActivateDormantSession(sid); // wake it if dormant; a no-op if already running / not hosted here
                        // Arm the veto only when the press would actually CHANGE the selection (a different
                        // tab). Clicking the already-selected session tab raises no SelectionChanged, so
                        // arming there would leave the flag stranded onto the next click.
                        if (_tabView.SelectedItem() != sender)
                        {
                            _tabSelectRevertTo = _tabView.SelectedItem();
                            _suppressTabSelectForActivate = true;
                        }
                        _middleClickClosePending = false;
                        e.Handled(true);
                        return; // Shift+Click a managed session tab NEVER switches — activate only
                    }
                }
            }
        }

        // Agentmaster (the Agent Manager PAGE): a plain left press on a tab header while the page is on
        // screen means "show me this tab". A header that is NOT the strip's selected item gets there through
        // MUX's own release-driven selection -> SelectionChanged -> the funnel, which leaves the page. The one
        // it can't: the header MUX kept (or re-acquired) as selected under the page — clicking it changes
        // nothing, raises nothing, and the page would stay up. Leave the page onto it here instead.
        if (pointerProps.IsLeftButtonPressed() && _managerPageVisible && _tabView.SelectedItem() == sender)
        {
            if (const auto tab = _GetTabByTabViewItem(sender))
            {
                _LeaveManagerPageOntoSelectedTab(tab);
            }
        }

        if (!pointerProps.IsMiddleButtonPressed())
        {
            // Agentmaster: a left/right press clears the middle-click marker so a subsequent X-button
            // close on this tab isn't mistaken for a middle click (see _OnTabCloseRequested).
            _middleClickClosePending = false;
            return;
        }

        // Agentmaster: record the middle press. When the X is SHOWN, WinUI closes the tab natively on
        // middle release (raising TabCloseRequested) and we have no other way there to tell it from an
        // X-button click — _OnTabCloseRequested reads this to honor "Close tab with middle-mouse click".
        _middleClickClosePending = true;

        // When the X is HIDDEN, WinUI raises no native close, so the manual hook below is the only
        // middle-click path — but only while the user keeps middle-click-close enabled
        // (_tabItemMiddleClickHookEnabled already folds in AppSettings.closeTabOnMiddleClick).
        if (!_tabItemMiddleClickHookEnabled)
        {
            return;
        }

        const auto tabViewItem = sender.try_as<MUX::Controls::TabViewItem>();
        if (!tabViewItem || !tabViewItem.CapturePointer(e.Pointer()))
        {
            return;
        }

        _tabItemMiddleClickExited = false;

        _tabItemMiddleClickPointerEntered = tabViewItem.PointerEntered(winrt::auto_revoke, [this](auto&&, auto&& e) {
            _tabItemMiddleClickExited = false;
            e.Handled(true);
        });
        _tabItemMiddleClickPointerExited = tabViewItem.PointerExited(winrt::auto_revoke, [this](auto&&, auto&& e) {
            _tabItemMiddleClickExited = true;
            e.Handled(true);
        });
        _tabItemMiddleClickPointerCaptureLost = tabViewItem.PointerCaptureLost(winrt::auto_revoke, [this](auto&& sender, auto&& e) {
            // The WinUI TabView calls CapturePointer() internally and it's not reference counted,
            // so when it calls ReleasePointerCapture() in its PointerReleased handler,
            // we get a PointerCaptureLost before we receive the PointerReleased event.
            // This makes typical handling of PointerReleased events on our side difficult.
            // Well, whatever, now we just hook PointerCaptureLost because we know WinUI will trigger it.

            _tabItemMiddleClickPointerEntered.revoke();
            _tabItemMiddleClickPointerExited.revoke();
            _tabItemMiddleClickPointerCaptureLost.revoke();

            if (!_tabItemMiddleClickExited && !e.GetCurrentPoint(nullptr).Properties().IsMiddleButtonPressed())
            {
                _OnTabPointerReleasedCloseTab(std::move(sender));
            }

            e.Handled(true);
        });
        e.Handled(true);
    }

    safe_void_coroutine TerminalPage::_OnTabPointerReleasedCloseTab(IInspectable sender)
    {
        // WinUI asynchronously updates its tab view items, so it may happen that we're given a
        // `TabViewItem` that still contains a `Tab` which has actually already been removed.
        // First we must yield once, to flush out whatever TabView is currently doing.
        const auto weak = get_weak();
        co_await wil::resume_foreground(Dispatcher());
        const auto strong = weak.get();
        if (!strong)
        {
            co_return;
        }

        const auto tab = _GetTabByTabViewItem(sender);
        if (!tab)
        {
            co_return;
        }

        // `tab.Shutdown()` in `_RemoveTab()` sets the content to null = This checks if the tab is closed.
        if (tab.Content())
        {
            _HandleCloseTabRequested(tab);
        }
    }

    // Method Description:
    // - Makes `tab` the visible, focused tab: swaps its content into the tab area, focuses it (unless
    //   the tab switcher is previewing), and scrolls the tab strip so its header is on screen.
    // - Agentmaster: the scroll is _RevealTabInStrip — upstream's StartBringIntoView could not reach a
    //   header virtualized out of a long strip; a header already wholly on screen still never moves.
    // Arguments:
    // - tab: the newly selected tab.
    // Return Value:
    // - <none>
    // Exceptions:
    // - none escape: the body is CATCH_LOG-wrapped (upstream), and the strip reveal logs its own failures.
    void TerminalPage::_UpdatedSelectedTab(const winrt::TerminalApp::Tab& tab)
    {
        // Unfocus all the tabs.
        for (const auto& tab : _tabs)
        {
            tab.Focus(FocusState::Unfocused);
        }

        try
        {
            _tabContent.Children().Clear();
            _tabContent.Children().Append(tab.Content());

            // Agentmaster: the full-window pages (Sessions, any future one) are WINDOW-level overlays
            // (mounted on Root, below the tab strip) opened from the Agent Manager page — they are NOT
            // part of any tab's Content(), so the swap above doesn't remove them and they would keep
            // covering the newly-selected tab. Dismiss them ALL — generically, via the registry each
            // page joins at build (_RegisterAgentPageOverlay), so a new page binds automatically —
            // whenever a TAB comes on screen (every tab: the Manager is a page now, never a tab).
            // Collapse synchronously: this is a SelectionChanged handler (already mutating the tree just
            // above), NOT an in-page pointer handler, so the hit-test AV that makes the in-page Hide paths
            // defer doesn't apply — and a synchronous collapse avoids a one-frame bleed over the new tab.
            // The symmetric re-show of a page still logically OPEN (its Show set the intent, a Hide would
            // have cleared it) runs when the Agent Manager page comes back (_ShowManagerPage), exactly as
            // left — the collapse kept its typed search + rows in memory; onRestore re-applies scroll +
            // focus. Pages otherwise open only from their Manager buttons.
            _DismissAgentPageOverlays();

            // GH#7409: If the tab switcher is open, then we _don't_ want to
            // automatically focus the new tab here. The tab switcher wants
            // to be able to "preview" the selected tab as the user tabs
            // through the menu, but if we toss the focus to the control
            // here, then the user won't be able to navigate the ATS any
            // longer.
            //
            // When the tab switcher is eventually dismissed, the focus will
            // get tossed back to the focused terminal control, so we don't
            // need to worry about focus getting lost.
            const auto p = CommandPaletteElement();
            if (!p || p.Visibility() != Visibility::Visible)
            {
                tab.Focus(FocusState::Programmatic);
                _UpdateMRUTab(tab);
                _updateAllTabCloseButtons();
            }

            // Agentmaster (tab-strip reveal): upstream's `tab.TabViewItem().StartBringIntoView()` silently
            // NO-OPS on a tab scrolled far off the strip — the strip virtualizes, so such a tab's
            // TabViewItem is not in the visual tree — and Agentmaster windows hold hundreds of tabs. Every
            // selection of a far tab (a card double-click, Jump Back, a toast click, switchToTab, a close
            // successor, Move to start/end, a restored window's focused tab) swapped the content in without
            // scrolling the strip to it. _RevealTabInStrip realizes it first. leaveFullyVisibleInPlace
            // keeps what StartBringIntoView did right: a tab already wholly on screen — the one you just
            // clicked — never moves.
            _RevealTabInStrip(tab, /*leaveFullyVisibleInPlace*/ true);

            // Raise an event that our title changed
            TitleChanged.raise(*this, nullptr);

            _updateThemeColors();

            auto tabImpl = _GetTabImpl(tab);
            if (tabImpl)
            {
                auto profile = tabImpl->GetFocusedProfile();
                _UpdateBackground(profile);
            }

            _adjustProcessPriorityThrottled->Run();
        }
        CATCH_LOG();
    }

    void TerminalPage::_UpdateBackground(const winrt::Microsoft::Terminal::Settings::Model::Profile& profile)
    {
        if (profile && _settings.GlobalSettings().UseBackgroundImageForWindow())
        {
            _SetBackgroundImage(profile.DefaultAppearance());
        }
    }

    // Method Description:
    // - Responds to the TabView control's Selection Changed event (to move a
    //      new terminal control into focus) when not in in the middle of a tab rearrangement.
    // - Agentmaster: also ignored while a tab is being removed (_removing — _RemoveTab applies the
    //   successor's effects itself) or moved (_movingTab — the TabView's momentary neighbor pick is not a
    //   switch), and for the strip deselect the Agent Manager page performs (_managerPageDeselecting);
    //   otherwise the Agentmaster reactions run through _ApplyAgentTabSelection. A real tab selected while
    //   the Agent Manager page is on screen takes the page down first (_HideManagerPage) — that is how a
    //   tab click, Ctrl+Tab, a new tab or an Activate leaves the page.
    // - Agentmaster: a brand-new tab's selection is HELD while _InitializeTab sets it
    //   (_holdTabSwitchForNewTab): this handler only records it (_tabSwitchHeldForNewTab), and _InitializeTab
    //   calls it again — directly, with the same sender — after the strip's first layout pass with the new
    //   tab. That call reads the selection off the TabView like the event does, so the switch it runs is the
    //   one the event would have run.
    // Arguments:
    // - sender: the control that originated this event (the TabView; _InitializeTab's direct call passes
    //   _tabView)
    // - eventArgs: the event's constituent arguments (unused — _InitializeTab's direct call passes null)
    // Return Value:
    // - <none>
    // Exceptions:
    // - whatever an Agentmaster step lets escape (_ApplyAgentTabSelection) — the same exposure those
    //   steps had when they were written inline here; the content swap itself is CATCH_LOG-wrapped.
    void TerminalPage::_OnTabSelectionChanged(const IInspectable& sender, const WUX::Controls::SelectionChangedEventArgs& /*eventArgs*/)
    {
        // Agentmaster (Shift+Click background-activate): our own selection-revert (below) re-enters this
        // handler synchronously — no-op it so the revert doesn't run the switch side effects. Likewise the
        // Agent Manager page's own strip deselect (_DeselectTabStripForManagerPage): bookkeeping, not a switch.
        if (_revertingTabSelection || _managerPageDeselecting)
        {
            return;
        }
        // Agentmaster (tab-strip reveal — a NEW tab): _InitializeTab is selecting a tab the strip has not laid
        // out yet. Record the switch and leave it to _InitializeTab, which runs it after that first layout
        // pass — revealing the tab now could only go through an estimated ScrollIntoView, and swapping its
        // content in now would let the layout pass start the terminal before the session is wired up (see
        // _holdTabSwitchForNewTab in TerminalPage.h).
        if (_holdTabSwitchForNewTab)
        {
            _tabSwitchHeldForNewTab = true;
            return;
        }

        // Agentmaster: _movingTab — _TryMoveTab has the moved tab out of TabItems, and the TabView's
        // momentary pick of its neighbor is not a switch the user made (see the flag's declaration).
        if (!_rearranging && !_removing && !_movingTab)
        {
            // Agentmaster (Shift+Click background-activate): a Shift+Click on a managed session tab
            // ACTIVATES the session in place and must NOT switch to it. The MUX TabViewItem selected
            // itself on pointer-release regardless of the press handler, so deterministically REVERT the
            // selection to the tab focused before the press — BEFORE _UpdatedSelectedTab swaps the tab
            // content. Consumed one-shot; the guarded revert's re-entrant SelectionChanged early-returns
            // above, and the old tab's content was never detached, so the revert needs no re-attach.
            if (_suppressTabSelectForActivate)
            {
                _suppressTabSelectForActivate = false;
                const auto restore = _tabSelectRevertTo;
                _tabSelectRevertTo = nullptr;
                if (_managerPageVisible)
                {
                    // Agentmaster: the press happened on the Agent Manager page, where NO tab is the selected
                    // one (the strip is deselected for it, whatever MUX may have re-selected under it) — so
                    // "revert" means deselect again, and the Shift+Clicked tab doesn't stay highlighted
                    // under the page.
                    _DeselectTabStripForManagerPage();
                }
                else if (restore)
                {
                    _revertingTabSelection = true;
                    auto clearReverting = wil::scope_exit([this]() noexcept { _revertingTabSelection = false; });
                    try
                    {
                        _tabView.SelectedItem(restore);
                    }
                    CATCH_LOG();
                }
                return; // never run the switch (content swap / focus) for a Shift+Click-activated tab
            }

            auto tabView = sender.as<MUX::Controls::TabView>();
            auto selectedIndex = tabView.SelectedIndex();
            if (selectedIndex >= 0 && selectedIndex < gsl::narrow_cast<int32_t>(_tabs.Size()))
            {
                const auto tab{ _tabs.GetAt(selectedIndex) };
                // Agentmaster (the Agent Manager PAGE): a tab coming on screen takes the page down first —
                // flag, lens, Home paint — before _UpdatedSelectedTab swaps the tab's content into the
                // page's place (which is also what removes the page's root). No-op when the page is not up.
                _HideManagerPage(tab);
                // Agentmaster (perf forensics — the SLOW TAB SWITCH stopwatch): a switch that stalls the UI
                // thread for seconds (the 2026-09-04 release log shows a 45s one right after a
                // `[nav] tab-focus`) leaves NOTHING behind that says which step ate the time — the 20s
                // [ui-stall] watchdog only reports that the pump stopped. So the funnel is timed in
                // four laps (content swap+focus+theme / lens+popups+flash+wake+summary+overlay /
                // Manager selection sync / highlight+record+nav) and, ONLY when the whole switch took
                // >= kSlowTabSwitchMs, logs `[tab-switch-slow]` with the breakdown; a Low-priority
                // dispatcher probe then measures how long until the UI thread was idle again (the XAML
                // layout/render of the new content + whatever was queued behind the switch) and logs
                // `[tab-switch-settle]` when THAT exceeds kSlowTabSettleMs. A normal switch logs nothing.
                constexpr double kSlowTabSwitchMs = 250.0;
                constexpr double kSlowTabSettleMs = 750.0;
                const auto swMs = [](const std::chrono::steady_clock::time_point& a, const std::chrono::steady_clock::time_point& b) {
                    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count()) / 1000.0;
                };
                const auto swT0 = std::chrono::steady_clock::now();
                _UpdatedSelectedTab(tab);
                const auto swT1 = std::chrono::steady_clock::now();
                // Agentmaster: every Agentmaster reaction to the switch — the Manager lens's on-screen
                // flag, the tag popups, the red flash, the Deactivated-tab wake, the summary, the overlay
                // focus gate, the pending dots, the Linked-Lenses selection sync, the pill, the Manager's
                // bring-into-view — lives in ONE helper (TerminalPage.AgentEngine.cpp), shared with
                // _RemoveTab's close-successor selection, which this funnel never sees (the _removing
                // guard above). It stamps laps 2-4 of the stopwatch.
                AgentTabSelectionLaps laps{};
                _ApplyAgentTabSelection(tab, &laps);
                const auto swT2 = laps.afterOverlayFocus; // lap 2: lens visibility + popups + flash + wake + summary + overlay focus
                const auto swT3 = laps.afterManagerSync; // lap 3: pending-dots contrast + the Manager selection sync (a full lens rebuild when the Manager is on screen)
                const auto swT4 = laps.afterManagerReveal; // lap 4: selection highlight + bring-into-view
                const double swTotal = swMs(swT0, swT4);
                if (swTotal >= kSlowTabSwitchMs)
                {
                    try
                    {
                        ::Agentmaster::AppendStateLog(L"hooks.log",
                                                      L"[tab-switch-slow] " + _DescribeTabForLog(tab) +
                                                          L" total=" + std::to_wstring(static_cast<int>(swTotal)) + L"ms" +
                                                          L" content-swap+focus+theme=" + std::to_wstring(static_cast<int>(swMs(swT0, swT1))) + L"ms" +
                                                          L" lens+popups+flash+wake+summary+overlay=" + std::to_wstring(static_cast<int>(swMs(swT1, swT2))) + L"ms" +
                                                          L" dots+manager-select=" + std::to_wstring(static_cast<int>(swMs(swT2, swT3))) + L"ms" +
                                                          L" highlight+reveal=" + std::to_wstring(static_cast<int>(swMs(swT3, swT4))) + L"ms\n");
                    }
                    CATCH_LOG();
                }
                // The settle probe: a Low-priority item runs only once everything the switch queued —
                // the new content's layout + render passes, any dispatcher work it triggered — has
                // drained, so (now - swT0) at that point is the switch as the USER experienced it.
                try
                {
                    const std::wstring swIdent = _DescribeTabForLog(tab);
                    Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [swT0, swIdent, swMs, kSlowTabSettleMs]() {
                        const double settleMs = swMs(swT0, std::chrono::steady_clock::now());
                        if (settleMs >= kSlowTabSettleMs)
                        {
                            ::Agentmaster::AppendStateLog(L"hooks.log", L"[tab-switch-settle] " + swIdent + L" idle-again-after=" + std::to_wstring(static_cast<int>(settleMs)) + L"ms (the switch + the layout/render/queued work behind it)\n");
                        }
                    });
                }
                CATCH_LOG();
            }
            // Agentmaster (M10): the focused tab is part of the per-window record, so a reopen restores
            // it. Debounce-save on switch so the selection persists LIVE (not only at the graceful
            // close-flush) — a switch-then-hard-kill then still reopens the tab you last had focused.
            // _ScheduleWindowRecordSave no-ops until startup completes, so the tab-creation selection
            // churn during restore never thrashes a save, and rapid switching collapses to one write.
            _ScheduleWindowRecordSave();
            // Agentmaster: the active tab just changed — re-evaluate the tab-strip nav buttons (Home drops
            // its active paint and Jump Back hides once a tab replaced the Agent Manager page).
            _UpdateManagerNavButtons();
        }
    }

    // Method Description:
    // - Updates all tabs with their current index in _tabs.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateTabIndices()
    {
        const auto size = _tabs.Size();
        // Agentmaster: how many non-closable tabs lead the strip ahead of the closable set — the count
        // each tab uses to enable "Close tabs to the left" / "Close other tabs". It was 1 while the pinned
        // Manager tab sat at index 0; the Manager is a page behind the Home button now, so every tab is
        // closable and nothing is reserved (the parameter stays for a future pinned tab).
        constexpr uint32_t reservedLeading = 0u;
        for (uint32_t i = 0; i < size; ++i)
        {
            auto tab{ _tabs.GetAt(i) };
            auto tabImpl{ winrt::get_self<Tab>(tab) };
            tabImpl->UpdateTabViewIndex(i, size, reservedLeading);
        }
    }

    // Method Description:
    // - Bumps the tab in its in-order index up to the top of the mru list.
    // Arguments:
    // - tab: tab to bump.
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateMRUTab(const winrt::TerminalApp::Tab& tab)
    {
        uint32_t mruIndex;
        if (_mruTabs.IndexOf(tab, mruIndex))
        {
            if (mruIndex > 0)
            {
                _mruTabs.RemoveAt(mruIndex);
                _mruTabs.InsertAt(0, tab);
            }
        }
    }

    // Method Description:
    // - Moves the tab to another index in the tabs row (if required).
    // - Agentmaster: the moved tab ends up selected (upstream behavior) and its header is revealed at
    //   its new position; while it is briefly out of TabItems the selection funnel is suppressed
    //   (_movingTab), so the TabView's momentary pick of its neighbor never counts as visiting that
    //   neighbor. Because the moved tab ends up selected, moving a tab while the Agent Manager page is up
    //   (a "Move to start/end" menu item, a drag) also leaves the page for that tab — upstream's "a moved
    //   tab gets focus" rule, unchanged.
    // Arguments:
    // - currentTabIndex: the current index of the tab to move
    // - suggestedNewTabIndex: the new index of the tab, might get clamped to fit int the tabs row boundaries
    // Return Value:
    // - <none>
    // Exceptions:
    // - a failing TabItems / collection mutation propagates (upstream behavior); the suppression flag is
    //   always cleared on the way out (scope_exit).
    void TerminalPage::_TryMoveTab(const uint32_t currentTabIndex,
                                   const int32_t suggestedNewTabIndex)
    {
        // Agentmaster: with <=1 tab there is nowhere to move, and bailing keeps Size()-1 from underflowing
        // on an EMPTY strip (an Agentmaster window lives on with no tabs — its Agent Manager page shows).
        // Every tab is movable, to any index: the Manager is a page now, not a tab pinned at index 0.
        if (_tabs.Size() <= 1)
        {
            return;
        }
        auto newTabIndex = gsl::narrow_cast<uint32_t>(std::clamp<int32_t>(suggestedNewTabIndex, 0, _tabs.Size() - 1));
        if (currentTabIndex != newTabIndex)
        {
            auto tab = _tabs.GetAt(currentTabIndex);
            // Agentmaster: [nav] — a same-window tab move (moveTab action / drop routing), BEGIN-style
            // (logged before the mutations so a crash mid-move still shows what was being moved).
            ::Agentmaster::LogNav(L"tab-move " + _DescribeTabForLog(tab) + L" idx=" + std::to_wstring(currentTabIndex) + L" -> " + std::to_wstring(newTabIndex));
            // Agentmaster (transcript overlay): "moving a tab closes the overlay". Explicit here because a
            // move of the ALREADY-selected tab raises no SelectionChanged, so the tab-switch funnel's close
            // never runs for it. Before the mutations, so the move's own selection + focus land normally.
            _CloseTranscriptOverlay(L"tab move");
            auto tabViewItem = tab.TabViewItem();
            _tabs.RemoveAt(currentTabIndex);
            _tabs.InsertAt(newTabIndex, tab);
            _UpdateTabIndices();

            // Agentmaster (tab move): while the moved tab is OUT of TabItems, the TabView selects its nearest
            // neighbor for that instant (when the moved tab was the selected one). That is not a switch the
            // user made, so the selection funnel must not treat it as one — mark it read, clear its flash,
            // wake it, log it (_movingTab; its declaration has the whole story). The SelectedItem below is
            // the one real selection and runs the funnel as usual.
            {
                _movingTab = true;
                auto clearMovingTab = wil::scope_exit([this]() noexcept { _movingTab = false; });
                _tabView.TabItems().RemoveAt(currentTabIndex);
                _tabView.TabItems().InsertAt(newTabIndex, tabViewItem);
            }
            // Did the TabView already put the selection back on the moved tab by itself (a selection the
            // guard above just ignored)? Then SelectedItem changes nothing, raises no SelectionChanged, and
            // the funnel won't run — so do its two strip-facing parts here: show the tab at its NEW
            // position (a "Move to end" must visibly land there) and re-evaluate the nav buttons, which
            // _OnTabItemsChanged last computed while the neighbor held the selection.
            const bool selectionUnchanged = (_tabView.SelectedItem() == tabViewItem);
            _tabView.SelectedItem(tabViewItem);
            _ApplyTabDragPolicy(tabViewItem); // Agentmaster: the reinsert breaks the item->container map — settle it (+ re-pin CanDrag(false))
            // Agentmaster (the Agent Manager PAGE): the same no-SelectionChanged trap on the page — MUX put the
            // selection on the moved tab under the page by itself, so the funnel never runs and the page would
            // stay up over the tab this move just gave focus to. Leave the page onto it explicitly: the leave's
            // content swap (_UpdatedSelectedTab) reveals the tab and the leave itself re-evaluates the nav
            // buttons, so the two strip-facing steps below are only needed off the page.
            if (selectionUnchanged && !_LeaveManagerPageOntoSelectedTab(tab))
            {
                _RevealTabInStrip(tab, /*leaveFullyVisibleInPlace*/ true);
                _UpdateManagerNavButtons();
            }

            if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
            {
                const auto tabTitle = tab.Title();
                autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                RS_fmt(L"TerminalPage_TabMovedAnnouncement_Direction", tabTitle, newTabIndex + 1),
                                                L"TerminalPageMoveTabWithDirection" /* unique name for this notification category */);
            }
        }
    }

    void TerminalPage::_TabDragStarted(const IInspectable& /*sender*/,
                                       const IInspectable& /*eventArgs*/)
    {
        _rearranging = true;
        _rearrangeFrom = std::nullopt;
        _rearrangeTo = std::nullopt;
    }

    void TerminalPage::_TabDragCompleted(const IInspectable& /*sender*/,
                                         const IInspectable& /*eventArgs*/)
    {
        auto& from{ _rearrangeFrom };
        auto& to{ _rearrangeTo };

        // Agentmaster: [nav] drag forensics — the gesture END (pairs with tab-drag-begin; a begin with
        // no matching end == the app died mid-drag, the v0.6.7 MUX AV class). from/to carry the
        // same-window reorder result; a tear-out / cross-window drop / aborted drag has neither.
        {
            std::wstring nav{ L"tab-drag-end" };
            if (from.has_value() && to.has_value())
            {
                nav += L" from=" + std::to_wstring(*from) + L" to=" + std::to_wstring(*to);
            }
            else
            {
                nav += L" (no same-window reorder)";
            }
            ::Agentmaster::LogNav(nav);
        }

        if (from.has_value() && to.has_value() && to != from)
        {
            try
            {
                auto& tabs{ _tabs };
                auto tab = tabs.GetAt(from.value());
                tabs.RemoveAt(from.value());
                tabs.InsertAt(to.value(), tab);
                _UpdateTabIndices();
            }
            CATCH_LOG();
        }

        _rearranging = false;

        if (to.has_value() &&
            *to < gsl::narrow_cast<int32_t>(TabRow().TabView().TabItems().Size()))
        {
            // Selecting the dropped tab
            TabRow().TabView().SelectedIndex(to.value());
        }

        from = std::nullopt;
        to = std::nullopt;

        _SettleTabStripLayout(); // Agentmaster: MUX's own drag-reorder mutated TabItems — settle before the user can re-grab (drag-start AV guard)
    }

    void TerminalPage::_DismissTabContextMenus()
    {
        for (const auto& tab : _tabs)
        {
            if (tab.TabViewItem().ContextFlyout())
            {
                tab.TabViewItem().ContextFlyout().Hide();
            }
        }
    }

    void TerminalPage::_FocusCurrentTab(const bool focusAlways)
    {
        // We don't want to set focus on the tab if fly-out is open as it will
        // be closed TODO GH#5400: consider checking we are not in the opening
        // state, by hooking both Opening and Open events
        if (focusAlways || !_newTabButton.Flyout().IsOpen())
        {
            // Return focus to the active control
            if (auto tab{ _GetFocusedTab() })
            {
                tab.Focus(FocusState::Programmatic);
                _UpdateMRUTab(tab);
                _updateAllTabCloseButtons();
            }
            else if (_managerPageVisible && _managerPage)
            {
                // Agentmaster: no tab is focused while the Agent Manager page is on screen — the page is
                // what a dismissed transient UI (palette, new-tab flyout, renamer) hands focus back to.
                _managerPage->Focus(FocusState::Programmatic);
            }
        }
    }

    bool TerminalPage::_HasMultipleTabs() const
    {
        return _tabs.Size() > 1;
    }

    // Method Description:
    // - Attempts to find and focus the given tab in this window.
    // Arguments:
    // - tab: The tab to focus.
    // Return Value:
    // - true if the tab was found and focused, false otherwise.
    bool TerminalPage::FocusTab(const winrt::TerminalApp::Tab& tab)
    {
        if (const auto tabIndex{ _GetTabIndex(tab) })
        {
            _SelectTab(tabIndex.value());
            return true;
        }
        return false;
    }

    // Method Description:
    // - Sends a desktop toast notification with the given title and body.
    //   When the toast is activated (clicked), the window is summoned and
    //   the originating tab is focused.
    // Arguments:
    // - tabTitle: The title to display in the notification.
    // - body: The body text. If empty, a standard tab-activity message is built.
    // - tab: The tab to switch to when the toast is activated.
    void TerminalPage::_SendDesktopNotification(const winrt::hstring& tabTitle, const winrt::hstring& body, const winrt::com_ptr<Tab>& tab, const winrt::TerminalApp::IPaneContent& content)
    {
        // Don't send a notification if the window is focused and the requesting
        // pane is the active pane. The user is already looking at it.
        if (_activated && tab == _GetFocusedTabImpl())
        {
            if (const auto activePane{ tab->GetActivePane() })
            {
                if (activePane->GetContent() == content)
                {
                    return;
                }
            }
        }

        // Build the notification message.
        // If a custom body is provided (e.g. from OSC 777), use the title/body directly.
        // Otherwise, build the standard tab-activity notification message.
        winrt::hstring notificationTitle;
        winrt::hstring message;
        if (!body.empty())
        {
            notificationTitle = tabTitle;
            message = body;
        }
        else
        {
            // Use the window name if available for context; otherwise just use the tab title.
            // Use the raw WindowName (not WindowNameForDisplay) so we don't include
            // the "<unnamed window>" placeholder in the notification body.
            const auto windowName = _WindowProperties ? _WindowProperties.WindowName() : winrt::hstring{};
            if (!windowName.empty())
            {
                message = RS_fmt(L"NotificationMessage_TabActivityInWindow", std::wstring_view{ tabTitle }, std::wstring_view{ windowName });
            }
            else
            {
                message = RS_fmt(L"NotificationMessage_TabActivity", std::wstring_view{ tabTitle });
            }
            notificationTitle = CascadiaSettings::ApplicationDisplayName();
        }

        // Use the Tab object's identity hash as a stable toast tag.
        // This survives tab reordering and cross-window moves.
        const auto tabHash = std::hash<winrt::Windows::Foundation::IUnknown>{}(*tab);
        const hstring tabTag{ fmt::format(FMT_COMPILE(L"wt-tab-{:016x}"), tabHash) };

        const implementation::DesktopNotificationArgs args{
            .Title = notificationTitle,
            .Message = message,
            .Tag = tabTag
        };

        implementation::DesktopNotification::SendNotification(args, [weakThis{ get_weak() }, weakTab{ tab->get_weak() }, weakContent{ winrt::make_weak(content) }]() {
            if (const auto page{ weakThis.get() })
            {
                // The toast Activated callback runs on a background thread.
                // Marshal to the UI thread for tab focus and window summon.
                page->Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakPage{ page->get_weak() }, weakTab, weakContent]() {
                    if (const auto p{ weakPage.get() })
                    {
                        if (const auto t{ weakTab.get() })
                        {
                            // Try to find and focus the tab in this window first.
                            if (const auto tabIndex{ p->_GetTabIndex(*t) })
                            {
                                p->SummonWindowRequested.raise(nullptr, nullptr);
                                p->_SelectTab(tabIndex.value());

                                // Focus the specific pane that raised the notification.
                                if (const auto paneContent{ weakContent.get() })
                                {
                                    const auto rootPane = t->GetRootPane();
                                    rootPane->WalkTree([&](const auto& pane) {
                                        if (pane->GetContent() == paneContent)
                                        {
                                            rootPane->FocusPane(pane);
                                        }
                                    });
                                }
                            }
                            else
                            {
                                // The tab may have moved to another window.
                                // Raise FocusTabRequested so the emperor can
                                // search all windows for it.
                                p->FocusTabRequested.raise(nullptr, *t);
                            }
                        }
                        else
                        {
                            // Tab was closed. Just summon this window.
                            p->SummonWindowRequested.raise(nullptr, nullptr);
                        }
                    }
                });
            }
        });
    }
}
