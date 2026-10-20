// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the Telegram FILE LINKS' server (MESSAGING.md §21.32.4) -- see MessagingFileServer.h.
//
// The one TU that includes Winsock. Plain Win32 + the pure rules of MessagingFileLinks.h; no WinRT, no service type, so the
// engine harness links it (tests_messaging_files.cpp serves real files over loopback with it).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <wincred.h>

#include "MessagingFileServer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

#include "ClaudeSpawn.h" // LogSwallowedException (Rule #18)

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib") // CredReadW / CredWriteW

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>How long a client may take to send its request head.</summary>
        constexpr DWORD kReadTimeoutMs = 15000;
        /// <summary>How long one send may block (a stalled phone or relay ends the transfer after it).</summary>
        constexpr DWORD kWriteTimeoutMs = 60000;
        /// <summary>The longest request head read (the request line and the headers).</summary>
        constexpr size_t kMaxHeadBytes = 16384;
        /// <summary>The chunk a file is read and sent in.</summary>
        constexpr DWORD kChunkBytes = 64 * 1024;
        /// <summary>How long Stop waits for the connections' threads to end.</summary>
        constexpr auto kStopWait = std::chrono::seconds(5);

        /// <summary>A short ms clock for the log's durations.</summary>
        /// <returns>Milliseconds of a monotonic clock.</returns>
        int64_t MonoMs() noexcept
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        /// <summary>The dotted form of an IPv4 address (network order).</summary>
        /// <param name="addr">The address.</param>
        /// <returns>"a.b.c.d".</returns>
        std::wstring DottedIPv4(const in_addr& addr)
        {
            wchar_t buf[INET_ADDRSTRLEN]{};
            if (!::InetNtopW(AF_INET, &addr, buf, INET_ADDRSTRLEN))
            {
                return L"?";
            }
            return buf;
        }

        /// <summary>Sends all bytes (a send may take part of them); false when the socket failed or timed out.</summary>
        /// <param name="s">The socket.</param>
        /// <param name="data">The bytes.</param>
        /// <param name="len">How many.</param>
        /// <returns>True when everything was sent.</returns>
        bool SendAll(SOCKET s, const char* data, size_t len) noexcept
        {
            while (len > 0)
            {
                const int chunk = static_cast<int>((std::min)(len, size_t{ 1 } << 30));
                const int n = ::send(s, data, chunk, 0);
                if (n == SOCKET_ERROR || n <= 0)
                {
                    return false;
                }
                data += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

        /// <summary>A whole response without a file body (an error, a refusal, a health answer).</summary>
        /// <param name="status">The status.</param>
        /// <param name="body">A short plain-text body ("" for none; never with HEAD).</param>
        /// <param name="headOnly">True for a HEAD request (the body's length is announced, the body not sent).</param>
        /// <param name="extra">Extra header lines, each ending in "\r\n".</param>
        /// <returns>The bytes.</returns>
        std::string SimpleResponse(int status, std::string_view body, bool headOnly, std::string_view extra = {})
        {
            std::string r = "HTTP/1.1 " + std::to_string(status) + " " + std::string{ HttpReasonPhrase(status) } + "\r\n";
            r += "Content-Type: text/plain; charset=utf-8\r\n";
            r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            r += "Cache-Control: no-store\r\n";
            r += "X-Content-Type-Options: nosniff\r\n";
            r += extra;
            r += "Connection: close\r\n\r\n";
            if (!headOnly)
            {
                r += body;
            }
            return r;
        }

        /// <summary>Widens bytes one to one (an ASCII header value or method into a log line; other bytes become their Latin-1
        /// look-alikes, which is good enough for a log).</summary>
        /// <param name="s">The bytes.</param>
        /// <returns>The wide text.</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        std::wstring FromAscii(std::string_view s)
        {
            std::wstring w;
            w.reserve(s.size());
            for (const char c : s)
            {
                w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }
            return w;
        }

        /// <summary>The final path of an open file ("\\?\K:\..." stripped to "K:\..."), "" when Windows refuses.</summary>
        /// <param name="h">The file handle.</param>
        /// <returns>The path.</returns>
        std::wstring FinalPathOf(HANDLE h)
        {
            std::wstring buf(1024, L'\0');
            DWORD n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (n >= buf.size())
            {
                buf.assign(static_cast<size_t>(n) + 1, L'\0');
                n = ::GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            }
            if (n == 0 || n >= buf.size())
            {
                return {};
            }
            buf.resize(n);
            if (buf.rfind(L"\\\\?\\UNC\\", 0) == 0)
            {
                return L"\\\\" + buf.substr(8);
            }
            if (buf.rfind(L"\\\\?\\", 0) == 0)
            {
                return buf.substr(4);
            }
            return buf;
        }

        /// <summary>A short form of a key for the log (its first 8 hex characters).</summary>
        /// <param name="hash">The key.</param>
        /// <returns>"3f9a8b2c".</returns>
        std::wstring HashShort(const std::string& hash)
        {
            return FromAscii(std::string_view{ hash }.substr(0, 8));
        }
    }

    struct FileLinkServer::Core
    {
        FileServeAuthorizer authorize;
        FileServeLogger log;
        std::atomic<bool> stopping{ false };
        SOCKET listener{ INVALID_SOCKET };
        std::atomic<uint16_t> port{ 0 }; // atomic: Port() reads it from any thread while Stop clears it
        bool wsaStarted{ false };
        std::mutex mtx; // guards `conns`
        std::condition_variable cv; // signalled when a connection ends
        std::vector<SOCKET> conns; // the live connections (Stop shuts them down; their own threads close them)
        std::atomic<uint32_t> active{ 0 };
        std::atomic<uint64_t> requests{ 0 };
        std::atomic<uint64_t> served{ 0 };
        std::atomic<uint64_t> refused{ 0 };
        std::atomic<uint64_t> bytes{ 0 };

        ~Core()
        {
            if (listener != INVALID_SOCKET)
            {
                ::closesocket(listener);
            }
            if (wsaStarted)
            {
                ::WSACleanup();
            }
        }

        /// <summary>Writes a log line; a throwing sink is contained (the server never stops for its logger).</summary>
        /// <param name="line">The line.</param>
        void Log(const std::wstring& line) noexcept
        {
            try
            {
                if (log)
                {
                    log(line);
                }
            }
            catch (...)
            {
                LogSwallowedException(L"FileLinkServer::Core::Log");
            }
        }
    };

    namespace
    {
        /// <summary>
        /// Serves ONE connection: reads the head, asks the service, sends the file (or the refusal), closes. Runs on its own
        /// thread; owns `s` (closes it) and holds the Core alive for its whole run.
        /// </summary>
        /// <param name="core">The server's state.</param>
        /// <param name="s">The connection.</param>
        /// <param name="peer">The peer's address.</param>
        void ServeConnection(std::shared_ptr<FileLinkServer::Core> core, SOCKET s, sockaddr_in peer) noexcept;
    }

    FileLinkServer::FileLinkServer(FileServeAuthorizer authorize, FileServeLogger log) :
        _core(std::make_shared<Core>())
    {
        _core->authorize = std::move(authorize);
        _core->log = std::move(log);
    }

    FileLinkServer::~FileLinkServer()
    {
        Stop();
    }

    std::shared_ptr<FileLinkServer::Core> FileLinkServer::_Current() const noexcept
    {
        std::lock_guard lk{ _coreMtx };
        return _core;
    }

    bool FileLinkServer::Start(uint16_t port, std::wstring& error, bool loopbackOnly) noexcept
    {
        try
        {
            const auto previous = _Current();
            if (_acceptThread.joinable() || !previous)
            {
                error = L"the server already runs";
                return false;
            }
            // A fresh Core per start: a connection thread of the last run (still ending) keeps the old one.
            auto core = std::make_shared<Core>();
            core->authorize = previous->authorize;
            core->log = previous->log;
            WSADATA wsa{};
            const int ws = ::WSAStartup(MAKEWORD(2, 2), &wsa);
            if (ws != 0)
            {
                error = L"Winsock did not start (error " + std::to_wstring(ws) + L")";
                return false;
            }
            core->wsaStarted = true;
            core->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (core->listener == INVALID_SOCKET)
            {
                error = L"no socket (error " + std::to_wstring(::WSAGetLastError()) + L")";
                return false;
            }
            // Exclusive: another program can never bind the same port beside it and take some of its connections.
            BOOL exclusive = TRUE;
            ::setsockopt(core->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
            addr.sin_port = htons(port);
            if (::bind(core->listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
            {
                const int e = ::WSAGetLastError();
                error = e == WSAEADDRINUSE ? L"port " + std::to_wstring(port) + L" is already in use (error 10048)" :
                                            L"could not bind port " + std::to_wstring(port) + L" (error " + std::to_wstring(e) + L")";
                return false;
            }
            if (::listen(core->listener, SOMAXCONN) == SOCKET_ERROR)
            {
                error = L"could not listen (error " + std::to_wstring(::WSAGetLastError()) + L")";
                return false;
            }
            // The port really bound (the one the system picked for port 0).
            sockaddr_in bound{};
            int boundLen = sizeof(bound);
            const uint16_t actualPort = ::getsockname(core->listener, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0 ? ntohs(bound.sin_port) : port;
            core->port.store(actualPort);
            {
                std::lock_guard lk{ _coreMtx };
                _core = core;
            }
            _acceptThread = std::thread([core]() noexcept {
                while (!core->stopping.load())
                {
                    sockaddr_in peer{};
                    int len = sizeof(peer);
                    const SOCKET s = ::accept(core->listener, reinterpret_cast<sockaddr*>(&peer), &len);
                    if (s == INVALID_SOCKET)
                    {
                        if (core->stopping.load())
                        {
                            break;
                        }
                        ::Sleep(50); // a transient error (no buffers): try again shortly
                        continue;
                    }
                    // Only loopback and private peers: the relay, the LAN, the VPN's NAT address. Anything else is closed unread.
                    if (!IsAllowedFileLinkPeer(ntohl(peer.sin_addr.s_addr)))
                    {
                        ::closesocket(s);
                        continue;
                    }
                    bool registered = false;
                    try
                    {
                        if (core->active.load() >= kMaxConnections)
                        {
                            const std::string busy = SimpleResponse(503, "busy\n", false, "Retry-After: 2\r\n");
                            ::send(s, busy.data(), static_cast<int>(busy.size()), 0);
                            ::closesocket(s);
                            continue;
                        }
                        {
                            std::lock_guard lk{ core->mtx };
                            core->conns.push_back(s);
                        }
                        registered = true;
                        core->active.fetch_add(1);
                        std::thread(ServeConnection, core, s, peer).detach();
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: this one connection is dropped (no reply could be built or no thread made); the
                        // server goes on. (This lambda is noexcept: an escaped throw would end the app.)
                        LogSwallowedException(L"FileLinkServer accept (spawn a connection thread)");
                        if (registered)
                        {
                            try
                            {
                                std::lock_guard lk{ core->mtx };
                                core->conns.erase(std::remove(core->conns.begin(), core->conns.end(), s), core->conns.end());
                            }
                            catch (...)
                            {
                                LogSwallowedException(L"FileLinkServer accept (unregister a dropped connection)");
                            }
                            core->active.fetch_sub(1);
                        }
                        ::closesocket(s);
                    }
                }
            });
            _running.store(true);
            core->Log(std::wstring{ L"[file-link] server listening on " } + (loopbackOnly ? L"127.0.0.1:" : L"0.0.0.0:") + std::to_wstring(actualPort));
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"FileLinkServer::Start");
            error = L"the server could not start (see hooks.log)";
            return false;
        }
    }

    void FileLinkServer::Stop() noexcept
    {
        try
        {
            if (!_acceptThread.joinable())
            {
                return;
            }
            const auto core = _Current();
            _running.store(false);
            core->stopping.store(true);
            // Closing the listener ends the blocking accept(). The handle is cleared only AFTER the join: the accept thread
            // reads it on every pass, and a write before the join would race that read.
            ::closesocket(core->listener);
            _acceptThread.join();
            core->listener = INVALID_SOCKET;
            // Every live transfer ends now: a shut-down socket fails its next send/recv, and its thread closes it.
            std::unique_lock lk{ core->mtx };
            for (const SOCKET s : core->conns)
            {
                ::shutdown(s, SD_BOTH);
            }
            core->cv.wait_for(lk, kStopWait, [&core] { return core->active.load() == 0; });
            const uint32_t left = core->active.load();
            lk.unlock();
            core->Log(L"[file-link] server stopped (port " + std::to_wstring(core->port.load()) + L"; " + std::to_wstring(core->requests.load()) + L" request(s), " +
                      std::to_wstring(core->served.load()) + L" served" + (left ? L"; " + std::to_wstring(left) + L" connection(s) still ending" : L"") + L")");
            core->port.store(0);
            // A new Core for a later Start keeps the callbacks; this one lives on in any thread still ending.
            auto next = std::make_shared<Core>();
            next->authorize = core->authorize;
            next->log = core->log;
            std::lock_guard coreLock{ _coreMtx };
            _core = std::move(next);
        }
        catch (...)
        {
            LogSwallowedException(L"FileLinkServer::Stop");
        }
    }

    bool FileLinkServer::Running() const noexcept
    {
        return _running.load();
    }

    uint16_t FileLinkServer::Port() const noexcept
    {
        const auto core = _Current();
        return core ? core->port.load() : 0;
    }

    FileLinkServer::Totals FileLinkServer::Counts() const noexcept
    {
        Totals t;
        if (const auto core = _Current())
        {
            t.requests = core->requests.load();
            t.served = core->served.load();
            t.refused = core->refused.load();
            t.bytes = core->bytes.load();
        }
        return t;
    }

    namespace
    {
        void ServeConnection(std::shared_ptr<FileLinkServer::Core> core, SOCKET s, sockaddr_in peer) noexcept
        {
            const int64_t started = MonoMs();
            std::wstring logLine;
            // Leaving: the socket closed, the connection unregistered, Stop told (whatever happened in between).
            struct Leave
            {
                FileLinkServer::Core* core;
                SOCKET s;
                ~Leave()
                {
                    ::closesocket(s);
                    try
                    {
                        std::lock_guard lk{ core->mtx };
                        core->conns.erase(std::remove(core->conns.begin(), core->conns.end(), s), core->conns.end());
                    }
                    catch (...)
                    {
                        // ⚠ DELIBERATELY BARE: erase of a SOCKET never throws; nothing to report, and a destructor must not throw.
                    }
                    core->active.fetch_sub(1);
                    core->cv.notify_all();
                }
            } leave{ core.get(), s };
            try
            {
                DWORD rto = kReadTimeoutMs;
                DWORD wto = kWriteTimeoutMs;
                ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof(rto));
                ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&wto), sizeof(wto));
                // The head: read until the blank line (the body of a GET/HEAD is never read).
                std::string head;
                char buf[4096];
                size_t end = std::string::npos;
                while (end == std::string::npos)
                {
                    const int n = ::recv(s, buf, sizeof(buf), 0);
                    if (n <= 0)
                    {
                        return; // closed, reset or timed out before a whole head: nothing to answer
                    }
                    head.append(buf, static_cast<size_t>(n));
                    end = head.find("\r\n\r\n");
                    if (end == std::string::npos && head.size() > kMaxHeadBytes)
                    {
                        const std::string r = SimpleResponse(400, "request head too large\n", false);
                        SendAll(s, r.data(), r.size());
                        core->refused.fetch_add(1);
                        return;
                    }
                }
                core->requests.fetch_add(1);
                const std::wstring peerText = DottedIPv4(peer.sin_addr);
                const auto req = ParseHttpRequestHead(std::string_view{ head }.substr(0, end));
                if (!req)
                {
                    const std::string r = SimpleResponse(400, "bad request\n", false);
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 400 (a malformed request) from " + peerText);
                    return;
                }
                const bool isHead = req->method == "HEAD";
                // Where the request really came from, when the relay passed it on (Cloudflare's header, then the relay's).
                std::wstring via;
                if (const auto* cf = req->Header("Cf-Connecting-Ip"))
                {
                    via = FromAscii(*cf);
                }
                else if (const auto* xff = req->Header("X-Forwarded-For"))
                {
                    via = FromAscii(*xff);
                }
                const std::wstring from = L" from " + peerText + (via.empty() ? L"" : L" (for " + via + L")");
                if (req->target == "/healthz")
                {
                    const std::string r = SimpleResponse(200, "ok\n", isHead);
                    SendAll(s, r.data(), r.size());
                    return;
                }
                if (req->method != "GET" && !isHead)
                {
                    const std::string r = SimpleResponse(405, "only GET and HEAD\n", false, "Allow: GET, HEAD\r\n");
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 405 " + FromAscii(req->method) + from);
                    return;
                }
                FileLinkTargetError why = FileLinkTargetError::None;
                const auto target = ParseFileLinkTarget(req->target, &why);
                if (!target)
                {
                    const std::string r = SimpleResponse(404, "not found\n", isHead);
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 404 (" + std::wstring{ FileLinkTargetErrorWord(why) } + L")" + from);
                    return;
                }
                FileServeDecision decision;
                try
                {
                    decision = core->authorize ? core->authorize(*target) : FileServeDecision{};
                }
                catch (...)
                {
                    LogSwallowedException(L"FileLinkServer authorizer");
                    decision.status = 500;
                    decision.reason = L"the service failed";
                }
                const std::wstring label = L" " + target->station + L" " + HashShort(target->hash) + L" \"" + FileLinkFileName(target->path) + L"\"";
                if (decision.status != 200)
                {
                    const int status = decision.status == 500 ? 500 : 404;
                    const std::string r = SimpleResponse(status, status == 500 ? "error\n" : "not found\n", isHead);
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] " + std::to_wstring(status) + label + L" (" + decision.reason + L")" + from);
                    return;
                }
                // The file: opened for reading, sharing every mode (an editor may hold it open and write it).
                HANDLE h = ::CreateFileW(decision.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    const DWORD e = ::GetLastError();
                    const std::string r = SimpleResponse(404, "not found\n", isHead);
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 404" + label + L" (cannot open: error " + std::to_wstring(e) + L")" + from);
                    return;
                }
                struct CloseFile
                {
                    HANDLE h;
                    ~CloseFile() { ::CloseHandle(h); }
                } closeFile{ h };
                // A FILE on a disk only: a folder, or a device that a name like "COM1" opens (a serial port would block the
                // read), is refused here even if a check before missed it.
                FILE_STANDARD_INFO info{};
                if (::GetFileType(h) != FILE_TYPE_DISK || !::GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info)) || info.Directory)
                {
                    const std::string r = SimpleResponse(404, "not found\n", isHead);
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 404" + label + L" (not a file)" + from);
                    return;
                }
                // A link to a link: the FINAL file is checked too (a symlink in a harmless folder that points at a key, or into a
                // folder the cog excludes).
                // A final path Windows cannot name is refused too: the check would have nothing to look at.
                const std::wstring finalPath = FinalPathOf(h);
                if (finalPath.empty() || LooksLikeSecretFile(finalPath) || IsUnderExcludedFolder(finalPath, decision.excludedKeys))
                {
                    const std::string r = SimpleResponse(404, "not found\n", isHead);
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 404" + label + L" (the file it resolves to has no name, looks like a secret or lies in an excluded folder)" + from);
                    return;
                }
                const uint64_t size = static_cast<uint64_t>(info.EndOfFile.QuadPart);
                FileByteRange range{};
                const FileRangeVerdict verdict = ParseFileByteRange(req->Header("Range"), size, range);
                if (verdict == FileRangeVerdict::Unsatisfiable)
                {
                    const std::string r = SimpleResponse(416, "", isHead, "Content-Range: bytes */" + std::to_string(size) + "\r\n");
                    SendAll(s, r.data(), r.size());
                    core->refused.fetch_add(1);
                    core->Log(L"[file-link] 416" + label + from);
                    return;
                }
                const uint64_t first = verdict == FileRangeVerdict::Partial ? range.first : 0;
                const uint64_t count = verdict == FileRangeVerdict::Partial ? range.last - range.first + 1 : size;
                const int status = verdict == FileRangeVerdict::Partial ? 206 : 200;
                std::string headers = "HTTP/1.1 " + std::to_string(status) + " " + std::string{ HttpReasonPhrase(status) } + "\r\n";
                headers += "Content-Type: " + std::string{ GuessFileMimeType(FileLinkFileName(decision.path)) } + "\r\n";
                headers += "Content-Length: " + std::to_string(count) + "\r\n";
                headers += "Content-Disposition: " + ContentDispositionAttachment(FileLinkFileName(decision.path)) + "\r\n";
                headers += "Accept-Ranges: bytes\r\n";
                if (status == 206)
                {
                    headers += "Content-Range: bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" + std::to_string(size) + "\r\n";
                }
                headers += "Cache-Control: no-store\r\n";
                headers += "X-Content-Type-Options: nosniff\r\n";
                headers += "X-Robots-Tag: noindex, nofollow\r\n";
                headers += "Referrer-Policy: no-referrer\r\n";
                headers += "Connection: close\r\n\r\n";
                if (!SendAll(s, headers.data(), headers.size()))
                {
                    return;
                }
                uint64_t sent = 0;
                bool complete = true;
                if (!isHead && count > 0)
                {
                    LARGE_INTEGER pos{};
                    pos.QuadPart = static_cast<LONGLONG>(first);
                    if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
                    {
                        return;
                    }
                    std::string chunk(kChunkBytes, '\0');
                    while (sent < count && !core->stopping.load())
                    {
                        const DWORD want = static_cast<DWORD>((std::min)(static_cast<uint64_t>(kChunkBytes), count - sent));
                        DWORD got = 0;
                        if (!::ReadFile(h, chunk.data(), want, &got, nullptr) || got == 0)
                        {
                            break; // the file shrank or failed mid-read: the client sees a short body
                        }
                        if (!SendAll(s, chunk.data(), got))
                        {
                            break;
                        }
                        sent += got;
                    }
                    complete = sent == count;
                }
                core->served.fetch_add(1);
                core->bytes.fetch_add(sent);
                logLine = L"[file-link] " + std::to_wstring(status) + L" " + FromAscii(req->method) + label + L" " + std::to_wstring(isHead ? 0 : sent) + L"/" +
                          std::to_wstring(count) + L" B in " + std::to_wstring(MonoMs() - started) + L" ms" +
                          (decision.sessionId.empty() ? L"" : L" session " + decision.sessionId.substr(0, 8)) + (complete ? L"" : L" (ENDED EARLY)") + from;
                core->Log(logLine);
            }
            catch (...)
            {
                // Rule #18. Recovery: this connection ends (the Leave guard closes it); the server goes on.
                LogSwallowedException(L"FileLinkServer::ServeConnection");
            }
        }
    }

    // ===== the adapters' addresses and the host name ====================================================================

    std::vector<FileLinkLanAddress> EnumerateFileLinkLanAddresses() noexcept
    {
        std::vector<FileLinkLanAddress> out;
        try
        {
            ULONG size = 16 * 1024;
            std::vector<unsigned char> buf;
            ULONG rc = ERROR_BUFFER_OVERFLOW;
            for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt)
            {
                buf.assign(size, 0);
                rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
            }
            if (rc != NO_ERROR)
            {
                return out;
            }
            for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next)
            {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                {
                    continue;
                }
                const std::wstring name = a->FriendlyName ? a->FriendlyName : L"";
                const std::wstring description = a->Description ? a->Description : L"";
                const auto has = [&](std::wstring_view needle) {
                    const auto lowered = [](std::wstring s) {
                        for (auto& c : s)
                        {
                            if (c >= L'A' && c <= L'Z')
                            {
                                c = static_cast<wchar_t>(c - L'A' + L'a');
                            }
                        }
                        return s;
                    };
                    const std::wstring n = lowered(std::wstring{ needle });
                    return lowered(name).find(n) != std::wstring::npos || lowered(description).find(n) != std::wstring::npos;
                };
                const bool isVirtual = a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PROP_VIRTUAL || has(L"vethernet") || has(L"hyper-v") ||
                                       has(L"wsl") || has(L"virtualbox") || has(L"vmware") || has(L"tap-windows") || has(L"tap adapter") ||
                                       has(L"wintun") || has(L"wireguard") || has(L"tailscale") || has(L"zerotier") || has(L"loopback");
                for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
                {
                    if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    {
                        continue;
                    }
                    const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                    const uint32_t host = ntohl(sin->sin_addr.s_addr);
                    if ((host >> 24) == 127 || !IsAllowedFileLinkPeer(host))
                    {
                        continue; // loopback, link-local (169.254) and public addresses are not offered
                    }
                    out.push_back(FileLinkLanAddress{ name, DottedIPv4(sin->sin_addr), isVirtual });
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the dropdown offers what was found so far (perhaps nothing but the PC's name).
            LogSwallowedException(L"EnumerateFileLinkLanAddresses");
        }
        return out;
    }

    std::wstring FileLinkHostName() noexcept
    {
        try
        {
            wchar_t buf[256]{};
            DWORD n = static_cast<DWORD>(std::size(buf));
            if (::GetComputerNameExW(ComputerNameDnsHostname, buf, &n))
            {
                return std::wstring{ buf, n };
            }
        }
        catch (...)
        {
            LogSwallowedException(L"FileLinkHostName");
        }
        return {};
    }

    // ===== the secret in Credential Manager ==============================================================================

    namespace
    {
        /// <summary>32 bytes from the system RNG.</summary>
        /// <param name="out">The bytes.</param>
        /// <returns>True when the RNG answered.</returns>
        bool RandomSecret(std::array<uint8_t, 32>& out) noexcept
        {
            return BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        }

        /// <summary>Stores the secret as a generic credential (64 hex characters).</summary>
        /// <param name="target">The target name.</param>
        /// <param name="secret">The secret.</param>
        /// <param name="error">Why it failed.</param>
        /// <returns>True when stored.</returns>
        bool WriteSecret(const std::wstring& target, const std::array<uint8_t, 32>& secret, std::wstring& error)
        {
            std::wstring value = FormatFileLinkSecretHex(secret);
            std::wstring user = L"agentmaster";
            std::wstring name = target;
            CREDENTIALW cred{};
            cred.Type = CRED_TYPE_GENERIC;
            cred.TargetName = name.data();
            cred.UserName = user.data();
            cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
            cred.CredentialBlobSize = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            cred.CredentialBlob = reinterpret_cast<LPBYTE>(value.data());
            const bool ok = ::CredWriteW(&cred, 0) != FALSE;
            if (!ok)
            {
                error = L"Credential Manager refused the secret (error " + std::to_wstring(::GetLastError()) + L")";
            }
            ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
            return ok;
        }
    }

    bool LoadOrCreateFileLinkSecret(const std::wstring& target, std::array<uint8_t, 32>& secret, bool& created, std::wstring& error) noexcept
    {
        created = false;
        try
        {
            PCREDENTIALW cred = nullptr;
            if (::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) && cred)
            {
                std::wstring hex;
                if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t))
                {
                    hex.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
                }
                ::CredFree(cred);
                while (!hex.empty() && (hex.back() == L'\0' || hex.back() == L' ' || hex.back() == L'\r' || hex.back() == L'\n'))
                {
                    hex.pop_back();
                }
                const auto parsed = ParseFileLinkSecretHex(hex);
                ::SecureZeroMemory(hex.data(), hex.size() * sizeof(wchar_t));
                if (parsed)
                {
                    secret = *parsed;
                    return true;
                }
                // A damaged credential: replaced below (its links were unreadable anyway).
            }
            if (!RandomSecret(secret))
            {
                error = L"the system RNG did not answer";
                return false;
            }
            if (!WriteSecret(target, secret, error))
            {
                return false;
            }
            created = true;
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"LoadOrCreateFileLinkSecret");
            error = L"the secret could not be read (see hooks.log)";
            return false;
        }
    }

    bool ReplaceFileLinkSecret(const std::wstring& target, std::array<uint8_t, 32>& secret, std::wstring& error) noexcept
    {
        try
        {
            std::array<uint8_t, 32> fresh{};
            if (!RandomSecret(fresh))
            {
                error = L"the system RNG did not answer";
                return false;
            }
            if (!WriteSecret(target, fresh, error))
            {
                return false;
            }
            secret = fresh;
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"ReplaceFileLinkSecret");
            error = L"the secret could not be replaced (see hooks.log)";
            return false;
        }
    }
}
