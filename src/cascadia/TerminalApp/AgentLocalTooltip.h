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
//     when room returns. Graceful degradation, no re-wiring.
//   * PLACEMENT — AnchorTopLeftOutside(host, anchor) pins the panel's right edge `gap` px left
//     of the anchor's left edge and its top to the anchor's top, recomputed from the anchor's
//     REAL position (TransformToVisual) on host/anchor SizeChanged. HorizontalAlignment::Right
//     + a computed right margin is what makes it grow ONLY leftward. Layout-cycle-safe by
//     construction: the panel is a sibling whose size affects neither host nor anchor, and
//     every write is change-gated (the _ApplyRenamerMaxWidth lesson).
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
        // alignment) instead of via AnchorTopLeftOutside. Null before Initialize.
        winrt::Windows::UI::Xaml::UIElement Visual() const { return _s ? _s->root : nullptr; }

        // Route every AgentSetTip text under `scopeRoot` into this panel: ONE bubbling PointerMoved
        // resolves the hovered tipped element (innermost wins) and renders title+body immediately;
        // the scope marker makes the floating host yield inside (see the header block). One scope
        // per instance (the marker the cramped fallback flips is the LAST attached root).
        void AttachScope(const winrt::Windows::UI::Xaml::FrameworkElement& scopeRoot)
        {
            if (!_s || !scopeRoot)
            {
                return;
            }
            namespace atd = agent_tip_details;
            _s->scopeRoot = winrt::make_weak(scopeRoot);
            scopeRoot.SetValue(atd::LocalTipScopeProperty(), winrt::box_value(true));
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
            _s->root.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Right);
            _s->root.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Top);
            host.Children().Append(_s->root);
            auto s = _s;
            auto wHost = winrt::make_weak(host);
            auto wAnchor = winrt::make_weak(anchor);
            const auto repos = [s, wHost, wAnchor, gap](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::SizeChangedEventArgs&) {
                _Reposition(s, wHost.get(), wAnchor.get(), gap);
            };
            // SizeChanged (not LayoutUpdated) on both: it fires on the first real arrange too, and
            // the anchor only ever MOVES when one of the two resizes (the anchor is host-centered).
            // Writes inside are to the sibling panel only + change-gated — no layout cycle.
            host.SizeChanged(repos);
            anchor.SizeChanged(repos);
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
        static constexpr double kMinRoomPx = 140.0; // below this much room left of the anchor -> cramped fallback
        static constexpr double kEdgePadPx = 8.0; // breathing room at the host's left/bottom edges

        struct State
        {
            winrt::Windows::UI::Xaml::Controls::Border root{ nullptr };
            winrt::Windows::UI::Xaml::Controls::TextBlock title{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border divider{ nullptr };
            winrt::Windows::UI::Xaml::Controls::TextBlock body{ nullptr };
            // Hover throttle: the last element rendered — WEAK, so a rebuilt-away element is never
            // pinned by the panel (the AgentTipHelpers leak discipline).
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> lastTarget{ nullptr };
            // The scope root whose LocalTipScopeProperty the cramped fallback flips (weak: the
            // root's own PointerMoved handler must not keep it alive).
            winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> scopeRoot{ nullptr };
            bool cramped{ true }; // no room to host the panel — floating tips serve; true until the first _Reposition proves room
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

        // Re-anchor: pin the panel's right edge `gap` px left of the anchor and its top to the
        // anchor's top; refine MaxWidth to the room actually left of the anchor (readability-capped)
        // and MaxHeight to the host's bottom. Too little room => the CRAMPED fallback: collapse the
        // panel and lift the scope marker so the classic floating tips serve — flipped back the
        // moment room returns. All writes change-gated; the panel is a sibling of the anchor, so
        // none of them feed back into the SizeChanged that ran this (no layout cycle).
        static void _Reposition(const std::shared_ptr<State>& s,
                                const winrt::Windows::UI::Xaml::Controls::Panel& host,
                                const winrt::Windows::UI::Xaml::FrameworkElement& anchor,
                                double gap)
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

            const double room = x - gap - kEdgePadPx; // width available left of the anchor
            const bool cramped = room < kMinRoomPx;
            if (cramped != s->cramped)
            {
                s->cramped = cramped;
                if (const auto scope = s->scopeRoot.get())
                {
                    // The live suppression switch: floating tips take over while cramped.
                    scope.SetValue(atd::LocalTipScopeProperty(), winrt::box_value(!cramped));
                }
                s->root.Visibility(!cramped && s->shown ? WUX::Visibility::Visible : WUX::Visibility::Collapsed);
            }
            if (cramped)
            {
                return;
            }

            const double top = std::max(0.0, y);
            const double right = std::max(0.0, hostW - x + gap); // HorizontalAlignment::Right => right edge at (hostW - right) == anchor left - gap
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
