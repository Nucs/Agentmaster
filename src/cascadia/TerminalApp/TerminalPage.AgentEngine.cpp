// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation (7 partial files)
// The TerminalPage-side Agentmaster glue -- engine wiring + session lifecycle + observer +
// window-record + the Sessions browser -- connecting the Manager engine to WT's TerminalPage.
// Same class (TerminalPage, declared in TerminalPage.h), split across same-class partial TUs (the
// upstream TabManagement.cpp pattern) so TerminalPage.cpp stays close to upstream.
//
// Partial files in this group (★ marks THIS file):
// ★ TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Manager tab, _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster — TerminalPage's engine wiring + Manager-tab hosting (M9 / M6; see
// doc/agentmaster/DESIGN.md and IMPLEMENTATION.md): consume the ONE process-wide
// SharedEngine (registry / bridge / scheduler / scanner / observer), claim this
// window's WindowRecord, open the pinned non-closable Manager tab, wire the
// AgentManagerContent callbacks, and detach everything again in ~TerminalPage.
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // GuidToString (mint a fresh windowId)

#include "AgentManagerContent.h" // the Manager tab's content (C1 UI) — created + wired here
#include "AgentTabOverlay.h" // ~TerminalPage destroys the com_ptr<AgentTabOverlay> maps — needs the complete type
#include "AgentCatchLog.h" // exception forensics: InstallAgentExceptionTrace (VEH throw-stack ring + wil failure logger, engine init below)
#include "AgentToastActivator.h" // System notifications: the toast COM activator (registered once, process-wide, from the engine init below)
#include "TabHeaderControl.h" // Agentmaster: SetTabRenameCommitMode (push the GLOBAL rename-commit mode to tab headers)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / AgentmasterStateDir / MaterializeSharedHookFiles
#include "AgentMaster/Engine.h" // SharedEngine / ClaimWindowRecord / Register-UnregisterLiveWindow
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings
#include "AgentMaster/ProcessObserver.h" // UnpublishWindow (teardown; Rule #10)
#include "AgentMaster/ProfileBootstrap.h" // IsDevOrDebugPackage — gate the registry's adopt-default autorunner mode push like the scheduler
#include "AgentMaster/Scheduler.h" // SetGlobalPause / Confirm (Manager callbacks)
#include "AgentMaster/SessionRegistry.h" // adoption-handler + registry-observer tokens
#include "AgentMaster/SessionScanner.h" // liveness-probe token
#include "AgentMaster/TabDragMath.h" // the pointer-owned tab reorder gesture's pure decision math (bands / slots / release / auto-scroll)

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

// Agentmaster: TabHeaderControl mirrors the rename-commit mode as raw ints (it doesn't include the
// engine model). SetTabRenameCommitMode below casts the enum straight to int, so the enum's
// underlying values MUST be these — lock it here, where both the enum and that contract are visible.
static_assert(static_cast<int32_t>(::Agentmaster::TabRenameCommitMode::ClickAwayOnly) == 0 &&
                  static_cast<int32_t>(::Agentmaster::TabRenameCommitMode::ClickAwayOrShiftEnter) == 1 &&
                  static_cast<int32_t>(::Agentmaster::TabRenameCommitMode::ClickAwayOrEnter) == 2,
              "TabRenameCommitMode values must match the raw-int constants in TabHeaderControl.cpp");

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

// Agentmaster: depth-first walk for the TabView's internal horizontal ScrollViewer — the template
// part named "ScrollViewer" inside its TabViewListView (see Microsoft.UI.Xaml 2.8 Generic.xaml: the
// TabScrollViewerStyle hosts the `<`/`>` ScrollDecrease/IncreaseButtons around it). That ScrollViewer
// is what scrolls the tab strip, so its HorizontalOffset tells us when tab 0 (the pinned Manager tab)
// has scrolled off the left edge. Each TabViewItem's content is just an empty Border (Tab.cpp), so the
// strip subtree contains exactly one ScrollViewer and the first found in document order is correct.
static winrt::WUX::Controls::ScrollViewer _FindTabStripScrollViewer(const winrt::WUX::DependencyObject& root)
{
    if (!root)
    {
        return nullptr;
    }
    const auto count = winrt::WUX::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int32_t i = 0; i < count; ++i)
    {
        const auto child = winrt::WUX::Media::VisualTreeHelper::GetChild(root, i);
        if (const auto sv = child.try_as<winrt::WUX::Controls::ScrollViewer>())
        {
            if (const auto fe = child.try_as<winrt::WUX::FrameworkElement>(); fe && fe.Name() == L"ScrollViewer")
            {
                return sv;
            }
        }
        if (auto found = _FindTabStripScrollViewer(child))
        {
            return found;
        }
    }
    return nullptr;
}

// Agentmaster: flip a tab-strip nav button's visibility, but only when it actually changes — the
// updater runs on the high-frequency ViewChanged stream, so a no-op write would needlessly invalidate
// the strip's layout each tick.
static void _SetTabStripButtonVisible(const winrt::WUX::Controls::Button& btn, bool visible)
{
    if (!btn)
    {
        return;
    }
    const auto desired = visible ? winrt::WUX::Visibility::Visible : winrt::WUX::Visibility::Collapsed;
    if (btn.Visibility() != desired)
    {
        btn.Visibility(desired);
    }
}

namespace winrt::TerminalApp::implementation
{
    // Agentmaster (M9): the engine is a process singleton shared by every window. When this
    // window is torn down, drop its adoption handler from the shared registry so it doesn't
    // linger (the handler captures get_weak(), so a stray call is already a safe no-op — this
    // just keeps the registry's handler list bounded across many window open/close cycles). The
    // registry itself outlives every window (held by SharedEngine), so this call stays valid.
    TerminalPage::~TerminalPage()
    {
        // Agentmaster (quit-all window-record loss): persist this window's record BEFORE the
        // teardown-archive below clears _claudeTabs — capturing after it would degrade every Claude tab
        // to an anonymous Other ref (the hazard CloseWindow's flush note describes). This is the
        // catch-all for the NON-initiating windows on a quit-all (QuitRequested), which are torn down
        // straight through this destructor with no per-window CloseWindow/RequestQuit, so without this
        // their final geometry / tab order / focus / lens never reaches disk past the 750ms autosave.
        // Gated on the latch so it does NOT re-capture (and clobber) a good close/quit-seam flush after
        // _claudeTabs was already cleared; the geometry fallback in _CaptureWindowRecord keeps the
        // last-known position/size if the window is already too far torn down to read it live.
        // Agentmaster (terminate-net — teardown can run on a BACKGROUND thread): ~TerminalPage assumes
        // UI-thread execution (it stops DispatcherTimers + reads XAML in the flush/archive here — all
        // UI-affine), but a fire_and_forget observer/sweep coroutine can hold the last get_strong() ref
        // and, when the window closes mid-tick, its resume_foreground() can't reach the dying dispatcher
        // and completes INLINE on the thread-pool thread — so releasing that last ref runs THIS
        // destructor THERE. A UI-affine call then throws RPC_E_WRONG_THREAD, and a throw escaping a
        // destructor => std::terminate (0xC0000409 FATAL_APP_EXIT — hit live after a tab drag-drop tore a
        // window down while _ObserverProbe was mid-tick). So EVERY UI-affine step below is individually
        // guarded (CATCH_LOG), while the thread-safe engine detaches (Rule #10) stay bare so a guarded
        // throw can never skip them. On the normal UI-thread teardown nothing throws, so this is inert.
        if (!_windowRecordTeardownFlushed)
        {
            try
            {
                _FlushWindowRecord();
            }
            CATCH_LOG();
            _windowRecordTeardownFlushed = true;
        }

        // Agentmaster (lifecycle gap #1): archive any of this window's still-live sessions before we
        // detach. Normally CloseWindow already did this (deterministically, before raising
        // CloseWindowRequested); this is the catch-all for quit-all / any teardown path that bypassed
        // it. Idempotent — a no-op if CloseWindow already cleared _claudeTabs. Guarded (bg-thread net).
        try
        {
            _ArchiveWindowSessionsOnTeardown();
        }
        CATCH_LOG();

        if (_sessionRegistry && _adoptionToken)
        {
            _sessionRegistry->RemoveAdoptionHandler(_adoptionToken);
        }
        // Agentmaster (tab status dot): same Rule-#10 detach for the dot's registry observer.
        if (_sessionRegistry && _agentDotObserverToken)
        {
            _sessionRegistry->RemoveObserver(_agentDotObserverToken);
        }
        // Symmetric to the adoption handler: drop this window's liveness probe from the shared
        // scanner so a closed window's probe (it captures get_weak()) doesn't linger on the
        // process-wide scanner. The scanner outlives every window (held by SharedEngine).
        if (_scanner && _livenessToken)
        {
            _scanner->RemoveLivenessProbe(_livenessToken);
        }
        // Agentmaster (alt+up/down prompt nav): stop the 30 s focused-refresh timer. DispatcherTimer is
        // UI-thread-affine, so Stop() throws RPC_E_WRONG_THREAD on a background-thread teardown (see the
        // terminate-net note above) — guard it. Unstopped is harmless: the timer dies with the page and
        // its Tick captures get_weak().
        if (_promptNavRefreshTimer)
        {
            try
            {
                _promptNavRefreshTimer.Stop();
            }
            CATCH_LOG();
        }
        // Agentmaster (eager-init "Activate All Tabs" pacing): stop a mid-flight activate-all drip — its
        // queue dies with the page; the weak Tick would no-op anyway. Same UI-affine guard as above.
        if (_activateAllTimer)
        {
            try
            {
                _activateAllTimer.Stop();
            }
            CATCH_LOG();
        }
        // Agentmaster (cross-window activate): drop this window's activate sink from the shared
        // engine — a stray fan-out after teardown is already a safe no-op (the sink captures
        // get_weak() + an agile dispatcher), this keeps the engine's sink list bounded (Rule #10).
        if (_windowActivateToken)
        {
            ::Agentmaster::UnregisterWindowActivateHandler(_windowActivateToken);
        }
        // Agentmaster (cross-window restart): drop this window's restart sink too (Rule #10).
        if (_commandActionToken)
        {
            ::Agentmaster::UnregisterCommandActionHandler(_commandActionToken);
            _commandActionToken = 0;
        }
        if (_windowRestartToken)
        {
            ::Agentmaster::UnregisterWindowRestartHandler(_windowRestartToken);
        }
        // Agentmaster (cross-window "Close ▸ Of Same Folder"): drop this window's close-session sink too (Rule #10).
        if (_windowCloseSessionToken)
        {
            ::Agentmaster::UnregisterWindowCloseSessionHandler(_windowCloseSessionToken);
        }
        // Agentmaster (eager-init "Activate All Tabs"): drop this window's activate-all sink too (Rule #10).
        if (_windowActivateAllToken)
        {
            ::Agentmaster::UnregisterActivateAllDormantHandler(_windowActivateAllToken);
        }
        // Agentmaster (cross-window settings broadcast): drop this window's settings sink too (Rule #10).
        if (_settingsChangedToken)
        {
            ::Agentmaster::UnregisterSettingsChangedHandler(_settingsChangedToken);
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

    // Agentmaster: "#RRGGBB" -> an opaque WT tab color. Mirrors AgentSessions.cpp's ClaudeHexToColor;
    // kept TU-local like the codebase's other per-file color converters. Used to re-apply the Manager
    // tab's persisted per-window color on reopen. nullopt on anything malformed.
    static std::optional<winrt::Windows::UI::Color> _ManagerHexToColor(const std::wstring& hexIn)
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
        // Agentmaster: ...but tab-switching chords (alt+left/alt+right, ctrl+tab) must beat a focused
        // Manager box, which would eat them before the bubbling KeyDown above. Tunnel them in.
        managerPane->GetRoot().PreviewKeyDown({ this, &TerminalPage::_ManagerPaneNavPreviewKeyDown });

        // Agentmaster: wire the Manager UI to the engine (registry + spawn/activate/kill).
        _WireAgentManagerContent(managerPane);

        const auto resultPane = std::make_shared<Pane>(*managerPane);
        _managerTab = _CreateNewTabFromPane(resultPane, 0); // 0 == leftmost

        if (_managerTab)
        {
            // Non-closable: hide this tab's close button.
            _managerTab.CloseButtonVisibility(winrt::Microsoft::Terminal::Settings::Model::TabCloseButtonVisibility::Never);

            // Agentmaster: also gray out the right-click "Move tab" / "Close" / "Close tab"
            // entries so the pinned Manager tab can't be relocated or closed from the menu, and
            // disable rename (the Manager tab's title is fixed): grays the "Rename Tab" menu item
            // and blocks the double-tap / openTabRenamer-action rename gestures.
            if (const auto tabImpl{ _GetTabImpl(_managerTab) })
            {
                tabImpl->DisableCloseAndMoveMenuItems();
                tabImpl->DisableTabRename();

                // Agentmaster: re-apply the Manager tab's per-window color from the claimed window record
                // (windows/<id>.json). The Manager tab is a per-window singleton, so unlike a session tab
                // (dir-keyed, Rule #12) its color lives in the window record; a claimed record with a saved
                // color re-tints this window's home tab on reopen. SetRuntimeTabColor raises TabColorChanged,
                // but _ScheduleWindowRecordSave no-ops pre-Initialized, so this can't spuriously re-save.
                if (_windowRecordClaimed && !_windowRecord.managerTabColor.empty())
                {
                    if (const auto c = _ManagerHexToColor(_windowRecord.managerTabColor))
                    {
                        tabImpl->SetRuntimeTabColor(*c);
                    }
                }
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

    // Agentmaster: lazily bind to the TabView's internal horizontal ScrollViewer. It isn't realized
    // until the strip's template is applied (and at least one tab exists), so the first attempts
    // (from _OnFirstLayout) may find nothing — later callers (_OnTabItemsChanged, every
    // _UpdateManagerNavButtons) retry until it's there. Once bound we watch ViewChanged (the user
    // scrolled — buttons, wheel or drag) and SizeChanged (the strip resized, so overflow appeared or
    // vanished) to re-evaluate the Home button. The guard makes repeat calls cheap no-ops.
    void TerminalPage::_EnsureTabStripScrollViewer()
    {
        if (_tabStripScrollViewer || !_tabView)
        {
            return;
        }

        auto sv = _FindTabStripScrollViewer(_tabView.as<winrt::WUX::DependencyObject>());
        if (!sv)
        {
            return; // template not realized yet — a later call will retry
        }

        _tabStripScrollViewer = sv;
        _tabStripViewChangedRevoker = sv.ViewChanged(winrt::auto_revoke, [weakThis = get_weak()](auto&&, auto&&) {
            if (auto page = weakThis.get())
            {
                page->_UpdateManagerNavButtons();
            }
        });
        _tabStripSizeChangedRevoker = sv.SizeChanged(winrt::auto_revoke, [weakThis = get_weak()](auto&&, auto&&) {
            if (auto page = weakThis.get())
            {
                page->_UpdateManagerNavButtons();
            }
        });
    }

    // Agentmaster: the Manager lens's currently-selected managed session (the highlighted board card /
    // tree row — the same one the Linked-Lenses sync auto-selects when you switch to the Manager from a
    // session's tab). Empty when nothing — or an external (observe-only) — is selected. Read fresh each
    // time so it always reflects the live selection.
    std::wstring TerminalPage::_ManagerSelectedSessionId() const
    {
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                return std::wstring{ mgr->SelectedSessionId() };
            }
        }
        return {};
    }

    // Agentmaster: recompute the two tab-strip nav buttons. They are inverses and mutually exclusive:
    //  - Home appears when you are NOT on the Manager tab — ALWAYS when the cog's "Always display Home
    //    button" is on (the default), otherwise only once the Manager tab has scrolled off the left edge
    //    of the strip (jump TO it). The Manager tab is index 0, content-x [~0, width); once the
    //    ScrollViewer's horizontal offset passes that width it is fully off-screen (a few-px sliver hides
    //    under the `<` arrow). The width is cached while the tab is realized because the ItemsStackPanel
    //    virtualizes the container away once it's scrolled off (a live ActualWidth then reads 0). The
    //    scroll-triggered path is gated on real overflow (ScrollableWidth > 0), so with few tabs (no
    //    `<`/`>` arrows) it stays hidden.
    //  - Jump Back appears when you ARE on the Manager tab and a managed session card is selected that
    //    this window hosts as a live tab (jump BACK to it — the session you came from).
    // Called from the strip's ViewChanged/SizeChanged (scroll/resize), tab add/remove, tab switch, the
    // lens-changed push (selection), a settings change (always-show toggled), and once at first layout.
    void TerminalPage::_UpdateManagerNavButtons()
    {
        _EnsureTabStripScrollViewer();

        // Is the pinned Manager tab the currently-focused tab? Home hides on it (you're already there);
        // Jump Back shows only on it.
        const bool onManager = _managerTab && (_GetFocusedTab() == _managerTab);

        // --- Home: always (cog setting), or only when the Manager scrolled off the left edge; never
        //     while we're already on the Manager tab. ---
        auto showHome = false;
        if (!onManager && _managerTab)
        {
            if (_appSettings.alwaysShowHomeButton)
            {
                showHome = true; // a persistent jump-to-Manager affordance, regardless of scroll/overflow
            }
            else if (_tabStripScrollViewer)
            {
                const auto& sv = _tabStripScrollViewer;
                if (sv.ScrollableWidth() > 0.5) // there's horizontal overflow (the scroll arrows are showing)
                {
                    if (const auto tvi = _managerTab.TabViewItem())
                    {
                        if (const auto w = tvi.ActualWidth(); w > 1.0)
                        {
                            _managerTabWidthCache = w;
                        }
                    }
                    if (_managerTabWidthCache > 1.0)
                    {
                        showHome = sv.HorizontalOffset() >= (_managerTabWidthCache - 1.0);
                    }
                }
            }
        }
        _SetTabStripButtonVisible(_managerHomeButton, showHome);

        // --- Jump Back: on the Manager tab, with a selected managed session this window hosts live. ---
        auto showBack = false;
        if (onManager)
        {
            if (const auto sel = _ManagerSelectedSessionId(); !sel.empty())
            {
                const auto it = _claudeTabs.find(sel);
                showBack = (it != _claudeTabs.end()) && static_cast<bool>(it->second.get());
            }
        }
        _SetTabStripButtonVisible(_managerJumpBackButton, showBack);
    }

    // Agentmaster: the Home button's action — jump TO the pinned Manager tab and scroll the strip fully
    // left so it's revealed (the ViewChanged that follows re-hides the button). Selecting the tab also
    // gives it focus, which is the "jump to Agent Manager" the user asked for.
    void TerminalPage::_OnManagerHomeButtonClick(const IInspectable& /*sender*/, const winrt::WUX::RoutedEventArgs& /*args*/)
    {
        if (!_managerTab)
        {
            return;
        }

        uint32_t idx = 0;
        if (_tabs.IndexOf(_managerTab, idx))
        {
            _SelectTab(idx);
        }
        if (_tabStripScrollViewer)
        {
            _tabStripScrollViewer.ChangeView(0.0, nullptr, nullptr, true);
        }
    }

    // Agentmaster: the Jump Back button's action — the inverse of Home. Return to the tab of the
    // session currently selected in the Manager lens (the card auto-selected when you came from it).
    // _ActivateClaudeSession jumps locally, else fans out to the window that hosts the session.
    void TerminalPage::_OnManagerJumpBackButtonClick(const IInspectable& /*sender*/, const winrt::WUX::RoutedEventArgs& /*args*/)
    {
        if (const auto sel = _ManagerSelectedSessionId(); !sel.empty())
        {
            _ActivateClaudeSession(winrt::hstring{ sel });
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
        // Agentmaster: publish the GLOBAL tab-rename commit mode to the (process-wide) tab headers.
        // Cross-window, so it lives in one process-static the headers read live, not per-tab state.
        SetTabRenameCommitMode(static_cast<int32_t>(_appSettings.tabRenameCommitMode));
        // Agentmaster: seed the tab-strip close affordances (show-X / middle-click close) from the
        // loaded settings now (the initial SetSettings pass ran with default _appSettings). No tabs
        // exist yet, so this just sets the hook flag + overlay mode; per-tab visibility is then applied
        // as each tab is created/selected (_UpdatedSelectedTab -> _updateAllTabCloseButtons).
        _updateAllTabCloseButtons();

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

        // Agentmaster (System notifications; NOTIFICATIONS.md §4a): register the TOAST COM ACTIVATOR
        // process-wide, so the shell delivers a toast click to THIS running instance
        // (INotificationActivationCallback::Activate -> the activate fan-out -> foreground + jump)
        // instead of falling back to an AUMID activation, which launches a second process and opens a
        // stray new window. Once per process (std::once_flag) — it registers a machine-global COM class
        // object, not per-window state. It lives here rather than in Engine.cpp because that TU is plain
        // C++/no-WinRT by contract, and this needs COM/WinRT; and it must be in TerminalApp.dll rather
        // than the EXE because the jump routes through the engine's fan-out, which the EXE doesn't link.
        {
            static std::once_flag toastActivatorOnce;
            std::call_once(toastActivatorOnce, [] { ::Agentmaster::ToastActivator::Register(); });
        }

        // Agentmaster (exception forensics): install the VEH throw-stack ring + this module's wil
        // failure sink, so every swallowed catch(...) / CATCH_LOG logs WHAT threw and the THROW-SITE
        // stack (module+RVA, offline-symbolizable) to hooks.log. Self-onced; cheap when idle.
        ::Agentmaster::InstallAgentExceptionTrace();

        // M10 (PERSISTENCE.md §13): claim this window's persisted record — an existing
        // windows/<id>.json (geometry + Manager lens + ordered tab refs), or a fresh GUID if none
        // remains. Stashed in _windowRecord; its lens is seeded into the Manager tab in
        // _WireAgentManagerContent, and changes are debounced-autosaved back to the same file.
        // Increment 3: when the Emperor assigned this window a specific record (multi-window
        // reopen), claim THAT id so geometry (TerminalWindow) and lens (here) come from the same
        // record; otherwise claim the front record (single-window) or mint a fresh id.
        // Agentmaster (tear-out content loss): a window created from MOVED content — a tab torn out
        // into a new window, or moved cross-window — must NEVER front-pop a leftover on-disk record.
        // Its content comes from the moved-content startup actions (_OnFirstLayout ->
        // ProcessStartupActions) and its geometry from the drop point. Front-popping an unclaimed
        // record here sets _windowRecordClaimed, which makes _OnFirstLayout SKIP the moved content
        // (the `&& !_windowRecordClaimed` gate) AND makes _RestoreWindowTabs replay the stale record's
        // tabs instead — silently DROPPING the dragged session (the live content orphans in the
        // process-wide ContentManager: claude.exe keeps running but no tab hosts it). This mirrors
        // upstream TerminalWindow::Initialize, where moved content wins over a persisted layout. An
        // explicit Emperor-assigned id (multi-window reopen) still claims THAT record by id.
        std::optional<::Agentmaster::WindowRecord> claimed;
        if (!_assignedWindowId.empty())
        {
            claimed = ::Agentmaster::ClaimWindowRecord(_assignedWindowId);
        }
        else if (!_isContentWindow)
        {
            claimed = ::Agentmaster::ClaimWindowRecord();
        }
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

        // Restore-story trace (line 1): did this window CLAIM a saved record (geometry + lens + ordered tab
        // refs) or start fresh? When claimed, count the refs by kind + name the focused session, so the
        // [rehome-begin]/[rehome] lines that follow (in _RestoreWindowTabs) read as one coherent window
        // restore story for THIS windowId. Once per window creation — never per-tick.
        if (_windowRecordClaimed)
        {
            size_t cl = 0, cx = 0, sh = 0;
            for (const auto& t : _windowRecord.tabs)
            {
                if (t.kind == ::Agentmaster::TabKind::Claude)
                {
                    ++cl;
                }
                else if (t.kind == ::Agentmaster::TabKind::Codex)
                {
                    ++cx;
                }
                else
                {
                    ++sh;
                }
            }
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[window-claim] " + _windowId + L" refs=" + std::to_wstring(_windowRecord.tabs.size()) +
                                              L" (claude=" + std::to_wstring(cl) + L" codex=" + std::to_wstring(cx) + L" shell=" + std::to_wstring(sh) +
                                              L") selected=" + ::Agentmaster::ShortId(_windowRecord.selectedSessionId) + L"\n");
        }
        else
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[window-fresh] " + _windowId + (_isContentWindow ? std::wstring{ L" (dragged-out content window)" } : std::wstring{ L" (new window \x2014 no saved record)" }) + L"\n");
        }

        // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5): mark this window LIVE in the
        // process-wide set, which rewrites open-windows.json. Done here (not at WindowEmperor create
        // time) because _windowId is only resolved now — register early + correct so even a one-window
        // session lands in the manifest and reopens next run. Unregistered in ~TerminalPage.
        ::Agentmaster::RegisterLiveWindow(_windowId);

        // Cross-window activate sink (Linked Lenses): the Manager board/tree show the WHOLE fleet,
        // but a session's tab lives in exactly one window — when ANOTHER window's Activate
        // (board/tree double-click, tree Enter, the Auto Testing's eye) targets a session hosted
        // HERE, this sink hops to this window's UI thread, re-checks _claudeTabs there (the host
        // can change while the hop is in flight), and on a hit selects the tab + brings this
        // window to the foreground. A miss is a no-op — the engine fans out to every window, and
        // only the (single) host acts. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowActivateToken = ::Agentmaster::RegisterWindowActivateHandler(_windowId, [weakThis, dispatcher](const std::wstring& id) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                    if (auto self = weakThis.get())
                    {
                        self->_FocusClaudeSessionTab(id, /*bringWindowToFront*/ true);
                    }
                });
            });
        }

        // Cross-window restart sink: the twin of the activate sink for "Restart session" (Triage Board
        // card / Explorer-tree row). When ANOTHER window's restart targets a session hosted HERE, this
        // sink hops to this window's UI thread and rebuilds its connection in place (no foreground — a
        // restart shouldn't yank the user away from the board). A miss is a no-op. Detached in
        // ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowRestartToken = ::Agentmaster::RegisterWindowRestartHandler(_windowId, [weakThis, dispatcher](const std::wstring& id) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                    if (auto self = weakThis.get())
                    {
                        self->_RestartClaudeSessionLocal(id);
                    }
                });
            });
        }

        // Cross-window close-session sink ("Close ▸ Of Same Folder"): the board/tree session menu closes
        // every live managed session sharing a folder; the initiating window closes the tabs it hosts and
        // fans the rest out here. When ANOTHER window's folder-close targets a session hosted HERE, this
        // sink hops to this window's UI thread and closes its tab (skip-confirm — the initiating window's
        // one "Close N sessions in this folder?" dialog already covered the whole batch). A miss is a
        // no-op. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowCloseSessionToken = ::Agentmaster::RegisterWindowCloseSessionHandler(_windowId, [weakThis, dispatcher](const std::wstring& id) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                    if (auto self = weakThis.get())
                    {
                        self->_CloseClaudeSessionLocal(id);
                    }
                });
            });
        }

        // Cross-window "Activate All Tabs" sink (eager-init): the Manager's fleet-wide "Activate All Tabs"
        // in ANOTHER window fans out here; this sink hops to this window's UI thread and eager-inits all of
        // its OWN dormant tabs (a tab can only be started in the window that hosts its control). Detached in
        // ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowActivateAllToken = ::Agentmaster::RegisterActivateAllDormantHandler(_windowId, [weakThis, dispatcher]() {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    if (auto self = weakThis.get())
                    {
                        self->_ActivateAllDormantTabsLocal();
                    }
                });
            });
        }

        // Command-action sink (COMMANDS.md): a CommandWatch binding's await resolved — v1 the
        // /handover markdown landed on disk. Fired on the engine's scanner thread and fanned to
        // EVERY window (no source window exists); this sink hops to this window's UI thread, and
        // the handler acts only when THIS window hosts the origin session's tab (spawn the
        // "(handover)" successor beside it). A miss is a no-op. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _commandActionToken = ::Agentmaster::RegisterCommandActionHandler(_windowId, [weakThis, dispatcher](const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, sessionId, command, payload, args]() {
                    // Safeguard: a background-originated action landing on the UI thread must
                    // never unwind unhandled (an uncaught throw here terminates the app). The
                    // handler is internally guarded too; this is the dispatch-lambda belt.
                    // `args` = the typed command's <command-args> verbatim — the §6b per-message
                    // model hint parses out of their leading words inside the handler.
                    try
                    {
                        if (auto self = weakThis.get())
                        {
                            if (command == L"handover")
                            {
                                self->_HandleCommandHandover(sessionId, payload, args);
                            }
                            else if (command == L"handover-here")
                            {
                                // COMMANDS.md — the in-place twin: REPLACE the origin tab (the
                                // Restart-session swap into a fresh successor) instead of
                                // spawning a new tab beside it.
                                self->_HandleCommandHandover(sessionId, payload, args, /*inPlace*/ true);
                            }
                            else if (command == L"handover-standby")
                            {
                                // COMMANDS.md §5b — the FILL-NOT-SEND member: new successor
                                // tab(s) like /handover, but each briefing is TYPED into its
                                // session's input box WITHOUT submitting (one Enter away).
                                self->_HandleCommandHandover(sessionId, payload, args, /*inPlace*/ false, /*standby*/ true);
                            }
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"command-action sink dispatch");
                    }
                });
            });
        }

        // Cross-window settings broadcast: a GLOBAL settings change in ANOTHER window (the cog Save, or
        // the Explorer-Tree / Triage-Board sort toggle) reaches here; this sink hops to this window's UI
        // thread and re-applies it live (this window's _appSettings + the sort toggles + a board/tree
        // re-sort). The SOURCE window is excluded by BroadcastSettingsChanged (it already applied it), so
        // this never echoes back. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _settingsChangedToken = ::Agentmaster::RegisterSettingsChangedHandler(_windowId, [weakThis, dispatcher](const ::Agentmaster::AppSettings& s) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, s]() {
                    if (auto self = weakThis.get())
                    {
                        self->_ApplyBroadcastSettings(s);
                    }
                });
            });
        }

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

        // Agentmaster (discard Manager-only windows): a debounced self-close check, run after every tab
        // add/remove + at the end of startup. Debounced (500ms < the 750ms record save) so a window
        // settling into Manager-only — a user closing/tearing-out its last terminal tab, or an
        // empty/failed reopen — is evaluated only once its tab set SETTLES (an async shell re-home is
        // never momentarily mistaken for empty), and so a burst of tab churn collapses to one decision.
        _managerOnlyCheckThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 500 },
                .debounce = true,
                .trailing = true,
            },
            [weakThis = get_weak()]() {
                if (auto self = weakThis.get())
                {
                    self->_CloseWindowIfManagerOnly();
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

            // Agentmaster (tab status dot): live-update the tab strip's "[icon] ● <title>" dot on
            // registry changes — the same push that redraws the Manager board recolors the hosting
            // tab's dot (the Waiting->Idle cache decay rides this too, so the dot fades with the
            // card). Observers fire on arbitrary threads (bridge/scanner) -> bounce to this window's
            // dispatcher; the UI-thread reaction is one _claudeTabs lookup + a brush write, and
            // _SetTabAgentDot is idempotent on an unchanged color. The SAME hop also re-pins the
            // hosting tab's TITLE when the registry title changed (cross-window rename, Rule #11:
            // an Explorer-tree/board rename in ANOTHER window writes the shared registry; only the
            // window holding the tab can retitle it — _SyncClaudeTabTitleFromRegistry reads the
            // title FRESH and pins it through the latch, so the two sync directions converge and
            // never ping-pong). Token detached in ~TerminalPage (Rule #10 — a closed window's
            // observer must not linger on the registry).
            //
            // ⚠ COALESCED (the AgentManagerContent lens-observer discipline; 2026-07-25 release
            // freeze). The old shape enqueued ONE RunAsync PER NOTIFY, so a notify storm — a hook
            // burst across a big fleet, or a misbehaving engine loop (the SemiAuto arm spin: ~300
            // notifies/s sustained for minutes) — grew this window's dispatcher queue faster than
            // the UI thread drained it, and input starved for good ("half-frozen app"; the engine
            // threads kept running, which is exactly what the logs showed). Now the observer only
            // FOLDS the changed id into a shared pending set; a single Low-priority hop drains the
            // WHOLE set, reading each session FRESH from the registry on the UI thread (the same
            // no-stale-capture rule the title re-read has always followed — a batched dot can only
            // be MORE current than the snapshot the notify carried). A notify landing mid-drain
            // re-queues the next hop, so the trailing state is never lost; sub-hop state blips are
            // deliberately foldable (the flash/toast edge detectors already suppress exactly those
            // via ShouldSuppressAnswerBlipFlash / the toast HOLD + dedupe guards).
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            struct AgentDotBatch
            {
                std::mutex mtx;
                std::unordered_set<std::wstring> ids; // sessions with an un-drained change
                bool hopQueued{ false }; // one drain hop in flight at a time
            };
            auto batch = std::make_shared<AgentDotBatch>();
            const auto registry = _sessionRegistry; // the drain re-reads FRESH state (never the notify snapshot)
            _agentDotObserverToken = _sessionRegistry->AddObserver([weakThis, dispatcher, batch, registry](const ::Agentmaster::SessionInfo& s, ::Agentmaster::HookEvent) {
                {
                    std::lock_guard lk{ batch->mtx };
                    batch->ids.insert(s.id);
                    if (batch->hopQueued)
                    {
                        return; // a drain hop is already queued — this change folds into it
                    }
                    batch->hopQueued = true;
                }
                try
                {
                    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis, batch, registry]() {
                        std::unordered_set<std::wstring> ids;
                        {
                            std::lock_guard lk{ batch->mtx };
                            ids.swap(batch->ids);
                            batch->hopQueued = false; // cleared WITH the swap: a notify after this queues the next hop
                        }
                        const auto self = weakThis.get();
                        if (!self)
                        {
                            return;
                        }
                        for (const auto& id : ids)
                        {
                            if (const auto cur = registry->Get(id))
                            {
                                // Agentmaster (eager-init): a live MANAGED session whose ConPTY hasn't
                                // started yet (a window-restored / re-homed tab the user never clicked)
                                // shows the half-hollow dot. An external (observe-only) session has no
                                // control of ours, so it is never "dormant".
                                const bool dormant = cur->live && !cur->started && !cur->external;
                                self->_UpdateTabAgentDot(id, cur->state, cur->live, dormant);
                            }
                            else
                            {
                                // Removed from the registry between the notify and this drain (the
                                // resume-fresh stale-drop). The per-notify shape delivered that final
                                // snapshot; a fresh read can only miss — so synthesize the !live update
                                // the flash/toast trackers key their per-session cleanup on.
                                self->_UpdateTabAgentDot(id, ::Agentmaster::SessionState::Idle, /*live*/ false, /*dormant*/ false);
                            }
                            self->_SyncClaudeTabTitleFromRegistry(id); // reads the CURRENT registry title (no stale capture)
                        }
                    });
                }
                catch (...)
                {
                    // Dispatcher gone (window tearing down) — never leave the drain latched off.
                    {
                        std::lock_guard lk{ batch->mtx };
                        batch->hopQueued = false;
                    }
                    ::Agentmaster::AgentLogCaughtException(L"agent tab-dot observer dispatch");
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
                    self->_ScanPendingInput(); // PENDING_INPUT.md: record each live Claude tab's unsent input-box draft
                    self->_ScanCacheWarmTabs(); // SPARK CROWN: re-evaluate ServerCacheStillWarm per hosted session -> the tab-strip glow + rising embers (polled: warmth ENDS on a clock, nothing pushes it)
                    self->_PumpHandoverInjections(); // COMMANDS.md §5: paste-inject a spawned successor's FULL over-budget handover document once its claude starts
                    self->_SweepHandoverDeletes(); // COMMANDS.md §6b: delete a handed-over md once its successor actually STARTED (opt-in; keeps the file if the successor never comes up)
                    self->_PumpDraftRestores(); // PENDING_INPUT.md §10: type a reopened session's remembered unsent draft back into its input box once its claude starts (fill, never submit)
                    self->_SweepAgentPendingToasts(); // System notifications: fire/drop the HELD completion toasts (spurious-toast suppression — SessionScanner's DecideHeldToast)
                    self->_ScanInferredTabColors(); // tab color modes: re-infer each Claude tab's ACTUAL workdir + recolor (every session under InferredWorkingDirectory; ONLY home-dir launches — forced inference, SessionInfersWorkingDir — in the other modes; throttled + mtime-gated inside)
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
                // Agentmaster (discard Manager-only windows): a tab add/remove may have left this window
                // holding only the Manager tab (its last terminal tab closed or torn out) — schedule the
                // debounced check that self-closes / un-persists such a window once its tab set settles.
                self->_ScheduleManagerOnlyCheck();
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

        // Agentmaster (alt+up/down prompt nav, SUMMARY_JUMP.md §7): a free-running 30 s refresh that keeps
        // the FOCUSED Claude session's jump data in sync with the live buffer between keypresses — re-reads
        // its sent prompts (mtime-gated) into _promptNavCache + re-resolves the summary panel's jump-icon
        // eligibility. Each tick no-ops unless a managed Claude tab is focused. Self-stops if the page is
        // gone (weak); also stopped in ~TerminalPage. (_InitAgentmasterEngine runs once per window — the
        // `_sessionRegistry` guard at the top — so this builds + starts exactly one timer.)
        _promptNavRefreshTimer = DispatcherTimer{};
        _promptNavRefreshTimer.Interval(std::chrono::seconds{ 30 });
        _promptNavRefreshTimer.Tick([weakThis = get_weak()](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable&) {
            if (auto self = weakThis.get())
            {
                if (const auto sid = self->_FocusedPromptNavSession(); !sid.empty())
                {
                    self->_RefreshPromptNavCache(sid);
                }
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop(); // page destroyed — stop ticking (UI thread, safe)
            }
        });
        _promptNavRefreshTimer.Start();
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
        content->SetSpawnHandler([weakThis](winrt::hstring dir, winrt::hstring title, winrt::hstring model) {
            if (auto self = weakThis.get())
            {
                // model rides the launch-model picker ("Open New Session Here ▸ <model>"); "" = Default.
                self->_SpawnClaudeSession(dir, title, static_cast<uint32_t>(-1), model);
            }
        });
        content->SetActivateHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ActivateClaudeSession(id);
            }
        });
        // Agentmaster (eager-init): "Activate Tab" — start a dormant session's claude IN PLACE (no focus
        // change). LOCAL only: the content offers the item solely for a session this window hosts (a
        // single-session start can only target the window owning that control). A no-op if it isn't ours
        // (e.g. it started since the menu was built); a remote dormant session is woken via "Jump to Tab"
        // or "Activate All Tabs".
        content->SetActivateDormantHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ActivateDormantSession(std::wstring{ id });
            }
        });
        // Agentmaster (eager-init): "Activate All Tabs" — wake this window's dormant tabs; if allWindows,
        // fan out to every other window too (each eager-inits its own). The content decides the scope
        // (prompting only when other windows also have dormant tabs).
        content->SetActivateAllHandler([weakThis](bool allWindows) {
            if (auto self = weakThis.get())
            {
                self->_ActivateAllDormantTabsLocal();
                if (allWindows)
                {
                    ::Agentmaster::ActivateAllDormantInOtherWindows(self->_windowId);
                }
            }
        });
        // Agentmaster (Linked Lenses): the Manager reports a pointer enter/leave on a managed
        // card/row (id, entering). Track the effective hovered session here — a leave only clears
        // when it's still the hovered id, which absorbs the enter-B-before-leave-A ordering when the
        // pointer slides between rows — then re-evaluate the per-tab "selected/active" pill (a live
        // preview that follows the mouse while you're on the Manager tab).
        content->SetHoverSessionHandler([weakThis](winrt::hstring id, bool entering) {
            if (auto self = weakThis.get())
            {
                const std::wstring sid{ id };
                if (entering)
                {
                    self->_managerHoverSessionId = sid;
                }
                else if (self->_managerHoverSessionId == sid)
                {
                    self->_managerHoverSessionId.clear();
                }
                self->_UpdateManagerSelectionHighlight();
            }
        });
        // Agentmaster (FAVORITES.md): the board/tree "Close" verb (formerly Archive) — shut the session
        // down but KEEP the record (always archived), so it stays resumable in the Sessions browser.
        content->SetArchiveHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ArchiveClaudeSession(id);
            }
        });
        // Agentmaster (board/tree session menu — "Close ▸ Of Same Folder"): close every live managed
        // session whose effective work dir matches the folder. The content already showed the ONE confirm
        // listing the titles; the page does the batch close (local tabs directly, cross-window fanned out).
        content->SetCloseFolderHandler([weakThis](winrt::hstring folder) {
            if (auto self = weakThis.get())
            {
                self->_CloseClaudeSessionsInFolder(folder);
            }
        });
        content->SetRestoreHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_RestoreArchivedSession(id);
            }
        });
        // Agentmaster: the Launch box accepted a FOUND session id. The content already resolved its
        // (dir, title), so reuse the same on-disk seams the Sessions page uses — resume (live -> Jump,
        // archived/unknown -> minimal record + claude --resume) and fork (claude --resume --fork-session).
        content->SetResumeSessionHandler([weakThis](winrt::hstring id, winrt::hstring dir, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_ResumeSessionFromDisk(std::wstring{ id }, std::wstring{ dir }, std::wstring{ title });
            }
        });
        content->SetForkSessionHandler([weakThis](winrt::hstring id, winrt::hstring dir, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_ForkSessionFromDisk(std::wstring{ id }, std::wstring{ dir }, std::wstring{ title });
            }
        });
        // Agentmaster: the Triage Board / Explorer-tree session menu's "Restart session" — restart a
        // managed session's live connection in place (resume the current conversation; never replay the
        // launch commandline). Local-first, then fan out to the hosting window (the board GLOBAL scope
        // shows the whole fleet; the live pane lives in exactly one window).
        content->SetRestartSessionHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_RestartClaudeSession(id);
            }
        });
        // Agentmaster: the Triage Board / Explorer-tree session menu's "Fork session" — kind-aware fork
        // (Claude --fork-session / Codex `codex fork`), the same path the WT tab's "Fork session" uses,
        // opening the fork tab in THIS window (it reads the shared registry; no live tab needed).
        // model rides the launch-model picker's "Fork session ▸ <model>" submenu ("" = Default).
        content->SetForkManagedSessionHandler([weakThis](winrt::hstring id, winrt::hstring model) {
            if (auto self = weakThis.get())
            {
                self->_ForkManagedSessionById(std::wstring{ id }, static_cast<uint32_t>(-1), std::wstring{ model });
            }
        });
        content->SetRenameHandler([weakThis](winrt::hstring id, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_RenameClaudeSession(id, title);
            }
        });
        // Agentmaster (bookmark tags): the board-card / tree-row "Tags" item — open the page's tag
        // editor panel for that session, anchored under the clicked card/row (the Manager content
        // lives inside this page's visual tree, so the anchor transforms into Root() space).
        content->SetTagsHandler([weakThis](winrt::hstring id, winrt::Windows::UI::Xaml::FrameworkElement anchor) {
            if (auto self = weakThis.get())
            {
                self->_OpenTagEditorForElement(std::wstring{ id }, anchor);
            }
        });
        // Agentmaster (bookmark tags): a Triage-Board card ribbon's pointer enter/leave — route to
        // the SAME rich tag hover panel the tab badges + the Sessions Tags column use (every session
        // carrying the tag + its status dot; clicking a live row jumps to its tab). The Manager
        // content lives inside this page's visual tree, so the ribbon anchors into Root() space
        // like any main-tree badge.
        content->SetTagHoverHandlers(
            [weakThis](winrt::hstring tag, winrt::Windows::UI::Xaml::UIElement anchor) {
                if (auto self = weakThis.get())
                {
                    self->_OnTagBadgeHoverBegin(tag, anchor);
                }
            },
            [weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_OnTagBadgeHoverEnd();
                }
            });
        // Agentmaster: adopt an EXTERNAL (observe-only) claude from the Explorer Tree's EXTERNAL
        // scope — bring its conversation under management. `fork` (chosen in the Manager's Adopt
        // dialog) selects the two-writers-safe branch (--fork-session into a NEW transcript) vs. a
        // straight --resume of the same conversation (true take-over; the user stops the original).
        content->SetAdoptExternalHandler([weakThis](uint32_t pid, winrt::hstring cwd, bool fork) {
            if (auto self = weakThis.get())
            {
                self->_AdoptExternalClaude(pid, cwd, fork);
            }
        });
        // Agentmaster (Codex-launch): the EXTERNAL-codex menu — Adopt (bring its rollout under
        // management: `fork` => `codex fork` into a NEW rollout [safe on a live codex], else
        // `codex resume` the same) or Open New Codex Session Here (a fresh managed codex in the cwd,
        // adopt==false). Lifecycle + state only.
        content->SetCodexLaunchHandler([weakThis](uint32_t pid, winrt::hstring cwd, bool adopt, bool fork) {
            if (auto self = weakThis.get())
            {
                if (adopt)
                {
                    self->_AdoptExternalCodex(pid, cwd, fork);
                }
                else
                {
                    self->_SpawnCodexSession(cwd, winrt::hstring{});
                }
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
        // Agentmaster (PENDING_INPUT.md §8): the session menu's "Copy Current Prompt" reads the UNSENT
        // draft LIVE off the session's buffer. Only the hosting window can do that, and the board/tree
        // list the WHOLE fleet — _ReadLiveDraftForSession answers "" for anything not readable HERE
        // (another window's tab, a dormant one, a torn-down control), which makes the copy fall back to
        // the observer's recorded draft. Read-only, on-demand (per click), UI thread.
        content->SetLiveDraftProvider([weakThis](const std::wstring& sessionId) -> std::wstring {
            auto self = weakThis.get();
            return self ? self->_ReadLiveDraftForSession(sessionId) : std::wstring{};
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
        // Agentmaster (focus-steal fix): tell the content whether THIS window is the OS foreground
        // window. The Manager's _Refresh restores keyboard focus to a board card after each rebuild,
        // and in XAML Islands Control.Focus() escalates to Win32 activation of the island's host
        // window — so a refresh firing on a BACKGROUND Manager window (the Triage Board sitting behind
        // a Claude tab you're working in, in ANOTHER window) would steal the OS foreground on every
        // ~2s observer/registry tick. Comparing GetForegroundWindow() to our HWND (the same real-time
        // test _FocusClaudeSessionTab uses) gates the restore to the foreground case only.
        content->SetWindowForegroundProvider([weakThis]() -> bool {
            auto self = weakThis.get();
            return self && self->_hostingHwnd && ::GetForegroundWindow() == *self->_hostingHwnd;
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
                // hiddenSessionIds (the Sessions browser's "Hide from list" set) AND showSummaryPanel
                // (the per-tab summary-panel pencil) are owned by UI actions OUTSIDE the cog form, each a
                // freshest-disk RMW. The cog FORM never edits them, so preserve the on-disk values here so
                // a form Save can't regress a hide/reset/pencil-toggle done since the modal was seeded
                // (incl. by another window).
                {
                    const auto disk = ::Agentmaster::LoadAppSettings();
                    s.hiddenSessionIds = disk.hiddenSessionIds;
                    s.showSummaryPanel = disk.showSummaryPanel;
                    s.summaryPanelWrapNewlines = disk.summaryPanelWrapNewlines; // wrap-line toggle (panel times bar), out-of-cog UI action
                    s.summaryPanelTruncate = disk.summaryPanelTruncate; // truncate toggle (panel times bar), out-of-cog UI action
                    // Tab color picker: the Custom / advanced (More) expansion states are written by
                    // the ColorPickupFlyout itself as the user toggles them (freshest-disk RMW), never
                    // by this form — preserve them so a cog Save can't reset how the picker opens.
                    s.tabColorPickerCustomOpen = disk.tabColorPickerCustomOpen;
                    s.tabColorPickerAdvancedOpen = disk.tabColorPickerAdvancedOpen;
                    // Updater (Updater.h): ALL update state is written outside the cog form —
                    // skip/postpone by the prompt's JSON RMW (possibly from another window or the
                    // startup check), the prerelease + nightly opt-ins by each switch's own
                    // INSTANT-APPLY RMW — so preserve all four from disk on Save (a stale form copy
                    // must never regress a flip/choice made since the modal was seeded).
                    s.allowUpdatePrerelease = disk.allowUpdatePrerelease;
                    s.allowUpdateNightly = disk.allowUpdateNightly;
                    s.updateSkippedVersion = disk.updateSkippedVersion;
                    s.updatePostponedUntilUnixMs = disk.updatePostponedUntilUnixMs;
                    // Slash commands (COMMANDS.md §6a): the MATERIALIZED-name markers are engine
                    // reality (which name's definition file the last init wrote), RMW'd at engine
                    // init — never form state. Preserve them like the seed markers so a cog Save
                    // can't rewrite what is actually on disk (the next init's migration relies
                    // on them to know which old file to clean up).
                    s.commandHandoverMaterializedName = disk.commandHandoverMaterializedName;
                    s.commandHandoverHereMaterializedName = disk.commandHandoverHereMaterializedName;
                    s.commandHandoverStandbyMaterializedName = disk.commandHandoverStandbyMaterializedName; // the §5b standby member's marker

                }
                // Inferred-workdir inputs (mode / "Use .git folder to infer") — capture the change
                // BEFORE adopting `s`, for the scan-state reset below.
                const bool inferInputsChanged = self->_appSettings.tabColorMode != s.tabColorMode ||
                                                self->_appSettings.inferGitRoot != s.inferGitRoot;
                self->_appSettings = s;
                ::Agentmaster::SaveAppSettings(s);
                // Adopt/re-home autorunner default: the registry stamps this MODE onto records it
                // creates ITSELF (the hook-adopt + observer first-sight creates — the two open-seams
                // outside TerminalPage's launch/restore stamping). Engine init seeds it from disk;
                // keep it live with the cog here. ONE shared engine per process, so no broadcast leg
                // is needed. Gated like the scheduler wiring (dev-or-debug): a plain release keeps
                // minting Off records to match its never-started autorunner.
                if (::Agentmaster::Profiles::IsDevOrDebugPackage() && self->_sessionRegistry)
                {
                    self->_sessionRegistry->SetDefaultAutorunnerMode(s.defaultAutorunnerMode);
                }
                // Apply the (possibly changed) GLOBAL rename-commit mode to every window's tab
                // headers immediately (process-wide static), not just next launch.
                SetTabRenameCommitMode(static_cast<int32_t>(s.tabRenameCommitMode));
                // Apply the (possibly changed) tab-strip close affordances (show-X / middle-click
                // close) to THIS window's tabs immediately; other windows get them via the broadcast.
                self->_updateAllTabCloseButtons();
                // Apply the (possibly changed) "Show icons on tabs" setting to THIS window's tabs now
                // (show/hide the profile icon on every tab); other windows get it via the broadcast.
                self->_UpdateAllTabIcons();
                // Apply the (possibly changed) "Always display Home button" setting to THIS window's
                // tab-strip nav buttons immediately; other windows get it via the broadcast.
                self->_UpdateManagerNavButtons();
                // FAVORITES.md §5a: the favorite marker (Crown <-> Star) is GLOBAL — re-assert it on every
                // hosted favorited tab so a glyph change takes effect now; other windows get it via the broadcast.
                self->_RefreshAllFavoriteIcons();
                // Status-dot flash-ring color is GLOBAL — re-point this window's shared flash-ring brush at the
                // (possibly changed) color/opacity so any currently-flashing tab recolors live; other windows
                // pick it up via the broadcast below.
                self->_RefreshFlashRingBrush();
                // Per-tab overlay rest/hover opacities are GLOBAL (TAB_OVERLAY.md) — push the (possibly
                // changed) pair to every hosted overlay so the badge/summary dim<->bright updates live; other
                // windows pick it up via the broadcast below.
                self->_RefreshOverlayOpacities();
                // Tab color mode is GLOBAL — repaint THIS window's managed tabs per the (possibly
                // changed) mode now (per-dir / individual / inferred); other windows repaint via the
                // broadcast below, and the board/chips re-resolve on their next rebuild.
                self->_ReapplyManagedTabColors();
                // Inferred-workdir inputs changed (the mode, or "Use .git folder to infer"): drop the
                // per-session scan gates (lastMtime/throttle) so every hosted session RE-INFERS under
                // the new rule now — a quiet transcript would otherwise keep its old inference until
                // its next write — and kick one scan pass immediately (it self-gates per session:
                // SessionInfersWorkingDir admits the whole fleet under the Inferred mode, home-dir
                // launches in every mode).
                if (inferInputsChanged)
                {
                    self->_inferredColorScan.clear();
                    self->_ScanInferredTabColors();
                }
                // Waiting-for-you "unread" model: push the (possibly changed) WaitingForInput -> Idle
                // timeout to the process-wide scanner so it applies immediately, not next launch.
                if (self->_scanner)
                {
                    self->_scanner->SetWaitingDecayMinutes(s.waitingForYouTimeoutMinutes);
                }
                // Re-materialize the shared --settings file so model / co-authored-by /
                // permission-mode changes also reach an adopted hand-typed `claude` (the PATH
                // shim points at this file). Fresh spawns rebuild it from settings regardless.
                try
                {
                    ::Agentmaster::MaterializeSharedHookFiles(::Agentmaster::AgentmasterStateDir(), s);
                }
                CATCH_LOG();
                // Cross-window settings broadcast: live-propagate the merged settings to every OTHER open
                // window, so the Explorer-Tree / Triage-Board sort (and the cog's globals) sync immediately
                // instead of only on each window's next launch. The source window already applied it (its
                // own toggle/cog re-rendered locally + self->_appSettings above), so the broadcast excludes
                // _windowId; each other window's sink marshals onto its UI thread and calls _ApplyBroadcastSettings.
                ::Agentmaster::BroadcastSettingsChanged(s, self->_windowId);
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
        // Agentmaster (Sessions page; SESSIONS.md): the Manager's "Sessions" button opens the
        // full-window browser over EVERY on-disk Claude Code session — the sole history view now that
        // the separate Archive page is gone (FAVORITES.md).
        content->SetOpenSessionsHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ShowSessionsPage();
            }
        });
        // Agentmaster (Sessions page; SESSIONS.md): the Settings cog's "Reset hidden sessions"
        // clears the user's "Hide from list" set (owned by the page, not the cog form) and re-shows
        // every hidden session in the Sessions browser.
        content->SetResetHiddenSessionsHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ResetHiddenSessions();
            }
        });
        // Agentmaster (updater; Updater.h): the cog's "Update now" launches the embedded am-update
        // installer detached, then closes the app (no-confirm) so the package isn't in use while the
        // installer Add-AppxPackages the new build and relaunches it. NOT RequestQuit — that pops WT's
        // generic "close all tabs?" confirmation, which is redundant after the update dialog and whose
        // Cancel would strand the app for the installer to force-kill 20s later.
        content->SetQuitForUpdateHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_QuitForUpdate();
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
                // Linked Lenses: a selection change is one of the lens mutations — re-pill the
                // selected session's tab (when nothing is hovered, the pill tracks the selection).
                self->_UpdateManagerSelectionHighlight();
                // Agentmaster: the Jump Back button targets the selected session, so a selection
                // change while on the Manager tab flips whether it has somewhere to jump back to.
                self->_UpdateManagerNavButtons();
            }
        });
    }

    // Agentmaster (updater; Updater.h): quit the app for an in-app update — the post-confirm half of
    // RequestQuit (flush this window's record + raise QuitRequested) WITHOUT the "close all tabs?"
    // confirmation. Fired by the cog's "Update now" after the embedded installer is launched detached:
    // the user already confirmed in the update dialog, sessions archive on teardown, and the installer
    // would force-close us regardless — so a second generic close-confirm (whose Cancel would only
    // strand the app for the installer to force-kill seconds later) is wrong here.
    void TerminalPage::_QuitForUpdate()
    {
        _FlushWindowRecord();
        _windowRecordTeardownFlushed = true; // mirror RequestQuit: don't let ~TerminalPage re-capture post-teardown
        QuitRequested.raise(nullptr, nullptr);
    }

    // Agentmaster (Linked Lenses — the per-tab -> Manager half of the selection sync): when the user
    // switches to a managed session's terminal tab, drive the Manager lens to select that session, so
    // moving to the Manager tab shows the session you were just in highlighted (board card + tree row +
    // its Auto Testing). Equivalent to a single-click on the session's board card. Called from the one
    // post-startup tab-switch funnel (_OnTabSelectionChanged), so a user click, Ctrl+Tab, or a
    // switchToTab action all follow through here.
    void TerminalPage::_SyncManagerSelectionToTab(const TerminalApp::Tab& tab)
    {
        // Only once startup/restore is done: during _RestoreWindowTabs the focused tab is re-selected,
        // and we must NOT clobber the per-window lens selection that SetManagerState restored from the
        // record (the same gate _ScheduleWindowRecordSave uses).
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        // The Manager tab itself (return to it = SEE the last selection) and non-session tabs
        // (pwsh / cmd / external) leave the current Manager selection untouched.
        if (!tab || tab == _managerTab)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // not a managed Claude/Codex session tab -> nothing to select
        }
        // Nav audit: the user switched FOCUS to this managed session's tab (a tab click, Ctrl+Tab, a
        // switchToTab action, or the landing of an Activate/jump). The core "where is the user now"
        // navigation signal; gated on Initialized so a restore's focus-restore can't spam it.
        ::Agentmaster::LogNav(L"tab-focus " + ::Agentmaster::ShortId(id));
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SelectSession(winrt::hstring{ id });
            }
        }
    }

    // Agentmaster (cross-window settings broadcast): a GLOBAL settings change in ANOTHER window reached
    // this window's engine sink, which marshaled us onto this UI thread. Adopt the merged settings for
    // this window's page copy (so future spawns / the cog's next open use the latest globals) and hand
    // them to the Manager content, which repaints its Explorer-Tree + Triage-Board sort toggles and
    // re-sorts. A no-op for the content if the Manager tab isn't built yet (the page copy still updates).
    void TerminalPage::_ApplyBroadcastSettings(const ::Agentmaster::AppSettings& settings)
    {
        // Inferred-workdir inputs (mode / "Use .git folder to infer") — capture the change BEFORE
        // adopting the broadcast, for the scan-state reset below.
        const bool inferInputsChanged = _appSettings.tabColorMode != settings.tabColorMode ||
                                        _appSettings.inferGitRoot != settings.inferGitRoot;
        _appSettings = settings;
        // Agentmaster: the tab-strip close affordances (show-X / middle-click close) are GLOBAL, so a
        // change made in another window must re-apply to THIS window's tabs live (the source window
        // already did so in its Save handler).
        _updateAllTabCloseButtons();
        // Agentmaster: "Show icons on tabs" is GLOBAL too — re-apply a change made in another window to
        // THIS window's tabs live (show/hide the profile icon on every tab).
        _UpdateAllTabIcons();
        // Agentmaster: "Always display Home button" is GLOBAL too — re-evaluate this window's nav buttons.
        _UpdateManagerNavButtons();
        // FAVORITES.md §5a: the favorite marker (Crown <-> Star) is GLOBAL — re-assert it on every hosted
        // favorited tab so a change made in another window switches the glyph live here too.
        _RefreshAllFavoriteIcons();
        // Status-dot flash-ring color is GLOBAL — re-point this window's shared flash-ring brush so a change
        // made in another window recolors this window's live flash too.
        _RefreshFlashRingBrush();
        // Per-tab overlay rest/hover opacities are GLOBAL (TAB_OVERLAY.md) — apply a change made in another
        // window to this window's overlays live too.
        _RefreshOverlayOpacities();
        // Tab color mode is GLOBAL — a mode change made in another window repaints THIS window's
        // managed tabs live too (the source window already repainted its own in the Save handler).
        _ReapplyManagedTabColors();
        // Inferred-workdir inputs changed in another window: re-infer THIS window's hosted sessions
        // under the new rule too (the Save-handler twin — drop the scan gates + kick one pass).
        if (inferInputsChanged)
        {
            _inferredColorScan.clear();
            _ScanInferredTabColors();
        }
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->ApplyExternalSettings(settings);
            }
        }
    }

    // Agentmaster (Linked Lenses — the reveal half of the per-tab -> Manager sync): when the user
    // switches TO the Manager tab, scroll the currently-selected board card / tree row into view. The
    // lens selection follows tab switches (_SyncManagerSelectionToTab) while the Manager is hidden, so
    // on return the highlighted card can be scrolled off-screen in a tall column (or after a state
    // change moved it to another column); this synchronizes its visibility with its highlight. Called
    // from the one tab-switch funnel (_OnTabSelectionChanged) only when the Manager tab is selected.
    void TerminalPage::_BringManagerSelectionIntoView()
    {
        // Pre-startup the focused-tab restore is in flight (the lens selection is being seeded from the
        // WindowRecord); don't yank the view — the same gate _SyncManagerSelectionToTab uses.
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->BringSelectedIntoView();
            }
        }
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
                    // A drop displaced the pinned Manager tab — record where it landed before snapping back.
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[pin-manager] snap-back (manager was displaced to view idx=" + std::to_wstring(viewIdx) + L")\n");
                    _tabView.TabItems().RemoveAt(viewIdx);
                    _tabView.TabItems().InsertAt(0, tvi);
                    _ApplyTabDragPolicy(tvi); // settle the map after the reinsert; the Manager stays CanDrag(false) like every tab
                }
            }
            CATCH_LOG();
            _UpdateTabIndices();
        }
    }

    // Agentmaster: one-line log identity for tab-strip forensics — `<sid8> "<title>"` for a managed
    // tab, `"<title>"` for a shell tab. Log-path safe: never throws.
    std::wstring TerminalPage::_DescribeTabForLog(const winrt::TerminalApp::Tab& tab)
    {
        try
        {
            if (!tab)
            {
                return L"?";
            }
            std::wstring ident;
            if (const auto sid = _ClaudeSessionForTab(tab); !sid.empty())
            {
                ident = ::Agentmaster::ShortId(sid) + L" ";
            }
            ident += L"\"" + std::wstring{ tab.Title() } + L"\"";
            return ident;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_DescribeTabForLog");
            return L"?";
        }
    }

    // Agentmaster: MUX TabView drag AV — the root cause, the disproven candidates, and the RESOLUTION.
    // Dragging ANY tab has WUX report the dragged ITEM as the pressed container's **Content**, not the
    // TabViewItem itself (ListViewBase::GetDraggedItems, ListViewBase_Partial_Reorder.cpp: for an
    // items-are-their-own-containers list — exactly WT's TabItems of TabViewItems — it appends
    // get_Content() to DragItemsStarting.Items). For WT that Content is the BODGY unique empty Border
    // every Tab plants at construction (Tab.cpp `TabViewItem().Content(Border{})` — upstream's
    // disambiguator for EXACTLY this MUX lookup). So MUX's TabView::FindTabViewItemFromDragItem can
    // NEVER resolve the item up front: ContainerFromItem(border) misses (a Border is not an item of
    // TabItems) and the SINGLE-level VisualTreeHelper::GetParent(border) is null (the Content is never
    // rendered), which sends EVERY drag into the fallback loop `ContainerFromIndex(i).Content() ==
    // item` from index 0 — and that loop null-derefs on the FIRST virtualized-out container
    // (0xC0000005 at Microsoft.UI.Xaml.dll+0xB605C, uncatchable under /EHsc). The lookup runs at
    // drag-START (OnListViewDragItemsStarting) AND at the DROP (OnListViewDragItemsCompleted), and
    // both are crash sites. Dump-proven FOUR times (3x 0.6.7.x on 2026-07-16; 0.6.8.1 on 2026-07-30
    // with Rsi=0xA7 == 167 TabItems, Rdi=0 == died at loop index 0 — the DROP-side variant, whose AV
    // was SWALLOWED at the callback boundary and left a live-but-wedged app). Every fix candidate is
    // DISPROVEN — see the CLAUDE.md "MUX TabView drag-start null-deref" gotcha for the full verdicts:
    //   - panel swap to StackPanel (0.6.7.2): ILLEGAL for a ListView — deterministic startup render
    //     crash; removed. Do NOT retry.
    //   - CacheLength boost (the former _BoostTabStripCacheForDrag, commit c4d366460): ACTIVE in the
    //     crashed 0.6.8.1 instance and index 0 was STILL unrealized at the drop — a large cache is a
    //     realization HINT the panel may not honor. Removed 2026-07-30.
    //   - check-then-allow gates: the 2026-07-30 crash PASSED the start-side lookup and died at the
    //     drop 2.5 s later — mid-drag derealization defeats any gate that can only veto the start.
    //   - Content tricks (self/template-root/parented-Border): the stock TabView template's
    //     UpdateTabContent re-parents tvi.Content() on every selection change — guaranteed
    //     "already child of another element" crash.
    // RESOLUTION: the lookup is made UNREACHABLE — CanReorderTabs(false) + CanDragTabs(false)
    // (TerminalPage::Create + TabRowControl.xaml; the exact configuration WT ships for elevated
    // windows), with reorder + tear-out re-implemented as the POINTER-OWNED gesture below
    // (_WireTabReorderGesture / _TabReorder*): raw pointer events + the existing _TryMoveTab /
    // _sendDraggedTabToWindow — no WUX/MUX drag pipeline, no OLE, nothing that consults an
    // unrealized container. The settle/CanDrag belts below stay: cheap, and they keep the
    // item->container map sane for OUR OWN ContainerFromIndex/ContainerFromItem probes.

    // Agentmaster: MUX TabView drag-start AV guard, layer 1 (a BELT — see the drag-AV note above) —
    // settle the strip's item->container mapping BEFORE
    // returning to the message pump. XAML dispatches queued INPUT ahead of the pending LAYOUT pass, so
    // a press+move queued behind a TabItems() insert/remove/reinsert can cross the drag threshold
    // while the mapping is still uncommitted, and MUX's drag paths consult that mapping
    // (ContainerFromItem/IndexFromContainer). UpdateLayout() runs the pass NOW — the layout was going
    // to run next frame anyway, so this is re-ordered work, not added work. Called after EVERY
    // TabItems() mutation (_InitializeTab / _RemoveTab / _TryMoveTab / _PinManagerTabFirst /
    // _TabDragCompleted).
    void TerminalPage::_SettleTabStripLayout()
    {
        try
        {
            if (_tabView)
            {
                _tabView.UpdateLayout();
            }
        }
        catch (...)
        {
            // Surface the failure in OUR log too (a swallowed settle re-opens the crash window).
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[tabdrag-guard] settle UpdateLayout threw\n");
            ::Agentmaster::AgentLogCaughtException(L"tabdrag-guard settle UpdateLayout");
        }
    }

    // Agentmaster: per-tab native-drag policy (see the drag-AV resolution note above) + the settle.
    // Native MUX tab drag is permanently OFF, so EVERY TabViewItem is pinned CanDrag(false),
    // unconditionally — a UIElement with CanDrag(true) could still start a raw XAML drag even with
    // the list-level CanDragTabs off, which would walk the same broken WUX GetDraggedItems path.
    // (This function replaced _GuardTabDragUntilRegistered, whose settle-then-defer-to-Loaded
    // re-ENABLE existed to restore native drag once the item->container map committed; with native
    // drag gone the re-enable was not just moot but the one hole left in the disable — and its
    // per-Loaded log line stormed at ~30 Hz during a big window-restore. The settle stays: our own
    // reorder gesture + _RevealTabInStrip probe the same item->container map.)
    void TerminalPage::_ApplyTabDragPolicy(const MUX::Controls::TabViewItem& tabViewItem)
    {
        if (!_tabView || !tabViewItem)
        {
            return;
        }
        _SettleTabStripLayout();
        try
        {
            tabViewItem.CanDrag(false); // native drag never starts — the pointer-owned gesture is the only drag
        }
        CATCH_LOG();
    }

    // ======================================================================================
    // Agentmaster: the POINTER-OWNED tab reorder/tear-out gesture — the native-MUX-drag
    // replacement (see the drag-AV resolution note above; pure decision math in
    // AgentMaster/TabDragMath.h, unit-tested in the engine harness).
    //
    // Shape: press a tab header (mouse/pen, left button) -> a 6-DIP threshold activates the
    // gesture and captures the pointer ON THE TABVIEW (not the item — item churn during the
    // gesture must not disturb the capture) -> an accent INSERTION CARET tracks the midpoint
    // rule over the REALIZED headers (a caret-commit design: nothing moves until release, so
    // there is no per-crossing selection/content churn) -> edge auto-scroll (per-move + a
    // hold-still timer) slides the strip -> release IN the strip band commits ONE _TryMoveTab
    // (which already logs [nav] tab-move, announces for UIA, settles + respects the Manager
    // floor); release CLEAR of the strip tears the tab out into a new window through the exact
    // native downstream (_sendDraggedTabToWindow, which logs [nav] tab-send-to-window and
    // detaches the managed session correctly); Esc cancels (nothing moved, nothing to revert).
    // [nav] taxonomy preserved: tab-drag-begin at activation, tab-drag-end at every exit.
    //
    // Deliberate limits: TOUCH is left to the strip's native panning (a touch-drag threshold
    // would fight the pan gesture); cross-window DOCKING (release over another Agentmaster
    // window's strip) is not detected — it tears out into a new window like any outside release
    // (the native OLE docking died with CanDragTabs=false; a same-process hit-test replacement
    // is the noted follow-up).
    // ======================================================================================

    namespace
    {
        constexpr double kTabDragThresholdDips = 6.0; // press->drag arming distance (either axis)
        constexpr double kTabDragEdgeBandDips = 36.0; // auto-scroll engages within this of a strip edge
        constexpr double kTabDragScrollStepDips = 28.0; // per move/tick step (~560 DIPs/s on the 50 ms timer)
        constexpr double kTabDragTearOutSlackDips = 32.0; // grace band around the strip before a release reads as tear-out
        constexpr auto kTabDragAutoScrollTick = std::chrono::milliseconds{ 50 };
    }

    // Wire the gesture's pointer handlers onto the TabView — once, from Create(). AddHandler with
    // handledEventsToo: the ListViewItem internals mark presses Handled (selection), and during our
    // capture the routed events surface on the TabView itself. The lambdas hold get_weak() so a
    // torn-down page no-ops; the handlers die with the TabView's tree, so no RemoveHandler pass.
    void TerminalPage::_WireTabReorderGesture()
    {
        if (!_tabView)
        {
            return;
        }
        try
        {
            auto weak = get_weak();
            _tabView.AddHandler(winrt::WUX::UIElement::PointerPressedEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnPressed(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerMovedEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnMoved(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerReleasedEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnReleased(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerCaptureLostEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnCaptureLost(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerCanceledEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnCaptureLost(e);
                                    }
                                } }),
                                true);
            // The hold-still edge auto-scroll tick (started/stopped per gesture; SafeDispatcherTimer's
            // guarded Destroy makes teardown safe on any thread).
            _tabDragAutoScrollTimer.Interval(kTabDragAutoScrollTick);
            _tabDragAutoScrollTimer.Tick([weak](const auto&, const auto&) {
                if (auto page = weak.get())
                {
                    if (page->_tabReorder.active)
                    {
                        page->_TabReorderUpdate(page->_tabReorder.lastPoint);
                    }
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_WireTabReorderGesture"); // gesture unavailable; tabs still work (menu/keyboard moves)
        }
    }

    // A left press over a draggable tab header ARMS the gesture (nothing visible yet — a plain
    // click stays a plain click; the threshold in OnMoved is what activates).
    void TerminalPage::_TabReorderOnPressed(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if (_tabReorder.active)
            {
                return; // one gesture at a time — a second pointer never steals it
            }
            if (_tabReorder.armed)
            {
                // A stale arm: the release happened off-strip (no capture below threshold), so we
                // never saw it. This press supersedes it.
                _TabReorderReset();
            }
            if (!_tabView)
            {
                return;
            }
            // Touch pans the strip natively — a touch drag-threshold would fight it. Mouse + pen only.
            if (e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch)
            {
                return;
            }
            const auto pt = e.GetCurrentPoint(_tabView);
            if (!pt.Properties().IsLeftButtonPressed())
            {
                return; // right = context menu, middle = close — not drags
            }
            // Resolve the pressed header: walk OriginalSource up to a TabViewItem, refusing when an
            // interactive control (the close button, the strip's scroll RepeatButtons) sits between —
            // those own their own press (and would capture the pointer themselves anyway).
            winrt::MUX::Controls::TabViewItem tvi{ nullptr };
            auto node = e.OriginalSource().try_as<winrt::WUX::DependencyObject>();
            while (node)
            {
                if (node.try_as<winrt::WUX::Controls::Primitives::ButtonBase>())
                {
                    return;
                }
                if (auto asTvi = node.try_as<winrt::MUX::Controls::TabViewItem>())
                {
                    tvi = asTvi;
                    break;
                }
                node = winrt::WUX::Media::VisualTreeHelper::GetParent(node);
            }
            if (!tvi)
            {
                return;
            }
            const auto tab = _GetTabByTabViewItem(tvi);
            if (!tab)
            {
                return;
            }
            if (_managerTab && tab == _managerTab)
            {
                return; // the pinned Manager tab is non-movable — never even arms
            }
            _tabReorder.armed = true;
            _tabReorder.pointerId = e.Pointer().PointerId();
            _tabReorder.pressPoint = pt.Position();
            _tabReorder.lastPoint = _tabReorder.pressPoint;
            _tabReorder.tab = tab;
            _tabReorder.tvi = tvi;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnPressed");
            _TabReorderReset();
        }
    }

    void TerminalPage::_TabReorderOnMoved(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if ((!_tabReorder.armed && !_tabReorder.active) || e.Pointer().PointerId() != _tabReorder.pointerId)
            {
                return;
            }
            const auto pt = e.GetCurrentPoint(_tabView);
            const auto pos = pt.Position();
            _tabReorder.lastPoint = pos;
            if (!_tabReorder.active)
            {
                if (!pt.Properties().IsLeftButtonPressed())
                {
                    // The button went up somewhere we never saw the release (below threshold there
                    // is no capture) — the arm is stale.
                    _TabReorderReset();
                    return;
                }
                if (!Agentmaster::TabDragThresholdCrossed(pos.X - _tabReorder.pressPoint.X,
                                                          pos.Y - _tabReorder.pressPoint.Y,
                                                          kTabDragThresholdDips))
                {
                    return;
                }
                _TabReorderActivate(e);
                if (!_tabReorder.active)
                {
                    return; // activation refused (tab vanished / capture failed)
                }
            }
            _TabReorderUpdate(pos);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnMoved");
            _TabReorderCancel(L"exception");
        }
    }

    // Threshold crossed: capture the pointer on the TabView, lift the tab, arm Esc + the
    // auto-scroll tick, and log the gesture BEGIN.
    void TerminalPage::_TabReorderActivate(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        const auto idx = _TabReorderIndexOf(_tabReorder.tab);
        if (idx < 0)
        {
            _TabReorderReset(); // the tab was closed between press and threshold
            return;
        }
        if (!_tabView.CapturePointer(e.Pointer()))
        {
            _TabReorderReset();
            return;
        }
        _tabReorder.active = true;
        _tabReorder.originalIndex = idx;
        _tabReorder.lastSlot = -1;
        // The native tear-out recipe's drag offset (position the new window under the cursor),
        // captured at activation exactly like _onTabDragStarting does.
        try
        {
            if (_hostingHwnd)
            {
                const auto inverseScale = 1.0f / static_cast<float>(_tabReorder.tvi.XamlRoot().RasterizationScale());
                POINT cursorPos;
                if (GetCursorPos(&cursorPos) && ScreenToClient(*_hostingHwnd, &cursorPos))
                {
                    _tabReorder.dragOffset = { cursorPos.x * inverseScale, cursorPos.y * inverseScale };
                }
            }
        }
        CATCH_LOG();
        ::Agentmaster::LogNav(L"tab-drag-begin " + _DescribeTabForLog(_tabReorder.tab));
        try
        {
            _tabReorder.tvi.Opacity(0.55); // the "lifted" cue; restored in _TabReorderReset
        }
        CATCH_LOG();
        _EnsureTabStripScrollViewer();
        // Esc cancels — a tunneling PreviewKeyDown on the page beats whichever control holds focus
        // (and keeps the Esc out of the focused terminal — a mid-drag Esc must not reach claude).
        _tabReorderKeyRevoker = Root().PreviewKeyDown(winrt::auto_revoke, [weak = get_weak()](const auto&, const winrt::WUX::Input::KeyRoutedEventArgs& k) {
            if (k.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                if (auto page = weak.get())
                {
                    if (page->_tabReorder.active)
                    {
                        k.Handled(true);
                        page->_TabReorderCancel(L"esc");
                    }
                }
            }
        });
        _tabDragAutoScrollTimer.Start();
    }

    // The per-move (and per-tick) decision pass: bail if the dragged tab vanished, edge
    // auto-scroll, then decide the insertion slot + place the caret (hidden in the tear-out zone —
    // the caret only ever promises a reorder).
    void TerminalPage::_TabReorderUpdate(const winrt::Windows::Foundation::Point& pos)
    {
        try
        {
            if (!_tabReorder.active || !_tabView)
            {
                return;
            }
            if (_TabReorderIndexOf(_tabReorder.tab) < 0)
            {
                _TabReorderCancel(L"tab closed mid-drag"); // e.g. the autorunner archived it under us
                return;
            }
            const double stripW = _tabView.ActualWidth();
            const double stripH = _tabView.ActualHeight();
            if (_tabStripScrollViewer)
            {
                const double step = Agentmaster::TabDragAutoScrollStep(pos.X, stripW, kTabDragEdgeBandDips, kTabDragScrollStepDips);
                if (step != 0.0)
                {
                    _tabStripScrollViewer.ChangeView(_tabStripScrollViewer.HorizontalOffset() + step, nullptr, nullptr, true);
                }
            }
            if (Agentmaster::ClassifyTabDragRelease(pos.X, pos.Y, stripW, stripH, kTabDragTearOutSlackDips) == Agentmaster::TabDragRelease::TearOut)
            {
                _tabReorder.lastSlot = -1; // a release here is a tear-out, not a reorder
                _HideTabDragIndicator();
                return;
            }
            const auto bands = _TabReorderBands();
            const int itemCount = static_cast<int>(_tabView.TabItems().Size());
            int slot = Agentmaster::DecideTabInsertionSlot(bands, pos.X, itemCount);
            if (slot < 0)
            {
                _HideTabDragIndicator(); // nothing realized to decide against — keep the previous slot
                return;
            }
            const int minSlot = _managerTab ? 1 : 0; // nothing inserts ahead of the pinned Manager tab
            slot = std::max(slot, minSlot);
            _tabReorder.lastSlot = slot;
            double boundaryX{};
            if (Agentmaster::InsertionSlotBoundaryX(bands, slot, boundaryX))
            {
                _PositionTabDragIndicator(boundaryX);
            }
            else
            {
                _HideTabDragIndicator();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderUpdate");
        }
    }

    void TerminalPage::_TabReorderOnReleased(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if (e.Pointer().PointerId() != _tabReorder.pointerId)
            {
                return;
            }
            if (!_tabReorder.active)
            {
                if (_tabReorder.armed)
                {
                    _TabReorderReset(); // a plain click — selection already happened at press
                }
                return;
            }
            const auto pos = e.GetCurrentPoint(_tabView).Position();
            _TabReorderCommitRelease(pos);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnReleased");
            _TabReorderCancel(L"exception");
        }
    }

    // Commit the gesture: snapshot what the release needs, RESET FIRST (visuals/capture/flags —
    // the strip mutations below rebuild containers and must run outside the gesture state), then
    // reorder or tear out.
    void TerminalPage::_TabReorderCommitRelease(const winrt::Windows::Foundation::Point& pos)
    {
        const auto tab = _tabReorder.tab;
        const int slot = _tabReorder.lastSlot;
        const int originalIndex = _tabReorder.originalIndex;
        const auto dragOffset = _tabReorder.dragOffset;
        double stripW = 0.0;
        double stripH = 0.0;
        try
        {
            stripW = _tabView ? _tabView.ActualWidth() : 0.0;
            stripH = _tabView ? _tabView.ActualHeight() : 0.0;
        }
        CATCH_LOG();
        _TabReorderReset();
        if (!tab)
        {
            return;
        }
        const int cur = _TabReorderIndexOf(tab);
        if (cur < 0)
        {
            ::Agentmaster::LogNav(L"tab-drag-end (tab closed mid-drag)");
            return;
        }
        if (Agentmaster::ClassifyTabDragRelease(pos.X, pos.Y, stripW, stripH, kTabDragTearOutSlackDips) == Agentmaster::TabDragRelease::TearOut)
        {
            // Tear out into a NEW window through the exact native downstream. _sendDraggedTabToWindow
            // consumes _stashed.draggedTab (BuildStartupActions + [nav] tab-send-to-window +
            // _DetachClaudeTabForMove + _DetachTabFromWindow + _MoveContent + _RemoveTab, which nulls
            // the stash again) — the same path the native TabDroppedOutside took.
            winrt::com_ptr<Tab> tabImpl;
            tabImpl.copy_from(winrt::get_self<Tab>(tab));
            if (!tabImpl)
            {
                ::Agentmaster::LogNav(L"tab-drag-end (tear-out refused: not a terminal tab)");
                return;
            }
            _stashed.draggedTab = tabImpl;
            _stashed.dragOffset = dragOffset;
            // The native release-point recipe (_onTabDroppedOutside): position the new window so the
            // tab lands under the cursor; unavailable => default placement.
            std::optional<winrt::Windows::Foundation::Point> adjusted;
            try
            {
                if (const auto coreWindow = CoreWindow::GetForCurrentThread())
                {
                    const auto pointerPos = coreWindow.PointerPosition();
                    adjusted = winrt::Windows::Foundation::Point{ pointerPos.X - dragOffset.X, pointerPos.Y - dragOffset.Y };
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_TabReorderCommitRelease pointer-position"); // fall through — default window placement
            }
            _sendDraggedTabToWindow(winrt::hstring{ L"-1" }, 0, adjusted);
            ::Agentmaster::LogNav(L"tab-drag-end (torn out to a new window)");
            return;
        }
        if (slot < 0)
        {
            ::Agentmaster::LogNav(L"tab-drag-end (no same-window reorder)");
            return;
        }
        const int minIndex = _managerTab ? 1 : 0;
        const int itemCount = static_cast<int>(_tabs.Size());
        const int target = Agentmaster::InsertionSlotToMoveTarget(slot, cur, minIndex, itemCount);
        if (target == cur)
        {
            ::Agentmaster::LogNav(L"tab-drag-end (no same-window reorder)");
            return;
        }
        _TryMoveTab(static_cast<uint32_t>(cur), target); // logs [nav] tab-move + UIA-announces + settles
        ::Agentmaster::LogNav(L"tab-drag-end from=" + std::to_wstring(originalIndex >= 0 ? originalIndex : cur) + L" to=" + std::to_wstring(target));
    }

    // External capture loss (alt-tab, a modal stealing the pointer, the window deactivating): the
    // gesture cannot continue — cancel WITHOUT mutating (a half-seen drag must never guess a drop).
    // Our own ReleasePointerCaptures in _TabReorderReset re-enters here, guarded by active==false
    // (reset clears the flags first).
    void TerminalPage::_TabReorderOnCaptureLost(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if (!_tabReorder.active || e.Pointer().PointerId() != _tabReorder.pointerId)
            {
                return;
            }
            _TabReorderCancel(L"capture lost");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnCaptureLost");
        }
    }

    void TerminalPage::_TabReorderCancel(const wchar_t* reason)
    {
        if (!_tabReorder.armed && !_tabReorder.active)
        {
            return;
        }
        const bool wasActive = _tabReorder.active;
        _TabReorderReset();
        if (wasActive)
        {
            // Caret-commit design: nothing moved during the gesture, so cancel == simply no move.
            ::Agentmaster::LogNav(std::wstring{ L"tab-drag-end (cancelled: " } + reason + L")");
        }
    }

    // Common teardown. Flags drop FIRST so the CaptureLost our own ReleasePointerCaptures raises
    // no-ops; every visual restore is individually guarded (a mid-teardown tree never blocks the
    // state clear).
    void TerminalPage::_TabReorderReset()
    {
        _tabReorder.armed = false;
        _tabReorder.active = false;
        _tabReorder.lastSlot = -1;
        _tabReorder.originalIndex = -1;
        _tabReorder.pointerId = 0;
        _tabDragAutoScrollTimer.Stop();
        _tabReorderKeyRevoker.revoke();
        try
        {
            if (_tabReorder.tvi)
            {
                _tabReorder.tvi.Opacity(1.0);
            }
        }
        CATCH_LOG();
        _HideTabDragIndicator();
        try
        {
            if (_tabView)
            {
                _tabView.ReleasePointerCaptures();
            }
        }
        CATCH_LOG();
        _tabReorder.tab = { nullptr };
        _tabReorder.tvi = { nullptr };
    }

    // The tab's CURRENT index in _tabs (identity compare), -1 when it is gone — indexes are
    // re-resolved at every decision point because tabs open/close/move under the gesture (the
    // autorunner, another window's cross-window move, …).
    int TerminalPage::_TabReorderIndexOf(const winrt::TerminalApp::Tab& tab) const
    {
        if (!tab || !_tabs)
        {
            return -1;
        }
        uint32_t idx{};
        if (_tabs.IndexOf(tab, idx))
        {
            return static_cast<int>(idx);
        }
        return -1;
    }

    // The realized, on-strip header bands in TabView coordinates — the gesture's whole world.
    // Virtualized-out tabs have no container and therefore NO band (never touched — the exact
    // hazard that killed the native drag is structurally absent here).
    std::vector<::Agentmaster::TabDragBand> TerminalPage::_TabReorderBands()
    {
        std::vector<::Agentmaster::TabDragBand> bands;
        if (!_tabView)
        {
            return bands;
        }
        const double stripW = _tabView.ActualWidth();
        const auto n = _tabView.TabItems().Size();
        bands.reserve(32);
        for (uint32_t i = 0; i < n; ++i)
        {
            const auto container = _tabView.ContainerFromIndex(i).try_as<winrt::MUX::Controls::TabViewItem>();
            if (!container)
            {
                continue; // virtualized out
            }
            const double w = container.ActualWidth();
            if (w <= 0.0)
            {
                continue; // realized but not laid out yet
            }
            winrt::Windows::Foundation::Point origin{};
            try
            {
                origin = container.TransformToVisual(_tabView).TransformPoint({ 0.0f, 0.0f });
            }
            catch (...)
            {
                continue; // expected control flow: a container detached mid-recycle has no transform (hot per-move loop — deliberately unlogged)
            }
            const double left = origin.X;
            const double right = left + w;
            if (right < 0.0 || left > stripW)
            {
                continue; // realized (cache buffer) but off the visible strip
            }
            bands.push_back({ static_cast<int>(i), left, right });
        }
        return bands; // ascending index == ascending x on the LTR strip
    }

    // The insertion caret: a thin accent bar parented into the TABVIEW'S TEMPLATE ROOT GRID —
    // deliberately NOT Root(): with showTabsInTitlebar (the default) the tab row is REMOVED from
    // Root() and re-homed into the titlebar (TerminalPage::Create -> SetTitleBarContent), so a
    // Root()-hosted caret would position against a collapsed row in a different subtree. The
    // template root travels WITH the TabView in both hosting modes, and coordinates stay local
    // (the root fills the TabView, so TabView-x == host-x up to the identity transform, which we
    // still apply for exactness). Root() is the fallback when the template isn't realized yet.
    // Created lazily; positioned only from pointer/timer code — never from a layout callback (the
    // E_LAYOUTCYCLE class). IsHitTestVisible(false) so it can never eat the release click.
    void TerminalPage::_EnsureTabDragIndicator()
    {
        try
        {
            // Resolve the host panel (re-resolved if the previous host went away with a template
            // reapply — the caret is re-parented rather than left dangling in a dead subtree).
            winrt::WUX::Controls::Panel host{ nullptr };
            if (_tabView && winrt::WUX::Media::VisualTreeHelper::GetChildrenCount(_tabView) > 0)
            {
                host = winrt::WUX::Media::VisualTreeHelper::GetChild(_tabView, 0).try_as<winrt::WUX::Controls::Panel>();
            }
            if (!host)
            {
                host = Root();
            }
            if (_tabDragIndicator && _tabDragIndicatorHost == host)
            {
                return; // already built + still parented right
            }
            if (_tabDragIndicator && _tabDragIndicatorHost)
            {
                // host changed (template reapplied) — move the caret
                uint32_t idx{};
                if (_tabDragIndicatorHost.Children().IndexOf(_tabDragIndicator, idx))
                {
                    _tabDragIndicatorHost.Children().RemoveAt(idx);
                }
                _tabDragIndicator = nullptr;
            }
            winrt::WUX::Controls::Border bar;
            bar.Width(3.0);
            bar.HorizontalAlignment(winrt::WUX::HorizontalAlignment::Left);
            bar.VerticalAlignment(winrt::WUX::VerticalAlignment::Top);
            bar.CornerRadius(winrt::WUX::CornerRadius{ 1.5, 1.5, 1.5, 1.5 });
            bar.IsHitTestVisible(false);
            bar.Visibility(winrt::WUX::Visibility::Collapsed);
            auto accent = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3B, 0x82, 0xF6); // fallback blue
            try
            {
                if (const auto res = winrt::WUX::Application::Current().Resources().TryLookup(winrt::box_value(L"SystemAccentColor")))
                {
                    accent = winrt::unbox_value_or<winrt::Windows::UI::Color>(res, accent);
                }
            }
            CATCH_LOG();
            bar.Background(winrt::WUX::Media::SolidColorBrush{ accent });
            winrt::WUX::Controls::Canvas::SetZIndex(bar, 1000); // above the strip chrome
            host.Children().Append(bar);
            _tabDragIndicator = bar;
            _tabDragIndicatorHost = host;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_EnsureTabDragIndicator"); // gesture still works, just caret-less
        }
    }

    void TerminalPage::_PositionTabDragIndicator(double xInTabView)
    {
        _EnsureTabDragIndicator();
        if (!_tabDragIndicator || !_tabDragIndicatorHost || !_tabView)
        {
            return;
        }
        try
        {
            const auto origin = _tabView.TransformToVisual(_tabDragIndicatorHost).TransformPoint({ static_cast<float>(xInTabView), 0.0f });
            _tabDragIndicator.Height(std::max(8.0, _tabView.ActualHeight()));
            _tabDragIndicator.Margin(winrt::WUX::Thickness{ origin.X - 1.5, origin.Y, 0.0, 0.0 });
            _tabDragIndicator.Visibility(winrt::WUX::Visibility::Visible);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PositionTabDragIndicator");
        }
    }

    void TerminalPage::_HideTabDragIndicator()
    {
        try
        {
            if (_tabDragIndicator)
            {
                _tabDragIndicator.Visibility(winrt::WUX::Visibility::Collapsed);
            }
        }
        CATCH_LOG();
    }

    // Agentmaster (M10 Increment 3): TerminalWindow hands us the record id it resolved from the
    // Emperor's -s <idx> (multi-window reopen), so _InitAgentmasterEngine claims THAT record (its
    // geometry already applied by TerminalWindow) instead of the front one. Must be set before
    // _OnFirstLayout. Empty => single-window (claim the front record).
    void TerminalPage::SetAgentmasterWindowId(winrt::hstring windowId)
    {
        _assignedWindowId = windowId;
    }

    // Agentmaster: TerminalWindow flags a window built from MOVED content (tab tear-out into a new
    // window, or a cross-window tab move) BEFORE _OnFirstLayout, so _InitAgentmasterEngine mints a
    // fresh window record for it instead of front-popping a leftover one (which would drop the moved
    // session — see _isContentWindow + the claim block in _InitAgentmasterEngine).
    void TerminalPage::SetAgentmasterContentWindow(bool isContentWindow)
    {
        _isContentWindow = isContentWindow;
    }
}
