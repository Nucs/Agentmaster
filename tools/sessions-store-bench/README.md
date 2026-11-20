# Session store benchmarks

These two scripts produced the measurements behind
[`doc/agentmaster/SESSIONS_STORE.md`](../../doc/agentmaster/SESSIONS_STORE.md) §3: the move of `sessions.json` to SQLite.
Both run on a COPY of a real `sessions.json` and write only under the work folder you give them.

| Script | Measures |
|---|---|
| `store_bench.py` | Four engines on one record per session: today's whole-file rewrite, an in-house journal (plain and preallocated), SQLite in WAL mode, DuckDB. For each: batch commit latency (relaxed and durable), bytes written per changed session, checkpoint, cold load and memory. It also runs 30 kills mid-write per engine, a read by another process while the store is open, and 250 torn tails of the journal. |
| `sqlite_bench.py` | SQLite only. Two layouts (one row per session, or the split session / prompt-state / prompt-text rows) at page sizes 512–4096. For each: the one-transaction migration of the whole fleet and its verification, WAL bytes and checkpoint bytes per change, latency, database size and cold load. |

## Run

1. Copy the profile's `sessions.json` without blocking the app's atomic save. Python's `open()` does not share delete,
   so use PowerShell:
   ```powershell
   $src = [IO.File]::Open("$HOME\.agentmaster\sessions.json", 'Open', 'Read', 'ReadWrite, Delete')
   $dst = [IO.File]::Create("<scratch>\sessions_copy.json"); $src.CopyTo($dst); $dst.Close(); $src.Close()
   ```
2. Run the scripts:
   ```
   python -I store_bench.py all <scratch>\sessions_copy.json <scratch>\work
   python -I sqlite_bench.py <scratch>\sessions_copy.json <scratch>\work-sqlite
   ```
3. Delete `<scratch>` afterwards. ⚠ The copy holds every prompt text of the fleet: never commit it, and never put it in a
   shared folder.

## What to know before reading the numbers

- **Run it on the profile's drive (C:).** Use a scratch folder there; the flush costs belong to that disk.
- **Python's own per-call cost is in every figure.** It weighs most on the journal's microsecond appends. Read the ratios
  between engines and between page sizes; a C++ store is cheaper still.
- **Bytes come from `GetProcessIoCounters`.** That counts what the process wrote: the WAL and the database for SQLite, the
  journal and the checkpoint for the journal. The disk writes 4 KB blocks, so an fsync costs at least one block whatever
  the logical size (SESSIONS_STORE.md §3.2).
- **Python's sqlite3 module needs SQLite 3.24 or newer** (the UPSERT `store_bench.py` uses). These scripts ran on 3.51.0
  and DuckDB 1.5.0, the Python packages in the session's default interpreter.
- **The PC's load moves the absolute times.** On 2026-11-20 it was ~50 % busy, so run twice and compare.
