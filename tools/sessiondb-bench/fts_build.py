# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Option A of SESSIONS_DB.md section 6.5: DuckDB's own `fts` extension on the real block text.

Builds an INCREMENTAL, LAYERED index (DuckDB 2.0's fts: triggers on INSERT / DELETE, prefix and
substring expansion through dictionary sidecars) over the folded agent / tool text of the layout
database (roles 2-6), times the extension's own query macros, and (unless --skip-incremental) the
cost of inserting and deleting blocks afterwards.

Needs DuckDB 2.0 (the 1.5 line ships the older static fts, without `incremental` or
`layered_search`). ⚠ `INSTALL fts` downloads the signed extension from extensions.duckdb.org into the
work directory on first use (46 MB on the 2.0 alpha for Windows x64).

The tokenizer settings are code-aware: no stemming, no stopwords, the text is already folded, and
everything that is not a Unicode letter, digit or `_` separates tokens (the extension's default
pattern would also drop digits, so `0xc000027b` would not be findable).

Usage: python fts_build.py <layout db> <mod> [--skip-incremental]
       mod 5 indexes every 5th block (a 20% sample), mod 1 indexes all of them.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

CREATE_INDEX = r"""
PRAGMA create_fts_index('d', 'doc_id', 't', stemmer = 'none', stopwords = 'none', lower = 0, strip_accents = 0,
                        ignore = '[^\p{L}\p{N}_]+', layered_search = 1, incremental = 1)
"""

# The extension's own retrieval macros; `top_k := NULL` asks for every match (a filter, not a top-k).
# Phrase mode is not in the 2.0 alpha build: search_layered_bm25 accepts only standard / autocomplete.
MACRO_QUERIES = [
    ("exact 'duckdb' (no expansion)", "select docname from fts_main_d.search_layered_bm25('duckdb', top_k := NULL, enable_prefix := false, enable_substring := false, enable_fuzzy := false)"),
    ("standard 'duckdb' (default expansions)", "select docname from fts_main_d.search_layered_bm25('duckdb', top_k := NULL)"),
    ("standard 'duckdb' top_k 50", "select docname from fts_main_d.search_layered_bm25('duckdb', top_k := 50)"),
    ("substring 'sessionstable' (all expansions, no df caps)", "select docname from fts_main_d.search_layered_bm25('sessionstable', top_k := NULL, term_limit := 100000, max_df_ratio := 1.0, max_df := NULL, enable_fuzzy := false)"),
    ("prefix 'rendersess' autocomplete", "select docname from fts_main_d.search_layered_bm25('rendersess', top_k := NULL, query_mode := 'autocomplete')"),
    ("standard 'release' (common term)", "select docname from fts_main_d.search_layered_bm25('release', top_k := NULL, enable_fuzzy := false)"),
    ("hits per session for 'duckdb' (exact)", """select d.sid, count(*) from fts_main_d.search_layered_bm25('duckdb', top_k := NULL, enable_prefix := false, enable_substring := false, enable_fuzzy := false) r
       join d on d.doc_id = r.docname::BIGINT group by d.sid"""),
]


def main() -> None:
    """Load the sample, build the index, time the macros and (optionally) incremental maintenance.

    The 1,000 rows of `later` are held back from the index so inserts can be timed afterwards.

    Raises:
        SystemExit: the arguments are missing.
        duckdb.Error: the build fails (an older DuckDB rejects `incremental`; out of memory).
    """
    bc.utf8_stdout()
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, mod = sys.argv[1], int(sys.argv[2])
    skip_incremental = "--skip-incremental" in sys.argv[3:]
    db = bc.work_path(f"fts_mod{mod}.duckdb")
    bc.remove_db(db)
    con = bc.connect(db)
    con.execute("INSTALL fts")
    con.execute("LOAD fts")
    con.execute(f"ATTACH '{src}' AS src (READ_ONLY)")
    # An incremental index needs a NOT NULL (or primary key) document id.
    con.execute("CREATE TABLE d (doc_id BIGINT NOT NULL PRIMARY KEY, sid VARCHAR, role TINYINT, t VARCHAR)")
    con.execute(f"INSERT INTO d SELECT doc_id, sid, role, text_fold FROM src.doc WHERE doc_id % {mod} = 0 AND role BETWEEN 2 AND 6")
    con.execute(f"CREATE TABLE later AS SELECT doc_id, sid, role, text_fold AS t FROM src.doc WHERE doc_id % {mod} = 1 AND role BETWEEN 2 AND 6 LIMIT 1000")
    con.execute("DETACH src")
    rows, mb = con.execute("select count(*), sum(strlen(t))/1e6 from d").fetchone()
    print(f"blocks={rows} text={mb:.0f} MB", flush=True)
    con.execute("CHECKPOINT")
    base_blocks = con.execute("select used_blocks from pragma_database_size()").fetchone()[0]

    start = time.time()
    con.execute(CREATE_INDEX)
    print(f"[{time.time() - start:7.1f} s] create_fts_index (layered, incremental, code-aware separators)", flush=True)
    con.execute("CHECKPOINT")
    used = con.execute("select used_blocks from pragma_database_size()").fetchone()[0]
    print(f"index size: {(used - base_blocks) * 262144 / 1e6:.0f} MB (used blocks before / after: {base_blocks} / {used})")
    for row in con.execute("select table_name, estimated_size, column_count from duckdb_tables() where schema_name like 'fts_main_d%' order by estimated_size desc").fetchall():
        print("   ", row)

    print("== the extension's query macros")
    for label, sql in MACRO_QUERIES:
        bc.timed(con, label, sql)

    if skip_incremental:
        return
    print("== incremental maintenance (triggers)")
    for label, sql in (("insert 100 blocks", "INSERT INTO d SELECT * FROM later ORDER BY doc_id LIMIT 100"),
                       ("insert 900 blocks", "INSERT INTO d SELECT * FROM later ORDER BY doc_id OFFSET 100"),
                       ("delete 100 blocks", "DELETE FROM d WHERE doc_id IN (SELECT doc_id FROM later ORDER BY doc_id LIMIT 100)")):
        start = time.time()
        con.execute(sql)
        print(f"{label}: {(time.time() - start) * 1000:.0f} ms", flush=True)
    bc.timed(con, "after the inserts: exact 'duckdb'", MACRO_QUERIES[0][1])
    con.execute("CHECKPOINT")
    print(f"database size after: {os.path.getsize(db) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
