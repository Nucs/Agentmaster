// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session summary the DATABASE forms (SESSIONS_DB.md §8.5, decided D14).
//
// The app's summary surfaces keep AnalyzeSessionTranscript (the user's decision of 2026-10-14). The
// database forms the same summary for ITS consumers — the AI search's tools, the MCP `session` tool,
// the CLI — as one precomputed `summary` row per session, rebuilt by ONE set-based SQL statement over
// the section rows the ingest extracted with the app's own rules (SessionDbExtract.cpp) and the
// live-branch flags the C++ walk set (SessionDbWalk.h).
//
// The cross-line rules are the ones the prototype verified equal to the analyzer on 921 of 921
// sessions (tools/sessiondb-bench/summary_from_db.py): the current segment's prompts come from the live
// branch, deduplicated among themselves; an earlier /compact segment lists every prompt it holds,
// deduplicated within the segment; a Write is "created" when its result says so, an Edit (or an
// overwriting Write) before a label's first creation keeps it "edited", Read keeps only labels neither
// created nor edited; tasks come from the last TodoWrite; the plan facts and the parent link from the
// first external user line on the branch. Two implementations of one rule set is what makes parity a
// TEST: `agentmaster-sessiondb verify-summary` compares this row with AnalyzeSessionTranscript.

#pragma once

#include <string>

#include "../ProcessInspect.h" // SessionSummary

namespace Agentmaster::SessionDb
{
    class Connection;

    // The version of the summary's RULES (SummaryBuildSql). A summary row is only rebuilt when its session
    // changes, so a database whose meta `summary_version` differs holds rows formed by older rules: the store
    // rebuilds every row once (Store::RebuildSummaries, at the end of a complete sweep) and records this value.
    // ⚠ Bump it with every change to SummaryBuildSql that can change a row.
    //   1 — the first rules (c0253b8b0).
    //   2 — the current list starts after the last ON-BRANCH boundary, and an entry carries `cur` (2026-10-15, 41fe5e493).
    inline constexpr int kSummaryVersion = 2;

    // The statement that (re)builds the summary rows of a set of sessions: INSERT OR REPLACE INTO summary.
    // Parameters: $1 the top-level file ids (BIGINT list), $2 their session keys (same order), $3 now (ms).
    const std::string& SummaryBuildSql();

    // Read one session's summary row back as the analyzer's SessionSummary (what RenderSessionSummaryBox
    // takes), so the CLI and the tools render exactly the box the app shows.
    //   con    — a reader connection.
    //   sidKey — the session.
    //   out    — the summary (found == false when the session has no summary row yet).
    // Throws DbError.
    void LoadSummary(Connection& con, int32_t sidKey, SessionSummary& out);

    // An ISO-8601 UTC timestamp ("2026-10-14T09:05:12.345Z") for a Unix-ms time, the shape the analyzer's
    // firstTs / lastTs carry. "" for 0.
    std::wstring IsoFromMs(int64_t ms);
}
