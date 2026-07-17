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
//   * ROUTING — AttachScope(root) installs ONE bubbling PointerMoved on the SCOPE root (the
//     InstallDevTooltipNames idiom): resolve the nearest ancestor of OriginalSource carrying a
//     tip text (innermost wins, like the floating recipe's nested-tip rule) and render it
//     IMMEDIATELY (a side panel is out of the pointer's path, so it needs no hover-rest delay,
//     can't flicker under the cursor, and can't eat a click). STICKY on purpose: crossing the
//     gaps between controls keeps the last description up (the title says which control it
//     describes), so panning down a settings column doesn't strobe the panel empty/full.
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
// per-element — one PointerMoved handler on the scope root, one panel, zero per-element
// handlers/allocations beyond the attached-property values AgentSetTip already wrote. The
// handlers capture a shared_ptr<State> (NOT `this`, so the class handle may be copied/reset
// freely; NOT the scope root, which would be an element→handler→element ref cycle) and the
// hover throttle holds the last target only WEAKLY, so a rebuilt-away element is never pinned.
// ToolTipService is never touched. Intended for scopes built ONCE (an overlay card, a page
// chrome) — attach to a per-refresh-rebuilt surface and you re-register a handler per rebuild.
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine
// directory. Header-only; relies on the pch projections (like AgentTipHelpers.h, which it
// builds on). Safe to include from AgentManagerContent.h — itself a .cpp-TU-only header.

#pragma once

#include <algorithm> // std::min/max — the anchor math
#include <memory> // the shared State the scope/anchor handlers capture
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
        // resolves the hovered tipped element (innermost wins) and renders title+body immediately;
        // the scope marker makes the floating host yield inside (see the header block). May be
        // called for SEVERAL roots feeding one panel (the Manager tab's toolbar/board/bottom) — the
        // cramped fallback flips every attached root's marker in lockstep.
        void AttachScope(const winrt::Windows::UI::Xaml::FrameworkElement& scopeRoot)
        {
            if (!_s || !scopeRoot)
            {
                return;
            }
            namespace atd = agent_tip_details;
            _s->scopeRoots.push_back(winrt::make_weak(scopeRoot));
            // Seed the live suppression marker: active, unless the anchor math has ALREADY proven
            // the window too cramped to host the panel — a root attached mid-cramped must not
            // suppress the floating tips that are currently serving.
            scopeRoot.SetValue(atd::LocalTipScopeProperty(), winrt::box_value(!(_s->crampedSynced && _s->cramped)));
            ++atd::Host().localTipScopes; // arm the floating host's suppression fast-path (this UI thread)
            // The ONE scope-level handler (capture-safe: the shared State only — never `this`, never
            // the scope root, which its own handler would cycle-pin).
            auto s = _s;
            scopeRoot.PointerMoved([s](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args) {
                _OnScopePointerMoved(s, sender, args);
            });
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
        // next reveal starts hidden-until-hover and re-hovering the SAME control re-renders.
        void Hide()
        {
            if (!_s)
            {
                return;
            }
            _s->root.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
            _s->shown = false;
            _s->lastTarget = nullptr;
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
            // pinned by the panel (the AgentTipHelpers leak discipline).
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> lastTarget{ nullptr };
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
        // floating recipe's nested-tip rule. No tipped element under the pointer => STICKY (keep
        // the last description; the title row says which control it belongs to).
        static void _OnScopePointerMoved(const std::shared_ptr<State>& s,
                                         const winrt::Windows::Foundation::IInspectable& sender,
                                         const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            namespace atd = agent_tip_details;
            const auto src = args.OriginalSource();
            if (!src || !s)
            {
                return;
            }
            const auto stop = sender.try_as<WUX::DependencyObject>(); // the scope root (the handler's owner)
            WUX::FrameworkElement found{ nullptr };
            winrt::hstring text;
            WUX::DependencyObject node = src.try_as<WUX::DependencyObject>();
            for (int i = 0; i < 48 && node; ++i)
            {
                if (const auto ui = node.try_as<WUX::UIElement>())
                {
                    if (const auto t = atd::TipTextOf(ui); !t.empty())
                    {
                        found = node.try_as<WUX::FrameworkElement>();
                        text = t;
                        break;
                    }
                }
                if (stop && node == stop)
                {
                    break; // the walk never leaves the scope
                }
                node = WUX::Media::VisualTreeHelper::GetParent(node);
            }
            if (!found || text.empty())
            {
                return; // sticky: gaps between controls don't strobe the panel empty
            }
            if (s->lastTarget.get() == found)
            {
                return; // same control as the last move — no re-render (the devtip throttle idiom)
            }
            s->lastTarget = winrt::make_weak(found);
            _ShowIn(s, _DeriveTitleFor(found), text);
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
                return; // not in the live tree yet — the next SizeChanged re-runs this
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
