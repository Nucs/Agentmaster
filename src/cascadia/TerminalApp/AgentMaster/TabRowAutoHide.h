// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — WHEN the auto-hiding tab row shows (TAB_STRIP.md "Auto-hiding the tab row").
//
// The user's request of 2026-11-15: "I want to add a autohide of the top row with tabs row and home button and + and V and
// minimize, maximize, close buttons. This row needs to be autohiding and functioning similar to windows taskbar autohide. Add a
// new configuration in settings too, default false. synced to the V menu item. It should bo toggleable via the V icon".
// Their picks the same day (AskUserHtmlQuestion, live demos): the toggle is the ▾ menu button beside + (back after the
// launcher removed the old ▾); the behavior is the taskbar's — "but no 2px line stays, 0": rest 0.15 s in the top 4 pixels
// and the row slides down over the terminal in 0.15 s, 0.6 s after the pointer leaves it slides away; and both extras: a
// 1.5 s peek on a keyboard tab switch, and the row staying out while the window shows its Agent Manager page.
//
// The row is Windows Terminal's title bar (TitlebarControl: the tab row with Home, the tabs, + and ▾, the drag bar and the
// minimize / maximize / close buttons). While the setting is on it no longer takes a layout row: it FLOATS over the top of
// the content, so the terminal never resizes when it comes or goes (a resize would make every Claude Code session redraw).
// NonClientIslandWindow (the window host, WindowsTerminal.exe) owns the title bar and applies this rule; the page
// (TerminalApp) only says whether the setting is on, whether the row must stay out (the Agent Manager page), and when to peek.
//
// This header is the rule alone — pure functions over plain numbers, pinned by the m5 harness (TestTabRowAutoHide,
// tests_tabrow.cpp):
//   - TabRowEdgeBand / TabRowStayBand / TabRowTouchEdgeBand / TabRowBandContains: where the pointer has to be, in screen
//     pixels, for "at the top edge", "in the row" and "a finger's tap at the top".
//   - NextTabRowAutoHide: the row's next state for one look at the inputs (a pointer event, the watch's tick, a setting).
//   - TabRowAutoHidePeek: a peek (a keyboard tab switch, the launcher opened by its keys).
//   - TabRowAutoHideFingerPress / TabRowAutoHideMouseMoved: a finger cannot hover, so a tap stands in for the approach.
//
// The rule in words:
//   - HIDDEN, the row comes out when the pointer RESTS at the window's top edge — the top kTabRowEdgeDip of the window, its
//     one-pixel frame border included — for kTabRowRevealDelayMs, with no button held from a press in the terminal. A pointer
//     that only crosses the edge (leaving the window upward) never brings it: the rest is what tells the two apart.
//   - A drag that started in the terminal (a text selection pulled to the top row) never brings it out, and it stays away
//     until the pointer has left the edge band and come back — the side panel strip's rule (SidePanelStripReveal.h). A drag
//     that started elsewhere (a folder dragged from Explorer onto +) brings it out like a plain approach.
//   - It also comes out AT ONCE for the page's hold (the Agent Manager page is on screen), a peek, and the keyboard focus
//     moving into it (a tab rename, keyboard navigation).
//   - SHOWN, it stays while any reason holds: the pointer in the row or within kTabRowStayBelowDip below it, the pointer over
//     a popup of the window (a menu, the + card, a tooltip), the keyboard in the row or inside an open popup, a button held
//     from a press in the row (a tab drag), the page's hold, a peek's time, a finger's reveal. When the last reason ends it
//     waits kTabRowHideDelayMs, then slides away; any reason coming back meanwhile cancels the wait. A peek that was the
//     last reason is the exception: its own length is the row's time, so the row goes when the peek ends.
//   - A window that cannot show the row (minimized, hidden, focus mode, full screen without tabs) forgets everything: the row
//     is hidden at once and nothing is pending.
//
// Plain C++, header-only (the engine idiom of SidePanelStripReveal.h / RestoreWake.h): no WinRT, no XAML, no <windows.h>.
// Included by WindowsTerminal.exe's NonClientIslandWindow.cpp (which links TerminalApp.dll, not the engine lib), by the page
// for its constants, and by the m5 harness. ⚠ No identifier here is named `near`, `far`, `small`, `hyper` or `interface`:
// those are macros under <windows.h> (CLAUDE.md, Gotchas "small is a macro"), and every includer includes it first.

#pragma once

#include <cstdint>

namespace Agentmaster
{
    // The top edge band's height, in DIPs: the user's "top 4 pixels" of the live demo. Measured from the window's first
    // visible row of pixels (a restored window's one-pixel frame border, a maximized window's screen edge). Tradeoff: a
    // taller band is easier to hit on a restored window, whose top is no barrier, but it also catches a pointer working on
    // the terminal's first row; 4 DIPs stay inside the terminal's default 8-DIP top padding.
    inline constexpr double kTabRowEdgeDip = 4.0;

    // A finger's top band, in DIPs: a finger cannot rest on 4 DIPs, so a tap anywhere in the top 16 DIPs reveals the row.
    inline constexpr double kTabRowTouchEdgeDip = 16.0;

    // How far below the row the pointer still counts as "in the row", in DIPs, so a pointer that drifts just under a tab
    // while reading it does not start the hide clock.
    inline constexpr double kTabRowStayBelowDip = 8.0;

    // How long the pointer must rest in the edge band before the row comes out, ms (the taskbar-like pick).
    inline constexpr uint64_t kTabRowRevealDelayMs = 150;

    // How long the row stays after its last reason to stay ended, ms (the taskbar-like pick).
    inline constexpr uint64_t kTabRowHideDelayMs = 600;

    // The slide's length each way, ms. The window host plays it only while Windows' "Animation effects" are on.
    inline constexpr uint64_t kTabRowSlideMs = 150;

    // How long a keyboard tab switch's peek keeps the row out, ms (the user's pick, "Peek on keyboard tab switch").
    inline constexpr uint64_t kTabRowPeekMs = 1500;

    // How long a keyboard-opened launcher's reveal lasts on its own, ms. The card itself keeps the row out while it is open
    // (the keyboard is inside a popup); this only covers the moment before the card takes the keyboard.
    inline constexpr uint64_t kTabRowLauncherPeekMs = 800;

    // How often the window host looks at the cursor while the row is out, ms. The pointer can leave the row without any
    // event reaching the window (the drag bar and the caption buttons are a separate input window, and a pointer that leaves
    // the window sends nothing more), so a shown row is watched; a hidden one is driven by events and the reveal's one-shot.
    inline constexpr uint64_t kTabRowWatchMs = 100;

    // A rectangle of screen pixels, half-open like pixels: left <= x < right and top <= y < bottom are inside.
    struct TabRowScreenBand
    {
        int32_t left{ 0 };
        int32_t top{ 0 };
        int32_t right{ 0 };
        int32_t bottom{ 0 };

        constexpr bool operator==(const TabRowScreenBand&) const noexcept = default;
    };

    // DIPs to whole pixels at a scale, rounded half up, never below `minimum`.
    //   dip     - the length in DIPs; a negative or NaN length counts as 0.
    //   scale   - pixels per DIP (the window's DPI / 96); a value that is not above 0 (or NaN) counts as 1, so a window that
    //             has not reported its DPI yet still gets a usable band.
    //   minimum - the smallest result, in pixels.
    //   returns - the pixel count. Lengths beyond the int32 range saturate at 1,000,000 pixels (no screen is that tall).
    //   throws  - nothing.
    [[nodiscard]] constexpr int32_t TabRowDipToPixels(double dip, double scale, int32_t minimum) noexcept
    {
        // Every comparison is written so that a NaN operand fails it (a comparison with NaN is false).
        const double safeScale = (scale > 0.0) ? scale : 1.0;
        const double safeDip = (dip > 0.0) ? dip : 0.0;
        const double px = safeDip * safeScale + 0.5;
        int32_t whole = 0;
        if (px >= 1000000.0)
        {
            whole = 1000000; // saturate: an absurd DPI or length must not overflow the cast
        }
        else if (px > 0.0)
        {
            whole = static_cast<int32_t>(px);
        }
        return whole < minimum ? minimum : whole;
    }

    // The top edge band: the window's first kTabRowEdgeDip of visible rows, across the window's whole client width.
    //   clientLeft, clientRight - the client area's left and right edges in screen pixels (right is exclusive).
    //   clientTop               - the client area's top row in screen pixels: a restored window's frame border row (the
    //                             window host keeps it in the client area), a maximized window's work-area edge.
    //   scale                   - pixels per DIP (see TabRowDipToPixels).
    //   edgeDip                 - the band's height in DIPs; the default is the user's pick.
    //   returns                 - the band; at least one pixel tall.
    //   throws                  - nothing.
    [[nodiscard]] constexpr TabRowScreenBand TabRowEdgeBand(int32_t clientLeft, int32_t clientTop, int32_t clientRight, double scale, double edgeDip = kTabRowEdgeDip) noexcept
    {
        return { clientLeft, clientTop, clientRight, clientTop + TabRowDipToPixels(edgeDip, scale, 1) };
    }

    // A finger's top band: like TabRowEdgeBand, kTabRowTouchEdgeDip tall (see there for the parameters).
    //   returns - the band; at least one pixel tall.
    //   throws  - nothing.
    [[nodiscard]] constexpr TabRowScreenBand TabRowTouchEdgeBand(int32_t clientLeft, int32_t clientTop, int32_t clientRight, double scale) noexcept
    {
        return TabRowEdgeBand(clientLeft, clientTop, clientRight, scale, kTabRowTouchEdgeDip);
    }

    // The band where the pointer counts as "in the row": from the window's top to kTabRowStayBelowDip below the row's
    // bottom edge, across the whole client width.
    //   clientLeft, clientTop, clientRight - as for TabRowEdgeBand.
    //   rowBottom - the row's bottom edge in screen pixels (where it ends when it is fully out).
    //   scale     - pixels per DIP.
    //   returns   - the band. A rowBottom above clientTop (a row not laid out yet) still yields the stay margin below the
    //               window's top, never an inverted band.
    //   throws    - nothing.
    [[nodiscard]] constexpr TabRowScreenBand TabRowStayBand(int32_t clientLeft, int32_t clientTop, int32_t clientRight, int32_t rowBottom, double scale) noexcept
    {
        const int32_t bottomOfRow = rowBottom < clientTop ? clientTop : rowBottom;
        return { clientLeft, clientTop, clientRight, bottomOfRow + TabRowDipToPixels(kTabRowStayBelowDip, scale, 0) };
    }

    // Is the screen point inside the band (half-open)?
    //   band - the band.
    //   x, y - the point, screen pixels.
    //   returns - true when inside. An empty or inverted band contains nothing.
    //   throws  - nothing.
    [[nodiscard]] constexpr bool TabRowBandContains(const TabRowScreenBand& band, int32_t x, int32_t y) noexcept
    {
        return x >= band.left && x < band.right && y >= band.top && y < band.bottom;
    }

    // What the rule remembers between two looks. Owned by the window host; value-semantic so every transition is testable.
    struct TabRowAutoHideState
    {
        // The row is out (or sliding out). The window host's slide follows every change of this flag.
        bool shown{ false };
        // When the pointer came to rest in the edge band (GetTickCount64 ms); 0 = it is not resting there.
        uint64_t edgeSinceMs{ 0 };
        // When the shown row lost its last reason to stay; 0 = it has a reason now (or the row is hidden).
        uint64_t awaySinceMs{ 0 };
        // A peek keeps the row out until this moment; 0 = no peek.
        uint64_t peekUntilMs{ 0 };
        // A drag from the terminal reached the edge: no reveal until the pointer has left the edge band. Only meaningful
        // while hidden.
        bool dismissed{ false };
        // A finger brought the row out: the mouse cursor's place says nothing about the finger, so the cursor's "away" does
        // not hide it; a tap outside the row does (TabRowAutoHideFingerPress), and so does the next mouse move
        // (TabRowAutoHideMouseMoved hands the row back to the cursor's rule).
        bool touchShown{ false };

        constexpr bool operator==(const TabRowAutoHideState&) const noexcept = default;
    };

    // One look at the window, sampled by the window host on its UI thread.
    struct TabRowAutoHideInputs
    {
        // The moment of the look (GetTickCount64 ms). Must not go backwards between looks of one window.
        uint64_t nowMs{ 0 };
        // The window can show the row: visible, not minimized, its title bar not hidden by focus mode or by full screen
        // without tabs, and the title bar laid out. False forgets everything (the rule's last bullet).
        bool usable{ true };
        // The page keeps the row out: the window shows its Agent Manager page (the user's pick, "Stay out on the Manager page").
        bool hold{ false };
        // The cursor is over THIS window (not over another window covering it) and inside the edge band.
        bool atEdge{ false };
        // The cursor is over this window and inside the stay band, or over one of this window's open popups.
        bool inRow{ false };
        // A mouse button is down now.
        bool buttonHeld{ false };
        // The held button went down over the terminal area (a selection drag, a scroll-bar drag). Meaningful only with
        // buttonHeld.
        bool pressInContent{ false };
        // The held button went down on the row (a tab drag, a button held down). Meaningful only with buttonHeld.
        bool pressInRow{ false };
        // The keyboard focus is in the row by the keyboard, or in a text box of the row (a tab rename).
        bool keyboardInRow{ false };
        // The keyboard focus is inside an open popup of the window (a menu, the + card). Keeps a shown row; never reveals one.
        bool keyboardInPopup{ false };
    };

    // The rule's answer to one look.
    struct TabRowAutoHideStep
    {
        // The state after the look.
        TabRowAutoHideState state{};
        // The row has to come out: the window host starts the slide in. Never true together with `hide`.
        bool reveal{ false };
        // The row has to go: the window host starts the slide out. Never true together with `reveal`.
        bool hide{ false };
        // The next moment the rule changes by itself without any new input — a rest at the edge ending, the hide delay
        // ending, a peek ending (GetTickCount64 ms); 0 = nothing pending. The window host wakes at that moment.
        uint64_t wakeAtMs{ 0 };
    };

    // The row's next state for one look (the file header states the rule in words).
    //   now - the state before the look.
    //   in  - the look.
    //   returns - the state after it, whether to start a slide, and when to look again. `reveal` and `hide` are never both
    //             true; `reveal` only when `now.shown` was false, `hide` only when it was true.
    //   throws  - nothing.
    [[nodiscard]] constexpr TabRowAutoHideStep NextTabRowAutoHide(TabRowAutoHideState now, const TabRowAutoHideInputs& in) noexcept
    {
        TabRowAutoHideStep step{};
        auto s = now;

        if (!in.usable)
        {
            // A window that cannot show the row: forget everything, so the next time it can, the row starts hidden and no
            // stale rest, delay or peek fires.
            step.hide = s.shown;
            step.state = TabRowAutoHideState{};
            return step;
        }

        const bool peeking = s.peekUntilMs != 0 && in.nowMs < s.peekUntilMs;
        // The first look after a peek's end: the peek was armed in `now` and its time is up at this look.
        const bool peekJustEnded = s.peekUntilMs != 0 && !peeking;
        if (!peeking)
        {
            s.peekUntilMs = 0; // an ended peek leaves no trace
        }

        if (s.shown)
        {
            // Shown: no rest at the edge and no dismissal are tracked while the row is out.
            s.edgeSinceMs = 0;
            s.dismissed = false;
            const bool stay = in.hold || peeking || in.keyboardInRow || in.keyboardInPopup || s.touchShown || in.inRow ||
                              (in.buttonHeld && in.pressInRow);
            if (stay)
            {
                s.awaySinceMs = 0;
                // Only a peek ends by itself; every other reason ends with an input the window host reports.
                step.wakeAtMs = peeking ? s.peekUntilMs : 0;
                step.state = s;
                return step;
            }
            if (peekJustEnded && s.awaySinceMs == 0)
            {
                // The peek was the row's last reason and its time is the row's time (the user's pick: out for 1.5 s, then
                // away): the row goes at the peek's end, not a hide delay later.
                s.shown = false;
                step.hide = true;
                step.state = s;
                return step;
            }
            if (s.awaySinceMs == 0 || s.awaySinceMs > in.nowMs)
            {
                s.awaySinceMs = in.nowMs; // the hide clock starts now (a clock "in the future" is repaired, never trusted)
            }
            if (in.nowMs - s.awaySinceMs >= kTabRowHideDelayMs)
            {
                s.shown = false;
                s.awaySinceMs = 0;
                step.hide = true;
                step.state = s;
                return step;
            }
            step.wakeAtMs = s.awaySinceMs + kTabRowHideDelayMs;
            step.state = s;
            return step;
        }

        // Hidden.
        s.awaySinceMs = 0;
        s.touchShown = false;
        if (in.hold || peeking || in.keyboardInRow)
        {
            // Reasons that bring the row out at once, wherever the pointer is.
            s.shown = true;
            s.edgeSinceMs = 0;
            s.dismissed = false;
            step.reveal = true;
            step.wakeAtMs = peeking ? s.peekUntilMs : 0;
            step.state = s;
            return step;
        }
        if (!in.atEdge)
        {
            // Away from the edge: no rest, and a dismissal is lifted (the pointer has left).
            s.edgeSinceMs = 0;
            s.dismissed = false;
            step.state = s;
            return step;
        }
        if (in.buttonHeld && in.pressInContent)
        {
            // A drag from the terminal reached the edge: step aside until the pointer has left the edge band.
            s.edgeSinceMs = 0;
            s.dismissed = true;
            step.state = s;
            return step;
        }
        if (s.dismissed)
        {
            // Still at the edge since a drag dismissed it: wait for the pointer to leave. A dismissed row tracks no rest
            // (cleared here too, so no state — however it was reached — keeps a rest clock that could fire later).
            s.edgeSinceMs = 0;
            step.state = s;
            return step;
        }
        if (s.edgeSinceMs == 0 || s.edgeSinceMs > in.nowMs)
        {
            s.edgeSinceMs = in.nowMs; // the rest starts now
        }
        if (in.nowMs - s.edgeSinceMs >= kTabRowRevealDelayMs)
        {
            s.shown = true;
            s.edgeSinceMs = 0;
            step.reveal = true;
            step.state = s;
            return step;
        }
        step.wakeAtMs = s.edgeSinceMs + kTabRowRevealDelayMs;
        step.state = s;
        return step;
    }

    // A peek: bring the row out now and keep it out for `durationMs` (a keyboard tab switch, the launcher opened by its keys).
    //   now        - the state before the peek.
    //   nowMs      - the moment of the peek (GetTickCount64 ms).
    //   durationMs - how long the peek keeps the row out; an earlier peek that lasts longer keeps its end (peeks never
    //                shorten each other). 0 brings the row out without a peek of its own (the other reasons then decide).
    //   returns    - the step: `reveal` when the row was hidden, `wakeAtMs` = the peek's end (0 without one). The caller
    //                must still respect a window that cannot show the row (it applies the next look's `usable`).
    //   throws     - nothing.
    [[nodiscard]] constexpr TabRowAutoHideStep TabRowAutoHidePeek(TabRowAutoHideState now, uint64_t nowMs, uint64_t durationMs) noexcept
    {
        TabRowAutoHideStep step{};
        auto s = now;
        if (durationMs > 0)
        {
            const uint64_t until = nowMs + durationMs;
            if (s.peekUntilMs == 0 || s.peekUntilMs < until)
            {
                s.peekUntilMs = until;
            }
        }
        step.reveal = !s.shown;
        s.shown = true;
        s.edgeSinceMs = 0;
        s.awaySinceMs = 0;
        s.dismissed = false;
        step.wakeAtMs = s.peekUntilMs;
        step.state = s;
        return step;
    }

    // A finger went down somewhere in the window (a finger cannot hover: its tap stands in for the approach).
    //   now         - the state before the tap.
    //   nowMs       - the moment of the tap (GetTickCount64 ms).
    //   atTouchEdge - the tap landed in the finger's top band (TabRowTouchEdgeBand).
    //   onRow       - the tap landed on the shown row or on one of the window's open popups.
    //   returns     - the step: a hidden row tapped at the top comes out and stays until a tap elsewhere (`touchShown`);
    //                 a shown row tapped elsewhere loses the finger's hold and starts its hide clock at this moment (the
    //                 next look hides it kTabRowHideDelayMs later unless another reason holds). A tap on the row changes
    //                 nothing (it belongs to the tab or the button under it).
    //   throws      - nothing.
    [[nodiscard]] constexpr TabRowAutoHideStep TabRowAutoHideFingerPress(TabRowAutoHideState now, uint64_t nowMs, bool atTouchEdge, bool onRow) noexcept
    {
        TabRowAutoHideStep step{};
        auto s = now;
        if (!s.shown)
        {
            if (atTouchEdge)
            {
                s.shown = true;
                s.touchShown = true;
                s.edgeSinceMs = 0;
                s.awaySinceMs = 0;
                s.dismissed = false;
                step.reveal = true;
            }
            step.state = s;
            return step;
        }
        if (onRow)
        {
            step.state = s;
            return step;
        }
        s.touchShown = false;
        s.awaySinceMs = nowMs;
        step.wakeAtMs = nowMs + kTabRowHideDelayMs;
        step.state = s;
        return step;
    }

    // The mouse (or a pen) moved: the cursor speaks for the pointer again, so a finger's hold ends and the cursor's rule
    // decides from the next look on.
    //   now     - the state before the move.
    //   returns - the state with `touchShown` cleared; nothing else changes.
    //   throws  - nothing.
    [[nodiscard]] constexpr TabRowAutoHideState TabRowAutoHideMouseMoved(TabRowAutoHideState now) noexcept
    {
        now.touchShown = false;
        return now;
    }
}
