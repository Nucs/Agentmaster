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

#include "m5_tests.h"

#include "../KeyedListDiff.h"
#include "../PointerCardPlacement.h" // the "+" launcher card's tooltip-like placement (TestPointerCardPlacement)
#include "../SessionTimingText.h" // the cards' and rows' "created 4d1h, active 2m4s ago" (TestSessionTimingText)
#include "../StatusIconMotion.h" // the status icon's motion rule, shared by the tab header and the card icon (TestStatusIconMotion)
#include "../VirtualListWindow.h" // the virtualized card list's realized rows (TestVirtualListWindow)

#include <algorithm>
#include <cmath> // std::fabs / std::nan / HUGE_VAL - TestPointerCardPlacement
#include <cstdint>
#include <string>
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
