# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The LOSSLESS store: can the database give a transcript back? (SESSIONS_DB.md section 5.5)

Every JSONL line of every transcript (top-level and subagent) is stored VERBATIM, one row per line,
with a ZSTD-compressed text column; every spilled tool output (`<sid>/tool-results/*`) as one row per
file. The script then prints:
  - the size on disk against the raw bytes (the compression ratio of raw JSONL), and the raw volume
    per line type;
  - a byte-exact rebuild of sample files (SHA-256 against the file on disk);
  - how much of the raw volume is fork copies (a line whose `uuid` appears in another file), and
    which top-level fields differ between a copy and its original (what a deduplicating store must
    keep per copy).

The database it writes is the input of access_paths.py (clustering against ART, section 6.8).

⚠ Spills are read as TEXT here, and DuckDB's read_text refuses a batch that holds a file that is not
valid UTF-8 (4 of 11 batches on 2026-10-12): those batches are skipped and counted out. The design
stores spills as BLOB.

READ-ONLY on ~/.claude. Writes the database named on the command line (several GB for the whole
corpus) and nothing else.

Usage: python raw_lines.py <db path> [project-folder glob, default *]
"""

import glob
import hashlib
import json
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

PROJECTS = os.path.expanduser("~/.claude/projects").replace("\\", "/")
BATCH_BYTES = 256 << 20
UUID_STEM = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\.jsonl$")

SCHEMA = """
CREATE TABLE file (file_id INTEGER, path VARCHAR, sub BOOLEAN, content_bytes BIGINT);
-- line_id = file_id << 32 | line_no: ascending in file order, so a file is one contiguous id range.
-- `raw` holds the line exactly as written (no newline). Splitting the file on '\\n' and joining the
-- pieces with '\\n' gives the file back, the empty piece after a final newline included.
CREATE TABLE line (line_id BIGINT, file_id INTEGER, line_no INTEGER, uuid VARCHAR, type VARCHAR,
                   bytes INTEGER, raw VARCHAR USING COMPRESSION zstd);
CREATE TABLE spill (path VARCHAR, bytes BIGINT, content VARCHAR USING COMPRESSION zstd);
"""


def list_files(project_glob: str):
    """The transcripts to store: top-level session files (uuid stem) and subagent files.

    Args:
        project_glob: a glob over the project folder names.

    Returns:
        A list of (path, is_subagent, size_bytes), sorted by path; the index in the list is the
        file_id.
    """
    top = [p.replace("\\", "/") for p in glob.glob(f"{PROJECTS}/{project_glob}/*.jsonl") if UUID_STEM.search(p)]
    sub = [p.replace("\\", "/") for p in glob.glob(f"{PROJECTS}/{project_glob}/*/subagents/*.jsonl")]
    files = [(p, False) for p in top] + [(p, True) for p in sub]
    files.sort()
    return [(p, s, os.path.getsize(p)) for p, s in files]


def ingest(con, files) -> float:
    """Store every line of every file, in file order, batch by batch.

    `read_text` reads a whole file as one string; `string_split` on '\\n' plus `generate_subscripts`
    numbers its lines. The `uuid` and `type` of each line come from a JSON parse that is wrapped in
    TRY, so a non-JSON line (a partial last line being written) stores as text with NULL fields. A
    batch that fails as a whole (a file that is not valid UTF-8, a file deleted meanwhile) is retried
    file by file, and a failing file is reported and skipped.

    Args:
        con: the open database (schema created).
        files: list_files()'s result.

    Returns:
        The seconds the ingest took.
    """
    con.execute("CREATE TEMP TABLE fmap (path VARCHAR, file_id INTEGER, sub BOOLEAN)")
    con.executemany("INSERT INTO fmap VALUES (?, ?, ?)", [(p, i, s) for i, (p, s, _) in enumerate(files)])
    started = time.time()
    batch, batch_bytes, done, skipped = [], 0, 0, 0
    for i, (path, _sub, size) in enumerate(files):
        batch.append(path)
        batch_bytes += size
        if batch_bytes >= BATCH_BYTES or i == len(files) - 1:
            skipped += ingest_batch(con, batch)
            done += len(batch)
            print(f"   {done}/{len(files)} files, {time.time() - started:6.1f} s", flush=True)
            batch, batch_bytes = [], 0
    print(f"   skipped files: {skipped}")
    return time.time() - started


def ingest_batch(con, paths) -> int:
    """Store one batch of files; on failure retry them one by one.

    Args:
        con: the open database.
        paths: the files of the batch.

    Returns:
        How many files were skipped.
    """
    try:
        insert_files(con, paths)
        return 0
    except Exception as ex:  # one bad file must not cost the batch: retry each file alone
        print("   batch failed, retrying file by file:", str(ex)[:160])
    skipped = 0
    for path in paths:
        try:
            insert_files(con, [path])
        except Exception as ex:
            skipped += 1
            print("   skipped", path, str(ex)[:160])
    return skipped


def insert_files(con, paths) -> None:
    """The INSERTs for a list of files (one statement per table, so a failure inserts nothing).

    Args:
        con: the open database.
        paths: the files.

    Raises:
        duckdb.Error: a file could not be read (invalid UTF-8, deleted) or the insert failed.
    """
    files = "[" + ",".join("'" + p.replace("'", "''") + "'" for p in paths) + "]"
    con.execute("BEGIN")
    try:
        con.execute(f"CREATE OR REPLACE TEMP TABLE t AS SELECT filename, content FROM read_text({files})")
        con.execute("""INSERT INTO file SELECT m.file_id, m.path, m.sub, strlen(t.content)
                       FROM t JOIN fmap m ON m.path = replace(t.filename, '\\', '/')""")
        con.execute("""
            INSERT INTO line
            SELECT (file_id::BIGINT << 32) | line_no, file_id, line_no,
                   TRY(json_extract_string(raw, '$.uuid')), TRY(json_extract_string(raw, '$.type')),
                   strlen(raw), raw
            FROM (SELECT m.file_id,
                         unnest(string_split(t.content, chr(10))) AS raw,
                         generate_subscripts(string_split(t.content, chr(10)), 1) AS line_no
                  FROM t JOIN fmap m ON m.path = replace(t.filename, '\\', '/'))
            ORDER BY 1""")
        con.execute("DROP TABLE t")
        con.execute("COMMIT")
    except Exception:
        con.execute("ROLLBACK")
        raise


def sizes(con) -> None:
    """Print the raw volume, the stored size and the volume per line type.

    Args:
        con: the open database, ingest done and checkpointed.
    """
    rows, raw_bytes = con.execute("select count(*), sum(bytes) from line").fetchone()
    used = con.execute("select used_blocks * block_size from pragma_database_size()").fetchone()[0]
    print(f"== lines {rows}, raw {raw_bytes / 1e9:.2f} GB, database {used / 1e9:.2f} GB "
          f"(includes the spill table), ratio {raw_bytes / used:.1f}x")
    print("== raw volume per line type (type, lines, GB)")
    for row in con.execute("""select coalesce(type, '(not json / empty)'), count(*), round(sum(bytes)/1e9, 3)
                              from line group by 1 order by 3 desc limit 14""").fetchall():
        print("  ", row)
    n, b = con.execute("select count(*), coalesce(sum(bytes), 0) from spill").fetchone()
    print(f"== spilled tool outputs: {n} files, {b / 1e9:.2f} GB raw")


def forks(con) -> None:
    """Measure fork copies at the line level and what differs between a copy and its original.

    A copy is any occurrence of a `uuid` after its first one (lowest file id). For a sample of
    copies the two lines are parsed and their top-level keys compared, which shows what a
    deduplicating store would have to keep per copy.

    Args:
        con: the open database.
    """
    total, copies, copy_bytes, total_bytes = con.execute("""
        with d as (select uuid, bytes, row_number() over (partition by uuid order by file_id, line_no) rn
                   from line where uuid is not null)
        select count(*), count(*) filter (where rn > 1), sum(bytes) filter (where rn > 1), sum(bytes) from d""").fetchone()
    print(f"== fork copies: {copies} of {total} uuid lines ({copies / total:.1%}), "
          f"{copy_bytes / 1e9:.2f} of {total_bytes / 1e9:.2f} GB ({copy_bytes / total_bytes:.1%})")
    pairs = con.execute("""
        with d as (select uuid, raw, row_number() over (partition by uuid order by file_id, line_no) rn
                   from line where uuid is not null),
             s as (select uuid from d where rn = 2 using sample 2000 rows)
        select a.raw, b.raw from d a join d b on a.uuid = b.uuid and a.rn = 1 and b.rn = 2
        where a.uuid in (select uuid from s)""").fetchall()
    identical, differing = 0, {}
    for original, copy in pairs:
        if original == copy:
            identical += 1
            continue
        try:
            a, b = json.loads(original), json.loads(copy)
        except ValueError:
            continue
        for key in sorted(set(a) | set(b)):
            if a.get(key) != b.get(key):
                differing[key] = differing.get(key, 0) + 1
    print(f"   sample {len(pairs)} copies: {identical} byte-identical; differing top-level keys:",
          sorted(differing.items(), key=lambda kv: -kv[1])[:10])


def rebuild(con, files) -> None:
    """Rebuild sample files from the database and compare them with the files on disk.

    The comparison covers the bytes that were stored (`content_bytes`): a live transcript keeps
    growing after the ingest read it, so its current file is longer.

    Args:
        con: the open database.
        files: list_files()'s result (file_id = index).
    """
    stored = dict(con.execute("select file_id, content_bytes from file").fetchall())
    by_size = sorted(stored, key=lambda f: -stored[f])
    random.seed(7)
    sample = by_size[:5] + random.sample(by_size[5:], min(15, max(0, len(by_size) - 5)))
    ok = 0
    for file_id in sample:
        start = time.perf_counter()
        text = con.execute("select string_agg(raw, chr(10) order by line_no) from line where file_id = ?", [file_id]).fetchone()[0] or ""
        elapsed = (time.perf_counter() - start) * 1000
        rebuilt = text.encode("utf-8")
        with open(files[file_id][0], "rb") as fh:
            original = fh.read(stored[file_id])
        same = hashlib.sha256(rebuilt).digest() == hashlib.sha256(original).digest()
        ok += same
        if file_id in by_size[:2]:
            print(f"   rebuilt {stored[file_id] / 1e6:.1f} MB in {elapsed:.0f} ms, byte-exact: {same}")
    print(f"== byte-exact rebuild: {ok} of {len(sample)} sample files identical")


def spills(con) -> None:
    """Store every spilled tool output (`<sid>/tool-results/*`) as one row.

    Args:
        con: the open database.
    """
    paths = [p.replace("\\", "/") for p in glob.glob(f"{PROJECTS}/*/*/tool-results/*") if os.path.isfile(p)]
    for i in range(0, len(paths), 400):
        chunk = "[" + ",".join("'" + p.replace("'", "''") + "'" for p in paths[i:i + 400]) + "]"
        try:
            con.execute(f"INSERT INTO spill SELECT filename, strlen(content), content FROM read_text({chunk})")
        except Exception as ex:  # a binary spill (an image) is not text: count it out, keep going
            print("   spill chunk skipped:", str(ex)[:160])


def main() -> None:
    """Run the whole measurement.

    Raises:
        SystemExit: no database path was given.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    db = sys.argv[1]
    project_glob = sys.argv[2] if len(sys.argv) > 2 else "*"
    files = list_files(project_glob)
    print(f"{len(files)} files, {sum(s for _, _, s in files) / 1e9:.2f} GB")
    bc.remove_db(db)
    con = bc.connect(db)
    # File order matters here: the clustered layout IS the insertion order.
    con.execute("SET preserve_insertion_order = true")
    con.execute(SCHEMA)
    print(f"== ingest of every line: {ingest(con, files):.1f} s")
    if project_glob == "*":
        spills(con)
    con.execute("CHECKPOINT")
    sizes(con)
    forks(con)
    rebuild(con, files)


if __name__ == "__main__":
    main()
