// Agentmaster (UI_THREAD.md §12): how often the Agent Manager page may rebuild for registry changes.
//
// The page rebuilds its board, tree and Prompt Queue when the session registry changes. The registry path
// (AgentManagerContent::_RefreshFromRegistryEvent) runs one rebuild at once and folds every later change
// into ONE trailing rebuild after a gap. Until 2026-10-14 the gap was a fixed 250 ms, measured from the end
// of the previous rebuild. A rebuild that costs seconds then left the UI thread no room: on the frozen
// release of 2026-10-14 a rebuild took ~4 s and the registry changed every second (a re-bind loop,
// OBSERVER.md §20), so the thread was ~94% busy with rebuilds and a click waited for minutes.
//
// The gap now grows with the cost of the last rebuild: twice the cost, never under the old 250 ms and never
// over 5 s. A cheap rebuild (the normal case: a few ms to ~100 ms) keeps the old 250 ms; a slow one leaves
// the thread at least two thirds free for input and rendering, and the page still shows the final state —
// a trailing rebuild always runs. User actions (a click, a scope or sort change) still rebuild at once.
//
// Pure, header-only, no Windows and no WinRT, so the standalone test harness (TestRefreshPacing in
// tests_state.cpp) covers it directly.
#pragma once

#include <cstdint>

namespace Agentmaster
{
    // The shortest gap between two registry-driven rebuilds (the fixed gap before 2026-10-14).
    inline constexpr int64_t kManagerRefreshMinGapMs = 250;
    // The longest gap: a rebuild that takes seconds still shows a registry change within ~5 s plus its cost.
    inline constexpr int64_t kManagerRefreshMaxGapMs = 5000;
    // The gap is this many times the cost of the last rebuild (between the two bounds above).
    inline constexpr int64_t kManagerRefreshCostFactor = 2;
    // A rebuild that takes at least this long is logged (hooks.log `[manager] slow rebuild`).
    inline constexpr int64_t kManagerSlowRebuildMs = 1000;
    // The shortest time between two slow-rebuild log lines, so a page that stays slow logs once a minute,
    // not once per rebuild.
    inline constexpr int64_t kManagerSlowRebuildLogEveryMs = 60000;

    // The gap to wait, after a registry-driven rebuild, before the next one may run.
    //   lastRebuildCostMs — how long the last rebuild took, in ms (0 or less when unknown: no rebuild yet).
    //   returns — kManagerRefreshCostFactor × the cost, clamped to [kManagerRefreshMinGapMs,
    //             kManagerRefreshMaxGapMs]. Never throws; a huge cost cannot overflow (it saturates).
    inline constexpr int64_t ManagerRefreshGapMs(const int64_t lastRebuildCostMs) noexcept
    {
        if (lastRebuildCostMs <= 0)
        {
            return kManagerRefreshMinGapMs;
        }
        // Saturate before the multiply: a cost past max / factor is past the cap anyway.
        if (lastRebuildCostMs >= kManagerRefreshMaxGapMs / kManagerRefreshCostFactor)
        {
            return kManagerRefreshMaxGapMs;
        }
        const int64_t gap = lastRebuildCostMs * kManagerRefreshCostFactor;
        return gap < kManagerRefreshMinGapMs ? kManagerRefreshMinGapMs : gap;
    }

    // Should a rebuild that took `costMs` be logged now?
    //   costMs        — how long the rebuild took, in ms.
    //   nowMs         — the current monotonic time, in ms (GetTickCount64).
    //   lastLoggedMs  — when the last slow rebuild was logged, in the same clock; 0 when never.
    //   returns — true when the rebuild is slow (≥ kManagerSlowRebuildMs) and no slow rebuild was logged in
    //             the last kManagerSlowRebuildLogEveryMs. Never throws.
    inline constexpr bool ShouldLogSlowManagerRebuild(const int64_t costMs, const int64_t nowMs, const int64_t lastLoggedMs) noexcept
    {
        if (costMs < kManagerSlowRebuildMs)
        {
            return false;
        }
        return lastLoggedMs == 0 || nowMs - lastLoggedMs >= kManagerSlowRebuildLogEveryMs;
    }
}
