// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#include "pch.h"
#include "ColorPickupFlyout.h"
#include "Tab.h"
#include "SettingsPaneContent.h"
#include "Tab.g.cpp"
#include "TabHeaderControl.h" // Agentmaster (bookmark tags): get_self — the badge-hover til::events aren't projected
#include "AgentCatchLog.h" // Agentmaster: AgentLogCaughtException — the tooltip hover/wheel hooks swallow, so they must still report (Rule #18)
#include "AgentModelMenu.h" // Agentmaster (launch-model picker): AgentFillModelPickItems — the shared "New Session Here ▸ <model>" submenu recipe
#include "Utils.h"
#include "AppLogic.h"
#include "../../types/inc/ColorFix.hpp"

#include <chrono> // DispatcherTimer intervals + til::throttled_func delays

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::System;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
}

#define ASSERT_UI_THREAD() assert(TabViewItem().Dispatcher().HasThreadAccess())

namespace winrt::TerminalApp::implementation
{
    Tab::Tab(std::shared_ptr<Pane> rootPane)
    {
        _rootPane = rootPane;
        _activePane = nullptr;

        _closePaneMenuItem.Visibility(WUX::Visibility::Collapsed);

        auto firstId = _nextPaneId;

        _rootPane->WalkTree([&](const auto& pane) {
            // update the IDs on each pane
            if (pane->_IsLeaf())
            {
                pane->Id(_nextPaneId);
                _nextPaneId++;
            }
            // Try to find the pane marked active (if it exists)
            if (pane->_lastActive)
            {
                _activePane = pane;
            }
        });

        // In case none of the panes were already marked as the focus, just
        // focus the first one.
        if (_activePane == nullptr)
        {
            const auto firstPane = _rootPane->FindPane(firstId);
            firstPane->SetActive();
            _activePane = firstPane;
        }
        // If the focused pane is a leaf, add it to the MRU panes
        if (const auto id = _activePane->Id())
        {
            _mruPanes.insert(_mruPanes.begin(), id.value());
        }

        _Setup();
    }

    // Method Description:
    // - Shared setup for the constructors. Assumed that _rootPane has been set.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_Setup()
    {
        _rootClosedToken = _rootPane->Closed([=](auto&& /*s*/, auto&& /*e*/) {
            Closed.raise(nullptr, nullptr);
        });

        Content(_rootPane->GetRootElement());

        _MakeTabViewItem();
        _CreateContextMenu();
        _UpdateMenuItemStates();

        _headerControl.TabStatus(_tabStatus);

        // Add an event handler for the header control to tell us when they want their title to change
        _headerControl.TitleChangeRequested([weakThis = get_weak()](auto&& title) {
            if (auto tab{ weakThis.get() })
            {
                tab->SetTabText(title);
            }
        });

        // GH#9162 - when the header is done renaming, ask for focus to be
        // tossed back to the control, rather into ourselves.
        _headerControl.RenameEnded([weakThis = get_weak()](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->RequestFocusActiveControl.raise();
            }
        });

        // Agentmaster (bookmark tags): forward the header's per-badge hover begin/end up to the page,
        // which owns the rich tag hover panel (the sessions carrying that tag, click == jump to its
        // tab). The header control is a leaf — it knows badges, not the registry — so it only
        // announces "the pointer is over tag T of this tab" and the page does the data + popup work.
        // Via get_self: these are impl-side til::events (not projected — same-module access, like the
        // page's own _GetTabImpl calls onto us).
        if (const auto headerImpl = winrt::get_self<TabHeaderControl>(_headerControl))
        {
            headerImpl->TagBadgeHoverBegin([weakThis = get_weak()](const winrt::hstring& tag, const WUX::UIElement& anchor) {
                if (auto tab{ weakThis.get() })
                {
                    tab->TagBadgeHoverBegin.raise(tag, anchor);
                }
            });
            headerImpl->TagBadgeHoverEnd([weakThis = get_weak()]() {
                if (auto tab{ weakThis.get() })
                {
                    tab->TagBadgeHoverEnd.raise();
                }
            });
        }

        _UpdateHeaderControlMaxWidth();

        // Use our header control as the TabViewItem's header
        TabViewItem().Header(_headerControl);
    }

    // Method Description:
    // - Called when the timer for the bell indicator in the tab header fires
    // - Removes the bell indicator from the tab header
    // Arguments:
    // - sender, e: not used
    void Tab::_BellIndicatorTimerTick(const Windows::Foundation::IInspectable& /*sender*/, const Windows::Foundation::IInspectable& /*e*/)
    {
        ShowBellIndicator(false);
        _bellIndicatorTimer.Stop();
    }

    // Method Description:
    // - Initializes a TabViewItem for this Tab instance.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_MakeTabViewItem()
    {
        TabViewItem(::winrt::MUX::Controls::TabViewItem{});

        // GH#3609 If the tab was tapped, and no one else was around to handle
        // it, then ask our parent to toss focus into the active control.
        TabViewItem().Tapped([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->RequestFocusActiveControl.raise();
            }
        });

        // BODGY: When the tab is drag/dropped, the TabView gets a
        // TabDragStarting. However, the way it is implemented[^1], the
        // TabViewItem needs either an Item or a Content for the event to
        // include the correct TabViewItem. Otherwise, it will just return the
        // first TabViewItem in the TabView with the same Content as the dragged
        // tab (which, if the Content is null, will be the _first_ tab).
        //
        // So here, we'll stick an empty border in, just so that every tab has a
        // Content which is not equal to the others.
        //
        // [^1]: microsoft-ui-xaml/blob/92fbfcd55f05c92ac65569f5d284c5b36492091e/dev/TabView/TabView.cpp#L751-L758
        TabViewItem().Content(winrt::WUX::Controls::Border{});

        TabViewItem().DoubleTapped([weakThis = get_weak()](auto&& /*s*/, auto&& /*e*/) {
            if (auto tab{ weakThis.get() })
            {
                tab->ActivateTabRenamer();
            }
        });

        UpdateTitle();
        _RecalculateAndApplyTabColor();
    }

    void Tab::_UpdateHeaderControlMaxWidth()
    {
        try
        {
            // Make sure to try/catch this, because the LocalTests won't be
            // able to use this helper.
            const auto settings{ winrt::TerminalApp::implementation::AppLogic::CurrentAppSettings() };
            if (settings.GlobalSettings().TabWidthMode() == winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode::SizeToContent)
            {
                _headerControl.RenamerMaxWidth(HeaderRenameBoxWidthTitleLength);
            }
            else
            {
                _headerControl.RenamerMaxWidth(HeaderRenameBoxWidthDefault);
            }
        }
        CATCH_LOG()
    }

    void Tab::SetDispatch(const winrt::TerminalApp::ShortcutActionDispatch& dispatch)
    {
        ASSERT_UI_THREAD();

        _dispatch = dispatch;
    }

    void Tab::SetActionMap(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap)
    {
        ASSERT_UI_THREAD();

        _actionMap = actionMap;
        _UpdateSwitchToTabKeyChord();
    }

    // Method Description:
    // - Sets the key chord resulting in switch to the current tab.
    // Updates tool tip if required
    // Arguments:
    // - keyChord - string representation of the key chord that switches to the current tab
    // Return Value:
    // - <none>
    void Tab::_UpdateSwitchToTabKeyChord()
    {
        const auto id = fmt::format(FMT_COMPILE(L"Terminal.SwitchToTab{}"), _TabViewIndex);
        const auto keyChord{ _actionMap.GetKeyBindingForAction(id) };
        const auto keyChordText = keyChord ? KeyChordSerialization::ToString(keyChord) : L"";

        if (_keyChord == keyChordText)
        {
            return;
        }

        _keyChord = keyChordText;
        _UpdateToolTip();
    }

    // Method Description:
    // - Sets tab tool tip to a concatenation of title and key chord
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_UpdateToolTip()
    {
        // Agentmaster (tab tooltip): when TerminalPage has pushed a rich session tooltip, render that
        // instead of the default title + key-chord (ClearAgentToolTip reverts to this default path).
        if (_agentToolTipActive)
        {
            _UpdateAgentToolTip();
            return;
        }

        auto titleRun = WUX::Documents::Run();
        titleRun.Text(_CreateToolTipTitle());

        auto textBlock = WUX::Controls::TextBlock{};
        textBlock.TextWrapping(WUX::TextWrapping::Wrap);
        textBlock.TextAlignment(WUX::TextAlignment::Center);
        textBlock.Inlines().Append(titleRun);

        if (!_keyChord.empty())
        {
            auto keyChordRun = WUX::Documents::Run();
            keyChordRun.Text(_keyChord);
            keyChordRun.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            textBlock.Inlines().Append(WUX::Documents::LineBreak{});
            textBlock.Inlines().Append(keyChordRun);
        }

        WUX::Controls::ToolTip toolTip{};
        // Agentmaster: pin the tooltip Dark, exactly as _UpdateAgentToolTip does for managed tabs. This
        // is the DEFAULT tab-tooltip path — shell (pwsh/cmd) tabs, the Manager tab, and any tab before a
        // rich session tooltip is pushed — and it was the lone tab tooltip left rendering at the popup
        // root's (light) theme: a ToolTip lives in the popup root and does NOT inherit the host theme, and
        // ToolTipService theme propagation is unreliable under XAML Islands (see AgentTipHelpers /
        // _UpdateAgentToolTip), so a non-managed tab's title tooltip flashed light over the dark strip
        // while managed tabs' tooltips were dark. Pin it so every tab's tooltip matches.
        toolTip.RequestedTheme(WUX::ElementTheme::Dark);
        toolTip.Content(textBlock);
        WUX::Controls::ToolTipService::SetToolTip(TabViewItem(), toolTip);
    }

    // Agentmaster (tab tooltip): accept a rich, session-aware tooltip ELEMENT from TerminalPage (the
    // page owns the SessionInfo + the registry, so it builds the whole summary-style card; the Tab only
    // hosts it). `signature` is a cheap content fingerprint the page computes — an identical push is a
    // no-op (so the hover-time rebuild, the bind tail, and the observe-badge probe can all re-assert it
    // without re-hosting XAML; content is LAZY/hover-built now — the old per-tick sweep re-assert is
    // gone). Hosting the element is deferred to _UpdateToolTip (which also owns the default-tooltip
    // fallback path). UI thread only.
    void Tab::SetAgentToolTip(winrt::Windows::UI::Xaml::UIElement content, winrt::hstring signature, bool swapWhileOpen)
    {
        ASSERT_UI_THREAD();
        if (!content)
        {
            return;
        }
        if (_agentToolTipActive && signature == _agentToolTipSig && _agentToolTip)
        {
            return; // identical content already shown — don't re-host the XAML
        }
        // `&& _agentToolTip` is load-bearing, not defensive: an owner recycle DETACHES + nulls the
        // tooltip (the crash #4 fix), and if the content signature happens to be unchanged since then,
        // this early-out used to skip _UpdateToolTip and leave the tab with NO tooltip attached at all
        // until some unrelated field changed. Attachment is the thing that matters — see the
        // Loaded-rehost comment in _WireAgentToolTipUnload.

        _agentToolTipActive = true;
        _agentToolTipContent = std::move(content);
        _agentToolTipSig = std::move(signature);
        _agentToolTipSwapOpenOnce = swapWhileOpen; // one-shot, consumed (and always cleared) by _UpdateAgentToolTip
        _UpdateToolTip();
    }

    // Agentmaster (tab tooltip): revert to the default title + key-chord tooltip (a session went away /
    // was archived, or the tab is no longer a managed/observed agent tab). No-op if none was set.
    void Tab::ClearAgentToolTip()
    {
        ASSERT_UI_THREAD();
        if (!_agentToolTipActive)
        {
            return;
        }
        _agentToolTipActive = false;
        _agentToolTipContent = nullptr;
        _agentToolTipSig = {};
        _DetachAgentToolTip(); // drop the framework-managed tooltip + detach it from the owner (the framework closes any open popup on its own)
        _UpdateToolTip(); // revert to the default title + key-chord tooltip (re-attaches the default)
    }

    // Agentmaster (tab tooltip): host the rich tooltip element TerminalPage built (a dark, summary-style
    // card — see TerminalPage::_UpdateTabAgentToolTip). The page owns the layout + content; the Tab owns
    // the ToolTip lifecycle. FRAMEWORK-MANAGED: ToolTipService owns open/close (hover opens after the
    // system delay, pointer-exit closes) — the SAME model as the plain default tab tooltip (_UpdateToolTip),
    // which has NEVER crashed. We never drive IsOpen; the six historical tooltip crashes were all on the old
    // manual-open path (timers + IsOpen + a cross-tab cooldown), now removed — see
    // doc/agentmaster/HANDOVER_tab-tooltip.md.
    //
    // Reuse ONE ToolTip object across refreshes and swap its Content ONLY while closed: re-creating it +
    // re-SetToolTip on each ~2s data refresh would replace — and so flicker/close — the framework's open
    // tip while you hover it (made constant by the seconds ticking in the 'ago' line). Configure it once:
    // pinned Dark (a ToolTip renders in the popup root and does NOT inherit the host theme, and
    // ToolTipService theme propagation is unreliable under XAML Islands — see AgentTipHelpers), placed BELOW
    // the tab (the default placement would push it up into the titlebar / off the top of the screen), and
    // hit-test-invisible (a big card sitting under the cursor as a pointer target would churn the
    // framework's own hover tracking).
    void Tab::_UpdateAgentToolTip()
    {
        // Never create OR mutate the tooltip for a detached owner. If the TabViewItem has left the visual
        // tree (a MUX TabView recycle), building + SetToolTip + `.Content()` here is at best wasted and at
        // worst touches a torn-down peer — skip until the owner is live again (the next ~2s refresh / the
        // state-line "ago" rollover rebuilds it). A normal loaded tab (foreground OR a background tab in the
        // strip) is always IsLoaded()+rooted, so steady-state is unaffected. Complements the Unloaded detach
        // (_WireAgentToolTipUnload -> _DetachAgentToolTip): a recycle drops the ref so a later RELOAD of the
        // same owner rebuilds a FRESH tooltip here instead of reusing a stale one (the crash #4 lesson).
        // Consume the one-shot FIRST (even the early-outs below must clear it — a swap-while-open grant
        // that survived a skipped refresh would leak onto a later, unrelated content push).
        const bool swapOpenOnce = std::exchange(_agentToolTipSwapOpenOnce, false);
        const auto owner = TabViewItem();
        if (!owner || !owner.IsLoaded() || !owner.XamlRoot())
        {
            return;
        }
        if (!_agentToolTip)
        {
            _agentToolTip = WUX::Controls::ToolTip{};
            _agentToolTip.RequestedTheme(WUX::ElementTheme::Dark);
            _agentToolTip.Placement(WUX::Controls::Primitives::PlacementMode::Bottom);
            // Make the tooltip popup click/hover-THROUGH: a hit-testable popup becomes a pointer target, so
            // the large card sitting under the cursor would churn the framework's own hover tracking. A
            // tooltip is purely informational and never needs input (the AgentTipHelpers recipe does the same).
            _agentToolTip.IsHitTestVisible(false);
            WUX::Controls::ToolTipService::SetToolTip(owner, _agentToolTip);
            _WireAgentToolTipUnload(); // wire (once) the owner Unloaded -> detach, so a recycle can't strand a stale ref
        }

        // Don't swap Content while the framework has the tip OPEN (you're reading it): swapping under the
        // pointer flickers. Reading IsOpen is safe — only DRIVING it ever raced. ONE exception, opt-in per
        // push (_agentToolTipSwapOpenOnce, consumed at entry above): the async summary-body arrival — the
        // hovering user is WAITING for that content, so a single in-place swap is the desired behavior.
        // (The old per-tick ago-churn this rule was written against is gone — content is hover-built now.)
        if (_agentToolTip.IsOpen() && !swapOpenOnce)
        {
            return;
        }
        // Anchor the popup to the TAB, not the cursor (a CLOSED-only mutation, unlike Content below): a
        // hover-opened AUTOMATIC tooltip places itself relative to the POINTER — Placement(Bottom) alone
        // reads "below the cursor", so the card landed wherever inside the tab the mouse happened to sit
        // (the old manual path opened programmatically, which the framework places target-relative; going
        // framework-managed changed the anchor). An explicit PlacementRect (the tab's own bounds, in the
        // placement target's coordinate space) overrides pointer placement, so Placement(Bottom) centers
        // the card directly under the TAB. Re-asserted each closed refresh — tab widths drift with tab
        // add/remove/rename and window resize; while OPEN we leave placement alone (moving a popup the
        // user is reading would make it jump).
        if (!_agentToolTip.IsOpen())
        {
            if (const auto w = static_cast<float>(owner.ActualWidth()), h = static_cast<float>(owner.ActualHeight()); w > 0 && h > 0)
            {
                _agentToolTip.PlacementRect(winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Rect>{ winrt::Windows::Foundation::Rect{ 0, 0, w, h } });
            }
        }
        _agentToolTip.Content(_agentToolTipContent); // host the page-built card on the reused object
    }

    // Agentmaster (tab tooltip): FRAMEWORK-MANAGED lifecycle. ToolTipService owns open (hover, after the
    // system delay) and close (pointer-exit) — the SAME model as the plain default tab tooltip
    // (_UpdateToolTip), which has NEVER crashed. The ONLY handler we wire is the owner's Unloaded, so a MUX
    // TabView recycle/detach can't leave us holding a stale ToolTip ref: on unload we detach + drop it, and
    // the next content refresh rebuilds a FRESH one bound to the live (reloaded) owner (the crash #4 lesson,
    // kept — but now with NO manual IsOpen, so it cannot fail-fast). Wired ONCE per tab.
    //
    // The historical manual-open path (a fast-open one-shot timer, an 8s auto-dismiss backstop, three
    // pointer-loss close handlers, PointerMoved keep-alive/re-open, a cross-tab reopen cooldown, and
    // _SafeSetAgentToolTipOpen driving IsOpen) is GONE — it was the source of ALL SIX tooltip crashes
    // (doc/agentmaster/HANDOVER_tab-tooltip.md §9). The tradeoff: the tip now opens at the system hover
    // delay instead of ~1/3 of it (this SDK exposes no ToolTipService.InitialShowDelay to tune).
    void Tab::_WireAgentToolTipUnload()
    {
        if (_agentToolTipUnloadWired)
        {
            return;
        }
        const auto tvi = TabViewItem();
        if (!tvi)
        {
            return;
        }
        _agentToolTipUnloadWired = true;
        const auto weakThis = get_weak();

        // The owner TabViewItem leaving the visual tree (MUX TabView recycles its containers — heavy during
        // a multi-tab window restore) tears down the ToolTip's native peer while our WinRT strong ref keeps
        // resolving non-null — a zombie. If the SAME owner later RELOADS, reusing that stale ref to swap
        // .Content() would read freed memory (crash #4). So on unload, detach the tooltip from the owner +
        // drop our ref; _UpdateAgentToolTip then rebuilds a fresh one on the next refresh. We drive NO
        // IsOpen here (the framework closes any open popup on exit/recycle itself), so this can't fail-fast.
        tvi.Unloaded([weakThis](auto&&, auto&&) {
            if (const auto self = weakThis.get())
            {
                self->_DetachAgentToolTip();
            }
        });

        // ...and the other half of that trade, which was MISSING and is the root cause of "the tab
        // tooltip only shows sometimes, and its wheel scrolling never works":
        //
        // ToolTipService subscribes to the OWNER's PointerEntered inside RegisterToolTip — i.e. at the
        // moment the tooltip is ATTACHED (dxaml ToolTipService_Partial.cpp: RegisterToolTip ->
        // add_PointerEntered). A tooltip attached DURING a dwell therefore never sees that dwell's
        // PointerEntered, so its open timer never starts and it does not open for that hover. The
        // Unloaded handler above nulls our tooltip on every MUX container recycle (which is constant on
        // a 50-tab strip), and re-attachment only happened on the NEXT content push — which, since the
        // card went lazy/hover-built, IS mid-dwell. Net effect: after any recycle, the next hover showed
        // nothing (or a leftover popup from an earlier dwell), and our ToolTip's IsOpen was honestly
        // false — which is exactly what the wheel gate reported (hooks.log "[tooltip-wheel] item: cb=1
        // tip=0/1 open=0", every single sample).
        //
        // So re-attach as soon as the owner is back in the tree, BEFORE any pointer can enter it.
        tvi.Loaded([weakThis](auto&&, auto&&) {
            if (const auto self = weakThis.get())
            {
                if (self->_agentToolTipActive && !self->_agentToolTip && self->_agentToolTipContent)
                {
                    self->_UpdateAgentToolTip(); // re-create + SetToolTip with the content we already have
                }
            }
        });
    }

    // Agentmaster (LAZY tab tooltip — the CPU fix): wire (once per tab) the owner TabViewItem's
    // PointerEntered to the page's build-now callback. PointerEntered fires the moment the pointer
    // enters the tab header — well BEFORE ToolTipService's open delay — so the page's rebuild +
    // Content swap still happen while the tip is CLOSED (the safe mutation window _UpdateAgentToolTip
    // enforces). This replaces the old model where the page rebuilt EVERY managed tab's card on every
    // ~2.5s sweep tick (70-tab strip: a constant stream of XAML card builds nobody ever saw — the
    // measured UI-thread hotspot); now a card is built only when a human is actually about to see it.
    // The handler drives NO IsOpen and hosts nothing itself — it only invokes the page callback, which
    // funnels through the same SetAgentToolTip/_UpdateAgentToolTip path as before. Returns true only
    // the ONE time the hook is wired, so the caller can arm-build once (the tooltip must be hosted
    // BEFORE the first hover for ToolTipService to open it on that hover). Like _WireAgentToolTipUnload,
    // the handler lives on the tab's own TabViewItem (stable across MUX recycles) and weak-captures the
    // Tab, so teardown order is safe; the callback itself weak-captures the page (set once, cleared on
    // Shutdown so a dead window's page is never invoked).
    bool Tab::EnsureAgentToolTipHoverHook(std::function<void()> onHoverBuild, std::function<bool(int)> onWheel)
    {
        ASSERT_UI_THREAD();
        if (_agentToolTipHoverWired)
        {
            return false; // already armed — keep the existing (identical-shape) callbacks
        }
        const auto tvi = TabViewItem();
        if (!tvi)
        {
            return false; // no owner yet — a later arm attempt (bind tail / liveness tick) wires it
        }
        _agentToolTipHoverCb = std::move(onHoverBuild);
        _agentToolTipWheelCb = std::move(onWheel);
        _agentToolTipHoverWired = true;
        const auto weakThis = get_weak();
        tvi.PointerEntered([weakThis](auto&&, auto&&) {
            if (const auto self = weakThis.get())
            {
                if (self->_agentToolTipHoverCb)
                {
                    try
                    {
                        self->_agentToolTipHoverCb();
                    }
                    catch (...)
                    {
                        // a tooltip must never take the tab down — swallow (the card simply stays stale)
                        ::Agentmaster::AgentLogCaughtException(L"Tab agent tooltip hover build");
                    }
                }
            }
        });

        // Agentmaster (tab tooltip — WHEEL SCROLL): the rich card is TALLER than it can show for a long
        // conversation, so the wheel scrolls its body. The wheel has to be read HERE, on the tab header,
        // because the card itself is IsHitTestVisible(false) and must stay that way — it is a popup, and
        // making popup content input-capable (a ScrollViewer, DirectManipulation) is the proven
        // 0xC000027B fail-fast class this whole feature was rebuilt around (HANDOVER_tab-tooltip.md
        // crash #7 / invariant 6a). The header is under the cursor for the tip's entire life anyway, so
        // "hover a tab, spin the wheel" needs no input on the card at all.
        //
        // We only ever READ IsOpen (never drive it — invariant 2), and we mark the notch Handled ONLY
        // when the page reports it actually scrolled something: otherwise the tab strip keeps its own
        // wheel behavior, which is what a card that fits on screen should never steal.
        //
        // AddHandler(..., handledEventsToo: true) rather than the `.PointerWheelChanged(...)` convenience
        // (which is handledEventsToo: FALSE): the tab strip is a nest of MUX controls, and anything inside
        // the header marking the notch Handled on its way up would otherwise make this hook silently dead.
        // The page keeps a second, whole-tab-row belt for the same reason (_WireTabStripTooltipWheel).
        tvi.AddHandler(
            WUX::UIElement::PointerWheelChangedEvent(),
            winrt::box_value(WUX::Input::PointerEventHandler{ [weakThis](const IInspectable&, const WUX::Input::PointerRoutedEventArgs& e) {
                const auto self = weakThis.get();
                if (!self || !self->_agentToolTipActive)
                {
                    return; // not a managed tab — the strip keeps its wheel, and we say nothing about it
                }
                try
                {
                    // ONE throttled line per gesture. This handler is the first link in the chain that
                    // cannot be observed from the outside, so without it "the scroll didn't register" is
                    // indistinguishable between "the notch never arrived", "the card wasn't open" and
                    // "the body had nothing to scroll" — three completely different bugs.
                    const auto nowTick = ::GetTickCount64();
                    const bool open = self->_agentToolTip && self->_agentToolTip.IsOpen();
                    if (nowTick - self->_agentToolTipWheelLogTick > 400)
                    {
                        self->_agentToolTipWheelLogTick = nowTick;
                        ::Agentmaster::AppendStateLog(L"hooks.log",
                                                      std::wstring{ L"[tooltip-wheel] item: cb=" } + (self->_agentToolTipWheelCb ? L"1" : L"0") +
                                                          L" tip=" + (self->_agentToolTip ? L"1" : L"0") +
                                                          L" open=" + (open ? L"1" : L"0"));
                    }
                    if (!self->_agentToolTipWheelCb || !open)
                    {
                        return; // no card on screen (or no page callback) — nothing to scroll
                    }
                    // Relative to the tab itself, not nullptr: the delta is placement-independent, and an
                    // element we KNOW is alive can't be the thing that throws on this path.
                    const auto pt = e.GetCurrentPoint(self->TabViewItem());
                    if (const auto delta = pt ? pt.Properties().MouseWheelDelta() : 0; delta != 0 && self->_agentToolTipWheelCb(delta))
                    {
                        e.Handled(true);
                    }
                }
                catch (...)
                {
                    // a tooltip must never take the tab down — swallow (the notch is simply lost)
                    ::Agentmaster::AgentLogCaughtException(L"Tab agent tooltip wheel scroll");
                }
            } }),
            true /* handledEventsToo */);
        return true;
    }

    // Agentmaster (tab tooltip): is the rich card on screen right now? Reading IsOpen is safe and always
    // was — it is DRIVING it that caused crashes #1/#5/#6, and invariant 2 forbids exactly that. The page
    // uses this for the WHEEL SCROLL bookkeeping: a card built while the tip is open can NOT be hosted
    // (invariant 3 — Content is swapped only while closed), so the page must neither adopt its scroll
    // pieces (it would drive an off-screen tree) nor reset the reader's scroll position under them.
    bool Tab::AgentToolTipOpen() const noexcept
    {
        try
        {
            return _agentToolTipActive && _agentToolTip && _agentToolTip.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"Tab::AgentToolTipOpen");
            return false;
        }
    }

    // Agentmaster (tab tooltip): detach the framework-managed tooltip from its owner and drop our strong
    // ref. Run on an owner recycle/unload (_WireAgentToolTipUnload), on ClearAgentToolTip (session gone), and
    // on Shutdown. Idempotent + safe when no tooltip was ever created (the guard leaves a default tooltip, if
    // any, untouched). We do NOT touch IsOpen — the framework owns open/close, so detaching is enough (it
    // dismisses any open popup itself); NOT driving IsOpen is precisely what removes the fail-fast/UAF class
    // the manual path suffered. Nulling the ref means the next _UpdateAgentToolTip rebuilds a FRESH tooltip
    // bound to the live owner. UI thread only.
    void Tab::_DetachAgentToolTip()
    {
        if (!_agentToolTip)
        {
            return; // nothing of ours attached — leave the default title+keychord tooltip (if any) alone
        }
        if (const auto tvi = TabViewItem())
        {
            try
            {
                WUX::Controls::ToolTipService::SetToolTip(tvi, nullptr);
            }
            catch (...)
            {
            }
        }
        _agentToolTip = nullptr;
    }

    // Method Description:
    // - Returns nullptr if no children of this tab were the last control to be
    //   focused, the active control of the current pane, or the last active child control
    //   of the active pane if it is a parent.
    // - This control might not currently be focused, if the tab itself is not
    //   currently focused.
    // Arguments:
    // - <none>
    // Return Value:
    // - nullptr if no children were marked `_lastFocused`, else the TermControl
    //   that was last focused.
    TermControl Tab::GetActiveTerminalControl() const
    {
        ASSERT_UI_THREAD();

        if (_activePane)
        {
            return _activePane->GetLastFocusedTerminalControl();
        }
        return nullptr;
    }

    IPaneContent Tab::GetActiveContent() const
    {
        return _activePane ? _activePane->GetContent() : nullptr;
    }

    // Method Description:
    // - Called after construction of a Tab object to bind event handlers to its
    //   associated Pane and TermControl objects
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::Initialize()
    {
        ASSERT_UI_THREAD();

        _rootPane->WalkTree([&](const auto& pane) {
            // Attach event handlers to each new pane
            _AttachEventHandlersToPane(pane);
            if (auto content = pane->GetContent())
            {
                _AttachEventHandlersToContent(pane->Id().value(), content);
            }
        });
    }

    // Method Description:
    // - Updates our focus state. If we're gaining focus, make sure to transfer
    //   focus to the last focused terminal control in our tree of controls.
    // Arguments:
    // - focused: our new focus state
    // Return Value:
    // - <none>
    void Tab::Focus(WUX::FocusState focusState)
    {
        ASSERT_UI_THREAD();

        _focusState = focusState;

        if (_focused())
        {
            auto lastFocusedControl = GetActiveTerminalControl();
            if (lastFocusedControl)
            {
                lastFocusedControl.Focus(_focusState);

                // Update our own progress state. This will fire an event signaling
                // that our taskbar progress changed.
                _UpdateProgressState();
            }
            else if (const auto content = GetActiveContent())
            {
                // Agentmaster: a non-terminal pane (an IPaneContent — the Agent Manager / Settings /
                // Scratchpad / tasks panes) has NO TermControl, so the terminal-only Focus above no-ops
                // and NOTHING in the tab gets keyboard focus on a tab switch. Focus the active pane's
                // CONTENT (-> IPaneContent::Focus) so its own key handlers work: the Manager tab's
                // ctrl+home / tab-nav chords are routed KeyDown events that fire only from a focused
                // element, and with NO focused element the whole tab swallowed them until the user
                // clicked something. (Terminal tabs take the if-branch above and are unaffected.)
                content.Focus(_focusState);
            }
            // When we gain focus, remove the bell indicator if it is active
            if (_tabStatus.BellIndicator())
            {
                ShowBellIndicator(false);
            }
        }
    }

    // Method Description:
    // - Returns nullopt if no children of this tab were the last control to be
    //   focused, or the GUID of the profile of the last control to be focused (if
    //   there was one).
    // Arguments:
    // - <none>
    // Return Value:
    // - nullopt if no children of this tab were the last control to be
    //   focused, else the GUID of the profile of the last control to be focused
    Profile Tab::GetFocusedProfile() const noexcept
    {
        ASSERT_UI_THREAD();

        return _activePane->GetFocusedProfile();
    }

    // Method Description:
    // - Attempts to update the settings that apply to this tab.
    // - Panes are handled elsewhere, by somebody who can establish broader knowledge
    //   of the settings that apply to all tabs.
    // Return Value:
    // - <none>
    void Tab::UpdateSettings(const CascadiaSettings& settings)
    {
        ASSERT_UI_THREAD();

        // The tabWidthMode may have changed, update the header control accordingly
        _UpdateHeaderControlMaxWidth();

        // Update the settings on all our panes.
        _rootPane->WalkTree([&](const auto& pane) {
            pane->UpdateSettings(settings);
            return false;
        });
    }

    // Method Description:
    // - Set the icon on the TabViewItem for this tab.
    // Arguments:
    // - iconPath: The new path string to use as the IconPath for our TabViewItem
    // Return Value:
    // - <none>
    void Tab::UpdateIcon(const winrt::hstring& iconPath, const winrt::Microsoft::Terminal::Settings::Model::IconStyle iconStyle)
    {
        ASSERT_UI_THREAD();

        // Don't reload our icon and iconStyle hasn't changed.
        if (iconPath == _lastIconPath && iconStyle == _lastIconStyle)
        {
            return;
        }
        _lastIconPath = iconPath;
        _lastIconStyle = iconStyle;

        // If the icon is currently hidden, just return here (but only after setting _lastIconPath to the new path
        // for when we show the icon again)
        if (_iconHidden)
        {
            return;
        }

        if (iconStyle == IconStyle::Hidden)
        {
            // The TabViewItem Icon needs MUX while the IconSourceElement in the CommandPalette needs WUX...
            Icon({});
            TabViewItem().IconSource(IconSource{ nullptr });
        }
        else
        {
            Icon(_lastIconPath);
            bool isMonochrome = iconStyle == IconStyle::Monochrome;
            TabViewItem().IconSource(Microsoft::Terminal::UI::IconPathConverter::IconSourceMUX(_lastIconPath, isMonochrome));
        }
    }

    // Method Description:
    // - Hide or show the tab icon for this tab
    // - Used when we want to show the progress ring, which should replace the icon
    // Arguments:
    // - hide: if true, we hide the icon; if false, we show the icon
    void Tab::HideIcon(const bool hide)
    {
        ASSERT_UI_THREAD();

        if (_iconHidden != hide)
        {
            if (hide || _lastIconStyle == IconStyle::Hidden)
            {
                // Agentmaster: restore to HIDDEN when the icon style is Hidden — either the WT theme's
                // "tab.iconStyle":"hidden" or our "Show icons on tabs" cog toggle (off). Without this,
                // a progress ring finishing (HideIcon(false)) would re-show the profile icon on a tab
                // whose icon is meant to stay hidden, until the next _UpdateTabIcon pass.
                Icon({});
                TabViewItem().IconSource(IconSource{ nullptr });
            }
            else
            {
                Icon(_lastIconPath);
                TabViewItem().IconSource(Microsoft::Terminal::UI::IconPathConverter::IconSourceMUX(_lastIconPath, _lastIconStyle == IconStyle::Monochrome));
            }
            _iconHidden = hide;
        }
    }

    // Method Description:
    // - Hide or show the bell indicator in the tab header
    // Arguments:
    // - show: if true, we show the indicator; if false, we hide the indicator
    void Tab::ShowBellIndicator(const bool show)
    {
        ASSERT_UI_THREAD();

        _tabStatus.BellIndicator(show);
    }

    // Method Description:
    // - Activates the timer for the bell indicator in the tab
    // - Called if a bell raised when the tab already has focus
    void Tab::ActivateBellIndicatorTimer()
    {
        ASSERT_UI_THREAD();

        if (!_bellIndicatorTimer)
        {
            _bellIndicatorTimer.Interval(std::chrono::milliseconds(2000));
            _bellIndicatorTimer.Tick({ get_weak(), &Tab::_BellIndicatorTimerTick });
        }

        _bellIndicatorTimer.Start();
    }

    // Method Description:
    // - Gets the title string of the last focused terminal control in our tree.
    //   Returns the empty string if there is no such control.
    // Arguments:
    // - <none>
    // Return Value:
    // - the title string of the last focused terminal control in our tree.
    winrt::hstring Tab::_GetActiveTitle() const
    {
        if (!_runtimeTabText.empty())
        {
            return _runtimeTabText;
        }
        if (!_activePane->_IsLeaf())
        {
            return RS_(L"MultiplePanes");
        }
        const auto activeContent = GetActiveContent();
        return activeContent ? activeContent.Title() : winrt::hstring{};
    }

    // Method Description:
    // - Set the text on the TabViewItem for this tab, and bubbles the new title
    //   value up to anyone listening for changes to our title. Callers can
    //   listen for the title change with a PropertyChanged even handler.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::UpdateTitle()
    {
        ASSERT_UI_THREAD();

        const auto activeTitle = _GetActiveTitle();
        // Bubble our current tab text to anyone who's listening for changes.
        Title(activeTitle);

        // Update the control to reflect the changed title
        _headerControl.Title(activeTitle);
        Automation::AutomationProperties::SetName(TabViewItem(), activeTitle);
        _UpdateToolTip();
    }

    // Method Description:
    // - Move the viewport of the terminal up or down a number of lines. Negative
    //      values of `delta` will move the view up, and positive values will move
    //      the viewport down.
    // Arguments:
    // - delta: a number of lines to move the viewport relative to the current viewport.
    // Return Value:
    // - <none>
    void Tab::Scroll(const int delta)
    {
        ASSERT_UI_THREAD();

        auto control = GetActiveTerminalControl();
        const auto currentOffset = control.ScrollOffset();
        control.ScrollViewport(::base::ClampAdd(currentOffset, delta));
    }

    // Method Description:
    // - Serializes the state of this tab as a series of commands that can be
    //   executed to recreate it.
    // Arguments:
    // - <none>
    // Return Value:
    // - A vector of commands
    std::vector<ActionAndArgs> Tab::BuildStartupActions(BuildStartupKind kind) const
    {
        ASSERT_UI_THREAD();

        // Give initial ids (0 for the child created with this tab,
        // 1 for the child after the first split.
        auto state = _rootPane->BuildStartupActions(0, 1, kind);

        {
            ActionAndArgs newTabAction{};
            INewContentArgs newContentArgs{ state.firstPane->GetTerminalArgsForPane(kind) };

            // Special case here: if there was one pane (which results in no actions
            // being generated), and it was a settings pane, then promote that to an
            // open settings action. The openSettings action itself has additional machinery
            // to prevent multiple top-level settings tabs.
            const auto wasSettings = state.args.empty() &&
                                     (newContentArgs && newContentArgs.Type() == L"settings");
            if (wasSettings)
            {
                newTabAction.Action(ShortcutAction::OpenSettings);
                newTabAction.Args(OpenSettingsArgs{ SettingsTarget::SettingsUI });
                return std::vector<ActionAndArgs>{ std::move(newTabAction) };
            }

            newTabAction.Action(ShortcutAction::NewTab);
            newTabAction.Args(NewTabArgs{ newContentArgs });

            state.args.emplace(state.args.begin(), std::move(newTabAction));
        }

        // Agentmaster (tab color modes — NoColor/"Remove colors"): persist the runtime color OR the
        // suspended one — a capture/tear-out taken while the mode has the color parked must record
        // the color the tab HAD (the mode hides colors, it never voids them). In a colored mode
        // _suspendedTabColor is always empty, so this is byte-identical to the old _runtimeTabColor
        // read. A torn-out/moved tab recreated in another window replays the setColor there and the
        // destination's NoColor chokepoint immediately re-parks it — the color survives the move.
        if (const auto persistColor = _runtimeTabColor ? _runtimeTabColor : _suspendedTabColor)
        {
            ActionAndArgs setColorAction{};
            setColorAction.Action(ShortcutAction::SetTabColor);

            SetTabColorArgs setColorArgs{ persistColor.value() };
            setColorAction.Args(setColorArgs);

            state.args.emplace_back(std::move(setColorAction));
        }

        if (!_runtimeTabText.empty())
        {
            ActionAndArgs renameTabAction{};
            renameTabAction.Action(ShortcutAction::RenameTab);

            RenameTabArgs renameTabArgs{ _runtimeTabText };
            renameTabAction.Args(renameTabArgs);

            state.args.emplace_back(std::move(renameTabAction));
        }

        // If we only have one arg, we only have 1 pane so we don't need any
        // special focus logic
        if (state.args.size() > 1 && state.focusedPaneId.has_value())
        {
            ActionAndArgs focusPaneAction{};
            focusPaneAction.Action(ShortcutAction::FocusPane);
            FocusPaneArgs focusArgs{ state.focusedPaneId.value() };
            focusPaneAction.Args(focusArgs);

            state.args.emplace_back(std::move(focusPaneAction));
        }

        if (_zoomedPane)
        {
            // we start without any panes zoomed so toggle zoom will enable zoom.
            ActionAndArgs zoomPaneAction{};
            zoomPaneAction.Action(ShortcutAction::TogglePaneZoom);

            state.args.emplace_back(std::move(zoomPaneAction));
        }

        return state.args;
    }

    // Method Description:
    // - Split the focused pane in our tree of panes, and place the
    //   given pane into the tree of panes according to the split
    // Arguments:
    // - splitType: The type of split we want to create
    // - splitSize: The size of the split we want to create
    // - pane: The new pane to add to the tree of panes; note that this pane
    //         could itself be a parent pane/the root node of a tree of panes
    // Return Value:
    // - a pair of (the Pane that now holds the original content, the new Pane in the tree)
    std::pair<std::shared_ptr<Pane>, std::shared_ptr<Pane>> Tab::SplitPane(SplitDirection splitType,
                                                                           const float splitSize,
                                                                           std::shared_ptr<Pane> pane)
    {
        ASSERT_UI_THREAD();

        // Add the new event handlers to the new pane(s)
        // and update their ids.
        pane->WalkTree([&](const auto& p) {
            _AttachEventHandlersToPane(p);
            if (p->_IsLeaf())
            {
                p->Id(_nextPaneId);
                if (const auto& content{ p->GetContent() })
                {
                    _AttachEventHandlersToContent(p->Id().value(), content);
                }
                _nextPaneId++;
            }
            return false;
        });
        pane->EnableBroadcast(_tabStatus.IsInputBroadcastActive());

        // Make sure to take the ID before calling Split() - Split() will clear out the active pane's ID
        const auto activePaneId = _activePane->Id();
        // Depending on which direction will be split, the new pane can be
        // either the first or second child, but this will always return the
        // original pane first.
        auto [original, newPane] = _activePane->Split(splitType, splitSize, pane);

        // After split, Close Pane Menu Item should be visible
        _closePaneMenuItem.Visibility(WUX::Visibility::Visible);

        // The active pane has an id if it is a leaf
        if (activePaneId)
        {
            original->Id(activePaneId.value());
        }

        _activePane = original;

        // Add event handlers to the new panes' GotFocus event. When the pane
        // gains focus, we'll mark it as the new active pane.
        _AttachEventHandlersToPane(original);

        // Immediately update our tracker of the focused pane now. If we're
        // splitting panes during startup (from a commandline), then it's
        // possible that the focus events won't propagate immediately. Updating
        // the focus here will give the same effect though.
        _UpdateActivePane(newPane);

        return { original, newPane };
    }

    // Method Description:
    // - Removes the currently active pane from this tab. If that was the only
    //   remaining pane, then the entire tab is closed as well.
    // Arguments:
    // - <none>
    // Return Value:
    // - The removed pane, if the remove succeeded.
    std::shared_ptr<Pane> Tab::DetachPane()
    {
        ASSERT_UI_THREAD();

        // if we only have one pane, or the focused pane is the root, remove it
        // entirely and close this tab
        if (_rootPane == _activePane)
        {
            return DetachRoot();
        }

        // Attempt to remove the active pane from the tree
        if (const auto pane = _rootPane->DetachPane(_activePane))
        {
            // Just make sure that the remaining pane is marked active
            _UpdateActivePane(_rootPane->GetActivePane());

            return pane;
        }

        return nullptr;
    }

    // Method Description:
    // - Closes this tab and returns the root pane to be used elsewhere.
    // Arguments:
    // - <none>
    // Return Value:
    // - The root pane.
    std::shared_ptr<Pane> Tab::DetachRoot()
    {
        ASSERT_UI_THREAD();

        // remove the closed event handler since we are closing the tab
        // manually.
        _rootPane->Closed(_rootClosedToken);
        auto p = _rootPane;
        p->WalkTree([](const auto& pane) {
            pane->Detached.raise(pane);
        });

        // Clean up references and close the tab
        _rootPane = nullptr;
        _activePane = nullptr;
        Content(nullptr);
        Closed.raise(nullptr, nullptr);

        return p;
    }

    // Method Description:
    // - Add an arbitrary pane to this tab. This will be added as a split on the
    //   currently active pane.
    // Arguments:
    // - pane: The pane to add.
    // Return Value:
    // - <none>
    void Tab::AttachPane(std::shared_ptr<Pane> pane)
    {
        ASSERT_UI_THREAD();

        // Add the new event handlers to the new pane(s)
        // and update their ids.
        pane->WalkTree([&](const auto& p) {
            _AttachEventHandlersToPane(p);
            if (p->_IsLeaf())
            {
                p->Id(_nextPaneId);

                if (const auto& content{ p->GetContent() })
                {
                    _AttachEventHandlersToContent(p->Id().value(), content);
                }
                _nextPaneId++;
            }
        });
        pane->EnableBroadcast(_tabStatus.IsInputBroadcastActive());
        // pass the old id to the new child
        const auto previousId = _activePane->Id();

        // Add the new pane as an automatic split on the active pane.
        auto first = _activePane->AttachPane(pane, SplitDirection::Automatic);

        // This will be true if the original _activePane is a leaf pane.
        // If it is a parent pane then we don't want to set an ID on it.
        if (previousId)
        {
            first->Id(previousId.value());
        }

        // Update with event handlers on the new child.
        _activePane = first;
        _AttachEventHandlersToPane(first);

        // Make sure that we have the right pane set as the active pane
        if (const auto focus = pane->GetActivePane())
        {
            _UpdateActivePane(focus);
        }
    }

    // Method Description:
    // - Attaches the given color picker to ourselves
    // - Typically will be called after we have sent a request for the color picker
    // Arguments:
    // - colorPicker: The color picker that we should attach to ourselves
    // Return Value:
    // - <none>
    void Tab::AttachColorPicker(TerminalApp::ColorPickupFlyout& colorPicker)
    {
        ASSERT_UI_THREAD();

        // Agentmaster (tab color modes — NoColor/"Remove colors"): the picker is disabled for this
        // tab (SetColorPickerEnabled(false)). The context-menu item is grayed, but the
        // openTabColorPicker ACTION (keybinding / command palette) lands here directly — refuse it
        // too, so a managed tab can't be recolored while the mode loads no colors.
        if (_colorPickerDisabled)
        {
            return;
        }

        auto weakThis{ get_weak() };

        _tabColorPickup = colorPicker;

        // Agentmaster ("Use Tab Color"): hand the picker the color THIS tab currently wears, so its
        // custom picker can be seeded with it instead of opening on black / the last tab's pick.
        // This is the ONE seam every entry point funnels through (the "Change tab color..."
        // context-menu item and the openTabColorPicker action both dispatch OpenTabColorPicker ->
        // AttachColorPicker), and the flyout is a per-window SINGLETON, so it must be pushed on
        // EVERY attach — pushing a null for a colorless tab is what stops the previously-picked
        // tab's color being offered as "this tab's color".
        // GetTabColor (not GetRuntimeTabColor): a profile-/content-colored tab is worth seeding from
        // too. It stays a seed — nothing is committed until the user drags the picker or presses OK.
        if (const auto current = GetTabColor())
        {
            _tabColorPickup.SetCurrentTabColor(*current);
        }
        else
        {
            _tabColorPickup.SetCurrentTabColor(nullptr);
        }

        _colorSelectedToken = _tabColorPickup.ColorSelected([weakThis](auto newTabColor) {
            if (auto tab{ weakThis.get() })
            {
                tab->SetRuntimeTabColor(newTabColor);
            }
        });

        _colorClearedToken = _tabColorPickup.ColorCleared([weakThis]() {
            if (auto tab{ weakThis.get() })
            {
                tab->ResetRuntimeTabColor();
            }
        });

        _pickerClosedToken = _tabColorPickup.Closed([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->_tabColorPickup.ColorSelected(tab->_colorSelectedToken);
                tab->_tabColorPickup.ColorCleared(tab->_colorClearedToken);
                tab->_tabColorPickup.Closed(tab->_pickerClosedToken);
                tab->_tabColorPickup = nullptr;
            }
        });

        _tabColorPickup.ShowAt(TabViewItem());
    }

    // Method Description:
    // - Find the currently active pane, and then switch the split direction of
    //   its parent. E.g. switch from Horizontal to Vertical.
    // Return Value:
    // - <none>
    void Tab::ToggleSplitOrientation()
    {
        ASSERT_UI_THREAD();

        _rootPane->ToggleSplitOrientation();
    }

    // Method Description:
    // - See Pane::CalcSnappedDimension
    float Tab::CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
    {
        ASSERT_UI_THREAD();

        return _rootPane->CalcSnappedDimension(widthOrHeight, dimension);
    }

    // Method Description:
    // - Attempt to move a separator between panes, as to resize each child on
    //   either size of the separator. See Pane::ResizePane for details.
    // Arguments:
    // - direction: The direction to move the separator in.
    // Return Value:
    // - whether a pane was resized
    bool Tab::ResizePane(const ResizeDirection& direction)
    {
        ASSERT_UI_THREAD();

        // NOTE: This _must_ be called on the root pane, so that it can propagate
        // throughout the entire tree.
        return _rootPane->ResizePane(direction);
    }

    // Method Description:
    // - Attempt to move focus between panes, as to focus the child on
    //   the other side of the separator. See Pane::NavigateFocus for details.
    // Arguments:
    // - direction: The direction to move the focus in.
    // Return Value:
    // - Whether changing the focus succeeded. This allows a keychord to propagate
    //   to the terminal when no other panes are present (GH#6219)
    bool Tab::NavigateFocus(const FocusDirection& direction)
    {
        ASSERT_UI_THREAD();

        // NOTE: This _must_ be called on the root pane, so that it can propagate
        // throughout the entire tree.
        if (const auto newFocus = _rootPane->NavigateDirection(_activePane, direction, _mruPanes))
        {
            // Mark that we want the active pane to changed
            _changingActivePane = true;
            const auto res = _rootPane->FocusPane(newFocus);
            _changingActivePane = false;

            if (_zoomedPane)
            {
                UpdateZoom(newFocus);
            }

            return res;
        }

        return false;
    }

    // Method Description:
    // - Attempts to swap the location of the focused pane with another pane
    //   according to direction. When there are multiple adjacent panes it will
    //   select the first one (top-left-most).
    // Arguments:
    // - direction: The direction to move the pane in.
    // Return Value:
    // - true if two panes were swapped.
    bool Tab::SwapPane(const FocusDirection& direction)
    {
        ASSERT_UI_THREAD();

        // You cannot swap panes with the parent/child pane because of the
        // circular reference.
        if (direction == FocusDirection::Parent || direction == FocusDirection::Child)
        {
            return false;
        }
        // NOTE: This _must_ be called on the root pane, so that it can propagate
        // throughout the entire tree.
        if (auto neighbor = _rootPane->NavigateDirection(_activePane, direction, _mruPanes))
        {
            // SwapPanes will refocus the terminal to make sure that it has focus
            // even after moving.
            _changingActivePane = true;
            const auto res = _rootPane->SwapPanes(_activePane, neighbor);
            _changingActivePane = false;
            return res;
        }

        return false;
    }

    bool Tab::FocusPane(const uint32_t id)
    {
        ASSERT_UI_THREAD();

        if (_rootPane == nullptr)
        {
            return false;
        }
        _changingActivePane = true;
        const auto res = _rootPane->FocusPane(id);
        _changingActivePane = false;
        return res;
    }

    void Tab::Close()
    {
        ASSERT_UI_THREAD();

        Closed.raise(nullptr, nullptr);
    }

    // Method Description:
    // - Prepares this tab for being removed from the UI hierarchy by shutting down all active connections.
    void Tab::Shutdown()
    {
        ASSERT_UI_THREAD();

        // Agentmaster: detach + drop the agent tooltip BEFORE the tab's visuals go away, so no stale ref
        // survives the teardown. The framework owns open/close, so there is no manually-opened popup to
        // force shut; detaching from the owner is enough. See _DetachAgentToolTip. Also drop the lazy
        // hover-build callback — it weak-captures the page, but releasing it here keeps a closed tab from
        // holding the last lambda (and a late PointerEntered on a dying strip invokes nothing).
        _DetachAgentToolTip();
        _agentToolTipHoverCb = nullptr;
        _agentToolTipWheelCb = nullptr; // WHEEL SCROLL: same reasoning — a late notch on a dying strip invokes nothing

        // NOTE: `TerminalPage::_HandleCloseTabRequested` relies on the content being null after this call.
        Content(nullptr);

        if (_rootPane)
        {
            _rootPane->Shutdown();
        }
    }

    // Method Description:
    // - Closes the currently focused pane in this tab. If it's the last pane in
    //   this tab, our Closed event will be fired (at a later time) for anyone
    //   registered as a handler of our close event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::ClosePane()
    {
        ASSERT_UI_THREAD();

        _activePane->Close();
    }

    void Tab::SetTabText(winrt::hstring title)
    {
        ASSERT_UI_THREAD();

        _runtimeTabText = title;
        UpdateTitle();
    }

    winrt::hstring Tab::GetTabText() const
    {
        ASSERT_UI_THREAD();

        return _runtimeTabText;
    }

    void Tab::ResetTabText()
    {
        ASSERT_UI_THREAD();

        _runtimeTabText = L"";
        UpdateTitle();
    }

    // Method Description:
    // - Show a TextBox in the Header to allow the user to set a string
    //     to use as an override for the tab's text
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::ActivateTabRenamer()
    {
        ASSERT_UI_THREAD();

        // Agentmaster: the pinned Manager tab has rename disabled. This is the single funnel for
        // every interactive rename gesture (double-tap, context-menu "Rename Tab", openTabRenamer
        // action), so one guard here blocks them all.
        if (_renameDisabled)
        {
            return;
        }

        _headerControl.BeginRename();
    }

    // Method Description:
    // - Removes any event handlers set by the tab on the given pane's control.
    //   The pane's ID is the most stable identifier for a given control, because
    //   the control itself doesn't have a particular ID and its pointer is
    //   unstable since it is moved when panes split.
    // Arguments:
    // - paneId: The ID of the pane that contains the given content.
    // Return Value:
    // - <none>
    void Tab::_DetachEventHandlersFromContent(const uint32_t paneId)
    {
        auto it = _contentEvents.find(paneId);
        if (it != _contentEvents.end())
        {
            // revoke the event handlers by resetting the event struct
            // and remove it from the map
            _contentEvents.erase(paneId);
        }
    }

    // Method Description:
    // - Register any event handlers that we may need with the given TermControl.
    //   This should be called on each and every TermControl that we add to the tree
    //   of Panes in this tab. We'll add events too:
    //   * notify us when the control's title changed, so we can update our own
    //     title (if necessary)
    // Arguments:
    // - paneId: the ID of the pane that this control belongs to.
    // - control: the TermControl to add events to.
    // Return Value:
    // - <none>
    void Tab::_AttachEventHandlersToContent(const uint32_t paneId, const TerminalApp::IPaneContent& content)
    {
        auto weakThis{ get_weak() };
        auto dispatcher = DispatcherQueue::GetForCurrentThread();
        ContentEventTokens events{};

        auto throttledTitleChanged = std::make_shared<ThrottledFunc<>>(
            dispatcher,
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 200 },
                .leading = true,
                .trailing = true,
            },
            [weakThis]() {
                if (const auto tab = weakThis.get())
                {
                    tab->UpdateTitle();
                }
            });

        events.TitleChanged = content.TitleChanged(
            winrt::auto_revoke,
            [func = std::move(throttledTitleChanged)](auto&&, auto&&) {
                func->Run();
            });

        auto throttledTaskbarProgressChanged = std::make_shared<ThrottledFunc<>>(
            dispatcher,
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 200 },
                .trailing = true,
            },
            [weakThis]() {
                if (const auto tab = weakThis.get())
                {
                    tab->_UpdateProgressState();
                }
            });

        events.TaskbarProgressChanged = content.TaskbarProgressChanged(
            winrt::auto_revoke,
            [func = std::move(throttledTaskbarProgressChanged)](auto&&, auto&&) {
                func->Run();
            });

        events.TabColorChanged = content.TabColorChanged(
            winrt::auto_revoke,
            [dispatcher, weakThis](auto&&, auto&&) -> safe_void_coroutine {
                const auto weakThisCopy = weakThis;
                co_await wil::resume_foreground(dispatcher);
                if (auto tab{ weakThisCopy.get() })
                {
                    // The control's tabColor changed, but it is not necessarily the
                    // active control in this tab. We'll just recalculate the
                    // current color anyways.
                    tab->_RecalculateAndApplyTabColor();
                    tab->_tabStatus.TabColorIndicator(tab->GetTabColor().value_or(Windows::UI::Colors::Transparent()));
                }
            });

        events.ConnectionStateChanged = content.ConnectionStateChanged(
            winrt::auto_revoke,
            [dispatcher, weakThis](auto&&, auto&&) -> safe_void_coroutine {
                const auto weakThisCopy = weakThis;
                co_await wil::resume_foreground(dispatcher);
                if (auto tab{ weakThisCopy.get() })
                {
                    tab->_UpdateConnectionClosedState();
                }
            });

        events.ReadOnlyChanged = content.ReadOnlyChanged(
            winrt::auto_revoke,
            [dispatcher, weakThis](auto&&, auto&&) -> safe_void_coroutine {
                const auto weakThisCopy = weakThis;
                co_await wil::resume_foreground(dispatcher);
                if (auto tab{ weakThisCopy.get() })
                {
                    tab->_RecalculateAndApplyReadOnly();
                }
            });

        events.FocusRequested = content.FocusRequested(
            winrt::auto_revoke,
            [dispatcher, weakThis](TerminalApp::IPaneContent sender, auto) -> safe_void_coroutine {
                const auto weakThisCopy = weakThis;
                co_await wil::resume_foreground(dispatcher);
                if (const auto tab{ weakThisCopy.get() })
                {
                    if (tab->_focused())
                    {
                        sender.Focus(FocusState::Pointer);
                    }
                }
            });

        events.BellRequested = content.BellRequested(
            winrt::auto_revoke,
            [dispatcher, weakThis](TerminalApp::IPaneContent sender, auto bellArgs) -> safe_void_coroutine {
                const auto weakThisCopy = weakThis;
                co_await wil::resume_foreground(dispatcher);
                if (const auto tab{ weakThisCopy.get() })
                {
                    if (bellArgs.FlashTaskbar())
                    {
                        // If visual is set, we need to bubble this event all the way to app host to flash the taskbar
                        // In this part of the chain we bubble it from the hosting tab to the page
                        tab->TabRaiseVisualBell.raise();
                    }

                    // Send a desktop toast notification if requested, but only if
                    // the pane isn't already in the belled state. This prevents
                    // sending repeated toasts for repeated BEL characters.
                    if (bellArgs.SendNotification() && !tab->_tabStatus.BellIndicator())
                    {
                        tab->TabToastNotificationRequested.raise(tab->Title(), L"", sender);
                    }

                    // Show the bell indicator in the tab header
                    tab->ShowBellIndicator(true);

                    // If this tab is focused, activate the bell indicator timer, which will
                    // remove the bell indicator once it fires
                    // (otherwise, the indicator is removed when the tab gets focus)
                    if (tab->_focusState != WUX::FocusState::Unfocused)
                    {
                        tab->ActivateBellIndicatorTimer();
                    }
                }
            });

        if (const auto& terminal{ content.try_as<TerminalApp::TerminalPaneContent>() })
        {
            events.RestartTerminalRequested = terminal.RestartTerminalRequested(winrt::auto_revoke, { get_weak(), &Tab::_bubbleRestartTerminalRequested });
        }

        events.NotificationRequested = content.NotificationRequested(
            winrt::auto_revoke,
            [dispatcher, weakThis](TerminalApp::IPaneContent sender, auto notifArgs) -> safe_void_coroutine {
                const auto weakThisCopy = weakThis;
                co_await wil::resume_foreground(dispatcher);
                if (const auto tab{ weakThisCopy.get() })
                {
                    const auto title = notifArgs.Title().empty() ? tab->Title() : notifArgs.Title();
                    tab->TabToastNotificationRequested.raise(title, notifArgs.Body(), sender);
                }
            });

        if (_tabStatus.IsInputBroadcastActive())
        {
            if (const auto& termContent{ content.try_as<TerminalApp::TerminalPaneContent>() })
            {
                _addBroadcastHandlers(termContent.GetTermControl(), events);
            }
        }

        _contentEvents[paneId] = std::move(events);
    }

    // Method Description:
    // - Get the combined taskbar state for the tab. This is the combination of
    //   all the states of all our panes. Taskbar states are given a priority
    //   based on the rules in:
    //   https://docs.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist3-setprogressstate
    //   under "How the Taskbar Button Chooses the Progress Indicator for a
    //   Group"
    // Arguments:
    // - <none>
    // Return Value:
    // - A TaskbarState object representing the combined taskbar state and
    //   progress percentage of all our panes.
    winrt::TerminalApp::TaskbarState Tab::GetCombinedTaskbarState() const
    {
        ASSERT_UI_THREAD();

        std::vector<winrt::TerminalApp::TaskbarState> states;
        if (_rootPane)
        {
            _rootPane->CollectTaskbarStates(states);
        }
        return states.empty() ? winrt::make<winrt::TerminalApp::implementation::TaskbarState>() :
                                *std::min_element(states.begin(), states.end(), TerminalApp::implementation::TaskbarState::ComparePriority);
    }

    // Method Description:
    // - This should be called on the UI thread. If you don't, then it might
    //   silently do nothing.
    // - Update our TabStatus to reflect the progress state of the currently
    //   active pane.
    // - This is called every time _any_ control's progress state changes,
    //   regardless of if that control is the active one or not. This is simpler
    //   then re-attaching this handler to the active control each time it
    //   changes.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_UpdateProgressState()
    {
        const auto state{ GetCombinedTaskbarState() };

        const auto taskbarState = state.State();
        // The progress of the control changed, but not necessarily the progress of the tab.
        // Set the tab's progress ring to the active pane's progress
        if (taskbarState > 0)
        {
            if (taskbarState == 3)
            {
                // 3 is the indeterminate state, set the progress ring as such
                _tabStatus.IsProgressRingIndeterminate(true);
            }
            else
            {
                // any non-indeterminate state has a value, set the progress ring as such
                _tabStatus.IsProgressRingIndeterminate(false);

                const auto progressValue = gsl::narrow<uint32_t>(state.Progress());
                _tabStatus.ProgressValue(progressValue);
            }
            // Hide the tab icon (the progress ring is placed over it)
            HideIcon(true);
            _tabStatus.IsProgressRingActive(true);
        }
        else
        {
            // Show the tab icon
            HideIcon(false);
            _tabStatus.IsProgressRingActive(false);
        }

        // fire an event signaling that our taskbar progress changed.
        TaskbarProgressChanged.raise(nullptr, nullptr);
    }

    // Method Description:
    // - Set an indicator on the tab if any pane is in a closed connection state.
    // - Show/hide the Restart Session context menu entry depending on active pane's state.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_UpdateConnectionClosedState()
    {
        ASSERT_UI_THREAD();

        if (_rootPane)
        {
            const bool isClosed = _rootPane->WalkTree([&](const auto& p) {
                return p->IsConnectionClosed();
            });

            _tabStatus.IsConnectionClosed(isClosed);
        }
    }

    void Tab::_RestartActivePaneConnection()
    {
        ActionAndArgs restartConnection{ ShortcutAction::RestartConnection, nullptr };
        _dispatch.DoAction(*this, restartConnection);
    }

    // Method Description:
    // - Mark the given pane as the active pane in this tab. All other panes
    //   will be marked as inactive. We'll also update our own UI state to
    //   reflect this newly active pane.
    // Arguments:
    // - pane: a Pane to mark as active.
    // Return Value:
    // - <none>
    void Tab::_UpdateActivePane(std::shared_ptr<Pane> pane)
    {
        // Clear the active state of the entire tree, and mark only the pane as active.
        _rootPane->ClearActive();
        _activePane = pane;
        _activePane->SetActive();

        // Update our own title text to match the newly-active pane.
        UpdateTitle();
        _UpdateProgressState();
        _UpdateConnectionClosedState();

        // We need to move the pane to the top of our mru list
        // If its already somewhere in the list, remove it first
        if (const auto paneId = pane->Id())
        {
            for (auto i = _mruPanes.begin(); i != _mruPanes.end(); ++i)
            {
                if (*i == paneId.value())
                {
                    _mruPanes.erase(i);
                    break;
                }
            }
            _mruPanes.insert(_mruPanes.begin(), paneId.value());
        }

        if (_rootPane->GetLeafPaneCount() == 1)
        {
            _closePaneMenuItem.Visibility(WUX::Visibility::Collapsed);
        }

        _RecalculateAndApplyReadOnly();

        // Raise our own ActivePaneChanged event.
        ActivePaneChanged.raise(*this, nullptr);

        // If the new active pane is a terminal, tell other interested panes
        // what the new active pane is.
        const auto content{ pane->GetContent() };
        if (const auto termContent{ content.try_as<winrt::TerminalApp::TerminalPaneContent>() })
        {
            const auto& termControl{ termContent.GetTermControl() };
            _rootPane->WalkTree([termControl](const auto& p) {
                if (const auto& taskPane{ p->GetContent().try_as<SnippetsPaneContent>() })
                {
                    taskPane.SetLastActiveControl(termControl);
                }
                else if (const auto& taskPane{ p->GetContent().try_as<MarkdownPaneContent>() })
                {
                    taskPane.SetLastActiveControl(termControl);
                }
            });
        }

        _UpdateMenuItemStates();
    }

    void Tab::_UpdateMenuItemStates()
    {
        // Terminal-specific menu items
        const auto content = _activePane ? _activePane->GetContent() : nullptr;
        const auto isTerm = content && content.try_as<winrt::TerminalApp::TerminalPaneContent>() != nullptr;
        _duplicateTabMenuItem.IsEnabled(isTerm);
        _exportTabMenuItem.IsEnabled(isTerm);
        _findMenuItem.IsEnabled(isTerm);
        _restartConnectionMenuItem.IsEnabled(isTerm);
        _newSessionHereMenuItem.IsEnabled(isTerm); // Agentmaster: only meaningful for a terminal tab (a cwd to spawn in)

        // Snippets Pane can technically be split
        _splitTabMenuItem.IsEnabled(isTerm || (content && content.try_as<winrt::TerminalApp::SnippetsPaneContent>() != nullptr));
    }

    // Method Description:
    // - Add an event handler to this pane's GotFocus event. When that pane gains
    //   focus, we'll mark it as the new active pane. We'll also query the title of
    //   that pane when it's focused to set our own text, and finally, we'll trigger
    //   our own ActivePaneChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_AttachEventHandlersToPane(std::shared_ptr<Pane> pane)
    {
        auto weakThis{ get_weak() };
        std::weak_ptr<Pane> weakPane{ pane };

        auto gotFocusToken = pane->GotFocus([weakThis](std::shared_ptr<Pane> sender, WUX::FocusState focus) {
            // Do nothing if the Tab's lifetime is expired or pane isn't new.
            auto tab{ weakThis.get() };

            if (tab)
            {
                if (sender != tab->_activePane)
                {
                    auto senderIsChild = tab->_activePane->_HasChild(sender);

                    // Only move focus if we the program moved focus, or the
                    // user moved with their mouse. This is a problem because a
                    // pane isn't a control itself, and if we have the parent
                    // focused we are fine if the terminal control is focused,
                    // but we don't want to update the active pane.
                    if (!senderIsChild ||
                        (focus == WUX::FocusState::Programmatic && tab->_changingActivePane) ||
                        focus == WUX::FocusState::Pointer)
                    {
                        tab->_UpdateActivePane(sender);
                        tab->_RecalculateAndApplyTabColor();
                    }
                }
                tab->_focusState = WUX::FocusState::Programmatic;
                // This tab has gained focus, remove the bell indicator if it is active
                if (tab->_tabStatus.BellIndicator())
                {
                    tab->ShowBellIndicator(false);
                }
            }
        });

        auto lostFocusToken = pane->LostFocus([weakThis](std::shared_ptr<Pane> /*sender*/) {
            // Do nothing if the Tab's lifetime is expired or pane isn't new.
            auto tab{ weakThis.get() };

            if (tab)
            {
                // update this tab's focus state
                tab->_focusState = WUX::FocusState::Unfocused;
            }
        });

        // Add a Closed event handler to the Pane. If the pane closes out from
        // underneath us, and it's zoomed, we want to be able to make sure to
        // update our state accordingly to un-zoom that pane. See GH#7252.
        auto closedToken = pane->Closed([weakThis, weakPane](auto&& /*s*/, auto&& /*e*/) {
            if (auto tab{ weakThis.get() })
            {
                if (tab->_zoomedPane)
                {
                    tab->Content(tab->_rootPane->GetRootElement());
                    tab->ExitZoom();
                }

                if (auto pane = weakPane.lock())
                {
                    // When a parent pane is selected, but one of its children
                    // close out under it we still need to update title/focus information
                    // but the GotFocus handler will rightly see that the _activePane
                    // did not actually change. Triggering
                    if (pane != tab->_activePane && !tab->_activePane->_IsLeaf())
                    {
                        tab->_UpdateActivePane(tab->_activePane);
                    }

                    for (auto i = tab->_mruPanes.begin(); i != tab->_mruPanes.end(); ++i)
                    {
                        if (*i == pane->Id())
                        {
                            tab->_mruPanes.erase(i);
                            break;
                        }
                    }
                }
            }
        });

        // box the event token so that we can give a reference to it in the
        // event handler.
        auto detachedToken = std::make_shared<winrt::event_token>();
        // Add a Detached event handler to the Pane to clean up tab state
        // and other event handlers when a pane is removed from this tab.
        *detachedToken = pane->Detached([weakThis, weakPane, gotFocusToken, lostFocusToken, closedToken, detachedToken](std::shared_ptr<Pane> /*sender*/) {
            // Make sure we do this at most once
            if (auto pane{ weakPane.lock() })
            {
                pane->Detached(*detachedToken);
                pane->GotFocus(gotFocusToken);
                pane->LostFocus(lostFocusToken);
                pane->Closed(closedToken);

                if (auto tab{ weakThis.get() })
                {
                    tab->_DetachEventHandlersFromContent(pane->Id().value());

                    for (auto i = tab->_mruPanes.begin(); i != tab->_mruPanes.end(); ++i)
                    {
                        if (*i == pane->Id())
                        {
                            tab->_mruPanes.erase(i);
                            break;
                        }
                    }
                }
            }
        });
    }

    void Tab::_AppendMoveMenuItems(winrt::Windows::UI::Xaml::Controls::MenuFlyout flyout)
    {
        auto weakThis{ get_weak() };

        // Move to new window
        {
            Controls::FontIcon moveTabToNewWindowTabSymbol;
            moveTabToNewWindowTabSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            moveTabToNewWindowTabSymbol.Glyph(L"\xE8A7");

            _moveToNewWindowMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    MoveTabArgs args{ L"new", MoveTabDirection::Forward };
                    ActionAndArgs actionAndArgs{ ShortcutAction::MoveTab, args };
                    tab->_dispatch.DoAction(*tab, actionAndArgs);
                }
            });
            _moveToNewWindowMenuItem.Text(RS_(L"MoveTabToNewWindowText"));
            _moveToNewWindowMenuItem.Icon(moveTabToNewWindowTabSymbol);

            const auto moveTabToNewWindowToolTip = RS_(L"MoveTabToNewWindowToolTip");
            WUX::Controls::ToolTipService::SetToolTip(_moveToNewWindowMenuItem, box_value(moveTabToNewWindowToolTip));
            Automation::AutomationProperties::SetHelpText(_moveToNewWindowMenuItem, moveTabToNewWindowToolTip);
        }

        // Move left
        {
            _moveLeftMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    MoveTabArgs args{ hstring{}, MoveTabDirection::Backward };
                    ActionAndArgs actionAndArgs{ ShortcutAction::MoveTab, args };
                    tab->_dispatch.DoAction(*tab, actionAndArgs);
                }
            });
            _moveLeftMenuItem.Text(RS_(L"TabMoveLeft"));
        }

        // Move right
        {
            _moveRightMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    MoveTabArgs args{ hstring{}, MoveTabDirection::Forward };
                    ActionAndArgs actionAndArgs{ ShortcutAction::MoveTab, args };
                    tab->_dispatch.DoAction(*tab, actionAndArgs);
                }
            });
            _moveRightMenuItem.Text(RS_(L"TabMoveRight"));
        }

        // Move to start (Agentmaster). The page computes the target slot (it reserves index 0 for
        // the pinned Manager tab), so this only raises the request.
        {
            _moveToStartMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->MoveTabToStartRequested.raise();
                }
            });
            _moveToStartMenuItem.Text(RS_(L"TabMoveToStart"));
        }

        // Move to end (Agentmaster).
        {
            _moveToEndMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->MoveTabToEndRequested.raise();
                }
            });
            _moveToEndMenuItem.Text(RS_(L"TabMoveToEnd"));
        }

        // Create a sub-menu for our extended move tab items.
        // Agentmaster: kept as a member (not a local) so the pinned Manager tab can gray
        // out the whole "Move tab" sub-menu. See DisableCloseAndMoveMenuItems().
        Controls::FontIcon moveSubMenuSymbol; // Agentmaster: a glyph on the "Move tab" submenu header
        moveSubMenuSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        moveSubMenuSymbol.Glyph(L"\xE7C2"); // Move
        _moveSubMenu.Text(RS_(L"TabMoveSubMenu"));
        _moveSubMenu.Icon(moveSubMenuSymbol);
        _moveSubMenu.Items().Append(_moveToNewWindowMenuItem);
        _moveSubMenu.Items().Append(_moveRightMenuItem);
        _moveSubMenu.Items().Append(_moveLeftMenuItem);
        _moveSubMenu.Items().Append(_moveToStartMenuItem); // Agentmaster
        _moveSubMenu.Items().Append(_moveToEndMenuItem); // Agentmaster
        flyout.Items().Append(_moveSubMenu);
    }

    // Method Description:
    // - Append the close menu items to the context menu flyout
    // Arguments:
    // - flyout - the menu flyout to which the close items must be appended
    // Return Value:
    // - the sub-item that we use for all the nested "close" entries. This
    //   enables subclasses to add their own entries to this menu.
    winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem Tab::_AppendCloseMenuItems(winrt::Windows::UI::Xaml::Controls::MenuFlyout flyout)
    {
        auto weakThis{ get_weak() };

        // Close tabs before (Agentmaster: the left-hand twin of "Close tabs to the right").
        // Upstream only ships "close tabs after"; this adds the mirror. It raises an Agentmaster
        // event (like "New Session Here" / "Move to start") rather than dispatching a ShortcutAction,
        // because there is no CloseTabsBefore action to bind — the page closes [0, index) directly,
        // skipping the pinned Manager tab. Inserted FIRST so the submenu reads left-to-right.
        _closeTabsBeforeMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->CloseTabsBeforeRequested.raise();
            }
        });
        _closeTabsBeforeMenuItem.Text(RS_(L"TabCloseBefore"));
        const auto closeTabsBeforeToolTip = RS_(L"TabCloseBeforeToolTip");

        WUX::Controls::ToolTipService::SetToolTip(_closeTabsBeforeMenuItem, box_value(closeTabsBeforeToolTip));
        Automation::AutomationProperties::SetHelpText(_closeTabsBeforeMenuItem, closeTabsBeforeToolTip);

        // Close tabs after
        _closeTabsAfterMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                CloseTabsAfterArgs args{ tab->_TabViewIndex };
                ActionAndArgs closeTabsAfter{ ShortcutAction::CloseTabsAfter, args };
                tab->_dispatch.DoAction(*tab, closeTabsAfter);
            }
        });
        _closeTabsAfterMenuItem.Text(RS_(L"TabCloseAfter"));
        const auto closeTabsAfterToolTip = RS_(L"TabCloseAfterToolTip");

        WUX::Controls::ToolTipService::SetToolTip(_closeTabsAfterMenuItem, box_value(closeTabsAfterToolTip));
        Automation::AutomationProperties::SetHelpText(_closeTabsAfterMenuItem, closeTabsAfterToolTip);

        // Close other tabs
        _closeOtherTabsMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                CloseOtherTabsArgs args{ tab->_TabViewIndex };
                ActionAndArgs closeOtherTabs{ ShortcutAction::CloseOtherTabs, args };
                tab->_dispatch.DoAction(*tab, closeOtherTabs);
            }
        });
        _closeOtherTabsMenuItem.Text(RS_(L"TabCloseOther"));
        const auto closeOtherTabsToolTip = RS_(L"TabCloseOtherToolTip");

        WUX::Controls::ToolTipService::SetToolTip(_closeOtherTabsMenuItem, box_value(closeOtherTabsToolTip));
        Automation::AutomationProperties::SetHelpText(_closeOtherTabsMenuItem, closeOtherTabsToolTip);

        // Close all tabs (Agentmaster) — the whole-window twin of "Close other tabs": closes EVERY tab
        // (this one included), skipping the pinned Manager tab. Raises an Agentmaster event; the page
        // snapshots all tabs and routes them through the SAME _RemoveTabs chokepoint as "Close tabs to
        // the left/right" (aggregate confirm + per-session archive bookkeeping + the Manager-tab skip).
        _closeAllTabsMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->CloseAllTabsRequested.raise();
            }
        });
        _closeAllTabsMenuItem.Text(RS_(L"TabCloseAll"));
        const auto closeAllTabsToolTip = RS_(L"TabCloseAllToolTip");

        WUX::Controls::ToolTipService::SetToolTip(_closeAllTabsMenuItem, box_value(closeAllTabsToolTip));
        Automation::AutomationProperties::SetHelpText(_closeAllTabsMenuItem, closeAllTabsToolTip);

        // ★ Favorite & close all tabs (Agentmaster, FAVORITES.md) — star every managed session in the
        // window, then close every tab: the batch twin of the single tab's "★ Favorite & Close" (a
        // one-gesture "keep all of these + close"). Built COLLAPSED; the page shows it at flyout-open only
        // when the window hosts >=1 managed session (SetFavoriteAndCloseAllVisible) — there is nothing to
        // favorite otherwise. Raises FavoriteAndCloseAllTabsRequested; the page routes through _RemoveTabs
        // with forceFavorite so the star disposition is pre-committed (a 2-button confirm, no re-offer).
        _favoriteAndCloseAllTabsMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->FavoriteAndCloseAllTabsRequested.raise();
            }
        });
        _favoriteAndCloseAllTabsMenuItem.Text(L"\x2605 Favorite & close all tabs"); // ★ (star matches the close-confirm's "★ Favorite & Close All")
        _favoriteAndCloseAllTabsMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only when the window hosts a managed session (page-driven)
        WUX::Controls::ToolTipService::SetToolTip(_favoriteAndCloseAllTabsMenuItem, box_value(winrt::hstring{ L"Star every managed session (find them later in Sessions), then close all tabs" }));

        // Of Same Folder / Other of Same Folder (Agentmaster) — the folder-scoped twins of "Close other
        // tabs", but keyed on the SESSION's effective work dir (inferred -> cwd) and CROSS-WINDOW: they
        // close every live managed session sharing this tab's session's folder (incl. / excl. this one).
        // Built COLLAPSED; the page shows them at flyout-open only for a managed session tab
        // (SetAgentFolderCloseVisible). Each raises its event; the page routes through the SHARED
        // _ConfirmAndCloseClaudeSessionsInFolder (the Manager board/tree "Close > Of Same Folder" twin),
        // which shows ONE confirm listing the titles, then closes. No icon — matches the icon-less close
        // sub-items beside them.
        _closeSessionsOfSameFolderMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->CloseSessionsOfSameFolderRequested.raise();
            }
        });
        _closeSessionsOfSameFolderMenuItem.Text(L"Of Same Folder");
        _closeSessionsOfSameFolderMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only for a managed session tab (page-driven)
        WUX::Controls::ToolTipService::SetToolTip(_closeSessionsOfSameFolderMenuItem, box_value(winrt::hstring{ L"Close every open session whose working directory is the same as this one \x2014 you'll see the full list first and can cancel. Each stays in Sessions, resumable anytime (nothing on disk is deleted)." }));

        _closeOtherSessionsOfSameFolderMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->CloseOtherSessionsOfSameFolderRequested.raise();
            }
        });
        _closeOtherSessionsOfSameFolderMenuItem.Text(L"Other of Same Folder");
        _closeOtherSessionsOfSameFolderMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only for a managed session tab (page-driven)
        WUX::Controls::ToolTipService::SetToolTip(_closeOtherSessionsOfSameFolderMenuItem, box_value(winrt::hstring{ L"Close every OTHER open session whose working directory is the same as this one \x2014 this session stays open. You'll see the full list first and can cancel. Each stays in Sessions, resumable anytime (nothing on disk is deleted)." }));

        // Close
        // Agentmaster: kept as a member (not a local) so the pinned Manager tab can gray
        // out the "Close tab" entry. See DisableCloseAndMoveMenuItems().
        Controls::FontIcon closeSymbol;
        closeSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        closeSymbol.Glyph(L"\xE711");

        _closeTabMenuItem.Click([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                tab->CloseRequested.raise(nullptr, nullptr);
            }
        });
        _closeTabMenuItem.Text(RS_(L"TabClose"));
        _closeTabMenuItem.Icon(closeSymbol);
        const auto closeTabToolTip = RS_(L"TabCloseToolTip");

        WUX::Controls::ToolTipService::SetToolTip(_closeTabMenuItem, box_value(closeTabToolTip));
        Automation::AutomationProperties::SetHelpText(_closeTabMenuItem, closeTabToolTip);

        // Create a sub-menu for our extended close items.
        // Agentmaster: kept as a member (not a local) so the pinned Manager tab can gray
        // out the whole "Close" sub-menu. See DisableCloseAndMoveMenuItems().
        Controls::FontIcon closeSubMenuSymbol; // Agentmaster: a glyph on the "Close" submenu header (same Cancel/X as "Close tab")
        closeSubMenuSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        closeSubMenuSymbol.Glyph(L"\xE711"); // Cancel
        _closeSubMenu.Text(RS_(L"TabCloseSubMenu"));
        _closeSubMenu.Icon(closeSubMenuSymbol);
        _closeSubMenu.Items().Append(_closeTabsBeforeMenuItem); // Agentmaster: left, then right, then "other"
        _closeSubMenu.Items().Append(_closeTabsAfterMenuItem);
        _closeSubMenu.Items().Append(_closeOtherTabsMenuItem);
        _closeSubMenu.Items().Append(_closeSessionsOfSameFolderMenuItem); // Agentmaster: folder-scoped, managed-session tabs only (cross-window)
        _closeSubMenu.Items().Append(_closeOtherSessionsOfSameFolderMenuItem); // Agentmaster: the same folder batch minus this session
        _closeSubMenu.Items().Append(_closeAllTabsMenuItem); // Agentmaster: close every tab in the window
        _closeSubMenu.Items().Append(_favoriteAndCloseAllTabsMenuItem); // Agentmaster (FAVORITES.md): star all + close all (shown only when a managed session exists)
        flyout.Items().Append(_closeSubMenu);

        flyout.Items().Append(_closeTabMenuItem);

        return _closeSubMenu;
    }

    // Method Description:
    // - Creates a context menu attached to the tab.
    // Currently contains elements allowing to select or
    // to close the current tab
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_CreateContextMenu()
    {
        auto weakThis{ get_weak() };

        // "Change tab color..."
        // Agentmaster: kept as a member (_chooseColorMenuItem) so the page can gray it per the
        // GLOBAL tab-color mode (SetColorPickerEnabled — NoColor/"Remove colors" disables it).
        {
            Controls::FontIcon colorPickSymbol;
            colorPickSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            colorPickSymbol.Glyph(L"\xE790");

            _chooseColorMenuItem.Click({ get_weak(), &Tab::_chooseColorClicked });
            _chooseColorMenuItem.Text(RS_(L"TabColorChoose"));
            _chooseColorMenuItem.Icon(colorPickSymbol);
            _chooseColorMenuItem.IsEnabled(!_colorPickerDisabled); // survives a menu rebuild with the toggle state intact

            const auto chooseColorToolTip = RS_(L"ChooseColorToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_chooseColorMenuItem, box_value(chooseColorToolTip));
            Automation::AutomationProperties::SetHelpText(_chooseColorMenuItem, chooseColorToolTip);
        }

        {
            // "Rename tab"
            // Agentmaster: kept as a member (_renameTabMenuItem) so the pinned Manager tab can gray
            // it out via DisableTabRename().
            Controls::FontIcon renameTabSymbol;
            renameTabSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            renameTabSymbol.Glyph(L"\xE8AC"); // Rename

            _renameTabMenuItem.Click({ get_weak(), &Tab::_renameTabClicked });
            _renameTabMenuItem.Text(RS_(L"RenameTabText"));
            _renameTabMenuItem.Icon(renameTabSymbol);

            const auto renameTabToolTip = RS_(L"RenameTabToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_renameTabMenuItem, box_value(renameTabToolTip));
            Automation::AutomationProperties::SetHelpText(_renameTabMenuItem, renameTabToolTip);
        }

        {
            // "Copy >" (Agentmaster) — a submenu mirroring the per-tab link badge's copy button
            // (TAB_OVERLAY.md / DESIGN §9.7): Session Id / Path / Branch / the REAL Claude & Codex launch
            // CLIs / the full Summary box / the whole Transcript. Each item raises CopySessionFieldRequested
            // with the copy-menu code; the page resolves THIS tab's managed session and routes the copy
            // through the SAME shared CopySessionField action the overlay + Manager menus use, so the three
            // copy menus can never drift apart. Labels match the overlay verbatim. Built COLLAPSED — the
            // page shows it (SetAgentCopyMenuVisible at flyout-open) only on a managed agent-session tab, so
            // a plain shell tab never carries these session-only items.
            Controls::FontIcon copySymbol;
            copySymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            copySymbol.Glyph(L"\xE8C8"); // Copy

            _copySessionSubMenu.Text(L"Copy");
            _copySessionSubMenu.Icon(copySymbol);
            _copySessionSubMenu.Visibility(WUX::Visibility::Collapsed); // shown only on a managed agent-session tab (page-driven)
            WUX::Controls::ToolTipService::SetToolTip(_copySessionSubMenu, box_value(winrt::hstring{ L"Copy this session's id, path, branch, current (unsent) prompt, launch command line, transcript, or full summary" }));

            const auto addCopyItem = [this, weakThis](const wchar_t* text, const wchar_t* tip, int32_t which) {
                Controls::MenuFlyoutItem item;
                item.Text(text);
                WUX::Controls::ToolTipService::SetToolTip(item, box_value(winrt::hstring{ tip }));
                item.Click([weakThis, which](auto&&, auto&&) {
                    if (auto tab{ weakThis.get() })
                    {
                        tab->CopySessionFieldRequested.raise(which);
                    }
                });
                _copySessionSubMenu.Items().Append(item);
                return item;
            };
            // The `which` codes + labels are the per-tab overlay copy menu's, verbatim (AgentCopyActions.h):
            // 0 Session Id, 1 Path, 2 Branch, 7 Current Prompt, 3 Claude CLI, 4 Codex CLI, 6 Summary, 5 Transcript.
            addCopyItem(L"Session Id", L"Copy the resumable conversation id (Codex: its rollout uuid)", 0);
            addCopyItem(L"Copy Path", L"Copy the session's working-directory path", 1);
            addCopyItem(L"Copy Branch Name", L"Copy the session's current git branch name", 2);
            // Copy Current Prompt (PENDING_INPUT.md) — the UNSENT draft in the session's input box, read
            // LIVE from this tab's buffer with the observer's recorded draft as the fallback. Built here
            // but revealed by SetAgentCopyMenuVisible only for a CLAUDE session: Codex's TUI has no ❯
            // rule-wrapped input box, so no draft is ever monitored (or readable) for it.
            _copyCurrentPromptItem = addCopyItem(L"Copy Current Prompt", L"Copy what is typed into this session's input box but NOT yet sent \x2014 read live from the terminal, falling back to the last observed draft (nothing is copied when the box is empty)", 7);
            // Both launch-CLI items are built; SetAgentCopyMenuVisible (page-driven at flyout-open) reveals
            // ONLY the one matching the session's agent — a Claude tab shows "Claude Launch CLI", a Codex tab
            // "Codex Launch CLI", never both (copying the other would synthesize a command for the wrong agent).
            _copyClaudeCliItem = addCopyItem(L"Claude Launch CLI", L"Copy the full claude.exe launch command line (with --settings hooks and flags)", 3);
            _copyCodexCliItem = addCopyItem(L"Codex Launch CLI", L"Copy the full codex launch command line", 4);
            addCopyItem(L"Summary", L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files)", 6);
            addCopyItem(L"Transcript", L"Copy the whole conversation as text (your prompts + the agent's replies)", 5);
        }

        {
            // "Mark Unread" (Agentmaster) — flash this tab's red attention ring until the user VISITS
            // (switches to) the tab. Works even when this IS the focused tab (no active-tab skip): only a
            // leave-then-return clears it. Built COLLAPSED — the page shows it (SetAgentMarkUnreadVisible
            // at flyout-open) only on a managed agent-session tab, like the "Copy >" submenu. Raises
            // MarkUnreadRequested; the page resolves THIS tab's session + drives the flash machinery.
            Controls::FontIcon markUnreadSymbol;
            markUnreadSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            markUnreadSymbol.Glyph(L"\xE7C1"); // Flag

            _markUnreadMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->MarkUnreadRequested.raise();
                }
            });
            _markUnreadMenuItem.Text(L"Mark Unread");
            _markUnreadMenuItem.Icon(markUnreadSymbol);
            _markUnreadMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only on a managed agent-session tab (page-driven)
            WUX::Controls::ToolTipService::SetToolTip(_markUnreadMenuItem, box_value(winrt::hstring{ L"Flash this tab's red attention ring until you switch to it" }));
        }

        {
            // "Open In Explorer" (Agentmaster) — open this session's EFFECTIVE working directory (the
            // inferred dir while it infers, else the launch cwd) in explorer.exe. Reuses the SAME shared
            // OpenSessionFolder action the per-tab overlay's folder button (Open Path) runs, so both
            // resolve the same folder. Built COLLAPSED — the page shows it (SetAgentOpenInExplorerVisible
            // at flyout-open) only on a managed agent-session tab, like "Mark Unread"; raises
            // OpenInExplorerRequested and the page resolves THIS tab's session + opens the folder.
            Controls::FontIcon openExplorerSymbol;
            openExplorerSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            openExplorerSymbol.Glyph(L"\xE838"); // FolderOpen

            _openInExplorerMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->OpenInExplorerRequested.raise();
                }
            });
            _openInExplorerMenuItem.Text(L"Open In Explorer");
            _openInExplorerMenuItem.Icon(openExplorerSymbol);
            _openInExplorerMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only on a managed agent-session tab (page-driven)
            WUX::Controls::ToolTipService::SetToolTip(_openInExplorerMenuItem, box_value(winrt::hstring{ L"Open this session's working directory in File Explorer" }));
        }

        {
            // "Move to Idle/Done" / "Move to Waiting-for-you" (Agentmaster, Waiting-for-you + Error triage)
            // — a status-adaptive manual state move: the tab-menu twin of the Triage Board card's "Move to
            // Idle/Done", plus its reverse. Built COLLAPSED with a placeholder label; the page shows it and
            // sets the direction-specific text + icon at flyout-open (SetAgentTriageMoveState) only on a
            // managed agent-session tab that is currently Waiting-for-you, Error, or Idle/Done (an Error
            // demote is the error DISMISSAL). EXPLICITLY separate from "Mark Unread": the promote direction
            // is a plain column move (no sticky flag, no ring flash). Raises TriageMoveRequested; the page
            // re-derives the direction from the LIVE state.
            _triageMoveMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->TriageMoveRequested.raise();
                }
            });
            _triageMoveMenuItem.Text(L"Move to Idle/Done"); // placeholder; the page overwrites text + icon by direction
            _triageMoveMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only on a managed agent-session tab in a triage state (page-driven)
        }

        {
            // "Favorite" / "Unfavorite" (Agentmaster, FAVORITES.md) — toggle this tab's session star
            // (the SessionStore "favorite" key), the SAME durable star the Sessions page's ★ column sets.
            // Built COLLAPSED — the page shows it + sets its label (Favorite vs Unfavorite) only on a
            // managed agent-session tab (SetAgentFavoriteState at flyout-open), like "Mark Unread".
            // Raises FavoriteRequested; the page resolves THIS tab's session + flips the star.
            Controls::FontIcon favoriteSymbol;
            favoriteSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            favoriteSymbol.Glyph(L"\xE734"); // FavoriteStar

            _favoriteMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->FavoriteRequested.raise();
                }
            });
            _favoriteMenuItem.Text(L"Favorite");
            _favoriteMenuItem.Icon(favoriteSymbol);
            _favoriteMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only on a managed agent-session tab (page-driven)
            WUX::Controls::ToolTipService::SetToolTip(_favoriteMenuItem, box_value(winrt::hstring{ L"Star this session so it's easy to find in Sessions (toggle)" }));
        }

        {
            // "Tag" (Agentmaster, bookmark tags) — open the TAG PANEL for this tab's session: a
            // first-row text box names a NEW tag (a "+" appears beside it once you type) over the
            // list of every existing tag, sorted by max(session activity) desc, click-toggled on
            // this session. The panel is an islands-safe raw Popup the PAGE owns — NOT a
            // MenuFlyoutSubItem hosting a TextBox, because a text box inside a Flyout gets no
            // keypresses under XAML Islands (the documented text-input trap; the Templates row and
            // the Sessions range popup learned the same lesson). Built COLLAPSED — the page shows it
            // (SetAgentTagVisible at flyout-open) only on a managed agent-session tab, like
            // "Favorite". Raises TagEditorRequested; the page resolves THIS tab's session + anchors
            // the panel under the tab.
            Controls::FontIcon tagSymbol;
            tagSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            tagSymbol.Glyph(L"\xE8A4"); // Bookmarks — matches the tab-header bookmark badges

            _tagMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->TagEditorRequested.raise();
                }
            });
            _tagMenuItem.Text(L"Tags");
            _tagMenuItem.Icon(tagSymbol);
            _tagMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only on a managed agent-session tab (page-driven)
            WUX::Controls::ToolTipService::SetToolTip(_tagMenuItem, box_value(winrt::hstring{ L"Bookmark-tag this session \x2014 name a new tag or toggle existing ones; each tag shows as a small bookmark at the bottom of the tab" }));
        }

        {
            // "Activate Tab (Shift+Click)" (Agentmaster, eager-init) — start this tab's DORMANT session's
            // claude IN PLACE (TermControl::InitializeWithSize), without switching the view. A WT background/
            // restored tab spawns its child lazily, only when first SHOWN, so a window-restored tab never
            // resumes until clicked; this wakes it where you are. Built COLLAPSED — the page shows it ONLY
            // when this tab's session is dormant (ConnectionState == NotConnected), at flyout-open
            // (SetAgentActivateVisible), and it is the FIRST menu item when present. Raises
            // ActivateSessionRequested. The label advertises the gesture twin: Shift+Left-Click the tab does
            // the same in place (TerminalPage::_OnTabPointerPressed), mirroring the Manager board/tree rows.
            Controls::FontIcon activateSymbol;
            activateSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            activateSymbol.Glyph(L"\xE768"); // Play — "start it"

            _activateSessionMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->ActivateSessionRequested.raise();
                }
            });
            _activateSessionMenuItem.Text(L"Activate Tab (Shift+Click)");
            _activateSessionMenuItem.Icon(activateSymbol);
            _activateSessionMenuItem.Visibility(WUX::Visibility::Collapsed); // shown only when this tab's session is dormant (page-driven)
            WUX::Controls::ToolTipService::SetToolTip(_activateSessionMenuItem, box_value(winrt::hstring{ L"Start this session's claude now, in place \x2014 it hasn't initialized yet (a restored tab you never opened). You can also Shift+Click the tab to do this." }));
        }

        {
            // "Duplicate tab"
            Controls::FontIcon duplicateTabSymbol;
            duplicateTabSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            duplicateTabSymbol.Glyph(L"\xF5ED");

            _duplicateTabMenuItem.Click({ get_weak(), &Tab::_duplicateTabClicked });
            _duplicateTabMenuItem.Text(RS_(L"DuplicateTabText"));
            _duplicateTabMenuItem.Icon(duplicateTabSymbol);

            const auto duplicateTabToolTip = RS_(L"DuplicateTabToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_duplicateTabMenuItem, box_value(duplicateTabToolTip));
            Automation::AutomationProperties::SetHelpText(_duplicateTabMenuItem, duplicateTabToolTip);

            // The SUBMENU twin (Agentmaster, launch-model picker): "Fork session" expanded into
            // Default + one item per configured model — a fork is a launch (`--resume <src>
            // --fork-session …`), so `--model <id>` picks what the FORKED session starts on. The
            // page repopulates it at flyout-open (SetForkSessionModels) and shows it ONLY for a
            // managed CLAUDE tab (a Codex fork takes no --model; a plain shell tab's "Fork
            // session" is really WT's duplicate-tab); the plain item above shows otherwise.
            // Starts collapsed — the first flyout-open decides which of the pair shows.
            Controls::FontIcon forkSubSymbol;
            forkSubSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            forkSubSymbol.Glyph(L"\xF5ED"); // Duplicate (same as the plain item)

            _forkSessionSubMenu.Text(RS_(L"DuplicateTabText"));
            _forkSessionSubMenu.Icon(forkSubSymbol);
            _forkSessionSubMenu.Visibility(WUX::Visibility::Collapsed);

            const auto forkSubToolTip = winrt::hstring{ L"Fork this session \x2014 branch its conversation into a new, independent session; the original is untouched. Pick the model the fork starts on, or Default. Edit the list in the Manager's Settings (\x2699) \x2192 Sessions \x2192 Launch models." };
            WUX::Controls::ToolTipService::SetToolTip(_forkSessionSubMenu, box_value(forkSubToolTip));
            Automation::AutomationProperties::SetHelpText(_forkSessionSubMenu, forkSubToolTip);
        }

        {
            // "Split tab"
            Controls::FontIcon splitTabSymbol;
            splitTabSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            splitTabSymbol.Glyph(L"\xF246"); // ViewDashboard

            _splitTabMenuItem.Click({ get_weak(), &Tab::_splitTabClicked });
            _splitTabMenuItem.Text(RS_(L"SplitTabText"));
            _splitTabMenuItem.Icon(splitTabSymbol);

            const auto splitTabToolTip = RS_(L"SplitTabToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_splitTabMenuItem, box_value(splitTabToolTip));
            Automation::AutomationProperties::SetHelpText(_splitTabMenuItem, splitTabToolTip);
        }

        {
            // "Close pane"
            _closePaneMenuItem.Click({ get_weak(), &Tab::_closePaneClicked });
            _closePaneMenuItem.Text(RS_(L"ClosePaneText"));

            const auto closePaneToolTip = RS_(L"ClosePaneToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_closePaneMenuItem, box_value(closePaneToolTip));
            Automation::AutomationProperties::SetHelpText(_closePaneMenuItem, closePaneToolTip);
        }

        {
            // "Export tab"
            Controls::FontIcon exportTabSymbol;
            exportTabSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            exportTabSymbol.Glyph(L"\xE74E"); // Save

            _exportTabMenuItem.Click({ get_weak(), &Tab::_exportTextClicked });
            _exportTabMenuItem.Text(RS_(L"ExportTabText"));
            _exportTabMenuItem.Icon(exportTabSymbol);

            const auto exportTabToolTip = RS_(L"ExportTabToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_exportTabMenuItem, box_value(exportTabToolTip));
            Automation::AutomationProperties::SetHelpText(_exportTabMenuItem, exportTabToolTip);
        }

        {
            // "Find"
            Controls::FontIcon findSymbol;
            findSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            findSymbol.Glyph(L"\xF78B"); // SearchMedium

            _findMenuItem.Click({ get_weak(), &Tab::_findClicked });
            _findMenuItem.Text(RS_(L"FindText"));
            _findMenuItem.Icon(findSymbol);

            const auto findToolTip = RS_(L"FindToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_findMenuItem, box_value(findToolTip));
            Automation::AutomationProperties::SetHelpText(_findMenuItem, findToolTip);
        }

        {
            // "New Session Here" (Agentmaster) — spawn a managed agent session in this tab's working
            // dir (a new, independent conversation). The page computes the dir + agent kind (Codex vs
            // Claude) and does the spawn, so this only raises the request (modelId "" = Default).
            Controls::FontIcon newSessionSymbol;
            newSessionSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            newSessionSymbol.Glyph(L"\xE710"); // Add

            _newSessionHereMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->NewSessionHereRequested.raise(winrt::hstring{});
                }
            });
            _newSessionHereMenuItem.Text(RS_(L"NewSessionHereText"));
            _newSessionHereMenuItem.Icon(newSessionSymbol);

            const auto newSessionHereToolTip = RS_(L"NewSessionHereToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_newSessionHereMenuItem, box_value(newSessionHereToolTip));
            Automation::AutomationProperties::SetHelpText(_newSessionHereMenuItem, newSessionHereToolTip);

            // The SUBMENU twin (Agentmaster, launch-model picker): the same "New Session Here",
            // expanded into Default + one item per configured model. The page repopulates it at
            // flyout-open (SetNewSessionModels — the models live in AppSettings.launchModels and
            // are editable without a restart) and shows exactly ONE of the pair: the submenu for
            // a Claude/shell tab, the plain item above for a Codex tab (Claude models don't apply)
            // or an empty list. Starts collapsed — the first flyout-open decides which shows.
            Controls::FontIcon newSessionSubSymbol;
            newSessionSubSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            newSessionSubSymbol.Glyph(L"\xE710"); // Add (matches the plain item)

            _newSessionHereSubMenu.Text(RS_(L"NewSessionHereText"));
            _newSessionHereSubMenu.Icon(newSessionSubSymbol);
            _newSessionHereSubMenu.Visibility(WUX::Visibility::Collapsed);

            // The tooltip carries the picker + where the model list is edited (the user asked the
            // tooltip to say so); the base sentence is the plain item's resource.
            const auto newSessionSubToolTip = newSessionHereToolTip + winrt::hstring{ L" \x2014 pick the model it starts on, or Default. Edit the list in the Manager's Settings (\x2699) \x2192 Sessions \x2192 Launch models." };
            WUX::Controls::ToolTipService::SetToolTip(_newSessionHereSubMenu, box_value(newSessionSubToolTip));
            Automation::AutomationProperties::SetHelpText(_newSessionHereSubMenu, newSessionSubToolTip);
        }

        {
            // "Restart session"
            Controls::FontIcon restartConnectionSymbol;
            restartConnectionSymbol.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            restartConnectionSymbol.Glyph(L"\xE72C");

            _restartConnectionMenuItem.Click([weakThis](auto&&, auto&&) {
                if (auto tab{ weakThis.get() })
                {
                    tab->_RestartActivePaneConnection();
                }
            });
            _restartConnectionMenuItem.Text(RS_(L"RestartConnectionText"));
            _restartConnectionMenuItem.Icon(restartConnectionSymbol);

            const auto restartConnectionToolTip = RS_(L"RestartConnectionToolTip");

            WUX::Controls::ToolTipService::SetToolTip(_restartConnectionMenuItem, box_value(restartConnectionToolTip));
            Automation::AutomationProperties::SetHelpText(_restartConnectionMenuItem, restartConnectionToolTip);
        }

        // Build the menu
        Controls::MenuFlyout contextMenuFlyout;
        Controls::MenuFlyoutSeparator menuSeparator;
        contextMenuFlyout.Items().Append(_activateSessionMenuItem); // Agentmaster (eager-init): "Activate Tab" — FIRST item, shown only when this tab's session is dormant
        contextMenuFlyout.Items().Append(_renameTabMenuItem);
        contextMenuFlyout.Items().Append(_copySessionSubMenu); // Agentmaster: "Copy >" directly below "Rename Tab" (hidden unless this tab hosts a managed session)
        contextMenuFlyout.Items().Append(_markUnreadMenuItem); // Agentmaster: "Mark Unread" — session-only, grouped under "Copy >"
        contextMenuFlyout.Items().Append(_openInExplorerMenuItem); // Agentmaster: "Open In Explorer" — session-only, beside "Mark Unread" (opens the session's working dir; the overlay Open Path twin)
        contextMenuFlyout.Items().Append(_triageMoveMenuItem); // Agentmaster (Waiting-for-you + Error triage): status-adaptive "Move to Idle/Done" / "Move to Waiting-for-you" — session-only, beside "Mark Unread"
        contextMenuFlyout.Items().Append(_favoriteMenuItem); // Agentmaster (FAVORITES.md): "Favorite"/"Unfavorite" — session-only, beside "Mark Unread"
        contextMenuFlyout.Items().Append(_tagMenuItem); // Agentmaster (bookmark tags): "Tag" — session-only, beside "Favorite"
        contextMenuFlyout.Items().Append(Controls::MenuFlyoutSeparator{}); // Agentmaster: separator above "Split tab" — sets rename/session ops apart from the layout group
        contextMenuFlyout.Items().Append(_splitTabMenuItem);
        _AppendMoveMenuItems(contextMenuFlyout);
        contextMenuFlyout.Items().Append(_chooseColorMenuItem); // Agentmaster: "Change tab color" moved below the "Move tab" submenu
        contextMenuFlyout.Items().Append(Controls::MenuFlyoutSeparator{}); // Agentmaster: separator above "Export tab" — sets the split/move layout group apart from export/find
        contextMenuFlyout.Items().Append(_exportTabMenuItem);
        contextMenuFlyout.Items().Append(_findMenuItem);
        contextMenuFlyout.Items().Append(Controls::MenuFlyoutSeparator{}); // Agentmaster: separator above "New Session Here" — sets export/find apart from the session ops (new/restart/fork)
        contextMenuFlyout.Items().Append(_newSessionHereMenuItem); // Agentmaster: "New Session Here" directly above "Restart session"
        contextMenuFlyout.Items().Append(_newSessionHereSubMenu); // Agentmaster (launch-model picker): its submenu twin — SetNewSessionModels shows exactly one of the pair, so they occupy one visual slot
        contextMenuFlyout.Items().Append(_restartConnectionMenuItem);
        // Agentmaster: place "Fork session" directly below "Restart session"
        contextMenuFlyout.Items().Append(_duplicateTabMenuItem);
        contextMenuFlyout.Items().Append(_forkSessionSubMenu); // Agentmaster (launch-model picker): its submenu twin — SetForkSessionModels shows exactly one of the pair, so they occupy one visual slot
        contextMenuFlyout.Items().Append(menuSeparator);

        auto closeSubMenu = _AppendCloseMenuItems(contextMenuFlyout);
        closeSubMenu.Items().Append(_closePaneMenuItem);

        // GH#5750 - When the context menu is dismissed with ESC, toss the focus
        // back to our control.
        contextMenuFlyout.Closed([weakThis](auto&&, auto&&) {
            if (auto tab{ weakThis.get() })
            {
                // GH#10112 - if we're opening the tab renamer, don't
                // immediately toss focus to the control. We don't want to steal
                // focus from the tab renamer.
                const auto& terminalControl{ tab->GetActiveTerminalControl() }; // maybe null
                // If we're
                // * NOT in a rename
                // * AND (the content isn't a TermControl, OR the term control doesn't have focus in the search box)
                if (!tab->_headerControl.InRename() &&
                    (terminalControl == nullptr || !terminalControl.SearchBoxEditInFocus()))
                {
                    tab->RequestFocusActiveControl.raise();
                }
            }
        });

        TabViewItem().ContextFlyout(contextMenuFlyout);
    }

    // Method Description:
    // - Enable menu items based on tab index and total number of tabs
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_EnableMenuItems()
    {
        const auto tabIndex = TabViewIndex();
        const auto numOfTabs = TabViewNumTabs();

        // Agentmaster: enabled only if there is a CLOSABLE other tab — i.e. one besides this tab
        // AND besides the reserved leading tabs (the pinned Manager tab, which _RemoveTabs skips).
        // With the Manager present (_reservedLeadingTabs == 1) a strip of just [Manager, thisTab]
        // leaves nothing to close, so this stays disabled (was: numOfTabs > 1, which lit up with
        // only the Manager as the "other" tab).
        _closeOtherTabsMenuItem.IsEnabled(numOfTabs > _reservedLeadingTabs + 1);

        // Agentmaster: "Close all tabs" / "★ Favorite & close all tabs" — enabled when there is at
        // least one CLOSABLE tab (one past the reserved leading tabs the Manager occupies). Since this
        // menu only ever appears on a non-Manager tab, that holds whenever the menu is shown, but gate
        // it for robustness (and so it greys out in the degenerate [Manager only] strip).
        _closeAllTabsMenuItem.IsEnabled(numOfTabs > _reservedLeadingTabs);
        _favoriteAndCloseAllTabsMenuItem.IsEnabled(numOfTabs > _reservedLeadingTabs);

        // Agentmaster: enabled only if there is a CLOSABLE tab to the left — i.e. a tab past the
        // reserved leading ones. From index 1 with the Manager at index 0 the only tab to the left
        // is the Manager (skipped by _RemoveTabs), so this stays disabled.
        _closeTabsBeforeMenuItem.IsEnabled(tabIndex > _reservedLeadingTabs);

        // enabled if there are other tabs on the right (the Manager is never to the right)
        _closeTabsAfterMenuItem.IsEnabled(tabIndex < numOfTabs - 1);

        // enabled if not left-most tab
        _moveLeftMenuItem.IsEnabled(tabIndex > 0);

        // enabled if not last tab
        _moveRightMenuItem.IsEnabled(tabIndex < numOfTabs - 1);

        // Agentmaster: "Move to start" enabled unless already left-most; "Move to end" unless last.
        _moveToStartMenuItem.IsEnabled(tabIndex > 0);
        _moveToEndMenuItem.IsEnabled(tabIndex < numOfTabs - 1);
    }

    // Agentmaster: permanently gray out the context-menu entries that would move or
    // close this tab ("Move tab", "Close", "Close tab"). Used by the pinned, leftmost
    // Manager tab, which must never be relocatable or closable (it mirrors the hidden
    // close button). _EnableMenuItems() only ever touches the inner items, never these
    // three, so a one-shot disable here is not undone on subsequent tab re-indexing.
    void Tab::DisableCloseAndMoveMenuItems()
    {
        ASSERT_UI_THREAD();

        _moveSubMenu.IsEnabled(false);
        _closeSubMenu.IsEnabled(false);
        _closeTabMenuItem.IsEnabled(false);
    }

    // Agentmaster: permanently disable renaming for this tab. Used by the pinned Manager tab,
    // whose title is fixed ("Agent Manager"). Grays out the context-menu "Rename Tab" entry and sets
    // _renameDisabled, which ActivateTabRenamer() honors so the double-tap and openTabRenamer
    // action are blocked too. Like DisableCloseAndMoveMenuItems(), this is a one-shot disable
    // that nothing in the tab re-indexing path undoes.
    void Tab::DisableTabRename()
    {
        ASSERT_UI_THREAD();

        _renameDisabled = true;
        _renameTabMenuItem.IsEnabled(false);
    }

    // Agentmaster (tab color modes — NoColor/"Remove colors"): enable/disable recoloring this tab.
    // Grays the context-menu "Change tab color..." entry and sets _colorPickerDisabled, which
    // AttachColorPicker() honors so the openTabColorPicker action / command-palette path is blocked
    // too. Unlike the Manager tab's one-shot disables this is a TOGGLE — the mode-aware paint seam
    // (TerminalPage::_ApplySessionTabColor) re-arms it the moment a colored mode repaints the tab,
    // and the page refreshes it at flyout-open (the SetAgentCopyMenuVisible idiom).
    void Tab::SetColorPickerEnabled(bool enabled)
    {
        ASSERT_UI_THREAD();

        _colorPickerDisabled = !enabled;
        _chooseColorMenuItem.IsEnabled(enabled);
    }

    // Agentmaster: show/hide the "Copy >" submenu (session id / path / branch / launch CLI / summary /
    // transcript). The page resolves whether THIS tab currently hosts a managed agent session and calls
    // this at flyout-open time (a '+' shell tab can become a claude after the menu is built), so the
    // submenu appears exactly where the per-tab overlay's copy button does — only on a linked Claude/Codex
    // tab, never on a plain shell or the pinned Manager tab.
    void Tab::SetAgentCopyMenuVisible(bool visible, bool isCodex)
    {
        ASSERT_UI_THREAD();

        _copySessionSubMenu.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
        // Reveal only the launch-CLI item matching this session's agent (meaningful only when the submenu
        // is visible) — Codex CLI for a Codex session, Claude CLI otherwise; never both.
        if (visible)
        {
            if (_copyClaudeCliItem)
            {
                _copyClaudeCliItem.Visibility(isCodex ? WUX::Visibility::Collapsed : WUX::Visibility::Visible);
            }
            if (_copyCodexCliItem)
            {
                _copyCodexCliItem.Visibility(isCodex ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
            }
            // "Copy Current Prompt" is CLAUDE-only (PENDING_INPUT.md): only Claude renders the ❯
            // rule-wrapped input box the draft is read from, so a Codex tab never offers it.
            if (_copyCurrentPromptItem)
            {
                _copyCurrentPromptItem.Visibility(isCodex ? WUX::Visibility::Collapsed : WUX::Visibility::Visible);
            }
        }
    }

    // Agentmaster: show/hide the "Mark Unread" item. Like SetAgentCopyMenuVisible, the page resolves
    // whether THIS tab hosts a managed agent session and calls this at flyout-open, so the item appears
    // only on a linked Claude/Codex tab (never a plain shell / the pinned Manager tab).
    void Tab::SetAgentMarkUnreadVisible(bool visible)
    {
        ASSERT_UI_THREAD();

        _markUnreadMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
    }

    // Agentmaster: show/hide the "Open In Explorer" item. Like SetAgentMarkUnreadVisible, the page
    // resolves whether THIS tab hosts a managed agent session and calls this at flyout-open, so the item
    // appears only on a linked Claude/Codex tab (never a plain shell / the pinned Manager tab) — matching
    // where the per-tab overlay's Open Path folder button appears.
    void Tab::SetAgentOpenInExplorerVisible(bool visible)
    {
        ASSERT_UI_THREAD();

        _openInExplorerMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
    }

    // Agentmaster (Waiting-for-you + Error triage): show/hide the status-adaptive triage-move item AND set
    // its label + icon by direction. toIdle == this session is Waiting-for-you or Error (offer "Move to
    // Idle/Done", the demote — fromError picks the error-dismissal tooltip); else it is Idle/Done (offer
    // "Move to Waiting-for-you", the plain promote). Page-driven at flyout-open — the page owns the
    // registry state lookup and re-derives the direction from the LIVE state on click. Hidden for the
    // remaining non-triage states (Running / NeedsApproval).
    void Tab::SetAgentTriageMoveState(bool visible, bool toIdle, bool fromError)
    {
        ASSERT_UI_THREAD();

        _triageMoveMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
        if (!visible)
        {
            return;
        }
        Controls::FontIcon icon;
        icon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        if (toIdle)
        {
            _triageMoveMenuItem.Text(L"Move to Idle/Done");
            icon.Glyph(L"\xE73E"); // CheckMark — "I've handled this; stop waiting on me / stop flagging this error" (matches the board card)
            if (fromError)
            {
                WUX::Controls::ToolTipService::SetToolTip(_triageMoveMenuItem, box_value(winrt::hstring{ L"Dismiss this errored session to Idle / Done \x2014 acknowledges the API error (it returns to Error if a new one lands)" }));
            }
            else
            {
                WUX::Controls::ToolTipService::SetToolTip(_triageMoveMenuItem, box_value(winrt::hstring{ L"Dismiss this \x201CWaiting-for-you\x201D session to Idle / Done (it returns to Waiting-for-you on its next turn)" }));
            }
        }
        else
        {
            _triageMoveMenuItem.Text(L"Move to Waiting-for-you");
            icon.Glyph(L"\xE823"); // Clock — put it back in the Waiting-for-you column (a plain move)
            WUX::Controls::ToolTipService::SetToolTip(_triageMoveMenuItem, box_value(winrt::hstring{ L"Move this session into the \x201CWaiting-for-you\x201D column \x2014 a plain move (no red-ring flash; unlike \x201CMark Unread\x201D it decays normally)" }));
        }
        _triageMoveMenuItem.Icon(icon);
    }

    // Agentmaster (FAVORITES.md): show/hide the "Favorite" item AND set its label to match the
    // session's current star (Favorite when not starred, Unfavorite when starred). Page-driven at
    // flyout-open — the page owns the SessionStore lookup (IsSessionFavorite) and the toggle.
    void Tab::SetAgentFavoriteState(bool visible, bool isFavorite)
    {
        ASSERT_UI_THREAD();

        _favoriteMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
        _favoriteMenuItem.Text(isFavorite ? L"Unfavorite" : L"Favorite");
    }

    // Agentmaster (bookmark tags): show/hide the "Tag" item. Like SetAgentFavoriteState, the page
    // resolves whether THIS tab hosts a managed agent session and calls this at flyout-open, so the
    // item appears only on a linked Claude/Codex tab (never a plain shell / the pinned Manager tab).
    void Tab::SetAgentTagVisible(bool visible)
    {
        ASSERT_UI_THREAD();

        _tagMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
    }

    // Agentmaster (eager-init): show/hide the "Activate Tab" item — shown ONLY when this tab's managed
    // session is DORMANT (its claude hasn't started: ConnectionState == NotConnected). Page-driven at
    // flyout-open (the page reads the live control state), so a tab that starts mid-session stops offering it.
    void Tab::SetAgentActivateVisible(bool visible)
    {
        ASSERT_UI_THREAD();

        _activateSessionMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
    }

    // Agentmaster (launch-model picker): choose which "New Session Here" form this tab's context menu
    // shows and (re)populate the submenu form. Page-driven at flyout-open — the page parses the LIVE
    // AppSettings.launchModels (editable in the cog without a restart) and resolves the tab's agent
    // kind. A Codex tab (the models are Claude models — a codex spawn takes no --model) or an EMPTY
    // list keeps the PLAIN item; otherwise the submenu shows: Default + one item per model, every
    // click raising NewSessionHereRequested with its model id ("" for Default) — the page spawns.
    // The rebuild is change-gated on the joined list (_newSessionModelsKey) so the usual flyout-open
    // with an unchanged settings list touches nothing.
    void Tab::SetNewSessionModels(const std::vector<std::pair<std::wstring, std::wstring>>& models, bool isCodex, std::function<void(std::function<void(winrt::hstring)>)> specify)
    {
        ASSERT_UI_THREAD();

        const bool plain = isCodex || models.empty();
        _newSessionHereMenuItem.Visibility(plain ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
        _newSessionHereSubMenu.Visibility(plain ? WUX::Visibility::Collapsed : WUX::Visibility::Visible);
        if (plain)
        {
            return; // keep the last-built submenu items around — the key gate makes a flip back free
        }
        std::wstring key;
        for (const auto& [name, id] : models)
        {
            key += name;
            key += L'\x1F'; // unit separator — can't appear in a settings-typed name/id line
            key += id;
            key += L'\x1E';
        }
        if (key == _newSessionModelsKey && _newSessionHereSubMenu.Items().Size() > 0)
        {
            return; // unchanged list — skip the item churn
        }
        _newSessionModelsKey = key;
        _newSessionHereSubMenu.Items().Clear();
        auto weakThis{ get_weak() };
        AgentFillModelPickItems(
            _newSessionHereSubMenu.Items(), models, [weakThis](winrt::hstring model) {
                if (auto tab{ weakThis.get() })
                {
                    tab->NewSessionHereRequested.raise(model);
                }
            },
            specify);
    }

    // Agentmaster (launch-model picker): the "Fork session" twin of SetNewSessionModels — swap the
    // plain item (WT's repurposed duplicate-tab, whose click dispatches the DuplicateTab action) for
    // the submenu form and (re)populate it. Shown ONLY for a managed CLAUDE tab with a non-empty
    // list: a fork is a launch, so the pick rides `--model <id>` onto the forked session's
    // commandline; a Codex fork takes no --model, and a shell tab's "Fork session" is really a
    // duplicate — both keep the plain item. Submenu picks raise ForkSessionRequested (the page
    // routes to _ForkManagedSessionById with the clicked tab's placement); the plain item keeps its
    // action dispatch, which reaches the same fork seam with model = Default. Change-gated like
    // SetNewSessionModels so an unchanged settings list costs nothing at flyout-open.
    void Tab::SetForkSessionModels(const std::vector<std::pair<std::wstring, std::wstring>>& models, bool isManagedClaude, std::function<void(std::function<void(winrt::hstring)>)> specify)
    {
        ASSERT_UI_THREAD();

        const bool plain = !isManagedClaude || models.empty();
        _duplicateTabMenuItem.Visibility(plain ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
        _forkSessionSubMenu.Visibility(plain ? WUX::Visibility::Collapsed : WUX::Visibility::Visible);
        if (plain)
        {
            return; // keep the last-built submenu items around — the key gate makes a flip back free
        }
        std::wstring key;
        for (const auto& [name, id] : models)
        {
            key += name;
            key += L'\x1F'; // unit separator — can't appear in a settings-typed name/id line
            key += id;
            key += L'\x1E';
        }
        if (key == _forkModelsKey && _forkSessionSubMenu.Items().Size() > 0)
        {
            return; // unchanged list — skip the item churn
        }
        _forkModelsKey = key;
        _forkSessionSubMenu.Items().Clear();
        auto weakThis{ get_weak() };
        AgentFillModelPickItems(
            _forkSessionSubMenu.Items(), models, [weakThis](winrt::hstring model) {
                if (auto tab{ weakThis.get() })
                {
                    tab->ForkSessionRequested.raise(model);
                }
            },
            specify);
    }

    // Agentmaster (FAVORITES.md): show/hide the "★ Favorite & close all tabs" close-submenu item. Unlike
    // the per-tab favorite (gated on THIS tab being a session), this is a WINDOW-scope action, so the page
    // shows it when the window hosts >=1 managed session — there is nothing to favorite otherwise. Built
    // collapsed in _AppendCloseMenuItems; the page calls this at flyout-open (alongside SetAgentFavoriteState).
    void Tab::SetFavoriteAndCloseAllVisible(bool visible)
    {
        ASSERT_UI_THREAD();

        _favoriteAndCloseAllTabsMenuItem.Visibility(visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
    }

    // Agentmaster: show/hide the "Close ▸ Of Same Folder" + "Other of Same Folder" close-submenu items.
    // Managed agent-session tabs only (they close every live session sharing THIS session's effective
    // work dir — a shell/Manager tab has no session/folder). Built collapsed in _AppendCloseMenuItems;
    // the page calls this at flyout-open (alongside SetAgentOpenInExplorerVisible etc.).
    void Tab::SetAgentFolderCloseVisible(bool visible)
    {
        ASSERT_UI_THREAD();

        const auto vis = visible ? WUX::Visibility::Visible : WUX::Visibility::Collapsed;
        _closeSessionsOfSameFolderMenuItem.Visibility(vis);
        _closeOtherSessionsOfSameFolderMenuItem.Visibility(vis);
    }

    void Tab::UpdateTabViewIndex(const uint32_t idx, const uint32_t numTabs, const uint32_t reservedLeading)
    {
        ASSERT_UI_THREAD();

        TabViewIndex(idx);
        TabViewNumTabs(numTabs);
        _reservedLeadingTabs = reservedLeading; // Agentmaster
        _EnableMenuItems();
        _UpdateSwitchToTabKeyChord();
    }

    // Method Description:
    // Returns the tab color, if any
    // Arguments:
    // - <none>
    // Return Value:
    // - The tab's color, if any
    std::optional<winrt::Windows::UI::Color> Tab::GetTabColor()
    {
        ASSERT_UI_THREAD();

        std::optional<winrt::Windows::UI::Color> contentTabColor;
        if (const auto& content{ GetActiveContent() })
        {
            if (const auto color = content.TabColor())
            {
                contentTabColor = color.Value();
            }
        }

        // A Tab's color will be the result of layering a variety of sources,
        // from the bottom up:
        //
        // Color                |             | Set by
        // -------------------- | --          | --
        // Runtime Color        | _optional_  | Color Picker / `setTabColor` action
        // Content Tab Color    | _optional_  | Profile's `tabColor`, or a color set by VT (whatever the tab's content wants)
        // Theme Tab Background | _optional_  | `tab.backgroundColor` in the theme (handled in _RecalculateAndApplyTabColor)
        // Tab Default Color    | **default** | TabView in XAML
        //
        // coalesce will get us the first of these values that's
        // actually set, with nullopt being our sentinel for "use the default
        // tabview color" (and clear out any colors we've set).

        return til::coalesce(_runtimeTabColor,
                             contentTabColor,
                             std::optional<Windows::UI::Color>(std::nullopt));
    }

    // Method Description:
    // - Sets the runtime tab background color to the color chosen by the user
    // - Sets the tab foreground color depending on the luminance of
    // the background color
    // Arguments:
    // - color: the color the user picked for their tab
    // Return Value:
    // - <none>
    void Tab::SetRuntimeTabColor(const winrt::Windows::UI::Color& color)
    {
        ASSERT_UI_THREAD();

        _runtimeTabColor.emplace(color);
        _RecalculateAndApplyTabColor();
        _tabStatus.TabColorIndicator(color);
        TabColorChanged.raise(); // Agentmaster: propagate to same-dir tabs + persist (per-dir color)
    }

    // Method Description:
    // - Clear the custom runtime color of the tab, if any color is set. This
    //   will re-apply whatever the tab's base color should be (either the color
    //   from the control, the theme, or the default tab color.)
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::ResetRuntimeTabColor()
    {
        ASSERT_UI_THREAD();

        _runtimeTabColor.reset();
        _RecalculateAndApplyTabColor();
        _tabStatus.TabColorIndicator(GetTabColor().value_or(Windows::UI::Colors::Transparent()));
        TabColorChanged.raise(); // Agentmaster: propagate the reset to same-dir tabs + drop the persisted color
    }

    // Agentmaster (tab color modes — NoColor/"Remove colors"): park (suspended=true) or restore
    // (false) this tab's runtime color. Parking sheds the VISUAL exactly like ResetRuntimeTabColor
    // but keeps the value in _suspendedTabColor, so persistence (GetPersistableTabColor /
    // BuildStartupActions' setColor fold) still records the color the tab HAD — the mode never
    // voids anything — and leaving the mode brings it back. Deliberately raises NO TabColorChanged
    // in either direction: nothing conceptually changed (the color is parked, not removed), the
    // page's color-changed handler is what CALLS this (re-entrancy would loop), and the restore is
    // immediately followed by the mode repaint for managed tabs anyway. Both directions are no-ops
    // when there's nothing to park/restore, so strip-wide sweeps are idempotent. A color set while
    // parked (runtime non-empty on resume) wins over the parked one — latest intent rules.
    void Tab::SetTabColorSuspended(bool suspended)
    {
        ASSERT_UI_THREAD();

        if (suspended)
        {
            if (_runtimeTabColor)
            {
                _suspendedTabColor = _runtimeTabColor;
                _runtimeTabColor.reset();
                _RecalculateAndApplyTabColor();
                _tabStatus.TabColorIndicator(GetTabColor().value_or(Windows::UI::Colors::Transparent()));
            }
        }
        else if (_suspendedTabColor)
        {
            if (!_runtimeTabColor)
            {
                _runtimeTabColor = _suspendedTabColor;
                _RecalculateAndApplyTabColor();
                _tabStatus.TabColorIndicator(GetTabColor().value_or(Windows::UI::Colors::Transparent()));
            }
            _suspendedTabColor.reset();
        }
    }

    winrt::Windows::UI::Xaml::Media::Brush Tab::_BackgroundBrush()
    {
        Media::Brush terminalBrush{ nullptr };
        if (const auto& c{ GetActiveContent() })
        {
            terminalBrush = c.BackgroundBrush();
        }
        return terminalBrush;
    }

    // - Get the total number of leaf panes in this tab. This will be the number
    //   of actual controls hosted by this tab.
    // Arguments:
    // - <none>
    // Return Value:
    // - The total number of leaf panes hosted by this tab.
    int Tab::GetLeafPaneCount() const noexcept
    {
        ASSERT_UI_THREAD();

        return _rootPane->GetLeafPaneCount();
    }

    // Method Description:
    // - Calculate if a split is possible with the given direction and size.
    // - Converts Automatic splits to an appropriate direction depending on space.
    // Arguments:
    // - splitType: what direction to split.
    // - splitSize: how big the new split should be.
    // - availableSpace: how much space there is to work with.
    // Return value:
    // - This will return nullopt if a split of the given size/direction was not possible,
    //   or it will return the split direction with automatic converted to a cardinal direction.
    std::optional<SplitDirection> Tab::PreCalculateCanSplit(SplitDirection splitType,
                                                            const float splitSize,
                                                            winrt::Windows::Foundation::Size availableSpace) const
    {
        ASSERT_UI_THREAD();

        return _rootPane->PreCalculateCanSplit(_activePane, splitType, splitSize, availableSpace).value_or(std::nullopt);
    }

    // Method Description:
    // - Updates the zoomed pane when the focus changes
    // Arguments:
    // - newFocus: the new pane to be zoomed
    // Return Value:
    // - <none>
    void Tab::UpdateZoom(std::shared_ptr<Pane> newFocus)
    {
        ASSERT_UI_THREAD();

        // clear the existing content so the old zoomed pane can be added back to the root tree
        Content(nullptr);
        _rootPane->Restore(_zoomedPane);
        _zoomedPane = newFocus;
        _rootPane->Maximize(_zoomedPane);
        Content(_zoomedPane->GetRootElement());
    }

    // Method Description:
    // - Toggle our zoom state.
    //   * If we're not zoomed, then zoom the active pane, making it take the
    //     full size of the tab. We'll achieve this by changing our response to
    //     Tab::GetTabContent, so that it'll return the zoomed pane only.
    //   *  If we're currently zoomed on a pane, un-zoom that pane.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::ToggleZoom()
    {
        ASSERT_UI_THREAD();

        if (_zoomedPane)
        {
            ExitZoom();
        }
        else
        {
            EnterZoom();
        }
    }

    void Tab::EnterZoom()
    {
        ASSERT_UI_THREAD();

        // Clear the content first, because with parent focusing it is possible
        // to zoom the root pane, but setting the content will not trigger the
        // property changed event since it is the same and you would end up with
        // an empty tab.
        Content(nullptr);
        _zoomedPane = _activePane;
        _rootPane->Maximize(_zoomedPane);
        // Update the tab header to show the magnifying glass
        _tabStatus.IsPaneZoomed(true);
        Content(_zoomedPane->GetRootElement());
    }
    void Tab::ExitZoom()
    {
        ASSERT_UI_THREAD();

        Content(nullptr);
        _rootPane->Restore(_zoomedPane);
        _zoomedPane = nullptr;
        // Update the tab header to hide the magnifying glass
        _tabStatus.IsPaneZoomed(false);
        Content(_rootPane->GetRootElement());
    }

    bool Tab::IsZoomed()
    {
        ASSERT_UI_THREAD();

        return _zoomedPane != nullptr;
    }

    TermControl _termControlFromPane(const auto& pane)
    {
        if (const auto content{ pane->GetContent() })
        {
            if (const auto termContent{ content.try_as<winrt::TerminalApp::TerminalPaneContent>() })
            {
                return termContent.GetTermControl();
            }
        }
        return nullptr;
    }

    // Method Description:
    // - Toggle read-only mode on the active pane
    // - If a parent pane is selected, this will ensure that all children have
    //   the same read-only status.
    void Tab::TogglePaneReadOnly()
    {
        ASSERT_UI_THREAD();

        auto hasReadOnly = false;
        auto allReadOnly = true;
        _activePane->WalkTree([&](const auto& p) {
            if (const auto& control{ _termControlFromPane(p) })
            {
                hasReadOnly |= control.ReadOnly();
                allReadOnly &= control.ReadOnly();
            }
        });
        _activePane->WalkTree([&](const auto& p) {
            if (const auto& control{ _termControlFromPane(p) })
            {
                // If all controls have the same read only state then just toggle
                if (allReadOnly || !hasReadOnly)
                {
                    control.ToggleReadOnly();
                }
                // otherwise set to all read only.
                else if (!control.ReadOnly())
                {
                    control.ToggleReadOnly();
                }
            }
        });
    }

    // Method Description:
    // - Set read-only mode on the active pane
    // - If a parent pane is selected, this will ensure that all children have
    //   the same read-only status.
    void Tab::SetPaneReadOnly(const bool readOnlyState)
    {
        auto hasReadOnly = false;
        auto allReadOnly = true;
        _activePane->WalkTree([&](const auto& p) {
            if (const auto& control{ _termControlFromPane(p) })
            {
                hasReadOnly |= control.ReadOnly();
                allReadOnly &= control.ReadOnly();
            }
        });
        _activePane->WalkTree([&](const auto& p) {
            if (const auto& control{ _termControlFromPane(p) })
            {
                // If all controls have the same read only state then just disable
                if (allReadOnly || !hasReadOnly)
                {
                    control.SetReadOnly(readOnlyState);
                }
                // otherwise set to all read only.
                else if (!control.ReadOnly())
                {
                    control.SetReadOnly(readOnlyState);
                }
            }
        });
    }

    // Method Description:
    // - Calculates if the tab is read-only.
    // The tab is considered read-only if one of the panes is read-only.
    // If, after the calculation, the tab is read-only we hide the close button on the tab view item
    void Tab::_RecalculateAndApplyReadOnly()
    {
        const auto control = GetActiveTerminalControl();
        if (control)
        {
            const auto isReadOnlyActive = control.ReadOnly();
            _tabStatus.IsReadOnlyActive(isReadOnlyActive);
        }

        ReadOnly(_rootPane->ContainsReadOnly());
        _updateIsClosable();

        // Update all the visuals on all our panes, so they can update their
        // border colors accordingly.
        _rootPane->WalkTree([](const auto& p) { p->UpdateVisuals(); });
    }

    std::shared_ptr<Pane> Tab::GetActivePane() const
    {
        ASSERT_UI_THREAD();

        return _activePane;
    }

    // Method Description:
    // - Creates a text for the title run in the tool tip by returning tab title
    // or <profile name>: <tab title> if the profile name differs from the title
    // Arguments:
    // - <none>
    // Return Value:
    // - The value to populate in the title run of the tool tip
    winrt::hstring Tab::_CreateToolTipTitle()
    {
        if (const auto profile{ GetFocusedProfile() })
        {
            const auto profileName{ profile.Name() };
            if (profileName != Title())
            {
                return til::hstring_format(FMT_COMPILE(L"{}: {}"), profileName, Title());
            }
        }

        return Title();
    }

    // Method Description:
    // - Toggle broadcasting input to all the panes in this tab.
    void Tab::ToggleBroadcastInput()
    {
        const bool newIsBroadcasting = !_tabStatus.IsInputBroadcastActive();
        _tabStatus.IsInputBroadcastActive(newIsBroadcasting);
        _rootPane->EnableBroadcast(newIsBroadcasting);

        auto weakThis{ get_weak() };

        // When we change the state of broadcasting, add or remove event
        // handlers appropriately, so that controls won't be propagating events
        // needlessly if no one is listening.

        _rootPane->WalkTree([&](const auto& p) {
            const auto paneId = p->Id();
            if (!paneId.has_value())
            {
                return;
            }
            if (const auto& control{ _termControlFromPane(p) })
            {
                auto it = _contentEvents.find(*paneId);
                if (it != _contentEvents.end())
                {
                    auto& events = it->second;

                    // Always clear out old ones, just in case.
                    events.KeySent.revoke();
                    events.CharSent.revoke();
                    events.StringSent.revoke();

                    // Agentmaster: a managed Claude pane is neither a broadcast SINK (skipped in
                    // Pane::Broadcast*) nor a SOURCE — don't wire its KeySent/CharSent/StringSent, so its
                    // input is never echoed into sibling shells. Its stdin is the orchestrator's alone.
                    if (newIsBroadcasting && !p->IsAgentManaged())
                    {
                        _addBroadcastHandlers(control, events);
                    }
                }
            }
        });
    }

    void Tab::_addBroadcastHandlers(const TermControl& termControl, ContentEventTokens& events)
    {
        auto weakThis{ get_weak() };
        // ADD EVENT HANDLERS HERE
        events.KeySent = termControl.KeySent(winrt::auto_revoke, [weakThis](auto&& sender, auto&& e) {
            if (const auto tab{ weakThis.get() })
            {
                if (tab->_tabStatus.IsInputBroadcastActive())
                {
                    tab->_rootPane->BroadcastKey(sender.try_as<TermControl>(),
                                                 e.VKey(),
                                                 e.ScanCode(),
                                                 e.Modifiers(),
                                                 e.KeyDown());
                }
            }
        });

        events.CharSent = termControl.CharSent(winrt::auto_revoke, [weakThis](auto&& sender, auto&& e) {
            if (const auto tab{ weakThis.get() })
            {
                if (tab->_tabStatus.IsInputBroadcastActive())
                {
                    tab->_rootPane->BroadcastChar(sender.try_as<TermControl>(),
                                                  e.Character(),
                                                  e.ScanCode(),
                                                  e.Modifiers());
                }
            }
        });

        events.StringSent = termControl.StringSent(winrt::auto_revoke, [weakThis](auto&& sender, auto&& e) {
            if (const auto tab{ weakThis.get() })
            {
                if (tab->_tabStatus.IsInputBroadcastActive())
                {
                    tab->_rootPane->BroadcastString(sender.try_as<TermControl>(),
                                                    e.Text());
                }
            }
        });
    }

    void Tab::ThemeColor(const winrt::Microsoft::Terminal::Settings::Model::ThemeColor& focused,
                         const winrt::Microsoft::Terminal::Settings::Model::ThemeColor& unfocused,
                         const til::color& tabRowColor)
    {
        ASSERT_UI_THREAD();

        _themeColor = focused;
        _unfocusedThemeColor = unfocused;
        _tabRowColor = tabRowColor;
        _RecalculateAndApplyTabColor();
    }

    // Method Description:
    // - This function dispatches a function to the UI thread to recalculate
    //   what this tab's current background color should be. If a color is set,
    //   it will apply the given color to the tab's background. Otherwise, it
    //   will clear the tab's background color.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_RecalculateAndApplyTabColor()
    {
        // GetTabColor will return the color set by the color picker, or the
        // color specified in the profile. If neither of those were set,
        // then look to _themeColor to see if there's a value there.
        // Otherwise, clear our color, falling back to the TabView defaults.
        const auto currentColor = GetTabColor();
        if (currentColor.has_value())
        {
            _ApplyTabColorOnUIThread(currentColor.value());
        }
        else if (_themeColor != nullptr)
        {
            // Safely get the active control's brush.
            const Media::Brush terminalBrush{ _BackgroundBrush() };

            if (const auto themeBrush{ _themeColor.Evaluate(Application::Current().Resources(), terminalBrush, false) })
            {
                // ThemeColor.Evaluate will get us a Brush (because the
                // TermControl could have an acrylic BG, for example). Take
                // that brush, and get the color out of it. We don't really
                // want to have the tab items themselves be acrylic.
                _ApplyTabColorOnUIThread(til::color{ ThemeColor::ColorFromBrush(themeBrush) });
            }
            else
            {
                _ClearTabBackgroundColor();
            }
        }
        else
        {
            _ClearTabBackgroundColor();
        }
    }

    // Agentmaster (PENDING_INPUT.md): the tab's CURRENT effective header background. WT renders a colored
    // tab very differently by state (_ApplyTabColorOnUIThread): a SELECTED tab uses the full color, a
    // DESELECTED tab uses it at 30% opacity layered over the tab-row color (much darker), and it picks a
    // contrasting text color for each. The pending-dots indicator reads THIS so its light/dark pick follows
    // the same selected/unselected light/dark shift the tab itself has — otherwise the dots, always
    // contrasted against the full color, wash out on an unfocused (deselected) colored tab.
    winrt::Windows::UI::Color Tab::CurrentEffectiveTabBackground(const winrt::Windows::UI::Color& fallbackSource)
    {
        ASSERT_UI_THREAD();

        const auto tabColor = GetTabColor();
        const til::color source{ tabColor.has_value() ? til::color{ tabColor.value() } : til::color{ fallbackSource } };

        const auto item = TabViewItem();
        const bool selected = item && item.IsSelected();

        // Mirror _ApplyTabColorOnUIThread: selected => the full color; deselected => the color at 30%
        // (alpha 77) opacity. Both layered over the tab-row color, so the result is the actual lightness
        // drawn behind the header (a transparent deselected tint reveals the darker row beneath).
        const til::color effective = selected ? source.layer_over(_tabRowColor) :
                                                source.with_alpha(77).layer_over(_tabRowColor);
        return effective;
    }

    // Method Description:
    // - Applies the given color to the background of this tab's TabViewItem.
    // - Sets the tab foreground color depending on the luminance of
    // the background color
    // - This method should only be called on the UI thread.
    // Arguments:
    // - uiColor: the color the user picked for their tab
    // Return Value:
    // - <none>
    void Tab::_ApplyTabColorOnUIThread(const winrt::Windows::UI::Color& uiColor)
    {
        constexpr auto lightnessThreshold = 0.6f;
        const til::color color{ uiColor };
        Media::SolidColorBrush selectedTabBrush{};
        Media::SolidColorBrush deselectedTabBrush{};
        Media::SolidColorBrush fontBrush{};
        Media::SolidColorBrush deselectedFontBrush{};
        Media::SolidColorBrush secondaryFontBrush{};
        Media::SolidColorBrush hoverTabBrush{};
        Media::SolidColorBrush subtleFillColorSecondaryBrush;
        Media::SolidColorBrush subtleFillColorTertiaryBrush;

        // calculate the luminance of the current color and select a font
        // color based on that
        // see https://www.w3.org/TR/WCAG20/#relativeluminancedef
        if (ColorFix::GetLightness(color) >= lightnessThreshold)
        {
            auto subtleFillColorSecondary = winrt::Windows::UI::Colors::Black();
            subtleFillColorSecondary.A = 0x09;
            subtleFillColorSecondaryBrush.Color(subtleFillColorSecondary);
            auto subtleFillColorTertiary = winrt::Windows::UI::Colors::Black();
            subtleFillColorTertiary.A = 0x06;
            subtleFillColorTertiaryBrush.Color(subtleFillColorTertiary);
        }
        else
        {
            auto subtleFillColorSecondary = winrt::Windows::UI::Colors::White();
            subtleFillColorSecondary.A = 0x0F;
            subtleFillColorSecondaryBrush.Color(subtleFillColorSecondary);
            auto subtleFillColorTertiary = winrt::Windows::UI::Colors::White();
            subtleFillColorTertiary.A = 0x0A;
            subtleFillColorTertiaryBrush.Color(subtleFillColorTertiary);
        }

        // The tab font should be based on the evaluated appearance of the tab color layered on tab row.
        const auto layeredTabColor = color.layer_over(_tabRowColor);
        if (ColorFix::GetLightness(layeredTabColor) >= lightnessThreshold)
        {
            fontBrush.Color(winrt::Windows::UI::Colors::Black());
            auto secondaryFontColor = winrt::Windows::UI::Colors::Black();
            // For alpha value see: https://github.com/microsoft/microsoft-ui-xaml/blob/7a33ad772d77d908aa6b316ec24e6d2eb3ebf571/dev/CommonStyles/Common_themeresources_any.xaml#L269
            secondaryFontColor.A = 0x9E;
            secondaryFontBrush.Color(secondaryFontColor);
        }
        else
        {
            fontBrush.Color(winrt::Windows::UI::Colors::White());
            auto secondaryFontColor = winrt::Windows::UI::Colors::White();
            // For alpha value see: https://github.com/microsoft/microsoft-ui-xaml/blob/7a33ad772d77d908aa6b316ec24e6d2eb3ebf571/dev/CommonStyles/Common_themeresources_any.xaml#L14
            secondaryFontColor.A = 0xC5;
            secondaryFontBrush.Color(secondaryFontColor);
        }

        selectedTabBrush.Color(color);

        // Start with the current tab color, set to Opacity=.3
        auto deselectedTabColor = color.with_alpha(77); // 255 * .3 = 77

        // If we DON'T have a color set from the color picker, or the profile's
        // tabColor, but if we have an unfocused color in the theme, use the
        // unfocused theme color here instead.
        if (!GetTabColor().has_value() &&
            _unfocusedThemeColor != nullptr)
        {
            // Safely get the active control's brush.
            const Media::Brush terminalBrush{ _BackgroundBrush() };

            // Get the color of the brush.
            if (const auto themeBrush{ _unfocusedThemeColor.Evaluate(Application::Current().Resources(), terminalBrush, false) })
            {
                // We did figure out the brush. Get the color out of it. If it
                // was "accent" or "terminalBackground", then we're gonna set
                // the alpha to .3 manually here.
                // (ThemeColor::UnfocusedTabOpacity will do this for us). If the
                // user sets both unfocused and focused tab.background to
                // terminalBackground, this will allow for some differentiation
                // (and is generally just sensible).
                deselectedTabColor = til::color{ ThemeColor::ColorFromBrush(themeBrush) }.with_alpha(_unfocusedThemeColor.UnfocusedTabOpacity());
            }
        }

        // currently if a tab has a custom color, a deselected state is
        // signified by using the same color with a bit of transparency
        deselectedTabBrush.Color(deselectedTabColor.with_alpha(255));
        deselectedTabBrush.Opacity(deselectedTabColor.a / 255.f);

        hoverTabBrush.Color(color);
        hoverTabBrush.Opacity(0.6);

        // Account for the color of the tab row when setting the color of text
        // on inactive tabs. Consider:
        // * black active tabs
        // * on a white tab row
        // * with a transparent inactive tab color
        //
        // We don't want that to result in white text on a white tab row for
        // inactive tabs.
        const auto deselectedActualColor = deselectedTabColor.layer_over(_tabRowColor);
        if (ColorFix::GetLightness(deselectedActualColor) >= lightnessThreshold)
        {
            deselectedFontBrush.Color(winrt::Windows::UI::Colors::Black());
        }
        else
        {
            deselectedFontBrush.Color(winrt::Windows::UI::Colors::White());
        }

        // Add the empty theme dictionaries
        const auto& tabItemThemeResources{ TabViewItem().Resources().ThemeDictionaries() };
        ResourceDictionary lightThemeDictionary;
        ResourceDictionary darkThemeDictionary;
        ResourceDictionary highContrastThemeDictionary;
        tabItemThemeResources.Insert(winrt::box_value(L"Light"), lightThemeDictionary);
        tabItemThemeResources.Insert(winrt::box_value(L"Dark"), darkThemeDictionary);
        tabItemThemeResources.Insert(winrt::box_value(L"HighContrast"), highContrastThemeDictionary);

        // Apply the color to the tab
        TabViewItem().Background(deselectedTabBrush);

        // Now actually set the resources we want in them.
        // Before, we used to put these on the ResourceDictionary directly.
        // However, HighContrast mode may require some adjustments. So let's just add
        //   all three so we can make those adjustments on the HighContrast version.
        for (const auto& [k, v] : tabItemThemeResources)
        {
            const bool isHighContrast = winrt::unbox_value<hstring>(k) == L"HighContrast";
            const auto& currentDictionary = v.as<ResourceDictionary>();

            // TabViewItem.Background
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackground"), selectedTabBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundSelected"), selectedTabBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundPointerOver"), isHighContrast ? fontBrush : hoverTabBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundPressed"), selectedTabBrush);

            // TabViewItem.Foreground (aka text)
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForeground"), deselectedFontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForegroundSelected"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForegroundPointerOver"), isHighContrast ? selectedTabBrush : fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForegroundPressed"), fontBrush);

            // TabViewItem.CloseButton.Foreground (aka X)
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForeground"), deselectedFontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForegroundPressed"), isHighContrast ? deselectedFontBrush : secondaryFontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForegroundPointerOver"), isHighContrast ? deselectedFontBrush : fontBrush);

            // TabViewItem.CloseButton.Foreground _when_ interacting with the tab
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderPressedCloseButtonForeground"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderPointerOverCloseButtonForeground"), isHighContrast ? selectedTabBrush : fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderSelectedCloseButtonForeground"), fontBrush);

            // TabViewItem.CloseButton.Background (aka X button)
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackgroundPressed"), isHighContrast ? selectedTabBrush : subtleFillColorTertiaryBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackgroundPointerOver"), isHighContrast ? selectedTabBrush : subtleFillColorSecondaryBrush);

            // A few miscellaneous resources that WinUI said may be removed in the future
            currentDictionary.Insert(winrt::box_value(L"TabViewButtonForegroundActiveTab"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewButtonForegroundPressed"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewButtonForegroundPointerOver"), fontBrush);

            // Add a few extra ones for high contrast mode
            // BODGY: contrary to the docs, Insert() seems to throw if the value already exists
            //   Make sure you don't touch any that already exist here!
            if (isHighContrast)
            {
                // TabViewItem.CloseButton.Border: in HC mode, the border makes the button more clearly visible
                currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBorderBrushPressed"), fontBrush);
                currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBorderBrushPointerOver"), fontBrush);
                currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBorderBrushSelected"), fontBrush);
            }
        }

        _RefreshVisualState();
    }

    // Method Description:
    // - Clear out any color we've set for the TabViewItem.
    // - This method should only be called on the UI thread.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_ClearTabBackgroundColor()
    {
        static const winrt::hstring keys[] = {
            // TabViewItem.Background
            L"TabViewItemHeaderBackground",
            L"TabViewItemHeaderBackgroundSelected",
            L"TabViewItemHeaderBackgroundPointerOver",
            L"TabViewItemHeaderBackgroundPressed",

            // TabViewItem.Foreground (aka text)
            L"TabViewItemHeaderForeground",
            L"TabViewItemHeaderForegroundSelected",
            L"TabViewItemHeaderForegroundPointerOver",
            L"TabViewItemHeaderForegroundPressed",

            // TabViewItem.CloseButton.Foreground (aka X)
            L"TabViewItemHeaderCloseButtonForeground",
            L"TabViewItemHeaderForegroundSelected",
            L"TabViewItemHeaderCloseButtonForegroundPointerOver",
            L"TabViewItemHeaderCloseButtonForegroundPressed",

            // TabViewItem.CloseButton.Foreground _when_ interacting with the tab
            L"TabViewItemHeaderPressedCloseButtonForeground",
            L"TabViewItemHeaderPointerOverCloseButtonForeground",
            L"TabViewItemHeaderSelectedCloseButtonForeground",

            // TabViewItem.CloseButton.Background (aka X button)
            L"TabViewItemHeaderCloseButtonBackground",
            L"TabViewItemHeaderCloseButtonBackgroundPressed",
            L"TabViewItemHeaderCloseButtonBackgroundPointerOver",

            // A few miscellaneous resources that WinUI said may be removed in the future
            L"TabViewButtonForegroundActiveTab",
            L"TabViewButtonForegroundPressed",
            L"TabViewButtonForegroundPointerOver",

            // TabViewItem.CloseButton.Border: in HC mode, the border makes the button more clearly visible
            L"TabViewItemHeaderCloseButtonBorderBrushPressed",
            L"TabViewItemHeaderCloseButtonBorderBrushPointerOver",
            L"TabViewItemHeaderCloseButtonBorderBrushSelected"
        };

        const auto& tabItemThemeResources{ TabViewItem().Resources().ThemeDictionaries() };

        // simply clear any of the colors in the tab's dict
        for (const auto& keyString : keys)
        {
            const auto key = winrt::box_value(keyString);
            for (const auto& [_, v] : tabItemThemeResources)
            {
                const auto& themeDictionary = v.as<ResourceDictionary>();
                themeDictionary.Remove(key);
            }
        }

        // GH#11382 DON'T set the background to null. If you do that, then the
        // tab won't be hit testable at all. Transparent, however, is a totally
        // valid hit test target. That makes sense.
        TabViewItem().Background(WUX::Media::SolidColorBrush{ Windows::UI::Colors::Transparent() });

        _RefreshVisualState();
    }

    // Method Description:
    // BODGY
    // - Toggles the requested theme of the tab view item,
    //   so that changes to the tab color are reflected immediately
    // - Prior to MUX 2.8, we only toggled the visual state here, but that seemingly
    //   doesn't work in 2.8.
    // - Just changing the Theme also doesn't seem to work by itself - there
    //   seems to be a way for the tab to set the deselected foreground onto
    //   itself as it becomes selected. If the mouse isn't over the tab, that
    //   can result in mismatched fg/bg's (see GH#15184). So that's right, we
    //   need to do both.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void Tab::_RefreshVisualState()
    {
        const auto& item{ TabViewItem() };

        const auto& reqTheme = TabViewItem().RequestedTheme();
        item.RequestedTheme(ElementTheme::Light);
        item.RequestedTheme(ElementTheme::Dark);
        item.RequestedTheme(reqTheme);

        if (TabViewItem().IsSelected())
        {
            VisualStateManager::GoToState(item, L"Normal", true);
            VisualStateManager::GoToState(item, L"Selected", true);
        }
        else
        {
            VisualStateManager::GoToState(item, L"Selected", true);
            VisualStateManager::GoToState(item, L"Normal", true);
        }
    }

    TabCloseButtonVisibility Tab::CloseButtonVisibility()
    {
        return _closeButtonVisibility;
    }

    // Method Description:
    // - set our internal state to track if we were requested to have a visible
    //   tab close button or not.
    // - This is called every time the active tab changes. That way, the changes
    //   in focused tab can be reflected for the "ActiveOnly" state.
    void Tab::CloseButtonVisibility(TabCloseButtonVisibility visibility)
    {
        _closeButtonVisibility = visibility;
        _updateIsClosable();
    }

    // Method Description:
    // - Update our close button's visibility, to reflect both the ReadOnly
    //   state of the tab content, and also if if we were told to have a visible
    //   close button at all.
    //   - the tab being read-only takes precedence. That will always suppress
    //     the close button.
    //   - Otherwise we'll use the state set in CloseButtonVisibility to control
    //     the tab's visibility.
    void Tab::_updateIsClosable()
    {
        bool isClosable = true;

        if (ReadOnly())
        {
            isClosable = false;
        }
        else
        {
            switch (_closeButtonVisibility)
            {
            case TabCloseButtonVisibility::Never:
                isClosable = false;
                break;
            case TabCloseButtonVisibility::Hover:
                isClosable = true;
                break;
            case TabCloseButtonVisibility::ActiveOnly:
                isClosable = _focused();
                break;
            default:
                isClosable = true;
                break;
            }
        }
        TabViewItem().IsClosable(isClosable);
    }

    bool Tab::_focused() const noexcept
    {
        return _focusState != FocusState::Unfocused;
    }

    void Tab::_chooseColorClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                                  const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        _dispatch.DoAction(*this, { ShortcutAction::OpenTabColorPicker, nullptr });
    }
    void Tab::_renameTabClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                                const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        ActivateTabRenamer();
    }
    void Tab::_duplicateTabClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                                   const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        ActionAndArgs actionAndArgs{ ShortcutAction::DuplicateTab, nullptr };
        _dispatch.DoAction(*this, actionAndArgs);
    }
    void Tab::_splitTabClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                               const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        ActionAndArgs actionAndArgs{ ShortcutAction::SplitPane, SplitPaneArgs{ SplitType::Duplicate } };
        _dispatch.DoAction(*this, actionAndArgs);
    }
    void Tab::_closePaneClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                                const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        ClosePane();
    }
    void Tab::_exportTextClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                                 const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        ActionAndArgs actionAndArgs{};
        actionAndArgs.Action(ShortcutAction::ExportBuffer);
        _dispatch.DoAction(*this, actionAndArgs);
    }
    void Tab::_findClicked(const winrt::Windows::Foundation::IInspectable& /* sender */,
                           const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        ActionAndArgs actionAndArgs{ ShortcutAction::Find, nullptr };
        _dispatch.DoAction(*this, actionAndArgs);
    }
    void Tab::_bubbleRestartTerminalRequested(TerminalApp::TerminalPaneContent sender,
                                              const winrt::Windows::Foundation::IInspectable& args)
    {
        RestartTerminalRequested.raise(sender, args);
    }
}
