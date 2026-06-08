
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"

#include <TerminalCore/ControlKeyStates.hpp>
#include <TerminalThemeHelpers.h>
#include <til/hash.h>
#include <til/unicode.h>
#include <Utils.h>

#include <filesystem> // Agentmaster: _SpawnClaudeSession derives a title from the cwd leaf

#include "../../types/inc/ColorFix.hpp"
#include "../../types/inc/utils.hpp"
#include "../TerminalSettingsAppAdapterLib/TerminalSettings.h"
#include "AgentManagerContent.h"
#include "AgentTabOverlay.h"
#include "AgentMaster/ClaudeSpawn.h"
#include "AgentMaster/Engine.h"
#include "AgentMaster/HookWire.h"
#include "AgentMaster/HooksBridge.h"
#include "AgentMaster/Persistence.h"
#include "AgentMaster/ProcessInspect.h"
#include "AgentMaster/ProcessObserver.h"
#include "AgentMaster/Scheduler.h"
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionScanner.h"
#include "App.h"
#include "DebugTapConnection.h"
#include "MarkdownPaneContent.h"
#include "Remoting.h"
#include "ScratchpadContent.h"
#include "SettingsPaneContent.h"
#include "SnippetsPaneContent.h"
#include "TabRowControl.h"
#include "TerminalSettingsCache.h"

#include "LaunchPositionRequest.g.cpp"
#include "RenameWindowRequestedArgs.g.cpp"
#include "RequestMoveContentArgs.g.cpp"
#include "TerminalPage.g.cpp"

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

#define HOOKUP_ACTION(action) _actionDispatch->action({ this, &TerminalPage::_Handle##action });

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace clipboard
{
    static SRWLOCK lock = SRWLOCK_INIT;

    struct ClipboardHandle
    {
        explicit ClipboardHandle(bool open) :
            _open{ open }
        {
        }

        ~ClipboardHandle()
        {
            if (_open)
            {
                ReleaseSRWLockExclusive(&lock);
                CloseClipboard();
            }
        }

        explicit operator bool() const noexcept
        {
            return _open;
        }

    private:
        bool _open = false;
    };

    ClipboardHandle open(HWND hwnd)
    {
        // Turns out, OpenClipboard/CloseClipboard are not thread-safe whatsoever,
        // and on CloseClipboard, the GetClipboardData handle may get freed.
        // The problem is that WinUI also uses OpenClipboard (through WinRT which uses OLE),
        // and so even with this mutex we can still crash randomly if you copy something via WinUI.
        // Makes you wonder how many Windows apps are subtly broken, huh.
        AcquireSRWLockExclusive(&lock);

        bool success = false;

        // OpenClipboard may fail to acquire the internal lock --> retry.
        for (DWORD sleep = 10;; sleep *= 2)
        {
            if (OpenClipboard(hwnd))
            {
                success = true;
                break;
            }
            // 10 iterations
            if (sleep > 10000)
            {
                break;
            }
            Sleep(sleep);
        }

        if (!success)
        {
            ReleaseSRWLockExclusive(&lock);
        }

        return ClipboardHandle{ success };
    }

    void write(wil::zwstring_view text, std::string_view html, std::string_view rtf)
    {
        static const auto regular = [](const UINT format, const void* src, const size_t bytes) {
            wil::unique_hglobal handle{ THROW_LAST_ERROR_IF_NULL(GlobalAlloc(GMEM_MOVEABLE, bytes)) };

            const auto locked = GlobalLock(handle.get());
            memcpy(locked, src, bytes);
            GlobalUnlock(handle.get());

            THROW_LAST_ERROR_IF_NULL(SetClipboardData(format, handle.get()));
            handle.release();
        };
        static const auto registered = [](const wchar_t* format, const void* src, size_t bytes) {
            const auto id = RegisterClipboardFormatW(format);
            if (!id)
            {
                LOG_LAST_ERROR();
                return;
            }
            regular(id, src, bytes);
        };

        EmptyClipboard();

        if (!text.empty())
        {
            // As per: https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats
            //   CF_UNICODETEXT: [...] A null character signals the end of the data.
            // --> We add +1 to the length. This works because .c_str() is null-terminated.
            regular(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        }

        if (!html.empty())
        {
            registered(L"HTML Format", html.data(), html.size());
        }

        if (!rtf.empty())
        {
            registered(L"Rich Text Format", rtf.data(), rtf.size());
        }
    }

    winrt::hstring read()
    {
        // This handles most cases of pasting text as the OS converts most formats to CF_UNICODETEXT automatically.
        if (const auto handle = GetClipboardData(CF_UNICODETEXT))
        {
            const wil::unique_hglobal_locked lock{ handle };
            const auto str = static_cast<const wchar_t*>(lock.get());
            if (!str)
            {
                return {};
            }

            const auto maxLen = GlobalSize(handle) / sizeof(wchar_t);
            const auto len = wcsnlen(str, maxLen);
            return winrt::hstring{ str, gsl::narrow_cast<uint32_t>(len) };
        }

        // We get CF_HDROP when a user copied a file with Ctrl+C in Explorer and pastes that into the terminal (among others).
        if (const auto handle = GetClipboardData(CF_HDROP))
        {
            const wil::unique_hglobal_locked lock{ handle };
            const auto drop = static_cast<HDROP>(lock.get());
            if (!drop)
            {
                return {};
            }

            const auto cap = DragQueryFileW(drop, 0, nullptr, 0);
            if (cap == 0)
            {
                return {};
            }

            auto buffer = winrt::impl::hstring_builder{ cap };
            const auto len = DragQueryFileW(drop, 0, buffer.data(), cap + 1);
            if (len == 0)
            {
                return {};
            }

            return buffer.to_hstring();
        }

        return {};
    }
} // namespace clipboard

namespace winrt::TerminalApp::implementation
{
    TerminalPage::TerminalPage(TerminalApp::WindowProperties properties, const TerminalApp::ContentManager& manager) :
        _tabs{ winrt::single_threaded_observable_vector<TerminalApp::Tab>() },
        _mruTabs{ winrt::single_threaded_observable_vector<TerminalApp::Tab>() },
        _manager{ manager },
        _hostingHwnd{},
        _WindowProperties{ std::move(properties) }
    {
        InitializeComponent();
        _WindowProperties.PropertyChanged({ get_weak(), &TerminalPage::_windowPropertyChanged });
    }

    // Agentmaster (M9): the engine is a process singleton shared by every window. When this
    // window is torn down, drop its adoption handler from the shared registry so it doesn't
    // linger (the handler captures get_weak(), so a stray call is already a safe no-op — this
    // just keeps the registry's handler list bounded across many window open/close cycles). The
    // registry itself outlives every window (held by SharedEngine), so this call stays valid.
    TerminalPage::~TerminalPage()
    {
        // Agentmaster (lifecycle gap #1): archive any of this window's still-live sessions before we
        // detach. Normally CloseWindow already did this (deterministically, before raising
        // CloseWindowRequested); this is the catch-all for quit-all / any teardown path that bypassed
        // it. Idempotent — a no-op if CloseWindow already cleared _claudeTabs.
        _ArchiveWindowSessionsOnTeardown();

        if (_sessionRegistry && _adoptionToken)
        {
            _sessionRegistry->RemoveAdoptionHandler(_adoptionToken);
        }
        // Symmetric to the adoption handler: drop this window's liveness probe from the shared
        // scanner so a closed window's probe (it captures get_weak()) doesn't linger on the
        // process-wide scanner. The scanner outlives every window (held by SharedEngine).
        if (_scanner && _livenessToken)
        {
            _scanner->RemoveLivenessProbe(_livenessToken);
        }
        // Fleet Observer (OBSERVER.md §10/§12): drop THIS window's tab roster from the process-wide
        // observer so a closed window's tabs aren't surveyed/correlated after teardown (Rule #10).
        if (_observer && !_windowId.empty())
        {
            _observer->UnpublishWindow(_windowId);
        }
        // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5): this window is gone — drop it
        // from the process-wide live set, which rewrites open-windows.json (skip-empty: the last window
        // out leaves the final snapshot intact). Symmetric to the RegisterLiveWindow in engine init.
        if (!_windowId.empty())
        {
            ::Agentmaster::UnregisterLiveWindow(_windowId);
        }
    }

    // Method Description:
    // - implements the IInitializeWithWindow interface from shobjidl_core.
    // - We're going to use this HWND as the owner for the ConPTY windows, via
    //   ConptyConnection::ReparentWindow. We need this for applications that
    //   call GetConsoleWindow, and attempt to open a MessageBox for the
    //   console. By marking the conpty windows as owned by the Terminal HWND,
    //   the message box will be owned by the Terminal window as well.
    //   - see GH#2988
    HRESULT TerminalPage::Initialize(HWND hwnd)
    {
        if (!_hostingHwnd.has_value())
        {
            // GH#13211 - if we haven't yet set the owning hwnd, reparent all the controls now.
            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto& term{ pane->GetTerminalControl() })
                        {
                            term.OwningHwnd(reinterpret_cast<uint64_t>(hwnd));
                        }
                    });
                }
                // We don't need to worry about resetting the owning hwnd for the
                // SUI here. GH#13211 only repros for a defterm connection, where
                // the tab is spawned before the window is created. It's not
                // possible to make a SUI tab like that, before the window is
                // created. The SUI could be spawned as a part of a window restore,
                // but that would still work fine. The window would be created
                // before restoring previous tabs in that scenario.
            }
        }

        _hostingHwnd = hwnd;
        return S_OK;
    }

    // INVARIANT: This needs to be called on OUR UI thread!
    void TerminalPage::SetSettings(CascadiaSettings settings, bool needRefreshUI)
    {
        assert(Dispatcher().HasThreadAccess());
        if (_settings == nullptr)
        {
            // Create this only on the first time we load the settings.
            _terminalSettingsCache = std::make_shared<TerminalSettingsCache>(settings);
        }
        _settings = settings;

        // Make sure to call SetCommands before _RefreshUIForSettingsReload.
        // SetCommands will make sure the KeyChordText of Commands is updated, which needs
        // to happen before the Settings UI is reloaded and tries to re-read those values.
        if (const auto p = CommandPaletteElement())
        {
            p.SetActionMap(_settings.ActionMap());
        }

        if (needRefreshUI)
        {
            _RefreshUIForSettingsReload();
        }

        // Upon settings update we reload the system settings for scrolling as well.
        // TODO: consider reloading this value periodically.
        _systemRowsToScroll = _ReadSystemRowsToScroll();
    }

    bool TerminalPage::IsRunningElevated() const noexcept
    {
        // GH#2455 - Make sure to try/catch calls to Application::Current,
        // because that _won't_ be an instance of TerminalApp::App in the
        // LocalTests
        try
        {
            return Application::Current().as<TerminalApp::App>().Logic().IsRunningElevated();
        }
        CATCH_LOG();
        return false;
    }
    bool TerminalPage::CanDragDrop() const noexcept
    {
        try
        {
            return Application::Current().as<TerminalApp::App>().Logic().CanDragDrop();
        }
        CATCH_LOG();
        return true;
    }

    void TerminalPage::Create()
    {
        // Hookup the key bindings
        _HookupKeyBindings(_settings.ActionMap());

        _tabContent = this->TabContent();
        _tabRow = this->TabRow();
        _tabView = _tabRow.TabView();
        _rearranging = false;

        const auto canDragDrop = CanDragDrop();

        _tabView.CanReorderTabs(canDragDrop);
        _tabView.CanDragTabs(canDragDrop);
        _tabView.TabDragStarting({ get_weak(), &TerminalPage::_TabDragStarted });
        _tabView.TabDragCompleted({ get_weak(), &TerminalPage::_TabDragCompleted });

        auto tabRowImpl = winrt::get_self<implementation::TabRowControl>(_tabRow);
        _newTabButton = tabRowImpl->NewTabButton();

        if (_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            // Remove the TabView from the page. We'll hang on to it, we need to
            // put it in the titlebar.
            uint32_t index = 0;
            if (this->Root().Children().IndexOf(_tabRow, index))
            {
                this->Root().Children().RemoveAt(index);
            }

            // Inform the host that our titlebar content has changed.
            SetTitleBarContent.raise(*this, _tabRow);

            // GH#13143 Manually set the tab row's background to transparent here.
            //
            // We're doing it this way because ThemeResources are tricky. We
            // default in XAML to using the appropriate ThemeResource background
            // color for our TabRow. When tabs in the titlebar are _disabled_,
            // this will ensure that the tab row has the correct theme-dependent
            // value. When tabs in the titlebar are _enabled_ (the default),
            // we'll switch the BG to Transparent, to let the Titlebar Control's
            // background be used as the BG for the tab row.
            //
            // We can't do it the other way around (default to Transparent, only
            // switch to a color when disabling tabs in the titlebar), because
            // looking up the correct ThemeResource from and App dictionary is a
            // capital-H Hard problem.
            const auto transparent = Media::SolidColorBrush();
            transparent.Color(Windows::UI::Colors::Transparent());
            _tabRow.Background(transparent);
        }
        _updateThemeColors();

        // Initialize the state of the CloseButtonOverlayMode property of
        // our TabView, to match the tab.showCloseButton property in the theme.
        if (const auto theme = _settings.GlobalSettings().CurrentTheme())
        {
            const auto visibility = theme.Tab() ? theme.Tab().ShowCloseButton() : Settings::Model::TabCloseButtonVisibility::Always;

            _tabItemMiddleClickHookEnabled = visibility == Settings::Model::TabCloseButtonVisibility::Never;

            switch (visibility)
            {
            case Settings::Model::TabCloseButtonVisibility::Never:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Auto);
                break;
            case Settings::Model::TabCloseButtonVisibility::Hover:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::OnPointerOver);
                break;
            default:
                _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Always);
                break;
            }
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _RegisterActionCallbacks();

        //Event Bindings (Early)
        _newTabButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuDefaultButtonClicked",
                    TraceLoggingDescription("Event emitted when the default button from the new tab split button is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                page->_OpenNewTerminalViaDropdown(NewTerminalArgs());
            }
        });
        _newTabButton.Drop({ get_weak(), &TerminalPage::_NewTerminalByDrop });
        _tabView.SelectionChanged({ this, &TerminalPage::_OnTabSelectionChanged });
        _tabView.TabCloseRequested({ this, &TerminalPage::_OnTabCloseRequested });
        _tabView.TabItemsChanged({ this, &TerminalPage::_OnTabItemsChanged });

        _tabView.TabDragStarting({ this, &TerminalPage::_onTabDragStarting });
        _tabView.TabStripDragOver({ this, &TerminalPage::_onTabStripDragOver });
        _tabView.TabStripDrop({ this, &TerminalPage::_onTabStripDrop });
        _tabView.TabDroppedOutside({ this, &TerminalPage::_onTabDroppedOutside });

        _CreateNewTabFlyout();

        _UpdateTabWidthMode();

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        // Once the page is actually laid out on the screen, trigger all our
        // startup actions. Things like Panes need to know at least how big the
        // window will be, so they can subdivide that space.
        //
        // _OnFirstLayout will remove this handler so it doesn't get called more than once.
        _layoutUpdatedRevoker = _tabContent.LayoutUpdated(winrt::auto_revoke, { this, &TerminalPage::_OnFirstLayout });

        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();
        _showTabsFullscreen = _settings.GlobalSettings().ShowTabsFullscreen();

        // DON'T set up Toasts/TeachingTips here. They should be loaded and
        // initialized the first time they're opened, in whatever method opens
        // them.

        _tabRow.ShowElevationShield(IsRunningElevated() && _settings.GlobalSettings().ShowAdminShield());

        _adjustProcessPriorityThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 100 },
                .debounce = true,
                .trailing = true,
            },
            [=]() {
                _adjustProcessPriority();
            });
    }

    Windows::UI::Xaml::Automation::Peers::AutomationPeer TerminalPage::OnCreateAutomationPeer()
    {
        return Automation::Peers::FrameworkElementAutomationPeer(*this);
    }

    // Method Description:
    // - This is a bit of trickiness: If we're running unelevated, and the user
    //   passed in only --elevate actions, the we don't _actually_ want to
    //   restore the layouts here. We're not _actually_ about to create the
    //   window. We're simply going to toss the commandlines
    // Arguments:
    // - <none>
    // Return Value:
    // - true if we're not elevated but all relevant pane-spawning actions are elevated
    bool TerminalPage::ShouldImmediatelyHandoffToElevated(const CascadiaSettings& settings) const
    {
        if (_startupActions.empty() || _startupConnection || IsRunningElevated())
        {
            // No point in handing off if we got no startup actions, or we're already elevated.
            // Also, we shouldn't need to elevate handoff ConPTY connections.
            assert(!_startupConnection);
            return false;
        }

        // Check that there's at least one action that's not just an elevated newTab action.
        for (const auto& action : _startupActions)
        {
            // Only new terminal panes will be requesting elevation.
            NewTerminalArgs newTerminalArgs{ nullptr };

            if (action.Action() == ShortcutAction::NewTab)
            {
                const auto& args{ action.Args().try_as<NewTabArgs>() };
                if (args)
                {
                    newTerminalArgs = args.ContentArgs().try_as<NewTerminalArgs>();
                }
                else
                {
                    // This was a nt action that didn't have any args. The default
                    // profile may want to be elevated, so don't just early return.
                }
            }
            else if (action.Action() == ShortcutAction::SplitPane)
            {
                const auto& args{ action.Args().try_as<SplitPaneArgs>() };
                if (args)
                {
                    newTerminalArgs = args.ContentArgs().try_as<NewTerminalArgs>();
                }
                else
                {
                    // This was a nt action that didn't have any args. The default
                    // profile may want to be elevated, so don't just early return.
                }
            }
            else
            {
                // This was not a new tab or split pane action.
                // This doesn't affect the outcome
                continue;
            }

            // It's possible that newTerminalArgs is null here.
            // GetProfileForArgs should be resilient to that.
            const auto profile{ settings.GetProfileForArgs(newTerminalArgs) };
            if (profile.Elevate())
            {
                continue;
            }

            // The profile didn't want to be elevated, and we aren't elevated.
            // We're going to open at least one tab, so return false.
            return false;
        }
        return true;
    }

    // Method Description:
    // - Escape hatch for immediately dispatching requests to elevated windows
    //   when first launched. At this point in startup, the window doesn't exist
    //   yet, XAML hasn't been started, but we need to dispatch these actions.
    //   We can't just go through ProcessStartupActions, because that processes
    //   the actions async using the XAML dispatcher (which doesn't exist yet)
    // - DON'T CALL THIS if you haven't already checked
    //   ShouldImmediatelyHandoffToElevated. If you're thinking about calling
    //   this outside of the one place it's used, that's probably the wrong
    //   solution.
    // Arguments:
    // - settings: the settings we should use for dispatching these actions. At
    //   this point in startup, we hadn't otherwise been initialized with these,
    //   so use them now.
    // Return Value:
    // - <none>
    void TerminalPage::HandoffToElevated(const CascadiaSettings& settings)
    {
        if (_startupActions.empty())
        {
            return;
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _settings = settings;
        _HookupKeyBindings(_settings.ActionMap());
        _RegisterActionCallbacks();

        for (const auto& action : _startupActions)
        {
            // only process new tabs and split panes. They're all going to the elevated window anyways.
            if (action.Action() == ShortcutAction::NewTab || action.Action() == ShortcutAction::SplitPane)
            {
                _actionDispatch->DoAction(action);
            }
        }
    }

    safe_void_coroutine TerminalPage::_NewTerminalByDrop(const Windows::Foundation::IInspectable&, winrt::Windows::UI::Xaml::DragEventArgs e)
    try
    {
        const auto data = e.DataView();
        if (!data.Contains(StandardDataFormats::StorageItems()))
        {
            co_return;
        }

        const auto weakThis = get_weak();
        const auto items = co_await data.GetStorageItemsAsync();
        const auto strongThis = weakThis.get();
        if (!strongThis)
        {
            co_return;
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabByDragDrop",
            TraceLoggingDescription("Event emitted when the user drag&drops onto the new tab button"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        for (const auto& item : items)
        {
            auto directory = item.Path();

            std::filesystem::path path(std::wstring_view{ directory });
            if (!std::filesystem::is_directory(path))
            {
                directory = winrt::hstring{ path.parent_path().native() };
            }

            NewTerminalArgs args;
            args.StartingDirectory(directory);
            _OpenNewTerminalViaDropdown(args);
        }
    }
    CATCH_LOG()

    // Method Description:
    // - This method is called once command palette action was chosen for dispatching
    //   We'll use this event to dispatch this command.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command)
    {
        const auto& actionAndArgs = command.ActionAndArgs();
        _actionDispatch->DoAction(sender, actionAndArgs);
    }

    // Method Description:
    // - This method is called once command palette command line was chosen for execution
    //   We'll use this event to create a command line execution command and dispatch it.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnCommandLineExecutionRequested(const IInspectable& /*sender*/, const winrt::hstring& commandLine)
    {
        ExecuteCommandlineArgs args{ commandLine };
        ActionAndArgs actionAndArgs{ ShortcutAction::ExecuteCommandline, args };
        _actionDispatch->DoAction(actionAndArgs);
    }

    // Method Description:
    // - This method is called once on startup, on the first LayoutUpdated event.
    //   We'll use this event to know that we have an ActualWidth and
    //   ActualHeight, so we can now attempt to process our list of startup
    //   actions.
    // - We'll remove this event handler when the event is first handled.
    // - If there are no startup actions, we'll open a single tab with the
    //   default profile.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    // Agentmaster: open the always-present Manager tab, pinned at the leftmost
    // position (tab 0) and non-closable. Tracked in _managerTab.
    void TerminalPage::_OpenAgentManagerTab()
    {
        if (_managerTab)
        {
            return;
        }

        const auto& managerPane{ winrt::make_self<AgentManagerContent>() };
        // Route keys the content didn't handle back to the page (as other content panes do).
        managerPane->GetRoot().KeyDown({ this, &TerminalPage::_KeyDownHandler });

        // Agentmaster: wire the Manager UI to the engine (registry + spawn/activate/kill).
        _WireAgentManagerContent(managerPane);

        const auto resultPane = std::make_shared<Pane>(*managerPane);
        _managerTab = _CreateNewTabFromPane(resultPane, 0); // 0 == leftmost

        if (_managerTab)
        {
            // Non-closable: hide this tab's close button.
            _managerTab.CloseButtonVisibility(winrt::Microsoft::Terminal::Settings::Model::TabCloseButtonVisibility::Never);

            // Agentmaster: also gray out the right-click "Move tab" / "Close" / "Close tab"
            // entries so the pinned Manager tab can't be relocated or closed from the menu.
            if (const auto tabImpl{ _GetTabImpl(_managerTab) })
            {
                tabImpl->DisableCloseAndMoveMenuItems();
            }
            // Non-movable by drag, too: CanDrag(false) stops the tab being dragged/torn out;
            // the drag/move seams additionally call _PinManagerTabFirst() to snap it back to
            // index 0 if another tab is dropped before it. Best-effort (wrapped).
            try
            {
                if (const auto tvi = _managerTab.TabViewItem())
                {
                    tvi.CanDrag(false);
                    tvi.AllowDrop(false);
                }
            }
            CATCH_LOG();
        }
    }

    // Agentmaster: stand up the session-management engine — the SessionRegistry (single
    // source of truth) and the HooksBridge (the local named-pipe listener that turns Claude
    // Code hook events into authoritative session state). Called once, before the Manager
    // tab opens, so the pipe is live and the registry exists when sessions are spawned.
    void TerminalPage::_InitAgentmasterEngine()
    {
        if (_sessionRegistry)
        {
            return;
        }

        _appSettings = ::Agentmaster::LoadAppSettings(); // the Settings cog (per-field defaults if absent)

        // M9: consume the ONE process-wide engine. v1.24 WT is a WindowEmperor — every window
        // lives in a single process — so the SessionRegistry (single source of truth), the
        // HooksBridge (the `\\.\pipe\agentmaster.<pid>` listener — the PID is unambiguous only
        // because there is exactly one bridge), and the Scheduler must be a process singleton,
        // NOT a per-window object. The first window to reach here constructs + wires + starts
        // it (the logging / scheduler / persistence observers, the pipe, bridge discovery + the
        // shared hook files + the claude PATH shim — see AgentMaster/Engine.{h,cpp}); every
        // later window receives the same instance. This page just copies the shared_ptrs.
        auto& engine = ::Agentmaster::SharedEngine();
        _sessionRegistry = engine.registry;
        _hooksBridge = engine.bridge;
        _scheduler = engine.scheduler;
        _scanner = engine.scanner;
        _observer = engine.observer; // Fleet Observer S-lane (PULL census/correlation; OBSERVER.md §10)

        // M10 (PERSISTENCE.md §13): claim this window's persisted record — an existing
        // windows/<id>.json (geometry + Manager lens + ordered tab refs), or a fresh GUID if none
        // remains. Stashed in _windowRecord; its lens is seeded into the Manager tab in
        // _WireAgentManagerContent, and changes are debounced-autosaved back to the same file.
        // Increment 3: when the Emperor assigned this window a specific record (multi-window
        // reopen), claim THAT id so geometry (TerminalWindow) and lens (here) come from the same
        // record; otherwise claim the front record (single-window) or mint a fresh id.
        auto claimed = _assignedWindowId.empty() ? ::Agentmaster::ClaimWindowRecord() :
                                                    ::Agentmaster::ClaimWindowRecord(_assignedWindowId);
        if (claimed)
        {
            _windowRecord = std::move(*claimed);
            _windowRecordClaimed = true;
        }
        else
        {
            GUID fresh{};
            ::CoCreateGuid(&fresh);
            _windowRecord = ::Agentmaster::WindowRecord{};
            _windowRecord.windowId = ::Microsoft::Console::Utils::GuidToString(fresh);
        }
        _windowId = _windowRecord.windowId;

        // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5): mark this window LIVE in the
        // process-wide set, which rewrites open-windows.json. Done here (not at WindowEmperor create
        // time) because _windowId is only resolved now — register early + correct so even a one-window
        // session lands in the manifest and reopens next run. Unregistered in ~TerminalPage.
        ::Agentmaster::RegisterLiveWindow(_windowId);

        // Debounced autosave of the window record (750ms trailing): structural/lens churn (drag a
        // splitter, reorder tabs, resize the window) collapses to one write; never per keystroke.
        _saveWindowRecordThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 750 },
                .debounce = true,
                .trailing = true,
            },
            [weakThis = get_weak()]() {
                if (auto self = weakThis.get())
                {
                    self->_FlushWindowRecord();
                }
            });

        // Adoption seam, PER WINDOW: a hook for a session we didn't Launch -> try to bind it to
        // its hosting ConPTY so it becomes fully managed (observe + control). The shared
        // registry fans the event out to EVERY window's handler; whichever window hosts the `+`
        // tab binds it, the rest no-op. Detached in ~TerminalPage so a closed window's handler
        // doesn't linger on the process-wide registry.
        {
            const auto weakThis = get_weak();
            _adoptionToken = _sessionRegistry->AddAdoptionHandler([weakThis](const std::wstring& id, const std::wstring& cwd, const std::wstring& tabToken) {
                if (auto self = weakThis.get())
                {
                    self->_AdoptExternalSession(winrt::hstring{ id }, winrt::hstring{ cwd }, winrt::hstring{ tabToken });
                }
            });
        }

        // Liveness probe, PER WINDOW: the shared scanner ticks this on its slow cadence; it
        // marshals to OUR UI thread and archives any of this window's claude tabs whose ConPTY
        // has Closed (a crash / `/exit` that fired no SessionEnd, or a clean SessionEnd that only
        // set Done). Fans out to every window like adoption; each window sweeps only its own tabs.
        if (_scanner)
        {
            const auto weakThis = get_weak();
            _livenessToken = _scanner->AddLivenessProbe([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ReconcileClaudeTabs(); // bind/attach + re-home hooked sessions (tabToken)
                    self->_ObserverProbe(); // Fleet Observer: publish this window's roster + bind via the correlation table (PULL; no hooks needed)
                    self->_SweepClaudeLiveness(); // then archive dead tabs (all self-marshal to the UI thread)
                }
            });
        }

        // M10 autosave triggers (PERSISTENCE.md §13): structural changes (tab add/remove/reorder)
        // and window resize debounce a window-record save. Lens changes come through the content's
        // push (SetLensChangedHandler in _WireAgentManagerContent); tab recolor through
        // _OnClaudeTabColorChanged. _ScheduleWindowRecordSave no-ops until startup completes, so
        // the tabs added during _OnFirstLayout don't thrash saves.
        _tabs.VectorChanged([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                self->_ScheduleWindowRecordSave();
            }
        });
        if (_tabContent)
        {
            _tabContent.SizeChanged([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->_ScheduleWindowRecordSave();
                }
            });
        }
    }

    // Agentmaster: per-directory tab color helpers — the engine persists colors as "#RRGGBB"
    // strings; these convert to/from the WT tab color (alpha forced opaque).
    static std::wstring ClaudeColorToHex(const winrt::Windows::UI::Color& c)
    {
        wchar_t buf[8];
        ::swprintf(buf, 8, L"#%02X%02X%02X", static_cast<unsigned>(c.R), static_cast<unsigned>(c.G), static_cast<unsigned>(c.B));
        return buf;
    }
    static std::optional<winrt::Windows::UI::Color> ClaudeHexToColor(const std::wstring& hexIn)
    {
        std::wstring s = hexIn;
        if (!s.empty() && s.front() == L'#')
        {
            s.erase(0, 1);
        }
        if (s.size() != 6)
        {
            return std::nullopt;
        }
        const auto byteAt = [&s](size_t i) {
            return static_cast<uint8_t>(::wcstoul(s.substr(i, 2).c_str(), nullptr, 16));
        };
        winrt::Windows::UI::Color c{};
        c.A = 255;
        c.R = byteAt(0);
        c.G = byteAt(2);
        c.B = byteAt(4);
        return c;
    }

    // Agentmaster (M10): WT's LaunchMode flags -> a stable JSON token we own both ends of
    // (capture here, restore in the TerminalWindow startup seam). Mirrors WT's own token names.
    static std::wstring LaunchModeToToken(winrt::Microsoft::Terminal::Settings::Model::LaunchMode mode)
    {
        using LM = winrt::Microsoft::Terminal::Settings::Model::LaunchMode;
        if (WI_IsFlagSet(mode, LM::FullscreenMode))
        {
            return L"fullscreen";
        }
        const bool maximized = WI_IsFlagSet(mode, LM::MaximizedMode);
        const bool focus = WI_IsFlagSet(mode, LM::FocusMode);
        if (maximized && focus)
        {
            return L"maximizedFocus";
        }
        if (maximized)
        {
            return L"maximized";
        }
        if (focus)
        {
            return L"focus";
        }
        return L"default";
    }

    // Agentmaster (M10; PERSISTENCE.md §13): build this window's record from LIVE state. Geometry
    // uses the exact recipe as PersistState() (size from the tab content, position asked of the
    // window layer, mode from the focus/maximize flags) — read-only, no side effects. Tabs are
    // captured IN ORDER as REFERENCES: the pinned Manager + transient Settings tabs are skipped; a
    // Claude tab becomes its sessionId (+ runtime color); any other tab is marked Other (its full
    // ActionAndArgs restore blob is only needed by multi-window restore — Increment 3). The
    // Manager lens (rec.manager) is kept current by the content's push, so it's carried verbatim.
    ::Agentmaster::WindowRecord TerminalPage::_CaptureWindowRecord()
    {
        using namespace winrt::Microsoft::Terminal::Settings::Model;

        ::Agentmaster::WindowRecord rec = _windowRecord; // preserves windowId + the cached lens
        rec.windowId = _windowId;

        // --- geometry (same recipe as PersistState) ---
        ::Agentmaster::WindowGeometry geo;
        auto mode = LaunchMode::DefaultMode;
        WI_SetFlagIf(mode, LaunchMode::FullscreenMode, _isFullscreen);
        WI_SetFlagIf(mode, LaunchMode::FocusMode, _isInFocusMode);
        WI_SetFlagIf(mode, LaunchMode::MaximizedMode, _isMaximized);
        geo.launchMode = LaunchModeToToken(mode);

        if (_tabContent)
        {
            const auto w = static_cast<double>(_tabContent.ActualWidth());
            const auto h = static_cast<double>(_tabContent.ActualHeight());
            if (w > 0 && h > 0)
            {
                geo.hasSize = true;
                geo.width = w;
                geo.height = h;
            }
        }

        // We don't know our own position; ask the window layer (exactly as PersistState does).
        const auto launchPosRequest{ winrt::make<LaunchPositionRequest>() };
        RequestLaunchPosition.raise(*this, launchPosRequest);
        // LaunchPosition is a WinRT struct: X/Y are IReference<Int32> FIELDS, not methods.
        if (const auto pos = launchPosRequest.Position(); pos.X)
        {
            geo.hasPosition = true;
            geo.x = static_cast<double>(pos.X.Value());
            geo.y = pos.Y ? static_cast<double>(pos.Y.Value()) : 0.0;
        }
        rec.geometry = geo;

        // --- ordered tab refs (+ which tab is focused) ---
        // The focused tab is persisted by STABLE IDENTITY so it survives a restart: a Claude tab by its
        // conversation id (selectedSessionId), a shell tab — which has no cross-restart id — by index
        // into rec.tabs (selectedTabIndex, the fallback). Manager / none leaves both unset; the Manager
        // tab is re-created at index 0, the natural default. On reopen the window re-selects this tab so
        // closing/reopening preserves the ACTIVE tab, not just the set of tabs.
        rec.tabs.clear();
        rec.selectedSessionId.clear();
        rec.selectedTabIndex = -1;
        TerminalApp::Tab focusedTab{ nullptr };
        if (const auto fi = _GetFocusedTabIndex(); fi && *fi < _tabs.Size())
        {
            focusedTab = _tabs.GetAt(*fi);
        }
        for (const auto& tab : _tabs)
        {
            if ((_managerTab && tab == _managerTab) || (_settingsTab && tab == _settingsTab))
            {
                continue; // the pinned Manager tab is re-created; the Settings tab is transient
            }
            ::Agentmaster::TabEntry entry;
            const auto sessionId = _ClaudeSessionForTab(tab);
            if (!sessionId.empty())
            {
                entry.kind = ::Agentmaster::TabKind::Claude;
                entry.sessionId = sessionId;
                if (const auto t = _GetTabImpl(tab))
                {
                    if (const auto c = t->GetRuntimeTabColor())
                    {
                        entry.tabColor = ClaudeColorToHex(*c);
                    }
                }
            }
            else
            {
                // Other (pwsh / cmd / any non-Claude) tab: capture its full WT restore blob so the
                // reopened window can rebuild it with its title + color + cwd. BuildStartupActions(Persist)
                // is exactly what WT's own PersistState() serializes per tab; wrap this one tab's actions
                // in a WindowLayout and stringify via the public WindowLayout::ToJson (round-tripped on
                // restore by WindowLayout::FromJson -> ProcessStartupActions in _RestoreWindowTabs).
                // Best-effort: a tab that yields no actions stays an empty Other ref (nothing to rebuild).
                entry.kind = ::Agentmaster::TabKind::Other;
                try
                {
                    if (const auto t = _GetTabImpl(tab))
                    {
                        auto tabActions = t->BuildStartupActions(BuildStartupKind::Persist);
                        if (!tabActions.empty())
                        {
                            WindowLayout layout;
                            layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(tabActions)));
                            entry.actionsJson = std::wstring{ WindowLayout::ToJson(layout) };
                        }
                    }
                }
                CATCH_LOG();
            }
            if (focusedTab && tab == focusedTab)
            {
                // Prefer the stable Claude conversation id; a shell tab (no cross-restart id) falls back
                // to its index in rec.tabs. Read entry BEFORE the move below.
                if (entry.kind == ::Agentmaster::TabKind::Claude && !entry.sessionId.empty())
                {
                    rec.selectedSessionId = entry.sessionId;
                }
                else
                {
                    rec.selectedTabIndex = static_cast<int>(rec.tabs.size()); // index this entry will occupy
                }
            }
            rec.tabs.push_back(std::move(entry));
        }

        return rec;
    }

    // Agentmaster (M10): the debounced autosave trigger. No-ops until startup completes so the
    // tabs/geometry churned during _OnFirstLayout don't write a half-built record.
    void TerminalPage::_ScheduleWindowRecordSave()
    {
        if (_startupState != StartupState::Initialized || _windowId.empty())
        {
            return;
        }
        if (_saveWindowRecordThrottled)
        {
            _saveWindowRecordThrottled->Run();
        }
    }

    // Agentmaster (M10): capture + persist synchronously. The throttled autosave funnels here, and
    // so does the close-flush (CloseWindow) so the last move/resize isn't lost past the debounce.
    void TerminalPage::_FlushWindowRecord()
    {
        if (_windowId.empty())
        {
            return; // engine not initialized (no Manager tab) — nothing to persist
        }
        // Anti-clobber (window-grouped restore): never overwrite a good record with a not-yet-started
        // window's nothing. A transient single-instance/handoff window (e.g. a quick reopen that opens
        // then closes before layout) would otherwise capture an EMPTY, geometry-less record over the
        // real one and wipe the saved workspace — breaking the very next reopen. Two cheap guards:
        //  (1) only flush once THIS window finished startup (a pre-Initialized window has no real state);
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        auto rec = _CaptureWindowRecord();
        //  (2) a capture with NEITHER content tabs NOR geometry is an un-laid-out window — keep the
        //      record on disk (the live window's real save follows). A legitimately session-less window
        //      still has geometry, so an empty-but-positioned window (just the Manager tab) still saves.
        if (rec.tabs.empty() && !rec.geometry.hasPosition && !rec.geometry.hasSize)
        {
            return;
        }
        _windowRecord = std::move(rec);
        ::Agentmaster::SaveWindowRecord(_windowRecord);
    }

    // Agentmaster: launch a fresh Claude session (the Manager's "Launch session").
    void TerminalPage::_SpawnClaudeSession(winrt::hstring workingDir, winrt::hstring title)
    {
        _LaunchClaudeSession(workingDir, title, std::nullopt);
    }

    // Agentmaster: launch a claude.exe on a ConPTY in `workingDir`, wired for hooks, as a
    // normal terminal tab, and register it so its hook-driven state is tracked. Both the
    // user (keystrokes) and the orchestrator (Autopilot) write the same stdin.
    //
    // If `restored` is set, this RESUMES that conversation (claude --resume <id>) and
    // restores its Flight Plan + autopilot from persistence (DESIGN §13) — so closing and
    // reopening the app brings the session back exactly as it was. `Sent` prompts are kept
    // Sent (never replayed, Correctness Rule #4).
    TerminalApp::Tab TerminalPage::_LaunchClaudeSession(winrt::hstring workingDir, winrt::hstring title, std::optional<::Agentmaster::SessionInfo> restored)
    {
        if (!_sessionRegistry || !_hooksBridge)
        {
            return nullptr;
        }

        std::wstring dir{ workingDir };
        if (dir.empty())
        {
            wchar_t up[MAX_PATH];
            const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            dir = (n > 0 && n < MAX_PATH) ? std::wstring{ up, n } : std::wstring{ L"C:\\" };
        }

        std::wstring ttl{ title };
        if (ttl.empty())
        {
            // Smart tab/session name: walk past generic bin/obj/Debug/... segments to the first
            // meaningful folder, then apply the length/case rules (see DeriveSessionTitle).
            ttl = ::Agentmaster::DeriveSessionTitle(dir);
        }

        // Resume ONLY if Claude actually has a saved conversation for this id. A session that
        // was opened but never received a prompt has no transcript, so `claude --resume <id>`
        // would fail with "No conversation found" and the tab would die (exit code 1). Such a
        // session is re-launched fresh instead — keeping its working dir + Flight Plan, with a
        // new conversation id. (Correctness Rule #6: restore == resume, never replay.)
        const bool wantResume = restored && !restored->id.empty() && ::Agentmaster::ClaudeConversationExists(restored->id);
        const std::wstring resumeId = wantResume ? restored->id : std::wstring{};
        const auto spec = ::Agentmaster::BuildClaudeSpawn(dir, ttl, _hooksBridge->PipeName(), resumeId, ::Agentmaster::LoadAppSettings());

        // Child environment: CCMGR_SESSION_ID + CCMGR_HOOK_PIPE so hook events correlate
        // back to this session's registry record (HOOKS.md).
        auto envMap = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();
        for (const auto& [k, v] : spec.env)
        {
            envMap.Insert(winrt::hstring{ k }, winrt::hstring{ v });
        }
        // Fleet Observer (§19-Q1): stamp this Launched claude with AM_SESSION = "<processGuid>:<windowId>"
        // so the observer attributes it to THIS window (the inherited process env already carries the
        // bare "<processGuid>"; this overrides it for the child with the owning window appended).
        // ClassifyRunningApp matches on the GUID prefix, so both forms still read as ours.
        if (const auto& eng = ::Agentmaster::SharedEngine(); !eng.amSession.empty() && !_windowId.empty())
        {
            envMap.Insert(winrt::hstring{ L"AM_SESSION" }, winrt::hstring{ eng.amSession + L":" + _windowId });
        }

        // Build the ConPTY connection ourselves (commandline = claude + our hooks settings),
        // then hand it to the normal terminal-pane path as an existing connection. The
        // default profile only supplies appearance; the process/cwd/env are ours.
        auto valueSet = TerminalConnection::ConptyConnection::CreateSettings(
            winrt::hstring{ spec.commandline },
            winrt::hstring{ dir },
            winrt::hstring{ ttl },
            false, // reloadEnvironmentVariables
            L"", // initialEnvironment: inherit our current block
            envMap.GetView(),
            30, // rows  (the control resizes the pty to the pane on first layout)
            120, // cols
            winrt::guid{}, // WT_SESSION (auto-generated by the connection)
            winrt::guid{}); // profileGuid

        TerminalConnection::ConptyConnection connection{};
        connection.Initialize(valueSet);

        Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs{};
        const auto pane = _MakePane(newTerminalArgs, winrt::TerminalApp::Tab{ nullptr }, connection);
        if (!pane)
        {
            return nullptr;
        }
        const auto tab = _CreateNewTabFromPane(pane);
        if (tab)
        {
            // Map sessionId -> tab so the Manager can Activate (jump) / Kill it.
            _claudeTabs[spec.sessionId] = winrt::make_weak(tab);
        }

        // Register the session (restoring its queue + autopilot if resuming) and bind its
        // stdin injector. Correctness Rule #3: the injector is bound to THIS session's id.
        ::Agentmaster::SessionInfo info = restored ? *restored : ::Agentmaster::SessionInfo{};
        info.id = spec.sessionId;
        info.title = ttl;
        info.workingDir = dir;
        info.state = ::Agentmaster::SessionState::Idle; // hooks re-establish the real state
        info.external = false; // we own this tab's ConPTY -> managed, not an adopted session
        info.live = true; // OPEN: has a live tab/claude now -> shows on the Triage Board (not Archived)
        info.pendingConfirmPromptId.clear();
        if (!restored)
        {
            // A NEW session inherits the global Autopilot defaults from the cog; a restored one
            // keeps its persisted AutopilotState (Correctness Rule #6).
            info.autopilot.mode = _appSettings.defaultAutopilotMode;
            info.autopilot.maxAutoSends = _appSettings.maxAutoSends;
            info.autopilot.stopOnError = _appSettings.stopOnError;
            info.autopilot.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
        }
        _sessionRegistry->Upsert(info);

        // Restoring an archived session whose transcript no longer exists yields a FRESH id
        // (restore-fresh). The new (live) record above replaces nothing, so drop the stale
        // archived record under the old id — otherwise it would linger in the Archived list.
        if (restored && !restored->id.empty() && restored->id != spec.sessionId)
        {
            _sessionRegistry->Remove(restored->id);
        }

        _sessionRegistry->SetInjector(spec.sessionId, [connection](const std::wstring& text) {
            const auto* begin = reinterpret_cast<const char16_t*>(text.data());
            connection.WriteInput(winrt::array_view<const char16_t>{ begin, begin + text.size() });
        });

        // Agentmaster: a session's title is ONE value — the Explorer-tree name, the persisted
        // SessionInfo.title, and the WT tab title are the same thing. Pin the tab to it now
        // (SetTabText) so the tab strip shows the managed name instead of floating with claude's
        // volatile OSC title; a later tab rename writes back through _SyncClaudeTitleFromTab, and an
        // Explorer-tree rename re-pins it through _RenameClaudeSession.
        if (tab)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetTabText(winrt::hstring{ ttl });
            }
            // Per-directory tab color: the dir's persisted color, or a stable auto-assigned one.
            _ApplyDirColorToTab(tab, dir);
            // Per-tab "link badge" overlay (TAB_OVERLAY.md): top-right status HUD for this session.
            _AttachClaudeOverlay(tab, spec.sessionId);
        }

        const std::wstring tag = wantResume ? L"[resume] " : (restored ? L"[restore-fresh] " : L"[spawn] ");
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      tag + spec.sessionId + L" \"" + ttl + L"\" cwd=" + dir + L"\n");
        return tab;
    }

    // Agentmaster (TAB_OVERLAY.md): build the per-tab "link badge" overlay for a Claude session and
    // install it into its terminal pane's top-right slot. Gated on AppSettings.showTabOverlay. A
    // re-attach replaces the prior overlay for that id (the old com_ptr's release detaches its
    // observer). Best-effort — a tab with no TerminalPaneContent is left alone.
    void TerminalPage::_AttachClaudeOverlay(const TerminalApp::Tab& tab, const std::wstring& sessionId)
    {
        if (!_appSettings.showTabOverlay || !_sessionRegistry || !tab || sessionId.empty())
        {
            return;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return;
        }
        TerminalApp::TerminalPaneContent termContent{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (termContent)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        termContent = term;
                    }
                }
            });
        }
        if (!termContent)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] " + sessionId + L" NOT attached (no TerminalPaneContent in tab)\n");
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        overlay->Initialize(sessionId, _sessionRegistry);
        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent))
        {
            impl->SetAgentOverlay(overlay->Root());
            _claudeOverlays[sessionId] = overlay; // replaces any prior overlay for this id
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] " + sessionId + L" attached\n");
        }
    }

    // Agentmaster (OBSERVER.md §4/§11d): attach-or-update a registry-LESS "○ <kind> · unlinked" badge
    // on a NON-bound tab the observer classified — a shell ("pwsh" / "cmd"), a never-prompted claude
    // ("claude": correlated but no transcript id yet, §11d), or codex. Keyed by WT_SESSION (there is no
    // sessionId). If a badge already exists for the tab its kind is updated in place (so a pwsh tab
    // that becomes a claude flips pwsh -> claude); the real _AttachClaudeOverlay replaces it once a
    // claude resolves an id. Idempotent — ShowActivity skips a re-render when the kind is unchanged.
    void TerminalPage::_SetTabActivityBadge(const TerminalApp::Tab& tab, const std::wstring& wtSession, const std::wstring& kind)
    {
        if (!_appSettings.showTabOverlay || !tab || wtSession.empty())
        {
            return;
        }
        if (const auto existing = _pendingOverlays.find(wtSession); existing != _pendingOverlays.end())
        {
            if (existing->second)
            {
                existing->second->ShowActivity(kind); // update the kind in place (no-op if unchanged)
            }
            return;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return;
        }
        TerminalApp::TerminalPaneContent termContent{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (termContent)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        termContent = term;
                    }
                }
            });
        }
        if (!termContent)
        {
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        overlay->ShowActivity(kind);
        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent))
        {
            impl->SetAgentOverlay(overlay->Root());
            _pendingOverlays[wtSession] = overlay;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] observe badge kind=" + kind + L" wt=" + wtSession + L"\n");
        }
    }

    // Agentmaster: on startup, load every persisted session into the registry as ARCHIVED
    // (live=false) — and do NOT auto-launch any of them. This is the deliberate reversal of the
    // old "close == reopen" auto-relaunch (Correctness Rule #6): the app opens to just the
    // Manager tab, the prior fleet arrives Archived (restorable as a whole), and the user
    // re-opens what they want from the Manager's "Archived" button (which resumes via
    // `claude --resume`, transcript-gated, in _RestoreArchivedSession). No tabs are created
    // here, so there is nothing to lay out / yield for — the body runs synchronously.
    winrt::fire_and_forget TerminalPage::_RestoreClaudeSessions()
    {
        if (!_sessionRegistry)
        {
            co_return;
        }
        // M9: the registry is a process singleton, so this load is a PROCESS-once action — the
        // first window populates the shared registry from sessions.json; a later window must NOT
        // re-load and double-insert. exchange() makes the first-window election race-free.
        if (::Agentmaster::SharedEngine().restored.exchange(true))
        {
            co_return;
        }

        auto saved = ::Agentmaster::LoadSessions();
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[restore] " + std::to_wstring(saved.size()) + L" session(s) loaded as archived\n");

        for (auto& s : saved)
        {
            if (s.id.empty() || s.workingDir.empty())
            {
                continue;
            }
            // Archived: present + restorable, but no live tab/claude. Reset the transient
            // runtime fields (a fresh process has no live state) and keep the persisted queue +
            // autopilot intact (Rule #6: restore == resume, never replay; Sent stays Sent).
            s.live = false;
            s.external = false;
            s.state = ::Agentmaster::SessionState::Idle;
            s.lastMessageWasQuestion = false;
            s.pendingConfirmPromptId.clear();
            _sessionRegistry->Upsert(std::move(s));
        }
        co_return;
    }

    // Agentmaster (M10 window-grouped restore; PERSISTENCE.md §13 Phase C): re-home THIS window's
    // persisted tabs. A window reopened from its WindowRecord (claimed by id at engine init) comes back
    // with just the pinned Manager tab; this rebuilds the rest from the record's ORDERED tab refs, in
    // place, so "close the window -> reopen it" brings the whole workspace back — not just geometry +
    // lens. Per ref, in record order:
    //   • Claude -> resume its session INTO this window (claude --resume, the archived record's queue +
    //     autopilot intact) via _LaunchClaudeSession — the Archived-button path, targeted here. Lazy-
    //     start safe: the tab is created through the normal pane path (no eager connection.Start()), so
    //     a background restored claude starts its conversation only when first focused (WT's lazy-tab
    //     behavior) — never the eager-Start AV (see Gotchas). A session already live (open in another
    //     window) or unknown to the registry (the process-once fleet load hasn't landed, or it was
    //     pruned) is skipped — it stays in the Archived list rather than being guessed.
    //   • Other -> replay its captured WT startup actions (WindowLayout::FromJson -> ProcessStartup-
    //     Actions) to rebuild the shell tab with its title + color + cwd.
    // Only a CLAIMED record restores (a fresh "+ new window" has nothing to re-home). v1 ordering: the
    // Claude tabs (launched synchronously) land before the Other tabs (whose actions replay on the XAML
    // dispatcher); exact left-to-right interleave of the two kinds is a deferred refinement.
    void TerminalPage::_RestoreWindowTabs()
    {
        if (!_windowRecordClaimed || _windowRecord.tabs.empty() || !_sessionRegistry)
        {
            return;
        }
        using namespace winrt::Microsoft::Terminal::Settings::Model;
        // Snapshot the refs + the focused-tab target FIRST: _LaunchClaudeSession / ProcessStartupActions
        // re-enter capture + autosave, which rewrites _windowRecord (tabs AND the selected* fields) out
        // from under us mid-loop.
        const auto tabs = _windowRecord.tabs;
        const std::wstring selSessionId = _windowRecord.selectedSessionId; // stable Claude id (preferred)
        const int selTabIndex = _windowRecord.selectedTabIndex; // shell-tab fallback; -1 = Manager/none
        const bool wantManager = selSessionId.empty() && selTabIndex < 0;
        size_t resumed = 0, shells = 0, skipped = 0;
        // The focused tab's ABSOLUTE index in _tabs after re-home (Manager is index 0, then the Claude
        // tabs in creation order, then the shell tabs). Computed LIVE from what actually got created —
        // the persisted identity is the stable sessionId (Claude) / record index (shell), never a live
        // position. -1 => unresolved (the selected session couldn't be restored) => leave default focus.
        int targetAbs = wantManager ? 0 : -1;

        // Pass 1 — Claude sessions, synchronously, in record order.
        for (const auto& entry : tabs)
        {
            if (entry.kind != ::Agentmaster::TabKind::Claude || entry.sessionId.empty())
            {
                continue;
            }
            if (_claudeTabs.find(entry.sessionId) != _claudeTabs.end())
            {
                continue; // already bound in this window (re-entrant restore)
            }
            const auto info = _sessionRegistry->Get(entry.sessionId);
            if (!info || info->live)
            {
                ++skipped; // unknown (fleet not loaded yet / pruned) or already open elsewhere
                continue;
            }
            _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
            if (!selSessionId.empty() && entry.sessionId == selSessionId)
            {
                targetAbs = 1 + static_cast<int>(resumed); // this Claude tab's index (Manager occupies 0)
            }
            ++resumed;
        }

        // Pass 2 — Other (shell) tabs: accumulate ALL their captured actions in record order and replay
        // them in ONE ProcessStartupActions call (the WT startup path appends a tab per newTab action).
        // A single call keeps the shells in order AND avoids the re-entrant virtual-cwd save/restore race
        // that per-tab calls would cause (ProcessStartupActions wraps its batch in a window cwd swap).
        std::vector<ActionAndArgs> shellActions;
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            const auto& entry = tabs[i];
            if (entry.kind != ::Agentmaster::TabKind::Other || entry.actionsJson.empty())
            {
                continue;
            }
            try
            {
                const auto layout = WindowLayout::FromJson(winrt::hstring{ entry.actionsJson });
                if (layout)
                {
                    if (const auto tl = layout.TabLayout(); tl && tl.Size() > 0)
                    {
                        for (const auto& a : tl)
                        {
                            shellActions.push_back(a);
                        }
                        if (selSessionId.empty() && selTabIndex == static_cast<int>(i))
                        {
                            targetAbs = 1 + static_cast<int>(resumed) + static_cast<int>(shells); // this shell's index
                        }
                        ++shells;
                    }
                }
            }
            CATCH_LOG();
        }

        // Re-select the focused tab. With shells (created ASYNC by ProcessStartupActions) we append a
        // SwitchToTab as the batch's LAST action so it runs AFTER the shells exist and overrides the
        // "focus the tab I just made" that each newTab does. With no shells the strip is already settled,
        // so select synchronously. targetAbs < 0 (selected session couldn't be restored) => leave default.
        if (!shellActions.empty())
        {
            if (targetAbs >= 0)
            {
                ActionAndArgs sel;
                sel.Action(ShortcutAction::SwitchToTab);
                sel.Args(SwitchToTabArgs{ static_cast<uint32_t>(targetAbs) });
                shellActions.push_back(std::move(sel));
            }
            ProcessStartupActions(std::move(shellActions));
        }
        else if (targetAbs >= 0 && targetAbs < static_cast<int>(_tabs.Size()))
        {
            if (const auto t = _tabs.GetAt(static_cast<uint32_t>(targetAbs)))
            {
                if (const auto& item = t.TabViewItem())
                {
                    _tabView.SelectedItem(item);
                }
            }
        }

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[rehome] window " + _windowId + L" resumed=" + std::to_wstring(resumed) +
                                          L" shells=" + std::to_wstring(shells) + L" skipped=" + std::to_wstring(skipped) +
                                          L" select=" + std::to_wstring(targetAbs) +
                                          L" of " + std::to_wstring(tabs.size()) + L" refs\n");
    }

    // Agentmaster: wire a freshly-created Manager content to the engine. Idempotently
    // ensures the engine exists, then hands the content the registry + the spawn / activate
    // / kill callbacks (all routed back through the page on the UI thread).
    void TerminalPage::_WireAgentManagerContent(const winrt::com_ptr<AgentManagerContent>& content)
    {
        if (!content)
        {
            return;
        }
        _InitAgentmasterEngine();
        content->SetRegistry(_sessionRegistry);
        // Agentmaster (O6): remember the content (weak, as its projected IPaneContent) so
        // _ObserverProbe can push the observer's External (WindowsTerminal) census to it each tick.
        _agentManagerContent = winrt::make_weak(content.as<winrt::TerminalApp::IPaneContent>());

        const auto weakThis = get_weak();
        content->SetSpawnHandler([weakThis](winrt::hstring dir, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_SpawnClaudeSession(dir, title);
            }
        });
        content->SetActivateHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ActivateClaudeSession(id);
            }
        });
        content->SetArchiveHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ArchiveClaudeSession(id);
            }
        });
        content->SetRestoreHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_RestoreArchivedSession(id);
            }
        });
        content->SetRenameHandler([weakThis](winrt::hstring id, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_RenameClaudeSession(id, title);
            }
        });
        // Agentmaster: adopt an EXTERNAL (observe-only) claude from the Explorer Tree's EXTERNAL
        // scope — resolve its conversation id from the transcript and resume it into a managed tab.
        content->SetAdoptExternalHandler([weakThis](uint32_t pid, winrt::hstring cwd) {
            if (auto self = weakThis.get())
            {
                self->_AdoptExternalClaude(pid, cwd);
            }
        });
        // Agentmaster: the Explorer Tree's refresh button — force the Fleet Observer to re-survey NOW
        // (re-enrich the registry + recompute the External census) and redraw, instead of waiting for
        // the next observer/scanner tick. Covers every scope (LOCAL/GLOBAL re-pull + EXTERNAL census).
        content->SetRefreshHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_RefreshObserverData();
            }
        });
        // Agentmaster: surface THIS window's hosted session ids for the Explorer Tree's LOCAL
        // scope. The shared (process-wide) registry holds every window's sessions; _claudeTabs is
        // the per-window subset. Expired weak tabs (torn-down) are skipped so the set is live.
        content->SetLocalScopeProvider([weakThis]() -> std::unordered_set<std::wstring> {
            std::unordered_set<std::wstring> ids;
            if (auto self = weakThis.get())
            {
                for (const auto& [id, weakTab] : self->_claudeTabs)
                {
                    if (weakTab.get())
                    {
                        ids.insert(id);
                    }
                }
            }
            return ids;
        });
        content->SetPauseHandler([weakThis](bool paused) {
            if (auto self = weakThis.get())
            {
                if (self->_scheduler)
                {
                    self->_scheduler->SetGlobalPause(paused);
                }
            }
        });
        content->SetConfirmHandler([weakThis](winrt::hstring id, bool confirm) {
            if (auto self = weakThis.get())
            {
                if (self->_scheduler)
                {
                    self->_scheduler->Confirm(std::wstring{ id }, confirm);
                }
            }
        });
        // Settings cog: seed the dialog with the loaded settings, and persist + apply on Save.
        content->SetSettings(_appSettings);
        content->SetSettingsHandler([weakThis](::Agentmaster::AppSettings s) {
            if (auto self = weakThis.get())
            {
                self->_appSettings = s;
                ::Agentmaster::SaveAppSettings(s);
                // Re-materialize the shared --settings file so model / co-authored-by /
                // permission-mode changes also reach an adopted hand-typed `claude` (the PATH
                // shim points at this file). Fresh spawns rebuild it from settings regardless.
                try
                {
                    ::Agentmaster::MaterializeSharedHookFiles(::Agentmaster::AgentmasterStateDir(), s);
                }
                CATCH_LOG();
            }
        });

        // M10 Increment 3 (PERSISTENCE.md §13.5): the Manager's "Reopen Windows (N)" recover button —
        // reopen saved windows that aren't currently open, the runtime analog of the Emperor's startup
        // reopen loop. Dispatched on the page (it owns the wt-exe new-window path).
        content->SetReopenWindowsHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ReopenSavedWindows();
            }
        });
        // M10 window-grouped restore: the grouped Archived overlay's per-window "Reopen window" button.
        content->SetReopenWindowHandler([weakThis](int index) {
            if (auto self = weakThis.get())
            {
                self->_ReopenSavedWindow(index);
            }
        });
        // Agentmaster (Archive page): the Manager's Archived button opens the full-window Archive page.
        content->SetOpenArchiveHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ShowArchivePage();
            }
        });

        // M10 (PERSISTENCE.md §13): seed this window's Manager lens from its claimed record
        // (selection / scope / collapsed dirs / splitter sizes survive close/reopen), and have the
        // content PUSH lens changes back so the page caches the current lens and debounce-saves the
        // window record. The push carries the lens payload, so _CaptureWindowRecord never has to
        // reach back into the tab to pull it. Only a CLAIMED record seeds — a fresh window keeps the
        // content's ctor-loaded global splitter sizes (no reset-to-default on every new window).
        if (_windowRecordClaimed)
        {
            content->SetManagerState(_windowRecord.manager); // content now matches the cache
        }
        else
        {
            // Fresh window: prime the cache FROM the content's actual lens (incl. its ctor-loaded
            // global splitter sizes) so a save triggered before the first lens push (e.g. a resize)
            // persists the real layout, not a stale default — which would reset splitters on reopen.
            _windowRecord.manager = content->GetManagerState();
        }
        content->SetLensChangedHandler([weakThis](::Agentmaster::ManagerState st) {
            if (auto self = weakThis.get())
            {
                self->_windowRecord.manager = std::move(st);
                self->_ScheduleWindowRecordSave();
            }
        });
    }

    // Agentmaster: jump to (focus) a session's terminal tab. This is the Explorer Tree's
    // "Activate" — it NEVER injects into the connection (Correctness Rule #2).
    void TerminalPage::_ActivateClaudeSession(winrt::hstring sessionId)
    {
        const auto it = _claudeTabs.find(std::wstring{ sessionId });
        if (it == _claudeTabs.end())
        {
            return;
        }
        if (const auto tab = it->second.get())
        {
            if (const auto& item = tab.TabViewItem())
            {
                _tabView.SelectedItem(item);
            }
        }
    }

    // Agentmaster: archive a session (the Manager's Delete / Archive / tree Del / Flight-Plan
    // "Archive"). In the lifecycle model "Delete" == archive: it routes through the SAME
    // tab-close seam as clicking the tab's X, so the one consequence confirm + archive
    // bookkeeping (in _HandleCloseTabRequested -> _ArchiveAndCloseClaudeTab) applies uniformly.
    // The session record is KEPT (live=false) so it persists and lists under "Archived".
    void TerminalPage::_ArchiveClaudeSession(winrt::hstring sessionId)
    {
        const std::wstring id{ sessionId };
        const auto it = _claudeTabs.find(id);
        if (const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr)
        {
            _HandleCloseTabRequested(tab); // -> archive confirm + bookkeeping + close
            return;
        }
        // No live tab in THIS window. The no-tab branch can't actually SHUT DOWN the claude (we don't
        // host its ConPTY), so it must NOT fake an archive on a still-running session (lifecycle gap
        // #2): setting live=false on a claude that's still alive is reverted to live=true by the next
        // Fleet Observer survey (ObserveClaude, Rule #7 — an observed claude is running), bouncing the
        // card back onto the board ~2s later. That alive-but-no-local-tab case is an OURS claude the
        // observer carded a tick before its bind completed, or one hosted by ANOTHER window (the board
        // shows the whole fleet, but per-window actions are local — like Activate). Leave such a
        // session Open: it gets archived from the window that hosts it, or once it binds here the first
        // branch above closes its tab for real. Only a session whose claude has actually EXITED archives
        // here — the legitimate "already closed" cleanup; its process is gone, so nothing revives it.
        if (_sessionRegistry)
        {
            if (const auto info = _sessionRegistry->Get(id); info && info->pid != 0 && ::Agentmaster::ProcessAlive(info->pid))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[archive] " + id + L" not archived here \x2014 claude pid=" + std::to_wstring(info->pid) + L" still alive, not hosted in this window\n");
                return;
            }
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(id, nullptr);
            ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        }
    }

    // Agentmaster: the shared archive seam — confirm the consequence (unless suppressed), do the
    // archive bookkeeping, then close the tab. KEEPING the registry record (live=false) is what
    // makes archive reversible: the session persists and lists under the Manager's "Archived"
    // button for restore. (The old behavior — Remove from the registry + sessions.json — was the
    // irreversible discard the lifecycle model drops: archive is terminal, nothing is forgotten,
    // and the Claude transcript on disk is never touched either.)
    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ArchiveAndCloseClaudeTab(TerminalApp::Tab tab, std::wstring sessionId, bool skipConfirm)
    {
        // Consequence confirm (a buttons-only ContentDialog -> XAML-Islands-safe). Gated by the
        // cog's confirmBeforeKill (relabeled "confirm before archiving"); skipConfirm is set when
        // an aggregate confirmation already ran (e.g. close-other-tabs).
        if (_appSettings.confirmBeforeKill && !skipConfirm)
        {
            if (const auto presenter{ _dialogPresenter.get() })
            {
                std::wstring title;
                if (_sessionRegistry)
                {
                    if (const auto s = _sessionRegistry->Get(sessionId))
                    {
                        title = s->title;
                    }
                }
                ContentDialog dialog;
                dialog.Title(winrt::box_value(L"Archive session?"));
                dialog.Content(winrt::box_value(title.empty() ?
                                                    winrt::hstring{ L"This shuts the session down and moves it to Archived. You can restore it (with its Flight Plan) anytime from the Manager’s “Archived” button." } :
                                                    winrt::hstring{ L"“" + title + L"” will be shut down and moved to Archived. You can restore it (with its Flight Plan) anytime from the Manager’s “Archived” button." }));
                dialog.PrimaryButtonText(L"Archive");
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel

                const auto weak = get_weak();
                const auto result = co_await presenter.ShowDialog(dialog);
                const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
                if (!strong)
                {
                    co_return;
                }
                if (result != ContentDialogResult::Primary)
                {
                    co_return; // cancelled -> leave the session Open
                }
            }
            // No presenter to confirm with -> archive anyway (it's reversible; don't strand the close).
        }

        // Archive bookkeeping: KEEP the record, flip it to Archived, unbind stdin, persist, and
        // drop the sessionId -> tab mapping (so the close below is a plain teardown).
        if (_sessionRegistry)
        {
            _sessionRegistry->Update(sessionId, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(sessionId, nullptr);
            ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        }
        _claudeTabs.erase(sessionId);
        _claudeOverlays.erase(sessionId); // drop the per-tab overlay (detaches its registry observer)
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[archive] " + sessionId + L"\n");

        tab.Close(); // -> Closed -> _RemoveTab (tab.Shutdown disconnects -> claude.exe exits)
        co_return;
    }

    // Agentmaster (lifecycle gap #1): archive THIS window's live Claude sessions as the window tears
    // down. Closing a window (chrome ✕ / Alt+F4 / closeWindow action / quit) destroys its tabs but —
    // unlike the per-tab X — runs NO archive bookkeeping. Without this the sessions linger live=true
    // in the SHARED registry as phantom cards on every OTHER window's Triage Board (Activate is a
    // no-op; the claude is already gone), and worse: each session's injector lambda holds a STRONG
    // ref to its ConptyConnection, so the connection never releases and claude.exe is orphaned past
    // the window. Mirror _ArchiveAndCloseClaudeTab's bookkeeping for every hosted session — minus the
    // confirm dialog and tab.Close() (the tabs go with the window): flip live=false, clear the
    // injector (releases the connection -> claude.exe exits), drop the per-window maps, persist once.
    // Called from the deterministic close seam (CloseWindow, after the confirm) for IMMEDIATE phantom
    // clearing, and again from ~TerminalPage as the catch-all for quit / any other teardown path.
    // Idempotent: the second call finds _claudeTabs already cleared and no-ops; sessions archived
    // earlier via tab-X are already gone from _claudeTabs, so they aren't touched. The shared registry
    // outlives every window (held by SharedEngine), so this stays valid in the destructor.
    void TerminalPage::_ArchiveWindowSessionsOnTeardown()
    {
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            return;
        }
        // Iterate-then-clear (never erase a node mid-iteration): `id` is a ref into the map node, used
        // only inside the loop body; the whole map is cleared AFTER the loop.
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(id, nullptr);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[teardown-archive] " + id + L"\n");
        }
        _claudeTabs.clear();
        _claudeOverlays.clear(); // releases the overlays' com_ptrs (detaches their registry observers)
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
    }

    // Agentmaster: re-launch (resume) an archived session from the Manager's "Archived" list.
    // The record is still in the registry (live=false); _LaunchClaudeSession resumes it
    // (claude --resume <id>, transcript-gated) with its Flight Plan + autopilot, and flips it
    // back to live (Open). A missing transcript yields a fresh id; _LaunchClaudeSession then
    // drops the stale archived record so it doesn't linger in the list.
    void TerminalPage::_RestoreArchivedSession(winrt::hstring sessionId)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto info = _sessionRegistry->Get(std::wstring{ sessionId });
        if (!info || info->live)
        {
            return; // unknown, or already Open
        }
        _LaunchClaudeSession(winrt::hstring{ info->workingDir }, winrt::hstring{ info->title }, *info);
    }

    // ===== Agentmaster: Archive page (full-window redesign) =================================
    // A "page" mounted over TerminalPage's Root (covering the tab strip), opened by the Manager's
    // Archived button (SetOpenArchiveHandler -> _ShowArchivePage). LEFT half = a dense sortable table
    // of archived sessions (+ which saved window each belongs to); RIGHT half = a detail/preview of the
    // selected row (metadata + read-only Flight Plan + restore actions). Back returns to the tabs.
    // Replaces the Manager's old in-content modal overlay. (A slide/fade transition is a deferred
    // polish — v1 toggles Visibility; the page is in the main visual tree so its search/sort typing
    // works, unlike a ContentDialog — the XAML-Islands keyboard trap.)
    namespace
    {
        int64_t ArchiveNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        std::wstring ArchiveLower(std::wstring s)
        {
            for (auto& ch : s)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return s;
        }

        // "now" / "5m" / "3h" / "2d" from a unix-ms timestamp; "" when unknown (0).
        std::wstring ArchiveAgo(int64_t unixMs, int64_t nowMs)
        {
            if (unixMs <= 0)
            {
                return L"";
            }
            int64_t s = (nowMs - unixMs) / 1000;
            if (s < 0)
            {
                s = 0;
            }
            if (s < 60)
            {
                return L"now";
            }
            if (s < 3600)
            {
                return std::to_wstring(s / 60) + L"m";
            }
            if (s < 86400)
            {
                return std::to_wstring(s / 3600) + L"h";
            }
            return std::to_wstring(s / 86400) + L"d";
        }

        winrt::Windows::UI::Xaml::Media::SolidColorBrush ArchiveBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
        {
            return winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b) };
        }

        // A single-line (optionally wrapping) TextBlock with size / weight / opacity. Named to avoid the
        // `Text(...)` helper / `winrt::Windows::UI::Text` namespace clash (a known C2872 gotcha).
        winrt::Windows::UI::Xaml::Controls::TextBlock ArchiveText(winrt::hstring text, double size, bool bold, double opacity, bool wrap = false)
        {
            winrt::Windows::UI::Xaml::Controls::TextBlock tb;
            tb.Text(text);
            tb.FontSize(size);
            if (bold)
            {
                tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            tb.Opacity(opacity);
            tb.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Center);
            if (wrap)
            {
                tb.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
            }
            else
            {
                tb.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::NoWrap);
                tb.TextTrimming(winrt::Windows::UI::Xaml::TextTrimming::CharacterEllipsis);
            }
            return tb;
        }

        // The dense table's 7 columns (shared by the header + every data row): select · Title · Dir ·
        // Branch · Created · Active · Window. Pixel for the fixed ends, star for the elastic middle.
        void ArchiveAddColumns(const winrt::Windows::UI::Xaml::Controls::Grid& g)
        {
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                g.ColumnDefinitions().Append(c);
            };
            col(30, GridUnitType::Pixel); // 0 select
            col(2.2, GridUnitType::Star); // 1 title
            col(3.0, GridUnitType::Star); // 2 dir
            col(1.4, GridUnitType::Star); // 3 branch
            col(52, GridUnitType::Pixel); // 4 created
            col(52, GridUnitType::Pixel); // 5 last activity
            col(58, GridUnitType::Pixel); // 6 window
        }

        // A stable per-window chip color (cycled palette), keyed by the 1-based display ordinal.
        winrt::Windows::UI::Color ArchiveWindowColor(int ordinal)
        {
            static const winrt::Windows::UI::Color kPalette[] = {
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x5E, 0x9C, 0xD6),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x57, 0xA6, 0x73),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xC4, 0x8A, 0x4E),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xB1, 0x6B, 0xC4),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xCB, 0x5C, 0x5C),
                winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x4F, 0xA8, 0xA8),
            };
            const int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
            const int i = ((ordinal - 1) % n + n) % n;
            return kPalette[i];
        }
    }

    // Build the page shell ONCE (host + header + table/detail split + footer), mounted full-bleed over
    // TerminalPage's Root so it covers the tab strip ("the whole window moved a page").
    void TerminalPage::_BuildArchivePageShell()
    {
        if (_archivePageHost)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        Grid host;
        host.Background(ArchiveBrush(0xFF, 0x1B, 0x1B, 0x1B)); // opaque -> fully hides the tabs behind it
        host.RequestedTheme(ElementTheme::Dark);
        host.Visibility(Visibility::Collapsed);
        {
            const auto row = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                host.RowDefinitions().Append(r);
            };
            row(0, GridUnitType::Auto); // 0 header
            row(1, GridUnitType::Star); // 1 body
            row(0, GridUnitType::Auto); // 2 footer
        }

        // --- header: Back · title/counts · search ---
        Grid header;
        header.Margin(Thickness{ 16, 10, 16, 8 });
        {
            const auto hcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                header.ColumnDefinitions().Append(c);
            };
            hcol(0, GridUnitType::Auto); // back
            hcol(1, GridUnitType::Star); // title/counts
            hcol(0, GridUnitType::Auto); // search
        }
        Button back;
        back.Content(winrt::box_value(winrt::hstring{ L"\x2190  Back" }));
        back.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) { _HideArchivePage(); });
        Grid::SetColumn(back, 0);
        header.Children().Append(back);

        StackPanel titleStack;
        titleStack.Margin(Thickness{ 14, 0, 0, 0 });
        titleStack.VerticalAlignment(VerticalAlignment::Center);
        titleStack.Children().Append(ArchiveText(L"Archive", 18, true, 1.0));
        _archiveCountText = ArchiveText(L"", 12, false, 0.6);
        titleStack.Children().Append(_archiveCountText);
        Grid::SetColumn(titleStack, 1);
        header.Children().Append(titleStack);

        TextBox search;
        search.PlaceholderText(L"Search title, dir, branch\x2026");
        search.Width(260);
        search.VerticalAlignment(VerticalAlignment::Center);
        _archiveSearchBox = search;
        search.TextChanged([this](const winrt::Windows::Foundation::IInspectable& s, const TextChangedEventArgs&) {
            if (const auto tb = s.try_as<TextBox>())
            {
                _archiveFilter = ArchiveLower(std::wstring{ tb.Text() });
                _RenderArchiveTable();
            }
        });
        Grid::SetColumn(search, 2);
        header.Children().Append(search);
        Grid::SetRow(header, 0);
        host.Children().Append(header);

        // --- body: 50/50 table | detail, divided by a thin separator ---
        Grid body;
        body.Margin(Thickness{ 16, 0, 16, 0 });
        {
            const auto bcol = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                body.ColumnDefinitions().Append(c);
            };
            bcol(1, GridUnitType::Star); // table
            bcol(0, GridUnitType::Auto); // separator
            bcol(1, GridUnitType::Star); // detail
        }

        // LEFT — sortable header over a scrolling rows host.
        Grid left;
        {
            const auto lrow = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                left.RowDefinitions().Append(r);
            };
            lrow(0, GridUnitType::Auto); // column header
            lrow(1, GridUnitType::Star); // rows
        }
        _archiveHeaderRow = Grid{};
        Grid::SetRow(_archiveHeaderRow, 0);
        left.Children().Append(_archiveHeaderRow);
        _archiveRowsHost = StackPanel{};
        _archiveRowsHost.Spacing(2);
        ScrollViewer leftScroll;
        leftScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        leftScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        leftScroll.Padding(Thickness{ 0, 0, 8, 0 });
        leftScroll.Content(_archiveRowsHost);
        Grid::SetRow(leftScroll, 1);
        left.Children().Append(leftScroll);
        Grid::SetColumn(left, 0);
        body.Children().Append(left);

        Border sep;
        sep.Width(1);
        sep.Background(ArchiveBrush(0x30, 0xC0, 0xC0, 0xC0));
        sep.Margin(Thickness{ 10, 4, 10, 4 });
        Grid::SetColumn(sep, 1);
        body.Children().Append(sep);

        // RIGHT — detail/preview.
        _archiveDetailHost = StackPanel{};
        _archiveDetailHost.Spacing(6);
        ScrollViewer rightScroll;
        rightScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        rightScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        rightScroll.Padding(Thickness{ 12, 0, 4, 0 });
        rightScroll.Content(_archiveDetailHost);
        Grid::SetColumn(rightScroll, 2);
        body.Children().Append(rightScroll);

        Grid::SetRow(body, 1);
        host.Children().Append(body);

        // --- footer: bulk restore (right-aligned) ---
        StackPanel footer;
        footer.Orientation(Orientation::Horizontal);
        footer.HorizontalAlignment(HorizontalAlignment::Right);
        footer.Spacing(8);
        footer.Margin(Thickness{ 16, 10, 16, 10 });
        _archiveRestoreSelBtn = Button{};
        _archiveRestoreSelBtn.Content(winrt::box_value(winrt::hstring{ L"Restore selected" }));
        _archiveRestoreSelBtn.IsEnabled(false);
        _archiveRestoreSelBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) { _RestoreCheckedArchived(); });
        footer.Children().Append(_archiveRestoreSelBtn);
        Grid::SetRow(footer, 2);
        host.Children().Append(footer);

        // Mount full-bleed over Root (RowSpan all 3 rows -> covers the tab strip + content).
        this->Root().Children().Append(host);
        Grid::SetRow(host, 0);
        Grid::SetRowSpan(host, 3);
        _archivePageHost = host;
    }

    void TerminalPage::_ShowArchivePage()
    {
        _BuildArchivePageShell();
        if (!_archivePageHost)
        {
            return;
        }
        _GatherArchiveRows();
        _RenderArchiveTable();
        _archivePageHost.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
    }

    void TerminalPage::_HideArchivePage()
    {
        if (_archivePageHost)
        {
            _archivePageHost.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        }
    }

    // Gather the archive data set: every saved-window record's archived Claude sessions (tagged with that
    // window's index + a 1-based display ordinal), then the loose archived sessions (in no record). Each
    // row's timing comes from a cheap transcript stat (TranscriptTimes). Reads disk -> called on show +
    // after an action, NOT per keystroke (the filter/sort in _RenderArchiveTable run over this cache).
    void TerminalPage::_GatherArchiveRows()
    {
        _archiveRows.clear();
        if (!_sessionRegistry)
        {
            return;
        }
        const auto sessions = _sessionRegistry->Snapshot();
        std::vector<::Agentmaster::RecoverableWindow> recoverable;
        try
        {
            recoverable = ::Agentmaster::RecoverableWindows();
        }
        CATCH_LOG();

        const auto findArchived = [&sessions](const std::wstring& id) -> const ::Agentmaster::SessionInfo* {
            for (const auto& s : sessions)
            {
                if (!s.live && s.id == id)
                {
                    return &s;
                }
            }
            return nullptr;
        };
        const auto buildRow = [](const ::Agentmaster::SessionInfo& s, int windowIndex, int windowOrdinal) {
            _ArchiveRow r;
            r.id = s.id;
            r.title = s.title;
            r.dir = s.workingDir;
            r.branch = s.branch;
            r.windowIndex = windowIndex;
            r.windowOrdinal = windowOrdinal;
            for (const auto& p : s.queue)
            {
                ++r.totalCount;
                if (p.status == ::Agentmaster::PromptStatus::Sent)
                {
                    ++r.sentCount;
                }
            }
            int64_t created = 0, last = 0;
            if (::Agentmaster::TranscriptTimes(s.workingDir, s.id, created, last))
            {
                r.createdUnixMs = created;
                r.lastActivityUnixMs = last;
            }
            return r;
        };

        std::unordered_set<std::wstring> grouped;
        int ordinal = 0;
        for (const auto& rw : recoverable)
        {
            std::vector<const ::Agentmaster::SessionInfo*> winSessions;
            for (const auto& t : rw.record.tabs)
            {
                if (t.kind == ::Agentmaster::TabKind::Claude && !t.sessionId.empty())
                {
                    if (const auto* hit = findArchived(t.sessionId))
                    {
                        winSessions.push_back(hit);
                    }
                }
            }
            if (winSessions.empty())
            {
                continue; // a window whose archived Claude sessions are all gone (only shells / reopened)
            }
            ++ordinal;
            for (const auto* s : winSessions)
            {
                _archiveRows.push_back(buildRow(*s, rw.index, ordinal));
                grouped.insert(s->id);
            }
        }
        for (const auto& s : sessions)
        {
            if (s.live || grouped.find(s.id) != grouped.end())
            {
                continue;
            }
            _archiveRows.push_back(buildRow(s, -1, 0));
        }
    }

    // Apply the search filter + the active sort to _archiveRows and (re)build the column header + data
    // rows. Also keeps the selection valid (defaults to the first row) and drives the detail pane.
    void TerminalPage::_RenderArchiveTable()
    {
        if (!_archiveRowsHost || !_archiveHeaderRow)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        const int64_t now = ArchiveNowMs();

        // --- sortable column header ---
        _archiveHeaderRow.Children().Clear();
        _archiveHeaderRow.ColumnDefinitions().Clear();
        ArchiveAddColumns(_archiveHeaderRow);
        _archiveHeaderRow.Margin(Thickness{ 8, 0, 8, 4 });
        const auto addHeader = [this](int col, winrt::hstring label, bool sortable) {
            if (!sortable)
            {
                auto t = ArchiveText(label, 11, true, 0.5);
                Grid::SetColumn(t, col);
                _archiveHeaderRow.Children().Append(t);
                return;
            }
            winrt::hstring arrow{};
            if (_archiveSortColumn == col)
            {
                arrow = _archiveSortAscending ? winrt::hstring{ L" \x25B2" } : winrt::hstring{ L" \x25BC" };
            }
            Button b;
            b.Background(ArchiveBrush(0, 0, 0, 0));
            b.BorderThickness(Thickness{ 0, 0, 0, 0 });
            b.Padding(Thickness{ 0, 0, 0, 0 });
            b.MinWidth(0);
            b.MinHeight(0);
            b.HorizontalAlignment(HorizontalAlignment::Left);
            b.HorizontalContentAlignment(HorizontalAlignment::Left);
            b.Content(ArchiveText(label + arrow, 11, true, 0.7));
            b.Click([this, col](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                if (_archiveSortColumn == col)
                {
                    _archiveSortAscending = !_archiveSortAscending;
                }
                else
                {
                    _archiveSortColumn = col;
                    _archiveSortAscending = (col == 1 || col == 2 || col == 3); // text ascending, time/window descending
                }
                _RenderArchiveTable();
            });
            Grid::SetColumn(b, col);
            _archiveHeaderRow.Children().Append(b);
        };
        addHeader(0, L"", false);
        addHeader(1, L"Title", true);
        addHeader(2, L"Directory", true);
        addHeader(3, L"Branch", true);
        addHeader(4, L"Created", true);
        addHeader(5, L"Active", true);
        addHeader(6, L"Window", true);

        // --- filter + sort over the gathered rows ---
        std::vector<const _ArchiveRow*> view;
        for (const auto& r : _archiveRows)
        {
            if (!_archiveFilter.empty())
            {
                const std::wstring hay = ArchiveLower(r.title) + L"\n" + ArchiveLower(r.dir) + L"\n" + ArchiveLower(r.branch);
                if (hay.find(_archiveFilter) == std::wstring::npos)
                {
                    continue;
                }
            }
            view.push_back(&r);
        }
        const int sortCol = _archiveSortColumn;
        const bool asc = _archiveSortAscending;
        std::sort(view.begin(), view.end(), [sortCol, asc](const _ArchiveRow* a, const _ArchiveRow* b) {
            const auto cmpS = [](const std::wstring& x, const std::wstring& y) {
                const auto lx = ArchiveLower(x), ly = ArchiveLower(y);
                return lx < ly ? -1 : (lx > ly ? 1 : 0);
            };
            const auto cmpI = [](int64_t x, int64_t y) { return x < y ? -1 : (x > y ? 1 : 0); };
            int c = 0;
            switch (sortCol)
            {
            case 1:
                c = cmpS(a->title, b->title);
                break;
            case 2:
                c = cmpS(a->dir, b->dir);
                break;
            case 3:
                c = cmpS(a->branch, b->branch);
                break;
            case 4:
                c = cmpI(a->createdUnixMs, b->createdUnixMs);
                break;
            case 5:
                c = cmpI(a->lastActivityUnixMs, b->lastActivityUnixMs);
                break;
            case 6:
                c = cmpI(a->windowOrdinal, b->windowOrdinal);
                break;
            default:
                break;
            }
            if (c == 0)
            {
                return a->createdUnixMs > b->createdUnixMs; // stable tiebreak: newest first
            }
            return asc ? (c < 0) : (c > 0);
        });

        // --- keep a valid selection (default to the first visible row) ---
        bool selValid = false;
        for (const auto* r : view)
        {
            if (r->id == _archiveSelectedId)
            {
                selValid = true;
                break;
            }
        }
        if (!selValid)
        {
            _archiveSelectedId = view.empty() ? std::wstring{} : view.front()->id;
        }

        // --- data rows ---
        _archiveRowsHost.Children().Clear();
        if (view.empty())
        {
            _archiveRowsHost.Children().Append(ArchiveText(_archiveRows.empty() ?
                                                               winrt::hstring{ L"No archived sessions. Closing a session's tab archives it here." } :
                                                               winrt::hstring{ L"No matches." },
                                                           12, false, 0.6, true));
        }
        for (const auto* rp : view)
        {
            const _ArchiveRow& r = *rp;
            const std::wstring rid = r.id;
            Grid g;
            ArchiveAddColumns(g);

            CheckBox cb;
            cb.MinWidth(0);
            cb.VerticalAlignment(VerticalAlignment::Center);
            cb.IsChecked(_archiveChecked.find(r.id) != _archiveChecked.end()); // set BEFORE handlers (no spurious fire)
            cb.Checked([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                _archiveChecked.insert(rid);
                _UpdateArchiveBulkButton();
            });
            cb.Unchecked([this, rid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                _archiveChecked.erase(rid);
                _UpdateArchiveBulkButton();
            });
            Grid::SetColumn(cb, 0);
            g.Children().Append(cb);

            auto title = ArchiveText(r.title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ r.title }, 13, true, 0.95);
            title.Margin(Thickness{ 2, 0, 6, 0 });
            Grid::SetColumn(title, 1);
            g.Children().Append(title);
            auto dir = ArchiveText(winrt::hstring{ r.dir }, 12, false, 0.6);
            dir.Margin(Thickness{ 0, 0, 6, 0 });
            Grid::SetColumn(dir, 2);
            g.Children().Append(dir);
            auto br = ArchiveText(winrt::hstring{ r.branch }, 12, false, 0.55);
            br.Margin(Thickness{ 0, 0, 6, 0 });
            Grid::SetColumn(br, 3);
            g.Children().Append(br);
            auto cr = ArchiveText(winrt::hstring{ ArchiveAgo(r.createdUnixMs, now) }, 11, false, 0.6);
            Grid::SetColumn(cr, 4);
            g.Children().Append(cr);
            auto la = ArchiveText(winrt::hstring{ ArchiveAgo(r.lastActivityUnixMs, now) }, 11, false, 0.6);
            Grid::SetColumn(la, 5);
            g.Children().Append(la);
            if (r.windowOrdinal > 0)
            {
                Border chip;
                chip.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush{ ArchiveWindowColor(r.windowOrdinal) });
                chip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 3, 3, 3, 3 });
                chip.Padding(Thickness{ 5, 1, 5, 1 });
                chip.HorizontalAlignment(HorizontalAlignment::Left);
                chip.VerticalAlignment(VerticalAlignment::Center);
                auto wt = ArchiveText(winrt::hstring{ L"W" + std::to_wstring(r.windowOrdinal) }, 10, true, 1.0);
                chip.Child(wt);
                Grid::SetColumn(chip, 6);
                g.Children().Append(chip);
            }

            Border row;
            row.Padding(Thickness{ 8, 5, 8, 5 });
            row.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            row.Background(r.id == _archiveSelectedId ? ArchiveBrush(0x50, 0x4A, 0x6E, 0xA8) : ArchiveBrush(0x14, 0x80, 0x80, 0x80));
            row.Child(g);
            row.Tapped([this, rid](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
                _archiveSelectedId = rid;
                _RenderArchiveTable();
            });
            _archiveRowsHost.Children().Append(row);
        }

        // --- header counts ---
        if (_archiveCountText)
        {
            std::unordered_set<int> ws;
            for (const auto& r : _archiveRows)
            {
                if (r.windowOrdinal > 0)
                {
                    ws.insert(r.windowOrdinal);
                }
            }
            std::wstring counts = std::to_wstring(_archiveRows.size()) + (_archiveRows.size() == 1 ? L" archived session" : L" archived sessions");
            if (!ws.empty())
            {
                counts += L"  \x00B7  " + std::to_wstring(ws.size()) + (ws.size() == 1 ? L" saved window" : L" saved windows");
            }
            _archiveCountText.Text(winrt::hstring{ counts });
        }

        _UpdateArchiveBulkButton();
        _ShowArchiveDetail(_archiveSelectedId);
    }

    // Populate the right pane for one archived session: metadata + a read-only Flight Plan (the persisted
    // queue; the transcript's human prompts as a fallback) + restore actions.
    void TerminalPage::_ShowArchiveDetail(const std::wstring& id)
    {
        if (!_archiveDetailHost)
        {
            return;
        }
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        _archiveDetailHost.Children().Clear();
        if (id.empty() || !_sessionRegistry)
        {
            _archiveDetailHost.Children().Append(ArchiveText(L"Select a session to preview its details and Flight Plan.", 12, false, 0.6, true));
            return;
        }
        const auto info = _sessionRegistry->Get(id);
        if (!info)
        {
            _archiveDetailHost.Children().Append(ArchiveText(L"This session is no longer available.", 12, false, 0.6, true));
            return;
        }

        const _ArchiveRow* row = nullptr;
        for (const auto& r : _archiveRows)
        {
            if (r.id == id)
            {
                row = &r;
                break;
            }
        }

        // Out-of-band transcript read (one row, on select) for branch + prompts + authoritative timing.
        ::Agentmaster::TranscriptInfo ti{};
        try
        {
            ti = ::Agentmaster::ReadTranscriptInfo(info->workingDir, id, 131072, 60);
        }
        CATCH_LOG();

        _archiveDetailHost.Children().Append(ArchiveText(info->title.empty() ? winrt::hstring{ L"(untitled)" } : winrt::hstring{ info->title }, 18, true, 1.0, true));
        _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ info->workingDir }, 12, false, 0.7, true));

        const int64_t now = ArchiveNowMs();
        const int64_t created = ti.createdUnixMs ? ti.createdUnixMs : (row ? row->createdUnixMs : 0);
        const int64_t last = ti.lastActivityUnixMs ? ti.lastActivityUnixMs : (row ? row->lastActivityUnixMs : 0);
        const std::wstring branch = !info->branch.empty() ? info->branch : std::wstring{ ti.gitBranch };
        std::wstring meta;
        if (!branch.empty())
        {
            meta += L"\x2387 " + branch + L"    ";
        }
        if (created)
        {
            meta += L"created " + ArchiveAgo(created, now) + L" ago";
        }
        if (last)
        {
            meta += (created ? std::wstring{ L"   \x00B7   " } : std::wstring{}) + L"last active " + ArchiveAgo(last, now) + L" ago";
        }
        if (!meta.empty())
        {
            _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ meta }, 12, false, 0.6, true));
        }

        {
            Border d;
            d.Height(1);
            d.Background(ArchiveBrush(0x24, 0xC0, 0xC0, 0xC0));
            d.Margin(Thickness{ 0, 8, 0, 4 });
            _archiveDetailHost.Children().Append(d);
        }

        _archiveDetailHost.Children().Append(ArchiveText(L"Flight Plan (read-only)", 13, true, 0.9));
        if (!info->queue.empty())
        {
            for (const auto& p : info->queue)
            {
                winrt::hstring tag;
                switch (p.status)
                {
                case ::Agentmaster::PromptStatus::Sent:
                    tag = L"\x2713 ";
                    break;
                case ::Agentmaster::PromptStatus::Held:
                    tag = L"\x23F8 ";
                    break;
                case ::Agentmaster::PromptStatus::Failed:
                    tag = L"\x2717 ";
                    break;
                case ::Agentmaster::PromptStatus::Skipped:
                    tag = L"\x2014 ";
                    break;
                default:
                    tag = L"\x2022 ";
                    break;
                }
                const std::wstring bodyText = !p.label.empty() ? std::wstring{ p.label } : std::wstring{ p.text };
                const winrt::hstring suffix = (p.origin == ::Agentmaster::PromptOrigin::Typed) ? winrt::hstring{ L"   (typed)" } : winrt::hstring{};
                _archiveDetailHost.Children().Append(ArchiveText(tag + winrt::hstring{ bodyText } + suffix, 12, false, 0.8, true));
            }
        }
        else if (!ti.userPrompts.empty())
        {
            for (const auto& up : ti.userPrompts)
            {
                _archiveDetailHost.Children().Append(ArchiveText(winrt::hstring{ L"\x2023 " + up }, 12, false, 0.75, true));
            }
        }
        else
        {
            _archiveDetailHost.Children().Append(ArchiveText(L"(no recorded prompts)", 12, false, 0.5, true));
        }

        {
            Border d;
            d.Height(1);
            d.Background(ArchiveBrush(0x24, 0xC0, 0xC0, 0xC0));
            d.Margin(Thickness{ 0, 10, 0, 6 });
            _archiveDetailHost.Children().Append(d);
        }
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(8);
        Button restore;
        restore.Content(winrt::box_value(winrt::hstring{ L"Restore here" }));
        const winrt::hstring hid{ id };
        restore.Click([this, hid](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            _RestoreArchivedSession(hid);
            _HideArchivePage();
        });
        actions.Children().Append(restore);
        if (row && row->windowIndex >= 0)
        {
            Button reopen;
            reopen.Content(winrt::box_value(winrt::hstring{ L"Reopen its window" }));
            const int idx = row->windowIndex;
            reopen.Click([this, idx](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                _ReopenSavedWindow(idx);
                _HideArchivePage();
            });
            actions.Children().Append(reopen);
        }
        _archiveDetailHost.Children().Append(actions);
    }

    // Bulk: restore every checked archived session into the current window, then close the page.
    void TerminalPage::_RestoreCheckedArchived()
    {
        if (_archiveChecked.empty())
        {
            return;
        }
        const std::vector<std::wstring> ids(_archiveChecked.begin(), _archiveChecked.end());
        for (const auto& id : ids)
        {
            _RestoreArchivedSession(winrt::hstring{ id });
        }
        _archiveChecked.clear();
        _HideArchivePage();
    }

    void TerminalPage::_UpdateArchiveBulkButton()
    {
        if (!_archiveRestoreSelBtn)
        {
            return;
        }
        const auto n = _archiveChecked.size();
        _archiveRestoreSelBtn.Content(winrt::box_value(n > 0 ?
                                                           winrt::hstring{ L"Restore selected (" + std::to_wstring(n) + L")" } :
                                                           winrt::hstring{ L"Restore selected" }));
        _archiveRestoreSelBtn.IsEnabled(n > 0);
    }

    // Agentmaster (Fleet Observer): adopt an EXTERNAL (observe-only) claude from the Explorer Tree's
    // EXTERNAL scope. We host no ConPTY for the foreign process, so we can NEVER inject into the live
    // external (Rule #9/#13) — "adopt" instead brings its CONVERSATION under management: resolve the
    // conversation id from the transcript (cwd + process-start -> ResolveSessionId, Rule #14) and
    // resume it into a NEW managed, controllable tab (claude --resume <id>, with a Flight Plan +
    // Autopilot), reusing the proven _LaunchClaudeSession resume path. When the external has no
    // transcript yet (never prompted -> id ""), fall back to a fresh managed session in the same dir
    // (== "Open New Session Here"). The original external process is left running and untouched — we never inject
    // into or kill it; the user closes it to avoid two writers on one transcript. _LaunchClaudeSession
    // re-checks ClaudeConversationExists, so a transcript that vanished between resolve and launch also
    // degrades to fresh rather than dying on "No conversation found".
    void TerminalPage::_AdoptExternalClaude(uint32_t pid, winrt::hstring cwd)
    {
        const std::wstring dir{ cwd };
        const int64_t start = ::Agentmaster::ProcessStartUnixMs(pid);
        const std::wstring id = ::Agentmaster::ResolveSessionId(dir, start);

        std::optional<::Agentmaster::SessionInfo> restored;
        if (!id.empty())
        {
            ::Agentmaster::SessionInfo info{};
            info.id = id; // _LaunchClaudeSession transcript-gates the --resume on this id
            info.workingDir = dir;
            // Adopted sessions are new to us (no persisted plan), so seed the cog's Autopilot defaults
            // as a fresh launch would — but resume the external's existing conversation.
            info.autopilot.mode = _appSettings.defaultAutopilotMode;
            info.autopilot.maxAutoSends = _appSettings.maxAutoSends;
            info.autopilot.stopOnError = _appSettings.stopOnError;
            info.autopilot.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
            restored = std::move(info);
        }
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      std::wstring{ L"[adopt-external] pid=" } + std::to_wstring(pid) + L" cwd=" + dir +
                                          L" -> " + (id.empty() ? std::wstring{ L"(fresh \x2014 no transcript)" } : (L"resume " + id)) + L"\n");
        _LaunchClaudeSession(cwd, winrt::hstring{}, restored);
    }

    // Agentmaster: which session (if any) hosts this tab? Reverse-lookup of _claudeTabs (whose
    // forward direction is sessionId -> tab). Used by the tab-close seam to recognize a Claude
    // session tab so it archives instead of plain-closing.
    std::wstring TerminalPage::_ClaudeSessionForTab(const TerminalApp::Tab& tab)
    {
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            if (const auto t = weakTab.get(); t && t == tab)
            {
                return id;
            }
        }
        return {};
    }

    // Agentmaster: rename a Claude session from the Manager's Explorer Tree. A session's title is
    // ONE value — the Explorer-tree name, the persisted SessionInfo.title, and the WT tab title.
    // Write it to the shared registry (which persists it via the autosave-on-change observer and
    // refreshes every window's Triage Board / Explorer Tree / Flight Plan) and, when THIS window
    // hosts the session's tab, retitle the tab strip to match. The tab's own rename path mirrors
    // the other direction (_SyncClaudeTitleFromTab); the SetTabText below re-enters it once and
    // settles immediately (the registry title already equals the new text).
    void TerminalPage::_RenameClaudeSession(winrt::hstring sessionId, winrt::hstring title)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const std::wstring id{ sessionId };
        const std::wstring name{ title };
        _sessionRegistry->Update(id, [&name](::Agentmaster::SessionInfo& s) { s.title = name; });

        const auto it = _claudeTabs.find(id);
        if (const auto tab = (it != _claudeTabs.end()) ? it->second.get() : nullptr)
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetTabText(title);
            }
        }
    }

    // Agentmaster: the reverse direction — a Claude TAB was renamed (the header double-click, the
    // right-click "Rename Tab", or the renameTab action; all funnel through Tab::SetTabText ->
    // PropertyChanged("Title") -> _UpdateTitle -> here). Mirror the tab's text into the session's
    // title so the Explorer-tree name and the persisted record track the tab. Because every Claude
    // tab is pinned to its managed name at launch, a non-empty runtime tab text that differs from
    // the registry title can only be a user rename; an emptied one (a bare renameTab / ResetTabText)
    // is re-pinned so the tab never falls back to claude's volatile OSC title and diverges.
    void TerminalPage::_SyncClaudeTitleFromTab(const TerminalApp::Tab& tab)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // not a Claude session tab (or not yet mapped) -> nothing to sync
        }
        const auto impl = _GetTabImpl(tab);
        if (!impl)
        {
            return;
        }
        const std::wstring text{ impl->GetTabText() };
        const auto info = _sessionRegistry->Get(id);
        if (text.empty())
        {
            // Override cleared (ResetTabText) -> re-pin to the managed name. Re-enters once, then
            // settles (GetTabText() == title -> the equality guard below returns without writing).
            if (info && !info->title.empty())
            {
                impl->SetTabText(winrt::hstring{ info->title });
            }
            return;
        }
        if (info && info->title == text)
        {
            return; // already in sync (our own pin / an Explorer-driven rename) -> no write, no loop
        }
        _sessionRegistry->Update(id, [&text](::Agentmaster::SessionInfo& s) { s.title = text; });
    }

    // Agentmaster: paint a Claude tab from its working directory's color — the persisted color for
    // the dir if any, else a stable auto-assigned one (which we persist so it survives + is shared
    // by the dir). SetRuntimeTabColor re-enters _OnClaudeTabColorChanged once, which settles
    // immediately (the persisted color now equals the tab's color), so this never loops.
    void TerminalPage::_ApplyDirColorToTab(const TerminalApp::Tab& tab, const std::wstring& dir)
    {
        auto hex = ::Agentmaster::GetDirColor(dir);
        if (!hex)
        {
            hex = ::Agentmaster::AutoDirColorHex(dir);
            ::Agentmaster::SetDirColor(dir, hex); // persist the auto color -> stable + shared by the dir
        }
        if (const auto color = ClaudeHexToColor(*hex))
        {
            if (const auto impl = _GetTabImpl(tab))
            {
                impl->SetRuntimeTabColor(*color);
            }
        }
    }

    // Agentmaster: recolor every live Claude tab whose session shares `dir` (filesystem-aware match)
    // to `colorHex`, or reset them when nullopt. Fans a user's color change across the directory.
    void TerminalPage::_ApplyDirColorToTabs(const std::wstring& dir, const std::optional<std::wstring>& colorHex)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        std::optional<winrt::Windows::UI::Color> color;
        if (colorHex)
        {
            color = ClaudeHexToColor(*colorHex);
        }
        const std::wstring key = ::Agentmaster::NormDirKey(dir);
        for (const auto& [sid, weakTab] : _claudeTabs)
        {
            const auto info = _sessionRegistry->Get(sid);
            if (!info || ::Agentmaster::NormDirKey(info->workingDir) != key)
            {
                continue;
            }
            if (const auto t = weakTab.get())
            {
                if (const auto impl = _GetTabImpl(t))
                {
                    if (color)
                    {
                        impl->SetRuntimeTabColor(*color);
                    }
                    else
                    {
                        impl->ResetRuntimeTabColor();
                    }
                }
            }
        }
    }

    // Agentmaster: the user changed a Claude tab's color (color picker / setTabColor action ->
    // Tab::SetRuntimeTabColor/Reset -> TabColorChanged -> here). Color is ONE value per working
    // directory: persist it for the dir and fan it out to every live tab in that dir. The de-dupe
    // vs the persisted color makes our own launch/propagation writes no-ops (no loop).
    void TerminalPage::_OnClaudeTabColorChanged(const TerminalApp::Tab& tab)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // not a Claude session tab
        }
        const auto info = _sessionRegistry->Get(id);
        if (!info)
        {
            return;
        }
        const std::wstring dir = info->workingDir;

        std::optional<std::wstring> newHex;
        if (const auto impl = _GetTabImpl(tab))
        {
            if (const auto c = impl->GetRuntimeTabColor())
            {
                newHex = ClaudeColorToHex(*c);
            }
        }
        if (::Agentmaster::GetDirColor(dir) == newHex)
        {
            return; // already in sync (our own launch/propagation write) -> no persist, no loop
        }
        ::Agentmaster::SetDirColor(dir, newHex); // upsert the color, or drop it on reset
        _ApplyDirColorToTabs(dir, newHex); // every live tab in this dir tracks the change
        _ScheduleWindowRecordSave(); // M10: the per-tab color rides in the window record
    }

    // Agentmaster: keep the pinned, non-closable Manager tab at index 0 after any reorder. Tab
    // creation appends (the Manager is created first), so the only ways it can drift are a tab
    // drag-drop or a move-tab action; this snaps it back. No-op when it is already first.
    void TerminalPage::_PinManagerTabFirst()
    {
        if (!_managerTab)
        {
            return;
        }
        uint32_t idx{};
        if (_tabs.IndexOf(_managerTab, idx) && idx != 0)
        {
            auto tab = _managerTab;
            const auto tvi = tab.TabViewItem();
            _tabs.RemoveAt(idx);
            _tabs.InsertAt(0, tab);
            try
            {
                uint32_t viewIdx{};
                if (tvi && _tabView.TabItems().IndexOf(tvi, viewIdx))
                {
                    _tabView.TabItems().RemoveAt(viewIdx);
                    _tabView.TabItems().InsertAt(0, tvi);
                }
            }
            CATCH_LOG();
            _UpdateTabIndices();
        }
    }

    // Agentmaster: reconcile a session's tab binding on a SessionStart, keyed by the STABLE
    // WT_SESSION `tabToken` (the ConPTY identity, which — unlike the Claude session id — never
    // changes for the life of a tab). Now fired for EVERY SessionStart, this one path handles:
    //   * ADOPT  — a `claude` the user typed into a `+` tab (a new, unknown id wired by the PATH
    //              shim): find the live ConPTY whose WT_SESSION == tabToken and bind a stdin
    //              injector to it, promoting it to full observe+control (Rule #3).
    //   * RE-HOME — a tab whose claude changed conversation id via the in-session `/resume` (the id
    //              changes, the tabToken does not). The same tab is found bound to an OLD id; that
    //              old id is archived (queue kept restorable) and the tab is re-pointed to the new
    //              id — even if the new id is a previously-known/archived one (which never creates a
    //              record here, so the old new-record-only adoption gate missed it).
    // Idempotent: a SessionStart for an already-bound session (every Manager-launched one) fast-
    // returns. Best-effort: a claude with no matching connection in this window stays observe-only
    // and the registry is left untouched. Runs on the UI thread (XAML walk).
    winrt::fire_and_forget TerminalPage::_AdoptExternalSession(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        if (!_sessionRegistry)
        {
            co_return;
        }

        const std::wstring id{ sessionId };

        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        const std::wstring token = lower(std::wstring{ tabToken });

        // Idempotent fast path: a SessionStart for a session already bound to a live tab (and
        // carrying its overlay, when overlays are enabled) needs nothing. This is the common case —
        // every Manager-launched session re-emits SessionStart and lands here, so the reconcile is
        // a cheap no-op for them (no walk, no registry writes, no cross-window notify spam).
        if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
        {
            if (const auto t = it->second.get())
            {
                const bool overlayOk = (_claudeOverlays.find(id) != _claudeOverlays.end()) || !_appSettings.showTabOverlay;
                if (overlayOk)
                {
                    co_return;
                }
            }
        }

        // Diagnostic: we are actually going to try to (re)bind this session to a tab (not a fast
        // no-op). Shows how many terminal tabs this window has, so a mismatch is visible in the log.
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[bind-try] " + id + L" token=" + token + L" tabs=" + std::to_wstring(_tabs.Size()) + L"\n");

        if (token.empty())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" observe-only (no tabToken)\n");
            co_return;
        }

        // Find the live ConPTY in THIS window whose WT_SESSION == token (the STABLE tab identity).
        TerminalApp::Tab hostTab{ nullptr };
        TerminalConnection::ITerminalConnection match{ nullptr };
        for (const auto& projectedTab : _tabs)
        {
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection found{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (found)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                const auto conn = ctrl.Connection();
                if (!conn)
                {
                    return;
                }
                if (lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId())) == token)
                {
                    found = conn;
                }
            });
            if (found)
            {
                hostTab = projectedTab;
                match = found;
                break;
            }
        }

        // No connection in this window hosts that WT_SESSION — a claude hosted outside this app (or
        // in another window). Stay observe-only; do NOT touch the registry (avoids notify spam on
        // every other window for a session it doesn't host).
        if (!match || !hostTab)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" observe-only (no connection for " + token + L")\n");
            co_return;
        }

        _BindClaudeSessionToTab(hostTab, match, id, std::wstring{ cwd }, L"WT_SESSION " + token);
        co_return;
    }

    // Agentmaster: bind a Claude session id to a specific live tab + ConPTY connection — the shared
    // tail of BOTH correlation paths: WT_SESSION-tabToken hook adoption (_AdoptExternalSession) and
    // the Fleet Observer's PEB correlation table (_ObserverProbe). Runs on the UI thread. Handles the in-
    // session /resume RE-HOME (a different id already bound to this tab is archived, its Flight Plan
    // kept restorable), derives/pins the title (one-title rule, Rule #11), binds the stdin injector
    // (Rule #3: inject by sessionId), marks the session live, colors the tab per working dir (Rule
    // #12), attaches the per-tab overlay, and persists. `origin` is a human tag for the log only.
    void TerminalPage::_BindClaudeSessionToTab(const TerminalApp::Tab& hostTab,
                                               const TerminalConnection::ITerminalConnection& conn,
                                               const std::wstring& id,
                                               const std::wstring& cwd,
                                               const std::wstring& origin)
    {
        if (!_sessionRegistry || !hostTab || !conn || id.empty())
        {
            return;
        }

        // RE-HOME (in-session `/resume`): if this SAME tab is currently bound to a DIFFERENT session
        // id, the conversation switched ids on a stable ConPTY. Supersede the old id — archive it
        // (its Flight Plan stays restorable) and drop its tab/overlay binding — then re-point below.
        for (const auto& [oldId, weakOld] : _claudeTabs)
        {
            if (const auto t = weakOld.get(); t && t == hostTab && oldId != id)
            {
                // COPY the key first: `oldId` is a reference INTO the _claudeTabs node, and the
                // erase below frees that node — using `oldId` afterwards (the _claudeOverlays erase)
                // would hash freed memory -> AV. (Latent use-after-free; the observer's more frequent
                // re-homes exposed it.)
                const std::wstring superseded = oldId;
                _sessionRegistry->SetInjector(superseded, nullptr);
                _sessionRegistry->Update(superseded, [](::Agentmaster::SessionInfo& s) {
                    s.live = false;
                    s.pendingConfirmPromptId.clear();
                });
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[rehome] " + superseded + L" -> " + id + L" (same tab, new conversation id)\n");
                _claudeTabs.erase(superseded);
                _claudeOverlays.erase(superseded);
                break; // one tab hosts one session
            }
        }

        // Give the card a readable title from its working dir if it has none.
        const std::wstring ttl = ::Agentmaster::DeriveSessionTitle(cwd);
        _sessionRegistry->Update(id, [&ttl](::Agentmaster::SessionInfo& s) {
            if (s.title.empty())
            {
                s.title = ttl;
            }
        });

        // Bind the id to this tab — full observe+control (Rule #3: inject by sessionId).
        const auto connection = conn;
        _sessionRegistry->SetInjector(id, [connection](const std::wstring& text) {
            const auto* begin = reinterpret_cast<const char16_t*>(text.data());
            connection.WriteInput(winrt::array_view<const char16_t>{ begin, begin + text.size() });
        });
        _claudeTabs[id] = winrt::make_weak(hostTab);
        _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
            s.live = true; // running in a tab we control now (covers /resume to a previously-archived id)
        });

        // One-title rule: a name the user already gave the tab wins (mirror it into the registry);
        // otherwise pin the tab to the managed name. Either way it is now in _claudeTabs, so later
        // renames sync via _SyncClaudeTitleFromTab.
        if (const auto impl = _GetTabImpl(hostTab))
        {
            const std::wstring tabText{ impl->GetTabText() };
            if (!tabText.empty())
            {
                _sessionRegistry->Update(id, [&tabText](::Agentmaster::SessionInfo& s) { s.title = tabText; });
            }
            else if (const auto s = _sessionRegistry->Get(id); s && !s->title.empty())
            {
                impl->SetTabText(winrt::hstring{ s->title });
            }
        }
        _ApplyDirColorToTab(hostTab, cwd); // per-directory tab color
        _AttachClaudeOverlay(hostTab, id); // per-tab "link badge" overlay (TAB_OVERLAY.md)
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" bound via " + origin + L"\n");
    }

    // Agentmaster: the scanner's interval liveness sweep, marshaled onto THIS window's UI thread
    // (the ConnectionState read + XAML tab walk are UI-thread-only, so the plain-C++ scanner can't
    // do it itself — it just TICKS this probe on its slow cadence). Walk this window's claude tabs;
    // any whose hosting ConPTY connection has reached Closed — the claude.exe exited (crashed with
    // no SessionEnd, ran `/exit`, or a clean SessionEnd that only set state=Done) — is archived in
    // place: flip the record to Archived (live=false), unbind its stdin injector, drop the
    // sessionId->tab mapping, and persist. The (now-dead) tab is LEFT for the user to read/close;
    // the session leaves the Triage Board and lists under "Archived", restorable via `claude
    // --resume`. No confirm dialog — the process is already gone (unlike the user-initiated archive
    // seam). Best-effort + idempotent: a tab with no terminal, or any live terminal, is left alone.
    winrt::fire_and_forget TerminalPage::_SweepClaudeLiveness()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        if (!_sessionRegistry || _claudeTabs.empty())
        {
            co_return;
        }

        // Collect first, mutate after — never erase from _claudeTabs while iterating it.
        std::vector<std::wstring> dead;
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            const auto tab = weakTab.get();
            if (!tab)
            {
                continue; // weak_ref already lapsed (tab fully torn down) — the close path owns it
            }
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl)
            {
                continue;
            }
            bool sawTerminal = false;
            bool anyAlive = false;
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                sawTerminal = true;
                // < Closed == NotConnected / Connecting / Connected / Closing -> still alive.
                if (ctrl.ConnectionState() < TerminalConnection::ConnectionState::Closed)
                {
                    anyAlive = true;
                }
            });
            // Only archive a tab we positively saw a (dead) terminal in — never one whose content
            // we couldn't read, and never one with any still-live terminal (e.g. a user split).
            if (sawTerminal && !anyAlive)
            {
                dead.push_back(id);
            }
        }

        if (dead.empty())
        {
            co_return;
        }
        for (const auto& id : dead)
        {
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(id, nullptr);
            _claudeTabs.erase(id);
            _claudeOverlays.erase(id); // drop the per-tab overlay (detaches its registry observer)
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[liveness] dead -> archived " + id + L"\n");
        }
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        co_return;
    }

    // Agentmaster (TAB_OVERLAY.md): the periodic tab<->session BIND reconcile — the POLL backstop to
    // event-driven adoption. Ticked by the shared SessionScanner alongside the liveness sweep. For
    // every LIVE session that has reported a hosting WT_SESSION (tabToken), re-run the idempotent
    // bind (_AdoptExternalSession), UNLESS it is already fully set up in THIS window (bound tab +
    // overlay) — that skip keeps the common case free of work and log noise. This catches: a session
    // whose SessionStart adoption was missed; a missed overlay attach; and a tab whose claude changed
    // its session id via an in-session /resume (the id changed; the tabToken — the ConPTY — did not,
    // and ANY later hook refreshes s.tabToken, so the poll re-homes even with no fresh SessionStart).
    // Only LIVE sessions are reconciled: a re-home archives the superseded id (live=false), so two
    // ids sharing one ConPTY can't ping-pong over the tab.
    winrt::fire_and_forget TerminalPage::_ReconcileClaudeTabs()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry)
        {
            co_return;
        }
        const auto sessions = _sessionRegistry->Snapshot();
        size_t attempts = 0;
        for (const auto& s : sessions)
        {
            if (!s.live || s.tabToken.empty())
            {
                continue; // archived, or no hook has revealed a hosting ConPTY yet
            }
            // Skip sessions already fully set up in THIS window (bound tab + overlay) — no work, no log.
            const auto it = _claudeTabs.find(s.id);
            const bool boundHere = (it != _claudeTabs.end() && it->second.get() != nullptr);
            const bool overlayOk = (_claudeOverlays.find(s.id) != _claudeOverlays.end()) || !_appSettings.showTabOverlay;
            if (boundHere && overlayOk)
            {
                continue;
            }
            ++attempts;
            _AdoptExternalSession(winrt::hstring{ s.id }, winrt::hstring{ s.workingDir }, winrt::hstring{ s.tabToken });
        }
        if (attempts > 0)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[reconcile] sessions=" + std::to_wstring(sessions.size()) +
                                              L" attempts=" + std::to_wstring(attempts) +
                                              L" boundTabs=" + std::to_wstring(_claudeTabs.size()) + L"\n");
        }
        co_return;
    }

    // Agentmaster (Fleet Observer, OBSERVER.md §10): the UI lane of the PULL observer — the one
    // WinRT thread that touches XAML. Replaces _DiscoverClaudeTabsByCwd (the per-tab Toolhelp walk).
    // Two halves, each scanner tick (alongside the reconcile + liveness sweep):
    //   PUBLISH — build THIS window's tab roster {WT_SESSION, shell PID, already-bound} (both reads
    //     are µs: ITerminalConnection::SessionId() + RootProcessHandle()->GetProcessId) and hand it
    //     to the process-wide ProcessObserver, which surveys ALL claude PEBs off-thread in ONE
    //     snapshot and keys correlation on the exact WT_SESSION.
    //   READ + BIND — read the observer's correlation table and, for each of our UNBOUND tabs whose
    //     claude the observer resolved to an OURS conversation id, run the shared
    //     _BindClaudeSessionToTab (injector + overlay + title + color) — full observe+control with
    //     NO hooks / shim / settings, so a hand-typed `claude` after a `cd` still binds to the right
    //     tab + id. A short settle between publish and read lets the publish-triggered survey land,
    //     so a freshly-typed claude binds this tick (≤ ~one cadence) rather than next.
    // External (WindowsTerminal) claudes are observe-only (runningApp != Agentmaster) and never bind.
    winrt::fire_and_forget TerminalPage::_ObserverProbe()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || !_observer)
        {
            co_return;
        }

        // Lowercase a GUID-plain string to match the observer's roster key form (it lowercases too).
        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };

        // --- PASS 1 (UI thread): build this window's roster + remember each tab's conn for the bind. ---
        struct ProbeTab
        {
            winrt::weak_ref<TerminalApp::Tab> tab; // weak so the 300ms settle below never delays a tab teardown
            TerminalConnection::ITerminalConnection conn{ nullptr };
            std::wstring wt;
        };
        std::vector<ProbeTab> probeTabs;
        std::vector<::Agentmaster::TabRosterEntry> roster;
        for (const auto& projectedTab : _tabs)
        {
            if (projectedTab == _managerTab)
            {
                continue;
            }
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection conn{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (conn)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                conn = ctrl.Connection();
            });
            if (!conn)
            {
                continue; // not a terminal tab
            }
            // The exact tab id: WT_SESSION == ITerminalConnection::SessionId() (the correlation key).
            const std::wstring wt = lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId()));
            // The tab's shell PID, from the ConPTY root process HANDLE (the observer reads the claude
            // DESCENDANT's PEB; we hand it the shell so its tree walk is anchored to this exact tab).
            uint32_t shellPid = 0;
            if (const auto cpc = conn.try_as<TerminalConnection::ConptyConnection>())
            {
                if (const auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cpc.RootProcessHandle())))
                {
                    shellPid = ::GetProcessId(h);
                }
            }
            bool bound = false;
            for (const auto& [boundId, weakBound] : _claudeTabs)
            {
                if (const auto t = weakBound.get(); t && t == projectedTab)
                {
                    bound = true;
                    break;
                }
            }
            ::Agentmaster::TabRosterEntry e;
            e.wtSession = wt;
            e.shellPid = shellPid;
            e.bound = bound;
            roster.push_back(std::move(e));
            probeTabs.push_back({ winrt::make_weak(projectedTab), conn, wt });
        }

        // PUBLISH (the observer diffs + Wake()s its survey when this roster changed).
        _observer->PublishRoster(_windowId, std::move(roster));

        // Let a publish-triggered survey land before we read, so a just-appeared claude binds THIS
        // tick rather than next. resume_after resumes on the threadpool — hop back to the UI thread.
        co_await winrt::resume_after(std::chrono::milliseconds(300));
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || !_observer)
        {
            co_return;
        }

        // --- PASS 2 (UI thread): push the External census to the Manager, then bind our tabs. ---
        // External (WindowsTerminal) claudes aren't in any roster, so push them regardless of corr
        // (a window with zero OUR tabs can still surface the external group). The setter diffs.
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SetExternalClaudes(_observer->External());
            }
        }

        const auto corr = _observer->Correlation();
        const auto act = _observer->Activity(); // every tab's foreground activity (pwsh / cmd / claude / codex)
        std::unordered_map<std::wstring, ::Agentmaster::CorrelationRow> byWt;
        for (const auto& c : corr)
        {
            byWt[c.wtSession] = c;
        }
        std::unordered_map<std::wstring, ::Agentmaster::TabActivity> actByWt;
        for (const auto& a : act)
        {
            actByWt[a.wtSession] = a.activity;
        }
        std::unordered_set<std::wstring> rosterWts; // this window's tabs this tick (for observe-badge pruning)
        for (const auto& pt : probeTabs)
        {
            rosterWts.insert(pt.wt);
            const auto hostTab = pt.tab.get();
            if (!hostTab)
            {
                continue; // tab torn down during the settle
            }
            // One session per tab; is this tab already bound (has a real overlay)?
            bool alreadyBound = false;
            for (const auto& [boundId, weakBound] : _claudeTabs)
            {
                if (const auto t = weakBound.get(); t && t == hostTab)
                {
                    alreadyBound = true;
                    break;
                }
            }
            if (alreadyBound)
            {
                _DropPendingOverlay(pt.wt); // bound -> the real overlay owns the slot now
                continue;
            }

            const auto it = byWt.find(pt.wt);
            const bool oursClaude = (it != byWt.end()) && it->second.runningApp == ::Agentmaster::RunningApp::Agentmaster;

            if (oursClaude && !it->second.sessionId.empty())
            {
                // Resolved (the claude has a transcript / explicit id). Cross-window "already claimed"
                // guard, then bind via the shared tail — keyed on the exact WT_SESSION, so a hand-typed
                // claude after a `cd` binds to the right id even with no hook. The bound overlay
                // replaces any pending badge in the slot.
                if (_sessionRegistry->HasInjector(it->second.sessionId))
                {
                    _DropPendingOverlay(pt.wt);
                    continue;
                }
                _DropPendingOverlay(pt.wt);
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[discover] " + it->second.sessionId + L" cwd=" + it->second.cwd + L" (observer wt=" + pt.wt + L")\n");
                _BindClaudeSessionToTab(hostTab, pt.conn, it->second.sessionId, it->second.cwd, L"observer wt=" + pt.wt);
                continue;
            }

            // Non-bound and not a resolved claude -> show an "observe" badge for whatever the observer
            // classified this tab as: an unresolved claude (no transcript id yet, §11d), or a shell /
            // codex from the activity table. So a pwsh / cmd tab carries "○ pwsh · unlinked" too, and
            // it flips to "claude" the instant the user runs claude (then to the linked overlay on the
            // first prompt).
            std::wstring kind;
            if (oursClaude)
            {
                kind = L"claude"; // correlated to a claude, but no conversation id yet
            }
            else if (const auto ait = actByWt.find(pt.wt); ait != actByWt.end())
            {
                switch (ait->second)
                {
                case ::Agentmaster::TabActivity::Powershell:
                    kind = L"pwsh";
                    break;
                case ::Agentmaster::TabActivity::Cmd:
                    kind = L"cmd";
                    break;
                case ::Agentmaster::TabActivity::Codex:
                    kind = L"codex";
                    break;
                case ::Agentmaster::TabActivity::ClaudeCode:
                    kind = L"claude"; // activity caught the claude before correlation did
                    break;
                default:
                    break; // Other / Unknown -> no badge
                }
            }
            if (!kind.empty())
            {
                _SetTabActivityBadge(hostTab, pt.wt, kind);
            }
            else if (!corr.empty() || !act.empty())
            {
                // A fresh survey says this tab is nothing we badge (or its claude exited) -> drop the badge.
                _DropPendingOverlay(pt.wt);
            }
        }
        // Tabs that left THIS window's roster (closed / moved) -> drop their pending badge.
        for (auto pit = _pendingOverlays.begin(); pit != _pendingOverlays.end();)
        {
            if (rosterWts.count(pit->first))
            {
                ++pit;
            }
            else
            {
                if (pit->second)
                {
                    pit->second->Root().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
                }
                pit = _pendingOverlays.erase(pit);
            }
        }
        co_return;
    }

    // Agentmaster: the Explorer Tree "refresh" button's action — force a fresh reload NOW instead of
    // waiting for the next observer/scanner tick. Wake() kicks an immediate FULL survey (re-reads each
    // claude's PEB + transcript -> re-enriches the registry and recomputes the External / Correlation /
    // Activity tables, bypassing the O7 steady-state debounce); _ObserverProbe() re-publishes this
    // window's roster, lets the survey settle, then re-pushes the External census + binds. The survey
    // enriches the registry SILENTLY (no change event, so it never drives churn — Rule #13), so once it
    // lands we force ONE Manager redraw so the refreshed LOCAL/GLOBAL timing + the EXTERNAL census both
    // show even when nothing structurally "changed". Covers every displayed scope.
    winrt::fire_and_forget TerminalPage::_RefreshObserverData()
    {
        auto weakThis = get_weak();
        if (_observer)
        {
            _observer->Wake(); // force an immediate full survey
        }
        _ObserverProbe(); // re-publish roster -> (settle) -> re-push External census + bind

        // Let the survey + probe land (the probe itself settles ~300 ms), then force a redraw so the
        // freshly enriched registry + recomputed census are reflected even when nothing "changed".
        co_await winrt::resume_after(std::chrono::milliseconds(450));
        co_await wil::resume_foreground(Dispatcher());
        if (auto self = weakThis.get())
        {
            if (const auto ipc = self->_agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->RefreshNow();
                }
            }
        }
        co_return;
    }

    // Agentmaster (OBSERVER.md §11d): drop this window's pending "claude · unlinked" badge for a tab
    // (by WT_SESSION) — collapse its element (we hold the overlay, not the pane) and release it. Used
    // when the tab binds a real session, the claude exits, or the tab leaves the roster.
    void TerminalPage::_DropPendingOverlay(const std::wstring& wtSession)
    {
        const auto it = _pendingOverlays.find(wtSession);
        if (it == _pendingOverlays.end())
        {
            return;
        }
        if (it->second)
        {
            it->second->Root().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        }
        _pendingOverlays.erase(it);
    }

    void TerminalPage::_OnFirstLayout(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        // Only let this succeed once.
        _layoutUpdatedRevoker.revoke();

        // This event fires every time the layout changes, but it is always the
        // last one to fire in any layout change chain. That gives us great
        // flexibility in finding the right point at which to initialize our
        // renderer (and our terminal). Any earlier than the last layout update
        // and we may not know the terminal's starting size.
        if (_startupState == StartupState::NotInitialized)
        {
            _startupState = StartupState::InStartup;

            // Agentmaster: stand up the session engine (registry + hooks pipe) first, so the
            // Manager tab — and any session it spawns — has it available.
            _InitAgentmasterEngine();

            // Agentmaster: the Manager tab is always present and leftmost (tab 0),
            // created before startup terminal tabs so they append after it.
            _OpenAgentManagerTab();

            // Agentmaster: re-launch persisted sessions (claude --resume) so the app reopens
            // to the exact state it was closed in (DESIGN §13).
            _RestoreClaudeSessions();

            // Agentmaster (M10 window-grouped restore): if THIS window was reopened from a saved record,
            // re-home its persisted tabs — resume each Claude session + replay each shell tab, in order —
            // so closing and reopening a window brings its whole workspace back, not just geometry + lens.
            // No-op for a fresh window (nothing claimed). Runs after the fleet load so the sessions exist.
            _RestoreWindowTabs();

            if (_startupConnection)
            {
                CreateTabFromConnection(std::move(_startupConnection));
            }
            else if (!_startupActions.empty() && !_windowRecordClaimed)
            {
                // Agentmaster (M10 window-grouped restore): a window restored from a claimed record gets
                // its content from _RestoreWindowTabs above — NOT the default startup action. In our
                // DefaultProfile mode ShouldUsePersistedLayout() is off, so a reopen's `wt -w new -s
                // <idx>` ignores `-s` and falls back to GetStartupActions() = a default `newTab` (pwsh).
                // Processing that here would append a spurious pwsh tab to every reopened window, which
                // then gets captured into the record and ACCUMULATES one shell per reopen cycle. A
                // record with no content tabs faithfully restores Manager-only. (A truly fresh window —
                // no claimed record — still opens its default tab.)
                ProcessStartupActions(std::move(_startupActions));
            }

            _CompleteInitialization();
        }
    }

    // Method Description:
    // - Process all the startup actions in the provided list of startup
    //   actions. We'll do this all at once here.
    // Arguments:
    // - actions: a winrt vector of actions to process. Note that this must NOT
    //   be an IVector&, because we need the collection to be accessible on the
    //   other side of the co_await.
    // - initial: if true, we're parsing these args during startup, and we
    //   should fire an Initialized event.
    // - cwd: If not empty, we should try switching to this provided directory
    //   while processing these actions. This will allow something like `wt -w 0
    //   nt -d .` from inside another directory to work as expected.
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::ProcessStartupActions(std::vector<ActionAndArgs> actions, const winrt::hstring cwd, const winrt::hstring env)
    {
        const auto strong = get_strong();

        // If the caller provided a CWD, "switch" to that directory, then switch
        // back once we're done.
        auto originalVirtualCwd{ _WindowProperties.VirtualWorkingDirectory() };
        auto originalVirtualEnv{ _WindowProperties.VirtualEnvVars() };
        auto restoreCwd = wil::scope_exit([&]() {
            if (!cwd.empty())
            {
                // ignore errors, we'll just power on through. We'd rather do
                // something rather than fail silently if the directory doesn't
                // actually exist.
                _WindowProperties.VirtualWorkingDirectory(originalVirtualCwd);
                _WindowProperties.VirtualEnvVars(originalVirtualEnv);
            }
        });
        if (!cwd.empty())
        {
            _WindowProperties.VirtualWorkingDirectory(cwd);
            _WindowProperties.VirtualEnvVars(env);
        }

        // The current TerminalWindow & TerminalPage architecture is rather instable
        // and fails to start up if the first tab isn't created synchronously.
        //
        // While that's a fair assumption in on itself, simultaneously WinUI will
        // not assign tab contents a size if they're not shown at least once,
        // which we need however in order to initialize ControlCore with a size.
        //
        // So, we do two things here:
        // * DO NOT suspend if this is the first tab.
        // * DO suspend between the creation of panes (or tabs) in order to allow
        //   WinUI to layout the new controls and for ControlCore to get a size.
        //
        // This same logic is also applied to CreateTabFromConnection.
        //
        // See GH#13136.
        auto suspend = _tabs.Size() > 0;

        for (size_t i = 0; i < actions.size(); ++i)
        {
            if (suspend)
            {
                co_await wil::resume_foreground(Dispatcher(), CoreDispatcherPriority::Low);
            }

            _actionDispatch->DoAction(actions[i]);
            suspend = true;
        }

        // GH#6586: now that we're done processing all startup commands,
        // focus the active control. This will work as expected for both
        // commandline invocations and for `wt` action invocations.
        if (const auto& tabImpl{ _GetFocusedTabImpl() })
        {
            if (const auto& content{ tabImpl->GetActiveContent() })
            {
                content.Focus(FocusState::Programmatic);
            }
        }
    }

    safe_void_coroutine TerminalPage::CreateTabFromConnection(ITerminalConnection connection)
    {
        const auto strong = get_strong();

        // This is the exact same logic as in ProcessStartupActions.
        if (_tabs.Size() > 0)
        {
            co_await wil::resume_foreground(Dispatcher(), CoreDispatcherPriority::Low);
        }

        NewTerminalArgs newTerminalArgs;

        if (const auto conpty = connection.try_as<ConptyConnection>())
        {
            newTerminalArgs.Commandline(conpty.Commandline());
            newTerminalArgs.TabTitle(conpty.StartingTitle());
        }

        // GH #12370: We absolutely cannot allow a defterm connection to
        // auto-elevate. Defterm doesn't work for elevated scenarios in the
        // first place. If we try accepting the connection, the spawning an
        // elevated version of the Terminal with that profile... that's a
        // recipe for disaster. We won't ever open up a tab in this window.
        newTerminalArgs.Elevate(false);

        const auto newPane = _MakePane(newTerminalArgs, nullptr, std::move(connection));
        newPane->WalkTree([](const auto& pane) {
            pane->FinalizeConfigurationGivenDefault();
        });
        _CreateNewTabFromPane(newPane);
    }

    // Method Description:
    // - Perform and steps that need to be done once our initial state is all
    //   set up. This includes entering fullscreen mode and firing our
    //   Initialized event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::_CompleteInitialization()
    {
        _startupState = StartupState::Initialized;

        // M10 (PERSISTENCE.md §13): now that startup has settled, write this window's record once
        // so its existence + geometry + lens are persisted from launch (not only after the first
        // interaction). _ScheduleWindowRecordSave gates on Initialized, just set above.
        _ScheduleWindowRecordSave();

        // GH#632 - It's possible that the user tried to create the terminal
        // with only one tab, with only an elevated profile. If that happens,
        // we'll create _another_ process to host the elevated version of that
        // profile. This can happen from the jumplist, or if the default profile
        // is `elevate:true`, or from the commandline.
        //
        // However, we need to make sure to close this window in that scenario.
        // Since there aren't any _tabs_ in this window, we won't ever get a
        // closed event. So do it manually.
        //
        // GH#12267: Make sure that we don't instantly close ourselves when
        // we're readying to accept a defterm connection. In that case, we don't
        // have a tab yet, but will once we're initialized.
        if (_tabs.Size() == 0)
        {
            CloseWindowRequested.raise(*this, nullptr);
            co_return;
        }
        else
        {
            // GH#11561: When we start up, our window is initially just a frame
            // with a transparent content area. We're gonna do all this startup
            // init on the UI thread, so the UI won't actually paint till it's
            // all done. This results in a few frames where the frame is
            // visible, before the page paints for the first time, before any
            // tabs appears, etc.
            //
            // To mitigate this, we're gonna wait for the UI thread to finish
            // everything it's gotta do for the initial init, and _then_ fire
            // our Initialized event. By waiting for everything else to finish
            // (CoreDispatcherPriority::Low), we let all the tabs and panes
            // actually get created. In the window layer, we're gonna cloak the
            // window till this event is fired, so we don't actually see this
            // frame until we're actually all ready to go.
            //
            // This will result in the window seemingly not loading as fast, but
            // it will actually take exactly the same amount of time before it's
            // usable.
            //
            // We also experimented with drawing a solid BG color before the
            // initialization is finished. However, there are still a few frames
            // after the frame is displayed before the XAML content first draws,
            // so that didn't actually resolve any issues.
            Dispatcher().RunAsync(CoreDispatcherPriority::Low, [weak = get_weak()]() {
                if (auto self{ weak.get() })
                {
                    self->Initialized.raise(*self, nullptr);
                }
            });
        }
    }

    // Method Description:
    // - Show a dialog with "About" information. Displays the app's Display
    //   Name, version, getting started link, source code link, documentation link, release
    //   Notes link, send feedback link and privacy policy link.
    void TerminalPage::_ShowAboutDialog()
    {
        _ShowDialogHelper(L"AboutDialog");
    }

    winrt::hstring TerminalPage::ApplicationDisplayName()
    {
        return CascadiaSettings::ApplicationDisplayName();
    }

    winrt::hstring TerminalPage::ApplicationVersion()
    {
        return CascadiaSettings::ApplicationVersion();
    }

    // Method Description:
    // - Helper to show a content dialog
    // - We only open a content dialog if there isn't one open already
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowDialogHelper(const std::wstring_view& name)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(name).try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays the unified close confirmation dialog configured for the
    //   given scenario. Resets the "don't ask me again" checkbox before showing.
    //   If the user confirms and checked "don't ask me again", sets
    //   confirmOnClose to Never and writes settings to disk.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowConfirmCloseDialog(ConfirmCloseDialogKind kind)
    {
        // Load the dialog (triggers x:Load) and configure its strings.
        const auto dialog = FindName(L"ConfirmCloseDialog").as<ContentDialog>();

        winrt::hstring title;
        winrt::hstring primary;
        switch (kind)
        {
        case ConfirmCloseDialogKind::CloseAll:
            title = RS_(L"ConfirmCloseDialog_CloseAllTitle");
            primary = RS_(L"ConfirmCloseDialog_CloseAllPrimary");
            break;
        case ConfirmCloseDialogKind::Window:
            title = RS_(L"ConfirmCloseDialog_WindowTitle");
            primary = RS_(L"ConfirmCloseDialog_WindowPrimary");
            break;
        case ConfirmCloseDialogKind::Tab:
            title = RS_(L"ConfirmCloseDialog_TabTitle");
            primary = RS_(L"ConfirmCloseDialog_TabPrimary");
            break;
        case ConfirmCloseDialogKind::MultiplePanes:
            title = RS_(L"ConfirmCloseDialog_MultiplePanesTitle");
            primary = RS_(L"ConfirmCloseDialog_MultiplePanesPrimary");
            break;
        case ConfirmCloseDialogKind::MultipleTabs:
            title = RS_(L"ConfirmCloseDialog_MultipleTabsTitle");
            primary = RS_(L"ConfirmCloseDialog_MultipleTabsPrimary");
            break;
        case ConfirmCloseDialogKind::Pane:
            title = RS_(L"ConfirmCloseDialog_PaneTitle");
            primary = RS_(L"ConfirmCloseDialog_PanePrimary");
            break;
        }
        dialog.Title(winrt::box_value(title));
        dialog.PrimaryButtonText(primary);
        dialog.CloseButtonText(RS_(L"ConfirmCloseDialog_Cancel"));

        // BODGY: After a ContentDialog is dismissed, FindName() can no longer
        // resolve children inside it. Use Content() to get the checkbox directly.
        const auto checkbox = dialog.Content().as<CheckBox>();
        checkbox.IsChecked(false);

        auto result = ContentDialogResult::None;
        if (auto presenter{ _dialogPresenter.get() })
        {
            const auto weak = get_weak();
            result = co_await presenter.ShowDialog(dialog);

            // ShowDialog blocks until the dialog is dismissed, so it is
            // possible for `this` to be torn down while we wait. Re-acquire
            // a strong reference before touching any of our state.
            const auto strong = weak.get();
            if (!strong)
            {
                co_return ContentDialogResult::None;
            }

            if (result == ContentDialogResult::Primary && checkbox.IsChecked().Value())
            {
                _settings.GlobalSettings().ConfirmOnClose(ConfirmOnClose::Never);
                _settings.WriteSettingsToDisk();
            }
        }

        co_return result;
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal tab marked as read-only
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseReadOnlyDialog()
    {
        return _ShowDialogHelper(L"CloseReadOnlyDialog");
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste contains the "new line" character which can
    //   have the effect of starting commands without the user's knowledge if
    //   it is pasted on a shell where the "new line" character marks the end
    //   of a command.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowMultiLinePasteWarningDialog()
    {
        return _ShowDialogHelper(L"MultiLinePasteDialog");
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste is very long, in case they did not mean to
    //   paste it but pressed the paste shortcut by accident.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowLargePasteWarningDialog()
    {
        return _ShowDialogHelper(L"LargePasteDialog");
    }

    // Method Description:
    // - Builds the flyout (dropdown) attached to the new tab button, and
    //   attaches it to the button. Populates the flyout with one entry per
    //   Profile, displaying the profile's name. Clicking each flyout item will
    //   open a new tab with that profile.
    //   Below the profiles are the static menu items: settings, command palette
    void TerminalPage::_CreateNewTabFlyout()
    {
        auto newTabFlyout = WUX::Controls::MenuFlyout{};
        newTabFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);

        // Create profile entries from the NewTabMenu configuration using a
        // recursive helper function. This returns a std::vector of FlyoutItemBases,
        // that we then add to our Flyout.
        auto entries = _settings.GlobalSettings().NewTabMenu();
        auto items = _CreateNewTabFlyoutItems(entries);
        for (const auto& item : items)
        {
            newTabFlyout.Items().Append(item);
        }

        // add menu separator
        auto separatorItem = WUX::Controls::MenuFlyoutSeparator{};
        newTabFlyout.Items().Append(separatorItem);

        // add static items
        {
            // Create the settings button.
            auto settingsItem = WUX::Controls::MenuFlyoutItem{};
            settingsItem.Text(RS_(L"SettingsMenuItem"));
            const auto settingsToolTip = RS_(L"SettingsToolTip");

            WUX::Controls::ToolTipService::SetToolTip(settingsItem, box_value(settingsToolTip));
            Automation::AutomationProperties::SetHelpText(settingsItem, settingsToolTip);

            WUX::Controls::SymbolIcon ico{};
            ico.Symbol(WUX::Controls::Symbol::Setting);
            settingsItem.Icon(ico);

            settingsItem.Click({ this, &TerminalPage::_SettingsButtonOnClick });
            newTabFlyout.Items().Append(settingsItem);

            auto actionMap = _settings.ActionMap();
            const auto settingsKeyChord{ actionMap.GetKeyBindingForAction(L"Terminal.OpenSettingsUI") };
            if (settingsKeyChord)
            {
                _SetAcceleratorForMenuItem(settingsItem, settingsKeyChord);
            }

            // Create the command palette button.
            auto commandPaletteFlyout = WUX::Controls::MenuFlyoutItem{};
            commandPaletteFlyout.Text(RS_(L"CommandPaletteMenuItem"));
            const auto commandPaletteToolTip = RS_(L"CommandPaletteToolTip");

            WUX::Controls::ToolTipService::SetToolTip(commandPaletteFlyout, box_value(commandPaletteToolTip));
            Automation::AutomationProperties::SetHelpText(commandPaletteFlyout, commandPaletteToolTip);

            WUX::Controls::FontIcon commandPaletteIcon{};
            commandPaletteIcon.Glyph(L"\xE945");
            commandPaletteIcon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            commandPaletteFlyout.Icon(commandPaletteIcon);

            commandPaletteFlyout.Click({ this, &TerminalPage::_CommandPaletteButtonOnClick });
            newTabFlyout.Items().Append(commandPaletteFlyout);

            const auto commandPaletteKeyChord{ actionMap.GetKeyBindingForAction(L"Terminal.ToggleCommandPalette") };
            if (commandPaletteKeyChord)
            {
                _SetAcceleratorForMenuItem(commandPaletteFlyout, commandPaletteKeyChord);
            }

            // Create the about button.
            auto aboutFlyout = WUX::Controls::MenuFlyoutItem{};
            aboutFlyout.Text(RS_(L"AboutMenuItem"));
            const auto aboutToolTip = RS_(L"AboutToolTip");

            WUX::Controls::ToolTipService::SetToolTip(aboutFlyout, box_value(aboutToolTip));
            Automation::AutomationProperties::SetHelpText(aboutFlyout, aboutToolTip);

            WUX::Controls::SymbolIcon aboutIcon{};
            aboutIcon.Symbol(WUX::Controls::Symbol::Help);
            aboutFlyout.Icon(aboutIcon);

            aboutFlyout.Click({ this, &TerminalPage::_AboutButtonOnClick });
            newTabFlyout.Items().Append(aboutFlyout);
        }

        // Before opening the fly-out set focus on the current tab
        // so no matter how fly-out is closed later on the focus will return to some tab.
        // We cannot do it on closing because if the window loses focus (alt+tab)
        // the closing event is not fired.
        // It is important to set the focus on the tab
        // Since the previous focus location might be discarded in the background,
        // e.g., the command palette will be dismissed by the menu,
        // and then closing the fly-out will move the focus to wrong location.
        newTabFlyout.Opening([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                page->_FocusCurrentTab(true);

                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuOpened",
                    TraceLoggingDescription("Event emitted when the new tab menu is opened"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The Count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
            }
        });
        // Necessary for fly-out sub items to get focus on a tab before collapsing. Related to #15049
        newTabFlyout.Closing([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                if (!page->_commandPaletteIs(Visibility::Visible))
                {
                    page->_FocusCurrentTab(true);
                }

                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuClosed",
                    TraceLoggingDescription("Event emitted when the new tab menu is closed"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The Count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
            }
        });
        _newTabButton.Flyout(newTabFlyout);
    }

    // Method Description:
    // - For a given list of tab menu entries, this method will create the corresponding
    //   list of flyout items. This is a recursive method that calls itself when it comes
    //   across a folder entry.
    std::vector<WUX::Controls::MenuFlyoutItemBase> TerminalPage::_CreateNewTabFlyoutItems(IVector<NewTabMenuEntry> entries)
    {
        std::vector<WUX::Controls::MenuFlyoutItemBase> items;

        if (entries == nullptr || entries.Size() == 0)
        {
            return items;
        }

        for (const auto& entry : entries)
        {
            if (entry == nullptr)
            {
                continue;
            }

            switch (entry.Type())
            {
            case NewTabMenuEntryType::Separator:
            {
                items.push_back(WUX::Controls::MenuFlyoutSeparator{});
                break;
            }
            // A folder has a custom name and icon, and has a number of entries that require
            // us to call this method recursively.
            case NewTabMenuEntryType::Folder:
            {
                const auto folderEntry = entry.as<FolderEntry>();
                const auto folderEntries = folderEntry.Entries();

                // If the folder is empty, we should skip the entry if AllowEmpty is false, or
                // when the folder should inline.
                // The IsEmpty check includes semantics for nested (empty) folders
                if (folderEntries.Size() == 0 && (!folderEntry.AllowEmpty() || folderEntry.Inlining() == FolderEntryInlining::Auto))
                {
                    break;
                }

                // Recursively generate flyout items
                auto folderEntryItems = _CreateNewTabFlyoutItems(folderEntries);

                // If the folder should auto-inline and there is only one item, do so.
                if (folderEntry.Inlining() == FolderEntryInlining::Auto && folderEntryItems.size() == 1)
                {
                    for (auto const& folderEntryItem : folderEntryItems)
                    {
                        items.push_back(folderEntryItem);
                    }

                    break;
                }

                // Otherwise, create a flyout
                auto folderItem = WUX::Controls::MenuFlyoutSubItem{};
                folderItem.Text(folderEntry.Name());

                auto icon = _CreateNewTabFlyoutIcon(folderEntry.Icon().Resolved());
                folderItem.Icon(icon);

                for (const auto& folderEntryItem : folderEntryItems)
                {
                    folderItem.Items().Append(folderEntryItem);
                }

                // If the folder is empty, and by now we know we set AllowEmpty to true,
                // create a placeholder item here
                if (folderEntries.Size() == 0)
                {
                    auto placeholder = WUX::Controls::MenuFlyoutItem{};
                    placeholder.Text(RS_(L"NewTabMenuFolderEmpty"));
                    placeholder.IsEnabled(false);

                    folderItem.Items().Append(placeholder);
                }

                items.push_back(folderItem);
                break;
            }
            // Any "collection entry" will simply make us add each profile in the collection
            // separately. This collection is stored as a map <int, Profile>, so the correct
            // profile index is already known.
            case NewTabMenuEntryType::RemainingProfiles:
            case NewTabMenuEntryType::MatchProfiles:
            {
                const auto remainingProfilesEntry = entry.as<ProfileCollectionEntry>();
                if (remainingProfilesEntry.Profiles() == nullptr)
                {
                    break;
                }

                for (auto&& [profileIndex, remainingProfile] : remainingProfilesEntry.Profiles())
                {
                    items.push_back(_CreateNewTabFlyoutProfile(remainingProfile, profileIndex, {}));
                }

                break;
            }
            // A single profile, the profile index is also given in the entry
            case NewTabMenuEntryType::Profile:
            {
                const auto profileEntry = entry.as<ProfileEntry>();
                if (profileEntry.Profile() == nullptr)
                {
                    break;
                }

                auto profileItem = _CreateNewTabFlyoutProfile(profileEntry.Profile(), profileEntry.ProfileIndex(), profileEntry.Icon().Resolved());
                items.push_back(profileItem);
                break;
            }
            case NewTabMenuEntryType::Action:
            {
                const auto actionEntry = entry.as<ActionEntry>();
                const auto actionId = actionEntry.ActionId();
                if (_settings.ActionMap().GetActionByID(actionId))
                {
                    auto actionItem = _CreateNewTabFlyoutAction(actionId, actionEntry.Icon().Resolved());
                    items.push_back(actionItem);
                }

                break;
            }
            }
        }

        return items;
    }

    // Method Description:
    // - This method creates a flyout menu item for a given profile with the given index.
    //   It makes sure to set the correct icon, keybinding, and click-action.
    WUX::Controls::MenuFlyoutItem TerminalPage::_CreateNewTabFlyoutProfile(const Profile profile, int profileIndex, const winrt::hstring& iconPathOverride)
    {
        auto profileMenuItem = WUX::Controls::MenuFlyoutItem{};

        // Add the keyboard shortcuts based on the number of profiles defined
        // Look for a keychord that is bound to the equivalent
        // NewTab(ProfileIndex=N) action
        NewTerminalArgs newTerminalArgs{ profileIndex };
        NewTabArgs newTabArgs{ newTerminalArgs };
        const auto id = fmt::format(FMT_COMPILE(L"Terminal.OpenNewTabProfile{}"), profileIndex);
        const auto profileKeyChord{ _settings.ActionMap().GetKeyBindingForAction(id) };

        // make sure we find one to display
        if (profileKeyChord)
        {
            _SetAcceleratorForMenuItem(profileMenuItem, profileKeyChord);
        }

        auto profileName = profile.Name();
        profileMenuItem.Text(profileName);

        // If a custom icon path has been specified, set it as the icon for
        // this flyout item. Otherwise, if an icon is set for this profile, set that icon
        // for this flyout item.
        const auto& iconPath = iconPathOverride.empty() ? profile.Icon().Resolved() : iconPathOverride;
        if (!iconPath.empty())
        {
            const auto icon = _CreateNewTabFlyoutIcon(iconPath);
            profileMenuItem.Icon(icon);
        }

        if (profile.Guid() == _settings.GlobalSettings().DefaultProfile())
        {
            // Contrast the default profile with others in font weight.
            profileMenuItem.FontWeight(FontWeights::Bold());
        }

        auto newTabRun = WUX::Documents::Run();
        newTabRun.Text(RS_(L"NewTabRun/Text"));
        auto newPaneRun = WUX::Documents::Run();
        newPaneRun.Text(RS_(L"NewPaneRun/Text"));
        newPaneRun.FontStyle(FontStyle::Italic);
        auto newWindowRun = WUX::Documents::Run();
        newWindowRun.Text(RS_(L"NewWindowRun/Text"));
        newWindowRun.FontStyle(FontStyle::Italic);
        auto elevatedRun = WUX::Documents::Run();
        elevatedRun.Text(RS_(L"ElevatedRun/Text"));
        elevatedRun.FontStyle(FontStyle::Italic);

        auto textBlock = WUX::Controls::TextBlock{};
        textBlock.Inlines().Append(newTabRun);
        textBlock.Inlines().Append(WUX::Documents::LineBreak{});
        textBlock.Inlines().Append(newPaneRun);
        textBlock.Inlines().Append(WUX::Documents::LineBreak{});
        textBlock.Inlines().Append(newWindowRun);
        textBlock.Inlines().Append(WUX::Documents::LineBreak{});
        textBlock.Inlines().Append(elevatedRun);

        auto toolTip = WUX::Controls::ToolTip{};
        toolTip.Content(textBlock);
        WUX::Controls::ToolTipService::SetToolTip(profileMenuItem, toolTip);

        profileMenuItem.Click([profileIndex, weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuItemClicked",
                    TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingValue("Profile", "ItemType", "The type of item that was clicked in the new tab menu"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                NewTerminalArgs newTerminalArgs{ profileIndex };
                page->_OpenNewTerminalViaDropdown(newTerminalArgs);
            }
        });

        // Using the static method on the base class seems to do what we want in terms of placement.
        WUX::Controls::Primitives::FlyoutBase::SetAttachedFlyout(profileMenuItem, _CreateRunAsAdminFlyout(profileIndex));

        // Since we are not setting the ContextFlyout property of the item we have to handle the ContextRequested event
        // and rely on the base class to show our menu.
        profileMenuItem.ContextRequested([profileMenuItem](auto&&, auto&&) {
            WUX::Controls::Primitives::FlyoutBase::ShowAttachedFlyout(profileMenuItem);
        });

        return profileMenuItem;
    }

    // Method Description:
    // - This method creates a flyout menu item for a given action
    //   It makes sure to set the correct icon, keybinding, and click-action.
    WUX::Controls::MenuFlyoutItem TerminalPage::_CreateNewTabFlyoutAction(const winrt::hstring& actionId, const winrt::hstring& iconPathOverride)
    {
        auto actionMenuItem = WUX::Controls::MenuFlyoutItem{};
        const auto action{ _settings.ActionMap().GetActionByID(actionId) };
        const auto actionKeyChord{ _settings.ActionMap().GetKeyBindingForAction(actionId) };

        if (actionKeyChord)
        {
            _SetAcceleratorForMenuItem(actionMenuItem, actionKeyChord);
        }

        actionMenuItem.Text(action.Name());

        // If a custom icon path has been specified, set it as the icon for
        // this flyout item. Otherwise, if an icon is set for this action, set that icon
        // for this flyout item.
        const auto& iconPath = iconPathOverride.empty() ? action.Icon().Resolved() : iconPathOverride;
        if (!iconPath.empty())
        {
            const auto icon = _CreateNewTabFlyoutIcon(iconPath);
            actionMenuItem.Icon(icon);
        }

        actionMenuItem.Click([action, weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuItemClicked",
                    TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingValue("Action", "ItemType", "The type of item that was clicked in the new tab menu"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                page->_actionDispatch->DoAction(action.ActionAndArgs());
            }
        });

        return actionMenuItem;
    }

    // Method Description:
    // - Helper method to create an IconElement that can be passed to MenuFlyoutItems and
    //   MenuFlyoutSubItems
    IconElement TerminalPage::_CreateNewTabFlyoutIcon(const winrt::hstring& iconSource)
    {
        if (iconSource.empty())
        {
            return nullptr;
        }

        auto icon = UI::IconPathConverter::IconWUX(iconSource);
        Automation::AutomationProperties::SetAccessibilityView(icon, Automation::Peers::AccessibilityView::Raw);

        return icon;
    }

    // Function Description:
    // Called when the openNewTabDropdown keybinding is used.
    // Shows the dropdown flyout.
    void TerminalPage::_OpenNewTabDropdown()
    {
        _newTabButton.Flyout().ShowAt(_newTabButton);
    }

    void TerminalPage::_OpenNewTerminalViaDropdown(const NewTerminalArgs newTerminalArgs)
    {
        // if alt is pressed, open a pane
        const auto window = CoreWindow::GetForCurrentThread();
        const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
        const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
        const auto altPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto rShiftState = window.GetKeyState(VirtualKey::RightShift);
        const auto lShiftState = window.GetKeyState(VirtualKey::LeftShift);
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        const auto ctrlState{ window.GetKeyState(VirtualKey::Control) };
        const auto rCtrlState = window.GetKeyState(VirtualKey::RightControl);
        const auto lCtrlState = window.GetKeyState(VirtualKey::LeftControl);
        const auto ctrlPressed{ WI_IsFlagSet(ctrlState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(rCtrlState, CoreVirtualKeyStates::Down) ||
                                WI_IsFlagSet(lCtrlState, CoreVirtualKeyStates::Down) };

        // Check for DebugTap
        auto debugTap = this->_settings.GlobalSettings().DebugFeaturesEnabled() &&
                        WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                        WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

        const auto dispatchToElevatedWindow = ctrlPressed && !IsRunningElevated();

        auto sessionType = "";
        if ((shiftPressed || dispatchToElevatedWindow) && !debugTap)
        {
            // Manually fill in the evaluated profile.
            if (newTerminalArgs.ProfileIndex() != nullptr)
            {
                // We want to promote the index to a GUID because there is no "launch to profile index" command.
                const auto profile = _settings.GetProfileForArgs(newTerminalArgs);
                if (profile)
                {
                    newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(profile.Guid()));
                    newTerminalArgs.StartingDirectory(_evaluatePathForCwd(profile.EvaluatedStartingDirectory()));
                }
            }

            if (dispatchToElevatedWindow)
            {
                _OpenElevatedWT(newTerminalArgs);
                sessionType = "ElevatedWindow";
            }
            else
            {
                _OpenNewWindow(newTerminalArgs);
                sessionType = "Window";
            }
        }
        else
        {
            const auto newPane = _MakePane(newTerminalArgs);
            // If the newTerminalArgs caused us to open an elevated window
            // instead of creating a pane, it may have returned nullptr. Just do
            // nothing then.
            if (!newPane)
            {
                return;
            }
            if (altPressed && !debugTap)
            {
                this->_SplitPane(_GetFocusedTabImpl(),
                                 SplitDirection::Automatic,
                                 0.5f,
                                 newPane);
                sessionType = "Pane";
            }
            else
            {
                _CreateNewTabFromPane(newPane);
                sessionType = "Tab";
            }
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuCreatedNewTerminalSession",
            TraceLoggingDescription("Event emitted when a new terminal was created via the new tab menu"),
            TraceLoggingValue(NumberOfTabs(), "NewTabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue(sessionType, "SessionType", "The type of session that was created"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    std::wstring TerminalPage::_evaluatePathForCwd(const std::wstring_view path)
    {
        return Utils::EvaluateStartingDirectory(_WindowProperties.VirtualWorkingDirectory(), path);
    }

    // Method Description:
    // - Creates a new connection based on the profile settings
    // Arguments:
    // - the profile we want the settings from
    // - the terminal settings
    // Return value:
    // - the desired connection
    TerminalConnection::ITerminalConnection TerminalPage::_CreateConnectionFromSettings(Profile profile,
                                                                                        IControlSettings settings,
                                                                                        const bool inheritCursor)
    {
        static const auto textMeasurement = [&]() -> std::wstring_view {
            switch (_settings.GlobalSettings().TextMeasurement())
            {
            case TextMeasurement::Graphemes:
                return L"graphemes";
            case TextMeasurement::Wcswidth:
                return L"wcswidth";
            case TextMeasurement::Console:
                return L"console";
            default:
                return {};
            }
        }();
        static const auto ambiguousIsWide = [&]() -> bool {
            return _settings.GlobalSettings().AmbiguousWidth() == AmbiguousWidth::Wide;
        }();

        TerminalConnection::ITerminalConnection connection{ nullptr };

        auto connectionType = profile.ConnectionType();
        Windows::Foundation::Collections::ValueSet valueSet;

        if (connectionType == TerminalConnection::AzureConnection::ConnectionType() &&
            TerminalConnection::AzureConnection::IsAzureConnectionAvailable())
        {
            connection = TerminalConnection::AzureConnection{};
            valueSet = TerminalConnection::ConptyConnection::CreateSettings(winrt::hstring{},
                                                                            L".",
                                                                            L"Azure",
                                                                            false,
                                                                            L"",
                                                                            nullptr,
                                                                            settings.InitialRows(),
                                                                            settings.InitialCols(),
                                                                            winrt::guid(),
                                                                            profile.Guid());
        }

        else
        {
            auto settingsInternal{ winrt::get_self<Settings::TerminalSettings>(settings) };
            const auto environment = settingsInternal->EnvironmentVariables();

            // Update the path to be relative to whatever our CWD is.
            //
            // Refer to the examples in
            // https://en.cppreference.com/w/cpp/filesystem/path/append
            //
            // We need to do this here, to ensure we tell the ConptyConnection
            // the correct starting path. If we're being invoked from another
            // terminal instance (e.g. `wt -w 0 -d .`), then we have switched our
            // CWD to the provided path. We should treat the StartingDirectory
            // as relative to the current CWD.
            //
            // The connection must be informed of the current CWD on
            // construction, because the connection might not spawn the child
            // process until later, on another thread, after we've already
            // restored the CWD to its original value.
            auto newWorkingDirectory{ _evaluatePathForCwd(settings.StartingDirectory()) };
            connection = TerminalConnection::ConptyConnection{};
            valueSet = TerminalConnection::ConptyConnection::CreateSettings(settings.Commandline(),
                                                                            newWorkingDirectory,
                                                                            settings.StartingTitle(),
                                                                            settingsInternal->ReloadEnvironmentVariables(),
                                                                            _WindowProperties.VirtualEnvVars(),
                                                                            environment,
                                                                            settings.InitialRows(),
                                                                            settings.InitialCols(),
                                                                            winrt::guid(),
                                                                            profile.Guid());

            if (inheritCursor)
            {
                valueSet.Insert(L"inheritCursor", Windows::Foundation::PropertyValue::CreateBoolean(true));
            }
        }

        if (!textMeasurement.empty())
        {
            valueSet.Insert(L"textMeasurement", Windows::Foundation::PropertyValue::CreateString(textMeasurement));
        }
        if (ambiguousIsWide)
        {
            valueSet.Insert(L"ambiguousIsWide", Windows::Foundation::PropertyValue::CreateBoolean(true));
        }

        if (const auto id = settings.SessionId(); id != winrt::guid{})
        {
            valueSet.Insert(L"sessionId", Windows::Foundation::PropertyValue::CreateGuid(id));
        }

        connection.Initialize(valueSet);

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "ConnectionCreated",
            TraceLoggingDescription("Event emitted upon the creation of a connection"),
            TraceLoggingGuid(connectionType, "ConnectionTypeGuid", "The type of the connection"),
            TraceLoggingGuid(profile.Guid(), "ProfileGuid", "The profile's GUID"),
            TraceLoggingGuid(connection.SessionId(), "SessionGuid", "The WT_SESSION's GUID"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        return connection;
    }

    TerminalConnection::ITerminalConnection TerminalPage::_duplicateConnectionForRestart(const TerminalApp::TerminalPaneContent& paneContent)
    {
        if (paneContent == nullptr)
        {
            return nullptr;
        }

        const auto& control{ paneContent.GetTermControl() };
        if (control == nullptr)
        {
            return nullptr;
        }
        const auto& connection = control.Connection();
        auto profile{ paneContent.GetProfile() };

        Settings::TerminalSettingsCreateResult controlSettings{ nullptr };

        if (profile)
        {
            // TODO GH#5047 If we cache the NewTerminalArgs, we no longer need to do this.
            profile = GetClosestProfileForDuplicationOfProfile(profile);
            controlSettings = Settings::TerminalSettings::CreateWithProfile(_settings, profile);

            // Replace the Starting directory with the CWD, if given
            const auto workingDirectory = control.WorkingDirectory();
            if (Utils::IsValidDirectory(workingDirectory.c_str()))
            {
                controlSettings.DefaultSettings()->StartingDirectory(workingDirectory);
            }

            // To facilitate restarting defterm connections: grab the original
            // commandline out of the connection and shove that back into the
            // settings.
            if (const auto& conpty{ connection.try_as<TerminalConnection::ConptyConnection>() })
            {
                controlSettings.DefaultSettings()->Commandline(conpty.Commandline());
            }
        }

        return _CreateConnectionFromSettings(profile, *controlSettings.DefaultSettings(), true);
    }

    // Method Description:
    // - Called when the settings button is clicked. Launches a background
    //   thread to open the settings file in the default JSON editor.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_SettingsButtonOnClick(const IInspectable&,
                                              const RoutedEventArgs&)
    {
        const auto window = CoreWindow::GetForCurrentThread();

        // check alt state
        const auto rAltState{ window.GetKeyState(VirtualKey::RightMenu) };
        const auto lAltState{ window.GetKeyState(VirtualKey::LeftMenu) };
        const auto altPressed{ WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                               WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down) };

        // check shift state
        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto lShiftState{ window.GetKeyState(VirtualKey::LeftShift) };
        const auto rShiftState{ window.GetKeyState(VirtualKey::RightShift) };
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        auto target{ SettingsTarget::SettingsUI };
        if (shiftPressed)
        {
            target = SettingsTarget::SettingsFile;
        }
        else if (altPressed)
        {
            target = SettingsTarget::DefaultsFile;
        }

        const auto targetAsString = [&target]() {
            switch (target)
            {
            case SettingsTarget::SettingsFile:
                return "SettingsFile";
            case SettingsTarget::DefaultsFile:
                return "DefaultsFile";
            case SettingsTarget::SettingsUI:
            default:
                return "UI";
            }
        }();

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuItemClicked",
            TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
            TraceLoggingValue(NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue("Settings", "ItemType", "The type of item that was clicked in the new tab menu"),
            TraceLoggingValue(targetAsString, "SettingsTarget", "The target settings file or UI"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        _LaunchSettings(target);
    }

    // Method Description:
    // - Called when the command palette button is clicked. Opens the command palette.
    void TerminalPage::_CommandPaletteButtonOnClick(const IInspectable&,
                                                    const RoutedEventArgs&)
    {
        auto p = LoadCommandPalette();
        p.EnableCommandPaletteMode(CommandPaletteLaunchMode::Action);
        p.Visibility(Visibility::Visible);

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuItemClicked",
            TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
            TraceLoggingValue(NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue("CommandPalette", "ItemType", "The type of item that was clicked in the new tab menu"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    // Method Description:
    // - Called when the about button is clicked. See _ShowAboutDialog for more info.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_AboutButtonOnClick(const IInspectable&,
                                           const RoutedEventArgs&)
    {
        _ShowAboutDialog();

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "NewTabMenuItemClicked",
            TraceLoggingDescription("Event emitted when an item from the new tab menu is invoked"),
            TraceLoggingValue(NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
            TraceLoggingValue("About", "ItemType", "The type of item that was clicked in the new tab menu"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    // Method Description:
    // - Called when the users pressed keyBindings while CommandPaletteElement is open.
    // - As of GH#8480, this is also bound to the TabRowControl's KeyUp event.
    //   That should only fire when focus is in the tab row, which is hard to
    //   do. Notably, that's possible:
    //   - When you have enough tabs to make the little scroll arrows appear,
    //     click one, then hit tab
    //   - When Narrator is in Scan mode (which is the a11y bug we're fixing here)
    // - This method is effectively an extract of TermControl::_KeyHandler and TermControl::_TryHandleKeyBinding.
    // Arguments:
    // - e: the KeyRoutedEventArgs containing info about the keystroke.
    // Return Value:
    // - <none>
    void TerminalPage::_KeyDownHandler(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        const auto keyStatus = e.KeyStatus();
        const auto vkey = gsl::narrow_cast<WORD>(e.OriginalKey());
        const auto scanCode = gsl::narrow_cast<WORD>(keyStatus.ScanCode);
        const auto modifiers = _GetPressedModifierKeys();

        // GH#11076:
        // For some weird reason we sometimes receive a WM_KEYDOWN
        // message without vkey or scanCode if a user drags a tab.
        // The KeyChord constructor has a debug assertion ensuring that all KeyChord
        // either have a valid vkey/scanCode. This is important, because this prevents
        // accidental insertion of invalid KeyChords into classes like ActionMap.
        if (!vkey && !scanCode)
        {
            return;
        }

        // Alt-Numpad# input will send us a character once the user releases
        // Alt, so we should be ignoring the individual keydowns. The character
        // will be sent through the TSFInputControl. See GH#1401 for more
        // details
        if (modifiers.IsAltPressed() && (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9))
        {
            return;
        }

        // GH#2235: Terminal::Settings hasn't been modified to differentiate
        // between AltGr and Ctrl+Alt yet.
        // -> Don't check for key bindings if this is an AltGr key combination.
        if (modifiers.IsAltGrPressed())
        {
            return;
        }

        const auto actionMap = _settings.ActionMap();
        if (!actionMap)
        {
            return;
        }

        const auto cmd = actionMap.GetActionByKeyChord({
            modifiers.IsCtrlPressed(),
            modifiers.IsAltPressed(),
            modifiers.IsShiftPressed(),
            modifiers.IsWinPressed(),
            vkey,
            scanCode,
        });
        if (!cmd)
        {
            return;
        }

        if (!_actionDispatch->DoAction(cmd.ActionAndArgs()))
        {
            return;
        }

        if (_commandPaletteIs(Visibility::Visible) &&
            cmd.ActionAndArgs().Action() != ShortcutAction::ToggleCommandPalette)
        {
            CommandPaletteElement().Visibility(Visibility::Collapsed);
        }
        if (_suggestionsControlIs(Visibility::Visible) &&
            cmd.ActionAndArgs().Action() != ShortcutAction::ToggleCommandPalette)
        {
            SuggestionsElement().Visibility(Visibility::Collapsed);
        }

        // Let's assume the user has bound the dead key "^" to a sendInput command that sends "b".
        // If the user presses the two keys "^a" it'll produce "bâ", despite us marking the key event as handled.
        // The following is used to manually "consume" such dead keys and clear them from the keyboard state.
        _ClearKeyboardState(vkey, scanCode);
        e.Handled(true);
    }

    bool TerminalPage::OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down)
    {
        const auto modifiers = _GetPressedModifierKeys();
        if (vkey == VK_SPACE && modifiers.IsAltPressed() && down)
        {
            if (const auto actionMap = _settings.ActionMap())
            {
                if (const auto cmd = actionMap.GetActionByKeyChord({
                        modifiers.IsCtrlPressed(),
                        modifiers.IsAltPressed(),
                        modifiers.IsShiftPressed(),
                        modifiers.IsWinPressed(),
                        gsl::narrow_cast<int32_t>(vkey),
                        scanCode,
                    }))
                {
                    return _actionDispatch->DoAction(cmd.ActionAndArgs());
                }
            }
        }
        return false;
    }

    // Method Description:
    // - Get the modifier keys that are currently pressed. This can be used to
    //   find out which modifiers (ctrl, alt, shift) are pressed in events that
    //   don't necessarily include that state.
    // - This is a copy of TermControl::_GetPressedModifierKeys.
    // Return Value:
    // - The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
    ControlKeyStates TerminalPage::_GetPressedModifierKeys() noexcept
    {
        const auto window = CoreWindow::GetForCurrentThread();
        // DONT USE
        //      != CoreVirtualKeyStates::None
        // OR
        //      == CoreVirtualKeyStates::Down
        // Sometimes with the key down, the state is Down | Locked.
        // Sometimes with the key up, the state is Locked.
        // IsFlagSet(Down) is the only correct solution.

        struct KeyModifier
        {
            VirtualKey vkey;
            ControlKeyStates flags;
        };

        constexpr std::array<KeyModifier, 7> modifiers{ {
            { VirtualKey::RightMenu, ControlKeyStates::RightAltPressed },
            { VirtualKey::LeftMenu, ControlKeyStates::LeftAltPressed },
            { VirtualKey::RightControl, ControlKeyStates::RightCtrlPressed },
            { VirtualKey::LeftControl, ControlKeyStates::LeftCtrlPressed },
            { VirtualKey::Shift, ControlKeyStates::ShiftPressed },
            { VirtualKey::RightWindows, ControlKeyStates::RightWinPressed },
            { VirtualKey::LeftWindows, ControlKeyStates::LeftWinPressed },
        } };

        ControlKeyStates flags;

        for (const auto& mod : modifiers)
        {
            const auto state = window.GetKeyState(mod.vkey);
            const auto isDown = WI_IsFlagSet(state, CoreVirtualKeyStates::Down);

            if (isDown)
            {
                flags |= mod.flags;
            }
        }

        return flags;
    }

    // Method Description:
    // - Discards currently pressed dead keys.
    // - This is a copy of TermControl::_ClearKeyboardState.
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    void TerminalPage::_ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept
    {
        std::array<BYTE, 256> keyState;
        if (!GetKeyboardState(keyState.data()))
        {
            return;
        }

        // As described in "Sometimes you *want* to interfere with the keyboard's state buffer":
        //   http://archives.miloush.net/michkap/archive/2006/09/10/748775.html
        // > "The key here is to keep trying to pass stuff to ToUnicode until -1 is not returned."
        std::array<wchar_t, 16> buffer;
        while (ToUnicodeEx(vkey, scanCode, keyState.data(), buffer.data(), gsl::narrow_cast<int>(buffer.size()), 0b1, nullptr) < 0)
        {
        }
    }

    // Method Description:
    // - Configure the AppKeyBindings to use our ShortcutActionDispatch and the updated ActionMap
    //    as the object to handle dispatching ShortcutAction events.
    // Arguments:
    // - bindings: An IActionMapView object to wire up with our event handlers
    void TerminalPage::_HookupKeyBindings(const IActionMapView& actionMap) noexcept
    {
        _bindings->SetDispatch(*_actionDispatch);
        _bindings->SetActionMap(actionMap);
    }

    // Method Description:
    // - Register our event handlers with our ShortcutActionDispatch. The
    //   ShortcutActionDispatch is responsible for raising the appropriate
    //   events for an ActionAndArgs. WE'll handle each possible event in our
    //   own way.
    // Arguments:
    // - <none>
    void TerminalPage::_RegisterActionCallbacks()
    {
        // Hook up the ShortcutActionDispatch object's events to our handlers.
        // They should all be hooked up here, regardless of whether or not
        // there's an actual keychord for them.
#define ON_ALL_ACTIONS(action) HOOKUP_ACTION(action);
        ALL_SHORTCUT_ACTIONS
        INTERNAL_SHORTCUT_ACTIONS
#undef ON_ALL_ACTIONS
    }

    // Method Description:
    // - Get the title of the currently focused terminal control. If this tab is
    //   the focused tab, then also bubble this title to any listeners of our
    //   TitleChanged event.
    // Arguments:
    // - tab: the Tab to update the title for.
    void TerminalPage::_UpdateTitle(const Tab& tab)
    {
        if (tab == _GetFocusedTab())
        {
            TitleChanged.raise(*this, nullptr);
        }
        // Agentmaster: a Claude session's title is one value (Explorer name == tab title ==
        // persisted record). Title changes on a Claude tab are user renames -> mirror them back
        // into the registry. No-op for non-Claude tabs and for our own pin/sync writes.
        _SyncClaudeTitleFromTab(tab);
    }

    // Method Description:
    // - Connects event handlers to the TermControl for events that we want to
    //   handle. This includes:
    //    * the Copy and Paste events, for setting and retrieving clipboard data
    //      on the right thread
    // Arguments:
    // - term: The newly created TermControl to connect the events for
    void TerminalPage::_RegisterTerminalEvents(TermControl term)
    {
        term.RaiseNotice({ this, &TerminalPage::_ControlNoticeRaisedHandler });

        term.WriteToClipboard({ get_weak(), &TerminalPage::_copyToClipboard });
        term.PasteFromClipboard({ this, &TerminalPage::_PasteFromClipboardHandler });

        term.OpenHyperlink({ this, &TerminalPage::_OpenHyperlinkHandler });

        // Add an event handler for when the terminal or tab wants to set a
        // progress indicator on the taskbar
        term.SetTaskbarProgress({ get_weak(), &TerminalPage::_SetTaskbarProgressHandler });

        term.ConnectionStateChanged({ get_weak(), &TerminalPage::_ConnectionStateChangedHandler });

        term.PropertyChanged([weakThis = get_weak()](auto& /*sender*/, auto& e) {
            if (auto page{ weakThis.get() })
            {
                if (e.PropertyName() == L"BackgroundBrush")
                {
                    page->_updateThemeColors();
                }
            }
        });

        term.ShowWindowChanged({ get_weak(), &TerminalPage::_ShowWindowChangedHandler });
        term.SearchMissingCommand({ get_weak(), &TerminalPage::_SearchMissingCommandHandler });
        term.WindowSizeChanged({ get_weak(), &TerminalPage::_WindowSizeChanged });

        // Don't even register for the event if the feature is compiled off.
        if constexpr (Feature_ShellCompletions::IsEnabled())
        {
            term.CompletionsChanged({ get_weak(), &TerminalPage::_ControlCompletionsChangedHandler });
        }
        winrt::weak_ref<TermControl> weakTerm{ term };
        term.ContextMenu().Opening([weak = get_weak(), weakTerm](auto&& sender, auto&& /*args*/) {
            if (const auto& page{ weak.get() })
            {
                page->_PopulateContextMenu(weakTerm.get(), sender.try_as<MUX::Controls::CommandBarFlyout>(), false);
            }
        });
        term.SelectionContextMenu().Opening([weak = get_weak(), weakTerm](auto&& sender, auto&& /*args*/) {
            if (const auto& page{ weak.get() })
            {
                page->_PopulateContextMenu(weakTerm.get(), sender.try_as<MUX::Controls::CommandBarFlyout>(), true);
            }
        });
        if constexpr (Feature_QuickFix::IsEnabled())
        {
            term.QuickFixMenu().Opening([weak = get_weak(), weakTerm](auto&& sender, auto&& /*args*/) {
                if (const auto& page{ weak.get() })
                {
                    page->_PopulateQuickFixMenu(weakTerm.get(), sender.try_as<Controls::MenuFlyout>());
                }
            });
        }
    }

    // Method Description:
    // - Connects event handlers to the Tab for events that we want to
    //   handle. This includes:
    //    * the TitleChanged event, for changing the text of the tab
    //    * the Color{Selected,Cleared} events to change the color of a tab.
    // Arguments:
    // - hostingTab: The Tab that's hosting this TermControl instance
    void TerminalPage::_RegisterTabEvents(Tab& hostingTab)
    {
        auto weakTab{ hostingTab.get_weak() };
        auto weakThis{ get_weak() };
        // PropertyChanged is the generic mechanism by which the Tab
        // communicates changes to any of its observable properties, including
        // the Title
        hostingTab.PropertyChanged([weakTab, weakThis](auto&&, const WUX::Data::PropertyChangedEventArgs& args) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };
            if (page && tab)
            {
                const auto propertyName = args.PropertyName();
                if (propertyName == L"Title")
                {
                    page->_UpdateTitle(*tab);
                }
                else if (propertyName == L"Content")
                {
                    if (*tab == page->_GetFocusedTab())
                    {
                        const auto children = page->_tabContent.Children();

                        children.Clear();
                        if (auto content = tab->Content())
                        {
                            page->_tabContent.Children().Append(std::move(content));
                        }

                        tab->Focus(FocusState::Programmatic);
                    }
                }
            }
        });

        // Add an event handler for when the terminal or tab wants to set a
        // progress indicator on the taskbar
        hostingTab.TaskbarProgressChanged({ get_weak(), &TerminalPage::_SetTaskbarProgressHandler });

        hostingTab.RestartTerminalRequested({ get_weak(), &TerminalPage::_restartPaneConnection });

        // Agentmaster: a Claude tab's color is shared by every tab in its working directory. When
        // the user changes it (color picker / setTabColor action -> SetRuntimeTabColor), mirror it
        // onto the dir's other tabs and persist it per directory.
        hostingTab.TabColorChanged([weakTab, weakThis]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };
            if (page && tab)
            {
                page->_OnClaudeTabColorChanged(*tab);
            }
        });
    }

    // Method Description:
    // - Helper to manually exit "zoom" when certain actions take place.
    //   Anything that modifies the state of the pane tree should probably
    //   un-zoom the focused pane first, so that the user can see the full pane
    //   tree again. These actions include:
    //   * Splitting a new pane
    //   * Closing a pane
    //   * Moving focus between panes
    //   * Resizing a pane
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UnZoomIfNeeded()
    {
        if (const auto activeTab{ _GetFocusedTabImpl() })
        {
            if (activeTab->IsZoomed())
            {
                // Remove the content from the tab first, so Pane::UnZoom can
                // re-attach the content to the tree w/in the pane
                _tabContent.Children().Clear();
                // In ExitZoom, we'll change the Tab's Content(), triggering the
                // content changed event, which will re-attach the tab's new content
                // root to the tree.
                activeTab->ExitZoom();
            }
        }
    }

    // Method Description:
    // - Attempt to move focus between panes, as to focus the child on
    //   the other side of the separator. See Pane::NavigateFocus for details.
    // - Moves the focus of the currently focused tab.
    // Arguments:
    // - direction: The direction to move the focus in.
    // Return Value:
    // - Whether changing the focus succeeded. This allows a keychord to propagate
    //   to the terminal when no other panes are present (GH#6219)
    bool TerminalPage::_MoveFocus(const FocusDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            return tabImpl->NavigateFocus(direction);
        }
        return false;
    }

    // Method Description:
    // - Attempt to swap the positions of the focused pane with another pane.
    //   See Pane::SwapPane for details.
    // Arguments:
    // - direction: The direction to move the focused pane in.
    // Return Value:
    // - true if panes were swapped.
    bool TerminalPage::_SwapPane(const FocusDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            return tabImpl->SwapPane(direction);
        }
        return false;
    }

    TermControl TerminalPage::_GetActiveControl() const
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            return tabImpl->GetActiveTerminalControl();
        }
        return nullptr;
    }

    CommandPalette TerminalPage::LoadCommandPalette()
    {
        if (const auto p = CommandPaletteElement())
        {
            return p;
        }

        return _loadCommandPaletteSlowPath();
    }
    bool TerminalPage::_commandPaletteIs(WUX::Visibility visibility)
    {
        const auto p = CommandPaletteElement();
        return p && p.Visibility() == visibility;
    }

    CommandPalette TerminalPage::_loadCommandPaletteSlowPath()
    {
        const auto p = FindName(L"CommandPaletteElement").as<CommandPalette>();

        p.SetActionMap(_settings.ActionMap());

        // When the visibility of the command palette changes to "collapsed",
        // the palette has been closed. Toss focus back to the currently active control.
        p.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (_commandPaletteIs(Visibility::Collapsed))
            {
                _FocusActiveControl(nullptr, nullptr);
            }
        });
        p.DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
        p.CommandLineExecutionRequested({ this, &TerminalPage::_OnCommandLineExecutionRequested });
        p.SwitchToTabRequested({ this, &TerminalPage::_OnSwitchToTabRequested });
        p.PreviewAction({ this, &TerminalPage::_PreviewActionHandler });

        return p;
    }

    SuggestionsControl TerminalPage::LoadSuggestionsUI()
    {
        if (const auto p = SuggestionsElement())
        {
            return p;
        }

        return _loadSuggestionsElementSlowPath();
    }
    bool TerminalPage::_suggestionsControlIs(WUX::Visibility visibility)
    {
        const auto p = SuggestionsElement();
        return p && p.Visibility() == visibility;
    }

    SuggestionsControl TerminalPage::_loadSuggestionsElementSlowPath()
    {
        const auto p = FindName(L"SuggestionsElement").as<SuggestionsControl>();

        p.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (SuggestionsElement().Visibility() == Visibility::Collapsed)
            {
                _FocusActiveControl(nullptr, nullptr);
            }
        });
        p.DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
        p.PreviewAction({ this, &TerminalPage::_PreviewActionHandler });

        return p;
    }

    // Method Description:
    // - Warn the user that they are about to close all open windows, then
    //   signal that we want to close everything.
    safe_void_coroutine TerminalPage::RequestQuit()
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        if (setting != ConfirmOnClose::Never && !_displayingCloseDialog)
        {
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::CloseAll);
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        QuitRequested.raise(nullptr, nullptr);
    }

    void TerminalPage::PersistState()
    {
        // This method may be called for a window even if it hasn't had a tab yet or lost all of them.
        // We shouldn't persist such windows.
        const auto tabCount = _tabs.Size();
        if (_startupState != StartupState::Initialized || tabCount == 0)
        {
            return;
        }

        std::vector<ActionAndArgs> actions;

        for (auto tab : _tabs)
        {
            auto t = winrt::get_self<implementation::Tab>(tab);
            auto tabActions = t->BuildStartupActions(BuildStartupKind::Persist);
            actions.insert(actions.end(), std::make_move_iterator(tabActions.begin()), std::make_move_iterator(tabActions.end()));
        }

        // Avoid persisting a window with zero tabs, because `BuildStartupActions` happened to return an empty vector.
        if (actions.empty())
        {
            return;
        }

        // if the focused tab was not the last tab, restore that
        auto idx = _GetFocusedTabIndex();
        if (idx && idx != tabCount - 1)
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::SwitchToTab);
            SwitchToTabArgs switchToTabArgs{ idx.value() };
            action.Args(switchToTabArgs);

            actions.emplace_back(std::move(action));
        }

        // If the user set a custom name, save it
        if (const auto& windowName{ _WindowProperties.WindowName() }; !windowName.empty())
        {
            ActionAndArgs action;
            action.Action(ShortcutAction::RenameWindow);
            RenameWindowArgs args{ windowName };
            action.Args(args);

            actions.emplace_back(std::move(action));
        }

        WindowLayout layout;
        layout.TabLayout(winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)));

        auto mode = LaunchMode::DefaultMode;
        WI_SetFlagIf(mode, LaunchMode::FullscreenMode, _isFullscreen);
        WI_SetFlagIf(mode, LaunchMode::FocusMode, _isInFocusMode);
        WI_SetFlagIf(mode, LaunchMode::MaximizedMode, _isMaximized);

        layout.LaunchMode({ mode });

        // Only save the content size because the tab size will be added on load.
        const auto contentWidth = static_cast<float>(_tabContent.ActualWidth());
        const auto contentHeight = static_cast<float>(_tabContent.ActualHeight());
        const winrt::Windows::Foundation::Size windowSize{ contentWidth, contentHeight };

        layout.InitialSize(windowSize);

        // We don't actually know our own position. So we have to ask the window
        // layer for that.
        const auto launchPosRequest{ winrt::make<LaunchPositionRequest>() };
        RequestLaunchPosition.raise(*this, launchPosRequest);
        layout.InitialPosition(launchPosRequest.Position());

        ApplicationState::SharedInstance().AppendPersistedWindowLayout(layout);
    }

    // Method Description:
    // - Determines whether a close-window action should show a confirmation
    //   dialog, based on the confirmOnClose setting and the current window state.
    // Arguments:
    // - <none>
    // Return Value:
    // - true, if a warning dialog should be shown before closing the window
    bool TerminalPage::_ShouldWarnOnClose() const
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        switch (setting)
        {
        case ConfirmOnClose::Always:
            return true;
        case ConfirmOnClose::Automatic:
        {
            // Warn if there's more than one tab, or the one tab has more than one pane.
            return _HasMultipleTabs() || _GetTabImpl(_tabs.GetAt(0))->GetLeafPaneCount() > 1;
        }
        case ConfirmOnClose::Never:
        default:
            return false;
        }
    }

    // Method Description:
    // - Determines whether closing a specific tab should show a confirmation
    //   dialog, based on the confirmOnClose setting and the tab's state.
    // Arguments:
    // - tab: The tab being closed
    // Return Value:
    // - true, if a warning dialog should be shown before closing the tab
    bool TerminalPage::_ShouldWarnOnCloseTab(const winrt::com_ptr<Tab>& tab) const
    {
        const auto setting = _settings.GlobalSettings().ConfirmOnClose();
        switch (setting)
        {
        case ConfirmOnClose::Always:
            return true;
        case ConfirmOnClose::Automatic:
            // Warn if this tab has more than one pane.
            return tab->GetLeafPaneCount() > 1;
        case ConfirmOnClose::Never:
        default:
            return false;
        }
    }

    // Method Description:
    // - Close the terminal app. If the confirmOnClose setting indicates we should
    //   warn for the current window state, show a warning dialog.
    safe_void_coroutine TerminalPage::CloseWindow()
    {
        if (_ShouldWarnOnClose() &&
            !_displayingCloseDialog)
        {
            if (_newTabButton && _newTabButton.Flyout())
            {
                _newTabButton.Flyout().Hide();
            }
            _DismissTabContextMenus();
            _displayingCloseDialog = true;

            const auto weak = get_weak();
            auto warningResult = co_await _ShowConfirmCloseDialog(ConfirmCloseDialogKind::Window);
            // Hold a strong reference to `this` after the co_await; we may
            // be the last holder if the window was already being torn down.
            auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        // Agentmaster (M10 window-grouped restore): capture this window's record ONE LAST TIME while it
        // is still intact — the live geometry AND the ordered Claude/Other tab refs — BEFORE the archive
        // below clears _claudeTabs. _CaptureWindowRecord reads sessionId refs out of _claudeTabs (via
        // _ClaudeSessionForTab); if it ran after the teardown-archive, every Claude tab would degrade to
        // an anonymous Other ref and lose its session, so the reopened window couldn't re-home it. This
        // also pins the final move/resize past the 750ms autosave debounce (the close-flush, §13.5).
        _FlushWindowRecord();

        // Agentmaster (lifecycle gap #1): archive this window's live sessions NOW — before the host
        // tears us down — so they don't linger as phantom cards on other windows or orphan claude.exe.
        // (Confirm already passed above; the destructor repeats this idempotently as a backstop.)
        _ArchiveWindowSessionsOnTeardown();

        CloseWindowRequested.raise(*this, nullptr);
    }

    std::vector<IPaneContent> TerminalPage::Panes() const
    {
        std::vector<IPaneContent> panes;

        for (const auto tab : _tabs)
        {
            const auto impl = _GetTabImpl(tab);
            if (!impl)
            {
                continue;
            }

            impl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (auto content = pane->GetContent())
                {
                    panes.push_back(std::move(content));
                }
            });
        }

        return panes;
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a number of lines.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    // - rowsToScroll: a number of lines to move the viewport. If not provided we will use a system default.
    void TerminalPage::_Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            uint32_t realRowsToScroll;
            if (rowsToScroll == nullptr)
            {
                // The magic value of WHEEL_PAGESCROLL indicates that we need to scroll the entire page
                realRowsToScroll = _systemRowsToScroll == WHEEL_PAGESCROLL ?
                                       tabImpl->GetActiveTerminalControl().ViewHeight() :
                                       _systemRowsToScroll;
            }
            else
            {
                // use the custom value specified in the command
                realRowsToScroll = rowsToScroll.Value();
            }
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, realRowsToScroll);
            tabImpl->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Moves the currently active pane on the currently active tab to the
    //   specified tab. If the tab index is greater than the number of
    //   tabs, then a new tab will be created for the pane. Similarly, if a pane
    //   is the last remaining pane on a tab, that tab will be closed upon moving.
    // - No move will occur if the tabIdx is the same as the current tab, or if
    //   the specified tab is not a host of terminals (such as the settings tab).
    // - If the Window is specified, the pane will instead be detached and moved
    //   to the window with the given name/id.
    // Return Value:
    // - true if the pane was successfully moved to the new tab.
    bool TerminalPage::_MovePane(MovePaneArgs args)
    {
        const auto tabIdx{ args.TabIndex() };
        const auto windowId{ args.Window() };

        auto focusedTab{ _GetFocusedTabImpl() };

        if (!focusedTab)
        {
            return false;
        }

        // If there was a windowId in the action, try to move it to the
        // specified window instead of moving it in our tab row.
        if (!windowId.empty())
        {
            if (const auto tabImpl{ _GetFocusedTabImpl() })
            {
                if (const auto pane{ tabImpl->GetActivePane() })
                {
                    auto startupActions = pane->BuildStartupActions(0, 1, BuildStartupKind::MovePane);
                    _DetachPaneFromWindow(pane);
                    _MoveContent(std::move(startupActions.args), windowId, tabIdx);
                    focusedTab->DetachPane();

                    if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
                    {
                        if (windowId == L"new")
                        {
                            autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                            Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                            RS_(L"TerminalPage_PaneMovedAnnouncement_NewWindow"),
                                                            L"TerminalPageMovePaneToNewWindow" /* unique name for this notification category */);
                        }
                        else
                        {
                            autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                            Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                            RS_fmt(L"TerminalPage_PaneMovedAnnouncement_ExistingWindow2", windowId),
                                                            L"TerminalPageMovePaneToExistingWindow" /* unique name for this notification category */);
                        }
                    }
                    return true;
                }
            }
        }

        // If we are trying to move from the current tab to the current tab do nothing.
        if (_GetFocusedTabIndex() == tabIdx)
        {
            return false;
        }

        // Moving the pane from the current tab might close it, so get the next
        // tab before its index changes.
        if (tabIdx < _tabs.Size())
        {
            auto targetTab = _GetTabImpl(_tabs.GetAt(tabIdx));
            // if the selected tab is not a host of terminals (e.g. settings)
            // don't attempt to add a pane to it.
            if (!targetTab)
            {
                return false;
            }
            auto pane = focusedTab->DetachPane();
            targetTab->AttachPane(pane);
            _SetFocusedTab(*targetTab);

            if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
            {
                const auto tabTitle = targetTab->Title();
                autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                RS_fmt(L"TerminalPage_PaneMovedAnnouncement_ExistingTab", tabTitle),
                                                L"TerminalPageMovePaneToExistingTab" /* unique name for this notification category */);
            }
        }
        else
        {
            auto pane = focusedTab->DetachPane();
            _CreateNewTabFromPane(pane);
            if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
            {
                autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                RS_(L"TerminalPage_PaneMovedAnnouncement_NewTab"),
                                                L"TerminalPageMovePaneToNewTab" /* unique name for this notification category */);
            }
        }

        return true;
    }

    // Detach a tree of panes from this terminal. Helper used for moving panes
    // and tabs to other windows.
    void TerminalPage::_DetachPaneFromWindow(std::shared_ptr<Pane> pane)
    {
        pane->WalkTree([&](auto p) {
            if (const auto& control{ p->GetTerminalControl() })
            {
                _manager.Detach(control);
            }
        });
    }

    void TerminalPage::_DetachTabFromWindow(const winrt::com_ptr<Tab>& tab)
    {
        // Detach the root pane, which will act like the whole tab got detached.
        if (const auto rootPane = tab->GetRootPane())
        {
            _DetachPaneFromWindow(rootPane);
        }
    }

    // Method Description:
    // - Serialize these actions to json, and raise them as a RequestMoveContent
    //   event. Our Window will raise that to the window manager / monarch, who
    //   will dispatch this blob of json back to the window that should handle
    //   this.
    // - `actions` will be emptied into a winrt IVector as a part of this method
    //   and should be expected to be empty after this call.
    void TerminalPage::_MoveContent(std::vector<Settings::Model::ActionAndArgs>&& actions,
                                    const winrt::hstring& windowName,
                                    const uint32_t tabIndex,
                                    const std::optional<winrt::Windows::Foundation::Point>& dragPoint)
    {
        const auto winRtActions{ winrt::single_threaded_vector<ActionAndArgs>(std::move(actions)) };
        const auto str{ ActionAndArgs::Serialize(winRtActions) };
        const auto request = winrt::make_self<RequestMoveContentArgs>(windowName,
                                                                      str,
                                                                      tabIndex);
        if (dragPoint.has_value())
        {
            request->WindowPosition(*dragPoint);
        }
        RequestMoveContent.raise(*this, *request);
    }

    bool TerminalPage::_MoveTab(winrt::com_ptr<Tab> tab, MoveTabArgs args)
    {
        if (!tab)
        {
            return false;
        }

        // If there was a windowId in the action, try to move it to the
        // specified window instead of moving it in our tab row.
        const auto windowId{ args.Window() };
        if (!windowId.empty())
        {
            // if the windowId is the same as our name, do nothing
            if (windowId == WindowProperties().WindowName() ||
                windowId == winrt::to_hstring(WindowProperties().WindowId()))
            {
                return true;
            }

            if (tab)
            {
                auto startupActions = tab->BuildStartupActions(BuildStartupKind::Content);
                _DetachTabFromWindow(tab);
                _MoveContent(std::move(startupActions), windowId, 0);
                _RemoveTab(*tab);
                if (auto autoPeer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(*this))
                {
                    const auto tabTitle = tab->Title();
                    if (windowId == L"new")
                    {
                        autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                        Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                        RS_fmt(L"TerminalPage_TabMovedAnnouncement_NewWindow", tabTitle),
                                                        L"TerminalPageMoveTabToNewWindow" /* unique name for this notification category */);
                    }
                    else
                    {
                        autoPeer.RaiseNotificationEvent(Automation::Peers::AutomationNotificationKind::ActionCompleted,
                                                        Automation::Peers::AutomationNotificationProcessing::ImportantMostRecent,
                                                        RS_fmt(L"TerminalPage_TabMovedAnnouncement_Default", tabTitle, windowId),
                                                        L"TerminalPageMoveTabToExistingWindow" /* unique name for this notification category */);
                    }
                }
                return true;
            }
        }

        const auto direction = args.Direction();
        if (direction != MoveTabDirection::None)
        {
            // Use the requested tab, if provided. Otherwise, use the currently
            // focused tab.
            const auto tabIndex = til::coalesce(_GetTabIndex(*tab),
                                                _GetFocusedTabIndex());
            if (tabIndex)
            {
                const auto currentTabIndex = tabIndex.value();
                const auto delta = direction == MoveTabDirection::Forward ? 1 : -1;
                _TryMoveTab(currentTabIndex, currentTabIndex + delta);
            }
        }

        return true;
    }

    // When the tab's active pane changes, we'll want to lookup a new icon
    // for it. The Title change will be propagated upwards through the tab's
    // PropertyChanged event handler.
    void TerminalPage::_activePaneChanged(winrt::TerminalApp::Tab sender,
                                          Windows::Foundation::IInspectable /*args*/)
    {
        if (const auto tab{ _GetTabImpl(sender) })
        {
            // Possibly update the icon of the tab.
            _UpdateTabIcon(*tab);

            _updateThemeColors();

            // Update the taskbar progress as well. We'll raise our own
            // SetTaskbarProgress event here, to get tell the hosting
            // application to re-query this value from us.
            SetTaskbarProgress.raise(*this, nullptr);

            auto profile = tab->GetFocusedProfile();
            _UpdateBackground(profile);
        }

        _adjustProcessPriorityThrottled->Run();
    }

    uint32_t TerminalPage::NumberOfTabs() const
    {
        return _tabs.Size();
    }

    // Method Description:
    // - Called when it is determined that an existing tab or pane should be
    //   attached to our window. content represents a blob of JSON describing
    //   some startup actions for rebuilding the specified panes. They will
    //   include `__content` properties with the GUID of the existing
    //   ControlInteractivity's we should use, rather than starting new ones.
    // - _MakePane is already enlightened to use the ContentId property to
    //   reattach instead of create new content, so this method simply needs to
    //   parse the JSON and pump it into our action handler. Almost the same as
    //   doing something like `wt -w 0 nt`.
    void TerminalPage::AttachContent(IVector<Settings::Model::ActionAndArgs> args, uint32_t tabIndex)
    {
        if (args == nullptr ||
            args.Size() == 0)
        {
            return;
        }

        const auto& firstAction = args.GetAt(0);
        const bool firstIsSplitPane{ firstAction.Action() == ShortcutAction::SplitPane };

        // `splitPane` allows the user to specify which tab to split. In that
        // case, split specifically the requested pane.
        //
        // If there's not enough tabs, then just turn this pane into a new tab.
        //
        // If the first action is `newTab`, the index is always going to be 0,
        // so don't do anything in that case.
        if (firstIsSplitPane && tabIndex < _tabs.Size())
        {
            _SelectTab(tabIndex);
        }

        for (const auto& action : args)
        {
            _actionDispatch->DoAction(action);
        }

        // After handling all the actions, then re-check the tabIndex. We might
        // have been called as a part of a tab drag/drop. In that case, the
        // tabIndex is actually relevant, and we need to move the tab we just
        // made into position.
        if (!firstIsSplitPane && tabIndex != -1)
        {
            // Move the currently active tab to the requested index Use the
            // currently focused tab index, because we don't know if the new tab
            // opened at the end of the list, or adjacent to the previously
            // active tab. This is affected by the user's "newTabPosition"
            // setting.
            if (const auto focusedTabIndex = _GetFocusedTabIndex())
            {
                const auto source = *focusedTabIndex;
                _TryMoveTab(source, tabIndex);
            }
            // else: This shouldn't really be possible, because the tab we _just_ opened should be active.
        }
    }

    // Method Description:
    // - Split the focused pane of the given tab, either horizontally or vertically, and place the
    //   given pane accordingly
    // Arguments:
    // - tab: The tab that is going to be split.
    // - newPane: the pane to add to our tree of panes
    // - splitDirection: one value from the TerminalApp::SplitDirection enum, indicating how the
    //   new pane should be split from its parent.
    // - splitSize: the size of the split
    void TerminalPage::_SplitPane(const winrt::com_ptr<Tab>& tab,
                                  const SplitDirection splitDirection,
                                  const float splitSize,
                                  std::shared_ptr<Pane> newPane)
    {
        auto activeTab = tab;
        // Clever hack for a crash in startup, with multiple sub-commands. Say
        // you have the following commandline:
        //
        //   wtd nt -p "elevated cmd" ; sp -p "elevated cmd" ; sp -p "Command Prompt"
        //
        // Where "elevated cmd" is an elevated profile.
        //
        // In that scenario, we won't dump off the commandline immediately to an
        // elevated window, because it's got the final unelevated split in it.
        // However, when we get to that command, there won't be a tab yet. So
        // we'd crash right about here.
        //
        // Instead, let's just promote this first split to be a tab instead.
        // Crash avoided, and we don't need to worry about inserting a new-tab
        // command in at the start.
        if (!tab)
        {
            if (_tabs.Size() == 0)
            {
                _CreateNewTabFromPane(newPane);
                return;
            }
            else
            {
                activeTab = _GetFocusedTabImpl();
            }
        }

        // For now, prevent splitting the _settingsTab. We can always revisit this later.
        if (*activeTab == _settingsTab)
        {
            return;
        }

        // If the caller is calling us with the return value of _MakePane
        // directly, it's possible that nullptr was returned, if the connections
        // was supposed to be launched in an elevated window. In that case, do
        // nothing here. We don't have a pane with which to create the split.
        if (!newPane)
        {
            return;
        }
        const auto contentWidth = static_cast<float>(_tabContent.ActualWidth());
        const auto contentHeight = static_cast<float>(_tabContent.ActualHeight());
        const winrt::Windows::Foundation::Size availableSpace{ contentWidth, contentHeight };

        const auto realSplitType = activeTab->PreCalculateCanSplit(splitDirection, splitSize, availableSpace);
        if (!realSplitType)
        {
            return;
        }

        _UnZoomIfNeeded();
        auto [original, newGuy] = activeTab->SplitPane(*realSplitType, splitSize, newPane);

        // After GH#6586, the control will no longer focus itself
        // automatically when it's finished being laid out. Manually focus
        // the control here instead.
        if (_startupState == StartupState::Initialized)
        {
            if (const auto& content{ newGuy->GetContent() })
            {
                content.Focus(FocusState::Programmatic);
            }
        }
    }

    // Method Description:
    // - Switches the split orientation of the currently focused pane.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ToggleSplitOrientation()
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            tabImpl->ToggleSplitOrientation();
        }
    }

    // Method Description:
    // - Attempt to move a separator between panes, as to resize each child on
    //   either size of the separator. See Pane::ResizePane for details.
    // - Moves a separator on the currently focused tab.
    // Arguments:
    // - direction: The direction to move the separator in.
    // Return Value:
    // - whether a pane was resized
    bool TerminalPage::_ResizePane(const ResizeDirection& direction)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            return tabImpl->ResizePane(direction);
        }
        return false;
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a page. The page length will be dependent on the terminal view height.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    void TerminalPage::_ScrollPage(ScrollDirection scrollDirection)
    {
        // Do nothing if for some reason, there's no terminal tab in focus. We don't want to crash.
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            if (const auto& control{ _GetActiveControl() })
            {
                const auto termHeight = control.ViewHeight();
                auto scrollDelta = _ComputeScrollDelta(scrollDirection, termHeight);
                tabImpl->Scroll(scrollDelta);
            }
        }
    }

    void TerminalPage::_ScrollToBufferEdge(ScrollDirection scrollDirection)
    {
        if (const auto tabImpl{ _GetFocusedTabImpl() })
        {
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, INT_MAX);
            tabImpl->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Gets the title of the currently focused terminal control. If there
    //   isn't a control selected for any reason, returns "Terminal"
    // Arguments:
    // - <none>
    // Return Value:
    // - the title of the focused control if there is one, else "Terminal"
    hstring TerminalPage::Title()
    {
        if (_settings.GlobalSettings().ShowTitleInTitlebar())
        {
            if (const auto tab{ _GetFocusedTab() })
            {
                return tab.Title();
            }
        }
        return { L"Terminal" };
    }

    // Method Description:
    // - Handles the special case of providing a text override for the UI shortcut due to VK_OEM issue.
    //      Looks at the flags from the KeyChord modifiers and provides a concatenated string value of all
    //      in the same order that XAML would put them as well.
    // Return Value:
    // - a string representation of the key modifiers for the shortcut
    //NOTE: This needs to be localized with https://github.com/microsoft/terminal/issues/794 if XAML framework issue not resolved before then
    static std::wstring _FormatOverrideShortcutText(VirtualKeyModifiers modifiers)
    {
        std::wstring buffer{ L"" };

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Control))
        {
            buffer += L"Ctrl+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Shift))
        {
            buffer += L"Shift+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Menu))
        {
            buffer += L"Alt+";
        }

        if (WI_IsFlagSet(modifiers, VirtualKeyModifiers::Windows))
        {
            buffer += L"Win+";
        }

        return buffer;
    }

    // Method Description:
    // - Takes a MenuFlyoutItem and a corresponding KeyChord value and creates the accelerator for UI display.
    //   Takes into account a special case for an error condition for a comma
    // Arguments:
    // - MenuFlyoutItem that will be displayed, and a KeyChord to map an accelerator
    void TerminalPage::_SetAcceleratorForMenuItem(WUX::Controls::MenuFlyoutItem& menuItem,
                                                  const KeyChord& keyChord)
    {
#ifdef DEP_MICROSOFT_UI_XAML_708_FIXED
        // work around https://github.com/microsoft/microsoft-ui-xaml/issues/708 in case of VK_OEM_COMMA
        if (keyChord.Vkey() != VK_OEM_COMMA)
        {
            // use the XAML shortcut to give us the automatic capabilities
            auto menuShortcut = Windows::UI::Xaml::Input::KeyboardAccelerator{};

            // TODO: Modify this when https://github.com/microsoft/terminal/issues/877 is resolved
            menuShortcut.Key(static_cast<Windows::System::VirtualKey>(keyChord.Vkey()));

            // add the modifiers to the shortcut
            menuShortcut.Modifiers(keyChord.Modifiers());

            // add to the menu
            menuItem.KeyboardAccelerators().Append(menuShortcut);
        }
        else // we've got a comma, so need to just use the alternate method
#endif
        {
            // extract the modifier and key to a nice format
            auto overrideString = _FormatOverrideShortcutText(keyChord.Modifiers());
            auto mappedCh = MapVirtualKeyW(keyChord.Vkey(), MAPVK_VK_TO_CHAR);
            if (mappedCh != 0)
            {
                menuItem.KeyboardAcceleratorTextOverride(overrideString + gsl::narrow_cast<wchar_t>(mappedCh));
            }
        }
    }

    // Method Description:
    // - Calculates the appropriate size to snap to in the given direction, for
    //   the given dimension. If the global setting `snapToGridOnResize` is set
    //   to `false`, this will just immediately return the provided dimension,
    //   effectively disabling snapping.
    // - See Pane::CalcSnappedDimension
    float TerminalPage::CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
    {
        if (_settings && _settings.GlobalSettings().SnapToGridOnResize())
        {
            if (const auto tabImpl{ _GetFocusedTabImpl() })
            {
                return tabImpl->CalcSnappedDimension(widthOrHeight, dimension);
            }
        }
        return dimension;
    }

    // Function Description:
    // - This function is called when the `TermControl` requests that we send
    //   it the clipboard's content.
    // - Retrieves the data from the Windows Clipboard and converts it to text.
    // - Shows warnings if the clipboard is too big or contains multiple lines
    //   of text.
    // - Sends the text back to the TermControl through the event's
    //   `HandleClipboardData` member function.
    // - Does some of this in a background thread, as to not hang/crash the UI thread.
    // Arguments:
    // - eventArgs: the PasteFromClipboard event sent from the TermControl
    safe_void_coroutine TerminalPage::_PasteFromClipboardHandler(const IInspectable sender, const PasteFromClipboardEventArgs eventArgs)
    try
    {
        // The old Win32 clipboard API as used below is somewhere in the order of 300-1000x faster than
        // the WinRT one on average, depending on CPU load. Don't use the WinRT clipboard API if you can.
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher();
        const auto globalSettings = _settings.GlobalSettings();
        const auto bracketedPaste = eventArgs.BracketedPasteEnabled();
        const auto sourceId = sender.try_as<ControlInteractivity>().Id();

        // GetClipboardData might block for up to 30s for delay-rendered contents.
        co_await winrt::resume_background();

        winrt::hstring text;
        if (const auto clipboard = clipboard::open(nullptr))
        {
            text = clipboard::read();
        }

        if (!bracketedPaste && globalSettings.TrimPaste())
        {
            text = winrt::hstring{ Utils::TrimPaste(text) };
        }

        // LOAD BEARING: Send an empty bracketed paste even if the clipboard was empty.
        // Bracketed Paste provides an application a way to know whether the
        // user pasted, even if there was no applicable content on it. This
        // behavior is observed in GNOME Terminal, among others.
        if (!bracketedPaste && text.empty())
        {
            co_return;
        }

        bool warnMultiLine = false;
        switch (globalSettings.WarnAboutMultiLinePaste())
        {
        case WarnAboutMultiLinePaste::Automatic:
            // NOTE that this is unsafe, because a shell that doesn't support bracketed paste
            // will allow an attacker to enable the mode, not realize that, and then accept
            // the paste as if it was a series of legitimate commands. See GH#13014.
            warnMultiLine = !bracketedPaste;
            break;
        case WarnAboutMultiLinePaste::Always:
            warnMultiLine = true;
            break;
        default:
            warnMultiLine = false;
            break;
        }

        if (warnMultiLine)
        {
            const std::wstring_view view{ text };
            warnMultiLine = view.find_first_of(L"\r\n") != std::wstring_view::npos;
        }

        constexpr std::size_t minimumSizeForWarning = 1024 * 5; // 5 KiB
        const auto warnLargeText = text.size() > minimumSizeForWarning && globalSettings.WarnAboutLargePaste();

        if (warnMultiLine || warnLargeText)
        {
            co_await wil::resume_foreground(dispatcher);

            if (const auto strongThis = weakThis.get())
            {
                // We have to initialize the dialog here to be able to change the text of the text block within it
                std::ignore = FindName(L"MultiLinePasteDialog");

                // WinUI absolutely cannot deal with large amounts of text (at least O(n), possibly O(n^2),
                // so we limit the string length here and add an ellipsis if necessary.
                auto clipboardText = text;
                if (clipboardText.size() > 1024)
                {
                    const std::wstring_view view{ text };
                    // Make sure we don't cut in the middle of a surrogate pair
                    const auto len = til::utf16_iterate_prev(view, 512);
                    clipboardText = til::hstring_format(FMT_COMPILE(L"{}\n…"), view.substr(0, len));
                }

                ClipboardText().Text(std::move(clipboardText));

                // The vertical offset on the scrollbar does not reset automatically, so reset it manually
                ClipboardContentScrollViewer().ScrollToVerticalOffset(0);

                auto warningResult = ContentDialogResult::Primary;
                if (warnMultiLine)
                {
                    warningResult = co_await _ShowMultiLinePasteWarningDialog();
                }
                else if (warnLargeText)
                {
                    warningResult = co_await _ShowLargePasteWarningDialog();
                }

                // Clear the clipboard text so it doesn't lie around in memory
                ClipboardText().Text({});

                if (warningResult != ContentDialogResult::Primary)
                {
                    // user rejected the paste
                    co_return;
                }
            }

            co_await winrt::resume_background();
        }

        // This will end up calling ConptyConnection::WriteInput which calls WriteFile which may block for
        // an indefinite amount of time. Avoid freezes and deadlocks by running this on a background thread.
        assert(!dispatcher.HasThreadAccess());
        eventArgs.HandleClipboardData(text);

        // GH#18821: If broadcast input is active, paste the same text into all other
        // panes on the tab. We do this here (rather than re-reading the
        // clipboard per-pane) so that only one paste warning is shown.
        co_await wil::resume_foreground(dispatcher);
        if (const auto strongThis = weakThis.get())
        {
            if (const auto& tab{ strongThis->_GetFocusedTabImpl() })
            {
                if (tab->TabStatus().IsInputBroadcastActive())
                {
                    tab->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto control = pane->GetTerminalControl())
                        {
                            if (control.ContentId() != sourceId && !control.ReadOnly())
                            {
                                control.RawWriteString(text);
                            }
                        }
                    });
                }
            }
        }
    }
    CATCH_LOG();

    safe_void_coroutine TerminalPage::_OpenHyperlinkHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs)
    {
        try
        {
            auto uriString{ eventArgs.Uri() };
            auto parsed = winrt::Windows::Foundation::Uri(uriString);
            if (_IsUriSupported(parsed))
            {
                bool shouldLaunch{ _IsUriConsideredSomewhatSafe(parsed) };

                if (!shouldLaunch)
                {
                    if (auto presenter{ _dialogPresenter.get() })
                    {
                        // FindName needs to be called first to actually load the xaml object
                        auto unopenedUriDialog = FindName(L"UriErrorDialog").try_as<WUX::Controls::ContentDialog>();

                        // Insert the reason and the URI
                        unopenedUriDialog.SecondaryButtonText(RS_(L"UnsafeUrlConfirmAllowAction"));
                        CouldNotOpenUriReason().Text(RS_(L"UnsafeUrlConfirmText"));
                        UnopenedUri().Text(uriString);

                        // Show the dialog
                        auto result = co_await presenter.ShowDialog(unopenedUriDialog);
                        shouldLaunch = result == ContentDialogResult::Secondary;
                    }
                }

                if (shouldLaunch)
                {
                    ShellExecuteW(nullptr, L"open", uriString.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            else
            {
                _ShowCouldNotOpenDialog(RS_(L"UnsupportedSchemeText"), uriString);
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _ShowCouldNotOpenDialog(RS_(L"InvalidUriText"), eventArgs.Uri());
        }
    }

    // Method Description:
    // - Opens up a dialog box explaining why we could not open a URI
    // Arguments:
    // - The reason (unsupported scheme, invalid uri, potentially more in the future)
    // - The uri
    void TerminalPage::_ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto unopenedUriDialog = FindName(L"UriErrorDialog").try_as<WUX::Controls::ContentDialog>();

            // Insert the reason and the URI
            unopenedUriDialog.SecondaryButtonText({});
            CouldNotOpenUriReason().Text(reason);
            UnopenedUri().Text(uri);

            // Show the dialog
            presenter.ShowDialog(unopenedUriDialog);
        }
    }

    // Method Description:
    // - Determines if the given URI is currently supported
    // Arguments:
    // - The parsed URI
    // Return value:
    // - True if we support it, false otherwise
    bool TerminalPage::_IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri)
    {
        if (parsedUri.SchemeName() == L"http" || parsedUri.SchemeName() == L"https")
        {
            return true;
        }
        if (parsedUri.SchemeName() == L"file")
        {
            const auto host = parsedUri.Host();
            // If no hostname was provided or if the hostname was "localhost", Host() will return an empty string
            // and we allow it
            if (host == L"")
            {
                return true;
            }

            // GH#10188: WSL paths are okay. We'll let those through.
            if (host == L"wsl$" || host == L"wsl.localhost")
            {
                return true;
            }

            // TODO: by the OSC 8 spec, if a hostname (other than localhost) is provided, we _should_ be
            // comparing that value against what is returned by GetComputerNameExW and making sure they match.
            // However, ShellExecute does not seem to be happy with file URIs of the form
            //          file://{hostname}/path/to/file.ext
            // and so while we could do the hostname matching, we do not know how to actually open the URI
            // if its given in that form. So for now we ignore all hostnames other than localhost
            return false;
        }

        // In this case, the app manually output a URI other than file:// or
        // http(s)://. We'll trust the user knows what they're doing when
        // clicking on those sorts of links.
        // See discussion in GH#7562 for more details.
        return true;
    }

    bool TerminalPage::_IsUriConsideredSomewhatSafe(const winrt::Windows::Foundation::Uri& parsedUri) const
    {
        const auto& schemeName = parsedUri.SchemeName();

        if (schemeName == L"http" || schemeName == L"https")
        {
            return true;
        }
        if (schemeName == L"file")
        {
            static const auto pathext{ wil::TryGetEnvironmentVariableW<std::wstring>(L"PATHEXT") };
            const auto filename = parsedUri.Path();
            for (const auto& e : til::split_iterator{ std::wstring_view{ pathext }, L';' })
            {
                if (til::ends_with_insensitive_ascii(filename, e))
                {
                    return false;
                }
            }

            return true;
        }
        if (const auto& safeSchemes = _settings.GlobalSettings().SafeUriSchemes())
        {
            for (const auto& scheme : safeSchemes)
            {
                if (til::equals_insensitive_ascii(schemeName, scheme))
                {
                    return true;
                }
            }
        }

        return false;
    }

    // Important! Don't take this eventArgs by reference, we need to extend the
    // lifetime of it to the other side of the co_await!
    safe_void_coroutine TerminalPage::_ControlNoticeRaisedHandler(const IInspectable /*sender*/,
                                                                  const Microsoft::Terminal::Control::NoticeEventArgs eventArgs)
    {
        auto weakThis = get_weak();
        co_await wil::resume_foreground(Dispatcher());
        if (auto page = weakThis.get())
        {
            auto message = eventArgs.Message();

            winrt::hstring title;

            switch (eventArgs.Level())
            {
            case NoticeLevel::Debug:
                title = RS_(L"NoticeDebug"); //\xebe8
                break;
            case NoticeLevel::Info:
                title = RS_(L"NoticeInfo"); // \xe946
                break;
            case NoticeLevel::Warning:
                title = RS_(L"NoticeWarning"); //\xe7ba
                break;
            case NoticeLevel::Error:
                title = RS_(L"NoticeError"); //\xe783
                break;
            }

            page->_ShowControlNoticeDialog(title, message);
        }
    }

    void TerminalPage::_ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto controlNoticeDialog = FindName(L"ControlNoticeDialog").try_as<WUX::Controls::ContentDialog>();

            ControlNoticeDialog().Title(winrt::box_value(title));

            // Insert the message
            NoticeMessage().Text(message);

            // Show the dialog
            presenter.ShowDialog(controlNoticeDialog);
        }
    }

    // Method Description:
    // - Copy text from the focused terminal to the Windows Clipboard
    // Arguments:
    // - dismissSelection: if not enabled, copying text doesn't dismiss the selection
    // - singleLine: if enabled, copy contents as a single line of text
    // - withControlSequences: if enabled, the copied plain text contains color/style ANSI escape codes from the selection
    // - formats: dictate which formats need to be copied
    // Return Value:
    // - true iff we we able to copy text (if a selection was active)
    bool TerminalPage::_CopyText(const bool dismissSelection, const bool singleLine, const bool withControlSequences, const CopyFormat formats)
    {
        if (const auto& control{ _GetActiveControl() })
        {
            return control.CopySelectionToClipboard(dismissSelection, singleLine, withControlSequences, formats);
        }
        return false;
    }

    // Method Description:
    // - Send an event (which will be caught by AppHost) to set the progress indicator on the taskbar
    // Arguments:
    // - sender (not used)
    // - eventArgs: the arguments specifying how to set the progress indicator
    safe_void_coroutine TerminalPage::_SetTaskbarProgressHandler(const IInspectable /*sender*/, const IInspectable /*eventArgs*/)
    {
        const auto weak = get_weak();
        co_await wil::resume_foreground(Dispatcher());
        if (const auto strong = weak.get())
        {
            SetTaskbarProgress.raise(*this, nullptr);
        }
    }

    // Method Description:
    // - Send an event (which will be caught by AppHost) to change the show window state of the entire hosting window
    // Arguments:
    // - sender (not used)
    // - args: the arguments specifying how to set the display status to ShowWindow for our window handle
    void TerminalPage::_ShowWindowChangedHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::ShowWindowArgs args)
    {
        ShowWindowChanged.raise(*this, args);
    }

    Windows::Foundation::IAsyncOperation<IVectorView<MatchResult>> TerminalPage::_FindPackageAsync(hstring query)
    {
        const PackageManager packageManager = WindowsPackageManagerFactory::CreatePackageManager();
        PackageCatalogReference catalogRef{
            packageManager.GetPredefinedPackageCatalog(PredefinedPackageCatalog::OpenWindowsCatalog)
        };
        catalogRef.PackageCatalogBackgroundUpdateInterval(std::chrono::hours(24));

        ConnectResult connectResult{ nullptr };
        for (int retries = 0;;)
        {
            connectResult = catalogRef.Connect();
            if (connectResult.Status() == ConnectResultStatus::Ok)
            {
                break;
            }

            if (++retries == 3)
            {
                co_return nullptr;
            }
        }

        PackageCatalog catalog = connectResult.PackageCatalog();
        PackageMatchFilter filter = WindowsPackageManagerFactory::CreatePackageMatchFilter();
        filter.Value(query);
        filter.Field(PackageMatchField::Command);
        filter.Option(PackageFieldMatchOption::Equals);

        FindPackagesOptions options = WindowsPackageManagerFactory::CreateFindPackagesOptions();
        options.Filters().Append(filter);
        options.ResultLimit(20);

        const auto result = co_await catalog.FindPackagesAsync(options);
        const IVectorView<MatchResult> pkgList = result.Matches();
        co_return pkgList;
    }

    Windows::Foundation::IAsyncAction TerminalPage::_SearchMissingCommandHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::SearchMissingCommandEventArgs args)
    {
        if (!Feature_QuickFix::IsEnabled())
        {
            co_return;
        }

        const auto weak = get_weak();
        const auto dispatcher = Dispatcher();

        // All of the code until resume_foreground is static and
        // doesn't touch `this`, so we don't need weak/strong_ref.
        co_await winrt::resume_background();

        // no packages were found, nothing to suggest
        const auto pkgList = co_await _FindPackageAsync(args.MissingCommand());
        if (!pkgList || pkgList.Size() == 0)
        {
            co_return;
        }

        std::vector<hstring> suggestions;
        suggestions.reserve(pkgList.Size());
        for (const auto& pkg : pkgList)
        {
            // --id and --source ensure we don't collide with another package catalog
            suggestions.emplace_back(fmt::format(FMT_COMPILE(L"winget install --id {} -s winget"), pkg.CatalogPackage().Id()));
        }

        co_await wil::resume_foreground(dispatcher);
        const auto strong = weak.get();
        if (!strong)
        {
            co_return;
        }

        auto term = _GetActiveControl();
        if (!term)
        {
            co_return;
        }
        term.UpdateWinGetSuggestions(single_threaded_vector<hstring>(std::move(suggestions)));
        term.RefreshQuickFixMenu();
    }

    void TerminalPage::_WindowSizeChanged(const IInspectable sender, const Microsoft::Terminal::Control::WindowSizeChangedEventArgs args)
    {
        // Raise if:
        // - Not in quake mode
        // - Not in fullscreen
        // - Only one tab exists
        // - Only one pane exists
        // else:
        // - Reset conpty to its original size back
        if (!WindowProperties().IsQuakeWindow() && !Fullscreen() &&
            NumberOfTabs() == 1 && _GetFocusedTabImpl()->GetLeafPaneCount() == 1)
        {
            WindowSizeChanged.raise(*this, args);
        }
        else if (const auto& control{ sender.try_as<TermControl>() })
        {
            const auto& connection = control.Connection();

            if (const auto& conpty{ connection.try_as<TerminalConnection::ConptyConnection>() })
            {
                conpty.ResetSize();
            }
        }
    }

    void TerminalPage::_copyToClipboard(const IInspectable, const WriteToClipboardEventArgs args) const
    {
        if (const auto clipboard = clipboard::open(_hostingHwnd.value_or(nullptr)))
        {
            const auto plain = args.Plain();
            const auto html = args.Html();
            const auto rtf = args.Rtf();

            clipboard::write(
                { plain.data(), plain.size() },
                { reinterpret_cast<const char*>(html.data()), html.size() },
                { reinterpret_cast<const char*>(rtf.data()), rtf.size() });
        }
    }

    // Method Description:
    // - Paste text from the Windows Clipboard to the focused terminal
    void TerminalPage::_PasteText()
    {
        if (const auto& control{ _GetActiveControl() })
        {
            control.PasteTextFromClipboard();
        }
    }

    // Function Description:
    // - Called when the settings button is clicked. ShellExecutes the settings
    //   file, as to open it in the default editor for .json files. Does this in
    //   a background thread, as to not hang/crash the UI thread.
    safe_void_coroutine TerminalPage::_LaunchSettings(const SettingsTarget target)
    {
        if (target == SettingsTarget::SettingsUI)
        {
            OpenSettingsUI();
        }
        else
        {
            // This will switch the execution of the function to a background (not
            // UI) thread. This is IMPORTANT, because the Windows.Storage API's
            // (used for retrieving the path to the file) will crash on the UI
            // thread, because the main thread is a STA.
            //
            // NOTE: All remaining code of this function doesn't touch `this`, so we don't need weak/strong_ref.
            // NOTE NOTE: Don't touch `this` when you make changes here.
            co_await winrt::resume_background();

            auto openFile = [](const auto& filePath) {
                HINSTANCE res = ShellExecute(nullptr, nullptr, filePath.c_str(), nullptr, nullptr, SW_SHOW);
                if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
                {
                    ShellExecute(nullptr, nullptr, L"notepad", filePath.c_str(), nullptr, SW_SHOW);
                }
            };

            auto openFolder = [](const auto& filePath) {
                HINSTANCE res = ShellExecute(nullptr, nullptr, filePath.c_str(), nullptr, nullptr, SW_SHOW);
                if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
                {
                    ShellExecute(nullptr, nullptr, L"open", filePath.c_str(), nullptr, SW_SHOW);
                }
            };

            switch (target)
            {
            case SettingsTarget::DefaultsFile:
                openFile(CascadiaSettings::DefaultSettingsPath());
                break;
            case SettingsTarget::SettingsFile:
                openFile(CascadiaSettings::SettingsPath());
                break;
            case SettingsTarget::Directory:
                openFolder(CascadiaSettings::SettingsDirectory());
                break;
            case SettingsTarget::AllFiles:
                openFile(CascadiaSettings::DefaultSettingsPath());
                openFile(CascadiaSettings::SettingsPath());
                break;
            }
        }
    }

    // Method Description:
    // - Responds to the TabView control's Tab Closing event by removing
    //      the indicated tab from the set and focusing another one.
    //      The event is cancelled so App maintains control over the
    //      items in the tabview.
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabCloseRequested(const IInspectable& /*sender*/, const MUX::Controls::TabViewTabCloseRequestedEventArgs& eventArgs)
    {
        const auto tabViewItem = eventArgs.Tab();
        if (auto tab{ _GetTabByTabViewItem(tabViewItem) })
        {
            _HandleCloseTabRequested(tab);
        }
    }

    TermControl TerminalPage::_CreateNewControlAndContent(const Settings::TerminalSettingsCreateResult& settings, const ITerminalConnection& connection)
    {
        // Do any initialization that needs to apply to _every_ TermControl we
        // create here.
        const auto content = _manager.CreateCore(*settings.DefaultSettings(), settings.UnfocusedSettings().try_as<IControlAppearance>(), connection);
        const TermControl control{ content };
        return _SetupControl(control);
    }

    TermControl TerminalPage::_AttachControlToContent(const uint64_t& contentId)
    {
        if (const auto& content{ _manager.TryLookupCore(contentId) })
        {
            // We have to pass in our current keybindings, because that's an
            // object that belongs to this TerminalPage, on this thread. If we
            // don't, then when we move the content to another thread, and it
            // tries to handle a key, it'll callback on the original page's
            // stack, inevitably resulting in a wrong_thread
            return _SetupControl(TermControl::NewControlByAttachingContent(content));
        }
        return nullptr;
    }

    TermControl TerminalPage::_SetupControl(const TermControl& term)
    {
        // GH#12515: ConPTY assumes it's hidden at the start. If we're not, let it know now.
        if (_visible)
        {
            term.WindowVisibilityChanged(_visible);
        }

        // Even in the case of re-attaching content from another window, this
        // will correctly update the control's owning HWND
        if (_hostingHwnd.has_value())
        {
            term.OwningHwnd(reinterpret_cast<uint64_t>(*_hostingHwnd));
        }

        term.KeyBindings(*_bindings);

        _RegisterTerminalEvents(term);
        return term;
    }

    // Method Description:
    // - Creates a pane and returns a shared_ptr to it
    // - The caller should handle where the pane goes after creation,
    //   either to split an already existing pane or to create a new tab with it
    // Arguments:
    // - newTerminalArgs: an object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See CascadiaSettings::BuildSettings for more details.
    // - sourceTab: an optional tab reference that indicates that the created
    //   pane should be a duplicate of the tab's focused pane
    // - existingConnection: optionally receives a connection from the outside
    //   world instead of attempting to create one
    // Return Value:
    // - If the newTerminalArgs required us to open the pane as a new elevated
    //   connection, then we'll return nullptr. Otherwise, we'll return a new
    //   Pane for this connection.
    std::shared_ptr<Pane> TerminalPage::_MakeTerminalPane(const NewTerminalArgs& newTerminalArgs,
                                                          const winrt::TerminalApp::Tab& sourceTab,
                                                          TerminalConnection::ITerminalConnection existingConnection)
    {
        // First things first - Check for making a pane from content ID.
        if (newTerminalArgs &&
            newTerminalArgs.ContentId() != 0)
        {
            // Don't need to worry about duplicating or anything - we'll
            // serialize the actual profile's GUID along with the content guid.
            const auto& profile = _settings.GetProfileForArgs(newTerminalArgs);
            const auto control = _AttachControlToContent(newTerminalArgs.ContentId());
            auto paneContent{ winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, control) };
            return std::make_shared<Pane>(paneContent);
        }

        Settings::TerminalSettingsCreateResult controlSettings{ nullptr };
        Profile profile{ nullptr };

        if (const auto& tabImpl{ _GetTabImpl(sourceTab) })
        {
            profile = tabImpl->GetFocusedProfile();
            if (profile)
            {
                // TODO GH#5047 If we cache the NewTerminalArgs, we no longer need to do this.
                profile = GetClosestProfileForDuplicationOfProfile(profile);
                controlSettings = Settings::TerminalSettings::CreateWithProfile(_settings, profile);
                const auto workingDirectory = tabImpl->GetActiveTerminalControl().WorkingDirectory();
                if (Utils::IsValidDirectory(workingDirectory.c_str()))
                {
                    controlSettings.DefaultSettings()->StartingDirectory(workingDirectory);
                }
            }
        }
        if (!profile)
        {
            profile = _settings.GetProfileForArgs(newTerminalArgs);
            controlSettings = Settings::TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs);
        }

        // Try to handle auto-elevation
        if (_maybeElevate(newTerminalArgs, controlSettings, profile))
        {
            return nullptr;
        }

        const auto sessionId = controlSettings.DefaultSettings()->SessionId();
        const auto hasSessionId = sessionId != winrt::guid{};

        auto connection = existingConnection ? existingConnection : _CreateConnectionFromSettings(profile, *controlSettings.DefaultSettings(), hasSessionId);
        if (existingConnection)
        {
            connection.Resize(controlSettings.DefaultSettings()->InitialRows(), controlSettings.DefaultSettings()->InitialCols());
        }

        TerminalConnection::ITerminalConnection debugConnection{ nullptr };
        if (_settings.GlobalSettings().DebugFeaturesEnabled())
        {
            const auto window = CoreWindow::GetForCurrentThread();
            const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
            const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
            const auto bothAltsPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                                         WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);
            if (bothAltsPressed)
            {
                std::tie(connection, debugConnection) = OpenDebugTapConnection(connection);
            }
        }

        const auto control = _CreateNewControlAndContent(controlSettings, connection);

        if (hasSessionId)
        {
            using namespace std::string_view_literals;

            const auto settingsDir = CascadiaSettings::SettingsDirectory();
            const auto admin = IsRunningElevated();
            const auto filenamePrefix = admin ? L"elevated_"sv : L"buffer_"sv;
            const auto path = fmt::format(FMT_COMPILE(L"{}\\{}{}.txt"), settingsDir, filenamePrefix, sessionId);
            control.RestoreFromPath(path);
        }

        auto paneContent{ winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, control) };

        auto resultPane = std::make_shared<Pane>(paneContent);

        if (debugConnection) // this will only be set if global debugging is on and tap is active
        {
            auto newControl = _CreateNewControlAndContent(controlSettings, debugConnection);
            // Split (auto) with the debug tap.
            auto debugContent{ winrt::make<TerminalPaneContent>(profile, _terminalSettingsCache, newControl) };
            auto debugPane = std::make_shared<Pane>(debugContent);

            // Since we're doing this split directly on the pane (instead of going through Tab,
            // we need to handle the panes 'active' states

            // Set the pane we're splitting to active (otherwise Split will not do anything)
            resultPane->SetActive();
            auto [original, _] = resultPane->Split(SplitDirection::Automatic, 0.5f, debugPane);

            // Set the non-debug pane as active
            resultPane->ClearActive();
            original->SetActive();
        }

        return resultPane;
    }

    // NOTE: callers of _MakePane should be able to accept nullptr as a return
    // value gracefully.
    std::shared_ptr<Pane> TerminalPage::_MakePane(const INewContentArgs& contentArgs,
                                                  const winrt::TerminalApp::Tab& sourceTab,
                                                  TerminalConnection::ITerminalConnection existingConnection)

    {
        const auto& newTerminalArgs{ contentArgs.try_as<NewTerminalArgs>() };
        if (contentArgs == nullptr || newTerminalArgs != nullptr || contentArgs.Type().empty())
        {
            // Terminals are of course special, and have to deal with debug taps, duplicating the tab, etc.
            return _MakeTerminalPane(newTerminalArgs, sourceTab, existingConnection);
        }

        IPaneContent content{ nullptr };

        const auto& paneType{ contentArgs.Type() };
        if (paneType == L"scratchpad")
        {
            const auto& scratchPane{ winrt::make_self<ScratchpadContent>() };

            // This is maybe a little wacky - add our key event handler to the pane
            // we made. So that we can get actions for keys that the content didn't
            // handle.
            scratchPane->GetRoot().KeyDown({ get_weak(), &TerminalPage::_KeyDownHandler });

            content = *scratchPane;
        }
        else if (paneType == L"agentManager")
        {
            // Agentmaster: content for the pinned, leftmost Manager tab (C1 "Linked Lenses").
            const auto& managerPane{ winrt::make_self<AgentManagerContent>() };
            managerPane->GetRoot().KeyDown({ get_weak(), &TerminalPage::_KeyDownHandler });
            _WireAgentManagerContent(managerPane);
            content = *managerPane;
        }
        else if (paneType == L"settings")
        {
            content = _makeSettingsContent();
        }
        else if (paneType == L"snippets")
        {
            // Prevent the user from opening a bunch of snippets panes.
            //
            // Look at the focused tab, and if it already has one, then just focus it.
            if (const auto& focusedTab{ _GetFocusedTabImpl() })
            {
                const auto rootPane{ focusedTab->GetRootPane() };
                const bool found = rootPane == nullptr ? false : rootPane->WalkTree([](const auto& p) -> bool {
                    if (const auto& snippets{ p->GetContent().try_as<SnippetsPaneContent>() })
                    {
                        snippets->Focus(FocusState::Programmatic);
                        return true;
                    }
                    return false;
                });
                // Bail out if we already found one.
                if (found)
                {
                    return nullptr;
                }
            }

            const auto& tasksContent{ winrt::make_self<SnippetsPaneContent>() };
            tasksContent->UpdateSettings(_settings);
            tasksContent->GetRoot().KeyDown({ this, &TerminalPage::_KeyDownHandler });
            tasksContent->DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
            if (const auto& termControl{ _GetActiveControl() })
            {
                tasksContent->SetLastActiveControl(termControl);
            }

            content = *tasksContent;
        }
        else if (paneType == L"x-markdown")
        {
            if (Feature_MarkdownPane::IsEnabled())
            {
                const auto& markdownContent{ winrt::make_self<MarkdownPaneContent>(L"") };
                markdownContent->UpdateSettings(_settings);
                markdownContent->GetRoot().KeyDown({ this, &TerminalPage::_KeyDownHandler });

                // This one doesn't use DispatchCommand, because we don't create
                // Command's freely at runtime like we do with just plain old actions.
                markdownContent->DispatchActionRequested([weak = get_weak()](const auto& sender, const auto& actionAndArgs) {
                    if (const auto& page{ weak.get() })
                    {
                        page->_actionDispatch->DoAction(sender, actionAndArgs);
                    }
                });
                if (const auto& termControl{ _GetActiveControl() })
                {
                    markdownContent->SetLastActiveControl(termControl);
                }

                content = *markdownContent;
            }
        }

        assert(content);

        return std::make_shared<Pane>(content);
    }

    void TerminalPage::_restartPaneConnection(
        const TerminalApp::TerminalPaneContent& paneContent,
        const winrt::Windows::Foundation::IInspectable&)
    {
        // Note: callers are likely passing in `nullptr` as the args here, as
        // the TermControl.RestartTerminalRequested event doesn't actually pass
        // any args upwards itself. If we ever change this, make sure you check
        // for nulls
        if (const auto& connection{ _duplicateConnectionForRestart(paneContent) })
        {
            // Reset the terminal's VT state before attaching the new connection.
            // The previous client may have left dirty modes (e.g., bracketed
            // paste, mouse tracking, alternate buffer, kitty keyboard) that
            // would corrupt input/output for the new shell process.
            const auto& termControl = paneContent.GetTermControl();
            termControl.HardResetWithoutErase();
            termControl.Connection(connection);
            connection.Start();
        }
    }

    // Method Description:
    // - Sets background image and applies its settings (stretch, opacity and alignment)
    // - Checks path validity
    // Arguments:
    // - newAppearance
    // Return Value:
    // - <none>
    void TerminalPage::_SetBackgroundImage(const winrt::Microsoft::Terminal::Settings::Model::IAppearanceConfig& newAppearance)
    {
        if (!_settings.GlobalSettings().UseBackgroundImageForWindow())
        {
            _tabContent.Background(nullptr);
            return;
        }

        const auto path = newAppearance.BackgroundImagePath().Resolved();
        if (path.empty())
        {
            _tabContent.Background(nullptr);
            return;
        }

        Windows::Foundation::Uri imageUri{ nullptr };
        try
        {
            imageUri = Windows::Foundation::Uri{ path };
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _tabContent.Background(nullptr);
            return;
        }
        // Check if the image brush is already pointing to the image
        // in the modified settings; if it isn't (or isn't there),
        // set a new image source for the brush

        auto brush = _tabContent.Background().try_as<Media::ImageBrush>();
        Media::Imaging::BitmapImage imageSource = brush == nullptr ? nullptr : brush.ImageSource().try_as<Media::Imaging::BitmapImage>();

        if (imageSource == nullptr ||
            imageSource.UriSource() == nullptr ||
            !imageSource.UriSource().Equals(imageUri))
        {
            Media::ImageBrush b{};
            // Note that BitmapImage handles the image load asynchronously,
            // which is especially important since the image
            // may well be both large and somewhere out on the
            // internet.
            Media::Imaging::BitmapImage image(imageUri);
            b.ImageSource(image);
            _tabContent.Background(b);
        }

        // Pull this into a separate block. If the image didn't change, but the
        // properties of the image did, we should still update them.
        if (const auto newBrush{ _tabContent.Background().try_as<Media::ImageBrush>() })
        {
            newBrush.Stretch(newAppearance.BackgroundImageStretchMode());
            newBrush.Opacity(newAppearance.BackgroundImageOpacity());
        }
    }

    // Method Description:
    // - Hook up keybindings, and refresh the UI of the terminal.
    //   This includes update the settings of all the tabs according
    //   to their profiles, update the title and icon of each tab, and
    //   finally create the tab flyout
    void TerminalPage::_RefreshUIForSettingsReload()
    {
        // Re-wire the keybindings to their handlers, as we'll have created a
        // new AppKeyBindings object.
        _HookupKeyBindings(_settings.ActionMap());

        // Refresh UI elements

        // Recreate the TerminalSettings cache here. We'll use that as we're
        // updating terminal panes, so that we don't have to build a _new_
        // TerminalSettings for every profile we update - we can just look them
        // up the previous ones we built.
        _terminalSettingsCache->Reset(_settings);

        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                // Let the tab know that there are new settings. It's up to each content to decide what to do with them.
                tabImpl->UpdateSettings(_settings);

                // Update the icon of the tab for the currently focused profile in that tab.
                // Only do this for TerminalTabs. Other types of tabs won't have multiple panes
                // and profiles so the Title and Icon will be set once and only once on init.
                _UpdateTabIcon(*tabImpl);

                // Force the TerminalTab to re-grab its currently active control's title.
                tabImpl->UpdateTitle();
            }

            auto tabImpl{ winrt::get_self<Tab>(tab) };
            tabImpl->SetActionMap(_settings.ActionMap());
        }

        if (const auto focusedTab{ _GetFocusedTabImpl() })
        {
            if (const auto profile{ focusedTab->GetFocusedProfile() })
            {
                _SetBackgroundImage(profile.DefaultAppearance());
            }
        }

        // repopulate the new tab button's flyout with entries for each
        // profile, which might have changed
        _UpdateTabWidthMode();
        _CreateNewTabFlyout();

        // Reload the current value of alwaysOnTop from the settings file. This
        // will let the user hot-reload this setting, but any runtime changes to
        // the alwaysOnTop setting will be lost.
        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();
        AlwaysOnTopChanged.raise(*this, nullptr);

        _showTabsFullscreen = _settings.GlobalSettings().ShowTabsFullscreen();

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        _tabRow.ShowElevationShield(IsRunningElevated() && _settings.GlobalSettings().ShowAdminShield());

        Media::SolidColorBrush transparent{ Windows::UI::Colors::Transparent() };
        _tabView.Background(transparent);

        ////////////////////////////////////////////////////////////////////////
        // Begin Theme handling
        _updateThemeColors();

        _updateAllTabCloseButtons();

        // The user may have changed the "show title in titlebar" setting.
        TitleChanged.raise(*this, nullptr);
    }

    void TerminalPage::_updateAllTabCloseButtons()
    {
        // Update the state of the CloseButtonOverlayMode property of
        // our TabView, to match the tab.showCloseButton property in the theme.
        //
        // Also update every tab's individual IsClosable to match the same property.
        const auto theme = _settings.GlobalSettings().CurrentTheme();
        const auto visibility = (theme && theme.Tab()) ?
                                    theme.Tab().ShowCloseButton() :
                                    Settings::Model::TabCloseButtonVisibility::Always;

        _tabItemMiddleClickHookEnabled = visibility == Settings::Model::TabCloseButtonVisibility::Never;

        for (const auto& tab : _tabs)
        {
            // Agentmaster: the pinned Manager tab is permanently non-closable — never let the
            // theme's global close-button setting re-enable its X (this loop is what was
            // clobbering the Never set in _OpenAgentManagerTab on every theme/settings apply).
            if (_managerTab && tab == _managerTab)
            {
                continue;
            }
            tab.CloseButtonVisibility(visibility);
        }

        switch (visibility)
        {
        case Settings::Model::TabCloseButtonVisibility::Never:
            _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Auto);
            break;
        case Settings::Model::TabCloseButtonVisibility::Hover:
            _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::OnPointerOver);
            break;
        case Settings::Model::TabCloseButtonVisibility::ActiveOnly:
        default:
            _tabView.CloseButtonOverlayMode(MUX::Controls::TabViewCloseButtonOverlayMode::Always);
            break;
        }
    }

    // Method Description:
    // - Sets the initial actions to process on startup. We'll make a copy of
    //   this list, and process these actions when we're loaded.
    // - This function will have no effective result after Create() is called.
    // Arguments:
    // - actions: a list of Actions to process on startup.
    // Return Value:
    // - <none>
    void TerminalPage::SetStartupActions(std::vector<ActionAndArgs> actions)
    {
        _startupActions = std::move(actions);
    }

    void TerminalPage::SetStartupConnection(ITerminalConnection connection)
    {
        _startupConnection = std::move(connection);
    }

    // Agentmaster (M10 Increment 3): TerminalWindow hands us the record id it resolved from the
    // Emperor's -s <idx> (multi-window reopen), so _InitAgentmasterEngine claims THAT record (its
    // geometry already applied by TerminalWindow) instead of the front one. Must be set before
    // _OnFirstLayout. Empty => single-window (claim the front record).
    void TerminalPage::SetAgentmasterWindowId(winrt::hstring windowId)
    {
        _assignedWindowId = windowId;
    }

    winrt::TerminalApp::IDialogPresenter TerminalPage::DialogPresenter() const
    {
        return _dialogPresenter.get();
    }

    void TerminalPage::DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter)
    {
        _dialogPresenter = dialogPresenter;
    }

    // Method Description:
    // - Get the combined taskbar state for the page. This is the combination of
    //   all the states of all the tabs, which are themselves a combination of
    //   all their panes. Taskbar states are given a priority based on the rules
    //   in:
    //   https://docs.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist3-setprogressstate
    //   under "How the Taskbar Button Chooses the Progress Indicator for a Group"
    // Arguments:
    // - <none>
    // Return Value:
    // - A TaskbarState object representing the combined taskbar state and
    //   progress percentage of all our tabs.
    winrt::TerminalApp::TaskbarState TerminalPage::TaskbarState() const
    {
        auto state{ winrt::make<winrt::TerminalApp::implementation::TaskbarState>() };

        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                auto tabState{ tabImpl->GetCombinedTaskbarState() };
                // lowest priority wins
                if (tabState.Priority() < state.Priority())
                {
                    state = tabState;
                }
            }
        }

        return state;
    }

    // Method Description:
    // - This is the method that App will call when the titlebar
    //   has been clicked. It dismisses any open flyouts.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::TitlebarClicked()
    {
        if (_newTabButton && _newTabButton.Flyout())
        {
            _newTabButton.Flyout().Hide();
        }
        _DismissTabContextMenus();
    }

    // Method Description:
    // - Notifies all attached console controls that the visibility of the
    //   hosting window has changed. The underlying PTYs may need to know this
    //   for the proper response to `::GetConsoleWindow()` from a Win32 console app.
    // Arguments:
    // - showOrHide: Show is true; hide is false.
    // Return Value:
    // - <none>
    void TerminalPage::WindowVisibilityChanged(const bool showOrHide)
    {
        _visible = showOrHide;
        for (const auto& tab : _tabs)
        {
            if (auto tabImpl{ _GetTabImpl(tab) })
            {
                // Manually enumerate the panes in each tab; this will let us recycle TerminalSettings
                // objects but only have to iterate one time.
                tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                    if (auto control = pane->GetTerminalControl())
                    {
                        control.WindowVisibilityChanged(showOrHide);
                    }
                });
            }
        }
    }

    // Method Description:
    // - Called when the user tries to do a search using keybindings.
    //   This will tell the active terminal control of the passed tab
    //   to create a search box and enable find process.
    // Arguments:
    // - tab: the tab where the search box should be created
    // Return Value:
    // - <none>
    void TerminalPage::_Find(const Tab& tab)
    {
        if (const auto& control{ tab.GetActiveTerminalControl() })
        {
            control.CreateSearchBoxControl();
        }
    }

    // Method Description:
    // - Toggles borderless mode. Hides the tab row, and raises our
    //   FocusModeChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFocusMode()
    {
        SetFocusMode(!_isInFocusMode);
    }

    void TerminalPage::SetFocusMode(const bool inFocusMode)
    {
        const auto newInFocusMode = inFocusMode;
        if (newInFocusMode != FocusMode())
        {
            _isInFocusMode = newInFocusMode;
            _UpdateTabView();
            FocusModeChanged.raise(*this, nullptr);
        }
    }

    // Method Description:
    // - Toggles fullscreen mode. Hides the tab row, and raises our
    //   FullscreenChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFullscreen()
    {
        SetFullscreen(!_isFullscreen);
    }

    // Method Description:
    // - Toggles always on top mode. Raises our AlwaysOnTopChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleAlwaysOnTop()
    {
        _isAlwaysOnTop = !_isAlwaysOnTop;
        AlwaysOnTopChanged.raise(*this, nullptr);
    }

    // Method Description:
    // - Sets the tab split button color when a new tab color is selected
    // Arguments:
    // - color: The color of the newly selected tab, used to properly calculate
    //          the foreground color of the split button (to match the font
    //          color of the tab)
    // - accentColor: the actual color we are going to use to paint the tab row and
    //                split button, so that there is some contrast between the tab
    //                and the non-client are behind it
    // Return Value:
    // - <none>
    void TerminalPage::_SetNewTabButtonColor(const til::color color, const til::color accentColor)
    {
        constexpr auto lightnessThreshold = 0.6f;
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        const auto isBrightColor = ColorFix::GetLightness(color) >= lightnessThreshold;
        const auto isLightAccentColor = ColorFix::GetLightness(accentColor) >= lightnessThreshold;
        const auto hoverColorAdjustment = isLightAccentColor ? -0.05f : 0.05f;
        const auto pressedColorAdjustment = isLightAccentColor ? -0.1f : 0.1f;

        const auto foregroundColor = isBrightColor ? Colors::Black() : Colors::White();
        const auto hoverColor = til::color{ ColorFix::AdjustLightness(accentColor, hoverColorAdjustment) };
        const auto pressedColor = til::color{ ColorFix::AdjustLightness(accentColor, pressedColorAdjustment) };

        Media::SolidColorBrush backgroundBrush{ accentColor };
        Media::SolidColorBrush backgroundHoverBrush{ hoverColor };
        Media::SolidColorBrush backgroundPressedBrush{ pressedColor };
        Media::SolidColorBrush foregroundBrush{ foregroundColor };

        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackground"), backgroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPointerOver"), backgroundHoverBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPressed"), backgroundPressedBrush);

        // Load bearing: The SplitButton uses SplitButtonForegroundSecondary for
        // the secondary button, but {TemplateBinding Foreground} for the
        // primary button.
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForeground"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPointerOver"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPressed"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundSecondary"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundSecondaryPressed"), foregroundBrush);

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);

        // This is just like what we do in Tab::_RefreshVisualState. We need
        // to manually toggle the visual state, so the setters in the visual
        // state group will re-apply, and set our currently selected colors in
        // the resources.
        VisualStateManager::GoToState(_newTabButton, L"FlyoutOpen", true);
        VisualStateManager::GoToState(_newTabButton, L"Normal", true);
    }

    // Method Description:
    // - Clears the tab split button color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // - Clears the tab row color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ClearNewTabButtonColor()
    {
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        winrt::hstring keys[] = {
            L"SplitButtonBackground",
            L"SplitButtonBackgroundPointerOver",
            L"SplitButtonBackgroundPressed",
            L"SplitButtonForeground",
            L"SplitButtonForegroundSecondary",
            L"SplitButtonForegroundPointerOver",
            L"SplitButtonForegroundPressed",
            L"SplitButtonForegroundSecondaryPressed"
        };

        // simply clear any of the colors in the split button's dict
        for (auto keyString : keys)
        {
            auto key = winrt::box_value(keyString);
            if (_newTabButton.Resources().HasKey(key))
            {
                _newTabButton.Resources().Remove(key);
            }
        }

        const auto res = Application::Current().Resources();

        const auto defaultBackgroundKey = winrt::box_value(L"TabViewItemHeaderBackground");
        const auto defaultForegroundKey = winrt::box_value(L"SystemControlForegroundBaseHighBrush");
        winrt::Windows::UI::Xaml::Media::SolidColorBrush backgroundBrush;
        winrt::Windows::UI::Xaml::Media::SolidColorBrush foregroundBrush;

        // TODO: Related to GH#3917 - I think if the system is set to "Dark"
        // theme, but the app is set to light theme, then this lookup still
        // returns to us the dark theme brushes. There's gotta be a way to get
        // the right brushes...
        // See also GH#5741
        if (res.HasKey(defaultBackgroundKey))
        {
            auto obj = res.Lookup(defaultBackgroundKey);
            backgroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            backgroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::Black() };
        }

        if (res.HasKey(defaultForegroundKey))
        {
            auto obj = res.Lookup(defaultForegroundKey);
            foregroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            foregroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::White() };
        }

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);
    }

    // Function Description:
    // - This is a helper method to get the commandline out of a
    //   ExecuteCommandline action, break it into subcommands, and attempt to
    //   parse it into actions. This is used by _HandleExecuteCommandline for
    //   processing commandlines in the current WT window.
    // Arguments:
    // - args: the ExecuteCommandlineArgs to synthesize a list of startup actions for.
    // Return Value:
    // - an empty list if we failed to parse; otherwise, a list of actions to execute.
    std::vector<ActionAndArgs> TerminalPage::ConvertExecuteCommandlineToActions(const ExecuteCommandlineArgs& args)
    {
        ::TerminalApp::AppCommandlineArgs appArgs;
        if (appArgs.ParseArgs(args) == 0)
        {
            return appArgs.GetStartupActions();
        }

        return {};
    }

    void TerminalPage::_FocusActiveControl(IInspectable /*sender*/,
                                           IInspectable /*eventArgs*/)
    {
        _FocusCurrentTab(false);
    }

    bool TerminalPage::FocusMode() const
    {
        return _isInFocusMode;
    }

    bool TerminalPage::Fullscreen() const
    {
        return _isFullscreen;
    }

    // Method Description:
    // - Returns true if we're currently in "Always on top" mode. When we're in
    //   always on top mode, the window should be on top of all other windows.
    //   If multiple windows are all "always on top", they'll maintain their own
    //   z-order, with all the windows on top of all other non-topmost windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if we should be in "always on top" mode
    bool TerminalPage::AlwaysOnTop() const
    {
        return _isAlwaysOnTop;
    }

    // Method Description:
    // - Returns true if the tab row should be visible when we're in full screen
    //   state.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if the tab row should be visible in full screen state
    bool TerminalPage::ShowTabsFullscreen() const
    {
        return _showTabsFullscreen;
    }

    // Method Description:
    // - Updates the visibility of the tab row when in fullscreen state.
    void TerminalPage::SetShowTabsFullscreen(bool newShowTabsFullscreen)
    {
        if (_showTabsFullscreen == newShowTabsFullscreen)
        {
            return;
        }

        _showTabsFullscreen = newShowTabsFullscreen;

        // if we're currently in fullscreen, update tab view to make
        // sure tabs are given the correct visibility
        if (_isFullscreen)
        {
            _UpdateTabView();
        }
    }

    void TerminalPage::SetFullscreen(bool newFullscreen)
    {
        if (_isFullscreen == newFullscreen)
        {
            return;
        }
        _isFullscreen = newFullscreen;
        _UpdateTabView();
        FullscreenChanged.raise(*this, nullptr);
    }

    // Method Description:
    // - Updates the page's state for isMaximized when the window changes externally.
    void TerminalPage::Maximized(bool newMaximized)
    {
        _isMaximized = newMaximized;
    }

    // Method Description:
    // - Asks the window to change its maximized state.
    void TerminalPage::RequestSetMaximized(bool newMaximized)
    {
        if (_isMaximized == newMaximized)
        {
            return;
        }
        _isMaximized = newMaximized;
        ChangeMaximizeRequested.raise(*this, nullptr);
    }

    TerminalApp::IPaneContent TerminalPage::_makeSettingsContent()
    {
        if (auto app{ winrt::Windows::UI::Xaml::Application::Current().try_as<winrt::TerminalApp::App>() })
        {
            if (auto appPrivate{ winrt::get_self<implementation::App>(app) })
            {
                // Lazily load the Settings UI components so that we don't do it on startup.
                appPrivate->PrepareForSettingsUI();
            }
        }

        // Create the SUI pane content
        auto settingsContent{ winrt::make_self<SettingsPaneContent>(_settings) };
        auto sui = settingsContent->SettingsUI();

        if (_hostingHwnd)
        {
            sui.SetHostingWindow(reinterpret_cast<uint64_t>(*_hostingHwnd));
        }

        // GH#8767 - let unhandled keys in the SUI try to run commands too.
        sui.KeyDown({ get_weak(), &TerminalPage::_KeyDownHandler });

        sui.OpenJson([weakThis{ get_weak() }](auto&& /*s*/, winrt::Microsoft::Terminal::Settings::Model::SettingsTarget e) {
            if (auto page{ weakThis.get() })
            {
                page->_LaunchSettings(e);
            }
        });

        sui.ShowLoadWarningsDialog([weakThis{ get_weak() }](auto&& /*s*/, const Windows::Foundation::Collections::IVectorView<winrt::Microsoft::Terminal::Settings::Model::SettingsLoadWarnings>& warnings) {
            if (auto page{ weakThis.get() })
            {
                page->ShowLoadWarningsDialog.raise(*page, warnings);
            }
        });

        return *settingsContent;
    }

    // Method Description:
    // - Creates a settings UI tab and focuses it. If there's already a settings UI tab open,
    //   just focus the existing one.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::OpenSettingsUI()
    {
        // If we're holding the settings tab's switch command, don't create a new one, switch to the existing one.
        if (!_settingsTab)
        {
            // Create the tab
            auto resultPane = std::make_shared<Pane>(_makeSettingsContent());
            _settingsTab = _CreateNewTabFromPane(resultPane);
        }
        else
        {
            _tabView.SelectedItem(_settingsTab.TabViewItem());
        }
    }

    // Method Description:
    // - Returns a com_ptr to the implementation type of the given tab if it's a Tab.
    //   If the tab is not a TerminalTab, returns nullptr.
    // Arguments:
    // - tab: the projected type of a Tab
    // Return Value:
    // - If the tab is a TerminalTab, a com_ptr to the implementation type.
    //   If the tab is not a TerminalTab, nullptr
    winrt::com_ptr<Tab> TerminalPage::_GetTabImpl(const TerminalApp::Tab& tab)
    {
        winrt::com_ptr<Tab> tabImpl;
        tabImpl.copy_from(winrt::get_self<Tab>(tab));
        return tabImpl;
    }

    // Method Description:
    // - Computes the delta for scrolling the tab's viewport.
    // Arguments:
    // - scrollDirection - direction (up / down) to scroll
    // - rowsToScroll - the number of rows to scroll
    // Return Value:
    // - delta - Signed delta, where a negative value means scrolling up.
    int TerminalPage::_ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll)
    {
        return scrollDirection == ScrollUp ? -1 * rowsToScroll : rowsToScroll;
    }

    // Method Description:
    // - Reads system settings for scrolling (based on the step of the mouse scroll).
    // Upon failure fallbacks to default.
    // Return Value:
    // - The number of rows to scroll or a magic value of WHEEL_PAGESCROLL
    // indicating that we need to scroll an entire view height
    uint32_t TerminalPage::_ReadSystemRowsToScroll()
    {
        uint32_t systemRowsToScroll;
        if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &systemRowsToScroll, 0))
        {
            LOG_LAST_ERROR();

            // If SystemParametersInfoW fails, which it shouldn't, fall back to
            // Windows' default value.
            return DefaultRowsToScroll;
        }

        return systemRowsToScroll;
    }

    // Method Description:
    // - Displays a dialog stating the "Touch Keyboard and Handwriting Panel
    //   Service" is disabled.
    void TerminalPage::ShowKeyboardServiceWarning() const
    {
        if (!_IsMessageDismissed(InfoBarMessage::KeyboardServiceWarning))
        {
            if (const auto keyboardServiceWarningInfoBar = FindName(L"KeyboardServiceWarningInfoBar").try_as<MUX::Controls::InfoBar>())
            {
                keyboardServiceWarningInfoBar.IsOpen(true);
            }
        }
    }

    // Function Description:
    // - Helper function to get the OS-localized name for the "Touch Keyboard
    //   and Handwriting Panel Service". If we can't open up the service for any
    //   reason, then we'll just return the service's key, "TabletInputService".
    // Return Value:
    // - The OS-localized name for the TabletInputService
    winrt::hstring _getTabletServiceName()
    {
        wil::unique_schandle hManager{ OpenSCManagerW(nullptr, nullptr, 0) };

        if (LOG_LAST_ERROR_IF(!hManager.is_valid()))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        DWORD cchBuffer = 0;
        const auto ok = GetServiceDisplayNameW(hManager.get(), TabletInputServiceKey.data(), nullptr, &cchBuffer);

        // Windows 11 doesn't have a TabletInputService.
        // (It was renamed to TextInputManagementService, because people kept thinking that a
        // service called "tablet-something" is system-irrelevant on PCs and can be disabled.)
        if (ok || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        std::wstring buffer;
        cchBuffer += 1; // Add space for a null
        buffer.resize(cchBuffer);

        if (LOG_LAST_ERROR_IF(!GetServiceDisplayNameW(hManager.get(),
                                                      TabletInputServiceKey.data(),
                                                      buffer.data(),
                                                      &cchBuffer)))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }
        return winrt::hstring{ buffer };
    }

    // Method Description:
    // - Return the fully-formed warning message for the
    //   "KeyboardServiceDisabled" InfoBar. This InfoBar is used to warn the user
    //   if the keyboard service is disabled, and uses the OS localization for
    //   the service's actual name. It's bound to the bar in XAML.
    // Return Value:
    // - The warning message, including the OS-localized service name.
    winrt::hstring TerminalPage::KeyboardServiceDisabledText()
    {
        const auto serviceName{ _getTabletServiceName() };
        const auto text{ RS_fmt(L"KeyboardServiceWarningText", serviceName) };
        return winrt::hstring{ text };
    }

    // Method Description:
    // - Update the RequestedTheme of the specified FrameworkElement and all its
    //   Parent elements. We need to do this so that we can actually theme all
    //   of the elements of the TeachingTip. See GH#9717
    // Arguments:
    // - element: The TeachingTip to set the theme on.
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateTeachingTipTheme(winrt::Windows::UI::Xaml::FrameworkElement element)
    {
        auto theme{ _settings.GlobalSettings().CurrentTheme() };
        auto requestedTheme{ theme.RequestedTheme() };
        while (element)
        {
            element.RequestedTheme(requestedTheme);
            element = element.Parent().try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
        }
    }

    // Method Description:
    // - Display the name and ID of this window in a TeachingTip. If the window
    //   has no name, the name will be presented as "<unnamed-window>".
    // - This can be invoked by either:
    //   * An identifyWindow action, that displays the info only for the current
    //     window
    //   * An identifyWindows action, that displays the info for all windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::IdentifyWindow()
    {
        // If we haven't ever loaded the TeachingTip, then do so now and
        // create the toast for it.
        if (_windowIdToast == nullptr)
        {
            if (auto tip{ FindName(L"WindowIdToast").try_as<MUX::Controls::TeachingTip>() })
            {
                _windowIdToast = std::make_shared<Toast>(tip);
                // IsLightDismissEnabled == true is bugged and poorly interacts with multi-windowing.
                // It causes the tip to be immediately dismissed when another tip is opened in another window.
                tip.IsLightDismissEnabled(false);
                // Make sure to use the weak ref when setting up this callback.
                tip.Closed({ get_weak(), &TerminalPage::_FocusActiveControl });
            }
        }
        _UpdateTeachingTipTheme(WindowIdToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

        if (_windowIdToast != nullptr)
        {
            _windowIdToast->Open();
        }
    }

    void TerminalPage::ShowTerminalWorkingDirectory()
    {
        // If we haven't ever loaded the TeachingTip, then do so now and
        // create the toast for it.
        if (_windowCwdToast == nullptr)
        {
            if (auto tip{ FindName(L"WindowCwdToast").try_as<MUX::Controls::TeachingTip>() })
            {
                _windowCwdToast = std::make_shared<Toast>(tip);
                // Make sure to use the weak ref when setting up this
                // callback.
                tip.Closed({ get_weak(), &TerminalPage::_FocusActiveControl });
            }
        }
        _UpdateTeachingTipTheme(WindowCwdToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

        if (_windowCwdToast != nullptr)
        {
            _windowCwdToast->Open();
        }
    }

    // Method Description:
    // - Called when the user hits the "Ok" button on the WindowRenamer TeachingTip.
    // - Will raise an event that will bubble up to the monarch, asking if this
    //   name is acceptable.
    //   - we'll eventually get called back in TerminalPage::WindowName(hstring).
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerActionClick(const IInspectable& /*sender*/,
                                                 const IInspectable& /*eventArgs*/)
    {
        auto newName = WindowRenamerTextBox().Text();
        _RequestWindowRename(newName);
    }

    void TerminalPage::_RequestWindowRename(const winrt::hstring& newName)
    {
        auto request = winrt::make<implementation::RenameWindowRequestedArgs>(newName);
        // The WindowRenamer is _not_ a Toast - we want it to stay open until
        // the user dismisses it.
        if (WindowRenamer())
        {
            WindowRenamer().IsOpen(false);
        }
        RenameWindowRequested.raise(*this, request);
        // We can't just use request.Successful here, because the handler might
        // (will) be handling this asynchronously, so when control returns to
        // us, this hasn't actually been handled yet. We'll get called back in
        // RenameFailed if this fails.
        //
        // Theoretically we could do a IAsyncOperation<RenameWindowResult> kind
        // of thing with co_return winrt::make<RenameWindowResult>(false).
    }

    // Method Description:
    // - Used to track if the user pressed enter with the renamer open. If we
    //   immediately focus it after hitting Enter on the command palette, then
    //   the Enter keydown will dismiss the command palette and open the
    //   renamer, and then the enter keyup will go to the renamer. So we need to
    //   make sure both a down and up go to the renamer.
    // Arguments:
    // - e: the KeyRoutedEventArgs describing the key that was released
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerKeyDown(const IInspectable& /*sender*/,
                                             const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        const auto key = e.OriginalKey();
        if (key == Windows::System::VirtualKey::Enter)
        {
            _renamerPressedEnter = true;
        }
    }

    // Method Description:
    // - Manually handle Enter and Escape for committing and dismissing a window
    //   rename. This is highly similar to the TabHeaderControl's KeyUp handler.
    // Arguments:
    // - e: the KeyRoutedEventArgs describing the key that was released
    // Return Value:
    // - <none>
    void TerminalPage::_WindowRenamerKeyUp(const IInspectable& sender,
                                           const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        const auto key = e.OriginalKey();
        if (key == Windows::System::VirtualKey::Enter && _renamerPressedEnter)
        {
            // User is done making changes, close the rename box
            _WindowRenamerActionClick(sender, nullptr);
        }
        else if (key == Windows::System::VirtualKey::Escape)
        {
            // User wants to discard the changes they made
            WindowRenamerTextBox().Text(_WindowProperties.WindowName());
            WindowRenamer().IsOpen(false);
            _renamerPressedEnter = false;
        }
    }

    // Method Description:
    // - This function stops people from duplicating the base profile, because
    //   it gets ~ ~ weird ~ ~ when they do. Remove when TODO GH#5047 is done.
    Profile TerminalPage::GetClosestProfileForDuplicationOfProfile(const Profile& profile) const noexcept
    {
        if (profile == _settings.ProfileDefaults())
        {
            return _settings.FindProfile(_settings.GlobalSettings().DefaultProfile());
        }
        return profile;
    }

    // Function Description:
    // - Helper to launch a new WT instance elevated. It'll do this by spawning
    //   a helper process, that will ask the shell to elevate the process for
    //   us. This might cause a UAC prompt. The elevation is performed on a
    //   background thread, as to not block the UI thread.
    // Arguments:
    // - newTerminalArgs: A NewTerminalArgs describing the terminal instance
    //   that should be spawned. The Profile should be filled in with the GUID
    //   of the profile we want to launch.
    // Return Value:
    // - <none>
    // Important: Don't take the param by reference, since we'll be doing work
    // on another thread.
    void TerminalPage::_OpenElevatedWT(NewTerminalArgs newTerminalArgs)
    {
        // BODGY
        //
        // We're going to construct the commandline we want, then toss it to a
        // helper process called `elevate-shim.exe` that happens to live next to
        // us. elevate-shim.exe will be the one to call ShellExecute with the
        // args that we want (to elevate the given profile).
        //
        // We can't be the one to call ShellExecute ourselves. ShellExecute
        // requires that the calling process stays alive until the child is
        // spawned. However, in the case of something like `wt -p
        // AlwaysElevateMe`, then the original WT will try to ShellExecute a new
        // wt.exe (elevated) and immediately exit, preventing ShellExecute from
        // successfully spawning the elevated WT.

        std::filesystem::path exePath = wil::GetModuleFileNameW<std::wstring>(nullptr);
        exePath.replace_filename(L"elevate-shim.exe");

        // Build the commandline to pass to wt for this set of NewTerminalArgs
        auto cmdline{
            fmt::format(FMT_COMPILE(L"new-tab {}"), newTerminalArgs.ToCommandline())
        };

        wil::unique_process_information pi;
        STARTUPINFOW si{};
        si.cb = sizeof(si);

        LOG_IF_WIN32_BOOL_FALSE(CreateProcessW(exePath.c_str(),
                                               cmdline.data(),
                                               nullptr,
                                               nullptr,
                                               FALSE,
                                               0,
                                               nullptr,
                                               nullptr,
                                               &si,
                                               &pi));

        // TODO: GH#8592 - It may be useful to pop a Toast here in the original
        // Terminal window informing the user that the tab was opened in a new
        // window.
    }

    // Method Description:
    // - If the requested settings want us to elevate this new terminal
    //   instance, and we're not currently elevated, then open the new terminal
    //   as an elevated instance (using _OpenElevatedWT). Does nothing if we're
    //   already elevated, or if the control settings don't want to be elevated.
    // Arguments:
    // - newTerminalArgs: The NewTerminalArgs for this terminal instance
    // - controlSettings: The constructed TerminalSettingsCreateResult for this Terminal instance
    // - profile: The Profile we're using to launch this Terminal instance
    // Return Value:
    // - true iff we tossed this request to an elevated window. Callers can use
    //   this result to early-return if needed.
    bool TerminalPage::_maybeElevate(const NewTerminalArgs& newTerminalArgs,
                                     const Settings::TerminalSettingsCreateResult& controlSettings,
                                     const Profile& profile)
    {
        // When duplicating a tab there aren't any newTerminalArgs.
        if (!newTerminalArgs)
        {
            return false;
        }

        const auto defaultSettings = controlSettings.DefaultSettings();

        // If we don't even want to elevate we can return early.
        // If we're already elevated we can also return, because it doesn't get any more elevated than that.
        if (!defaultSettings->Elevate() || IsRunningElevated())
        {
            return false;
        }

        // Manually set the Profile of the NewTerminalArgs to the guid we've
        // resolved to. If there was a profile in the NewTerminalArgs, this
        // will be that profile's GUID. If there wasn't, then we'll use
        // whatever the default profile's GUID is.
        newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(profile.Guid()));
        newTerminalArgs.StartingDirectory(_evaluatePathForCwd(defaultSettings->StartingDirectory()));
        _OpenElevatedWT(newTerminalArgs);
        return true;
    }

    // Method Description:
    // - Handles the change of connection state.
    // If the connection state is failure show information bar suggesting to configure termination behavior
    // (unless user asked not to show this message again)
    // Arguments:
    // - sender: the ICoreState instance containing the connection state
    // Return Value:
    // - <none>
    safe_void_coroutine TerminalPage::_ConnectionStateChangedHandler(const IInspectable& sender, const IInspectable& /*args*/)
    {
        if (const auto coreState{ sender.try_as<winrt::Microsoft::Terminal::Control::ICoreState>() })
        {
            const auto newConnectionState = coreState.ConnectionState();
            const auto weak = get_weak();
            co_await wil::resume_foreground(Dispatcher());
            const auto strong = weak.get();
            if (!strong)
            {
                co_return;
            }

            _adjustProcessPriorityThrottled->Run();

            if (newConnectionState == ConnectionState::Failed && !_IsMessageDismissed(InfoBarMessage::CloseOnExitInfo))
            {
                if (const auto infoBar = FindName(L"CloseOnExitInfoBar").try_as<MUX::Controls::InfoBar>())
                {
                    infoBar.IsOpen(true);
                }
            }
        }
    }

    // Method Description:
    // - Persists the user's choice not to show information bar guiding to configure termination behavior.
    // Then hides this information buffer.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_CloseOnExitInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        _DismissMessage(InfoBarMessage::CloseOnExitInfo);
        if (const auto infoBar = FindName(L"CloseOnExitInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // Method Description:
    // - Persists the user's choice not to show information bar warning about "Touch keyboard and Handwriting Panel Service" disabled
    // Then hides this information buffer.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_KeyboardServiceWarningInfoDismissHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/) const
    {
        _DismissMessage(InfoBarMessage::KeyboardServiceWarning);
        if (const auto infoBar = FindName(L"KeyboardServiceWarningInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            infoBar.IsOpen(false);
        }
    }

    // Method Description:
    // - Checks whether information bar message was dismissed earlier (in the application state)
    // Arguments:
    // - message: message to look for in the state
    // Return Value:
    // - true, if the message was dismissed
    bool TerminalPage::_IsMessageDismissed(const InfoBarMessage& message)
    {
        if (const auto dismissedMessages{ ApplicationState::SharedInstance().DismissedMessages() })
        {
            for (const auto& dismissedMessage : dismissedMessages)
            {
                if (dismissedMessage == message)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Method Description:
    // - Persists the user's choice to dismiss information bar message (in application state)
    // Arguments:
    // - message: message to dismiss
    // Return Value:
    // - <none>
    void TerminalPage::_DismissMessage(const InfoBarMessage& message)
    {
        const auto applicationState = ApplicationState::SharedInstance();
        std::vector<InfoBarMessage> messages;

        if (const auto values = applicationState.DismissedMessages())
        {
            messages.resize(values.Size());
            values.GetMany(0, messages);
        }

        if (std::none_of(messages.begin(), messages.end(), [&](const auto& m) { return m == message; }))
        {
            messages.emplace_back(message);
        }

        applicationState.DismissedMessages(std::move(messages));
    }

    void TerminalPage::_updateThemeColors()
    {
        if (_settings == nullptr)
        {
            return;
        }

        const auto theme = _settings.GlobalSettings().CurrentTheme();
        auto requestedTheme{ theme.RequestedTheme() };

        {
            _updatePaneResources(requestedTheme);

            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    // The root pane will propagate the theme change to all its children.
                    if (const auto& rootPane{ tabImpl->GetRootPane() })
                    {
                        rootPane->UpdateResources(_paneResources);
                    }
                }
            }
        }

        const auto res = Application::Current().Resources();

        // Use our helper to lookup the theme-aware version of the resource.
        const auto tabViewBackgroundKey = winrt::box_value(L"TabViewBackground");
        const auto backgroundSolidBrush = ThemeLookup(res, requestedTheme, tabViewBackgroundKey).as<Media::SolidColorBrush>();

        til::color bgColor = backgroundSolidBrush.Color();

        Media::Brush terminalBrush{ nullptr };
        if (const auto tab{ _GetFocusedTabImpl() })
        {
            if (const auto& pane{ tab->GetActivePane() })
            {
                if (const auto& lastContent{ pane->GetLastFocusedContent() })
                {
                    terminalBrush = lastContent.BackgroundBrush();
                }
            }
        }

        // GH#19604: Get the theme's tabRow color to use as the acrylic tint.
        const auto tabRowBg{ theme.TabRow() ? (_activated ? theme.TabRow().Background() :
                                                            theme.TabRow().UnfocusedBackground()) :
                                              ThemeColor{ nullptr } };

        if (_settings.GlobalSettings().UseAcrylicInTabRow() && (_activated || _settings.GlobalSettings().EnableUnfocusedAcrylic()))
        {
            if (tabRowBg)
            {
                bgColor = ThemeColor::ColorFromBrush(tabRowBg.Evaluate(res, terminalBrush, true));
            }

            const auto acrylicBrush = Media::AcrylicBrush();
            acrylicBrush.BackgroundSource(Media::AcrylicBackgroundSource::HostBackdrop);
            acrylicBrush.FallbackColor(bgColor);
            acrylicBrush.TintColor(bgColor);
            acrylicBrush.TintOpacity(0.5);

            TitlebarBrush(acrylicBrush);
        }
        else if (tabRowBg)
        {
            const auto themeBrush{ tabRowBg.Evaluate(res, terminalBrush, true) };
            bgColor = ThemeColor::ColorFromBrush(themeBrush);
            // If the tab content returned nullptr for the terminalBrush, we
            // _don't_ want to use it as the tab row background. We want to just
            // use the default tab row background.
            TitlebarBrush(themeBrush ? themeBrush : backgroundSolidBrush);
        }
        else
        {
            // Nothing was set in the theme - fall back to our original `TabViewBackground` color.
            TitlebarBrush(backgroundSolidBrush);
        }

        if (!_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            _tabRow.Background(TitlebarBrush());
        }

        // Second: Update the colors of our individual TabViewItems. This
        // applies tab.background to the tabs via Tab::ThemeColor.
        //
        // Do this second, so that we already know the bgColor of the titlebar.
        {
            const auto tabBackground = theme.Tab() ? theme.Tab().Background() : nullptr;
            const auto tabUnfocusedBackground = theme.Tab() ? theme.Tab().UnfocusedBackground() : nullptr;
            for (const auto& tab : _tabs)
            {
                winrt::com_ptr<Tab> tabImpl;
                tabImpl.copy_from(winrt::get_self<Tab>(tab));
                tabImpl->ThemeColor(tabBackground, tabUnfocusedBackground, bgColor);
            }
        }
        // Update the new tab button to have better contrast with the new color.
        // In theory, it would be convenient to also change these for the
        // inactive tabs as well, but we're leaving that as a follow up.
        _SetNewTabButtonColor(bgColor, bgColor);

        // Third: the window frame. This is basically the same logic as the tab row background.
        // We'll set our `FrameBrush` property, for the window to later use.
        const auto windowTheme{ theme.Window() };
        if (auto windowFrame{ windowTheme ? (_activated ? windowTheme.Frame() :
                                                          windowTheme.UnfocusedFrame()) :
                                            ThemeColor{ nullptr } })
        {
            const auto themeBrush{ windowFrame.Evaluate(res, terminalBrush, true) };
            FrameBrush(themeBrush);
        }
        else
        {
            // Nothing was set in the theme - fall back to null. The window will
            // use that as an indication to use the default window frame.
            FrameBrush(nullptr);
        }
    }

    // Function Description:
    // - Attempts to load some XAML resources that Panes will need. This includes:
    //   * The Color they'll use for active Panes's borders - SystemAccentColor
    //   * The Brush they'll use for inactive Panes - TabViewBackground (to match the
    //     color of the titlebar)
    // Arguments:
    // - requestedTheme: this should be the currently active Theme for the app
    // Return Value:
    // - <none>
    void TerminalPage::_updatePaneResources(const winrt::Windows::UI::Xaml::ElementTheme& requestedTheme)
    {
        const auto res = Application::Current().Resources();
        const auto accentColorKey = winrt::box_value(L"SystemAccentColor");
        if (res.HasKey(accentColorKey))
        {
            const auto colorFromResources = ThemeLookup(res, requestedTheme, accentColorKey);
            // If SystemAccentColor is _not_ a Color for some reason, use
            // Transparent as the color, so we don't do this process again on
            // the next pane (by leaving s_focusedBorderBrush nullptr)
            auto actualColor = winrt::unbox_value_or<Color>(colorFromResources, Colors::Black());
            _paneResources.focusedBorderBrush = SolidColorBrush(actualColor);
        }
        else
        {
            // DON'T use Transparent here - if it's "Transparent", then it won't
            // be able to hittest for clicks, and then clicking on the border
            // will eat focus.
            _paneResources.focusedBorderBrush = SolidColorBrush{ Colors::Black() };
        }

        const auto unfocusedBorderBrushKey = winrt::box_value(L"UnfocusedBorderBrush");
        if (res.HasKey(unfocusedBorderBrushKey))
        {
            // MAKE SURE TO USE ThemeLookup, so that we get the correct resource for
            // the requestedTheme, not just the value from the resources (which
            // might not respect the settings' requested theme)
            auto obj = ThemeLookup(res, requestedTheme, unfocusedBorderBrushKey);
            _paneResources.unfocusedBorderBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            // DON'T use Transparent here - if it's "Transparent", then it won't
            // be able to hittest for clicks, and then clicking on the border
            // will eat focus.
            _paneResources.unfocusedBorderBrush = SolidColorBrush{ Colors::Black() };
        }

        const auto broadcastColorKey = winrt::box_value(L"BroadcastPaneBorderColor");
        if (res.HasKey(broadcastColorKey))
        {
            // MAKE SURE TO USE ThemeLookup
            auto obj = ThemeLookup(res, requestedTheme, broadcastColorKey);
            _paneResources.broadcastBorderBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            // DON'T use Transparent here - if it's "Transparent", then it won't
            // be able to hittest for clicks, and then clicking on the border
            // will eat focus.
            _paneResources.broadcastBorderBrush = SolidColorBrush{ Colors::Black() };
        }
    }

    void TerminalPage::_adjustProcessPriority() const
    {
        // Windowing is single-threaded, so this will not cause a race condition.
        static uint64_t s_lastUpdateHash{ 0 };
        static bool s_supported{ true };

        if (!s_supported || !_hostingHwnd.has_value())
        {
            return;
        }

        std::array<HANDLE, 32> processes;
        auto it = processes.begin();
        const auto end = processes.end();

        auto&& appendFromControl = [&](auto&& control) {
            if (it == end)
            {
                return;
            }
            if (control)
            {
                if (const auto conn{ control.Connection() })
                {
                    if (const auto pty{ conn.try_as<winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection>() })
                    {
                        if (const uint64_t process{ pty.RootProcessHandle() }; process != 0)
                        {
                            *it++ = reinterpret_cast<HANDLE>(process);
                        }
                    }
                }
            }
        };

        auto&& appendFromTab = [&](auto&& tabImpl) {
            if (const auto pane{ tabImpl->GetRootPane() })
            {
                pane->WalkTree([&](auto&& child) {
                    if (const auto& control{ child->GetTerminalControl() })
                    {
                        appendFromControl(control);
                    }
                });
            }
        };

        if (!_activated)
        {
            // When a window is out of focus, we want to attach all of the processes
            // under it to the window so they all go into the background at the same time.
            for (auto&& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    appendFromTab(tabImpl);
                }
            }
        }
        else
        {
            // When a window is in focus, propagate our foreground boost (if we have one)
            // to current all panes in the current tab.
            if (auto tabImpl{ _GetFocusedTabImpl() })
            {
                appendFromTab(tabImpl);
            }
        }

        const auto count{ gsl::narrow_cast<DWORD>(it - processes.begin()) };
        const auto hash = til::hash((void*)processes.data(), count * sizeof(HANDLE));

        if (hash == s_lastUpdateHash)
        {
            return;
        }

        s_lastUpdateHash = hash;
        const auto hr = TerminalTrySetWindowAssociatedProcesses(_hostingHwnd.value(), count, count ? processes.data() : nullptr);

        if (S_FALSE == hr)
        {
            // Don't bother trying again or logging. The wrapper tells us it's unsupported.
            s_supported = false;
            return;
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "CalledNewQoSAPI",
            TraceLoggingValue(reinterpret_cast<uintptr_t>(_hostingHwnd.value()), "hwnd"),
            TraceLoggingValue(count),
            TraceLoggingHResult(hr));
#ifdef _DEBUG
        OutputDebugStringW(fmt::format(FMT_COMPILE(L"Submitted {} processes to TerminalTrySetWindowAssociatedProcesses; return=0x{:08x}\n"), count, hr).c_str());
#endif
    }

    void TerminalPage::WindowActivated(const bool activated)
    {
        // Stash if we're activated. Use that when we reload
        // the settings, change active panes, etc.
        _activated = activated;
        _updateThemeColors();

        _adjustProcessPriorityThrottled->Run();

        if (const auto& tab{ _GetFocusedTabImpl() })
        {
            if (tab->TabStatus().IsInputBroadcastActive())
            {
                tab->GetRootPane()->WalkTree([activated](const auto& p) {
                    if (const auto& control{ p->GetTerminalControl() })
                    {
                        control.CursorVisibility(activated ?
                                                     Microsoft::Terminal::Control::CursorDisplayState::Shown :
                                                     Microsoft::Terminal::Control::CursorDisplayState::Default);
                    }
                });
            }
        }
    }

    safe_void_coroutine TerminalPage::_ControlCompletionsChangedHandler(const IInspectable sender,
                                                                        const CompletionsChangedEventArgs args)
    {
        // This won't even get hit if the velocity flag is disabled - we gate
        // registering for the event based off of
        // Feature_ShellCompletions::IsEnabled back in _RegisterTerminalEvents

        // User must explicitly opt-in on Preview builds
        if (!_settings.GlobalSettings().EnableShellCompletionMenu())
        {
            co_return;
        }

        // Parse the json string into a collection of actions
        try
        {
            auto commandsCollection = Command::ParsePowerShellMenuComplete(args.MenuJson(),
                                                                           args.ReplacementLength());

            auto weakThis{ get_weak() };
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weakThis, commandsCollection, sender]() {
                // On the UI thread...
                if (const auto& page{ weakThis.get() })
                {
                    // Open the Suggestions UI with the commands from the control
                    page->_OpenSuggestions(sender.try_as<TermControl>(), commandsCollection, SuggestionsMode::Menu, L"");
                }
            });
        }
        CATCH_LOG();
    }

    void TerminalPage::_OpenSuggestions(
        const TermControl& sender,
        IVector<Command> commandsCollection,
        winrt::TerminalApp::SuggestionsMode mode,
        winrt::hstring filterText)

    {
        // ON THE UI THREAD
        assert(Dispatcher().HasThreadAccess());

        if (commandsCollection == nullptr)
        {
            return;
        }
        if (commandsCollection.Size() == 0)
        {
            if (const auto p = SuggestionsElement())
            {
                p.Visibility(Visibility::Collapsed);
            }
            return;
        }

        const auto& control{ sender ? sender : _GetActiveControl() };
        if (!control)
        {
            return;
        }

        const auto& sxnUi{ LoadSuggestionsUI() };

        const auto characterSize{ control.CharacterDimensions() };
        // This is in control-relative space. We'll need to convert it to page-relative space.
        const auto cursorPos{ control.CursorPositionInDips() };
        const auto controlTransform = control.TransformToVisual(this->Root());
        const auto realCursorPos{ controlTransform.TransformPoint({ cursorPos.X, cursorPos.Y }) }; // == controlTransform + cursorPos
        const Windows::Foundation::Size windowDimensions{ gsl::narrow_cast<float>(ActualWidth()), gsl::narrow_cast<float>(ActualHeight()) };

        sxnUi.Open(mode,
                   commandsCollection,
                   filterText,
                   realCursorPos,
                   windowDimensions,
                   characterSize.Height);
    }

    void TerminalPage::_PopulateContextMenu(const TermControl& control,
                                            const MUX::Controls::CommandBarFlyout& menu,
                                            const bool withSelection)
    {
        // withSelection can be used to add actions that only appear if there's
        // selected text, like "search the web"

        if (!control || !menu)
        {
            return;
        }

        // Helper lambda for dispatching an ActionAndArgs onto the
        // ShortcutActionDispatch. Used below to wire up each menu entry to the
        // respective action.

        auto weak = get_weak();
        auto makeCallback = [weak](const ActionAndArgs& actionAndArgs) {
            return [weak, actionAndArgs](auto&&, auto&&) {
                if (auto page{ weak.get() })
                {
                    page->_actionDispatch->DoAction(actionAndArgs);
                }
            };
        };

        auto makeItem = [&makeCallback](const winrt::hstring& label,
                                        const winrt::hstring& icon,
                                        const auto& action,
                                        auto& targetMenu) {
            AppBarButton button{};

            if (!icon.empty())
            {
                auto iconElement = UI::IconPathConverter::IconWUX(icon);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                button.Icon(iconElement);
            }

            button.Label(label);
            button.Click(makeCallback(action));
            targetMenu.SecondaryCommands().Append(button);
        };

        auto makeMenuItem = [](const winrt::hstring& label,
                               const winrt::hstring& icon,
                               const auto& subMenu,
                               auto& targetMenu) {
            AppBarButton button{};

            if (!icon.empty())
            {
                auto iconElement = UI::IconPathConverter::IconWUX(icon);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                button.Icon(iconElement);
            }

            button.Label(label);
            button.Flyout(subMenu);
            targetMenu.SecondaryCommands().Append(button);
        };

        auto makeContextItem = [&makeCallback](const winrt::hstring& label,
                                               const winrt::hstring& icon,
                                               const winrt::hstring& tooltip,
                                               const auto& action,
                                               const auto& subMenu,
                                               auto& targetMenu) {
            AppBarButton button{};

            if (!icon.empty())
            {
                auto iconElement = UI::IconPathConverter::IconWUX(icon);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
                button.Icon(iconElement);
            }

            button.Label(label);
            button.Click(makeCallback(action));
            WUX::Controls::ToolTipService::SetToolTip(button, box_value(tooltip));
            button.ContextFlyout(subMenu);
            targetMenu.SecondaryCommands().Append(button);
        };

        const auto focusedProfile = _GetFocusedTabImpl()->GetFocusedProfile();
        auto separatorItem = AppBarSeparator{};
        auto activeProfiles = _settings.ActiveProfiles();
        auto activeProfileCount = gsl::narrow_cast<int>(activeProfiles.Size());
        MUX::Controls::CommandBarFlyout splitPaneMenu{};

        // Wire up each item to the action that should be performed. By actually
        // connecting these to actions, we ensure the implementation is
        // consistent. This also leaves room for customizing this menu with
        // actions in the future.

        makeItem(RS_(L"DuplicateTabText"), L"\xF5ED", ActionAndArgs{ ShortcutAction::DuplicateTab, nullptr }, menu);

        const auto focusedProfileName = focusedProfile.Name();
        const auto focusedProfileIcon = focusedProfile.Icon().Resolved();
        const auto splitPaneDuplicateText = RS_(L"SplitPaneDuplicateText") + L" " + focusedProfileName; // SplitPaneDuplicateText

        const auto splitPaneRightText = RS_(L"SplitPaneRightText");
        const auto splitPaneDownText = RS_(L"SplitPaneDownText");
        const auto splitPaneUpText = RS_(L"SplitPaneUpText");
        const auto splitPaneLeftText = RS_(L"SplitPaneLeftText");
        const auto splitPaneToolTipText = RS_(L"SplitPaneToolTipText");

        MUX::Controls::CommandBarFlyout splitPaneContextMenu{};
        makeItem(splitPaneRightText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Right, .5, nullptr } }, splitPaneContextMenu);
        makeItem(splitPaneDownText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Down, .5, nullptr } }, splitPaneContextMenu);
        makeItem(splitPaneUpText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Up, .5, nullptr } }, splitPaneContextMenu);
        makeItem(splitPaneLeftText, focusedProfileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Left, .5, nullptr } }, splitPaneContextMenu);

        makeContextItem(splitPaneDuplicateText, focusedProfileIcon, splitPaneToolTipText, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate, SplitDirection::Automatic, .5, nullptr } }, splitPaneContextMenu, splitPaneMenu);

        // add menu separator
        const auto separatorAutoItem = AppBarSeparator{};

        splitPaneMenu.SecondaryCommands().Append(separatorAutoItem);

        for (auto profileIndex = 0; profileIndex < activeProfileCount; profileIndex++)
        {
            const auto profile = activeProfiles.GetAt(profileIndex);
            const auto profileName = profile.Name();
            const auto profileIcon = profile.Icon().Resolved();

            NewTerminalArgs args{};
            args.Profile(profileName);

            MUX::Controls::CommandBarFlyout splitPaneContextMenu{};
            makeItem(splitPaneRightText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Right, .5, args } }, splitPaneContextMenu);
            makeItem(splitPaneDownText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Down, .5, args } }, splitPaneContextMenu);
            makeItem(splitPaneUpText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Up, .5, args } }, splitPaneContextMenu);
            makeItem(splitPaneLeftText, profileIcon, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Left, .5, args } }, splitPaneContextMenu);

            makeContextItem(profileName, profileIcon, splitPaneToolTipText, ActionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Manual, SplitDirection::Automatic, .5, args } }, splitPaneContextMenu, splitPaneMenu);
        }

        makeMenuItem(RS_(L"SplitPaneText"), L"\xF246", splitPaneMenu, menu);

        // Only wire up "Close Pane" if there's multiple panes.
        if (_GetFocusedTabImpl()->GetLeafPaneCount() > 1)
        {
            MUX::Controls::CommandBarFlyout swapPaneMenu{};
            const auto rootPane = _GetFocusedTabImpl()->GetRootPane();
            const auto mruPanes = _GetFocusedTabImpl()->GetMruPanes();
            auto activePane = _GetFocusedTabImpl()->GetActivePane();
            rootPane->WalkTree([&](auto p) {
                if (const auto& c{ p->GetTerminalControl() })
                {
                    if (c == control)
                    {
                        activePane = p;
                    }
                }
            });

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Down, mruPanes))
            {
                makeItem(RS_(L"SwapPaneDownText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Down } }, swapPaneMenu);
            }

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Right, mruPanes))
            {
                makeItem(RS_(L"SwapPaneRightText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Right } }, swapPaneMenu);
            }

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Up, mruPanes))
            {
                makeItem(RS_(L"SwapPaneUpText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Up } }, swapPaneMenu);
            }

            if (auto neighbor = rootPane->NavigateDirection(activePane, FocusDirection::Left, mruPanes))
            {
                makeItem(RS_(L"SwapPaneLeftText"), neighbor->GetProfile().Icon().Resolved(), ActionAndArgs{ ShortcutAction::SwapPane, SwapPaneArgs{ FocusDirection::Left } }, swapPaneMenu);
            }

            makeMenuItem(RS_(L"SwapPaneText"), L"\xF1CB", swapPaneMenu, menu);

            makeItem(RS_(L"TogglePaneZoomText"), L"\xE8A3", ActionAndArgs{ ShortcutAction::TogglePaneZoom, nullptr }, menu);
            makeItem(RS_(L"CloseOtherPanesText"), L"\xE89F", ActionAndArgs{ ShortcutAction::CloseOtherPanes, nullptr }, menu);
            makeItem(RS_(L"PaneClose"), L"\xE89F", ActionAndArgs{ ShortcutAction::ClosePane, nullptr }, menu);
        }

        if (control.ConnectionState() >= ConnectionState::Closed)
        {
            makeItem(RS_(L"RestartConnectionText"), L"\xE72C", ActionAndArgs{ ShortcutAction::RestartConnection, nullptr }, menu);
        }

        if (withSelection)
        {
            makeItem(RS_(L"SearchWebText"), L"\xF6FA", ActionAndArgs{ ShortcutAction::SearchForText, nullptr }, menu);
        }

        makeItem(RS_(L"TabClose"), L"\xE711", ActionAndArgs{ ShortcutAction::CloseTab, CloseTabArgs{ _GetFocusedTabIndex().value() } }, menu);
    }

    void TerminalPage::_PopulateQuickFixMenu(const TermControl& control,
                                             const Controls::MenuFlyout& menu)
    {
        if (!control || !menu)
        {
            return;
        }

        // Helper lambda for dispatching a SendInput ActionAndArgs onto the
        // ShortcutActionDispatch. Used below to wire up each menu entry to the
        // respective action. Then clear the quick fix menu.
        auto weak = get_weak();
        auto makeCallback = [weak](const hstring& suggestion) {
            return [weak, suggestion](auto&&, auto&&) {
                if (auto page{ weak.get() })
                {
                    const auto actionAndArgs = ActionAndArgs{ ShortcutAction::SendInput, SendInputArgs{ hstring{ L"\u0003" } + suggestion } };
                    page->_actionDispatch->DoAction(actionAndArgs);
                    if (auto ctrl = page->_GetActiveControl())
                    {
                        ctrl.ClearQuickFix();
                    }

                    TraceLoggingWrite(
                        g_hTerminalAppProvider,
                        "QuickFixSuggestionUsed",
                        TraceLoggingDescription("Event emitted when a winget suggestion from is used"),
                        TraceLoggingValue("QuickFixMenu", "Source"),
                        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
                }
            };
        };

        // Wire up each item to the action that should be performed. By actually
        // connecting these to actions, we ensure the implementation is
        // consistent. This also leaves room for customizing this menu with
        // actions in the future.

        menu.Items().Clear();
        const auto quickFixes = control.CommandHistory().QuickFixes();
        for (const auto& qf : quickFixes)
        {
            MenuFlyoutItem item{};

            auto iconElement = UI::IconPathConverter::IconWUX(L"\ue74c");
            Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
            item.Icon(iconElement);

            item.Text(qf);
            item.Click(makeCallback(qf));
            ToolTipService::SetToolTip(item, box_value(qf));
            menu.Items().Append(item);
        }
    }

    // Handler for our WindowProperties's PropertyChanged event. We'll use this
    // to pop the "Identify Window" toast when the user renames our window.
    void TerminalPage::_windowPropertyChanged(const IInspectable& /*sender*/, const WUX::Data::PropertyChangedEventArgs& args)
    {
        if (args.PropertyName() != L"WindowName")
        {
            return;
        }

        // DON'T display the confirmation if this is the name we were
        // given on startup!
        if (_startupState == StartupState::Initialized)
        {
            IdentifyWindow();
        }
    }

    void TerminalPage::_onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView&,
                                          const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& e)
    {
        // Get the tab impl from this event.
        const auto eventTab = e.Tab();
        const auto tabBase = _GetTabByTabViewItem(eventTab);
        winrt::com_ptr<Tab> tabImpl;
        tabImpl.copy_from(winrt::get_self<Tab>(tabBase));
        if (tabImpl)
        {
            // First: stash the tab we started dragging.
            // We're going to be asked for this.
            _stashed.draggedTab = tabImpl;

            // Stash the offset from where we started the drag to the
            // tab's origin. We'll use that offset in the future to help
            // position the dropped window.
            const auto inverseScale = 1.0f / static_cast<float>(eventTab.XamlRoot().RasterizationScale());
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            ScreenToClient(*_hostingHwnd, &cursorPos);
            _stashed.dragOffset.X = cursorPos.x * inverseScale;
            _stashed.dragOffset.Y = cursorPos.y * inverseScale;

            // Into the DataPackage, let's stash our own window ID.
            const auto id{ _WindowProperties.WindowId() };

            // Get our PID
            const auto pid{ GetCurrentProcessId() };

            e.Data().Properties().Insert(L"windowId", winrt::box_value(id));
            e.Data().Properties().Insert(L"pid", winrt::box_value<uint32_t>(pid));
            e.Data().RequestedOperation(DataPackageOperation::Move);

            // The next thing that will happen:
            //  * Another TerminalPage will get a TabStripDragOver, then get a
            //    TabStripDrop
            //    * This will be handled by the _other_ page asking the monarch
            //      to ask us to send our content to them.
            //  * We'll get a TabDroppedOutside to indicate that this tab was
            //    dropped _not_ on a TabView.
            //    * This will be handled by _onTabDroppedOutside, which will
            //      raise a MoveContent (to a new window) event.
        }
    }

    void TerminalPage::_onTabStripDragOver(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                           const winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        // We must mark that we can accept the drag/drop. The system will never
        // call TabStripDrop on us if we don't indicate that we're willing.
        const auto& props{ e.DataView().Properties() };
        if (props.HasKey(L"windowId") &&
            props.HasKey(L"pid") &&
            (winrt::unbox_value_or<uint32_t>(props.TryLookup(L"pid"), 0u) == GetCurrentProcessId()))
        {
            e.AcceptedOperation(DataPackageOperation::Move);
        }

        // You may think to yourself, this is a great place to increase the
        // width of the TabView artificially, to make room for the new tab item.
        // However, we'll never get a message that the tab left the tab view
        // (without being dropped). So there's no good way to resize back down.
    }

    // Method Description:
    // - Called on the TARGET of a tab drag/drop. We'll unpack the DataPackage
    //   to find who the tab came from. We'll then ask the Monarch to ask the
    //   sender to move that tab to us.
    void TerminalPage::_onTabStripDrop(winrt::Windows::Foundation::IInspectable /*sender*/,
                                       winrt::Windows::UI::Xaml::DragEventArgs e)
    {
        // Get the PID and make sure it is the same as ours.
        if (const auto& pidObj{ e.DataView().Properties().TryLookup(L"pid") })
        {
            const auto pid{ winrt::unbox_value_or<uint32_t>(pidObj, 0u) };
            if (pid != GetCurrentProcessId())
            {
                // The PID doesn't match ours. We can't handle this drop.
                return;
            }
        }
        else
        {
            // No PID? We can't handle this drop. Bail.
            return;
        }

        const auto& windowIdObj{ e.DataView().Properties().TryLookup(L"windowId") };
        if (windowIdObj == nullptr)
        {
            // No windowId? Bail.
            return;
        }
        const uint64_t src{ winrt::unbox_value<uint64_t>(windowIdObj) };

        // Figure out where in the tab strip we're dropping this tab. Add that
        // index to the request. This is largely taken from the WinUI sample
        // app.

        // First we need to get the position in the List to drop to
        auto index = -1;

        // Determine which items in the list our pointer is between.
        for (auto i = 0u; i < _tabView.TabItems().Size(); i++)
        {
            if (const auto& item{ _tabView.ContainerFromIndex(i).try_as<winrt::MUX::Controls::TabViewItem>() })
            {
                const auto posX{ e.GetPosition(item).X }; // The point of the drop, relative to the tab
                const auto itemWidth{ item.ActualWidth() }; // The right of the tab
                // If the drag point is on the left half of the tab, then insert here.
                if (posX < itemWidth / 2)
                {
                    index = i;
                    break;
                }
            }
        }

        // `this` is safe to use
        const auto request = winrt::make_self<RequestReceiveContentArgs>(src, _WindowProperties.WindowId(), index);

        // This will go up to the monarch, who will then dispatch the request
        // back down to the source TerminalPage, who will then perform a
        // RequestMoveContent to move their tab to us.
        RequestReceiveContent.raise(*this, *request);
    }

    // Method Description:
    // - This is called on the drag/drop SOURCE TerminalPage, when the monarch has
    //   requested that we send our tab to another window. We'll need to
    //   serialize the tab, and send it to the monarch, who will then send it to
    //   the destination window.
    // - Fortunately, sending the tab is basically just a MoveTab action, so we
    //   can largely reuse that.
    void TerminalPage::SendContentToOther(winrt::TerminalApp::RequestReceiveContentArgs args)
    {
        // validate that we're the source window of the tab in this request
        if (args.SourceWindow() != _WindowProperties.WindowId())
        {
            return;
        }
        if (!_stashed.draggedTab)
        {
            return;
        }

        _sendDraggedTabToWindow(winrt::to_hstring(args.TargetWindow()), args.TabIndex(), std::nullopt);
    }

    void TerminalPage::_onTabDroppedOutside(winrt::IInspectable /*sender*/,
                                            winrt::MUX::Controls::TabViewTabDroppedOutsideEventArgs /*e*/)
    {
        // Get the current pointer point from the CoreWindow
        const auto& pointerPoint{ CoreWindow::GetForCurrentThread().PointerPosition() };

        // This is called when a tab FROM OUR WINDOW was dropped outside the
        // tabview. We already know which tab was being dragged. We'll just
        // invoke a moveTab action with the target window being -1. That will
        // force the creation of a new window.

        if (!_stashed.draggedTab)
        {
            return;
        }

        // We need to convert the pointer point to a point that we can use
        // to position the new window. We'll use the drag offset from before
        // so that the tab in the new window is positioned so that it's
        // basically still directly under the cursor.

        // -1 is the magic number for "new window"
        // 0 as the tab index, because we don't care. It's making a new window. It'll be the only tab.
        const winrt::Windows::Foundation::Point adjusted = {
            pointerPoint.X - _stashed.dragOffset.X,
            pointerPoint.Y - _stashed.dragOffset.Y,
        };
        _sendDraggedTabToWindow(winrt::hstring{ L"-1" }, 0, adjusted);
    }

    void TerminalPage::_sendDraggedTabToWindow(const winrt::hstring& windowId,
                                               const uint32_t tabIndex,
                                               std::optional<winrt::Windows::Foundation::Point> dragPoint)
    {
        auto startupActions = _stashed.draggedTab->BuildStartupActions(BuildStartupKind::Content);
        _DetachTabFromWindow(_stashed.draggedTab);

        _MoveContent(std::move(startupActions), windowId, tabIndex, dragPoint);
        // _RemoveTab will make sure to null out the _stashed.draggedTab
        _RemoveTab(*_stashed.draggedTab);
    }

    /// <summary>
    /// Creates a sub flyout menu for profile items in the split button menu that when clicked will show a menu item for
    /// Run as Administrator
    /// </summary>
    /// <param name="profileIndex">The index for the profileMenuItem</param>
    /// <returns>MenuFlyout that will show when the context is request on a profileMenuItem</returns>
    WUX::Controls::MenuFlyout TerminalPage::_CreateRunAsAdminFlyout(int profileIndex)
    {
        // Create the MenuFlyout and set its placement
        WUX::Controls::MenuFlyout profileMenuItemFlyout{};
        profileMenuItemFlyout.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);

        // Create the menu item and an icon to use in the menu
        WUX::Controls::MenuFlyoutItem runAsAdminItem{};
        WUX::Controls::FontIcon adminShieldIcon{};

        adminShieldIcon.Glyph(L"\xEA18");
        adminShieldIcon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });

        runAsAdminItem.Icon(adminShieldIcon);
        runAsAdminItem.Text(RS_(L"RunAsAdminFlyout/Text"));

        // Click handler for the flyout item
        runAsAdminItem.Click([profileIndex, weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                TraceLoggingWrite(
                    g_hTerminalAppProvider,
                    "NewTabMenuItemElevateSubmenuItemClicked",
                    TraceLoggingDescription("Event emitted when the elevate submenu item from the new tab menu is invoked"),
                    TraceLoggingValue(page->NumberOfTabs(), "TabCount", "The count of tabs currently opened in this window"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                NewTerminalArgs args{ profileIndex };
                args.Elevate(true);
                page->_OpenNewTerminalViaDropdown(args);
            }
        });

        profileMenuItemFlyout.Items().Append(runAsAdminItem);

        return profileMenuItemFlyout;
    }
}
