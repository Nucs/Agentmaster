# The session store moves from `sessions.json` to SQLite (designed and implemented 2026-11-20)

**What this is.** The plan that replaces `<profile>\sessions.json` with a SQLite database, `<profile>\sessions.db`, and
(§8) what was built from it: `FleetDb.{h,cpp}` and `FleetWriter.cpp`, the places it differs from the plan, and its tests.
Today every saved change rewrites the whole JSON file. In the new store a change writes about one kilobyte to a
write-ahead log. The owner asked for a plan that (a) uses memory better and never rewrites the whole file, (b) has the
highest performance and safety, and (c) survives a sudden shutdown.

**The owner's decisions (2026-11-20, from the measurements in §3):**

| | Decision |
|---|---|
| D1 Engine | **SQLite in WAL mode**, with pages made as small as SQLite allows so a change writes as few bytes as possible. The rejected options are DuckDB and an in-house journal (§3). |
| D2 Prompt history | **It stays in each session's record.** Nothing is dropped, and there is no lazy loading. On disk the history is split into small rows (§4.2), so keeping it costs nothing per change. |
| D3 Durability | **Critical changes are on disk before their call returns; routine changes within 250 ms** (§4.4). |
| D4 Older builds | **They never write the new store.** The owner: "We want to permanently move from json to sqlite. on first launch, perform a transaction migration where at the end the sessions.json is deleted. make sure to do this safely with no chance of loss of data in the transaction." (§4.8, §4.9) |

## 1. Why: what the JSON file costs today

Measured on production 0.8.0.17 on 2026-11-20. The finding is in the catalog's Changelog.

- **Disk.** Agentmaster.exe wrote **19.8 GB in its first 11.3 h**, about 40 GB a day. About 85 % of that was whole
  rewrites of `sessions.json`:
  - The file is 13 MB and holds 2,190 sessions. 12 MB of it is 9,396 already-sent prompts, median 70 days old.
  - It was rewritten 3–6 times a minute at 05:00 at night, and the code allows up to once a second.
  - Every rewrite is a temp file, a `FlushFileBuffers` and a write-through rename.
  - `SessionAutosaver` (Engine.cpp) saves 300 ms after the last change, at most 1 s late. `SaveSessions` skips a byte-identical
    text, but that rarely helps: the draft stamp `pendingInputUnixMs` is refreshed quietly at every draft scan
    (`SessionRegistry::SetPendingInput`, 197 sessions with a draft), and hook timestamps move.
- **The UI thread.** Nine seams in TerminalPage (the window's UI layer, most of them on its UI thread) call
  `SaveSessions(_sessionRegistry->Snapshot())` synchronously. Each copies the whole fleet, serializes 13 MB and writes it.
  A !live notify saves once more, synchronously, on the thread that raised it.
  - `_ArchiveClaudeSession`
  - `_ArchiveAndCloseClaudeTab`
  - `_CloseClaudeSessionsInFolder`
  - `_RemoveSessionRecord`
  - `_ArchiveWindowSessionsOnTeardown`
  - `_BindClaudeSessionToTab`
  - `_SweepClaudeLivenessImpl`
  - `_ScanPendingInputImpl`
  - `_HandleClosePaneRequested`

  The benchmark (§3) measured one such save at **323–589 ms** in Python.
- **Memory.**
  - Every save takes a deep copy of every record (`Snapshot()`), a JSON tree of the fleet, a 13-million-character UTF-16
    string and a UTF-8 copy of it.
  - The dedup keeps the last text for good: `s_last`, about 26 MB as UTF-16; the 2026-10-19 leak hunt measured 24.6 MB.
- **Rule #4's window.** The scheduler marks a prompt Sent with `_registry->Update(…)` and then types it in at once
  (`Scheduler.cpp`, the advance). The mark reaches the disk through the debounced autosaver up to about 1 s LATER. A crash in
  that second restores the prompt as Pending, and the runner can send it twice.

## 2. Goals and acceptance targets

| Target | Today | Required |
|---|---|---|
| Bytes written per routine change | 13 MB (the whole file) | ≤ ~1.5 KB, the WAL frames plus the change's share of a checkpoint |
| Disk work on the UI thread | 9 seams at 0.3–0.6 s each | a NORMAL commit of the changed rows (~0.1–0.5 ms), never an fsync, never a fleet copy |
| Prompt marked Sent → typed into claude | persisted ≤ ~1 s after | durable BEFORE the typing (Rule #4 closed) |
| A process kill loses | ≤ ~1 s of changes | no committed change |
| A power cut loses | ≤ ~1 s | at most 250 ms of routine changes, no critical change |
| Damage from a torn write | none (atomic rename) | none (SQLite's WAL frame checksums) |
| Memory per save | fleet copy + JSON tree + 13 M-char strings; 26 MB kept | the changed rows only; ~3 MB of last-written docs kept (§4.3) |
| Start-up load | parse 13 MB | ≤ today's |

## 3. Measurements

Taken 2026-11-20 on this PC's C: (the profile's drive, an NVMe) with a copy of production's real fleet: 2,190 sessions
and 9,466 prompts. The scripts are in [`tools/sessions-store-bench/`](../../tools/sessions-store-bench/README.md).
- Everything ran in Python with the PC about 50 % busy, so absolute times are pessimistic. Compare the rows with each other.
- A *batch* is one commit of K changed sessions. *Relaxed* = left in the OS cache; *durable* = flushed before returning.

### 3.1 Four engines (`store_bench.py`, each session's record as one JSON row)

| | Today (whole file) | Journal (in-house) | SQLite (WAL) | DuckDB |
|---|---|---|---|---|
| Batch, relaxed (median) | 323–518 ms | 0.01–0.13 ms | 0.04–0.32 ms | 14–90 ms |
| Batch, durable (median / p99) | 527–589 ms | 2.7–2.9 / 6–16 ms | 2.7–3.6 / 7–14 ms | 8–83 / 13–101 ms |
| Bytes per changed session | 0.7–14.7 MB | ~155 B | 1.9–4.5 KB | 0.7–0.9 KB |
| Cold load | 265 ms | 147 ms | 75 ms | 85 ms |
| Memory added | +11 MB | +0 MB | +3 MB | **+933 MB** |
| 30 kills mid-write (the last acknowledged commit kept) | – | 30/30 | 30/30 | 30/30 |
| Another process reads while it writes | yes | yes | yes | **refused** (one process per file) |

- **DuckDB is out.**
  - A commit costs about 4 ms per changed row, and every commit syncs.
  - The first import took 17 s.
  - It added 933 MB of memory.
  - It locks its file to one process, so the CLI, e2e and the skill could not read sessions while the app runs.
- **The in-house journal and SQLite were even on speed.** The journal wrote 12–30× fewer bytes, but its crash safety would be
  ours to prove. The torn-tail fuzz (250/250 after a fix) caught one bug on its first run: a line complete except for its newline
  must not count. SQLite has decades of crash testing behind it.
- **A preallocated journal gained nothing measurable** and doubled the bytes it wrote.

### 3.2 SQLite: the layout and the page size (`sqlite_bench.py`, every prompt kept)

- **The `doc` layout** stores a session's whole record, queue included, as one JSON row.
- **The `split` layout** (§4.2) has three parts:
  - the session's members in one small row;
  - each prompt's mutable state in its own small row;
  - each prompt's text in a row written once.

WAL bytes are written at the commit; checkpoint bytes are the change's share of copying the dirty pages into the
database later.

| Layout, page size | WAL bytes per changed session | + checkpoint share | Durable batch, K=1 / K=20 (median) | Migration (1 transaction) + verify | Database | Cold load |
|---|---|---|---|---|---|---|
| doc, 4096 | 6.5–19.5 KB | 0.2–3.4 KB | 3.4 / 16.3 ms | 0.56 s + 0.58 s | 14.9 MB | 214 ms |
| split, 4096 | 1.9–4.6 KB | 0.2–0.6 KB | 2.9 / 4.0 ms | 0.65 s + 0.29 s | 16.8 MB | 371 ms |
| split, 2048 | 1.5–2.4 KB | 0.2–0.4 KB | 2.9 / 4.9 ms | 0.91 s + 0.34 s | 17.0 MB | 360 ms |
| split, 1024 | 1.1–1.3 KB | 0.2–0.3 KB | 2.9 / 5.0 ms | 1.28 s + 0.41 s | 17.3 MB | 406 ms |
| **split, 512** | **0.9–1.05 KB** | **0.15–0.25 KB** | 3.0 / 7.4 ms | 1.96 s + 0.54 s | 19.0 MB | 554 ms |

- The `doc` layout rewrites a session's whole queue at every change; the split layout fixes that, which is what makes
  D2 free.
- **512-byte pages write the fewest bytes, so they are chosen (D1).**
  - What the measurements show: about 20 % less WAL than 1 KB pages and 4× less than 4 KB.
  - The costs: a 10 % bigger file, and a slower full load and migration, by a fraction of a second in Python.
  - ⚠ What the disk itself writes: Windows and the SSD write 4 KB blocks. A FULL commit's fsync writes at least the WAL's
    last 4 KB block, and a checkpoint writes each dirty 4 KB page of the file once. Smaller pages therefore cut the
    streamed WAL bytes, not the per-fsync floor.
- **Verification.** Reading every session and prompt back after the one-transaction import matched all 2,190 sessions and
  9,466 prompts exactly. The first comparison reported a mismatch, but only because JSON member order differed:
  ⚠ the migration compares PARSED values (sorted keys), never text.

## 4. The design

### 4.1 Files
- **`<profile>\sessions.db`.** Its `-wal` and `-shm` files live beside it and belong to it: never delete a `-wal`, because it
  holds committed transactions until a checkpoint.
- **`sessions.json` is gone after the migration** (§4.9). Its verified copy stays in `backups\` for good.
- **Unchanged:**
  - the window records (`windows\<id>.json`);
  - the session store (`session-store\<sid>.json`);
  - the Telegram bot's `state.json` (870 KB, about 7 rewrites a minute, the next candidate for the same treatment, §7);
  - every other state file.

### 4.2 The schema (page size 512, WAL)

```sql
PRAGMA page_size = 512;          -- set before the first table; fixed for the file's life (D1, §3.2)
PRAGMA journal_mode = WAL;       -- persistent in the file header
PRAGMA wal_autocheckpoint = 0;   -- checkpoints run on the store thread, never inside a caller's commit (§4.4)

CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
  -- 'format' (the format guard, §4.7), 'writer' (the build that opened it for writing last),
  -- 'migration' (what was imported, its SHA-256, the counts and when: §4.9)

CREATE TABLE sessions(id TEXT NOT NULL UNIQUE, doc TEXT NOT NULL);
  -- every member of the session's JSON record EXCEPT "queue" (median ~420 B: one 512-byte page per row)

CREATE TABLE prompt_state(session_id TEXT NOT NULL, prompt_id TEXT NOT NULL, ord INTEGER NOT NULL, state TEXT NOT NULL);
CREATE UNIQUE INDEX prompt_state_key ON prompt_state(session_id, prompt_id);
CREATE INDEX prompt_state_order ON prompt_state(session_id, ord);
  -- every member of a queued prompt EXCEPT "text": status, times, counters, label, gate, origin, ...

CREATE TABLE prompt_text(session_id TEXT NOT NULL, prompt_id TEXT NOT NULL, text TEXT NOT NULL);
CREATE UNIQUE INDEX prompt_text_key ON prompt_text(session_id, prompt_id);
  -- written ONCE when the prompt is queued; a 1.95 MB prompt never costs anything again
```

- **Rowid tables, not WITHOUT ROWID.**
  - A 512-byte rowid leaf keeps up to ~477 bytes of a row in the page.
  - A WITHOUT ROWID table keeps only ~100 bytes there and spills the rest to overflow pages.
  - An UPDATE that keeps a row's id therefore touches one leaf page, and no index.
- **JSON docs per row, not a column per member.**
  - The record format is the one §17's compatibility machinery already understands (§4.7).
  - A newer build's members ride inside the doc.
  - The page is the unit of writing, so columns would not save bytes.
- **The queue's order is `ord`.**
  - A move (the Prompt Queue, Telegram's Move Up) rewrites the moved prompts' `ord`.
  - A removal deletes the prompt's two rows. That covers `TrimQueueHistory`'s cap of 200, a delete, and a merge.
  - The unique `(session_id, prompt_id)` makes a duplicate id a constraint error. The store logs `[store]` and
    keeps the first.

### 4.3 The write path: only what changed

- **Dirty marks in the registry.**
  - Every method of `SessionRegistry` that changes a record marks its id dirty under `_mtx`, in the same lock as the change.
  - That includes the QUIET setters that raise no notify: `SetPendingInput`, the paste references, the enrichment fields.
  - Phase 2 audits every mutator, and an m5 check fails if a mutator forgets its mark.
  - A re-key (the `/clear` and `/resume` re-home, a fork's bind) marks both ids, and they are written in ONE transaction.
- **The store diffs what it last wrote.**
  - For each session it keeps the last written `sessions.doc` text.
  - For each prompt it keeps `ord` + the `state` text + a 64-bit hash of the text. That is about 1 MB + 1.5 MB, replacing
    `s_last`'s 26 MB.
  - Writing a dirty session serializes its doc and its prompts' state docs. It then UPDATEs only the rows whose text changed,
    INSERTs the new prompts (state + text) and DELETEs the gone ones.
  - It reads that one record with `Get(id)` under the registry's lock: never `Snapshot()` of the fleet.
- **One store thread** owns the connection. Callers on other threads reach it through the store's mutex, a leaf lock
  like the outbox's (MESSAGING_OUTBOX.md §5.1): code holding it calls nothing else.
- **What goes away:**
  - `SessionAutosaver`;
  - `SaveSessions(Snapshot())` at all twelve call sites, three in Engine.cpp and nine in TerminalPage;
  - `s_last`;
  - the fleet JSON document;
  - the sessions.json shadow (§4.7).

### 4.4 Durability (D3)

| Change | How it is committed | Survives a kill | Survives a power cut |
|---|---|---|---|
| **Critical on a worker thread** (the scheduler's Sent mark before it types; an engine-thread archive) | at once, in the caller, `synchronous=FULL` (WAL fsynced; ~3 ms measured) | at return | at return |
| **Critical on the UI thread** (close / archive, the window teardown, bind / re-key, held tab refs) | at once, in the caller, `synchronous=NORMAL` (~0.1–0.5 ms; frames in the OS cache); the store thread then fsyncs the WAL at once (its next FULL commit, or `FlushFileBuffers` on its own handle to the `-wal` when nothing else is pending) | at return | within a few ms |
| **Routine** (states, timestamps, drafts, counters, everything else) | the store thread commits the dirty set every 250 ms in ONE transaction with `synchronous=FULL` | ≤ 250 ms | ≤ 250 ms |

- **Rule #4 closed.** Every seam that marks a prompt Sent before typing it commits that prompt's state row as critical
  first, and types only after the commit returned:
  - the scheduler's advance (`Scheduler.cpp`);
  - Telegram's sends (`Messaging.Queue.cpp`, `Messaging.Inbound.cpp`);
  - the typed-prompt capture (`SessionRegistry.cpp`).

  A failed commit types nothing: the prompt rolls back to Pending, as a failed injection does today.
- **Checkpoints run on the store thread, never inside a caller's commit.**
  - A PASSIVE checkpoint when the WAL passes 4 MB, at most every 5 minutes otherwise.
  - A TRUNCATE checkpoint when the last window closes.
  - Measured: 10–100 ms each, on the store thread only.
  - A checkpoint copies each dirty page once, however many times it changed.

### 4.5 Reading at start
- The engine opens the store before the fleet loads, in the place of `LoadSessionsForStartup`.
- One read transaction takes `sessions`, `prompt_state` ordered by `ord`, and `prompt_text`, and builds `SessionInfo` with
  its queue exactly as today. The in-memory model does not change (D2).
- A `PRAGMA quick_check` runs first. It reads the 19 MB once, a few tens of ms in C++. Anything but `ok` is §4.6's unreadable case.

### 4.6 Sudden shutdown, damage and full disks
- **Kill.** A committed transaction is in the OS cache, so a kill loses nothing committed. The 30/30 test pins this.
- **Power cut.** WAL frames carry checksums and salts. At the next open SQLite replays every complete committed transaction
  and drops a torn tail. Every FULL commit had its frames on the disk before it returned.
- **A store that cannot be used never gets overwritten (§15's rule).** On SQLITE_CORRUPT, NOTADB, a failed `quick_check`
  or a lock that never clears:
  - the store refuses to write;
  - `PreserveWorkspaceForRecovery` copies the db, its `-wal` and `-shm` and the window records into `recovery\<stamp>\`
    with its README;
  - the run continues with the fleet it could read, or with none, and the windows hold their refs (§16, held tab refs);
  - the log says `[store] … NOT used: <why>; a copy is in <folder>`.

  Never start a fresh empty store over an unreadable one.
- **Full disk or a failed write.** The transaction rolls back, the ids stay dirty, and the next pass retries.
  `[store-fail]` is logged once per quiet window. Nothing is dropped.
- **Two processes.** Only one may write. The store holds `sessions.db.lock` (no sharing, `FILE_FLAG_DELETE_ON_CLOSE`, the
  outbox's `.lock` pattern). A second instance on the same profile opens read-only and says so; §16 says that setup is not
  supported anyway.

### 4.7 Across versions (§17 in the database)
- **Format guard.** `meta.format` is 1. A build that finds a greater number opens the store READ-ONLY: it loads, never
  writes, and logs `[store-guard]` once. A newer build's data is never overwritten.
- **Residue per row.**
  - At load, the loader records each row's members it did not consume. These are §17's lookups: `json::detail::FindObserver`.
  - Every write puts them back into the fresh doc, under §17's rules: an unknown member is kept whole, an unrepresentable
    value is kept until this build changes it, and a consumed key is dropped.
  - Records are rows, so a row this build removes stays removed.
- **Writer stamp.** `meta.writer` holds the version and install kind (`StateBackups.h`'s identity), written when a
  build first opens the store for writing in a run.
- **The version-change snapshot copies the store CONSISTENTLY.** It uses SQLite's online backup API into
  `backups\<stamp> …\sessions.db` (WAL folded in), never a raw copy of a live file. The snapshot runs before the engine
  opens the store, so no writer is active then.
- **Shadows and the older-writer repair** do not apply to the store, because no older build can write it.

### 4.8 Older builds (D4)
- **An older build finds no `sessions.json`.**
  - Its window records still name the sessions, and its claims hold them as refs (§16), so nothing is lost.
  - It may write a NEW `sessions.json` for sessions it creates.
- **When this build starts and finds a `sessions.json` beside `sessions.db`** (an older build ran in between):
  1. It imports only the sessions whose id the store does not know, in one transaction, each record whole. It never
     overwrites a stored session.
  2. It moves the file aside as `sessions.json.after-sqlite-<stamp>`, kept, never deleted.
  3. It logs `[store-migrate] an older Agentmaster wrote sessions.json after the move: N new session(s) imported, M known
     ones left as the store has them; the file is kept as …`.

### 4.9 The migration: one transaction, sessions.json deleted last, no way to lose data

It runs once, at the first start of a SQLite build, when the engine loads the fleet. That is after the version-change
snapshot (§17.5) has copied every state file. At every step exactly one complete, verified copy is authoritative.

1. **Decide.**
   - `sessions.db` exists with `meta.migration` done → no migration.
   - `sessions.json` missing → create an empty store (a fresh profile).
   - `sessions.json` unreadable (`ReadStateFileUtf8` says Unreadable) → NO migration and no store. The start takes
     today's unreadable path (`PreserveWorkspaceForRecovery`), runs with the fleet it could load, and retries the migration
     at the next start.
2. **Back up, verified.**
   - Copy `sessions.json` byte for byte into `backups\<stamp> sessions.json to SQLite\sessions.json` with a README, and
     flush it.
   - Check that the copy's SHA-256 equals the original's.
   - The README's first line is "Agentmaster sessions.json migration backup", which the 12-snapshot pruner never
     matches, so the copy is kept for good.
3. **Parse.** Use this build's loader for the model. The row docs come from the file's ORIGINAL JSON objects (each session
   without `queue`, each prompt without `text`), so even members this build does not know move over unchanged.
4. **Build in a temp file.**
   - `sessions.db.migrating`: page 512, then WAL.
   - The whole fleet goes in ONE transaction at `synchronous=FULL`, together with `meta.migration` = {source SHA-256,
     2,190 sessions, 9,466 prompts, the time, the writer}.
   - Measured: 2 s in Python.
5. **Verify** with a separate read-only connection:
   - `PRAGMA integrity_check` = ok;
   - the session and prompt counts equal the source's;
   - every session's and every prompt's parsed value equals the source's (sorted keys);
   - a SHA-256 over the canonical fleet equals the one computed from the source.

   Measured: 0.5 s and 0 mismatches. Any difference → delete the temp files, keep `sessions.json`, log
   `[store-migrate] FAILED …` with the first difference, and run on `sessions.json` (today's code path) until the next start.
6. **Switch.** A TRUNCATE checkpoint and close leave no `-wal`; flush; then
   `MoveFileExW(sessions.db.migrating → sessions.db, REPLACE_EXISTING | WRITE_THROUGH)`.
7. **Delete `sessions.json` last,** and its shadow `shadow\sessions.json`, then log
   `[store-migrate] sessions.json → sessions.db: N sessions, M prompts, verified; sessions.json removed (backup: …)`.

**A crash or kill at any step:**

| Stopped during | At the next start |
|---|---|
| steps 1–5 | there is no `sessions.db`; `*.migrating` leftovers are deleted and the migration starts over from the intact `sessions.json` |
| step 6 | the same as above: the rename is atomic, so the temp file is either still there or now `sessions.db` |
| between steps 6 and 7 | `sessions.db` (migration done) and `sessions.json` both exist. The hashes match → finish step 7. They differ → §4.8's rule (an older build wrote it) |

### 4.10 Readers outside the app
- **The CLI** (`agentcli.cpp`, which compiles `Persistence.cpp`): the store's read API on a read-only connection. WAL lets it
  read a consistent snapshot while the app writes; measured "read while open: ok".
- **The session database service** (`SessionDbStore.cpp`'s `RefreshFacts`, which reads the `/clear` and `/handover` links from
  `sessions.json` today): the same read API.
- **The e2e driver** (15 files in `e2e/amdriver/` and `e2e/suites/`) and **the am-agentmaster-cli skill** (`am_inspect.py`):
  - one helper, `e2e/amdriver/sessions_store.py`, using Python's built-in `sqlite3` with `mode=ro`;
  - it returns the same dicts as `json.load(sessions.json)["sessions"]` did, so no assertion changes;
  - the skill gets a copy.
- **Docs that name `sessions.json`:** CLAUDE.md, PERSISTENCE.md, OBSERVER.md, DELIVERY.md, CLI.md, E2E_TESTING.md and the
  others found by a grep in Phase 4.

### 4.11 Building it in
- **vcpkg.** `sqlite3` joins the `terminal` feature in `vcpkg.json`, as a static library:
  - TerminalApp.dll and agentmaster-cli.exe link it;
  - the m5 harness links `obj\x64\vcpkg\x64-windows-static\lib\sqlite3.lib`, as it does cmark (`run-m5-tests.bat`);
  - CI and the release workflows restore it through the vcpkg cache.
- **License.** SQLite is public domain; it is listed in `NOTICE.md` / `THIRD-PARTY-NOTICES.txt` for completeness.
- **Configuration.**
  - The defaults, `SQLITE_THREADSAFE=1` (serialized), with the store's own mutex on top.
  - `PRAGMA foreign_keys` off (no cross-table constraint: the store keeps the rows consistent).
  - `PRAGMA trusted_schema=OFF`.

### 4.12 Memory
- The registry keeps every record and queue in memory, as today (D2).
- Gone: the fleet copy per save, the JSON tree, the 13-million-character strings, and `s_last`'s ~26 MB.
- Added: the store's last-written docs and hashes (~3 MB) and SQLite's page cache (`cache_size` 2 MB).

## 5. Tests

- **m5, pure parts:**
  - the row mapping both ways (`SessionInfo` ↔ session doc + state docs + texts) on every golden record;
  - the diff (what a change writes: exactly the changed rows);
  - the residue per row (§17's cases, re-run on rows);
  - the format guard;
  - the migration's verification (a planted mismatch must stop it);
  - the critical / routine routing of every mutator;
  - the dirty-mark audit (every mutator of `SessionRegistry` marks its id).
- **m5, against a real database in a temp folder:**
  - open, create, write, read back;
  - a duplicate prompt id;
  - a full disk, simulated with a size-capped virtual disk (VHD) or by failing writes through a test seam;
  - an unreadable db: the copy is kept and nothing is overwritten.
- **Kills.** A child process commits critical changes and prints acknowledgements; the parent kills it at random moments
  (`store_bench.py`'s test, ported to the real store through a test executable). 100 rounds, and every acknowledged change
  must be there.
- **The migration under kills.**
  - Kill the migrating process at each step 1–7, then start again. The fleet must equal the source every time, and
    `sessions.json` may be gone only once the store holds it verified.
  - Also: an older build's `sessions.json` beside the store (§4.8).
- **Negative controls.** Undo each guarantee in an isolated copy, and its check must fail:
  - the mark in one mutator;
  - the FULL on a critical commit;
  - the verify step;
  - the delete-last order;
  - the format guard;
  - the residue.
- **e2e.**
  - The whole suite through the new helper.
  - An install-lane test: a profile holding a real `sessions.json` → first start → `sessions.db` present, `sessions.json`
    gone, the backup present with the same SHA-256, every session listed.
  - A rollback check: an older build after the move, then the new one again.
- **Measure in the app.**
  - `[store] commit … rows=N bytes=B ms=X` once a minute (a summary, never per commit).
  - The disk-write counter of a busy hour, before and after the move.

## 6. Phases

| Phase | Work | Purge |
|---|---|---|
| P1 | The store module (`SessionStoreDb.{h,cpp}`: open, create, the schema, read, the row writes, durability, the store thread, checkpoints, the lock) + vcpkg + its m5 tests | no (new files) |
| P2 | Dirty marks in `SessionRegistry` + the store thread replace `SessionAutosaver` and every `SaveSessions`; the six UI seams; the Rule #4 commits | **yes** (`SessionRegistry.*`) |
| P3 | The migration (§4.9), the older-build import (§4.8), the format guard and residue per row (§4.7), the consistent backup | with P2 |
| P4 | The readers: the CLI, the session database service, the e2e helper, the skill; the docs | with P2 |
| P5 | The kill and migration tests, e2e, the install lane; then ONE purged build of P1–P4 for dev, the live checks (§5 "Measure in the app"), then production | – |

## 7. Risks and what stays open

- **A bug in our mapping or diff** (a row not written, an order lost). Mitigations:
  - the m5 round trip over every golden record;
  - the migration's full verification;
  - the in-app summary line, which makes a silent non-write visible;
  - the version-change snapshot, which keeps the store as it was at each build change.
- **The physical write floor.** A FULL commit writes at least a 4 KB block (§3.2): about 58 MB an hour (4 KB × 4 a second)
  if routine commits run every 250 ms all day. That is still about 80× under today's measured ≥ 4.6 GB an hour; quiet
  periods commit nothing.
- **A long WAL** if a reader keeps a read transaction open (the CLI, a test): checkpoints cannot pass it. Readers hold a
  transaction only while they read, and the store logs a WAL above 64 MB.
- **Not covered yet:**
  - the Telegram bot's `state.json` (870 KB, about 7 rewrites a minute);
  - the window records (54 KB, several a minute);
  - the session store's thousands of small files.

  Each is a candidate for its own table in the same database once this works.

## 8. As built (2026-11-20)

The plan above is unchanged; this section is what the code does, and where it differs.

### 8.1 The files

| File | What it holds |
|---|---|
| `AgentMaster/FleetDb.{h,cpp}` | The database (`FleetDb`: open, the schema, the load, the row writes, the import, the checks, the lock), the startup open with the migration (`OpenFleetForStartupIn`), the other processes' read (`LoadFleetReadOnlyIn`). The plan called it `SessionStoreDb`; `SessionStore` was taken (the per-session key-value store). |
| `AgentMaster/FleetWriter.cpp` | The writer thread and the critical commits (`StartFleetWriter`, `NoteFleetRecordChanged`, `PersistFleetNow`, `PersistFleetNowDurable`, `FlushFleetAtAppClose`, `FleetBulkScope`). Separate because the CLI and the session database service link `FleetDb.cpp` without the registry's writer. |
| `SessionRegistry` | `SetRecordChangeSink` + `_markChanged`: every `_notify` marks its record, and so do the two quiet setters (`SetPendingInput`'s text-only edit, `SetPendingPasteRefs`). `Ids()` gives the insertion order. |
| `Persistence.{h,cpp}` | `LoadSessionsForStartupIn` = `OpenFleetForStartupIn`; the old body is `ReadSessionsJsonForStartupIn`; `LoadSessions` = `LoadFleetReadOnlyIn`; `SaveSessionsIn` writes a given profile's `sessions.json` (the Json mode). |
| `Engine.cpp` | `SessionAutosaver` is gone; `StartFleetWriter(registry)` at engine init; a `!live` notify calls `PersistFleetNow`; the last window's close calls `FlushFleetAtAppClose`. |

### 8.2 The schema as built

```sql
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);          -- format, created, writer, migration
CREATE TABLE sessions(id TEXT NOT NULL UNIQUE, ord INTEGER NOT NULL, has_queue INTEGER NOT NULL, doc TEXT NOT NULL);
CREATE TABLE prompt_state(session_id TEXT NOT NULL, prompt_key TEXT NOT NULL, ord INTEGER NOT NULL, state TEXT NOT NULL);
CREATE UNIQUE INDEX prompt_state_key ON prompt_state(session_id, prompt_key);
CREATE TABLE prompt_text(session_id TEXT NOT NULL, prompt_key TEXT NOT NULL, text TEXT);
CREATE UNIQUE INDEX prompt_text_key ON prompt_text(session_id, prompt_key);
```

- **`sessions.ord`** keeps the registry's insertion order (the next start restores in it); a new session takes the next
  number, nothing renumbers. **`has_queue`** says whether the record had a `"queue"` array (a record without one comes back
  without one).
- **`prompt_key`** is the prompt's id, or — for an empty id or one repeated in the same queue — `<id>` U+001F `#<position>`,
  so every prompt keeps its rows. A session row with an empty or repeated session id gets the same kind of key at import.
- **`prompt_text.text` may be NULL:** a prompt with no `"text"` member comes back with none.
- **No `prompt_state_order` index:** the load sorts each queue in memory, and an index would cost another page for every
  moved prompt.
- **The prompts' `ord` is spaced by 65,536** (`kFleetOrdGap`, `FleetPlanOrds`). The longest run of stored numbers that still
  rises in the new order keeps its numbers; a new or moved prompt takes a number between its neighbours'. So the history
  cap's trim (the oldest of 200 removed at every new prompt) rewrites no other prompt's row — with plain positions it would
  have rewritten all 199. Only two neighbours left without room renumber the queue.
- **Text is bound and read as UTF-8 that Windows converts**, never through SQLite's UTF-16 calls: SQLite's own UTF-16 →
  UTF-8 translation joins a lone high surrogate (a title cut inside an emoji) with whatever unit follows — in a doc often the
  closing quote, so the row would no longer parse. Windows writes U+FFFD for it, which is what `sessions.json` always stored.
  A harness case pins it.

### 8.3 Where the code differs from the plan

- **A store that cannot be used (§4.6) is still never written, but the run is not lost:** the run saves `sessions.json`
  beside it (`FleetMode::Json`, the legacy writer with its §15 guards) and loads the `sessions.json` such a run left, and
  the next start that can use the store imports what that file holds and the store does not (§4.8's rule; the file then
  goes aside as `sessions.json.after-sqlite-<stamp>`). The plan had the run save nothing, which would have lost every
  session made in it.
- **A row the load cannot read stays in the database**, as planned; a NEW record with its id moves it (and any prompt rows
  whose session row is gone) under `<id>` U+001F `kept-<ms>-<n>` in the same transaction, so old rows never mix into the new
  record. `[fleet] a new record found rows under its id that this run had not loaded; they are kept as "…"`.
- **A fresh profile's empty store is built aside** (`sessions.db.migrating`, checkpointed, closed, renamed), like the
  migration: a crash while it is created leaves only a temp file the next start deletes, never a `sessions.db` without its
  schema (which every later start would read as unusable).
- **Rule #4 has six seams, not three:** the scheduler's advance (`PersistFleetNowDurable`, FULL on its worker thread), the
  Prompt Queue's Send now and the `/handover` paste pump (`PersistFleetNow` on the UI thread: NORMAL, then the writer's
  fsync), Telegram's `/sendnow`, `/steer` and a ticket's ⚡ Steer (the service's thread, FULL). A commit that fails types
  nothing: the prompt goes back to Pending (`[send-deferred] … the Sent mark could not be saved`). The typed-prompt capture
  (`SessionRegistry.cpp`) needs none: it records a prompt the user already sent, and nothing is typed after it.
- **The UI commit's fsync is tracked:** every NORMAL commit that wrote rows takes a sequence number, and a FULL commit or an
  fsync of the WAL that started after it marks it on the disk. `PersistFleetNowDurable` fsyncs the WAL itself when it finds
  its record already committed by a UI thread an instant earlier.
- **Before the store is open** (`FleetMode::None`: only before the first window's restore, when no session is live, and in
  the harness) the critical commit answers true and the marks wait for the open. Refusing would stall every send of a run
  whose open never happened.
- **The restore runs inside one `FleetBulkScope`:** its Upserts mark their records and nothing commits until the loop ends,
  then the writer commits the restored fleet as one batch. On production 0.8.0.17 each restored session had made a
  synchronous whole-file save (the autosaver's `!live` save): 2,186 rewrites of a growing file, ~14 GB and 277 s per start.
- **The version-change snapshot copies `sessions.db` and `sessions.db-wal` as files** (`StateBackups.h` `kStateFiles`), not
  through SQLite's backup API: the snapshot runs before this start opens the store, so no writer is active, and
  `StateBackups.h` is shared with the EXE, which does not link SQLite. `PreserveWorkspaceForRecovery` copies the pair too.
- **Logs:** `[fleet]` (the open, its mode, a minute's totals: commits, records, rows written and deleted, failures, the WAL's
  size; a WAL over 64 MB once), `[fleet-migrate]` (the migration, an interrupted last step finished, a file written after the
  move imported), `[fleet-fail]` (a failed commit, at most once per 30 s; its records stay marked and are retried after 2 s).
- **The e2e evidence copies the store through SQLite's backup** (`evidence.py`), the one place a live store is copied.
- **The startup load was made cheaper than planned** (the measurements are §8.6):
  - **Memory-mapped reads** (`PRAGMA mmap_size`, every connection of the store): SQLite reads the 512-byte pages from the
    mapping instead of a `ReadFile` each. The read went from ~300 to ~55-90 ms, and the quick check from ~260 to ~30-50 ms.
    The trade-off: a disk that cannot page the file in raises an in-page error, not `SQLITE_IOERR`.
  - **The quick check runs beside the load**, on a read-only connection of its own on another thread
    (`FleetStartQuickCheck`): a start pays the longer of the two, not their sum. A store that fails it is not used (the
    load is thrown away). The import of a `sessions.json` found beside the store writes, so it waits for the check.
  - **The residue's fast path:** a stored record that equals this build's round trip of it (compared as values,
    `FleetSameValue`) has no residue, so the lookups are not recorded and nothing is extracted. On production's migrated
    fleet every session took the fast path (its `sessions.json` came from this build's own serializer).
  - **Each prompt's text is read once:** one join (`prompt_state LEFT JOIN prompt_text`) instead of a map of every text,
    moved (not copied) into the record, and fingerprinted word-wise for the cache.

### 8.4 The readers

- **The CLI and the session database service** link `FleetDb.cpp`: `LoadSessions` reads the store read-only (WAL: a
  consistent snapshot while the app writes); a store that cannot be read falls back to `sessions.json`.
- **The e2e driver:** `e2e/amdriver/sessions_store.py` (`load_fleet`, `read_store`) gives the dicts `sessions.json` gave;
  `Profile.sessions()` reads through it. ROB-K1 (a reader holding `sessions.json` failed every save) is fixed by the store,
  and its known-defect test became `test_a_reader_holding_the_fleet_store_never_loses_a_save`.
- **The am-agentmaster-cli skill:** `am_inspect.py`'s `read_fleet_store`, the same join.

### 8.5 The tests (`tests/tests_fleetdb.cpp`, in the m5 harness)

- **`TestFleetDbRows`:** the row round trip (newlines, quotes, Hebrew, an emoji, an embedded NUL, lone surrogates, a
  1.5-million-character prompt, duplicate and empty prompt ids); the diff with exact row counts (a status = 1 row, a title =
  1, the oldest prompt trimmed = 2 deletes and nothing rewritten, an appended prompt = 2, a Move Up = 1, a capped queue's
  send = 4); the order planner (pure, 400 fuzzed edits against a brute-force longest rising run); removals; the residue per
  row (a newer build's session and prompt members kept through this build's write); the writer lock; the format guard; an
  unreadable row and orphan prompt rows kept aside; a reader beside the writer (the WAL) and after it.
- **`TestFleetStoreStartup`:** a fresh profile; the migration (the loaded fleet equals what the JSON reader makes of the
  file, the backup byte for byte, the record's SHA-256 and counts, the pruner never removing the backup, the CLI's read, a
  second start); a planted mismatch in a session doc and in a prompt text, each stopping the migration (the verification's
  negative controls); the interrupted last step; a `sessions.json` written after the move; an unusable `sessions.json`; an
  unusable store (never written, the run's session saved beside it, imported at the next good start); the format guard.
- **`TestFleetWriter`:** the marks of every registry mutator that matters (Upsert, Update, both `SetPendingInput` paths,
  `SetPendingPasteRefs`, a hook, a rollback, Remove); the commits; the UI and worker critical commits and their answers; the
  bulk scope; a batch in insertion order; the app's close.
- **`TestFleetKills`:** REAL kills of the harness exe run as a child (`--fleet-child`): 30 children killed mid-commit (every
  acknowledged commit survives, at most the one in flight beyond it, and every commit is whole); a migration killed at each
  of its seven step boundaries and at ten random moments (`sessions.json` is gone only once `sessions.db` holds the whole
  fleet, and the next start always ends with the whole fleet in the store).
- **Negative controls** (run 2026-11-20 on a scratch copy of the engine, never the repository; each undoes one guarantee and
  rebuilds a runner of these tests): every one was caught by its check — the quiet setter's mark, the verification, the
  delete-last order (`sessions.json` deleted before the store held it: the step kills failed), the format guard, the
  residue, the UTF-8 binding (SQLite's UTF-16 binding broke the lone-surrogate row), the bulk scope, the spaced order
  numbers, and a fresh store written over an unusable one. **Not observable without a power cut:** whether a commit
  fsynced (FULL vs NORMAL, and the UI commit's fsync before a durable answer) — a kill never loses what reached the OS cache,
  so those guarantees rest on SQLite's documented semantics and a code read, not on a test.
- **A real fleet:** a copy of production's `sessions.json` (13.6 MB, 2,186 sessions, 9,450 prompts) migrated by the harness
  exe — verified, `sessions.json` removed, its backup byte for byte — and read back by the e2e driver's and the skill's
  readers: both equal the source, session by session.

### 8.6 Measurements as built

An optimized (`/O2`) build of the store on a copy of production's fleet (2,186 sessions, 9,450 prompts, 13.6 MB of JSON),
2026-11-20, on this PC's C: with its usual background load:

| | `sessions.json` (before) | `sessions.db` |
|---|---|---|
| The startup read / load | 153-242 ms | 172-293 ms (steady ~275 ms: the read ~55-90, the records ~150, the quick check ~30 beside them) |
| The restore that follows (2,186 Upserts) | 277 s on production (a synchronous whole-file save per restored session) | one batch commit at the loop's end, on the writer thread |
| The migration | – | ~2.1 s, once (backup, import, verification, rename, first load) |
| A routine durable commit (one session, 78 prompts: a status + a stamp) | 323-589 ms (the whole file) | ~2.1-2.9 ms: 2 rows, ~1 KB of WAL |
| An unchanged record (the rolling sweep's cost) | – | ~0.9 ms of serialization, no SQL |
| The file | 13.6 MB | 19.3 MB |

- The load itself stays ~0.1 s above the old read (the plan's target was "≤ today's"); the start as a whole is minutes
  faster, because the restore no longer rewrites the fleet once per session.
