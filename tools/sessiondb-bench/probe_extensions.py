# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Which DuckDB extensions the installed build can load, and whether its fts knows the 2.0 options.

Prints the DuckDB version, installs and loads `fts`, `quack` and `vss` (downloads into the work
directory, never ~/.duckdb), lists their versions (the fts commit tells the February 2026 static code
from the July 2026 incremental / layered rewrite: SESSIONS_DB.md section 3.2), and tries an
incremental, layered index on a tiny in-memory table.

Note the syntax trap it encodes: PRAGMA named parameters take `=` (`incremental = true`); the
table macros take `:=` (`top_k := 10`). `:=` in a PRAGMA is a parser error.

Usage: python probe_extensions.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def main() -> None:
    """Probe the extensions and the new fts options; every failure is printed, none is fatal."""
    bc.utf8_stdout()
    con = bc.connect(":memory:", memory="1GB")
    print("version:", con.execute("pragma version").fetchall())
    for name in ("fts", "quack", "vss"):
        try:
            con.execute(f"INSTALL {name}")
            con.execute(f"LOAD {name}")
            print(name, "loaded OK")
        except Exception as ex:  # a missing build for this version/platform is a finding, not a crash
            print(name, "failed:", str(ex)[:300])
    for row in con.execute("""select extension_name, loaded, installed, install_mode, extension_version
                              from duckdb_extensions() where extension_name in ('fts','quack','vss','json','icu') order by 1""").fetchall():
        print(row)
    try:
        con.execute("create table t(id int not null primary key, s varchar)")
        con.execute("insert into t values (1, 'hello world'), (2, 'the quick brown fox')")
        con.execute("pragma create_fts_index('t', 'id', 's', incremental = true, layered_search = true, stemmer = 'none', stopwords = 'none')")
        con.execute("insert into t values (3, 'hello again')")
        # On three documents the default max_df_ratio (0.15) filters every expansion of 'hel', so an
        # empty answer here is expected: it shows the caps that make the macros unfit as a filter.
        print("search 'hel':", con.execute("select docname, score, rank from fts_main_t.search_layered_bm25('hel', top_k := 10)").fetchall())
        print("incremental + layered OK (in-memory database)")
    except Exception as ex:  # 1.5's fts rejects these options: print which
        print("new fts options failed:", str(ex)[:400])


if __name__ == "__main__":
    main()
