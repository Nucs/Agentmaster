// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SESSIONS_STORE.md): the fleet store's database (FleetDb), the process-wide store a run opens at its start
// (OpenFleetForStartupIn: the checks, the one-time migration of sessions.json, the import of a sessions.json an older build
// wrote after it), and the read-only load of the other processes (LoadFleetReadOnlyIn). The writer thread that commits the
// registry's changes is FleetWriter.cpp: it needs the registry, and the CLI and the session database service link this file
// without it.

#include "FleetDb.h"

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "Json.h"
#include "Sha256.h"
#include "StateBackups.h" // CurrentWriter: the build named in meta 'writer' and in the migration record
#include "StateCompat.h" // the residue per session (ExtractResidue / ApplyResidue) and the loader's lookups (LookedKeys)
#include "StateFileGuard.h"

#include <windows.h>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <future>
#include <numeric>
#include <system_error>
#include <type_traits>
#include <unordered_set>

namespace
{
    using namespace Agentmaster;

    // ===== small helpers ======================================================================================================

    // UTF-16 → UTF-8 (SQLite's file names, the SHA-256 of a canonical text). A lone surrogate becomes U+FFFD, as every
    // state file's writer already makes it. Throws std::bad_alloc only.
    std::string FleetUtf8(std::wstring_view s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        }
        return out;
    }

    // UTF-8 → UTF-16 (a file's bytes, SQLite's error text). Invalid bytes become U+FFFD, as ReadStateFileUtf8 decodes them.
    // Throws std::bad_alloc only.
    std::wstring FleetUtf16(std::string_view s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0)
        {
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        }
        return out;
    }

    // Whether a FILE (not a folder) exists. Never throws.
    bool FleetFileExists(const std::wstring& path) noexcept
    {
        const DWORD a = ::GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // A file's size (0 when it is missing). Never throws.
    uint64_t FleetFileSize(const std::wstring& path) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
        {
            return 0;
        }
        return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    }

    // A whole file's bytes. The open shares read, write and delete, so the app's own atomic writes and a reader never block
    // each other. Returns false when the file cannot be opened or read (out is then empty). Never throws (an allocation
    // failure reads as unreadable).
    bool FleetReadAllBytes(const std::wstring& path, std::string& out) noexcept
    {
        out.clear();
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        bool ok = true;
        try
        {
            LARGE_INTEGER size{};
            if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (1LL << 31))
            {
                ok = false; // a 2 GB fleet file is not a fleet file: refuse rather than allocate it
            }
            else
            {
                out.resize(static_cast<size_t>(size.QuadPart));
                size_t done = 0;
                while (ok && done < out.size())
                {
                    DWORD got = 0;
                    const DWORD want = static_cast<DWORD>((std::min)(out.size() - done, size_t{ 1 } << 24));
                    if (!::ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0)
                    {
                        ok = false;
                    }
                    done += got;
                }
            }
        }
        catch (...)
        {
            ok = false; // std::bad_alloc: unreadable for this caller
        }
        ::CloseHandle(h);
        if (!ok)
        {
            out.clear();
        }
        return ok;
    }

    // FlushFileBuffers on a file through a handle of its own (a flush writes the file's cached data, whoever wrote it).
    // Returns true when flushed. Never throws.
    bool FleetFlushFile(const std::wstring& path) noexcept
    {
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        const BOOL ok = ::FlushFileBuffers(h);
        ::CloseHandle(h);
        return ok != FALSE;
    }

    // The local time as yyyyMMdd-HHmmss (folder and file name stamps). Never throws (swprintf into a fixed buffer).
    std::wstring FleetLocalStamp() noexcept
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        wchar_t buf[32]{};
        ::swprintf(buf, 32, L"%04u%02u%02u-%02u%02u%02u", static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
                   static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
        try
        {
            return buf;
        }
        catch (...)
        {
            return {};
        }
    }

    // Wall-clock Unix milliseconds (the migration record's time). Never throws.
    int64_t FleetWallUnixMs() noexcept
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return static_cast<int64_t>((t - 116444736000000000ULL) / 10000ULL);
    }

    // The row cache's fingerprint of a prompt text: a 64-bit hash, eight bytes (four UTF-16 units) per step, then a final
    // avalanche (murmur3's mixers). With the length beside it, a text that changed and kept both is a 1-in-2^64 event. It
    // never leaves the process (the cache is rebuilt at every load), so it may change between builds; it is word-wise
    // because the load hashes every stored text — 12 MB on production's fleet, where a byte-wise FNV cost ~25 ms of the
    // start. Never throws.
    uint64_t FleetTextHash(std::wstring_view s) noexcept
    {
        constexpr uint64_t c1 = 0x87C37B91114253D5ULL;
        constexpr uint64_t c2 = 0x4CF5AD432745937FULL;
        uint64_t h = 0x9E3779B97F4A7C15ULL ^ (static_cast<uint64_t>(s.size()) * 0xC2B2AE3D27D4EB4FULL);
        const size_t n = s.size();
        size_t i = 0;
        for (; i + 4 <= n; i += 4)
        {
            uint64_t k = 0;
            std::memcpy(&k, s.data() + i, sizeof(k)); // four code units; memcpy: no unaligned or aliasing read
            k *= c1;
            k = (k << 31) | (k >> 33);
            k *= c2;
            h ^= k;
            h = ((h << 27) | (h >> 37)) * 5 + 0x52DCE729ULL;
        }
        for (; i < n; ++i)
        {
            h ^= static_cast<uint64_t>(s[i]);
            h *= 0x100000001B3ULL;
        }
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33;
        return h;
    }

    // The build that runs, as "<version> <install>" (meta 'writer'). Never throws (an unknown build reads "unknown").
    std::wstring FleetWriterWord() noexcept
    {
        try
        {
            const auto w = StateBackups::CurrentWriter();
            if (w.version.empty())
            {
                return L"unknown";
            }
            return w.version + L" " + w.install;
        }
        catch (...)
        {
            return L"unknown";
        }
    }

    // Logs one line to hooks.log under the given tag. Never throws (AppendStateLog is best-effort).
    void FleetLog(const std::wstring& line) noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", line + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE: the log line itself could not be built (std::bad_alloc); LogSwallowedException would
            // build another line on the same failing allocator. Nothing to recover.
        }
    }

    // ===== JSON: the rows' exact text and the canonical comparison ============================================================

    // A value's JSON text. As json::Dump, except numbers that are not integers keep 17 significant digits (exact for a
    // double: "%g" keeps 6 and would change a value), and — with sortKeys — object members in key order (the canonical form
    // the migration's verification compares). Throws std::bad_alloc only.
    void FleetDumpExact(const json::Value& v, std::wstring& out, bool sortKeys)
    {
        switch (v.type)
        {
        case json::Value::Type::Null:
            out += L"null";
            break;
        case json::Value::Type::Bool:
            out += v.boolean ? L"true" : L"false";
            break;
        case json::Value::Type::Num:
        {
            if (!std::isfinite(v.num))
            {
                out += L"null"; // JSON has no NaN / infinity (json::Dump writes null too)
                break;
            }
            wchar_t buf[40]{};
            // An integer within int64's exact range prints as one (what every state file holds: ids, stamps, counts); the
            // range check comes first because converting a larger double to int64 is undefined.
            if (std::fabs(v.num) < 9.2e18 && v.num == static_cast<double>(static_cast<int64_t>(v.num)))
            {
                ::swprintf(buf, 40, L"%lld", static_cast<long long>(static_cast<int64_t>(v.num)));
            }
            else
            {
                ::swprintf(buf, 40, L"%.17g", v.num);
            }
            out += buf;
            break;
        }
        case json::Value::Type::Str:
            json::detail::Escape(v.str, out);
            break;
        case json::Value::Type::Arr:
            out += L'[';
            for (size_t k = 0; k < v.arr.size(); ++k)
            {
                if (k)
                {
                    out += L',';
                }
                FleetDumpExact(v.arr[k], out, sortKeys);
            }
            out += L']';
            break;
        case json::Value::Type::Obj:
        {
            out += L'{';
            std::vector<size_t> order(v.members.size());
            std::iota(order.begin(), order.end(), size_t{ 0 });
            if (sortKeys)
            {
                // Stable: two members with the same key keep their relative order on both sides of a comparison.
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return v.members[a].first < v.members[b].first; });
            }
            for (size_t k = 0; k < order.size(); ++k)
            {
                if (k)
                {
                    out += L',';
                }
                json::detail::Escape(v.members[order[k]].first, out);
                out += L':';
                FleetDumpExact(v.members[order[k]].second, out, sortKeys);
            }
            out += L'}';
            break;
        }
        }
    }

    // A row's stored text (member order kept). Throws std::bad_alloc only.
    std::wstring FleetRowText(const json::Value& v)
    {
        std::wstring out;
        FleetDumpExact(v, out, false);
        return out;
    }

    // ===== a session object ↔ its rows ========================================================================================

    // The separator of a synthesized row key (never in an id: ids are GUIDs and generated words).
    constexpr wchar_t kFleetKeySeparator = L'\x1F';

    // The spacing of the prompts' stored order (prompt_state.ord). The order only has to sort, so a prompt keeps its number
    // for life and a NEW or MOVED prompt takes one between its neighbours' (FleetPlanOrds): a removal — the history cap
    // trimming the oldest of 200 at every new prompt, a delete, a merge — then rewrites no other prompt's row, where numbers
    // 0, 1, 2… would rewrite every later prompt's.
    constexpr int64_t kFleetOrdGap = int64_t{ 1 } << 16;
    // Stored order numbers beyond this magnitude are renumbered (a damaged row, or 2^44 insertions at one end): it keeps
    // every sum and difference FleetPlanOrds makes far inside int64.
    constexpr int64_t kFleetOrdLimit = int64_t{ 1 } << 60;

    // One prompt's rows.
    struct FleetSplitPrompt
    {
        std::wstring key; // the row key (its id, or a synthesized one; see FleetSplitRows)
        int64_t pos{ 0 }; // its position in the queue (0-based); the stored order number is planned from it (FleetPlanOrds)
        std::wstring state; // every member but its string "text"
        bool hasText{ false }; // it had a string "text" member (stored in prompt_text; NULL otherwise)
        std::wstring text; // that text
    };

    // One session's rows.
    struct FleetSplit
    {
        bool hasQueue{ false }; // its first "queue" member was an array (taken out of the doc)
        std::wstring doc; // every other member, in order
        std::vector<FleetSplitPrompt> prompts; // the queue's elements, in order
    };

    // Takes a session object apart into its rows: the first "queue" member that is an array leaves the doc, and each element
    // becomes a state row (every member but its first string "text", which becomes a text row). A prompt's row key is its
    // "id" while that is a non-empty string not seen earlier in this queue; otherwise its id + U+001F + "#" + its position,
    // so the keys of one session are always distinct and the state doc keeps the real id. A non-object element is stored
    // whole as its state. Throws std::bad_alloc only.
    FleetSplit FleetSplitRows(const json::Value& session)
    {
        FleetSplit out;
        json::Value doc = json::Value::MkObj();
        const json::Value* queue = nullptr;
        // Members are walked directly, not through Find: this is not a loader, and a lookup observer must not see it.
        for (const auto& [key, value] : session.members)
        {
            if (!queue && key == L"queue" && value.type == json::Value::Type::Arr)
            {
                queue = &value;
                continue;
            }
            doc.members.emplace_back(key, value);
        }
        if (queue)
        {
            out.hasQueue = true;
            std::unordered_set<std::wstring> seen;
            int64_t ord = 0;
            for (const auto& element : queue->arr)
            {
                FleetSplitPrompt p;
                p.pos = ord;
                std::wstring id;
                bool idSeen = false;
                if (element.type == json::Value::Type::Obj)
                {
                    json::Value state = json::Value::MkObj();
                    for (const auto& [key, value] : element.members)
                    {
                        if (!p.hasText && key == L"text" && value.type == json::Value::Type::Str)
                        {
                            p.hasText = true;
                            p.text = value.str;
                            continue;
                        }
                        if (!idSeen && key == L"id" && value.type == json::Value::Type::Str)
                        {
                            idSeen = true;
                            id = value.str;
                        }
                        state.members.emplace_back(key, value);
                    }
                    p.state = FleetRowText(state);
                }
                else
                {
                    p.state = FleetRowText(element);
                }
                if (!id.empty() && seen.insert(id).second)
                {
                    p.key = id;
                }
                else
                {
                    p.key = id + kFleetKeySeparator + L"#" + std::to_wstring(ord);
                    seen.insert(p.key);
                }
                out.prompts.push_back(std::move(p));
                ++ord;
            }
        }
        out.doc = FleetRowText(doc);
        return out;
    }

    // One stored prompt as read back.
    struct FleetStoredPrompt
    {
        std::wstring key;
        int64_t ord{ 0 };
        std::wstring state;
        std::optional<std::wstring> text; // nullopt = NULL (the prompt had no string "text") or no text row
    };

    // One stored session as read back.
    struct FleetStored
    {
        std::wstring key; // its row key (the session id; a synthesized key for an empty or repeated id)
        int64_t ord{ 0 };
        bool hasQueue{ false };
        std::wstring doc;
        std::vector<FleetStoredPrompt> prompts; // in queue order
    };

    // Puts a session object back together from its rows — what FleetSplitRows took apart: the doc's members, then
    // "queue" (when it had one) with each element's "text" appended (when it had one). Member order differs from the
    // original (the queue and each text come last); every reader finds members by key. Returns false when the doc or a
    // state does not parse; a doc that parses to a non-object comes back as that value. Throws std::bad_alloc only.
    // FleetJoinRows' body. A NON-const `Rows` gives each prompt's text to the value (moved out of the rows: LoadAll, whose
    // rows are dropped after the join); a const one copies it (ReadRaw). Throws std::bad_alloc only.
    template<typename Rows>
    bool FleetJoinRowsImpl(Rows& rows, json::Value& out)
    {
        constexpr bool takeText = !std::is_const_v<Rows>;
        auto doc = json::Parse(rows.doc);
        if (!doc)
        {
            return false;
        }
        if (doc->type != json::Value::Type::Obj)
        {
            out = std::move(*doc); // a non-object record of the source array, stored whole
            return true;
        }
        if (rows.hasQueue)
        {
            auto arr = json::Value::MkArr();
            arr.arr.reserve(rows.prompts.size());
            for (auto& p : rows.prompts)
            {
                auto element = json::Parse(p.state);
                if (!element)
                {
                    return false;
                }
                if (p.text && element->type == json::Value::Type::Obj)
                {
                    if constexpr (takeText)
                    {
                        element->members.emplace_back(L"text", json::Value::MkStr(std::move(*p.text)));
                    }
                    else
                    {
                        element->members.emplace_back(L"text", json::Value::MkStr(*p.text));
                    }
                }
                arr.Push(std::move(*element));
            }
            doc->members.emplace_back(L"queue", std::move(arr));
        }
        out = std::move(*doc);
        return true;
    }

    bool FleetJoinRows(const FleetStored& rows, json::Value& out)
    {
        return FleetJoinRowsImpl(rows, out); // const: every text is copied, `rows` stays as it was
    }

    // FleetJoinRows that MOVES each prompt's text into the value (12 MB on production's fleet, not copied once more): the
    // rows' texts are empty afterwards. Throws std::bad_alloc only.
    bool FleetJoinRowsTakingTexts(FleetStored& rows, json::Value& out)
    {
        return FleetJoinRowsImpl(rows, out);
    }

    // Whether two JSON values are equal, objects compared as unordered member sets (LoadAll's fast path: a stored record and
    // this build's round trip of it hold their members in different orders — the queue and each text come last in a joined
    // record). Each object is checked in both directions, so a repeated key never passes for a missing one. Members are
    // looked up at the same position first (the usual case), then by a scan. Never throws.
    bool FleetSameValue(const json::Value& a, const json::Value& b) noexcept
    {
        if (a.type != b.type)
        {
            return false;
        }
        switch (a.type)
        {
        case json::Value::Type::Null:
            return true;
        case json::Value::Type::Bool:
            return a.boolean == b.boolean;
        case json::Value::Type::Num:
            return a.num == b.num || (a.num != a.num && b.num != b.num); // NaN equals NaN here (both stored as null)
        case json::Value::Type::Str:
            return a.str == b.str;
        case json::Value::Type::Arr:
            if (a.arr.size() != b.arr.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.arr.size(); ++i)
            {
                if (!FleetSameValue(a.arr[i], b.arr[i]))
                {
                    return false;
                }
            }
            return true;
        case json::Value::Type::Obj:
        {
            if (a.members.size() != b.members.size())
            {
                return false;
            }
            const auto find = [](const json::Value& obj, size_t hint, const std::wstring& key) -> const json::Value* {
                if (hint < obj.members.size() && obj.members[hint].first == key)
                {
                    return &obj.members[hint].second;
                }
                for (const auto& m : obj.members)
                {
                    if (m.first == key)
                    {
                        return &m.second;
                    }
                }
                return nullptr;
            };
            for (size_t i = 0; i < a.members.size(); ++i)
            {
                const json::Value* other = find(b, i, a.members[i].first);
                if (!other || !FleetSameValue(a.members[i].second, *other))
                {
                    return false;
                }
            }
            for (size_t i = 0; i < b.members.size(); ++i)
            {
                if (!find(a, i, b.members[i].first))
                {
                    return false;
                }
            }
            return true;
        }
        }
        return false;
    }

    // ===== SQLite ==============================================================================================================

    // SQLite's last error on a connection, as text. Never throws (an allocation failure gives "").
    std::wstring FleetSqliteError(sqlite3* db) noexcept
    {
        try
        {
            const void* m = db ? sqlite3_errmsg16(db) : nullptr;
            return m ? std::wstring{ static_cast<const wchar_t*>(m) } : std::wstring{ L"(no connection)" };
        }
        catch (...)
        {
            return {};
        }
    }

    // Runs SQL without results. Returns false with SQLite's message on failure. Never throws.
    bool FleetExec(sqlite3* db, const char* sql, std::wstring& error) noexcept
    {
        char* msg = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
        if (rc == SQLITE_OK)
        {
            return true;
        }
        try
        {
            error = msg ? FleetUtf16(msg) : FleetSqliteError(db);
        }
        catch (...)
        {
            error.clear();
        }
        sqlite3_free(msg);
        return false;
    }

    // A prepared statement (RAII). Text is bound and read as UTF-8 converted HERE (FleetUtf8 / FleetUtf16), never through
    // SQLite's own UTF-16 entry points: SQLite's UTF-16 → UTF-8 translation joins a high surrogate with whatever unit follows
    // it, so a lone one (a title cut inside an emoji) would swallow the next character — in a doc, often the closing quote,
    // and the row would no longer parse. Windows' conversion turns a lone surrogate into U+FFFD instead, which is exactly
    // what sessions.json's writer always stored, so the two stores keep the same text.
    class FleetStmt
    {
    public:
        FleetStmt(sqlite3* db, const char* sql) noexcept
        {
            _rc = sqlite3_prepare_v2(db, sql, -1, &_s, nullptr);
        }
        ~FleetStmt()
        {
            if (_s)
            {
                sqlite3_finalize(_s);
            }
        }
        FleetStmt(const FleetStmt&) = delete;
        FleetStmt& operator=(const FleetStmt&) = delete;

        bool Ok() const noexcept { return _rc == SQLITE_OK && _s != nullptr; }
        // Binds a text parameter (1-based). SQLITE_TRANSIENT: SQLite copies the bytes, so the temporary may go at once.
        // Throws std::bad_alloc only (the UTF-8 copy).
        void Text(int i, const std::wstring& v)
        {
            const std::string utf8 = FleetUtf8(v);
            sqlite3_bind_text(_s, i, utf8.data(), static_cast<int>(utf8.size()), SQLITE_TRANSIENT);
        }
        void Int(int i, int64_t v) noexcept { sqlite3_bind_int64(_s, i, v); }
        void Null(int i) noexcept { sqlite3_bind_null(_s, i); }
        int Step() noexcept { return sqlite3_step(_s); }
        // Runs a statement that returns no rows, then resets it for the next binding. True on success.
        bool Run() noexcept
        {
            const int rc = sqlite3_step(_s);
            sqlite3_reset(_s);
            sqlite3_clear_bindings(_s);
            return rc == SQLITE_DONE || rc == SQLITE_ROW;
        }
        // A column's text (the pointer first, then its byte count: SQLite's documented order; the count keeps an embedded
        // NUL). Throws std::bad_alloc only.
        std::wstring ColText(int c) const
        {
            const unsigned char* p = sqlite3_column_text(_s, c);
            const int n = sqlite3_column_bytes(_s, c);
            return p && n > 0 ? FleetUtf16(std::string_view{ reinterpret_cast<const char*>(p), static_cast<size_t>(n) }) : std::wstring{};
        }
        int64_t ColInt(int c) const noexcept { return sqlite3_column_int64(_s, c); }
        bool ColNull(int c) const noexcept { return sqlite3_column_type(_s, c) == SQLITE_NULL; }

    private:
        sqlite3_stmt* _s{ nullptr };
        int _rc{ SQLITE_ERROR };
    };

    // The schema (SESSIONS_STORE.md §4.2). Rowid tables: a 512-byte rowid leaf keeps ~477 bytes of a row in the page (a
    // WITHOUT ROWID leaf ~100, the rest spilling to overflow pages), so an UPDATE by id touches one leaf page. No index on
    // the prompts' order: a moved prompt then rewrites its row only (the load sorts in memory).
    constexpr const char* kFleetSchemaSql =
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions(id TEXT NOT NULL UNIQUE, ord INTEGER NOT NULL, has_queue INTEGER NOT NULL, doc TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS prompt_state(session_id TEXT NOT NULL, prompt_key TEXT NOT NULL, ord INTEGER NOT NULL, state TEXT NOT NULL);"
        "CREATE UNIQUE INDEX IF NOT EXISTS prompt_state_key ON prompt_state(session_id, prompt_key);"
        "CREATE TABLE IF NOT EXISTS prompt_text(session_id TEXT NOT NULL, prompt_key TEXT NOT NULL, text TEXT);"
        "CREATE UNIQUE INDEX IF NOT EXISTS prompt_text_key ON prompt_text(session_id, prompt_key);";

    // Sets one meta row (inside or outside a transaction). Never throws (an allocation failure is a failed write).
    bool FleetSetMetaRow(sqlite3* db, const std::wstring& key, const std::wstring& value) noexcept
    {
        try
        {
            FleetStmt s(db, "INSERT INTO meta(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
            if (!s.Ok())
            {
                return false;
            }
            s.Text(1, key);
            s.Text(2, value);
            return s.Run();
        }
        catch (...)
        {
            return false; // std::bad_alloc binding the text: the caller's transaction rolls back
        }
    }

    // A file name as an SQLite URI (file:///C:/...), for the immutable snapshot open: forward slashes, and the characters a
    // URI gives meaning to (% ? # and a space) percent-encoded. Throws std::bad_alloc only.
    std::string FleetFileUri(const std::wstring& path)
    {
        const std::string utf8 = FleetUtf8(path);
        std::string out = "file:///";
        for (const char c : utf8)
        {
            if (c == '\\')
            {
                out += '/';
            }
            else if (c == '%' || c == '?' || c == '#' || c == ' ')
            {
                char hex[4]{};
                std::snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += hex;
            }
            else
            {
                out += c;
            }
        }
        return out;
    }

    // Every stored session with its prompts (in queue order), sessions in stored order. Returns false with SQLite's message
    // when a read fails. Throws std::bad_alloc only.
    bool FleetReadStored(sqlite3* db, std::vector<FleetStored>& out, std::wstring& error)
    {
        out.clear();
        // Each prompt's state WITH its text, in one pass: the text found through prompt_text's unique index on (session_id,
        // prompt_key), so no map of every text is built first (12 MB of texts on production's fleet were copied into one,
        // then out of it). A text row that is NULL, and a prompt with no text row, both read as "no text member".
        std::unordered_map<std::wstring, std::vector<FleetStoredPrompt>> prompts;
        {
            FleetStmt s(db, "SELECT s.session_id, s.prompt_key, s.ord, s.state, t.text FROM prompt_state s "
                            "LEFT JOIN prompt_text t ON t.session_id = s.session_id AND t.prompt_key = s.prompt_key");
            if (!s.Ok())
            {
                error = FleetSqliteError(db);
                return false;
            }
            std::wstring sid;
            std::vector<FleetStoredPrompt>* bucket = nullptr; // rows of one session usually come together: one lookup per run
            for (;;)
            {
                const int rc = s.Step();
                if (rc == SQLITE_DONE)
                {
                    break;
                }
                if (rc != SQLITE_ROW)
                {
                    error = FleetSqliteError(db);
                    return false;
                }
                FleetStoredPrompt p;
                std::wstring rowSid = s.ColText(0);
                p.key = s.ColText(1);
                p.ord = s.ColInt(2);
                p.state = s.ColText(3);
                if (!s.ColNull(4))
                {
                    p.text = s.ColText(4);
                }
                if (!bucket || rowSid != sid)
                {
                    sid = std::move(rowSid);
                    bucket = &prompts[sid];
                }
                bucket->push_back(std::move(p));
            }
        }
        {
            FleetStmt s(db, "SELECT id, ord, has_queue, doc FROM sessions ORDER BY ord, rowid");
            if (!s.Ok())
            {
                error = FleetSqliteError(db);
                return false;
            }
            for (;;)
            {
                const int rc = s.Step();
                if (rc == SQLITE_DONE)
                {
                    break;
                }
                if (rc != SQLITE_ROW)
                {
                    error = FleetSqliteError(db);
                    return false;
                }
                FleetStored r;
                r.key = s.ColText(0);
                r.ord = s.ColInt(1);
                r.hasQueue = s.ColInt(2) != 0;
                r.doc = s.ColText(3);
                if (const auto p = prompts.find(r.key); p != prompts.end())
                {
                    r.prompts = std::move(p->second);
                    std::stable_sort(r.prompts.begin(), r.prompts.end(), [](const FleetStoredPrompt& a, const FleetStoredPrompt& b) { return a.ord < b.ord; });
                }
                out.push_back(std::move(r));
            }
        }
        return true;
    }

    // Whether a session's split rows equal what the cache says is stored (then a write needs no SQL): the same doc, the same
    // prompts with the same states and texts, and their stored order numbers rising in the queue's order (the numbers
    // themselves only have to sort; see kFleetOrdGap).
    bool FleetSameRows(const FleetDb::CachedSession& cached, const FleetSplit& split) noexcept
    {
        if (cached.hasQueue != split.hasQueue || cached.doc != split.doc || cached.prompts.size() != split.prompts.size())
        {
            return false;
        }
        bool first = true;
        int64_t last = 0;
        for (const auto& p : split.prompts)
        {
            const auto c = cached.prompts.find(p.key);
            if (c == cached.prompts.end() || c->second.state != p.state || c->second.hasText != p.hasText || (!first && c->second.ord <= last))
            {
                return false;
            }
            if (p.hasText && (c->second.textLength != p.text.size() || c->second.textHash != FleetTextHash(p.text)))
            {
                return false;
            }
            first = false;
            last = c->second.ord;
        }
        return true;
    }

    // The order numbers to store for a queue (see kFleetOrdGap): `cached[k]` is the number the k-th prompt of the NEW order
    // is stored with (nullopt = a new prompt). The longest run of stored numbers that already rises in the new order keeps
    // its numbers; every other prompt (new, or moved past that run) takes a number between its kept neighbours', spread
    // evenly, or a gap's step beyond an end. Only when two kept neighbours leave too little room, or a stored number is out of
    // range, is the whole queue renumbered (k+1)·gap. The result rises strictly. Throws std::bad_alloc only.
    std::vector<int64_t> FleetPlanOrds(const std::vector<std::optional<int64_t>>& cached)
    {
        const size_t n = cached.size();
        std::vector<int64_t> out(n, 0);
        const auto renumberAll = [&]() {
            for (size_t k = 0; k < n; ++k)
            {
                out[k] = static_cast<int64_t>(k + 1) * kFleetOrdGap;
            }
            return out;
        };
        for (const auto& c : cached)
        {
            if (c && (*c > kFleetOrdLimit || *c < -kFleetOrdLimit))
            {
                return renumberAll(); // a number this arithmetic must not touch
            }
        }
        // The longest strictly rising subsequence of the stored numbers (patience sorting, O(n log n)): `tails[L]` is the
        // index whose number ends the best rising run of length L+1 found so far, `prev` links each index to the one before
        // it in its run.
        constexpr size_t kNone = static_cast<size_t>(-1);
        std::vector<size_t> tails;
        std::vector<size_t> prev(n, kNone);
        for (size_t i = 0; i < n; ++i)
        {
            if (!cached[i])
            {
                continue;
            }
            const int64_t v = *cached[i];
            const auto it = std::lower_bound(tails.begin(), tails.end(), v, [&](size_t t, int64_t x) { return *cached[t] < x; });
            if (it != tails.begin())
            {
                prev[i] = *(it - 1);
            }
            if (it == tails.end())
            {
                tails.push_back(i);
            }
            else
            {
                *it = i;
            }
        }
        std::vector<bool> keep(n, false);
        if (!tails.empty())
        {
            for (size_t i = tails.back(); i != kNone; i = prev[i])
            {
                keep[i] = true;
                out[i] = *cached[i];
            }
        }
        // Every maximal run of prompts that are not kept gets numbers inside the room its kept neighbours leave.
        for (size_t i = 0; i < n;)
        {
            if (keep[i])
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < n && !keep[j])
            {
                ++j;
            }
            const size_t run = j - i; // prompts [i, j) need numbers
            const bool hasLow = i > 0; // out[i-1] is a kept number (the runs are maximal)
            const bool hasHigh = j < n; // out[j] is a kept number
            if (hasLow && hasHigh)
            {
                const int64_t lo = out[i - 1];
                const int64_t hi = out[j];
                if (hi - lo <= static_cast<int64_t>(run))
                {
                    return renumberAll(); // no room between them: the rare renumbering
                }
                const int64_t step = (hi - lo) / static_cast<int64_t>(run + 1); // >= 1, and lo + step*run < hi
                for (size_t k = 0; k < run; ++k)
                {
                    out[i + k] = lo + step * static_cast<int64_t>(k + 1);
                }
            }
            else if (hasLow)
            {
                for (size_t k = 0; k < run; ++k)
                {
                    out[i + k] = out[i - 1] + kFleetOrdGap * static_cast<int64_t>(k + 1);
                }
            }
            else if (hasHigh)
            {
                for (size_t k = 0; k < run; ++k)
                {
                    out[i + k] = out[j] - kFleetOrdGap * static_cast<int64_t>(run - k);
                }
            }
            else
            {
                return renumberAll(); // nothing kept: a new queue
            }
            i = j;
        }
        for (const int64_t v : out)
        {
            if (v > kFleetOrdLimit || v < -kFleetOrdLimit)
            {
                return renumberAll(); // drifted to the edge after very many insertions at one end
            }
        }
        return out;
    }

    // The cache entry that describes a session's split rows once they are stored with the given prompt order numbers
    // (`ords[k]` is the k-th prompt's).
    FleetDb::CachedSession FleetCacheOf(int64_t ord, const FleetSplit& split, const std::vector<int64_t>& ords)
    {
        FleetDb::CachedSession c;
        c.ord = ord;
        c.hasQueue = split.hasQueue;
        c.doc = split.doc;
        for (size_t k = 0; k < split.prompts.size(); ++k)
        {
            const auto& p = split.prompts[k];
            FleetDb::CachedPrompt cp;
            cp.ord = ords[k];
            cp.state = p.state;
            cp.hasText = p.hasText;
            if (p.hasText)
            {
                cp.textHash = FleetTextHash(p.text);
                cp.textLength = p.text.size();
            }
            c.prompts.emplace(p.key, std::move(cp));
        }
        return c;
    }
}

namespace Agentmaster
{
    std::vector<int64_t> PlanFleetPromptOrdsForTests(const std::vector<std::optional<int64_t>>& cached)
    {
        return FleetPlanOrds(cached);
    }
}

namespace Agentmaster
{
    std::wstring CanonicalJsonText(const json::Value& v)
    {
        std::wstring out;
        FleetDumpExact(v, out, true);
        return out;
    }

    // ===== FleetDb ============================================================================================================

    std::unique_ptr<FleetDb> FleetDb::Open(const std::wstring& path, FleetDbAccess access, bool create, std::wstring& problem) noexcept
    {
        problem.clear();
        try
        {
            std::unique_ptr<FleetDb> self{ new FleetDb() };
            self->_path = path;
            const bool exists = FleetFileExists(path);
            if (!exists && (!create || access != FleetDbAccess::ReadWrite))
            {
                problem = L"the file does not exist";
                return nullptr;
            }
            bool readOnly = access != FleetDbAccess::ReadWrite;
            if (!readOnly)
            {
                // The one-writer lock (the outbox's .lock, MESSAGING_OUTBOX.md §5.5): no sharing, deleted when its handle
                // closes — a dead process leaves none behind, and a second Agentmaster on the same profile cannot take it.
                const HANDLE lock = ::CreateFileW((path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                                  FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (lock == INVALID_HANDLE_VALUE)
                {
                    const DWORD e = ::GetLastError();
                    if (!exists)
                    {
                        problem = L"its lock file could not be created (Windows error " + std::to_wstring(e) + L")";
                        return nullptr; // never create a store without holding its lock
                    }
                    readOnly = true;
                    self->_readOnlyReason = e == ERROR_SHARING_VIOLATION ? std::wstring{ L"another Agentmaster process holds sessions.db.lock" } :
                                                                           (L"its lock file could not be opened (Windows error " + std::to_wstring(e) + L")");
                }
                else
                {
                    self->_lock = lock;
                }
            }
            int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | (exists ? 0 : SQLITE_OPEN_CREATE));
            std::string name;
            if (access == FleetDbAccess::Snapshot)
            {
                // immutable: SQLite reads the file alone — no lock, no -wal, no -shm — which is right only because no
                // writer exists (the migration closed its writer after a TRUNCATE checkpoint).
                flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
                name = FleetFileUri(path) + "?mode=ro&immutable=1";
            }
            else
            {
                name = FleetUtf8(path);
            }
            if (sqlite3_open_v2(name.c_str(), &self->_db, flags, nullptr) != SQLITE_OK)
            {
                problem = L"SQLite could not open it: " + FleetSqliteError(self->_db);
                return nullptr; // the destructor closes the half-made handle
            }
            sqlite3_busy_timeout(self->_db, 5000); // a reader's checkpoint or another reader never fails a call outright
            std::wstring err;
            // Memory-mapped reads: SQLite reads pages straight from the mapping instead of a ReadFile per page. With 512-byte
            // pages the startup load and the quick check touch ~38,000 pages of production's 19 MB fleet: measured 2026-11-20
            // on a copy of it, the read went from ~300 to ~85 ms and the quick check from ~260 to ~50 ms. Writes still go
            // through the file (SQLite never writes through the mapping by default). The cap is address space, not memory:
            // only the file's size is mapped. The trade-off: a disk that fails to page the file in raises an in-page error
            // instead of a SQLITE_IOERR — on a local profile, a failing disk loses the run either way.
            FleetExec(self->_db, "PRAGMA mmap_size=268435456", err);
            if (!exists)
            {
                // A new file: the page size BEFORE the first table (fixed from then on), then WAL (persistent in the header),
                // then the schema and the meta rows in one transaction.
                const bool made = FleetExec(self->_db, "PRAGMA page_size=512", err) && FleetExec(self->_db, "PRAGMA journal_mode=WAL", err) &&
                                  FleetExec(self->_db, "BEGIN IMMEDIATE", err) && FleetExec(self->_db, kFleetSchemaSql, err) &&
                                  FleetSetMetaRow(self->_db, L"format", std::to_wstring(kFleetDbFormat)) &&
                                  FleetSetMetaRow(self->_db, L"created", std::to_wstring(FleetWallUnixMs()) + L" " + FleetWriterWord()) && FleetExec(self->_db, "COMMIT", err);
                if (!made)
                {
                    problem = L"its schema could not be created: " + (err.empty() ? FleetSqliteError(self->_db) : err);
                    return nullptr;
                }
            }
            if (!readOnly)
            {
                // Checkpoints run on the writer thread (FleetWriter.cpp), never inside a caller's commit.
                FleetExec(self->_db, "PRAGMA wal_autocheckpoint=0", err);
            }
            FleetExec(self->_db, "PRAGMA trusted_schema=OFF", err); // no SQL function in a schema we did not write may run
            const std::wstring format = self->Meta(L"format");
            if (format.empty())
            {
                problem = L"it holds no format record (not a fleet database, or a damaged one)";
                return nullptr;
            }
            if (std::wcstol(format.c_str(), nullptr, 10) > kFleetDbFormat && !readOnly)
            {
                // §17's format guard: a newer build's store is read, never overwritten.
                readOnly = true;
                self->_readOnlyReason = L"its format (" + format + L") is newer than this build's (" + std::to_wstring(kFleetDbFormat) + L")";
            }
            self->_readOnly = readOnly;
            return self;
        }
        catch (...)
        {
            LogSwallowedException(L"FleetDb::Open"); // Rule #18. Recovery: no connection; the caller decides the mode.
            problem = L"it could not be opened (an exception, logged)";
            return nullptr;
        }
    }

    FleetDb::~FleetDb()
    {
        Close();
    }

    void FleetDb::Close() noexcept
    {
        std::lock_guard lk{ _mtx };
        if (_db)
        {
            sqlite3_close_v2(_db); // v2: a statement left open cannot keep the file busy forever (none should be)
            _db = nullptr;
        }
        if (_lock)
        {
            ::CloseHandle(static_cast<HANDLE>(_lock)); // FILE_FLAG_DELETE_ON_CLOSE removes the lock file
            _lock = nullptr;
        }
    }

    std::wstring FleetDb::Meta(const std::wstring& key) const noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            if (!_db)
            {
                return {};
            }
            FleetStmt s(_db, "SELECT value FROM meta WHERE key = ?1");
            if (!s.Ok())
            {
                return {};
            }
            s.Text(1, key);
            return s.Step() == SQLITE_ROW ? s.ColText(0) : std::wstring{};
        }
        catch (...)
        {
            return {}; // std::bad_alloc: unreadable reads as absent
        }
    }

    bool FleetDb::Check(bool full, std::wstring& problem) const noexcept
    {
        problem.clear();
        try
        {
            std::lock_guard lk{ _mtx };
            if (!_db)
            {
                problem = L"the connection is closed";
                return false;
            }
            FleetStmt s(_db, full ? "PRAGMA integrity_check" : "PRAGMA quick_check");
            if (!s.Ok())
            {
                problem = FleetSqliteError(_db);
                return false;
            }
            std::wstring answer;
            for (int rc = s.Step(); rc == SQLITE_ROW; rc = s.Step())
            {
                answer += (answer.empty() ? L"" : L"; ") + s.ColText(0);
                if (answer.size() > 600)
                {
                    break; // the first problems say enough
                }
            }
            if (answer == L"ok")
            {
                return true;
            }
            problem = answer.empty() ? FleetSqliteError(_db) : answer;
            return false;
        }
        catch (...)
        {
            problem = L"the check could not run (an exception)";
            return false;
        }
    }

    bool FleetDb::LoadAll(std::vector<SessionInfo>& out, std::wstring& problem)
    {
        std::lock_guard lk{ _mtx };
        out.clear();
        problem.clear();
        if (!_db)
        {
            problem = L"the connection is closed";
            return false;
        }
        std::vector<FleetStored> rows;
        std::wstring error;
        if (!FleetReadStored(_db, rows, error))
        {
            problem = L"a read failed: " + error;
            return false;
        }
        _cache.clear();
        _residue.clear();
        _nextOrd = 0;
        size_t skipped = 0;
        out.reserve(rows.size());
        std::vector<std::pair<uint64_t, size_t>> prints; // one session's text fingerprints (reused)
        for (auto& r : rows)
        {
            _nextOrd = (std::max)(_nextOrd, r.ord + 1);
            // The texts' fingerprints for the cache FIRST: the join below moves the texts out of the rows (they are not
            // copied once more — 12 MB on production's fleet).
            prints.clear();
            for (const auto& p : r.prompts)
            {
                prints.emplace_back(p.text ? FleetTextHash(*p.text) : 0, p.text ? p.text->size() : 0);
            }
            json::Value obj;
            if (!FleetJoinRowsTakingTexts(r, obj) || obj.type != json::Value::Type::Obj)
            {
                // A row this build cannot read stays on disk exactly as it is: it is never cached, so no write of this run
                // ever touches it (the registry never holds it either; a new record with its id moves it aside: Write).
                ++skipped;
                continue;
            }
            // This build's record. The FAST PATH: when the stored record is exactly what this build makes of it (its round
            // trip, compared as values) — every session this build or the migration from its own sessions.json wrote — the
            // rows hold nothing this build does not represent, so there is no residue to keep and no lookup to record. Only a
            // record that differs (a newer build's members, a value this build cannot represent) pays the residue's
            // extraction, which costs about as much as the rest of the load together.
            SessionInfo s = SessionFromJson(obj);
            json::Value roundTrip = ToJson(s);
            if (!FleetSameValue(obj, roundTrip))
            {
                // The record with the lookups its loader made: a member it looked at is understood, the rest (a newer build's)
                // is the residue every write of this session puts back (PERSISTENCE.md §17, per row).
                StateCompat::LookedKeys looked;
                {
                    StateCompat::ObserveLookups observe{ looked };
                    s = SessionFromJson(obj);
                }
                roundTrip = ToJson(s);
                auto residue = StateCompat::ExtractResidue(obj, roundTrip, true, looked.Complete() ? &looked : nullptr);
                if (!residue.empty())
                {
                    _residue[r.key] = std::make_shared<const StateCompat::ResidueNode>(std::move(residue));
                }
            }
            // The cache: the rows AS STORED (the doc and the states moved in: the rows are dropped after this loop).
            CachedSession c;
            c.ord = r.ord;
            c.hasQueue = r.hasQueue;
            c.doc = std::move(r.doc);
            c.prompts.reserve(r.prompts.size());
            for (size_t k = 0; k < r.prompts.size(); ++k)
            {
                auto& p = r.prompts[k];
                CachedPrompt cp;
                cp.ord = p.ord;
                cp.state = std::move(p.state);
                cp.hasText = p.text.has_value(); // a moved-from text still says it was there
                cp.textHash = prints[k].first;
                cp.textLength = prints[k].second;
                c.prompts.emplace(std::move(p.key), std::move(cp));
            }
            _cache[r.key] = std::move(c);
            out.push_back(std::move(s));
        }
        if (skipped)
        {
            problem = std::to_wstring(skipped) + L" stored session(s) could not be read; they are kept in the database as they are (a new record with one of " +
                      L"their ids moves them under a key of their own)";
        }
        return true;
    }

    FleetWriteResult FleetDb::Write(const std::vector<FleetChange>& changes, bool durable) noexcept
    {
        FleetWriteResult result;
        std::lock_guard lk{ _mtx };
        if (_readOnly || !_db || changes.empty())
        {
            return result;
        }
        bool inTransaction = false;
        try
        {
            // 1. Plan OUTSIDE the transaction: serialize each record and compare its rows with the cache. A list naming a
            //    session twice writes its LAST record.
            struct Planned
            {
                std::wstring id;
                bool remove{ false };
                bool exists{ false };
                FleetSplit split;
                bool residueKnown{ false }; // the record had a residue: `carried` replaces it after the commit
                std::shared_ptr<const StateCompat::ResidueNode> carried;
            };
            std::vector<Planned> plan;
            std::unordered_set<std::wstring> planned;
            for (auto it = changes.rbegin(); it != changes.rend(); ++it)
            {
                if (!planned.insert(it->id).second)
                {
                    continue;
                }
                Planned p;
                p.id = it->id;
                const auto cached = _cache.find(it->id);
                p.exists = cached != _cache.end();
                if (!it->record)
                {
                    if (p.exists)
                    {
                        p.remove = true;
                        plan.push_back(std::move(p));
                    }
                    continue;
                }
                json::Value fresh = ToJson(*it->record);
                if (const auto r = _residue.find(it->id); r != _residue.end())
                {
                    // What a newer build wrote goes back into this build's record (§17), and what the record now carries
                    // replaces the residue once the commit is through.
                    auto carried = StateCompat::ApplyResidue(fresh, *r->second);
                    p.residueKnown = true;
                    if (!carried.empty())
                    {
                        p.carried = std::make_shared<const StateCompat::ResidueNode>(std::move(carried));
                    }
                }
                p.split = FleetSplitRows(fresh);
                if (p.exists && FleetSameRows(cached->second, p.split))
                {
                    if (p.residueKnown)
                    {
                        // Nothing to write, but the record's residue is what it carries now (its rows already hold it).
                        if (p.carried)
                        {
                            _residue[p.id] = p.carried;
                        }
                        else
                        {
                            _residue.erase(p.id);
                        }
                    }
                    continue;
                }
                plan.push_back(std::move(p));
            }
            if (plan.empty())
            {
                return result;
            }
            // The plan was built from the END of the list (so a session named twice writes its last record): put it back in
            // the list's order, so new sessions take their stored order (sessions.ord) in the order the caller listed them
            // — the registry's insertion order, which the next start's restore rebuilds from it.
            std::reverse(plan.begin(), plan.end());

            // 2. One transaction. The synchronous level is a connection setting, so it is set before BEGIN.
            std::wstring err;
            if (!FleetExec(_db, durable ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL", err) || !FleetExec(_db, "BEGIN IMMEDIATE", err))
            {
                result.ok = false;
                result.error = err;
                return result;
            }
            inTransaction = true;
            FleetStmt upsertSession(_db, "INSERT INTO sessions(id, ord, has_queue, doc) VALUES(?1, ?2, ?3, ?4) "
                                    "ON CONFLICT(id) DO UPDATE SET has_queue = excluded.has_queue, doc = excluded.doc");
            FleetStmt updateSession(_db, "UPDATE sessions SET has_queue = ?2, doc = ?3 WHERE id = ?1");
            FleetStmt deleteSession(_db, "DELETE FROM sessions WHERE id = ?1");
            FleetStmt deleteStates(_db, "DELETE FROM prompt_state WHERE session_id = ?1");
            FleetStmt deleteTexts(_db, "DELETE FROM prompt_text WHERE session_id = ?1");
            FleetStmt upsertState(_db, "INSERT INTO prompt_state(session_id, prompt_key, ord, state) VALUES(?1, ?2, ?3, ?4) "
                                  "ON CONFLICT(session_id, prompt_key) DO UPDATE SET ord = excluded.ord, state = excluded.state");
            FleetStmt deleteState(_db, "DELETE FROM prompt_state WHERE session_id = ?1 AND prompt_key = ?2");
            FleetStmt upsertText(_db, "INSERT INTO prompt_text(session_id, prompt_key, text) VALUES(?1, ?2, ?3) "
                                 "ON CONFLICT(session_id, prompt_key) DO UPDATE SET text = excluded.text");
            FleetStmt deleteText(_db, "DELETE FROM prompt_text WHERE session_id = ?1 AND prompt_key = ?2");
            // A NEW record never mixes with rows already stored under its id — a row this build could not read at the load
            // (it was never cached), or prompt rows whose session row is gone: those move under a key of their own, kept.
            FleetStmt keepSession(_db, "UPDATE sessions SET id = ?2 WHERE id = ?1");
            FleetStmt keepStates(_db, "UPDATE prompt_state SET session_id = ?2 WHERE session_id = ?1");
            FleetStmt keepTexts(_db, "UPDATE prompt_text SET session_id = ?2 WHERE session_id = ?1");
            const bool prepared = upsertSession.Ok() && updateSession.Ok() && deleteSession.Ok() && deleteStates.Ok() && deleteTexts.Ok() &&
                                  upsertState.Ok() && deleteState.Ok() && upsertText.Ok() && deleteText.Ok() && keepSession.Ok() && keepStates.Ok() &&
                                  keepTexts.Ok();
            const auto fail = [&]() {
                result.ok = false;
                result.error = FleetSqliteError(_db);
                std::wstring ignored;
                FleetExec(_db, "ROLLBACK", ignored);
                inTransaction = false;
                return result;
            };
            if (!prepared)
            {
                return fail();
            }
            int64_t nextOrd = _nextOrd;
            std::unordered_map<std::wstring, CachedSession> staged; // id → its cache entry once committed
            staged.reserve(plan.size());
            std::vector<std::wstring> keptAside; // keys rows were moved under (see keepSession), for the log
            for (auto& p : plan)
            {
                if (p.remove)
                {
                    for (FleetStmt* s : { &deleteSession, &deleteStates, &deleteTexts })
                    {
                        s->Text(1, p.id);
                        if (!s->Run())
                        {
                            return fail();
                        }
                        result.rowsDeleted += static_cast<size_t>(sqlite3_changes(_db));
                    }
                    continue;
                }
                const CachedSession* cached = nullptr;
                if (const auto c = _cache.find(p.id); c != _cache.end())
                {
                    cached = &c->second;
                }
                const int64_t ord = cached ? cached->ord : nextOrd++;
                // The prompts' stored order numbers: kept where the order did not change (FleetPlanOrds), so a removal or a
                // move rewrites only the prompts it moved.
                std::vector<std::optional<int64_t>> storedOrds;
                storedOrds.reserve(p.split.prompts.size());
                for (const auto& sp : p.split.prompts)
                {
                    std::optional<int64_t> stored;
                    if (cached)
                    {
                        if (const auto c = cached->prompts.find(sp.key); c != cached->prompts.end())
                        {
                            stored = c->second.ord;
                        }
                    }
                    storedOrds.push_back(stored);
                }
                const std::vector<int64_t> ords = FleetPlanOrds(storedOrds);
                if (!cached)
                {
                    // Rows under this id that the cache does not know (see keepSession) move aside first; the UPDATEs change
                    // nothing when there are none, which is the normal case.
                    const std::wstring keptKey = p.id + kFleetKeySeparator + L"kept-" + std::to_wstring(FleetWallUnixMs()) + L"-" + std::to_wstring(++_keptSeq);
                    size_t moved = 0;
                    for (FleetStmt* s : { &keepSession, &keepStates, &keepTexts })
                    {
                        s->Text(1, p.id);
                        s->Text(2, keptKey);
                        if (!s->Run())
                        {
                            return fail();
                        }
                        moved += static_cast<size_t>(sqlite3_changes(_db));
                    }
                    if (moved)
                    {
                        result.rowsWritten += moved;
                        keptAside.push_back(keptKey); // logged once the commit is through
                    }
                    upsertSession.Text(1, p.id);
                    upsertSession.Int(2, ord);
                    upsertSession.Int(3, p.split.hasQueue ? 1 : 0);
                    upsertSession.Text(4, p.split.doc);
                    if (!upsertSession.Run())
                    {
                        return fail();
                    }
                    ++result.rowsWritten;
                }
                else if (cached->doc != p.split.doc || cached->hasQueue != p.split.hasQueue)
                {
                    updateSession.Text(1, p.id);
                    updateSession.Int(2, p.split.hasQueue ? 1 : 0);
                    updateSession.Text(3, p.split.doc);
                    if (!updateSession.Run())
                    {
                        return fail();
                    }
                    ++result.rowsWritten;
                }
                std::unordered_set<std::wstring> keys;
                for (size_t k = 0; k < p.split.prompts.size(); ++k)
                {
                    const auto& sp = p.split.prompts[k];
                    keys.insert(sp.key);
                    const CachedPrompt* cp = nullptr;
                    if (cached)
                    {
                        if (const auto c = cached->prompts.find(sp.key); c != cached->prompts.end())
                        {
                            cp = &c->second;
                        }
                    }
                    if (!cp || cp->ord != ords[k] || cp->state != sp.state)
                    {
                        upsertState.Text(1, p.id);
                        upsertState.Text(2, sp.key);
                        upsertState.Int(3, ords[k]);
                        upsertState.Text(4, sp.state);
                        if (!upsertState.Run())
                        {
                            return fail();
                        }
                        ++result.rowsWritten;
                    }
                    const bool textChanged = !cp || cp->hasText != sp.hasText ||
                                             (sp.hasText && (cp->textLength != sp.text.size() || cp->textHash != FleetTextHash(sp.text)));
                    if (textChanged)
                    {
                        // Every prompt has a text row: its text, or NULL when it had no string "text" (so the load puts
                        // back exactly the members it had).
                        upsertText.Text(1, p.id);
                        upsertText.Text(2, sp.key);
                        if (sp.hasText)
                        {
                            upsertText.Text(3, sp.text);
                        }
                        else
                        {
                            upsertText.Null(3);
                        }
                        if (!upsertText.Run())
                        {
                            return fail();
                        }
                        ++result.rowsWritten;
                    }
                }
                if (cached)
                {
                    for (const auto& entry : cached->prompts)
                    {
                        const std::wstring& key = entry.first;
                        if (keys.count(key))
                        {
                            continue;
                        }
                        // A prompt the queue no longer holds (sent history trimmed past its cap, deleted, merged): its rows go.
                        for (FleetStmt* s : { &deleteState, &deleteText })
                        {
                            s->Text(1, p.id);
                            s->Text(2, key);
                            if (!s->Run())
                            {
                                return fail();
                            }
                            result.rowsDeleted += static_cast<size_t>(sqlite3_changes(_db));
                        }
                    }
                }
                staged.insert_or_assign(p.id, FleetCacheOf(ord, p.split, ords));
            }
            // Everything the cache update below needs is allocated BEFORE the commit (the staged entries, room for the new
            // ones), so once the commit is through, bringing the cache in line allocates nothing and cannot fail half-way: a
            // cache that disagreed with the disk would make a later write skip a changed row or move a stored one aside.
            _cache.reserve(_cache.size() + staged.size());
            if (!FleetExec(_db, "COMMIT", err))
            {
                return fail();
            }
            inTransaction = false;

            // 3. What is on disk now is what the cache says. (No allocation from here on: erase, a move-assignment, a
            //    node-handle insert into reserved room, a shared_ptr copy over an existing key.)
            for (auto& p : plan)
            {
                if (p.remove)
                {
                    _cache.erase(p.id);
                    _residue.erase(p.id);
                    continue;
                }
                if (p.residueKnown)
                {
                    if (p.carried)
                    {
                        _residue[p.id] = p.carried; // residueKnown: the key is in _residue already
                    }
                    else
                    {
                        _residue.erase(p.id);
                    }
                }
            }
            for (auto it = staged.begin(); it != staged.end();)
            {
                const auto next = std::next(it);
                if (const auto existing = _cache.find(it->first); existing != _cache.end())
                {
                    existing->second = std::move(it->second);
                }
                else
                {
                    _cache.insert(staged.extract(it));
                }
                it = next;
            }
            _nextOrd = nextOrd;
            for (const auto& key : keptAside)
            {
                try
                {
                    FleetLog(L"[fleet] a new record found rows under its id that this run had not loaded; they are kept as \"" + key + L"\"");
                }
                catch (...)
                {
                    // ⚠ DELIBERATELY BARE: only the log line could not be built (std::bad_alloc); the rows are committed.
                }
            }
            return result;
        }
        catch (...)
        {
            // Rule #18. Recovery: the transaction rolls back; the cache was not touched, so the caller's retry writes the
            // same rows again.
            LogSwallowedException(L"FleetDb::Write");
            if (inTransaction)
            {
                std::wstring ignored;
                FleetExec(_db, "ROLLBACK", ignored);
            }
            result.ok = false;
            result.error = L"an exception (logged)";
            return result;
        }
    }

    bool FleetDb::ImportRaw(const std::vector<const json::Value*>& sessions, const std::vector<std::pair<std::wstring, std::wstring>>& meta,
                            std::vector<std::wstring>& imported, std::wstring& problem) noexcept
    {
        imported.clear();
        problem.clear();
        std::lock_guard lk{ _mtx };
        if (_readOnly || !_db)
        {
            problem = _db ? L"the store is read-only" : L"the connection is closed";
            return false;
        }
        bool inTransaction = false;
        try
        {
            std::unordered_set<std::wstring> taken;
            int64_t nextOrd = 0;
            {
                FleetStmt s(_db, "SELECT id, ord FROM sessions");
                if (!s.Ok())
                {
                    problem = FleetSqliteError(_db);
                    return false;
                }
                for (int rc = s.Step(); rc == SQLITE_ROW; rc = s.Step())
                {
                    taken.insert(s.ColText(0));
                    nextOrd = (std::max)(nextOrd, s.ColInt(1) + 1);
                }
            }
            std::wstring err;
            if (!FleetExec(_db, "PRAGMA synchronous=FULL", err) || !FleetExec(_db, "BEGIN IMMEDIATE", err))
            {
                problem = err;
                return false;
            }
            inTransaction = true;
            FleetStmt insertSession(_db, "INSERT INTO sessions(id, ord, has_queue, doc) VALUES(?1, ?2, ?3, ?4)");
            FleetStmt insertState(_db, "INSERT INTO prompt_state(session_id, prompt_key, ord, state) VALUES(?1, ?2, ?3, ?4)");
            FleetStmt insertText(_db, "INSERT INTO prompt_text(session_id, prompt_key, text) VALUES(?1, ?2, ?3)");
            const auto fail = [&]() {
                problem = FleetSqliteError(_db);
                std::wstring ignored;
                FleetExec(_db, "ROLLBACK", ignored);
                inTransaction = false;
                imported.clear();
                return false;
            };
            if (!insertSession.Ok() || !insertState.Ok() || !insertText.Ok())
            {
                return fail();
            }
            const std::unordered_set<std::wstring> storedBefore = taken;
            for (const json::Value* session : sessions)
            {
                if (!session)
                {
                    continue;
                }
                std::wstring id;
                if (session->type == json::Value::Type::Obj)
                {
                    for (const auto& [key, value] : session->members)
                    {
                        if (key == L"id" && value.type == json::Value::Type::Str)
                        {
                            id = value.str;
                            break;
                        }
                    }
                }
                if (!id.empty() && storedBefore.count(id))
                {
                    continue; // the store has this session already: never overwritten by an import
                }
                // The row key: the id while it is non-empty and free; else a key of its own (an empty or repeated id in the
                // source still moves over — the restore skips such a record, but the store keeps it).
                std::wstring key = id;
                if (key.empty() || taken.count(key))
                {
                    key = id + kFleetKeySeparator + L"#" + std::to_wstring(nextOrd);
                }
                taken.insert(key);
                const int64_t ord = nextOrd++;
                FleetSplit split;
                if (session->type == json::Value::Type::Obj)
                {
                    split = FleetSplitRows(*session);
                }
                else
                {
                    split.doc = FleetRowText(*session); // a non-object element of the source array, kept whole
                }
                insertSession.Text(1, key);
                insertSession.Int(2, ord);
                insertSession.Int(3, split.hasQueue ? 1 : 0);
                insertSession.Text(4, split.doc);
                if (!insertSession.Run())
                {
                    return fail();
                }
                for (const auto& p : split.prompts)
                {
                    insertState.Text(1, key);
                    insertState.Text(2, p.key);
                    insertState.Int(3, (p.pos + 1) * kFleetOrdGap); // spaced, so later moves find room (kFleetOrdGap)
                    insertState.Text(4, p.state);
                    insertText.Text(1, key);
                    insertText.Text(2, p.key);
                    if (p.hasText)
                    {
                        insertText.Text(3, p.text);
                    }
                    else
                    {
                        insertText.Null(3);
                    }
                    if (!insertState.Run() || !insertText.Run())
                    {
                        return fail();
                    }
                }
                imported.push_back(id);
            }
            for (const auto& [key, value] : meta)
            {
                if (!FleetSetMetaRow(_db, key, value))
                {
                    return fail();
                }
            }
            if (!FleetExec(_db, "COMMIT", err))
            {
                return fail();
            }
            inTransaction = false;
            _nextOrd = (std::max)(_nextOrd, nextOrd);
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"FleetDb::ImportRaw"); // Rule #18. Recovery: rolled back; nothing imported.
            if (inTransaction)
            {
                std::wstring ignored;
                FleetExec(_db, "ROLLBACK", ignored);
            }
            imported.clear();
            problem = L"an exception (logged)";
            return false;
        }
    }

    bool FleetDb::ReadRaw(std::vector<std::pair<std::wstring, json::Value>>& out, std::wstring& problem) const noexcept
    {
        out.clear();
        problem.clear();
        try
        {
            std::lock_guard lk{ _mtx };
            if (!_db)
            {
                problem = L"the connection is closed";
                return false;
            }
            std::vector<FleetStored> rows;
            std::wstring error;
            if (!FleetReadStored(_db, rows, error))
            {
                problem = L"a read failed: " + error;
                return false;
            }
            out.reserve(rows.size());
            for (const auto& r : rows)
            {
                json::Value obj;
                if (!FleetJoinRows(r, obj))
                {
                    problem = L"the stored rows of " + r.key + L" do not parse";
                    out.clear();
                    return false;
                }
                out.emplace_back(r.key, std::move(obj));
            }
            return true;
        }
        catch (...)
        {
            problem = L"the read failed (an exception)";
            out.clear();
            return false;
        }
    }

    std::vector<std::wstring> FleetDb::StoredIds() const noexcept
    {
        std::vector<std::wstring> ids;
        try
        {
            std::lock_guard lk{ _mtx };
            if (!_db)
            {
                return ids;
            }
            FleetStmt s(_db, "SELECT id FROM sessions");
            if (!s.Ok())
            {
                return ids;
            }
            for (int rc = s.Step(); rc == SQLITE_ROW; rc = s.Step())
            {
                ids.push_back(s.ColText(0));
            }
        }
        catch (...)
        {
            ids.clear(); // std::bad_alloc: unknown reads as none
        }
        return ids;
    }

    bool FleetDb::FlushWal() noexcept
    {
        std::wstring wal;
        try
        {
            wal = _path + L"-wal";
        }
        catch (...)
        {
            return false;
        }
        if (!FleetFileExists(wal))
        {
            return true; // no WAL: every commit is in the database file
        }
        return FleetFlushFile(wal);
    }

    bool FleetDb::Checkpoint(bool truncate) noexcept
    {
        std::lock_guard lk{ _mtx };
        if (!_db || _readOnly)
        {
            return false;
        }
        int logFrames = 0;
        int checkpointed = 0;
        return sqlite3_wal_checkpoint_v2(_db, nullptr, truncate ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE, &logFrames, &checkpointed) == SQLITE_OK;
    }

    uint64_t FleetDb::WalBytes() const noexcept
    {
        try
        {
            return FleetFileSize(_path + L"-wal");
        }
        catch (...)
        {
            return 0;
        }
    }
}

// ===== the process-wide store ======================================================================================================

namespace
{
    // What the run opened. The mode and the connection are set once, by OpenFleetForStartupIn (under the engine's restore
    // barrier), and never change afterwards except through ResetFleetStoreForTests.
    struct FleetStoreState
    {
        std::mutex mtx;
        FleetMode mode{ FleetMode::None };
        std::unique_ptr<FleetDb> db;
        std::wstring profileDir;
        std::mutex hookMtx;
        std::function<void(int)> stepHook; // the migration's test hook
    };

    FleetStoreState& FleetStore() noexcept
    {
        // Leaked on purpose, like the engine: the app ends with TerminateProcess, and no destructor may race a late write.
        static FleetStoreState* s = new FleetStoreState();
        return *s;
    }

    // Calls the migration's test hook at a step boundary (tests only; no hook in the app).
    void FleetMigrationStep(int step) noexcept
    {
        std::function<void(int)> hook;
        {
            std::lock_guard lk{ FleetStore().hookMtx };
            hook = FleetStore().stepHook;
        }
        if (hook)
        {
            try
            {
                hook(step);
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: a test hook's own failure; the app installs none.
            }
        }
    }

    // Deletes the leftovers of an interrupted migration (never authoritative: the rename that makes the temp database the
    // store is the commit point). Never throws.
    void FleetDeleteLeftovers(const std::wstring& tempPath) noexcept
    {
        for (const wchar_t* suffix : { L"", L"-wal", L"-shm", L"-journal", L".lock" })
        {
            try
            {
                ::DeleteFileW((tempPath + suffix).c_str());
            }
            catch (...)
            {
                // std::bad_alloc building the name: that leftover stays (harmless; the next start tries again).
            }
        }
    }

    // A new, EMPTY store for a fresh profile, made the way the migration makes one: built in the temp file, checkpointed and
    // closed, then renamed into place (atomic, write-through). A crash while it is created therefore leaves only a temp file
    // the next start deletes — never a sessions.db without its schema, which every later start would read as unusable.
    // Returns true when sessions.db now exists; `problem` says why not. Never throws.
    bool FleetCreateEmptyStore(const std::wstring& profileDir, std::wstring& problem) noexcept
    {
        try
        {
            const std::wstring dbPath = profileDir + L"\\" + kFleetDbFile;
            const std::wstring tempPath = dbPath + L".migrating";
            FleetDeleteLeftovers(tempPath);
            {
                auto temp = FleetDb::Open(tempPath, FleetDbAccess::ReadWrite, true, problem);
                if (!temp)
                {
                    FleetDeleteLeftovers(tempPath);
                    return false;
                }
                if (!temp->Checkpoint(true))
                {
                    problem = L"the new database's checkpoint failed";
                    temp.reset();
                    FleetDeleteLeftovers(tempPath);
                    return false;
                }
            } // closed: the schema is in the file itself, and the lock file is gone
            for (const wchar_t* suffix : { L"-wal", L"-shm", L".lock" })
            {
                ::DeleteFileW((tempPath + suffix).c_str());
            }
            if (FleetFileExists(dbPath) || !::MoveFileExW(tempPath.c_str(), dbPath.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                problem = L"the new database could not take its name (Windows error " + std::to_wstring(::GetLastError()) + L")";
                FleetDeleteLeftovers(tempPath);
                return false;
            }
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"FleetCreateEmptyStore"); // Rule #18. Recovery: no store this start (the caller runs on sessions.json).
            problem = L"an exception (logged)";
            return false;
        }
    }

    // A session object's prompt count (its first "queue" array's size).
    size_t FleetPromptCount(const json::Value& session) noexcept
    {
        if (session.type != json::Value::Type::Obj)
        {
            return 0;
        }
        for (const auto& [key, value] : session.members)
        {
            if (key == L"queue" && value.type == json::Value::Type::Arr)
            {
                return value.arr.size();
            }
        }
        return 0;
    }

    // The canonical fingerprint of a list of session objects: SHA-256 (hex) of their canonical texts, one per line.
    // Throws std::bad_alloc only.
    std::wstring FleetCanonicalSha(const std::vector<const json::Value*>& sessions)
    {
        std::string utf8;
        for (const json::Value* s : sessions)
        {
            utf8 += FleetUtf8(CanonicalJsonText(*s));
            utf8 += '\n';
        }
        return FleetUtf16(Sha256Hex(utf8));
    }

    // The bytes of sessions.json as UTF-16 text, a UTF-8 BOM dropped (ReadStateFileUtf8's decoding).
    std::wstring FleetJsonText(const std::string& bytes)
    {
        std::string_view view{ bytes };
        if (view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF && static_cast<unsigned char>(view[1]) == 0xBB &&
            static_cast<unsigned char>(view[2]) == 0xBF)
        {
            view.remove_prefix(3);
        }
        return FleetUtf16(view);
    }

    // The session objects of a parsed sessions.json (the "sessions" array's elements), or false when it holds none.
    bool FleetSessionObjects(const json::Value& root, std::vector<const json::Value*>& out) noexcept
    {
        out.clear();
        if (root.type != json::Value::Type::Obj)
        {
            return false;
        }
        for (const auto& [key, value] : root.members)
        {
            if (key == L"sessions" && value.type == json::Value::Type::Arr)
            {
                try
                {
                    out.reserve(value.arr.size());
                    for (const auto& s : value.arr)
                    {
                        out.push_back(&s);
                    }
                }
                catch (...)
                {
                    out.clear();
                    return false;
                }
                return true;
            }
        }
        return false;
    }

    // Step 2 of the migration: copies sessions.json byte for byte into a new folder under backups\ with a README, flushes
    // the copy, and proves it by its SHA-256. Returns the folder ("" on failure, with `problem`). Never throws.
    std::wstring FleetBackUpJson(const std::wstring& profileDir, const std::string& bytes, const std::wstring& sha,
                                    size_t sessionCount, size_t promptCount, std::wstring& problem) noexcept
    {
        try
        {
            const std::wstring root = profileDir + L"\\" + StateBackups::kBackupsFolder;
            ::CreateDirectoryW(root.c_str(), nullptr); // may exist
            const std::wstring stamp = FleetLocalStamp();
            std::wstring dir;
            for (int n = 1; n <= 99 && dir.empty(); ++n)
            {
                const std::wstring candidate = root + L"\\" + stamp + L" sessions.json to SQLite" + (n > 1 ? (L"-" + std::to_wstring(n)) : std::wstring{});
                if (::CreateDirectoryW(candidate.c_str(), nullptr))
                {
                    dir = candidate;
                }
                else if (::GetLastError() != ERROR_ALREADY_EXISTS)
                {
                    break;
                }
            }
            if (dir.empty())
            {
                problem = L"the backup folder could not be created under " + root + L" (Windows error " + std::to_wstring(::GetLastError()) + L")";
                return {};
            }
            const std::wstring copy = dir + L"\\sessions.json";
            // The bytes the migration read are written, not a second copy of the file: the backup is exactly what was migrated.
            const HANDLE h = ::CreateFileW(copy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                problem = L"the backup copy could not be created (Windows error " + std::to_wstring(::GetLastError()) + L")";
                return {};
            }
            bool written = true;
            size_t done = 0;
            while (written && done < bytes.size())
            {
                DWORD wrote = 0;
                const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - done, size_t{ 1 } << 24));
                written = ::WriteFile(h, bytes.data() + done, chunk, &wrote, nullptr) && wrote == chunk;
                done += wrote;
            }
            written = written && ::FlushFileBuffers(h);
            ::CloseHandle(h);
            std::string back;
            if (!written || !FleetReadAllBytes(copy, back) || FleetUtf16(Sha256Hex(back)) != sha)
            {
                problem = L"the backup copy could not be written and verified (" + copy + L")";
                return {};
            }
            const std::wstring readme =
                std::wstring{ kFleetMigrationBackupMarker } + L"\r\n\r\n" + L"At " + stamp + L" Agentmaster (" + FleetWriterWord() +
                L") moved its saved sessions from sessions.json into the database sessions.db, in the same folder:\r\n" + profileDir +
                L"\r\n\r\nThis folder keeps sessions.json exactly as it was migrated: " + std::to_wstring(bytes.size()) + L" bytes, SHA-256 " + sha + L", " +
                std::to_wstring(sessionCount) + L" session(s), " + std::to_wstring(promptCount) +
                L" queued prompt(s). It is never deleted by Agentmaster.\r\n\r\n"
                L"To go back to it (with an Agentmaster that still reads sessions.json): close Agentmaster, move sessions.db, sessions.db-wal and\r\n"
                L"sessions.db-shm out of the profile folder, copy this sessions.json back into it, and start that Agentmaster.\r\n"
                L"An Agentmaster that uses sessions.db migrates such a file again at its next start.\r\n";
            const std::string readmeBytes = FleetUtf8(readme);
            const HANDLE r = ::CreateFileW((dir + L"\\README.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (r != INVALID_HANDLE_VALUE)
            {
                DWORD wrote = 0;
                ::WriteFile(r, readmeBytes.data(), static_cast<DWORD>(readmeBytes.size()), &wrote, nullptr);
                ::FlushFileBuffers(r);
                ::CloseHandle(r);
            }
            return dir;
        }
        catch (...)
        {
            LogSwallowedException(L"FleetBackUpJson"); // Rule #18. Recovery: no backup → no migration this start.
            problem = L"the backup failed (an exception, logged)";
            return {};
        }
    }

    // The migration of a usable sessions.json into a new sessions.db (SESSIONS_STORE.md §4.9, steps 2-7; step 1, the
    // decision, is the caller's). On success sessions.db holds every session and sessions.json is gone; on any failure
    // sessions.json is untouched and no sessions.db exists. Returns true on success; `problem` says why not. Never throws.
    bool FleetMigrateJson(const std::wstring& profileDir, std::wstring& problem) noexcept
    {
        problem.clear();
        try
        {
            const std::wstring jsonPath = profileDir + L"\\" + kFleetJsonFile;
            const std::wstring dbPath = profileDir + L"\\" + kFleetDbFile;
            const std::wstring tempPath = dbPath + L".migrating";
            FleetMigrationStep(1);

            // The source: the file's bytes, read once — the backup, the hash and the parse all come from them.
            std::string bytes;
            if (!FleetReadAllBytes(jsonPath, bytes))
            {
                problem = L"sessions.json could not be read for the migration";
                return false;
            }
            const std::wstring sha = FleetUtf16(Sha256Hex(bytes));
            const auto parsed = json::Parse(FleetJsonText(bytes));
            std::vector<const json::Value*> sessions;
            if (!parsed || !FleetSessionObjects(*parsed, sessions))
            {
                problem = L"sessions.json does not hold a \"sessions\" list";
                return false;
            }
            size_t prompts = 0;
            for (const json::Value* s : sessions)
            {
                prompts += FleetPromptCount(*s);
            }

            // 2. The verified, permanent backup.
            std::wstring backupProblem;
            const std::wstring backup = FleetBackUpJson(profileDir, bytes, sha, sessions.size(), prompts, backupProblem);
            if (backup.empty())
            {
                problem = L"no verified backup: " + backupProblem;
                return false;
            }
            FleetMigrationStep(2);
            const std::wstring canonicalSha = FleetCanonicalSha(sessions);
            FleetMigrationStep(3);

            // 4. Build the temp database in ONE transaction (synchronous=FULL), the migration record with it.
            FleetDeleteLeftovers(tempPath);
            std::wstring openProblem;
            auto temp = FleetDb::Open(tempPath, FleetDbAccess::ReadWrite, true, openProblem);
            if (!temp)
            {
                problem = L"the new database could not be created: " + openProblem;
                FleetDeleteLeftovers(tempPath);
                return false;
            }
            json::Value record = json::Value::MkObj();
            record.Set(L"from", json::Value::MkStr(kFleetJsonFile));
            record.Set(L"bytes", json::Value::MkNum(static_cast<double>(bytes.size())));
            record.Set(L"sha256", json::Value::MkStr(sha));
            record.Set(L"canonicalSha256", json::Value::MkStr(canonicalSha));
            record.Set(L"sessions", json::Value::MkNum(static_cast<double>(sessions.size())));
            record.Set(L"prompts", json::Value::MkNum(static_cast<double>(prompts)));
            record.Set(L"at", json::Value::MkNum(static_cast<double>(FleetWallUnixMs())));
            record.Set(L"writer", json::Value::MkStr(FleetWriterWord()));
            record.Set(L"backup", json::Value::MkStr(backup));
            std::vector<std::wstring> imported;
            std::wstring importProblem;
            if (!temp->ImportRaw(sessions, { { L"migration", FleetRowText(record) }, { L"writer", FleetWriterWord() } }, imported, importProblem) ||
                imported.size() != sessions.size())
            {
                problem = L"the import failed: " + (importProblem.empty() ? std::wstring{ L"not every session was imported" } : importProblem);
                temp.reset();
                FleetDeleteLeftovers(tempPath);
                return false;
            }
            // Fold the WAL into the file and close the writer: the verification reads the file ALONE.
            if (!temp->Checkpoint(true))
            {
                problem = L"the new database's checkpoint failed";
                temp.reset();
                FleetDeleteLeftovers(tempPath);
                return false;
            }
            temp.reset();
            FleetMigrationStep(4);

            // 5. Verify through a second connection that reads the file alone (immutable): the full integrity check, the
            //    counts, and every session's and prompt's value against the source's.
            std::wstring verifyProblem;
            {
                auto check = FleetDb::Open(tempPath, FleetDbAccess::Snapshot, false, verifyProblem);
                std::vector<std::pair<std::wstring, json::Value>> back;
                std::wstring integrity;
                if (!check)
                {
                    verifyProblem = L"the new database could not be reopened: " + verifyProblem;
                }
                else if (!check->Check(true, integrity))
                {
                    verifyProblem = L"its integrity check answered: " + integrity;
                }
                else if (!check->ReadRaw(back, verifyProblem))
                {
                    verifyProblem = L"it could not be read back: " + verifyProblem;
                }
                else if (back.size() != sessions.size())
                {
                    verifyProblem = L"it holds " + std::to_wstring(back.size()) + L" session(s), the source " + std::to_wstring(sessions.size());
                }
                else
                {
                    size_t backPrompts = 0;
                    std::vector<const json::Value*> backObjects;
                    backObjects.reserve(back.size());
                    for (size_t i = 0; i < back.size() && verifyProblem.empty(); ++i)
                    {
                        backPrompts += FleetPromptCount(back[i].second);
                        backObjects.push_back(&back[i].second);
                        // Compared as UTF-8, the form both stores keep text in: a lone surrogate (only possible from a
                        // "\ud800"-style escape someone wrote into the file by hand — this app's writer never escapes one)
                        // is U+FFFD on both sides, as sessions.json's own next save would have made it.
                        if (FleetUtf8(CanonicalJsonText(back[i].second)) != FleetUtf8(CanonicalJsonText(*sessions[i])))
                        {
                            verifyProblem = L"session #" + std::to_wstring(i + 1) + L" (" + back[i].first + L") differs from the source";
                        }
                    }
                    if (verifyProblem.empty() && backPrompts != prompts)
                    {
                        verifyProblem = L"it holds " + std::to_wstring(backPrompts) + L" prompt(s), the source " + std::to_wstring(prompts);
                    }
                    if (verifyProblem.empty() && FleetCanonicalSha(backObjects) != canonicalSha)
                    {
                        verifyProblem = L"its canonical fingerprint differs from the source's";
                    }
                }
            }
            if (!verifyProblem.empty())
            {
                problem = L"the verification failed: " + verifyProblem;
                FleetDeleteLeftovers(tempPath);
                return false;
            }
            FleetMigrationStep(5);

            // 6. The commit point: the verified file becomes the store (an atomic, write-through rename; no -wal is left).
            for (const wchar_t* suffix : { L"-wal", L"-shm", L".lock" })
            {
                ::DeleteFileW((tempPath + suffix).c_str());
            }
            if (FleetFileExists(dbPath) || !::MoveFileExW(tempPath.c_str(), dbPath.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                problem = L"the new database could not take its name (Windows error " + std::to_wstring(::GetLastError()) + L")";
                FleetDeleteLeftovers(tempPath);
                return false;
            }
            FleetMigrationStep(6);

            // 7. Only now does sessions.json go, with its §17 shadow (the older-writer repair does not apply to the store).
            ForgetStateDocument(jsonPath);
            const bool deleted = ::DeleteFileW(jsonPath.c_str()) != FALSE;
            ::DeleteFileW((profileDir + L"\\" + StateBackups::kShadowFolder + L"\\" + kFleetJsonFile).c_str());
            FleetMigrationStep(7);
            FleetLog(L"[fleet-migrate] sessions.json -> sessions.db: " + std::to_wstring(sessions.size()) + L" session(s), " + std::to_wstring(prompts) +
                L" prompt(s), verified (integrity, counts, every value); " +
                (deleted ? std::wstring{ L"sessions.json removed" } : std::wstring{ L"sessions.json could NOT be removed (it is finished at the next start)" }) +
                L"; the backup is " + backup);
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"FleetMigrateJson"); // Rule #18. Recovery: sessions.json untouched; no store this start.
            problem = L"an exception (logged)";
            try
            {
                FleetDeleteLeftovers(profileDir + L"\\" + kFleetDbFile + L".migrating");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the leftovers' names could not be built; the next start deletes them.
            }
            return false;
        }
    }

    // sessions.json beside an open store (an existing database): the migration's last step was interrupted (the bytes are
    // the ones migrated → delete it), or it was written after the move — by an older Agentmaster, or by a run of this one that
    // could not use the database (FleetFallBackToJson) — so the sessions the store does not know are imported and the file
    // is kept aside. Never throws; a failure leaves sessions.json where it is for the next start.
    void FleetReconcileJson(FleetDb& db, const std::wstring& profileDir) noexcept
    {
        try
        {
            const std::wstring jsonPath = profileDir + L"\\" + kFleetJsonFile;
            if (!FleetFileExists(jsonPath) || db.ReadOnly())
            {
                return;
            }
            std::string bytes;
            if (!FleetReadAllBytes(jsonPath, bytes))
            {
                FleetLog(L"[fleet] sessions.json is beside sessions.db but could not be read: it is left where it is");
                return;
            }
            const std::wstring sha = FleetUtf16(Sha256Hex(bytes));
            std::wstring migratedSha;
            if (const auto record = json::Parse(db.Meta(L"migration")); record && record->type == json::Value::Type::Obj)
            {
                migratedSha = record->StrAt(L"sha256");
            }
            if (!migratedSha.empty() && sha == migratedSha)
            {
                ForgetStateDocument(jsonPath);
                ::DeleteFileW(jsonPath.c_str());
                ::DeleteFileW((profileDir + L"\\" + StateBackups::kShadowFolder + L"\\" + kFleetJsonFile).c_str());
                FleetLog(L"[fleet-migrate] finished an interrupted migration: sessions.json (the bytes it migrated) removed");
                return;
            }
            // A file written after the move: import only what the store does not have, never overwrite a stored session.
            const auto parsed = json::Parse(FleetJsonText(bytes));
            std::vector<const json::Value*> sessions;
            size_t importedCount = 0;
            size_t known = 0;
            if (parsed && FleetSessionObjects(*parsed, sessions))
            {
                std::vector<std::wstring> imported;
                std::wstring problem;
                if (!db.ImportRaw(sessions, {}, imported, problem))
                {
                    FleetLog(L"[fleet-migrate] sessions.json written after the move could not be imported (" + problem + L"): it is left where it is");
                    return;
                }
                // ImportRaw lists every source element it imported (a non-object one too), so the rest are sessions the
                // store already holds.
                importedCount = imported.size();
                known = sessions.size() - (std::min)(sessions.size(), importedCount);
            }
            const std::wstring aside = jsonPath + L".after-sqlite-" + FleetLocalStamp();
            const bool moved = ::MoveFileExW(jsonPath.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
            ForgetStateDocument(jsonPath);
            FleetLog(L"[fleet-migrate] sessions.json was written after the move (by an older Agentmaster, or by a run that could not use sessions.db): " +
                std::to_wstring(importedCount) +
                L" new session(s) imported, " + std::to_wstring(known) + L" known one(s) left as the store has them" +
                (parsed ? std::wstring{} : std::wstring{ L" (the file did not parse)" }) + L"; the file is " +
                (moved ? (L"kept as " + aside) : std::wstring{ L"left in place (it could not be renamed)" }));
        }
        catch (...)
        {
            LogSwallowedException(L"FleetReconcileJson"); // Rule #18. Recovery: sessions.json stays; next start retries.
        }
    }

    // sessions.db exists but cannot be used (SESSIONS_STORE.md §4.6). It is never written over — and nothing of this run may
    // be lost either — so the run saves sessions.json beside it through the legacy writer (with its own guards, §15), and the
    // next start that can use the database imports what that file holds and the database does not (FleetReconcileJson). The
    // fleet is what such an earlier run saved there, or none. `usable` stays false, so the caller copies the workspace (the
    // database and its WAL included) into recovery\ and every window holds the refs of the sessions it does not know (§16).
    // Called under the store's lock. Throws std::bad_alloc only.
    FleetLoad FleetFallBackToJson(FleetStoreState& st, const std::wstring& profileDir, const std::wstring& why)
    {
        st.mode = FleetMode::Json;
        FleetLoad out = ReadSessionsJsonForStartupIn(profileDir);
        const size_t fromJson = out.usable ? out.sessions.size() : 0;
        out.usable = false;
        out.fileStatus = StateFileStatus::Unreadable;
        out.problem = L"sessions.db could not be used (" + why + L")";
        FleetLog(L"[fleet] sessions.db NOT used: " + why + L"; nothing is written over it - this run saves sessions.json beside it (" +
                 std::to_wstring(fromJson) + L" session(s) loaded from an earlier such run), and the next start that can use the database imports it");
        return out;
    }

    // Starts the store's quick check (PRAGMA quick_check: every page of the file, ~0.15-0.4 s at production's 19 MB) on a
    // read-only connection of its own, on another thread. The answer is "" when SQLite says ok, else what was wrong. When no
    // thread can be started, the check runs here, before this returns (a ready future). Never throws but std::bad_alloc.
    std::future<std::wstring> FleetStartQuickCheck(const std::wstring& dbPath)
    {
        auto run = [dbPath]() noexcept -> std::wstring {
            try
            {
                std::wstring problem;
                auto reader = FleetDb::Open(dbPath, FleetDbAccess::ReadOnly, false, problem);
                if (!reader)
                {
                    return L"the check's own connection could not open it (" + problem + L")";
                }
                std::wstring answer;
                return reader->Check(false, answer) ? std::wstring{} : (answer.empty() ? std::wstring{ L"the check failed" } : answer);
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the answer itself could not be built (std::bad_alloc); a check that cannot answer
                // fails, so the store is not used this run (never a silent pass).
                return L"?";
            }
        };
        try
        {
            return std::async(std::launch::async, run);
        }
        catch (const std::system_error&)
        {
            std::promise<std::wstring> answered; // no thread: check here (the start pays the sum, as it would have anyway)
            answered.set_value(run());
            return answered.get_future();
        }
    }

    // Opens an existing sessions.db for the run and loads it (the store's mode set). The quick check runs beside the load
    // (FleetStartQuickCheck), so a start pays the longer of the two, not their sum; a store that fails it is not used — the
    // load is thrown away. The import of a sessions.json found beside the store WRITES, so it waits for the check's ok.
    // Called under the store's lock. Throws std::bad_alloc only.
    FleetLoad FleetOpenExisting(FleetStoreState& st, const std::wstring& profileDir)
    {
        FleetLoad out;
        const std::wstring dbPath = profileDir + L"\\" + kFleetDbFile;
        std::wstring problem;
        auto db = FleetDb::Open(dbPath, FleetDbAccess::ReadWrite, false, problem);
        if (!db)
        {
            return FleetFallBackToJson(st, profileDir, problem);
        }
        auto check = FleetStartQuickCheck(dbPath);
        const auto failedCheck = [&](std::wstring answer) {
            db.reset(); // closed (and its lock released) before the fallback: nothing of this run touches it again
            return FleetFallBackToJson(st, profileDir, L"its quick check answered: " + answer);
        };
        const bool jsonBeside = FleetFileExists(profileDir + L"\\" + kFleetJsonFile);
        if (jsonBeside)
        {
            if (std::wstring answer = check.get(); !answer.empty())
            {
                return failedCheck(std::move(answer));
            }
            FleetReconcileJson(*db, profileDir);
        }
        std::vector<SessionInfo> sessions;
        std::wstring loadProblem;
        const bool loaded = db->LoadAll(sessions, loadProblem);
        if (!jsonBeside)
        {
            if (std::wstring answer = check.get(); !answer.empty())
            {
                return failedCheck(std::move(answer)); // whatever the load read is not used
            }
        }
        if (!loaded)
        {
            db.reset(); // closed (and its lock released) before the fallback: nothing of this run touches it again
            return FleetFallBackToJson(st, profileDir, loadProblem);
        }
        if (!loadProblem.empty())
        {
            FleetLog(L"[fleet] " + loadProblem);
        }
        st.mode = db->ReadOnly() ? FleetMode::SqliteReadOnly : FleetMode::Sqlite;
        if (db->ReadOnly())
        {
            FleetLog(L"[fleet] sessions.db is READ-ONLY this run (" + db->ReadOnlyReason() + L"): changes to sessions are not saved");
        }
        FleetLog(L"[fleet] sessions.db open: " + std::to_wstring(sessions.size()) + L" session(s), WAL " + std::to_wstring(db->WalBytes() / 1024) + L" KB");
        st.db = std::move(db);
        out.sessions = std::move(sessions);
        out.usable = true;
        out.fileStatus = StateFileStatus::Read;
        return out;
    }
}

namespace Agentmaster
{
    FleetLoad OpenFleetForStartupIn(const std::wstring& profileDir)
    {
        auto& st = FleetStore();
        std::lock_guard lk{ st.mtx };
        st.profileDir = profileDir;
        const std::wstring dbPath = profileDir + L"\\" + kFleetDbFile;
        FleetDeleteLeftovers(dbPath + L".migrating"); // an interrupted migration's temp files are never the store
        if (st.db)
        {
            // A second open in one process (never in the app: the restore barrier opens once): read again from the store.
            FleetLoad out;
            std::wstring problem;
            out.usable = st.db->LoadAll(out.sessions, problem);
            out.fileStatus = out.usable ? StateFileStatus::Read : StateFileStatus::Unreadable;
            out.problem = problem;
            return out;
        }
        if (FleetFileExists(dbPath))
        {
            return FleetOpenExisting(st, profileDir);
        }
        // No store yet: sessions.json decides.
        FleetLoad fromJson = ReadSessionsJsonForStartupIn(profileDir);
        if (fromJson.fileStatus == StateFileStatus::Missing)
        {
            // A fresh profile: an empty store (FleetCreateEmptyStore: built aside and renamed into place).
            std::wstring problem;
            if (!FleetCreateEmptyStore(profileDir, problem))
            {
                st.mode = FleetMode::Json; // the legacy file, as before: nothing is lost (there is nothing yet)
                FleetLog(L"[fleet] sessions.db could not be created (" + problem + L"): this run saves sessions.json");
                return fromJson;
            }
            FleetLog(L"[fleet] a new, empty sessions.db");
            FleetLoad out = FleetOpenExisting(st, profileDir);
            if (out.usable)
            {
                out.fileStatus = StateFileStatus::Missing; // as before the store: nothing had been saved in this profile
            }
            return out;
        }
        if (!fromJson.usable)
        {
            // sessions.json exists but cannot be used: no migration (it would move nothing, or the wrong thing). The run
            // goes on as before — the legacy writer's guard keeps the file's bytes — and the next start tries again.
            st.mode = FleetMode::Json;
            FleetLog(L"[fleet-migrate] NOT migrated: sessions.json could not be used (" + fromJson.problem + L"); this run saves sessions.json");
            return fromJson;
        }
        std::wstring problem;
        if (!FleetMigrateJson(profileDir, problem))
        {
            st.mode = FleetMode::Json;
            FleetLog(L"[fleet-migrate] FAILED: " + problem + L"; sessions.json is untouched and this run saves it as before (the next start tries again)");
            return fromJson;
        }
        return FleetOpenExisting(st, profileDir);
    }

    FleetMode CurrentFleetMode() noexcept
    {
        std::lock_guard lk{ FleetStore().mtx };
        return FleetStore().mode;
    }

    void GetFleetStore(FleetMode& mode, FleetDb*& db) noexcept
    {
        std::lock_guard lk{ FleetStore().mtx };
        mode = FleetStore().mode;
        db = FleetStore().db.get();
    }

    std::wstring FleetStoreProfileDir()
    {
        std::lock_guard lk{ FleetStore().mtx };
        return FleetStore().profileDir;
    }

    std::vector<SessionInfo> LoadFleetReadOnlyIn(const std::wstring& profileDir)
    {
        const std::wstring dbPath = profileDir + L"\\" + kFleetDbFile;
        if (FleetFileExists(dbPath))
        {
            std::wstring problem;
            auto db = FleetDb::Open(dbPath, FleetDbAccess::ReadOnly, false, problem);
            std::vector<SessionInfo> sessions;
            if (db && db->LoadAll(sessions, problem))
            {
                return sessions; // the store wins over any sessions.json beside it (that file is imported at the next start)
            }
            // A store that cannot be read: the app's run then saves sessions.json beside it (FleetFallBackToJson), which is
            // the best view there is — or nothing, when there is no such file.
        }
        return DeserializeSessions(ReadStateFileUtf8(profileDir + L"\\" + kFleetJsonFile).text);
    }

    void SetFleetMigrationStepHookForTests(std::function<void(int step)> hook) noexcept
    {
        std::lock_guard lk{ FleetStore().hookMtx };
        FleetStore().stepHook = std::move(hook);
    }

    void ResetFleetStoreForTests() noexcept
    {
        std::lock_guard lk{ FleetStore().mtx };
        FleetStore().db.reset();
        FleetStore().mode = FleetMode::None;
        FleetStore().profileDir.clear();
    }

    FleetDb* FleetDbForTests() noexcept
    {
        std::lock_guard lk{ FleetStore().mtx };
        return FleetStore().db.get();
    }
}
