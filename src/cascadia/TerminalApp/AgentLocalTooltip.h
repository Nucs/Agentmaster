// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — LocalTooltip: a DESIGNATED-AREA tooltip surface. Instead of the floating
// ToolTip chasing the pointer (AgentTipHelpers.h — which pops over the very control you are
// about to click and crams long explanations into a small popup), a LocalTooltip renders the
// hovered element's tip text in ONE fixed panel the scope owner places wherever fits — for the
// Settings cog: OUTSIDE the card at its top-LEFT (same top edge, the panel's right edge glued
// to the card's left edge, growing only leftward). Any page / component / overlay can host its
// own instance at its own designated area; the per-element tips need NO rewrite — the panel
// reads the SAME attached tip text AgentSetTip already records.
//
// The pieces, and how they compose with the floating recipe:
//   * SOURCE — the per-element text is agent_tip_details::TipTextProperty (what AgentSetTip
//     writes), so every existing AgentSetTip call site feeds a LocalTooltip unchanged. A TITLE
//     line is auto-derived from the element (its Header / string Content — the settings rows'
//     natural labels), overridable per element via AgentSetTipTitle.
//   * ROUTING — AttachScope(root) installs ONE bubbling PointerMoved on the SCOPE root (one
//     handler over the whole scope, never per element): resolve the nearest ancestor of OriginalSource carrying a
//     tip text (innermost wins, like the floating recipe's nested-tip rule) and render it
//     IMMEDIATELY (a side panel is out of the pointer's path, so it needs no hover-rest delay,
//     can't flicker under the cursor, and can't eat a click). STICKY on purpose: crossing the
//     gaps between controls keeps the last description up (the title says which control it
//     describes), so panning down a settings column doesn't strobe the panel empty/full.
//     That is the DEFAULT (the Settings cog, the Sessions page); a panel that floats OVER content
//     opts into TIMING instead (next bullet).
//   * TIMING (opt-in, SetTooltipTiming — the Agent Manager page, whose panel covers the board's
//     top-right corner, where a sticky description stayed up for good): the panel behaves like a
//     tooltip. A control's description shows once the pointer RESTS on it for Windows' hover time
//     (a move restarts the wait, so a sweeping pointer shows nothing), stays while the pointer moves
//     inside that control, and hides when the pointer is off it (a gap, another control, out of the
//     scope), on any press in the scope, and after the panel's show time. A description hidden by
//     the time limit or a press stays hidden while the pointer stays on its control. AttachScope
//     adds a PointerExited and a PointerPressed on each scope root for this (handledEventsToo — a
//     Button marks its press handled); a sticky panel's copies return at once. PointerExited
//     BUBBLES, so every exit is judged by the pointer's POSITION against the element in question,
//     never by which element raised it (the PointerStillWithin rule of AgentManagerContent).
//   * SUPPRESSION — AttachScope marks the root (LocalTipScopeProperty true) and the floating
//     host's open tick yields inside a marked scope, so the two never double-show. The marker
//     is a live switch: _Reposition flips it false while the window is too NARROW to host the
//     panel (no room left of the anchor) — the classic floating tips take over — and back true
//     when room returns. Graceful degradation, no re-wiring. AttachScope may be called for
//     SEVERAL roots feeding ONE panel (the Manager tab scopes its toolbar/board/bottom regions
//     separately, so the settings / claude-missing overlay cards — SIBLINGS in the same root
//     Grid — stay outside the scope: no nested-scope resolution exists or is needed).
//   * PLACEMENT — three anchors, all recomputed from the anchor's REAL position
//     (TransformToVisual) on host/anchor SizeChanged, all growing ONLY leftward
//     (HorizontalAlignment::Right + a computed right margin):
//       - AnchorTopLeftOutside(host, anchor, gap): right edge `gap` px LEFT of the anchor, tops
//         aligned — the settings card's "outside the dialog" placement;
//       - AnchorTopRightAbove(host, anchor, topPad): right edges ALIGNED, top pinned at the
//         HOST's top — the empty zone ABOVE the anchor (the Sessions page's header-right
//         alignment space over its detail pane);
//       - AnchorTopRightInside(host, anchor, pad): nested `pad` px inside the anchor's
//         top-right corner (the Manager tab's board region).
//     Layout-cycle-safe by construction: the panel is a floating sibling whose size affects
//     neither host nor anchor (a Grid host gets Row/ColumnSpan 99 so no Auto row ever grows to
//     fit it), and every write is change-gated (the _ApplyRenamerMaxWidth lesson).
//   * INPUT — SetClickThrough(true) makes the panel ignore pointer input entirely (the floating
//     ToolTip's own IsHitTestVisible(false) rationale) for placements that FLOAT OVER
//     interactive content; the default swallows taps instead, for the settings placement over a
//     modal dim whose Tapped means "dismiss".
//
// ⚠ LEAK DISCIPLINE (the 68 GB AgentTipHelpers lesson): everything here is per-SCOPE, never
// per-element — three pointer handlers on the scope root (PointerMoved, PointerExited,
// PointerPressed), one panel, at most two DispatcherTimers per PANEL (the timing's rest wait and
// time limit), zero per-element handlers/allocations beyond the attached-property values
// AgentSetTip already wrote. The handlers capture a shared_ptr<State> (NOT `this`, so the class
// handle may be copied/reset freely; NOT the scope root, which would be an element→handler→element
// ref cycle); the timers' Tick handlers capture the State only WEAKLY (State owns the timers, so a
// strong capture would be a cycle keeping the panel alive after its page); and every element
// reference the State keeps (the hover throttle / shown control, the control a rest wait is armed
// on, the dismissed control) is WEAK, so a rebuilt-away element is never pinned.
// ToolTipService is never touched. Intended for scopes built ONCE (an overlay card, a page
// chrome) — attach to a per-refresh-rebuilt surface and you re-register the handlers per rebuild.
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine
// directory. Header-only; relies on the pch projections (like AgentTipHelpers.h, which it
// builds on). Safe to include from AgentManagerContent.h — itself a .cpp-TU-only header.

#pragma once

#include <algorithm> // std::min/max — the anchor math
#include <chrono> // the timing's rest wait and time limit (SetTooltipTiming)
#include <memory> // the shared State the scope/anchor handlers capture (weak_ptr in the timers' Ticks)
#include <string> // the timing's description keys (State::shownKey / dismissedKey)
#include <vector> // the attached scope roots (a panel may serve several regions)

#include "AgentTipHelpers.h" // TipTextOf/TipTitleProperty (the per-element tip store) + LocalTipScopeProperty (the floating-tip suppression seam)

namespace winrt::TerminalApp::implementation
{
    class AgentLocalTooltip
    {
    public:
        AgentLocalTooltip() = default;

        // Build the panel visuals (UI thread; call once from the scope owner's builder). A
        // default-constructed handle is inert — every method no-ops until Initialize ran — so the
        // class can sit as a plain value member of a WinRT implementation type.
        void Initialize()
        {
            if (_s)
            {
                return;
            }
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace WUXC = WUX::Controls;
            _s = std::make_shared<State>();

            // The panel: a mini-card visually siblinged to the settings card (same border/corner
            // family, a shade darker so it reads as an annex, not a second dialog). Collapsed until
            // the first hover; MaxWidth/MaxHeight are refined live by _Reposition.
            _s->root = WUXC::Border{};
            _s->root.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x20, 0x20, 0x20) });
            _s->root.BorderBrush(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0x90, 0x80, 0x80, 0x80) });
            _s->root.BorderThickness(WUX::Thickness{ 1, 1, 1, 1 });
            _s->root.CornerRadius(WUX::CornerRadius{ 8, 8, 8, 8 });
            _s->root.Padding(WUX::Thickness{ 14, 10, 14, 12 });
            _s->root.MinWidth(kMinRoomPx);
            _s->root.MaxWidth(kMaxWidthPx);
            _s->root.Visibility(WUX::Visibility::Collapsed);
            _s->root.RequestedTheme(WUX::ElementTheme::Dark); // Agentmaster surfaces are ALWAYS dark (Conventions)
            // Swallow taps: the panel typically sits over a modal overlay's dim BACKDROP, whose
            // Tapped is "dismiss" (the settings card idiom) — a click on the description panel must
            // read as "nothing", not close the dialog under the user.
            _s->root.Tapped([](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::TappedRoutedEventArgs& e) {
                e.Handled(true);
            });

            auto col = WUXC::StackPanel{};
            col.Spacing(6);

            _s->title = WUXC::TextBlock{};
            _s->title.FontSize(13);
            _s->title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            _s->title.TextWrapping(WUX::TextWrapping::Wrap);
            _s->title.Opacity(0.95);
            _s->title.Visibility(WUX::Visibility::Collapsed);
            col.Children().Append(_s->title);

            // A thin rule under the title (the cog tab-strip divider idiom) — shown only with one.
            _s->divider = WUXC::Border{};
            _s->divider.Height(1);
            _s->divider.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0x30, 0xFF, 0xFF, 0xFF) });
            _s->divider.Visibility(WUX::Visibility::Collapsed);
            col.Children().Append(_s->divider);

            _s->body = WUXC::TextBlock{};
            _s->body.FontSize(12);
            _s->body.TextWrapping(WUX::TextWrapping::Wrap);
            _s->body.Opacity(0.85);
            col.Children().Append(_s->body);

            _s->root.Child(col);
        }

        explicit operator bool() const noexcept { return static_cast<bool>(_s); }

        // The panel element, for a scope owner that places it MANUALLY (its own designated area /
        // alignment) instead of via the Anchor* methods. Null before Initialize.
        winrt::Windows::UI::Xaml::UIElement Visual() const { return _s ? _s->root : nullptr; }

        // Informational-overlay mode: the panel ignores pointer input entirely, so a placement that
        // FLOATS OVER interactive content (the sessions detail pane's top, the manager board) can
        // never block a click — the floating ToolTip's own IsHitTestVisible(false) rationale. The
        // default (off) keeps the Initialize tap-swallow instead: the settings panel sits over a
        // modal DIM whose Tapped means "dismiss", where a click-through would close the dialog
        // under the very click.
        void SetClickThrough(bool clickThrough)
        {
            if (_s)
            {
                _s->root.IsHitTestVisible(!clickThrough);
            }
        }

        // Route every AgentSetTip text under `scopeRoot` into this panel: ONE bubbling PointerMoved
        // resolves the hovered tipped element (innermost wins) and renders title+body — at once on a
        // sticky panel, after a rest on a timed one (SetTooltipTiming); the scope marker makes the
        // floating host yield inside (see the header block). May be called for SEVERAL roots feeding
        // one panel (the Manager tab's toolbar/board/bottom) — the cramped fallback flips every
        // attached root's marker in lockstep. Also hooks the root's PointerExited and PointerPressed,
        // which only a timed panel acts on (a sticky one returns from them at once), so the order of
        // AttachScope and SetTooltipTiming does not matter.
        //   scopeRoot - the root of the region whose tips this panel shows; null is ignored. Kept only
        //               WEAKLY (the cramped fallback's marker flips), and its handlers hold the panel's
        //               State, never the root itself, so attaching creates no reference cycle.
        // UI thread only; a no-op before Initialize. Throws what registering a handler or writing an
        // attached property on a live XAML element throws (the builder's own contract).
        void AttachScope(const winrt::Windows::UI::Xaml::FrameworkElement& scopeRoot)
        {
            if (!_s || !scopeRoot)
            {
                return;
            }
            namespace atd = agent_tip_details;
            namespace WUX = winrt::Windows::UI::Xaml;
            _s->scopeRoots.push_back(winrt::make_weak(scopeRoot));
            // Seed the live suppression marker: active, unless the anchor math has ALREADY proven
            // the window too cramped to host the panel — a root attached mid-cramped must not
            // suppress the floating tips that are currently serving.
            scopeRoot.SetValue(atd::LocalTipScopeProperty(), winrt::box_value(!(_s->crampedSynced && _s->cramped)));
            ++atd::Host().localTipScopes; // arm the floating host's suppression fast-path (this UI thread)
            // The scope-level handlers (capture-safe: the shared State only — never `this`, never
            // the scope root, which its own handler would cycle-pin).
            auto s = _s;
            scopeRoot.PointerMoved([s](const winrt::Windows::Foundation::IInspectable& sender, const WUX::Input::PointerRoutedEventArgs& args) {
                _OnScopePointerMoved(s, sender, args);
            });
            // The timing's two belts beside the moves: an EXIT is how the panel learns that the
            // pointer left the described control for somewhere the scope hears no move from (a
            // splitter outside the scope, a popup's menu, another region, the window's edge); a PRESS
            // is a click on the control, after which a tooltip goes away. Both registered
            // handledEventsToo: a Button marks its own press handled, so the plain .PointerPressed
            // registration would never hear a click on any button of the scope.
            scopeRoot.AddHandler(
                WUX::UIElement::PointerExitedEvent(),
                winrt::box_value(WUX::Input::PointerEventHandler{ [s](const winrt::Windows::Foundation::IInspectable& sender, const WUX::Input::PointerRoutedEventArgs& args) {
                    _OnScopePointerExited(s, sender, args);
                } }),
                true);
            scopeRoot.AddHandler(
                WUX::UIElement::PointerPressedEvent(),
                winrt::box_value(WUX::Input::PointerEventHandler{ [s](const winrt::Windows::Foundation::IInspectable& sender, const WUX::Input::PointerRoutedEventArgs& args) {
                    _OnScopePointerPressed(s, sender, args);
                } }),
                true);
        }

        // Opt this panel into TOOLTIP TIMING — the Agent Manager page's choice (2026-11-13: "Like a
        // tooltip", up for 10 s), for a panel that floats OVER content, where the sticky default left
        // the last description covering the board's corner for good. With it on:
        //   * a control's description shows only once the pointer RESTS on that control for Windows'
        //     hover time (SPI_GETMOUSEHOVERTIME, 400 ms by default): each move restarts the wait, so a
        //     pointer sweeping across the page shows nothing;
        //   * it stays while the pointer moves INSIDE that control;
        //   * it hides the moment the pointer is off that control (onto a gap, another control, out of
        //     the scope), on any press in the scope (a click, the right-click that opens a menu), and
        //     `showFor` after it appeared even while the pointer stays;
        //   * a description hidden by the time limit or a press stays hidden while the pointer stays on
        //     its control, and may show again once the pointer has left it (a Windows tooltip's rule:
        //     without it, the jitter of a resting hand would bring the same text straight back).
        // Without it (the default: the Settings cog, the Sessions page) the panel stays STICKY: it
        // renders the moment the pointer reaches a control and keeps the last description until Hide().
        // The scope handlers read the switch at every event, so the call may come before or after
        // AttachScope; switching an attached panel over mid-hover only changes what the NEXT event does.
        //   showFor - how long a description may stay up while the pointer stays on its control; zero
        //             or negative means no time limit (moving off and pressing still hide it).
        // A no-op before Initialize. Never throws (it only records two values; the timers are made on
        // the first rest, on the UI thread that runs the pointer handlers).
        void SetTooltipTiming(std::chrono::milliseconds showFor) noexcept
        {
            if (!_s)
            {
                return;
            }
            _s->tooltipTiming = true;
            _s->showFor = showFor;
        }

        // Place the panel in `host` (a full-bleed layer BEHIND/AROUND the anchor — e.g. the settings
        // overlay Grid) anchored OUTSIDE `anchor`'s top-LEFT: same top edge, right edge pinned `gap`
        // px left of the anchor, growing only leftward (up to kMaxWidthPx / the window's left edge).
        // Re-anchors itself on host/anchor SizeChanged — an anchor that re-centers (the card's height
        // changes per settings tab) or a window resize both land there.
        void AnchorTopLeftOutside(const winrt::Windows::UI::Xaml::Controls::Panel& host,
                                  const winrt::Windows::UI::Xaml::FrameworkElement& anchor,
                                  double gap = 12.0)
        {
            if (!_s || !host || !anchor)
            {
                return;
            }
            _s->mode = AnchorMode::TopLeftOutside;
            _s->pad = gap;
            _WireAnchor(host, anchor);
        }

        // Place the panel in `host` ABOVE `anchor`'s top-right: right edges ALIGNED, top pinned at
        // the HOST's top + topPad — the placement for an empty header zone that sits over a pane
        // (the Sessions page's header-right alignment space above its detail pane). A long text may
        // extend down OVER the anchor's top; pair with SetClickThrough(true) so it can never block.
        void AnchorTopRightAbove(const winrt::Windows::UI::Xaml::Controls::Panel& host,
                                 const winrt::Windows::UI::Xaml::FrameworkElement& anchor,
                                 double topPad = 10.0)
        {
            if (!_s || !host || !anchor)
            {
                return;
            }
            _s->mode = AnchorMode::TopRightAbove;
            _s->pad = topPad;
            _WireAnchor(host, anchor);
        }

        // Place the panel in `host` NESTED `pad` px inside `anchor`'s top-right corner (the Manager
        // tab's board region). Floats over the anchor's own content — pair with
        // SetClickThrough(true).
        void AnchorTopRightInside(const winrt::Windows::UI::Xaml::Controls::Panel& host,
                                  const winrt::Windows::UI::Xaml::FrameworkElement& anchor,
                                  double pad = 8.0)
        {
            if (!_s || !host || !anchor)
            {
                return;
            }
            _s->mode = AnchorMode::TopRightInside;
            _s->pad = pad;
            _WireAnchor(host, anchor);
        }

        // Manual show (the scope routing calls this too): title may be empty — the heading row +
        // its rule collapse. While the cramped fallback is active the content is recorded but the
        // panel stays hidden (the floating tips are serving); room returning restores it.
        void Show(const winrt::hstring& title, const winrt::hstring& body)
        {
            if (_s)
            {
                _ShowIn(_s, title, body);
            }
        }

        // Collapse the panel + drop the sticky content and the hover throttle — so the owner's
        // next reveal starts hidden-until-hover and re-hovering the SAME control re-renders. For a
        // timed panel (SetTooltipTiming) it also ends a rest wait in progress (or the description
        // would appear after the owner hid its page), stops the time limit, and forgets the
        // dismissed control, so the next hover of that control shows it again. The owner calls it
        // when its surface leaves the screen (the cog's close, the Sessions page's close, the
        // Manager page's hide).
        // A no-op before Initialize. UI thread only (DispatcherTimer and the panel are thread-affine);
        // throws what a XAML property write or a timer Stop throws there, which in practice is nothing.
        void Hide()
        {
            if (!_s)
            {
                return;
            }
            _s->root.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
            _s->shown = false;
            _s->lastTarget = nullptr;
            // The timing's state; all of it is already empty on a sticky panel (no timer was ever
            // made, nothing was ever shown by key, pending or dismissed), so the cog and the Sessions
            // page pay nothing.
            _s->shownKey.clear();
            _CancelRest(_s);
            if (_s->hideTimer)
            {
                _s->hideTimer.Stop();
            }
            _s->dismissed = nullptr;
            _s->dismissedKey.clear();
        }

    private:
        static constexpr double kMaxWidthPx = 340.0; // readability cap (the floating ToolTip's ~320 family)
        static constexpr double kMinRoomPx = 140.0; // below this much room for the panel -> cramped fallback
        static constexpr double kEdgePadPx = 8.0; // breathing room at the host's left/bottom edges

        // Which corner the panel is pinned to, relative to the anchor (see the Anchor* methods).
        enum class AnchorMode
        {
            TopLeftOutside, // settings: left of the anchor, tops aligned
            TopRightAbove, // sessions: right edges aligned, top at the HOST's top
            TopRightInside, // manager: nested inside the anchor's top-right corner
        };

        struct State
        {
            winrt::Windows::UI::Xaml::Controls::Border root{ nullptr };
            winrt::Windows::UI::Xaml::Controls::TextBlock title{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border divider{ nullptr };
            winrt::Windows::UI::Xaml::Controls::TextBlock body{ nullptr };
            // Hover throttle: the last element rendered — WEAK, so a rebuilt-away element is never
            // pinned by the panel (the AgentTipHelpers leak discipline). On a TIMED panel it is the
            // control whose description shows now (null whenever the panel is hidden), which the
            // move, exit and time-limit rules compare against.
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> lastTarget{ nullptr };
            // TOOLTIP TIMING (SetTooltipTiming): on = the panel shows a description after a rest and
            // hides it on a move off its control, a press or the time limit; off (the default) = the
            // sticky panel. Read by every scope handler, so it can be switched on after AttachScope.
            bool tooltipTiming{ false };
            // How long a timed description may stay up while the pointer stays on its control; zero
            // or negative = no limit. Meaningless while tooltipTiming is off.
            std::chrono::milliseconds showFor{ 0 };
            // The shown description's KEY (_TipKey: title + text) on a timed panel; empty while hidden.
            // "The same control" is this key OR the same element: the board and the tree REBUILD their
            // cards and rows on registry changes, so the control under a resting pointer can become a
            // new element with the same description — by element alone, every rebuild would blink
            // the panel off and back on and restart its time limit. Two different controls that share
            // a description read as one, which shows the same text anyway.
            std::wstring shownKey;
            // The control a rest wait is armed on (the restTimer's target) — WEAK, like lastTarget; a
            // control rebuilt away during the wait resolves null at the tick, which then shows nothing.
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> pending{ nullptr };
            // The control whose description the time limit or a press hid. It stays hidden while the
            // pointer stays on it — the resting hand's jitter would otherwise bring the same text
            // straight back — and the first event that finds the pointer elsewhere forgets it. WEAK;
            // a rebuilt twin (same dismissedKey) is adopted in its place by the move rule.
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> dismissed{ nullptr };
            // The dismissed control's KEY (see shownKey); empty = nothing is dismissed. The key, not
            // the weak element, says whether a dismissal is in force: the element may be gone (rebuilt)
            // while the pointer still rests on its twin.
            std::wstring dismissedKey;
            // One-shot rest wait (_ArmRest; Interval = _RestDelay()). Made on the first rest of a timed
            // panel, on the UI thread; its Tick holds the State WEAKLY (this struct owns it) and stops
            // its sender first, so it can never tick on after its panel is gone.
            winrt::Windows::UI::Xaml::DispatcherTimer restTimer{ nullptr };
            // One-shot time limit (_StartHideTimer; Interval = showFor), running only while a timed
            // description shows; the same weak-capture and self-stop rules as restTimer.
            winrt::Windows::UI::Xaml::DispatcherTimer hideTimer{ nullptr };
            // The scope roots whose LocalTipScopeProperty the cramped fallback flips in lockstep
            // (weak: a root's own PointerMoved handler must not keep it alive).
            std::vector<winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement>> scopeRoots;
            AnchorMode mode{ AnchorMode::TopLeftOutside };
            double pad{ 12.0 }; // the mode's gap/topPad/pad (see the Anchor* methods)
            bool cramped{ true }; // no room to host the panel — floating tips serve; true until the first _Reposition proves room
            bool crampedSynced{ false }; // the first _Reposition must PUSH the computed state onto the scope markers even when it equals the seed (a genuinely-cramped first layout would otherwise leave the markers suppressing with the panel hidden — no tips at all)
            bool shown{ false }; // a Show happened since the last Hide (what a cramped->roomy flip restores)
            // Change gates for the anchor writes (never re-set an unchanged layout property).
            double lastTop{ -1.0 };
            double lastRight{ -1.0 };
            double lastMaxW{ -1.0 };
            double lastMaxH{ -1.0 };
        };

        static void _ShowIn(const std::shared_ptr<State>& s, const winrt::hstring& title, const winrt::hstring& body)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            const bool hasTitle = !title.empty();
            s->title.Text(title);
            s->title.Visibility(hasTitle ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
            s->divider.Visibility(hasTitle ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
            s->body.Text(body);
            s->shown = true;
            if (!s->cramped)
            {
                s->root.Visibility(WUX::Visibility::Visible);
            }
        }

        // The scope-level hover resolution: nearest ancestor of OriginalSource (up to the scope
        // root) carrying an AgentSetTip text — the innermost tipped element wins, matching the
        // floating recipe's nested-tip rule. Shared by the move handler and the timing's press handler.
        //   sender - the scope root whose handler runs (the walk's upper bound: it never leaves the scope).
        //   args   - the pointer event; its OriginalSource is where the walk starts.
        //   text   - out: the found element's tip text; left empty when nothing is found.
        //   returns - the innermost tipped FrameworkElement at or above OriginalSource within 48 levels
        //             and inside the scope, else null (no source, nothing tipped, or the first tipped
        //             ancestor is not a FrameworkElement — the walk stops there, as it always has).
        // Throws nothing in practice: plain attached-property reads and VisualTreeHelper::GetParent on
        // the UI thread that raised the event.
        static winrt::Windows::UI::Xaml::FrameworkElement _ResolveTipped(const winrt::Windows::Foundation::IInspectable& sender,
                                                                         const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args,
                                                                         winrt::hstring& text)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace atd = agent_tip_details;
            text = {};
            const auto src = args.OriginalSource();
            if (!src)
            {
                return nullptr;
            }
            const auto stop = sender.try_as<WUX::DependencyObject>(); // the scope root (the handler's owner)
            WUX::DependencyObject node = src.try_as<WUX::DependencyObject>();
            for (int i = 0; i < 48 && node; ++i)
            {
                if (const auto ui = node.try_as<WUX::UIElement>())
                {
                    if (const auto t = atd::TipTextOf(ui); !t.empty())
                    {
                        // The first tipped ancestor decides, FrameworkElement or not: a non-FE answers
                        // "nothing", exactly as the walk did before it was shared.
                        const auto found = node.try_as<WUX::FrameworkElement>();
                        if (found)
                        {
                            text = t;
                        }
                        return found;
                    }
                }
                if (stop && node == stop)
                {
                    break; // the walk never leaves the scope
                }
                node = WUX::Media::VisualTreeHelper::GetParent(node);
            }
            return nullptr;
        }

        // The scope root's PointerMoved. A STICKY panel renders the resolved control at once and,
        // with no tipped element under the pointer, keeps the last description (the title row says
        // which control it belongs to); a TIMED panel hands the resolved control to
        // _OnScopePointerMovedTimed.
        //   s      - the panel's State (null is ignored).
        //   sender - the scope root (the resolution's upper bound).
        //   args   - the move.
        // Throws what writing the panel's TextBlocks / Visibility throws on the sticky path (nothing in
        // practice); the timed path contains and logs its own failures (Rule #18).
        static void _OnScopePointerMoved(const std::shared_ptr<State>& s,
                                         const winrt::Windows::Foundation::IInspectable& sender,
                                         const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args)
        {
            if (!s)
            {
                return;
            }
            winrt::hstring text;
            const auto found = _ResolveTipped(sender, args, text);
            if (s->tooltipTiming)
            {
                _OnScopePointerMovedTimed(s, found, text);
                return;
            }
            if (!found || text.empty())
            {
                return; // sticky: gaps between controls don't strobe the panel empty
            }
            if (s->lastTarget.get() == found)
            {
                return; // same control as the last move — no re-render (throttle on the resolved target)
            }
            s->lastTarget = winrt::make_weak(found);
            _ShowIn(s, _DeriveTitleFor(found), text);
        }

        // A TIMED panel's move rule (SetTooltipTiming): the panel describes only the control under a
        // RESTING pointer, and only while the pointer stays on it. "The same control" is the same
        // element OR the same description key (State::shownKey says why: rebuilt cards and rows).
        //   s     - the panel's State (non-null; the caller checked).
        //   found - the tipped control under the pointer now (_ResolveTipped), or null for a gap.
        //   text  - found's tip text (_ResolveTipped's out value; empty with a null found).
        // Never throws: a failure is logged (Rule #18) and the panel is left hidden, because an
        // exception escaping a XAML event handler is reported as unhandled and can end the process.
        static void _OnScopePointerMovedTimed(const std::shared_ptr<State>& s,
                                              const winrt::Windows::UI::Xaml::FrameworkElement& found,
                                              const winrt::hstring& text) noexcept
        {
            try
            {
                // found's key, worked out at most once and only when an element compare was not
                // enough (a title derivation is a few casts and a property read; most moves stay on
                // the very element already compared).
                std::wstring foundKey;
                const auto keyOfFound = [&]() -> const std::wstring& {
                    if (foundKey.empty() && found)
                    {
                        foundKey = _TipKey(_DeriveTitleFor(found), text);
                    }
                    return foundKey;
                };
                // The dismissed control stays quiet only while the pointer is still ON it: the first
                // move that resolves anything else (a gap included) forgets it, so coming back shows
                // it. A rebuilt twin of it (same key) is adopted, so the exit test measures the
                // element that is really under the pointer.
                bool onDismissed = false;
                if (!s->dismissedKey.empty())
                {
                    onDismissed = found && (s->dismissed.get() == found || keyOfFound() == s->dismissedKey);
                    if (onDismissed)
                    {
                        if (s->dismissed.get() != found)
                        {
                            s->dismissed = winrt::make_weak(found);
                        }
                    }
                    else
                    {
                        s->dismissed = nullptr;
                        s->dismissedKey.clear();
                    }
                }
                // Moving inside the control whose description shows keeps it (its time limit still
                // runs), a rebuilt twin included (adopted, for the exit test). This also absorbs the
                // synthetic PointerMoved XAML raises when the panel's appearance or a rebuild makes
                // it hit-test again: same position, same control.
                if (found && s->shown && (s->lastTarget.get() == found || keyOfFound() == s->shownKey))
                {
                    if (s->lastTarget.get() != found)
                    {
                        s->lastTarget = winrt::make_weak(found);
                    }
                    return;
                }
                // Off the described control (a gap, another control): its description goes now.
                if (s->shown)
                {
                    _HideIn(s);
                }
                if (!found || onDismissed)
                {
                    _CancelRest(s); // nothing to describe, or a control the user just dismissed
                    return;
                }
                // A control under a moving pointer: (re)start the rest wait. Every move restarts it,
                // so a pointer sweeping across the page never shows a description.
                _ArmRest(s, found);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"local-tip timed move");
            }
        }

        // A description's KEY: the panel's title and the tip text, joined by U+001F (a control
        // character no title or tip text carries). Equal keys mean the panel would show the same
        // thing, which is what the timing treats as the same control (State::shownKey).
        //   title - the panel's title for the control (_DeriveTitleFor; may be empty).
        //   text  - the control's tip text.
        //   returns - the key.
        // Throws std::bad_alloc; the timed handlers that call it contain it.
        static std::wstring _TipKey(const winrt::hstring& title, const winrt::hstring& text)
        {
            std::wstring key;
            key.reserve(title.size() + 1 + text.size());
            key.append(title.c_str(), title.size());
            key.push_back(L'\x1f');
            key.append(text.c_str(), text.size());
            return key;
        }

        // The scope root's PointerExited (handledEventsToo), acted on by a TIMED panel only. PointerExited
        // BUBBLES: the root also hears the exit of every child the pointer crosses out of, inside the
        // scope or not. So the event is never taken as "the pointer left": each element the timing
        // keeps — the described control, the rest wait's control, the dismissed control — is tested
        // against the pointer's position, and only the ones the pointer is now OUTSIDE are let go.
        // That is what hides the description when the pointer leaves for a place the scope hears no
        // move from (a splitter outside the scope, a popup's menu, another region, the window's edge).
        // An element no longer in the tree (a card the board rebuilt) cannot be measured: for it only
        // a pointer outside the whole SCOPE counts as a leave — inside, the next move re-resolves its
        // rebuilt twin by key (State::shownKey), and the time limit bounds the rest.
        //   s      - the panel's State (null is ignored).
        //   sender - the scope root whose handler runs (the bounds of "left the scope").
        //   args   - the exit; only its pointer position is read.
        // Never throws: a failure is logged (Rule #18).
        static void _OnScopePointerExited(const std::shared_ptr<State>& s,
                                          const winrt::Windows::Foundation::IInspectable& sender,
                                          const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args) noexcept
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            if (!s || !s->tooltipTiming)
            {
                return; // a sticky panel ignores exits: its last description stays up by design
            }
            try
            {
                const auto scope = sender.try_as<WUX::FrameworkElement>();
                const bool leftScope = !_PointerWithin(scope, args); // a null scope reads as left: reset rather than strand
                // Is the pointer off this kept element? Measured when the element is in the tree;
                // otherwise only leaving the scope says so (see above).
                const auto off = [&](const winrt::weak_ref<WUX::FrameworkElement>& kept) {
                    const auto el = kept.get();
                    if (el && el.IsLoaded())
                    {
                        return !_PointerWithin(el, args);
                    }
                    return leftScope;
                };
                if (s->shown && off(s->lastTarget))
                {
                    _HideIn(s);
                }
                if (off(s->pending))
                {
                    _CancelRest(s); // it left before it rested: the wait's tick must not show it afterwards
                }
                if (!s->dismissedKey.empty() && off(s->dismissed))
                {
                    // It left the dismissed control: coming back may show it again.
                    s->dismissed = nullptr;
                    s->dismissedKey.clear();
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"local-tip timed exit");
            }
        }

        // The scope root's PointerPressed (handledEventsToo), acted on by a TIMED panel only: any press
        // in the scope (a click on a button, the right-click that opens a card's menu, a splitter's
        // grab) hides the description, as a Windows tooltip does on a click, and dismisses the control
        // under the pointer until the pointer leaves it — the pointer usually rests there after the
        // click, so without that the same description would return half a second later.
        //   s      - the panel's State (null is ignored).
        //   sender - the scope root (the resolution's upper bound).
        //   args   - the press; its OriginalSource names the pressed control.
        // Never throws: a failure is logged (Rule #18).
        static void _OnScopePointerPressed(const std::shared_ptr<State>& s,
                                           const winrt::Windows::Foundation::IInspectable& sender,
                                           const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args) noexcept
        {
            if (!s || !s->tooltipTiming)
            {
                return; // a sticky panel keeps showing through clicks (the cog's rows are edited under it)
            }
            try
            {
                winrt::hstring text;
                const auto found = _ResolveTipped(sender, args, text);
                _CancelRest(s);
                if (found)
                {
                    s->dismissed = winrt::make_weak(found);
                    s->dismissedKey = _TipKey(_DeriveTitleFor(found), text);
                }
                else
                {
                    // A press on a gap dismisses nothing; it still hides below.
                    s->dismissed = nullptr;
                    s->dismissedKey.clear();
                }
                if (s->shown)
                {
                    _HideIn(s);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"local-tip timed press");
            }
        }

        // How long the pointer must rest on a control before a TIMED panel shows its description:
        // Windows' own mouse hover time (SPI_GETMOUSEHOVERTIME, 400 ms by default — a user who slows
        // it in Windows gets a slower panel too). The FULL hover time, not the floating tip's third of
        // it (agent_tip_details::DefaultOpenDelay): the panel is the rest the owner tried in the mock
        // and picked (2026-11-13), and a far-away panel need not race the pointer. Ignores a control's
        // own open delay (AgentSetTip's override, the board cards' 4 s): that throttles the FLOATING
        // tip, which only a too-narrow window shows here. Read once per process, as DefaultOpenDelay.
        //   returns - the hover time, or 400 ms when Windows reports none.
        // Never throws.
        static std::chrono::milliseconds _RestDelay() noexcept
        {
            static const auto delay = []() noexcept {
                unsigned int hoverMs{ 400 };
                if (!::SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hoverMs, 0) || hoverMs == 0)
                {
                    hoverMs = 400;
                }
                return std::chrono::milliseconds{ hoverMs };
            }();
            return delay;
        }

        // Is the pointer of `args` inside `el`'s own bounds, at least 1 px in from every edge? The one
        // test behind every exit decision (see _OnScopePointerExited for why the event alone cannot
        // tell). The outermost pixel counts as outside, so a real leave sampled right on the edge is
        // never taken for a stay (AgentManagerContent's PointerStillWithin draws the same line).
        //   el   - the element to test; null, or no longer in the live tree (a card rebuilt away under
        //          the pointer), reads as outside, so the panel never keeps describing what is gone.
        //   args - the pointer event whose current position is tested.
        //   returns - true while the pointer is inside el; false otherwise, and when the position
        //             cannot be read.
        // Never throws: a failed position read is logged (Rule #18) and reads as outside.
        static bool _PointerWithin(const winrt::Windows::UI::Xaml::FrameworkElement& el,
                                   const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args) noexcept
        {
            if (!el)
            {
                return false;
            }
            try
            {
                if (!el.IsLoaded())
                {
                    return false; // GetCurrentPoint relative to an element out of the tree has nothing to measure against
                }
                const auto p = args.GetCurrentPoint(el).Position();
                constexpr double kEdge = 1.0;
                return p.X > kEdge && p.Y > kEdge && p.X < el.ActualWidth() - kEdge && p.Y < el.ActualHeight() - kEdge;
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"local-tip pointer position");
                return false;
            }
        }

        // Collapse the panel and forget which control it describes (its element and its key); a
        // TIMED panel's time limit stops with it (a limit that outlived its description would cut the
        // NEXT one short). The rest wait and the dismissed control are left to the caller: a move off
        // a control arms the next wait at once, and the time limit marks the control it hid as
        // dismissed (it copies shownKey BEFORE calling this).
        //   s - the panel's State (non-null; every caller checked).
        // UI thread only. Throws what a XAML property write or a timer Stop throws there (nothing in
        // practice); the timed handlers that call it contain that themselves.
        static void _HideIn(const std::shared_ptr<State>& s)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            if (s->root.Visibility() != WUX::Visibility::Collapsed)
            {
                s->root.Visibility(WUX::Visibility::Collapsed); // change-gated: no layout pass for a panel already down
            }
            s->shown = false;
            s->lastTarget = nullptr;
            s->shownKey.clear();
            if (s->hideTimer)
            {
                s->hideTimer.Stop();
            }
        }

        // End a rest wait in progress: its tick will not show anything.
        //   s - the panel's State (non-null; every caller checked).
        // UI thread only (DispatcherTimer is thread-affine). Throws nothing in practice.
        static void _CancelRest(const std::shared_ptr<State>& s)
        {
            if (s->restTimer)
            {
                s->restTimer.Stop();
            }
            s->pending = nullptr;
        }

        // (Re)start the rest wait on `el`: its description shows once the pointer has stayed on it
        // for _RestDelay() with no move in between. Every move over the same control calls this again
        // — that restart is what makes it a REST test (the floating recipe's re-arm). The timer is
        // made on first use, on the UI thread running the pointer handlers; its Tick stops its own
        // sender before anything else and holds the State only weakly (the State owns the timer: a
        // strong capture would keep the panel alive after its page, and a weak one that just returned
        // on a dead State would leave a repeating DispatcherTimer ticking forever).
        //   s  - the panel's State (non-null).
        //   el - the control to describe once the pointer rests on it.
        // UI thread only. Throws what making or starting a DispatcherTimer throws (the timed move
        // handler, its only caller, contains it).
        static void _ArmRest(const std::shared_ptr<State>& s, const winrt::Windows::UI::Xaml::FrameworkElement& el)
        {
            s->pending = winrt::make_weak(el);
            if (!s->restTimer)
            {
                winrt::Windows::UI::Xaml::DispatcherTimer timer;
                timer.Interval(_RestDelay());
                std::weak_ptr<State> weak = s;
                timer.Tick([weak](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable&) {
                    _OnTimerTick(sender, weak, &_OnRestTick, L"local-tip rest tick");
                });
                s->restTimer = timer;
            }
            s->restTimer.Stop(); // Stop + Start restarts the full wait (the explicit form ArmOpenTimer uses too)
            s->restTimer.Start();
        }

        // Start (or restart) a TIMED description's time limit: `showFor` after this call the panel
        // hides. No limit when showFor is zero or negative. Made on first use, with the restTimer's
        // self-stop and weak-capture rules (see _ArmRest).
        //   s - the panel's State (non-null).
        // UI thread only. Throws what making or starting a DispatcherTimer throws (its caller, the rest
        // tick, contains it).
        static void _StartHideTimer(const std::shared_ptr<State>& s)
        {
            if (s->showFor.count() <= 0)
            {
                return;
            }
            if (!s->hideTimer)
            {
                winrt::Windows::UI::Xaml::DispatcherTimer timer;
                std::weak_ptr<State> weak = s;
                timer.Tick([weak](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable&) {
                    _OnTimerTick(sender, weak, &_OnHideTick, L"local-tip time limit");
                });
                s->hideTimer = timer;
            }
            s->hideTimer.Stop();
            s->hideTimer.Interval(s->showFor); // read at every show: the owner may have changed it since
            s->hideTimer.Start();
        }

        // The shared body of both timers' Ticks: make the DispatcherTimer one-shot by stopping its
        // SENDER first (it repeats until stopped, and a State that is gone could not stop it), then run
        // `step` on the State if it still lives — all inside one catch, because an exception escaping
        // a Tick is an unhandled XAML error that can end the process.
        //   sender  - the ticking DispatcherTimer.
        //   weak    - the panel's State, held weakly by the timer (see _ArmRest).
        //   step    - what the tick does with a live State (_OnRestTick or _OnHideTick).
        //   context - the [exc] context logged if anything throws (Rule #18).
        // Never throws.
        static void _OnTimerTick(const winrt::Windows::Foundation::IInspectable& sender,
                                 const std::weak_ptr<State>& weak,
                                 void (*step)(const std::shared_ptr<State>&),
                                 const wchar_t* context) noexcept
        {
            try
            {
                if (const auto timer = sender.try_as<winrt::Windows::UI::Xaml::DispatcherTimer>())
                {
                    timer.Stop();
                }
                if (const auto s = weak.lock())
                {
                    step(s);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(context);
            }
        }

        // The rest wait ran out with the pointer still on its control: show that control's
        // description and start its time limit. Shows nothing for a control rebuilt away or taken out
        // of the tree during the wait (a board rebuild), or whose tip was silenced (AgentSuppressTip).
        //   s - the panel's State (non-null; _OnTimerTick checked).
        // Throws what writing the panel or starting the time limit throws (contained by _OnTimerTick).
        static void _OnRestTick(const std::shared_ptr<State>& s)
        {
            const auto el = s->pending.get();
            s->pending = nullptr;
            if (!el || !el.IsLoaded())
            {
                return;
            }
            const auto text = agent_tip_details::TipTextOf(el);
            if (text.empty())
            {
                return;
            }
            const auto title = _DeriveTitleFor(el);
            s->lastTarget = winrt::make_weak(el);
            s->shownKey = _TipKey(title, text);
            _ShowIn(s, title, text);
            _StartHideTimer(s);
        }

        // The time limit ran out: hide the description and dismiss its control, so it stays hidden
        // while the pointer stays there (the next event that finds the pointer elsewhere lifts that).
        //   s - the panel's State (non-null; _OnTimerTick checked).
        // Throws what collapsing the panel throws (contained by _OnTimerTick).
        static void _OnHideTick(const std::shared_ptr<State>& s)
        {
            if (!s->shown)
            {
                return; // already hidden by a move, an exit, a press or Hide(): nothing left to time out
            }
            s->dismissed = s->lastTarget;
            s->dismissedKey = s->shownKey; // copied before _HideIn clears it
            _HideIn(s);
        }

        // A Header property's display string: the boxed hstring every settings control uses, or a
        // TextBlock header's text; anything else (custom header content) yields no title.
        static winrt::hstring _HeaderText(const winrt::Windows::Foundation::IInspectable& header)
        {
            if (!header)
            {
                return {};
            }
            if (const auto str = winrt::unbox_value_or<winrt::hstring>(header, winrt::hstring{}); !str.empty())
            {
                return str;
            }
            if (const auto tb = header.try_as<winrt::Windows::UI::Xaml::Controls::TextBlock>())
            {
                return tb.Text();
            }
            return {};
        }

        // The panel's TITLE for a hovered element: the explicit AgentSetTipTitle override first,
        // else the element's own label — its Header (TextBox/ComboBox/ToggleSwitch/Slider/
        // AutoSuggestBox, no common projection interface so each is tried) or its string Content
        // (Button/CheckBox/HyperlinkButton). Empty (a glyph button, a bare TextBlock) collapses
        // the heading row — the body alone still shows.
        static winrt::hstring _DeriveTitleFor(const winrt::Windows::UI::Xaml::FrameworkElement& fe)
        {
            namespace WUXC = winrt::Windows::UI::Xaml::Controls;
            namespace atd = agent_tip_details;
            if (!fe)
            {
                return {};
            }
            if (const auto explicitTitle = winrt::unbox_value_or<winrt::hstring>(fe.GetValue(atd::TipTitleProperty()), winrt::hstring{}); !explicitTitle.empty())
            {
                return explicitTitle;
            }
            if (const auto c = fe.try_as<WUXC::TextBox>())
            {
                return _HeaderText(c.Header());
            }
            if (const auto c = fe.try_as<WUXC::ComboBox>())
            {
                return _HeaderText(c.Header());
            }
            if (const auto c = fe.try_as<WUXC::ToggleSwitch>())
            {
                return _HeaderText(c.Header());
            }
            if (const auto c = fe.try_as<WUXC::Slider>())
            {
                return _HeaderText(c.Header());
            }
            if (const auto c = fe.try_as<WUXC::AutoSuggestBox>())
            {
                return _HeaderText(c.Header());
            }
            if (const auto cc = fe.try_as<WUXC::ContentControl>())
            {
                return winrt::unbox_value_or<winrt::hstring>(cc.Content(), winrt::hstring{});
            }
            return {};
        }

        // The shared anchor plumbing behind the Anchor* methods: mount the panel into the host,
        // widen its slot to the WHOLE grid, and wire the re-anchor triggers.
        void _WireAnchor(const winrt::Windows::UI::Xaml::Controls::Panel& host,
                         const winrt::Windows::UI::Xaml::FrameworkElement& anchor)
        {
            _s->root.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Right);
            _s->root.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Top);
            host.Children().Append(_s->root);
            // A Grid host: span every row/column (the settings-overlay idiom), so the floating
            // panel's slot is the WHOLE grid. Without this the panel lands in cell (0,0) — and an
            // Auto row (the sessions header, the manager toolbar) would GROW to fit it, shifting
            // the real content on every hover.
            if (host.try_as<winrt::Windows::UI::Xaml::Controls::Grid>())
            {
                winrt::Windows::UI::Xaml::Controls::Grid::SetRow(_s->root, 0);
                winrt::Windows::UI::Xaml::Controls::Grid::SetColumn(_s->root, 0);
                winrt::Windows::UI::Xaml::Controls::Grid::SetRowSpan(_s->root, 99);
                winrt::Windows::UI::Xaml::Controls::Grid::SetColumnSpan(_s->root, 99);
            }
            auto s = _s;
            auto wHost = winrt::make_weak(host);
            auto wAnchor = winrt::make_weak(anchor);
            const auto repos = [s, wHost, wAnchor](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::SizeChangedEventArgs&) {
                _Reposition(s, wHost.get(), wAnchor.get());
            };
            // SizeChanged (not LayoutUpdated) on both: it fires on the first real arrange too, and
            // the anchor only ever MOVES when one of the two resizes (the anchors here are
            // host-centered / host-proportional). Writes inside are to the sibling panel only +
            // change-gated — no layout cycle.
            host.SizeChanged(repos);
            anchor.SizeChanged(repos);
        }

        // Re-anchor: compute the mode's pin (top, right margin) and the ROOM the panel may grow
        // into, refine MaxWidth (readability-capped) + MaxHeight (to the host's bottom). Too little
        // room => the CRAMPED fallback: collapse the panel and lift every scope marker so the
        // classic floating tips serve — flipped back the moment room returns. All writes are
        // change-gated; the panel is a floating sibling, so none of them feed back into the
        // SizeChanged that ran this (no layout cycle).
        static void _Reposition(const std::shared_ptr<State>& s,
                                const winrt::Windows::UI::Xaml::Controls::Panel& host,
                                const winrt::Windows::UI::Xaml::FrameworkElement& anchor)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace atd = agent_tip_details;
            if (!s || !host || !anchor)
            {
                return;
            }
            double x = 0.0, y = 0.0;
            try
            {
                const auto pt = anchor.TransformToVisual(host).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
                x = pt.X;
                y = pt.Y;
            }
            catch (...)
            {
                // Likely benign (anchor not in the live tree yet — the next SizeChanged re-runs this),
                // but per the forensics policy the throw is still recorded (throttled).
                ::Agentmaster::AgentLogCaughtException(L"local-tip anchor transform");
                return;
            }
            const double hostW = host.ActualWidth();
            const double hostH = host.ActualHeight();
            if (hostW < 1.0 || hostH < 1.0)
            {
                return;
            }

            const double anchorW = anchor.ActualWidth();
            double top = 0.0; // margin.top — the panel's top edge
            double right = 0.0; // margin.right — HorizontalAlignment::Right puts the panel's right edge at (hostW - right)
            double room = 0.0; // how wide the panel may grow (leftward) at this placement
            switch (s->mode)
            {
            case AnchorMode::TopLeftOutside:
                // Right edge `pad` (gap) px LEFT of the anchor; tops aligned; room = the space
                // between the anchor and the host's left edge.
                room = x - s->pad - kEdgePadPx;
                top = std::max(0.0, y);
                right = std::max(0.0, hostW - x + s->pad);
                break;
            case AnchorMode::TopRightAbove:
                // Right edges ALIGNED; top pinned at the HOST's top — the empty zone ABOVE the
                // anchor; room = the anchor's own width (the column it heads).
                room = anchorW;
                top = s->pad;
                right = std::max(0.0, hostW - (x + anchorW));
                break;
            case AnchorMode::TopRightInside:
                // Nested `pad` px inside the anchor's top-right corner; room = the anchor's width
                // minus both pads.
                room = anchorW - 2.0 * s->pad;
                top = std::max(0.0, y + s->pad);
                right = std::max(0.0, hostW - (x + anchorW) + s->pad);
                break;
            }

            const bool cramped = room < kMinRoomPx;
            // Sync on EVERY transition AND on the first computation (crampedSynced): the seed is
            // cramped=true with the scope markers seeded ACTIVE, so a genuinely-cramped first
            // layout takes this branch to push the markers OFF (floating serves) — without it the
            // markers would keep suppressing while the panel stays hidden: no tips at all.
            if (!s->crampedSynced || cramped != s->cramped)
            {
                s->crampedSynced = true;
                s->cramped = cramped;
                for (const auto& weakScope : s->scopeRoots)
                {
                    if (const auto scope = weakScope.get())
                    {
                        // The live suppression switch: floating tips take over while cramped.
                        scope.SetValue(atd::LocalTipScopeProperty(), winrt::box_value(!cramped));
                    }
                }
                s->root.Visibility(!cramped && s->shown ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
            }
            if (cramped)
            {
                return;
            }

            const double maxW = std::min(kMaxWidthPx, room);
            const double maxH = std::max(60.0, hostH - top - kEdgePadPx);
            if (std::abs(top - s->lastTop) > 0.5 || std::abs(right - s->lastRight) > 0.5)
            {
                s->lastTop = top;
                s->lastRight = right;
                s->root.Margin(WUX::Thickness{ 0, top, right, 0 });
            }
            if (std::abs(maxW - s->lastMaxW) > 0.5)
            {
                s->lastMaxW = maxW;
                s->root.MaxWidth(maxW);
            }
            if (std::abs(maxH - s->lastMaxH) > 0.5)
            {
                s->lastMaxH = maxH;
                s->root.MaxHeight(maxH);
            }
        }

        std::shared_ptr<State> _s; // null until Initialize; the ONLY state (handlers capture it, never `this`)
    };
}
