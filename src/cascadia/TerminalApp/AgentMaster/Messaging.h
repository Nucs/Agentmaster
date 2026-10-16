// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging: the fleet in Telegram (MESSAGING.md). The bot runs INSIDE the Agentmaster
// process (D1): this service is FleetLink + the messaging hub + the Telegram connector of the design,
// one per process, created by SharedEngine() and driven by the Settings cog's MESSAGING tab.
//
// What it does, in one paragraph: it keeps one Telegram TOPIC in the bot's private chat for every
// Claude Code tab of every window whose session is not Idle (a pinned session is never idle), with the
// tab's title and color (§5); in each topic it shows the session as it happens — the prompts, ONE
// work-log message per turn edited in place with the tool calls and remarks, the final reply with sound
// and everything else silent (§6); a message typed in a topic is QUEUED as the session's next prompt,
// never a steer, with its status as a reaction on the message (§7.1); `/commands` run the tab's actions
// or go to Claude Code, except the ones that leave no record (§7.2–§7.3); a pinned "Search" topic hosts
// the Sessions page's search agent and brings old sessions back into tabs (§5.4).
//
// Threads (H1 — nothing here ever runs on a UI thread):
//   - the POLL thread: getUpdates long polls; every update is handled on it (the sender is checked
//     first — H3 — and anything from someone but the paired owner is dropped);
//   - the SEND thread: the outbox, paced to Telegram's one-message-per-second-per-chat (§6.6);
//   - the TICK thread (every 1.5 s, or when the registry notifies): the topic sync, the transcript
//     mirror, the cards, the state file, the run marker.
// Every thread body and callback is wrapped (Rule #18); an exception never crosses into the engine.
//
// Window-hosted actions (start a dormant tab, read its screen, mark unread, resume / fork / open a
// session into a tab) go through ServiceHooks, which Engine.cpp wires to the per-window sinks.
//
// Plain C++ (no WinRT, no PCH); the m5 harness compiles Messaging.cpp + Messaging.Inbound.cpp +
// TelegramApi.cpp as LINK dependencies of Engine.cpp and tests the pure core (MessagingModels.h).
// The Search agent is reached through SearchBridge: the implementation that wraps SessionSearchAgent
// (MessagingSearchBridge.cpp) is app-only, because the agent needs the SDK unity file the harness
// does not build; with no factory set, the Search topic answers that search is not available.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MessagingModels.h"
#include "RemoteLaunch.h"
#include "SessionModels.h" // AppSettings, SessionInfo
#include "TelegramApi.h" // TelegramApi + TgResult (the outbox's completion callbacks name TgResult)

namespace Agentmaster
{
    class SessionRegistry;
    class Scheduler;
}

namespace Agentmaster::Msg
{

    // ===== the Search agent, behind a seam ====================================================

    // One session a search found, as the Search topic lists it.
    struct SearchRow
    {
        std::wstring sessionId;
        std::wstring title; // the best title the agent knows (live tab title, custom, ai, first prompt)
        std::wstring dir; // the session's working directory ("" when unknown)
        int64_t lastActivityMs{ 0 };
    };

    // What the Search agent reports back.
    struct SearchBridgeEvent
    {
        enum class Kind
        {
            Searching, // the model runs a search (text describes it)
            Shown, // the model chose these sessions (rows)
            Answer, // the request is answered (text = the model's sentence); rows = the last Shown
            Failed, // the request failed (text says why)
        };
        Kind kind{ Kind::Answer };
        uint64_t turn{ 0 };
        std::wstring text;
        std::vector<SearchRow> rows;
    };

    // The agent as the service sees it. Every method may be called from the service's threads.
    struct SearchBridge
    {
        virtual ~SearchBridge() = default;
        // Ask; the answer comes through the sink the factory was given. Returns the turn id (0 = ignored).
        virtual uint64_t Ask(std::wstring text) = 0;
        // Throw the conversation away (the Search topic's /reset).
        virtual void Reset() = 0;
        // Free the index and end the idle process after `delayMs` (nothing was asked for a while).
        virtual void Park(int64_t delayMs) = 0;
    };

    // Makes the agent on first use. thinkingWord = "off" … "max" (AppSettings::messagingSearchThinking).
    using SearchBridgeFactory = std::function<std::unique_ptr<SearchBridge>(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord)>;

    // ===== what the service needs from the app ================================================

    // Each member may be empty (the test harness): the service then answers "not available".
    struct ServiceHooks
    {
        // A window-hosted action on a session's tab: "start" (start a dormant tab's claude in place, no
        // focus), "screen" (reply with the terminal's visible text), "markunread", "refresh-tags" (repaint
        // the tab's tag badges after a remote tag edit), "refresh-favorite". `reply` is called by the
        // hosting window with the action's text answer (may be empty); a window that does not host the
        // session calls nothing.
        std::function<void(const std::wstring& sessionId, const std::wstring& action, const std::wstring& arg, std::function<void(std::wstring)> reply)> sessionAction;
        // Open a session into a tab in the most recently active window (RemoteLaunch.h).
        std::function<bool(const RemoteLaunchRequest&)> launch;
        // The existing cross-window seams: restart a session's connection in place; park its tab.
        std::function<void(const std::wstring& sessionId)> restartSession;
        std::function<void(const std::wstring& sessionId)> deactivateSession;
        // The app's version word for the start note ("dev 0.0.1.9"); "" = not shown.
        std::function<std::wstring()> versionWord;
        // How many open windows have not finished their first-layout restore (Engine::WindowsStillRestoring).
        // While it is above 0, a session that reads live=false may only be waiting for its tab to be re-homed,
        // so the service holds its "closed" topic deletes (the START-UP HOLD, §21.4). Empty = 0.
        std::function<size_t()> windowsRestoring;
    };

    // ===== the service ========================================================================

    class MessagingService : public std::enable_shared_from_this<MessagingService>
    {
    public:
        /// <summary>Creates the service stopped. Apply() starts it when the settings ask for it.</summary>
        /// <param name="registry">The fleet (the topics' input, the queue the prompts go into).</param>
        /// <param name="scheduler">The runner (Pause all, the advance request after a queued prompt).</param>
        /// <param name="profileDir">The active profile: the state file lives in <profile>\messaging\telegram\.</param>
        /// <param name="hooks">The window-hosted seams (ServiceHooks); members may be empty.</param>
        /// <exception cref="std::bad_alloc">If the state cannot be allocated.</exception>
        MessagingService(std::shared_ptr<SessionRegistry> registry, std::shared_ptr<Scheduler> scheduler, std::wstring profileDir, ServiceHooks hooks);
        /// <summary>Stop(). Never throws.</summary>
        ~MessagingService();
        MessagingService(const MessagingService&) = delete;
        MessagingService& operator=(const MessagingService&) = delete;

        /// <summary>
        /// Applies the settings: messagingEnabled with a stored token and a paired owner starts the three
        /// threads (idempotent); disabled, or no token, or no owner stops them. The idle grace and the
        /// Search thinking level apply from the next tick / the next Search process. Called at engine init
        /// and on every Settings save. Never throws: a thread that cannot start is logged and the status
        /// says so.
        /// </summary>
        /// <param name="settings">The settings to apply.</param>
        void Apply(const AppSettings& settings) noexcept;

        /// <summary>
        /// Stops the threads and the long poll (TelegramApi::Abort), flushes nothing that is still queued
        /// (the outbox is in memory; Telegram keeps the user's side 24 h). Idempotent. Never throws.
        /// </summary>
        void Stop() noexcept;

        /// <summary>Is the connector running (threads up)? Any thread.</summary>
        bool Running() const noexcept;

        /// <summary>
        /// One line for the Settings cog: connected as @bot with N topics, waiting for Threaded Mode, no
        /// token, nobody paired, stopped because another instance polls the bot, … Any thread.
        /// </summary>
        /// <returns>The status text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be copied.</exception>
        std::wstring StatusText() const;

        /// <summary>
        /// Sets the factory that makes the Search topic's agent on first use. Called once at engine init by
        /// the app layer (the harness never sets one: the Search topic then says search is not available).
        /// </summary>
        /// <param name="factory">The factory; an empty one removes the current factory.</param>
        void SetSearchFactory(SearchBridgeFactory factory) noexcept;

        // ----- the bot token (Windows Credential Manager; never in a profile file — M4) -----

        /// <summary>The Credential Manager target of a profile's bot token: "Agentmaster:messaging:telegram:<leaf>".</summary>
        /// <param name="profileDir">The profile directory; its last path segment is the key (two installs never share a bot).</param>
        /// <returns>The target name.</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
        static std::wstring TokenCredentialTarget(std::wstring_view profileDir);
        /// <summary>Reads the stored token.</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <returns>The token, or "" when none is stored or Credential Manager refused the read.</returns>
        static std::wstring ReadStoredToken(std::wstring_view profileDir) noexcept;
        /// <summary>Stores (replaces) the token. The cog's Save calls it with a typed token.</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <param name="token">The token; whitespace trimmed; an empty token is refused (use ClearToken).</param>
        /// <returns>True on success; false (logged) when Credential Manager refused.</returns>
        static bool StoreToken(std::wstring_view profileDir, std::wstring_view token) noexcept;
        /// <summary>Removes the stored token (the cog's "Forget token").</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <returns>True when removed or absent; false (logged) on a refusal.</returns>
        static bool ClearToken(std::wstring_view profileDir) noexcept;

        // The result of a getMe probe (the cog's "Check bot" button, and the service's own start).
        struct BotCheck
        {
            bool ok{ false };
            std::wstring username; // "@SidKatrineBot"
            bool topicsEnabled{ false }; // User.has_topics_enabled — Threaded Mode on in @BotFather
            bool usersMayCreateTopics{ false }; // User.allows_users_to_create_topics
            std::wstring error; // when !ok
        };
        /// <summary>Calls getMe with a token (a bounded network round trip — call it off the UI thread).</summary>
        /// <param name="token">The token to check.</param>
        /// <returns>The bot's facts, or an error text.</returns>
        static BotCheck CheckBot(std::wstring_view token) noexcept;

    private:
        // ----- lifecycle (Messaging.cpp) -----
        void _start(std::wstring token) noexcept;
        void _pollLoop() noexcept;
        void _sendLoop() noexcept;
        void _tickLoop() noexcept;
        void _onRegistryNotify(const SessionInfo& s) noexcept;
        void _setStatus(std::wstring text);
        void _log(const std::wstring& line) const noexcept;
        int64_t _nowMs() const noexcept;

        // ----- the state file -----
        void _loadState();
        void _saveStateIfDirty(bool force);

        // ----- the outbox -----
        struct OutItem
        {
            SendPriority prio{ SendPriority::New };
            uint64_t seq{ 0 };
            std::wstring key; // non-empty: a newer item with the same key replaces this one (edits)
            std::wstring method;
            json::Value params;
            bool document{ false };
            std::wstring fileName;
            std::string fileBytes;
            bool paced{ true }; // counts against the one-message-per-second budget
            std::function<void(const TgResult&)> onDone; // called on the send thread (may be empty)
        };
        void _enqueue(OutItem item);
        void _sendText(int64_t threadId, const std::wstring& html, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId = 0);
        void _editText(int64_t messageId, const std::wstring& html, SendPriority prio, const std::wstring& key, json::Value replyMarkup);
        void _react(int64_t messageId, std::wstring_view emoji);
        void _sendDocument(int64_t threadId, std::wstring_view fileName, std::string bytes, const std::wstring& caption, SendPriority prio);
        json::Value _buttons(const std::vector<std::vector<std::pair<std::wstring, std::wstring>>>& rows) const;
        std::wstring _cardMarkupFor(const std::wstring& sessionId, bool pinned, AutorunnerMode mode, json::Value& markupOut) const;

        // ----- topics = tabs (Messaging.cpp) -----
        void _ensureSearchTopic();
        void _syncTopics();
        void _createTopic(const std::wstring& sessionId, const std::wstring& title, const std::wstring& colorHex);
        void _deleteTopic(const std::wstring& sessionId, const std::wstring& reason);
        void _renameTopic(const std::wstring& sessionId, const std::wstring& title);
        void _refreshCard(const std::wstring& sessionId, bool force);
        std::optional<SessionInfo> _session(const std::wstring& sessionId) const;

        // ----- the mirror (Messaging.cpp) -----
        struct Mirror
        {
            std::wstring pendingText; // assistant text not yet known to be a remark or the final reply
            std::wstring workLog; // the current turn's work-log text (plain, before escaping)
            int64_t workLogMessageId{ 0 };
            int64_t workLogLastEditMs{ 0 };
            bool workLogDirty{ false };
            bool turnOpen{ false };
            SessionState lastState{ SessionState::Idle };
            int64_t lastStateMs{ 0 };
            std::wstring lastCardText;
            int64_t lastCardEditMs{ 0 };
            std::unordered_map<std::wstring, std::wstring> toolNames; // tool_use id -> name (to label a failed result)
            int64_t lastTranscriptCheckMs{ 0 };
            int64_t lastResolveMs{ 0 };
        };
        void _followTranscript(const std::wstring& sessionId, TopicRecord& topic, Mirror& m);
        void _applyItems(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::vector<TopicItem>& items, const SessionInfo& s);
        void _flushWorkLog(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, bool force);
        void _postFinal(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::wstring& html, const std::wstring& why);
        void _appendWorkLog(Mirror& m, const std::wstring& line);
        void _onStateEdge(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s);
        void _syncReactions(const SessionInfo& s);

        // ----- inbound (Messaging.Inbound.cpp) -----
        void _handleUpdate(const json::Value& update) noexcept;
        void _handleMessage(const json::Value& msg, bool edited);
        void _handleCallback(const json::Value& cq);
        void _handleTopicMessage(const std::wstring& sessionId, const json::Value& msg, int64_t threadId);
        void _handleSearchMessage(const json::Value& msg, int64_t threadId);
        bool _queueRemotePrompt(const std::wstring& sessionId, const std::wstring& text, int64_t messageId, int64_t threadId, bool fromBacklog);
        void _runSessionCommand(const std::wstring& sessionId, const RemoteCommand& cmd, const json::Value& msg, int64_t threadId);
        void _runSearchCommand(const RemoteCommand& cmd, const json::Value& msg, int64_t threadId);
        void _reply(int64_t threadId, const std::wstring& html, bool sound = false, json::Value markup = json::Value{}, int64_t replyTo = 0);
        void _sendHelp(int64_t threadId, bool searchTopic);
        std::wstring _tokenFor(std::wstring kind, std::wstring sessionId, std::wstring payload, int64_t ttlMs);
        // An action token's lifetime in hours as milliseconds (the backlog question lives 2 h, a confirm 1 h).
        static int64_t kTokenTtlMsFor(int hours) noexcept;
        void _sweepTokens();
        void _sendnow(const std::wstring& sessionId, int64_t threadId);
        void _wake(const std::wstring& sessionId, bool pin);
        void _setPinned(const std::wstring& sessionId, bool pinned);
        void _requestLaunch(RemoteLaunchRequest req, int64_t threadId);
        void _sweepRevives(const SessionInfo& s);
        void _askSearch(const std::wstring& text, int64_t threadId);
        void _onSearchEvent(const SearchBridgeEvent& ev) noexcept;
        void _postSearchResults(const SearchBridgeEvent& ev);
        void _appNote(const std::wstring& html, bool sound);

        // ----- data -----
        std::shared_ptr<SessionRegistry> _registry;
        std::shared_ptr<Scheduler> _scheduler;
        std::wstring _profile;
        ServiceHooks _hooks;

        mutable std::mutex _mtx; // guards everything below not marked atomic; never held across a network call
        std::condition_variable _tickCv;
        std::condition_variable _sendCv;
        std::atomic<bool> _stop{ false };
        std::atomic<bool> _running{ false };
        std::thread _pollThread;
        std::thread _sendThread;
        std::thread _tickThread;
        std::unique_ptr<TelegramApi> _api;
        void* _tokenMutex{ nullptr }; // the machine-wide one-poller-per-token mutex (M7); HANDLE as void*

        int64_t _ownerId{ 0 }; // the paired Telegram user (AppSettings::messagingTelegramOwnerId)
        std::atomic<int64_t> _idleGraceMs{ 10 * 60 * 1000 };
        std::wstring _searchThinkingWord{ L"high" };
        std::atomic<bool> _topicsEnabled{ false };
        int64_t _lastGetMeMs{ 0 };
        /// <summary>
        /// The bot's own Telegram user id, from getMe; 0 until the first getMe of this run has answered. It is the
        /// poll loop's IDENTITY GATE (no getUpdates before the bot is known, so a state file written for ANOTHER
        /// bot can be reset first — update ids are per bot) and the sender test that keeps the bot's own service
        /// messages (the topics it creates, the cards it pins) out of the "not the paired owner" log.
        /// </summary>
        std::atomic<int64_t> _botId{ 0 };
        std::wstring _botUsername;
        std::wstring _status;
        std::wstring _stateFile;
        MessagingState _state;
        bool _stateDirty{ false };
        int64_t _lastStateSaveMs{ 0 };
        uint64_t _registryObserver{ 0 };
        bool _tickRequested{ false };
        int64_t _lastMarkerMs{ 0 };
        bool _startNotePosted{ false };
        /// <summary>
        /// How many mirrored sessions have a turn open (counted by the tick loop each pass). The work log's edit
        /// gap grows with it (`kWorkLogEditGapMs` × 1 at one turn, ~2 s × N at N turns), so N busy sessions never
        /// ask for more than ~0.5 edit/s between them: the chat's whole budget is ~1 call/s (kSendGapMs).
        /// </summary>
        std::atomic<size_t> _openTurns{ 0 };
        /// <summary>
        /// Outbound API calls since the last traffic line, by method (plus "429" for pacing refusals), under `_mtx`;
        /// the tick loop logs and clears them every `kTrafficLogMs` (`[msg] traffic (10 min): …`), the one place the
        /// chat's real call rate can be read — Telegram's flood limit is per chat and its penalty was 21 minutes.
        /// </summary>
        std::map<std::wstring, uint32_t> _trafficByMethod;
        int64_t _lastTrafficLogMs{ 0 };
        /// <summary>
        /// The START-UP HOLD on "closed" topic deletes (MESSAGING.md §21.4). `_startedMs` is when this run's
        /// service started; while the start is younger than `kStartupCloseHoldMinMs`, and while any registered
        /// window is still restoring (`ServiceHooks::windowsRestoring`, capped at `kStartupCloseHoldCapMs`), a topic
        /// whose session has not yet been seen live in this run is kept, because at an app start every session
        /// reads live=false until its window re-homes its tabs — without the hold the first tick after a swap
        /// deleted all 47 topics and the next minute re-created 38; a fixed 120 s hold still churned the 16 tabs
        /// re-homed after it (2026-10-16). `_seenLive` holds the ids seen live inside the hold (tick thread only,
        /// cleared under `_mtx` at _start and dropped when the hold ends); `_startupHoldLogged` makes the "N
        /// topic(s) kept" line one per start, `_startupHoldOpen` the "hold ended" line.
        /// </summary>
        int64_t _startedMs{ 0 };
        std::unordered_set<std::wstring> _seenLive;
        bool _startupHoldLogged{ false };
        bool _startupHoldOpen{ false };

        std::deque<OutItem> _outbox;
        uint64_t _outSeq{ 0 };
        int64_t _lastSendMs{ 0 };

        std::unordered_map<std::wstring, Mirror> _mirrors;
        std::unordered_map<std::wstring, PromptStatus> _reacted; // prompt id -> the status its Telegram message shows
        std::unordered_map<std::wstring, int64_t> _dirty; // session ids the registry notified since the last tick (-> ms)

        struct Token
        {
            std::wstring kind;
            std::wstring sessionId;
            std::wstring payload;
            int64_t expiresMs{ 0 };
        };
        std::unordered_map<std::wstring, Token> _tokens;
        uint64_t _tokenSeq{ 0 };

        struct Revive
        {
            RemoteLaunchRequest req;
            int64_t requestedMs{ 0 };
            int64_t threadId{ 0 }; // where to report
        };
        std::vector<Revive> _revives;

        SearchBridgeFactory _searchFactory;
        std::unique_ptr<SearchBridge> _search;
        std::vector<SearchRow> _searchRows; // the last Shown
        int64_t _searchLastUsedMs{ 0 };
        bool _searchParked{ true };
    };
}
