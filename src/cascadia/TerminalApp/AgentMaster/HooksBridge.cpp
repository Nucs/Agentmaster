// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly). The pipe transport is raw Win32.
#include "HooksBridge.h"
#include "AskQuestion.h" // kAskRequestMaxBytes (the ask pipe's request bound)
#include "ClaudeSpawn.h" // LogSwallowedException (the never-lose-a-swallowed-exception policy), AppendStateLog, ShortId
#include "HookWire.h"
#include "Json.h" // the ask request's session id

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace
{
    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

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
                        if (auto msg = ParseWireLine(wline); msg && _sink)
                        {
                            // Agentmaster (the ea1dc3dd double-send, 2026-08-15): a truncated/garbled
                            // record parses with ts==0 — and ts==0 DISABLES both of the ordered state
                            // machine's defenses (the stale check + the kMinRealTurnSpanMs floor,
                            // HookEvents.h: both are gated on `ts != 0`), which is exactly how a
                            // DUPLICATE Stop delivery that lost its trailing ts field read as a fresh
                            // 37ms turn-complete and double-sent the queue into a running turn. Every
                            // CURRENT forwarder stamps ts unconditionally (FIRST, before any slow
                            // work), so a missing ts here means a mangled delivery, not an old
                            // forwarder — stamp the ARRIVAL time instead. Arrival can only OVERSTATE
                            // the fire time, which errs toward reading a suspect event as stale /
                            // too-fast — the self-healing direction (a genuinely-completed turn is
                            // settled by the scanner's exempt quiescent Stop ~2.5s later).
                            if (msg->ts == 0)
                            {
                                msg->ts = NowMs();
                            }
                            try
                            {
                                _sink(*msg);
                            }
                            catch (...)
                            {
                                // Forensics: the hook-event sink (bridge -> registry -> state
                                // machine). A throw DROPS that hook entirely, so the push state
                                // machine desyncs — a lost Stop strands a session Running until the
                                // scanner's missed-Stop backstop happens to reconcile it.
                                LogSwallowedException(L"HooksBridge sink (framed line)");
                            }
                        }
                    }
                }

                // Flush a final line without a trailing newline (message-mode writes often
                // arrive without one).
                if (!accum.empty())
                {
                    const std::wstring wline = Utf8ToUtf16(accum.data(), static_cast<int>(accum.size()));
                    if (auto msg = ParseWireLine(wline); msg && _sink)
                    {
                        // Arrival-stamp a ts-less parse (see the framed-line dispatch above). This
                        // trailing-flush path parses a record with NO terminating '\n' — for our
                        // forwarder (which ALWAYS appends one) that is BY CONSTRUCTION a truncated
                        // fragment of a dying client, i.e. precisely the delivery most likely to
                        // have lost its trailing ts field.
                        if (msg->ts == 0)
                        {
                            msg->ts = NowMs();
                        }
                        try
                        {
                            _sink(*msg);
                        }
                        catch (...)
                        {
                            // Forensics: same sink, the trailing unterminated record (a client that
                            // closed without a final '\n'). Same desync cost as the framed path.
                            LogSwallowedException(L"HooksBridge sink (trailing line)");
                        }
                    }
                }
            }

            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(ov.hEvent);
            ::CloseHandle(pipe);
        }
    }

    // ===== the ASK pipe (HooksBridge.h; INTERACTIVE_TOOLS.md §6) =====================================================

    namespace
    {
        /// <summary>How an overlapped pipe operation ended (AskWaitIo).</summary>
        enum class AskIo
        {
            Done,
            Failed, // the pipe broke (the helper went away) or the call failed
            Stopped, // the bridge's stop event
            TimedOut,
        };

        /// <summary>
        /// Waits for one overlapped operation on the pipe, the stop event, or the timeout. Every way out but Done CANCELS the
        /// operation and waits for the cancellation, so the OVERLAPPED (and its event) may be reused or closed right after.
        /// </summary>
        /// <param name="pipe">The pipe.</param>
        /// <param name="ov">The operation's OVERLAPPED (its event is the one waited on).</param>
        /// <param name="stopEvent">The bridge's stop event.</param>
        /// <param name="timeoutMs">The bound (INFINITE allowed).</param>
        /// <param name="transferred">Out: the bytes moved.</param>
        /// <returns>How it ended.</returns>
        AskIo AskWaitIo(HANDLE pipe, OVERLAPPED& ov, HANDLE stopEvent, DWORD timeoutMs, DWORD& transferred) noexcept
        {
            const HANDLE waits[2] = { stopEvent, ov.hEvent };
            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
            if (w == WAIT_OBJECT_0 + 1)
            {
                return ::GetOverlappedResult(pipe, &ov, &transferred, FALSE) ? AskIo::Done : AskIo::Failed;
            }
            // Stop, timeout or a failed wait: the operation must not outlive this frame's OVERLAPPED.
            ::CancelIoEx(pipe, &ov);
            ::GetOverlappedResult(pipe, &ov, &transferred, TRUE);
            if (w == WAIT_OBJECT_0)
            {
                return AskIo::Stopped;
            }
            return w == WAIT_TIMEOUT ? AskIo::TimedOut : AskIo::Failed;
        }

        /// <summary>Writes every byte to the pipe, within `timeoutMs` in all; the stop event ends it early.</summary>
        /// <param name="pipe">The pipe (overlapped).</param>
        /// <param name="bytes">What to write.</param>
        /// <param name="stopEvent">The bridge's stop event.</param>
        /// <param name="timeoutMs">The bound for the whole write.</param>
        /// <returns>True when every byte went.</returns>
        bool AskWriteAll(HANDLE pipe, const std::string& bytes, HANDLE stopEvent, DWORD timeoutMs) noexcept
        {
            OVERLAPPED ov{};
            ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (ov.hEvent == nullptr)
            {
                return false;
            }
            const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
            size_t off = 0;
            bool ok = true;
            while (off < bytes.size())
            {
                ::ResetEvent(ov.hEvent);
                DWORD written = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - off, 64 * 1024));
                if (::WriteFile(pipe, bytes.data() + off, chunk, nullptr, &ov))
                {
                    // Completed at once: the count is in the OVERLAPPED (an overlapped handle's direct count is not reliable).
                    ok = ::GetOverlappedResult(pipe, &ov, &written, FALSE) != FALSE;
                }
                else if (::GetLastError() == ERROR_IO_PENDING)
                {
                    const ULONGLONG now = ::GetTickCount64();
                    ok = AskWaitIo(pipe, ov, stopEvent, now >= deadline ? 0 : static_cast<DWORD>(deadline - now), written) == AskIo::Done;
                }
                else
                {
                    ok = false;
                }
                if (!ok || written == 0)
                {
                    ok = false;
                    break;
                }
                off += written;
            }
            ::CloseHandle(ov.hEvent);
            return ok;
        }

        /// <summary>
        /// Reads the helper's request line: bytes up to the first '\n' (excluded), at most `maxBytes`, within `timeoutMs`. A helper
        /// that sends more than one line, or nothing, or too much, is not a valid request.
        /// </summary>
        /// <param name="pipe">The pipe.</param>
        /// <param name="stopEvent">The bridge's stop event.</param>
        /// <param name="maxBytes">The bound.</param>
        /// <param name="timeoutMs">The bound in time.</param>
        /// <param name="line">Out: the line's bytes (UTF-8).</param>
        /// <returns>True when a whole line arrived in time.</returns>
        bool AskReadLine(HANDLE pipe, HANDLE stopEvent, size_t maxBytes, DWORD timeoutMs, std::string& line) noexcept
        {
            OVERLAPPED ov{};
            ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (ov.hEvent == nullptr)
            {
                return false;
            }
            const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
            bool ok = false;
            try
            {
                char buf[16 * 1024];
                for (;;)
                {
                    ::ResetEvent(ov.hEvent);
                    DWORD got = 0;
                    if (::ReadFile(pipe, buf, sizeof(buf), nullptr, &ov))
                    {
                        if (!::GetOverlappedResult(pipe, &ov, &got, FALSE))
                        {
                            break;
                        }
                    }
                    else
                    {
                        const DWORD err = ::GetLastError();
                        if (err != ERROR_IO_PENDING && err != ERROR_MORE_DATA)
                        {
                            break; // broken pipe: the helper went away before its line was whole
                        }
                        if (err == ERROR_IO_PENDING)
                        {
                            const ULONGLONG now = ::GetTickCount64();
                            if (AskWaitIo(pipe, ov, stopEvent, now >= deadline ? 0 : static_cast<DWORD>(deadline - now), got) != AskIo::Done)
                            {
                                break;
                            }
                        }
                    }
                    if (got == 0)
                    {
                        break;
                    }
                    line.append(buf, got);
                    if (const size_t nl = line.find('\n'); nl != std::string::npos)
                    {
                        line.resize(nl);
                        ok = true;
                        break;
                    }
                    if (line.size() > maxBytes)
                    {
                        break; // too long: refused, never buffered without bound
                    }
                }
            }
            catch (...)
            {
                // Forensics: a request line that could not be buffered (out of memory) — the helper hears nothing and its dialog
                // stays with the PC; the swallow must not end the worker silently.
                LogSwallowedException(L"AskBridge request read");
                ok = false;
            }
            ::CloseHandle(ov.hEvent);
            return ok;
        }
    }

    void AskRegistry::SetAcceptor(Acceptor accept)
    {
        std::lock_guard lk{ _mtx };
        _accept = std::move(accept);
    }

    void AskRegistry::SetListener(std::function<void()> listener)
    {
        std::lock_guard lk{ _mtx };
        _listener = std::move(listener);
    }

    void AskRegistry::_changed() const
    {
        std::function<void()> listener;
        {
            std::lock_guard lk{ _mtx };
            listener = _listener;
        }
        if (!listener)
        {
            return;
        }
        try
        {
            listener();
        }
        catch (...)
        {
            // Forensics: the service's wake-up threw; the change is recorded, and the service's next periodic tick still sees it.
            LogSwallowedException(L"AskRegistry listener");
        }
    }

    uint64_t AskRegistry::Open(const std::wstring& sessionId, std::wstring requestLine, void* wakeEvent)
    {
        Acceptor accept;
        {
            std::lock_guard lk{ _mtx };
            accept = _accept;
        }
        bool accepted = false;
        if (accept)
        {
            try
            {
                accepted = accept(sessionId);
            }
            catch (...)
            {
                // Forensics: an acceptor that throws refuses — the helper hears "{}" and the PC answers as before.
                LogSwallowedException(L"AskRegistry acceptor");
                accepted = false;
            }
        }
        if (!accepted)
        {
            return 0;
        }
        uint64_t id = 0;
        {
            std::lock_guard lk{ _mtx };
            id = _next++;
            Entry e;
            e.info.id = id;
            e.info.sessionId = sessionId;
            e.info.requestLine = std::move(requestLine);
            e.info.openedMs = NowMs();
            e.info.changedMs = e.info.openedMs;
            e.wake = wakeEvent;
            _entries.emplace(id, std::move(e));
        }
        _changed();
        return id;
    }

    std::optional<std::string> AskRegistry::TakeReply(uint64_t id)
    {
        std::lock_guard lk{ _mtx };
        const auto it = _entries.find(id);
        if (it == _entries.end() || !it->second.reply)
        {
            return std::nullopt;
        }
        std::optional<std::string> r = std::move(it->second.reply);
        it->second.reply.reset();
        return r;
    }

    void AskRegistry::NoteWritten(uint64_t id, bool ok)
    {
        {
            std::lock_guard lk{ _mtx };
            const auto it = _entries.find(id);
            if (it == _entries.end())
            {
                return;
            }
            Entry& e = it->second;
            e.wake = nullptr; // the worker closes its event next: never signal it again
            if (ok)
            {
                e.info.state = e.replyIsDecision ? AskEntryState::Delivered : AskEntryState::Released;
            }
            else
            {
                e.info.state = AskEntryState::Gone;
                e.info.why = L"the reply could not be written to the hook";
            }
            e.info.changedMs = NowMs();
        }
        _changed();
    }

    void AskRegistry::NoteGone(uint64_t id, std::wstring why)
    {
        {
            std::lock_guard lk{ _mtx };
            const auto it = _entries.find(id);
            if (it == _entries.end())
            {
                return;
            }
            Entry& e = it->second;
            e.wake = nullptr;
            if (e.info.state == AskEntryState::Open || e.info.state == AskEntryState::Answered)
            {
                e.info.state = AskEntryState::Gone;
                e.info.why = std::move(why);
                e.info.changedMs = NowMs();
            }
        }
        _changed();
    }

    std::vector<AskEntryInfo> AskRegistry::Snapshot() const
    {
        std::lock_guard lk{ _mtx };
        std::vector<AskEntryInfo> out;
        out.reserve(_entries.size());
        for (const auto& [id, e] : _entries)
        {
            out.push_back(e.info);
        }
        return out;
    }

    bool AskRegistry::Answer(uint64_t id, std::string decisionLine)
    {
        {
            std::lock_guard lk{ _mtx };
            const auto it = _entries.find(id);
            if (it == _entries.end() || it->second.info.state != AskEntryState::Open)
            {
                return false;
            }
            Entry& e = it->second;
            e.reply = std::move(decisionLine);
            e.replyIsDecision = true;
            e.info.state = AskEntryState::Answered;
            e.info.changedMs = NowMs();
            // Under the lock: the worker forgets the handle (NoteWritten / NoteGone) under this same lock before it closes it.
            if (e.wake != nullptr)
            {
                ::SetEvent(e.wake);
            }
        }
        _changed();
        return true;
    }

    bool AskRegistry::Release(uint64_t id, std::wstring why)
    {
        {
            std::lock_guard lk{ _mtx };
            const auto it = _entries.find(id);
            if (it == _entries.end() || it->second.info.state != AskEntryState::Open)
            {
                return false;
            }
            Entry& e = it->second;
            e.reply = std::string{ "{}" };
            e.replyIsDecision = false;
            e.info.state = AskEntryState::Answered;
            e.info.why = std::move(why);
            e.info.changedMs = NowMs();
            if (e.wake != nullptr)
            {
                ::SetEvent(e.wake);
            }
        }
        _changed();
        return true;
    }

    void AskRegistry::Forget(uint64_t id)
    {
        std::lock_guard lk{ _mtx };
        const auto it = _entries.find(id);
        if (it != _entries.end() && it->second.info.state != AskEntryState::Open && it->second.info.state != AskEntryState::Answered)
        {
            _entries.erase(it);
        }
    }

    AskBridge::AskBridge(std::wstring pipeName, std::shared_ptr<AskRegistry> registry) :
        _pipeName{ std::move(pipeName) },
        _registry{ std::move(registry) }
    {
    }

    AskBridge::~AskBridge()
    {
        Stop();
    }

    bool AskBridge::Start()
    {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            return true;
        }
        _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (_stopEvent == nullptr || !_registry)
        {
            _running.store(false);
            return false;
        }
        try
        {
            _listener = std::thread([this]() noexcept { _Listen(); });
        }
        catch (...)
        {
            // Forensics: no thread for the listener — helpers then find no pipe and leave every dialog to the PC.
            LogSwallowedException(L"AskBridge::Start");
            _running.store(false);
            return false;
        }
        AppendStateLog(L"hooks.log", L"[ask] question pipe listening on " + _pipeName + L"\n");
        return true;
    }

    void AskBridge::Stop() noexcept
    {
        _running.store(false);
        if (_stopEvent != nullptr)
        {
            ::SetEvent(_stopEvent);
        }
        if (_listener.joinable())
        {
            _listener.join();
        }
        std::vector<Worker> workers;
        {
            std::lock_guard lk{ _workersMtx };
            workers.swap(_workers);
        }
        for (auto& w : workers)
        {
            if (w.thread.joinable())
            {
                w.thread.join();
            }
        }
        if (_stopEvent != nullptr)
        {
            ::CloseHandle(_stopEvent);
            _stopEvent = nullptr;
        }
    }

    void AskBridge::_ReapWorkers()
    {
        std::vector<Worker> finished;
        {
            std::lock_guard lk{ _workersMtx };
            for (auto it = _workers.begin(); it != _workers.end();)
            {
                if (it->done && it->done->load())
                {
                    finished.push_back(std::move(*it));
                    it = _workers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (auto& w : finished)
        {
            if (w.thread.joinable())
            {
                w.thread.join(); // done: returns at once
            }
        }
    }

    void AskBridge::_Listen() noexcept
    {
        while (_running.load())
        {
            try
            {
                _ReapWorkers();
            }
            catch (...)
            {
                // Forensics: reaping finished workers failed (a join that threw); they are joined at Stop instead.
                LogSwallowedException(L"AskBridge reap");
            }
            const HANDLE pipe = ::CreateNamedPipeW(_pipeName.c_str(),
                                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                   PIPE_UNLIMITED_INSTANCES,
                                                   64 * 1024,
                                                   64 * 1024,
                                                   0,
                                                   nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
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
            bool connected = false;
            if (::ConnectNamedPipe(pipe, &ov))
            {
                connected = true;
            }
            else
            {
                const DWORD err = ::GetLastError();
                if (err == ERROR_PIPE_CONNECTED)
                {
                    connected = true;
                }
                else if (err == ERROR_IO_PENDING)
                {
                    DWORD ignored = 0;
                    const AskIo r = AskWaitIo(pipe, ov, _stopEvent, INFINITE, ignored);
                    connected = r == AskIo::Done;
                    if (r == AskIo::Stopped)
                    {
                        ::CloseHandle(ov.hEvent);
                        ::CloseHandle(pipe);
                        break;
                    }
                }
            }
            ::CloseHandle(ov.hEvent);
            if (!connected || !_running.load())
            {
                ::CloseHandle(pipe);
                continue;
            }
            try
            {
                auto done = std::make_shared<std::atomic<bool>>(false);
                std::thread t([this, pipe, done]() noexcept {
                    _Serve(pipe);
                    done->store(true);
                });
                std::lock_guard lk{ _workersMtx };
                _workers.push_back(Worker{ std::move(t), std::move(done) });
            }
            catch (...)
            {
                // Forensics: no worker thread for a connected helper — it is disconnected, finds a broken pipe and leaves the
                // dialog to the PC.
                LogSwallowedException(L"AskBridge worker start");
                ::DisconnectNamedPipe(pipe);
                ::CloseHandle(pipe);
            }
        }
    }

    void AskBridge::_Serve(void* pipeHandle) noexcept
    {
        const HANDLE pipe = static_cast<HANDLE>(pipeHandle);
        const HANDLE wake = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        uint64_t id = 0;
        std::wstring sid8;
        try
        {
            std::string raw;
            if (wake == nullptr || !AskReadLine(pipe, _stopEvent, Msg::kAskRequestMaxBytes, 10000, raw))
            {
                AppendStateLog(L"hooks.log", L"[ask] a question hook's request could not be read (no whole line within 10 s / 1 MiB); its dialog stays with the PC\n");
            }
            else
            {
                const std::wstring line = Utf8ToUtf16(raw.data(), static_cast<int>(raw.size()));
                // The session, and the tool and subagent marks once more: the helper filters both, and the engine does not trust it.
                std::wstring sid;
                bool askTool = false;
                bool subagent = false;
                if (const auto root = json::Parse(line); root && root->type == json::Value::Type::Obj)
                {
                    if (const auto* payload = root->Find(L"payload"); payload && payload->type == json::Value::Type::Obj)
                    {
                        sid = payload->StrAt(L"session_id");
                        askTool = payload->StrAt(L"tool_name") == L"AskUserQuestion";
                        subagent = !payload->StrAt(L"agent_id").empty();
                    }
                }
                sid8 = ShortId(sid);
                if (!sid.empty() && askTool && !subagent)
                {
                    id = _registry->Open(sid, line, wake);
                }
                if (id == 0)
                {
                    // Nothing can show it (the bot is off, the session unknown, a malformed request): the dialog stays with the PC.
                    AskWriteAll(pipe, std::string{ "{}\n" }, _stopEvent, 5000);
                    AppendStateLog(L"hooks.log", L"[ask] " + (sid8.empty() ? std::wstring{ L"?" } : sid8) + L" question not taken here (the Telegram bot is off, or the session is not one of ours); its dialog stays with the PC\n");
                }
                else
                {
                    AppendStateLog(L"hooks.log", L"[ask] " + sid8 + L" question open (request " + std::to_wstring(id) + L"); answerable from Telegram\n");
                    // A one-byte read that completes only when the helper goes away (it never writes a second line): the pipe breaks.
                    OVERLAPPED rov{};
                    rov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    char probe = 0;
                    bool readPending = false;
                    if (rov.hEvent != nullptr)
                    {
                        if (!::ReadFile(pipe, &probe, 1, nullptr, &rov) && ::GetLastError() == ERROR_IO_PENDING)
                        {
                            readPending = true;
                        }
                    }
                    if (!readPending)
                    {
                        _registry->NoteGone(id, L"the hook ended at once");
                    }
                    else
                    {
                        for (;;)
                        {
                            const HANDLE waits[3] = { _stopEvent, wake, rov.hEvent };
                            const DWORD w = ::WaitForMultipleObjects(3, waits, FALSE, INFINITE);
                            if (w == WAIT_OBJECT_0)
                            {
                                // The app is closing: the helper hears "{}" (no decision) and the PC keeps the dialog.
                                AskWriteAll(pipe, std::string{ "{}\n" }, _stopEvent, 2000);
                                _registry->NoteGone(id, L"Agentmaster closed");
                                break;
                            }
                            if (w == WAIT_OBJECT_0 + 1)
                            {
                                ::ResetEvent(wake);
                                std::optional<std::string> reply = _registry->TakeReply(id);
                                if (!reply)
                                {
                                    continue; // a wake-up with nothing queued (a release raced a write): wait again
                                }
                                const bool decision = *reply != "{}";
                                const bool ok = AskWriteAll(pipe, *reply + "\n", _stopEvent, 10000);
                                _registry->NoteWritten(id, ok);
                                AppendStateLog(L"hooks.log", L"[ask] " + sid8 + L" request " + std::to_wstring(id) + (decision ? L" answered from Telegram (" : L" released to the PC (") +
                                                                 std::to_wstring(reply->size()) + L" bytes)" + (ok ? L"" : L" \x2014 the WRITE FAILED: the hook had gone") + L"\n");
                                // The helper prints the line and exits; DisconnectNamedPipe would discard what it has not read yet, so
                                // wait (bounded) for it to close its end — the pending read then completes with a broken pipe.
                                DWORD ignored = 0;
                                AskWaitIo(pipe, rov, _stopEvent, 10000, ignored);
                                readPending = false;
                                break;
                            }
                            // The helper went away: answered or cancelled at the PC (Claude Code ends the hook on Esc; an answer at
                            // the PC leaves it running until the engine releases it), its claude exited, or its timeout passed.
                            _registry->NoteGone(id, L"the hook ended before an answer from here");
                            AppendStateLog(L"hooks.log", L"[ask] " + sid8 + L" request " + std::to_wstring(id) + L" ended at the hook's side (answered or cancelled at the PC, or claude exited)\n");
                            readPending = false;
                            break;
                        }
                    }
                    if (readPending)
                    {
                        DWORD ignored = 0;
                        ::CancelIoEx(pipe, &rov);
                        ::GetOverlappedResult(pipe, &rov, &ignored, TRUE);
                    }
                    if (rov.hEvent != nullptr)
                    {
                        ::CloseHandle(rov.hEvent);
                    }
                }
            }
        }
        catch (...)
        {
            // Forensics: a worker's failure (an allocation). The request is marked gone so its card closes; the helper finds a
            // broken pipe and leaves the dialog to the PC.
            LogSwallowedException(L"AskBridge::_Serve");
            if (id != 0)
            {
                _registry->NoteGone(id, L"an error in the question pipe");
            }
        }
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
        if (wake != nullptr)
        {
            ::CloseHandle(wake);
        }
    }
}
