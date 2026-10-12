# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Shared helpers of the session-database benchmarks (doc/agentmaster/SESSIONS_DB.md, section 14).

Every script in this folder imports this module for three things: WHERE scratch files go (never the
repository: a staging database of the full corpus is several GB), HOW a DuckDB connection is
configured (small thread and memory budgets: the machine these numbers come from runs 100+ live
claude sessions, and an unbounded DuckDB takes 80% of RAM by default), and HOW a query is timed (best
of N, so a cold first run does not dominate the table).

The scripts only READ ~/.claude (the transcripts); they write nothing outside the work directory.
"""

import os
import sys
import tempfile
import time

import duckdb

# Where every scratch file goes: databases, DuckDB spill files, downloaded extensions. Overridable
# because a full staging run needs ~10 GB free and %TEMP% may sit on a full system drive.
WORK_ENV = "AM_SESSIONDB_BENCH_DIR"


def work_dir() -> str:
    """The folder that holds every scratch file of the benchmarks, created on first use.

    Returns:
        The absolute path of %AM_SESSIONDB_BENCH_DIR% when that variable is set, else
        %TEMP%/agentmaster-sessiondb-bench.

    Raises:
        OSError: the folder cannot be created (no permission, disk full).
    """
    path = os.environ.get(WORK_ENV) or os.path.join(tempfile.gettempdir(), "agentmaster-sessiondb-bench")
    os.makedirs(path, exist_ok=True)
    return path


def work_path(name: str) -> str:
    """A file name inside the work directory, for a script's own outputs (databases, logs).

    Args:
        name: the file's leaf name; a relative path with folders is allowed.

    Returns:
        The absolute path. The file itself is not created.

    Raises:
        OSError: the work directory cannot be created.
    """
    return os.path.join(work_dir(), name)


def remove_db(path: str) -> None:
    """Delete a scratch database and its write-ahead log, so a run starts from an empty file.

    A leftover WAL from an aborted run would be replayed into the new file and corrupt the numbers,
    which is why both files go.

    Args:
        path: the database file's path.

    Raises:
        OSError: a file exists but cannot be deleted (another process still has it open).
    """
    for p in (path, path + ".wal"):
        if os.path.exists(p):
            os.remove(p)


def connect(path: str, read_only: bool = False, threads: int = 4, memory: str = "3GB") -> "duckdb.DuckDBPyConnection":
    """Open a DuckDB database with the benchmarks' resource budget.

    The budget matters more than it looks: DuckDB's default memory limit is 80% of RAM and its
    default thread count is every core. On a machine that hosts many claude processes the default
    pushes the commit charge to its limit. Spill files and extensions are redirected into the work
    directory so nothing lands in ~/.duckdb.

    Args:
        path: the database file (":memory:" for an in-memory database).
        read_only: open without the write lock, so several benchmark processes can read one file.
        threads: DuckDB worker threads (the numbers in SESSIONS_DB.md use 4).
        memory: DuckDB's memory limit, as DuckDB parses it ("3GB").

    Returns:
        The open connection.

    Raises:
        duckdb.IOException: the file cannot be opened (missing for read_only, or locked by a
            process that opened it read-write).
    """
    config = {
        "threads": threads,
        "memory_limit": memory,
        "preserve_insertion_order": False,  # lets CREATE TABLE AS stream instead of buffering for order
        "temp_directory": os.path.join(work_dir(), "duck_tmp"),
        "extension_directory": os.path.join(work_dir(), "extensions"),
    }
    return duckdb.connect(path, read_only=read_only, config=config)


def timed(con, label: str, sql: str, params=None, reps: int = 3):
    """Run a query `reps` times, print the best wall time and the result size, return the rows.

    The best run is reported, not the mean: the first run pays for cold pages and plan caches,
    and the design's latency targets are about a warm service.

    Args:
        con: an open DuckDB connection.
        label: what the line says the query is.
        sql: the query.
        params: positional parameters for `?` placeholders, or None.
        reps: how many runs to take the best of.

    Returns:
        The rows of the last run.

    Raises:
        duckdb.Error: the query fails (a missing table, a syntax error, out of memory).
    """
    best, rows = None, None
    for _ in range(reps):
        start = time.perf_counter()
        rows = con.execute(sql, params or []).fetchall()
        elapsed = (time.perf_counter() - start) * 1000
        best = elapsed if best is None else min(best, elapsed)
    shown = rows[0] if len(rows) == 1 else f"{len(rows)} rows"
    print(f"{best:9.1f} ms  {label} -> {shown}", flush=True)
    return rows


def utf8_stdout() -> None:
    """Make print() write UTF-8 even on a cp1252 console.

    Session titles and paths hold characters outside cp1252, and `python -I` ignores
    PYTHONIOENCODING, so without this a benchmark dies on its first such row.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
