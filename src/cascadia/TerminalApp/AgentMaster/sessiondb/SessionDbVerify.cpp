// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the summary parity check (see SessionDbVerify.h).

#include "SessionDbVerify.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <functional>
#include <map>
#include <vector>

#include "DuckDbWrap.h"
#include "SessionDbStore.h"
#include "SessionDbSummary.h"

#include "../ProcessInspect.h" // AnalyzeSessionTranscript
#include "../TranscriptStore.h" // ParseTranscriptTimestamp

namespace Agentmaster::SessionDb
{
    namespace
    {
        // A list's printable form for an example line (first items, cut).
        std::wstring Brief(const std::vector<std::wstring>& v)
        {
            std::wstring out = L"[" + std::to_wstring(v.size()) + L"]";
            for (size_t i = 0; i < v.size() && i < 3; ++i)
            {
                out += L" \"" + v[i].substr(0, 40) + L"\"";
            }
            return out;
        }

        // Timestamps compare as instants: the analyzer keeps the line's text, the database its milliseconds.
        bool SameInstant(const std::wstring& a, const std::wstring& b)
        {
            if (a.empty() || b.empty())
            {
                return a.empty() && b.empty();
            }
            return ParseTranscriptTimestamp(a) == ParseTranscriptTimestamp(b);
        }
    }

    VerifyReport VerifySummaries(Store& store, size_t limit, bool rebuild)
    {
        VerifyReport rep;
        Connection con{ store.Db() };
        struct Candidate
        {
            int32_t sid;
            std::wstring path;
            int64_t size;
            std::wstring shortId;
        };
        std::vector<Candidate> cands;
        {
            auto r = con.Query("SELECT s.sid_key, f.path, f.size_bytes, f.parsed_bytes, s.short_id FROM session s JOIN src_file f ON f.file_id = s.file_id "
                               "WHERE f.missing_since IS NULL ORDER BY s.last_activity_ms DESC NULLS LAST");
            while (r.Next())
            {
                if (r.Int64(2) != r.Int64(3))
                {
                    continue; // only partly ingested: a parity check needs the whole file
                }
                cands.push_back(Candidate{ static_cast<int32_t>(r.Int64(0)), FromUtf8(r.Str(1)), r.Int64(2), FromUtf8(r.Str(4)) });
                if (limit && cands.size() >= limit)
                {
                    break;
                }
            }
        }
        if (rebuild && !cands.empty())
        {
            // The rows as THIS build forms them (an older build's rows would test the older rules).
            std::vector<int32_t> sids;
            sids.reserve(cands.size());
            for (const auto& c : cands)
            {
                sids.push_back(c.sid);
            }
            (void)store.RebuildSummaries(sids, {});
        }
        // field name -> (agreed, examples)
        std::map<std::wstring, std::pair<int64_t, std::vector<std::wstring>>> fields;
        const auto check = [&fields](const wchar_t* name, bool ok, const std::wstring& example) {
            auto& f = fields[name];
            if (ok)
            {
                ++f.first;
            }
            else if (f.second.size() < 3)
            {
                f.second.push_back(example);
            }
            return ok;
        };
        for (const auto& c : cands)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(c.path.c_str(), GetFileExInfoStandard, &fad))
            {
                ++rep.skipped;
                continue;
            }
            const int64_t size = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            if (size != c.size)
            {
                ++rep.skipped; // the transcript grew after the ingest: not comparable
                continue;
            }
            SessionSummary db;
            LoadSummary(con, c.sid, db);
            const SessionSummary app = AnalyzeSessionTranscript(c.path, 0);
            ++rep.compared;
            bool all = true;
            const std::wstring id = c.shortId + L": ";
            all &= check(L"prompts (current segment)", db.userMsgs == app.userMsgs, id + L"db " + Brief(db.userMsgs) + L" vs app " + Brief(app.userMsgs));
            std::vector<std::wstring> dbPrev, appPrev;
            for (const auto& s : db.previousSegments)
            {
                dbPrev.insert(dbPrev.end(), s.userMsgs.begin(), s.userMsgs.end());
            }
            for (const auto& s : app.previousSegments)
            {
                appPrev.insert(appPrev.end(), s.userMsgs.begin(), s.userMsgs.end());
            }
            all &= check(L"earlier segments", db.previousSegments.size() == app.previousSegments.size() && dbPrev == appPrev,
                         id + L"db " + std::to_wstring(db.previousSegments.size()) + L" " + Brief(dbPrev) + L" vs app " +
                             std::to_wstring(app.previousSegments.size()) + L" " + Brief(appPrev));
            all &= check(L"files read", db.filesRead == app.filesRead, id + L"db " + Brief(db.filesRead) + L" vs app " + Brief(app.filesRead));
            all &= check(L"files created", db.filesCreated == app.filesCreated, id + L"db " + Brief(db.filesCreated) + L" vs app " + Brief(app.filesCreated));
            all &= check(L"files edited", db.filesEdited == app.filesEdited, id + L"db " + Brief(db.filesEdited) + L" vs app " + Brief(app.filesEdited));
            all &= check(L"skills", db.skillsLoaded == app.skillsLoaded, id + L"db " + Brief(db.skillsLoaded) + L" vs app " + Brief(app.skillsLoaded));
            all &= check(L"tasks", db.tasksCompleted == app.tasksCompleted && db.tasksPending == app.tasksPending,
                         id + L"db " + std::to_wstring(db.tasksCompleted) + L"/" + std::to_wstring(db.tasksPending) + L" vs app " +
                             std::to_wstring(app.tasksCompleted) + L"/" + std::to_wstring(app.tasksPending));
            all &= check(L"compacted", db.compacted == app.compacted, id + L"db " + std::to_wstring(db.compacted) + L" vs app " + std::to_wstring(app.compacted));
            all &= check(L"recap", db.awaySummary == app.awaySummary, id + L"db " + std::to_wstring(db.awaySummary.size()) + L" chars vs app " + std::to_wstring(app.awaySummary.size()));
            all &= check(L"first ts", SameInstant(db.firstTs, app.firstTs), id + L"db " + db.firstTs + L" vs app " + app.firstTs);
            all &= check(L"last ts", SameInstant(db.lastTs, app.lastTs), id + L"db " + db.lastTs + L" vs app " + app.lastTs);
            all &= check(L"last user ts", SameInstant(db.lastUserTs, app.lastUserTs), id + L"db " + db.lastUserTs + L" vs app " + app.lastUserTs);
            all &= check(L"branch", db.branch == app.branch, id + L"db " + db.branch + L" vs app " + app.branch);
            all &= check(L"plan parent", db.parentSessionId == app.parentSessionId, id + L"db " + db.parentSessionId + L" vs app " + app.parentSessionId);
            all &= check(L"plan exit", db.hasExitPlanMode == app.hasExitPlanMode, id + L"db " + std::to_wstring(db.hasExitPlanMode) + L" vs app " + std::to_wstring(app.hasExitPlanMode));
            all &= check(L"plan content", db.hasPlanContent == app.hasPlanContent, id + L"db " + std::to_wstring(db.hasPlanContent) + L" vs app " + std::to_wstring(app.hasPlanContent));
            all &= check(L"plan file", db.planFilePath == app.planFilePath, id + L"db " + db.planFilePath + L" vs app " + app.planFilePath);
            all &= check(L"plan reads", db.planFilesRead == app.planFilesRead, id + L"db " + Brief(db.planFilesRead) + L" vs app " + Brief(app.planFilesRead));
            if (all)
            {
                ++rep.allAgree;
            }
        }
        rep.text = L"compared " + std::to_wstring(rep.compared) + L" session(s) (" + std::to_wstring(rep.skipped) + L" skipped: grown or gone); " +
                   std::to_wstring(rep.allAgree) + L" agree on every field\n";
        for (const auto& [name, f] : fields)
        {
            rep.text += L"  " + name + L": " + std::to_wstring(f.first) + L"/" + std::to_wstring(rep.compared);
            for (const auto& e : f.second)
            {
                rep.text += L"\n      " + e;
            }
            rep.text += L"\n";
        }
        return rep;
    }
}
