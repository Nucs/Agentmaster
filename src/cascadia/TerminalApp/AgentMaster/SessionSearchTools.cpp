// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
// The Sessions AI search's tools; see SessionSearchTools.h, SESSIONS.md §9 and SDK_HARNESS.md §7.
#include "SessionSearchTools.h"

#include "ClaudeSpawn.h" // ClaudeProjectsDir, LogSwallowedException
#include "SessionStore.h" // stored titles, tags, favorites

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <stdexcept>

namespace
{
    using namespace Agentmaster;

    int64_t SystemNowUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Cut `text` to `maxChars` UTF-16 units, ending a cut text in "..." and never splitting a
    // surrogate pair.
    std::wstring Cut(std::wstring text, size_t maxChars)
    {
        if (text.size() <= maxChars)
        {
            return text;
        }
        size_t keep = maxChars > 3 ? maxChars - 3 : 0;
        if (keep > 0 && text[keep - 1] >= 0xD800 && text[keep - 1] <= 0xDBFF)
        {
            --keep;
        }
        text.resize(keep);
        text += L"...";
        return text;
    }

    // Line breaks and tabs to single spaces, so a title or a snippet is one line of JSON text.
    std::wstring OneLine(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        bool lastSpace = true;
        for (const wchar_t c : text)
        {
            const bool space = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
            if (space)
            {
                if (!lastSpace)
                {
                    out.push_back(L' ');
                }
                lastSpace = true;
                continue;
            }
            out.push_back(c);
            lastSpace = false;
        }
        while (!out.empty() && out.back() == L' ')
        {
            out.pop_back();
        }
        return out;
    }

    std::wstring Trim(std::wstring_view text)
    {
        size_t a = 0, b = text.size();
        while (a < b && (text[a] == L' ' || text[a] == L'\t' || text[a] == L'\r' || text[a] == L'\n'))
        {
            ++a;
        }
        while (b > a && (text[b - 1] == L' ' || text[b - 1] == L'\t' || text[b - 1] == L'\r' || text[b - 1] == L'\n'))
        {
            --b;
        }
        return std::wstring{ text.substr(a, b - a) };
    }

    // The local broken-down time of a Unix ms instant. false when the C runtime cannot convert it.
    bool LocalTm(int64_t unixMs, struct tm& out)
    {
        const __time64_t t = unixMs / 1000;
        return _localtime64_s(&out, &t) == 0;
    }

    // "2026-10-08 14:32" (local), or empty for 0 / an instant the runtime cannot convert.
    std::wstring LocalDateTimeText(int64_t unixMs)
    {
        struct tm lt
        {
        };
        if (unixMs <= 0 || !LocalTm(unixMs, lt))
        {
            return {};
        }
        wchar_t buf[24]{};
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        return buf;
    }

    // "2026-10-08" (local), or empty.
    std::wstring LocalDateText(int64_t unixMs)
    {
        struct tm lt
        {
        };
        if (unixMs <= 0 || !LocalTm(unixMs, lt))
        {
            return {};
        }
        wchar_t buf[16]{};
        swprintf_s(buf, L"%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        return buf;
    }

    // How long ago, in the Sessions page's compact form: "now", "5m", "3h", "2d", "3mo", "1y".
    std::wstring AgoText(int64_t unixMs, int64_t nowUnixMs)
    {
        if (unixMs <= 0)
        {
            return {};
        }
        const int64_t s = (std::max)(int64_t{ 0 }, (nowUnixMs - unixMs) / 1000);
        if (s < 60)
        {
            return L"now";
        }
        if (s < 3600)
        {
            return std::to_wstring(s / 60) + L"m";
        }
        if (s < 86400)
        {
            return std::to_wstring(s / 3600) + L"h";
        }
        if (s < 30LL * 86400)
        {
            return std::to_wstring(s / 86400) + L"d";
        }
        if (s < 365LL * 86400)
        {
            return std::to_wstring(s / (30LL * 86400)) + L"mo";
        }
        return std::to_wstring(s / (365LL * 86400)) + L"y";
    }

    // A local calendar date "YYYY-MM-DD" -> the Unix ms of its local midnight (endOfDay: of its
    // last millisecond). 0 when the text is not such a date. DST-safe: the C runtime resolves the
    // offset of that local wall-clock time (tm_isdst -1).
    int64_t ParseLocalDate(std::wstring_view text, bool endOfDay)
    {
        const std::wstring t = Trim(text);
        if (t.size() != 10 || t[4] != L'-' || t[7] != L'-')
        {
            return 0;
        }
        int v[3]{};
        const size_t starts[3] = { 0, 5, 8 };
        const size_t lengths[3] = { 4, 2, 2 };
        for (int i = 0; i < 3; ++i)
        {
            for (size_t k = 0; k < lengths[i]; ++k)
            {
                const wchar_t c = t[starts[i] + k];
                if (c < L'0' || c > L'9')
                {
                    return 0;
                }
                v[i] = v[i] * 10 + (c - L'0');
            }
        }
        if (v[0] < 1971 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31)
        {
            return 0;
        }
        struct tm lt
        {
        };
        lt.tm_year = v[0] - 1900;
        lt.tm_mon = v[1] - 1;
        lt.tm_mday = v[2];
        lt.tm_isdst = -1;
        if (endOfDay)
        {
            lt.tm_hour = 23;
            lt.tm_min = 59;
            lt.tm_sec = 59;
        }
        const __time64_t tt = _mktime64(&lt);
        if (tt == static_cast<__time64_t>(-1))
        {
            return 0;
        }
        return static_cast<int64_t>(tt) * 1000 + (endOfDay ? 999 : 0);
    }

    // <claude home>\history.jsonl — the file of every typed prompt (the sibling of projects\).
    std::wstring HistoryPath()
    {
        std::wstring projects = ClaudeProjectsDir();
        while (!projects.empty() && (projects.back() == L'\\' || projects.back() == L'/'))
        {
            projects.pop_back();
        }
        const size_t cut = projects.find_last_of(L"\\/");
        return cut == std::wstring::npos ? std::wstring{} : projects.substr(0, cut) + L"\\history.jsonl";
    }

    // True when every term of the query matches the session's NAME: its title, its branch, or
    // (an id term) its own or its fork parent's id. The Sessions page's tier-0 rule.
    bool NameMatches(const SessionIndexEntry& e, const std::vector<SearchTerm>& terms)
    {
        if (terms.empty())
        {
            return false;
        }
        const std::wstring title = FoldLower(SessionSearchTitleOf(e));
        const std::wstring branch = FoldLower(e.stats.gitBranch);
        const std::wstring id = FoldLower(e.sessionId);
        const std::wstring parent = FoldLower(e.stats.forkedFromId);
        for (const auto& t : terms)
        {
            const bool hit = (t.isGuid && (t.textLower == id || (!parent.empty() && t.textLower == parent))) ||
                             MatchesQueryText(title, t.textLower, false) ||
                             MatchesQueryText(branch, t.textLower, false);
            if (!hit)
            {
                return false;
            }
        }
        return true;
    }

    const wchar_t* TierWord(SessionMatchTier tier) noexcept
    {
        switch (tier)
        {
        case SessionMatchTier::Name:
            return L"title";
        case SessionMatchTier::Path:
            return L"path";
        case SessionMatchTier::Prompt:
            return L"prompt";
        case SessionMatchTier::Text:
            return L"text";
        case SessionMatchTier::Listed:
        default:
            return L"";
        }
    }

    const wchar_t kSearchSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("query":{"type":"string","description":"Words that must ALL match a session: its title, folder, branch, the paths of files it touched, or a prompt the user typed. Use 1 to 3 distinctive words and leave filler words out. Words in \"double quotes\" match as one exact phrase. Leave it out to list sessions by the filters only."},)"
        LR"("days":{"type":"integer","description":"Only sessions active in the last N days. Default 90. Leave it out unless the user names a period."},)"
        LR"("after":{"type":"string","description":"Only sessions active on or after this local date, YYYY-MM-DD. Overrides days."},)"
        LR"("before":{"type":"string","description":"Only sessions active on or before this local date, YYYY-MM-DD."},)"
        LR"("dir":{"type":"string","description":"Part of the working folder's path."},)"
        LR"("branch":{"type":"string","description":"Part of the git branch name."},)"
        LR"("tag":{"type":"string","description":"A bookmark tag the session carries (the whole tag name)."},)"
        LR"("favorite":{"type":"boolean","description":"true: only starred sessions."},)"
        LR"("open":{"type":"boolean","description":"true: only sessions that have a tab open now."},)"
        LR"("deep":{"type":"boolean","description":"true: also search the full text of the conversations (the agent's replies and tool output). Slower and broader. Use it only when a normal search found nothing useful."},)"
        LR"("sort":{"type":"string","enum":["relevance","active","created"],"description":"relevance (default): title matches first, newest first inside a group. active / created: newest first."},)"
        LR"("limit":{"type":"integer","description":"Rows to return, 1 to 50. Default 15."})"
        LR"(},"required":[]})";
    // The schema above and the system prompt below say "90" in words the model reads.
    static_assert(kSessionSearchDefaultDays == 90, "kSearchSchema and kSystemPrompt state the default period: change their text too");

    const wchar_t kShowSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("ids":{"type":"array","items":{"type":"string"},"description":"The ids of the sessions to show, best match first, exactly as search returned them. An empty list shows nothing: use it when nothing fits."},)"
        LR"("top":{"type":"integer","description":"Instead of ids: show the first N rows of your latest search, in its order."},)"
        LR"("note":{"type":"string","description":"A caption of a few words for the list, in the user's language."})"
        LR"(},"required":[]})";

    // ASCII on purpose: it travels through a file and the API unchanged whatever the code page.
    const wchar_t kSystemPrompt[] =
        L"You are the search assistant inside the Sessions browser of Agentmaster, an app that manages Claude Code sessions. "
        L"The user looks for past Claude Code sessions (conversations) on this computer. You find them with one tool and put them "
        L"on the screen with another. The user sees the list on the screen and your one-sentence reply. The user does not see "
        L"your tool calls or their results.\n"
        L"\n"
        L"Tools:\n"
        L"- mcp__sessions__search finds sessions. Every word of `query` must match somewhere in a session: its title, folder, "
        L"branch, the paths of files it touched, or a prompt the user typed. Filters narrow it: days, after, before, dir, "
        L"branch, tag, favorite, open. It returns rows with a short id.\n"
        L"- mcp__display__show puts sessions on the screen, in your order, and replaces what was there. Give it the short ids "
        L"from search results, best match first, or top:N to show the first N rows of your latest search as they are.\n"
        L"\n"
        L"Rules:\n"
        L"1. Act at once. Never ask a question before you search. For every request: search, then call show exactly once, "
        L"then reply.\n"
        L"2. Be fast. One search is usually enough: 1 to 3 distinctive words, no filler words. Leave `days` out (a search then "
        L"covers the last 90 days) unless the user names a period. If a search finds nothing useful, try again with other or "
        L"fewer words, then a longer period (days:365), then deep:true. Make independent searches in one step.\n"
        L"3. Show only what fits the request, best first: usually 1 to 15 sessions. When nothing fits, call show with an empty "
        L"ids list.\n"
        L"4. After show returns, reply with ONE short sentence, under 25 words, in the user's language: what is on the screen "
        L"and the title of the best match. No list, no markdown, no ids.\n"
        L"5. A follow-up refers to what you showed before (\"only the open ones\", \"older ones\", \"the second one\").\n"
        L"6. Times in the results are local time. Each result gives the current local time as `now`.\n"
        L"7. A result with an `indexing` field does not have the older sessions yet. Do not search again for them: show what "
        L"fits, and say in your sentence that older sessions are still being indexed.\n";
}

namespace Agentmaster
{
    // ===== the catalog ======================================================================

    std::wstring SessionSearchTitleOf(const SessionIndexEntry& entry)
    {
        if (!entry.liveTitle.empty())
        {
            return entry.liveTitle;
        }
        std::wstring title = PickDisplayTitle(entry.stats.customTitle, entry.stats.aiTitle, entry.stats.summary, entry.stats.firstUserPrompt);
        if (title.empty())
        {
            title = L"(no prompt yet)";
        }
        return title;
    }

    int64_t SessionSearchLastActivityMs(const SessionIndexEntry& entry) noexcept
    {
        return entry.stats.lastTimestampMs != 0 ? entry.stats.lastTimestampMs : entry.mtimeMs;
    }

    int64_t SessionSearchCreatedMs(const SessionIndexEntry& entry) noexcept
    {
        if (!entry.stats.forkedFromId.empty())
        {
            return entry.birthMs; // a fork's line timestamps are its parent's
        }
        return entry.stats.firstTimestampMs != 0 ? entry.stats.firstTimestampMs : entry.birthMs;
    }

    std::wstring SessionSearchDirOf(const SessionIndexEntry& entry)
    {
        if (!entry.stats.cwd.empty())
        {
            return entry.stats.cwd;
        }
        // <projects>\<encoded cwd>\<id>.jsonl: the folder's name is the lossy fallback.
        const size_t fileCut = entry.path.find_last_of(L"\\/");
        if (fileCut == std::wstring::npos || fileCut == 0)
        {
            return {}; // no folder part
        }
        const size_t dirCut = entry.path.find_last_of(L"\\/", fileCut - 1);
        const size_t start = dirCut == std::wstring::npos ? 0 : dirCut + 1;
        return entry.path.substr(start, fileCut - start);
    }

    std::shared_ptr<const SessionCatalog> GatherSessionCatalog(int64_t sinceUnixMs, const SessionCatalogSources& sources, const SessionCatalogGatherOptions& options)
    {
        const auto& previous = options.previous;
        auto catalog = std::make_shared<SessionCatalog>();
        catalog->sinceUnixMs = sinceUnixMs;
        catalog->coveredSinceUnixMs = sinceUnixMs; // raised below when a session is left out
        catalog->gatheredUnixMs = SystemNowUnixMs();
        // Newest modification time first — which is what makes a gather that stops early hold
        // the NEWEST sessions.
        const auto refs = sources.projectsDir.empty() ? EnumerateTranscripts(sinceUnixMs) : EnumerateTranscriptsIn(sources.projectsDir, sinceUnixMs);
        catalog->entries.reserve(refs.size());
        std::unordered_map<std::wstring, size_t> indexById;
        indexById.reserve(refs.size());
        std::unordered_map<std::wstring, size_t> previousById;
        if (previous)
        {
            previousById.reserve(previous->entries.size());
            for (size_t i = 0; i < previous->entries.size(); ++i)
            {
                previousById.emplace(previous->entries[i].sessionId, i);
            }
        }
        // The budget's clock: it starts at the first sidecar read (see budgetMs), so the listing
        // above and the copies from `previous` never use the budget up.
        bool readingStarted = false;
        std::chrono::steady_clock::time_point readingSince{};
        bool stopped = false; // the budget is spent, or the caller cancelled: read no more sidecars
        const auto mayRead = [&]() -> bool {
            if (stopped)
            {
                return false;
            }
            if (options.cancelled && options.cancelled())
            {
                stopped = true;
                return false;
            }
            if (!readingStarted)
            {
                readingStarted = true;
                readingSince = std::chrono::steady_clock::now();
                return true; // the one read a budget always allows
            }
            if (options.budgetMs > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - readingSince).count() >= options.budgetMs)
            {
                stopped = true;
                return false;
            }
            return true;
        };
        for (const auto& ref : refs)
        {
            SessionIndexEntry entry;
            const auto old = previousById.find(ref.sessionId);
            if (old != previousById.end() &&
                previous->entries[old->second].path == ref.path &&
                previous->entries[old->second].sizeBytes == ref.sizeBytes &&
                previous->entries[old->second].mtimeMs == ref.mtimeMs)
            {
                // Unchanged since the older catalog: the same (size, mtime) key the sidecar itself
                // is validated by. Its title overlay is applied again below, from fresh sources.
                // Taken even after the budget is spent: it costs no file read.
                entry = previous->entries[old->second];
                entry.liveTitle.clear();
            }
            else if (!mayRead())
            {
                // Left out. The catalog is then complete only for sessions that changed AFTER
                // this one. (mtime >= last activity always holds, so a bound on the modification
                // time is a bound on the activity time a search filters by.) The maximum makes
                // the bound right whatever the order of the listing.
                catalog->coveredSinceUnixMs = (std::max)(catalog->coveredSinceUnixMs, ref.mtimeMs + 1);
                continue;
            }
            else
            {
                entry = sources.indexDir.empty() ? LoadOrRefreshSessionIndex(ref) : LoadOrRefreshSessionIndexIn(sources.indexDir, ref);
            }
            if (!entry.valid)
            {
                continue; // deleted while we listed
            }
            indexById.emplace(entry.sessionId, catalog->entries.size());
            catalog->entries.push_back(std::move(entry));
        }
        // The session store (titles, tags, favorites). Its first read of a process opens every
        // record file, which a caller that gave a budget or a cancel check cannot wait for: such
        // a gather takes the store only when it was read before (then a read is one directory
        // listing), and says so in storeOverlay when it did not.
        const bool bounded = options.budgetMs > 0 || static_cast<bool>(options.cancelled);
        catalog->storeOverlay = !bounded || SessionStoreScanIsWarm();
        if (catalog->storeOverlay)
        {
            // The same overlays the Sessions page applies, in the same order: a stored title
            // first, then (below) the live tab title of an open session, the freshest name.
            for (const auto& [id, title] : LoadAllStoredSessionTitles())
            {
                if (title.empty())
                {
                    continue;
                }
                if (const auto it = indexById.find(id); it != indexById.end())
                {
                    catalog->entries[it->second].liveTitle = title;
                }
            }
            catalog->tags = LoadAllSessionTags();
            catalog->favorites = LoadAllFavoriteSessions();
        }
        if (sources.openSessions)
        {
            for (auto& [id, title] : sources.openSessions())
            {
                if (!title.empty())
                {
                    if (const auto it = indexById.find(id); it != indexById.end())
                    {
                        catalog->entries[it->second].liveTitle = title;
                    }
                }
                catalog->openIds.insert(std::move(id));
            }
        }
        return catalog;
    }

    // ===== one search =======================================================================

    std::optional<SessionSearchParams> ParseSessionSearchParams(const json::Value& arguments, int64_t nowUnixMs, std::wstring& error)
    {
        using Type = json::Value::Type;
        SessionSearchParams p;
        error.clear();
        const auto wrongType = [&error](const wchar_t* key, const wchar_t* expected) {
            error = std::wstring{ L"The argument \"" } + key + L"\" must be " + expected + L".";
        };
        if (const auto* v = arguments.Find(L"query"); v && v->type != Type::Null)
        {
            if (v->type != Type::Str)
            {
                wrongType(L"query", L"a string");
                return std::nullopt;
            }
            p.query = Trim(v->str);
        }
        if (const auto* v = arguments.Find(L"days"); v && v->type != Type::Null)
        {
            if (v->type != Type::Num)
            {
                wrongType(L"days", L"a whole number");
                return std::nullopt;
            }
            p.days = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, 3650));
        }
        const auto date = [&](const wchar_t* key, bool endOfDay, int64_t& out) -> bool {
            const auto* v = arguments.Find(key);
            if (!v || v->type == Type::Null)
            {
                return true;
            }
            out = v->type == Type::Str ? ParseLocalDate(v->str, endOfDay) : 0;
            if (out == 0)
            {
                wrongType(key, L"a date written YYYY-MM-DD");
                return false;
            }
            return true;
        };
        if (!date(L"after", false, p.afterUnixMs) || !date(L"before", true, p.beforeUnixMs))
        {
            return std::nullopt;
        }
        if (p.afterUnixMs != 0 && p.beforeUnixMs != 0 && p.beforeUnixMs < p.afterUnixMs)
        {
            error = L"\"before\" is earlier than \"after\": swap them.";
            return std::nullopt;
        }
        const auto text = [&](const wchar_t* key, std::wstring& out) -> bool {
            const auto* v = arguments.Find(key);
            if (!v || v->type == Type::Null)
            {
                return true;
            }
            if (v->type != Type::Str)
            {
                wrongType(key, L"a string");
                return false;
            }
            out = Trim(v->str);
            return true;
        };
        if (!text(L"dir", p.dir) || !text(L"branch", p.branch) || !text(L"tag", p.tag))
        {
            return std::nullopt;
        }
        // A boolean given as the wrong type reads as false: nothing is lost by a wider search.
        p.favoriteOnly = arguments.BoolAt(L"favorite");
        p.openOnly = arguments.BoolAt(L"open");
        p.deep = arguments.BoolAt(L"deep");
        if (const std::wstring sort = arguments.StrAt(L"sort"); sort == L"active" || sort == L"created")
        {
            p.sort = sort;
        }
        if (const auto* v = arguments.Find(L"limit"); v && v->type == Type::Num)
        {
            p.limit = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, 50));
        }
        (void)nowUnixMs; // the time bounds are resolved by SessionSearchSinceMs, at search time
        return p;
    }

    int64_t SessionSearchSinceMs(const SessionSearchParams& params, int64_t nowUnixMs) noexcept
    {
        if (params.afterUnixMs != 0)
        {
            return params.afterUnixMs;
        }
        return nowUnixMs - static_cast<int64_t>(params.days) * 86400000LL;
    }

    SessionContentSearch DefaultSessionContentSearch()
    {
        SessionContentSearch content;
        content.prompts = [](const SessionQuery& q) {
            return SearchHistoryPrompts(HistoryPath(), q, 2);
        };
        content.transcripts = [](const std::vector<TranscriptRef>& refs, const SessionQuery& q, const std::function<bool()>& cancelled) {
            // SearchTranscriptsSlow calls its predicate between files: it must not be empty.
            static const std::function<bool()> never = [] { return false; };
            return SearchTranscriptsSlow(refs, q, 2, cancelled ? cancelled : never);
        };
        return content;
    }

    SessionSearchOutcome RunSessionSearch(const SessionCatalog& catalog, const SessionSearchParams& params, int64_t nowUnixMs, const SessionContentSearch& content, const std::function<bool()>& cancelled)
    {
        SessionSearchOutcome outcome;
        const auto& entries = catalog.entries;
        const int64_t since = SessionSearchSinceMs(params, nowUnixMs);
        const std::wstring dirLower = FoldLower(params.dir);
        const std::wstring branchLower = FoldLower(params.branch);
        const std::wstring tagFolded = params.tag.empty() ? std::wstring{} : FoldTagName(NormalizeTagName(params.tag));

        // 1. The scope: the time window and the filters. A query never widens it.
        std::vector<char> inScope(entries.size(), 0);
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            const int64_t active = SessionSearchLastActivityMs(e);
            if (active < since || (params.beforeUnixMs != 0 && active > params.beforeUnixMs))
            {
                continue;
            }
            if (!dirLower.empty() && !MatchesPathQuery(FoldLower(SessionSearchDirOf(e)), dirLower, false))
            {
                continue;
            }
            if (!branchLower.empty() && !MatchesQueryText(FoldLower(e.stats.gitBranch), branchLower, false))
            {
                continue;
            }
            if (!tagFolded.empty())
            {
                bool carries = false;
                if (const auto it = catalog.tags.find(e.sessionId); it != catalog.tags.end())
                {
                    for (const auto& tag : it->second)
                    {
                        carries = carries || FoldTagName(tag) == tagFolded;
                    }
                }
                if (!carries)
                {
                    continue;
                }
            }
            if (params.favoriteOnly && catalog.favorites.count(e.sessionId) == 0)
            {
                continue;
            }
            if (params.openOnly && catalog.openIds.count(e.sessionId) == 0)
            {
                continue;
            }
            inScope[i] = 1;
            ++outcome.searched;
        }

        // 2. The matches.
        const auto terms = ParseSessionQuery(params.query);
        std::vector<SessionSearchRow> rows;
        if (terms.empty())
        {
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (inScope[i])
                {
                    rows.push_back(SessionSearchRow{ i, SessionMatchTier::Listed, 0, {} });
                }
            }
        }
        else
        {
            // The index: title, folder, touched paths, id. Over the whole catalog (the function
            // takes the entries as they are); the scope is applied to its answer.
            SessionQuery q;
            q.text = params.query;
            q.scopeTitle = true;
            q.scopeDirs = true;
            q.scopeFiles = true;
            std::unordered_set<std::wstring> fast;
            for (auto& id : SearchIndexFast(entries, q))
            {
                fast.insert(std::move(id));
            }
            // The typed prompts: always, they are one prebuilt file.
            std::unordered_map<std::wstring, SessionHit> promptHits;
            if (content.prompts)
            {
                SessionQuery qp = q;
                qp.scopeUser = true;
                promptHits = content.prompts(qp);
            }
            // The transcripts' text: only on request, and only the sessions in scope. The one
            // step of a search that can take many seconds, so the one that can be cancelled.
            std::unordered_map<std::wstring, SessionHit> textHits;
            if (params.deep && content.transcripts && !(cancelled && cancelled()))
            {
                std::vector<TranscriptRef> refs;
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    if (!inScope[i])
                    {
                        continue;
                    }
                    TranscriptRef r;
                    r.sessionId = entries[i].sessionId;
                    r.path = entries[i].path;
                    r.sizeBytes = entries[i].sizeBytes;
                    r.mtimeMs = entries[i].mtimeMs;
                    r.birthMs = entries[i].birthMs;
                    refs.push_back(std::move(r));
                }
                SessionQuery qd = q;
                qd.scopeUser = true;
                qd.scopeAgent = true;
                for (auto& hit : content.transcripts(refs, qd, cancelled))
                {
                    std::wstring id = hit.sessionId;
                    textHits.emplace(std::move(id), std::move(hit));
                }
            }
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (!inScope[i])
                {
                    continue;
                }
                const auto& e = entries[i];
                const bool isFast = fast.count(e.sessionId) != 0;
                const auto prompt = promptHits.find(e.sessionId);
                const auto text = textHits.find(e.sessionId);
                const bool hasPrompt = prompt != promptHits.end();
                const bool hasText = text != textHits.end();
                if (!isFast && !hasPrompt && !hasText)
                {
                    continue;
                }
                SessionSearchRow row;
                row.entryIndex = i;
                row.tier = (isFast && NameMatches(e, terms)) ? SessionMatchTier::Name : (isFast ? SessionMatchTier::Path : (hasPrompt ? SessionMatchTier::Prompt : SessionMatchTier::Text));
                for (const SessionHit* hit : { hasPrompt ? &prompt->second : nullptr, hasText ? &text->second : nullptr })
                {
                    if (!hit)
                    {
                        continue;
                    }
                    row.hits += hit->hitCount;
                    for (const auto& s : hit->snippets)
                    {
                        if (row.snippets.size() < 2)
                        {
                            row.snippets.push_back(s);
                        }
                    }
                }
                rows.push_back(std::move(row));
            }
        }
        outcome.matched = rows.size();

        // 3. The order, then the cut.
        const bool byCreated = params.sort == L"created";
        const bool byTier = params.sort != L"active" && !byCreated;
        std::stable_sort(rows.begin(), rows.end(), [&](const SessionSearchRow& a, const SessionSearchRow& b) {
            if (byTier && a.tier != b.tier)
            {
                return static_cast<int>(a.tier) < static_cast<int>(b.tier);
            }
            const int64_t ta = byCreated ? SessionSearchCreatedMs(entries[a.entryIndex]) : SessionSearchLastActivityMs(entries[a.entryIndex]);
            const int64_t tb = byCreated ? SessionSearchCreatedMs(entries[b.entryIndex]) : SessionSearchLastActivityMs(entries[b.entryIndex]);
            return ta > tb; // newest first
        });
        const size_t limit = static_cast<size_t>(std::clamp(params.limit, 1, 50));
        if (rows.size() > limit)
        {
            rows.resize(limit);
        }
        outcome.rows = std::move(rows);
        return outcome;
    }

    std::wstring SessionSearchHandleOf(const std::wstring& sessionId)
    {
        return sessionId.substr(0, (std::min)(sessionId.size(), size_t{ 8 }));
    }

    std::wstring DescribeSessionSearch(const SessionSearchParams& params)
    {
        std::wstring out = params.query.empty() ? std::wstring{ L"all sessions" } : (L"\x201C" + params.query + L"\x201D");
        if (params.afterUnixMs != 0)
        {
            out += L" \x00B7 from " + LocalDateText(params.afterUnixMs);
        }
        else
        {
            out += L" \x00B7 last " + std::to_wstring(params.days) + (params.days == 1 ? L" day" : L" days");
        }
        if (params.beforeUnixMs != 0)
        {
            out += L" \x00B7 until " + LocalDateText(params.beforeUnixMs);
        }
        if (!params.dir.empty())
        {
            out += L" \x00B7 in " + params.dir;
        }
        if (!params.branch.empty())
        {
            out += L" \x00B7 branch " + params.branch;
        }
        if (!params.tag.empty())
        {
            out += L" \x00B7 tag " + params.tag;
        }
        if (params.favoriteOnly)
        {
            out += L" \x00B7 favorites";
        }
        if (params.openOnly)
        {
            out += L" \x00B7 open";
        }
        if (params.deep)
        {
            out += L" \x00B7 full text";
        }
        return out;
    }

    std::wstring FormatSessionSearchResult(const SessionCatalog& catalog, const SessionSearchParams& params, const SessionSearchOutcome& outcome, int64_t nowUnixMs, const std::function<std::wstring(const std::wstring&)>& handleOf)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"now", Value::MkStr(LocalDateTimeText(nowUnixMs)));
        std::wstring window = params.afterUnixMs != 0 ? (L"from " + LocalDateText(params.afterUnixMs)) : (L"last " + std::to_wstring(params.days) + L" days");
        if (params.beforeUnixMs != 0)
        {
            window += L" until " + LocalDateText(params.beforeUnixMs);
        }
        root.Set(L"window", Value::MkStr(std::move(window)));
        root.Set(L"searched", Value::MkNum(static_cast<double>(outcome.searched)));
        root.Set(L"matched", Value::MkNum(static_cast<double>(outcome.matched)));
        root.Set(L"returned", Value::MkNum(static_cast<double>(outcome.rows.size())));
        // The catalog does not reach back as far as the search asked: say so, or the model
        // reads "nothing matched" as "there is no such session" and tells the user that.
        const bool partial = catalog.coveredSinceUnixMs > SessionSearchSinceMs(params, nowUnixMs);
        // The tags and the favorites are not loaded yet, and this search filters by one of them:
        // its empty result means "cannot be answered yet", not "no such session".
        const bool storeFilterBlind = !catalog.storeOverlay && (!params.tag.empty() || params.favoriteOnly);
        if (partial || storeFilterBlind)
        {
            std::wstring indexing;
            if (partial)
            {
                indexing = L"Sessions last active before " + LocalDateTimeText(catalog.coveredSinceUnixMs) + L" are not in this result: they are still being indexed. A search a little later has them.";
            }
            if (storeFilterBlind)
            {
                indexing += std::wstring{ indexing.empty() ? L"" : L" " } + L"Tags and favorites are still being loaded, so the tag / favorite filter of this search matched nothing: it is still being indexed too.";
            }
            root.Set(L"indexing", Value::MkStr(std::move(indexing)));
        }
        if (outcome.matched == 0)
        {
            // With a partial index, a longer period cannot help: do not suggest it.
            root.Set(L"hint", Value::MkStr((partial || storeFilterBlind) ? L"Nothing matched in the indexed sessions. Try other or fewer words." : (params.deep ? L"Nothing matched. Try other or fewer words, or a longer period (days)." : L"Nothing matched. Try other or fewer words, a longer period (days), or deep:true.")));
        }
        Value sessions = Value::MkArr();
        for (const auto& row : outcome.rows)
        {
            const auto& e = catalog.entries[row.entryIndex];
            Value o = Value::MkObj();
            o.Set(L"id", Value::MkStr(handleOf ? handleOf(e.sessionId) : SessionSearchHandleOf(e.sessionId)));
            o.Set(L"title", Value::MkStr(Cut(OneLine(SessionSearchTitleOf(e)), 120)));
            o.Set(L"dir", Value::MkStr(SessionSearchDirOf(e)));
            if (!e.stats.gitBranch.empty())
            {
                o.Set(L"branch", Value::MkStr(e.stats.gitBranch));
            }
            const int64_t active = SessionSearchLastActivityMs(e);
            // "2026-10-12 01:08 (5m ago)". A session active within the last minute reads "(now)" —
            // "(now ago)" is what the model was given before 2026-10-12 — and a session with no
            // known activity gets no parentheses at all.
            const std::wstring ago = AgoText(active, nowUnixMs);
            o.Set(L"active", Value::MkStr(LocalDateTimeText(active) + (ago.empty() ? std::wstring{} : (ago == L"now" ? std::wstring{ L" (now)" } : (L" (" + ago + L" ago)")))));
            o.Set(L"created", Value::MkStr(LocalDateText(SessionSearchCreatedMs(e))));
            o.Set(L"msgs", Value::MkNum(e.stats.userPrompts));
            o.Set(L"tools", Value::MkNum(e.stats.toolUses));
            if (row.tier != SessionMatchTier::Listed)
            {
                o.Set(L"match", Value::MkStr(TierWord(row.tier)));
            }
            // Flags and lists only when they say something: fewer tokens for the model to read.
            if (catalog.openIds.count(e.sessionId) != 0)
            {
                o.Set(L"open", Value::MkBool(true));
            }
            if (catalog.favorites.count(e.sessionId) != 0)
            {
                o.Set(L"fav", Value::MkBool(true));
            }
            if (!e.stats.forkedFromId.empty())
            {
                o.Set(L"fork", Value::MkBool(true));
            }
            if (const auto it = catalog.tags.find(e.sessionId); it != catalog.tags.end() && !it->second.empty())
            {
                Value tags = Value::MkArr();
                for (const auto& tag : it->second)
                {
                    tags.Push(Value::MkStr(tag));
                }
                o.Set(L"tags", std::move(tags));
            }
            if (row.hits > 0)
            {
                o.Set(L"hits", Value::MkNum(row.hits));
            }
            if (!row.snippets.empty())
            {
                Value snippets = Value::MkArr();
                for (const auto& s : row.snippets)
                {
                    snippets.Push(Value::MkStr(Cut(OneLine(s), 160)));
                }
                o.Set(L"snippets", std::move(snippets));
            }
            sessions.Push(std::move(o));
        }
        root.Set(L"sessions", std::move(sessions));
        return json::Dump(root);
    }

    // ===== the tool servers =================================================================

    SessionSearchToolset::SessionSearchToolset(Callbacks callbacks) :
        _callbacks{ std::move(callbacks) }
    {
        if (!_callbacks.catalog || !_callbacks.onShow)
        {
            throw std::invalid_argument("SessionSearchToolset needs the catalog and onShow callbacks.");
        }
        const SessionContentSearch defaults = DefaultSessionContentSearch();
        if (!_callbacks.content.prompts)
        {
            _callbacks.content.prompts = defaults.prompts;
        }
        if (!_callbacks.content.transcripts)
        {
            _callbacks.content.transcripts = defaults.transcripts;
        }
    }

    int64_t SessionSearchToolset::Now() const
    {
        return _callbacks.now ? _callbacks.now() : SystemNowUnixMs();
    }

    const wchar_t* SessionSearchToolset::SystemPrompt() noexcept
    {
        return kSystemPrompt;
    }

    void SessionSearchToolset::Reset()
    {
        std::lock_guard lock{ _mutex };
        _handles.clear();
        _lastSearchIds.clear();
    }

    std::wstring SessionSearchToolset::ResolveHandle(const std::wstring& handle) const
    {
        const std::wstring key = FoldLower(Trim(handle));
        std::lock_guard lock{ _mutex };
        if (const auto it = _handles.find(key); it != _handles.end())
        {
            return it->second;
        }
        // The model may write a whole id, or more characters than the short id has: accept any
        // id of this conversation that the text is a prefix of, when it names exactly one.
        std::wstring found;
        if (key.size() >= 8)
        {
            for (const auto& entry : _handles)
            {
                const std::wstring& id = entry.second;
                if (FoldLower(id).rfind(key, 0) == 0)
                {
                    if (!found.empty() && found != id)
                    {
                        return {}; // two sessions: not an id
                    }
                    found = id;
                }
            }
        }
        return found;
    }

    HarnessToolResult SessionSearchToolset::Search(const json::Value& arguments)
    {
        const int64_t now = Now();
        std::wstring error;
        const auto params = ParseSessionSearchParams(arguments, now, error);
        if (!params)
        {
            return { error, true };
        }
        // "Is this search still wanted?" The handler runs on the SDK's reader thread, and while
        // it runs that thread can deliver nothing else — not even the interrupt of this very
        // request. So every slow step below asks, and a search that is not wanted any more
        // returns at once.
        std::function<bool()> cancelled;
        if (_callbacks.generation)
        {
            const uint64_t startGeneration = _callbacks.generation();
            cancelled = [this, startGeneration] { return _callbacks.generation() != startGeneration; };
        }
        // The text of a cancelled search. An error result: the model must not treat it as "no
        // session matched" (its turn is being interrupted anyway).
        const auto cancelledResult = [] {
            return HarnessToolResult{ L"The search was cancelled: the request was stopped or replaced.", true };
        };
        if (_callbacks.onSearching)
        {
            _callbacks.onSearching(DescribeSessionSearch(*params));
        }
        const auto catalog = _callbacks.catalog(SessionSearchSinceMs(*params, now), cancelled);
        if (cancelled && cancelled())
        {
            return cancelledResult();
        }
        if (!catalog)
        {
            // No index at all: its first read is still running (or could not start). The model
            // must not answer "no such session".
            return { L"The session index is still being read, so the search could not run. Do not search again now and do not call show: reply that the session index is still being read and that the user can ask again in a few seconds.", true };
        }
        const SessionSearchOutcome outcome = RunSessionSearch(*catalog, *params, now, _callbacks.content, cancelled);
        if (cancelled && cancelled())
        {
            // Before the short ids are recorded and the page is told: a cancelled search must
            // leave no trace (its rows may be half of what a full scan finds).
            return cancelledResult();
        }

        std::vector<std::wstring> ids;
        ids.reserve(outcome.rows.size());
        std::unordered_map<std::wstring, std::wstring> handleById;
        {
            std::lock_guard lock{ _mutex };
            for (const auto& row : outcome.rows)
            {
                const std::wstring& id = catalog->entries[row.entryIndex].sessionId;
                ids.push_back(id);
                // The short id is the first 8 characters. When another session of this
                // conversation already has it, this one gets a longer prefix, until it is unique.
                std::wstring handle;
                for (size_t len = 8; len <= id.size(); len += 5)
                {
                    handle = FoldLower(id.substr(0, len));
                    const auto it = _handles.find(handle);
                    if (it == _handles.end() || it->second == id)
                    {
                        break;
                    }
                    handle.clear();
                }
                if (handle.empty())
                {
                    handle = FoldLower(id);
                }
                _handles[handle] = id;
                handleById[id] = handle;
            }
            _lastSearchIds = ids;
        }
        if (_callbacks.onCandidates)
        {
            _callbacks.onCandidates(ids);
        }
        const auto handleOf = [&handleById](const std::wstring& id) {
            const auto it = handleById.find(id);
            return it != handleById.end() ? it->second : SessionSearchHandleOf(id);
        };
        return { FormatSessionSearchResult(*catalog, *params, outcome, now, handleOf), false };
    }

    HarnessToolResult SessionSearchToolset::Show(const json::Value& arguments)
    {
        using Type = json::Value::Type;
        const auto* idsArg = arguments.Find(L"ids");
        const auto* topArg = arguments.Find(L"top");
        const bool hasIds = idsArg && idsArg->type == Type::Arr;
        const bool hasTop = topArg && topArg->type == Type::Num && topArg->AsI64() > 0;
        if (!hasIds && !hasTop)
        {
            return { L"Pass \"ids\" (an array of ids from search results; an empty array shows nothing) or \"top\" (a number).", true };
        }
        constexpr size_t kMaxShown = 100;
        std::vector<std::wstring> resolved;
        std::vector<std::wstring> unknown;
        std::unordered_set<std::wstring> seen;
        if (hasIds)
        {
            for (const auto& v : idsArg->arr)
            {
                if (v.type != Type::Str)
                {
                    continue;
                }
                const std::wstring full = ResolveHandle(v.str);
                if (full.empty())
                {
                    if (unknown.size() < 5)
                    {
                        unknown.push_back(Cut(Trim(v.str), 40));
                    }
                    continue;
                }
                if (resolved.size() < kMaxShown && seen.insert(full).second)
                {
                    resolved.push_back(full);
                }
            }
        }
        else
        {
            std::lock_guard lock{ _mutex };
            const size_t n = (std::min)(static_cast<size_t>(topArg->AsI64()), (std::min)(_lastSearchIds.size(), kMaxShown));
            resolved.assign(_lastSearchIds.begin(), _lastSearchIds.begin() + static_cast<std::ptrdiff_t>(n));
        }
        if (resolved.empty() && !unknown.empty())
        {
            // Nothing usable: do not clear the screen for a list of ids that mean nothing.
            std::wstring text = L"None of these ids is from a search result of this conversation:";
            for (const auto& u : unknown)
            {
                text += L" " + u;
            }
            text += L". Search first, then pass the ids exactly as search returned them.";
            return { text, true };
        }
        const std::wstring note = Cut(OneLine(arguments.StrAt(L"note")), 160);
        _callbacks.onShow(resolved, note);

        std::wstring text = L"Shown " + std::to_wstring(resolved.size()) + (resolved.size() == 1 ? L" session." : L" sessions.");
        if (!unknown.empty())
        {
            text += L" Not shown, unknown ids:";
            for (const auto& u : unknown)
            {
                text += L" " + u;
            }
            text += L".";
        }
        text += L" Now reply to the user with one short sentence.";
        return { text, false };
    }

    std::vector<HarnessToolServer> SessionSearchToolset::Servers()
    {
        HarnessTool search;
        search.name = kSearchTool;
        search.description = L"Find Claude Code sessions on this computer. Returns matching sessions as JSON rows (id, title, dir, branch, active, created, msgs, tools, match), best match first. Changes nothing on the screen.";
        search.inputSchemaJson = kSearchSchema;
        search.readOnly = true; // several searches of one model step may run at the same time
        search.handler = [this](const json::Value& arguments) { return Search(arguments); };

        HarnessTool show;
        show.name = kDisplayTool;
        show.description = L"Show sessions to the user in the Sessions browser, in the given order. Replaces what the list shows. Call it once per request, after searching.";
        show.inputSchemaJson = kShowSchema;
        show.handler = [this](const json::Value& arguments) { return Show(arguments); };

        HarnessToolServer sessions;
        sessions.name = kSearchServer;
        sessions.tools.push_back(std::move(search));
        HarnessToolServer display;
        display.name = kDisplayServer;
        display.tools.push_back(std::move(show));
        std::vector<HarnessToolServer> servers;
        servers.push_back(std::move(sessions));
        servers.push_back(std::move(display));
        return servers;
    }
}
