// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — WHEN the collapsed side panel's strip shows (SIDE_PANEL.md §2m).
//
// A collapsed side panel leaves one way back: a 20 DIP strip at the docked edge with the expand chevron in its middle.
// The user's request of 2026-11-12: "The strip should autohide. If mouse comes close by 40pixels the strips appears."
// Their pick, the same day, of what comes back (three live mocks): "The whole strip, floating" — "and button inside".
// So the strip takes no column any more. It FLOATS over the docked edge of the content (the terminal never changes
// size), hidden, and shows while the pointer is near it.
//
// This header is the rule alone, as two pure functions the m5 harness pins (TestSidePanelStripReveal,
// tests_sidepanel.cpp); TerminalPage applies it (TerminalPage.AgentSidePanel.cpp: _SidePanelStripOnPointer,
// _SidePanelStripWatchTick, _SidePanelStripApply):
//   - SidePanelStripPointerNear: is the pointer within the reveal distance of the strip's place?
//   - NextSidePanelStripReveal: the strip's next state for one pointer event.
//
// The rule in words:
//   - The strip shows when the pointer ARRIVES near it: a move, with no button held, by a device that can hover (a
//     mouse, a pen in range). It stays while the pointer stays near, and hides when the pointer is away.
//   - A drag that passes by never brings it in: a text selection dragged to the terminal's first column must not get a
//     strip over the letters it is selecting. The strip then stays away until the pointer has left and come back.
//   - A press beside the button belongs to what is under the strip (the strip's band takes no input: the terminal's
//     first columns, or its scroll bar docked right, get the press). The strip steps aside at that press, and stays
//     away until the pointer has left and come back, so working at the terminal's edge is one click from undisturbed.
//   - A press on the button is the button's: the strip stays under it and its click expands the panel.
//   - A finger cannot hover: its tap near the edge is the approach, and a tap beside the button sends the strip away.
//     Nothing else hides a strip a tap brought in: the watch (below) follows the mouse cursor, which a finger leaves
//     where it was.
//   - The pointer can leave the window without one more event reaching the content (the strip's edge is usually the
//     window's edge), so while the strip shows, or waits for the pointer to leave, a slow watch reads the cursor and
//     applies the "away" half of the rule. It never shows the strip.
//   - Right after a collapse the strip lingers for a moment wherever the pointer is, so the user sees where the panel
//     went; a press anywhere but the button ends that at once.
//
// Plain C++, header-only (the engine idiom of SidePanelPins.h / StatusIconMotion.h): no WinRT, no XAML, no
// <windows.h>. ⚠ No identifier here is named `near` or `far`: both are macros under <windows.h> (CLAUDE.md, Gotchas
// "small is a macro"), and the app's translation units include this header after it.

#pragma once

#include <cstdint>

namespace Agentmaster
{
    // How close the pointer must come to the strip for it to show, in DIPs: the user's number ("close by 40pixels").
    // Measured from the strip's own inner edge, so the strip shows while the pointer is within
    // (the strip's width + this) of the docked edge of the content: 60 DIPs for the 20 DIP strip.
    inline constexpr double kSidePanelStripRevealDistance = 40.0;

    // How long the strip stays after a collapse wherever the pointer is, in ms. Long enough to see where the panel
    // went, short enough not to read as "the strip does not hide". Tradeoff: for this long the strip covers the first
    // 20 DIPs of the content even though the pointer is elsewhere; a press anywhere but its button ends it at once.
    inline constexpr uint64_t kSidePanelStripCollapseLingerMs = 1500;

    // Is the pointer near the strip's place: inside the content area, and within `revealDistance` of the strip?
    // The strip's place is the `stripWidth` band along the docked edge, over the area's whole height, so only the
    // horizontal distance counts: any height along that edge is near.
    // ⚠ A pointer OUTSIDE the area is never near — over the tab row above it, or past the window's edge. The edge the
    // strip sits on is usually the window's own edge, and a pointer that left the window through it must hide the
    // strip, not keep it.
    //   dockLeft       - true = the panel docks left (the strip is at the area's left edge), false = right.
    //   areaWidth      - the content area's width, DIPs (ContentArea().ActualWidth()).
    //   areaHeight     - the content area's height, DIPs.
    //   x, y           - the pointer, relative to the area's top-left corner, DIPs. Half-open like pixels: [0, width)
    //                    and [0, height) are inside.
    //   stripWidth     - the strip's width, DIPs (a negative or NaN width counts as 0).
    //   revealDistance - how near the pointer must come to the strip, DIPs (a negative or NaN distance counts as 0:
    //                    then only a pointer over the strip's own place is near).
    //   returns        - true when near. False for an unmeasured area (a width or height that is not above 0) and for
    //                    any NaN coordinate: with nothing to measure against, the strip stays as the caller's rule
    //                    makes it for an "away" pointer (hidden).
    //   throws         - nothing.
    [[nodiscard]] constexpr bool SidePanelStripPointerNear(bool dockLeft,
                                                           double areaWidth,
                                                           double areaHeight,
                                                           double x,
                                                           double y,
                                                           double stripWidth,
                                                           double revealDistance = kSidePanelStripRevealDistance) noexcept
    {
        // Every test is written so that a NaN operand fails it (a comparison with NaN is false).
        if (!(areaWidth > 0.0) || !(areaHeight > 0.0))
        {
            return false; // not laid out yet: no edge to be near
        }
        if (!(x >= 0.0) || !(x < areaWidth) || !(y >= 0.0) || !(y < areaHeight))
        {
            return false; // outside the content area (or NaN)
        }
        const double reach = (stripWidth > 0.0 ? stripWidth : 0.0) + (revealDistance > 0.0 ? revealDistance : 0.0);
        // The distance to the docked edge. Never negative here: x is inside [0, areaWidth).
        const double toEdge = dockLeft ? x : areaWidth - x;
        return toEdge <= reach;
    }

    // What the strip is doing.
    struct SidePanelStripReveal
    {
        bool shown{ false }; // the strip is on screen (and its button takes input)
        // The strip stepped aside for a press or a drag at the edge: it stays hidden while the pointer stays near, and
        // is armed again once the pointer was seen away. Never true together with `shown`.
        bool dismissed{ false };

        constexpr bool operator==(const SidePanelStripReveal&) const noexcept = default;
    };

    // Which pointer event is being judged.
    enum class SidePanelStripPointerKind : uint8_t
    {
        Move, // the pointer moved over the content area
        Press, // a button, the pen or a finger went down over the content area
        // The watch's look at the cursor (no event reached the area: the pointer may have left the window). It only
        // ever hides the strip or arms it again; it never shows it.
        Watch,
    };

    // One pointer event, as the rule reads it. Sampled by the caller on the UI thread.
    struct SidePanelStripPointer
    {
        SidePanelStripPointerKind kind{ SidePanelStripPointerKind::Move };
        bool nearStrip{ false }; // SidePanelStripPointerNear for this event's position
        bool inContact{ false }; // a mouse button is held, or the pen or finger touches (Pointer.IsInContact)
        // The device can move without contact: a mouse or a pen. False = touch. For the Watch kind: the device of the
        // LAST pointer event the area saw (the watch reads the mouse cursor, which a finger does not move).
        bool hovers{ true };
        bool onButton{ false }; // (Press) the press landed on the strip's expand button
        bool lingering{ false }; // the after-collapse linger is running (kSidePanelStripCollapseLingerMs)
    };

    // The strip's next state for one pointer event (the file header states the rule in words).
    //   now     - the strip's state before the event.
    //   pointer - the event.
    //   returns - the state after it. `shown` and `dismissed` are never both true.
    //   throws  - nothing.
    [[nodiscard]] constexpr SidePanelStripReveal NextSidePanelStripReveal(SidePanelStripReveal now, const SidePanelStripPointer& pointer) noexcept
    {
        using Kind = SidePanelStripPointerKind;

        // A press is judged wherever it lands: during the linger the strip shows with the pointer anywhere.
        if (pointer.kind == Kind::Press)
        {
            if (pointer.onButton)
            {
                return now; // the button's own press: hiding the strip now would take the button from under its click
            }
            if (now.shown)
            {
                // The press belongs to what is under the strip. Dismissed only when the pointer is near: a press far
                // from the strip (the linger) leaves it armed, because the pointer is away already.
                return { false, pointer.nearStrip };
            }
            if (pointer.nearStrip && !pointer.hovers)
            {
                return { true, false }; // a finger's tap near the edge is its way of coming close
            }
            // A mouse or pen press never shows the strip. It keeps a dismissal while near, and arms the strip when away.
            return { false, now.dismissed && pointer.nearStrip };
        }

        if (pointer.kind == Kind::Watch && !pointer.hovers)
        {
            // The last input was a finger. The watch reads the mouse cursor, and that is not where the finger was: a
            // strip a tap brought in would be hidden a moment later. A tap elsewhere sends it away instead.
            return now;
        }

        if (!pointer.nearStrip)
        {
            if (now.shown && pointer.lingering)
            {
                return now; // just collapsed: the strip stays for its moment wherever the pointer is
            }
            return { false, false }; // away: hidden, and armed for the next approach
        }

        // Near the strip, by a move or by the watch's look.
        if (pointer.kind == Kind::Watch || now.shown)
        {
            return now; // the watch never shows the strip; a shown strip stays while the pointer is near
        }
        if (pointer.inContact)
        {
            return { false, true }; // a drag passing by (a selection dragged to the edge): stay away until it has left
        }
        if (now.dismissed || !pointer.hovers)
        {
            return now; // stepped aside and the pointer has not left yet; or a device that cannot arrive by moving
        }
        return { true, false }; // the pointer came close
    }
}
