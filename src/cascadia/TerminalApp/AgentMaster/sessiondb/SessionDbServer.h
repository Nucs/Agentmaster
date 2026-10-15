// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database SERVICE (SESSIONS_DB.md §4, §9, §10, §11): the process that owns
// <profile>\sessiondb\sessions.duckdb and answers every client on the per-profile named pipe.
//
// Shape:
//   - ONE service per profile: the named mutex Local\Agentmaster.SessionDb.<hash>; a second service that
//     cannot take it exits at once (exit code 0, logged).
//   - The pipe \\.\pipe\agentmaster.sessiondb.<hash> (PIPE_REJECT_REMOTE_CLIENTS, the default DACL: the
//     user, SYSTEM, administrators), one reader thread per connected client, framed requests
//     (SessionDbProtocol.h). A client may pipeline requests: each runs on a worker and is answered by id.
//   - Reads run on a pool of query connections (4); everything that writes runs on ONE writer thread (the
//     Store's threading contract), which also merges, re-clusters and folds history.jsonl in idle time at
//     background priority.
//   - Every query request has a deadline (2.5 s for `sql`, 10 s otherwise); a watchdog interrupts the query
//     past it (duckdb_interrupt) and the client gets 408. A `cancel` from the client interrupts the same
//     way and answers 499.
//   - At start the writer sweeps every transcript against its cursor (the cold build on an empty database;
//     D16's open question answered as recommended: a service started without the app catches up at once).
//   - It exits when no client has been connected for 10 minutes and no write work is pending (§4.2), on a
//     `shutdown` request, or when the process is asked to stop. service.json names the running service and
//     is deleted at exit.

#pragma once

#include <string>

#include "SessionDbStore.h"

namespace Agentmaster::SessionDb
{
    // The store configuration for a profile: the projects folder and history.jsonl of the Claude config the
    // app resolves, DuckDB's limits from the Settings cog (§10), the inferred-folder setting, the service log.
    StoreConfig ServiceConfigFor(const std::wstring& profileDir);

    // Run the service until it stops (see above).
    //   profileDir — the profile it serves.
    //   sweepLimit — 0 in production; > 0 caps every sweep to that many newest changed sessions (the
    //                diagnostic `serve --limit N`, so a test profile does not ingest the whole corpus).
    // Returns the process exit code: 0 (stopped normally, or another service owns the profile), 3 (the
    // database belongs to a newer service), 1 (a failure, logged).
    int RunServe(const std::wstring& profileDir, size_t sweepLimit = 0);
}
