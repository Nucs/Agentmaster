// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's tool registry (see SessionDbTools.h; the contracts are
// SESSIONS_DB.md §8.4.3).

#include "SessionDbTools.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator> // std::size
#include <map>
#include <set>
#include <stdexcept> // std::logic_error (a definition without a handler)
#include <unordered_map>
#include <unordered_set>

#include "duckdb.h"

#include "DuckDbWrap.h"
#include "SessionDbExtract.h"
#include "SessionDbIndex.h"
#include "SessionDbLog.h"
#include "SessionDbSearch.h"
#include "SessionDbStore.h"

#include "../ClaudeSpawn.h" // LogSwallowedException
#include "../SessionDbProtocol.h"
#include "../SessionDbToolDefs.h" // the registry's names, descriptions, schemas and budgets (shared with the app)
#include "../SessionDbTokenizer.h"
#include "../SessionSearch.h" // FoldLower, MatchesPathQuery

namespace Agentmaster::SessionDb
{
    using json = nlohmann::json;
    namespace Proto = SessionDbProtocol;

    std::string Utf8(std::wstring_view text)
    {
        return ToUtf8(text);
    }

    std::wstring Wide(std::string_view utf8)
    {
        return FromUtf8(utf8);
    }

    std::string LocalTime(int64_t unixMs)
    {
        if (unixMs <= 0)
        {
            return {};
        }
        const time_t t = static_cast<time_t>(unixMs / 1000);
        tm lt{};
        if (localtime_s(&lt, &t) != 0)
        {
            return {};
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        return buf;
    }

    bool ParseLocalDate(std::string_view text, bool endOfDay, int64_t& unixMs)
    {
        if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        {
            return false;
        }
        int parts[3]{};
        const size_t starts[3]{ 0, 5, 8 };
        const size_t lens[3]{ 4, 2, 2 };
        for (int k = 0; k < 3; ++k)
        {
            for (size_t i = 0; i < lens[k]; ++i)
            {
                const char c = text[starts[k] + i];
                if (c < '0' || c > '9')
                {
                    return false;
                }
                parts[k] = parts[k] * 10 + (c - '0');
            }
        }
        const int y = parts[0], m = parts[1], d = parts[2];
        if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31)
        {
            return false;
        }
        tm lt{};
        lt.tm_year = y - 1900;
        lt.tm_mon = m - 1;
        lt.tm_mday = endOfDay ? d + 1 : d; // the end of a day is the next day's midnight, minus one ms
        lt.tm_isdst = -1; // let the C runtime decide daylight saving for that date
        const time_t t = mktime(&lt);
        if (t == static_cast<time_t>(-1))
        {
            return false;
        }
        // mktime normalizes Feb 30 into March: such a date is not a date.
        tm check{};
        const time_t start = endOfDay ? t - 1 : t;
        if (localtime_s(&check, &start) != 0 || check.tm_mday != d || check.tm_mon != m - 1)
        {
            return false;
        }
        unixMs = static_cast<int64_t>(t) * 1000 - (endOfDay ? 1 : 0);
        return true;
    }

    namespace
    {
        // The result cap of §8.4.2 (compact JSON). Rows are added while the running size stays under it.
        constexpr size_t kResultCap = 64 * 1024;
        // A message block's text is cut at this many characters unless the call asks for `full`.
        constexpr size_t kBlockChars = 2000;
        // The dictionary expansion cap for a grep literal (a common word expands to many terms).
        constexpr size_t kGrepExpansion = 60000;

        const char* const kRoleNames[] = { "", "prompt", "reply", "thinking", "tool_call", "tool_result", "system", "queued" };

        // The role's name in results ("" for an unknown code).
        std::string RoleName(uint8_t role)
        {
            return role < std::size(kRoleNames) ? kRoleNames[role] : std::string{};
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s)
            {
                c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        std::string Join(std::initializer_list<const char*> items)
        {
            std::string out;
            for (const char* i : items)
            {
                out += out.empty() ? "" : ", ";
                out += i;
            }
            return out;
        }

        ToolError BadRequest(std::string message)
        {
            return ToolError{ Proto::kErrBadRequest, std::move(message) };
        }

        // --- argument readers (each error says what is valid) ---

        // additionalProperties: false.
        void Allow(const json& a, std::initializer_list<const char*> keys)
        {
            if (a.is_null())
            {
                return;
            }
            if (!a.is_object())
            {
                throw BadRequest("The arguments must be a JSON object.");
            }
            for (auto it = a.begin(); it != a.end(); ++it)
            {
                bool known = false;
                for (const char* k : keys)
                {
                    known = known || it.key() == k;
                }
                if (!known)
                {
                    throw BadRequest("Unknown parameter \"" + it.key() + "\". Valid parameters: " + Join(keys) + ".");
                }
            }
        }

        bool Has(const json& a, const char* key)
        {
            return a.is_object() && a.contains(key) && !a[key].is_null();
        }

        std::string Str(const json& a, const char* key, size_t maxLen, const std::string& def = {})
        {
            if (!Has(a, key))
            {
                return def;
            }
            if (!a[key].is_string())
            {
                throw BadRequest(std::string{ key } + " must be a string.");
            }
            std::string v = a[key].get<std::string>();
            if (v.size() > maxLen)
            {
                throw BadRequest(std::string{ key } + " must be at most " + std::to_string(maxLen) + " characters.");
            }
            return v;
        }

        int64_t Int(const json& a, const char* key, int64_t lo, int64_t hi, int64_t def)
        {
            if (!Has(a, key))
            {
                return def;
            }
            const json& v = a[key];
            int64_t n = 0;
            if (v.is_number_integer())
            {
                n = v.get<int64_t>();
            }
            else if (v.is_number_float() && std::floor(v.get<double>()) == v.get<double>())
            {
                n = static_cast<int64_t>(v.get<double>());
            }
            else
            {
                throw BadRequest(std::string{ key } + " must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi) + ".");
            }
            if (n < lo || n > hi)
            {
                throw BadRequest(std::string{ key } + " must be from " + std::to_string(lo) + " to " + std::to_string(hi) + " (got " + std::to_string(n) + ").");
            }
            return n;
        }

        bool Bool(const json& a, const char* key, bool def)
        {
            if (!Has(a, key))
            {
                return def;
            }
            if (!a[key].is_boolean())
            {
                throw BadRequest(std::string{ key } + " must be true or false.");
            }
            return a[key].get<bool>();
        }

        // -1 absent, 0 false, 1 true.
        int TriBool(const json& a, const char* key)
        {
            return Has(a, key) ? (Bool(a, key, false) ? 1 : 0) : -1;
        }

        std::string Enum(const json& a, const char* key, std::initializer_list<const char*> allowed, const std::string& def)
        {
            const std::string v = Str(a, key, 64, def);
            for (const char* x : allowed)
            {
                if (v == x)
                {
                    return v;
                }
            }
            throw BadRequest("Unknown " + std::string{ key } + " \"" + v + "\". Valid values: " + Join(allowed) + ".");
        }

        std::vector<std::string> EnumList(const json& a, const char* key, std::initializer_list<const char*> allowed, std::vector<std::string> def)
        {
            if (!Has(a, key))
            {
                return def;
            }
            if (!a[key].is_array())
            {
                throw BadRequest(std::string{ key } + " must be an array of strings. Valid values: " + Join(allowed) + ".");
            }
            std::vector<std::string> out;
            for (const json& v : a[key])
            {
                if (!v.is_string())
                {
                    throw BadRequest(std::string{ key } + " must be an array of strings. Valid values: " + Join(allowed) + ".");
                }
                const std::string s = v.get<std::string>();
                bool ok = false;
                for (const char* x : allowed)
                {
                    ok = ok || s == x;
                }
                if (!ok)
                {
                    throw BadRequest("Unknown value \"" + s + "\" in " + key + ". Valid values: " + Join(allowed) + ".");
                }
                if (std::find(out.begin(), out.end(), s) == out.end())
                {
                    out.push_back(s);
                }
            }
            return out;
        }

        bool Contains(const std::vector<std::string>& list, const char* v)
        {
            return std::find(list.begin(), list.end(), v) != list.end();
        }

        // --- the window (days / after / before) ---

        struct Window
        {
            int64_t afterMs{ 0 };
            int64_t beforeMs{ 0 };
            std::string label;
        };

        std::string LocalDate(int64_t unixMs)
        {
            const std::string t = LocalTime(unixMs);
            return t.size() >= 10 ? t.substr(0, 10) : t;
        }

        // Read `days` (default `defDays`; 0 = no default window), `after` (overrides days) and `before`.
        Window ReadWindow(const json& a, int64_t nowMs, int64_t defDays)
        {
            Window w;
            if (Has(a, "after"))
            {
                if (!ParseLocalDate(Str(a, "after", 10), false, w.afterMs))
                {
                    throw BadRequest("after must be a local date YYYY-MM-DD (for example " + LocalDate(nowMs) + ").");
                }
            }
            else
            {
                const int64_t days = Int(a, "days", 1, 3650, defDays);
                if (days > 0)
                {
                    w.afterMs = nowMs - days * 86400000LL;
                    w.label = "last " + std::to_string(days) + (days == 1 ? " day" : " days");
                }
            }
            if (Has(a, "before"))
            {
                if (!ParseLocalDate(Str(a, "before", 10), true, w.beforeMs))
                {
                    throw BadRequest("before must be a local date YYYY-MM-DD (for example " + LocalDate(nowMs) + ").");
                }
            }
            if (w.afterMs && w.beforeMs && w.afterMs > w.beforeMs)
            {
                throw BadRequest("after is later than before.");
            }
            if (Has(a, "after") || Has(a, "before"))
            {
                w.label = Has(a, "after") && Has(a, "before") ? LocalDate(w.afterMs) + " to " + LocalDate(w.beforeMs)
                          : Has(a, "after")                    ? "since " + LocalDate(w.afterMs)
                                                               : w.label + (w.label.empty() ? "" : ", ") + "until " + LocalDate(w.beforeMs);
            }
            if (w.label.empty())
            {
                w.label = "all time";
            }
            return w;
        }

        // --- sessions and refs ---

        SessionKey Resolve(ToolContext& ctx, const std::string& id)
        {
            std::wstring w = Wide(id);
            while (!w.empty() && iswspace(w.back()))
            {
                w.pop_back();
            }
            while (!w.empty() && iswspace(w.front()))
            {
                w.erase(0, 1);
            }
            if (w.size() >= 2 && w.front() == L'{' && w.back() == L'}')
            {
                w = w.substr(1, w.size() - 2);
            }
            if (w.size() < 8)
            {
                throw BadRequest("Session id \"" + id + "\" is too short: use 8 or more characters of an id from a search result.");
            }
            int matches = 0;
            const auto key = ctx.store.ResolveSession(w, matches);
            if (!key)
            {
                if (matches > 1)
                {
                    throw ToolError{ Proto::kErrConflict, "\"" + id + "\" is shared by " + std::to_string(matches) + " sessions. Give more characters of the id." };
                }
                throw ToolError{ Proto::kErrNotFound, "Unknown session \"" + id + "\". Use an id from a search result (8 or more characters)." };
            }
            return *key;
        }

        // A block ref: <id>[/<agent>]:<line>[.<block>].
        struct Ref
        {
            std::string id;
            std::string agent;
            int32_t line{ 0 };
            int32_t block{ 0 };
        };

        bool ParseRef(std::string_view s, Ref& r)
        {
            const size_t colon = s.rfind(':');
            if (colon == std::string_view::npos || colon == 0)
            {
                return false;
            }
            std::string_view head = s.substr(0, colon);
            std::string_view tail = s.substr(colon + 1);
            const size_t slash = head.find('/');
            r.id = std::string{ head.substr(0, slash) };
            r.agent = slash == std::string_view::npos ? std::string{} : std::string{ head.substr(slash + 1) };
            const size_t dot = tail.find('.');
            const std::string_view lineText = tail.substr(0, dot);
            const std::string_view blockText = dot == std::string_view::npos ? std::string_view{} : tail.substr(dot + 1);
            if (lineText.empty() || lineText.size() > 9 || (dot != std::string_view::npos && (blockText.empty() || blockText.size() > 5)))
            {
                return false;
            }
            r.line = 0;
            for (const char c : lineText)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                r.line = r.line * 10 + (c - '0');
            }
            r.block = 0;
            for (const char c : blockText)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                r.block = r.block * 10 + (c - '0');
            }
            return r.line > 0 && !r.id.empty();
        }

        std::string MakeRef(const std::wstring& shortId, const std::wstring& agent, int32_t line, int32_t block)
        {
            std::string r = Utf8(shortId);
            if (!agent.empty())
            {
                r += "/" + Utf8(agent);
            }
            r += ":" + std::to_string(line);
            if (block > 0)
            {
                r += "." + std::to_string(block);
            }
            return r;
        }

        // --- text ---

        // The first `maxChars` characters as UTF-8, with an ellipsis when cut (never inside a surrogate pair).
        std::string Clip(std::wstring_view text, size_t maxChars, bool& truncated)
        {
            truncated = text.size() > maxChars;
            if (!truncated)
            {
                return Utf8(text);
            }
            size_t n = maxChars;
            if (n > 0 && IS_HIGH_SURROGATE(text[n - 1]))
            {
                --n;
            }
            return Utf8(text.substr(0, n)) + "\xE2\x80\xA6";
        }

        // One line, spaces collapsed (for snippets).
        std::wstring OneLine(std::wstring_view text)
        {
            std::wstring out;
            out.reserve(text.size());
            bool space = false;
            for (const wchar_t c : text)
            {
                if (c == L'\n' || c == L'\r' || c == L'\t' || c == L' ')
                {
                    space = !out.empty();
                    continue;
                }
                if (space)
                {
                    out += L' ';
                    space = false;
                }
                out += c;
            }
            return out;
        }

        // --- paging ---

        uint32_t Fnv(std::string_view s)
        {
            uint32_t h = 2166136261u;
            for (const char c : s)
            {
                h ^= static_cast<unsigned char>(c);
                h *= 16777619u;
            }
            return h;
        }

        std::string ArgsKey(const json& a)
        {
            json c = a.is_object() ? a : json::object();
            c.erase("cursor");
            return c.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        std::string MakeCursor(size_t offset, const json& a)
        {
            char hash[16];
            std::snprintf(hash, sizeof(hash), "%08x", Fnv(ArgsKey(a)));
            return "o" + std::to_string(offset) + "." + hash;
        }

        size_t ReadCursor(const json& a)
        {
            const std::string c = Str(a, "cursor", 64);
            if (c.empty())
            {
                return 0;
            }
            const size_t dot = c.find('.');
            if (c[0] != 'o' || dot == std::string::npos || dot < 2)
            {
                throw BadRequest("The cursor \"" + c + "\" is not one this tool returned. Pass the `next` value of the previous result.");
            }
            size_t offset = 0;
            for (size_t i = 1; i < dot; ++i)
            {
                if (c[i] < '0' || c[i] > '9' || offset > 100000000)
                {
                    throw BadRequest("The cursor \"" + c + "\" is not one this tool returned. Pass the `next` value of the previous result.");
                }
                offset = offset * 10 + static_cast<size_t>(c[i] - '0');
            }
            char hash[16];
            std::snprintf(hash, sizeof(hash), "%08x", Fnv(ArgsKey(a)));
            if (c.substr(dot + 1) != hash)
            {
                throw BadRequest("The cursor belongs to a call with other parameters. Repeat the call with the same parameters as the one that returned it, or leave cursor out.");
            }
            return offset;
        }

        // Rows added while the result stays under the cap.
        class Capped
        {
        public:
            explicit Capped(size_t reserved) :
                _used(reserved)
            {
            }
            bool Add(json& arr, json row)
            {
                const size_t n = row.dump(-1, ' ', false, json::error_handler_t::replace).size() + 1;
                if (_used + n > kResultCap)
                {
                    return false;
                }
                _used += n;
                arr.push_back(std::move(row));
                return true;
            }

        private:
            size_t _used;
        };

        // The cold build's coverage (§8.4.2 "Coverage"): while the first sweep runs, results may miss older
        // sessions, so they carry {"indexing": {"covered_since": <date>}}.
        void AddIndexing(ToolContext& ctx, json& out)
        {
            auto r = ctx.con.Query("SELECT (SELECT value FROM meta WHERE key = 'cold_build_done'), "
                                   "(SELECT coalesce(min(last_activity_ms), 0) FROM session WHERE last_activity_ms > 0)");
            if (r.Next() && r.IsNull(0))
            {
                out["indexing"] = { { "covered_since", LocalDate(r.Int64(1)) } };
            }
        }

        // The role filter of a `roles` list (`subagent` = also read subagents' transcripts) as SQL over a
        // doc / doc_hot row (role, flags).
        std::string RoleFilterSql(const std::vector<std::string>& roles, bool excludeMetaNoise)
        {
            std::string codes;
            for (uint8_t r = 1; r < std::size(kRoleNames); ++r)
            {
                if (Contains(roles, kRoleNames[r]))
                {
                    codes += (codes.empty() ? "" : ",") + std::to_string(r);
                }
            }
            std::string sql = codes.empty() ? std::string{ "false" } : "role IN (" + codes + ")";
            if (!Contains(roles, "subagent"))
            {
                sql += " AND (flags & 8) = 0";
            }
            if (excludeMetaNoise)
            {
                sql += " AND (flags & 2) = 0";
            }
            return "(" + sql + ")";
        }

        std::string OpsKey(uint8_t op)
        {
            switch (op)
            {
            case kOpRead:
                return "read";
            case kOpEdit:
                return "edit";
            case kOpWrite:
                return "write";
            case kOpSearch:
                return "search";
            case kOpShell:
                return "shell";
            default:
                return "other";
            }
        }

        int OpCode(const std::string& name)
        {
            static const std::pair<const char*, int> kOps[] = { { "read", kOpRead }, { "edit", kOpEdit }, { "write", kOpWrite }, { "search", kOpSearch }, { "shell", kOpShell } };
            for (const auto& [n, c] : kOps)
            {
                if (name == n)
                {
                    return c;
                }
            }
            return 0;
        }

        // A path folded to the path_access forms: path_key (lower, '\') and dir_fold (lower, '/').
        std::wstring KeyForm(std::wstring p)
        {
            for (auto& c : p)
            {
                c = c == L'/' ? L'\\' : c;
            }
            if (!p.empty())
            {
                ::CharLowerBuffW(p.data(), static_cast<DWORD>(p.size()));
            }
            while (p.size() > 3 && p.back() == L'\\')
            {
                p.pop_back();
            }
            return p;
        }

        std::wstring DirForm(std::wstring p)
        {
            p = KeyForm(std::move(p));
            for (auto& c : p)
            {
                c = c == L'\\' ? L'/' : c;
            }
            return p;
        }

        // The JSON array a session row stores as text ("[...]"), parsed; an empty array on anything else.
        json JsonArrayText(const std::string& text)
        {
            const json v = json::parse(text, nullptr, false);
            return v.is_array() ? v : json::array();
        }

        // ===================================================================================================
        // search
        // ===================================================================================================

        json ToolSearch(ToolContext& ctx, const json& a)
        {
            Allow(a, { "query", "scopes", "fuzzy", "days", "after", "before", "dir", "branch", "tag", "favorite", "open", "sort", "limit", "cursor" });
            SearchRequest req;
            req.query = Wide(Str(a, "query", 500));
            const auto scopes = EnumList(a, "scopes", { "title", "path", "prompt", "reply", "tool", "subagent" }, { "title", "path", "prompt", "reply" });
            req.scopeTitle = Contains(scopes, "title");
            req.scopeDirs = req.scopeFiles = Contains(scopes, "path");
            req.scopePrompt = Contains(scopes, "prompt");
            req.scopeReply = Contains(scopes, "reply");
            req.scopeTool = Contains(scopes, "tool");
            req.scopeSubagent = Contains(scopes, "subagent");
            req.fuzzy = Bool(a, "fuzzy", false);
            const Window w = ReadWindow(a, ctx.nowMs, 90);
            req.afterMs = w.afterMs;
            req.beforeMs = w.beforeMs;
            req.dir = Wide(Str(a, "dir", 500));
            req.branch = Wide(Str(a, "branch", 200));
            req.tag = Wide(Str(a, "tag", 100));
            req.favorite = TriBool(a, "favorite");
            req.open = TriBool(a, "open");
            const std::string sort = Enum(a, "sort", { "relevance", "active", "created" }, "relevance");
            req.sort = sort == "active" ? SearchRequest::Sort::Active : sort == "created" ? SearchRequest::Sort::Created : SearchRequest::Sort::Relevance;
            req.limit = static_cast<size_t>(Int(a, "limit", 1, 50, 15));
            req.offset = ReadCursor(a);
            req.cancel = ctx.cancel;
            const SearchResult r = ctx.search.Search(ctx.con, req);

            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            out["window"] = w.label;
            out["searched"] = r.searched;
            out["matched"] = r.matched;
            static const char* const kMatch[] = { "name", "path", "prompt", "text", "listed" };
            const auto open = ctx.store.OpenSessions();
            json rows = json::array();
            Capped cap{ 512 };
            bool cut = false;
            for (const SearchRow& row : r.rows)
            {
                const SessionCard& c = r.cards->cards[r.cards->bySid.at(row.sidKey)];
                json j = json::object();
                j["id"] = Utf8(c.shortId);
                bool t = false;
                j["title"] = Clip(OneLine(c.title), 200, t);
                j["dir"] = Utf8(c.cwd);
                if (!c.branch.empty())
                {
                    j["branch"] = Utf8(c.branch);
                }
                j["active"] = LocalTime(c.activeMs ? c.activeMs : c.createdMs);
                j["created"] = LocalTime(c.createdMs);
                j["msgs"] = c.msgs;
                j["tools"] = c.tools;
                j["match"] = kMatch[std::min<size_t>(row.tier, 4)];
                if (row.hits > 0)
                {
                    j["hits"] = row.hits;
                }
                if (row.tier >= 2 && row.score > 0)
                {
                    j["score"] = std::round(row.score * 100.0) / 100.0;
                }
                if (open.count(c.sessionId))
                {
                    j["open"] = true;
                }
                if (c.favorite)
                {
                    j["favorite"] = true;
                }
                if (!c.tags.empty())
                {
                    json tags = json::array();
                    for (const auto& tg : c.tags)
                    {
                        tags.push_back(Utf8(tg));
                    }
                    j["tags"] = tags;
                }
                if (!c.forkedFrom.empty())
                {
                    int m = 0;
                    const auto parent = ctx.store.ResolveSession(c.forkedFrom, m);
                    j["fork_of"] = Utf8(parent ? parent->shortId : c.forkedFrom.substr(0, 8));
                }
                if (!c.present)
                {
                    j["transcript"] = "deleted";
                }
                json snips = json::array();
                for (const SearchSnippet& s : row.snippets)
                {
                    json sj = json::object();
                    if (!s.history)
                    {
                        sj["ref"] = MakeRef(c.shortId, ctx.store.AgentIdOfFile(s.fileId), s.lineNo, s.block);
                    }
                    sj["role"] = s.history ? std::string{ "prompt" } : RoleName(s.role);
                    if (s.history)
                    {
                        sj["source"] = "history.jsonl";
                    }
                    sj["text"] = Utf8(s.text);
                    snips.push_back(std::move(sj));
                }
                if (!snips.empty())
                {
                    j["snippets"] = std::move(snips);
                }
                if (!cap.Add(rows, std::move(j)))
                {
                    cut = true;
                    break;
                }
            }
            out["rows"] = rows;
            if (r.hasMore || cut)
            {
                out["next"] = MakeCursor(req.offset + rows.size(), a);
            }
            if (r.partial)
            {
                out["partial"] = true;
            }
            if (!r.note.empty())
            {
                out["note"] = Utf8(r.note);
            }
            AddIndexing(ctx, out);
            return out;
        }

        // ===================================================================================================
        // session
        // ===================================================================================================

        // The lineage of a session: the session it was forked from, its forks, the plan / /clear / /handover
        // it continues, the sessions that continue it.
        json Lineage(ToolContext& ctx, const SessionKey& k, const std::string& forkedFrom, const std::string& planParent)
        {
            json lin = json::object();
            const auto shortOf = [&ctx](const std::string& sessionId) -> std::string {
                int m = 0;
                const auto key = ctx.store.ResolveSession(Wide(sessionId), m);
                return key ? Utf8(key->shortId) : sessionId.substr(0, 8);
            };
            if (!forkedFrom.empty())
            {
                lin["forked_from"] = shortOf(forkedFrom);
            }
            {
                auto p = ctx.con.Prepare("SELECT short_id FROM session WHERE forked_from = $1 ORDER BY created_ms");
                p.Bind(1, std::string_view{ Utf8(k.sessionId) });
                auto r = p.Execute();
                json forks = json::array();
                while (r.Next())
                {
                    forks.push_back(r.Str(0));
                }
                if (!forks.empty())
                {
                    lin["forks"] = forks;
                }
            }
            if (!planParent.empty())
            {
                lin["continues"] = { { "id", shortOf(planParent) }, { "via", "plan" } };
            }
            {
                auto p = ctx.con.Prepare("SELECT from_id, coalesce(via, '') FROM am_link WHERE session_id = $1");
                p.Bind(1, std::string_view{ Utf8(k.sessionId) });
                auto r = p.Execute();
                while (r.Next())
                {
                    lin["continues"] = { { "id", shortOf(r.String(0)) }, { "via", r.String(1) } };
                }
            }
            {
                auto p = ctx.con.Prepare("SELECT session_id, coalesce(via, '') FROM am_link WHERE from_id = $1");
                p.Bind(1, std::string_view{ Utf8(k.sessionId) });
                auto r = p.Execute();
                json by = json::array();
                while (r.Next())
                {
                    by.push_back({ { "id", shortOf(r.String(0)) }, { "via", r.String(1) } });
                }
                if (!by.empty())
                {
                    lin["continued_by"] = by;
                }
            }
            return lin;
        }

        json ToolSession(ToolContext& ctx, const json& a)
        {
            Allow(a, { "id" });
            if (!Has(a, "id"))
            {
                throw BadRequest("id is required: a session id from a search result (8 or more characters).");
            }
            const SessionKey k = Resolve(ctx, Str(a, "id", 36));
            auto p = ctx.con.Prepare(R"SQL(
SELECT s.session_id, s.short_id, coalesce(t.title, ''), coalesce(s.custom_title, ''), coalesce(s.ai_title, ''), coalesce(s.first_prompt, ''),
       coalesce(s.cwd, ''), coalesce(s.inferred_dir, ''), coalesce(s.home_launch, false), coalesce(s.branch_last, s.branch_first, ''),
       coalesce(s.created_ms, 0), coalesce(s.last_activity_ms, s.created_ms, 0), coalesce(s.prompts_typed, 0), coalesce(s.tool_calls, 0),
       coalesce(s.subagent_files, 0), coalesce(s.context_tokens, 0), coalesce(s.in_tokens, 0), coalesce(s.out_tokens, 0),
       coalesce(s.cache_read_tokens, 0), coalesce(s.cache_write_tokens, 0), coalesce(s.models, '[]'), coalesce(s.forked_from, ''),
       coalesce(s.transcript, 0), s.cost_usd, coalesce(s.has_cost_state, false), coalesce(s.git_root, ''), coalesce(s.size_bytes, 0),
       coalesce(s.lines, 0), coalesce(s.compactions, 0), coalesce(s.api_errors, 0), coalesce(s.interrupts, 0), coalesce(s.legacy_summary, ''),
       coalesce(sm.parent, ''), coalesce(s.model_last, ''), coalesce(s.recap_last, ''), coalesce(s.last_prompt, '')
FROM session s
LEFT JOIN (SELECT session_id, any_value(title) AS title FROM am_title GROUP BY session_id) t ON t.session_id = s.session_id
LEFT JOIN summary sm ON sm.sid_key = s.sid_key
WHERE s.sid_key = $1)SQL");
            p.Bind(1, static_cast<int64_t>(k.sidKey));
            auto r = p.Execute();
            if (!r.Next())
            {
                throw ToolError{ Proto::kErrNotFound, "Unknown session \"" + Utf8(k.shortId) + "\"." };
            }
            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            out["id"] = r.Str(1);
            out["uuid"] = r.Str(0);
            const std::string stored = r.String(2), custom = r.String(3), ai = r.String(4), first = r.String(5), legacy = r.String(31);
            const std::string title = !stored.empty() ? stored : !custom.empty() ? custom : !ai.empty() ? ai : !legacy.empty() ? legacy : first;
            bool cut = false;
            out["title"] = Clip(OneLine(Wide(title)), 200, cut);
            json titles = json::object();
            if (!stored.empty())
            {
                titles["stored"] = stored;
            }
            if (!custom.empty())
            {
                titles["custom"] = custom;
            }
            if (!ai.empty())
            {
                titles["ai"] = ai;
            }
            if (!first.empty())
            {
                titles["first_prompt"] = Clip(OneLine(Wide(first)), 300, cut);
            }
            out["titles"] = titles;
            const std::string cwd = r.String(6), inferred = r.String(7);
            const bool home = r.Bool(8);
            // The folder it worked in: the inferred one for a session launched in the home folder (where no
            // one works; the app's own rule, SessionInfersWorkingDir), else the launch folder.
            const std::string dir = home && !inferred.empty() ? inferred : cwd;
            out["dir"] = dir;
            if (dir != cwd)
            {
                out["dir_launched"] = cwd;
            }
            else if (!inferred.empty() && inferred != cwd)
            {
                out["dir_worked"] = inferred;
            }
            if (!r.Str(9).empty())
            {
                out["branch"] = r.Str(9);
            }
            if (!r.Str(25).empty())
            {
                out["git_root"] = r.Str(25);
            }
            out["created"] = LocalTime(r.Int64(10));
            out["active"] = LocalTime(r.Int64(11));
            out["msgs"] = r.Int64(12);
            out["tools"] = r.Int64(13);
            if (r.Int64(14) > 0)
            {
                out["subagents"] = r.Int64(14);
            }
            if (r.Int64(15) > 0)
            {
                out["context_tokens"] = r.Int64(15);
            }
            out["tokens"] = { { "input", r.Int64(16) }, { "output", r.Int64(17) }, { "cache_read", r.Int64(18) }, { "cache_write", r.Int64(19) } };
            out["models"] = JsonArrayText(r.String(20));
            if (!r.Str(33).empty())
            {
                out["model"] = r.Str(33);
            }
            if (r.Bool(24) && !r.IsNull(23))
            {
                out["cost_usd"] = std::round(r.Double(23) * 100.0) / 100.0;
                out["cost_source"] = "claude";
            }
            out["size_bytes"] = r.Int64(26);
            out["lines"] = r.Int64(27);
            if (r.Int64(28) > 0)
            {
                out["compactions"] = r.Int64(28);
            }
            if (r.Int64(29) > 0)
            {
                out["api_errors"] = r.Int64(29);
            }
            if (r.Int64(30) > 0)
            {
                out["interrupts"] = r.Int64(30);
            }
            if (!r.Str(34).empty())
            {
                out["recap"] = Clip(Wide(r.Str(34)), 1200, cut);
            }
            if (!r.Str(35).empty())
            {
                out["last_prompt"] = Clip(OneLine(Wide(r.Str(35))), 300, cut);
            }
            const uint8_t transcript = static_cast<uint8_t>(r.Int64(22));
            out["transcript"] = transcript == 0 ? "present" : transcript == 1 ? "deleted" : "replaced";
            const json lin = Lineage(ctx, k, r.String(21), r.String(32));
            if (!lin.empty())
            {
                out["lineage"] = lin;
            }
            {
                auto q = ctx.con.Prepare("SELECT tag FROM am_tag WHERE session_id = $1 ORDER BY tag_fold");
                q.Bind(1, std::string_view{ Utf8(k.sessionId) });
                auto t = q.Execute();
                json tags = json::array();
                while (t.Next())
                {
                    tags.push_back(t.Str(0));
                }
                if (!tags.empty())
                {
                    out["tags"] = tags;
                }
            }
            {
                auto q = ctx.con.Prepare("SELECT count(*) FROM am_favorite WHERE session_id = $1");
                q.Bind(1, std::string_view{ Utf8(k.sessionId) });
                auto f = q.Execute();
                if (f.Next() && f.Int64(0) > 0)
                {
                    out["favorite"] = true;
                }
            }
            if (ctx.store.OpenSessions().count(k.sessionId))
            {
                out["open"] = true;
            }
            {
                auto q = ctx.con.Prepare("SELECT path, string_agg(DISTINCT op::VARCHAR, ',' ORDER BY op::VARCHAR), sum(n) AS uses FROM path_access "
                                         "WHERE sid_key = $1 GROUP BY path ORDER BY uses DESC, path LIMIT 10");
                q.Bind(1, static_cast<int64_t>(k.sidKey));
                auto f = q.Execute();
                json files = json::array();
                while (f.Next())
                {
                    json ops = json::array();
                    // The ops arrive as text ("1,2"): DuckDbWrap reads scalar columns only.
                    for (const char c : f.String(1))
                    {
                        if (c >= '1' && c <= '6')
                        {
                            ops.push_back(OpsKey(static_cast<uint8_t>(c - '0')));
                        }
                    }
                    files.push_back({ { "path", f.Str(0) }, { "ops", ops }, { "uses", f.Int64(2) } });
                }
                if (!files.empty())
                {
                    out["top_files"] = files;
                }
            }
            return out;
        }

        // ===================================================================================================
        // messages
        // ===================================================================================================

        // One block's key (no text) for ordering and paging a session's conversation.
        struct BlockKey
        {
            int64_t docId{ 0 };
            int32_t fileId{ 0 };
            int32_t line{ 0 };
            int32_t block{ 0 };
            int64_t ts{ 0 };
            uint8_t role{ 0 };
            uint8_t flags{ 0 };
            bool pass{ false }; // passes the role filter
        };

        // Every block of a session (cold and hot), in conversation order: the main transcript by (line,
        // block); with subagents, each subagent block by its time among them.
        std::vector<BlockKey> SessionBlocks(ToolContext& ctx, const SessionKey& k, const std::vector<std::string>& roles)
        {
            const std::string filter = RoleFilterSql(roles, true);
            auto p = ctx.con.Prepare("SELECT doc_id, file_id, line_no, block, coalesce(ts_ms, 0), role, flags, " + filter +
                                     " FROM doc WHERE sid_key = $1 AND (flags & 2) = 0 AND doc_id NOT IN (SELECT doc_id FROM tomb)"
                                     " UNION ALL SELECT doc_id, file_id, line_no, block, coalesce(ts_ms, 0), role, flags, " +
                                     filter + " FROM doc_hot WHERE sid_key = $1 AND (flags & 2) = 0");
            p.Bind(1, static_cast<int64_t>(k.sidKey));
            auto r = p.Execute();
            std::vector<BlockKey> out;
            while (r.Next())
            {
                BlockKey b;
                b.docId = r.Int64(0);
                b.fileId = static_cast<int32_t>(r.Int64(1));
                b.line = static_cast<int32_t>(r.Int64(2));
                b.block = static_cast<int32_t>(r.Int64(3));
                b.ts = r.Int64(4);
                b.role = static_cast<uint8_t>(r.Int64(5));
                b.flags = static_cast<uint8_t>(r.Int64(6));
                b.pass = r.Bool(7);
                out.push_back(b);
            }
            const bool withSub = Contains(roles, "subagent");
            const int32_t mainFile = k.fileId;
            if (!withSub)
            {
                std::sort(out.begin(), out.end(), [mainFile](const BlockKey& x, const BlockKey& y) {
                    if ((x.fileId == mainFile) != (y.fileId == mainFile))
                    {
                        return x.fileId == mainFile; // the main transcript first (a stray side block after it)
                    }
                    if (x.fileId != y.fileId)
                    {
                        return x.fileId < y.fileId;
                    }
                    return x.line != y.line ? x.line < y.line : x.block < y.block;
                });
                return out;
            }
            // With subagents: the main transcript in line order gives every main block a time (its own, else
            // the last one before it); a subagent block goes after the main blocks at or before its time.
            std::vector<BlockKey> main, side;
            for (auto& b : out)
            {
                (b.fileId == mainFile ? main : side).push_back(b);
            }
            std::sort(main.begin(), main.end(), [](const BlockKey& x, const BlockKey& y) { return x.line != y.line ? x.line < y.line : x.block < y.block; });
            std::sort(side.begin(), side.end(), [](const BlockKey& x, const BlockKey& y) {
                if (x.ts != y.ts)
                {
                    return x.ts < y.ts;
                }
                if (x.fileId != y.fileId)
                {
                    return x.fileId < y.fileId;
                }
                return x.line != y.line ? x.line < y.line : x.block < y.block;
            });
            std::vector<BlockKey> merged;
            merged.reserve(out.size());
            size_t si = 0;
            int64_t clock = 0;
            for (const auto& m : main)
            {
                clock = std::max(clock, m.ts);
                // Side blocks that happened before this main block's time go first.
                while (si < side.size() && side[si].ts > 0 && m.ts > 0 && side[si].ts < m.ts)
                {
                    merged.push_back(side[si++]);
                }
                merged.push_back(m);
            }
            while (si < side.size())
            {
                merged.push_back(side[si++]);
            }
            return merged;
        }

        json ToolMessages(ToolContext& ctx, const json& a)
        {
            Allow(a, { "id", "roles", "around", "from", "order", "limit", "full", "cursor" });
            if (!Has(a, "id"))
            {
                throw BadRequest("id is required: a session id from a search result (8 or more characters).");
            }
            const SessionKey k = Resolve(ctx, Str(a, "id", 36));
            const auto roles = EnumList(a, "roles", { "prompt", "queued", "reply", "thinking", "tool_call", "tool_result", "system", "subagent" },
                                        { "prompt", "queued", "reply" });
            if (Has(a, "around") && Has(a, "from"))
            {
                throw BadRequest("Give either around or from, not both.");
            }
            const bool desc = Enum(a, "order", { "asc", "desc" }, "asc") == "desc";
            const size_t limit = static_cast<size_t>(Int(a, "limit", 1, 100, 20));
            const bool full = Bool(a, "full", false);
            size_t offset = ReadCursor(a);

            std::vector<BlockKey> all = SessionBlocks(ctx, k, roles);
            // The position of a ref in the full list, mapped to the filtered list.
            const auto positionOf = [&](const std::string& refText) -> size_t {
                Ref ref;
                if (!ParseRef(refText, ref))
                {
                    throw BadRequest("\"" + refText + "\" is not a ref. A ref looks like 1e0f7ab2:184 or 1e0f7ab2:184.1 (from search, grep or messages).");
                }
                int m = 0;
                const auto rk = ctx.store.ResolveSession(Wide(ref.id), m);
                if (!rk || rk->sidKey != k.sidKey)
                {
                    throw BadRequest("The ref \"" + refText + "\" names another session than \"" + Utf8(k.shortId) + "\".");
                }
                const int32_t fileId = ref.agent.empty() ? k.fileId : ctx.store.SubagentFileOf(k.sidKey, Wide(ref.agent));
                if (fileId == 0)
                {
                    throw ToolError{ Proto::kErrNotFound, "The ref \"" + refText + "\" names a subagent this session does not have." };
                }
                size_t at = all.size();
                for (size_t i = 0; i < all.size(); ++i)
                {
                    const auto& b = all[i];
                    if (b.fileId == fileId && (b.line > ref.line || (b.line == ref.line && b.block >= ref.block)))
                    {
                        at = i;
                        break;
                    }
                }
                size_t pos = 0;
                for (size_t i = 0; i < at && i < all.size(); ++i)
                {
                    pos += all[i].pass ? 1 : 0;
                }
                return pos;
            };
            std::vector<BlockKey> list;
            for (const auto& b : all)
            {
                if (b.pass)
                {
                    list.push_back(b);
                }
            }
            if (Has(a, "around") && offset == 0)
            {
                const size_t pos = positionOf(Str(a, "around", 80));
                offset = pos > limit / 2 ? pos - limit / 2 : 0;
            }
            else if (Has(a, "from") && offset == 0)
            {
                offset = positionOf(Str(a, "from", 80));
            }
            if (desc)
            {
                std::reverse(list.begin(), list.end());
            }
            const size_t from = std::min(offset, list.size());
            const size_t to = std::min(list.size(), from + limit);
            std::vector<int64_t> ids;
            for (size_t i = from; i < to; ++i)
            {
                ids.push_back(list[i].docId);
            }
            std::unordered_map<uint32_t, BlockText> texts;
            const bool complete = ReadBlockTexts(ctx.con, ids, true, ctx.search.ToolTexts(), std::chrono::steady_clock::now() + std::chrono::milliseconds(800),
                                                 [&texts](uint32_t id, BlockText& b) { texts[id] = std::move(b); });
            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            out["id"] = Utf8(k.shortId);
            json blocks = json::array();
            Capped cap{ 512 };
            size_t shown = from;
            for (size_t i = from; i < to; ++i)
            {
                const BlockKey& b = list[i];
                const auto it = texts.find(static_cast<uint32_t>(b.docId));
                json j = json::object();
                j["ref"] = MakeRef(k.shortId, b.fileId == k.fileId ? std::wstring{} : ctx.store.AgentIdOfFile(b.fileId), b.line, b.block);
                if (b.ts > 0)
                {
                    j["time"] = LocalTime(b.ts);
                }
                j["role"] = RoleName(b.role);
                if ((b.flags & kDfSubagent) != 0)
                {
                    j["subagent"] = true;
                }
                if (it != texts.end())
                {
                    if (!it->second.tool.empty())
                    {
                        j["tool"] = Utf8(it->second.tool);
                    }
                    bool truncated = false;
                    j["text"] = full ? Utf8(it->second.text) : Clip(it->second.text, kBlockChars, truncated);
                    if (truncated)
                    {
                        j["truncated"] = true;
                    }
                }
                else
                {
                    j["text"] = "";
                    j["unread"] = true; // the tool-text budget ran out before this block
                }
                if (!cap.Add(blocks, std::move(j)))
                {
                    out["truncated"] = true;
                    break;
                }
                ++shown;
            }
            out["blocks"] = blocks;
            out["total"] = list.size();
            if (shown < list.size())
            {
                out["next"] = MakeCursor(shown, a);
            }
            if (!complete)
            {
                out["partial"] = true;
            }
            return out;
        }

        // ===================================================================================================
        // grep
        // ===================================================================================================

        // The longest run of literal characters in a regex (outside classes, groups' syntax and escapes of
        // classes), for narrowing through the index. A character followed by a quantifier that allows zero
        // (?, *, {0) ends the run without itself. Conservative: anything unclear ends the run.
        std::wstring LongestRegexLiteral(std::wstring_view re)
        {
            std::wstring best, cur;
            const auto flush = [&]() {
                if (cur.size() > best.size())
                {
                    best = cur;
                }
                cur.clear();
            };
            for (size_t i = 0; i < re.size(); ++i)
            {
                const wchar_t c = re[i];
                const wchar_t next = i + 1 < re.size() ? re[i + 1] : 0;
                const bool optional = next == L'?' || next == L'*' || (next == L'{' && i + 2 < re.size() && re[i + 2] == L'0');
                if (c == L'\\' && i + 1 < re.size())
                {
                    const wchar_t e = re[i + 1];
                    const bool literalEscape = !iswalnum(e); // \. \( \\ ... ; \d \w \s \b are classes
                    ++i;
                    const wchar_t after = i + 1 < re.size() ? re[i + 1] : 0;
                    const bool opt = after == L'?' || after == L'*' || (after == L'{' && i + 2 < re.size() && re[i + 2] == L'0');
                    if (literalEscape && !opt)
                    {
                        cur += e;
                    }
                    else
                    {
                        flush();
                    }
                    continue;
                }
                if (c == L'[')
                {
                    flush();
                    while (i < re.size() && re[i] != L']')
                    {
                        i += re[i] == L'\\' ? 2 : 1;
                    }
                    continue;
                }
                if (wcschr(L".^$|()+*?{}", c))
                {
                    flush();
                    if (c == L'|')
                    {
                        // An alternation: no literal is required on both sides, so nothing narrows safely.
                        return {};
                    }
                    continue;
                }
                if (optional)
                {
                    flush();
                    continue;
                }
                cur += c;
            }
            flush();
            return best;
        }

        // Fold `text` the index's way and keep, for every folded character, the original offset it came from.
        std::wstring FoldWithMap(std::wstring_view text, std::vector<uint32_t>& map)
        {
            std::wstring out;
            out.reserve(text.size());
            map.clear();
            map.reserve(text.size() + 1);
            size_t i = 0;
            while (i < text.size())
            {
                const size_t start = i;
                const char32_t cp = SessionDbTokenizer::NextCodePoint(text, i);
                const size_t before = out.size();
                SessionDbTokenizer::AppendCodePoint(out, SessionDbTokenizer::Classify(cp, ServiceClassifier()).folded);
                for (size_t k = before; k < out.size(); ++k)
                {
                    map.push_back(static_cast<uint32_t>(start));
                }
            }
            map.push_back(static_cast<uint32_t>(text.size()));
            return out;
        }

        json ToolGrep(ToolContext& ctx, const json& a)
        {
            Allow(a, { "pattern", "regex", "case_sensitive", "ids", "dir", "days", "after", "before", "roles", "context", "limit", "cursor" });
            const std::string pattern = Str(a, "pattern", 500);
            if (pattern.empty())
            {
                throw BadRequest("pattern is required: 1 to 500 characters of text (or a regular expression with regex: true).");
            }
            const bool regex = Bool(a, "regex", false);
            const bool caseSensitive = Bool(a, "case_sensitive", false);
            const auto roles = EnumList(a, "roles", { "prompt", "queued", "reply", "thinking", "tool_call", "tool_result", "system", "subagent" },
                                        { "prompt", "queued", "reply", "tool_call", "tool_result", "system" });
            const size_t context = static_cast<size_t>(Int(a, "context", 0, 400, 80));
            const size_t limit = static_cast<size_t>(Int(a, "limit", 1, 200, 30));
            const size_t offset = ReadCursor(a);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);

            // The sessions: `ids`, or the window and folder filters.
            std::vector<int32_t> sids;
            const auto cards = ctx.search.Cards(ctx.con);
            if (Has(a, "ids"))
            {
                if (!a["ids"].is_array() || a["ids"].size() > 50)
                {
                    throw BadRequest("ids must be an array of up to 50 session ids.");
                }
                for (const json& v : a["ids"])
                {
                    if (!v.is_string())
                    {
                        throw BadRequest("ids must be an array of up to 50 session ids.");
                    }
                    sids.push_back(Resolve(ctx, v.get<std::string>()).sidKey);
                }
            }
            else
            {
                const Window w = ReadWindow(a, ctx.nowMs, 90);
                const std::wstring dirF = FoldLower(Wide(Str(a, "dir", 500)));
                for (const auto& c : cards->cards)
                {
                    const int64_t active = c.activeMs ? c.activeMs : c.createdMs;
                    if ((w.afterMs && active < w.afterMs) || (w.beforeMs && active > w.beforeMs))
                    {
                        continue;
                    }
                    if (!dirF.empty() && !MatchesPathQuery(c.cwdF, dirF, false))
                    {
                        continue;
                    }
                    sids.push_back(c.sidKey);
                }
            }
            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            if (sids.empty())
            {
                out["matches"] = json::array();
                out["sessions"] = 0;
                return out;
            }

            if (regex)
            {
                // RE2's own verdict on the pattern (DuckDB compiles it): a bad regex says why, before anything else.
                try
                {
                    ctx.con.Query("SELECT regexp_matches('', " + SqlQuote(pattern) + ")");
                }
                catch (const DbError& e)
                {
                    throw BadRequest(std::string{ "Invalid regular expression: " } + e.what());
                }
            }
            // The literal that narrows the blocks through the index.
            const std::wstring literal = regex ? LongestRegexLiteral(Wide(pattern)) : Wide(pattern);
            std::wstring token;
            for (const auto& t : SessionDbTokenizer::TokenizeQueryTerm(literal, ServiceClassifier()).tokens)
            {
                if (t.size() >= 3 && t.size() > token.size())
                {
                    token = t;
                }
            }
            if (token.empty() && !Has(a, "ids"))
            {
                throw BadRequest(regex ? "The regex needs a literal of 3 or more letters or digits (outside classes and optional parts), or name the sessions in ids."
                                       : "The pattern needs a word of 3 or more letters or digits, or name the sessions in ids.");
            }

            // Candidate blocks: the token's postings (cold) + a hot-tier scan, inside the sessions and roles.
            const std::string roleSql = RoleFilterSql(roles, true);
            std::vector<std::pair<int32_t, int64_t>> cand; // (sid, doc id)
            {
                const std::vector<int64_t> sidList(sids.begin(), sids.end());
                if (!token.empty())
                {
                    const auto termIds = ctx.store.Index().Dict().TermsContaining(token, kGrepExpansion);
                    if (termIds.size() >= kGrepExpansion)
                    {
                        out["partial"] = true;
                    }
                    if (!termIds.empty())
                    {
                        auto p = ctx.con.Prepare("SELECT DISTINCT d.sid_key, d.doc_id FROM post p JOIN doc d USING (doc_id) "
                                                 "WHERE p.term_id IN (SELECT unnest(CAST($1 AS UINTEGER[]))) AND d.sid_key IN (SELECT unnest(CAST($2 AS BIGINT[]))) AND " +
                                                 roleSql + " AND d.doc_id NOT IN (SELECT doc_id FROM tomb)");
                        p.BindInt64List(1, std::vector<int64_t>(termIds.begin(), termIds.end()));
                        p.BindInt64List(2, sidList);
                        auto r = p.Execute();
                        while (r.Next())
                        {
                            cand.emplace_back(static_cast<int32_t>(r.Int64(0)), r.Int64(1));
                        }
                    }
                    auto h = ctx.con.Prepare("SELECT sid_key, doc_id FROM doc_hot WHERE contains(text_fold, $1) AND sid_key IN (SELECT unnest(CAST($2 AS BIGINT[]))) AND " + roleSql);
                    h.Bind(1, std::string_view{ Utf8(token) });
                    h.BindInt64List(2, sidList);
                    auto r = h.Execute();
                    while (r.Next())
                    {
                        cand.emplace_back(static_cast<int32_t>(r.Int64(0)), r.Int64(1));
                    }
                }
                else
                {
                    // No literal: every block of the named sessions (bounded: at most 50 sessions).
                    auto p = ctx.con.Prepare("SELECT sid_key, doc_id FROM doc WHERE sid_key IN (SELECT unnest(CAST($1 AS BIGINT[]))) AND " + roleSql +
                                             " AND doc_id NOT IN (SELECT doc_id FROM tomb) UNION ALL SELECT sid_key, doc_id FROM doc_hot WHERE sid_key IN "
                                             "(SELECT unnest(CAST($1 AS BIGINT[]))) AND " +
                                             roleSql);
                    p.BindInt64List(1, sidList);
                    auto r = p.Execute();
                    while (r.Next())
                    {
                        cand.emplace_back(static_cast<int32_t>(r.Int64(0)), r.Int64(1));
                    }
                }
            }
            // Order: newest sessions first, each session's blocks in conversation order (doc ids follow it).
            std::unordered_map<int32_t, int64_t> activeOf;
            for (const auto& c : cards->cards)
            {
                activeOf[c.sidKey] = c.activeMs ? c.activeMs : c.createdMs;
            }
            std::sort(cand.begin(), cand.end(), [&activeOf](const auto& x, const auto& y) {
                const int64_t ax = activeOf[x.first], ay = activeOf[y.first];
                if (ax != ay)
                {
                    return ax > ay;
                }
                return x.first != y.first ? x.first > y.first : x.second < y.second;
            });
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

            // Verify in order, a chunk at a time, until offset + limit + 1 matches or the budget.
            const std::wstring needle = Wide(pattern);
            std::vector<uint32_t> map;
            const std::wstring needleFolded = caseSensitive ? needle : SessionDbTokenizer::FoldText(needle, ServiceClassifier());
            json matches = json::array();
            Capped cap{ 512 };
            size_t found = 0;
            bool more = false, partial = false, cut = false;
            std::set<int32_t> sessionsHit;
            constexpr size_t kChunk = 200;
            for (size_t at = 0; at < cand.size() && !more && !cut; at += kChunk)
            {
                if (std::chrono::steady_clock::now() >= deadline || (ctx.cancel && ctx.cancel()))
                {
                    partial = true;
                    break;
                }
                const size_t end = std::min(cand.size(), at + kChunk);
                std::vector<int64_t> ids;
                for (size_t i = at; i < end; ++i)
                {
                    ids.push_back(cand[i].second);
                }
                std::unordered_map<uint32_t, BlockText> texts;
                if (!ReadBlockTexts(ctx.con, ids, true, ctx.search.ToolTexts(), deadline, [&texts](uint32_t id, BlockText& b) { texts[id] = std::move(b); }))
                {
                    partial = true;
                }
                // Regex: RE2 (DuckDB) decides, over the texts just read; it returns the matched substring.
                std::unordered_map<uint32_t, std::wstring> regexHit;
                if (regex && !texts.empty())
                {
                    std::vector<int64_t> readIds;
                    std::vector<std::string> readTexts;
                    for (const auto& [id, b] : texts)
                    {
                        readIds.push_back(id);
                        readTexts.push_back(Utf8(b.text));
                    }
                    // The pattern and its options are SQL literals: DuckDB compiles a constant pattern once, and it
                    // wants the options constant.
                    const std::string re = SqlQuote(pattern);
                    const std::string opts = caseSensitive ? "''" : "'i'";
                    auto p = ctx.con.Prepare("SELECT i, regexp_extract(t, " + re + ", 0, " + opts + ") FROM (SELECT unnest($1) AS i, unnest($2) AS t) WHERE regexp_matches(t, " +
                                             re + ", " + opts + ")");
                    p.BindInt64List(1, readIds);
                    p.BindStringList(2, readTexts);
                    auto r = p.Execute();
                    while (r.Next())
                    {
                        regexHit[static_cast<uint32_t>(r.Int64(0))] = Wide(r.Str(1));
                    }
                }
                for (size_t i = at; i < end; ++i)
                {
                    const auto it = texts.find(static_cast<uint32_t>(cand[i].second));
                    if (it == texts.end())
                    {
                        continue;
                    }
                    const std::wstring& text = it->second.text;
                    size_t pos = std::wstring::npos, len = 0;
                    if (regex)
                    {
                        const auto h = regexHit.find(it->first);
                        if (h == regexHit.end())
                        {
                            continue;
                        }
                        pos = h->second.empty() ? 0 : text.find(h->second);
                        len = h->second.size();
                        pos = pos == std::wstring::npos ? 0 : pos;
                    }
                    else if (caseSensitive)
                    {
                        pos = text.find(needle);
                        len = needle.size();
                    }
                    else
                    {
                        const std::wstring folded = FoldWithMap(text, map);
                        const size_t fpos = folded.find(needleFolded);
                        if (fpos != std::wstring::npos)
                        {
                            pos = map[fpos];
                            len = map[std::min(fpos + needleFolded.size(), map.size() - 1)] - pos;
                        }
                    }
                    if (pos == std::wstring::npos)
                    {
                        continue;
                    }
                    if (found++ < offset)
                    {
                        continue;
                    }
                    if (matches.size() >= limit)
                    {
                        more = true;
                        break;
                    }
                    const SessionCard& c = cards->cards[cards->bySid.at(cand[i].first)];
                    const size_t s0 = pos > context ? pos - context : 0;
                    const size_t s1 = std::min(text.size(), pos + len + context);
                    std::wstring snippet = (s0 > 0 ? L"…" : L"") + OneLine(std::wstring_view{ text }.substr(s0, s1 - s0)) + (s1 < text.size() ? L"…" : L"");
                    json j = json::object();
                    j["id"] = Utf8(c.shortId);
                    bool t = false;
                    j["title"] = Clip(OneLine(c.title), 120, t);
                    j["ref"] = MakeRef(c.shortId, ctx.store.AgentIdOfFile(it->second.fileId), it->second.line, it->second.block);
                    if (it->second.tsMs > 0)
                    {
                        j["time"] = LocalTime(it->second.tsMs);
                    }
                    j["role"] = RoleName(it->second.role);
                    if (!it->second.tool.empty())
                    {
                        j["tool"] = Utf8(it->second.tool);
                    }
                    j["snippet"] = Utf8(snippet);
                    if (!cap.Add(matches, std::move(j)))
                    {
                        cut = true;
                        break;
                    }
                    sessionsHit.insert(cand[i].first);
                }
            }
            out["matches"] = matches;
            out["sessions"] = sessionsHit.size();
            if (more || cut)
            {
                out["next"] = MakeCursor(offset + matches.size(), a);
            }
            if (partial)
            {
                out["partial"] = true;
            }
            return out;
        }

        // ===================================================================================================
        // files / who_touched
        // ===================================================================================================

        json ToolFiles(ToolContext& ctx, const json& a)
        {
            Allow(a, { "id", "op", "under", "limit", "cursor" });
            if (!Has(a, "id"))
            {
                throw BadRequest("id is required: a session id from a search result (8 or more characters).");
            }
            const SessionKey k = Resolve(ctx, Str(a, "id", 36));
            const int op = Has(a, "op") ? OpCode(Enum(a, "op", { "read", "edit", "write", "search", "shell" }, "")) : 0;
            const std::wstring under = Has(a, "under") ? KeyForm(Wide(Str(a, "under", 500))) : std::wstring{};
            const size_t limit = static_cast<size_t>(Int(a, "limit", 1, 500, 100));
            const size_t offset = ReadCursor(a);
            auto p = ctx.con.Prepare("SELECT path, path_key, op, coalesce(n, 0), coalesce(first_ms, 0), coalesce(last_ms, 0) FROM path_access WHERE sid_key = $1" +
                                     std::string{ op ? " AND op = $2" : "" });
            p.Bind(1, static_cast<int64_t>(k.sidKey));
            if (op)
            {
                p.Bind(2, static_cast<int64_t>(op));
            }
            struct FileAgg
            {
                std::string path;
                std::map<std::string, int64_t> ops;
                int64_t first{ 0 }, last{ 0 };
            };
            std::map<std::wstring, FileAgg> files; // by path key
            auto r = p.Execute();
            while (r.Next())
            {
                const std::wstring key = FromUtf8(r.Str(1));
                if (!under.empty() && !(key.size() > under.size() && key.compare(0, under.size(), under) == 0 && key[under.size()] == L'\\'))
                {
                    continue;
                }
                FileAgg& f = files[key];
                f.path = r.String(0);
                f.ops[OpsKey(static_cast<uint8_t>(r.Int64(2)))] += r.Int64(3);
                const int64_t first = r.Int64(4), last = r.Int64(5);
                f.first = f.first == 0 || (first && first < f.first) ? first : f.first;
                f.last = std::max(f.last, last);
            }
            std::vector<const FileAgg*> order;
            for (const auto& [key, f] : files)
            {
                order.push_back(&f);
            }
            std::sort(order.begin(), order.end(), [](const FileAgg* x, const FileAgg* y) { return x->last != y->last ? x->last > y->last : x->path < y->path; });
            json out = json::object();
            out["id"] = Utf8(k.shortId);
            json list = json::array();
            Capped cap{ 4096 };
            size_t shown = offset;
            for (size_t i = offset; i < order.size() && list.size() < limit; ++i)
            {
                json j = { { "path", order[i]->path }, { "ops", order[i]->ops } };
                if (order[i]->first)
                {
                    j["first"] = LocalTime(order[i]->first);
                }
                if (order[i]->last)
                {
                    j["last"] = LocalTime(order[i]->last);
                }
                if (!cap.Add(list, std::move(j)))
                {
                    out["truncated"] = true;
                    break;
                }
                ++shown;
            }
            out["files"] = list;
            // The folders, with how many of the session's files each holds (all files, not just this page).
            std::map<std::string, int64_t> folders;
            for (const auto* f : order)
            {
                const size_t cut = f->path.find_last_of("\\/");
                folders[cut == std::string::npos ? std::string{} : f->path.substr(0, cut)]++;
            }
            json dirs = json::array();
            std::vector<std::pair<std::string, int64_t>> fv(folders.begin(), folders.end());
            std::sort(fv.begin(), fv.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
            for (size_t i = 0; i < fv.size() && i < 30; ++i)
            {
                dirs.push_back({ { "dir", fv[i].first }, { "files", fv[i].second } });
            }
            out["folders"] = dirs;
            out["total"] = order.size();
            if (shown < order.size())
            {
                out["next"] = MakeCursor(shown, a);
            }
            return out;
        }

        json ToolWhoTouched(ToolContext& ctx, const json& a)
        {
            Allow(a, { "path", "op", "days", "after", "before", "limit", "cursor" });
            const std::string path = Str(a, "path", 500);
            if (path.empty())
            {
                throw BadRequest("path is required: a full path, a folder (everything under it), or a file name alone.");
            }
            const int op = Has(a, "op") ? OpCode(Enum(a, "op", { "read", "edit", "write", "search", "shell" }, "")) : 0;
            const Window w = ReadWindow(a, ctx.nowMs, 365);
            const size_t limit = static_cast<size_t>(Int(a, "limit", 1, 100, 20));
            const size_t offset = ReadCursor(a);
            const std::wstring wide = Wide(path);
            const bool bare = wide.find_first_of(L"\\/") == std::wstring::npos;
            std::string where;
            std::vector<std::string> binds;
            if (bare)
            {
                where = "p.leaf_fold = $1";
                binds.push_back(Utf8(DirForm(wide)));
            }
            else
            {
                // A file (its exact key), the files directly in a folder, or anything deeper.
                where = "(p.path_key = $1 OR p.dir_fold = $2 OR starts_with(p.dir_fold, $3))";
                binds.push_back(Utf8(KeyForm(wide)));
                binds.push_back(Utf8(DirForm(wide)));
                binds.push_back(Utf8(DirForm(wide)) + "/");
            }
            std::string sql = "SELECT p.sid_key, p.path, p.op, coalesce(p.n, 0), coalesce(s.last_activity_ms, s.created_ms, 0) FROM path_access p JOIN session s USING (sid_key) WHERE " + where;
            size_t next = binds.size() + 1;
            if (op)
            {
                sql += " AND p.op = $" + std::to_string(next++);
            }
            if (w.afterMs)
            {
                sql += " AND coalesce(s.last_activity_ms, s.created_ms, 0) >= $" + std::to_string(next++);
            }
            if (w.beforeMs)
            {
                sql += " AND coalesce(s.last_activity_ms, s.created_ms, 0) <= $" + std::to_string(next++);
            }
            auto p = ctx.con.Prepare(sql);
            size_t b = 1;
            for (const auto& v : binds)
            {
                p.Bind(b++, std::string_view{ v });
            }
            if (op)
            {
                p.Bind(b++, static_cast<int64_t>(op));
            }
            if (w.afterMs)
            {
                p.Bind(b++, w.afterMs);
            }
            if (w.beforeMs)
            {
                p.Bind(b++, w.beforeMs);
            }
            struct Hit
            {
                int64_t active{ 0 };
                std::map<std::string, int64_t> ops;
                std::vector<std::string> paths;
            };
            std::unordered_map<int32_t, Hit> bySession;
            auto r = p.Execute();
            while (r.Next())
            {
                Hit& h = bySession[static_cast<int32_t>(r.Int64(0))];
                h.active = r.Int64(4);
                h.ops[OpsKey(static_cast<uint8_t>(r.Int64(2)))] += r.Int64(3);
                const std::string pth = r.String(1);
                if (h.paths.size() < 3 && std::find(h.paths.begin(), h.paths.end(), pth) == h.paths.end())
                {
                    h.paths.push_back(pth);
                }
            }
            std::vector<std::pair<int32_t, const Hit*>> order;
            for (const auto& [sid, h] : bySession)
            {
                order.emplace_back(sid, &h);
            }
            std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.second->active != y.second->active ? x.second->active > y.second->active : x.first > y.first; });
            const auto cards = ctx.search.Cards(ctx.con);
            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            out["window"] = w.label;
            json list = json::array();
            Capped cap{ 512 };
            size_t shown = offset;
            for (size_t i = offset; i < order.size() && list.size() < limit; ++i)
            {
                const auto it = cards->bySid.find(order[i].first);
                if (it == cards->bySid.end())
                {
                    continue;
                }
                const SessionCard& c = cards->cards[it->second];
                bool t = false;
                json j = { { "id", Utf8(c.shortId) }, { "title", Clip(OneLine(c.title), 120, t) }, { "active", LocalTime(order[i].second->active) },
                           { "ops", order[i].second->ops }, { "paths", order[i].second->paths } };
                if (!cap.Add(list, std::move(j)))
                {
                    out["truncated"] = true;
                    break;
                }
                ++shown;
            }
            out["sessions"] = list;
            out["matched"] = order.size();
            if (shown < order.size())
            {
                out["next"] = MakeCursor(shown, a);
            }
            return out;
        }

        // ===================================================================================================
        // timeline
        // ===================================================================================================

        json ToolTimeline(ToolContext& ctx, const json& a)
        {
            Allow(a, { "from", "to", "group", "dir", "limit" });
            int64_t fromMs = 0, toMs = 0;
            if (Has(a, "from") && !ParseLocalDate(Str(a, "from", 10), false, fromMs))
            {
                throw BadRequest("from must be a local date YYYY-MM-DD.");
            }
            if (Has(a, "to") && !ParseLocalDate(Str(a, "to", 10), true, toMs))
            {
                throw BadRequest("to must be a local date YYYY-MM-DD.");
            }
            if (!toMs)
            {
                toMs = ctx.nowMs;
            }
            if (!fromMs)
            {
                // The last 7 days: from the local midnight six days before today.
                const std::string today = LocalDate(ctx.nowMs);
                int64_t midnight = 0;
                ParseLocalDate(today, false, midnight);
                fromMs = midnight - 6 * 86400000LL;
            }
            if (fromMs > toMs)
            {
                throw BadRequest("from is later than to.");
            }
            const bool hourly = Enum(a, "group", { "day", "hour" }, "day") == "hour";
            const int64_t span = toMs - fromMs;
            if ((!hourly && span > 366LL * 86400000LL) || (hourly && span > 744LL * 3600000LL))
            {
                throw BadRequest(hourly ? "At most 744 hour buckets (31 days): narrow from / to." : "At most 366 day buckets: narrow from / to.");
            }
            const size_t limit = static_cast<size_t>(Int(a, "limit", 1, 50, 10));
            const std::wstring dirF = FoldLower(Wide(Str(a, "dir", 500)));
            const auto cards = ctx.search.Cards(ctx.con);
            // Prompts (typed / queued) and replies, by time: the blocks carry their line's time.
            auto p = ctx.con.Prepare("SELECT sid_key, ts_ms, role FROM doc WHERE role IN (1, 2, 7) AND (flags & 11) = 0 AND ts_ms BETWEEN $1 AND $2 "
                                     "UNION ALL SELECT sid_key, ts_ms, role FROM doc_hot WHERE role IN (1, 2, 7) AND (flags & 11) = 0 AND ts_ms BETWEEN $1 AND $2");
            p.Bind(1, fromMs).Bind(2, toMs);
            struct Cell
            {
                int64_t prompts{ 0 }, turns{ 0 }, first{ 0 }, last{ 0 };
            };
            std::map<std::string, std::unordered_map<int32_t, Cell>> buckets; // bucket label -> session -> counts
            auto r = p.Execute();
            while (r.Next())
            {
                const int32_t sid = static_cast<int32_t>(r.Int64(0));
                const auto it = cards->bySid.find(sid);
                if (it == cards->bySid.end())
                {
                    continue;
                }
                if (!dirF.empty() && !MatchesPathQuery(cards->cards[it->second].cwdF, dirF, false))
                {
                    continue;
                }
                const int64_t ts = r.Int64(1);
                const std::string t = LocalTime(ts);
                const std::string label = hourly ? t.substr(0, 13) + ":00" : t.substr(0, 10);
                Cell& c = buckets[label][sid];
                (r.Int64(2) == kRoleReply ? c.turns : c.prompts)++;
                c.first = c.first == 0 ? ts : std::min(c.first, ts);
                c.last = std::max(c.last, ts);
            }
            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            json list = json::array();
            Capped cap{ 512 };
            for (const auto& [label, sessions] : buckets)
            {
                std::vector<std::pair<int32_t, Cell>> v(sessions.begin(), sessions.end());
                std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) {
                    const int64_t ax = x.second.prompts + x.second.turns, ay = y.second.prompts + y.second.turns;
                    return ax != ay ? ax > ay : x.second.first < y.second.first;
                });
                json ss = json::array();
                for (size_t i = 0; i < v.size() && i < limit; ++i)
                {
                    const SessionCard& c = cards->cards[cards->bySid.at(v[i].first)];
                    bool t = false;
                    ss.push_back({ { "id", Utf8(c.shortId) }, { "title", Clip(OneLine(c.title), 100, t) }, { "prompts", v[i].second.prompts },
                                   { "turns", v[i].second.turns }, { "first", LocalTime(v[i].second.first) }, { "last", LocalTime(v[i].second.last) } });
                }
                json b = { { "start", label }, { "sessions", ss } };
                if (v.size() > limit)
                {
                    b["more"] = v.size() - limit;
                }
                if (!cap.Add(list, std::move(b)))
                {
                    out["truncated"] = true;
                    break;
                }
            }
            out["buckets"] = list;
            return out;
        }

        // ===================================================================================================
        // usage
        // ===================================================================================================

        json ToolUsage(ToolContext& ctx, const json& a)
        {
            Allow(a, { "group_by", "days", "after", "before", "dir", "id", "limit" });
            if (!Has(a, "group_by"))
            {
                throw BadRequest("group_by is required: session, day, model or dir.");
            }
            const std::string group = Enum(a, "group_by", { "session", "day", "model", "dir" }, "");
            const Window w = ReadWindow(a, ctx.nowMs, 30);
            const size_t limit = static_cast<size_t>(Int(a, "limit", 1, 200, 20));
            const std::wstring dirF = FoldLower(Wide(Str(a, "dir", 500)));
            int32_t onlySid = 0;
            if (Has(a, "id"))
            {
                onlySid = Resolve(ctx, Str(a, "id", 36)).sidKey;
            }
            std::string sql = "SELECT sid_key, coalesce(ts_ms, 0), coalesce(model, ''), coalesce(in_tok, 0), coalesce(out_tok, 0), coalesce(cache_read, 0), "
                              "coalesce(cache_write, 0) FROM usage WHERE true";
            if (w.afterMs)
            {
                sql += " AND ts_ms >= $1";
            }
            if (w.beforeMs)
            {
                sql += std::string{ " AND ts_ms <= $" } + (w.afterMs ? "2" : "1");
            }
            if (onlySid)
            {
                sql += " AND sid_key = $" + std::to_string(1 + (w.afterMs ? 1 : 0) + (w.beforeMs ? 1 : 0));
            }
            auto p = ctx.con.Prepare(sql);
            size_t b = 1;
            if (w.afterMs)
            {
                p.Bind(b++, w.afterMs);
            }
            if (w.beforeMs)
            {
                p.Bind(b++, w.beforeMs);
            }
            if (onlySid)
            {
                p.Bind(b++, static_cast<int64_t>(onlySid));
            }
            const auto cards = ctx.search.Cards(ctx.con);
            struct Row
            {
                int64_t in{ 0 }, out{ 0 }, cr{ 0 }, cw{ 0 }, messages{ 0 };
                int32_t sid{ 0 };
            };
            std::map<std::string, Row> rows;
            Row totals;
            auto r = p.Execute();
            while (r.Next())
            {
                const int32_t sid = static_cast<int32_t>(r.Int64(0));
                const auto it = cards->bySid.find(sid);
                if (it == cards->bySid.end())
                {
                    continue;
                }
                const SessionCard& c = cards->cards[it->second];
                if (!dirF.empty() && !MatchesPathQuery(c.cwdF, dirF, false))
                {
                    continue;
                }
                std::string key;
                if (group == "session")
                {
                    key = Utf8(c.shortId);
                }
                else if (group == "day")
                {
                    key = LocalDate(r.Int64(1));
                }
                else if (group == "model")
                {
                    key = r.Str(2).empty() ? std::string{ "(unknown)" } : r.String(2);
                }
                else
                {
                    key = Utf8(c.cwd);
                }
                Row& row = rows[key];
                row.sid = sid;
                for (Row* t : { &row, &totals })
                {
                    t->in += r.Int64(3);
                    t->out += r.Int64(4);
                    t->cr += r.Int64(5);
                    t->cw += r.Int64(6);
                    ++t->messages;
                }
            }
            std::vector<std::pair<std::string, Row>> v(rows.begin(), rows.end());
            if (group == "day")
            {
                std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            }
            else
            {
                std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) {
                    const int64_t ax = x.second.in + x.second.out + x.second.cr + x.second.cw, ay = y.second.in + y.second.out + y.second.cr + y.second.cw;
                    return ax != ay ? ax > ay : x.first < y.first;
                });
            }
            // Claude Code's own cost record, per session (only where the session has one): the WHOLE session's
            // cost, not the window's share — so it is given for group_by session alone.
            std::unordered_map<int32_t, double> costOf;
            if (group == "session")
            {
                auto q = ctx.con.Query("SELECT sid_key, cost_usd FROM session WHERE has_cost_state AND cost_usd IS NOT NULL");
                while (q.Next())
                {
                    costOf[static_cast<int32_t>(q.Int64(0))] = q.Double(1);
                }
            }
            json out = json::object();
            out["now"] = LocalTime(ctx.nowMs);
            out["window"] = w.label;
            json list = json::array();
            for (size_t i = 0; i < v.size() && i < limit; ++i)
            {
                const Row& row = v[i].second;
                json j = { { "key", v[i].first }, { "messages", row.messages }, { "input", row.in }, { "output", row.out }, { "cache_read", row.cr }, { "cache_write", row.cw } };
                if (group == "session")
                {
                    const auto c = costOf.find(row.sid);
                    if (c != costOf.end())
                    {
                        j["cost_usd"] = std::round(c->second * 100.0) / 100.0;
                        j["cost_source"] = "claude";
                    }
                    const SessionCard& card = cards->cards[cards->bySid.at(row.sid)];
                    bool t = false;
                    j["title"] = Clip(OneLine(card.title), 80, t);
                }
                list.push_back(std::move(j));
            }
            out["rows"] = list;
            out["totals"] = { { "messages", totals.messages }, { "input", totals.in }, { "output", totals.out }, { "cache_read", totals.cr }, { "cache_write", totals.cw } };
            if (v.size() > limit)
            {
                out["more"] = v.size() - limit;
            }
            // D13 is open: no price table ships, so no estimate is made for sessions without Claude's record.
            out["prices"] = nullptr;
            out["note"] = group == "session" ? "cost_usd is Claude Code's own record for the whole session, where it wrote one; other costs are not estimated (no price table yet)."
                                             : "Costs are not estimated (no price table yet); group_by session shows Claude Code's own per-session record where it exists.";
            return out;
        }

        // ===================================================================================================
        // related
        // ===================================================================================================

        json ToolRelated(ToolContext& ctx, const json& a)
        {
            Allow(a, { "id", "kinds", "limit" });
            if (!Has(a, "id"))
            {
                throw BadRequest("id is required: a session id from a search result (8 or more characters).");
            }
            const SessionKey k = Resolve(ctx, Str(a, "id", 36));
            const auto kinds = EnumList(a, "kinds", { "lineage", "files", "words" }, { "lineage", "files", "words" });
            const int64_t limit = Int(a, "limit", 1, 20, 5);
            const auto cards = ctx.search.Cards(ctx.con);
            const auto rowOf = [&cards](int32_t sid, json extra) -> json {
                const auto it = cards->bySid.find(sid);
                if (it == cards->bySid.end())
                {
                    return nullptr;
                }
                const SessionCard& c = cards->cards[it->second];
                bool t = false;
                extra["id"] = Utf8(c.shortId);
                extra["title"] = Clip(OneLine(c.title), 100, t);
                extra["active"] = LocalTime(c.activeMs ? c.activeMs : c.createdMs);
                return extra;
            };
            json out = json::object();
            out["id"] = Utf8(k.shortId);
            if (Contains(kinds, "lineage"))
            {
                auto q = ctx.con.Prepare("SELECT coalesce(s.forked_from, ''), coalesce(sm.parent, '') FROM session s LEFT JOIN summary sm USING (sid_key) WHERE s.sid_key = $1");
                q.Bind(1, static_cast<int64_t>(k.sidKey));
                auto r = q.Execute();
                if (r.Next())
                {
                    const json lin = Lineage(ctx, k, r.String(0), r.String(1));
                    if (!lin.empty())
                    {
                        out["lineage"] = lin;
                    }
                }
            }
            if (Contains(kinds, "files"))
            {
                // Shared touched files, each weighted by how rare it is (a file every session reads, like a
                // CLAUDE.md, says little).
                auto q = ctx.con.Prepare(R"SQL(
WITH mine AS (SELECT DISTINCT path_key FROM path_access WHERE sid_key = $1),
     touch AS (SELECT DISTINCT sid_key, path_key FROM path_access WHERE path_key IN (SELECT path_key FROM mine)),
     df AS (SELECT path_key, count(*) AS n FROM touch GROUP BY path_key)
SELECT t.sid_key, count(*) AS shared, sum(1.0 / df.n) AS score
FROM touch t JOIN df USING (path_key) WHERE t.sid_key <> $1
GROUP BY t.sid_key ORDER BY score DESC, shared DESC LIMIT $2)SQL");
                q.Bind(1, static_cast<int64_t>(k.sidKey)).Bind(2, limit);
                auto r = q.Execute();
                json list = json::array();
                while (r.Next())
                {
                    json row = rowOf(static_cast<int32_t>(r.Int64(0)), { { "shared", r.Int64(1) } });
                    if (!row.is_null())
                    {
                        list.push_back(std::move(row));
                    }
                }
                out["by_files"] = list;
            }
            if (Contains(kinds, "words"))
            {
                // The session's rare words (held by few sessions), and the sessions that share the most of them.
                const double n = static_cast<double>(std::max<size_t>(1, cards->cards.size()));
                auto q = ctx.con.Prepare(R"SQL(
WITH mine AS (SELECT s.term_id, d.sf FROM spost s JOIN dict d USING (term_id)
              WHERE s.sid_key = $1 AND d.sf BETWEEN 2 AND 25 AND length(d.term) >= 4
              ORDER BY d.sf, s.hits DESC LIMIT 60)
SELECT o.sid_key, sum(ln($2 / m.sf)) AS score, count(*) AS shared
FROM spost o JOIN mine m USING (term_id) WHERE o.sid_key <> $1
GROUP BY o.sid_key ORDER BY score DESC LIMIT $3)SQL");
                q.Bind(1, static_cast<int64_t>(k.sidKey)).Bind(2, n).Bind(3, limit);
                auto r = q.Execute();
                json list = json::array();
                while (r.Next())
                {
                    json row = rowOf(static_cast<int32_t>(r.Int64(0)), { { "score", std::round(r.Double(1) * 10.0) / 10.0 }, { "shared_words", r.Int64(2) } });
                    if (!row.is_null())
                    {
                        list.push_back(std::move(row));
                    }
                }
                out["by_words"] = list;
            }
            return out;
        }

        // ===================================================================================================
        // sql
        // ===================================================================================================

        // The views the sql tool may read (§5.3); anything else in the query is refused.
        const char* const kSqlViews[] = { "v_sessions", "v_messages", "v_lines", "v_paths", "v_usage", "v_links" };

        // Walk DuckDB's parse tree (json_serialize_sql) and collect every base-table name and every CTE name;
        // report any table function. Returns false (with `why`) for a node the tool refuses.
        bool CollectTables(const json& node, std::set<std::string>& tables, std::set<std::string>& ctes, std::string& why)
        {
            if (node.is_object())
            {
                const auto type = node.find("type");
                if (type != node.end() && type->is_string())
                {
                    const std::string t = type->get<std::string>();
                    if (t == "BASE_TABLE")
                    {
                        const auto name = node.find("table_name");
                        if (name != node.end() && name->is_string())
                        {
                            tables.insert(Lower(name->get<std::string>()));
                        }
                        const auto schema = node.find("schema_name");
                        const auto catalog = node.find("catalog_name");
                        if ((schema != node.end() && schema->is_string() && !schema->get<std::string>().empty() && Lower(schema->get<std::string>()) != "main") ||
                            (catalog != node.end() && catalog->is_string() && !catalog->get<std::string>().empty()))
                        {
                            why = "Name the views without a schema or catalog (for example FROM v_sessions).";
                            return false;
                        }
                    }
                    else if (t == "TABLE_FUNCTION")
                    {
                        why = "Table functions are not allowed; read the views only.";
                        return false;
                    }
                }
                const auto map = node.find("cte_map");
                if (map != node.end() && map->is_object())
                {
                    const auto entries = map->find("map");
                    if (entries != map->end() && entries->is_array())
                    {
                        for (const json& e : *entries)
                        {
                            const auto key = e.find("key");
                            if (key != e.end() && key->is_string())
                            {
                                ctes.insert(Lower(key->get<std::string>()));
                            }
                        }
                    }
                }
                for (const auto& [k, v] : node.items())
                {
                    if (!CollectTables(v, tables, ctes, why))
                    {
                        return false;
                    }
                }
            }
            else if (node.is_array())
            {
                for (const json& v : node)
                {
                    if (!CollectTables(v, tables, ctes, why))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        json ToolSql(ToolContext& ctx, const json& a)
        {
            Allow(a, { "query", "limit" });
            const std::string query = Str(a, "query", 4000);
            if (query.empty())
            {
                throw BadRequest("query is required: one read-only SELECT over the views (1 to 4,000 characters).");
            }
            const int64_t limit = Int(a, "limit", 1, 200, 50);
            const auto t0 = std::chrono::steady_clock::now();
            // Exactly one statement.
            {
                duckdb_extracted_statements ex = nullptr;
                const idx_t n = duckdb_extract_statements(ctx.con.Handle(), query.c_str(), &ex);
                std::string err = n == 0 && ex ? std::string{ duckdb_extract_statements_error(ex) ? duckdb_extract_statements_error(ex) : "" } : std::string{};
                duckdb_destroy_extracted(&ex);
                if (n == 0)
                {
                    throw BadRequest(err.empty() ? std::string{ "The query could not be parsed." } : "The query could not be parsed: " + err);
                }
                if (n > 1)
                {
                    throw BadRequest("Exactly one statement is allowed (found " + std::to_string(n) + ").");
                }
            }
            // Only the views, no table functions: DuckDB's own parse tree says what the query reads.
            {
                // json_serialize_sql wants its argument as a constant string (a bound parameter has no type yet).
                auto r = ctx.con.Query("SELECT json_serialize_sql(" + SqlQuote(query) + ")");
                if (!r.Next())
                {
                    throw BadRequest("The query could not be parsed.");
                }
                const json tree = json::parse(r.String(0), nullptr, false);
                if (tree.is_discarded())
                {
                    throw BadRequest("The query could not be parsed.");
                }
                if (tree.value("error", false))
                {
                    throw BadRequest("The query is not a SELECT: " + tree.value("error_message", std::string{ "only a SELECT is allowed." }));
                }
                std::set<std::string> tables, ctes;
                std::string why;
                if (!CollectTables(tree, tables, ctes, why))
                {
                    throw BadRequest(why);
                }
                for (const auto& t : tables)
                {
                    const bool view = std::any_of(std::begin(kSqlViews), std::end(kSqlViews), [&t](const char* v) { return t == v; });
                    if (!view && !ctes.count(t))
                    {
                        throw BadRequest("\"" + t + "\" is not one of the views. Read v_sessions, v_messages, v_lines, v_paths, v_usage or v_links.");
                    }
                }
            }
            // A SELECT: DuckDB's own statement type.
            {
                auto p = ctx.con.Prepare(query);
                if (p.Type() != DUCKDB_STATEMENT_TYPE_SELECT)
                {
                    throw BadRequest("Only a SELECT is allowed.");
                }
            }
            // The column types, then every value as text (one rendering for every type, lists and structs too).
            std::vector<std::pair<std::string, std::string>> columns;
            {
                auto r = ctx.con.Query("DESCRIBE (" + query + ")");
                while (r.Next())
                {
                    columns.emplace_back(r.String(0), r.String(1));
                }
            }
            auto r = ctx.con.Query("SELECT COLUMNS(*)::VARCHAR FROM (" + query + ") LIMIT " + std::to_string(limit + 1));
            json cols = json::array();
            for (const auto& [name, type] : columns)
            {
                cols.push_back({ { "name", name }, { "type", type } });
            }
            json rows = json::array();
            Capped cap{ 1024 + cols.dump().size() };
            bool truncated = false;
            while (r.Next())
            {
                if (static_cast<int64_t>(rows.size()) >= limit)
                {
                    truncated = true;
                    break;
                }
                json row = json::array();
                for (size_t c = 0; c < r.ColumnCount(); ++c)
                {
                    if (r.IsNull(c))
                    {
                        row.push_back(nullptr);
                        continue;
                    }
                    const std::string v = r.String(c);
                    const std::string& type = c < columns.size() ? columns[c].second : std::string{};
                    const bool integer = type == "BIGINT" || type == "INTEGER" || type == "SMALLINT" || type == "TINYINT" || type == "UBIGINT" || type == "UINTEGER" ||
                                         type == "USMALLINT" || type == "UTINYINT" || type == "HUGEINT";
                    const bool real = type == "DOUBLE" || type == "FLOAT" || type.rfind("DECIMAL", 0) == 0;
                    if (integer || real)
                    {
                        const json num = json::parse(v, nullptr, false);
                        row.push_back(num.is_number() ? num : json(v));
                    }
                    else if (type == "BOOLEAN")
                    {
                        row.push_back(v == "true");
                    }
                    else
                    {
                        bool cut = false;
                        row.push_back(Clip(Wide(v), kBlockChars, cut));
                    }
                }
                if (!cap.Add(rows, std::move(row)))
                {
                    truncated = true;
                    break;
                }
            }
            json out = json::object();
            out["columns"] = cols;
            out["rows"] = rows;
            out["truncated"] = truncated;
            out["ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            return out;
        }

        // ===================================================================================================
        // the registry
        // ===================================================================================================

        // The registry: each definition of SessionDbToolDefs.h (the ONE copy of the names, descriptions, schemas and
        // budgets, which the in-app AI search reads too) paired with its handler here. A definition with no handler,
        // or a handler with no definition, is a build-time mistake: the registry then throws at its first use (the
        // service logs it and answers no tool), and the m5 harness's registry check fails.
        std::vector<ToolSpec> BuildRegistry()
        {
            using Handler = std::function<json(ToolContext&, const json&)>;
            const std::pair<const char*, Handler> handlers[] = {
                { "search", ToolSearch }, { "session", ToolSession }, { "messages", ToolMessages }, { "grep", ToolGrep },
                { "files", ToolFiles }, { "who_touched", ToolWhoTouched }, { "timeline", ToolTimeline }, { "usage", ToolUsage },
                { "related", ToolRelated }, { "sql", ToolSql },
            };
            static_assert(std::size(handlers) == SessionDbToolDefs::kToolCount, "every tool definition needs exactly one handler");
            std::vector<ToolSpec> r;
            r.reserve(SessionDbToolDefs::kToolCount);
            for (const SessionDbToolDefs::ToolDef& d : SessionDbToolDefs::kTools)
            {
                const Handler* handler = nullptr;
                for (const auto& [name, h] : handlers)
                {
                    if (std::string_view{ name } == d.name)
                    {
                        handler = &h;
                        break;
                    }
                }
                if (!handler)
                {
                    throw std::logic_error(std::string{ "the tool definition \"" } + d.name + "\" has no handler");
                }
                r.push_back(ToolSpec{ d.name, d.description, d.inputSchema, d.budgetMs, *handler });
            }
            return r;
        }
    }

    const std::vector<ToolSpec>& ToolRegistry()
    {
        static const std::vector<ToolSpec> registry = BuildRegistry();
        return registry;
    }

    nlohmann::json ToolDefinitionsJson()
    {
        json list = json::array();
        for (const ToolSpec& t : ToolRegistry())
        {
            json schema = json::parse(t.inputSchema, nullptr, false);
            list.push_back({ { "name", t.name },
                             { "description", t.description },
                             { "inputSchema", schema.is_discarded() ? json::object() : schema },
                             { "annotations", { { "readOnlyHint", true }, { "idempotentHint", true }, { "openWorldHint", false } } } });
        }
        return list;
    }

    ToolOutcome CallTool(ToolContext& ctx, std::string_view name, const nlohmann::json& args)
    {
        ToolOutcome o;
        const ToolSpec* spec = nullptr;
        for (const ToolSpec& t : ToolRegistry())
        {
            if (t.name == name)
            {
                spec = &t;
                break;
            }
        }
        if (!spec)
        {
            o.isError = true;
            o.code = Proto::kErrBadRequest;
            o.text = "Unknown tool \"" + std::string{ name } + "\".";
            return o;
        }
        try
        {
            o.result = spec->handler(ctx, args.is_null() ? json::object() : args);
            o.text = o.result.dump(-1, ' ', false, json::error_handler_t::replace);
            return o;
        }
        catch (const ToolError& e)
        {
            o.isError = true;
            o.code = e.code;
            o.text = e.message;
        }
        catch (const DbError& e)
        {
            o.isError = true;
            const std::string what = e.what();
            if (what.find("INTERRUPT") != std::string::npos || what.find("nterrupt") != std::string::npos)
            {
                // The caller cancelled, or the service's watchdog stopped the query at the budget.
                o.code = Proto::kErrTimeout;
                o.text = "The " + std::string{ name } + " call ran past its time budget and was stopped. Narrow it (fewer days, a folder, more words) and call again.";
            }
            else
            {
                LogSwallowedException((L"SessionDb tool " + Wide(name)).c_str());
                ServiceLog(L"[sessdb] tool " + Wide(name) + L" failed: " + Wide(what));
                o.code = Proto::kErrInternal;
                o.text = "The " + std::string{ name } + " call failed inside the database: " + what;
            }
        }
        catch (const std::exception& e)
        {
            LogSwallowedException((L"SessionDb tool " + Wide(name)).c_str());
            ServiceLog(L"[sessdb] tool " + Wide(name) + L" failed: " + Wide(e.what()));
            o.isError = true;
            o.code = Proto::kErrInternal;
            o.text = "The " + std::string{ name } + " call failed: " + std::string{ e.what() };
        }
        catch (...)
        {
            LogSwallowedException((L"SessionDb tool " + Wide(name)).c_str());
            o.isError = true;
            o.code = Proto::kErrInternal;
            o.text = "The " + std::string{ name } + " call failed.";
        }
        o.result = nullptr;
        return o;
    }
}
