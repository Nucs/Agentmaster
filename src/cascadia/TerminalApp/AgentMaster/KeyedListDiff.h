// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SIDE_PANEL.md section 2c - the side panel's card list, updated in place): plan the
// FEWEST list operations that turn one ordered list of unique keys into another.
//
// WHY: the side panel used to Clear() its card list and build every card again on each registry
// change (466 cards in the big window, up to twice a second). The list now keeps one card per
// session and only edits what changed; this header decides how the ORDER changes: which cards to
// remove, which new cards to insert where, and which surviving cards to move. A XAML panel's
// UIElementCollection.Move is a plain reorder of its item array (WinUI's CDOCollection::MoveInternal
// is a std::rotate - no Unloaded/Loaded, no re-templating), so a reorder costs one Move per card
// that really changed place.
//
// HOW (the keyed-children diff of the Inferno / ivi / Vue 3 family):
//   1. Remove every key that is not in the target, from the highest index down (each index then
//      stays valid when its operation runs).
//   2. Among the survivors, keep in place the longest run whose current order already matches the
//      target order (a longest increasing subsequence of their target positions, patience-sorted);
//      no surviving element outside it can stay, so the number of moves is the minimum.
//   3. Walk the target from its END to its START and put every new key (Insert) and every survivor
//      outside the kept run (Move) immediately in front of its target successor, which is already
//      in place by then. An empty starting list is filled front to back instead (plain appends).
// The result: exactly |current - target| removes, exactly |target - current| inserts, and exactly
// |survivors| - LIS moves (never more; a planned move that would land where the element already is,
// is dropped).
//
// Pure, header-only, pure-ASCII source (the PromptAnchor.h idiom): no Windows, no WinRT, so the
// m5 harness tests it without a build of the app. Unit-tested: tests_sidepanel.cpp
// (TestKeyedListDiff).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Agentmaster
{
    // The three operations a planned list edit is made of.
    enum class KeyedListOpKind : uint8_t
    {
        Remove, // take the element at `from` out of the list
        Insert, // put the NEW element `key` at index `to`
        Move, // take the element at `from` out and put it back at index `to`
    };

    // One planned operation. Indexes are valid AT THE MOMENT THE OPERATION RUNS, after every earlier
    // operation of the same plan - apply them in order, each exactly once.
    //   Remove: `from` < size; `to` unused (0).
    //   Insert: `to` <= size (size == append); `from` unused (0).
    //   Move:   `from` < size and `to` < size; `to` is the index the element has AFTER the move (the
    //           semantics of XAML's UIElementCollection.Move and of ApplyKeyedListOp below).
    struct KeyedListOp
    {
        KeyedListOpKind kind{ KeyedListOpKind::Remove };
        uint32_t from{ 0 };
        uint32_t to{ 0 };
        std::wstring key; // the element the operation is about (Insert: the element to create)
    };

    namespace detail
    {
        // One longest STRICTLY increasing subsequence of `seq` (patience sorting with back links,
        // O(n log n)).
        //   seq     - the values; the planner passes distinct target positions.
        //   returns - the INDEXES into `seq` of one longest strictly increasing subsequence, in
        //             increasing order. Empty for an empty input.
        //   throws  - std::bad_alloc on allocation failure only.
        inline std::vector<size_t> LongestIncreasingSubsequenceIndexes(const std::vector<size_t>& seq)
        {
            // tails[L] = the index (into seq) of the smallest last value of any increasing run of
            // length L + 1 seen so far; prev[i] = the index before i in the run that ends at i.
            std::vector<size_t> tails;
            std::vector<size_t> prev(seq.size(), SIZE_MAX);
            for (size_t i = 0; i < seq.size(); ++i)
            {
                // The first run whose last value is >= seq[i]: seq[i] replaces that tail (a strictly
                // increasing run can't extend it), or extends the longest run when none is.
                const auto it = std::lower_bound(tails.begin(), tails.end(), seq[i], [&seq](size_t tailIndex, size_t value) {
                    return seq[tailIndex] < value;
                });
                if (it != tails.begin())
                {
                    prev[i] = *(it - 1);
                }
                if (it == tails.end())
                {
                    tails.push_back(i);
                }
                else
                {
                    *it = i;
                }
            }
            std::vector<size_t> out(tails.size());
            size_t cursor = tails.empty() ? SIZE_MAX : tails.back();
            for (size_t pos = tails.size(); pos-- > 0;)
            {
                out[pos] = cursor;
                cursor = prev[cursor];
            }
            return out;
        }

        // The position of `value` in `list` (a linear search: the plan's lists hold small integers,
        // and a plan only searches for the elements it moves).
        //   returns - the index, or list.size() when `value` is not in the list.
        //   throws  - nothing.
        inline size_t IndexOf(const std::vector<size_t>& list, size_t value) noexcept
        {
            const auto it = std::find(list.begin(), list.end(), value);
            return static_cast<size_t>(it - list.begin());
        }
    }

    // Plan the fewest Remove / Insert / Move operations that turn `current` into `target`.
    //   current - the list as it is now (unique keys).
    //   target  - the list it must become (unique keys).
    //   returns - the operations, in the order they must be applied. Empty when the two lists are
    //             equal. Applying every one of them in order (ApplyKeyedListOp, or the same edits on a
    //             XAML panel's Children()) turns `current` into exactly `target`.
    //   throws  - std::bad_alloc on allocation failure only.
    // Precondition: the keys of each list are unique. A repeated key is not an error the planner can
    // report - the lists it gets are built from unique session ids - so a caller that cannot promise
    // uniqueness must deduplicate first (the planner keeps a repeated target key's FIRST position).
    inline std::vector<KeyedListOp> PlanKeyedListOps(const std::vector<std::wstring>& current, const std::vector<std::wstring>& target)
    {
        std::vector<KeyedListOp> ops;

        // Every target key's position: the planner works on these small integers instead of strings.
        std::unordered_map<std::wstring, size_t> targetPos;
        targetPos.reserve(target.size());
        for (size_t i = 0; i < target.size(); ++i)
        {
            targetPos.emplace(target[i], i); // a repeated key keeps its first position (see the precondition)
        }

        // 1. Removes, highest index first: an element removed later would otherwise shift the index of
        //    every remove planned after it.
        for (size_t i = current.size(); i-- > 0;)
        {
            if (targetPos.find(current[i]) == targetPos.end())
            {
                ops.push_back(KeyedListOp{ KeyedListOpKind::Remove, static_cast<uint32_t>(i), 0, current[i] });
            }
        }

        // The survivors in their current order, each as its target position: the list after the
        // removes, as the plan's working mirror.
        std::vector<size_t> mirror;
        mirror.reserve(current.size());
        for (const auto& key : current)
        {
            if (const auto it = targetPos.find(key); it != targetPos.end())
            {
                mirror.push_back(it->second);
            }
        }

        // An empty list is filled front to back: every target key is new, and appending in order is
        // the natural first build (no anchors, no moves).
        if (mirror.empty())
        {
            for (size_t i = 0; i < target.size(); ++i)
            {
                ops.push_back(KeyedListOp{ KeyedListOpKind::Insert, 0, static_cast<uint32_t>(i), target[i] });
            }
            return ops;
        }

        // 2. The survivors that can stay: one longest run already in target order. present[t] = the
        //    target key t is in the list; keep[t] = it is in that run and never moves.
        std::vector<bool> present(target.size(), false);
        std::vector<bool> keep(target.size(), false);
        for (const size_t t : mirror)
        {
            present[t] = true;
        }
        for (const size_t index : detail::LongestIncreasingSubsequenceIndexes(mirror))
        {
            keep[mirror[index]] = true;
        }

        // 3. Back to front: every element that is new or outside the kept run goes immediately in front
        //    of its target successor. That successor was handled in the previous step, so it is already
        //    in its final place relative to everything after it; by induction the whole list ends in
        //    target order. The kept run never moves: its relative order is already right, and moving the
        //    other elements around it cannot change that order.
        for (size_t k = target.size(); k-- > 0;)
        {
            if (present[k] && keep[k])
            {
                continue;
            }
            // The successor's current index, or the end of the list for the last target key.
            const size_t anchor = (k + 1 < target.size()) ? detail::IndexOf(mirror, k + 1) : mirror.size();
            if (!present[k])
            {
                ops.push_back(KeyedListOp{ KeyedListOpKind::Insert, 0, static_cast<uint32_t>(anchor), target[k] });
                mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(anchor), k);
                continue;
            }
            const size_t from = detail::IndexOf(mirror, k);
            // Taking the element out first shifts an anchor that sits after it one place left.
            const size_t to = (from < anchor) ? anchor - 1 : anchor;
            if (from == to)
            {
                continue; // it already sits right in front of its successor - nothing to do
            }
            ops.push_back(KeyedListOp{ KeyedListOpKind::Move, static_cast<uint32_t>(from), static_cast<uint32_t>(to), target[k] });
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(from));
            mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(to), k);
        }
        return ops;
    }

    // Apply ONE planned operation to a list of keys - the same edit the side panel makes to its
    // XAML panel (RemoveAt / InsertAt / Move), so its key mirror stays the panel's exact image.
    //   list    - the list to edit, in place.
    //   op      - the operation, with indexes valid for `list` as it is now.
    //   returns - true when applied; false (and `list` unchanged) when an index is out of range, which a
    //             planned operation applied in order never is - the caller's mirror has drifted from
    //             whatever it mirrors and must be rebuilt.
    //   throws  - std::bad_alloc on allocation failure only (an Insert copies the key).
    inline bool ApplyKeyedListOp(std::vector<std::wstring>& list, const KeyedListOp& op)
    {
        switch (op.kind)
        {
        case KeyedListOpKind::Remove:
            if (op.from >= list.size())
            {
                return false;
            }
            list.erase(list.begin() + op.from);
            return true;
        case KeyedListOpKind::Insert:
            if (op.to > list.size())
            {
                return false;
            }
            list.insert(list.begin() + op.to, op.key);
            return true;
        case KeyedListOpKind::Move:
            if (op.from >= list.size() || op.to >= list.size())
            {
                return false;
            }
            {
                std::wstring moved = std::move(list[op.from]);
                list.erase(list.begin() + op.from);
                list.insert(list.begin() + op.to, std::move(moved));
            }
            return true;
        }
        return false; // an operation kind this switch doesn't know - nothing applied
    }
}
