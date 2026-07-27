// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionStore implementation (see SessionStore.h). Pure C++/Win32, no WinRT, no PCH.

#include "SessionStore.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir
#include "Json.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <string>

namespace Agentmaster
{
    namespace
    {
        // UTF-8 bytes -> wide (the store files are UTF-8 JSON). Empty on empty/failure.
        std::wstring Utf8ToWide(const char* data, size_t len)
        {
            if (len == 0)
            {
                return {};
            }
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
            if (n <= 0)
            {
                return {};
            }
            std::wstring w(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), w.data(), n);
            return w;
        }

        // Read a whole (small) store file, capped. Shared read/write/delete so a concurrent writer
        // is never blocked. Empty on failure / oversize.
        std::string ReadWhole(const std::wstring& path, size_t maxBytes)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            LARGE_INTEGER sz{};
            ::GetFileSizeEx(h, &sz);
            if (sz.QuadPart < 0 || static_cast<uint64_t>(sz.QuadPart) > maxBytes)
            {
                ::CloseHandle(h);
                return {};
            }
            std::string bytes(static_cast<size_t>(sz.QuadPart), '\0');
            size_t off = 0;
            while (off < bytes.size())
            {
                DWORD got = 0;
                if (!::ReadFile(h, bytes.data() + off, static_cast<DWORD>(bytes.size() - off), &got, nullptr) || got == 0)
                {
                    break;
                }
                off += got;
            }
            ::CloseHandle(h);
            bytes.resize(off);
            return bytes;
        }

        // Atomic UTF-8 write: temp file + replace, so a torn write never survives. The temp name is
        // made unique per writer (thread id) so two threads writing DIFFERENT sessions never collide
        // on the temp path; the final MoveFileEx replace is atomic (last writer wins for the SAME
        // session — rare, since a session's title is set from one place at a time).
        bool AtomicWriteUtf8(const std::wstring& path, const std::wstring& text)
        {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string bytes(static_cast<size_t>(n > 0 ? n : 0), '\0');
            if (n > 0)
            {
                ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
            }
            const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
            const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD wrote = 0;
            const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
            ::CloseHandle(h);
            if (!ok || wrote != bytes.size())
            {
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }

        // A sessionId is the file STEM, so it must be a safe single path component. GUIDs (and the
        // Codex rollout uuids / our minted handles) always pass; this only rejects a malformed id
        // that could escape the store dir.
        bool ValidSid(const std::wstring& sid)
        {
            if (sid.empty() || sid.size() > 256)
            {
                return false;
            }
            for (const wchar_t c : sid)
            {
                if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 0x20)
                {
                    return false;
                }
            }
            return true;
        }

        std::wstring FilePath(const std::wstring& storeDir, const std::wstring& sid)
        {
            return storeDir + L"\\" + sid + L".json";
        }

        // Parse <storeDir>\<sid>.json into a record. Only STRING members are kept (the store's value
        // type); a malformed / non-object file yields an empty record (treated as "no data").
        SessionStoreRecord ParseRecord(const std::wstring& storeDir, const std::wstring& sid)
        {
            SessionStoreRecord rec;
            const std::string bytes = ReadWhole(FilePath(storeDir, sid), 1u << 20);
            if (bytes.empty())
            {
                return rec;
            }
            const auto parsed = json::Parse(Utf8ToWide(bytes.data(), bytes.size()));
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                return rec;
            }
            for (const auto& [k, v] : parsed->members)
            {
                if (v.type == json::Value::Type::Str)
                {
                    rec[k] = v.str;
                }
            }
            return rec;
        }

        // Agentmaster (perf — the CPU-hotspot fix): an in-process, (size,mtime)-VALIDATED record cache.
        // The Manager board reads GetSessionTags PER CARD PER REBUILD (and titles/favorites read the same
        // files elsewhere) — each a CreateFile+ReadFile+JSON-parse of <session-store>\<sid>.json, most of
        // which don't even exist (the store is sparse by design). Dump-proven on the live 70-tab release
        // instance: GetSessionTags -> LoadSessionStoreIn -> ReadWhole sat ON the hot UI-thread stack.
        // The cache turns a steady-state read into ONE GetFileAttributesExW (fail-fast for the common
        // absent-file case) + a map hit; the file is re-parsed only when its (size,mtime) actually moved.
        // Cross-PROCESS edits (another instance / the CLI writing the store) are caught by the same stat
        // validation — no staleness class the old direct read didn't already have between polls. Same-
        // process writes are WRITE-THROUGH below (WriteRecord updates the entry from what it just wrote),
        // so a rapid write→read never depends on mtime granularity. Guarded by its own mutex — the store
        // is called from the UI lane, the engine observer, and tests concurrently.
        struct RecCacheEntry
        {
            SessionStoreRecord rec;
            uint64_t size = 0;
            int64_t mtime = 0; // FILETIME ticks
            bool present = false; // file existed at last look (an ABSENT file caches as an empty record)
        };
        std::mutex g_recCacheMtx;
        std::unordered_map<std::wstring, RecCacheEntry> g_recCache; // key: case-folded full path

        std::wstring RecCacheKey(const std::wstring& path)
        {
            std::wstring k = path;
            for (auto& c : k)
            {
                c = static_cast<wchar_t>(::towlower(c));
            }
            return k;
        }

        bool StatFile(const std::wstring& path, uint64_t& size, int64_t& mtime)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            {
                size = 0;
                mtime = 0;
                return false;
            }
            size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            ULARGE_INTEGER li{};
            li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            mtime = static_cast<int64_t>(li.QuadPart);
            return true;
        }

        // The cached read behind LoadSessionStoreIn. A hit whose (present,size,mtime) still matches is a
        // map copy; anything else re-parses and refreshes the entry (a transient stat miss mid-atomic-
        // replace simply caches "absent" for one round and self-heals on the next stat).
        SessionStoreRecord LoadRecordCached(const std::wstring& storeDir, const std::wstring& sid)
        {
            const std::wstring path = FilePath(storeDir, sid);
            uint64_t size = 0;
            int64_t mtime = 0;
            const bool present = StatFile(path, size, mtime);
            const std::wstring key = RecCacheKey(path);
            {
                std::lock_guard guard{ g_recCacheMtx };
                const auto it = g_recCache.find(key);
                if (it != g_recCache.end() && it->second.present == present && it->second.size == size && it->second.mtime == mtime)
                {
                    return it->second.rec;
                }
            }
            SessionStoreRecord rec = present ? ParseRecord(storeDir, sid) : SessionStoreRecord{};
            {
                std::lock_guard guard{ g_recCacheMtx };
                g_recCache[key] = RecCacheEntry{ rec, size, mtime, present };
            }
            return rec;
        }

        // Write a record (deleting the file when empty, so the store stays sparse). WRITE-THROUGH: the
        // cache entry is refreshed from what was just written, so a same-process write→read is coherent
        // even when two writes land inside one mtime tick (the stat-validation blind spot).
        bool WriteRecord(const std::wstring& storeDir, const std::wstring& sid, const SessionStoreRecord& rec)
        {
            const std::wstring path = FilePath(storeDir, sid);
            if (rec.empty())
            {
                ::DeleteFileW(path.c_str());
                std::lock_guard guard{ g_recCacheMtx };
                g_recCache[RecCacheKey(path)] = RecCacheEntry{}; // absent, empty record
                return true;
            }
            json::Value o = json::Value::MkObj();
            for (const auto& [k, v] : rec)
            {
                o.Set(k, json::Value::MkStr(v));
            }
            if (!AtomicWriteUtf8(path, json::Dump(o)))
            {
                return false;
            }
            uint64_t size = 0;
            int64_t mtime = 0;
            StatFile(path, size, mtime); // best-effort; a failed stat just means the next read re-parses
            std::lock_guard guard{ g_recCacheMtx };
            g_recCache[RecCacheKey(path)] = RecCacheEntry{ rec, size, mtime, true };
            return true;
        }

        std::wstring StoreDir()
        {
            return AgentmasterStateDir() + L"\\session-store";
        }
    }

    // ===== testable core =====================================================================

    SessionStoreRecord LoadSessionStoreIn(const std::wstring& storeDir, const std::wstring& sessionId)
    {
        if (storeDir.empty() || !ValidSid(sessionId))
        {
            return {};
        }
        // Cached ((size,mtime)-validated): the hot read behind GetSessionTags / titles / favorites —
        // called per card per board rebuild. See LoadRecordCached. (SetSessionStoreFieldIn's own
        // read-modify-write below deliberately stays on the direct ParseRecord — freshest-disk RMW.)
        return LoadRecordCached(storeDir, sessionId);
    }

    std::wstring GetSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& key)
    {
        const auto rec = LoadSessionStoreIn(storeDir, sessionId);
        const auto it = rec.find(key);
        return it == rec.end() ? std::wstring{} : it->second;
    }

    bool SetSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& key, const std::wstring& value)
    {
        if (storeDir.empty() || !ValidSid(sessionId) || key.empty())
        {
            return false;
        }
        SessionStoreRecord rec = ParseRecord(storeDir, sessionId);
        const auto it = rec.find(key);
        const bool present = it != rec.end();
        if (value.empty())
        {
            if (!present)
            {
                return true; // nothing to remove — no write
            }
            rec.erase(it);
        }
        else
        {
            if (present && it->second == value)
            {
                return true; // unchanged — dedup the write (steady-state re-publish is one read)
            }
            rec[key] = value;
        }
        ::CreateDirectoryW(storeDir.c_str(), nullptr); // idempotent; only when we actually write
        return WriteRecord(storeDir, sessionId, rec);
    }

    bool SetSessionStoreFieldsIn(const std::wstring& storeDir, const std::wstring& sessionId, const SessionStoreRecord& fields)
    {
        if (storeDir.empty() || !ValidSid(sessionId) || fields.empty())
        {
            return false;
        }
        SessionStoreRecord rec = ParseRecord(storeDir, sessionId); // freshest-disk RMW (as the single-field setter)
        bool changed = false;
        for (const auto& [key, value] : fields)
        {
            if (key.empty())
            {
                continue; // an unnamed field is not storable; the rest of the batch still applies
            }
            const auto it = rec.find(key);
            const bool present = it != rec.end();
            if (value.empty())
            {
                if (present)
                {
                    rec.erase(it);
                    changed = true;
                }
                continue; // absent already — nothing to remove
            }
            if (present && it->second == value)
            {
                continue; // unchanged
            }
            rec[key] = value;
            changed = true;
        }
        if (!changed)
        {
            return true; // every field already held its value — dedup the write entirely
        }
        ::CreateDirectoryW(storeDir.c_str(), nullptr); // idempotent; only when we actually write
        return WriteRecord(storeDir, sessionId, rec);
    }

    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& key)
    {
        std::unordered_map<std::wstring, std::wstring> out;
        if (storeDir.empty() || key.empty())
        {
            return out;
        }
        WIN32_FIND_DATAW fd{};
        const std::wstring glob = storeDir + L"\\*.json";
        const HANDLE h = ::FindFirstFileW(glob.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return out; // no store dir yet / empty
        }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            std::wstring name = fd.cFileName;
            const size_t dot = name.size() >= 5 ? name.size() - 5 : std::wstring::npos; // ".json"
            if (dot == std::wstring::npos || name.compare(dot, 5, L".json") != 0)
            {
                continue;
            }
            const std::wstring sid = name.substr(0, dot);
            if (!ValidSid(sid))
            {
                continue;
            }
            const auto rec = ParseRecord(storeDir, sid);
            const auto it = rec.find(key);
            if (it != rec.end() && !it->second.empty())
            {
                out[sid] = it->second;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return out;
    }

    // ===== live wrappers =====================================================================

    SessionStoreRecord LoadSessionStore(const std::wstring& sessionId)
    {
        return LoadSessionStoreIn(StoreDir(), sessionId);
    }
    std::wstring GetSessionStoreField(const std::wstring& sessionId, const std::wstring& key)
    {
        return GetSessionStoreFieldIn(StoreDir(), sessionId, key);
    }
    bool SetSessionStoreField(const std::wstring& sessionId, const std::wstring& key, const std::wstring& value)
    {
        return SetSessionStoreFieldIn(StoreDir(), sessionId, key, value);
    }
    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreField(const std::wstring& key)
    {
        return LoadAllSessionStoreFieldIn(StoreDir(), key);
    }

    // ===== typed convenience: the TITLE ======================================================

    std::wstring GetStoredSessionTitle(const std::wstring& sessionId)
    {
        return GetSessionStoreField(sessionId, kSessionStoreTitleKey);
    }
    bool SetStoredSessionTitle(const std::wstring& sessionId, const std::wstring& title)
    {
        return SetSessionStoreField(sessionId, kSessionStoreTitleKey, title);
    }
    std::unordered_map<std::wstring, std::wstring> LoadAllStoredSessionTitles()
    {
        return LoadAllSessionStoreField(kSessionStoreTitleKey);
    }

    // ===== typed convenience: the FAVORITE (FAVORITES.md) ====================================

    bool IsSessionFavorite(const std::wstring& sessionId)
    {
        return !GetSessionStoreField(sessionId, kSessionStoreFavoriteKey).empty();
    }
    bool SetSessionFavorite(const std::wstring& sessionId, bool favorite)
    {
        // "1" when on; "" removes the key (and the file, once empty) when off — keeps the store sparse.
        return SetSessionStoreField(sessionId, kSessionStoreFavoriteKey, favorite ? std::wstring{ L"1" } : std::wstring{});
    }
    std::unordered_set<std::wstring> LoadAllFavoriteSessions()
    {
        std::unordered_set<std::wstring> out;
        for (const auto& [sid, value] : LoadAllSessionStoreField(kSessionStoreFavoriteKey))
        {
            out.insert(sid);
        }
        return out;
    }

    // ===== typed convenience: the DRAFT (PENDING_INPUT.md §8c) ===============================

    std::wstring GetStoredSessionDraftIn(const std::wstring& storeDir, const std::wstring& sessionId)
    {
        return GetSessionStoreFieldIn(storeDir, sessionId, kSessionStoreDraftKey);
    }
    int64_t GetStoredSessionDraftAtIn(const std::wstring& storeDir, const std::wstring& sessionId)
    {
        const auto raw = GetSessionStoreFieldIn(storeDir, sessionId, kSessionStoreDraftAtKey);
        if (raw.empty())
        {
            return 0;
        }
        // Hand-mangled / non-numeric content reads as "no stamp" rather than throwing: the stamp is an
        // honesty hint (how old is this memory), never a correctness input.
        try
        {
            return std::stoll(raw);
        }
        catch (...)
        {
            LogSwallowedException(L"GetStoredSessionDraftAtIn");
            return 0;
        }
    }
    bool SetStoredSessionDraftIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& text, int64_t atUnixMs)
    {
        // ONE write for both keys, so the draft and its observation stamp can never disagree (and a
        // clear can never leave a stamp behind pointing at a draft that is gone).
        SessionStoreRecord fields;
        fields[kSessionStoreDraftKey] = text;
        fields[kSessionStoreDraftAtKey] = (text.empty() || atUnixMs <= 0) ? std::wstring{} : std::to_wstring(atUnixMs);
        return SetSessionStoreFieldsIn(storeDir, sessionId, fields);
    }

    std::wstring GetStoredSessionDraft(const std::wstring& sessionId)
    {
        return GetStoredSessionDraftIn(StoreDir(), sessionId);
    }
    int64_t GetStoredSessionDraftAt(const std::wstring& sessionId)
    {
        return GetStoredSessionDraftAtIn(StoreDir(), sessionId);
    }
    bool SetStoredSessionDraft(const std::wstring& sessionId, const std::wstring& text, int64_t atUnixMs)
    {
        return SetStoredSessionDraftIn(StoreDir(), sessionId, text, atUnixMs);
    }

    // ===== typed convenience: the TAGS (bookmark tags on a session's tab) ====================

    std::wstring NormalizeTagName(const std::wstring& raw)
    {
        std::wstring out;
        out.reserve(raw.size());
        for (const wchar_t c : raw)
        {
            if (c >= 0x20) // strip control chars (incl. the \n/\t that would break the tab-strip spec join)
            {
                out.push_back(c);
            }
        }
        const auto isSpace = [](wchar_t c) { return c == L' ' || c == 0x00A0; };
        size_t b = 0;
        size_t e = out.size();
        while (b < e && isSpace(out[b]))
        {
            ++b;
        }
        while (e > b && isSpace(out[e - 1]))
        {
            --e;
        }
        out = out.substr(b, e - b);
        if (out.size() > kMaxTagNameLength)
        {
            out.resize(kMaxTagNameLength);
            // A trim can re-expose trailing whitespace — drop it so the capped name stays clean.
            while (!out.empty() && isSpace(out.back()))
            {
                out.pop_back();
            }
        }
        return out;
    }

    std::wstring FoldTagName(const std::wstring& name)
    {
        std::wstring out;
        out.reserve(name.size());
        for (const wchar_t c : name)
        {
            out.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
        return out;
    }

    std::wstring EncodeTagList(const std::vector<std::wstring>& tags)
    {
        if (tags.empty())
        {
            return {}; // "" removes the key — the store stays sparse
        }
        json::Value a = json::Value::MkArr();
        for (const auto& t : tags)
        {
            a.Push(json::Value::MkStr(t));
        }
        return json::Dump(a);
    }

    std::vector<std::wstring> DecodeTagList(const std::wstring& value)
    {
        std::vector<std::wstring> out;
        if (value.empty())
        {
            return out;
        }
        const auto parsed = json::Parse(value);
        if (!parsed || parsed->type != json::Value::Type::Arr)
        {
            return out; // malformed / non-array -> "no tags" (never crashes the caller)
        }
        std::unordered_set<std::wstring> seen; // folded — CI dedupe, order-preserving
        for (const auto& e : parsed->arr)
        {
            if (e.type != json::Value::Type::Str)
            {
                continue;
            }
            const std::wstring name = NormalizeTagName(e.str);
            if (name.empty())
            {
                continue;
            }
            if (seen.insert(FoldTagName(name)).second)
            {
                out.push_back(name);
            }
        }
        return out;
    }

    std::vector<GlobalTagInfo> CollectGlobalTags(
        const std::unordered_map<std::wstring, std::vector<std::wstring>>& tagsBySession,
        const std::unordered_map<std::wstring, int64_t>& activityBySession)
    {
        return CollectGlobalTags(tagsBySession, activityBySession, {});
    }

    std::vector<GlobalTagInfo> CollectGlobalTags(
        const std::unordered_map<std::wstring, std::vector<std::wstring>>& tagsBySession,
        const std::unordered_map<std::wstring, int64_t>& activityBySession,
        const std::vector<std::wstring>& knownTags)
    {
        // folded name -> merged info (display casing from the highest-activity carrier).
        struct Merged
        {
            std::wstring display;
            int64_t displayActivity{ -1 }; // activity of the carrier whose casing `display` is
            int64_t maxActivity{ 0 };
            uint32_t count{ 0 };
        };
        std::unordered_map<std::wstring, Merged> merged;
        for (const auto& [sid, tags] : tagsBySession)
        {
            int64_t activity = 0;
            if (const auto it = activityBySession.find(sid); it != activityBySession.end())
            {
                activity = it->second;
            }
            for (const auto& rawTag : tags)
            {
                const std::wstring name = NormalizeTagName(rawTag);
                if (name.empty())
                {
                    continue;
                }
                auto& m = merged[FoldTagName(name)];
                ++m.count;
                m.maxActivity = (std::max)(m.maxActivity, activity); // parenthesized — windows.h's max macro
                // Display casing: the most-active carrier wins; on an exact activity tie the
                // lexicographically smaller casing wins, so the result is input-order independent.
                if (activity > m.displayActivity || (activity == m.displayActivity && (m.display.empty() || name < m.display)))
                {
                    m.display = name;
                    m.displayActivity = activity;
                }
            }
        }
        // The registry's 0-carrier tags: add each known name NO session carries — count 0, activity
        // 0 (sorts last), the registry's casing. A carried known tag keeps its carrier-derived entry.
        for (const auto& rawKnown : knownTags)
        {
            const std::wstring name = NormalizeTagName(rawKnown);
            if (name.empty())
            {
                continue;
            }
            const std::wstring folded = FoldTagName(name);
            if (merged.find(folded) == merged.end())
            {
                auto& m = merged[folded];
                m.display = name;
                m.displayActivity = 0;
            }
        }
        std::vector<std::pair<std::wstring, Merged>> rows(merged.begin(), merged.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.second.maxActivity != b.second.maxActivity)
            {
                return a.second.maxActivity > b.second.maxActivity; // most recently active first
            }
            return a.first < b.first; // folded-name ASC — deterministic tiebreak
        });
        std::vector<GlobalTagInfo> out;
        out.reserve(rows.size());
        for (auto& [folded, m] : rows)
        {
            out.push_back(GlobalTagInfo{ std::move(m.display), m.maxActivity, m.count });
        }
        return out;
    }

    std::vector<std::wstring> GetSessionTagsIn(const std::wstring& storeDir, const std::wstring& sessionId)
    {
        return DecodeTagList(GetSessionStoreFieldIn(storeDir, sessionId, kSessionStoreTagsKey));
    }

    bool SetSessionTagsIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::vector<std::wstring>& tags)
    {
        // Round through decode(encode(...)) semantics by normalizing + deduping here, so the
        // stored value is always canonical (what GetSessionTagsIn would return).
        std::vector<std::wstring> canon;
        std::unordered_set<std::wstring> seen;
        for (const auto& raw : tags)
        {
            const std::wstring name = NormalizeTagName(raw);
            if (!name.empty() && seen.insert(FoldTagName(name)).second)
            {
                canon.push_back(name);
            }
        }
        return SetSessionStoreFieldIn(storeDir, sessionId, kSessionStoreTagsKey, EncodeTagList(canon));
    }

    bool AddSessionTagIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& tag)
    {
        const std::wstring name = NormalizeTagName(tag);
        if (name.empty())
        {
            return false;
        }
        auto tags = GetSessionTagsIn(storeDir, sessionId);
        const std::wstring folded = FoldTagName(name);
        for (const auto& t : tags)
        {
            if (FoldTagName(t) == folded)
            {
                return false; // already tagged (case-insensitive) — no write
            }
        }
        tags.push_back(name);
        return SetSessionTagsIn(storeDir, sessionId, tags);
    }

    bool RemoveSessionTagIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& tag)
    {
        const std::wstring folded = FoldTagName(NormalizeTagName(tag));
        if (folded.empty())
        {
            return false;
        }
        auto tags = GetSessionTagsIn(storeDir, sessionId);
        const size_t before = tags.size();
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::wstring& t) { return FoldTagName(t) == folded; }),
                   tags.end());
        if (tags.size() == before)
        {
            return false; // wasn't tagged — no write
        }
        return SetSessionTagsIn(storeDir, sessionId, tags);
    }

    std::unordered_map<std::wstring, std::vector<std::wstring>> LoadAllSessionTagsIn(const std::wstring& storeDir)
    {
        std::unordered_map<std::wstring, std::vector<std::wstring>> out;
        for (const auto& [sid, value] : LoadAllSessionStoreFieldIn(storeDir, kSessionStoreTagsKey))
        {
            auto tags = DecodeTagList(value);
            if (!tags.empty())
            {
                out.emplace(sid, std::move(tags));
            }
        }
        return out;
    }

    std::vector<std::wstring> GetSessionTags(const std::wstring& sessionId)
    {
        return GetSessionTagsIn(StoreDir(), sessionId);
    }
    bool SetSessionTags(const std::wstring& sessionId, const std::vector<std::wstring>& tags)
    {
        return SetSessionTagsIn(StoreDir(), sessionId, tags);
    }
    bool AddSessionTag(const std::wstring& sessionId, const std::wstring& tag)
    {
        return AddSessionTagIn(StoreDir(), sessionId, tag);
    }
    bool RemoveSessionTag(const std::wstring& sessionId, const std::wstring& tag)
    {
        return RemoveSessionTagIn(StoreDir(), sessionId, tag);
    }
    std::unordered_map<std::wstring, std::vector<std::wstring>> LoadAllSessionTags()
    {
        return LoadAllSessionTagsIn(StoreDir());
    }

    // ===== typed convenience: the TAG COLORS (tag-colors.json — see SessionStore.h) ==========

    namespace
    {
        std::wstring TagColorsPath(const std::wstring& stateDir)
        {
            return stateDir + L"\\tag-colors.json";
        }

        // "#RRGGBB" / "#AARRGGBB" shape only — enforced on write AND load so a mangled file can
        // never feed a garbage color into the UI (the reader falls back to the name hash instead).
        bool ValidTagColorHex(const std::wstring& hex)
        {
            if ((hex.size() != 7 && hex.size() != 9) || hex[0] != L'#')
            {
                return false;
            }
            for (size_t i = 1; i < hex.size(); ++i)
            {
                const wchar_t c = hex[i];
                if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
                {
                    return false;
                }
            }
            return true;
        }
    }

    std::map<std::wstring, std::wstring> LoadAllTagColorsIn(const std::wstring& stateDir)
    {
        std::map<std::wstring, std::wstring> out;
        if (stateDir.empty())
        {
            return out;
        }
        const std::string bytes = ReadWhole(TagColorsPath(stateDir), 1u << 20);
        if (bytes.empty())
        {
            return out; // no file yet / unreadable — every tag falls back to its hash color
        }
        const auto parsed = json::Parse(Utf8ToWide(bytes.data(), bytes.size()));
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return out;
        }
        for (const auto& [k, v] : parsed->members)
        {
            if (v.type != json::Value::Type::Str || !ValidTagColorHex(v.str))
            {
                continue;
            }
            // Keys are stored folded already, but re-fold defensively so a hand-edited casing
            // still lands on the tag's case-insensitive identity.
            const std::wstring folded = FoldTagName(NormalizeTagName(k));
            if (!folded.empty())
            {
                out[folded] = v.str;
            }
        }
        return out;
    }

    std::wstring GetTagColorIn(const std::wstring& stateDir, const std::wstring& tag)
    {
        const std::wstring folded = FoldTagName(NormalizeTagName(tag));
        if (folded.empty())
        {
            return {};
        }
        const auto all = LoadAllTagColorsIn(stateDir);
        const auto it = all.find(folded);
        return it == all.end() ? std::wstring{} : it->second;
    }

    bool SetTagColorIn(const std::wstring& stateDir, const std::wstring& tag, const std::wstring& hexOrEmpty)
    {
        if (stateDir.empty())
        {
            return false;
        }
        const std::wstring folded = FoldTagName(NormalizeTagName(tag));
        if (folded.empty())
        {
            return false;
        }
        if (!hexOrEmpty.empty() && !ValidTagColorHex(hexOrEmpty))
        {
            return false; // malformed color — refuse rather than store garbage
        }
        auto all = LoadAllTagColorsIn(stateDir);
        const auto it = all.find(folded);
        if (hexOrEmpty.empty())
        {
            if (it == all.end())
            {
                return true; // nothing to remove — no write
            }
            all.erase(it);
        }
        else
        {
            if (it != all.end() && it->second == hexOrEmpty)
            {
                return true; // unchanged — dedup the write
            }
            all[folded] = hexOrEmpty;
        }
        json::Value o = json::Value::MkObj();
        for (const auto& [k, v] : all)
        {
            o.Set(k, json::Value::MkStr(v));
        }
        ::CreateDirectoryW(stateDir.c_str(), nullptr); // idempotent (tests point at a fresh temp dir)
        return AtomicWriteUtf8(TagColorsPath(stateDir), json::Dump(o));
    }

    std::map<std::wstring, std::wstring> LoadAllTagColors()
    {
        return LoadAllTagColorsIn(AgentmasterStateDir());
    }
    std::wstring GetTagColor(const std::wstring& tag)
    {
        return GetTagColorIn(AgentmasterStateDir(), tag);
    }
    bool SetTagColor(const std::wstring& tag, const std::wstring& hex)
    {
        return SetTagColorIn(AgentmasterStateDir(), tag, hex);
    }

    // ===== the KNOWN-TAG registry (tags.json — see SessionStore.h) ===========================

    namespace
    {
        std::wstring KnownTagsPath(const std::wstring& stateDir)
        {
            return stateDir + L"\\tags.json";
        }

        bool WriteKnownTags(const std::wstring& stateDir, const std::vector<std::wstring>& known)
        {
            json::Value a = json::Value::MkArr();
            for (const auto& k : known)
            {
                a.Push(json::Value::MkStr(k));
            }
            ::CreateDirectoryW(stateDir.c_str(), nullptr); // idempotent (tests point at a fresh temp dir)
            return AtomicWriteUtf8(KnownTagsPath(stateDir), json::Dump(a));
        }
    }

    std::vector<std::wstring> LoadKnownTagsIn(const std::wstring& stateDir)
    {
        std::vector<std::wstring> out;
        if (stateDir.empty())
        {
            return out;
        }
        const std::string bytes = ReadWhole(KnownTagsPath(stateDir), 1u << 20);
        if (bytes.empty())
        {
            return out; // no registry yet — the universe is purely session-derived
        }
        const auto parsed = json::Parse(Utf8ToWide(bytes.data(), bytes.size()));
        if (!parsed || parsed->type != json::Value::Type::Arr)
        {
            return out; // malformed -> "no registry" (never crashes the caller)
        }
        std::unordered_set<std::wstring> seen; // folded — CI dedupe, order-preserving
        for (const auto& e : parsed->arr)
        {
            if (e.type != json::Value::Type::Str)
            {
                continue;
            }
            const std::wstring name = NormalizeTagName(e.str);
            if (!name.empty() && seen.insert(FoldTagName(name)).second)
            {
                out.push_back(name);
            }
        }
        return out;
    }

    bool RegisterKnownTagIn(const std::wstring& stateDir, const std::wstring& tag)
    {
        if (stateDir.empty())
        {
            return false;
        }
        const std::wstring name = NormalizeTagName(tag);
        if (name.empty())
        {
            return false;
        }
        auto known = LoadKnownTagsIn(stateDir);
        const std::wstring folded = FoldTagName(name);
        for (const auto& k : known)
        {
            if (FoldTagName(k) == folded)
            {
                return true; // already known — no write
            }
        }
        known.push_back(name);
        return WriteKnownTags(stateDir, known);
    }

    bool UnregisterKnownTagIn(const std::wstring& stateDir, const std::wstring& tag)
    {
        if (stateDir.empty())
        {
            return false;
        }
        const std::wstring folded = FoldTagName(NormalizeTagName(tag));
        if (folded.empty())
        {
            return false;
        }
        auto known = LoadKnownTagsIn(stateDir);
        const size_t before = known.size();
        known.erase(std::remove_if(known.begin(), known.end(), [&](const std::wstring& k) { return FoldTagName(k) == folded; }),
                    known.end());
        if (known.size() == before)
        {
            return true; // wasn't known — no write
        }
        return WriteKnownTags(stateDir, known); // an emptied registry writes "[]" (still a valid file)
    }

    std::vector<std::wstring> LoadKnownTags()
    {
        return LoadKnownTagsIn(AgentmasterStateDir());
    }
    bool RegisterKnownTag(const std::wstring& tag)
    {
        return RegisterKnownTagIn(AgentmasterStateDir(), tag);
    }
    bool UnregisterKnownTag(const std::wstring& tag)
    {
        return UnregisterKnownTagIn(AgentmasterStateDir(), tag);
    }
}
