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

#include "m5_tests.h"

#include "../KeyedListDiff.h"
#include "../PointerCardPlacement.h" // the "+" launcher card's tooltip-like placement (TestPointerCardPlacement)
#include "../SessionTimingText.h" // the cards' and rows' "created 4d1h, active 2m4s ago" (TestSessionTimingText)

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
