# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Which indexes to build: clustering (zonemaps) against ART, measured (SESSIONS_DB.md section 6.8).

Runs on the database written by raw_lines.py (every transcript line stored verbatim). On a 10% sample
of its lines it builds two physical orders of the same rows and times the reads the design needs:

  CLUSTERED    sorted by line_id (= file_id << 32 | line_no): each session's lines are contiguous,
               which is what a sorted merge or a re-cluster produces;
  INTERLEAVED  shuffled: what live sessions appending at the same time produce without merges.

Reads: a whole big session, a whole small session, ONE line by id, 30 random lines by an IN list,
and the same 30 through a join with a temporary table. Then it adds ART indexes and repeats them,
printing each index's build time, its memory (duckdb_memory) and whether the plan uses it.

DuckDB's rule, from its documentation: an ART index serves only single-column equality / IN
filters, and only when they match fewer than MAX(2048, 0.1% of the table's) rows; index buffers are
not evicted under memory pressure.

Usage: python access_paths.py <db written by raw_lines.py>
"""

import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def art_memory(con) -> int:
    """Bytes DuckDB holds for ART indexes right now (duckdb_memory's ART tag), 0 if none.

    Args:
        con: the open database.

    Returns:
        The byte count.
    """
    rows = con.execute("select tag, memory_usage_bytes from duckdb_memory()").fetchall()
    return sum(b for tag, b in rows if "ART" in tag.upper())


def plan_scan_type(con, sql: str) -> str:
    """How the query really read the table: 'index scan', 'sequential scan' or 'unknown'.

    ⚠ Uses EXPLAIN ANALYZE, which runs the query: plain EXPLAIN prints "Sequential Scan" even when the
    ART index will be used, because DuckDB picks the index scan inside the table scan at run time
    (measured 2026-10-12: EXPLAIN said Sequential Scan, EXPLAIN ANALYZE said Index Scan with
    "Row Groups Scanned: 0 / 1"). The scan's "Type" line names the access path.

    Args:
        con: the open database.
        sql: the query to run and profile.

    Returns:
        A short label.
    """
    plan = "\n".join(str(row[-1]) for row in con.execute("EXPLAIN ANALYZE " + sql).fetchall()).lower()
    if "index scan" in plan:
        return "index scan"
    if "sequential scan" in plan:
        return "sequential scan"
    return "unknown"


def reads(con, table: str, label: str, big: int, small: int, one_id: int, ids) -> None:
    """Time the five reads on one table.

    Args:
        con: the open database.
        table: the table to read.
        label: the layout's name, for the output.
        big: the file id of the big session.
        small: the file id of the small session.
        one_id: a line id for the single lookup.
        ids: 30 line ids for the multi lookups (also loaded into the temp table `want`).
    """
    in_list = ",".join(map(str, ids))
    whole = "select strlen(string_agg(raw, chr(10) order by line_no)) from {t} where file_id = {f}"
    bc.timed(con, f"{label}: whole big session", whole.format(t=table, f=big))
    bc.timed(con, f"{label}: whole small session", whole.format(t=table, f=small))
    bc.timed(con, f"{label}: one line by id", f"select strlen(raw) from {table} where line_id = {one_id}")
    bc.timed(con, f"{label}: 30 lines by IN list", f"select strlen(raw) from {table} where line_id in ({in_list})")
    bc.timed(con, f"{label}: 30 lines by join", f"select strlen(raw) from {table} t semi join want w on t.line_id = w.line_id")


def main() -> None:
    """Build the two layouts, time the reads, add ART indexes, time them again.

    Raises:
        SystemExit: the database path was not given.
        duckdb.Error: the database lacks raw_lines.py's `line` table.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    con = bc.connect(sys.argv[1])
    con.execute("SET preserve_insertion_order = true")

    start = time.time()
    con.execute("CREATE OR REPLACE TABLE lc AS SELECT * FROM line WHERE file_id % 10 = 0 ORDER BY line_id")
    con.execute("CHECKPOINT")
    clustered_s = time.time() - start
    rows, raw = con.execute("select count(*), sum(bytes) from lc").fetchone()
    start = time.time()
    con.execute("CREATE OR REPLACE TABLE li AS SELECT * FROM lc ORDER BY hash(line_id)")
    con.execute("CHECKPOINT")
    shuffle_s = time.time() - start
    print(f"== 10% sample: {rows} lines, {raw / 1e9:.2f} GB raw; building it sorted took {clustered_s:.1f} s, "
          f"rewriting it in another order {shuffle_s:.1f} s (the cost of a re-cluster per GB)")

    big, big_lines = con.execute("select file_id, count(*) from lc group by 1 order by 2 desc limit 1").fetchone()
    small, small_lines = con.execute("select file_id, count(*) from lc group by 1 having count(*) between 300 and 1500 order by 2 limit 1").fetchone()
    random.seed(11)
    all_ids = [r[0] for r in con.execute("select line_id from lc using sample 2000 rows").fetchall()]
    ids = random.sample(all_ids, 30)
    one_id = ids[0]
    con.execute("CREATE OR REPLACE TEMP TABLE want (line_id BIGINT)")
    con.executemany("INSERT INTO want VALUES (?)", [(i,) for i in ids])
    print(f"   big session {big_lines} lines, small session {small_lines} lines")

    reads(con, "lc", "clustered", big, small, one_id, ids)
    reads(con, "li", "interleaved", big, small, one_id, ids)

    for table, column in (("li", "file_id"), ("li", "line_id"), ("lc", "line_id")):
        before = art_memory(con)
        start = time.time()
        con.execute(f"CREATE INDEX {table}_{column} ON {table} ({column})")
        built = time.time() - start
        print(f"   ART on {table}({column}): built in {built:.2f} s, ART memory {before / 1e6:.1f} -> {art_memory(con) / 1e6:.1f} MB")

    reads(con, "li", "interleaved + ART", big, small, one_id, ids)
    reads(con, "lc", "clustered + ART", big, small, one_id, ids)
    in_list = ",".join(map(str, ids))
    for label, sql in (("small session (interleaved)", f"select raw from li where file_id = {small}"),
                       ("big session (interleaved)", f"select raw from li where file_id = {big}"),
                       ("one line (clustered)", f"select raw from lc where line_id = {one_id}"),
                       ("30 lines IN (clustered)", f"select raw from lc where line_id in ({in_list})"),
                       ("30 lines join (clustered)", "select raw from lc t semi join want w on t.line_id = w.line_id")):
        print(f"   plan, {label}: {plan_scan_type(con, sql)}")
    con.execute("DROP TABLE lc")
    con.execute("DROP TABLE li")


if __name__ == "__main__":
    main()
