# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""String compression on real block text (SESSIONS_DB.md sections 5.1 and 14).

Copies a 20% sample of the layout database's block text into fresh files, each with one compression
method forced for the whole database, and prints the file size and the time of a substring scan. The
finding behind the design: DuckDB's default methods leave large strings uncompressed (the 270 MB
sample took 329 MB), while ZSTD keeps it in 70 MB at 2.6x the scan cost.

Usage: python compress_test.py <layout db>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

METHODS = ("auto", "zstd", "fsst", "dictionary", "uncompressed")


def measure(src: str, method: str) -> None:
    """Build the sample with one method, print its size and scan time, delete it.

    Args:
        src: the layout database (build_layout.py) to sample from.
        method: "auto" (DuckDB's own choice) or a value for `force_compression`.
    """
    path = bc.work_path(f"compress_{method}.duckdb")
    bc.remove_db(path)
    con = bc.connect(path, memory="2GB")
    try:
        if method != "auto":
            con.execute(f"SET force_compression = '{method}'")
        con.execute(f"ATTACH '{src}' AS src (READ_ONLY)")
        start = time.time()
        con.execute("CREATE TABLE d AS SELECT doc_id, role, text FROM src.doc WHERE doc_id % 5 = 0")
        con.execute("DETACH src")
        con.execute("CHECKPOINT")
        build = time.time() - start
        rows, mb = con.execute("select count(*), sum(strlen(text))/1e6 from d").fetchone()
        best = None
        for _ in range(3):
            t = time.perf_counter()
            con.execute("select count(*) from d where contains(text, 'duckdb')").fetchall()
            elapsed = (time.perf_counter() - t) * 1000
            best = elapsed if best is None else min(best, elapsed)
        con.close()
        print(f"{method:12s} rows={rows} text={mb:.0f} MB  file={os.path.getsize(path) / 1e6:.0f} MB  "
              f"build={build:.1f}s  scan={best:.0f} ms", flush=True)
    except Exception as ex:  # one method failing must not hide the others' numbers
        print(method, "failed:", str(ex)[:200])
        con.close()
    bc.remove_db(path)


def main() -> None:
    """Measure every method of METHODS.

    Raises:
        SystemExit: the layout database path was not given.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for method in METHODS:
        measure(sys.argv[1], method)


if __name__ == "__main__":
    main()
