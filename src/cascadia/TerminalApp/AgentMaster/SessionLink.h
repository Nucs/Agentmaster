// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the SESSION LINK: Agentmaster ⇄ the agentmaster-link mod inside each Claude Code session it launches
// (MOD_LINK.md; its first slice, built for MESSAGING.md §21.56's /btw).
//
// The mod runs inside claude.exe and has no listener of its own (a mod reaches out only through `$.http.fetch`,
// MOD_LINK.md F12), so the link is a small HTTP/1.1 server in Agentmaster that every linked session POLLS:
//
//   claude.exe ── agentmaster-link mod ── GET /v1/commands?hold=5000&session=<id>   (held: answered the moment a command
//                                     │                                              is queued, else empty after 5 s)
//                                     ├── POST /v1/hello   {protocol, mod, claude, session}
//                                     └── POST /v1/results {results:[{id, …}]}       (a command's outcome)
//   Agentmaster ── SessionLinkServer (127.0.0.1:<ephemeral port>, one per process) ── LinkHub (this header: the tokens,
//                  the links, what each one has queued, whether it is live)
//
//   * Every managed Claude launch carries AM_LINK=http://127.0.0.1:<port> and AM_LINK_TOKEN=<128-bit random hex> in its
//     environment (ClaudeSpawn's link env provider → SessionLinkLaunchEnv): the token is minted per launch and registered
//     here with the launch's session id, never logged, persisted or put on a command line. Every request carries it in
//     `x-am-token`; an unknown token gets 404 and the mod stops.
//   * The poll is the link's heartbeat: a link is LIVE while a poll is parked or one was answered within the hold plus a
//     grace (kLinkLiveGraceMs). The mod names its CURRENT conversation in every poll (`session`): a /clear or an in-session
//     /resume moves the link to the new id with no message of its own.
//   * Commands are JSON objects with an "id" and a "kind". This slice has one kind, "fork" ({prompt, effort?}: the mod runs
//     Claude Code's `$.model.fork` and posts its result); MOD_LINK.md's submit, fill and abort come later.
//   * 127.0.0.1 only (no firewall prompt, nothing on another interface). TCP rather than MOD_LINK.md's AF_UNIX socket:
//     loopback TCP needs no path-length fallback and measured 3–4 ms a request from a mod (F1); the socket file is a later
//     step of the design.
//
// This header: the pure part (the HTTP request reader, the response writer, LinkHub) — tested by the m5 harness
// (tests_side_question.cpp) — and the declarations of the OS part (SessionLink.cpp: the Winsock loop, the waiters).

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Json.h"
#include "MessagingFileLinks.h" // Msg::HttpRequestHead + Msg::ParseHttpRequestHead (the strict request-head reader)

namespace Agentmaster::Link
{
    // ===== the protocol's constants ==========================================================================================

    /// <summary>The protocol this build speaks (POST /v1/hello says it; a mod of another protocol is answered 409).</summary>
    inline constexpr int kLinkProtocol = 1;
    /// <summary>
    /// How long a poll is held by default (ms): 5 s, MOD_LINK.md's choice — an open fetch holds a headless claude's exit
    /// until it is answered (F9), so the hold is short and costs ~one tiny request per 5 s per session.
    /// </summary>
    inline constexpr uint32_t kLinkDefaultHoldMs = 5000;
    /// <summary>The longest hold a mod may ask for (ms); a longer `hold` is cut to it.</summary>
    inline constexpr uint32_t kLinkMaxHoldMs = 25000;
    /// <summary>How long after its last poll was answered a link still reads live, beyond its hold (the mod's next poll
    /// is ~ms behind; a missed one means the claude ended, hung or lost its module).</summary>
    inline constexpr int64_t kLinkLiveGraceMs = 3000;
    /// <summary>The largest request head read (bytes); a longer one is a 431.</summary>
    inline constexpr size_t kLinkMaxHeadBytes = 16 * 1024;
    /// <summary>The largest request body read (bytes): a fork's result with a long answer fits many times over; more is a 413.</summary>
    inline constexpr size_t kLinkMaxBodyBytes = 8 * 1024 * 1024;
    /// <summary>The header every request carries its launch token in.</summary>
    inline constexpr std::string_view kLinkTokenHeader = "x-am-token";
    /// <summary>The environment variable a launch gets the link's endpoint in ("http://127.0.0.1:&lt;port&gt;").</summary>
    inline constexpr std::wstring_view kLinkEndpointEnv = L"AM_LINK";
    /// <summary>The environment variable a launch gets its token in (32 hex digits).</summary>
    inline constexpr std::wstring_view kLinkTokenEnv = L"AM_LINK_TOKEN";

    namespace detail
    {
        /// <summary>
        /// UTF-8 bytes as UTF-16 (a portable decoder: the m5 harness runs this header without Win32). A malformed sequence
        /// becomes U+FFFD and decoding goes on, so a bad byte never loses the rest of a body.
        /// </summary>
        /// <param name="s">The bytes.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring Utf8ToWide(std::string_view s)
        {
            std::wstring out;
            out.reserve(s.size());
            size_t i = 0;
            while (i < s.size())
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                uint32_t cp = 0xFFFD;
                size_t len = 1;
                if (c < 0x80)
                {
                    cp = c;
                }
                else if ((c & 0xE0) == 0xC0 || (c & 0xF0) == 0xE0 || (c & 0xF8) == 0xF0)
                {
                    len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4);
                    uint32_t v = c & (len == 2 ? 0x1Fu : (len == 3 ? 0x0Fu : 0x07u));
                    bool ok = i + len <= s.size();
                    for (size_t k = 1; ok && k < len; ++k)
                    {
                        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                        ok = (cc & 0xC0) == 0x80;
                        v = (v << 6) | (cc & 0x3Fu);
                    }
                    // Overlong forms, surrogates and values past U+10FFFF are malformed: one U+FFFD for the lead byte.
                    const uint32_t minimum = len == 2 ? 0x80u : (len == 3 ? 0x800u : 0x10000u);
                    if (ok && v >= minimum && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF))
                    {
                        cp = v;
                    }
                    else
                    {
                        len = 1;
                    }
                }
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                }
                else
                {
                    out.push_back(static_cast<wchar_t>(cp));
                }
                i += len;
            }
            return out;
        }

        /// <summary>UTF-16 as UTF-8 (portable; a lone surrogate becomes U+FFFD's bytes).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The bytes.</returns>
        /// <exception cref="std::bad_alloc">If the bytes cannot be allocated.</exception>
        inline std::string WideToUtf8(std::wstring_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                uint32_t cp = static_cast<uint16_t>(s[i]);
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && static_cast<uint16_t>(s[i + 1]) >= 0xDC00 && static_cast<uint16_t>(s[i + 1]) <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint16_t>(s[i + 1]) - 0xDC00);
                    ++i;
                }
                else if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                    cp = 0xFFFD;
                }
                if (cp < 0x80)
                {
                    out.push_back(static_cast<char>(cp));
                }
                else if (cp < 0x800)
                {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp < 0x10000)
                {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            return out;
        }

        /// <summary>A query value's %XX escapes and '+' (a space, as form encoding writes it) decoded; a bad escape stays as typed.</summary>
        /// <param name="s">The raw value.</param>
        /// <returns>The decoded bytes.</returns>
        /// <exception cref="std::bad_alloc">If the bytes cannot be allocated.</exception>
        inline std::string PercentDecode(std::string_view s)
        {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f')
                {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F')
                {
                    return c - 'A' + 10;
                }
                return -1;
            };
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                // Both hex digits must be there and valid; anything less is kept as typed (a lone '%' is data, not an error).
                if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0)
                {
                    out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
                    i += 2;
                }
                else
                {
                    out.push_back(s[i] == '+' ? ' ' : s[i]);
                }
            }
            return out;
        }

        /// <summary>ASCII lower case of a header name or a token (the protocol's names are ASCII).</summary>
        /// <param name="s">The text.</param>
        /// <returns>Its lower case.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        inline std::string LowerAscii(std::string_view s)
        {
            std::string out{ s };
            for (auto& c : out)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return out;
        }
    }

    // ===== reading a request, writing a response ===============================================================================

    /// <summary>One request read from a connection (TakeLinkHttpRequest).</summary>
    struct LinkHttpRequest
    {
        std::string method; // "GET", "POST"
        std::string path; // the target before '?' ("/v1/commands")
        std::map<std::string, std::string> query; // the query's arguments, decoded (a repeated name keeps its last value)
        std::string token; // the x-am-token header ("" when absent)
        std::string body; // the body (Content-Length bytes; "" without one)
        bool keepAlive{ true }; // HTTP/1.1 without "Connection: close" (HTTP/1.0 only with "keep-alive")
    };

    /// <summary>What TakeLinkHttpRequest found at the start of a connection's buffer.</summary>
    enum class LinkHttpTake
    {
        NeedMore, // the head or the body is not complete yet (nothing consumed)
        Request, // one request read and consumed from the buffer
        Bad, // malformed: answer 400 and close (the rest of the buffer cannot be trusted)
        TooLarge, // a head past kLinkMaxHeadBytes or a body past kLinkMaxBodyBytes: answer 413 and close
        LengthRequired, // a body with no Content-Length (chunked): answer 411 and close — the mod always sends a length
    };

    /// <summary>
    /// Reads ONE request from the start of a connection's receive buffer and, when it is complete, removes it from the buffer
    /// (pipelined requests stay for the next call). The head goes through the file-link server's strict reader
    /// (Msg::ParseHttpRequestHead); a body needs a Content-Length (Transfer-Encoding is refused, a mod's fetch sends a length).
    /// </summary>
    /// <param name="buffer">The connection's bytes so far (consumed on Request).</param>
    /// <param name="out">Receives the request on Request.</param>
    /// <returns>What was found.</returns>
    /// <exception cref="std::bad_alloc">If the request cannot be allocated.</exception>
    inline LinkHttpTake TakeLinkHttpRequest(std::string& buffer, LinkHttpRequest& out)
    {
        const size_t end = buffer.find("\r\n\r\n");
        if (end == std::string::npos)
        {
            return buffer.size() > kLinkMaxHeadBytes ? LinkHttpTake::TooLarge : LinkHttpTake::NeedMore;
        }
        if (end > kLinkMaxHeadBytes)
        {
            return LinkHttpTake::TooLarge;
        }
        const auto head = Msg::ParseHttpRequestHead(std::string_view{ buffer }.substr(0, end));
        if (!head)
        {
            return LinkHttpTake::Bad;
        }
        if (head->Header("Transfer-Encoding"))
        {
            return LinkHttpTake::LengthRequired;
        }
        size_t length = 0;
        if (const std::string* cl = head->Header("Content-Length"))
        {
            if (cl->empty() || cl->size() > 10 || !std::all_of(cl->begin(), cl->end(), [](char c) { return c >= '0' && c <= '9'; }))
            {
                return LinkHttpTake::Bad;
            }
            const unsigned long long n = std::stoull(*cl);
            if (n > kLinkMaxBodyBytes)
            {
                return LinkHttpTake::TooLarge;
            }
            length = static_cast<size_t>(n);
        }
        const size_t total = end + 4 + length;
        if (buffer.size() < total)
        {
            return LinkHttpTake::NeedMore;
        }
        LinkHttpRequest r;
        r.method = head->method;
        const size_t q = head->target.find('?');
        r.path = head->target.substr(0, q);
        if (q != std::string::npos)
        {
            std::string_view rest = std::string_view{ head->target }.substr(q + 1);
            while (!rest.empty())
            {
                const size_t amp = rest.find('&');
                const std::string_view pair = rest.substr(0, amp);
                const size_t eq = pair.find('=');
                const std::string name = detail::PercentDecode(pair.substr(0, eq));
                if (!name.empty())
                {
                    r.query[name] = eq == std::string_view::npos ? std::string{} : detail::PercentDecode(pair.substr(eq + 1));
                }
                if (amp == std::string_view::npos)
                {
                    break;
                }
                rest.remove_prefix(amp + 1);
            }
        }
        if (const std::string* t = head->Header(kLinkTokenHeader))
        {
            r.token = *t;
        }
        const std::string* connection = head->Header("Connection");
        const std::string conn = connection ? detail::LowerAscii(*connection) : std::string{};
        r.keepAlive = head->version == "HTTP/1.1" ? conn != "close" : conn == "keep-alive";
        r.body = buffer.substr(end + 4, length);
        buffer.erase(0, total);
        out = std::move(r);
        return LinkHttpTake::Request;
    }

    /// <summary>A complete HTTP/1.1 response with a JSON body (Content-Type application/json; charset=utf-8, its length, and the
    /// connection's fate).</summary>
    /// <param name="status">The status code (200, 400, 404, …).</param>
    /// <param name="reason">Its reason phrase ("OK").</param>
    /// <param name="jsonUtf8">The body, UTF-8 JSON ("" = an empty body).</param>
    /// <param name="keepAlive">Keep the connection for the next request (else "Connection: close").</param>
    /// <returns>The bytes to send.</returns>
    /// <exception cref="std::bad_alloc">If the bytes cannot be allocated.</exception>
    inline std::string BuildLinkHttpResponse(int status, std::string_view reason, std::string_view jsonUtf8, bool keepAlive)
    {
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string{ reason } + "\r\n";
        out += "Content-Type: application/json; charset=utf-8\r\n";
        out += "Content-Length: " + std::to_string(jsonUtf8.size()) + "\r\n";
        out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "Cache-Control: no-store\r\n\r\n";
        out.append(jsonUtf8);
        return out;
    }

    /// <summary>A JSON value as the UTF-8 body of a response (json::Dump, then UTF-8).</summary>
    /// <param name="v">The value.</param>
    /// <returns>The bytes.</returns>
    /// <exception cref="std::bad_alloc">If the bytes cannot be allocated.</exception>
    inline std::string LinkJsonBody(const json::Value& v)
    {
        return detail::WideToUtf8(json::Dump(v));
    }

    /// <summary>The body of a poll's answer: {"commands":[…]} (none: an empty list, the hold ran out).</summary>
    /// <param name="commands">The commands handed to the mod.</param>
    /// <returns>The JSON value.</returns>
    /// <exception cref="std::bad_alloc">If the value cannot be allocated.</exception>
    inline json::Value LinkPollAnswer(const std::vector<json::Value>& commands)
    {
        auto arr = json::Value::MkArr();
        for (const auto& c : commands)
        {
            arr.Push(c);
        }
        auto root = json::Value::MkObj();
        root.Set(L"commands", std::move(arr));
        return root;
    }

    // ===== the hub: tokens, links, queued commands, liveness ======================================================================

    /// <summary>What the hub knows of one link (LinkHub::Info): a snapshot for logs and the census.</summary>
    struct LinkInfo
    {
        std::wstring launchSessionId; // the session the launch was for
        std::wstring sessionId; // the conversation the mod named last (its hello or its newest poll)
        std::wstring modVersion; // the mod's version (its hello)
        std::wstring claudeVersion; // Claude Code's version (its hello)
        int64_t registeredAtMs{ 0 }; // when the launch registered its token
        int64_t helloAtMs{ 0 }; // when the mod said hello (0 = never: not linked yet)
        int64_t lastSeenAtMs{ 0 }; // the newest request of the mod (a poll answered, a hello, a result)
        bool pollParked{ false }; // a poll is held right now
        size_t queued{ 0 }; // commands waiting for its next poll
    };

    /// <summary>
    /// The link table: every launch's token, the conversation its mod names, the commands queued for it, and whether it is
    /// live. Pure (no sockets, no clock of its own: every call takes the time), single-threaded — SessionLinkServer guards it
    /// with its mutex. Tokens are 32 hex digits the server mints; the hub only compares them.
    /// </summary>
    class LinkHub
    {
    public:
        /// <summary>
        /// A launch's token, registered when its spawn is built (before the claude starts). A token registered again keeps
        /// its link but takes the new session id (a builder that runs twice for one launch).
        /// </summary>
        /// <param name="token">The token.</param>
        /// <param name="sessionId">The launch's session id.</param>
        /// <param name="nowMs">Now (ms, any monotonic or wall clock the caller uses throughout).</param>
        /// <exception cref="std::bad_alloc">If the record cannot be allocated.</exception>
        void RegisterLaunch(const std::wstring& token, const std::wstring& sessionId, int64_t nowMs)
        {
            auto& r = _links[token];
            r.info.launchSessionId = sessionId;
            if (r.info.sessionId.empty())
            {
                r.info.sessionId = sessionId;
            }
            r.info.registeredAtMs = nowMs;
        }

        /// <summary>Whether a token was registered (an unknown one is answered 404).</summary>
        /// <param name="token">The token.</param>
        /// <returns>True when registered.</returns>
        bool Knows(const std::wstring& token) const
        {
            return _links.find(token) != _links.end();
        }

        /// <summary>
        /// The mod's hello: its versions and the conversation it runs. The link becomes live (it is seen now).
        /// </summary>
        /// <param name="token">The token.</param>
        /// <param name="sessionId">The session the mod names ("" keeps the one known).</param>
        /// <param name="modVersion">The mod's version.</param>
        /// <param name="claudeVersion">Claude Code's version.</param>
        /// <param name="nowMs">Now.</param>
        /// <returns>False for an unknown token (nothing changes).</returns>
        /// <exception cref="std::bad_alloc">If the strings cannot be copied.</exception>
        bool Hello(const std::wstring& token, const std::wstring& sessionId, const std::wstring& modVersion, const std::wstring& claudeVersion, int64_t nowMs)
        {
            const auto it = _links.find(token);
            if (it == _links.end())
            {
                return false;
            }
            auto& i = it->second.info;
            if (!sessionId.empty())
            {
                i.sessionId = sessionId;
            }
            i.modVersion = modVersion;
            i.claudeVersion = claudeVersion;
            i.helloAtMs = nowMs;
            i.lastSeenAtMs = nowMs;
            return true;
        }

        /// <summary>
        /// A poll arrived: the link is seen now and names its current conversation; the commands queued for it are TAKEN into
        /// `commands` (the caller answers at once when it got any, else parks the poll and calls Park).
        /// </summary>
        /// <param name="token">The token.</param>
        /// <param name="sessionId">The session the mod names now ("" keeps the one known).</param>
        /// <param name="nowMs">Now.</param>
        /// <param name="commands">Receives the queued commands (appended).</param>
        /// <returns>False for an unknown token.</returns>
        /// <exception cref="std::bad_alloc">If the commands cannot be moved out.</exception>
        bool Poll(const std::wstring& token, const std::wstring& sessionId, int64_t nowMs, std::vector<json::Value>& commands)
        {
            const auto it = _links.find(token);
            if (it == _links.end())
            {
                return false;
            }
            auto& r = it->second;
            if (!sessionId.empty())
            {
                r.info.sessionId = sessionId;
            }
            r.info.lastSeenAtMs = nowMs;
            for (auto& c : r.queued)
            {
                commands.push_back(std::move(c));
            }
            r.queued.clear();
            return true;
        }

        /// <summary>
        /// A poll of `token` was parked (true) or answered (false): a parked poll keeps the link live however long it waits;
        /// an answered one leaves it live for the hold plus the grace (IsLive). Answering also counts as seeing it.
        /// </summary>
        /// <param name="token">The token.</param>
        /// <param name="parked">True: parked now; false: its answer was written.</param>
        /// <param name="holdMs">The hold the poll asked for (its next poll comes within it).</param>
        /// <param name="nowMs">Now.</param>
        void SetParked(const std::wstring& token, bool parked, uint32_t holdMs, int64_t nowMs) noexcept
        {
            const auto it = _links.find(token);
            if (it == _links.end())
            {
                return;
            }
            it->second.info.pollParked = parked;
            it->second.holdMs = holdMs;
            it->second.info.lastSeenAtMs = nowMs;
        }

        /// <summary>Notes any other request of the mod (a result): the link is seen now.</summary>
        /// <param name="token">The token.</param>
        /// <param name="nowMs">Now.</param>
        void Seen(const std::wstring& token, int64_t nowMs) noexcept
        {
            if (const auto it = _links.find(token); it != _links.end())
            {
                it->second.info.lastSeenAtMs = nowMs;
            }
        }

        /// <summary>
        /// Whether a token's link is LIVE: its mod said hello, and a poll is parked now or its last request is younger than
        /// its hold plus kLinkLiveGraceMs.
        /// </summary>
        /// <param name="token">The token.</param>
        /// <param name="nowMs">Now.</param>
        /// <returns>True when live.</returns>
        bool IsLive(const std::wstring& token, int64_t nowMs) const noexcept
        {
            const auto it = _links.find(token);
            return it != _links.end() && LiveRecord(it->second, nowMs);
        }

        /// <summary>
        /// The token of the live link that runs `sessionId` now ("" = none): the one seen most recently when two tabs run the
        /// same conversation (MOD_LINK.md §4.2).
        /// </summary>
        /// <param name="sessionId">The conversation.</param>
        /// <param name="nowMs">Now.</param>
        /// <returns>The token, or "".</returns>
        /// <exception cref="std::bad_alloc">If the token cannot be copied.</exception>
        std::wstring LiveTokenFor(const std::wstring& sessionId, int64_t nowMs) const
        {
            std::wstring best;
            int64_t bestSeen = -1;
            for (const auto& [token, r] : _links)
            {
                if (r.info.sessionId == sessionId && LiveRecord(r, nowMs) && r.info.lastSeenAtMs > bestSeen)
                {
                    best = token;
                    bestSeen = r.info.lastSeenAtMs;
                }
            }
            return best;
        }

        /// <summary>
        /// Queues a command for the live link of `sessionId`: it goes out with that link's next poll (or at once, when the
        /// caller then answers the parked one). Nothing is queued when no live link runs the session.
        /// </summary>
        /// <param name="sessionId">The conversation.</param>
        /// <param name="command">The command (an object with "id" and "kind").</param>
        /// <param name="nowMs">Now.</param>
        /// <returns>The token it went to, or "" when no live link runs the session.</returns>
        /// <exception cref="std::bad_alloc">If the command cannot be queued.</exception>
        std::wstring Enqueue(const std::wstring& sessionId, json::Value command, int64_t nowMs)
        {
            const std::wstring token = LiveTokenFor(sessionId, nowMs);
            if (!token.empty())
            {
                _links[token].queued.push_back(std::move(command));
            }
            return token;
        }

        /// <summary>Whether a token has commands waiting (a parked poll is then answered at once).</summary>
        /// <param name="token">The token.</param>
        /// <returns>True when something is queued.</returns>
        bool HasQueued(const std::wstring& token) const noexcept
        {
            const auto it = _links.find(token);
            return it != _links.end() && !it->second.queued.empty();
        }

        /// <summary>
        /// Takes back a command not yet handed to the mod (its waiter gave up), by its "id".
        /// </summary>
        /// <param name="token">The token it was queued for.</param>
        /// <param name="commandId">The command's id.</param>
        /// <returns>True when it was still queued (it is gone now); false when it was already delivered or unknown.</returns>
        bool Withdraw(const std::wstring& token, const std::wstring& commandId)
        {
            const auto it = _links.find(token);
            if (it == _links.end())
            {
                return false;
            }
            auto& q = it->second.queued;
            const auto c = std::find_if(q.begin(), q.end(), [&](const json::Value& v) { return v.StrAt(L"id") == commandId; });
            if (c == q.end())
            {
                return false;
            }
            q.erase(c);
            return true;
        }

        /// <summary>A snapshot of one link (for a log line); an unknown token gives an empty one.</summary>
        /// <param name="token">The token.</param>
        /// <returns>The link's facts.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        LinkInfo Info(const std::wstring& token) const
        {
            const auto it = _links.find(token);
            if (it == _links.end())
            {
                return {};
            }
            LinkInfo i = it->second.info;
            i.queued = it->second.queued.size();
            return i;
        }

        /// <summary>How many tokens are registered, and how many of their links are live now.</summary>
        /// <param name="nowMs">Now.</param>
        /// <returns>{registered, live}.</returns>
        std::pair<size_t, size_t> Counts(int64_t nowMs) const noexcept
        {
            size_t live = 0;
            for (const auto& [token, r] : _links)
            {
                live += LiveRecord(r, nowMs) ? 1 : 0;
            }
            return { _links.size(), live };
        }

        /// <summary>
        /// Forgets what can no longer link: a token whose mod never said hello `neverLinkedMs` after its registration (a
        /// launch without the mod, a claude that never started), and a link not seen for `goneMs` (its claude ended). Their
        /// queued commands go with them (their waiters time out on their own).
        /// </summary>
        /// <param name="nowMs">Now.</param>
        /// <param name="neverLinkedMs">How long an unlinked registration is kept.</param>
        /// <param name="goneMs">How long a silent link is kept.</param>
        /// <returns>How many tokens were forgotten.</returns>
        size_t Forget(int64_t nowMs, int64_t neverLinkedMs, int64_t goneMs)
        {
            size_t n = 0;
            for (auto it = _links.begin(); it != _links.end();)
            {
                const auto& i = it->second.info;
                const bool gone = i.helloAtMs == 0 ? nowMs - i.registeredAtMs > neverLinkedMs : (!i.pollParked && nowMs - i.lastSeenAtMs > goneMs);
                if (gone)
                {
                    it = _links.erase(it);
                    ++n;
                }
                else
                {
                    ++it;
                }
            }
            return n;
        }

    private:
        /// <summary>One token's link.</summary>
        struct Record
        {
            LinkInfo info; // what is known of it
            std::vector<json::Value> queued; // commands for its next poll
            uint32_t holdMs{ kLinkDefaultHoldMs }; // the hold its last poll asked for (its liveness window)
        };

        /// <summary>The liveness rule (IsLive): said hello, and parked now or seen within its hold plus the grace.</summary>
        /// <param name="r">The record.</param>
        /// <param name="nowMs">Now.</param>
        /// <returns>True when live.</returns>
        static bool LiveRecord(const Record& r, int64_t nowMs) noexcept
        {
            if (r.info.helloAtMs == 0)
            {
                return false;
            }
            return r.info.pollParked || nowMs - r.info.lastSeenAtMs <= static_cast<int64_t>(r.holdMs) + kLinkLiveGraceMs;
        }

        std::map<std::wstring, Record> _links; // token → its link
    };

    // ===== the OS part (SessionLink.cpp) ===========================================================================================

    /// <summary>How a command sent through the link ended (SessionLinkServer::Run).</summary>
    enum class LinkRunOutcome
    {
        Answered, // the mod posted the command's result (in `result`)
        NoLink, // no live link runs the session (nothing was sent)
        Lost, // the link stopped polling before the result came (its claude ended, hung or unloaded the mod)
        TimedOut, // no result within the time given (a command still queued was taken back)
        Stopped, // the server stopped while it waited
    };

    /// <summary>The outcome of SessionLinkServer::Run.</summary>
    struct LinkRunResult
    {
        LinkRunOutcome outcome{ LinkRunOutcome::NoLink }; // how it ended
        json::Value result; // the mod's result object (Answered); null otherwise
        int64_t tookMs{ 0 }; // from the send to the end
    };

    /// <summary>The word a log line uses for a LinkRunOutcome.</summary>
    /// <param name="o">The outcome.</param>
    /// <returns>"answered", "no-link", "lost", "timed-out" or "stopped" (a static string).</returns>
    inline const wchar_t* LinkRunOutcomeWord(LinkRunOutcome o) noexcept
    {
        switch (o)
        {
        case LinkRunOutcome::Answered:
            return L"answered";
        case LinkRunOutcome::NoLink:
            return L"no-link";
        case LinkRunOutcome::Lost:
            return L"lost";
        case LinkRunOutcome::TimedOut:
            return L"timed-out";
        default:
            return L"stopped";
        }
    }

    /// <summary>
    /// The link's server (SessionLink.cpp): one Winsock loop on 127.0.0.1:&lt;ephemeral port&gt; that answers the mods' hello,
    /// polls and results, and blocking senders (Run) that wait for a command's result. One per process (SharedSessionLink);
    /// thread-safe. Start once at engine init; every managed launch then gets its token and endpoint (LaunchEnv).
    /// </summary>
    class SessionLinkServer
    {
    public:
        SessionLinkServer();
        /// <summary>Stop(). Never throws.</summary>
        ~SessionLinkServer();
        SessionLinkServer(const SessionLinkServer&) = delete;
        SessionLinkServer& operator=(const SessionLinkServer&) = delete;

        /// <summary>
        /// Binds 127.0.0.1 on an ephemeral port and starts the loop (idempotent: a running server only answers true). Logs
        /// `[link] listening on 127.0.0.1:&lt;port&gt;` or why it could not.
        /// </summary>
        /// <param name="error">Receives why it could not start ("" on success).</param>
        /// <returns>True when the server runs.</returns>
        bool Start(std::wstring& error) noexcept;

        /// <summary>Stops the loop, closes every connection and wakes every waiting Run (Stopped). Never throws.</summary>
        void Stop() noexcept;

        /// <summary>Whether the loop runs.</summary>
        /// <returns>True while it runs.</returns>
        bool Running() const noexcept;

        /// <summary>
        /// The environment a managed launch of `sessionId` gets: AM_LINK (the endpoint) and AM_LINK_TOKEN (a new token,
        /// registered here for that session). Empty while the server is not running (the launch then has no link and its
        /// mod stays dormant). Never throws (a failure is logged and gives no link).
        /// </summary>
        /// <param name="sessionId">The launch's session id.</param>
        /// <returns>The two variables, or none.</returns>
        std::vector<std::pair<std::wstring, std::wstring>> LaunchEnv(const std::wstring& sessionId) noexcept;

        /// <summary>Whether a live link runs `sessionId` now (its mod polls). Never throws.</summary>
        /// <param name="sessionId">The conversation.</param>
        /// <returns>True when live.</returns>
        bool HasLiveLink(const std::wstring& sessionId) const noexcept;

        /// <summary>
        /// Sends one command to the live link of `sessionId` and WAITS for its result: NoLink at once when none is live;
        /// Lost when the link stops polling before the result came; TimedOut after `timeoutMs` (a command still queued is
        /// taken back); Stopped when the server stops. Blocks: call it from a worker thread, never a UI or poll thread.
        /// </summary>
        /// <param name="sessionId">The conversation.</param>
        /// <param name="command">The command (an object with a "kind"; Run sets its "id").</param>
        /// <param name="timeoutMs">How long to wait for the result.</param>
        /// <returns>How it ended, with the mod's result when it answered.</returns>
        LinkRunResult Run(const std::wstring& sessionId, json::Value command, uint32_t timeoutMs) noexcept;

        /// <summary>The registered tokens and the live links now (for log lines). Never throws.</summary>
        /// <returns>{registered, live}.</returns>
        std::pair<size_t, size_t> Counts() const noexcept;

        struct Core; // the loop's state (SessionLink.cpp)

    private:
        // Made in the constructor and never replaced, so no caller ever races a pointer swap; Start and Stop run the loop
        // inside it under its own lock. The loop thread holds a reference of its own while it runs.
        std::shared_ptr<Core> _core;
    };

    /// <summary>
    /// The process's ONE link server (created on first use, never destroyed: a waiting worker may still hold it when the app
    /// quits).
    /// </summary>
    /// <returns>The server.</returns>
    /// <exception cref="std::bad_alloc">On the first call only, if it cannot be allocated.</exception>
    SessionLinkServer& SharedSessionLink();

    /// <summary>
    /// The launch env provider ClaudeSpawn calls for every managed Claude launch (SetSessionLinkEnvProvider): the shared
    /// server's LaunchEnv. Set once per process by the app; every other program that builds spawns leaves it unset.
    /// </summary>
    /// <param name="sessionId">The launch's session id.</param>
    /// <returns>AM_LINK + AM_LINK_TOKEN, or none.</returns>
    std::vector<std::pair<std::wstring, std::wstring>> SessionLinkLaunchEnv(const std::wstring& sessionId) noexcept;
}
