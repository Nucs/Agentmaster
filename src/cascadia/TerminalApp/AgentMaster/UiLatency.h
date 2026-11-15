// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (UI_THREAD.md §19, LOGGING.md "[ui-latency]"): how long the UI thread keeps input WAITING, written into
// hooks.log so the stalls a key press feels show in the field. Every window of the process runs on ONE UI thread, so a
// key or a click in any window waits for whatever that thread is doing; on 2026-11-15 it stalled 50–166 ms about every
// sweep, which the [ui-stall] watchdog (20 s threshold) never logs.
//
// The engine's latency monitor (Engine.cpp, RegisterUiLatencyPing) SENDS a no-op message to each UI thread every
// kUiLatencyCheckGapMs and times the round trip: Windows hands a thread its sent messages before its input, so the round
// trip is about the least a key press waited at that moment. This header is the PURE half — the per-minute tally of
// those round trips and the lines it writes — so the m5 harness can drive it with any clock (TestUiLatencyTally).
//
// What a line looks like (hooks.log adds its own [HH:MM:SS.mmm] stamp in front):
//   [ui-latency] UI thread 157384, the last 60 s: input waited 100 ms or more 5 times (250 ms or more: 1, 1 s or more:
//     0), 0.7 s in all, the longest 166 ms; each wait in ms @ seconds before this line: 104@52.1 166@44.0 121@36.0
//     110@28.1 149@20.0; 1183 checks
//   [ui-latency] UI thread 157384 kept input waiting 1842 ms (answered just now)
// A minute with no wait of kUiLatencyStallMs or more writes nothing, so a quiet instance adds no line.
//
// Pure: the STL only (no Win32, no WinRT). The members that build a line can throw std::bad_alloc; the others are
// noexcept.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar> // std::swprintf (the seconds' one decimal)
#include <string>
#include <utility>
#include <vector>

namespace Agentmaster
{
    // The pause between two checks of one UI thread while it answers promptly. A stall of S ms is measured by the check
    // in flight when it began, as a wait between S - kUiLatencyCheckGapMs and S: every stall of 150 ms or more reads
    // as one of kUiLatencyStallMs or more. 20 sends a second cost the UI thread microseconds each.
    inline constexpr uint32_t kUiLatencyCheckGapMs = 50;

    // How long one check waits for its answer. A stall longer than this reads as a TIMED-OUT check of this length (and
    // the next check starts at once); past 20 s the [ui-stall] watchdog logs and escalates on its own clock.
    inline constexpr uint32_t kUiLatencyCheckTimeoutMs = 10000;

    // A wait this long or longer is a stall the user feels: it is counted, listed and summed in the minute's line.
    inline constexpr uint32_t kUiLatencyStallMs = 100;

    // The second count of the summary line ("250 ms or more"): a clearly visible freeze.
    inline constexpr uint32_t kUiLatencyBigStallMs = 250;

    // A wait this long or longer also gets a line of its own at once (capped per minute, kUiLatencyLongLinesPerWindow).
    inline constexpr uint32_t kUiLatencyLongStallMs = 1000;

    // The length of one tally window; a window that held a stall writes one summary line when the first check after it
    // ends. A window can run longer than this when checks pause (a timed-out check, the machine asleep).
    inline constexpr int64_t kUiLatencyWindowMs = 60000;

    // How many of a window's stalls its summary lists with their place in time ("+N more" counts the rest), so a periodic
    // stall reads as a period (the 2026-11-15 rundown's "worst every ~8 s") and the line stays short.
    inline constexpr size_t kUiLatencyListedStalls = 12;

    // How many long-stall lines (kUiLatencyLongStallMs or more) one window writes at most: a UI thread frozen for minutes
    // times out a check every 10 s, and the summary still counts every one of them.
    inline constexpr int kUiLatencyLongLinesPerWindow = 10;

    // What one check of a UI thread came back with.
    enum class UiPingResult : uint8_t
    {
        Answered, // the thread answered within the timeout: the round trip is the wait
        TimedOut, // no answer within the timeout: the thread was stalled at least that long
        Failed, // no measurement (the window was torn down between the copy and the send): ignored
    };

    // The per-minute tally of one UI thread's round trips and the lines it writes. One instance per UI thread, fed by the
    // monitor thread alone (not thread-safe by itself).
    class UiLatencyTally
    {
    public:
        // Parameters:
        //   label - what the lines call this thread, e.g. L"UI thread 157384". Copied.
        // Exceptions: std::bad_alloc when the label cannot be copied.
        explicit UiLatencyTally(std::wstring label) :
            _label{ std::move(label) }
        {
        }

        // Records one check and returns the lines to log for it, oldest first: the summary of the window this check
        // closes (when that window held a stall), then this check's own long-stall line (when it waited
        // kUiLatencyLongStallMs or more and the window's cap allows).
        // Parameters:
        //   waitedMs   - the round trip in ms; for a timed-out check, the timeout (kUiLatencyCheckTimeoutMs).
        //   endedAtMs  - a MONOTONIC clock in ms (the monitor uses QueryUnbiasedInterruptTime, which skips sleep and
        //                hibernation) when the check ended. A clock that goes backwards closes the window like a full one.
        //   timedOut   - the check got no answer; its wait is written as "<timeout>+" (at least that long).
        // Returns: 0, 1 or 2 complete lines, each ending in "\n".
        // Exceptions: std::bad_alloc while building a line or listing a stall; the tally's counts stay consistent (a
        //   failed list push loses only that one listing).
        std::vector<std::wstring> Note(uint32_t waitedMs, int64_t endedAtMs, bool timedOut)
        {
            std::vector<std::wstring> lines;
            if (_windowStartMs < 0)
            {
                _windowStartMs = endedAtMs; // the first check opens the first window
            }
            else if (endedAtMs - _windowStartMs >= kUiLatencyWindowMs || endedAtMs < _windowStartMs)
            {
                if (_stalls > 0)
                {
                    lines.push_back(_Summary(endedAtMs));
                }
                _Reset(endedAtMs); // this check belongs to the new window
            }
            ++_checks;
            if (waitedMs < kUiLatencyStallMs)
            {
                return lines;
            }
            ++_stalls;
            _stalledMs += waitedMs;
            if (waitedMs >= kUiLatencyBigStallMs)
            {
                ++_bigStalls;
            }
            if (waitedMs >= kUiLatencyLongStallMs)
            {
                ++_longStalls;
            }
            if (waitedMs > _longestMs)
            {
                _longestMs = waitedMs;
                _longestTimedOut = timedOut;
            }
            if (_listed.size() < kUiLatencyListedStalls)
            {
                _listed.push_back(Listed{ waitedMs, endedAtMs, timedOut });
            }
            if (waitedMs >= kUiLatencyLongStallMs && _longLines < kUiLatencyLongLinesPerWindow)
            {
                ++_longLines;
                lines.push_back(timedOut ? L"[ui-latency] " + _label + L" did not answer for " + std::to_wstring(waitedMs) +
                                               L" ms or more; past 20 s the [ui-stall] watchdog takes over\n" :
                                           L"[ui-latency] " + _label + L" kept input waiting " + std::to_wstring(waitedMs) + L" ms (answered just now)\n");
            }
            return lines;
        }

        // The checks of the current window so far (the harness reads it; the monitor does not need it).
        // Returns: the count. Never throws.
        uint32_t Checks() const noexcept
        {
            return _checks;
        }

        // The stalls (waits of kUiLatencyStallMs or more) of the current window so far.
        // Returns: the count. Never throws.
        uint32_t Stalls() const noexcept
        {
            return _stalls;
        }

    private:
        // One stall a summary lists: its wait, when its check ended, and whether that check timed out.
        struct Listed
        {
            uint32_t waitedMs;
            int64_t endedAtMs;
            bool timedOut;
        };

        // A wait for a line: "166", or "10000+" for a check that timed out (at least that long).
        // Parameters: waitedMs - the wait; timedOut - the check got no answer.
        // Returns: the text. Exceptions: std::bad_alloc.
        static std::wstring _WaitText(uint32_t waitedMs, bool timedOut)
        {
            return std::to_wstring(waitedMs) + (timedOut ? L"+" : L"");
        }

        // Milliseconds as seconds with one decimal ("52.1"); negative spans (a clock that went back) read as 0.0.
        // Parameters: ms - the span. Returns: the text. Exceptions: std::bad_alloc.
        static std::wstring _Seconds(int64_t ms)
        {
            wchar_t buf[32];
            const double seconds = ms > 0 ? static_cast<double>(ms) / 1000.0 : 0.0;
            std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f", seconds);
            return buf;
        }

        // The window's summary line, written at the check that closes it.
        // Parameters: nowMs - when that closing check ended (the "seconds before this line" are counted back from it).
        // Returns: the line, ending in "\n". Exceptions: std::bad_alloc.
        std::wstring _Summary(int64_t nowMs) const
        {
            // The window's span in whole seconds, rounded; a clock that went backwards (the closing check before the
            // window's start) reads as 0, like the offsets below.
            const int64_t spanMs = nowMs > _windowStartMs ? nowMs - _windowStartMs : 0;
            std::wstring line = L"[ui-latency] " + _label + L", the last " + std::to_wstring((spanMs + 500) / 1000) +
                                L" s: input waited " + std::to_wstring(kUiLatencyStallMs) + L" ms or more " + std::to_wstring(_stalls) +
                                (_stalls == 1 ? L" time" : L" times") + L" (" + std::to_wstring(kUiLatencyBigStallMs) + L" ms or more: " +
                                std::to_wstring(_bigStalls) + L", 1 s or more: " + std::to_wstring(_longStalls) + L"), " +
                                _Seconds(static_cast<int64_t>(_stalledMs)) + L" s in all, the longest " + _WaitText(_longestMs, _longestTimedOut) +
                                L" ms; each wait in ms @ seconds before this line:";
            for (const auto& s : _listed)
            {
                line += L" " + _WaitText(s.waitedMs, s.timedOut) + L"@" + _Seconds(nowMs - s.endedAtMs);
            }
            if (_stalls > _listed.size())
            {
                line += L" +" + std::to_wstring(_stalls - _listed.size()) + L" more";
            }
            line += L"; " + std::to_wstring(_checks) + L" checks\n";
            return line;
        }

        // Opens a new, empty window.
        // Parameters: startMs - when it opens (the check that closed the previous one). Never throws.
        void _Reset(int64_t startMs) noexcept
        {
            _windowStartMs = startMs;
            _checks = 0;
            _stalls = 0;
            _bigStalls = 0;
            _longStalls = 0;
            _stalledMs = 0;
            _longestMs = 0;
            _longestTimedOut = false;
            _longLines = 0;
            _listed.clear();
        }

        std::wstring _label; // the thread's name in every line
        int64_t _windowStartMs = -1; // when the current window opened; -1 until the first check
        uint32_t _checks = 0; // checks in the current window (every measured one, stalls included)
        uint32_t _stalls = 0; // checks that waited kUiLatencyStallMs or more
        uint32_t _bigStalls = 0; // ... kUiLatencyBigStallMs or more
        uint32_t _longStalls = 0; // ... kUiLatencyLongStallMs or more
        uint64_t _stalledMs = 0; // the stalls' waits summed
        uint32_t _longestMs = 0; // the longest wait of the window
        bool _longestTimedOut = false; // the longest wait was a timed-out check (written "<n>+")
        int _longLines = 0; // long-stall lines this window already wrote (kUiLatencyLongLinesPerWindow caps them)
        std::vector<Listed> _listed; // the first kUiLatencyListedStalls stalls, in order
    };
}
