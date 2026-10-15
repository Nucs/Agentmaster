// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionStore implementation (see SessionStore.h). Pure C++/Win32, no WinRT, no PCH.

#include "SessionStore.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir
#include "Json.h"

#include <windows.h>

#include <algorithm>
#include <atomic> // the tag-change epoch (SessionTagsEpoch)
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

        // Agentmaster (tags perf, 2026-10-10): how many record FILES this process has opened to parse
        // (ParseRecord) since it started — the count behind SessionStoreRecordFileReads() (SessionStore.h).
        // A statistic, never a synchronization point, hence the relaxed ordering everywhere. It exists
        // because the store's cost IS its file opens: the session-store warm-up logs it, and the harness
        // reads it to prove the scan cache's two promises (a repeated whole-directory read opens no file;
        // one changed record costs one read) instead of timing them.
        std::atomic<uint64_t> g_recordFileReads{ 0 };
        // Its twin for the cheaper cost: how many times the record cache has STAT'ed a record file to
        // validate an entry (WithRecordCached) — the count behind SessionStoreRecordFileStats(). A warm
        // whole-directory read must not move it either: its fast path trusts the directory listing's
        // stamp, and a regression that sent every record down the per-record path would still open no
        // file but cost one stat each (~40 ms for the release profile's 2,181 records, on the UI thread).
        std::atomic<uint64_t> g_recordFileStats{ 0 };

        // TEST SEAM (SetSessionStoreParseHookForTests, SessionStore.h): called by the record cache between
        // a miss's parse and the store of its result, on the reading thread, with no lock held. Null in
        // the app, always — only the harness sets it, to stage the one interleaving the write-order guard
        // exists for (a write of this process landing while a reader holds an older parse). That race is
        // too narrow to hit with threads alone: 8 runs of four readers beside a writer never failed with
        // the guard removed.
        std::atomic<void (*)()> g_parseHookForTests{ nullptr };

        // Parse <storeDir>\<sid>.json into a record. Only STRING members are kept (the store's value
        // type); a malformed / non-object file yields an empty record (treated as "no data").
        SessionStoreRecord ParseRecord(const std::wstring& storeDir, const std::wstring& sid)
        {
            // Counted BEFORE the read: the open is the cost, whatever the file then holds (measured: most
            // of the ~85 ms that 2,181 records took was open + read, not the JSON parse).
            g_recordFileReads.fetch_add(1, std::memory_order_relaxed);
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
            // g_recCacheWrites as it stood when a WRITE of this process last stored this entry (WriteRecord);
            // 0 for an entry a read stored. A reader compares it with the count it saw BEFORE it parsed —
            // see the write-order guard in WithRecordCached.
            uint64_t writtenAt = 0;
        };
        std::mutex g_recCacheMtx;
        std::unordered_map<std::wstring, RecCacheEntry> g_recCache; // key: case-folded full path
        // Agentmaster (tags perf, 2026-10-10): the number of write-throughs so far (WriteRecord), guarded by
        // g_recCacheMtx. It orders a reader's parse against this process's own writes — the write-order
        // guard in WithRecordCached. Starts at 0 and only grows, so an entry's writtenAt of 0 can never be
        // "newer" than any count a reader saw.
        uint64_t g_recCacheWrites = 0;

        // Agentmaster (SIDE_PANEL.md §2c): the TAG-CHANGE EPOCH behind SessionTagsEpoch (SessionStore.h).
        // Atomic: tags are written from any window's UI thread and read from every other window's.
        std::atomic<uint64_t> g_tagsEpoch{ 0 };

        // Move the epoch: a session's tag list or a tag's color has just CHANGED on disk. Called only
        // after the write (and its write-through cache update) succeeded — the ordering the reader
        // contract in SessionStore.h relies on. Release order publishes the completed write to a reader
        // that acquires the new value.
        //   throws — nothing.
        void NoteTagsChanged() noexcept
        {
            g_tagsEpoch.fetch_add(1, std::memory_order_acq_rel);
        }

        // Agentmaster (SESSIONS_DB.md §7.6): the FACTS EPOCH behind SessionFactsEpoch — a session's title,
        // favorite or tags changed on disk. Bumped after the write, like the tag epoch.
        std::atomic<uint64_t> g_factsEpoch{ 0 };

        // Whether a session-store key is one of the facts the session database mirrors (am_title, am_favorite,
        // am_tag): drafts, command progress and launch snapshots are not.
        bool IsFactKey(const std::wstring& key) noexcept
        {
            return key == kSessionStoreTitleKey || key == kSessionStoreFavoriteKey || key == kSessionStoreTagsKey;
        }

        // Agentmaster (tags perf, 2026-10-10): the stores of this process that had ONE complete
        // whole-directory read (VisitStoreDir) — the "warm" set behind SessionStoreScanIsWarmIn. Key: the
        // case-folded store directory (RecCacheKey). Guarded by g_recCacheMtx, like the cache it describes.
        // An entry is never removed: once every record of a directory is cached, a later read re-parses
        // only what changed, however long the process lives.
        std::unordered_set<std::wstring> g_scannedDirs;

        // Case-fold ONE character of a cache key. Every key is built through this (RecCacheKey for a whole
        // path, VisitStoreDir for a session id appended to a pre-folded directory prefix), so the two
        // builders can never disagree about a key.
        // ASCII is folded inline: the whole-directory read builds one key per record (thousands per call),
        // and ::towlower is a call into the CRT per character. For ASCII both give the same answer in
        // every locale, so the shortcut changes no key.
        //   c       - the character.
        //   returns - its lower-case form (unchanged when it has none).
        //   throws  - nothing.
        wchar_t FoldPathChar(const wchar_t c) noexcept
        {
            if (c < 0x80)
            {
                return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
            }
            return static_cast<wchar_t>(::towlower(c));
        }

        std::wstring RecCacheKey(const std::wstring& path)
        {
            std::wstring k = path;
            for (auto& c : k)
            {
                c = FoldPathChar(c);
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

        // Agentmaster (tags perf, 2026-10-10): run `read` on the cached record of <storeDir>\<sid>.json and
        // return its result. This is the ONE per-record read of the cache: LoadRecordCached (the whole
        // record) and GetSessionStoreFieldIn (one field) both come through it, so a single-field read
        // copies that field and not the record around it. The Manager board asks for `tags` per card per
        // rebuild, and the same record can hold a whole paste-expanded draft (PENDING_INPUT.md §8c) that a
        // whole-record copy dragged along every time.
        // A hit whose (present,size,mtime) still matches a fresh stat is served from memory; anything else
        // re-parses and refreshes the entry first (a transient stat miss mid-atomic-replace simply caches
        // "absent" for one round and self-heals on the next stat).
        //   storeDir - the store directory (non-empty; the caller checked).
        //   sid      - a session id that passed ValidSid (the caller checked).
        //   read     - called exactly once as read(const SessionStoreRecord&) WHILE g_recCacheMtx is held.
        //              It must only copy out of the record: the mutex is not recursive, so a `read` that
        //              calls back into the store deadlocks, and one that blocks stalls every other reader.
        //   returns  - whatever `read` returns, by value.
        //   throws   - std::bad_alloc, and whatever `read` throws. The cache entry is complete either way
        //              (it is refreshed before `read` runs).
        template<typename Read>
        auto WithRecordCached(const std::wstring& storeDir, const std::wstring& sid, Read&& read)
        {
            const std::wstring path = FilePath(storeDir, sid);
            uint64_t size = 0;
            int64_t mtime = 0;
            g_recordFileStats.fetch_add(1, std::memory_order_relaxed); // the per-record validation's one system call
            const bool present = StatFile(path, size, mtime);
            const std::wstring key = RecCacheKey(path);
            uint64_t writesSeen = 0; // g_recCacheWrites before the parse below (the write-order guard)
            {
                std::lock_guard guard{ g_recCacheMtx };
                const auto it = g_recCache.find(key);
                if (it != g_recCache.end() && it->second.present == present && it->second.size == size && it->second.mtime == mtime)
                {
                    return read(static_cast<const SessionStoreRecord&>(it->second.rec));
                }
                writesSeen = g_recCacheWrites;
            }
            // Parsed OUTSIDE the lock: the open + read of a cold file can take milliseconds, and every
            // other reader of the cache (the UI thread's per-card reads) must not wait behind it.
            SessionStoreRecord rec = present ? ParseRecord(storeDir, sid) : SessionStoreRecord{};
            if (const auto hook = g_parseHookForTests.load(std::memory_order_relaxed))
            {
                hook(); // harness only (null in the app): lets a test write the record exactly here
            }
            std::lock_guard guard{ g_recCacheMtx };
            auto& entry = g_recCache[key];
            // The WRITE-ORDER GUARD. While this thread parsed, a write of THIS process may have replaced
            // the record and written it through (WriteRecord). What was parsed here can then be the OLDER
            // content, and storing it would put that older content back under a stamp taken before the
            // write. Normally the next stat would catch the difference, but not when the new file has the
            // same size and the same timestamp tick as the one parsed — then the stale record would be
            // served until the file changed again. So an entry that a write stored after this read began
            // is kept, and served: it is at least as new as anything this thread could have parsed.
            // (Readers on other threads — the start-up warm-up, the Sessions gather — run beside the UI
            // thread's tag and title writes and the draft writer, so the order is not hypothetical.)
            if (entry.writtenAt > writesSeen)
            {
                return read(static_cast<const SessionStoreRecord&>(entry.rec));
            }
            entry = RecCacheEntry{ std::move(rec), size, mtime, present, 0 };
            return read(static_cast<const SessionStoreRecord&>(entry.rec));
        }

        // The cached read behind LoadSessionStoreIn. A hit whose (present,size,mtime) still matches is a
        // map copy; anything else re-parses and refreshes the entry (a transient stat miss mid-atomic-
        // replace simply caches "absent" for one round and self-heals on the next stat).
        SessionStoreRecord LoadRecordCached(const std::wstring& storeDir, const std::wstring& sid)
        {
            return WithRecordCached(storeDir, sid, [](const SessionStoreRecord& rec) { return rec; });
        }

        // One entry of a store directory listing (ListStoreDir): the session id — the file's stem — and
        // the stamp the LISTING reports for <sid>.json.
        struct StoreDirItem
        {
            std::wstring sid;
            uint64_t size = 0;
            int64_t mtime = 0; // FILETIME ticks, the unit StatFile reports
        };

        // Agentmaster (tags perf, 2026-10-10): list <storeDir>\*.json ONCE — every record file whose stem is
        // a valid session id, each with the (size, mtime) of its directory entry. No file is opened.
        // FindExInfoBasic leaves out the 8.3 short name of every entry, and FIND_FIRST_EX_LARGE_FETCH asks
        // for a larger kernel buffer, so a directory of thousands of records costs a few system calls
        // (measured on the release profile: 2,181 records in ~3.5 ms, against ~85 ms to open and parse them).
        // The filter is the one the old per-file scan applied: a directory is skipped, the leaf must end in
        // ".json" (case-sensitive, as before), and the stem must pass ValidSid.
        //   storeDir - the directory to list.
        //   returns  - the entries in the file system's order; empty when the directory is missing, is
        //              empty, or cannot be listed (the three read alike: there is nothing to visit).
        //   throws   - std::bad_alloc.
        std::vector<StoreDirItem> ListStoreDir(const std::wstring& storeDir)
        {
            std::vector<StoreDirItem> items;
            WIN32_FIND_DATAW fd{};
            const std::wstring glob = storeDir + L"\\*.json";
            const HANDLE h = ::FindFirstFileExW(glob.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE)
            {
                return items; // no store dir yet / empty
            }
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                const std::wstring name = fd.cFileName;
                const size_t dot = name.size() >= 5 ? name.size() - 5 : std::wstring::npos; // ".json"
                if (dot == std::wstring::npos || name.compare(dot, 5, L".json") != 0)
                {
                    continue;
                }
                StoreDirItem item;
                item.sid = name.substr(0, dot);
                if (!ValidSid(item.sid))
                {
                    continue;
                }
                item.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                ULARGE_INTEGER li{};
                li.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                item.mtime = static_cast<int64_t>(li.QuadPart);
                items.push_back(std::move(item));
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
            return items;
        }

        // Agentmaster (tags perf, 2026-10-10): visit EVERY record of a store directory — the
        // whole-directory read behind LoadAllSessionStoreFieldIn and the warm-up (WarmSessionStoreIn).
        //
        // Why it exists: the old scan opened, read and JSON-parsed every record file on every call. The
        // store was designed as sparse, but every launched session gets a `title` record, so the release
        // profile held 2,181 files by 2026-10-10. The Tags panel ran that scan on the UI thread each time it
        // opened and after every toggle (~85 ms measured with a warm disk cache; the user reported more
        // than 5 s for the first open, which was not measured), and a hover on any tag ribbon ran it again.
        //
        // How: ONE directory listing (ListStoreDir), then per record:
        //   - FAST: the listing's (size, mtime) equals the cached entry's stamp => `visit` gets the cached
        //     record. No stat, no open. In the steady state that is every record.
        //   - SLOW: no entry yet, or a different stamp => the per-record path (WithRecordCached): one stat,
        //     and a re-parse only when THAT stamp differs from the cache too.
        // So the first visit of a directory reads each file once, and every later visit reads only the
        // files that changed since.
        //
        // The listing's stamp is only ever COMPARED, never stored: every cached stamp comes from a per-file
        // stat (WithRecordCached, WriteRecord). If a file system reported different stamps through the two
        // calls, the cost would be one stat per such record per visit, never a re-parse ping-pong between
        // the two readers. They agree on NTFS: 2,601 existing records and 1,200 fresh atomic writes compared
        // equal on 2026-10-10.
        // Trusting the listing is as safe as trusting the stat: every writer of a record replaces the file
        // through a temp file + rename (AtomicWriteUtf8), which gives the new directory entry the final
        // size and time, and a file edited in place by another tool gets its entry updated when that tool
        // closes it. The one blind spot is the cache's old one: a change that keeps both the size and the
        // time (two writes inside one timestamp tick), which a same-process writer closes by writing
        // through (WriteRecord).
        //
        // A completed pass marks the directory as scanned (SessionStoreScanIsWarmIn).
        //   storeDir - the directory. A missing one visits nothing and still counts as scanned: there is
        //              nothing in it to read.
        //   visit    - called once per record as visit(const std::wstring& sid, const SessionStoreRecord&)
        //              WHILE g_recCacheMtx is held — the WithRecordCached rules: copy out of the record,
        //              never call back into the store, never block.
        //   returns  - the number of records visited.
        //   throws   - std::bad_alloc, and whatever `visit` throws. The directory is then NOT marked
        //              scanned; the records visited so far stay cached, so a retry continues from them.
        template<typename Visit>
        size_t VisitStoreDir(const std::wstring& storeDir, Visit&& visit)
        {
            const std::vector<StoreDirItem> items = ListStoreDir(storeDir);
            // Each record's cache key is this prefix + its folded id + ".json" — the very string
            // RecCacheKey(FilePath(storeDir, sid)) builds, without folding the directory again per record.
            const std::wstring keyPrefix = RecCacheKey(storeDir + L"\\");
            std::wstring key;
            for (const auto& item : items)
            {
                key.assign(keyPrefix);
                for (const wchar_t c : item.sid)
                {
                    key.push_back(FoldPathChar(c));
                }
                key.append(L".json"); // already lower-case: ListStoreDir admits only this exact suffix
                bool served = false;
                {
                    std::lock_guard guard{ g_recCacheMtx };
                    const auto it = g_recCache.find(key);
                    if (it != g_recCache.end() && it->second.present && it->second.size == item.size && it->second.mtime == item.mtime)
                    {
                        visit(item.sid, static_cast<const SessionStoreRecord&>(it->second.rec));
                        served = true;
                    }
                }
                if (!served)
                {
                    // Unknown or changed: stat it, and parse it only when the stat disagrees with the cache
                    // as well. A record deleted since the listing reads as an empty record here.
                    WithRecordCached(storeDir, item.sid, [&](const SessionStoreRecord& rec) {
                        visit(item.sid, rec);
                        return true;
                    });
                }
            }
            {
                std::lock_guard guard{ g_recCacheMtx };
                g_scannedDirs.insert(RecCacheKey(storeDir));
            }
            return items.size();
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
                // writtenAt = the new write count: a reader that parsed the file before this delete must
                // not store its copy over the "absent" entry (WithRecordCached's write-order guard).
                g_recCache[RecCacheKey(path)] = RecCacheEntry{ {}, 0, 0, false, ++g_recCacheWrites }; // absent, empty record
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
            g_recCache[RecCacheKey(path)] = RecCacheEntry{ rec, size, mtime, true, ++g_recCacheWrites }; // stamped for the write-order guard
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
        if (storeDir.empty() || !ValidSid(sessionId))
        {
            return {}; // the guard LoadSessionStoreIn applies (this read no longer goes through it)
        }
        // Copy ONE field out of the cached record, under the cache lock. This used to copy the whole
        // record first — every key, a stored draft included — to look up one of them; the board reads
        // `tags` this way for every card on every rebuild.
        return WithRecordCached(storeDir, sessionId, [&key](const SessionStoreRecord& rec) {
            const auto it = rec.find(key);
            return it == rec.end() ? std::wstring{} : it->second;
        });
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
        const bool written = WriteRecord(storeDir, sessionId, rec);
        // Only a CHANGED tag list moves the epoch (SessionTagsEpoch): the dedup returns above never get
        // here, and every other key (title / favorite / draft / cmdProgress) leaves the tags as they were.
        if (written && key == kSessionStoreTagsKey)
        {
            NoteTagsChanged();
        }
        if (written && IsFactKey(key))
        {
            g_factsEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        return written;
    }

    bool SetSessionStoreFieldsIn(const std::wstring& storeDir, const std::wstring& sessionId, const SessionStoreRecord& fields)
    {
        if (storeDir.empty() || !ValidSid(sessionId) || fields.empty())
        {
            return false;
        }
        SessionStoreRecord rec = ParseRecord(storeDir, sessionId); // freshest-disk RMW (as the single-field setter)
        bool changed = false;
        bool tagsChanged = false; // the batch changed the `tags` key — moves the tag-change epoch once written
        bool factsChanged = false; // the batch changed a title, favorite or tags — moves the facts epoch once written
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
                    tagsChanged = tagsChanged || key == kSessionStoreTagsKey;
                    factsChanged = factsChanged || IsFactKey(key);
                }
                continue; // absent already — nothing to remove
            }
            if (present && it->second == value)
            {
                continue; // unchanged
            }
            rec[key] = value;
            changed = true;
            tagsChanged = tagsChanged || key == kSessionStoreTagsKey;
            factsChanged = factsChanged || IsFactKey(key);
        }
        if (!changed)
        {
            return true; // every field already held its value — dedup the write entirely
        }
        ::CreateDirectoryW(storeDir.c_str(), nullptr); // idempotent; only when we actually write
        const bool written = WriteRecord(storeDir, sessionId, rec);
        if (written && tagsChanged)
        {
            NoteTagsChanged(); // after the write — the SessionTagsEpoch reader contract
        }
        if (written && factsChanged)
        {
            g_factsEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        return written;
    }

    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& key)
    {
        std::unordered_map<std::wstring, std::wstring> out;
        if (storeDir.empty() || key.empty())
        {
            return out;
        }
        // Agentmaster (tags perf, 2026-10-10): through the scan cache (VisitStoreDir) — one directory
        // listing, and a file is opened only when it is new or changed since this process last read it.
        // The old body opened and parsed EVERY record file on every call. The enumeration and its filter
        // (ListStoreDir) are the ones that body had; the records come from the same cache the per-session
        // reads use, so the two can never disagree about a record.
        VisitStoreDir(storeDir, [&](const std::wstring& sid, const SessionStoreRecord& rec) {
            const auto it = rec.find(key);
            if (it != rec.end() && !it->second.empty())
            {
                out[sid] = it->second;
            }
        });
        return out;
    }

    // ===== the scan cache's public face (see SessionStore.h) =================================

    bool SessionStoreScanIsWarmIn(const std::wstring& storeDir)
    {
        if (storeDir.empty())
        {
            return false;
        }
        const std::wstring key = RecCacheKey(storeDir);
        std::lock_guard guard{ g_recCacheMtx };
        return g_scannedDirs.find(key) != g_scannedDirs.end();
    }

    size_t WarmSessionStoreIn(const std::wstring& storeDir)
    {
        if (storeDir.empty())
        {
            return 0;
        }
        // A visit that keeps nothing: its whole effect is the cache it fills and the scanned mark.
        return VisitStoreDir(storeDir, [](const std::wstring&, const SessionStoreRecord&) {});
    }

    uint64_t SessionStoreRecordFileReads() noexcept
    {
        return g_recordFileReads.load(std::memory_order_relaxed);
    }

    uint64_t SessionStoreRecordFileStats() noexcept
    {
        return g_recordFileStats.load(std::memory_order_relaxed);
    }

    void SetSessionStoreParseHookForTests(void (*hook)()) noexcept
    {
        g_parseHookForTests.store(hook, std::memory_order_relaxed);
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
    bool SessionStoreScanIsWarm()
    {
        return SessionStoreScanIsWarmIn(StoreDir());
    }
    size_t WarmSessionStore()
    {
        return WarmSessionStoreIn(StoreDir());
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

        // Agentmaster (tags perf, 2026-10-10): the two small profile-level tag files — tag-colors.json and
        // tags.json — parsed ONCE per change instead of once per reader.
        // Every tag surface resolves colors through LoadAllTagColorsIn: each tagged tab's badge re-assert
        // (launch, bind, restore, every toggle), every Manager board rebuild that shows a tag, every
        // Sessions table render, the hover panel, the Tags panel. Each call opened, read and JSON-parsed
        // the file. Now a read is one stat (the record cache's (size,mtime) validation) and a copy of the
        // parsed value; the file is parsed again only after its stamp moved.
        // The same rules as the record cache above:
        //   - an edit by another process is caught by the stat on the next read;
        //   - a write by THIS process is written through (SetTagColorIn, WriteKnownTags), so a rapid
        //     write -> read never depends on the timestamp's granularity;
        //   - a read-modify-write never reads through the cache (ReadTagColorsFile, ReadKnownTagsFile):
        //     writing back a stale copy would silently undo another writer's change, where a stale
        //     display only shows an old color until the next stamp change.
        // Unlike the record cache, g_tagFilesMtx is HELD ACROSS the file I/O — a reader's stat and (on a
        // miss) its read, and a writer's whole read-modify-write. Each file is tiny and there are two of
        // them, so the wait is tens of microseconds; in exchange every reader and writer of this process
        // is serialized, which needs no ordering guard (a reader can never store an older parse over a
        // newer write) and also stops two threads' read-modify-writes from losing one another's change.
        // The record cache cannot do this: its first pass over a directory reads thousands of files.
        // One entry per distinct file path: one profile in the app, a few temp directories in the harness.
        struct TagColorsCacheEntry
        {
            std::map<std::wstring, std::wstring> colors; // folded name -> "#AARRGGBB", as ReadTagColorsFile returns it
            uint64_t size = 0;
            int64_t mtime = 0; // FILETIME ticks
            bool present = false; // the file existed at the last look (an absent file caches as an empty map)
        };
        struct KnownTagsCacheEntry
        {
            std::vector<std::wstring> tags; // display-cased, CI-deduped, registration order (ReadKnownTagsFile)
            uint64_t size = 0;
            int64_t mtime = 0; // FILETIME ticks
            bool present = false;
        };
        std::mutex g_tagFilesMtx; // guards both maps below AND serializes every read / write of the two files (see above). Not recursive
        std::unordered_map<std::wstring, TagColorsCacheEntry> g_tagColorsCache; // key: RecCacheKey(TagColorsPath(dir))
        std::unordered_map<std::wstring, KnownTagsCacheEntry> g_knownTagsCache; // key: RecCacheKey(KnownTagsPath(dir))

        // Read + parse + validate <stateDir>\tag-colors.json straight from disk — the body LoadAllTagColorsIn
        // had before the cache. Callers (both hold g_tagFilesMtx): the cached read on a stamp change, and
        // SetTagColorIn's read-modify-write, which must see the freshest file (see the note above).
        //   stateDir - the profile directory (non-empty; the callers checked).
        //   returns  - folded tag name -> color; empty when the file is missing, unreadable, over 1 MiB or
        //              not a JSON object. An entry with a malformed color is left out.
        //   throws   - std::bad_alloc.
        std::map<std::wstring, std::wstring> ReadTagColorsFile(const std::wstring& stateDir)
        {
            std::map<std::wstring, std::wstring> out;
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
    }

    std::map<std::wstring, std::wstring> LoadAllTagColorsIn(const std::wstring& stateDir)
    {
        std::map<std::wstring, std::wstring> out;
        if (stateDir.empty())
        {
            return out;
        }
        // Cached, (size,mtime)-validated (g_tagColorsCache): a hit costs one stat and a copy of the map.
        // The lock covers the stat and a miss's file read, so a concurrent SetTagColorIn is either wholly
        // before this read or wholly after it.
        const std::wstring path = TagColorsPath(stateDir);
        const std::wstring cacheKey = RecCacheKey(path);
        std::lock_guard guard{ g_tagFilesMtx };
        uint64_t size = 0;
        int64_t mtime = 0;
        const bool present = StatFile(path, size, mtime);
        const auto it = g_tagColorsCache.find(cacheKey);
        if (it != g_tagColorsCache.end() && it->second.present == present && it->second.size == size && it->second.mtime == mtime)
        {
            return it->second.colors;
        }
        if (present)
        {
            out = ReadTagColorsFile(stateDir);
        }
        g_tagColorsCache[cacheKey] = TagColorsCacheEntry{ out, size, mtime, present };
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
        bool written = false;
        {
            // The whole read-modify-write under g_tagFilesMtx: no reader of this process sees a half-done
            // state, and two writers cannot lose one another's color (see the note above the two caches).
            std::lock_guard guard{ g_tagFilesMtx };
            // Freshest-disk read: straight from the file, never through the cache (a stale copy written
            // back would undo another writer's color — the record store's RMW rule).
            auto all = ReadTagColorsFile(stateDir);
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
            const std::wstring path = TagColorsPath(stateDir);
            written = AtomicWriteUtf8(path, json::Dump(o));
            if (written)
            {
                // WRITE-THROUGH: the next cached read answers with what was just written, even when this
                // write and the previous one share one timestamp tick (the stat validation's blind spot).
                // A failed stat drops the entry instead, so the next read parses the file — never a
                // guessed stamp.
                uint64_t size = 0;
                int64_t mtime = 0;
                if (StatFile(path, size, mtime))
                {
                    g_tagColorsCache[RecCacheKey(path)] = TagColorsCacheEntry{ std::move(all), size, mtime, true };
                }
                else
                {
                    g_tagColorsCache.erase(RecCacheKey(path));
                }
            }
        }
        if (written)
        {
            // After the write AND its cache update (the SessionTagsEpoch reader contract), and outside
            // the lock: a ribbon color changed (the dedup returns above never get here).
            NoteTagsChanged();
        }
        return written;
    }

    uint64_t SessionTagsEpoch() noexcept
    {
        return g_tagsEpoch.load(std::memory_order_acquire);
    }

    uint64_t SessionFactsEpoch() noexcept
    {
        return g_factsEpoch.load(std::memory_order_acquire);
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
            const std::wstring path = KnownTagsPath(stateDir);
            if (!AtomicWriteUtf8(path, json::Dump(a)))
            {
                return false;
            }
            // WRITE-THROUGH (g_knownTagsCache — the note above the two tag-file caches): the next cached
            // read answers with what was just written, whatever the timestamp's granularity. `known` is
            // already what a re-read would give: both callers build it from ReadKnownTagsFile's
            // normalized, CI-deduped list. A failed stat drops the entry, so the next read parses the file.
            // ⚠ The CALLER holds g_tagFilesMtx (Register / Unregister take it for their whole
            // read-modify-write), so the maps are touched here without locking — the mutex is not recursive.
            uint64_t size = 0;
            int64_t mtime = 0;
            if (StatFile(path, size, mtime))
            {
                g_knownTagsCache[RecCacheKey(path)] = KnownTagsCacheEntry{ known, size, mtime, true };
            }
            else
            {
                g_knownTagsCache.erase(RecCacheKey(path));
            }
            return true;
        }

        // Read + parse <stateDir>\tags.json straight from disk — the body LoadKnownTagsIn had before the
        // cache. Callers (all hold g_tagFilesMtx): the cached read on a stamp change, and the Register /
        // Unregister read-modify-writes, which must see the freshest file (the note above the two caches).
        //   stateDir - the profile directory (non-empty; the callers checked).
        //   returns  - the registered names: display-cased, CI-deduped, in registration order; empty when
        //              the file is missing, unreadable, over 1 MiB or not a JSON array.
        //   throws   - std::bad_alloc.
        std::vector<std::wstring> ReadKnownTagsFile(const std::wstring& stateDir)
        {
            std::vector<std::wstring> out;
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
    }

    std::vector<std::wstring> LoadKnownTagsIn(const std::wstring& stateDir)
    {
        std::vector<std::wstring> out;
        if (stateDir.empty())
        {
            return out;
        }
        // Cached, (size,mtime)-validated (g_knownTagsCache): a hit costs one stat and a copy of the list.
        // The Tags panel reads the registry on every open and after every toggle. The lock covers the stat
        // and a miss's file read, so a concurrent Register / Unregister is wholly before or wholly after.
        const std::wstring path = KnownTagsPath(stateDir);
        const std::wstring cacheKey = RecCacheKey(path);
        std::lock_guard guard{ g_tagFilesMtx };
        uint64_t size = 0;
        int64_t mtime = 0;
        const bool present = StatFile(path, size, mtime);
        const auto it = g_knownTagsCache.find(cacheKey);
        if (it != g_knownTagsCache.end() && it->second.present == present && it->second.size == size && it->second.mtime == mtime)
        {
            return it->second.tags;
        }
        if (present)
        {
            out = ReadKnownTagsFile(stateDir);
        }
        g_knownTagsCache[cacheKey] = KnownTagsCacheEntry{ out, size, mtime, present };
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
        std::lock_guard guard{ g_tagFilesMtx }; // the whole read-modify-write (WriteKnownTags relies on it)
        auto known = ReadKnownTagsFile(stateDir); // freshest-disk read-modify-write, never the cache
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
        std::lock_guard guard{ g_tagFilesMtx }; // the whole read-modify-write (WriteKnownTags relies on it)
        auto known = ReadKnownTagsFile(stateDir); // freshest-disk read-modify-write, never the cache
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
