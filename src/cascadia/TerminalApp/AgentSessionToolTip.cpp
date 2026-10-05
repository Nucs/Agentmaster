// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the rich session tooltip's lifecycle (see AgentSessionToolTip.h for the rules and the
// ownership model). Every method body below was MOVED from Tab.cpp (the tab header was the only owner until
// the side panel's cards became the second one) with three changes, each called out where it lands:
//   1. the owner is any FrameworkElement held weakly (was `TabViewItem()`);
//   2. the placement is pluggable (was hard-wired Bottom + the owner's bounds — still the default);
//   3. the detach's bare `catch (...) {}` now logs (Rule #18 — "never lose a swallowed exception").
// Plus one addition: RefreshPlacement, run by the hover hook, so a card that is not re-hosted (identical
// signature) is still anchored to the owner's CURRENT geometry.

#include "pch.h"
#include "AgentSessionToolTip.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — the hooks swallow, so they must still report (Rule #18)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog — the "[tooltip-wheel] <label>:" trace

#include <utility> // std::exchange — the one-shot swap grant

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;

namespace winrt
{
    namespace WUX = Windows::UI::Xaml;
}

namespace winrt::TerminalApp::implementation
{
    std::shared_ptr<AgentSessionToolTipHost> AgentSessionToolTipHost::Create(const FrameworkElement& owner,
                                                                             std::wstring wheelLogLabel,
                                                                             std::wstring logContext)
    {
        // `new` instead of std::make_shared: the constructor is private (a host must exist inside a
        // shared_ptr before weak_from_this can work), and make_shared cannot reach a private constructor.
        return std::shared_ptr<AgentSessionToolTipHost>(new AgentSessionToolTipHost(owner, std::move(wheelLogLabel), std::move(logContext)));
    }

    AgentSessionToolTipHost::AgentSessionToolTipHost(const FrameworkElement& owner, std::wstring wheelLogLabel, std::wstring logContext) :
        _owner{ owner ? winrt::make_weak(owner) : winrt::weak_ref<FrameworkElement>{} },
        _wheelLogLabel{ std::move(wheelLogLabel) },
        _ctxHoverBuild{ logContext + L" agent tooltip hover build" },
        _ctxWheel{ logContext + L" agent tooltip wheel scroll" },
        _ctxReattach{ logContext + L" agent tooltip re-attach" },
        _ctxPlacement{ logContext + L" agent tooltip placement" },
        _ctxDetach{ logContext + L" agent tooltip detach" },
        _ctxOpen{ logContext + L"::AgentToolTipOpen" }
    {
    }

    void AgentSessionToolTipHost::SetPlacementProvider(PlacementProvider provider)
    {
        _placementProvider = std::move(provider);
    }

    // Was Tab::SetAgentToolTip: accept a rich, session-aware tooltip ELEMENT from TerminalPage (the page owns
    // the SessionInfo + the registry, so it builds the whole summary-style card; the host only hosts it).
    // `signature` is a cheap content fingerprint the page computes — an identical push is a no-op (so the
    // hover-time rebuild, the bind tail, and the observe-badge probe can all re-assert it without re-hosting
    // XAML; content is LAZY/hover-built — the old per-tick sweep re-assert is gone).
    void AgentSessionToolTipHost::SetContent(UIElement content, hstring signature, bool swapWhileOpen)
    {
        if (!content)
        {
            return;
        }
        if (_active && signature == _sig && _toolTip)
        {
            return; // identical content already shown — don't re-host the XAML
        }
        // `&& _toolTip` is load-bearing, not defensive: an owner recycle DETACHES + nulls the tooltip (the
        // crash #4 fix), and if the content signature happens to be unchanged since then, this early-out used
        // to skip the re-host and leave the owner with NO tooltip attached at all until some unrelated field
        // changed. Attachment is the thing that matters — see the Loaded re-attach in _WireOwnerLifetime.

        _active = true;
        _content = std::move(content);
        _sig = std::move(signature);
        _swapOpenOnce = swapWhileOpen; // one-shot, consumed (and always cleared) by Update
        Update();
    }

    // Was Tab::ClearAgentToolTip (minus the default-tooltip revert, which is the owner's business): forget the
    // card and detach. No-op if none was set.
    bool AgentSessionToolTipHost::Clear()
    {
        if (!_active)
        {
            return false;
        }
        _active = false;
        _content = nullptr;
        _sig = {};
        Detach(); // drop the framework-managed tooltip + detach it from the owner (the framework closes any open popup on its own)
        return true;
    }

    // Was Tab::_UpdateAgentToolTip: host the rich tooltip element TerminalPage built (a dark, summary-style
    // card — see TerminalPage::_PushSessionToolTipCard). The page owns the layout + content; the host owns the
    // ToolTip lifecycle. FRAMEWORK-MANAGED: ToolTipService owns open/close (hover opens after the system delay,
    // pointer-exit closes) — the SAME model as the plain default tab tooltip (Tab::_UpdateToolTip), which has
    // NEVER crashed. We never drive IsOpen; the six historical tooltip crashes were all on the old manual-open
    // path (timers + IsOpen + a cross-tab cooldown), now removed — see doc/agentmaster/HANDOVER_tab-tooltip.md.
    //
    // Reuse ONE ToolTip object across refreshes and swap its Content ONLY while closed: re-creating it +
    // re-SetToolTip on each refresh would replace — and so flicker/close — the framework's open tip while you
    // hover it. Configure it once: pinned Dark (a ToolTip renders in the popup root and does NOT inherit the
    // host theme, and ToolTipService theme propagation is unreliable under XAML Islands — see AgentTipHelpers),
    // placed by the placement provider (the tab default: BELOW the tab — the default placement would push it up
    // into the titlebar / off the top of the screen), and hit-test-invisible (a big card sitting under the
    // cursor as a pointer target would churn the framework's own hover tracking).
    void AgentSessionToolTipHost::Update()
    {
        // Never create OR mutate the tooltip for a detached owner. If the owner has left the visual tree (a MUX
        // TabView recycle; a side-panel card dropped from the list), building + SetToolTip + `.Content()` here is
        // at best wasted and at worst touches a torn-down peer — skip until the owner is live again (the Loaded
        // re-attach / the next hover rebuilds it). A normal loaded owner is always IsLoaded()+rooted, so
        // steady-state is unaffected. Complements the Unloaded detach (_WireOwnerLifetime -> Detach): a recycle
        // drops the ref so a later RELOAD of the same owner rebuilds a FRESH tooltip here instead of reusing a
        // stale one (the crash #4 lesson). Consume the one-shot FIRST (even the early-outs below must clear it — a
        // swap-while-open grant that survived a skipped refresh would leak onto a later, unrelated content push).
        const bool swapOpenOnce = std::exchange(_swapOpenOnce, false);
        const auto owner = _owner.get();
        if (!owner || !owner.IsLoaded() || !owner.XamlRoot())
        {
            return;
        }
        if (!_toolTip)
        {
            _toolTip = WUX::Controls::ToolTip{};
            _toolTip.RequestedTheme(WUX::ElementTheme::Dark);
            _toolTip.Placement(WUX::Controls::Primitives::PlacementMode::Bottom);
            // Make the tooltip popup click/hover-THROUGH: a hit-testable popup becomes a pointer target, so the
            // large card sitting under the cursor would churn the framework's own hover tracking. A tooltip is
            // purely informational and never needs input (the AgentTipHelpers recipe does the same).
            _toolTip.IsHitTestVisible(false);
            WUX::Controls::ToolTipService::SetToolTip(owner, _toolTip);
            _WireOwnerLifetime(); // wire (once) the owner Unloaded -> detach, so a recycle can't strand a stale ref
        }

        // Don't swap Content while the framework has the tip OPEN (you're reading it): swapping under the pointer
        // flickers. Reading IsOpen is safe — only DRIVING it ever raced. ONE exception, opt-in per push
        // (_swapOpenOnce, consumed at entry above): the async summary-body arrival — the hovering user is WAITING
        // for that content, so a single in-place swap is the desired behavior.
        if (_toolTip.IsOpen() && !swapOpenOnce)
        {
            return;
        }
        // Anchor the popup to the OWNER, not the cursor (a CLOSED-only mutation, unlike Content below): a
        // hover-opened AUTOMATIC tooltip places itself relative to the POINTER — Placement(Bottom) alone reads
        // "below the cursor", so the card landed wherever inside the tab the mouse happened to sit. An explicit
        // PlacementRect (in the placement target's coordinate space) overrides pointer placement. Re-asserted on
        // each closed refresh — owner geometry drifts (tab add/remove/rename, window resize, a side-panel width
        // drag or dock flip); while OPEN we leave placement alone (moving a popup the user is reading would make
        // it jump).
        if (!_toolTip.IsOpen())
        {
            _ApplyPlacement(owner);
        }
        _toolTip.Content(_content); // host the page-built card on the reused object
    }

    // The closed-only placement write, factored out of Update so RefreshPlacement can run it too. Without a
    // provider it is exactly the old Tab code: PlacementRect = the owner's own bounds (the Bottom placement was
    // set once at creation). With one, the provider's mode + rect (a side-panel card: Right/Left, spanning the
    // panel). A provider that declines (nullopt) leaves the current placement in place.
    void AgentSessionToolTipHost::_ApplyPlacement(const FrameworkElement& owner)
    {
        if (_placementProvider)
        {
            if (const auto placement = _placementProvider(owner))
            {
                if (_toolTip.Placement() != placement->mode)
                {
                    _toolTip.Placement(placement->mode); // a dock flip moves the card to the panel's other side
                }
                if (placement->rect.Width > 0 && placement->rect.Height > 0)
                {
                    _toolTip.PlacementRect(winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Rect>{ placement->rect });
                }
            }
            return;
        }
        if (const auto w = static_cast<float>(owner.ActualWidth()), h = static_cast<float>(owner.ActualHeight()); w > 0 && h > 0)
        {
            _toolTip.PlacementRect(winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Rect>{ winrt::Windows::Foundation::Rect{ 0, 0, w, h } });
        }
    }

    void AgentSessionToolTipHost::RefreshPlacement() noexcept
    {
        try
        {
            if (!_toolTip)
            {
                return; // nothing attached — the next Update places it on creation
            }
            const auto owner = _owner.get();
            if (!owner || !owner.IsLoaded() || !owner.XamlRoot())
            {
                return; // same rule as Update: never touch the tooltip of an owner that is not live
            }
            if (_toolTip.IsOpen())
            {
                return; // PointerEntered re-fires mid-hover (it bubbles from the owner's children) — never move an open card
            }
            _ApplyPlacement(owner);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(_ctxPlacement.c_str());
        }
    }

    // Was Tab::_WireAgentToolTipUnload: FRAMEWORK-MANAGED lifecycle. ToolTipService owns open (hover, after the
    // system delay) and close (pointer-exit) — the SAME model as the plain default tab tooltip, which has NEVER
    // crashed. The only handlers we wire for the lifetime are the owner's Unloaded + Loaded, so a MUX TabView
    // recycle/detach (or a card dropped from the side-panel list) can't leave us holding a stale ToolTip ref: on
    // unload we detach + drop it, and the next refresh rebuilds a FRESH one bound to the live (reloaded) owner
    // (the crash #4 lesson — with NO manual IsOpen, so it cannot fail-fast). Wired ONCE per host.
    //
    // The historical manual-open path (a fast-open one-shot timer, an 8s auto-dismiss backstop, three pointer-loss
    // close handlers, PointerMoved keep-alive/re-open, a cross-tab reopen cooldown, and _SafeSetAgentToolTipOpen
    // driving IsOpen) is GONE — it was the source of ALL SIX tooltip crashes (HANDOVER_tab-tooltip.md §9). The
    // tradeoff: the tip opens at the system hover delay (this SDK exposes no ToolTipService.InitialShowDelay).
    void AgentSessionToolTipHost::_WireOwnerLifetime()
    {
        if (_lifetimeWired)
        {
            return;
        }
        const auto owner = _owner.get();
        if (!owner)
        {
            return;
        }
        _lifetimeWired = true;
        const std::weak_ptr<AgentSessionToolTipHost> weakSelf = weak_from_this();

        // The owner leaving the visual tree (MUX TabView recycles its containers — heavy during a multi-tab window
        // restore) tears down the ToolTip's native peer while our WinRT strong ref keeps resolving non-null — a
        // zombie. If the SAME owner later RELOADS, reusing that stale ref to swap .Content() would read freed memory
        // (crash #4). So on unload, detach the tooltip from the owner + drop our ref; Update then rebuilds a fresh
        // one on the next refresh. We drive NO IsOpen here (the framework closes any open popup on exit/recycle
        // itself), so this can't fail-fast.
        owner.Unloaded([weakSelf](auto&&, auto&&) {
            if (const auto self = weakSelf.lock())
            {
                self->Detach();
            }
        });

        // ...and the other half of that trade, which was MISSING and was the root cause of "the tab tooltip only
        // shows sometimes, and its wheel scrolling never works":
        //
        // ToolTipService subscribes to the OWNER's PointerEntered inside RegisterToolTip — i.e. at the moment the
        // tooltip is ATTACHED (dxaml ToolTipService_Partial.cpp: RegisterToolTip -> add_PointerEntered). A tooltip
        // attached DURING a dwell therefore misses that dwell's PointerEntered, so its open timer does not start
        // for that hover. The Unloaded handler above nulls our tooltip on every recycle, and re-attachment used to
        // happen only on the NEXT content push — which, since the card went lazy/hover-built, IS mid-dwell.
        //
        // So re-attach as soon as the owner is back in the tree, BEFORE any pointer can enter it. It is also the
        // FIRST attach of a side-panel card armed before it was inserted in the list (Update skips an owner that
        // is not loaded yet, then this handler hosts it).
        owner.Loaded([weakSelf](auto&&, auto&&) {
            if (const auto self = weakSelf.lock())
            {
                if (self->_active && !self->_toolTip && self->_content)
                {
                    try
                    {
                        self->Update(); // re-create + SetToolTip with the content we already have
                    }
                    catch (...)
                    {
                        // a tooltip must never take its owner down — swallow (the next hover rebuilds it)
                        ::Agentmaster::AgentLogCaughtException(self->_ctxReattach.c_str());
                    }
                }
            }
        });
    }

    // Was Tab::EnsureAgentToolTipHoverHook (LAZY tooltip — the CPU fix): wire (once per host) the owner's
    // PointerEntered to the page's build-now callback. PointerEntered fires the moment the pointer enters the
    // owner — well BEFORE ToolTipService's open delay — so the page's rebuild + Content swap still happen while
    // the tip is CLOSED (the safe mutation window Update enforces). This replaced the old model where the page
    // rebuilt EVERY managed tab's card on every ~2.5s sweep tick (70-tab strip: a constant stream of XAML card
    // builds nobody ever saw — the measured UI-thread hotspot); a card is built only when a human is actually
    // about to see it. The handler drives NO IsOpen and hosts nothing itself — it re-anchors the placement and
    // invokes the page callback, which funnels through SetContent/Update. Returns true only the ONE time the hook
    // is wired, so the caller can arm-build once (the tooltip must be hosted BEFORE the first hover for
    // ToolTipService to open it on that hover). The handlers live on the owner (stable across MUX recycles) and
    // weak-capture the host, so teardown order is safe; the callbacks weak-capture the page (dropped by Shutdown).
    bool AgentSessionToolTipHost::EnsureHoverHook(std::function<void()> onHoverBuild, std::function<bool(int)> onWheel)
    {
        if (_hoverWired)
        {
            return false; // already armed — keep the existing (identical-shape) callbacks
        }
        const auto owner = _owner.get();
        if (!owner)
        {
            return false; // no owner (yet / any more) — a later arm attempt wires it
        }
        _hoverCb = std::move(onHoverBuild);
        _wheelCb = std::move(onWheel);
        _hoverWired = true;
        // The Unloaded/Loaded pair is wired HERE too (it used to wait for the first Update): an owner armed while
        // detached — a side-panel card built before its list insert — needs the Loaded re-attach to get its FIRST
        // attach before any pointer arrives. Idempotent: the first of the two callers wins.
        _WireOwnerLifetime();
        const std::weak_ptr<AgentSessionToolTipHost> weakSelf = weak_from_this();
        owner.PointerEntered([weakSelf](auto&&, auto&&) {
            if (const auto self = weakSelf.lock())
            {
                // Re-anchor FIRST (a no-op while open): an identical card is never re-hosted, so without this a
                // card hovered after its owner moved (a strip reflow, a panel resize or dock flip) would open at
                // the OLD place. Contained inside RefreshPlacement.
                self->RefreshPlacement();
                if (self->_hoverCb)
                {
                    try
                    {
                        self->_hoverCb();
                    }
                    catch (...)
                    {
                        // a tooltip must never take its owner down — swallow (the card simply stays stale)
                        ::Agentmaster::AgentLogCaughtException(self->_ctxHoverBuild.c_str());
                    }
                }
            }
        });

        // WHEEL SCROLL: the rich card is TALLER than it can show for a long conversation, so the wheel scrolls its
        // body. The wheel has to be read HERE, on the owner, because the card itself is IsHitTestVisible(false) and
        // must stay that way — it is a popup, and making popup content input-capable (a ScrollViewer,
        // DirectManipulation) is the proven 0xC000027B fail-fast class this whole feature was rebuilt around
        // (HANDOVER_tab-tooltip.md crash #7 / invariant 6a). The owner is under the cursor for the tip's entire
        // life anyway, so "hover, spin the wheel" needs no input on the card at all.
        //
        // We only ever READ IsOpen (never drive it — invariant 2), and we mark the notch Handled ONLY when the page
        // reports it actually scrolled something: otherwise the owner's own scroller (the tab strip, the side
        // panel's card list) keeps its wheel behavior, which is what a card that fits on screen should never steal.
        //
        // AddHandler(..., handledEventsToo: true) rather than the `.PointerWheelChanged(...)` convenience (which is
        // handledEventsToo: FALSE): the tab strip is a nest of MUX controls, and anything inside the header marking
        // the notch Handled on its way up would otherwise make this hook silently dead. (The page keeps a second,
        // whole-tab-row belt for the tab case — TerminalPage::_WireTabStripTooltipWheel.)
        owner.AddHandler(
            UIElement::PointerWheelChangedEvent(),
            winrt::box_value(WUX::Input::PointerEventHandler{ [weakSelf](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::PointerRoutedEventArgs& e) {
                const auto self = weakSelf.lock();
                if (!self || !self->_active)
                {
                    return; // no rich card on this owner — its scroller keeps the wheel, and we say nothing about it
                }
                try
                {
                    // ONE throttled line per gesture. This handler is the first link in the chain that cannot be
                    // observed from the outside, so without it "the scroll didn't register" is indistinguishable
                    // between "the notch never arrived", "the card wasn't open" and "the body had nothing to
                    // scroll" — three completely different bugs.
                    const auto nowTick = ::GetTickCount64();
                    const bool open = self->_toolTip && self->_toolTip.IsOpen();
                    if (nowTick - self->_wheelLogTick > 400)
                    {
                        self->_wheelLogTick = nowTick;
                        // The trailing newline is what lets AppendStateLog stamp + frame the NEXT record — without
                        // it every later line of ANY tag was glued onto this one (the hooks.log "[tooltip-wheel]
                        // item: ...[tooltip-wheel] row: ...[notify] ..." run-on lines).
                        ::Agentmaster::AppendStateLog(L"hooks.log",
                                                      std::wstring{ L"[tooltip-wheel] " } + self->_wheelLogLabel + L": cb=" + (self->_wheelCb ? L"1" : L"0") +
                                                          L" tip=" + (self->_toolTip ? L"1" : L"0") +
                                                          L" open=" + (open ? L"1" : L"0") + L"\n");
                    }
                    if (!self->_wheelCb || !open)
                    {
                        return; // no card on screen (or no page callback) — nothing to scroll
                    }
                    // Relative to the owner itself, not nullptr: the delta is placement-independent, and an
                    // element we KNOW is alive can't be the thing that throws on this path.
                    const auto owner = self->_owner.get();
                    const auto pt = owner ? e.GetCurrentPoint(owner) : e.GetCurrentPoint(nullptr);
                    if (const auto delta = pt ? pt.Properties().MouseWheelDelta() : 0; delta != 0 && self->_wheelCb(delta))
                    {
                        e.Handled(true);
                    }
                }
                catch (...)
                {
                    // a tooltip must never take its owner down — swallow (the notch is simply lost)
                    ::Agentmaster::AgentLogCaughtException(self->_ctxWheel.c_str());
                }
            } }),
            true /* handledEventsToo */);
        return true;
    }

    // Was Tab::AgentToolTipOpen: is the rich card on screen right now? Reading IsOpen is safe and always was — it
    // is DRIVING it that caused crashes #1/#5/#6, and invariant 2 forbids exactly that. The page uses this for the
    // WHEEL SCROLL bookkeeping: a card built while the tip is open can NOT be hosted (invariant 3 — Content is
    // swapped only while closed), so the page must neither adopt its scroll pieces (it would drive an off-screen
    // tree) nor reset the reader's scroll position under them.
    bool AgentSessionToolTipHost::Open() const noexcept
    {
        try
        {
            return _active && _toolTip && _toolTip.IsOpen();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(_ctxOpen.c_str()); // "Tab::AgentToolTipOpen" for a tab — the historical name
            return false;
        }
    }

    // Was Tab::_DetachAgentToolTip: detach the framework-managed tooltip from its owner and drop our strong ref.
    // Run on an owner recycle/unload (_WireOwnerLifetime), on Clear (session gone), on Shutdown, and by the side
    // panel's tag-ribbon suspend. Idempotent + safe when no tooltip was ever created (the guard leaves an owner's
    // OTHER tooltip — a tab's default title+keychord tooltip — untouched). We do NOT touch IsOpen — the framework
    // owns open/close, so detaching is enough (it dismisses any open popup itself: UnregisterToolTip ->
    // OnOwnerLeaveInternal -> CancelAutomaticToolTip); NOT driving IsOpen is precisely what removes the
    // fail-fast/UAF class the manual path suffered. Nulling the ref means the next Update rebuilds a FRESH tooltip
    // bound to the live owner. UI thread only.
    void AgentSessionToolTipHost::Detach() noexcept
    {
        if (!_toolTip)
        {
            return; // nothing of ours attached — leave the owner's default tooltip (if any) alone
        }
        try
        {
            if (const auto owner = _owner.get())
            {
                WUX::Controls::ToolTipService::SetToolTip(owner, nullptr);
            }
        }
        catch (...)
        {
            // A torn-down owner can refuse the detach. The ref is dropped below regardless (a zombie must never be
            // reused — crash #4), and the failure is reported instead of vanishing (Rule #18; the Tab-era code
            // swallowed it bare). The context string was built at construction, so this handler allocates nothing.
            ::Agentmaster::AgentLogCaughtException(_ctxDetach.c_str());
        }
        _toolTip = nullptr;
    }

    void AgentSessionToolTipHost::Shutdown() noexcept
    {
        // Detach + drop the lazy hover-build and wheel callbacks — they weak-capture the page, but releasing them
        // here keeps a closed owner from holding the last lambda (and a late PointerEntered / notch on a dying
        // owner invokes nothing).
        Detach();
        _hoverCb = nullptr;
        _wheelCb = nullptr;
    }
}
