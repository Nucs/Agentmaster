// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TeamsService (see TeamsService.h): the bot-less Teams connector's threads, outbox, router, state and legend.

#include "TeamsService.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // the transcript reads (shared read, write and delete: claude appends while they run)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime> // localtime_s: the clock words the owner reads
#include <cwchar> // swprintf_s
#include <filesystem>
#include <set>

#include "ApiErrorRewind.h" // AssessForkContinuation: whether a fork from Teams asks "interrupted or uninterrupted?" (§8 🔀)
#include "ClaudeInvisibleChars.h" // NormalizePromptTextForCompare: a prompt from Teams known again in its transcript echo
#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException, ResolveClaudeTranscriptPath, ResolveClaudeDisplayTranscript, NewSessionId
#include "ClaudeStatus.h" // SharedClaudeStatusProbe, ReadClaudeTranscriptStatusFile: 📊 and /status
#include "ConversationImage.h" // RenderConversationImages: 📜 and /allscreen, the whole conversation as pictures
#include "FolderProbe.h" // SharedFolderProber: a new session's folders, checked with a bounded wait (§9.1, Telegram's §21.17.4)
#include "MessagingStatus.h" // BuildClaudeStatusReport: the status report's layout, Telegram's
#include "Scheduler.h" // RequestAdvance, kAnswersQuestionOk: a prompt from Teams is sent like a Telegram topic's
#include "ScreenImage.h" // RenderScreenSnapshotPng: 🖼️'s picture
#include "ScreenSnapshot.h" // ParseScreenSnapshot: the window's styled screen read
#include "TranscriptStore.h" // ReadSessionPresenceForPid: the status report's heartbeat; CollectMachineTempRoots: a new session's step leaves them out
#include "Persistence.h" // WriteStateDocument, LoadDirColorIndex, ResolveSessionColorHex
#include "SessionRegistry.h" // SnapshotLiveForCards: the board's sessions
#include "StateFileGuard.h" // ReadStateFileUtf8, MarkStateFileSafeToReplace / MarkStateFileKeepBeforeReplace

namespace Agentmaster::Teams
{
    namespace
    {
        /// <summary>The state is saved at most this often (a burst of changes is one write).</summary>
        constexpr int64_t kStateSaveGapMs = 2000;
        /// <summary>The previous copy (".prev") is refreshed at most this often.</summary>
        constexpr int64_t kStatePrevCopyGapMs = 60'000;
        /// <summary>The journal is pruned this often.</summary>
        constexpr int64_t kPruneGapMs = 3'600'000;
        /// <summary>A failed legend post is tried again after this long.</summary>
        constexpr int64_t kLegendRetryMs = 60'000;
        /// <summary>The fleet board is built at most this often (it reads the registry and the folder colors).</summary>
        constexpr int64_t kLegendBuildGapMs = 5000;
        /// <summary>A write that keeps failing with a retryable answer is given up after this many attempts (~2 minutes).</summary>
        constexpr int kMaxSendAttempts = 8;
        /// <summary>A content write no token can make now waits this long in its lane before it is tried again.</summary>
        constexpr int64_t kNoTokenWaitMs = 30'000;
        /// <summary>The threads' step runs at most this often (a registry snapshot and a file stat per thread).</summary>
        constexpr int64_t kThreadsStepGapMs = 2000;
        /// <summary>A failed root post (a new thread) is tried again after this long.</summary>
        constexpr int64_t kRootRetryMs = 60'000;
        /// <summary>A session whose transcript is not found yet is looked for again after this long.</summary>
        constexpr int64_t kResolveGapMs = 10'000;
        /// <summary>The live mirror reads at most this much of a transcript per pass (a longer line grows the read, below).</summary>
        constexpr int64_t kLiveReadBytes = 1024 * 1024;
        /// <summary>A history window's first size; a window with no complete line grows ×4 up to kWindowMaxBytes.</summary>
        constexpr int64_t kHistoryWindowBytes = 1024 * 1024;
        /// <summary>The largest window: one line longer than this (a pasted image can make a line megabytes long) is skipped.</summary>
        constexpr int64_t kWindowMaxBytes = 64LL * 1024 * 1024;
        /// <summary>The folder colors are read again at most this often.</summary>
        constexpr int64_t kDirColorsGapMs = 30'000;
        /// <summary>One poll reads at most this many delta pages; a longer first sync goes on at the next poll from the saved link.</summary>
        constexpr int kMaxDeltaPagesPerPoll = 20;
        /// <summary>A second Esc for one session within this long is refused: two Esc in a row open Claude Code's Rewind menu.</summary>
        constexpr int64_t kEscGapMs = 3000;
        /// <summary>
        /// The most conversations this connector draws at once (📜): a long transcript holds hundreds of MB while it is read and
        /// drawn (a 95 MB one peaked at ~315 MB of commit, MESSAGING.md §21.39). The Telegram bot keeps its own one.
        /// </summary>
        constexpr int kAllScreenMaxRenders = 1;

        /// <summary>
        /// A window action's ONE answer (Telegram's SessionActionAnswer): every window that hosts nothing drops its copy of the
        /// reply without a call, so when the last copy goes unclaimed, `onUnanswered` tells the owner that no window answered.
        /// </summary>
        class AnswerOnce
        {
        public:
            /// <summary>Arms the guard.</summary>
            /// <param name="onUnanswered">The "no window answered" report (a throw is logged and dropped).</param>
            explicit AnswerOnce(std::function<void()> onUnanswered) noexcept :
                _onUnanswered{ std::move(onUnanswered) }
            {
            }

            /// <summary>Runs the report when no window claimed the answer; contains anything it throws (a destructor never throws).</summary>
            ~AnswerOnce()
            {
                if (_claimed.exchange(true))
                {
                    return;
                }
                try
                {
                    if (_onUnanswered)
                    {
                        _onUnanswered();
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: the "no window answered" line is lost; the owner sees no answer for this command.
                    LogSwallowedException(L"TeamsService AnswerOnce (no window answered)");
                }
            }

            AnswerOnce(const AnswerOnce&) = delete;
            AnswerOnce& operator=(const AnswerOnce&) = delete;

            /// <summary>Claims the one answer for the calling reply.</summary>
            /// <returns>True for the first caller only.</returns>
            bool Claim() noexcept
            {
                return !_claimed.exchange(true);
            }

        private:
            /// <summary>A window answered (or the destructor ran).</summary>
            std::atomic<bool> _claimed{ false };
            /// <summary>The "no window answered" report.</summary>
            std::function<void()> _onUnanswered;
        };

        /// <summary>A file's size, without opening it (the transcript claude appends to).</summary>
        /// <param name="path">The file.</param>
        /// <returns>The size in bytes; -1 when the file is gone or cannot be read.</returns>
        int64_t FileSizeOf(const std::wstring& path) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
            {
                return -1;
            }
            return static_cast<int64_t>((static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow);
        }

        /// <summary>
        /// Reads [from, to) of a file, or less when the file is shorter now. Shared read, write and delete: claude appends to
        /// the transcript while this reads, and must never be blocked by it.
        /// </summary>
        /// <param name="path">The file.</param>
        /// <param name="from">The first byte.</param>
        /// <param name="to">The byte after the last.</param>
        /// <returns>The bytes read (fewer when the file is shorter or cannot be read).</returns>
        /// <exception cref="std::bad_alloc">If the buffer cannot be allocated.</exception>
        std::string ReadFileRange(const std::wstring& path, int64_t from, int64_t to)
        {
            std::string out;
            if (to <= from)
            {
                return out;
            }
            // Closes the handle on every path, an allocation failure's too (it goes to the caller, whose pass is lost and logged).
            struct HandleCloser
            {
                HANDLE h;
                ~HandleCloser()
                {
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        ::CloseHandle(h);
                    }
                }
            };
            const HandleCloser file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                                   FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (file.h == INVALID_HANDLE_VALUE)
            {
                return out;
            }
            LARGE_INTEGER pos{};
            pos.QuadPart = from;
            if (::SetFilePointerEx(file.h, pos, nullptr, FILE_BEGIN))
            {
                out.resize(static_cast<size_t>(to - from));
                size_t got = 0;
                while (got < out.size())
                {
                    DWORD read = 0;
                    const DWORD want = static_cast<DWORD>((std::min)(out.size() - got, size_t{ 16u * 1024u * 1024u }));
                    if (!::ReadFile(file.h, out.data() + got, want, &read, nullptr) || read == 0)
                    {
                        break;
                    }
                    got += read;
                }
                out.resize(got);
            }
            return out;
        }

        /// <summary>
        /// Reads the COMPLETE lines of [from, size) — at most `firstWindow` bytes, more when not one line ends in it (growing ×4
        /// up to kWindowMaxBytes): the bytes up to and including the last newline read. A torn last line (still being written)
        /// is never part of it.
        /// </summary>
        /// <param name="path">The transcript.</param>
        /// <param name="from">A line start.</param>
        /// <param name="size">The file's size.</param>
        /// <param name="firstWindow">The first window's size.</param>
        /// <param name="skipTo">Receives where to go on when one line is longer than the largest window (-1 = no skip).</param>
        /// <param name="reachedEnd">Receives whether the read reached the file's end: every complete line of the file is in the
        /// result (a torn last line aside).</param>
        /// <returns>The complete lines' bytes ("" = none yet, or the file shrank).</returns>
        /// <exception cref="std::bad_alloc">If the buffer cannot be allocated.</exception>
        std::string ReadCompleteLines(const std::wstring& path, int64_t from, int64_t size, int64_t firstWindow, int64_t& skipTo, bool& reachedEnd)
        {
            skipTo = -1;
            reachedEnd = false;
            for (int64_t want = firstWindow;; want *= 4)
            {
                const int64_t to = (std::min)(size, from + want);
                std::string bytes = ReadFileRange(path, from, to);
                if (bytes.size() < static_cast<size_t>(to - from))
                {
                    return {}; // shrunk or gone: the next pass looks again
                }
                reachedEnd = to >= size;
                const size_t lastNl = bytes.rfind('\n');
                if (lastNl != std::string::npos)
                {
                    bytes.resize(lastNl + 1);
                    return bytes;
                }
                if (reachedEnd)
                {
                    return {}; // the only line left is still being written
                }
                if (want >= kWindowMaxBytes)
                {
                    skipTo = to; // one line longer than the largest window: skipped (its torn rest reads as unparsable)
                    return {};
                }
            }
        }

        /// <summary>
        /// The state file's round trip (Persistence.h StateRoundTrip): what this build makes of a parsed file, so what it does
        /// not understand (a newer build's members) is kept by the save (PERSISTENCE.md §17).
        /// </summary>
        /// <param name="disk">The parsed file.</param>
        /// <returns>This build's document for it (an empty object for a file it cannot read).</returns>
        /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
        json::Value TeamsStateRoundTrip(const json::Value& disk)
        {
            if (auto s = ChannelStateFromJson(disk))
            {
                return ChannelStateToJson(*s);
            }
            return json::Value::MkObj();
        }

        /// <summary>A clock word the owner reads ("14:09", local time).</summary>
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

        /// <summary>An answer in a few words for the status line and the log ("HTTP 403 Forbidden: …", "aborted").</summary>
        /// <param name="r">The answer.</param>
        /// <returns>The words.</returns>
        /// <exception cref="std::bad_alloc">If the words cannot be allocated.</exception>
        std::wstring Describe(const GraphResult& r)
        {
            if (r.httpStatus == 0)
            {
                return r.description.empty() ? std::wstring{ L"no answer" } : r.description;
            }
            std::wstring s = L"HTTP " + std::to_wstring(r.httpStatus);
            if (!r.errorCode.empty())
            {
                s += L" " + r.errorCode;
            }
            if (!r.description.empty() && r.description != r.errorCode)
            {
                s += L": " + r.description;
            }
            return s;
        }

        /// <summary>A source's name in the status line.</summary>
        /// <param name="source">The router's index.</param>
        /// <returns>"the app" or "the Azure CLI".</returns>
        const wchar_t* SourceWord(size_t source) noexcept
        {
            return source == 0 ? L"the app" : L"the Azure CLI";
        }

        /// <summary>A source's name in the log.</summary>
        /// <param name="source">The router's index.</param>
        /// <returns>"app" or "azure-cli".</returns>
        const wchar_t* SourceLogName(size_t source) noexcept
        {
            return source == 0 ? L"app" : L"azure-cli";
        }
    }

    TeamsService::TeamsService(std::shared_ptr<SessionRegistry> registry, std::shared_ptr<Scheduler> scheduler, std::wstring profileDir, Msg::ServiceHooks hooks) :
        _registry(std::move(registry)),
        _scheduler(std::move(scheduler)),
        _profileDir(std::move(profileDir)),
        _hooks(std::move(hooks)),
        _app(_profileDir),
        _cli(std::make_shared<AzureCliSource>())
    {
    }

    TeamsService::~TeamsService()
    {
        Stop();
    }

    void TeamsService::_log(const std::wstring& line) const noexcept
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

    void TeamsService::_noteProblem(std::wstring problem) noexcept
    {
        try
        {
            bool changed = false;
            {
                std::lock_guard lk{ _mtx };
                changed = problem != _problem;
                _problem = problem;
            }
            if (changed && !problem.empty())
            {
                _log(L"[teams] " + problem);
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_noteProblem");
        }
    }

    void TeamsService::_wake() noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                _tickWake = true;
            }
            _cv.notify_all();
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_wake");
        }
    }

    // ===== Apply, start, stop ================================================================================================

    void TeamsService::Apply(const AppSettings& settings) noexcept
    {
        try
        {
            std::lock_guard applyLk{ _applyMtx };
            const TeamsConfig next = TeamsConfigFrom(settings);
            TeamsConfig current;
            bool running = false;
            {
                std::lock_guard lk{ _mtx };
                current = _config;
                running = _running;
            }
            const bool wants = WantsToRun(next);
            // A restart stops BEFORE the new config is set: the old threads must never act on the new channel with the old
            // channel's state (a legend id of one channel edited in another).
            if (running && (!wants || RestartNeeded(current, next)))
            {
                _stop();
                running = false;
            }
            bool searchThinkingChanged = false;
            std::shared_ptr<Msg::SearchBridge> search;
            {
                std::lock_guard lk{ _mtx };
                _config = next;
                _colorMode = settings.tabColorMode;
                // §5: a session gets a thread by Telegram's rule, with Telegram's idle grace (one setting for both connectors).
                _idleGraceMs = static_cast<int64_t>(ClampMessagingIdleGraceMinutes(settings.messagingIdleGraceMinutes)) * 60'000;
                // §8 📜: the level a bare /allscreen draws is the cog's Messaging tab's (one setting for both connectors).
                _allScreenLevel = Msg::AllScreenLevelSetting(settings.messagingAllScreenLevel);
                // §9.3: the Search conversation's thinking level, the cog's Messaging tab's (one setting for both connectors);
                // a running bridge takes it below, outside the lock.
                searchThinkingChanged = _searchThinking != settings.messagingSearchThinking;
                _searchThinking = settings.messagingSearchThinking;
                search = _search;
                _autoSignInTried = false; // each Apply may offer one new sign-in code (a cog Save is a way to ask for one)
            }
            if (search && searchThinkingChanged)
            {
                // The RUNNING conversation takes the new level from its next request (SetThinkingLevel never throws).
                search->SetThinkingLevel(settings.messagingSearchThinking);
            }
            _app.Configure(next.clientId, next.tenant);
            if (wants && !running)
            {
                _start();
            }
            else if (!wants && (current.enabled != next.enabled || current.channelId != next.channelId))
            {
                _log(next.enabled ? L"[teams] not started: set the team and the channel (teamsTeamId, teamsChannelId)" : L"[teams] off");
            }
            _wake();
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::Apply");
        }
    }

    void TeamsService::Stop() noexcept
    {
        try
        {
            std::lock_guard applyLk{ _applyMtx };
            _stop();
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::Stop");
        }
    }

    bool TeamsService::Running() const noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            return _running;
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::Running");
            return false;
        }
    }

    void TeamsService::_start() noexcept
    {
        try
        {
            const std::wstring dir = _profileDir + L"\\messaging\\teams";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
            {
                // The state cannot be saved; the connector still runs (the next save tries the folder again through the writer).
                _log(L"[teams] could not create " + dir + L" (error " + std::to_wstring(ec.value()) + L")");
            }
            TeamsConfig config;
            {
                std::lock_guard lk{ _mtx };
                config = _config;
                _stopping = false;
                _tickWake = false;
                _http = std::make_shared<GraphClient>();
                _stateFile = dir + L"\\" + ChannelStateFileLeaf(_config.teamId, _config.channelId);
                // An empty state for this channel until the tick loads its file: nothing of a channel this service ran on
                // before (its legend id, its threads) may be shown or acted on meanwhile.
                _state = ChannelState{};
                _state.teamId = _config.teamId;
                _state.channelId = _config.channelId;
                _stateDirty = false;
                _stateFileReadable = false;
                _lastPrevCopyMs = 0;
                _lastPruneMs = 0;
                _owner = Owner{};
                // What a stopped run had in flight (a history window, a root post) is not waited for: its answers never came.
                _runtime.clear();
                _lastThreadsStepMs = 0;
                // M4: the poll's clocks; a reply older than this start was written while Agentmaster was closed (§7.4), and a
                // post older than it is history (§9.1).
                _startedMs = TeamsNowMs();
                _stateLoaded = false;
                _lastOwnerActionMs = 0;
                _lastChangeMs = _startedMs;
                _ownerAwaited = false;
                _lastReadMs = 0;
                _tracks.clear();
                _teamsPrompts.clear();
                _ownFingerprints.clear();
                _lastEscMs.clear();
                // §9.1: a new run's posts are new to it (a waiting step is known again by its watch); a launch a stopped run
                // asked binds nothing in this one (LaunchAsk::runMs), so its session may get a thread of its own.
                _wizardRoots.clear();
                _wizardRepliesMs.clear();
                _launching.clear();
                _wantThread.clear(); // a fork asked by a stopped run gets a thread by Telegram's rule only
                // §9.3: a request written in the Search thread while Agentmaster was closed is not run; the last answer's rows
                // are forgotten (a number after a restart asks to search again).
                _searchReadToMs = _startedMs;
                _searchRows.clear();
                _searchPosting = false;
                _searchRetryAtMs = 0;
                _outbox.clear();
                _refused.clear();
                _nextSendMs = 0;
                _legendPosting = false;
                _legendEditQueued = false;
                _legendRetryAtMs = 0;
                _legendBoardHash = 0;
                _lastLegendEditMs = 0;
                _lastLegendBuildMs = 0;
                _postsBy.clear();
                _editsBy.clear();
                _readsBy.clear();
                _reactsBy.clear();
                _cliProblem.clear();
                _problem.clear();
                _running = true;
            }
            try
            {
                _tick = std::thread([this] { _tickLoop(); });
                _send = std::thread([this] { _sendLoop(); });
                _poll = std::thread([this] { _pollLoop(); });
            }
            catch (...)
            {
                // Rule #18; recovery: what started is stopped again, and the status line says the connector could not start.
                LogSwallowedException(L"TeamsService::_start (threads)");
                _stop();
                _noteProblem(L"could not start its threads");
                return;
            }
            _log(L"[teams] started: team " + config.teamId + L", channel " + config.channelId + (config.clientId.empty() ? L", no app" : L", app " + config.clientId) +
                 (config.useAzureCli ? L", the Azure CLI on" : L", the Azure CLI off"));
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_start");
        }
    }

    void TeamsService::_stop() noexcept
    {
        try
        {
            std::shared_ptr<GraphClient> http;
            {
                std::lock_guard lk{ _mtx };
                if (!_running && !_tick.joinable() && !_send.joinable() && !_poll.joinable())
                {
                    return;
                }
                _stopping = true;
                http = _http;
            }
            _cv.notify_all();
            // A call in flight returns at once ("aborted"), and no new one starts: the joins below never wait for the network.
            if (http)
            {
                http->Shutdown();
            }
            if (_tick.joinable())
            {
                _tick.join();
            }
            if (_send.joinable())
            {
                _send.join();
            }
            if (_poll.joinable())
            {
                _poll.join();
            }
            try
            {
                _saveState(true);
            }
            catch (...)
            {
                // Rule #18; recovery: the state keeps its last save (the next start mirrors from there).
                LogSwallowedException(L"TeamsService::_stop (save)");
            }
            size_t dropped = 0;
            std::shared_ptr<Msg::SearchBridge> search;
            {
                std::lock_guard lk{ _mtx };
                dropped = _outbox.size();
                _outbox.clear();
                _running = false;
                _http.reset();
                // §9.3: the bridge detaches from the Search conversation (which is the process's and goes on); destroyed below,
                // outside the lock, so its detach never runs under _mtx.
                search = std::move(_search);
                _search.reset();
            }
            search.reset();
            _log(L"[teams] stopped" + (dropped ? L" (" + std::to_wstring(dropped) + L" queued calls dropped)" : std::wstring{}));
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_stop");
        }
    }

    // ===== the status line and the cog's buttons =============================================================================

    std::wstring TeamsService::StatusText() const
    {
        StatusFacts f;
        bool appConfigured = false;
        bool useCli = false;
        std::wstring cliProblem;
        {
            std::lock_guard lk{ _mtx };
            f.enabled = _config.enabled;
            f.channelSet = !_config.teamId.empty() && !_config.channelId.empty();
            f.running = _running;
            f.ownerName = _owner.name;
            f.postsBy = _postsBy;
            f.editsBy = _editsBy;
            f.readsBy = _readsBy;
            f.reactsBy = _reactsBy;
            f.threads = _state.threads.size();
            f.legendPosted = !_state.legendId.empty();
            f.problem = _problem;
            appConfigured = !_config.clientId.empty();
            useCli = _config.useAzureCli;
            cliProblem = _cliProblem;
        }
        if (const auto pending = _app.Pending())
        {
            f.signInUri = pending->verificationUri;
            f.signInCode = pending->userCode;
            f.signInUntil = ClockWord(pending->expiresMs);
        }
        // With no newer problem, the line names the first source that is not ready (the app's sign-in, then the CLI).
        if (f.problem.empty() && appConfigured && f.signInCode.empty())
        {
            const std::wstring app = _app.StateText();
            if (!app.empty() && app != L"ready")
            {
                f.problem = L"the app: " + app;
            }
        }
        if (f.problem.empty() && useCli)
        {
            if (!cliProblem.empty())
            {
                f.problem = cliProblem;
            }
            else
            {
                const std::wstring cli = _cli->StateText();
                if (!cli.empty() && cli != L"ready")
                {
                    f.problem = L"the Azure CLI: " + cli;
                }
            }
        }
        return BuildStatusLine(f);
    }

    void TeamsService::NoteAppClosing() noexcept
    {
        try
        {
            _saveState(true);
        }
        catch (...)
        {
            // Rule #18; recovery: the state keeps its last save.
            LogSwallowedException(L"TeamsService::NoteAppClosing");
        }
    }

    void TeamsService::SetSearchFactory(Msg::SearchBridgeFactory factory) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                _searchFactory = std::move(factory);
            }
            _wake(); // a running connector posts the Search thread's root at its next tick
        }
        catch (...)
        {
            // Rule #18. Recovery: no search in this run (std::function's move does not throw; the lock might).
            LogSwallowedException(L"TeamsService::SetSearchFactory");
        }
    }

    void TeamsService::RequestSignIn() noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                _signInWanted = true;
            }
            _wake();
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::RequestSignIn");
        }
    }

    void TeamsService::SignOut() noexcept
    {
        _app.SignOut();
        _wake();
    }

    // ===== the tick ==========================================================================================================

    void TeamsService::_tickLoop() noexcept
    {
        try
        {
            _loadState();
        }
        catch (...)
        {
            // Rule #18; recovery: the run starts with an empty state for the channel (the file is not replaced unread: the
            // guard keeps a copy of a file it could not use).
            LogSwallowedException(L"TeamsService::_loadState");
        }
        try
        {
            // The poll waits for this: it reads the delta link and the threads of the state just loaded (or of an empty one).
            {
                std::lock_guard lk{ _mtx };
                _stateLoaded = true;
            }
            _cv.notify_all();
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_tickLoop (state loaded)");
        }
        for (;;)
        {
            try
            {
                {
                    std::lock_guard lk{ _mtx };
                    if (_stopping)
                    {
                        break;
                    }
                }
                _tickOnce(TeamsNowMs());
            }
            catch (...)
            {
                // Rule #18; recovery: this pass is lost, the next one runs in a second.
                LogSwallowedException(L"TeamsService::_tickOnce");
            }
            try
            {
                std::unique_lock lk{ _mtx };
                // §9.1: a window's launch answer ends the wait too (its flag is set without _mtx: a wake-up lost to the race
                // between this check and the store costs one tick).
                _cv.wait_for(lk, std::chrono::milliseconds(1000), [this] { return _stopping || _tickWake || _launchAnswersPending.load(); });
                _tickWake = false;
            }
            catch (...)
            {
                LogSwallowedException(L"TeamsService::_tickLoop (wait)");
            }
        }
    }

    void TeamsService::_tickOnce(int64_t nowMs)
    {
        // First, and never throwing: it clears the flag that ends the tick's wait, so a step that throws below cannot keep the
        // wait from waiting (a busy loop); and before the threads, so a post bound to its new session is its thread in this pass.
        _settleLaunchAnswers(nowMs);
        _routeStep(nowMs);
        _signInStep(nowMs);
        _threadsStep(nowMs);
        _legendStep(nowMs);
        _searchRootStep(nowMs);
        _housekeeping(nowMs);
    }

    TeamsService::SourceTokens TeamsService::_sourceTokens(GraphClient& http, bool useCli)
    {
        SourceTokens t;
        t.app = _app.Token(http, false); // a refresh when its token is due; a fresh one costs a lock
        if (useCli)
        {
            // The app's tenant, when known, so both tokens speak for the same tenant.
            t.cli = _cli->Token(t.app ? std::wstring_view{ t.app->claims.tenantId } : std::wstring_view{});
        }
        // Never post as someone else: an Azure CLI signed in to another account than the app's is left out.
        if (t.app && t.cli && !t.app->claims.oid.empty() && !detail::EqualsAsciiNoCase(t.app->claims.oid, t.cli->claims.oid))
        {
            t.cliProblem = L"the Azure CLI is signed in as " + (t.cli->claims.name.empty() ? std::wstring{ L"another account" } : t.cli->claims.name) +
                           L", not as the app's " + (t.app->claims.name.empty() ? std::wstring{ L"account" } : t.app->claims.name) + L": it is not used";
            t.cli.reset();
        }
        t.views.resize(2);
        t.views[kAppSource].name = L"app";
        t.views[kAppSource].available = t.app.has_value();
        if (t.app)
        {
            t.views[kAppSource].scopes = t.app->claims.scopes;
        }
        t.views[kCliSource].name = L"azure-cli";
        t.views[kCliSource].available = t.cli.has_value();
        if (t.cli)
        {
            t.views[kCliSource].scopes = t.cli->claims.scopes;
        }
        return t;
    }

    void TeamsService::_routeStep(int64_t nowMs)
    {
        std::shared_ptr<GraphClient> http;
        bool useCli = false;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            useCli = _config.useAzureCli;
        }
        if (!http)
        {
            return;
        }
        const SourceTokens t = _sourceTokens(*http, useCli);
        const std::vector<RefusedPair> refused = _refusedNow(nowMs);
        const auto word = [&](GraphOp op) {
            const auto k = PickTokenFor(op, t.views, refused);
            return k ? std::wstring{ SourceWord(*k) } : std::wstring{};
        };
        Owner owner;
        if (t.app)
        {
            owner = Owner{ t.app->claims.oid, t.app->claims.name };
        }
        else if (t.cli)
        {
            owner = Owner{ t.cli->claims.oid, t.cli->claims.name };
        }
        const std::wstring posts = word(GraphOp::Post);
        const std::wstring edits = word(GraphOp::Edit);
        const std::wstring reads = word(GraphOp::Read);
        const std::wstring reacts = word(GraphOp::React);
        std::wstring line;
        {
            std::lock_guard lk{ _mtx };
            // One log line whenever the routing changes (a sign-in finished, a token was refused, the CLI came or went).
            if (posts != _postsBy || edits != _editsBy || reads != _readsBy || reacts != _reactsBy)
            {
                line = L"[teams] routing: posts " + (posts.empty() ? std::wstring{ L"none" } : posts) + L", edits " + (edits.empty() ? std::wstring{ L"none" } : edits) +
                       L", reads " + (reads.empty() ? std::wstring{ L"none" } : reads) + L", reactions " + (reacts.empty() ? std::wstring{ L"none" } : reacts);
            }
            _postsBy = posts;
            _editsBy = edits;
            _readsBy = reads;
            _reactsBy = reacts;
            _cliProblem = t.cliProblem;
            if (!owner.id.empty())
            {
                _owner = owner;
            }
        }
        if (!line.empty())
        {
            _log(line);
        }
    }

    void TeamsService::_signInStep(int64_t nowMs)
    {
        std::shared_ptr<GraphClient> http;
        bool wanted = false;
        bool autoTried = false;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            wanted = _signInWanted;
            _signInWanted = false;
            autoTried = _autoSignInTried;
        }
        if (!http)
        {
            return;
        }
        if (!_app.Configured())
        {
            if (wanted)
            {
                _noteProblem(L"the sign-in needs an app (teamsClientId) and a tenant (teamsTenantId)");
            }
            return;
        }
        if (_app.Pending())
        {
            std::wstring error;
            const SignInPoll poll = _app.PollSignIn(*http, nowMs, error);
            if (poll == SignInPoll::Done)
            {
                _noteProblem(L"");
                _cv.notify_all(); // a write waiting for a token can go now
            }
            else if (poll == SignInPoll::Declined || poll == SignInPoll::Expired || poll == SignInPoll::Failed)
            {
                _noteProblem(L"the sign-in ended: " + error + L" (save the settings again, or press Sign in, for a new code)");
            }
            if (!wanted)
            {
                return; // a wanted sign-in replaces the one that waits (below)
            }
        }
        bool start = wanted;
        if (!start && !autoTried)
        {
            // One automatic code per Apply, and only when the app has no stored sign-in or Microsoft refused it: a code that
            // expires unseen is not renewed by itself (the status line says how to get a new one).
            {
                std::lock_guard lk{ _mtx };
                _autoSignInTried = true;
            }
            const std::wstring state = _app.StateText();
            start = !_app.HasStoredSignIn() || state.rfind(L"sign in again", 0) == 0;
        }
        if (start)
        {
            std::wstring error;
            if (_app.BeginSignIn(*http, error))
            {
                _noteProblem(L"");
            }
            else
            {
                _noteProblem(L"the sign-in could not start: " + error);
            }
        }
    }

    // ===== the legend ========================================================================================================

    std::vector<FleetLine> TeamsService::_fleetLines(size_t& more)
    {
        more = 0;
        TabColorMode mode = TabColorMode::WorkingDirectory;
        std::map<std::wstring, std::wstring> threadLinks;
        {
            std::lock_guard lk{ _mtx };
            mode = _colorMode;
            for (const auto& [sid, t] : _state.threads)
            {
                threadLinks[sid] = t.webUrl;
            }
        }
        DirColorIndex dirColors;
        try
        {
            dirColors = LoadDirColorIndex();
        }
        catch (...)
        {
            // Rule #18. Recovery: an empty index — every folder resolves to its automatic color this pass.
            LogSwallowedException(L"TeamsService::_fleetLines (dir colors)");
        }
        std::vector<std::pair<int64_t, FleetLine>> keyed;
        if (_registry)
        {
            for (const SessionInfo& s : _registry->SnapshotLiveForCards())
            {
                // D-C4: every Claude tab — an open session with a tab this run; Codex is not mirrored.
                if (!s.live || s.tabToken.empty() || s.kind != AgentKind::Claude)
                {
                    continue;
                }
                FleetLine line;
                line.title = s.title;
                line.state = s.state;
                try
                {
                    line.colorHex = ResolveSessionColorHex(mode, s, dirColors);
                }
                catch (...)
                {
                    // Rule #18. Recovery: no color square for this line.
                    LogSwallowedException(L"TeamsService::_fleetLines (color)");
                }
                if (const auto it = threadLinks.find(s.id); it != threadLinks.end())
                {
                    line.webUrl = it->second;
                }
                keyed.emplace_back(MostRecentSortKeyMs(s), std::move(line));
            }
        }
        // The most recent first: the side panel's MOST RECENT key, as the Telegram topics are ordered.
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<FleetLine> lines;
        lines.reserve((std::min)(keyed.size(), kLegendFleetMax));
        for (auto& entry : keyed)
        {
            if (lines.size() == kLegendFleetMax)
            {
                ++more;
                continue;
            }
            lines.push_back(std::move(entry.second));
        }
        return lines;
    }

    void TeamsService::_legendStep(int64_t nowMs)
    {
        TeamsConfig config;
        std::wstring legendId;
        bool posting = false;
        bool editQueued = false;
        int64_t retryAt = 0;
        int64_t lastEdit = 0;
        uint64_t boardHash = 0;
        bool canPost = false;
        bool canEdit = false;
        Owner owner;
        {
            std::lock_guard lk{ _mtx };
            if (nowMs - _lastLegendBuildMs < kLegendBuildGapMs)
            {
                return;
            }
            _lastLegendBuildMs = nowMs;
            config = _config;
            legendId = _state.legendId;
            posting = _legendPosting;
            editQueued = _legendEditQueued;
            retryAt = _legendRetryAtMs;
            lastEdit = _lastLegendEditMs;
            boardHash = _legendBoardHash;
            canPost = !_postsBy.empty();
            canEdit = !_editsBy.empty();
            owner = _owner;
        }
        if (legendId.empty() ? (!canPost || posting || nowMs < retryAt) : (!canEdit || editQueued || nowMs - lastEdit < kLegendEditGapMs))
        {
            return; // nothing to do now (the board is built only when it could be sent)
        }
        size_t more = 0;
        const std::vector<FleetLine> lines = _fleetLines(more);
        // What decides an edit is the board itself, without its clock word: an unchanged fleet is never edited.
        const uint64_t hash = CardHash(kLegendSubject, BuildLegendHtml(lines, L"", more));
        if (!legendId.empty() && hash == boardHash)
        {
            return;
        }
        OutboxItem item;
        item.op = legendId.empty() ? GraphOp::Post : GraphOp::Edit;
        const std::wstring legendHtml = BuildLegendHtml(lines, ClockWord(nowMs), more);
        // ⚠ An edit carries the subject too: a PATCH without it clears the root's subject (TEAMS.md §15.4).
        item.body = BuildPostJson(legendHtml, kLegendSubject, AlertKind::None, owner);
        if (legendId.empty())
        {
            _rememberOwnPost(L"", legendHtml); // the new legend is never read as the owner's new post
            item.priority = OutboxPriority::Card;
            item.method = L"POST";
            item.path = ChannelMessagesPath(config.teamId, config.channelId);
            item.what = L"legend post";
            item.done = [this, hash](const GraphResult& r) { _onLegendPosted(r, hash); };
            {
                std::lock_guard lk{ _mtx };
                _legendPosting = true;
            }
        }
        else
        {
            item.priority = OutboxPriority::Legend;
            item.method = L"PATCH";
            item.path = MessagePath(config.teamId, config.channelId, legendId);
            item.what = L"legend edit";
            item.done = [this, hash, legendId](const GraphResult& r) { _onLegendEdited(r, hash, legendId); };
            {
                std::lock_guard lk{ _mtx };
                _legendEditQueued = true;
            }
        }
        _enqueue(std::move(item));
    }

    void TeamsService::_onLegendPosted(const GraphResult& r, uint64_t boardHash) noexcept
    {
        try
        {
            const int64_t now = TeamsNowMs();
            std::wstring id;
            if (r.Ok())
            {
                id = r.body.StrAt(L"id");
            }
            {
                std::lock_guard lk{ _mtx };
                _legendPosting = false;
                if (!id.empty())
                {
                    _state.legendId = id;
                    _state.ownPosts[id] = now; // the journal: Agentmaster's own post, never read as the owner's (§7.2)
                    _stateDirty = true;
                    _legendBoardHash = boardHash;
                    _lastLegendEditMs = now;
                }
                else
                {
                    _legendRetryAtMs = now + kLegendRetryMs;
                }
            }
            if (!id.empty())
            {
                _log(L"[teams] the legend is posted (message " + id + L"): pin it in the channel");
                _noteProblem(L"");
            }
            else
            {
                _noteProblem(L"the legend could not be posted (" + (r.Ok() ? std::wstring{ L"the answer had no id" } : Describe(r)) + L")");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_onLegendPosted");
        }
    }

    void TeamsService::_onLegendEdited(const GraphResult& r, uint64_t boardHash, const std::wstring& legendId) noexcept
    {
        try
        {
            bool gone = false;
            {
                std::lock_guard lk{ _mtx };
                _legendEditQueued = false;
                _lastLegendEditMs = TeamsNowMs();
                if (r.Ok())
                {
                    _legendBoardHash = boardHash;
                }
                else if (r.httpStatus == 404 && _state.legendId == legendId)
                {
                    // The owner deleted the legend post: the next pass posts a new one.
                    _state.legendId.clear();
                    _stateDirty = true;
                    gone = true;
                }
            }
            if (gone)
            {
                _log(L"[teams] the legend post is gone (deleted?): a new one is posted");
            }
            else if (!r.Ok() && !r.aborted)
            {
                _noteProblem(L"the legend could not be edited (" + Describe(r) + L")");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_onLegendEdited");
        }
    }

    // ===== the state =========================================================================================================

    void TeamsService::_housekeeping(int64_t nowMs) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                if (nowMs - _lastPruneMs >= kPruneGapMs)
                {
                    _lastPruneMs = nowMs;
                    if (PruneChannelState(_state, nowMs))
                    {
                        _stateDirty = true;
                    }
                }
            }
            _saveState(false);
        }
        catch (...)
        {
            // Rule #18; recovery: the state stays dirty and the next pass saves it.
            LogSwallowedException(L"TeamsService::_housekeeping");
        }
    }

    void TeamsService::_loadState()
    {
        TeamsConfig config;
        std::wstring file;
        {
            std::lock_guard lk{ _mtx };
            config = _config;
            file = _stateFile;
        }
        // Reads one candidate file into a state for THIS channel; nothing for a file that is not one.
        const auto readState = [&config](const StateFileRead& read) -> std::optional<ChannelState> {
            if (read.status != StateFileStatus::Read)
            {
                return std::nullopt;
            }
            const auto doc = json::Parse(read.text);
            if (!doc)
            {
                return std::nullopt;
            }
            auto s = ChannelStateFromJson(*doc);
            if (!s || s->teamId != config.teamId || s->channelId != config.channelId)
            {
                return std::nullopt;
            }
            return s;
        };
        ChannelState state;
        state.teamId = config.teamId;
        state.channelId = config.channelId;
        bool readable = false;
        std::wstring note;
        const StateFileRead read = ReadStateFileUtf8(file, kStartupStateReadRetryBudgetMs);
        if (read.status == StateFileStatus::Missing)
        {
            readable = true; // the first start in this channel: nothing saved is the truth
        }
        else if (auto s = readState(read))
        {
            state = std::move(*s);
            readable = true;
            MarkStateFileSafeToReplace(file); // loaded: its first replace needs no second read
        }
        else
        {
            // The file is there but this start cannot use it (locked, torn, another format): the first save keeps a copy of it
            // aside (PERSISTENCE.md §16), and the previous copy is the state to run with — without it every session would get a
            // second thread.
            if (read.status == StateFileStatus::Read)
            {
                MarkStateFileKeepBeforeReplace(file); // an Unreadable read was marked by the reader itself
            }
            if (auto prev = readState(ReadStateFileUtf8(file + L".prev", kStartupStateReadRetryBudgetMs)))
            {
                state = std::move(*prev);
                note = L"[teams] the state file could not be used: its previous copy (.prev) was, and the file is kept aside before it is next written";
            }
            else
            {
                note = L"[teams] the state file could not be used and has no usable previous copy: starting without threads; the file is kept aside before it is next written";
            }
        }
        {
            std::lock_guard lk{ _mtx };
            _state = std::move(state);
            _stateDirty = false;
            _stateFileReadable = readable;
        }
        if (!note.empty())
        {
            _log(note);
        }
    }

    void TeamsService::_saveState(bool force)
    {
        std::lock_guard saveLk{ _saveMtx };
        json::Value doc;
        std::wstring file;
        bool copyPrev = false;
        {
            std::lock_guard lk{ _mtx };
            if (!_stateDirty || _stateFile.empty())
            {
                return;
            }
            const int64_t now = TeamsNowMs();
            if (!force && now - _lastSaveMs < kStateSaveGapMs)
            {
                return;
            }
            doc = ChannelStateToJson(_state);
            file = _stateFile;
            _stateDirty = false;
            _lastSaveMs = now;
            // The previous copy is refreshed only from a file this run could read: copying a file it could not use over the
            // last good copy would destroy the one readable version left.
            copyPrev = _stateFileReadable && now - _lastPrevCopyMs >= kStatePrevCopyGapMs;
            if (copyPrev)
            {
                _lastPrevCopyMs = now;
            }
        }
        if (copyPrev)
        {
            std::error_code ec; // the first save has no file to copy yet: that fails quietly (the result is not needed)
            (void)std::filesystem::copy_file(file, file + L".prev", std::filesystem::copy_options::overwrite_existing, ec);
        }
        // PERSISTENCE.md §17: what the file holds that this build does not understand is kept; a newer format is not written.
        const bool written = WriteStateDocument(file, std::move(doc), &TeamsStateRoundTrip);
        std::lock_guard lk{ _mtx };
        if (written)
        {
            _stateFileReadable = true;
        }
        else
        {
            _stateDirty = true; // the next pass tries again (the writer logged why)
        }
    }

    // ===== the outbox ========================================================================================================

    void TeamsService::_enqueue(OutboxItem item)
    {
        {
            std::lock_guard lk{ _mtx };
            item.seq = ++_seq;
            _outbox.push_back(std::move(item));
        }
        _cv.notify_all();
    }

    void TeamsService::_requeue(OutboxItem item, int64_t notBeforeMs)
    {
        {
            std::lock_guard lk{ _mtx };
            if (_stopping)
            {
                return; // the stop drops the outbox anyway
            }
            item.notBeforeMs = notBeforeMs;
            _outbox.push_back(std::move(item)); // its seq is kept: it goes before the writes that came after it
        }
        _cv.notify_all();
    }

    std::vector<RefusedPair> TeamsService::_refusedNow(int64_t nowMs)
    {
        std::lock_guard lk{ _mtx };
        _refused.erase(std::remove_if(_refused.begin(), _refused.end(), [nowMs](const auto& entry) { return entry.second <= nowMs; }), _refused.end());
        std::vector<RefusedPair> pairs;
        pairs.reserve(_refused.size());
        for (const auto& entry : _refused)
        {
            pairs.push_back(entry.first);
        }
        return pairs;
    }

    void TeamsService::_sendLoop() noexcept
    {
        for (;;)
        {
            try
            {
                OutboxItem item;
                {
                    std::unique_lock lk{ _mtx };
                    for (;;)
                    {
                        if (_stopping)
                        {
                            return;
                        }
                        const int64_t now = TeamsNowMs();
                        // The next write: among those whose wait is over, the lowest priority value, then the earliest arrival —
                        // but only a lane's FIRST write may go: a thread's posts never overtake each other (a retry in back-off
                        // holds its lane).
                        auto best = _outbox.end();
                        int64_t nextDue = INT64_MAX;
                        for (auto it = _outbox.begin(); it != _outbox.end(); ++it)
                        {
                            if (!it->lane.empty())
                            {
                                const bool behind = std::any_of(_outbox.begin(), _outbox.end(), [&it](const OutboxItem& other) {
                                    return other.lane == it->lane && other.seq < it->seq;
                                });
                                if (behind)
                                {
                                    continue;
                                }
                            }
                            if (it->notBeforeMs > now)
                            {
                                nextDue = (std::min)(nextDue, it->notBeforeMs);
                                continue;
                            }
                            if (best == _outbox.end() || it->priority < best->priority || (it->priority == best->priority && it->seq < best->seq))
                            {
                                best = it;
                            }
                        }
                        if (best != _outbox.end() && now >= _nextSendMs)
                        {
                            item = std::move(*best);
                            _outbox.erase(best);
                            break;
                        }
                        // Sleep until the gap ends (a write is ready) or the first back-off ends; at most a second, so a stop or a
                        // clock change is seen.
                        const int64_t wakeAt = best != _outbox.end() ? _nextSendMs : nextDue;
                        int64_t waitMs = 1000;
                        if (wakeAt != INT64_MAX)
                        {
                            waitMs = (std::max)(int64_t{ 1 }, (std::min)(int64_t{ 1000 }, wakeAt - now));
                        }
                        _cv.wait_for(lk, std::chrono::milliseconds(waitMs));
                    }
                }
                _sendOne(std::move(item));
            }
            catch (...)
            {
                // Rule #18; recovery: this write is lost (logged), the loop goes on after a pause.
                LogSwallowedException(L"TeamsService::_sendLoop");
                try
                {
                    std::unique_lock lk{ _mtx };
                    _cv.wait_for(lk, std::chrono::milliseconds(1000), [this] { return _stopping; });
                }
                catch (...)
                {
                    LogSwallowedException(L"TeamsService::_sendLoop (pause)");
                }
            }
        }
    }

    void TeamsService::_sendOne(OutboxItem item) noexcept
    {
        try
        {
            std::shared_ptr<GraphClient> http;
            bool useCli = false;
            {
                std::lock_guard lk{ _mtx };
                http = _http;
                useCli = _config.useAzureCli;
            }
            if (!http)
            {
                return; // stopping
            }
            const SourceTokens tokens = _sourceTokens(*http, useCli);
            const std::optional<size_t> source = PickTokenFor(item.op, tokens.views, _refusedNow(TeamsNowMs()));
            if (!source && !item.lane.empty())
            {
                // A thread's content (a reply, a root) waits in its lane for a token (a refresh that failed, a sign-in under
                // way): dropping it would lose a message, and its lane keeps the order.
                _noteProblem(L"nothing can " + std::wstring{ GraphOpWord(item.op) } + L" now: the thread's posts wait");
                const int64_t until = TeamsNowMs() + kNoTokenWaitMs;
                _requeue(std::move(item), until);
                return;
            }
            if (!source)
            {
                // No token can make this kind of call (§4.2): the feature degrades, and the caller hears it at once.
                GraphResult none;
                none.transportFailed = true;
                none.refused = true;
                none.description = L"no token can " + std::wstring{ GraphOpWord(item.op) };
                _noteProblem(L"nothing can " + std::wstring{ GraphOpWord(item.op) } + L" now (" + item.what + L")");
                if (item.done)
                {
                    item.done(none);
                }
                return;
            }
            const AccessToken& token = *source == kAppSource ? *tokens.app : *tokens.cli;
            const GraphResult r = http->Call(item.method, item.path, token.token, item.body.type == json::Value::Type::Null ? nullptr : &item.body, item.what);
            {
                std::lock_guard lk{ _mtx };
                _nextSendMs = TeamsNowMs() + kSendGapMs; // §6.5: one write per 1.2 s for the whole channel, failures included
            }
            if (r.aborted)
            {
                return; // the stop: nobody waits for the answer any more
            }
            if (r.Ok())
            {
                // A write went out: a "nothing can …" problem of a missing token is over.
                std::lock_guard lk{ _mtx };
                if (_problem.rfind(L"nothing can", 0) == 0)
                {
                    _problem.clear();
                }
            }
            if (r.httpStatus == 401 && !item.renewedToken)
            {
                // The token was refused (revoked, or expired early): that source renews it, and the write goes again at once.
                if (*source == kAppSource)
                {
                    _app.Forget();
                }
                else
                {
                    _cli->Forget();
                }
                item.renewedToken = true;
                _requeue(std::move(item), 0);
                return;
            }
            if (r.httpStatus == 403)
            {
                // This source may not make this kind of call (§4.2): the router skips the pair for 30 minutes, and the write goes
                // once to the other source when it can make it.
                {
                    std::lock_guard lk{ _mtx };
                    _refused.emplace_back(RefusedPair{ *source, item.op }, TeamsNowMs() + kRefusedForMs);
                }
                _log(L"[teams] " + std::wstring{ SourceLogName(*source) } + L" may not " + GraphOpWord(item.op) + L" (403): the router skips it for 30 minutes");
                if (!item.triedOtherSource)
                {
                    const auto next = PickTokenFor(item.op, tokens.views, _refusedNow(TeamsNowMs()));
                    if (next && *next != *source)
                    {
                        item.triedOtherSource = true;
                        _requeue(std::move(item), 0);
                        return;
                    }
                }
            }
            else if (r.httpStatus == 429)
            {
                // Throttled: the whole channel waits Retry-After, and this write keeps its place.
                const int64_t until = TeamsNowMs() + ThrottleWaitMs(r.retryAfterSec);
                {
                    std::lock_guard lk{ _mtx };
                    _nextSendMs = (std::max)(_nextSendMs, until);
                }
                _requeue(std::move(item), until);
                return;
            }
            else if (r.Retryable())
            {
                ++item.attempts;
                if (item.attempts < kMaxSendAttempts)
                {
                    const int64_t until = TeamsNowMs() + RetryBackoffMs(item.attempts);
                    _requeue(std::move(item), until);
                    return;
                }
                _log(L"[teams] " + item.what + L" given up after " + std::to_wstring(item.attempts) + L" attempts (" + Describe(r) + L")");
            }
            if (item.done)
            {
                item.done(r);
            }
        }
        catch (...)
        {
            // Rule #18; recovery: this write is lost (its answer never comes; the step that queued it tries again on its own
            // schedule: the legend's flags are reset by the next start, and the threads' content is read again from the
            // committed cursor at the next start).
            LogSwallowedException(L"TeamsService::_sendOne");
        }
    }

    // ===== the threads (§5, §6) ==============================================================================================

    void TeamsService::_threadsStep(int64_t nowMs)
    {
        bool canPost = false;
        bool canEdit = false;
        TabColorMode mode = TabColorMode::WorkingDirectory;
        int64_t grace = 0;
        {
            std::lock_guard lk{ _mtx };
            if (nowMs - _lastThreadsStepMs < kThreadsStepGapMs)
            {
                return;
            }
            _lastThreadsStepMs = nowMs;
            canPost = !_postsBy.empty();
            canEdit = !_editsBy.empty();
            mode = _colorMode;
            grace = _idleGraceMs;
        }
        if (!canPost || !_registry)
        {
            return; // nothing can be posted yet: the threads wait for a token (the transcript cursors keep their place)
        }
        if (nowMs - _dirColorsAtMs >= kDirColorsGapMs)
        {
            _dirColorsAtMs = nowMs;
            try
            {
                _dirColors = LoadDirColorIndex();
            }
            catch (...)
            {
                // Rule #18. Recovery: the last index stays (or none: every folder's automatic color).
                LogSwallowedException(L"TeamsService::_threadsStep (dir colors)");
            }
        }
        const auto colorOf = [this, mode](const SessionInfo& s) {
            try
            {
                return ResolveSessionColorHex(mode, s, _dirColors);
            }
            catch (...)
            {
                // Rule #18. Recovery: no color square for this session this pass.
                LogSwallowedException(L"TeamsService::_threadsStep (color)");
                return std::wstring{};
            }
        };
        std::set<std::wstring> live;
        bool awaited = false;
        // The lean live records keep their OPEN prompts (the card counts the pending ones) and leave out the texts nothing here
        // reads (SessionRegistry.h SnapshotLiveForScan's contract).
        for (const SessionInfo& s : _registry->SnapshotLiveForScan())
        {
            if (s.kind != AgentKind::Claude)
            {
                continue; // D-C4: Claude Code tabs (a Codex rollout is not a transcript the mirror reads)
            }
            live.insert(s.id);
            // §7.3: a session that waits for the owner keeps the poll at 5 s (an answer from Teams then lands quickly).
            awaited = awaited || s.state == SessionState::WaitingForInput || s.state == SessionState::NeedsApproval;
            try
            {
                const std::wstring color = colorOf(s);
                bool hasThread = false;
                bool launching = false;
                bool wanted = false;
                {
                    std::lock_guard lk{ _mtx };
                    hasThread = _state.threads.count(s.id) > 0;
                    // §9.1: a session the owner's new post asked for, whose answer the tick has not settled yet: that post
                    // becomes its thread (set before the ask, and swapped for the thread under this same lock).
                    launching = !hasThread && _launching.count(s.id) > 0;
                    // §8 🔀: a fork the owner asked for here gets its thread whatever Telegram's rule says (once it has one,
                    // the wish is done).
                    wanted = !hasThread && _wantThread.count(s.id) > 0;
                    if (hasThread)
                    {
                        _runtime[s.id].closedShown = false; // open again: a later close is shown again
                        _wantThread.erase(s.id);
                    }
                }
                if (!hasThread)
                {
                    if (!launching && (QualifiesForThread(s, nowMs, grace) || (wanted && s.live)))
                    {
                        _postRoot(s, color, nowMs);
                    }
                    continue;
                }
                _cardStep(s, color, nowMs, canEdit);
                // The transcript before the approval notice: a question posted in this pass is the moment's one alert.
                _mirrorStep(s, nowMs);
                _approvalStep(s, nowMs);
            }
            catch (...)
            {
                // Rule #18; recovery: this session's pass is lost, the others go on, and the next pass tries it again.
                LogSwallowedException(L"TeamsService::_threadsStep (session)");
            }
        }
        // Threads whose session left the live set (closed, archived): the transcript is finished (the session rests, so a held
        // text is its final reply), and the card is brought up to date once ("closed").
        std::vector<std::wstring> gone;
        {
            std::lock_guard lk{ _mtx };
            _ownerAwaited = awaited;
            for (const auto& entry : _state.threads)
            {
                if (live.count(entry.first) == 0 && !_runtime[entry.first].closedShown)
                {
                    gone.push_back(entry.first);
                }
            }
        }
        for (const std::wstring& sid : gone)
        {
            try
            {
                bool shown = true;
                if (const auto record = _registry->Get(sid))
                {
                    _mirrorStep(*record, nowMs);
                    shown = _cardStep(*record, colorOf(*record), nowMs, canEdit);
                }
                if (shown)
                {
                    std::lock_guard lk{ _mtx };
                    _runtime[sid].closedShown = true;
                }
            }
            catch (...)
            {
                // Rule #18; recovery: the next pass tries this closed session again.
                LogSwallowedException(L"TeamsService::_threadsStep (closed session)");
            }
        }
        // The prompts sent from Teams: each one's status emoji follows its queue entry (👀 → 👨‍💻 → 👌).
        _trackStep(nowMs);
    }

    void TeamsService::_postRoot(const SessionInfo& s, const std::wstring& colorHex, int64_t nowMs)
    {
        TeamsConfig config;
        Owner owner;
        {
            std::lock_guard lk{ _mtx };
            ThreadRuntime& rt = _runtime[s.id];
            if (rt.rootPosting || nowMs < rt.rootRetryAtMs)
            {
                return;
            }
            rt.rootPosting = true;
            config = _config;
            owner = _owner;
        }
        const CardFacts facts = CardFactsFromSession(s, colorHex, nowMs);
        const std::wstring subject = CardSubject(s.state, colorHex, facts.title);
        const std::wstring html = BuildCardHtml(facts);
        const uint64_t hash = CardHash(subject, html);
        OutboxItem item;
        item.priority = OutboxPriority::Card;
        item.op = GraphOp::Post;
        item.method = L"POST";
        item.path = ChannelMessagesPath(config.teamId, config.channelId);
        item.body = BuildPostJson(html, subject, AlertKind::None, owner);
        item.what = L"thread for " + s.id.substr(0, 8);
        // In the session's lane: nothing of the thread can go before its root anyway (its replies need the root's id).
        item.lane = s.id;
        _rememberOwnPost(L"", html); // never read as the owner's new post, even before its answer journals it
        const std::wstring sid = s.id;
        item.done = [this, sid, hash](const GraphResult& r) { _onRootPosted(r, sid, hash); };
        _enqueue(std::move(item));
    }

    void TeamsService::_onRootPosted(const GraphResult& r, const std::wstring& sessionId, uint64_t cardHash) noexcept
    {
        try
        {
            const int64_t now = TeamsNowMs();
            std::wstring id;
            std::wstring url;
            if (r.Ok())
            {
                id = r.body.StrAt(L"id");
                url = r.body.StrAt(L"webUrl");
            }
            {
                std::lock_guard lk{ _mtx };
                ThreadRuntime& rt = _runtime[sessionId];
                rt.rootPosting = false;
                if (id.empty())
                {
                    rt.rootRetryAtMs = now + kRootRetryMs;
                }
                else
                {
                    Thread t;
                    t.sessionId = sessionId;
                    t.rootId = id;
                    t.webUrl = url;
                    t.cardHash = cardHash;
                    t.createdMs = now;
                    t.lastCardEditMs = now;
                    _state.threads[sessionId] = std::move(t);
                    // The journal: the root is Agentmaster's own post — the poll never reads it as the owner's new session (§7.2).
                    _state.ownPosts[id] = now;
                    _stateDirty = true;
                    rt.readOffset = -1;
                    rt.mirror = MirrorState{};
                    rt.historyInFlight = false;
                    rt.historyDayKey = 0;
                }
            }
            if (!id.empty())
            {
                _log(L"[teams] thread made for " + sessionId.substr(0, 8) + L" (message " + id + L"); its history follows");
            }
            else
            {
                _noteProblem(L"a thread could not be made (" + (r.Ok() ? std::wstring{ L"the answer had no id" } : Describe(r)) + L")");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_onRootPosted");
        }
    }

    bool TeamsService::_cardStep(const SessionInfo& s, const std::wstring& colorHex, int64_t nowMs, bool canEdit)
    {
        std::wstring rootId;
        std::wstring firstPrompt;
        uint64_t lastHash = 0;
        int64_t lastEdit = 0;
        bool queued = false;
        std::wstring lastStateLine;
        TeamsConfig config;
        Owner owner;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(s.id);
            if (it == _state.threads.end())
            {
                return true;
            }
            rootId = it->second.rootId;
            firstPrompt = it->second.firstPrompt; // §9.1: the owner's words of a post that became the card stay on it
            lastHash = it->second.cardHash;
            lastEdit = it->second.lastCardEditMs;
            const ThreadRuntime& rt = _runtime[s.id];
            queued = rt.cardEditQueued;
            lastStateLine = rt.lastStateLine;
            config = _config;
            owner = _owner;
        }
        const CardFacts facts = CardFactsFromSession(s, colorHex, nowMs, firstPrompt);
        const std::wstring subject = CardSubject(s.state, colorHex, facts.title);
        const std::wstring html = BuildCardHtml(facts);
        const uint64_t hash = CardHash(subject, html);
        if (hash == lastHash)
        {
            return true;
        }
        if (!canEdit)
        {
            // §4.2: no source can edit — the root stays as it was posted, and a changed state line is told as a short reply.
            if (subject != lastStateLine)
            {
                {
                    std::lock_guard lk{ _mtx };
                    _runtime[s.id].lastStateLine = subject;
                }
                _postReply(s.id, L"<i>" + Msg::TgEscapeHtml(subject) + L"</i>", AlertKind::None, OutboxPriority::Mirror, 0);
            }
            return true;
        }
        if (queued || nowMs - lastEdit < kCardEditGapMs)
        {
            return false; // the changes wait for the next edit (at most one per card per 10 s; they merge)
        }
        OutboxItem item;
        item.priority = OutboxPriority::Card;
        item.op = GraphOp::Edit;
        item.method = L"PATCH";
        item.path = MessagePath(config.teamId, config.channelId, rootId);
        // ⚠ The subject goes with the body: a PATCH without it clears the root's subject (TEAMS.md §15.4).
        item.body = BuildPostJson(html, subject, AlertKind::None, owner);
        item.what = L"card edit " + s.id.substr(0, 8);
        const std::wstring sid = s.id;
        item.done = [this, sid, hash, rootId](const GraphResult& r) { _onCardEdited(r, sid, hash, rootId); };
        {
            std::lock_guard lk{ _mtx };
            _runtime[s.id].cardEditQueued = true;
        }
        _enqueue(std::move(item));
        return false;
    }

    void TeamsService::_onCardEdited(const GraphResult& r, const std::wstring& sessionId, uint64_t cardHash, const std::wstring& rootId) noexcept
    {
        try
        {
            bool gone = false;
            {
                std::lock_guard lk{ _mtx };
                _runtime[sessionId].cardEditQueued = false;
                const auto it = _state.threads.find(sessionId);
                if (it != _state.threads.end() && it->second.rootId == rootId)
                {
                    it->second.lastCardEditMs = TeamsNowMs();
                    if (r.Ok())
                    {
                        it->second.cardHash = cardHash;
                        _stateDirty = true;
                    }
                    else if (r.httpStatus == 404)
                    {
                        // The owner deleted the thread: its record goes, and the next pass makes a new one (with the history).
                        _state.threads.erase(it);
                        _runtime.erase(sessionId);
                        _stateDirty = true;
                        gone = true;
                    }
                }
            }
            if (gone)
            {
                _log(L"[teams] the thread of " + sessionId.substr(0, 8) + L" is gone (deleted?): a new one is made");
            }
            else if (!r.Ok() && !r.aborted)
            {
                _noteProblem(L"a card could not be edited (" + Describe(r) + L")");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_onCardEdited");
        }
    }

    void TeamsService::_approvalStep(const SessionInfo& s, int64_t nowMs)
    {
        bool post = false;
        {
            std::lock_guard lk{ _mtx };
            ThreadRuntime& rt = _runtime[s.id];
            const bool entered = s.state == SessionState::NeedsApproval && (!rt.stateSeen || rt.lastState != SessionState::NeedsApproval);
            // A NeedsApproval restored at a start (it survives a restart, Rule #6) is no news: only a recent change rings.
            const bool fresh = s.stateChangedAtUnixMs > 0 && nowMs - s.stateChangedAtUnixMs < kAlertFreshMs;
            rt.lastState = s.state;
            rt.stateSeen = true;
            if (entered && fresh && nowMs - rt.lastNeedsYouMs > kNeedsYouQuietMs)
            {
                rt.lastNeedsYouMs = nowMs;
                post = true;
            }
        }
        if (post)
        {
            _postReply(s.id, ApprovalPostHtml(), AlertKind::NeedsYou, OutboxPriority::Alert, 0);
        }
    }

    void TeamsService::_mirrorStep(const SessionInfo& s, int64_t nowMs)
    {
        std::wstring path;
        bool historyDone = false;
        bool look = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(s.id);
            if (it == _state.threads.end())
            {
                return;
            }
            path = it->second.transcriptPath;
            historyDone = it->second.historyDone;
            ThreadRuntime& rt = _runtime[s.id];
            if (path.empty() && nowMs - rt.lastResolveMs >= kResolveGapMs)
            {
                rt.lastResolveMs = nowMs;
                look = true;
            }
        }
        if (path.empty())
        {
            if (!look)
            {
                return;
            }
            // A glob over the projects folders (ClaudeSpawn.h): at most every 10 s for a session that has no transcript yet.
            path = ResolveClaudeTranscriptPath(s.id);
            if (path.empty())
            {
                return; // not prompted yet: nothing to mirror
            }
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.threads.find(s.id); it != _state.threads.end())
            {
                it->second.transcriptPath = path;
                _stateDirty = true;
            }
        }
        if (!historyDone)
        {
            _historyStep(s.id, path);
        }
        else
        {
            _liveStep(s, path, nowMs);
        }
    }

    void TeamsService::_historyStep(const std::wstring& sessionId, const std::wstring& path)
    {
        int64_t pos = 0;
        int64_t dayKey = 0;
        bool verbose = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(sessionId);
            if (it == _state.threads.end())
            {
                return;
            }
            const ThreadRuntime& rt = _runtime[sessionId];
            if (rt.historyInFlight)
            {
                return; // one window at a time: the next one starts where this one's answers commit
            }
            pos = it->second.transcriptOffset;
            dayKey = rt.historyDayKey;
            verbose = _config.verbose;
        }
        const int64_t size = FileSizeOf(path);
        if (size < 0)
        {
            return;
        }
        int64_t skipTo = -1;
        bool reachedEnd = true;
        const std::string bytes = pos < size ? ReadCompleteLines(path, pos, size, kHistoryWindowBytes, skipTo, reachedEnd) : std::string{};
        if (skipTo > 0)
        {
            // One line longer than the largest window: it cannot be shown, so the replay goes on after it.
            _commit(sessionId, skipTo);
            _log(L"[teams] the history of " + sessionId.substr(0, 8) + L" skips one line longer than 64 MB");
            return;
        }
        if (bytes.empty())
        {
            if (!reachedEnd)
            {
                return; // the file is changing under the read: the next pass reads again
            }
            // Nothing complete past the cursor: the history is everything before it, and the live mirror takes over here.
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.threads.find(sessionId); it != _state.threads.end())
                {
                    it->second.historyDone = true;
                    _stateDirty = true;
                }
                _runtime[sessionId].readOffset = -1;
            }
            _log(L"[teams] the history of " + sessionId.substr(0, 8) + L" is in its thread; the live mirror follows");
            return;
        }
        Msg::HistoryWindow w = Msg::SplitHistoryWindow(bytes, pos, reachedEnd);
        if (w.needMore || w.consumedTo < 0)
        {
            return;
        }
        if (reachedEnd)
        {
            // The replay's last window: the newest turn is left to the live mirror, which posts it as it goes on — and its
            // final reply, when the turn ended recently, alerts like any other.
            CutNewestTurn(w);
        }
        const std::vector<Msg::HistoryPlanPost> plan = Msg::PlanHistoryPosts(w, dayKey, {}, verbose, L"");
        std::vector<HistoryChunk> chunks = PackHistoryPosts(plan);
        // A commit-only last chunk joins the post before it: its commit must not overtake that post's answer.
        if (chunks.size() >= 2 && chunks.back().html.empty())
        {
            const int64_t commit = chunks.back().commitTo;
            chunks.pop_back();
            chunks.back().commitTo = (std::max)(chunks.back().commitTo, commit);
        }
        // The window's end is committed with its last post, even when no turn ends inside it.
        if (chunks.empty())
        {
            chunks.push_back(HistoryChunk{ std::wstring{}, w.consumedTo });
        }
        else
        {
            chunks.back().commitTo = (std::max)(chunks.back().commitTo, w.consumedTo);
        }
        {
            std::lock_guard lk{ _mtx };
            ThreadRuntime& rt = _runtime[sessionId];
            rt.historyDayKey = dayKey;
            rt.historyInFlight = true;
        }
        const bool finishes = reachedEnd;
        // The window's last answer frees the replay for its next window (or hands the thread to the live mirror).
        const std::function<void(const GraphResult&)> windowDone = [this, sessionId, finishes](const GraphResult&) {
            try
            {
                std::lock_guard lk{ _mtx };
                ThreadRuntime& rt = _runtime[sessionId];
                rt.historyInFlight = false;
                if (finishes)
                {
                    if (const auto it = _state.threads.find(sessionId); it != _state.threads.end())
                    {
                        it->second.historyDone = true;
                        _stateDirty = true;
                    }
                    rt.readOffset = -1;
                }
            }
            catch (...)
            {
                LogSwallowedException(L"TeamsService::_historyStep (window done)");
            }
        };
        for (size_t k = 0; k < chunks.size(); ++k)
        {
            const bool last = k + 1 == chunks.size();
            if (chunks[k].html.empty())
            {
                // Only possible as the window's single chunk (above): nothing to post, so its commit and its end happen now.
                _commit(sessionId, chunks[k].commitTo);
                if (last)
                {
                    windowDone(GraphResult{});
                }
                continue;
            }
            _postReply(sessionId, chunks[k].html, AlertKind::None, OutboxPriority::History, chunks[k].commitTo,
                       last ? windowDone : std::function<void(const GraphResult&)>{});
        }
    }

    void TeamsService::_liveStep(const SessionInfo& s, const std::wstring& path, int64_t nowMs)
    {
        int64_t from = 0;
        MirrorState st;
        bool verbose = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(s.id);
            if (it == _state.threads.end())
            {
                return;
            }
            const ThreadRuntime& rt = _runtime[s.id];
            from = rt.readOffset >= 0 ? rt.readOffset : it->second.transcriptOffset;
            st = rt.mirror; // a copy: the send thread's answers touch the same runtime under the lock
            verbose = _config.verbose;
        }
        const int64_t size = FileSizeOf(path);
        if (size < 0)
        {
            return;
        }
        if (size < from)
        {
            // The transcript shrank (rewritten by a tool): the mirror goes on from its new end, and nothing is posted twice.
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.threads.find(s.id); it != _state.threads.end())
                {
                    it->second.transcriptOffset = size;
                    _stateDirty = true;
                }
                ThreadRuntime& rt = _runtime[s.id];
                rt.readOffset = size;
                rt.mirror = MirrorState{};
            }
            _log(L"[teams] the transcript of " + s.id.substr(0, 8) + L" shrank: its mirror goes on from the new end");
            return;
        }
        std::vector<MirrorLine> lines;
        int64_t readTo = from;
        bool reachedEnd = true;
        if (from < size)
        {
            int64_t skipTo = -1;
            const std::string bytes = ReadCompleteLines(path, from, size, kLiveReadBytes, skipTo, reachedEnd);
            if (skipTo > 0)
            {
                std::lock_guard lk{ _mtx };
                _runtime[s.id].readOffset = skipTo; // one giant line: skipped (the committed cursor stays before it)
                return;
            }
            size_t p = 0;
            while (p < bytes.size())
            {
                const size_t nl = bytes.find('\n', p);
                if (nl == std::string::npos)
                {
                    break;
                }
                MirrorLine line;
                line.at = from + static_cast<int64_t>(p);
                line.end = from + static_cast<int64_t>(nl) + 1;
                if (nl > p)
                {
                    auto parsed = ParseTranscriptJsonl(bytes.substr(p, nl - p));
                    if (!parsed.empty())
                    {
                        // Live mode: a fork's copies of its source's lines are history, never live news.
                        line.items = Msg::ClassifyTranscriptLine(parsed.front());
                    }
                }
                readTo = line.end;
                if (!line.items.empty())
                {
                    lines.push_back(std::move(line));
                }
                p = nl + 1;
            }
        }
        // A held text is the final reply once the session rests (or is closed) and every complete line was read.
        const bool resting = !s.live || (s.state != SessionState::Running && s.state != SessionState::NeedsApproval);
        const bool turnAtRest = resting && reachedEnd;
        std::vector<MirrorPost> posts;
        if (!lines.empty() || (turnAtRest && st.holding))
        {
            // §7.5: a prompt the owner wrote in the thread is there already (their own reply): the mirror posts no "👤 You" for it.
            const std::wstring sid = s.id;
            const std::function<bool(const std::wstring&)> postedInTeams = [this, sid](const std::wstring& text) { return _consumeTeamsPrompt(sid, text); };
            posts = MirrorLines(st, lines, readTo, turnAtRest, verbose, nowMs - kAlertFreshMs, nowMs, {}, postedInTeams);
        }
        bool needsYou = false;
        for (const MirrorPost& p : posts)
        {
            needsYou = needsYou || p.alert == AlertKind::NeedsYou;
        }
        {
            std::lock_guard lk{ _mtx };
            ThreadRuntime& rt = _runtime[s.id];
            rt.readOffset = readTo;
            rt.mirror = std::move(st);
            if (needsYou)
            {
                rt.lastNeedsYouMs = nowMs; // the approval notice of the same moment stays quiet
            }
        }
        for (const MirrorPost& p : posts)
        {
            _postReply(s.id, p.html, p.alert, p.alert == AlertKind::None ? OutboxPriority::Mirror : OutboxPriority::Alert, p.commitTo);
        }
    }

    void TeamsService::_postReply(const std::wstring& sessionId, const std::wstring& html, AlertKind alert, OutboxPriority priority, int64_t commitTo,
                                  std::function<void(const GraphResult&)> onDone)
    {
        TeamsConfig config;
        Owner owner;
        std::wstring rootId;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(sessionId);
            if (it == _state.threads.end())
            {
                return;
            }
            rootId = it->second.rootId;
            config = _config;
            owner = _owner;
        }
        std::vector<std::wstring> parts;
        if (Utf8Length(html) > kTeamsPostMaxBytes)
        {
            parts = SplitTeamsHtml(html);
        }
        else
        {
            parts.push_back(html);
        }
        for (size_t k = 0; k < parts.size(); ++k)
        {
            const bool last = k + 1 == parts.size();
            OutboxItem item;
            item.priority = priority;
            item.op = GraphOp::Post;
            item.method = L"POST";
            item.path = RepliesPath(config.teamId, config.channelId, rootId);
            // The alert rides the last part: the owner is called once the whole reply is there.
            item.body = BuildPostJson(parts[k], L"", last ? alert : AlertKind::None, owner);
            item.what = L"reply in " + sessionId.substr(0, 8);
            item.lane = sessionId;
            const int64_t commit = last ? commitTo : 0;
            std::function<void(const GraphResult&)> after = last ? onDone : std::function<void(const GraphResult&)>{};
            // Known as Agentmaster's before its answer journals it: the poll may read the reply first (§7.2).
            _rememberOwnPost(rootId, parts[k]);
            item.done = [this, sessionId, commit, after](const GraphResult& r) {
                _journalPost(r);
                // A final answer moves the cursor — a write given up too, which would only fail again after a restart; a refusal
                // of the client's own (not a Graph address) does not.
                if (commit > 0 && !r.refused)
                {
                    _commit(sessionId, commit);
                }
                if (after)
                {
                    after(r);
                }
            };
            _enqueue(std::move(item));
        }
    }

    void TeamsService::_commit(const std::wstring& sessionId, int64_t commitTo) noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(sessionId);
            if (it != _state.threads.end() && commitTo > it->second.transcriptOffset)
            {
                it->second.transcriptOffset = commitTo;
                _stateDirty = true;
            }
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_commit");
        }
    }

    void TeamsService::_journalPost(const GraphResult& r) noexcept
    {
        try
        {
            if (!r.Ok())
            {
                return;
            }
            const std::wstring id = r.body.StrAt(L"id");
            if (id.empty())
            {
                return;
            }
            std::lock_guard lk{ _mtx };
            _state.ownPosts[id] = TeamsNowMs();
            _stateDirty = true;
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_journalPost");
        }
    }

    void TeamsService::_rememberOwnPost(const std::wstring& rootId, const std::wstring& html) noexcept
    {
        try
        {
            const uint64_t fingerprint = PostFingerprint(PlainTextFromTeamsHtml(html));
            if (fingerprint == 0)
            {
                return;
            }
            const int64_t now = TeamsNowMs();
            std::lock_guard lk{ _mtx };
            auto& list = _ownFingerprints[rootId];
            list.erase(std::remove_if(list.begin(), list.end(), [now](const auto& e) { return now - e.second > kOwnPostFingerprintMs; }), list.end());
            list.emplace_back(fingerprint, now);
        }
        catch (...)
        {
            // Rule #18. Recovery: this post is known only by its journal entry, once its answer comes.
            LogSwallowedException(L"TeamsService::_rememberOwnPost");
        }
    }

    bool TeamsService::_isRecentOwnPost(const std::wstring& rootId, const InboundMessage& m) noexcept
    {
        try
        {
            const uint64_t fingerprint = PostFingerprint(m.text);
            if (fingerprint == 0)
            {
                return false;
            }
            std::lock_guard lk{ _mtx };
            const auto it = _ownFingerprints.find(rootId);
            if (it == _ownFingerprints.end())
            {
                return false;
            }
            for (const auto& entry : it->second)
            {
                if (entry.first == fingerprint)
                {
                    // Agentmaster's own post, read before its answer journaled it: journaled now, never read as the owner's.
                    _state.ownPosts[m.id] = TeamsNowMs();
                    _stateDirty = true;
                    return true;
                }
            }
            return false;
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_isRecentOwnPost");
            return false;
        }
    }

    // ===== inbound: the poll (§7) ============================================================================================

    void TeamsService::_pollLoop() noexcept
    {
        int64_t nextAt = 0;
        for (;;)
        {
            try
            {
                {
                    std::unique_lock lk{ _mtx };
                    // Waits for the next poll's time, for the tick's state load at a start, or for the stop.
                    while (!_stopping && (!_stateLoaded || TeamsNowMs() < nextAt))
                    {
                        const int64_t wait = _stateLoaded ? (std::min)(int64_t{ 1000 }, nextAt - TeamsNowMs()) : 200;
                        _cv.wait_for(lk, std::chrono::milliseconds(wait > 0 ? wait : 1));
                    }
                    if (_stopping)
                    {
                        return;
                    }
                }
                int retryAfterSec = 0;
                const PollOutcome outcome = _pollOnce(retryAfterSec);
                PollFacts facts;
                {
                    std::lock_guard lk{ _mtx };
                    facts.nowMs = TeamsNowMs();
                    facts.lastOwnerActionMs = _lastOwnerActionMs;
                    facts.ownerAwaited = _ownerAwaited;
                    facts.lastChangeMs = _lastChangeMs;
                }
                int64_t delay = NextPollDelayMs(facts);
                if (outcome == PollOutcome::NoReader)
                {
                    delay = 30'000; // no token can read (a mirror only): looked at again in 30 s
                }
                else if (retryAfterSec > 0)
                {
                    delay = (std::max)(delay * 2, static_cast<int64_t>(retryAfterSec) * 1000); // §7.3: a 429 doubles the next delay once
                }
                else if (outcome == PollOutcome::Failed)
                {
                    delay = (std::max)(delay, int64_t{ 30'000 });
                }
                nextAt = TeamsNowMs() + delay;
            }
            catch (...)
            {
                // Rule #18; recovery: this poll is lost, the next one comes in 30 s.
                LogSwallowedException(L"TeamsService::_pollLoop");
                nextAt = TeamsNowMs() + 30'000;
            }
        }
    }

    bool TeamsService::_readGet(GraphClient& http, const std::wstring& pathOrUrl, std::wstring_view what, GraphResult& result)
    {
        bool useCli = false;
        int64_t waitMs = 0;
        {
            std::lock_guard lk{ _mtx };
            useCli = _config.useAzureCli;
            waitMs = _lastReadMs + kMinReadGapMs - TeamsNowMs();
        }
        if (waitMs > 0)
        {
            // Graph allows one read a second per channel: reads keep their gap (a stop ends the wait).
            std::unique_lock lk{ _mtx };
            _cv.wait_for(lk, std::chrono::milliseconds(waitMs), [this] { return _stopping; });
            if (_stopping)
            {
                result = GraphResult{};
                result.transportFailed = true;
                result.aborted = true;
                return true;
            }
        }
        const SourceTokens tokens = _sourceTokens(http, useCli);
        const std::optional<size_t> source = PickTokenFor(GraphOp::Read, tokens.views, _refusedNow(TeamsNowMs()));
        if (!source)
        {
            return false;
        }
        const AccessToken& token = *source == kAppSource ? *tokens.app : *tokens.cli;
        result = http.Call(L"GET", pathOrUrl, token.token, nullptr, what);
        {
            std::lock_guard lk{ _mtx };
            _lastReadMs = TeamsNowMs();
            if (result.httpStatus == 403)
            {
                _refused.emplace_back(RefusedPair{ *source, GraphOp::Read }, TeamsNowMs() + kRefusedForMs);
            }
        }
        if (result.httpStatus == 401)
        {
            // The token was refused: that source renews it before the next read.
            if (*source == kAppSource)
            {
                _app.Forget();
            }
            else
            {
                _cli->Forget();
            }
        }
        return true;
    }

    TeamsService::PollOutcome TeamsService::_pollOnce(int& retryAfterSec)
    {
        retryAfterSec = 0;
        std::shared_ptr<GraphClient> http;
        TeamsConfig config;
        std::wstring link;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            config = _config;
            link = _state.deltaLink;
        }
        if (!http)
        {
            return PollOutcome::NoReader;
        }
        if (link.empty())
        {
            link = ChannelMessagesPath(config.teamId, config.channelId) + L"/delta"; // the first sync, from the channel's start
        }
        bool changed = false;
        for (int page = 0; page < kMaxDeltaPagesPerPoll; ++page)
        {
            GraphResult r;
            if (!_readGet(*http, link, L"channel poll", r))
            {
                return PollOutcome::NoReader;
            }
            if (r.aborted)
            {
                return PollOutcome::Failed;
            }
            if (!r.Ok())
            {
                if (r.httpStatus == 429)
                {
                    retryAfterSec = r.retryAfterSec > 0 ? r.retryAfterSec : 5;
                    return PollOutcome::Failed;
                }
                if (r.httpStatus == 400 || r.httpStatus == 404 || r.httpStatus == 410)
                {
                    // A link that is no longer good (an expired delta token, a reset sync): the next poll syncs from the
                    // channel's start. Its old posts are history, not requests: older than the run, or already handled by it
                    // (_onChangedRoot).
                    {
                        std::lock_guard lk{ _mtx };
                        if (!_state.deltaLink.empty())
                        {
                            _state.deltaLink.clear();
                            _stateDirty = true;
                        }
                    }
                    _log(L"[teams] the channel's delta link is no longer good (" + Describe(r) + L"): the next poll reads the channel again");
                }
                _noteProblem(L"the channel could not be read (" + Describe(r) + L")");
                return PollOutcome::Failed;
            }
            const DeltaPage p = ParseDeltaPage(r.body);
            for (const InboundMessage& root : p.roots)
            {
                changed = true;
                try
                {
                    _onChangedRoot(root);
                }
                catch (...)
                {
                    // Rule #18; recovery: this root is skipped; its thread is read again when it changes next.
                    LogSwallowedException(L"TeamsService::_pollOnce (a root)");
                }
            }
            // The link is saved after every page: a long first sync resumes from it (a page's next link resumes the sync too).
            const std::wstring next = p.nextLink.empty() ? p.deltaLink : p.nextLink;
            if (next.empty())
            {
                _noteProblem(L"the channel's answer had no link to go on");
                return PollOutcome::Failed;
            }
            {
                std::lock_guard lk{ _mtx };
                _state.deltaLink = next;
                _stateDirty = true;
                if (changed)
                {
                    _lastChangeMs = TeamsNowMs();
                }
                if (_problem.rfind(L"the channel", 0) == 0)
                {
                    _problem.clear(); // the channel reads again
                }
            }
            if (p.nextLink.empty())
            {
                return changed ? PollOutcome::Changed : PollOutcome::Unchanged;
            }
            link = p.nextLink;
        }
        return changed ? PollOutcome::Changed : PollOutcome::Unchanged; // more pages: the next poll goes on from the saved link
    }

    void TeamsService::_onChangedRoot(const InboundMessage& root)
    {
        std::wstring sessionId;
        bool legend = false;
        bool search = false;
        std::wstring ownerId;
        std::wstring markers;
        int64_t started = 0;
        {
            std::lock_guard lk{ _mtx };
            legend = !_state.legendId.empty() && root.id == _state.legendId;
            search = !_state.searchId.empty() && root.id == _state.searchId;
            for (const auto& entry : _state.threads)
            {
                if (entry.second.rootId == root.id)
                {
                    sessionId = entry.first;
                    break;
                }
            }
            ownerId = _owner.id;
            markers = _config.foreignMarkers;
            started = _startedMs;
        }
        if (legend)
        {
            _legendReactions(root);
            return;
        }
        if (search)
        {
            _searchStep(root); // §9.3: the owner's requests, picks and /clear
            return;
        }
        if (!sessionId.empty())
        {
            // A changed thread: the card's emoji, the watched replies' emoji, then the owner's new replies (§7.2).
            _rootReactions(sessionId, root);
            _watchedStep(sessionId, root.id);
            _readThreadReplies(sessionId, root.id);
            return;
        }
        if (_isRecentOwnPost(L"", root))
        {
            return; // Agentmaster's own root (a card, the legend), read before its answer journaled it
        }
        bool wizard = false;
        NewRootVerdict verdict = NewRootVerdict::Ignore;
        {
            std::lock_guard lk{ _mtx };
            // A post whose new session is under way: its step's emoji or a typed folder decides (§9.1).
            wizard = _wizardRoots.count(root.id) > 0 ||
                     std::any_of(_state.watched.begin(), _state.watched.end(), [&root](const Watched& w) { return w.kind == MessageKind::Step && w.rootId == root.id; });
            if (!wizard)
            {
                verdict = ClassifyNewRoot(root, ownerId, _state.ownPosts, markers);
            }
        }
        if (wizard)
        {
            _wizardStep(root);
            return;
        }
        // A post written before this run is history at EVERY poll: the delta returns an old post again when a reply or a reaction
        // is added to it, which does not make it new (a first sync, or the re-sync after a lost link, returns them all).
        if (verdict != NewRootVerdict::NewSession || root.createdMs < started)
        {
            return; // another tool's post, a system message, or a post older than this run
        }
        _startWizard(root); // D-C9: every new post of the owner opens a session, its words the first prompt
    }

    void TeamsService::_readThreadReplies(const std::wstring& sessionId, const std::wstring& rootId)
    {
        std::shared_ptr<GraphClient> http;
        TeamsConfig config;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            config = _config;
        }
        if (!http)
        {
            return;
        }
        GraphResult r;
        const std::wstring path = RepliesPath(config.teamId, config.channelId, rootId) + L"?$top=" + std::to_wstring(kRepliesPerRead);
        if (!_readGet(*http, path, L"replies of " + sessionId.substr(0, 8), r) || !r.Ok())
        {
            return;
        }
        std::vector<InboundMessage> fresh;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(sessionId);
            if (it == _state.threads.end())
            {
                return;
            }
            fresh = NewOwnerReplies(r.body, _owner.id, _state.ownPosts, it->second.lastReplyMs);
            if (!fresh.empty())
            {
                it->second.lastReplyMs = fresh.back().createdMs;
                _stateDirty = true;
            }
        }
        for (const InboundMessage& m : fresh)
        {
            if (_isRecentOwnPost(rootId, m))
            {
                continue; // Agentmaster's own reply, read before its answer journaled it: never a prompt
            }
            {
                std::lock_guard lk{ _mtx };
                _lastOwnerActionMs = TeamsNowMs(); // §7.3: the owner acts — the poll runs every 5 s for 2 minutes
            }
            try
            {
                _handleOwnerReply(sessionId, rootId, m);
            }
            catch (...)
            {
                // Rule #18; recovery: this reply is not acted on (it is marked read); the owner can write it again.
                LogSwallowedException(L"TeamsService::_readThreadReplies (a reply)");
            }
        }
    }

    // ===== inbound: what the owner's reply does (§7.4, §7.5) =================================================================

    void TeamsService::_handleOwnerReply(const std::wstring& sessionId, const std::wstring& rootId, const InboundMessage& m)
    {
        const std::wstring text = detail::Trim(m.text);
        if (text.empty())
        {
            return; // a picture or a file without words: nothing to send
        }
        int64_t started = 0;
        {
            std::lock_guard lk{ _mtx };
            started = _startedMs;
        }
        if (m.createdMs > 0 && m.createdMs < started)
        {
            _offerBacklog(sessionId, rootId, m, text); // §7.4: written while Agentmaster was closed — offered, never run
            return;
        }
        const Msg::RemoteCommand cmd = Msg::ParseRemoteCommand(text);
        switch (cmd.kind)
        {
        case Msg::RemoteKind::Text:
        case Msg::RemoteKind::ClaudeCommand:
            _queueTeamsPrompt(sessionId, rootId, m.id, cmd.text.empty() ? text : cmd.text);
            return;
        case Msg::RemoteKind::RefusedNoRecord:
        case Msg::RemoteKind::RefusedLeavesTab:
            _answer(sessionId, RefusedCommandHtml(cmd));
            return;
        default:
            break; // one of Agentmaster's commands
        }
        if (cmd.name == L"steer")
        {
            _steerTeams(sessionId, rootId, m.id, cmd.args);
        }
        else if (cmd.name == L"enter")
        {
            _enterTeams(sessionId, cmd.args);
        }
        else if (cmd.name == L"interrupt")
        {
            _escTeams(sessionId);
        }
        else if (cmd.name == L"deactivate")
        {
            _parkTeams(sessionId, false);
        }
        else if (cmd.name == L"status")
        {
            _statusTeams(sessionId);
        }
        else if (cmd.name == L"screen" || cmd.name == L"screenimage")
        {
            _screenTeams(sessionId);
        }
        else if (cmd.name == L"allscreen")
        {
            _allScreenTeams(sessionId, cmd.args);
        }
        else if (cmd.name == L"forksession")
        {
            _forkTeams(sessionId, detail::Trim(cmd.args)); // its argument is a per-launch model, as Telegram's /forksession
        }
        else if (cmd.name == L"newsession")
        {
            _newSessionTeams(sessionId, detail::Trim(cmd.args)); // a new session in this session's folder (Telegram's, in a topic)
        }
        else if (cmd.name == L"help" || cmd.name == L"start")
        {
            _answer(sessionId, TeamsCommandsHelpHtml());
        }
        else
        {
            _answer(sessionId, L"<code>/" + Msg::TgEscapeHtml(cmd.name) + L"</code> is not available in Teams yet.<br>" + TeamsCommandsHelpHtml());
        }
    }

    void TeamsService::_queueTeamsPrompt(const std::wstring& sessionId, const std::wstring& rootId, const std::wstring& replyId, const std::wstring& text)
    {
        const auto s = _registry->Get(sessionId);
        if (!s || !s->live)
        {
            _answer(sessionId, L"This session is closed: resume it at the PC.");
            return;
        }
        // §21.34's rule: a BARE /add-dir opens a path input at the PC that nobody here can fill — the usage line instead.
        std::wstring addDirArgs;
        if (Msg::ParseAddDirCommandText(text, addDirArgs) && addDirArgs.empty())
        {
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::AddDirUsageHtml()));
            return;
        }
        // Telegram's remote prompt (Messaging.Inbound.cpp _queueRemotePrompt), field by field: the Autorun origin every
        // delivery-evidence rule keys on, the question guard's pass (the owner's words answer an open question), the source.
        QueuedPrompt p;
        p.id = NewSessionId();
        p.text = text;
        p.label = Msg::TgFirstLine(text, 56);
        p.status = PromptStatus::Pending;
        p.gate = PromptGate::OnTurnComplete;
        p.origin = PromptOrigin::Autorun;
        p.guardPattern = std::wstring{ kAnswersQuestionOk };
        p.remoteSource = L"teams:" + (replyId.empty() ? rootId : replyId); // §9.1: a new post's words came from the root itself
        Msg::RemoteWait waits = Msg::RemoteWait::None;
        const bool inserted = _registry->Update(sessionId, [&](SessionInfo& ss) {
            // D30: next in line — before the runner's own planned prompts, after earlier remote ones.
            auto at = ss.queue.end();
            for (auto it = ss.queue.begin(); it != ss.queue.end(); ++it)
            {
                if (it->status == PromptStatus::Pending && it->remoteSource.empty())
                {
                    at = it;
                    break;
                }
            }
            waits = Msg::RemotePromptWait(ss, static_cast<size_t>(std::distance(ss.queue.begin(), at)));
            at = ss.queue.insert(at, p);
            if (waits != Msg::RemoteWait::None)
            {
                at->remoteQueuedBusyUnixMs = TeamsNowMs();
            }
        });
        if (!inserted)
        {
            _answer(sessionId, L"This session is closed: resume it at the PC.");
            return;
        }
        const int64_t now = TeamsNowMs();
        {
            std::lock_guard lk{ _mtx };
            _teamsPrompts[sessionId].emplace_back(NormalizePromptTextForCompare(text), now);
            // §8: ❌ on the owner's reply takes the prompt out of the queue while it waits; the watch ends when it is sent. Words
            // that are the root itself (§9.1) get no watch: the root is the card, whose ❌ means nothing.
            if (!replyId.empty())
            {
                Watched w;
                w.messageId = replyId;
                w.rootId = rootId;
                w.kind = MessageKind::QueuedPrompt;
                w.sessionId = sessionId;
                w.payload = p.id;
                w.expiresMs = now + kPromptTrackMaxMs;
                _state.watched.push_back(std::move(w));
                _stateDirty = true;
            }
        }
        if (!replyId.empty())
        {
            // The status emoji go on the owner's reply. Never on the root (§9.1): the card's 👌 would stay there for good (§8).
            PromptTrack track;
            track.sessionId = sessionId;
            track.rootId = rootId;
            track.replyId = replyId;
            track.promptId = p.id;
            track.sinceMs = now;
            _trackPrompt(std::move(track), kEmojiSeen);
        }
        LogNav(L"remote teams queue " + ShortId(sessionId) + L" chars=" + std::to_wstring(text.size()));
        _log(L"[teams] " + ShortId(sessionId) + L" queued from Teams (" + std::to_wstring(text.size()) + L" chars" +
             (waits != Msg::RemoteWait::None ? std::wstring{ L"; waits: " } + Msg::RemoteWaitWords(waits) : std::wstring{}) + L")");
        // A dormant tab (restored, parked) is started so the queue can deliver (Telegram's §7.1).
        if (!s->started && !s->external && _hooks.sessionAction)
        {
            try
            {
                _hooks.sessionAction(sessionId, L"start", L"", nullptr);
            }
            catch (...)
            {
                // Rule #18. Recovery: the prompt waits in the queue; it goes when the tab is entered or activated.
                LogSwallowedException(L"TeamsService::_queueTeamsPrompt (start)");
            }
        }
        if (_scheduler)
        {
            _scheduler->RequestAdvance(sessionId);
        }
    }

    void TeamsService::_steerTeams(const std::wstring& sessionId, const std::wstring& rootId, const std::wstring& replyId, const std::wstring& words)
    {
        const auto s = _registry->Get(sessionId);
        // A session the registry no longer knows reads as closed: the same answer as a closed one.
        const Msg::SteerAction action = s ? Msg::DecideSteer(words, s->live, s->started, s->external, s->state, s->pendingBoxState) :
                                            Msg::DecideSteer(words, false, false, false, SessionState::Idle, InputBoxState::Unknown);
        if (action == Msg::SteerAction::Usage || action == Msg::SteerAction::Closed)
        {
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::SteerReplyHtml(action)));
            return;
        }
        std::wstring addDirArgs;
        if (Msg::ParseAddDirCommandText(words, addDirArgs) && addDirArgs.empty())
        {
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::AddDirUsageHtml()));
            return;
        }
        const bool sendNow = action == Msg::SteerAction::Send;
        const int64_t now = TeamsNowMs();
        QueuedPrompt p = Msg::BuildSteerPrompt(NewSessionId(), words, 0, sendNow, now);
        p.remoteSource = L"teams:" + replyId;
        const bool inserted = _registry->Update(sessionId, [&](SessionInfo& ss) {
            ss.queue.insert(ss.queue.begin() + static_cast<std::ptrdiff_t>(Msg::SteerInsertIndex(ss.queue)), p);
        });
        if (!inserted)
        {
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::SteerReplyHtml(Msg::SteerAction::Closed)));
            return;
        }
        {
            std::lock_guard lk{ _mtx };
            _teamsPrompts[sessionId].emplace_back(NormalizePromptTextForCompare(words), now);
        }
        PromptTrack track;
        track.sessionId = sessionId;
        track.rootId = rootId;
        track.replyId = replyId;
        track.promptId = p.id;
        track.sinceMs = now;
        _trackPrompt(std::move(track), kEmojiSeen);
        if (sendNow)
        {
            PromptSubmission submission;
            submission.sessionId = sessionId;
            submission.promptId = p.id;
            submission.text = p.text;
            if (!_registry->SubmitPrompt(submission))
            {
                // Declined at once (another delivery owns the input box): the words wait FIRST in the queue instead.
                _registry->RollbackPromptToPending(sessionId, p.id, false);
                _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::SteerDeclinedNoticeHtml(Msg::SteerDecline::BoxHeld)));
            }
            LogNav(L"remote teams /steer " + ShortId(sessionId) + L" chars=" + std::to_wstring(words.size()) + L" -> sent");
        }
        else
        {
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::SteerReplyHtml(action)));
            if (action == Msg::SteerAction::QueueDormant && _hooks.sessionAction)
            {
                try
                {
                    _hooks.sessionAction(sessionId, L"start", L"", nullptr);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the words stay first in the queue; they go when the tab is entered or activated.
                    LogSwallowedException(L"TeamsService::_steerTeams (start)");
                }
            }
            LogNav(L"remote teams /steer " + ShortId(sessionId) + L" chars=" + std::to_wstring(words.size()) + L" -> queued first");
        }
        if (_scheduler)
        {
            _scheduler->RequestAdvance(sessionId);
        }
    }

    void TeamsService::_escTeams(const std::wstring& sessionId)
    {
        if (!_hooks.sessionAction)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        const int64_t now = TeamsNowMs();
        bool tooSoon = false;
        {
            // Two Esc in a row open Claude Code's Rewind menu: one per 3 s for a session; a refused one does not move the clock.
            std::lock_guard lk{ _mtx };
            int64_t& last = _lastEscMs[sessionId];
            tooSoon = now - last < kEscGapMs;
            if (!tooSoon)
            {
                last = now;
            }
        }
        if (tooSoon)
        {
            _answer(sessionId, L"An Esc went moments ago: ask again in a few seconds.");
            return;
        }
        const auto s = _registry->GetWithoutQueue(sessionId);
        const SessionState stateAtAsk = s ? s->state : SessionState::Idle;
        const std::weak_ptr<TeamsService> self = weak_from_this();
        // No window answered (none hosts the tab): the owner is told, never left without a line.
        const auto once = std::make_shared<AnswerOnce>([self, sessionId]() {
            if (const auto me = self.lock())
            {
                me->_answer(sessionId, Msg::TgEscapeHtml(std::wstring{ Msg::kRemoteEscUnansweredText }));
            }
        });
        LogNav(L"remote teams esc " + ShortId(sessionId) + L" -> asked the hosting window");
        _hooks.sessionAction(sessionId, std::wstring{ Msg::kPressEscAction }, std::wstring{}, [self, sessionId, stateAtAsk, once](std::wstring word) {
            // The hosting window's UI thread: the claim first, so the guard stays silent once a window answered.
            if (!once->Claim())
            {
                return;
            }
            if (const auto me = self.lock())
            {
                me->_answer(sessionId, Msg::TgEscapeHtml(Msg::RemoteEscOutcomeText(Msg::ParseRemoteEscResult(word), stateAtAsk)));
            }
        });
    }

    void TeamsService::_enterTeams(const std::wstring& sessionId, const std::wstring& args)
    {
        if (!args.empty())
        {
            // "/enter fix it" would press Enter on whatever the box holds while the words went nowhere: the usage line instead.
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::RemoteEnterUsageHtml()));
            return;
        }
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || !s->live)
        {
            _answer(sessionId, Msg::TgEscapeHtml(std::wstring{ Msg::kRemoteEnterClosedText }));
            return;
        }
        // The record's own checks first (a parked tab, an exited claude, Codex): answered at once, with no window round trip.
        if (const Msg::RemoteEnterResult early = Msg::DecideRemoteEnterTarget(*s); early != Msg::RemoteEnterResult::Pressed)
        {
            _answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::RemoteEnterOutcomeHtml(Msg::RemoteEnterAnswer{ early, {} }, s->state)));
            return;
        }
        if (!_hooks.sessionAction)
        {
            _answer(sessionId, L"Not available here."); // a blind Enter is exactly what this command must never write
            return;
        }
        const SessionState stateAtAsk = s->state;
        const std::weak_ptr<TeamsService> self = weak_from_this();
        const auto once = std::make_shared<AnswerOnce>([self, sessionId]() {
            if (const auto me = self.lock())
            {
                me->_answer(sessionId, Msg::TgEscapeHtml(std::wstring{ Msg::kRemoteEnterUnansweredText }));
            }
        });
        LogNav(L"remote teams /enter " + ShortId(sessionId) + L" -> asked the hosting window");
        _hooks.sessionAction(sessionId, std::wstring{ Msg::kPressEnterAction }, std::wstring{}, [self, sessionId, stateAtAsk, once](std::wstring reply) {
            if (!once->Claim())
            {
                return;
            }
            if (const auto me = self.lock())
            {
                me->_answer(sessionId, TeamsHtmlFromTelegramHtml(Msg::RemoteEnterOutcomeHtml(Msg::ParseRemoteEnterReply(reply), stateAtAsk)));
            }
        });
    }

    void TeamsService::_parkTeams(const std::wstring& sessionId, bool cutOffTurn)
    {
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || !s->live)
        {
            _answer(sessionId, L"This session is closed.");
            return;
        }
        if (!cutOffTurn && Msg::SessionTurnInFlight(*s))
        {
            // Parking cuts a running turn off: the owner decides (M5's confirm on the card's 🌙).
            _answer(sessionId, L"A turn is running: parking the tab now cuts it off. React " + std::wstring{ kEmojiPark } +
                                   L" on the card to park it anyway (you are asked to confirm).");
            return;
        }
        if (!_hooks.sessionAction)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        const std::weak_ptr<TeamsService> self = weak_from_this();
        const auto once = std::make_shared<AnswerOnce>([self, sessionId]() {
            if (const auto me = self.lock())
            {
                me->_answer(sessionId, Msg::TgEscapeHtml(std::wstring{ Msg::kRemoteParkUnansweredText }));
            }
        });
        LogNav(L"remote teams deactivate " + ShortId(sessionId) + L" -> asked the hosting window" + (cutOffTurn ? L" (a turn may be cut off)" : L""));
        // ⚠ kDeactivateCutOffTurnArg only after the owner's confirm: the window reads any other argument as "at rest only".
        _hooks.sessionAction(sessionId, std::wstring{ Msg::kDeactivateAction }, cutOffTurn ? std::wstring{ Msg::kDeactivateCutOffTurnArg } : std::wstring{},
                             [self, sessionId, once](std::wstring word) {
                                 if (!once->Claim())
                                 {
                                     return;
                                 }
                                 if (const auto me = self.lock())
                                 {
                                     me->_answer(sessionId, Msg::TgEscapeHtml(Msg::RemoteParkOutcomeText(Msg::ParseRemoteParkResult(word))));
                                 }
                             });
    }

    void TeamsService::_offerBacklog(const std::wstring& sessionId, const std::wstring& rootId, const InboundMessage& m, const std::wstring& text)
    {
        const std::wstring html = BacklogOfferHtml(text, SpanWord(TeamsNowMs() - m.createdMs));
        // The offer is watched once it is posted (its id comes with the answer): M5 acts on the owner's ✅ or ❌ on it.
        _postReply(sessionId, html, AlertKind::None, OutboxPriority::Answer, 0, [this, sessionId, rootId, text](const GraphResult& r) {
            try
            {
                if (!r.Ok())
                {
                    return;
                }
                const std::wstring id = r.body.StrAt(L"id");
                if (id.empty())
                {
                    return;
                }
                Watched w;
                w.messageId = id;
                w.rootId = rootId;
                w.kind = MessageKind::Backlog;
                w.sessionId = sessionId;
                w.payload = text;
                w.expiresMs = TeamsNowMs() + kBacklogOfferMs;
                std::lock_guard lk{ _mtx };
                _state.watched.push_back(std::move(w));
                _stateDirty = true;
            }
            catch (...)
            {
                // Rule #18. Recovery: the offer is posted but not watched: the owner writes the message again.
                LogSwallowedException(L"TeamsService::_offerBacklog (watch)");
            }
        });
        _log(L"[teams] " + ShortId(sessionId) + L" a reply written while Agentmaster was closed is offered, not sent");
    }

    void TeamsService::_answer(const std::wstring& sessionId, const std::wstring& html)
    {
        _postReply(sessionId, html, AlertKind::None, OutboxPriority::Answer, 0);
    }

    void TeamsService::_react(const std::wstring& rootId, const std::wstring& replyId, std::wstring_view emoji, bool set)
    {
        TeamsConfig config;
        {
            std::lock_guard lk{ _mtx };
            if (_reactsBy.empty())
            {
                return; // §4.2: no source can react — the statuses are not shown (the status line names the degradation)
            }
            config = _config;
            if (set)
            {
                // Journaled when queued: the poll may read the reaction before the call's answer, and must never take it for the
                // owner's action (§7.2).
                _state.ownReactions.push_back(OwnReaction{ replyId.empty() ? rootId : replyId, NormalizeEmoji(emoji), TeamsNowMs() });
                _stateDirty = true;
            }
        }
        OutboxItem item;
        item.priority = OutboxPriority::Answer;
        item.op = GraphOp::React;
        item.method = L"POST";
        item.path = MessagePath(config.teamId, config.channelId, rootId, replyId) + (set ? L"/setReaction" : L"/unsetReaction");
        json::Value body = json::Value::MkObj();
        body.Set(L"reactionType", json::Value::MkStr(std::wstring{ emoji }));
        item.body = std::move(body);
        item.what = set ? L"reaction" : L"reaction removal";
        _enqueue(std::move(item));
    }

    void TeamsService::_trackPrompt(PromptTrack track, std::wstring_view emoji)
    {
        track.shown = std::wstring{ emoji };
        const std::wstring rootId = track.rootId;
        const std::wstring replyId = track.replyId;
        {
            std::lock_guard lk{ _mtx };
            _tracks.push_back(std::move(track));
        }
        _react(rootId, replyId, emoji, true);
    }

    void TeamsService::_trackStep(int64_t nowMs) noexcept
    {
        try
        {
            std::vector<PromptTrack> tracks;
            {
                std::lock_guard lk{ _mtx };
                tracks = _tracks;
            }
            if (tracks.empty() || !_registry)
            {
                return;
            }
            // One full read per session (the queue's sent prompts are needed), whatever the number of its prompts.
            std::map<std::wstring, std::optional<SessionInfo>> sessions;
            std::vector<std::wstring> ended; // the follows that end (by prompt id)
            std::vector<std::wstring> sent; // the prompts that left the queue's waiting (their ❌ watch ends)
            std::vector<std::pair<std::wstring, std::wstring>> shown; // (prompt id, emoji now shown)
            for (const PromptTrack& t : tracks)
            {
                if (nowMs - t.sinceMs > kPromptTrackMaxMs)
                {
                    ended.push_back(t.promptId);
                    continue;
                }
                auto it = sessions.find(t.sessionId);
                if (it == sessions.end())
                {
                    it = sessions.emplace(t.sessionId, _registry->Get(t.sessionId)).first;
                }
                const std::wstring want = it->second ? PromptReactionFor(*it->second, t.promptId) : std::wstring{};
                if (want.empty())
                {
                    ended.push_back(t.promptId); // the queue no longer holds it (cleared, or the session is gone)
                    continue;
                }
                if (!SameEmoji(want, t.shown))
                {
                    // One status at a time on the owner's reply: the old one goes, the new one comes.
                    if (!t.shown.empty())
                    {
                        _react(t.rootId, t.replyId, t.shown, false);
                    }
                    _react(t.rootId, t.replyId, want, true);
                    shown.emplace_back(t.promptId, want);
                }
                if (IsFinalPromptReaction(want))
                {
                    ended.push_back(t.promptId);
                }
                if (!SameEmoji(want, kEmojiSeen))
                {
                    sent.push_back(t.promptId); // no longer waiting: its ❌ watch has nothing left to take out
                }
            }
            // Merged back by prompt id: the poll thread may have added follows meanwhile.
            std::lock_guard lk{ _mtx };
            if (!sent.empty() || !ended.empty())
            {
                const auto unwatched = [&](const Watched& w) {
                    return w.kind == MessageKind::QueuedPrompt && (std::find(sent.begin(), sent.end(), w.payload) != sent.end() ||
                                                                   std::find(ended.begin(), ended.end(), w.payload) != ended.end());
                };
                const size_t before = _state.watched.size();
                _state.watched.erase(std::remove_if(_state.watched.begin(), _state.watched.end(), unwatched), _state.watched.end());
                _stateDirty = _stateDirty || before != _state.watched.size();
            }
            for (const auto& [promptId, emoji] : shown)
            {
                for (PromptTrack& t : _tracks)
                {
                    if (t.promptId == promptId)
                    {
                        t.shown = emoji;
                    }
                }
            }
            _tracks.erase(std::remove_if(_tracks.begin(), _tracks.end(),
                                         [&ended](const PromptTrack& t) { return std::find(ended.begin(), ended.end(), t.promptId) != ended.end(); }),
                          _tracks.end());
        }
        catch (...)
        {
            // Rule #18. Recovery: the statuses stay as they are this pass; the next pass compares again.
            LogSwallowedException(L"TeamsService::_trackStep");
        }
    }

    bool TeamsService::_consumeTeamsPrompt(const std::wstring& sessionId, const std::wstring& text) noexcept
    {
        try
        {
            const std::wstring compare = NormalizePromptTextForCompare(text);
            const int64_t now = TeamsNowMs();
            std::lock_guard lk{ _mtx };
            const auto it = _teamsPrompts.find(sessionId);
            if (it == _teamsPrompts.end())
            {
                return false;
            }
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(), [now](const auto& e) { return now - e.second > kPromptTrackMaxMs; }), list.end());
            for (auto e = list.begin(); e != list.end(); ++e)
            {
                if (e->first == compare)
                {
                    list.erase(e); // consumed: the same words typed again at the PC are posted
                    return true;
                }
            }
            return false;
        }
        catch (...)
        {
            // Rule #18. Recovery: the prompt is posted (a duplicate of the owner's reply at worst).
            LogSwallowedException(L"TeamsService::_consumeTeamsPrompt");
            return false;
        }
    }

    // ===== emoji actions (§8) ================================================================================================

    void TeamsService::_rootReactions(const std::wstring& sessionId, const InboundMessage& root)
    {
        std::vector<Reaction> acts;
        int64_t started = 0;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(sessionId);
            if (it == _state.threads.end())
            {
                return;
            }
            // Only what the OWNER added since the last read, minus Agentmaster's own (the journal) and the status emoji.
            const ReactionDiff diff = DiffReactions(it->second.rootReactions, root.reactions);
            acts = OwnerReactionsToAct(root.id, diff, _owner.id, _state.ownReactions);
            it->second.rootReactions = root.reactions;
            _stateDirty = true;
            started = _startedMs;
            if (!acts.empty())
            {
                _lastOwnerActionMs = TeamsNowMs(); // §7.3: the owner acts — the poll runs every 5 s for 2 minutes
            }
        }
        for (const Reaction& r : acts)
        {
            try
            {
                if (r.atMs > 0 && r.atMs < started)
                {
                    // Added while Agentmaster was closed: an action of an old moment is not run (§7.1's rule for the first
                    // read). The emoji is removed, so the card is free for a press now.
                    _react(root.id, L"", r.emoji, false);
                    _answer(sessionId, L"<i>An emoji added while Agentmaster was closed was not acted on: add it again.</i>");
                    continue;
                }
                _cardAction(sessionId, root.id, r.emoji);
            }
            catch (...)
            {
                // Rule #18; recovery: this emoji is not acted on (it stays on the card; the owner can press again).
                LogSwallowedException(L"TeamsService::_rootReactions (an emoji)");
            }
        }
    }

    void TeamsService::_legendReactions(const InboundMessage& root)
    {
        std::vector<Reaction> acts;
        {
            std::lock_guard lk{ _mtx };
            const ReactionDiff diff = DiffReactions(_state.legendReactions, root.reactions);
            acts = OwnerReactionsToAct(root.id, diff, _owner.id, _state.ownReactions);
            _state.legendReactions = root.reactions;
            _stateDirty = true;
        }
        for (const Reaction& r : acts)
        {
            if (ActionForReaction(MessageKind::Legend, r.emoji).action == Action::NewSession)
            {
                // §9.2: a session needs its first prompt, and a new post carries it (D-C9): the ➕ points there. Removed, so a
                // later press is seen as new.
                _react(root.id, L"", r.emoji, false);
                _postToRoot(root.id, L"\U0001F195 To open a session, write a NEW post in this channel: its words are the session's first prompt.", {});
            }
        }
    }

    void TeamsService::_cardAction(const std::wstring& sessionId, const std::wstring& rootId, const std::wstring& emoji)
    {
        // The owner's emoji is consumed whatever it does: the card's reactions stay free for the next press (TEAMS.md §15.4).
        _react(rootId, L"", emoji, false);
        const ReactionAction ra = ActionForReaction(MessageKind::SessionCard, emoji);
        LogNav(L"remote teams emoji " + ShortId(sessionId) + L" -> " + ActionWord(ra.action));
        switch (ra.action)
        {
        case Action::Screen:
            _screenTeams(sessionId);
            break;
        case Action::Esc:
            _escTeams(sessionId);
            break;
        case Action::Enter:
            _enterTeams(sessionId, L"");
            break;
        case Action::Wake:
            _wakeTeams(sessionId);
            break;
        case Action::Restart:
            _confirm(sessionId, Action::Restart); // ActionNeedsConfirm: a restart always asks
            break;
        case Action::Park:
        {
            // A park asks only while a turn runs (it would cut the turn off), Telegram's 🌙 rule.
            const auto s = _registry->GetWithoutQueue(sessionId);
            if (s && s->live && ActionNeedsConfirm(Action::Park, Msg::SessionTurnInFlight(*s)))
            {
                _confirm(sessionId, Action::Park);
            }
            else
            {
                _parkTeams(sessionId, false);
            }
            break;
        }
        case Action::Status:
            _statusTeams(sessionId);
            break;
        case Action::AllScreen:
            _allScreenTeams(sessionId, L""); // the default level (Teams has no level picker: /allscreen names another)
            break;
        case Action::Fork:
            _forkTeams(sessionId, L""); // the settings' model (/forksession names another)
            break;
        default:
            break; // an emoji the legend gives no meaning on a card: removed, nothing else
        }
    }

    void TeamsService::_confirm(const std::wstring& sessionId, Action a)
    {
        std::wstring title;
        if (const auto s = _registry->GetWithoutQueue(sessionId))
        {
            title = s->title;
        }
        const Action action = a;
        // Watched from its answer on (its id), for 60 s: the owner's ✅ on it runs the action (_watchedAction).
        _postReply(sessionId, ConfirmPostHtml(a, title), AlertKind::None, OutboxPriority::Answer, 0, [this, sessionId, action](const GraphResult& r) {
            try
            {
                if (!r.Ok())
                {
                    return;
                }
                const std::wstring id = r.body.StrAt(L"id");
                if (id.empty())
                {
                    return;
                }
                std::lock_guard lk{ _mtx };
                const auto it = _state.threads.find(sessionId);
                if (it == _state.threads.end())
                {
                    return;
                }
                Watched w;
                w.messageId = id;
                w.rootId = it->second.rootId;
                w.kind = MessageKind::Confirm;
                w.sessionId = sessionId;
                w.payload = ConfirmWord(action);
                w.expiresMs = TeamsNowMs() + kConfirmWindowMs;
                _state.watched.push_back(std::move(w));
                _stateDirty = true;
            }
            catch (...)
            {
                // Rule #18. Recovery: the confirm is posted but not watched: its ✅ does nothing; the owner presses the card again.
                LogSwallowedException(L"TeamsService::_confirm (watch)");
            }
        });
    }

    void TeamsService::_watchedStep(const std::wstring& sessionId, const std::wstring& rootId)
    {
        std::shared_ptr<GraphClient> http;
        TeamsConfig config;
        std::vector<Watched> watched;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            config = _config;
            const int64_t now = TeamsNowMs();
            for (const Watched& w : _state.watched)
            {
                // Questions are not answered from Teams in this build (the ask registry has one acceptor: the Telegram bot).
                if (w.rootId == rootId && w.kind != MessageKind::Question && (w.expiresMs == 0 || w.expiresMs >= now))
                {
                    watched.push_back(w);
                }
            }
        }
        if (!http || watched.empty())
        {
            return;
        }
        for (const Watched& w : watched)
        {
            GraphResult r;
            if (!_readGet(*http, MessagePath(config.teamId, config.channelId, rootId, w.messageId), L"watched reply", r))
            {
                return; // no source can read
            }
            if (!r.Ok())
            {
                if (r.httpStatus == 404)
                {
                    // The reply is gone (deleted): its watch goes too.
                    std::lock_guard lk{ _mtx };
                    _state.watched.erase(std::remove_if(_state.watched.begin(), _state.watched.end(), [&w](const Watched& x) { return x.messageId == w.messageId; }),
                                         _state.watched.end());
                    _stateDirty = true;
                }
                continue;
            }
            const InboundMessage m = ParseInboundMessage(r.body);
            std::vector<Reaction> acts;
            {
                std::lock_guard lk{ _mtx };
                const auto it = std::find_if(_state.watched.begin(), _state.watched.end(), [&w](const Watched& x) { return x.messageId == w.messageId; });
                if (it == _state.watched.end())
                {
                    continue; // ended meanwhile (a prompt that was sent)
                }
                const ReactionDiff diff = DiffReactions(it->reactions, m.reactions);
                acts = OwnerReactionsToAct(w.messageId, diff, _owner.id, _state.ownReactions);
                it->reactions = m.reactions;
                _stateDirty = true;
                if (!acts.empty())
                {
                    _lastOwnerActionMs = TeamsNowMs();
                }
            }
            for (const Reaction& react : acts)
            {
                const ReactionAction ra = ActionForReaction(w.kind, react.emoji);
                if (ra.action == Action::None)
                {
                    continue;
                }
                try
                {
                    _watchedAction(w, ra.action, react.emoji);
                }
                catch (...)
                {
                    // Rule #18; recovery: the watch has ended without its action; the owner does it again (a command, a card emoji).
                    LogSwallowedException(L"TeamsService::_watchedStep (an action)");
                }
                break; // a watch acts once: its first meaningful emoji decides
            }
        }
    }

    void TeamsService::_watchedAction(const Watched& w, Action a, const std::wstring& emoji)
    {
        // The watch ends FIRST (a second emoji, or the next poll, finds nothing to act on), then the owner's emoji is consumed.
        {
            std::lock_guard lk{ _mtx };
            _state.watched.erase(std::remove_if(_state.watched.begin(), _state.watched.end(), [&w](const Watched& x) { return x.messageId == w.messageId; }),
                                 _state.watched.end());
            _stateDirty = true;
        }
        _react(w.rootId, w.messageId, emoji, false);
        LogNav(L"remote teams " + std::wstring{ MessageKindWord(w.kind) } + L" " + ShortId(w.sessionId) + L" -> " + ActionWord(a));
        switch (w.kind)
        {
        case MessageKind::Backlog:
            if (a == Action::Yes)
            {
                _queueTeamsPrompt(w.sessionId, w.rootId, w.messageId, w.payload); // §7.4: the owner's ✅ sends what waited
            }
            else
            {
                _answer(w.sessionId, L"<i>Dropped.</i>");
            }
            break;
        case MessageKind::Confirm:
            if (a == Action::Yes)
            {
                const Action confirmed = ConfirmActionFromWord(w.payload);
                if (confirmed == Action::Restart)
                {
                    _restartTeams(w.sessionId);
                }
                else if (confirmed == Action::Park)
                {
                    _parkTeams(w.sessionId, true); // confirmed: a turn in flight may be cut off
                }
            }
            else
            {
                _answer(w.sessionId, L"<i>Not done.</i>");
            }
            break;
        case MessageKind::QueuedPrompt:
            if (a == Action::No)
            {
                _unqueueTeams(w.sessionId, w.payload);
            }
            break;
        case MessageKind::ForkChoice:
        {
            // §8 🔀: 1️⃣ "Fork interrupted", 2️⃣ "Fork uninterrupted" (what each launches depends on the question's situation,
            // kept in the payload with the folder, the title and the model); ❌ forks nothing.
            const auto data = ParseForkQuestionPayload(w.payload);
            const auto pick = KeycapNumber(emoji);
            if (a == Action::Pick && data && pick && (*pick == 1 || *pick == 2))
            {
                _launchForkTeams(w.sessionId, *data, ForkChoiceFor(data->situation, *pick == 2));
            }
            else
            {
                _answer(w.sessionId, L"<i>No fork.</i>");
            }
            break;
        }
        default:
            break;
        }
    }

    void TeamsService::_restartTeams(const std::wstring& sessionId)
    {
        if (!_hooks.restartSession)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        try
        {
            // KeepInterrupted: the transcript is not touched and the PC asks nothing (Telegram's answer for a restart with
            // nothing to ask). The hosting window, whichever it is, restarts its tab in place.
            _hooks.restartSession(sessionId, RestartTurnChoice::KeepInterrupted);
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_restartTeams");
            _answer(sessionId, L"The restart could not be asked for (hooks.log says why).");
            return;
        }
        LogNav(L"remote teams restart " + ShortId(sessionId) + L" -> asked the hosting window");
        _answer(sessionId, std::wstring{ kEmojiRestart } + L" <i>Restarting (the conversation goes on).</i>");
    }

    void TeamsService::_wakeTeams(const std::wstring& sessionId)
    {
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || !s->live)
        {
            _answer(sessionId, L"This session is closed: resume it at the PC.");
            return;
        }
        if (s->started || s->external)
        {
            const bool ended = s->state == SessionState::Error || s->state == SessionState::Done;
            _answer(sessionId, ended ? L"<i>Its claude is not parked but ended: react " + std::wstring{ kEmojiRestart } + L" on the card to restart it.</i>" :
                                       std::wstring{ L"<i>The session is not parked.</i>" });
            return;
        }
        if (!_hooks.sessionAction)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        try
        {
            _hooks.sessionAction(sessionId, L"start", L"", nullptr);
        }
        catch (...)
        {
            LogSwallowedException(L"TeamsService::_wakeTeams");
            _answer(sessionId, L"The tab could not be started (hooks.log says why).");
            return;
        }
        LogNav(L"remote teams wake " + ShortId(sessionId) + L" -> asked the hosting window");
        _answer(sessionId, std::wstring{ kEmojiWake } + L" <i>Starting the tab.</i>");
    }

    void TeamsService::_screenTeams(const std::wstring& sessionId)
    {
        if (!_hooks.sessionAction)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        std::wstring title;
        if (const auto s = _registry->GetWithoutQueue(sessionId))
        {
            title = s->title;
        }
        const std::weak_ptr<TeamsService> self = weak_from_this();
        const auto once = std::make_shared<AnswerOnce>([self, sessionId]() {
            if (const auto me = self.lock())
            {
                me->_answer(sessionId, L"<i>No window hosts this session's tab: there is no screen to send.</i>");
            }
        });
        LogNav(L"remote teams screen " + ShortId(sessionId) + L" -> asked the hosting window");
        // The STYLED snapshot (Telegram's 🖼 Screen Image): the window answers on its UI thread; the picture is drawn on a worker,
        // never there (Direct2D + WIC take tens of milliseconds for a full screen).
        _hooks.sessionAction(sessionId, L"screen-snapshot", L"", [self, sessionId, title, once](std::wstring snapshot) {
            if (!once->Claim())
            {
                return;
            }
            try
            {
                std::thread([self, sessionId, title, text = std::move(snapshot)]() noexcept {
                    const auto me = self.lock();
                    if (!me)
                    {
                        return; // the service is gone (an app quit)
                    }
                    try
                    {
                        const auto parsed = ParseScreenSnapshot(text);
                        if (!parsed)
                        {
                            // Not a snapshot this build reads: the screen's words, at least.
                            me->_answer(sessionId, ScreenTextPostHtml(text));
                            return;
                        }
                        const ScreenImage image = RenderScreenSnapshotPng(*parsed);
                        if (image.png.empty())
                        {
                            me->_answer(sessionId, L"The screen could not be drawn: " + Msg::TgEscapeHtml(image.error));
                            me->_log(L"[teams] screen picture of " + ShortId(sessionId) + L" failed: " + image.error);
                            return;
                        }
                        const int64_t now = TeamsNowMs();
                        const std::wstring caption = L"\U0001F5BC <b>" + Msg::TgEscapeHtml(title.empty() ? ShortId(sessionId) : Msg::TgOneLineTitle(title)) + L"</b> · " +
                                                     Msg::TgClockText(now, now) + L" · " + std::to_wstring(parsed->cols) + L"×" + std::to_wstring(parsed->rows);
                        me->_postImage(sessionId, caption, image.png);
                        me->_log(L"[teams] screen picture of " + ShortId(sessionId) + L": " + std::to_wstring(image.png.size() / 1024) + L" KB");
                    }
                    catch (...)
                    {
                        // Rule #18: an exception escaping a std::thread would end the process. Recovery: no picture this time.
                        LogSwallowedException(L"TeamsService::_screenTeams (worker)");
                    }
                }).detach();
            }
            catch (...)
            {
                // The worker could not start (std::system_error): the owner is told, nothing is drawing.
                LogSwallowedException(L"TeamsService::_screenTeams (thread)");
                if (const auto me = self.lock())
                {
                    me->_answer(sessionId, L"The screen could not be drawn (hooks.log says why).");
                }
            }
        });
    }

    void TeamsService::_postImage(const std::wstring& sessionId, const std::wstring& captionHtml, const std::string& png)
    {
        if (png.size() > kImagePostMaxBytes)
        {
            _answer(sessionId, captionHtml + L"<br><i>The picture is too large for one post (" + std::to_wstring(png.size() / 1024) + L" KB).</i>");
            return;
        }
        TeamsConfig config;
        std::wstring rootId;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.threads.find(sessionId);
            if (it == _state.threads.end())
            {
                return;
            }
            rootId = it->second.rootId;
            config = _config;
        }
        _rememberOwnPost(rootId, captionHtml); // the caption's words: the poll knows the post as Agentmaster's before its answer
        OutboxItem item;
        item.priority = OutboxPriority::Answer;
        item.op = GraphOp::Post;
        item.method = L"POST";
        item.path = RepliesPath(config.teamId, config.channelId, rootId);
        item.body = BuildImagePostJson(captionHtml, png);
        item.what = L"picture in " + sessionId.substr(0, 8);
        item.lane = sessionId;
        item.done = [this](const GraphResult& r) { _journalPost(r); };
        _enqueue(std::move(item));
    }

    void TeamsService::_statusTeams(const std::wstring& sessionId)
    {
        std::optional<SessionInfo> session = _registry->Get(sessionId);
        if (!session)
        {
            _answer(sessionId, L"This session is closed.");
            return;
        }
        if (session->kind != AgentKind::Claude)
        {
            _answer(sessionId, L"\U0001F4CA Claude Code's status, account and plan usage are read for Claude Code sessions; this one runs Codex.");
            return;
        }
        // One process-wide slot per report (the Telegram bot's too): two reports at once would run two hidden claudes for nothing.
        if (!SharedClaudeStatusProbe().TryBeginReport())
        {
            _answer(sessionId, L"Two status reports are being read right now; ask again in a moment.");
            return;
        }
        // The engine's claude.exe: the one every session runs, so the account and the usage are those sessions' own.
        const std::wstring claudeExe = _hooks.claudeExe ? _hooks.claudeExe() : std::wstring{};
        // Telegram's title TOKEN is its send layer's; here the title is plain escaped text.
        const std::wstring titleToken = Msg::TgEscapeHtml(Msg::TgOneLineTitle(session->title));
        LogNav(L"remote teams status " + ShortId(sessionId) + L" -> reading");
        try
        {
            std::thread([self = weak_from_this(), report = std::move(session), sessionId, titleToken, claudeExe]() noexcept {
                // The slot goes back on every path out of the worker, the service gone included.
                struct SlotRelease
                {
                    ~SlotRelease()
                    {
                        SharedClaudeStatusProbe().EndReport();
                    }
                } release;
                const auto me = self.lock();
                if (!me)
                {
                    return;
                }
                try
                {
                    Msg::ClaudeStatusReportInput in;
                    in.session = &*report;
                    in.titleToken = titleToken;
                    in.account = SharedClaudeStatusProbe().Account(claudeExe);
                    // The heartbeat, trusted only when it names THIS conversation (a pid Windows gave to another claude never matches).
                    SessionPresenceRow row;
                    if (report->pid != 0 && ReadSessionPresenceForPid(report->pid, row) && row.sessionId == report->id)
                    {
                        in.process.running = true;
                        in.process.pid = row.pid;
                        in.process.version = row.version;
                        in.process.status = row.status;
                        in.process.waitingFor = row.waitingFor;
                        in.process.startedAtMs = row.startedAtMs;
                    }
                    // The session's own transcript, or a message-less fork's source (every view of a conversation reads it so).
                    const ClaudeDisplayTranscript display = ResolveClaudeDisplayTranscript(report->id, report->forkParentId);
                    if (display.path.empty())
                    {
                        in.transcriptNote = L"No transcript yet: the session has no message, so the rows its transcript gives are not shown.";
                    }
                    else
                    {
                        std::wstring error;
                        if (ReadClaudeTranscriptStatusFile(display.path, in.transcript, error))
                        {
                            in.transcriptRead = true;
                            if (!display.forkSourceId.empty())
                            {
                                in.transcriptNote = L"A fork with no message of its own yet: the rows from its transcript are its source's (" + ShortId(display.forkSourceId) + L").";
                            }
                        }
                        else
                        {
                            in.transcriptNote = L"Its transcript could not be read: " + error;
                        }
                    }
                    // The plan usage last: the slowest read (~6 s, a hidden claude -p /usage), bounded by its own time limit.
                    in.usage = SharedClaudeStatusProbe().Usage(claudeExe);
                    in.nowMs = TeamsNowMs();
                    const std::vector<std::wstring> messages = Msg::BuildClaudeStatusReport(in);
                    for (const std::wstring& message : messages)
                    {
                        me->_answer(sessionId, TeamsHtmlFromTelegramHtml(message));
                    }
                    me->_log(L"[teams] status report of " + ShortId(sessionId) + L": " + std::to_wstring(messages.size()) + L" post(s)");
                }
                catch (...)
                {
                    // Rule #18: an exception escaping a std::thread would end the process. Recovery: one line in the thread.
                    LogSwallowedException(L"TeamsService::_statusTeams (worker)");
                    try
                    {
                        me->_answer(sessionId, L"The status could not be read (hooks.log says why).");
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"TeamsService::_statusTeams (worker answer)");
                    }
                }
            }).detach();
        }
        catch (...)
        {
            // The worker could not start: nothing reads, so the slot goes back, and the owner is told.
            LogSwallowedException(L"TeamsService::_statusTeams");
            SharedClaudeStatusProbe().EndReport();
            _answer(sessionId, L"The status could not be read (hooks.log says why).");
        }
    }

    void TeamsService::_unqueueTeams(const std::wstring& sessionId, const std::wstring& promptId)
    {
        bool removed = false;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            const auto it = std::find_if(ss.queue.begin(), ss.queue.end(), [&promptId](const QueuedPrompt& q) { return q.id == promptId && q.status == PromptStatus::Pending; });
            if (it != ss.queue.end())
            {
                ss.queue.erase(it);
                removed = true;
            }
        });
        LogNav(L"remote teams unqueue " + ShortId(sessionId) + (removed ? L" -> removed" : L" -> not waiting"));
        _answer(sessionId, removed ? std::wstring{ L"<i>Taken out of the queue.</i>" } : std::wstring{ L"<i>It was sent already (or is no longer queued).</i>" });
    }

    // ===== the whole conversation as pictures (§8 📜) =========================================================================

    void TeamsService::_allScreenTeams(const std::wstring& sessionId, const std::wstring& args)
    {
        TurnsLevel defaultLevel = Msg::kAllScreenDefaultLevel;
        {
            std::lock_guard lk{ _mtx };
            defaultLevel = _allScreenLevel;
        }
        // A level named, "diffs", "last" (Telegram's parser): bare, the cog's default (Full unless set otherwise).
        const auto pick = Msg::ParseAllScreenArgs(args, defaultLevel);
        if (!pick)
        {
            _answer(sessionId, AllScreenUsageHtml(defaultLevel));
            return;
        }
        // Drawn from the TRANSCRIPT, never the terminal (ConversationImage.h): a fullscreen Claude Code keeps no scrollback, so
        // no window is asked, and a parked tab or a closed session draws as well as a live one. The lean read: no queue.
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s)
        {
            _answer(sessionId, L"This session is not known any more.");
            return;
        }
        if (s->kind != AgentKind::Claude)
        {
            _answer(sessionId, L"\U0001F4DC All Screen draws Claude Code conversations; this session runs Codex.");
            return;
        }
        std::wstring path = ResolveClaudeTranscriptPath(sessionId);
        if (path.empty() && !s->forkParentId.empty())
        {
            path = ResolveClaudeTranscriptPath(s->forkParentId); // a fork with no turn of its own shows its source (Telegram's rule)
        }
        if (path.empty())
        {
            _answer(sessionId, L"No transcript yet: the session has no conversation to draw.");
            return;
        }
        bool drawing = false;
        {
            // One drawing per session: a second press while the parts are drawn would post the whole conversation twice.
            std::lock_guard lk{ _mtx };
            drawing = !_allScreenSessions.insert(sessionId).second;
        }
        if (drawing)
        {
            // Answered after the lock: _answer takes _mtx itself (a std::mutex taken twice by one thread deadlocks).
            _answer(sessionId, L"<i>This conversation's pictures are being drawn already; they come in a moment.</i>");
            return;
        }
        // Takes the session's mark back on every path that starts no worker.
        const auto unmark = [this, &sessionId]() {
            std::lock_guard lk{ _mtx };
            _allScreenSessions.erase(sessionId);
        };
        if (_allScreenRenders.fetch_add(1) >= kAllScreenMaxRenders)
        {
            _allScreenRenders.fetch_sub(1);
            unmark();
            _answer(sessionId, L"<i>Another conversation is being drawn right now; ask again in a moment.</i>");
            return;
        }
        const std::wstring title = s->title;
        const TurnsLevel level = pick->level;
        const bool diffs = pick->diffs;
        const bool lastTurnOnly = pick->lastTurn;
        LogNav(L"remote teams allscreen " + ShortId(sessionId) + L" -> drawing " + TurnsLevelName(level) + (diffs ? L"+diffs" : L"") + (lastTurnOnly ? L"+last" : L""));
        try
        {
            // The worker holds the service only while it draws (a weak reference until then): an app quit mid-drawing loses the
            // remaining parts, nothing else.
            std::thread([self = weak_from_this(), sessionId, path, title, level, diffs, lastTurnOnly]() noexcept {
                const auto me = self.lock();
                if (!me)
                {
                    return; // the service is gone (an app quit): its counter and marks went with it
                }
                try
                {
                    ConversationImageOptions options;
                    options.level = level;
                    options.editDiffs = diffs; // every file change as a diff, whatever the level
                    options.lastTurnOnly = lastTurnOnly; // the newest turn only, numbered as in the whole conversation
                    options.image.maxPartHeightPx = kAllScreenPartHeightPx; // each part a post of its own (kImagePostMaxBytes)
                    const std::wstring levelName = TurnsLevelName(level);
                    const std::wstring titleLine = Msg::TgTitleLine(title, 120).empty() ? ShortId(sessionId) : Msg::TgTitleLine(title, 120);
                    size_t posted = 0;
                    size_t totalBytes = 0;
                    // Each part is queued as soon as it is drawn, so part 1 is on its way while part 2 is drawn (the session's
                    // lane keeps their order). A part past the post's bytes cap is told in words (_postImage).
                    options.onPart = [&](ConversationImagePart&& part) {
                        totalBytes += part.picture.png.size();
                        me->_postImage(sessionId,
                                       AllScreenCaptionHtml(titleLine, lastTurnOnly, levelName, diffs, part.index, part.count, part.firstTurn, part.lastTurn),
                                       part.picture.png);
                        ++posted;
                    };
                    const ConversationImageResult drawn = RenderConversationImages(path, options);
                    me->_log(L"[teams] conversation pictures of " + ShortId(sessionId) + L": " + levelName + (diffs ? L"+diffs" : L"") + (lastTurnOnly ? L"+last" : L"") + L", " +
                             std::to_wstring(posted) + L" of " + std::to_wstring(drawn.parts.size()) + L" part(s) posted, " + std::to_wstring(drawn.rows) + L" rows (" +
                             std::to_wstring(drawn.droppedRows) + L" left out), " + std::to_wstring(totalBytes / 1024) + L" KB, drawn in " + std::to_wstring(drawn.drawMs) +
                             L" ms" + (drawn.error.empty() ? std::wstring{} : L" - failed: " + drawn.error));
                    const std::wstring what = lastTurnOnly ? L"The last turn" : L"The conversation";
                    if (!drawn.error.empty())
                    {
                        me->_answer(sessionId, posted == 0 ? what + L" could not be drawn: " + Msg::TgEscapeHtml(drawn.error) :
                                                             what + L"'s pictures stopped after part " + std::to_wstring(posted) + L": " + Msg::TgEscapeHtml(drawn.error));
                    }
                    else if (drawn.droppedRows > 0)
                    {
                        me->_answer(sessionId, AllScreenCutNoticeHtml(lastTurnOnly, drawn.parts.size(), drawn.droppedRows, drawn.droppedThroughTurn, level, diffs));
                    }
                }
                catch (...)
                {
                    // Rule #18: an exception escaping a std::thread would end the process. Recovery: the parts posted stay; no line
                    // says why the rest is missing (hooks.log does).
                    LogSwallowedException(L"TeamsService::_allScreenTeams (worker)");
                }
                me->_allScreenRenders.fetch_sub(1);
                try
                {
                    std::lock_guard lk{ me->_mtx };
                    me->_allScreenSessions.erase(sessionId);
                }
                catch (...)
                {
                    // std::system_error from the lock: the mark stays, and the next request for this session hears "being drawn".
                    LogSwallowedException(L"TeamsService::_allScreenTeams (unmark)");
                }
            }).detach();
        }
        catch (...)
        {
            // The worker could not start (std::system_error, std::bad_alloc copying its arguments): nothing is drawing, so the
            // count goes back and the session is unmarked.
            _allScreenRenders.fetch_sub(1);
            unmark();
            LogSwallowedException(L"TeamsService::_allScreenTeams");
            _answer(sessionId, L"The pictures could not be started (hooks.log says why).");
        }
    }

    // ===== a fork from Teams (§8 🔀) ==========================================================================================

    void TeamsService::_forkTeams(const std::wstring& sessionId, const std::wstring& model)
    {
        if (!_hooks.launchAnswered)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        // The full record: its kind, state and folder decide (one queue copy per press).
        const auto s = _registry->Get(sessionId);
        if (!s)
        {
            _answer(sessionId, L"This session is not known any more.");
            return;
        }
        if (s->workingDir.empty())
        {
            _answer(sessionId, L"The session's folder is not known: fork it at the PC.");
            return;
        }
        ForkQuestionData data;
        data.dir = s->workingDir;
        data.title = s->title;
        data.model = model;
        if (s->kind != AgentKind::Claude)
        {
            // A Codex fork (`codex fork`) has no Claude Code resume to stop: nothing to ask (Telegram's rule).
            _launchForkTeams(sessionId, data, ForkTurnChoice::Uninterrupted);
            return;
        }
        const bool sourceRunning = s->live && (s->state == SessionState::Running || s->state == SessionState::NeedsApproval);
        // HOOKS.md "Interrupted turns", the one rule of the PC's fork seams and the Telegram bot: situation B is offered while
        // Agentmaster may take API errors back (resumeApiErrorTurns), situation C always. AssessForkContinuation never throws
        // (a failure is "do not ask", logged).
        const AppSettings settings = LoadAppSettings();
        const ForkContinuationOutlook outlook =
            AssessForkContinuation(sessionId, sourceRunning, s->workingDir, ResolveSessionEnv(settings, s->workingDir), settings.resumeApiErrorTurns, /*considerUserInterrupt*/ true);
        if (!outlook.ask)
        {
            LogNav(L"remote teams fork " + ShortId(sessionId) + L" -> not asked: " + outlook.why);
            _launchForkTeams(sessionId, data, ForkTurnChoice::Uninterrupted);
            return;
        }
        // The words the question needs, read from the outlook here so ForkQuestionHtml stays pure.
        ForkQuestionFacts facts;
        facts.sourceRunning = sourceRunning;
        const bool switchOff = ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile) == AutoContinueAtRestart::OffInSettings;
        if (outlook.apiError)
        {
            facts.situation = ForkSituation::ApiError;
            facts.errorText = outlook.apiErrorPlan.errorText;
            facts.switchOffInSettings = switchOff;
        }
        else if (outlook.userInterrupt)
        {
            facts.situation = ForkSituation::UserInterrupt;
            facts.duringToolUse = outlook.interruptPlan.duringToolUse;
            facts.sendsPromptAgain = outlook.interruptPlan.targetKind == L"prompt";
            facts.promptText = outlook.interruptPlan.targetText;
            facts.switchOffInSettings = switchOff;
        }
        else
        {
            facts.situation = ForkSituation::Continues;
            facts.droppedTool = outlook.tail.droppedTool;
        }
        data.situation = facts.situation;
        _askForkTeams(sessionId, s->title, data, facts);
    }

    void TeamsService::_askForkTeams(const std::wstring& sessionId, const std::wstring& title, const ForkQuestionData& data, const ForkQuestionFacts& facts)
    {
        const std::wstring payload = ForkQuestionPayload(data);
        // Watched from its answer on (the watch needs its id), for an hour: the owner's 1️⃣ / 2️⃣ / ❌ decides (_watchedAction).
        _postReply(sessionId, ForkQuestionHtml(title, facts), AlertKind::None, OutboxPriority::Answer, 0, [this, sessionId, payload](const GraphResult& r) {
            try
            {
                if (!r.Ok())
                {
                    return;
                }
                const std::wstring id = r.body.StrAt(L"id");
                if (id.empty())
                {
                    return;
                }
                std::lock_guard lk{ _mtx };
                const auto it = _state.threads.find(sessionId);
                if (it == _state.threads.end())
                {
                    return;
                }
                Watched w;
                w.messageId = id;
                w.rootId = it->second.rootId;
                w.kind = MessageKind::ForkChoice;
                w.sessionId = sessionId;
                w.payload = payload;
                w.expiresMs = TeamsNowMs() + kForkQuestionMs;
                _state.watched.push_back(std::move(w));
                _stateDirty = true;
            }
            catch (...)
            {
                // Rule #18. Recovery: the question is posted but not watched: its numbers do nothing; the owner presses 🔀 again.
                LogSwallowedException(L"TeamsService::_askForkTeams (watch)");
            }
        });
        LogNav(L"remote teams fork " + ShortId(sessionId) + L" -> asked (" + ForkSituationWord(facts.situation) + L")");
    }

    void TeamsService::_launchForkTeams(const std::wstring& sourceId, const ForkQuestionData& data, ForkTurnChoice choice)
    {
        if (!_hooks.launchAnswered)
        {
            _answer(sourceId, L"Not available here.");
            return;
        }
        RemoteLaunchRequest req;
        req.kind = RemoteLaunchRequest::Kind::Fork;
        req.sessionId = sourceId;
        req.dir = data.dir;
        req.title = data.title; // the source's: the window derives the fork's own ("(fork)", "(fork 2)" …, DeriveForkTitle)
        req.model = data.model;
        req.forkTurn = choice;
        LaunchAsk launch;
        launch.askedIn = sourceId;
        launch.kind = LaunchAsk::Kind::Fork;
        launch.folder = data.dir;
        {
            std::lock_guard lk{ _mtx };
            launch.runMs = _startedMs;
        }
        _answer(sourceId, L"\U0001F500 <i>Forking it into a new tab\x2026</i>");
        LogNav(L"remote teams fork " + ShortId(sourceId) + L" -> asking a window (" + ForkTurnChoiceName(choice) + L")");
        if (!_askWindow(req, launch))
        {
            _answer(sourceId, L"No window could open the fork: open Agentmaster at the PC, then press \U0001F500 again.");
            LogNav(L"remote teams fork " + ShortId(sourceId) + L" -> no window took it");
        }
    }

    void TeamsService::_newSessionTeams(const std::wstring& sessionId, const std::wstring& model)
    {
        if (!_hooks.launchAnswered)
        {
            _answer(sessionId, L"Not available here.");
            return;
        }
        // The lean record: only the folder is read.
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || s->workingDir.empty())
        {
            _answer(sessionId, L"The session's folder is not known: write a new post in the channel to open a session in a folder you pick.");
            return;
        }
        // Telegram's /newsession in a topic: a new session in the topic's session's folder, the settings' model unless one is
        // named, no title (the window derives one from the folder) and no first prompt (the owner writes it in the new thread).
        RemoteLaunchRequest req;
        req.kind = RemoteLaunchRequest::Kind::NewSession;
        req.dir = s->workingDir;
        req.model = model;
        LaunchAsk launch;
        launch.askedIn = sessionId;
        launch.kind = LaunchAsk::Kind::NewInFolder;
        launch.folder = s->workingDir;
        {
            std::lock_guard lk{ _mtx };
            launch.runMs = _startedMs;
        }
        _answer(sessionId, L"\U0001F195 <i>Opening a new session in</i> <code>" + Msg::TgEscapeHtml(s->workingDir) + L"</code>\x2026");
        LogNav(L"remote teams newsession " + ShortId(sessionId) + L" dir=" + s->workingDir + (model.empty() ? std::wstring{} : L" model=" + model) + L" -> asking a window");
        if (!_askWindow(req, launch))
        {
            _answer(sessionId, L"No window could open the session: open Agentmaster at the PC, then ask again.");
            LogNav(L"remote teams newsession " + ShortId(sessionId) + L" -> no window took it");
        }
    }

    bool TeamsService::_askWindow(const RemoteLaunchRequest& req, const LaunchAsk& launch) noexcept
    {
        try
        {
            if (!_hooks.launchAnswered)
            {
                return false;
            }
            // The answer may outlive the service (a window answers whenever its UI thread runs the request): a weak reference.
            // _onLaunchAnswered only queues it for the tick, so the window's thread never waits on _mtx.
            const std::weak_ptr<TeamsService> self = weak_from_this();
            return _hooks.launchAnswered(req, [self, launch](const RemoteLaunchOutcome& outcome) {
                if (const auto me = self.lock())
                {
                    me->_onLaunchAnswered(launch, outcome);
                }
            });
        }
        catch (...)
        {
            // Rule #18. Recovery: taken as not asked: the caller tells the owner, who asks again.
            LogSwallowedException(L"TeamsService::_askWindow");
            return false;
        }
    }

    void TeamsService::_settleThreadAsk(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome)
    {
        const bool isFork = launch.kind == LaunchAsk::Kind::Fork; // else NewInFolder: /newsession's new session
        const std::wstring logLead = std::wstring{ isFork ? L"remote teams fork " : L"remote teams newsession " } + ShortId(launch.askedIn);
        if (!RemoteLaunchOpened(outcome))
        {
            _answer(launch.askedIn, std::wstring{ isFork ? L"\U0001F500 The fork did not start. " : L"\U0001F195 The new session did not start. " } +
                                        Msg::TgEscapeHtml(RemoteLaunchRefusalText(outcome)));
            LogNav(logLead + L" -> " + RemoteLaunchResultName(outcome.result));
            return;
        }
        std::wstring title;
        if (const auto opened = _registry->GetWithoutQueue(outcome.sessionId))
        {
            title = opened->title;
        }
        {
            // The owner asked for it here: it gets a thread at the next pass, whatever Telegram's rule says of a new idle tab.
            std::lock_guard lk{ _mtx };
            _wantThread.insert(outcome.sessionId);
        }
        const std::wstring named = L"<b>" + Msg::TgEscapeHtml(title.empty() ? ShortId(outcome.sessionId) : title) + L"</b>";
        if (isFork)
        {
            _answer(launch.askedIn, L"\U0001F500 <i>Forked into a new tab:</i> " + named + (outcome.fresh ? L" (a NEW session: the source had no transcript to continue)" : L"") +
                                        L". Its own thread comes up in the channel.");
        }
        else
        {
            _answer(launch.askedIn, L"\U0001F195 <i>Opened a new session in the folder:</i> " + named + L". Its own thread comes up in the channel: write its first prompt there.");
        }
        LogNav(logLead + L" -> opened " + ShortId(outcome.sessionId) + (outcome.fresh ? L" (fresh)" : L""));
    }

    // ===== the 🔎 Search thread (§9.3) =======================================================================================

    void TeamsService::_searchRootStep(int64_t nowMs)
    {
        TeamsConfig config;
        Owner owner;
        {
            std::lock_guard lk{ _mtx };
            // Posted once: with a search here (a factory), a source that can post, and this run's state loaded (it may hold
            // the root's id already).
            if (!_searchFactory || !_state.searchId.empty() || _searchPosting || nowMs < _searchRetryAtMs || _postsBy.empty() || !_stateLoaded)
            {
                return;
            }
            _searchPosting = true;
            config = _config;
            owner = _owner;
        }
        const std::wstring html = SearchRootHtml();
        _rememberOwnPost(L"", html); // the poll never reads the new root as the owner's new post (which would open a session)
        OutboxItem item;
        item.priority = OutboxPriority::Card;
        item.op = GraphOp::Post;
        item.method = L"POST";
        item.path = ChannelMessagesPath(config.teamId, config.channelId);
        item.body = BuildPostJson(html, kSearchSubject, AlertKind::None, owner);
        item.what = L"search root post";
        item.done = [this](const GraphResult& r) {
            try
            {
                _journalPost(r);
                const std::wstring id = r.Ok() ? r.body.StrAt(L"id") : std::wstring{};
                std::lock_guard lk{ _mtx };
                _searchPosting = false;
                if (id.empty())
                {
                    _searchRetryAtMs = TeamsNowMs() + kLegendRetryMs; // tried again a minute later, as the legend
                    return;
                }
                _state.searchId = id;
                _stateDirty = true;
            }
            catch (...)
            {
                // Rule #18. Recovery: the root may stand without its id kept; the next run posts another (the old one is a
                // post of Agentmaster's, journaled, never read as the owner's).
                LogSwallowedException(L"TeamsService::_searchRootStep (answer)");
            }
        };
        _enqueue(std::move(item));
        _log(L"[teams] posting the Search thread's root");
    }

    void TeamsService::_searchStep(const InboundMessage& root)
    {
        std::shared_ptr<GraphClient> http;
        TeamsConfig config;
        int64_t readTo = 0;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            config = _config;
            readTo = _searchReadToMs;
        }
        if (!http)
        {
            return;
        }
        GraphResult replies;
        if (!_readGet(*http, RepliesPath(config.teamId, config.channelId, root.id) + L"?$top=" + std::to_wstring(kRepliesPerRead), L"search replies", replies) ||
            !replies.Ok())
        {
            return;
        }
        std::vector<InboundMessage> fresh;
        {
            std::lock_guard lk{ _mtx };
            fresh = NewOwnerReplies(replies.body, _owner.id, _state.ownPosts, readTo);
        }
        for (const InboundMessage& m : fresh)
        {
            if (_isRecentOwnPost(root.id, m))
            {
                continue; // Agentmaster's own answer, read before its post's answer journaled it
            }
            {
                std::lock_guard lk{ _mtx };
                _searchReadToMs = (std::max)(_searchReadToMs, m.createdMs); // each reply is read once
                _lastOwnerActionMs = TeamsNowMs();
            }
            const SearchReply reply = ClassifySearchReply(m.text);
            switch (reply.kind)
            {
            case SearchReplyKind::Clear:
            {
                std::shared_ptr<Msg::SearchBridge> bridge;
                {
                    std::lock_guard lk{ _mtx };
                    bridge = _search;
                    _searchRows.clear();
                }
                if (bridge)
                {
                    bridge->Reset(); // the context, the record and the shown list; works on a parked conversation too
                }
                _postToRoot(root.id, L"\U0001F50E <i>The search conversation starts over.</i>", {});
                LogNav(L"remote teams search -> cleared");
                break;
            }
            case SearchReplyKind::Help:
                _postToRoot(root.id, SearchRootHtml(), {});
                break;
            case SearchReplyKind::Pick:
                _searchPick(root.id, reply.pick);
                break;
            case SearchReplyKind::Request:
            default:
            {
                if (reply.text.empty())
                {
                    break; // a picture or a file without words
                }
                const std::shared_ptr<Msg::SearchBridge> bridge = _ensureSearch();
                if (!bridge)
                {
                    _postToRoot(root.id, L"Search is not available here.", {});
                    break;
                }
                // Said before the ask: the answer (seconds later, on the agent's thread) goes into the same lane after it.
                _postToRoot(root.id, L"\U0001F50E <i>Searching\x2026</i>", {});
                const uint64_t turn = bridge->Ask(reply.text);
                if (turn == 0)
                {
                    _postToRoot(root.id, L"<i>A search is running: ask again when it has answered.</i>", {});
                    break;
                }
                LogNav(L"remote teams search -> asked (turn " + std::to_wstring(turn) + L", " + std::to_wstring(reply.text.size()) + L" chars)");
                break;
            }
            }
        }
    }

    std::shared_ptr<Msg::SearchBridge> TeamsService::_ensureSearch()
    {
        Msg::SearchBridgeFactory factory;
        std::wstring thinking;
        {
            std::lock_guard lk{ _mtx };
            if (_search)
            {
                return _search;
            }
            factory = _searchFactory;
            thinking = _searchThinking;
        }
        if (!factory)
        {
            return nullptr;
        }
        // Made outside the lock: the factory may make the conversation (its worker thread) and read the settings. The sink holds
        // the service weakly: the conversation outlives every bridge.
        const std::weak_ptr<TeamsService> self = weak_from_this();
        std::shared_ptr<Msg::SearchBridge> made{ factory(
            [self](const Msg::SearchBridgeEvent& ev) {
                if (const auto me = self.lock())
                {
                    me->_onSearchEvent(ev);
                }
            },
            thinking) };
        // `lk` is declared after `made`, so it unlocks first: a bridge that lost a race is destroyed (detached) outside the lock.
        std::lock_guard lk{ _mtx };
        if (!_search)
        {
            _search = std::move(made);
        }
        return _search;
    }

    void TeamsService::_onSearchEvent(const Msg::SearchBridgeEvent& ev) noexcept
    {
        try
        {
            std::wstring rootId;
            {
                std::lock_guard lk{ _mtx };
                rootId = _state.searchId;
            }
            if (rootId.empty())
            {
                return; // no Search thread in this run's channel: nowhere to answer
            }
            if (ev.kind == Msg::SearchBridgeEvent::Kind::Failed)
            {
                _postToRoot(rootId, L"\x26A0 " + Msg::TgEscapeHtml(ev.text.empty() ? std::wstring{ L"The search failed." } : ev.text), {});
                LogNav(L"remote teams search -> failed");
                return;
            }
            if (ev.kind != Msg::SearchBridgeEvent::Kind::Answer)
            {
                return; // Searching and Shown are quiet: the Answer carries the rows (the bridge hands the last Shown to it)
            }
            std::vector<SearchListing> rows;
            rows.reserve(ev.rows.size());
            for (const Msg::SearchRow& row : ev.rows)
            {
                SearchListing listing;
                listing.sessionId = row.sessionId;
                listing.title = row.title;
                listing.dir = row.dir;
                listing.lastActivityMs = row.lastActivityMs;
                listing.prompts = row.prompts;
                listing.why = row.why;
                // Where it stands NOW (a pick reads it again): ▶ for a session a tab hosts.
                if (const auto s = _registry->GetWithoutQueue(row.sessionId))
                {
                    listing.open = s->live;
                }
                rows.push_back(std::move(listing));
            }
            const size_t count = rows.size();
            const std::wstring html = SearchAnswerHtml(ev.text, rows, TeamsNowMs());
            {
                // The picks read THIS answer's rows: an answer that listed nothing leaves no rows to pick.
                std::lock_guard lk{ _mtx };
                _searchRows = std::move(rows);
            }
            _postToRoot(rootId, html, {});
            LogNav(L"remote teams search -> answered (" + std::to_wstring(count) + L" sessions)");
        }
        catch (...)
        {
            // Rule #18. Recovery: this answer is not posted; the owner asks again.
            LogSwallowedException(L"TeamsService::_onSearchEvent");
        }
    }

    void TeamsService::_searchPick(const std::wstring& rootId, int pick)
    {
        SearchListing row;
        bool found = false;
        {
            std::lock_guard lk{ _mtx };
            if (pick >= 1 && static_cast<size_t>(pick) <= _searchRows.size())
            {
                row = _searchRows[static_cast<size_t>(pick) - 1];
                found = true;
            }
        }
        if (!found)
        {
            _postToRoot(rootId, L"No session " + std::to_wstring(pick) + L" in the last answer: search first, then reply with a number it listed.", {});
            return;
        }
        // Where it stands NOW, and the title the PC shows (the registry's, else the agent's).
        const auto s = _registry->GetWithoutQueue(row.sessionId);
        const std::wstring title = s && !s->title.empty() ? s->title : (row.title.empty() ? ShortId(row.sessionId) : row.title);
        const std::wstring named = L"<b>" + Msg::TgEscapeHtml(Msg::TgOneLineTitle(title)) + L"</b>";
        if (s && s->live)
        {
            // Open in a tab: a link to its thread, or a thread now (the owner asked about it here).
            std::wstring link;
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.threads.find(row.sessionId); it != _state.threads.end())
                {
                    link = it->second.webUrl;
                }
                else
                {
                    _wantThread.insert(row.sessionId);
                }
            }
            _postToRoot(rootId,
                        L"\x25B6 " + named + L" is open in a tab" +
                            (link.empty() ? std::wstring{ L": its thread comes up in the channel." } : L": <a href=\"" + Msg::detail::TgEscapeHtmlAttribute(link) + L"\">its thread</a>."),
                        {});
            LogNav(L"remote teams search pick " + std::to_wstring(pick) + L" " + ShortId(row.sessionId) + L" -> open already");
            return;
        }
        if (!_hooks.launchAnswered)
        {
            _postToRoot(rootId, named + L" is closed, and resuming is not available here: resume it from the Sessions page at the PC.", {});
            return;
        }
        // Closed: resumed into a tab (the registry's folder, else the agent's).
        const std::wstring dir = s && !s->workingDir.empty() ? s->workingDir : row.dir;
        if (dir.empty())
        {
            _postToRoot(rootId, named + L" is closed and its folder is not known: resume it from the Sessions page at the PC.", {});
            return;
        }
        RemoteLaunchRequest req;
        req.kind = RemoteLaunchRequest::Kind::Resume;
        req.sessionId = row.sessionId;
        req.dir = dir;
        req.title = s ? s->title : row.title;
        LaunchAsk launch;
        launch.kind = LaunchAsk::Kind::SearchPick;
        launch.rootId = rootId;
        launch.folder = dir;
        {
            std::lock_guard lk{ _mtx };
            launch.runMs = _startedMs;
        }
        _postToRoot(rootId, L"\x25B6 <i>Resuming</i> " + named + L" <i>into a tab\x2026</i>", {});
        LogNav(L"remote teams search pick " + std::to_wstring(pick) + L" " + ShortId(row.sessionId) + L" -> resume asked");
        if (!_askWindow(req, launch))
        {
            _postToRoot(rootId, L"No window could open it: open Agentmaster at the PC, then pick it again.", {});
            LogNav(L"remote teams search pick " + ShortId(row.sessionId) + L" -> no window took it");
        }
    }

    void TeamsService::_settleSearchPick(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome)
    {
        if (!RemoteLaunchOpened(outcome))
        {
            _postToRoot(launch.rootId, L"\x25B6 It did not open. " + Msg::TgEscapeHtml(RemoteLaunchRefusalText(outcome)), {});
            LogNav(L"remote teams search resume -> " + std::wstring{ RemoteLaunchResultName(outcome.result) });
            return;
        }
        std::wstring title;
        if (const auto opened = _registry->GetWithoutQueue(outcome.sessionId))
        {
            title = opened->title;
        }
        {
            // The owner asked for it here: it gets a thread at the next pass, whatever Telegram's rule says of an idle tab.
            std::lock_guard lk{ _mtx };
            _wantThread.insert(outcome.sessionId);
        }
        const std::wstring named = L"<b>" + Msg::TgEscapeHtml(title.empty() ? ShortId(outcome.sessionId) : Msg::TgOneLineTitle(title)) + L"</b>";
        const std::wstring said = outcome.result == RemoteLaunchResult::AlreadyOpen ?
                                      L"\x25B6 " + named + L" was open already." :
                                      L"\x25B6 <i>Resumed</i> " + named + L" <i>into a tab</i>" + (outcome.fresh ? L" (a NEW session: its transcript is gone)" : L"") + L".";
        _postToRoot(launch.rootId, said + L" Its thread comes up in the channel.", {});
        LogNav(L"remote teams search resume " + ShortId(outcome.sessionId) + L" -> " + RemoteLaunchResultName(outcome.result));
    }

    // ===== a new session from a new post (§9.1) ==============================================================================

    void TeamsService::_postToRoot(const std::wstring& rootId, const std::wstring& html, std::function<void(const GraphResult&)> onDone)
    {
        TeamsConfig config;
        Owner owner;
        {
            std::lock_guard lk{ _mtx };
            config = _config;
            owner = _owner;
        }
        _rememberOwnPost(rootId, html); // the poll knows it as Agentmaster's even before its answer journals it
        OutboxItem item;
        item.priority = OutboxPriority::Answer;
        item.op = GraphOp::Post;
        item.method = L"POST";
        item.path = RepliesPath(config.teamId, config.channelId, rootId);
        item.body = BuildPostJson(html, L"", AlertKind::None, owner);
        item.what = L"reply under post " + rootId;
        item.lane = L"post:" + rootId; // no session's lane (the post is no thread yet): its replies keep their order
        item.done = [this, onDone](const GraphResult& r) {
            _journalPost(r);
            if (onDone)
            {
                onDone(r);
            }
        };
        _enqueue(std::move(item));
    }

    std::vector<std::wstring> TeamsService::_wizardFolders()
    {
        std::vector<SessionFolderFacts> facts;
        if (_registry)
        {
            for (const auto& sample : _registry->PlaceSamples())
            {
                facts.push_back(SessionFolderFacts{ sample.workingDir, sample.inferredWorkingDir, sample.lastActivityUnixMs });
            }
        }
        // The machine's temp folders are left out: a session started there is a probe, never a place to work.
        std::vector<std::wstring> scratchRoots;
        try
        {
            for (const auto& tempRoot : CollectMachineTempRoots())
            {
                scratchRoots.push_back(NormDirKey(TrimFolderPath(tempRoot)));
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: no scratch roots: a temp folder a session ran in may be offered; nothing else changes.
            LogSwallowedException(L"TeamsService::_wizardFolders (temp roots)");
        }
        const uint64_t probeStartTick = ::GetTickCount64(); // the list's whole budget counts from here
        const Msg::NewThreadLists lists = Msg::BuildNewThreadLists(
            facts,
            scratchRoots,
            [probeStartTick](const std::wstring& path) {
                // A UNC path is listed unchecked: an offline share would hold the poll thread for the network's timeout.
                if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
                {
                    return true;
                }
                // What is left of the budget, at most one folder's share; 0 once it is spent (the probe then only starts, and
                // its answer serves the next list).
                const uint64_t spentMs = ::GetTickCount64() - probeStartTick;
                const uint32_t waitMs = spentMs >= Msg::kNewThreadListProbeBudgetMs ?
                                            0 :
                                            static_cast<uint32_t>((std::min<uint64_t>)(Msg::kNewThreadListProbeEachMs, Msg::kNewThreadListProbeBudgetMs - spentMs)); // (std::min): the <windows.h> macro
                // Only a folder known to be gone is left out; one that did not answer is listed (the pick checks it again).
                return SharedFolderProber().Probe(path, waitMs, Msg::kNewThreadListProbeMaxAgeMs) != FolderProbeAnswer::NotFolder;
            },
            kWizardMaxFolders);
        std::vector<std::wstring> folders;
        for (const Msg::NewThreadFolder& f : lists.recent)
        {
            folders.push_back(f.path);
        }
        return folders;
    }

    void TeamsService::_startWizard(const InboundMessage& root)
    {
        const std::wstring prompt = detail::Trim(root.text);
        if (prompt.empty())
        {
            return; // a post without words (a picture alone): no first prompt to start with
        }
        {
            std::lock_guard lk{ _mtx };
            if (!_wizardRoots.insert(root.id).second)
            {
                return; // this run started one for this post already (every reply under the post changes it again)
            }
            _lastOwnerActionMs = TeamsNowMs(); // the owner just wrote: the poll looks every 5 s for the pick (§7.3)
        }
        if (!_hooks.launchAnswered)
        {
            // A host that cannot open a tab and answer (a test harness): said once, no step asked.
            _postToRoot(root.id, L"A new session cannot be opened from Teams here: open it at the PC.", {});
            return;
        }
        WizardData data;
        data.prompt = prompt;
        data.webUrl = root.webUrl;
        data.folders = _wizardFolders();
        const std::wstring rootId = root.id;
        const std::wstring payload = WizardPayload(data);
        // The step is watched from its answer on (the watch needs its id): a number emoji on it picks a folder. Its data rides
        // the watch, so a restart in between still knows the words and the list.
        _postToRoot(rootId, WizardStepHtml(prompt, data.folders), [this, rootId, payload](const GraphResult& r) {
            try
            {
                const std::wstring id = r.Ok() ? r.body.StrAt(L"id") : std::wstring{};
                std::lock_guard lk{ _mtx };
                if (id.empty())
                {
                    _wizardRoots.erase(rootId); // the step did not go: the post's next change asks again
                    return;
                }
                Watched w;
                w.messageId = id;
                w.rootId = rootId;
                w.kind = MessageKind::Step;
                w.payload = payload;
                w.expiresMs = TeamsNowMs() + kWizardStepMs;
                _state.watched.push_back(std::move(w));
                _stateDirty = true;
            }
            catch (...)
            {
                // Rule #18. Recovery: the step is posted but not watched: its pick does nothing; the owner writes a new post.
                LogSwallowedException(L"TeamsService::_startWizard (the step's watch)");
            }
        });
        LogNav(L"remote teams new post " + rootId + L" -> folder step (" + std::to_wstring(data.folders.size()) + L" folders)");
    }

    void TeamsService::_wizardStep(const InboundMessage& root)
    {
        std::optional<Watched> watch;
        std::shared_ptr<GraphClient> http;
        TeamsConfig config;
        int64_t readTo = 0;
        bool ended = false;
        {
            std::lock_guard lk{ _mtx };
            http = _http;
            config = _config;
            const auto it = std::find_if(_state.watched.begin(), _state.watched.end(), [&root](const Watched& w) { return w.kind == MessageKind::Step && w.rootId == root.id; });
            if (it != _state.watched.end())
            {
                if (root.deleted)
                {
                    // The owner deleted the post: its session is not wanted any more (nothing is posted under a deleted post).
                    _state.watched.erase(it);
                    _stateDirty = true;
                    _wizardRepliesMs.erase(root.id);
                    ended = true;
                }
                else
                {
                    watch = *it;
                    // The owner's replies are read once: those read before do not count again (nor, after a restart, any older
                    // than the post itself).
                    const auto seen = _wizardRepliesMs.find(root.id);
                    readTo = (std::max)(root.createdMs, seen == _wizardRepliesMs.end() ? int64_t{ 0 } : seen->second);
                }
            }
        }
        if (ended)
        {
            LogNav(L"remote teams new post " + root.id + L" -> deleted: its step ends");
            return;
        }
        if (!watch || !http)
        {
            return; // the step's answer has not come back yet (the watch needs its id), or the step ended (a pick, an expiry)
        }
        const WizardData data = ParseWizardPayload(watch->payload);
        // 1. The owner's number emoji on the step picks a listed folder.
        GraphResult stepRead;
        if (!_readGet(*http, MessagePath(config.teamId, config.channelId, root.id, watch->messageId), L"new session step", stepRead))
        {
            return; // no source can read now: the post's next change reads it again
        }
        if (stepRead.Ok())
        {
            const InboundMessage step = ParseInboundMessage(stepRead.body);
            std::vector<Reaction> acts;
            {
                std::lock_guard lk{ _mtx };
                const auto it = std::find_if(_state.watched.begin(), _state.watched.end(), [&watch](const Watched& x) { return x.messageId == watch->messageId; });
                if (it == _state.watched.end())
                {
                    return; // ended meanwhile (an expiry)
                }
                acts = OwnerReactionsToAct(watch->messageId, DiffReactions(it->reactions, step.reactions), _owner.id, _state.ownReactions);
                it->reactions = step.reactions;
                _stateDirty = true;
                if (!acts.empty())
                {
                    _lastOwnerActionMs = TeamsNowMs();
                }
            }
            for (const Reaction& react : acts)
            {
                const ReactionAction ra = ActionForReaction(MessageKind::Step, react.emoji);
                if (ra.action != Action::Pick || ra.pick < 1 || static_cast<size_t>(ra.pick) > data.folders.size())
                {
                    continue; // a feeling, or a number the step did not list
                }
                _react(root.id, watch->messageId, react.emoji, false); // consumed: the step is free for another pick
                // The owner's replies older than the pick are not prompts once the post is the session's thread.
                _pickWizardFolder(root.id, watch->messageId, data, data.folders[static_cast<size_t>(ra.pick) - 1], (std::max)(readTo, react.atMs));
                return;
            }
        }
        else if (stepRead.httpStatus == 404)
        {
            // The step is gone (the owner deleted it): its watch goes too, and with it the post's new session.
            {
                std::lock_guard lk{ _mtx };
                _state.watched.erase(std::remove_if(_state.watched.begin(), _state.watched.end(), [&watch](const Watched& x) { return x.messageId == watch->messageId; }),
                                     _state.watched.end());
                _stateDirty = true;
                _wizardRepliesMs.erase(root.id);
            }
            LogNav(L"remote teams new post " + root.id + L" -> its step was deleted: the step ends");
            return;
        }
        // 2. Or the owner's reply that is one folder's full path.
        GraphResult replies;
        if (!_readGet(*http, RepliesPath(config.teamId, config.channelId, root.id) + L"?$top=" + std::to_wstring(kRepliesPerRead), L"new session replies", replies) ||
            !replies.Ok())
        {
            return;
        }
        std::vector<InboundMessage> fresh;
        {
            std::lock_guard lk{ _mtx };
            fresh = NewOwnerReplies(replies.body, _owner.id, _state.ownPosts, readTo);
        }
        for (const InboundMessage& m : fresh)
        {
            if (_isRecentOwnPost(root.id, m))
            {
                continue; // Agentmaster's own, read before its answer journaled it
            }
            {
                std::lock_guard lk{ _mtx };
                int64_t& seen = _wizardRepliesMs[root.id];
                seen = (std::max)(seen, m.createdMs); // read once: words that name no folder are not read again
                _lastOwnerActionMs = TeamsNowMs();
            }
            if (const auto folder = Msg::NewThreadTypedFolder(m.text))
            {
                _pickWizardFolder(root.id, watch->messageId, data, *folder, m.createdMs);
                return;
            }
        }
    }

    void TeamsService::_pickWizardFolder(const std::wstring& rootId, const std::wstring& stepId, const WizardData& data, const std::wstring& folder, int64_t readToMs)
    {
        // A FRESH check (Telegram's §21.17.4): a listed folder can be gone since the list was made, and a typed one may never have
        // been there. A folder that is not there, or that did not answer, is said, and the step stays for another pick.
        const FolderProbeAnswer probe = SharedFolderProber().Probe(folder, Msg::kNewThreadPickProbeWaitMs, 0);
        const std::wstring shown = L"<code>" + Msg::TgEscapeHtml(folder) + L"</code>";
        if (probe == FolderProbeAnswer::NotFolder)
        {
            _postToRoot(rootId, shown + L" is not a folder on the PC. React with another number, or reply with an existing folder's full path.", {});
            LogNav(L"remote teams new post " + rootId + L" -> not a folder: " + folder);
            return;
        }
        if (probe == FolderProbeAnswer::NoAnswer)
        {
            _postToRoot(rootId, Msg::NewThreadFolderNoAnswerText(shown), {});
            LogNav(L"remote teams new post " + rootId + L" -> the folder did not answer: " + folder);
            return;
        }
        _launchFromWizard(rootId, stepId, data, folder, readToMs);
    }

    void TeamsService::_launchFromWizard(const std::wstring& rootId, const std::wstring& stepId, const WizardData& data, const std::wstring& folder, int64_t readToMs)
    {
        const bool canLaunch = static_cast<bool>(_hooks.launchAnswered);
        LaunchAsk launch;
        launch.rootId = rootId;
        launch.sessionId = NewSessionId();
        launch.prompt = data.prompt;
        launch.webUrl = data.webUrl;
        launch.folder = folder;
        launch.readToMs = readToMs;
        {
            std::lock_guard lk{ _mtx };
            // The step's watch ends: a second pick finds nothing (the post stays in _wizardRoots: it never starts again).
            _state.watched.erase(std::remove_if(_state.watched.begin(), _state.watched.end(), [&stepId](const Watched& x) { return x.messageId == stepId; }),
                                 _state.watched.end());
            _stateDirty = true;
            _wizardRoots.insert(rootId);
            _wizardRepliesMs.erase(rootId);
            launch.runMs = _startedMs;
            // BEFORE the ask: the window's answer reaches the tick after the tab exists (and is live in the registry), and the
            // threads' step must not post a root of its own for the session meanwhile.
            if (canLaunch)
            {
                _launching.insert(launch.sessionId);
            }
        }
        if (!canLaunch)
        {
            return; // _startWizard asked no step on such a host: not reached
        }
        _postToRoot(rootId, L"<i>Opening a session in</i> <code>" + Msg::TgEscapeHtml(folder) + L"</code>\x2026", {});
        RemoteLaunchRequest req;
        req.kind = RemoteLaunchRequest::Kind::NewSession;
        req.dir = folder;
        req.title = Msg::NewThreadNameForMessage(data.prompt); // the tab's title: the post's first line, as Telegram names a thread
        req.newSessionId = launch.sessionId;
        LogNav(L"remote teams new session " + ShortId(launch.sessionId) + L" dir=" + folder + L" -> asking a window");
        const std::weak_ptr<TeamsService> self = weak_from_this();
        bool asked = false;
        try
        {
            // The answer comes exactly once for a request the engine took, from ANY thread, maybe before this returns
            // (RemoteLaunch.h RemoteLaunchReply); none comes for a request it refused (false).
            asked = _hooks.launchAnswered(req, [self, launch](const RemoteLaunchOutcome& outcome) {
                if (const auto me = self.lock())
                {
                    me->_onLaunchAnswered(launch, outcome);
                }
            });
        }
        catch (...)
        {
            // Rule #18. Recovery: taken as not asked (below): the post is answered and done.
            LogSwallowedException(L"TeamsService::_launchFromWizard (ask)");
        }
        if (!asked)
        {
            {
                std::lock_guard lk{ _mtx };
                _launching.erase(launch.sessionId);
            }
            _postToRoot(rootId, L"No window could open the session: open Agentmaster at the PC, then write a new post.", {});
            LogNav(L"remote teams new session " + ShortId(launch.sessionId) + L" -> no window took it");
        }
    }

    void TeamsService::_onLaunchAnswered(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _launchMtx };
                _launchAnswers.emplace_back(launch, outcome);
            }
            // Without _mtx (the leaf lock above is the only lock this thread takes): the tick's wait reads the flag, and a wake-up
            // lost to the race between its check and this store costs one tick (a second).
            _launchAnswersPending.store(true);
            _cv.notify_all();
        }
        catch (...)
        {
            // Rule #18. Recovery: the answer is lost: an opened session gets no thread in this run (its id stays in _launching)
            // and its first prompt is not queued; the session is there at the PC.
            LogSwallowedException(L"TeamsService::_onLaunchAnswered");
        }
    }

    void TeamsService::_settleLaunchAnswers(int64_t nowMs) noexcept
    {
        try
        {
            // Cleared before the take: an answer queued after it sets the flag again and wakes the next tick.
            if (!_launchAnswersPending.exchange(false))
            {
                return;
            }
            std::vector<std::pair<LaunchAsk, RemoteLaunchOutcome>> answers;
            {
                std::lock_guard lk{ _launchMtx };
                answers.swap(_launchAnswers);
            }
            for (const auto& [launch, outcome] : answers)
            {
                try
                {
                    _settleLaunchAnswer(launch, outcome, nowMs);
                }
                catch (...)
                {
                    // Rule #18. Recovery: this answer is lost (an opened session is there at the PC); the others go on.
                    LogSwallowedException(L"TeamsService::_settleLaunchAnswers (an answer)");
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the answers taken are lost; any left in the queue wait for the next answer's wake-up.
            LogSwallowedException(L"TeamsService::_settleLaunchAnswers");
        }
    }

    void TeamsService::_settleLaunchAnswer(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome, int64_t nowMs)
    {
        bool thisRun = false;
        {
            std::lock_guard lk{ _mtx };
            thisRun = launch.runMs == _startedMs;
            if (thisRun)
            {
                _launching.erase(launch.sessionId);
            }
        }
        if (!thisRun)
        {
            // Asked by a run that has stopped since (the settings changed): this run's state may be another channel's, so nothing
            // is bound; an opened session gets a thread of its own like any other.
            _log(L"[teams] a launch answer came after its run ended (" + std::wstring{ RemoteLaunchResultName(outcome.result) } + L" " +
                 ShortId(launch.kind == LaunchAsk::Kind::NewPost ? launch.sessionId : outcome.sessionId) + L"): nothing bound or said");
            return;
        }
        if (launch.kind == LaunchAsk::Kind::Fork || launch.kind == LaunchAsk::Kind::NewInFolder)
        {
            _settleThreadAsk(launch, outcome); // §8: the asking thread hears how the fork or the new session went
            return;
        }
        if (launch.kind == LaunchAsk::Kind::SearchPick)
        {
            _settleSearchPick(launch, outcome); // §9.3: the Search thread hears how the resume went
            return;
        }
        if (!RemoteLaunchOpened(outcome))
        {
            _postToRoot(launch.rootId, L"The new session did not start. " + Msg::TgEscapeHtml(RemoteLaunchRefusalText(outcome)) + L" Write a new post to try again.", {});
            LogNav(L"remote teams new session " + ShortId(launch.sessionId) + L" -> " + RemoteLaunchResultName(outcome.result));
            return;
        }
        // The session the tab hosts: the id asked for (a new session takes the requester's id), the answer's when it differs.
        const std::wstring sessionId = outcome.sessionId.empty() ? launch.sessionId : outcome.sessionId;
        bool bound = false;
        {
            std::lock_guard lk{ _mtx };
            _launching.erase(sessionId);
            if (_state.threads.count(sessionId) == 0)
            {
                // The owner's post becomes the session's thread: the card replaces its words at the next pass (cardHash 0: never
                // edited) and keeps them as the first prompt; a new session has no history to replay; the replies the step read
                // (the typed folder among them) are not prompts.
                Thread t;
                t.sessionId = sessionId;
                t.rootId = launch.rootId;
                t.webUrl = launch.webUrl;
                t.createdMs = nowMs;
                t.historyDone = true;
                t.lastReplyMs = launch.readToMs;
                t.firstPrompt = launch.prompt;
                _state.threads[sessionId] = std::move(t);
                _stateDirty = true;
                bound = true;
            }
        }
        LogNav(L"remote teams new session " + ShortId(sessionId) + L" -> opened" + (bound ? L"; the post is its thread" : L"; it has a thread already"));
        _log(L"[teams] new session " + ShortId(sessionId) + L" opened in " + launch.folder + L" for a post of the owner");
        // The post's words are the first prompt: no status emoji (the post is the card now) and no ❌ watch.
        _queueTeamsPrompt(sessionId, launch.rootId, L"", launch.prompt);
    }
}
