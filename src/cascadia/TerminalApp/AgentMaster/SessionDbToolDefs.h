// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's TOOL DEFINITIONS (SESSIONS_DB.md §8.4.3): for each tool, the name, the
// description the model reads, the JSON Schema of its arguments and its work budget. The ONE copy of that text,
// shared by every consumer, so they cannot disagree (§8.4.4 rule 1):
//   - the service's registry (sessiondb/SessionDbTools.cpp BuildRegistry pairs each entry with its handler; the
//     stdio MCP server `agentmaster` and the pipe's `toolList` answer from it);
//   - the in-app AI search (SessionSearchTools.cpp): with the session database On, the model gets these tools as
//     the in-process server `agentmaster` (mcp__agentmaster__<name>), and each call is forwarded to the service
//     through the pipe's `toolCall`.
//
// PURE and header-only (no JSON library, no DuckDB, no Windows headers): TerminalApp includes it, the service
// includes it, and the m5 harness checks it (every schema parses, the names are unique, the text is ASCII).
//
// ⚠ A change to a name, a description or a schema changes the model's prompt (and its cache): bump
// SessionDbProtocol::kToolRegistryVersion with it (§8.4.4 rule 8). Descriptions stay ASCII (§8.4.2).

#pragma once

#include <cstddef>
#include <cstdint>

namespace Agentmaster::SessionDbToolDefs
{
    // One tool's definition.
    struct ToolDef
    {
        const char* name; // e.g. "search" (the MCP name is mcp__agentmaster__<name>)
        const char* description; // the exact text the model reads (§8.4.3)
        const char* inputSchema; // JSON Schema text (additionalProperties false, bounds, defaults)
        int64_t budgetMs; // the work budget; the pipe adds a margin before it interrupts the query
    };

    // The tools, in the order tools/list presents them.
    inline constexpr ToolDef kTools[] = {
        { "search",
          "Find Claude Code sessions on this computer. Every word of `query` must match somewhere in a session: its title, folder, branch, the path of a file it "
          "touched, or a message in the chosen `scopes`. Words in double quotes match as one phrase; a whole session id finds that session and its forks. "
          "Returns up to `limit` sessions, best first, each with why it matched and up to two snippets whose `ref` opens the context in `messages`. Changes "
          "nothing on the screen. Fast (under 0.2 s), so searching again with other words is cheap.",
          R"JSON({"type":"object","additionalProperties":false,"properties":{
"query":{"type":"string","maxLength":500,"description":"Words that must all match. Leave it out to list sessions by the filters only."},
"scopes":{"type":"array","uniqueItems":true,"items":{"enum":["title","path","prompt","reply","tool","subagent"]},"default":["title","path","prompt","reply"],"description":"Where words may match. title: names. path: folders and touched files. prompt: what the user typed. reply: Claude's answers. tool: tool calls and their output (broad, noisy). subagent: subagents' work."},
"fuzzy":{"type":"boolean","default":false,"description":"Also match words whose letters appear in order with other letters between them."},
"days":{"type":"integer","minimum":1,"maximum":3650,"default":90,"description":"Only sessions active in the last N days."},
"after":{"type":"string","format":"date","description":"Only sessions active on or after this local date (YYYY-MM-DD). Overrides days."},
"before":{"type":"string","format":"date","description":"Only sessions active on or before this local date (YYYY-MM-DD)."},
"dir":{"type":"string","maxLength":500,"description":"Part of the working folder's path; '/' and '\\' are the same."},
"branch":{"type":"string","maxLength":200,"description":"Part of the git branch name."},
"tag":{"type":"string","maxLength":100,"description":"A bookmark tag, the whole name, any case."},
"favorite":{"type":"boolean","description":"true: only starred sessions. false: only sessions without a star."},
"open":{"type":"boolean","description":"true: only sessions with a tab open now. false: only closed ones."},
"sort":{"enum":["relevance","active","created"],"default":"relevance","description":"relevance: name matches first, then path, prompt, other text; BM25 inside a group. active / created: newest first."},
"limit":{"type":"integer","minimum":1,"maximum":50,"default":15},
"cursor":{"type":"string","description":"The `next` value of the previous result, for the following rows."}}})JSON",
          150 },
        { "session",
          "Everything known about one session, without its messages: its titles, folder and branch, when it was created and last active, its size, models and "
          "tokens, its lineage (the session it was forked from, its forks, the plan, /clear or /handover it continues), its tags and star, whether a tab has it "
          "open, and the files it touched most. Use it to check a search result before you show it.",
          R"JSON({"type":"object","additionalProperties":false,"required":["id"],"properties":{
"id":{"type":"string","minLength":8,"maxLength":36,"description":"A session id from a result, or a whole UUID."}}})JSON",
          20 },
        { "messages",
          "Read one session's conversation in order, a page at a time: what the user typed and Claude's replies, and on request tool calls, tool output and "
          "thinking. Give `around` a `ref` from search or grep to read the context of a hit, or `order: \"desc\"` to read the end first. Blocks longer than "
          "2,000 characters are cut unless `full` is true.",
          R"JSON({"type":"object","additionalProperties":false,"required":["id"],"properties":{
"id":{"type":"string","minLength":8,"maxLength":36,"description":"A session id from a result, or a whole UUID."},
"roles":{"type":"array","uniqueItems":true,"items":{"enum":["prompt","queued","reply","thinking","tool_call","tool_result","system","subagent"]},"default":["prompt","queued","reply"],"description":"Which blocks to read. subagent: also the subagents' transcripts, merged in by time."},
"around":{"type":"string","description":"A ref: the blocks before and after it."},
"from":{"type":"string","description":"A ref: the blocks starting at it."},
"order":{"enum":["asc","desc"],"default":"asc","description":"desc: newest first."},
"limit":{"type":"integer","minimum":1,"maximum":100,"default":20},
"full":{"type":"boolean","default":false,"description":"Do not cut long blocks (the 64 KB result cap still applies)."},
"cursor":{"type":"string","description":"The `next` value of the previous result."}}})JSON",
          30 },
        { "grep",
          "Find exact text, or a regular expression, in conversations, with the words around it. Use it for precise lookups: an error message, a function name, a "
          "command. Searches the sessions in `ids`, or every session in the filters. A regex needs a literal of 3 or more characters, or `ids`.",
          R"JSON({"type":"object","additionalProperties":false,"required":["pattern"],"properties":{
"pattern":{"type":"string","minLength":1,"maxLength":500,"description":"The text to find (or a regular expression, RE2 syntax, with regex: true)."},
"regex":{"type":"boolean","default":false},
"case_sensitive":{"type":"boolean","default":false},
"ids":{"type":"array","maxItems":50,"items":{"type":"string"},"description":"Only these sessions (ids from results)."},
"dir":{"type":"string","maxLength":500,"description":"Part of the working folder's path (when ids is not given)."},
"days":{"type":"integer","minimum":1,"maximum":3650,"default":90,"description":"Only sessions active in the last N days (when ids is not given)."},
"after":{"type":"string","format":"date","description":"Only sessions active on or after this local date (YYYY-MM-DD)."},
"before":{"type":"string","format":"date","description":"Only sessions active on or before this local date (YYYY-MM-DD)."},
"roles":{"type":"array","uniqueItems":true,"items":{"enum":["prompt","queued","reply","thinking","tool_call","tool_result","system","subagent"]},"default":["prompt","queued","reply","tool_call","tool_result","system"]},
"context":{"type":"integer","minimum":0,"maximum":400,"default":80,"description":"Characters shown on each side of a match."},
"limit":{"type":"integer","minimum":1,"maximum":200,"default":30},
"cursor":{"type":"string","description":"The `next` value of the previous result."}}})JSON",
          200 },
        { "files",
          "The files and folders one session read, edited, wrote, searched or named in a shell command, with counts and first and last times. Use it to see what "
          "a session worked on.",
          R"JSON({"type":"object","additionalProperties":false,"required":["id"],"properties":{
"id":{"type":"string","minLength":8,"maxLength":36,"description":"A session id from a result, or a whole UUID."},
"op":{"enum":["read","edit","write","search","shell"],"description":"Only this kind of use."},
"under":{"type":"string","maxLength":500,"description":"Only files under this folder."},
"limit":{"type":"integer","minimum":1,"maximum":500,"default":100},
"cursor":{"type":"string","description":"The `next` value of the previous result."}}})JSON",
          20 },
        { "who_touched",
          "Which sessions touched a file, or anything under a folder, newest first, and what they did to it. Takes a full path, a folder (everything under it), or "
          "a file name alone (TerminalPage.cpp).",
          R"JSON({"type":"object","additionalProperties":false,"required":["path"],"properties":{
"path":{"type":"string","minLength":1,"maxLength":500,"description":"A full path, a folder, or a file name alone."},
"op":{"enum":["read","edit","write","search","shell"],"description":"Only this kind of use."},
"days":{"type":"integer","minimum":1,"maximum":3650,"default":365},
"after":{"type":"string","format":"date","description":"Only sessions active on or after this local date (YYYY-MM-DD)."},
"before":{"type":"string","format":"date","description":"Only sessions active on or before this local date (YYYY-MM-DD)."},
"limit":{"type":"integer","minimum":1,"maximum":100,"default":20},
"cursor":{"type":"string","description":"The `next` value of the previous result."}}})JSON",
          30 },
        { "timeline",
          "What happened when: for each day (or hour) of a period, the sessions that were active, with their prompts and turns. Use it for questions like 'what "
          "was I doing last Tuesday'.",
          R"JSON({"type":"object","additionalProperties":false,"properties":{
"from":{"type":"string","format":"date","description":"The first local date (YYYY-MM-DD); default 6 days before today."},
"to":{"type":"string","format":"date","description":"The last local date (YYYY-MM-DD); default today."},
"group":{"enum":["day","hour"],"default":"day"},
"dir":{"type":"string","maxLength":500,"description":"Part of the working folder's path."},
"limit":{"type":"integer","minimum":1,"maximum":50,"default":10,"description":"Sessions per bucket."}}})JSON",
          50 },
        { "usage",
          "Token use, grouped by session, day, model or folder. Each API message counts once. Cost appears only where Claude Code recorded it for a session "
          "(cost_source \"claude\", the whole session's cost, with group_by session); no estimate is made.",
          R"JSON({"type":"object","additionalProperties":false,"required":["group_by"],"properties":{
"group_by":{"enum":["session","day","model","dir"]},
"days":{"type":"integer","minimum":1,"maximum":3650,"default":30},
"after":{"type":"string","format":"date","description":"From this local date (YYYY-MM-DD). Overrides days."},
"before":{"type":"string","format":"date","description":"Until this local date (YYYY-MM-DD)."},
"dir":{"type":"string","maxLength":500,"description":"Part of the working folder's path."},
"id":{"type":"string","minLength":8,"maxLength":36,"description":"Only this session."},
"limit":{"type":"integer","minimum":1,"maximum":200,"default":20}}})JSON",
          50 },
        { "related",
          "Sessions connected to one session: its lineage (forked from, its forks, the plan, /clear or /handover chain), and the sessions that share the most "
          "files or rare words with it.",
          R"JSON({"type":"object","additionalProperties":false,"required":["id"],"properties":{
"id":{"type":"string","minLength":8,"maxLength":36,"description":"A session id from a result, or a whole UUID."},
"kinds":{"type":"array","uniqueItems":true,"items":{"enum":["lineage","files","words"]},"default":["lineage","files","words"]},
"limit":{"type":"integer","minimum":1,"maximum":20,"default":5,"description":"Sessions per kind."}}})JSON",
          100 },
        { "sql",
          "Run one read-only SQL query (DuckDB dialect) over these views when no other tool fits: counts, groupings, unusual filters. Returns at most 200 rows "
          "within 2 s. Views: v_sessions(id, uuid, title, dir, dir_key, inferred_dir, inferred_dir_key, git_root, branch, created, active, msgs, tools, model, "
          "models, effort, version, input_tokens, output_tokens, cache_read_tokens, cache_write_tokens, cost_usd, api_errors, interrupts, compactions, "
          "forked_from, bytes, lines, favorite, tags, open, prompts_listed, files_read, files_created, files_edited, transcript_present); "
          "v_messages(id, ref, time, role, tool, subagent, text); v_lines(id, line, type, time, raw): raw is the line's JSON, read fields with json_extract; "
          "v_paths(id, path, dir, file, op, n, first, last); v_usage(id, time, model, input_tokens, output_tokens, cache_read_tokens, cache_write_tokens); "
          "v_links(id, from_id, via).",
          R"JSON({"type":"object","additionalProperties":false,"required":["query"],"properties":{
"query":{"type":"string","minLength":1,"maxLength":4000,"description":"One SELECT over the views."},
"limit":{"type":"integer","minimum":1,"maximum":200,"default":50}}})JSON",
          2000 },
    };

    // How many tools there are.
    inline constexpr size_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);

    // The definition named `name`, or nullptr. Pure; never throws.
    inline const ToolDef* FindTool(const char* name) noexcept
    {
        if (!name)
        {
            return nullptr;
        }
        for (const ToolDef& t : kTools)
        {
            const char* a = t.name;
            const char* b = name;
            while (*a && *a == *b)
            {
                ++a;
                ++b;
            }
            if (*a == '\0' && *b == '\0')
            {
                return &t;
            }
        }
        return nullptr;
    }
}
