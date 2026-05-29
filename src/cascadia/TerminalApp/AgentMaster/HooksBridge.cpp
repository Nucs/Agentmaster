// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly). The pipe transport is raw Win32.
#include "HooksBridge.h"
#include "HookWire.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace
{
    // Convert a UTF-8 byte span to UTF-16. Returns empty on failure.
    std::wstring Utf8ToUtf16(const char* data, int len)
    {
        if (len <= 0)
        {
            return {};
        }
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, data, len, nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        const int written = ::MultiByteToWideChar(CP_UTF8, 0, data, len, out.data(), needed);
        if (written <= 0)
        {
            return {};
        }
        out.resize(static_cast<size_t>(written));
        return out;
    }
}

namespace Agentmaster
{
    HooksBridge::HooksBridge(std::wstring pipeName, HookSink sink, unsigned instances) :
        _pipeName{ std::move(pipeName) },
        _sink{ std::move(sink) },
        _instances{ instances == 0 ? 1u : instances }
    {
    }

    HooksBridge::~HooksBridge()
    {
        Stop();
    }

    bool HooksBridge::Start()
    {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            return true; // already running
        }

        // Manual-reset stop event so a single Set wakes every worker.
        _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (_stopEvent == nullptr)
        {
            _running.store(false);
            return false;
        }

        _threads.reserve(_instances);
        for (unsigned i = 0; i < _instances; ++i)
        {
            _threads.emplace_back([this]() noexcept { _Worker(); });
        }
        return true;
    }

    void HooksBridge::Stop() noexcept
    {
        if (!_running.exchange(false))
        {
            // Either never started, or already stopped. Still join any threads/handles.
        }

        if (_stopEvent != nullptr)
        {
            ::SetEvent(_stopEvent);
        }

        // Nudge any worker blocked in ConnectNamedPipe: opening + closing a client handle
        // completes the pending ConnectNamedPipe so the worker re-checks _running.
        for (size_t i = 0; i < _threads.size(); ++i)
        {
            const HANDLE h = ::CreateFileW(_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(h);
            }
        }

        for (auto& t : _threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        _threads.clear();

        if (_stopEvent != nullptr)
        {
            ::CloseHandle(_stopEvent);
            _stopEvent = nullptr;
        }
    }

    void HooksBridge::_Worker() noexcept
    {
        constexpr DWORD kBufSize = 64 * 1024;

        while (_running.load())
        {
            const HANDLE pipe = ::CreateNamedPipeW(
                _pipeName.c_str(),
                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                // Byte stream: the forwarder frames each record with a trailing '\n', so
                // we don't need message boundaries and byte mode interoperates with any
                // client write mode (NamedPipeClientStream defaults to byte).
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                0,
                kBufSize,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                // Could not create an instance; back off briefly, then retry / re-check stop.
                if (::WaitForSingleObject(_stopEvent, 250) == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }

            OVERLAPPED ov{};
            ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (ov.hEvent == nullptr)
            {
                ::CloseHandle(pipe);
                break;
            }

            // Wait for a client (a hook invocation) to connect.
            bool connected = false;
            const BOOL ok = ::ConnectNamedPipe(pipe, &ov);
            if (ok)
            {
                connected = true; // unusual, but valid
            }
            else
            {
                const DWORD err = ::GetLastError();
                if (err == ERROR_PIPE_CONNECTED)
                {
                    connected = true; // client connected between CreateNamedPipe and ConnectNamedPipe
                }
                else if (err == ERROR_IO_PENDING)
                {
                    const HANDLE waits[2] = { _stopEvent, ov.hEvent };
                    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (w == WAIT_OBJECT_0)
                    {
                        // Shutting down.
                        ::CancelIoEx(pipe, &ov);
                        ::CloseHandle(ov.hEvent);
                        ::CloseHandle(pipe);
                        break;
                    }
                    DWORD ignored = 0;
                    connected = ::GetOverlappedResult(pipe, &ov, &ignored, FALSE) != FALSE;
                }
            }

            if (connected && _running.load())
            {
                // Read messages until the client disconnects (each forwarder writes one
                // line then closes, but tolerate several).
                std::string accum;
                char buf[kBufSize];
                for (;;)
                {
                    ::ResetEvent(ov.hEvent);
                    DWORD read = 0;
                    const BOOL rok = ::ReadFile(pipe, buf, sizeof(buf), &read, &ov);
                    if (!rok)
                    {
                        const DWORD err = ::GetLastError();
                        if (err == ERROR_IO_PENDING)
                        {
                            const HANDLE waits[2] = { _stopEvent, ov.hEvent };
                            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                            if (w == WAIT_OBJECT_0)
                            {
                                ::CancelIoEx(pipe, &ov);
                                break;
                            }
                            if (!::GetOverlappedResult(pipe, &ov, &read, FALSE))
                            {
                                break; // broken pipe / disconnect
                            }
                        }
                        else if (err == ERROR_MORE_DATA)
                        {
                            // Message larger than buffer: take what we have and keep reading.
                            accum.append(buf, read);
                            continue;
                        }
                        else
                        {
                            break; // ERROR_BROKEN_PIPE etc.
                        }
                    }

                    if (read == 0)
                    {
                        break;
                    }
                    accum.append(buf, read);

                    // Dispatch any complete lines we have accumulated.
                    size_t nl;
                    while ((nl = accum.find('\n')) != std::string::npos)
                    {
                        const std::wstring wline = Utf8ToUtf16(accum.data(), static_cast<int>(nl));
                        accum.erase(0, nl + 1);
                        if (const auto msg = ParseWireLine(wline); msg && _sink)
                        {
                            try
                            {
                                _sink(*msg);
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                }

                // Flush a final line without a trailing newline (message-mode writes often
                // arrive without one).
                if (!accum.empty())
                {
                    const std::wstring wline = Utf8ToUtf16(accum.data(), static_cast<int>(accum.size()));
                    if (const auto msg = ParseWireLine(wline); msg && _sink)
                    {
                        try
                        {
                            _sink(*msg);
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }

            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(ov.hEvent);
            ::CloseHandle(pipe);
        }
    }
}
