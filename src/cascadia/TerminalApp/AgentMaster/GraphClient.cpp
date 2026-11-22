// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — GraphClient (see GraphClient.h): WinHTTP calls to Microsoft Graph and the Microsoft identity platform.

#include "GraphClient.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "TeamsModels.h" // GraphRequestPath, IsTenantWord, ReadGraphError, Utf8FromUtf16 / Utf16FromUtf8, kGraphHost, kLoginHost

#pragma comment(lib, "winhttp.lib")

namespace Agentmaster::Teams
{
    namespace
    {
        /// <summary>
        /// The most bytes of an answer kept. A delta page with 50 messages of up to 80 KB each stays under 4 MB; a larger answer
        /// is cut (and then fails to parse, logged), never buffered without bound.
        /// </summary>
        constexpr size_t kMaxBodyBytes = 16u * 1024u * 1024u;

        /// <summary>The monotonic clock for a call's duration.</summary>
        /// <returns>Milliseconds since the system started.</returns>
        int64_t TickMs() noexcept
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        /// <summary>Appends one line to hooks.log (the connector's `[teams]` lines, TEAMS_CHANNEL.md §11).</summary>
        /// <param name="line">The line, without its newline.</param>
        void Log(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"hooks.log", line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the logger itself failed; there is nothing left to report through (Rule #18's exemption).
            }
        }

        /// <summary>A call the client refuses without a request (GraphResult::refused): never retried.</summary>
        /// <param name="description">Why.</param>
        /// <returns>The result.</returns>
        GraphResult Refused(std::wstring description)
        {
            GraphResult r;
            r.transportFailed = true;
            r.refused = true;
            r.description = std::move(description);
            return r;
        }

        /// <summary>Overwrites a text that held a secret before it is freed (best effort: a copy the allocator made is not reached).</summary>
        /// <typeparam name="Text">std::string or std::wstring.</typeparam>
        /// <param name="text">The text.</param>
        template<typename Text>
        void Wipe(Text& text) noexcept
        {
            std::fill(text.begin(), text.end(), static_cast<typename Text::value_type>(0));
        }
    }

    GraphClient::~GraphClient()
    {
        Abort();
    }

    void GraphClient::Abort() noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            for (auto& request : _inflight)
            {
                if (request.handle)
                {
                    // Closing the request handle from another thread makes the blocked WinHttpSendRequest /
                    // WinHttpReceiveResponse / WinHttpReadData return with ERROR_WINHTTP_OPERATION_CANCELLED; the owner sees its
                    // entry's `aborted` and skips its own close of this handle (closing twice is undefined).
                    ::WinHttpCloseHandle(static_cast<HINTERNET>(request.handle));
                    request.handle = nullptr;
                    request.aborted = true;
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"GraphClient::Abort");
        }
    }

    void GraphClient::Shutdown() noexcept
    {
        {
            std::lock_guard lk{ _mtx };
            _shutdown = true; // first: a call that registers after this point is refused instead of escaping the abort
        }
        Abort();
    }

    GraphResult GraphClient::Call(std::wstring_view method, std::wstring_view pathOrUrl, std::wstring_view accessToken, const json::Value* body,
                                  std::wstring_view what, uint32_t timeoutMs) noexcept
    {
        try
        {
            // The guard that keeps the owner's token on Graph: a delta or next link is read from the network, and only a plain
            // graph.microsoft.com address may receive the Authorization header.
            const auto path = GraphRequestPath(pathOrUrl);
            if (!path)
            {
                Log(L"[teams] " + std::wstring{ what } + L" refused: not a Graph address");
                return Refused(L"not a Graph address");
            }
            if (accessToken.empty())
            {
                return Refused(L"no token");
            }
            std::wstring headers = L"Authorization: Bearer " + std::wstring{ accessToken } + L"\r\nAccept: application/json\r\n";
            std::string bytes;
            if (body)
            {
                headers += L"Content-Type: application/json; charset=utf-8\r\n";
                bytes = Utf8FromUtf16(json::Dump(*body));
            }
            GraphResult r = _request(kGraphHost, method, *path, headers, bytes, what, L"", timeoutMs);
            Wipe(headers); // the token leaves this copy as soon as the call is done
            return r;
        }
        catch (...)
        {
            // Rule #18; recovery: the call reads as failed and retryable (an allocation failure is transient).
            LogSwallowedException(L"GraphClient::Call");
            GraphResult f;
            f.transportFailed = true;
            f.description = L"exception while building the request";
            return f;
        }
    }

    GraphResult GraphClient::Upload(std::wstring_view method, std::wstring_view pathOrUrl, std::wstring_view accessToken, std::wstring_view contentType,
                                    const std::string& bytes, std::wstring_view what, uint32_t timeoutMs) noexcept
    {
        try
        {
            const auto path = GraphRequestPath(pathOrUrl);
            if (!path)
            {
                Log(L"[teams] " + std::wstring{ what } + L" refused: not a Graph address");
                return Refused(L"not a Graph address");
            }
            if (accessToken.empty())
            {
                return Refused(L"no token");
            }
            // A content type with a line break would add a header of the caller's choosing: refused.
            if (contentType.find_first_of(L"\r\n") != std::wstring_view::npos)
            {
                return Refused(L"a content type with a line break");
            }
            std::wstring headers = L"Authorization: Bearer " + std::wstring{ accessToken } + L"\r\nAccept: application/json\r\nContent-Type: " +
                                   std::wstring{ contentType } + L"\r\n";
            GraphResult r = _request(kGraphHost, method, *path, headers, bytes, what, L"", timeoutMs);
            Wipe(headers);
            return r;
        }
        catch (...)
        {
            LogSwallowedException(L"GraphClient::Upload");
            GraphResult f;
            f.transportFailed = true;
            f.description = L"exception while building the upload";
            return f;
        }
    }

    GraphResult GraphClient::PostForm(std::wstring_view tenant, std::wstring_view endpoint, const std::string& form, std::wstring_view what, uint32_t timeoutMs) noexcept
    {
        try
        {
            // The tenant comes from the settings: only a tenant word may shape the path, and only the two endpoints the sign-in
            // uses are reachable.
            if (!IsTenantWord(tenant))
            {
                return Refused(L"the tenant is not a tenant id, a domain, \"organizations\" or \"common\"");
            }
            if (endpoint != L"devicecode" && endpoint != L"token")
            {
                return Refused(L"not a sign-in endpoint");
            }
            const std::wstring path = L"/" + std::wstring{ tenant } + L"/oauth2/v2.0/" + std::wstring{ endpoint };
            const std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n";
            // "authorization_pending" is the token endpoint's normal answer while the owner has not signed in yet: not logged.
            return _request(kLoginHost, L"POST", path, headers, form, what, L"authorization_pending", timeoutMs);
        }
        catch (...)
        {
            LogSwallowedException(L"GraphClient::PostForm");
            GraphResult f;
            f.transportFailed = true;
            f.description = L"exception while building the form";
            return f;
        }
    }

    GraphResult GraphClient::_request(std::wstring_view host, std::wstring_view verb, const std::wstring& path, const std::wstring& headers, const std::string& body,
                                      std::wstring_view what, std::wstring_view quietErrorCode, uint32_t timeoutMs) noexcept
    {
        GraphResult r;
        const int64_t startedAt = TickMs();
        HINTERNET hSession = nullptr;
        HINTERNET hConnect = nullptr;
        HINTERNET hRequest = nullptr;
        bool registered = false; // _inflight holds this call's entry (`slot`)
        std::list<InFlight>::iterator slot;
        try
        {
            {
                // After Shutdown nothing new goes out: a thread finishing its pass must not start a call the stop's abort passed.
                std::lock_guard lk{ _mtx };
                if (_shutdown)
                {
                    GraphResult s = Refused(L"the client is shut down");
                    s.aborted = true;
                    return s;
                }
            }
            const std::wstring hostText{ host };
            const std::wstring verbText{ verb };
            hSession = ::WinHttpOpen(L"Agentmaster-Teams/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSession)
            {
                r.transportFailed = true;
                r.description = L"WinHttpOpen failed (" + std::to_wstring(::GetLastError()) + L")";
                return r;
            }
            // Resolve and connect are short; the send grows with the body (an upload: 30 s, or the body at 256 KB/s, as
            // TelegramApi), the receive is the caller's.
            const int sendTimeoutMs = static_cast<int>((std::max)(uint64_t{ 30000 }, static_cast<uint64_t>(body.size()) / 256));
            ::WinHttpSetTimeouts(hSession, 15000, 15000, sendTimeoutMs, static_cast<int>(timeoutMs));
            hConnect = ::WinHttpConnect(hSession, hostText.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!hConnect)
            {
                r.transportFailed = true;
                r.description = L"WinHttpConnect failed (" + std::to_wstring(::GetLastError()) + L")";
                ::WinHttpCloseHandle(hSession);
                return r;
            }
            hRequest = ::WinHttpOpenRequest(hConnect, verbText.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (!hRequest)
            {
                r.transportFailed = true;
                r.description = L"WinHttpOpenRequest failed (" + std::to_wstring(::GetLastError()) + L")";
                ::WinHttpCloseHandle(hConnect);
                ::WinHttpCloseHandle(hSession);
                return r;
            }
            // No automatic redirect: WinHTTP would send the same headers — the bearer token — to wherever a 3xx points. Graph
            // answers its API calls without redirects; a 3xx comes back to the caller as it is.
            DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            ::WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
            {
                std::lock_guard lk{ _mtx };
                if (_shutdown)
                {
                    // A Shutdown that ran between the check above and here aborted every REGISTERED request, not this one: it is
                    // refused here instead, so nothing escapes the stop.
                    ::WinHttpCloseHandle(hRequest);
                    ::WinHttpCloseHandle(hConnect);
                    ::WinHttpCloseHandle(hSession);
                    hRequest = hConnect = hSession = nullptr;
                    GraphResult s = Refused(L"the client is shut down");
                    s.aborted = true;
                    return s;
                }
                slot = _inflight.insert(_inflight.end(), InFlight{ hRequest, false });
                registered = true;
            }
            BOOL ok = ::WinHttpSendRequest(hRequest,
                                           headers.c_str(),
                                           static_cast<DWORD>(-1),
                                           body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                           static_cast<DWORD>(body.size()),
                                           static_cast<DWORD>(body.size()),
                                           0);
            DWORD lastError = ok ? 0 : ::GetLastError();
            if (ok)
            {
                ok = ::WinHttpReceiveResponse(hRequest, nullptr);
                lastError = ok ? 0 : ::GetLastError();
            }
            std::string answer;
            bool cut = false;
            if (ok)
            {
                DWORD status = 0;
                DWORD slen = sizeof(status);
                ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
                r.httpStatus = static_cast<int>(status);
                // Retry-After in seconds (Graph's form); an HTTP date fails the number read and leaves 0 (the caller's own wait).
                DWORD retryAfter = 0;
                DWORD rlen = sizeof(retryAfter);
                if (::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &retryAfter, &rlen, WINHTTP_NO_HEADER_INDEX))
                {
                    r.retryAfterSec = static_cast<int>((std::min)(retryAfter, DWORD{ 3600 }));
                }
                for (;;)
                {
                    DWORD avail = 0;
                    if (!::WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0)
                    {
                        break;
                    }
                    if (answer.size() + avail > kMaxBodyBytes)
                    {
                        avail = static_cast<DWORD>(kMaxBodyBytes - answer.size());
                        cut = true;
                        if (avail == 0)
                        {
                            break;
                        }
                    }
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!::WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0)
                    {
                        break;
                    }
                    answer.append(chunk.data(), read);
                }
            }
            // Release the in-flight slot BEFORE parsing: Abort must find nothing to close now.
            bool aborted = false;
            {
                std::lock_guard lk{ _mtx };
                aborted = slot->aborted;
                _inflight.erase(slot);
                registered = false;
            }
            if (!aborted)
            {
                ::WinHttpCloseHandle(hRequest);
            }
            hRequest = nullptr;
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            hConnect = hSession = nullptr;
            r.tookMs = TickMs() - startedAt;
            r.bytes = answer.size();

            if (!ok || aborted)
            {
                r.transportFailed = true;
                r.aborted = aborted;
                r.httpStatus = aborted ? 0 : r.httpStatus;
                r.description = aborted ? L"aborted" : L"request failed (" + std::to_wstring(lastError) + L")";
                if (!aborted)
                {
                    Log(L"[teams] " + std::wstring{ what } + L" transport failure " + std::to_wstring(lastError) + L" after " + std::to_wstring(r.tookMs) + L" ms");
                }
                return r;
            }
            if (!answer.empty())
            {
                if (auto doc = json::Parse(Utf16FromUtf8(answer)))
                {
                    r.body = std::move(*doc);
                }
            }
            if (!r.Ok())
            {
                const GraphError e = ReadGraphError(r.body);
                r.errorCode = e.code;
                r.description = e.message.empty() ? L"HTTP " + std::to_wstring(r.httpStatus) : e.message;
                if (quietErrorCode.empty() || r.errorCode != quietErrorCode)
                {
                    Log(L"[teams] " + std::wstring{ what } + L" HTTP " + std::to_wstring(r.httpStatus) + (r.errorCode.empty() ? std::wstring{} : L" " + r.errorCode) + L": " +
                        r.description + (r.retryAfterSec ? L" (retry after " + std::to_wstring(r.retryAfterSec) + L" s)" : std::wstring{}));
                }
            }
            else if (cut)
            {
                // A success whose body was cut cannot be read: the caller sees no body, and the log says why.
                Log(L"[teams] " + std::wstring{ what } + L" answer cut at " + std::to_wstring(kMaxBodyBytes) + L" bytes");
            }
            if (r.tookMs > 8000)
            {
                Log(L"[teams] " + std::wstring{ what } + L" took " + std::to_wstring(r.tookMs) + L" ms");
            }
            return r;
        }
        catch (...)
        {
            // Rule #18: the failure is logged; the handles are closed below so a failed call can never leak them.
            LogSwallowedException(L"GraphClient::_request");
            if (registered)
            {
                std::lock_guard lk{ _mtx };
                if (slot->aborted)
                {
                    hRequest = nullptr; // Abort closed it
                }
                _inflight.erase(slot);
            }
            if (hRequest)
            {
                ::WinHttpCloseHandle(hRequest);
            }
            if (hConnect)
            {
                ::WinHttpCloseHandle(hConnect);
            }
            if (hSession)
            {
                ::WinHttpCloseHandle(hSession);
            }
            GraphResult f;
            f.transportFailed = true;
            f.description = L"exception during the request";
            return f;
        }
    }
}
