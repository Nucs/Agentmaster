// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the RICH SESSION TOOLTIP's lifecycle, shared by every element that shows one.
//
// WHAT: the hover card a managed session shows (a dark, summary-style card: state dot + title, folder/branch,
// state line, kind/model/effort line, tags, then the transcript Summary body — TerminalPage builds it in
// TtBuildTooltipCard) needs a HOST: one framework-managed ToolTip attached to an owner element, its content
// swapped only while it is closed, re-attached after the owner leaves and re-enters the tree, a hover hook
// that rebuilds the card right before ToolTipService opens it, and a wheel hook that scrolls its body. That
// lifecycle used to live inside Tab (the tab header was its only owner). It is extracted here VERBATIM so a
// second owner — a SIDE-PANEL session card (TerminalPage.AgentSidePanel.cpp, the "Sum" off mode) — runs the
// exact same code, crash lessons included. Tab keeps thin forwarders (SetAgentToolTip, ClearAgentToolTip, …)
// so its callers did not change.
//
// THE RULES THIS CLASS EXISTS TO KEEP (doc/agentmaster/HANDOVER_tab-tooltip.md §9-§10 has the crash history):
//   - FRAMEWORK-MANAGED: ToolTipService opens the tip on hover and closes it on pointer exit. This class
//     NEVER writes IsOpen (writing it was the source of crashes #1/#5/#6); it only reads it.
//   - ONE reused ToolTip per owner; Content is swapped only while CLOSED (swapping under the pointer flickers),
//     with one opt-in exception per push (swapWhileOpen — the async summary body landing in the open tip).
//   - Never create or mutate the tooltip for an owner that is not loaded (crash #4: a recycled owner's peer).
//   - Every DETACH needs a re-attach that runs OUTSIDE a dwell: ToolTipService starts watching the owner's
//     hover only when the tooltip is attached (RegisterToolTip), so the Loaded re-attach is load-bearing.
//   - The card itself takes no input (IsHitTestVisible(false), no ScrollViewer inside — crash #7); the wheel
//     is read on the OWNER and applied by the page as a render transform.
//
// OWNERSHIP: the owner element (a TabViewItem, a card Border) is held WEAKLY — its real owner (the Tab, the
// side-panel card view) keeps it alive. The host itself lives in a std::shared_ptr; every handler it wires on
// the owner captures a weak_ptr to it, so a host that dies before its owner leaves handlers that do nothing.
// No destructor here touches XAML (a page teardown can run on a pool thread — the ~TerminalPage cascade rule);
// owners call Shutdown() / Detach() on the UI thread instead.
//
// WinRT XAML types, so this lives in TerminalApp, not the plain-C++ AgentMaster/ engine directory. Include
// from TUs that include pch.h first (the projections come from the pch, like AgentTipHelpers.h).

#pragma once

#include <functional> // the hover / wheel / placement callbacks
#include <memory> // std::shared_ptr / enable_shared_from_this — stable identity for weak-capturing handlers
#include <optional> // a placement provider may decline (the owner is not laid out yet)
#include <string> // the log label + context

namespace winrt::TerminalApp::implementation
{
    // Where a host's tooltip should open, in the OWNER's coordinate space. ToolTip.PlacementRect overrides
    // the pointer-relative placement an automatic (hover-opened) tooltip would otherwise use, so `mode` docks
    // the card OUTSIDE `rect` (WinUI ToolTip::PerformNonMousePlacementWithPopup: the rect is mapped to window
    // coordinates at open time via the owner's TransformToVisual, inflated by the default 20 px mouse offset,
    // then positioned + slid to fit the window). A rect larger than the owner is legal — the side-panel card
    // passes a rect that spans the whole panel so the card opens just past the panel's edge.
    struct AgentSessionToolTipPlacement
    {
        winrt::Windows::UI::Xaml::Controls::Primitives::PlacementMode mode{ winrt::Windows::UI::Xaml::Controls::Primitives::PlacementMode::Bottom }; // which side of `rect` the card opens on
        winrt::Windows::Foundation::Rect rect{}; // the dock rectangle, in the owner's own coordinate space
    };

    // The lifecycle of ONE owner's rich session tooltip. Create it once per owner element (Create), point the
    // page's content builder at SetContent, and arm the lazy hover build with EnsureHoverHook. UI thread only:
    // every member reads or writes XAML objects of the owner's thread.
    class AgentSessionToolTipHost : public std::enable_shared_from_this<AgentSessionToolTipHost>
    {
    public:
        // Computes the placement for `owner` right before the tooltip is shown or re-anchored. Returning
        // nullopt keeps the current placement (e.g. the owner has no size yet). Called only while the tooltip
        // is CLOSED: moving a popup the user is reading would make it jump.
        using PlacementProvider = std::function<std::optional<AgentSessionToolTipPlacement>(const winrt::Windows::UI::Xaml::FrameworkElement& owner)>;

        // Create the host for `owner`. Nothing is wired and nothing is attached yet: handlers are added by
        // EnsureHoverHook / the first Update, so a host that is never used costs one small heap object.
        //   owner        — the element the tooltip belongs to (held WEAKLY; its real owner keeps it alive).
        //   wheelLogLabel — the source word of the throttled "[tooltip-wheel] <label>: cb=… tip=… open=…"
        //                  hooks.log line ("item" for a tab, which keeps the historical line byte-identical).
        //   logContext   — the prefix of every swallowed-exception context ("Tab" keeps the historical
        //                  "[exc] Tab agent tooltip …" names).
        //   returns      — the new host (never null).
        //   throws       — std::bad_alloc on allocation failure; nothing is wired then.
        static std::shared_ptr<AgentSessionToolTipHost> Create(const winrt::Windows::UI::Xaml::FrameworkElement& owner,
                                                               std::wstring wheelLogLabel,
                                                               std::wstring logContext);

        // Replace how the tooltip is placed. Without a provider the host keeps the TAB placement it was
        // written for: Placement(Bottom) + PlacementRect = the owner's own bounds, i.e. the card centered
        // under the owner. Takes effect at the next closed placement (Update / RefreshPlacement).
        //   provider — the side-specific placement; an empty function restores the tab default.
        void SetPlacementProvider(PlacementProvider provider);

        // Host a freshly built card (the page's TtBuildTooltipCard output). An identical push — same signature,
        // already active, and a tooltip still ATTACHED — is a no-op, so the hover build can re-assert it for
        // free; anything else stores the card and runs Update. ⚠ The element must be a FRESH tree: a XAML element
        // has one parent, so the same card set as Content on two ToolTips would corrupt — the cache holds strings.
        //   content       — the card element; null is ignored (nothing to show).
        //   signature     — the page's content fingerprint (what makes an identical push free).
        //   swapWhileOpen — the ONE-SHOT grant to swap Content while the tip is OPEN (the async summary body
        //                   landing in the card the hovering user is waiting for); consumed by the next Update.
        //   throws        — whatever a XAML call on the UI thread throws (the page's hover build contains it).
        void SetContent(winrt::Windows::UI::Xaml::UIElement content, winrt::hstring signature, bool swapWhileOpen);

        // Forget the card and detach the tooltip (the framework closes an open one itself — no IsOpen write).
        // The OWNER decides what shows next: a Tab re-attaches its default title + key-chord tooltip; a side-panel
        // card turns its own floating tip back on.
        //   returns — true when a card was active (so the caller restores its default), false when it was not.
        //   throws  — nothing: Detach contains and logs its own XAML failure.
        bool Clear();

        // Create (once) + attach the reused ToolTip and host the stored card on it — the old Tab::_UpdateAgentToolTip.
        // Skipped for an owner that is not loaded (crash #4); while the tip is OPEN only the one-shot swap grant may
        // touch Content. Placement is re-applied first, while CLOSED. Also the owner-Loaded re-attach path.
        //   throws — whatever a XAML call on the UI thread throws; every caller is a contained UI-thread path.
        void Update();

        // Re-anchor a CLOSED tooltip to the owner's current geometry (the placement provider, else the tab
        // default). Called from the hover hook before the page's build, because an identical card is never
        // re-hosted, yet the owner may have moved or resized since (a tab strip reflow, a side-panel width drag,
        // a dock flip). A no-op while open, while unattached, or when the provider declines.
        //   throws — nothing: a XAML failure is contained and logged (Rule #18) — a lost re-anchor only costs aim.
        void RefreshPlacement() noexcept;

        // Detach the tooltip from the owner and drop the ref (the next Update creates a FRESH one bound to the
        // live owner — the crash #4 lesson). Never writes IsOpen: detaching is enough, the framework closes an
        // open popup itself. Also the side-panel card's "tag ribbon hovered" suspend: the card must not float
        // beside the panel while the tag hover panel is up.
        //   throws — nothing: a failed SetToolTip is logged (Rule #18) and the ref is dropped anyway.
        void Detach() noexcept;

        // Wire (ONCE per host) the owner's PointerEntered -> `onHoverBuild` and PointerWheelChanged -> `onWheel`,
        // plus the owner's Unloaded/Loaded detach/re-attach pair. PointerEntered fires well BEFORE ToolTipService's
        // open delay, so the page's rebuild + Content swap happen while the tip is still closed. The wheel is read
        // here because the card is hit-test-invisible: the owner is the only real pointer target, and it is under
        // the cursor the whole time the card is up. Both callbacks run inside a catch that logs (a tooltip must
        // never take its owner down).
        //   onHoverBuild — the page's build-now callback (resolves the owner's CURRENT session at call time).
        //   onWheel      — the page's body-scroll callback: receives the wheel delta, returns true when it
        //                  scrolled (only then is the notch marked Handled — a card that fits leaves the wheel
        //                  to the owner's own scroller); may be empty.
        //   returns      — true only the ONE time the hooks are wired (the caller's arm-build cue); false when
        //                  already wired or the owner is gone.
        //   throws       — std::bad_alloc / a XAML failure while wiring (the caller's arm path is contained).
        bool EnsureHoverHook(std::function<void()> onHoverBuild, std::function<bool(int)> onWheel);

        // Teardown on the UI thread: Detach + drop both callbacks, so a late PointerEntered or wheel notch on a
        // dying owner invokes nothing (the callbacks weak-capture the page, but a closed tab or a dropped card
        // must not keep the last lambda). The host stays usable (EnsureHoverHook will not re-wire: its hooks
        // still exist on the owner and simply have no callback now).
        //   throws — nothing (Detach contains its own failure).
        void Shutdown() noexcept;

        // True while a rich card is pushed (SetContent ran and Clear did not). The owner's default tooltip
        // path reads it to decide which tooltip to show.
        bool Active() const noexcept { return _active; }
        // The hosted card's fingerprint ("" when none) — lets a per-tick producer skip building a card that
        // would only be signature-discarded (the observe badge's pre-check).
        winrt::hstring Sig() const noexcept { return _sig; }
        // True while a ToolTip object is ATTACHED to the owner. A tooltip that is not attached cannot open:
        // ToolTipService watches the owner's hover only once it is attached — so the page re-pushes when false.
        bool Attached() const noexcept { return _toolTip != nullptr; }
        // True while the hover hook is wired (EnsureHoverHook ran) — the cheap "already armed" probe.
        bool HoverWired() const noexcept { return _hoverWired; }
        // Is the rich card on screen right now? A safe READ of IsOpen (writing it is forbidden). The page needs
        // it to keep its wheel-scroll state in step with the card actually shown, and not to reset a reader's
        // scroll when PointerEntered re-fires mid-hover.
        //   returns — false on any failure (logged), which is the "not open" answer every caller can live with.
        bool Open() const noexcept;

    private:
        // Use Create: the host must live in a std::shared_ptr before any handler can weak-capture it.
        AgentSessionToolTipHost(const winrt::Windows::UI::Xaml::FrameworkElement& owner, std::wstring wheelLogLabel, std::wstring logContext);

        // Wire (ONCE) the owner's Unloaded -> Detach and Loaded -> re-attach pair — the old
        // Tab::_WireAgentToolTipUnload. Called by EnsureHoverHook and by the first Update; idempotent.
        void _WireOwnerLifetime();

        // Apply the placement for `owner` to the CLOSED tooltip: the provider's mode + rect, else the tab
        // default (PlacementRect = the owner's bounds under the Bottom placement set at creation).
        //   owner — the live, loaded owner.
        //   throws — whatever a XAML property write throws (callers contain it).
        void _ApplyPlacement(const winrt::Windows::UI::Xaml::FrameworkElement& owner);

        winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> _owner; // the tooltip's owner element (weak — see the file header)
        std::wstring _wheelLogLabel; // the "[tooltip-wheel] <label>:" source word
        PlacementProvider _placementProvider; // empty == the tab default placement
        // The swallowed-exception context names, built ONCE from the log context at construction, so no catch
        // handler has to allocate a string (several of them sit in noexcept members, where a throw while
        // logging would be std::terminate). "Tab" + the suffix reproduces every historical Tab-era name.
        std::wstring _ctxHoverBuild; // "<context> agent tooltip hover build"
        std::wstring _ctxWheel; // "<context> agent tooltip wheel scroll"
        std::wstring _ctxReattach; // "<context> agent tooltip re-attach"
        std::wstring _ctxPlacement; // "<context> agent tooltip placement"
        std::wstring _ctxDetach; // "<context> agent tooltip detach"
        std::wstring _ctxOpen; // "<context>::AgentToolTipOpen"

        bool _active{ false }; // a rich card is pushed (SetContent ran, Clear did not)
        winrt::Windows::UI::Xaml::UIElement _content{ nullptr }; // the card last handed to us
        winrt::hstring _sig{}; // its fingerprint (identical pushes are free)
        // The ONE reused ToolTip (Content swapped only while CLOSED). Configured once: pinned Dark (a ToolTip
        // renders in the popup root and does NOT inherit the host theme; ToolTipService theme propagation is
        // unreliable under XAML Islands), Placement Bottom (re-pointed by a provider), hit-test-invisible (a
        // big card under the cursor would churn the framework's own hover tracking). Detached + nulled on an
        // owner unload / Clear / Shutdown / Detach.
        winrt::Windows::UI::Xaml::Controls::ToolTip _toolTip{ nullptr };
        bool _lifetimeWired{ false }; // the Unloaded/Loaded pair is wired (once per host)
        std::function<void()> _hoverCb{ nullptr }; // the page's build-now callback (dropped by Shutdown)
        std::function<bool(int)> _wheelCb{ nullptr }; // the page's body-scroll callback (dropped by Shutdown)
        uint64_t _wheelLogTick{ 0 }; // throttle for the one-line-per-gesture "[tooltip-wheel] <label>:" trace
        bool _hoverWired{ false }; // the PointerEntered + wheel hooks are wired (once per host)
        bool _swapOpenOnce{ false }; // one-shot: let the NEXT Update swap Content while OPEN (the async summary body)
    };
}
