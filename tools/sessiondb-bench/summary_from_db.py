# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The side panel's session summary, computed from the database (SESSIONS_DB.md section 8.5).

The summary box today (AgentTabOverlay's summary panel, also the side panel's bottom area, the tab
tooltip and the Sessions detail) comes from AnalyzeSessionTranscript: one whole-file pass over the
transcript, re-run whenever the file grows. This script computes the same FACTS from the database
written by raw_lines.py (every transcript line stored verbatim), two ways:

  A. FROM RAW LINES: parse one session's stored lines as JSON in SQL and derive every section — what a
     summary costs with nothing precomputed;
  B. FROM PRECOMPUTED TABLES: the per-session section tables built for EVERY session in the database
     (in ~256 MB batches, as an ingest would), then a summary is a handful of small reads, or ONE read
     of a precomputed summary row — what the service would do per request.

It follows AnalyzeSessionTranscript's rules (ProcessInspect.Content.cpp): a line whose uuid appeared
earlier in the file is a re-written copy and is skipped; the CURRENT segment's prompts and every other
field come from the live branch only (ActiveBranchUuids: the lines a double-ESC rewind abandoned are
dropped), the earlier /compact segments' prompts from every line (CollectConversationSegments); prompts
are external user lines (string content or the first text block) and human queued_command attachments,
slash commands rebuilt as "/name args", leading whitespace trimmed, SeIsCommandNoise's markers dropped,
deduplicated within each segment; files are display labels (basename, or scratchpad/<basename>), a Write
is "created" when its tool_result says "File created successfully at:", an Edit (or an overwriting
Write) before the label's first creation still lists it as edited, Read lists only files not also
created or edited; tasks come from the last TodoWrite; the plan-restart parent from the first external
user line.

The live branch is a GRAPH WALK (parent edges, compaction crossings, the stale-marker extension). SQL
is the wrong tool for it (one recursive step per line of the chain), so this script runs a Python port
of ActiveBranchUuids and hands SQL the result as a table; the service would run the C++ original at
ingest and store an `on_branch` flag per line. `--verify` diffs every session's facts against the real
analyzer's (a TSV written by a scratch C++ build of AnalyzeSessionTranscript; SESSIONS_DB.md section 14
has the recipe).

Usage:
  python summary_from_db.py <db written by raw_lines.py> <session id prefix> [more prefixes...]
  python summary_from_db.py <db> --verify <facts.tsv> [session id prefixes...]
"""

import csv
import itertools
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

# The envelope fields every section needs, read with ONE JSON parse per line (from_json). Sub-objects
# whose shape varies (message.content: a string OR a block array; planContent; origin) stay JSON and are
# parsed again only where a section needs them.
# ⚠ Never use DuckDB's arrow chains here: 2.0 parses `j->'a'->>'b' = 'x'` as `j -> (('a'->>'b') = 'x')`
# (the lambda-arrow precedence), which fails to bind or, worse, silently reads the wrong thing.
ENVELOPE = ('{"type":"VARCHAR","subtype":"VARCHAR","uuid":"VARCHAR","timestamp":"VARCHAR","gitBranch":"VARCHAR",'
            '"userType":"VARCHAR","isMeta":"BOOLEAN","isSidechain":"BOOLEAN","isCompactSummary":"BOOLEAN",'
            '"planContent":"JSON","content":"JSON","message":{"content":"JSON"},'
            '"attachment":{"type":"VARCHAR","commandMode":"VARCHAR","isMeta":"BOOLEAN","origin":"JSON","prompt":"JSON"}}')

# The fields the live-branch walk reads (ActiveBranchUuids): every node's upward edges and the markers.
BRANCH_FIELDS = ('{"uuid":"VARCHAR","parentUuid":"VARCHAR","leafUuid":"VARCHAR","logicalParentUuid":"VARCHAR",'
                 '"subtype":"VARCHAR","isSidechain":"BOOLEAN",'
                 '"compactMetadata":{"preservedMessages":{"uuids":["VARCHAR"],"allUuids":["VARCHAR"]},'
                 '"preservedSegment":{"headUuid":"VARCHAR","tailUuid":"VARCHAR"}}}')

BRANCH_SQL = f"""
SELECT file_id, e['uuid'], e['parentUuid'], e['leafUuid'], e['logicalParentUuid'], e['subtype'], coalesce(e['isSidechain'], false),
       e['compactMetadata']['preservedMessages']['uuids'], e['compactMetadata']['preservedMessages']['allUuids'],
       e['compactMetadata']['preservedSegment']['headUuid'], e['compactMetadata']['preservedSegment']['tailUuid']
FROM (SELECT l.file_id, l.line_no, try(from_json(l.raw, '{BRANCH_FIELDS}')) AS e FROM line l WHERE {{where}} AND l.raw <> '')
WHERE e IS NOT NULL
ORDER BY file_id, line_no"""

# The analyzer's whitespace set (space, tab, CR, LF), for trim / ltrim.
WS = "' ' || chr(9) || chr(13) || chr(10)"

MACROS = [
    # The text of a content value the way the analyzer reads a prompt: a string as is, an array's FIRST
    # text block (a tool_result-only user line has none, so it is no prompt).
    """CREATE OR REPLACE TEMP MACRO first_text(c) AS CASE json_type(c)
         WHEN 'VARCHAR' THEN json_extract_string(c, '$')
         WHEN 'ARRAY' THEN list_filter(from_json(c, '[{"type":"VARCHAR","text":"VARCHAR"}]'), lambda b: b.type = 'text')[1].text
       END""",
    # A tool_result's whole text: a string, or every text block joined (the created-vs-edited test reads it).
    """CREATE OR REPLACE TEMP MACRO all_text(c) AS CASE json_type(c)
         WHEN 'VARCHAR' THEN json_extract_string(c, '$')
         WHEN 'ARRAY' THEN array_to_string(list_transform(list_filter(from_json(c, '[{"type":"VARCHAR","text":"VARCHAR"}]'), lambda b: b.type = 'text'), lambda b: b.text), '')
       END""",
    # SeIsCommandNoise, marker for marker: a pasted TUI bullet, command / bash / task wrappers,
    # background-task results, telemetry, reminders, three leading phrases, and teammate PROTOCOL — only
    # when the FIRST <teammate-message> tag's payload is a JSON envelope (a prose report is a real
    # message, even when a later tag in the same text is a protocol notice). regexp_extract answers the
    # LEFTMOST tag, which is the analyzer's "first occurrence".
    r"""CREATE OR REPLACE TEMP MACRO is_noise(t) AS
         left(t, 1) IN (chr(9679), chr(9210), chr(9151))
         OR contains(t, '<command-message>') OR contains(t, '<command-name>') OR contains(t, '<command-args>') OR contains(t, '<local-command-')
         OR contains(t, '<bash-input>') OR contains(t, '<bash-stdout>') OR contains(t, '<bash-stderr>')
         OR (contains(t, '<teammate-message') AND regexp_extract(t, '(?s)<teammate-message[^>]*>\s*(.{0,9})', 1) = '{"type":"')
         OR contains(t, '<task-notification>') OR contains(t, '<output-file>') OR (contains(t, '<status>') AND contains(t, '<summary>'))
         OR contains(t, '<usage>') OR contains(t, '<subagent_tokens>')
         OR contains(t, '<bash-notification>') OR contains(t, '<shell-id>') OR contains(t, '<persisted-output>')
         OR contains(t, '<background-task-input>') OR contains(t, '<system-reminder>')
         OR starts_with(t, 'Caveat:') OR starts_with(t, 'Overview:') OR starts_with(t, '[Request interrupted')""",
    # SeSummaryFileLabel: the basename, or scratchpad/<basename> for a file in a session scratchpad
    # (a `scratchpad` component exactly three below a `claude` one, with a file beneath it).
    r"""CREATE OR REPLACE TEMP MACRO file_label(p) AS
         CASE WHEN regexp_matches(p, '(?i)(?:^|[/\\])claude[/\\]+[^/\\]+[/\\]+[^/\\]+[/\\]+scratchpad[/\\]+[^/\\]')
              THEN 'scratchpad/' || regexp_extract(p, '([^/\\]*)$', 1)
              ELSE regexp_extract(p, '([^/\\]*)$', 1) END""",
    # SeIsPlansPath: a /plans/ segment, separators and case folded.
    r"""CREATE OR REPLACE TEMP MACRO is_plans_path(p) AS contains(lower(replace(p, '\', '/')), '/plans/')""",
    # NormalizeRecapText: trailing whitespace, the "(disable recaps in /config)" hint, then both ends.
    rf"""CREATE OR REPLACE TEMP MACRO recap_norm(c) AS
         trim(regexp_replace(rtrim(c, {WS}), '\(disable recaps in /config\)$', ''), {WS})""",
]

# Parse a batch of files' lines once: the envelope as flat columns, a re-written copy (a uuid seen
# earlier in the same file) dropped, each line's /compact segment number (how many boundaries precede
# it, the boundary itself included — the analyzer's file-order rule, over every line), and whether the
# line is on the live branch (no walk result for the file = no leaf marker = every line is).
PARSE = f"""
CREATE OR REPLACE TEMP TABLE sl AS
SELECT x.*,
       CAST(sum(CASE WHEN ltype = 'system' AND subtype = 'compact_boundary' THEN 1 ELSE 0 END)
            OVER (PARTITION BY x.file_id ORDER BY line_no ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS INTEGER) AS seg,
       bf.file_id IS NULL OR coalesce(x.uuid, '') = '' OR b.uuid IS NOT NULL AS on_branch
FROM (
    SELECT file_id, line_no,
           e['type'] AS ltype, e['subtype'] AS subtype, e['uuid'] AS uuid, e['timestamp'] AS ts,
           e['gitBranch'] AS git_branch, e['userType'] AS user_type,
           coalesce(e['isMeta'], false) AS is_meta, coalesce(e['isSidechain'], false) AS is_sidechain,
           coalesce(e['isCompactSummary'], false) AS is_compact_summary,
           e['planContent'] AS plan_content, e['content'] AS sys_content,
           e['message']['content'] AS msg_content, e['attachment'] AS att
    FROM (SELECT l.file_id, l.line_no, try(from_json(l.raw, '{ENVELOPE}')) AS e
          FROM line l WHERE {{where}} AND l.raw <> '')
    WHERE e IS NOT NULL
    QUALIFY coalesce(e['uuid'], '') = '' OR row_number() OVER (PARTITION BY file_id, e['uuid'] ORDER BY line_no) = 1
) x
LEFT JOIN branch_files bf ON bf.file_id = x.file_id
LEFT JOIN branch b ON b.file_id = x.file_id AND b.uuid = x.uuid"""

RESULT_TABLES = """
CREATE OR REPLACE TEMP TABLE branch (file_id INTEGER, uuid VARCHAR);
CREATE OR REPLACE TEMP TABLE branch_files (file_id INTEGER);
CREATE OR REPLACE TEMP TABLE r_prompt (file_id INTEGER, line_no INTEGER, seg INTEGER, uuid VARCHAR, ts VARCHAR, kind VARCHAR, text VARCHAR,
                                       on_branch BOOLEAN, listed BOOLEAN);
CREATE OR REPLACE TEMP TABLE r_tool (file_id INTEGER, line_no INTEGER, id VARCHAR, name VARCHAR, path VARCHAR, skill VARCHAR, todos JSON);
CREATE OR REPLACE TEMP TABLE r_result (file_id INTEGER, line_no INTEGER, ts VARCHAR, id VARCHAR, created BOOLEAN);
CREATE OR REPLACE TEMP TABLE r_fact (file_id INTEGER, first_ts VARCHAR, last_ts VARCHAR, branch VARCHAR, recap VARCHAR,
                                     compactions INTEGER, plan_content BOOLEAN, parent VARCHAR);
"""

SECTIONS = {
    # Typed prompts (external user lines, not meta / compact summary) and messages typed while claude
    # worked (human queued_command attachments), normalized like SeNormalizeHumanPromptText. `listed`
    # is what the summary numbers: in the CURRENT segment (after the last boundary) the live-branch
    # prompts, deduplicated among themselves (AnalyzeSessionTranscript's main walk); in an EARLIER
    # segment every prompt, deduplicated within the segment (CollectConversationSegments, by position).
    # Off-branch and duplicate rows are kept: "last user message" counts a repeated prompt too.
    "prompts": rf"""
        INSERT INTO r_prompt
        WITH cand AS (
            SELECT file_id, line_no, seg, uuid, ts, on_branch, 'typed' AS kind, first_text(msg_content) AS raw_text
            FROM sl WHERE ltype = 'user' AND user_type = 'external' AND NOT is_compact_summary AND NOT is_meta
            UNION ALL
            SELECT file_id, line_no, seg, uuid, ts, on_branch, 'queued', first_text(att['prompt'])
            FROM sl WHERE ltype = 'attachment' AND NOT is_meta AND NOT is_sidechain AND coalesce(user_type, 'external') = 'external'
              AND att['type'] = 'queued_command' AND att['commandMode'] = 'prompt' AND NOT coalesce(att['isMeta'], false)
              AND (att['origin'] IS NULL OR json_type(att['origin']) = 'NULL' OR json_extract_string(att['origin'], '$.kind') = 'human')
        ), cmd AS (
            SELECT *,
                   CASE WHEN contains(raw_text, '<command-name>') THEN trim(regexp_extract(raw_text, '(?s)<command-name>(.*?)</command-name>', 1), {WS}) END AS cmd_name,
                   CASE WHEN contains(raw_text, '<command-name>') THEN trim(regexp_extract(raw_text, '(?s)<command-args>(.*?)</command-args>', 1), {WS}) END AS cmd_args
            FROM cand WHERE raw_text IS NOT NULL
        ), normed AS (
            SELECT *, ltrim(CASE WHEN coalesce(cmd_name, '') <> ''
                                 THEN CASE WHEN coalesce(cmd_args, '') = '' THEN cmd_name ELSE cmd_name || ' ' || cmd_args END
                                 ELSE raw_text END, {WS}) AS text
            FROM cmd
        ), kept AS (
            SELECT n.*, last.last_seg,
                   row_number() OVER (PARTITION BY n.file_id, seg, text ORDER BY line_no) AS nth_all,
                   row_number() OVER (PARTITION BY n.file_id, seg, text, on_branch ORDER BY line_no) AS nth_branch
            FROM normed n JOIN (SELECT file_id, max(seg) AS last_seg FROM sl GROUP BY file_id) last ON last.file_id = n.file_id
            WHERE text <> '' AND NOT is_noise(text)
        )
        SELECT file_id, line_no, seg, uuid, ts, kind, text, on_branch,
               (seg = last_seg AND on_branch AND nth_branch = 1) OR (seg < last_seg AND nth_all = 1)
        FROM kept""",
    # Every tool_use block of the live-branch assistant lines (the summary reads Read / Edit / Write /
    # Skill / TodoWrite / ExitPlanMode / AskUserQuestion; the other tools cost nothing more to keep).
    "tool uses": """
        INSERT INTO r_tool
        SELECT file_id, line_no, b.id, b.name, b.input.file_path, b.input.skill, b.input.todos
        FROM (SELECT file_id, line_no,
                     unnest(from_json(msg_content, '[{"type":"VARCHAR","id":"VARCHAR","name":"VARCHAR","input":{"file_path":"VARCHAR","skill":"VARCHAR","todos":"JSON"}}]')) AS b
              FROM sl WHERE on_branch AND ltype = 'assistant' AND json_type(msg_content) = 'ARRAY')
        WHERE b.type = 'tool_use'""",
    # The live-branch tool_results the summary needs: a Write's (created or overwrote?) and an
    # AskUserQuestion's (answering one moves "last user message"). Only those are text-scanned.
    "tool results": """
        INSERT INTO r_result
        SELECT r.file_id, r.line_no, r.ts, r.id,
               CASE WHEN t.name = 'Write' THEN contains(coalesce(all_text(r.content), ''), 'File created successfully at:') END
        FROM (SELECT file_id, line_no, ts, b.tool_use_id AS id, b.content AS content
              FROM (SELECT file_id, line_no, ts,
                           unnest(from_json(msg_content, '[{"type":"VARCHAR","tool_use_id":"VARCHAR","content":"JSON"}]')) AS b
                    FROM sl WHERE on_branch AND ltype = 'user' AND json_type(msg_content) = 'ARRAY')
              WHERE b.type = 'tool_result') r
        JOIN (SELECT DISTINCT file_id, id, name FROM r_tool
              WHERE name IN ('Write', 'AskUserQuestion') AND file_id IN (SELECT DISTINCT file_id FROM sl)) t
          ON t.file_id = r.file_id AND t.id = r.id""",
    # Per-session facts, from live-branch lines: first / last timestamp (file order), the first branch,
    # the last non-empty recap, and two reads of the TRUE first external user line — its planContent
    # (plan-start signal) and the "read the full transcript at: <parent>.jsonl" link. The compactions
    # count every line (the segment rule is positional). arg_min_null, not arg_min: arg_min skips a row
    # whose value is NULL and would answer the NEXT line.
    "facts": r"""
        INSERT INTO r_fact
        WITH f AS (
            SELECT file_id,
                   arg_min(ts, line_no) FILTER (WHERE on_branch AND coalesce(ts, '') <> '') AS first_ts,
                   arg_max(ts, line_no) FILTER (WHERE on_branch AND coalesce(ts, '') <> '') AS last_ts,
                   arg_min(git_branch, line_no) FILTER (WHERE on_branch AND coalesce(git_branch, '') <> '') AS branch,
                   arg_max(recap_norm(json_extract_string(sys_content, '$')), line_no)
                       FILTER (WHERE on_branch AND ltype = 'system' AND subtype = 'away_summary' AND recap_norm(json_extract_string(sys_content, '$')) <> '') AS recap,
                   count(*) FILTER (WHERE ltype = 'system' AND subtype = 'compact_boundary') AS compactions,
                   arg_min_null(plan_content, line_no) FILTER (WHERE on_branch AND ltype = 'user' AND user_type = 'external') AS first_plan,
                   arg_min_null(CASE WHEN json_type(msg_content) = 'VARCHAR' THEN json_extract_string(msg_content, '$') END, line_no)
                       FILTER (WHERE on_branch AND ltype = 'user' AND user_type = 'external') AS first_text
            FROM sl GROUP BY file_id)
        SELECT file_id, first_ts, last_ts, branch, recap, compactions,
               coalesce(first_plan IS NOT NULL AND json_type(first_plan) <> 'NULL'
                        AND NOT (json_type(first_plan) = 'BOOLEAN' AND CAST(first_plan AS VARCHAR) = 'false')
                        AND NOT (json_type(first_plan) = 'VARCHAR' AND json_extract_string(first_plan, '$') = ''), false),
               nullif(regexp_extract(regexp_extract(first_text, '(?i)read the full transcript at:[ \t]*([^ \t\r\n]+)', 1), '(?i)([^/\\]+)\.jsonl$', 1), '')
        FROM f""",
}


def files_ctes(where: str) -> str:
    """The three file lists' rules as CTEs, shared by the per-session read and the summary-row build.

    The analyzer's rules: a Write is created when its result says so, else (or with no result yet)
    edited; an Edit, or an overwriting Write, lists its label as edited when it happened BEFORE the
    label's first creation (the analyzer walks in file order and never removes an earlier "edited");
    Read keeps only labels neither created nor edited.

    Args:
        where: a condition over r_tool choosing the sessions ("file_id = ?" or "true").

    Returns:
        "t, w, created, edit_events, edited, readonly" CTE text (no leading WITH).
    """
    return f"""
     t AS (SELECT file_id, name, id, line_no, file_label(path) AS label FROM r_tool
           WHERE {where} AND name IN ('Read', 'Edit', 'Write') AND coalesce(path, '') <> ''),
     w AS (SELECT t.file_id, t.label, r.line_no AS result_line, coalesce(r.created, false) AS created
           FROM t LEFT JOIN r_result r ON r.file_id = t.file_id AND r.id = t.id WHERE t.name = 'Write'),
     created AS (SELECT file_id, label, min(result_line) AS at FROM w WHERE created GROUP BY file_id, label),
     edit_events AS (SELECT file_id, label, line_no AS at FROM t WHERE name = 'Edit'
                     UNION ALL SELECT file_id, label, coalesce(result_line, 2147483647) FROM w WHERE NOT created),
     edited AS (SELECT DISTINCT e.file_id, e.label FROM edit_events e
                LEFT JOIN created c ON c.file_id = e.file_id AND c.label = e.label
                WHERE c.at IS NULL OR e.at < c.at),
     readonly AS (SELECT DISTINCT r.file_id, r.label FROM (SELECT file_id, label FROM t WHERE name = 'Read') r
                  ANTI JOIN created c ON c.file_id = r.file_id AND c.label = r.label
                  ANTI JOIN edited e ON e.file_id = r.file_id AND e.label = r.label)"""


# One session's three file lists.
FILES_SQL = "WITH" + files_ctes("file_id = ?") + """
SELECT 'created', label FROM created UNION ALL SELECT 'edited', label FROM edited UNION ALL SELECT 'read', label FROM readonly
"""

# The whole summary of EVERY session as one row each (lists for the prompts and the three file lists),
# built from the section tables by the same rules as FILES_SQL and summary() — the precomputed form a
# request reads with ONE point query.
SUMMARY_ROWS_SQL = "CREATE OR REPLACE TEMP TABLE r_summary AS WITH" + files_ctes("true") + """,
     files AS (SELECT file_id, list(label ORDER BY label) FILTER (WHERE kind = 'created') AS created,
                      list(label ORDER BY label) FILTER (WHERE kind = 'edited') AS edited,
                      list(label ORDER BY label) FILTER (WHERE kind = 'read') AS read
               FROM (SELECT file_id, 'created' AS kind, label FROM created UNION ALL SELECT file_id, 'edited', label FROM edited
                     UNION ALL SELECT file_id, 'read', label FROM readonly) GROUP BY file_id),
     p AS (SELECT file_id, list({'seg': seg, 'kind': kind, 'text': text} ORDER BY line_no) FILTER (WHERE listed) AS prompts,
                  arg_max(ts, line_no) FILTER (WHERE kind = 'typed' AND on_branch) AS last_typed_ts,
                  max(ts) FILTER (WHERE kind = 'queued' AND on_branch) AS last_queued_ts
           FROM r_prompt GROUP BY file_id),
     tl AS (SELECT file_id,
                   list(DISTINCT skill ORDER BY skill) FILTER (WHERE name = 'Skill' AND coalesce(skill, '') <> '') AS skills,
                   arg_max(todos, line_no) FILTER (WHERE name = 'TodoWrite' AND json_type(todos) = 'ARRAY') AS last_todos,
                   coalesce(bool_or(name = 'ExitPlanMode'), false) AS exit_plan,
                   arg_max(path, line_no) FILTER (WHERE name = 'Write' AND is_plans_path(path)) AS plan_file,
                   count(*) FILTER (WHERE name = 'Read' AND is_plans_path(path)) AS plan_reads
            FROM r_tool GROUP BY file_id),
     a AS (SELECT r.file_id, arg_max(r.ts, r.line_no) AS last_answer_ts
           FROM r_result r JOIN r_tool t ON t.file_id = r.file_id AND t.id = r.id AND t.name = 'AskUserQuestion'
           GROUP BY r.file_id)
SELECT f.file_id, f.first_ts, f.last_ts, f.branch, f.recap, f.compactions, f.plan_content, f.parent,
       p.prompts, p.last_typed_ts, p.last_queued_ts, files.created, files.edited, files.read, tl.skills,
       len(list_filter(from_json(tl.last_todos, '[{"status":"VARCHAR"}]'), lambda x: x.status = 'completed')) AS tasks_done,
       len(list_filter(from_json(tl.last_todos, '[{"status":"VARCHAR"}]'), lambda x: x.status IN ('pending', 'in_progress'))) AS tasks_open,
       tl.exit_plan, tl.plan_file, tl.plan_reads, a.last_answer_ts
FROM r_fact f LEFT JOIN p USING (file_id) LEFT JOIN files USING (file_id) LEFT JOIN tl USING (file_id) LEFT JOIN a USING (file_id)
ORDER BY f.file_id
"""


def active_branch(rows):
    """A Python port of TranscriptStore::ActiveBranchUuids over one file's lines (file order).

    Every uuid line is a node; its upward edges are its parentUuid (every occurrence), and for a
    compaction root (a null parent on a compact_boundary or on a line with a logicalParentUuid) the
    logical parent, the preserved messages and the pre-boundary leaf. The walk starts at the last
    leafUuid marker, or at the physical tail when the tail descends from it (the stale-marker
    extension), and collects every node reachable upward.

    Args:
        rows: (file_id, uuid, parentUuid, leafUuid, logicalParentUuid, subtype, isSidechain,
               preserved uuids, preserved allUuids, preservedSegment head, preservedSegment tail) tuples.

    Returns:
        The set of live-branch uuids, or None when the file has no usable leaf marker (the analyzer then
        keeps every line).
    """
    up = {}

    def add(node, target):
        # No edge for a true root's null parent or a malformed self-reference; a verbatim re-append
        # repeats the same edge, which is kept once.
        if not target or target == node:
            return
        edges = up.setdefault(node, [])
        if target not in edges:
            edges.append(target)

    pre = {}
    leaf = tail = main_tail = ""
    after_marker = False
    for _fid, u, parent, lu, lp, subtype, side, pm_uuids, pm_all, ps_head, ps_tail in rows:
        if lu:
            leaf, after_marker = lu, False
        if not u:
            continue
        first = u not in up
        up.setdefault(u, [])
        add(u, parent or "")
        if not parent and (lp or subtype == "compact_boundary"):
            add(u, lp or "")
            for item in (pm_uuids or []) + (pm_all or []):
                add(u, item or "")
            add(u, ps_head or "")
            add(u, ps_tail or "")
            if first:
                pre[u] = (leaf, main_tail, after_marker)
        tail = u
        if not side:
            main_tail = u
        after_marker = True
    for root, (marker_leaf, tail_before, node_after) in pre.items():
        tail_known = bool(tail_before) and tail_before in up
        marker_known = bool(marker_leaf) and marker_leaf in up
        if node_after and tail_known:
            add(root, tail_before)
        elif marker_known:
            add(root, marker_leaf)
        elif tail_known:
            add(root, tail_before)
    if not leaf or leaf not in up:
        return None
    anchor = leaf
    if after_marker and tail and tail != leaf:
        seen, stack = set(), [tail]
        while stack:
            c = stack.pop()
            if not c or c in seen:
                continue
            seen.add(c)
            if c == leaf:
                anchor = tail  # the tail descends from the marked leaf: it is the live tip
                break
            stack.extend(up.get(c, []))
    active, stack = set(), [anchor]
    while stack:
        c = stack.pop()
        if not c or c in active:
            continue
        active.add(c)
        stack.extend(up.get(c, []))
    return active


def load_branch(con, where: str) -> int:
    """Walk the live branch of the chosen files and load the result into `branch` / `branch_files`.

    Args:
        con: the open database (create_helpers has run).
        where: a SQL condition over `line l` choosing the files.

    Returns:
        How many of the files have a live branch (a usable leaf marker).

    Raises:
        duckdb.Error: a statement fails.
        OSError: the scratch CSV cannot be written.
    """
    rows = con.execute(BRANCH_SQL.replace("{where}", where)).fetchall()
    known, members = [], []
    for file_id, group in itertools.groupby(rows, key=lambda r: r[0]):
        active = active_branch(group)
        if active is not None:
            known.append(file_id)
            members.extend((file_id, u) for u in active)
    # A CSV + COPY, not executemany: a batch can hold a million uuids.
    path = bc.work_path("branch.csv")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        csv.writer(handle).writerows(members)
    if members:
        con.execute(f"COPY branch FROM '{path}' (HEADER false)")
    if known:
        con.execute(f"INSERT INTO branch_files VALUES {','.join(f'({f})' for f in known)}")
    os.remove(path)
    return len(known)


def create_helpers(con) -> None:
    """Create the macros and the (empty) result tables.

    Args:
        con: the open database.

    Raises:
        duckdb.Error: a statement fails (an older DuckDB without lambda / arg_min_null / try).
    """
    for macro in MACROS:
        con.execute(macro)
    con.execute(RESULT_TABLES)


def build_sections(con, where: str) -> dict:
    """Walk the live branch, parse the chosen files' lines and append their sections; time each step.

    Args:
        con: the open database (create_helpers has run).
        where: a SQL condition over `line l` choosing the files.

    Returns:
        A dict step name -> milliseconds.

    Raises:
        duckdb.Error: a statement fails.
    """
    times = {}
    start = time.perf_counter()
    load_branch(con, where)
    times["branch walk"] = (time.perf_counter() - start) * 1000
    start = time.perf_counter()
    # replace, not str.format: the envelope's JSON braces are in the text and would read as fields.
    con.execute(PARSE.replace("{where}", where))
    times["parse"] = (time.perf_counter() - start) * 1000
    for name, sql in SECTIONS.items():
        start = time.perf_counter()
        con.execute(sql)
        times[name] = (time.perf_counter() - start) * 1000
    return times


def derive(fact: tuple, listed: list, typed_last: str, queued_last: str, files: dict, skills: list, tasks: tuple,
           plan: tuple, answer_last: str) -> dict:
    """Turn a session's summary parts into the analyzer's facts (the TSV's column names) — one rule for both read paths.

    Args:
        fact: (first_ts, last_ts, branch, recap, compactions, plan_content, parent).
        listed: the prompts the summary numbers, as (seg, kind, text), in file order.
        typed_last: the timestamp of the last live-branch typed prompt (duplicates included), or "".
        queued_last: the latest timestamp of a live-branch message typed while claude worked, or "".
        files: {"created": [...], "edited": [...], "read": [...]} display labels.
        skills: the skills loaded.
        tasks: (done, open) of the last TodoWrite.
        plan: (exit_plan, plan_file, plan_reads).
        answer_last: the timestamp of the last AskUserQuestion answer, or "".

    Returns:
        The facts dict.
    """
    # The analyzer's Messages list is the CURRENT segment (after the last boundary); earlier segments
    # are listed separately, and an empty one is dropped.
    current = fact[4] or 0
    # "last user message": the last typed prompt or AskUserQuestion answer, or a later queued message
    # (ISO-8601 UTC strings order as times).
    last_user = max(typed_last or "", answer_last or "", queued_last or "")
    return {
        "prompts_current": sum(1 for p in listed if p[0] == current),
        "prompts_all": len(listed),
        "earlier_segments": len({p[0] for p in listed if p[0] < current}),
        "read": len(files.get("read") or []), "created": len(files.get("created") or []), "edited": len(files.get("edited") or []),
        "skills": len(skills or []), "tasks_done": tasks[0] or 0, "tasks_open": tasks[1] or 0,
        "compacted": 1 if current > 0 else 0, "recap_chars": len(fact[3] or ""),
        "first_ts": fact[0] or "", "last_ts": fact[1] or "", "last_user_ts": last_user, "branch": fact[2] or "",
        "parent": fact[6] or "", "exit_plan": 1 if plan[0] else 0, "plan_content": 1 if fact[5] else 0,
        "plan_file": plan[1] or "", "plan_reads": plan[2] or 0,
        "first prompts": [p[2][:60] for p in listed[:3]],
    }


def summary(con, file_id: int) -> dict:
    """The summary's reads for one session over the section tables — the lists the panel shows, not just counts.

    Args:
        con: the open database (the session's sections are built).
        file_id: the session's top-level file.

    Returns:
        derive()'s facts plus "ms", the time the reads took.

    Raises:
        duckdb.Error: a statement fails.
    """
    start = time.perf_counter()
    fact = con.execute("select first_ts, last_ts, branch, recap, compactions, plan_content, parent from r_fact where file_id = ?",
                       [file_id]).fetchone() or (None, None, None, None, 0, False, None)
    prompts = con.execute("select seg, kind, text, ts, on_branch, listed from r_prompt where file_id = ? order by line_no", [file_id]).fetchall()
    files = con.execute(FILES_SQL, [file_id]).fetchall()
    skills = con.execute("select list(distinct skill order by skill) from r_tool where file_id = ? and name = 'Skill' and coalesce(skill, '') <> ''",
                         [file_id]).fetchone()[0] or []
    todos = con.execute("""select len(list_filter(td, lambda t: t.status = 'completed')),
                                  len(list_filter(td, lambda t: t.status in ('pending', 'in_progress')))
                           from (select from_json(todos, '[{"status":"VARCHAR"}]') as td from r_tool
                                 where file_id = ? and name = 'TodoWrite' and json_type(todos) = 'ARRAY' order by line_no desc limit 1)""",
                        [file_id]).fetchone() or (0, 0)
    plan = con.execute("""select coalesce(bool_or(name = 'ExitPlanMode'), false),
                                 arg_max(path, line_no) filter (where name = 'Write' and is_plans_path(path)),
                                 count(*) filter (where name = 'Read' and is_plans_path(path))
                          from r_tool where file_id = ?""", [file_id]).fetchone()
    answer = con.execute("""select arg_max(r.ts, r.line_no) from r_result r
                            join r_tool t on t.file_id = r.file_id and t.id = r.id and t.name = 'AskUserQuestion'
                            where r.file_id = ?""", [file_id]).fetchone()[0]
    elapsed = (time.perf_counter() - start) * 1000
    lists = {"created": [], "edited": [], "read": []}
    for kind, label in files:
        lists[kind].append(label)
    typed = [p[3] or "" for p in prompts if p[1] == "typed" and p[4]]
    queued = [p[3] or "" for p in prompts if p[1] == "queued" and p[4]]
    out = derive(fact, [(p[0], p[1], p[2]) for p in prompts if p[5]], typed[-1] if typed else "",
                 max(queued, default=""), lists, skills, todos, plan, answer or "")
    out["ms"] = elapsed
    return out


def summary_row(con, file_id: int) -> dict:
    """The same summary from the precomputed one-row-per-session table: ONE point query.

    Args:
        con: the open database (build_summary_rows has run).
        file_id: the session's top-level file.

    Returns:
        derive()'s facts plus "ms", the time the read took.

    Raises:
        duckdb.Error: the query fails.
    """
    start = time.perf_counter()
    row = con.execute("select * from r_summary where file_id = ?", [file_id]).fetchone()
    elapsed = (time.perf_counter() - start) * 1000
    names = [d[0] for d in con.description]
    r = dict(zip(names, row)) if row else {}
    fact = (r.get("first_ts"), r.get("last_ts"), r.get("branch"), r.get("recap"), r.get("compactions"), r.get("plan_content"), r.get("parent"))
    listed = [(p["seg"], p["kind"], p["text"]) for p in (r.get("prompts") or [])]
    out = derive(fact, listed, r.get("last_typed_ts") or "", r.get("last_queued_ts") or "",
                 {"created": r.get("created"), "edited": r.get("edited"), "read": r.get("read")}, r.get("skills"),
                 (r.get("tasks_done"), r.get("tasks_open")), (r.get("exit_plan"), r.get("plan_file"), r.get("plan_reads")),
                 r.get("last_answer_ts") or "")
    out["ms"] = elapsed
    return out


def resolve(con, prefix: str):
    """Find a top-level transcript by its session id prefix.

    Args:
        con: the open database.
        prefix: the first characters of the session id.

    Returns:
        (file_id, path, content_bytes).

    Raises:
        SystemExit: no top-level transcript starts with the prefix.
    """
    row = con.execute("select file_id, path, content_bytes from file where not sub and path like '%' || ? || '%.jsonl' limit 1", [prefix]).fetchone()
    if not row:
        raise SystemExit(f"no top-level transcript starts with {prefix}")
    return row


def build_all(con) -> tuple:
    """Build the section tables for every top-level session, in ~256 MB batches, then sort them by file.

    Args:
        con: the open database (create_helpers has run).

    Returns:
        (sessions, bytes, seconds, per-step seconds).

    Raises:
        duckdb.Error: a statement fails.
    """
    files = con.execute("select file_id, content_bytes from file where not sub order by file_id").fetchall()
    batches, current, size = [], [], 0
    for file_id, content in files:
        current.append(file_id)
        size += content or 0
        if size >= 256_000_000:
            batches.append(current)
            current, size = [], 0
    if current:
        batches.append(current)
    steps = {}
    start = time.perf_counter()
    for batch in batches:
        for name, ms in build_sections(con, f"l.file_id in ({','.join(map(str, batch))})").items():
            steps[name] = steps.get(name, 0) + ms / 1000
    # Sorted by file, each session's rows are one zonemap-prunable run (SESSIONS_DB.md section 6.8).
    for table in ("r_prompt", "r_tool", "r_result", "r_fact"):
        con.execute(f"CREATE OR REPLACE TEMP TABLE {table}_s AS SELECT * FROM {table} ORDER BY file_id")
        con.execute(f"DROP TABLE {table}")
        con.execute(f"ALTER TABLE {table}_s RENAME TO {table}")
    total = time.perf_counter() - start
    return len(files), sum(c or 0 for _, c in files), total, steps


def build_summary_rows(con) -> float:
    """Build r_summary (one row per session) from the section tables.

    Args:
        con: the open database (build_all has run).

    Returns:
        The seconds it took.

    Raises:
        duckdb.Error: the statement fails.
    """
    start = time.perf_counter()
    con.execute(SUMMARY_ROWS_SQL)
    return time.perf_counter() - start


def verify(con, tsv_path: str) -> None:
    """Diff every session's facts, by BOTH read paths, against the real analyzer's TSV.

    Only sessions whose transcript did not change since the ingest are compared (the TSV carries the
    file's size when the analyzer read it).

    Args:
        con: the open database (build_all and build_summary_rows have run).
        tsv_path: the analyzer's TSV (bench_summary.exe --facts).

    Raises:
        OSError: the TSV cannot be read.
    """
    with open(tsv_path, encoding="utf-8", newline="") as handle:
        truth = {row["stem"]: row for row in csv.DictReader(handle, delimiter="\t")}
    files = con.execute("select file_id, path, content_bytes from file where not sub").fetchall()
    fields = ["prompts_current", "prompts_all", "earlier_segments", "read", "created", "edited", "skills", "tasks_done",
              "tasks_open", "compacted", "recap_chars", "first_ts", "last_ts", "last_user_ts", "branch", "parent",
              "exit_plan", "plan_content", "plan_file", "plan_reads"]
    for label, reader in (("section tables (7 queries)", summary), ("one summary row (1 query)", summary_row)):
        agree = {f: 0 for f in fields}
        examples = {f: [] for f in fields}
        compared = changed = exact = 0
        for file_id, path, content in files:
            stem = os.path.basename(path)[:-6]
            row = truth.get(stem)
            if row is None:
                continue
            if int(row["size"]) != content:
                changed += 1  # the transcript grew after the ingest: not comparable
                continue
            compared += 1
            mine = reader(con, file_id)
            all_agree = True
            for f in fields:
                if str(mine[f]) == row[f]:
                    agree[f] += 1
                else:
                    all_agree = False
                    if len(examples[f]) < 3:
                        examples[f].append(f"{stem[:8]}: db {mine[f]!r} vs analyzer {row[f]!r}")
            exact += all_agree
        print(f"== verify, {label}: {compared} sessions compared with the analyzer ({changed} grew since the ingest, skipped); "
              f"{exact} agree on every field")
        for f in fields:
            print(f"   {f}: {agree[f]}/{compared} agree" + ("" if not examples[f] else "  e.g. " + "; ".join(examples[f])))


def main() -> None:
    """A per named session (from raw lines), then B (all sessions precomputed; per-session reads), then --verify.

    Raises:
        SystemExit: the arguments are missing, or a prefix matches no top-level file.
        duckdb.Error: the database lacks raw_lines.py's tables.
    """
    bc.utf8_stdout()
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    tsv = None
    if "--verify" in args:
        at = args.index("--verify")
        tsv = args[at + 1]
        del args[at:at + 2]
    con = bc.connect(args[0])
    create_helpers(con)
    targets = [resolve(con, prefix) for prefix in args[1:]]

    print("== A. one session, from its raw lines (nothing precomputed)")
    for file_id, path, size in targets:
        con.execute(RESULT_TABLES)
        steps = build_sections(con, f"l.file_id = {file_id}")
        result = summary(con, file_id)
        lines = con.execute("select count(*) from sl").fetchone()[0]
        total = sum(steps.values()) + result["ms"]
        print(f"   {os.path.basename(path)[:8]} ({size / 1e6:.1f} MB, {lines} lines): {total:.0f} ms = "
              + " + ".join(f"{k} {v:.0f}" for k, v in steps.items()) + f" + reads {result['ms']:.0f}")
        print("      " + ", ".join(f"{k} {v}" for k, v in result.items() if k != "ms"))

    print("== B. the section tables for EVERY session, then per-session reads")
    con.execute(RESULT_TABLES)
    sessions, total_bytes, seconds, steps = build_all(con)
    rows = {t: con.execute(f"select count(*) from {t}").fetchone()[0] for t in ("r_prompt", "r_tool", "r_result", "r_fact", "branch")}
    print(f"   built for {sessions} sessions ({total_bytes / 1e9:.2f} GB of JSONL) in {seconds:.1f} s "
          f"({total_bytes / 1e6 / seconds:.0f} MB/s; " + ", ".join(f"{k} {v:.1f} s" for k, v in steps.items()) + ")")
    print("   rows: " + ", ".join(f"{t} {n}" for t, n in rows.items()))
    summary_s = build_summary_rows(con)
    print(f"   one summary row per session (r_summary) built from them in {summary_s:.2f} s")
    for file_id, path, size in targets:
        best = min(summary(con, file_id)["ms"] for _ in range(5))
        best_row = min(summary_row(con, file_id)["ms"] for _ in range(5))
        print(f"   {os.path.basename(path)[:8]} ({size / 1e6:.1f} MB): the summary's reads {best:.1f} ms over the section tables, "
              f"{best_row:.2f} ms as one row")
    if tsv:
        start = time.perf_counter()
        verify(con, tsv)
        print(f"   (verify read every session's summary twice in {time.perf_counter() - start:.1f} s)")


if __name__ == "__main__":
    main()
