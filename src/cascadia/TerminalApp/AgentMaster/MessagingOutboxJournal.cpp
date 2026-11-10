// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingOutboxJournal (see MessagingOutboxJournal.h): the durable outbox's files on Win32.

#include "MessagingOutboxJournal.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>

#include "ClaudeSpawn.h" // LogSwallowedException (Rule #18)

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>The journal's file name inside the outbox folder.</summary>
        constexpr const wchar_t* kJournalName = L"outbox.jsonl";
        /// <summary>The rewrite's temporary file (renamed over the journal once complete and flushed).</summary>
        constexpr const wchar_t* kJournalTmpName = L"outbox.jsonl.tmp";
        /// <summary>The dead letters' file.</summary>
        constexpr const wchar_t* kDeadName = L"dead.jsonl";
        /// <summary>The one-writer lock file.</summary>
        constexpr const wchar_t* kLockName = L".lock";
        /// <summary>How long the flusher lets appends gather before it flushes them (D-O3: power-safe within ~250 ms).</summary>
        constexpr auto kFlushGather = std::chrono::milliseconds(250);

        /// <summary>A Win32 error code as words for the error texts ("error 32").</summary>
        /// <param name="code">The code (GetLastError).</param>
        /// <returns>The words.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring ErrorWords(DWORD code)
        {
            return L"error " + std::to_wstring(code);
        }

        /// <summary>
        /// Writes every byte through `h`, looping on short writes (WriteFile may write less than asked for a pipe or a
        /// network file; on a local file it writes all or fails).
        /// </summary>
        /// <param name="h">An open handle with write or append access.</param>
        /// <param name="bytes">The bytes.</param>
        /// <returns>True when every byte was written.</returns>
        bool WriteAll(HANDLE h, std::string_view bytes) noexcept
        {
            size_t done = 0;
            while (done < bytes.size())
            {
                const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - done, size_t{ 1u << 30 }));
                DWORD written = 0;
                if (!::WriteFile(h, bytes.data() + done, chunk, &written, nullptr) || written == 0)
                {
                    return false;
                }
                done += written;
            }
            return true;
        }
    }

    OutboxJournal::~OutboxJournal()
    {
        Close();
    }

    bool OutboxJournal::Open(const std::wstring& folder, std::string& bytes, std::wstring& error)
    {
        bytes.clear();
        if (_lock)
        {
            // A second Open on the same object would leak the first lock and confuse the paths: one Open per Close.
            error = L"the outbox is open already";
            return false;
        }
        _folder = folder;
        _path = folder + L"\\" + kJournalName;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ folder }, ec);
        if (ec && !std::filesystem::is_directory(std::filesystem::path{ folder }, ec))
        {
            error = L"the outbox folder could not be made";
            return false;
        }
        // ONE writer per profile (§5.5): no sharing, so a second process's open fails with a sharing violation; and
        // delete-on-close, so a process that is killed leaves no lock behind (Windows closes its handles, the file goes).
        const std::wstring lockPath = folder + L"\\" + kLockName;
        HANDLE lock = ::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock == INVALID_HANDLE_VALUE)
        {
            const DWORD code = ::GetLastError();
            error = code == ERROR_SHARING_VIOLATION ? std::wstring{ L"another process writes this outbox (its .lock is held)" } : L"the outbox lock could not be taken (" + ErrorWords(code) + L")";
            return false;
        }
        _lock = lock;
        try
        {
            HANDLE h = ::CreateFileW(_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                const DWORD code = ::GetLastError();
                if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                {
                    return true; // no journal yet: an empty queue
                }
                error = L"the journal could not be opened (" + ErrorWords(code) + L")";
                ::CloseHandle(static_cast<HANDLE>(_lock));
                _lock = nullptr;
                return false;
            }
            LARGE_INTEGER size{};
            bool ok = ::GetFileSizeEx(h, &size) != FALSE && size.QuadPart >= 0;
            if (ok)
            {
                bytes.resize(static_cast<size_t>(size.QuadPart));
                size_t done = 0;
                while (ok && done < bytes.size())
                {
                    DWORD read = 0;
                    const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - done, size_t{ 1u << 30 }));
                    ok = ::ReadFile(h, bytes.data() + done, chunk, &read, nullptr) != FALSE;
                    if (ok && read == 0)
                    {
                        break; // the file shrank meanwhile (no other writer holds .lock, so this is a damaged volume): keep what came
                    }
                    done += read;
                }
                bytes.resize(done);
            }
            const DWORD code = ok ? 0 : ::GetLastError();
            ::CloseHandle(h);
            if (!ok)
            {
                bytes.clear();
                error = L"the journal could not be read (" + ErrorWords(code) + L")";
                ::CloseHandle(static_cast<HANDLE>(_lock));
                _lock = nullptr;
                return false;
            }
            return true;
        }
        catch (...)
        {
            // std::bad_alloc for a huge journal: the lock goes with the failure (the caller runs without durability).
            ::CloseHandle(static_cast<HANDLE>(_lock));
            _lock = nullptr;
            bytes.clear();
            throw;
        }
    }

    bool OutboxJournal::Rewrite(std::string_view content, std::wstring& error)
    {
        if (!_lock)
        {
            error = L"the outbox is not open";
            return false;
        }
        const std::wstring tmp = _folder + L"\\" + kJournalTmpName;
        bool written = false;
        {
            // The flusher is not flushing (it holds `_flushMtx` for that) and no append runs (`_mtx`): the handles may be closed.
            std::lock_guard flushLock{ _flushMtx };
            std::lock_guard lk{ _mtx };
            _closeHandlesLocked();
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                error = L"the compacted journal could not be created (" + ErrorWords(::GetLastError()) + L")";
            }
            else
            {
                // Flushed BEFORE the rename: the name must never point at a file whose bytes are still only in the cache.
                written = WriteAll(h, content) && ::FlushFileBuffers(h) != FALSE;
                const DWORD code = written ? 0 : ::GetLastError();
                ::CloseHandle(h);
                if (!written)
                {
                    error = L"the compacted journal could not be written (" + ErrorWords(code) + L")";
                    ::DeleteFileW(tmp.c_str());
                }
                else if (!::MoveFileExW(tmp.c_str(), _path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    written = false;
                    error = L"the compacted journal could not replace the old one (" + ErrorWords(::GetLastError()) + L")";
                    ::DeleteFileW(tmp.c_str());
                }
            }
            // Either way the journal path is opened for appends again: the new file, or — after a failure — the old one, so
            // durability goes on (uncompacted) instead of stopping.
            std::wstring openError;
            if (!_openHandlesLocked(openError))
            {
                error += (error.empty() ? L"" : L"; ") + openError;
                written = false;
            }
            else
            {
                if (written)
                {
                    _bytes.store(content.size());
                }
                else
                {
                    LARGE_INTEGER size{};
                    _bytes.store(::GetFileSizeEx(static_cast<HANDLE>(_flush), &size) ? static_cast<uint64_t>(size.QuadPart) : 0);
                }
            }
        }
        _startFlusher();
        return written;
    }

    bool OutboxJournal::Append(std::string_view bytes) noexcept
    {
        if (bytes.empty())
        {
            return true;
        }
        bool ok = false;
        {
            std::lock_guard lk{ _mtx };
            ok = _append != nullptr && WriteAll(static_cast<HANDLE>(_append), bytes);
            if (ok)
            {
                _bytes.fetch_add(bytes.size());
            }
        }
        if (!ok)
        {
            _appendFailures.fetch_add(1);
            return false;
        }
        // Wake the flusher on the first append since its last flush only. `_dirty` is set under `_cvMtx`: set without it, an
        // append between the flusher's check and its wait would be a lost wake-up, and the line would wait for the next one.
        bool wake = false;
        {
            std::lock_guard lk{ _cvMtx };
            wake = !_dirty;
            _dirty = true;
        }
        if (wake)
        {
            _cv.notify_one();
        }
        return true;
    }

    bool OutboxJournal::AppendDead(std::string_view line) noexcept
    {
        try
        {
            if (_folder.empty())
            {
                return false;
            }
            const std::wstring path = _folder + L"\\" + kDeadName;
            HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            const bool ok = WriteAll(h, line) && ::FlushFileBuffers(h) != FALSE;
            ::CloseHandle(h);
            return ok;
        }
        catch (...)
        {
            LogSwallowedException(L"OutboxJournal::AppendDead"); // Rule #18; recovery: the dead letter is only in the log
            return false;
        }
    }

    void OutboxJournal::FlushNow() noexcept
    {
        _flushOnce();
    }

    void OutboxJournal::Close() noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _cvMtx };
                _stopFlusher = true;
            }
            _cv.notify_all();
            if (_flusher.joinable())
            {
                _flusher.join();
            }
            {
                std::lock_guard lk{ _cvMtx };
                _stopFlusher = false; // a later Rewrite (a restart of the bot on this object) may start it again
            }
            // The last lines go to the disk before the handles close (a clean stop is power-safe at once).
            _flushOnce();
            {
                std::lock_guard flushLock{ _flushMtx };
                std::lock_guard lk{ _mtx };
                _closeHandlesLocked();
            }
            if (_lock)
            {
                ::CloseHandle(static_cast<HANDLE>(_lock)); // FILE_FLAG_DELETE_ON_CLOSE: the lock file goes with it
                _lock = nullptr;
            }
        }
        catch (...)
        {
            LogSwallowedException(L"OutboxJournal::Close"); // Rule #18; recovery: a handle may stay open until the process ends
        }
    }

    bool OutboxJournal::IsOpen() const noexcept
    {
        std::lock_guard lk{ _mtx };
        return _append != nullptr;
    }

    uint64_t OutboxJournal::Bytes() const noexcept
    {
        return _bytes.load();
    }

    uint64_t OutboxJournal::AppendFailures() const noexcept
    {
        return _appendFailures.load();
    }

    const std::wstring& OutboxJournal::Folder() const noexcept
    {
        return _folder;
    }

    bool OutboxJournal::_openHandlesLocked(std::wstring& error)
    {
        // Read, write and delete sharing: the flush handle needs the write share, a person or a script may read or tail the
        // journal (Get-Content -Wait), and the next Rewrite renames over it. `.lock` is what keeps every other WRITER out.
        constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE a = ::CreateFileW(_path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, kShare, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (a == INVALID_HANDLE_VALUE)
        {
            error = L"the journal could not be opened for appends (" + ErrorWords(::GetLastError()) + L")";
            return false;
        }
        HANDLE f = ::CreateFileW(_path.c_str(), GENERIC_READ | GENERIC_WRITE, kShare, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            error = L"the journal's flush handle could not be opened (" + ErrorWords(::GetLastError()) + L")";
            ::CloseHandle(a);
            return false;
        }
        _append = a;
        _flush = f;
        return true;
    }

    void OutboxJournal::_closeHandlesLocked() noexcept
    {
        if (_append)
        {
            ::CloseHandle(static_cast<HANDLE>(_append));
            _append = nullptr;
        }
        if (_flush)
        {
            ::CloseHandle(static_cast<HANDLE>(_flush));
            _flush = nullptr;
        }
    }

    void OutboxJournal::_flushOnce() noexcept
    {
        try
        {
            std::lock_guard flushLock{ _flushMtx };
            {
                // Cleared BEFORE the flush: an append during the flush sets it again, so the flusher comes back for it.
                std::lock_guard lk{ _cvMtx };
                if (!_dirty)
                {
                    return;
                }
                _dirty = false;
            }
            HANDLE h = nullptr;
            {
                std::lock_guard lk{ _mtx };
                h = static_cast<HANDLE>(_flush);
            }
            // Outside `_mtx`: appends go on through their own handle meanwhile. The handle cannot close under us: Rewrite and
            // Close take `_flushMtx`, which this flush holds.
            if (h)
            {
                ::FlushFileBuffers(h);
            }
        }
        catch (...)
        {
            LogSwallowedException(L"OutboxJournal::_flushOnce"); // Rule #18; recovery: the next flush writes it
        }
    }

    void OutboxJournal::_flusherLoop() noexcept
    {
        for (;;)
        {
            try
            {
                {
                    std::unique_lock lk{ _cvMtx };
                    _cv.wait(lk, [this] { return _stopFlusher || _dirty; });
                    if (_stopFlusher)
                    {
                        return; // Close flushes once more itself
                    }
                    // Let the appends of the next ~250 ms gather into this flush (at most ~4 flushes a second, off every
                    // queuing thread); a stop ends the wait at once.
                    _cv.wait_for(lk, kFlushGather, [this] { return _stopFlusher; });
                }
                _flushOnce();
            }
            catch (...)
            {
                LogSwallowedException(L"OutboxJournal::_flusherLoop"); // Rule #18; recovery: the next pass flushes
                std::this_thread::sleep_for(kFlushGather);
            }
        }
    }

    void OutboxJournal::_startFlusher()
    {
        if (_flusher.joinable())
        {
            return;
        }
        _flusher = std::thread([this]() noexcept { _flusherLoop(); });
    }
}
