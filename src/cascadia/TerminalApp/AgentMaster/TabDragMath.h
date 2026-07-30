// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — pure decision math for the POINTER-OWNED tab reorder/tear-out gesture
// (TerminalPage::_WireTabReorderGesture and friends, TerminalPage.AgentEngine.cpp).
//
// Native MUX tab drag is permanently DISABLED (CanReorderTabs/CanDragTabs false): MUX's
// TabView::FindTabViewItemFromDragItem null-derefs on the first virtualized-out container on BOTH
// the drag-start AND the drop path, and every make-the-lookup-succeed candidate is disproven —
// see the CLAUDE.md "MUX TabView drag-start null-deref" gotcha (2026-07-30 verdicts). The
// replacement gesture lives entirely at our layer: pointer events + the existing _TryMoveTab.
// This header is the gesture's BRAIN — pure, header-only, no WinRT (the PromptAnchor.h idiom) —
// so the engine test harness covers the decisions with no XAML.
//
// Coordinate convention: everything is in TabView-local DIPs. A "band" is a realized, on-strip
// tab header's horizontal interval [left, right) with its TabItems index; the caller builds the
// band list from the realized containers only (ContainerFromIndex(i) != null), ascending index,
// filtered to the visible strip — virtualized-out tabs simply have no band, which is the whole
// point: nothing here ever needs an unrealized container.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace Agentmaster
{
    struct TabDragBand
    {
        int index{ 0 }; // the tab's CURRENT TabItems index
        double left{ 0.0 }; // band start, TabView coords (DIPs)
        double right{ 0.0 }; // band end
    };

    // The press->drag arming threshold: has the pointer moved far enough from the press point to
    // count as a drag (vs a click's jitter)? Either axis alone qualifies — a straight-down pull
    // toward tear-out must arm just like a horizontal reorder pull.
    inline bool TabDragThresholdCrossed(double dx, double dy, double thresholdDips) noexcept
    {
        return std::abs(dx) >= thresholdDips || std::abs(dy) >= thresholdDips;
    }

    // The INSERTION SLOT for the pointer: s in [0, itemCount] meaning "insert before the item
    // currently at index s" (s == itemCount => append after the last item). Decided by the classic
    // midpoint rule over the realized bands: the first band whose midpoint lies right of the
    // pointer is the insertion target; past every band's midpoint => after the last band. A pointer
    // left of the first visible band inserts before it (with edge auto-scroll sliding the window,
    // farther targets become reachable). Returns -1 when there is nothing to decide against
    // (no bands / no items) — the caller keeps its previous slot.
    inline int DecideTabInsertionSlot(const std::vector<TabDragBand>& bands, double pointerX, int itemCount) noexcept
    {
        if (bands.empty() || itemCount <= 0)
        {
            return -1;
        }
        for (const auto& b : bands)
        {
            const double mid = (b.left + b.right) / 2.0;
            if (pointerX < mid)
            {
                return std::clamp(b.index, 0, itemCount);
            }
        }
        return std::clamp(bands.back().index + 1, 0, itemCount);
    }

    // The x coordinate of the insertion CARET for a slot: the left edge of the band the slot
    // inserts before, or the right edge of the last band for an append. False when no bands (no
    // caret to draw). A slot below every band (possible after the manager-floor clamp when only
    // higher-index bands are realized) snaps to the first band's left edge.
    inline bool InsertionSlotBoundaryX(const std::vector<TabDragBand>& bands, int slot, double& outX) noexcept
    {
        if (bands.empty())
        {
            return false;
        }
        for (const auto& b : bands)
        {
            if (slot <= b.index)
            {
                outX = b.left;
                return true;
            }
        }
        outX = bands.back().right;
        return true;
    }

    // Convert an insertion slot into a _TryMoveTab TARGET index for the item currently at
    // fromIndex: removing the item first shifts everything after it left by one, so a slot past
    // the item lands at slot-1; a slot at/before it lands at slot. Clamped to [minIndex,
    // itemCount-1] — minIndex is the pinned floor (1 while the Manager tab holds index 0), the
    // same clamp _TryMoveTab applies (kept here so the caret and the commit can never disagree).
    inline int InsertionSlotToMoveTarget(int slot, int fromIndex, int minIndex, int itemCount) noexcept
    {
        if (itemCount <= 0)
        {
            return minIndex;
        }
        const int target = (slot > fromIndex) ? slot - 1 : slot;
        return std::clamp(target, minIndex, itemCount - 1);
    }

    enum class TabDragRelease
    {
        Reorder, // released within the strip band (± slack) => commit the reorder
        TearOut, // released clear of the strip => tear out into a new window (the native TabDroppedOutside semantic)
    };

    // Classify a release point against the strip rect [0,0,stripW,stripH] (TabView coords) with a
    // grace band of slackDips on every side: inside-or-near => Reorder, clear of it => TearOut.
    // Mirrors native behavior, where any drop not on a TabView (the content area below, outside
    // the window, …) tore the tab out into a new window.
    inline TabDragRelease ClassifyTabDragRelease(double x, double y, double stripW, double stripH, double slackDips) noexcept
    {
        if (y < -slackDips || y > stripH + slackDips)
        {
            return TabDragRelease::TearOut;
        }
        if (x < -slackDips || x > stripW + slackDips)
        {
            return TabDragRelease::TearOut;
        }
        return TabDragRelease::Reorder;
    }

    // Edge auto-scroll: the signed scroll step for a pointer at pointerX over a viewport of
    // viewportWidth — negative (scroll left) within edgeBandDips of the left edge, positive within
    // edgeBandDips of the right edge, 0 elsewhere or on a degenerate viewport. A viewport narrower
    // than the two bands combined scrolls toward the nearer edge (left wins the exact center).
    inline double TabDragAutoScrollStep(double pointerX, double viewportWidth, double edgeBandDips, double stepDips) noexcept
    {
        if (viewportWidth <= 0.0 || stepDips <= 0.0)
        {
            return 0.0;
        }
        if (viewportWidth < edgeBandDips * 2.0)
        {
            return (pointerX <= viewportWidth / 2.0) ? -stepDips : stepDips;
        }
        if (pointerX < edgeBandDips)
        {
            return -stepDips;
        }
        if (pointerX > viewportWidth - edgeBandDips)
        {
            return stepDips;
        }
        return 0.0;
    }
}
