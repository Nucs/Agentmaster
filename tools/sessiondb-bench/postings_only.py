# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Postings fetch with the term ids already known (SESSIONS_DB.md sections 6.3 and 6.5).

The design keeps the dictionary in the service's memory, so a lookup starts with term ids. This
script times exactly that part, on our (term, block, count) table `post` and on the fts extension's
per-occurrence `terms` table: the per-(term, block) table has 2.3x fewer rows and fetches common
terms 2-6x faster. It also shows the 0.3 ms floor of a trivial query.

Usage: python postings_only.py <fts db, after bench_layout.py added `post`>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def main() -> None:
    """Resolve three term ids and a broad expansion once, then time the fetches with literal ids.

    Raises:
        SystemExit: the database path was not given.
        duckdb.Error: the database lacks `post` or the fts index.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    con = bc.connect(sys.argv[1], read_only=True)
    con.execute("LOAD fts")
    ids = dict(con.execute("select term, termid from fts_main_d.dict where term in ('duckdb', 'release', 'the')").fetchall())
    broad = [row[0] for row in con.execute("select termid from fts_main_d.dict where contains(term, 'str')").fetchall()]
    print("broad 'str' expands to", len(broad), "terms")
    bc.timed(con, "noop select 1", "select 1", reps=5)
    for word in ("duckdb", "release", "the"):
        if word not in ids:
            print(f"   '{word}' is not in this corpus's dictionary: skipped")
            continue
        bc.timed(con, f"post  termid={ids[word]} ({word})", f"select docid, tf from post where termid = {ids[word]}", reps=5)
        bc.timed(con, f"terms termid={ids[word]} ({word})", f"select docid from fts_main_d.terms where termid = {ids[word]}", reps=5)
    if not broad:
        print("   no dictionary term contains 'str': the broad expansion is skipped")
        return
    # A literal IN list of 14 K ids is the slow way on purpose: session_postings.py shows the
    # semi-join through a temporary table, which the design uses.
    id_list = ",".join(map(str, broad))
    bc.timed(con, "post  broad IN-list, distinct blocks", f"select count(distinct docid) from post where termid in ({id_list})")
    bc.timed(con, "terms broad IN-list, distinct blocks", f"select count(distinct docid) from fts_main_d.terms where termid in ({id_list})")


if __name__ == "__main__":
    main()
