// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - the SIDE PANEL's pure helpers (SIDE_PANEL.md section 2c) and the session timing line its
// cards share with the Triage Board's cards and the Explorer tree's rows.
//
// TestSessionTimingText pins that line (AgentMaster/SessionTimingText.h): "created 4d1h, active 2m4s ago" on a fixed
// clock - the user's two examples, the word thresholds ("created now" under 5 minutes, "created recently" under an
// hour), the units (y / d / h for created, + m / s for active, seconds only under an hour, no month), the active part
// left out when it is 0, and the unknown / clock-skew edges; plus a sweep over 3 years of ages.
//
// TestKeyedListDiff pins the card list's order planner (AgentMaster/KeyedListDiff.h): for every
// (current, target) pair it checks that applying the planned operations in order turns current into
// EXACTLY target, that every index is in range when its operation runs (the XAML panel would throw
// E_INVALIDARG otherwise), and that the plan is MINIMAL - one Remove per vanished key, one Insert per
// new key, and |survivors| - LIS moves (checked against an independent O(n^2) longest-increasing-
// subsequence count, so a bug in the planner's patience-sorting LIS can't hide behind itself).
// Exhaustive over every permutation of 5 keys plus a seeded random sweep with removals and insertions.
//
// TestPointerCardPlacement pins where the header's "+" launcher card opens (AgentMaster/PointerCardPlacement.h,
// SIDE_PANEL.md section 2e): its corner at the click point, extending toward the window's center (each axis decided by
// the click's half, the exact middle counting as the left / top half), clamped inside the window with a margin, a card
// larger than the window pinned to the near margin, an unmeasured window never clamped, and every NaN / infinity /
// negative input neutralized; plus a seeded sweep of the invariants over random clicks, cards and windows.
//
// TestSubmenuCardPlacement pins where the funnel panel's "By Folder..." row opens its hover SUBMENU (PlaceSubmenuCard, the
// same header, SIDE_PANEL.md section 2l "The By Folder submenu"): beside the panel's right edge when the whole card fits
// there, else beside its left edge, else on the roomier side clamped into the window; its top level with the row and
// clamped; an unmeasured window never clamped; NaN / negative / reversed inputs neutralized; plus a seeded sweep - inside
// the window, never over the panel while a side holds it, the right side first, exactly gap off the panel.
//
// TestVirtualListWindow pins which rows of the VIRTUALIZED card list get cards (AgentMaster/VirtualListWindow.h,
// SIDE_PANEL.md section 2f): the row arithmetic at exact row boundaries and outside the content, the viewport clamp
// for a list that just shrank, the policy's hysteresis (the realized range is kept while it covers the viewport's
// NEAR rows, replaced by the FAR rows otherwise, trimmed when much larger), the reveal range (it holds the asked-for
// row, and after either minimum scroll the plan keeps it - no second pass), the spacer heights (the content is
// count * stride tall whatever is realized), the median stride (an outlier cannot move it), and a seeded scroll
// simulation over random lists, strides and viewports: the visible rows always have cards, the realized range stays
// bounded, and a scroll of one row per step re-plans only every few rows.
//
// TestStatusIconMotion pins WHEN the status icon's three motions may run (AgentMaster/StatusIconMotion.h, SIDE_PANEL.md
// section 2g) - the tab header's rule, which the side panel's card icon now shares: the named cases the rule exists for
// (a collapsed panel, a card scrolled away, Windows' animation effects off, a dormant tab's remembered draft, the ring as
// an alert), then all 128 combinations of the seven inputs against an independent statement of each motion's gates.
//
// TestSidePanelAiList pins WHEN the side panel's AI view changes its list (AgentMaster/SidePanelAiList.h, SIDE_PANEL.md
// section 2h; the user's rule of 2026-11-05, "update the list only at the end when the result is final"): the step every
// event kind takes, running or not; the comparison that decides whether a request's end replaces the list (the flag, the
// ids, their order, each why); and whole requests replayed through a small model of the view - a request that searches,
// shows, shows again and answers changes the list ONCE, at its end, to the last show; one that answers in words changes
// nothing; a request that ends in a failure after a show still lands its pick; a reset clears at once.
//
// TestSidePanelAiCopy pins the AI strip's crossed circle and the copy button under it (AgentMaster/SidePanelAiCopy.h,
// SIDE_PANEL.md section 2h; the user's two requests of 2026-11-10): the box refill (an empty or blank box gets the newest
// request on one line, a box with words keeps them, no request writes nothing); the fold of the conversation's record (the
// plan before a tool call left out, the text after the last call as the reply, several blocks joined, a failure's notice, a
// request stopped without a reply, a running newest request whether or not the record holds it yet, items of no turn and
// compaction notices skipped, a request lost to the record's cap with its orphaned reply, a gap from a replaced request never
// blamed on the cap); the whole Markdown text of a known conversation, byte for byte; the text's edges (one request, running,
// a list that fits nothing, no list, the list of the answer before a running request, the cap's note, a missing or two-line
// title, a two-digit item's indent, a reply of two paragraphs, code spans around backticks, no time, nothing to copy); and the
// local time stamp's shape.
//
// TestSidePanelPins pins the cards' PIN TO TOP (AgentMaster/SidePanelPins.h, SIDE_PANEL.md section 2j): the pins first, the
// newest on top, the board's order for the rest; a pin again bringing a card back to the top on any clock; the comparator's
// strict weak ordering; the stamp's edges; the pin a tab keeps when it switches conversation; the sessions.json key (apart
// from Telegram's never-idle pin) and the registry's lean copies.
//
// TestSidePanelFilters pins the funnel's FILTERS (AgentMaster/SidePanelFilters.h, SIDE_PANEL.md section 2l): the facet bits
// and the "every facet but one" question the panel's counts ask; the six states' bits; the "active within" boundary; which
// records filter at all; the edits (a folder trimmed, deduped by its key and re-choosing its subfolders; a state, a tag
// case-insensitively, a window); the matcher - an empty record passes everything, a folder with and without its subfolders
// at a segment boundary and through the inferred folder, OR inside a facet, AND across facets, the flags each narrowing,
// Untagged; the chips in the facets' order with their words and what each chip's x leaves; and the record's JSON - a round
// trip, the empty record, every malformed part read as "not set", and the window record's key written only while a filter
// is set. Then By Folder's PICK HISTORY and its hover submenu (the user's request of 2026-11-10): a pick on top, a repick in
// any spelling moved up with its new choice, several picks at once, the seeding that adds only missing folders, the cap of
// ten, the JSON and the filter-folders.json document, the file itself through the `...In` seams on a scratch profile (no
// file for a seeding that adds nothing, the freshest-file read-modify-write, the writer stamp); the submenu's lines (the
// record's unlisted folders first and checked, the history's order, the record's spelling and choice on a checked line), the
// toggle judged against the record as it is, a line turned off turning back on as it was, the removal by any spelling, the
// line's words, and a line's coverage equal to the matcher's By Folder verdict for that folder alone. And THE CARD'S FILTER
// MENU (the user's request of 2026-11-12): the two facets only a card sets, Branch and ForkFamily (the record, what applies,
// the matcher - a branch exactly, a family by membership -, their chips after the folders', their x and click, a pick
// resuming, their JSON and what normalizing keeps); the fork family as the connected component of the fork links, ids in
// any case; the Folder submenu's levels (nearest first, no drive or share root, any spelling, the cap) and a line's dim hint
// (a short path whole, a deep one cut in the middle around an ellipsis, never longer than its cap); and a card pick - the
// record of each line, the filter started over whatever it held, the check on the line that is the whole filter (by
// identity, only while it applies and no text is typed), the drop on its second click, a line that names nothing.
//
// TestSidePanelStripReveal pins WHEN the collapsed panel's strip shows (AgentMaster/SidePanelStripReveal.h, SIDE_PANEL.md
// section 2m; the user's request of 2026-11-12): near = within 40 DIPs of the strip (60 from the docked edge of the content,
// inclusive, any height, mirrored docked right, never outside the area, bad inputs neutralized); the rule - an approach
// shows it, away hides it, a drag passing by and a press beside the button keep it away until the pointer has left, a press
// on the button keeps it, a finger's taps, the watch that only hides, the after-collapse linger; all 288 input combinations
// against a second statement of the rule; one user's visit event by event.

#include "m5_tests.h"

#include "../KeyedListDiff.h"
#include "../PointerCardPlacement.h" // the "+" launcher card's tooltip-like placement (TestPointerCardPlacement)
#include "../SessionTimingText.h" // the cards' and rows' "created 4d1h, active 2m4s ago" (TestSessionTimingText)
#include "../SidePanelAiList.h" // the AI view's list rule: only a request's end or a reset changes it (TestSidePanelAiList)
#include "../SidePanelAiCopy.h" // the crossed circle's refill and the copy button's Markdown (TestSidePanelAiCopy)
#include "../SidePanelFilters.h" // the funnel's FILTERS: facets, matcher, edits, chips, the record's JSON (TestSidePanelFilters)
#include "../SidePanelPins.h" // Pin to Top: pinned cards first, the newest pin on top, the pin stamp, the re-home carry (TestSidePanelPins)
#include "../SidePanelStripReveal.h" // the collapsed strip's auto-hide: when the pointer is near it, and the strip's next state (TestSidePanelStripReveal)
#include "../StatusIconMotion.h" // the status icon's motion rule, shared by the tab header and the card icon (TestStatusIconMotion)
#include "../VirtualListWindow.h" // the virtualized card list's realized rows (TestVirtualListWindow)

#include <algorithm>
#include <cmath> // std::fabs / std::nan / HUGE_VAL - TestPointerCardPlacement
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using KeyList = std::vector<std::wstring>;

    // The length of a longest strictly increasing subsequence of `seq` - the textbook O(n^2) dynamic
    // program, deliberately NOT the planner's algorithm, so the two cross-check each other.
    //   seq     - the values.
    //   returns - the length (0 for an empty input).
    size_t LisLengthSlow(const std::vector<size_t>& seq)
    {
        std::vector<size_t> best(seq.size(), 1);
        size_t longest = 0;
        for (size_t i = 0; i < seq.size(); ++i)
        {
            for (size_t j = 0; j < i; ++j)
            {
                if (seq[j] < seq[i])
                {
                    best[i] = (std::max)(best[i], best[j] + 1);
                }
            }
            longest = (std::max)(longest, best[i]);
        }
        return longest;
    }

    // The fewest moves any plan can make: the survivors (keys in both lists), in their CURRENT order,
    // mapped to their TARGET positions; whatever lies outside a longest increasing run of those must move.
    //   current, target - the two lists (unique keys).
    //   returns         - |survivors| - LIS.
    size_t MinimumMoves(const KeyList& current, const KeyList& target)
    {
        std::vector<size_t> positions;
        for (const auto& key : current)
        {
            const auto it = std::find(target.begin(), target.end(), key);
            if (it != target.end())
            {
                positions.push_back(static_cast<size_t>(it - target.begin()));
            }
        }
        return positions.size() - LisLengthSlow(positions);
    }

    // The outcome of checking one (current, target) pair.
    struct PlanCheck
    {
        bool reachesTarget{ false }; // applying the plan in order yields exactly `target`
        bool indexesValid{ false }; // every op's indexes were in range when it ran (XAML's own preconditions)
        bool minimal{ false }; // removes / inserts / moves equal their lower bounds
        size_t removes{ 0 };
        size_t inserts{ 0 };
        size_t moves{ 0 };
    };

    // Plan current -> target, apply the plan to a copy of current, and judge it.
    //   current, target - the two lists (unique keys).
    //   returns         - the verdicts + the op counts.
    PlanCheck CheckPlan(const KeyList& current, const KeyList& target)
    {
        PlanCheck result;
        result.indexesValid = true;
        KeyList list = current;
        for (const auto& op : PlanKeyedListOps(current, target))
        {
            // The XAML preconditions, checked BEFORE the apply: RemoveAt/Move need from < size, Move needs
            // to < size, InsertAt needs to <= size.
            switch (op.kind)
            {
            case KeyedListOpKind::Remove:
                ++result.removes;
                result.indexesValid = result.indexesValid && op.from < list.size() && list[op.from] == op.key;
                break;
            case KeyedListOpKind::Insert:
                ++result.inserts;
                result.indexesValid = result.indexesValid && op.to <= list.size();
                break;
            case KeyedListOpKind::Move:
                ++result.moves;
                result.indexesValid = result.indexesValid && op.from < list.size() && op.to < list.size() && list[op.from] == op.key && op.from != op.to;
                break;
            }
            if (!ApplyKeyedListOp(list, op))
            {
                result.indexesValid = false;
                return result;
            }
        }
        result.reachesTarget = (list == target);
        size_t vanished = 0;
        for (const auto& key : current)
        {
            vanished += (std::find(target.begin(), target.end(), key) == target.end()) ? 1 : 0;
        }
        size_t added = 0;
        for (const auto& key : target)
        {
            added += (std::find(current.begin(), current.end(), key) == current.end()) ? 1 : 0;
        }
        result.minimal = result.removes == vanished && result.inserts == added && result.moves == MinimumMoves(current, target);
        return result;
    }

    // A tiny deterministic generator (an LCG): the random sweep must be the same on every run.
    struct Lcg
    {
        uint64_t state;
        // returns - the next pseudo-random value in [0, bound); bound must be > 0.
        size_t Next(size_t bound)
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<size_t>((state >> 33) % bound);
        }
    };
}

void TestKeyedListDiff()
{
    std::wprintf(L"\n[TestKeyedListDiff]\n");

    // --- the named shapes a card list actually goes through ---
    {
        const KeyList five{ L"A", L"B", L"C", L"D", L"E" };
        CHECK(PlanKeyedListOps(five, five).empty(), "list diff: an unchanged list plans nothing (no XAML edit at all)");
        CHECK(PlanKeyedListOps({}, {}).empty(), "list diff: empty -> empty plans nothing");

        const auto build = CheckPlan({}, five);
        CHECK(build.reachesTarget && build.indexesValid && build.inserts == 5 && build.removes == 0 && build.moves == 0,
              "list diff: the first build is 5 inserts");
        {
            const auto ops = PlanKeyedListOps({}, five);
            bool frontToBack = ops.size() == 5;
            for (size_t i = 0; frontToBack && i < ops.size(); ++i)
            {
                frontToBack = ops[i].kind == KeyedListOpKind::Insert && ops[i].to == i && ops[i].key == five[i];
            }
            CHECK(frontToBack, "list diff: the first build appends front to back (Insert at 0, 1, 2, ...)");
        }
        const auto clear = CheckPlan(five, {});
        CHECK(clear.reachesTarget && clear.indexesValid && clear.removes == 5 && clear.inserts == 0 && clear.moves == 0,
              "list diff: emptying the list is 5 removes");
        {
            const auto ops = PlanKeyedListOps(five, {});
            bool highestFirst = ops.size() == 5;
            for (size_t i = 0; highestFirst && i < ops.size(); ++i)
            {
                highestFirst = ops[i].kind == KeyedListOpKind::Remove && ops[i].from == 4 - i;
            }
            CHECK(highestFirst, "list diff: removes run from the highest index down (each index stays valid)");
        }

        // MOST ACTIVE: the session that just worked jumps to the top - ONE move.
        const auto toFront = CheckPlan(five, { L"E", L"A", L"B", L"C", L"D" });
        CHECK(toFront.reachesTarget && toFront.indexesValid && toFront.moves == 1 && toFront.inserts == 0 && toFront.removes == 0,
              "list diff: a card moving to the top is ONE move");
        // A running session stops and sinks to the bottom - still ONE move (a forward walk would make 4).
        const auto toBack = CheckPlan(five, { L"B", L"C", L"D", L"E", L"A" });
        CHECK(toBack.reachesTarget && toBack.indexesValid && toBack.moves == 1, "list diff: a card sinking to the bottom is ONE move");
        const auto middle = CheckPlan(five, { L"A", L"D", L"B", L"C", L"E" });
        CHECK(middle.reachesTarget && middle.indexesValid && middle.moves == 1, "list diff: a card moving up inside the list is ONE move");
        const auto swap = CheckPlan(five, { L"B", L"A", L"C", L"D", L"E" });
        CHECK(swap.reachesTarget && swap.indexesValid && swap.moves == 1, "list diff: two neighbours swapping is ONE move");
        const auto reversed = CheckPlan(five, { L"E", L"D", L"C", L"B", L"A" });
        CHECK(reversed.reachesTarget && reversed.indexesValid && reversed.moves == 4, "list diff: a full reversal of 5 is 4 moves (the minimum)");

        // A new session's card at the top (a launch), a closed one leaving (a close), both at once.
        const auto launch = CheckPlan(five, { L"N", L"A", L"B", L"C", L"D", L"E" });
        CHECK(launch.reachesTarget && launch.indexesValid && launch.inserts == 1 && launch.moves == 0 && launch.removes == 0,
              "list diff: a new card at the top is one insert and nothing else");
        const auto close = CheckPlan(five, { L"A", L"B", L"D", L"E" });
        CHECK(close.reachesTarget && close.indexesValid && close.removes == 1 && close.moves == 0 && close.inserts == 0,
              "list diff: a closed card is one remove and nothing else");
        const auto mixed = CheckPlan({ L"A", L"B", L"C" }, { L"X", L"C", L"A", L"Y" });
        CHECK(mixed.reachesTarget && mixed.indexesValid && mixed.minimal && mixed.removes == 1 && mixed.inserts == 2 && mixed.moves == 1,
              "list diff: remove + inserts + a move together, minimal");
        // A filter keystroke: most cards leave, the rest keep their order - removes only.
        const auto filtered = CheckPlan(five, { L"B", L"D" });
        CHECK(filtered.reachesTarget && filtered.indexesValid && filtered.removes == 3 && filtered.moves == 0 && filtered.inserts == 0,
              "list diff: narrowing the filter only removes cards");
        const auto unfiltered = CheckPlan({ L"B", L"D" }, five);
        CHECK(unfiltered.reachesTarget && unfiltered.indexesValid && unfiltered.inserts == 3 && unfiltered.moves == 0 && unfiltered.removes == 0,
              "list diff: clearing the filter only inserts cards");
    }

    // --- ApplyKeyedListOp refuses an out-of-range op and leaves the list as it was ---
    {
        KeyList list{ L"A", L"B" };
        CHECK(!ApplyKeyedListOp(list, KeyedListOp{ KeyedListOpKind::Remove, 2, 0, L"X" }), "apply: a Remove past the end is refused");
        CHECK(!ApplyKeyedListOp(list, KeyedListOp{ KeyedListOpKind::Insert, 0, 3, L"X" }), "apply: an Insert past size is refused");
        CHECK(!ApplyKeyedListOp(list, KeyedListOp{ KeyedListOpKind::Move, 0, 2, L"A" }), "apply: a Move to size is refused (post-move index < size)");
        CHECK(list == KeyList({ L"A", L"B" }), "apply: a refused op changes nothing");
        CHECK(ApplyKeyedListOp(list, KeyedListOp{ KeyedListOpKind::Insert, 0, 2, L"C" }) && list == KeyList({ L"A", L"B", L"C" }),
              "apply: an Insert AT size appends");
        CHECK(ApplyKeyedListOp(list, KeyedListOp{ KeyedListOpKind::Move, 0, 2, L"A" }) && list == KeyList({ L"B", L"C", L"A" }),
              "apply: Move(from, to) leaves the element AT `to` (XAML UIElementCollection.Move semantics)");
        CHECK(ApplyKeyedListOp(list, KeyedListOp{ KeyedListOpKind::Move, 2, 0, L"A" }) && list == KeyList({ L"A", L"B", L"C" }),
              "apply: a backward Move too");
    }

    // --- exhaustive: every permutation of 5 keys, from the identity ---
    {
        KeyList current{ L"A", L"B", L"C", L"D", L"E" };
        KeyList target = current;
        size_t cases = 0;
        size_t failures = 0;
        do
        {
            const auto c = CheckPlan(current, target);
            ++cases;
            failures += (c.reachesTarget && c.indexesValid && c.minimal && c.removes == 0 && c.inserts == 0) ? 0 : 1;
        } while (std::next_permutation(target.begin(), target.end()));
        CHECK(cases == 120 && failures == 0, "list diff: all 120 permutations of 5 cards reach the target with the minimum of moves");
    }

    // --- random sweep: a 9-key universe, random subsets in random orders on both sides ---
    {
        const KeyList universe{ L"k0", L"k1", L"k2", L"k3", L"k4", L"k5", L"k6", L"k7", L"k8" };
        Lcg rng{ 0x5EED5EEDull };
        // A random subset of `universe` in a random order.
        const auto randomList = [&]() {
            KeyList pool = universe;
            for (size_t i = pool.size(); i > 1; --i)
            {
                std::swap(pool[i - 1], pool[rng.Next(i)]); // Fisher-Yates
            }
            pool.resize(rng.Next(pool.size() + 1));
            return pool;
        };
        size_t failures = 0;
        size_t moved = 0;
        for (int round = 0; round < 3000; ++round)
        {
            const KeyList current = randomList();
            const KeyList target = randomList();
            const auto c = CheckPlan(current, target);
            failures += (c.reachesTarget && c.indexesValid && c.minimal) ? 0 : 1;
            moved += c.moves;
        }
        CHECK(failures == 0, "list diff: 3000 random (current, target) pairs - every plan reaches its target, in range, minimal");
        CHECK(moved > 0, "list diff: the sweep exercised moves (not only inserts/removes)");
    }

    // --- a big list: the release's 466-card window, one card jumping from the bottom to the top ---
    {
        KeyList current;
        for (int i = 0; i < 466; ++i)
        {
            current.push_back(L"s" + std::to_wstring(i));
        }
        KeyList target = current;
        std::rotate(target.begin(), target.end() - 1, target.end());
        const auto c = CheckPlan(current, target);
        CHECK(c.reachesTarget && c.indexesValid && c.moves == 1 && c.removes == 0 && c.inserts == 0,
              "list diff: 466 cards, the last one jumps to the top - ONE move");
        KeyList sunk = current;
        std::rotate(sunk.begin(), sunk.begin() + 1, sunk.end());
        const auto d = CheckPlan(current, sunk);
        CHECK(d.reachesTarget && d.indexesValid && d.moves == 1, "list diff: 466 cards, the first one sinks to the bottom - ONE move");
    }
}

namespace
{
    // One expected timing text: counts a CHECK, and on a mismatch prints what came out next to what was wanted (the
    // CHECK line alone names only the case).
    //   got  - the formatted text.
    //   want - the expected text.
    //   what - the case's description for the [FAIL] line.
    // Returns nothing. Throws nothing the harness does not already allow (wprintf).
    void ExpectTimingText(const std::wstring& got, const wchar_t* want, const char* what)
    {
        if (got != want)
        {
            std::wprintf(L"    got \"%ls\", want \"%ls\"\n", got.c_str(), want);
        }
        CHECK(got == want, what);
    }
}

// The session timing line of the side-panel cards, the Triage Board's cards and the Explorer tree's rows
// (AgentMaster/SessionTimingText.h): "created 4d1h, active 2m4s ago", replacing the unreadable "-7h5m/7h6m/2m4s"
// (2026-10-05). Pins the user's rules on a fixed clock: "created" in years / days / hours only, "created recently"
// under an hour, "created now" under 5 minutes; "active ... ago" in y / d / h / m / s (seconds only under an hour) and
// left out when it is 0; no month unit, so 'm' is minutes only; and the unknown / clock-skew edges.
void TestSessionTimingText()
{
    std::wprintf(L"\n[TestSessionTimingText]\n");
    constexpr int64_t s = kTimingMsPerSecond;
    constexpr int64_t m = kTimingMsPerMinute;
    constexpr int64_t h = kTimingMsPerHour;
    constexpr int64_t d = kTimingMsPerDay;
    constexpr int64_t y = kTimingMsPerYear;
    constexpr int64_t now = 1'800'000'000'000; // a fixed instant (2027-01-15): every case below is relative to it

    // --- the user's own examples ---
    ExpectTimingText(FormatSessionTimingLine(now - (4 * d + 1 * h + 30 * m + 15 * s), now - (2 * m + 4 * s), now),
                     L"created 4d1h, active 2m4s ago", "timing: the user's example - created 4d1h (minutes cut), active 2m4s ago");
    ExpectTimingText(FormatSessionTimingLine(now - (1 * y + 4 * d + 1 * h + 20 * m), now, now),
                     L"created 1y4d1h", "timing: the user's example - created 1y4d1h, the active part left out when it is 0");

    // --- created: the words under an hour, then years / days / hours ---
    ExpectTimingText(FormatSessionTimingLine(now, 0, now), L"created now", "timing: created this instant -> \"created now\"");
    ExpectTimingText(FormatSessionTimingLine(now - (4 * m + 59 * s), 0, now), L"created now", "timing: created 4m59s ago -> still \"now\" (< 5 min)");
    ExpectTimingText(FormatSessionTimingLine(now - 5 * m, 0, now), L"created recently", "timing: created exactly 5 min ago -> \"recently\" (the boundary)");
    ExpectTimingText(FormatSessionTimingLine(now - (59 * m + 59 * s), 0, now), L"created recently", "timing: created 59m59s ago -> still \"recently\" (< 1 h)");
    ExpectTimingText(FormatSessionTimingLine(now - 1 * h, 0, now), L"created 1h", "timing: created exactly 1 h ago -> \"1h\" (the boundary)");
    ExpectTimingText(FormatSessionTimingLine(now - (1 * h + 59 * m + 59 * s), 0, now), L"created 1h", "timing: created 1h59m59s ago -> \"1h\" (cut, never rounded up)");
    ExpectTimingText(FormatSessionTimingLine(now - (45 * d + 3 * h), 0, now), L"created 45d3h", "timing: 45 days -> \"45d3h\" (no month unit)");
    ExpectTimingText(FormatSessionTimingLine(now - (1 * y + 5 * h), 0, now), L"created 1y5h", "timing: a zero day count is left out -> \"1y5h\"");
    ExpectTimingText(FormatSessionTimingLine(now - (2 * y + 30 * m), 0, now), L"created 2y", "timing: whole years -> \"2y\"");
    ExpectTimingText(FormatSessionTimingLine(now + 10 * m, 0, now), L"created now", "timing: a creation stamp in the future (clock skew) reads \"now\"");

    // --- active ... ago: seconds under an hour, minutes as the floor above it, left out when 0 ---
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, 0, now), L"created 2d", "timing: no recorded activity (0) -> no active part");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - 999, now), L"created 2d", "timing: activity under a second ago is the 0 that is left out");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now + 5 * s, now), L"created 2d", "timing: an activity stamp in the future reads as 0 and is left out");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - 1 * s, now), L"created 2d, active 1s ago", "timing: activity 1 s ago");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - 45 * s, now), L"created 2d, active 45s ago", "timing: activity 45 s ago");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - 1 * m, now), L"created 2d, active 1m ago", "timing: a zero second count is left out -> \"1m\"");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - (59 * m + 59 * s), now), L"created 2d, active 59m59s ago", "timing: just under an hour keeps its seconds");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - (1 * h + 30 * s), now), L"created 2d, active 1h ago", "timing: from an hour on the seconds go -> \"1h\"");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * d, now - (2 * h + 30 * m + 59 * s), now), L"created 2d, active 2h30m ago", "timing: hours and minutes");
    ExpectTimingText(FormatSessionTimingLine(now - 9 * d, now - (3 * d + 2 * h + 15 * m), now), L"created 9d, active 3d2h15m ago", "timing: days, hours and minutes");
    ExpectTimingText(FormatSessionTimingLine(now - 2 * y, now - 400 * d, now), L"created 2y, active 1y35d ago", "timing: years in the active part too");
    ExpectTimingText(FormatSessionTimingLine(now - 3 * m, now - 12 * s, now), L"created now, active 12s ago", "timing: \"created now\" with its activity");
    ExpectTimingText(FormatSessionTimingLine(now - 20 * m, now - (4 * m + 4 * s), now), L"created recently, active 4m4s ago", "timing: \"created recently\" with its activity");

    // --- unknown creation, and an activity stamp older than the creation ---
    ExpectTimingText(FormatSessionTimingLine(0, now - 10 * s, now), L"", "timing: no creation time -> no line at all (a never-prompted session)");
    ExpectTimingText(FormatSessionTimingLine(-5, now - 10 * s, now), L"", "timing: a negative creation time is unknown too");
    ExpectTimingText(FormatSessionTimingLine(now - 3 * h, now - 5 * h, now), L"created 3h, active 3h ago",
                     "timing: activity before the creation is raised to the creation (the old line's clamp)");

    // --- the units, swept: 'm' only ever means minutes, the created span never shows minutes or seconds, the
    //     active span never shows seconds from an hour on, and neither span is ever empty where a value is due ---
    {
        size_t createdBad = 0;
        size_t activeBad = 0;
        size_t lineBad = 0;
        size_t cases = 0;
        for (int64_t age = 0; age < 3 * y; age += 7919 * s + 13) // a prime stride: lands on every unit mix
        {
            ++cases;
            const std::wstring created = FormatCreatedSpan(age);
            if (age >= kCreatedRecentlyMs)
            {
                // A number: digits and y / d / h only, never empty.
                const bool ok = !created.empty() && created.find_first_not_of(L"0123456789ydh") == std::wstring::npos;
                createdBad += ok ? 0 : 1;
            }
            else
            {
                createdBad += (created == (age < kCreatedNowMs ? L"now" : L"recently")) ? 0 : 1;
            }
            const std::wstring active = FormatActiveAgoSpan(age);
            if (age < kTimingMsPerSecond)
            {
                activeBad += active.empty() ? 0 : 1;
            }
            else
            {
                const bool allowedChars = active.find_first_not_of(L"0123456789ydhms") == std::wstring::npos;
                const bool secondsOk = age < kTimingMsPerHour || active.find(L's') == std::wstring::npos;
                activeBad += (!active.empty() && allowedChars && secondsOk) ? 0 : 1;
            }
            const std::wstring line = FormatSessionTimingLine(now - age, now - age / 2, now);
            lineBad += (line.rfind(L"created ", 0) == 0) ? 0 : 1;
        }
        CHECK(cases > 10000, "timing sweep: covered more than 10000 ages across 3 years");
        CHECK(createdBad == 0, "timing sweep: the created span is \"now\" / \"recently\" under an hour, else only y / d / h - never minutes or seconds");
        CHECK(activeBad == 0, "timing sweep: the active span is empty under a second, else y / d / h / m (+ s only under an hour), never empty");
        CHECK(lineBad == 0, "timing sweep: every line with a creation time starts \"created \"");
    }
}

namespace
{
    // One placement case: checks all four results and prints what came back when any of them differs, so a failing
    // case names its numbers instead of only its label.
    //   got   - the placement under test.
    //   left  - the expected left edge.
    //   top   - the expected top edge.
    //   right - the expected towardRight.
    //   down  - the expected towardDown.
    //   what  - the case's label (the CHECK message).
    void ExpectCardPlacement(const PointerCardPlacement& got, double left, double top, bool right, bool down, const char* what)
    {
        const bool ok = std::fabs(got.left - left) < 1e-9 && std::fabs(got.top - top) < 1e-9 && got.towardRight == right && got.towardDown == down;
        if (!ok)
        {
            std::wprintf(L"    got left=%g top=%g right=%d down=%d, want left=%g top=%g right=%d down=%d\n",
                         got.left,
                         got.top,
                         got.towardRight ? 1 : 0,
                         got.towardDown ? 1 : 0,
                         left,
                         top,
                         right ? 1 : 0,
                         down ? 1 : 0);
        }
        CHECK(ok, what);
    }

    // One axis of the sweep's invariants: a card that fits lands inside [margin, area - margin - size] and, when its
    // unclamped position is inside that band too, sits exactly `gap` away from the click; a card that doesn't fit is
    // pinned to the near margin.
    //   start      - the placed edge (left / top).
    //   size       - the card's extent on the axis.
    //   area       - the window's extent on the axis (measured, > 0).
    //   click      - the click coordinate.
    //   forward    - the direction the placement took (right / down).
    //   outside    - incremented for a placement outside the band (or not pinned when too large).
    //   notAtClick - incremented for an unclamped placement that is not gap away from the click.
    void CheckCardAxis(double start, double size, double area, double click, bool forward, size_t& outside, size_t& notAtClick)
    {
        const double lo = kPointerCardMarginDips;
        const double hi = area - kPointerCardMarginDips - size;
        if (hi < lo)
        {
            outside += std::fabs(start - lo) < 1e-9 ? 0 : 1; // too large: pinned to the near margin
            return;
        }
        outside += (start >= lo - 1e-9 && start <= hi + 1e-9) ? 0 : 1;
        const double unclamped = forward ? (click + kPointerCardGapDips) : (click - kPointerCardGapDips - size);
        if (unclamped >= lo && unclamped <= hi)
        {
            notAtClick += std::fabs(start - unclamped) < 1e-9 ? 0 : 1; // inside the window: exactly at the click
        }
    }
}

// Where the side panel's "+" launcher card opens (AgentMaster/PointerCardPlacement.h): "cornered to the cursor" and
// extending toward the window's center, the user's tooltip-like rule (2026-10-06), kept inside the window.
void TestPointerCardPlacement()
{
    std::wprintf(L"\n[TestPointerCardPlacement]\n");
    const double nan = std::nan("");
    const double inf = HUGE_VAL;

    // --- the four quadrants of a 1600 x 900 window, a 600 x 60 card (default gap 2, margin 8) ---
    ExpectCardPlacement(PlacePointerCard(40, 60, 600, 60, 1600, 900), 42, 62, true, true,
                        "card placement: a click near the top-left corner opens down-right, the corner just past the click");
    ExpectCardPlacement(PlacePointerCard(1560, 60, 600, 60, 1600, 900), 958, 62, false, true,
                        "card placement: near the top-right corner it opens down-left (the card's RIGHT edge at the click)");
    ExpectCardPlacement(PlacePointerCard(40, 880, 600, 60, 1600, 900), 42, 818, true, false,
                        "card placement: near the bottom-left corner it opens up-right (the card's BOTTOM edge at the click)");
    ExpectCardPlacement(PlacePointerCard(1560, 880, 600, 60, 1600, 900), 958, 818, false, false,
                        "card placement: near the bottom-right corner it opens up-left");

    // --- the middle line counts as the left / top half ---
    ExpectCardPlacement(PlacePointerCard(800, 450, 600, 60, 1600, 900), 802, 452, true, true,
                        "card placement: a click exactly on the center lines extends right and down");
    ExpectCardPlacement(PlacePointerCard(800.5, 450.5, 600, 60, 1600, 900), 198.5, 388.5, false, false,
                        "card placement: half a DIP past the center lines flips both axes");

    // --- the clamp: an edge the card would cross stops at the margin ---
    ExpectCardPlacement(PlacePointerCard(300, 40, 800, 60, 1000, 900), 192, 42, true, true,
                        "card placement: a rightward card that would cross the right edge stops 8 DIP inside it");
    ExpectCardPlacement(PlacePointerCard(700, 40, 800, 60, 1000, 900), 8, 42, false, true,
                        "card placement: a leftward card that would cross the left edge stops 8 DIP inside it");
    ExpectCardPlacement(PlacePointerCard(40, 500, 300, 700, 1600, 800), 42, 8, true, false,
                        "card placement: an upward card taller than the room above stops at the top margin");
    ExpectCardPlacement(PlacePointerCard(40, 300, 300, 700, 1600, 800), 42, 92, true, true,
                        "card placement: a downward card that would cross the bottom edge stops 8 DIP above it");

    // --- a card larger than the window is pinned to the near (left / top) margin, whatever the click ---
    ExpectCardPlacement(PlacePointerCard(100, 50, 1200, 60, 1000, 900), 8, 52, true, true,
                        "card placement: a card wider than the window starts at the left margin (clicked on the left)");
    ExpectCardPlacement(PlacePointerCard(900, 50, 1200, 60, 1000, 900), 8, 52, false, true,
                        "card placement: ... and when clicked on the right (its start stays visible)");
    ExpectCardPlacement(PlacePointerCard(100, 50, 984, 60, 1000, 900), 8, 52, true, true,
                        "card placement: a card exactly as wide as the window minus both margins sits at the margin");

    // --- an unmeasured window: no clamp, and no middle to flip on (forward = right / down) ---
    ExpectCardPlacement(PlacePointerCard(300, 40, 600, 60, 0, 0), 302, 42, true, true,
                        "card placement: an unmeasured window (0 x 0) is not clamped");
    ExpectCardPlacement(PlacePointerCard(5000, 4000, 600, 60, 0, 0), 5002, 4002, true, true,
                        "card placement: ... even far outside, and it extends right / down (no middle to compare with)");
    ExpectCardPlacement(PlacePointerCard(2000, 60, 600, 60, nan, -1), 2002, 62, true, true,
                        "card placement: a NaN or negative window size reads as unmeasured");

    // --- non-finite and negative inputs are neutralized (no NaN can reach a XAML offset) ---
    ExpectCardPlacement(PlacePointerCard(nan, nan, 600, 60, 1600, 900), 8, 8, true, true,
                        "card placement: a NaN click reads as (0, 0): right / down, clamped to the margins");
    ExpectCardPlacement(PlacePointerCard(inf, -inf, 600, 60, 1600, 900), 8, 8, true, true,
                        "card placement: an infinite click reads as (0, 0) too");
    ExpectCardPlacement(PlacePointerCard(40, 60, -50, nan, 1600, 900), 42, 62, true, true,
                        "card placement: a negative or NaN card size reads as 0");
    ExpectCardPlacement(PlacePointerCard(40, 60, 600, 60, 1600, 900, -5, -5), 40, 60, true, true,
                        "card placement: a negative gap and margin read as 0 (the corner exactly on the click)");
    ExpectCardPlacement(PlacePointerCard(1590, 10, 600, 60, 1600, 900, 10, 20), 980, 20, false, true,
                        "card placement: a custom gap (10) and margin (20) both apply");

    // --- a seeded sweep of the invariants ---
    {
        uint64_t seed = 0x9E3779B97F4A7C15ull;
        // splitmix64 -> [0, span): deterministic, so a failure reproduces exactly.
        const auto next = [&seed](double span) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return span * static_cast<double>(z >> 11) / 9007199254740992.0;
        };
        size_t outside = 0; // a fitting card placed outside [margin, area - margin - size], or a too-large one not pinned
        size_t notAtClick = 0; // an unclamped card whose corner is not gap away from the click
        size_t wrongDirection = 0; // a direction that disagrees with the click's half
        size_t cases = 0;
        for (int i = 0; i < 20000; ++i)
        {
            const double aw = 200.0 + next(3000.0);
            const double ah = 150.0 + next(2000.0);
            const double cw = next(1500.0);
            const double ch = next(400.0);
            const double x = next(aw);
            const double y = next(ah);
            const auto p = PlacePointerCard(x, y, cw, ch, aw, ah);
            ++cases;
            wrongDirection += (p.towardRight == (x <= aw / 2.0) && p.towardDown == (y <= ah / 2.0)) ? 0 : 1;
            CheckCardAxis(p.left, cw, aw, x, p.towardRight, outside, notAtClick);
            CheckCardAxis(p.top, ch, ah, y, p.towardDown, outside, notAtClick);
        }
        CHECK(cases == 20000, "card placement sweep: 20000 random clicks, cards and windows");
        CHECK(wrongDirection == 0, "card placement sweep: every direction follows the click's half of the window");
        CHECK(outside == 0, "card placement sweep: a card that fits always lands inside the margins; one that doesn't is pinned to the near margin");
        CHECK(notAtClick == 0, "card placement sweep: an unclamped card always has its corner exactly gap away from the click");
    }
}

namespace
{
    // One submenu placement case: checks the three results and prints what came back when any differs, so a failing case
    // names its numbers instead of only its label (ExpectCardPlacement's rule).
    //   got     - the placement under test.
    //   left    - the expected left edge.
    //   top     - the expected top edge.
    //   onRight - the expected side.
    //   what    - the case's label (the CHECK message).
    void ExpectSubmenuPlacement(const SubmenuCardPlacement& got, double left, double top, bool onRight, const char* what)
    {
        const bool ok = std::fabs(got.left - left) < 1e-9 && std::fabs(got.top - top) < 1e-9 && got.onRight == onRight;
        if (!ok)
        {
            std::wprintf(L"    got left=%g top=%g right=%d, want left=%g top=%g right=%d\n", got.left, got.top, got.onRight ? 1 : 0, left, top, onRight ? 1 : 0);
        }
        CHECK(ok, what);
    }
}

// Where By Folder's hover submenu opens (AgentMaster/PointerCardPlacement.h PlaceSubmenuCard, SIDE_PANEL.md section 2l "The
// By Folder submenu"): beside the panel's right edge, else its left edge, else on the roomier side clamped into the window;
// its top level with the hovered row, clamped; an unmeasured window never clamped; every NaN / negative input neutralized;
// plus a seeded sweep of the invariants (inside the window, and never over the panel while a side holds the whole card).
void TestSubmenuCardPlacement()
{
    std::wprintf(L"\n[TestSubmenuCardPlacement]\n");
    const double nan = std::nan("");

    // --- the sides: a 400 x 300 submenu in a 1600 x 900 window (default gap 2, margin 8) ---
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, 40, 400, 300, 1600, 900), 352, 40, true,
                           "submenu placement: room on the right: beside the panel's right edge, 2 DIP off, its top as asked");
    ExpectSubmenuPlacement(PlaceSubmenuCard(1200, 1450, 40, 400, 300, 1600, 900), 798, 40, false,
                           "submenu placement: no room on the right: beside the panel's left edge");
    ExpectSubmenuPlacement(PlaceSubmenuCard(940, 1190, 40, 400, 300, 1600, 900), 1192, 40, true,
                           "submenu placement: a card ending exactly at the right margin still opens on the right");
    ExpectSubmenuPlacement(PlaceSubmenuCard(940, 1191, 40, 400, 300, 1600, 900), 538, 40, false,
                           "submenu placement: one DIP more and it opens on the left");

    // --- neither side holds the whole card: the roomier side, clamped into the window ---
    ExpectSubmenuPlacement(PlaceSubmenuCard(150, 400, 40, 400, 300, 700, 900), 292, 40, true,
                           "submenu placement: neither side fits: the roomier right side, clamped 8 DIP inside the right edge");
    ExpectSubmenuPlacement(PlaceSubmenuCard(300, 550, 40, 400, 300, 700, 900), 8, 40, false,
                           "submenu placement: neither side fits: the roomier left side, clamped to the left margin");
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, 40, 900, 300, 600, 900), 8, 40, true,
                           "submenu placement: a card wider than the window is pinned to the left margin (its start stays visible)");
    ExpectSubmenuPlacement(PlaceSubmenuCard(995, 1245, 40, 400, 300, 1000, 900), 592, 40, false,
                           "submenu placement: a panel reaching past the right edge: the left side would end 1 DIP too far right, so it is clamped");

    // --- vertically: level with the row, clamped ---
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, 800, 400, 300, 1600, 900), 352, 592, true,
                           "submenu placement: a row near the bottom: the card rises to stay 8 DIP above the bottom edge");
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, -20, 400, 300, 1600, 900), 352, 8, true,
                           "submenu placement: a top above the window is clamped to the top margin");
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, 300, 400, 1000, 1600, 900), 352, 8, true,
                           "submenu placement: a card taller than the window is pinned to the top margin");

    // --- an unmeasured window: no clamp, the right side ---
    ExpectSubmenuPlacement(PlaceSubmenuCard(1200, 1450, 5000, 400, 300, 0, 0), 1452, 5000, true,
                           "submenu placement: an unmeasured window (0 x 0) is not clamped and opens on the right");
    ExpectSubmenuPlacement(PlaceSubmenuCard(1200, 1450, 40, 400, 300, nan, -1), 1452, 40, true,
                           "submenu placement: a NaN or negative window size reads as unmeasured");

    // --- non-finite and negative inputs are neutralized (no NaN can reach a XAML offset) ---
    ExpectSubmenuPlacement(PlaceSubmenuCard(nan, nan, nan, 400, 300, 1600, 900), 8, 8, true,
                           "submenu placement: a NaN parent and top read as 0 (the card's left edge, 2 DIP off, is inside the margin: clamped to it)");
    ExpectSubmenuPlacement(PlaceSubmenuCard(350, 100, 40, 400, 300, 1600, 900), 352, 40, true,
                           "submenu placement: a parent whose right edge lies left of its left edge reads as a parent of no width");
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, 40, -5, nan, 1600, 900), 352, 40, true,
                           "submenu placement: a negative or NaN card size reads as 0");
    ExpectSubmenuPlacement(PlaceSubmenuCard(100, 350, 40, 400, 300, 1600, 900, -5, -5), 350, 40, true,
                           "submenu placement: a negative gap and margin read as 0 (the card touching the panel)");
    ExpectSubmenuPlacement(PlaceSubmenuCard(1180, 1400, 10, 400, 300, 1600, 900, 6, 20), 774, 20, false,
                           "submenu placement: a custom gap (6) and margin (20) both apply");

    // --- a seeded sweep of the invariants ---
    {
        uint64_t seed = 0xD1B54A32D192ED03ull;
        // splitmix64 -> [0, span): deterministic, so a failure reproduces exactly.
        const auto next = [&seed](double span) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return span * static_cast<double>(z >> 11) / 9007199254740992.0;
        };
        const double m = kPointerCardMarginDips;
        const double g = kPointerCardGapDips;
        size_t outside = 0; // a card that fits the window placed outside [margin, area - margin - size], or one too large not pinned
        size_t overParent = 0; // a card over the panel although one side holds it whole
        size_t notPreferred = 0; // a card on the left although the right side holds it, or not exactly gap off the side it took
        size_t cases = 0;
        for (int i = 0; i < 20000; ++i)
        {
            const double aw = 300.0 + next(3000.0);
            const double ah = 200.0 + next(1600.0);
            const double pw = 50.0 + next(400.0);
            const double pl = next(aw);
            const double pr = pl + pw;
            const double w = next(900.0);
            const double h = next(700.0);
            const double t = next(ah * 1.2) - ah * 0.1;
            const auto p = PlaceSubmenuCard(pl, pr, t, w, h, aw, ah);
            ++cases;
            const auto axisOk = [&](double start, double size, double area) {
                const double hi = area - m - size;
                return hi < m ? std::fabs(start - m) < 1e-9 : (start >= m - 1e-9 && start <= hi + 1e-9);
            };
            outside += (axisOk(p.left, w, aw) && axisOk(p.top, h, ah)) ? 0 : 1;
            // A side holds the card when the whole card lies inside the margins there (the parent may reach past the right
            // edge in this sweep: pl is anywhere in the window and pw is added to it).
            const bool fitsRight = pr + g >= m && pr + g + w <= aw - m;
            const bool fitsLeft = pl - g - w >= m && pl - g <= aw - m;
            if (fitsRight || fitsLeft)
            {
                overParent += (p.left >= pr + g - 1e-9 || p.left + w <= pl - g + 1e-9) ? 0 : 1;
                const bool expectRight = fitsRight;
                const double expectLeft = expectRight ? pr + g : pl - g - w;
                notPreferred += (p.onRight == expectRight && std::fabs(p.left - expectLeft) < 1e-9) ? 0 : 1;
            }
        }
        CHECK(cases == 20000, "submenu placement sweep: 20000 random panels, cards, rows and windows");
        CHECK(outside == 0, "submenu placement sweep: a card that fits the window always lands inside its margins; one that doesn't is pinned to the near margin");
        CHECK(overParent == 0, "submenu placement sweep: while a side holds the whole card, it never covers the panel");
        CHECK(notPreferred == 0, "submenu placement sweep: the right side whenever it holds the card, else the left, each exactly gap off the panel");
    }
}

namespace
{
    // One row-range case: checks both ends and prints what came back when they differ, so a failing case names its
    // numbers instead of only its label.
    //   got   - the range under test.
    //   first - the expected first row.
    //   last  - the expected one-past-the-last row.
    //   what  - the case's label (the CHECK message).
    void ExpectRows(const RowRange& got, size_t first, size_t last, const char* what)
    {
        const bool ok = got.first == first && got.last == last;
        if (!ok)
        {
            std::wprintf(L"    got [%zu, %zu), want [%zu, %zu)\n", got.first, got.last, first, last);
        }
        CHECK(ok, what);
    }

    // A deterministic random source for the sweeps (splitmix64), so a failure reproduces exactly from its seed.
    struct SweepRandom
    {
        uint64_t state;

        // A double in [0, span) (0 for span <= 0). Never throws.
        //   span    - the upper bound (exclusive).
        //   returns - the next value.
        double Next(double span) noexcept
        {
            state += 0x9E3779B97F4A7C15ull;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return span > 0.0 ? span * static_cast<double>(z >> 11) / 9007199254740992.0 : 0.0;
        }

        // An index in [0, count) (0 for count 0). The product can round up to `count` exactly for 1 draw in 2^53, so
        // the result is clamped. Never throws.
        //   count   - the number of indexes.
        //   returns - the next index.
        size_t NextIndex(size_t count) noexcept
        {
            if (count == 0)
            {
                return 0;
            }
            return (std::min)(count - 1, static_cast<size_t>(Next(static_cast<double>(count))));
        }
    };
}

// Which rows of the side panel's VIRTUALIZED card list get cards (AgentMaster/VirtualListWindow.h, SIDE_PANEL.md section
// 2f). The named cases use the release's GLOBAL list where it matters: 706 rows of 60 DIPs under a 600-DIP viewport,
// so the NEAR margin is 300 DIPs (5 rows) and the FAR margin 600 DIPs (10 rows) on each side.
void TestVirtualListWindow()
{
    std::wprintf(L"\n[TestVirtualListWindow]\n");
    const double nan = std::nan("");
    const double inf = HUGE_VAL;

    // --- RowRange: half-open [first, last) ---
    {
        const RowRange r{ 3, 7 };
        const RowRange inverted{ 7, 3 };
        const RowRange none{};
        CHECK(r.Size() == 4 && !r.Empty(), "row range: [3, 7) holds 4 rows");
        CHECK(inverted.Size() == 0 && inverted.Empty() && RowRange{}.Empty(), "row range: an inverted or default range is empty, size 0");
        CHECK(r.Contains(3) && r.Contains(6) && !r.Contains(7) && !r.Contains(2), "row range: Contains is half-open");
        CHECK(r.Covers(RowRange{ 4, 6 }) && r.Covers(r), "row range: a range covers its sub-ranges and itself");
        CHECK(!r.Covers(RowRange{ 2, 6 }) && !r.Covers(RowRange{ 4, 8 }), "row range: a range does not cover one that sticks out on either side");
        CHECK(r.Covers(none) && r.Covers(RowRange{ 9, 9 }) && none.Covers(none), "row range: an empty range is covered by any range (no row must exist)");
        CHECK(!none.Covers(RowRange{ 0, 1 }), "row range: an empty range covers no row");
        const RowRange same{ 3, 7 };
        const RowRange longer{ 3, 8 };
        CHECK(r == same && !(r == longer), "row range: equality compares both ends");
    }

    // --- RowsInSpan: the rows with any part inside [top, bottom), clamped to the list ---
    ExpectRows(RowsInSpan(706, 60, 0, 60), 0, 1, "span: [0, 60) is row 0 alone");
    ExpectRows(RowsInSpan(706, 60, 60, 120), 1, 2, "span: [60, 120) is row 1 alone - the row starting at 120 is outside (half-open)");
    ExpectRows(RowsInSpan(706, 60, 59.9, 60.1), 0, 2, "span: a span across a row boundary touches both rows");
    ExpectRows(RowsInSpan(706, 60, -100, 100), 0, 2, "span: a span starting above the content is clamped to row 0");
    ExpectRows(RowsInSpan(706, 60, 42000, 99999), 700, 706, "span: a span running past the end is clamped to the last row");
    CHECK(RowsInSpan(706, 60, -500, -100).Empty(), "span: a span above the content holds no row");
    CHECK(RowsInSpan(706, 60, 50000, 60000).Empty(), "span: a span below the content holds no row");
    CHECK(RowsInSpan(706, 60, 100, 100).Empty() && RowsInSpan(706, 60, 100, 50).Empty(), "span: an empty or inverted span holds no row");
    CHECK(RowsInSpan(0, 60, 0, 600).Empty(), "span: an empty list has no rows");
    ExpectRows(RowsInSpan(706, 0, 0, 10), 0, 10, "span: a zero stride counts as 1 DIP");
    ExpectRows(RowsInSpan(706, nan, 0, 10), 0, 10, "span: a NaN stride counts as 1 DIP");
    ExpectRows(RowsInSpan(706, -60, 0, 10), 0, 10, "span: a negative stride counts as 1 DIP");
    CHECK(RowsInSpan(706, 60, nan, 600).Empty() && RowsInSpan(706, 60, 0, inf).Empty() && RowsInSpan(706, 60, -inf, 600).Empty(),
          "span: a non-finite edge holds no row");

    // --- ClampViewTop: [0, count * stride - viewHeight] ---
    CHECK(ClampViewTop(706, 60, 1234.5, 600) == 1234.5, "clamp: an offset inside the content is kept");
    CHECK(ClampViewTop(706, 60, 50000, 600) == 41760, "clamp: an offset past the end is clamped to the last full viewport (42360 - 600)");
    CHECK(ClampViewTop(706, 60, -10, 600) == 0 && ClampViewTop(706, 60, nan, 600) == 0, "clamp: a negative or NaN offset reads as 0");
    CHECK(ClampViewTop(5, 60, 100, 600) == 0, "clamp: a list shorter than its viewport cannot scroll");
    CHECK(ClampViewTop(706, 60, 50000, 0) == 41640, "clamp: a viewport with no height yet counts as 720 DIPs (42360 - 720)");

    // --- WidenedViewportRows: the building block of NEAR and FAR ---
    ExpectRows(WidenedViewportRows(706, 60, 6000, 600, 0, 0), 100, 110, "widened: no margin = the visible rows 100..109");
    ExpectRows(WidenedViewportRows(706, 60, 6000, 600, kVirtualRowsNearViewports, kVirtualRowsNearMinRows), 95, 115,
               "widened: NEAR adds half a viewport (300 DIPs = 5 rows) on each side");
    ExpectRows(WidenedViewportRows(706, 60, 6000, 600, kVirtualRowsFarViewports, kVirtualRowsFarMinRows), 90, 120,
               "widened: FAR adds a viewport (600 DIPs = 10 rows) on each side");
    ExpectRows(WidenedViewportRows(706, 60, 6000, 100, kVirtualRowsNearViewports, kVirtualRowsNearMinRows), 96, 106,
               "widened: under a short viewport NEAR is 4 rows on each side, not half a viewport");
    ExpectRows(WidenedViewportRows(706, 60, 6000, 100, kVirtualRowsFarViewports, kVirtualRowsFarMinRows), 92, 110,
               "widened: ... and FAR 8 rows");

    // --- PlanRealizedRows: keep the range while it covers NEAR, else realize FAR ---
    const RowRange firstPlan = PlanRealizedRows(706, 60, 0, 600, RowRange{});
    ExpectRows(firstPlan, 0, 20, "plan: a list's first plan realizes FAR: the 10 visible rows + 10 below");
    CHECK(PlanRealizedRows(706, 60, 300, 600, firstPlan) == firstPlan, "plan: half a viewport down, the range still covers NEAR - kept");
    ExpectRows(PlanRealizedRows(706, 60, 301, 600, firstPlan), 0, 26, "plan: one DIP further NEAR needs row 20 - the new viewport's FAR rows");
    ExpectRows(PlanRealizedRows(706, 60, 361, 600, firstPlan), 0, 27, "plan: ... at 361 DIPs FAR reaches row 26");
    const RowRange middle = PlanRealizedRows(706, 60, 6000, 600, firstPlan);
    ExpectRows(middle, 90, 120, "plan: a jump to row 100 replaces the range with that viewport's FAR rows");
    CHECK(PlanRealizedRows(706, 60, 5700, 600, middle) == middle && PlanRealizedRows(706, 60, 6300, 600, middle) == middle,
          "plan: a fresh range survives half a viewport of scrolling either way");
    CHECK(!(PlanRealizedRows(706, 60, 5699, 600, middle) == middle) && !(PlanRealizedRows(706, 60, 6301, 600, middle) == middle),
          "plan: ... and not one DIP more");
    CHECK(PlanRealizedRows(706, 60, 6000, 600, middle) == middle, "plan: a plan is a fixed point - planning again for the same viewport changes nothing");
    ExpectRows(PlanRealizedRows(706, 60, 6000, 600, RowRange{ 0, 706 }), 90, 120, "plan: a range much larger than FAR (every row) is trimmed back to FAR");
    ExpectRows(PlanRealizedRows(706, 60, 6000, 600, RowRange{ 75, 135 }), 75, 135, "plan: a range exactly twice FAR's size is kept");
    ExpectRows(PlanRealizedRows(706, 60, 6000, 600, RowRange{ 75, 136 }), 90, 120, "plan: ... one row more and it is trimmed");
    ExpectRows(PlanRealizedRows(10, 60, 0, 600, RowRange{ 0, 50 }), 0, 10, "plan: a range past the end of a list that shrank is clamped to it");
    ExpectRows(PlanRealizedRows(100, 60, 30000, 600, RowRange{}), 80, 100,
               "plan: an offset the shrunk content no longer allows plans from the clamped one (the last viewport, rows 90..99, + 10 above)");
    CHECK(PlanRealizedRows(0, 60, 0, 600, RowRange{ 3, 9 }).Empty(), "plan: an empty list realizes nothing");
    ExpectRows(PlanRealizedRows(706, 60, 0, 0, RowRange{}), 0, 24, "plan: a viewport with no height yet counts as 720 DIPs (12 rows + 12 below)");
    ExpectRows(PlanRealizedRows(706, 60, 0, nan, RowRange{}), 0, 24, "plan: ... so does a NaN height");
    ExpectRows(PlanRealizedRows(5, 60, 0, 600, RowRange{}), 0, 5, "plan: a list shorter than its viewport realizes every row");

    // --- RowsAroundRow: the reveal of a card whose row has none ---
    const RowRange around = RowsAroundRow(706, 60, 500, 600);
    ExpectRows(around, 481, 520, "reveal: row 500 realizes both viewports a minimum scroll can end in (rows 491..509) + FAR (10 rows) on each side");
    CHECK(PlanRealizedRows(706, 60, 29460, 600, around) == around, "reveal: scrolled from above (row 500 at the bottom edge), the plan keeps the range - no second pass");
    CHECK(PlanRealizedRows(706, 60, 30000, 600, around) == around, "reveal: scrolled from below (row 500 at the top edge), the plan keeps it too");
    ExpectRows(RowsAroundRow(706, 60, 0, 600), 0, 20, "reveal: the first row's range is clamped at the top");
    const RowRange lastRow = RowsAroundRow(706, 60, 705, 600);
    ExpectRows(lastRow, 686, 706, "reveal: the last row's range is clamped at the end");
    CHECK(PlanRealizedRows(706, 60, 41760, 600, lastRow) == lastRow, "reveal: ... and the plan at the end of the list keeps it");
    CHECK(RowsAroundRow(706, 60, 706, 600).Empty() && RowsAroundRow(0, 60, 0, 600).Empty(), "reveal: a row past the end gives no range");
    CHECK(RowsAroundRow(706, 60, 300, 0).Contains(300) && RowsAroundRow(706, nan, 300, nan).Contains(300),
          "reveal: the range holds the row whatever the stride and viewport read");

    // --- SpacersFor: the content is count * stride tall whatever is realized ---
    {
        const auto spacers = SpacersFor(706, 60, around);
        CHECK(spacers.top == 28860 && spacers.bottom == 11160, "spacers: rows 481..519 realized = 481 rows above (28860 DIPs) and 186 below (11160)");
        CHECK(spacers.top + static_cast<double>(around.Size()) * 60.0 + spacers.bottom == 706 * 60.0, "spacers: ... the two and the realized rows make the whole list");
        const auto nothing = SpacersFor(706, 60, RowRange{});
        CHECK(nothing.top == 0 && nothing.bottom == 42360, "spacers: nothing realized = one bottom spacer as tall as the list");
        const auto past = SpacersFor(10, 60, RowRange{ 50, 60 });
        CHECK(past.top == 600 && past.bottom == 0, "spacers: a range past the end of a shrunk list is clamped (every row in the top spacer)");
        const auto badStride = SpacersFor(706, nan, RowRange{ 0, 1 });
        CHECK(badStride.top == 0 && badStride.bottom == 705, "spacers: a NaN stride counts as 1 DIP");
    }

    // --- MedianRowHeight / StrideChanged: a stride one card cannot move ---
    CHECK(MedianRowHeight({}, 57.5) == 57.5, "stride: no measurement returns the fallback");
    CHECK(MedianRowHeight({ 60.0 }, 0) == 60.0, "stride: one measurement is the stride");
    CHECK(MedianRowHeight({ 62.0, 58.0, 60.0 }, 0) == 60.0, "stride: the middle of three");
    CHECK(MedianRowHeight({ 60.0, 60.0, 60.0, 120.0 }, 0) == 60.0, "stride: one taller card (an emoji title from a fallback font) does not move it");
    CHECK(MedianRowHeight({ 60.0, 62.0 }, 0) == 62.0, "stride: an even count takes the upper middle value");
    CHECK(MedianRowHeight({ nan, -5.0, 0.0, inf, 61.0 }, 0) == 61.0, "stride: NaN, infinite, negative and zero heights (cards not laid out yet) are ignored");
    CHECK(MedianRowHeight({ nan, 0.0 }, 57.5) == 57.5, "stride: only unusable heights return the fallback");
    CHECK(!StrideChanged(60.0, 60.0) && !StrideChanged(60.0, 60.04), "stride: rounding noise (under 0.05 DIP) is no change");
    CHECK(StrideChanged(60.0, 60.06) && StrideChanged(60.0, 59.9) && StrideChanged(60.0, 75.0), "stride: a real change in either direction is one");

    // --- a seeded sweep of the reveal: wherever the minimum scroll ends, the range covers that viewport and the plan
    //     made there keeps it (the reveal never needs a second realize pass) ---
    {
        SweepRandom random{ 0xD1B54A32D192ED03ull };
        size_t missingRow = 0; // a range without its own row
        size_t uncovered = 0; // a viewport after the minimum scroll with a row outside the range
        size_t secondPass = 0; // a plan at that viewport that differs from the range
        size_t cases = 0;
        for (int i = 0; i < 20000; ++i)
        {
            const size_t count = 1 + random.NextIndex(2000);
            const double stride = 20.0 + random.Next(150.0);
            const double height = 50.0 + random.Next(2000.0);
            const size_t row = random.NextIndex(count);
            const double maxTop = (std::max)(0.0, static_cast<double>(count) * stride - height);
            const double viewTop = random.Next(maxTop);
            const RowRange range = RowsAroundRow(count, stride, row, height);
            missingRow += range.Contains(row) ? 0 : 1;
            // The minimum scroll that shows the whole row (StartBringIntoView with default options), clamped like the
            // ScrollViewer clamps it.
            const double rowTop = static_cast<double>(row) * stride;
            double top = viewTop;
            if (rowTop < viewTop)
            {
                top = rowTop; // the row above the viewport: it ends at the top edge
            }
            else if (rowTop + stride > viewTop + height)
            {
                top = rowTop + stride - height; // below: at the bottom edge
            }
            top = std::clamp(top, 0.0, maxTop);
            uncovered += range.Covers(RowsInSpan(count, stride, top, top + height)) ? 0 : 1;
            secondPass += PlanRealizedRows(count, stride, top, height, range) == range ? 0 : 1;
            ++cases;
        }
        CHECK(cases == 20000, "reveal sweep: 20000 random lists, strides, viewports and rows");
        CHECK(missingRow == 0, "reveal sweep: every range holds its row");
        CHECK(uncovered == 0, "reveal sweep: after the minimum scroll every visible row is realized");
        CHECK(secondPass == 0, "reveal sweep: the plan at the scrolled-to viewport always keeps the range (no second pass)");
    }

    // --- a seeded scroll simulation: wheel notches, thumb jumps and panel resizes over random lists ---
    {
        SweepRandom random{ 0x2545F4914F6CDD1Dull };
        size_t outOfList = 0; // a plan with first > last or last > count
        size_t uncovered = 0; // a visible row without a card
        size_t oversized = 0; // a plan larger than the trim allows
        size_t unstable = 0; // a plan that is not a fixed point of the policy
        size_t steps = 0;
        for (int trial = 0; trial < 300; ++trial)
        {
            const size_t count = random.NextIndex(1500); // 0 rows included: the empty list
            const double stride = 30.0 + random.Next(90.0);
            double height = 100.0 + random.Next(1400.0);
            RowRange current{};
            double top = 0.0;
            for (int step = 0; step < 400; ++step)
            {
                const double pick = random.Next(1.0);
                if (pick < 0.70)
                {
                    top += (random.Next(1.0) < 0.5 ? -1.0 : 1.0) * (stride + random.Next(5.0 * stride)); // a wheel notch or a short drag
                }
                else if (pick < 0.90)
                {
                    top = random.Next(static_cast<double>(count) * stride); // a thumb jump
                }
                else
                {
                    height = 100.0 + random.Next(1400.0); // the panel resized
                }
                top = ClampViewTop(count, stride, top, height); // the ScrollViewer never leaves its extent
                const RowRange plan = PlanRealizedRows(count, stride, top, height, current);
                // (not named `far`: <windows.h> defines far / near as empty macros)
                const RowRange farRows = WidenedViewportRows(count, stride, top, height, kVirtualRowsFarViewports, kVirtualRowsFarMinRows);
                outOfList += (plan.first <= plan.last && plan.last <= count) ? 0 : 1;
                uncovered += plan.Covers(RowsInSpan(count, stride, top, top + height)) ? 0 : 1;
                oversized += plan.Size() <= kVirtualRowsTrimFactor * (std::max)(farRows.Size(), size_t{ 1 }) ? 0 : 1;
                unstable += PlanRealizedRows(count, stride, top, height, plan) == plan ? 0 : 1;
                current = plan;
                ++steps;
            }
        }
        CHECK(steps == 300 * 400, "scroll sweep: 300 random lists, 400 steps each");
        CHECK(outOfList == 0, "scroll sweep: every plan stays inside its list");
        CHECK(uncovered == 0, "scroll sweep: every visible row always has its card");
        CHECK(oversized == 0, "scroll sweep: no plan is larger than twice the FAR range");
        CHECK(unstable == 0, "scroll sweep: every plan is a fixed point (a ViewChanged at the same offset starts no pass)");
    }

    // --- one row per step, top to bottom and back: the hysteresis keeps re-plans to one per several rows ---
    {
        struct Shape
        {
            size_t count;
            double stride;
            double height;
        };
        const Shape shapes[] = {
            { 706, 60.0, 600.0 }, // the release's GLOBAL list: a re-plan every 6 rows
            { 706, 60.0, 100.0 }, // a viewport shorter than 8 rows: the 4- / 8-row margins decide (every 5 rows)
            { 706, 37.3, 913.7 }, // fractional sizes: rounding at the row boundaries
            { 2000, 45.5, 1400.0 },
            { 96, 58.25, 820.0 }, // the release's 96-tab window
        };
        size_t uncovered = 0;
        size_t tooEager = 0;
        for (const auto& shape : shapes)
        {
            const double maxTop = (std::max)(0.0, static_cast<double>(shape.count) * shape.stride - shape.height);
            RowRange current = PlanRealizedRows(shape.count, shape.stride, 0.0, shape.height, RowRange{});
            size_t replans = 0;
            size_t moves = 0;
            const auto stepTo = [&](double top) {
                const RowRange plan = PlanRealizedRows(shape.count, shape.stride, top, shape.height, current);
                replans += plan == current ? 0 : 1;
                uncovered += plan.Covers(RowsInSpan(shape.count, shape.stride, top, top + shape.height)) ? 0 : 1;
                current = plan;
                ++moves;
            };
            for (double top = shape.stride; top < maxTop; top += shape.stride)
            {
                stepTo(top);
            }
            stepTo(maxTop);
            for (double top = maxTop - shape.stride; top > 0.0; top -= shape.stride)
            {
                stepTo(top);
            }
            stepTo(0.0);
            // The FAR margin exceeds the NEAR one by at least 4 rows, so a fresh range lasts at least 4 one-row steps in
            // either direction.
            if (replans > moves / 4 + 2)
            {
                ++tooEager;
                std::wprintf(L"    %zu rows of %g DIPs under %g: %zu re-plans over %zu one-row steps\n", shape.count, shape.stride, shape.height, replans, moves);
            }
        }
        CHECK(uncovered == 0, "row-by-row: every visible row always has its card");
        CHECK(tooEager == 0, "row-by-row: a scroll of one row per step re-plans at most once per 4 rows");
    }
}

void TestStatusIconMotion()
{
    std::wprintf(L"Status icon motion (StatusIconMotion.h, SIDE_PANEL.md section 2g):\n");

    // Everything shown, on a started tab, in the live tree of a shown host, animations on: all three motions run - the
    // steady state of a card whose session has a draft, a warm cache and an unread turn.
    StatusIconMotionInputs all{};
    all.inLiveTree = true;
    all.hostShown = true;
    all.animationsEnabled = true;
    all.pendingShown = true;
    all.pendingMayPulse = true;
    all.cacheWarmShown = true;
    all.flashRingShown = true;
    {
        const auto m = DecideStatusIconMotion(all);
        CHECK(m.pendingPulse && m.cacheWarmCrown && m.flashRingBlink, "motion: everything shown in a visible icon - all three motions run");
    }

    // Nothing shown: nothing moves, however visible the icon is.
    {
        StatusIconMotionInputs none = all;
        none.pendingShown = false;
        none.cacheWarmShown = false;
        none.flashRingShown = false;
        const auto m = DecideStatusIconMotion(none);
        CHECK(!m.pendingPulse && !m.cacheWarmCrown && !m.flashRingBlink, "motion: a hidden indicator never moves");
    }

    // Out of the live tree (a card scrolled out of the realized rows, a header the strip virtualized away): nothing moves,
    // the alert included - the compositor must not animate an element nobody can see.
    {
        StatusIconMotionInputs gone = all;
        gone.inLiveTree = false;
        const auto m = DecideStatusIconMotion(gone);
        CHECK(!m.pendingPulse && !m.cacheWarmCrown && !m.flashRingBlink, "motion: out of the live tree - nothing moves, the ring included");
    }

    // A collapsed side panel: its cards stay LOADED (only the content collapses), so the host gate is what stops them.
    {
        StatusIconMotionInputs collapsed = all;
        collapsed.hostShown = false;
        const auto m = DecideStatusIconMotion(collapsed);
        CHECK(!m.pendingPulse && !m.cacheWarmCrown && !m.flashRingBlink, "motion: a hidden host (a collapsed panel) - nothing moves, though the icon is loaded");
    }

    // Windows' "Animation effects" off: the two DECORATIVE motions stand still, the ALERT keeps blinking (the timer blink
    // it replaced ran whatever the setting said, and a steady ring would read as "nothing new").
    {
        StatusIconMotionInputs still = all;
        still.animationsEnabled = false;
        const auto m = DecideStatusIconMotion(still);
        CHECK(!m.pendingPulse, "motion: animations off - the 3 dots stand still (still shown)");
        CHECK(!m.cacheWarmCrown, "motion: animations off - the crown stands still (the steady glow, no embers)");
        CHECK(m.flashRingBlink, "motion: animations off - the flash ring STILL blinks (an alert, not a decoration)");
    }

    // A dormant tab (window-restored or deactivated, its claude not started): its REMEMBERED draft shows still dots, while
    // its crown and its ring move as on any tab (a manual Mark Unread flashes a dormant tab too).
    {
        StatusIconMotionInputs dormant = all;
        dormant.pendingMayPulse = false;
        const auto m = DecideStatusIconMotion(dormant);
        CHECK(!m.pendingPulse, "motion: a dormant tab's remembered draft - still dots");
        CHECK(m.cacheWarmCrown && m.flashRingBlink, "motion: a dormant tab - the crown and the ring are unaffected by dormancy");
    }

    // The three motions are independent: each depends only on its own "shown" flag (and the shared gates).
    {
        StatusIconMotionInputs ringOnly = all;
        ringOnly.pendingShown = false;
        ringOnly.cacheWarmShown = false;
        const auto m = DecideStatusIconMotion(ringOnly);
        CHECK(!m.pendingPulse && !m.cacheWarmCrown && m.flashRingBlink, "motion: only the ring shown - only the ring blinks");
        StatusIconMotionInputs crownOnly = all;
        crownOnly.pendingShown = false;
        crownOnly.flashRingShown = false;
        const auto c = DecideStatusIconMotion(crownOnly);
        CHECK(!c.pendingPulse && c.cacheWarmCrown && !c.flashRingBlink, "motion: only the crown shown - only the crown moves");
    }

    // Exhaustive: every combination of the seven inputs against an independent statement of each motion's gates (written
    // as the conditions the tab header's _Update*Animation methods test, one by one, not as the function's expressions).
    {
        size_t cases = 0;
        size_t failures = 0;
        for (unsigned bits = 0; bits < 128; ++bits)
        {
            StatusIconMotionInputs in{};
            in.inLiveTree = (bits & 1u) != 0;
            in.hostShown = (bits & 2u) != 0;
            in.animationsEnabled = (bits & 4u) != 0;
            in.pendingShown = (bits & 8u) != 0;
            in.pendingMayPulse = (bits & 16u) != 0;
            in.cacheWarmShown = (bits & 32u) != 0;
            in.flashRingShown = (bits & 64u) != 0;
            const auto m = DecideStatusIconMotion(in);
            // _UpdatePendingAnimation: AgentPendingVisible && !AgentStatusHalfVisible (not dormant) && IsLoaded() && AgentAnimationsEnabled()
            bool pulse = in.pendingShown;
            pulse = pulse && in.pendingMayPulse;
            pulse = pulse && in.inLiveTree && in.hostShown;
            pulse = pulse && in.animationsEnabled;
            // _UpdateCacheWarmAnimation: AgentCacheWarmVisible && IsLoaded() && AgentAnimationsEnabled()
            bool crown = in.cacheWarmShown;
            crown = crown && in.inLiveTree && in.hostShown;
            crown = crown && in.animationsEnabled;
            // _UpdateFlashRingAnimation: AgentFlashRingVisible && IsLoaded() - no animation switch, no dormancy
            bool blink = in.flashRingShown;
            blink = blink && in.inLiveTree && in.hostShown;
            ++cases;
            if (m.pendingPulse != pulse || m.cacheWarmCrown != crown || m.flashRingBlink != blink)
            {
                ++failures;
                std::wprintf(L"    inputs 0x%02X: pulse %d/%d crown %d/%d blink %d/%d (got/expected)\n",
                             bits,
                             m.pendingPulse ? 1 : 0,
                             pulse ? 1 : 0,
                             m.cacheWarmCrown ? 1 : 0,
                             crown ? 1 : 0,
                             m.flashRingBlink ? 1 : 0,
                             blink ? 1 : 0);
            }
        }
        CHECK(cases == 128 && failures == 0, "motion: all 128 input combinations match the tab header's gates, one motion at a time");
    }

    // constexpr: the rule can be evaluated at compile time (a pure function of its inputs, nothing else).
    {
        constexpr StatusIconMotionInputs kAlertOnly{ true, true, false, false, false, false, true };
        constexpr StatusIconMotion kAlertMotion = DecideStatusIconMotion(kAlertOnly);
        static_assert(kAlertMotion.flashRingBlink && !kAlertMotion.pendingPulse && !kAlertMotion.cacheWarmCrown,
                      "the flash ring blinks with animations off; nothing else is shown");
        CHECK(kAlertMotion.flashRingBlink, "motion: the rule is constexpr (evaluated at compile time)");
    }
}

namespace
{
    // A conversation state as the agent's events carry it: the shown flag, the ids and the index-aligned whys.
    SessionSearchSnapshot AiConversation(bool busy, bool hasShown, std::vector<std::wstring> ids, std::vector<std::wstring> whys)
    {
        SessionSearchSnapshot s;
        s.busy = busy;
        s.hasShown = hasShown;
        s.shownIds = std::move(ids);
        s.shownReasons = std::move(whys);
        return s;
    }

    // A small model of the side panel's AI view: the list it shows and how many times it replaced it. apply() is
    // TerminalPage::_OnSidePanelAiEvent's list half, reduced to its data: the step, the comparison, then the adoption that
    // _SidePanelAiAdoptSnapshot performs (the flag, the ids, a why per id that has one).
    struct AiListView
    {
        bool hasShown = false;
        std::vector<std::wstring> ids;
        std::unordered_map<std::wstring, std::wstring> whys;
        int replacements = 0; // list changes the user saw (each a rebuild of the cards)

        void Adopt(const SessionSearchSnapshot& s)
        {
            hasShown = s.hasShown;
            ids = s.hasShown ? s.shownIds : std::vector<std::wstring>{};
            whys.clear();
            for (size_t i = 0; i < ids.size() && i < s.shownReasons.size(); ++i)
            {
                if (!s.shownReasons[i].empty())
                {
                    whys.insert_or_assign(ids[i], s.shownReasons[i]);
                }
            }
            ++replacements;
        }

        void Apply(SessionSearchEventKind kind, const SessionSearchSnapshot& s)
        {
            switch (SidePanelAiListStepFor(kind, s.busy))
            {
            case SidePanelAiListStep::AdoptNow:
                Adopt(s);
                break;
            case SidePanelAiListStep::AdoptIfChanged:
                if (SidePanelAiListDiffers(hasShown, ids, whys, s))
                {
                    Adopt(s);
                }
                break;
            case SidePanelAiListStep::Keep:
            default:
                break;
            }
        }
    };
}

void TestSidePanelAiList()
{
    std::wprintf(L"Side panel AI list (SidePanelAiList.h, SIDE_PANEL.md section 2h):\n");
    using Kind = SessionSearchEventKind;
    using Step = SidePanelAiListStep;

    // The step of every kind, running or not.
    {
        CHECK(SidePanelAiListStepFor(Kind::Candidates, true) == Step::Keep, "ai list: a running search's candidates leave the list alone");
        CHECK(SidePanelAiListStepFor(Kind::Candidates, false) == Step::Keep, "ai list: candidates are never drawn, even with nothing running");
        CHECK(SidePanelAiListStepFor(Kind::Shown, true) == Step::Keep, "ai list: a show while its request runs is held (the model may show again)");
        CHECK(SidePanelAiListStepFor(Kind::Shown, false) == Step::AdoptIfChanged, "ai list: a show with nothing running is final at once");
        bool endsAdopt = true;
        bool resetsAdopt = true;
        bool stripOnlyKeeps = true;
        for (const bool busy : { false, true })
        {
            endsAdopt = endsAdopt && SidePanelAiListStepFor(Kind::Answer, busy) == Step::AdoptIfChanged &&
                        SidePanelAiListStepFor(Kind::Failed, busy) == Step::AdoptIfChanged;
            resetsAdopt = resetsAdopt && SidePanelAiListStepFor(Kind::Reset, busy) == Step::AdoptNow;
            for (const Kind k : { Kind::StateChanged, Kind::Asked, Kind::Searching, Kind::Compacting, Kind::Narration })
            {
                stripOnlyKeeps = stripOnlyKeeps && SidePanelAiListStepFor(k, busy) == Step::Keep;
            }
        }
        CHECK(endsAdopt, "ai list: a request's end (Answer, Failed) takes the conversation's list when it differs");
        CHECK(resetsAdopt, "ai list: a reset clears the list at once");
        CHECK(stripOnlyKeeps, "ai list: state, asked, searching, compacting and narration events change only the strip");
        // A kind the agent adds later: Keep until the rule names it (the default branch).
        CHECK(SidePanelAiListStepFor(static_cast<Kind>(250), true) == Step::Keep, "ai list: an unknown event kind changes nothing");
    }

    // The comparison that decides whether a request's end replaces the cards.
    {
        const std::unordered_map<std::wstring, std::wstring> none;
        const std::unordered_map<std::wstring, std::wstring> whyA{ { L"a", L"edited TabDrag.cpp" } };
        CHECK(!SidePanelAiListDiffers(false, {}, none, AiConversation(false, false, {}, {})), "ai list diff: no list on either side - nothing to take");
        CHECK(SidePanelAiListDiffers(false, {}, none, AiConversation(false, true, {}, {})), "ai list diff: an empty show ('nothing fits') differs from no list yet");
        CHECK(!SidePanelAiListDiffers(true, {}, none, AiConversation(false, true, {}, {})), "ai list diff: 'nothing fits' on both sides - the same");
        CHECK(SidePanelAiListDiffers(true, { L"a" }, none, AiConversation(false, false, {}, {})), "ai list diff: a list on screen against none differs");
        CHECK(!SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a", L"b" }, { L"edited TabDrag.cpp", L"" })), "ai list diff: the same ids and whys - the same");
        CHECK(!SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a", L"b" }, { L"edited TabDrag.cpp" })), "ai list diff: a shorter why vector reads as no why for the rest");
        CHECK(SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"b", L"a" }, { L"", L"edited TabDrag.cpp" })), "ai list diff: the same sessions in another order differ");
        CHECK(SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a", L"b", L"c" }, { L"edited TabDrag.cpp" })), "ai list diff: one more session differs");
        CHECK(SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a" }, { L"edited TabDrag.cpp" })), "ai list diff: one session fewer differs");
        CHECK(SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a", L"b" }, { L"read the crash dump", L"" })), "ai list diff: a changed why differs (a show can relist the same sessions with new evidence)");
        CHECK(SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a", L"b" }, { L"edited TabDrag.cpp", L"grep: drag" })), "ai list diff: a why added differs");
        CHECK(SidePanelAiListDiffers(true, { L"a", L"b" }, whyA, AiConversation(false, true, { L"a", L"b" }, { L"", L"" })), "ai list diff: a why dropped differs");
    }

    // Whole requests, replayed through the view's list half.
    {
        // The last answer's list is on screen: [x] with its why.
        AiListView view;
        view.Adopt(AiConversation(false, true, { L"x" }, { L"the old pick" }));
        view.replacements = 0;
        const auto old = AiConversation(true, true, { L"x" }, { L"the old pick" }); // the conversation while the next request runs, before its show
        // A request: asked, searching, candidates (twice), a show, a second show, then the answer.
        view.Apply(Kind::Asked, old);
        view.Apply(Kind::Searching, old);
        view.Apply(Kind::Candidates, old);
        view.Apply(Kind::Narration, old);
        view.Apply(Kind::Candidates, old);
        CHECK(view.replacements == 0 && view.ids == std::vector<std::wstring>{ L"x" }, "ai request: a running search's candidates leave the last answer's list on screen");
        view.Apply(Kind::Shown, AiConversation(true, true, { L"a", L"b", L"c" }, { L"w1", L"w2", L"w3" }));
        CHECK(view.replacements == 0 && view.ids == std::vector<std::wstring>{ L"x" }, "ai request: the model's first show is held while the request runs");
        view.Apply(Kind::Shown, AiConversation(true, true, { L"b" }, { L"w2 (narrowed)" }));
        view.Apply(Kind::Compacting, AiConversation(true, true, { L"b" }, { L"w2 (narrowed)" }));
        CHECK(view.replacements == 0 && view.ids == std::vector<std::wstring>{ L"x" }, "ai request: a second, narrowed show is held too");
        view.Apply(Kind::Answer, AiConversation(false, true, { L"b" }, { L"w2 (narrowed)" }));
        CHECK(view.replacements == 1, "ai request: the list changes exactly once per request - at its end");
        CHECK(view.ids == std::vector<std::wstring>{ L"b" } && view.whys.count(L"b") == 1 && view.whys.at(L"b") == L"w2 (narrowed)", "ai request: the end shows the LAST show's pick with its why");
        view.Apply(Kind::StateChanged, AiConversation(false, true, { L"b" }, { L"w2 (narrowed)" }));
        CHECK(view.replacements == 1, "ai request: an event after the end leaves the final list alone");

        // A follow-up the model answers in words, without a show: the cards stay as they are (no rebuild, no scroll jump).
        view.Apply(Kind::Asked, AiConversation(true, true, { L"b" }, { L"w2 (narrowed)" }));
        view.Apply(Kind::Candidates, AiConversation(true, true, { L"b" }, { L"w2 (narrowed)" }));
        view.Apply(Kind::Answer, AiConversation(false, true, { L"b" }, { L"w2 (narrowed)" }));
        CHECK(view.replacements == 1 && view.ids == std::vector<std::wstring>{ L"b" }, "ai request: an answer in words changes nothing on screen");

        // A request that shows, then fails (an API error after the show): the pick is the conversation's list, and it lands at
        // the failure - the request's end.
        view.Apply(Kind::Asked, AiConversation(true, true, { L"b" }, { L"w2 (narrowed)" }));
        view.Apply(Kind::Shown, AiConversation(true, true, { L"c", L"d" }, { L"w3", L"" }));
        CHECK(view.replacements == 1, "ai request: a show before a failure is held while the request runs");
        view.Apply(Kind::Failed, AiConversation(false, true, { L"c", L"d" }, { L"w3", L"" }));
        // The expected list in parentheses: CHECK is a macro, and only parentheses keep the brace list's comma inside one argument.
        CHECK(view.replacements == 2 && (view.ids == std::vector<std::wstring>{ L"c", L"d" }) && view.whys.count(L"d") == 0, "ai request: a failure after a show lands the pick (a session without a why keeps none)");

        // An empty show ("nothing fits") is a result too: it replaces the list at the end with the empty list.
        view.Apply(Kind::Asked, AiConversation(true, true, { L"c", L"d" }, { L"w3", L"" }));
        view.Apply(Kind::Shown, AiConversation(true, true, {}, {}));
        view.Apply(Kind::Answer, AiConversation(false, true, {}, {}));
        CHECK(view.replacements == 3 && view.hasShown && view.ids.empty(), "ai request: an empty show replaces the list at the end ('no session fits')");

        // A reset (the strip's crossed circle) clears at once, a request running or not.
        view.Apply(Kind::Asked, AiConversation(true, true, {}, {}));
        view.Apply(Kind::Reset, AiConversation(false, false, {}, {}));
        CHECK(view.replacements == 4 && !view.hasShown && view.ids.empty(), "ai request: a reset clears the list at once");
    }
}

namespace
{
    // One entry of an AI search conversation's record, as the harness appends it (AgentHarnessCore.h HarnessItem).
    //   kind    - what the entry is (a request, a text block, a tool call or result, a notice).
    //   turn    - the request it belongs to (0 = none).
    //   text    - its text (a request, a text block, a notice, a tool result).
    //   at      - when the harness saw it (Unix ms).
    //   isError - a notice that reports a failure.
    //   returns - the entry.
    //   throws  - std::bad_alloc only.
    HarnessItem AiCopyItem(HarnessItemKind kind, uint64_t turn, std::wstring text, int64_t at = 0, bool isError = false)
    {
        HarnessItem item;
        item.kind = kind;
        item.turn = turn;
        item.atUnixMs = at;
        item.text = std::move(text);
        item.isError = isError;
        return item;
    }

    // The fixed time formatter of the copy tests: "T<ms>", so a heading's time is known whatever this machine's time zone.
    //   ms      - the moment (Unix ms).
    //   returns - "T" and the number.
    //   throws  - std::bad_alloc only.
    std::wstring AiCopyStamp(int64_t ms)
    {
        return L"T" + std::to_wstring(ms);
    }

    // The conversation every copy test starts from: four recorded requests (1, 2, 3, 5 - request 4 was replaced before it
    // started, so the harness never recorded it), around a process notice of no turn and a compaction notice:
    //   1 - a plan, a search, then a reply of TWO text blocks;
    //   2 - a search, then a one-block reply;
    //   3 - a plan, a search, then an error notice (the turn failed);
    //   5 - a plan and a search, then nothing (stopped by Esc).
    //   returns - the record, far below the cap.
    //   throws  - std::bad_alloc only.
    HarnessRecord AiCopyRecord()
    {
        using K = HarnessItemKind;
        HarnessRecord r;
        r.items.push_back(AiCopyItem(K::Notice, 0, L"the process started"));
        r.items.push_back(AiCopyItem(K::User, 1, L"the session where I fixed the tab drag crash", 1000));
        r.items.push_back(AiCopyItem(K::Assistant, 1, L"I'll 1. search 2. read the candidates 3. show them"));
        r.items.push_back(AiCopyItem(K::ToolCall, 1, L""));
        r.items.push_back(AiCopyItem(K::ToolResult, 1, L"14 sessions"));
        r.items.push_back(AiCopyItem(K::Assistant, 1, L"Three sessions worked on the tab drag crash."));
        r.items.push_back(AiCopyItem(K::Assistant, 1, L"The fix is in \"Tab drag: pointer-owned gesture\"."));
        r.items.push_back(AiCopyItem(K::Notice, 1, L"The context was compacted: 210000 -> 30000 tokens."));
        r.items.push_back(AiCopyItem(K::User, 2, L"only the open ones", 2000));
        r.items.push_back(AiCopyItem(K::ToolCall, 2, L""));
        r.items.push_back(AiCopyItem(K::ToolResult, 2, L"1 session"));
        r.items.push_back(AiCopyItem(K::Assistant, 2, L"One of them is open."));
        r.items.push_back(AiCopyItem(K::User, 3, L"and the newest?", 3000));
        r.items.push_back(AiCopyItem(K::Assistant, 3, L"I'll check the newest first."));
        r.items.push_back(AiCopyItem(K::ToolCall, 3, L""));
        r.items.push_back(AiCopyItem(K::Notice, 3, L"Claude Code ended during the turn.", 0, true));
        r.items.push_back(AiCopyItem(K::User, 5, L"stop me", 5000));
        r.items.push_back(AiCopyItem(K::Assistant, 5, L"I'll search."));
        r.items.push_back(AiCopyItem(K::ToolCall, 5, L""));
        r.turns = 5;
        return r;
    }
}

// TestSidePanelAiCopy pins the AI strip's crossed circle and the copy button under it (AgentMaster/SidePanelAiCopy.h,
// SIDE_PANEL.md section 2h; the user's two requests of 2026-11-10) - see the file's header for the list.
void TestSidePanelAiCopy()
{
    std::wprintf(L"Side panel AI copy (SidePanelAiCopy.h, SIDE_PANEL.md section 2h):\n");

    // The box refill: what the crossed circle and the copy button write into the request box.
    {
        CHECK(SidePanelAiRefillText(L"", L"the tab drag crash") == L"the tab drag crash", "ai refill: an empty box gets the newest request back");
        CHECK(SidePanelAiRefillText(L"  \t\r\n ", L"the tab drag crash") == L"the tab drag crash", "ai refill: a box of white space counts as empty");
        CHECK(SidePanelAiRefillText(L"only the open ones", L"the tab drag crash").empty(), "ai refill: a box with words keeps them");
        CHECK(SidePanelAiRefillText(L" x ", L"the tab drag crash").empty(), "ai refill: one visible character in the box is a prompt the user typed");
        CHECK(SidePanelAiRefillText(L"", L"").empty() && SidePanelAiRefillText(L"", L" \r\n\t ").empty(), "ai refill: no request (or white space alone) writes nothing");
        CHECK(SidePanelAiRefillText(L"", L"  the  drag \r\n\r\n  crash\nfix  ") == L"the  drag crash fix", "ai refill: one line - the ends trimmed, a run of line breaks one space, the inner spaces kept");
        CHECK(SidePanelAiRefillText(L"", L"\n\nfirst\r") == L"first", "ai refill: line breaks at the ends leave no space behind");
    }

    // The fold of the record into requests and replies.
    {
        const HarnessRecord record = AiCopyRecord();
        const auto folded = SidePanelAiCopyExchanges(record, 5, L"stop me", false);
        const auto& e = folded.exchanges;
        CHECK(e.size() == 4 && e[0].turn == 1 && e[1].turn == 2 && e[2].turn == 3 && e[3].turn == 5, "ai copy fold: one exchange per recorded request, in order, the replaced request's gap kept");
        CHECK(e.size() == 4 && e[0].askedAtUnixMs == 1000 && e[3].askedAtUnixMs == 5000 && e[0].request == L"the session where I fixed the tab drag crash", "ai copy fold: a request keeps its words and its time");
        CHECK(e.size() == 4 && e[0].reply == L"Three sessions worked on the tab drag crash.\nThe fix is in \"Tab drag: pointer-owned gesture\".", "ai copy fold: the reply is the text after the last tool call, two blocks joined by a line break - the plan before the call is left out");
        CHECK(e.size() == 4 && e[1].reply == L"One of them is open." && !e[1].failed && !e[1].running, "ai copy fold: a one-block reply");
        CHECK(e.size() == 4 && e[2].failed && e[2].reply == L"Claude Code ended during the turn.", "ai copy fold: an error notice makes the turn a failure with its text as the reply (the plan is not the reply)");
        CHECK(e.size() == 4 && e[3].reply.empty() && !e[3].failed && !e[3].running, "ai copy fold: a request stopped after a tool call has no reply");
        CHECK(!folded.earlierDropped, "ai copy fold: a record below its cap never lost a request, even when it starts past request 1");

        // The newest request still runs, and the record holds it: no reply yet, whatever it wrote after its last call.
        HarnessRecord running = record;
        running.items.push_back(AiCopyItem(HarnessItemKind::User, 6, L"the one with the spinner", 6000));
        running.items.push_back(AiCopyItem(HarnessItemKind::Assistant, 6, L"Searching for the spinner"));
        const auto live = SidePanelAiCopyExchanges(running, 6, L"the one with the spinner", true);
        CHECK(live.exchanges.size() == 5 && live.exchanges[4].running && live.exchanges[4].reply.empty() && live.exchanges[4].askedAtUnixMs == 6000, "ai copy fold: a running request the record holds has no reply yet - its text so far may still be its plan");
        CHECK(live.exchanges.size() == 5 && !live.exchanges[3].running && live.exchanges[0].reply == e[0].reply, "ai copy fold: only the NEWEST request runs");
        const auto settled = SidePanelAiCopyExchanges(running, 6, L"the one with the spinner", false);
        CHECK(settled.exchanges.size() == 5 && !settled.exchanges[4].running && settled.exchanges[4].reply == L"Searching for the spinner", "ai copy fold: the same turn at rest has its last text as the reply");

        // The newest request runs, but its turn has not started in the harness yet: the snapshot's words stand in.
        const auto waiting = SidePanelAiCopyExchanges(record, 6, L"  and the spinner one?  ", true);
        CHECK(waiting.exchanges.size() == 5 && waiting.exchanges[4].turn == 6 && waiting.exchanges[4].running && waiting.exchanges[4].request == L"and the spinner one?" && waiting.exchanges[4].askedAtUnixMs == 0, "ai copy fold: a running request not recorded yet is added from the snapshot, trimmed, with no time");
        CHECK(SidePanelAiCopyExchanges(record, 6, L" ", true).exchanges.size() == 4, "ai copy fold: a running request with no words adds nothing");
        CHECK(SidePanelAiCopyExchanges(record, 6, L"never asked", false).exchanges.size() == 4, "ai copy fold: a newest request that does not run adds nothing the record lacks");
    }

    // The record's cap: only a record AT its cap can have lost requests.
    {
        using K = HarnessItemKind;
        HarnessRecord full;
        full.items.push_back(AiCopyItem(K::Assistant, 1, L"the reply of a request the cap dropped"));
        full.items.push_back(AiCopyItem(K::ToolResult, 1, L"its last tool result"));
        uint64_t turn = 2;
        while (full.items.size() < kHarnessMaxItems)
        {
            full.items.push_back(AiCopyItem(K::User, turn, L"request " + std::to_wstring(turn), static_cast<int64_t>(turn)));
            if (full.items.size() < kHarnessMaxItems)
            {
                full.items.push_back(AiCopyItem(K::Assistant, turn, L"reply " + std::to_wstring(turn)));
            }
            ++turn;
        }
        const auto capped = SidePanelAiCopyExchanges(full, turn - 1, L"", false);
        CHECK(capped.earlierDropped && !capped.exchanges.empty() && capped.exchanges.front().turn == 2, "ai copy cap: a full record that lost a request says so, and its orphaned reply is left out");
        bool orphanLeftOut = true;
        for (const auto& x : capped.exchanges)
        {
            orphanLeftOut = orphanLeftOut && x.reply != L"the reply of a request the cap dropped";
        }
        CHECK(orphanLeftOut, "ai copy cap: no exchange carries the dropped request's reply");

        HarnessRecord fullFromOne = full;
        fullFromOne.items.front() = AiCopyItem(K::User, 1, L"request 1", 1);
        fullFromOne.items[1] = AiCopyItem(K::Assistant, 1, L"reply 1");
        const auto whole = SidePanelAiCopyExchanges(fullFromOne, turn - 1, L"", false);
        CHECK(!whole.earlierDropped && whole.exchanges.front().turn == 1, "ai copy cap: a full record that still starts at request 1 lost nothing");

        HarnessRecord fromTwo;
        fromTwo.items.push_back(AiCopyItem(K::User, 2, L"request 2", 2));
        CHECK(!SidePanelAiCopyExchanges(fromTwo, 2, L"", false).earlierDropped, "ai copy cap: below the cap, starting at request 2 (request 1 replaced) is never blamed on the cap");
    }

    // The whole text of a known conversation, byte for byte (the user's approved layout of 2026-11-10).
    {
        SidePanelAiCopyContent content;
        content.conversation = SidePanelAiCopyExchanges(AiCopyRecord(), 5, L"stop me", false);
        content.hasShown = true;
        SidePanelAiCopyListRow open;
        open.id = L"7d1f0c2a-5b3e-4c1a-9f0e-2b6d8e4a1c3f";
        open.title = L"Tab drag: pointer-owned gesture";
        open.state = L"open \x00B7 waiting-for-you";
        open.folder = L"K:\\source\\Agentmaster";
        open.branch = L"agentmaster";
        open.why = L"replaced CanDragTabs with the pointer-owned drag";
        SidePanelAiCopyListRow unknown;
        unknown.id = L"0123abcd-0000-4000-8000-000000000000";
        unknown.state = L"closed";
        content.listed = { open, unknown };
        const std::wstring expected =
            L"## AI search \x00B7 4 requests\n"
            L"\n"
            L"### Request 1 \x00B7 T1000\n"
            L"\n"
            L"**\x276F User**\n"
            L"> the session where I fixed the tab drag crash\n"
            L"\n"
            L"**\x25CF Claude**\n"
            L"> Three sessions worked on the tab drag crash.\n"
            L"> The fix is in \"Tab drag: pointer-owned gesture\".\n"
            L"\n"
            L"### Request 2 \x00B7 T2000\n"
            L"\n"
            L"**\x276F User**\n"
            L"> only the open ones\n"
            L"\n"
            L"**\x25CF Claude**\n"
            L"> One of them is open.\n"
            L"\n"
            L"### Request 3 \x00B7 T3000\n"
            L"\n"
            L"**\x276F User**\n"
            L"> and the newest?\n"
            L"\n"
            L"**\x25CF Claude** \x00B7 failed\n"
            L"> Claude Code ended during the turn.\n"
            L"\n"
            L"### Request 5 \x00B7 T5000\n"
            L"\n"
            L"**\x276F User**\n"
            L"> stop me\n"
            L"\n"
            L"**\x25CF Claude**\n"
            L"> _(no reply \x2014 the request was stopped before Claude answered)_\n"
            L"\n"
            L"### Listed sessions \x00B7 2\n"
            L"\n"
            L"1. **Tab drag: pointer-owned gesture** \x00B7 open \x00B7 waiting-for-you\n"
            L"   `K:\\source\\Agentmaster` \x00B7 `agentmaster`\n"
            L"   id `7d1f0c2a-5b3e-4c1a-9f0e-2b6d8e4a1c3f`\n"
            L"   > replaced CanDragTabs with the pointer-owned drag\n"
            L"2. **session 0123abcd** \x00B7 closed\n"
            L"   id `0123abcd-0000-4000-8000-000000000000`\n";
        const std::wstring text = BuildSidePanelAiCopyText(content, AiCopyStamp);
        CHECK(text == expected, "ai copy text: the whole conversation and the list, byte for byte");
        if (text != expected)
        {
            std::wprintf(L"    got:\n%ls\n    expected:\n%ls\n", text.c_str(), expected.c_str());
        }
    }

    // The text's edges.
    {
        const auto has = [](const std::wstring& text, const std::wstring& part) { return text.find(part) != std::wstring::npos; };
        SidePanelAiCopyContent one;
        SidePanelAiCopyExchange ask;
        ask.turn = 1;
        ask.askedAtUnixMs = 42;
        ask.request = L"the drag crash";
        ask.running = true;
        one.conversation.exchanges = { ask };
        const std::wstring runningText = BuildSidePanelAiCopyText(one, AiCopyStamp);
        CHECK(runningText.rfind(L"## AI search \x00B7 1 request\n", 0) == 0, "ai copy text: one request reads 'request', not 'requests'");
        CHECK(has(runningText, L"**\x25CF Claude**\n> _(still running \x2014 no reply yet)_\n"), "ai copy text: a running request says it has no reply yet");
        CHECK(!has(runningText, L"Listed sessions"), "ai copy text: no list section before the model listed anything");
        CHECK(has(BuildSidePanelAiCopyText(one, {}), L"### Request 1\n\n"), "ai copy text: without a time formatter the heading names no time");

        SidePanelAiCopyContent nothingFits = one;
        nothingFits.hasShown = true;
        CHECK(has(BuildSidePanelAiCopyText(nothingFits, AiCopyStamp), L"### Listed sessions \x00B7 0\n\nNo session fits this request.\n"), "ai copy text: an empty pick reads the view's own 'No session fits this request.'");

        SidePanelAiCopyContent previous = nothingFits;
        previous.listIsPrevious = true;
        CHECK(has(BuildSidePanelAiCopyText(previous, AiCopyStamp), L"_(The list of the answer before the running request: it changes when that request ends.)_"), "ai copy text: while a request runs the list says it is the answer before it");

        SidePanelAiCopyContent dropped = one;
        dropped.conversation.earlierDropped = true;
        CHECK(has(BuildSidePanelAiCopyText(dropped, AiCopyStamp), L"_(The first requests of this conversation are no longer in its record, which keeps its newest " + std::to_wstring(kHarnessMaxItems) + L" entries.)_"), "ai copy text: the cap's note names the cap");

        SidePanelAiCopyContent paragraphs;
        SidePanelAiCopyExchange answered;
        answered.turn = 1;
        answered.request = L"two lines\r\nof request";
        answered.reply = L"para one\n\npara two\n";
        paragraphs.conversation.exchanges = { answered };
        CHECK(has(BuildSidePanelAiCopyText(paragraphs, AiCopyStamp), L"> two lines\n> of request\n\n**\x25CF Claude**\n> para one\n>\n> para two\n"), "ai copy text: a request keeps its lines, a reply's empty line stays inside the quote, the trailing break adds no line");

        SidePanelAiCopyContent many;
        many.hasShown = true;
        for (int i = 1; i <= 10; ++i)
        {
            SidePanelAiCopyListRow row;
            row.id = L"id" + std::to_wstring(i);
            row.title = (i == 1) ? std::wstring{ L"fix\r\nthe drag" } : (L"t" + std::to_wstring(i));
            row.state = L"closed";
            row.why = (i == 10) ? std::wstring{ L"why ten" } : std::wstring{};
            many.listed.push_back(row);
        }
        const std::wstring manyText = BuildSidePanelAiCopyText(many, AiCopyStamp);
        CHECK(manyText.rfind(L"## AI search \x00B7 0 requests\n", 0) == 0, "ai copy text: a list without a recorded request still copies");
        CHECK(has(manyText, L"1. **fix the drag** \x00B7 closed\n   id `id1`\n"), "ai copy text: a title on two lines is written on one");
        CHECK(has(manyText, L"10. **t10** \x00B7 closed\n    id `id10`\n    > why ten\n"), "ai copy text: a two-digit item's lines are indented by its marker's width");
        CHECK(!has(manyText, L"> \n") && !has(manyText, L"`id2`\n   >"), "ai copy text: a session without a why gets no quote");

        CHECK(BuildSidePanelAiCopyText(SidePanelAiCopyContent{}, AiCopyStamp).empty(), "ai copy text: nothing asked and nothing listed copies nothing");
    }

    // Code spans: a path's backslashes and a branch's underscores read as they are, whatever backticks they hold.
    {
        CHECK(SidePanelAiCodeSpan(L"K:\\source\\a_b") == L"`K:\\source\\a_b`", "ai code span: a plain text gets one backtick");
        CHECK(SidePanelAiCodeSpan(L"a`b") == L"``a`b``", "ai code span: an inner backtick needs a longer fence");
        CHECK(SidePanelAiCodeSpan(L"a``b`c") == L"```a``b`c```", "ai code span: the fence is longer than the LONGEST inner run");
        CHECK(SidePanelAiCodeSpan(L"`edge") == L"`` `edge ``", "ai code span: a text that starts with a backtick is padded");
    }

    // The local time stamp: "Mon DD HH:MM" from this machine's clock, nothing for an unknown moment.
    {
        CHECK(SidePanelAiCopyLocalStamp(0).empty() && SidePanelAiCopyLocalStamp(-5).empty(), "ai copy stamp: no moment, no time");
        const int64_t ms = 1'762'780'920'000;
        const time_t seconds = static_cast<time_t>(ms / 1000);
        std::tm local{};
        const bool converted = localtime_s(&local, &seconds) == 0;
        static const wchar_t* const kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        wchar_t want[32]{};
        if (converted)
        {
            std::swprintf(want, 32, L"%ls %02d %02d:%02d", kMonths[local.tm_mon % 12], local.tm_mday, local.tm_hour, local.tm_min);
        }
        const std::wstring stamp = SidePanelAiCopyLocalStamp(ms);
        CHECK(converted && stamp == want && stamp.size() == 12 && stamp[3] == L' ' && stamp[6] == L' ' && stamp[9] == L':', "ai copy stamp: the month's English name, a two-digit day and a 24-hour time");
    }
}

namespace
{
    // One row of the side panel's card list as _SidePanelRebuildCards orders it: the card's pin stamp, and its rank in the
    // board's shared order (MakeSortKey / SortKeyLess, stood in for by a number: lower = earlier).
    struct PinRow
    {
        std::wstring id;
        int64_t pin;
        int boardRank;
    };

    // The list's comparator, written exactly as _SidePanelRebuildCards writes it: the pins first, the board's order for what
    // they leave equal.
    //   a, b    - two rows.
    //   returns - true when a goes before b.
    //   throws  - nothing.
    bool PinRowLess(const PinRow& a, const PinRow& b) noexcept
    {
        const int pin = CompareSidePanelPins(a.pin, b.pin);
        return pin < 0 || (pin == 0 && a.boardRank < b.boardRank);
    }

    // The rows' ids in order, as one string ("bfdg..."), so a CHECK names the whole order at once.
    //   rows    - the rows.
    //   returns - their ids concatenated.
    //   throws  - std::bad_alloc only.
    std::wstring PinRowOrder(const std::vector<PinRow>& rows)
    {
        std::wstring order;
        for (const auto& r : rows)
        {
            order += r.id;
        }
        return order;
    }
}

// TestSidePanelPins pins the side panel's PIN TO TOP (AgentMaster/SidePanelPins.h, SIDE_PANEL.md section 2j; the user's request
// of 2026-11-06, "pinning puts on top and repinning will bring a tab to the top again", and their pick of a pin SEPARATE from
// Telegram's never-idle /pin): which stamps are pins; the pinned-first comparison; a list sorted the way the panel sorts it
// (the pins first, the newest on top, then the board's order) with a pin again bringing the bottom pin to the top; that
// comparator's strict weak ordering over seeded random rows (std::stable_sort needs one); the stamp a pin gets (the clock, past
// the newest pin, never below 1, no overflow at the type's end; a frozen clock still orders a burst of pins); the pin a tab
// keeps when it switches conversation; the sessions.json round trip (written only for a pin, independent of the never-idle
// key, absent / zero / negative / non-number read as not pinned); and the registry: Update reaches the lean copies the cards
// read, and the card pin never holds a session out of Idle (that is the never-idle pin's hold, ApplyPinHold).
void TestSidePanelPins()
{
    std::wprintf(L"Side panel pins (SidePanelPins.h, SIDE_PANEL.md section 2j):\n");
    constexpr auto npos = std::wstring::npos;
    constexpr int64_t kMax = (std::numeric_limits<int64_t>::max)();

    // Which stamps are pins.
    CHECK(!IsSidePanelPinStamp(0), "pins: 0 is not a pin");
    CHECK(!IsSidePanelPinStamp(-5), "pins: a negative stamp (a hand edit) is not a pin");
    CHECK(IsSidePanelPinStamp(1) && IsSidePanelPinStamp(1'762'400'000'000) && IsSidePanelPinStamp(kMax), "pins: every positive stamp is a pin");

    // The pinned-first comparison.
    CHECK(CompareSidePanelPins(0, 0) == 0, "pins: two unpinned rows - the board's order decides");
    CHECK(CompareSidePanelPins(-3, 0) == 0 && CompareSidePanelPins(0, -3) == 0 && CompareSidePanelPins(-3, -9) == 0, "pins: a negative stamp ranks like an unpinned row");
    CHECK(CompareSidePanelPins(100, 0) < 0 && CompareSidePanelPins(0, 100) > 0, "pins: a pinned row goes before an unpinned one, from either side");
    CHECK(CompareSidePanelPins(100, -1) < 0 && CompareSidePanelPins(-1, 100) > 0, "pins: a pinned row goes before a negative-stamped one");
    CHECK(CompareSidePanelPins(200, 100) < 0 && CompareSidePanelPins(100, 200) > 0, "pins: the newer pin goes first - the last card pinned is the top card");
    CHECK(CompareSidePanelPins(100, 100) == 0, "pins: one stamp - the board's order decides");

    // A list sorted the way the side panel sorts it, then a pin again of its bottom pin, then an unpin.
    {
        std::vector<PinRow> rows = {
            { L"a", 0, 0 }, { L"b", 300, 4 }, { L"c", 0, 1 }, { L"d", 100, 2 }, { L"e", 0, 3 }, { L"f", 200, 5 }, { L"g", 100, 6 },
        };
        std::stable_sort(rows.begin(), rows.end(), PinRowLess);
        CHECK(PinRowOrder(rows) == L"bfdgace", "pins order: the pins first, newest on top (b 300, f 200; d and g share 100 and keep the board's order), then the rest in the board's order");
        // Pin again: g, the bottom pin, gets a stamp past the newest pin (b's 300) on a clock that reads 150 (stepped back).
        int64_t newest = 0;
        for (const auto& r : rows)
        {
            newest = (std::max)(newest, r.pin);
        }
        for (auto& r : rows)
        {
            if (r.id == L"g")
            {
                r.pin = NextSidePanelPinStamp(150, newest);
            }
        }
        std::stable_sort(rows.begin(), rows.end(), PinRowLess);
        CHECK(PinRowOrder(rows) == L"gbfdace", "pins order: a pin again brings the bottom pin back to the top, whatever the clock says");
        // Unpin b: it drops back into the board's order (rank 4: after a, c, e), the other pins keep theirs.
        for (auto& r : rows)
        {
            if (r.id == L"b")
            {
                r.pin = 0;
            }
        }
        std::stable_sort(rows.begin(), rows.end(), PinRowLess);
        CHECK(PinRowOrder(rows) == L"gfdaceb", "pins order: an unpinned card goes back to its place in the board's order");
        // Nothing pinned: exactly the board's order.
        for (auto& r : rows)
        {
            r.pin = 0;
        }
        std::stable_sort(rows.begin(), rows.end(), PinRowLess);
        CHECK(PinRowOrder(rows) == L"acdebfg", "pins order: with no pin the list is the board's order, untouched");
    }

    // The comparator is a strict weak ordering (irreflexive, asymmetric, transitive, and its "neither goes first" is transitive
    // too), over seeded random rows with few distinct pins and ranks, so ties of every kind meet.
    {
        bool irreflexive = true;
        bool asymmetric = true;
        bool transitive = true;
        bool equivTransitive = true;
        uint32_t seed = 0x5EED2026u;
        const auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u; // a fixed LCG: the same rows on every run
            return seed >> 8;
        };
        const int64_t pinChoices[] = { 0, -1, 5, 7, 9, 0 };
        for (int trial = 0; trial < 4; ++trial)
        {
            std::vector<PinRow> rows;
            for (int i = 0; i < 24; ++i)
            {
                rows.push_back(PinRow{ std::to_wstring(i), pinChoices[next() % 6], static_cast<int>(next() % 4) });
            }
            for (const auto& x : rows)
            {
                irreflexive = irreflexive && !PinRowLess(x, x);
                for (const auto& y : rows)
                {
                    asymmetric = asymmetric && !(PinRowLess(x, y) && PinRowLess(y, x));
                    for (const auto& z : rows)
                    {
                        if (PinRowLess(x, y) && PinRowLess(y, z))
                        {
                            transitive = transitive && PinRowLess(x, z);
                        }
                        const bool xyEquiv = !PinRowLess(x, y) && !PinRowLess(y, x);
                        const bool yzEquiv = !PinRowLess(y, z) && !PinRowLess(z, y);
                        if (xyEquiv && yzEquiv)
                        {
                            equivTransitive = equivTransitive && !PinRowLess(x, z) && !PinRowLess(z, x);
                        }
                    }
                }
            }
        }
        CHECK(irreflexive && asymmetric, "pins order: the comparator is irreflexive and asymmetric");
        CHECK(transitive && equivTransitive, "pins order: the comparator and its equivalence are transitive - a strict weak ordering std::stable_sort accepts");
    }

    // The stamp a pin gets.
    CHECK(NextSidePanelPinStamp(1000, 0) == 1000, "pin stamp: nothing pinned - the clock");
    CHECK(NextSidePanelPinStamp(1000, 999) == 1000, "pin stamp: the clock past the newest pin - the clock");
    CHECK(NextSidePanelPinStamp(1000, 1000) == 1001, "pin stamp: two pins in one millisecond - one past the newest");
    CHECK(NextSidePanelPinStamp(500, 1000) == 1001, "pin stamp: a clock stepped back - one past the newest, so the card is still the top one");
    CHECK(NextSidePanelPinStamp(0, 0) == 1 && NextSidePanelPinStamp(-7, 0) == 1, "pin stamp: never below 1 (a pin is a positive stamp)");
    CHECK(NextSidePanelPinStamp(5, -9) == 5, "pin stamp: a negative newest stamp counts as none");
    CHECK(NextSidePanelPinStamp(5, kMax) == kMax && NextSidePanelPinStamp(kMax, kMax) == kMax, "pin stamp: a newest stamp at the type's end returns that end - no wrap to negative");
    {
        // A burst of pins on a frozen clock (each pin again of the card that was on top a moment ago): every stamp is newer.
        int64_t newest = 0;
        bool increasing = true;
        for (int i = 0; i < 1000; ++i)
        {
            const int64_t stamp = NextSidePanelPinStamp(1'762'400'000'000, newest);
            increasing = increasing && stamp > newest && IsSidePanelPinStamp(stamp);
            newest = stamp;
        }
        CHECK(increasing && newest == 1'762'400'000'999, "pin stamp: 1000 pins in one millisecond each land on top (strictly newer stamps)");
    }

    // The pin a tab keeps when it switches conversation (a /clear, a /resume, a /handover-here).
    CHECK(SidePanelPinAfterRehome(0, 0) == 0, "pin carry: neither pinned - the successor is not pinned");
    CHECK(SidePanelPinAfterRehome(0, 500) == 500, "pin carry: the successor takes the replaced card's pin, at its place among the pins");
    CHECK(SidePanelPinAfterRehome(700, 500) == 700 && SidePanelPinAfterRehome(300, 500) == 500, "pin carry: the newer of the two stamps");
    CHECK(SidePanelPinAfterRehome(-1, 500) == 500 && SidePanelPinAfterRehome(500, -1) == 500 && SidePanelPinAfterRehome(-1, -1) == 0, "pin carry: a negative stamp counts as none");

    // sessions.json: the stamp round-trips, apart from Telegram's never-idle key.
    {
        SessionInfo p = MakeSession(L"spin-1");
        p.sidePanelPinnedAtUnixMs = 1'762'400'000'123;
        const auto text = SerializeSessions({ p });
        CHECK(text.find(L"\"sidePanelPinnedAtUnixMs\":1762400000123") != npos, "pins persist: a pinned card's stamp is written");
        CHECK(text.find(L"\"pinned\"") == npos, "pins persist: a card pin writes no never-idle key");
        const auto back = DeserializeSessions(text);
        CHECK(back.size() == 1 && back[0].sidePanelPinnedAtUnixMs == 1'762'400'000'123 && !back[0].pinned, "pins persist: the stamp reads back, and the session is not never-idle pinned");
        CHECK(SerializeSessions({ MakeSession(L"spin-2") }).find(L"sidePanelPinnedAtUnixMs") == npos, "pins persist: nothing written while unpinned (an untouched sessions.json is byte-unchanged)");
        SessionInfo tg = MakeSession(L"spin-3");
        tg.pinned = true;
        const auto tgBack = DeserializeSessions(SerializeSessions({ tg }));
        CHECK(tgBack.size() == 1 && tgBack[0].pinned && tgBack[0].sidePanelPinnedAtUnixMs == 0, "pins persist: Telegram's never-idle pin does not pin the card");
        // Hand edits of the stamp: zero, negative and a non-number read as not pinned; a real stamp stands.
        const std::wstring key = L"\"sidePanelPinnedAtUnixMs\":1762400000123";
        const auto edited = [&](const std::wstring& value) {
            std::wstring t = text;
            const size_t at = t.find(key);
            if (at != npos)
            {
                t.replace(at, key.size(), L"\"sidePanelPinnedAtUnixMs\":" + value);
            }
            const auto rows = DeserializeSessions(t);
            return rows.size() == 1 ? rows[0].sidePanelPinnedAtUnixMs : int64_t{ -42 };
        };
        CHECK(edited(L"0") == 0 && edited(L"-1762400000123") == 0 && edited(L"\"soon\"") == 0 && edited(L"null") == 0, "pins persist: a zero, negative, string or null stamp reads as not pinned");
        CHECK(edited(L"77") == 77, "pins persist: a small positive stamp is a pin");
    }

    // The registry: an Update reaches the lean copies the cards read, and the card pin holds no state.
    {
        SessionRegistry reg;
        SessionInfo r = MakeSession(L"spin-reg");
        r.live = true;
        reg.Upsert(r);
        CHECK(reg.Update(L"spin-reg", [](SessionInfo& s) { s.sidePanelPinnedAtUnixMs = 4242; }), "pins registry: Update knows the session");
        const auto lean = reg.GetWithoutQueue(L"spin-reg");
        const auto cards = reg.SnapshotLiveForCards();
        CHECK(lean && lean->sidePanelPinnedAtUnixMs == 4242, "pins registry: the one-session lean read carries the stamp (the menu reads it)");
        CHECK(cards.size() == 1 && cards[0].sidePanelPinnedAtUnixMs == 4242, "pins registry: the cards' lean snapshot carries the stamp (the list sorts by it)");
        reg.Update(L"spin-reg", [](SessionInfo& s) { s.state = SessionState::Idle; });
        const auto idle = reg.Get(L"spin-reg");
        CHECK(idle && idle->state == SessionState::Idle && !idle->pinned, "pins registry: a card pin never holds a session out of Idle (the never-idle hold is Telegram's pin)");
    }
}

namespace
{
    // One session's filter facts for TestSidePanelFilters: the folder keys from the given folders (NormDirKey, as the panel
    // computes them), the rest as given.
    //   workingDir  - the session's working folder ("" = none).
    //   inferredDir - its inferred folder ("" = none).
    //   state       - its state.
    //   tags        - its tags (null = none).
    //   returns     - the facts.
    SidePanelFilterFacts FilterFacts(const std::wstring& workingDir, const std::wstring& inferredDir, SessionState state = SessionState::Idle, const std::vector<std::wstring>* tags = nullptr)
    {
        SidePanelFilterFacts f;
        f.launchKey = workingDir.empty() ? std::wstring{} : NormDirKey(workingDir);
        f.inferredKey = inferredDir.empty() ? std::wstring{} : NormDirKey(inferredDir);
        f.state = state;
        f.tags = tags;
        return f;
    }
}

void TestSidePanelFilters()
{
    std::wprintf(L"Side panel filters (SidePanelFilters.h, SIDE_PANEL.md section 2l):\n");
    constexpr int64_t kNow = 1'762'600'000'000; // a fixed clock (2025-11-08)

    // The facet bits and the panel counts' question.
    {
        uint32_t all = 0;
        for (uint32_t i = 0; i < kSidePanelFacetCount; ++i)
        {
            all |= SidePanelFacetBit(static_cast<SidePanelFacet>(i));
        }
        CHECK(all == kSidePanelAllFacets && kSidePanelFacetCount == 11, "filters facets: the eleven facet bits are distinct and make the all-pass mask");
        CHECK(static_cast<int>(SidePanelFacet::Text) == 8 && static_cast<int>(SidePanelFacet::Branch) == 9 && static_cast<int>(SidePanelFacet::ForkFamily) == 10,
              "filters facets: Branch and ForkFamily were added at the enum's end - no earlier facet's value moved");
        const uint32_t allButState = kSidePanelAllFacets & ~SidePanelFacetBit(SidePanelFacet::State);
        CHECK(SidePanelPassesAllBut(allButState, SidePanelFacet::State), "filters facets: failing only By State passes 'every facet but State'");
        CHECK(!SidePanelPassesAllBut(allButState, SidePanelFacet::Tag), "filters facets: failing By State does not pass 'every facet but Tag'");
        CHECK(SidePanelPassesAllBut(kSidePanelAllFacets, SidePanelFacet::Active), "filters facets: a listed session passes 'every facet but X' for any X");
    }

    // The six states.
    {
        uint32_t seen = 0;
        bool distinct = true;
        for (const auto& choice : kSidePanelStateChoices)
        {
            distinct = distinct && (seen & choice.bit) == 0 && SidePanelStateBit(choice.state) == choice.bit;
            seen |= choice.bit;
        }
        CHECK(distinct && seen == kSidePanelAllStateBits, "filters states: six distinct bits, one per state, and they make the all-states mask");
        CHECK(std::wstring{ kSidePanelStateChoices[1].label } == L"Waiting for you" && std::wstring{ kSidePanelStateChoices[1].token } == L"waiting", "filters states: WaitingForInput reads 'Waiting for you' and stores 'waiting'");
    }

    // Active within: the boundary.
    {
        CHECK(SidePanelActiveWithin(0, kNow, 0), "filters active: any time (0) passes even an unknown activity");
        CHECK(!SidePanelActiveWithin(0, kNow, 60), "filters active: an unknown activity is never within a window");
        CHECK(SidePanelActiveWithin(kNow - 60 * 60'000, kNow, 60), "filters active: exactly 1 h old is within 1 h");
        CHECK(!SidePanelActiveWithin(kNow - 60 * 60'000 - 1, kNow, 60), "filters active: 1 h and 1 ms old is not within 1 h");
        CHECK(SidePanelActiveWithin(kNow + 5'000, kNow, 15), "filters active: a stamp in the future (clock skew) counts as now");
        CHECK(SidePanelActiveChipText(60) == L"active in 1 h" && SidePanelActiveChipText(7) == L"active in 7 min", "filters active: the chip names an offered window by its words, any other in minutes");
    }

    // Which records filter at all.
    {
        SidePanelFilters f;
        CHECK(!SidePanelFiltersActive(f) && SidePanelActiveFacets(f, false) == 0, "filters active: the empty record filters nothing");
        CHECK(SidePanelActiveFacets(f, true) == SidePanelFacetBit(SidePanelFacet::Text), "filters active: the typed text alone is the Text facet");
        f.untagged = true;
        CHECK(SidePanelFiltersActive(f) && SidePanelActiveFacets(f, false) == SidePanelFacetBit(SidePanelFacet::Tag), "filters active: Untagged alone is the Tag facet");
        f = {};
        f.favorite = true;
        f.activeWithinMinutes = 15;
        CHECK(SidePanelActiveFacets(f, false) == (SidePanelFacetBit(SidePanelFacet::Favorite) | SidePanelFacetBit(SidePanelFacet::Active)), "filters active: each flag and the window are their own facets");
    }

    // The edits.
    {
        SidePanelFilters f;
        CHECK(SidePanelAddFolderFilter(f, L"  \"K:/src/app/\"  ", true) && f.folders.size() == 1 && f.folders[0].path == L"K:\\src\\app" && f.folders[0].includeSubfolders,
              "filters edits: a folder is trimmed (spaces, quotes, '/' and the trailing separator) and keeps its subfolders");
        CHECK(!SidePanelAddFolderFilter(f, L"k:\\SRC\\app", true) && f.folders.size() == 1, "filters edits: the same folder in another case is no second pick");
        CHECK(SidePanelAddFolderFilter(f, L"k:\\SRC\\app", false) && f.folders.size() == 1 && !f.folders[0].includeSubfolders && f.folders[0].path == L"K:\\src\\app",
              "filters edits: picking it again unchecked makes it 'exactly' in place (its first spelling kept)");
        CHECK(!SidePanelAddFolderFilter(f, L"src\\app", true) && !SidePanelAddFolderFilter(f, L"K:", true) && !SidePanelAddFolderFilter(f, L"   ", true) && f.folders.size() == 1,
              "filters edits: a relative path, a bare drive and blanks are no folders");
        CHECK(SidePanelAddFolderFilter(f, L"\\\\server\\share\\x", true) && f.folders.size() == 2, "filters edits: a UNC folder is a folder");
        CHECK(SidePanelToggleState(f, kSidePanelStateChoices[0].bit) && f.states == kSidePanelStateChoices[0].bit, "filters edits: a state toggles on");
        CHECK(SidePanelToggleState(f, kSidePanelStateChoices[0].bit) && f.states == 0, "filters edits: and off again");
        CHECK(!SidePanelToggleState(f, 1u << 20) && f.states == 0, "filters edits: a bit no state owns changes nothing");
        CHECK(SidePanelToggleTag(f, L"Release") && f.tags.size() == 1 && f.tags[0] == L"Release", "filters edits: a tag toggles on in its spelling");
        CHECK(SidePanelToggleTag(f, L"release") && f.tags.empty(), "filters edits: and off again in any case");
        CHECK(!SidePanelToggleTag(f, L"   "), "filters edits: a blank tag name changes nothing");
        CHECK(SidePanelSetActiveWithin(f, 60) && f.activeWithinMinutes == 60 && !SidePanelSetActiveWithin(f, 60), "filters edits: a window is set once");
        CHECK(SidePanelSetActiveWithin(f, kSidePanelActiveMaxMinutes + 1) && f.activeWithinMinutes == 0, "filters edits: a window past a year reads as any time");
    }

    // The matcher.
    {
        const std::vector<std::wstring> releaseTags{ L"Release", L"ui" };
        const std::vector<std::wstring> noTags;
        // An empty record passes everything; the typed text is the caller's verdict.
        {
            const SidePanelFilterMatcher m{ SidePanelFilters{}, kNow };
            const auto facts = FilterFacts(L"K:\\a", L"");
            CHECK(m.Passed(facts, true) == kSidePanelAllFacets && m.Matches(facts, true), "filters matcher: no filter lists every session");
            CHECK(!m.Matches(facts, false) && m.Passed(facts, false) == (kSidePanelAllFacets & ~SidePanelFacetBit(SidePanelFacet::Text)), "filters matcher: the typed text's 'no' hides it, and only the Text facet fails");
            CHECK(!m.NeedsTags() && !m.NeedsFavorite() && !m.NeedsUnread(), "filters matcher: an empty record reads no tags, no star, no census");
        }
        // By Folder: subfolders (the default) vs exactly, the segment boundary, the inferred folder, OR between folders.
        {
            SidePanelFilters f;
            SidePanelAddFolderFilter(f, L"K:\\src", true);
            const SidePanelFilterMatcher sub{ f, kNow };
            CHECK(sub.Matches(FilterFacts(L"K:\\src", L""), true), "filters folder: the folder itself");
            CHECK(sub.Matches(FilterFacts(L"k:\\SRC\\app\\e2e", L""), true), "filters folder: a folder under it, in any case");
            CHECK(!sub.Matches(FilterFacts(L"K:\\srcfoo", L""), true), "filters folder: a prefix that is no folder boundary does not match");
            CHECK(sub.Matches(FilterFacts(L"C:\\Users\\me", L"K:\\src\\lib"), true), "filters folder: the inferred folder counts as where the session works");
            CHECK(!sub.Matches(FilterFacts(L"C:\\Users\\me", L""), true), "filters folder: a session elsewhere does not match");
            SidePanelAddFolderFilter(f, L"K:\\src", false);
            const SidePanelFilterMatcher exact{ f, kNow };
            CHECK(exact.Matches(FilterFacts(L"K:\\src", L""), true) && !exact.Matches(FilterFacts(L"K:\\src\\app", L""), true), "filters folder: 'exactly' keeps the folder and drops its subfolders");
            SidePanelAddFolderFilter(f, L"Q:\\tmp\\wt", true);
            const SidePanelFilterMatcher either{ f, kNow };
            CHECK(either.Matches(FilterFacts(L"Q:\\tmp\\wt\\x", L""), true) && either.Matches(FilterFacts(L"K:\\src", L""), true), "filters folder: two folders - a session in either is listed (OR)");
            SidePanelFilters root;
            SidePanelAddFolderFilter(root, L"K:\\", true);
            CHECK(SidePanelFilterMatcher(root, kNow).Matches(FilterFacts(L"K:\\anything\\deep", L""), true), "filters folder: a drive root covers the drive");
        }
        // By State: OR.
        {
            SidePanelFilters f;
            SidePanelToggleState(f, SidePanelStateBit(SessionState::Running));
            SidePanelToggleState(f, SidePanelStateBit(SessionState::WaitingForInput));
            const SidePanelFilterMatcher m{ f, kNow };
            CHECK(m.Matches(FilterFacts(L"", L"", SessionState::Running), true) && m.Matches(FilterFacts(L"", L"", SessionState::WaitingForInput), true), "filters state: either picked state is listed (OR)");
            CHECK(!m.Matches(FilterFacts(L"", L"", SessionState::Idle), true), "filters state: another state is not");
        }
        // By Tag: OR, any case, Untagged.
        {
            SidePanelFilters f;
            SidePanelToggleTag(f, L"release");
            SidePanelToggleTag(f, L"bug");
            const SidePanelFilterMatcher m{ f, kNow };
            CHECK(m.NeedsTags(), "filters tag: a tag pick reads the tags");
            CHECK(m.Matches(FilterFacts(L"", L"", SessionState::Idle, &releaseTags), true), "filters tag: a session carrying a picked tag in another case is listed");
            CHECK(!m.Matches(FilterFacts(L"", L"", SessionState::Idle, &noTags), true) && !m.Matches(FilterFacts(L"", L"", SessionState::Idle, nullptr), true), "filters tag: an untagged session is not (Untagged not picked)");
            f.untagged = true;
            const SidePanelFilterMatcher withUntagged{ f, kNow };
            CHECK(withUntagged.Matches(FilterFacts(L"", L"", SessionState::Idle, nullptr), true) && withUntagged.Matches(FilterFacts(L"", L"", SessionState::Idle, &releaseTags), true),
                  "filters tag: with Untagged picked, an untagged session OR a tagged one is listed");
            SidePanelFilters onlyUntagged;
            onlyUntagged.untagged = true;
            CHECK(!SidePanelFilterMatcher(onlyUntagged, kNow).Matches(FilterFacts(L"", L"", SessionState::Idle, &releaseTags), true), "filters tag: Untagged alone hides every tagged session");
        }
        // The flags (AND) and the window, and AND across facets.
        {
            SidePanelFilters f;
            f.draft = true;
            f.unread = true;
            const SidePanelFilterMatcher m{ f, kNow };
            auto facts = FilterFacts(L"", L"");
            facts.draft = true;
            CHECK(!m.Matches(facts, true), "filters flags: an unsent draft alone does not pass 'draft AND unread'");
            facts.unread = true;
            CHECK(m.Matches(facts, true) && m.NeedsUnread(), "filters flags: both flags hold - listed");
            SidePanelFilters active;
            active.activeWithinMinutes = 15;
            auto recent = FilterFacts(L"", L"");
            recent.lastActivityMs = kNow - 10 * 60'000;
            auto stale = recent;
            stale.lastActivityMs = kNow - 16 * 60'000;
            const SidePanelFilterMatcher w{ active, kNow };
            CHECK(w.Matches(recent, true) && !w.Matches(stale, true), "filters active: 10 min old is within 15 min, 16 min old is not");
            SidePanelFilters both;
            SidePanelAddFolderFilter(both, L"K:\\src", true);
            SidePanelToggleState(both, SidePanelStateBit(SessionState::Running));
            const SidePanelFilterMatcher bm{ both, kNow };
            const auto idleHere = FilterFacts(L"K:\\src\\app", L"", SessionState::Idle);
            const uint32_t passed = bm.Passed(idleHere, true);
            CHECK(!bm.Matches(idleHere, true) && SidePanelPassesAllBut(passed, SidePanelFacet::State) && !SidePanelPassesAllBut(passed, SidePanelFacet::Folder),
                  "filters AND: the right folder in the wrong state is hidden, and counts for By State's rows (every facet but State)");
            SidePanelFilters fav;
            fav.favorite = true;
            auto starred = FilterFacts(L"", L"");
            starred.favorite = true;
            CHECK(SidePanelFilterMatcher(fav, kNow).NeedsFavorite() && SidePanelFilterMatcher(fav, kNow).Matches(starred, true) && !SidePanelFilterMatcher(fav, kNow).Matches(FilterFacts(L"", L""), true),
                  "filters flags: Favorite lists the starred session alone");
        }
    }

    // The chips row.
    {
        CHECK(SidePanelFilterChips(SidePanelFilters{}, L"").empty(), "filters chips: no filter and no text - no chip (the row hides)");
        SidePanelFilters f;
        SidePanelAddFolderFilter(f, L"K:\\source\\Agentmaster", true);
        SidePanelAddFolderFilter(f, L"K:\\work\\webapp", false);
        SidePanelToggleState(f, SidePanelStateBit(SessionState::WaitingForInput));
        SidePanelToggleState(f, SidePanelStateBit(SessionState::Running));
        SidePanelToggleTag(f, L"release");
        f.untagged = true;
        f.draft = true;
        f.favorite = true;
        f.activeWithinMinutes = 60;
        const auto chips = SidePanelFilterChips(f, L"login");
        std::wstring kinds;
        for (const auto& c : chips)
        {
            kinds += std::to_wstring(static_cast<int>(c.kind));
        }
        CHECK(chips.size() == 8 && kinds == L"00123678", "filters chips: one per folder, the states, the tags, each flag set, the window, the text - in the facets' order");
        CHECK(chips.size() == 8 && chips[0].text == L"Agentmaster" && chips[1].text == L"webapp (exactly)" && chips[1].folderIndex == 1, "filters chips: a folder chip shows its leaf, '(exactly)' when unchecked");
        CHECK(chips.size() == 8 && chips[2].text == L"Running or Waiting for you", "filters chips: the states in the board's order, joined with 'or'");
        CHECK(chips.size() == 8 && chips[3].text == L"release or untagged", "filters chips: the tags, then 'untagged'");
        CHECK(chips.size() == 8 && chips[6].text == L"active in 1 h" && chips[7].text == L"\x201Clogin\x201D", "filters chips: the window's words and the typed text in quotes");
        CHECK(chips.size() == 8 && chips[0].tip.find(L"K:\\source\\Agentmaster") != std::wstring::npos && chips[1].tip.find(L"Exactly") != std::wstring::npos, "filters chips: a folder chip's tip names the whole path and its choice");
        // Each chip's x.
        const auto without = [&](size_t i) { return SidePanelFiltersWithoutChip(f, chips[i]); };
        CHECK(without(0).folders.size() == 1 && without(0).folders[0].path == L"K:\\work\\webapp", "filters chips x: a folder chip drops its folder alone");
        CHECK(without(2).states == 0 && without(2).folders.size() == 2, "filters chips x: the states chip drops every state, nothing else");
        CHECK(without(3).tags.empty() && !without(3).untagged, "filters chips x: the tags chip drops the tags and Untagged");
        CHECK(!without(4).draft && without(4).favorite, "filters chips x: a flag chip drops its flag alone");
        CHECK(without(6).activeWithinMinutes == 0, "filters chips x: the window chip drops the window");
        CHECK(without(7) == f, "filters chips x: the text chip leaves the record (the caller empties the box)");
        SidePanelFilterChip stale;
        stale.kind = SidePanelChipKind::Folder;
        stale.folderIndex = 9;
        CHECK(SidePanelFiltersWithoutChip(f, stale) == f, "filters chips x: a folder index past the record drops nothing");
        CHECK(SidePanelFolderLeaf(L"K:\\") == L"K:\\" && SidePanelFolderLeaf(L"\\\\server\\share") == L"share" && SidePanelFolderLeaf(L"K:\\a\\b\\") == L"b", "filters chips: a folder's leaf (a drive root names itself)");
    }

    // PICKED VS APPLIED (SIDE_PANEL.md section 2l "Pause filter, Resume filter and a chip's click"; the user's request of
    // 2026-11-12): what applies of a record whose row is paused or whose chips are switched off, what each click leaves,
    // and what a pick and a typed text resume.
    {
        // The record every case starts from: two folders, two states, a tag, a flag and a window - five facets picked.
        SidePanelFilters f;
        SidePanelAddFolderFilter(f, L"K:\\source\\Agentmaster", true);
        SidePanelAddFolderFilter(f, L"K:\\work\\webapp", false);
        SidePanelToggleState(f, SidePanelStateBit(SessionState::Running));
        SidePanelToggleState(f, SidePanelStateBit(SessionState::WaitingForInput));
        SidePanelToggleTag(f, L"release");
        f.favorite = true;
        f.activeWithinMinutes = 60;
        const uint32_t picked = SidePanelActiveFacets(f, false);
        const uint32_t stateBit = SidePanelFacetBit(SidePanelFacet::State);
        const uint32_t favoriteBit = SidePanelFacetBit(SidePanelFacet::Favorite);

        // What applies.
        {
            CHECK(SidePanelFiltersInEffect(f) == f && SidePanelFacetsInEffect(f) == picked && SidePanelFiltersNarrow(f), "pause in effect: a record with nothing off applies whole");
            CHECK(!SidePanelFiltersNarrow(SidePanelFilters{}) && SidePanelFacetsInEffect(SidePanelFilters{}) == 0, "pause in effect: the empty record narrows nothing");
            auto paused = f;
            paused.paused = true;
            CHECK(SidePanelFiltersActive(paused) && SidePanelActiveFacets(paused, false) == picked, "pause in effect: a paused record still HOLDS its picks (the row shows, the picks are saved)");
            CHECK(SidePanelFiltersInEffect(paused) == SidePanelFilters{} && !SidePanelFiltersNarrow(paused) && SidePanelFacetsInEffect(paused) == 0, "pause in effect: a paused record applies nothing");
            CHECK(!SidePanelTextInEffect(paused, true, false) && !SidePanelAnyChipOn(paused, true, false), "pause in effect: the pause covers the typed text too (everything in the row)");
            CHECK(SidePanelTextInEffect(f, true, false) && !SidePanelTextInEffect(f, true, true) && !SidePanelTextInEffect(f, false, false), "pause in effect: the text applies while typed, not paused and its chip on");
            auto stateOff = f;
            stateOff.offFacets = stateBit;
            const auto applied = SidePanelFiltersInEffect(stateOff);
            CHECK(applied.states == 0 && applied.folders.size() == 2 && applied.tags.size() == 1 && applied.favorite && applied.activeWithinMinutes == 60 && applied.offFacets == 0,
                  "pause in effect: a chip switched off leaves its facet out and every other pick in");
            CHECK(!SidePanelFacetInEffect(stateOff, SidePanelFacet::State) && SidePanelFacetInEffect(stateOff, SidePanelFacet::Favorite) && SidePanelFacetPicked(stateOff, SidePanelFacet::State),
                  "pause in effect: the facet is picked but not in effect; the others are");
            CHECK(!SidePanelFacetInEffect(f, SidePanelFacet::Unread) && !SidePanelFacetInEffect(f, SidePanelFacet::Text), "pause in effect: a facet with no pick, and Text, are never in effect");
            auto oneFolderOff = f;
            oneFolderOff.folders[0].off = true;
            const auto oneFolder = SidePanelFiltersInEffect(oneFolderOff);
            CHECK(oneFolder.folders.size() == 1 && oneFolder.folders[0].path == L"K:\\work\\webapp" && !oneFolder.folders[0].off && SidePanelFacetInEffect(oneFolderOff, SidePanelFacet::Folder),
                  "pause in effect: one folder chip off - the other folder still applies, By Folder still narrows");
            auto bothFoldersOff = oneFolderOff;
            bothFoldersOff.folders[1].off = true;
            CHECK(SidePanelFiltersInEffect(bothFoldersOff).folders.empty() && !SidePanelFacetInEffect(bothFoldersOff, SidePanelFacet::Folder) && SidePanelFiltersNarrow(bothFoldersOff),
                  "pause in effect: every folder chip off - By Folder constrains nothing, the other facets still narrow");
            // The counts' view: the pause lifted, a switched-off chip still left out.
            auto pausedStateOff = stateOff;
            pausedStateOff.paused = true;
            const auto counts = SidePanelFiltersForCounts(pausedStateOff);
            CHECK(counts.states == 0 && counts.folders.size() == 2 && counts.favorite && !counts.paused, "pause counts: the funnel's counts ignore the pause (a pick resumes) and keep a switched-off chip out");
        }

        // The matcher compiles only what applies.
        {
            const auto idleElsewhere = FilterFacts(L"C:\\other", L"", SessionState::Idle);
            CHECK(!SidePanelFilterMatcher(f, kNow).Matches(idleElsewhere, true), "pause matcher: the record hides a session outside every pick");
            auto paused = f;
            paused.paused = true;
            const SidePanelFilterMatcher pm{ paused, kNow };
            CHECK(pm.Matches(idleElsewhere, true) && !pm.NeedsTags() && !pm.NeedsFavorite(), "pause matcher: a paused record lists every session and reads no tags and no star");
            SidePanelFilters stateOnly;
            SidePanelToggleState(stateOnly, SidePanelStateBit(SessionState::Running));
            SidePanelToggleTag(stateOnly, L"release");
            stateOnly.offFacets = SidePanelFacetBit(SidePanelFacet::Tag);
            const SidePanelFilterMatcher sm{ stateOnly, kNow };
            CHECK(sm.Matches(FilterFacts(L"", L"", SessionState::Running), true) && !sm.Matches(FilterFacts(L"", L"", SessionState::Idle), true) && !sm.NeedsTags(),
                  "pause matcher: a switched-off tag chip constrains nothing and reads no tags; the state chip still filters");
            auto folderOff = SidePanelFilters{};
            SidePanelAddFolderFilter(folderOff, L"K:\\src", true);
            SidePanelAddFolderFilter(folderOff, L"Q:\\wt", true);
            folderOff.folders[0].off = true;
            const SidePanelFilterMatcher fm{ folderOff, kNow };
            CHECK(!fm.Matches(FilterFacts(L"K:\\src\\app", L""), true) && fm.Matches(FilterFacts(L"Q:\\wt\\x", L""), true), "pause matcher: a switched-off folder keeps no session; the other folder does");
        }

        // The chips: what is off is dimmed, and still in the row.
        {
            auto paused = f;
            paused.paused = true;
            const auto chips = SidePanelFilterChips(paused, L"login");
            bool allOff = chips.size() == 7;
            for (const auto& c : chips)
            {
                allOff = allOff && c.off;
            }
            CHECK(allOff, "pause chips: a paused row keeps all seven chips - two folders, states, tags, favorite, window, text - every one dimmed");
            auto mixed = f;
            mixed.folders[1].off = true;
            mixed.offFacets = favoriteBit;
            const auto some = SidePanelFilterChips(mixed, L"login", /*textOff*/ true);
            CHECK(some.size() == 7 && !some[0].off && some[1].off && !some[2].off && !some[3].off && some[4].off && !some[5].off && some[6].off,
                  "pause chips: each chip follows its own switch - the second folder, Favorite and the text are dimmed, the rest are lit");
            const auto plain = SidePanelFilterChips(f, L"login");
            CHECK(plain.size() == 7 && !plain[0].off && !plain[6].off, "pause chips: nothing off - no chip is dimmed");
            CHECK(SidePanelChipFacet(SidePanelChipKind::Folder) == SidePanelFacet::Folder && SidePanelChipFacet(SidePanelChipKind::Active) == SidePanelFacet::Active && SidePanelChipFacet(SidePanelChipKind::Text) == SidePanelFacet::Text,
                  "pause chips: a chip's kind maps to its facet");
        }

        // Pause filter / Resume filter.
        {
            auto r = f;
            r.offFacets = stateBit;
            bool textOff = false;
            CHECK(SidePanelAnyChipOn(r, false, false), "pause link: with a chip on, the link reads Pause filter");
            CHECK(SidePanelPauseAll(r) && r.paused && r.offFacets == stateBit && !SidePanelPauseAll(r), "pause link: Pause filter pauses once and keeps every chip's own switch");
            CHECK(!SidePanelAnyChipOn(r, true, false), "pause link: paused - nothing is on, the link reads Resume filter");
            CHECK(SidePanelResumeAll(r, true, textOff) && !r.paused && r.offFacets == stateBit && !textOff, "pause link: Resume filter brings back exactly what applied before (the state chip stays off)");
            // Every chip switched off one by one: Resume filter switches them all on.
            auto none = f;
            none.folders[0].off = true;
            none.folders[1].off = true;
            none.offFacets = SidePanelActiveFacets(none, false) & kSidePanelOffFacetBits;
            bool typedOff = true;
            CHECK(!SidePanelAnyChipOn(none, true, typedOff), "pause link: every chip switched off one by one reads Resume filter too");
            CHECK(SidePanelResumeAll(none, true, typedOff) && none.offFacets == 0 && !none.folders[0].off && !none.folders[1].off && !typedOff && SidePanelFiltersInEffect(none) == f,
                  "pause link: Resume filter with nothing on switches every chip on, the text chip too");
            // A pause over a row whose chips were all off: Resume filter must still leave something applied.
            auto pausedNone = f;
            pausedNone.paused = true;
            pausedNone.folders[0].off = true;
            pausedNone.folders[1].off = true;
            pausedNone.offFacets = SidePanelActiveFacets(pausedNone, false) & kSidePanelOffFacetBits;
            bool noText = false;
            CHECK(SidePanelResumeAll(pausedNone, false, noText) && !pausedNone.paused && SidePanelFiltersNarrow(pausedNone) && pausedNone.offFacets == 0,
                  "pause link: Resume filter never leaves a row that applies nothing");
            auto idle = f;
            bool idleText = false;
            CHECK(!SidePanelResumeAll(idle, false, idleText) && idle == f, "pause link: Resume filter on a row that applies changes nothing");
        }

        // A chip's click.
        {
            auto r = f;
            bool textOff = false;
            const auto chips = SidePanelFilterChips(r, L"login");
            CHECK(chips.size() == 7 && SidePanelToggleChip(r, chips[2], true, textOff) && r.offFacets == stateBit && !r.paused, "chip click: the states chip switches By State off");
            CHECK(SidePanelToggleChip(r, chips[2], true, textOff) && r.offFacets == 0, "chip click: and on again");
            CHECK(SidePanelToggleChip(r, chips[1], true, textOff) && r.folders[1].off && !r.folders[0].off, "chip click: a folder chip switches its own folder alone");
            CHECK(SidePanelToggleChip(r, chips[6], true, textOff) && textOff && r.offFacets == 0, "chip click: the text chip switches the text off (the record keeps its switches)");
            CHECK(!SidePanelToggleChip(r, chips[6], false, textOff), "chip click: the text chip with an empty box changes nothing");
            SidePanelFilterChip staleFolder;
            staleFolder.kind = SidePanelChipKind::Folder;
            staleFolder.folderIndex = 9;
            SidePanelFilterChip noPick;
            noPick.kind = SidePanelChipKind::Unread;
            const auto untouched = r;
            CHECK(!SidePanelToggleChip(r, staleFolder, true, textOff) && !SidePanelToggleChip(r, noPick, true, textOff) && r == untouched, "chip click: a folder past the record and a facet with no pick change nothing");
            // While the row is paused, the clicked chip ALONE comes back.
            auto paused = f;
            paused.paused = true;
            bool pausedText = false;
            CHECK(SidePanelToggleChip(paused, chips[4], true, pausedText) && !paused.paused, "chip click paused: the click lifts the pause");
            CHECK(SidePanelFacetsInEffect(paused) == favoriteBit && pausedText && paused.folders[0].off && paused.folders[1].off,
                  "chip click paused: the clicked chip (Favorite) alone applies - every folder, every other facet and the text are switched off");
            CHECK(SidePanelActiveFacets(paused, false) == picked, "chip click paused: every pick is still in the row");
            auto pausedFolder = f;
            pausedFolder.paused = true;
            bool folderText = false;
            CHECK(SidePanelToggleChip(pausedFolder, chips[0], false, folderText) && !pausedFolder.folders[0].off && pausedFolder.folders[1].off && SidePanelFacetsInEffect(pausedFolder) == SidePanelFacetBit(SidePanelFacet::Folder) && !folderText,
                  "chip click paused: a folder chip brings back that folder alone (no text typed: its switch stays clear)");
            auto pausedTextRow = f;
            pausedTextRow.paused = true;
            bool onlyText = true;
            CHECK(SidePanelToggleChip(pausedTextRow, chips[6], true, onlyText) && !onlyText && !SidePanelFiltersNarrow(pausedTextRow) && SidePanelTextInEffect(pausedTextRow, true, onlyText),
                  "chip click paused: the text chip brings back the typed text alone");
        }

        // A pick resumes; a removal does not.
        {
            auto paused = f;
            paused.paused = true;
            paused.offFacets = favoriteBit;
            auto pick = paused;
            SidePanelToggleState(pick, SidePanelStateBit(SessionState::Idle));
            const auto afterPick = SidePanelFiltersAfterPick(paused, pick);
            CHECK(!afterPick.paused && afterPick.offFacets == favoriteBit, "pick resumes: a state added to a paused row resumes it; a chip switched off by hand stays off");
            auto unpick = paused;
            SidePanelToggleState(unpick, SidePanelStateBit(SessionState::Running));
            CHECK(SidePanelFiltersAfterPick(paused, unpick).paused, "pick resumes: a state REMOVED leaves the row paused");
            const auto chips = SidePanelFilterChips(paused, L"");
            CHECK(chips.size() == 6 && SidePanelFiltersAfterPick(paused, SidePanelFiltersWithoutChip(paused, chips[2])).paused, "pick resumes: a chip's x leaves the row paused");
            auto pauseOnly = f;
            SidePanelPauseAll(pauseOnly);
            CHECK(SidePanelFiltersAfterPick(f, pauseOnly) == pauseOnly, "pick resumes: Pause filter itself adds nothing - it passes through unchanged");
            // A pick in a facet whose chip is switched off switches that chip on.
            auto stateOff = f;
            stateOff.offFacets = stateBit | favoriteBit;
            auto moreState = stateOff;
            SidePanelToggleState(moreState, SidePanelStateBit(SessionState::Error));
            CHECK(SidePanelFiltersAfterPick(stateOff, moreState).offFacets == favoriteBit, "pick resumes: a state added while the states chip is off switches that chip on, and no other");
            auto tagOff = f;
            tagOff.offFacets = SidePanelFacetBit(SidePanelFacet::Tag);
            auto untagged = tagOff;
            untagged.untagged = true;
            auto moreTag = tagOff;
            SidePanelToggleTag(moreTag, L"bug");
            auto sameTag = tagOff;
            sameTag.tags[0] = L"RELEASE"; // the tag it already holds, in another case
            CHECK(SidePanelFiltersAfterPick(tagOff, untagged).offFacets == 0 && SidePanelFiltersAfterPick(tagOff, moreTag).offFacets == 0, "pick resumes: Untagged or a new tag switches the tags chip on");
            CHECK(SidePanelFiltersAfterPick(tagOff, sameTag).offFacets == SidePanelFacetBit(SidePanelFacet::Tag), "pick resumes: the same tag in another case is no new pick");
            auto activeOff = f;
            activeOff.offFacets = SidePanelFacetBit(SidePanelFacet::Active);
            auto moved = activeOff;
            SidePanelSetActiveWithin(moved, 1440);
            auto anyTime = activeOff;
            SidePanelSetActiveWithin(anyTime, 0);
            CHECK(SidePanelFiltersAfterPick(activeOff, moved).offFacets == 0, "pick resumes: a moved window switches its chip on");
            CHECK(NormalizeSidePanelFilters(SidePanelFiltersAfterPick(activeOff, anyTime)).offFacets == 0 && !SidePanelFacetPicked(anyTime, SidePanelFacet::Active),
                  "pick resumes: 'Any time' drops the window, and normalizing drops the switch of a chip that is gone");
            auto flagOn = paused;
            flagOn.unread = true;
            CHECK(!SidePanelFiltersAfterPick(paused, flagOn).paused, "pick resumes: a flag turned on resumes a paused row");
            // Folders: a new one, the same one with another choice, a dimmed one picked again.
            auto folderOff = f;
            folderOff.folders[0].off = true;
            folderOff.paused = true;
            auto repick = folderOff;
            CHECK(SidePanelAddFolderFilter(repick, L"k:\\SOURCE\\agentmaster", true) && !repick.folders[0].off && !repick.paused, "pick resumes: picking a dimmed folder again switches it on and resumes the row");
            auto same = f;
            same.paused = true;
            CHECK(SidePanelAddFolderFilter(same, L"K:\\work\\webapp", false) && !same.paused && same.folders == f.folders, "pick resumes: picking a folder again while paused resumes the row, the folders as they were");
            auto choice = folderOff;
            choice.folders[0].includeSubfolders = false; // an edit made past SidePanelAddFolderFilter: the rule still sees the pick
            const auto afterChoice = SidePanelFiltersAfterPick(folderOff, choice);
            CHECK(!afterChoice.paused && !afterChoice.folders[0].off, "pick resumes: a folder's other choice is a pick - its chip on, the row resumed");
            auto fresh = folderOff;
            fresh.folders.push_back(SidePanelFolderFilter{ L"Q:\\new", true });
            const auto afterFresh = SidePanelFiltersAfterPick(folderOff, fresh);
            CHECK(!afterFresh.paused && afterFresh.folders[0].off && !afterFresh.folders[2].off, "pick resumes: a new folder resumes the row and leaves a dimmed folder dimmed");
            auto dropped = folderOff;
            dropped.folders.erase(dropped.folders.begin() + 1);
            CHECK(SidePanelFiltersAfterPick(folderOff, dropped).paused, "pick resumes: a folder dropped leaves the row paused");
        }

        // Typing resumes; an emptied box does not; a pause with nothing left is dropped.
        {
            auto paused = f;
            paused.paused = true;
            CHECK(!SidePanelFiltersAfterTextChange(paused, true).paused, "pause text: typing in the box resumes a paused row");
            CHECK(SidePanelFiltersAfterTextChange(paused, false).paused, "pause text: emptying the box leaves a row with picks paused");
            SidePanelFilters onlyText;
            onlyText.paused = true; // the pause covers the typed text alone
            CHECK(SidePanelFiltersAfterTextChange(onlyText, false) == SidePanelFilters{}, "pause text: the box emptied with no pick left - the pause is dropped");
            auto keeps = onlyText;
            CHECK(!SidePanelSettlePause(keeps, true) && keeps.paused, "pause settle: a pause over the typed text alone stays while the text is there");
            CHECK(SidePanelSettlePause(keeps, false) && !keeps.paused && !SidePanelSettlePause(keeps, false), "pause settle: no pick and no text - the pause is dropped, once");
            auto withPicks = paused;
            CHECK(!SidePanelSettlePause(withPicks, false) && withPicks.paused, "pause settle: a pause over picks stays");
        }

        // The funnel's words.
        {
            CHECK(SidePanelFiltersStatusText(SidePanelFilters{}).empty(), "pause status: no pick - no words");
            CHECK(SidePanelFiltersStatusText(f) == L"5 filters on", "pause status: five facets on (two folders are one filter)");
            SidePanelFilters one;
            one.draft = true;
            CHECK(SidePanelFiltersStatusText(one) == L"1 filter on", "pause status: one filter, singular");
            auto paused = f;
            paused.paused = true;
            CHECK(SidePanelFiltersStatusText(paused) == L"5 filters paused", "pause status: a paused row");
            auto mixed = f;
            mixed.offFacets = stateBit;
            mixed.folders[0].off = true;
            CHECK(SidePanelFiltersStatusText(mixed) == L"4 filters on, 1 paused", "pause status: one chip off; By Folder still counts as on while one folder is");
            one.offFacets = SidePanelFacetBit(SidePanelFacet::Draft);
            CHECK(SidePanelFiltersStatusText(one) == L"1 filter paused", "pause status: the only filter switched off");
        }

        // Normalizing: a switch with no chip is dropped; the pause is kept.
        {
            SidePanelFilters raw;
            raw.draft = true;
            raw.offFacets = 0xFFFFFFFFu;
            raw.paused = true;
            const auto n = NormalizeSidePanelFilters(raw);
            CHECK(n.offFacets == SidePanelFacetBit(SidePanelFacet::Draft) && n.paused, "pause normalize: off bits are cut to the facets that hold a pick; the pause is kept");
            SidePanelFilters empty;
            empty.offFacets = stateBit;
            empty.paused = true;
            const auto e = NormalizeSidePanelFilters(empty);
            CHECK(e.offFacets == 0 && e.paused, "pause normalize: an empty record keeps no switch, and keeps a pause that may cover the typed text");
            SidePanelFilters dup;
            dup.folders.push_back(SidePanelFolderFilter{ L"K:\\a", true, true });
            dup.folders.push_back(SidePanelFolderFilter{ L"k:\\A", false, false });
            const auto d = NormalizeSidePanelFilters(dup);
            CHECK(d.folders.size() == 1 && d.folders[0].off && d.folders[0].includeSubfolders, "pause normalize: a duplicate folder keeps the first entry's switch with its spelling and choice");
        }

        // By Folder's pick history and its submenu never carry a switch.
        {
            auto record = f;
            record.folders[0].off = true;
            const auto history = SidePanelFolderHistoryAfterPicks(std::vector<SidePanelFolderFilter>{}, record.folders, false);
            CHECK(history.size() == 2 && !history[0].off && !history[1].off, "pause history: a folder switched off in a window enters the shared pick history ON");
            const auto lines = SidePanelFolderMenuLines(history, record);
            CHECK(lines.size() == 2 && lines[0].checked && lines[1].checked, "pause history: a submenu line is checked while its folder is PICKED, switched off or not");
            bool turnedOn = true;
            auto rec = record;
            CHECK(lines.size() == 2 && SidePanelToggleFolderLine(rec, lines[0], turnedOn) && !turnedOn && rec.folders.size() == 1, "pause history: a click on the checked line of a switched-off folder drops the folder");
            auto pausedRec = SidePanelFilters{};
            pausedRec.draft = true;
            pausedRec.paused = true;
            CHECK(lines.size() == 2 && SidePanelToggleFolderLine(pausedRec, lines[0], turnedOn) && turnedOn && !pausedRec.paused && !pausedRec.folders[0].off,
                  "pause history: a line turned on while the row is paused resumes it (a pick resumes)");
        }
    }

    // The record's JSON.
    {
        // json::Parse answers an optional (empty for text that does not parse): a null Value then, which the loader reads as
        // no filter - every text below parses.
        const auto ParseJsonOrNull = [](std::wstring_view text) {
            auto v = json::Parse(text);
            return v ? *v : json::Value{};
        };
        SidePanelFilters f;
        SidePanelAddFolderFilter(f, L"K:\\src", true);
        SidePanelAddFolderFilter(f, L"Q:\\wt", false);
        SidePanelToggleState(f, SidePanelStateBit(SessionState::NeedsApproval));
        SidePanelToggleState(f, SidePanelStateBit(SessionState::Done));
        SidePanelToggleTag(f, L"Release");
        f.untagged = true;
        f.draft = true;
        f.unread = true;
        f.pinned = true;
        f.favorite = true;
        f.activeWithinMinutes = 1440;
        const auto text = json::Dump(SidePanelFiltersToJson(f));
        const auto back = SidePanelFiltersFromJson(ParseJsonOrNull(text));
        CHECK(back == f, "filters json: a full record round-trips");
        CHECK(text.find(L"\"approval\"") != std::wstring::npos && text.find(L"Needs approval") == std::wstring::npos, "filters json: states are stored as tokens, never as their words");
        CHECK(json::Dump(SidePanelFiltersToJson(SidePanelFilters{})) == L"{}", "filters json: the empty record is an empty object");
        const auto bad = SidePanelFiltersFromJson(ParseJsonOrNull(LR"({"folders":[{"path":"src\\rel"},{"path":"K:\\a"},{"path":"k:\\A","subfolders":false},7,{"path":"K:\\b","subfolders":"no"}],
            "states":["running","teleporting",3,"waiting"],"tags":["x","X","",5,"y"],"untagged":"yes","draft":1,"favorite":true,"activeWithinMinutes":-5})"));
        CHECK(bad.folders.size() == 2 && bad.folders[0].path == L"K:\\a" && bad.folders[0].includeSubfolders && bad.folders[1].path == L"K:\\b" && bad.folders[1].includeSubfolders,
              "filters json: a relative folder, a duplicate by case and a non-object are dropped; a non-bool 'subfolders' reads as the default (included)");
        CHECK(bad.states == (SidePanelStateBit(SessionState::Running) | SidePanelStateBit(SessionState::WaitingForInput)), "filters json: an unknown state token and a non-string are ignored");
        CHECK(bad.tags.size() == 2 && bad.tags[0] == L"x" && bad.tags[1] == L"y", "filters json: tags deduped by case (the first spelling), blanks and non-strings dropped");
        CHECK(!bad.untagged && !bad.draft && bad.favorite && bad.activeWithinMinutes == 0, "filters json: a non-bool flag is off, a negative window is any time");
        CHECK(SidePanelFiltersFromJson(ParseJsonOrNull(L"{\"activeWithinMinutes\":1.5}")).activeWithinMinutes == 0 && SidePanelFiltersFromJson(ParseJsonOrNull(L"{\"activeWithinMinutes\":99999999}")).activeWithinMinutes == 0,
              "filters json: a fractional or huge window is any time");
        CHECK(!SidePanelFiltersActive(SidePanelFiltersFromJson(ParseJsonOrNull(L"[1,2]"))), "filters json: a non-object reads as no filter");
        SidePanelFilters masked;
        masked.states = 0xFFFFFFFFu;
        CHECK(NormalizeSidePanelFilters(masked).states == kSidePanelAllStateBits, "filters json: bits no state owns are cut on normalizing");
        // The switches (SIDE_PANEL.md section 2l "Pause filter, Resume filter and a chip's click"): a folder's "off", the
        // record's "off" tokens and "paused" - written only while set, read back tolerantly.
        auto switched = f;
        switched.folders[1].off = true;
        switched.offFacets = SidePanelFacetBit(SidePanelFacet::State) | SidePanelFacetBit(SidePanelFacet::Favorite);
        switched.paused = true;
        const auto switchedText = json::Dump(SidePanelFiltersToJson(switched));
        CHECK(SidePanelFiltersFromJson(ParseJsonOrNull(switchedText)) == switched, "filters json: a folder switched off, two chips switched off and the pause round-trip");
        CHECK(switchedText.find(L"\"off\":[\"state\",\"favorite\"]") != std::wstring::npos && switchedText.find(L"\"paused\":true") != std::wstring::npos,
              "filters json: the switched-off chips are stored as their facets' tokens, the pause as a flag");
        CHECK(text.find(L"\"off\"") == std::wstring::npos && text.find(L"\"paused\"") == std::wstring::npos, "filters json: a record with nothing off writes no switch (the object is as before the feature)");
        SidePanelFilters pausedOnly;
        pausedOnly.paused = true;
        CHECK(json::Dump(SidePanelFiltersToJson(pausedOnly)) == L"{}", "filters json: a pause that covers only the typed text is not saved");
        const auto odd = SidePanelFiltersFromJson(ParseJsonOrNull(LR"({"draft":true,"folders":[{"path":"K:\\a","off":"yes"},{"path":"K:\\b","off":true}],"off":["draft","teleport",3,"state"],"paused":"yes"})"));
        CHECK(odd.folders.size() == 2 && !odd.folders[0].off && odd.folders[1].off, "filters json: a non-bool folder switch reads as on; a true one as off");
        CHECK(odd.offFacets == SidePanelFacetBit(SidePanelFacet::Draft) && !odd.paused, "filters json: an unknown token, a non-string and a switch for a facet with no pick are dropped; a non-bool pause is no pause");
        CHECK(!SidePanelFiltersFromJson(ParseJsonOrNull(L"{\"paused\":true}")).paused && SidePanelFiltersFromJson(ParseJsonOrNull(L"{\"paused\":true,\"draft\":true}")).paused,
              "filters json: a pause with no pick is dropped on load (no typed text is saved); beside a pick it is kept");
    }

    // The window record: the key only while a filter is set, and a round trip through the whole record.
    {
        WindowRecord plain;
        plain.windowId = L"w-filters";
        CHECK(SerializeWindowRecord(plain).find(L"sidePanelFilters") == std::wstring::npos, "filters record: an unfiltered window writes no sidePanelFilters key");
        WindowRecord filtered = plain;
        SidePanelAddFolderFilter(filtered.sidePanelFilters, L"K:\\source\\Agentmaster", false);
        filtered.sidePanelFilters.pinned = true;
        const auto text = SerializeWindowRecord(filtered);
        CHECK(text.find(L"sidePanelFilters") != std::wstring::npos, "filters record: a filtered window writes the key");
        const auto back = DeserializeWindowRecord(text);
        CHECK(back.sidePanelFilters == filtered.sidePanelFilters, "filters record: the filters round-trip through the window record");
        CHECK(!SidePanelFiltersActive(DeserializeWindowRecord(L"{\"version\":1,\"window\":{\"windowId\":\"w\",\"sidePanelFilters\":\"nope\"}}").sidePanelFilters),
              "filters record: a sidePanelFilters of the wrong type reads as no filter");
        // The pause and the chips switched off are part of the window's filters: kept across a restart.
        WindowRecord pausedWindow = filtered;
        pausedWindow.sidePanelFilters.paused = true;
        pausedWindow.sidePanelFilters.folders[0].off = true;
        pausedWindow.sidePanelFilters.offFacets = SidePanelFacetBit(SidePanelFacet::Pinned);
        CHECK(DeserializeWindowRecord(SerializeWindowRecord(pausedWindow)).sidePanelFilters == pausedWindow.sidePanelFilters,
              "filters record: the pause and the switched-off chips round-trip through the window record");
        WindowRecord textPause = plain;
        textPause.sidePanelFilters.paused = true; // a pause over the typed text alone
        CHECK(SerializeWindowRecord(textPause).find(L"sidePanelFilters") == std::wstring::npos, "filters record: a pause with no pick writes no sidePanelFilters key");
    }

    // THE CARD'S FILTER MENU (SIDE_PANEL.md section 2l "The card's Filter menu"; the user's request of 2026-11-12): the two
    // facets only a card sets (Branch, ForkFamily), the fork family itself, the Folder submenu's levels and a line's dim
    // hint, and a pick that starts the filter over.
    {
        // Branch and ForkFamily as facets of the record.
        {
            SidePanelFilters f;
            f.branch = L"agentmaster";
            CHECK(SidePanelFiltersActive(f) && SidePanelActiveFacets(f, false) == SidePanelFacetBit(SidePanelFacet::Branch) && SidePanelFacetPicked(f, SidePanelFacet::Branch),
                  "card facets: a branch alone is the Branch facet");
            f.forkFamilyOf = L"11111111-aaaa-bbbb-cccc-000000000001";
            CHECK(SidePanelActiveFacets(f, false) == (SidePanelFacetBit(SidePanelFacet::Branch) | SidePanelFacetBit(SidePanelFacet::ForkFamily)) && SidePanelFiltersStatusText(f) == L"2 filters on",
                  "card facets: a fork family is a facet of its own (two filters on)");
            CHECK(SidePanelFiltersInEffect(f) == f, "card facets: both apply while nothing is off");
            auto off = f;
            off.offFacets = SidePanelFacetBit(SidePanelFacet::Branch);
            const auto applied = SidePanelFiltersInEffect(off);
            CHECK(applied.branch.empty() && applied.forkFamilyOf == f.forkFamilyOf && !SidePanelFacetInEffect(off, SidePanelFacet::Branch) && SidePanelFacetInEffect(off, SidePanelFacet::ForkFamily),
                  "card facets: the branch chip switched off leaves the branch out and the fork family in");
            CHECK((kSidePanelOffFacetBits & SidePanelFacetBit(SidePanelFacet::Branch)) != 0 && (kSidePanelOffFacetBits & SidePanelFacetBit(SidePanelFacet::ForkFamily)) != 0,
                  "card facets: both chips can be switched off by a click");
            // The matcher.
            auto here = FilterFacts(L"", L"");
            here.branch = L"agentmaster";
            auto elsewhere = FilterFacts(L"", L"");
            elsewhere.branch = L"Agentmaster";
            SidePanelFilters onlyBranch;
            onlyBranch.branch = L"agentmaster";
            const SidePanelFilterMatcher bm{ onlyBranch, kNow };
            CHECK(bm.NeedsBranch() && !bm.NeedsForkFamily() && bm.Matches(here, true) && !bm.Matches(elsewhere, true) && !bm.Matches(FilterFacts(L"", L""), true),
                  "card facets matcher: a branch matches exactly - another case and an unknown branch do not");
            SidePanelFilters onlyFamily;
            onlyFamily.forkFamilyOf = L"anchor";
            const SidePanelFilterMatcher fm{ onlyFamily, kNow };
            auto member = FilterFacts(L"", L"");
            member.inForkFamily = true;
            const uint32_t outsider = fm.Passed(FilterFacts(L"", L""), true);
            CHECK(fm.NeedsForkFamily() && !fm.NeedsBranch() && fm.Matches(member, true) && !fm.Matches(FilterFacts(L"", L""), true) && SidePanelPassesAllBut(outsider, SidePanelFacet::ForkFamily),
                  "card facets matcher: a fork family lists its members alone, and an outsider fails only that facet");
            auto pausedFamily = onlyFamily;
            pausedFamily.paused = true;
            CHECK(!SidePanelFilterMatcher(pausedFamily, kNow).NeedsForkFamily() && SidePanelFilterMatcher(pausedFamily, kNow).Matches(FilterFacts(L"", L""), true),
                  "card facets matcher: a paused fork family constrains nothing and asks for no membership");
            // The chips: after the folders, before the states.
            SidePanelFilters row;
            SidePanelAddFolderFilter(row, L"K:\\source\\Agentmaster", true);
            row.branch = L"agentmaster";
            row.forkFamilyOf = L"anchor";
            SidePanelToggleState(row, SidePanelStateBit(SessionState::Running));
            const auto chips = SidePanelFilterChips(row, L"login", false, L"login page e2e");
            std::wstring kinds;
            for (const auto& c : chips)
            {
                kinds += std::to_wstring(static_cast<int>(c.kind)) + L" ";
            }
            CHECK(chips.size() == 5 && kinds == L"0 9 10 1 8 ", "card chips: the row reads folder, branch, fork family, states, text");
            CHECK(chips.size() == 5 && chips[1].text == L"agentmaster" && chips[2].text == L"fork family: login page e2e" && chips[2].tip.find(L"login page e2e") != std::wstring::npos,
                  "card chips: the branch chip shows the branch, the family chip the session it is the family of");
            CHECK(SidePanelFilterChips(row, L"").size() == 4 && SidePanelFilterChips(row, L"")[2].text == L"fork family", "card chips: with no name known the family chip reads 'fork family'");
            CHECK(chips.size() == 5 && SidePanelFiltersWithoutChip(row, chips[1]).branch.empty() && SidePanelFiltersWithoutChip(row, chips[1]).forkFamilyOf == L"anchor" && SidePanelFiltersWithoutChip(row, chips[2]).forkFamilyOf.empty() && SidePanelFiltersWithoutChip(row, chips[2]).branch == L"agentmaster",
                  "card chips x: each of the two chips drops its own pick alone");
            bool textOff = false;
            auto toggled = row;
            CHECK(chips.size() == 5 && SidePanelToggleChip(toggled, chips[2], true, textOff) && toggled.offFacets == SidePanelFacetBit(SidePanelFacet::ForkFamily) && SidePanelFilterChips(toggled, L"")[2].off && !SidePanelFilterChips(toggled, L"")[1].off,
                  "card chips: a click on the family chip switches it off alone");
            CHECK(SidePanelChipFacet(SidePanelChipKind::Branch) == SidePanelFacet::Branch && SidePanelChipFacet(SidePanelChipKind::ForkFamily) == SidePanelFacet::ForkFamily, "card chips: the new kinds map to their facets");
            // A pick resumes.
            auto pausedRow = row;
            pausedRow.paused = true;
            pausedRow.offFacets = SidePanelFacetBit(SidePanelFacet::Branch);
            auto otherBranch = pausedRow;
            otherBranch.branch = L"main";
            const auto resumed = SidePanelFiltersAfterPick(pausedRow, otherBranch);
            CHECK(!resumed.paused && resumed.offFacets == 0, "card facets resume: another branch picked switches the branch chip on and resumes the row");
            auto noBranch = pausedRow;
            noBranch.branch.clear();
            CHECK(SidePanelFiltersAfterPick(pausedRow, noBranch).paused, "card facets resume: the branch dropped leaves the row paused");
        }

        // The JSON of the two facets, and what normalizing accepts.
        {
            const auto parse = [](std::wstring_view text) {
                auto v = json::Parse(text);
                return v ? *v : json::Value{};
            };
            SidePanelFilters f;
            f.branch = L"feature/side-panel";
            f.forkFamilyOf = L"11111111-aaaa-bbbb-cccc-000000000001";
            f.offFacets = SidePanelFacetBit(SidePanelFacet::Branch) | SidePanelFacetBit(SidePanelFacet::ForkFamily);
            const auto text = json::Dump(SidePanelFiltersToJson(f));
            CHECK(SidePanelFiltersFromJson(parse(text)) == f, "card facets json: the branch, the family's session and both switches round-trip");
            CHECK(text.find(L"\"branch\":\"feature/side-panel\"") != std::wstring::npos && text.find(L"\"forkFamilyOf\":\"11111111-aaaa-bbbb-cccc-000000000001\"") != std::wstring::npos && text.find(L"\"off\":[\"branch\",\"family\"]") != std::wstring::npos,
                  "card facets json: stored as the branch's name, the session's id and the two facets' tokens");
            const auto odd = SidePanelFiltersFromJson(parse(LR"({"branch":7,"forkFamilyOf":["x"],"draft":true})"));
            CHECK(odd.branch.empty() && odd.forkFamilyOf.empty() && odd.draft, "card facets json: a branch or an id that is not a string reads as not set");
            SidePanelFilters raw;
            raw.branch = L"  main \t";
            raw.forkFamilyOf = L"abc\ndef";
            const auto n = NormalizeSidePanelFilters(raw);
            CHECK(n.branch == L"main" && n.forkFamilyOf.empty(), "card facets normalize: blanks are cut; a value with a line break is not kept");
            CHECK(SidePanelOneLineOrEmpty(std::wstring(300, L'x'), kSidePanelBranchMaxChars).empty() && SidePanelOneLineOrEmpty(L"   ", 10).empty() && SidePanelOneLineOrEmpty(L"a b", 10) == L"a b",
                  "card facets normalize: a value past the length cap or all blanks is not kept; an inner space is");
        }

        // The fork family: the connected component of the fork links.
        {
            using Links = std::vector<SidePanelForkLink>;
            // root <- a <- a1 ; root <- b ; other <- c ; lone has no link.
            const Links links{ { L"a", L"root" }, { L"a1", L"a" }, { L"b", L"root" }, { L"c", L"other" } };
            const auto family = SidePanelForkFamily(L"a1", links);
            CHECK(family.size() == 4 && family.count(L"a1") && family.count(L"a") && family.count(L"root") && family.count(L"b"), "fork family: a fork, its parent, the root and the root's other fork are one family");
            CHECK(!family.count(L"c") && !family.count(L"other"), "fork family: an unrelated fork pair stays out");
            CHECK(SidePanelForkFamily(L"root", links) == family && SidePanelForkFamily(L"b", links) == family, "fork family: every member's family is the same set");
            const auto alone = SidePanelForkFamily(L"lone", links);
            CHECK(alone.size() == 1 && alone.count(L"lone"), "fork family: a session in no link is a family of one");
            const Links messy{ { L"A", L"Root" }, { L"a", L"ROOT" }, { L"x", L"x" }, { L"", L"root" }, { L"y", L"" } };
            const auto folded = SidePanelForkFamily(L"ROOT", messy);
            CHECK(folded.size() == 2 && folded.count(L"a") && folded.count(L"root"), "fork family: ids compare whatever their case; a duplicate, a self link and a half link are harmless");
            CHECK(SidePanelFoldId(L"11AA-bbCC") == L"11aa-bbcc", "fork family: an id folds its ASCII letters to lower case");
        }

        // The Folder submenu's levels: the card's folder and each folder above it, the root left out.
        {
            using Levels = std::vector<std::wstring>;
            CHECK(SidePanelFolderLevels(L"K:\\source\\Agentmaster\\e2e") == Levels({ L"K:\\source\\Agentmaster\\e2e", L"K:\\source\\Agentmaster", L"K:\\source" }),
                  "folder levels: the folder, then each parent, nearest first; the drive's root is left out");
            CHECK(SidePanelFolderLevels(L" \"K:/source/Agentmaster/\" ") == Levels({ L"K:\\source\\Agentmaster", L"K:\\source" }), "folder levels: any spelling is trimmed first");
            CHECK(SidePanelFolderLevels(L"K:\\source") == Levels({ L"K:\\source" }), "folder levels: a folder right under the root lists itself alone");
            CHECK(SidePanelFolderLevels(L"K:\\") == Levels({ L"K:\\" }), "folder levels: a card whose folder IS the root still gets that one line");
            CHECK(SidePanelFolderLevels(L"\\\\server\\share\\a\\b") == Levels({ L"\\\\server\\share\\a\\b", L"\\\\server\\share\\a" }), "folder levels: a share's root is left out too");
            CHECK(SidePanelFolderLevels(L"\\\\server\\share") == Levels({ L"\\\\server\\share" }), "folder levels: a share's root itself lists alone");
            CHECK(SidePanelFolderLevels(L"relative\\dir").empty() && SidePanelFolderLevels(L"").empty() && SidePanelFolderLevels(L"K:\\a\\b", 0).empty(), "folder levels: a relative path, nothing, or no room lists nothing");
            CHECK(SidePanelFolderLevels(L"K:\\1\\2\\3\\4\\5\\6\\7\\8\\9\\10").size() == kSidePanelFolderLevelsMax && SidePanelFolderLevels(L"K:\\1\\2\\3\\4", 2) == Levels({ L"K:\\1\\2\\3\\4", L"K:\\1\\2\\3" }),
                  "folder levels: the list is cut at the most levels");
        }

        // A Folder line's dim hint: the whole path while it is short, else its two ends around an ellipsis.
        {
            const std::wstring deep = L"K:\\source\\Agentmaster\\src\\cascadia\\TerminalApp\\AgentMaster\\tests"; // 64 characters
            CHECK(SidePanelFolderHint(L"K:\\source\\Agentmaster") == L"K:\\source\\Agentmaster" && SidePanelFolderHint(std::wstring(kSidePanelFolderHintMaxChars, L'a')).size() == kSidePanelFolderHintMaxChars,
                  "folder hint: a path that fits is shown whole, up to the cap exactly");
            CHECK(SidePanelFolderHint(deep) == L"K:\\source\\\x2026\\AgentMaster\\tests", "folder hint: a deep path keeps the root with its first folder, then as many whole trailing folders as fit");
            CHECK(SidePanelFolderHint(deep).size() <= kSidePanelFolderHintMaxChars && SidePanelFolderHint(deep, 40) == SidePanelFolderHint(deep) &&
                      SidePanelFolderHint(deep, 41) == L"K:\\source\\\x2026\\TerminalApp\\AgentMaster\\tests" && SidePanelFolderHint(deep, 41).size() == 41,
                  "folder hint: never longer than the cap; one more folder shows only once the cap has room for the whole of it");
            // A first folder that would take more than half the room is left out: the hint is the tail alone.
            CHECK(SidePanelFolderHint(L"K:\\a-very-long-first-folder-name\\projects\\webapp\\src") == L"\x2026\\projects\\webapp\\src", "folder hint: a long first folder gives its room to the path's end");
            // The folder's own name longer than the room: cut inside the name, the END kept (it tells the lines apart).
            const std::wstring longLeaf = L"K:\\w\\" + std::wstring(60, L'x') + L"END";
            const std::wstring leafHint = SidePanelFolderHint(longLeaf);
            CHECK(leafHint.size() == kSidePanelFolderHintMaxChars && leafHint.rfind(L"K:\\w\\\x2026", 0) == 0 && leafHint.size() >= 3 && leafHint.compare(leafHint.size() - 3, 3, L"END") == 0,
                  "folder hint: a name longer than the room is cut inside the name, keeping its end");
            CHECK(SidePanelFolderHint(L"\\\\server\\share\\projects\\agentmaster\\worktrees\\feature") == L"\\\\server\\\x2026\\worktrees\\feature", "folder hint: a share keeps its server as the head");
            CHECK(SidePanelFolderHint(deep, 7) == deep && SidePanelFolderHint(L"").empty(), "folder hint: no room for two ends, or nothing, is returned as it is");
            // Every hint is shorter than its path, and ends as the path ends.
            bool shorter = true;
            for (size_t cap = 8; cap < deep.size(); ++cap)
            {
                const std::wstring hint = SidePanelFolderHint(deep, cap);
                shorter = shorter && hint.size() <= cap && hint.size() < deep.size() && hint.back() == deep.back() && hint.find(L'\x2026') != std::wstring::npos;
            }
            CHECK(shorter, "folder hint: at every cap the hint fits, is shorter than the path, holds the ellipsis and ends as the path ends");
        }

        // A pick starts the filter over.
        {
            const auto pickOf = [](SidePanelCardPickKind kind, std::wstring value, bool subfolders = true, uint32_t stateBit = 0) {
                SidePanelCardPick p;
                p.kind = kind;
                p.value = std::move(value);
                p.includeSubfolders = subfolders;
                p.stateBit = stateBit;
                return p;
            };
            const uint32_t waiting = SidePanelStateBit(SessionState::WaitingForInput);
            const auto folderPick = pickOf(SidePanelCardPickKind::Folder, L"K:\\source\\Agentmaster\\e2e");
            const auto exactPick = pickOf(SidePanelCardPickKind::Folder, L"K:\\source\\Agentmaster\\e2e", false);
            const auto statePick = pickOf(SidePanelCardPickKind::State, L"", true, waiting);
            const auto tagPick = pickOf(SidePanelCardPickKind::Tag, L"Release");
            const auto untaggedPick = pickOf(SidePanelCardPickKind::Untagged, L"");
            const auto branchPick = pickOf(SidePanelCardPickKind::Branch, L"agentmaster");
            const auto familyPick = pickOf(SidePanelCardPickKind::ForkFamily, L"11111111-AAAA-bbbb-cccc-000000000001");
            // The record of each line: exactly that one thing.
            const auto folderOnly = SidePanelFiltersForCardPick(folderPick);
            CHECK(folderOnly.folders.size() == 1 && folderOnly.folders[0].includeSubfolders && SidePanelActiveFacets(folderOnly, false) == SidePanelFacetBit(SidePanelFacet::Folder), "card pick: a folder line is that folder with its subfolders, alone");
            CHECK(!SidePanelFiltersForCardPick(exactPick).folders[0].includeSubfolders, "card pick: the 'exactly' line is that folder without its subfolders");
            CHECK(SidePanelFiltersForCardPick(statePick).states == waiting && SidePanelFiltersForCardPick(tagPick).tags == std::vector<std::wstring>({ L"Release" }) && SidePanelFiltersForCardPick(untaggedPick).untagged,
                  "card pick: a state, a tag and Untagged each make their one pick");
            CHECK(SidePanelFiltersForCardPick(branchPick).branch == L"agentmaster" && SidePanelFiltersForCardPick(familyPick).forkFamilyOf == L"11111111-AAAA-bbbb-cccc-000000000001", "card pick: a branch and a fork family each make their one pick");
            CHECK(!SidePanelFiltersActive(SidePanelFiltersForCardPick(pickOf(SidePanelCardPickKind::Folder, L"relative"))) && !SidePanelFiltersActive(SidePanelFiltersForCardPick(pickOf(SidePanelCardPickKind::Tag, L"  "))) &&
                      !SidePanelFiltersActive(SidePanelFiltersForCardPick(pickOf(SidePanelCardPickKind::Branch, L""))) && !SidePanelFiltersActive(SidePanelFiltersForCardPick(pickOf(SidePanelCardPickKind::State, L"", true, 1u << 20))),
                  "card pick: a line that names nothing a filter can hold makes no filter");
            // Start over: whatever the filter held, it becomes exactly the pick.
            SidePanelFilters busy;
            SidePanelAddFolderFilter(busy, L"K:\\work\\webapp", true);
            SidePanelToggleState(busy, SidePanelStateBit(SessionState::Running));
            SidePanelToggleState(busy, waiting);
            busy.unread = true;
            busy.branch = L"main";
            busy.offFacets = SidePanelFacetBit(SidePanelFacet::State);
            busy.paused = true;
            auto record = busy;
            CHECK(SidePanelApplyCardPick(record, true, statePick) == SidePanelCardPickOutcome::StartedOver && record == SidePanelFiltersForCardPick(statePick),
                  "card pick start over: every other pick, every chip's switch and the pause are dropped - the filter is exactly the pick");
            CHECK(SidePanelFiltersNarrow(record) && SidePanelFiltersStatusText(record) == L"1 filter on", "card pick start over: the one pick applies");
            // The line that IS the whole filter shows a check, and its click drops the filter.
            CHECK(SidePanelCardPickIsWholeFilter(record, false, statePick) && !SidePanelCardPickIsWholeFilter(record, true, statePick), "card pick check: the line that is the whole filter is checked - never while text is typed (a chip too)");
            CHECK(!SidePanelCardPickIsWholeFilter(record, false, tagPick) && !SidePanelCardPickIsWholeFilter(busy, false, statePick), "card pick check: another line, or a filter that holds more, is not checked");
            auto dimmed = record;
            dimmed.offFacets = SidePanelFacetBit(SidePanelFacet::State);
            auto held = record;
            held.paused = true;
            CHECK(!SidePanelCardPickIsWholeFilter(dimmed, false, statePick) && !SidePanelCardPickIsWholeFilter(held, false, statePick), "card pick check: a pick that is switched off or paused is not checked (it must apply)");
            CHECK(SidePanelApplyCardPick(dimmed, false, statePick) == SidePanelCardPickOutcome::StartedOver && dimmed.offFacets == 0 && SidePanelFiltersNarrow(dimmed), "card pick start over: picking a dimmed pick again applies it");
            auto drop = record;
            CHECK(SidePanelApplyCardPick(drop, false, statePick) == SidePanelCardPickOutcome::Dropped && drop == SidePanelFilters{}, "card pick drop: a click on the checked line drops the filter");
            // Identity, not spelling.
            SidePanelFilters spelled;
            SidePanelAddFolderFilter(spelled, L"k:/SOURCE/agentmaster/E2E/", true);
            CHECK(SidePanelCardPickIsWholeFilter(spelled, false, folderPick) && !SidePanelCardPickIsWholeFilter(spelled, false, exactPick), "card pick check: a folder is the same in any spelling; its 'exactly' twin is another pick");
            SidePanelFilters tagged;
            SidePanelToggleTag(tagged, L"release");
            CHECK(SidePanelCardPickIsWholeFilter(tagged, false, tagPick), "card pick check: a tag is the same in any case");
            SidePanelFilters family;
            family.forkFamilyOf = L"11111111-aaaa-BBBB-cccc-000000000001";
            CHECK(SidePanelCardPickIsWholeFilter(family, false, familyPick), "card pick check: a session id is the same in any case");
            // A line that names nothing leaves the record alone.
            auto untouched = busy;
            CHECK(SidePanelApplyCardPick(untouched, false, pickOf(SidePanelCardPickKind::Folder, L"not a path")) == SidePanelCardPickOutcome::Nothing && untouched == busy, "card pick: a line that names nothing changes nothing");
        }
    }

    // By Folder's PICK HISTORY (its hover submenu, SIDE_PANEL.md section 2l "The By Folder submenu"; the user's request of
    // 2026-11-10): repicking moves to the top, the seeding of a window's own filters, the cap, the JSON and the file.
    using History = std::vector<SidePanelFolderFilter>;
    const History start{ { L"K:\\a", true }, { L"K:\\b", false }, { L"K:\\c", true } };
    {
        auto h = SidePanelFolderHistoryAfterPicks(start, History{ { L"K:\\d", true } }, false);
        CHECK(h.size() == 4 && h[0].path == L"K:\\d" && h[1].path == L"K:\\a" && h[3].path == L"K:\\c", "folder history: a new pick goes on top, the rest keep their order");
        h = SidePanelFolderHistoryAfterPicks(start, History{ { L"  k:/C/  ", false } }, false);
        CHECK(h.size() == 3 && h[0].path == L"k:\\C" && !h[0].includeSubfolders && h[1].path == L"K:\\a" && h[2].path == L"K:\\b",
              "folder history: repicking a folder (any spelling) moves it to the top in the new spelling and choice");
        CHECK(SidePanelFolderHistoryAfterPicks(start, History{ { L"K:\\a", true } }, false) == start, "folder history: repicking the top folder as it was changes nothing");
        h = SidePanelFolderHistoryAfterPicks(start, History{ { L"K:\\x", true }, { L"K:\\c", false } }, false);
        CHECK(h.size() == 4 && h[0].path == L"K:\\x" && h[1].path == L"K:\\c" && !h[1].includeSubfolders && h[2].path == L"K:\\a",
              "folder history: several picks at once: the first ends first");
        h = SidePanelFolderHistoryAfterPicks(start, History{ { L"K:\\b", true }, { L"K:\\e", false } }, true);
        CHECK(h.size() == 4 && h[0].path == L"K:\\e" && h[1].path == L"K:\\a" && h[2].path == L"K:\\b" && !h[2].includeSubfolders,
              "folder history: seeding (onlyMissing) adds only an unlisted folder, on top, and leaves a listed one where and as it is");
        h = SidePanelFolderHistoryAfterPicks(start, History{ { L"K:\\y", true }, { L"k:\\Y", false } }, false);
        CHECK(h.size() == 4 && h[0].path == L"K:\\y" && h[0].includeSubfolders, "folder history: two picks of one folder in one call: the first pick wins");
        h = SidePanelFolderHistoryAfterPicks(start, History{ { L"rel\\dir", true }, { L"", true }, { L"\"K:\\q\"", true } }, false);
        CHECK(h.size() == 4 && h[0].path == L"K:\\q", "folder history: a relative or empty pick is skipped; a quoted path loses its quotes");
        History many;
        for (int i = 0; i < 14; ++i)
        {
            many.push_back(SidePanelFolderFilter{ L"K:\\f" + std::to_wstring(i), true });
        }
        h = SidePanelFolderHistoryAfterPicks(many, History{ { L"K:\\new", true } }, false);
        CHECK(h.size() == kSidePanelFolderHistoryMax && h[0].path == L"K:\\new" && h.back().path == L"K:\\f8", "folder history: the cap keeps the newest ten, the oldest fall off");
        h = SidePanelFolderHistoryAfterPicks(many, History{ { L"K:\\f12", false } }, false);
        CHECK(h.size() == kSidePanelFolderHistoryMax && h[0].path == L"K:\\f12" && !h[0].includeSubfolders && h[1].path == L"K:\\f0",
              "folder history: repicking a folder past the cap brings it back to the top, never as a second copy");
        CHECK(NormalizeSidePanelFolderHistory(History{ { L"K:\\a", true }, { L"k:\\A\\", false }, { L"x", true } }) == History({ { L"K:\\a", true } }),
              "folder history: normalizing keeps a folder's first (newest) entry and drops a relative one");
    }
    {
        // json::Parse answers an optional (empty for text that does not parse): a null Value then.
        const auto parseOrNull = [](std::wstring_view text) {
            auto v = json::Parse(text);
            return v ? *v : json::Value{};
        };
        CHECK(SidePanelFolderHistoryFromJson(parseOrNull(json::Dump(SidePanelFolderHistoryToJson(start)))) == start, "folder history json: the history round-trips");
        const auto bad = SidePanelFolderHistoryFromJson(parseOrNull(LR"([{"path":"K:\\a"},{"path":"k:\\A","subfolders":false},7,{"path":"rel"},{"subfolders":true},{"path":"K:\\b","subfolders":"no"}])"));
        CHECK(bad == History({ { L"K:\\a", true }, { L"K:\\b", true } }),
              "folder history json: a duplicate by case, a non-object, a relative or missing path are dropped; a non-bool subfolders reads as included");
        CHECK(SidePanelFolderHistoryFromJson(parseOrNull(L"{\"folders\":[]}")).empty(), "folder history json: anything but an array is no history");
        const auto doc = SerializeFilterFolderHistory(start);
        CHECK(doc.find(L"\"version\"") != std::wstring::npos && DeserializeFilterFolderHistory(doc) == start, "folder history file: the document ({version, folders}) round-trips");
        CHECK(DeserializeFilterFolderHistory(L"").empty() && DeserializeFilterFolderHistory(L"not json").empty() && DeserializeFilterFolderHistory(L"[1]").empty(),
              "folder history file: no text, text that does not parse, or no object reads as no history");
        CHECK(json::Dump(FilterFoldersRoundTrip(parseOrNull(doc))) == json::Dump(parseOrNull(doc)), "folder history file: its round trip of its own document is the document");
    }
    {
        // The file itself, through the `...In` seams on a scratch profile: a direct harness run must never write the live
        // profile's history (AgentmasterStateDir() is the harness's, resolved once per process).
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am-filterfolders-" + NewSessionId();
        std::filesystem::create_directories(std::filesystem::path{ dir });
        const std::wstring file = dir + L"\\filter-folders.json";
        CHECK(LoadFilterFolderHistoryIn(dir).empty(), "folder history file: a fresh profile has no history");
        CHECK(RememberFilterFolderPicksIn(dir, History{}, true).empty() && !std::filesystem::exists(std::filesystem::path{ file }),
              "folder history file: a seeding that adds nothing writes no file");
        RememberFilterFolderPicksIn(dir, History{ { L"K:\\a", true } }, false);
        RememberFilterFolderPicksIn(dir, History{ { L"K:\\b", false } }, false);
        CHECK(LoadFilterFolderHistoryIn(dir) == History({ { L"K:\\b", false }, { L"K:\\a", true } }), "folder history file: picks are kept, the newest first");
        const auto seeded = RememberFilterFolderPicksIn(dir, History{ { L"K:\\a", false }, { L"K:\\c", true } }, true);
        CHECK(seeded == History({ { L"K:\\c", true }, { L"K:\\b", false }, { L"K:\\a", true } }) && LoadFilterFolderHistoryIn(dir) == seeded,
              "folder history file: a seeding adds only the missing folder, on top, and answers what the file now holds");
        const auto repicked = RememberFilterFolderPicksIn(dir, History{ { L"K:\\a", false } }, false);
        CHECK(repicked.size() == 3 && repicked[0].path == L"K:\\a" && !repicked[0].includeSubfolders && LoadFilterFolderHistoryIn(dir) == repicked,
              "folder history file: a repick moves the folder to the top in the file too");
        CHECK(ReadStateFileUtf8(file).text.find(L"\"writer\"") != std::wstring::npos, "folder history file: written as a state document (its writer stamp)");
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }

    // The submenu's LINES and their toggle: the record's unlisted folders first (checked), then the history, checked in the
    // record's spelling and choice; a click judged against the record as it is; a line turned off turns back on as it was.
    {
        SidePanelFilters record;
        SidePanelAddFolderFilter(record, L"K:\\z", true); // filters this window, but no history lists it
        SidePanelAddFolderFilter(record, L"k:\\B", true); // the history lists it as K:\b (exactly)
        const auto lines = SidePanelFolderMenuLines(start, record);
        CHECK(lines.size() == 4 && lines[0].path == L"K:\\z" && lines[0].checked, "folder menu: a filtering folder the history lacks is listed first, checked");
        CHECK(lines.size() == 4 && lines[1].path == L"K:\\a" && !lines[1].checked && lines[1].includeSubfolders, "folder menu: an unpicked line shows the history's spelling and choice");
        CHECK(lines.size() == 4 && lines[2].checked && lines[2].path == L"k:\\B" && lines[2].includeSubfolders,
              "folder menu: a line the window filters by is checked, in the record's spelling and choice");
        CHECK(lines.size() == 4 && lines[3].path == L"K:\\c" && !lines[3].checked, "folder menu: the history's order is kept");
        CHECK(SidePanelFolderMenuLines(History{}, SidePanelFilters{}).empty(), "folder menu: nothing picked and nothing filtering: no line");
        if (lines.size() == 4)
        {
            auto rec = record;
            bool on = true;
            CHECK(SidePanelToggleFolderLine(rec, lines[2], on) && !on && rec.folders.size() == 1 && rec.folders[0].path == L"K:\\z", "folder menu: a click on a checked line drops its folder");
            auto line = lines[2];
            CHECK(!SidePanelSyncFolderMenuLine(line, rec) && !line.checked && line.path == L"k:\\B" && line.includeSubfolders,
                  "folder menu: a line turned off keeps its words (it turns back on as it was)");
            CHECK(SidePanelToggleFolderLine(rec, line, on) && on && rec.folders.size() == 2 && rec.folders[1].includeSubfolders,
                  "folder menu: a click on an unchecked line adds its folder with the line's choice");
            CHECK(!SidePanelSyncFolderMenuLine(line, rec) && line.checked, "folder menu: the line turned on reads checked, its words unchanged");
            auto exact = rec;
            SidePanelAddFolderFilter(exact, L"K:\\b", false);
            CHECK(SidePanelSyncFolderMenuLine(line, exact) && !line.includeSubfolders && line.path == L"k:\\B",
                  "folder menu: a held folder whose choice moved: the line adopts the record's and asks for its words to be rewritten");
            auto stale = lines[1]; // drawn unchecked, but the record took the folder meanwhile
            auto took = record;
            SidePanelAddFolderFilter(took, L"K:\\a", true);
            CHECK(SidePanelToggleFolderLine(took, stale, on) && !on && took.folders.size() == 2, "folder menu: a click is judged against the record as it is, never the check drawn");
        }
        auto dropped = record;
        CHECK(SidePanelRemoveFolderFilter(dropped, L" k:/z/ ") && dropped.folders.size() == 1 && !SidePanelRemoveFolderFilter(dropped, L"K:\\nope"),
              "folder menu: removing matches a folder in any spelling; an unheld one changes nothing");
        CHECK(SidePanelFolderLineName(L"K:\\source\\kvnode", false) == L"kvnode (exactly)" && SidePanelFolderLineName(L"K:\\", true) == L"K:\\",
              "folder menu: a line reads like its chip (the leaf, and (exactly) without subfolders)");

        // A line's count rule is the matcher's By Folder verdict for that one folder.
        const SidePanelFolderMenuLine sub{ L"K:\\src", NormDirKey(L"K:\\src"), true, false };
        const SidePanelFolderMenuLine exactly{ L"K:\\src", NormDirKey(L"K:\\src"), false, false };
        CHECK(SidePanelFolderLineCovers(sub, FilterFacts(L"K:\\src\\app", L"")) && !SidePanelFolderLineCovers(exactly, FilterFacts(L"K:\\src\\app", L"")),
              "folder menu: a line with its subfolders covers a folder under it; exactly does not");
        CHECK(SidePanelFolderLineCovers(exactly, FilterFacts(L"C:\\Users\\me", L"k:\\SRC")) && !SidePanelFolderLineCovers(sub, FilterFacts(L"K:\\srcfoo", L"")),
              "folder menu: the inferred folder counts; a sibling that shares a prefix never does");
        bool agree = true;
        for (const bool withSub : { true, false })
        {
            SidePanelFilters only;
            SidePanelAddFolderFilter(only, L"K:\\src", withSub);
            const SidePanelFilterMatcher matcher(only, kNow);
            const auto& line = withSub ? sub : exactly;
            for (const auto* d : { L"K:\\src", L"K:\\src\\a\\b", L"K:\\srcfoo", L"K:\\", L"D:\\src", L"" })
            {
                const auto facts = FilterFacts(d, L"");
                agree = agree && SidePanelFolderLineCovers(line, facts) == ((matcher.Passed(facts, true) & SidePanelFacetBit(SidePanelFacet::Folder)) != 0);
            }
        }
        CHECK(agree, "folder menu: a line's coverage equals the matcher's By Folder verdict for that folder alone (with and without subfolders)");
    }
}

namespace
{
    // The strip's rule stated a second time, for the exhaustive cross-check of TestSidePanelStripReveal: one flat chain of
    // the cases in the order SIDE_PANEL.md section 2m lists them, changing two flags, where NextSidePanelStripReveal
    // returns early from nested blocks. A slip in either one shows as a disagreement.
    //   now     - the strip's state before the event.
    //   p       - the event.
    //   returns - the state the section's words give.
    SidePanelStripReveal ExpectedStripReveal(SidePanelStripReveal now, const SidePanelStripPointer& p)
    {
        const bool press = p.kind == SidePanelStripPointerKind::Press;
        const bool watch = p.kind == SidePanelStripPointerKind::Watch;
        bool shown = now.shown;
        bool dismissed = now.dismissed;
        if (press && p.onButton)
        {
            // the button's own press changes nothing
        }
        else if (press && now.shown)
        {
            shown = false; // the press belongs to what is under the strip
            dismissed = p.nearStrip;
        }
        else if (press && !p.hovers && p.nearStrip)
        {
            shown = true; // a finger's tap near the edge
            dismissed = false;
        }
        else if (press)
        {
            dismissed = now.dismissed && p.nearStrip; // a mouse press never shows
        }
        else if (watch && !p.hovers)
        {
            // a finger moved no cursor: the watch has nothing to judge
        }
        else if (!p.nearStrip)
        {
            if (!(now.shown && p.lingering))
            {
                shown = false; // away
                dismissed = false;
            }
        }
        else if (!watch && !now.shown)
        {
            if (p.inContact)
            {
                dismissed = true; // a drag passing by
            }
            else if (!now.dismissed && p.hovers)
            {
                shown = true; // the pointer came close
            }
        }
        return { shown, dismissed };
    }

    // A pointer event for the strip's rule, written in one line at the call.
    //   kind      - move, press or the watch's look.
    //   nearStrip - the pointer is near the strip's place.
    //   inContact - a button is held (or the pen / finger touches).
    //   hovers    - a mouse or a pen (false = a finger).
    //   onButton  - (press) it landed on the expand button.
    //   lingering - the after-collapse linger runs.
    //   returns   - the event.
    constexpr SidePanelStripPointer StripEvent(SidePanelStripPointerKind kind, bool nearStrip, bool inContact = false, bool hovers = true, bool onButton = false, bool lingering = false) noexcept
    {
        SidePanelStripPointer p{};
        p.kind = kind;
        p.nearStrip = nearStrip;
        p.inContact = inContact;
        p.hovers = hovers;
        p.onButton = onButton;
        p.lingering = lingering;
        return p;
    }
}

// TestSidePanelStripReveal pins WHEN the collapsed side panel's strip shows (AgentMaster/SidePanelStripReveal.h, SIDE_PANEL.md
// section 2m; the user's request of 2026-11-12, "The strip should autohide. If mouse comes close by 40pixels the strips
// appears", and their pick of the floating whole strip): the geometry - the user's 40 measured from the strip, so 60 DIPs
// from the docked edge of the content, inclusive, at any height, mirrored docked right, never outside the area, an
// unmeasured area and every NaN / negative input neutralized, plus a seeded mirror sweep; the rule's named cases - the
// approach, staying and leaving, a drag passing by, a press beside the button (the strip steps aside until the pointer
// has left), a press on the button, a finger's taps, the watch that only hides, the after-collapse linger; all 288
// combinations of the inputs against a second statement of the rule and its invariants; and one user's whole visit,
// event by event.
void TestSidePanelStripReveal()
{
    std::wprintf(L"Collapsed strip auto-hide (SidePanelStripReveal.h, SIDE_PANEL.md section 2m):\n");
    using Kind = SidePanelStripPointerKind;
    constexpr double kStrip = 20.0; // the strip's width in the app (kSidePanelStripPx)
    constexpr double kW = 1000.0;
    constexpr double kH = 600.0;

    // --- the geometry -------------------------------------------------------------------------------------------------
    static_assert(kSidePanelStripRevealDistance == 40.0, "the reveal distance is the user's 40");
    // Docked left: the strip's place is x in [0, 20); "within 40 of it" reaches x = 60, inclusive.
    CHECK(SidePanelStripPointerNear(true, kW, kH, 0.0, 300.0, kStrip), "strip near: docked left, the very edge is near");
    CHECK(SidePanelStripPointerNear(true, kW, kH, 10.0, 300.0, kStrip), "strip near: docked left, over the strip's own place");
    CHECK(SidePanelStripPointerNear(true, kW, kH, 59.75, 300.0, kStrip), "strip near: docked left, 39.75 from the strip");
    CHECK(SidePanelStripPointerNear(true, kW, kH, 60.0, 300.0, kStrip), "strip near: docked left, exactly 40 from the strip is still near");
    CHECK(!SidePanelStripPointerNear(true, kW, kH, 60.25, 300.0, kStrip), "strip near: docked left, 40.25 from the strip is away");
    CHECK(!SidePanelStripPointerNear(true, kW, kH, 500.0, 300.0, kStrip), "strip near: docked left, the middle of the content is away");
    CHECK(!SidePanelStripPointerNear(true, kW, kH, 990.0, 300.0, kStrip), "strip near: docked left, the OTHER edge is away");
    // Any height along the edge - the strip is as tall as the content.
    CHECK(SidePanelStripPointerNear(true, kW, kH, 30.0, 0.0, kStrip) && SidePanelStripPointerNear(true, kW, kH, 30.0, 599.75, kStrip),
          "strip near: the top row and the bottom row of the content count like the middle");
    // Outside the content area the pointer is never near, however close to the strip's place.
    CHECK(!SidePanelStripPointerNear(true, kW, kH, -0.25, 300.0, kStrip), "strip near: a pointer that left through the docked edge (x < 0) is away");
    CHECK(!SidePanelStripPointerNear(true, kW, kH, 30.0, -1.0, kStrip), "strip near: over the tab row above the content (y < 0) is away");
    CHECK(!SidePanelStripPointerNear(true, kW, kH, 30.0, 600.0, kStrip), "strip near: the first row below the content (y == height) is away");
    CHECK(!SidePanelStripPointerNear(false, kW, kH, 1000.0, 300.0, kStrip), "strip near: docked right, the first column past the content (x == width) is away");
    // Docked right: the mirror image.
    CHECK(SidePanelStripPointerNear(false, kW, kH, 999.75, 300.0, kStrip), "strip near: docked right, the very edge is near");
    CHECK(SidePanelStripPointerNear(false, kW, kH, 940.0, 300.0, kStrip), "strip near: docked right, exactly 40 from the strip is still near");
    CHECK(!SidePanelStripPointerNear(false, kW, kH, 939.75, 300.0, kStrip), "strip near: docked right, 40.25 from the strip is away");
    CHECK(!SidePanelStripPointerNear(false, kW, kH, 10.0, 300.0, kStrip), "strip near: docked right, the left edge is away");
    // Nothing to measure against: never near.
    CHECK(!SidePanelStripPointerNear(true, 0.0, kH, 0.0, 300.0, kStrip) && !SidePanelStripPointerNear(true, kW, 0.0, 0.0, 0.0, kStrip),
          "strip near: an unmeasured content area (a 0 width or height) has no edge to be near");
    CHECK(!SidePanelStripPointerNear(true, -5.0, kH, 0.0, 300.0, kStrip), "strip near: a negative width reads as unmeasured");
    {
        const double nan = std::nan("");
        CHECK(!SidePanelStripPointerNear(true, nan, kH, 10.0, 300.0, kStrip) && !SidePanelStripPointerNear(true, kW, nan, 10.0, 300.0, kStrip) &&
                  !SidePanelStripPointerNear(true, kW, kH, nan, 300.0, kStrip) && !SidePanelStripPointerNear(true, kW, kH, 10.0, nan, kStrip),
              "strip near: a NaN size or coordinate is never near");
        // A broken strip width or distance counts as 0, it never widens the reach.
        CHECK(SidePanelStripPointerNear(true, kW, kH, 40.0, 300.0, nan) && !SidePanelStripPointerNear(true, kW, kH, 40.25, 300.0, nan),
              "strip near: a NaN strip width counts as 0 (the reach is the 40 alone)");
        CHECK(SidePanelStripPointerNear(true, kW, kH, 20.0, 300.0, kStrip, nan) && !SidePanelStripPointerNear(true, kW, kH, 20.25, 300.0, kStrip, nan),
              "strip near: a NaN distance counts as 0 (only the strip's own place is near)");
    }
    CHECK(SidePanelStripPointerNear(true, kW, kH, 40.0, 300.0, -8.0) && !SidePanelStripPointerNear(true, kW, kH, 40.25, 300.0, -8.0),
          "strip near: a negative strip width counts as 0");
    CHECK(SidePanelStripPointerNear(true, kW, kH, 20.0, 300.0, kStrip, -1.0) && !SidePanelStripPointerNear(true, kW, kH, 20.25, 300.0, kStrip, -1.0),
          "strip near: a negative distance counts as 0");
    CHECK(SidePanelStripPointerNear(true, kW, kH, 100.0, 300.0, kStrip, 80.0) && !SidePanelStripPointerNear(true, kW, kH, 100.25, 300.0, kStrip, 80.0),
          "strip near: another distance moves the boundary with it (20 + 80)");
    // A content area narrower than the reach: the whole of it is near, from either dock.
    CHECK(SidePanelStripPointerNear(true, 50.0, kH, 49.0, 10.0, kStrip) && SidePanelStripPointerNear(false, 50.0, kH, 0.0, 10.0, kStrip),
          "strip near: a content area narrower than 60 is near all over");
    {
        // An infinite width (never produced by layout; a belt): docked left the edge is where it always is, docked right it
        // is infinitely far.
        CHECK(SidePanelStripPointerNear(true, HUGE_VAL, kH, 30.0, 10.0, kStrip) && !SidePanelStripPointerNear(false, HUGE_VAL, kH, 30.0, 10.0, kStrip),
              "strip near: an infinite width keeps the left edge and loses the right one");
    }
    {
        // The two docks are mirror images: a pointer x from the left edge docked left reads like one x from the right edge
        // docked right. Quarter-DIP positions and whole sizes, so every subtraction is exact.
        uint64_t seed = 0xA0761D6478BD642Full;
        const auto next = [&seed](uint64_t span) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return z % span;
        };
        size_t cases = 0;
        size_t mirrorBroken = 0; // the docks disagree about mirrored points
        size_t boundaryBroken = 0; // near does not equal "at most 60 from the docked edge"
        for (int i = 0; i < 20000; ++i)
        {
            const double width = static_cast<double>(1 + next(3000));
            const double height = static_cast<double>(1 + next(2000));
            const double quarters = static_cast<double>(1 + next(static_cast<uint64_t>(width) * 4 - 1)); // 1 .. 4*width - 1
            const double x = quarters / 4.0; // strictly inside (0, width)
            const double y = static_cast<double>(next(static_cast<uint64_t>(height)));
            const bool fromLeft = SidePanelStripPointerNear(true, width, height, x, y, kStrip);
            const bool fromRight = SidePanelStripPointerNear(false, width, height, width - x, y, kStrip);
            ++cases;
            if (fromLeft != fromRight)
            {
                ++mirrorBroken;
            }
            if (fromLeft != (x <= 60.0))
            {
                ++boundaryBroken;
            }
        }
        CHECK(cases == 20000 && mirrorBroken == 0, "strip near (sweep): docked right is the mirror image of docked left");
        CHECK(boundaryBroken == 0, "strip near (sweep): near is exactly 'at most 60 DIPs from the docked edge', whatever the area's size");
    }

    // --- the rule's named cases ----------------------------------------------------------------------------------------
    const SidePanelStripReveal hidden{ false, false };
    const SidePanelStripReveal shown{ true, false };
    const SidePanelStripReveal dismissed{ false, true };

    // The approach: a mouse arriving near the strip, no button held.
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Move, true)) == shown, "strip rule: the pointer comes close - the strip shows");
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Move, false)) == hidden, "strip rule: a move far from the strip shows nothing");
    // It stays while the pointer is near and goes when the pointer is away.
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Move, true)) == shown, "strip rule: it stays while the pointer stays near");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Move, true, /*inContact*/ true)) == shown, "strip rule: a held button does not hide a strip that shows");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Move, false)) == hidden, "strip rule: the pointer moves away - the strip hides, armed again");

    // A drag passing by (a text selection dragged to the first column) never brings it in, and it stays away until the
    // pointer has left and come back.
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Move, true, /*inContact*/ true)) == dismissed, "strip rule: a drag arriving near the strip keeps it hidden");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Move, true)) == dismissed, "strip rule: after that drag, the released pointer moving near still shows nothing");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Move, false)) == hidden, "strip rule: the pointer leaves - the strip is armed again");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Move, false, /*inContact*/ true)) == hidden, "strip rule: leaving with the button still held arms it too");

    // A press beside the button belongs to what is under the strip: the strip steps aside.
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, true, true)) == dismissed, "strip rule: a press beside the button hides the strip until the pointer has left");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, true, true, true, /*onButton*/ true)) == shown, "strip rule: a press ON the button keeps the strip under it");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Press, true, true)) == dismissed, "strip rule: a second press at the edge (a double-click) keeps it away");
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Press, true, true)) == hidden, "strip rule: a mouse press never shows the strip");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Press, false, true)) == hidden, "strip rule: a press far from the strip arms it (the pointer is away)");

    // A finger cannot hover: its tap near the edge is the approach; a tap beside the button sends the strip away again.
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Press, true, true, /*hovers*/ false)) == shown, "strip rule: a finger's tap near the edge shows the strip");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Press, true, true, false)) == shown, "strip rule: a finger's tap shows it even after it stepped aside");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, true, true, false)) == dismissed, "strip rule: a finger's tap beside the button hides it");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, true, true, false, /*onButton*/ true)) == shown, "strip rule: a finger's tap on the button keeps it for the click");
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Press, false, true, false)) == hidden, "strip rule: a finger's tap far from the edge shows nothing");
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Move, true, true, false)) == dismissed && NextSidePanelStripReveal(hidden, StripEvent(Kind::Move, true, false, false)) == hidden,
          "strip rule: a finger's move never shows the strip");

    // The watch (the cursor read while the strip shows or waits): it hides and arms, it never shows.
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Watch, true)) == shown, "strip watch: the cursor still near - the strip stays");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Watch, false)) == hidden, "strip watch: the cursor left the window - the strip hides");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Watch, false)) == hidden, "strip watch: the cursor left - a strip that stepped aside is armed again");
    CHECK(NextSidePanelStripReveal(dismissed, StripEvent(Kind::Watch, true)) == dismissed, "strip watch: the cursor still near - it stays stepped aside");
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Watch, true)) == hidden, "strip watch: the watch never shows the strip");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Watch, false, false, /*hovers*/ false)) == shown, "strip watch: after a finger's tap the cursor proves nothing - the strip stays");

    // The after-collapse linger: the strip shows for a moment wherever the pointer is.
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Move, false, false, true, false, /*lingering*/ true)) == shown, "strip linger: a move far away does not hide the just-collapsed strip");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Watch, false, false, true, false, true)) == shown, "strip linger: nor does the watch");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Watch, false)) == hidden, "strip linger: once it is over, the watch hides the strip");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, false, true, true, false, true)) == hidden, "strip linger: a press far from the strip ends it at once, armed");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, true, true, true, false, true)) == dismissed, "strip linger: a press beside the button ends it, stepped aside");
    CHECK(NextSidePanelStripReveal(shown, StripEvent(Kind::Press, true, true, true, true, true)) == shown, "strip linger: a press on the button keeps it for the click");
    CHECK(NextSidePanelStripReveal(hidden, StripEvent(Kind::Move, false, false, true, false, true)) == hidden && NextSidePanelStripReveal(hidden, StripEvent(Kind::Watch, true, false, true, false, true)) == hidden,
          "strip linger: lingering never SHOWS a hidden strip");

    // --- every combination against the second statement and the invariants ----------------------------------------------
    {
        const SidePanelStripReveal states[] = { hidden, shown, dismissed }; // shown + dismissed is not a state
        const Kind kinds[] = { Kind::Move, Kind::Press, Kind::Watch };
        size_t cases = 0;
        size_t disagree = 0; // the rule and its second statement differ
        size_t bothFlags = 0; // shown and dismissed at once
        size_t watchShowed = 0; // the watch turned a hidden strip on
        size_t mousePressShowed = 0; // a mouse or pen press turned a hidden strip on
        size_t showedFromAfar = 0; // a hidden strip turned on with the pointer away
        size_t buttonPressChanged = 0; // a press on the button changed anything
        size_t awayKept = 0; // an away pointer (no linger, a hovering device) left the strip shown or dismissed
        for (const auto& state : states)
        {
            for (const auto kind : kinds)
            {
                for (unsigned bits = 0; bits < 32; ++bits)
                {
                    const auto p = StripEvent(kind, (bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0, (bits & 8u) != 0, (bits & 16u) != 0);
                    const auto got = NextSidePanelStripReveal(state, p);
                    const auto want = ExpectedStripReveal(state, p);
                    ++cases;
                    if (!(got == want))
                    {
                        ++disagree;
                        std::wprintf(L"    state %d/%d kind %d bits 0x%02X: got %d/%d, expected %d/%d (shown/dismissed)\n",
                                     state.shown ? 1 : 0,
                                     state.dismissed ? 1 : 0,
                                     static_cast<int>(kind),
                                     bits,
                                     got.shown ? 1 : 0,
                                     got.dismissed ? 1 : 0,
                                     want.shown ? 1 : 0,
                                     want.dismissed ? 1 : 0);
                    }
                    bothFlags += (got.shown && got.dismissed) ? 1 : 0;
                    watchShowed += (kind == Kind::Watch && !state.shown && got.shown) ? 1 : 0;
                    mousePressShowed += (kind == Kind::Press && p.hovers && !state.shown && got.shown) ? 1 : 0;
                    showedFromAfar += (!state.shown && got.shown && !p.nearStrip) ? 1 : 0;
                    buttonPressChanged += (kind == Kind::Press && p.onButton && !(got == state)) ? 1 : 0;
                    awayKept += (kind != Kind::Press && !p.nearStrip && !p.lingering && p.hovers && !(got == hidden)) ? 1 : 0;
                }
            }
        }
        CHECK(cases == 288 && disagree == 0, "strip rule: all 288 combinations match the second statement of the rule");
        CHECK(bothFlags == 0, "strip rule: never shown and stepped aside at once");
        CHECK(watchShowed == 0, "strip rule: the watch never shows the strip");
        CHECK(mousePressShowed == 0, "strip rule: a mouse or pen press never shows the strip");
        CHECK(showedFromAfar == 0, "strip rule: the strip never appears while the pointer is away");
        CHECK(buttonPressChanged == 0, "strip rule: a press on the button never changes the strip");
        CHECK(awayKept == 0, "strip rule: an away pointer (no linger) always leaves the strip hidden and armed");
    }

    // --- one visit, event by event --------------------------------------------------------------------------------------
    {
        // The pointer positions go through the real geometry (docked left, a 1000 x 600 content area).
        const auto at = [&](double x) { return SidePanelStripPointerNear(true, kW, kH, x, 300.0, kStrip); };
        SidePanelStripReveal s{}; // a window that opens with its panel collapsed: hidden
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(400.0)));
        CHECK(s == hidden, "strip visit: working in the middle of the terminal - nothing shows");
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(61.0)));
        CHECK(s == hidden, "strip visit: 41 from the strip - still nothing");
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(58.0)));
        CHECK(s == shown, "strip visit: 38 from the strip - it shows");
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(12.0)));
        CHECK(s == shown, "strip visit: over the strip itself - it stays");
        // A click on the terminal's first column, beside the button: the strip steps aside for it.
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Press, at(12.0), true));
        CHECK(s == dismissed, "strip visit: a click beside the button - the strip steps aside");
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(14.0), true));
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(30.0)));
        CHECK(s == dismissed, "strip visit: selecting and moving at the edge - it stays away");
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(200.0)));
        CHECK(s == hidden, "strip visit: the pointer leaves the edge - armed again");
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(40.0)));
        CHECK(s == shown, "strip visit: back at the edge - it shows again");
        // The pointer leaves the window through the edge: no move reaches the content, the watch sees it.
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Watch, at(-80.0)));
        CHECK(s == hidden, "strip visit: the pointer left the window through the edge - the watch hides the strip");
        // Back in, onto the button, and a click on it: the strip is still there for the click.
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Move, at(8.0)));
        s = NextSidePanelStripReveal(s, StripEvent(Kind::Press, at(8.0), true, true, /*onButton*/ true));
        CHECK(s == shown, "strip visit: the click on the button finds the strip shown (its click expands the panel)");
    }

    // constexpr: both functions are pure and usable at compile time.
    {
        static_assert(SidePanelStripPointerNear(true, 1000.0, 600.0, 60.0, 0.0, 20.0), "40 DIPs from a 20 DIP strip is near");
        static_assert(!SidePanelStripPointerNear(true, 1000.0, 600.0, 61.0, 0.0, 20.0), "41 DIPs from it is away");
        static_assert(NextSidePanelStripReveal(SidePanelStripReveal{}, StripEvent(SidePanelStripPointerKind::Move, true)).shown, "an approach shows the strip");
        static_assert(!NextSidePanelStripReveal(SidePanelStripReveal{}, StripEvent(SidePanelStripPointerKind::Watch, true)).shown, "the watch never does");
        static_assert(kSidePanelStripCollapseLingerMs >= 1000 && kSidePanelStripCollapseLingerMs <= 3000,
                      "the after-collapse linger is long enough to be seen (1 s) and short enough not to read as stuck (3 s)");
    }
}
