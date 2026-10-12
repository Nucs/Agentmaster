# Agentmaster — the session database (DuckDB): one index for every session on the machine

> **Status: DESIGN (2026-10-12). Not implemented.** Every number here was measured on this machine's
> real corpus, with a prototype of the schema in a scratch database (DuckDB 2.0.0-alpha through Python;
> the scripts are in [`tools/sessiondb-bench/`](../../tools/sessiondb-bench/), §14). The decisions D1–D13
> (§13) are open; the recommendations are marked. Companions: [`SESSIONS.md`](./SESSIONS.md) (the Sessions
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
  within about 2 s (§7.4). `duckdb.dll` is 36.7 MB (x64, 1.5.6): about +13 MB per architecture in the
  downloads (D10).
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
- **G3 — freshness.** Text that a running claude writes is searchable within ~2 s.
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
  it runs.
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

-- One row per session: the browse list, the sidecar's TranscriptStats, lineage, usage totals.
CREATE TABLE session (
    sid_key        INTEGER PRIMARY KEY,
    session_id     UUID NOT NULL UNIQUE,
    file_id        INTEGER,
    project_dir    VARCHAR,             -- the encoded folder name (lossy)
    cwd            VARCHAR,             -- the first line's cwd (the truth)
    cwd_fold       VARCHAR,             -- folded, '/'-separated: the folder search and grouping key
    git_branch     VARCHAR,
    created_ms     BIGINT,              -- fork-aware (a fork: the file's birth time)
    last_activity_ms BIGINT,            -- the last non-sidechain user/assistant timestamp
    custom_title   VARCHAR, ai_title VARCHAR, legacy_summary VARCHAR, first_prompt VARCHAR,
    forked_from    UUID, fork_root UUID,
    user_prompts   INTEGER, assistant_lines INTEGER, tool_uses INTEGER, subagents INTEGER,
    context_tokens BIGINT,              -- the newest turn's usage sum, or a compaction's postTokens
    in_tokens BIGINT, out_tokens BIGINT, cache_read_tokens BIGINT, cache_write_tokens BIGINT,
    model          VARCHAR, cc_version VARCHAR, size_bytes BIGINT,
    transcript     UTINYINT,            -- 0 present, 1 deleted by Claude's sweep (D7), 2 replaced
    first_doc_id   UINTEGER, last_doc_id UINTEGER  -- the session's block range after the last re-cluster (§6.8)
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
    uuid UUID, type VARCHAR, ts_ms BIGINT, bytes INTEGER, raw VARCHAR
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
first prompt), `v_messages` (`doc` ∪ `doc_hot` with role names), `v_lines` (`line` ∪ `line_hot`: the raw
JSON of every line, for `json_extract` over fields no other view has), `v_paths`, `v_usage`, `v_links`.
The read-only SQL tool sees only these views (§10); their columns are listed in the `sql` tool's
contract (§8.4).

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
- **Later:** the transcript overlay, the summary panel and the Copy ▸ Transcript levels can read a
  session from the database instead of its file.
- **SQL over any field:** `v_lines` plus `json_extract`.

**Not covered: side folders outside `projects/`.** `file-history/` (2.07 GB, 62,708 files: the pre-edit
snapshots that `/rewind` restores; D12), `paste-cache/` (7 MB), `todos/`, `tasks/`, `plans/` (small).
`debug/` (3.2 GB) and `shell-snapshots/` (0.47 GB) are logs and environments, not session content.

**Caveats.** The prototype read the spills as text, and DuckDB's `read_text` refused 4 of its 11
batches (files that are not valid UTF-8), so the store keeps spills as BLOB. A live transcript keeps
growing, so a rebuild equals the bytes stored so far; the check compared exactly those bytes.

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
usage deduplicated by message id, and no duplicate of a tool result through `toolUseResult`.

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

- The watcher (the `TranscriptWatcher` code: one `ReadDirectoryChangesW` on `projects`, subtree on)
  plus a 30 s resync.
- Per changed file: read from `parsed_bytes` to the last complete line; classify; append to `doc_hot`,
  `prompt`, `path_access`, `usage`; update the `session` row and the cursor; **one transaction**.
- From a line written to it being searchable: **≤ 2 s** (batching window + one transaction).

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
goes with them:

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

- **Description:** "Token use, and an estimated cost, grouped by session, day, model or folder. Each API
  message counts once. The cost comes from a price table and is an estimate, not a bill."
- **Parameters:** `group_by` (required: `session` | `day` | `model` | `dir`), `days` (default 30),
  `after`, `before`, `dir`, `id`, `limit` (1–200, default 20); `additionalProperties: false`.
- **Result:** `{"rows": [{"key", "input", "output", "cache_read", "cache_write", "cost_usd"}], "totals": {…}, "prices": "<price table version>"}`.
- **Errors:** an unknown `group_by`; a bad date.
- **Plan:** `usage` grouped (deduplicated by message id at ingest).
- **Budget:** 50 ms.
- **Open:** the price table (D13).

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
  v_sessions(id, uuid, title, dir, branch, created, active, msgs, tools, model, input_tokens,
  output_tokens, forked_from, favorite, tags, open); v_messages(id, ref, time, role, tool, text);
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

## 9. The protocol

- **Transport:** the named pipe `\\.\pipe\agentmaster.sessiondb.<hash>`, `PIPE_REJECT_REMOTE_CLIENTS`,
  the default DACL (the user, SYSTEM, administrators). A discovery file
  `<profile>\sessiondb\service.json` names the pipe, the pid and the version (the `bridge.json` idiom).
- **Framing:** a 4-byte little-endian length, then UTF-8 JSON. Request `{v, id, method, params}`;
  response `{id, result}` or `{id, error: {code, message}}`; `{method: "cancel", params: {id}}` interrupts
  that request's query.
- **Methods:** `hello` (versions: service, schema, tool registry), `listSessions`, `search`,
  `sessionDetail` (the `session` tool), `messages`, `grep`, `files`, `whoTouched`, `timeline`, `usage`,
  `related`, `sql`, `rebuild` (a transcript from the lossless store, or the whole index), `status`,
  `factsChanged` (the GUI says a title, tag, favorite or link changed), `openChanged` (the GUI's open
  sessions), `shutdown`. Each tool of §8.4 maps to one method.
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
| The schema version differs | A newer service migrates (or rebuilds); an older one refuses a newer schema and never writes it. |
| A new DuckDB version | The storage format reads forward; the index is ours, so only a change of `meta.tokenizer_version` forces a rebuild. |
| The disk is full | Ingest pauses and logs; searches keep working on what is there. |
| Two installs on one profile | One service (the mutex); the profile mutex already warns. |

## 12. Rollout and tests

| Phase | Content |
|---|---|
| **P0 spike** | A new project `agentmaster-sessiondb` linking `duckdb.dll` (2.0.0), shipped in the wapproj, the portable zip and CI (x64 and arm64); the copy-out and a hot-swap with an MCP client connected; appender throughput, `duckdb_interrupt`, and **the C++ cold build measured on this corpus**. |
| **P1 engine** | Schema and migrations; the lossless line store (D11) and its byte-exact rebuild check; `ClassifyTranscriptBlocks` shared with the sidecars; tokenizer; cold build, incremental ingest, merge, re-cluster; the planner; the pipe server; harness tests; `agentmaster sessdb status`. |
| **P2 page** | The virtualized table (can ship first, on today's data); `SessionDbClient`; list / search / detail through the service behind `AppSettings::sessionDb` = Off / Shadow / On. **Shadow** runs both and logs timings and differences (`[sessdb-diff]`). |
| **P3 AI search** | On the service; the tool registry of §8.4.4 with the contracts of §8.4.3 (`scopes` replace `deep`); a live trace per tool. |
| **P4 CLI and MCP** | The verbs; `mcp` mode; the Settings button (D6). |
| **P5 cleanup** | After weeks of Shadow parity: remove the sidecars and the ripgrep path; the inferred-folder scan reads `path_access`. |
| **Later** | `vss` semantic search, Codex rollouts, a usage view. |

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
| **D13** | `usage` costs | **A price table** shipped with the app, versioned, labeled an estimate | Tokens only, no cost |

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
- This repository: SESSIONS.md §1, §6, §9; `SessionSearch.{h,cpp}`, `TranscriptStore.{h,cpp}`,
  `TerminalPage.AgentSessionsPage.cpp`, `SessionSearchTools.h`; the release `hooks.log` `[nav] sessions`
  lines.
