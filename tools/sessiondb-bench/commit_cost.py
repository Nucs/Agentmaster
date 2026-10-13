# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""What keeping the database current costs DuckDB: small commits, a merge, a checkpoint (SESSIONS_DB.md section 7.6).

The always-on ingest appends a few lines every couple of seconds (append_rate.py: ~190 MB of transcript
lines a day here, 23 MB in the busiest hour). This script replays real transcript lines into a scratch
file database in the work folder (the same drive as the profile) and times:

  1. SMALL COMMITS: 200 transactions, each appending 20 lines to `line_hot` (uncompressed) and their
     text to `doc_hot` — the ingest's steady state. Prints the insert and the COMMIT (write-ahead-log
     flush) time per transaction, median and p95.
  2. A BUSY HOUR: 23 MB of lines appended in 20-line transactions, as fast as possible — how long a
     whole busy hour's ingest takes the database.
  3. THE MERGE: `line_hot` moved into the ZSTD `line` table sorted by line_id, in one transaction.
  4. A CHECKPOINT after all of it.

Each batch is inserted with one statement per table (parameter lists unnested into rows); the C API's
appender would be faster still, so the insert times are an upper bound. The COMMIT and CHECKPOINT times
are the database's own.

Usage: python commit_cost.py [db path, default <work dir>\\commit_cost.duckdb]   (the file is deleted first)
"""

import glob
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

SCHEMA = """
CREATE TABLE line (line_id BIGINT, file_id INTEGER, line_no INTEGER, uuid VARCHAR, type VARCHAR, ts_ms BIGINT,
                   bytes INTEGER, raw VARCHAR USING COMPRESSION zstd);
CREATE TABLE line_hot (line_id BIGINT, file_id INTEGER, line_no INTEGER, uuid VARCHAR, type VARCHAR, ts_ms BIGINT,
                       bytes INTEGER, raw VARCHAR);
CREATE TABLE doc_hot (doc_id UINTEGER, line_id BIGINT, role UTINYINT, text VARCHAR, text_fold VARCHAR);
"""


def sample_lines(limit_bytes: int) -> list:
    """Real transcript lines from the newest transcripts, up to a byte budget.

    Args:
        limit_bytes: stop after this many bytes.

    Returns:
        The lines (str, without their newline), in file order.
    """
    root = os.path.join(os.path.expanduser("~"), ".claude", "projects")
    files = sorted(glob.glob(os.path.join(root, "*", "*.jsonl")), key=lambda p: os.stat(p).st_mtime, reverse=True)
    out, total = [], 0
    for path in files:
        try:
            with open(path, "rb") as handle:
                for raw in handle:
                    line = raw.rstrip(b"\r\n").decode("utf-8", "replace")
                    if line:
                        out.append(line)
                        total += len(raw)
                    if total >= limit_bytes:
                        return out
        except OSError:
            continue
    return out


def append_batch(con, lines: list, first_line_id: int) -> tuple:
    """Append one ingest batch (each line to line_hot, its text to doc_hot) in ONE transaction.

    Args:
        con: the open database.
        lines: the batch's lines.
        first_line_id: the id of the batch's first line.

    Returns:
        (insert ms, commit ms).
    """
    ids = list(range(first_line_id, first_line_id + len(lines)))
    sizes = [len(text) for text in lines]
    texts = [text[:16384] for text in lines]
    start = time.perf_counter()
    con.execute("BEGIN TRANSACTION")
    # One statement per table: unnest() zips the parameter lists into rows. Python's executemany would
    # run one statement per row (~6 ms each here) and measure the client, not the database.
    con.execute("INSERT INTO line_hot SELECT unnest($1), 1, unnest($1), NULL, 'user', 0, unnest($2), unnest($3)", [ids, sizes, lines])
    con.execute("INSERT INTO doc_hot SELECT unnest($1), unnest($1), 2, unnest($2), lower(unnest($2))", [ids, texts])
    inserted = time.perf_counter()
    con.execute("COMMIT")
    done = time.perf_counter()
    return (inserted - start) * 1000, (done - inserted) * 1000


def p95(values: list) -> float:
    """The 95th percentile of a list (nearest rank).

    Args:
        values: the numbers.

    Returns:
        The value at rank ceil(0.95 n).
    """
    ordered = sorted(values)
    return ordered[max(0, int(len(ordered) * 0.95 + 0.999999) - 1)]


def main() -> None:
    """Run the four measurements on a fresh scratch database, then delete it.

    Raises:
        OSError: the scratch database cannot be created or deleted.
        duckdb.Error: a statement fails.
    """
    bc.utf8_stdout()
    path = sys.argv[1] if len(sys.argv) > 1 else bc.work_path("commit_cost.duckdb")
    bc.remove_db(path)
    lines = sample_lines(24_000_000)
    print(f"== {len(lines)} real lines, {sum(len(s) for s in lines) / 1e6:.1f} MB, from the newest transcripts; database {path}")
    con = bc.connect(path)
    con.execute(SCHEMA)

    inserts, commits, next_id, cursor = [], [], 0, 0
    for _ in range(200):
        batch = [lines[(cursor + i) % len(lines)] for i in range(20)]  # cycled: never an empty batch
        cursor += 20
        ins, com = append_batch(con, batch, next_id)
        next_id += len(batch)
        inserts.append(ins)
        commits.append(com)
    print(f"== 1. 200 transactions of 20 lines: insert median {statistics.median(inserts):.2f} ms (p95 {p95(inserts):.2f}), "
          f"COMMIT median {statistics.median(commits):.2f} ms (p95 {p95(commits):.2f})")

    start = time.perf_counter()
    hour_bytes = 0
    batches = 0
    while hour_bytes < 23_000_000:
        # The sample is cycled: the busiest hour measured here wrote 23.4 MB (append_rate.py).
        batch = [lines[(cursor + i) % len(lines)] for i in range(20)]
        cursor += 20
        append_batch(con, batch, next_id)
        next_id += len(batch)
        hour_bytes += sum(len(s) for s in batch)
        batches += 1
    elapsed = time.perf_counter() - start
    print(f"== 2. a busy hour: {hour_bytes / 1e6:.1f} MB in {batches} transactions took {elapsed:.1f} s of database time "
          f"({elapsed / 3600 * 100:.2f}% of that hour)")

    hot_rows = con.execute("select count(*) from line_hot").fetchone()[0]
    start = time.perf_counter()
    con.execute("BEGIN TRANSACTION")
    con.execute("INSERT INTO line SELECT * FROM line_hot ORDER BY line_id")
    con.execute("DELETE FROM line_hot")
    con.execute("COMMIT")
    print(f"== 3. merge of {hot_rows} hot lines into the ZSTD line table: {(time.perf_counter() - start) * 1000:.0f} ms")

    start = time.perf_counter()
    con.execute("CHECKPOINT")
    print(f"== 4. checkpoint: {(time.perf_counter() - start) * 1000:.0f} ms; file {os.path.getsize(path) / 1e6:.1f} MB")
    con.close()
    bc.remove_db(path)


if __name__ == "__main__":
    main()
