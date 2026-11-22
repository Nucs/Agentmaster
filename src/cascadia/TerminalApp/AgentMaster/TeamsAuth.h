// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TeamsAuth: the bot-less Teams connector's two token sources (TEAMS_CHANNEL.md §4.1). Both give a DELEGATED
// Graph token of the owner; neither needs an administrator or an app registration of Agentmaster's own.
//
//   * AzureCliSource: `az account get-access-token --resource https://graph.microsoft.com` in a hidden child (ClaudeStatus.h
//     RunClaudeCapture: a kill-on-close job, NUL stdin). Its token carries Group.ReadWrite.All, which posts and EDITS. The
//     run takes 1–3 s, so it never blocks a caller: Token returns what is cached and starts one background run when the
//     cached token is due for renewal.
//   * AppSource: an app the tenant already allows (a public client id and a tenant from the settings). The owner signs in
//     once with the device-code flow; the refresh token goes to Windows Credential Manager (over several credentials when
//     it is longer than one holds: TeamsModels.h §14), and every access token comes from it. Its token typically carries
//     ChannelMessage.Send, ChannelMessage.Read.All and Files.ReadWrite: posts, reads, reactions, uploads.
//
// Which source makes which call is TeamsModels.h PickTokenFor's (the service's router). Nothing here logs a token, a
// refresh token or a device code; a log line names a token's length, its scopes and its expiry at most.
//
// Plain C++ + Win32 in the .cpp (Credential Manager, the hidden child); no WinRT.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "GraphClient.h"
#include "TeamsModels.h"

namespace Agentmaster::Teams
{
    /// <summary>A Graph access token and what it says about itself.</summary>
    struct AccessToken
    {
        /// <summary>⚠ A secret: the bearer token. Never logged.</summary>
        std::wstring token;
        /// <summary>Its claims (scopes, owner, tenant, expiry), read from the token itself.</summary>
        JwtClaims claims;
        /// <summary>When it stops working (Unix ms): the earlier of the answer's lifetime and the token's own `exp`; never 0.</summary>
        int64_t expiresMs{ 0 };

        /// <summary>Has it more than kTokenEarlyRefreshMs left (no renewal needed yet)?</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <returns>True when it is fresh.</returns>
        bool FreshAt(int64_t nowMs) const noexcept
        {
            return !token.empty() && expiresMs - nowMs > kTokenEarlyRefreshMs;
        }

        /// <summary>Can a call still go out with it (more than a minute left)? A renewal may be running meanwhile.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <returns>True when it is usable.</returns>
        bool UsableAt(int64_t nowMs) const noexcept
        {
            return !token.empty() && expiresMs - nowMs > 60'000;
        }
    };

    /// <summary>
    /// Builds an AccessToken from a token text: its claims (a token that is no readable JWT keeps empty claims, and then no
    /// scope routes to it) and its expiry — the earlier of `answerExpiresMs` and the token's `exp`; 30 minutes from now when
    /// neither is known.
    /// </summary>
    /// <param name="token">The token (moved in).</param>
    /// <param name="answerExpiresMs">The expiry its answer named (Unix ms; 0 = none).</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <returns>The token.</returns>
    /// <exception cref="std::bad_alloc">If the claims cannot be allocated.</exception>
    AccessToken MakeAccessToken(std::wstring token, int64_t answerExpiresMs, int64_t nowMs);

    /// <summary>The wall clock (Unix ms) the connector's times use.</summary>
    /// <returns>Milliseconds since 1970-01-01 UTC.</returns>
    int64_t TeamsNowMs() noexcept;

    /// <summary>
    /// The Azure CLI's Graph token (§4.1). Held by shared_ptr: a background run keeps its source alive until it ends, so a
    /// service that stops never waits for `az`.
    /// </summary>
    class AzureCliSource : public std::enable_shared_from_this<AzureCliSource>
    {
    public:
        /// <summary>Creates the source; runs nothing until the first Token.</summary>
        AzureCliSource() = default;
        AzureCliSource(const AzureCliSource&) = delete;
        AzureCliSource& operator=(const AzureCliSource&) = delete;

        /// <summary>
        /// The cached token while it is usable. When it is not fresh (or there is none), starts ONE background `az` run — none
        /// while one runs, and none within 60 s of a failed run, so a missing or signed-out CLI costs one run a minute. Never
        /// blocks on `az`.
        /// </summary>
        /// <param name="tenantId">The tenant whose token is wanted (the app token's `tid`; "" = the CLI's default tenant).
        /// Only a tenant word is passed to `az` (IsTenantWord); a cached token of another tenant (its own `tid`) is not used and
        /// is replaced by a run for this one.</param>
        /// <returns>A usable token, or nothing yet.</returns>
        /// <remarks>
        /// Any thread. Never throws: a failure is in StateText and the log. The run's thread is detached and holds this source
        /// (shared_from_this), so the source must be owned by a std::shared_ptr; a stopping service never waits for `az`.
        /// </remarks>
        std::optional<AccessToken> Token(std::wstring_view tenantId) noexcept;

        /// <summary>Drops the cached token (Graph answered 401 with it): the next Token starts a run at once.</summary>
        /// <remarks>Any thread. Never throws.</remarks>
        void Forget() noexcept;

        /// <summary>The source's state for the status line and the log: "ready", "not found", "run az login", ….</summary>
        /// <returns>The words ("" before the first run).</returns>
        /// <exception cref="std::bad_alloc">If the words cannot be copied.</exception>
        std::wstring StateText() const;

        /// <summary>
        /// The Azure CLI's command (az.cmd, az.bat or az.exe) on the PATH, else in the installer's folders under Program Files.
        /// Only the PATH's folders are searched, never the current folder or the app's own.
        /// </summary>
        /// <returns>The full path, or "" when the CLI is not installed.</returns>
        /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
        static std::wstring FindAzCommand();

    private:
        /// <summary>The background run: `az` in a hidden child, its answer read, the cache and the state set. Never throws.</summary>
        /// <param name="tenantId">As Token (already checked).</param>
        void _run(std::wstring tenantId) noexcept;

        /// <summary>Guards every member below.</summary>
        mutable std::mutex _mtx;
        /// <summary>The cached token.</summary>
        std::optional<AccessToken> _cached;
        /// <summary>A run is in progress.</summary>
        bool _running{ false };
        /// <summary>When the last run failed (Unix ms; 0 = none): the next run waits 60 s after it.</summary>
        int64_t _lastFailureMs{ 0 };
        /// <summary>The state words.</summary>
        std::wstring _state;
    };

    /// <summary>A device-code sign-in that waits for the owner (§4.1): what the status line shows.</summary>
    struct PendingSignIn
    {
        /// <summary>The code the owner types ("ABCD1234").</summary>
        std::wstring userCode;
        /// <summary>The address ("https://microsoft.com/devicelogin").</summary>
        std::wstring verificationUri;
        /// <summary>When the code expires (Unix ms).</summary>
        int64_t expiresMs{ 0 };
    };

    /// <summary>
    /// The allowed app's Graph token (§4.1): the device-code sign-in, the refresh token in Credential Manager, the access token
    /// cached until it is due. Thread-safe; its HTTP calls go through the GraphClient the caller passes (the service's current
    /// one, so the service's Stop aborts them).
    /// </summary>
    class AppSource
    {
    public:
        /// <summary>Creates the source for a profile; reads nothing until it is used.</summary>
        /// <param name="profileDir">The active profile (its folder's last segment keys the credential).</param>
        /// <exception cref="std::bad_alloc">If the folder cannot be copied.</exception>
        explicit AppSource(std::wstring profileDir);
        AppSource(const AppSource&) = delete;
        AppSource& operator=(const AppSource&) = delete;

        /// <summary>
        /// Sets the app and the tenant (teamsClientId, teamsTenantId). A changed client or tenant drops the cached token, a
        /// sign-in in progress and a refused refresh (the stored refresh token of another client is not read for this one).
        /// </summary>
        /// <param name="clientId">The app's client id ("" = no app: the source is off).</param>
        /// <param name="tenant">The tenant ("organizations", "common", a tenant id or a domain).</param>
        /// <remarks>Any thread. Never throws.</remarks>
        void Configure(std::wstring clientId, std::wstring tenant) noexcept;

        /// <summary>Is an app set, with a client id and a tenant of the right form?</summary>
        /// <returns>True when the source can sign in.</returns>
        bool Configured() const noexcept;

        /// <summary>Is a refresh token stored for the configured client (Credential Manager; read on each call)?</summary>
        /// <returns>True when the owner signed in before.</returns>
        /// <remarks>Never throws: a failed read reads as false.</remarks>
        bool HasStoredSignIn() const noexcept;

        /// <summary>
        /// The cached token while it is fresh; otherwise a refresh with the stored refresh token (one at a time: a second caller
        /// waits for the first one's answer). A refresh that Microsoft refuses for good (`invalid_grant`, a revoked consent, a
        /// password change) stops further refreshes until the next sign-in; a failed request waits 30 s before the next try.
        /// </summary>
        /// <param name="http">The client for the refresh.</param>
        /// <param name="force">Refresh even when the cached token is fresh (Graph answered 401 with it).</param>
        /// <returns>A usable token, or nothing.</returns>
        /// <remarks>Blocks for a refresh (one HTTP round trip): a worker thread only. Never throws.</remarks>
        std::optional<AccessToken> Token(GraphClient& http, bool force) noexcept;

        /// <summary>Drops the cached access token (Graph answered 401 with it); the refresh token stays.</summary>
        /// <remarks>Any thread. Never throws.</remarks>
        void Forget() noexcept;

        /// <summary>
        /// Starts a device-code sign-in: asks the identity platform for a code, keeps it (and the device code, a secret) as the
        /// pending sign-in, and logs the address and the code the owner types (`[teams] sign in: …`). Replaces a pending one.
        /// </summary>
        /// <param name="http">The client.</param>
        /// <param name="error">Receives why it failed.</param>
        /// <returns>True when a code waits for the owner.</returns>
        /// <remarks>One HTTP round trip: a worker thread only. Never throws.</remarks>
        bool BeginSignIn(GraphClient& http, std::wstring& error) noexcept;

        /// <summary>
        /// One step of a pending sign-in: when its interval has passed, one ask of the token endpoint. Done stores the refresh
        /// token, caches the access token and ends the sign-in; Declined, Expired and Failed end it with `error` set; Pending and
        /// SlowDown keep it (a request that got no answer counts as Pending: the next interval asks again).
        /// </summary>
        /// <param name="http">The client.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <param name="error">Receives why the sign-in ended without a token.</param>
        /// <returns>Where the sign-in stands; Failed when none waits.</returns>
        /// <remarks>At most one HTTP round trip: a worker thread only. Never throws.</remarks>
        SignInPoll PollSignIn(GraphClient& http, int64_t nowMs, std::wstring& error) noexcept;

        /// <summary>The pending sign-in, for the status line.</summary>
        /// <returns>The sign-in, or nothing when none waits.</returns>
        /// <exception cref="std::bad_alloc">If the texts cannot be copied.</exception>
        std::optional<PendingSignIn> Pending() const;

        /// <summary>Signs out: deletes the stored refresh token and drops the cached token and a pending sign-in.</summary>
        /// <remarks>Any thread. Never throws.</remarks>
        void SignOut() noexcept;

        /// <summary>The source's state for the log and the status line: "ready", "sign in needed", "sign in again: …", ….</summary>
        /// <returns>The words.</returns>
        /// <exception cref="std::bad_alloc">If the words cannot be copied.</exception>
        std::wstring StateText() const;

        /// <summary>
        /// The Credential Manager target of a profile's refresh token: "Agentmaster:messaging:teams:&lt;profile folder leaf&gt;"
        /// (more parts: "…#2", "…#3"). Per profile, like the Telegram bot's token, so the release and dev installs never share a
        /// sign-in.
        /// </summary>
        /// <param name="profileDir">The profile folder.</param>
        /// <returns>The first part's target.</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be built.</exception>
        static std::wstring CredentialTarget(std::wstring_view profileDir);

    private:
        /// <summary>Ends the pending sign-in, its device code overwritten first (it is a secret). Call with `_mtx` held.</summary>
        void _resetSignInLocked() noexcept;

        /// <summary>The profile folder (the credential's key).</summary>
        std::wstring _profileDir;
        /// <summary>Guards every member below.</summary>
        mutable std::mutex _mtx;
        /// <summary>One refresh at a time (taken before `_mtx`, never while holding it).</summary>
        std::mutex _refreshMtx;
        /// <summary>The app's client id ("" = off).</summary>
        std::wstring _clientId;
        /// <summary>The tenant.</summary>
        std::wstring _tenant{ L"organizations" };
        /// <summary>The cached access token.</summary>
        std::optional<AccessToken> _cached;
        /// <summary>The pending sign-in's answer (⚠ holds the device code).</summary>
        std::optional<DeviceCodeAnswer> _signIn;
        /// <summary>When the pending sign-in's code expires (Unix ms).</summary>
        int64_t _signInExpiresMs{ 0 };
        /// <summary>When the token endpoint may be asked next (Unix ms).</summary>
        int64_t _signInNextPollMs{ 0 };
        /// <summary>The interval between asks (ms).</summary>
        int64_t _signInIntervalMs{ 5000 };
        /// <summary>Microsoft refused the refresh token for good: no refresh until the next sign-in.</summary>
        bool _refreshRejected{ false };
        /// <summary>When the last refresh request failed without an answer (Unix ms; 0 = none).</summary>
        int64_t _lastRefreshFailureMs{ 0 };
        /// <summary>The state words.</summary>
        std::wstring _state;
    };
}
