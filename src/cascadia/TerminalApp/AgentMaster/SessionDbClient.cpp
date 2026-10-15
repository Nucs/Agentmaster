// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's client (see SessionDbClient.h).

#include "SessionDbClient.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ClaudeSpawn.h" // LogSwallowedException (every binary that links the client links the engine's logger)
#include "SessionDbProtocol.h"

namespace Agentmaster::SessionDb
{
    namespace Proto = SessionDbProtocol;

    namespace
    {
        bool FileExists(const std::wstring& path)
        {
            const DWORD a = ::GetFileAttributesW(path.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        std::wstring DirOf(const std::wstring& path)
        {
            const size_t cut = path.find_last_of(L"\\/");
            return cut == std::wstring::npos ? std::wstring{} : path.substr(0, cut);
        }

        std::wstring ModulePath(HMODULE module)
        {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD n = ::GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0)
                {
                    return {};
                }
                if (n < buf.size())
                {
                    buf.resize(n);
                    return buf;
                }
                buf.resize(buf.size() * 2);
            }
        }

        // The integer value of a top-level `"key":<digits>` in a compact JSON answer (0 when absent). The
        // service writes its answers with nlohmann's compact dump, so the shape is fixed.
        int64_t IntField(std::string_view json, std::string_view key)
        {
            const std::string needle = "\"" + std::string{ key } + "\":";
            const size_t at = json.find(needle);
            if (at == std::string_view::npos)
            {
                return 0;
            }
            int64_t v = 0;
            for (size_t i = at + needle.size(); i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i)
            {
                v = v * 10 + (json[i] - '0');
            }
            return v;
        }

        // Whether an answer envelope belongs to request `id`: nlohmann sorts the keys, so the id is the first
        // key of {"id":N,"result":...} or the last of {"error":{...},"id":N}.
        bool IsAnswerTo(std::string_view json, uint64_t id)
        {
            const std::string n = std::to_string(id);
            const std::string head = "{\"id\":" + n + ",";
            const std::string tail = "\"id\":" + n + "}";
            return json.substr(0, head.size()) == head || (json.size() >= tail.size() && json.substr(json.size() - tail.size()) == tail);
        }

        // A kernel handle that closes at scope exit.
        struct Owned
        {
            HANDLE h{ nullptr };
            ~Owned()
            {
                if (h && h != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(h);
                }
            }
        };
    }

    ServiceClient::ServiceClient(std::wstring profileDir) :
        _profile(std::move(profileDir))
    {
    }

    ServiceClient::~ServiceClient()
    {
        Close();
    }

    void ServiceClient::Close() noexcept
    {
        std::lock_guard lk{ _writeMtx };
        if (_pipe)
        {
            ::CloseHandle(static_cast<HANDLE>(_pipe));
            _pipe = nullptr;
        }
        if (_readEvent)
        {
            ::CloseHandle(static_cast<HANDLE>(_readEvent));
            _readEvent = nullptr;
        }
        if (_writeEvent)
        {
            ::CloseHandle(static_cast<HANDLE>(_writeEvent));
            _writeEvent = nullptr;
        }
    }

    bool ServiceClient::_open(std::wstring& error)
    {
        const std::wstring name = Proto::PipeName(_profile);
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            HANDLE h = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::lock_guard lk{ _writeMtx };
                _pipe = h;
                _readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                _writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                return true;
            }
            const DWORD e = ::GetLastError();
            if (e == ERROR_PIPE_BUSY)
            {
                // Every instance is taken this instant; the service creates the next one at once.
                ::WaitNamedPipeW(name.c_str(), 500);
                continue;
            }
            error = e == ERROR_FILE_NOT_FOUND ? L"No session database service runs for this profile." : L"The session database pipe did not open (error " + std::to_wstring(e) + L").";
            return false;
        }
        error = L"The session database service is busy.";
        return false;
    }

    std::wstring ServiceClient::FindServiceExe(const std::wstring& profileDir)
    {
        const std::wstring leaf{ Proto::kServiceExeLeaf };
        const std::wstring copied = Proto::BinDir(profileDir) + L"\\" + leaf;
        if (FileExists(copied))
        {
            return copied;
        }
        // Beside the calling process (the CLI, the MCP server, or an unpackaged GUI)...
        const std::wstring besideExe = DirOf(ModulePath(nullptr)) + L"\\" + leaf;
        if (FileExists(besideExe))
        {
            return besideExe;
        }
        // ...or beside the module that holds this code (TerminalApp.dll in the package folder).
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&ServiceClient::FindServiceExe), &self))
        {
            const std::wstring besideModule = DirOf(ModulePath(self)) + L"\\" + leaf;
            if (FileExists(besideModule))
            {
                return besideModule;
            }
        }
        return {};
    }

    bool ServiceClient::LaunchService(const std::wstring& profileDir, std::wstring& error)
    {
        const std::wstring exe = FindServiceExe(profileDir);
        if (exe.empty())
        {
            error = L"The session database service (agentmaster-sessiondb.exe) is not installed.";
            return false;
        }
        std::wstring cmd = L"\"" + exe + L"\" serve --profile \"" + profileDir + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const std::wstring dir = DirOf(exe);
        // No window, its own process group (a Ctrl+C in the caller's console never reaches it), no inherited
        // handles (an MCP server's stdio pipes must close when it exits), and outside the caller's job when the
        // job allows breakaway (claude.exe's job must not end the service with the MCP server).
        DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
        BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si, &pi);
        if (!ok && ::GetLastError() == ERROR_ACCESS_DENIED)
        {
            flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB); // the job forbids breakaway: start inside it
            ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si, &pi);
        }
        if (!ok)
        {
            error = L"The session database service did not start (error " + std::to_wstring(::GetLastError()) + L").";
            return false;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return true;
    }

    bool ServiceClient::Connect(bool start, uint32_t waitMs, std::wstring& error)
    {
        Close();
        if (_open(error))
        {
            return _helloCheck(start, waitMs, error);
        }
        if (!start)
        {
            return false;
        }
        // Start one, under the profile's start mutex: a second client that finds no service at the same moment
        // waits here, then finds the first one's pipe.
        Owned startMutex{ ::CreateMutexW(nullptr, FALSE, Proto::StartMutexName(_profile).c_str()) };
        if (startMutex.h)
        {
            ::WaitForSingleObject(startMutex.h, std::max<DWORD>(waitMs, 5000));
        }
        std::wstring ignored;
        if (!_open(ignored))
        {
            if (!LaunchService(_profile, error))
            {
                if (startMutex.h)
                {
                    ::ReleaseMutex(startMutex.h);
                }
                return false;
            }
            const ULONGLONG until = ::GetTickCount64() + waitMs;
            bool opened = false;
            while (::GetTickCount64() < until)
            {
                if (_open(ignored))
                {
                    opened = true;
                    break;
                }
                ::Sleep(100);
            }
            if (!opened)
            {
                if (startMutex.h)
                {
                    ::ReleaseMutex(startMutex.h);
                }
                error = L"The session database service started but did not answer within " + std::to_wstring(waitMs / 1000) + L" s.";
                return false;
            }
        }
        if (startMutex.h)
        {
            ::ReleaseMutex(startMutex.h);
        }
        return _helloCheck(false, waitMs, error);
    }

    bool ServiceClient::_helloCheck(bool start, uint32_t waitMs, std::wstring& error)
    {
        std::string answer;
        if (!Call(Proto::kHello, "{}", answer, 5000, error))
        {
            return false;
        }
        const int64_t build = IntField(answer, "service");
        if (build >= Proto::kServiceBuild || !start)
        {
            return true; // the same or a newer service: use it (an older client never replaces a newer service)
        }
        // An OLDER service: ask it to stop, wait for its pipe to go, start ours (§4.2 "Versions").
        std::string ignored;
        std::wstring err2;
        Call(Proto::kShutdown, "{}", ignored, 5000, err2);
        Close();
        const std::wstring name = Proto::PipeName(_profile);
        for (int i = 0; i < 100; ++i)
        {
            if (!::WaitNamedPipeW(name.c_str(), 0) && ::GetLastError() == ERROR_FILE_NOT_FOUND)
            {
                break;
            }
            ::Sleep(100);
        }
        return Connect(true, waitMs, error);
    }

    bool ServiceClient::_write(std::string_view frame, std::wstring& error)
    {
        std::lock_guard lk{ _writeMtx };
        if (!_pipe)
        {
            error = L"Not connected to the session database service.";
            return false;
        }
        const HANDLE pipe = static_cast<HANDLE>(_pipe);
        const HANDLE ev = static_cast<HANDLE>(_writeEvent);
        size_t done = 0;
        while (done < frame.size())
        {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            DWORD w = 0;
            if (!::WriteFile(pipe, frame.data() + done, static_cast<DWORD>(frame.size() - done), &w, &ov))
            {
                if (::GetLastError() != ERROR_IO_PENDING || ::WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0 || !::GetOverlappedResult(pipe, &ov, &w, FALSE))
                {
                    ::CancelIoEx(pipe, &ov);
                    error = L"The request could not be sent to the session database service.";
                    return false;
                }
            }
            done += w;
        }
        return true;
    }

    bool ServiceClient::_readExact(char* buf, uint32_t n, uint32_t timeoutMs, std::wstring& error)
    {
        const HANDLE pipe = static_cast<HANDLE>(_pipe);
        const HANDLE ev = static_cast<HANDLE>(_readEvent);
        const ULONGLONG until = ::GetTickCount64() + timeoutMs;
        uint32_t got = 0;
        while (got < n)
        {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            DWORD r = 0;
            if (!::ReadFile(pipe, buf + got, n - got, &r, &ov))
            {
                if (::GetLastError() != ERROR_IO_PENDING)
                {
                    error = L"The session database service closed the connection.";
                    return false;
                }
                const ULONGLONG now = ::GetTickCount64();
                const DWORD left = now >= until ? 0 : static_cast<DWORD>(until - now);
                if (::WaitForSingleObject(ev, left) != WAIT_OBJECT_0)
                {
                    ::CancelIoEx(pipe, &ov);
                    ::GetOverlappedResult(pipe, &ov, &r, TRUE);
                    error = L"The session database service did not answer within " + std::to_wstring(timeoutMs / 1000) + L" s.";
                    return false;
                }
                if (!::GetOverlappedResult(pipe, &ov, &r, FALSE))
                {
                    error = L"The session database service closed the connection.";
                    return false;
                }
            }
            if (r == 0)
            {
                error = L"The session database service closed the connection.";
                return false;
            }
            got += r;
        }
        return true;
    }

    bool ServiceClient::Call(std::string_view method, std::string_view paramsJson, std::string& response, uint32_t timeoutMs, std::wstring& error)
    {
        if (!_pipe)
        {
            error = L"Not connected to the session database service.";
            return false;
        }
        const uint64_t id = _nextId++;
        std::string req = "{\"v\":" + std::to_string(Proto::kProtocolVersion) + ",\"id\":" + std::to_string(id) + ",\"method\":\"" + std::string{ method } +
                          "\",\"params\":" + std::string{ paramsJson.empty() ? std::string_view{ "{}" } : paramsJson } + "}";
        if (req.size() > Proto::kMaxRequestBytes)
        {
            error = L"The request is larger than the service accepts (1 MB).";
            return false;
        }
        _pendingId.store(id);
        if (!_write(Proto::EncodeFrame(req), error))
        {
            _pendingId.store(0);
            Close();
            return false;
        }
        const ULONGLONG until = ::GetTickCount64() + timeoutMs;
        for (;;)
        {
            const ULONGLONG now = ::GetTickCount64();
            const uint32_t left = now >= until ? 0 : static_cast<uint32_t>(until - now);
            unsigned char header[4];
            if (!_readExact(reinterpret_cast<char*>(header), 4, left, error))
            {
                _pendingId.store(0);
                Close(); // a late answer would be read as the next request's
                return false;
            }
            const uint32_t n = Proto::DecodeFrameLength(header);
            if (n > Proto::kMaxResponseBytes + 1024)
            {
                _pendingId.store(0);
                Close();
                error = L"The session database service sent an oversized answer.";
                return false;
            }
            response.assign(n, '\0');
            if (n && !_readExact(response.data(), n, std::max<uint32_t>(left, 5000), error))
            {
                _pendingId.store(0);
                Close();
                return false;
            }
            if (IsAnswerTo(response, id))
            {
                _pendingId.store(0);
                return true;
            }
            // An answer to an earlier, abandoned request: skip it.
        }
    }

    void ServiceClient::CancelPending() noexcept
    {
        const uint64_t id = _pendingId.load();
        if (id == 0)
        {
            return;
        }
        try
        {
            std::wstring ignored;
            const std::string req = "{\"v\":" + std::to_string(Proto::kProtocolVersion) + ",\"id\":0,\"method\":\"cancel\",\"params\":{\"id\":" + std::to_string(id) + "}}";
            _write(Proto::EncodeFrame(req), ignored);
        }
        catch (...)
        {
            // Best effort: a failed cancel only means the request runs to its deadline.
            LogSwallowedException(L"SessionDb CancelPending");
        }
    }

    bool EnsureServiceBinaries(const std::wstring& profileDir, const std::wstring& sourceDir, std::wstring& error)
    {
        try
        {
            const std::wstring bin = Proto::BinDir(profileDir);
            ::CreateDirectoryW(Proto::ServiceDir(profileDir).c_str(), nullptr);
            ::CreateDirectoryW(bin.c_str(), nullptr);
            // Old renamed copies first: deleting one that still runs fails, and that is fine.
            {
                WIN32_FIND_DATAW fd{};
                const HANDLE find = ::FindFirstFileW((bin + L"\\*.old-*").c_str(), &fd);
                if (find != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        ::DeleteFileW((bin + L"\\" + fd.cFileName).c_str());
                    } while (::FindNextFileW(find, &fd));
                    ::FindClose(find);
                }
            }
            for (const std::wstring_view leaf : { Proto::kDuckDbDllLeaf, Proto::kServiceExeLeaf })
            {
                const std::wstring src = sourceDir + L"\\" + std::wstring{ leaf };
                const std::wstring dst = bin + L"\\" + std::wstring{ leaf };
                WIN32_FILE_ATTRIBUTE_DATA s{}, d{};
                if (!::GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &s))
                {
                    error = L"The package has no " + std::wstring{ leaf } + L".";
                    return false;
                }
                if (::GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &d) && s.nFileSizeHigh == d.nFileSizeHigh && s.nFileSizeLow == d.nFileSizeLow &&
                    ::CompareFileTime(&s.ftLastWriteTime, &d.ftLastWriteTime) == 0)
                {
                    continue; // already the same copy
                }
                const std::wstring tmp = dst + L".new";
                if (!::CopyFileW(src.c_str(), tmp.c_str(), FALSE))
                {
                    error = L"Copying " + std::wstring{ leaf } + L" failed (error " + std::to_wstring(::GetLastError()) + L").";
                    return false;
                }
                if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
                {
                    // The stable name is in use (a running service): rename it aside, then take the name.
                    const std::wstring aside = dst + L".old-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
                    if (!::MoveFileExW(dst.c_str(), aside.c_str(), 0) || !::MoveFileExW(tmp.c_str(), dst.c_str(), 0))
                    {
                        error = L"Replacing " + std::wstring{ leaf } + L" failed (error " + std::to_wstring(::GetLastError()) + L").";
                        ::DeleteFileW(tmp.c_str());
                        return false;
                    }
                }
            }
            return true;
        }
        catch (const std::exception&)
        {
            LogSwallowedException(L"SessionDb EnsureServiceBinaries");
            error = L"Copying the session database service failed (out of memory).";
            return false;
        }
    }
}
