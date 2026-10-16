# Agentmaster — the session agent: tools to search, read and travel every session

> **Status: DESIGN ONLY (2026-10-13). Nothing here is built.** It redesigns the tools of the
> Sessions page's AI search ([`SESSIONS.md`](SESSIONS.md) §9; the harness:
> [`SDK_HARNESS.md`](SDK_HARNESS.md)) on top of the session database
> ([`SESSIONS_DB.md`](SESSIONS_DB.md)). Where this document and `SESSIONS_DB.md` §8.4.3 name different
> tools, this document is the newer proposal. Every number is measured unless it says "estimate";
> how to reproduce each is in §13.
>
> **2026-10-17: three pieces exist in a first form** (written, NOT BUILT; [`SESSIONS.md`](SESSIONS.md)
> §9.12), on today's code — not this design's database phase:
> - **`grep`** — a text or an ECMAScript regex over the period's (or the given ids') transcripts, the
>   first three matches per session in context with `by` / `at`. Unlike §5 here: no refs, no scopes beyond
>   `in` (all / user / claude), sessions ordered by the user's own hits first, a 20 s budget.
> - **`read`** — up to 8 sessions at `minimal`, `compact` or `default` (no outline, full, max or raw; no
>   paging by turns or refs): a session past `chars` keeps its first and last turns.
> - **`show` with `why`** — one line of evidence per session (no `ref`). The system prompt gained the rules
>   "read every candidate before you show it", "a `why` each" and a verdict sentence (nine rules).
>
> The rest of this design (`peek`, the zoom levels, refs, `files`, `related`, `timeline`, `reveal`, the
> context line, the instant pass, redaction) is still unbuilt.

## 0. Summary

**The ask (2026-10-13).** Redesign the tools the harness gives the AI search, so that it can dig out
what the user looks for with full-text search, recreate a session's transcript and digest it at a
high level (compact, then full, to go deeper), and travel freely between sessions.

**What ships today.** Two tools: `search` lists sessions (by title, folder, touched paths and typed
prompts; one `deep` switch adds everything else) and `show` puts ids on the screen. The model cannot
open a session, exclude a folder, search shell commands on their own, or learn from a result why it
missed. Its first real follow-up (2026-10-12, §1) took **52.6 s**, 41.7 s of it in one full-text
scan, and showed a session from the very project the user had asked it to leave out.

**The design, in one table:**

| Server | Tool | The question it answers | In `SESSIONS_DB.md` §8.4.3 |
|---|---|---|---|
| `sessions` | `search` | Which sessions match these words, or any of these phrasings, in these scopes (incl. shell commands and edits), in or not in these folders, touching this file, in this period? Each row with evidence refs; facets to narrow; why a word missed. | `search`, `who_touched` |
| | `grep` | Where exactly does this text or regex occur? Every place, in order, with context. | `grep` |
| | `peek` | What were these sessions about? Up to 8 at once: the summary facts and one line per turn. | `session` |
| | `read` | This session, or these turns of it, as a transcript at zoom level outline, minimal, compact, default, full, max or raw. | `messages` |
| | `files` | What did this session touch? | `files` |
| | `related` | Its lineage, the sessions that shared its files or rare words, what ran beside it. | `related` |
| | `timeline` | What ran in this period? | `timeline` |
| | `usage`, `sql` | Tokens and cost; anything else, read-only. Database phase only. | same |
| `display` | `show` | Put these sessions on the screen, each with the reason and the evidence. | `show` (+ `why`, `ref`) |
| | `reveal` | Open this place for the user: the transcript at that turn. | — |

**Around the tools** (§7–§8): a context line on every request (the time, the page's filters, what is
on the screen), an instant search of the user's words before the model's first step, a clock and a
progress line in every result, secret redaction of every result, a stop rule, a new system prompt,
and an evaluation set that settles each open choice by measurement (§11).

**Expected effect (estimate from the trace's model time, §1.2).** The 52.6 s follow-up becomes about
five model steps of ~1.7 s each plus tool time: about 9–10 s on the session database, and from the
right project (§9.1). On today's code (phase A, §10), scoped content scans replace the one 42 s scan
and stop at a budget. The price: the fixed prompt (system prompt + tool list) grows from 1,620 to
7,000 tokens (measured, §8.7), served by the prompt cache after a process's first request.

## 1. The shipped agent, and its first real failure

### 1.1 What it is

- One windowless `claude.exe` per conversation (Sonnet 5.5, thinking off, effort `low`, `--tools ""`,
  two in-process MCP servers), bypass permissions; in production since 0.6.8.19 (2026-10-12).
- The process is ready in ~0.7 s; a fresh request with one search and one show put the list on the
  screen 3.2–4.0 s after the request (the live test) and 4.6 s in the trace below.
- A seven-rule system prompt (`SessionSearchTools.cpp` `kSystemPrompt`): act at once, one search with
  1–3 words, then more words, then `days: 365`, then `deep: true`; show once; one sentence.
  - Since 2026-10-13 (in no build yet) it has eight: the new first rule makes the model's first step
    a short mention of which kinds of tool calls it can use for the request, out of everything its
    tools can do, and how it will use them in different ways, written in the same message as its
    first calls (`SESSIONS.md` 9.3, measured in 9.9). The proposed prompt keeps that rule (§8.6).

### 1.2 The trace (the release profile's harness record, 2026-10-12)

| Time | Step | Took | Answer |
|---|---|---|---|
| 20:31:33 | **"We find lost code"** | | |
| +1.6 s | `search {"query": "lost code"}` | 0.3 s | 3 sessions |
| +3.5 s | `show ["7e345983", "7cb81be9"]` | | both in `K:\source\NumSharp` |
| +4.6 s | reply | | "…a NumSharp session that scans all worktrees for lost code." |
| 20:31:52 | **"In a different project, we used git for it"** | | |
| +1.4 s | `search {"deep": true, "query": "git lost"}` | **41.7 s** | 300 matched |
| +43.1 s | `search {"days": 365, "query": "git reflog"}` | 2.1 s | 0, and "sessions before 2026-07-14 are not in the index yet" |
| +47.4 s | `search {"query": "recover dangling commits"}` + `search {"query": "stash"}` | 0.1 s each | 0; 6 |
| +50.4 s | `show ["38f148f0"]` | | `K:\source\NumSharp` again |
| +52.6 s | reply | | "…It's also in NumSharp, so I found nothing in another project." |

Model time: 4.3 s over three steps in the first request, 8.6 s over five steps in the second
(**~1.5–1.7 s per model step**). Tool time in the second: 44.0 s.

### 1.3 What the tools could not do

1. **Exclude a folder.** "A different project" is a negative folder filter; `search` has `dir` (include)
   only. Five of the eleven sessions that ran git object-recovery commands are NumSharp's (§1.4);
   nothing kept them out, and the one session shown was NumSharp's.
2. **Search what was done.** The words the user remembers ("lost", "git") are not the words in the
   session ("git fsck --unreachable --no-reflogs", "dangling", "unreachable commits"), and shell
   commands sit only inside `deep`, together with replies, thinking and all tool output.
3. **Try phrasings together.** Each phrasing cost a model step (~1.7 s) plus its own scan.
4. **Read.** No tool opens a session; 38f148f0 was shown without being looked at.
5. **Learn from a miss.** A 0-row result named no word that failed and said nothing about older
   sessions; the index's coverage note made the model stop instead of widening.

### 1.4 What the right tools would have found (a read-only check of the transcripts, 2026-10-13)

Shell commands that ran git object recovery (`git fsck`, `--lost-found`, dangling or unreachable
objects) appear in **11 sessions**: NumSharp 5, `~/.claude` 3 (+1 in a `.claude - Copy` folder),
LEANN 1, proxmox 1; two more sessions mention them in text only. Outside NumSharp, by what the session
was doing:

| Session | Folder | Active | What its outline shows |
|---|---|---|---|
| 66c94e21 | `K:\source\proxmox` | 2026-10-05 | Before deleting an old repo: "The old repo has 5 unreachable commits (not on `main`, reflog empty). Checking what they are before anything gets deleted" (line 4813) — **a recovery of commits** |
| 6191d2fe | `C:\Users\ELI\.claude` | 2026-07-16 | "git is for some reason locked. fix lock, push…" — repairing a repo |
| 39526d1e, 0e5817b4 | `C:\Users\ELI\.claude` | 2026-01-30 | "The git is corrupt, try to fetch"; a pasted `git reset --hard`, then `fsck` — repairing or checking a repo |
| 79ba0c00 | `C:\Users\ELI\.claude - Copy` | 2026-01-09 | "…its corrupt, I want you to either fix credentials login or fix corrupt" — repairing a repo |
| 27a6a430 | `K:\source\LEANN` | 2026-01-19 | "The corruption persists at the disk level…" — repairing a repo |

`git reflog` in a shell command adds three more sessions outside NumSharp, none a recovery (two
Agentmaster `git reflog -12` status checks; a proxmox `git reflog expire`). Which session the user
meant was not confirmed. The point: one `search` with phrasings over the `command` scope and
`not_dirs: ["NumSharp"]` returns these (the older ones after its `outside_window` hint widens the
period), and one `peek` tells a recovery from a repair (§9.1).

## 2. Principles

1. **Model steps cost; tool calls are cheap.** A model step takes ~1.7 s; a database tool call 2–300
   ms. So tools take batches (`any` phrasings, up to 8 ids per `peek`), are read-only so Claude Code
   runs a step's calls together (§8.3), and return enough for the next decision in one result.
2. **Zoom, don't dump.** A session is read at the level the question needs, a few turns at a time:
   outline → minimal → compact → default → full → max → raw, sized by measurement (§4.2).
3. **One address space.** Every hit, turn and block has a `ref`; every reading tool accepts it and
   every result cites the refs of what it shows. Traveling is following refs.
4. **Results teach.** A thin result says why (which word matched nothing, how many matches lie
   outside the period, how complete the index is); a broad one says how to narrow (facets).
5. **Filters carry meaning,** negative ones included: "another project" (`not_dirs`), "not those"
   (`not_ids`), "the file X" (`touched`), "last week" (`after`).
6. **Evidence before the screen.** `show` takes a reason and an evidence ref per row; the user sees
   both and can open the evidence.
7. **Read-only, bounded, interruptible.** Every `sessions` tool changes nothing, answers within its
   budget or says `partial`, and asks before each slow step whether its request is still wanted.
8. **One registry, two clients.** The in-app agent and any Claude Code session (the external MCP
   server, `SESSIONS_DB.md` §8.4) get the same tools, minus `display`.
9. **Nothing secret leaves.** Every result is redacted for credentials before the model reads it
   (§8.4): the transcripts hold some.

## 3. The tools at a glance, and the travel map

| From | To | Tool |
|---|---|---|
| words, several phrasings | sessions, ranked, with evidence | `search` |
| exact text, a regex | every place, in order | `grep` |
| a file or folder | the sessions that touched it | `search` (`touched`, `op`) |
| a period | what ran | `timeline`; `search` (`after`, `before`) |
| candidate sessions | what each was about | `peek` |
| a session | its story; one turn in detail; exact blocks | `read` (levels, `turns`) |
| a ref | the context around it | `read` (`around`) |
| a session | its files | `files` |
| a session | its lineage, file and word neighbors, what ran beside it | `related` |
| anything else | rows | `sql` (database phase) |
| what was found | the screen; one place opened | `show`; `reveal` |

## 4. Addresses and zoom levels

### 4.1 Ids, refs, turns

- **A session**: its short id (the first 8 characters of its UUID, longer when two sessions share
  them); every `id` parameter also takes a longer prefix or the whole UUID. **A subagent's own
  transcript**: `<id>/<agent id>`.
- **A ref** names one line, or one block of a line: `<id>:<line>` or `<id>:<line>.<block>`; a
  subagent's: `<id>/<agent>:<line>`. `<line>` is the 1-based line number in the session's `.jsonl`
  (ripgrep's numbers on today's code; `line.line_no` in the database), so a ref is the same on both
  backends and survives the database's merges and re-clusters (`SESSIONS_DB.md` §8.4.2).
- **A turn**: `<id>#<n>`, turn `n` of the session's own part (the numbering of `read` with
  `chain: false`). "Work before the first prompt" is turn 0.
- Every tool that takes a place takes any of the three; `read(around: ref)` reads the turn that holds
  the line.

### 4.2 The zoom levels, measured

The levels are the Copy ▸ Transcript levels (`TranscriptTurns`, pinned byte for byte to the
am-agentmaster-cli skill's `transcript_turns.py`), plus an outline below them and the raw blocks
above them. Sizes from 26 sessions (a stratified random sample of the 1,800 transcripts changed in
the last 120 days, rendered by the reference script with `--no-chain`; §13.1):

| Level | Adds | Per turn, chars (median / p90) | Whole session, chars (median / p75 / p90 / max) | Whole session fits 12K / 24K / 48K |
|---|---|---|---|---|
| outline | one line per turn (estimate: header + first line of the user's message + start of the last reply) | 218 / 312 | 366 / 1,366 / 2,559 / 37,450 | 96% / 96% / 100% |
| minimal | the user's messages and each final reply | 3,135 / 5,558 | 6,164 / 25,492 / 45,952 / 306,285 | 62% / 73% / 88% |
| compact | + the user's answers, slash commands, compactions, subagent briefs and reports | 3,135 / 5,578 | 6,277 / 25,556 / 46,316 / 345,520 | 62% / 73% / 88% |
| default | + every tool call on one line, the remarks between calls | 10,095 / 28,008 | 29,492 / 79,087 / 185,186 / 989,565 | 31% / 50% / 62% |
| full | + each shell command, its first output line, per-call durations | 20,145 / 71,963 | 75,247 / 168,545 / 437,510 / 2,374,053 | 19% / 35% / 42% |
| max | + every file written, a stats line per turn | 23,070 / 84,665 | 80,014 / 269,933 / 788,930 / 4,243,039 | 19% / 35% / 38% |
| raw | the exact message blocks, verbatim, each with its ref | — | about the transcript's size (default is ~2% of the `.jsonl` bytes, median) | — |

Turns per session: median 2, p75 6, p90 12, max 179. A turn is one user message and all the work
until the next one, and long autonomous turns are common, so a "turn" at `full` is often a large
read.

What follows for the tools:

- **`peek`**: an outline fits whole for 96% of sessions in 12K characters and is under 2,600
  characters for nine sessions in ten, so 8 candidates fit one result; longer outlines are cut (first
  10 turns, last 30).
- **`read` defaults to `minimal` with a 20K-character budget** (~5K tokens): the whole story of about
  two thirds of sessions in one call.
- **`compact` is nearly free over `minimal`** (they differ only where answers, subagents or
  compactions exist); **`default` is the real step down**, and `full` / `max` are read one turn at a
  time (a p90 turn at `full` is 72K characters, above the largest budget: it is paged).
- **Render cost**: the C++ port renders 1.7 MB in 24 ms and 128.7 MB in 1.62 s at `default`
  (`SESSIONS_DB.md` §8.5); the reference script took 0.15–0.88 s per level here (with its interpreter's
  start). A rendered session is cached (§6.4).

## 5. Conventions every tool follows

`SESSIONS_DB.md` §8.4.2 holds (read-only annotations, short ids and refs, local times with `now`,
compact JSON, paging cursors, `partial`, `indexing`, errors that say how to fix the call, ASCII
descriptions). Added here:

- **`elapsed`**: every result carries the seconds since the user's request began — the agent's clock
  (§7.3).
- **A progress line**: every call reports one line for the page ("Searching shell commands for 6
  phrasings, not in NumSharp…") (§7.4).
- **Redaction**: every string of every result passes `RedactSecrets` (§8.4).
- **Concurrency**: each `sessions` tool is `readOnlyHint: true` (`HarnessTool::readOnly`). claude.exe
  (2.1.291) treats exactly those MCP tools as safe to run together:
  `isConcurrencySafe() { return mayOverlap || (annotations?.readOnlyHint ?? false) }`.
- **Size**: JSON results stay under 64 KB and `read` under its budget (at most 60,000 characters).
  Claude Code's own cap for an MCP result is **25,000 tokens** (`MAX_MCP_OUTPUT_TOKENS`, default `M =
  25000` in claude.exe 2.1.291), so no tool reaches it.
- **Hidden sessions** (the page's "Hide from list") are left out unless the page's Hidden box is on;
  the context line says which (§7.1).
- **Tool search**: the in-app harness runs `--tools ""`, so Claude Code has no ToolSearch tool and
  loads every MCP tool into the prompt (claude.exe: "Tool search disabled: ToolSearchTool is not
  available"; the trace's first step called `search` directly). In an ordinary Claude Code session
  the external server's tools are deferred behind ToolSearch by default, so each description starts
  with the words a person would ask with.

## 6. The contracts

Each contract gives the description the model reads (exact, ASCII), the parameters, the result, the
errors, the plan on today's code (**A**, §10) and on the session database (**B**), and the budget.
Example values are illustrative unless marked real.

### 6.1 `search`

- **Description:** "Find sessions (Claude Code conversations on this computer) by words, by the files
  they touched, and by when and where they ran. Every word of `query` must match somewhere in a
  session, in the chosen `scopes` (each word may match a different place); \"double quotes\" make a
  phrase; a whole session id finds that session and its forks. `any` adds up to 6 alternative
  phrasings and a session must also match at least one of them: users describe outcomes, sessions
  contain the actions, so add the commands, error texts, file and function names that would appear.
  Returns sessions best first; each row says where it matched and gives up to `hits` evidence
  snippets whose `ref` opens that place in read. A thin result says which words matched nothing and
  how many matches lie outside the period; a broad one adds `facets` to narrow with. Changes nothing
  on the screen. Fast: search again freely."
- **Parameters:**

  ```json
  {
    "type": "object", "additionalProperties": false,
    "properties": {
      "query":    {"type": "string", "maxLength": 500, "description": "Words that must all match. Leave it out to list sessions by `any` and the filters only."},
      "any":      {"type": "array", "maxItems": 6, "items": {"type": "string", "minLength": 1, "maxLength": 200},
                   "description": "Alternative phrasings; a session must match at least one (all of that phrasing's words). Results are merged; `via` says which matched."},
      "scopes":   {"type": "array", "uniqueItems": true,
                   "items": {"enum": ["title", "path", "prompt", "reply", "command", "edit", "tool", "output", "thinking", "subagent"]},
                   "default": ["title", "path", "prompt", "reply", "command"],
                   "description": "Where words may match. title: names and ids. path: folders and touched files. prompt: what the user typed. reply: Claude's text. command: shell commands Claude ran. edit: text Claude wrote into files. tool: other tool inputs (searches, reads, web, agents). output: tool results (broad, noisy). thinking. subagent: subagents' work."},
      "fuzzy":    {"type": "boolean", "default": false, "description": "Also match words one or two letters off."},
      "days":     {"type": "integer", "minimum": 1, "maximum": 3650, "default": 90, "description": "Only sessions active in the last N days."},
      "after":    {"type": "string", "format": "date", "description": "Only sessions active on or after this local date, YYYY-MM-DD. Overrides days."},
      "before":   {"type": "string", "format": "date", "description": "Only sessions active on or before this local date."},
      "dirs":     {"type": "array", "maxItems": 5, "items": {"type": "string", "maxLength": 300}, "description": "Only sessions whose folder (launched in, or worked in) contains one of these parts. '/' and '\\' are the same."},
      "not_dirs": {"type": "array", "maxItems": 10, "items": {"type": "string", "maxLength": 300}, "description": "Leave out sessions whose folder contains any of these parts: 'another project'."},
      "touched":  {"type": "string", "maxLength": 300, "description": "Only sessions that touched this: a full path, a folder (everything under it), or a file name alone."},
      "op":       {"enum": ["read", "edit", "write", "search", "shell"], "description": "With touched: only this kind of touch."},
      "branch":   {"type": "string", "maxLength": 200, "description": "Part of the git branch name."},
      "tag":      {"type": "string", "maxLength": 100, "description": "A bookmark tag, the whole name, any case."},
      "favorite": {"type": "boolean", "description": "true: only starred sessions."},
      "open":     {"type": "boolean", "description": "true: only sessions with a tab open now."},
      "model":    {"type": "string", "maxLength": 60, "description": "Part of a model id the session used: opus, sonnet, fable, a preset."},
      "ids":      {"type": "array", "maxItems": 50, "items": {"type": "string"}, "description": "Only these sessions: 'which of those mention X'."},
      "not_ids":  {"type": "array", "maxItems": 100, "items": {"type": "string"}, "description": "Never these sessions."},
      "sort":     {"enum": ["relevance", "active", "created"], "default": "relevance", "description": "relevance: name matches, then path, then content by score. active / created: newest first."},
      "hits":     {"type": "integer", "minimum": 0, "maximum": 5, "default": 2, "description": "Evidence snippets per session."},
      "limit":    {"type": "integer", "minimum": 1, "maximum": 30, "default": 10},
      "cursor":   {"type": "string", "description": "The `next` of the previous result, for the following rows."}
    }
  }
  ```

- **Result** (the two evidence texts are the real lines 4804 and 4813 of 66c94e21; the rest is
  illustrative):

  ```json
  {"now": "2026-10-13 07:10", "elapsed": 2.1, "window": "last 90 days", "searched": 1459, "matched": 3,
   "rows": [
    {"id": "66c94e21", "title": "We own sysd.cc, in our cloudflare infra...", "dir": "K:\\source\\proxmox",
     "active": "2026-10-05 23:58", "created": "2026-10-04 14:02", "turns": 31,
     "match": "command", "via": [0, 3], "hits": 4, "score": 13.1,
     "evidence": [
      {"ref": "66c94e21:4804", "turn": "66c94e21#29", "scope": "command",
       "text": "echo \"== unreachable/dangling objects\"; git fsck --unreachable --no-reflogs 2>/dev/null | grep -v '^unreachable blob' | ..."},
      {"ref": "66c94e21:4813", "turn": "66c94e21#29", "scope": "reply",
       "text": "The old repo has 5 unreachable commits (not on `main`, reflog empty). Checking what they are before anything gets deleted:"}]}],
   "words": {"git fsck": {"window": 2, "outside": 4}, "lost-found": {"window": 0, "outside": 0},
             "dangling commit": {"window": 0, "outside": 0}, "unreachable commit": {"window": 1, "outside": 0},
             "git reflog": {"window": 1, "outside": 2}, "recover lost commits": {"window": 0, "outside": 0}},
   "outside_window": {"matched": 6, "newest": "2026-06-19"},
   "next": null}
  ```

  - `match` is the best place a match used: `name`, `path`, `prompt`, `command`, `edit`, `reply`,
    `tool`, `output`, `thinking`, `subagent`, or `listed` (no words). `via` lists the indexes of the
    `any` phrasings the session matched. `hits` counts matched blocks; `score` exists for content
    matches.
  - `evidence` picks up to `hits` blocks, preferring different scopes (a command and a reply over two
    replies). `turn` is present when the session's turn index is known (always on B; on A when the
    session was parsed before).
  - `words` (each word of `query` and each phrasing of `any`: sessions matching it alone, in the window
    and outside) and `outside_window` appear when a word matched nothing or fewer rows than `limit`
    matched. `facets` (`dirs`: the top 8 folders with counts; `months`; `scopes`: matched blocks per
    scope) appear when more sessions matched than were returned.
- **Errors:** an unknown scope; a date that is not `YYYY-MM-DD`; `after` later than `before`; `op`
  without `touched`; a `cursor` from other parameters; unknown `ids` or `not_ids` (listed). A query of
  1–2 characters searches title, path and prompt only and says so (`SESSIONS_DB.md` D8).
- **Ranking.** Name, then path, then content; inside content, BM25 with a weight per scope (prompt
  1.5, command 1.3, edit 1.2, reply 1.0, tool 0.8, subagent 0.7, output 0.5, thinking 0.5; to be
  tuned on the evaluation set, §11). With `any`, each phrasing ranks its own list and the lists merge
  by reciprocal rank (score = Σ 1 / (60 + rank)), so a session that several phrasings find rises.
- **Plan A (today's code).** Title, folder, branch, id and touched paths from the catalog
  (`SessionCatalog`, in memory); `prompt` through `SearchHistoryPrompts`; the content scopes through
  `SearchTranscriptsSlow` with two changes: its block classifier splits tool calls by tool name
  (`command`: Bash, PowerShell; `edit`: Edit, Write, MultiEdit, NotebookEdit; `tool`: the rest) and
  results (`output`), and the `subagent` scope adds `<id>\subagents\*.jsonl`; one ripgrep pass carries
  every phrasing's longest word, and each phrasing's AND is checked per block in process. `touched`
  reads the sidecar's `pathsAccessed` (no `op` yet: an `op` is ignored with a note). `outside_window`
  counts only what the index keeper has read (it says so). Facets from the catalog.
- **Plan B (database).** The tiers of `SESSIONS_DB.md` §6 with the scopes as `doc.role` + `doc.tool`
  filters; RRF in the service; facets by one `GROUP BY` over the matched session ids; `words` from
  `dict.df` and `spost` with and without the time bound; `touched` / `op` from `path_access`.
- **Budget.** A: the catalog part ≤ 300 ms, the content scopes ≤ 8 s, then `partial` with the newest
  sessions done. B: 150 ms (300 ms with `any` and facets).

### 6.2 `grep`

- **Description:** "Find every place an exact text or a regular expression occurs, in order, with the
  text around it: an error message, a hash, a function name, a command. Searches the sessions in
  `ids`, or all sessions in the filters. Use search to rank sessions by topic and grep to locate
  exact strings. Each match has a `ref` for read. A regex needs a literal of 3 or more characters, or
  `ids`."
- **Parameters:** `pattern` (1–500 characters) or `patterns` (up to 5, any of them), `regex`
  (default false), `case_sensitive` (default false), `ids` (up to 50), `not_ids`, `dirs`, `not_dirs`,
  `days` (default 90) / `after` / `before`, `scopes` (as in `search`; default every scope but
  `thinking`), `context` (0–400 characters each side, default 100), `per_session` (1–20, default 3),
  `limit` (1–200, default 30), `cursor`; `additionalProperties: false`.
- **Result:** `{"now", "elapsed", "sessions": 4, "matches": 17, "groups": [{"id", "title", "dir",
  "active", "count", "matches": [{"ref", "turn", "time", "scope", "tool", "text"}]}], "next",
  "partial"}` — grouped by session, newest session first, matches in transcript order.
- **Errors:** an invalid regex (the engine's message); a regex without a 3-character literal and
  without `ids`; both `pattern` and `patterns`.
- **Plan A.** `rg --json` (`-F` for text, `-i` unless case-sensitive) over the transcript files of
  `ids`, or of the catalog's sessions in the window (+ their subagent files); each hit line parsed
  once, the match located in its block, the scope attributed, the snippet cut.
- **Plan B.** As `SESSIONS_DB.md` §8.4.3: candidates through the index (the regex's longest literal),
  then exact verification on the candidate blocks.
- **Budget.** A: 3 s, then `partial` (newest sessions first). B: 200 ms.

### 6.3 `peek`

- **Description:** "A quick look at up to 8 sessions at once, to tell what each was about without
  reading it: its facts (titles, folder, branch, dates, size, models, lineage, tags, open tab, files
  edited, plan, tasks) and its outline, one line per turn: when, how much work, what the user asked
  and how Claude's last reply began. Turns that hold evidence from your latest search are marked. Use
  it on a search's candidates before you show them."
- **Parameters:** `ids` (1–8, required), `outline` (lines per session, 0–200, default 40; a longer
  session keeps its first 10 and last 30 turns and says which are left out), `files` (0–10, default
  5); `additionalProperties: false`.
- **Result** (the shape; the two quoted texts are 66c94e21's real first prompt and a real reply, every
  number and the turn numbers are placeholders):

  ```json
  {"now": "2026-10-13 07:10", "elapsed": 4.0, "sessions": [
    {"id": "66c94e21", "title": "We own sysd.cc, in our cloudflare infra...", "dir": "K:\\source\\proxmox", "branch": "...",
     "created": "...", "active": "2026-10-05 23:58", "turns": 31, "tools": 2210, "subagents": 14,
     "models": ["..."], "ctx": 412000,
     "lineage": {"forks": []},
     "summary": {"edited": 37, "created": 12, "read": 160, "skills": ["..."], "tasks": "9 done, 0 open", "compactions": 3},
     "files": [["K:\\source\\proxmox\\...", {"edit": 14}]],
     "outline": [
      "#1 10-04 14:02-15:40 312 tools | user: We own sysd.cc, in our cloudflare infra. plan for me: 1. how do the domain behind sysd.cc ... | claude: ...",
      "... 18 turns left out (11-28): read turns 11-28 at level outline ...",
      "#29 10-05 21:30-21:58 41 tools *2 hits* | user: ... | claude: Import committed as `03958f8`, with parents `48703f2` and `1d68d37` ..."]}]}
  ```

- **Errors:** an unknown id (the others are still answered); more than 8 ids.
- **Plan A.** The card from the catalog entry, the registry (open, the live title), the session store
  (tags, star), the lineage (`forkedFromId`, the catalog's forks of the id, `CollectConversationUpstream`
  for plan, `/clear` and `/handover` parents); `summary` from `AnalyzeSessionTranscript` (the summary
  panel's analyzer); the outline from a new **`RenderTranscriptOutline`** in the TranscriptTurns port —
  an Agentmaster-only function on the same parse as the levels, so the outline and `read` can never
  disagree; no golden changes. Each parse is cached by (path, size, mtime); `peek` parses up to 4
  sessions at once on its own threads.
- **Plan B.** The `summary` row (`SESSIONS_DB.md` §8.5, ~3 ms) and a new **`turn`** table filled at
  ingest from the port's parse: (session, n, first_line, last_line, start, end, tools, edits,
  interrupted, compacted, user_head, reply_head). About 35,000 rows for today's corpus (an estimate:
  ~8 turns × 4,444 sessions).
- **Budget.** A: 2 s; a session whose parse is not done by then comes back with its card and
  `"outline": "pending"` (`partial`). B: 10 ms per session.

### 6.4 `read`

- **Description:** "Read one session as a transcript at the zoom level you need, a few turns at a
  time. Levels, shortest first: outline (one line per turn); minimal (the user's messages and each
  final reply; a whole session is often 3K to 25K characters); compact (+ the user's answers, slash
  commands, compactions, subagent briefs and reports); default (+ every tool call on one line and the
  remarks between them; about 10K characters per turn); full (+ each shell command, its first output
  line, durations; about 20K per turn, some 70K+); max (+ every file written, stats per turn); raw
  (the exact message blocks, verbatim, each with its ref; set `roles`). Start shallow and go deeper
  only on the turns that matter: `turns` (\"3\", \"2-4\", \"last\", \"last 2\") or `around` (a ref
  from search or grep). The text ends with `next` when more remains."
- **Parameters:**

  ```json
  {
    "type": "object", "additionalProperties": false, "required": ["id"],
    "properties": {
      "id":     {"type": "string", "minLength": 8, "maxLength": 80, "description": "A session id, or <id>/<agent id> for a subagent's own transcript."},
      "level":  {"enum": ["outline", "minimal", "compact", "default", "full", "max", "raw"], "default": "minimal"},
      "turns":  {"type": "string", "maxLength": 20, "description": "\"5\", \"2-4\", \"last\", \"last 3\" or \"all\". Default: from turn 1, or the turn of `around`."},
      "around": {"type": "string", "maxLength": 100, "description": "A ref from search or grep: the turn that holds it (raw: the blocks around it)."},
      "roles":  {"type": "array", "uniqueItems": true,
                 "items": {"enum": ["prompt", "queued", "reply", "thinking", "tool_call", "tool_result", "system"]},
                 "default": ["prompt", "queued", "reply", "tool_call"], "description": "Level raw only: the kinds of blocks."},
      "chain":  {"type": "boolean", "default": false, "description": "Also render the sessions this one was forked or restarted from, as one conversation; turns then number through the chain."},
      "budget": {"type": "integer", "minimum": 2000, "maximum": 60000, "default": 20000, "description": "At most this many characters; the rest is paged."},
      "cursor": {"type": "string", "description": "The `next` of the previous result."}
    }
  }
  ```

- **Result:** text, not JSON (a transcript is a document; JSON would escape every newline and quote).
  A header line, the turns exactly as Copy ▸ Transcript renders them with each turn heading followed
  by its ref, and a footer:

  ```
  session 66c94e21 · "We own sysd.cc, in our cloudflare infra..." · K:\source\proxmox · level compact · turn 29 of 31 · 7,412 chars · elapsed 5.9
  ### Turn 29 · Oct 5 21:30 → last reply Oct 5 21:58   [66c94e21:4711]
  **❯ User**
  > ...
  **● Claude**
  ...
  --- next: turns 30-31 (cursor "t:30") · deeper: level default or full with turns "29"
  ```

  A turn longer than the budget is cut at a line boundary and continued by the cursor
  (`"…[turn 29 continues: cursor t:29.2]"`). `raw` returns blocks as `[ref] time role tool: text`.
- **Errors:** an unknown id or agent; a ref of another session; a `turns` value it cannot read (it says
  the forms); `roles` with a level other than raw.
- **Plan A.** `RenderTranscriptTurnsFile` (the C++ port: the same text as Copy ▸ Transcript; `--no-chain`
  unless `chain`), cached per (path, size, mtime, level, chain) in a 64 MB LRU. A new **turn index**
  out of the same render (turn → first and last line) lets the tool slice turns and map refs; the
  rendered text does not change, so the goldens hold. `raw` reads lines by number through a cached
  line-offset index of the file. A subagent's transcript renders the same way.
- **Plan B.** The same renderer over the lossless line store (`SESSIONS_DB.md` §5.5, byte-exact), so a
  session that Claude swept from disk stays readable (D7); `raw` blocks from `doc` by ref.
- **Budget.** A: the first render of a session at a level as measured (24 ms for 1.7 MB, 1.62 s for
  128.7 MB at default), then ~1 ms per slice. B: the same render without the file read and JSON
  re-parse once the render reads parsed rows (`SESSIONS_DB.md` §8.6, "Later").

### 6.5 `files`

- **Description:** "The files and folders one session read, edited, wrote, searched or named in shell
  commands, with counts and first and last times. Use it to see what a session worked on; to go the
  other way (which sessions touched a file) use search with `touched`."
- **Parameters, result, errors:** as `SESSIONS_DB.md` §8.4.3 `files` (`id`, `op`, `under`, `limit`).
- **Plan A.** The sidecar's `pathsAccessed` (deduplicated, at most 512 paths, no kind and no counts):
  the result has `"ops": null` and a note that kinds and counts come with the database. **Plan B:**
  `path_access`.
- **Budget.** A: 20 ms. B: 20 ms.

### 6.6 `related`

- **Description:** "Travel from one session to its neighbors: its lineage (the session it was forked,
  restarted or continued from, its forks and successors), the sessions that touched the most of the
  same files, the sessions that share its rare words, and the sessions that ran in the same folder
  within a day of it."
- **Parameters:** `id` (required), `kinds` (array of `lineage`, `files`, `words`, `nearby`; default
  all), `limit` (1–10 per kind, default 5); `additionalProperties: false`.
- **Result:** `{"lineage": {"forked_from", "forks", "continues": {"id", "via"}, "continued_by"},
  "by_files": [{"id", "title", "shared": 12}], "by_words": [{"id", "title", "words": ["..."]}],
  "nearby": [{"id", "title", "active"}]}`.
- **Plan A.** Lineage from the sidecar's `forkedFromId`, the catalog (forks of the id),
  `CollectConversationUpstream` and the registry's `continuedFromId`; `by_files`: the overlap of the
  catalog's `pathsAccessed` sets (in memory); `nearby`: the catalog's sessions with the same folder
  (`NormDirKey`) active within 24 hours; `by_words` is B only (it needs document frequencies) and the
  result says so.
- **Plan B.** As `SESSIONS_DB.md` §8.4.3 `related`, plus `nearby`.
- **Budget.** A: 100 ms. B: 100 ms.

### 6.7 `timeline`

- **Description, parameters, result:** as `SESSIONS_DB.md` §8.4.3 `timeline`, plus `not_dirs`.
- **Plan A.** The catalog's created and last-active times (a session counts on both days); prompts per
  day are not in the sidecar, so `prompts` is null. **Plan B:** the prompt and reply timestamps.
- **Budget.** A: 50 ms. B: 50 ms.

### 6.8 `usage` and `sql`

As `SESSIONS_DB.md` §8.4.3, database phase only. The `sql` description lists the views; it is the
escape hatch for counts and odd filters, never for reading text (that is `read`).

### 6.9 `show`

- **Description:** "Put sessions on the user's screen in your order, replacing the list. Each row needs
  `why`: what in that session matches the request, concrete, in a few words (shown under its title),
  and the `ref` of your evidence (a click opens the transcript there). Show only sessions you have
  evidence for. Call it once per request, after checking the candidates; an empty `rows` clears the
  list when nothing fits."
- **Parameters:**

  ```json
  {
    "type": "object", "additionalProperties": false,
    "properties": {
      "rows": {"type": "array", "maxItems": 30,
               "items": {"type": "object", "additionalProperties": false, "required": ["id", "why"],
                         "properties": {"id": {"type": "string"}, "why": {"type": "string", "maxLength": 100}, "ref": {"type": "string", "maxLength": 100}}}},
      "top":  {"type": "integer", "minimum": 1, "maximum": 30, "description": "Instead of rows: the first N rows of your latest search; each row's why is its first evidence."},
      "note": {"type": "string", "maxLength": 80, "description": "A caption for the list, in the user's language."}
    }
  }
  ```

- **Result:** "Shown 3 sessions. Now reply: one sentence; a question about content: up to three."
- **Errors:** both or neither of `rows` and `top`; unknown ids (listed); a row without `why`; a ref of
  another session.
- **On the page.** Each row shows `why` as a dim second line under its title, and an evidence link
  (⤷) that opens the transcript overlay ([`TRANSCRIPT_OVERLAY.md`](TRANSCRIPT_OVERLAY.md)) at the ref's
  turn, at level default. The overlay scrolls to session headings today; a turn heading is the same
  scroll.
- **Not in the external server** (it changes the Agentmaster window).

### 6.10 `reveal`

- **Description:** "Open one place for the user: selects the session on the screen and opens its
  transcript at the turn of `ref`, at `level`. Use it after show when the user asked to see or read
  something specific."
- **Parameters:** `ref` (required), `level` (`minimal` … `max`, default `default`);
  `additionalProperties: false`.
- **Result:** "Opened 66c94e21 at turn 29, level default."
- **Errors:** a ref whose session is not on the screen ("show it first"); an unknown ref.
- **Not in the external server.**

## 7. Around the tools: what the harness adds to each request

### 7.1 The context line

The page prepends one line to every user message (the record keeps the user's words and the line
separately):

```
[context] now 2026-10-13 07:12 Mon | page: last 1 month, folders: any, hidden: off | on screen (your last show): 1. 7e345983 NumSharp 10-01 "forgotten code" - scans all worktrees for lost code; 2. 7cb81be9 NumSharp 08-24 "..." - ... | selected: 7e345983 | this window's session: K:\source\Agentmaster
```

- Up to 15 rows, each with its folder's last part, its active date, its title (≤ 50 characters) and
  its `why` (≤ 40); at most 1,500 characters.
- It gives a follow-up its referents: "a different project" (the folders on the screen), "the second
  one", "older", "only the open ones". It prevents a failure seen twice in the live tests of
  2026-10-12: after a list of one or two rows, "only the three newest of those" made the model search
  again and ADD sessions.
- `SessionSearchAgent::Send` takes it from a page callback; the page has every field (the time
  window, the folder filter of 589931f9c, the Open / Hidden / Favorite boxes, the AI list, the
  selection, the window's last focused session).

### 7.2 The instant pass

At Send, before the model's first step, the agent runs a cheap search of the request's content words
(stop words dropped; sessions ranked by how many of the words they match, then by recency): on the
database, `search` with each word as one `any` phrasing (~150 ms); on today's code, the catalog's
name, path and prompt tiers (milliseconds).

- Its top 8 go into the context line as `quick matches (your words, unverified): …`, so the model's
  first step can already `peek` them while it searches more widely.
- On the first request of a conversation (and after ⊘), the page also shows them at once, dimmed,
  under "Quick matches — refining…", until the model's `show` replaces them. A follow-up keeps the
  current list on the screen (it usually refines it).

### 7.3 The clock and the stop rule

- Every result carries `elapsed`; the system prompt says: show by the fourth step; by step 8 or 30
  seconds, show the best you have (§8.6, rule 9).
- The hard cap stays: `SessionSearchAgent.cpp` sets `HarnessOptions::maxTurnsPerMessage = 12` today,
  above the stop rule's 8 steps.

### 7.4 Progress on the page

Each call reports one line, which the strip shows while the agent works ("Searching shell commands
for 6 phrasings, not in NumSharp…", "Looking at 4 sessions…", "Reading 66c94e21, turn 29…"). Today
only `search` reports (`onSearching`); every tool gets the callback.

### 7.5 Memory across requests, parks and restarts

The record (`<profile>\harness\sessions-search.json`) and the view file (`….view.json`) also keep each
shown row's `why` and `ref` and each request's context line. A new process's digest ends with the
last shown list and its reasons, so "the second one" still resolves after a park or a restart.

### 7.6 Interrupts and supersedes

Unchanged: the generation number (`SessionSearchToolset::Callbacks::generation`). Every tool asks it
before each slow step (A's scans and parses, `peek`'s threads), and the service cancels its query
(`SESSIONS_DB.md` §9 `cancel`).

- ⚠ **Today a call that STARTS after the supersede is not cancelled** (seen 2026-10-13, `SESSIONS.md`
  9.11). The handler takes the number when it starts (`SessionSearchTools.cpp`, `startGeneration`).
  A call that the SDK queued behind a slow call of the same step (§8.3) starts after the next
  request raised the number, reads the new one, and runs in full.
  - Measured: a superseded request's second full-text scan of 365 days ran 29 s, and the follow-up
    reached the process only after it: the harness writes a new request when the old turn has
    ended, and Claude Code's acknowledgement of the interrupt waits behind the queued call on the
    SDK's reader thread.
  - The fix (for today's tool and for every tool here): a call compares with the generation of the
    request whose turn the process is answering, not with the newest. The agent can record that
    generation when the harness reports that it wrote the request (its `UserMessage` event); a
    call whose turn is older returns at once.

### 7.7 What the user sees

1. Enter → (first request) quick matches at once, dimmed.
2. The strip narrates: "Searching…", "Looking at 4 sessions…", "Reading…".
3. The list: each row with its `why` line and its evidence link.
4. The answer strip: one sentence, or up to three for a question about content; a question back when
   the results split into different groups.
5. A click on an evidence link (or `reveal`) opens the transcript at that turn.

## 8. Harness changes

### 8.1 One registry

`SessionAgentTools` (the `SessionDbTools` registry of `SESSIONS_DB.md` §8.4.4, renamed for what it
serves): one table entry per tool — server, name, description, schema, annotations, budget, phase
(A / B), handler. In-app, it builds the two `HarnessToolServer`s; the external `mcp` mode builds its
tool list from the same table without `display`. A version number (`kSessionAgentToolsVersion`) is
recorded in the harness record and logged: a description is part of the model's prompt and its cache
key, so every change of one is visible.

### 8.2 Model and process settings

| Setting | Today | Proposed | Why |
|---|---|---|---|
| Model | Sonnet 5.5 | Sonnet 5.5 | speed; the tools carry the precision |
| Thinking | the user's level: off by default (the strip's toggle since 2026-10-14, changed live: `SESSIONS.md` 9.1, `SDK_HARNESS.md` 5.3). On Sonnet 5.5 "off" is the API's `between_tools` (no thinking before the answer; per the API, the short updates between tool calls may come back as thinking blocks — measured: none, they came as text) — the measurements of §1 ran with an "off" that still thought | off by default; the toggle stays | each step stays ~1.5–1.7 s (§1.2) |
| Effort | low with thinking off; the level's own effort otherwise (the same toggle) | **decided by the evaluation (E4)**: the default level, low or medium | a multi-step plan (phrasings, filters, a check) is where effort may matter |
| `--max-turns` | 12 | 12 | a safety net under the stop rule (§7.3) |
| Auto-compaction | 500,000 tokens | **200,000 (E5)** | a search conversation piles up tool results (a peek ~3K tokens, a read up to ~15K); a smaller window keeps each step's context small, and the record and digest carry the continuity |
| `--tools ""` | yes | yes | no built-in tools; and no ToolSearch, so the MCP tools are always loaded (§5) |
| Permission policy | bypass, then auto, then dontAsk | unchanged | `SDK_HARNESS.md` §5.1; with bypass the tool list is the only limit, and every tool here is read-only or a display |

### 8.3 Parallel calls: the SDK runs them one at a time

- Claude Code sends the read-only calls of one step at once (§5). The SDK
  (`refs/claude-agent-sdk-cpp/src/client.cpp`, the reader loop) handles each `mcp_message` control
  request on its reader thread: it calls the handler, then writes the response; the next request
  waits. So a step's calls run one after another, and while a handler runs, the reader cannot deliver
  the interrupt of its own request (the generation check covers that, §7.6).
- On the database (2–300 ms a call) this costs little: ≤ 1.5 s for a five-call step. On A's
  seconds-long content scans it adds up.
- **Option (E6):** the reader hands each `mcp_message` to a small worker pool, and the worker writes
  the response. The write is already safe from several threads (`SubprocessTransport::write` takes
  `write_mutex_`). It also frees the reader for interrupts. It changes the pinned submodule: a fork,
  or an upstream PR, and the checks of `SDK_HARNESS.md` §9.

### 8.4 Secret redaction

- **Why:** the transcripts hold credentials in plain text (the check of §1.4 met a GitHub token pasted
  into a user prompt). Whatever a tool returns goes to the model and can end up in a `why` line or the
  answer.
- **What:** `RedactSecrets` (pure, header-only, unit-tested) replaces known shapes before any result
  leaves a tool, in-app and in the external server: GitHub (`ghp_`, `gho_`, `ghu_`, `ghs_`, `ghr_`,
  `github_pat_`), Anthropic (`sk-ant-`), OpenAI (`sk-`, `sk-proj-`), AWS (`AKIA` / `ASIA` keys, and a
  40-character value after `aws_secret_access_key`), Slack (`xox[abprs]-`), Google (`AIza`), Stripe
  (`sk_live_`, `rk_live_`), JWTs (three dot-separated base64url parts, each of the first two starting `eyJ`), PEM private-key blocks, Azure `AccountKey=` /
  `SharedAccessKey=`, and a long high-entropy value after `password`, `passwd`, `secret`, `token` or
  `api_key` with `=` or `:`. The replacement names the kind: `[redacted github-token]`.
- Search still finds a session by its other words; the page's own search (no model) is unchanged.

### 8.5 Logs and implicit feedback

- Per request: `[ai-search] request 7 done: steps 5, calls search x1 peek x2 read x1 show x1, model
  7.9 s, tools 0.3 s, shown 1, elapsed 8.4 s`.
- Per call: `[ai-search] tool peek 41 ms (4 sessions) [partial]`.
- Implicit relevance: `[nav] sessions ai open-row rank=2 <sid8>` when the user opens, resumes or forks
  a row the agent showed, and `[nav] sessions ai evidence <sid8>` for an evidence click. The share of
  requests whose top row is opened is the live quality signal; logs stay local.

### 8.6 The system prompt (replaces the shipped eight rules; ASCII, 4,064 characters, 1,299 tokens)

```
You are the search agent of the Sessions browser in Agentmaster, an app that manages Claude Code
sessions. The user looks for past Claude Code sessions (conversations) on this computer, or for
something that was said or done in one. You travel the sessions with read-only tools, put what you
found on the screen with show, and answer briefly. The user sees the list, the why line of each row,
and your reply; not your tool calls or their results.

Each request starts with a [context] line: the time, the page's filters, what is on the screen now
(your last show, with its why lines), the selected row, and the folder of the user's current
session. Follow-ups refer to it: "a different project" means not the folders on the screen; "the
second one" is the second row; "older" is older than the rows shown. A request that only narrows or
reorders what is on the screen ("only the open ones", "the three newest") is answered from those
rows with show, without a new search.

Tools (mcp__sessions__*, all read-only): search (sessions by words, phrasings, scopes, folders, files
touched, time), grep (every place an exact text or regex occurs), peek (what up to 8 sessions are
about: facts and one line per turn), read (one session as a transcript at a zoom level, a few turns
at a time), files, related, timeline, usage, sql. Display (mcp__display__*): show, reveal.

How to work:
1. Your first step, for every request: before your first tool call, in the same message, mention in
   one or two short lines which kinds of tool calls you can use for this request, out of everything
   your tools can do (search by phrasings, scopes, folders, files touched and time; grep for an
   exact text; peek and read the candidates; files, related, timeline; usage and sql; show and
   reveal), and how you will use them in different ways (the phrasings and scopes you will try, the
   filters, the level you will read at, what you will check before show). Then make the calls of
   that step at once.
2. Act at once. Never ask before searching. Make independent calls in the same step.
3. Phrase widely in one call. Users describe outcomes; sessions contain the actions. Put 3 to 6
   phrasings into search's `any`: synonyms, the commands, error texts, file and function names that
   would appear. Keep the `command` scope on when the request is about something that was done.
4. Turn meaning into filters: a project or folder -> dirs or not_dirs; a time -> after and before; a
   file -> touched (and op); starred -> favorite; open tabs -> open.
5. Read the hints in every result: `words` (a word matched nothing: rephrase it), `outside_window`
   (matches exist before the period: widen days), `facets` (narrow by folder or month), `partial` or
   `indexing` (coverage is incomplete: say so in your reply).
6. Check before you show. Peek the candidates (one call, up to 8 ids). If an outline does not settle
   it, read the turn of the evidence (`around` its ref) at level minimal or compact, or grep inside
   the candidates. Use default, full, max or raw only for the one turn whose details you need.
7. Show once, best first, only sessions you have evidence for. Each row gets `why`: what in that
   session matches, concrete, under 12 words, in the user's language; and `ref`: the evidence. When
   nothing fits, show an empty list.
8. Reply after show. A search request: ONE sentence under 25 words (what is on the screen and the
   best match's title), for example: Three sessions are on the screen; the best match is "Fix tab
   drag crash". A question about content (what was decided, which command worked, why it failed):
   answer in up to three short sentences and name the session; call reveal when the user asked to
   see the place. When the results split into clearly different groups, end with one short
   question. The mention of rule 1 is not your reply. No markdown, no ids.
9. Be quick. Every result gives `elapsed` seconds. Aim to show by the fourth step; by step 8 or 30
   seconds, show the best you have and say what you tried.
10. Times are local; `now` is in every result. Use only ids that a result gave you.
```

Rule 1 is the shipped prompt's rule 1 (the user's request of 2026-10-13), with this tool list in its
parentheses; `SESSIONS.md` 9.3 states it and 9.9 has its measured cost.

- **The mention is written in the same message as the step's first calls**, so it adds output
  tokens to that step, never a model request: measured on the shipped agent, the first call came
  1.6–2.1 s after the request instead of 1.3–1.5 s.
- **It made the replies wordier.** The example in rule 8 and the sentence "The mention of rule 1 is
  not your reply" are what brought the shipped agent's replies back to one sentence (`SESSIONS.md`
  9.9). The evaluation (§11) checks both against this prompt's longer reply rule.
- **It does not bind the model.** In one shipped run the mention named a folder search that the
  model then did not make; the evaluation reads the mentions beside the calls.

### 8.7 What the prompt and the tool list cost (measured)

claude.exe 2.1.291's own context report (the control request `get_context_usage`, sent with no user
message, so no model request; `tools/sessiondb-bench/agent_prompt_cost.py`). The proposed tools are
[`SESSIONS_AGENT.tools.json`](SESSIONS_AGENT.tools.json), the machine-readable form of §6.

| | System prompt | Tool list | Together |
|---|---|---|---|
| Shipped (8 rules; `search`, `show`) | 892 tokens (2,824 characters) | 728 (`search` 535, `show` 193) | **1,620** |
| Proposed (§8.6; 11 tools) | 1,299 (4,064 characters) | 5,701 | **7,000** |
| Proposed, phase A (without `usage`, `sql`) | 1,299 | 5,015 | 6,314 |

Per tool: `search` 1,648, `read` 829, `grep` 735, `show` 398, `sql` 381, `peek` 375, `usage` 305,
`timeline` 302, `related` 280, `files` 271, `reveal` 179.

- **The first rule's cost** (the first step's mention, 2026-10-13, measured again the same day): the
  shipped prompt went from 661 tokens (2,064 characters, seven rules) to 892, with the reply rule's
  example; the proposed one from 1,085 (3,343 characters) to 1,299. The fixed prompt was 1,389
  (shipped) and 6,786 (proposed) before it. The seven-rule prompt is the one in production
  (0.6.8.19).

- It is the fixed start of every model request, so the prompt cache serves it after a process's
  first request; the evaluation (§11) measures the step time with it.
- `search` is a quarter of the list: the scope and filter descriptions. The evaluation shows which
  ones the model needs; a description it never uses is cut.

## 9. Worked examples

Timings are estimates: ~1.7 s per model step (§1.2) plus the tool budgets of §6 (B).

### 9.1 The real follow-up, replayed

Context: on the screen, 7e345983 and 7cb81be9, both `K:\source\NumSharp`.

| Step | Calls (one step, run together) | What comes back |
|---|---|---|
| 1 | `search {"any": ["git fsck", "lost-found", "dangling commit", "unreachable commit", "git reflog", "recover lost commits"], "scopes": ["prompt", "reply", "command"], "not_dirs": ["NumSharp"]}` | in the 90 days: 66c94e21 (proxmox, `command`), 6191d2fe (`~/.claude`), 6d5c4df3 (proxmox, a `git reflog expire`); `outside_window: {"matched": 6, "newest": "2026-06-19"}` |
| 2 | `peek` of the three + the same `search` with `days: 365` | 66c94e21: "…5 unreachable commits… before anything gets deleted"; 6191d2fe: "git is for some reason locked…"; 6d5c4df3: "Have nucs.dev temporarily redirect to github.com/Nucs" (a cleanup, not a recovery); the wider search adds two Agentmaster sessions (June, `git reflog -12`), 39526d1e, 0e5817b4, 79ba0c00 and 27a6a430 |
| 3 | `peek` of the six older ones | the Agentmaster pair read recent history; the other four repair a corrupt repo |
| 4 | `show {"rows": [{"id": "66c94e21", "why": "found 5 unreachable commits in the old repo before deleting it", "ref": "66c94e21:4813"}]}` | |
| 5 | reply | "One proxmox session fits: it recovered unreachable commits with git fsck before deleting the old repo; the others repaired repos or only read history." |

About 9–10 s on the database (five steps, tools < 1 s), against 52.6 s; no NumSharp row, and a
reason the user can check with one click. On today's code the two content scans add a few seconds
each: their words (`fsck`, `lost-found`, `dangling`, `unreachable`, `reflog`) are rare, so ripgrep's
prefilter keeps few files.

### 9.2 "What did we decide about the delivery gate?"

1. `search {"any": ["delivery gate", "DeliveryGate", "gate owner"], "scopes": ["title", "prompt", "reply", "edit"]}`.
2. `peek` the top 4; the outlines show which session designed it and which only used it.
3. `read {"id": "<best>", "level": "compact", "around": "<its evidence ref>"}`.
4. `show` the 1–3 sessions with whys; `reveal` the decision's turn.
5. Reply in up to three sentences: the decision, and the session's title.

### 9.3 "Which sessions edited TerminalPage.cpp last week, and why?"

1. `search {"touched": "TerminalPage.cpp", "op": "edit", "after": "2026-10-05", "sort": "active", "limit": 20}`
   (on A, `op` is ignored with a note: the sidecar has paths but no kinds).
2. `peek` the rows (two calls if there are more than 8); each outline's first user line is the "why".
3. `show` with whys; one sentence.

### 9.4 "What was I doing last Tuesday afternoon?"

1. `timeline {"from": "2026-10-06", "to": "2026-10-06", "group": "hour"}`.
2. `show` the sessions active 12:00–18:00, each `why` its first prompt of that afternoon.

### 9.5 "Only the three newest of those"

No search: the context line lists the rows with their dates; `show` the three newest with their old
whys; one sentence.

## 10. Phases

Phase A runs on today's code (the catalog, the sidecars, ripgrep, the TranscriptTurns port) and ships
without the database; every tool keeps its contract in phase B, where the service answers faster and
more completely.

| Tool or part | Phase A (today's code) | Phase B (session database) |
|---|---|---|
| `search` | catalog tiers in ms; content scopes by scoped scans (≤ 8 s, then partial); `touched` without `op`; facets; `outside_window` within the indexed period | 150–300 ms, every scope, `op`, `outside_window` over all sessions |
| `grep` | `rg --json` + attribution, ≤ 3 s | 200 ms |
| `peek` | analyzer + `RenderTranscriptOutline`, cached; ≤ 2 s for 8 | `summary` + `turn` rows, ~10 ms per session |
| `read` | the port's render, cached, sliced by the new turn index | the same over the lossless line store |
| `files` | paths only | paths, kinds, counts, times |
| `related` | lineage, shared files, nearby | + rare words |
| `timeline` | sessions per day | + prompts per day |
| `usage`, `sql` | — | yes |
| `show` (why, ref), `reveal` | yes | yes |
| Context line, instant pass, clock, progress, redaction, logs, prompt | yes | yes |

**Phase A work list:** `SessionAgentTools` with the eight phase-A tools; the scope split in
`SearchTranscriptsSlow`'s classifier (+ subagent files); `any` with RRF; `not_dirs`, `not_ids`,
`touched`, `ids`; the `words` / `outside_window` / `facets` result fields; `RenderTranscriptOutline`
and the turn index in the port (no golden change); the render cache; `RedactSecrets`; the context line
and the instant pass in `SessionSearchAgent` and the page; the why line and the evidence link on the
page; the turn jump in the transcript overlay; `reveal`; progress for every tool; the new system
prompt; the logs.

**Phase B** (with `SESSIONS_DB.md` P3): the tools call the service; the `turn` table at ingest;
`usage` and `sql`; the external server gets the same registry (P4).

## 11. Tests and evaluation

- **m5 harness (no model):** every schema parses; defaults and clamps; every error text; each tool
  over a fixture catalog (A) and a fixture database (B) as golden text; the `read` slicer (turns, the
  budget cut, cursors) over the TranscriptTurns goldens of `tests/turns/expected`, which must stay byte
  for byte; `RenderTranscriptOutline` on the same fixtures; `RedactSecrets` on a table of real-shaped,
  fake secrets and near misses (a SHA, a GUID, base64 image data must stay).
- **The evaluation set (live, `harness_live --agent-eval <file>`):** 15–20 requests, each with the
  sessions it expects and the folders or sessions it must not show; with follow-ups. Run against a warm
  scratch copy of the index (phase A) or the database (phase B), never the live profile.
  - Sources: the two real requests of §1.2 (the second one's expectation to be confirmed by the user);
    requests whose answer is true by construction (sessions that edited a file, sessions holding an
    exact error text, a period's sessions); and requests the user labels from memory.
  - Measured per run: the expected session shown (in the top 3), a forbidden one shown, model steps,
    seconds, tokens, rows shown.
  - Each open choice of §12 (effort, the compaction window, the instant pass, the scope weights) is
    decided by these numbers; each description change of the registry reruns the set.
- **Live traces per tool** (`SESSIONS_DB.md` §8.4.4 rule 7): does the model pick the tool when it
  should.

## 12. Decisions for the user

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **E1** | `peek` and `read` | **Two tools**: peek many (card + outline), read one (levels, turns) | One `read` with a `card` level and batch ids for card and outline (fewer tools, a busier schema) |
| **E2** | `why` in `show` | **Required**, with an evidence `ref` the user can open | Optional (faster to write, unverifiable) |
| **E3** | The instant pass on the screen | **Shown, dimmed, on a conversation's first request**; given to the model on every request | Given to the model only |
| **E4** | Effort | **The one the evaluation picks**: medium if it finds the expected session more often at no more than +2 s median | Low (today) |
| **E5** | Auto-compaction window | **200,000 tokens** for this agent | 500,000 (today) |
| **E6** | The SDK's serial tool dispatch | **Change it only if phase A's measured steps lose more than 1 s to it**; phase B's tools make it moot | A worker pool in our pin of the SDK now |
| **E7** | `reveal` | **Yes** | The evidence link only |
| **E8** | Answer length | **One sentence for a search; up to three for a question about content**; a question back when results split | One sentence always (today) |
| **E9** | Redaction | **Always on**, in-app and in the external server | An off switch |
| **E10** | Order of work | **Phase A first**: everything but the database's speed works on today's code and carries over | Wait for the database (P3) |
| **E11** | The tool's name | **Keep `search`** (the shipped name, the logs, `SESSIONS_DB.md`) | `find` (clearer next to `grep`) |
| **E12** | The external server | **The same `sessions` tools**, `peek` and `read` included (the data a session can already Read), no `display` | Search tools only |

## 13. Measurements and sources

### 13.1 Transcript sizes per level (2026-10-13)

26 sessions, 6 per size bucket (2 in the largest), drawn with seed 7 from the 1,800 top-level
transcripts changed in the last 120 days; rendered by `transcript_turns.py` (the am-agentmaster-cli
skill; its default preset `latest` is v9, the preset the C++ port is pinned to) with `--no-chain`.
Characters of the rendered Markdown; the outline is the estimate of §4.2.

| Bucket | Session | Bytes | Turns | Outline | Minimal | Compact | Default | Full | Max |
|---|---|---|---|---|---|---|---|---|---|
| < 200 KB | a713aba2 | 141,399 | 8 | 1,490 | 10,504 | 10,504 | 11,113 | 13,135 | 16,959 |
| | 6337e445 | 57,581 | 1 | 167 | 1,897 | 1,897 | 2,662 | 3,300 | 3,408 |
| | 158911d4 | 132,389 | 1 | 231 | 1,558 | 1,558 | 3,583 | 5,991 | 6,082 |
| | 02e592b1 | 178,689 | 1 | 168 | 1,770 | 2,281 | 6,702 | 21,604 | 22,435 |
| | 08685461 | 81,848 | 1 | 213 | 288 | 288 | 699 | 785 | 873 |
| | c4545bfd | 36,422 | 3 | 294 | 441 | 505 | 568 | 568 | 827 |
| 200 KB – 1 MB | f05dbb45 | 425,359 | 5 | 1,367 | 35,004 | 35,004 | 42,118 | 71,903 | 72,368 |
| | e68b47ad | 646,333 | 1 | 320 | 3,229 | 3,229 | 17,166 | 33,646 | 36,659 |
| | 058415f3 | 949,998 | 1 | 305 | 4,104 | 4,104 | 13,655 | 24,961 | 66,519 |
| | 3a5091f1 | 922,606 | 1 | 259 | 3,379 | 3,379 | 14,297 | 16,375 | 16,471 |
| | f6279712 | 276,746 | 1 | 320 | 2,647 | 2,647 | 7,223 | 12,910 | 13,005 |
| | 743b9124 | 317,935 | 1 | 320 | 2,530 | 2,530 | 6,056 | 9,061 | 9,156 |
| 1 – 5 MB | 4a6e826e | 2,113,109 | 9 | 2,016 | 27,365 | 27,365 | 65,012 | 149,620 | 180,712 |
| | 4a22cd14 | 1,430,444 | 3 | 795 | 8,419 | 8,705 | 37,588 | 108,670 | 140,409 |
| | 52fbb4c2 | 1,004,556 | 1 | 228 | 4,092 | 4,230 | 21,395 | 78,591 | 82,232 |
| | 82fc0c80 | 3,147,307 | 1 | 241 | 4,581 | 4,785 | 18,459 | 65,335 | 77,795 |
| | 8500dbb7 | 3,819,342 | 1 | 208 | 6,659 | 6,659 | 52,508 | 140,887 | 170,466 |
| | 3d3749e6 | 1,608,759 | 5 | 1,363 | 18,227 | 18,227 | 37,724 | 93,426 | 98,133 |
| 5 – 20 MB | e9858b0e | 11,370,443 | 7 | 1,389 | 27,544 | 27,735 | 82,368 | 184,760 | 609,688 |
| | e897bda5 | 12,967,369 | 4 | 812 | 19,371 | 19,542 | 82,629 | 174,853 | 299,673 |
| | 0104237f | 8,512,681 | 65 | 8,872 | 56,901 | 57,627 | 169,905 | 450,284 | 492,653 |
| | 07235450 | 8,999,644 | 6 | 914 | 33,002 | 33,088 | 83,261 | 183,543 | 404,289 |
| | 45fadf2c | 14,133,275 | 16 | 3,102 | 89,860 | 90,252 | 200,467 | 424,735 | 1,000,339 |
| | f62a102b | 9,899,541 | 2 | 413 | 5,670 | 5,895 | 69,244 | 114,317 | 137,742 |
| > 20 MB | a04a5992 | 25,275,773 | 5 | 1,130 | 19,873 | 20,131 | 205,948 | 544,786 | 968,173 |
| | e1214709 | 43,561,046 | 179 | 37,450 | 306,285 | 345,520 | 989,565 | 2,374,053 | 4,243,039 |

- Ratio of rendered characters to `.jsonl` bytes, median (p90): minimal 0.61% (2.3%), default 1.95%
  (4.2%), full 4.1% (8.6%), max 5.5% (10.9%), outline 0.07% (0.31%).
- Whole text within 16,000 characters: minimal 62%, default 38%, full 27%, max 23%, outline 96%.
- Render time of the reference script: median 0.17 s, max 0.88 s (interpreter start included); no
  render failed.
- **Reproduce:** `python tools/sessiondb-bench/turns_level_sizes.py measure <out.json>` (read-only;
  ~2 minutes), then `… stats <out.json>`.

### 13.2 The trace

The release profile's harness record, `%USERPROFILE%\.agentmaster\harness\sessions-search.json`,
items 1–17 of 2026-10-12 20:31–20:32 (§1.2).

### 13.3 The corpus check of §1.4

A read-only ripgrep over `~/.claude/projects/*/*.jsonl` for `git fsck`, `lost-found`,
`dangling (commit|blob)`, `unreachable (commit|blob)` (14 files; this design's own session removed), then
each file's Bash `command` inputs and user prompts read with a short script; `git reflog` inside a
`"command"` value for the second list.

### 13.4 claude.exe and the SDK (read from the code)

- claude.exe 2.1.291: `MAX_MCP_OUTPUT_TOKENS` defaults to `M = 25000`; an MCP tool
  `isConcurrencySafe() { return mayOverlap || (annotations?.readOnlyHint ?? false) }`; tool search is
  off when the ToolSearch tool is not available ("Tool search disabled: ToolSearchTool is not
  available (may have been disallowed via disallowedTools)"); a server or tool `_meta`
  `anthropic/alwaysLoad` keeps tools out of tool search.
- `refs/claude-agent-sdk-cpp/src/client.cpp`: the reader loop calls an SDK MCP server's handler
  synchronously, then `send_control_response`; `SubprocessTransport::write` holds `write_mutex_`.

### 13.5 The prompt cost of §8.7

`python tools/sessiondb-bench/agent_prompt_cost.py [<work folder>]`: reads the shipped prompt and
tools from `SessionSearchTools.cpp` and the proposed ones from this document and
`SESSIONS_AGENT.tools.json`, starts claude.exe twice (no user message), and prints the tokens per
category and per tool. About 10 s.
