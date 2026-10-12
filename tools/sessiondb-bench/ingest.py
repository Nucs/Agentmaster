# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Prototype ingest of the real Claude Code corpus into a scratch DuckDB file (SESSIONS_DB.md section 14).

Reads ~/.claude/projects (READ-ONLY) with DuckDB's own JSON reader and writes four tables shaped
like the design's schema:

    line_meta  the envelope of every JSONL line (type, uuid, timestamps, cwd, branch, fork lineage,
               titles, model, token usage)
    doc        one row per searchable BLOCK, with ClassifyTranscriptLine's rules: role 1 a typed
               prompt (string content or a text block of a user line), 2 assistant text, 3 thinking,
               4 a tool call (name + input JSON), 5 a tool result, 6 system content; attachments are
               not indexed (injected context)
    path       distinct (session, path, tool) for Read/Edit/Write/... file_path, notebook_path, path
    session    per-session facts derived from line_meta at the end

Files are ingested in batches of ~256 MB, one temporary table per batch. A single CREATE TABLE AS
over the whole corpus ran out of a 3 GB memory budget: it buffers ~120 K rows of raw JSON per thread
before compressing them. Batching is also how a production indexer works (per file, incremental).

Usage: python ingest.py <db path> [project-folder glob, default *]
"""

import glob
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

PROJECTS = os.path.expanduser("~/.claude/projects").replace("\\", "/")
BATCH_BYTES = 256 << 20
UUIDRX = r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
# The largest line in the corpus was 18.5 MB; DuckDB sizes its read buffers from this cap, so a
# generous value (256 MB was tried) runs out of memory before reading anything.
MAX_OBJECT = 20 << 20

SCHEMA = """
CREATE TABLE IF NOT EXISTS line_meta (sid VARCHAR, sub BOOLEAN, type VARCHAR, uuid VARCHAR, parent_uuid VARCHAR,
    ts TIMESTAMPTZ, sidechain BOOLEAN, meta BOOLEAN, cwd VARCHAR, branch VARCHAR, forked_from VARCHAR,
    subtype VARCHAR, custom_title VARCHAR, ai_title VARCHAR, summary VARCHAR, model VARCHAR,
    in_tok BIGINT, out_tok BIGINT, cache_read_tok BIGINT, cache_write_tok BIGINT, line_bytes INTEGER);
CREATE TABLE IF NOT EXISTS doc (sid VARCHAR, sub BOOLEAN, uuid VARCHAR, bi INTEGER, ts TIMESTAMPTZ,
    role TINYINT, tool VARCHAR, text VARCHAR, meta BOOLEAN, sidechain BOOLEAN);
CREATE TABLE IF NOT EXISTS path (sid VARCHAR, p VARCHAR, tool VARCHAR);
"""

# One batch's lines: the envelope fields plus the message content as JSON. A subagent line carries
# its PARENT's session id in `sessionId`; a top-level line's session id is its file name.
BATCH_LINES = """
CREATE OR REPLACE TEMP TABLE bl AS
SELECT
    sub,
    CASE WHEN sub THEN j->>'sessionId' ELSE regexp_extract(filename, '({uuid})\\.jsonl$', 1) END AS sid,
    j->>'type' AS type, j->>'uuid' AS uuid, j->>'parentUuid' AS parent_uuid,
    try_cast(j->>'timestamp' AS TIMESTAMPTZ) AS ts,
    coalesce(try_cast(j->>'isSidechain' AS BOOLEAN), false) AS sidechain,
    coalesce(try_cast(j->>'isMeta' AS BOOLEAN), false) OR coalesce(try_cast(j->>'isCompactSummary' AS BOOLEAN), false) AS meta,
    j->>'cwd' AS cwd, j->>'gitBranch' AS branch, j->'forkedFrom'->>'sessionId' AS forked_from,
    j->'message'->'content' AS content,
    CASE WHEN j->>'type' = 'system' THEN j->>'content' END AS sys_content,
    j->>'subtype' AS subtype, j->>'customTitle' AS custom_title, j->>'aiTitle' AS ai_title,
    CASE WHEN j->>'type' = 'summary' THEN j->>'summary' END AS summary,
    j->'message'->>'model' AS model,
    try_cast(j->'message'->'usage'->>'input_tokens' AS BIGINT) AS in_tok,
    try_cast(j->'message'->'usage'->>'output_tokens' AS BIGINT) AS out_tok,
    try_cast(j->'message'->'usage'->>'cache_read_input_tokens' AS BIGINT) AS cache_read_tok,
    try_cast(j->'message'->'usage'->>'cache_creation_input_tokens' AS BIGINT) AS cache_write_tok,
    line_bytes
FROM b
"""

INSERT_META = """
INSERT INTO line_meta SELECT sid, sub, type, uuid, parent_uuid, ts, sidechain, meta, cwd, branch, forked_from,
    subtype, custom_title, ai_title, summary, model, in_tok, out_tok, cache_read_tok, cache_write_tok, line_bytes FROM bl
"""

# The block extraction: a string user content is one typed prompt; array contents are unnested into
# their blocks; system lines contribute their content.
INSERT_DOC = r"""
INSERT INTO doc
WITH arr AS (
    SELECT sid, sub, uuid, ts, type, meta, sidechain,
           unnest(json_extract(content, '$[*]')) AS b,
           generate_subscripts(json_extract(content, '$[*]'), 1) AS bi
    FROM bl WHERE json_type(content) = 'ARRAY'
)
SELECT sid, sub, uuid, 0, ts, 1, NULL, content->>'$', meta, sidechain
FROM bl WHERE type = 'user' AND json_type(content) = 'VARCHAR'
UNION ALL
SELECT sid, sub, uuid, bi, ts,
       CASE b->>'type' WHEN 'text' THEN CASE WHEN type = 'user' THEN 1 ELSE 2 END
                       WHEN 'thinking' THEN 3 WHEN 'tool_use' THEN 4 WHEN 'tool_result' THEN 5 END,
       CASE WHEN b->>'type' = 'tool_use' THEN b->>'name' END,
       CASE b->>'type'
            WHEN 'text' THEN b->>'text'
            WHEN 'thinking' THEN b->>'thinking'
            WHEN 'tool_use' THEN (b->>'name') || ' ' || CAST(b->'input' AS VARCHAR)
            WHEN 'tool_result' THEN CASE json_type(b->'content')
                    WHEN 'VARCHAR' THEN b->'content'->>'$'
                    ELSE array_to_string(json_extract_string(b->'content', '$[*].text'), chr(10)) END
       END,
       meta, sidechain
FROM arr WHERE b->>'type' IN ('text', 'thinking', 'tool_use', 'tool_result')
UNION ALL
SELECT sid, sub, uuid, 0, ts, 6, NULL, sys_content, meta, sidechain FROM bl WHERE type = 'system' AND sys_content IS NOT NULL
"""

INSERT_PATH = r"""
INSERT INTO path
WITH tu AS (SELECT sid, unnest(json_extract(content, '$[*]')) AS b FROM bl WHERE type = 'assistant' AND json_type(content) = 'ARRAY')
SELECT DISTINCT sid, coalesce(b->'input'->>'file_path', b->'input'->>'notebook_path', b->'input'->>'path'), b->>'name'
FROM tu WHERE b->>'type' = 'tool_use'
  AND coalesce(b->'input'->>'file_path', b->'input'->>'notebook_path', b->'input'->>'path') IS NOT NULL
"""

SESSION_FACTS = r"""
CREATE OR REPLACE TABLE session AS
SELECT sid,
       min(ts) FILTER (WHERE type IN ('user', 'assistant'))                       AS first_ts,
       max(ts) FILTER (WHERE type IN ('user', 'assistant') AND NOT sidechain)     AS last_ts,
       arg_min(cwd, ts) FILTER (WHERE cwd IS NOT NULL)                           AS cwd,
       arg_min(branch, ts) FILTER (WHERE branch IS NOT NULL)                     AS branch,
       any_value(forked_from) FILTER (WHERE forked_from IS NOT NULL)             AS forked_from,
       last(custom_title) FILTER (WHERE custom_title IS NOT NULL)                AS custom_title,
       last(ai_title) FILTER (WHERE ai_title IS NOT NULL)                        AS ai_title,
       last(summary) FILTER (WHERE summary IS NOT NULL)                          AS summary,
       count(*) FILTER (WHERE type = 'assistant')                                AS assistant_lines,
       sum(coalesce(out_tok, 0))                                                 AS out_tokens,
       sum(line_bytes)                                                           AS bytes,
       mode(model) FILTER (WHERE model IS NOT NULL AND model <> '<synthetic>')   AS model
FROM line_meta WHERE NOT sub
GROUP BY sid
"""


def list_files(project_glob: str):
    """The files to ingest: top-level session transcripts (uuid stem) and subagent transcripts.

    Args:
        project_glob: a glob over the project folder names ("*" = every project).

    Returns:
        (files, sizes): `files` is a sorted list of (forward-slash path, is_subagent); `sizes` maps
        each path to its size in bytes at listing time.
    """
    uuid_stem = re.compile(UUIDRX + r"\.jsonl$")
    files = [(p.replace("\\", "/"), False) for p in glob.glob(f"{PROJECTS}/{project_glob}/*.jsonl") if uuid_stem.search(p)]
    files += [(p.replace("\\", "/"), True) for p in glob.glob(f"{PROJECTS}/{project_glob}/*/subagents/*.jsonl")]
    sizes = {p: os.path.getsize(p) for p, _ in files}
    return sorted(files), sizes


def batches(files, sizes):
    """Group the files into consecutive batches of at least BATCH_BYTES (the last one smaller).

    Args:
        files: the (path, is_subagent) list of list_files.
        sizes: path -> bytes.

    Yields:
        Lists of (path, is_subagent).
    """
    current, current_bytes = [], 0
    for path, sub in files:
        current.append((path, sub))
        current_bytes += sizes[path]
        if current_bytes >= BATCH_BYTES:
            yield current
            current, current_bytes = [], 0
    if current:
        yield current


def ingest_batch(con, batch) -> None:
    """Read one batch with DuckDB's JSON reader and append its lines, blocks and paths.

    Lines that are not valid JSON (a partial last line of a transcript being written) are skipped by
    the reader (`ignore_errors`), as the app's own reader leaves a partial line for the next pass.

    Args:
        con: the open database.
        batch: a list of (path, is_subagent) from batches().

    Raises:
        duckdb.Error: the batch could not be read or inserted (out of memory, a file vanished).
    """
    files = "[" + ",".join("'" + p.replace("'", "''") + "'" for p, _ in batch) + "]"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE b AS
        SELECT filename, json AS j, regexp_matches(filename, '/subagents/') AS sub, strlen(json::VARCHAR) AS line_bytes
        FROM read_ndjson_objects({files}, filename = true, ignore_errors = true, maximum_object_size = {MAX_OBJECT})""")
    con.execute(BATCH_LINES.format(uuid=UUIDRX))
    con.execute("DROP TABLE b")
    con.execute(INSERT_META)
    con.execute(INSERT_DOC)
    con.execute(INSERT_PATH)
    con.execute("DROP TABLE bl")


def main() -> None:
    """Ingest everything the arguments select, then print the counts and the file size.

    Raises:
        SystemExit: no database path was given.
        duckdb.Error: a batch failed (see ingest_batch).
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    db = sys.argv[1]
    project_glob = sys.argv[2] if len(sys.argv) > 2 else "*"
    files, sizes = list_files(project_glob)
    print(f"{len(files)} files, {sum(sizes.values()) / 1e9:.2f} GB", flush=True)
    bc.remove_db(db)
    con = bc.connect(db)
    con.execute(SCHEMA)
    started = time.time()
    for n, batch in enumerate(batches(files, sizes)):
        t = time.time()
        ingest_batch(con, batch)
        mb = sum(sizes[p] for p, _ in batch) / 1e6
        print(f"batch {n:3d}: {len(batch):4d} files {mb:7.1f} MB in {time.time() - t:6.1f} s", flush=True)
    t = time.time()
    con.execute(SESSION_FACTS)
    print(f"session facts in {time.time() - t:.1f} s")
    con.execute("CHECKPOINT")
    print(f"TOTAL ingest {time.time() - started:.1f} s")
    for table in ("line_meta", "doc", "session", "path"):
        print(table, con.execute(f"select count(*) from {table}").fetchone()[0])
    print("db size MB:", round(os.path.getsize(db) / 1e6, 1))


if __name__ == "__main__":
    main()
