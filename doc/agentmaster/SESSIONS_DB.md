# Agentmaster — the session database (DuckDB): one index for every session on the machine

> **Status: IMPLEMENTED 2026-10-15, NOT BUILT into the app.** The service (`agentmaster-sessiondb.exe`), the
> app's refresh tick (D16), the Sessions page's content phase, the AI search's database mode, the CLI verbs, the
> stdio MCP server and the packaging are written and committed (c0253b8b0 … 41fe5e493); the service was built
> standalone and run over the whole corpus (3,595 sessions, 11.3 GB of transcript files), the app files pass `cl /Zs`,
> the m5 suite covers the shared parts (`TestSessionDb`). **Off by default** (the Settings cog's SESSION DATABASE
> section: Off / Shadow / On). What was built, how it differs from this design, and what the full corpus measured:
> **§16**. The design text below (§0–§15) is kept as written on 2026-10-12/14; where the build differs, §16 says so.
> D14 (the summary) and D16 (the refresh) were decided by the user on 2026-10-14; the other decisions (§13) are
> open, and §16.2 says what the build does for each. Companions: [`SESSIONS.md`](./SESSIONS.md) (the Sessions
> page, its search and the AI search as they work today), [`SDK_HARNESS.md`](./SDK_HARNESS.md) (the agent
> harness the AI search runs on), [`CLI.md`](./CLI.md), [`PROFILES.md`](./PROFILES.md).

## 0. Summary

- **The problem (measured, §1).** The Sessions page's content search takes **2 to 42 s** (release
  `hooks.log`: `release` 8.9 s, `str` 33.5 s, `string` 41.7 s). A page open reads one sidecar file per
  session (15–30 s when Windows has not cached them; 3 minutes when they are missing). Every keystroke
  rebuilds the whole table as XAML (about 12 elements per row, up to 2,500 rows).
- **The proposal.** One **DuckDB** database per profile holds every Claude Code session on the machine:
  **every transcript line verbatim** (so any transcript can be rebuilt byte for byte, §5.5), the session
  facts, the text of every message and tool block, the files each session touched, token usage and
  Agentmaster's own facts (titles, tags, favorites, links), plus an **inverted index** for full-text
  search. A small **per-profile service process** owns the file and serves it, over a named pipe, to
  the Sessions page, the AI search, the `agentmaster` CLI and a new **MCP server** that any Claude Code
  session can use, with **eleven read tools specified in full** (§8.4).
- **What the prototype measured on the full corpus** (3,570 sessions, 10.4 GB of transcripts, 1.06 M
  agent/tool blocks; 4 threads, warm cache):

  | Search | Today | Prototype |
  |---|---|---|
  | Session list (all time) | page open 6 s+ (sidecar gather) | **8 ms** |
  | Title / folder / branch | in memory after the gather | **5 ms** |
  | Touched files and folders | in memory after the gather | **26 ms** |
  | Typed prompts | ripgrep + re-parse | **14–37 ms** |
  | Agent and tool text, rare word | 2–42 s | **2–14 ms** |
  | Agent and tool text, common word (49 K blocks) | 7–9 s | **20–33 ms** |
  | In-word substring (`sessionstable`) | 2–42 s | **20 ms** |
  | Text written in the last 24 h (not indexed yet) | — | **32 ms** (scan) |

- **Size and cost.** About **3.1 GB** on disk for today's corpus: 2.2 GB for every line verbatim
  (10.4 GB of JSONL at 4.7×, measured; D11 can leave it out) and ~0.9 GB for the search projection and
  its index (§5.4). A cold build is estimated at about 2 minutes in C++ (§7.3); new text is searchable
  within one refresh period, default 5 s (§7.6). `duckdb.dll` is 36.7 MB (x64, 1.5.6): about +13 MB per architecture in the
  downloads (D10).
- **The database can form the session summary (§8.5, measured and verified)** — a precomputed row in
  **2.8 ms**, its 20 facts equal to the C++ analyzer's on **921 of 921** sessions — for its own
  consumers: the AI search's tools, the MCP `session` tool, the CLI. **The app's summary surfaces do
  not use it** (decided 2026-10-14): the per-tab panel, the side panel, the tab tooltip and the Sessions
  detail keep `AnalyzeSessionTranscript`.
- **The session row (§5.6).** About 120 columns per session, grounded in a census of what Claude Code
  writes: normalized place keys (`cwd_key`, `inferred_dir_key`, `git_root_key`, …, by `NormDirKey`),
  versions, models, effort, Claude Code's **own cost record** (`cost-state`), token usage, outcomes,
  work done, titles, lineage. One pass computes them: 922 sessions (3.11 GB) in 11.5 s.
- **Refreshed by the app's observer, never rebuilt for a search (§7.6; decided 2026-10-14).** The app's
  `TranscriptWatcher` counts each session's changes; every refresh tick (a setting, default 5 s) the
  sessions that changed are upserted — their new lines appended, their derived rows upserted — and a
  tick without a change does nothing. At the app's connect a `resync` catches up. Measured: ~190 MB of
  transcript lines written a day; a tick ~17–27 ms; the busiest hour 0.18–0.34% of one core in the
  service.
- **What DuckDB buys and what it does not (§3.5)**, and **the other parts of Agentmaster that would
  read it (§8.6)**: the conversation chain, the inferred folder of closed sessions, the Fleet
  Observer's External rows, the launch bar's id check, usage and cost, diagnostics.
- **Indexes (§6.8, measured).** Every large table is kept **sorted by its main read key** (sorted merges
  plus a periodic re-cluster, 2–5 s per GB), so DuckDB's automatic min/max zonemaps do the work: a
  session's 304 lines read in 2.9 ms sorted against 113 ms interleaved. **ART indexes only where
  batches of random point lookups happen** (block ids for snippets, line ids): 30 random lines in
  4–7 ms instead of ~145 ms, at 13–16 bytes of memory per row. The text index is our own (dictionary
  in memory, postings in tables).
- **Key choices.** DuckDB **2.0** (released 2026-10-21; 1.5 reaches end of life on 2026-11-01) through
  its **C API** and the official DLL. **Our own inverted index in plain DuckDB tables**, not the `fts`
  extension: §6.5 has the measured comparison (the extension works, but its query macros are about 4–8×
  slower than direct reads of its own tables, its Windows binary is 22–46 MB per architecture, and its
  tokenizer cannot split code identifiers). **A service process**, because DuckDB allows one writer
  process per file and an index build must never share the GUI process. **A virtualized table** on the
  page: the render cost (§1.1, not measured yet) needs no database at all (§8.1).

## 1. The problem, measured

### 1.1 Where the time goes today

| Step | Code | What it costs |
|---|---|---|
| Page open: the gather | `_RefreshSessionsRows` (`TerminalPage.AgentSessionsPage.cpp`) enumerates the transcripts, then calls `LoadOrRefreshSessionIndex` for **each** session (one sidecar JSON per session in `<profile>\sessions-index\`; a new or grown transcript is parsed), then three session-store scans. | 0.3 ms per cached sidecar, 10–20 ms per sidecar on first touch: 15–30 s for 90 days on a cold machine (SESSIONS.md §9.4); **3 minutes** for 3,600 sessions with no sidecars (`SessionSearchTools.h`). Release log: an open at 15:25:51.8 showed its rows at 15:25:58.1 (6.3 s). |
| Fast phase | `SearchIndexFast` (`SessionSearch.cpp`): every term against the entries in memory. | Cheap. |
| Content phase | `SearchHistoryPrompts` (ripgrep over `history.jsonl`) and `SearchTranscriptsSlow`: one `rg -il` per text term over the window's transcripts (batched command lines), then a full in-process re-parse (`ScanTranscript` → `ClassifyTranscriptLine` → `json::Parse` of every line) of every file ripgrep named. | **2–42 s** (below). |
| Render | `_RenderSessionsTable` clears the rows host and builds **every** visible row: about 12 XAML elements, a tooltip string, a registry `Get` (a whole `SessionInfo` copy, queued prompts included) and a linear presence scan per row. It runs on every keystroke (after the 250 ms debounce) and again when the content phase lands. | Not measured. 2,500 rows ≈ 30,000 elements per render. |

**The content phase in the release `hooks.log`** (`[nav] sessions search` → `search-done`):

| Query | Content phase |
|---|---|
| `encrypt` | 2.0 s |
| `paper` | 4.3 s |
| `dl` | 4.5 s |
| `release` | 8.9 s, and 6.9 s on a re-run |
| `str` | 33.5 s |
| `string` | 29.9 s, and 41.7 s on a re-run |
| `e`, `P` (one letter) | 2,506 and 2,307 sessions found |

Two semantic gaps of today's search that the database closes:

- **Only the first 4,096 characters of each line are searched.** `SearchTranscriptsSlow` extracts each
  line's text with `kScanTextBudget` = 4096; a match past that point of a long prompt, tool call or tool
  result is never seen, although ripgrep found the file.
- **Subagent transcripts and spilled tool outputs are never searched** (only top-level `<sid>.jsonl`).

The AI search pays the same costs: a five-search request took 43 s, 15 s of it a 365-day full-text scan
(SESSIONS.md §9.9).

### 1.2 The corpus (2026-10-12)

| | Count | Size |
|---|---|---|
| Session transcripts (`projects/*/<uuid>.jsonl`) | 3,623 | 8.26 GB (p50 0.97 MB, p90 4.6 MB, p99 23 MB, max 129 MB) |
| … changed in the last 1 / 7 / 30 / 90 days | 66 / 209 / 695 / 1,517 | 0.82 / 1.92 / 4.40 / 5.84 GB |
| Subagent transcripts (`<sid>/subagents/agent-*.jsonl`) | 2,406 | 2.14 GB |
| Spilled tool outputs (`<sid>/tool-results/*`) | 4,305 | 0.57 GB |
| `history.jsonl` | 18,521 lines | 8 MB |
| JSONL lines (all transcripts) | 3.30 M | ≤1 KB: 51%, ≤16 KB: 46%, ≤64 KB: 2.4%, ≤1 MB: 0.3%, 9 lines over 4 MB, max 18.5 MB |
| Sidecar files (release / dev profile) | 3,622 / 2,096 | 9.9 / 7.8 MB |

**Searchable text** (the prototype's extraction, one row per block):

| Block | Rows | Text | Median / p99 / max chars |
|---|---|---|---|
| Typed prompt (user string or text block) | 41,849 | 83.6 MB | 326 / 24 K / 568 K |
| Assistant text | 240,292 | 84.7 MB | 130 / 4.3 K / 105 K |
| Thinking | 303,352 | 26.0 MB | 0 / 1.1 K / 173 K (current builds keep only signatures) |
| Tool call (name + input JSON) | 574,494 | 593.7 MB | 296 / 11.5 K / 252 K |
| Tool result | 574,538 | 1,284.4 MB | 431 / 26.6 K / 1.04 M |
| System | 8,025 | 1.5 MB | |
| **Total** | **1.74 M** | **2.07 GB** | |

- **25.6% of all blocks are fork copies.** A fork copies its parent's lines verbatim with the same
  `uuid` (SESSIONS.md §4): 1,742,550 blocks hold 1,295,878 distinct (`uuid`, block) pairs.
- After deduplication, without empty blocks and capped at 16 KB per block: **1.09 M blocks, 1.35 GB**.
- **Typed prompts** (not `isMeta` / `isCompactSummary`, deduplicated; the prototype did not apply the
  `IsNoiseUserPrompt` filter): **23,089 blocks, 15.5 MB**.

## 2. Goals and non-goals

**Goals**

- **G1 — search speed.** First results in **< 50 ms (p95)** for any query of 3+ characters over **all**
  sessions, not a window; the full content answer in < 150 ms (p95). Queries of 1–2 characters search
  titles, folders, paths and prompts in < 50 ms.
- **G2 — page open.** The list on screen in < 100 ms; no per-session file read.
- **G3 — freshness.** Text that a running claude writes is searchable within one refresh period
  (default 5 s, a setting from 1 to 60 s; §7.6) while the app runs.
- **G4 — one store.** The page, the AI search, the CLI and MCP clients read the same database.
- **G5 — the same semantics.** Today's query grammar (AND terms, `"quoted phrases"`, session ids,
  scopes, fuzzy) gives the same sessions for substring queries; tools also get BM25 ranking.
- **G6 — derived and rebuildable.** Deleting the database loses nothing: the transcripts stay the
  source; Agentmaster's own facts stay in their files (session-store, `sessions.json`, settings).
- **G7 — bounded.** ≤ 1 GB of DuckDB memory and ≤ 4 threads by default (settings), background priority
  for maintenance, ~1 GB of disk for today's corpus.

**Non-goals (version 1):** vector / semantic search (DuckDB's `vss` extension is the later path); live
state (the registry and the Fleet Observer own it); replacing `sessions.json`, the session store or the
settings as sources of truth; Codex rollouts (same model, later); syncing between machines.

## 3. DuckDB: what exists

### 3.1 The engine, its versions, its interfaces

- **DuckDB** (MIT): an in-process, columnar, vectorized SQL database with MVCC. One process may open a
  database file read-write; several processes may open it read-only only while no process writes it
  (the official concurrency page).
- **Versions** (the release calendar, 2026-10-12):

  | Line | Released | Status |
  |---|---|---|
  | 1.4 "Andium" | 2025-09-16, patches to 1.4.5 (2026-06-17) | LTS (one year of community support) |
  | 1.5 "Variegata" | 2026-03-09, patches to 1.5.6 (2026-09-28) | end of life **2026-11-01** |
  | **2.0 "Cyanoptera"** | **2026-10-21**; 2.0.1 on 2026-11-18 | alpha now (`pip install --pre duckdb` → `v2.0.0-alpha43763`, used here). "Every other release is LTS" makes 2.0 the next LTS candidate; the calendar does not say so yet. |

- **What 2.0 adds that matters here:** triggers; `quack` (client-server) 1.0; storage format 2.0.0
  (`DICT_FSST` strings by default, lazy column metadata); **a versioned, stable C API** with a thin C++
  wrapper; async I/O; the VARIANT type.
- **Interfaces:**
  - **The C API (`duckdb.h`)** is the stable one, and the one to use: `duckdb_open_ext` + a config
    (threads, memory limit, temp directory, access mode, external access off, no extension autoload),
    one `duckdb_connection` per worker thread, prepared statements (`duckdb_prepared_statement_type`
    tells a SELECT apart; `duckdb_extract_statements` counts statements), **appenders**
    (`duckdb_appender_create`, `duckdb_append_data_chunk`), streaming results (`duckdb_fetch_chunk`),
    `duckdb_interrupt` + `duckdb_query_progress`, and scalar function registration
    (`duckdb_create_scalar_function` — for our own exact fuzzy matcher).
  - The C++ API (`duckdb.hpp`) is internal and changes without notice: not used.
  - **Distribution:** every release has `libduckdb-windows-amd64.zip` (12.7 MB for 1.5.6) and
    `libduckdb-windows-arm64.zip` (13.7 MB) with `duckdb.dll`, `duckdb.lib` and the headers;
    `libduckdb-src.zip` is the amalgamation (4.7 MB of source). The vcpkg port `duckdb` (1.5.6) has
    the features autocomplete, excel, httpfs, icu, json, tpcds, tpch — **no `fts`**. The DLL needs the
    Visual C++ runtime, which the package and the portable zip already carry for Terminal.

### 3.2 Extensions

| Extension | What | Relevant facts |
|---|---|---|
| `json` | JSON reader and functions | Statically linked in `libduckdb` and in the Python build. Not needed here: Agentmaster parses transcripts itself. |
| `fts` | Full-text search | **Not** in `libduckdb` or vcpkg. **1.5.x** pins the February 2026 code (`6814ec9`): a static snapshot index, the `match_bm25` macro, rebuild with `overwrite`. **2.0** pins the July 2026 rewrite (`2266e67d`): `incremental` indexes maintained by triggers on INSERT and DELETE (storage 2.0.0 required), `cluster_terms`, `layered_search` (prefix, substring and fuzzy expansion through dictionary sidecars and a trigram table), BM25F, structured boolean queries, `search_layered_bm25` top-k. The README also lists phrase / near / wildcard / regex modes; **the 2.0 alpha's `search_layered_bm25` accepts only `standard` and `autocomplete`** (measured). ⚠ **On Windows each extension file statically links its own copy of the core: `fts.duckdb_extension` is 22.7 MB (1.5.0) and 46.1 MB (2.0 alpha).** |
| `vss` | HNSW vector index | The later path to semantic search. |
| `quack` | Client-server protocol over HTTP | Default port 9494, binds to localhost, token authentication; beta in 1.5.3+, 1.0 in 2.0. Lets several processes write one database. |

### 3.3 MCP servers for DuckDB

| Project | What | Why it is not the answer here |
|---|---|---|
| **`mcp-server-motherduck`** (MotherDuck, MIT, Python via `uvx`) | Tools `execute_query`, `list_databases`, `list_tables`, `list_columns`, `switch_database_connection`; local files read-only by default with ephemeral connections ("won't hold on to the file lock"); stdio or HTTP. | It opens the file itself: impossible while another process writes it (§3.1). It exposes raw SQL, not the session semantics (scopes, tiers, snippets, short ids). |
| **`duckdb_mcp`** (community extension, Apache-2.0) | `PRAGMA mcp_server_start('stdio')` turns DuckDB into an MCP server with `query`, `describe`, `list_tables`, `database_info`, `export` (and `execute`, off by default); `PRAGMA mcp_publish_tool(...)` publishes parameterized SQL as tools. Windows: server only. | The same file-lock problem, a third-party extension to ship (22+ MB on Windows), SQL templates instead of the search planner. Its `mcp_publish_tool` idea (a tool = a named, parameterized query) is what §8.4 does in C++. |
| `mcp-server-duckdb` and others | One `query` tool. | Same. |

Agentmaster serves its own tools (§8.4) from the process that owns the database.

### 3.4 What Agentmaster uses

The official `libduckdb` DLL of the pinned version (x64 and arm64) through the C API. **No extension**
(Option A in §6.5 would add `fts` through a custom DuckDB build). No network: extension autoinstall and
autoload are off.

### 3.5 What DuckDB buys, feature by feature, and what it does not

**Wins** (measured on this machine unless marked otherwise):

- **Full-text search over every session:** 2–42 s → 2–33 ms (§6; our own inverted index in DuckDB
  tables).
- **The session list without a file read per session:** 6 s+ → 8 ms.
- **A session's summary for the database's own consumers** (the AI search's tools, the MCP `session`
  tool, the CLI): 2.8 ms as a precomputed row, equal to the C++ analyzer's on 921 of 921 sessions. The
  app's summary panels keep the analyzer (§8.5).
- **One pass computes every fact of every session:** `from_json` parses each line once into a typed
  struct (3.11 GB in 6.7 s on 4 threads), and ordered aggregates turn the lines into a session row:
  922 sessions in 11.5 s (§5.6).
- **Columnar storage with ZSTD:** 10.42 GB of JSONL kept verbatim in 2.22 GB (4.7×).
  - Any transcript rebuilds byte for byte (128.7 MB in 0.6–0.9 s), even after Claude's sweep deleted
    it (D7).
  - A parser fix re-derives every table from the stored lines without touching the files.
- **Joins and aggregations across all sessions:**
  - A fork's parent from its copied uuids in 8.7–29 ms; the C++ chain walk scans the project folder in
    260–309 ms.
  - "Who touched this file", related sessions, token use by day / model / folder.
- **Ordered aggregates and window functions** (`arg_min` / `arg_max`, `row_number`, `QUALIFY`, running
  sums) do first / last values, copy de-duplication, compaction segments and timelines in one
  statement.
- **Lists, structs and maps in columns:** `histogram()` gives each session a tool-call map, and a whole
  summary (the numbered prompts, three file lists) is one row.
- **Zonemaps on sorted tables:** a session's rows in 2.9 ms with no index; ART indexes only for batches
  of random id lookups (§6.8).
- **Transactions (MVCC) and a write-ahead log:** an ingest batch is atomic, readers never see half of
  it, and a crash loses nothing committed.
- **Cancellation and progress** (`duckdb_interrupt`, `duckdb_query_progress`): a newer keystroke stops
  the older query.
- **Appenders and prepared statements** for the C++ ingest and the planner.
- **SQL over any field** (`v_lines` + `json_extract`): a new question needs no new parser. The summary
  rules of §8.5 and the session columns of §5.6 were expressed and verified this way, with no C++.
- **Exports** (Parquet, CSV, JSON), and the DuckDB CLI for diagnosis while the service is stopped (or
  through `quack` later).
- **Semantic search later** through the `vss` extension (HNSW).

**No win, or a cost:**

- **Live state** — the scanner's tail and the state machine (Rules #1 and #7), the hook bridge, the
  delivery gate, presence, the pending-input reader. It stays in memory in the GUI process; a database
  round trip would only add latency.
- **Graph walks** (the live branch, long fork chains): SQL runs one recursive step per link. They stay
  C++ at ingest, and the database stores their result (`line.on_branch`, `session.forked_from`).
- **Small answers computed from raw lines:** per-statement overhead makes a 1.7 MB session's summary
  194 ms in SQL against 33 ms in C++. Only precomputed rows are fast (2.8 ms).
- **One writer process per file**, hence the service process (D3), its protocol and its lifecycle.
- **A second implementation** of rules that already exist in C++ (the summary), hence a parity test
  (D14).
- **Size:** the 36.7 MB DLL (+13 MB per architecture in the downloads, D10), ~3.1 GB on disk, a bounded
  memory budget (1 GB), a cold build (~2 minutes, estimated).
- **Traps hit in this prototype** (rules for the implementation):
  - In 2.0, `->` and `->>` take the lambda arrow's precedence: `j->'a'->>'b' = 'x'` binds as
    `j -> (('a'->>'b') = 'x')`. It fails to bind, or reads the wrong field without an error. Use
    `json_extract_string` and `from_json`.
  - `arg_min` / `arg_max` skip rows whose VALUE is NULL; `arg_min_null` does not.
  - `regexp_extract` answers the leftmost match, and `EXPLAIN` alone does not show an ART index scan
    (§6.8).

## 4. Architecture

### 4.1 The processes

```
  ~/.claude/projects/**.jsonl     ~/.claude/history.jsonl     <profile>\session-store\*.json, sessions.json
               │                          │                             │
               ▼                          ▼                             ▼
  ┌──────────────────── agentmaster-sessiondb.exe  (one per profile) ──────────────────────────┐
  │  watcher (ReadDirectoryChangesW) ─► ingest workers (C++ classifier + tokenizer)              │
  │        ─► one writer (appenders, merge, re-cluster) ─► <profile>\sessiondb\sessions.duckdb  │
  │  query workers (one DuckDB connection each) ◄── \\.\pipe\agentmaster.sessiondb.<hash>      │
  └─────────────────────────────────────────▲────────────────────────────────────────────────────┘
            ┌─────────────────────────┬──────┴──────────────┬───────────────────────────────┐
     Agentmaster GUI            agentmaster-cli          agentmaster-sessiondb.exe mcp
     Sessions page, AI search   search / sql / status    (stdio MCP server, one per claude.exe
     tools (pipe client)        (pipe client)             that registered it; a pipe client)
```

### 4.2 Why a service process

1. **DuckDB's lock.** One process may hold the file read-write, and then no other process can open it
   at all. The CLI and the MCP servers run in other processes; so one process must own the file and
   answer everyone.
2. **Isolation.** A cold build reads 10 GB and uses several cores and up to the memory limit for
   minutes; the GUI hosts every live session. A crash or a stall in DuckDB or in the ingest must not
   take the GUI with it, and the GUI never loads `duckdb.dll` (36.7 MB for 1.5.6 x64).
3. **It outlives a GUI restart or a hot swap**, so neither rebuilds anything.

**Rejected:** DuckDB inside the GUI process (simpler, but items 1 and 2); `quack` (an HTTP port and a
token to manage, beta in 1.5; still an option later for power users who want to `ATTACH` the database
from the DuckDB CLI).

**Lifecycle**

- The GUI starts the service at engine init (setting `sessionDb` = Shadow or On), and so does a CLI or
  MCP client that finds no service.
- **One per profile:** a named mutex `Local\Agentmaster.SessionDb.<hash>` and the pipe name, where
  `<hash>` = the first 16 hex digits of SHA-256 of the folded profile path.
- It exits when no client has been connected for 10 minutes. The GUI keeps one connection open while
  it runs, so the database stays current for as long as the app runs; after a downtime, the next start
  catches up (§7.6).
- **Versions:** the `hello` request returns the service's version and schema. A newer GUI asks an older
  service to `shutdown` (it commits, closes the file and exits) and starts its own. An older client never
  replaces a newer service.

**Deploy and update.** A hot-deploy swap, an MSIX update and the portable update are all refused while
any process of the package runs (`0x80073D02`). MCP servers live as long as the claude session that
started them, so they would pin the package for hours. The service binary and `duckdb.dll` are
therefore **copied out of the package** into `<profile>\sessiondb\bin\` and run from there:

- the GUI copies them at engine init when the copy's version differs;
- a running copy is renamed (Windows allows renaming a running executable), and the new one takes the
  stable name;
- the MCP command registered in Claude Code names that stable path (§8.4).

### 4.3 Files

| Path | What |
|---|---|
| `<profile>\sessiondb\sessions.duckdb` (+ `.wal`) | The database. |
| `<profile>\sessiondb\tmp\` | DuckDB spill files. |
| `<profile>\sessiondb\bin\` | The copied-out service (`agentmaster-sessiondb.exe`, `duckdb.dll`). |
| `<profile>\sessiondb\sessiondb.log` | The service's log (`[sessdb]` lines). Lines that tell a GUI story also go to `hooks.log`. |

## 5. Data model

### 5.1 Principles

- **Integer keys** in the big tables (`sid_key INTEGER`, `doc_id UINTEGER`); the session `UUID` appears
  once, in `session`.
- **Clustering instead of ART indexes.** DuckDB prunes row groups by min/max zonemaps; a table appended
  in term order is as good as an index for range lookups, and costs no memory. No `PRIMARY KEY` on
  multi-million-row tables.
- **Text compression per column.** Large strings are not compressed by default: the 270 MB sample took
  329 MB with the default methods, and **70 MB with ZSTD** forced for the whole database (3.9×; 73 MB with
  the per-column `USING COMPRESSION zstd` clause, which works in 2.0), at 2.6× the scan cost (372 vs
  144 ms). So the full text is ZSTD (it is read only for the few blocks a search verifies
  or shows), and the small hot columns stay `DICT_FSST`.
- **Fork copies are stored once.** A block's text belongs to the first session that wrote its `uuid`;
  the forks that copied it get a row in `doc_copy`, so a search still lists them (as today, where
  ripgrep finds the text in the fork's own file).

### 5.2 Tables

```sql
-- Schema version, tokenizer version, build provenance.
CREATE TABLE meta (key VARCHAR PRIMARY KEY, value VARCHAR);

-- Every source file and its ingest cursor.
CREATE TABLE src_file (
    file_id        INTEGER PRIMARY KEY,
    path           VARCHAR NOT NULL,
    kind           UTINYINT NOT NULL,   -- 0 transcript, 1 subagent, 2 spilled tool output, 3 history.jsonl
    sid_key        INTEGER,             -- the owning session (the parent for subagents and spills)
    agent_id       VARCHAR,             -- a subagent's id
    size_bytes     BIGINT, mtime_ms BIGINT, birth_ms BIGINT,
    parsed_bytes   BIGINT,              -- complete lines consumed so far (the resume cursor)
    head_hash      UBIGINT,             -- hash of the first 4 KB: a replaced file reads as changed
    missing_since  BIGINT               -- set when the file disappeared (D7)
);

-- One row per session: the browse list, the sidecar's TranscriptStats, places, versions, cost, usage,
-- outcomes, lineage. ABRIDGED: §5.6 lists every column with its source and rule.
CREATE TABLE session (
    sid_key        INTEGER PRIMARY KEY,
    session_id     UUID NOT NULL UNIQUE,
    file_id        INTEGER,
    project_dir    VARCHAR,             -- the encoded folder name (lossy, never decoded)
    cwd            VARCHAR,             -- the first line's cwd as written (the truth)
    cwd_key        VARCHAR,             -- NormDirKey(cwd): equality and grouping, identical to the GUI's key
    cwd_fold       VARCHAR,             -- lower-case, '/'-separated: the slash-insensitive folder SEARCH
    cwd_last VARCHAR, cwd_last_key VARCHAR, cwd_count SMALLINT,
    inferred_dir VARCHAR, inferred_dir_key VARCHAR,   -- InferWorkingDirectory, computed by the service (D15)
    git_root_key VARCHAR, color_key VARCHAR, home_launch BOOLEAN,
    branch_first VARCHAR, branch_last VARCHAR,
    version_first VARCHAR, version_last VARCHAR, models VARCHAR[], model_last VARCHAR, effort_last VARCHAR,
    created_ms     BIGINT,              -- fork-aware (a fork: the file's birth time)
    last_activity_ms BIGINT,            -- the last non-sidechain user/assistant timestamp
    last_typed_ms BIGINT, last_turn_end_ms BIGINT,
    custom_title   VARCHAR, ai_title VARCHAR, legacy_summary VARCHAR, first_prompt VARCHAR,
    forked_from    UUID, fork_root UUID, fork_point_line BIGINT, plan_parent UUID,
    user_prompts   INTEGER, assistant_lines INTEGER, tool_uses INTEGER, subagents INTEGER,
    context_tokens BIGINT,              -- the newest turn's usage sum, or a compaction's postTokens
    in_tokens BIGINT, out_tokens BIGINT, cache_read_tokens BIGINT, cache_write_tokens BIGINT,
    cost_usd DOUBLE, api_ms BIGINT, tool_ms BIGINT, lines_added INTEGER, lines_removed INTEGER,  -- cost-state
    model          VARCHAR, cc_version VARCHAR, size_bytes BIGINT,
    -- … the rest of §5.6: outcomes, work done, more titles, Agentmaster's facts
    transcript     UTINYINT,            -- 0 present, 1 deleted by Claude's sweep (D7), 2 replaced
    first_doc_id   UINTEGER, last_doc_id UINTEGER  -- the session's block range after the last re-cluster (§6.8)
);

-- The summary box of every session (§8.5), one row each, upserted for the sessions a refresh tick
-- named: the database's OWN summary, for its consumers (the AI search's tools, the MCP `session` tool,
-- the CLI) — one point query, ~3 ms. The app's summary surfaces do not read it (decided 2026-10-14).
CREATE TABLE summary (
    sid_key INTEGER PRIMARY KEY,
    prompts STRUCT(seg SMALLINT, kind UTINYINT, text VARCHAR, uuid UUID)[],  -- the numbered list, earlier segments first
    files_created VARCHAR[], files_edited VARCHAR[], files_read VARCHAR[],   -- display labels (SeSummaryFileLabel)
    skills VARCHAR[], tasks_done SMALLINT, tasks_open SMALLINT,
    plan_exit BOOLEAN, plan_content BOOLEAN, plan_file VARCHAR, plan_reads SMALLINT,
    recap VARCHAR, compactions SMALLINT,
    first_ts VARCHAR, last_ts VARCHAR, last_user_ts VARCHAR, branch VARCHAR
);

-- THE LOSSLESS STORE (§5.5): every line of every transcript (top-level and subagent), verbatim.
-- line_id = file_id << 32 | line_no: ascending in file order, so one file = one id range, and
-- string_agg(raw, chr(10) ORDER BY line_no) over a file gives the file back byte for byte.
CREATE TABLE line (
    line_id   BIGINT NOT NULL,
    file_id   INTEGER NOT NULL,
    line_no   INTEGER NOT NULL,
    uuid      UUID,                     -- the line's own uuid (NULL for state lines)
    type      VARCHAR,                  -- user, assistant, attachment, progress, mode, last-prompt, ...
    ts_ms     BIGINT,
    bytes     INTEGER,
    parent_uuid UUID,                   -- the line's parentUuid: the edge the live-branch walk follows
    leaf_uuid UUID,                     -- a leaf marker's leafUuid (last-prompt lines); a compaction
                                        -- root's other edges come from its own raw line (few)
    on_branch BOOLEAN,                  -- on the live branch (ActiveBranchUuids, run in C++ at ingest);
                                        -- rewritten for the session when a rewind moves the leaf (§8.5)
    seg       SMALLINT,                 -- the /compact segment by position (boundaries before it, itself included)
    is_copy   BOOLEAN,                  -- its uuid was written earlier: a fork copy, or a legacy re-append
    raw       VARCHAR USING COMPRESSION zstd   -- the line exactly as written, without its newline
);
-- Spilled tool outputs (<sid>/tool-results/*) and other per-session side files, one row per file.
-- BLOB, not VARCHAR: some spills are not valid UTF-8 (the prototype's text reader refused them).
CREATE TABLE side_file (
    file_id   INTEGER NOT NULL,         -- src_file row (kind: spill, subagent meta, ...)
    bytes     BIGINT,
    content   BLOB USING COMPRESSION zstd
);

-- Every searchable block, cold (merged into the index). Its text, ZSTD.
CREATE TABLE doc (
    doc_id      UINTEGER NOT NULL,      -- dense; renumbered in (session, line) order at each re-cluster
    sid_key     INTEGER NOT NULL,
    line_id     BIGINT NOT NULL,        -- the line the block comes from (its raw JSON is in `line`)
    uuid        UUID, block USMALLINT,  -- the line's uuid and the block's index in it
    ts_ms       BIGINT,
    role        UTINYINT,               -- 1 typed prompt, 2 reply, 3 thinking, 4 tool call, 5 tool result,
                                        -- 6 system, 7 queued prompt (queued_command, human), 8 subagent brief/report
    tool        VARCHAR,                -- the tool's name (calls, and results through tool_use_id)
    tool_use_id VARCHAR,                -- pairs a call with its result
    flags       UTINYINT,               -- meta | noise | sidechain | subagent | error | compact summary
    text_len    INTEGER,                -- the length before any cap
    text        VARCHAR USING COMPRESSION zstd
);
CREATE TABLE doc_copy (sid_key INTEGER, doc_id UINTEGER);  -- a fork also holds this block

-- The typed-prompt tier (the 👤 scope), small and pre-folded: scanned, never indexed.
CREATE TABLE prompt (doc_id UINTEGER, sid_key INTEGER, ts_ms BIGINT, kind UTINYINT, text_fold VARCHAR);

-- The inverted index over doc.text (§6.3, §6.4).
CREATE TABLE dict  (term_id UINTEGER, term VARCHAR, df UINTEGER, sf UINTEGER); -- block and session frequency
CREATE TABLE post  (term_id UINTEGER, doc_id UINTEGER, tf USMALLINT);           -- appended in term order
CREATE TABLE spost (term_id UINTEGER, sid_key INTEGER, hits UINTEGER);          -- term -> sessions

-- Blocks written since the last merge: not in post yet, scanned. Uncompressed and pre-folded, so the
-- scan needs no lower() and no decompression.
CREATE TABLE doc_hot (
    doc_id UINTEGER NOT NULL, sid_key INTEGER NOT NULL, line_id BIGINT NOT NULL,
    uuid UUID, block USMALLINT, ts_ms BIGINT, role UTINYINT, tool VARCHAR, tool_use_id VARCHAR,
    flags UTINYINT, text_len INTEGER, text VARCHAR, text_fold VARCHAR
);
-- New lines wait here the same way until a merge appends them to `line` sorted by line_id (§6.8).
CREATE TABLE line_hot (
    line_id BIGINT NOT NULL, file_id INTEGER NOT NULL, line_no INTEGER NOT NULL,
    uuid UUID, type VARCHAR, ts_ms BIGINT, bytes INTEGER, parent_uuid UUID, leaf_uuid UUID,
    on_branch BOOLEAN, seg SMALLINT, is_copy BOOLEAN, raw VARCHAR
);

-- Tool-touched paths: the 📁/📄 scopes, "who touched this file", the inferred folder.
CREATE TABLE path_access (
    sid_key INTEGER, path VARCHAR, dir_fold VARCHAR, leaf_fold VARCHAR,
    op UTINYINT,                        -- read, edit, write, search, shell (mined from a command)
    n INTEGER, first_ms BIGINT, last_ms BIGINT
);

-- One row per API message (Claude Code writes one assistant line per content block with the same
-- message id and usage: deduplicated by message id + request id, as ccusage does).
CREATE TABLE usage (sid_key INTEGER, ts_ms BIGINT, model VARCHAR, message_id VARCHAR,
                    in_tok BIGINT, out_tok BIGINT, cache_read BIGINT, cache_write BIGINT);

-- Typed prompts from history.jsonl: they outlive a swept transcript.
CREATE TABLE history_prompt (session_id UUID, ts_ms BIGINT, project VARCHAR, text VARCHAR, text_fold VARCHAR);

-- Agentmaster's facts, mirrored for joins. The sources stay authoritative (G6).
CREATE TABLE am_title    (session_id UUID PRIMARY KEY, title VARCHAR);       -- session-store "title"
CREATE TABLE am_favorite (session_id UUID PRIMARY KEY);                      -- session-store "favorite"
CREATE TABLE am_tag      (session_id UUID, tag VARCHAR, tag_fold VARCHAR);   -- session-store "tags"
CREATE TABLE am_link     (session_id UUID, from_id UUID, via VARCHAR);       -- sessions.json continuedFromId / Via
```

Runtime state stays out of the database. The GUI tells the service which sessions are open
(`openChanged`, §9), so the tools' `open` filter works for every client; the service holds that set in
memory and never stores it. Presence and the hidden-sessions list (a view preference in
`settings.json`) stay in the GUI, which applies them to the rows it gets back.

### 5.3 Views (the SQL and MCP surface)

`v_sessions` (display title by the page's precedence: stored title > custom > AI > legacy summary >
first prompt; the §5.6 columns a question needs, and the macro `effective_dir_key(mode)`), `v_messages`
(`doc` ∪ `doc_hot` with role names), `v_lines` (`line` ∪ `line_hot`: the raw JSON of every line, for
`json_extract` over fields no other view has), `v_paths`, `v_usage`, `v_links`. The read-only SQL tool
sees only these views (§10); their columns are listed in the `sql` tool's contract (§8.4).
⚠ Read JSON with `json_extract_string` / `from_json`, never with `->` chains (§3.5).

### 5.4 Size for today's corpus

| Part | Measured or derived from | Size |
|---|---|---|
| `line` + `side_file`: every line verbatim + the spilled outputs (the lossless store, D11) | measured: 10.42 GB of JSONL and 0.33 GB of spills → 2.22 GB (4.7×) | **2.2 GB** |
| Block text, deduplicated, full length, ZSTD | 1.35 GB capped at 16 KB → ~350 MB at the measured 3.9× | ~0.4 GB |
| `post` (75.6 M rows) + `dict` (0.9 M terms) | measured: 317 MB for the clustered postings | ~0.35 GB |
| `spost` (10.3 M rows) | measured: ~41 MB | ~0.04 GB |
| `prompt`, `session`, `path_access`, `usage`, the rest | estimated | < 0.1 GB |
| ART indexes (in memory, §6.8) | measured: ~13–16 bytes per indexed row | ~60 MB of RAM |
| **Total** | | **~3.1 GB on disk** (~0.9 GB without the lossless store) |

The prototype's unoptimized files were 3.3 GB (raw staging, uncompressed text) and 2.0 GB (the `fts`
extension's index over uncompressed text); §14 lists them.

### 5.5 Rebuilding a transcript: the lossless store

**The question:** can the database give a session back exactly as Claude Code wrote it?

**The search projection alone cannot.** `doc` keeps the text of each block and a few envelope fields.
It drops:

- every line with no searchable text: `attachment` (1.30 GB of today's corpus), `progress` (0.71 GB),
  `file-history-snapshot`, `last-prompt` (the leaf marker that decides the live branch), `mode`,
  `permission-mode`, `queue-operation`, the title lines, and line types the classifier does not know yet
  (`atis-latch`, `cost-state` and `file-history-delta` are in today's corpus);
- most of the envelope: `parentUuid` (the conversation tree), `cwd`, `gitBranch`, `version`, `slug`,
  `promptId`, `requestId`, `message.id`, `stop_reason`, `isApiErrorMessage`, `compactMetadata`, the
  thinking signatures, the structured `toolUseResult`, images;
- the JSON's own bytes and key order.

That is enough to read a conversation (the Copy ▸ Transcript levels), not to rebuild the file.

**The lossless store** (`line`, `side_file`; §5.2) keeps every line as written:

- one row per line, the raw text ZSTD-compressed, `line_id = file_id << 32 | line_no`;
- a rebuild is one query: `SELECT string_agg(raw, chr(10) ORDER BY line_no) FROM line WHERE file_id = ?`
  (the empty piece after the final newline is a row too, so the trailing newline comes back);
- a session = its top-level file, its subagent files and their `.meta.json`, and its spilled outputs.

**Measured on the full corpus** (`tools/sessiondb-bench/raw_lines.py`):

| | |
|---|---|
| Lines stored | 3,308,864 (top-level and subagent files), 10.42 GB of JSONL |
| On disk with ZSTD | **2.22 GB**, 2,800 spilled outputs (0.33 GB) included: **4.7×** |
| Ingest (DuckDB `read_text`, 4 threads) | 232 s |
| Byte-exact rebuild | **20 of 20** sample files (the 5 largest and 15 random) identical by SHA-256 |
| Rebuild time | 128.7 MB in 609 ms, 102.3 MB in 638 ms, a 304-line session in 2.9 ms (sorted, §6.8) |

Raw volume per line type: `user` 5.07 GB (tool results travel in user lines), `assistant` 3.18 GB,
`attachment` 1.30 GB, `progress` 0.71 GB, `file-history-snapshot` 0.06 GB, every other type under
0.03 GB.

**Fork copies are stored as they are.** 36.4% of the lines that carry a `uuid` are copies (the same
`uuid` in another file): 23.4% of the raw bytes. A copy is not byte-identical to its original: in a
sample of 115 copies, 4 were; the others differ in envelope fields (`sessionId` 59 times, `agentId` 47,
`slug` 24, `promptId` 18, `cwd` 9, `gitBranch` 3, `version` 3). Storing a copy as "the original plus its
changed fields" would need a re-serialization that reproduces Claude's exact bytes. So version 1 stores
every line. A later "verified reference" (keep a copy as a reference only when rebuilding it from the
original reproduces it exactly, checked when it is written) could save up to ~23%, about 0.5 GB.

**What it enables**

- **D7:** a transcript that Claude's sweep deleted can be written back to its folder and resumed with
  `claude --resume`: its lines, subagent files and spilled outputs are all there.
- **A stronger G6:** a parser upgrade (a new line type, a fixed rule) re-derives `doc`, `session`,
  `path_access` and `usage` from the stored lines, without the files.
- **The summary panel** reads a precomputed row instead of its file (§8.5); later the transcript overlay
  and the Copy ▸ Transcript levels can read parsed rows too (§8.6).
- **SQL over any field:** `v_lines` plus `json_extract`.

**Not covered: side folders outside `projects/`.** `file-history/` (2.07 GB, 62,708 files: the pre-edit
snapshots that `/rewind` restores; D12), `paste-cache/` (7 MB), `todos/`, `tasks/`, `plans/` (small).
`debug/` (3.2 GB) and `shell-snapshots/` (0.47 GB) are logs and environments, not session content.

**Caveats.** The prototype read the spills as text, and DuckDB's `read_text` refused 4 of its 11
batches (files that are not valid UTF-8), so the store keeps spills as BLOB. A live transcript keeps
growing, so a rebuild equals the bytes stored so far; the check compared exactly those bytes.

### 5.6 The session row: every column, and where it comes from

**The question** (2026-10-12): which columns should a session carry? For example, the working
directory and the inferred directory as normalized paths.

**What the transcripts carry.** Measured by `field_census.py` over the newest 3.0 GB (332 sessions,
633,905 lines, Claude Code 2.1.123–2.1.291), plus `rg` counts over all 4,444 top-level transcripts on
disk. A value that changes inside a session needs a first / last pair or a list:

| A session … | Share |
|---|---|
| changes `cwd` (the shell's `cd`; fdefc0e8 visits 31 folders) | 47.6% |
| changes model | 28.3% |
| changes Claude Code version (resumed after an update) | 8.7% |
| changes git branch | 6.9% |
| changes permission mode | 0.3% |
| has a `/compact` boundary · an idle recap · a message typed while Claude worked | 45.5% · 67.2% · 52.1% |
| hit an API error · was interrupted · pasted an image | 10.5% · 8.7% · 2.4% |
| carries more than one leaf marker (one per turn; rewinds hide among them) | 96.7% |

Line kinds that carry session facts. The lossless store keeps them all; ⚠ the classifier
(`ClassifyTranscriptLine`) extracts none of these facts today.

| Kind | Carries | Sessions |
|---|---|---|
| `cost-state` | Claude Code's own running totals: `totalCostUSD`, API time with and without retries, tool time, `totalLinesAdded` / `totalLinesRemoved`, wall time, per-model usage with its `costUSD`, `hasUnknownModelCost` (a model it cannot price, such as a preset's) | 562 of 4,444 (written since ~2.1.225) |
| `worktree-state` | `worktreePath`, `worktreeName`, `worktreeBranch`, `originalCwd`, `originalBranch`, `originalHeadCommit` | 14 |
| `relocated` | `relocatedCwd`: the session moved to another folder | rare |
| `agent-name` | `agentName` | 22 |
| `frame-link` | an artifact the session published: `path`, `frameUrl`, `title` | 25 |
| attachment `environment` | `workingDirectory`, `isWorktree`, `isGitRepo`, `additionalWorkingDirectories`, `platform`, `shell`, `osVersion`, `scratchpadDirectory` | newer sessions |
| attachment `model` | `modelId`, `marketingName`, `knowledgeCutoff` | newer sessions |
| system `stop_hook_summary` | each Stop hook's command and `durationMs`, its errors | most |
| system `turn_duration` | `durationMs` and `messageCount` per turn | most |
| `mode`, `permission-mode` | `normal` / `plan`; `bypassPermissions` / `plan` / `default` | most |
| `file-history-delta`, `atis-latch` | the file-history tracking of each edited file; an internal flag | — |

Envelope fields, per line:

- **Assistant lines:** `effort` (`max` / `high`), `perTurnEffort`, `requestId`, `attributionSkill` (29% of
  lines), `attributionMcpServer` / `attributionMcpTool` (6%), `thinkingDurationMs` (14%),
  `isApiErrorMessage` + `apiErrorStatus`, `isAbortedMidStream`.
- **`message.usage`:** input, output, cache read, cache creation (split 5 min / 1 h), `server_tool_use`
  (web searches and fetches), `service_tier`, `speed`, `inference_geo`.
- **`stop_reason`** includes `refusal` (99 of 228,907 assistant lines) and `max_tokens`.
- **User lines:** `promptId`, `promptSource` (`typed` / `system`), `turnOrigin`, `origin.kind`, and the
  structured `toolUseResult` (a Write's `type` create / update, an Edit's `structuredPatch`, a Bash
  call's stdout / stderr / interrupted).
- **Entry points:** `cli`, `sdk-cli`, `sdk-ts`.
- **Model ids** include preset models (`xiaomi/mimo-v2.6-flash`, `z-ai/glm-5.3-flash-uncensored`,
  `moonshotai/kimi-k3`) and `<synthetic>` (an API error line, never a model).

**Normalized paths: two forms per path.**

- **The KEY** is `NormDirKey` (`Persistence.h`). On Windows: `/` → `\`, trailing separators dropped (a
  drive root keeps its `\`), lower-case; on POSIX only the trailing `/` goes. The per-directory tab
  colors are keyed by it, and it agrees with `PathEq` / `NormPath`, which the Explorer tree groups with
  (case- and slash-insensitive on Windows, Rule #8). The service runs the same C++ function, so a key in
  the database equals the GUI's key for the same folder: `GROUP BY cwd_key` is the Explorer tree's
  grouping.
- **The FOLD** is lower-case with `/` separators, for the folder SEARCH (today's search is
  slash-insensitive; T1 in §6.2). Kept only where a search reads it (`cwd_fold`,
  `path_access.dir_fold` / `leaf_fold`).

**The columns** (about 120, plus Agentmaster's facts). Sources:

- **T** — the transcript, computed while the ingest classifies the lines;
- **S** — the service, with Agentmaster's own C++ (filesystem probes, graph walks);
- **A** — Agentmaster's facts, mirrored from their files (§7.1);
- **V** — a view or a macro, not stored.

Coverage is the share of NumSharp's 922 sessions where `session_columns.py` found the column set; "—"
means the prototype did not compute it.

*Places*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `cwd`, `cwd_key`, `cwd_fold` | T | the first line's cwd as written, its key and its fold | 96.5% |
| `cwd_last`, `cwd_last_key` | T | the last line's cwd (where the shell ended up) | 96.5% |
| `cwd_count` | T | distinct cwd keys | 96.5% (fdefc0e8: 31) |
| `project_dir` | T | the encoded projects folder the transcript is filed under (never decoded: the encoding is lossy) | 100% |
| `relocated_cwd`, `relocated_cwd_key` | T | the last `relocated` line | 0% |
| `worktree_path`, `worktree_key`, `worktree_name`, `worktree_branch`, `worktree_original_cwd` | T | the last `worktree-state` line | 0.5% |
| `env_is_git_repo`, `env_is_worktree`, `env_shell`, `env_extra_dirs` | T | the first `environment` attachment | 7.5% |
| `git_root`, `git_root_key` | S | `FindGitRootForDir(cwd)`: the nearest `.git` folder or file | — |
| `color_key` | S | the main repository root (`ResolveWorktreeMainRoot`) when the cwd is a worktree, else `cwd_key`: Rule #12's color key | — |
| `inferred_dir`, `inferred_dir_key`, `inferred_git` | S | `InferWorkingDirectory` over the session's `path_access` votes: machine TEMP excluded, the git snap per the `inferGitRoot` setting (stored with the result). Re-run when `path_access` grows (throttled, as `_ScanInferredTabColors` is) and for every session when the setting changes. Stored only when it differs from the cwd, like `SessionInfo::inferredWorkingDir` (D15). | — |
| `home_launch` | S | `cwd_key` equals the key of `%USERPROFILE%` (`SessionInfersWorkingDir`'s forcing rule) | — |
| `effective_dir_key(mode)` | V | a macro over the row: the inferred key when the tab-color mode is Inferred or the session is a home launch, else `cwd_key` — `EffectiveWorkingDir`'s rule, so the page and the tools group like the Explorer tree | — |
| `fs_checked_ms` | S | when the S columns were last probed: again when the session changes, and once a day (a repository can appear or vanish) | — |

*Identity, versions, models*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `session_id`, `short_id` | T, S | the file stem; the short id the tools print (§8.4.2) | 100% |
| `kind` | T | `claude` (Codex later) | 100% |
| `entrypoint` | T | the first line's (`cli` / `sdk-cli` / `sdk-ts`) | 72.7% |
| `branch_first`, `branch_last`, `branch_count` | T | the first and last `gitBranch`, and how many | 96.5% |
| `version_first`, `version_last` | T | Claude Code's version on the first and the last line | 96.5% |
| `models`, `model_last` | T | the distinct assistant models (not `<synthetic>`), and the last one | 94.9% |
| `effort_last` | T | the last assistant line's `effort` | 58.9% |
| `permission_mode_last`, `plan_mode_marks` | T | the last `permission-mode` line; the `mode` lines that read `plan` | 72.0%, 0% |
| `agent_name` | T | the last `agent-name` line | 0.9% |
| `preset`, `launch_model` | A | the session store's `launchArgs` (HARNESSES.md §7) | — |

*Times and turns*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `created_ms` | T, S | fork-aware: a fork's file birth time (§5.2) | 100% |
| `first_ms`, `last_ms` | T | the first and last live-branch timestamp (the summary's) | 96.3% |
| `last_typed_ms`, `last_turn_end_ms` | T | the last typed prompt and the last `end_turn` reply: the MOST RECENT sort's turn boundaries for sessions that are not open | 96.3%, 79.6% |
| `last_user_ms` | T | the summary's "last user message" (a typed prompt, an answered question, or a message typed while Claude worked) | — |
| `turns_timed`, `turn_ms_total`, `turn_ms_max` | T | the `turn_duration` lines | 91.4% |

*Cost and usage*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `cost_usd`, `api_ms`, `api_ms_no_retries`, `tool_ms`, `wall_ms`, `lines_added`, `lines_removed`, `cost_unknown_model` | T | the LAST `cost-state` line: Claude Code's own running totals (D13) | 18.4% (newer sessions) |
| `api_messages`, `in_tokens`, `out_tokens`, `cache_read_tokens`, `cache_write_tokens`, `web_searches`, `web_fetches` | T | summed per API message (`message.id`): Claude Code writes each content block of a response as its own line and repeats the response's usage on each one | 94.9% |
| `context_max`, `context_tokens` | T | the largest, and the newest, input + cache read + cache write of one message; a compaction's `postTokens` replaces the newest (the Ctx column) | 94.9% |

*Outcomes*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `api_errors`, `api_error_status_last` | T | `isApiErrorMessage` lines, and the last one's HTTP status | 9.0%, 1.7% |
| `refusals`, `max_token_stops`, `aborted_mid_stream` | T | `stop_reason` `refusal` / `max_tokens`; `isAbortedMidStream` | 0.3%, 0%, 0% |
| `interrupts` | T | `[Request interrupted …]` markers | 18.3% |
| `end_state` | S | how the last ingested turn ended: completed, cut off (`ClassifyCutOffTurn`), or waiting on a question — for sessions that are not open | — |
| `stop_hook_ms_total`, `stop_hook_ms_max` | T | the `stop_hook_summary` lines (fdefc0e8's slowest Stop hook took 32.4 s) | 65.3% |

*Work done*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `prompts_typed`, `prompts_queued`, `prompts_listed` | T | typed prompts; messages typed while Claude worked; the summary's numbered prompts | 96.3%, 18.7%, — |
| `assistant_lines`, `tool_calls`, `tool_calls_by_tool` | T | `tool_calls_by_tool` is a `MAP(VARCHAR, BIGINT)` (`histogram()`) | 94.9%, 93.4%, 93.4% |
| `skills_used`, `mcp_servers_used` | T | `attributionSkill` and Skill tool calls; `attributionMcpServer` | 27.5%, 5.0% |
| `subagents_spawned`, `subagent_files` | T | Task / Agent tool calls; the `subagents` side files | 7.4%, 7.2% |
| `files_read`, `files_created`, `files_edited` | T | the counts of the summary's three lists (the `summary` table holds the lists) | — |
| `images_pasted` | T | image blocks in user lines | 0.7% |
| `compactions`, `compactions_auto`, `compaction_pre_tokens_max` | T | `compact_boundary` lines and their `compactMetadata` | 12.0%, 6.2%, 12.0% |
| `recaps`, `recap_last` | T | `away_summary` lines (the summary's Recap) | 58.6% |
| `tasks_done`, `tasks_open`, `plan_exit`, `plan_file` | T | the summary's rules (the last TodoWrite; ExitPlanMode; a Write into `/plans/`) | — |
| `artifacts_published`, `artifact_last_title`, `artifact_last_url` | T | `frame-link` lines | 0.5% |

*Titles*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `custom_title`, `ai_title`, `legacy_summary` | T | the last line of each kind | 0.1%, 0.8%, 0% |
| `first_prompt`, `last_prompt`, `last_reply` | T | the first 200 characters of each: the fallback title and the row's preview | 96.3% (first) |
| `display_title` | V | stored title > custom > AI > legacy summary > first prompt (`PickDisplayTitle`'s order, Agentmaster's title first) | — |

*Lineage*

| Column | Src | Rule | Coverage |
|---|---|---|---|
| `forked_from`, `fork_point_line`, `fork_root` | S | at a file's first ingest: the file that first wrote the uuids it opens with (a uuid → first-file map, §8.5), and the last copied line | — |
| `plan_parent` | T | "read the full transcript at: <parent>.jsonl" in the first prompt | 0% in NumSharp |
| `continued_from`, `continued_via` | A | the `/clear` and `/handover` links (`sessions.json`, `am_link`) | — |
| `forks` | V | the sessions whose `forked_from` is this one | — |
| `leaf_uuid`, `off_branch_lines` | S | the live-branch walk at ingest; more than 0 means a rewind abandoned lines | — |

*Agentmaster's facts* (A, mirrored; the files stay the truth, G6): the stored title, favorite, tags,
hidden, the session's own tab color (Individual mode), the inferred folder the GUI last computed,
`launchArgs`, `managed` (known to `sessions.json`), the last window. Runtime state is never stored
(open, live state, presence, the unsent draft; §5.2).

*Bookkeeping* (`src_file`): size, modification and birth times, the parsed byte cursor, the head hash,
the line count, a torn last line, the classifier version, the ingest time.

**Per-line columns** (`line`, §5.2): `on_branch`, `seg` and `is_copy`, which the summary needs (§8.5).

**Measured** (`session_columns.py`: 73 of the T columns over NumSharp's 922 sessions, 3.11 GB of JSONL
in the line store): one `from_json` parse per line, 6.7 s; the columns, 4.8 s; **11.5 s in all, 269 MB/s
on 4 threads**. The C++ ingest computes them while it classifies, with no second pass.

## 6. Search

### 6.1 Today's semantics, kept

| Today (SESSIONS.md §1, `SessionSearch.h`) | In the database |
|---|---|
| Terms split on whitespace, **AND**: in the fast phase each term may hit a different field; in the content phase a message hits when **all** terms match **that message** | Same: session-level AND over title / folder / branch / paths / id (§6.2 T0–T1), message-level AND over prompts and blocks (T2–T3); the result is the union, as today. |
| Case-insensitive substring | Folded text (`lower`, the same per-character fold as `FoldLower`); substring through `contains` on scanned tiers, and through the dictionary on the index (§6.3). |
| `"quoted phrase"` = one exact contiguous term | A term with separators (§6.3, verified). |
| A whole GUID also matches the session's identity (its id or fork-parent id) | `session_id = ?` or `forked_from = ?`; in the content tiers it limits hits to that session. |
| `(F)` fuzzy = the term's characters in order with any gap | T0–T2 (scans): a registered scalar function `am_subseq(text, needle)` with exactly `MatchesQueryText`'s rule. T3: D9 decides between the same rule by scan (~0.5–2 s) and typo-tolerant term matching over the dictionary (fast). |
| Scopes 🏷 👤 🤖 📁 📄 | 🏷 → T0, 📁 📄 → T1, 👤 → T2 (+ `history_prompt`), 🤖 → T3 (roles 2–6, 8; subagents and spilled outputs included). |
| The time window filters by last activity | `session.last_activity_ms` between the bounds. |
| Row filters (folder, branch, day / week / month, fork family, tags, favorites, open, hidden) | Applied by the GUI over the rows, as today (the open set and the hidden list are runtime state); the same filters are request parameters for the AI and MCP tools. |
| Relevance tiers when sorted by Active: name > path > content | Same tiers (Name, Path, Prompt, Text), computed by the service; BM25 inside the content tiers for the tools. |

### 6.2 The tiers

| Tier | Data | How | Measured |
|---|---|---|---|
| **T0** identity and metadata | `session` (3,570 rows) + `am_title` + `am_tag` | Per term: `contains` on the folded fields; a GUID by equality. | 4–8 ms list; 5 ms per term |
| **T1** touched paths (📁 📄) | `path_access` (98 K distinct session/path/tool rows today) | `contains` on `dir_fold` / `leaf_fold` (`/`-normalized, so the folder search stays slash-insensitive) | 26 ms (with `lower()` per row; pre-folded columns are cheaper) |
| **T2** typed prompts (👤) | `prompt` (23 K rows, 15.5 MB) + `history_prompt` | Scan with `contains` per term, AND per message, hits counted per session | **14–37 ms** clustered and pre-folded (186–273 ms when the prompts sat among 1.7 M blocks and were folded per row) |
| **T3** agent and tool text (🤖) | `doc` via `dict` / `post` / `spost`, plus `doc_hot` | The index plan (§6.3) on cold blocks; a scan of `doc_hot` | **2–33 ms** for single terms, 20 ms in-word substring, 19.5 ms AND of three terms; 32 ms for 24 h of hot text |

Queries of 1–2 characters skip T3 by default and say so (D8): the one-letter queries in today's log
matched 2,300–2,500 sessions, and expanding a 1-letter term over the dictionary reaches nearly every
block.

### 6.3 The index plan (T3)

The tokenizer splits folded text on everything that is not a Unicode letter (with its combining
marks), digit or `_`, and adds the parts of `camelCase` and `snake_case` identifiers as extra tokens
(`_RenderSessionsTable` → the token itself plus `render`, `sessions`, `table`). Every token is indexed,
whatever its length: dropping long ones (base64, hashes) would break rule 1 below, and they are few
(3,458 of the 909 K terms are longer than 64 characters).

For each text term `t`:

1. **`t` has no separator** (only letters, digits, `_`): a block contains `t` exactly when one of its
   tokens contains `t`, because a match cannot cross a separator. So the dictionary terms that contain
   `t` give **the exact answer, with no verification**. The service holds the dictionary in memory
   (909 K terms, 11 MB of term text), so the expansion is a C++ substring scan (or a trigram map over
   the terms) instead of the 11–17 ms table scan the prototype measured.
2. **`t` has separators** (`up-arrow`, `K:\source`, `"tab drag"`): its first token must be a token's
   suffix, its middle tokens must be whole tokens and its last token a token's prefix; the blocks that
   have all of them are **candidates**, and each candidate is **verified** with `contains(lower(text), t)`
   (only those blocks are decompressed). A term made only of separators (`::`, `->`) has no token: it is
   verified on the other terms' candidates, or, alone, answered by a time-bounded scan.
3. **Rarest first.** The terms run in order of block frequency (`df`); each later term only keeps blocks
   the earlier ones kept. A message-level AND therefore costs about the rarest term's postings.
4. **Session-only answers use `spost`.** A single-term query that only needs the list of sessions and
   their hit counts reads `spost` (term → sessions, with the number of blocks): 1.2–2.1 ms, even for a
   word in 575 K blocks.
5. **The hot tier.** `doc_hot.text_fold` is scanned with the same AND rule and the results are added.

**Measured** (full corpus, 4 threads, term ids known):

| Query | `post` (term → blocks) | `spost` (term → sessions) |
|---|---|---|
| `duckdb` (2,371 blocks, 165 sessions) | 1.9 ms | 1.2 ms |
| `release` (49,006 blocks, 1,784 sessions) | 19.7 ms | 1.6 ms |
| `the` (574,971 blocks, 2,815 sessions) | 223 ms (every block id returned) | 2.1 ms |
| `str`: a 3-letter substring, 14,149 dictionary terms, 2,665 sessions | distinct blocks 39.5 ms; exact blocks per session 171 ms | 25.6 ms (hit counts summed per term: a block that holds two of the terms counts twice) |

The broad 3-letter substring is the practical worst case. For it, the page shows the `spost` answer
(the exact set of sessions) at once and the exact counts when they land.

### 6.4 The hot tier and the cold index (an LSM on DuckDB tables)

- **Ingest** appends new blocks to `doc_hot` (pre-folded, uncompressed): milliseconds, no index work.
  A day of activity is ~11 K blocks / 18 MB; scanning it takes 32 ms (a week: 120 K blocks, 186 MB,
  118 ms).
- **Merge** (every 30 minutes, or when `doc_hot` holds 64 MB, in one transaction): tokenize the hot
  blocks; append their postings to `post` and `spost` **sorted by term** (so each appended row group
  covers a narrow term range and stays prunable); move the rows into `doc` (ZSTD); update `dict`. Until
  the next re-cluster, `spost` can hold several rows for one (term, session) pair; a lookup sums them.
  The same merge moves `line_hot` into `line` sorted by `line_id`, so each merged batch holds every
  session's new lines in one run.
- **Re-cluster** (weekly, or after 50 merges; when idle; interruptible): rebuild `post` and `spost` in
  one sorted `CREATE TABLE … AS … ORDER BY term_id, doc_id`, renumbering `doc` in (session, line) order
  on the way, and rewrite `line` sorted by `line_id`. Measured: **15 s** for 75.6 M postings, about
  **5 s per GB** of raw JSON for the line store (§6.8).
- **Deletes** (a replaced or deleted file): the block ids go into a small tombstone set that every
  lookup filters out; the next re-cluster drops them.

### 6.5 Why our own index and not the `fts` extension (Option A)

Both were built over the same 1.06 M blocks (1.31 GB of folded agent and tool text).

| | `fts` extension, DuckDB 2.0 (Option A) | Own index in DuckDB tables (Option B, **recommended**) |
|---|---|---|
| Build | **214.5 s**: SQL tokenizer, 174 M occurrence rows, 0.9 M terms, 8.4 M trigram rows, 429 MB | Tokens produced by the C++ ingest; one sorted rebuild **15 s** for 75.6 M (term, block, count) rows (317 MB), plus **2.2 s** for `spost` (10.3 M rows, ~41 MB) |
| Query through the extension's macros | exact 57.8 ms, prefix 89 ms, substring 138.7 ms, `release` 121 ms | — |
| Query through its tables directly | dictionary 11 ms; postings 3.5 ms (rare) / 38.7 ms (`release`) / 1,417 ms (`the`) | postings **1.9 / 19.7 / 223 ms**: one row per (term, block), not per occurrence (2.3× fewer rows) |
| Incremental | **Works**: 26 statement-level triggers (13 run on each INSERT, 13 on each DELETE) keep the dictionary, the sidecars and the statistics current; a fixed cost per statement of ~0.6–0.9 s on a large index (0.9 s for 1 block, 2.2 s for 900 on the 20% index; 0.57 s for 100 on the full one; 0.12 s on a 2 K-block index) | The hot tier and merges (§6.4): appends in milliseconds |
| Tokenizer | `regex` or `opensearch_standard`; no `camelCase` / `snake_case` splitting | Ours: Unicode letters, digits, `_`, identifier parts, paths |
| Packaging on Windows | Not in `libduckdb` or vcpkg: ship `fts.duckdb_extension` (22.7 MB on 1.5, **46 MB** on the 2.0 alpha, per architecture), or build DuckDB from source with `fts` linked in (CI work for x64 and arm64 at every version bump) | The official `libduckdb` DLL as it is |
| Coupling | Fast queries read the extension's private tables (`dict`, `terms`, `docs`; the layout version is in `index_metadata`) | Our own schema |
| Extras for free | BM25F, Damerau-Levenshtein fuzzy expansion, prefix sidecars, boolean queries | BM25 in SQL over our postings; fuzzy over the in-memory dictionary (D9) |

The extension does what its README promises, and it may be the better choice once its tables are a
stable interface and `libduckdb` links it: D1 keeps it open. The comparison also covers the user's
"indexing required only once": with either option the index is built once and then **maintained**
incrementally; neither needs a periodic full rebuild to stay correct (Option B re-clusters only for
speed).

**Rejected outright:** brute force alone (1.6–2.4 s over the agent text with `lower()`, 0.38–0.51 s
pre-folded: far faster than today, but 10–50× slower than the index); the extension's `max_df_ratio` /
`term_limit` expansion caps (documented defaults 0.15 and 32) for a filter, because they drop common
expansions without saying so (a 3-letter query would then miss sessions).

### 6.6 Ranking, hit counts, snippets

- **Order:** the tiers (Name, Path, Prompt, Text), then the user's column sort (the page) or BM25 (tools):
  score = Σ idf(t) · tf · (k1 + 1) / (tf + k1 · (1 − b + b · len / avgdl)), from `post.tf`, `dict.df`
  and the block lengths.
- **Hit counts** = matched blocks per session (today: matched messages), copies through `doc_copy`
  included.
- **Snippets** only for rows on screen: the first 2–3 matched blocks of each, decompressed, cut by the
  existing `MakeSnippet`.

### 6.7 Latency budget (C++, prepared statements, a warm database)

| Step | Budget |
|---|---|
| Pipe round trip (GUI ↔ service) | < 0.5 ms (the measured `select 1` costs 0.3 ms inside DuckDB) |
| T0 + T1 | ≤ 15 ms |
| T2 | ≤ 40 ms |
| T3 (3+ characters), the list of sessions | ≤ 80 ms p95 (a broad 3-letter substring: 26 ms through `spost`) |
| T3 exact hit counts | ≤ 80 ms p95; the broad 3-letter substring ~170 ms (shown when they land) |
| Snippets for 30 rows | ≤ 10 ms |
| **First rows** (T0–T2 return first) | **≤ 50 ms** |
| **Complete answer** | **≤ 150 ms** (the broad 3-letter case: ~200 ms) |

The Python prototype's numbers include its client overhead; the C++ client should be at or under them.
**A newer request cancels the older one** (`duckdb_interrupt`), so typing never queues searches; the
page's 250 ms debounce can drop to ~80 ms.

### 6.8 Indexes: what to build, and why

**What DuckDB offers** (its documentation, read 2026-10-12):

- **Zonemaps:** a min/max per column per row group (122,880 rows), automatic for every column. They
  skip row groups only as well as the column is ordered.
- **ART** (adaptive radix tree): made by `PRIMARY KEY`, `UNIQUE` or `CREATE INDEX`. An index scan serves
  only a single-column equality or `IN` filter, and only when it matches fewer than MAX(2,048 rows,
  0.1% of the table). It does not help ranges, substrings, joins, aggregations or sorts. Its buffers
  stay in memory (never evicted under pressure), every write to the table pays for it, and it should be
  built after a bulk load.
  - ⚠ `EXPLAIN` prints "Sequential Scan" even when the index will be used: the choice is made at run
    time. `EXPLAIN ANALYZE` shows `Type: Index Scan` and `Row Groups Scanned: 0 / 1` (measured).
- **Extension indexes:** `fts` (§6.5, not used), HNSW in `vss` (vectors: a later semantic search),
  R-tree in `spatial` (not relevant).

**Measured** (`tools/sessiondb-bench/access_paths.py`: a 10% sample of the line store, 312,887 lines,
0.98 GB of raw JSON, the same rows in two physical orders; the access path from `EXPLAIN ANALYZE`):

| Read | Sorted by `line_id` | Interleaved (shuffled) | Interleaved + ART | Sorted + ART |
|---|---|---|---|---|
| A big session (19,224 lines, 82 MB) | 149 ms | 303 ms | 286 ms (sequential scan: above the ART threshold) | 164 ms |
| A small session (304 lines) | **2.9 ms** | 113 ms | 4.6 ms (index scan) | 5.2 ms |
| One line by id | 2.8 ms | 3.8 ms | 1.4 ms (index scan) | 1.3 ms |
| 30 random lines, `IN` list | 145 ms | 142 ms | **4.2 ms** (index scan) | **6.9 ms** |
| 30 random lines, join with a temporary table | 121 ms | 147 ms | 2.8 ms (index scan) | 4.7 ms |

A first run, under heavier load on the machine, measured 1.2–1.9× these times with the same ranking
(a small session 5.6 ms sorted against 193 ms interleaved; 30 random lines 198 ms without ART, 6.9 ms
with it).

- **ART cost:** 0.06–0.14 s to build each index on the 312,887 rows; 3.9–5.0 MB of memory each, so
  13–16 bytes per indexed row.
- **Re-cluster cost:** writing the 0.98 GB sample in another order took 2.2–4.8 s over two runs, so
  2–5 s per GB of raw JSON (~25–50 s for the whole line store; 15 s for the postings, §6.4).
- Without an index, 30 scattered ids run as a join over every row: zonemaps cannot prune scattered ids.
  That is the one read where ART pays.

**The rules that follow**

1. **Keep every large table sorted by its main read key.** New rows wait in a hot table and are merged
   in sorted batches; a periodic re-cluster rewrites the table in order. Zonemaps then prune range
   reads (a session, a term) for free, at no memory cost.
2. **ART only for batches of random point lookups by id**, on small key columns, where a lookup returns
   at most a few thousand rows: snippets and verification (30 random blocks), `messages` around a
   block, raw-line fetches.
3. **No ART for range reads, joins or substrings** (DuckDB's own limits), and none on the large tables'
   other columns.
4. **The text index is ours** (§6.3): postings sorted by term (zonemaps), the dictionary in the
   service's memory.

**The plan, table by table**

| Table | Rows today | Kept sorted by (zonemaps) | ART | Serves |
|---|---|---|---|---|
| `session` | 3.6 K | `sid_key` | `sid_key` (PK), `session_id` (UNIQUE, for upserts) | the list, id lookups, T0 |
| `src_file` | ~6 K | `file_id` | `file_id` (PK) | ingest cursors |
| `line` | 3.3 M | `line_id` (file, line): one session = one id range | `line_id` (~45 MB) | rebuilds, `messages`, raw fetches |
| `side_file` | ~4 K | `file_id` | — | rebuilds |
| `doc` | 1.1 M | `doc_id`, renumbered in (session, line) order at each re-cluster: one session = one id range (`session.first_doc_id` … `last_doc_id`) | `doc_id` (~15 MB) | snippets, verification, `messages`, `grep` |
| `doc_copy` | ~0.45 M | `doc_id` | — | fork hits |
| `post` | 75.6 M | (`term_id`, `doc_id`) | none: a term's postings exceed the ART threshold | T3 |
| `spost` | 10.3 M | (`term_id`, `sid_key`) | none | T3 session lists |
| `prompt` | 23 K | (`sid_key`, `ts_ms`) | none (a scan: 14–37 ms) | T2 |
| `path_access` | ~0.1 M | (`dir_fold`, `leaf_fold`): a folder = a prefix range | none | T1, `who_touched` (prefix), `files` (a scan) |
| `usage` | ~0.5 M (estimated) | `ts_ms` (append order) | none | `usage`, `timeline` |
| `history_prompt` | 18.5 K | `ts_ms` | none | T2 |
| `doc_hot`, `line_hot` | up to ~100 K | append order | none (a scan: 32 ms for a day of blocks) | the hot tier |
| `am_*` | small | the key | PK | joins |

ART memory in total: ~60 MB for today's corpus, held for the service's life.

**In the service's memory (not DuckDB indexes)**

- the dictionary: term → `term_id` (a hash), the terms sorted (prefix ranges), and a trigram → terms
  map (substring expansion without the 11–17 ms table scan, §6.3);
- each term's row range in `post` after a re-cluster, so a lookup reads exactly its rows;
- the short-id map: 8-character prefix → session id (the ids the tools print);
- each session's `doc_id` range and its hot tail.

**Later:** an HNSW index (`vss`) over embeddings of prompts or session summaries, for semantic search
(a non-goal of version 1).

## 7. Ingest

### 7.1 Sources

| Source | Into |
|---|---|
| `projects/*/<uuid>.jsonl` | `session`, `doc` / `doc_hot`, `prompt`, `path_access`, `usage`, `src_file` |
| `<sid>/subagents/agent-*.jsonl` (+ `.meta.json`) | the parent session's blocks, flag `subagent`, role 8 for a brief / report |
| `<sid>/tool-results/*.txt` | the tool result block of that `tool_use` id |
| `history.jsonl` | `history_prompt` |
| `<profile>\session-store\*.json` | `am_title`, `am_favorite`, `am_tag` (the session store's scan cache tells what changed) |
| `<profile>\sessions.json` | `am_link` (`continuedFromId`, `continuedVia`) |

### 7.2 One classifier, block by block

`ClassifyTranscriptLine` already holds the rules (noise prompts, meta and compact-summary lines, tool
results, `toolUseResult`, system content, titles, fork lineage, paths, usage). It concatenates a line's
blocks into one string; the database needs them apart. A block-level variant
(`ClassifyTranscriptBlocks`) is the one implementation, and the sidecar path and the database both use
it, so the two can never disagree. Additions: the human `queued_command` prompts
(`SeQueuedHumanPromptRawText`'s rule, SESSIONS.md §3), tool names on results (through `tool_use_id`),
usage deduplicated by message id, and no duplicate of a tool result through `toolUseResult`. It also
reads the line kinds and envelope fields the session row needs (§5.6: `cost-state`, `worktree-state`,
`relocated`, `agent-name`, `frame-link`, the `environment` attachment, `effort`, `stop_reason`, the
Stop-hook times), and the ingest runs `ActiveBranchUuids` per changed session for `line.on_branch`
(§8.5).

### 7.3 The cold build

- **Newest sessions first**, so the page is usable while older ones load; the service publishes progress
  and coverage (`indexed back to <date>`), the way the AI search's catalog does today.
- **4 parse workers** (background priority) classify and tokenize; **one writer** appends through the
  appenders and builds the in-memory dictionary; at the end one sorted build of `post` / `spost` (15 s)
  and a checkpoint.
- **Estimate: about 2 minutes** for today's 10.4 GB: today's parser did 3,600 sessions (8.26 GB) in
  3 minutes on one thread (46 MB/s, `SessionSearchTools.h`), so ~60 s on 4 threads, plus tokenizing,
  appending and the 15 s sort. To measure in P0. (DuckDB's own JSON reader in the prototype: 379 s,
  27 MB/s on 4 threads.)

### 7.4 Incremental

- **The trigger** (decided 2026-10-14, D16): the app's observer counts each session's changes (its
  `TranscriptWatcher`'s change sequences), and every refresh tick (default 5 s) it sends the sessions
  that changed (`upsertSessions`), or a `resync` after a connect or a re-index (§7.6).
- Per changed session: read its files from their cursors (`parsed_bytes`) to the last complete line;
  classify; append to `doc_hot`, `prompt`, `line_hot`; upsert the `session`, `summary`, `usage` and
  `path_access` rows; advance the cursors; **one transaction** per request.
- From a line written to it being searchable: **one refresh period** (default 5 s) plus the tick
  (~20–30 ms).
- **The live branch and the summary** (§8.5): new lines normally extend the live branch (they descend
  from the current leaf), so they are `on_branch`. A new leaf marker that does not descend from the old
  leaf is a rewind: that session is walked again and its `on_branch` flags rewritten. The `summary` rows
  of the sessions in the request are upserted in the same transaction.

### 7.5 Correctness rules

- **Append-only, mostly.** `size < parsed_bytes`, or a changed `head_hash` ⇒ the file was replaced:
  tombstone its blocks and ingest it again from 0.
- **A partial last line is never consumed** (the cursor stops at the last newline); a line over the 20 MB
  read cap is stored cut (the largest seen is 18.5 MB).
- **Forks and legacy resumes** (≤ 2.1.147 re-appended whole histories): the same `(uuid, block)` again ⇒
  a `doc_copy` row, not a second copy of the text.
- **`/cd` moves a transcript** to another project folder: same session id ⇒ update `src_file.path`, keep
  the rows.
- **Claude's sweep deletes a transcript** (after `cleanupPeriodDays`): D7.
- **Never lose a swallowed exception** (Rule #18): the service reports through the same
  `LogSwallowedException` chokepoint into its log.

### 7.6 Staying current: the app's observer counts changes, a refresh tick upserts

**The questions** (2026-10-13): does the database stay current by itself, from events in the
background, or does it belong to the Sessions page and get rebuilt when the page re-indexes? Can the
changes come from the observer? Must the data be 1–2 s behind real time, or is 30–60 s enough?

**Decided by the user (2026-10-14):** "Duckdb will be refreshed by counting changes from the observer
and if we had changes within the refresh rate range then we update or rather upsert." And for the
summary: "I want the duckdb to be capable of forming a summary but I don't want summary to use duckdb"
(§8.5).

**In short:**

- **The app's observer counts each session's changes.** Every refresh tick (a setting, default 5 s),
  the sessions whose count moved since the last tick are upserted; a tick without a change does
  nothing.
- **Never rebuilt for a search or a page open.** Full rebuilds happen only in the three cases of
  §7.6.5.
- **Freshness = the refresh period** (default 5 s) plus the tick itself (~20–30 ms). That is enough for
  every consumer, now that the summary surfaces stay on the C++ analyzer (§7.6.2).
- The 2026-10-13 version of this section recommended the service's own watcher (A below); the user
  chose the observer (B).

#### 7.6.1 Where the changes come from (D16, decided)

What the app's engine knows today (read from the code):

- **`TranscriptWatcher`** (one per app process, in the shared engine): one `ReadDirectoryChangesW` on
  `~/.claude/projects`, subtree on, notified ~0.4 ms after Claude's write. It keeps an index (session
  id → path) and per-session CHANGE SEQUENCES that the scanner polls for its live sessions (`Stamp(id)`),
  and it wakes the scanner (coalesced, 100 ms). It publishes no list of changed files.
- **The scanner** reads the new lines of LIVE sessions only, and keeps state facts (turns, stop reasons,
  the pending interactive tool, recaps), not the content.
- **The Fleet Observer** knows every running claude and its session (the census and Claude's presence
  files, `~/.claude/sessions/<pid>.json`): exactly the transcripts that can grow right now (49 of 12,749
  files while this was measured).

So the app knows WHEN and WHICH files change, but not what they hold. Whatever wakes it, the service
reads and classifies the new bytes itself: the database keeps every line, the scanner a few facts of
live sessions.

**The design that follows the decision:**

- **The counting.** The app's `TranscriptWatcher` already bumps a per-session change sequence for every
  write to a session's transcript and side folder (subagent transcripts, spilled outputs). The tick
  needs the sessions whose sequence moved since the previous tick: a small addition,
  `TakeChangedSessions()`, returns those ids and clears them, plus an `epochChanged` flag when the
  watcher re-indexed everything. It counts every session under `projects/`, managed or not, while the
  app runs.
- **The tick** (the app's engine thread, never the UI thread), every refresh period:
  - no changed session → nothing, not even a request;
  - otherwise ONE `upsertSessions` request: each changed session's id and transcript path (the index
    knows it);
  - the response carries each session's new cursor; a failed request keeps its ids for the next tick.
    The counts only say "look again", and the cursors say where, so nothing is lost or read twice.
- **The belts:**
  - at the app's (re)connect, and whenever its watcher re-indexes (the epoch moved: an overflow, a lost
    watch), the tick sends `resync` instead: the service compares every file's size with its cursor (a
    sweep, ~150 ms) and upserts what moved;
  - while the app's watcher is unhealthy (no counts), a `resync` every 30 s;
  - `history.jsonl` is not under `projects/`: a tick that sends anything also stats it (one stat).
- **With the app closed, nothing refreshes the database.** The CLI and the MCP tools read it as of the
  app's last tick, and their results say so (`as_of`). One exception is recommended: a service started
  without the app (by the CLI or an MCP client) sweeps once at its start (~150 ms). Without it, a
  session written in another terminal stays invisible until the app runs again.
- **The app's own facts** still go as `factsChanged` and `openChanged`, and the app never reads the
  database for live state (Rules #1 and #7).

The three triggers that were compared (A was recommended on 2026-10-13; the user chose B):

| | A. The service's own watcher | **B. The app's observer counts, the tick upserts** (decided) | C. Poll the files of live sessions |
|---|---|---|---|
| How | the same `TranscriptWatcher` class, instantiated in the service | `TakeChangedSessions()` on the app's watcher; the tick sends the changed sessions over the pipe; `resync` after a reconnect or a re-index | read Claude's presence files (the observer's own source), stat those sessions' files about every second |
| Latency while the app runs | ~1 s (a coalescing window) | the refresh period (default 5 s) | ~1 s for live sessions; other changes at the next sweep |
| With the app closed (CLI, MCP, claudes in other terminals) | the same | as of the app's last tick (plus the one sweep of a service started without the app) | the same |
| Idle cost | none: the kernel notifies, microseconds per event | none: no change, no request | ~6 ms a second for 49 presence files, plus the stats |
| New code | none: the class exists, and the m5 harness tests it end to end | `TakeChangedSessions()`, the tick, `upsertSessions` / `resync` | presence parsing with pid validation (the observer's `PresenceRowMatchesProcess` exists) |
| Failure modes | an overflow → a sweep | an app crash or hang delays the database until the next connect's `resync` | a change no claude made (Claude's sweep deleting a file, a moved file) waits for the sweep |

All three share the same backbone — the per-file cursor committed with the rows, and a sweep at start
and as a belt — so correctness does not depend on the choice. Only the latency without the app, the
coupling and the code do.

#### 7.6.2 How fresh, per consumer

| Consumer | Today | Needs | Why |
|---|---|---|---|
| The app's summary surfaces (the side panel, the per-tab panel, the tab tooltip's card, the Sessions detail, the Manager's Summary tab, Copy ▸ Summary) | `AnalyzeSessionTranscript` on every registry notify, plus a 5 s backstop tick | — | **not a consumer:** they keep the C++ analyzer (decided 2026-10-14, §8.5) |
| The Sessions page for an open session (its row and detail), and the AI search asked about "the session I just worked in" | the gather at page open and ↻ (sidecars) | ≤ 5–10 s | the last prompt is findable soon after it was sent |
| A global prompt history (§8.6) | — | ≤ 5–10 s | the prompt just sent in another tab comes back on Up |
| The AI search's tools and the MCP `session` tool, which read the database's summary (§8.5) | — | ≤ 30–60 s | a request takes seconds anyway |
| Search over history, the CLI, the MCP tools | 2–42 s per content search | ≤ 30–60 s | the sessions asked about are mostly closed |
| The inferred folder | a ~15 s throttle per open session | ≤ 30–60 s | it changes slowly |
| The External rows, closed sessions' sort keys, the launch bar's id check | head reads, sidecars, a glob | ≤ 30–60 s | facts of sessions that are not growing; resume itself reads the disk (Rule #6) |
| Usage, cost, diagnostics | — | minutes | reports |
| Live state: status dots, the delivery gate, the Prompt Queue Runner | hooks and the scanner, milliseconds | — | never from the database |

So **the refresh period sets the freshness: default 5 s**, a setting from 1 to 60 s. The ~1 s target
of the 2026-10-13 version came from the summary surfaces, which no longer read the database. The
`sessionsChanged` notification of that version is dropped as well: the app drives the refresh, so each
`upsertSessions` response already tells it that the data landed.

#### 7.6.3 One tick, step by step

- **The app** (its engine thread, every refresh period):
  1. take the changed sessions (`TakeChangedSessions()`);
  2. none → stop; otherwise send `upsertSessions` (or `resync`, §7.6.1);
  3. a failed request keeps its ids for the next tick.
- **The service**, one transaction per request:
  1. for each named session, read its transcript and side files from their cursors to the last complete
     line;
  2. classify the new lines and APPEND them. Transcripts are append-only, and the cursor commits with
     the rows, so a replay can neither lose nor double a line;
  3. UPSERT the session's derived rows, each once per tick:
     - the `session` row, its aggregates (`INSERT … ON CONFLICT (sid_key) DO UPDATE`);
     - its `summary` row (`INSERT OR REPLACE`; the database's own summary, §8.5);
     - `usage` per API message (a message's later line carries its final usage);
     - `path_access` per (session, path, operation);
     - the live branch, recomputed in C++ from that session's `parent_uuid` / `leaf_uuid` columns
       (milliseconds), writing only the flags that changed;
  4. advance the cursors in the SAME transaction, and answer with them.

  Every step is proportional to the new bytes: no step re-reads a session's history.
- **Lifecycle.** The app holds a connection, so the service runs while Agentmaster runs. It exits
  10 minutes after its last client. No resident daemon: at the app's next connect, the `resync` catches
  up — a sweep plus whatever was appended meanwhile (a whole day here, ~190 MB: about 4 s of parsing at
  46 MB/s).
- **The page's ↻** sends a `resync` (a sweep), never a rebuild.

#### 7.6.4 The load, measured

`append_rate.py`, `commit_cost.py`; 2026-10-12/13, 49–100+ claude sessions:

| | |
|---|---|
| Written in the last 24 h (line timestamps; a file created that day counted whole, for its fork copies) | **190 MB** of transcript lines (38,720 lines, 71 files, 19 of them new); average **2.2 KB/s**; busiest hour **23.4 MB** (6.5 KB/s); plus 32.5 MB of spilled tool outputs |
| A burst | a new fork file: 5.2 MB at once |
| A sweep of the whole tree by directory enumeration (12,749 files in 1,293 folders) | **145–150 ms** warm; ~0.36–0.39 s right after reading ~1 GB of transcripts (a cold directory cache) |
| Claude's presence files (the live sessions: 49 while measured) | read in 6 ms |
| COMMIT of one ingest transaction (20 real lines, a file database on C:) | 1.9–2.3 ms median, 3.1–4.6 ms p95 |
| A whole busy hour (23 MB) in 20-line transactions | 6.6–12.3 s of database time: **0.18–0.34% of one core for that hour**, inserted through Python (the C API's appender is cheaper) |
| Merging the hot lines into the ZSTD table (6.9–9.7 K lines) | 0.21–0.28 s |
| A checkpoint after it | 0.22–0.27 s |
| A refresh tick's upserts: the changed sessions' `session` and `summary` rows, one transaction (a 4,500-row `session` table with a primary key) | 1–5 sessions **~17 ms** median (p95 19–25 ms), 20 sessions ~27 ms, through Python. One key written twice in one transaction was accepted by this build, but a tick writes each session once anyway |

So the steady cost is under 1% of one core in the busiest hour, at background CPU and I/O priority, in
another process. A tick without a change costs nothing (no request); a tick with changes ~20–30 ms of
database time plus the parse of the new bytes — at the default 5 s and constant activity, at most ~0.5%
of one core. The app pays one change count per write (it already does, for its scanner) and one
request per busy tick. Merges, re-clusters and checkpoints run when no query runs (the service knows
when one does), and they can be interrupted.

#### 7.6.5 Full rebuilds happen only in three cases

1. **A profile's first build** (§7.3): once, in the background, newest sessions first. The page works
   meanwhile from what is indexed, and says how far back that is.
2. **A new schema, tokenizer or classifier version:** new tables are re-derived from the lossless `line`
   table (no transcript is read) and swapped in, in one transaction; searches use the old tables until
   the swap. The per-line envelope parse runs at ~460 MB/s (3.11 GB in 6.7 s on 4 threads, §5.6), so a
   re-derive of today's 10.4 GB is a background job of minutes.
3. **A file that does not open** (§11): it is renamed aside and rebuilt from the transcripts, in the
   background.

## 8. Consumers

### 8.1 The Sessions page

- **Open:** one `listSessions` request replaces the gather loop; "indexing N of M" while a cold build
  runs.
- **Search:** one `search` request per settled query; the service returns the session ids, tiers, hit
  counts and the snippets of the first screen; a newer request cancels the older one.
- **Detail pane:** `sessionDetail` returns the facts, the numbered prompt list and the snippets; no
  transcript read.
- **The row filters stay in the GUI**, over the returned rows (the same code), because the open set and
  the hidden list are runtime state.
- **A virtualized table (independent of the database, and shippable first).** Rows have one fixed height;
  only the rows in the viewport plus one screen above and below exist as XAML, between two spacers sized
  to the rest; the scroll handler re-renders on the next clean tick (never inside a layout pass — the
  `E_LAYOUTCYCLE` lesson of the tag badges); one lean registry read per render (a live map, not a
  `SessionInfo` copy per row); presence as a hash map. About 60 rows × 12 elements per update instead of
  every row. Up/Down navigation and bring-into-view use row index × row height.

### 8.2 The AI search

- `GatherSessionCatalog` becomes one `listSessions` request; the index keeper (SESSIONS.md §9.4) goes
  away, and with it the "older sessions are still being indexed" answers after a restart.
- `RunSessionSearch` becomes `search`; the `deep` switch becomes `scopes` (replies are searched by
  default, tool output on request), since content search is now cheap.
- The model gets the read tools of §8.4, so it can check a candidate (read its prompts, grep it) before
  it shows it. The 43 s request of SESSIONS.md §9.9 would be dominated by model time (~3–5 s).

### 8.3 The CLI

New verbs: `agentmaster search <query> [--scopes …] [--days N] [--json]`, `agentmaster sql "<select>"`,
`agentmaster sessdb status | rebuild | stop`. They are pipe clients; the existing verbs are unchanged.

### 8.4 The MCP tools

> **2026-10-13:** [`SESSIONS_AGENT.md`](SESSIONS_AGENT.md) redesigns this tool list for the AI search
> (search with alternative phrasings, a shell-command scope and folder exclusion; `peek`; `read` at the
> Copy ▸ Transcript levels; `show` with a reason and an evidence ref per row; `reveal`). Where the two
> documents differ, `SESSIONS_AGENT.md` is the newer proposal; the conventions of §8.4.2 and the
> extension contract of §8.4.4 hold for both.

The same tool registry serves two clients: the in-app AI search (tool servers `sessions` and
`display`, in-process, calling the service) and **any Claude Code session** through the stdio MCP server
`agentmaster-sessiondb.exe mcp` (server name `agentmaster`, so its tools read `mcp__agentmaster__search`
and so on), registered with
`claude mcp add --scope user agentmaster -- "<profile>\sessiondb\bin\agentmaster-sessiondb.exe" mcp`, or
from a Settings button (D6).

Examples a Claude Code session can do with them: "find the session where we designed the delivery gate
and list its decisions" (`search` → `messages`); "which sessions edited `TerminalPage.cpp` this week"
(`who_touched`); "how many tokens did NumSharp cost in September" (`usage`); "what did I ask about DuckDB"
(`grep` with `roles: ["prompt"]`).

#### 8.4.1 What exists today, and what its descriptions leave out

Two tools exist (built 2026-10-10; `SessionSearchTools.cpp`, SESSIONS.md §9.3). Each has a description
and a JSON schema in which every parameter has a one-sentence description, and a 7-rule system prompt
goes with them (8 rules since 2026-10-13, in no build yet: the first step mentions the kinds of tool
calls the model can use and how; SESSIONS.md §9.3):

| Tool | Description the model reads (as shipped) | Parameters |
|---|---|---|
| `mcp__sessions__search` | "Find Claude Code sessions on this computer. Returns matching sessions as JSON rows (id, title, dir, branch, active, created, msgs, tools, match), best match first. Changes nothing on the screen." | `query`, `days`, `after`, `before`, `dir`, `branch`, `tag`, `favorite`, `open`, `deep`, `sort` (an enum), `limit` |
| `mcp__display__show` | "Show sessions to the user in the Sessions browser, in the given order. Replaces what the list shows. Call it once per request, after searching." | `ids`, `top`, `note` |

What they do **not** specify:

- **The result.** The row fields are named, not defined (the time format, what `match` can be, `hits`,
  the snippets); the envelope (`now`, the window, `searched`, `matched`) is not described; the
  `indexing` field is explained only in the system prompt.
- **Bounds.** `limit` (1–50) and `days` are clamped in code, not declared (`minimum` / `maximum`); the
  defaults are in the prose, not in `default`; there is no `additionalProperties: false`.
- **Errors.** The handler writes its errors for the model, but no description says which can happen.
- **Paging.** None: 50 rows is the end of any search.
- **Scopes.** Typed prompts are always searched; `deep` lumps replies, thinking, tool calls and tool
  output into one switch.
- **Rules the model cannot see.** A whole session id also finds the session's forks; fuzzy matching
  exists on the page only.
- **Reading.** No tool opens a session, so the model cannot check a candidate before it shows it.
- **For developers.** There is no extension contract; SESSIONS.md §9.3 is a two-row table.

The contracts below close each gap.

#### 8.4.2 Conventions every tool follows

- **Read-only.** Every tool except `show` changes nothing. MCP annotations: `readOnlyHint: true`,
  `idempotentHint: true`, `openWorldHint: false`. The harness may run them in parallel
  (`HarnessTool::readOnly`).
- **Ids.** A session is named by its short id: the first 8 characters of its UUID, longer when two
  sessions share them (today's rule). Every `id` parameter takes a short id, a longer prefix or the whole
  UUID. A block is named by a **`ref`**: `<short id>:<line>` or `<short id>:<line>.<block>` (its line in the
  session's file and its block index in that line); a block of a subagent's transcript adds the agent:
  `<short id>/<agent id>:<line>`. A ref stays valid across merges and re-clusters (it never names a
  `doc_id`).
- **Times.** Local time, `YYYY-MM-DD HH:MM`; every result carries `now` in that form. Date parameters
  take `YYYY-MM-DD`, local.
- **Roles** (`search` scopes, `messages`, `grep`): `prompt` (typed by the user), `queued` (typed while
  claude worked), `reply` (Claude's text), `thinking`, `tool_call`, `tool_result`, `system`, `subagent`
  (any block of a subagent's transcript).
- **Size.** A result is compact JSON of at most 64 KB. Text is cut, with `"truncated": true`: snippets at
  160 characters, message blocks at 2,000 unless `full: true`.
- **Paging.** A tool that has more rows than `limit` returns `"next": "<cursor>"`; passed back as
  `cursor`, it continues. A cursor is valid only with the same other parameters.
- **Coverage.** While a cold build runs, a result that may miss sessions carries
  `"indexing": {"covered_since": "<date>"}`.
- **Optional fields** appear only when they say something (`open`, `favorite`, `tags`, `fork_of`, …).
- **Errors** are MCP error results whose text says what is wrong and how to fix the call, for example
  `Unknown session "1e0f7ab". Use an id from a search result (8 or more characters).` A handler never
  throws.
- **Budgets.** A tool answers within its budget or returns what it has with `"partial": true`.
- **Language.** Descriptions and errors are ASCII English (the harness's rule: they travel through files
  and the API unchanged).

#### 8.4.3 The contracts

Each contract gives the **description** (the exact text the model reads), the **parameters** (a JSON
Schema), the **result**, the **errors**, the **plan** (what the service runs) and the **budget**.

##### `search`

- **Description:** "Find Claude Code sessions on this computer. Every word of `query` must match
  somewhere in a session: its title, folder, branch, the path of a file it touched, or a message in the
  chosen `scopes`. Words in double quotes match as one phrase; a whole session id finds that session and
  its forks. Returns up to `limit` sessions, best first, each with why it matched and up to two snippets
  whose `ref` opens the context in `messages`. Changes nothing on the screen. Fast (under 0.2 s), so
  searching again with other words is cheap."
- **Parameters:**

  ```json
  {
    "type": "object", "additionalProperties": false,
    "properties": {
      "query":    {"type": "string", "maxLength": 500, "description": "Words that must all match. Leave it out to list sessions by the filters only."},
      "scopes":   {"type": "array", "uniqueItems": true, "items": {"enum": ["title", "path", "prompt", "reply", "tool", "subagent"]},
                   "default": ["title", "path", "prompt", "reply"],
                   "description": "Where words may match. title: names. path: folders and touched files. prompt: what the user typed. reply: Claude's answers. tool: tool calls and their output (broad, noisy). subagent: subagents' work."},
      "fuzzy":    {"type": "boolean", "default": false, "description": "Also match words with a typo (one or two letters off)."},
      "days":     {"type": "integer", "minimum": 1, "maximum": 3650, "default": 90, "description": "Only sessions active in the last N days."},
      "after":    {"type": "string", "format": "date", "description": "Only sessions active on or after this local date. Overrides days."},
      "before":   {"type": "string", "format": "date", "description": "Only sessions active on or before this local date."},
      "dir":      {"type": "string", "maxLength": 500, "description": "Part of the working folder's path; '/' and '\\' are the same."},
      "branch":   {"type": "string", "maxLength": 200, "description": "Part of the git branch name."},
      "tag":      {"type": "string", "maxLength": 100, "description": "A bookmark tag, the whole name, any case."},
      "favorite": {"type": "boolean", "description": "true: only starred sessions."},
      "open":     {"type": "boolean", "description": "true: only sessions with a tab open now."},
      "sort":     {"enum": ["relevance", "active", "created"], "default": "relevance",
                   "description": "relevance: name matches first, then path, prompt, other text; BM25 inside a group. active / created: newest first."},
      "limit":    {"type": "integer", "minimum": 1, "maximum": 50, "default": 15},
      "cursor":   {"type": "string", "description": "The `next` value of the previous result, for the following rows."}
    }
  }
  ```

- **Result:**

  ```json
  {"now": "2026-10-12 21:40", "window": "last 90 days", "searched": 1447, "matched": 38,
   "rows": [{"id": "1e0f7ab2", "title": "DuckDB session database design", "dir": "K:\\source\\Agentmaster",
             "branch": "agentmaster", "active": "2026-10-12 21:39", "created": "2026-10-12 20:31",
             "msgs": 6, "tools": 412, "match": "prompt", "hits": 17, "score": 12.4, "open": true,
             "snippets": [{"ref": "1e0f7ab2:184", "role": "prompt", "text": "…what if we will use duckdb? search for…"}]}],
   "next": "s:15"}
  ```

  `match` is `name`, `path`, `prompt`, `text` or `listed` (no query); `hits` counts matched blocks; `score`
  is BM25 and appears only for content matches.
- **Errors:** an unknown scope; a date that is not `YYYY-MM-DD`; `after` later than `before`; a cursor
  from other parameters. Each says the valid form.
- **Plan:** the tiers T0–T3 (§6.2) over the window; the session list and counts first, snippets only for
  the rows returned.
- **Budget:** 150 ms (§6.7).

##### `session`

- **Description:** "Everything known about one session, without its messages: its titles, folder and
  branch, when it was created and last active, its size, models and tokens, its lineage (the session it
  was forked from, its forks, the plan, /clear or /handover it continues), its tags and star, whether a
  tab has it open, and the files it touched most. Use it to check a search result before you show it."
- **Parameters:** `{"type": "object", "additionalProperties": false, "required": ["id"], "properties":
  {"id": {"type": "string", "minLength": 8, "maxLength": 36, "description": "A session id from a result, or a whole UUID."}}}`
- **Result:** `id`, `uuid`, `title`, `titles` {`stored`, `custom`, `ai`, `first_prompt`}, `dir`,
  `dir_launched` (when it differs), `branch`, `created`, `active`, `msgs`, `tools`, `subagents`,
  `context_tokens`, `tokens` {`input`, `output`, `cache_read`, `cache_write`}, `models`, `lineage`
  {`forked_from`, `forks`, `continues` {`id`, `via`: `plan` | `clear` | `handover`}, `continued_by`},
  `tags`, `favorite`, `open`, `top_files` [{`path`, `ops`}], `transcript`: `present` | `deleted` |
  `replaced`.
- **Errors:** an unknown id; an ambiguous prefix (says how many sessions share it).
- **Plan:** the `session` row, `am_*`, `am_link`, the ten most-touched `path_access` rows.
- **Budget:** 20 ms.

##### `messages`

- **Description:** "Read one session's conversation in order, a page at a time: what the user typed and
  Claude's replies, and on request tool calls, tool output and thinking. Give `around` a `ref` from
  search or grep to read the context of a hit, or `order: \"desc\"` to read the end first. Blocks longer
  than 2,000 characters are cut unless `full` is true."
- **Parameters:**

  ```json
  {
    "type": "object", "additionalProperties": false, "required": ["id"],
    "properties": {
      "id":     {"type": "string", "minLength": 8, "maxLength": 36},
      "roles":  {"type": "array", "uniqueItems": true,
                 "items": {"enum": ["prompt", "queued", "reply", "thinking", "tool_call", "tool_result", "system", "subagent"]},
                 "default": ["prompt", "queued", "reply"]},
      "around": {"type": "string", "description": "A ref: the blocks before and after it."},
      "from":   {"type": "string", "description": "A ref: the blocks starting at it."},
      "order":  {"enum": ["asc", "desc"], "default": "asc", "description": "desc: newest first."},
      "limit":  {"type": "integer", "minimum": 1, "maximum": 100, "default": 20},
      "full":   {"type": "boolean", "default": false, "description": "Do not cut long blocks (the 64 KB result cap still applies)."},
      "cursor": {"type": "string"}
    }
  }
  ```

- **Result:** `{"id", "title", "blocks": [{"ref", "time", "role", "tool", "text", "truncated"}], "next"}`.
  A `tool_call`'s text is its name and input JSON; a `tool_result`'s, its output.
- **Errors:** an unknown id; a `ref` from another session; both `around` and `from`.
- **Plan:** the session's block range (`first_doc_id` … `last_doc_id`, then `doc_hot`); `around` reads
  the ref's position ± `limit` / 2.
- **Budget:** 30 ms a page (a whole 304-line session read in 2.9 ms, §6.8).

##### `grep`

- **Description:** "Find exact text, or a regular expression, in conversations, with the words around
  it. Use it for precise lookups: an error message, a function name, a command. Searches the sessions in
  `ids`, or every session in the filters. A regex needs a literal of 3 or more characters, or `ids`."
- **Parameters:** `pattern` (string, required, 1–500 characters), `regex` (boolean, default false),
  `case_sensitive` (boolean, default false), `ids` (array of session ids, up to 50), `dir`, `days`
  (1–3650, default 90), `after`, `before`, `roles` (as in `messages`; default every role but `thinking`),
  `context` (integer 0–400 characters on each side, default 80), `limit` (1–200, default 30), `cursor`;
  `additionalProperties: false`.
- **Result:** `{"now", "matches": [{"id", "title", "ref", "time", "role", "tool", "snippet"}], "sessions": 7, "next", "partial"}`.
- **Errors:** an invalid regex (RE2's message); a regex without a 3-character literal and without `ids`
  ("add a longer literal, or name the sessions").
- **Plan:** a literal goes through the index plan (§6.3) for candidates, then exact verification on the
  candidate blocks' text (fetched by `doc_id` through its ART); a regex sends its longest literal
  through the index, then `regexp_matches` (RE2) on the candidates.
- **Budget:** 200 ms, then `partial`.

##### `files`

- **Description:** "The files and folders one session read, edited, wrote, searched or named in a shell
  command, with counts and first and last times. Use it to see what a session worked on."
- **Parameters:** `id` (required), `op` (enum `read`, `edit`, `write`, `search`, `shell`; default all),
  `under` (a folder prefix), `limit` (1–500, default 100); `additionalProperties: false`.
- **Result:** `{"id", "files": [{"path", "ops": {"read": 3, "edit": 2}, "first", "last"}], "folders": [{"dir", "files"}], "next"}`.
- **Errors:** an unknown id; an unknown `op`.
- **Plan:** `path_access` rows of the session.
- **Budget:** 20 ms.

##### `who_touched`

- **Description:** "Which sessions touched a file, or anything under a folder, newest first, and what
  they did to it. Takes a full path, a folder (everything under it), or a file name alone
  (`TerminalPage.cpp`)."
- **Parameters:** `path` (required, 1–500 characters), `op` (as in `files`), `days` (default 365),
  `after`, `before`, `limit` (1–100, default 20), `cursor`; `additionalProperties: false`.
- **Result:** `{"now", "sessions": [{"id", "title", "active", "ops": {"edit": 4}, "paths": ["up to 3 matching paths"]}], "next"}`.
- **Errors:** an empty path; an unknown `op`.
- **Plan:** a folder reads the `dir_fold` prefix range (zonemaps, §6.8); a file name, `leaf_fold`
  equality; joined to `session`.
- **Budget:** 30 ms.

##### `timeline`

- **Description:** "What happened when: for each day (or hour) of a period, the sessions that were
  active, with their prompts and turns. Use it for questions like 'what was I doing last Tuesday'."
- **Parameters:** `from`, `to` (dates; default the last 7 days), `group` (`day` | `hour`, default `day`),
  `dir`, `limit` (sessions per bucket, 1–50, default 10); `additionalProperties: false`.
- **Result:** `{"buckets": [{"start": "2026-10-06", "sessions": [{"id", "title", "prompts", "turns", "first", "last"}]}]}`.
- **Errors:** a bad date; `from` later than `to`; more than 366 day buckets or 744 hour buckets.
- **Plan:** prompt and reply timestamps (`doc`, `doc_hot`), grouped by bucket and session.
- **Budget:** 50 ms.

##### `usage`

- **Description:** "Token use and cost, grouped by session, day, model or folder. Each API message counts
  once. A session's cost is Claude Code's own record when the session has one (`cost_source` "claude");
  otherwise it comes from a price table and is an estimate (`cost_source` "estimate"), not a bill."
- **Parameters:** `group_by` (required: `session` | `day` | `model` | `dir`), `days` (default 30),
  `after`, `before`, `dir`, `id`, `limit` (1–200, default 20); `additionalProperties: false`.
- **Result:** `{"rows": [{"key", "input", "output", "cache_read", "cache_write", "cost_usd", "cost_source"}], "totals": {…}, "prices": "<price table version>"}`.
- **Errors:** an unknown `group_by`; a bad date.
- **Plan:** `usage` grouped (deduplicated by message id at ingest); costs from `session.cost_usd` (the
  last `cost-state`, §5.6) where set, else the price table.
- **Budget:** 50 ms.
- **Open:** the price table for the sessions without a `cost-state` (D13).

##### `related`

- **Description:** "Sessions connected to one session: its lineage (forked from, its forks, the plan,
  /clear or /handover chain), and the sessions that share the most files or rare words with it."
- **Parameters:** `id` (required), `kinds` (array of `lineage`, `files`, `words`; default all), `limit`
  (1–20 per kind, default 5); `additionalProperties: false`.
- **Result:** `{"lineage": {…as in session…}, "by_files": [{"id", "title", "shared": 12}], "by_words": [{"id", "title", "score"}]}`.
- **Errors:** an unknown id.
- **Plan:** lineage from `session` and `am_link`; files: shared `path_access` paths; words: the
  session's rarest terms (low session frequency in `dict`) and the sessions that carry them (`spost`).
- **Budget:** 100 ms.

##### `sql`

- **Description:** "Run one read-only SQL query (DuckDB dialect) over these views when no other tool
  fits: counts, groupings, unusual filters. Returns at most 200 rows within 2 s. Views:
  v_sessions(id, uuid, title, dir, dir_key, inferred_dir, branch, created, active, msgs, tools, model,
  models, effort, version, input_tokens, output_tokens, cost_usd, api_errors, interrupts, compactions,
  forked_from, favorite, tags, open); v_messages(id, ref, time, role, tool, text);
  v_lines(id, line, type, time, raw): raw is the line's JSON, read fields with json_extract;
  v_paths(id, path, dir, file, op, n, first, last); v_usage(id, time, model, input_tokens,
  output_tokens, cache_read_tokens, cache_write_tokens); v_links(id, from_id, via)."
- **Parameters:** `query` (required, 1–4,000 characters), `limit` (1–200, default 50);
  `additionalProperties: false`.
- **Result:** `{"columns": [...], "rows": [[...]], "truncated": false, "ms": 12}`.
- **Errors:** not a SELECT; more than one statement; DuckDB's own message for an unknown view or column;
  the 2 s timeout.
- **Plan:** a dedicated connection: exactly one statement (`duckdb_extract_statements`), of type SELECT
  (`duckdb_prepared_statement_type`), the views only, `duckdb_interrupt` at 2 s (§10).
- **Budget:** 2 s.

##### `show` (in-app only)

- **Description (as shipped):** "Show sessions to the user in the Sessions browser, in the given order.
  Replaces what the list shows. Call it once per request, after searching."
- **Parameters:** `ids` (array of session ids, up to 50) or `top` (integer 1–50, the first rows of the
  latest search), exactly one of them; `note` (a caption, up to 80 characters);
  `additionalProperties: false`.
- **Result:** today's text: "Shown N sessions. Now reply to the user with one short sentence."
- **Errors:** both or neither of `ids` and `top`; unknown ids (listed).
- **Not in the external server:** it changes the Agentmaster window, which another Claude session has
  no business doing.

#### 8.4.4 Adding or changing a tool (the extension contract)

1. **One registry.** Every tool is one entry of `SessionDbTools` (the service): server, name,
   description, input schema, result fields, budget, handler. The in-app harness (`HarnessToolServer`s)
   and the external stdio server both build their tool lists from it, so the two cannot disagree.
2. **The description** says what the tool answers, when to use it instead of the others, what it
   returns, its limits and cost, and how to continue (cursor, refs). **The schema** gives every parameter
   a description, a type, a `default`, bounds (`minimum` / `maximum` / `maxLength`), an `enum` when the
   set is closed; `additionalProperties: false`; `required`.
3. **The result** follows §8.4.2 (compact JSON, short ids and refs, `now`, `next`, `truncated`,
   `partial`, `indexing`; a field only when it says something).
4. **Errors** say what is wrong and how to fix the call. A handler never throws.
5. **Budget.** A tool answers within its budget or returns `partial`. It asks before each slow step
   whether its request is still wanted (the harness rule: a handler runs on the SDK's reader thread and
   cannot receive its own interrupt), and the service interrupts its query (`duckdb_interrupt`).
6. **Read-only**, unless it is a display tool. A tool that writes anything (a tag, a title) needs its
   own decision and a confirmation path.
7. **Tests** (the m5 harness): the schema parses; defaults and clamps; every error text; the result over
   a fixture database as golden text. And one live `harness_live.exe --agent` trace: does the model pick
   the tool when it should?
8. **Versioning.** The registry has a version; the harness records it and `hello` returns it. A
   description change changes the model's prompt (and its cache), so changes ship together.
9. **Docs.** The contract goes into §8.4.3, and a line into SESSIONS.md §9.3.

### 8.5 The session summary, formed by the database

**The question** (2026-10-12): can the summary we show in the side panel come from DuckDB? Yes. It
was measured, and verified field for field against today's code.

**Decided by the user (2026-10-14):** "I want the duckdb to be capable of forming a summary but I don't
want summary to use duckdb." So the database forms the summary for ITS consumers (the AI search's
tools — SESSIONS_AGENT.md reads this row —, the MCP `session` tool, the CLI), and the app's summary
surfaces keep `AnalyzeSessionTranscript`, unchanged: the per-tab panel, the side panel, the tab
tooltip's card, the Sessions detail, the Manager's Summary tab and Copy ▸ Summary.

**Today.** The summary box is `AnalyzeSessionTranscript` (`ProcessInspect.Content.cpp`). It serves the
per-tab summary panel, the side panel's summary area (the same `AgentTabOverlay` in fill-host mode), the
tab tooltip's card, the Sessions detail, the Manager's Summary tab and Copy ▸ Summary.

- It reads the WHOLE transcript, widens it to UTF-16, parses every line and walks the live branch.
- It runs again whenever the file grows (a (size, mtime) cache), so a working session pays it every
  turn.
- Its own comment puts the transient allocations at 5–8× the file size (the `bad_alloc` crash class).

Measured with an optimized build of that code (`bench_summary.cpp`, warm file cache):

| | 128.7 MB | 102.3 MB | 1.7 MB | 922 NumSharp sessions (3.11 GB) |
|---|---|---|---|---|
| `AnalyzeSessionTranscript` (the summary box) | 2.70–2.85 s | 2.20 s | 33 ms | 67.7 s |
| `RenderTranscriptTurnsFile`, Default level (the transcript overlay) | 1.62 s | 1.37 s | 24 ms | |
| `CollectConversationUpstream` (the whole-conversation chain) | 17 ms | 21 ms | 260–309 ms (a fork: it scans the project folder) | |

**From the database** (`summary_from_db.py` over the lossless store):

| | 128.7 MB | 102.3 MB | 1.7 MB |
|---|---|---|---|
| Today (C++, whole file) | 2.70–2.85 s | 2.20 s | 33 ms |
| A: from the stored lines, nothing precomputed | 1.91 s (the live-branch walk 0.70 s in Python, the parse 0.89 s, the sections 0.28 s, the reads 37 ms) | 1.53 s | 194 ms |
| B: precomputed section tables, 7 queries | 39.5 ms | 39.7 ms | 35.7 ms |
| B: one precomputed summary row, 1 query | **2.75 ms** | **2.99 ms** | **2.80 ms** |

- B's tables for all 922 sessions take 54.4 s with the Python live-branch walk (20.4 s of it), about
  34 s without it, against 67.7 s for the C++ analyzer.
- The one-row-per-session `summary` table builds from them in 0.12 s.
- Their sizes: 5,149 prompt rows, 150,094 tool calls, 9,899 tool results, 478,634 live-branch uuids.

**Verified.** 20 facts are compared on every session that did not change after the ingest: the prompts
of the current segment, all prompts, the earlier segments, the files read / created / edited, the
skills, the tasks done / open, compacted, the recap's length, the first / last / last-user time, the
branch, the plan parent, the plan-mode exit, the plan content, the plan file and the plan reads. They
**equal the C++ analyzer's on 921 of 921 sessions**, through both read paths. Getting there found
three rules a first SQL draft had wrong (each is now a comment in the script):

1. **The live branch.** Lines that a double-ESC rewind abandoned stay in the file. The first draft had
   no walk (and the two rules below wrong): it disagreed on up to 10% of the sessions per field (files
   read: 95 of 921).
2. **Teammate protocol.** Only the FIRST `<teammate-message>` tag of a message decides. A prose report
   followed by a termination notice in the same message is a real prompt.
3. **Created vs edited.** An Edit before a file's first creation keeps the file under "edited": the
   analyzer never removes an earlier entry.

**The design**

- **The summary reads tables the design already has:** prompts (`prompt`; `doc` roles 1 and 7), tool
  calls and results (`doc` roles 4 and 5, with `tool` and `tool_use_id`), files (`path_access`). The
  ingest adds three per-line facts (`line`, §5.2):
  - `on_branch`, from the C++ `ActiveBranchUuids` walk. A graph walk does not belong in SQL: a
    recursive CTE runs one step per link of a chain that is 20,000 lines long. A rewind moves the leaf
    and rewrites the flag for that one session.
  - `seg`, the compaction segment by position.
  - `is_copy`.
- **A `summary` table** (§5.2): one row per session with the numbered prompts (with segment numbers
  and uuids), the three file-label lists, the skills, the task counts, the plan facts, the recap and
  the times. A refresh tick upserts the rows of the sessions it names, in the same transaction as their
  lines (§7.6.3; measured: ~17 ms for a tick of 1–5 sessions, with the `session` rows).
- **`sessionSummary(id)`** (§9) returns that row in ~3 ms, to the AI search's tools, the MCP `session`
  tool and the CLI. The app's summary surfaces never call it.
- **Two implementations of the rules,** so parity is a test: the app shows the C++ analyzer's summary
  and the tools show the database's, and the two must agree. The m5 fixtures, a live corpus check (the
  prototype's `--verify`), and, while the database runs in Shadow mode, a `[sessdb-diff] summary <sid8>
  <field>` line whenever the app's analyzer and the database's row disagree (D14).
- **The whole-conversation chain** — the database's own, for the same consumers; the app's summary
  keeps `CollectConversationUpstream`:
  - the fork parent is a lookup: 29 ms by scanning every line's uuid, 8.7 ms through a uuid →
    first-file map (715,662 uuids, built in 0.1 s), and none at all once the ingest stores
    `forked_from` (§5.6). It finds the same 150 copied lines as the C++ chain walk;
  - the plan parent is a column, and `/clear` / `/handover` come from `am_link`;
  - each parent's prompts come from its own summary row; the copied uuids say where that parent's
    part ends.

### 8.6 Other parts of Agentmaster that would read the database

| Part | Today | Cost (measured or logged) | With the database | Phase |
|---|---|---|---|---|
| The summary panel, the side panel's summary, the tab tooltip's card, the Sessions detail, the Manager's Summary tab, Copy ▸ Summary | `AnalyzeSessionTranscript`: the whole file, on every growth | 2.7 s per growth of a 128.7 MB session; 5–8× the file size in memory | **none: they keep the C++ analyzer** (decided 2026-10-14, §8.5) | — |
| The summary's conversation chain | `CollectConversationUpstream`: a project-folder scan, then each parent analyzed | 17–309 ms, plus each parent's analyze | none for the app (it keeps the C++ chain walk); the database's own chain serves its tools | — |
| The AI search's tools, the MCP `session` tool, the CLI asking for a session's summary | — (the AI search reads no summary today) | — | `sessionSummary`, ~3 ms (§8.5) | P3 |
| The transcript overlay, Copy ▸ Transcript levels | `RenderTranscriptTurnsFile`: read, parse, render | 1.62 s at Default for 128.7 MB (measured); 0.27–5.3 s in the release log, once 94.2 s | Later: the render reads parsed rows instead of re-parsing JSON; the Markdown rules stay in C++ (the goldens) | Later |
| The Sessions page | sidecars, ripgrep, re-parse | §1.1: 6 s+ to open, 2–42 s for content | §8.1 | P2 |
| The AI search's catalog and index keeper | `GatherSessionCatalog` | 3 minutes for a cold catalog | §8.2 | P3 |
| Sessions that are not open: MOST RECENT sort keys, "active … ago" | the sidecar facts | — | `last_typed_ms`, `last_turn_end_ms`, `last_activity_ms` | P2 |
| The launch bar's typed session id (the green "Resume session") | `ClaudeConversationExists`: a glob over every project folder | — | a `session` lookup; the resume itself stays transcript-gated on disk (Rule #6) | P2 |
| The inferred working directory | `_ScanInferredTabColors`: each open session's sidecar paths, mtime-gated, every ~15 s | — | `session.inferred_dir_key` for EVERY session (D15), so the page can group closed sessions by where they worked | P5 |
| The Fleet Observer's External rows | `ReadTranscriptInfo` head reads, a worker-thread title cache | — | the `session` row by id: title, branch, times | P4 |
| The Manager's read-only prompt list of an external | a background transcript read | — | `summary.prompts` | P4 |
| The CLI's `show` and `sessions` | transcript tails and `sessions.json`, offline | — | the service when it runs; the offline readers stay (the CLI works with the app down) | P4 |
| The Tags panel, the tag hover panel, favorites | session-store scans (cached since UI_THREAD.md §11) | ~5 ms per call | `am_tag` joins: "tagged release AND mentions DuckDB" in one query | Later |
| The compose box's Up / Down history | `_BuildPromptHistory`: the selected session's sent prompts, from the registry's queue | — | a global history: every prompt ever typed in any session, searchable | Later |
| Usage and cost (new) | — | — | `cost_usd` (Claude Code's own `cost-state`) and token sums by day, model, folder (the `usage` tool) | Later |
| Diagnostics (new) | `grep` over `hooks.log` | — | API errors, refusals, interrupts, slow Stop hooks per session (fdefc0e8's slowest: 32.4 s), in SQL | Later |

**What stays out** (no win): the scanner's live tail and the state machine (Rules #1 and #7), the hook
bridge, the pending-input reader (it reads the screen), the delivery gate, presence and liveness. That
is real-time state in the GUI process, where a pipe round trip only adds latency. Every resume and fork
decision reads the transcript on disk: the database can lag by up to ~2 s (G3).

## 9. The protocol

- **Transport:** the named pipe `\\.\pipe\agentmaster.sessiondb.<hash>`, `PIPE_REJECT_REMOTE_CLIENTS`,
  the default DACL (the user, SYSTEM, administrators). A discovery file
  `<profile>\sessiondb\service.json` names the pipe, the pid and the version (the `bridge.json` idiom).
- **Framing:** a 4-byte little-endian length, then UTF-8 JSON. Request `{v, id, method, params}`;
  response `{id, result}` or `{id, error: {code, message}}`; `{method: "cancel", params: {id}}` interrupts
  that request's query.
- **Methods:** `hello` (versions: service, schema, tool registry), `listSessions`, `search`,
  `sessionDetail` (the `session` tool), `sessionSummary` (the `summary` row, §8.5; with `chain: true`
  also the rows of the sessions it continues), `messages`, `grep`, `files`, `whoTouched`, `timeline`, `usage`,
  `related`, `sql`, `rebuild` (a transcript from the lossless store, or the whole index), `status`,
  `factsChanged` (the GUI says a title, tag, favorite or link changed), `openChanged` (the GUI's open
  sessions), `upsertSessions`, `resync`, `shutdown`. Each tool of §8.4 maps to one method.
- **The refresh** (§7.6, decided 2026-10-14), both sent by the app's refresh tick:
  - `upsertSessions` `{sessions: [{id, path}]}` — the sessions whose change count moved since the last
    tick; the service reads their files from the cursors and upserts. Answer: `{sessions: [{id, cursor,
    lines}]}`. A failed request leaves the ids for the next tick.
  - `resync` `{}` — after the app's (re)connect, a watcher re-index, an unhealthy watcher (every 30 s)
    or the page's ↻: the service sweeps every file against its cursor and upserts what moved.

  The service sends nothing unasked: the app drives the refresh, and each answer says what landed.
- **Limits:** a request ≤ 1 MB, a response ≤ 16 MB; per-request timeouts (2 s for `sql`, 10 s for
  `search`); one query connection per request, from a pool of 4.

## 10. Resources and safety

- **DuckDB settings:** `threads` 4, `memory_limit` 1 GB (both in the Settings cog), `temp_directory`
  `<profile>\sessiondb\tmp`, **`enable_external_access` false** (no file reads, `COPY`, `ATTACH` or
  `INSTALL` from SQL — Agentmaster reads the transcripts itself), `autoinstall_known_extensions` and
  `autoload_known_extensions` false, `allow_community_extensions` false, `lock_configuration` true after
  setup. The prototype's out-of-memory errors came from staging raw JSON in SQL; the C++ ingest appends
  extracted rows only.
- **Priorities:** ingest, merge and re-cluster run at background priority
  (`THREAD_MODE_BACKGROUND_BEGIN` lowers CPU and I/O priority) and pause while queries arrive; queries
  run at normal priority.
- **The read-only SQL tool:** exactly one statement (`duckdb_extract_statements`), of type SELECT
  (`duckdb_prepared_statement_type`), only the documented views, 200 rows, 2 s, then
  `duckdb_interrupt`. With external access off, no SELECT can read a file either.
- **No network**, ever.
- **Privacy.** The database holds the full text of every conversation, as the transcripts do, inside the
  profile. The MCP server shows it to every Claude Code session that registers it: the same data those
  sessions can read with their Read tool, read-only. Registering it is the user's explicit step (D6).
- **Disk:** ~0.9 GB today; roughly 1 MB of database per 10 MB of transcripts.

## 11. Failures and recovery

| Failure | What happens |
|---|---|
| The service crashes | The GUI's client reconnects and restarts it (with backoff). During the rollout (P2–P4) the page falls back to today's code path; later it shows "search unavailable" and retries. |
| The file does not open (corrupt, or written by a newer DuckDB) | Renamed to `sessions.duckdb.broken-<time>`; a background rebuild starts; the page says "indexing". |
| The schema version differs | A newer service migrates, or re-derives new tables from the stored lines and swaps them in (§7.6); an older one refuses a newer schema and never writes it. |
| A new DuckDB version | The storage format reads forward; the index is ours, so only a change of `meta.tokenizer_version` forces a re-derive (§7.6). |
| The watch overflows or fails | A stat sweep at once, then polling every 30 s until the watch recovers (§7.6). |
| The service was not running while transcripts changed | At its next start: a stat sweep of every file, the deltas ingested, open and newest sessions first; searches report the coverage meanwhile (§7.6). |
| The disk is full | Ingest pauses and logs; searches keep working on what is there. |
| Two installs on one profile | One service (the mutex); the profile mutex already warns. |

## 12. Rollout and tests

| Phase | Content |
|---|---|
| **P0 spike** | A new project `agentmaster-sessiondb` linking `duckdb.dll` (2.0.0), shipped in the wapproj, the portable zip and CI (x64 and arm64); the copy-out and a hot-swap with an MCP client connected; appender throughput, `duckdb_interrupt`, and **the C++ cold build measured on this corpus**. |
| **P1 engine** | Schema and migrations; the lossless line store (D11) and its byte-exact rebuild check; `ClassifyTranscriptBlocks` shared with the sidecars, extended to the line kinds of §5.6 (`cost-state`, `worktree-state`, `relocated`, `agent-name`, `frame-link`, the `environment` attachment); the session row of §5.6; the live-branch walk at ingest (`line.on_branch`, `seg`, `is_copy`) and the `summary` table (§8.5); tokenizer; cold build, incremental ingest, merge, re-cluster; the planner; the pipe server; the refresh (§7.6): `TranscriptWatcher::TakeChangedSessions()` in the app, the app's refresh tick (setting `sessionDbRefreshSeconds`, default 5), `upsertSessions` / `resync` with upserts in the service; harness tests; `agentmaster sessdb status`. |
| **P2 page** | The virtualized table (can ship first, on today's data); `SessionDbClient`; list / search / detail through the service behind `AppSettings::sessionDb` = Off / Shadow / On. **Shadow** runs both and logs timings and differences (`[sessdb-diff]`). The sort keys of sessions that are not open; the launch bar's session-id lookup. |
| **P3 AI search and the database's summary** | The AI search on the service; the tool registry of §8.4.4 with the contracts of §8.4.3 (`scopes` replace `deep`); a live trace per tool. The database's summary (`sessionSummary`, §8.5) for the AI search's tools, the MCP `session` tool and the CLI, with its parity test against `AnalyzeSessionTranscript` (in Shadow: `[sessdb-diff] summary`); the database's conversation chain from `forked_from` / `plan_parent` / `am_link`. The app's summary surfaces stay as they are (decided 2026-10-14). |
| **P4 CLI and MCP** | The verbs; `mcp` mode; the Settings button (D6); the Fleet Observer's External rows and the Manager's external prompt list from the `session` and `summary` rows. |
| **P5 cleanup** | After weeks of Shadow parity: remove the sidecars and the ripgrep path; the inferred-folder scan reads `path_access` and `session.inferred_dir_key` (D15). |
| **Later** | `vss` semantic search, Codex rollouts, a usage and cost view, diagnostics views, a global prompt history, the transcript overlay from parsed rows (§8.6). |

**Tests**

- **m5 harness:** tokenizer, block classifier, planner, BM25, snippets.
- **Golden parity:** fixture corpora (today's transcript fixtures plus synthetic ones). For a fixed query
  set, today's `SearchIndexFast` + `SearchTranscriptsSlow` and the database return the same sessions,
  except the documented gains (matches past 4 KB of a line; subagent text).
- **A live bench** on the real corpus (the `harness_live` idiom): p50 / p95 per query class, cold-build
  time, write-to-searchable latency, memory.

## 13. Decisions for the user

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **D1** | The index engine | **Our own inverted index in DuckDB tables** (§6.5) | DuckDB's `fts` extension (a custom DuckDB build, or +22–46 MB per architecture) |
| **D2** | The DuckDB version | **2.0.x** (2.0.0 on 2026-10-21; build against 2.0.1 for a release) | 1.4 LTS (no incremental FTS, older C API) |
| **D3** | The process model | **A per-profile service process** | DuckDB inside the GUI process |
| **D4** | What is indexed | **Everything**: prompts, replies, tool calls, tool results, subagents, spilled outputs (full text stored; per-block index cap 64 KB) | Prompts, replies and tool calls only (~3× smaller index) |
| **D5** | Databases per install | **One per profile** (Rule #15) | One shared corpus index for the release and dev installs (saves ~1 GB and one build) |
| **D6** | The external MCP server | **A Settings button** that registers `agentmaster` in Claude Code (user scope) | Documented only; the user runs `claude mcp add` |
| **D7** | Transcripts Claude's sweep deletes | **Keep them searchable**, marked "transcript deleted", not resumable ("never lose a session") | Drop them, as today ("not found ⇒ don't show") |
| **D8** | Queries of 1–2 characters | **Titles, folders, paths and prompts only** | Everything (slow and unhelpful) |
| **D9** | `(F)` fuzzy over agent text | **Typo-tolerant term matching** over the dictionary (fast) | Today's subsequence rule (a scan, ~0.5–2 s) |
| **D10** | `duckdb.dll` (36.7 MB x64; +~13 MB per architecture in the bundle and the portable zips, today 20.6 / 13.9 / 14.3 MB) | **Ship it** in the package and the zips | Download it on first use, pinned by hash (smaller downloads; needs the network once and a trust rule like the updater's) |
| **D11** | The lossless store: every transcript line verbatim (§5.5) | **Yes**: +2.2 GB, byte-exact rebuilds, resumable after Claude's sweep, re-derivable index | No: the search projection only (~0.9 GB); a transcript cannot be rebuilt |
| **D12** | `file-history/` (2.07 GB, the `/rewind` snapshots) in the lossless store | **No** for version 1: files, not conversation | Yes: a swept session would keep `/rewind` |
| **D13** | `usage` costs | **Claude Code's own `cost-state` totals where a session has them** (562 of 4,444 sessions, written since ~2.1.225, with per-model `costUSD`; `hasUnknownModelCost` marks a model it cannot price), and **a price table only for the others**, shipped with the app, versioned, labeled an estimate | A price table for every session; or tokens only, no cost |
| **D14** | The session summary (§8.5) | **DECIDED by the user (2026-10-14):** the database must be able to form the summary — a precomputed `summary` row per session (SQL over the section tables, the live branch walked in C++ at ingest, a parity test against the C++ analyzer) — for its own consumers (the AI search's tools, the MCP `session` tool, the CLI); the app's summary surfaces do NOT use it and keep `AnalyzeSessionTranscript` | (Rejected: the app's summary surfaces reading the database's row; the C++ analyzer as an incremental fold inside the service) |
| **D15** | Columns that need the filesystem: the git root, the worktree's main root, the inferred working directory (§5.6) | **The service computes them for every session** with Agentmaster's C++ (`FindGitRootForDir`, `ResolveWorktreeMainRoot`, `InferWorkingDirectory`), probes again when the session changes and once a day, and re-infers every session when `inferGitRoot` changes | The GUI computes them for its open sessions only, as today (closed sessions cannot be grouped by where they worked) |
| **D16** | What refreshes the database (§7.6.1) | **DECIDED by the user (2026-10-14):** the app's observer counts each session's changes (`TranscriptWatcher`'s change sequences, `TakeChangedSessions()`); every refresh tick (setting, default 5 s, 1–60 s) the sessions that changed are upserted (`upsertSessions`), a `resync` after a connect or a re-index. Still open within it: should a service started WITHOUT the app (by the CLI or an MCP client) sweep once at its start (~150 ms)? Recommended yes; no keeps the observer the only refresher, and such clients then read the data as of the app's last tick | (Rejected: the service's own watcher, recommended on 2026-10-13; polling the live sessions' files from Claude's presence files) |

## 14. Measurements

**Environment.** 2026-10-12, this machine (i9-13900K, 64 GB, NVMe), 100+ live claude sessions; Python
3.12; DuckDB **2.0.0-alpha43763** (`fts` `2266e67d01`); `threads` 4, `memory_limit` 3 GB; warm file
cache; best of 3–5 runs. The corpus as in §1.2.

| Step | Result |
|---|---|
| Prototype ingest (DuckDB's JSON reader, 38 batches of ~270 MB) | 10.41 GB, 6,029 files → 3.30 M lines, 1.74 M blocks, 3,570 sessions, 98 K paths in **379 s**; file 3.3 GB |
| Production-shaped table (dedup, 16 KB cap, folded copy, clustered by role) | 1.09 M blocks, 1.35 GB of text, **25.7 s**; file 3.1 GB (uncompressed large strings) |
| String compression (20% sample, 270 MB) | default 329 MB, scan 144 ms; **ZSTD 70 MB**, scan 372 ms; uncompressed 331 MB, 225 ms |
| `fts` index, 20% sample (213 K blocks, 261 MB) | build 53 s; macros 92–162 ms; direct table SQL 6–28 ms; incremental insert 949 ms (1 block) … 2,197 ms (900); delete 100: 1,366 ms |
| `fts` index, full (1.06 M blocks, 1.31 GB) | build **214.5 s**; 429 MB; macros: exact 57.8, prefix 89.4, substring 138.7, `release` 121.1 ms; direct SQL: exact 14.2, `release` 33.4, substring 20.0, prefix 11.3, `str` 78.1, AND of 3 terms 19.5, sessions with hits 19.4 ms; incremental insert 100: 568 ms, 900: 623 ms; delete 100: 175 ms |
| Own postings, full | sorted rebuild **15.0 s**, 75.6 M rows, 317 MB; postings with known term ids: 1.9 ms (2,371 blocks), 19.7 ms (49,006), 223 ms (574,971); `str` expansion (14,149 terms): 39.5 ms distinct blocks, 171 ms exact blocks per session |
| Session postings (`spost`), full | build **2.2 s**, 10.3 M rows, ~41 MB; lookups 1.2 / 1.6 / 2.1 ms (165 / 1,784 / 2,815 sessions); `str` expansion 25.6 ms (2,665 sessions) |
| Dictionary | 909,313 terms, 11.2 MB of term text, average 12.4 characters, 3,458 longer than 64 |
| Scans, full | list 4.2 / 8.4 ms; title / folder / branch 5.1–5.4 ms; paths 26.1 ms; prompts 14–37 ms (clustered, folded) vs 186–273 ms (interleaved, `lower()`); agent text 1.6–2.4 s with `lower()`, 0.56 s raw, 0.38–0.51 s pre-folded; hot 24 h 32 ms, 7 days 118 ms |
| Lossless store, full (`raw_lines.py`) | 3,308,864 lines, 10.42 GB of JSONL (+ 2,800 spills, 0.33 GB) → **2.22 GB** (4.7×) in **232 s**; **20 / 20** sample files rebuilt byte-exact (128.7 MB in 609 ms); fork copies 36.4% of uuid lines, 23.4% of the bytes, 4 of 115 sampled copies byte-identical |
| Access paths (`access_paths.py`, 10% sample: 312,887 lines, 0.98 GB) | sorted vs shuffled: big session 149 / 303 ms, small session 2.9 / 113 ms, one line 2.8 / 3.8 ms, 30 random lines 145 / 142 ms; with ART: 30 random lines 6.9 / 4.2 ms, small session 5.2 / 4.6 ms, big session unchanged (above the threshold; `EXPLAIN ANALYZE`: sequential scan, the others index scans); ART build 0.06–0.10 s and 3.9–5.0 MB per index; a rewrite in another order 2.2 s (a first run under more load: 1.2–1.9× slower) |
| Side folders | `file-history` 2.07 GB (62,708 files), `debug` 3.20 GB, `shell-snapshots` 0.47 GB, `paste-cache` 7 MB, `todos` / `tasks` / `plans` small |
| Today's whole-file readers (`bench_summary.exe`: an optimized build, `/O2`, of the engine's own code) | `AnalyzeSessionTranscript` 2.70–2.85 s (128.7 MB), 2.20 s (102.3 MB), 33–40 ms (1.7 MB), **67.7 s** for NumSharp's 922 sessions (3.11 GB); `RenderTranscriptTurnsFile` at Default 1.62 / 1.37 s / 24 ms; `CollectConversationUpstream` 17 / 21 / 260–309 ms |
| The summary from the database (`summary_from_db.py`; NumSharp's line store: 1,771 files, 4.63 GB, ingested by `raw_lines.py` in 125 s) | A, from the stored lines: 1.91 / 1.53 s / 194 ms. B: the section tables for 922 sessions in 54.4 s (the Python live-branch walk 20.4 s); a summary in 35.7–39.7 ms over 7 queries, **2.75–2.99 ms** as one precomputed row (all rows built in 0.12 s). **921 / 921** sessions equal the analyzer on all 20 facts, through both read paths |
| The session row (`session_columns.py`) | 73 columns for 922 sessions (3.11 GB): the parse 6.7 s, the columns 4.8 s (269 MB/s) |
| The field census (`field_census.py`) | the newest 3.0 GB (332 sessions, 633,905 lines, Claude Code 2.1.123–2.1.291) in 31 s; `cost-state` in 562 of 4,444 top-level transcripts (`rg -l`) |
| A fork's parent (NumSharp's 1.63 M lines) | 29.1 ms by scanning every line's uuid; 8.7 ms through a uuid → first-file map (715,662 uuids, built in 0.1 s); the same 150 copied lines as `CollectConversationUpstream` |
| The ingest load (`append_rate.py`, 2026-10-12/13) | 190 MB of transcript lines in 24 h (38,720 lines, 71 files; 21.7 MB of it fork copies in 19 new files), 2.2 KB/s average, 23.4 MB in the busiest hour; + 32.5 MB of spills; a sweep of the whole tree by directory enumeration (12,749 files, 1,293 folders) 145–150 ms warm, ~0.36–0.39 s with a cold directory cache; the 49 live sessions' presence files read in 6 ms |
| Its cost in DuckDB (`commit_cost.py`, a file database on C:) | COMMIT of a 20-line transaction 1.9–2.3 ms median (p95 3.1–4.6 ms); a 23 MB busy hour 6.6–12.3 s of database time (0.18–0.34% of one core); a merge of 6.9–9.7 K hot lines 0.21–0.28 s; a checkpoint 0.19–0.28 s; a refresh tick's upserts (the `session` + `summary` rows of the changed sessions, one transaction, a 4,500-row table with a primary key): 1–5 sessions ~17 ms median, 20 sessions ~27 ms (2026-10-14) |

**Reproduce** with [`tools/sessiondb-bench/`](../../tools/sessiondb-bench/): Python 3.12 in a venv
outside the repo with `pip install --pre duckdb` (2.0 until it is released). Scratch files go to
`%TEMP%\agentmaster-sessiondb-bench` (or `%AM_SESSIONDB_BENCH_DIR%`; the full run needs ~10 GB free);
the transcripts are only read. The order matters, each step uses the previous one's output:

1. `probe_extensions.py` — the installed DuckDB, its `fts` commit, and whether `incremental` /
   `layered_search` work.
2. `corpus_census.py`, `line_sizes.py` — the corpus (§1.2).
3. `ingest.py <staging db>` (~6 minutes for the full corpus) — line envelopes, blocks, sessions, paths.
4. `bench_scan.py <staging db>` — sizes per role, fork duplication, the scans on the staging layout.
5. `build_layout.py <staging db> <layout db>` — the production-shaped block table.
6. `compress_test.py <layout db>`, `column_compression.py <layout db>` — string compression.
7. `fts_build.py <layout db> 1`, then `fts_probe.py <work dir>\fts_mod1.duckdb` — Option A on all
   blocks. For the 20% numbers: `fts_build.py <layout db> 5 --skip-incremental`, then
   `fts_incremental.py <work dir>\fts_mod5.duckdb`.
8. `bench_layout.py <layout db> <work dir>\fts_mod1.duckdb` (adds the `post` table), then
   `postings_only.py` and `session_postings.py` on the same database — prompts, the hot tier, own
   postings (Option B).
9. `raw_lines.py <line db>` (~6 minutes; independent of the steps above) — the lossless store, the
   byte-exact rebuild check and the fork copies (§5.5); then `access_paths.py <line db>` — clustering
   against ART (§6.8).
10. `field_census.py [--budget-gb N | --all]` — the fields and line kinds Claude Code writes (§5.6); no
    database.
11. The summary and the session row (§8.5, §5.6) on ONE project folder (the numbers above:
    `K--source-NumSharp`):
    - `raw_lines.py <project db> K--source-NumSharp` — that folder's line store (~2 minutes);
    - `build_bench_summary.bat` — builds `bench_summary.exe` into the work folder (~30 s; Visual Studio
      2022's `vcvars64.bat`; no app build, no package);
    - `set AGENTMASTER_PROFILE=<an empty scratch folder>`, then `bench_summary.exe <scratch dir>
      <transcript.jsonl>...` for today's timings, and `bench_summary.exe --facts <facts.tsv>
      %USERPROFILE%\.claude\projects\K--source-NumSharp` for the analyzer's facts of every session (68 s).
      ⚠ Set the variable: a shell hosted by the release carries the LIVE profile's path, and the
      engine code logs through it;
    - `summary_from_db.py <project db> <session id prefixes> --verify <facts.tsv>` — A, B and the
      comparison;
    - `session_columns.py <project db> [session id prefix]` — the session row and its coverage.
12. `append_rate.py [--samples N] [--interval S]` and `commit_cost.py` — what keeping the database
    current costs (§7.6), the refresh tick's upserts included; no database needed for the first, the
    second makes and deletes its own.

## 15. Sources

- DuckDB: [release calendar](https://duckdb.org/release_calendar) · [releases](https://github.com/duckdb/duckdb/releases) ·
  [A preview of DuckDB v2.0](https://duckdb.org/2026/08/17/duckdb-20-highlights) ·
  [Try DuckDB v2.0-dev](https://duckdb.org/2026/09/02/try-duckdb-20-alpha) ·
  [Concurrency](https://duckdb.org/docs/current/connect/concurrency) ·
  [C API](https://duckdb.org/docs/current/clients/c/api) ·
  [Quack: the client-server protocol](https://duckdb.org/2026/05/12/quack-remote-protocol) ·
  [`duckdb-fts`](https://github.com/duckdb/duckdb-fts) (pins read from `.github/config/extensions/fts.cmake`
  at `v1.5.0`, `v1.5.6` and `main`) · [vcpkg port](https://github.com/microsoft/vcpkg/blob/master/ports/duckdb/vcpkg.json).
- MCP: [`mcp-server-motherduck`](https://github.com/motherduckdb/mcp-server-motherduck) ·
  [`duckdb_mcp`](https://github.com/teaguesterling/duckdb_mcp).
- This repository: SESSIONS.md §1, §6, §9; `SessionSearch.{h,cpp}`, `TranscriptStore.{h,cpp}`
  (`ActiveBranchUuids`, `InferWorkingDirectory`, `FindGitRootForDir`), `ProcessInspect.Content.cpp`
  (`AnalyzeSessionTranscript`, `CollectConversationSegments`, `SeIsCommandNoise`, `SeSummaryFileLabel`),
  `TranscriptTurns.h` (`CollectConversationUpstream`), `Persistence.h` (`NormDirKey`,
  `EffectiveWorkingDir`, `SessionColorKeyDir`), `TerminalPage.AgentSessionsPage.cpp`,
  `SessionSearchTools.h`; the release `hooks.log` `[nav] sessions` and `[transcript]` lines.

## 16. What is built (2026-10-15, NOT BUILT into the app)

Asked for on 2026-10-14 ("Kindly implement fully"); built between c0253b8b0 and 41fe5e493. Nothing here is in a
build of the app yet: the service and the CLI were built standalone (`sessiondb\_compile.bat`, `cli\_compile.bat`)
and run on scratch profiles and on the whole corpus; the app's files pass `cl /Zs` with the last app build's command
lines; the m5 suite runs `TestSessionDb`. The first msbuild of the solution (the user's next app build, or CI)
compiles `agentmaster-sessiondb.vcxproj` for the first time.

### 16.1 The pieces

| Piece | Files | Commit |
|---|---|---|
| The store: schema, the lossless line store (D11), the ingest of every line, the fold, the live-branch walk, the session row (~110 columns), the section tables, the summary (D14), `upsertSessions` / `resync` | `sessiondb\SessionDbSchema.*`, `SessionDbExtract.*`, `SessionDbFold.*`, `SessionDbWalk.h`, `SessionDbStore.*`, `SessionDbSummary.*`, `SessionDbVerify.*`, `DuckDbWrap.*`, `SessionDbLog.*`; libduckdb pinned by `tools\Get-DuckDb.ps1` | c0253b8b0 |
| The text index and the search: tokenizer, dictionary, postings, the hot tier and merges, the re-cluster, tiers name > path > prompt > text, BM25, snippets, D8, D9 | `AgentMaster\SessionDbTokenizer.h`, `sessiondb\SessionDbIndex.*`, `SessionDbSearch.*` | 1d0e03134, 7804fb2c2, 43762d44a |
| The service: the per-profile pipe server (a query pool of 4, one writer thread, deadlines, `cancel`, idle exit after 10 min), the tool registry (§8.4.3), the stdio MCP server `agentmaster`, the one-shot diagnostic modes | `sessiondb\SessionDbServer.*`, `SessionDbTools.*`, `SessionDbMcp.cpp`, `sessiondb_main.cpp`; `AgentMaster\SessionDbProtocol.h`, `SessionDbToolDefs.h` | 4a1f44348, 43762d44a |
| The client the GUI, the CLI and the MCP server share (connect, start a service, versions, `Call`, `CancelPending`, the copy-out to `<profile>\sessiondb\bin`) | `AgentMaster\SessionDbClient.*` | 4a1f44348 |
| The app's refresh tick (D16): the watcher's change set, the refresher thread, the facts epoch, the Settings cog's SESSION DATABASE section | `TranscriptWatcher.*`, `SessionScanner.*`, `SessionStore.*`, `AgentMaster\SessionDbRefresher.*`, `Engine.*`, `AgentManagerContent.Settings.cpp` | 4139b6d6e |
| The Sessions page's content phase (`pageSearch`; Shadow's `[sessdb-diff]`), its ↻ asking for a sweep | `TerminalPage.AgentSessionsPage.cpp` | 7804fb2c2, 43762d44a |
| The CLI verbs `search`, `sql`, `sessdb` | `cli\agentcli.cpp`, `wt\shim.cpp` | 7804fb2c2 |
| The AI search on the database (§8.2) | `SessionSearchTools.*`, `SessionSearchAgent.*` | 43762d44a |
| Packaging: the service's project and its standalone twin, `duckdb.props`, the solution entry, the package reference and `duckdb.dll` as package content, libduckdb in the build wrapper and both workflows | `sessiondb\agentmaster-sessiondb.vcxproj`, `_compile.bat`, `duckdb.props`, `OpenConsole.slnx`, `CascadiaPackage.wapproj`, `tools\Build-Agentmaster.ps1`, `.github\workflows\*.yml` | 50df738c1 |
| Tests | `tests\tests_sessiondb.cpp` (`TestSessionDb`: protocol names and framing, the tool definitions, the tokenizer, the settings clamps, the change set, the facts epoch, the AI search's database mode) | 43762d44a, 41fe5e493 |

### 16.2 The decisions as built

| # | What the build does |
|---|---|
| D1 | Our own inverted index in DuckDB tables, as recommended. |
| D2 | **DuckDB 1.5.6**, not 2.0: 2.0.0 ships on 2026-10-21 and had no C-library build on 2026-10-15. The pin is `tools\Get-DuckDb.ps1` + `sessiondb\duckdb.props` (move both). |
| D3 | A per-profile service process, as recommended. |
| D4 | Everything is indexed, as recommended; `kMaxIndexedChars` caps one block's tokenization at 64 K characters. |
| D5 | One database per profile. |
| D6 | **Not built**: no Settings button. Register by hand: `claude mcp add --scope user agentmaster -- "<profile>\sessiondb\bin\agentmaster-sessiondb.exe" mcp`. |
| D7 | Kept and searchable: a transcript gone from disk keeps its lines and its session row, marked deleted (`src_file.missing_since`; `v_sessions.transcript_present` false). |
| D8 | As recommended: a 1-2 character term never uses the index; in a query with a longer term it is checked on the text of that term's candidates. |
| D9 | Subsequence matching over the in-memory dictionary (`TermsWithSubsequence`): today's `(F)` rule, but over terms, not a scan of the text. Not edit-distance typo tolerance. |
| D10 | Shipped: `duckdb.dll` in the package root and the portable zips. |
| D11 | Yes: every line verbatim; a transcript rebuilds byte for byte (`rebuild`, checked on transcripts up to 102 MB). |
| D12 | No `file-history`. |
| D13 | **Half**: Claude Code's own `cost-state` totals where a session has them; no price table, no estimate (the `usage` tool says so). |
| D14 | Decided and built: the database forms the summary (`summary` rows, `sessionSummary`, the `session` tool); the app's summary surfaces keep `AnalyzeSessionTranscript`. `verify-summary` re-forms the rows with the running build's rules and compares the two field by field: §16.4. |
| D15 | The service computes them for every session: the git root (`FindGitRootForDir`, memoized), the worktree's main root, the inferred working directory (`InferWorkingDirectory` over its touched paths); only on a local, ready drive (a UNC or remote path is never probed). |
| D16 | Decided and built: the watcher's change set, a tick every `sessionDbRefreshSeconds` (default 5), `upsertSessions` for what changed, `resync` after a connect, a re-index, or every 30 s while the watch is unhealthy. Its open question is answered **yes**: a service started without the app (by the CLI or an MCP client) sweeps once at its start. |

### 16.3 Where the build differs from the design

- **The text column's compression** (1d0e03134). A ZSTD block is served by decompressing its whole 2,048-row
  vector: 30 scattered blocks took 145 ms, 1,000 took 2.95 s. Conversation text (prompts, replies, thinking,
  system) therefore stays in DuckDB's default compression in `doc.text` (1,000 blocks in 47 ms); only tool text is
  ZSTD, in `doc_tool`. The line store's `raw` is ZSTD.
- **Merges run in slices** (43762d44a). The design's one-transaction merge failed with "Out of Memory" under the
  default 1 GB limit during the first full cold build, and every failure left a bigger hot tier (§16.4).
  `TextIndex::Merge` now merges about `mergeSliceBytes` of text per transaction (a sixteenth of the memory limit:
  64 MB at 1 GB), then moves the hot lines in their own transaction, sliced.
- **The re-cluster has two parts with their own triggers** (7804fb2c2, 43762d44a), not "weekly, or after 50
  merges" (§6.4):
  - the **text index** (`post` re-sorted by term, `spost` and df / sf recomputed exactly, the tombstoned blocks
    deleted from `doc` / `doc_tool`): when the blocks merged since the last one are a quarter of the index (and at
    least 20,000), or after a day with any new block. Its three new tables are built by statements that commit one
    by one, and only the swap is one transaction: inside one transaction the same statements ran out of memory at
    full scale. `doc` and `doc_tool` are never copied (every merge appends them in id order);
  - the **line store**: only once it holds 64 sorted runs (a merge that interleaves files adds one; meta
    `line_runs`), re-sorted SLICE BY SLICE, each slice moved to the end of the table in its own transaction (insert
    it sorted, delete the old copies by `rowid`), the resume point in meta `line_resort_from`. A waiting write job
    pauses it without losing a slice; the next idle period resumes it; an already-sorted prefix stays in place; the
    file grows by one slice, not by a copy of the table. A cold build leaves the line store sorted (its 44 slices
    were all found in place).
  - Both run only after 120 s without a write job; a failure waits 15 min, doubling up to a day.
- **The Sessions page** uses the database for its CONTENT phase only (`pageSearch`, §8.1's "search"): the page keeps
  its own gather, its fast phase (titles, folders, touched paths) and its rows. `listSessions` for the list,
  `sessionDetail` for the detail pane and the virtualized table are not built.
- **The AI search** gets the ten tools of §8.4.3 (not the redesign of SESSIONS_AGENT.md) when the mode is On at
  the app's start: the agent lives for the process, so a change of the mode reaches it at the next start. Shadow
  keeps the page search. `show` resolves the short ids the database prints through a new `resolveIds` method.
- **The tool registry's text** lives in `AgentMaster\SessionDbToolDefs.h`, one copy for the service and the app.
- **The summary's rules carry a version** (`SessionDbSummary.h` `kSummaryVersion`, meta `summary_version`). A summary
  row is re-formed only when its session changes, so after a change of the rules the store re-forms every row once,
  at the end of a complete sweep (`Store::RebuildSummaries`, batches of 256); a new database is stamped current. Version
  2 (2026-10-15): the CURRENT prompt list starts after the last ON-BRANCH compaction, as the analyzer's main walk does
  (version 1 started it at the last compaction by file position, which disagreed with the analyzer on a session whose
  only compaction was off-branch: 2 prompts listed against 9).

### 16.4 The full corpus, measured (2026-10-15)

This machine's whole corpus: 11,898 files, 11.3 GB — 3,638 top-level transcripts (8.5 GB; 3,595 sessions, the other
43 files are empty), 2,406 subagent transcripts (2.1 GB), 1,518 subagent meta files, 4,335 spilled outputs (0.6 GB)
and `history.jsonl`. The service
built standalone (x64 /O2 /MT), DuckDB 1.5.6, `threads` 4, `memory_limit` 1024 MB (the defaults), under heavy load
(100+ claude sessions, other builds, 3–13 GB of free commit).

| Step | Result |
|---|---|
| First cold build (the one-transaction merge) | the sweep finished in **44 min**, but every merge failed with "Out of Memory Error" from minute 25 on: 401,537 blocks (640 MB of text) and 1.13 M lines (2.4 GB) were left in the hot tier |
| The sliced merge on that database | the 401,537 hot blocks and 2.4 GB of hot lines into the index in **155 s**, no error |
| **Clean cold build** (sliced merges) | **37 min** (2,223 s): 3,355,715 lines, 1,533,061 blocks, 1,118,783 terms, 127.6 M postings; steps: ingest 722 s, merges 705 s (111 merges, 120 slices), commit 357 s, session rows 175 s, summary 113 s, cursors 62 s, appenders 33 s, walk 23 s, the final checkpoint 19 s; no failure |
| The file after it | **6.9 GiB**: 3.5 GiB of used blocks, 3.5 GiB free (the hot tier is written, then deleted by the merges; DuckDB reuses free blocks but does not shrink the file). The design estimated 3.1 GB for 10.4 GB of transcripts |
| Forced re-cluster (`recluster`) | text index **52 s** (127.6 M postings); line store **0.7 s** (44 slices, all in place) |
| Search, warm (second run of the one-shot `search`) | `duckdb` 313 ms, `tab drag crash` 594 ms, `the` 391 ms (2,906 of 3,595 sessions); the postings step 263–503 ms of it; a first run 750–1,437 ms. After the forced re-cluster: 329 / 688 / 468 ms |
| The service's memory | working set 1.3–2.0 GB during a cold build at the 1 GB DuckDB limit (the dictionary, the ART indexes and the batch live outside DuckDB's limit) |
| Summary parity (D14, `verify-summary`: the summary row against `AnalyzeSessionTranscript`, 18 fields) | rules version 1, the newest 296 sessions: 295 agreed (the one miss: a session whose only compaction was off-branch, §16.3). Version 2: the same 296 all agree; then **every session: 3,591 of 3,591 agree on every field** (the other 4 of the 3,595 skipped: their transcript grew after the ingest), in 336 s including re-forming their summary rows |
| On the 900 MB scratch database | the old one-statement line re-sort ran out of memory; the slice re-sort paused after 2 of 7 slices (471 MB, 11 s) and resumed the other 5 (24 s) with an identical content checksum; a third run found all in place (22 ms) |

The search is 5–100× faster than today's slow phase (2–42 s), but slower than the prototype's direct queries
(§6.7): the postings step joins every term of a word's substring expansion with all 127.6 M postings (a hash
semi-join DuckDB cannot prune), and a re-cluster does not change that.

### 16.5 Open

- **The first app build and a live run** — Off by default; then Shadow, and read hooks.log's `[sessdb-diff]` lines
  (the database against today's search on the page's own sessions) before switching to On.
- The postings cost: keep each term's row range in `post` after a re-cluster in the service's memory (§6.8) and
  read those ranges instead of a semi-join.
- A smaller file: a compaction after the cold build (copy into a fresh file), or a cold build that skips the hot
  tier (tokenize at ingest); the merges and commits are 48% of the cold build.
- D6 (the MCP button), D13's price table, D2 → 2.0 once its C library ships.
- §8.1's `listSessions`, `sessionDetail` and the virtualized table; SESSIONS_AGENT.md's redesigned tools.
- The AI search follows a change of the mode only at the next app start.

### 16.6 Running it by hand

- `pwsh -File tools\Get-DuckDb.ps1 -Arch x64`, then `src\cascadia\TerminalApp\AgentMaster\sessiondb\_compile.bat
  <output folder>`: `agentmaster-sessiondb.exe` with `duckdb.dll` beside it.
- One-shot modes (`--profile <dir>`; ⚠ never a live profile while its service runs — they open the file
  themselves): `resync [--limit N]` (a sweep; N newest sessions), `search <query>` (cold and warm times, the steps),
  `sql "<statement>"` (it CAN write), `recluster [--limit N]` (both parts; `--limit` pauses the line re-sort after N
  slices), `verify-summary [--limit N]` (D14's comparison), `tool <name> <json>`, `call <method> <json>` (through a
  running service), `serve [--limit N]`, `mcp`, `--version`.
- The CLI: `agentmaster search <words>`, `agentmaster sql "<SELECT>"`, `agentmaster sessdb status|stop|resync|rebuild`
  (CLI.md §5).
- Logs: `<profile>\sessiondb\sessiondb.log` (`[sessdb]`), hooks.log (`[sessdb]`, `[sessdb-diff]`, `[ai-search]`).
