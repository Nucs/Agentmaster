// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the side panel's PIN TO TOP (SIDE_PANEL.md §2j): which cards go first, and the stamp a pin gets.
//
// The user's request of 2026-11-06: a card's right-click menu pins it ("Pin to Top", right under Jump to Tab); a pinned
// card goes to the top of the side panel's list and wears a pushpin in its title band's top-right corner, and pinning a
// pinned card again brings it back to the top. Their pick, the same day, of how it relates to Telegram's /pin
// (SessionInfo::pinned, MESSAGING.md §5.3, "never idle"): a SEPARATE pin that orders the cards and nothing else — it never
// touches the session's state, and neither pin sets, reads or shows the other.
//
// One stamp per session carries it all (SessionInfo::sidePanelPinnedAtUnixMs, persisted; 0 = not pinned):
//   - the list puts every pinned card first, the NEWEST pin on top, and keeps the board's own order (MakeSortKey /
//     SortKeyLess) for the rest and for pins of one stamp (CompareSidePanelPins);
//   - a pin, or a pin again, stamps the clock — past the newest pin there is, so the card lands above every other pinned
//     card even when the clock stepped back or two pins fall in one millisecond (NextSidePanelPinStamp);
//   - when one tab switches conversation (a /clear, a /resume, a /handover-here), the successor takes the pin of the
//     conversation it replaced (SidePanelPinAfterRehome): the user pinned the tab's card, not a conversation id.
//
// Plain C++, header-only (the engine idiom of SidePanelAiList.h / StatusIconMotion.h): no WinRT, no XAML, no <windows.h>
// macro hazard (the max below is parenthesized), so the m5 harness pins it (TestSidePanelPins, tests_sidepanel.cpp) and
// the app only applies it (TerminalPage::_SidePanelRebuildCards and _SidePanelSetPinned; the re-home in
// TerminalPage::_BindClaudeSessionToTab).

#pragma once

#include <cstdint>
#include <limits>

namespace Agentmaster
{
    // Is a stamp a pin? Only a positive one is: 0 means "not pinned", and a negative value (a hand-edited sessions.json —
    // the loader already reads one as 0) must never sort as pinned.
    //   pinnedAtUnixMs - SessionInfo::sidePanelPinnedAtUnixMs.
    //   returns        - true when the card is pinned.
    //   throws         - nothing.
    [[nodiscard]] constexpr bool IsSidePanelPinStamp(int64_t pinnedAtUnixMs) noexcept
    {
        return pinnedAtUnixMs > 0;
    }

    // The pinned-first half of the side panel's order, for two rows. The pins decide when exactly one row is pinned (it goes
    // first) or both are pinned with different stamps (the newer goes first, so the last card pinned is the top card);
    // otherwise they decide nothing and the caller's own order (the board's sort) ranks the two.
    // ⚠ Combine it as `pin < 0 || (pin == 0 && boardLess(a, b))`, never with the board's order first: that pairing is a
    // lexicographic order on (pin class, board key), hence a strict weak ordering std::stable_sort accepts — the pins split
    // the rows into "pinned, one class per stamp, newest first" and "the rest", and the board ranks rows inside one class.
    //   a, b    - the two rows' SessionInfo::sidePanelPinnedAtUnixMs.
    //   returns - < 0 when a goes first, > 0 when b goes first, 0 when the pins leave it to the caller's order.
    //   throws  - nothing.
    [[nodiscard]] constexpr int CompareSidePanelPins(int64_t a, int64_t b) noexcept
    {
        const bool aPinned = IsSidePanelPinStamp(a);
        const bool bPinned = IsSidePanelPinStamp(b);
        if (aPinned != bPinned)
        {
            return aPinned ? -1 : 1;
        }
        if (!aPinned || a == b)
        {
            return 0; // neither is pinned, or both carry one stamp: the board's order ranks them
        }
        return a > b ? -1 : 1;
    }

    // The stamp a pin (or a pin again) gives its card: the clock, but always past `newestPinMs`, the newest pin among the
    // cards it is ordered with, so the card is the top card the moment it is stamped. The clock alone could fail that: a
    // clock stepped back (an NTP correction) would rank an older pin above it, and two pins in one millisecond would tie.
    //   nowMs       - the system clock, ms since the Unix epoch.
    //   newestPinMs - the newest SessionInfo::sidePanelPinnedAtUnixMs among those cards (0 when none is pinned). The card's
    //                 own stamp may be that newest one: a pin again of the top card keeps it on top and still stamps it newer.
    //   returns     - max(nowMs, newestPinMs + 1), never below 1 (a pin is a positive stamp) and never past INT64_MAX (a corrupt
    //                 newest stamp at the type's end returns that end — the card ties with it instead of wrapping negative).
    //   throws      - nothing.
    [[nodiscard]] constexpr int64_t NextSidePanelPinStamp(int64_t nowMs, int64_t newestPinMs) noexcept
    {
        constexpr int64_t kMax = (std::numeric_limits<int64_t>::max)();
        const int64_t pastNewest = newestPinMs <= 0 ? 1 : (newestPinMs >= kMax ? kMax : newestPinMs + 1);
        return nowMs > pastNewest ? nowMs : pastNewest;
    }

    // The pin a successor carries when its TAB switches conversation (TerminalPage::_BindClaudeSessionToTab's re-home: a
    // /clear, a /resume, a /handover-here). The card the user pinned is the tab's, so the successor keeps the newer of its
    // own stamp (a /resume back to a conversation that was pinned when it closed) and the replaced conversation's. The
    // replaced conversation, archived by the re-home, gives its pin up (the caller clears it there), so resuming it later
    // from the Sessions page brings back no second pinned card.
    //   successorPinMs  - the incoming conversation's SessionInfo::sidePanelPinnedAtUnixMs.
    //   supersededPinMs - the replaced conversation's.
    //   returns         - the successor's new stamp; 0 when neither was pinned (a negative stamp counts as none).
    //   throws          - nothing.
    [[nodiscard]] constexpr int64_t SidePanelPinAfterRehome(int64_t successorPinMs, int64_t supersededPinMs) noexcept
    {
        const int64_t own = IsSidePanelPinStamp(successorPinMs) ? successorPinMs : 0;
        const int64_t carried = IsSidePanelPinStamp(supersededPinMs) ? supersededPinMs : 0;
        return own > carried ? own : carried;
    }
}
