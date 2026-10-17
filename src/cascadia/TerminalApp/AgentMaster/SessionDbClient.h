// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's CLIENT (SESSIONS_DB.md §4.2, §9): how the GUI (its refresh tick,
// the Sessions page, the AI search), the agentmaster CLI and the stdio MCP server reach the per-profile
// service (agentmaster-sessiondb.exe).
//
// What it does:
//   - Connect: open the profile's pipe; when no service answers, start one (under the profile's START
//     mutex, so two clients that find none at the same moment start ONE) and wait for its pipe.
//   - Versions (§4.2): a client NEWER than the running service asks it to `shutdown` and starts its own;
//     an older client never replaces a newer service.
//   - Call: one framed request, its framed answer, with a timeout; CancelPending from another thread.
//   - EnsureServiceBinaries: the GUI copies the service and duckdb.dll out of the package into
//     <profile>\sessiondb\bin (a running copy is renamed aside), so an MCP server that lives for hours never
//     pins the package against a hot swap or an update.
//
// No DuckDB, no JSON library in this header (the GUI includes it): requests and answers are JSON TEXT; the
// caller builds the params and parses the answer with whatever it uses.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace Agentmaster::SessionDb
{
    class ServiceClient
    {
    public:
        // profileDir — the profile the service belongs to (its pipe and files derive from it).
        explicit ServiceClient(std::wstring profileDir);
        ~ServiceClient();
        ServiceClient(const ServiceClient&) = delete;
        ServiceClient& operator=(const ServiceClient&) = delete;

        // Connect to the profile's service.
        //   start  — start a service when none answers (and replace an OLDER one, §4.2); false = connect only.
        //   waitMs — how long to wait for a started service's pipe to appear.
        //   error  — out: one sentence when it returns false.
        // Returns true when connected. Never throws (beyond std::bad_alloc).
        bool Connect(bool start, uint32_t waitMs, std::wstring& error);
        // Whether the pipe is open.
        bool Connected() const noexcept
        {
            return _pipe != nullptr;
        }
        // Close the pipe (an in-flight Call fails). Never throws.
        void Close() noexcept;

        // Send one request and wait for its answer.
        //   method     — a SessionDbProtocol method name.
        //   paramsJson — the params object as JSON text ("{}" for none; must be valid JSON).
        //   response   — out: the answer envelope as JSON text: {"id", "result"} or {"id", "error": {code, message}}.
        //   timeoutMs  — how long to wait; on a timeout the pipe is closed (a late answer would otherwise be read
        //                as the next request's), and the next Call needs a Connect.
        //   error      — out: one sentence when it returns false.
        // Returns false when the pipe failed, closed or timed out. One Call at a time per client.
        bool Call(std::string_view method, std::string_view paramsJson, std::string& response, uint32_t timeoutMs, std::wstring& error);

        // Ask the service to interrupt the request this client's Call is waiting for (call it from another
        // thread). The Call then returns the service's 499 answer. No-op when nothing is pending. Never throws.
        void CancelPending() noexcept;
        // Make a Call that waits in ANOTHER thread return false at once (an app that stops must not wait out a
        // long request, such as a sweep during the cold build). The pipe stays open until its owner closes it
        // (Close from the Call's own thread); every later Call fails until a new Connect. Never throws.
        void Abort() noexcept;

        // The service executable for a profile: the copied-out <profile>\sessiondb\bin\agentmaster-sessiondb.exe
        // when it exists, else the one beside the calling process. "" when neither exists.
        static std::wstring FindServiceExe(const std::wstring& profileDir);
        // Start a service for the profile, detached: no inherited handles, no console window, outside the
        // caller's job when the job allows it (an MCP server's claude.exe must not take the service with it).
        // Returns false with `error` when it could not start.
        static bool LaunchService(const std::wstring& profileDir, std::wstring& error);

    private:
        bool _open(std::wstring& error);
        bool _write(std::string_view frame, std::wstring& error);
        bool _readExact(char* buf, uint32_t n, uint32_t timeoutMs, std::wstring& error);
        bool _helloCheck(bool start, uint32_t waitMs, std::wstring& error);

        std::wstring _profile;
        void* _pipe{ nullptr }; // HANDLE (overlapped)
        void* _readEvent{ nullptr }; // HANDLE
        void* _writeEvent{ nullptr }; // HANDLE
        std::mutex _writeMtx; // Call's request and CancelPending's frame never interleave
        std::atomic<uint64_t> _nextId{ 1 };
        std::atomic<uint64_t> _pendingId{ 0 }; // the request a Call waits for (0 = none)
        std::atomic<bool> _aborted{ false }; // Abort() was called: every wait returns at its next slice
    };

    // Copy agentmaster-sessiondb.exe and duckdb.dll from `sourceDir` (the package or unzip folder) into
    // <profile>\sessiondb\bin when they differ by size or write time (§4.2 "Deploy and update"). A copy that
    // is running is renamed aside (`<leaf>.old-<pid>-<tick>`; Windows allows renaming a running image) and the
    // new one takes the stable name; old renamed copies are deleted when nothing holds them.
    // Returns false with `error` when the source files are missing or a copy failed. Never throws.
    bool EnsureServiceBinaries(const std::wstring& profileDir, const std::wstring& sourceDir, std::wstring& error);

    /// <summary>
    /// True when the session database service CAN start for `profileDir`: the agentmaster-sessiondb.exe that
    /// ServiceClient::FindServiceExe would start (the copied-out one in &lt;profile&gt;\sessiondb\bin, else the package's)
    /// has duckdb.dll beside it — the import the service cannot start without.
    /// </summary>
    /// <param name="profileDir">The profile whose service would run.</param>
    /// <returns>True when both files are there; false otherwise, and on any failure to look (logged).</returns>
    /// <remarks>
    /// A file check only (no process is started, nothing is copied), cheap enough for the AI search to ask once when it
    /// creates its conversation (SearchDatabaseProfileIfUsable). Every loose layout built before 2026-10-17 (the stock
    /// bin\x64\&lt;Config&gt; and every -LayoutDir hot-deploy slot) shipped the service WITHOUT duckdb.dll — the wapproj's
    /// Content item never reached a build folder that is registered as it is, until CopyAgentmasterDuckDbToLayout — which
    /// is the case this answers false for. Never throws.
    /// </remarks>
    bool SessionDbServiceInstalled(const std::wstring& profileDir) noexcept;
}
