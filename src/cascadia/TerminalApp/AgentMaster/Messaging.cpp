// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging (see Messaging.h): the service's lifecycle, the outbox, the topic sync and
// the transcript mirror. The inbound half (updates, commands, buttons, the Search topic) is in
// Messaging.Inbound.cpp — one class, two TUs, like TerminalPage's partial files.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId, ResolveClaudeTranscriptPath, LogSwallowedException
#include "Persistence.h" // LoadAppSettings, ResolveSessionColorHex
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8 / Utf8ToWide
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "TelegramApi.h"
#include "TranscriptTurns.h" // ParseTranscriptJsonl

#pragma comment(lib, "advapi32.lib") // CredReadW / CredWriteW / CredDeleteW

namespace Agentmaster::Msg
{
    namespace
    {
        // The tick's cadence: the transcript mirror and the topic sync run this often when nothing wakes
        // them sooner (a registry notify does).
        constexpr int64_t kTickMs = 1500;
        // getUpdates' long-poll timeout (seconds) and the receive timeout around it.
        constexpr int64_t kPollTimeoutSec = 40;
        constexpr uint32_t kPollReceiveTimeoutMs = 60000;
        // While Threaded Mode is off, getMe is retried this often (the user turns it on in @BotFather).
        constexpr int64_t kGetMeRetryMs = 60 * 1000;
        // The outbox's pacing: Telegram allows about one message per second in one chat (every topic of
        // the private chat shares it); calls that send no message (reactions, answers to buttons) are
        // spaced less.
        constexpr int64_t kSendGapMs = 1050;
        // ⚠ Edits count against the same per-chat budget as messages: with a 300 ms gap here, the card
        // refreshes of 47 topics (each card's "3m" becoming "4m") ran at ~1 edit/s for ten minutes and
        // Telegram answered an editMessageText with 429 + retry_after 1526 s (2026-10-16). Every call now
        // keeps the ~1/s per-chat pace; what makes it bearable is that nothing periodic edits any more.
        constexpr int64_t kLightGapMs = 1050;
        // How often a work-log message is edited while the turn runs, and how long it may grow before it
        // rolls over into a new message.
        constexpr int64_t kWorkLogEditGapMs = 15 * 1000;
        constexpr size_t kWorkLogRolloverChars = 3200;
        // The card is edited at most this often while the state does not change.
        constexpr int64_t kCardEditGapMs = 20 * 1000;
        // The STICKY CARD (§21.5): a card with newer posts below it is deleted and sent again at the bottom once
        // the posts have been quiet for this long (a reply and its remarks cost ONE re-post, two calls, not one
        // each); a card whose post Telegram refused is tried again after kCardRepostAfterFailureMs, never every tick.
        constexpr int64_t kCardStickyLagMs = 6 * 1000;
        constexpr int64_t kCardRepostAfterFailureMs = 60 * 1000;
        // A new topic reads this much of the transcript's tail (§21.6): the last away_summary and the newest message
        // for the recap rule, and — for a session that is mid-turn — the start of the turn's own prompt line, where
        // the mirror then begins (a turn longer than this starts mirroring from the file's end instead).
        constexpr int64_t kRecapSeedBytes = 256 * 1024;
        // How long a text sent AS THE USER is remembered for the echo guard (_takeSecretaryEcho): Telegram
        // delivers such a message back, if it does, within seconds; two minutes covers a slow poll.
        constexpr int64_t kSecretaryEchoMs = 2 * 60 * 1000;
        // How often the tick loop logs the outbound call census (`[msg] traffic (10 min): …`).
        constexpr int64_t kTrafficLogMs = 10 * 60 * 1000;
        // How much transcript a tick reads per session at most (a busy session writes kilobytes a second;
        // a catch-up after a pause must not stall the tick).
        constexpr size_t kMaxReadPerTick = 4u * 1024u * 1024u;
        // A transcript path that did not resolve is tried again this often — every ~2 s while the session is busy
        // (a NEW session or fork has its topic before its first message, §21.6, and that first prompt is what the
        // file starts with: found late, it would still be read from the start, but the thread would lag), and
        // every 20 s while it rests (nothing will appear until the user types).
        constexpr int64_t kResolveRetryMs = 20 * 1000;
        constexpr int64_t kResolveRetryBusyMs = 2 * 1000;
        // The run marker (§4.3) is rewritten this often; a marker found at start = the previous run ended
        // without a clean stop.
        constexpr int64_t kMarkerMs = 60 * 1000;
        // The state file is saved at most this often while dirty (and at once on Stop).
        constexpr int64_t kStateSaveGapMs = 2000;
        // A Search agent idle this long is parked (its claude.exe holds ~400 MB).
        constexpr int64_t kSearchParkMs = 10 * 60 * 1000;
        // Action tokens live this long (M5: a stale button is refused).
        constexpr int64_t kTokenTtlMs = 2 * 60 * 60 * 1000;
        // How many topics a tick creates or deletes at most (each is an API call on the tick thread).
        constexpr size_t kTopicOpsPerTick = 6;
        // The START-UP HOLD on "closed" topic deletes (§21.4): at an app start every session reads live=false
        // until its window re-homes its tabs (the registry loads sessions.json as archived; each reopened window
        // flips its own tabs live, one by one, in its first layout; a second window registers only when the
        // first one's restore has released the shared UI thread). A "closed" delete of a topic whose session
        // has NOT been seen live in this run is held (a) for kStartupCloseHoldMinMs after the start — the
        // windows that have not registered yet — and (b) while any registered window is still restoring
        // (ServiceHooks::windowsRestoring, Engine::WindowsStillRestoring), up to kStartupCloseHoldCapMs so a
        // restore that never ends cannot hold forever. Measured 2026-10-16 without it: the first tick after a
        // dev swap deleted all 47 topics as "closed" and the next minute re-created 38; with a fixed 120 s
        // hold, one 56-tab window took 72 s of fleet load + 68 s of re-home, and the 16 tabs re-homed after
        // the hold churned again — a restart must never churn the chat.
        constexpr int64_t kStartupCloseHoldMinMs = 120 * 1000;
        constexpr int64_t kStartupCloseHoldCapMs = 30 * 60 * 1000;

        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // FNV-1a of a token, for the one-poller-per-token mutex name (never the token itself).
        std::wstring TokenHash(std::wstring_view token)
        {
            uint64_t h = 1469598103934665603ull;
            for (const wchar_t c : token)
            {
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ull;
            }
            wchar_t buf[24];
            swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(h));
            return buf;
        }

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

        // An atomic UTF-8 file write: a temp sibling, flushed, then moved over the target (the engine's
        // WriteAllUtf8 recipe; Persistence.cpp's is file-local).
        bool WriteFileAtomic(const std::wstring& path, const std::string& bytes)
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

        std::string ReadFileBytes(const std::wstring& path)
        {
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            std::string out;
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16ll << 20))
            {
                out.resize(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (!::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr))
                {
                    out.clear();
                }
                else
                {
                    out.resize(read);
                }
            }
            ::CloseHandle(h);
            return out;
        }

        // Reads [cursor, size) of a transcript (at most kMaxReadPerTick bytes), returning the bytes and
        // the file's size; an unreadable file returns an empty string with size -1.
        std::string ReadTranscriptTail(const std::wstring& path, int64_t cursor, int64_t& sizeOut)
        {
            sizeOut = -1;
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            std::string out;
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size))
            {
                sizeOut = size.QuadPart;
                if (size.QuadPart > cursor)
                {
                    LARGE_INTEGER pos{};
                    pos.QuadPart = cursor;
                    if (::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
                    {
                        const size_t want = static_cast<size_t>(std::min<int64_t>(size.QuadPart - cursor, static_cast<int64_t>(kMaxReadPerTick)));
                        out.resize(want);
                        DWORD read = 0;
                        if (!::ReadFile(h, out.data(), static_cast<DWORD>(want), &read, nullptr))
                        {
                            out.clear();
                        }
                        else
                        {
                            out.resize(read);
                        }
                    }
                }
            }
            ::CloseHandle(h);
            return out;
        }

        // What a new topic learns from the tail of a transcript (§21.6, _seedFromTranscript).
        struct TranscriptTailScan
        {
            int64_t size{ -1 }; // the file's size when read (-1 = unreadable / absent)
            int64_t lastPromptLineAt{ -1 }; // where the newest turn-starting prompt's line begins (-1 = none in the window)
            std::wstring recapText; // the newest away_summary in the window (FoldRecapFacts)
            int64_t recapAtMs{ 0 };
            int64_t lastMessageAtMs{ 0 }; // the newest conversation item's time in the window
        };

        // Reads the last `windowBytes` of a transcript and folds every COMPLETE line of it: the recap facts and the
        // byte offset where the newest turn-starting prompt's line begins (a line torn by the window's start is
        // dropped; a torn last line — still being written — is not read). Lines are parsed one at a time because the
        // offset of each is needed. Throws std::bad_alloc only (the caller contains it).
        TranscriptTailScan ScanTranscriptTail(const std::wstring& path, int64_t windowBytes)
        {
            TranscriptTailScan scan;
            if (path.empty())
            {
                return scan;
            }
            int64_t size = -1;
            ReadTranscriptTail(path, 0x7FFFFFFFFFFFFFFFll, size); // the size only
            scan.size = size;
            if (size <= 0)
            {
                return scan;
            }
            const int64_t from = size > windowBytes ? size - windowBytes : 0;
            int64_t sizeNow = -1;
            const std::string bytes = ReadTranscriptTail(path, from, sizeNow);
            size_t pos = 0;
            if (from > 0)
            {
                const size_t nl = bytes.find('\n');
                if (nl == std::string::npos)
                {
                    return scan; // one line longer than the window: nothing complete to read
                }
                pos = nl + 1;
            }
            while (pos < bytes.size())
            {
                const size_t nl = bytes.find('\n', pos);
                if (nl == std::string::npos)
                {
                    break; // the last line is still being written
                }
                const int64_t lineAt = from + static_cast<int64_t>(pos);
                auto parsed = ParseTranscriptJsonl(bytes.substr(pos, nl - pos));
                pos = nl + 1;
                if (parsed.empty())
                {
                    continue;
                }
                const auto items = ClassifyTranscriptLine(parsed.front());
                for (const auto& it : items)
                {
                    if (it.kind == TopicItemKind::Prompt && !it.midTurn)
                    {
                        scan.lastPromptLineAt = lineAt;
                    }
                }
                FoldRecapFacts(items, scan.recapText, scan.recapAtMs, scan.lastMessageAtMs);
            }
            return scan;
        }

        // FNV-1a 32 of a text with its surrounding whitespace dropped: the identity the secretary echo guard
        // compares (Telegram may trim what it stores).
        uint32_t EchoHash(std::wstring_view text)
        {
            const size_t a = text.find_first_not_of(L" \t\r\n");
            if (a == std::wstring_view::npos)
            {
                return CardTextHash(L"");
            }
            const size_t b = text.find_last_not_of(L" \t\r\n");
            return CardTextHash(text.substr(a, b - a + 1));
        }

        std::wstring StateWord(SessionState s)
        {
            switch (s)
            {
            case SessionState::Running:
                return L"running";
            case SessionState::WaitingForInput:
                return L"waiting";
            case SessionState::NeedsApproval:
                return L"needs-approval";
            case SessionState::Error:
                return L"error";
            case SessionState::Done:
                return L"done";
            default:
                return L"idle";
            }
        }
    }

    // ===== construction / lifecycle ==========================================================

    MessagingService::MessagingService(std::shared_ptr<SessionRegistry> registry, std::shared_ptr<Scheduler> scheduler, std::wstring profileDir, ServiceHooks hooks) :
        _registry(std::move(registry)), _scheduler(std::move(scheduler)), _profile(std::move(profileDir)), _hooks(std::move(hooks))
    {
        _stateFile = _profile + L"\\messaging\\telegram\\state.json";
        _status = L"Off.";
    }

    MessagingService::~MessagingService()
    {
        Stop();
    }

    int64_t MessagingService::_nowMs() const noexcept
    {
        return WallMs();
    }

    void MessagingService::_log(const std::wstring& line) const noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", line + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE: the logger itself failed; nothing left to report through.
        }
    }

    void MessagingService::_setStatus(std::wstring text)
    {
        std::lock_guard lk{ _mtx };
        _status = std::move(text);
    }

    bool MessagingService::Running() const noexcept
    {
        return _running.load();
    }

    std::wstring MessagingService::StatusText() const
    {
        std::lock_guard lk{ _mtx };
        return _status;
    }

    void MessagingService::SetSearchFactory(SearchBridgeFactory factory) noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            _searchFactory = std::move(factory);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::SetSearchFactory");
        }
    }

    void MessagingService::Apply(const AppSettings& settings) noexcept
    {
        try
        {
            _idleGraceMs.store(static_cast<int64_t>(ClampMessagingIdleGraceMinutes(settings.messagingIdleGraceMinutes)) * 60 * 1000);
            int64_t owner = 0;
            for (const wchar_t c : settings.messagingTelegramOwnerId)
            {
                if (c >= L'0' && c <= L'9' && owner < 100000000000000ll)
                {
                    owner = owner * 10 + (c - L'0');
                }
            }
            {
                std::lock_guard lk{ _mtx };
                _ownerId = owner;
                _searchThinkingWord = settings.messagingSearchThinking.empty() ? L"high" : settings.messagingSearchThinking;
                // The Search conversation is the process's (§21.7): made once, it is never made again with a new
                // level, so a Save must reach it live. Under `_mtx`, which also orders it after the poll thread's
                // creation of the bridge; the agent calls no listener back into this service for it (its StateChanged
                // event is ignored by the bridge), so holding the lock is safe.
                if (_search)
                {
                    _search->SetThinkingLevel(_searchThinkingWord);
                }
            }
            if (!settings.messagingEnabled)
            {
                if (_running.load())
                {
                    _log(L"[msg] messaging switched off: the bot stops");
                }
                Stop();
                _setStatus(L"Off.");
                return;
            }
            const std::wstring token = ReadStoredToken(_profile);
            if (token.empty())
            {
                Stop();
                _setStatus(L"On, but no bot token is stored: paste the token from @BotFather and Save.");
                return;
            }
            if (owner == 0)
            {
                Stop();
                _setStatus(L"On, but nobody is paired: enter your Telegram user id (numeric) and Save.");
                return;
            }
            if (_running.load())
            {
                // A live reconfiguration: the owner / grace / thinking word were taken above; a token change
                // needs a restart of the poller (the token is baked into the client).
                _tickCv.notify_all();
                return;
            }
            _start(token);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::Apply");
        }
    }

    void MessagingService::_start(std::wstring token) noexcept
    {
        try
        {
            // M7 — one poller per token, machine-wide: Telegram answers a second concurrent getUpdates
            // with 409 and the two readers steal each other's updates. The mutex is named by a hash of the
            // token (never the token).
            const std::wstring mutexName = L"Global\\Agentmaster-telegram-" + TokenHash(token);
            HANDLE m = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
            if (m && ::GetLastError() == ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(m);
                _setStatus(L"Stopped: another Agentmaster instance (or program) is already polling this bot. One bot per install.");
                _log(L"[msg] not started: another process holds the poller mutex for this bot token");
                return;
            }
            if (!m)
            {
                _log(L"[msg] poller mutex could not be created (error " + std::to_wstring(::GetLastError()) + L"); starting anyway");
            }
            _tokenMutex = m;

            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path{ _stateFile }.parent_path(), ec);
            _loadState();
            {
                std::lock_guard lk{ _mtx };
                for (auto& kv : _state.topics)
                {
                    // An "in flight" marker (-1) that a stop mid-send left in the file would wait for an answer that
                    // never comes: no send is in flight at a start, so a card or work log marked so is simply absent.
                    if (kv.second.cardMessageId < 0)
                    {
                        kv.second.cardMessageId = 0;
                    }
                    if (kv.second.workLogMessageId < 0)
                    {
                        kv.second.workLogMessageId = 0;
                    }
                }
                // A business connection stored without a verdict (the app closed before the probe ran) is probed
                // once the Search topic exists.
                _probeWanted.store(_state.businessEnabled && _state.businessCanReply && _state.businessMode == 0 && !_state.businessConnectionId.empty());
            }
            _secretaryFailures.store(0);
            _stop.store(false);
            _api = std::make_unique<TelegramApi>(std::move(token));
            _topicsEnabled.store(false);
            _botId.store(0); // re-arms the poll loop's identity gate: this token may name another bot than the last run's
            _lastGetMeMs = 0;
            _startNotePosted = false;
            _startedMs = WallMs(); // the start-up hold on "closed" deletes counts from here
            _startupHoldLogged = false;
            {
                std::lock_guard lk{ _mtx };
                _mirrors.clear();
                _cards.clear();
                _reacted.clear();
                _outbox.clear();
                _dirty.clear();
                _seenLive.clear();
                _secretaryEchoes.clear();
            }
            // The registry notifies every change of every session; the service only marks the session
            // dirty and wakes the tick (never any work on the notifying thread).
            {
                std::weak_ptr<MessagingService> weak = weak_from_this();
                _registryObserver = _registry->AddObserver([weak](const SessionInfo& s, HookEvent) {
                    if (auto self = weak.lock())
                    {
                        self->_onRegistryNotify(s);
                    }
                });
            }
            _running.store(true);
            _setStatus(L"Starting\x2026");
            _pollThread = std::thread([this]() noexcept { _pollLoop(); });
            _sendThread = std::thread([this]() noexcept { _sendLoop(); });
            _tickThread = std::thread([this]() noexcept { _tickLoop(); });
            _log(L"[msg] messaging started (owner " + std::to_wstring(_ownerId) + L", idle grace " + std::to_wstring(_idleGraceMs.load() / 60000) + L" min)");
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_start");
            _running.store(false);
            _setStatus(L"Failed to start (see hooks.log).");
        }
    }

    void MessagingService::Stop() noexcept
    {
        try
        {
            if (!_running.exchange(false))
            {
                // Threads are down; a token mutex from a refused start is already released.
                return;
            }
            _stop.store(true);
            _tickCv.notify_all();
            _sendCv.notify_all();
            if (_api)
            {
                _api->Abort(); // the long poll returns at once
            }
            if (_registryObserver)
            {
                _registry->RemoveObserver(_registryObserver);
                _registryObserver = 0;
            }
            if (_pollThread.joinable())
            {
                _pollThread.join();
            }
            if (_sendThread.joinable())
            {
                _sendThread.join();
            }
            if (_tickThread.joinable())
            {
                _tickThread.join();
            }
            _saveStateIfDirty(true);
            // A clean stop removes the run marker (a marker found at the next start means a crash).
            ::DeleteFileW((_profile + L"\\messaging\\run.marker").c_str());
            if (_tokenMutex)
            {
                ::ReleaseMutex(static_cast<HANDLE>(_tokenMutex));
                ::CloseHandle(static_cast<HANDLE>(_tokenMutex));
                _tokenMutex = nullptr;
            }
            {
                std::lock_guard lk{ _mtx };
                if (_search)
                {
                    _search->Park(0);
                }
                // Detaches this start's sink only: the Search conversation is the process's and lives on (parked), so
                // the next start of the bot continues it (§21.7).
                _search.reset();
            }
            _api.reset();
            _log(L"[msg] messaging stopped");
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::Stop");
        }
    }

    void MessagingService::_onRegistryNotify(const SessionInfo& s) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                _dirty[s.id] = WallMs();
                _tickRequested = true;
            }
            _tickCv.notify_all();
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_onRegistryNotify");
        }
    }

    // ===== the token (Credential Manager) ==================================================

    std::wstring MessagingService::TokenCredentialTarget(std::wstring_view profileDir)
    {
        std::wstring leaf = Leaf(profileDir);
        if (leaf.empty())
        {
            leaf = L"default";
        }
        return L"Agentmaster:messaging:telegram:" + leaf;
    }

    std::wstring MessagingService::ReadStoredToken(std::wstring_view profileDir) noexcept
    {
        try
        {
            const auto target = TokenCredentialTarget(profileDir);
            PCREDENTIALW cred = nullptr;
            if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
            {
                return {};
            }
            std::wstring token;
            if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t))
            {
                token.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
            }
            ::CredFree(cred);
            while (!token.empty() && (token.back() == L'\0' || token.back() == L' ' || token.back() == L'\r' || token.back() == L'\n'))
            {
                token.pop_back();
            }
            return token;
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::ReadStoredToken");
            return {};
        }
    }

    bool MessagingService::StoreToken(std::wstring_view profileDir, std::wstring_view token) noexcept
    {
        try
        {
            std::wstring value{ token };
            const size_t a = value.find_first_not_of(L" \t\r\n");
            if (a == std::wstring::npos)
            {
                return false;
            }
            const size_t b = value.find_last_not_of(L" \t\r\n");
            value = value.substr(a, b - a + 1);
            auto target = TokenCredentialTarget(profileDir);
            std::wstring user = L"bot";
            CREDENTIALW cred{};
            cred.Type = CRED_TYPE_GENERIC;
            cred.TargetName = target.data();
            cred.UserName = user.data();
            cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
            cred.CredentialBlobSize = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            cred.CredentialBlob = reinterpret_cast<LPBYTE>(value.data());
            const bool ok = ::CredWriteW(&cred, 0) != FALSE;
            if (!ok)
            {
                AppendStateLog(L"hooks.log", L"[msg] could not store the bot token in Credential Manager (error " + std::to_wstring(::GetLastError()) + L")\n");
            }
            return ok;
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::StoreToken");
            return false;
        }
    }

    bool MessagingService::ClearToken(std::wstring_view profileDir) noexcept
    {
        try
        {
            const auto target = TokenCredentialTarget(profileDir);
            if (::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
            {
                return true;
            }
            return ::GetLastError() == ERROR_NOT_FOUND;
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::ClearToken");
            return false;
        }
    }

    MessagingService::BotCheck MessagingService::CheckBot(std::wstring_view token) noexcept
    {
        BotCheck c;
        try
        {
            std::wstring t{ token };
            const size_t a = t.find_first_not_of(L" \t\r\n");
            if (a == std::wstring::npos)
            {
                c.error = L"no token";
                return c;
            }
            const size_t b = t.find_last_not_of(L" \t\r\n");
            TelegramApi api{ t.substr(a, b - a + 1) };
            const TgResult r = api.Call(L"getMe", json::Value::MkObj(), 15000);
            if (!r.ok)
            {
                c.error = r.httpStatus == 401 ? std::wstring{ L"Telegram refused the token (401 Unauthorized)" } : (r.description.empty() ? std::wstring{ L"no answer" } : r.description);
                return c;
            }
            c.ok = true;
            c.username = L"@" + r.result.StrAt(L"username");
            c.topicsEnabled = r.result.BoolAt(L"has_topics_enabled", false);
            c.usersMayCreateTopics = r.result.BoolAt(L"allows_users_to_create_topics", false);
            return c;
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::CheckBot");
            c.error = L"exception";
            return c;
        }
    }

    // ===== the state file ====================================================================

    void MessagingService::_loadState()
    {
        MessagingState loaded;
        const std::string bytes = ReadFileBytes(_stateFile);
        if (!bytes.empty())
        {
            if (const auto doc = json::Parse(Profiles::detail::Utf8ToWide(bytes)))
            {
                if (auto s = StateFromJson(*doc))
                {
                    loaded = std::move(*s);
                }
            }
            else
            {
                // A torn file: try the previous copy (kept at every save), else start fresh — the topics then
                // become orphans the bot cannot find (§10.5) and new ones are created.
                const std::string prev = ReadFileBytes(_stateFile + L".prev");
                if (const auto doc2 = prev.empty() ? std::nullopt : json::Parse(Profiles::detail::Utf8ToWide(prev)))
                {
                    if (auto s = StateFromJson(*doc2))
                    {
                        loaded = std::move(*s);
                        _log(L"[msg] state.json was unreadable; the previous copy was used");
                    }
                }
                else
                {
                    _log(L"[msg] state.json was unreadable and no previous copy could be read: starting with no topic map");
                }
            }
        }
        std::lock_guard lk{ _mtx };
        _state = std::move(loaded);
        _stateDirty = false;
    }

    void MessagingService::_saveStateIfDirty(bool force)
    {
        json::Value doc;
        {
            std::lock_guard lk{ _mtx };
            if (!_stateDirty)
            {
                return;
            }
            const int64_t now = WallMs();
            if (!force && now - _lastStateSaveMs < kStateSaveGapMs)
            {
                return;
            }
            doc = StateToJson(_state);
            _stateDirty = false;
            _lastStateSaveMs = now;
        }
        const std::string bytes = Profiles::detail::WideToUtf8(json::Dump(doc));
        // Keep the previous copy: the Bot API cannot list topics, so this file is their only record.
        ::CopyFileW(_stateFile.c_str(), (_stateFile + L".prev").c_str(), FALSE);
        if (!WriteFileAtomic(_stateFile, bytes))
        {
            _log(L"[msg] state.json could not be written (error " + std::to_wstring(::GetLastError()) + L")");
            std::lock_guard lk{ _mtx };
            _stateDirty = true;
        }
    }

    // ===== the threads =======================================================================

    void MessagingService::_pollLoop() noexcept
    {
        int64_t backoffMs = 1000;
        while (!_stop.load())
        {
            try
            {
                // The IDENTITY GATE: no getUpdates before getMe has named the bot (the tick loop asks it first
                // thing). Update ids are per bot, and an offset carried over from ANOTHER bot's state file
                // would confirm — silently drop — every update of this bot below it, for good. The tick loop
                // resets the state when the bot id changed and only then sets _botId, which opens this gate.
                if (_botId.load() == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
                auto params = json::Value::MkObj();
                int64_t offset = 0;
                {
                    std::lock_guard lk{ _mtx };
                    offset = _state.updateOffset;
                }
                if (offset > 0)
                {
                    params.Set(L"offset", json::Value::MkNum(static_cast<double>(offset)));
                }
                params.Set(L"timeout", json::Value::MkNum(static_cast<double>(kPollTimeoutSec)));
                params.Set(L"limit", json::Value::MkNum(100));
                auto allowed = json::Value::MkArr();
                allowed.Push(json::Value::MkStr(L"message"));
                allowed.Push(json::Value::MkStr(L"edited_message"));
                allowed.Push(json::Value::MkStr(L"callback_query"));
                // The user's "Secretary mode" (Telegram Business → Chatbots): the connection and its rights arrive
                // ONLY as this update, when the user adds, edits or removes the bot there (§21.5).
                allowed.Push(json::Value::MkStr(L"business_connection"));
                params.Set(L"allowed_updates", std::move(allowed));
                const TgResult r = _api->Call(L"getUpdates", params, kPollReceiveTimeoutMs);
                if (_stop.load())
                {
                    break;
                }
                if (!r.ok)
                {
                    if (r.httpStatus == 409)
                    {
                        _setStatus(L"Stopped: Telegram says another poller reads this bot (409). One bot per install.");
                        _log(L"[msg] getUpdates 409 Conflict: another reader polls this bot; the poll loop ends");
                        return;
                    }
                    if (r.httpStatus == 401)
                    {
                        _setStatus(L"Stopped: Telegram refused the bot token (401). Paste a new token from @BotFather.");
                        _log(L"[msg] getUpdates 401: the token was refused; the poll loop ends");
                        return;
                    }
                    // Network loss, sleep, a 5xx: back off 1, 2, 4 … 60 s.
                    const int64_t wait = r.retryAfterSec > 0 ? static_cast<int64_t>(r.retryAfterSec) * 1000 : backoffMs;
                    backoffMs = std::min<int64_t>(backoffMs * 2, 60000);
                    for (int64_t slept = 0; slept < wait && !_stop.load(); slept += 250)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    continue;
                }
                backoffMs = 1000;
                if (r.result.type == json::Value::Type::Arr)
                {
                    for (const auto& u : r.result.arr)
                    {
                        _handleUpdate(u);
                        if (_stop.load())
                        {
                            break;
                        }
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_pollLoop");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    void MessagingService::_sendLoop() noexcept
    {
        while (!_stop.load())
        {
            try
            {
                OutItem item;
                int64_t waitMs = 0;
                {
                    std::unique_lock lk{ _mtx };
                    _sendCv.wait_for(lk, std::chrono::milliseconds(500), [this] { return _stop.load() || !_outbox.empty(); });
                    if (_stop.load())
                    {
                        break;
                    }
                    if (_outbox.empty())
                    {
                        continue;
                    }
                    // The best item: lowest priority value, then the oldest.
                    auto best = _outbox.begin();
                    for (auto it = _outbox.begin(); it != _outbox.end(); ++it)
                    {
                        if (static_cast<int>(it->prio) < static_cast<int>(best->prio) || (it->prio == best->prio && it->seq < best->seq))
                        {
                            best = it;
                        }
                    }
                    const int64_t now = static_cast<int64_t>(::GetTickCount64());
                    const int64_t gap = best->paced ? kSendGapMs : kLightGapMs;
                    if (_lastSendMs != 0 && now - _lastSendMs < gap)
                    {
                        waitMs = gap - (now - _lastSendMs);
                    }
                    else
                    {
                        item = std::move(*best);
                        _outbox.erase(best);
                        _lastSendMs = now;
                    }
                }
                if (waitMs > 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(waitMs, 1200)));
                    continue;
                }
                TgResult r;
                if (item.document)
                {
                    r = _api->SendDocument(item.params, item.fileName, item.fileBytes, 60000);
                }
                else
                {
                    r = _api->Call(item.method, item.params, 30000);
                }
                {
                    // The traffic census the tick loop logs every 10 minutes: one count per method, and the
                    // pacing refusals under "429" — the chat's real call rate, which no other log shows.
                    std::lock_guard lk{ _mtx };
                    ++_trafficByMethod[item.document ? std::wstring{ L"sendDocument" } : item.method];
                    if (!r.ok && r.httpStatus == 429)
                    {
                        ++_trafficByMethod[L"429"];
                    }
                }
                if (!r.ok && r.httpStatus == 429)
                {
                    // Telegram's pacing verdict: wait what it says, then retry the same item once.
                    const int64_t wait = std::max<int64_t>(1, r.retryAfterSec) * 1000;
                    _log(L"[msg] pacing: 429 on " + item.method + L", waiting " + std::to_wstring(wait / 1000) + L" s");
                    for (int64_t slept = 0; slept < wait && !_stop.load(); slept += 250)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    if (item.document)
                    {
                        r = _api->SendDocument(item.params, item.fileName, item.fileBytes, 60000);
                    }
                    else
                    {
                        r = _api->Call(item.method, item.params, 30000);
                    }
                }
                if (item.onDone)
                {
                    try
                    {
                        item.onDone(r);
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"MessagingService::_sendLoop (onDone)");
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_sendLoop");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    void MessagingService::_tickLoop() noexcept
    {
        while (!_stop.load())
        {
            try
            {
                {
                    std::unique_lock lk{ _mtx };
                    _tickCv.wait_for(lk, std::chrono::milliseconds(kTickMs), [this] { return _stop.load() || _tickRequested; });
                    _tickRequested = false;
                }
                if (_stop.load())
                {
                    break;
                }
                const int64_t now = WallMs();
                // The run marker (§4.3): rewritten every minute; found at start = the previous run ended
                // without a clean stop.
                if (now - _lastMarkerMs >= kMarkerMs)
                {
                    const std::wstring marker = _profile + L"\\messaging\\run.marker";
                    if (_lastMarkerMs == 0)
                    {
                        // The first write of this run: a marker still on disk means the previous run ended without
                        // a clean Stop (which deletes it). Keep its content as `.last` for the start note the Search
                        // topic posts (_ensureSearchTopic reads and removes it).
                        const std::string prevText = ReadFileBytes(marker);
                        if (!prevText.empty())
                        {
                            WriteFileAtomic(marker + L".last", prevText);
                        }
                    }
                    WriteFileAtomic(marker, std::to_string(static_cast<long long>(now)));
                    _lastMarkerMs = now;
                }
                // The traffic census: every call the send loop made since the last line, by method. Written
                // even when nothing was sent (a "quiet" line is evidence too); the first line comes 10 minutes
                // after the start.
                if (_lastTrafficLogMs == 0)
                {
                    _lastTrafficLogMs = now;
                }
                else if (now - _lastTrafficLogMs >= kTrafficLogMs)
                {
                    _lastTrafficLogMs = now;
                    std::map<std::wstring, uint32_t> census;
                    {
                        std::lock_guard lk{ _mtx };
                        census.swap(_trafficByMethod);
                    }
                    std::wstring line = L"[msg] traffic (" + std::to_wstring(kTrafficLogMs / 60000) + L" min):";
                    uint32_t total = 0;
                    for (const auto& [method, count] : census)
                    {
                        line += L" " + method + L"=" + std::to_wstring(count);
                        total += count;
                    }
                    line += census.empty() ? std::wstring{ L" nothing sent" } : L" (" + std::to_wstring(total) + L" calls)";
                    _log(line);
                }
                // Threaded Mode: topics need it; until it is on, getMe is re-asked every minute and the owner
                // gets one note with the switch's location.
                if (!_topicsEnabled.load())
                {
                    if (now - _lastGetMeMs >= kGetMeRetryMs)
                    {
                        _lastGetMeMs = now;
                        const TgResult r = _api->Call(L"getMe", json::Value::MkObj(), 15000);
                        if (r.ok)
                        {
                            const std::wstring user = r.result.StrAt(L"username");
                            const int64_t id = r.result.I64At(L"id", 0);
                            std::wstring previousBot; // non-empty = the state file belonged to another bot
                            {
                                std::lock_guard lk{ _mtx };
                                _botUsername = user;
                                // A DIFFERENT bot than the state file's (the token was replaced in the cog): the
                                // update offset, the Search topic and the topic map are the OLD bot's — update ids
                                // are per bot (a stale higher offset would drop every update of the new one), and
                                // the thread ids name topics that exist only in the old bot's chat. The per-session
                                // mirrors and reactions point into that chat too. The new bot starts from nothing.
                                if (_state.botId != 0 && id != 0 && _state.botId != id)
                                {
                                    previousBot = _state.botUsername.empty() ? std::to_wstring(_state.botId) : _state.botUsername;
                                    _state.updateOffset = 0;
                                    _state.searchThreadId = 0;
                                    _state.setupNoticeMs = 0;
                                    _state.topics.clear();
                                    // A business connection (secretary mode) is made to ONE bot: the new bot has none
                                    // until the user adds it under Telegram Business → Chatbots.
                                    _state.businessConnectionId.clear();
                                    _state.businessEnabled = false;
                                    _state.businessCanReply = false;
                                    _state.businessMode = 0;
                                    _state.businessNote.clear();
                                    _probeWanted.store(false);
                                    _mirrors.clear();
                                    _cards.clear();
                                    _reacted.clear();
                                    _stateDirty = true;
                                }
                                if (id != 0 && _state.botId != id)
                                {
                                    _state.botId = id;
                                    _stateDirty = true;
                                }
                                if (_state.botUsername != user)
                                {
                                    _state.botUsername = user;
                                    _stateDirty = true;
                                }
                            }
                            if (!previousBot.empty())
                            {
                                _log(L"[msg] the bot changed (@" + previousBot + L" -> @" + user + L"): the update offset, the Search topic and the topic map of the old bot were reset");
                            }
                            // Opens the poll loop's identity gate — AFTER the reset above, never before it.
                            if (id != 0)
                            {
                                _botId.store(id);
                            }
                            if (r.result.BoolAt(L"has_topics_enabled", false))
                            {
                                _topicsEnabled.store(true);
                                _setStatus(L"Connected as @" + user + L".");
                                _log(L"[msg] connected as @" + user + L" (topics enabled)");
                            }
                            else
                            {
                                // ⚠ The switch exists ONLY in the BotFather MINI APP (tap Open on @BotFather); the classic /mybots
                                // chat menu does not show Threads Settings (the user flipped Secretary Mode there on 2026-10-16,
                                // which is the business-bot setting, and topics stayed off).
                                _setStatus(L"Connected as @" + user + L", waiting for Threaded Mode: open the @BotFather MINI APP (tap Open on @BotFather) \x2192 My bots \x2192 @" + user + L" \x2192 Bot Settings \x2192 Threads Settings \x2192 Threaded Mode \x2192 On (the classic /mybots menu has no such setting). Checked again every minute.");
                                bool sendNotice = false;
                                {
                                    std::lock_guard lk{ _mtx };
                                    // One notice per day at most: the user may take a while, and every
                                    // Agentmaster start must not repeat it.
                                    if (now - _state.setupNoticeMs > 24ll * 3600 * 1000)
                                    {
                                        _state.setupNoticeMs = now;
                                        _stateDirty = true;
                                        sendNotice = true;
                                    }
                                }
                                if (sendNotice)
                                {
                                    _sendText(0,
                                              L"<b>Agentmaster is connected to this bot</b>, but topics are off, so there is nothing to show yet.\n\n"
                                              L"Turn <b>Threaded Mode</b> on in the BotFather <b>Mini App</b> (search botfather, tap <b>Open</b> \x2014 the classic /mybots chat menu does not have this setting): My bots \x2192 @" +
                                                  TgEscapeHtml(user) + L" \x2192 Bot Settings \x2192 <b>Threads Settings</b> \x2192 Threaded Mode \x2192 Turn on. "
                                                                        L"Agentmaster checks again every minute and then creates one topic per working session.",
                                              false,
                                              SendPriority::Final,
                                              L"setup-notice",
                                              json::Value{},
                                              nullptr);
                                    _log(L"[msg] topics are off for @" + user + L": the setup notice was sent");
                                }
                            }
                        }
                        else if (r.httpStatus == 401)
                        {
                            _setStatus(L"Stopped: Telegram refused the bot token (401). Paste a new token from @BotFather.");
                        }
                        else
                        {
                            _setStatus(L"Cannot reach Telegram: " + (r.description.empty() ? std::wstring{ L"no answer" } : r.description) + L". Retrying.");
                        }
                    }
                    _saveStateIfDirty(false);
                    continue;
                }

                _ensureSearchTopic();
                if (_probeWanted.load())
                {
                    int64_t search = 0;
                    {
                        std::lock_guard lk{ _mtx };
                        search = _state.searchThreadId;
                    }
                    if (search != 0)
                    {
                        _probeWanted.store(false);
                        _probeSecretary();
                    }
                }
                _syncTopics();

                // The mirror and the cards, per topic. The topic set is copied under the lock; each
                // session's work reads the registry and the transcript outside it.
                std::vector<std::wstring> ids;
                {
                    std::lock_guard lk{ _mtx };
                    ids.reserve(_state.topics.size());
                    for (const auto& [sid, t] : _state.topics)
                    {
                        if (t.threadId != 0)
                        {
                            ids.push_back(sid);
                        }
                    }
                    // The open-turn count that stretches the work log's edit gap (_flushWorkLog): a pass-old
                    // value is fine, the gap only has to follow the fleet's load, not each turn's edge.
                    size_t open = 0;
                    for (const auto& kv : _mirrors)
                    {
                        if (kv.second.turnOpen)
                        {
                            ++open;
                        }
                    }
                    _openTurns.store(open);
                }
                for (const auto& sid : ids)
                {
                    if (_stop.load())
                    {
                        break;
                    }
                    const auto s = _session(sid);
                    if (!s)
                    {
                        continue;
                    }
                    TopicRecord topic;
                    Mirror mirror;
                    {
                        std::lock_guard lk{ _mtx };
                        const auto it = _state.topics.find(sid);
                        if (it == _state.topics.end())
                        {
                            continue;
                        }
                        topic = it->second;
                        mirror = _mirrors[sid];
                    }
                    _onStateEdge(sid, topic, mirror, *s);
                    _followTranscript(sid, topic, mirror, *s);
                    _flushWorkLog(sid, topic, mirror, false);
                    _syncReactions(*s);
                    _sweepRevives(*s);
                    {
                        std::lock_guard lk{ _mtx };
                        if (const auto it = _state.topics.find(sid); it != _state.topics.end())
                        {
                            // Keep the fields the mirror advanced; the sync may have changed the others (never the
                            // card clocks: _noteTopicPosted and the card's callbacks write those under the lock).
                            auto& rec = it->second;
                            rec.cursor = topic.cursor;
                            rec.transcriptPath = topic.transcriptPath;
                            // A work-log create whose answer landed while this session was processed: the copy still
                            // says "in flight" (-1), the record already holds the answer (the new message's id, or 0
                            // for a refusal; _flushWorkLog marks the record -1 when it sends) — keep it, or the next
                            // tick waits for an answer that already came and the work log stops for the turn.
                            const bool workLogAnswerLanded = topic.workLogMessageId < 0 && rec.workLogMessageId >= 0;
                            if (!workLogAnswerLanded)
                            {
                                rec.workLogMessageId = topic.workLogMessageId;
                            }
                            rec.recapText = topic.recapText;
                            rec.recapAtMs = topic.recapAtMs;
                            rec.lastMessageAtMs = topic.lastMessageAtMs;
                            rec.errorAnnounced = topic.errorAnnounced;
                            _stateDirty = true;
                            auto& cur = _mirrors[sid];
                            if (workLogAnswerLanded && cur.workLogGen == mirror.workLogGen && cur.workLogMessageId > 0)
                            {
                                mirror.workLogMessageId = cur.workLogMessageId;
                                mirror.workLogDirty = true; // what arrived meanwhile goes out as an edit
                            }
                            cur = std::move(mirror);
                        }
                    }
                    _refreshCard(sid, false);
                }
                // Revives for sessions that have no topic yet (a resumed session that is still Idle).
                {
                    std::vector<std::wstring> reviveIds;
                    {
                        std::lock_guard lk{ _mtx };
                        for (const auto& rv : _revives)
                        {
                            if (!rv.req.sessionId.empty())
                            {
                                reviveIds.push_back(rv.req.sessionId);
                            }
                        }
                    }
                    for (const auto& rid : reviveIds)
                    {
                        if (const auto s = _session(rid))
                        {
                            _sweepRevives(*s);
                        }
                    }
                    // A New-session revive has no id: match any live session in its folder.
                    bool anyNew = false;
                    {
                        std::lock_guard lk{ _mtx };
                        for (const auto& rv : _revives)
                        {
                            anyNew = anyNew || rv.req.sessionId.empty();
                        }
                    }
                    if (anyNew)
                    {
                        for (const auto& s : _registry->SnapshotLiveForCards())
                        {
                            _sweepRevives(s);
                        }
                    }
                }
                _sweepTokens();
                // Park an idle Search agent.
                {
                    std::lock_guard lk{ _mtx };
                    if (_search && !_searchParked && now - _searchLastUsedMs > kSearchParkMs)
                    {
                        _search->Park(0);
                        _searchParked = true;
                    }
                }
                _saveStateIfDirty(false);
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_tickLoop");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    std::optional<SessionInfo> MessagingService::_session(const std::wstring& sessionId) const
    {
        return _registry->Get(sessionId);
    }

    // ===== the outbox ========================================================================

    void MessagingService::_enqueue(OutItem item)
    {
        {
            std::lock_guard lk{ _mtx };
            if (!item.key.empty())
            {
                // A newer item with the same key replaces the queued one (an edit of the same message).
                for (auto it = _outbox.begin(); it != _outbox.end();)
                {
                    if (it->key == item.key)
                    {
                        it = _outbox.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            item.seq = ++_outSeq;
            _outbox.push_back(std::move(item));
        }
        _sendCv.notify_one();
    }

    json::Value MessagingService::_buttons(const std::vector<std::vector<std::pair<std::wstring, std::wstring>>>& rows) const
    {
        auto kb = json::Value::MkArr();
        for (const auto& row : rows)
        {
            auto r = json::Value::MkArr();
            for (const auto& [text, data] : row)
            {
                auto b = json::Value::MkObj();
                b.Set(L"text", json::Value::MkStr(text));
                if (data.rfind(L"copy:", 0) == 0)
                {
                    auto ct = json::Value::MkObj();
                    ct.Set(L"text", json::Value::MkStr(data.substr(5)));
                    b.Set(L"copy_text", std::move(ct));
                }
                else
                {
                    b.Set(L"callback_data", json::Value::MkStr(data));
                }
                r.Push(std::move(b));
            }
            kb.Push(std::move(r));
        }
        auto markup = json::Value::MkObj();
        markup.Set(L"inline_keyboard", std::move(kb));
        return markup;
    }

    void MessagingService::_sendText(int64_t threadId, const std::wstring& html, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, bool movesCard)
    {
        // A text past Telegram's limit goes out as several messages; only the LAST carries the buttons
        // and the callback, and only the first the reply reference.
        const auto chunks = TgChunkHtml(html);
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            OutItem item;
            item.prio = prio;
            item.key = (i + 1 == chunks.size()) ? key : std::wstring{};
            item.method = L"sendMessage";
            item.params = json::Value::MkObj();
            item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            if (threadId > 0)
            {
                item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
            }
            item.params.Set(L"text", json::Value::MkStr(chunks[i]));
            item.params.Set(L"parse_mode", json::Value::MkStr(L"HTML"));
            item.params.Set(L"disable_notification", json::Value::MkBool(silent || i + 1 < chunks.size()));
            {
                auto lp = json::Value::MkObj();
                lp.Set(L"is_disabled", json::Value::MkBool(true));
                item.params.Set(L"link_preview_options", std::move(lp));
            }
            if (i == 0 && replyToMessageId > 0)
            {
                auto rp = json::Value::MkObj();
                rp.Set(L"message_id", json::Value::MkNum(static_cast<double>(replyToMessageId)));
                rp.Set(L"allow_sending_without_reply", json::Value::MkBool(true));
                item.params.Set(L"reply_parameters", std::move(rp));
            }
            if (i + 1 == chunks.size())
            {
                if (replyMarkup.type == json::Value::Type::Obj)
                {
                    item.params.Set(L"reply_markup", replyMarkup);
                }
                item.onDone = onDone;
            }
            // Telegram refuses HTML it cannot parse (a 400): fall back to the escaped plain text, so a
            // reply is never lost to a formatting edge case.
            const std::wstring plainText = chunks[i];
            const int64_t thread = threadId;
            auto self = weak_from_this();
            auto inner = std::move(item.onDone);
            item.onDone = [self, plainText, thread, inner](const TgResult& r) {
                if (!r.ok && r.httpStatus == 400 && r.description.find(L"can't parse entities") != std::wstring::npos)
                {
                    if (auto me = self.lock())
                    {
                        me->_log(L"[msg] Telegram refused the HTML of a message; sending it as plain text");
                        OutItem plain;
                        plain.prio = SendPriority::Final;
                        plain.method = L"sendMessage";
                        plain.params = json::Value::MkObj();
                        plain.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(me->_ownerId)));
                        if (thread > 0)
                        {
                            plain.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
                        }
                        // Strip the tags this header emits; entities stay escaped, which plain mode shows as
                        // typed ("&lt;"): a readable degradation.
                        std::wstring stripped;
                        bool inTag = false;
                        for (const wchar_t c : plainText)
                        {
                            if (c == L'<')
                            {
                                inTag = true;
                            }
                            else if (c == L'>' && inTag)
                            {
                                inTag = false;
                            }
                            else if (!inTag)
                            {
                                stripped += c;
                            }
                        }
                        plain.params.Set(L"text", json::Value::MkStr(stripped));
                        plain.params.Set(L"disable_notification", json::Value::MkBool(true));
                        plain.onDone = inner;
                        me->_enqueue(std::move(plain));
                    }
                    return;
                }
                if (inner)
                {
                    inner(r);
                }
            };
            _enqueue(std::move(item));
        }
        if (movesCard && threadId > 0)
        {
            _noteTopicPosted(threadId);
        }
    }

    void MessagingService::_noteTopicPosted(int64_t threadId)
    {
        if (threadId <= 0)
        {
            return;
        }
        std::lock_guard lk{ _mtx };
        for (auto& kv : _state.topics)
        {
            if (kv.second.threadId == threadId)
            {
                kv.second.lastPostedMs = WallMs();
                _stateDirty = true;
                return;
            }
        }
    }

    void MessagingService::_sendAsUser(int64_t threadId, const std::wstring& text, std::function<void(const TgResult&)> onDone)
    {
        std::wstring connection;
        {
            std::lock_guard lk{ _mtx };
            connection = _state.businessConnectionId;
        }
        const int64_t botId = _botId.load();
        if (connection.empty() || botId == 0)
        {
            TgResult none;
            none.ok = false;
            none.httpStatus = 0;
            none.description = connection.empty() ? L"no business connection" : L"the bot's id is not known yet";
            if (onDone)
            {
                onDone(none);
            }
            return;
        }
        // Plain text (the user's own words carry no markup), split at Telegram's limit on line ends.
        std::vector<std::wstring> parts;
        {
            std::wstring rest = text;
            constexpr size_t kPart = 4000;
            while (rest.size() > kPart)
            {
                size_t cut = rest.rfind(L'\n', kPart);
                if (cut == std::wstring::npos || cut < kPart / 2)
                {
                    cut = kPart;
                }
                parts.push_back(rest.substr(0, cut));
                rest.erase(0, cut == kPart ? cut : cut + 1);
            }
            parts.push_back(rest);
        }
        {
            // The echo guard's memory (_takeSecretaryEcho): if Telegram hands the message back to the bot as a
            // message from the owner in this topic, it must never be queued as a new prompt.
            std::lock_guard lk{ _mtx };
            const int64_t now = WallMs();
            std::erase_if(_secretaryEchoes, [now](const SecretaryEcho& e) { return e.expiresMs < now; });
            for (const auto& part : parts)
            {
                _secretaryEchoes.push_back(SecretaryEcho{ threadId, EchoHash(part), now + kSecretaryEchoMs });
            }
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            OutItem item;
            item.prio = SendPriority::New;
            item.method = L"sendMessage";
            item.params = json::Value::MkObj();
            item.params.Set(L"business_connection_id", json::Value::MkStr(connection));
            // Seen from the business account (the user), its chat with this bot is "the private chat with user
            // <bot id>": that is the chat_id a send on the user's behalf names. Whether Telegram allows the
            // bot's own chat as a target is exactly what the probe establishes (§21.5).
            item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(botId)));
            if (threadId > 0)
            {
                item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
            }
            item.params.Set(L"text", json::Value::MkStr(parts[i]));
            item.params.Set(L"disable_notification", json::Value::MkBool(true));
            if (i + 1 == parts.size())
            {
                item.onDone = onDone;
            }
            _enqueue(std::move(item));
        }
        _noteTopicPosted(threadId);
    }

    bool MessagingService::_takeSecretaryEcho(int64_t threadId, const std::wstring& text)
    {
        const uint32_t hash = EchoHash(text);
        std::lock_guard lk{ _mtx };
        const int64_t now = WallMs();
        for (auto it = _secretaryEchoes.begin(); it != _secretaryEchoes.end(); ++it)
        {
            if (it->threadId == threadId && it->hash == hash && it->expiresMs >= now)
            {
                _secretaryEchoes.erase(it); // one echo per send: the same words typed again later are a new message
                return true;
            }
        }
        return false;
    }

    void MessagingService::_editText(int64_t messageId, const std::wstring& html, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone)
    {
        if (messageId <= 0)
        {
            return;
        }
        OutItem item;
        item.prio = prio;
        item.key = key;
        item.method = L"editMessageText";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        // An edit cannot be split: cut at the limit (the work log rolls over before it, the card is short).
        std::wstring text = html;
        if (text.size() > kTelegramMessageMaxChars)
        {
            text = TgChunkHtml(text).front();
        }
        item.params.Set(L"text", json::Value::MkStr(text));
        item.params.Set(L"parse_mode", json::Value::MkStr(L"HTML"));
        {
            auto lp = json::Value::MkObj();
            lp.Set(L"is_disabled", json::Value::MkBool(true));
            item.params.Set(L"link_preview_options", std::move(lp));
        }
        if (replyMarkup.type == json::Value::Type::Obj)
        {
            item.params.Set(L"reply_markup", replyMarkup);
        }
        item.onDone = std::move(onDone);
        _enqueue(std::move(item));
    }

    void MessagingService::_react(int64_t messageId, std::wstring_view emoji)
    {
        if (messageId <= 0)
        {
            return;
        }
        OutItem item;
        item.prio = SendPriority::Backfill;
        item.key = L"react:" + std::to_wstring(messageId);
        item.method = L"setMessageReaction";
        item.paced = false;
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        auto arr = json::Value::MkArr();
        if (!emoji.empty())
        {
            auto re = json::Value::MkObj();
            re.Set(L"type", json::Value::MkStr(L"emoji"));
            re.Set(L"emoji", json::Value::MkStr(std::wstring{ emoji }));
            arr.Push(std::move(re));
        }
        item.params.Set(L"reaction", std::move(arr));
        _enqueue(std::move(item));
    }

    void MessagingService::_sendDocument(int64_t threadId, std::wstring_view fileName, std::string bytes, const std::wstring& caption, SendPriority prio)
    {
        OutItem item;
        item.prio = prio;
        item.document = true;
        item.fileName = std::wstring{ fileName };
        item.fileBytes = std::move(bytes);
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        if (threadId > 0)
        {
            item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
        }
        if (!caption.empty())
        {
            item.params.Set(L"caption", json::Value::MkStr(caption.size() > 1000 ? caption.substr(0, 1000) : caption));
        }
        item.params.Set(L"disable_notification", json::Value::MkBool(true));
        _enqueue(std::move(item));
    }

    std::wstring MessagingService::_cardMarkupFor(const std::wstring& sessionId, bool pinned, AutorunnerMode mode, json::Value& markupOut, bool reactivatable) const
    {
        // Card buttons use STABLE callback data ("c:<verb>:<session id>", 45 bytes of the 64 allowed):
        // each verb acts on the session's CURRENT state (a toggle reads it first), so a stale button
        // cannot do something old (M5 is about answers and queue items, which use tokens).
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        if (reactivatable)
        {
            // A parked tab or a session in Error: the one action the user wants first (§21.5).
            rows.push_back({ { L"\x25B6 Reactivate", L"c:reactivate:" + sessionId } });
        }
        rows.push_back({ { mode == AutorunnerMode::Full ? L"\x23F8 Pause runner" : L"\x25B6 Runner auto", L"c:runner:" + sessionId },
                         { L"\U0001F4CB Queue", L"c:queue:" + sessionId } });
        rows.push_back({ { pinned ? L"\U0001F4CC Unpin" : L"\U0001F4CC Pin", L"c:pin:" + sessionId },
                         { L"\U0001F4C4 Transcript", L"c:transcript:" + sessionId },
                         { L"\U0001F5A5 Screen", L"c:screen:" + sessionId } });
        markupOut = _buttons(rows);
        return {};
    }

    // ===== topics = tabs =====================================================================

    void MessagingService::_ensureSearchTopic()
    {
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
        }
        if (thread == 0)
        {
            auto p = json::Value::MkObj();
            p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            p.Set(L"name", json::Value::MkStr(L"\U0001F50E Search"));
            p.Set(L"icon_color", json::Value::MkNum(static_cast<double>(kTelegramTopicColors[0])));
            const TgResult r = _api->Call(L"createForumTopic", p, 20000);
            if (!r.ok)
            {
                _setStatus(L"Connected, but the Search topic could not be created: " + r.description);
                return;
            }
            thread = r.result.I64At(L"message_thread_id", 0);
            {
                std::lock_guard lk{ _mtx };
                _state.searchThreadId = thread;
                _stateDirty = true;
            }
            _log(L"[msg] topic created: Search (thread " + std::to_wstring(thread) + L")");
            _sendText(thread,
                      L"<b>\U0001F50E Search</b> \x2014 ask for a session in plain words (\x201Cthe session where I fixed the tab drag crash\x201D) and the Sessions page's search agent finds it. Tap a result's number to pick it, then Resume or Fork it into a tab.\n\n"
                      L"This is one conversation for the whole app: it carries on after a restart, and /clear (or \x2298 Clear under an answer) starts it over.\n\n"
                      L"Fleet commands: /idle (idle tabs), /board (counts), /newsession &lt;folder&gt;, /pauseall, /resumeall, /status, /help.\n\n"
                      L"Pin this topic at the top of the list (Telegram has no way for a bot to do it).",
                      true,
                      SendPriority::Final,
                      L"",
                      json::Value{},
                      nullptr);
            // The chat's "/" menu: one list for the whole chat (setMyCommands with the chat scope).
            auto cmds = json::Value::MkArr();
            const std::pair<const wchar_t*, const wchar_t*> list[] = {
                { L"queue", L"the queued prompts (session topic)" },
                { L"sendnow", L"send the next queued prompt now" },
                { L"runner", L"runner auto|paused" },
                { L"pin", L"never idle: keep this topic" },
                { L"unpin", L"let the session idle again" },
                { L"wake", L"Idle -> Waiting for you" },
                { L"transcript", L"the session as a .md file [minimal|compact|default|full|max]" },
                { L"summary", L"the session's summary" },
                { L"screen", L"the terminal's visible text" },
                { L"status", L"the session's card / the fleet" },
                { L"rename", L"rename the tab and the topic" },
                { L"restartsession", L"restart the session (asks while running)" },
                { L"deactivate", L"park the tab (asks while running)" },
                { L"activate", L"start a parked tab's claude" },
                { L"reactivate", L"resume a parked tab or a cut-off / failed session" },
                { L"forksession", L"fork into a new tab" },
                { L"newsession", L"a new session in this folder (or <folder> in Search)" },
                { L"interrupt", L"stop the running turn (asks first)" },
                { L"idle", L"Search: the idle tabs" },
                { L"board", L"Search: counts per state" },
                { L"pauseall", L"pause every runner" },
                { L"resumeall", L"resume every runner" },
                { L"help", L"what you can do here" },
            };
            for (const auto& [c, d] : list)
            {
                auto o = json::Value::MkObj();
                o.Set(L"command", json::Value::MkStr(c));
                o.Set(L"description", json::Value::MkStr(d));
                cmds.Push(std::move(o));
            }
            auto sp = json::Value::MkObj();
            sp.Set(L"commands", std::move(cmds));
            auto scope = json::Value::MkObj();
            scope.Set(L"type", json::Value::MkStr(L"chat"));
            scope.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            sp.Set(L"scope", std::move(scope));
            OutItem item;
            item.prio = SendPriority::Backfill;
            item.method = L"setMyCommands";
            item.params = std::move(sp);
            item.paced = false;
            _enqueue(std::move(item));
        }
        if (!_startNotePosted)
        {
            _startNotePosted = true;
            // The previous run's marker, if any, means it ended without a clean stop (§4.3).
            const std::wstring marker = _profile + L"\\messaging\\run.marker";
            std::wstring crashed;
            {
                const std::string prev = ReadFileBytes(marker + L".last");
                if (!prev.empty())
                {
                    try
                    {
                        const long long when = std::stoll(prev);
                        crashed = L"\n\x26A0 the previous run ended unexpectedly at <tg-time unix=\"" + std::to_wstring(when / 1000) + L"\" format=\"t\"></tg-time>";
                    }
                    catch (...)
                    {
                        crashed = L"\n\x26A0 the previous run ended unexpectedly";
                    }
                    ::DeleteFileW((marker + L".last").c_str());
                }
            }
            size_t topics = 0;
            {
                std::lock_guard lk{ _mtx };
                topics = _state.topics.size();
            }
            std::wstring version;
            if (_hooks.versionWord)
            {
                try
                {
                    version = _hooks.versionWord();
                }
                catch (...)
                {
                    LogSwallowedException(L"MessagingService (versionWord)");
                }
            }
            _appNote(L"Agentmaster started" + (version.empty() ? std::wstring{} : L" (" + TgEscapeHtml(version) + L")") + L" \x2014 " + std::to_wstring(topics) + L" topic(s) kept from the last run" + crashed, !crashed.empty());
        }
    }

    void MessagingService::_appNote(const std::wstring& html, bool sound)
    {
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
        }
        _sendText(thread, html, !sound, SendPriority::Final, L"", json::Value{}, nullptr);
    }

    void MessagingService::_syncTopics()
    {
        // The facts: every live session, lean (SnapshotLiveForCards carries title / state / tabToken /
        // kind / pinned — no queue).
        std::vector<TabFact> facts;
        std::unordered_map<std::wstring, SessionInfo> byId;
        const TabColorMode colorMode = LoadAppSettings().tabColorMode;
        for (auto& s : _registry->SnapshotLiveForCards())
        {
            TabFact f;
            f.sessionId = s.id;
            f.title = s.title;
            f.state = s.state;
            f.live = s.live;
            f.inTab = !s.tabToken.empty();
            f.claude = (s.kind == AgentKind::Claude);
            f.pinned = s.pinned;
            f.lastActivityMs = s.lastActivityUnixMs; // a new tab's creation instant: it is listed before its first message
            f.tabToken = s.tabToken; // per tab: a topic follows its tab to the next conversation (Rekey)
            f.continuedFromId = s.continuedFromId; // ...also to a /handover-here successor, which takes the tab on a new connection
            facts.push_back(std::move(f));
            byId.emplace(s.id, std::move(s));
        }
        {
            // The tab each topic's session is in NOW (a restart gives every tab a new token, and the topic's tab is
            // what a later Rekey matches): kept in step here, before the plan reads the records.
            std::lock_guard lk{ _mtx };
            for (auto& [sid, t] : _state.topics)
            {
                if (const auto it = byId.find(sid); it != byId.end() && it->second.live && !it->second.tabToken.empty() && it->second.tabToken != t.tabToken)
                {
                    t.tabToken = it->second.tabToken;
                    _stateDirty = true;
                }
            }
        }
        // The start-up hold's evidence: a session seen live in this run has re-homed, so its topic follows the
        // plan from then on (a tab the user closes 30 s after a start loses its topic at once). The set is
        // only consulted inside the hold and is dropped when the hold ends.
        const int64_t nowMs = WallMs();
        const int64_t sinceStartMs = nowMs - _startedMs;
        size_t restoring = 0;
        if (_hooks.windowsRestoring && sinceStartMs < kStartupCloseHoldCapMs)
        {
            try
            {
                restoring = _hooks.windowsRestoring();
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_syncTopics (windowsRestoring)");
            }
        }
        const bool inStartupHold = sinceStartMs < kStartupCloseHoldMinMs || restoring > 0;
        if (inStartupHold)
        {
            _startupHoldOpen = true;
            for (const auto& f : facts)
            {
                if (f.live)
                {
                    _seenLive.insert(f.sessionId);
                }
            }
        }
        else
        {
            if (_startupHoldOpen)
            {
                // Once per start, the moment the last restoring window is done (or the cap ran out).
                _startupHoldOpen = false;
                _log(L"[msg] start-up hold ended " + std::to_wstring(sinceStartMs / 1000) + L" s after the start (" + (sinceStartMs < kStartupCloseHoldCapMs ? L"every window restored" : L"the cap ran out") + L")");
            }
            if (!_seenLive.empty())
            {
                _seenLive.clear();
            }
        }
        std::vector<TopicSyncOp> ops;
        {
            std::lock_guard lk{ _mtx };
            ops = PlanTopicSync(facts, _state.topics, nowMs, _idleGraceMs.load());
        }
        size_t apiOps = 0;
        size_t heldCloses = 0;
        for (const auto& op : ops)
        {
            if (_stop.load())
            {
                return;
            }
            switch (op.kind)
            {
            case TopicSyncOp::Kind::Delete:
                if (op.reason == L"closed" && inStartupHold && _seenLive.find(op.sessionId) == _seenLive.end())
                {
                    // The start-up hold: "closed" here most likely means "its window has not re-homed its tabs
                    // yet". The plan re-emits the delete every tick; once the session comes back live the plan
                    // stops asking, and a session still closed when the hold ends is deleted then.
                    ++heldCloses;
                    break;
                }
                if (apiOps++ < kTopicOpsPerTick)
                {
                    _deleteTopic(op.sessionId, op.reason);
                }
                break;
            case TopicSyncOp::Kind::Create:
                if (apiOps++ < kTopicOpsPerTick)
                {
                    std::wstring color;
                    if (const auto it = byId.find(op.sessionId); it != byId.end())
                    {
                        try
                        {
                            color = ResolveSessionColorHex(colorMode, it->second);
                        }
                        catch (...)
                        {
                            LogSwallowedException(L"MessagingService::_syncTopics (color)");
                        }
                    }
                    _createTopic(op.sessionId, op.title, color);
                }
                break;
            case TopicSyncOp::Kind::Rename:
                if (apiOps++ < kTopicOpsPerTick)
                {
                    _renameTopic(op.sessionId, op.title);
                }
                break;
            case TopicSyncOp::Kind::Rekey:
                // The tab's conversation changed (/clear, /resume, a handover in place): the topic follows the tab.
                // A rename may be one API call; past the tick's budget the next tick plans it again.
                if (apiOps++ < kTopicOpsPerTick)
                {
                    _rekeyTopic(op.fromSessionId, op.sessionId, op.title);
                }
                break;
            case TopicSyncOp::Kind::IdleCard:
            {
                int64_t leavesAt = 0;
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _state.topics.find(op.sessionId); it != _state.topics.end())
                    {
                        it->second.idleSinceMs = nowMs;
                        _stateDirty = true;
                    }
                }
                if (const auto it = byId.find(op.sessionId); it != byId.end())
                {
                    leavesAt = IdleTopicLeavesAtMs(nowMs, it->second.lastActivityUnixMs, _idleGraceMs.load());
                }
                _log(L"[msg] topic idle " + ShortId(op.sessionId) + L" (leaves the list in " + std::to_wstring(std::max<int64_t>(0, leavesAt - nowMs) / 60000) + L" min unless pinned or woken)");
            }
                _refreshCard(op.sessionId, true);
                break;
            case TopicSyncOp::Kind::UnidleCard:
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(op.sessionId); it != _state.topics.end())
                {
                    it->second.idleSinceMs = 0;
                    _stateDirty = true;
                }
            }
                _refreshCard(op.sessionId, true);
                break;
            }
        }
        if (heldCloses > 0 && !_startupHoldLogged)
        {
            // Once per start: the plan re-asks every tick, and the count only shrinks as the windows re-home.
            _startupHoldLogged = true;
            _log(L"[msg] start-up hold: " + std::to_wstring(heldCloses) + L" topic(s) kept until their sessions re-home (" + std::to_wstring(restoring) + L" window(s) still restoring; at least " + std::to_wstring(kStartupCloseHoldMinMs / 1000) + L" s after the start)");
        }
    }

    void MessagingService::_createTopic(const std::wstring& sessionId, const std::wstring& title, const std::wstring& colorHex)
    {
        std::wstring name = title.empty() ? ShortId(sessionId) : title;
        if (name.size() > 120)
        {
            name = name.substr(0, 119) + L"\x2026";
        }
        auto p = json::Value::MkObj();
        p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        p.Set(L"name", json::Value::MkStr(name));
        p.Set(L"icon_color", json::Value::MkNum(static_cast<double>(TgTopicIconColor(colorHex))));
        TgResult r = _api->Call(L"createForumTopic", p, 20000);
        if (!r.ok && r.httpStatus == 400 && r.description.find(L"icon") != std::wstring::npos)
        {
            // A color Telegram did not take: create without one.
            auto p2 = json::Value::MkObj();
            p2.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            p2.Set(L"name", json::Value::MkStr(name));
            r = _api->Call(L"createForumTopic", p2, 20000);
        }
        if (!r.ok)
        {
            if (r.httpStatus == 429)
            {
                std::this_thread::sleep_for(std::chrono::seconds(std::max(1, r.retryAfterSec)));
            }
            _log(L"[msg] topic create failed for " + ShortId(sessionId) + L": " + r.description);
            return;
        }
        const int64_t thread = r.result.I64At(L"message_thread_id", 0);
        if (thread == 0)
        {
            return;
        }
        // The topic shows the session from now on (the backfill of §6.7 is a later phase; /transcript sends the
        // whole session as a file meanwhile): _seedFromTranscript decides where the mirror starts and what the
        // first card's recap is. Nothing but the card is posted here — the thread is the conversation (§21.6).
        const auto s = _session(sessionId);
        TopicRecord t;
        t.sessionId = sessionId;
        t.threadId = thread;
        t.title = name;
        t.createdMs = WallMs();
        if (s)
        {
            t.tabToken = s->tabToken;
            _seedFromTranscript(t, *s);
        }
        {
            std::lock_guard lk{ _mtx };
            _state.topics[sessionId] = t;
            _mirrors.erase(sessionId);
            _cards.erase(sessionId);
            _stateDirty = true;
        }
        _log(L"[msg] topic created " + ShortId(sessionId) + L" \x201C" + name + L"\x201D (thread " + std::to_wstring(thread) + L", " +
             (t.transcriptPath.empty() ? std::wstring{ L"no transcript yet: mirrored from its first line" } : L"mirrored from byte " + std::to_wstring(t.cursor)) + L")");
        if (!s)
        {
            // The session left the registry between the plan and now: a bare card, the next sync deletes the topic.
            _postCard(sessionId, thread, L"\U0001F4CC " + TgEscapeHtml(name), json::Value{}, 0, false, 0);
            return;
        }
        // The card is the STICKY bottom message of the topic (§21.5): _refreshCard posts the first one (cardMessageId
        // is 0) with the recap rule, the buttons and the text hash, exactly as every later re-post.
        _refreshCard(sessionId, true);
    }

    void MessagingService::_postCard(const std::wstring& sessionId, int64_t threadId, const std::wstring& text, json::Value markup, int64_t deleteFirst, bool ping, uint32_t textHash)
    {
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                it->second.cardMessageId = -1; // in flight: _refreshCard waits for the answer below
                it->second.cardTextHash = textHash; // what the topic will show (a refusal re-posts it anyway)
                _stateDirty = true;
            }
        }
        if (deleteFirst > 0)
        {
            // Same priority as the send that follows and queued first, so the old card is gone before the new
            // one appears; a card already gone (the user deleted it) is a harmless 400.
            OutItem del;
            del.prio = SendPriority::CardEdit;
            del.key = L"card-del:" + sessionId;
            del.method = L"deleteMessage";
            del.params = json::Value::MkObj();
            del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(deleteFirst)));
            _enqueue(std::move(del));
        }
        auto self = weak_from_this();
        // `!ping` = silent: a card is a status message, and only a state the user must know about rings (§21.6).
        _sendText(threadId, text, !ping, SendPriority::CardEdit, L"card-post:" + sessionId, std::move(markup), [self, sessionId, threadId](const TgResult& r) {
            auto me = self.lock();
            if (!me)
            {
                return;
            }
            const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
            {
                // By THREAD, not by session: a Rekey (§21.6) may have moved the topic to another session's key while
                // this post flew, and the record must still learn its card (else it would wait for it forever).
                std::lock_guard lk{ me->_mtx };
                for (auto& [sid, t] : me->_state.topics)
                {
                    if (t.threadId == threadId)
                    {
                        t.cardMessageId = id;
                        t.cardPostedMs = WallMs(); // on a refusal too: the retry waits kCardRepostAfterFailureMs
                        me->_stateDirty = true;
                        break;
                    }
                }
            }
            if (!r.ok)
            {
                me->_log(L"[msg] card post refused for " + ShortId(sessionId) + L": " + r.description);
            }
        }, 0, false);
    }

    void MessagingService::_seedFromTranscript(TopicRecord& t, const SessionInfo& s) noexcept
    {
        try
        {
            t.transcriptPath = ResolveClaudeTranscriptPath(s.id);
            t.cursor = 0; // no transcript yet (a new session, a fork before its first message): its first line is news
            t.recapText.clear();
            t.recapAtMs = 0;
            t.lastMessageAtMs = 0;
            // The recap facts come from the transcript the session SHOWS: its own, or — for a fork with no message of
            // its own yet — the one it was forked from (its conversation so far IS the source's). Never cached: the
            // fork's first message creates its own transcript, and the mirror reads that from its first line.
            std::wstring recapPath = t.transcriptPath;
            if (recapPath.empty() && !s.forkParentId.empty())
            {
                recapPath = ResolveClaudeDisplayTranscript(s.id, s.forkParentId).path;
            }
            TranscriptTailScan recapScan;
            if (!recapPath.empty())
            {
                recapScan = ScanTranscriptTail(recapPath, kRecapSeedBytes);
                t.recapText = recapScan.recapText;
                t.recapAtMs = recapScan.recapAtMs;
                t.lastMessageAtMs = recapScan.lastMessageAtMs;
            }
            if (t.transcriptPath.empty())
            {
                return;
            }
            // Its own transcript exists: the topic shows the session from now on — from the END of the file for a
            // session at rest, and from the start of the turn's own prompt line for one that is mid-turn (a resting tab
            // that the user prompts at the PC gets its topic a tick after the prompt line was written; starting at the
            // end lost exactly that prompt — 2026-10-16). A turn longer than the window starts at the end.
            int64_t size = -1;
            if (recapPath == t.transcriptPath)
            {
                size = recapScan.size; // the scan above read this very file
            }
            else
            {
                ReadTranscriptTail(t.transcriptPath, 0x7FFFFFFFFFFFFFFFll, size); // the size only
            }
            t.cursor = size > 0 ? size : 0;
            const bool midTurn = s.state == SessionState::Running || s.state == SessionState::NeedsApproval;
            if (midTurn && recapPath == t.transcriptPath && recapScan.lastPromptLineAt >= 0 && recapScan.lastPromptLineAt < t.cursor)
            {
                t.cursor = recapScan.lastPromptLineAt;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the mirror starts at the file's end (or waits for the file), the card has no recap.
            LogSwallowedException(L"MessagingService::_seedFromTranscript");
            try
            {
                int64_t size = -1;
                if (!t.transcriptPath.empty())
                {
                    ReadTranscriptTail(t.transcriptPath, 0x7FFFFFFFFFFFFFFFll, size);
                }
                t.cursor = size > 0 ? size : 0;
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_seedFromTranscript (recovery)");
            }
        }
    }

    void MessagingService::_rekeyTopic(const std::wstring& fromSessionId, const std::wstring& toSessionId, const std::wstring& title)
    {
        const auto s = _session(toSessionId);
        if (!s)
        {
            return; // gone between the plan and now: the next sync decides again
        }
        TopicRecord t;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(fromSessionId);
            if (it == _state.topics.end() || _state.topics.count(toSessionId))
            {
                return; // the plan is stale (deleted meanwhile, or the heir got a topic of its own)
            }
            t = it->second;
        }
        // Everything that belonged to the OLD conversation goes: its mirror position and transcript, its work log, its
        // recap, its Error note. What belongs to the TOPIC stays: the thread, the card message, the sticky clocks.
        t.sessionId = toSessionId;
        t.tabToken = s->tabToken;
        t.workLogMessageId = 0;
        t.idleSinceMs = 0;
        t.errorAnnounced = false;
        t.cardTextHash = 0; // the card describes another session now: edited at once
        _seedFromTranscript(t, *s);
        const std::wstring oldTitle = t.title;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(fromSessionId);
            if (it == _state.topics.end() || _state.topics.count(toSessionId))
            {
                return;
            }
            t.cardMessageId = it->second.cardMessageId; // a card post may have answered while the seed read the file
            t.cardPostedMs = it->second.cardPostedMs;
            t.lastPostedMs = it->second.lastPostedMs;
            _state.topics.erase(it);
            _state.topics[toSessionId] = t;
            _mirrors.erase(fromSessionId);
            _mirrors.erase(toSessionId);
            _cards.erase(fromSessionId);
            _cards.erase(toSessionId);
            _stateDirty = true;
            // Queued edits keyed by the old session (its work log, its card edit) belong to a conversation the topic no
            // longer shows; a card post / delete in flight keeps going (its answer is matched by thread).
            for (auto o = _outbox.begin(); o != _outbox.end();)
            {
                const bool old = o->key == L"wl:" + fromSessionId || o->key == L"card:" + fromSessionId;
                o = old ? _outbox.erase(o) : std::next(o);
            }
        }
        _log(L"[msg] topic carried over " + ShortId(fromSessionId) + L" -> " + ShortId(toSessionId) + L" (the same tab, thread " + std::to_wstring(t.threadId) + L"; " +
             (t.transcriptPath.empty() ? std::wstring{ L"no transcript yet" } : L"mirrored from byte " + std::to_wstring(t.cursor)) + L")");
        LogNav(L"remote telegram topic-rekey " + ShortId(fromSessionId) + L" -> " + ShortId(toSessionId));
        // One silent line marks where the new conversation starts (the thread is the conversation, and this is its
        // boundary: a /clear, a /resume or a handover the user ran at the PC).
        _sendText(t.threadId, L"<i>\x2014 a new conversation in this tab (" + TgEscapeHtml(ShortId(toSessionId)) + L") \x2014</i>", true, SendPriority::New, L"", json::Value{}, nullptr);
        if (!title.empty() && title != oldTitle)
        {
            _renameTopic(toSessionId, title); // refreshes the card on success
        }
        _refreshCard(toSessionId, true);
    }

    void MessagingService::_deleteTopic(const std::wstring& sessionId, const std::wstring& reason)
    {
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(sessionId);
            if (it == _state.topics.end())
            {
                return;
            }
            thread = it->second.threadId;
        }
        auto p = json::Value::MkObj();
        p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        p.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
        const TgResult r = _api->Call(L"deleteForumTopic", p, 20000);
        if (!r.ok && !r.ThreadNotFound())
        {
            _log(L"[msg] topic delete failed for " + ShortId(sessionId) + L": " + r.description + L" (dropping the record anyway)");
        }
        {
            std::lock_guard lk{ _mtx };
            _state.topics.erase(sessionId);
            _mirrors.erase(sessionId);
            _cards.erase(sessionId);
            _stateDirty = true;
            // Queued edits for the dead topic are pointless.
            for (auto it = _outbox.begin(); it != _outbox.end();)
            {
                const bool mine = it->key.find(sessionId) != std::wstring::npos;
                it = mine ? _outbox.erase(it) : std::next(it);
            }
        }
        _log(L"[msg] topic deleted " + ShortId(sessionId) + L" (" + reason + L")");
    }

    void MessagingService::_renameTopic(const std::wstring& sessionId, const std::wstring& title)
    {
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(sessionId);
            if (it == _state.topics.end())
            {
                return;
            }
            thread = it->second.threadId;
        }
        std::wstring name = title;
        if (name.size() > 120)
        {
            name = name.substr(0, 119) + L"\x2026";
        }
        auto p = json::Value::MkObj();
        p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        p.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
        p.Set(L"name", json::Value::MkStr(name));
        const TgResult r = _api->Call(L"editForumTopic", p, 20000);
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                // Remember the title either way: a refused rename must not be retried every tick.
                it->second.title = title;
                _stateDirty = true;
            }
        }
        if (r.ok)
        {
            _log(L"[msg] topic renamed " + ShortId(sessionId) + L" \x2192 \x201C" + name + L"\x201D");
            _refreshCard(sessionId, true);
        }
        else if (!r.NotModified())
        {
            _log(L"[msg] topic rename failed for " + ShortId(sessionId) + L": " + r.description);
        }
    }

    void MessagingService::_refreshCard(const std::wstring& sessionId, bool force, bool ping)
    {
        const auto s = _session(sessionId);
        if (!s)
        {
            return;
        }
        TopicRecord topic;
        bool pingWanted = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(sessionId);
            if (it == _state.topics.end() || it->second.threadId == 0)
            {
                return;
            }
            topic = it->second;
            auto& card = _cards[sessionId];
            if (ping)
            {
                card.pingWanted = true; // kept until a post can carry it (below)
            }
            pingWanted = card.pingWanted;
        }
        if (topic.cardMessageId < 0)
        {
            return; // a post is in flight; its answer sets the id (a wanted ping goes with the next post)
        }
        const int64_t now = WallMs();
        if (topic.cardMessageId == 0 && now - topic.cardPostedMs < kCardRepostAfterFailureMs)
        {
            return; // Telegram refused the last post: not every tick
        }
        // The idle line's clock comes from the persisted stamps (IdleTopicLeavesAtMs — the same rule the plan deletes
        // by), so it only changes when the session is used: no periodic edit.
        const int64_t idleLeaves = IdleTopicLeavesAtMs(topic.idleSinceMs, s->lastActivityUnixMs, _idleGraceMs.load());
        CardExtras extras;
        extras.recapFresh = RecapIsFresh(topic.recapText, topic.recapAtMs, topic.lastMessageAtMs);
        extras.recapText = topic.recapText;
        extras.recapAtMs = topic.recapAtMs;
        const std::wstring text = BuildCardText(*s, now, idleLeaves, _scheduler ? _scheduler->GlobalPaused() : false, &extras);
        json::Value markup;
        _cardMarkupFor(sessionId, s->pinned, s->autorunner.mode, markup, SessionIsReactivatable(*s));
        // The text AND the buttons: a card whose text is unchanged but whose buttons differ is a change too.
        const uint32_t hash = CardTextHash(text + L"\x1F" + json::Dump(markup));
        // The STICKY CARD (§21.5): posts newer than the card push it down — once they have been quiet for
        // kCardStickyLagMs the old card is deleted and a fresh one sent at the bottom (two calls per burst, not
        // per message). A missing card (the first one, or a refused post a minute ago) is posted the same way, and
        // so is a PING (§21.6): the card re-sent WITH SOUND is the notification for a state the user must know about.
        const bool behind = topic.cardMessageId > 0 && topic.lastPostedMs > topic.cardPostedMs && now - topic.lastPostedMs >= kCardStickyLagMs;
        if (topic.cardMessageId == 0 || behind || pingWanted)
        {
            {
                std::lock_guard lk{ _mtx };
                auto& card = _cards[sessionId];
                card.pingWanted = false;
                card.lastEditMs = now;
                card.lastState = s->state;
            }
            if (pingWanted)
            {
                _log(L"[msg] " + ShortId(sessionId) + L" card re-posted with sound (" + StateWord(s->state) + L")");
            }
            _postCard(sessionId, topic.threadId, text, std::move(markup), topic.cardMessageId > 0 ? topic.cardMessageId : 0, pingWanted, hash);
            return;
        }
        if (hash == topic.cardTextHash)
        {
            return; // the topic shows this very card already (also after a restart: the hash is persisted)
        }
        bool send = false;
        {
            std::lock_guard lk{ _mtx };
            auto& card = _cards[sessionId];
            const bool stateChanged = card.lastState != s->state;
            if (force || stateChanged || now - card.lastEditMs >= kCardEditGapMs)
            {
                card.lastEditMs = now;
                card.lastState = s->state;
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    it->second.cardTextHash = hash;
                    _stateDirty = true;
                }
                send = true;
            }
        }
        if (send)
        {
            // A card the user deleted on the phone cannot be edited: the answer clears the record's card, and the next
            // refresh posts a new one at the bottom (cardPostedMs 0 = no failure wait).
            auto self = weak_from_this();
            const int64_t thread = topic.threadId;
            const int64_t cardId = topic.cardMessageId;
            _editText(topic.cardMessageId, text, SendPriority::CardEdit, L"card:" + sessionId, markup, [self, thread, cardId](const TgResult& r) {
                if (r.ok || r.NotModified() || r.httpStatus != 400 || r.description.find(L"message to edit not found") == std::wstring::npos)
                {
                    return;
                }
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                {
                    std::lock_guard lk{ me->_mtx };
                    for (auto& [sid, t] : me->_state.topics)
                    {
                        if (t.threadId == thread && t.cardMessageId == cardId)
                        {
                            t.cardMessageId = 0;
                            t.cardPostedMs = 0;
                            t.cardTextHash = 0;
                            me->_stateDirty = true;
                            me->_tickRequested = true;
                            break;
                        }
                    }
                }
                me->_log(L"[msg] a card was deleted in Telegram: a new one is posted (thread " + std::to_wstring(thread) + L")");
                me->_tickCv.notify_one();
            });
        }
    }

    void MessagingService::_postRemark(const std::wstring& sessionId, TopicRecord& topic, Mirror& m)
    {
        if (m.pendingText.empty())
        {
            return;
        }
        // The thread is the conversation (§21.5): what the assistant said before it reached for a tool is a
        // message of its own, silent. The work log so far is closed with its final content first, so the remark
        // lands below it, and the tools that follow start a new work-log message below the remark.
        _flushWorkLog(sessionId, topic, m, true);
        _sendText(topic.threadId, TgMarkdownToHtml(m.pendingText), true, SendPriority::New, L"", json::Value{}, nullptr);
        m.pendingText.clear();
        m.workLog.clear();
        m.workLogMessageId = 0;
        m.workLogDirty = false;
        ++m.workLogGen; // a create still in flight for the closed segment must not become the new segment's message
        topic.workLogMessageId = 0;
        m.turnOpen = true;
    }

    // ===== the mirror ========================================================================

    void MessagingService::_appendWorkLog(Mirror& m, const std::wstring& line)
    {
        if (!m.workLog.empty())
        {
            m.workLog += L'\n';
        }
        m.workLog += line;
        m.workLogDirty = true;
        m.turnOpen = true;
    }

    void MessagingService::_flushWorkLog(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, bool force)
    {
        if (!m.workLogDirty || m.workLog.empty())
        {
            return;
        }
        const int64_t now = WallMs();
        // The gap grows with the number of turns open across the fleet (the tick loop counts them): one busy
        // session edits every 15 s, twenty busy sessions every 40 s each — together never more than ~0.5
        // edit/s, half of the chat's ~1 call/s budget (the other half is for messages and cards).
        const int64_t gapMs = std::max<int64_t>(kWorkLogEditGapMs, static_cast<int64_t>(_openTurns.load()) * 2000);
        if (!force && m.workLogMessageId != 0 && now - m.workLogLastEditMs < gapMs)
        {
            return;
        }
        // Past the roll-over size the message is closed and the next line starts a new one.
        std::wstring html = L"<blockquote expandable>" + TgEscapeHtml(m.workLog) + L"</blockquote>";
        if (m.workLog.size() > kWorkLogRolloverChars && m.workLogMessageId != 0)
        {
            _editText(m.workLogMessageId, html, SendPriority::WorkLogEdit, L"wl:" + sessionId, json::Value{});
            m.workLog.clear();
            m.workLogMessageId = 0;
            topic.workLogMessageId = 0;
            m.workLogDirty = false;
            m.workLogLastEditMs = now;
            return;
        }
        m.workLogDirty = false;
        m.workLogLastEditMs = now;
        if (m.workLogMessageId == 0)
        {
            if (topic.workLogMessageId < 0)
            {
                return; // a create is in flight; the next tick edits it
            }
            topic.workLogMessageId = -1; // in flight
            {
                // The shared record says "in flight" too, from now on: the tick's write-back then knows that a record
                // no longer at -1 holds the create's ANSWER (a message id, or 0 for a refusal) and keeps it.
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    it->second.workLogMessageId = -1;
                }
            }
            auto self = weak_from_this();
            const int64_t thread = topic.threadId;
            const uint32_t gen = m.workLogGen;
            _sendText(thread, html, true, SendPriority::New, L"", json::Value{}, [self, sessionId, gen](const TgResult& r) {
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                std::lock_guard lk{ me->_mtx };
                const auto mit = me->_mirrors.find(sessionId);
                if (mit == me->_mirrors.end())
                {
                    return; // the topic was deleted or carried over to another session meanwhile
                }
                auto& mm = mit->second;
                if (mm.workLogGen != gen)
                {
                    return; // the segment was closed (a remark, a new turn) while this create flew: its message stays as it is
                }
                if (const auto it = me->_state.topics.find(sessionId); it != me->_state.topics.end())
                {
                    it->second.workLogMessageId = id;
                }
                mm.workLogMessageId = id;
                if (id != 0)
                {
                    mm.workLogDirty = true; // what arrived while the create was in flight goes out as an edit
                    me->_tickRequested = true;
                }
            }, 0);
        }
        else
        {
            _editText(m.workLogMessageId, html, SendPriority::WorkLogEdit, L"wl:" + sessionId, json::Value{});
        }
    }

    void MessagingService::_postFinal(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::wstring& html, const std::wstring& why, bool sound)
    {
        _flushWorkLog(sessionId, topic, m, true);
        // The ONE message of a turn with sound (D5 / M12) — unless the caller rings something else for it (§21.6).
        _sendText(topic.threadId, html, !sound, SendPriority::Final, L"", json::Value{}, nullptr);
        _endTurnQuietly(sessionId, topic, m);
        _log(L"[msg] " + ShortId(sessionId) + L" final message (" + why + (sound ? L"" : L", silent") + L")");
    }

    void MessagingService::_endTurnQuietly(const std::wstring& sessionId, TopicRecord& topic, Mirror& m)
    {
        _flushWorkLog(sessionId, topic, m, true); // the work log's last lines (a no-op when nothing is pending)
        m.pendingText.clear();
        m.turnOpen = false;
        m.workLog.clear();
        m.workLogMessageId = 0;
        m.workLogDirty = false;
        ++m.workLogGen; // a create still in flight belongs to the closed turn
        topic.workLogMessageId = 0;
    }

    void MessagingService::_followTranscript(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s)
    {
        const int64_t now = WallMs();
        if (topic.transcriptPath.empty())
        {
            // A new session or fork has its topic before its transcript exists (§21.6): look often while it is busy (its
            // first prompt is being written), seldom while it rests.
            const bool busy = s.state != SessionState::Idle;
            if (now - m.lastResolveMs < (busy ? kResolveRetryBusyMs : kResolveRetryMs))
            {
                return;
            }
            m.lastResolveMs = now;
            topic.transcriptPath = ResolveClaudeTranscriptPath(sessionId);
            if (topic.transcriptPath.empty())
            {
                return; // a never-prompted session has no transcript yet
            }
        }
        int64_t size = -1;
        std::string bytes = ReadTranscriptTail(topic.transcriptPath, topic.cursor, size);
        if (size < 0)
        {
            // Gone or unreadable for a moment: forget the path and resolve again later, but KEEP the cursor — reset to 0
            // it would replay the whole conversation into the topic when the same file is found again (a file that
            // came back smaller is mirrored from its new end, below).
            topic.transcriptPath.clear();
            return;
        }
        if (size < topic.cursor)
        {
            topic.cursor = size; // the file shrank (replaced): mirror from its new end
            return;
        }
        if (bytes.empty())
        {
            return;
        }
        // Complete lines only; a torn last line is read again next tick.
        const size_t lastNl = bytes.rfind('\n');
        if (lastNl == std::string::npos)
        {
            return;
        }
        bytes.resize(lastNl + 1);
        topic.cursor += static_cast<int64_t>(bytes.size());
        std::vector<TopicItem> items;
        try
        {
            const auto lines = ParseTranscriptJsonl(std::move(bytes));
            for (const auto& line : lines)
            {
                auto its = ClassifyTranscriptLine(line);
                for (auto& it : its)
                {
                    items.push_back(std::move(it));
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_followTranscript (parse)");
            return;
        }
        if (!items.empty())
        {
            _applyItems(sessionId, topic, m, items, s);
        }
    }

    void MessagingService::_applyItems(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::vector<TopicItem>& items, const SessionInfo& s)
    {
        // The recap facts first (§21.6): Claude Code's away_summary, and the newest conversation message that makes an
        // older one stale. The card reads them at its next refresh; a Recap item is nothing more in the thread.
        FoldRecapFacts(items, topic.recapText, topic.recapAtMs, topic.lastMessageAtMs);
        std::optional<SessionInfo> fresher; // a newer snapshot, fetched at most once, for a prompt the queue copy lacks
        for (const auto& it : items)
        {
            switch (it.kind)
            {
            case TopicItemKind::Prompt:
            {
                // A new turn: whatever the previous one left is closed first.
                if (!it.midTurn)
                {
                    if (!m.pendingText.empty())
                    {
                        _postFinal(sessionId, topic, m, TgMarkdownToHtml(m.pendingText), L"a new prompt closed the turn");
                    }
                    else
                    {
                        _flushWorkLog(sessionId, topic, m, true);
                        m.workLog.clear();
                        m.workLogMessageId = 0;
                        topic.workLogMessageId = 0;
                        m.workLogDirty = false;
                        ++m.workLogGen;
                    }
                    m.toolNames.clear();
                    m.turnOpen = true;
                    m.awaitingAnswer = false; // a new turn: its own question / permission prompt rings again
                    m.approvalPinged = false;
                }
                // Its source: a prompt sent from this topic is the user's own message (not mirrored); one
                // the runner sent reads ⚙; one typed at the PC reads ⌨.
                const std::wstring key = NormalizePromptTextForCompare(it.text);
                bool fromTopic = false;
                bool fromQueue = false;
                const auto matchQueue = [&](const SessionInfo& from) {
                    size_t looked = 0;
                    for (auto q = from.queue.rbegin(); q != from.queue.rend() && looked < 60; ++q, ++looked)
                    {
                        if (q->status != PromptStatus::Sent && q->status != PromptStatus::Pending)
                        {
                            continue;
                        }
                        if (NormalizePromptTextForCompare(q->text) == key)
                        {
                            fromTopic = !q->remoteSource.empty();
                            fromQueue = q->remoteSource.empty() && q->origin == PromptOrigin::Autorun;
                            return true;
                        }
                    }
                    return false;
                };
                if (!matchQueue(s))
                {
                    // The tick's snapshot predates this line: the registry may have recorded the prompt since (a typed
                    // row, or a remote prompt the snapshot missed). One fresh read decides — a phone message must never
                    // come back as a ⌨ line.
                    if (!fresher)
                    {
                        fresher = _session(sessionId);
                    }
                    if (fresher)
                    {
                        matchQueue(*fresher);
                    }
                }
                if (fromTopic)
                {
                    break;
                }
                std::wstring html = std::wstring{ fromQueue ? L"\x2699 " : L"\x2328 " } + (it.midTurn ? L"(mid-turn) " : L"") + L"<b>" + TgEscapeHtml(TgFirstLine(it.text, 200)) + L"</b>";
                if (it.text.find(L'\n') != std::wstring::npos || it.text.size() > 200)
                {
                    html += L"\n<blockquote expandable>" + TgEscapeHtml(it.text.size() > 3000 ? it.text.substr(0, 3000) + L"\x2026" : it.text) + L"</blockquote>";
                }
                int businessMode = 0;
                {
                    std::lock_guard lk{ _mtx };
                    businessMode = _state.businessMode;
                }
                if (businessMode == 1)
                {
                    // SECRETARY MODE (§21.5): the prompt is the user's own bubble in the topic, sent on the user's
                    // behalf through the Telegram Business connection; a refused send falls back to the bot's line
                    // above, and three refusals in a row turn the mode off (the user may have revoked the right).
                    const int64_t thread = topic.threadId;
                    const std::wstring fallback = html;
                    const std::wstring plain = it.text;
                    auto self = weak_from_this();
                    _sendAsUser(thread, plain, [self, thread, fallback](const TgResult& r) {
                        auto me = self.lock();
                        if (!me)
                        {
                            return;
                        }
                        if (r.ok)
                        {
                            me->_secretaryFailures.store(0);
                            return;
                        }
                        const int failures = me->_secretaryFailures.fetch_add(1) + 1;
                        me->_log(L"[msg] secretary: a send as the user was refused (" + r.description + L"); the prompt goes out as the bot's line" + (failures >= 3 ? L"; secretary mode is off until the bot is connected again" : L""));
                        if (failures >= 3)
                        {
                            std::lock_guard lk{ me->_mtx };
                            me->_state.businessMode = -1;
                            me->_state.businessNote = r.description;
                            me->_stateDirty = true;
                        }
                        me->_sendText(thread, fallback, true, SendPriority::New, L"", json::Value{}, nullptr);
                    });
                }
                else
                {
                    _sendText(topic.threadId, html, true, SendPriority::New, L"", json::Value{}, nullptr);
                }
                break;
            }
            case TopicItemKind::Text:
                if (it.turnEnds)
                {
                    std::wstring text = m.pendingText;
                    if (!text.empty())
                    {
                        text += L"\n\n";
                    }
                    text += it.text;
                    _postFinal(sessionId, topic, m, TgMarkdownToHtml(text), L"end of turn");
                }
                else
                {
                    if (!m.pendingText.empty())
                    {
                        m.pendingText += L"\n\n";
                    }
                    m.pendingText += it.text;
                    m.turnOpen = true;
                }
                break;
            case TopicItemKind::ToolCall:
                // What the assistant said before the tool call — "why the bash is invoked" — is a message of
                // its own (§21.5), not a line in the work log.
                _postRemark(sessionId, topic, m);
                m.toolNames[it.toolUseId] = it.toolName;
                _appendWorkLog(m, L"\x2022 " + it.summary);
                break;
            case TopicItemKind::ToolResult:
                if (it.isError)
                {
                    std::wstring name;
                    if (const auto n = m.toolNames.find(it.toolUseId); n != m.toolNames.end())
                    {
                        name = n->second;
                    }
                    _appendWorkLog(m, L"\x2717 " + (name.empty() ? std::wstring{} : name + L": ") + it.text);
                }
                break;
            case TopicItemKind::Question:
            {
                _postRemark(sessionId, topic, m);
                std::wstring html = L"\x2753 <b>" + TgEscapeHtml(it.text) + L"</b>";
                if (!it.options.empty())
                {
                    html += L"\n";
                    for (size_t i = 0; i < it.options.size(); ++i)
                    {
                        html += L"\n" + std::to_wstring(i + 1) + L". " + TgEscapeHtml(it.options[i]);
                    }
                    html += L"\n\n<i>Answer it at the PC (answering from here comes in a later build).</i>";
                }
                // ONE sound per "needs you": silent when the NeedsApproval edge already rang the card (§21.6).
                const bool sound = !m.approvalPinged;
                m.awaitingAnswer = true;
                _postFinal(sessionId, topic, m, html, L"a question", sound);
                break;
            }
            case TopicItemKind::Plan:
            {
                std::wstring html = L"\U0001F4CB <b>Plan ready for approval</b> \x2014 approve or reject it at the PC.";
                if (!it.text.empty())
                {
                    html += L"\n" + TgMarkdownToHtml(it.text.size() > 3500 ? it.text.substr(0, 3500) + L"\x2026" : it.text);
                }
                const bool sound = !m.approvalPinged;
                m.awaitingAnswer = true;
                _postFinal(sessionId, topic, m, html, L"a plan", sound);
                break;
            }
            case TopicItemKind::Compaction:
                _appendWorkLog(m, L"\x2014 " + it.text + L" \x2014");
                break;
            case TopicItemKind::Interrupt:
                // The user stopped the turn (Esc at the PC, or /interrupt here): what the assistant had said so far,
                // then the mark — silent, the user did it. A stray marker with no turn shown posts nothing.
                if (m.turnOpen || !m.pendingText.empty())
                {
                    _postFinal(sessionId, topic, m, (m.pendingText.empty() ? std::wstring{} : TgMarkdownToHtml(m.pendingText) + L"\n\n") + L"\x25A0 <i>interrupted</i>", L"interrupted", false);
                }
                break;
            case TopicItemKind::ApiError:
            {
                // The turn died: an assistant line of the conversation (Claude Code writes it). It rings unless the
                // Error edge already rang the card; either way the edge that follows rings nothing more.
                const bool sound = !topic.errorAnnounced;
                topic.errorAnnounced = true;
                _postFinal(sessionId, topic, m, L"\x26A0 <b>API error</b>\n" + TgEscapeHtml(it.text.size() > 2000 ? it.text.substr(0, 2000) : it.text), L"an API error", sound);
                break;
            }
            case TopicItemKind::Command:
                _sendText(topic.threadId, L"<code>" + TgEscapeHtml(it.text.size() > 300 ? it.text.substr(0, 300) : it.text) + L"</code>", true, SendPriority::New, L"", json::Value{}, nullptr);
                break;
            default:
                break;
            }
        }
    }

    void MessagingService::_onStateEdge(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s)
    {
        const int64_t now = WallMs();
        if (m.lastStateMs == 0)
        {
            // The first look at this session in this run: no edge to report.
            m.lastState = s.state;
            m.lastStateMs = now;
            return;
        }
        if (s.state == m.lastState)
        {
            return;
        }
        const SessionState from = m.lastState;
        m.lastState = s.state;
        m.lastStateMs = now;
        _log(L"[msg] " + ShortId(sessionId) + L" " + StateWord(from) + L" -> " + StateWord(s.state));
        if (s.state == SessionState::Running)
        {
            topic.errorAnnounced = false; // a new turn: a later Error is news again
            m.awaitingAnswer = false; // answered / approved: a later question or permission prompt rings again
            m.approvalPinged = false;
        }
        // What the transcript did not say: a turn that ended without a terminal line (a recon synth), an exit, an
        // error the tail did not carry, a permission prompt (no AskUserQuestion line). The thread is the conversation
        // (§21.6): a state is said by the CARD — re-sent at the bottom WITH SOUND (a ping) for the states the user
        // must know about — never by a bot message of its own.
        const bool atRest = s.state == SessionState::WaitingForInput || s.state == SessionState::Idle;
        bool ping = false;
        if (from == SessionState::Running && atRest && !m.pendingText.empty())
        {
            // The turn's last words that no terminal line marked as final: the turn's message with sound.
            _postFinal(sessionId, topic, m, TgMarkdownToHtml(m.pendingText), L"the state left Running");
        }
        else if (s.state == SessionState::Done && from != SessionState::Done)
        {
            // claude exited: what it said last closes the turn (silent), the card says it exited and rings.
            if (!m.pendingText.empty())
            {
                _postFinal(sessionId, topic, m, TgMarkdownToHtml(m.pendingText), L"exited", false);
            }
            else
            {
                _endTurnQuietly(sessionId, topic, m);
            }
            ping = true;
        }
        else if (s.state == SessionState::Error && from != SessionState::Error)
        {
            // An API error the mirror already posted set errorAnnounced; otherwise (a cut-off turn, a scanner-derived
            // error) the card rings — ONCE per episode: a session at rest in Error reads Idle at every app start until
            // the scanner re-asserts Error, and the topic was told the first time.
            if (!topic.errorAnnounced && (m.turnOpen || !m.pendingText.empty() || !s.errorMessage.empty()))
            {
                topic.errorAnnounced = true;
                if (!m.pendingText.empty())
                {
                    _postFinal(sessionId, topic, m, TgMarkdownToHtml(m.pendingText), L"error state", false);
                }
                else
                {
                    _endTurnQuietly(sessionId, topic, m);
                }
                ping = true;
            }
        }
        else if (s.state == SessionState::NeedsApproval && from == SessionState::Running && !m.awaitingAnswer)
        {
            // A permission prompt — or a question whose line the mirror has not read yet: the card rings now, and the
            // question posted after it goes out silent (approvalPinged).
            m.approvalPinged = true;
            ping = true;
        }
        _refreshCard(sessionId, true, ping);
    }

    void MessagingService::_syncReactions(const SessionInfo& s)
    {
        // A remote prompt's status as a reaction on the user's own Telegram message: ✍ queued (set when it
        // was queued), 👨‍💻 sent and picked up, 👎 failed.
        for (const auto& q : s.queue)
        {
            if (q.remoteSource.rfind(L"telegram:", 0) != 0)
            {
                continue;
            }
            PromptStatus shown = PromptStatus::Pending;
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _reacted.find(q.id); it != _reacted.end())
                {
                    shown = it->second;
                }
            }
            PromptStatus want = q.status;
            if (q.status == PromptStatus::Sent && !q.echoed)
            {
                want = PromptStatus::Pending; // sent but not yet picked up: still ✍
            }
            if (want == shown)
            {
                continue;
            }
            int64_t msgId = 0;
            try
            {
                msgId = std::stoll(q.remoteSource.substr(9));
            }
            catch (...)
            {
                continue;
            }
            const wchar_t* emoji = want == PromptStatus::Failed ? L"\U0001F44E" : (want == PromptStatus::Sent ? L"\U0001F468‍\U0001F4BB" : L"✍");
            _react(msgId, emoji);
            {
                std::lock_guard lk{ _mtx };
                _reacted[q.id] = want;
            }
            if (want == PromptStatus::Failed)
            {
                int64_t thread = 0;
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _state.topics.find(s.id); it != _state.topics.end())
                    {
                        thread = it->second.threadId;
                    }
                }
                _sendText(thread, L"\U0001F44E <b>not delivered</b> \x2014 the prompt never became a message (lost send or a refused delivery); the runner paused. Look at the terminal, then /sendnow or send it again.", true, SendPriority::Final, L"", json::Value{}, nullptr, msgId);
            }
        }
    }
}
