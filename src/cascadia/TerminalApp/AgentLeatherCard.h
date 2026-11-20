// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the collapsed side panel's LEATHER CARD as XAML (SIDE_PANEL.md §2q): the builder that turns one leather
// of AgentMaster/SidePanelLeatherCard.h into elements. Two hosts draw it:
//   - the side panel's collapsed card (TerminalPage.AgentSidePanel.cpp, _SidePanelStripSkin): a Button whose BACKGROUND
//     is the leather's body — so the button's own PointerOver / Pressed states lighten and darken it through the three
//     brushes AgentInstallLeatherBodyBrushes puts in its resources — and whose CONTENT is the face built here;
//   - the cog's live preview (AgentManagerContent.Settings.cpp, Tabs & Overlay -> SIDE PANEL): the same face over a
//     Border painted with the rest body brush (AgentBuildLeatherCardPreview).
//
// THE FACE (bottom to top, the card's own frame — docked LEFT; a right dock mirrors it with AgentSetLeatherFaceMirrored):
// the gloss (a childless Border, a white gradient), the grain (a childless Border, an ImageBrush over a WriteableBitmap
// written once per device pixel), the rim (a childless Border, the burnished 1 DIP edge), the thread's shadow and the
// thread (two dashed Paths), the chevron's lip and stamp (two Paths). Every element is childless or a Shape — a rounded
// element WITH children would be a composition node every frame walks (CLAUDE.md, Gotchas "XAML and Islands"), though
// with ONE card per window the cost would be small anyway. The face takes no input (IsHitTestVisible false on its root):
// the host's own background takes the pointer, so a press anywhere on the card is the button's.
//
// Include from TUs that include pch.h first (the XAML projections come from the pch, as for AgentStatusGlyph.h). UI
// thread only: every function creates or edits XAML objects of the calling thread.

#pragma once

#include "AgentMaster/SidePanelLeatherCard.h" // the leathers' numbers, the stitch path, the grain (pure, harness-tested)

#include <cmath> // std::lround — the grain bitmap's pixel size
#include <cstring> // std::memcpy — the grain's bytes into the bitmap's buffer

namespace winrt::TerminalApp::implementation
{
    /// <summary>A leather color (0xAARRGGBB) as a XAML Color.</summary>
    /// <param name="argb">The color.</param>
    /// <returns>The same color. Never throws.</returns>
    inline winrt::Windows::UI::Color AgentLeatherColor(uint32_t argb) noexcept
    {
        return winrt::Windows::UI::Color{ static_cast<uint8_t>(argb >> 24), static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8), static_cast<uint8_t>(argb) };
    }

    /// <summary>
    /// Point an existing body brush at a leather in a pointer state: its two stops take the toned top and bottom colors.
    /// Editing IN PLACE (never a new brush) is the point: the button's visual states resolved their brushes from its
    /// resources when its template was applied, so a new brush in the dictionary would not reach a state already set up.
    /// </summary>
    /// <param name="brush">A brush made by AgentMakeLeatherBodyBrush (two stops); null or a different stop count changes
    /// nothing.</param>
    /// <param name="spec">The leather.</param>
    /// <param name="state">Which tone: rest, hover (lighter) or pressed (darker).</param>
    /// <exception cref="winrt::hresult_error">When XAML refuses the write (off the UI thread, a torn-down core).</exception>
    inline void AgentRetintLeatherBodyBrush(const winrt::Windows::UI::Xaml::Media::LinearGradientBrush& brush,
                                            const ::Agentmaster::SidePanelLeatherSpec& spec,
                                            ::Agentmaster::SidePanelCardState state)
    {
        if (!brush)
        {
            return;
        }
        const auto stops = brush.GradientStops();
        if (stops.Size() != 2)
        {
            return;
        }
        stops.GetAt(0).Color(AgentLeatherColor(::Agentmaster::SidePanelLeatherTone(spec.bodyTop, state)));
        stops.GetAt(1).Color(AgentLeatherColor(::Agentmaster::SidePanelLeatherTone(spec.bodyBottom, state)));
    }

    /// <summary>A new vertical body brush (top color to bottom color) for a leather in a pointer state.</summary>
    /// <param name="spec">The leather.</param>
    /// <param name="state">Which tone.</param>
    /// <returns>The brush: two stops at 0 and 1, from the top edge to the bottom edge of whatever it paints.</returns>
    /// <exception cref="winrt::hresult_error">When XAML refuses to build the brush.</exception>
    inline winrt::Windows::UI::Xaml::Media::LinearGradientBrush AgentMakeLeatherBodyBrush(const ::Agentmaster::SidePanelLeatherSpec& spec,
                                                                                         ::Agentmaster::SidePanelCardState state)
    {
        using namespace winrt::Windows::UI::Xaml::Media;
        LinearGradientBrush brush{};
        brush.StartPoint({ 0.5f, 0.0f });
        brush.EndPoint({ 0.5f, 1.0f });
        GradientStop top{};
        top.Offset(0.0);
        GradientStop bottom{};
        bottom.Offset(1.0);
        brush.GradientStops().Append(top);
        brush.GradientStops().Append(bottom);
        AgentRetintLeatherBodyBrush(brush, spec, state);
        return brush;
    }

    /// <summary>
    /// Make a Button wear a leather's body: its Background is the rest brush, and its PointerOver / Pressed states'
    /// brushes (the lightweight-styling keys ButtonBackgroundPointerOver / ButtonBackgroundPressed in its own Resources)
    /// are the hover and pressed tones. The first call creates the three brushes; every later call retints the SAME
    /// brushes (see AgentRetintLeatherBodyBrush for why), so a leather switch reaches a button already on screen.
    /// </summary>
    /// <param name="button">The card's button. ⚠ Call it once BEFORE the button's template is applied (before it enters
    /// the tree): the states resolve the two keys when the template is applied, and a key missing then falls back to the
    /// theme's grey for good.</param>
    /// <param name="spec">The leather.</param>
    /// <exception cref="winrt::hresult_error">When XAML refuses a write or a lookup.</exception>
    inline void AgentInstallLeatherBodyBrushes(const winrt::Windows::UI::Xaml::Controls::Button& button, const ::Agentmaster::SidePanelLeatherSpec& spec)
    {
        using namespace winrt::Windows::UI::Xaml::Media;
        using ::Agentmaster::SidePanelCardState;
        if (!button)
        {
            return;
        }
        const auto resources = button.Resources();
        // One key at a time: the brush in place is retinted; a missing (or foreign) one is created and inserted.
        const auto install = [&](const wchar_t* key, SidePanelCardState state) {
            const auto boxedKey = winrt::box_value(key);
            if (resources.HasKey(boxedKey))
            {
                if (const auto existing = resources.Lookup(boxedKey).try_as<LinearGradientBrush>())
                {
                    AgentRetintLeatherBodyBrush(existing, spec, state);
                    return;
                }
            }
            resources.Insert(boxedKey, AgentMakeLeatherBodyBrush(spec, state));
        };
        install(L"ButtonBackgroundPointerOver", SidePanelCardState::Hover);
        install(L"ButtonBackgroundPressed", SidePanelCardState::Pressed);
        if (const auto rest = button.Background().try_as<LinearGradientBrush>(); rest && rest.GradientStops().Size() == 2)
        {
            AgentRetintLeatherBodyBrush(rest, spec, SidePanelCardState::Rest);
        }
        else
        {
            button.Background(AgentMakeLeatherBodyBrush(spec, SidePanelCardState::Rest));
        }
    }

    /// <summary>The card's corner radii for a dock: square on the docked edge (the card is a tab sewn to it), rounded on
    /// the free side.</summary>
    /// <param name="spec">The leather (its radius).</param>
    /// <param name="dockLeft">True when the panel docks left (the card is on the left edge).</param>
    /// <returns>The CornerRadius (TopLeft, TopRight, BottomRight, BottomLeft). Never throws.</returns>
    inline winrt::Windows::UI::Xaml::CornerRadius AgentLeatherCardRadius(const ::Agentmaster::SidePanelLeatherSpec& spec, bool dockLeft) noexcept
    {
        const double r = spec.radius;
        return dockLeft ? winrt::Windows::UI::Xaml::CornerRadius{ 0, r, r, 0 } : winrt::Windows::UI::Xaml::CornerRadius{ r, 0, 0, r };
    }

    /// <summary>The thread's path (SidePanelStitchPathFor) as a XAML geometry in the card's frame.</summary>
    /// <param name="path">The path.</param>
    /// <returns>A new geometry with one open or closed, unfilled figure. A geometry belongs to ONE Path: build one per
    /// Path.</returns>
    /// <exception cref="winrt::hresult_error">When XAML refuses to build an element.</exception>
    inline winrt::Windows::UI::Xaml::Media::PathGeometry AgentLeatherStitchGeometry(const ::Agentmaster::SidePanelStitchPath& path)
    {
        using namespace winrt::Windows::UI::Xaml::Media;
        PathFigure figure{};
        figure.StartPoint({ static_cast<float>(path.startX), static_cast<float>(path.startY) });
        figure.IsClosed(path.closed);
        figure.IsFilled(false);
        for (size_t i = 0; i < path.count && i < path.steps.size(); ++i)
        {
            const auto& step = path.steps[i];
            const winrt::Windows::Foundation::Point to{ static_cast<float>(step.x), static_cast<float>(step.y) };
            if (step.arc)
            {
                // A clockwise quarter turn: the corners of a path that runs top -> free side -> bottom are all clockwise.
                ArcSegment arc{};
                arc.Point(to);
                arc.Size({ static_cast<float>(step.radius), static_cast<float>(step.radius) });
                arc.SweepDirection(SweepDirection::Clockwise);
                arc.IsLargeArc(false);
                figure.Segments().Append(arc);
            }
            else
            {
                LineSegment line{};
                line.Point(to);
                figure.Segments().Append(line);
            }
        }
        PathGeometry geometry{};
        geometry.Figures().Append(figure);
        return geometry;
    }

    /// <summary>
    /// A Path the size of the card (12 × 40 DIPs, at its top-left), so the geometry's coordinates ARE the card's: the
    /// common shape of every stroke on the face. It takes no input.
    /// </summary>
    /// <param name="geometry">What it draws.</param>
    /// <param name="color">Its stroke color, 0xAARRGGBB (the alpha is the stroke's strength).</param>
    /// <param name="thickness">Its stroke thickness, DIPs.</param>
    /// <param name="dy">A downward shift, DIPs, as a render transform (0 = none): a shadow or a lip under its stroke.</param>
    /// <param name="dx">A rightward shift, DIPs (0 = none).</param>
    /// <returns>The Path.</returns>
    /// <exception cref="winrt::hresult_error">When XAML refuses to build an element.</exception>
    inline winrt::Windows::UI::Xaml::Shapes::Path AgentLeatherStroke(const winrt::Windows::UI::Xaml::Media::Geometry& geometry,
                                                                     uint32_t color,
                                                                     double thickness,
                                                                     double dy = 0.0,
                                                                     double dx = 0.0)
    {
        using namespace winrt::Windows::UI::Xaml;
        Shapes::Path path{};
        path.Data(geometry);
        path.Stroke(Media::SolidColorBrush{ AgentLeatherColor(color) });
        path.StrokeThickness(thickness);
        path.Width(::Agentmaster::kSidePanelCardWidth);
        path.Height(::Agentmaster::kSidePanelCardHeight);
        path.HorizontalAlignment(HorizontalAlignment::Left);
        path.VerticalAlignment(VerticalAlignment::Top);
        path.IsHitTestVisible(false);
        if (dx != 0.0 || dy != 0.0)
        {
            // A render transform, not a margin: layout rounding snaps margins to whole pixels, and these shifts are
            // half a DIP (the shadow and the lip ARE the half-pixel offset).
            Media::TranslateTransform shift{};
            shift.X(dx);
            shift.Y(dy);
            path.RenderTransform(shift);
        }
        return path;
    }

    /// <summary>
    /// The grain as a brush: FillSidePanelLeatherGrain's bytes in a WriteableBitmap the card's size in device pixels.
    /// </summary>
    /// <param name="spec">The leather.</param>
    /// <param name="rasterScale">Device pixels per DIP where the card is drawn (XamlRoot.RasterizationScale). A value
    /// that is not above 0 (unknown yet) is taken as 1; the bitmap is then 12 × 40 and the next skin with the real scale
    /// replaces it.</param>
    /// <returns>An ImageBrush stretched over whatever it paints; null for a smooth leather (no grain) or a size the
    /// generator refuses.</returns>
    /// <exception cref="winrt::hresult_error">When XAML refuses to build the bitmap or the brush.</exception>
    /// <exception cref="std::bad_alloc">When the bytes cannot be allocated.</exception>
    inline winrt::Windows::UI::Xaml::Media::ImageBrush AgentLeatherGrainBrush(const ::Agentmaster::SidePanelLeatherSpec& spec, double rasterScale)
    {
        using namespace winrt::Windows::UI::Xaml::Media;
        const double scale = rasterScale > 0.0 ? rasterScale : 1.0;
        const int widthPx = (std::max)(1, static_cast<int>(std::lround(::Agentmaster::kSidePanelCardWidth * scale)));
        const int heightPx = (std::max)(1, static_cast<int>(std::lround(::Agentmaster::kSidePanelCardHeight * scale)));
        const auto bytes = ::Agentmaster::FillSidePanelLeatherGrain(spec, widthPx, heightPx);
        if (bytes.empty() || !(spec.grainAmp > 0.0))
        {
            return nullptr;
        }
        Imaging::WriteableBitmap bitmap{ widthPx, heightPx };
        // A WriteableBitmap's buffer is exactly width × height × 4 bytes of premultiplied BGRA, rows top to bottom with no
        // padding (TermControl's scroll-bar marks write it the same way) — the generator's layout, byte for byte.
        std::memcpy(bitmap.PixelBuffer().data(), bytes.data(), bytes.size());
        bitmap.Invalidate();
        ImageBrush brush{};
        brush.ImageSource(bitmap);
        brush.Stretch(Stretch::Fill);
        return brush;
    }

    /// <summary>
    /// Build a leather's FACE: everything on the card above its body, in the card's frame (12 × 40 DIPs, docked left).
    /// Put it as the content of a host whose background is the body (AgentInstallLeatherBodyBrushes, or the preview's
    /// Border), with no padding and no border.
    /// </summary>
    /// <param name="spec">The leather.</param>
    /// <param name="rasterScale">Device pixels per DIP where the face is drawn (for the grain bitmap; see
    /// AgentLeatherGrainBrush).</param>
    /// <returns>A Grid of exactly the card's size, not hit-testable, not mirrored (AgentSetLeatherFaceMirrored does a
    /// right dock).</returns>
    /// <exception cref="winrt::hresult_error">When XAML refuses to build an element.</exception>
    /// <exception cref="std::bad_alloc">When the grain's bytes cannot be allocated.</exception>
    inline winrt::Windows::UI::Xaml::Controls::Grid AgentBuildLeatherCardFace(const ::Agentmaster::SidePanelLeatherSpec& spec, double rasterScale)
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Media;
        Controls::Grid face{};
        face.Width(::Agentmaster::kSidePanelCardWidth);
        face.Height(::Agentmaster::kSidePanelCardHeight);
        face.IsHitTestVisible(false); // the host's own background takes the pointer: a press anywhere is the button's
        face.RenderTransformOrigin({ 0.5f, 0.5f }); // the mirror flips the face about its own middle
        const auto radius = AgentLeatherCardRadius(spec, /*dockLeft*/ true); // the face's frame is always docked left
        const auto children = face.Children();

        // The gloss: white at `gloss` on the top edge, nothing from `glossEnd` of the height down.
        if (spec.gloss > 0.0)
        {
            LinearGradientBrush sheen{};
            sheen.StartPoint({ 0.5f, 0.0f });
            sheen.EndPoint({ 0.5f, 1.0f });
            GradientStop from{};
            from.Offset(0.0);
            from.Color(winrt::Windows::UI::Color{ static_cast<uint8_t>(std::lround(spec.gloss * 255.0)), 0xFF, 0xFF, 0xFF });
            GradientStop to{};
            to.Offset(spec.glossEnd);
            to.Color(winrt::Windows::UI::Color{ 0x00, 0xFF, 0xFF, 0xFF });
            sheen.GradientStops().Append(from);
            sheen.GradientStops().Append(to);
            Controls::Border gloss{};
            gloss.CornerRadius(radius);
            gloss.Background(sheen);
            children.Append(gloss);
        }

        // The grain: one value per device pixel, clipped to the card's rounded outline by the Border's radius.
        if (const auto grainBrush = AgentLeatherGrainBrush(spec, rasterScale))
        {
            Controls::Border grain{};
            grain.CornerRadius(radius);
            grain.Background(grainBrush);
            children.Append(grain);
        }

        // The rim: the burnished edge inside the outline.
        {
            Controls::Border rim{};
            rim.CornerRadius(radius);
            rim.BorderThickness(ThicknessHelper::FromUniformLength(spec.edgeWidth));
            rim.BorderBrush(SolidColorBrush{ AgentLeatherColor(spec.edge) });
            children.Append(rim);
        }

        // The stitches: the shadow (the thread's holes) first, the thread over it — both dashed along one path, flat
        // ends (the dash IS the visible stitch). XAML counts dash lengths in stroke thicknesses, the mock in DIPs.
        {
            const auto path = ::Agentmaster::SidePanelStitchPathFor(spec);
            const double t = spec.stitchThickness > 0.0 ? spec.stitchThickness : 1.0;
            const auto dashed = [&](uint32_t color, double dy, double dx) {
                auto stroke = AgentLeatherStroke(AgentLeatherStitchGeometry(path), color, t, dy, dx);
                DoubleCollection dashes{};
                dashes.Append(spec.stitchDash / t);
                dashes.Append(spec.stitchGap / t);
                stroke.StrokeDashArray(dashes);
                stroke.StrokeDashCap(PenLineCap::Flat);
                stroke.StrokeStartLineCap(PenLineCap::Flat);
                stroke.StrokeEndLineCap(PenLineCap::Flat);
                stroke.StrokeLineJoin(PenLineJoin::Round);
                return stroke;
            };
            if ((spec.stitchShadow >> 24) != 0)
            {
                children.Append(dashed(spec.stitchShadow, spec.stitchShadowDy, spec.stitchShadowDx));
            }
            children.Append(dashed(spec.stitchColor, 0.0, 0.0));
        }

        // The chevron, debossed: the light lip shifted down, then the dark stamp. It points away from the docked edge (the
        // way the panel opens); the mirror turns it for a right dock.
        {
            const auto chevron = [&]() {
                PathFigure figure{};
                figure.StartPoint({ static_cast<float>(spec.chevronCx - spec.chevronHalfWidth), static_cast<float>(spec.chevronCy - spec.chevronHalfHeight) });
                figure.IsClosed(false);
                figure.IsFilled(false);
                LineSegment tip{};
                tip.Point({ static_cast<float>(spec.chevronCx + spec.chevronHalfWidth), static_cast<float>(spec.chevronCy) });
                LineSegment tail{};
                tail.Point({ static_cast<float>(spec.chevronCx - spec.chevronHalfWidth), static_cast<float>(spec.chevronCy + spec.chevronHalfHeight) });
                figure.Segments().Append(tip);
                figure.Segments().Append(tail);
                PathGeometry geometry{};
                geometry.Figures().Append(figure);
                return geometry;
            };
            const auto rounded = [&](Shapes::Path stroke) {
                stroke.StrokeStartLineCap(PenLineCap::Round);
                stroke.StrokeEndLineCap(PenLineCap::Round);
                stroke.StrokeLineJoin(PenLineJoin::Round);
                return stroke;
            };
            if ((spec.chevronLip >> 24) != 0)
            {
                children.Append(rounded(AgentLeatherStroke(chevron(), spec.chevronLip, spec.chevronWidth, spec.chevronLipDy)));
            }
            children.Append(rounded(AgentLeatherStroke(chevron(), spec.chevronColor, spec.chevronWidth)));
        }
        return face;
    }

    /// <summary>Mirror a face for a right dock (the chevron then points left, the square side is on the right), or put it
    /// back for a left dock. Writes only when the mirror changes.</summary>
    /// <param name="face">A face from AgentBuildLeatherCardFace; null changes nothing.</param>
    /// <param name="mirrored">True for a right dock.</param>
    /// <exception cref="winrt::hresult_error">When XAML refuses the write.</exception>
    inline void AgentSetLeatherFaceMirrored(const winrt::Windows::UI::Xaml::Controls::Grid& face, bool mirrored)
    {
        using namespace winrt::Windows::UI::Xaml::Media;
        if (!face)
        {
            return;
        }
        const auto current = face.RenderTransform().try_as<ScaleTransform>();
        const bool isMirrored = current && current.ScaleX() < 0.0;
        if (isMirrored == mirrored)
        {
            return;
        }
        if (mirrored)
        {
            ScaleTransform flip{};
            flip.ScaleX(-1.0);
            face.RenderTransform(flip);
        }
        else
        {
            face.RenderTransform(nullptr);
        }
    }

    /// <summary>
    /// A still picture of a leather card for a settings page: the body (rest tone) and the face, docked left, scaled by
    /// <c>zoom</c> with a Viewbox so its strokes stay sharp (the grain is written for the zoomed size too).
    /// </summary>
    /// <param name="spec">The leather.</param>
    /// <param name="zoom">How much larger than the real card (1 = true size; the cog uses 2).</param>
    /// <param name="rasterScale">Device pixels per DIP of the page (for the grain; not above 0 = 1).</param>
    /// <returns>A Viewbox of (12 × zoom) × (40 × zoom) DIPs, not hit-testable.</returns>
    /// <exception cref="winrt::hresult_error">When XAML refuses to build an element.</exception>
    /// <exception cref="std::bad_alloc">When the grain's bytes cannot be allocated.</exception>
    inline winrt::Windows::UI::Xaml::Controls::Viewbox AgentBuildLeatherCardPreview(const ::Agentmaster::SidePanelLeatherSpec& spec, double zoom, double rasterScale)
    {
        using namespace winrt::Windows::UI::Xaml;
        const double z = zoom > 0.0 ? zoom : 1.0;
        Controls::Border body{};
        body.Width(::Agentmaster::kSidePanelCardWidth);
        body.Height(::Agentmaster::kSidePanelCardHeight);
        body.CornerRadius(AgentLeatherCardRadius(spec, /*dockLeft*/ true));
        body.Background(AgentMakeLeatherBodyBrush(spec, ::Agentmaster::SidePanelCardState::Rest));
        body.Child(AgentBuildLeatherCardFace(spec, (rasterScale > 0.0 ? rasterScale : 1.0) * z));
        Controls::Viewbox box{};
        box.Width(::Agentmaster::kSidePanelCardWidth * z);
        box.Height(::Agentmaster::kSidePanelCardHeight * z);
        box.Stretch(Media::Stretch::Uniform);
        box.IsHitTestVisible(false);
        box.Child(body);
        return box;
    }
}
