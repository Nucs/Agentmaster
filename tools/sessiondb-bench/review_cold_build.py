# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Can the session database be built from scratch in under 30 s? (SESSIONS_DB.md section 17: the user's
requirement of 2026-10-16.) Measures DuckDB's side of a cold build on the real corpus; the C++ side (the
existing line extractor on N threads) is bench_extract.cpp.

Stages (the first argument):
  scan      read every transcript line with DuckDB's parallel NDJSON reader, count only: the read floor
  measure   the per-block extraction in DuckDB SQL (from_json + json_extract + unnest), aggregated, no table
  build     the same extraction written to the table `blk` (file, line type, uuid, session, time, kind, tool, text)
  fold      add `fold`, a lowercase copy of `text` (what a search by scan reads)
  search    one word at a time by scan: ILIKE on `text` against contains() on `fold` (best of 3)
  write     `blk` repeated --copies times into a fresh file: DuckDB's write cost at the whole corpus's size

Options: --threads N (16), --memory 4GB, --project <a folder under ~/.claude/projects> (only that project),
--db <file name in the work folder> (cold_build.duckdb), --copies N (9).

Measured 2026-10-16 (section 17.2): scan 6.7 s for 10.74 GB; the SQL extraction ran out of memory at 16 and 8
threads (4 GB), a torn line aborted it at 4 threads, and `try()` around the parse crashed DuckDB 1.5.0; one
project (1.23 GB) built in 4.6 s at 4 threads; `write` of 2.2 GB of text plus its lowercase copy took 13.5 s.

Only ~/.claude is read; every file this writes is in the benchmarks' work folder (bench_common.work_dir).
A commit guard interrupts DuckDB when the machine's free commit drops below 3 GB: this machine runs 100+
claude sessions, and exhausting the commit limit wedges other programs.
"""

import ctypes
import os
import sys
import threading
import time

from bench_common import connect, remove_db, utf8_stdout, work_path

# The block extraction: one parse per line (from_json into a struct), then one row per content block.
# `text` is what a search reads: a prompt / reply / thinking text, a tool call's input JSON, a tool result's
# text. A content that is a plain string is one text block. {src} is the read_ndjson_objects(...) call.
EXTRACT_SQL = """
WITH l AS (
  SELECT filename AS f,
         from_json(json, '{{"type":"VARCHAR","uuid":"VARCHAR","sessionId":"VARCHAR","timestamp":"VARCHAR",
                            "message":{{"role":"VARCHAR","content":"JSON"}}}}') AS j
  FROM {src}
),
b AS (
  SELECT f, j.type AS t, j.uuid AS u, j.sessionId AS s, j.timestamp AS ts,
         CASE WHEN json_type(j.message.content) = 'VARCHAR' THEN [j.message.content]
              ELSE json_extract(j.message.content, '$[*]') END AS bl
  FROM l WHERE j.message.content IS NOT NULL
),
x AS (SELECT f, t, u, s, ts, unnest(bl) AS e FROM b)
SELECT f, t, u, s, ts,
       CASE WHEN json_type(e) = 'VARCHAR' THEN 'text' ELSE json_extract_string(e, '$.type') END AS k,
       json_extract_string(e, '$.name') AS tool,
       CASE WHEN json_type(e) = 'VARCHAR' THEN json_extract_string(e, '$')
            WHEN json_extract_string(e, '$.type') = 'text' THEN json_extract_string(e, '$.text')
            WHEN json_extract_string(e, '$.type') = 'thinking' THEN json_extract_string(e, '$.thinking')
            WHEN json_extract_string(e, '$.type') = 'tool_use' THEN CAST(json_extract(e, '$.input') AS VARCHAR)
            WHEN json_extract_string(e, '$.type') = 'tool_result' THEN
                 CASE WHEN json_type(e, '$.content') = 'VARCHAR' THEN json_extract_string(e, '$.content')
                      ELSE CAST(json_extract(e, '$.content') AS VARCHAR) END
       END AS text
FROM x
"""

WORDS = ["tab drag crash", "duckdb", "profilebootstrap", "ksummaryversion", "the"]


class _MemoryStatus(ctypes.Structure):
    """MEMORYSTATUSEX, for GlobalMemoryStatusEx (the commit guard)."""

    _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]


def free_commit_gb() -> float:
    """The machine's free commit (what can still be committed before the commit limit), in GB.

    Returns:
        ullAvailPageFile of GlobalMemoryStatusEx, divided by 1e9.
    """
    m = _MemoryStatus()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPageFile / 1e9


def start_commit_guard(con, floor_gb: float = 3.0):
    """Interrupt `con`'s running query when the free commit drops below `floor_gb`.

    Args:
        con: the DuckDB connection to interrupt.
        floor_gb: the free commit below which the query is interrupted.

    Returns:
        (stop event, one-element list holding the lowest free commit seen): set the event to end the guard.
    """
    stop = threading.Event()
    low = [free_commit_gb()]

    def run():
        while not stop.is_set():
            f = free_commit_gb()
            low[0] = min(low[0], f)
            if f < floor_gb:
                print(f"!! free commit {f:.1f} GB: interrupting DuckDB", flush=True)
                con.interrupt()
                return
            time.sleep(0.25)

    threading.Thread(target=run, daemon=True).start()
    return stop, low


def option(args, name: str, default):
    """Read one command-line option.

    Args:
        args: the command-line arguments after the stage.
        name: the option's flag, e.g. "--threads".
        default: the value when the flag is absent; its type converts the value given.

    Returns:
        The argument after `name`, converted to `default`'s type, or `default` itself.

    Raises:
        IndexError: the flag is the last argument (no value follows it).
        ValueError: the value does not convert to `default`'s type.
    """
    return type(default)(args[args.index(name) + 1]) if name in args else default


def source(project: str) -> str:
    """The read_ndjson_objects(...) call over every top-level and subagent transcript (or one project's).

    Args:
        project: a folder name under ~/.claude/projects, or "" for every project.

    Returns:
        The SQL table function call, tolerant of torn lines, with the file name as a column.
    """
    root = os.path.expanduser(r"~\.claude\projects").replace("\\", "/")
    p = project or "*"
    files = [f"{root}/{p}/*.jsonl", f"{root}/{p}/*/subagents/*.jsonl"]
    return f"read_ndjson_objects({files!r}, maximum_object_size=33554432, ignore_errors=true, filename=true)"


def best_of(con, sql: str, params, reps: int = 3):
    """Time a query as its best of several runs, so a cold first run does not set the number.

    Args:
        con: the open DuckDB connection.
        sql: the query; its first result row is returned.
        params: the query's parameters (a list, may be empty).
        reps: how many times to run it.

    Returns:
        (the first result row of the last run, the best wall time in seconds).

    Raises:
        duckdb.Error: the query fails (or the commit guard interrupted it).
    """
    best, row = 1e9, None
    for _ in range(reps):
        t0 = time.perf_counter()
        row = con.execute(sql, params).fetchone()
        best = min(best, time.perf_counter() - t0)
    return row, best


def main(argv) -> int:
    """Run one stage of the measurement (the module docstring lists them).

    Args:
        argv: the command-line arguments: the stage, then its options.

    Returns:
        The process exit code: 0 when the stage ran, 2 for an unknown stage (the usage is printed).

    Raises:
        duckdb.Error: a query failed: out of memory, a torn line in `measure` / `build`, or the commit guard's
            interrupt. The guard is stopped and the lowest free commit printed either way.
    """
    utf8_stdout()
    stage = argv[0]
    threads = option(argv, "--threads", 16)
    memory = option(argv, "--memory", "4GB")
    project = option(argv, "--project", "")
    db = work_path(option(argv, "--db", "cold_build.duckdb"))
    if stage == "build":
        remove_db(db)
    con = connect(db, threads=threads, memory=memory)
    stop, low = start_commit_guard(con)
    print(f"stage {stage}, {threads} threads, memory {memory}, {project or 'every project'}, "
          f"free commit {free_commit_gb():.1f} GB", flush=True)
    try:
        src = source(project)
        if stage == "scan":
            t0 = time.perf_counter()
            n, b = con.execute(f"SELECT count(*), sum(strlen(CAST(json AS VARCHAR))) FROM {src}").fetchone()
            dt = time.perf_counter() - t0
            print(f"scan: {n:,} lines, {b / 1e9:.2f} GB of JSON in {dt:.1f} s ({b / 1e9 / dt:.2f} GB/s)")
        elif stage == "measure":
            t0 = time.perf_counter()
            rows = con.execute("SELECT k, count(*), sum(strlen(text)), max(strlen(text)) FROM ("
                               + EXTRACT_SQL.format(src=src) + ") GROUP BY k ORDER BY 3 DESC").fetchall()
            print(f"measure: the extraction alone in {time.perf_counter() - t0:.1f} s")
            for k, n, b, big in rows:
                print(f"   {str(k):12s} {n:>10,} blocks {(b or 0) / 1e9:7.3f} GB, biggest {(big or 0) / 1e6:6.1f} MB")
        elif stage == "build":
            t0 = time.perf_counter()
            con.execute("CREATE TABLE blk AS " + EXTRACT_SQL.format(src=src))
            t1 = time.perf_counter()
            con.execute("CHECKPOINT")
            rows, tb = con.execute("SELECT count(*), sum(strlen(text)) FROM blk").fetchone()
            print(f"build: {rows:,} blocks, {(tb or 0) / 1e9:.2f} GB of text; extract + insert {t1 - t0:.1f} s, "
                  f"checkpoint {time.perf_counter() - t1:.1f} s; file {os.path.getsize(db) / 1e9:.2f} GB")
        elif stage == "fold":
            t0 = time.perf_counter()
            con.execute("ALTER TABLE blk ADD COLUMN IF NOT EXISTS fold VARCHAR")
            con.execute("UPDATE blk SET fold = lower(text)")
            con.execute("CHECKPOINT")
            print(f"fold: {time.perf_counter() - t0:.1f} s")
        elif stage == "search":
            total = con.execute("SELECT sum(strlen(fold)) FROM blk").fetchone()[0] or 1
            for word in WORDS:
                (n1,), t1 = best_of(con, "SELECT count(DISTINCT s) FROM blk WHERE text ILIKE '%' || ? || '%'", [word])
                (n2,), t2 = best_of(con, "SELECT count(DISTINCT s) FROM blk WHERE contains(fold, ?)", [word])
                print(f"{word!r:20s} ILIKE {n1:5d} sessions {t1 * 1000:6.0f} ms ({total / 1e9 / t1:.2f} GB/s) | "
                      f"contains(fold) {n2:5d} sessions {t2 * 1000:6.0f} ms ({total / 1e9 / t2:.2f} GB/s)")
        elif stage == "write":
            copies = option(argv, "--copies", 9)
            dst = work_path("cold_build_write.duckdb")
            remove_db(dst)
            con.execute(f"ATTACH '{dst}' AS w")
            t0 = time.perf_counter()
            con.execute(f"CREATE TABLE w.big AS SELECT s, k, tool, ts, text, fold FROM blk CROSS JOIN range({copies})")
            t1 = time.perf_counter()
            con.execute("CHECKPOINT w")
            rows, chars = con.execute("SELECT count(*), sum(strlen(text)) FROM w.big").fetchone()
            print(f"write: {rows:,} rows, {chars / 1e9:.2f} GB of text plus its lowercase copy: insert {t1 - t0:.1f} s, "
                  f"checkpoint {time.perf_counter() - t1:.1f} s, file {os.path.getsize(dst) / 1e9:.2f} GB")
            (n,), t = best_of(con, "SELECT count(DISTINCT s) FROM w.big WHERE contains(fold, ?)", ["profilebootstrap"])
            print(f"search by scan over all of it: {t * 1000:.0f} ms (best of 3)")
            con.execute("DETACH w")
            remove_db(dst)
        else:
            print(__doc__)
            return 2
    finally:
        stop.set()
        print(f"lowest free commit during the run: {low[0]:.1f} GB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["help"]))
