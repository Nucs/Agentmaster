// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceHttp (see VoiceHttp.h): the OpenAI-compatible transcription call over WinHTTP, and the API key in
// Credential Manager.

#include "VoiceHttp.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <winhttp.h>

#include <algorithm>
#include <vector>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "Json.h"
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8 / Utf8ToWide

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

namespace Agentmaster::Voice
{
    namespace
    {
        /// <summary>An answer body larger than this is cut (a transcript of a 30-minute note is ~40 KB; 1 MB is no answer).</summary>
        constexpr size_t kMaxAnswerBytes = 1u << 20;

        /// <summary>One multipart text field.</summary>
        /// <param name="body">The body being built.</param>
        /// <param name="boundary">The boundary.</param>
        /// <param name="name">The field name.</param>
        /// <param name="value">The value (UTF-16; written as UTF-8).</param>
        void AppendTextField(std::string& body, const std::string& boundary, const char* name, std::wstring_view value)
        {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" +
                    Profiles::detail::WideToUtf8(std::wstring{ value }) + "\r\n";
        }

        /// <summary>The leaf of a profile folder ("C:\Users\me\.agentmaster" → ".agentmaster"; "" → "default").</summary>
        /// <param name="profileDir">The folder.</param>
        /// <returns>The leaf.</returns>
        std::wstring ProfileLeaf(std::wstring_view profileDir)
        {
            std::wstring dir{ profileDir };
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            {
                dir.pop_back();
            }
            const size_t slash = dir.find_last_of(L"\\/");
            std::wstring leaf = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
            return leaf.empty() ? std::wstring{ L"default" } : leaf;
        }
    }

    std::wstring VoiceApiKeyTarget(std::wstring_view profileDir)
    {
        return L"Agentmaster:voice:api-key:" + ProfileLeaf(profileDir);
    }

    std::wstring ReadVoiceApiKey(std::wstring_view profileDir) noexcept
    {
        try
        {
            const std::wstring target = VoiceApiKeyTarget(profileDir);
            PCREDENTIALW cred = nullptr;
            if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
            {
                return {};
            }
            std::wstring key;
            if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t))
            {
                key.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
            }
            ::CredFree(cred);
            while (!key.empty() && (key.back() == L'\0' || key.back() == L' ' || key.back() == L'\r' || key.back() == L'\n'))
            {
                key.pop_back();
            }
            return key;
        }
        catch (...)
        {
            LogSwallowedException(L"ReadVoiceApiKey");
            return {};
        }
    }

    bool StoreVoiceApiKey(std::wstring_view profileDir, std::wstring_view key) noexcept
    {
        try
        {
            const std::wstring target = VoiceApiKeyTarget(profileDir);
            std::wstring value{ key };
            const size_t a = value.find_first_not_of(L" \t\r\n");
            if (a == std::wstring::npos)
            {
                // A blank key is "forget it": deleting a missing credential is a success too.
                return ::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || ::GetLastError() == ERROR_NOT_FOUND;
            }
            value = value.substr(a, value.find_last_not_of(L" \t\r\n") - a + 1);
            CREDENTIALW cred{};
            cred.Type = CRED_TYPE_GENERIC;
            cred.TargetName = const_cast<wchar_t*>(target.c_str());
            cred.CredentialBlobSize = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            cred.CredentialBlob = reinterpret_cast<LPBYTE>(value.data());
            cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
            cred.UserName = const_cast<wchar_t*>(L"agentmaster-voice");
            const bool ok = ::CredWriteW(&cred, 0) != FALSE;
            if (!ok)
            {
                AppendStateLog(L"hooks.log", L"[voice] Credential Manager refused the voice API key (error " + std::to_wstring(::GetLastError()) + L")\n");
            }
            return ok;
        }
        catch (...)
        {
            LogSwallowedException(L"StoreVoiceApiKey");
            return false;
        }
    }

    VoiceHttpResult TranscribeWithEndpoint(std::wstring_view url, std::wstring_view apiKey, std::wstring_view model, std::wstring_view language,
                                           std::wstring_view prompt, std::wstring_view fileName, const std::string& audio, uint32_t timeoutMs) noexcept
    {
        VoiceHttpResult r;
        const ULONGLONG started = ::GetTickCount64();
        HINTERNET session = nullptr;
        HINTERNET connect = nullptr;
        HINTERNET request = nullptr;
        // Every handle closes on every path (the HttpsGet discipline): the lambda runs at each return below.
        const auto closeAll = [&]() noexcept {
            if (request)
            {
                ::WinHttpCloseHandle(request);
            }
            if (connect)
            {
                ::WinHttpCloseHandle(connect);
            }
            if (session)
            {
                ::WinHttpCloseHandle(session);
            }
            request = connect = session = nullptr;
        };
        try
        {
            std::wstring urlText{ url };
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            wchar_t host[256]{};
            wchar_t path[2048]{};
            parts.lpszHostName = host;
            parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
            if (!::WinHttpCrackUrl(urlText.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0)
            {
                r.error = L"the endpoint URL could not be read";
                return r;
            }
            const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
            session = ::WinHttpOpen(L"Agentmaster-Voice/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session)
            {
                r.error = L"WinHttpOpen failed (" + std::to_wstring(::GetLastError()) + L")";
                return r;
            }
            // The upload of a long note on a slow uplink can take a while: the send timeout grows with the body (64 KB/s).
            const int sendTimeoutMs = static_cast<int>((std::max)(uint64_t{ 30000 }, static_cast<uint64_t>(audio.size()) / 64));
            ::WinHttpSetTimeouts(session, 15000, 15000, sendTimeoutMs, static_cast<int>(timeoutMs));
            connect = ::WinHttpConnect(session, host, parts.nPort, 0);
            if (!connect)
            {
                r.error = L"could not connect to " + std::wstring{ host } + L" (" + std::to_wstring(::GetLastError()) + L")";
                closeAll();
                return r;
            }
            request = ::WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
            if (!request)
            {
                r.error = L"WinHttpOpenRequest failed (" + std::to_wstring(::GetLastError()) + L")";
                closeAll();
                return r;
            }
            // The body: the text fields, then the file. The boundary carries the tick count, which no audio byte run can be
            // relied on to contain; a collision would only corrupt this one request.
            const std::string boundary = "----AgentmasterVoice" + std::to_string(static_cast<unsigned long long>(::GetTickCount64())) + "q7Z";
            std::string body;
            body.reserve(audio.size() + 4096);
            AppendTextField(body, boundary, "model", model);
            if (!language.empty())
            {
                AppendTextField(body, boundary, "language", language);
            }
            if (!prompt.empty())
            {
                AppendTextField(body, boundary, "prompt", prompt);
            }
            AppendTextField(body, boundary, "response_format", L"json");
            AppendTextField(body, boundary, "temperature", L"0");
            std::string name = Profiles::detail::WideToUtf8(std::wstring{ fileName });
            for (auto& c : name)
            {
                if (c == '"' || c == '\\' || c == '\r' || c == '\n')
                {
                    c = '_';
                }
            }
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name + "\"\r\nContent-Type: audio/ogg\r\n\r\n";
            body += audio;
            body += "\r\n--" + boundary + "--\r\n";
            std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + Profiles::detail::Utf8ToWide(boundary) + L"\r\n";
            if (!apiKey.empty())
            {
                headers += L"Authorization: Bearer " + std::wstring{ apiKey } + L"\r\n";
            }
            BOOL ok = ::WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), body.data(), static_cast<DWORD>(body.size()),
                                           static_cast<DWORD>(body.size()), 0);
            // The headers held the key: wiped before anything else can see the string (a crash dump, a later log line).
            std::fill(headers.begin(), headers.end(), L'\0');
            if (ok)
            {
                ok = ::WinHttpReceiveResponse(request, nullptr);
            }
            if (!ok)
            {
                r.error = L"the endpoint did not answer (" + std::to_wstring(::GetLastError()) + L")";
                closeAll();
                r.ms = static_cast<int64_t>(::GetTickCount64() - started);
                return r;
            }
            DWORD status = 0, statusLen = sizeof(status);
            ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
            r.httpStatus = static_cast<int>(status);
            std::string answer;
            for (;;)
            {
                DWORD avail = 0;
                if (!::WinHttpQueryDataAvailable(request, &avail) || avail == 0 || answer.size() >= kMaxAnswerBytes)
                {
                    break;
                }
                std::string chunk((std::min)(static_cast<size_t>(avail), kMaxAnswerBytes - answer.size()), '\0');
                DWORD read = 0;
                if (!::WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read) || read == 0)
                {
                    break;
                }
                answer.append(chunk.data(), read);
            }
            closeAll();
            r.ms = static_cast<int64_t>(::GetTickCount64() - started);
            const auto doc = json::Parse(Profiles::detail::Utf8ToWide(answer));
            if (r.httpStatus >= 200 && r.httpStatus < 300 && doc && doc->type == json::Value::Type::Obj && doc->Find(L"text"))
            {
                r.ok = true;
                r.text = doc->StrAt(L"text");
                return r;
            }
            // OpenAI's error shape: {"error":{"message":"…"}}; anything else is reported by its status alone.
            std::wstring message;
            if (doc && doc->type == json::Value::Type::Obj)
            {
                if (const auto* e = doc->Find(L"error"); e && e->type == json::Value::Type::Obj)
                {
                    message = e->StrAt(L"message");
                }
                else if (e && e->type == json::Value::Type::Str)
                {
                    message = e->str;
                }
            }
            r.error = L"the endpoint answered HTTP " + std::to_wstring(r.httpStatus) + (message.empty() ? std::wstring{} : L": " + message.substr(0, 300));
            return r;
        }
        catch (...)
        {
            LogSwallowedException(L"TranscribeWithEndpoint");
            closeAll();
            r.ok = false;
            r.error = L"the endpoint call failed (an exception; see hooks.log)";
            return r;
        }
    }
}
