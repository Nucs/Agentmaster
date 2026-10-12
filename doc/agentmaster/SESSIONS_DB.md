# Agentmaster — the session database (DuckDB): one index for every session on the machine

> **Status: DESIGN (2026-10-12). Not implemented.** Every number here was measured on this machine's
> real corpus, with a prototype of the schema in a scratch database (DuckDB 2.0.0-alpha through Python;
> the scripts are in [`tools/sessiondb-bench/`](../../tools/sessiondb-bench/), §14). The decisions D1–D10
> (§13) are open; the recommendations are marked. Companions: [`SESSIONS.md`](./SESSIONS.md) (the Sessions
> page, its search and the AI search as they work today), [`SDK_HARNESS.md`](./SDK_HARNESS.md) (the agent
> harness the AI search runs on), [`CLI.md`](./CLI.md), [`PROFILES.md`](./PROFILES.md).

## 0. Summary

- **The problem (measured, §1).** The Sessions page's content search takes **2 to 42 s** (release
  `hooks.log`: `release` 8.9 s, `str` 33.5 s, `string` 41.7 s). A page open reads one sidecar file per
  session (15–30 s when Windows has not cached them; 3 minutes when they are missing). Every keystroke
  rebuilds the whole table as XAML (about 12 elements per row, up to 2,500 rows).
- **The proposal.** One **DuckDB** database per profile holds every Claude Code session on the machine:
  the session facts, **the full text of every message and tool block** (ZSTD-compressed), the files each
  session touched, token usage and Agentmaster's own facts (titles, tags, favorites, links), plus an
  **inverted index** for full-text search. A small **per-profile service process** owns the file and
  serves it, over a named pipe, to the Sessions page, the AI search, the `agentmaster` CLI and a new
  **MCP server** that any Claude Code session can use.
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

- **Size and cost.** About **0.9 GB** on disk for today's corpus (§5.4). A cold build is estimated at
  about 2 minutes in C++ (§7.3); new text is searchable within about 2 s (§7.4). `duckdb.dll` is
  36.7 MB (x64, 1.5.6): about +13 MB per architecture in the downloads (D10).
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
    transcript     UTINYINT             -- 0 present, 1 deleted by Claude's sweep (D7), 2 replaced
);

-- Every searchable block, cold (merged into the index). Full text, ZSTD.
CREATE TABLE doc (
    doc_id    UINTEGER NOT NULL,        -- dense, ingest order
    sid_key   INTEGER NOT NULL,
    file_id   INTEGER, line_off BIGINT, -- where the line starts in its file
    uuid      UUID, block USMALLINT,    -- the line's uuid and the block's index in it
    ts_ms     BIGINT,
    role      UTINYINT,                 -- 1 typed prompt, 2 reply, 3 thinking, 4 tool call, 5 tool result,
                                        -- 6 system, 7 queued prompt (queued_command, human), 8 subagent brief/report
    tool      VARCHAR,                  -- the tool's name (calls, and results through tool_use_id)
    flags     UTINYINT,                 -- meta | noise | sidechain | subagent | error | compact summary
    text_len  INTEGER,                  -- the length before any cap
    text      VARCHAR USING COMPRESSION zstd
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
    doc_id UINTEGER NOT NULL, sid_key INTEGER NOT NULL, file_id INTEGER, line_off BIGINT,
    uuid UUID, block USMALLINT, ts_ms BIGINT, role UTINYINT, tool VARCHAR, flags UTINYINT,
    text_len INTEGER, text VARCHAR, text_fold VARCHAR
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

Runtime state stays out of the database: which sessions are open, presence, the hidden-sessions list
(a view preference in `settings.json`). The GUI applies them to the rows it gets back.

### 5.3 Views (the SQL and MCP surface)

`v_sessions` (display title by the page's precedence: stored title > custom > AI > legacy summary >
first prompt), `v_messages` (`doc` ∪ `doc_hot` with role names), `v_paths`, `v_usage`, `v_links`.
The read-only SQL tool sees only these views (§10).

### 5.4 Size for today's corpus

| Part | Measured or derived from | Size |
|---|---|---|
| Block text, deduplicated, full length, ZSTD | 1.35 GB capped at 16 KB → ~350 MB at the measured 3.9× | ~0.4 GB |
| `post` (75.6 M rows) + `dict` (0.9 M terms) | measured: 317 MB for the clustered postings | ~0.35 GB |
| `spost` (10.3 M rows) | measured: ~41 MB | ~0.04 GB |
| `prompt`, `session`, `path_access`, `usage`, the rest | estimated | < 0.1 GB |
| **Total** | | **~0.9 GB** |

The prototype's unoptimized files were 3.3 GB (raw staging, uncompressed text) and 2.0 GB (the `fts`
extension's index over uncompressed text); §14 lists them.

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
- **Re-cluster** (weekly, or after 50 merges; when idle; interruptible): rebuild `post` and `spost` in
  one sorted `CREATE TABLE … AS … ORDER BY term_id, doc_id`. Measured: **15 s** for 75.6 M postings.
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
- `RunSessionSearch` becomes `search`; the `deep` search is now cheap, so it is on by default.
- The model gets the read tools of §8.4, so it can check a candidate (read its prompts, grep it) before
  it shows it. The 43 s request of SESSIONS.md §9.9 would be dominated by model time (~3–5 s).

### 8.3 The CLI

New verbs: `agentmaster search <query> [--scopes …] [--days N] [--json]`, `agentmaster sql "<select>"`,
`agentmaster sessdb status | rebuild | stop`. They are pipe clients; the existing verbs are unchanged.

### 8.4 The MCP tools

The same tool set serves two clients: the in-app AI search (tool server `sessions`, in-process, calling
the service) and **any Claude Code session** through the stdio MCP server
`agentmaster-sessiondb.exe mcp` (server name `agentmaster`, registered with
`claude mcp add --scope user agentmaster -- "<profile>\sessiondb\bin\agentmaster-sessiondb.exe" mcp`, or
from a Settings button — D6). Every tool is read-only, returns compact JSON with 8-character session
ids (as today), and caps its rows.

| Tool | What it answers | Main parameters |
|---|---|---|
| `search` | Which sessions match (today's tool, now over everything, with BM25 and snippets) | `query`, `scopes`, `days` / `after` / `before`, `dir`, `branch`, `tag`, `favorite`, `open`, `sort` (relevance / active / created), `limit` |
| `session` | One session's facts: titles, folders, branch, times, lineage (fork family, plan restart, `/clear`, `/handover`), tags, counts, tokens, model, top files | `id` |
| `messages` | Read a conversation in pages: prompts, replies, tool calls, results | `id`, `roles`, `after`, `around`, `limit` |
| `grep` | Exact text or a regex in one session or over a filter, with context | `pattern`, `regex`, `ids` / `dir` / `days`, `roles`, `limit` |
| `files` | What a session read, edited, wrote, searched, ran | `id`, `op` |
| `who_touched` | Which sessions touched a file or a folder (prefix or glob), newest first | `path`, `days`, `op` |
| `timeline` | What happened when: turns per session per hour or day | `from`, `to`, `dir` |
| `usage` | Tokens (and an estimated cost) by session, day, model or folder | `group_by`, `days`, `dir` |
| `related` | Lineage plus sessions that share files or rare terms | `id`, `limit` |
| `sql` | Anything else, read-only, over the documented views (§5.3) | `query` (one SELECT; 200 rows; 2 s) |
| `show` (in-app only) | Put sessions on the screen (today's `display.show`) | `ids` / `top` |

Examples a Claude Code session can do with them: "find the session where we designed the delivery gate
and list its decisions" (`search` → `messages`); "which sessions edited `TerminalPage.cpp` this week"
(`who_touched`); "how many tokens did NumSharp cost in September" (`usage`); "what did I ask about DuckDB"
(`grep` with roles = prompts).

## 9. The protocol

- **Transport:** the named pipe `\\.\pipe\agentmaster.sessiondb.<hash>`, `PIPE_REJECT_REMOTE_CLIENTS`,
  the default DACL (the user, SYSTEM, administrators). A discovery file
  `<profile>\sessiondb\service.json` names the pipe, the pid and the version (the `bridge.json` idiom).
- **Framing:** a 4-byte little-endian length, then UTF-8 JSON. Request `{v, id, method, params}`;
  response `{id, result}` or `{id, error: {code, message}}`; `{method: "cancel", params: {id}}` interrupts
  that request's query.
- **Methods:** `hello`, `listSessions`, `search`, `sessionDetail`, `messages`, `grep`, `files`,
  `whoTouched`, `timeline`, `usage`, `related`, `sql`, `status`, `rebuild`, `factsChanged` (the GUI
  says a title, tag, favorite or link changed), `shutdown`.
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
| **P1 engine** | Schema and migrations; `ClassifyTranscriptBlocks` shared with the sidecars; tokenizer; cold build, incremental ingest, merge, re-cluster; the planner; the pipe server; harness tests; `agentmaster sessdb status`. |
| **P2 page** | The virtualized table (can ship first, on today's data); `SessionDbClient`; list / search / detail through the service behind `AppSettings::sessionDb` = Off / Shadow / On. **Shadow** runs both and logs timings and differences (`[sessdb-diff]`). |
| **P3 AI search** | On the service, `deep` on by default, the new read tools. |
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
