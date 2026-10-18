// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging: the fleet in Telegram (MESSAGING.md). The bot runs INSIDE the Agentmaster
// process (D1): this service is FleetLink + the messaging hub + the Telegram connector of the design,
// one per process, created by SharedEngine() and driven by the Settings cog's MESSAGING tab.
//
// What it does, in one paragraph: it keeps one Telegram TOPIC in the bot's private chat for every
// Claude Code TAB of every window whose session is not idle — a new tab (a fresh session, a fork) is
// listed at once, before its first message, and a topic follows its tab to the next conversation (a
// /clear, a /resume, a handover in place) (§5, §21.6) — with the tab's title and color; in each topic it
// shows the conversation as it happens — the prompts, the assistant's remarks (why a tool runs), a
// work-log message per stretch of tool calls, the final reply with sound and everything else silent —
// under a STICKY card that stays the topic's last message and says the state, with Claude Code's own
// recap while it is fresh (§6, §21.5, §21.6); a message typed in a topic is QUEUED as the session's next
// prompt, never a steer, with its status as a reaction on the message (§7.1); `/commands` run the tab's
// actions or go to Claude Code, except the ones that leave no record (§7.2–§7.3); a pinned "Search"
// topic hosts the Sessions page's search agent and brings old sessions back into tabs (§5.4).
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

#include "MessagingDirectory.h" // the pinned directory's model and renders (§21.16)
#include "MessagingStartNote.h" // the start note's facts, kind, sound and text (§21.18)
#include "MessagingModels.h"
#include "MessagingNewThread.h" // §21.17: the new-thread folder picker (lists, text, buttons, callback data)
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
        // §21.10 (the evidence cards): what a card and the pick menu show beyond the title.
        std::wstring why; // the model's one line of evidence for this session ("" = it gave none)
        std::wstring branch; // the git branch the session ran on ("" when unknown)
        int64_t createdMs{ 0 }; // when the conversation started (0 when unknown)
        int prompts{ 0 }; // the prompts the user typed in it
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

    // The agent as the service sees it. Every method may be called from the service's threads. Behind it is the
    // process's ONE telegram-search conversation (§21.7): a bridge only attaches the service's sink to it, so a stop and
    // a start of the bot make a new bridge to the same conversation, and destroying a bridge never ends it.
    struct SearchBridge
    {
        virtual ~SearchBridge() = default;
        // Ask; the answer comes through the sink the factory was given. Returns the turn id (0 = ignored).
        virtual uint64_t Ask(std::wstring text) = 0;
        // Throw the conversation away (the Search topic's /clear, /reset and ⊘ Clear): the context, the record and the
        // shown list; the next request starts a fresh conversation. Works on a parked conversation too.
        virtual void Reset() = 0;
        // Free the index and end the idle process after `delayMs` (nothing was asked for a while). The conversation is
        // kept: the next request continues it from its record.
        virtual void Park(int64_t delayMs) = 0;
        // How much the model thinks from the next request on ("off" … "max"; an unknown word reads as high, the Search
        // topic's default). It changes the RUNNING conversation (SessionSearchAgent::SetThinkingLevel), so the cog's
        // level reaches the one process-wide conversation without throwing its context away. Never throws.
        virtual void SetThinkingLevel(std::wstring word) noexcept = 0;
    };

    // Makes a bridge to the process's one Search conversation (the conversation itself is made on first use and never
    // destroyed). thinkingWord = "off" … "max" (AppSettings::messagingSearchThinking), applied to the conversation.
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
        // Open a session into a tab in the most recently active window (RemoteLaunch.h): true when a window took the request.
        // §21.23: a host that sets launchAnswered (below) may leave this empty — the service's constructor then builds it on top
        // of launchAnswered (answers dropped), for the code that only asks whether launching is available. The service's own
        // launches go through _askLaunch, which uses launchAnswered whenever it is set, so their answers reach the service.
        std::function<bool(const RemoteLaunchRequest&)> launch;
        // §21.23: the ANSWERED launch (Engine.h RequestLaunchInWindow): true when a window took the request — the reply then
        // comes exactly once, from any thread, possibly before this returns, with the session the tab hosts or why nothing
        // opened; false when no window could take it (no reply follows).
        std::function<bool(const RemoteLaunchRequest&, RemoteLaunchReply)> launchAnswered;
        // The existing cross-window seams: restart a session's connection in place; park its tab.
        std::function<void(const std::wstring& sessionId)> restartSession;
        std::function<void(const std::wstring& sessionId)> deactivateSession;
        // The app's version word for the start note and the run marker (§21.18): "0.6.8.21", "dev 0.0.1.28", "portable
        // 0.7.9.0" (MessagingStartNote.h FormatAppVersionWord; Engine.cpp reads the package identity or the portable stamp).
        // Asked once per start of the bot. Empty, or "", = the note shows no version and the marker carries none.
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
        /// The app is closing: its LAST window just closed (Engine.cpp, UnregisterLiveWindowIn). Writes
        /// &lt;profile&gt;\messaging\run.closed with the time, so the next start's note says the run CLOSED instead of "did not
        /// close normally" (MessagingStartNote.h, §21.18). The app ends with TerminateProcess and the engine is never
        /// destroyed, so Stop() never runs at a quit: this is the only moment a clean end can be recorded. A stamp written
        /// while the app goes on (a window set that was empty for a moment) is outdated by the next minute's marker write
        /// and then proves nothing (StartNoteRunClosedCleanly). Does nothing while the bot is not running — there is no
        /// marker of this run for the stamp to be compared with. From the stamp on, the tick does not rewrite the run marker
        /// for kAppClosingMarkerHoldMs (Messaging.cpp): a marker written between the stamp and the process's end would make
        /// a clean quit read as a crash. Any thread; takes none of the service's locks, only the log's (it runs under the
        /// engine's window mutex). Never throws: a failed write is logged, and the next note then reads the end as
        /// unexpected — the safe side, since a false "it closed" would hide a crash.
        /// </summary>
        void NoteAppClosing() noexcept;

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

        // ----- the SECRETARY bot's token (§21.11; optional; Credential Manager like the topics bot's) -----
        // A second bot, connected to the user's account under Telegram → Settings → Chat Automation, sends the user's
        // turns into the topics bot's chat AS THE USER. Telegram documents exactly this ("a bot connected to a business
        // account … can send messages to other bots used by that business account"; the sending bot needs Bot-to-Bot
        // Communication Mode); a bot posting into its OWN chat on the user's behalf is not documented.

        /// <summary>The Credential Manager target of a profile's secretary bot token: "Agentmaster:messaging:telegram-secretary:&lt;leaf&gt;".</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <returns>The target name.</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
        static std::wstring SecretaryTokenCredentialTarget(std::wstring_view profileDir);
        /// <summary>Reads the stored secretary bot token.</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <returns>The token, or "" when none is stored or Credential Manager refused the read.</returns>
        static std::wstring ReadStoredSecretaryToken(std::wstring_view profileDir) noexcept;
        /// <summary>Stores (replaces) the secretary bot token; a running service restarts its bots at the next Apply.</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <param name="token">The token; whitespace trimmed; an empty token is refused (use ClearSecretaryToken).</param>
        /// <returns>True on success; false (logged) when Credential Manager refused.</returns>
        static bool StoreSecretaryToken(std::wstring_view profileDir, std::wstring_view token) noexcept;
        /// <summary>Removes the stored secretary bot token (the cog's "Forget").</summary>
        /// <param name="profileDir">The profile directory.</param>
        /// <returns>True when removed or absent; false (logged) on a refusal.</returns>
        static bool ClearSecretaryToken(std::wstring_view profileDir) noexcept;

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
        /// <summary>
        /// The SECRETARY bot's poll loop (§21.11; its own thread, only when a secretary token is stored): getMe once (the
        /// bot's id — update ids are per bot — and whether Secretary Mode is on for it), then getUpdates for
        /// `business_connection` ONLY (allowed_updates: a business bot would otherwise get every message of the chats it
        /// may access). Each connection update goes to _handleBusinessConnection as the secretary's. A 401 or a 409 ends the
        /// loop (logged; secretary sends stay off). Never throws (Rule #18 inside).
        /// </summary>
        void _secretaryPollLoop() noexcept;
        void _sendLoop() noexcept;
        void _tickLoop() noexcept;
        /// <summary>
        /// §21.23: hands `req` to a window (the host's launchAnswered, else its unanswered launch) under `token`, the key its
        /// ANSWER comes back with (_onLaunchAnswered → _settleLaunchAnswers): the caller records its revive or bind with that
        /// token BEFORE it asks, because the answer may come back from another thread before this returns. Takes no lock, and
        /// the answer takes only the answer queue's own; the caller must not hold `_mtx` (no caller does).
        /// </summary>
        /// <param name="req">What to open.</param>
        /// <param name="token">The key the caller recorded its revive or bind under (from _launchTokenSeq).</param>
        /// <returns>True when a window took the request (an answer follows when the host answers); false when none could, or
        /// the host has no launch at all.</returns>
        /// <exception cref="std::bad_alloc">If the request or the reply cannot be copied.</exception>
        bool _askLaunch(const RemoteLaunchRequest& req, uint64_t token);
        /// <summary>
        /// §21.23: a window's answer to the launch recorded under `token` — queued for the tick (_launchAnswers, behind its own
        /// leaf lock) and the tick woken, nothing more: this runs on whichever thread the answer comes from (the window's UI
        /// thread, or the engine's when the window dropped the request), possibly inside _askLaunch's own call. Never throws
        /// (Rule #18 inside: a lost answer leaves the revive or bind to its timeout).
        /// </summary>
        /// <param name="token">The launch's key.</param>
        /// <param name="outcome">The window's answer (RemoteLaunch.h).</param>
        void _onLaunchAnswered(uint64_t token, const RemoteLaunchOutcome& outcome) noexcept;
        /// <summary>
        /// §21.23, tick thread, at the top of every tick: applies the queued launch answers. A new thread's bind that the window
        /// opened is marked answered (the tick binds it once its session is live, _adoptBoundThreads); a refused one leaves at
        /// once — its thread goes back to its folder picker with the messages it held, and the thread is told why. A revive is
        /// told the session the window opened (_sweepRevives settles on exactly that one); a refused revive leaves, and its
        /// thread is told why — after _requestLaunch's own "… into a tab…" line (Revive::announced), so the two never swap
        /// places. Never throws (Rule #18 inside).
        /// </summary>
        void _settleLaunchAnswers() noexcept;
        /// <summary>
        /// §21.23, tick thread, once per tick: the revives that waited longer than kRemoteLaunchTimeoutMs leave, each with a line
        /// in its thread — the PC never answered, or the session it opened never came up. Before §21.23 _sweepRevives dropped
        /// them without a word. Never throws (Rule #18 inside).
        /// </summary>
        void _expireRevives() noexcept;
        void _onRegistryNotify(const SessionInfo& s) noexcept;
        void _setStatus(std::wstring text);
        void _log(const std::wstring& line) const noexcept;
        int64_t _nowMs() const noexcept;

        // ----- the state file -----
        void _loadState();
        void _saveStateIfDirty(bool force);

        // ----- the outbox -----
        /// <summary>
        /// §21.12: a turn's PROMPT message as its send answers it — the bot's prompt line (its id from sendMessage, `botLine`
        /// true: its status is an edit of the line), or the user's own message (a prompt sent from the phone: its id is known
        /// at once; one sent as the user by the secretary bot: its id in THIS bot's view comes with the echo, matched by
        /// `echoHash`; `botLine` false: its status is a reaction). An OBJECT shared by the mirror, the send's callback and the
        /// final reply's pre-send hook: the tick works on a COPY of a session's mirror and writes it back, so a field written
        /// by a callback meanwhile would be lost — an object behind a shared pointer is the same in both copies. 0 = not known.
        /// </summary>
        struct PromptRef
        {
            std::atomic<int64_t> id{ 0 };
            std::atomic<bool> botLine{ false };
            std::atomic<uint32_t> echoHash{ 0 };
        };
        /// <summary>
        /// Points `params` at the turn's prompt (reply_parameters, allow_sending_without_reply) when its message id is known by
        /// now and the item names no other reply; otherwise leaves it (the reply goes without a quote). The outbox's pre-send
        /// hook of a final reply (OutItem::beforeSend).
        /// </summary>
        static void _applyReplyRef(json::Value& params, const PromptRef& ref);
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
            /// <summary>
            /// The order WITHIN one sequence number (the outbox serves by priority, then seq, then subSeq): the chunks of
            /// one message share its seq, and a chunk's fallback (a refused rich part sent again as HTML, a refused HTML
            /// sent again as plain text) takes the slot right after the refused one — so a fallback lands where the
            /// refused message would have, never after the next message.
            /// </summary>
            uint32_t subSeq{ 0 };
            /// <summary>Sent with the SECRETARY bot's client (§21.11): a send as the user through its business connection.</summary>
            bool asSecretary{ false };
            /// <summary>
            /// §21.24: with `document`, the file goes up as a PHOTO (sendPhoto, the multipart field "photo") instead of a document:
            /// the card's 🖼 Screen Image, which the apps show inline and zoomable. Telegram recompresses a photo, so a refused
            /// photo is sent again as a document by the item's onDone (_sendPhoto).
            /// </summary>
            bool photo{ false };
            /// <summary>
            /// Called on the send thread right before the call (may be empty): a final reply sets its reply_parameters here, from
            /// the turn's PromptRef — its prompt was queued before it and has answered with its id by the time this item goes
            /// (the send loop makes one call at a time, in order; §21.12).
            /// </summary>
            std::function<void(json::Value&)> beforeSend;
        };
        /// <summary>
        /// The Bot API method an outbox item calls, as the traffic census and the flood parking name it: "sendDocument" for a
        /// document upload (its `method` field is not what goes on the wire), else the item's `method`.
        /// </summary>
        /// <param name="item">The outbox item.</param>
        /// <returns>The method name.</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be copied.</exception>
        static std::wstring _outItemMethod(const OutItem& item)
        {
            return item.document ? std::wstring{ item.photo ? L"sendPhoto" : L"sendDocument" } : item.method;
        }
        /// <summary>Queues an item; an item with no seq yet (0) gets the next one, an item that carries one keeps it (a fallback, a chunk).</summary>
        void _enqueue(OutItem item);
        /// <summary>The next outbox sequence number (for a message whose parts and fallbacks must keep one place). Takes `_mtx`.</summary>
        uint64_t _takeOutSeq();
        /// <summary>
        /// The sendMessage items of one HTML text (chunked at Telegram's limit): the buttons and `onDone` ride the LAST chunk,
        /// the reply reference the first, only the last may ring. Chunk i sorts at (`seq`, `subSeqBase` + 2i); a chunk
        /// Telegram refuses as HTML ("can't parse entities") is sent again as plain text in the slot right after it.
        /// </summary>
        std::vector<OutItem> _htmlItems(int64_t threadId, const std::wstring& html, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, uint64_t seq, uint32_t subSeqBase, std::shared_ptr<PromptRef> replyRef = nullptr);
        /// <summary>
        /// Posts HTML into a topic (chunked at Telegram's limit; the buttons and `onDone` ride the LAST chunk, the
        /// reply reference the first; a refused HTML is re-sent as plain text). `movesCard` (default) stamps the
        /// topic's `lastPostedMs`, which makes the sticky card re-post itself below the new message; the card's own
        /// posts pass false, or the card would chase itself forever.
        /// </summary>
        /// <param name="replyRef">§21.12: reply to the turn's prompt once its message id is known (the first chunk only;
        /// `replyToMessageId` wins when both are given). May be empty.</param>
        /// <remarks>
        /// §21.15: a text that names a session carries TITLE TOKENS (TgTitleToken). While title chips are wanted
        /// (_titleChipsWanted) and the rich form fits Telegram's rich limit, it goes as ONE rich message whose titles are chips
        /// (_sendTitledRich); otherwise — and as that message's fallback — its classic render (TgRenderTitleTokensClassic: the
        /// §21.13 squares) goes the HTML way described above. A text without tokens goes the HTML way exactly as before.
        /// </remarks>
        void _sendText(int64_t threadId, const std::wstring& html, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId = 0, bool movesCard = true, std::shared_ptr<PromptRef> replyRef = nullptr);
        /// <summary>
        /// Whether a message that names a session goes as a RICH message whose titles are CHIPS (§21.15): the cog's rich switch
        /// (AppSettings::messagingRichMessages, the same switch as Claude's rich Markdown) is on, and this run has not found the
        /// Bot API without sendRichMessage (a 404 latches _richUnavailable). Any thread (two atomics).
        /// </summary>
        /// <returns>True when titled messages go rich.</returns>
        bool _titleChipsWanted() const noexcept;
        /// <summary>
        /// Queues ONE rich message whose titles are chips (§21.15). `classicHtml` is the same message's classic render: when
        /// Telegram refuses the rich message (RichRefusalWantsFallback — a 400 about the message, or a 404 that latches
        /// _richUnavailable for the run), the classic render goes in its place (the same outbox sequence number, so it lands
        /// where the rich message would have) with the same buttons, reply and `onDone`. The rich message carries the buttons,
        /// the reply reference and `onDone` itself; `key` replaces a queued item with the same key, as for _sendText.
        /// </summary>
        /// <param name="threadId">The topic (0 = the chat's General view).</param>
        /// <param name="richHtml">The rich-message HTML (TgRichHtmlFromClassic of the tokenized text), within kRichMessageMaxChars.</param>
        /// <param name="classicHtml">The classic render of the same text (TgRenderTitleTokensClassic): the fallback.</param>
        /// <param name="silent">No sound.</param>
        /// <param name="prio">The outbox priority.</param>
        /// <param name="key">"" or the replacement key (a card post's "card-post:&lt;session&gt;").</param>
        /// <param name="replyMarkup">The inline keyboard (a non-object = none).</param>
        /// <param name="onDone">Called with Telegram's answer to the message that went (rich, or its fallback's last chunk); may be empty.</param>
        /// <param name="replyToMessageId">The message it replies to (0 = none).</param>
        /// <param name="replyRef">§21.12: reply to the turn's prompt once its id is known; may be empty.</param>
        /// <exception cref="std::bad_alloc">If an item cannot be allocated.</exception>
        void _sendTitledRich(int64_t threadId, const std::wstring& richHtml, const std::wstring& classicHtml, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, std::shared_ptr<PromptRef> replyRef);
        /// <summary>
        /// Posts Claude's MARKDOWN into a topic as a RICH message (§21.11, Bot API 10.1: tables, headings, lists and code
        /// render the way Claude wrote them; the HTML conversion has no tables). The text is split into parts that fit
        /// (TgChunkMarkdown) and each part prepared (TgPrepareRichMarkdown). A part Telegram refuses (any 400 that names no
        /// missing chat or thread; a 404 means the Bot API has no rich messages, and none is tried again this run) goes
        /// again as HTML in its own place: `fallbackHtml` for a message of one part, else that part converted. With rich
        /// messages off (AppSettings::messagingRichMessages) or `markdown` empty, `fallbackHtml` goes at once (_sendText).
        /// Only the last part rings and gets `onDone`; the first carries the reply reference; `movesCard` as _sendText.
        /// </summary>
        /// <param name="threadId">The topic (0 = the chat's General view).</param>
        /// <param name="markdown">Claude's Markdown as written (the caller's own lines escaped with TgMdEscapeText).</param>
        /// <param name="fallbackHtml">The same message as Telegram HTML ("" = TgMarkdownToHtml of the Markdown).</param>
        /// <param name="silent">No sound (the last part rings otherwise).</param>
        /// <param name="prio">The outbox priority.</param>
        /// <param name="onDone">Called with the last part's answer (on the send thread; may be empty).</param>
        /// <param name="replyToMessageId">The message the first part replies to (0 = none).</param>
        /// <param name="movesCard">Stamps the topic's lastPostedMs (the sticky card re-posts below it).</param>
        /// <param name="replyRef">§21.12: reply to the turn's prompt once its message id is known (the first part, and its HTML
        /// fallback). May be empty.</param>
        void _sendRich(int64_t threadId, const std::wstring& markdown, const std::wstring& fallbackHtml, bool silent, SendPriority prio, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId = 0, bool movesCard = true, std::shared_ptr<PromptRef> replyRef = nullptr);
        /// <summary>
        /// Sends plain text into a topic AS THE USER through the Telegram Business connection (the user's "Secretary
        /// mode"; §21.5) — the way a prompt typed at the PC becomes the user's own bubble in the topic. Only valid
        /// once the SECRETARY PROBE found that Telegram accepts such a send into this chat (`businessMode == 1`), or
        /// as the probe itself. `onDone` gets Telegram's answer on the send thread (a refusal is the caller's cue to
        /// fall back to the bot-rendered line). `prio` is the outbox priority: a live prompt goes New; a HISTORY replay's
        /// turn (§21.9) goes History, and its echo-guard entry lives kBackfillStallMs longer (it may wait that long to go).
        /// </summary>
        void _sendAsUser(int64_t threadId, const std::wstring& text, std::function<void(const TgResult&)> onDone, SendPriority prio = SendPriority::New);
        /// <summary>
        /// The SECRETARY ECHO guard (§21.6): true (and the entry consumed) when an incoming message from the owner in
        /// `threadId` carries a text this service sent AS THE USER in the last two minutes. Such a message is the
        /// user's own bubble in the chat, but it is our echo of a prompt typed at the PC — queued again, it would
        /// be sent to claude a second time and come back once more: a loop. Any thread; takes `_mtx`.
        /// </summary>
        /// <param name="threadId">The message's topic.</param>
        /// <param name="text">The message's text.</param>
        /// <returns>True when the message is our echo and must be ignored.</returns>
        bool _takeSecretaryEcho(int64_t threadId, const std::wstring& text);
        /// <summary>
        /// §21.12: an echo of our send AS THE USER (a message of the owner that the secretary bot sent, or that repeats a text
        /// sent as the user) arrived in `threadId` with `messageId` — THIS bot's id of the user's bubble. When it is the topic's
        /// current prompt (its PromptRef waits for an id and its text hash matches), the id is recorded: the turn's status
        /// becomes a reaction on that message, and the turn's final reply quotes it. Any thread; takes `_mtx`.
        /// </summary>
        void _notePromptEcho(int64_t threadId, const std::wstring& text, int64_t messageId);
        /// <summary>Stamps the topic that owns `threadId` as "something newer than the card landed here" (the sticky card's trigger).</summary>
        void _noteTopicPosted(int64_t threadId);
        /// <summary>
        /// Edits a message in place (an edit cannot be split: cut at Telegram's limit). `onDone` gets Telegram's answer on the
        /// send thread (may be empty).
        /// </summary>
        /// <param name="messageId">The message (0 or less = nothing to edit: no call).</param>
        /// <param name="html">The new text: Telegram HTML, with title tokens (§21.15) or without.</param>
        /// <param name="prio">The outbox priority.</param>
        /// <param name="key">"" or the replacement key: a newer edit with the same key replaces a queued one.</param>
        /// <param name="replyMarkup">The inline keyboard (a non-object = none: an edit without one removes the message's).</param>
        /// <param name="onDone">Called with Telegram's answer to the edit that went (the rich one, or its classic fallback).</param>
        /// <param name="onForm">§21.15: called on the send thread with the FORM the message shows after an edit that took (or that
        /// Telegram found unchanged): true = rich, false = classic. Not called when the edit failed otherwise. May be empty.</param>
        /// <remarks>
        /// §21.15: a text with title tokens is edited as a RICH message whose titles are chips while _titleChipsWanted and the rich
        /// form fits — editMessageText turns a classic message into a rich one and back (measured on the dev bot, thread 26785,
        /// messages 909 / 910) — else as its classic render. A rich edit Telegram refuses (RichEditRefusalWantsFallback) is made
        /// again with the classic render, in its own outbox place and without a key, so it never replaces a newer edit queued
        /// meanwhile.
        /// </remarks>
        /// <exception cref="std::bad_alloc">If the item cannot be allocated.</exception>
        void _editText(int64_t messageId, const std::wstring& html, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone = nullptr, std::function<void(bool)> onForm = nullptr);
        /// <summary>
        /// The CLASSIC editMessageText item of a text without title tokens (§21.15 split it out of _editText, which also builds
        /// the classic fallback of a refused rich edit with it): HTML parse mode, no link previews, the text cut at Telegram's
        /// limit.
        /// </summary>
        /// <param name="messageId">The message.</param>
        /// <param name="html">Telegram HTML without title tokens.</param>
        /// <param name="prio">The outbox priority.</param>
        /// <param name="key">"" or the replacement key.</param>
        /// <param name="replyMarkup">The inline keyboard (a non-object = none).</param>
        /// <returns>The item, without onDone.</returns>
        /// <exception cref="std::bad_alloc">If the item cannot be allocated.</exception>
        OutItem _classicEditItem(int64_t messageId, const std::wstring& html, SendPriority prio, const std::wstring& key, const json::Value& replyMarkup) const;
        void _react(int64_t messageId, std::wstring_view emoji);
        void _sendDocument(int64_t threadId, std::wstring_view fileName, std::string bytes, const std::wstring& caption, SendPriority prio);
        json::Value _buttons(const std::vector<std::vector<std::pair<std::wstring, std::wstring>>>& rows) const;
        /// <summary>
        /// The card's buttons on one page (§21.24 - Main: ⚙ Session · ⑂ Fork · 🆕 New Session · 🔄 Restart Session; Session: the
        /// card's buttons of before, 🖼 Screen Image and ◀ Back), each page led by the state's own action (🔧 Resume Session for a
        /// session in Error, ▶ Reactivate for a parked tab or a claude that exited). BuildCardButtonRows lays them out.
        /// </summary>
        /// <param name="sessionId">The card's session (it goes into every button's data).</param>
        /// <param name="facts">What the labels and the first row depend on (CardButtonFactsFor).</param>
        /// <param name="page">The page to show.</param>
        /// <param name="markupOut">The inline keyboard (Telegram's reply_markup).</param>
        /// <returns>Always "" (the keyboard is the output).</returns>
        /// <exception cref="std::bad_alloc">If the keyboard cannot be allocated.</exception>
        std::wstring _cardMarkupFor(const std::wstring& sessionId, const CardButtonFacts& facts, CardPage page, json::Value& markupOut) const;
        /// <summary>
        /// Sends a fresh card at the bottom of the topic, deleting the old card message first when `deleteFirst` is
        /// a message id (the STICKY CARD, §21.5). Marks the record's card "in flight" (-1) until Telegram answers;
        /// a refused send leaves it 0 with `cardPostedMs` = now, and `_refreshCard` tries a new card a minute later.
        /// `ping` sends it WITH SOUND — the notification for a state the user must know about (claude exited, an
        /// Error, a permission prompt; §21.6), which the card itself now says instead of a bot message. `textHash`
        /// (CardTextHash) is recorded as the text the topic shows. The answer is matched to its topic by THREAD,
        /// so a topic carried over to another session meanwhile (a Rekey) still learns its card's message id.
        /// </summary>
        void _postCard(const std::wstring& sessionId, int64_t threadId, const std::wstring& text, json::Value markup, int64_t deleteFirst, bool ping, uint32_t textHash);
        /// <summary>
        /// Fills a NEW topic record from the session's transcript (§21.6): the transcript path, where the mirror
        /// starts (the end of the file for a session at rest — a topic shows the session from now on; the start of
        /// the turn's own prompt line for one that is mid-turn, so the prompt that made the tab qualify is not lost;
        /// 0 for a session with no transcript yet, whose first lines are all news), and the recap facts read from
        /// the tail of the transcript the session SHOWS (its own, else a fresh fork's source — ResolveClaudeDisplayTranscript).
        /// Reads at most kRecapSeedBytes; never throws (a failure is logged and leaves the mirror at the file's end).
        /// </summary>
        /// <param name="t">The record to fill (sessionId set).</param>
        /// <param name="s">The session (its state picks the cursor; forkParentId the recap's source).</param>
        void _seedFromTranscript(TopicRecord& t, const SessionInfo& s) noexcept;
        /// <summary>
        /// Carries a topic over to the session that now holds its tab (TopicSyncOp::Rekey, §21.6 — a /clear, a
        /// /resume in the tab, a handover in place): the record moves to the new key with a fresh mirror seeded from
        /// the new session's transcript, the topic is renamed when the title differs, one silent line marks the new
        /// conversation, and the card is refreshed. Tick thread.
        /// </summary>
        /// <param name="fromSessionId">The closed session the topic belonged to.</param>
        /// <param name="toSessionId">The session that holds the tab now.</param>
        /// <param name="title">The new session's title.</param>
        void _rekeyTopic(const std::wstring& fromSessionId, const std::wstring& toSessionId, const std::wstring& title);

        // ----- topics = tabs (Messaging.cpp) -----
        void _ensureSearchTopic();
        /// <summary>
        /// §21.18: sends the START NOTE when it is due — its first send (the facts gathered by _ensureSearchTopic) or a new
        /// attempt after a failed one (_onStartNoteSent set its time) — into the Search topic, built with this attempt's clock
        /// (BuildStartNoteText: a note that goes out late says so), with sound only for an app launch (StartNoteRings). With
        /// no Search topic yet it waits for a later tick. Tick thread; two atomic loads when nothing is due.
        /// </summary>
        void _pumpStartNote();
        /// <summary>
        /// §21.18: what one send of the start note came back with. Delivered: logged with its message id, done. The Search
        /// topic gone (the user deleted it: "message thread not found"): the topic is forgotten, the next tick makes a new
        /// one, and the note goes there. A failure that can pass (no network, a 5xx, a 429 the send loop's one retry did not
        /// outlast): sent again after a growing wait (30 s doubling, 10 minutes at most), kStartNoteMaxAttempts sends in all.
        /// Any other refusal (a 4xx about the note itself): logged and dropped — sending it again would be refused again.
        /// Send thread (the outbox item's onDone).
        /// </summary>
        /// <param name="threadId">The Search topic the attempt was sent to.</param>
        /// <param name="r">Telegram's answer (for a refused HTML, the answer of its plain-text resend).</param>
        void _onStartNoteSent(int64_t threadId, const TgResult& r);
        void _syncTopics();
        /// <summary>
        /// Makes a session's topic: its ICON the state's (StateTopicIcon, §21.22) and its name TopicDisplayName(state, square,
        /// title, true) — the tab color's square and the title, no glyph (§21.13) — while state icons are wanted
        /// (_stateIconsWanted); else the glyph form (§21.11). The creation color (`icon_color`) is still the tab's: it is what
        /// Telegram shows if the icon is ever removed. A create Telegram refuses for its icon is made again in the glyph form
        /// (and the state icons go off for the run); one refused for its color, without it. Seeded from the transcript (its
        /// history replayed first, §21.9), its card posted. The record keeps the square, the tab-color mode it stands for and
        /// the form (TopicRecord::nameColor / nameColorMode / stateIcon). `verbose` is the new topic's TopicRecord::verbose
        /// (-1 = the default for all; a topic recreated in a mode passes 0 or 1). Tick thread.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="title">Its tab title ("" = the short id).</param>
        /// <param name="colorHex">Its tab color (ResolveSessionColorHex; "" = none: no icon color, no square).</param>
        /// <param name="state">Its state (the icon, or the name's glyph in the glyph form).</param>
        /// <param name="verbose">The topic's own verbose mode.</param>
        void _createTopic(const std::wstring& sessionId, const std::wstring& title, const std::wstring& colorHex, SessionState state, int verbose = -1);
        void _deleteTopic(const std::wstring& sessionId, const std::wstring& reason);
        /// <summary>
        /// Updates a session's topic in ONE editForumTopic: its ICON to the state's (StateTopicIcon) and its name to
        /// TopicDisplayName(state, square, title, true) while state icons are wanted (§21.22) — a state change then changes only
        /// the icon —, else the glyph form with any state icon the topic shows removed. Records the title, the state, the square
        /// and the form (a refused rename is recorded too, so it is not retried every tick — except a refusal of Telegram's flood
        /// control, and a refused ICON, which turns the state icons off for the run and leaves the topic due in the glyph form).
        /// The square is ChooseNameColor's: the tab's color once it has held, else the one the name shows. When the TITLE
        /// changed, or the tab's COLOR changed from one square to another under a settled mode, a silent line in the topic says
        /// so (BuildTopicChangeNoticeHtml — the user's "a message like whatsapp does for group chat name changed", §21.13); a
        /// state-only change, a quiet re-square or re-form and a topic whose history is still being replayed get none. The card
        /// is refreshed after a rename Telegram took. Tick thread (a direct call, paced by _awaitCallSlot).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="title">The title the name shows from now on.</param>
        /// <param name="state">The state the topic shows (its icon, or its name's glyph in the glyph form).</param>
        void _renameTopic(const std::wstring& sessionId, const std::wstring& title, SessionState state);
        /// <summary>
        /// Whether the topics show their sessions' states as their ICONS (§21.22, TopicRecord::stateIcon) — true until Telegram
        /// refuses a topic icon in this run (TopicIconRefused; then the glyph form, §21.11, until the bot starts again). Any
        /// thread.
        /// </summary>
        /// <returns>True when the state icons are wanted.</returns>
        bool _stateIconsWanted() const noexcept;
        /// <summary>
        /// The topics' states (their icons, §21.22 — or their names' glyphs in the glyph form, §21.11) and color squares
        /// (§21.13), once per tick after the sync: at most one rename (PickNameRename over the sync's facts with
        /// _stateIconsWanted: the states that need the user, then a tab's color change, then the quiet re-squares and
        /// re-forms) and only while the last minute's calls leave room for its two calls (NameRenameFits), no 429 pause runs
        /// and no long 429 parks editForumTopic, the start-up hold is over and no recreation is under way. A due rename that
        /// finds no room stamps _renameWaitingSinceMs, and the history replay holds until it went (HistoryYieldsToRename):
        /// renames come after the live traffic and BEFORE the replay. Tick thread; never throws (Rule #18 inside).
        /// </summary>
        void _stateNameTick() noexcept;
        /// <summary>
        /// The DIRECTORY (§21.16), once per tick after the names, at most every kDirectoryCheckMs: builds the model — the fleet
        /// counts from the sync's facts and the registry's queues, the dedicated topics (_directoryTopics), every tag of the
        /// session store joined with the open tabs that carry it — renders it (RenderDirectory, which lists only the tags an
        /// open tab carries: the user's "Tags with 0 tabs should not be shown") and does what DecideDirectoryAction
        /// says: POST a new message in the chat's main view and pin it (_onDirectoryPosted, _pinDirectory), or EDIT the one it
        /// has (_editDirectory) when its content or form is not the wanted one and the edit gap has passed. Skipped while the
        /// facts are incomplete (the start-up hold, a recreation), before the session store's start-up warm-up is done (its
        /// first whole read opens every record file), before the bot's username is known (the links need it), and while a
        /// 429 pause runs or a long one parks a method the directory uses. Tick thread; never throws (Rule #18 inside).
        /// </summary>
        void _directoryTick() noexcept;
        /// <summary>
        /// The chat's DEDICATED topics, in the order the directory lists them: 🔎 Search today (its thread, 0 until it is
        /// made; its icon color). A new dedicated topic adds its entry here. Tick thread; takes `_mtx`.
        /// </summary>
        /// <returns>The topics.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        std::vector<DirectoryTopic> _directoryTopics() const;
        /// <summary>
        /// Telegram's answer to the directory's POST: a message id makes it THE directory (MessagingState's fields; its form
        /// from the answer — a rich send that fell back to HTML is classic, and that content's rich form counts as refused)
        /// and asks for its pin; a refusal backs off kDirectoryRetryMs. An answer of a send this run already forgot (a stall,
        /// kDirectoryStallMs) is not taken: a message it made after all is deleted, so the chat never holds two directories.
        /// Send thread.
        /// </summary>
        /// <param name="serial">The send's number (_directorySerial when it was queued).</param>
        /// <param name="hash">The content hash of what was sent.</param>
        /// <param name="wantRich">The rich form was asked for.</param>
        /// <param name="r">Telegram's answer (the rich send's, or its classic fallback's).</param>
        void _onDirectoryPosted(uint64_t serial, uint32_t hash, bool wantRich, const TgResult& r);
        /// <summary>
        /// Edits the directory message in place (§21.16): a RICH edit when `wantRich` (a refusal — RichEditRefusalWantsFallback —
        /// is made again with the classic render, in its own outbox place and without a key, so it never replaces a newer
        /// edit), else a classic edit. Keyed "dir:edit": a newer edit replaces a queued one. Tick thread.
        /// </summary>
        /// <param name="messageId">The directory message.</param>
        /// <param name="r">The render.</param>
        /// <param name="wantRich">The form to edit it into.</param>
        /// <param name="serial">The send's number (_directorySerial when it was queued).</param>
        /// <exception cref="std::bad_alloc">If an item cannot be allocated.</exception>
        void _editDirectory(int64_t messageId, const DirectoryRender& r, bool wantRich, uint64_t serial);
        /// <summary>
        /// Telegram's answer to a directory EDIT: taken (or "not modified") records the content and the form the message
        /// shows, and the run's FIRST taken edit is logged with what it lists; "the message is gone"
        /// (DirectoryEditFoundNoMessage — the user deleted it) forgets the message and posts a new one after
        /// kDirectoryRepostGapMs; any other refusal backs off kDirectoryRetryMs. A stale serial is ignored. Send thread.
        /// </summary>
        /// <param name="serial">The edit's number.</param>
        /// <param name="hash">The content hash of what was sent.</param>
        /// <param name="messageId">The message that was edited.</param>
        /// <param name="wantRich">The rich form was asked for.</param>
        /// <param name="r">Telegram's answer.</param>
        /// <param name="shownRich">The form the message shows when the edit took (or was found unchanged).</param>
        /// <param name="what">What the edited render lists (DescribeDirectoryRender), for that first log line.</param>
        /// <exception cref="std::bad_alloc">If a log line cannot be allocated.</exception>
        void _onDirectoryEdited(uint64_t serial, uint32_t hash, int64_t messageId, bool wantRich, const TgResult& r, bool shownRich, const std::wstring& what);
        /// <summary>
        /// Pins the directory message (pinChatMessage, silent; keyed "dir:pin"), once per message and run: a refused pin is
        /// logged and tried again at the next start. Telegram's "pinned a message" note that the pin leaves in the chat is
        /// deleted when it comes back through getUpdates (_handleMessage). Any thread; takes `_mtx`.
        /// </summary>
        /// <param name="messageId">The directory message.</param>
        /// <exception cref="std::bad_alloc">If the item cannot be allocated.</exception>
        void _pinDirectory(int64_t messageId);
        /// <summary>The last sync's facts of one session (tick thread; null when the sync did not list it).</summary>
        const TabFact* _lastFact(const std::wstring& sessionId) const noexcept;
        /// <summary>The tab-color mode the last sync resolved the colors under (the settings' before the first sync). Any thread.</summary>
        TabColorMode _currentColorMode() const noexcept;
        /// <summary>
        /// The color square for a session's title in a message (§21.13, every place a title is printed): TgTitleColorSquare
        /// of its tab color, resolved like the tab (ResolveSessionColorHex under _currentColorMode); a session the registry does
        /// not know (an old one the Search found) wears its folder's color, `dirFallback`, like the Sessions page's chip. Any
        /// thread: it reads dir-colors.json through its cache. Never throws: a failure is logged (Rule #18) and gives "".
        /// </summary>
        /// <param name="s">The session (null = the registry does not know it).</param>
        /// <param name="dirFallback">The working folder of a session the registry does not know ("" = no square).</param>
        /// <returns>The square, or "" (the "Remove colors" mode, nothing known).</returns>
        std::wstring _titleSquare(const SessionInfo* s, const std::wstring& dirFallback = {}) const noexcept;
        /// <summary>
        /// Recreates ONE session's topic in a mode (§21.11, /recreate in the topic): the topic is deleted with its messages and
        /// made again at once with its session's whole history, rendered verbose or not (TopicRecord::verbose = `verbose`).
        /// Tick thread (the requests come from the poll thread through `_recreateOne`).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="verbose">0 = the conversation only, 1 = verbose.</param>
        void _recreateOneTopic(const std::wstring& sessionId, int verbose);
        /// <summary>Whether a topic is verbose now (TopicIsVerbose with the default for all topics). Any thread.</summary>
        bool _topicVerbose(const TopicRecord& t) const noexcept;
        /// <summary>
        /// Starts the RECREATION (§21.8): every session topic's thread moves to MessagingState::orphanThreads (persisted),
        /// the topic map, the mirrors and the cards are cleared, queued work for the old topics is dropped, the layout is
        /// recorded as current, and a silent note in the 🔎 Search topic says what happens. The sync then deletes the
        /// orphans (paced, a few per tick) and, once they are all gone, makes the list again — oldest first, so it
        /// comes out in MOST RECENT order. Tick thread only.
        /// </summary>
        /// <param name="why">Why (logged and named in the note).</param>
        void _recreateTopics(const std::wstring& why);
        /// <summary>
        /// Deletes up to `budget` of the RECREATION's orphan threads (paced), removing each from the list whatever
        /// Telegram answered (a topic already gone is fine; a refusal is logged — a later delete by hand is the user's).
        /// Tick thread only.
        /// </summary>
        /// <param name="budget">At most this many deletes.</param>
        /// <returns>How many were attempted.</returns>
        size_t _deleteOrphanTopics(size_t budget);
        /// <summary>
        /// The HISTORY replay's pump (§21.9), once per tick: while a batch is going out it only waits (ONE batch in the
        /// whole outbox at a time — small memory, a strict order); when the batch's last message was answered it moves that
        /// topic's backfillPos on (a stalled batch, kBackfillStallMs, is dropped and sent again from the old position);
        /// then it picks the next topic with history pending — the highest-ranked in MOST RECENT order first, the sessions
        /// the user cares about now — reads the next window of its transcript (complete turns only), renders it
        /// (RenderHistory) and queues the posts at the History priority (sent only when nothing else is queued, one per
        /// kBackfillGapMs), silent, the user's turns as the user when secretary mode works. It waits while a RECREATION is
        /// still deleting the old topics or making the new list (5 minutes at most), so the replay goes most recent first over
        /// the whole list. When no history is pending after a recreation, it asks for the full re-sort of the list. Tick
        /// thread; never throws (Rule #18 inside).
        /// </summary>
        void _pumpBackfill() noexcept;
        /// <summary>
        /// Keeps Telegram's topic list in the side panel's MOST RECENT order (§21.9): after a recreation's history replay, all
        /// cards are re-posted lowest-ranked first (the list was in replay order); otherwise, at most once a minute and only
        /// when everything is quiet (nothing queued, no replay, no session running, no topic posted in the last half
        /// minute), the shortest set of cards that puts the list back in order is re-posted (PlanTopicResort, at most
        /// kResortMaxTopics; a longer fix is left to the next replay or /recreate). Never during the start-up hold or a
        /// recreation (the order keys are incomplete then). The re-posts themselves run in the next tick's per-topic pass,
        /// which goes lowest-ranked first. Tick thread; never throws (Rule #18 inside).
        /// </summary>
        void _resortTick() noexcept;
        /// <summary>
        /// Takes the per-chat pacing slot that the send thread's outbox uses (kSendGapMs), waiting for it, before a
        /// DIRECT call of the tick thread (createForumTopic, deleteForumTopic, editForumTopic) — such calls counted
        /// against the chat's ~1 call/s budget without waiting for it before §21.8 (a recreation makes ~150 calls).
        /// Counts the call in the traffic census.
        /// </summary>
        /// <param name="method">The Bot API method (the census key, and the key the flood parking holds).</param>
        /// <returns>True when the call may go; false when the service is stopping, or when a long 429 parks the method
        /// (_floodParking) — the caller must not make it, and leaves the work for a later tick (Stop aborts only the request
        /// in flight, so a call started now would hold Stop's join for its timeout; a parked method would only collect
        /// another refusal).</returns>
        bool _awaitCallSlot(const wchar_t* method);
        /// <summary>
        /// Brings the topic's STICKY card up to date (§21.5, §21.6): posts it when there is none, re-posts it at the
        /// bottom once newer posts have settled, and otherwise edits it in place when its text changed — never when
        /// the topic already shows that text (TopicRecord::cardTextHash, which survives a restart). `force` skips
        /// the edit gap (a state change, a rename); `ping` asks for a re-post WITH SOUND, kept until it can be sent
        /// (a card post still in flight, or a refused one waiting its minute).
        /// </summary>
        /// <remarks>
        /// §21.15: the card's title is a chip when titled messages go rich (_titleChipsWanted). The comparison hash is taken over
        /// the card's classic render, so it does not move with the form; a card whose text is unchanged but whose FORM is not the
        /// wanted one (TopicRecord::cardRich — every card posted before the chips, or after the rich switch flipped) is re-edited
        /// only when _takeCardFormatSlot gives it the turn: one card per kCardFormatGapMs, within the chat's call budget.
        /// §21.24: the card shows the page its CardState holds (CardPage: Main unless ⚙ Session was tapped), and raising a ping
        /// takes it back to Main, where the state's own action is. `tapped`: the user tapped a button that changes the card itself (its
        /// page), so the edit goes at the priority of an answer to the user (SendPriority::Final) instead of the cards' own, and
        /// the keyboard flips at once even behind a busy outbox.
        /// </remarks>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="force">Edit now, past the edit gap.</param>
        /// <param name="ping">Re-post the card with sound (kept until a post can carry it).</param>
        /// <param name="tapped">A button on the card changed it: edit at the priority of an answer.</param>
        void _refreshCard(const std::wstring& sessionId, bool force, bool ping = false, bool tapped = false);
        /// <summary>
        /// The SLOW FORM UPGRADE's gate (§21.15): whether the card of `sessionId`, unchanged but shown in the other form, may be
        /// re-edited now. One such edit per kCardFormatGapMs across all topics, none while a 429 pause runs or a long one parks
        /// editMessageText, none while the last minute's calls leave no room for one more under kHistoryCallsPerMinute, and never
        /// for a card whose rich form Telegram refused for this very text (CardState::richRefusedHash). Taking the slot stamps it
        /// and logs how many cards still wait. Takes `_mtx`.
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="hash">The card's classic hash (what TopicRecord::cardTextHash holds).</param>
        /// <param name="wantRich">The form wanted (_titleChipsWanted).</param>
        /// <param name="wantLinked">§21.17: whether the card's title should link to its topic (the bot's username is known). A card
        /// that lacks its link is upgraded through this slot too, even when Telegram refused its rich form for this text (the
        /// edit then falls back to the classic form, which carries the link).</param>
        /// <returns>True when the card is re-edited now.</returns>
        bool _takeCardFormatSlot(const std::wstring& sessionId, uint32_t hash, bool wantRich, bool wantLinked);
        /// <summary>
        /// The bot's @username WITHOUT the '@' for the topic links of titles (§21.17), "" until this run's first getMe answered.
        /// A copy of `_botUsername` behind its own LEAF lock (`_linkBotMtx`), so every send path can read it whatever its caller
        /// holds: some callers of _sendText / _editText hold `_mtx`, which guards `_botUsername`. Any thread.
        /// </summary>
        /// <returns>The username; "" = no links yet (titles go as copy chips / plain mentions, as before §21.17).</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        std::wstring _linkBot() const;
        /// <summary>
        /// The thread of a session's topic (§21.17): what a reply that names the session links its title to. Takes `_mtx` — never
        /// call it with `_mtx` held.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <returns>Its topic's message_thread_id; 0 when it has no topic (closed, idle, Codex) — the title then links nowhere.</returns>
        int64_t _threadOf(const std::wstring& sessionId) const;
        std::optional<SessionInfo> _session(const std::wstring& sessionId) const;

        // ----- the mirror (Messaging.cpp) -----
        struct Mirror
        {
            std::wstring pendingText; // assistant text not yet known to be a remark or the final reply
            std::wstring workLog; // the current turn's work-log text (plain, before escaping)
            int64_t workLogMessageId{ 0 };
            int64_t workLogLastEditMs{ 0 };
            bool workLogDirty{ false };
            /// <summary>
            /// The work-log SEGMENT number: a remark posted as its own message closes the segment (the tools that
            /// follow start a new work-log message below the remark, §21.5), and a create that was in flight for
            /// the closed segment must not hand its message id to the new one — its callback compares this.
            /// </summary>
            uint32_t workLogGen{ 0 };
            bool turnOpen{ false };
            SessionState lastState{ SessionState::Idle };
            int64_t lastStateMs{ 0 };
            /// <summary>
            /// One sound per "the session needs you" (§21.6): the turn's question or plan was posted (with sound) —
            /// the NeedsApproval edge that follows rings nothing more; and the card was re-posted with sound for a
            /// NeedsApproval edge (a permission prompt, or the edge seen before the question's line) — the question
            /// posted after it goes out silent. Both clear when the turn runs on or a new one starts.
            /// </summary>
            bool awaitingAnswer{ false };
            bool approvalPinged{ false };
            std::unordered_map<std::wstring, std::wstring> toolNames; // tool_use id -> name (to label a failed result)
            int64_t lastTranscriptCheckMs{ 0 };
            int64_t lastResolveMs{ 0 };
            /// <summary>
            /// §21.12: the current turn's PROMPT (its message id as its send answers; see PromptRef) — the final reply quotes it
            /// and its status follows the turn. Null until the mirror sees a prompt in this run (then rebuilt from the
            /// TopicRecord's persisted promptMessageId).
            /// </summary>
            std::shared_ptr<PromptRef> promptRef;
        };
        /// <summary>
        /// The card's transient state per session, apart from the Mirror on purpose: the tick works on a COPY of a
        /// session's mirror and writes it back, so anything _refreshCard records in the map during that window would
        /// be overwritten (the old lastCardText / lastCardEditMs were, and a card was edited twice). Under `_mtx`.
        /// </summary>
        struct CardState
        {
            int64_t lastEditMs{ 0 }; // when the card was last sent or edited (the edit gap counts from it)
            SessionState lastState{ SessionState::Idle }; // the state the card last showed (a change edits at once)
            bool pingWanted{ false }; // a re-post WITH SOUND is owed (see _refreshCard)
            bool repostWanted{ false }; // a SILENT re-post is owed: the list's re-sort (§21.9) moves this topic up
            // §21.24: the page of buttons the sticky card shows (⚙ Session flips it to Session; ◀ Back and a ping take it back to
            // Main). In memory only, like the rest of this state: after a restart every card shows Main again.
            CardPage page{ CardPage::Main };
            // §21.15: the classic hash (TopicRecord::cardTextHash) of the card text whose RICH form Telegram refused (it went as
            // HTML). The slow form upgrade skips the card while its text is that one, so a refusal is not retried every gap; a
            // changed text tries the rich form again. 0 = none refused.
            uint32_t richRefusedHash{ 0 };
        };
        void _followTranscript(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s);
        void _applyItems(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::vector<TopicItem>& items, const SessionInfo& s);
        void _flushWorkLog(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, bool force);
        /// <summary>Posts the assistant text gathered since the last tool call as ITS OWN silent message and rolls the work log, so the topic reads remark → tools → remark → … → reply (§21.5).</summary>
        void _postRemark(const std::wstring& sessionId, TopicRecord& topic, Mirror& m);
        /// <summary>
        /// Closes the turn with a message: the work log is finished, `html` is posted — with sound unless `sound` is
        /// false (D5 / M12: a turn's ONE message with sound; §21.6 posts an interrupt, or the text before an exit or an
        /// Error whose card rings instead, silently) — and the turn's work-log state is reset. With `markdown` (§21.11:
        /// Claude's own text) the message goes as a RICH message, `html` its fallback (_sendRich).
        /// </summary>
        void _postFinal(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::wstring& html, const std::wstring& why, bool sound = true, const std::wstring& markdown = {});
        /// <summary>Closes the turn WITHOUT a message (claude exited or failed with nothing more to say): the work log is finished and its state reset.</summary>
        void _endTurnQuietly(const std::wstring& sessionId, TopicRecord& topic, Mirror& m);
        void _appendWorkLog(Mirror& m, const std::wstring& line);
        /// <summary>
        /// §21.12, once per tick and topic: brings the LAST prompt's status up to the session's state (PromptTurnStatusFor) —
        /// an edit of the bot's prompt line, or a reaction on the user's own message — once its message id is known and the
        /// new state has held kPromptStatusSettleMs (an Error shows at once). Syncs the record's id from the mirror's
        /// PromptRef (or rebuilds the PromptRef from the record after a restart). Tick thread.
        /// </summary>
        void _updatePromptStatus(TopicRecord& topic, Mirror& m, const SessionInfo& s);
        /// <summary>Shows `status` on the topic's last prompt (an edit of our line, or a reaction on the user's message; None removes it) and records it. Tick thread.</summary>
        void _showPromptStatus(TopicRecord& topic, PromptTurnStatus status);
        void _onStateEdge(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s);
        void _syncReactions(const SessionInfo& s);

        // ----- inbound (Messaging.Inbound.cpp) -----
        void _handleUpdate(const json::Value& update) noexcept;
        /// <summary>
        /// One message (or one edit) of the owner's private chat with the bot, routed by where it was written: the bot's own
        /// service messages (a rename notice and the directory's pin note are deleted), a sender other than the paired owner
        /// (dropped), what the bot itself sent as the user (secretary mode — never a new prompt), a thread the user created (its
        /// §21.17 picker), a rename of a session's topic, a text-less message, /search-tab and /search from any thread (§21.19),
        /// the 🔎 Search topic, a session's topic (an edit replaces a still-queued prompt), the main view (the help line), a
        /// recreation's old topic, and any other thread (held for a new session, _holdForNewThread). A thread that is a
        /// session's topic but still has its new-thread bind is being bound by the tick right now: its message joins the bind
        /// behind the messages it holds (an edit replaces one there), so the session gets them in the order the user wrote them.
        /// Poll thread; called through _handleUpdate, which catches what it throws.
        /// </summary>
        /// <param name="msg">The Telegram Message object.</param>
        /// <param name="edited">True for an edited_message update.</param>
        /// <exception cref="std::bad_alloc">If memory runs out (the registry, the held lists and the outbox copy what they
        /// keep); a handler it routes to may throw the same.</exception>
        void _handleMessage(const json::Value& msg, bool edited);
        void _handleCallback(const json::Value& cq);
        /// <summary>
        /// A message in a session's topic: an Agentmaster /command runs, a refused one is answered, anything else is queued as the
        /// session's next prompt (a message older than kBacklogAgeMs is asked about first, D2).
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="msg">The Telegram message.</param>
        /// <param name="threadId">The topic's thread.</param>
        /// <param name="held">§21.17: the message was HELD by a new thread's picker and is handed over now that the session is up —
        /// it was read live when it came, so it is never asked about as backlog, however long the folder pick took.</param>
        void _handleTopicMessage(const std::wstring& sessionId, const json::Value& msg, int64_t threadId, bool held = false);
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
        /// <summary>
        /// REACTIVATE (§21.5, §21.6): a parked tab's claude is started in place; a session in Error, or one whose
        /// claude exited (Done), is restarted (the same conversation resumes in the same tab; with
        /// CLAUDE_CODE_RESUME_INTERRUPTED_TURN a cut-off turn goes on by itself). A running session is left alone.
        /// The card's ▶ Reactivate and 🔧 Resume Session (§21.24) buttons show the outcome as the button's toast (nothing is
        /// posted in the thread, which is the conversation); `/reactivate` replies with it.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <returns>The outcome, plain text (one sentence).</returns>
        std::wstring _reactivate(const std::wstring& sessionId);
        /// <summary>
        /// A directory DOT's tap (§21.16.7): puts the tab's topic FIRST in Telegram's thread list — the way into a topic on
        /// Telegram Android, which opens no topic from a link. It asks the tick for a silent re-post of the topic's sticky card
        /// (`CardState::repostWanted`, the re-sort's own mechanism: the next per-topic pass deletes the card and sends it again,
        /// two calls), which makes the card the topic's newest message: Telegram orders a bot chat's topics by their last
        /// message, and Android's "All" view adds "Continue last thread ›" under the chat's last message when it is in a topic.
        /// It also records the topic for `_frontHeld` (via `_frontRequests`), so the quiet re-sort does not move it back. A
        /// topic whose history is still being replayed keeps its card above the history until the replay is in: the move waits
        /// for that, and the toast says so. A second tap on the same dot within kFrontTapGapMs only answers. Runs on the poll
        /// thread; takes `_mtx` and wakes the tick.
        /// </summary>
        /// <param name="threadId">The topic's thread id, from the dot's callback data (ParseDirectoryFrontCallbackData).</param>
        /// <returns>The toast, plain text (DirectoryFrontAnswer, or why nothing moves).</returns>
        /// <exception cref="std::bad_alloc">If memory runs out (it passes through _handleCallback to _handleUpdate, whose guard
        /// logs it; the tap then gets no toast and nothing moves).</exception>
        std::wstring _bringTopicToFront(int64_t threadId);
        /// <summary>
        /// /search-tab (§21.19; also /search_tab, the "/" menu's spelling — kSearchTabCommands — and a bare /search, which has
        /// nothing to ask): takes the user to the 🔎 Search topic from
        /// any thread. Telegram gives a bot no way to switch the app's open view, so it does what the directory's dot does for
        /// a tab (§21.16.7): it makes the Search topic the chat's NEWEST message — Telegram lists a bot chat's topics by their
        /// last message, so Search is then FIRST in the thread list on every app (one tap opens it), and Telegram Android's
        /// "All" view puts "Continue last thread ›" under it. Two calls, in this order:
        ///   1. the command message itself is DELETED (a bot may delete the user's messages in its private chat): it is a
        ///      move, not content, and left where it was typed it would be that thread's newest message — in a session's
        ///      topic its sticky card would then be re-posted below it a few seconds later, which puts THAT topic back on
        ///      top. A delete Telegram refuses (too old, already gone) is logged and changes nothing else;
        ///   2. one SILENT line is posted in the Search topic — the user is at the phone and asked for it.
        /// With no Search topic yet (the first connect is still making it, or it is being made again after the user
        /// deleted it, §21.18), it answers in the thread the command was typed in and deletes nothing. Poll thread.
        /// </summary>
        /// <param name="fromThreadId">The thread the command was typed in (0 = the chat's main view, "All").</param>
        /// <param name="messageId">The command message's id (0 = unknown: nothing to delete).</param>
        /// <exception cref="std::bad_alloc">If memory runs out (it passes through _handleMessage to _handleUpdate, whose guard
        /// logs it; the command then stays where it was typed and nothing moves).</exception>
        void _jumpToSearchTopic(int64_t fromThreadId, int64_t messageId);
        /// <summary>
        /// /search &lt;words&gt; typed in any thread but the Search topic (§21.19, kSearchCommand): SENDS the search there. The
        /// command message is deleted where it was typed (_deleteCommandMessage, for the reason _jumpToSearchTopic gives), the
        /// words are posted in the Search topic as the user's line, naming the session's topic they came from
        /// (BuildRelayedSearchLineHtml), and asked of the Search agent there (_askSearch: "Searching…", then the answer with
        /// sound, D5) — so the Search topic is also the chat's newest thread, first in the list. Not done, with the reason
        /// answered where the command was typed and nothing deleted: with no Search topic yet, and for a message older than
        /// the backlog limit (D2: a search sent while Agentmaster was closed is answered, never run unasked — the Search
        /// topic's own rule for a request). Poll thread.
        /// </summary>
        /// <param name="fromThreadId">The thread the command was typed in (0 = the chat's main view, "All").</param>
        /// <param name="msg">The command message (its id, its date).</param>
        /// <param name="words">The search words (SearchCommandWords; not empty).</param>
        /// <exception cref="std::bad_alloc">If memory runs out (it passes through _handleMessage to _handleUpdate, whose guard
        /// logs it; the search is then not sent, and the command stays where it was typed).</exception>
        void _relaySearchRequest(int64_t fromThreadId, const json::Value& msg, const std::wstring& words);
        /// <summary>
        /// §21.19: queues the delete of a command message the user typed (a bot may delete the user's messages in its private
        /// chat): /search-tab and /search are moves to the Search topic, not content, and left where they were typed they would
        /// be that thread's newest message — in a session's topic its sticky card would then be re-posted below them, putting
        /// that topic back on top of the thread list. Final, so it never waits behind the fleet's other traffic. A refusal (a
        /// message too old, already gone) is only logged. Any thread.
        /// </summary>
        /// <param name="messageId">The message to delete (0 or less = nothing to do).</param>
        /// <param name="what">The command's name for the log ("/search-tab", "/search").</param>
        /// <exception cref="std::bad_alloc">If the outbox item cannot be allocated.</exception>
        void _deleteCommandMessage(int64_t messageId, const wchar_t* what);
        /// <summary>
        /// A `business_connection` update: the user connected, edited or removed a bot under Telegram → Settings → Chat
        /// Automation (H3: the connecting user must be the paired owner). `viaSecretary`: it came to the SECRETARY bot
        /// (§21.11), whose client then sends as the user; else to the topics bot. A disconnect of the bot that does NOT
        /// hold the stored connection changes nothing (the user moved the connection to the other bot).
        /// </summary>
        void _handleBusinessConnection(const json::Value& bc, bool viaSecretary = false);
        /// <summary>
        /// /verbose (§21.11): in a session topic (`sessionId` non-empty) shows and sets that topic's mode — "on", "off",
        /// "default", or buttons; in the 🔎 Search topic the DEFAULT for all topics (AppSettings::messagingVerbose, written
        /// to settings.json and applied at once). A change applies to the messages that come from now on; /recreate redoes a
        /// topic's history in a mode. Poll thread.
        /// </summary>
        void _runVerboseCommand(const std::wstring& sessionId, const std::wstring& args, int64_t threadId);
        /// <summary>Sets one topic's mode (-1 default, 0 off, 1 on) and replies in its topic. Poll thread.</summary>
        void _setTopicVerbose(const std::wstring& sessionId, int verbose, int64_t threadId);
        /// <summary>
        /// Sets the default mode for all topics (settings.json read-modify-write, then live) and replies in `threadId` — no
        /// reply when it is negative (a recreation of all topics in a mode says it in its own line). Poll thread.
        /// </summary>
        void _setDefaultVerbose(bool verbose, int64_t threadId);
        /// <summary>The SECRETARY PROBE: one send as the user into the Search topic; its answer decides `businessMode` and is reported there.</summary>
        void _probeSecretary();
        void _wake(const std::wstring& sessionId, bool pin);
        void _setPinned(const std::wstring& sessionId, bool pinned);
        /// <summary>
        /// Opens a session into a tab of the most recently active window (resume / fork / a new session) and tells the thread how
        /// it goes: "… into a tab…" once a window took the request, then (§21.23) the window's ANSWER — "Opened …" when the
        /// session it opened is live (_sweepRevives), or why nothing opened (_settleLaunchAnswers), or, after
        /// kRemoteLaunchTimeoutMs without either, that the PC never answered (_expireRevives). The revive is recorded under the
        /// launch's token BEFORE the request goes out (the answer may come back from another thread at once). Poll thread.
        /// </summary>
        /// <param name="req">The request (a Resume or Fork without a folder gets the registry's, else the transcript's).</param>
        /// <param name="threadId">The thread to reply in (a session's topic, or the 🔎 Search topic).</param>
        /// <exception cref="std::bad_alloc">If the request, the revive or a reply cannot be allocated.</exception>
        void _requestLaunch(RemoteLaunchRequest req, int64_t threadId);
        /// <summary>
        /// Settles every revive whose launch the window ANSWERED with `s` (§21.23: Revive::openedId — the fork's new session, a
        /// fresh resume's new id, the resumed session itself) once `s` is live: pins it when asked, holds it in Waiting for you so
        /// its topic appears, says "Opened …" in the revive's thread, and forgets the revive. A revive with no answer yet waits
        /// (_expireRevives tells its thread when the wait runs out). Before §21.23 a revive matched the request's own id or
        /// folder, and a /forksession or /newsession in a live session's topic, or 🔎 Search's Fork of a live session, settled on
        /// the SOURCE (§21.21). Tick thread.
        /// </summary>
        /// <param name="s">A session the tick looked at (a topic's, or the one an answer named).</param>
        /// <exception cref="std::bad_alloc">If the matched copies or the reply cannot be allocated.</exception>
        void _sweepRevives(const SessionInfo& s);
        /// <summary>
        /// The card's ⚙ Session and ◀ Back (§21.24): shows `page` on the message the button sits on. On the topic's sticky card
        /// the page is remembered (CardState::page, so a re-post keeps it) and the card is edited through _refreshCard (its
        /// hash covers the buttons); on any other message - a /status copy of the card - only that message's buttons change.
        /// </summary>
        /// <param name="sessionId">The card's session.</param>
        /// <param name="messageId">The message the tapped button sits on.</param>
        /// <param name="page">The page to show.</param>
        void _showCardPage(const std::wstring& sessionId, int64_t messageId, CardPage page);
        /// <summary>
        /// The card's ⑂ Fork and 🆕 New Session (§21.24): the topic's /forksession and /newsession with no model - a fork, or a
        /// fresh session in the session's folder, opened as a background tab - through the commands' own path (their replies go in
        /// the thread, so the button and the typed command never differ); the toast only says it was asked for.
        /// </summary>
        /// <param name="sessionId">The card's session (the fork's source; the folder of the new session).</param>
        /// <param name="fork">True for ⑂ Fork, false for 🆕 New Session.</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>The toast, one plain sentence.</returns>
        std::wstring _launchFromCard(const std::wstring& sessionId, bool fork, int64_t threadId);
        /// <summary>
        /// The card's 🔄 Restart Session (§21.24): the topic's /restartsession. A session at rest is restarted at once (the same
        /// conversation resumes in its tab) and the toast says so; while a turn runs, the restart asks first in the thread (the
        /// turn would be cut off), exactly as /restartsession does, and the toast points at that question.
        /// </summary>
        /// <param name="sessionId">The card's session.</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>The toast, one plain sentence.</returns>
        std::wstring _restartFromCard(const std::wstring& sessionId, int64_t threadId);
        /// <summary>
        /// The second half of 🖼 Screen Image and /screenimage (§21.24), called with the hosting window's answer on ITS UI thread:
        /// parses the snapshot (ScreenSnapshot.h) and draws it as a PNG on a worker thread of its own (ScreenImage.h - ~90 to
        /// ~185 ms of drawing for a 200 x 60 to 300 x 80 screen, measured; never on a UI thread), then sends the picture into
        /// the topic (_sendPhoto). At most kScreenImageMaxRenders pictures are drawn at once; a request past that is answered in
        /// the thread instead.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="threadId">The topic the picture goes to.</param>
        /// <param name="snapshot">The window's answer: the serialized snapshot, or "" when the screen could not be read.</param>
        void _startScreenImage(const std::wstring& sessionId, int64_t threadId, std::wstring snapshot);
        /// <summary>
        /// Sends a picture into a topic as a PHOTO (sendPhoto, silent, at `prio`); when Telegram refuses it as a photo (a size
        /// it will not take as one, a picture it cannot read), the same bytes go again as a document, in its place.
        /// </summary>
        /// <param name="threadId">The topic (0 = the chat's main view).</param>
        /// <param name="fileName">The file's name (screen-TITLE.png).</param>
        /// <param name="bytes">The PNG.</param>
        /// <param name="caption">The caption, plain text (cut at 1,000 characters).</param>
        /// <param name="prio">The send priority.</param>
        void _sendPhoto(int64_t threadId, std::wstring fileName, std::string bytes, std::wstring caption, SendPriority prio);

        // ----- §21.17: a thread the user opens starts a new session (Messaging.Inbound.cpp) -----
        /// <summary>
        /// The owner created a thread (forum_topic_created) that no session owns. A name the user gave it (is_name_implicit
        /// false) is kept — it becomes the session's title — and the picker is posted at once, before any message. A thread
        /// Telegram named (the user only typed a message, which arrives right after) gets its picker with that message
        /// (_holdForNewThread): one post, not a post and an edit. Poll thread.
        /// </summary>
        /// <param name="threadId">The new thread.</param>
        /// <param name="msg">The service message (its forum_topic_created carries the name and is_name_implicit).</param>
        void _onUserThreadCreated(int64_t threadId, const json::Value& msg);
        /// <summary>
        /// A message in a thread no session owns (and not the Search topic): HELD for the new session the thread's picker starts —
        /// a lone folder path starts it in that folder at once; anything else waits (at most kNewThreadHeldMax messages) and goes
        /// to the session in order once it is up. The picker is posted with the first held message when the thread has none yet
        /// (after a restart, or a thread made before §21.17). An edit replaces the held message's text. A thread whose pick is
        /// already launching holds the message for the launch, and so does a thread whose bind is handing its messages over to
        /// the new topic (_handleMessage sends such a thread's message here). A thread whose bind has become its topic since the
        /// caller looked gets the message as that topic's (_handleTopicMessage) — the binds and the topics are read in one lock
        /// scope, and a bind leaves its list only after its topic is recorded, so a just-bound topic never gets a picker. Poll
        /// thread.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="msg">The Telegram message.</param>
        /// <param name="edited">True for an edited_message update.</param>
        void _holdForNewThread(int64_t threadId, const json::Value& msg, bool edited);
        /// <summary>
        /// Posts the thread's picker, or edits it in place when it is up (a page change, one more held message). The lists are
        /// built on first use (BuildNewThreadLists over the registry's PlaceSamples, the machine's temp roots left out, every
        /// listed folder probed once). Takes `_mtx` for the state and releases it before the send. Poll thread.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        void _showNewThreadPicker(int64_t threadId);
        /// <summary>
        /// A tap on a picker button (ParseNewThreadCallbackData): a pager arrow edits the picker to its page; a pager's page label
        /// changes nothing and answers which page it is (NewThreadPageToast); a folder starts the session (_startNewThreadSession);
        /// Cancel deletes the picker and drops what it held. A button of a picker this run no longer holds (a restart, a pick)
        /// answers that a new message in the thread brings a new picker. Takes `_mtx` for the picker's state. Poll thread.
        /// </summary>
        /// <param name="action">The button's action.</param>
        /// <param name="threadId">The thread the picker sits in (the callback's message).</param>
        /// <returns>The button's toast, plain text ("" = none).</returns>
        std::wstring _onNewThreadAction(const NewThreadAction& action, int64_t threadId);
        /// <summary>
        /// Starts the thread's NEW SESSION in `folder` (§21.17): a fresh Claude tab in the background of the most recently active
        /// window, under a session id minted here, recorded in `_threadBinds` BEFORE the launch so the tick can bind the thread to
        /// the session the moment it is live (_adoptBoundThreads); the picker is deleted. A folder that does not exist, or no
        /// window to open the tab, leaves the picker where it is and says why. §21.23: the window ANSWERS the launch — a refusal
        /// (no claude.exe on the PC, the tab could not be made, the window closed) sends the thread back to a new picker at once,
        /// with the messages it held and the reason (_settleLaunchAnswers), instead of the 3-minute timeout's "no window opened
        /// it".
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="folder">The working folder (a picker entry, or a path the user typed).</param>
        /// <param name="replyTo">The user's message that typed the folder (0 = a picker button). §21.23: for a typed folder this
        /// function posts the replies itself, quoting that message, and posts "Opening …" BEFORE the launch — so the window's
        /// answer, which the tick may post a moment later from another thread, can never land above it.</param>
        /// <returns>The outcome, plain text, for the caller to show: a button's toast; for a typed folder, a refusal made before
        /// the launch (the caller replies with it) or "" when this function replied itself.</returns>
        /// <exception cref="std::bad_alloc">If the bind or a reply cannot be allocated.</exception>
        std::wstring _startNewThreadSession(int64_t threadId, const std::wstring& folder, int64_t replyTo = 0);
        /// <summary>
        /// Binds each picked thread to its session once the session is LIVE (§21.17, tick thread, before the sync's plan so the
        /// plan never makes the session a topic of its own): the thread becomes the session's topic (a TopicRecord with the
        /// user's thread id, seeded like a created topic), is renamed like every topic (_renameTopic: the state, the color
        /// square, the title), gets its card, and receives the held messages as if they had been sent in the topic (the first
        /// one is the session's first prompt). The bind stays in `_threadBinds` until the topic is recorded and every message it
        /// holds is handed over: what the user writes meanwhile joins it and reaches the session after the earlier messages, in
        /// order (the record and the bind's messages change hands in one lock scope, and the bind leaves only in a scope that
        /// finds it empty). The first name's color is resolved fresh from the session's record, as a created topic's is, and
        /// becomes the sync's settled fact, so a folder color the tab's first paint persisted after the sync's dir-colors
        /// snapshot causes no second rename. A session at rest is held in "Waiting for you" (like a revive) so its topic
        /// outlasts the idle grace, and a tab that has not started yet is started. A bind whose session never came up within
        /// kNewThreadBindTimeoutMs is dropped, and the thread is told. §21.23: a launch the window REFUSED never waits that long
        /// (_settleLaunchAnswers sends the thread back to its picker at once), so the timeout says what is left — the PC never
        /// answered, or the tab opened and the session never came up (ThreadBind::answered) — and the messages the bind held
        /// wait in the thread's picker record for the next pick instead of being dropped.
        /// </summary>
        /// <param name="byId">The sync's live sessions (SnapshotLiveForCards), by id.</param>
        /// <param name="colorMode">The tab color mode of the sync.</param>
        void _adoptBoundThreads(const std::unordered_map<std::wstring, SessionInfo>& byId, TabColorMode colorMode);
        /// <summary>
        /// The owner renamed a session's topic in Telegram (forum_topic_edited): the session takes the name as its title, without
        /// the state glyph and color square the service shows before it (StripTopicNameDecoration) — like /rename, so the tab,
        /// the Manager and the topic follow (the sync renames the topic back to the decorated form and posts its change line). A
        /// name that strips to nothing, or to the title the session has, changes nothing. Poll thread.
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="threadId">The topic's thread.</param>
        /// <param name="name">The new name as Telegram reports it.</param>
        void _onUserTopicRenamed(const std::wstring& sessionId, int64_t threadId, const std::wstring& name);
        void _askSearch(const std::wstring& text, int64_t threadId);
        void _onSearchEvent(const SearchBridgeEvent& ev) noexcept;
        /// <summary>
        /// Posts a Search answer (§21.7): the model's sentence, the sessions of THIS request as numbered rows (each with
        /// where it stands now), one PICK button per row and a "⊘ Clear" button — with sound, the answer being the
        /// request's final message. Every listed row is remembered for the picks (`_searchRowInfo`).
        /// </summary>
        /// <param name="ev">The Answer event; its rows are the request's last shown list (empty when it showed none).</param>
        void _postSearchResults(const SearchBridgeEvent& ev);
        /// <summary>
        /// Makes the bridge to the Search conversation when this start of the bot has none yet (the factory set by the app
        /// at engine init). Poll thread (a message or a button); takes `_mtx`.
        /// </summary>
        /// <param name="threadId">Where to say so when there is no search in this build, or it could not start.</param>
        /// <returns>True when `_search` exists now.</returns>
        bool _ensureSearchBridge(int64_t threadId);
        /// <summary>
        /// CLEARS the Search conversation (the user's one action on it, §21.7 — the Sessions page's crossed circle): the
        /// agent forgets the context and deletes its record and shown list, and a silent line in the topic marks where
        /// the next request starts fresh. Works before any search of this run (the bridge is made for it, so a record left
        /// by an earlier run is cleared too) and on a parked conversation. Poll thread.
        /// </summary>
        /// <param name="threadId">The Search topic (where the line goes).</param>
        /// <param name="replyTo">The /clear message to answer, or 0 (a button).</param>
        void _clearSearch(int64_t threadId, int64_t replyTo);
        /// <summary>
        /// A PICK of a Search answer's row (§21.7): posts the ACTION MENU of that session as a reply to the answer — its
        /// title, folder and last activity as the answer listed them (else the registry's record, else the short id),
        /// where it stands NOW, and ▶ Resume / ▶ Resume 📌 / ⑂ Fork / ✕ Cancel as single-use tokens (M5) that carry the
        /// row number, folder and title. Silent (a menu is no final message). Poll thread.
        /// </summary>
        /// <param name="pick">The pick read from the button.</param>
        /// <param name="threadId">The Search topic.</param>
        /// <param name="answerMessageId">The answer message the pick button sits on (the menu replies to it); 0 = none.</param>
        void _showSearchPick(const SearchPick& pick, int64_t threadId, int64_t answerMessageId);
        /// <summary>
        /// Carries out a tapped action of a Search pick's menu, on the session's CURRENT state (it may have changed since
        /// the menu): Resume opens a closed session into a background tab, wakes an idle tab, and only says so for a session
        /// that has its topic already (with 📌 it pins it); Fork forks it into a new tab, never pinned (no fork-and-pin, the
        /// user's rule); Cancel deletes the menu. The menu message loses its buttons and keeps the choice (or, for an action
        /// done at once, the outcome). Poll thread.
        /// </summary>
        /// <param name="kind">The token's kind: "pick-resume", "pick-resume-pin", "pick-fork" or "pick-cancel".</param>
        /// <param name="sessionId">The picked session.</param>
        /// <param name="payload">The token's payload: "&lt;row number&gt;\n&lt;folder&gt;\n&lt;title&gt;".</param>
        /// <param name="threadId">The Search topic.</param>
        /// <param name="menuMessageId">The menu message the button sits on (edited or deleted); 0 = none.</param>
        void _onSearchPickAction(const std::wstring& kind, const std::wstring& sessionId, const std::wstring& payload, int64_t threadId, int64_t menuMessageId);
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
        /// <summary>
        /// The SECRETARY bot (§21.11), only when its token is stored: its client (made at _start, reset at Stop after the
        /// threads are joined; the send loop uses it for OutItem::asSecretary), its poll thread, its one-poller mutex, its
        /// id from getMe (0 until known) and name, and the FNV hash of the token the service started with (a cog Save
        /// with another token restarts the service).
        /// </summary>
        std::unique_ptr<TelegramApi> _secretaryApi;
        std::thread _secretaryThread;
        void* _secretaryMutex{ nullptr };
        std::atomic<int64_t> _secretaryBotId{ 0 };
        std::wstring _secretaryUsername; // under `_mtx`
        std::wstring _secretaryProblem; // under `_mtx`: why the secretary bot does not work ("" = no known problem)
        std::wstring _secretaryTokenHash; // "" = no secretary token at the start
        std::atomic<bool> _secretaryOn{ false }; // the secretary's client exists (StatusText reads it from any thread)

        int64_t _ownerId{ 0 }; // the paired Telegram user (AppSettings::messagingTelegramOwnerId)
        std::atomic<int64_t> _idleGraceMs{ 10 * 60 * 1000 };
        std::wstring _searchThinkingWord{ L"high" };
        /// <summary>
        /// §21.11: the default mode of the topics (AppSettings::messagingVerbose), whether Claude's Markdown goes as rich
        /// messages (AppSettings::messagingRichMessages), and — set once a send found the Bot API without them (a 404) —
        /// that rich messages are not available this run.
        /// </summary>
        std::atomic<bool> _defaultVerbose{ false };
        std::atomic<bool> _richMessages{ true };
        std::atomic<bool> _richUnavailable{ false };
        /// <summary>
        /// Tick thread: the last sync's facts (the state glyph renames read them, _stateNameTick). `_renameNoticeFailures`
        /// (any thread) counts the deletes of Telegram's "topic renamed" notices that were refused in a row — three stop
        /// the deletes, which would only cost calls.
        /// </summary>
        std::vector<TabFact> _lastFacts;
        std::atomic<int> _renameNoticeFailures{ 0 };
        /// <summary>
        /// §21.22: Telegram refused a topic ICON in this run (TopicIconRefused, read by _createTopic and _renameTopic), so the
        /// topics show their states as name glyphs again (_stateIconsWanted is false) until the bot starts again — _start clears
        /// it, and the next run tries the icons once more (one refused call per run when Telegram keeps refusing). Any thread.
        /// </summary>
        std::atomic<bool> _stateIconRefused{ false };
        /// <summary>
        /// §21.13, the tab colors the topic names follow. Tick thread: `_colorSince` is each live session's color as the last
        /// sync resolved it and since when it holds (0 = since its first sight in this run, which reads as settled) — the
        /// TabFact::colorSinceMs the name renames wait on; `_colorModeSeen` is the tab-color mode of the last sync (-1 = no sync
        /// yet) and `_colorModeChangedMs` when a sync saw it change (0 = not in this run), which keeps the re-colors of a mode
        /// switch quiet (NoticeOldSquare, kColorModeSettleMs). `_colorMode` is the same mode for the poll thread's replies
        /// (_titleSquare; -1 = no sync yet: they read the settings).
        /// </summary>
        std::unordered_map<std::wstring, std::pair<std::wstring, int64_t>> _colorSince;
        int _colorModeSeen{ -1 };
        int64_t _colorModeChangedMs{ 0 };
        std::atomic<int> _colorMode{ -1 };
        /// <summary>
        /// The single-topic recreations the user confirmed (§21.11; the poll thread queues, the tick runs them): session id
        /// and mode (0 the conversation only, 1 verbose). Under `_mtx`.
        /// </summary>
        std::deque<std::pair<std::wstring, int>> _recreateOne;
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
        /// <summary>
        /// §21.17: `_botUsername` again, for the title links (_linkBot), behind `_linkBotMtx` — a LEAF lock: nothing else is taken
        /// while it is held, so a send path can read it with `_mtx` held by its caller. Written where `_botUsername` is.
        /// </summary>
        mutable std::mutex _linkBotMtx;
        std::wstring _linkBotName;
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
        /// §21.18, the START NOTE. `_botStarts` counts the starts of the bot in this process (_start; only the first, at the
        /// app's launch, rings — ClassifyStartNote). `_versionWord` is the app's version word, read once per start
        /// (ServiceHooks::versionWord); the run marker and the note carry it — written by _start before the threads run, so
        /// the threads read it without a lock. `_startNote` holds the facts _ensureSearchTopic gathered (tick thread only).
        /// `_startNoteState` is where the note stands (kStartNote… in Messaging.cpp), `_startNoteRetryAtMs` when a note whose
        /// send failed goes again and `_startNoteAttempts` how many sends it took: atomics, since the send thread's answer
        /// (_onStartNoteSent) moves them while the tick thread reads them.
        /// </summary>
        size_t _botStarts{ 0 };
        std::wstring _versionWord;
        StartNoteFacts _startNote;
        std::atomic<int> _startNoteState{ 0 };
        std::atomic<int64_t> _startNoteRetryAtMs{ 0 };
        std::atomic<int> _startNoteAttempts{ 0 };
        /// <summary>
        /// §21.18: when the app's last window closed (NoteAppClosing, any thread; 0 = not closing). While it is younger than
        /// kAppClosingMarkerHoldMs the tick writes no run marker, so the close stamp stays the run's newest sign of life.
        /// </summary>
        std::atomic<int64_t> _appClosingAtMs{ 0 };
        /// <summary>
        /// §21.18, tick thread only: a refused creation of the Search topic is asked for again only from this time on
        /// (kSearchCreateRetryMs) — the start note re-creates a Search topic the user deleted, and a refusal that lasts must
        /// not become a createForumTopic call on every tick.
        /// </summary>
        int64_t _searchCreateRetryAtMs{ 0 };
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
        /// <summary>
        /// The editMessageText calls of the same census by WHAT they edit — "card", "prompt" (a §21.12 status), "worklog", or
        /// "other" (from the outbox item's key) — under `_mtx`, logged and cleared with the census as `edits(card=N …)`. Edits
        /// draw on the chat's flood budget like messages, and one earned a 6542 s retry_after (2026-10-17) with no way to tell
        /// from the log which kind had piled up; not counted in the line's call total (the census counts each call once).
        /// </summary>
        std::map<std::wstring, uint32_t> _editsByKind;
        int64_t _lastTrafficLogMs{ 0 };
        /// <summary>
        /// The topic list's ORDER keys (§21.8), refreshed by every sync from the live sessions: session id ->
        /// (MostRecentSortKeyMs, title). The tick works through the topics in the REVERSE of the list's order (the
        /// lowest-ranked first), so posts queued in one pass — replies, card re-posts — leave the highest-ranked topic
        /// on top. Tick thread only.
        /// </summary>
        std::unordered_map<std::wstring, std::pair<int64_t, std::wstring>> _orderKeys;
        /// <summary>
        /// A RECREATION is under way (tick thread only): set by _recreateTopics, cleared — with one summary line — when
        /// the orphans are gone and the sync has no topic left to make. `_recreatedCount` counts the topics it made.
        /// </summary>
        bool _recreating{ false };
        size_t _recreatedCount{ 0 };
        /// <summary>
        /// The user confirmed `/recreate` in the 🔎 Search topic (the poll thread sets it); the tick's next sync runs the
        /// RECREATION — after the start-up hold, never while one is under way.
        /// </summary>
        std::atomic<bool> _recreateRequested{ false };
        /// <summary>
        /// The history replay's batch in flight (§21.9), under `_mtx` (the send thread's answers count it down): the topic,
        /// where its window ended (`consumedTo`, committed to the topic only when `inFlight` reached 0), the day of its last
        /// turn, and a sequence number so an answer of a dropped batch never counts down the next one.
        /// </summary>
        struct BackfillBatch
        {
            bool active{ false };
            uint64_t seq{ 0 };
            std::wstring sessionId;
            int64_t threadId{ 0 };
            int64_t consumedTo{ 0 };
            int64_t dayKey{ 0 };
            size_t inFlight{ 0 };
            size_t posts{ 0 };
            int64_t startedMs{ 0 };
            size_t failed{ 0 }; // answers that were refusals (logged once per batch, with the first one's reason)
            std::wstring firstFailure;
            int64_t lastAnswerMs{ 0 }; // the batch's newest answer (WallMs): the stall guard counts from it, not from the start
        };
        BackfillBatch _backfill;
        uint64_t _backfillSeq{ 0 };
        /// <summary>
        /// Tick thread: the history messages queued per topic (the "history in" line names the count; erased then), and for
        /// the whole replay of this run (`_replayPosts` messages into `_replayTopics` topics — the summary after a
        /// recreation).
        /// </summary>
        std::unordered_map<std::wstring, size_t> _backfillPostsBy;
        size_t _replayPosts{ 0 };
        size_t _replayTopics{ 0 };
        /// <summary>
        /// Tick thread: when the running RECREATION started (or resumed at a start). The replay waits for the recreation's
        /// new list, but not past kRecreateListWaitMs — a topic Telegram keeps refusing to make must not hold every history.
        /// </summary>
        int64_t _recreateStartedMs{ 0 };
        /// <summary>
        /// Tick thread: a recreation's history replay is under way — when the last topic's history is in, the whole list is
        /// re-sorted (`_resortFull`). Also set at a start that finds a replay pending (the recreation of the last run).
        /// </summary>
        bool _backfillAfterRecreation{ false };
        bool _resortFull{ false };
        int64_t _lastResortMs{ 0 };
        bool _resortTooLongLogged{ false };
        /// <summary>
        /// Tick thread: the re-sort's re-posts still to make, lowest-ranked first. `_resortTick` releases ONE per tick, and
        /// only while the call window has room for its two calls (the old card's delete and the new card's send): a full
        /// re-sort is ~2 calls per topic, and all at once — ~60 calls a minute for 48 topics — is over the chat's flood
        /// budget. A re-post queued later can never overtake one queued before it (one priority, the outbox's order).
        /// </summary>
        std::deque<std::wstring> _resortQueue;
        std::unordered_set<std::wstring> _runningIds; // tick thread: the sessions the last sync saw Running (no re-sort while any runs)
        /// <summary>
        /// §21.16.7, tick thread only: the topics the user put FIRST with a directory dot — session id -> the session's order key
        /// (`_orderKeys`' MostRecentSortKeyMs) at the tap. The quiet re-sort leaves such a topic out of the list it puts in MOST
        /// RECENT order while that key is unchanged, so it does not re-post up to kResortMaxTopics other cards a minute later to
        /// push the topic back to its rank — which would undo the user's move. The session's next turn changes the key (a turn
        /// boundary), and from then on the topic is ordered like any other. Cleared with the re-sort's state (a start, a bot
        /// change, a recreation); a session whose topic is gone drops out at the next re-sort pass.
        /// </summary>
        std::unordered_map<std::wstring, int64_t> _frontHeld;
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
        /// <summary>
        /// The SECRETARY PROBE is due (a business connection arrived, or one was stored with no verdict yet) and
        /// waits for the Search topic to exist; the tick loop runs it. `_secretaryFailures` counts consecutive
        /// refused sends as the user once the probe said yes — three in a row turn the mode off again (-1), so a
        /// permission the user revoked does not cost a refused call per prompt.
        /// </summary>
        std::atomic<bool> _probeWanted{ false };
        std::atomic<int> _secretaryFailures{ 0 };
        /// <summary>
        /// The texts sent AS THE USER (secretary mode) in the last two minutes, per topic: the echo guard's memory
        /// (_takeSecretaryEcho). FNV-1a 32 of the trimmed text. Under `_mtx`.
        /// </summary>
        struct SecretaryEcho
        {
            int64_t threadId{ 0 };
            uint32_t hash{ 0 };
            int64_t expiresMs{ 0 };
        };
        std::vector<SecretaryEcho> _secretaryEchoes;

        std::deque<OutItem> _outbox;
        uint64_t _outSeq{ 0 };
        int64_t _lastSendMs{ 0 };
        /// <summary>
        /// The CALL WINDOW (§21.9), under `_mtx`, GetTickCount64 ms: when each call into the chat of the last 60 s was made
        /// (the send loop's items and the tick's direct calls alike). A HISTORY message goes only while it holds fewer than
        /// kHistoryCallsPerMinute: Telegram's flood budget for this chat drains near the documented group limit of 20
        /// messages a minute — the first replay, ~27 a minute for 11 minutes, earned a 429 (live, 2026-10-16 23:59:54).
        /// </summary>
        std::deque<int64_t> _recentCallsMs;
        /// <summary>When the last 429 came (GetTickCount64 ms; 0 = none this run): history waits kHistoryFloodPauseMs after it, live traffic does not.</summary>
        int64_t _floodAtMs{ 0 };
        /// <summary>
        /// The LONG-FLOOD PARKING (MessagingModels.h FloodParking), under `_mtx`: a 429 whose retry_after is above
        /// kFloodParkAfterSec parks only the refused method (the send loop serves the other methods' items, and the tick's
        /// direct topic calls skip a parked method until a later tick); a second method refused meanwhile parks every method.
        /// Before 2026-10-17 the thread that met the 429 slept the whole retry_after — 6542 s once, with nothing sent.
        /// </summary>
        FloodParking _floodParking;
        /// <summary>
        /// The same parking for the SECRETARY bot's sends (§21.11: items with OutItem::asSecretary go out with its client), under
        /// `_mtx`: its flood control is that bot's own, so its 429s never park — nor escalate to — this bot's methods.
        /// </summary>
        FloodParking _secretaryFloodParking;
        /// <summary>
        /// When a due topic rename first found the call window full (GetTickCount64 ms; 0 = no rename waits), under `_mtx`. Set
        /// and cleared by _stateNameTick; while set (at most kNameRenameHoldMaxMs) the send loop holds HISTORY items, so the
        /// window drains and the rename goes before the replay (HistoryYieldsToRename) — without it the replay kept the window
        /// full for hours and no topic name followed its session's state.
        /// </summary>
        int64_t _renameWaitingSinceMs{ 0 };
        /// <summary>
        /// When the SLOW FORM UPGRADE (§21.15, _takeCardFormatSlot) last re-edited a card whose text was unchanged (GetTickCount64
        /// ms; 0 = none this run), under `_mtx`: the next such edit waits kCardFormatGapMs, so turning title chips on (the first
        /// start of this build, or the rich switch) converts the cards one at a time instead of editing every card at once —
        /// 47 cards edited together earned a 429 with a 21-minute retry_after (2026-10-16).
        /// </summary>
        int64_t _lastCardFormatEditMs{ 0 };
        /// <summary>
        /// A title token reached the outbox unrendered (§21.15 belt in _enqueue: rendered classic there) — logged once per run,
        /// since it means a send path that bypasses _sendText / _editText.
        /// </summary>
        std::atomic<bool> _titleTokenLeakLogged{ false };
        /// <summary>
        /// The DIRECTORY's run-time state (§21.16; what the message shows is persisted in MessagingState). Tick thread:
        /// `_directoryCheckedMs`, when the last check ran (WallMs; 0 = none this run). Under `_mtx`: `_directoryInFlight` — a post
        /// or an edit waits for its answer (since `_directoryInFlightSinceMs`, the stall clock), so no second one is queued;
        /// `_directorySerial` numbers the sends, so an answer of a send that a stall made the service forget is recognized;
        /// `_directoryLastSendMs` — when the last send was queued (the edit gap); `_directoryNotBeforeMs` — a refusal's back-off
        /// (0 = none); `_directoryRichRefusedHash` — the content whose rich form Telegram refused (DirectoryShown); and
        /// `_directoryPinTried` — this run asked to pin the current message already. All reset at each start (_start).
        /// </summary>
        int64_t _directoryCheckedMs{ 0 };
        bool _directoryInFlight{ false };
        int64_t _directoryInFlightSinceMs{ 0 };
        uint64_t _directorySerial{ 0 };
        int64_t _directoryLastSendMs{ 0 };
        int64_t _directoryNotBeforeMs{ 0 };
        uint32_t _directoryRichRefusedHash{ 0 };
        bool _directoryPinTried{ false };
        // The first edit of a run is logged (the later ones only when they fail): the directory may be edited every two minutes.
        bool _directoryEditLogged{ false };
        /// <summary>
        /// Parks `method` for a long 429 (ParkForFlood under `_mtx`) and logs what is held until when (a local clock time). Takes
        /// `_mtx`.
        /// </summary>
        /// <param name="method">The refused method.</param>
        /// <param name="retryAfterSec">Telegram's retry_after (above kFloodParkAfterSec).</param>
        /// <param name="secretary">True for a send of the SECRETARY bot (parks in _secretaryFloodParking, never this bot's methods).</param>
        /// <exception cref="std::bad_alloc">If the parking or the log line cannot grow.</exception>
        void _parkForFlood(const std::wstring& method, int retryAfterSec, bool secretary = false);
        /// <summary>
        /// Telegram's verdict on one call made on the TICK thread (a topic create / delete / rename): a long 429 parks the
        /// method (_parkForFlood) and returns at once; a short one waits it out here, stop-aware. Either way the history replay
        /// pauses (_noteFlood). Anything but a 429 returns at once. A topic call that met a 429 is NOT retried here: its
        /// caller leaves it for a later tick.
        /// </summary>
        /// <param name="method">The method that was called.</param>
        /// <param name="r">Its answer.</param>
        /// <returns>True when the answer was a 429 (the call did not happen).</returns>
        bool _handleTickFlood(const std::wstring& method, const TgResult& r);
        /// <summary>Records one call in the call window and drops what is older than a minute. Caller holds `_mtx`.</summary>
        /// <param name="tickMs">GetTickCount64 at the call.</param>
        void _noteCallLocked(int64_t tickMs);
        /// <summary>A 429: the history replay pauses (logged once per 429). Takes `_mtx`.</summary>
        /// <param name="method">The refused method (for the log line).</param>
        /// <param name="retryAfterSec">Telegram's retry_after.</param>
        void _noteFlood(const std::wstring& method, int retryAfterSec);

        std::unordered_map<std::wstring, Mirror> _mirrors;
        std::unordered_map<std::wstring, CardState> _cards; // session id -> its card's transient state (CardState)
        std::unordered_map<std::wstring, PromptStatus> _reacted; // prompt id -> the status its Telegram message shows
        std::unordered_map<std::wstring, int64_t> _dirty; // session ids the registry notified since the last tick (-> ms)
        /// <summary>
        /// §21.16.7, under `_mtx`: the topics a directory DOT put first, not yet seen by the tick — session id -> when the tap
        /// came (WallMs). The poll thread writes it (_bringTopicToFront); the tick folds it into `_frontHeld` at its next
        /// re-sort pass, because the order key it records there (`_orderKeys`) is the tick thread's alone.
        /// </summary>
        std::unordered_map<std::wstring, int64_t> _frontRequests;
        /// <summary>
        /// §21.16.7, under `_mtx`: the last dot tap per THREAD (WallMs). A second tap on the same dot within kFrontTapGapMs is
        /// answered without a second re-post: one double tap must not cost four calls of the chat's flood budget.
        /// </summary>
        std::unordered_map<int64_t, int64_t> _frontTapMs;

        struct Token
        {
            std::wstring kind;
            std::wstring sessionId;
            std::wstring payload;
            int64_t expiresMs{ 0 };
        };
        std::unordered_map<std::wstring, Token> _tokens;
        uint64_t _tokenSeq{ 0 };

        /// <summary>
        /// Under `_mtx`: a launch _requestLaunch asked for, until the session it opened is live (_sweepRevives), the window refused
        /// it (_settleLaunchAnswers) or kRemoteLaunchTimeoutMs ran out (_expireRevives). §21.23: recorded BEFORE the request goes
        /// out, under the launch's token; the window's answer fills in what it opened.
        /// </summary>
        struct Revive
        {
            uint64_t launchToken{ 0 }; // §21.23: the key the launch's answer comes back with (_askLaunch)
            bool announced{ false }; // §21.23: _requestLaunch posted its "… into a tab…" line — a refusal may now be posted after it
            bool answered{ false }; // §21.23: the window answered that a tab hosts the session (openedId)
            std::wstring openedId; // §21.23: the session the window opened: the one the revive settles on, and no other
            bool alreadyOpen{ false }; // §21.23: a resume of a session that was open already (nothing new opened)
            bool fresh{ false }; // §21.23: a NEW conversation opened in place of the asked one (no transcript to continue)
            RemoteLaunchRequest req;
            int64_t requestedMs{ 0 };
            int64_t threadId{ 0 }; // where to report
        };
        std::vector<Revive> _revives;
        // §21.24: the pictures of screens being drawn right now (_startScreenImage's worker threads) - at most
        // kScreenImageMaxRenders at once, each one ~90 to ~185 ms of drawing (measured for a 200 x 60 to 300 x 80 screen).
        std::atomic<int> _screenImageRenders{ 0 };
        // §21.24, under `_mtx`: when each session's screen picture was last asked for (WallMs). A second request within
        // kScreenImageTapGapMs asks nothing new: a double tap must not draw and send two pictures on the chat's call budget.
        std::unordered_map<std::wstring, int64_t> _screenImageTapMs;

        /// <summary>
        /// §21.17, under `_mtx`: a thread the user opened that no session owns yet, and its PICKER. In memory only: after a restart
        /// the next message in the thread posts a new picker (the old one's buttons answer that they are from before the restart).
        /// </summary>
        struct NewThread
        {
            std::wstring topicName; // the name the user gave the thread ("" = Telegram named it, or it is not known)
            NewThreadLists lists; // built on first show (listsBuilt)
            bool listsBuilt{ false };
            size_t recentPage{ 0 };
            size_t usedPage{ 0 };
            int64_t pickerMessageId{ 0 }; // the picker (0 = not posted, or its post was refused; -1 = the post is in flight)
            bool pickerStale{ false }; // a page change or a held message came while the post flew: its answer renders it again
            std::vector<json::Value> held; // the user's messages in the thread, oldest first (each a copy of the Message object)
            int64_t lastUseMs{ 0 }; // the last message or tap (WallMs): a picker idle past kNewThreadPickerIdleMs is forgotten
        };
        std::map<int64_t, NewThread> _newThreads; // thread id -> its picker
        /// <summary>
        /// §21.17, under `_mtx`: a picked folder's launch, waiting for its session to be live — the session id the launch was
        /// given (RemoteLaunchRequest::newSessionId), the thread it becomes the topic of, the title it was given ("" = the
        /// folder-derived default) and the user's messages to hand over. The tick binds it (_adoptBoundThreads) and keeps it
        /// here, as the thread's hand-over queue, until the topic is recorded and every held message has been handed over: a
        /// thread that has a topic and a bind is being bound right now, and its new messages join `held`.
        /// </summary>
        struct ThreadBind
        {
            uint64_t launchToken{ 0 }; // §21.23: the key the launch's answer comes back with (_askLaunch)
            bool answered{ false }; // §21.23: the window opened the tab — a timeout then says the session never came up, not that the PC never answered
            int64_t replyTo{ 0 }; // §21.23: the user's message that typed the folder (0 = a picker button): a refusal's reply quotes it
            std::wstring sessionId;
            int64_t threadId{ 0 };
            std::wstring dir;
            std::wstring title;
            std::vector<json::Value> held;
            int64_t requestedMs{ 0 }; // WallMs of the launch: kNewThreadBindTimeoutMs counts from it
        };
        std::vector<ThreadBind> _threadBinds;
        uint64_t _launchTokenSeq{ 0 }; // §21.23, under `_mtx`: the last launch token handed out (0 is never one)
        /// <summary>
        /// §21.23: one window's answer to a launch of this service, waiting for the tick (_settleLaunchAnswers).
        /// </summary>
        struct LaunchAnswerItem
        {
            uint64_t token{ 0 }; // the launch's key (Revive::launchToken / ThreadBind::launchToken)
            RemoteLaunchOutcome outcome; // the window's answer
            int64_t atMs{ 0 }; // WallMs when it came: a refusal whose revive never says it announced itself goes out after 5 s anyway
        };
        // §21.23: the launch answers not applied yet, behind their OWN leaf lock — an answer comes from any thread (a window's
        // UI thread, the engine's), possibly inside _askLaunch's call, so it must not need `_mtx`. `_launchAnswersPending` wakes
        // the tick (its wait reads it with `_tickRequested`): a wake-up lost to the race between the two costs one tick, 1.5 s.
        std::mutex _launchAnswerMtx;
        std::vector<LaunchAnswerItem> _launchAnswers;
        std::atomic<bool> _launchAnswersPending{ false };

        SearchBridgeFactory _searchFactory;
        std::unique_ptr<SearchBridge> _search;
        /// <summary>
        /// The rows the Search answers of this run listed, by session id (title, folder, last activity as the answer
        /// showed them): what a stateless PICK opens its menu with, and the folder and title a Resume / Fork hands to the
        /// launch (a session Agentmaster never managed has no other source for its title). `_searchRowOrder` is the
        /// insertion order for the cap (kSearchRowMemory, the oldest go first). Not persisted: after a restart a pick
        /// falls back to the registry's record, else the short id, and the launch reads the folder from the transcript.
        /// Under `_mtx`.
        /// </summary>
        std::unordered_map<std::wstring, SearchRow> _searchRowInfo;
        std::deque<std::wstring> _searchRowOrder;
        int64_t _searchLastUsedMs{ 0 };
        bool _searchParked{ true };
    };
}
