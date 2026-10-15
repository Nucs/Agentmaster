// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database service's own log (<profile>\sessiondb\sessiondb.log, the
// `[sessdb]` lines of SESSIONS_DB.md §4.3). The service is a background process with no console when
// the GUI starts it, so this file is where its story is told: opens, resyncs, ticks, merges, refusals.
// Engine-level swallowed exceptions still go to the profile's hooks.log through the shared
// LogSwallowedException chokepoint (Rule #18).

#pragma once

#include <string>

namespace Agentmaster::SessionDb
{
    // Point the log at a file (UTF-8, appended). Call once at startup. `echo` also writes each line to
    // stderr (the one-shot command-line modes, which a person runs in a terminal).
    void InitServiceLog(const std::wstring& path, bool echo);

    // Append one line, prefixed with a local "[HH:MM:SS.mmm]" stamp. Thread-safe; never throws (a log that
    // cannot be written is dropped: there is nowhere left to report it).
    void ServiceLog(const std::wstring& line) noexcept;
}
