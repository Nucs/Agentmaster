# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Option B of SESSIONS_DB.md section 6.5, and the scans of the design's layout.

Three measurements:
  1. typed prompts on the production layout (clustered by role, pre-folded text): the T2 tier;
  2. the "hot tier": blocks of the last 24 h / 7 days searched by brute force, as the design does for
     blocks written since the last merge (section 6.4);
  3. our own clustered postings: (term, block, count) rows rebuilt from the fts index's occurrence
     table in one sorted CREATE TABLE AS (the re-cluster / compaction cost of Option B), with lookups.

Step 3 WRITES a table `post` into the fts database; postings_only.py and session_postings.py use it.

Usage: python bench_layout.py <layout db> <fts db written by fts_build.py with mod 1>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def prompts_and_hot(layout: str) -> None:
    """Time the T2 prompt scan and the hot-tier scans on the layout database.

    Args:
        layout: the database written by build_layout.py.
    """
    con = bc.connect(layout, read_only=True)
    print("== typed prompts (role 1), clustered + pre-folded")
    print("   prompt rows, MB:", con.execute("select count(*), round(sum(strlen(text_fold))/1e6,1) from doc where role = 1 and not meta").fetchone())
    for term in ("release", "duckdb", "tab drag", "e"):
        bc.timed(con, f"prompts contains '{term}'", f"select sid, count(*) from doc where role = 1 and not meta and contains(text_fold, '{term}') group by sid")
    bc.timed(con, "prompts AND tab / drag / crash", """select sid, count(*) from doc where role = 1 and not meta
        and contains(text_fold,'tab') and contains(text_fold,'drag') and contains(text_fold,'crash') group by sid""")
    print("== hot tier: recent blocks, brute force on pre-folded text")
    newest = con.execute("select max(ts_ms) from doc").fetchone()[0]
    for hours in (24, 24 * 7):
        since = newest - hours * 3600 * 1000
        print(f"   last {hours} h (blocks, MB):", con.execute(f"select count(*), round(sum(strlen(text_fold))/1e6,1) from doc where ts_ms >= {since}").fetchone())
        bc.timed(con, f"hot {hours} h contains 'duckdb'", f"select sid, count(*) from doc where ts_ms >= {since} and contains(text_fold, 'duckdb') group by sid")
    con.close()


def own_postings(fts_db: str) -> None:
    """Rebuild (term, block, count) postings sorted by term, print the cost and size, time lookups.

    Args:
        fts_db: the database written by fts_build.py (its occurrence table is the token source).
    """
    con = bc.connect(fts_db)
    con.execute("LOAD fts")
    start = time.time()
    con.execute("""CREATE OR REPLACE TABLE post AS
                   SELECT termid::UINTEGER AS termid, docid::UINTEGER AS docid, count(*)::USMALLINT AS tf
                   FROM fts_main_d.terms GROUP BY ALL ORDER BY termid, docid""")
    con.execute("CHECKPOINT")
    print(f"== clustered postings rebuild: {time.time() - start:.1f} s, rows", con.execute("select count(*) from post").fetchone()[0], flush=True)
    for row in con.execute("select column_name, count(distinct block_id), round(count(distinct block_id) * 262144 / 1e6) from pragma_storage_info('post') group by column_name").fetchall():
        print("   ", row)
    bc.timed(con, "post lookup 'duckdb'", "select docid, tf from post where termid = (select termid from fts_main_d.dict where term = 'duckdb')")
    bc.timed(con, "post lookup 'release'", "select docid, tf from post where termid = (select termid from fts_main_d.dict where term = 'release')")
    bc.timed(con, "post substring 'str' distinct blocks", "select count(distinct docid) from post where termid in (select termid from fts_main_d.dict where contains(term, 'str'))")
    con.close()


def main() -> None:
    """Run both parts.

    Raises:
        SystemExit: the two database paths were not given.
        duckdb.Error: a database lacks the expected tables.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    prompts_and_hot(sys.argv[1])
    own_postings(sys.argv[2])


if __name__ == "__main__":
    main()
