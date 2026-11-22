// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TeamsAuth (see TeamsAuth.h): the Azure CLI's token, the allowed app's device-code sign-in and refresh, and the
// refresh token's parts in Windows Credential Manager.

#include "TeamsAuth.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <chrono>
#include <ctime> // localtime_s: the log's clock words
#include <cwchar> // swprintf_s
#include <thread>
#include <vector>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "ClaudeStatus.h" // RunClaudeCapture: a hidden child in a kill-on-close job, its output captured

#pragma comment(lib, "advapi32.lib") // CredReadW / CredWriteW / CredDeleteW

namespace Agentmaster::Teams
{
    namespace
    {
        /// <summary>Appends one `[teams]` line to hooks.log.</summary>
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

        /// <summary>Overwrites a text that held a secret before it is freed (best effort: a copy the allocator made is not reached).</summary>
        /// <typeparam name="Text">std::string or std::wstring.</typeparam>
        /// <param name="text">The text.</param>
        template<typename Text>
        void Wipe(Text& text) noexcept
        {
            std::fill(text.begin(), text.end(), static_cast<typename Text::value_type>(0));
        }

        /// <summary>A folder's last segment ("C:\Users\me\.agentmaster-dev" → ".agentmaster-dev"): the credential's key.</summary>
        /// <param name="dir">The folder.</param>
        /// <returns>The segment ("" for an empty path).</returns>
        /// <exception cref="std::bad_alloc">If the segment cannot be allocated.</exception>
        std::wstring Leaf(std::wstring_view dir)
        {
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            {
                dir.remove_suffix(1);
            }
            const size_t slash = dir.find_last_of(L"\\/");
            return std::wstring{ slash == std::wstring_view::npos ? dir : dir.substr(slash + 1) };
        }

        /// <summary>The system's cmd.exe by full path (the Azure CLI's az.cmd is a batch file: it runs through cmd).</summary>
        /// <returns>The path; "cmd.exe" when the system folder cannot be read (CreateProcessW then searches for it).</returns>
        /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
        std::wstring SystemCmdExe()
        {
            wchar_t dir[MAX_PATH] = {};
            const UINT n = ::GetSystemDirectoryW(dir, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return L"cmd.exe";
            }
            return std::wstring(dir, n) + L"\\cmd.exe";
        }

        /// <summary>Frees a credential CredReadW returned, at the end of its scope.</summary>
        struct CredentialHolder
        {
            /// <summary>The credential (nullptr = none).</summary>
            PCREDENTIALW p{ nullptr };
            /// <summary>Frees it.</summary>
            ~CredentialHolder()
            {
                if (p)
                {
                    ::CredFree(p);
                }
            }
        };

        /// <summary>Reads one generic credential: its blob's bytes and its user name.</summary>
        /// <param name="target">The credential's target.</param>
        /// <param name="blob">Receives the blob (⚠ a secret: the caller wipes it).</param>
        /// <param name="user">Receives the user name.</param>
        /// <returns>True when the credential exists and was read.</returns>
        /// <exception cref="std::bad_alloc">If the texts cannot be allocated (the credential is freed either way).</exception>
        bool ReadCredentialPart(const std::wstring& target, std::string& blob, std::wstring& user)
        {
            CredentialHolder cred;
            if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred.p) || !cred.p)
            {
                return false;
            }
            blob.clear();
            if (cred.p->CredentialBlob && cred.p->CredentialBlobSize > 0)
            {
                blob.assign(reinterpret_cast<const char*>(cred.p->CredentialBlob), cred.p->CredentialBlobSize);
            }
            user = cred.p->UserName ? std::wstring{ cred.p->UserName } : std::wstring{};
            return true;
        }

        /// <summary>Writes (replaces) one generic credential, kept for this user on this PC (CRED_PERSIST_LOCAL_MACHINE).</summary>
        /// <param name="target">The target.</param>
        /// <param name="user">The user name.</param>
        /// <param name="blob">The blob (at most kCredentialPartBytes).</param>
        /// <returns>True when Credential Manager took it (GetLastError says why not).</returns>
        /// <exception cref="std::bad_alloc">If the writable copies cannot be allocated.</exception>
        bool WriteCredentialPart(const std::wstring& target, const std::wstring& user, const std::string& blob)
        {
            // CREDENTIALW takes writable strings: copies, so the caller's stay const.
            std::wstring targetText = target;
            std::wstring userText = user;
            CREDENTIALW cred{};
            cred.Type = CRED_TYPE_GENERIC;
            cred.TargetName = targetText.data();
            cred.UserName = userText.data();
            cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
            cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
            cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.data()));
            return ::CredWriteW(&cred, 0) != FALSE;
        }

        /// <summary>Deletes one generic credential.</summary>
        /// <param name="target">The target.</param>
        /// <returns>True when it was deleted or did not exist.</returns>
        bool DeleteCredentialPart(const std::wstring& target) noexcept
        {
            if (::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
            {
                return true;
            }
            return ::GetLastError() == ERROR_NOT_FOUND;
        }

        /// <summary>
        /// The stored refresh token of a client (TeamsModels.h §14): the first part's user word names the part count and the
        /// client; the parts' bytes are joined and decoded.
        /// </summary>
        /// <param name="base">The first part's target (AppSource::CredentialTarget).</param>
        /// <param name="clientId">The client whose token is wanted.</param>
        /// <returns>The token (⚠ a secret: the caller wipes it); "" when none is stored, it belongs to another client, or a part is
        /// missing.</returns>
        /// <exception cref="std::bad_alloc">If the token cannot be allocated.</exception>
        std::wstring ReadRefreshToken(const std::wstring& base, std::wstring_view clientId)
        {
            std::string bytes;
            std::wstring user;
            if (!ReadCredentialPart(base, bytes, user))
            {
                return {};
            }
            const auto word = ParseCredentialUserWord(user);
            if (!word || !detail::EqualsAsciiNoCase(word->second, clientId))
            {
                Wipe(bytes);
                return {};
            }
            for (size_t k = 1; k < word->first; ++k)
            {
                std::string part;
                std::wstring partUser;
                if (!ReadCredentialPart(CredentialPartTarget(base, k), part, partUser))
                {
                    // A part is missing (a store cut off between its writes): the token is incomplete, and only a new
                    // sign-in can give a whole one.
                    Wipe(bytes);
                    return {};
                }
                bytes += part;
                Wipe(part);
            }
            std::wstring token = Utf16FromUtf8(bytes);
            Wipe(bytes);
            return token;
        }

        /// <summary>
        /// Stores a client's refresh token over as many credentials as it needs (TeamsModels.h §14). The later parts are written
        /// first and the first part — whose user word names the count — last, so a reader never sees a count whose parts are not
        /// written yet; parts an older, longer token left behind are deleted.
        /// </summary>
        /// <param name="base">The first part's target.</param>
        /// <param name="clientId">The client the token belongs to.</param>
        /// <param name="token">The refresh token (⚠ a secret).</param>
        /// <returns>True when every part was stored. A refusal is logged with the Windows error — never the token.</returns>
        /// <exception cref="std::bad_alloc">If the parts cannot be allocated.</exception>
        bool StoreRefreshToken(const std::wstring& base, std::wstring_view clientId, std::wstring_view token)
        {
            std::string bytes = Utf8FromUtf16(token);
            std::vector<std::string> parts = SplitSecretParts(bytes);
            Wipe(bytes);
            if (parts.empty())
            {
                Log(L"[teams] the app's refresh token is empty or longer than " + std::to_wstring(kCredentialPartBytes * kCredentialMaxParts) +
                    L" bytes: not stored");
                return false;
            }
            bool ok = true;
            for (size_t k = parts.size(); k-- > 1;)
            {
                if (!WriteCredentialPart(CredentialPartTarget(base, k), L"teams-part", parts[k]))
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
            {
                ok = WriteCredentialPart(base, CredentialUserWord(parts.size(), clientId), parts[0]);
            }
            const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
            for (size_t k = parts.size(); k < kCredentialMaxParts; ++k)
            {
                DeleteCredentialPart(CredentialPartTarget(base, k));
            }
            for (std::string& p : parts)
            {
                Wipe(p);
            }
            if (!ok)
            {
                Log(L"[teams] could not store the app's sign-in in Credential Manager (error " + std::to_wstring(error) + L")");
            }
            return ok;
        }

        /// <summary>Deletes every part of the stored refresh token.</summary>
        /// <param name="base">The first part's target.</param>
        /// <returns>True when every part is gone.</returns>
        /// <exception cref="std::bad_alloc">If a part's target cannot be built.</exception>
        bool ClearRefreshToken(const std::wstring& base)
        {
            bool ok = true;
            for (size_t k = 0; k < kCredentialMaxParts; ++k)
            {
                ok = DeleteCredentialPart(CredentialPartTarget(base, k)) && ok;
            }
            return ok;
        }

        /// <summary>A clock word for the log ("15:04", local time).</summary>
        /// <param name="unixMs">The time (Unix ms).</param>
        /// <returns>The word ("" when the time cannot be converted).</returns>
        /// <exception cref="std::bad_alloc">If the word cannot be allocated.</exception>
        std::wstring ClockWord(int64_t unixMs)
        {
            const std::time_t t = static_cast<std::time_t>(unixMs / 1000);
            std::tm local{};
            if (localtime_s(&local, &t) != 0)
            {
                return {};
            }
            wchar_t buf[8] = {};
            swprintf_s(buf, L"%02d:%02d", local.tm_hour, local.tm_min);
            return buf;
        }

        /// <summary>A token's scopes as one line for the log ("ChannelMessage.Send, Files.ReadWrite").</summary>
        /// <param name="claims">The token's claims.</param>
        /// <returns>The line.</returns>
        /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
        std::wstring ScopesWord(const JwtClaims& claims)
        {
            std::wstring s;
            for (const std::wstring& scope : claims.scopes)
            {
                if (!s.empty())
                {
                    s += L", ";
                }
                s += scope;
            }
            return s.empty() ? std::wstring{ L"no scopes" } : s;
        }
    }

    int64_t TeamsNowMs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    AccessToken MakeAccessToken(std::wstring token, int64_t answerExpiresMs, int64_t nowMs)
    {
        AccessToken t;
        if (auto claims = ReadJwtClaims(token))
        {
            t.claims = std::move(*claims);
        }
        // The earlier of the two: an answer's lifetime counts from when it was sent, the token's exp is absolute.
        int64_t expires = answerExpiresMs;
        if (t.claims.expiresMs > 0 && (expires <= 0 || t.claims.expiresMs < expires))
        {
            expires = t.claims.expiresMs;
        }
        t.expiresMs = expires > 0 ? expires : nowMs + 30LL * 60 * 1000;
        t.token = std::move(token);
        return t;
    }

    // ===== the Azure CLI =====================================================================================================

    std::optional<AccessToken> AzureCliSource::Token(std::wstring_view tenantId) noexcept
    {
        try
        {
            const int64_t now = TeamsNowMs();
            std::optional<AccessToken> usable;
            bool start = false;
            {
                std::lock_guard lk{ _mtx };
                // A token counts only for the tenant asked for (its own `tid`): one the CLI gave for its default tenant before the
                // app's tenant was known is replaced by a run for that tenant, not used across tenants.
                const bool forTenant = _cached && (tenantId.empty() || detail::EqualsAsciiNoCase(_cached->claims.tenantId, tenantId));
                if (forTenant && _cached->UsableAt(now))
                {
                    usable = _cached;
                }
                const bool fresh = forTenant && _cached->FreshAt(now);
                // One run at a time, and none within a minute of a failed one: a missing or signed-out CLI then costs one
                // hidden `az` a minute, not one per call.
                if (!fresh && !_running && (_lastFailureMs == 0 || now - _lastFailureMs >= 60'000))
                {
                    _running = true;
                    start = true;
                }
            }
            if (start)
            {
                std::wstring tenant = IsTenantWord(tenantId) ? std::wstring{ tenantId } : std::wstring{};
                try
                {
                    // Detached, holding this source: the service never joins it, so its Stop never waits for `az` (up to 30 s).
                    // The capture has a name of its own: a capture named like the local it moves would shadow it (C4456 at /W4,
                    // an error under the project's warnings-as-errors).
                    std::thread([self = shared_from_this(), runTenant = std::move(tenant)]() mutable { self->_run(std::move(runTenant)); }).detach();
                }
                catch (...)
                {
                    // Rule #18; recovery: no run now — the next Token tries again after the failure's minute.
                    LogSwallowedException(L"AzureCliSource::Token (the run's thread)");
                    std::lock_guard lk{ _mtx };
                    _running = false;
                    _lastFailureMs = now;
                    _state = L"could not start its run";
                }
            }
            return usable;
        }
        catch (...)
        {
            LogSwallowedException(L"AzureCliSource::Token");
            return std::nullopt;
        }
    }

    void AzureCliSource::_run(std::wstring tenantId) noexcept
    {
        std::optional<AccessToken> got;
        std::wstring state;
        try
        {
            const std::wstring az = FindAzCommand();
            if (az.empty())
            {
                state = L"not found (install the Azure CLI, or set teamsUseAzureCli to false)";
            }
            else
            {
                // cmd /s /c "<line>": with /s, cmd takes the line between the outer quotes as it is, so the quoted path of az.cmd
                // (under Program Files, with spaces) survives.
                std::wstring args = L"/d /s /c \"\"" + az + L"\" account get-access-token --resource https://graph.microsoft.com --output json";
                if (!tenantId.empty())
                {
                    args += L" --tenant " + tenantId;
                }
                args += L"\"";
                ClaudeCaptureResult r = RunClaudeCapture(SystemCmdExe(), args, L"", 30000);
                if (!r.started)
                {
                    state = L"could not start (" + r.error + L")";
                }
                else if (r.timedOut)
                {
                    state = L"did not answer within 30 s";
                }
                else if (r.exitCode != 0)
                {
                    // `az` says "Please run 'az login' to setup account." (or names the tenant) when it has no sign-in.
                    if (r.err.find(L"az login") != std::wstring::npos)
                    {
                        state = tenantId.empty() ? std::wstring{ L"run az login" } : L"run az login --tenant " + tenantId;
                    }
                    else
                    {
                        const std::wstring said = detail::FirstLine(r.err, 160);
                        state = L"failed (exit code " + std::to_wstring(r.exitCode) + (said.empty() ? std::wstring{} : L": " + said) + L")";
                    }
                }
                else if (auto answer = ParseAzCliTokenAnswer(r.out))
                {
                    got = MakeAccessToken(std::move(answer->accessToken), answer->expiresMs, TeamsNowMs());
                    Wipe(answer->accessToken);
                    state = L"ready";
                }
                else
                {
                    state = L"gave no token";
                }
                Wipe(r.out); // the token was in the child's output
            }
        }
        catch (...)
        {
            // Rule #18; recovery: no token from this run, and the next one waits a minute.
            LogSwallowedException(L"AzureCliSource::_run");
            got.reset();
            state = L"failed (hooks.log says why)";
        }
        try
        {
            std::wstring line;
            {
                std::lock_guard lk{ _mtx };
                const bool changed = state != _state;
                _state = state;
                _running = false;
                if (got)
                {
                    _cached = got;
                    _lastFailureMs = 0;
                }
                else
                {
                    _lastFailureMs = TeamsNowMs();
                }
                if (got)
                {
                    line = L"[teams] Azure CLI: a token for " + (got->claims.name.empty() ? std::wstring{ L"?" } : got->claims.name) + L" until " +
                           ClockWord(got->expiresMs) + L" (" + ScopesWord(got->claims) + L")";
                }
                else if (changed)
                {
                    line = L"[teams] Azure CLI: " + state;
                }
            }
            if (!line.empty())
            {
                Log(line);
            }
        }
        catch (...)
        {
            // Rule #18. The run's end could not be recorded: `_running` may stay set, and the CLI is then not asked again in
            // this process (the status says so through the state it kept). Only a failed lock or allocation gets here.
            LogSwallowedException(L"AzureCliSource::_run (end)");
        }
    }

    void AzureCliSource::Forget() noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            if (_cached)
            {
                Wipe(_cached->token);
            }
            _cached.reset();
            _lastFailureMs = 0; // the next Token runs `az` at once
        }
        catch (...)
        {
            LogSwallowedException(L"AzureCliSource::Forget");
        }
    }

    std::wstring AzureCliSource::StateText() const
    {
        std::lock_guard lk{ _mtx };
        return _state;
    }

    std::wstring AzureCliSource::FindAzCommand()
    {
        // The PATH's folders only (SearchPathW with an explicit path): never the current folder or the app's own, where a
        // stray az.cmd would run with the owner's rights.
        std::wstring pathVar;
        const DWORD need = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (need > 0)
        {
            pathVar.resize(need);
            const DWORD got = ::GetEnvironmentVariableW(L"PATH", pathVar.data(), need);
            pathVar.resize(got < need ? got : 0);
        }
        std::vector<wchar_t> buf(32768);
        if (!pathVar.empty())
        {
            for (const wchar_t* name : { L"az.cmd", L"az.bat", L"az.exe" })
            {
                const DWORD n = ::SearchPathW(pathVar.c_str(), name, nullptr, static_cast<DWORD>(buf.size()), buf.data(), nullptr);
                if (n > 0 && n < buf.size())
                {
                    return std::wstring(buf.data(), n);
                }
            }
        }
        // The installer's folder, for a PATH that lost it (an app started before the CLI was installed keeps its old PATH).
        for (const wchar_t* var : { L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)" })
        {
            const DWORD n = ::GetEnvironmentVariableW(var, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0 || n >= buf.size())
            {
                continue;
            }
            const std::wstring candidate = std::wstring(buf.data(), n) + L"\\Microsoft SDKs\\Azure\\CLI2\\wbin\\az.cmd";
            const DWORD attributes = ::GetFileAttributesW(candidate.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                return candidate;
            }
        }
        return {};
    }

    // ===== the allowed app ===================================================================================================

    AppSource::AppSource(std::wstring profileDir) :
        _profileDir(std::move(profileDir))
    {
    }

    std::wstring AppSource::CredentialTarget(std::wstring_view profileDir)
    {
        std::wstring leaf = Leaf(profileDir);
        if (leaf.empty())
        {
            leaf = L"default";
        }
        return L"Agentmaster:messaging:teams:" + leaf;
    }

    void AppSource::_resetSignInLocked() noexcept
    {
        if (_signIn)
        {
            Wipe(_signIn->deviceCode);
        }
        _signIn.reset();
        _signInExpiresMs = 0;
        _signInNextPollMs = 0;
    }

    void AppSource::Configure(std::wstring clientId, std::wstring tenant) noexcept
    {
        try
        {
            if (tenant.empty())
            {
                tenant = L"organizations";
            }
            std::lock_guard lk{ _mtx };
            if (clientId == _clientId && tenant == _tenant)
            {
                return;
            }
            // Another app or tenant: what this source held belongs to the old one.
            _clientId = std::move(clientId);
            _tenant = std::move(tenant);
            if (_cached)
            {
                Wipe(_cached->token);
            }
            _cached.reset();
            _resetSignInLocked();
            _refreshRejected = false;
            _lastRefreshFailureMs = 0;
            _state.clear();
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::Configure");
        }
    }

    bool AppSource::Configured() const noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            return IsClientIdWord(_clientId) && IsTenantWord(_tenant);
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::Configured");
            return false;
        }
    }

    bool AppSource::HasStoredSignIn() const noexcept
    {
        try
        {
            std::wstring clientId;
            {
                std::lock_guard lk{ _mtx };
                clientId = _clientId;
            }
            if (clientId.empty())
            {
                return false;
            }
            std::wstring token = ReadRefreshToken(CredentialTarget(_profileDir), clientId);
            const bool has = !token.empty();
            Wipe(token);
            return has;
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::HasStoredSignIn");
            return false;
        }
    }

    std::optional<AccessToken> AppSource::Token(GraphClient& http, bool force) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                if (!force && _cached && _cached->FreshAt(TeamsNowMs()))
                {
                    return _cached;
                }
                if (_clientId.empty())
                {
                    return std::nullopt;
                }
            }
            // One refresh at a time: a second caller waits here, then finds the first one's token fresh below.
            std::lock_guard refreshLk{ _refreshMtx };
            std::wstring clientId;
            std::wstring tenant;
            std::optional<AccessToken> usable;
            {
                std::lock_guard lk{ _mtx };
                const int64_t now = TeamsNowMs();
                if (!force && _cached && _cached->FreshAt(now))
                {
                    return _cached;
                }
                if (!force && _cached && _cached->UsableAt(now))
                {
                    usable = _cached;
                }
                if (_refreshRejected)
                {
                    return usable; // only a new sign-in helps
                }
                if (!force && _lastRefreshFailureMs > 0 && now - _lastRefreshFailureMs < 30'000)
                {
                    return usable; // the sign-in service did not answer moments ago
                }
                clientId = _clientId;
                tenant = _tenant;
            }
            std::wstring refresh = ReadRefreshToken(CredentialTarget(_profileDir), clientId);
            if (refresh.empty())
            {
                std::lock_guard lk{ _mtx };
                _state = L"sign in needed";
                // The service asks every second: Credential Manager is read again after the 30-s gap, or at once after a
                // sign-in (PollSignIn clears the mark).
                _lastRefreshFailureMs = TeamsNowMs();
                return usable;
            }
            std::string form = "client_id=" + FormUrlEncode(clientId) + "&grant_type=refresh_token&refresh_token=" + FormUrlEncode(refresh) +
                               "&scope=" + FormUrlEncode(kAppScope);
            Wipe(refresh);
            GraphResult r = http.PostForm(tenant, L"token", form, L"token refresh");
            Wipe(form);
            TokenAnswer answer = ParseTokenAnswer(r.body);
            const int64_t now = TeamsNowMs();
            if (!answer.accessToken.empty())
            {
                AccessToken t = MakeAccessToken(std::move(answer.accessToken), answer.expiresInSec > 0 ? now + answer.expiresInSec * 1000 : 0, now);
                Wipe(answer.accessToken);
                if (!answer.refreshToken.empty())
                {
                    // Microsoft may ROTATE the refresh token: the newest is kept. An older one keeps working for a while, so a
                    // store that fails is logged, not fatal.
                    StoreRefreshToken(CredentialTarget(_profileDir), clientId, answer.refreshToken);
                    Wipe(answer.refreshToken);
                }
                std::lock_guard lk{ _mtx };
                if (_clientId != clientId)
                {
                    return std::nullopt; // the app changed during the refresh: this token is the old app's
                }
                _cached = t;
                _lastRefreshFailureMs = 0;
                _state = L"ready";
                return t;
            }
            std::wstring line;
            {
                std::lock_guard lk{ _mtx };
                if (r.aborted)
                {
                    return usable; // the service is stopping
                }
                if (r.refused)
                {
                    _state = r.description;
                    return usable;
                }
                if (r.transportFailed || r.httpStatus == 429 || r.httpStatus >= 500)
                {
                    _lastRefreshFailureMs = now;
                    _state = L"could not reach the sign-in service";
                    return usable;
                }
                if (r.Ok())
                {
                    // A success without a token is no refusal of the refresh token: tried again after the gap.
                    _lastRefreshFailureMs = now;
                    _state = L"the sign-in service gave no token";
                    return usable;
                }
                // A 4xx: Microsoft refused this refresh token for good — it expired or was revoked (a password change, a
                // consent removed), or the app no longer takes it. Only a new sign-in helps.
                _refreshRejected = true;
                if (_cached)
                {
                    Wipe(_cached->token);
                }
                _cached.reset();
                _state = L"sign in again: " + (answer.error.message.empty() ? answer.error.code : answer.error.message);
                line = L"[teams] the app's sign-in was refused (" + answer.error.code + L"): sign in again";
            }
            Log(line);
            return std::nullopt;
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::Token");
            return std::nullopt;
        }
    }

    void AppSource::Forget() noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            if (_cached)
            {
                Wipe(_cached->token);
            }
            _cached.reset();
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::Forget");
        }
    }

    bool AppSource::BeginSignIn(GraphClient& http, std::wstring& error) noexcept
    {
        try
        {
            std::wstring clientId;
            std::wstring tenant;
            {
                std::lock_guard lk{ _mtx };
                clientId = _clientId;
                tenant = _tenant;
            }
            if (!IsClientIdWord(clientId))
            {
                error = L"no app is set (teamsClientId)";
                return false;
            }
            if (!IsTenantWord(tenant))
            {
                error = L"the tenant is not a tenant id, a domain, \"organizations\" or \"common\" (teamsTenantId)";
                return false;
            }
            const std::string form = "client_id=" + FormUrlEncode(clientId) + "&scope=" + FormUrlEncode(kAppScope);
            GraphResult r = http.PostForm(tenant, L"devicecode", form, L"sign-in code");
            auto answer = ParseDeviceCodeAnswer(r.body);
            if (!answer)
            {
                error = r.Ok() ? std::wstring{ L"the sign-in service gave no code" } : r.description;
                std::lock_guard lk{ _mtx };
                _state = L"sign-in failed: " + error;
                return false;
            }
            const int64_t now = TeamsNowMs();
            const std::wstring uri = answer->verificationUri;
            const std::wstring code = answer->userCode;
            const int64_t minutes = answer->expiresInSec / 60;
            {
                std::lock_guard lk{ _mtx };
                if (_clientId != clientId)
                {
                    Wipe(answer->deviceCode);
                    error = L"the app changed during the sign-in";
                    return false;
                }
                _resetSignInLocked();
                _signInExpiresMs = now + answer->expiresInSec * 1000;
                _signInIntervalMs = answer->intervalSec * 1000;
                _signInNextPollMs = now + _signInIntervalMs;
                _signIn = std::move(*answer);
                _state = L"sign in: open " + uri + L" and enter " + code;
            }
            // The address and the user code are what the owner must see (the cog shows them too); the device code — the
            // secret that redeems the tokens — is never written anywhere.
            Log(L"[teams] sign in: open " + uri + L" and enter " + code + L" (the code is good for " + std::to_wstring(minutes) + L" minutes)");
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::BeginSignIn");
            error = L"the sign-in could not start (hooks.log says why)";
            return false;
        }
    }

    SignInPoll AppSource::PollSignIn(GraphClient& http, int64_t nowMs, std::wstring& error) noexcept
    {
        try
        {
            std::wstring clientId;
            std::wstring tenant;
            std::wstring deviceCode;
            {
                std::lock_guard lk{ _mtx };
                if (!_signIn)
                {
                    error = L"no sign-in waits";
                    return SignInPoll::Failed;
                }
                if (nowMs >= _signInExpiresMs)
                {
                    _resetSignInLocked();
                    _state = L"the sign-in code expired";
                    error = _state;
                    return SignInPoll::Expired;
                }
                if (nowMs < _signInNextPollMs)
                {
                    return SignInPoll::Pending;
                }
                _signInNextPollMs = nowMs + _signInIntervalMs;
                clientId = _clientId;
                tenant = _tenant;
                deviceCode = _signIn->deviceCode;
            }
            std::string form = "grant_type=" + FormUrlEncode(L"urn:ietf:params:oauth:grant-type:device_code") + "&client_id=" + FormUrlEncode(clientId) +
                               "&device_code=" + FormUrlEncode(deviceCode);
            Wipe(deviceCode);
            GraphResult r = http.PostForm(tenant, L"token", form, L"sign-in");
            Wipe(form);
            if (r.transportFailed)
            {
                return SignInPoll::Pending; // no answer (or the stop): the next interval asks again, until the code expires
            }
            TokenAnswer answer = ParseTokenAnswer(r.body);
            const SignInPoll poll = ClassifySignInPoll(answer);
            if (poll == SignInPoll::Pending)
            {
                return poll;
            }
            if (poll == SignInPoll::SlowDown)
            {
                std::lock_guard lk{ _mtx };
                _signInIntervalMs += 5000; // RFC 8628 §3.5
                _signInNextPollMs = nowMs + _signInIntervalMs;
                return poll;
            }
            if (poll == SignInPoll::Done)
            {
                AccessToken t = MakeAccessToken(std::move(answer.accessToken), answer.expiresInSec > 0 ? nowMs + answer.expiresInSec * 1000 : 0, nowMs);
                Wipe(answer.accessToken);
                bool stored = false;
                if (!answer.refreshToken.empty())
                {
                    stored = StoreRefreshToken(CredentialTarget(_profileDir), clientId, answer.refreshToken);
                    Wipe(answer.refreshToken);
                }
                const std::wstring who = t.claims.name.empty() ? std::wstring{ L"?" } : t.claims.name;
                const std::wstring scopes = ScopesWord(t.claims);
                {
                    std::lock_guard lk{ _mtx };
                    _resetSignInLocked();
                    _refreshRejected = false;
                    _lastRefreshFailureMs = 0;
                    if (_clientId == clientId)
                    {
                        _cached = std::move(t);
                    }
                    _state = stored ? std::wstring{ L"ready" } :
                                      std::wstring{ L"ready for this run only (no refresh token came, or Credential Manager refused it)" };
                }
                Log(L"[teams] signed in to the app as " + who + L" (" + scopes + L")" + (stored ? std::wstring{} : std::wstring{ L"; the sign-in was not stored" }));
                return poll;
            }
            // Declined, expired or refused: this sign-in ends; the cog's Sign in starts a new one.
            error = answer.error.message.empty() ? answer.error.code : answer.error.message;
            {
                std::lock_guard lk{ _mtx };
                _resetSignInLocked();
                _state = L"sign-in failed: " + error;
            }
            Log(L"[teams] the app's sign-in ended without a token (" + answer.error.code + L")");
            return poll;
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::PollSignIn");
            error = L"the sign-in failed (hooks.log says why)";
            return SignInPoll::Failed;
        }
    }

    std::optional<PendingSignIn> AppSource::Pending() const
    {
        std::lock_guard lk{ _mtx };
        if (!_signIn)
        {
            return std::nullopt;
        }
        PendingSignIn p;
        p.userCode = _signIn->userCode;
        p.verificationUri = _signIn->verificationUri;
        p.expiresMs = _signInExpiresMs;
        return p;
    }

    void AppSource::SignOut() noexcept
    {
        try
        {
            const bool cleared = ClearRefreshToken(CredentialTarget(_profileDir));
            {
                std::lock_guard lk{ _mtx };
                if (_cached)
                {
                    Wipe(_cached->token);
                }
                _cached.reset();
                _resetSignInLocked();
                _refreshRejected = false;
                _state = L"signed out";
            }
            Log(cleared ? L"[teams] signed out of the app (the stored sign-in was deleted)" : L"[teams] signed out of the app (a stored part could not be deleted)");
        }
        catch (...)
        {
            LogSwallowedException(L"AppSource::SignOut");
        }
    }

    std::wstring AppSource::StateText() const
    {
        std::lock_guard lk{ _mtx };
        return _state;
    }
}
