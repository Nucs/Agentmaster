# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read an fts index's TABLES directly, bypassing the extension's macros (SESSIONS_DB.md section 6.5).

The finding it documents: the index itself answers in 11-80 ms on the full corpus; the extension's
retrieval macros add 40-120 ms on top (their CTEs, BM25 scoring and joins). It also prints the
schema of the index tables (dict, terms, docs, ...), which a direct reader depends on.

Usage: python fts_probe.py <fts db written by fts_build.py>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

TABLES = ("dict", "raw_dict", "terms", "term_grams", "term_prefixes", "docs", "term_stats", "stats", "fields")

QUERIES = [
    ("dict lookup 'duckdb'", "select termid from fts_main_d.dict where term = 'duckdb'"),
    ("postings for 'duckdb' (distinct blocks)", "select distinct docid from fts_main_d.terms where termid = (select termid from fts_main_d.dict where term = 'duckdb')"),
    ("postings for 'release'", "select distinct docid from fts_main_d.terms where termid = (select termid from fts_main_d.dict where term = 'release')"),
    ("dict substring scan contains 'sessionstable'", "select termid from fts_main_d.dict where contains(term, 'sessionstable')"),
    ("postings for dict terms containing 'sessionstable'", "select distinct docid from fts_main_d.terms where termid in (select termid from fts_main_d.dict where contains(term, 'sessionstable'))"),
    ("dict prefix 'rendersess'", "select termid from fts_main_d.dict where starts_with(term, 'rendersess')"),
    ("postings for dict terms containing 'str' (very broad)", "select count(distinct docid) from fts_main_d.terms where termid in (select termid from fts_main_d.dict where contains(term, 'str'))"),
    ("AND tab + drag + crash (blocks holding all three)", """select docid from fts_main_d.terms
        where termid in (select termid from fts_main_d.dict where term in ('tab', 'drag', 'crash'))
        group by docid having count(distinct termid) = 3"""),
    ("session hit counts for 'duckdb' (postings + joins)", """select d.sid, count(*) from (select distinct docid from fts_main_d.terms
        where termid = (select termid from fts_main_d.dict where term = 'duckdb')) p
        join fts_main_d.docs x on x.docid = p.docid join d on d.doc_id = x.name::BIGINT group by d.sid"""),
    ("brute force on the same blocks: contains(t, 'duckdb')", "select count(*) from d where contains(t, 'duckdb')"),
    ("brute force on the same blocks: contains(t, 'sessionstable')", "select count(*) from d where contains(t, 'sessionstable')"),
]


def main() -> None:
    """Print the database size, the index tables' schemas and samples, then time QUERIES.

    Raises:
        SystemExit: the database path was not given.
        duckdb.Error: the database has no fts index on table `d`.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    con = bc.connect(sys.argv[1])
    con.execute("LOAD fts")
    print(con.execute("pragma database_size").fetchall())
    for table in TABLES:
        print(table, con.execute(f"describe fts_main_d.{table}").fetchall())
    print("dict sample:", con.execute("select * from fts_main_d.dict limit 3").fetchall())
    print("terms sample:", con.execute("select * from fts_main_d.terms limit 3").fetchall())
    for label, sql in QUERIES:
        bc.timed(con, label, sql, reps=5)


if __name__ == "__main__":
    main()
