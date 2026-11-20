// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the collapsed side panel's LEATHER CARD: what it looks like and where it sits (SIDE_PANEL.md §2q).
//
// The user's request of 2026-11-20: "The sidemenu collapse, it should not be a whole strip but a card you can click and
// it looks like a leather tailored with stickes card … it should be a 12x40 height, from 80px top of the terminal".
// Their picks from eight rendered designs: design 1, "Cognac & Cream", AND design 7, "Charcoal Pebble & Silver",
// "Switchable in settings" (AppSettings::sidePanelCardLeather; Cognac by default). The card keeps the strip's
// auto-hide (SidePanelStripReveal.h), with their reach of 25 DIPs.
//
// WHY ONE HEADER: four places must draw and store the same card — the side panel's collapsed card
// (TerminalPage.AgentSidePanel.cpp), the cog's live preview (AgentManagerContent.Settings.cpp), the settings file
// (Persistence.cpp: the token) and the m5 harness (TestSidePanelLeatherCard). The XAML builder that turns this data into
// elements is the UI layer's AgentLeatherCard.h; this header is the data and the arithmetic alone, so the harness pins
// every number and the grain bit for bit.
//
// HOW THE CARD IS LAYERED (bottom to top; every length in DIPs, the card docked LEFT — a right dock mirrors the face):
//   1. the body: a vertical gradient, lighter while the pointer is over the card, darker while it is pressed
//      (SidePanelLeatherTone) — the Button's own background, so the button's states recolor it;
//   2. the gloss: a white sheen fading out from the top edge;
//   3. the grain: value noise defined in DIP space and written once per DEVICE pixel (FillSidePanelLeatherGrain), so a
//      pebble is the same size at every display scale;
//   4. the rim: the burnished edge, a 1 DIP border inside the card's outline;
//   5. the stitches: the thread's shadow (its holes), then the thread, dashed (SidePanelStitchPathFor);
//   6. the chevron: debossed — a light lip under a dark stamp — pointing away from the docked edge (the way the panel
//      opens).
// The HTML mocks the user chose from drew exactly these layers with exactly these numbers (the scratchpad's leather.js
// of session 097399a4); the grain's hash is the mock's, constant for constant.
//
// Plain C++, header-only (the engine idiom of SidePanelStripReveal.h): no WinRT, no XAML, no <windows.h>. ⚠ No
// identifier here is named `near`, `far` or `small`: all three are macros under <windows.h> (CLAUDE.md, Gotchas), and
// the app's translation units include this header after it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    /// <summary>The card's width, DIPs: the user's "12". The auto-hide's reach is measured from its inner edge.</summary>
    inline constexpr double kSidePanelCardWidth = 12.0;

    /// <summary>The card's height, DIPs: the user's "40".</summary>
    inline constexpr double kSidePanelCardHeight = 40.0;

    /// <summary>
    /// How far below the top of the content area the card's top edge sits, DIPs: the user's "from 80px top of the
    /// terminal". The content area is what is under the tab row (the terminal, or the Agent Manager page), so the card
    /// keeps its place whether the tab row is shown, auto-hidden or floating over it.
    /// </summary>
    inline constexpr double kSidePanelCardTop = 80.0;

    /// <summary>
    /// Which leather the collapsed card wears. GLOBAL (one value for the profile), stored as settings.json's
    /// <c>settings.sidePanelCardLeather</c>; set from the cog (Settings → Tabs &amp; Overlay → SIDE PANEL).
    /// </summary>
    /// <remarks>
    /// The integer values are the cog's ComboBox item order (AgentManagerContent.Settings.cpp seeds and reads the box by
    /// index), so a new leather goes at the END; the file never stores the integer, only the token.
    /// </remarks>
    enum class SidePanelCardLeather
    {
        /// <summary>The default (and every settings.json written before the setting existed): cognac saddle leather,
        /// cream thread all round, a debossed chevron. Warm, and clearly visible on a dark terminal.</summary>
        Cognac = 0,
        /// <summary>Pebbled charcoal (Agentmaster's own grey), silver thread on three sides as if the leather wrapped
        /// around the window's edge, a debossed chevron. Blends with the app's dark surfaces.</summary>
        Charcoal = 1,
    };

    /// <summary>The settings.json member (inside the engine's <c>settings</c> object) that holds the leather.</summary>
    inline constexpr std::wstring_view kSidePanelCardLeatherKey = L"sidePanelCardLeather";

    /// <summary>The word settings.json stores for a leather. Use it for every write, so a leather is never spelled two
    /// ways.</summary>
    /// <param name="leather">The leather.</param>
    /// <returns><c>"cognac"</c> or <c>"charcoal"</c>; an out-of-range value (a cast integer) answers <c>"cognac"</c>, the
    /// default. The view points at a string literal, valid for the whole run.</returns>
    /// <remarks>Never throws (<c>constexpr</c>, <c>noexcept</c>).</remarks>
    constexpr std::wstring_view SidePanelCardLeatherToken(SidePanelCardLeather leather) noexcept
    {
        switch (leather)
        {
        case SidePanelCardLeather::Charcoal:
            return L"charcoal";
        case SidePanelCardLeather::Cognac:
        default:
            return L"cognac";
        }
    }

    /// <summary>The leather a settings.json word stands for: the inverse of SidePanelCardLeatherToken. Use it for every
    /// read.</summary>
    /// <param name="token">The stored word (case-sensitive, as written).</param>
    /// <returns>The leather; a missing, empty or unknown word (a hand edit, a newer build's leather) reads as the default,
    /// SidePanelCardLeather::Cognac — a card always draws.</returns>
    /// <remarks>Never throws (<c>constexpr</c>, <c>noexcept</c>).</remarks>
    constexpr SidePanelCardLeather SidePanelCardLeatherFromToken(std::wstring_view token) noexcept
    {
        return token == L"charcoal" ? SidePanelCardLeather::Charcoal : SidePanelCardLeather::Cognac;
    }

    /// <summary>The pointer's effect on the card, as its button reports it: the body's tone follows.</summary>
    enum class SidePanelCardState
    {
        /// <summary>No pointer on it: the leather's own colors.</summary>
        Rest = 0,
        /// <summary>The pointer is over it: 13% lighter (toward white), so the card answers the approach.</summary>
        Hover = 1,
        /// <summary>A press is held on it: 20% darker (toward black), the leather pushed in.</summary>
        Pressed = 2,
    };

    /// <summary>How the thread runs around the card.</summary>
    enum class SidePanelStitchLayout
    {
        /// <summary>All round, a closed loop inset from every edge (square corners on the docked side).</summary>
        Closed = 0,
        /// <summary>Top, free side and bottom only, running off the docked edge at both ends: the leather reads as
        /// wrapped around the window's edge.</summary>
        ThreeSided = 1,
    };

    /// <summary>
    /// Everything that draws one leather. Colors are 0xAARRGGBB; lengths are DIPs in the card's own frame (docked left:
    /// x = 0 is the docked edge, x = 12 the free side). Values come only from SidePanelLeatherSpecFor.
    /// </summary>
    struct SidePanelLeatherSpec
    {
        /// <summary>The body gradient's color at the top edge (at rest).</summary>
        uint32_t bodyTop{ 0 };
        /// <summary>The body gradient's color at the bottom edge (at rest).</summary>
        uint32_t bodyBottom{ 0 };
        /// <summary>The gloss's white opacity at the top edge, 0..1 (0 = no gloss).</summary>
        double gloss{ 0.0 };
        /// <summary>Where the gloss has faded to nothing, as a fraction of the card's height (0..1).</summary>
        double glossEnd{ 0.0 };
        /// <summary>How strongly the grain lightens and darkens the leather (0 = smooth). A grain value is multiplied by
        /// this and clamped to ±1: +t blends white in at opacity t, −t blends black in at opacity t.</summary>
        double grainAmp{ 0.0 };
        /// <summary>The grain's coarse cell, DIPs: the size of one pebble. The fine octave is 0.45 of it.</summary>
        double grainCell{ 1.0 };
        /// <summary>The grain's seed: each leather has its own pattern.</summary>
        int32_t grainSeed{ 0 };
        /// <summary>The burnished rim's color (opaque).</summary>
        uint32_t edge{ 0 };
        /// <summary>The rim's width, DIPs (inside the outline).</summary>
        double edgeWidth{ 1.0 };
        /// <summary>The free side's corner radius, DIPs. The docked side's corners are square: the card is a tab sewn to
        /// the edge.</summary>
        double radius{ 0.0 };
        /// <summary>How the thread runs (all round, or three sides).</summary>
        SidePanelStitchLayout stitchLayout{ SidePanelStitchLayout::Closed };
        /// <summary>The thread's centerline distance from the card's edges, DIPs.</summary>
        double stitchInset{ 0.0 };
        /// <summary>The thread's thickness, DIPs.</summary>
        double stitchThickness{ 1.0 };
        /// <summary>The thread's color (opaque).</summary>
        uint32_t stitchColor{ 0 };
        /// <summary>One stitch's length along the path, DIPs (flat ends: the dash IS the visible stitch).</summary>
        double stitchDash{ 1.0 };
        /// <summary>The bare leather between two stitches, DIPs.</summary>
        double stitchGap{ 1.0 };
        /// <summary>The thread's shadow (its holes) — the same path, offset, drawn first; its alpha is its strength.</summary>
        uint32_t stitchShadow{ 0 };
        /// <summary>The shadow's offset to the right (toward the free side when docked left), DIPs.</summary>
        double stitchShadowDx{ 0.0 };
        /// <summary>The shadow's offset downward, DIPs.</summary>
        double stitchShadowDy{ 0.0 };
        /// <summary>The chevron stamp's color (opaque).</summary>
        uint32_t chevronColor{ 0 };
        /// <summary>The stamp's light lip, drawn under it shifted down by chevronLipDy; its alpha is its strength.</summary>
        uint32_t chevronLip{ 0 };
        /// <summary>The chevron's stroke width, DIPs (round caps and joins).</summary>
        double chevronWidth{ 1.0 };
        /// <summary>The chevron's tip-to-tail midpoint, x, DIPs.</summary>
        double chevronCx{ 6.1 };
        /// <summary>The chevron's middle, y, DIPs.</summary>
        double chevronCy{ 20.0 };
        /// <summary>Half the chevron's width, DIPs: the tip is at cx + this, the tails at cx − this.</summary>
        double chevronHalfWidth{ 1.35 };
        /// <summary>Half the chevron's height, DIPs.</summary>
        double chevronHalfHeight{ 3.1 };
        /// <summary>How far down the lip is shifted, DIPs (a debossed stamp catches the light on its lower edge).</summary>
        double chevronLipDy{ 0.65 };
    };

    /// <summary>The numbers of one leather — exactly those of the design the user picked from.</summary>
    /// <param name="leather">The leather; an out-of-range value draws as Cognac.</param>
    /// <returns>The spec. Never throws (<c>constexpr</c>, <c>noexcept</c>).</returns>
    constexpr SidePanelLeatherSpec SidePanelLeatherSpecFor(SidePanelCardLeather leather) noexcept
    {
        SidePanelLeatherSpec s{};
        if (leather == SidePanelCardLeather::Charcoal)
        {
            // Design 7, "Charcoal Pebble & Silver".
            s.bodyTop = 0xFF4D4E54;
            s.bodyBottom = 0xFF2A2B2F;
            s.gloss = 0.06;
            s.glossEnd = 0.45;
            s.grainAmp = 0.16;
            s.grainCell = 1.45;
            s.grainSeed = 79;
            s.edge = 0xFF151517;
            s.edgeWidth = 1.0;
            s.radius = 4.0;
            s.stitchLayout = SidePanelStitchLayout::ThreeSided;
            s.stitchInset = 2.5;
            s.stitchThickness = 0.9;
            s.stitchColor = 0xFFCDD1D9;
            s.stitchDash = 1.3;
            s.stitchGap = 1.2;
            s.stitchShadow = 0xA6000000; // black at 0.65
            s.stitchShadowDx = 0.4;
            s.stitchShadowDy = 0.5;
            s.chevronColor = 0xFF1B1C1F;
            s.chevronLip = 0x997B7D84; // #7B7D84 at 0.6
            s.chevronWidth = 1.15;
            return s;
        }
        // Design 1, "Cognac & Cream" (also every unknown value).
        s.bodyTop = 0xFFB8743D;
        s.bodyBottom = 0xFF8A4A20;
        s.gloss = 0.12;
        s.glossEnd = 0.5;
        s.grainAmp = 0.09;
        s.grainCell = 2.1;
        s.grainSeed = 11;
        s.edge = 0xFF4A240C;
        s.edgeWidth = 1.0;
        s.radius = 4.0;
        s.stitchLayout = SidePanelStitchLayout::Closed;
        s.stitchInset = 2.5;
        s.stitchThickness = 1.0;
        s.stitchColor = 0xFFF3E4C2;
        s.stitchDash = 1.7;
        s.stitchGap = 1.3;
        s.stitchShadow = 0x992B1205; // #2B1205 at 0.6
        s.stitchShadowDx = 0.45;
        s.stitchShadowDy = 0.5;
        s.chevronColor = 0xFF5A2B0F;
        s.chevronLip = 0x8CE2A06A; // #E2A06A at 0.55
        s.chevronWidth = 1.15;
        return s;
    }

    /// <summary>
    /// A body color in the pointer's state: hover blends 13% of white in, a press 20% of black (the mock's numbers). The
    /// alpha is kept. Each channel is rounded to the nearest integer.
    /// </summary>
    /// <param name="argb">The rest color, 0xAARRGGBB.</param>
    /// <param name="state">The pointer's state; Rest returns the color unchanged.</param>
    /// <returns>The toned color, 0xAARRGGBB. Never throws.</returns>
    constexpr uint32_t SidePanelLeatherTone(uint32_t argb, SidePanelCardState state) noexcept
    {
        if (state == SidePanelCardState::Rest)
        {
            return argb;
        }
        const double t = state == SidePanelCardState::Hover ? 0.13 : 0.20;
        const double target = state == SidePanelCardState::Hover ? 255.0 : 0.0;
        uint32_t out = argb & 0xFF000000u;
        for (int shift = 0; shift <= 16; shift += 8)
        {
            const double c = static_cast<double>((argb >> shift) & 0xFFu);
            const double v = c + (target - c) * t;
            // Round half up; v is within [0, 255] by construction (a blend of two values in that range).
            const auto byte = static_cast<uint32_t>(v + 0.5);
            out |= (byte > 255u ? 255u : byte) << shift;
        }
        return out;
    }

    /// <summary>One step of a stitch path: a straight line, or a quarter-circle corner, to (x, y).</summary>
    struct SidePanelStitchStep
    {
        /// <summary>True for a clockwise circular arc of <c>radius</c> (a rounded corner); false for a straight line.</summary>
        bool arc{ false };
        /// <summary>The step's end point, x, DIPs.</summary>
        double x{ 0.0 };
        /// <summary>The step's end point, y, DIPs.</summary>
        double y{ 0.0 };
        /// <summary>The arc's radius, DIPs (unused for a line).</summary>
        double radius{ 0.0 };
    };

    /// <summary>
    /// The thread's path in the card's own frame (docked left), as the mock built it: a start point, then lines and
    /// clockwise quarter arcs. The dashes start at the start point, so the path's direction is part of the look.
    /// </summary>
    struct SidePanelStitchPath
    {
        /// <summary>Where the thread starts, x, DIPs.</summary>
        double startX{ 0.0 };
        /// <summary>Where the thread starts, y, DIPs.</summary>
        double startY{ 0.0 };
        /// <summary>True when the path closes back to its start (a loop); false for an open run.</summary>
        bool closed{ false };
        /// <summary>The steps, in order; only the first <c>count</c> are used.</summary>
        std::array<SidePanelStitchStep, 6> steps{};
        /// <summary>How many of <c>steps</c> the path uses.</summary>
        size_t count{ 0 };
    };

    /// <summary>
    /// The thread's path for a leather: inset <c>stitchInset</c> from the edges, its free-side corners rounded with
    /// radius max(radius − inset + 0.6, 0.8) (the mock's rule: the thread's turn is a little rounder than the inset alone
    /// would leave it), square on the docked side.
    /// </summary>
    /// <param name="spec">The leather.</param>
    /// <returns>The path. A Closed loop starts at the top-left inset corner and runs clockwise (top, free side, bottom,
    /// docked side); a ThreeSided run starts at the docked edge (x = 0) on the top line and ends at the docked edge on the
    /// bottom line. Never throws.</returns>
    constexpr SidePanelStitchPath SidePanelStitchPathFor(const SidePanelLeatherSpec& spec) noexcept
    {
        constexpr double w = kSidePanelCardWidth;
        constexpr double h = kSidePanelCardHeight;
        const double m = spec.stitchInset;
        // Parenthesized: a translation unit that includes <windows.h> without NOMINMAX has a `max` macro.
        const double ri = (std::max)(spec.radius - m + 0.6, 0.8);
        SidePanelStitchPath p{};
        p.closed = spec.stitchLayout == SidePanelStitchLayout::Closed;
        p.startX = p.closed ? m : 0.0;
        p.startY = m;
        size_t n = 0;
        p.steps[n++] = { false, w - m - ri, m, 0.0 }; // the top line, toward the free side
        p.steps[n++] = { true, w - m, m + ri, ri }; // the free side's top corner
        p.steps[n++] = { false, w - m, h - m - ri, 0.0 }; // down the free side
        p.steps[n++] = { true, w - m - ri, h - m, ri }; // the free side's bottom corner
        // The bottom line: back to the docked side's inset corner (a loop, which then closes up the docked side), or off
        // the docked edge (three sides).
        p.steps[n++] = { false, p.closed ? m : 0.0, h - m, 0.0 };
        p.count = n;
        return p;
    }

    /// <summary>The grain's 32-bit hash of a lattice point: the mock's constants, so the app's pattern is the mock's.</summary>
    /// <param name="x">The lattice column (non-negative in use).</param>
    /// <param name="y">The lattice row (non-negative in use).</param>
    /// <param name="seed">The pattern's seed.</param>
    /// <returns>A well-mixed 32-bit value. Never throws (unsigned arithmetic wraps by definition).</returns>
    constexpr uint32_t SidePanelLeatherHash(int32_t x, int32_t y, int32_t seed) noexcept
    {
        uint32_t h = (static_cast<uint32_t>(x) * 0x8DA6B343u) ^ (static_cast<uint32_t>(y) * 0xD8163841u) ^ (static_cast<uint32_t>(seed) * 0xCB1AB31Fu);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        return h;
    }

    /// <summary>Smooth value noise in [0, 1): the four lattice hashes around (x, y), blended with a smoothstep.</summary>
    /// <param name="x">The point, in lattice units (non-negative in use).</param>
    /// <param name="y">The point, in lattice units (non-negative in use).</param>
    /// <param name="seed">The pattern's seed.</param>
    /// <returns>The noise value. Never throws.</returns>
    inline double SidePanelLeatherValueNoise(double x, double y, int32_t seed) noexcept
    {
        const double fx = std::floor(x);
        const double fy = std::floor(y);
        const auto xi = static_cast<int32_t>(fx);
        const auto yi = static_cast<int32_t>(fy);
        const double xf = x - fx;
        const double yf = y - fy;
        const double u = xf * xf * (3.0 - 2.0 * xf);
        const double v = yf * yf * (3.0 - 2.0 * yf);
        constexpr double kUnit = 4294967296.0; // 2^32: maps a hash to [0, 1)
        const double a = SidePanelLeatherHash(xi, yi, seed) / kUnit;
        const double b = SidePanelLeatherHash(xi + 1, yi, seed) / kUnit;
        const double c = SidePanelLeatherHash(xi, yi + 1, seed) / kUnit;
        const double d = SidePanelLeatherHash(xi + 1, yi + 1, seed) / kUnit;
        return (a * (1.0 - u) + b * u) * (1.0 - v) + (c * (1.0 - u) + d * u) * v;
    }

    /// <summary>
    /// The grain at a point of the card, before the leather's strength: two octaves of value noise (the pebble and its
    /// texture), centered on 0 and stretched to about ±1.
    /// </summary>
    /// <param name="xDip">The point, x, DIPs in the card's frame.</param>
    /// <param name="yDip">The point, y, DIPs in the card's frame.</param>
    /// <param name="cell">The pebble's size, DIPs (above 0).</param>
    /// <param name="seed">The pattern's seed.</param>
    /// <returns>The grain, roughly in [−1.2, 1.2]. Never throws.</returns>
    inline double SidePanelLeatherGrainAt(double xDip, double yDip, double cell, int32_t seed) noexcept
    {
        const double n1 = SidePanelLeatherValueNoise(xDip / cell, yDip / cell, seed);
        const double n2 = SidePanelLeatherValueNoise(xDip / (cell * 0.45), yDip / (cell * 0.45), seed + 977);
        return (n1 * 0.62 + n2 * 0.38 - 0.5) * 2.4;
    }

    /// <summary>
    /// The grain's pixels for a card drawn at <c>widthPx</c> × <c>heightPx</c> device pixels: one value per pixel, taken
    /// at the pixel's center in DIPs (so a pebble is the same size at every display scale), as premultiplied BGRA — white
    /// at opacity t where the grain lightens, black at opacity t where it darkens. Laid over the body with ordinary
    /// alpha blending, that is exactly the mock's "lighten toward white by t / darken by t".
    /// </summary>
    /// <param name="spec">The leather (its grainAmp, grainCell and grainSeed).</param>
    /// <param name="widthPx">The bitmap's width in pixels (the card's 12 DIPs at the display's scale); at least 1.</param>
    /// <param name="heightPx">The bitmap's height in pixels; at least 1.</param>
    /// <returns>widthPx × heightPx × 4 bytes, rows top to bottom, each pixel B, G, R, A. An empty vector for a size
    /// below 1 or a pixel count over 4 million (a broken scale must never allocate gigabytes); all zeros (transparent)
    /// for a leather without grain.</returns>
    /// <exception cref="std::bad_alloc">When the bytes cannot be allocated.</exception>
    /// <remarks>
    /// The pixel→DIP mapping is per axis (widthPx / 12, heightPx / 40), so a rounded pixel size still spans the whole
    /// card. Deterministic: the same spec and size give the same bytes on every machine.
    /// </remarks>
    inline std::vector<uint8_t> FillSidePanelLeatherGrain(const SidePanelLeatherSpec& spec, int widthPx, int heightPx)
    {
        std::vector<uint8_t> bgra;
        if (widthPx < 1 || heightPx < 1 || static_cast<int64_t>(widthPx) * heightPx > 4'000'000)
        {
            return bgra;
        }
        bgra.assign(static_cast<size_t>(widthPx) * static_cast<size_t>(heightPx) * 4u, 0u);
        if (!(spec.grainAmp > 0.0) || !(spec.grainCell > 0.0))
        {
            return bgra; // a smooth leather: a transparent overlay
        }
        const double sx = static_cast<double>(widthPx) / kSidePanelCardWidth;
        const double sy = static_cast<double>(heightPx) / kSidePanelCardHeight;
        for (int y = 0; y < heightPx; ++y)
        {
            for (int x = 0; x < widthPx; ++x)
            {
                const double xd = (x + 0.5) / sx;
                const double yd = (y + 0.5) / sy;
                const double t = std::clamp(SidePanelLeatherGrainAt(xd, yd, spec.grainCell, spec.grainSeed) * spec.grainAmp, -1.0, 1.0);
                const auto alpha = static_cast<uint8_t>(std::lround(std::abs(t) * 255.0));
                const auto light = static_cast<uint8_t>(t > 0.0 ? alpha : 0u); // premultiplied: white at alpha a is (a, a, a, a)
                uint8_t* const px = bgra.data() + (static_cast<size_t>(y) * static_cast<size_t>(widthPx) + static_cast<size_t>(x)) * 4u;
                px[0] = light;
                px[1] = light;
                px[2] = light;
                px[3] = alpha;
            }
        }
        return bgra;
    }
}
