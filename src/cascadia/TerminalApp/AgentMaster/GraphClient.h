// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — GraphClient: the ONE HTTPS client of the bot-less Teams connector (TEAMS_CHANNEL.md §11).
//
// It moves bytes and nothing else: a Graph call (a method, a path or a link Graph handed back, a bearer token, a JSON or
// binary body) or a sign-in form in, the HTTP status, Retry-After, the refusal and the parsed body out. Every rule — which
// token, pacing, what an answer means — lives in TeamsService and TeamsAuth. The WinHTTP pattern is TelegramApi's: one
// session per call, a request that another thread can ABORT (Stop must never wait for a slow call), and Shutdown, after
// which every call is refused without touching the network.
//
// A bearer token travels to graph.microsoft.com ONLY: every Graph address goes through TeamsModels.h GraphRequestPath, which
// refuses any other host (a link read from an answer included). The sign-in forms go to login.microsoftonline.com and carry
// no bearer token. No token, refresh token or device code is ever logged; a log line names the call's purpose ("legend
// post"), the status and Graph's error code and message.
//
// Plain C++ + Win32 (no WinRT, no PCH); links winhttp through its #pragma in the .cpp. The m5 harness compiles it (a LINK
// dependency of TeamsService.cpp, which Engine.cpp creates) but never calls the network.

#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>

#include "Json.h"

namespace Agentmaster::Teams
{
    /// <summary>One HTTP answer from Microsoft Graph or the Microsoft identity platform.</summary>
    struct GraphResult
    {
        /// <summary>The HTTP status (0 when no response came back).</summary>
        int httpStatus{ 0 };
        /// <summary>No HTTP response at all (DNS, TLS, a timeout, an abort, a refused address, the client shut down).</summary>
        bool transportFailed{ false };
        /// <summary>The call ended because Abort or Shutdown closed it: the service is stopping, so nobody retries it.</summary>
        bool aborted{ false };
        /// <summary>
        /// The client refused the call without sending anything: not a Graph address, a tenant that is no tenant word, an empty
        /// token, or the client is shut down. A retry cannot help (Retryable is false); the caller's input is wrong or it stops.
        /// </summary>
        bool refused{ false };
        /// <summary>The answer's Retry-After header in seconds (a 429 or a 503; 0 = none, or a date this client does not read).</summary>
        int retryAfterSec{ 0 };
        /// <summary>The refusal's code: Graph's `error.code`, or the sign-in's `error` ("" for a success or a transport failure).</summary>
        std::wstring errorCode;
        /// <summary>The refusal's message, or the transport failure's note; one line.</summary>
        std::wstring description;
        /// <summary>The parsed body (Null for an empty body, a 204, or text that is not JSON).</summary>
        json::Value body;
        /// <summary>The body's size in bytes, for the log.</summary>
        size_t bytes{ 0 };
        /// <summary>How long the call took (ms).</summary>
        int64_t tookMs{ 0 };

        /// <summary>A 2xx answer.</summary>
        /// <returns>True for a success.</returns>
        bool Ok() const noexcept
        {
            return !transportFailed && httpStatus >= 200 && httpStatus < 300;
        }

        /// <summary>
        /// A failure the same call may still win by trying again later: no answer (but not an abort — the service is stopping —
        /// and not a refusal of the client's own), a 429, or a 5xx. A 4xx other than 429 is the caller's to judge (a 401 renews
        /// the token, a 403 asks the router for another source).
        /// </summary>
        /// <returns>True when a retry can help.</returns>
        bool Retryable() const noexcept
        {
            return (transportFailed && !aborted && !refused) || httpStatus == 429 || httpStatus >= 500;
        }
    };

    /// <summary>The Teams connector's HTTPS client (see the file header). Thread-safe: the service's threads share one client.</summary>
    class GraphClient
    {
    public:
        /// <summary>Creates a client. Opens nothing until the first call.</summary>
        GraphClient() = default;
        /// <summary>Aborts every request in flight (their callers return "aborted"). Never throws.</summary>
        ~GraphClient();
        GraphClient(const GraphClient&) = delete;
        GraphClient& operator=(const GraphClient&) = delete;

        /// <summary>
        /// Makes one Graph call with a JSON body (or none). Blocks for the round trip; call it on a worker thread only, never on
        /// a UI thread.
        /// </summary>
        /// <param name="method">"GET", "POST", "PATCH" or "DELETE".</param>
        /// <param name="pathOrUrl">A path under "/v1.0/" or "/beta/", or a link Graph handed back (a delta or next link); any other
        /// address is refused without a request (GraphRequestPath).</param>
        /// <param name="accessToken">The bearer token (⚠ a secret: sent in the Authorization header only, never logged).</param>
        /// <param name="body">The JSON body; nullptr for none (a GET).</param>
        /// <param name="what">The call's purpose for the log ("legend post", "card edit 1a2b3c4d"); never the address, which can
        /// carry a delta token.</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>The answer; never throws (a failure is in the result).</returns>
        /// <remarks>
        /// A refusal (a non-2xx answer) is logged to hooks.log as `[teams] <what> HTTP <status> <code>: <message>`, a transport
        /// failure as `[teams] <what> transport failure <error>`; an abort is not logged (the stop logs once).
        /// </remarks>
        GraphResult Call(std::wstring_view method, std::wstring_view pathOrUrl, std::wstring_view accessToken, const json::Value* body,
                         std::wstring_view what, uint32_t timeoutMs = 30000) noexcept;

        /// <summary>
        /// Makes one Graph call with a BINARY body (a file's bytes for OneDrive, TEAMS_CHANNEL.md §8's pictures and files).
        /// Blocks like Call. The send timeout grows with the body (TelegramApi's rule: 30 s, or the body at 256 KB/s).
        /// </summary>
        /// <param name="method">"PUT" or "POST".</param>
        /// <param name="pathOrUrl">As Call.</param>
        /// <param name="accessToken">As Call.</param>
        /// <param name="contentType">The body's Content-Type ("image/png", "application/octet-stream").</param>
        /// <param name="bytes">The body.</param>
        /// <param name="what">As Call.</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>The answer; never throws.</returns>
        GraphResult Upload(std::wstring_view method, std::wstring_view pathOrUrl, std::wstring_view accessToken, std::wstring_view contentType,
                           const std::string& bytes, std::wstring_view what, uint32_t timeoutMs = 120000) noexcept;

        /// <summary>
        /// POSTs a form to the Microsoft identity platform (login.microsoftonline.com): the device-code request, a device-code
        /// redemption, a token refresh. The form carries no bearer token; it may carry a device code or a refresh token, so it is
        /// never logged.
        /// </summary>
        /// <param name="tenant">The tenant in the path (checked by IsTenantWord; anything else is refused without a request).</param>
        /// <param name="endpoint">"devicecode" or "token" (the path is "/{tenant}/oauth2/v2.0/{endpoint}"; anything else is refused).</param>
        /// <param name="form">The application/x-www-form-urlencoded body (FormUrlEncode each value).</param>
        /// <param name="what">The call's purpose for the log ("sign-in code", "token refresh").</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>The answer; never throws. A refusal's errorCode is the sign-in's `error` ("authorization_pending" is the normal
        /// answer while the owner has not signed in yet, and is not logged).</returns>
        GraphResult PostForm(std::wstring_view tenant, std::wstring_view endpoint, const std::string& form, std::wstring_view what, uint32_t timeoutMs = 30000) noexcept;

        /// <summary>
        /// Ends EVERY request in flight, from another thread: each blocked call returns a transport failure marked `aborted`.
        /// Safe with nothing in flight; later calls are not refused (see Shutdown). Never throws.
        /// </summary>
        void Abort() noexcept;

        /// <summary>
        /// Abort, and refuse every call that starts afterwards at once ("the client is shut down"), so a thread that is finishing a
        /// pass cannot start a new call after the service's stop. There is no way back: the service makes a new client at its next
        /// start. Never throws.
        /// </summary>
        void Shutdown() noexcept;

    private:
        /// <summary>
        /// The one request behind Call, Upload and PostForm: a WinHTTP session, connection and request on `host`, registered
        /// in `_inflight` while it runs so Abort can close it.
        /// </summary>
        /// <param name="host">The host (kGraphHost or kLoginHost, never anything a caller passed).</param>
        /// <param name="verb">The HTTP method.</param>
        /// <param name="path">The path with its query, already checked.</param>
        /// <param name="headers">The request headers, each ending in CRLF (⚠ may hold the bearer token: wiped by the caller).</param>
        /// <param name="body">The body (empty = none).</param>
        /// <param name="what">The purpose for the log.</param>
        /// <param name="quietErrorCode">A refusal code that is an expected answer and is not logged ("" = log every refusal).</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>The answer; never throws.</returns>
        GraphResult _request(std::wstring_view host, std::wstring_view verb, const std::wstring& path, const std::wstring& headers, const std::string& body,
                             std::wstring_view what, std::wstring_view quietErrorCode, uint32_t timeoutMs) noexcept;

        /// <summary>One request in flight, as Abort finds it (one entry per concurrent call).</summary>
        struct InFlight
        {
            /// <summary>The request's HINTERNET (nullptr once Abort closed it).</summary>
            void* handle{ nullptr };
            /// <summary>Set by Abort when it closed `handle`: the owner then skips its own close (closing twice is undefined) and
            /// reports the call as aborted.</summary>
            bool aborted{ false };
        };

        /// <summary>Guards `_inflight` and `_shutdown`.</summary>
        std::mutex _mtx;
        /// <summary>Every request in flight (a list: an entry's iterator stays valid while other calls come and go).</summary>
        std::list<InFlight> _inflight;
        /// <summary>Shutdown ran: every later call fails at once.</summary>
        bool _shutdown{ false };
    };
}
