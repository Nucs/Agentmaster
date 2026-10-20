// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SIDE_PANEL.md section 2f - the side panel's card list, VIRTUALIZED): which rows of a long
// list of equal-height rows get their real elements, and how tall the two stand-ins for all the others are.
//
// WHY: a side panel in GLOBAL scope lists every live session of the process - ~706 cards in production's two
// GLOBAL windows on 2026-10-20 (UI_THREAD.md section 13c). The list was a plain StackPanel, so every card
// existed, on screen or not: ~13,500 measured TextBlocks (~224 MB of DirectWrite state) and every card's
// elements in each layout pass. Only the cards near the viewport need to exist.
//
// HOW: every row is taken to be `stride` DIPs tall - the side-panel cards are (one-line texts of fixed sizes,
// fixed paddings), and the caller measures the stride from the cards it realized (MedianRowHeight). Row i then
// spans [i * stride, (i + 1) * stride). The list keeps ONE contiguous RANGE of rows realized; every row above
// it is folded into a top spacer of first * stride, every row below into a bottom spacer of
// (count - last) * stride. So the content height is count * stride whatever is realized, and realizing other
// rows never changes the scroll extent - a scroll in flight (DirectManipulation inertia, a thumb drag) is not
// disturbed by the list changing under it.
//
// THE POLICY (PlanRealizedRows) has two margins around the viewport, so a re-realization is not paid on every
// wheel notch:
//   NEAR - the viewport widened by max(half a viewport, 4 rows) on each side: the rows that must exist now.
//          While the realized range covers them, nothing changes.
//   FAR  - the viewport widened by max(one viewport, 8 rows) on each side: the range realized when NEAR is no
//          longer covered. The next re-realization is then about half a viewport of scrolling away.
// A realized range much larger than FAR (a viewport that shrank) is trimmed back to FAR.
// A viewport that has no height yet (the panel's first update runs before layout) counts as
// kVirtualRowsFallbackViewportDip tall.
//
// Pure, header-only, pure-ASCII source (the KeyedListDiff.h idiom): no Windows, no WinRT, so the m5 harness
// tests it without a build of the app. Every std::min / std::max is written (std::min)(...): the harness includes
// <windows.h> without NOMINMAX, whose min / max macros would otherwise eat the calls (C2589).
// Unit-tested: tests_sidepanel.cpp (TestVirtualListWindow).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Agentmaster
{
    // A half-open range of row indexes [first, last). `last <= first` is empty; the functions below always
    // return ranges with first <= last <= count.
    struct RowRange
    {
        size_t first{ 0 };
        size_t last{ 0 }; // one past the last row of the range

        // The number of rows in the range (0 for an empty range). Never throws.
        constexpr size_t Size() const noexcept
        {
            return last > first ? last - first : 0;
        }

        // True when the range holds no row. Never throws.
        constexpr bool Empty() const noexcept
        {
            return last <= first;
        }

        // True when `row` lies inside the range. Never throws.
        constexpr bool Contains(size_t row) const noexcept
        {
            return row >= first && row < last;
        }

        // True when every row of `inner` lies inside this range. An EMPTY inner is covered by any range (there
        // is nothing that must exist), an empty outer covers only an empty inner. Never throws.
        constexpr bool Covers(const RowRange& inner) const noexcept
        {
            return inner.Empty() || (!Empty() && inner.first >= first && inner.last <= last);
        }

        constexpr bool operator==(const RowRange& other) const noexcept = default;
    };

    // The viewport height used while the list has none yet (DIPs): the panel's first update runs before its
    // first layout pass, and a guess of a tall viewport realizes enough rows for whatever the real one is.
    inline constexpr double kVirtualRowsFallbackViewportDip = 720.0;
    // The NEAR margin: max(this many viewports, kVirtualRowsNearMinRows rows) on each side of the viewport.
    inline constexpr double kVirtualRowsNearViewports = 0.5;
    inline constexpr double kVirtualRowsNearMinRows = 4.0;
    // The FAR margin: max(this many viewports, kVirtualRowsFarMinRows rows) on each side - twice NEAR, so a
    // fresh range leaves half a viewport of scrolling before the next re-realization.
    inline constexpr double kVirtualRowsFarViewports = 1.0;
    inline constexpr double kVirtualRowsFarMinRows = 8.0;
    // A realized range more than this many times the FAR range's size is trimmed back to FAR (a viewport that
    // shrank must not keep the rows of the old, bigger one alive forever).
    inline constexpr size_t kVirtualRowsTrimFactor = 2;
    // Two row heights closer than this (DIPs) are the same height: a re-measure that moves the stride by less
    // never re-lays the spacers (layout rounding makes measured heights differ in the last digits).
    inline constexpr double kVirtualRowsStrideEpsilonDip = 0.05;

    namespace detail
    {
        // A usable stride: finite and > 0, else 1 (every function stays defined for a caller's bad value).
        //   stride  - the caller's row height (DIPs).
        //   returns - the stride to compute with. Never throws.
        inline double SaneStride(double stride) noexcept
        {
            return (std::isfinite(stride) && stride > 0.0) ? stride : 1.0;
        }

        // A usable viewport height: finite and > 0, else kVirtualRowsFallbackViewportDip.
        //   height  - the viewport height (DIPs).
        //   returns - the height to compute with. Never throws.
        inline double SaneViewportHeight(double height) noexcept
        {
            return (std::isfinite(height) && height > 0.0) ? height : kVirtualRowsFallbackViewportDip;
        }

        // A row index from a DIP offset, rounded by `roundUp` (ceil) or down (floor), clamped to [0, count].
        //   offset  - DIPs from the top of the content (may be negative or past the end).
        //   stride  - a sane row height.
        //   count   - the number of rows.
        //   roundUp - ceil instead of floor.
        //   returns - the clamped index. Never throws (a non-finite offset reads as 0).
        inline size_t RowAt(double offset, double stride, size_t count, bool roundUp) noexcept
        {
            if (!std::isfinite(offset) || offset <= 0.0)
            {
                return 0;
            }
            const double rows = offset / stride;
            const double rounded = roundUp ? std::ceil(rows) : std::floor(rows);
            if (rounded >= static_cast<double>(count))
            {
                return count;
            }
            return static_cast<size_t>(rounded);
        }
    }

    // The rows that have any part inside the content span [top, bottom), clamped to the list.
    //   count  - the number of rows.
    //   stride - the row height (DIPs; a non-positive or non-finite value counts as 1).
    //   top    - the span's top, DIPs from the top of the content (may be negative).
    //   bottom - the span's bottom (exclusive); a span with bottom <= top holds no row.
    //   returns - the range; empty when the span misses every row. Never throws.
    inline RowRange RowsInSpan(size_t count, double stride, double top, double bottom) noexcept
    {
        const double s = detail::SaneStride(stride);
        if (count == 0 || !std::isfinite(top) || !std::isfinite(bottom) || bottom <= top)
        {
            return {};
        }
        const size_t first = detail::RowAt(top, s, count, /*roundUp*/ false);
        const size_t last = detail::RowAt(bottom, s, count, /*roundUp*/ true);
        return RowRange{ first, (std::max)(first, last) };
    }

    // The scroll offset clamped to what the content allows: [0, max(0, count * stride - viewHeight)]. A list
    // that just shrank (a filter typed, LOCAL picked) can still report the old, larger offset until the
    // ScrollViewer's next layout clamps it; a plan made from the clamped value already shows the right rows.
    //   count      - the number of rows.
    //   stride     - the row height (DIPs).
    //   viewTop    - the ScrollViewer's vertical offset (DIPs).
    //   viewHeight - the viewport height (DIPs; 0 / non-finite = the fallback height).
    //   returns    - the clamped offset. Never throws (a non-finite offset reads as 0).
    inline double ClampViewTop(size_t count, double stride, double viewTop, double viewHeight) noexcept
    {
        const double s = detail::SaneStride(stride);
        const double h = detail::SaneViewportHeight(viewHeight);
        const double content = static_cast<double>(count) * s;
        const double maxTop = (std::max)(0.0, content - h);
        if (!std::isfinite(viewTop) || viewTop <= 0.0)
        {
            return 0.0;
        }
        return (std::min)(viewTop, maxTop);
    }

    // The rows of the viewport widened by `viewports` viewport heights or `minRows` rows (whichever is larger)
    // on each side. The building block of the NEAR and FAR ranges.
    //   count, stride        - the list.
    //   viewTop, viewHeight  - the viewport (clamped / defaulted like ClampViewTop does).
    //   viewports, minRows   - the margin on each side.
    //   returns              - the range. Never throws.
    inline RowRange WidenedViewportRows(size_t count, double stride, double viewTop, double viewHeight, double viewports, double minRows) noexcept
    {
        const double s = detail::SaneStride(stride);
        const double h = detail::SaneViewportHeight(viewHeight);
        const double top = ClampViewTop(count, s, viewTop, h);
        const double margin = (std::max)(viewports * h, minRows * s);
        return RowsInSpan(count, s, top - margin, top + h + margin);
    }

    // Decide which rows the list realizes for this viewport (the policy in the header comment): keep `current`
    // while it covers the NEAR rows and is not much larger than the FAR range; otherwise the FAR range.
    //   count      - the number of rows (the current list; `current` is clamped to it first).
    //   stride     - the row height (DIPs).
    //   viewTop    - the ScrollViewer's vertical offset (DIPs).
    //   viewHeight - the viewport height (DIPs; 0 / non-finite = kVirtualRowsFallbackViewportDip).
    //   current    - the rows realized now.
    //   returns    - the rows to realize: `current` (clamped) when nothing has to change. Never throws.
    inline RowRange PlanRealizedRows(size_t count, double stride, double viewTop, double viewHeight, RowRange current) noexcept
    {
        if (count == 0)
        {
            return {};
        }
        const RowRange clamped{ (std::min)(current.first, count), (std::min)(current.last, count) };
        const RowRange nearRows = WidenedViewportRows(count, stride, viewTop, viewHeight, kVirtualRowsNearViewports, kVirtualRowsNearMinRows);
        const RowRange farRows = WidenedViewportRows(count, stride, viewTop, viewHeight, kVirtualRowsFarViewports, kVirtualRowsFarMinRows);
        // With count > 0 the NEAR rows are never empty (the clamped viewport always overlaps the content), so an
        // empty `current` never covers them: a list's first plan always realizes the FAR range.
        const bool coversNear = clamped.Covers(nearRows);
        const bool tooLarge = clamped.Size() > kVirtualRowsTrimFactor * (std::max<size_t>)(farRows.Size(), 1);
        if (coversNear && !tooLarge)
        {
            return clamped;
        }
        return farRows;
    }

    // The rows to realize so that `row` can be scrolled into view with the minimum scroll (the side panel's
    // "scroll to the selected tab's card"): wherever the minimum scroll leaves the viewport - the row at its top
    // edge or at its bottom edge - the realized range covers it, widened by the FAR margin, so the scroll that
    // follows needs no second re-realization.
    //   count      - the number of rows.
    //   stride     - the row height (DIPs).
    //   row        - the row to reveal; a row >= count gives an empty range.
    //   viewHeight - the viewport height (DIPs; 0 / non-finite = the fallback height).
    //   returns    - the range; it always contains `row` when row < count. Never throws.
    inline RowRange RowsAroundRow(size_t count, double stride, size_t row, double viewHeight) noexcept
    {
        if (row >= count)
        {
            return {};
        }
        const double s = detail::SaneStride(stride);
        const double h = detail::SaneViewportHeight(viewHeight);
        const double rowTop = static_cast<double>(row) * s;
        // The union of the two viewports a minimum scroll can end in: the row at the bottom edge
        // [rowTop + s - h, rowTop + s) and the row at the top edge [rowTop, rowTop + h).
        const double spanTop = (std::min)(rowTop + s - h, rowTop);
        const double spanBottom = (std::max)(rowTop + h, rowTop + s);
        const double margin = (std::max)(kVirtualRowsFarViewports * h, kVirtualRowsFarMinRows * s);
        RowRange range = RowsInSpan(count, s, spanTop - margin, spanBottom + margin);
        // Belt: floating-point rounding must never leave the asked-for row outside its own range.
        range.first = (std::min)(range.first, row);
        range.last = (std::max)(range.last, row + 1);
        return range;
    }

    // The heights of the two stand-ins for the rows outside `realized`.
    struct SpacerHeights
    {
        double top{ 0.0 }; // the rows above the realized range: first * stride
        double bottom{ 0.0 }; // the rows below it: (count - last) * stride
    };

    // The spacer heights for a realized range.
    //   count    - the number of rows.
    //   stride   - the row height (DIPs).
    //   realized - the realized rows (clamped to count first).
    //   returns  - the two heights, each >= 0. Never throws.
    inline SpacerHeights SpacersFor(size_t count, double stride, RowRange realized) noexcept
    {
        const double s = detail::SaneStride(stride);
        const size_t first = (std::min)(realized.first, count);
        const size_t last = (std::max)(first, (std::min)(realized.last, count));
        return SpacerHeights{ static_cast<double>(first) * s, static_cast<double>(count - last) * s };
    }

    // The row height to compute with: the MEDIAN of the measured heights, so one taller card (a title line whose
    // emoji came from a fallback font with a taller line box) cannot move the stride - only a change of most
    // cards can (a DPI or text-size change). Non-finite and non-positive values (a card not laid out yet) are
    // ignored.
    //   heights  - the measured row heights (DIPs), by value: the function sorts its own copy.
    //   fallback - the height to return when no measurement is usable (the caller's previous stride).
    //   returns  - the median (the upper one of the two middle values for an even count), or `fallback`.
    //   throws   - std::bad_alloc only (the copy).
    inline double MedianRowHeight(std::vector<double> heights, double fallback)
    {
        heights.erase(std::remove_if(heights.begin(), heights.end(), [](double h) { return !std::isfinite(h) || h <= 0.0; }), heights.end());
        if (heights.empty())
        {
            return fallback;
        }
        const auto middle = heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2);
        std::nth_element(heights.begin(), middle, heights.end());
        return *middle;
    }

    // True when two row heights differ by more than kVirtualRowsStrideEpsilonDip (a real change, not rounding
    // noise). Never throws.
    inline bool StrideChanged(double a, double b) noexcept
    {
        return std::fabs(a - b) > kVirtualRowsStrideEpsilonDip;
    }
}
