// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the SUMMARY PARITY CHECK (SESSIONS_DB.md §8.5, D14: "two implementations of the rules,
// so parity is a test"). The app shows AnalyzeSessionTranscript's summary; the database's tools show the
// summary row. This compares the two, field by field, over sessions whose transcript did not change since
// it was ingested, and reports how many agree — the C++ successor of the prototype's
// `summary_from_db.py --verify` (921 of 921 sessions agreed there).

#pragma once

#include <cstdint>
#include <string>

namespace Agentmaster::SessionDb
{
    class Store;

    // The outcome of one parity run.
    struct VerifyReport
    {
        int64_t compared{ 0 }; // sessions compared
        int64_t skipped{ 0 }; // sessions whose transcript grew after the ingest (not comparable)
        int64_t allAgree{ 0 }; // sessions equal on every field
        std::wstring text; // the per-field report (one line per field: agreed / compared + up to 3 examples)
    };

    // Compare the database's summary with AnalyzeSessionTranscript for up to `limit` sessions (newest first;
    // 0 = every session). Opens its own reader connection for the comparison.
    //   rebuild — first re-form the compared sessions' summary rows with THIS build's rules
    //             (Store::RebuildSummaries, on the store's writer connection): the check then tests the
    //             running code, not rows an older build wrote. The one-shot `verify-summary` passes true.
    // Throws DbError.
    VerifyReport VerifySummaries(Store& store, size_t limit, bool rebuild = false);
}
