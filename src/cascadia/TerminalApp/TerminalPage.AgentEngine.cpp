// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
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
#include "TabHeaderControl.h" // Agentmaster: SetTabRenameCommitMode (push the GLOBAL rename-commit mode to tab headers)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / AgentmasterStateDir / MaterializeSharedHookFiles
#include "AgentMaster/Engine.h" // SharedEngine / ClaimWindowRecord / Register-UnregisterLiveWindow
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings
#include "AgentMaster/ProcessObserver.h" // UnpublishWindow (teardown; Rule #10)
#include "AgentMaster/Scheduler.h" // SetGlobalPause / Confirm (Manager callbacks)
#include "AgentMaster/SessionRegistry.h" // adoption-handler + registry-observer tokens
#include "AgentMaster/SessionScanner.h" // liveness-probe token

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
        if (!_windowRecordTeardownFlushed)
        {
            _FlushWindowRecord();
            _windowRecordTeardownFlushed = true;
        }

        // Agentmaster (lifecycle gap #1): archive any of this window's still-live sessions before we
        // detach. Normally CloseWindow already did this (deterministically, before raising
        // CloseWindowRequested); this is the catch-all for quit-all / any teardown path that bypassed
        // it. Idempotent — a no-op if CloseWindow already cleared _claudeTabs.
        _ArchiveWindowSessionsOnTeardown();

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
        // Agentmaster (alt+up/down prompt nav): stop the 30 s focused-refresh timer (UI thread, safe).
        if (_promptNavRefreshTimer)
        {
            _promptNavRefreshTimer.Stop();
        }
        // Agentmaster (cross-window activate): drop this window's activate sink from the shared
        // engine — a stray fan-out after teardown is already a safe no-op (the sink captures
        // get_weak() + an agile dispatcher), this keeps the engine's sink list bounded (Rule #10).
        if (_windowActivateToken)
        {
            ::Agentmaster::UnregisterWindowActivateHandler(_windowActivateToken);
        }
        // Agentmaster (cross-window restart): drop this window's restart sink too (Rule #10).
        if (_windowRestartToken)
        {
            ::Agentmaster::UnregisterWindowRestartHandler(_windowRestartToken);
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
        // (board/tree double-click, tree Enter, the Flight Plan's eye) targets a session hosted
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
            // never ping-pong). The RunAsync coalesces bursts; the title MUST be re-read on the UI
            // thread, NOT captured here, or a stale snapshot races a concurrent rename and the
            // directions oscillate (the /clear re-home title-swap + [Unknown] flood). Token detached
            // in ~TerminalPage (Rule #10 — a closed window's observer must not linger on the registry).
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _agentDotObserverToken = _sessionRegistry->AddObserver([weakThis, dispatcher](const ::Agentmaster::SessionInfo& s, ::Agentmaster::HookEvent) {
                const std::wstring id = s.id;
                const auto state = s.state;
                const bool live = s.live;
                // Agentmaster (eager-init): a live MANAGED session whose ConPTY hasn't started yet (a
                // window-restored / re-homed tab the user never clicked) shows the half-hollow dot. An
                // external (observe-only) session has no control of ours, so it is never "dormant".
                const bool dormant = live && !s.started && !s.external;
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis, id, state, live, dormant]() {
                    if (auto self = weakThis.get())
                    {
                        self->_UpdateTabAgentDot(id, state, live, dormant);
                        self->_SyncClaudeTabTitleFromRegistry(id); // reads the CURRENT registry title (no stale capture)
                    }
                });
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
        content->SetForkManagedSessionHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ForkManagedSessionById(std::wstring{ id });
            }
        });
        content->SetRenameHandler([weakThis](winrt::hstring id, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_RenameClaudeSession(id, title);
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
                    // Updater (Updater.h): skip/postpone are written outside the cog form (a JSON RMW
                    // from the prompt, possibly from another window or the startup check). The form
                    // owns ONLY allowUpdatePrerelease, so preserve these two from disk on Save.
                    s.updateSkippedVersion = disk.updateSkippedVersion;
                    s.updatePostponedUntilUnixMs = disk.updatePostponedUntilUnixMs;
                }
                self->_appSettings = s;
                ::Agentmaster::SaveAppSettings(s);
                // Apply the (possibly changed) GLOBAL rename-commit mode to every window's tab
                // headers immediately (process-wide static), not just next launch.
                SetTabRenameCommitMode(static_cast<int32_t>(s.tabRenameCommitMode));
                // Apply the (possibly changed) tab-strip close affordances (show-X / middle-click
                // close) to THIS window's tabs immediately; other windows get them via the broadcast.
                self->_updateAllTabCloseButtons();
                // Apply the (possibly changed) "Always display Home button" setting to THIS window's
                // tab-strip nav buttons immediately; other windows get it via the broadcast.
                self->_UpdateManagerNavButtons();
                // FAVORITES.md §5a: the favorite marker (Crown <-> Star) is GLOBAL — re-assert it on every
                // hosted favorited tab so a glyph change takes effect now; other windows get it via the broadcast.
                self->_RefreshAllFavoriteIcons();
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
    // its Flight Plan). Equivalent to a single-click on the session's board card. Called from the one
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
        _appSettings = settings;
        // Agentmaster: the tab-strip close affordances (show-X / middle-click close) are GLOBAL, so a
        // change made in another window must re-apply to THIS window's tabs live (the source window
        // already did so in its Save handler).
        _updateAllTabCloseButtons();
        // Agentmaster: "Always display Home button" is GLOBAL too — re-evaluate this window's nav buttons.
        _UpdateManagerNavButtons();
        // FAVORITES.md §5a: the favorite marker (Crown <-> Star) is GLOBAL — re-assert it on every hosted
        // favorited tab so a change made in another window switches the glyph live here too.
        _RefreshAllFavoriteIcons();
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
                    _tabView.TabItems().RemoveAt(viewIdx);
                    _tabView.TabItems().InsertAt(0, tvi);
                }
            }
            CATCH_LOG();
            _UpdateTabIndices();
        }
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
