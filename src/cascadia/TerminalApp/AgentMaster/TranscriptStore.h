// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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
        // notebook_path / path — Read/Edit/Write/Grep/Glob/... — PLUS absolute paths MINED from a
        // shell tool's `command` string via ExtractPathsFromText: quoted paths free-form, unquoted
        // with one interior space allowed inside a folder name). Feeds the Sessions page's 📁/📄
        // scope (filter by directories/files ACCESSED) and the inferred-workdir vote
        // (InferWorkingDirectory). Capped (8/line); always extracted (cheap).
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

    // ===== absolute-path mining from free text (tool commands) ===============================
    // Find absolute Windows paths INSIDE a text (a Bash tool's `command` string): a drive form
    // ("K:\x\y", forward slashes tolerated) or a UNC form ("\\server\share\x"), each requiring at
    // least one child segment below its root (a bare "K:\" / share root carries no signal). The
    // canonical tool fields (file_path/notebook_path/path) never carry the paths a shell command
    // touches — `cd`, `cat`, compiler/git arguments — so this is what lets those feed
    // TranscriptLineFacts::toolPaths (the 📁/📄 search scopes + the inferred-workdir vote).
    // Whitespace rule ("1 whitespace support in folder names"): a QUOTED path ("..." / '...')
    // consumes freely to its closing quote — spaces anywhere, leaf included. An UNQUOTED path may
    // contain a SINGLE interior space per segment, and only in a NON-LEAF segment — the space is
    // accepted only when a later path separator confirms it ("C:\Program Files (x86)\App\x.exe")
    // — so prose after a path ("cat K:\a\x.txt and then ...") is never swallowed into the match
    // (an unquoted leaf-with-space truncates at the space; its PARENT — what the inference votes
    // with — is still exact). Two consecutive spaces always terminate. Prose punctuation
    // terminates (`, ; : = & < > | * ?` + quotes; `:` only past the drive colon — so "K:\a\f.cs:12"
    // line refs stop clean); trailing sentence punctuation and unbalanced `)`/`]` are trimmed
    // (parenthesized folder names like "(x86)" survive). Matches are deduped case-insensitively,
    // each ≤512 chars, at most `maxPaths` returned, drive/UNC starts only at a non-alphanumeric
    // boundary (never mid-word). POSIX-style absolute paths ("/k/source/x") are deliberately NOT
    // mined — no drive/share marker to anchor on. Pure.
    std::vector<std::wstring> ExtractPathsFromText(std::wstring_view text, size_t maxPaths);

    // ===== inferred working directory (tab color modes) ======================================
    // Infer the directory a session ACTUALLY works in from its tool-touched paths — the files it
    // read / edited / created, the dirs it searched, AND the absolute paths its shell commands
    // named (ExtractPathsFromText above): TranscriptStats::pathsAccessed, already a deduped
    // per-file set (so one file edited 50 times votes once). Every path votes for its
    // whole ancestor DIRECTORY chain (its leaf excluded — tool paths are mostly files), and the
    // inferred dir is the DEEPEST directory holding a STRICT MAJORITY (>50%) of the votes: "the
    // most common shared path, ranked+picked by occurrence". Ancestor counts are monotone
    // (a parent's count >= a child's), so the majority set is a root-anchored chain and "deepest
    // majority" is well-defined; requiring the majority is what keeps a stray one-off read (a
    // ~/.claude settings file, a temp dir) from dragging the pick toward the drive root the way a
    // plain longest-common-prefix would, while depth preference keeps it from settling on a
    // too-shallow ancestor. Drive/UNC roots themselves never win (candidates start one segment
    // below the root token); a count/depth tie breaks lexicographically (determinism). Paths are
    // separator-normalized and compared by NormDirKey (case-insensitive on Windows, Rule #8); the
    // returned spelling is the first-seen original. No majority anywhere (paths split across
    // drives) or no usable paths => `fallbackDir` (the session's launch cwd). Pure.
    //
    // `gitRootOf` — the "Use .git folder to infer" arm (AppSettings::inferGitRoot, default ON).
    // When provided, it maps a DIRECTORY to its enclosing git ROOT (the nearest ancestor holding a
    // `.git` entry — dir or worktree/submodule FILE; empty = not in a repo). Each voting path's
    // PARENT dir resolves through it, the git roots are tallied, and a git root holding the same
    // STRICT MAJORITY of the voting paths IS the inferred dir — as-is, never deeper: a repo is ONE
    // working area, so a session concentrated in `repo\src\cascadia` infers `repo`, which normally
    // == its launch cwd (the "switching modes suddenly recolors my tab" fix — the shared-per-dir
    // and inferred modes then agree on the key). Nested repos (a worktree under the main checkout)
    // resolve per-path to the NEAREST root, so worktree work keys the worktree, not the outer
    // repo. Tallies are disjoint per path => at most one root can hold a strict majority (no
    // tiebreak needed). A resolver result that is a bare drive/share root is ignored (roots never
    // win, same as the ancestor arm). No git majority (paths split across repos, or mostly
    // outside any repo) => fall through to the ancestor majority-deepest above. The resolver is
    // injected so this stays PURE (the real caller wraps FindGitRootForDir, memoized); {} == the
    // classic two-arg behavior.
    std::wstring InferWorkingDirectory(const std::vector<std::wstring>& paths,
                                       const std::wstring& fallbackDir,
                                       const std::function<std::wstring(const std::wstring&)>& gitRootOf = {});

    // The REAL git-root resolver behind InferWorkingDirectory's `gitRootOf` (filesystem-touching —
    // inject it, don't call it from pure code): walk UP from `dir` to the nearest ancestor
    // containing a `.git` entry — a directory (a normal checkout) OR a file (a worktree /
    // submodule, e.g. this repo's `.claude\worktrees\<x>\.git`) — and return that ancestor.
    // Stops ABOVE the drive/UNC-share root (a root-level repo could never win the vote anyway, so
    // it is never probed/returned); relative / rootless input => empty. One GetFileAttributesW
    // per ancestor level — callers batch-memoize per NormDirKey (the inferred-color scan does).
    std::wstring FindGitRootForDir(const std::wstring& dir);

    // Drop every path that lives under any of `roots` (filesystem-aware: case-insensitive,
    // separator-normalized, SEGMENT-boundary matched — "C:\Temp" never swallows
    // "C:\Temperature\x"; a path equal to a root is dropped too). In-place; empty roots = no-op.
    // The inference-vote NOISE filter: a Claude session scratches under the machine temp
    // constantly (scripts, outputs, its per-session scratchpad dir), and those writes are
    // tool-touched paths like any other — a proven live failure had a session whose ONLY
    // captured path was one scratchpad `pr-body.md`, a 1/1 "majority" that inferred the
    // scratchpad dir (no git root above temp, so the git snap couldn't save it) and recolored
    // the tab off its repo. Temp is scratch BY DEFINITION, never a working directory, so the
    // inferred-color scan excludes it from the vote — the corpus that remains (possibly empty →
    // the honest cwd fallback) is what the session actually works ON. Applied ONLY at inference
    // time: the sidecar keeps the full set, so the Sessions 📁/📄 search scopes still match temp
    // paths. Pure.
    void ExcludePathsUnderRoots(std::vector<std::wstring>& paths, const std::vector<std::wstring>& roots);

    // The machine's transient scratch roots for ExcludePathsUnderRoots (Win32, not pure): the
    // effective user temp dir (GetTempPathW — resolves TMP > TEMP > USERPROFILE; long-formed via
    // GetLongPathNameW so an 8.3 "RUNNER~1" spelling still prefix-matches the long paths
    // transcripts carry) plus the system "<windir>\Temp". Best-effort — a lookup that fails just
    // contributes nothing.
    std::vector<std::wstring> CollectMachineTempRoots();

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

}
