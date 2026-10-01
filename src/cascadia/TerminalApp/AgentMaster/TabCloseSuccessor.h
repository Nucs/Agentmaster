// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — which tab takes focus after the FOCUSED tab is closed, without WAKING a dormant one
// (TerminalPage::_PickCloseSuccessorTab, called from TabManagement.cpp's _RemoveTab).
//
// Why this exists: a window-restored Agentmaster tab is DORMANT — its claude.exe (or shell) spawns
// lazily on the control's first layout, i.e. the first time the tab is SHOWN — and a Deactivated tab is
// in that very state (Deactivate gives it a never-initialized control). Upstream _RemoveTab hands focus to the tab that slid into
// the closed slot (in-order) or to the MRU head; in a strip of hundreds of mostly-dormant tabs that
// successor is usually asleep, so closing ONE session silently STARTED another: one claude.exe exited
// and a new one spawned ~0.4 s later, and to a user watching Task Manager the closed tab's claude.exe
// "was never closed" (FAVORITES.md §4a — the 2026-09-25 report, measured off hooks.log: the closed
// tab's claude.exe was gone 1.3 s after the close while the right neighbor's claude.exe started).
// Closing is not an intent to open anything, so the successor must be a tab whose selection starts
// nothing.
//
// Pure + header-only, no WinRT (the TabDragMath.h / PromptAnchor.h idiom) so the engine test harness
// covers the decision with no XAML; the caller builds the per-tab facts from the live strip.
#pragma once

#include <algorithm>
#include <vector>

namespace Agentmaster
{
    // Picks the index of the tab to select after the FOCUSED tab at `closedSlot` was removed from the
    // strip. Upstream's own choice (`preferred`) is kept verbatim whenever selecting it starts nothing,
    // so a close whose natural successor is already running behaves exactly as before; only a DORMANT
    // successor is replaced. The replacement honors the user's configured tab-switcher mode:
    //   * MRU mode      — the most-recently-used tab that starts nothing (typically the tab you came
    //                     from), then the positional scan as a belt.
    //   * in-order mode — the nearest tab that starts nothing, scanning outward from the closed slot:
    //                     the tab that slid INTO it (the old right neighbor) first, then the old left
    //                     neighbor, then alternating further out (right-first on ties — upstream's own
    //                     in-order preference).
    // With EVERY tab dormant the function falls back to `preferred` — upstream behavior, never a dead end
    // with no selection. The CALLER owns the no-wake guarantee for that case: TerminalPage::_RemoveTab
    // sees the pick would still start a process and shows the Agent Manager page instead (the harbor the
    // pinned, never-starting Manager tab used to be before the Manager became a page).
    //
    // Parameters:
    //   closedSlot — the removed tab's FORMER index. In the post-removal strip it is where the old right
    //                neighbor now sits (== the strip size when the LAST tab was closed). Any value is
    //                tolerated: it is clamped into [0, strip size] before the scan.
    //   preferred  — upstream's pick as a post-removal index (clamp(closedSlot) in in-order mode, the MRU
    //                head's index in MRU mode). Out of range == "no usable preference".
    //   wouldStart — per tab, post-removal strip order: true when SELECTING that tab would start a process
    //                that is not running yet (a never-shown restored tab, a Deactivated tab). Its size is
    //                the strip size.
    //   mruOrder   — post-removal strip indices, most-recently-used first (the closed tab already gone).
    //                Out-of-range or duplicate entries are skipped; only consulted in MRU mode.
    //   mruMode    — the user's tabSwitcherMode is MostRecentlyUsed.
    // Returns: the index to select, in [0, wouldStart.size()); -1 only when the strip is empty.
    // Never throws (no allocation, bounds-checked indexing).
    inline int PickCloseSuccessorIndex(int closedSlot, int preferred, const std::vector<bool>& wouldStart, const std::vector<int>& mruOrder, bool mruMode) noexcept
    {
        const int count = static_cast<int>(wouldStart.size());
        if (count <= 0)
        {
            return -1; // nothing left to select (the caller's window is closing anyway)
        }
        // Pin the anchor into [0, count] (count == "the LAST tab was closed"), so the outward scan below
        // always sweeps the whole strip — a hostile or stale slot must never make it miss a safe tab.
        closedSlot = std::clamp(closedSlot, 0, count);
        const auto startsNothing = [&](const int i) noexcept {
            return i >= 0 && i < count && !wouldStart[static_cast<size_t>(i)];
        };

        // Upstream's choice wins whenever it is safe: unchanged behavior for every close whose
        // successor is already running (or is not a terminal at all).
        if (startsNothing(preferred))
        {
            return preferred;
        }

        if (mruMode)
        {
            for (const int i : mruOrder)
            {
                if (startsNothing(i))
                {
                    return i;
                }
            }
            // Every MRU entry is dormant or the list is incomplete: fall through to the positional scan
            // (a belt — the MRU list normally holds every tab).
        }

        // Positional, outward from the closed slot: step 0 -> the slot itself (old right neighbor),
        // 1 -> slot-1 (old left neighbor), 2 -> slot+1, 3 -> slot-2, ... With the slot pinned into
        // [0, count] the farthest tab is at most `count` away, so 2*count+1 steps cover the strip.
        for (int step = 0; step <= 2 * count; ++step)
        {
            const int offset = (step % 2 == 0) ? (step / 2) : -((step + 1) / 2);
            const int i = closedSlot + offset;
            if (startsNothing(i))
            {
                return i;
            }
        }

        // Every tab would start something: keep upstream's pick rather than leave the strip with no
        // selection (the Agentmaster caller then shows the Agent Manager page instead of waking it).
        if (preferred >= 0 && preferred < count)
        {
            return preferred;
        }
        return std::clamp(closedSlot, 0, count - 1);
    }
}
