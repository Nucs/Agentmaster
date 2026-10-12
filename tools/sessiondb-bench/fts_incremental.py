# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The cost of keeping an incremental fts index up to date (SESSIONS_DB.md section 6.5).

DuckDB 2.0's fts maintains an `incremental` index with STATEMENT-level triggers (AFTER INSERT /
DELETE, with transition tables). This script inserts 1, 10, 89 and 900 held-back blocks and deletes
100, printing each statement's time and the index's row counts, then checks that a lookup stays fast
over the trigger-appended (unclustered) postings. The finding: a fixed cost of ~0.6-0.9 s per
statement, whatever its size, so a production indexer must batch.

Run it on an index built with `fts_build.py <layout db> <mod> --skip-incremental` (the held-back
rows are still in `later` then).

Usage: python fts_incremental.py <fts db>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def counts(con):
    """The index's size as (occurrence rows, documents, dictionary terms).

    Args:
        con: the open fts database.

    Returns:
        A tuple of three ints.
    """
    return con.execute("""select (select count(*) from fts_main_d.terms), (select count(*) from fts_main_d.docs),
                                 (select count(*) from fts_main_d.dict)""").fetchone()


def lookup(con, term: str):
    """Count the blocks holding `term` through the index tables, and time it.

    Args:
        con: the open fts database.
        term: an already folded dictionary term (no quoting: benchmark input only).

    Returns:
        (blocks, milliseconds).
    """
    start = time.perf_counter()
    n = con.execute(f"""select count(distinct docid) from fts_main_d.terms
        where termid in (select termid from fts_main_d.dict where term = '{term}')""").fetchone()[0]
    return n, (time.perf_counter() - start) * 1000


def main() -> None:
    """Print the triggers, then time the inserts, the lookup and the delete.

    Raises:
        SystemExit: the database path was not given.
        duckdb.Error: the database lacks the index or the `later` table.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    con = bc.connect(sys.argv[1])
    con.execute("LOAD fts")
    print("triggers:", [row[4] for row in con.execute("select * from duckdb_triggers()").fetchall()])
    print("before:", counts(con), "held-back rows:", con.execute("select count(*), sum(strlen(t)) from later").fetchone())
    for label, sql in (("insert 1 block", "INSERT INTO d SELECT * FROM later ORDER BY doc_id LIMIT 1"),
                       ("insert 10 blocks", "INSERT INTO d SELECT * FROM later ORDER BY doc_id LIMIT 10 OFFSET 1"),
                       ("insert 89 blocks", "INSERT INTO d SELECT * FROM later ORDER BY doc_id LIMIT 89 OFFSET 11"),
                       ("insert 900 blocks", "INSERT INTO d SELECT * FROM later ORDER BY doc_id OFFSET 100")):
        start = time.time()
        con.execute(sql)
        print(f"{label:17s} {(time.time() - start) * 1000:8.0f} ms   rows / docs / terms now {counts(con)}", flush=True)
    print("lookup 'duckdb' after the inserts (blocks, ms):", lookup(con, "duckdb"))
    start = time.time()
    con.execute("DELETE FROM d WHERE doc_id IN (SELECT doc_id FROM later ORDER BY doc_id LIMIT 100)")
    print(f"delete 100 blocks {(time.time() - start) * 1000:8.0f} ms   rows / docs / terms now {counts(con)}", flush=True)
    start = time.time()
    con.execute("CHECKPOINT")
    print(f"checkpoint {(time.time() - start) * 1000:.0f} ms;", con.execute("pragma database_size").fetchall())


if __name__ == "__main__":
    main()
