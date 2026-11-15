// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the SESSION LINK's server (SessionLink.h, MOD_LINK.md, MESSAGING.md §21.56): one Winsock loop on
// 127.0.0.1:<ephemeral port> that answers the agentmaster-link mods' hello, held polls and results, plus the blocking
// senders (Run) that wait for a command's result.
//
// Why one thread and WSAPoll (not a thread per connection, not IOCP): every linked session keeps ONE poll parked for up
// to 5 s, so the server holds ~one idle connection per session (100+ at fleet scale, MOD_LINK.md §4.3) and answers a
// few tiny requests a second. A single poll loop over non-blocking sockets parks a poll as a flag on its connection
// (no thread waits on it), costs nothing while idle (the loop sleeps in WSAPoll, woken by a loopback socket pair when a
// command is queued), and needs none of IOCP's completion bookkeeping at this request rate.
//
// Plain Win32 + Winsock, no WinRT (the vcxproj marks it <PrecompiledHeader>NotUsing</PrecompiledHeader>); the m5
// harness links it (tests_side_question.cpp drives a real loopback round trip).

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

#include "SessionLink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException, ShortId

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace Agentmaster::Link
{
    namespace
    {
        /// <summary>The most connections the loop keeps at once; one more is closed at its accept (a leak, or someone else's).</summary>
        constexpr size_t kMaxConnections = 512;
        /// <summary>A connection with no parked poll and nothing to send is closed after this long idle (ms): the mod's
        /// keep-alive reconnects at once, and a connection a claude left behind cannot pile up.</summary>
        constexpr int64_t kIdleConnectionMs = 120 * 1000;
        /// <summary>The bytes one recv takes.</summary>
        constexpr int kRecvChunk = 64 * 1024;
        /// <summary>A registration whose mod never said hello is forgotten after this long (ms): a launch without the mod.</summary>
        constexpr int64_t kForgetNeverLinkedMs = 24LL * 60 * 60 * 1000;
        /// <summary>A link not seen for this long (ms) is forgotten: its claude ended.</summary>
        constexpr int64_t kForgetGoneMs = 60LL * 60 * 1000;
        /// <summary>How often the loop forgets dead tokens (ms).</summary>
        constexpr int64_t kForgetEveryMs = 60 * 1000;

        /// <summary>The monotonic clock in ms (the hub's and the deadlines' time: immune to a wall-clock change).</summary>
        /// <returns>GetTickCount64.</returns>
        int64_t TickMs() noexcept
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        /// <summary>One [link] line in hooks.log. Never throws (a logging failure must not become a link failure).</summary>
        /// <param name="line">The line, without the tag and the line break.</param>
        void LogLink(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"hooks.log", L"[link] " + line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS a log write; reporting its own
                // failure would write to the same log again. Degradation: the line is lost, nothing else.
            }
        }

        /// <summary>A new launch token: 16 random bytes (BCryptGenRandom, the system's CSPRNG) as 32 lowercase hex digits.</summary>
        /// <returns>The token, or "" when the system could not give random bytes (the launch then has no link).</returns>
        std::wstring MintToken() noexcept
        {
            unsigned char bytes[16]{};
            if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            {
                return {};
            }
            static constexpr wchar_t kHex[] = L"0123456789abcdef";
            std::wstring out;
            try
            {
                out.reserve(32);
                for (const unsigned char b : bytes)
                {
                    out.push_back(kHex[b >> 4]);
                    out.push_back(kHex[b & 0x0F]);
                }
            }
            catch (...)
            {
                // std::bad_alloc for 32 characters: no token, no link for this launch (the mod stays dormant).
                LogSwallowedException(L"SessionLink MintToken");
                return {};
            }
            return out;
        }

        /// <summary>Puts a socket in non-blocking mode (the loop never waits on one socket).</summary>
        /// <param name="s">The socket.</param>
        /// <returns>True on success.</returns>
        bool SetNonBlocking(SOCKET s) noexcept
        {
            u_long on = 1;
            return ::ioctlsocket(s, FIONBIO, &on) == 0;
        }

        /// <summary>A short JSON error body: {"error":"…"}.</summary>
        /// <param name="what">The reason (ASCII).</param>
        /// <returns>The UTF-8 body.</returns>
        /// <exception cref="std::bad_alloc">If the body cannot be allocated.</exception>
        std::string ErrorBody(const char* what)
        {
            return std::string{ "{\"error\":\"" } + what + "\"}";
        }
    }

    /// <summary>One accepted connection (the loop thread's alone).</summary>
    struct LinkConnection
    {
        SOCKET s{ INVALID_SOCKET }; // the socket
        std::string in; // received bytes not yet read as requests
        std::string out; // bytes still to send
        bool closeAfterWrite{ false }; // close once `out` is sent (an error answer, "Connection: close")
        bool parked{ false }; // a poll is held on it
        std::wstring parkedToken; // the token of the parked poll
        uint32_t holdMs{ kLinkDefaultHoldMs }; // the parked poll's hold
        int64_t parkedDeadline{ 0 }; // when the parked poll is answered empty (TickMs)
        bool keepAlive{ true }; // the parked poll's request asked to keep the connection
        int64_t lastActivity{ 0 }; // the last byte in or out (TickMs)
    };

    /// <summary>A Run waiting for a command's result.</summary>
    struct LinkWaiter
    {
        std::wstring token; // the link the command went to
        bool done{ false }; // the result arrived
        json::Value result; // the mod's result object
    };

    /// <summary>The loop's state: the hub and the waiters under one lock, the sockets, the thread.</summary>
    struct SessionLinkServer::Core
    {
        mutable std::mutex mtx; // guards hub, waiters, nextCommand, running, endpoint, wakeSend
        std::condition_variable cv; // a result arrived, a link was judged, or the loop stopped (the waiters re-check)
        LinkHub hub; // tokens, links, queues, liveness
        std::map<std::wstring, LinkWaiter> waiters; // command id → its waiter
        uint64_t nextCommand{ 1 }; // the next command id's number
        bool running{ false }; // the loop runs (Start..Stop)
        std::atomic<bool> stopping{ false }; // the loop must end at its next turn
        std::wstring endpoint; // "http://127.0.0.1:<port>" while running
        SOCKET listener{ INVALID_SOCKET }; // the listening socket
        SOCKET wakeRecv{ INVALID_SOCKET }; // the loop polls it: a byte means "look at the hub"
        SOCKET wakeSend{ INVALID_SOCKET }; // senders write a byte to it (under mtx)
        std::thread loop; // the loop thread
        std::vector<LinkConnection> conns; // the loop thread's connections (only it touches them)
        int64_t lastForget{ 0 }; // when the hub last forgot dead tokens (TickMs)
        bool wsaStarted{ false }; // WSAStartup succeeded (paired with WSACleanup at Stop)

        /// <summary>Wakes the loop (a command was queued, the server stops). Call with mtx held. Never throws.</summary>
        void WakeLocked() noexcept
        {
            if (wakeSend != INVALID_SOCKET)
            {
                const char b = 1;
                ::send(wakeSend, &b, 1, 0); // a full buffer means a wake is already pending: losing this byte loses nothing
            }
        }

        /// <summary>The loop thread's body (catches everything: an exception escaping a std::thread ends the process).</summary>
        void Loop() noexcept;

        /// <summary>Reads what arrived on a connection and answers each complete request (loop thread).</summary>
        /// <param name="c">The connection.</param>
        /// <returns>False when the connection is to be closed now.</returns>
        bool Receive(LinkConnection& c);

        /// <summary>Answers one request (loop thread; takes mtx for the hub).</summary>
        /// <param name="c">The connection it came on.</param>
        /// <param name="r">The request.</param>
        void Handle(LinkConnection& c, LinkHttpRequest& r);

        /// <summary>Writes a response into a connection's output (loop thread).</summary>
        /// <param name="c">The connection.</param>
        /// <param name="status">The status.</param>
        /// <param name="reason">The reason phrase.</param>
        /// <param name="body">The UTF-8 JSON body.</param>
        /// <param name="keepAlive">Keep the connection.</param>
        void Respond(LinkConnection& c, int status, const char* reason, const std::string& body, bool keepAlive);

        /// <summary>Answers every parked poll that has commands queued or whose hold ran out (loop thread).</summary>
        /// <param name="now">Now (TickMs).</param>
        void ServeParked(int64_t now);

        /// <summary>Closes a connection and releases a poll parked on it (loop thread). Never throws.</summary>
        /// <param name="c">The connection.</param>
        void Close(LinkConnection& c) noexcept;
    };

    void SessionLinkServer::Core::Respond(LinkConnection& c, int status, const char* reason, const std::string& body, bool keepAlive)
    {
        c.out += BuildLinkHttpResponse(status, reason, body, keepAlive);
        c.closeAfterWrite = c.closeAfterWrite || !keepAlive;
    }

    void SessionLinkServer::Core::Close(LinkConnection& c) noexcept
    {
        if (c.parked)
        {
            // The poll's connection is gone (the claude ended, the mod reloaded): the link stays live only for its grace, so a
            // Run waiting on it learns of the loss at its next look.
            try
            {
                std::lock_guard lk{ mtx };
                hub.SetParked(c.parkedToken, false, c.holdMs, TickMs());
            }
            catch (...)
            {
                // std::system_error from the lock: the link reads live until its grace runs out, then lost. Logged, not fatal.
                LogSwallowedException(L"SessionLink Close (release the parked poll)");
            }
            cv.notify_all();
            c.parked = false;
        }
        if (c.s != INVALID_SOCKET)
        {
            ::closesocket(c.s);
            c.s = INVALID_SOCKET;
        }
    }

    bool SessionLinkServer::Core::Receive(LinkConnection& c)
    {
        char buffer[kRecvChunk];
        bool peerClosed = false;
        for (;;)
        {
            const int got = ::recv(c.s, buffer, sizeof(buffer), 0);
            if (got == 0)
            {
                peerClosed = true; // the peer closed (or half-closed): what it sent before is still answered
                break;
            }
            if (got == SOCKET_ERROR)
            {
                if (::WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    return false; // a broken connection
                }
                break; // nothing more for now: answer what came
            }
            c.in.append(buffer, static_cast<size_t>(got));
            c.lastActivity = TickMs();
            if (c.in.size() > kLinkMaxHeadBytes + kLinkMaxBodyBytes + 4)
            {
                break; // more than one whole request could hold: the reader below refuses it
            }
        }
        // Each complete request is answered in order; a parked poll stops the reading (a fetch never pipelines behind it).
        bool reading = true;
        while (reading && !c.parked && !c.closeAfterWrite)
        {
            LinkHttpRequest r;
            switch (TakeLinkHttpRequest(c.in, r))
            {
            case LinkHttpTake::NeedMore:
                reading = false;
                break;
            case LinkHttpTake::Request:
                Handle(c, r);
                break;
            case LinkHttpTake::TooLarge:
                Respond(c, 413, "Payload Too Large", ErrorBody("too large"), false);
                reading = false;
                break;
            case LinkHttpTake::LengthRequired:
                Respond(c, 411, "Length Required", ErrorBody("a body needs a Content-Length"), false);
                reading = false;
                break;
            default:
                Respond(c, 400, "Bad Request", ErrorBody("malformed request"), false);
                reading = false;
                break;
            }
        }
        if (peerClosed)
        {
            // The peer closed (or half-closed) after its requests: what they asked is answered and written, then the connection
            // closes. A poll it left parked has no reader any more: closed at once (Close releases the link's poll).
            if (c.parked || c.out.empty())
            {
                return false;
            }
            c.closeAfterWrite = true;
        }
        return true;
    }

    void SessionLinkServer::Core::Handle(LinkConnection& c, LinkHttpRequest& r)
    {
        const std::wstring token = detail::Utf8ToWide(r.token);
        const int64_t now = TickMs();
        std::wstring logLine; // written after the lock is released (a log write must never hold the hub)
        {
            std::unique_lock lk{ mtx };
            if (token.empty() || !hub.Knows(token))
            {
                lk.unlock();
                // An unknown token: the mod stops polling (its launch is not one this process made — an older run's claude).
                Respond(c, 404, "Not Found", ErrorBody("unknown token"), r.keepAlive);
                return;
            }
            if (r.method == "POST" && r.path == "/v1/hello")
            {
                const auto body = json::Parse(detail::Utf8ToWide(r.body));
                if (!body || body->type != json::Value::Type::Obj)
                {
                    lk.unlock();
                    Respond(c, 400, "Bad Request", ErrorBody("the hello is not a JSON object"), r.keepAlive);
                    return;
                }
                const int64_t protocol = body->I64At(L"protocol", 0);
                if (protocol != kLinkProtocol)
                {
                    lk.unlock();
                    Respond(c, 409, "Conflict", ErrorBody("protocol"), r.keepAlive);
                    LogLink(L"a mod of protocol " + std::to_wstring(protocol) + L" said hello; this build speaks " + std::to_wstring(kLinkProtocol) + L" (it stays unlinked)");
                    return;
                }
                const std::wstring session = body->StrAt(L"session");
                hub.Hello(token, session, body->StrAt(L"mod"), body->StrAt(L"claude"), now);
                const LinkInfo info = hub.Info(token);
                lk.unlock();
                auto reply = json::Value::MkObj();
                reply.Set(L"protocol", json::Value::MkNum(kLinkProtocol));
                reply.Set(L"hold", json::Value::MkNum(kLinkDefaultHoldMs));
                Respond(c, 200, "OK", LinkJsonBody(reply), r.keepAlive);
                logLine = L"hello from " + ShortId(info.sessionId) + (info.sessionId != info.launchSessionId ? L" (launched as " + ShortId(info.launchSessionId) + L")" : std::wstring{}) +
                          L": mod " + info.modVersion + L", Claude Code " + info.claudeVersion;
            }
            else if (r.method == "GET" && r.path == "/v1/commands")
            {
                uint32_t hold = kLinkDefaultHoldMs;
                if (const auto h = r.query.find("hold"); h != r.query.end() && !h->second.empty() && h->second.size() <= 6 &&
                                                         std::all_of(h->second.begin(), h->second.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                {
                    hold = (std::min)(static_cast<uint32_t>(std::stoul(h->second)), kLinkMaxHoldMs);
                }
                const auto s = r.query.find("session");
                const std::wstring session = s == r.query.end() ? std::wstring{} : detail::Utf8ToWide(s->second);
                std::vector<json::Value> commands;
                hub.Poll(token, session, now, commands);
                if (!commands.empty() || hold == 0)
                {
                    hub.SetParked(token, false, hold, now);
                    lk.unlock();
                    Respond(c, 200, "OK", LinkJsonBody(LinkPollAnswer(commands)), r.keepAlive);
                    return;
                }
                hub.SetParked(token, true, hold, now);
                lk.unlock();
                // One parked poll per link: an older one still held (a reconnect raced its answer) is answered empty first, so
                // the mod never waits on a poll the server forgot.
                for (auto& other : conns)
                {
                    if (&other != &c && other.parked && other.parkedToken == token)
                    {
                        other.parked = false;
                        Respond(other, 200, "OK", LinkJsonBody(LinkPollAnswer({})), other.keepAlive);
                    }
                }
                c.parked = true;
                c.parkedToken = token;
                c.holdMs = hold;
                c.parkedDeadline = now + hold;
                c.keepAlive = r.keepAlive;
                return;
            }
            else if (r.method == "POST" && r.path == "/v1/results")
            {
                const auto body = json::Parse(detail::Utf8ToWide(r.body));
                size_t taken = 0;
                if (body && body->type == json::Value::Type::Obj)
                {
                    if (const auto* arr = body->Find(L"results"); arr && arr->type == json::Value::Type::Arr)
                    {
                        for (const auto& result : arr->arr)
                        {
                            const auto w = waiters.find(result.StrAt(L"id"));
                            // Only the link the command went to may answer it: a token cannot complete another link's waiter.
                            if (w != waiters.end() && w->second.token == token && !w->second.done)
                            {
                                w->second.done = true;
                                w->second.result = result;
                                ++taken;
                            }
                        }
                    }
                }
                hub.Seen(token, now);
                lk.unlock();
                cv.notify_all();
                auto reply = json::Value::MkObj();
                reply.Set(L"ok", json::Value::MkBool(true));
                reply.Set(L"taken", json::Value::MkNum(static_cast<double>(taken)));
                Respond(c, 200, "OK", LinkJsonBody(reply), r.keepAlive);
                return;
            }
            else
            {
                lk.unlock();
                Respond(c, 404, "Not Found", ErrorBody("no such route"), r.keepAlive);
                return;
            }
        }
        if (!logLine.empty())
        {
            LogLink(logLine);
        }
    }

    void SessionLinkServer::Core::ServeParked(int64_t now)
    {
        for (auto& c : conns)
        {
            if (!c.parked || c.s == INVALID_SOCKET)
            {
                continue;
            }
            std::vector<json::Value> commands;
            bool answer = false;
            {
                std::lock_guard lk{ mtx };
                if (hub.HasQueued(c.parkedToken))
                {
                    hub.Poll(c.parkedToken, L"", now, commands);
                    answer = true;
                }
                else if (now >= c.parkedDeadline)
                {
                    answer = true; // the hold ran out: an empty answer, and the mod polls again at once
                }
                if (answer)
                {
                    hub.SetParked(c.parkedToken, false, c.holdMs, now);
                }
            }
            if (answer)
            {
                c.parked = false;
                Respond(c, 200, "OK", LinkJsonBody(LinkPollAnswer(commands)), c.keepAlive);
            }
        }
    }

    void SessionLinkServer::Core::Loop() noexcept
    {
        try
        {
            std::vector<WSAPOLLFD> fds;
            while (!stopping.load())
            {
                // The poll set: the listener, the wake socket, then every connection (writable only while it has output).
                fds.clear();
                fds.push_back(WSAPOLLFD{ listener, POLLRDNORM, 0 });
                fds.push_back(WSAPOLLFD{ wakeRecv, POLLRDNORM, 0 });
                int64_t now = TickMs();
                int64_t nextDeadline = now + 1000; // at least once a second: idle sweeps and the hub's forgetting
                for (const auto& c : conns)
                {
                    SHORT events = POLLRDNORM;
                    if (!c.out.empty())
                    {
                        events |= POLLWRNORM;
                    }
                    fds.push_back(WSAPOLLFD{ c.s, events, 0 });
                    if (c.parked)
                    {
                        nextDeadline = (std::min)(nextDeadline, c.parkedDeadline);
                    }
                }
                // Explicit template arguments need the unparenthesized names (NOMINMAX keeps windows.h's macros away).
                const int timeout = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(nextDeadline - now, 1000)));
                const int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout);
                if (stopping.load())
                {
                    break;
                }
                if (ready == SOCKET_ERROR)
                {
                    // WSAPoll failed as a whole (not expected): a short pause instead of a spin, then the set is rebuilt.
                    LogLink(L"WSAPoll failed (" + std::to_wstring(::WSAGetLastError()) + L"); retrying");
                    ::Sleep(50);
                    continue;
                }
                now = TickMs();
                if (fds[1].revents & (POLLRDNORM | POLLERR | POLLHUP))
                {
                    char drain[256];
                    while (::recv(wakeRecv, drain, sizeof(drain), 0) > 0)
                    {
                    }
                }
                // Each connection by its index in the set (built from `conns` above, in order): reads and writes first, closes
                // after, so an index never shifts while the set is walked.
                for (size_t i = 0; i < conns.size(); ++i)
                {
                    auto& c = conns[i];
                    const SHORT re = fds[2 + i].revents;
                    if (re & (POLLERR | POLLNVAL))
                    {
                        Close(c);
                        continue;
                    }
                    if (re & (POLLRDNORM | POLLHUP))
                    {
                        if (!Receive(c))
                        {
                            Close(c);
                            continue;
                        }
                    }
                }
                ServeParked(now);
                // Write what is pending (a socket that takes less than all keeps the rest for its POLLWRNORM).
                for (auto& c : conns)
                {
                    while (c.s != INVALID_SOCKET && !c.out.empty())
                    {
                        const int sent = ::send(c.s, c.out.data(), static_cast<int>(std::min<size_t>(c.out.size(), 1 << 20)), 0);
                        if (sent == SOCKET_ERROR)
                        {
                            if (::WSAGetLastError() != WSAEWOULDBLOCK)
                            {
                                Close(c);
                            }
                            break;
                        }
                        c.out.erase(0, static_cast<size_t>(sent));
                        c.lastActivity = now;
                    }
                    if (c.s != INVALID_SOCKET && c.out.empty() && c.closeAfterWrite)
                    {
                        Close(c);
                    }
                    else if (c.s != INVALID_SOCKET && !c.parked && c.out.empty() && now - c.lastActivity > kIdleConnectionMs)
                    {
                        Close(c);
                    }
                }
                conns.erase(std::remove_if(conns.begin(), conns.end(), [](const LinkConnection& c) { return c.s == INVALID_SOCKET; }), conns.end());
                // New connections last, so the indexes above matched the set the poll was built from.
                if (fds[0].revents & POLLRDNORM)
                {
                    for (;;)
                    {
                        const SOCKET s = ::accept(listener, nullptr, nullptr);
                        if (s == INVALID_SOCKET)
                        {
                            break; // WSAEWOULDBLOCK: no more waiting
                        }
                        if (conns.size() >= kMaxConnections || !SetNonBlocking(s))
                        {
                            ::closesocket(s);
                            continue;
                        }
                        LinkConnection c;
                        c.s = s;
                        c.lastActivity = now;
                        conns.push_back(std::move(c));
                    }
                }
                if (now - lastForget > kForgetEveryMs)
                {
                    lastForget = now;
                    size_t forgotten = 0;
                    {
                        std::lock_guard lk{ mtx };
                        forgotten = hub.Forget(now, kForgetNeverLinkedMs, kForgetGoneMs);
                    }
                    if (forgotten)
                    {
                        cv.notify_all();
                    }
                }
                // A link judged lost (its poll's connection closed, its grace ran out) wakes the waiters so they can give up.
                cv.notify_all();
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the loop ends; the waiters see `running` false through Stop's notify, and launches stop getting
            // a link (LaunchEnv checks the loop). The log names the throw site.
            LogSwallowedException(L"SessionLink Loop");
            LogLink(L"the loop ended on an exception: new launches get no link until Agentmaster restarts");
            try
            {
                std::lock_guard lk{ mtx };
                running = false;
            }
            catch (...)
            {
                LogSwallowedException(L"SessionLink Loop (mark stopped)");
            }
            cv.notify_all();
        }
        for (auto& c : conns)
        {
            Close(c);
        }
        conns.clear();
    }

    SessionLinkServer::SessionLinkServer() :
        _core{ std::make_shared<Core>() }
    {
    }

    SessionLinkServer::~SessionLinkServer()
    {
        Stop();
    }

    bool SessionLinkServer::Start(std::wstring& error) noexcept
    {
        error.clear();
        try
        {
            std::lock_guard lk{ _core->mtx };
            if (_core->running)
            {
                return true;
            }
            WSADATA wsa{};
            if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            {
                error = L"Winsock did not start (" + std::to_wstring(::WSAGetLastError()) + L")";
                LogLink(L"not started: " + error);
                return false;
            }
            _core->wsaStarted = true;
            // Closes whatever this attempt opened when it fails half-way (each socket is set only once it exists).
            const auto fail = [&](const std::wstring& why) {
                error = why + L" (" + std::to_wstring(::WSAGetLastError()) + L")";
                for (SOCKET* s : { &_core->listener, &_core->wakeRecv, &_core->wakeSend })
                {
                    if (*s != INVALID_SOCKET)
                    {
                        ::closesocket(*s);
                        *s = INVALID_SOCKET;
                    }
                }
                ::WSACleanup();
                _core->wsaStarted = false;
                LogLink(L"not started: " + error);
                return false;
            };
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only: nothing on another interface, no firewall prompt
            addr.sin_port = 0; // an ephemeral port: two Agentmaster instances (release and dev) never collide
            _core->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_core->listener == INVALID_SOCKET)
            {
                return fail(L"no listening socket");
            }
            // Exclusive: no other process can bind the same address and steal the mods' requests.
            BOOL exclusive = TRUE;
            ::setsockopt(_core->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            if (::bind(_core->listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(_core->listener, SOMAXCONN) != 0)
            {
                return fail(L"could not listen on 127.0.0.1");
            }
            sockaddr_in bound{};
            int boundLen = sizeof(bound);
            ::getsockname(_core->listener, reinterpret_cast<sockaddr*>(&bound), &boundLen);
            const uint16_t port = ntohs(bound.sin_port);
            // The wake pair: a loopback connection to the listener itself, accepted as the loop's wake socket. A byte written
            // to the client end makes WSAPoll return (a command was queued, the server stops).
            _core->wakeSend = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_core->wakeSend == INVALID_SOCKET || ::connect(_core->wakeSend, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) != 0)
            {
                return fail(L"could not make the wake connection");
            }
            _core->wakeRecv = ::accept(_core->listener, nullptr, nullptr);
            if (_core->wakeRecv == INVALID_SOCKET || !SetNonBlocking(_core->wakeRecv) || !SetNonBlocking(_core->listener))
            {
                return fail(L"could not accept the wake connection");
            }
            BOOL noDelay = TRUE;
            ::setsockopt(_core->wakeSend, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            _core->endpoint = L"http://127.0.0.1:" + std::to_wstring(port);
            _core->stopping.store(false);
            _core->lastForget = TickMs();
            _core->running = true;
            // The thread holds its own reference: a Stop racing the app's end never frees the state under it.
            _core->loop = std::thread([core = _core]() noexcept { core->Loop(); });
            LogLink(L"listening on 127.0.0.1:" + std::to_wstring(port) + L" (protocol " + std::to_wstring(kLinkProtocol) + L")");
            return true;
        }
        catch (...)
        {
            // std::system_error (the thread or the lock), std::bad_alloc: no link this run; launches get no AM_LINK.
            LogSwallowedException(L"SessionLinkServer::Start");
            if (error.empty())
            {
                error = L"it failed (hooks.log says why)";
            }
            return false;
        }
    }

    void SessionLinkServer::Stop() noexcept
    {
        try
        {
            std::thread loop;
            {
                std::lock_guard lk{ _core->mtx };
                if (!_core->running)
                {
                    return;
                }
                _core->running = false;
                _core->stopping.store(true);
                _core->WakeLocked();
                loop = std::move(_core->loop);
            }
            _core->cv.notify_all();
            if (loop.joinable())
            {
                if (loop.get_id() == std::this_thread::get_id())
                {
                    loop.detach(); // never join yourself (a Stop from inside the loop is not expected)
                }
                else
                {
                    loop.join();
                }
            }
            std::lock_guard lk{ _core->mtx };
            for (SOCKET* s : { &_core->listener, &_core->wakeRecv, &_core->wakeSend })
            {
                if (*s != INVALID_SOCKET)
                {
                    ::closesocket(*s);
                    *s = INVALID_SOCKET;
                }
            }
            _core->endpoint.clear();
            if (_core->wsaStarted)
            {
                ::WSACleanup();
                _core->wsaStarted = false;
            }
            LogLink(L"stopped");
        }
        catch (...)
        {
            // Rule #18. Recovery: whatever was not closed stays open until the process ends.
            LogSwallowedException(L"SessionLinkServer::Stop");
        }
    }

    bool SessionLinkServer::Running() const noexcept
    {
        try
        {
            std::lock_guard lk{ _core->mtx };
            return _core->running;
        }
        catch (...)
        {
            LogSwallowedException(L"SessionLinkServer::Running");
            return false;
        }
    }

    std::vector<std::pair<std::wstring, std::wstring>> SessionLinkServer::LaunchEnv(const std::wstring& sessionId) noexcept
    {
        try
        {
            const std::wstring token = MintToken();
            if (token.empty())
            {
                LogLink(L"no token for " + ShortId(sessionId) + L" (the system gave no random bytes): this launch has no link");
                return {};
            }
            std::lock_guard lk{ _core->mtx };
            if (!_core->running)
            {
                return {};
            }
            _core->hub.RegisterLaunch(token, sessionId, TickMs());
            return { { std::wstring{ kLinkEndpointEnv }, _core->endpoint }, { std::wstring{ kLinkTokenEnv }, token } };
        }
        catch (...)
        {
            // std::bad_alloc, std::system_error: the launch goes on without a link (its mod stays dormant; the headless way serves it).
            LogSwallowedException(L"SessionLinkServer::LaunchEnv");
            return {};
        }
    }

    bool SessionLinkServer::HasLiveLink(const std::wstring& sessionId) const noexcept
    {
        try
        {
            std::lock_guard lk{ _core->mtx };
            return _core->running && !_core->hub.LiveTokenFor(sessionId, TickMs()).empty();
        }
        catch (...)
        {
            LogSwallowedException(L"SessionLinkServer::HasLiveLink");
            return false;
        }
    }

    LinkRunResult SessionLinkServer::Run(const std::wstring& sessionId, json::Value command, uint32_t timeoutMs) noexcept
    {
        LinkRunResult out;
        const int64_t started = TickMs();
        std::wstring id;
        try
        {
            std::unique_lock lk{ _core->mtx };
            if (!_core->running)
            {
                out.outcome = LinkRunOutcome::Stopped;
                return out;
            }
            id = L"c" + std::to_wstring(_core->nextCommand++);
            command.Set(L"id", json::Value::MkStr(id));
            const std::wstring token = _core->hub.Enqueue(sessionId, std::move(command), started);
            if (token.empty())
            {
                out.outcome = LinkRunOutcome::NoLink;
                return out;
            }
            _core->waiters[id] = LinkWaiter{ token, false, json::Value{} };
            _core->WakeLocked();
            const int64_t deadline = started + timeoutMs;
            for (;;)
            {
                auto& w = _core->waiters[id];
                if (w.done)
                {
                    out.outcome = LinkRunOutcome::Answered;
                    out.result = std::move(w.result);
                    break;
                }
                const int64_t now = TickMs();
                if (!_core->running)
                {
                    out.outcome = LinkRunOutcome::Stopped;
                    break;
                }
                if (!_core->hub.IsLive(token, now))
                {
                    // The link stopped polling: its claude ended, hung or lost the module. A command still queued never left.
                    _core->hub.Withdraw(token, id);
                    out.outcome = LinkRunOutcome::Lost;
                    break;
                }
                if (now >= deadline)
                {
                    _core->hub.Withdraw(token, id);
                    out.outcome = LinkRunOutcome::TimedOut;
                    break;
                }
                // Woken by a result, a judged link or the loop's once-a-second turn; a second at most between looks.
                _core->cv.wait_for(lk, std::chrono::milliseconds(std::min<int64_t>(deadline - now, 1000)));
            }
            _core->waiters.erase(id);
        }
        catch (...)
        {
            // std::system_error, std::bad_alloc: the command reads as lost; its waiter (if made) is left for the loop's
            // stopping to clear — at worst one stale entry per failure.
            LogSwallowedException(L"SessionLinkServer::Run");
            out.outcome = LinkRunOutcome::Lost;
        }
        out.tookMs = TickMs() - started;
        return out;
    }

    std::pair<size_t, size_t> SessionLinkServer::Counts() const noexcept
    {
        try
        {
            std::lock_guard lk{ _core->mtx };
            return _core->hub.Counts(TickMs());
        }
        catch (...)
        {
            LogSwallowedException(L"SessionLinkServer::Counts");
            return { 0, 0 };
        }
    }

    SessionLinkServer& SharedSessionLink()
    {
        // Never destroyed: a btw worker may still wait in Run when the app ends (the app quits with TerminateProcess anyway).
        static SessionLinkServer* const server = new SessionLinkServer();
        return *server;
    }

    std::vector<std::pair<std::wstring, std::wstring>> SessionLinkLaunchEnv(const std::wstring& sessionId) noexcept
    {
        try
        {
            return SharedSessionLink().LaunchEnv(sessionId);
        }
        catch (...)
        {
            // std::bad_alloc on the first call (the server could not be made): no link for this launch.
            LogSwallowedException(L"SessionLinkLaunchEnv");
            return {};
        }
    }
}
