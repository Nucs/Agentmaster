// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's search (see SessionDbSearch.h).

#include "SessionDbSearch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <set>
#include <unordered_set>

#include "DuckDbWrap.h"
#include "SessionDbExtract.h" // the role / flag values
#include "SessionDbIndex.h"
#include "SessionDbStore.h"

#include "../SessionSearch.h" // ParseSessionQuery, MatchesQueryText, MatchesPathQuery, FoldLower, MakeSnippet, BuildSearchRegex
#include "../SessionDbTokenizer.h"

namespace Agentmaster::SessionDb
{
    namespace
    {
        // A term expansion past this many dictionary terms is cut (and the result marked partial): a
        // two-letter fragment can match most of a million terms.
        constexpr size_t kMaxExpansion = 60000;
        // Rule-2 candidates verified on their text at most (a broad separated term is answered partially).
        constexpr size_t kMaxVerify = 4000;
        // BM25's constants.
        constexpr double kK1 = 1.2;
        constexpr double kB = 0.75;

        std::wstring_view DirPartOf(std::wstring_view p)
        {
            const size_t cut = p.find_last_of(L"\\/");
            return cut == std::wstring_view::npos ? std::wstring_view{} : p.substr(0, cut);
        }

        std::wstring_view LeafPartOf(std::wstring_view p)
        {
            const size_t cut = p.find_last_of(L"\\/");
            return cut == std::wstring_view::npos ? p : p.substr(cut + 1);
        }

        // The SQL filter of the content scopes over a block's role and flags (doc / doc_hot columns).
        std::string ScopeFilter(const SearchRequest& req)
        {
            std::string roles;
            const auto add = [&roles](int r) {
                if (!roles.empty())
                {
                    roles += ",";
                }
                roles += std::to_string(r);
            };
            if (req.scopeReply)
            {
                add(kRoleReply);
                add(kRoleThinking);
                add(kRoleSystem);
            }
            if (req.scopeTool)
            {
                add(kRoleToolCall);
                add(kRoleToolResult);
            }
            // Meta / noise blocks (caveats, command echoes) are stored for reading, never searched (today's rule).
            // The role scopes apply to a subagent's blocks too: "subagents" widens WHERE the search looks, the
            // roles say WHAT it reads there (with tools off, a subagent's replies but not its tool output).
            const std::string rolesSql = roles.empty() ? std::string{ "false" } : "role IN (" + roles + ")";
            const std::string where = req.scopeSubagent ? std::string{ "true" } : std::string{ "(flags & 8) = 0" };
            return "((flags & 3) = 0 AND " + rolesSql + " AND " + where + ")";
        }

        // A list of session keys as the BIGINT list parameter.
        std::vector<int64_t> SidList(const std::vector<int32_t>& sids)
        {
            return { sids.begin(), sids.end() };
        }

    }

    DocTextCache::DocTextCache(size_t budgetBytes) :
        _budget(budgetBytes)
    {
    }

    bool DocTextCache::Get(uint32_t docId, std::wstring& text)
    {
        std::lock_guard lk{ _mtx };
        const auto it = _at.find(docId);
        if (it == _at.end())
        {
            return false;
        }
        _lru.splice(_lru.begin(), _lru, it->second); // most recently used first
        text = it->second->second;
        return true;
    }

    void DocTextCache::Put(uint32_t docId, const std::wstring& text)
    {
        const size_t bytes = text.size() * sizeof(wchar_t);
        if (bytes > _budget / 4)
        {
            return; // one huge tool output must not flush everything else
        }
        std::lock_guard lk{ _mtx };
        if (_at.count(docId))
        {
            return; // a doc id's text never changes
        }
        _lru.emplace_front(docId, text);
        _at.emplace(docId, _lru.begin());
        _bytes += bytes;
        while (_bytes > _budget && !_lru.empty())
        {
            const auto& last = _lru.back();
            _bytes -= last.second.size() * sizeof(wchar_t);
            _at.erase(last.first);
            _lru.pop_back();
        }
    }

    bool ReadBlockTexts(Connection& con, const std::vector<int64_t>& ids, bool withHot, DocTextCache& cache,
                        std::chrono::steady_clock::time_point deadline, const std::function<void(uint32_t, BlockText&)>& fn)
    {
        // Ids fetched by ONE point-lookup query at most: DuckDB serves an id list through the doc_by_id ART
        // index only while it matches fewer than MAX(2,048 rows, 0.1% of the table); past that it scans and
        // decompresses every block's text (§6.8).
        constexpr size_t kMaxPointLookup = 1000;
        // Tool texts read from doc_tool by one query at most. Each scattered block there costs a ZSTD
        // vector's decompression, so small chunks let the deadline stop the reads close to the budget.
        constexpr size_t kToolChunk = 32;
        if (ids.empty())
        {
            return true;
        }
        std::vector<std::pair<uint32_t, BlockText>> tool; // merged tool blocks: their text is in doc_tool
        const char* const tables[] = { "doc", "doc_hot" };
        for (const char* table : tables)
        {
            if (!withHot && table == tables[1])
            {
                break;
            }
            auto p = con.Prepare(std::string{ "SELECT doc_id, line_no, block, role, flags, text, file_id, ts_ms, coalesce(tool, '') FROM " } + table +
                                 " WHERE doc_id IN (SELECT unnest(CAST($1 AS BIGINT[])))");
            for (size_t at = 0; at < ids.size(); at += kMaxPointLookup)
            {
                const size_t end = std::min(ids.size(), at + kMaxPointLookup);
                p.BindInt64List(1, std::vector<int64_t>(ids.begin() + static_cast<ptrdiff_t>(at), ids.begin() + static_cast<ptrdiff_t>(end)));
                auto r = p.Execute();
                while (r.Next())
                {
                    const uint32_t id = static_cast<uint32_t>(r.Int64(0));
                    BlockText row;
                    row.fileId = static_cast<int32_t>(r.Int64(6));
                    row.line = static_cast<int32_t>(r.Int64(1));
                    row.block = static_cast<uint16_t>(r.Int64(2));
                    row.role = static_cast<uint8_t>(r.Int64(3));
                    row.flags = static_cast<uint8_t>(r.Int64(4));
                    row.tsMs = r.Int64(7);
                    row.tool = FromUtf8(r.Str(8));
                    if (r.IsNull(5))
                    {
                        tool.emplace_back(id, std::move(row)); // a merged tool block (doc.text is NULL for it)
                        continue;
                    }
                    row.text = FromUtf8(r.Str(5));
                    fn(id, row);
                }
            }
        }
        if (tool.empty())
        {
            return true;
        }
        // Spend the budget on the best candidates: the tool blocks in the order `ids` named them.
        std::unordered_map<uint32_t, size_t> rank;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            rank.emplace(static_cast<uint32_t>(ids[i]), i);
        }
        std::sort(tool.begin(), tool.end(), [&rank](const auto& a, const auto& b) { return rank[a.first] < rank[b.first]; });
        std::vector<size_t> missing; // indexes into `tool` the cache did not hold
        for (size_t i = 0; i < tool.size(); ++i)
        {
            if (cache.Get(tool[i].first, tool[i].second.text))
            {
                fn(tool[i].first, tool[i].second);
            }
            else
            {
                missing.push_back(i);
            }
        }
        if (missing.empty())
        {
            return true;
        }
        auto p = con.Prepare("SELECT doc_id, text FROM doc_tool WHERE doc_id IN (SELECT unnest(CAST($1 AS BIGINT[])))");
        for (size_t at = 0; at < missing.size(); at += kToolChunk)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false; // the budget is spent: the remaining (lower-ranked) tool blocks stay unread
            }
            const size_t end = std::min(missing.size(), at + kToolChunk);
            std::vector<int64_t> chunk;
            std::unordered_map<uint32_t, size_t> slot; // doc id -> index into `tool`
            for (size_t k = at; k < end; ++k)
            {
                chunk.push_back(tool[missing[k]].first);
                slot.emplace(tool[missing[k]].first, missing[k]);
            }
            p.BindInt64List(1, chunk);
            auto r = p.Execute();
            while (r.Next())
            {
                const auto it = slot.find(static_cast<uint32_t>(r.Int64(0)));
                if (it == slot.end() || r.IsNull(1))
                {
                    continue;
                }
                auto& [id, row] = tool[it->second];
                row.text = FromUtf8(r.Str(1));
                cache.Put(id, row.text);
                fn(id, row);
            }
        }
        return true;
    }

    std::wstring CardTitle(const SessionCard& c)
    {
        for (const std::wstring* t : { &c.storedTitle, &c.customTitle, &c.aiTitle, &c.legacySummary, &c.firstPrompt })
        {
            if (!t->empty())
            {
                return *t;
            }
        }
        return {};
    }

    SearchEngine::SearchEngine(Store& store) :
        _store(store)
    {
    }

    void SearchEngine::Invalidate()
    {
        std::lock_guard lk{ _mtx };
        _invalid = true;
    }

    std::shared_ptr<const CardSet> SearchEngine::Cards(Connection& con)
    {
        const int64_t stamp = _store.LastWriteMs();
        {
            std::lock_guard lk{ _mtx };
            if (_cards && !_invalid && _builtAt == stamp)
            {
                return _cards;
            }
        }
        auto set = std::make_shared<CardSet>();
        set->version = stamp;
        {
            auto r = con.Query(R"SQL(
SELECT s.sid_key, s.session_id, coalesce(s.short_id, ''), coalesce(t.title, ''), coalesce(s.custom_title, ''), coalesce(s.ai_title, ''),
       coalesce(s.legacy_summary, ''), coalesce(s.first_prompt, ''), coalesce(s.cwd, ''), coalesce(s.cwd_key, ''), coalesce(s.inferred_dir, ''),
       coalesce(s.branch_last, s.branch_first, ''), coalesce(s.forked_from, ''), coalesce(s.model_last, ''),
       coalesce(s.created_ms, 0), coalesce(s.last_activity_ms, s.created_ms, 0), coalesce(s.last_typed_ms, 0), coalesce(s.last_turn_end_ms, 0),
       coalesce(s.prompts_typed, 0), coalesce(s.tool_calls, 0), coalesce(s.size_bytes, 0), coalesce(s.context_tokens, 0),
       f.session_id IS NOT NULL, coalesce(s.transcript, 0) = 0,
       (SELECT string_agg(tag, chr(10) ORDER BY tag_fold) FROM am_tag g WHERE g.session_id = s.session_id)
FROM session s
LEFT JOIN (SELECT session_id, any_value(title) AS title FROM am_title GROUP BY session_id) t ON t.session_id = s.session_id
LEFT JOIN (SELECT DISTINCT session_id FROM am_favorite) f ON f.session_id = s.session_id
ORDER BY s.sid_key)SQL");
            while (r.Next())
            {
                SessionCard c;
                c.sidKey = static_cast<int32_t>(r.Int64(0));
                c.sessionId = FromUtf8(r.Str(1));
                c.shortId = FromUtf8(r.Str(2));
                c.storedTitle = FromUtf8(r.Str(3));
                c.customTitle = FromUtf8(r.Str(4));
                c.aiTitle = FromUtf8(r.Str(5));
                c.legacySummary = FromUtf8(r.Str(6));
                c.firstPrompt = FromUtf8(r.Str(7));
                c.cwd = FromUtf8(r.Str(8));
                c.cwdKey = FromUtf8(r.Str(9));
                c.inferredDir = FromUtf8(r.Str(10));
                c.branch = FromUtf8(r.Str(11));
                c.forkedFrom = FromUtf8(r.Str(12));
                c.model = FromUtf8(r.Str(13));
                c.createdMs = r.Int64(14);
                c.activeMs = r.Int64(15);
                c.lastTypedMs = r.Int64(16);
                c.lastTurnEndMs = r.Int64(17);
                c.msgs = static_cast<int32_t>(r.Int64(18));
                c.tools = static_cast<int32_t>(r.Int64(19));
                c.bytes = r.Int64(20);
                c.contextTokens = r.Int64(21);
                c.favorite = r.Bool(22);
                c.present = r.Bool(23);
                if (!r.IsNull(24))
                {
                    const std::wstring tags = FromUtf8(r.Str(24));
                    size_t at = 0;
                    while (at <= tags.size())
                    {
                        const size_t nl = tags.find(L'\n', at);
                        const std::wstring one = tags.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
                        if (!one.empty())
                        {
                            c.tags.push_back(one);
                        }
                        if (nl == std::wstring::npos)
                        {
                            break;
                        }
                        at = nl + 1;
                    }
                }
                c.title = CardTitle(c);
                c.idF = FoldLower(c.sessionId);
                c.forkF = FoldLower(c.forkedFrom);
                c.cwdF = FoldLower(c.cwd);
                c.branchF = FoldLower(c.branch);
                c.titleF[0] = FoldLower(c.storedTitle);
                c.titleF[1] = FoldLower(c.customTitle);
                c.titleF[2] = FoldLower(c.aiTitle);
                c.titleF[3] = FoldLower(c.legacySummary);
                c.titleF[4] = FoldLower(c.firstPrompt);
                set->bySid[c.sidKey] = set->cards.size();
                set->cards.push_back(std::move(c));
            }
        }
        {
            // The touched folders and file names, folded and distinct per session (the 📁 / 📄 haystacks).
            auto r = con.Query("SELECT sid_key, path FROM path_access ORDER BY sid_key");
            int32_t current = -1;
            SessionCard* card = nullptr;
            std::unordered_set<std::wstring> dirs, leaves;
            const auto flush = [&]() {
                if (card)
                {
                    card->dirsF.assign(dirs.begin(), dirs.end());
                    card->leavesF.assign(leaves.begin(), leaves.end());
                }
                dirs.clear();
                leaves.clear();
            };
            while (r.Next())
            {
                const int32_t sid = static_cast<int32_t>(r.Int64(0));
                if (sid != current)
                {
                    flush();
                    current = sid;
                    const auto it = set->bySid.find(sid);
                    card = it == set->bySid.end() ? nullptr : &set->cards[it->second];
                }
                if (!card)
                {
                    continue;
                }
                const std::wstring lowered = FoldLower(FromUtf8(r.Str(1)));
                if (const auto d = DirPartOf(lowered); !d.empty())
                {
                    dirs.emplace(d);
                }
                leaves.emplace(LeafPartOf(lowered));
            }
            flush();
        }
        std::lock_guard lk{ _mtx };
        _cards = set;
        _builtAt = stamp;
        _invalid = false;
        return _cards;
    }

    SearchResult SearchEngine::Search(Connection& con, const SearchRequest& req)
    {
        SearchResult res;
        // The step timer behind res.timings: each lap appends the time since the previous one, so the
        // service log can say which step a slow search spent its time in.
        auto lastLap = std::chrono::steady_clock::now();
        const auto lap = [&res, &lastLap](const wchar_t* step) {
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - lastLap).count();
            lastLap = now;
            wchar_t buf[96];
            swprintf_s(buf, L"%s%s %.1f ms", res.timings.empty() ? L"" : L", ", step, ms);
            res.timings += buf;
        };
        const auto cards = Cards(con);
        res.cards = cards;
        lap(L"cards");
        const auto open = _store.OpenSessions();
        const std::wstring dirF = FoldLower(req.dir);
        const std::wstring branchF = FoldLower(req.branch);
        const std::wstring tagF = FoldLower(req.tag);

        // ----- the window and the row filters (session-level, in memory) -----
        std::vector<size_t> cand;
        cand.reserve(cards->cards.size());
        for (size_t i = 0; i < cards->cards.size(); ++i)
        {
            const SessionCard& c = cards->cards[i];
            const int64_t active = c.activeMs ? c.activeMs : c.createdMs;
            if ((req.afterMs && active < req.afterMs) || (req.beforeMs && active > req.beforeMs))
            {
                continue;
            }
            if (!dirF.empty() && !MatchesPathQuery(c.cwdF, dirF, false) && !MatchesPathQuery(FoldLower(c.inferredDir), dirF, false))
            {
                continue;
            }
            if (!branchF.empty() && c.branchF.find(branchF) == std::wstring::npos)
            {
                continue;
            }
            if (!tagF.empty())
            {
                bool has = false;
                for (const auto& t : c.tags)
                {
                    has = has || FoldLower(t) == tagF;
                }
                if (!has)
                {
                    continue;
                }
            }
            if (req.favorite >= 0 && c.favorite != (req.favorite == 1))
            {
                continue;
            }
            if (req.open >= 0 && (open.count(c.sessionId) != 0) != (req.open == 1))
            {
                continue;
            }
            cand.push_back(i);
        }
        res.searched = static_cast<int64_t>(cand.size());

        const auto terms = ParseSessionQuery(req.query);
        std::vector<const SearchTerm*> textTerms;
        std::vector<std::wstring> guidIds;
        for (const auto& t : terms)
        {
            if (t.isGuid)
            {
                std::wstring id = t.textLower;
                if (id.size() >= 2 && id.front() == L'{')
                {
                    id = id.substr(1, id.size() - 2);
                }
                guidIds.push_back(id);
            }
            else
            {
                textTerms.push_back(&t);
            }
        }

        // tier + content facts per candidate index
        struct Acc
        {
            uint8_t tier{ 255 };
            int64_t hits{ 0 };
            double score{ 0 };
            std::vector<std::pair<double, uint32_t>> docs; // (score, doc id) of matched blocks, for snippets
            std::vector<std::wstring> historyTexts; // history.jsonl prompts that matched (no doc id)
        };
        std::unordered_map<int32_t, Acc> acc;

        if (terms.empty())
        {
            for (const size_t i : cand)
            {
                acc[cards->cards[i].sidKey].tier = 4;
            }
        }
        else
        {
            // ----- T0 / T1: session-level AND over the names and places (today's fast phase) -----
            for (const size_t i : cand)
            {
                const SessionCard& c = cards->cards[i];
                bool nameAll = true, pathAll = true;
                for (const auto& t : terms)
                {
                    const bool fz = TermIsFuzzy(t, req.fuzzy);
                    bool name = t.isGuid && (t.textLower == c.idF || (!c.forkF.empty() && t.textLower == c.forkF));
                    if (!name && req.scopeTitle)
                    {
                        for (const auto& h : c.titleF)
                        {
                            if (MatchesQueryText(h, t.textLower, fz))
                            {
                                name = true;
                                break;
                            }
                        }
                        name = name || MatchesQueryText(c.branchF, t.textLower, fz);
                    }
                    bool place = name || MatchesPathQuery(c.cwdF, t.textLower, fz); // the working folder: always a target
                    if (!place && req.scopeDirs)
                    {
                        for (const auto& d : c.dirsF)
                        {
                            if (MatchesPathQuery(d, t.textLower, fz))
                            {
                                place = true;
                                break;
                            }
                        }
                    }
                    if (!place && req.scopeFiles)
                    {
                        for (const auto& l : c.leavesF)
                        {
                            if (MatchesQueryText(l, t.textLower, fz))
                            {
                                place = true;
                                break;
                            }
                        }
                    }
                    nameAll = nameAll && name;
                    pathAll = pathAll && place;
                    if (!pathAll)
                    {
                        break;
                    }
                }
                if (nameAll)
                {
                    acc[c.sidKey].tier = 0;
                }
                else if (pathAll)
                {
                    acc[c.sidKey].tier = 1;
                }
            }
            lap(L"names");

            // ----- the content tiers need at least one text term -----
            if (!textTerms.empty() && !(req.cancel && req.cancel()))
            {
                // Restrict the content scans to the candidate sessions (and to the sessions a whole-id term names).
                std::vector<int32_t> scope;
                bool scoped = cand.size() < cards->cards.size() || !guidIds.empty();
                for (const size_t i : cand)
                {
                    const SessionCard& c = cards->cards[i];
                    bool ok = true;
                    for (const auto& g : guidIds)
                    {
                        ok = ok && (c.sessionId == g || c.forkedFrom == g);
                    }
                    if (ok)
                    {
                        scope.push_back(c.sidKey);
                    }
                }
                const std::string scopeSql = scoped ? std::string{ " AND sid_key IN (SELECT unnest(CAST($S AS BIGINT[])))" } : std::string{};

                // The folded forms of the text terms, the index's fold (ICU) for the content tiers.
                std::vector<std::wstring> folded;
                std::vector<bool> fuzzyTerm;
                for (const auto* t : textTerms)
                {
                    folded.push_back(FoldForIndex(t->text));
                    fuzzyTerm.push_back(TermIsFuzzy(*t, req.fuzzy));
                }
                // SQL conditions over a folded text column, one per text term ($1..$n), and their values.
                const auto termConds = [&](const char* column) {
                    std::string sql;
                    for (size_t k = 0; k < folded.size(); ++k)
                    {
                        if (k)
                        {
                            sql += " AND ";
                        }
                        sql += fuzzyTerm[k] ? std::string{ "regexp_matches(" } + column + ", $" + std::to_string(k + 1) + ")"
                                            : std::string{ "contains(" } + column + ", $" + std::to_string(k + 1) + ")";
                    }
                    return sql;
                };
                const auto bindTerms = [&](Prepared& p) {
                    for (size_t k = 0; k < folded.size(); ++k)
                    {
                        // A fuzzy term is today's rg pattern (the characters in order, a lazy gap between them).
                        // `(?s)` lets the gap cross a line break: RE2's `.` stops at '\n' by default, while today's
                        // in-process subsequence match (MatchesQueryText) does not care about lines.
                        const std::wstring v = fuzzyTerm[k] ? L"(?s)" + BuildSearchRegex(folded[k], true) : folded[k];
                        p.Bind(k + 1, std::string_view{ ToUtf8(v) });
                    }
                };
                const auto withScope = [&](std::string sql) {
                    const size_t at = sql.find("$S");
                    if (at != std::string::npos)
                    {
                        sql.replace(at, 2, "$" + std::to_string(folded.size() + 1));
                    }
                    return sql;
                };

                // ----- T2: typed prompts (the prompt tier) and history.jsonl -----
                if (req.scopePrompt)
                {
                    auto p = con.Prepare(withScope("SELECT doc_id, sid_key FROM prompt WHERE " + termConds("text_fold") + scopeSql));
                    bindTerms(p);
                    if (scoped)
                    {
                        p.BindInt64List(folded.size() + 1, SidList(scope));
                    }
                    auto r = p.Execute();
                    while (r.Next())
                    {
                        auto& a = acc[static_cast<int32_t>(r.Int64(1))];
                        a.tier = std::min<uint8_t>(a.tier, 2);
                        ++a.hits;
                        a.docs.emplace_back(1.0, static_cast<uint32_t>(r.Int64(0)));
                    }
                    lap(L"prompts");
                    // history.jsonl's prompts (they outlive a swept transcript), by session id.
                    std::unordered_map<std::wstring, int32_t> sidOf;
                    for (const size_t i : cand)
                    {
                        sidOf[cards->cards[i].sessionId] = cards->cards[i].sidKey;
                    }
                    auto h = con.Prepare("SELECT session_id, text FROM history_prompt WHERE " + termConds("text_fold"));
                    bindTerms(h);
                    auto hr = h.Execute();
                    while (hr.Next())
                    {
                        const auto it = sidOf.find(FromUtf8(hr.Str(0)));
                        if (it == sidOf.end() || (scoped && std::find(scope.begin(), scope.end(), it->second) == scope.end()))
                        {
                            continue;
                        }
                        auto& a = acc[it->second];
                        if (!a.docs.empty())
                        {
                            continue; // the transcript's own prompt matched already: history.jsonl repeats it
                        }
                        a.historyTexts.push_back(FromUtf8(hr.Str(1)));
                        a.tier = std::min<uint8_t>(a.tier, 2);
                        ++a.hits;
                    }
                    lap(L"history");
                }

                // ----- T3: everything else, through the index (cold) and a scan (hot) -----
                // T3 reads Claude's side (replies, thinking, system text, tool calls and output); the subagent
                // scope only widens where those roles are read, so it alone selects nothing.
                const bool anyAgentScope = req.scopeReply || req.scopeTool;
                bool shortOnly = true;
                for (const auto& f : folded)
                {
                    shortOnly = shortOnly && f.size() <= 2;
                }
                if (anyAgentScope && shortOnly)
                {
                    res.note = L"terms of 1-2 characters search names, folders and prompts only";
                }
                else if (anyAgentScope && !(req.cancel && req.cancel()))
                {
                    const Dictionary& dict = _store.Index().Dict();
                    const double n = static_cast<double>(std::max<uint64_t>(1, dict.DocCount()));
                    const double avgdl = std::max(1.0, dict.AvgDocLength());
                    // One posting GROUP per token group; a doc must appear in every group (AND).
                    struct Group
                    {
                        std::vector<uint32_t> ids;
                        double idf{ 0 };
                    };
                    std::vector<Group> groups;
                    std::vector<size_t> verifyTerms; // indexes into folded: rule-2 terms checked on the text
                    bool impossible = false;
                    const auto expand = [&](std::wstring_view token, bool prefix, bool suffix, bool fuzzy) {
                        Group g;
                        if (fuzzy)
                        {
                            g.ids = dict.TermsWithSubsequence(token, kMaxExpansion);
                        }
                        else
                        {
                            g.ids = dict.TermsContaining(token, kMaxExpansion, prefix, suffix);
                        }
                        if (g.ids.size() >= kMaxExpansion)
                        {
                            res.partial = true;
                        }
                        uint64_t df = 0;
                        for (const uint32_t id : g.ids)
                        {
                            df += dict.Stats(id).df;
                        }
                        const double dfd = std::min<double>(static_cast<double>(df), n);
                        g.idf = std::log(1.0 + (n - dfd + 0.5) / (dfd + 0.5));
                        if (g.ids.empty())
                        {
                            impossible = true; // no term holds this token: no block can match the AND
                        }
                        groups.push_back(std::move(g));
                    };
                    for (size_t k = 0; k < textTerms.size(); ++k)
                    {
                        if (folded[k].size() <= 2)
                        {
                            // D8: a 1-2 character term cannot use the index (it would expand to most of the
                            // dictionary). In a query with a longer term it is checked on the text of the blocks
                            // the longer terms found, so the AND still holds.
                            verifyTerms.push_back(k);
                            continue;
                        }
                        const auto qt = SessionDbTokenizer::TokenizeQueryTerm(textTerms[k]->text, ServiceClassifier());
                        if (qt.tokens.empty())
                        {
                            verifyTerms.push_back(k); // separators only ("::="): checked on the other terms' candidates
                            continue;
                        }
                        if (!qt.hasSeparator && qt.tokens.size() == 1)
                        {
                            expand(qt.tokens[0], false, false, fuzzyTerm[k]); // rule 1: exact through the dictionary
                            continue;
                        }
                        // Rule 2: the tokens narrow the blocks, the whole term is then verified on their text.
                        verifyTerms.push_back(k);
                        for (size_t j = 0; j < qt.tokens.size(); ++j)
                        {
                            if (fuzzyTerm[k])
                            {
                                // A fuzzy term with separators: each token's characters in order inside one block
                                // token (D9's per-term rule), the whole term as a subsequence at verification.
                                expand(qt.tokens[j], false, false, true);
                                continue;
                            }
                            // The first token is followed by a separator, so it must END a block token (suffix);
                            // the last is preceded by one, so it must START one (prefix); a middle token is a whole
                            // token (both). A lone token between separators may be any part of a block token.
                            const bool first = j == 0, last = j + 1 == qt.tokens.size();
                            expand(qt.tokens[j], /*prefix*/ !first, /*suffix*/ !last, false);
                        }
                    }
                    lap(L"expand");
                    const std::string scopeFilter = ScopeFilter(req);
                    if (groups.empty())
                    {
                        // Every term is verify-only (short, or separators only): no posting can narrow the cold
                        // blocks, and verifying all of them would read the whole store. Only the hot blocks
                        // (written since the last merge) are searched; say so.
                        res.partial = true;
                        res.note = L"no term of this query can use the index; only the newest messages were searched";
                    }

                    // --- cold blocks: the postings of every group, joined on the block ---
                    struct DocHit
                    {
                        int32_t sid;
                        uint32_t doc;
                        double score;
                    };
                    std::vector<DocHit> hits;
                    if (!groups.empty() && !impossible)
                    {
                        std::string sql = "WITH ";
                        for (size_t g = 0; g < groups.size(); ++g)
                        {
                            if (g)
                            {
                                sql += ", ";
                            }
                            sql += "g" + std::to_string(g) + " AS (SELECT doc_id, max(tf) AS tf FROM post WHERE term_id IN (SELECT unnest(CAST($" +
                                   std::to_string(g + 1) + " AS UINTEGER[]))) GROUP BY doc_id)";
                        }
                        sql += " SELECT d.doc_id, d.sid_key, d.tok_len";
                        for (size_t g = 0; g < groups.size(); ++g)
                        {
                            sql += ", g" + std::to_string(g) + ".tf";
                        }
                        sql += " FROM g0";
                        for (size_t g = 1; g < groups.size(); ++g)
                        {
                            sql += " JOIN g" + std::to_string(g) + " USING (doc_id)";
                        }
                        sql += " JOIN doc d USING (doc_id) WHERE " + scopeFilter + " AND d.doc_id NOT IN (SELECT doc_id FROM tomb)";
                        if (scoped)
                        {
                            sql += " AND d.sid_key IN (SELECT unnest(CAST($" + std::to_string(groups.size() + 1) + " AS BIGINT[])))";
                        }
                        auto p = con.Prepare(sql);
                        for (size_t g = 0; g < groups.size(); ++g)
                        {
                            p.BindInt64List(g + 1, std::vector<int64_t>(groups[g].ids.begin(), groups[g].ids.end()));
                        }
                        if (scoped)
                        {
                            p.BindInt64List(groups.size() + 1, SidList(scope));
                        }
                        auto r = p.Execute();
                        while (r.Next())
                        {
                            const double len = std::max<double>(1.0, static_cast<double>(r.Int64(2)));
                            double score = 0;
                            for (size_t g = 0; g < groups.size(); ++g)
                            {
                                const double tf = static_cast<double>(r.Int64(3 + g));
                                score += groups[g].idf * tf * (kK1 + 1.0) / (tf + kK1 * (1.0 - kB + kB * len / avgdl));
                            }
                            hits.push_back(DocHit{ static_cast<int32_t>(r.Int64(1)), static_cast<uint32_t>(r.Int64(0)), score });
                        }
                    }
                    lap(L"postings");
                    // Rule-2 terms: verify each candidate's text (only those blocks are decompressed).
                    if (!verifyTerms.empty() && !hits.empty())
                    {
                        // Best first: the tool-text budget (and the cap) then cut the least relevant candidates.
                        const size_t take = std::min(hits.size(), kMaxVerify);
                        std::partial_sort(hits.begin(), hits.begin() + static_cast<ptrdiff_t>(take), hits.end(),
                                          [](const DocHit& a, const DocHit& b) { return a.score > b.score; });
                        if (hits.size() > kMaxVerify)
                        {
                            hits.resize(kMaxVerify);
                            res.partial = true;
                        }
                        std::vector<int64_t> ids;
                        for (const auto& h : hits)
                        {
                            ids.push_back(h.doc);
                        }
                        std::unordered_set<uint32_t> keep;
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<int64_t>(0, req.toolTextBudgetMs));
                        const bool complete = ReadBlockTexts(con, ids, false, _toolTexts, deadline, [&](uint32_t docId, const BlockText& row) {
                            const std::wstring text = FoldForIndex(row.text);
                            bool all = true;
                            for (const size_t k : verifyTerms)
                            {
                                // Today's rules over the block's folded text: a substring, or for (F) the term's
                                // characters in order (MatchesQueryText's subsequence).
                                all = all && (fuzzyTerm[k] ? MatchesQueryText(text, folded[k], true) : text.find(folded[k]) != std::wstring::npos);
                            }
                            if (all)
                            {
                                keep.insert(docId);
                            }
                        });
                        if (!complete)
                        {
                            res.partial = true; // tool blocks the budget left unread are not counted as hits
                        }
                        hits.erase(std::remove_if(hits.begin(), hits.end(), [&keep](const DocHit& h) { return keep.count(h.doc) == 0; }), hits.end());
                    }
                    lap(L"verify");
                    // --- hot blocks: a scan of the pre-folded text with the same AND ---
                    {
                        auto p = con.Prepare(withScope("SELECT doc_id, sid_key FROM doc_hot WHERE " + termConds("text_fold") + " AND " + scopeFilter + scopeSql));
                        bindTerms(p);
                        if (scoped)
                        {
                            p.BindInt64List(folded.size() + 1, SidList(scope));
                        }
                        auto r = p.Execute();
                        double idfSum = 0;
                        for (const auto& g : groups)
                        {
                            idfSum += g.idf;
                        }
                        while (r.Next())
                        {
                            hits.push_back(DocHit{ static_cast<int32_t>(r.Int64(1)), static_cast<uint32_t>(r.Int64(0)), idfSum });
                        }
                    }
                    lap(L"hot");
                    for (const auto& h : hits)
                    {
                        auto& a = acc[h.sid];
                        a.tier = std::min<uint8_t>(a.tier, 3);
                        ++a.hits;
                        a.score = std::max(a.score, h.score);
                        a.docs.emplace_back(h.score, h.doc);
                    }
                }
            }
        }

        // ----- rank, page, snippets -----
        std::vector<std::pair<int32_t, Acc*>> rows;
        rows.reserve(acc.size());
        for (auto& [sid, a] : acc)
        {
            if (a.tier <= 4 && cards->bySid.count(sid))
            {
                rows.emplace_back(sid, &a);
            }
        }
        res.matched = static_cast<int64_t>(rows.size());
        const auto cardOf = [&cards](int32_t sid) -> const SessionCard& { return cards->cards[cards->bySid.at(sid)]; };
        std::sort(rows.begin(), rows.end(), [&](const auto& x, const auto& y) {
            const SessionCard& a = cardOf(x.first);
            const SessionCard& b = cardOf(y.first);
            const int64_t aActive = a.activeMs ? a.activeMs : a.createdMs;
            const int64_t bActive = b.activeMs ? b.activeMs : b.createdMs;
            switch (req.sort)
            {
            case SearchRequest::Sort::Active:
                return aActive != bActive ? aActive > bActive : x.first > y.first;
            case SearchRequest::Sort::Created:
                return a.createdMs != b.createdMs ? a.createdMs > b.createdMs : x.first > y.first;
            case SearchRequest::Sort::Relevance:
            default:
                if (x.second->tier != y.second->tier)
                {
                    return x.second->tier < y.second->tier;
                }
                if (x.second->tier >= 2 && x.second->score != y.second->score)
                {
                    return x.second->score > y.second->score;
                }
                return aActive != bActive ? aActive > bActive : x.first > y.first;
            }
        });
        lap(L"rank");
        const size_t from = std::min(req.offset, rows.size());
        const size_t to = std::min(rows.size(), from + std::max<size_t>(1, req.limit));
        res.hasMore = to < rows.size();

        // Snippets for the page's rows only (§6.6): the best matched blocks, in conversation order.
        std::vector<int64_t> snippetDocs;
        for (size_t i = from; i < to; ++i)
        {
            auto& a = *rows[i].second;
            std::sort(a.docs.begin(), a.docs.end(), [](const auto& p, const auto& q) { return p.first > q.first; });
            for (size_t k = 0; k < a.docs.size() && k < req.snippetsPerRow; ++k)
            {
                snippetDocs.push_back(a.docs[k].second);
            }
        }
        std::unordered_map<uint32_t, BlockText> texts;
        // Snippets get their own share of the tool-text budget (half of it), so a verification that spent the
        // whole budget still leaves the page its snippets; a tool block left unread shows without one.
        const auto snippetDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<int64_t>(0, req.toolTextBudgetMs / 2));
        ReadBlockTexts(con, snippetDocs, true, _toolTexts, snippetDeadline, [&texts](uint32_t docId, BlockText& row) { texts[docId] = std::move(row); });
        lap(L"snippet text");
        const std::wstring snippetTerm = textTerms.empty() ? std::wstring{} : textTerms.front()->textLower;
        const bool snippetFuzzy = !textTerms.empty() && TermIsFuzzy(*textTerms.front(), req.fuzzy);
        for (size_t i = from; i < to; ++i)
        {
            SearchRow row;
            row.sidKey = rows[i].first;
            const Acc& a = *rows[i].second;
            row.tier = a.tier;
            row.hits = a.hits;
            row.score = a.score;
            std::vector<SearchSnippet> snips;
            for (size_t k = 0; k < a.docs.size() && snips.size() < req.snippetsPerRow; ++k)
            {
                const auto it = texts.find(a.docs[k].second);
                if (it == texts.end())
                {
                    continue;
                }
                SearchSnippet s;
                s.fileId = it->second.fileId;
                s.lineNo = it->second.line;
                s.block = it->second.block;
                s.role = it->second.role;
                s.subagent = (it->second.flags & kDfSubagent) != 0;
                s.text = MakeSnippet(it->second.text, snippetTerm, snippetFuzzy, req.snippetChars);
                snips.push_back(std::move(s));
            }
            for (size_t k = 0; k < a.historyTexts.size() && snips.size() < req.snippetsPerRow; ++k)
            {
                SearchSnippet s;
                s.role = kRolePrompt;
                s.history = true;
                s.text = MakeSnippet(a.historyTexts[k], snippetTerm, snippetFuzzy, req.snippetChars);
                snips.push_back(std::move(s));
            }
            // Conversation order on screen.
            std::stable_sort(snips.begin(), snips.end(), [](const SearchSnippet& x, const SearchSnippet& y) {
                return x.lineNo != y.lineNo ? x.lineNo < y.lineNo : x.block < y.block;
            });
            row.snippets = std::move(snips);
            res.rows.push_back(std::move(row));
        }
        lap(L"snippets");
        return res;
    }
}
