// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the database's session summary (see SessionDbSummary.h).

#include "SessionDbSummary.h"

#include <algorithm>
#include <ctime>
#include <map>

#include <nlohmann/json.hpp>

#include "DuckDbWrap.h"

namespace Agentmaster::SessionDb
{
    namespace
    {
        // The set-based summary build. Every CTE groups by file_id, so one statement serves a whole batch
        // (a refresh tick's sessions, or a cold build's hundreds). Comments name the analyzer rule each
        // part restates (ProcessInspect.Content.cpp AnalyzeSessionTranscriptImpl / CollectConversationSegments).
        //   ln  — the files' stored lines (merged + hot): what the facts read.
        //   nd  — minus the re-written copies (flags & 16): the analyzer skips a uuid seen earlier in the file.
        //   br  — the live branch (on_branch, set by the C++ walk): every field but the earlier segments' prompts.
        //   ls  — the last segment number by position (the earlier segments are those below it) and the boundary count.
        //   cb  — where the CURRENT prompt list starts: after the last ON-BRANCH boundary (the analyzer's main walk).
        constexpr const char* kSummarySql = R"SQL(
INSERT OR REPLACE INTO summary
WITH f AS (SELECT CAST(unnest(CAST($1 AS BIGINT[])) AS INTEGER) AS file_id, CAST(unnest(CAST($2 AS BIGINT[])) AS INTEGER) AS sid_key),
ln AS (
    SELECT file_id, line_no, ts_ms, branch, flags, seg, on_branch FROM line WHERE file_id IN (SELECT file_id FROM f)
    UNION ALL
    SELECT file_id, line_no, ts_ms, branch, flags, seg, on_branch FROM line_hot WHERE file_id IN (SELECT file_id FROM f)),
nd AS (SELECT * FROM ln WHERE (flags & 16) = 0),
br AS (SELECT * FROM nd WHERE on_branch),
ls AS (SELECT f.file_id, coalesce(max(nd.seg), 0) AS last_seg,
              count(nd.line_no) FILTER (WHERE (nd.flags & 2) <> 0) AS boundaries
       FROM f LEFT JOIN nd ON nd.file_id = f.file_id GROUP BY f.file_id),
-- firstTs / lastTs: the first / last live-branch line with a timestamp (file order); branch: the first gitBranch.
fx AS (SELECT file_id,
              arg_min(ts_ms, line_no) FILTER (WHERE coalesce(ts_ms, 0) <> 0) AS first_ms,
              arg_max(ts_ms, line_no) FILTER (WHERE coalesce(ts_ms, 0) <> 0) AS last_ms,
              arg_min(branch, line_no) FILTER (WHERE coalesce(branch, '') <> '') AS branch,
              min(line_no) FILTER (WHERE (flags & 8) <> 0) AS first_ext
       FROM br GROUP BY file_id),
-- Where the CURRENT list starts: after the last ON-BRANCH boundary. The analyzer's main walk reads only live-branch
-- lines and clears its Messages list at each boundary it reaches, so an OFF-branch boundary (a rewind past a
-- compaction) never restarts it, and the earlier live-branch prompts stay listed (2026-10-15: one session of 296 at
-- full scale listed 2 prompts here and 9 in the analyzer, its only compaction being off-branch).
cb AS (SELECT f.file_id, coalesce(max(b.line_no) FILTER (WHERE (b.flags & 2) <> 0), -1) AS cur_from
       FROM f LEFT JOIN br b ON b.file_id = f.file_id GROUP BY f.file_id),
-- The prompts (SummaryPromptOfLine's rows) with their branch flag, time, and whether they belong to the current list.
p AS (SELECT sp.file_id, sp.line_no, sp.seg, sp.kind, sp.uuid, sp.text, n.on_branch, n.ts_ms,
             (n.on_branch AND sp.line_no > cb.cur_from) AS cur
      FROM sec_prompt sp JOIN nd n ON n.file_id = sp.file_id AND n.line_no = sp.line_no JOIN cb ON cb.file_id = sp.file_id),
pk AS (SELECT p.*,
              row_number() OVER (PARTITION BY p.file_id, p.seg, p.text ORDER BY p.line_no) AS nth_all,
              row_number() OVER (PARTITION BY p.file_id, p.cur, p.text ORDER BY p.line_no) AS nth_cur
       FROM p),
-- Listed: the CURRENT list (the live-branch prompts after the last on-branch boundary, deduplicated among
-- themselves: the analyzer's main walk), and the EARLIER segments (split by position at EVERY boundary, every prompt
-- deduplicated within its segment: CollectConversationSegments). The two rules differ, so one prompt can be in both
-- lists (an off-branch boundary): each entry carries `cur`.
lst AS (SELECT file_id, line_no, seg, kind, text, uuid, true AS cur FROM pk WHERE cur AND nth_cur = 1
        UNION ALL
        SELECT pk.file_id, pk.line_no, pk.seg, pk.kind, pk.text, pk.uuid, false AS cur
        FROM pk JOIN ls ON ls.file_id = pk.file_id WHERE pk.seg < ls.last_seg AND pk.nth_all = 1),
pl AS (SELECT file_id,
              to_json(list({'seg': seg, 'kind': kind, 'text': text, 'uuid': uuid, 'cur': cur} ORDER BY cur, line_no))::VARCHAR AS prompts,
              count(*) AS listed
       FROM lst GROUP BY file_id),
-- "Last user message": the last live-branch typed prompt (duplicates count), the latest queued message.
pt AS (SELECT file_id,
              arg_max(ts_ms, line_no) FILTER (WHERE kind = 1 AND on_branch) AS last_typed_ms,
              max(ts_ms) FILTER (WHERE kind = 2 AND on_branch) AS last_queued_ms
       FROM p GROUP BY file_id),
-- An answered AskUserQuestion moves "last user message" too.
ans AS (SELECT r.file_id, arg_max(b.ts_ms, r.line_no) AS last_answer_ms
        FROM sec_result r JOIN br b ON b.file_id = r.file_id AND b.line_no = r.line_no WHERE r.ask GROUP BY r.file_id),
t AS (SELECT st.* FROM sec_tool st JOIN br b ON b.file_id = st.file_id AND b.line_no = st.line_no),
rr AS (SELECT r.* FROM sec_result r JOIN br b ON b.file_id = r.file_id AND b.line_no = r.line_no WHERE NOT r.ask),
-- The three file lists (labels): a Write is created when its result says so; an Edit, or an overwriting
-- Write, before the label's first creation keeps it edited (the analyzer never removes an entry); Read
-- keeps only labels neither created nor edited.
ft AS (SELECT file_id, name, id, line_no, label FROM t WHERE name IN ('Read', 'Edit', 'Write') AND coalesce(path, '') <> ''),
w AS (SELECT ft.file_id, ft.label, rr.line_no AS result_line, coalesce(rr.created, false) AS created
      FROM ft LEFT JOIN rr ON rr.file_id = ft.file_id AND rr.id = ft.id WHERE ft.name = 'Write'),
cr AS (SELECT file_id, label, min(result_line) AS at FROM w WHERE created GROUP BY file_id, label),
ev AS (SELECT file_id, label, line_no AS at FROM ft WHERE name = 'Edit'
       UNION ALL SELECT file_id, label, coalesce(result_line, 2147483647) FROM w WHERE NOT created),
ed AS (SELECT DISTINCT e.file_id, e.label FROM ev e LEFT JOIN cr c ON c.file_id = e.file_id AND c.label = e.label
       WHERE c.at IS NULL OR e.at < c.at),
ro AS (SELECT DISTINCT x.file_id, x.label FROM (SELECT file_id, label FROM ft WHERE name = 'Read') x
       ANTI JOIN cr c ON c.file_id = x.file_id AND c.label = x.label
       ANTI JOIN ed e ON e.file_id = x.file_id AND e.label = x.label),
fl AS (SELECT file_id,
              to_json(list(label ORDER BY starts_with(label, 'scratchpad/'), label) FILTER (WHERE kind = 'c'))::VARCHAR AS created,
              count(*) FILTER (WHERE kind = 'c') AS created_n,
              to_json(list(label ORDER BY starts_with(label, 'scratchpad/'), label) FILTER (WHERE kind = 'e'))::VARCHAR AS edited,
              count(*) FILTER (WHERE kind = 'e') AS edited_n,
              to_json(list(label ORDER BY starts_with(label, 'scratchpad/'), label) FILTER (WHERE kind = 'r'))::VARCHAR AS readonly,
              count(*) FILTER (WHERE kind = 'r') AS read_n
       FROM (SELECT file_id, 'c' AS kind, label FROM cr UNION ALL SELECT file_id, 'e', label FROM ed
             UNION ALL SELECT file_id, 'r', label FROM ro)
       GROUP BY file_id),
-- Skills loaded, the last TodoWrite's tasks, the plan-end facts.
tl AS (SELECT file_id,
              to_json(list(DISTINCT skill ORDER BY skill) FILTER (WHERE name = 'Skill' AND coalesce(skill, '') <> ''))::VARCHAR AS skills,
              arg_max(todos_done, line_no) FILTER (WHERE name = 'TodoWrite' AND todos_done >= 0) AS tasks_done,
              arg_max(todos_open, line_no) FILTER (WHERE name = 'TodoWrite' AND todos_open >= 0) AS tasks_open,
              coalesce(bool_or(name = 'ExitPlanMode'), false) AS exit_plan,
              arg_max(path, line_no) FILTER (WHERE name = 'Write' AND plans) AS plan_file,
              to_json(list(path ORDER BY line_no) FILTER (WHERE name = 'Read' AND plans))::VARCHAR AS plan_reads
       FROM t GROUP BY file_id),
-- The last live-branch recap.
rc AS (SELECT r.file_id, arg_max(r.text, r.line_no) AS recap
       FROM sec_recap r JOIN br b ON b.file_id = r.file_id AND b.line_no = r.line_no WHERE r.text <> '' GROUP BY r.file_id),
-- The first live-branch external user line's plan content and plan-restart parent.
fu AS (SELECT u.file_id, u.plan_content, u.parent FROM sec_user u JOIN fx ON fx.file_id = u.file_id AND fx.first_ext = u.line_no),
-- Each compaction's label, in file order (non-copy boundaries).
sg AS (SELECT c.file_id, to_json(list({'seg': c.seg, 'label': c.label} ORDER BY c.line_no))::VARCHAR AS segments
       FROM sec_compact c JOIN nd n ON n.file_id = c.file_id AND n.line_no = c.line_no GROUP BY c.file_id)
SELECT f.sid_key, pl.prompts, sg.segments, ls.last_seg,
       fl.created, fl.edited, fl.readonly, tl.skills, tl.tasks_done, tl.tasks_open,
       coalesce(tl.exit_plan, false), coalesce(fu.plan_content, false), tl.plan_file, tl.plan_reads,
       fu.parent, rc.recap, ls.boundaries,
       fx.first_ms, fx.last_ms,
       nullif(greatest(coalesce(pt.last_typed_ms, 0), coalesce(pt.last_queued_ms, 0), coalesce(ans.last_answer_ms, 0)), 0),
       fx.branch,
       coalesce(pl.listed, 0), coalesce(fl.read_n, 0), coalesce(fl.created_n, 0), coalesce(fl.edited_n, 0),
       $3
FROM f
JOIN ls ON ls.file_id = f.file_id
LEFT JOIN fx ON fx.file_id = f.file_id
LEFT JOIN pl ON pl.file_id = f.file_id
LEFT JOIN pt ON pt.file_id = f.file_id
LEFT JOIN ans ON ans.file_id = f.file_id
LEFT JOIN fl ON fl.file_id = f.file_id
LEFT JOIN tl ON tl.file_id = f.file_id
LEFT JOIN rc ON rc.file_id = f.file_id
LEFT JOIN fu ON fu.file_id = f.file_id
LEFT JOIN sg ON sg.file_id = f.file_id
)SQL";

        using json = nlohmann::json;

        // A JSON array of strings (text) -> wide strings. Malformed / NULL -> empty.
        std::vector<std::wstring> WideList(std::string_view text)
        {
            std::vector<std::wstring> out;
            if (text.empty())
            {
                return out;
            }
            const json j = json::parse(text, nullptr, false);
            if (!j.is_array())
            {
                return out;
            }
            for (const auto& e : j)
            {
                if (e.is_string())
                {
                    out.push_back(FromUtf8(e.get_ref<const std::string&>()));
                }
            }
            return out;
        }
    }

    const std::string& SummaryBuildSql()
    {
        static const std::string sql{ kSummarySql };
        return sql;
    }

    std::wstring IsoFromMs(int64_t ms)
    {
        if (ms <= 0)
        {
            return {};
        }
        const time_t secs = static_cast<time_t>(ms / 1000);
        std::tm tm{};
        if (gmtime_s(&tm, &secs) != 0)
        {
            return {};
        }
        wchar_t buf[40];
        swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                   static_cast<int>(ms % 1000));
        return buf;
    }

    void LoadSummary(Connection& con, int32_t sidKey, SessionSummary& out)
    {
        out = SessionSummary{};
        auto p = con.Prepare("SELECT prompts, segments, current_seg, files_created, files_edited, files_read, skills, tasks_done, tasks_open, "
                             "plan_exit, plan_content, plan_file, plan_files_read, parent, recap, compactions, first_ms, last_ms, last_user_ms, branch "
                             "FROM summary WHERE sid_key = ?");
        p.Bind(1, static_cast<int64_t>(sidKey));
        auto r = p.Execute();
        if (!r.Next())
        {
            return;
        }
        out.found = true;
        const int64_t currentSeg = r.Int64(2);
        // Segment labels: the boundary with segment number s+1 ended segment s.
        std::map<int64_t, std::wstring> labelEnding;
        if (const std::string_view segs = r.Str(1); !segs.empty())
        {
            const json j = json::parse(segs, nullptr, false);
            if (j.is_array())
            {
                for (const auto& e : j)
                {
                    if (e.is_object() && e.contains("seg") && e["seg"].is_number() && e.contains("label") && e["label"].is_string())
                    {
                        labelEnding[e["seg"].get<int64_t>() - 1] = FromUtf8(e["label"].get_ref<const std::string&>());
                    }
                }
            }
        }
        std::map<int64_t, ConversationSegment> earlier;
        if (const std::string_view prompts = r.Str(0); !prompts.empty())
        {
            const json j = json::parse(prompts, nullptr, false);
            if (j.is_array())
            {
                for (const auto& e : j)
                {
                    if (!e.is_object() || !e.contains("text") || !e["text"].is_string())
                    {
                        continue;
                    }
                    const int64_t seg = e.contains("seg") && e["seg"].is_number() ? e["seg"].get<int64_t>() : 0;
                    std::wstring text = FromUtf8(e["text"].get_ref<const std::string&>());
                    std::wstring uuid = e.contains("uuid") && e["uuid"].is_string() ? FromUtf8(e["uuid"].get_ref<const std::string&>()) : std::wstring{};
                    // `cur` says which list an entry is in (the SQL's two rules can put one prompt in both). A row
                    // written before the flag existed falls back to the segment test it used then.
                    const bool current = e.contains("cur") && e["cur"].is_boolean() ? e["cur"].get<bool>() : seg == currentSeg;
                    if (current)
                    {
                        out.userMsgs.push_back(std::move(text));
                        out.userMsgUuids.push_back(std::move(uuid));
                    }
                    else
                    {
                        auto& s = earlier[seg];
                        s.userMsgs.push_back(std::move(text));
                        s.uuids.push_back(std::move(uuid));
                    }
                }
            }
        }
        out.compacted = currentSeg > 0;
        for (auto& [seg, s] : earlier)
        {
            if (const auto it = labelEnding.find(seg); it != labelEnding.end())
            {
                s.label = it->second;
            }
            if (!s.userMsgs.empty())
            {
                out.previousSegments.push_back(std::move(s)); // oldest first (the map is ordered); empty ones dropped
            }
        }
        out.filesCreated = WideList(r.Str(3));
        out.filesEdited = WideList(r.Str(4));
        out.filesRead = WideList(r.Str(5));
        // The analyzer's own order (plain labels, then scratchpad/ labels), restated over UTF-16.
        std::sort(out.filesCreated.begin(), out.filesCreated.end(), SeSummaryLabelLess);
        std::sort(out.filesEdited.begin(), out.filesEdited.end(), SeSummaryLabelLess);
        std::sort(out.filesRead.begin(), out.filesRead.end(), SeSummaryLabelLess);
        out.skillsLoaded = WideList(r.Str(6));
        std::sort(out.skillsLoaded.begin(), out.skillsLoaded.end());
        out.tasksCompleted = r.IsNull(7) ? 0 : static_cast<int>(r.Int64(7));
        out.tasksPending = r.IsNull(8) ? 0 : static_cast<int>(r.Int64(8));
        out.hasExitPlanMode = r.Bool(9);
        out.hasPlanContent = r.Bool(10);
        out.planFilePath = FromUtf8(r.Str(11));
        out.planFilesRead = WideList(r.Str(12));
        out.parentSessionId = FromUtf8(r.Str(13));
        out.awaySummary = FromUtf8(r.Str(14));
        out.firstTs = IsoFromMs(r.Int64(16));
        out.lastTs = IsoFromMs(r.Int64(17));
        out.lastUserTs = IsoFromMs(r.Int64(18));
        out.branch = FromUtf8(r.Str(19));
    }
}
