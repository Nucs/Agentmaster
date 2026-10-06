// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SIDE_PANEL.md section 2e - the side panel's "+" launcher card): place a card that opens AT the
// pointer, the way a tooltip does.
//
// WHAT the user asked for: the card's corner sits at the click point, and the card extends from there TOWARD THE
// CENTER of the window - so a click near the window's top-left corner opens the card down and to the right, a click
// near the top-right corner opens it down and to the left, and so on for the bottom half. The card then stays inside
// the window: an edge it would cross is clamped to a small margin.
//
// HOW:
//   - Each axis on its own. The click's half of the window decides the direction: at or left of the middle the card
//     extends rightward (its LEFT edge at the click), right of the middle it extends leftward (its RIGHT edge at the
//     click); the same with top / bottom. "At the middle" counts as the left / top half, so a click on the exact
//     center line has one fixed answer.
//   - A small GAP keeps the card's corner off the exact click pixel, so the pointer rests just outside the card (it
//     can still move onto the card freely; nothing closes on pointer-leave).
//   - Clamp into [margin, area - margin - size]. A card larger than the area (a window narrower than the card) is
//     pinned to the near margin, so its start - the agent element and the box - stays visible.
//   - An area that is not measured yet (0, negative or not finite) is not clamped at all: the caller re-places the
//     card once the window has a size.
//   - Every non-finite input reads as 0, and a negative size / gap / margin as 0, so no NaN ever reaches the
//     XAML offsets (a NaN offset is a silent layout failure).
//
// Pure, header-only, pure-ASCII source (the KeyedListDiff.h idiom): no Windows, no WinRT, so the m5 harness tests it
// without a build of the app. Unit-tested: tests_sidepanel.cpp (TestPointerCardPlacement).

#pragma once

#include <cmath>

namespace Agentmaster
{
    // Where the card goes: its top-left corner in the AREA's coordinates (the caller's popup offsets) and the
    // direction each axis took (the tests and the open log read them).
    struct PointerCardPlacement
    {
        double left{ 0.0 }; // the card's left edge (a popup HorizontalOffset)
        double top{ 0.0 }; // the card's top edge (a popup VerticalOffset)
        bool towardRight{ true }; // true: the card extends rightward from the click (the click is in the left half)
        bool towardDown{ true }; // true: the card extends downward from the click (the click is in the top half)
    };

    // The default distance between the click point and the card's corner (DIPs): enough that the pointer is not on
    // the card's first pixel, small enough that the card reads as opened by that click.
    inline constexpr double kPointerCardGapDips = 2.0;
    // The default distance the card keeps from every window edge when it is clamped (DIPs).
    inline constexpr double kPointerCardMarginDips = 8.0;

    namespace detail
    {
        // `v` when it is a finite number, else 0 (a NaN or an infinity from an unmeasured element).
        inline double PointerCardFinite(double v) noexcept
        {
            return std::isfinite(v) ? v : 0.0;
        }

        // `v` when it is finite and positive, else 0 (a size, a gap or a margin can't be negative).
        inline double PointerCardNonNegative(double v) noexcept
        {
            return (std::isfinite(v) && v > 0.0) ? v : 0.0;
        }

        // One axis: the card's start edge for a click at `click` on an axis of length `area`.
        //   click   - the click coordinate (finite).
        //   size    - the card's extent on this axis (>= 0).
        //   area    - the area's extent on this axis (> 0 = measured; anything else = no clamp).
        //   gap     - the distance from the click to the card's corner (>= 0).
        //   margin  - the distance the clamp keeps from each edge (>= 0).
        //   forward - receives true when the card extends toward the axis' far end (right / down).
        //   returns - the start edge (left / top).
        inline double PointerCardAxis(double click, double size, double area, double gap, double margin, bool& forward) noexcept
        {
            const bool measured = area > 0.0;
            // Without a measured area there is no middle: extend forward (right / down), the natural reading order.
            forward = !measured || click <= area / 2.0;
            const double start = forward ? (click + gap) : (click - gap - size);
            if (!measured)
            {
                return start;
            }
            const double lo = margin;
            const double hi = area - margin - size;
            if (hi < lo)
            {
                return lo; // the card is larger than the area: pin its start to the near margin
            }
            return start < lo ? lo : (start > hi ? hi : start);
        }
    }

    // Place a card of `cardWidth` x `cardHeight` that opens at the click (`anchorX`, `anchorY`) inside an area of
    // `areaWidth` x `areaHeight` (all in the same coordinate space - the page root's, in DIPs).
    //   anchorX, anchorY       - the click point.
    //   cardWidth, cardHeight  - the card's size (its DesiredSize / ActualSize; 0 before it was measured).
    //   areaWidth, areaHeight  - the window's size (0 / not finite = not measured yet: no clamp).
    //   gap                    - click-to-corner distance (default kPointerCardGapDips).
    //   margin                 - edge distance of the clamp (default kPointerCardMarginDips).
    //   returns                - the card's top-left corner and the two directions. Never throws.
    inline PointerCardPlacement PlacePointerCard(double anchorX,
                                                 double anchorY,
                                                 double cardWidth,
                                                 double cardHeight,
                                                 double areaWidth,
                                                 double areaHeight,
                                                 double gap = kPointerCardGapDips,
                                                 double margin = kPointerCardMarginDips) noexcept
    {
        const double x = detail::PointerCardFinite(anchorX);
        const double y = detail::PointerCardFinite(anchorY);
        const double w = detail::PointerCardNonNegative(cardWidth);
        const double h = detail::PointerCardNonNegative(cardHeight);
        const double aw = detail::PointerCardNonNegative(areaWidth);
        const double ah = detail::PointerCardNonNegative(areaHeight);
        const double g = detail::PointerCardNonNegative(gap);
        const double m = detail::PointerCardNonNegative(margin);

        PointerCardPlacement p;
        p.left = detail::PointerCardAxis(x, w, aw, g, m, p.towardRight);
        p.top = detail::PointerCardAxis(y, h, ah, g, m, p.towardDown);
        return p;
    }
}
