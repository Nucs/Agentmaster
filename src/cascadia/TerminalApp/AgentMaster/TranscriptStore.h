// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — TranscriptStore: the on-disk Claude-session store API (SESSIONS.md §6).
//
// Everything the Sessions browser (and any other consumer) needs to ENUMERATE, TIME, TITLE,
// COUNT, and SEARCH-INDEX the Claude Code sessions under the global `~/.claude/projects/`,
// grounded in the corpus survey (SESSIONS.md §2-§5; 1,817 uuid transcripts, versions 2.0.50 →
// 2.1.170 — the API tolerates every stratum but is tuned for the recent 1-3 months):
//
//   - A session == a TOP-LEVEL `<uuid>.jsonl` under `projects/<enc-cwd>/` (filename stem ==
//     every line's sessionId). Legacy `agent-*.jsonl`, `memory/`, and `<sid>/` subdirs are not
//     sessions and are excluded by the uuid-stem filter.
//   - File mtime is a SUPERSET signal only (mtime >= last-activity always holds, but the gap's
//     median is ~1 h and the max ~43 days — background `away_summary` forks, exit-time flushes
//     of untimestamped state lines, and bulk metadata passes all touch the file without a
//     turn). So: `(size, mtime)` is the cache-invalidation key; the DISPLAYED created /
//     last-activity come from the LINES' own `timestamp` fields (ReadTranscriptQuickFacts).
//   - The literal last line is almost never a timestamped message (system/permission-mode/
//     last-prompt tails dominate), and single lines reach >1 MiB — the tail scan reads a
//     growing window and walks backwards.
//   - A FORK (`/branch`, `/rewind` restore, `--fork-session`, in-session /resume-to-a-message)
//     duplicates the parent's subtree VERBATIM (same uuids + timestamps) with
//     `forkedFrom:{sessionId, messageUuid}` stamped on every copied line — so a fork's
//     line-timestamps lie about its creation (use file birth) and search hits dedupe by uuid.
//   - Title precedence: customTitle (last `custom-title` line) > aiTitle (last `ai-title`) >
//     legacy `summary` (v2.0.75-2.1.25 stratum only) > the first REAL human prompt.
//   - "REAL human prompt" excludes the control/noise markers that masquerade as user lines:
//     `<command-name>`/`<command-message>`/`<local-command`/`<bash-*>`/`<task-notification>`/
//     `<system-reminder`/`<teammate-message`/`Caveat:`/`[Request interrupted` (IsNoiseUserPrompt)
//     plus isMeta / isCompactSummary / isSidechain lines.
//
// Plain C++ + Win32, no WinRT (the .cpp is <PrecompiledHeader>NotUsing and links into the
// standalone test harness). The line-level primitives are PURE (string -> values) so the
// harness exercises them with canned lines; only Enumerate*/Scan*/Read* touch the filesystem.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Agentmaster
{
    // ===== enumeration =======================================================================

    // One enumerated on-disk Claude session (a top-level `<uuid>.jsonl`).
    struct TranscriptRef
    {
        std::wstring sessionId; // the filename stem (== every line's sessionId)
        std::wstring path; // full path to the .jsonl
        std::wstring projectDirLeaf; // the encoded project-dir leaf (e.g. "K--source-Agentmaster") — LOSSY; the real cwd comes from the lines
        int64_t sizeBytes{};
        int64_t mtimeMs{}; // last write — the SUPERSET window filter + half the (size,mtime) invalidation key
        int64_t birthMs{}; // file creation time (a FORK's true creation; line timestamps lie for forks)
    };

    // True iff `stem` is a 36-char hyphenated UUID — the session-transcript filename shape.
    // (Excludes legacy `agent-<7hex>` files and anything else.) Pure.
    bool IsSessionIdStem(std::wstring_view stem);

    // Enumerate every session transcript across ALL project dirs under `projectsDir` whose
    // mtime >= sinceUnixMs (0 == everything). mtime >= last-activity always holds, so this never
    // misses a session active inside the window (it can only over-include; refine with
    // ReadTranscriptQuickFacts). Newest-mtime first. Tolerates the sweep deleting files mid-walk
    // (a vanished file is simply skipped). ~0.15 s for ~2,600 files across ~170 dirs.
    std::vector<TranscriptRef> EnumerateTranscriptsIn(std::wstring_view projectsDir, int64_t sinceUnixMs);
    // Against the live Claude projects root (ClaudeProjectsDir()).
    std::vector<TranscriptRef> EnumerateTranscripts(int64_t sinceUnixMs);

    // ===== pure line-level primitives ========================================================

    // A transcript line's `timestamp` ("2026-06-10T12:34:56.789Z" — every recent line is ISO-Z;
    // the oldest strata carried epoch numbers) -> Unix ms. Accepts ISO-8601 with optional
    // fractional seconds + optional trailing 'Z', or a pure-digit epoch (>= 12 digits == ms,
    // 9-11 digits == seconds). 0 on anything else. Pure.
    int64_t ParseTranscriptTimestamp(std::wstring_view ts);

    // True iff a user-line prompt body is a control/noise marker, not a human message: command
    // echoes (`<command-name>` / `<command-message>` / `<local-command`), bash-tool echoes
    // (`<bash-input>` / `<bash-stdout>` / `<bash-stderr>`), background-task notifications
    // (`<task-notification>`), injected reminders (`<system-reminder`, `Caveat:`), teammate
    // traffic (`<teammate-message`), and interrupt markers (`[Request interrupted`). The
    // noise-suppression rule set proven by the user's own exit hook + corpus survey
    // (SESSIONS.md §6a). Leading whitespace is ignored. Pure.
    bool IsNoiseUserPrompt(std::wstring_view prompt);

    // The display-title precedence in ONE place (SESSIONS.md §6.3): customTitle > aiTitle >
    // legacy summary > the first REAL prompt (already collapsed to a display line by the
    // extractors). Returns the first non-empty, else empty. Pure.
    std::wstring PickDisplayTitle(std::wstring_view customTitle, std::wstring_view aiTitle, std::wstring_view summary, std::wstring_view firstPrompt);

    enum class TranscriptLineKind
    {
        Other, // unrecognized / state lines (mode, permission-mode, last-prompt, progress, pr-link, ...)
        UserPrompt, // a human-prompt-SHAPED user line (string / pure-text content); REAL iff !meta && !sidechain
        UserToolResult, // a user line carrying tool_result blocks (a tool turn, not a human message)
        Assistant, // an assistant line (text / thinking / tool_use blocks)
        System, // system lines (away_summary, api_error, compact_boundary, turn_duration, ...)
        Attachment, // injected context (task_reminder, file snippets, ...) — never indexed as conversation text
        CustomTitle, // {"type":"custom-title","customTitle":...} — the user-set conversation name
        AiTitle, // {"type":"ai-title","aiTitle":...} — the async picker title
        Summary, // {"type":"summary","summary":...} — LEGACY (v2.0.75-2.1.25); kept for old strata
        QueueOperation, // prompt-queue ops (a queue-op FIRST line marks a task file, §6a noise rule)
        FileHistorySnapshot, // checkpoint index lines
    };

    // Everything one classified transcript line contributes to browse/search/stats. The text
    // fields are CAPPED by the caller-supplied budgets (a single line can exceed 1 MiB):
    //   maxUserTextChars — cap for `userText` (the human prompt body; 0 = don't extract)
    //   maxAgentTextChars — cap for `agentText` (assistant text + thinking + tool_use name/input,
    //                       tool_result bodies, toolUseResult stdout/stderr/content, system
    //                       content; 0 = don't extract). Attachment payloads are deliberately
    //                       NOT extracted (injected context, not conversation — §6.4).
    struct TranscriptLineFacts
    {
        TranscriptLineKind kind{ TranscriptLineKind::Other };
        std::wstring uuid; // the line's own `uuid` (its conversation-tree node id); empty for state/marker lines
        // Agentmaster (revert-aware): the per-message "IsActiveLeaf" flag — true when this message is on
        // the ACTIVE conversation branch (the chain from the current leaf to root), false when a double-ESC
        // rewind abandoned its branch. ClassifyTranscriptLine alone leaves it true (a single line has no
        // tree context); ClassifyTranscriptLines stamps it from the whole-file leaf chain. DISPLAY keeps
        // only true; SEARCH ignores it (every line stays indexed/findable).
        bool onActiveBranch{ true };
        int64_t timestampMs{}; // 0 when the line carries no timestamp (state/tail lines)
        bool sidechain{}; // isSidechain:true (inline subagent lines, old strata) — never main-chain state/stats
        bool meta{}; // isMeta / isCompactSummary / a noise prompt — not a human message
        int toolUses{}; // # of assistant tool_use blocks on this line
        int64_t contextTokens{}; // Assistant only: message.usage sum (input + cache_creation + cache_read + output) ≈ this turn's context size; 0 when no usage block
        std::wstring userText; // UserPrompt only (REAL prompts only), capped
        std::wstring agentText; // Assistant / UserToolResult / System searchable text, capped
        std::wstring title; // CustomTitle / AiTitle / Summary payload (collapsed to one line)
        std::wstring forkedFromId; // forkedFrom.sessionId when stamped (=> the file is a fork copy)
        std::wstring cwd; // the line's cwd field, when present
        std::wstring gitBranch; // the line's gitBranch field, when present
        // Filesystem paths this line's tool calls touched (assistant tool_use inputs: file_path /
        // notebook_path / path — Read/Edit/Write/Grep/Glob/...). Feeds the Sessions page's 📁/📄
        // scope (filter by directories/files ACCESSED). Capped (8/line); always extracted (cheap).
        std::vector<std::wstring> toolPaths;
    };

    // Classify + extract ONE transcript line (any stratum; tolerant of unknown types). Pure.
    TranscriptLineFacts ClassifyTranscriptLine(std::wstring_view line, size_t maxUserTextChars = 0, size_t maxAgentTextChars = 0);

    // ===== active-branch (revert-aware) reconstruction =======================================
    // A Claude conversation is an append-only TREE: every message line carries `uuid` +
    // `parentUuid` (a root's parentUuid is null/absent). A double-ESC REWIND (or `/rewind`)
    // repoints the conversation's LEAF to an earlier node — it does NOT delete the abandoned
    // branch's lines, which stay in the .jsonl INTERLEAVED with the live ones (in-flight tool
    // results from the dead branch keep landing AFTER the new branch starts, so the discarded set
    // is NOT a contiguous prefix). Claude records the current leaf in the trailing `leafUuid` of
    // its `last-prompt` markers (the LAST one in file order is authoritative).
    //
    // DISPLAY surfaces (the summary panel, a session's prompt list/title) must show ONLY the live
    // branch — the chain from the current leaf back to the root. SEARCH/index surfaces deliberately
    // DO NOT use this (the sidecar index + content scan keep indexing EVERY line, so a reverted
    // message stays findable — we just never DISPLAY it).
    //
    // Given a transcript's COMPLETE text, return the set of message `uuid`s on the active branch.
    // Returns EMPTY when the text carries no `leafUuid` marker (a pre-leaf-marker stratum, or a
    // truncated HEAD read whose tail markers are absent) OR when the leaf names a node that isn't
    // present — callers MUST treat an empty set as "no revert info => keep every line" (the legacy,
    // all-messages behavior), and MUST NOT call this on a truncated/head read (an early in-window
    // `last-prompt` marker would name a STALE leaf). Pure; no file IO; cycle-safe.
    std::unordered_set<std::wstring> ActiveBranchUuids(std::wstring_view transcriptText);

    // Classify EVERY line of a transcript's COMPLETE text into per-message facts, AND stamp each
    // message's `onActiveBranch` ("IsActiveLeaf") from the current leaf chain (ActiveBranchUuids). The
    // reusable per-message view carrying the active/reverted status: DISPLAY consumers keep only
    // onActiveBranch==true; SEARCH keeps all. uuid-less state/marker lines stay onActiveBranch==true.
    // Pass `markActiveBranch=false` when `transcriptText` is a PARTIAL / HEAD read (the tail leaf marker
    // is absent and any in-window marker would name a STALE leaf) — then NOTHING is marked inactive
    // (keep-all). maxUserTextChars / maxAgentTextChars cap the extracted bodies (0 == don't extract that
    // body), per ClassifyTranscriptLine. Pure; no file IO.
    std::vector<TranscriptLineFacts> ClassifyTranscriptLines(std::wstring_view transcriptText, size_t maxUserTextChars = 0, size_t maxAgentTextChars = 0, bool markActiveBranch = true);

    // ===== streaming scan + stats (the incremental-index primitives) =========================

    // Stream the file's COMPLETE lines from byte offset `fromOffset`, invoking `sink` per line's
    // facts, bounded memory (chunked reads; a partial trailing line is left for the next call —
    // the SessionScanner tail-cursor pattern, which is exactly what makes the page's per-session
    // index INCREMENTAL: re-call from the cached offset after an append). Returns the new offset
    // (>= fromOffset; == file size when fully consumed), or -1 when the file can't be opened.
    // Shares read/write/delete so a live claude is never blocked.
    using TranscriptLineSink = std::function<void(const TranscriptLineFacts&)>;
    int64_t ScanTranscript(const std::wstring& path, int64_t fromOffset, size_t maxUserTextChars, size_t maxAgentTextChars, const TranscriptLineSink& sink);

    // Accumulated per-session stats — ONE struct that doubles as the resume cursor: feed the
    // same instance back after an append and only the suffix past `parsedBytes` is read. A size
    // SHRINK means the file was replaced — the accumulate resets the struct and rebuilds from 0
    // automatically. All time/activity fields derive from the LINES, never file mtime.
    struct TranscriptStats
    {
        bool found{}; // the file existed and was readable on the last accumulate
        int64_t parsedBytes{}; // resume cursor: bytes of COMPLETE lines consumed so far
        int64_t firstTimestampMs{}; // first timestamped line (created — but a FORK's is copied; prefer birth, see quick facts)
        int64_t lastTimestampMs{}; // last non-sidechain user/assistant timestamp == the REAL last activity
        int userPrompts{}; // REAL human prompts (noise/meta/sidechain filtered)
        int assistantLines{}; // assistant lines (NOT turns — one turn can span several lines)
        int toolUses{}; // total assistant tool_use blocks
        int64_t contextTokens{}; // context occupancy: the NEWEST assistant line's message.usage sum (≈ tokens in the session right now); 0 until an assistant turn carries a usage block — the same value the Triage-Board card shows as "ctx N"
        std::wstring customTitle; // last custom-title (newer retitles win)
        std::wstring aiTitle; // last ai-title
        std::wstring summary; // last legacy summary
        std::wstring firstUserPrompt; // the title fallback (first REAL prompt, display-collapsed)
        std::wstring forkedFromId; // non-empty => this file is a fork copy of that session
        std::wstring cwd; // first cwd seen on a line (the REAL dir; the folder name is lossy)
        std::wstring gitBranch; // first gitBranch seen
        // Deduped union of every tool-touched path (TranscriptLineFacts::toolPaths), capped at
        // kMaxPathsAccessed — the 📁/📄 search scopes' haystack (dirs derive from these + cwd).
        std::vector<std::wstring> pathsAccessed;
    };

    inline constexpr size_t kMaxPathsAccessed = 512;

    // One incremental accumulate: scan [stats.parsedBytes, EOF) and fold into `stats`. Returns
    // false (stats.found=false) when the file is gone — the sweep deletes mid-listing; callers
    // drop the row. The display title is PickDisplayTitle(customTitle, aiTitle, summary,
    // firstUserPrompt).
    bool AccumulateTranscriptStats(const std::wstring& path, TranscriptStats& stats);

    // ===== cheap row facts: head + growing-tail windows, never a full read ===================

    // The browse-row essentials, extracted without reading the body: `created` from the FIRST
    // timestamped line (head window, grown when a giant first line — e.g. a file-history-snapshot
    // — overflows it), EXCEPT a fork (forkedFrom in the head), whose copied timestamps lie —
    // there `fileBirthMs` (the enumerated TranscriptRef::birthMs) is the truth; `lastActivity`
    // from the LAST non-sidechain user/assistant timestamped line (tail window, grown ×4 past
    // >1 MiB single lines, capped — 0 when the transcript has no timestamped message at all,
    // e.g. a never-prompted prefix-only file; callers fall back to birth/mtime).
    struct TranscriptQuickFacts
    {
        bool found{};
        int64_t createdMs{}; // first line timestamp; forks: fileBirthMs; 0 when nothing timestamped
        int64_t lastActivityMs{}; // last user/assistant timestamp; 0 when none
        bool fork{}; // forkedFrom seen in the head
        std::wstring forkedFromId; // the fork's parent session id (group rows under the root)
        std::wstring cwd; // from the head window (the REAL dir for grouping; PathEq it)
    };
    TranscriptQuickFacts ReadTranscriptQuickFacts(const std::wstring& path, int64_t fileBirthMs);

    // ===== live-session presence (`~/.claude/sessions/<pid>.json`) ===========================
    // Claude Code's own heartbeat presence files: pid ↔ sessionId ↔ cwd plus a self-reported
    // `status` (busy | idle | waiting | shell) and `updatedAt`. RAW reads — stale files linger
    // after crashes, so the OBSERVER validates each row against its process snapshot (pid alive
    // AND still a claude.exe) before publishing. Separation contract (SESSIONS.md §7-Q5): the
    // STORE owns every claude-session domain read; everything live/changing is surfaced to the
    // app through the Fleet Observer's published tables, never read ad-hoc by the UI.
    struct SessionPresenceRow
    {
        uint32_t pid{};
        std::wstring sessionId;
        std::wstring cwd;
        std::wstring status; // busy | idle | waiting | shell (claude's own heartbeat state)
        std::wstring version; // claude version that wrote the file
        int64_t startedAtMs{};
        int64_t updatedAtMs{};
    };
    std::vector<SessionPresenceRow> ReadSessionPresenceIn(std::wstring_view sessionsDir);
    // Against the live `<claude home>/sessions` (the sibling of ClaudeProjectsDir()).
    std::vector<SessionPresenceRow> ReadSessionPresence();

    // ===== the per-session sidecar index (`~/.agentmaster/sessions-index/<sid>.json`) ========
    // The Sessions page's cache (SESSIONS.md §6.2): one sidecar per session holding the
    // accumulated TranscriptStats + the (sizeBytes, mtimeMs) invalidation key. Load-or-refresh:
    // key matches => parse the small sidecar only (no transcript IO); size grew => RESUME the
    // accumulate from the stored parsedBytes (only the appended suffix is read) and rewrite;
    // shrink => stats auto-rebuild. The fast search phase greps these sidecars + history.jsonl;
    // only the slow phase touches transcripts.
    struct SessionIndexEntry
    {
        bool valid{}; // the transcript existed and the entry is current
        std::wstring sessionId;
        std::wstring path; // transcript path at index time (key the CACHE by sid — /cd relocates files)
        int64_t sizeBytes{};
        int64_t mtimeMs{};
        int64_t birthMs{};
        TranscriptStats stats;
        // Runtime overlay — NOT persisted to the sidecar (the load/store above is explicit
        // field-by-field, so this stays empty across them). The Sessions page sets it, after a
        // gather, to the REAL tab title of a session OPEN in an Agentmaster window (the registry
        // title — Rule #11). SearchIndexFast folds it into the title haystack under
        // SessionQuery::scopeTitle, so a renamed open session is findable by the name shown. Empty
        // for on-disk / archived sessions (their title precedence already lives in `stats`).
        std::wstring liveTitle;
    };
    SessionIndexEntry LoadOrRefreshSessionIndexIn(const std::wstring& indexDir, const TranscriptRef& ref);
    // Against the live index dir (<AgentmasterStateDir>\sessions-index, created on demand).
    SessionIndexEntry LoadOrRefreshSessionIndex(const TranscriptRef& ref);

    // ===== continuation-chain lineage (/clear + plan-restart "where did this go?") ===========
    // A logical Claude conversation that gets `/clear`ed — or plan-restarted via a slash command,
    // or continued by pasting a handover into a fresh session — mints a NEW session id each time,
    // and the resulting files are UNLINKED on disk. Verified across this corpus: NO parentUuid
    // chain across files, NO forkedFrom (that stamps a `/branch` FORK, a divergent branch — not a
    // continuation), NO isCompactSummary (a true `/compact` stays in ONE file via a
    // `system/compact_boundary`), and NO `SessionEnd`/next-id pointer. So the only evidence that B
    // continues A is STRUCTURAL: same cwd + B created shortly AFTER A's last activity + B is not a
    // fork. resume / restore / fork follow this chain to its TAIL so the user lands where they LEFT
    // OFF — not on the earliest link they happen to recognize by its original first-prompt title.
    // (SESSIONS.md §4 / §324.2's "plan-mode parent->child chains".)

    // Max gap from one session's last activity to the next session's creation for them to count as
    // the SAME continued conversation. Deliberately conservative: an automatic redirect must never
    // merge two genuinely-separate same-dir conversations (a return hours later is a NEW one), but
    // it must span a /clear-rephrase-and-paste pause (observed up to ~11 min on a real chain).
    inline constexpr int64_t kContinuationGapMaxMs = 15 * 60 * 1000; // 15 minutes
    // Small negative tolerance: B may be timestamped a hair before A's last main-chain line (clock
    // skew / a trailing untimestamped write); within this, "B starts after A ends" still holds.
    inline constexpr int64_t kContinuationSkewMs = 5 * 1000; // 5 seconds

    // One on-disk session reduced to what chain-linking needs. created/lastActivity come from the
    // transcript LINES (ReadTranscriptQuickFacts), never file mtime (which lies — §5).
    struct SessionChainNode
    {
        std::wstring sessionId;
        std::wstring cwd; // the REAL cwd (compare with NormDirKey — Rule #8)
        int64_t createdMs{}; // first activity (a fork's is the file birth)
        int64_t lastActivityMs{}; // last user/assistant timestamp (== createdMs when never-prompted)
        bool fork{}; // a `/branch` fork is a BRANCH, never a continuation link
    };

    struct SessionChainResult
    {
        std::wstring tailId; // the chain tail (== startId when there is no newer continuation)
        int hops{}; // continuation links followed (0 == no redirect)
    };

    // Pure: from `startId`, follow continuation edges to the chain TAIL. An edge A->B holds iff
    // NormDirKey(A.cwd)==NormDirKey(B.cwd) && !B.fork && B is the EARLIEST same-cwd non-fork session
    // created in [A.lastActivity - skewMs, A.lastActivity + gapMaxMs]. Bails (no further edge) on
    // AMBIGUITY — a second candidate whose lifetime OVERLAPS B's (started at/before B's last
    // activity), i.e. parallel same-cwd sessions — so two claudes in one dir are never silently
    // merged (Rule #14 spirit). Returns {startId, 0} when startId isn't in `nodes` or has no
    // continuation. Cycle-safe (a visited set + a hop cap). Deterministic.
    SessionChainResult ResolveContinuationChainTail(const std::vector<SessionChainNode>& nodes,
                                                    const std::wstring& startId,
                                                    int64_t gapMaxMs = kContinuationGapMaxMs,
                                                    int64_t skewMs = kContinuationSkewMs);

    struct ContinuationTail
    {
        std::wstring tailId; // resolved tail (== input sessionId when no newer continuation)
        std::wstring tailCwd; // the tail's own cwd (falls back to the input cwd)
        std::wstring tailTitle; // the tail's display title (PickDisplayTitle); empty when only a redirect needs it
        int hops{};
    };

    // Filesystem: enumerate the on-disk sessions in `sessionId`'s project dir (the encoding of
    // `cwd`), build the chain nodes (ReadTranscriptQuickFacts for timing/fork/cwd), resolve the
    // tail, and — only when it differs — read the tail's title from its sidecar index. Returns
    // {sessionId, cwd, "", 0} when there's no newer continuation, the dir is unreadable, or the
    // session isn't present. Read-only.
    ContinuationTail ResolveContinuationTailOnDisk(const std::wstring& sessionId, const std::wstring& cwd);

    // Pure: the DIRECT continuation PREDECESSOR of `targetId` among `nodes` (the A in A->targetId), or
    // empty — the EXACT inverse of ResolveContinuationChainTail's forward edge (both share the single
    // ContinuationNext hop, so reverse lineage can't drift from forward redirect). Empty when no node
    // continues into targetId, or when MORE THAN ONE does (ambiguous — never guess). Drives cross-file
    // conversation LINEAGE (the summary panel's "previous session(s)" across a /clear or plan-restart
    // join — CollectConversationLineage). [Agentmaster]
    std::wstring ResolveContinuationPredecessor(const std::vector<SessionChainNode>& nodes,
                                                const std::wstring& targetId,
                                                int64_t gapMaxMs = kContinuationGapMaxMs,
                                                int64_t skewMs = kContinuationSkewMs);

    struct ContinuationPredecessor
    {
        std::wstring predId;  // the session that continues INTO the queried one (empty == none / ambiguous)
        std::wstring predCwd; // the predecessor's own cwd (for recursing the lineage walk)
    };

    // Filesystem: enumerate `sessionId`'s project dir (the encoding of `cwd`), build the chain nodes,
    // and resolve the DIRECT predecessor (the inverse of ResolveContinuationTailOnDisk — "where did
    // this conversation COME FROM?"). Read-only; {empty,empty} when there's no unambiguous predecessor,
    // the dir is unreadable, or only one session is present. [Agentmaster]
    ContinuationPredecessor ResolveContinuationPredecessorOnDisk(const std::wstring& sessionId, const std::wstring& cwd);
}
