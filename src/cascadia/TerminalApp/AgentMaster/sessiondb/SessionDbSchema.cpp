// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's schema (see SessionDbSchema.h).
//
// Storage notes (SESSIONS_DB.md §5.1, §6.8):
//   - Session and line ids are VARCHAR (the uuid text). The UUID type saves ~20 bytes per line but makes
//     every malformed id a hard append failure; the lossless store must accept whatever Claude wrote.
//   - Large text is ZSTD per column (`USING COMPRESSION zstd`, verified on libduckdb 1.5.6): ~3.9x on
//     transcript text, and those columns are read only for the few rows a request shows. Hot tables stay
//     uncompressed (they are scanned).
//   - No primary key on the big append-only tables: they are kept sorted by their read key and read
//     through zonemaps. Primary keys only where rows are upserted by key (session, summary, path_access).

#include "SessionDbSchema.h"

#include "DuckDbWrap.h"

#include "../SessionDbProtocol.h"

namespace Agentmaster::SessionDb
{
    namespace
    {
        // Every table. CREATE ... IF NOT EXISTS, so running it on an existing file is a no-op.
        constexpr const char* kTables = R"SQL(
CREATE TABLE IF NOT EXISTS meta (key VARCHAR PRIMARY KEY, value VARCHAR);

-- Every source file and its ingest cursor (one row per transcript, subagent transcript, subagent meta,
-- spilled tool output, and history.jsonl).
CREATE TABLE IF NOT EXISTS src_file (
    file_id       INTEGER PRIMARY KEY,
    path          VARCHAR NOT NULL,
    kind          UTINYINT NOT NULL,        -- 0 transcript, 1 subagent transcript, 2 subagent meta, 3 spilled output, 4 history.jsonl
    sid_key       INTEGER,                  -- the owning session (a subagent's / spill's parent); NULL for history.jsonl
    agent_id      VARCHAR,                  -- a subagent file's agent id
    size_bytes    BIGINT,
    mtime_ms      BIGINT,
    birth_ms      BIGINT,
    parsed_bytes  BIGINT NOT NULL DEFAULT 0, -- complete lines consumed so far (the resume cursor)
    line_count    INTEGER NOT NULL DEFAULT 0,
    head_hash     BIGINT,                   -- FNV-1a of the first 4 KB: a replaced file reads as changed
    seg_count     SMALLINT NOT NULL DEFAULT 0, -- /compact boundaries so far (the next line's segment)
    missing_since BIGINT,                   -- the file vanished at this time (Claude's sweep; D7 keeps the rows)
    fold          VARCHAR,                  -- the transcript's fold state (JSON): the session row's accumulators
    ingested_ms   BIGINT
);

-- One row per session (SESSIONS_DB.md §5.6). Columns the summary decides live in `summary`; the
-- v_sessions view joins them.
CREATE TABLE IF NOT EXISTS session (
    sid_key INTEGER PRIMARY KEY,
    session_id VARCHAR NOT NULL,
    short_id VARCHAR,
    file_id INTEGER,
    project_dir VARCHAR,
    kind VARCHAR,
    entrypoint VARCHAR,
    size_bytes BIGINT, birth_ms BIGINT, lines INTEGER,
    transcript UTINYINT,                    -- 0 present, 1 deleted by Claude's sweep (D7), 2 replaced
    cwd VARCHAR, cwd_key VARCHAR, cwd_fold VARCHAR,
    cwd_last VARCHAR, cwd_last_key VARCHAR, cwd_count SMALLINT,
    relocated_cwd VARCHAR, relocated_cwd_key VARCHAR,
    worktree_path VARCHAR, worktree_key VARCHAR, worktree_name VARCHAR, worktree_branch VARCHAR, worktree_original_cwd VARCHAR,
    env_is_git_repo BOOLEAN, env_is_worktree BOOLEAN, env_shell VARCHAR, env_extra_dirs SMALLINT,
    git_root VARCHAR, git_root_key VARCHAR, color_key VARCHAR,
    inferred_dir VARCHAR, inferred_dir_key VARCHAR, inferred_git BOOLEAN,
    home_launch BOOLEAN, fs_checked_ms BIGINT,
    branch_first VARCHAR, branch_last VARCHAR, branch_count SMALLINT,
    version_first VARCHAR, version_last VARCHAR,
    models VARCHAR,                         -- JSON array of the distinct assistant models
    model_last VARCHAR, effort_last VARCHAR, permission_mode_last VARCHAR, plan_mode_marks INTEGER, agent_name VARCHAR,
    created_ms BIGINT, first_ts_ms BIGINT, last_activity_ms BIGINT, last_typed_ms BIGINT, last_turn_end_ms BIGINT,
    turns_timed INTEGER, turn_ms_total BIGINT, turn_ms_max BIGINT,
    cost_usd DOUBLE, api_ms BIGINT, api_ms_no_retries BIGINT, tool_ms BIGINT, wall_ms BIGINT,
    lines_added BIGINT, lines_removed BIGINT, cost_unknown_model BOOLEAN, has_cost_state BOOLEAN,
    api_messages INTEGER, in_tokens BIGINT, out_tokens BIGINT, cache_read_tokens BIGINT, cache_write_tokens BIGINT,
    web_searches INTEGER, web_fetches INTEGER, context_max BIGINT, context_tokens BIGINT,
    api_errors INTEGER, api_error_status_last VARCHAR, refusals INTEGER, max_token_stops INTEGER, aborted_mid_stream INTEGER,
    interrupts INTEGER, end_state VARCHAR, stop_hook_ms_total BIGINT, stop_hook_ms_max BIGINT,
    prompts_typed INTEGER, prompts_queued INTEGER, assistant_lines INTEGER, tool_calls INTEGER,
    tool_calls_by_tool VARCHAR,             -- JSON object name -> count
    skills_used VARCHAR, mcp_servers_used VARCHAR,  -- JSON arrays
    subagents_spawned INTEGER, subagent_files INTEGER, images_pasted INTEGER,
    compactions INTEGER, compactions_auto INTEGER, compaction_pre_tokens_max BIGINT,
    recaps INTEGER, recap_last VARCHAR,
    artifacts_published INTEGER, artifact_last_title VARCHAR, artifact_last_url VARCHAR,
    custom_title VARCHAR, ai_title VARCHAR, legacy_summary VARCHAR, first_prompt VARCHAR, last_prompt VARCHAR, last_reply VARCHAR,
    forked_from VARCHAR, fork_point_line INTEGER, leaf_uuid VARCHAR, off_branch_lines INTEGER,
    updated_ms BIGINT
);

-- The session summary the database forms (SESSIONS_DB.md §8.5, D14): one row per session, rebuilt by
-- SQL over the section tables whenever a refresh tick names the session. Lists are JSON text.
CREATE TABLE IF NOT EXISTS summary (
    sid_key INTEGER PRIMARY KEY,
    prompts VARCHAR,                        -- [{seg, kind, text, uuid}] — the numbered prompts, earlier segments first
    segments VARCHAR,                       -- [{seg, label}] — each /compact boundary's label (the segment it ended is seg - 1)
    current_seg SMALLINT,
    files_created VARCHAR, files_edited VARCHAR, files_read VARCHAR,  -- display labels (SeSummaryFileLabel)
    skills VARCHAR,
    tasks_done SMALLINT, tasks_open SMALLINT,
    plan_exit BOOLEAN, plan_content BOOLEAN, plan_file VARCHAR, plan_files_read VARCHAR,
    parent VARCHAR, recap VARCHAR, compactions SMALLINT,
    first_ms BIGINT, last_ms BIGINT, last_user_ms BIGINT, branch VARCHAR,
    prompts_listed INTEGER, files_read_n INTEGER, files_created_n INTEGER, files_edited_n INTEGER,
    built_ms BIGINT
);

-- THE LOSSLESS STORE (§5.5): every line of every transcript, verbatim. line_id = file_id << 32 | line_no
-- (1-based), so one file is one id range in file order. A line that is not valid UTF-8 keeps its bytes in
-- raw_bin (raw NULL).
CREATE TABLE IF NOT EXISTS line (
    line_id BIGINT NOT NULL, file_id INTEGER NOT NULL, line_no INTEGER NOT NULL,
    uuid VARCHAR, parent_uuid VARCHAR, leaf_uuid VARCHAR,
    type VARCHAR, subtype VARCHAR, ts_ms BIGINT, branch VARCHAR,
    flags UTINYINT, seg SMALLINT, on_branch BOOLEAN,
    bytes INTEGER,
    raw VARCHAR USING COMPRESSION zstd,
    raw_bin BLOB
);
-- New lines wait here until a merge appends them to `line` sorted by line_id (§6.8).
CREATE TABLE IF NOT EXISTS line_hot (
    line_id BIGINT NOT NULL, file_id INTEGER NOT NULL, line_no INTEGER NOT NULL,
    uuid VARCHAR, parent_uuid VARCHAR, leaf_uuid VARCHAR,
    type VARCHAR, subtype VARCHAR, ts_ms BIGINT, branch VARCHAR,
    flags UTINYINT, seg SMALLINT, on_branch BOOLEAN,
    bytes INTEGER,
    raw VARCHAR,
    raw_bin BLOB
);
-- A compaction root's crossing edges (its logical parent and preserved messages; few rows).
CREATE TABLE IF NOT EXISTS line_edge (file_id INTEGER, line_no INTEGER, target VARCHAR);
-- Spilled tool outputs and subagent meta files, one row per file (BLOB: a spill need not be UTF-8).
CREATE TABLE IF NOT EXISTS side_file (file_id INTEGER, bytes BIGINT, content BLOB);

-- Every searchable block, merged into the index (§6.3). Text ZSTD; a block's BM25 length in tok_len.
CREATE TABLE IF NOT EXISTS doc (
    doc_id UINTEGER NOT NULL, sid_key INTEGER NOT NULL, file_id INTEGER NOT NULL, line_no INTEGER NOT NULL,
    block USMALLINT, ts_ms BIGINT, role UTINYINT, flags UTINYINT, tool VARCHAR, tool_use_id VARCHAR,
    text_len INTEGER, tok_len INTEGER,
    text VARCHAR USING COMPRESSION zstd
);
-- Blocks written since the last merge: scanned, pre-folded (capped at the index cap), uncompressed.
CREATE TABLE IF NOT EXISTS doc_hot (
    doc_id UINTEGER NOT NULL, sid_key INTEGER NOT NULL, file_id INTEGER NOT NULL, line_no INTEGER NOT NULL,
    block USMALLINT, ts_ms BIGINT, role UTINYINT, flags UTINYINT, tool VARCHAR, tool_use_id VARCHAR,
    text_len INTEGER, tok_len INTEGER,
    text VARCHAR, text_fold VARCHAR
);
-- Blocks of replaced / deleted files, filtered out of every lookup until the next re-cluster drops them.
CREATE TABLE IF NOT EXISTS tomb (doc_id UINTEGER);
-- The inverted index (§6.3, §6.4): the dictionary (also held in memory), the postings (term -> blocks,
-- appended in term order by every merge) and the session postings (term -> sessions with hit counts).
CREATE TABLE IF NOT EXISTS dict (term_id UINTEGER, term VARCHAR, df UINTEGER, sf UINTEGER);
CREATE TABLE IF NOT EXISTS post (term_id UINTEGER, doc_id UINTEGER, tf USMALLINT);
CREATE TABLE IF NOT EXISTS spost (term_id UINTEGER, sid_key INTEGER, hits UINTEGER);
-- The typed-prompt tier (the prompt scope): small and pre-folded, scanned (§6.2 T2).
CREATE TABLE IF NOT EXISTS prompt (doc_id UINTEGER, sid_key INTEGER, ts_ms BIGINT, kind UTINYINT, text_fold VARCHAR);
-- Typed prompts from ~/.claude/history.jsonl: they outlive a swept transcript.
CREATE TABLE IF NOT EXISTS history_prompt (session_id VARCHAR, ts_ms BIGINT, project VARCHAR, text VARCHAR, text_fold VARCHAR);

-- Tool-touched paths (the folder / file scopes, who_touched, files, the inferred folder).
CREATE TABLE IF NOT EXISTS path_access (
    sid_key INTEGER NOT NULL, path VARCHAR, path_key VARCHAR NOT NULL, dir_fold VARCHAR, leaf_fold VARCHAR, op UTINYINT NOT NULL,
    n INTEGER, first_ms BIGINT, last_ms BIGINT, first_line INTEGER,
    PRIMARY KEY (sid_key, path_key, op)
);
-- One row per API message (deduplicated by message id: Claude writes one line per content block and
-- repeats the response's usage on each).
CREATE TABLE IF NOT EXISTS usage (sid_key INTEGER, message_id VARCHAR, ts_ms BIGINT, model VARCHAR,
    in_tok BIGINT, out_tok BIGINT, cache_read BIGINT, cache_write BIGINT, web_search INTEGER, web_fetch INTEGER);

-- The summary's section rows (D14), extracted at ingest by the app's own rules; the summary SQL joins
-- them with line.on_branch.
CREATE TABLE IF NOT EXISTS sec_prompt (file_id INTEGER, line_no INTEGER, seg SMALLINT, kind UTINYINT, uuid VARCHAR, text VARCHAR);
CREATE TABLE IF NOT EXISTS sec_tool (file_id INTEGER, line_no INTEGER, id VARCHAR, name VARCHAR, path VARCHAR, label VARCHAR,
    plans BOOLEAN, skill VARCHAR, todos_done SMALLINT, todos_open SMALLINT);
CREATE TABLE IF NOT EXISTS sec_result (file_id INTEGER, line_no INTEGER, id VARCHAR, ask BOOLEAN, created BOOLEAN);
CREATE TABLE IF NOT EXISTS sec_recap (file_id INTEGER, line_no INTEGER, text VARCHAR);
CREATE TABLE IF NOT EXISTS sec_compact (file_id INTEGER, line_no INTEGER, seg SMALLINT, label VARCHAR);
CREATE TABLE IF NOT EXISTS sec_user (file_id INTEGER, line_no INTEGER, plan_content BOOLEAN, parent VARCHAR);

-- Agentmaster's own facts, mirrored for joins (the profile's files stay the truth, G6).
CREATE TABLE IF NOT EXISTS am_title (session_id VARCHAR, title VARCHAR);
CREATE TABLE IF NOT EXISTS am_favorite (session_id VARCHAR);
CREATE TABLE IF NOT EXISTS am_tag (session_id VARCHAR, tag VARCHAR, tag_fold VARCHAR);
CREATE TABLE IF NOT EXISTS am_link (session_id VARCHAR, from_id VARCHAR, via VARCHAR);
-- Runtime state the GUI pushes (openChanged): emptied at every service start, never authoritative.
CREATE TABLE IF NOT EXISTS rt_open (session_id VARCHAR);
)SQL";

        // The read-only surface (§5.3): the views the `sql` tool documents, and the macro the GUI's
        // grouping rule needs. CREATE OR REPLACE: a newer service rewrites them freely (no data).
        constexpr const char* kViews = R"SQL(
CREATE OR REPLACE MACRO effective_dir_key(cwd_key, inferred_dir_key, home_launch, inferred_mode) AS
    CASE WHEN (inferred_mode OR coalesce(home_launch, false)) AND coalesce(inferred_dir_key, '') <> '' THEN inferred_dir_key ELSE cwd_key END;

CREATE OR REPLACE VIEW v_sessions AS
SELECT s.short_id AS id, s.session_id AS uuid,
       coalesce(nullif(t.title, ''), nullif(s.custom_title, ''), nullif(s.ai_title, ''), nullif(s.legacy_summary, ''), s.first_prompt) AS title,
       s.cwd AS dir, s.cwd_key AS dir_key, s.inferred_dir, s.inferred_dir_key, s.git_root, s.branch_last AS branch,
       epoch_ms(s.created_ms) AS created, epoch_ms(s.last_activity_ms) AS active,
       s.prompts_typed AS msgs, s.tool_calls AS tools, s.model_last AS model, s.models, s.effort_last AS effort,
       s.version_last AS version, s.in_tokens AS input_tokens, s.out_tokens AS output_tokens,
       s.cache_read_tokens, s.cache_write_tokens, s.cost_usd, s.api_errors, s.interrupts, s.compactions,
       s.forked_from, s.size_bytes AS bytes, s.lines,
       fav.session_id IS NOT NULL AS favorite,
       (SELECT list(tag ORDER BY tag) FROM am_tag g WHERE g.session_id = s.session_id) AS tags,
       op.session_id IS NOT NULL AS "open",
       sm.prompts_listed, sm.files_read_n AS files_read, sm.files_created_n AS files_created, sm.files_edited_n AS files_edited,
       s.transcript = 0 AS transcript_present
FROM session s
LEFT JOIN am_title t ON t.session_id = s.session_id
LEFT JOIN am_favorite fav ON fav.session_id = s.session_id
LEFT JOIN rt_open op ON op.session_id = s.session_id
LEFT JOIN summary sm ON sm.sid_key = s.sid_key;

CREATE OR REPLACE VIEW v_messages AS
SELECT s.short_id AS id, s.short_id || ':' || d.line_no || '.' || d.block AS ref, epoch_ms(d.ts_ms) AS time,
       CASE d.role WHEN 1 THEN 'prompt' WHEN 2 THEN 'reply' WHEN 3 THEN 'thinking' WHEN 4 THEN 'tool_call'
                   WHEN 5 THEN 'tool_result' WHEN 6 THEN 'system' WHEN 7 THEN 'queued' ELSE 'other' END AS role,
       d.tool, (d.flags & 8) <> 0 AS subagent, d.text
FROM (SELECT sid_key, line_no, block, ts_ms, role, flags, tool, text FROM doc
      UNION ALL SELECT sid_key, line_no, block, ts_ms, role, flags, tool, text FROM doc_hot) d
JOIN session s ON s.sid_key = d.sid_key;

CREATE OR REPLACE VIEW v_lines AS
SELECT s.short_id AS id, l.line_no AS line, l.type, epoch_ms(l.ts_ms) AS time, l.raw
FROM (SELECT file_id, line_no, type, ts_ms, raw FROM line UNION ALL SELECT file_id, line_no, type, ts_ms, raw FROM line_hot) l
JOIN src_file f ON f.file_id = l.file_id AND f.kind = 0
JOIN session s ON s.sid_key = f.sid_key;

CREATE OR REPLACE VIEW v_paths AS
SELECT s.short_id AS id, p.path, p.dir_fold AS dir, p.leaf_fold AS file,
       CASE p.op WHEN 1 THEN 'read' WHEN 2 THEN 'edit' WHEN 3 THEN 'write' WHEN 4 THEN 'search' WHEN 5 THEN 'shell' ELSE 'other' END AS op,
       p.n, epoch_ms(p.first_ms) AS first, epoch_ms(p.last_ms) AS last
FROM path_access p JOIN session s ON s.sid_key = p.sid_key;

CREATE OR REPLACE VIEW v_usage AS
SELECT s.short_id AS id, epoch_ms(u.ts_ms) AS time, u.model, u.in_tok AS input_tokens, u.out_tok AS output_tokens,
       u.cache_read AS cache_read_tokens, u.cache_write AS cache_write_tokens
FROM usage u JOIN session s ON s.sid_key = u.sid_key;

CREATE OR REPLACE VIEW v_links AS
SELECT s.short_id AS id, l.from_id, l.via
FROM am_link l JOIN session s ON s.session_id = l.session_id;
)SQL";
    }

    std::string GetMeta(Connection& con, const std::string& key)
    {
        auto p = con.Prepare("SELECT value FROM meta WHERE key = ?");
        p.Bind(1, std::string_view{ key });
        auto r = p.Execute();
        return r.Next() ? r.String(0) : std::string{};
    }

    void SetMeta(Connection& con, const std::string& key, const std::string& value)
    {
        auto p = con.Prepare("INSERT OR REPLACE INTO meta VALUES (?, ?)");
        p.Bind(1, std::string_view{ key }).Bind(2, std::string_view{ value });
        p.Execute();
    }

    SchemaState EnsureSchema(Connection& con)
    {
        // meta first: the version check must happen before any other DDL can touch a newer file.
        con.Execute("CREATE TABLE IF NOT EXISTS meta (key VARCHAR PRIMARY KEY, value VARCHAR)");
        const std::string stored = GetMeta(con, "schema_version");
        if (!stored.empty())
        {
            const int v = std::atoi(stored.c_str());
            if (v > SessionDbProtocol::kSchemaVersion)
            {
                return SchemaState::TooNew; // written by a newer service: never write it (§11)
            }
            // v < kSchemaVersion: migrations go here, version by version, when version 2 exists.
        }
        con.Execute(kTables);
        con.Execute(kViews);
        con.Execute("DELETE FROM rt_open"); // runtime state never survives a service restart
        if (stored.empty())
        {
            SetMeta(con, "schema_version", std::to_string(SessionDbProtocol::kSchemaVersion));
            SetMeta(con, "created_by_build", std::to_string(SessionDbProtocol::kServiceBuild));
            return SchemaState::Created;
        }
        return SchemaState::Current;
    }
}
