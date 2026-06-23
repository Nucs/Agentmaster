// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — SessionStore implementation (see SessionStore.h). Pure C++/Win32, no WinRT, no PCH.

#include "SessionStore.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir
#include "Json.h"

#include <windows.h>

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

        // Write a record (deleting the file when empty, so the store stays sparse).
        bool WriteRecord(const std::wstring& storeDir, const std::wstring& sid, const SessionStoreRecord& rec)
        {
            if (rec.empty())
            {
                ::DeleteFileW(FilePath(storeDir, sid).c_str());
                return true;
            }
            json::Value o = json::Value::MkObj();
            for (const auto& [k, v] : rec)
            {
                o.Set(k, json::Value::MkStr(v));
            }
            return AtomicWriteUtf8(FilePath(storeDir, sid), json::Dump(o));
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
        return ParseRecord(storeDir, sessionId);
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
}
