// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, the FILE LINKS (see Messaging.h, MESSAGING.md §21.32.4): the links Claude writes to files on this PC
// ("[notes.md](file:///K:/source/x/notes.md)") made openable from the phone. One more TU of the one service class, beside
// Messaging.cpp and the others: the settings (Apply), the LINKED SET (filelinks.json), the session keys, the server's lifecycle
// and its authorizer, the rewrite of the mirror's and the history's items, /download and its "📤 Send as file" button. The pure
// rules are MessagingFileLinks.h; the server, the secret and the LAN addresses are MessagingFileServer.{h,cpp}.
//
// The user's decisions (2026-10-20, §21.32.3): a link is <base>/station-<station>/<session key>/<the exact path>; the session key
// is a SHA-256 with secret initial words on both sides (no HMAC); Cloudflare lets through only that shape (cc.sysd.cc: its edge
// rules, the tunnel connector on this PC and the fastrun relay — K:\source\proxmox\cc-sysd); /download delivers the same way,
// with an optional upload through Telegram itself.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId, LogSwallowedException
#include "ProfileBootstrap.h" // Profiles::IsDevPackage, Profiles::detail::WideToUtf8 / Utf8ToWide
#include "SessionRegistry.h"

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>How often a changed linked set is written at most: a busy mirror refreshes link times every few seconds, and
        /// a link made in those seconds is lost only by a crash (the app's last window and Stop write it at once).</summary>
        constexpr int64_t kFileLinksSaveGapMs = 10 * 1000;
        /// <summary>How often expired links (kFileLinkLifetimeMs after their last mention) are dropped.</summary>
        constexpr int64_t kFileLinksPruneGapMs = 60 * 60 * 1000;
        /// <summary>How often the summary line of the links made is logged (only when some were made or kept).</summary>
        constexpr int64_t kFileLinksStatsGapMs = 10 * 60 * 1000;
        /// <summary>How long a server that could not start waits for the next try: a port still held by an instance that is
        /// exiting frees within seconds, a port another program holds may free later.</summary>
        constexpr int64_t kFileLinksServerRetryMs = 60 * 1000;
        /// <summary>How often the LAN addresses are read again (a new Wi-Fi network, a VPN adapter that came up).</summary>
        constexpr int64_t kFileLinksLanRefreshMs = 30 * 60 * 1000;
        /// <summary>The most "📤 Send as file" offers kept; the oldest go first, and an older button then asks for /download again.</summary>
        constexpr size_t kFileLinksMaxOffers = 200;
        /// <summary>The most uploads read at once: each holds up to 50 MB in memory until the send loop has sent it.</summary>
        constexpr int kFileLinksMaxUploads = 2;
        /// <summary>The largest filelinks.json read; a bigger file is damaged (4,000 files a session would not reach it), and the
        /// set then starts empty instead of filling memory.</summary>
        constexpr int64_t kFileLinksStoreMaxBytes = 64ll << 20;

        /// <summary>Now, as Unix ms (the clock the linked set's times use).</summary>
        /// <returns>The time.</returns>
        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>The last segment of a path (the profile folder's leaf names the credential).</summary>
        /// <param name="path">The path.</param>
        /// <returns>The leaf ("" for an empty path).</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring Leaf(std::wstring_view path)
        {
            std::wstring p{ path };
            while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            {
                p.pop_back();
            }
            const size_t cut = p.find_last_of(L"\\/");
            return cut == std::wstring::npos ? p : p.substr(cut + 1);
        }

        /// <summary>
        /// One line into hooks.log, from any thread, never throwing: the server's logger (its connection threads must not need
        /// the service to log) and the worker of an upload.
        /// </summary>
        /// <param name="line">The line, without the newline.</param>
        void LogLine(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"hooks.log", line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the logger itself failed; nothing is left to report through (Rule #18's logging-path
                // exemption). Degradation: the line is lost.
            }
        }

        /// <summary>
        /// An atomic file write: a temp sibling, flushed, then moved over the target — a crash leaves the old file or the new one,
        /// never half of one (Messaging.cpp's state.json recipe; that copy is file-local).
        /// </summary>
        /// <param name="path">The target.</param>
        /// <param name="bytes">The content.</param>
        /// <returns>True when the target holds the new content.</returns>
        bool WriteFileAtomic(const std::wstring& path, const std::string& bytes) noexcept
        {
            const std::wstring tmp = path + L".tmp";
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
            ::FlushFileBuffers(h);
            ::CloseHandle(h);
            if (!ok)
            {
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        }

        /// <summary>
        /// Reads a whole file of at most <paramref name="maxBytes"/>, sharing every mode (an editor may hold it open and write it).
        /// </summary>
        /// <param name="path">The file.</param>
        /// <param name="maxBytes">The largest size read; a bigger file is refused (nothing read).</param>
        /// <param name="out">The bytes.</param>
        /// <param name="error">Why it failed, when it returns false.</param>
        /// <returns>True when <paramref name="out"/> holds the whole file.</returns>
        bool ReadWholeFile(const std::wstring& path, uint64_t maxBytes, std::string& out, std::wstring& error) noexcept
        {
            try
            {
                out.clear();
                HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    error = L"cannot open it (error " + std::to_wstring(::GetLastError()) + L")";
                    return false;
                }
                struct CloseFile
                {
                    HANDLE h;
                    ~CloseFile() { ::CloseHandle(h); }
                } closeFile{ h };
                // A device (a name a check before missed) would block the read: disk files only.
                LARGE_INTEGER size{};
                if (::GetFileType(h) != FILE_TYPE_DISK || !::GetFileSizeEx(h, &size) || size.QuadPart < 0)
                {
                    error = L"not a file on a disk";
                    return false;
                }
                if (static_cast<uint64_t>(size.QuadPart) > maxBytes)
                {
                    error = L"bigger than " + std::to_wstring(maxBytes >> 20) + L" MB";
                    return false;
                }
                out.resize(static_cast<size_t>(size.QuadPart));
                size_t at = 0;
                while (at < out.size())
                {
                    const DWORD want = static_cast<DWORD>((std::min)(out.size() - at, size_t{ 8u << 20 }));
                    DWORD got = 0;
                    if (!::ReadFile(h, out.data() + at, want, &got, nullptr))
                    {
                        error = L"read failed (error " + std::to_wstring(::GetLastError()) + L")";
                        out.clear();
                        return false;
                    }
                    if (got == 0)
                    {
                        // The file shrank while it was read: what was read is the file now.
                        out.resize(at);
                        break;
                    }
                    at += got;
                }
                return true;
            }
            catch (...)
            {
                // Rule #18. Recovery: nothing is read (a 50 MB buffer that could not be allocated, most likely).
                LogSwallowedException(L"Messaging.Files ReadWholeFile");
                out.clear();
                error = L"out of memory";
                return false;
            }
        }

        /// <summary>What the file system says of a path at link time.</summary>
        struct FileFacts
        {
            /// <summary>Something exists at the path.</summary>
            bool exists{ false };
            /// <summary>It is a folder.</summary>
            bool directory{ false };
            /// <summary>Its size in bytes (a file).</summary>
            uint64_t size{ 0 };
        };

        /// <summary>The path's facts, from its attributes (no open: a link check must not touch the file's sharing or times).</summary>
        /// <param name="path">The path.</param>
        /// <returns>The facts (nothing exists when Windows refuses).</returns>
        FileFacts StatFile(const std::wstring& path) noexcept
        {
            FileFacts f;
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
            {
                return f;
            }
            f.exists = true;
            f.directory = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            f.size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            return f;
        }

        /// <summary>A size as a person reads it ("812 B", "4.2 KB", "12.3 MB").</summary>
        /// <param name="n">The bytes.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring FormatBytes(uint64_t n)
        {
            if (n < 1024)
            {
                return std::to_wstring(n) + L" B";
            }
            wchar_t buf[32];
            if (n < (1024ull << 10))
            {
                swprintf_s(buf, L"%.1f KB", static_cast<double>(n) / 1024.0);
            }
            else if (n < (1024ull << 20))
            {
                swprintf_s(buf, L"%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
            }
            else
            {
                swprintf_s(buf, L"%.2f GB", static_cast<double>(n) / (1024.0 * 1024.0 * 1024.0));
            }
            return buf;
        }

        /// <summary>A text's first and last whitespace removed (the cog's boxes).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The trimmed text.</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring Trimmed(std::wstring_view s)
        {
            const size_t a = s.find_first_not_of(L" \t\r\n");
            if (a == std::wstring_view::npos)
            {
                return {};
            }
            const size_t b = s.find_last_not_of(L" \t\r\n");
            return std::wstring{ s.substr(a, b - a + 1) };
        }
    }

    // ===== the credential, the status line ================================================================================

    std::wstring MessagingService::FileLinksCredentialTarget(std::wstring_view profileDir)
    {
        // Per profile like the bot token (TokenCredentialTarget): the release and dev installs never share keys.
        std::wstring leaf = Leaf(profileDir);
        if (leaf.empty())
        {
            leaf = L"default";
        }
        return L"Agentmaster:messaging:telegram-filelinks:" + leaf;
    }

    std::wstring MessagingService::FileLinksStatusText() const
    {
        FileLinkMode mode;
        std::wstring station;
        uint16_t port;
        std::wstring base;
        std::wstring error;
        size_t files;
        size_t sessions;
        std::shared_ptr<FileLinkServer> server;
        {
            std::lock_guard lk{ _filesMtx };
            mode = _filesMode;
            station = _filesStation;
            port = _filesPort;
            base = ChooseFileLinkBase(_filesMode, _filesLanPick, _filesLanChoices, _filesHostedBase);
            error = _filesServerError;
            files = _filesStore.FileCount();
            sessions = _filesStore.sessions.size();
            server = _filesServer; // a copy: the counts are read outside the lock (the server's counters are atomic)
        }
        if (mode == FileLinkMode::Off)
        {
            return L"Off: links to files stay as Claude wrote them (file:///\x2026), which the phone cannot open.";
        }
        std::wstring line;
        if (!_running.load())
        {
            line = L"Waiting for the bot: the server runs while the bot runs.";
        }
        else if (!error.empty())
        {
            line = L"Not serving: " + error + L" (tried again every minute).";
        }
        else if (server && server->Running())
        {
            const FileLinkServer::Totals t = server->Counts();
            line = L"Serving on port " + std::to_wstring(server->Port()) + L" as station " + station + L"; links point to " +
                   (base.empty() ? std::wstring{ L"(no LAN address found)" } : base) + L". " + std::to_wstring(t.served) + L" served, " +
                   std::to_wstring(t.refused) + L" refused since the start.";
        }
        else
        {
            line = L"Starting on port " + std::to_wstring(port) + L"\x2026";
        }
        return line + L"\n" + std::to_wstring(files) + L" file(s) linked in " + std::to_wstring(sessions) + L" session(s); a link lives " +
               std::to_wstring(kFileLinkLifetimeMs / (24ll * 60 * 60 * 1000)) + L" days after Claude last mentioned the file.";
    }

    // ===== the settings, the lifecycle =====================================================================================

    void MessagingService::_filesApply(const AppSettings& settings) noexcept
    {
        try
        {
            const bool dev = Profiles::IsDevPackage();
            const FileLinkMode mode = ParseFileLinkMode(settings.messagingFileLinksMode);
            // A station or a base the box cannot hold safely falls back to the install's default (the cog shows the box red).
            std::wstring station = NormalizeFileLinkStation(settings.messagingFileLinkStation);
            if (station.empty())
            {
                station = DefaultFileLinkStation(dev);
            }
            const uint32_t typedPort = settings.messagingFileLinkPort;
            const uint16_t port = typedPort == 0 || typedPort > 65535 ? DefaultFileLinkPort(dev) : static_cast<uint16_t>(typedPort);
            std::wstring lanPick = NormalizeFileLinkBase(settings.messagingFileLinkLanBase);
            std::wstring hostedBase = NormalizeFileLinkBase(settings.messagingFileLinkHostedBase);
            std::vector<std::wstring> excluded = ParseFileLinkExcludedFolders(settings.messagingFileLinksExclude);
            bool stationChanged = false;
            {
                std::lock_guard lk{ _filesMtx };
                _filesDevInstall = dev;
                if (station != _filesStation)
                {
                    // Every key depends on the station: the old ones would name a station the relay no longer routes here.
                    stationChanged = !_filesStation.empty();
                    _filesStation = std::move(station);
                    _filesRehashLocked();
                }
                _filesMode = mode;
                _filesPort = port;
                _filesLanPick = std::move(lanPick);
                _filesHostedBase = std::move(hostedBase);
                _filesExcluded = std::move(excluded);
            }
            if (stationChanged)
            {
                _log(L"[file-link] the station changed: every link posted before names the old one and stops working");
            }
            // While the bot runs, the server follows the settings at once (a new port, the mode switched off); before the bot
            // runs, _filesStart does it. _filesReconcileServer reads `_filesBotUp` under its own lock, so a Stop racing this call
            // can never leave a server running.
            _filesReconcileServer();
        }
        catch (...)
        {
            // Rule #18. Recovery: the previous settings stay in force.
            LogSwallowedException(L"MessagingService::_filesApply");
        }
    }

    void MessagingService::_filesStart() noexcept
    {
        try
        {
            bool load = false;
            {
                std::lock_guard lk{ _filesMtx };
                load = !_filesStoreLoaded;
            }
            if (load)
            {
                // Once per process: a restart of the bot keeps the set it has in memory (Stop saved it).
                const std::wstring file = _profile + L"\\messaging\\telegram\\filelinks.json";
                FileLinkStore store;
                std::string bytes;
                std::wstring readError;
                bool damaged = false;
                if (::GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    if (ReadWholeFile(file, kFileLinksStoreMaxBytes, bytes, readError))
                    {
                        if (const auto doc = json::Parse(Profiles::detail::Utf8ToWide(bytes)))
                        {
                            store = FileLinkStoreFromJson(*doc);
                        }
                        else
                        {
                            damaged = true;
                        }
                    }
                    else
                    {
                        damaged = true;
                    }
                }
                const int64_t now = WallMs();
                const size_t dropped = store.Prune(now, kFileLinkLifetimeMs, kFileLinkMaxFilesPerSession);
                size_t files = 0;
                size_t sessions = 0;
                {
                    std::lock_guard lk{ _filesMtx };
                    _filesStore = std::move(store);
                    _filesStoreLoaded = true;
                    _filesDirty = dropped > 0;
                    _filesPrunedMs = now;
                    _filesSavedMs = now;
                    _filesStatsLoggedMs = now;
                    _filesRehashLocked(); // a no-op until the secret is read (the reconcile below reads it and rehashes)
                    files = _filesStore.FileCount();
                    sessions = _filesStore.sessions.size();
                }
                if (damaged)
                {
                    // Kept on disk as it is (the next save replaces it): a person may want to look at it.
                    _log(L"[file-link] filelinks.json could not be read" + (readError.empty() ? std::wstring{ L" (not JSON)" } : L" (" + readError + L")") +
                         L": the linked set starts empty, and every link posted before answers 404");
                }
                _log(L"[file-link] the linked set: " + std::to_wstring(files) + L" file(s) of " + std::to_wstring(sessions) + L" session(s)" +
                     (dropped > 0 ? L", " + std::to_wstring(dropped) + L" expired dropped" : std::wstring{}));
            }
            {
                std::lock_guard life{ _filesLifeMtx };
                _filesBotUp = true;
            }
            _filesReconcileServer();
        }
        catch (...)
        {
            // Rule #18. Recovery: file links stay off for this run of the bot (no server); the mirror keeps Claude's links.
            LogSwallowedException(L"MessagingService::_filesStart");
        }
    }

    void MessagingService::_filesStop() noexcept
    {
        try
        {
            {
                std::lock_guard life{ _filesLifeMtx };
                _filesBotUp = false;
                std::shared_ptr<FileLinkServer> server;
                {
                    std::lock_guard lk{ _filesMtx };
                    server = std::move(_filesServer);
                    _filesServer.reset();
                    _filesServerError.clear();
                }
                if (server)
                {
                    // OUTSIDE `_filesMtx`: Stop waits for the connection threads, and their authorizer takes that lock.
                    const uint16_t port = server->Port();
                    server->Stop();
                    _log(L"[file-link] server stopped (port " + std::to_wstring(port) + L"): the bot stopped");
                }
            }
            _filesSaveStore(true);
        }
        catch (...)
        {
            // Rule #18. Recovery: a server that could not be stopped here stops with the process; the set is saved at the next
            // save (or lost to the last 10 s).
            LogSwallowedException(L"MessagingService::_filesStop");
        }
    }

    void MessagingService::_filesReconcileServer() noexcept
    {
        try
        {
            std::lock_guard life{ _filesLifeMtx };
            FileLinkMode mode;
            uint16_t port;
            bool secretReady;
            std::shared_ptr<FileLinkServer> running;
            std::wstring previousError;
            {
                std::lock_guard lk{ _filesMtx };
                mode = _filesMode;
                port = _filesPort;
                secretReady = _filesSecretReady;
                running = _filesServer;
                previousError = _filesServerError;
                _filesServerTriedMs = WallMs();
            }
            const bool want = _filesBotUp && mode != FileLinkMode::Off;

            // 1. The secret, read (or made) the first time a server is wanted: never while file links are off, so an install
            //    that never turns them on never gets a credential.
            if (want && !secretReady)
            {
                std::array<uint8_t, 32> secret{};
                bool created = false;
                std::wstring error;
                if (LoadOrCreateFileLinkSecret(FileLinksCredentialTarget(_profile), secret, created, error))
                {
                    const std::array<uint32_t, 8> iv = FileLinkInitialStateFromSecret(secret);
                    ::SecureZeroMemory(secret.data(), secret.size());
                    {
                        std::lock_guard lk{ _filesMtx };
                        _filesIv = iv;
                        _filesSecretReady = true;
                        _filesRehashLocked();
                    }
                    secretReady = true;
                    _log(created ? L"[file-link] a new secret was made and stored in Credential Manager (" + FileLinksCredentialTarget(_profile) + L")" :
                                   L"[file-link] the secret was read from Credential Manager");
                }
                else
                {
                    ::SecureZeroMemory(secret.data(), secret.size());
                    const std::wstring reason = L"no secret (" + error + L")";
                    {
                        std::lock_guard lk{ _filesMtx };
                        _filesServerError = reason;
                    }
                    if (reason != previousError)
                    {
                        _log(L"[file-link] the secret could not be read or made: " + error + L" (no server; tried again every minute)");
                    }
                }
            }
            const bool serve = want && secretReady;

            // 2. A server that must not run, or runs on another port, stops — OUTSIDE `_filesMtx` (its threads take it).
            if (running && (!serve || running->Port() != port || !running->Running()))
            {
                {
                    std::lock_guard lk{ _filesMtx };
                    if (_filesServer == running)
                    {
                        _filesServer.reset();
                    }
                }
                const uint16_t oldPort = running->Port();
                running->Stop();
                _log(L"[file-link] server stopped (port " + std::to_wstring(oldPort) + L")" +
                     (!serve ? std::wstring{ L": file links are off" } : L": it moves to port " + std::to_wstring(port)));
                running.reset();
            }

            // 3. A server that should run and does not starts. Its LAN addresses are read here (the "lan" mode's fallback base).
            if (serve && !running)
            {
                std::vector<FileLinkUrlChoice> choices = ComposeFileLinkLanChoices(EnumerateFileLinkLanAddresses(), FileLinkHostName(), port);
                std::weak_ptr<MessagingService> weak = weak_from_this();
                // The authorizer holds the service WEAKLY: the server's threads may outlive a service that is going away, and
                // they then refuse with 503 instead of touching freed memory.
                auto server = std::make_shared<FileLinkServer>(
                    [weak](const FileLinkTarget& target) -> FileServeDecision {
                        if (auto self = weak.lock())
                        {
                            return self->_filesAuthorize(target);
                        }
                        FileServeDecision d;
                        d.status = 503;
                        d.reason = L"the bot service is gone";
                        return d;
                    },
                    [](const std::wstring& line) { LogLine(line); });
                std::wstring error;
                if (server->Start(port, error))
                {
                    std::wstring base;
                    std::wstring station;
                    {
                        std::lock_guard lk{ _filesMtx };
                        _filesServer = server;
                        _filesServerError.clear();
                        _filesLanChoices = std::move(choices);
                        _filesLanReadMs = WallMs();
                        base = ChooseFileLinkBase(_filesMode, _filesLanPick, _filesLanChoices, _filesHostedBase);
                        station = _filesStation;
                    }
                    _log(L"[file-link] serving on port " + std::to_wstring(port) + L" as station " + station + L" (" + std::wstring{ FileLinkModeWord(mode) } +
                         L" mode); links point to " + (base.empty() ? std::wstring{ L"nothing yet: no LAN address found" } : base));
                }
                else
                {
                    const std::wstring reason = L"port " + std::to_wstring(port) + L": " + error;
                    {
                        std::lock_guard lk{ _filesMtx };
                        _filesServerError = reason;
                        _filesLanChoices = std::move(choices);
                    }
                    if (reason != previousError)
                    {
                        _log(L"[file-link] the server could not start on " + reason + L" (tried again every minute)");
                    }
                }
            }
            else if (!want)
            {
                // Off (or the bot is down): no reason to show.
                std::lock_guard lk{ _filesMtx };
                _filesServerError.clear();
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the server stays as it was; the tick tries again a minute later when it is not running.
            LogSwallowedException(L"MessagingService::_filesReconcileServer");
        }
    }

    void MessagingService::_filesTick(int64_t nowMs) noexcept
    {
        try
        {
            std::wstring stats;
            bool retryServer = false;
            bool refreshLan = false;
            uint16_t port = 0;
            {
                std::lock_guard lk{ _filesMtx };
                if (_filesStoreLoaded && nowMs - _filesPrunedMs >= kFileLinksPruneGapMs)
                {
                    _filesPrunedMs = nowMs;
                    const size_t dropped = _filesStore.Prune(nowMs, kFileLinkLifetimeMs, kFileLinkMaxFilesPerSession);
                    if (dropped > 0)
                    {
                        _filesDirty = true;
                        _filesRehashLocked(); // the maps forget the sessions that have no link left
                        stats = L"[file-link] " + std::to_wstring(dropped) + L" expired link(s) dropped; " + std::to_wstring(_filesStore.FileCount()) +
                                L" file(s) of " + std::to_wstring(_filesStore.sessions.size()) + L" session(s) remain";
                    }
                }
                if (_filesLinksMade + _filesLinksKept > 0 && nowMs - _filesStatsLoggedMs >= kFileLinksStatsGapMs)
                {
                    if (!stats.empty())
                    {
                        stats += L"\n";
                    }
                    stats += L"[file-link] in the last " + std::to_wstring((nowMs - _filesStatsLoggedMs) / 60000) + L" min: " + std::to_wstring(_filesLinksMade) +
                             L" link(s) made, " + std::to_wstring(_filesLinksKept) + L" file link(s) kept as Claude wrote them (missing, a folder, a secret or excluded)";
                    _filesLinksMade = 0;
                    _filesLinksKept = 0;
                    _filesStatsLoggedMs = nowMs;
                }
                else if (_filesLinksMade + _filesLinksKept == 0)
                {
                    _filesStatsLoggedMs = nowMs; // the next line covers the next 10 minutes with links, not the quiet ones before
                }
                const bool on = _filesMode != FileLinkMode::Off;
                retryServer = on && !_filesServer && nowMs - _filesServerTriedMs >= kFileLinksServerRetryMs;
                // The LAN addresses of a running server, read again now and then (only the "lan" mode's fallback base uses them).
                refreshLan = on && _filesServer && _filesMode == FileLinkMode::Lan && nowMs - _filesLanReadMs >= kFileLinksLanRefreshMs;
                if (refreshLan)
                {
                    _filesLanReadMs = nowMs;
                    port = _filesPort;
                }
            }
            if (!stats.empty())
            {
                _log(stats);
            }
            _filesSaveStore(false);
            if (refreshLan)
            {
                std::vector<FileLinkUrlChoice> choices = ComposeFileLinkLanChoices(EnumerateFileLinkLanAddresses(), FileLinkHostName(), port);
                std::lock_guard lk{ _filesMtx };
                _filesLanChoices = std::move(choices);
            }
            if (retryServer)
            {
                // A server that could not start (a port in use, a secret Credential Manager refused), or one the bot's start
                // has not made yet: _filesReconcileServer decides, and does nothing while the bot is down.
                _filesReconcileServer();
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: this tick's housekeeping is skipped; the next tick does it.
            LogSwallowedException(L"MessagingService::_filesTick");
        }
    }

    void MessagingService::_filesSaveStore(bool force) noexcept
    {
        try
        {
            // `_filesSaveMtx` orders the writes: a snapshot taken under `_filesMtx` after another is written after it too.
            std::lock_guard saveLock{ _filesSaveMtx };
            std::string bytes;
            {
                std::lock_guard lk{ _filesMtx };
                if (!_filesStoreLoaded || !_filesDirty)
                {
                    return;
                }
                const int64_t now = WallMs();
                if (!force && now - _filesSavedMs < kFileLinksSaveGapMs)
                {
                    return;
                }
                bytes = Profiles::detail::WideToUtf8(json::Dump(FileLinkStoreToJson(_filesStore)));
                _filesDirty = false;
                _filesSavedMs = now;
            }
            const std::wstring file = _profile + L"\\messaging\\telegram\\filelinks.json";
            if (!WriteFileAtomic(file, bytes))
            {
                const DWORD e = ::GetLastError();
                {
                    std::lock_guard lk{ _filesMtx };
                    _filesDirty = true; // the next tick tries again (10 s later)
                }
                _log(L"[file-link] filelinks.json could not be written (error " + std::to_wstring(e) + L")");
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the set stays dirty or not as it was; the next tick tries again.
            LogSwallowedException(L"MessagingService::_filesSaveStore");
        }
    }

    // ===== the keys, the links, the authorizer ============================================================================

    std::string MessagingService::_filesHashForLocked(const std::wstring& sessionId)
    {
        if (const auto it = _filesSessionHash.find(sessionId); it != _filesSessionHash.end())
        {
            return it->second;
        }
        std::string hash = FileLinkSessionHash(_filesIv, _filesStation, sessionId);
        _filesHashToSession[hash] = sessionId;
        _filesSessionHash[sessionId] = hash;
        return hash;
    }

    void MessagingService::_filesRehashLocked()
    {
        _filesHashToSession.clear();
        _filesSessionHash.clear();
        if (!_filesSecretReady || _filesStation.empty())
        {
            return;
        }
        for (const auto& [sid, files] : _filesStore.sessions)
        {
            _filesHashForLocked(sid);
        }
    }

    std::optional<std::wstring> MessagingService::_filesLinkFor(const std::wstring& sessionId, const std::wstring& winPath, const std::wstring& base)
    {
        std::vector<std::wstring> excluded;
        {
            std::lock_guard lk{ _filesMtx };
            excluded = _filesExcluded;
        }
        // The cheap name rules first, then the file system (outside the lock: a slow drive must not hold the authorizer up).
        if (LooksLikeSecretFile(winPath) || IsUnderExcludedFolder(winPath, excluded))
        {
            return std::nullopt;
        }
        const FileFacts f = StatFile(winPath);
        if (!f.exists || f.directory)
        {
            return std::nullopt;
        }
        const auto encoded = EncodeFileLinkPath(winPath);
        if (!encoded)
        {
            return std::nullopt;
        }
        std::wstring station;
        std::string hash;
        {
            std::lock_guard lk{ _filesMtx };
            if (_filesMode == FileLinkMode::Off || !_filesSecretReady || _filesStation.empty())
            {
                return std::nullopt; // switched off meanwhile
            }
            station = _filesStation;
            hash = _filesHashForLocked(sessionId);
            // New, or a mention that renews the link's 30 days: either way the file has something new to save.
            _filesStore.Add(sessionId, winPath, WallMs());
            _filesDirty = true;
        }
        return BuildFileLinkUrl(base, station, hash, *encoded);
    }

    FileServeDecision MessagingService::_filesAuthorize(const FileLinkTarget& target) noexcept
    {
        FileServeDecision d; // 404 until every check passed
        try
        {
            std::vector<std::wstring> excluded;
            {
                std::lock_guard lk{ _filesMtx };
                if (_filesMode == FileLinkMode::Off || !_filesSecretReady)
                {
                    d.reason = L"file links are off";
                    return d;
                }
                if (target.station != _filesStation)
                {
                    d.reason = L"another station (" + target.station + L")";
                    return d;
                }
                const auto s = _filesHashToSession.find(target.hash);
                if (s == _filesHashToSession.end())
                {
                    d.reason = L"unknown key";
                    return d;
                }
                d.sessionId = s->second;
                const FileLinkEntry* e = _filesStore.Find(d.sessionId, target.path);
                if (!e)
                {
                    d.reason = L"not linked by that session";
                    return d;
                }
                if (WallMs() - e->linkedMs > kFileLinkLifetimeMs)
                {
                    d.reason = L"expired";
                    return d;
                }
                d.path = e->path;
                excluded = _filesExcluded;
            }
            // The rules may have changed since the link was made (a folder excluded later): checked again, outside the lock.
            if (LooksLikeSecretFile(d.path) || IsUnderExcludedFolder(d.path, excluded))
            {
                d.path.clear();
                d.reason = L"excluded now";
                return d;
            }
            d.excludedKeys = std::move(excluded); // the server checks the FINAL path against them too
            d.status = 200;
            return d;
        }
        catch (...)
        {
            // Rule #18. Recovery: this request is answered 500 (the server logs it); the next one is decided afresh.
            LogSwallowedException(L"MessagingService::_filesAuthorize");
            FileServeDecision err;
            err.status = 500;
            err.reason = L"internal error";
            return err;
        }
    }

    void MessagingService::_filesRewriteItems(const std::wstring& sessionId, std::vector<TopicItem>& items) noexcept
    {
        try
        {
            std::wstring base;
            {
                std::lock_guard lk{ _filesMtx };
                if (_filesMode == FileLinkMode::Off || !_filesSecretReady)
                {
                    return;
                }
                base = ChooseFileLinkBase(_filesMode, _filesLanPick, _filesLanChoices, _filesHostedBase);
            }
            if (base.empty())
            {
                return; // "lan" with no LAN address: Claude's links stay (they are no worse than a link to nowhere)
            }
            size_t made = 0;
            size_t kept = 0;
            for (auto& item : items)
            {
                // Claude's own text only: a Prompt is the user's (and the mirror matches it against the queue byte for byte),
                // and a tool's input or result is not a message the phone reads as prose.
                if ((item.kind != TopicItemKind::Text && item.kind != TopicItemKind::Plan) || item.text.find(L"](") == std::wstring::npos)
                {
                    continue;
                }
                try
                {
                    FileLinkRewrite r = RewriteFileLinksInMarkdown(item.text, [&](const std::wstring& path) { return _filesLinkFor(sessionId, path, base); });
                    made += r.rewritten;
                    kept += r.kept;
                    if (r.rewritten > 0)
                    {
                        item.text = std::move(r.text);
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: this item keeps Claude's links; the others are still rewritten.
                    LogSwallowedException(L"MessagingService::_filesRewriteItems (an item)");
                }
            }
            if (made + kept > 0)
            {
                std::lock_guard lk{ _filesMtx };
                _filesLinksMade += made;
                _filesLinksKept += kept;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the items keep Claude's links.
            LogSwallowedException(L"MessagingService::_filesRewriteItems");
        }
    }

    // ===== /download and its "📤 Send as file" button =====================================================================

    std::wstring MessagingService::_filesDownloadCommand(const std::wstring& sessionId, const SessionInfo& s, const std::wstring& args, int64_t threadId, int64_t replyTo)
    {
        FileLinkMode mode;
        bool ready;
        std::wstring base;
        std::wstring error;
        std::vector<std::wstring> excluded;
        {
            std::lock_guard lk{ _filesMtx };
            mode = _filesMode;
            ready = _filesSecretReady;
            base = ChooseFileLinkBase(_filesMode, _filesLanPick, _filesLanChoices, _filesHostedBase);
            error = _filesServerError;
            excluded = _filesExcluded;
        }
        if (mode == FileLinkMode::Off)
        {
            _reply(threadId, L"File links are off \x2014 turn them on in Agentmaster \x2192 Settings \x2192 Messaging \x2192 FILE LINKS.", false, json::Value{}, replyTo);
            return L"off";
        }
        if (!ready || base.empty())
        {
            _reply(threadId, L"File links are not ready: " + TgEscapeHtml(!error.empty() ? error : (base.empty() ? std::wstring{ L"no LAN address to link to" } : std::wstring{ L"starting" })) + L".",
                   false, json::Value{}, replyTo);
            return L"not-ready";
        }
        // A relative path is Claude's: relative to its process's folder (where its tools resolve one), else the launch folder.
        const std::wstring dir = s.liveCwd.empty() ? s.workingDir : s.liveCwd;
        const auto path = ResolveDownloadArgument(args, dir);
        if (!path)
        {
            _reply(threadId, L"Usage: <code>/download &lt;path&gt;</code> \x2014 an absolute path, a file:/// link, or a path relative to <code>" + TgEscapeHtml(dir) +
                                 L"</code>.",
                   false, json::Value{}, replyTo);
            return L"usage";
        }
        const std::wstring name = FileLinkFileName(*path);
        if (LooksLikeSecretFile(*path))
        {
            _reply(threadId, L"<code>" + TgEscapeHtml(name) + L"</code> looks like a secret (a key, a token, a credentials file): it is never linked.", false, json::Value{}, replyTo);
            return L"refused:secret";
        }
        if (IsUnderExcludedFolder(*path, excluded))
        {
            _reply(threadId, L"<code>" + TgEscapeHtml(*path) + L"</code> lies in a folder the settings exclude: it is never linked.", false, json::Value{}, replyTo);
            return L"refused:excluded";
        }
        const FileFacts f = StatFile(*path);
        if (!f.exists)
        {
            _reply(threadId, L"No such file: <code>" + TgEscapeHtml(*path) + L"</code>", false, json::Value{}, replyTo);
            return L"refused:missing";
        }
        if (f.directory)
        {
            _reply(threadId, L"<code>" + TgEscapeHtml(*path) + L"</code> is a folder: /download takes a file.", false, json::Value{}, replyTo);
            return L"refused:folder";
        }
        const auto url = _filesLinkFor(sessionId, *path, base);
        if (!url)
        {
            _reply(threadId, L"<code>" + TgEscapeHtml(*path) + L"</code> cannot be linked (a name Windows reserves, or not a drive path).", false, json::Value{}, replyTo);
            return L"refused:unlinkable";
        }
        uint64_t offer = 0;
        if (f.size <= kFileLinkMaxUploadBytes)
        {
            std::lock_guard lk{ _filesMtx };
            offer = _filesNextOffer++;
            _filesOffers[offer] = FileUploadOffer{ sessionId, *path, WallMs() };
            while (_filesOffers.size() > kFileLinksMaxOffers)
            {
                _filesOffers.erase(_filesOffers.begin()); // the oldest offer (numbers only grow)
            }
        }
        std::wstring html = L"\U0001F4CE <b>" + TgEscapeHtml(name) + L"</b> \x00B7 " + FormatBytes(f.size) + L"\n<a href=\"" + TgEscapeAttr(*url) +
                            L"\">\x2B07 Download</a>\n<code>" + TgEscapeHtml(*path) + L"</code>";
        if (mode == FileLinkMode::Lan)
        {
            html += L"\nA LAN link: it opens on the home network only.";
        }
        if (offer == 0)
        {
            html += L"\nToo big to send through Telegram (" + std::to_wstring(kFileLinkMaxUploadBytes >> 20) + L" MB at most): the link is the way.";
        }
        _reply(threadId, html, false, offer == 0 ? json::Value{} : _buttons({ { { L"\U0001F4E4 Send as file", L"dl:" + std::to_wstring(offer) } } }), replyTo);
        return L"ok";
    }

    std::wstring MessagingService::_filesOnTap(const std::wstring& data, int64_t threadId)
    {
        // "dl:<offer number>"
        uint64_t number = 0;
        for (size_t i = 3; i < data.size(); ++i)
        {
            const wchar_t c = data[i];
            if (c < L'0' || c > L'9' || number > 1000000000000ull)
            {
                return L"";
            }
            number = number * 10 + static_cast<uint64_t>(c - L'0');
        }
        FileUploadOffer offer;
        bool found = false;
        FileLinkMode mode;
        std::vector<std::wstring> excluded;
        {
            std::lock_guard lk{ _filesMtx };
            if (const auto it = _filesOffers.find(number); it != _filesOffers.end())
            {
                offer = it->second;
                found = true;
            }
            mode = _filesMode;
            excluded = _filesExcluded;
        }
        if (!found)
        {
            return L"This button is from before a restart (or too old) \x2014 send /download again.";
        }
        if (mode == FileLinkMode::Off)
        {
            return L"File links are off.";
        }
        // Checked again: the rules or the file may have changed since the offer.
        if (LooksLikeSecretFile(offer.path) || IsUnderExcludedFolder(offer.path, excluded))
        {
            return L"That file is excluded now.";
        }
        const FileFacts f = StatFile(offer.path);
        if (!f.exists || f.directory)
        {
            return L"The file is gone.";
        }
        if (f.size > kFileLinkMaxUploadBytes)
        {
            return L"It grew past " + std::to_wstring(kFileLinkMaxUploadBytes >> 20) + L" MB: the link is the way now.";
        }
        if (_filesUploadsInFlight.fetch_add(1) >= kFileLinksMaxUploads)
        {
            _filesUploadsInFlight.fetch_sub(1);
            return L"Two files are being read already \x2014 tap again in a moment.";
        }
        const std::wstring name = FileLinkFileName(offer.path);
        // The read on a worker: a 50 MB file on a slow drive must not hold the poll loop up. The tap stays busy (its holds go
        // with the document's outbox item) until the document is sent.
        std::weak_ptr<MessagingService> weak = weak_from_this();
        std::vector<TapHoldRef> taps = _holdTaps();
        try
        {
            std::thread([weak, offer, threadId, taps, name]() noexcept {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                try
                {
                    const TapScope tapScope{ taps };
                    std::string bytes;
                    std::wstring error;
                    if (ReadWholeFile(offer.path, kFileLinkMaxUploadBytes, bytes, error))
                    {
                        const size_t size = bytes.size();
                        self->_sendDocument(threadId, name, std::move(bytes), L"\U0001F4CE " + offer.path, SendPriority::Final);
                        LogLine(L"[file-link] \"" + name + L"\" (" + FormatBytes(size) + L") of session " + ShortId(offer.sessionId) + L" queued as a document");
                    }
                    else
                    {
                        self->_reply(threadId, L"Could not read <code>" + TgEscapeHtml(name) + L"</code>: " + TgEscapeHtml(error) + L".", false);
                        LogLine(L"[file-link] \"" + name + L"\" of session " + ShortId(offer.sessionId) + L" not sent: " + error);
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: nothing is sent; the user can tap again.
                    LogSwallowedException(L"MessagingService::_filesOnTap (the worker)");
                }
                self->_filesUploadsInFlight.fetch_sub(1);
            }).detach();
        }
        catch (...)
        {
            // Rule #18. Recovery: no thread could be made; the slot is given back and the user can tap again.
            LogSwallowedException(L"MessagingService::_filesOnTap (the thread)");
            _filesUploadsInFlight.fetch_sub(1);
            return L"Could not start the upload \x2014 tap again.";
        }
        return L"Sending " + name + L" (" + FormatBytes(f.size) + L")\x2026";
    }
}
