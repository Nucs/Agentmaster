// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: the AUTO-HIDING TAB ROW (AgentMaster/TabRowAutoHide.h; TAB_STRIP.md
// "Auto-hiding the tab row"; the user's request of 2026-11-15 and their picks the same day: the Windows taskbar's
// behavior with no line left behind - rest 0.15 s in the top 4 pixels and the row slides out over the terminal, 0.6 s after
// the pointer leaves it slides away - plus a 1.5 s peek on a keyboard tab switch and the row staying out on the Agent
// Manager page). Pinned here: the screen bands (the edge band, the finger's band, the stay band, DIPs to pixels at every
// scale and for every broken input), the rule's named cases (a rest, a crossing, a drag from the terminal and a drag from
// elsewhere, a tab drag, the hold, the peeks, the keyboard, the popups, a window that cannot show the row, a finger's taps,
// clocks that run backwards), an exhaustive sweep of every input combination over a grid of states against a second
// statement of the rule plus its invariants, and one user's whole visit, event by event. Shared CHECK/decls: m5_tests.h.

#include "m5_tests.h"

#include "../TabRowAutoHide.h"

#include <cmath> // std::nan - the broken-input cases of the DIP conversion
#include <cstdint>
#include <vector>

namespace
{
    using Agentmaster::TabRowAutoHideInputs;
    using Agentmaster::TabRowAutoHideState;
    using Agentmaster::TabRowAutoHideStep;

    // A look with nothing going on (the pointer elsewhere, no button, no keyboard, no hold) at `nowMs`.
    //   nowMs   - the moment of the look.
    //   returns - the inputs; every flag false except `usable`.
    TabRowAutoHideInputs Quiet(uint64_t nowMs)
    {
        TabRowAutoHideInputs in{};
        in.nowMs = nowMs;
        in.usable = true;
        return in;
    }

    // The same look with the cursor resting in the top edge band (which is also inside the row's stay band).
    //   nowMs   - the moment of the look.
    //   returns - the inputs.
    TabRowAutoHideInputs AtEdge(uint64_t nowMs)
    {
        auto in = Quiet(nowMs);
        in.atEdge = true;
        in.inRow = true; // the edge band lies inside the stay band: a shown row under the pointer stays
        return in;
    }

    // The same look with the cursor in the row (below the edge band).
    //   nowMs   - the moment of the look.
    //   returns - the inputs.
    TabRowAutoHideInputs InRow(uint64_t nowMs)
    {
        auto in = Quiet(nowMs);
        in.inRow = true;
        return in;
    }

    // The rule stated a second time, for the exhaustive cross-check: is the row out after one look? Written as one flat
    // chain of the file header's sentences, deliberately not the header's branch layout, so the two check each other.
    //   before - the state before the look.
    //   in     - the look.
    //   returns - the expected `shown` after the look.
    bool OracleShownAfter(const TabRowAutoHideState& before, const TabRowAutoHideInputs& in)
    {
        if (!in.usable)
        {
            return false; // a window that cannot show the row forgets everything
        }
        const bool peeking = before.peekUntilMs != 0 && in.nowMs < before.peekUntilMs;
        const bool peekEnded = before.peekUntilMs != 0 && !peeking;
        if (before.shown)
        {
            const bool reason = in.hold || peeking || in.keyboardInRow || in.keyboardInPopup || before.touchShown || in.inRow ||
                                (in.buttonHeld && in.pressInRow);
            if (reason)
            {
                return true;
            }
            if (peekEnded && before.awaySinceMs == 0)
            {
                return false; // the peek was the last reason: the row goes with it
            }
            const uint64_t clock = (before.awaySinceMs == 0 || before.awaySinceMs > in.nowMs) ? in.nowMs : before.awaySinceMs;
            return in.nowMs - clock < Agentmaster::kTabRowHideDelayMs;
        }
        if (in.hold || peeking || in.keyboardInRow)
        {
            return true;
        }
        if (!in.atEdge || (in.buttonHeld && in.pressInContent) || before.dismissed)
        {
            return false;
        }
        const uint64_t rest = (before.edgeSinceMs == 0 || before.edgeSinceMs > in.nowMs) ? in.nowMs : before.edgeSinceMs;
        return in.nowMs - rest >= Agentmaster::kTabRowRevealDelayMs;
    }
}

// TestTabRowAutoHide pins WHEN the auto-hiding tab row shows (AgentMaster/TabRowAutoHide.h; the file header above).
void TestTabRowAutoHide()
{
    std::wprintf(L"Auto-hiding tab row (TabRowAutoHide.h, TAB_STRIP.md \"Auto-hiding the tab row\"):\n");
    using namespace Agentmaster;

    // --- the user's numbers -------------------------------------------------------------------------------------------
    static_assert(kTabRowEdgeDip == 4.0, "the edge band is the user's top 4 pixels");
    static_assert(kTabRowRevealDelayMs == 150, "the row comes out after a 0.15 s rest");
    static_assert(kTabRowHideDelayMs == 600, "the row goes 0.6 s after its last reason");
    static_assert(kTabRowSlideMs == 150, "the slide lasts 0.15 s");
    static_assert(kTabRowPeekMs == 1500, "a keyboard tab switch peeks for 1.5 s");
    static_assert(kTabRowWatchMs > 0 && kTabRowWatchMs < kTabRowHideDelayMs, "the watch looks several times inside one hide delay");

    // --- DIPs to pixels -------------------------------------------------------------------------------------------------
    CHECK(TabRowDipToPixels(4.0, 1.0, 1) == 4, "dip: 4 DIPs at 100% are 4 pixels");
    CHECK(TabRowDipToPixels(4.0, 1.25, 1) == 5, "dip: 4 DIPs at 125% are 5 pixels");
    CHECK(TabRowDipToPixels(4.0, 1.5, 1) == 6, "dip: 4 DIPs at 150% are 6 pixels");
    CHECK(TabRowDipToPixels(4.0, 1.75, 1) == 7, "dip: 4 DIPs at 175% are 7 pixels");
    CHECK(TabRowDipToPixels(4.0, 2.0, 1) == 8, "dip: 4 DIPs at 200% are 8 pixels");
    CHECK(TabRowDipToPixels(8.0, 1.0625, 0) == 9, "dip: a half pixel rounds up (8.5 -> 9)");
    CHECK(TabRowDipToPixels(8.0, 1.06, 0) == 8, "dip: under a half pixel rounds down (8.48 -> 8)");
    CHECK(TabRowDipToPixels(0.0, 1.0, 1) == 1, "dip: the minimum holds for a zero length");
    CHECK(TabRowDipToPixels(-3.0, 1.0, 0) == 0, "dip: a negative length counts as 0");
    CHECK(TabRowDipToPixels(4.0, 0.0, 1) == 4, "dip: a zero scale counts as 100%");
    CHECK(TabRowDipToPixels(4.0, -2.0, 1) == 4, "dip: a negative scale counts as 100%");
    {
        const double nan = std::nan("");
        CHECK(TabRowDipToPixels(nan, 1.0, 1) == 1 && TabRowDipToPixels(4.0, nan, 1) == 4, "dip: a NaN length or scale is neutralized");
    }
    CHECK(TabRowDipToPixels(1e12, 1.0, 0) == 1000000, "dip: an absurd length saturates instead of overflowing");
    CHECK(TabRowDipToPixels(4.0, 1e300, 0) == 1000000, "dip: an absurd scale saturates instead of overflowing");

    // --- the bands ------------------------------------------------------------------------------------------------------
    {
        // A restored window whose client area spans x 100..900, its top (the frame border row) at y 50.
        const auto edge = TabRowEdgeBand(100, 50, 900, 1.0);
        CHECK((edge == TabRowScreenBand{ 100, 50, 900, 54 }), "band: the edge band at 100% is the top 4 rows");
        CHECK(TabRowBandContains(edge, 100, 50) && TabRowBandContains(edge, 899, 53), "band: both corners of the edge band are inside");
        CHECK(!TabRowBandContains(edge, 900, 50), "band: the first column past the window is outside (half-open)");
        CHECK(!TabRowBandContains(edge, 99, 50), "band: the column left of the window is outside");
        CHECK(!TabRowBandContains(edge, 500, 54), "band: the fifth row is outside the 4-row band");
        CHECK(!TabRowBandContains(edge, 500, 49), "band: the row above the window is outside");
        CHECK((TabRowEdgeBand(100, 50, 900, 1.5) == TabRowScreenBand{ 100, 50, 900, 56 }), "band: at 150% the edge band is 6 rows");
        CHECK((TabRowEdgeBand(100, 50, 900, 0.0).bottom == 54), "band: an unreported scale still yields the 4-row band");
        CHECK((TabRowTouchEdgeBand(100, 50, 900, 1.0) == TabRowScreenBand{ 100, 50, 900, 66 }), "band: a finger's band is 16 rows");
        CHECK((TabRowTouchEdgeBand(100, 50, 900, 2.0).bottom == 82), "band: a finger's band scales (32 rows at 200%)");
        // The stay band: from the window's top to 8 DIPs under the row's bottom.
        const auto stay = TabRowStayBand(100, 50, 900, 90, 1.0);
        CHECK((stay == TabRowScreenBand{ 100, 50, 900, 98 }), "band: the stay band reaches 8 rows under the row");
        CHECK(TabRowBandContains(stay, 400, 97) && !TabRowBandContains(stay, 400, 98), "band: the stay band's last row is 8 under the row");
        CHECK((TabRowStayBand(100, 50, 900, 130, 2.0).bottom == 146), "band: at 200% the stay margin is 16 rows");
        CHECK((TabRowStayBand(100, 50, 900, 20, 1.0) == TabRowScreenBand{ 100, 50, 900, 58 }), "band: a row bottom above the top (not laid out) is clamped, never inverted");
        // A maximized window: its client top is the work area's edge.
        const auto maxEdge = TabRowEdgeBand(0, 0, 2560, 1.25);
        CHECK(TabRowBandContains(maxEdge, 0, 0) && TabRowBandContains(maxEdge, 2559, 4) && !TabRowBandContains(maxEdge, 1280, 5),
              "band: a maximized window's edge band is the screen's top 5 rows at 125%");
        // An inverted or empty band contains nothing.
        CHECK(!TabRowBandContains(TabRowScreenBand{ 10, 10, 10, 20 }, 10, 15) && !TabRowBandContains(TabRowScreenBand{ 10, 20, 30, 10 }, 15, 15),
              "band: an empty or inverted band contains nothing");
        // Negative screen coordinates (a monitor left of or above the primary) work like any other.
        CHECK(TabRowBandContains(TabRowEdgeBand(-1920, -1080, 0, 1.0), -1, -1077) && !TabRowBandContains(TabRowEdgeBand(-1920, -1080, 0, 1.0), 0, -1080),
              "band: a monitor at negative coordinates");
    }

    // --- a rest at the edge, and a crossing -----------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        auto step = NextTabRowAutoHide(s, AtEdge(1000));
        CHECK(!step.reveal && !step.state.shown && step.state.edgeSinceMs == 1000 && step.wakeAtMs == 1150,
              "rest: arriving at the edge starts the rest and asks for a look at its end");
        step = NextTabRowAutoHide(step.state, AtEdge(1100));
        CHECK(!step.reveal && step.state.edgeSinceMs == 1000 && step.wakeAtMs == 1150, "rest: 100 ms into the rest nothing shows yet");
        step = NextTabRowAutoHide(step.state, AtEdge(1149));
        CHECK(!step.reveal && !step.state.shown, "rest: 149 ms is not enough");
        step = NextTabRowAutoHide(step.state, AtEdge(1150));
        CHECK(step.reveal && !step.hide && step.state.shown && step.state.edgeSinceMs == 0 && step.wakeAtMs == 0,
              "rest: after 150 ms the row comes out, and nothing is pending while the pointer is on it");
        // A crossing: the pointer passes the edge in less than the rest and is gone.
        TabRowAutoHideState c{};
        step = NextTabRowAutoHide(c, AtEdge(2000));
        step = NextTabRowAutoHide(step.state, Quiet(2040));
        CHECK(!step.reveal && !step.state.shown && step.state.edgeSinceMs == 0 && step.wakeAtMs == 0,
              "crossing: leaving the edge band within the rest cancels it");
        step = NextTabRowAutoHide(step.state, Quiet(2200));
        CHECK(!step.reveal && !step.state.shown, "crossing: the cancelled rest never fires later");
        step = NextTabRowAutoHide(step.state, AtEdge(3000));
        CHECK(step.state.edgeSinceMs == 3000 && step.wakeAtMs == 3150, "crossing: coming back starts a fresh rest");
        // A clock in the future (a state carried across a GetTickCount64 reset that cannot happen, or a bad caller) restarts.
        TabRowAutoHideState future{};
        future.edgeSinceMs = 9000;
        step = NextTabRowAutoHide(future, AtEdge(5000));
        CHECK(!step.reveal && step.state.edgeSinceMs == 5000 && step.wakeAtMs == 5150, "rest: a rest that starts in the future restarts now");
    }

    // --- staying and leaving --------------------------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        s.shown = true;
        auto step = NextTabRowAutoHide(s, InRow(1000));
        CHECK(step.state.shown && !step.hide && step.state.awaySinceMs == 0 && step.wakeAtMs == 0, "stay: the pointer in the row keeps it, nothing pending");
        step = NextTabRowAutoHide(step.state, Quiet(2000));
        CHECK(step.state.shown && !step.hide && step.state.awaySinceMs == 2000 && step.wakeAtMs == 2600,
              "leave: the hide clock starts when the pointer leaves, with a look at its end");
        step = NextTabRowAutoHide(step.state, Quiet(2599));
        CHECK(step.state.shown && !step.hide, "leave: 599 ms away it is still out");
        step = NextTabRowAutoHide(step.state, Quiet(2600));
        CHECK(!step.state.shown && step.hide && !step.reveal && step.state.awaySinceMs == 0 && step.wakeAtMs == 0,
              "leave: 600 ms away it slides away");
        // Coming back inside the delay cancels it; leaving again starts it over.
        TabRowAutoHideState t{};
        t.shown = true;
        step = NextTabRowAutoHide(t, Quiet(1000));
        step = NextTabRowAutoHide(step.state, InRow(1300));
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "back: returning within the delay cancels the hide clock");
        step = NextTabRowAutoHide(step.state, Quiet(1500));
        CHECK(step.state.awaySinceMs == 1500 && step.wakeAtMs == 2100, "back: leaving again restarts the clock from that moment");
        step = NextTabRowAutoHide(step.state, Quiet(1700));
        CHECK(step.state.shown && !step.hide, "back: the first clock's end (1600) does not hide it");
        // The pointer over a popup of the window counts as in the row (a menu hanging below it, the + card, a tooltip).
        TabRowAutoHideState p{};
        p.shown = true;
        auto popup = Quiet(5000);
        popup.inRow = true; // the window host sets inRow for a pointer over any of its open popups
        step = NextTabRowAutoHide(p, popup);
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "popup: the pointer over a popup keeps the row");
        // A shown row's look never tracks a rest or a dismissal.
        TabRowAutoHideState odd{};
        odd.shown = true;
        odd.edgeSinceMs = 400;
        odd.dismissed = true;
        step = NextTabRowAutoHide(odd, InRow(1000));
        CHECK(step.state.edgeSinceMs == 0 && !step.state.dismissed, "stay: a shown row clears a stale rest and dismissal");
    }

    // --- drags ----------------------------------------------------------------------------------------------------------
    {
        // A selection dragged from the terminal to the top row: dismissed, and stays dismissed while at the edge.
        TabRowAutoHideState s{};
        auto drag = AtEdge(1000);
        drag.buttonHeld = true;
        drag.pressInContent = true;
        auto step = NextTabRowAutoHide(s, drag);
        CHECK(!step.reveal && step.state.dismissed && step.state.edgeSinceMs == 0 && step.wakeAtMs == 0,
              "drag: a drag from the terminal reaching the edge is dismissed, nothing pending");
        drag.nowMs = 3000;
        step = NextTabRowAutoHide(step.state, drag);
        CHECK(!step.reveal && !step.state.shown, "drag: resting there two seconds still never brings the row");
        step = NextTabRowAutoHide(step.state, AtEdge(3100)); // the button released, the pointer still at the edge
        CHECK(!step.reveal && step.state.dismissed && step.wakeAtMs == 0, "drag: releasing at the edge keeps it away (the pointer has not left)");
        step = NextTabRowAutoHide(step.state, AtEdge(4000));
        CHECK(!step.reveal && !step.state.shown, "drag: still away at the edge long after the release");
        step = NextTabRowAutoHide(step.state, Quiet(4100));
        CHECK(!step.state.dismissed, "drag: leaving the edge band lifts the dismissal");
        step = NextTabRowAutoHide(step.state, AtEdge(4200));
        step = NextTabRowAutoHide(step.state, AtEdge(4350));
        CHECK(step.reveal && step.state.shown, "drag: coming back afterwards brings the row after the usual rest");
        // A drag that started outside the terminal (a folder from Explorer onto +, a press in a popup or the drag bar).
        TabRowAutoHideState o{};
        auto ole = AtEdge(1000);
        ole.buttonHeld = true;
        step = NextTabRowAutoHide(o, ole);
        ole.nowMs = 1150;
        step = NextTabRowAutoHide(step.state, ole);
        CHECK(step.reveal && step.state.shown, "drag: a drag that started elsewhere brings the row like a plain approach");
        // A tab drag: the press in the row keeps it out while the pointer is away.
        TabRowAutoHideState t{};
        t.shown = true;
        auto tabDrag = Quiet(1000);
        tabDrag.buttonHeld = true;
        tabDrag.pressInRow = true;
        step = NextTabRowAutoHide(t, tabDrag);
        tabDrag.nowMs = 5000;
        step = NextTabRowAutoHide(step.state, tabDrag);
        CHECK(step.state.shown && !step.hide && step.state.awaySinceMs == 0, "drag: a tab drag keeps the row out however far the pointer goes");
        step = NextTabRowAutoHide(step.state, Quiet(5100));
        CHECK(step.state.shown && step.state.awaySinceMs == 5100 && step.wakeAtMs == 5700, "drag: the release away from the row starts the hide clock");
        // A press origin flag without a held button means nothing.
        TabRowAutoHideState h{};
        auto stale = AtEdge(1000);
        stale.pressInContent = true; // buttonHeld false: a stale origin the host had not cleared yet
        step = NextTabRowAutoHide(h, stale);
        CHECK(!step.state.dismissed && step.state.edgeSinceMs == 1000, "drag: a press origin with no button held is ignored");
        // A held button from a content press while the row is SHOWN does not keep it (only a press in the row does).
        TabRowAutoHideState sel{};
        sel.shown = true;
        auto selection = Quiet(1000);
        selection.buttonHeld = true;
        selection.pressInContent = true;
        step = NextTabRowAutoHide(sel, selection);
        CHECK(step.state.awaySinceMs == 1000, "drag: a selection in the terminal does not hold a shown row");
    }

    // --- the hold (the Agent Manager page) ------------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        auto held = Quiet(1000);
        held.hold = true;
        auto step = NextTabRowAutoHide(s, held);
        CHECK(step.reveal && step.state.shown && step.wakeAtMs == 0, "hold: the page's hold brings the row out at once, nothing pending");
        held.nowMs = 100000;
        step = NextTabRowAutoHide(step.state, held);
        CHECK(step.state.shown && !step.hide && step.state.awaySinceMs == 0 && step.wakeAtMs == 0, "hold: the row stays out the whole time, the pointer elsewhere");
        step = NextTabRowAutoHide(step.state, Quiet(100100));
        CHECK(step.state.shown && step.state.awaySinceMs == 100100 && step.wakeAtMs == 100700, "hold: when the hold ends the usual hide delay runs");
        step = NextTabRowAutoHide(step.state, Quiet(100700));
        CHECK(step.hide && !step.state.shown, "hold: ...and the row goes at its end");
        // A hold that ends while the pointer is on the row: the row stays.
        TabRowAutoHideState r{};
        r.shown = true;
        step = NextTabRowAutoHide(r, InRow(500));
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "hold: ending under the pointer, the row stays");
    }

    // --- peeks ----------------------------------------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        auto step = TabRowAutoHidePeek(s, 5000, kTabRowPeekMs);
        CHECK(step.reveal && step.state.shown && step.state.peekUntilMs == 6500 && step.wakeAtMs == 6500, "peek: a hidden row comes out until the peek's end");
        step = NextTabRowAutoHide(step.state, Quiet(6000));
        CHECK(step.state.shown && !step.hide && step.state.awaySinceMs == 0 && step.wakeAtMs == 6500, "peek: during the peek it stays, the pointer elsewhere");
        step = NextTabRowAutoHide(step.state, Quiet(6500));
        CHECK(step.hide && !step.state.shown && step.state.peekUntilMs == 0 && step.wakeAtMs == 0,
              "peek: at its end the row goes at once (1.5 s, not 1.5 s + the hide delay)");
        // A peek that ends under the pointer: the row stays, and leaving later runs the usual delay.
        TabRowAutoHideState u{};
        step = TabRowAutoHidePeek(u, 1000, kTabRowPeekMs);
        step = NextTabRowAutoHide(step.state, InRow(2500));
        CHECK(step.state.shown && step.state.peekUntilMs == 0, "peek: ending with the pointer on the row keeps it out");
        step = NextTabRowAutoHide(step.state, Quiet(3000));
        CHECK(step.state.shown && step.state.awaySinceMs == 3000 && step.wakeAtMs == 3600, "peek: leaving after it ended runs the usual delay");
        // Peeks never shorten each other; a later longer one extends.
        TabRowAutoHideState e{};
        step = TabRowAutoHidePeek(e, 1000, 1500);
        step = TabRowAutoHidePeek(step.state, 1200, 500);
        CHECK(!step.reveal && step.state.peekUntilMs == 2500, "peek: a shorter later peek keeps the longer end (and the row is already out)");
        step = TabRowAutoHidePeek(step.state, 2000, 1500);
        CHECK(step.state.peekUntilMs == 3500 && step.wakeAtMs == 3500, "peek: a later peek that ends later extends it");
        // A peek on a shown row with the hide clock running cancels the clock.
        TabRowAutoHideState g{};
        g.shown = true;
        g.awaySinceMs = 800;
        step = TabRowAutoHidePeek(g, 1000, kTabRowPeekMs);
        CHECK(!step.reveal && step.state.awaySinceMs == 0 && step.state.peekUntilMs == 2500, "peek: a peek on a leaving row cancels its hide clock");
        // A zero-length peek brings the row out with no peek of its own: the next look decides.
        TabRowAutoHideState z{};
        step = TabRowAutoHidePeek(z, 1000, 0);
        CHECK(step.reveal && step.state.shown && step.state.peekUntilMs == 0 && step.wakeAtMs == 0, "peek: a zero-length peek only brings the row out");
        step = NextTabRowAutoHide(step.state, Quiet(1001));
        CHECK(step.state.shown && step.state.awaySinceMs == 1001, "peek: ...and the next look runs the usual delay");
        // A peek's end that a late look finds long past still hides at once.
        TabRowAutoHideState late{};
        step = TabRowAutoHidePeek(late, 1000, kTabRowPeekMs);
        step = NextTabRowAutoHide(step.state, Quiet(9000));
        CHECK(step.hide && !step.state.shown, "peek: a look long after the peek's end hides at once");
    }

    // --- the keyboard ---------------------------------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        auto key = Quiet(1000);
        key.keyboardInRow = true;
        auto step = NextTabRowAutoHide(s, key);
        CHECK(step.reveal && step.state.shown, "keyboard: the keyboard moving into the row (a rename) brings it out at once");
        key.nowMs = 60000;
        step = NextTabRowAutoHide(step.state, key);
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "keyboard: it stays while the keyboard is in it");
        TabRowAutoHideState m{};
        m.shown = true;
        auto menu = Quiet(1000);
        menu.keyboardInPopup = true;
        step = NextTabRowAutoHide(m, menu);
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "keyboard: an open menu or the + card (focus in a popup) keeps the row");
        TabRowAutoHideState n{};
        step = NextTabRowAutoHide(n, menu);
        CHECK(!step.reveal && !step.state.shown, "keyboard: a popup elsewhere (the side panel's funnel) never brings a hidden row");
    }

    // --- a window that cannot show the row ------------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        s.shown = true;
        s.peekUntilMs = 9000;
        s.touchShown = true;
        auto off = InRow(1000);
        off.usable = false;
        off.hold = true;
        auto step = NextTabRowAutoHide(s, off);
        CHECK(step.hide && !step.reveal && (step.state == TabRowAutoHideState{}) && step.wakeAtMs == 0,
              "unusable: a minimized / focus-mode window hides the row and forgets everything, even a hold and a peek");
        TabRowAutoHideState r{};
        r.edgeSinceMs = 900;
        r.dismissed = true;
        auto off2 = AtEdge(1000);
        off2.usable = false;
        step = NextTabRowAutoHide(r, off2);
        CHECK(!step.hide && !step.reveal && (step.state == TabRowAutoHideState{}), "unusable: a hidden row's pending rest is dropped, no slide ordered");
    }

    // --- a finger -------------------------------------------------------------------------------------------------------
    {
        TabRowAutoHideState s{};
        auto step = TabRowAutoHideFingerPress(s, 1000, false, false);
        CHECK(!step.reveal && !step.state.shown, "finger: a tap in the terminal leaves a hidden row hidden");
        step = TabRowAutoHideFingerPress(s, 1000, true, false);
        CHECK(step.reveal && step.state.shown && step.state.touchShown && step.wakeAtMs == 0, "finger: a tap at the top brings the row out");
        step = NextTabRowAutoHide(step.state, Quiet(5000));
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "finger: the mouse cursor elsewhere does not hide a row a finger brought");
        step = TabRowAutoHideFingerPress(step.state, 6000, false, true);
        CHECK(step.state.shown && step.state.touchShown && !step.hide, "finger: a tap on the row (a tab) changes nothing");
        step = TabRowAutoHideFingerPress(step.state, 7000, false, false);
        CHECK(step.state.shown && !step.state.touchShown && step.state.awaySinceMs == 7000 && step.wakeAtMs == 7600,
              "finger: a tap elsewhere ends the finger's hold and starts the hide clock");
        step = NextTabRowAutoHide(step.state, Quiet(7600));
        CHECK(step.hide && !step.state.shown, "finger: ...and the row goes at its end");
        // The mouse moving hands the row back to the cursor's rule.
        TabRowAutoHideState t{};
        t.shown = true;
        t.touchShown = true;
        const auto moved = TabRowAutoHideMouseMoved(t);
        CHECK(!moved.touchShown && moved.shown, "finger: a mouse move ends the finger's hold, nothing else");
        step = NextTabRowAutoHide(moved, Quiet(100));
        CHECK(step.state.awaySinceMs == 100, "finger: ...after which the cursor away starts the hide clock");
        // A tap at the top on a SHOWN row is a tap on the row (the band lies inside it): nothing changes.
        TabRowAutoHideState u{};
        u.shown = true;
        step = TabRowAutoHideFingerPress(u, 100, true, true);
        CHECK(step.state == u && !step.reveal && !step.hide, "finger: a tap on the top of a shown row changes nothing");
    }

    // --- every input combination over a grid of states, against the second statement and the invariants -----------------
    {
        int combos = 0;
        int mismatches = 0;
        int invariantBreaks = 0;
        const uint64_t now = 10000;
        // The states: hidden / shown x dismissed x touchShown x the rest's clock x the hide clock x the peek.
        const uint64_t edgeClocks[] = { 0, now - 50, now - 150, now - 5000, now + 100 };
        const uint64_t awayClocks[] = { 0, now - 50, now - 600, now - 5000, now + 100 };
        const uint64_t peeks[] = { 0, now + 500, now, now - 1 };
        for (int bits = 0; bits < (1 << 9); ++bits)
        {
            TabRowAutoHideInputs in{};
            in.nowMs = now;
            in.usable = (bits & 1) != 0;
            in.hold = (bits & 2) != 0;
            in.atEdge = (bits & 4) != 0;
            in.inRow = (bits & 8) != 0 || in.atEdge; // the edge band lies inside the stay band
            in.buttonHeld = (bits & 16) != 0;
            in.pressInContent = (bits & 32) != 0;
            in.pressInRow = (bits & 64) != 0 && !in.pressInContent; // one press has one origin
            in.keyboardInRow = (bits & 128) != 0;
            in.keyboardInPopup = (bits & 256) != 0;
            for (const bool shown : { false, true })
            {
                for (const bool dismissed : { false, true })
                {
                    for (const bool touch : { false, true })
                    {
                        for (const auto edgeClock : edgeClocks)
                        {
                            for (const auto awayClock : awayClocks)
                            {
                                for (const auto peek : peeks)
                                {
                                    TabRowAutoHideState before{};
                                    before.shown = shown;
                                    // Only the combinations a real sequence can produce: a shown row tracks no rest and no
                                    // dismissal; a hidden one runs no hide clock and holds no finger.
                                    before.dismissed = shown ? false : dismissed;
                                    before.touchShown = shown ? touch : false;
                                    before.edgeSinceMs = shown ? 0 : edgeClock;
                                    before.awaySinceMs = shown ? awayClock : 0;
                                    before.peekUntilMs = peek;
                                    if (!shown && (touch || awayClock != 0))
                                    {
                                        continue; // skip the duplicates the reductions above create
                                    }
                                    if (shown && (dismissed || edgeClock != 0))
                                    {
                                        continue;
                                    }
                                    ++combos;
                                    const auto step = NextTabRowAutoHide(before, in);
                                    if (step.state.shown != OracleShownAfter(before, in))
                                    {
                                        ++mismatches;
                                    }
                                    bool ok = true;
                                    ok = ok && !(step.reveal && step.hide); // one slide at most
                                    ok = ok && (step.reveal == (!before.shown && step.state.shown)); // a reveal is exactly hidden -> shown
                                    ok = ok && (step.hide == (before.shown && !step.state.shown)); // a hide is exactly shown -> hidden
                                    ok = ok && (in.usable || (step.state == TabRowAutoHideState{})); // unusable forgets everything
                                    ok = ok && (!step.state.shown || (!step.state.dismissed && step.state.edgeSinceMs == 0)); // shown: no rest
                                    ok = ok && (step.state.shown || (step.state.awaySinceMs == 0 && !step.state.touchShown)); // hidden: no clock
                                    ok = ok && (!(in.usable && in.hold) || step.state.shown); // the hold always shows
                                    ok = ok && (step.wakeAtMs == 0 || step.wakeAtMs > now); // a wake is always in the future
                                    ok = ok && (step.state.peekUntilMs == 0 || step.state.peekUntilMs > now); // an ended peek leaves no trace
                                    ok = ok && (step.state.edgeSinceMs <= now && step.state.awaySinceMs <= now); // no clock in the future survives
                                    if (!ok)
                                    {
                                        ++invariantBreaks;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        std::wprintf(L"  sweep: %d combinations, %d mismatches with the second statement, %d invariant breaks\n", combos, mismatches, invariantBreaks);
        CHECK(combos > 10000, "sweep: the grid covers every input combination over the reachable states");
        CHECK(mismatches == 0, "sweep: the rule agrees with its second statement on every combination");
        CHECK(invariantBreaks == 0, "sweep: no combination breaks an invariant (one slide, exact slides, unusable forgets, no rest while shown, no clock while hidden, the hold shows, wakes and clocks never in the past/future wrongly)");
    }

    // --- one user's visit, event by event ------------------------------------------------------------------------------
    {
        // Maximized window. The pointer goes up and stops at the screen edge; the row comes out; the user opens the ▾ menu
        // (the menu hangs below the row); picks nothing and clicks into the terminal; the row goes. Later Ctrl+Tab peeks.
        TabRowAutoHideState s{};
        auto step = NextTabRowAutoHide(s, Quiet(0)); // the pointer works in the terminal
        CHECK(!step.state.shown, "visit: starts hidden");
        step = NextTabRowAutoHide(step.state, AtEdge(10000)); // stops at the screen's top
        CHECK(!step.reveal && step.wakeAtMs == 10150, "visit: resting at the top asks for a look at 10150");
        step = NextTabRowAutoHide(step.state, AtEdge(10150)); // the wake
        CHECK(step.reveal, "visit: the row slides out at 10150");
        step = NextTabRowAutoHide(step.state, InRow(10400)); // over a tab
        auto menuOpen = Quiet(10600);
        menuOpen.keyboardInPopup = true; // the ▾ menu took the keyboard
        menuOpen.inRow = true; // the pointer over the menu
        step = NextTabRowAutoHide(step.state, menuOpen);
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "visit: the open menu keeps it");
        menuOpen.inRow = false; // the pointer leaves the menu but it stays open
        menuOpen.nowMs = 12000;
        step = NextTabRowAutoHide(step.state, menuOpen);
        CHECK(step.state.shown && step.state.awaySinceMs == 0, "visit: the menu still open, the pointer off it: still kept");
        auto click = Quiet(13000); // a click into the terminal closed the menu; the keyboard went back to the terminal
        click.buttonHeld = true;
        click.pressInContent = true;
        step = NextTabRowAutoHide(step.state, click);
        CHECK(step.state.shown && step.state.awaySinceMs == 13000, "visit: the click into the terminal starts the hide clock");
        step = NextTabRowAutoHide(step.state, Quiet(13300));
        step = NextTabRowAutoHide(step.state, Quiet(13600));
        CHECK(step.hide && !step.state.shown, "visit: 0.6 s later the row slides away");
        step = TabRowAutoHidePeek(step.state, 20000, kTabRowPeekMs); // Ctrl+Tab
        CHECK(step.reveal && step.wakeAtMs == 21500, "visit: Ctrl+Tab peeks");
        step = NextTabRowAutoHide(step.state, Quiet(21500));
        CHECK(step.hide, "visit: ...and the peek ends at 21500");
    }
}
