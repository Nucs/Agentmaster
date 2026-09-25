// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See TranscriptWatcher.h for the design.
#define NOMINMAX
#include "TranscriptWatcher.h"

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <new>
#include <system_error>
#include <utility>

namespace
{
    // Lowercase a component for use as a map key. Project folder names are ASCII (Claude's cwd encoding
    // maps every non-[A-Za-z0-9] character to '-') and session ids are hex GUIDs, so towlower is exact here.
    std::wstring Lower(std::wstring_view s)
    {
        std::wstring out{ s };
        for (auto& c : out)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return out;
    }

    // Case-insensitive ordinal equality (the filesystem's rule for these names). Never throws.
    bool EqualsNoCase(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    // Case-insensitive ordinal "a < b" — the order the index keeps one id's candidate paths in (NTFS
    // enumerates a folder case-insensitively, which is the order the legacy walk met its first match in).
    bool LessNoCase(const std::wstring& a, const std::wstring& b) noexcept
    {
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }

    // True when `name` ends in ".jsonl" (any case) AND has a non-empty stem in front of it.
    bool IsJsonlLeaf(std::wstring_view name) noexcept
    {
        constexpr std::wstring_view kExt = L".jsonl";
        return name.size() > kExt.size() && EqualsNoCase(name.substr(name.size() - kExt.size()), kExt);
    }

    // FILE_ACTION_* -> the typed action; false for a code this build does not know (skipped).
    bool MapAction(DWORD action, Agentmaster::ProjectsChangeAction& out) noexcept
    {
        using A = Agentmaster::ProjectsChangeAction;
        switch (action)
        {
        case FILE_ACTION_ADDED:
            out = A::Added;
            return true;
        case FILE_ACTION_REMOVED:
            out = A::Removed;
            return true;
        case FILE_ACTION_MODIFIED:
            out = A::Modified;
            return true;
        case FILE_ACTION_RENAMED_OLD_NAME:
            out = A::RenamedOld;
            return true;
        case FILE_ACTION_RENAMED_NEW_NAME:
            out = A::RenamedNew;
            return true;
        default:
            return false;
        }
    }

    // Collect the "<id>.jsonl" FILES directly inside `folder` (never recursing: a transcript is a direct
    // child of its project folder, and subagent transcripts live one level deeper, in <id>\subagents).
    // Appends file names to `out`. A missing folder is simply empty. Never throws (beyond std::bad_alloc).
    void ListJsonlFiles(const std::wstring& folder, std::vector<std::wstring>& out)
    {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = folder + L"\\*.jsonl";
        // FindExInfoBasic skips the 8.3 short-name lookup and LARGE_FETCH batches the directory reads —
        // a full index walks every project folder once, so the cheaper call matters there.
        const HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && IsJsonlLeaf(fd.cFileName))
            {
                out.emplace_back(fd.cFileName);
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }

    // The OVERLAPPED living in the watcher's opaque storage (placement-constructed in the constructor).
    OVERLAPPED* OverlappedIn(unsigned char* storage) noexcept
    {
        return std::launder(reinterpret_cast<OVERLAPPED*>(storage));
    }
}

namespace Agentmaster
{
    // ===== pure classification ================================================================================

    ProjectsPathInfo ClassifyProjectsRelativePath(std::wstring_view relative)
    {
        // Split on either separator, dropping empty components (a doubled or trailing separator).
        std::vector<std::wstring_view> parts;
        size_t start = 0;
        for (size_t i = 0; i <= relative.size(); ++i)
        {
            if (i == relative.size() || relative[i] == L'\\' || relative[i] == L'/')
            {
                if (i > start)
                {
                    parts.push_back(relative.substr(start, i - start));
                }
                start = i + 1;
            }
        }

        ProjectsPathInfo out;
        if (parts.empty())
        {
            return out; // Ignored: an empty name carries nothing
        }
        if (parts.size() == 1)
        {
            if (IsJsonlLeaf(parts[0]))
            {
                return out; // Ignored: a .jsonl directly under projects\ is not a transcript (the walk never looked there)
            }
            out.kind = ProjectsPathKind::ProjectDir;
            out.projectDir = std::wstring{ parts[0] };
            return out;
        }
        if (parts.size() == 2)
        {
            out.projectDir = std::wstring{ parts[0] };
            if (IsJsonlLeaf(parts[1]))
            {
                out.kind = ProjectsPathKind::Transcript;
                out.fileName = std::wstring{ parts[1] };
                out.sessionId = Lower(parts[1].substr(0, parts[1].size() - 6)); // strip ".jsonl" (6 chars)
                return out;
            }
            // "<proj>\<name>": the session's side folder being created / removed (or another project entry,
            // such as a memory folder — a spurious side touch for an id no session has is harmless).
            out.kind = ProjectsPathKind::SideFile;
            out.sessionId = Lower(parts[1]);
            return out;
        }
        // Depth >= 3: only the two side folders SubagentActivityUnixMs reads count.
        if (EqualsNoCase(parts[2], L"subagents") || EqualsNoCase(parts[2], L"tool-results"))
        {
            out.kind = ProjectsPathKind::SideFile;
            out.projectDir = std::wstring{ parts[0] };
            out.sessionId = Lower(parts[1]);
            return out;
        }
        return out; // Ignored: deeper content the scanner never reads
    }

    // ===== pure index ==========================================================================================

    void TranscriptIndex::ClearPaths() noexcept
    {
        for (auto& [id, entry] : _byId)
        {
            if (!entry.locations.empty())
            {
                entry.locations.clear();
                ++entry.transcriptSeq; // the path may come back with different content: never reuse a seen sequence
            }
        }
        _paths = 0;
    }

    void TranscriptIndex::AddTranscript(std::wstring_view projectDir, std::wstring_view sessionId, std::wstring fullPath)
    {
        auto& entry = _byId[Lower(sessionId)];
        const std::wstring key = Lower(projectDir);
        bool replaced = false;
        for (auto& loc : entry.locations)
        {
            if (loc.projectKey == key)
            {
                loc.path = std::move(fullPath); // the same (project, id) re-added: refresh the spelling, keep one row
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            entry.locations.push_back(Location{ key, std::move(fullPath) });
            ++_paths;
        }
        std::sort(entry.locations.begin(), entry.locations.end(), [](const Location& a, const Location& b) { return LessNoCase(a.path, b.path); });
        ++entry.transcriptSeq;
    }

    void TranscriptIndex::RemoveTranscript(std::wstring_view projectDir, std::wstring_view sessionId)
    {
        auto it = _byId.find(Lower(sessionId));
        if (it == _byId.end())
        {
            return; // never indexed and never touched: no sequence a scanner could hold for it
        }
        auto& locs = it->second.locations;
        const std::wstring key = Lower(projectDir);
        const auto before = locs.size();
        locs.erase(std::remove_if(locs.begin(), locs.end(), [&key](const Location& l) { return l.projectKey == key; }), locs.end());
        _paths -= before - locs.size();
        ++it->second.transcriptSeq; // even a no-op removal: the scanner re-stats, which a vanished file needs
    }

    void TranscriptIndex::RemoveProjectDir(std::wstring_view projectDir)
    {
        const std::wstring key = Lower(projectDir);
        for (auto& [id, entry] : _byId)
        {
            auto& locs = entry.locations;
            const auto before = locs.size();
            locs.erase(std::remove_if(locs.begin(), locs.end(), [&key](const Location& l) { return l.projectKey == key; }), locs.end());
            if (locs.size() != before)
            {
                _paths -= before - locs.size();
                ++entry.transcriptSeq;
            }
        }
    }

    void TranscriptIndex::TouchTranscript(std::wstring_view sessionId)
    {
        ++_byId[Lower(sessionId)].transcriptSeq;
    }

    void TranscriptIndex::TouchSide(std::wstring_view sessionId)
    {
        ++_byId[Lower(sessionId)].sideSeq;
    }

    std::wstring TranscriptIndex::Apply(ProjectsChangeAction action, std::wstring_view relative, std::wstring_view root)
    {
        const auto info = ClassifyProjectsRelativePath(relative);
        switch (info.kind)
        {
        case ProjectsPathKind::Transcript:
        {
            if (action == ProjectsChangeAction::Removed || action == ProjectsChangeAction::RenamedOld)
            {
                RemoveTranscript(info.projectDir, info.sessionId);
                return {};
            }
            if (action == ProjectsChangeAction::Modified)
            {
                // A MODIFIED record for a file already indexed at this project is the common case (an append).
                // One the index has never seen means its ADDED record was missed (a buffer boundary or an
                // overflow race), so index it now instead of dropping the change.
                const auto it = _byId.find(info.sessionId);
                const std::wstring key = Lower(info.projectDir);
                const bool known = it != _byId.end() &&
                                   std::any_of(it->second.locations.begin(), it->second.locations.end(), [&key](const Location& l) { return l.projectKey == key; });
                if (known)
                {
                    TouchTranscript(info.sessionId);
                    return {};
                }
            }
            // Added / RenamedNew, or an unknown Modified: (re-)index the file under its project.
            AddTranscript(info.projectDir, info.sessionId, std::wstring{ root } + L"\\" + info.projectDir + L"\\" + info.fileName);
            return {};
        }
        case ProjectsPathKind::SideFile:
            TouchSide(info.sessionId);
            return {};
        case ProjectsPathKind::ProjectDir:
            if (action == ProjectsChangeAction::Removed || action == ProjectsChangeAction::RenamedOld)
            {
                // The system reports only the folder, never its children: drop everything that lived in it.
                RemoveProjectDir(info.projectDir);
                return {};
            }
            if (action == ProjectsChangeAction::Added || action == ProjectsChangeAction::RenamedNew)
            {
                // A folder created (usually empty — its files will report their own ADDED) or renamed/moved IN
                // with content that will NOT report: the caller enumerates it to be sure.
                return info.projectDir;
            }
            // Modified: a folder's own last-write moves whenever an entry inside it changes. Those entries
            // report themselves; re-enumerating here would turn every new transcript into a folder walk.
            return {};
        case ProjectsPathKind::Ignored:
        default:
            return {};
        }
    }

    std::wstring TranscriptIndex::Resolve(std::wstring_view sessionId) const
    {
        const auto it = _byId.find(Lower(sessionId));
        if (it == _byId.end() || it->second.locations.empty())
        {
            return {};
        }
        return it->second.locations.front().path; // sorted: the legacy walk's first match
    }

    void TranscriptIndex::SequencesFor(std::wstring_view sessionId, uint64_t& transcriptSeq, uint64_t& sideSeq) const
    {
        transcriptSeq = 0;
        sideSeq = 0;
        const auto it = _byId.find(Lower(sessionId));
        if (it != _byId.end())
        {
            transcriptSeq = it->second.transcriptSeq;
            sideSeq = it->second.sideSeq;
        }
    }

    size_t TranscriptIndex::TranscriptCount() const noexcept
    {
        return _paths;
    }

    // ===== the live watcher ====================================================================================

    TranscriptWatcher::TranscriptWatcher(std::wstring projectsDir) :
        _root{ std::move(projectsDir) }
    {
        // The header keeps the OVERLAPPED opaque (no <windows.h> there); make sure the storage fits it and
        // start its lifetime here so every later access is to a real object.
        static_assert(sizeof(OVERLAPPED) <= sizeof(_overlapped), "OVERLAPPED no longer fits the watcher's opaque storage");
        static_assert(alignof(OVERLAPPED) <= 8, "OVERLAPPED needs more alignment than the watcher's opaque storage provides");
        new (_overlapped) OVERLAPPED{};
        // Trim a trailing separator so the paths the index builds never contain "\\".
        while (!_root.empty() && (_root.back() == L'\\' || _root.back() == L'/'))
        {
            _root.pop_back();
        }
    }

    TranscriptWatcher::~TranscriptWatcher()
    {
        Stop();
    }

    void TranscriptWatcher::Start(ChangeCallback onChange)
    {
        bool expected = false;
        if (!_started.compare_exchange_strong(expected, true))
        {
            return; // already running
        }
        _onChange = std::move(onChange);
        // Both events are MANUAL-reset: the stop event must stay signaled for every wait that follows it,
        // and the I/O event is reset by _arm before each read is issued.
        _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        _ioEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (_stopEvent == nullptr || _ioEvent == nullptr)
        {
            const DWORD err = ::GetLastError();
            if (_stopEvent)
            {
                ::CloseHandle(static_cast<HANDLE>(_stopEvent));
                _stopEvent = nullptr;
            }
            if (_ioEvent)
            {
                ::CloseHandle(static_cast<HANDLE>(_ioEvent));
                _ioEvent = nullptr;
            }
            _started.store(false);
            throw std::system_error(static_cast<int>(err), std::system_category(), "TranscriptWatcher: CreateEventW");
        }
        try
        {
            _thread = std::thread([this]() noexcept { _run(); });
        }
        catch (...)
        {
            // No thread: release the events and leave the watcher stopped (the scanner keeps polling).
            ::CloseHandle(static_cast<HANDLE>(_stopEvent));
            ::CloseHandle(static_cast<HANDLE>(_ioEvent));
            _stopEvent = nullptr;
            _ioEvent = nullptr;
            _started.store(false);
            throw;
        }
    }

    void TranscriptWatcher::Stop() noexcept
    {
        if (!_started.exchange(false))
        {
            return;
        }
        if (_stopEvent)
        {
            ::SetEvent(static_cast<HANDLE>(_stopEvent));
        }
        // The thread cancels and DRAINS its pending read and closes the directory handle itself before it
        // returns, so the buffer and the OVERLAPPED are provably idle once join() comes back.
        if (_thread.joinable())
        {
            _thread.join();
        }
        if (_stopEvent)
        {
            ::CloseHandle(static_cast<HANDLE>(_stopEvent));
            _stopEvent = nullptr;
        }
        if (_ioEvent)
        {
            ::CloseHandle(static_cast<HANDLE>(_ioEvent));
            _ioEvent = nullptr;
        }
    }

    TranscriptWatchStamp TranscriptWatcher::Stamp(std::wstring_view sessionId) const
    {
        TranscriptWatchStamp stamp;
        std::lock_guard lk{ _mtx };
        stamp.healthy = _healthy;
        stamp.epoch = _epoch;
        _index.SequencesFor(sessionId, stamp.transcriptSeq, stamp.sideSeq);
        return stamp;
    }

    std::wstring TranscriptWatcher::Resolve(std::wstring_view sessionId) const
    {
        std::lock_guard lk{ _mtx };
        return _index.Resolve(sessionId);
    }

    bool TranscriptWatcher::Healthy() const noexcept
    {
        std::lock_guard lk{ _mtx };
        return _healthy;
    }

    void TranscriptWatcher::_run() noexcept
    {
        const HANDLE stop = static_cast<HANDLE>(_stopEvent);
        bool outageLogged = false;
        for (;;)
        {
            if (::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
            {
                break;
            }
            bool stopRequested = false;
            try
            {
                // Arm the read BEFORE the full enumeration: the kernel starts queueing changes the moment the
                // first ReadDirectoryChangesW is issued, so nothing that happens DURING the walk can fall into
                // the gap between "listed" and "watched" (a change seen twice is idempotent in the index).
                if (_open() && _arm())
                {
                    _fullIndex();
                    outageLogged = false;
                    _notifyNow(); // everything may have changed since the scanner last looked
                    stopRequested = _eventLoop();
                }
            }
            catch (...)
            {
                // Forensics (Rule #18): an allocation failure mid-index or mid-batch. The watch restarts after
                // the back-off below; the scanner polls meanwhile (Healthy() is false), so nothing is lost.
                LogSwallowedException(L"TranscriptWatcher::_run");
            }
            {
                std::lock_guard lk{ _mtx };
                _healthy = false; // from here until the next full index, the scanner must not trust the index
            }
            _closeWatch();
            if (stopRequested)
            {
                break;
            }
            if (!outageLogged)
            {
                // One line per outage (not per retry): the scanner is polling again until the watch comes back.
                outageLogged = true;
                try
                {
                    AppendStateLog(L"scanner.log", L"[watch] cannot watch " + _root + L" (err " + std::to_wstring(_lastError) +
                                                       L") - the scanner polls transcripts until it can; retrying every " +
                                                       std::to_wstring(kTranscriptWatchRetryMs / 1000) + L"s\n");
                }
                catch (...)
                {
                    // Forensics (Rule #18): building the line can only fail on allocation. This function is
                    // noexcept, so the throw must not escape; the retry below proceeds either way.
                    LogSwallowedException(L"TranscriptWatcher::_run (outage log)");
                }
            }
            if (::WaitForSingleObject(stop, static_cast<DWORD>(kTranscriptWatchRetryMs)) == WAIT_OBJECT_0)
            {
                break;
            }
        }
        _closeWatch();
        std::lock_guard lk{ _mtx };
        _healthy = false;
    }

    bool TranscriptWatcher::_open()
    {
        // FILE_LIST_DIRECTORY is the one right ReadDirectoryChangesW needs. Share EVERYTHING — the watch must
        // never block Claude creating, appending, renaming or deleting anything under the tree (DELETE share
        // included, so even removing the projects folder itself is not prevented; the watch just ends).
        const HANDLE h = ::CreateFileW(_root.c_str(),
                                       FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, // a directory handle, for async reads
                                       nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            _lastError = ::GetLastError(); // the outage line's cause (2 = no projects folder yet, 5 = no access)
            return false; // no projects folder yet (a fresh install), or no access: retried after the back-off
        }
        _dir = h;
        _buffer.assign(kTranscriptWatchBufferBytes / sizeof(uint32_t), 0u);
        return true;
    }

    bool TranscriptWatcher::_arm()
    {
        OVERLAPPED* ov = OverlappedIn(_overlapped);
        *ov = OVERLAPPED{};
        ov->hEvent = static_cast<HANDLE>(_ioEvent);
        ::ResetEvent(ov->hEvent);
        // FILE_NAME + DIR_NAME: transcripts and side files appearing / vanishing / renamed, project folders
        // created. SIZE + LAST_WRITE: an append (Claude opens, appends and closes, so both land at once).
        constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
        const auto issue = [&]() {
            return ::ReadDirectoryChangesW(static_cast<HANDLE>(_dir),
                                           _buffer.data(),
                                           static_cast<DWORD>(_buffer.size() * sizeof(uint32_t)),
                                           TRUE, // bWatchSubtree: one watch on the PARENT covers every project folder below it
                                           kFilter,
                                           nullptr,
                                           ov,
                                           nullptr);
        };
        BOOL ok = issue();
        if (!ok && ::GetLastError() == ERROR_INVALID_PARAMETER && _buffer.size() * sizeof(uint32_t) > kTranscriptWatchNetworkBufferBytes)
        {
            // A network share rejects buffers over 64 KiB: retry once at the network size.
            _buffer.assign(kTranscriptWatchNetworkBufferBytes / sizeof(uint32_t), 0u);
            ok = issue();
        }
        _pending = ok != FALSE;
        if (!_pending)
        {
            _lastError = ::GetLastError(); // the outage line's cause
        }
        return _pending;
    }

    bool TranscriptWatcher::_eventLoop()
    {
        const HANDLE waits[2] = { static_cast<HANDLE>(_stopEvent), static_cast<HANDLE>(_ioEvent) };
        // Overflows in a row with no records read in between. A real overflow is followed by a normal read;
        // a read that keeps completing EMPTY at once is a watch that no longer works (seen on redirected or
        // pending-delete folders), and re-indexing each time would spin a core, so a streak ends the watch.
        int overflowStreak = 0;
        for (;;)
        {
            // While an announcement is owed, wake up in time to deliver it at the coalescing boundary.
            DWORD timeout = INFINITE;
            if (_notifyPending)
            {
                const int64_t since = static_cast<int64_t>(::GetTickCount64()) - _lastNotifyTick;
                timeout = since >= kTranscriptWatchCoalesceMs ? 0 : static_cast<DWORD>(kTranscriptWatchCoalesceMs - since);
            }
            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, timeout);
            if (w == WAIT_OBJECT_0)
            {
                return true; // Stop(): _run cancels + drains the read and closes the handle
            }
            if (w == WAIT_TIMEOUT)
            {
                if (_notifyPending)
                {
                    _notifyNow();
                }
                continue;
            }
            if (w != WAIT_OBJECT_0 + 1)
            {
                _lastError = ::GetLastError(); // the outage line's cause
                return false; // WAIT_FAILED — treat as a broken watch (re-open after the back-off)
            }

            DWORD bytes = 0;
            const BOOL done = ::GetOverlappedResult(static_cast<HANDLE>(_dir), OverlappedIn(_overlapped), &bytes, FALSE);
            const DWORD err = done ? ERROR_SUCCESS : ::GetLastError();
            _pending = false; // the read completed (successfully or not): the buffer is ours again

            bool applied = false;
            if (done && bytes > 0)
            {
                // The normal path: apply the records FIRST (they live in _buffer), then re-arm into the same
                // buffer. Changes in between are queued by the kernel on the handle, never lost.
                overflowStreak = 0;
                applied = _applyBuffer(reinterpret_cast<const unsigned char*>(_buffer.data()), bytes);
                if (!_arm())
                {
                    return false;
                }
            }
            else if (done || err == ERROR_NOTIFY_ENUM_DIR)
            {
                // Zero bytes (or ENUM_DIR): the kernel's change queue overflowed between two reads, so an
                // unknown set of changes is gone. Re-arm first (so the re-walk cannot race new changes into a
                // gap), then rebuild the whole index; the epoch bump makes the scanner refresh every session.
                if (++overflowStreak > kTranscriptWatchMaxOverflowStreak)
                {
                    _lastError = done ? static_cast<DWORD>(ERROR_NOTIFY_ENUM_DIR) : err; // the outage line's cause
                    AppendStateLog(L"scanner.log", L"[watch] " + std::to_wstring(overflowStreak) + L" empty reads in a row under " + _root +
                                                       L" - the watch is not delivering changes; re-opening after the back-off\n");
                    return false;
                }
                AppendStateLog(L"scanner.log", L"[watch] change-notification overflow under " + _root + L" -> full re-index\n");
                if (!_arm())
                {
                    return false;
                }
                _fullIndex();
                applied = true;
            }
            else
            {
                // The watch itself broke — the root was deleted or renamed, access was revoked, the volume went
                // away. Back to polling until it can be re-established.
                _lastError = err; // the outage line's cause
                AppendStateLog(L"scanner.log", L"[watch] watch on " + _root + L" ended (err " + std::to_wstring(err) + L") - re-opening\n");
                return false;
            }

            if (applied)
            {
                _notifyPending = true;
                if (static_cast<int64_t>(::GetTickCount64()) - _lastNotifyTick >= kTranscriptWatchCoalesceMs)
                {
                    _notifyNow(); // outside the coalescing window: announce at once (the common, quiet case)
                }
            }
        }
    }

    void TranscriptWatcher::_closeWatch() noexcept
    {
        if (_dir == nullptr)
        {
            return;
        }
        const HANDLE h = static_cast<HANDLE>(_dir);
        if (_pending)
        {
            // Cancel the in-flight read and WAIT for it to finish: the kernel may still write into _buffer and
            // the OVERLAPPED until the cancellation completes, so neither may be reused or freed before that.
            ::CancelIoEx(h, OverlappedIn(_overlapped));
            DWORD ignored = 0;
            ::GetOverlappedResult(h, OverlappedIn(_overlapped), &ignored, TRUE);
            _pending = false;
        }
        ::CloseHandle(h);
        _dir = nullptr;
    }

    void TranscriptWatcher::_fullIndex()
    {
        const uint64_t t0 = ::GetTickCount64();
        // Walk OUTSIDE the lock (hundreds of folders); the scanner keeps asking Stamp/Resolve meanwhile and
        // simply sees the previous state until the swap below.
        struct Found
        {
            std::wstring projectDir;
            std::wstring fileName;
        };
        std::vector<Found> found;
        size_t folders = 0;
        {
            WIN32_FIND_DATAW fd{};
            const std::wstring pattern = _root + L"\\*";
            const HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::vector<std::wstring> files;
                do
                {
                    // FindExSearchLimitToDirectories is only advisory, so the attribute check stays.
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    {
                        continue;
                    }
                    const std::wstring_view name{ fd.cFileName };
                    if (name == L"." || name == L"..")
                    {
                        continue;
                    }
                    ++folders;
                    files.clear();
                    ListJsonlFiles(_root + L"\\" + fd.cFileName, files);
                    for (auto& f : files)
                    {
                        found.push_back(Found{ std::wstring{ name }, std::move(f) });
                    }
                } while (::FindNextFileW(h, &fd));
                ::FindClose(h);
            }
        }
        size_t indexed = 0;
        {
            std::lock_guard lk{ _mtx };
            _index.ClearPaths();
            for (auto& f : found)
            {
                const std::wstring id = f.fileName.substr(0, f.fileName.size() - 6); // strip ".jsonl"
                _index.AddTranscript(f.projectDir, Lower(id), _root + L"\\" + f.projectDir + L"\\" + f.fileName);
            }
            indexed = _index.TranscriptCount();
            ++_epoch;
            _healthy = true;
        }
        AppendStateLog(L"scanner.log", L"[watch] watching " + _root + L" (subtree): " + std::to_wstring(indexed) + L" transcripts in " +
                                           std::to_wstring(folders) + L" project folders indexed in " +
                                           std::to_wstring(::GetTickCount64() - t0) + L" ms\n");
    }

    void TranscriptWatcher::_indexProjectDir(const std::wstring& projectDir)
    {
        std::vector<std::wstring> files;
        ListJsonlFiles(_root + L"\\" + projectDir, files); // outside the lock
        std::lock_guard lk{ _mtx };
        _index.RemoveProjectDir(projectDir); // a renamed-in folder may shadow stale rows: start it clean
        for (auto& f : files)
        {
            const std::wstring id = f.substr(0, f.size() - 6); // strip ".jsonl"
            _index.AddTranscript(projectDir, Lower(id), _root + L"\\" + projectDir + L"\\" + f);
        }
    }

    bool TranscriptWatcher::_applyBuffer(const unsigned char* data, unsigned long bytes)
    {
        // Decode every record first (bounds-checked: the kernel filled at most `bytes`), then apply them all
        // under ONE lock hold, so the scanner never sees half a batch.
        std::vector<std::pair<ProjectsChangeAction, std::wstring>> records;
        size_t off = 0;
        constexpr size_t kHeader = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        while (off + kHeader <= bytes)
        {
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data + off);
            if (off + kHeader + fni->FileNameLength > bytes)
            {
                break; // a truncated record (never expected): ignore it and the rest of the buffer
            }
            ProjectsChangeAction action{};
            if (MapAction(fni->Action, action))
            {
                records.emplace_back(action, std::wstring{ fni->FileName, fni->FileNameLength / sizeof(wchar_t) });
            }
            if (fni->NextEntryOffset == 0)
            {
                break;
            }
            off += fni->NextEntryOffset;
        }
        if (records.empty())
        {
            return false;
        }
        std::vector<std::wstring> foldersToIndex;
        {
            std::lock_guard lk{ _mtx };
            for (const auto& [action, name] : records)
            {
                if (auto folder = _index.Apply(action, name, _root); !folder.empty())
                {
                    foldersToIndex.push_back(std::move(folder));
                }
            }
        }
        for (const auto& folder : foldersToIndex)
        {
            _indexProjectDir(folder); // a created / renamed-in project folder: list what it already holds
        }
        return true;
    }

    void TranscriptWatcher::_notifyNow() noexcept
    {
        _notifyPending = false;
        _lastNotifyTick = static_cast<int64_t>(::GetTickCount64());
        if (!_onChange)
        {
            return;
        }
        try
        {
            _onChange();
        }
        catch (...)
        {
            // Forensics (Rule #18): the scanner's Wake() does not throw today; if a future callback does, the
            // watch keeps running and the scanner still catches up on its own polling cadence.
            LogSwallowedException(L"TranscriptWatcher change callback");
        }
    }
}
