# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build the production-shaped block table from ingest.py's staging database (SESSIONS_DB.md section 5).

Writes a NEW database with:
  - fork copies removed: a fork repeats its parent's lines with the same uuid, so (uuid, block) keeps
    only its first writer (25.6% of the blocks go on today's corpus);
  - empty blocks removed and the text capped at 16 KB per block;
  - a pre-folded copy (text_fold = lower(text)), so a search never runs lower() per row;
  - rows clustered by (role, timestamp), so a role filter prunes row groups by their zonemaps;
  - the session and path tables copied.

⚠ This layout keeps both text columns with DuckDB's default string compression, which does not
compress large strings: the file is about as big as the text (3.1 GB for 1.35 GB x 2). The design
stores the text with ZSTD instead (compress_test.py, column_compression.py).

Usage: python build_layout.py <staging db> <layout db>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

KEEP = """
CREATE TEMP TABLE keep AS
SELECT min_by(rowid, ts) AS rid FROM src.doc WHERE uuid IS NOT NULL GROUP BY uuid, bi
UNION ALL
SELECT rowid FROM src.doc WHERE uuid IS NULL
"""

DOC = """
CREATE TABLE doc AS
SELECT row_number() OVER (ORDER BY role, ts_ms) AS doc_id, *
FROM (
    SELECT d.sid, d.uuid, d.bi, epoch_ms(d.ts) AS ts_ms, d.role, d.tool, d.meta, d.sidechain, d.sub,
           strlen(d.text) AS text_len, left(d.text, 16384) AS text, lower(left(d.text, 16384)) AS text_fold
    FROM src.doc d SEMI JOIN keep k ON d.rowid = k.rid
    WHERE d.text IS NOT NULL AND d.text <> ''
)
ORDER BY role, ts_ms
"""


def step(con, name: str, sql: str) -> None:
    """Run one build statement and print how long it took.

    Args:
        con: the open layout database.
        name: what the line says the step is.
        sql: the statement.

    Raises:
        duckdb.Error: the statement fails (out of memory, a missing staging table).
    """
    start = time.time()
    con.execute(sql)
    print(f"[{time.time() - start:7.1f} s] {name}", flush=True)


def main() -> None:
    """Build the layout database and print its rows, size and per-column storage.

    Raises:
        SystemExit: the two paths were not given.
        duckdb.Error: a build step failed.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    bc.remove_db(dst)
    con = bc.connect(dst)
    con.execute(f"ATTACH '{src}' AS src (READ_ONLY)")
    step(con, "keep set (dedupe by uuid + block, first writer wins)", KEEP)
    step(con, "doc table (capped, folded, clustered by role, ts)", DOC)
    step(con, "session copy", "CREATE TABLE session AS SELECT * FROM src.session")
    step(con, "path copy", "CREATE TABLE path AS SELECT * FROM src.path")
    con.execute("DETACH src")
    con.execute("CHECKPOINT")
    print("rows, MB of text:", con.execute("select count(*), sum(strlen(text))/1e6 from doc").fetchall())
    print("database size:", con.execute("pragma database_size").fetchall())
    # Large strings live in overflow blocks that pragma_storage_info does not list, so the per-column
    # numbers below undercount the text columns; database_size above is the real footprint.
    for row in con.execute("""select column_name, count(distinct block_id) blocks, round(count(distinct block_id) * 262144 / 1e6) approx_mb
                              from pragma_storage_info('doc') group by column_name order by approx_mb desc""").fetchall():
        print("  ", row)


if __name__ == "__main__":
    main()
