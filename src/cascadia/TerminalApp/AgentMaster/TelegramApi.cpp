// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TelegramApi (see TelegramApi.h): WinHTTP POSTs to api.telegram.org.

#include "TelegramApi.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8 / Utf8ToWide

#pragma comment(lib, "winhttp.lib")

namespace Agentmaster::Msg
{
    namespace
    {
        constexpr const wchar_t* kHost = L"api.telegram.org";
        // Telegram's answers are small JSON; a getUpdates answer with 100 updates of 4 KB each is still
        // under 1 MB. The cap is the H2 body limit: a larger answer is cut and fails to parse (logged),
        // never buffered without bound.
        constexpr size_t kMaxBodyBytes = 2u * 1024u * 1024u;

        int64_t TickMs()
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        void Log(const std::wstring& line)
        {
            AppendStateLog(L"messaging.log", line + L"\n");
        }

        // One multipart form-data part for a text field.
        void AppendField(std::string& body, const std::string& boundary, const std::string& name, const std::string& value)
        {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
        }

        // A JSON value as a multipart field's text: strings verbatim, numbers as integers when integral,
        // booleans as "true"/"false", objects/arrays as JSON (reply_markup is sent that way).
        std::string FieldText(const json::Value& v)
        {
            switch (v.type)
            {
            case json::Value::Type::Str:
                return Profiles::detail::WideToUtf8(v.str);
            case json::Value::Type::Bool:
                return v.boolean ? "true" : "false";
            case json::Value::Type::Num:
            {
                if (v.num == static_cast<double>(static_cast<int64_t>(v.num)))
                {
                    return std::to_string(static_cast<long long>(v.num));
                }
                return std::to_string(v.num);
            }
            case json::Value::Type::Obj:
            case json::Value::Type::Arr:
                return Profiles::detail::WideToUtf8(json::Dump(v));
            default:
                return {};
            }
        }
    }

    TelegramApi::TelegramApi(std::wstring token) :
        _token(std::move(token))
    {
    }

    TelegramApi::~TelegramApi()
    {
        Abort();
    }

    std::wstring TelegramApi::MaskToken(std::wstring_view token)
    {
        if (token.empty())
        {
            return {};
        }
        if (token.size() <= 8)
        {
            return L"\x2026";
        }
        return std::wstring{ token.substr(0, 4) } + L"\x2026" + std::wstring{ token.substr(token.size() - 3) };
    }

    void TelegramApi::Abort() noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            if (_inflight)
            {
                // Closing the request handle from another thread makes the blocked WinHttpReceiveResponse /
                // WinHttpReadData return with ERROR_WINHTTP_OPERATION_CANCELLED; the owner sees _aborted
                // and skips its own close of this handle (closing twice is undefined).
                ::WinHttpCloseHandle(static_cast<HINTERNET>(_inflight));
                _inflight = nullptr;
                _aborted = true;
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TelegramApi::Abort");
        }
    }

    TgResult TelegramApi::Call(std::wstring_view method, const json::Value& params, uint32_t timeoutMs) noexcept
    {
        try
        {
            const std::string body = Profiles::detail::WideToUtf8(json::Dump(params.type == json::Value::Type::Obj ? params : json::Value::MkObj()));
            return _request(method, body, L"Content-Type: application/json; charset=utf-8\r\n", timeoutMs);
        }
        catch (...)
        {
            LogSwallowedException(L"TelegramApi::Call");
            TgResult r;
            r.transportFailed = true;
            r.description = L"exception while building the request";
            return r;
        }
    }

    TgResult TelegramApi::SendDocument(const json::Value& params, std::wstring_view fileName, const std::string& bytes, uint32_t timeoutMs) noexcept
    {
        try
        {
            // A boundary no file content can hold: the tick count plus a fixed tag.
            const std::string boundary = "----AgentmasterTg" + std::to_string(static_cast<unsigned long long>(TickMs())) + "x7Q";
            std::string body;
            body.reserve(bytes.size() + 1024);
            if (params.type == json::Value::Type::Obj)
            {
                for (const auto& [k, v] : params.members)
                {
                    AppendField(body, boundary, Profiles::detail::WideToUtf8(k), FieldText(v));
                }
            }
            std::string name = Profiles::detail::WideToUtf8(fileName);
            for (auto& c : name)
            {
                if (c == '"' || c == '\\' || c == '\r' || c == '\n')
                {
                    c = '_';
                }
            }
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" + name +
                    "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
            body += bytes;
            body += "\r\n--" + boundary + "--\r\n";
            const std::wstring contentType = L"Content-Type: multipart/form-data; boundary=" + Profiles::detail::Utf8ToWide(boundary) + L"\r\n";
            return _request(L"sendDocument", body, contentType, timeoutMs);
        }
        catch (...)
        {
            LogSwallowedException(L"TelegramApi::SendDocument");
            TgResult r;
            r.transportFailed = true;
            r.description = L"exception while building the upload";
            return r;
        }
    }

    TgResult TelegramApi::_request(std::wstring_view method, const std::string& body, const std::wstring& contentType, uint32_t timeoutMs) noexcept
    {
        TgResult r;
        const int64_t startedAt = TickMs();
        HINTERNET hSession = nullptr;
        HINTERNET hConnect = nullptr;
        HINTERNET hRequest = nullptr;
        bool registered = false; // _inflight holds hRequest
        try
        {
            hSession = ::WinHttpOpen(L"Agentmaster-Messaging/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSession)
            {
                r.transportFailed = true;
                r.description = L"WinHttpOpen failed (" + std::to_wstring(::GetLastError()) + L")";
                return r;
            }
            // Resolve / connect / send are short; only the receive waits for a long poll.
            ::WinHttpSetTimeouts(hSession, 15000, 15000, 30000, static_cast<int>(timeoutMs));
            hConnect = ::WinHttpConnect(hSession, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!hConnect)
            {
                r.transportFailed = true;
                r.description = L"WinHttpConnect failed (" + std::to_wstring(::GetLastError()) + L")";
                ::WinHttpCloseHandle(hSession);
                return r;
            }
            const std::wstring path = L"/bot" + _token + L"/" + std::wstring{ method };
            hRequest = ::WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (!hRequest)
            {
                r.transportFailed = true;
                r.description = L"WinHttpOpenRequest failed (" + std::to_wstring(::GetLastError()) + L")";
                ::WinHttpCloseHandle(hConnect);
                ::WinHttpCloseHandle(hSession);
                return r;
            }
            {
                std::lock_guard lk{ _mtx };
                _inflight = hRequest;
                _aborted = false;
                registered = true;
            }
            BOOL ok = ::WinHttpSendRequest(hRequest,
                                           contentType.c_str(),
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
            if (ok)
            {
                DWORD status = 0, slen = sizeof(status);
                ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
                r.httpStatus = static_cast<int>(status);
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
                aborted = _aborted;
                if (_inflight == hRequest)
                {
                    _inflight = nullptr;
                }
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

            if (!ok)
            {
                r.transportFailed = true;
                r.description = aborted ? L"aborted" : L"request failed (" + std::to_wstring(lastError) + L")";
                if (!aborted)
                {
                    Log(L"[tg] " + std::wstring{ method } + L" transport failure " + std::to_wstring(lastError) + L" after " + std::to_wstring(TickMs() - startedAt) + L" ms");
                }
                return r;
            }
            const auto doc = json::Parse(Profiles::detail::Utf8ToWide(answer));
            if (!doc || doc->type != json::Value::Type::Obj)
            {
                r.description = L"unreadable answer (HTTP " + std::to_wstring(r.httpStatus) + L", " + std::to_wstring(answer.size()) + L" bytes)";
                Log(L"[tg] " + std::wstring{ method } + L" " + r.description);
                return r;
            }
            r.ok = doc->BoolAt(L"ok", false);
            if (r.ok)
            {
                if (const auto* res = doc->Find(L"result"))
                {
                    r.result = *res;
                }
            }
            else
            {
                r.description = doc->StrAt(L"description");
                if (const auto* p = doc->Find(L"parameters"); p && p->type == json::Value::Type::Obj)
                {
                    r.retryAfterSec = static_cast<int>(p->I64At(L"retry_after", 0));
                }
                // The long poll's own errors are the caller's to pace; everything else is logged once here.
                Log(L"[tg] " + std::wstring{ method } + L" HTTP " + std::to_wstring(r.httpStatus) + L": " + r.description +
                    (r.retryAfterSec ? L" (retry after " + std::to_wstring(r.retryAfterSec) + L" s)" : std::wstring{}));
            }
            const int64_t took = TickMs() - startedAt;
            if (took > 8000 && method != L"getUpdates")
            {
                Log(L"[tg] " + std::wstring{ method } + L" took " + std::to_wstring(took) + L" ms");
            }
            return r;
        }
        catch (...)
        {
            // Rule #18: the allocation that failed is logged; the handles are closed below so a failed
            // call can never leak them (the HttpsGet discipline).
            LogSwallowedException(L"TelegramApi::_request");
            if (registered)
            {
                std::lock_guard lk{ _mtx };
                if (_inflight == hRequest)
                {
                    _inflight = nullptr;
                }
                if (_aborted)
                {
                    hRequest = nullptr; // Abort closed it
                }
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
            TgResult f;
            f.transportFailed = true;
            f.description = L"exception during the request";
            return f;
        }
    }
}
