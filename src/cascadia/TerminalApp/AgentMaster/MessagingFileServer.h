// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the Telegram FILE LINKS' server (MESSAGING.md §21.32.4) -- the Windows half.
//
// One small HTTP/1.1 server per install, run by the messaging service while the bot runs and file links are on. It answers
// exactly one kind of request, "GET|HEAD /station-<station>/<hash>/<path>" (MessagingFileLinks.h), with the file -- and only
// after the service's authorizer said yes (the hash is one of its sessions', the path one that session linked). It talks
// only to loopback and private-network peers: the fastrun relay on the k3s nodes, the LAN, the sysd VPN's NAT address. The
// public route (cc.sysd.cc) reaches it only through Cloudflare's edge rules, the tunnel on this PC and the relay.
//
// Also here: the session-key secret in Windows Credential Manager, and the network adapters' addresses the cog offers.
//
// This header names no Windows type (the server's state is an opaque Core), so the service and the cog include it without
// <winsock2.h>'s ordering rules; the .cpp is the one TU that includes Winsock.

#pragma once

#include "MessagingFileLinks.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Agentmaster::Msg
{
    /// <summary>What the service decides for one request (FileLinkServer's authorizer).</summary>
    struct FileServeDecision
    {
        /// <summary>200 = serve <see cref="path"/>; any other status is answered as it is (404 for every refusal the requester
        /// must not tell apart: an unknown key, a file the session never linked, an expired link).</summary>
        int status{ 404 };
        /// <summary>The file to serve when 200.</summary>
        std::wstring path;
        /// <summary>The session the key named ("" when none), for the log.</summary>
        std::wstring sessionId;
        /// <summary>Why it was refused ("unknown key", "not linked", "expired", ...), for the log.</summary>
        std::wstring reason;
        /// <summary>The cog's excluded folders (ParseFileLinkExcludedFolders' keys), checked against the FINAL path after the open
        /// too: a symlink in a linkable folder that points into an excluded one is refused like one that points at a key.</summary>
        std::vector<std::wstring> excludedKeys;
    };

    /// <summary>The service's authorizer: called on a connection's thread for every well-formed request; must not throw (a throw
    /// is caught and answered 500).</summary>
    using FileServeAuthorizer = std::function<FileServeDecision(const FileLinkTarget&)>;

    /// <summary>The log sink: one line per request and per start/stop (the service writes them to hooks.log).</summary>
    using FileServeLogger = std::function<void(const std::wstring&)>;

    /// <summary>
    /// The file links' HTTP server. Start binds 0.0.0.0:&lt;port&gt; and serves until Stop; each connection runs on its own short
    /// thread (at most kMaxConnections at once — a busy server answers 503), with a 15 s read and a 60 s write timeout.
    /// </summary>
    /// <remarks>
    /// Lifetime: the accept thread and the connection threads hold the server's Core by shared_ptr, so a connection that is
    /// still sending when Stop gives up waiting (5 s) can never touch freed memory; Stop shuts every live socket down first,
    /// which ends a transfer in flight at once. ⚠ The authorizer and the logger are called on those threads: whatever they
    /// capture must outlive the server or be held weakly (the service captures itself weakly).
    /// </remarks>
    class FileLinkServer
    {
    public:
        /// <summary>The most connections served at once; the next one gets 503 at once.</summary>
        static constexpr uint32_t kMaxConnections = 24;

        /// <summary>Makes a stopped server.</summary>
        /// <param name="authorize">The service's decision per request.</param>
        /// <param name="log">The log sink.</param>
        FileLinkServer(FileServeAuthorizer authorize, FileServeLogger log);

        /// <summary>Stops the server (Stop).</summary>
        ~FileLinkServer();

        FileLinkServer(const FileLinkServer&) = delete;
        FileLinkServer& operator=(const FileLinkServer&) = delete;

        /// <summary>Binds 0.0.0.0:&lt;port&gt; (exclusively; 127.0.0.1 with <paramref name="loopbackOnly"/>) and starts serving.</summary>
        /// <remarks>
        /// Port 0 binds a free port the system picks (<see cref="Port"/> then names it): the tests' way. Loopback-only is the tests'
        /// way too: a program that listens on every address makes this workstation's firewall ask "allow access?" (NotifyOnListen)
        /// unless a rule names it, and the app's rules name only Agentmaster.exe of the layout folders. Start and Stop are called
        /// from one thread at a time (the service's lifecycle lock); the getters from any thread.
        /// </remarks>
        /// <param name="port">The TCP port (0 = any free port).</param>
        /// <param name="error">Why it failed (a Winsock error number and what it means), when it returns false.</param>
        /// <param name="loopbackOnly">Listen on 127.0.0.1 only (no other machine can reach it).</param>
        /// <returns>True when listening. False when the port is taken, Winsock refused, or the server already runs.</returns>
        bool Start(uint16_t port, std::wstring& error, bool loopbackOnly = false) noexcept;

        /// <summary>Stops accepting, ends every connection (their sockets are shut down) and waits up to 5 s for their threads.</summary>
        void Stop() noexcept;

        /// <summary>True between a successful Start and Stop. Any thread.</summary>
        /// <returns>True when listening.</returns>
        bool Running() const noexcept;

        /// <summary>The port it listens on (0 when stopped). Any thread.</summary>
        /// <returns>The port.</returns>
        uint16_t Port() const noexcept;

        /// <summary>The server's totals since Start.</summary>
        struct Totals
        {
            uint64_t requests{ 0 };
            uint64_t served{ 0 };
            uint64_t refused{ 0 };
            uint64_t bytes{ 0 };
        };

        /// <summary>The totals since Start (for the cog's status line). Any thread.</summary>
        /// <returns>The counts.</returns>
        Totals Counts() const noexcept;

        /// <summary>The server's state, shared with its threads. Opaque outside MessagingFileServer.cpp (declared public only so
        /// the .cpp's connection function can name it; no other file can see its members).</summary>
        struct Core;

    private:
        /// <summary>The current run's state; Stop replaces it (a fresh one for the next Start). Read and written under
        /// `_coreMtx`, so the getters may run on any thread while Start or Stop runs on another.</summary>
        std::shared_ptr<Core> _core;
        /// <summary>Guards `_core` (never held across a socket call or a join).</summary>
        mutable std::mutex _coreMtx;
        /// <summary>The accept loop's thread.</summary>
        std::thread _acceptThread;
        /// <summary>Between a successful Start and Stop: what Running() answers without touching the thread object.</summary>
        std::atomic<bool> _running{ false };

        /// <summary>The current core, copied under `_coreMtx`.</summary>
        /// <returns>The core.</returns>
        std::shared_ptr<Core> _Current() const noexcept;
    };

    /// <summary>The private IPv4 addresses of this PC's adapters that are up (no loopback), virtual adapters marked.</summary>
    /// <remarks>The cog's dropdown lists a URL for each (ComposeFileLinkLanChoices). Never throws: an error yields an empty list,
    /// and the dropdown then offers only the PC's name.</remarks>
    /// <returns>The addresses, in the adapters' order.</returns>
    std::vector<FileLinkLanAddress> EnumerateFileLinkLanAddresses() noexcept;

    /// <summary>This PC's DNS host name ("" when Windows refuses).</summary>
    /// <returns>The name.</returns>
    std::wstring FileLinkHostName() noexcept;

    /// <summary>
    /// Reads the install's session-key secret from Windows Credential Manager, or makes one (32 bytes from the system RNG) and
    /// stores it when there is none or the stored one is damaged.
    /// </summary>
    /// <remarks>The secret is per install (the target names the profile folder), never in a file, never printed. A new one
    /// changes every session's key: every file link the bot ever posted stops working.</remarks>
    /// <param name="target">The credential's target name ("Agentmaster:messaging:telegram-filelinks:&lt;profile leaf&gt;").</param>
    /// <param name="secret">The secret.</param>
    /// <param name="created">True when a new secret was made now.</param>
    /// <param name="error">Why it failed, when it returns false.</param>
    /// <returns>True when <paramref name="secret"/> holds the install's secret.</returns>
    bool LoadOrCreateFileLinkSecret(const std::wstring& target, std::array<uint8_t, 32>& secret, bool& created, std::wstring& error) noexcept;

    /// <summary>Makes and stores a NEW secret (ending every link the bot posted).</summary>
    /// <param name="target">The credential's target name.</param>
    /// <param name="secret">The new secret.</param>
    /// <param name="error">Why it failed, when it returns false.</param>
    /// <returns>True when stored.</returns>
    bool ReplaceFileLinkSecret(const std::wstring& target, std::array<uint8_t, 32>& secret, std::wstring& error) noexcept;
}
