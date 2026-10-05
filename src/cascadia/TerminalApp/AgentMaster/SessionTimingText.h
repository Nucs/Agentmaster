// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - the per-session TIMING line that every session card and row shows: the side-panel cards, the Triage
// Board's cards (managed and External) and the Explorer tree's rows. It reads "created 4d1h, active 2m4s ago".
//
// Why it changed (2026-10-05): the line used to be "-7h5m/7h6m/2m4s" - created-ago / active-span / last-activity-ago.
// Three bare spans, told apart only by their position and a minus sign, with 'm' meaning MONTHS before a d or an h and
// MINUTES after one. The user found it unreadable. The line now NAMES the two facts a card needs and drops the middle
// span (it was only the difference of the other two):
//
//   created <span>     how long ago the session was created: years, days, hours (y / d / h), never minutes or
//                      seconds. Under an hour it is a word instead: "created recently"; under 5 minutes "created now".
//   active <span> ago  how long ago it last did something: y / d / h / m (minutes), plus s (seconds) only while the
//                      span is under an hour. Left out when it is 0: no recorded activity, or activity this very
//                      second.
//
// There is no month unit anywhere, so 'm' is always minutes and no span can be read two ways. A year is 365 days.
//
// Pure + header-only, no WinRT (the TabCloseSuccessor.h idiom): the clock is a parameter, so the engine test harness
// pins every rule (TestSessionTimingText in tests/tests_sidepanel.cpp) with no XAML. The UI's FormatSessionTiming
// (AgentManagerContent.Internal.h) passes the wall clock. Pure-ASCII source.
#pragma once

#include <cstdint>
#include <string>

namespace Agentmaster
{
    // Span units in milliseconds. A year is a calendar-free 365 days; there is deliberately no month (see the header):
    // a month unit needs a letter, and 'm' already means minutes.
    inline constexpr int64_t kTimingMsPerSecond = 1000;
    inline constexpr int64_t kTimingMsPerMinute = 60 * kTimingMsPerSecond;
    inline constexpr int64_t kTimingMsPerHour = 60 * kTimingMsPerMinute;
    inline constexpr int64_t kTimingMsPerDay = 24 * kTimingMsPerHour;
    inline constexpr int64_t kTimingMsPerYear = 365 * kTimingMsPerDay;

    // The two word thresholds of the creation span: a session younger than kCreatedNowMs was "created now", one younger
    // than kCreatedRecentlyMs "created recently" (the user's rules: under 5 minutes = now, under 1 hour = recently).
    // From one hour on the age is a number. Change them only together with the tooltip text that names them
    // (kTimingTooltip, AgentManagerContent.Internal.h).
    inline constexpr int64_t kCreatedNowMs = 5 * kTimingMsPerMinute;
    inline constexpr int64_t kCreatedRecentlyMs = kTimingMsPerHour;

    namespace timing_text_detail
    {
        // Appends "<value><unit>" to `out` when `value` is not 0, so a zero unit is left out ("1y5h", never "1y0d5h").
        //   out   - the span being built.
        //   value - the unit's count (0 or more).
        //   unit  - its letter: "y", "d", "h", "m" or "s".
        // Returns nothing. Throws std::bad_alloc only (the append).
        inline void AppendUnit(std::wstring& out, int64_t value, const wchar_t* unit)
        {
            if (value != 0)
            {
                out += std::to_wstring(value);
                out += unit;
            }
        }
    }

    // The word or span that follows "created": "now" under 5 minutes, "recently" under an hour, else the age in years /
    // days / hours with every zero unit left out ("1h", "4d1h", "45d3h", "1y5h", "1y4d1h"). Minutes and seconds are cut
    // off, never rounded up: a session 1 h 59 min old reads "1h", so the span never claims an hour that has not passed.
    //   ageMs   - how long ago the session was created, in ms. A negative age (a creation stamp in the future: clock skew
    //             between the transcript's file system and this machine) counts as 0, so it reads "now".
    //   returns - "now", "recently", or a span of one to three units; never empty.
    // Throws std::bad_alloc only.
    inline std::wstring FormatCreatedSpan(int64_t ageMs)
    {
        if (ageMs < kCreatedNowMs) // every negative age lands here too
        {
            return L"now";
        }
        if (ageMs < kCreatedRecentlyMs)
        {
            return L"recently";
        }
        const int64_t years = ageMs / kTimingMsPerYear;
        int64_t rest = ageMs % kTimingMsPerYear;
        const int64_t days = rest / kTimingMsPerDay;
        rest %= kTimingMsPerDay;
        const int64_t hours = rest / kTimingMsPerHour;
        std::wstring out;
        timing_text_detail::AppendUnit(out, years, L"y");
        timing_text_detail::AppendUnit(out, days, L"d");
        timing_text_detail::AppendUnit(out, hours, L"h");
        // Not empty: the age is at least one hour here, so with no year and no day the hour count is at least 1.
        return out;
    }

    // The span inside "active <span> ago": years / days / hours / minutes, plus seconds only while the whole span is
    // under an hour (the old line's rule: seconds when there are no hours, minutes as the smallest unit above that),
    // with every zero unit left out ("45s", "1m", "2m4s", "59m59s", "1h", "2h30m", "3d2h15m", "1y35d"). Units below the
    // smallest one shown are cut off, never rounded up.
    //   agoMs   - how long ago the session last did something, in ms. A negative span (an activity stamp in the future)
    //             counts as 0.
    //   returns - the span, or "" when it is 0 at its precision (under one second): the "0" the line leaves out.
    // Throws std::bad_alloc only.
    inline std::wstring FormatActiveAgoSpan(int64_t agoMs)
    {
        if (agoMs < kTimingMsPerSecond) // every negative span lands here too
        {
            return {};
        }
        const bool withSeconds = agoMs < kTimingMsPerHour;
        const int64_t years = agoMs / kTimingMsPerYear;
        int64_t rest = agoMs % kTimingMsPerYear;
        const int64_t days = rest / kTimingMsPerDay;
        rest %= kTimingMsPerDay;
        const int64_t hours = rest / kTimingMsPerHour;
        rest %= kTimingMsPerHour;
        const int64_t minutes = rest / kTimingMsPerMinute;
        rest %= kTimingMsPerMinute;
        const int64_t seconds = rest / kTimingMsPerSecond;
        std::wstring out;
        timing_text_detail::AppendUnit(out, years, L"y");
        timing_text_detail::AppendUnit(out, days, L"d");
        timing_text_detail::AppendUnit(out, hours, L"h");
        timing_text_detail::AppendUnit(out, minutes, L"m");
        if (withSeconds)
        {
            timing_text_detail::AppendUnit(out, seconds, L"s");
        }
        // Not empty: the span is at least one second; under an hour the minutes or the seconds are non-zero, from an
        // hour on the hours (or a larger unit) are.
        return out;
    }

    // The whole timing line: "created <span>" and, when there is activity to report, ", active <span> ago". Examples:
    // "created 4d1h, active 2m4s ago", "created 1y4d1h", "created now, active 12s ago", "created recently".
    //   createdMs      - when the session was created (unix ms): a managed session's transcript birth
    //                    (SessionInfo::convCreatedUnixMs), an External row's transcript birth or its process start.
    //                    0 or negative = unknown.
    //   lastActivityMs - when it last did something (unix ms). 0 or negative = unknown, and the active part is left
    //                    out. A value BEFORE createdMs is raised to createdMs: the creation is itself activity (the
    //                    first message wrote the transcript), and an older stamp is clock skew or a launch-time hook
    //                    stamp from before that first message. The old line made the same clamp.
    //   nowMs          - the clock (unix ms). The UI passes the wall clock, the tests a fixed instant.
    //   returns        - the line, or "" when createdMs is unknown (a never-prompted session: nothing meaningful to
    //                    show, and every caller then shows no timing at all, as before).
    // Throws std::bad_alloc only.
    inline std::wstring FormatSessionTimingLine(int64_t createdMs, int64_t lastActivityMs, int64_t nowMs)
    {
        if (createdMs <= 0)
        {
            return {};
        }
        std::wstring line = L"created ";
        line += FormatCreatedSpan(nowMs - createdMs);
        if (lastActivityMs > 0)
        {
            const int64_t last = lastActivityMs < createdMs ? createdMs : lastActivityMs;
            const std::wstring ago = FormatActiveAgoSpan(nowMs - last);
            if (!ago.empty())
            {
                line += L", active ";
                line += ago;
                line += L" ago";
            }
        }
        return line;
    }
}
