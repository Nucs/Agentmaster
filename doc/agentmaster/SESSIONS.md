# Agentmaster — Sessions Browser (the "Sessions" page) + the `~/.claude` storage map

> **Status: IMPLEMENTED (engine + UI; pending deploy).** The store API (`TranscriptStore`), the
> two-phase search (`SessionSearch`), the presence→observer integration, and the page itself
> (`TerminalPage.AgentSessionsPage.cpp`, the "Sessions" toolbar button after Archived) are
> built, lib-compiled green, and engine-tested (926 harness checks). §7 records the user's
> decisions. Two things in one doc: **(1)** the feature spec for a
> global **Sessions browser** — a full-window page behind a new **"Sessions"** toolbar button,
> right after **Archived**, listing *every* Claude Code session on the machine in a selectable
> time window with full-text search; **(2)** the map it stands on — how Claude Code stores
> sessions inside the global `~/.claude` folder and **every relationship between the session
> files**, researched online (official docs + community parsers) and **verified against this
> machine's live corpus** (Claude Code **2.1.170**; 2,635 top-level `.jsonl` = **1,817 uuid
> session transcripts + 818 legacy `agent-*` files**, 169 project dirs, 4.2 GB; 499 sessions in
> the 92-day window; spot-checks re-run independently).
> Companions: [`STATE.md`](./STATE.md) (the state-bearing JSONL subset) · [`OBSERVER.md`](./OBSERVER.md) ·
> [`DESIGN.md`](./DESIGN.md) · [`HOOKS.md`](./HOOKS.md).

---

## 1. The feature (spec)

- **Toolbar:** Launch · Reopen · `⚙` · Pause Prompt Queue Runner · Archived · **Sessions** *(new — right
  after Archived)*.
- **Page shell:** duplicate the Archive page structure (`_BuildArchivePageShell` /
  `_ShowArchivePage` — full-window, mounted over `TerminalPage`'s Root content rows, ← Back to
  dismiss, **every pointer handler defers its visual-tree mutation** — the XAML-Islands hit-test
  AV class), with a **search bar at the top** replacing the Archive page's filter box.
- **Content:** all Claude Code sessions on this machine whose **last activity** falls inside the
  selected window (default **1 month**) — not just Agentmaster-managed ones: everything under
  `~/.claude/projects/`.
- **Search bar:** `[ search for sessions ] (👤) (🤖) (📁) (📄) (F) [1 month] (↻)`
  - **`↻` Refresh** (`_sessRefreshBtn`, rightmost) re-runs the gather pass — re-enumerate the
    window's transcripts + load-or-refresh every sidecar index — so a session created (or grown)
    since the page opened shows up and its new content folds into the search index. Same pass as
    opening the page; the `_sessionsIndexing` flag dedupes an in-flight gather, so a double-click
    is safe.
  - All five toggles are **toggle buttons** (click = select/deselect, visible selected state):
    - `👤` selected ⇒ the search also scans **user messages** (typed prompts).
    - `🤖` selected ⇒ the search also scans **agent + tools** (everything *but* user messages:
      assistant text/thinking, tool inputs/results, system content).
    - `📁` ⇒ match **directories accessed** (tool-call paths' directory parts + the cwd).
    - `📄` ⇒ match **files accessed** (tool-call paths' leaves).
    - **Directory matching is slash-INSENSITIVE** — for the cwd + `📁` dir-parts, `/` and `\` are
      equivalent (`MatchesPathQuery` folds both sides to `/`), so a `src/foo` query finds a Windows
      `src\foo` cwd and vice versa. A separator-free term is unaffected (defers to `MatchesQueryText`,
      allocation-free); `📄` leaves never hold a separator so stay a plain match.
    - `(F)` ⇒ **fuzzy** (query characters in order, gaps allowed — identical semantics in rg
      and the in-process matcher).
  - **Defaults:** `📁` + `📄` start **ON** — they ride the FAST phase only (in-memory match
    over the sidecar index's `pathsAccessed`; no rg, no transcript IO — effectively free), so
    path queries work out of the box. `👤` also starts **ON** — it enables the SLOW
    rg-prefiltered transcript content scan for your own typed prompts, so a search reads the
    conversations by default (slower than the path scopes; results arrive as found). `🤖` starts
    **OFF** — the heaviest scope: agent/tool text raw-matches almost any query, so the file
    prefilter passes most of the window. `(F)`
    starts **OFF** (a semantics toggle — subsequence matches are noisy as a default and its
    `.*?` patterns inflate the slow phase's candidate set).
  - **Query grammar** (`ParseSessionQuery`, `SessionSearch.h`): the text splits into
    whitespace-separated terms that **AND-match** — every term must hit, each may hit a
    *different* field (the Archive page's token semantics). `"quoted phrase"` = ONE **exact**
    contiguous term (spaces kept; `(F)` never applies inside quotes; an unterminated quote runs
    to the end; empty `""` is dropped). A bare **whole session-id GUID** token (8-4-4-4-12 hex,
    `{braces}` tolerated) also matches the session's **identity** — its id *and* its
    fork-parent id — so an id pasted from `hooks.log` / the detail pane finds that session plus
    its forks; it still matches as literal text too (additive), quoting it makes it pure text,
    and it is never fuzzied (a 36-char hex subsequence would match almost anything). In the
    content phases a guid term **scopes** hits to that session (satisfied by the line's/file's
    own session id); a guid-only query is answered by the fast phase's identity match alone.
  - `[1 month]` — the window selector: **hover** opens a From/To **range popup** (text boxes
    with placeholder dates — answer Q4); **click** cycles
    `1 day → 3 days → 7 days → 14 days → 1 month → 3 months → (wrap)`.
- **Caching:** per-session search/index cache; **invalidated when the session's last-active
  changes** (mechanically: the `(size, mtime)` pair — see §6.2).
- **Opened sessions get more:** a session that is currently OPEN in this app (live in the
  registry) renders enriched — its **per-working-dir tab color** chip, the live **state glyph**,
  and Jump-to-tab; see §6.6 for the full row-affordance matrix.
- **Edit title (rename in place):** a row's **right-click → "Edit Title"** OR a **"slow double-click"**
  (re-clicking the already-selected row — the Windows-Explorer rename gesture, disambiguated from a fast
  double-click by an OS-`GetDoubleClickTime` timer that a `DoubleTapped`=resume disarms) swaps the row's
  Title cell for an in-place `TextBox` (`_BeginSessionsRename` — a `ContentDialog` text box gets no
  keypresses under XAML Islands, so editing is inline, the Manager's Explorer-tree rename idiom). Commit
  on **Enter** / focus-loss, cancel on **Escape** (both deferred so the re-render can't tear the box out
  mid-keystroke; a blank edit keeps the old title — a title never goes empty, Rule #11). The new title is
  **persisted durably** (`_PersistEditedSessionTitle`): a session **known to the registry** (open OR
  archived) routes through the live rename (`_RenameClaudeSession` → registry + the WT tab if open + the
  Engine observer mirrors it to the **SessionStore** `title` key AND `sessions.json` — Rule #11, the title
  is ONE value); a **pure on-disk** session (never managed) writes the SessionStore directly
  (`SetStoredSessionTitle`) — exactly the overlay the browser reads for closed/historical rows
  (`LoadAllStoredSessionTitles`). Either way the edit reflects instantly into the in-memory rows + search
  index (no re-gather) and survives across windows/runs.
- **Hide from list:** a row's **right-click → "Hide from list"** drops that session from the
  browser (`_HideSessionFromList`), persisted in `AppSettings.hiddenSessionIds` via a freshest-disk
  read-modify-write (the splitter/`treeSort` pattern), so it survives restarts. The render filters
  the set out at its single chokepoint (`_RenderSessionsTable` — covers both the empty-query and
  search paths) and the count shows `· N hidden`. It is a pure **browse-list preference** — the
  transcript on disk is never touched and nothing else (registry / Triage Board / Archive) reads the
  list. **Resettable** from the Settings cog's **"Reset hidden sessions"** button (`_ResetHiddenSessions`
  via `SetResetHiddenSessionsHandler`), which clears the set and re-shows every hidden session. The
  cog FORM never edits the list, so the cog's Save preserves it from freshest disk (no regression).
- **Filter ▸ (row right-click):** a **`Filter`** submenu on every row narrows the list to sessions
  **like the clicked ("anchor") row** in one dimension — **By Same Directory** (filesystem-aware,
  `NormDirKey` — Rule #8), **By Same Branch** (exact, shown only when the row has a branch), **By Same
  Day / Week / Month** (the row's **created** time bucketed in **local** time — DST-safe `[start, end)`
  via `SessLocalBucket`; week is Monday-start), and **By Fork Family** (the anchor's connected
  fork-graph component — its forks + fork-parent — shown only when the row has lineage). Each is a
  `_SessionsRowFilterKind` facet of `_sessionsRowFilter`; facets **stack across dimensions as AND**
  (dir AND branch AND time AND family), the time dimension holds **one granularity at a time** (picking
  Week replaces Day), and a facet the anchor row already matches reads **✓** and toggles **OFF** on
  re-click (`_SessionsRowFilterMatchesAnchor` drives both the ✓ and the toggle direction). It is
  applied at the SAME render chokepoint as Hide/Open/search (`_SessionsRowPassesRowFilter` in
  `_RenderSessionsTable`), so it composes (**AND**) with the search text **and** the scope/Open/Hidden
  toggles. A **`✕ filter: …`** chip beside the search box shows the active facets and clears them all
  on click (the submenu also carries **Clear filters**); the count line notes `· filtered`. Like the
  hidden set it is a pure **browse-list preference** — nothing is persisted and the transcript on disk
  is never touched (it resets on app restart, unlike `hiddenSessionIds`).
- **`📂 Folders ▾` (the folder filter, §10):** a dropdown in the filters row listing the folders the
  listed sessions worked in — each session's launch folder **and** its inferred folder — split
  **50% / 50%** into **RECENT** over **MOST USED**, with a find box. The picked folders combine with
  **OR**; the folder filter as a whole ANDs with everything above and reads in the `✕ filter` chip.

### 1a. Resolved behaviors (proposals — confirm before build)

- **Both toggles OFF** ⇒ the search matches **title + working directory only** (cheap, no
  content scan). Both ON ⇒ union of the two scopes.
- The **date window filters by last-activity**, not creation.
- **Forks are grouped under their root** conversation (`forkedFrom` lineage — exactly what
  claude's own `/resume` picker does), so a `(Branch)` copy doesn't read as a distinct session.
- Search results **filter rows** (per-session hit), with the hit count as a row adornment;
  per-message hit navigation is a later increment (§7-Q3).

---

## 2. The `~/.claude` directory map (verified)

`~/.claude` == `%USERPROFILE%\.claude`; `CLAUDE_CONFIG_DIR` relocates everything. "Swept" =
deleted by the startup sweep when older than **`cleanupPeriodDays`** (default **30**, min 1;
this machine: 2555 ≈ 7 years ⇒ nothing pruned). User-added entries (skills, commands, hooks, …) omitted.

| Entry | Purpose | Keyed by | Swept |
| --- | --- | --- | --- |
| `projects/<enc-cwd>/<sid>.jsonl` | **The session transcript** (append-only JSONL) | encoded start-cwd + session UUID | yes |
| `projects/<enc-cwd>/<sid>/subagents/agent-<aid>.jsonl` + `.meta.json` | Task-tool **subagent** transcripts + metadata (`{agentType, description, name, toolUseId}`) | parent sid + agentId | with parent |
| `projects/<enc-cwd>/<sid>/tool-results/<toolu_id\|rand>.txt` | **Large tool outputs** spilled out of the transcript | tool_use id | yes |
| `projects/<enc-cwd>/memory/` | Auto-memory notes — **not a session** | project | no |
| `projects/<enc-cwd>/agent-<7hex>.jsonl` | **Legacy** (≤ ~2.0.x) subagent files at top level (818 here); lines carry the parent sid | short agentId | yes |
| `history.jsonl` | **Every typed prompt**, globally: `{display, pastedContents, project, sessionId, timestamp}` — up-arrow recall | — (append) | **no** |
| `sessions/<pid>.json` | **Live-session presence**: `{pid, sessionId, cwd, startedAt, procStart, version, kind, entrypoint, status: busy\|idle\|waiting\|shell, waitingFor, updatedAt, statusUpdatedAt, messagingSocketPath, name, …}` — rewritten on every status transition; `sessionId` FOLLOWS `/clear`/`/resume` (a `/compact` keeps it), while `startedAt` + `procStart` (the process FILETIME, a decimal string — the exact PID-reuse guard, OBSERVER.md §8b) are written once at registration; stale after crashes | OS pid | undocumented |
| `file-history/<sid>/<hash>@v<N>` | Pre-edit checkpoint snapshots (`/rewind`) | sid | yes |
| `tasks/<sid>/<n>.json` | TaskCreate/TaskUpdate task lists | sid | yes |
| `session-env/<sid>/` | Per-session env / session-scoped hook scripts | sid | yes |
| `shell-snapshots/snapshot-bash-<epochms>-<rand>.sh` | Captured shell env for the Bash tool | epoch (NOT sid) | yes |
| `debug/<sid>.txt`, `debug/latest` | Per-session debug logs (docs say `--debug`-only; locally written for ~every session — drift) | sid | yes |
| `paste-cache/<contentHash16>.txt` | Large pasted text; referenced from `history.jsonl` `pastedContents.<n>.contentHash` (linkage verified) | content hash | yes |
| `state/title-<sid>.txt`, `state/skills-pid-<pid>.txt` | Saved terminal title; runtime pid state | sid / pid | undocumented |
| `summary-cache/<sid8>_<hash16>.txt` | Cached generated summaries (legacy-era) | sid prefix | undocumented |
| `todos/<sid>-agent-<aid>.json` | **LEGACY** task lists (aid==sid for the main thread); not written since ~Mar | sid+agentId | as legacy |
| `plans/`, `image-cache/`, `backups/`, `feedback-bundles/` | Plan-mode files; pasted images; `~/.claude.json` migration backups; `/feedback` bundles | — | yes |
| `stats-cache.json`, `settings.json`, `.credentials.json`, `.last-cleanup`, `mcp-needs-auth-cache.json` | `/usage` totals; user settings (`cleanupPeriodDays`); OAuth; last-sweep stamp; MCP re-auth | — | no |
| `daemon/`, `teams/`, `jobs/<sid8>/`, `ipc/`, `recovery/`, `statsig/`(legacy), `logs/`(legacy), `ide/`, `plugins/`, `cache/` | Agent-teams/background-jobs infra; IPC; crash recovery; legacy flags/logs; IDE locks; plugins; changelog cache | varies | varies |
| **`~/.claude.json`** *(sibling file, not inside)* | App state + **`projects.<fwd-slash-path>`** per-project: `allowedTools, mcpServers, hasTrustDialogAccepted, lastSessionId, lastCost, lastDuration, lastTotalInput/OutputTokens, …` | project path (fwd slashes) | no |

**No SQLite anywhere** (no `__store.db`) — everything is JSON/JSONL. **Path encoding is lossy**:
every non-`[A-Za-z0-9]` char → `-` (`K:\source\Agentmaster` → `K--source-Agentmaster`), so
`K--source-TraderLab-options` is ambiguous (`TraderLab-options` vs `TraderLab\options`) — ground
truth is the `cwd` **field inside the lines** (matches our `EncodeCwdToProjectDir` + its
documented drift-fallback `ExtractCwdFromTranscriptHead`).

---

## 3. Transcript JSONL schema (what the page reads)

One JSON object per line, **append-only**. Local census (Agentmaster project dir, 19,219 lines):
`assistant 10198 · user 4688 · mode 1425 · permission-mode 1409 · last-prompt 1381 · attachment
937 · file-history-snapshot 534 · system 448 · queue-operation 97 · custom-title 1 ·
agent-name 1`; rare/elsewhere: `summary` (legacy), `ai-title`, `worktree-state`, `agent-setting`,
`progress` (legacy), `fork-context-ref` (subagent files only). The **state-bearing subset** is
already documented in [`STATE.md`](./STATE.md) §2/§9 — this section covers the browse/search
fields.

- **Envelope** (user/assistant/system/attachment): `uuid`, `parentUuid` (tree edge), `sessionId`
  (== filename — verified), `timestamp` (ISO-8601 Z), `cwd`, `gitBranch`, `version`,
  `isSidechain`, `userType`, optional `isMeta`, `slug`, and on forked lines
  **`forkedFrom:{sessionId, messageUuid}`**.
- **`user`** — `message.content` is a **string** (a typed prompt; 337/4,688 in the sample) or an
  array (mostly `tool_result` blocks `{tool_use_id, content, is_error}`; rarely `text` blocks).
  `isMeta:true` = injected context; `isCompactSummary:true` = the post-compaction summary
  message. **`toolUseResult`** mirrors each tool's structured result (Bash `{stdout, stderr…}`,
  Edit `{filePath, structuredPatch…}`, …).
- **`assistant`** — `message` is the API shape `{id, model, role, content[], stop_reason, usage}`;
  block types `tool_use {id, name, input}` / `thinking {thinking}` / `text`. `usage` carries full
  token accounting. `tool_use.id` ↔ a later user line's `tool_result.tool_use_id` (oversized
  results spill to `<sid>/tool-results/<id>.txt`).
- **`system`** — `subtype` ∈ `turn_duration · away_summary · local_command · api_error ·
  compact_boundary · stop_hook_summary`.
- **`attachment`** — context injections (`task_reminder`, `edited_text_file`,
  `compact_file_reference`, `skill_listing`, `date_change`, …) — **not** user text. **One
  exception: `queued_command`.** A message the user types WHILE claude works never becomes a `user`
  line. Enter queues it (`queue-operation` enqueue); the running turn absorbs it at its next
  tool-result boundary (`queue-operation` remove, `reason:"absorbed_mid_turn"`), and claude writes
  `attachment {type:"queued_command", commandMode:"prompt", prompt, origin:{kind:"human"},
  humanTurn:true}` on the live chain, stamped with the Enter time (builds up to 2.1.179 omit
  `origin`; `prompt` is a string, or a content array when an image is pasted). The same type also
  carries machine traffic: `commandMode:"task-notification"` (a background task finished) and
  `origin.kind` `peer` (an agent or cross-session message, `isMeta`) or `task-notification`. Survey
  2026-10-03: 2,192 `queued_command` lines in 478 transcripts — 1,983 task notices, 204 human
  messages, 5 peer / task messages. The summary analyzer lists the human ones like typed prompts
  (`ProcessInspect.Content.cpp` `SeQueuedHumanPromptRawText`); this page's index
  (`ClassifyTranscriptLine`) still files every attachment as context, so the 👤 scope, the msgs
  count and the External rows' read-only plan do not see them yet.
- **Untimestamped session-state tail lines** (no uuid): `mode`, `permission-mode`,
  `last-prompt {lastPrompt, leafUuid}`, **`custom-title {customTitle}`** (from `/rename`, `-n`,
  the picker, plan-accept), **`ai-title {aiTitle}`** (async picker title),
  `agent-name`, `agent-setting`, `worktree-state`, `queue-operation {enqueue|dequeue|remove|popAll}`.
- **`summary {summary, leafUuid}`** — **legacy** (every local instance is v2.0.75–2.1.25,
  Jan–Feb 2026; zero in current files — reconciles our earlier 0/1,842 survey with community
  claims). `leafUuid` can point into **another (or a deleted) file** — cross-file and fallible.
- **Compaction never truncates**: `/compact` appends `system/compact_boundary`
  (`parentUuid: null`, `logicalParentUuid` = pre-compact tail — occasionally a message never written
  to disk — `compactMetadata {trigger, preTokens, postTokens, durationMs, preservedSegment,
  preservedMessages, messagesSummarized, userContext}`) + a `user isCompactSummary:true` line; the file
  keeps growing, same id. The boundary's null parent is why every live-branch walk must CROSS it (logical
  parent / preserved messages / the pre-boundary leaf) or it hides the whole pre-compaction
  conversation — HOOKS.md *Compaction*. A partial compaction writes its preserved tail after the summary
  (auto) or keeps the early messages and summarizes the middle ("summarize from here"); a legacy resume
  (≤ 2.1.147) re-appended the whole history (duplicate uuids — display readers keep the first).

---

## 4. The relationship graph (sessionId-centric)

```
                      ~/.claude.json ─ projects."K:/source/X".lastSessionId ──┐
history.jsonl ── {sessionId, project, display, ts, pastedContents.contentHash ┼─→ paste-cache/<hash>.txt
                                                                              ▼
projects/K--source-X/  ◄── enc(cwd)                                 ┌── <sid>.jsonl ──┐
│   lines: uuid ◄─ parentUuid (tree; branches = edits/retries)      │ (sessionId == filename)
│   assistant.tool_use.id ───────────► user.tool_result.tool_use_id │
│     │            │ (Task tool)      └ big outputs ─► <sid>/tool-results/<id>.txt
│     │            ▼
│     │   <sid>/subagents/agent-<aid>.meta.json {toolUseId == tool_use.id}
│     │   <sid>/subagents/agent-<aid>.jsonl  (lines: sessionId = PARENT sid, agentId=aid,
│     │       isSidechain:true; may open with fork-context-ref {parentSessionId,
│     │       parentLastUuid} = "my context is the parent's, by reference")
│     ├ file-history-snapshot.trackedFileBackups ─► ~/.claude/file-history/<sid>/<hash>@vN
│     ├ compact_boundary.logicalParentUuid / preservedMessages ─► in-file uuids
│     ├ last-prompt.leafUuid ─► in-file leaf uuid
│     └ (legacy) summary.leafUuid ─► uuid possibly in ANOTHER (or deleted) file
├── FORK (/branch, /rewind restore, --fork-session, in-session /resume-to-a-message):
│     <newSid>.jsonl = VERBATIM COPY of the parent's live subtree (same uuids, same
│     timestamps) + forkedFrom:{sessionId, messageUuid} on every copied line;
│     custom-title "<parent> (Branch)". Verified: 517/517 fork uuids exist in the parent.
├── side state: tasks/<sid>/ · session-env/<sid>/ · debug/<sid>.txt · state/title-<sid>.txt
│               · jobs/<sid8>/ · (legacy) todos/<sid>-agent-<aid>.json · summary-cache/<sid8>_*
└── live: ~/.claude/sessions/<pid>.json {pid ↔ sessionId ↔ cwd, status, updatedAt}
```

- **Resume** (`--resume <id>`, `--continue`, picker) **appends to the same file, same id** — two
  terminals resuming one session interleave into one transcript [docs]. No copy-on-resume in current
  builds; Claude Code ≤ 2.1.147 re-appended the loaded history verbatim on resume (6 transcripts here
  carry up to 8,240 duplicate uuids — the display readers keep each message's first occurrence).
- **Fork** = new file, full duplicate of the parent subtree from the current context root (the
  latest compact boundary); can contain **zero novel messages**.
- **`/clear`** ends the session (`SessionEnd` reason `"clear"`, then `SessionStart` source `"clear"`)
  and mints a new id in the SAME process; the old file stays resumable. The presence file follows the new
  id; the cmdline `--session-id` does not.
- **`/compact`** is **in-place**: same id, same file (`PreCompact` → `PostCompact` →
  `SessionStart` source `"compact"`) — HOOKS.md *Compaction*.
- **`/cd`** (v2.1.169+) **relocates** the transcript to the new dir's project folder — project-dir
  membership is not immutable.

---

## 5. Identity, timestamps, retention

- **Identity = the session UUID** (filename == every line's `sessionId`). Survives resume; a fork
  is a NEW id with `forkedFrom` lineage.
- **File birth time** matches the first prompt for normal sessions — but a **fork's** birth is the
  fork moment while its line timestamps are *copied* (older). So: created = first line
  `timestamp`, EXCEPT forks (detect `forkedFrom` in the head) → file birth time.
- **mtime lies, three ways** (all measured locally): (1) `away_summary` appends ~3 min after the
  turn; (2) **exit-time flush** of untimestamped tail-state lines (observed +43 min); (3) **bulk
  metadata passes** re-touching files 8–9 *days* after their last turn. Distribution over the 300
  most recent transcripts (mtime − last-message-timestamp): **median ~63 min, p90 ~43 h, max
  ~43 days**. `mtime ≥ last-activity` always holds ⇒ mtime is a safe **superset filter** and a
  correct **change detector**, never the displayed last-activity. (Same caveat STATE.md §2 hit.)
- **Retention:** the startup sweep deletes per-session artifacts older than `cleanupPeriodDays`
  (default **30 days**) — transcripts, subagents, tool-results, file-history, debug, paste-cache,
  session-env, tasks, … **`history.jsonl` and `stats-cache.json` are never swept** (they
  reference sessions that no longer exist). `claude project purge` (2.1.124+) wipes one project.

---

## 6. Implementation plan (page mechanics)

> **Status: the store API layer is IMPLEMENTED + tested** — `AgentMaster/TranscriptStore.{h,cpp}`
> (plain C++, standalone-harness covered; survey-grounded against the live 92-day corpus of 499
> transcripts, versions 2.1.76→2.1.170, with old-strata tolerance back to 2.0.x):
> **`EnumerateTranscripts(In)(sinceUnixMs)`** (uuid-stem filter — legacy `agent-*` / `memory/` /
> `<sid>/` subdirs excluded; mtime superset window; newest first) · **`ScanTranscript`** (the
> byte-offset-resumable line stream — the incremental indexer; >1 MiB lines + corrupt-run guard)
> · **`AccumulateTranscriptStats`** (the resume-cursor stats fold: REAL-prompt / assistant / tool
> counts, title set, first/last **message** timestamps, fork lineage, line-`cwd`, shrink ⇒
> auto-rebuild) · **`ReadTranscriptQuickFacts`** (head + growing-tail windows: fork-aware
> `created`, state-line/`away_summary`-proof `lastActivity`) · **`ClassifyTranscriptLine`** (the
> pure per-line kind/text extractor behind the 👤/🤖 search scopes, caps enforced) ·
> **`IsNoiseUserPrompt`** + **`PickDisplayTitle`** (the §6a rules, shared engine-wide — also now
> applied inside `ReadTranscriptInfo`, which gained `aiTitle`/legacy `summary` +
> `TranscriptDisplayTitle`). Remaining for the page: the cache files themselves (Q2) + the UI.

1. **Enumeration.** Glob `projects/*/<36-char-uuid>.jsonl` (the UUID-name filter automatically
   excludes legacy `agent-*.jsonl`, `memory/`, and the `<sid>/` subdirs). Full stat of all 2,635
   files: **0.143 s** — enumerate-everything-then-filter is fine even on this extreme corpus
   (default machines hold ≤30 days). Window filter: `mtime ≥ cutoff` first (superset), then
   refine by real last-activity from the cache/tail.
2. **Last-activity + cache invalidation.** Two keys, deliberately different:
   - *Invalidation key* = **`(size, mtime)`** — append-only means change ⇒ growth. The cache
     stores the **byte offset indexed so far** and incrementally indexes only the appended
     suffix (the `SessionScanner` tail-cursor pattern); `size` shrank ⇒ replaced ⇒ full rebuild.
   - *Displayed last-activity* = the last `user`/`assistant` line's `timestamp` (skip
     `away_summary`): read a 64 KB tail window (~70 ms on a 10 MB file), scan backwards, grow
     ×4 on miss (a giant trailing tool_result can exceed 64 KB).
   - Cache lives in `%USERPROFILE%\.agentmaster\sessions-index\<sid>.json` (sid-keyed, NOT
     path-keyed — `/cd` can relocate the transcript).
3. **Title precedence:** `customTitle` > `aiTitle` > legacy `summary.summary` > first non-meta
   user prompt (skip `<command-name>`/caveat-prefixed lines), `last-prompt.lastPrompt` as a
   secondary. Title lines are rare (6 files each corpus-wide) and **untimestamped** — take the
   LAST occurrence; a raw substring scan (`"type":"custom-title"`) is the robust fallback.
   Extend `ReadTranscriptInfo` (it already does custom-title > first prompt) with `ai-title`.
4. **The two search scopes:**
   - **👤 user** = `type=="user" && !isMeta && !isCompactSummary && content is string/text-blocks`
     (`promptSource:"typed"` corroborates). **Accelerator:** `history.jsonl` is a prebuilt global
     user-scope index (`{display, sessionId, project, timestamp}` per typed prompt) — near-free
     first pass; caveats: ~166 ancient lines lack `sessionId`, it's never pruned (references
     swept sessions), paste bodies live in `paste-cache/<contentHash>.txt`.
   - **🤖 agent+tools** = assistant `text`+`thinking` blocks, `tool_use.input`,
     `tool_result.content`, `toolUseResult.{stdout,stderr,content,…}`, `system.content`,
     attachment payloads — plus `<sid>/tool-results/*.txt` and `<sid>/subagents/agent-*.jsonl`
     (attributed to the parent via their `sessionId` field). ~99 % of the bytes — cap indexed
     text per line, index incrementally, never parse whole files on the UI thread.
5. **Window selector `[1 month]`:** the click-cycle is the `_CycleTreeSort` toggle precedent; the
   hover calendar is the path-picker `Primitives::Popup` precedent (root-parented offsets;
   deferred dismissal — the XAML-Islands focus/hover traps apply). Persist the chosen window in
   `AppSettings` via the existing settings sink (like `treeSort`).
6. **Row affordances by relationship to the app** (correlate each listed sid against the
   registry):
   - **OPEN** (live, `SessionRegistry::Get(sid)->live`) — a **per-dir tab color** chip
     (`dir-colors.json` palette via the existing dir-color system; *as shipped*, solid when OPEN
     vs dim for an on-disk row, with a brighter **presence ring** + tooltip when claude's
     heartbeat reports busy/idle/waiting — §7-Q5) + the timing adornment + **Jump** (Activate,
     Rule #2: focus only, never inject).
   - **ARCHIVED** (in `sessions.json`, `!live`) — **Restore here** (the existing
     transcript-gated `claude --resume` seam).
   - **On-disk only** (not in the registry at all) — **Open / Adopt**: resume it into a managed
     tab (`_AdoptExternalClaude`-style, transcript-gated) + **Open New Session Here**.
   - A session live in a FOREIGN host (its sid in `sessions/<pid>.json` but not ours) — mark
     "live elsewhere"; adopting it means two writers on one transcript (the known adopt caveat).
   - *As shipped*, every row's right-click menu carries the relationship-appropriate primary
     action (**Jump to tab** when OPEN, else **Resume here**) PLUS **Fork here** on EVERY row —
     including a live one, since a fork writes its OWN transcript so the two-writers hazard
     doesn't apply (`_ForkSessionFromDisk`) — **Edit Title** (rename in place → the durable
     SessionStore title; also the **slow-double-click** gesture — see "Edit title" in §1),
     **Open New Session Here**, a **Filter ▸** submenu
     (By Same Directory / Branch / Day / Week / Month / Fork Family — the AND-stacking browse facets,
     §1), and **Hide from list**
     (§1). Resume / Fork / Open-New open in a BACKGROUND tab so the list stays up for bulk-open.
     **Resume / Fork open EXACTLY the picked id — no continuation-tail redirect.** The former
     `ResolveContinuationTailOnDisk` "hop forward to the newest same-cwd link" was **removed**: it was
     pure-timing (no solid signal — `/compact` is in-place, `/clear` leaves no successor link, a
     plan-restart references its parent backward), so it chained the *next independent session* in a busy
     dir onto the prior one (proven on the real corpus: 21/198 redirects, 5 targets each "continuing" 2–4
     unrelated predecessors). Picking a row now resumes/forks THAT conversation. (The SOLID links — a
     plan-restart's explicit `"read the full transcript at: <parent>.jsonl"` reference, a fork's copied
     lines, and the `/clear` + `/handover` links Agentmaster records — are followed only BACKWARD, by the
     summary panel's whole-conversation view: TAB_OVERLAY.md §13e.)
     **Double-click** an OPEN row jumps to its tab (`_ActivateClaudeSession`); on any other row it
     resumes (`_ResumeSessionFromDisk`).
7. **Fork grouping:** dedupe search hits by line `uuid` (forks duplicate content verbatim);
   group rows by root via `forkedFrom.sessionId` chains.
8. **Reuse, don't re-build:** `ClaudeProjectsDir`, `EncodeCwdToProjectDir`,
   `ExtractCwdFromTranscriptHead`, `TranscriptTimes(In)`, `ReadTranscriptInfo(In)`,
   `ParseTranscriptDelta` (per-line parse rules), `PathEq`/`NormPath` (group by the line `cwd`
   field, NEVER the lossy folder name), `FormatSessionTiming`, the Archive page shell + its
   deferred-pointer-handler discipline, and the settings sink.
9. **Pitfalls checklist:** huge files (10 MB+ seen) ⇒ incremental only; ENOENT mid-listing (the
   sweep runs at any `claude` startup) ⇒ tolerate-and-drop; >1-month windows on default machines
   are mostly empty (`cleanupPeriodDays` 30) ⇒ consider a hint when the window exceeds the
   user's `cleanupPeriodDays`; the `sessions/<pid>.json` presence files go stale after crashes ⇒
   pair with pid+start-time liveness (the observer already does exactly this).

---

## 6a. Prior art — the user's own session tooling (dug from `~/.claude`)

The user already built a CLI version of this page, an extraction engine, and a SessionEnd exit
hook — outdated in places, but they encode **proven-useful display fields** (what he actually
looks at, post-session). Key files: `hooks/session-end.js` (the live exit hook; `.sh`
predecessor kept), `bash-ext/sessioninfo.sh` (`si` — the extraction engine, `--json` API),
`bash-ext/clone.sh` (`cr`/`cf`/`rs` — resume / manual fork / **the CLI Sessions browser**),
`scripts/session-end-autocommit.ps1`, `scripts/restart-discover.ps1` (UIA tab↔session
correlation — superseded in-process by the Fleet Observer), `conversationlog.md` (the
append-only cross-session log the hook maintains), `state/title-<sid>.txt` (his per-session
saved titles), `summary-cache/` (AI summaries, file-size-keyed invalidation).

**What the exit hook prints on claude exit** (SessionEnd → stderr + exit 2, so it rides the
"hook failed:" display path) — the proven summary set:

```
 Session:  <sid> [🚪 exit | 🚀 plan-start | 📋 plan-end | 🧹 clear]
 Parent:   <parent sid>                (plan-start: parsed from the first message's
 Plan:     <plan file written/read>     "read the full transcript at: …jsonl" ref)
 Dir / Folder / Resume:  <cwd> · <enc folder> · claude --resume <sid>
 Duration: 25m 42s (15:32 -> 15:57)    Branch: <gitBranch>    Tasks: 3 completed / 1 pending
 Messages: 1..N   (filtered, deduped, truncated user prompts)
 Files Read: …    Files Edited: …      (basenames; the rs browser renders dir-grouped trees)
```

The `si --json` schema adds what the JS rewrite dropped: **`commits[{hash,message}]`** (a plain
`git log --after=<start> --before=<end>` in the session's cwd — zero transcript parsing),
`tool_count`, `size_kb`, `msg_count`.

**Distilled for this page** (column ▸ detail-pane candidates, in his order of proven value):
1. **Title, layered** — saved user title > AI summary (cached) > first prompt; the detail pane's
   core artifact is the **numbered, filtered user-prompt list** (every tool of his shows it).
2. **Session type + lineage** — exit / clear / plan-start / plan-end / *unexpected-exit* (= a
   transcript with no clean end — our registry knows clean archives, so
   transcript-without-archive ⇒ crashed/external), with **plan-mode parent→child chains**.
3. **Timing** — duration + `(HH:MM → HH:MM)`, **date-suffixed when multi-day** (his `rs` fix
   that the hook lacks; a left-open tab otherwise reads `94h 39m`).
4. **Weight row** — `N msgs, M tools, K kb` (his standard one-liner; his `-m`/`--min-tools`
   filters prove counts are how he separates real work from noise).
5. **Files** — read vs edited; basenames in rows, **directory-grouped trees** in the detail.
6. **Tasks** — last-TodoWrite completed/pending = a "did it finish" indicator.
7. **Git** — branch + **commits made during the session window** (his autocommit stamps
   `{session:short}` into commit messages, so commits→session reverse lookup exists too).
8. **Resume everywhere** — every view prints `claude --resume <sid>`; pickers bind Enter=resume,
   Ctrl-Y=copy-id ⇒ rows need one-click Restore/Adopt + copy-id.
9. **His `rs` filter set == our search bar** — time window (6h/2d/1w…), dir glob, full-text over
   prompts, min-msgs/min-tools, multi-select bulk open (`rs 1 3 5` / `all`).

**Noise-suppression rules (adopt verbatim, §6.3/§6.4):** hide `agent-*` transcripts, 0-message
sessions, `/clear`-only sessions, `queue-operation`-first task files; filter prompts matching
`<command-message>`/`<command-name>`/`<local-command-`/`<bash-input|stdout|stderr>`/`^Caveat:`/
`[Request interrupted` — **plus `<task-notification>`, `<system-reminder`, `<teammate-message`**
(post-hook schema additions his filter misses — the shipped `IsNoiseUserPrompt` catches all three;
today's log shows raw task-notification blobs as messages); dedupe repeated prompts;
suppress `Branch: HEAD` (detached); harvest `file-history-snapshot.trackedFileBackups` as an
extra edited-files source (his `si` does, the JS hook doesn't).

**Gaps his tooling never solved that this page owns for free:** model/effort (Fleet Observer
facts), **token usage / cost** (`message.usage` is in every assistant line, never tapped), live
state (his is all post-mortem; we have the registry + observer), per-tab titles without the
UIA/`Console.Title`-timer fights (`Tab::SetTabText` pinning, Rule #11), and principled
tab↔session correlation (`WT_SESSION` roster vs his creation-time-adjacency heuristics).

---

## 7. Open questions — ANSWERED (user decisions; all implemented)

1. **Both toggles OFF** ⇒ title+dir-only search — **confirmed**; plus three more toggles: `📁`
   (directories accessed — any tool call to a path), `📄` (files accessed), `(F)` (fuzzy).
2. **Cache store** ⇒ sidecars (`sessions-index/<sid>.json`) AND a **two-phase search**: the fast
   phase first (index + `history.jsonl`), then a separate slower content pass — built on
   **ripgrep** (PATH-resolved, in-process fallback). Case-insensitive by default; fuzzy via the
   rg regex (`a.*?b.*?c`).
3. **Hit granularity** ⇒ (delegated) rows + per-row hit counts, with the selected row's detail
   pane showing the matched-message **snippets** (scope-tagged) and the numbered prompt list;
   a full read-only conversation click-through stays a later increment.
4. **Calendar popup** ⇒ a **real From/To range picker** — plain text boxes with placeholder
   dates (no islands-fragile calendar control); the click-cycle presets remain the shortcuts.
5. **Presence** ⇒ **proper integration with separation**: the `TranscriptStore` owns the raw
   `sessions/<pid>.json` read (claude-session domain logic, reusable); everything live/changing
   is published **via the observer** — a pid-validated `Presence()` table each full survey, plus
   the per-session `presenceStatus` enrichment through `ObserveClaude` (a display FACT, never
   `SessionState` — Rule #13; STATE.md owns any future promotion).
6. **`history.jsonl` accelerator** ⇒ yes, via **ripgrep** (rg filters the global prompt log;
   every candidate line is re-verified by the in-process matcher; full in-process scan when rg
   is absent).

---

## 8. Sources

- [Explore the .claude directory](https://code.claude.com/docs/en/claude-directory) [docs] — the
  application-data layout, cleanup, purge.
- [Manage sessions](https://code.claude.com/docs/en/sessions) [docs] — resume/continue/fork,
  picker, naming, the two-terminals-interleave warning.
- [Checkpointing](https://code.claude.com/docs/en/checkpointing) [docs] — rewind/restore,
  file-history.
- [Settings](https://code.claude.com/docs/en/settings) [docs] — `cleanupPeriodDays`,
  `~/.claude.json`.
- [Hooks](https://code.claude.com/docs/en/hooks) [docs] — `session_id`/`transcript_path`,
  SessionStart sources, SessionEnd reasons.
- [samkeen — claude-code-data-structures gist](https://gist.github.com/samkeen/dc6a9771a78d1ecee7eb9ec1307f1b52)
  [community] — the ≤2.0.x legacy strata (top-level `agent-*.jsonl`, `todos/`).
- [claude-code-log](https://github.com/daaain/claude-code-log) · [claude-dev.tools JSONL notes](https://claude-dev.tools/docs/jsonl-format)
  [community] — envelope; cross-file summary matching.
- [ccusage](https://github.com/ryoppippi/ccusage) [community] — also scans `~/.config/claude/projects/`;
  dedupes by message id + request id.
- [anthropics/claude-code#52411](https://github.com/anthropics/claude-code/issues/52411)
  [community] — `ai-title` in the picker.
- Everything else: **[local-verified]** against `C:\Users\ELI\.claude` (CC 2.1.170, 2,635
  transcripts), read-only. Key verifications: fork-copy uuid identity (517/517), Task
  `toolUseId` ↔ `meta.json`, `paste-cache` contentHash linkage, `compact_boundary` metadata,
  the three mtime-drift modes, the sweep config, **no SQLite**, the `summary`-line version
  stratum (2.0.75–2.1.25 only), `sessions/<pid>.json` shape, `history.jsonl` shape.

## 9. The AI search

> Status: written 2026-10-10. Built on 2026-10-11 and **published in the v0.7.9 pre-release** (the
> tag is at `b842fcf5a`). No build with it is installed on the development machine, and the page
> itself was never run on screen by its authors. The engine is tested live against the real
> `claude.exe`. What 2026-10-12 changed (bypass permissions, two tools instead of five in the
> model's list, the "(now)" text) is in no build yet. The engine under it is the agent harness:
> [`SDK_HARNESS.md`](./SDK_HARNESS.md).

### 9.1 What the user sees

- An **AI** toggle **inside the search box**, at its right end, like the regex toggle of an
  editor's find box: flat while it is off, accent-filled while it is on. With it on, the box is a
  **request line**: write what you remember ("the session where I fixed the tab drag crash") and
  press **Enter**.
- A **strip** under the header shows the request, what happens now ("Searching: …"), then the
  model's one-sentence answer, and the size of the conversation ("ctx 12K / 500K").
- The **table shows exactly the sessions that the model picked**, best match first. The first
  row is selected, so its detail pane is there without a click.
- A **follow-up refines the list** ("only the open ones", "older than that"): the conversation
  keeps its context.
- The **crossed circle** (⊘) right of the box throws the conversation away. The model forgets
  everything, and the normal list comes back. It shows only while the AI search is on.
- **Escape** in the box stops a request that runs. A new request also stops the one before it
  (the newest wins).
- Typing filters nothing in AI mode. Turning the toggle off brings back the filter box and the
  normal list; the conversation is kept.

### 9.2 One conversation for the whole process

There is **one** search conversation, in every window (`SharedSessionSearchAgent`,
`AgentMaster/SessionSearchAgent.{h,cpp}`). Each window's Sessions page is a view of it: a request
from one window shows its list in every page that has the AI search on. This is the engine's rule
(one fleet, many lenses), and it is what "one conversation at a time" means here.

The model and its settings are fixed: **Sonnet 5.5, thinking off, low effort**, no built-in tools,
nothing written under `~/.claude` (`--no-session-persistence`, so the search conversation is never
one of the sessions it searches), auto-compaction at 500,000 tokens, and **bypass permissions**
(the harness's rule for every conversation; auto mode when Claude Code does not allow bypass —
`SDK_HARNESS.md` 5.1). The model has exactly two tools (9.3), so bypass refuses nothing that it
could otherwise do: the tool list is the limit.

### 9.3 Two tool servers, and the system prompt

The model can do two things (`AgentMaster/SessionSearchTools.{h,cpp}`):

| Tool | What it does |
|---|---|
| `mcp__sessions__search` | Runs the page's own search over the session index: every word must match a title, a folder, a branch, a touched file or a typed prompt. Filters: `days` (default 90), `after`, `before`, `dir`, `branch`, `tag`, `favorite`, `open`; `deep` also reads the conversations' text; `sort`, `limit`. It changes nothing on the screen. |
| `mcp__display__show` | Puts sessions on the screen, in the model's order: `ids` from search results, or `top: N` for the first N rows of the latest search. An empty list means "nothing fits". |

The split is the point. The search tool never changes what the user sees, so the model can look
around freely. The show tool is the one place where the screen changes.

- **Short ids.** The model reads and writes the first 8 characters of a session id. It must write
  every id that it wants shown, and output tokens are what a search waits for. A second session
  with the same first 8 characters gets a longer prefix.
- **The result text** is compact JSON: the window, the counts, and one object per row (id, title,
  folder, branch, times, sizes, why it matched). Flags and lists appear only when they say
  something.
- **The system prompt** has eight rules. The first is new (the user's request of 2026-10-13, in no
  build yet; production 0.6.8.19 has the seven rules without it):
  - The model's first step, for every request, starts with a mention, in one or two short lines,
    of which kinds of tool calls it can use for the request, out of everything its two tools can do
    (search by words; by folder, branch, tag, favorite or open; by a period; the full text; sort and
    limit; show by ids or `top`), and how it will use them in different ways (the words it will
    try, the filters, a longer period, `deep` last).
  - It is written in the same message as the step's first calls, so it costs output tokens, never
    a model request (measured in 9.9).
  - **The page does not show it.** The strip shows the turn's final sentence: Claude Code's
    `result` line carries only the last message's text (checked live). The mention stays in the
    conversation record (`harness\sessions-search.json`) and in the live test's trace.

  The other seven: act at once and never ask; one search is usually enough (1 to 3 distinctive
  words, no `days` unless the user names a period); show only what fits; exactly one sentence under
  25 words in the user's language, with an example of one, and the mention is not the reply; a
  follow-up refers to the shown list; times are local; a result with an `indexing` field lacks
  older sessions.
- **The redesign** (2026-10-13, DESIGN ONLY): [`SESSIONS_AGENT.md`](SESSIONS_AGENT.md) — eleven tools
  (`search` with phrasings, scopes and folder exclusion, `grep`, `peek`, `read` at the transcript
  levels, `files`, `related`, `timeline`, `usage`, `sql`, `show` with reasons, `reveal`), a context
  line and an instant pass on every request, secret redaction, and a new system prompt; worked out on
  the first real request that failed (52.6 s, a session from the excluded project).

### 9.4 The session index, and why a search never waits for it

A search runs over a **catalog**: one immutable snapshot of the sessions' index entries (the same
sidecar files the page itself reads, section 6), with the stored titles, tags, favorites and the
open sessions.

Reading it can be slow. Measured on this machine (1,506 sessions in 90 days, an optimized build):

| Case | Time |
|---|---|
| A sidecar that Windows has cached | 0.3 ms per session; 90 days in 0.5 s |
| The first touch of a sidecar file (an on-access scan) | 10 to 20 ms; 90 days in 15 to 30 s |
| A transcript that has no sidecar yet (parsed whole) | seconds for a large one; a new profile needs minutes |
| A refresh of a complete catalog | 0.1 s |

So the search tool never reads the disk. The agent has an **index keeper**: a background task that
reads in **steps**. Each step takes everything the step before had (no file read), reads new
sidecar files for 2.5 s (400 ms in the first step), newest sessions first, and publishes the
result. A search takes the newest published catalog.

- A catalog that stopped early says how far back it reaches (`coveredSinceUnixMs`). When that is
  not as far as the search asked, the result has an **`indexing`** field, and the model tells the
  user that older sessions are still being indexed.
- A search waits at most 2 s for a catalog that covers its period, and only the first search of a
  request waits.
- **The session store is not waited for either.** The stored titles, the tags and the favorites
  come from `session-store\`, whose first read of a process opens every record file (16 s were
  measured for 2,181 files that Windows had not cached; this read, not the sidecar files, once
  made a first search wait 15 s). A step that finds the store unread publishes without it
  (`storeOverlay` false), the keeper reads the store after that publish, and the next step has
  it. A search that filters by tag or favorite over such a catalog says so in `indexing`. In the
  app the engine reads the store at start-up, so this matters only in the first seconds of a run.
- **A search can be cancelled.** A tool handler runs on the SDK's reader thread; while it runs,
  that thread cannot deliver the interrupt of its own request. So the handler asks, before each
  slow step, whether its request is still the newest one (a generation number). A cancelled search
  returns at once and leaves no trace.

### 9.5 The page

`TerminalPage.AgentSessionsAi.cpp` is the view of one window. It hooks the page at five places:
the header build, the open and close seams, the gather's end, and the render.

- **The list replaces the view.** `_RenderSessionsTable` is the page's one render chokepoint.
  While an AI list is active, `_SessionsAiApplyView` replaces the rows that the filters chose with
  the listed sessions, in the model's order. The Open / Favorite / Hidden / row filters do not
  apply to it: the model chose these sessions for the request.
- **A column click sorts the list.** The list keeps the model's order while the sort is the one
  that was set when the list arrived.
- **A preview.** A search's rows are shown at once, before the model picked from them (the count
  line says "choosing…"). The model's pick replaces them about a second later. An empty search
  result is not previewed.
- **Rows outside the page's window.** The page lists one month by default; a search covers three.
  A listed session that the page did not gather gets a real row (`aiExtra`), appended to
  `_sessionsRows` and `_sessionsEntries`, so the detail pane, resume, fork, rename, favorite and
  tags work on it. The normal list skips such rows, and they are put back after every gather.
- **Events** come on the agent's threads. The listener only posts them to the window's dispatcher;
  it takes no strong reference to the page there.
- **The toggle is inside the box, but it is not a part of the `TextBox`.** It is a sibling in the
  same grid cell, appended after the box, so it is drawn over the box's right end and takes the
  pointer there. The box keeps its own chrome (border, hover, the focus underline) around both.
  `_SessionsAiFitToggleIntoBox` makes the room, each step once: the box's right padding grows by
  36 DIP (the 30 DIP toggle, its 4 DIP inset and a 2 DIP gap), and the box's own clear button
  (the template part `DeleteButton`) is shifted left by the same amount with a render transform,
  so it reads text | ✕ | AI. The toggle's flat look is two local values (a transparent fill, no
  border): only the rest state uses them, so the stock hover fill and the accent fill of the
  checked state still show.
  - ⚠ What this rests on: the WinUI 2.8.4 package's `Generic.xaml` holds the text box template of
    NumberBox as text (a copy of the default one with spin buttons). There the clear button is the
    part `DeleteButton`, in the last column, collapsed until the box has focus and text, and not
    deferred. The DEFAULT text box template is compiled into the package and was not read, and
    nothing of this was seen on screen. When a template has no such part, the ✕ shows under the
    toggle (a cosmetic fault; the toggle takes the click), and the code tries again at every focus
    and text change.

### 9.6 Memory, and when the process ends

The idle `claude.exe` holds about 400 MB, and the catalog tens of megabytes. A process-wide tally
counts the pages that show the AI search (the mode is on and the page is on screen). When the last
one leaves (the toggle goes off, the page closes or is covered by a tab, the window closes), the
agent **frees its catalog** at once and **parks its process** 10 minutes later. A page that comes
back sooner finds the same process. After a park, the conversation continues from a digest of its
record.

The hidden process is not a session: the Fleet Observer and the CLI skip it
(`SDK_HARNESS.md` section 5).

### 9.7 What is kept across a restart

In `<profile>\harness\`: `sessions-search.json` (the conversation's record) and
`sessions-search.view.json` (the request, the answer, and the ids of the shown list). A reset
deletes both. The toggle's state is not kept: the AI search is off when a window opens.

### 9.8 Tests

`tests\tests_harness.cpp` (m5, no model): the arguments (defaults, clamps, the errors written for
the model), one search over a hand-built catalog with an injected content search (the scope, the
four match tiers, the sorts, the limit, deep search only on request, a cancelled search), the
result text (row fields, the `indexing` note), the two tool handlers (short ids and collisions,
show by ids / top / nothing, unknown ids, a cancelled request, no index at all), the catalog
gather on scratch folders (a gather that stops early says how far it got; steps always move
forward; a refresh reads only what changed), and the system prompt (ASCII, the two tool names,
rule 1 with every kind of call in it, eight rules).

### 9.9 Measured, end to end

`harness_live.exe --agent "<request>"`, on this machine's real sessions (1,506 in 90 days), Claude
Code 2.1.291, 2026-10-10, two runs with different requests, each from a new process with no
prewarm and with the index files in the Windows file cache:

| | Time from the request |
|---|---|
| Process start (no prewarm) | 0.7 to 0.9 s |
| Two searches, run in one model step | at 2.0 s; 0.1 s each |
| The list on the screen | 3.2 to 3.3 s |
| The sentence | 4.4 to 4.9 s |
| A follow-up ("only the three newest of those"): the list | 2.1 to 2.5 s |

With a prewarmed process, the first request saves the process start. On a new profile (no sidecar
file at all) the first search answered after 2 s of waiting, with the sessions indexed so far, and
the model said that older sessions were still being indexed.

A model that finds nothing at once tries again (other words, then a longer period, then the full
text). One such request took 43 s with five searches: 15 s of it were the session-store read that
is now out of a search's path (9.4), and 15 s were one full-text scan of 365 days. The list is
shown at the end of such a request; the strip shows each search while it runs, and a search that
found rows previews them.

**Two runs of 2026-10-12** (bypass permissions, two tools instead of five in the model's list;
1,447 sessions in 90 days), each about this document's own work:

| Request | The model's searches | The list | The sentence |
|---|---|---|---|
| "find the session where we built the agent harness on the claude-agent-sdk-cpp submodule and the AI search for the Sessions page" | `claude-agent-sdk-cpp` (2 rows), `harness` (72 matches, 15 rows) | 2 sessions at 4.0 s, the right one first | at 5.5 s |
| "where did I ask for the harness to always run in bypass permissions or auto mode" | `bypass permissions` (2 rows: a match on the typed prompt), `auto mode` (49 matches) | 1 session at 3.2 s, the right one | at 4.2 s |

`harness_live.exe --agent "<request>"` prints such a run step by step (`SDK_HARNESS.md` section 8).
Since 2026-10-13 it also checks rule 1 (a mention before the show call, and a page answer without
it) and prints when the first tool call came and how many sentences and words the answer has.

**Rule 1, measured (2026-10-13)**: seven runs from a new process each (1,464 sessions in 90 days),
each a request and the follow-up "only the three newest of those". The requests: "find the session
where we built the agent harness on the claude-agent-sdk-cpp submodule" (three runs), "what did we
change about the tab strip last week" (two), "the session about the taskbar unread badge", and "the
session where I recovered lost code with git, in a project other than NumSharp" (the request that
failed on 2026-10-12, `SESSIONS_AGENT.md` §1.2). The column "before" is the two runs of 2026-10-12
above.

| | Before | With rule 1 |
|---|---|---|
| Requests that started with the mention | — | 14 of 14 |
| The first tool call of a first request (from the request reaching the process) | 1.3 and 1.5 s | 1.6 to 2.1 s (median 1.85 s) |
| The list on the screen, first request (process start included) | 3.2 and 4.0 s | 3.9 to 4.8 s; one at 8.3 s |
| The follow-up made a new search | 2 of 2 (and widened the list, 9.11) | 0 of 6 that had a list on the screen: show by ids from the earlier results |
| The follow-up's list (from the request reaching the process) | 3.6 and 4.3 s | 2.4 to 3.4 s |
| Answers of one sentence | 4 of 4 | 0 of 2 with the old reply rule; 2 of 5 with "exactly ONE … not your reply"; 5 of 6 with the example |

- The mention: "I'll search by words (the "claude-agent-sdk-cpp" name and "harness"), and fall back
  to a longer period or deep:true if needed."; for a follow-up: "I can reuse the earlier results,
  with no new search: I'll show the three by ids, newest first."
- **It costs about 0.4–0.5 s** on a first request (the mention's output tokens). A follow-up that
  only narrows the list came sooner than on 2026-10-12, because the model did not search again; the
  follow-ups of 2026-10-10 took 2.1 to 2.5 s (the table above).
- **The answers grew.** With rule 1 and the old reply rule both answers had two or three sentences.
  The reply rule now says "exactly ONE short sentence", "The mention of rule 1 is not your reply: no
  second sentence", and shows one answer as an example. The one answer of two sentences left
  answered a question about content ("I only saw the session titles and snippets, so I can't say
  what changed in the code."); one answer of one sentence had 26 words.
- **The mention does not bind the model.** In one run it named a folder search that the model then
  did not make.
- **The request of 2026-10-12 still fails.** Three quick searches found one unrelated session; the
  model's second step searched `recover` with `deep: true` and `days: 365`, which ran past the live
  test's 120 s limit. The shipped search cannot exclude a project; the redesign answers this
  request (`SESSIONS_AGENT.md` §9.1). The superseded search then held up the follow-up (9.11).
- Seven runs is a small set: the evaluation of the redesign (`SESSIONS_AGENT.md` §11) measures
  such effects with a set of requests.

### 9.10 Logs

- `[nav] sessions ai on | off | ai-ask turn=N chars=M | ai-stop | ai-reset` (hooks.log): what the
  user did. The request's words are in the agent's line, not in the nav trail.
- `[ai-search] ask[N]: … | shown N session(s) | reset | the session index is being built … | the
  session index is complete: …` (hooks.log).
- `[harness] sessions-search: process up … | permission mode bypassPermissions (at start) | init: …
  permissionMode=… tools=2 […] servers: … commands=0 skills=0 agents=5 plugins=2 … | turn N done: …
  | parked … | reset …`. A process that could not get bypass says why in its `permission mode`
  line (`SDK_HARNESS.md` 5.1).
- `[observer] skipping harness claude (sessions-search, not a session) pid=…`, once per process.

### 9.11 Limits and follow-ups

- **The page was not run by its authors.** The engine is tested live; the XAML code compiles and
  links (the v0.7.9 pre-release holds it), but nobody looked at it on screen before that release.
- **The toggle is not remembered** across windows or restarts.
- **A cold index is slow** (9.4). Three ways to make it faster, none built:
  - a saved snapshot of the catalog, so the first read of a run is one file instead of one per
    session;
  - reading the sidecar files on a few threads (a transcript parse is CPU work per file; whether
    the first-touch scan runs in parallel was not measured);
  - serving a STALE sidecar entry at once and refreshing it in a later step. The newest sessions
    are the live ones, so their sidecar files are stale by construction, and the first step reads
    exactly those. A stale entry has the right title, folder and paths; only its last-activity
    time is old.
- **After a restart, a restored list can lack rows**: the rows outside the page's window need the
  agent's catalog, which is read again only at the next request.
- **The header shows the column sort's arrow** even while the model's order applies.
- **Sessions that the user hid are shown** when the model lists them.
- **A follow-up can widen the list instead of narrowing it** (seen twice, 2026-10-12). After a
  list of one or two sessions, "only the three newest of those" made the model search again
  (`sort: active, limit: 3`) and show three sessions, with sessions that were not on the screen
  before. It said so in its sentence. Rule 6 of the system prompt (rule 5 before 2026-10-13: "a
  follow-up refers to what you showed before") does not forbid it. Not changed: the wording of the
  rules is for the review. With the first-step rule, six runs of six that had a list on the screen
  answered the same follow-up from the earlier results, with no new search (9.9); one of them added
  a session of the earlier search to a list of two.
- **A superseded request's queued search runs in full** (seen 2026-10-13, NOT FIXED). The search
  handler takes the request generation when it starts (`SessionSearchTools.cpp`, `startGeneration`).
  A call that the SDK queued behind a slow call of the same step starts only after the next request
  raised the number, reads the new one, and is never cancelled. Measured: a full-text scan of 365
  days ran 29 s after the follow-up was sent, and the follow-up reached the process only after it.
  The fix and the reason are in `SESSIONS_AGENT.md` §7.6.
- **One live compaction of the search conversation was not run** (`SDK_HARNESS.md` section 11).

---

## 10. The folder filter

> Written 2026-10-12, **NOT BUILT** (checked with `cl /Zs` against the last Debug build and the m5
> harness; never run on screen). The user's request: "add a folder filter dropdown which pops up a
> dropdown menu in the sessions search as a filter with OR between each path. The paths are a list
> split to two 50%/50%, first is the recent sessions workdir/inferred workdir and second is the most
> commonly used from all sessions. We need the user to be able to add a folder/s that sessions were
> working in (workdir/inferred workdir)."

### 10.1 What the user sees

- **`📂 Folders ▾`** in the filters row, after **Favorite** and before the window button. It wears the
  checked accent while a folder is picked; its label then names the one picked folder (a long name
  ends in `…`) or says `N folders`.
- A click opens a **dropdown** under it (`_OpenSessionsFoldersPopup`):
  - a **find box**: every word must appear in a folder's path (case- and slash-insensitive). **Enter**
    (or **+**) adds the top match of the RECENT half; a typed **absolute path** (`K:\x`,
    `\\server\share\x`) adds that folder itself, in the sessions' own spelling when one of them uses it;
  - the **list, split 50% / 50%**: **RECENT** on top (the folder whose newest session was active last
    comes first; the row shows that age) and **MOST USED** below (the folder more sessions worked in
    comes first; the row shows the count). Two equal Star rows make the split exactly half and half;
    each half scrolls on its own. Both halves hold the same folders in two orders;
  - one **hint** line (what Enter adds, why a list is empty, "still finding where some sessions
    worked…"), the **picked folders as chips** (`name ✕` removes one — also a picked folder that no
    list shows), **Include subfolders**, and **Clear**.
- A click on a folder row picks it or removes it; the dropdown stays open for the next pick, and the
  rows are re-marked in place (a keyboard user keeps the focused row). **The picked folders combine
  with OR** — a session in ANY of them is listed — and the folder filter as a whole **ANDs** with the
  search, the toggles and the other facets (`_SessionsRowPassesRowFilter`).
- The `✕ filter` chip reads `folder A or B` (plus `(+ subfolders)`), and the count line notes
  `· filtered`. The chip's click and the row menu's "Clear filters" drop the folders with every other
  facet; **Include subfolders** keeps its setting (a preference, not a pick).
- A row's tooltip and the detail pane say **`Works in → <folder> (inferred from its tool calls)`** when a
  session's inferred folder differs from its launch folder.
- Keys: Escape closes (the focus returns to the search box), Tab cycles inside the card, the arrow keys
  move between its controls (XY focus). The page's Up/Down row navigation stands down while the
  dropdown is open.
- Closed by a press outside the card (one `handledEventsToo` hook on the page host — the Tags panel's
  recipe), Escape, the toggle, the page's Back and a tab switch (the overlay's dismiss hook). Nothing is
  persisted: like every facet it resets at restart.
- Tooltips inside the card render in the page's description panel (the card is a LocalTooltip scope of
  the page); in the cramped fallback the floating tips wait 3 s, the Tags panel's value.

### 10.2 Which folders, and how they are counted

- A session **worked in** its **launch folder** (the transcript's `cwd`) and in its **inferred folder**
  (10.3). It counts once per folder, so a session whose inferred folder is its launch folder counts once.
- The universe is **every session the page gathered for its time window** — the request's "all
  sessions", against the RECENT half's "recent sessions" — except the AI search's extra rows and,
  unless **Hidden** reveals them, hidden sessions. Every listed folder therefore has at least one
  listed session; older folders come with a wider window.
  - Measured on the release profile (2026-10-12, its sidecar index): one month = 653 sessions in 40
    launch folders; three months = 1,458 in 62; all time = 3,624 in 215.
- A launch "folder" that is not an absolute path (the encoded project-folder name the page shows when
  no line carried a `cwd`) is no folder; such a session can still count through its inferred folder.
- Matching is `NormDirKey` (Rule #8). **Exact** by default — the counts in the lists are exact — or,
  with **Include subfolders**, the folder and everything below it, at a segment boundary (`K:\src`
  covers `K:\src\app`, never `K:\srcfoo`; a picked drive root covers the drive). The lists themselves
  always count exactly: a parent folder would otherwise top MOST USED with everyone else's sessions.

### 10.3 The inferred folder of every listed session

- The tab colors' rule (`_ScanInferredTabColors`): the sidecar's `pathsAccessed`, the machine temp
  roots excluded (`ExcludePathsUnderRoots`), `InferWorkingDirectory` with the git arm per
  `AppSettings::inferGitRoot`. An answer equal to the launch folder is stored empty.
- Computed by a **follow-up pass** (`_InferSessionsRowDirs`) after each gather, off the UI thread, so the
  table never waits for it. The cost that decided this: a Python model of the git walk over one month
  of sessions (4,571 distinct parent folders) made 6,442 `.git` probes, **6.6 s** with a cold disk cache
  and 0.3 s warm. `GitRootMemo` is that walk with every probed ancestor memoized.
- A per-window **cache** (`_sessionsInferredDirCache`), keyed by session id + the transcript's (size,
  mtime) + the git setting: the gather's background loop takes cache hits itself and hands only the
  misses to the pass, so only a window's first gather pays. A session that touched no path has nothing
  to infer and never becomes a job. Results reach the rows only for the gather that asked
  (`_sessionsGatherGen`); the cache takes them either way.
- When a pass lands: the table re-renders (with the page hidden, when it returns — the overlay's
  restore hook), the selected row's detail pane is redrawn, and an open dropdown re-lists.

### 10.4 Code

- `AgentMaster/SessionFolderFilter.h` — pure and header-only: `CollectSessionFolders`,
  `SortFoldersByRecency` / `SortFoldersByUse`, `SplitFolderFindTerms` / `FolderMatchesFindTerms`,
  `IsAbsoluteFolderPath` / `TrimFolderPath`, `FolderKeyCovers` / `SessionInPickedFolders`, `GitRootMemo`.
- `TerminalPage.AgentSessionsPageActions.cpp` — the toggle, the dropdown and its handlers
  (`_BuildSessionsFolderFilter` through `_InferSessionsRowDirs`); the predicate rides
  `_SessionsRowPassesRowFilter`, the chip text `_UpdateSessionsFilterChip`.
- `TerminalPage.AgentSessionsPage.cpp` — the gather's cache lookup and jobs, the dismiss / hide / restore
  hooks, the Up/Down guard, the card's LocalTooltip scope, the `Works in` lines.
- `TerminalPage.h` — `_SessionsRow::inferredDir`, `_SessionsRowFilterState::folders`,
  `_SessionsFolderFilterUi`, the cache and the inference job.

### 10.5 Tests, logs, limits

- `TestSessionFolderFilter` (tests_transcript.cpp): the universe and its counts (spelling variants,
  launch vs inferred, the encoded fallback), both orders and their tie-breaks, the find box, typed paths,
  the predicate (exact, subfolders, segment boundary, drive root, UNC, OR), `GitRootMemo` against a
  counting probe (one probe per new ancestor) and against the real `FindGitRootForDir` on a scratch tree
  (a `.git` directory and a worktree's `.git` file), and the inference through it.
- hooks.log: `[sessions-folders] inferred N session folder(s) in X ms (P .git probe(s), git arm on|off)`,
  one line per pass. Picks are not logged (a view filter, like every facet).
- Limits: not run on screen yet; not persisted across restarts; MOST USED counts the window's sessions,
  not all time (a folder the window does not reach would list nothing to pick).
