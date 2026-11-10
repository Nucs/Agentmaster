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
// The same header places a SUBMENU card beside its parent card (PlaceSubmenuCard: the filter panel's "By Folder..." row
// opens one on hover - SIDE_PANEL.md section 2l), the way a desktop menu does: beside the parent's right edge, else its
// left edge, its first line level with the hovered row, clamped inside the window like the pointer card.
//
// Pure, header-only, pure-ASCII source (the KeyedListDiff.h idiom): no Windows, no WinRT, so the m5 harness tests it
// without a build of the app. Unit-tested: tests_sidepanel.cpp (TestPointerCardPlacement, TestSubmenuCardPlacement).

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

    // Where a SUBMENU card goes (SIDE_PANEL.md section 2l - the hover submenu of the filter panel's "By Folder..." row):
    // its top-left corner in the area's coordinates and the side it took.
    struct SubmenuCardPlacement
    {
        double left{ 0.0 }; // the card's left edge (a popup HorizontalOffset)
        double top{ 0.0 }; // the card's top edge (a popup VerticalOffset)
        bool onRight{ true }; // true: the card stands beside the parent's RIGHT edge; false: beside its left edge
    };

    // Place a submenu card of `cardWidth` x `cardHeight` BESIDE its parent card, the way a desktop menu opens a submenu:
    //   - Horizontally: beside the parent's right edge when the whole card fits there (inside the margin), else beside
    //     its left edge when it fits there; when neither side fits, on the side with more room, clamped into the area
    //     (it may then cover part of the parent - a window narrower than both cards has no better place). Right first,
    //     the reading order, so a panel near the window's left edge opens its submenu rightward.
    //   - Vertically: its top at `top` (the caller passes the hovered row's top less the card's own chrome, so the first
    //     line of the submenu sits level with the row), clamped into the area; a card taller than the area is pinned to
    //     the top margin, so its first lines stay reachable.
    //   - An area that is not measured yet (0, negative or not finite) is not clamped: right side, top as given.
    //   - Every non-finite input reads as 0, a negative size / gap / margin as 0, and a parent whose right edge lies left
    //     of its left edge as a parent of no width - no NaN ever reaches a XAML offset.
    //   parentLeft, parentRight - the parent card's left and right edges.
    //   top                     - where the card's top edge should be.
    //   cardWidth, cardHeight   - the submenu card's size (its DesiredSize).
    //   areaWidth, areaHeight   - the window's size (0 / not finite = not measured yet: no clamp).
    //   gap                     - the distance between the two cards (default kPointerCardGapDips).
    //   margin                  - the distance the clamp keeps from each edge (default kPointerCardMarginDips).
    //   returns                 - the card's top-left corner and the side it took. Never throws.
    inline SubmenuCardPlacement PlaceSubmenuCard(double parentLeft,
                                                 double parentRight,
                                                 double top,
                                                 double cardWidth,
                                                 double cardHeight,
                                                 double areaWidth,
                                                 double areaHeight,
                                                 double gap = kPointerCardGapDips,
                                                 double margin = kPointerCardMarginDips) noexcept
    {
        const double pl = detail::PointerCardFinite(parentLeft);
        const double prRaw = detail::PointerCardFinite(parentRight);
        const double pr = prRaw < pl ? pl : prRaw;
        const double t = detail::PointerCardFinite(top);
        const double w = detail::PointerCardNonNegative(cardWidth);
        const double h = detail::PointerCardNonNegative(cardHeight);
        const double aw = detail::PointerCardNonNegative(areaWidth);
        const double ah = detail::PointerCardNonNegative(areaHeight);
        const double g = detail::PointerCardNonNegative(gap);
        const double m = detail::PointerCardNonNegative(margin);

        SubmenuCardPlacement p;
        const double rightStart = pr + g;
        const double leftStart = pl - g - w;
        if (aw <= 0.0)
        {
            // Unmeasured: no edge to fit against, so the reading order decides (the caller re-places once it has a size).
            p.onRight = true;
            p.left = rightStart;
        }
        else
        {
            // A side "holds" the card when the WHOLE card lies inside [margin, area - margin] there - both edges checked, so
            // a parent that itself reaches past a window edge (a window shrinking under an open panel) never yields a card
            // placed off screen without the clamp below.
            const bool fitsRight = rightStart >= m && rightStart + w <= aw - m;
            const bool fitsLeft = leftStart >= m && leftStart + w <= aw - m;
            if (fitsRight)
            {
                p.onRight = true;
                p.left = rightStart;
            }
            else if (fitsLeft)
            {
                p.onRight = false;
                p.left = leftStart;
            }
            else
            {
                // Neither side holds the whole card: the side with more room (ties go right), clamped into the area.
                const double roomRight = (aw - m) - rightStart;
                const double roomLeft = (pl - g) - m;
                p.onRight = roomRight >= roomLeft;
                const double start = p.onRight ? rightStart : leftStart;
                const double lo = m;
                const double hi = aw - m - w;
                p.left = hi < lo ? lo : (start < lo ? lo : (start > hi ? hi : start));
            }
        }
        if (ah <= 0.0)
        {
            p.top = t;
        }
        else
        {
            const double lo = m;
            const double hi = ah - m - h;
            p.top = hi < lo ? lo : (t < lo ? lo : (t > hi ? hi : t));
        }
        return p;
    }
}
