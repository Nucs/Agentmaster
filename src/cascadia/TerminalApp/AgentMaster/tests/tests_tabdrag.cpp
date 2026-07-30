// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: TabDragMath.h — the pure brain of the pointer-owned
// tab reorder/tear-out gesture (the native-MUX-drag replacement; CLAUDE.md "MUX TabView
// drag-start null-deref" gotcha). Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#include "../TabDragMath.h"

using Agentmaster::TabDragBand;

void TestTabDragMath()
{
    std::wprintf(L"TabDragMath (pointer-owned tab reorder gesture):\n");

    // --- threshold ---
    CHECK(!Agentmaster::TabDragThresholdCrossed(0.0, 0.0, 6.0), "threshold: no motion never arms");
    CHECK(!Agentmaster::TabDragThresholdCrossed(5.9, -5.9, 6.0), "threshold: sub-threshold jitter on both axes stays a click");
    CHECK(Agentmaster::TabDragThresholdCrossed(6.0, 0.0, 6.0), "threshold: horizontal pull arms (reorder)");
    CHECK(Agentmaster::TabDragThresholdCrossed(0.0, 6.0, 6.0), "threshold: vertical pull arms (tear-out intent)");
    CHECK(Agentmaster::TabDragThresholdCrossed(-7.0, 0.0, 6.0), "threshold: leftward motion arms (abs)");

    // --- insertion slot: the midpoint rule over realized bands ---
    // A 5-tab strip, tabs 1..3 realized+visible at [100,200) [200,300) [300,400) (0 = the pinned
    // Manager tab, virtualized out; 4 scrolled off right).
    const std::vector<TabDragBand> bands{ { 1, 100, 200 }, { 2, 200, 300 }, { 3, 300, 400 } };
    CHECK(Agentmaster::DecideTabInsertionSlot({}, 150, 5) == -1, "slot: no bands => undecided (-1)");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 150, 0) == -1, "slot: no items => undecided (-1)");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 50, 5) == 1, "slot: left of the first band => before it");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 149, 5) == 1, "slot: left of band 1's midpoint => before 1");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 151, 5) == 2, "slot: right of band 1's midpoint => before 2");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 251, 5) == 3, "slot: right of band 2's midpoint => before 3");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 351, 5) == 4, "slot: right of band 3's midpoint => after 3 (slot 4)");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 9999, 5) == 4, "slot: far right => after the last band");
    CHECK(Agentmaster::DecideTabInsertionSlot(bands, 9999, 4) == 4, "slot: append slot == itemCount is legal");
    // a hostile band index past itemCount clamps rather than yielding an out-of-range slot
    const std::vector<TabDragBand> hostile{ { 9, 0, 100 } };
    CHECK(Agentmaster::DecideTabInsertionSlot(hostile, 10, 3) == 3, "slot: band index past itemCount clamps to itemCount");

    // --- caret boundary x ---
    double x{ -1 };
    CHECK(!Agentmaster::InsertionSlotBoundaryX({}, 1, x), "caret: no bands => no caret");
    CHECK(Agentmaster::InsertionSlotBoundaryX(bands, 1, x) && x == 100, "caret: slot 1 => band 1's left edge");
    CHECK(Agentmaster::InsertionSlotBoundaryX(bands, 3, x) && x == 300, "caret: slot 3 => band 3's left edge");
    CHECK(Agentmaster::InsertionSlotBoundaryX(bands, 4, x) && x == 400, "caret: append slot => last band's right edge");
    CHECK(Agentmaster::InsertionSlotBoundaryX(bands, 0, x) && x == 100, "caret: slot below every band snaps to the first band's left (manager-floor clamp case)");

    // --- slot -> _TryMoveTab target ---
    // moving the item at index 2 in a 6-item strip with the Manager floor (minIndex 1):
    CHECK(Agentmaster::InsertionSlotToMoveTarget(5, 2, 1, 6) == 4, "target: slot past the item lands at slot-1 (remove shifts left)");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(6, 2, 1, 6) == 5, "target: append slot (==itemCount) lands at the last index");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(1, 4, 1, 6) == 1, "target: slot at/before the item is taken verbatim");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(2, 2, 1, 6) == 2, "target: the item's own slot is a no-op move (== fromIndex)");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(3, 2, 1, 6) == 2, "target: the slot right after the item is also a no-op (slot-1 == fromIndex)");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(0, 3, 1, 6) == 1, "target: the manager floor clamps slot 0 up to 1");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(0, 3, 0, 6) == 0, "target: no manager => index 0 reachable");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(99, 1, 1, 6) == 5, "target: a hostile slot clamps to the last index");
    CHECK(Agentmaster::InsertionSlotToMoveTarget(2, 1, 1, 0) == 1, "target: zero items degrades to the floor (never negative)");

    // --- release classification (strip 800x40, slack 32) ---
    using R = Agentmaster::TabDragRelease;
    CHECK(Agentmaster::ClassifyTabDragRelease(400, 20, 800, 40, 32) == R::Reorder, "release: inside the strip => reorder");
    CHECK(Agentmaster::ClassifyTabDragRelease(400, -31, 800, 40, 32) == R::Reorder, "release: just above within slack => still reorder");
    CHECK(Agentmaster::ClassifyTabDragRelease(400, 71, 800, 40, 32) == R::Reorder, "release: just below within slack => still reorder");
    CHECK(Agentmaster::ClassifyTabDragRelease(400, -33, 800, 40, 32) == R::TearOut, "release: clear above the strip => tear out");
    CHECK(Agentmaster::ClassifyTabDragRelease(400, 73, 800, 40, 32) == R::TearOut, "release: clear below (over the terminal content) => tear out");
    CHECK(Agentmaster::ClassifyTabDragRelease(-33, 20, 800, 40, 32) == R::TearOut, "release: clear left of the window => tear out");
    CHECK(Agentmaster::ClassifyTabDragRelease(834, 20, 800, 40, 32) == R::TearOut, "release: clear right of the window => tear out");
    CHECK(Agentmaster::ClassifyTabDragRelease(-31, 20, 800, 40, 32) == R::Reorder, "release: x within slack => reorder");

    // --- edge auto-scroll (viewport 800, band 36, step 28) ---
    CHECK(Agentmaster::TabDragAutoScrollStep(400, 800, 36, 28) == 0.0, "autoscroll: mid-strip => none");
    CHECK(Agentmaster::TabDragAutoScrollStep(10, 800, 36, 28) == -28.0, "autoscroll: left band => scroll left");
    CHECK(Agentmaster::TabDragAutoScrollStep(795, 800, 36, 28) == 28.0, "autoscroll: right band => scroll right");
    CHECK(Agentmaster::TabDragAutoScrollStep(36, 800, 36, 28) == 0.0, "autoscroll: exactly on the band edge => none (strict <)");
    CHECK(Agentmaster::TabDragAutoScrollStep(400, 0, 36, 28) == 0.0, "autoscroll: degenerate viewport => none");
    CHECK(Agentmaster::TabDragAutoScrollStep(-50, 800, 36, 28) == -28.0, "autoscroll: pointer left of the viewport (captured drag) => scroll left");
    CHECK(Agentmaster::TabDragAutoScrollStep(900, 800, 36, 28) == 28.0, "autoscroll: pointer right of the viewport (captured drag) => scroll right");
    CHECK(Agentmaster::TabDragAutoScrollStep(10, 60, 36, 28) == -28.0, "autoscroll: viewport narrower than both bands => nearer edge wins (left)");
    CHECK(Agentmaster::TabDragAutoScrollStep(50, 60, 36, 28) == 28.0, "autoscroll: viewport narrower than both bands => nearer edge wins (right)");

    std::wprintf(L"\n");
}
