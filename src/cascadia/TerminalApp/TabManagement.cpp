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
    // Arguments:
    // - newTabImpl: the uninitialized tab.
    // - insertPosition: Optional parameter to indicate the position of tab.
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

        // Agentmaster: index 0 is permanently reserved for the pinned, non-closable Manager tab —
        // NOTHING may insert ahead of it. This is the single hard chokepoint for every tab insertion
        // (both _tabs and the TabView item list go through here), so it backs the NewTabPosition math
        // above regardless of caller or setting. _managerTab is still null while the Manager tab
        // itself is being created (it is assigned from the return of _CreateNewTabFromPane, AFTER this
        // method runs), so this guard never displaces that one intentional insert at 0 — it only
        // clamps every later tab.
        if (_managerTab && insertPosition == 0)
        {
            insertPosition = 1;
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
        if (!_openClaudeTabInBackground)
        {
            _tabView.SelectedItem(tabViewItem);
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
    void TerminalPage::_UpdateTabWidthMode()
    {
        _tabView.TabWidthMode(_settings.GlobalSettings().TabWidthMode());
    }

    // Method Description:
    // - Handle changes in tab layout.
    void TerminalPage::_UpdateTabView()
    {
        // The tab row should only be visible if:
        // - we're not in focus mode
        // - we're not in full screen, or the user has enabled fullscreen tabs
        // - there is more than one tab, or the user has chosen to always show tabs
        const auto isVisible = !_isInFocusMode &&
                               (!_isFullscreen || _showTabsFullscreen) &&
                               (_settings.GlobalSettings().ShowTabsInTitlebar() ||
                                (_tabs.Size() > 1) ||
                                _settings.GlobalSettings().AlwaysShowTabs());

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

        if (tab == _managerTab)
        {
            _managerTab = nullptr;
        }

        if (_stashed.draggedTab && *_stashed.draggedTab == tab)
        {
            _stashed.draggedTab = nullptr;
        }

        _tabs.RemoveAt(tabIndex);
        _tabView.TabItems().RemoveAt(tabIndex);
        _UpdateTabIndices();

        // To close the window here, we need to close the hosting window.
        if (_tabs.Size() == 0)
        {
            // If we are supposed to save state, make sure we clear it out
            // if the user manually closed all tabs.
            // Do this only if we are the last window; the monarch will notice
            // we are missing and remove us that way otherwise.
            CloseWindowRequested.raise(*this, nullptr);
        }
        else if (focusedTabIndex.has_value() && focusedTabIndex.value() == gsl::narrow_cast<uint32_t>(tabIndex))
        {
            // Manually select the new tab to get focus, rather than relying on TabView since:
            // 1. We want to customize this behavior (e.g., use MRU logic)
            // 2. In fullscreen (GH#5799) and focus (GH#7916) modes the _OnTabItemsChanged is not fired
            // 3. When rearranging tabs (GH#7916) _OnTabItemsChanged is suppressed
            const auto tabSwitchMode = _settings.GlobalSettings().TabSwitcherMode();

            if (tabSwitchMode == TabSwitcherMode::MostRecentlyUsed)
            {
                const auto newSelectedTab = _mruTabs.GetAt(0);
                _UpdatedSelectedTab(newSelectedTab);
                _tabView.SelectedItem(newSelectedTab.TabViewItem());
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
                auto newSelectedTab{ _tabs.GetAt(newSelectedIndex) };
                _UpdatedSelectedTab(newSelectedTab);

                // Also, we need to _manually_ set the SelectedItem of the tabView
                // here. If we don't, then the TabView will technically not have a
                // selected item at all, which can make things like ClosePane not
                // work correctly.
                _tabView.SelectedItem(newSelectedTab.TabViewItem());
            }
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
    void TerminalPage::_SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode)
    {
        const auto index{ _GetFocusedTabIndex().value_or(0) };
        const auto tabSwitchMode = customTabSwitcherMode ? customTabSwitcherMode.Value() : _settings.GlobalSettings().TabSwitcherMode();
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
    bool TerminalPage::_SelectTab(uint32_t tabIndex)
    {
        // GH#9369 - if the argument is out of range, then clamp to the number
        // of available tabs. Previously, we'd just silently do nothing if the
        // value was greater than the number of tabs.
        tabIndex = std::clamp(tabIndex, 0u, _tabs.Size() - 1);

        auto tab{ _tabs.GetAt(tabIndex) };
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
                    s.pendingConfirmPromptId.clear();
                });
                _sessionRegistry->SetInjector(id, nullptr);
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
    // and hands them to _RemoveTabs, which owns the aggregate close confirmation, the per-session
    // archive bookkeeping, AND the pinned-Manager-tab skip (it filters the Manager tab out, so it
    // survives). favoriteFirst forwards to _RemoveTabs(forceFavorite): it stars every managed session
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
        // Agentmaster: never bulk-close the pinned, non-closable Manager tab. Upstream's
        // "Close other tabs" copies every tab but the focused one (which includes the Manager
        // tab at index 0), and our "Close tabs to the left" targets [0, index) (likewise). This
        // is the single chokepoint for every bulk close, so filtering the Manager tab out here
        // keeps it alive no matter which direction the user closed from. (_managerTab is null when
        // there is no Manager tab, in which case nothing is filtered — plain upstream behavior.)
        std::vector<winrt::TerminalApp::Tab> closable;
        closable.reserve(tabs.size());
        for (const auto& tab : tabs)
        {
            if (tab != _managerTab)
            {
                closable.push_back(tab);
            }
        }

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
                co_return;
            }
        }

        // Apply the resolved decision to each tab, without re-prompting.
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

        // Agentmaster: adding/removing tabs can make the strip overflow (the `<`/`>` arrows appear) or
        // stop overflowing (offset snaps back to 0), and can close the tab Jump Back targets — so
        // re-evaluate the Manager nav buttons. Also a retry point for binding the internal scroller if
        // it wasn't realized at first layout.
        _UpdateManagerNavButtons();
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

        // Agentmaster: the manual middle-click hook fires for tabs whose X is hidden — but the pinned
        // Manager tab is hidden-X because it is permanently NON-closable, not merely styled that way.
        // Never middle-click-close it (the X button can't, so neither should middle click); other
        // X-hidden tabs still close normally below.
        if (_managerTab && tab == _managerTab)
        {
            co_return;
        }

        // `tab.Shutdown()` in `_RemoveTab()` sets the content to null = This checks if the tab is closed.
        if (tab.Content())
        {
            _HandleCloseTabRequested(tab);
        }
    }

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

            // Agentmaster: the full-window pages (Archive, Sessions, any future one) are WINDOW-level
            // overlays (mounted on Root, below the tab strip) opened from the Manager tab — they are
            // NOT part of any tab's Content(), so the swap above doesn't remove them and they would
            // keep covering the newly-selected tab (and still be there when returning to the Manager).
            // Dismiss them ALL — generically, via the registry each page joins at build
            // (_RegisterAgentPageOverlay), so a new page binds automatically — whenever the selection
            // moves OFF the Manager tab. Collapse synchronously: this is a SelectionChanged handler
            // (already mutating the tree just above), NOT an in-page pointer handler, so the hit-test
            // AV that makes the in-page Hide paths defer doesn't apply — and a synchronous collapse
            // avoids a one-frame bleed over the new tab. Symmetrically, RETURNING to the Manager tab
            // re-shows any page still logically OPEN (its Show set the intent, a Hide would have cleared
            // it) exactly as left — the collapse kept its typed search + rows in memory; onRestore re-
            // applies scroll + focus. Pages otherwise open only from their Manager buttons.
            if (tab != _managerTab)
            {
                _DismissAgentPageOverlays();
            }
            else
            {
                _RestoreAgentPageOverlays();
            }

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

            tab.TabViewItem().StartBringIntoView();

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
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabSelectionChanged(const IInspectable& sender, const WUX::Controls::SelectionChangedEventArgs& /*eventArgs*/)
    {
        // Agentmaster (Shift+Click background-activate): our own selection-revert (below) re-enters this
        // handler synchronously — no-op it so the revert doesn't run the switch side effects.
        if (_revertingTabSelection)
        {
            return;
        }

        if (!_rearranging && !_removing)
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
                if (restore)
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
                _UpdatedSelectedTab(tab);
                // Agentmaster (bookmark tags): a tab switch dismisses the Tags panel — it is anchored
                // under the tab it was opened for, which the switch just left behind. The hover panel
                // likewise (its badge anchor belongs to the strip layout that is about to change).
                _CloseTagEditorPopup();
                _CloseTagHoverPopup();
                // Agentmaster (tab status-dot red flash): switching TO a tab is a "visit" — the current
                // tab is always considered visited, so stop any red flash on the now-focused tab.
                _VisitTabClearFlash(tab);
                // Agentmaster (TAB_OVERLAY.md summary panel): a "here-and-now lens" should be current the
                // instant you look at it — kick a cheap, mtime-gated content re-read of the now-focused
                // tab's summary panel so it isn't up to ~5 s stale (its own timer-backstop cadence).
                _RefreshFocusedTabSummary(tab);
                // Agentmaster (PENDING_INPUT.md): a colored tab's effective background shifts on
                // selection (WT draws a deselected tab at 30% over the tab row, much darker), so the
                // now-deselected and now-selected pending tabs must re-pick their "3 dots" light/dark
                // color immediately instead of waiting for the next ~2s scan tick. Cheap (no buffer read).
                _RefreshPendingDotsContrast();
                // Agentmaster (Linked Lenses): follow the switch into the Manager lens — select this
                // tab's managed session so returning to the Manager tab shows the session you were just
                // in. No-op for the Manager tab, a non-session tab, or before startup completes.
                _SyncManagerSelectionToTab(tab);
                // Agentmaster (Linked Lenses): re-evaluate the per-tab "selected/active" pill — it
                // shows only while the Manager tab is active, so leaving the Manager clears it and
                // returning re-applies it for the current hover/selection.
                _UpdateManagerSelectionHighlight();
                // Agentmaster (Linked Lenses): switching TO the Manager tab REVEALS the current
                // selection — scroll the selected card/row into view, since it may have scrolled off
                // while the selection followed tab switches with the Manager hidden. Manager tab only.
                if (_managerTab && tab == _managerTab)
                {
                    _BringManagerSelectionIntoView();
                }
            }
            // Agentmaster (M10): the focused tab is part of the per-window record, so a reopen restores
            // it. Debounce-save on switch so the selection persists LIVE (not only at the graceful
            // close-flush) — a switch-then-hard-kill then still reopens the tab you last had focused.
            // _ScheduleWindowRecordSave no-ops until startup completes, so the tab-creation selection
            // churn during restore never thrashes a save, and rapid switching collapses to one write.
            _ScheduleWindowRecordSave();
            // Agentmaster: the active tab just changed — re-evaluate the tab-strip nav buttons. Home
            // hides when the Manager tab becomes active; Jump Back shows there (targeting the session
            // the lens auto-selected for the tab you came from).
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
        // Agentmaster: the pinned Manager tab (index 0, non-closable) is skipped by every bulk
        // close (_RemoveTabs), so it must NOT count as a closable neighbor when enabling
        // "Close tabs to the left" / "Close other tabs". Tell each tab how many such reserved
        // leading tabs precede the closable set (0 or 1). The Manager is always pinned first, so
        // checking the front tab is sufficient.
        const uint32_t reservedLeading = (_managerTab && size > 0 && _tabs.GetAt(0) == _managerTab) ? 1u : 0u;
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
    // Arguments:
    // - currentTabIndex: the current index of the tab to move
    // - suggestedNewTabIndex: the new index of the tab, might get clamped to fit int the tabs row boundaries
    // Return Value:
    // - <none>
    void TerminalPage::_TryMoveTab(const uint32_t currentTabIndex,
                                   const int32_t suggestedNewTabIndex)
    {
        // Agentmaster: the pinned Manager tab is non-movable, and nothing may move ahead of it.
        // Bail with <=1 tab so the clamp below can never get lo>hi (lowerBound 1 vs hi 0 when the
        // sole tab is the Manager) and so Size()-1 can't underflow.
        if (_tabs.Size() <= 1)
        {
            return;
        }
        if (_managerTab && currentTabIndex < _tabs.Size() && _tabs.GetAt(currentTabIndex) == _managerTab)
        {
            return; // can't move the Manager tab itself
        }
        const int32_t lowerBound = _managerTab ? 1 : 0; // keep index 0 reserved for the Manager tab
        auto newTabIndex = gsl::narrow_cast<uint32_t>(std::clamp<int32_t>(suggestedNewTabIndex, lowerBound, _tabs.Size() - 1));
        if (currentTabIndex != newTabIndex)
        {
            auto tab = _tabs.GetAt(currentTabIndex);
            auto tabViewItem = tab.TabViewItem();
            _tabs.RemoveAt(currentTabIndex);
            _tabs.InsertAt(newTabIndex, tab);
            _UpdateTabIndices();

            _tabView.TabItems().RemoveAt(currentTabIndex);
            _tabView.TabItems().InsertAt(newTabIndex, tabViewItem);
            _tabView.SelectedItem(tabViewItem);

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

        _PinManagerTabFirst(); // Agentmaster: a tab dropped before the pinned Manager tab snaps it back to 0
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
