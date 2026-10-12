# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The proposed per-session columns, derived in SQL from the stored lines (SESSIONS_DB.md section 5.6).

Runs on the database written by raw_lines.py. One pass over every top-level session's lines (one JSON
parse per line into a wide envelope) builds a table with one row per session holding the columns the
design proposes that the transcripts themselves carry: places (cwd first / last / count and a
normalized key, relocation, worktree, the environment snapshot), versions, models, effort, entry
point, permission mode, times, turn durations, Claude Code's own cost record, token usage (deduplicated
per API message: one response is written as several lines that repeat its usage), refusals, API
errors, interrupts, tool calls by tool, MCP servers and skills used, subagents spawned, images,
compactions, recaps, titles, artifacts published, Stop-hook time.

It prints the build time, then for every column the share of sessions where it is set (non-null,
non-empty, non-zero), then one session's row. The columns that need the filesystem or Agentmaster's
own state (git roots, the inferred working directory, the registry) are listed in the design, not here.

Usage: python session_columns.py <db written by raw_lines.py> [session id prefix to print]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

# Every field a session column reads, parsed ONCE per line. ⚠ Function forms only, never `->` chains
# (DuckDB 2.0 gives `->` the lambda arrow's precedence; see summary_from_db.py).
ENVELOPE = (
    '{"type":"VARCHAR","subtype":"VARCHAR","timestamp":"VARCHAR","cwd":"VARCHAR","gitBranch":"VARCHAR","version":"VARCHAR",'
    '"entrypoint":"VARCHAR","userType":"VARCHAR","isMeta":"BOOLEAN","isCompactSummary":"BOOLEAN","effort":"VARCHAR",'
    '"isApiErrorMessage":"BOOLEAN","apiErrorStatus":"VARCHAR","attributionSkill":"VARCHAR","attributionMcpServer":"VARCHAR",'
    '"isAbortedMidStream":"BOOLEAN","permissionMode":"VARCHAR","mode":"VARCHAR","customTitle":"VARCHAR","aiTitle":"VARCHAR",'
    '"agentName":"VARCHAR","summary":"VARCHAR","relocatedCwd":"VARCHAR","durationMs":"BIGINT","content":"JSON",'
    '"compactMetadata":{"trigger":"VARCHAR","preTokens":"BIGINT","postTokens":"BIGINT"},'
    '"worktreeSession":{"worktreePath":"VARCHAR","worktreeName":"VARCHAR","worktreeBranch":"VARCHAR","originalCwd":"VARCHAR"},'
    '"frameUrl":"VARCHAR","title":"VARCHAR",'
    '"totalCostUSD":"DOUBLE","totalAPIDuration":"BIGINT","totalToolDuration":"BIGINT","totalLinesAdded":"BIGINT",'
    '"totalLinesRemoved":"BIGINT","totalDuration":"BIGINT","hasUnknownModelCost":"BOOLEAN",'
    '"hookInfos":[{"durationMs":"BIGINT"}],'
    '"message":{"id":"VARCHAR","model":"VARCHAR","stop_reason":"VARCHAR","content":"JSON",'
    '"usage":{"input_tokens":"BIGINT","output_tokens":"BIGINT","cache_read_input_tokens":"BIGINT","cache_creation_input_tokens":"BIGINT",'
    '"server_tool_use":{"web_search_requests":"BIGINT","web_fetch_requests":"BIGINT"}}},'
    '"attachment":{"type":"VARCHAR","snapshot":{"isWorktree":"BOOLEAN","isGitRepo":"BOOLEAN","shell":"VARCHAR",'
    '"additionalWorkingDirectories":["VARCHAR"]}}}'
)

PARSE = f"""
CREATE OR REPLACE TEMP TABLE pl AS
SELECT l.file_id, l.line_no, e['type'] AS ltype, e['subtype'] AS subtype, e['timestamp'] AS ts, e['cwd'] AS cwd,
       e['gitBranch'] AS git_branch, e['version'] AS version, e['entrypoint'] AS entrypoint, e['userType'] AS user_type,
       coalesce(e['isMeta'], false) AS is_meta, coalesce(e['isCompactSummary'], false) AS is_compact_summary,
       e['effort'] AS effort, coalesce(e['isApiErrorMessage'], false) AS api_error, e['apiErrorStatus'] AS api_status,
       e['attributionSkill'] AS skill, e['attributionMcpServer'] AS mcp_server, coalesce(e['isAbortedMidStream'], false) AS aborted,
       e['permissionMode'] AS permission_mode, e['mode'] AS mode, e['customTitle'] AS custom_title, e['aiTitle'] AS ai_title,
       e['agentName'] AS agent_name, e['summary'] AS legacy_summary, e['relocatedCwd'] AS relocated_cwd, e['durationMs'] AS duration_ms,
       e['content'] AS sys_content, e['compactMetadata'] AS compact, e['worktreeSession'] AS worktree,
       e['frameUrl'] AS frame_url, e['title'] AS frame_title,
       e['totalCostUSD'] AS cost_usd, e['totalAPIDuration'] AS api_ms, e['totalToolDuration'] AS tool_ms,
       e['totalLinesAdded'] AS lines_added, e['totalLinesRemoved'] AS lines_removed, e['totalDuration'] AS cost_duration_ms,
       e['hasUnknownModelCost'] AS unknown_cost, e['hookInfos'] AS hooks,
       e['message']['id'] AS msg_id, e['message']['model'] AS model, e['message']['stop_reason'] AS stop_reason,
       e['message']['content'] AS msg_content, e['message']['usage'] AS usage, e['attachment'] AS att
FROM (SELECT file_id, line_no, try(from_json(raw, '{ENVELOPE}')) AS e FROM line WHERE {{where}} AND raw <> '') l
WHERE e IS NOT NULL"""

# One row per session. Usage is summed per API MESSAGE (message.id), never per line: Claude Code writes
# each content block of one response as its own line and repeats the response's usage on every one.
COLUMNS = r"""
CREATE OR REPLACE TEMP TABLE session_cols AS
WITH msg AS (
    SELECT file_id, msg_id, any_value(model) AS model,
           max(usage['input_tokens']) AS input_tokens, max(usage['output_tokens']) AS output_tokens,
           max(usage['cache_read_input_tokens']) AS cache_read, max(usage['cache_creation_input_tokens']) AS cache_write,
           max(usage['server_tool_use']['web_search_requests']) AS web_searches, max(usage['server_tool_use']['web_fetch_requests']) AS web_fetches,
           max(coalesce(usage['input_tokens'], 0) + coalesce(usage['cache_read_input_tokens'], 0) + coalesce(usage['cache_creation_input_tokens'], 0)) AS context
    FROM pl WHERE ltype = 'assistant' AND msg_id IS NOT NULL AND coalesce(model, '') <> '<synthetic>'
    GROUP BY file_id, msg_id),
usage AS (
    SELECT file_id, count(*) AS api_messages, sum(input_tokens) AS input_tokens, sum(output_tokens) AS output_tokens,
           sum(cache_read) AS cache_read_tokens, sum(cache_write) AS cache_write_tokens,
           sum(web_searches) AS web_searches, sum(web_fetches) AS web_fetches, max(context) AS context_max
    FROM msg GROUP BY file_id),
tools AS (
    SELECT file_id, histogram(b.name) AS tool_calls_by_tool, count(*) AS tool_calls,
           count(*) FILTER (WHERE b.name IN ('Task', 'Agent')) AS subagents_spawned
    FROM (SELECT file_id, unnest(from_json(msg_content, '[{"type":"VARCHAR","name":"VARCHAR"}]')) AS b
          FROM pl WHERE ltype = 'assistant' AND json_type(msg_content) = 'ARRAY')
    WHERE b.type = 'tool_use' GROUP BY file_id),
images AS (
    SELECT file_id, count(*) AS images_pasted
    FROM (SELECT file_id, unnest(from_json(msg_content, '[{"type":"VARCHAR"}]')) AS b
          FROM pl WHERE ltype = 'user' AND json_type(msg_content) = 'ARRAY')
    WHERE b.type = 'image' GROUP BY file_id),
base AS (
    SELECT file_id,
           count(*) AS lines,
           -- places: the recorded spelling, and a normalized key (the service would use NormDirKey)
           arg_min(cwd, line_no) FILTER (WHERE coalesce(cwd, '') <> '') AS cwd_first,
           arg_max(cwd, line_no) FILTER (WHERE coalesce(cwd, '') <> '') AS cwd_last,
           count(DISTINCT lower(rtrim(replace(cwd, '\', '/'), '/'))) FILTER (WHERE coalesce(cwd, '') <> '') AS cwd_count,
           arg_max(relocated_cwd, line_no) FILTER (WHERE ltype = 'relocated') AS relocated_cwd,
           arg_max(worktree['worktreePath'], line_no) FILTER (WHERE ltype = 'worktree-state') AS worktree_path,
           arg_max(worktree['worktreeBranch'], line_no) FILTER (WHERE ltype = 'worktree-state') AS worktree_branch,
           arg_max(worktree['originalCwd'], line_no) FILTER (WHERE ltype = 'worktree-state') AS worktree_original_cwd,
           arg_min(att['snapshot']['isGitRepo'], line_no) FILTER (WHERE ltype = 'attachment' AND att['type'] = 'environment') AS env_is_git_repo,
           arg_min(att['snapshot']['isWorktree'], line_no) FILTER (WHERE ltype = 'attachment' AND att['type'] = 'environment') AS env_is_worktree,
           arg_min(att['snapshot']['shell'], line_no) FILTER (WHERE ltype = 'attachment' AND att['type'] = 'environment') AS env_shell,
           arg_min(len(att['snapshot']['additionalWorkingDirectories']), line_no) FILTER (WHERE ltype = 'attachment' AND att['type'] = 'environment') AS env_extra_dirs,
           -- branch, versions, models, effort, entry point, permission
           arg_min(git_branch, line_no) FILTER (WHERE coalesce(git_branch, '') <> '') AS branch_first,
           arg_max(git_branch, line_no) FILTER (WHERE coalesce(git_branch, '') <> '') AS branch_last,
           count(DISTINCT git_branch) FILTER (WHERE coalesce(git_branch, '') <> '') AS branch_count,
           arg_min(version, line_no) FILTER (WHERE coalesce(version, '') <> '') AS version_first,
           arg_max(version, line_no) FILTER (WHERE coalesce(version, '') <> '') AS version_last,
           list(DISTINCT model) FILTER (WHERE ltype = 'assistant' AND coalesce(model, '') NOT IN ('', '<synthetic>')) AS models,
           arg_max(model, line_no) FILTER (WHERE ltype = 'assistant' AND coalesce(model, '') NOT IN ('', '<synthetic>')) AS model_last,
           arg_max(effort, line_no) FILTER (WHERE coalesce(effort, '') <> '') AS effort_last,
           arg_min(entrypoint, line_no) FILTER (WHERE coalesce(entrypoint, '') <> '') AS entrypoint,
           arg_max(permission_mode, line_no) FILTER (WHERE ltype = 'permission-mode') AS permission_mode_last,
           count(*) FILTER (WHERE ltype = 'mode' AND mode = 'plan') AS plan_mode_marks,
           -- times
           min(ts) FILTER (WHERE ltype IN ('user', 'assistant')) AS first_ts,
           max(ts) FILTER (WHERE ltype IN ('user', 'assistant')) AS last_ts,
           max(ts) FILTER (WHERE ltype = 'user' AND user_type = 'external' AND json_type(msg_content) = 'VARCHAR' AND NOT is_meta) AS last_typed_ts,
           max(ts) FILTER (WHERE ltype = 'assistant' AND stop_reason = 'end_turn') AS last_turn_end_ts,
           count(*) FILTER (WHERE ltype = 'system' AND subtype = 'turn_duration') AS turns_timed,
           sum(duration_ms) FILTER (WHERE ltype = 'system' AND subtype = 'turn_duration') AS turn_ms_total,
           max(duration_ms) FILTER (WHERE ltype = 'system' AND subtype = 'turn_duration') AS turn_ms_max,
           -- Claude Code's own cost record (the LAST cost-state line holds the running totals)
           arg_max(cost_usd, line_no) FILTER (WHERE ltype = 'cost-state') AS cost_usd,
           arg_max(api_ms, line_no) FILTER (WHERE ltype = 'cost-state') AS api_ms,
           arg_max(tool_ms, line_no) FILTER (WHERE ltype = 'cost-state') AS tool_ms,
           arg_max(lines_added, line_no) FILTER (WHERE ltype = 'cost-state') AS lines_added,
           arg_max(lines_removed, line_no) FILTER (WHERE ltype = 'cost-state') AS lines_removed,
           arg_max(unknown_cost, line_no) FILTER (WHERE ltype = 'cost-state') AS cost_has_unknown_model,
           -- outcomes
           count(*) FILTER (WHERE ltype = 'assistant' AND api_error) AS api_errors,
           arg_max(api_status, line_no) FILTER (WHERE ltype = 'assistant' AND api_error) AS api_error_status_last,
           count(*) FILTER (WHERE ltype = 'assistant' AND stop_reason = 'refusal') AS refusals,
           count(*) FILTER (WHERE ltype = 'assistant' AND stop_reason = 'max_tokens') AS max_token_stops,
           count(*) FILTER (WHERE ltype = 'assistant' AND aborted) AS aborted_mid_stream,
           count(*) FILTER (WHERE ltype = 'user' AND (starts_with(json_extract_string(msg_content, '$'), '[Request interrupted')
                                                      OR starts_with(json_extract_string(msg_content, '$[0].text'), '[Request interrupted'))) AS interrupts,
           -- prompts, work, context
           count(*) FILTER (WHERE ltype = 'user' AND user_type = 'external' AND json_type(msg_content) = 'VARCHAR' AND NOT is_meta AND NOT is_compact_summary) AS prompts_typed,
           count(*) FILTER (WHERE ltype = 'attachment' AND att['type'] = 'queued_command') AS prompts_queued,
           count(*) FILTER (WHERE ltype = 'assistant') AS assistant_lines,
           list(DISTINCT skill) FILTER (WHERE coalesce(skill, '') <> '') AS skills_used,
           list(DISTINCT mcp_server) FILTER (WHERE coalesce(mcp_server, '') <> '') AS mcp_servers_used,
           count(*) FILTER (WHERE ltype = 'system' AND subtype = 'compact_boundary') AS compactions,
           count(*) FILTER (WHERE ltype = 'system' AND subtype = 'compact_boundary' AND compact['trigger'] = 'auto') AS compactions_auto,
           max(compact['preTokens']) FILTER (WHERE ltype = 'system' AND subtype = 'compact_boundary') AS compaction_pre_tokens_max,
           count(*) FILTER (WHERE ltype = 'system' AND subtype = 'away_summary') AS recaps,
           -- titles and names
           arg_max(custom_title, line_no) FILTER (WHERE ltype = 'custom-title') AS custom_title,
           arg_max(ai_title, line_no) FILTER (WHERE ltype = 'ai-title') AS ai_title,
           arg_max(legacy_summary, line_no) FILTER (WHERE ltype = 'summary') AS legacy_summary,
           arg_max(agent_name, line_no) FILTER (WHERE ltype = 'agent-name') AS agent_name,
           arg_min(left(json_extract_string(msg_content, '$'), 200), line_no)
               FILTER (WHERE ltype = 'user' AND user_type = 'external' AND json_type(msg_content) = 'VARCHAR' AND NOT is_meta AND NOT is_compact_summary) AS first_prompt_head,
           -- artifacts published, Stop-hook time
           count(*) FILTER (WHERE ltype = 'frame-link') AS artifacts_published,
           arg_max(frame_title, line_no) FILTER (WHERE ltype = 'frame-link') AS artifact_last_title,
           sum(list_sum(list_transform(hooks, lambda h: h.durationMs))) FILTER (WHERE ltype = 'system' AND subtype = 'stop_hook_summary') AS stop_hook_ms_total,
           max(list_max(list_transform(hooks, lambda h: h.durationMs))) FILTER (WHERE ltype = 'system' AND subtype = 'stop_hook_summary') AS stop_hook_ms_max
    FROM pl GROUP BY file_id)
SELECT f.path, b.*, u.* EXCLUDE (file_id), t.* EXCLUDE (file_id), i.* EXCLUDE (file_id),
       sub.subagent_files
FROM base b
JOIN file f ON f.file_id = b.file_id
LEFT JOIN usage u ON u.file_id = b.file_id
LEFT JOIN tools t ON t.file_id = b.file_id
LEFT JOIN images i ON i.file_id = b.file_id
LEFT JOIN (SELECT regexp_extract(path, '([0-9a-f-]{36})[/\\]subagents[/\\]', 1) AS sid, count(*) AS subagent_files
           FROM file WHERE sub GROUP BY 1) sub ON contains(f.path, sub.sid)
"""


def coverage(con) -> list:
    """For every column, the share of sessions where it is set (non-null, non-empty, non-zero, non-false).

    Args:
        con: the open database (session_cols exists).

    Returns:
        (column, percent) pairs in table order.

    Raises:
        duckdb.Error: a statement fails.
    """
    cols = [(r[0], r[1]) for r in con.execute("describe session_cols").fetchall()]
    n = con.execute("select count(*) from session_cols").fetchone()[0] or 1
    out = []
    for name, ctype in cols:
        if name in ("path", "file_id"):
            continue
        q = f'"{name}"'
        # Lists first: "VARCHAR[]" also starts with "VARCHAR".
        if ctype.endswith("[]"):
            cond = f"coalesce(len({q}), 0) > 0"
        elif ctype.startswith("VARCHAR"):
            cond = f"coalesce({q}, '') <> ''"
        elif ctype.startswith(("BIGINT", "INTEGER", "DOUBLE", "HUGEINT", "DECIMAL")):
            cond = f"coalesce({q}, 0) <> 0"
        elif ctype == "BOOLEAN":
            cond = f"coalesce({q}, false)"
        elif ctype.startswith("MAP"):
            cond = f"coalesce(cardinality({q}), 0) > 0"
        else:
            cond = f"coalesce(len({q}), 0) > 0"
        k = con.execute(f"select count(*) from session_cols where {cond}").fetchone()[0]
        out.append((name, k * 100.0 / n))
    return out


def main() -> None:
    """Build the per-session columns for every top-level session, print coverage and one example row.

    Raises:
        SystemExit: the database path is missing.
        duckdb.Error: the database lacks raw_lines.py's tables.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    con = bc.connect(sys.argv[1])
    sessions, size = con.execute("select count(*), sum(content_bytes) from file where not sub").fetchone()
    start = time.perf_counter()
    con.execute(PARSE.replace("{where}", "file_id in (select file_id from file where not sub)"))
    parsed = time.perf_counter() - start
    con.execute(COLUMNS)
    total = time.perf_counter() - start
    print(f"== {sessions} sessions ({size / 1e9:.2f} GB of JSONL): parsed in {parsed:.1f} s, columns built in {total - parsed:.1f} s "
          f"({size / 1e6 / total:.0f} MB/s overall)")
    cwd_spellings, cwd_keys = con.execute(
        "select count(distinct cwd_first), count(distinct lower(rtrim(replace(cwd_first, '\\', '/'), '/'))) from session_cols").fetchone()
    print(f"   cwd_first: {cwd_spellings} distinct spellings, {cwd_keys} distinct normalized keys")
    print("== coverage (share of sessions where the column is set)")
    for name, pct in coverage(con):
        print(f"   {name}: {pct:.1f}%")
    if len(sys.argv) > 2:
        row = con.execute("select * from session_cols where path like '%' || ? || '%.jsonl'", [sys.argv[2]]).fetchone()
        names = [d[0] for d in con.description]
        print(f"== one row ({sys.argv[2]})")
        for name, value in zip(names, row or []):
            print(f"   {name}: {value!r}"[:220])


if __name__ == "__main__":
    main()
