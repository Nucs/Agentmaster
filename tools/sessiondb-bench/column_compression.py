# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check the per-column compression clause (CREATE TABLE ... VARCHAR USING COMPRESSION zstd).

The design (SESSIONS_DB.md section 5.1) compresses ONE column (the full block text) with ZSTD and
keeps the small, hot columns in DuckDB's default string method. This script proves the clause works
in the installed DuckDB and prints which method each string column got.

Usage: python column_compression.py <layout db>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def main() -> None:
    """Build a 20% sample with a ZSTD text column and a default short column; print the methods.

    Raises:
        SystemExit: the layout database path was not given.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = bc.work_path("column_compression.duckdb")
    bc.remove_db(path)
    con = bc.connect(path, memory="2GB")
    con.execute(f"ATTACH '{sys.argv[1]}' AS src (READ_ONLY)")
    try:
        con.execute("CREATE TABLE d (doc_id BIGINT, role TINYINT, text VARCHAR USING COMPRESSION zstd, small VARCHAR)")
        con.execute("INSERT INTO d SELECT doc_id, role, text, left(text, 40) FROM src.doc WHERE doc_id % 5 = 0")
        con.execute("CHECKPOINT")
        methods = con.execute("""select column_name, compression, count(*) from pragma_storage_info('d')
                                 where segment_type = 'VARCHAR' group by all order by 1""").fetchall()
        print("per-column zstd OK; file MB:", round(os.path.getsize(path) / 1e6), methods)
    except Exception as ex:  # an older DuckDB rejects the clause: say so instead of a stack trace
        print("per-column zstd failed:", str(ex)[:300])
    con.close()
    bc.remove_db(path)


if __name__ == "__main__":
    main()
