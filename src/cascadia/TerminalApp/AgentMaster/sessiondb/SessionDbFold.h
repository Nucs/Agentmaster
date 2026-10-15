// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the SESSION FOLD (SESSIONS_DB.md §5.6, §7.6.3): the per-transcript accumulator the
// session row is written from.
//
// Why a fold: a refresh tick reads only the NEW lines of a session (from its cursor), yet the session
// row holds whole-history facts (the first cwd, the distinct models, the last cost record, counts).
// The fold is the resumable state that turns "the row so far + these new lines" into "the row now":
// it is persisted as JSON in src_file.fold in the SAME transaction as the cursor, so a tick that fails
// rolls both back and a replay can neither lose nor double-count a line.
//
// Its rules follow the sidecar's TranscriptStats (AccumulateTranscriptStats) where the two overlap —
// first cwd / branch, the typed-prompt and assistant-line counts, the Ctx column, the titles, the fork
// id — so a session reads the same in the database as on today's Sessions page, and §5.6's rules for
// the rest (versions, models, effort, Claude Code's cost-state, outcomes, Stop-hook and turn times,
// work done, artifacts).
//
// What it does NOT hold: per-file structure the summary and the dedupe need (the uuids seen, the
// segment count — the ingest keeps those), and usage sums (aggregated from the usage table, which is
// deduplicated per API message).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "SessionDbExtract.h"

namespace Agentmaster::SessionDb
{
    // The accumulators of one top-level transcript. Default = an empty transcript.
    struct SessionFold
    {
        int64_t lines{ 0 }; // lines folded (every line, JSON or not)

        // places
        std::wstring cwdFirst, cwdLast;
        std::vector<std::wstring> cwdKeys; // distinct NormDirKey(cwd), first-seen order
        std::wstring relocatedCwd;
        std::wstring worktreePath, worktreeName, worktreeBranch, worktreeOriginalCwd;
        bool env{ false }, envIsGitRepo{ false }, envIsWorktree{ false };
        std::wstring envShell;
        int32_t envExtraDirs{ 0 };

        // identity, versions, models
        std::wstring branchFirst, branchLast;
        std::vector<std::wstring> branches;
        std::wstring versionFirst, versionLast;
        std::vector<std::wstring> models; // distinct, first-seen order
        std::wstring modelLast, effortLast, entrypoint, permissionModeLast, agentName;
        int32_t planModeMarks{ 0 };

        // times
        int64_t firstTsMs{ 0 }; // the first timestamped line (a fork's is copied: created_ms prefers the file's birth)
        int64_t lastActivityMs{ 0 }; // the newest non-sidechain user / assistant timestamp (max, as TranscriptStats)
        int64_t lastTypedMs{ 0 }; // the newest real typed prompt (the MOST RECENT sort's turn start)
        int64_t lastTurnEndMs{ 0 }; // the newest terminal-stop reply or interrupt marker (its turn end)
        int32_t turnsTimed{ 0 };
        int64_t turnMsTotal{ 0 }, turnMsMax{ 0 };

        // Claude Code's own cost record (the LAST cost-state line)
        bool costState{ false };
        double costUsd{ 0 };
        int64_t apiMs{ 0 }, apiMsNoRetries{ 0 }, toolMs{ 0 }, wallMs{ 0 }, linesAdded{ 0 }, linesRemoved{ 0 };
        bool costUnknownModel{ false };

        // context
        int64_t contextTokens{ 0 }; // the Ctx column: the newest assistant usage sum, or a compaction's postTokens
        int64_t contextMax{ 0 }; // the largest input + cache read + cache write of one line

        // outcomes
        int32_t apiErrors{ 0 };
        std::wstring apiErrorStatusLast;
        int32_t refusals{ 0 }, maxTokenStops{ 0 }, aborted{ 0 }, interrupts{ 0 };
        int64_t stopHookMsTotal{ 0 }, stopHookMsMax{ 0 };

        // work done
        int32_t promptsTyped{ 0 }, promptsQueued{ 0 }, assistantLines{ 0 }, toolCalls{ 0 };
        std::map<std::wstring, int32_t> toolCallsByTool;
        std::vector<std::wstring> skillsUsed, mcpServers;
        int32_t subagentsSpawned{ 0 }, imagesPasted{ 0 };
        int32_t compactions{ 0 }, compactionsAuto{ 0 };
        int64_t compactionPreTokensMax{ 0 };
        int32_t recaps{ 0 };
        std::wstring recapLast;
        int32_t artifacts{ 0 };
        std::wstring artifactTitle, artifactUrl;

        // titles
        std::wstring customTitle, aiTitle, legacySummary, firstPrompt, lastPrompt, lastReply;

        // lineage
        std::wstring forkedFrom;
        int32_t forkPointLine{ 0 }; // the last line a fork copied from its parent

        // the end state of the last turn (end_state)
        std::wstring lastStopReason; // the newest assistant stop_reason
        bool lastWasInterrupt{ false }; // the newest turn-ending event was an interrupt marker
        std::wstring pendingAskId; // an AskUserQuestion with no answer yet

        // tool_use id -> name for calls whose result has not arrived (bounded: results arrive within a
        // turn; a result names its tool through this when its call was ingested on an earlier tick)
        std::map<std::wstring, std::wstring> pendingTools;
    };

    // Fold one line's facts into `f`.
    //   f        — the accumulator.
    //   facts    — ExtractLine's result for the line (a non-JSON line only counts in `lines`).
    //   lineNo   — the line's 1-based number in the file (the fork point).
    //   normDir  — NormDirKey (injected so the fold stays free of Persistence's link dependencies in tests).
    // Never throws beyond std::bad_alloc.
    void ApplyLine(SessionFold& f, const LineFacts& facts, int32_t lineNo, std::wstring (*normDir)(const std::wstring&));

    // The tool name a tool_result answers, from the calls folded so far (and forget that call: a call
    // has one result). "" when unknown (a call on a line the fold never saw).
    std::wstring TakeToolName(SessionFold& f, const std::wstring& toolUseId);

    // How the last turn ended, for a session that is not open: "completed" (a terminal stop),
    // "interrupted" (Esc), "question" (an AskUserQuestion with no answer), "unfinished" (cut off mid-turn),
    // or "" (no assistant line yet).
    std::wstring EndState(const SessionFold& f);

    // The fold as JSON text (UTF-8) for src_file.fold, and back. FromJson on an empty / malformed text
    // returns a default fold (the caller then re-ingests the file from 0 to rebuild it).
    std::string FoldToJson(const SessionFold& f);
    bool FoldFromJson(const std::string& json, SessionFold& out);
}
