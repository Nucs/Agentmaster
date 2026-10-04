// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - the SIDE PANEL's pure helpers (SIDE_PANEL.md section 2c).
//
// TestKeyedListDiff pins the card list's order planner (AgentMaster/KeyedListDiff.h): for every
// (current, target) pair it checks that applying the planned operations in order turns current into
// EXACTLY target, that every index is in range when its operation runs (the XAML panel would throw
// E_INVALIDARG otherwise), and that the plan is MINIMAL - one Remove per vanished key, one Insert per
// new key, and |survivors| - LIS moves (checked against an independent O(n^2) longest-increasing-
// subsequence count, so a bug in the planner's patience-sorting LIS can't hide behind itself).
// Exhaustive over every permutation of 5 keys plus a seeded random sweep with removals and insertions.

#include "m5_tests.h"

#include "../KeyedListDiff.h"

#include <algorithm>
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
