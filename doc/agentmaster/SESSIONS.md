# Agentmaster — Sessions Browser (the "Sessions" page) + the `~/.claude` storage map

> **Status: IMPLEMENTED (engine + UI; pending deploy).** The store API (`TranscriptStore`), the
> two-phase search (`SessionSearch`), the presence→observer integration, and the page itself
> (`TerminalPage.AgentSessionsPage.cpp`, the "Sessions" toolbar button after Archived) are
> built, lib-compiled green, and engine-tested (536 harness checks). §7 records the user's
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

- **Toolbar:** Launch · Reopen · `⚙` · Pause Autopilot · Archived · **Sessions** *(new — right
  after Archived)*.
- **Page shell:** duplicate the Archive page structure (`_BuildArchivePageShell` /
  `_ShowArchivePage` — full-window, mounted over `TerminalPage`'s Root content rows, ← Back to
  dismiss, **every pointer handler defers its visual-tree mutation** — the XAML-Islands hit-test
  AV class), with a **search bar at the top** replacing the Archive page's filter box.
- **Content:** all Claude Code sessions on this machine whose **last activity** falls inside the
  selected window (default **1 month**) — not just Agentmaster-managed ones: everything under
  `~/.claude/projects/`.
- **Search bar:** `[ search for sessions ] (👤) (🤖) (📁) (📄) (F) [1 month]`
  - All five are **toggle buttons** (click = select/deselect, visible selected state):
    - `👤` selected ⇒ the search also scans **user messages** (typed prompts).
    - `🤖` selected ⇒ the search also scans **agent + tools** (everything *but* user messages:
      assistant text/thinking, tool inputs/results, system content).
    - `📁` ⇒ match **directories accessed** (tool-call paths' directory parts + the cwd).
    - `📄` ⇒ match **files accessed** (tool-call paths' leaves).
    - `(F)` ⇒ **fuzzy** (query characters in order, gaps allowed — identical semantics in rg
      and the in-process matcher).
  - `[1 month]` — the window selector: **hover** opens a From/To **range popup** (text boxes
    with placeholder dates — answer Q4); **click** cycles
    `1 day → 3 days → 7 days → 14 days → 1 month → 3 months → (wrap)`.
- **Caching:** per-session search/index cache; **invalidated when the session's last-active
  changes** (mechanically: the `(size, mtime)` pair — see §6.2).
- **Opened sessions get more:** a session that is currently OPEN in this app (live in the
  registry) renders enriched — its **per-working-dir tab color** chip, the live **state glyph**,
  and Jump-to-tab; see §6.6 for the full row-affordance matrix.

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
this machine: 25555 ⇒ nothing pruned). User-added entries (skills, commands, hooks, …) omitted.

| Entry | Purpose | Keyed by | Swept |
| --- | --- | --- | --- |
| `projects/<enc-cwd>/<sid>.jsonl` | **The session transcript** (append-only JSONL) | encoded start-cwd + session UUID | yes |
| `projects/<enc-cwd>/<sid>/subagents/agent-<aid>.jsonl` + `.meta.json` | Task-tool **subagent** transcripts + metadata (`{agentType, description, name, toolUseId}`) | parent sid + agentId | with parent |
| `projects/<enc-cwd>/<sid>/tool-results/<toolu_id\|rand>.txt` | **Large tool outputs** spilled out of the transcript | tool_use id | yes |
| `projects/<enc-cwd>/memory/` | Auto-memory notes — **not a session** | project | no |
| `projects/<enc-cwd>/agent-<7hex>.jsonl` | **Legacy** (≤ ~2.0.x) subagent files at top level (818 here); lines carry the parent sid | short agentId | yes |
| `history.jsonl` | **Every typed prompt**, globally: `{display, pastedContents, project, sessionId, timestamp}` — up-arrow recall | — (append) | **no** |
| `sessions/<pid>.json` | **Live-session presence**: `{pid, sessionId, cwd, startedAt, version, kind, entrypoint, status: busy\|idle\|waiting\|shell, updatedAt}` — heartbeat-updated; stale after crashes | OS pid | undocumented |
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
  `compact_file_reference`, `skill_listing`, `date_change`, `queued_command`, …) — **not** user
  text.
- **Untimestamped session-state tail lines** (no uuid): `mode`, `permission-mode`,
  `last-prompt {lastPrompt, leafUuid}`, **`custom-title {customTitle}`** (from `/rename`, `-n`,
  the picker, plan-accept), **`ai-title {aiTitle}`** (async picker title),
  `agent-name`, `agent-setting`, `worktree-state`, `queue-operation {enqueue|dequeue|remove|popAll}`.
- **`summary {summary, leafUuid}`** — **legacy** (every local instance is v2.0.75–2.1.25,
  Jan–Feb 2026; zero in current files — reconciles our earlier 0/1,842 survey with community
  claims). `leafUuid` can point into **another (or a deleted) file** — cross-file and fallible.
- **Compaction never truncates**: `/compact` appends `system/compact_boundary`
  (`logicalParentUuid` = pre-compact tail, `compactMetadata {trigger, preTokens, postTokens,
  preservedSegment, preservedMessages}`) + a `user isCompactSummary:true` line; the file keeps
  growing.

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
  terminals resuming one session interleave into one transcript [docs]. No copy-on-resume.
- **Fork** = new file, full duplicate of the parent subtree from the current context root (the
  latest compact boundary); can contain **zero novel messages**.
- **`/clear`** ends the session (`SessionEnd why_ended:"clear"`) and mints a new id; the old file
  stays resumable.
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
   - **OPEN** (live, `SessionRegistry::Get(sid)->live`) — state glyph + **per-dir tab color**
     chip (`dir-colors.json` palette via the existing dir-color system) + the timing adornment +
     **Jump** (Activate, Rule #2: focus only, never inject).
   - **ARCHIVED** (in `sessions.json`, `!live`) — **Restore here** (the existing
     transcript-gated `claude --resume` seam).
   - **On-disk only** (not in the registry at all) — **Open / Adopt**: resume it into a managed
     tab (`_AdoptExternalClaude`-style, transcript-gated) + **Open New Session Here**.
   - A session live in a FOREIGN host (its sid in `sessions/<pid>.json` but not ours) — mark
     "live elsewhere"; adopting it means two writers on one transcript (the known adopt caveat).
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
`[Request interrupted` — **plus `<task-notification>`** (a post-hook schema addition his filter
misses — today's log shows raw task-notification blobs as messages); dedupe repeated prompts;
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
