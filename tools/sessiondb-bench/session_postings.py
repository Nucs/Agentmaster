# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Session-level postings `spost` (term -> session, block count): SESSIONS_DB.md section 6.3.

The Sessions page mostly asks "which sessions mention X, and how often". Block-level postings answer
that in time proportional to the term's frequency (223 ms for `the`, 575 K blocks); a table keyed by
(term, session) answers it in ~2 ms whatever the frequency. This script builds that table from
bench_layout.py's `post`, prints its cost and size and the dictionary's size, and times lookups for
a rare word, a common word, a very common word and a broad 3-letter substring.

Usage: python session_postings.py <fts db, after bench_layout.py added `post`>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def main() -> None:
    """Build `spost`, then time the lookups.

    Raises:
        SystemExit: the database path was not given.
        duckdb.Error: the database lacks `post`, the fts index or the `d` table.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    con = bc.connect(sys.argv[1])
    con.execute("LOAD fts")
    print("dict (terms, chars, avg, max, longer than 64):",
          con.execute("select count(*), sum(strlen(term)), round(avg(strlen(term)),1), max(strlen(term)), count(*) filter (where strlen(term) > 64) from fts_main_d.dict").fetchone())
    # The fts index numbers its documents itself (docs.docid); map them to sessions, and number the
    # sessions densely, as the design's sid_key does.
    con.execute("CREATE OR REPLACE TEMP TABLE dmap AS SELECT x.docid::UINTEGER AS docid, d.sid FROM fts_main_d.docs x JOIN d ON d.doc_id = x.name")
    con.execute("CREATE OR REPLACE TEMP TABLE sids AS SELECT sid, row_number() OVER (ORDER BY sid)::INTEGER AS sid_key FROM (SELECT DISTINCT sid FROM d)")
    start = time.time()
    con.execute("""CREATE OR REPLACE TABLE spost AS
                   SELECT p.termid, s.sid_key, count(*)::UINTEGER AS hits
                   FROM post p JOIN dmap m ON m.docid = p.docid JOIN sids s ON s.sid = m.sid
                   GROUP BY ALL ORDER BY p.termid, s.sid_key""")
    con.execute("CHECKPOINT")
    print(f"spost build {time.time() - start:.1f} s, rows", con.execute("select count(*) from spost").fetchone()[0])
    for row in con.execute("select column_name, count(distinct block_id), round(count(distinct block_id) * 262144 / 1e6) from pragma_storage_info('spost') group by column_name").fetchall():
        print("   ", row)
    ids = dict(con.execute("select term, termid from fts_main_d.dict where term in ('duckdb', 'release', 'the')").fetchall())
    for word in ("duckdb", "release", "the"):
        if word not in ids:
            print(f"   '{word}' is not in this corpus's dictionary: skipped")
            continue
        bc.timed(con, f"spost sessions for '{word}'", f"select sid_key, hits from spost where termid = {ids[word]}", reps=5)
    con.execute("CREATE OR REPLACE TEMP TABLE exp AS SELECT termid::UINTEGER AS termid FROM fts_main_d.dict WHERE contains(term, 'str')")
    print("'str' expands to", con.execute("select count(*) from exp").fetchone()[0], "terms")
    bc.timed(con, "spost sessions for the 'str' expansion (hits summed per term)", "select sid_key, sum(hits) from spost where termid in (select termid from exp) group by sid_key", reps=5)
    bc.timed(con, "post distinct blocks for the 'str' expansion", "select count(distinct docid) from post where termid in (select termid from exp)", reps=5)
    bc.timed(con, "post exact blocks per session for 'str'", """select m.sid, count(distinct p.docid) from post p join dmap m on m.docid = p.docid
        where p.termid in (select termid from exp) group by m.sid""")


if __name__ == "__main__":
    main()
