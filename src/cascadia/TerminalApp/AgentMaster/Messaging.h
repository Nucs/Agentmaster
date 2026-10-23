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

#include <array>
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

#include "AskQuestion.h" // §21.30: Claude's question dialogs answered from Telegram (the card's model, its renders, the decision lines)
#include "InterruptedTurns.h" // HOOKS.md "Interrupted turns": RestartTurnChoice / ForkTurnChoice — the answers the topic's restart and fork questions pass on
#include "MessagingDirectory.h" // the pinned directory's model and renders (§21.16)
#include "MessagingFileLinks.h" // §21.32.4: the file links' pure rules (the link, the session key, the linked set)
#include "MessagingFileServer.h" // §21.32.4: the file links' server, secret and LAN addresses (no Windows header in it)
#include "MessagingStartNote.h" // the start note's facts, kind, sound and text (§21.18)
#include "MessagingModels.h"
#include "MessagingNewThread.h" // §21.17: the new-thread folder picker (lists, text, buttons, callback data)
#include "MessagingNewThreadWizard.h" // §21.25: the new-thread wizard (create a folder, a model, a window; records + persistence)
#include "MessagingTaps.h" // §21.28: a tapped button's answer, held while its work runs (TapAnswer, TapHold, TapScope)
#include "MessagingSteer.h" // §21.33: /steer, the words sent NOW (the decision, the texts, the prompt, the watch's verdict)
#include "TuiMenus.h" // §21.34: a local command's dialog answered from Telegram (/add-dir: the screen parser, the press, the card)
#include "RemoteLaunch.h"
#include "WindowTabTools.h" // §21.25: WindowCensus (ServiceHooks::windowCensus)
#include "SessionModels.h" // AppSettings, SessionInfo
#include "TelegramApi.h" // TelegramApi + TgResult (the outbox's completion callbacks name TgResult)

namespace Agentmaster
{
    class SessionRegistry;
    class Scheduler;
    class AskRegistry; // §21.30: the open question dialogs whose hook waits for an answer (HooksBridge.h)
    struct AskEntryInfo; // §21.30: one of them, as AskRegistry::Snapshot shows it (HooksBridge.h)
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

    // ===== the launch-model menu, behind a seam (§21.25) =====================================

    /// <summary>
    /// What the new-thread wizard's MODEL step shows (§21.25): the right-click menu's own list — the configured launch models,
    /// then every visible preset as an "@id" target (LaunchPresetsOs.h LaunchMenuEntries) — and whether the user has no preset
    /// at all, in which case the step says where to add one, as the PC's menus offer "Add presets…".
    /// </summary>
    struct LaunchMenuInfo
    {
        std::vector<std::pair<std::wstring, std::wstring>> entries; // {display name, launch target}, the menu's order
        bool offerAddPresets{ false }; // ShouldOfferAddPresets(): the preset list is empty
    };

    /// <summary>
    /// Reads the launch-model menu for a launch-models spec (AppSettings::launchModels). The presets live in app-only code (the
    /// presets file, Windows Credential Manager — LaunchPresetsOs.h, which the m5 harness does not build), so the app layer sets
    /// it (TerminalPage::_InitAgentmasterEngine); with none set the model step offers Default and a typed id only. Called on the
    /// service's poll thread; it must be thread-safe (the presets loader is stamp-cached under its own lock) and may throw
    /// (std::bad_alloc), which the caller contains.
    /// </summary>
    using LaunchMenuSource = std::function<LaunchMenuInfo(const std::wstring& launchModelsSpec)>;

    // ===== what the service needs from the app ================================================

    // Each member may be empty (the test harness): the service then answers "not available".
    struct ServiceHooks
    {
        // A window-hosted action on a session's tab: "start" (start a dormant tab's claude in place, no
        // focus), "screen" (reply with the terminal's visible text), "markunread", "refresh-tags" (repaint
        // the tab's tag badges after a remote tag edit), "refresh-favorite", and §21.34's "menu-screen"
        // (the terminal's last rows, for the dialog a delivered /add-dir opened) and "menu-answer" (press
        // that dialog's option — only after the screen shows it: TuiMenus.h DecideMenuAnswer; its reply is
        // MenuAnswerResultWord). `reply` is called by the hosting window with the action's text answer (may
        // be empty); a window that does not host the session calls nothing.
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
        // HOOKS.md "Interrupted turns": restartSession carries the topic's answer to "continue the turn you interrupted?"
        // (KeepInterrupted for every restart that asks nothing — a running turn cut off, an errored session reactivated),
        // so the PC never shows a dialog for a restart the phone asked for.
        std::function<void(const std::wstring& sessionId, RestartTurnChoice turnChoice)> restartSession;
        std::function<void(const std::wstring& sessionId)> deactivateSession;
        // The app's version word for the start note and the run marker (§21.18): "0.6.8.21", "dev 0.0.1.28", "portable
        // 0.7.9.0" (MessagingStartNote.h FormatAppVersionWord; Engine.cpp reads the package identity or the portable stamp).
        // Asked once per start of the bot. Empty, or "", = the note shows no version and the marker carries none.
        std::function<std::wstring()> versionWord;
        // How many open windows have not finished their first-layout restore (Engine::WindowsStillRestoring).
        // While it is above 0, a session that reads live=false may only be waiting for its tab to be re-homed,
        // so the service holds its "closed" topic deletes (the START-UP HOLD, §21.4). Empty = 0.
        std::function<size_t()> windowsRestoring;
        // §21.25: every window's tab strip (Engine.h CollectWindowCensus), waiting at most `timeoutMs` for the slowest window —
        // the new-thread wizard's window step (each window's tab count, newest tab and titles). It BLOCKS until every window
        // answered or the time is up, and must never run on a window's UI thread: the service calls it on a worker thread of
        // its own. Empty = no window step (the launch goes to the engine's choice of window).
        std::function<std::vector<WindowCensus>(uint32_t timeoutMs)> windowCensus;
    };

    // ===== the service ========================================================================

    // A TapReleaseSink (§21.28) too: a tapped button's last hold tells the service, which then queues the tap's answer
    // (OnTapHoldsGone, private — only a TapHold calls it, through the interface).
    class MessagingService : public std::enable_shared_from_this<MessagingService>, public TapReleaseSink
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
        /// a clean quit read as a crash. It then FLUSHES the state file (_saveStateIfDirty, forced): the mirror's progress is
        /// saved only every kStateProgressSaveGapMs, and the next start re-reads every transcript from the saved cursors, so
        /// a cursor lost at a quit would post up to that many seconds of messages twice. Any thread. It runs under the
        /// engine's window mutex, so it never waits for a service lock WITHOUT A BOUND: the flush waits at most
        /// kAppCloseSaveLockWaitMs (250 ms) for each of `_stateSaveMtx` and `_mtx` and is skipped, with a log line, when one
        /// stays held that long; the save itself (serialize + write + flush, tens of ms with 441 topics) runs on the
        /// caller's thread. Never throws: a failed write is logged, and the next note then reads the end as unexpected — the
        /// safe side, since a false "it closed" would hide a crash.
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
        /// §21.32.4: one line for the cog's FILE LINKS section — off, serving on a port as a station with the links' base and
        /// the linked-file counts, or why the server is not running (a port in use, no secret). Any thread.
        /// </summary>
        /// <returns>The status text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be built.</exception>
        std::wstring FileLinksStatusText() const;

        /// <summary>
        /// §21.32.4: the Windows Credential Manager target of the install's file-link SECRET (the session keys' eight initial
        /// words): "Agentmaster:messaging:telegram-filelinks:&lt;profile folder leaf&gt;". Per profile, like the bot token, so the
        /// release and dev installs never share keys. Deleting the credential makes a new secret at the next start — every
        /// link the bot ever posted then stops working.
        /// </summary>
        /// <param name="profileDir">The profile folder.</param>
        /// <returns>The target name.</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be built.</exception>
        static std::wstring FileLinksCredentialTarget(std::wstring_view profileDir);

        /// <summary>
        /// Sets the factory that makes the Search topic's agent on first use. Called once at engine init by
        /// the app layer (the harness never sets one: the Search topic then says search is not available).
        /// </summary>
        /// <param name="factory">The factory; an empty one removes the current factory.</param>
        void SetSearchFactory(SearchBridgeFactory factory) noexcept;

        /// <summary>
        /// §21.25: sets the source of the launch-model menu the new-thread wizard's model step lists (LaunchMenuSource). Called once
        /// at engine init by the app layer (the harness never sets one: the step then offers Default and a typed model id only).
        /// Any thread. Never throws: a failed store is logged, and the step goes on without the menu.
        /// </summary>
        /// <param name="source">The source; an empty one removes the current source.</param>
        void SetLaunchMenuSource(LaunchMenuSource source) noexcept;

        /// <summary>
        /// §21.30: hands the service the process's QUESTION registry (Engine.cpp, before the first Apply) — the open AskUserQuestion
        /// dialogs whose question hook waits on the ask pipe for an answer. The service wakes its tick on every change of the
        /// registry (its listener), and while the bot runs it is the registry's ACCEPTOR: a question of a live Claude session of
        /// the fleet is taken, shown in the session's topic as a QUESTION CARD and answered from it; while the bot is off every
        /// question is refused at once and stays with the PC. Without a registry (the m5 harness) a question is posted show-only,
        /// as before. Any thread; call it once. Never throws: a failure is logged, and the questions then stay with the PC.
        /// </summary>
        /// <param name="registry">The registry (null removes the current one: its listener and acceptor are cleared).</param>
        void SetAskRegistry(std::shared_ptr<AskRegistry> registry) noexcept;

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
        /// <summary>
        /// Starts the bot with `token`: the client(s), the one-poller mutex, the state file, every run-time table cleared or restored
        /// (§21.28: the last run's tapped buttons are marked answered and forgotten — Telegram stopped waiting for them long ago),
        /// the registry observer, then the poll, send and tick threads (and the secretary's poll thread). Never throws: a failure
        /// is logged and the status line says the bot failed to start.
        /// </summary>
        /// <param name="token">The bot token (from Credential Manager).</param>
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
        /// <summary>
        /// The SEND thread: one outbox item per pass, by priority then sequence, paced to the chat's ~1 call/s (kSendGapMs; history
        /// slower, within kHistoryCallsPerMinute), a long 429 parking only its method; the item's `onDone` runs after the call.
        /// §21.28: each pass first queues the answers of the tapped buttons that are due (_answerDueTaps), and an item's callback
        /// runs inside a TapScope of the item's holds, which go with the item at the end of the pass. Never throws (Rule #18
        /// inside: a failed pass is logged and the loop goes on after 0.5 s).
        /// </summary>
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
        /// places. Never throws (Rule #18 inside). §21.28: a refusal's notice (and the wizard posted after it) goes inside a scope
        /// of the launch's tap holds — the revive's, or the new thread's (`_bindTapHolds`) — so it answers the tapped button; a new
        /// thread's opened tab lets its holds go, and a revive's are kept for its "Opened …" line.
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
        /// <summary>
        /// Reads state.json (or its backup state.json.prev when state.json is torn) into `_state` at a bot start, and records the
        /// bytes it read as the file's content (`_lastSavedStateBytes`), so the first save of an unchanged state writes nothing.
        /// </summary>
        /// <remarks>Clears both dirty flags and the backup's refresh stamp (the first save of this start refreshes the backup), and
        /// records whether state.json itself was readable (`_stateFileReadable`: a torn or unknown file is never copied over the
        /// backup).</remarks>
        /// <exception cref="std::bad_alloc">On allocation failure; `_state` is then left as it was.</exception>
        void _loadState();
        /// <summary>
        /// Saves the state file when a save is DUE (StateSaveDue): a structural change within kStateSaveGapMs, the live mirror's
        /// progress alone within kStateProgressSaveGapMs. The file is serialized whole; bytes equal to the last write are not
        /// written at all (no copy, no write, no flush), and the backup copy is refreshed at most once a minute
        /// (StatePrevCopyDue), never from a state.json this run could not read. A failed write marks the state dirty again for
        /// the next tick.
        /// </summary>
        /// <remarks>
        /// Saves are serialized by `_stateSaveMtx`, taken BEFORE `_mtx` (lock order: `_stateSaveMtx` → `_mtx`); a caller must
        /// hold neither. With a `lockWaitMs` of 0 or more (the app-close flush, NoteAppClosing, which runs under the engine's
        /// window mutex) each lock is waited for at most that long, and the save is skipped (logged) when one stays held, so the
        /// caller never depends on a bot thread making progress.
        /// </remarks>
        /// <param name="force">Save now whatever the gaps, if anything is dirty (Stop, the recreation's orphan list, the app's close).</param>
        /// <param name="lockWaitMs">The longest wait for each lock in milliseconds; negative (the default) = wait as long as it takes.</param>
        /// <exception cref="std::bad_alloc">Serializing may fail to allocate; the dirty flags are then already cleared and the next
        /// change sets them again (the file on disk is untouched).</exception>
        /// <exception cref="std::system_error">From std::unique_lock on a lock failure the OS reports (not expected).</exception>
        void _saveStateIfDirty(bool force, int64_t lockWaitMs = -1);

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
            /// <summary>
            /// §21.28: the tapped buttons whose ANSWER waits for this call — one hold per tap whose handler (or one of its
            /// continuations) queued the item inside its tap scope (_enqueue adds them). The send loop runs `onDone` inside a scope
            /// of these holds, so what the callback queues in turn holds them too, and drops them with the item after the
            /// callback: the last hold of a tap to go lets its answer go. A newer item that replaces this one by its key takes them
            /// over (it carries the change the tap waits for); an item dropped from the outbox lets them go at once.
            /// </summary>
            std::vector<TapHoldRef> tapHolds;
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
        /// <summary>
        /// Queues an item; an item with no seq yet (0) gets the next one, an item that carries one keeps it (a fallback, a chunk).
        /// A queued item with the same non-empty key is replaced. §21.28: inside a TapScope (a tapped button's handler or a
        /// continuation of its work) the item also gets one hold per tap in scope — the tap's answer then waits until Telegram has
        /// answered this call — and a replaced item hands its holds to its replacement; an answerCallbackQuery item never holds.
        /// </summary>
        /// <param name="item">The item (moved in).</param>
        /// <exception cref="std::bad_alloc">If the item, its holds or the outbox cannot grow (a hold made so far goes, which may
        /// answer a tap early, never late).</exception>
        void _enqueue(OutItem item);
        /// <summary>The next outbox sequence number (for a message whose parts and fallbacks must keep one place). Takes `_mtx`.</summary>
        uint64_t _takeOutSeq();

        // ----- §21.28: a tapped button's answer, held while its work runs (MessagingTaps.h) -----

        /// <summary>
        /// TapReleaseSink: the last hold of `tap` went (its work is done or dropped), so its answer may go. Queues the tap for the
        /// send loop (`_tapsDue`, under the LEAF lock `_tapMtx`) and wakes it; the answer itself is queued there (_answerDueTaps),
        /// never here — this runs on whatever thread let the hold go, possibly with `_mtx` held (an outbox item dropped under it),
        /// and _enqueue takes `_mtx`. Never throws: a tap that cannot be queued (std::bad_alloc, logged) is answered at its hold's
        /// limit instead, since it is still in `_openTaps`.
        /// </summary>
        /// <param name="tap">The tap whose holds count reached 0.</param>
        void OnTapHoldsGone(const std::shared_ptr<TapAnswer>& tap) noexcept override;
        /// <summary>
        /// One new hold for every unanswered tap of the calling thread's TapScope (HoldTapsOfScope with this service as the sink):
        /// what a continuation captures to finish a tap's work on another thread or later, and what an outbox item queued in a
        /// scope keeps (_enqueue). Empty outside a scope — the fleet's own traffic, nearly every call.
        /// </summary>
        /// <returns>The holds (empty outside a scope).</returns>
        /// <exception cref="std::bad_alloc">If a hold cannot be allocated.</exception>
        std::vector<TapHoldRef> _holdTaps();
        /// <summary>
        /// Keeps the taps of the calling thread's scope waiting for the NEXT change of `sessionId`'s sticky card (CardState::tapHolds):
        /// a tapped 🔄 Restart / ▶ Reactivate / 🔧 Resume or a confirmed restart, park or interrupt — whose work happens in the
        /// window and shows on the card as the new state — and a card refresh that cannot change the card yet. Nothing outside a
        /// scope, and nothing for a session without a topic (no card will change: the tap is answered when its other holds go).
        /// Takes `_mtx`: never call it with `_mtx` held.
        /// </summary>
        /// <param name="sessionId">The session whose card the taps wait for.</param>
        /// <exception cref="std::bad_alloc">If a hold or the stash cannot be allocated.</exception>
        void _awaitCardChange(const std::wstring& sessionId);
        /// <summary>
        /// The send loop's look at the taps, once per pass, before it takes `_mtx`: every tap whose last hold went (`_tapsDue`) and
        /// every open tap past its hold's limit gets its answer queued (_answerTap); answered taps leave `_openTaps`. Send thread.
        /// Never throws (Rule #18 inside: a pass that fails leaves the rest to the next pass, and the buttons to Telegram's limit).
        /// </summary>
        void _answerDueTaps() noexcept;
        /// <summary>
        /// Queues `tap`'s answer (answerCallbackQuery) — once: the first of the last hold's release and the hold's limit claims it
        /// (TapAnswer::TryClaimAnswer), every later call does nothing. It carries the handler's toast, or kTapStillWorkingToast when
        /// the limit answers a tap whose work still runs (TapAnswerText). The answer goes ahead of every other item of its priority
        /// (it is no message of the chat, and the button spins until it lands); a refusal (an answer Telegram found too late) is
        /// logged. One "[msg] tap …" line in hooks.log per tap says how long the button was held and why it stopped. Send thread.
        /// </summary>
        /// <param name="tap">The tap.</param>
        /// <param name="byLimit">True when the hold's limit (kTapAnswerHoldMaxMs) answers it, false when its last hold went.</param>
        /// <param name="nowTickMs">GetTickCount64 now (the log's held time).</param>
        /// <exception cref="std::bad_alloc">If the item or the log line cannot be allocated.</exception>
        void _answerTap(const std::shared_ptr<TapAnswer>& tap, bool byLimit, int64_t nowTickMs);
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
        /// The card's buttons on one page (§21.24 - Main: ⚙ Session, plus 🔄 Restart Session in Error or after an exit, or
        /// ▶ Reactivate for a parked tab; Session: ⑂ Fork · 🆕 New Session, 🔄 Restart Session with 🔧 Resume Session beside it in
        /// Error, the card's other buttons of before, 🖼 Screen Image and ◀ Back). BuildCardButtonRows lays them out.
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
        /// `prio` is the outbox priority of the old card's delete and the new card's send: the cards' own (CardEdit) by default;
        /// SendPriority::Final when a tapped button waits for this post (§21.28: a directory dot's re-post, a tap's change that met
        /// a missing card), so the user's wait is not spent behind the fleet's work logs and card edits.
        /// </summary>
        void _postCard(const std::wstring& sessionId, int64_t threadId, const std::wstring& text, json::Value markup, int64_t deleteFirst, bool ping, uint32_t textHash, SendPriority prio = SendPriority::CardEdit);
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
        /// <summary>
        /// Deletes a session's topic with deleteForumTopic and forgets it: the record, the mirror, the card state, the outbox items
        /// queued for it and its history batch. A refusal of Telegram's flood control keeps the record (the sync plans the delete
        /// again after the pause); any other failure drops the record anyway, and a topic already gone is no failure. §21.31: a
        /// topic deleted because its session went IDLE (`reason` "idle", the sync's D25 grace) also joins the main view's idle
        /// notice (_idleNoticePending, told by _flushIdleNotices), with the title and the square its name showed. Tick thread (a
        /// direct call, paced by _awaitCallSlot).
        /// </summary>
        /// <param name="sessionId">The session whose topic goes.</param>
        /// <param name="reason">Why, for the log and the idle notice: "idle", "closed", "not a claude tab", "left its tab", ….</param>
        void _deleteTopic(const std::wstring& sessionId, const std::wstring& reason);
        /// <summary>
        /// Tells the chat's MAIN VIEW which sessions went idle (§21.31, the user's request of 2026-10-19: "send a message in the all
        /// chat that &lt;title&gt; is idle"): once IdleNoticeDue says the collected deletes have settled, ONE silent line names them
        /// all (BuildIdleNoticeHtml), so a burst — an app start lets every expired idle topic go at once — never floods the main
        /// view or the chat's flood budget. The pending entries live in memory only: a quit within the wait (at most
        /// kIdleNoticeMaxWaitMs) loses that line, never anything else. Tick thread, after the topic sync.
        /// </summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        void _flushIdleNotices(int64_t nowMs);
        /// <summary>
        /// Updates a session's topic in ONE editForumTopic: its ICON to the state's (StateTopicIcon) and its name to
        /// TopicDisplayName(state, square, title, true) while state icons are wanted (§21.22) — a state change then changes only
        /// the icon —, else the glyph form with any state icon the topic shows removed. Records the title, the state, the square
        /// and the form (a refused rename is recorded too, so it is not retried every tick — except a refusal of Telegram's flood
        /// control, a refused ICON, which turns the state icons off for the run and leaves the topic due in the glyph form, and a
        /// call that got NO ANSWER at all, which stays due and is made again at the next tick, §21.29).
        /// The square is ChooseNameColor's: the tab's color once it has held, else the one the name shows. When the TITLE
        /// changed, or the tab's COLOR changed from one square to another under a settled mode, a silent line in the topic says
        /// so (BuildTopicChangeNoticeHtml — the user's "a message like whatsapp does for group chat name changed", §21.13); a
        /// state-only change and a quiet re-square or re-form get none. The card is refreshed after a rename Telegram took.
        /// Tick thread (a direct call, paced by _awaitCallSlot).
        /// </summary>
        /// <remarks>
        /// §21.29 (the user's rule of 2026-10-19): the change line is a rename's ONLY answer (a /rename in the topic posts no reply
        /// of its own), so it is never dropped. A topic whose history is still being replayed HOLDS its line (HoldTopicChangeLine,
        /// persisted in the topic's record), and _pumpBackfill tells it once the replay is done (TakeHeldTopicChangeLine): one line
        /// from the first held change's start to the name the topic shows then. TOPIC_NOT_MODIFIED (the topic shows this name and
        /// icon already) counts as a rename Telegram took, so a rename whose lost answer was made again still tells its line.
        /// </remarks>
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
        /// session store joined with the open tabs that carry it, and every open Claude tab as a candidate of the Recent activity
        /// section (§21.16.8) — renders it (RenderDirectory, which lists only the tags an open tab carries: the user's "Tags with 0
        /// tabs should not be shown", and at the bottom the ten newest tabs that are not stationary) and does what DecideDirectoryAction
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
        /// §21.28: a tapped button that waits for this card's NEXT change (CardState::tapHolds) makes the change go now — past the
        /// edit gap, at the priority of an answer — and the edit or post that carries it holds the tap's answer until Telegram has
        /// answered it. A refresh that cannot change the card yet (a post in flight, a refused post's minute) while a tap is in
        /// scope stashes the tap there (_awaitCardChange), so the change that follows answers it; one that finds the card already
        /// showing this text holds nothing (the tap is answered at once).
        /// </remarks>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="force">Edit now, past the edit gap.</param>
        /// <param name="ping">Re-post the card with sound (kept until a post can carry it).</param>
        /// <param name="tapped">A button on the card changed it: edit at the priority of an answer.</param>
        /// <param name="known">The session as the caller already copied it (the tick's per-topic pass), so the card is built from
        /// that copy instead of a second full SessionRegistry::Get; nullptr = read the registry now. A copy taken earlier in the
        /// same pass may miss a change made since then; that change reaches the card at the next refresh.</param>
        void _refreshCard(const std::wstring& sessionId, bool force, bool ping = false, bool tapped = false, const SessionInfo* known = nullptr);
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
            // §21.28: the tapped buttons whose ANSWER waits for this card's NEXT change — a directory dot (its re-post makes the topic
            // first in the list), a 🔄 Restart / ▶ Reactivate / 🔧 Resume or a confirmed restart, park or interrupt (the card that
            // shows the new state), and a page flip that met a card post in flight (_awaitCardChange). _refreshCard's next edit or
            // post takes them (going at once, at an answer's priority) and holds them until Telegram answered it; a card dropped
            // with its topic lets them go, and the hold's limit answers a tap whose card never changes (kTapAnswerHoldMaxMs).
            std::vector<TapHoldRef> tapHolds;
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
        /// <summary>
        /// A tapped inline button (a callback_query update): a stateless Search button, a Search pick, a directory dot, a new
        /// thread's picker, a card verb ("c:&lt;verb&gt;:&lt;session&gt;") or a single-use token ("t:…"), each carried out on the
        /// session's state NOW. §21.28: the tap's ANSWER (answerCallbackQuery — the end of the button's loading indicator, and its
        /// toast) is HELD while what the tap set off runs: the handler runs inside a TapScope of its own hold, so every outbox item
        /// it queues holds the answer until Telegram answered that call, the continuations it starts carry the tap along, and the
        /// handler's `answer(text)` only sets the toast. The answer goes when the last hold goes, or at the hold's limit
        /// (kTapAnswerHoldMaxMs); a tap from anyone but the paired owner is answered at once ("This bot is private."). Poll thread;
        /// called through _handleUpdate, which catches what it throws (the hold goes with the stack, and the answer then carries
        /// the toast set so far).
        /// </summary>
        /// <param name="cq">The Telegram CallbackQuery object.</param>
        /// <exception cref="std::bad_alloc">If memory runs out (the tap, its holds, the handler's replies).</exception>
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
        /// <summary>
        /// Runs an Agentmaster /command in a session's topic — typed, or a card button that names one (§21.24) — and replies in the
        /// topic (a refused or unknown one says why). §21.28: called from a tapped button, it runs inside the tap's scope, so its
        /// replies hold the button's answer; the commands whose answer comes later carry the tap along — /screen and /screenimage
        /// in the window's reply (and the drawing's worker), /forksession and /newsession in their launch's revive.
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="cmd">The command (name and arguments).</param>
        /// <param name="msg">The command message (its id: a reply quotes it); an empty object for a button.</param>
        /// <param name="threadId">The topic.</param>
        /// <exception cref="std::bad_alloc">If a reply, a request or a hold cannot be allocated.</exception>
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
        /// /steer (§21.33): the words go into the session's input box NOW — the Manager's Send now with a new prompt, so a running
        /// turn takes them in at its next step — when DecideSteer says Send; otherwise (a tab whose claude is not started, one whose
        /// claude ended, a menu on the terminal) they wait FIRST in the queue, the reply says why, and a dormant tab is started.
        /// The prompt is a remote one (BuildSteerPrompt: its status is a reaction on the /steer message — ⚡ sending, 👨‍💻 taken,
        /// ✍ queued) placed before every waiting prompt (SteerInsertIndex). A send the delivery gate declines at once goes back to
        /// the queue and is answered at once; one the hosting window declines later is told by _sweepSteers. A /steer older than
        /// kBacklogAgeMs (written while Agentmaster was closed) is asked about like a plain message (D2), unless a new thread HELD
        /// it. Poll thread (from _handleTopicMessage; _runSessionCommand keeps a branch for any other path).
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="words">The words after /steer (ParseRemoteCommand's args).</param>
        /// <param name="msg">The /steer message (its id: the reaction and the replies; its date: the backlog rule); an empty object
        /// when no message stands behind it.</param>
        /// <param name="threadId">The topic.</param>
        /// <param name="held">The message was held by a new thread's picker (§21.17): it is never backlog.</param>
        /// <exception cref="std::bad_alloc">If the prompt, a reply or a reaction cannot be allocated.</exception>
        void _steerRemotePrompt(const std::wstring& sessionId, const std::wstring& words, const json::Value& msg, int64_t threadId, bool held);
        /// <summary>
        /// §21.33, once per tick and topic (right after _syncReactions): looks at each steer this service SENT to the session
        /// (_steers) and acts on its outcome (ClassifySteerWatch). Words the hosting window put back in the queue get ✍ on their
        /// message and ONE line that says why (SteerDeclinedNoticeHtml over the facts the decline left); a watch that is taken,
        /// resolved, gone or expired is dropped without a word (the reactions and the delivery's own lines speak for those). Any
        /// OTHER session's watch past kSteerWatchMaxMs is dropped too: a session whose topic is gone is swept by no tick any more,
        /// and _steers must not keep its watches for the rest of the run. Tick thread.
        /// </summary>
        /// <param name="s">The session (the tick's snapshot).</param>
        /// <exception cref="std::bad_alloc">If a notice cannot be allocated.</exception>
        void _sweepSteers(const SessionInfo& s);
        /// <summary>
        /// REACTIVATE (§21.5, §21.6): a parked tab's claude is started in place; a session in Error, or one whose
        /// claude exited (Done), is restarted (the same conversation resumes in the same tab; with
        /// CLAUDE_CODE_RESUME_INTERRUPTED_TURN a cut-off turn goes on by itself). A running session is left alone.
        /// The card's ▶ Reactivate and 🔧 Resume Session (§21.24) buttons show the outcome as the button's toast (nothing is
        /// posted in the thread, which is the conversation); `/reactivate` replies with it. §21.28: from a tapped button (inside its
        /// scope), a start or a restart keeps the button busy until the card shows the result (_awaitCardChange).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <returns>The outcome, plain text (one sentence).</returns>
        /// <exception cref="std::bad_alloc">If the outcome text or a hold cannot be allocated.</exception>
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
        /// thread; takes `_mtx` and wakes the tick. §21.28: the dot stays busy until the re-post is in (its holds wait in
        /// CardState::tapHolds for that post); a dot whose topic still replays its history, and a double tap, are answered at once.
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
        /// <returns>True when the command LEFT the thread it was typed in (its delete was queued and the line went to the Search
        /// topic); false when it was answered where it was typed (no Search topic yet). §21.25: a new thread whose only message
        /// was the command is then empty, and _handleMessage deletes it (_abortUntouchedNewThread).</returns>
        /// <exception cref="std::bad_alloc">If memory runs out (it passes through _handleMessage to _handleUpdate, whose guard
        /// logs it; the command then stays where it was typed and nothing moves).</exception>
        bool _jumpToSearchTopic(int64_t fromThreadId, int64_t messageId);
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
        /// <returns>True when the command LEFT the thread it was typed in (deleted there, the request posted in the Search topic);
        /// false when it was answered where it was typed (no Search topic yet, or a backlog message). §21.25: as for
        /// _jumpToSearchTopic, a new thread left empty by it is deleted.</returns>
        /// <exception cref="std::bad_alloc">If memory runs out (it passes through _handleMessage to _handleUpdate, whose guard
        /// logs it; the search is then not sent, and the command stays where it was typed).</exception>
        bool _relaySearchRequest(int64_t fromThreadId, const json::Value& msg, const std::wstring& words);
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
        /// §21.28: called from a tapped button, the revive carries the tap (Revive::tapHolds), so the button stays busy until the
        /// "Opened …" line or the window's refusal is in the thread (or until the hold's limit).
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
        /// the SOURCE (§21.21). Tick thread. §21.28: the "Opened …" line is posted inside a scope of the revive's tap holds, so a
        /// tapped launch button is answered once that line is in.
        /// </summary>
        /// <param name="s">A session the tick looked at (a topic's, or the one an answer named).</param>
        /// <exception cref="std::bad_alloc">If the matched copies or the reply cannot be allocated.</exception>
        void _sweepRevives(const SessionInfo& s);
        /// <summary>
        /// The card's ⚙ Session and ◀ Back (§21.24): shows `page` on the message the button sits on. On the topic's sticky card
        /// the page is remembered (CardState::page, so a re-post keeps it) and the card is edited through _refreshCard (its
        /// hash covers the buttons); on any other message - a /status copy of the card - only that message's buttons change.
        /// §21.28: the tapped button stays busy until that edit is in (or the one that follows a card post in flight).
        /// </summary>
        /// <param name="sessionId">The card's session.</param>
        /// <param name="messageId">The message the tapped button sits on.</param>
        /// <param name="page">The page to show.</param>
        void _showCardPage(const std::wstring& sessionId, int64_t messageId, CardPage page);
        /// <summary>
        /// The card's ⑂ Fork and 🆕 New Session (§21.24): the topic's /forksession and /newsession with no model - a fork, or a
        /// fresh session in the session's folder, opened as a background tab - through the commands' own path (their replies go in
        /// the thread, so the button and the typed command never differ). §21.28: no toast — the button stays busy until the thread
        /// says how the launch went ("… into a tab…", then "Opened …" or why not: _requestLaunch's revive carries the tap), and a
        /// toast that said "Forking it…" would only show up after the fact.
        /// </summary>
        /// <param name="sessionId">The card's session (the fork's source; the folder of the new session).</param>
        /// <param name="fork">True for ⑂ Fork, false for 🆕 New Session.</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>The toast: always "" (the thread's lines are the answer).</returns>
        /// <exception cref="std::bad_alloc">If the command or a reply cannot be allocated.</exception>
        std::wstring _launchFromCard(const std::wstring& sessionId, bool fork, int64_t threadId);
        /// <summary>
        /// The card's 🔄 Restart Session (§21.24): the topic's /restartsession. A session at rest is restarted at once (the same
        /// conversation resumes in its tab) and the toast says so; while a turn runs, the restart asks first in the thread (the
        /// turn would be cut off), exactly as /restartsession does, and the toast points at that question. A PARKED tab (its claude
        /// never started: restored, deactivated) is started in place, as ▶ Reactivate does — the window's own restart refuses a
        /// connection that never started (`_restartPaneConnection`'s guard: an AV otherwise), so a restart there did nothing.
        /// §21.28: the button stays busy until the card shows the restarted (or started) session (_awaitCardChange), or until the
        /// question is in the thread.
        /// </summary>
        /// <param name="sessionId">The card's session.</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>The toast, one plain sentence.</returns>
        /// <exception cref="std::bad_alloc">If the toast, a reply or a hold cannot be allocated.</exception>
        std::wstring _restartFromCard(const std::wstring& sessionId, int64_t threadId);
        /// <summary>
        /// HOOKS.md "Interrupted turns" — the restart question, asked IN THE TOPIC (the PC shows no dialog for a restart the
        /// phone asked for). When the session's active branch ends in a turn the USER interrupted and a rewind would let Claude
        /// Code continue it (PlanInterruptRewindForSession == Rewind), posts "Continue the interrupted turn?" with three
        /// single-use buttons: ▶ Continue the turn (token restart-uninterrupt), ⏸ Keep it interrupted (restart-keep) and Cancel.
        /// Their handlers restart with the answer through ServiceHooks::restartSession. /restartsession at rest and the card's 🔄
        /// Restart Session ask here; a restart of a RUNNING turn (confirm-restartsession) and ▶ Reactivate of an errored or
        /// exited session never do (nothing the user interrupted).
        /// </summary>
        /// <param name="sessionId">The session to restart.</param>
        /// <param name="s">Its record (the title for the question).</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>True when the question was posted (the caller restarts nothing now); false when there is nothing to ask.</returns>
        /// <exception cref="std::bad_alloc">If the question cannot be allocated.</exception>
        bool _askUninterruptBeforeRestart(const std::wstring& sessionId, const SessionInfo& s, int64_t threadId);
        /// <summary>
        /// HOOKS.md "Interrupted turns" — the fork question, asked IN THE TOPIC for a fork the phone asked for (/forksession,
        /// the card's ⑂ Fork, the Search topic's ⑂ pick). When a fork of `req.sessionId` would continue the source's turn by
        /// itself (AssessForkContinuation — the source's tail or its live state, with CLAUDE_CODE_RESUME_INTERRUPTED_TURN on),
        /// posts "Fork a session in the middle of a turn?" with ⏸ Fork interrupted (token fork-interrupted), ▶ Fork uninterrupted
        /// (fork-uninterrupted) and Cancel. A tap rebuilds the request with the answer (RemoteLaunchRequest::forkTurn) and sends
        /// it through _requestLaunch, so the window forks with the answer (an Interrupted fork arms its stop watch there).
        /// </summary>
        /// <param name="req">The fork request (dir already resolved).</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>True when the question was posted; false when the fork may go on at once (Uninterrupted).</returns>
        /// <exception cref="std::bad_alloc">If the question cannot be allocated.</exception>
        bool _askForkTurnBeforeLaunch(const RemoteLaunchRequest& req, int64_t threadId);
        /// <summary>
        /// The second half of 🖼 Screen Image and /screenimage (§21.24), called with the hosting window's answer on ITS UI thread:
        /// parses the snapshot (ScreenSnapshot.h) and draws it as a PNG on a worker thread of its own (ScreenImage.h - ~90 to
        /// ~185 ms of drawing for a 200 x 60 to 300 x 80 screen, measured; never on a UI thread), then sends the picture into
        /// the topic (_sendPhoto). At most kScreenImageMaxRenders pictures are drawn at once; a request past that is answered in
        /// the thread instead. §21.28: the worker takes the holds of the calling scope's taps (a tapped 🖼 Screen Image, through the
        /// window's reply) and sends inside a scope of them, so the button stays busy until the photo or the line is in.
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

        // ----- §21.17: a thread the user opens starts a new session; §21.25: through the wizard (Messaging.Inbound.cpp) -----
        /// <summary>
        /// The owner created a thread (forum_topic_created) that no session owns. A name the user gave it (is_name_implicit
        /// false) is kept — it becomes the session's title — and the picker is posted at once, before any message. A thread
        /// Telegram named (the user only typed a message, which arrives right after) gets its picker with that message
        /// (_holdForNewThread): one post, not a post and an edit. §21.25: the record is persisted with the state file from
        /// here on (a restart resumes the wizard). Poll thread.
        /// </summary>
        /// <param name="threadId">The new thread.</param>
        /// <param name="msg">The service message (its forum_topic_created carries the name and is_name_implicit).</param>
        void _onUserThreadCreated(int64_t threadId, const json::Value& msg);
        /// <summary>
        /// A message in a thread no session owns (and not the Search topic): read by the thread's WIZARD (§21.25,
        /// ApplyNewThreadText) — a lone folder path is a folder choice at every step, a model id at the model steps ("/model
        /// &lt;id&gt;", or one word after ✏ Specify…), a window number at the window step; a choice re-posts the picker below the
        /// message (the user looks at the bottom of the thread). Anything else is HELD for the new session (at most
        /// kNewThreadHeldMax messages) and goes to it in order once it is up. The picker is posted with the first held message
        /// when the thread has none yet (a thread made before §21.17). An edit replaces the held message's text. A thread whose pick is
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
        /// Shows the thread's wizard at its current step (§21.25, BuildNewThreadView): posts the picker, or edits it in place when
        /// it is up (a page change, one more held message, a step reached by a button). The folder lists are built on first use
        /// at the folder step (BuildNewThreadLists over the registry's PlaceSamples, the machine's temp roots left out, every
        /// listed folder probed with a bound). A window step whose census is not running (a restored record, a lost answer)
        /// starts one (_collectNewThreadWindows). An edit Telegram answers "message not found" (the user deleted the picker)
        /// posts a new one; "thread not found" (the user deleted the thread) drops the record. Takes `_mtx` for the state and
        /// releases it before the send. Poll thread, or a census worker.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="repost">True: delete the picker and post it again at the BOTTOM of the thread — after a choice the user
        /// TYPED (a folder, a model id, a window number), which sits below the picker. False: edit in place.</param>
        void _showNewThreadPicker(int64_t threadId, bool repost = false);
        /// <summary>
        /// A tap on a picker button (ParseNewThreadCallbackData), read by the thread's wizard (§21.25, ApplyNewThreadButton) and
        /// carried out (_applyNewThreadOutcome): a pager arrow edits the folder step to its page; a pager's page label changes
        /// nothing and answers which page it is; a folder is checked (_checkNewThreadFolder) and leads to the model step; Create
        /// makes a typed folder (_createNewThreadFolder); a model leads to the window step (_collectNewThreadWindows); a window
        /// launches (_launchNewThread); ✖ Cancel deletes the thread (_abortNewThread). A button of an earlier step answers so and
        /// changes nothing; a button of a picker this service holds no record of (already launched, cancelled) answers that a
        /// message in the thread brings a new picker. Takes `_mtx` for the record. Poll thread.
        /// </summary>
        /// <param name="action">The button's action.</param>
        /// <param name="threadId">The thread the picker sits in (the callback's message).</param>
        /// <returns>The button's toast, plain text ("" = none).</returns>
        std::wstring _onNewThreadAction(const NewThreadAction& action, int64_t threadId);
        /// <summary>
        /// §21.25: carries out what the wizard decided (NewThreadOutcome) for the thread — render, check a folder, create one, enter
        /// the model step, collect the windows, launch, abort, or answer. The one dispatcher for a button (its toast is the return
        /// value) and a typed message (`replyTo` set: a reply quotes it, a render re-posts the picker below it). A Hold is the
        /// caller's (it owns the message). Poll thread, or a census worker (a window step skipped by its census launches here).
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="out">What the wizard decided.</param>
        /// <param name="replyTo">The user's message that made the choice (0 = a button, or a census answer).</param>
        /// <returns>The toast for a button (plain text, "" = none); ignored for a typed message.</returns>
        /// <exception cref="std::bad_alloc">If a reply or an outbox item cannot be allocated.</exception>
        std::wstring _applyNewThreadOutcome(int64_t threadId, const NewThreadOutcome& out, int64_t replyTo);
        /// <summary>
        /// §21.25: checks a chosen folder with a BOUND on the wait (the poll thread must never hang on a slow folder,
        /// §21.17.4): a list pick through SharedFolderProber (exists, gone, no answer); a TYPED path through the full
        /// case-insensitive resolution (ResolveNewThreadFolder over the real file system, on a worker thread of its own) — an
        /// existing folder in its disk spelling, a missing one that can be made (the CreateFolder step), or why not. Then
        /// ApplyNewThreadFolderResolved, the model step's choices when the folder is chosen, and a render.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="path">The folder (a list entry, or the path the user typed).</param>
        /// <param name="typed">True when the user typed it (a missing folder may then be created).</param>
        /// <param name="replyTo">The typed message (0 = a button): the picker is re-posted below it.</param>
        /// <returns>The toast for a button: "" when the step moved on, the reason when it did not.</returns>
        /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
        std::wstring _checkNewThreadFolder(int64_t threadId, const std::wstring& path, bool typed, int64_t replyTo);
        /// <summary>
        /// §21.25, the CreateFolder step's ✓: makes the folder the user typed — every missing level, outermost first
        /// (CreateDirectoryW on a worker thread, waited for at most kNewThreadCreateWaitMs) — and goes on to the model step in
        /// it; a refusal stays at the step with Windows' reason. A level made by someone else meanwhile counts as made.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="path">The folder to make (NewThreadWizard::createPath).</param>
        /// <returns>The toast: "Created …", or why not.</returns>
        /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
        std::wstring _createNewThreadFolder(int64_t threadId, const std::wstring& path);
        /// <summary>
        /// §21.25: enters the thread's MODEL step with the choices the right-click menu shows now (the launch-menu source over
        /// the settings' launch models, BuildNewThreadModelChoices) and the model last typed in a "Specify a model" prompt
        /// (LoadLastSpecifiedModel). No render: the caller renders. Takes `_mtx` for the record only; the menu and the model
        /// list are read outside it.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        void _enterNewThreadModelStep(int64_t threadId);
        /// <summary>
        /// §21.25: the window step's list. Marks the step "looking at the windows" with a new census serial, renders it, and
        /// asks every window for its strip on a WORKER thread (ServiceHooks::windowCensus blocks up to kNewThreadCensusTimeoutMs;
        /// the poll thread never waits for a window). The answer, with the registry's facts per tab, becomes the step's list
        /// (_applyNewThreadWindows). With no census hook the step is skipped at once (the engine's choice of window). §21.28: the
        /// worker carries the tapped model button along and applies the answer inside a scope of its holds, so the button stays
        /// busy until the window step (or the launch it skips to) is in the thread.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="replyTo">The typed message that chose the model (0 = a button): the picker is re-posted below it.</param>
        void _collectNewThreadWindows(int64_t threadId, int64_t replyTo);
        /// <summary>
        /// §21.25: a census answer reaches the thread's window step: stale (another census was asked since, the step moved on,
        /// the record is gone) — dropped; else ApplyNewThreadWindows decides: render the list, or launch at once (one window, or
        /// none answered). Any thread (the census worker).
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="serial">The census's serial (NewThreadWizard::windowsSerial when it was asked).</param>
        /// <param name="plan">The windows and their default (BuildNewThreadWindowPlan).</param>
        void _applyNewThreadWindows(int64_t threadId, uint64_t serial, NewThreadWindowPlan plan);
        /// <summary>
        /// Starts the thread's NEW SESSION with every choice of its wizard (§21.17, §21.25, BuildNewThreadLaunchPlan): a fresh Claude
        /// tab in the background of the CHOSEN window (or the engine's choice), right after the anchor tab, with the chosen model,
        /// under a session id minted here and recorded in `_threadBinds` BEFORE the launch so the tick can bind the thread to the
        /// session the moment it is live (_adoptBoundThreads). A line in the thread says what is opening; the picker is deleted.
        /// No window to take it (the chosen one closed since the census) puts the wizard back at the step that can fix it. §21.23:
        /// the window ANSWERS the launch — a refusal sends the thread back to the wizard at once, with the messages it held and the
        /// reason (_settleLaunchAnswers). §21.28: a tapped wizard button stays busy until that answer — its holds wait in
        /// `_bindTapHolds` under the launch's token, recorded in the bind's own lock scope.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="replyTo">The user's message that made the last choice by typing (0 = a button or a census answer): the
        /// "Opening …" line quotes it. §21.23: that line goes out BEFORE the launch, so the window's answer, which the tick may
        /// post a moment later from another thread, can never land above it.</param>
        /// <returns>The toast for a button (plain text; the opening line, or why nothing opened).</returns>
        /// <exception cref="std::bad_alloc">If the bind or a reply cannot be allocated.</exception>
        std::wstring _launchNewThread(int64_t threadId, int64_t replyTo);
        /// <summary>
        /// §21.25, the user's rule — "If the creation process aborts in any way, the new topic needs to be deleted": removes the
        /// thread's wizard and DELETES the thread (deleteForumTopic, queued; every queued item for it dropped first). When the
        /// thread held messages, a silent line in the chat's main view keeps what the user wrote there (they go with the thread).
        /// A delete Telegram refuses is logged, and the picker message is deleted instead. Any thread; takes `_mtx`.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="why">Why, for the log and the main-view line ("cancelled", "nothing was chosen for a day", …).</param>
        /// <param name="cancelled">True for the user's ✖ Cancel (the main-view line says "cancelled"); false for an abandoned one.</param>
        /// <returns>True when a record was there (and the delete is queued); false when the thread had none.</returns>
        /// <exception cref="std::bad_alloc">If the outbox items or the line cannot be allocated.</exception>
        bool _abortNewThread(int64_t threadId, const std::wstring& why, bool cancelled);
        /// <summary>
        /// §21.25: deletes a new thread that holds NOTHING the user wrote — no held message and no picker yet (an implicit-named
        /// thread whose first and only message was a /search command, which went to the Search topic): the creation never
        /// started, and the empty thread is its leftover. A thread with a picker or held messages is left as it is. Any thread.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="why">Why, for the log.</param>
        void _abortUntouchedNewThread(int64_t threadId, const std::wstring& why);
        /// <summary>
        /// §21.25, tick thread, every tick (cheap: a scan under `_mtx`): a wizard nobody used for kNewThreadPickerIdleMs is aborted
        /// (_abortNewThread: its thread is deleted); and once after a start, when the windows have finished restoring (the
        /// start-up hold ended), a restored wizard that was waiting for its windows asks them again.
        /// </summary>
        void _newThreadsTick();
        /// <summary>
        /// §21.25: the owner renamed a thread that is still a wizard (no session yet) or whose launch is under way: the name is the
        /// coming session's title (the picker shows it), and a launch under way takes it at its bind (_adoptBoundThreads renames
        /// the session to it). The name without the service's decoration (StripTopicNameDecoration); an empty one changes nothing.
        /// Poll thread.
        /// </summary>
        /// <param name="threadId">The thread.</param>
        /// <param name="name">The new name as Telegram reports it.</param>
        /// <returns>True when the thread was a wizard's or a launch's (handled); false otherwise.</returns>
        bool _onNewThreadRenamed(int64_t threadId, const std::wstring& name);
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
        /// wait in the thread's picker record for the next pick instead of being dropped. §21.25: the timeout puts the thread
        /// back into its WIZARD (NewThreadStepAfterRefusal: the window step, or the model step when that was skipped) and shows
        /// it; a launch restored from the state file waits while the windows restore (its session may only be waiting to be
        /// re-homed); and a name the user gave the thread while the launch ran is the session's title (renamed at the bind).
        /// §21.28: it also drops the tap holds of every launch whose bind is gone (`_bindTapHolds`), so that map never grows.
        /// </summary>
        /// <param name="byId">The sync's live sessions (SnapshotLiveForCards), by id.</param>
        /// <param name="colorMode">The tab color mode of the sync.</param>
        /// <param name="inStartupHold">§21.25: the sync's start-up hold (windows still restoring their tabs): no bind times out
        /// meanwhile — its deadline starts over when the hold ends.</param>
        void _adoptBoundThreads(const std::unordered_map<std::wstring, SessionInfo>& byId, TabColorMode colorMode, bool inStartupHold);
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
        /// where it stands NOW, and the buttons that fit there (§21.38, SearchPickMenuRows: ▶ Resume for a closed session,
        /// ▶ Activate for a tab whose claude is not running, ⏰ Wake for an idle tab, 💬 Go to topic / 📌 Pin for one with a
        /// topic; ⑂ Fork and ✕ Cancel always) as single-use tokens (M5) that carry the row number, folder and title. An
        /// open session is never offered ▶ Resume ("open it": the user's "but we already exist"). Silent (a menu is no final
        /// message). Poll thread.
        /// </summary>
        /// <param name="pick">The pick read from the button.</param>
        /// <param name="threadId">The Search topic.</param>
        /// <param name="answerMessageId">The answer message the pick button sits on (the menu replies to it); 0 = none.</param>
        void _showSearchPick(const SearchPick& pick, int64_t threadId, int64_t answerMessageId);
        /// <summary>
        /// Carries out a tapped action of a Search pick's menu, on the session's CURRENT state (it may have changed since
        /// the menu). ▶ Resume, ▶ Activate and ⏰ Wake (and their 📌 twins) all make the session usable from here, by its
        /// state now: a closed one is opened into a background tab (D27); a tab whose claude does not run — deactivated,
        /// restored and never entered, or exited — gets it started in place (_reactivate: no focus change on the PC; an
        /// exited claude is started again in the same conversation), then an Idle or exited one is held in Waiting for you so
        /// its topic appears (§21.38); an idle tab is woken; one with its topic already only says so. 💬 Go to topic puts its
        /// topic first in the thread list (_bringTopicToFront); 📌 Pin pins it; Fork forks it into a new tab, never pinned
        /// (no fork-and-pin, the user's rule); Cancel deletes the menu. The menu message loses its buttons and keeps the
        /// choice and its outcome. Poll thread.
        /// </summary>
        /// <param name="kind">The token's kind: one of the kSearchPick* kinds (IsSearchPickActionKind).</param>
        /// <param name="sessionId">The picked session.</param>
        /// <param name="payload">The token's payload: "&lt;row number&gt;\n&lt;folder&gt;\n&lt;title&gt;".</param>
        /// <param name="threadId">The Search topic.</param>
        /// <param name="menuMessageId">The menu message the button sits on (edited or deleted); 0 = none.</param>
        /// <exception cref="std::bad_alloc">If a text, a token or a request cannot be allocated (the callback handler logs it, Rule #18).</exception>
        void _onSearchPickAction(const std::wstring& kind, const std::wstring& sessionId, const std::wstring& payload, int64_t threadId, int64_t menuMessageId);
        void _appNote(const std::wstring& html, bool sound);

        // ----- data -----
        std::shared_ptr<SessionRegistry> _registry;
        std::shared_ptr<Scheduler> _scheduler;
        std::wstring _profile;
        ServiceHooks _hooks;

        // ----- §21.32.4: the Telegram FILE LINKS (Messaging.Files.cpp) -----
        // Three locks, always taken in this order and never the other way round:
        //   `_filesLifeMtx` serializes the server's start and stop (Apply on the UI thread vs the bot's _start / Stop) and owns
        //       `_filesBotUp`; it is held while a server stops (up to 5 s), so only the lifecycle takes it — never the status
        //       line, the tick's quick paths, a connection thread or the mirror;
        //   `_filesSaveMtx` orders the writes of filelinks.json (a snapshot taken later is never overwritten by an older one);
        //   `_filesMtx` guards every other _files* member, the server POINTER included (written under both it and
        //       `_filesLifeMtx`). The server's connection threads take it (the authorizer), so a server is never stopped while
        //       it is held, and no file or network I/O happens under it.
        mutable std::mutex _filesMtx;
        std::mutex _filesLifeMtx;
        std::mutex _filesSaveMtx;
        bool _filesBotUp{ false }; // between _filesStart and _filesStop: only then may a server run (guarded by `_filesLifeMtx`)
        FileLinkMode _filesMode{ FileLinkMode::Off }; // the settings' mode
        std::wstring _filesStation; // the normalized station ("homepc")
        uint16_t _filesPort{ 0 }; // the server's port
        std::wstring _filesLanPick; // the cog's dropdown pick ("" = the first LAN address)
        std::wstring _filesHostedBase; // the hosted base ("" = https://cc.sysd.cc)
        std::vector<std::wstring> _filesExcluded; // the excluded folders' keys (ParseFileLinkExcludedFolders)
        std::vector<FileLinkUrlChoice> _filesLanChoices; // the LAN URLs, read at the server's start (the "lan" mode's fallback base)
        std::array<uint32_t, 8> _filesIv{}; // the session keys' eight initial words (from the secret)
        bool _filesSecretReady{ false }; // `_filesIv` holds the install's secret
        FileLinkStore _filesStore; // the linked set
        bool _filesStoreLoaded{ false }; // filelinks.json was read in this process
        bool _filesDirty{ false }; // the linked set changed since the last save
        int64_t _filesSavedMs{ 0 }; // when it was last saved
        int64_t _filesPrunedMs{ 0 }; // when expired links were last dropped
        int64_t _filesStatsLoggedMs{ 0 }; // when the last summary line was logged
        uint64_t _filesLinksMade{ 0 }; // links made since that line
        uint64_t _filesLinksKept{ 0 }; // file links kept as Claude wrote them since that line
        std::map<std::string, std::wstring> _filesHashToSession; // key -> session (the authorizer's lookup)
        std::map<std::wstring, std::string> _filesSessionHash; // session -> key (computed once per station and secret)
        std::shared_ptr<FileLinkServer> _filesServer; // the server while it runs (shared so the status line can read its counts)
        std::wstring _filesServerError; // why the server is not running, for the status line ("" = it runs, or file links are off)
        int64_t _filesServerTriedMs{ 0 }; // the last start attempt: a failed one is tried again a minute later (a port in use)
        int64_t _filesLanReadMs{ 0 }; // when `_filesLanChoices` was last read (again every 30 minutes while the "lan" mode serves)
        std::atomic<int> _filesUploadsInFlight{ 0 }; // "Send as file" reads in progress (at most two at once)
        /// <summary>A "📤 Send as file" offer of /download: the button carries only its number (callback data is 64 bytes at most).</summary>
        struct FileUploadOffer
        {
            std::wstring sessionId;
            std::wstring path;
            int64_t madeMs{ 0 };
        };
        std::map<uint64_t, FileUploadOffer> _filesOffers; // offer number -> offer (the newest 200 kept)
        uint64_t _filesNextOffer{ 1 };
        bool _filesDevInstall{ false }; // the AgentmasterDev package: the dev defaults (homepc-dev, port 32595)

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
        // Under `_mtx` (§21.31): the sessions whose topics left the list because they went idle and the main view's idle notice has
        // not named yet (_deleteTopic adds, _flushIdleNotices takes them all at once). In memory only.
        std::vector<IdleNoticeEntry> _idleNoticePending;
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
        /// <summary>
        /// A STRUCTURAL change of `_state` is not saved yet (saved within kStateSaveGapMs): anything the next start cannot
        /// learn again (a topic made, rekeyed or deleted, a card's message id, an offset, a bind). Guarded by `_mtx`.
        /// </summary>
        bool _stateDirty{ false };
        /// <summary>
        /// Only PROGRESS changed since the last save: the live mirror's cursors, recap facts and prompt lines
        /// (CopyBackTopicProgress), a work log's new id, a topic's posted clock (_noteTopicPosted) and its card's stored hash —
        /// what costs at most a repeated post or edit when lost at an unexpected end. Saved within kStateProgressSaveGapMs, or
        /// with the next structural change. ⚠ Mark a change here only when losing its last kStateProgressSaveGapMs is harmless;
        /// anything that would orphan a message or a topic is structural. Guarded by `_mtx`.
        /// </summary>
        bool _stateProgressDirty{ false };
        int64_t _lastStateSaveMs{ 0 };
        /// <summary>
        /// One state-file writer at a time (_saveStateIfDirty): the tick, Stop, the recreation and the app-close flush. Taken
        /// before `_mtx`, never after it. Guards `_lastSavedStateBytes`, `_lastPrevCopyMs` and `_stateFileReadable`.
        /// </summary>
        std::mutex _stateSaveMtx;
        /// <summary>
        /// The bytes state.json holds as far as this run knows (the last write, or what _loadState read): a save that would write
        /// the same bytes writes nothing. ~0.5 MB with 441 topics. Guarded by `_stateSaveMtx`.
        /// </summary>
        std::string _lastSavedStateBytes;
        /// <summary>When a save last refreshed state.json.prev (0 = not yet in this bot start). Guarded by `_stateSaveMtx`.</summary>
        int64_t _lastPrevCopyMs{ 0 };
        /// <summary>
        /// state.json is a file this run could READ (_loadState parsed it) or wrote itself: only then may a save copy it over the
        /// backup. After a start that found it torn (and took the backup) or unknown, the backup is kept until a good write.
        /// Guarded by `_stateSaveMtx`.
        /// </summary>
        bool _stateFileReadable{ false };
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
        /// §21.28, the tapped buttons whose answer is not sent yet, behind their OWN leaf lock `_tapMtx` — a hold can go on any
        /// thread, also one that holds `_mtx` (OnTapHoldsGone), so nothing is ever taken while `_tapMtx` is held: `_mtx` before
        /// `_tapMtx`, never the other way round. ⚠ These lists keep TAPS, never a TapHold: a hold that went while `_tapMtx` is
        /// held would call OnTapHoldsGone, which takes `_tapMtx` again (a std::mutex: a self-deadlock). `_openTaps`: every tap from its arrival until it is answered (the hold's limit
        /// reads it). `_tapsDue`: the taps whose last hold went, for the send loop to answer. `_tapAnswersDue` wakes the send loop
        /// (its wait reads it; a wake-up lost to the race between its check and the store costs one wait, at most 0.5 s).
        /// </summary>
        std::mutex _tapMtx;
        std::vector<std::shared_ptr<TapAnswer>> _openTaps;
        std::vector<std::shared_ptr<TapAnswer>> _tapsDue;
        std::atomic<bool> _tapAnswersDue{ false };
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
        /// <summary>
        /// §21.33: the steers this service SENT and still watches (prompt id -> SteerWatch), until _sweepSteers learns their
        /// outcome. Written by the poll thread (_steerRemotePrompt), read and pruned by the tick (_sweepSteers); guarded by `_mtx`.
        /// Cleared with _reacted at a start and at a change of bot (a watch names a message of the old chat).
        /// </summary>
        std::unordered_map<std::wstring, SteerWatch> _steers;
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
            // §21.28: a tapped ⑂ Fork / 🆕 New Session / ▶ Resume whose answer waits for this launch's outcome in the thread — the
            // "Opened …" line (_sweepRevives) or the window's refusal (_settleLaunchAnswers posts it inside a scope of these
            // holds). The revive's end lets them go; the hold's limit answers a slower launch.
            std::vector<TapHoldRef> tapHolds;
        };
        std::vector<Revive> _revives;
        // §21.24: the pictures of screens being drawn right now (_startScreenImage's worker threads) - at most
        // kScreenImageMaxRenders at once, each one ~90 to ~185 ms of drawing (measured for a 200 x 60 to 300 x 80 screen).
        std::atomic<int> _screenImageRenders{ 0 };
        // §21.24, under `_mtx`: when each session's screen picture was last asked for (WallMs). A second request within
        // kScreenImageTapGapMs asks nothing new: a double tap must not draw and send two pictures on the chat's call budget.
        std::unordered_map<std::wstring, int64_t> _screenImageTapMs;

        /// <summary>
        /// §21.17, under `_mtx`: a thread the user opened that no session owns yet, and its PICKER — since §21.25 its WIZARD
        /// (MessagingNewThreadWizard.h NewThreadRecord: the step, the answers, the folder lists and the model / window choices it
        /// showed, the held messages). PERSISTED with the state file (MessagingState::newThreads, written by _saveStateIfDirty,
        /// read at _start), so a restart resumes the wizard and its picker's buttons still map to what they showed. Every change
        /// to a record sets `_stateDirty`. Before §21.25 it was in memory only and a restart forgot it.
        /// </summary>
        using NewThread = NewThreadRecord;
        std::map<int64_t, NewThread> _newThreads; // thread id -> its wizard
        /// <summary>
        /// §21.17, under `_mtx`: a wizard's launch, waiting for its session to be live (MessagingNewThreadWizard.h NewThreadBind)
        /// — the session id the launch was given (RemoteLaunchRequest::newSessionId), the thread it becomes the topic of, the title
        /// it was given ("" = the folder-derived default), the user's messages to hand over, and (§21.25) the wizard it launched
        /// with, which a refused or timed-out launch goes back to. The tick binds it (_adoptBoundThreads) and keeps it here, as
        /// the thread's hand-over queue, until the topic is recorded and every held message has been handed over: a thread that
        /// has a topic and a bind is being bound right now, and its new messages join `held`. PERSISTED like `_newThreads` (its
        /// launch token and answer are not: no answer survives a restart, so a restored bind waits for its session, or times out).
        /// </summary>
        using ThreadBind = NewThreadBind;
        std::vector<ThreadBind> _threadBinds;
        /// <summary>§21.25, under `_mtx`: the last census serial handed out (NewThreadWizard::windowsSerial; 0 is never one).</summary>
        uint64_t _censusSerial{ 0 };
        /// <summary>
        /// §21.25, tick thread only: a start restored wizards from the state file, and the ones waiting for their windows (a census
        /// lost to the stop) ask again once the start-up hold has ended (_newThreadsTick).
        /// </summary>
        bool _newThreadsResumePending{ false };
        /// <summary>§21.25, under `_mtx`: the launch-model menu's source (SetLaunchMenuSource); empty = Default and a typed id only.</summary>
        LaunchMenuSource _launchMenuSource;
        /// <summary>§21.25, under `_mtx`: AppSettings::launchModels as the last Apply had it — the spec the model step's menu reads.</summary>
        std::wstring _launchModelsSpec;
        /// <summary>§21.25, under `_mtx`: AppSettings::model as the last Apply had it — what the model step's Default stands for.</summary>
        std::wstring _settingsModel;
        uint64_t _launchTokenSeq{ 0 }; // §21.23, under `_mtx`: the last launch token handed out (0 is never one)
        /// <summary>
        /// §21.28, under `_mtx`: a new thread's launch (_launchNewThread) by its launch token -> the holds of the tapped wizard
        /// button that started it (a model or a window). Kept beside the bind, not in it: a bind is persisted (NewThreadBind,
        /// MessagingNewThreadWizard.h), a hold is a moment of this run. The window's answer ends it (_settleLaunchAnswers: an
        /// opened tab lets the holds go, a refusal posts its notice and the wizard again inside a scope of them); so does a
        /// launch no window took, and an entry whose bind is gone (_adoptBoundThreads drops those every tick).
        /// </summary>
        std::unordered_map<uint64_t, std::vector<TapHoldRef>> _bindTapHolds;
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

        // ===== §21.30: Claude's QUESTIONS answered from Telegram (Messaging.Questions.cpp; AskQuestion.h, HooksBridge.h) =====
        //
        // An AskUserQuestion dialog at the PC runs Agentmaster's question hook beside it (ClaudeSpawn.cpp BuildAskHookScript); its
        // request reaches `_asks` over the ask pipe, and the tick turns it into the QUESTION CARD: ONE managed message in the
        // session's topic that shows the questions with their options, previews and notes, and takes the answer — an option, the
        // user's own words, a note, or "Chat about this" — composed exactly as the PC composes it (AskQuestion.h), and edited as
        // the user goes. The card's own WATCH reads the transcript for the dialog's result: the phone's answer confirmed, or the
        // PC's answer, which RELEASES the hook ("{}"), since Claude Code does not end a hook whose dialog the PC answered.

        /// <summary>How a dialog ended in the transcript: its tool result carried answers (or none, without an error), or an error.</summary>
        enum class QuestionResult
        {
            Answered, // an answer (from the phone or the PC) — `toolUseResult.answers`, or a result without an error
            Rejected, // an error result: "Chat about this", Esc at the PC, or a refusal of the answer
            TimedOut, // Claude Code's own idle timeout resolved it (`toolUseResult.afkTimeoutMs`): nobody answered
        };
        /// <summary>
        /// One question card: one open — or recently finished — dialog and its message. Under `_mtx`; a pointer to one
        /// (_findQuestionCardLocked) is valid only while the lock is held.
        /// </summary>
        struct QuestionCard
        {
            uint64_t askId{ 0 }; // the AskRegistry request the decision goes to
            std::wstring key; // the callback key ([0-9a-z]{10}, random): a button of another card, or of an earlier run, never acts on this one
            std::wstring sessionId;
            int64_t threadId{ 0 }; // the topic the card is in
            int64_t messageId{ 0 }; // the card's message: 0 = none (not posted, refused, or deleted), -1 = its post is in flight
            int64_t postFailedMs{ 0 }; // when the last post was refused (an open card posts again kAskRepostAfterFailureMs later)
            AskCardModel model; // the questions, the user's choices, the question the card shows, the phase
            std::wstring signature; // AskQuestionsSignature: the transcript's question item of the same dialog has this text
            json::Value toolInput; // the call's input as the request carried it: the allow decision's base
            std::wstring toolUseId; // the transcript's tool_use id of the dialog, once known (the watch, or the mirror's question item)
            std::wstring transcriptPath; // the request's transcript_path: what the card's watch reads
            int64_t watchOffset{ -1 }; // where the watch reads on (-1 = not started: its first read looks back kAskWatchSeedBytes)
            int64_t lastWatchMs{ 0 }; // when the watch last read
            bool resultSeen{ false }; // the transcript showed the dialog's result: the watch stops
            int64_t openedMs{ 0 }; // when the request arrived (the registry's clock)
            int64_t sentMs{ 0 }; // when the phone's decision was handed to the hook
            int64_t noTopicSinceMs{ 0 }; // when the card found its session without a topic (0 = it has one)
            bool sound{ true }; // the first post rings — the session's one "needs you" sound (§21.6); a re-post never does
            std::shared_ptr<PromptRef> replyRef; // the turn's prompt: the first post replies to it (§21.12)
            uint64_t version{ 1 }; // bumped by every change of what the card shows
            uint64_t sendingVersion{ 0 }; // the version the edit on its way rendered (its beforeSend)
            uint64_t shownVersion{ 0 }; // the version Telegram last took (a post or an edit): a newer version wants another edit
            AskTextTarget writePromptFor{ AskTextTarget::None }; // what the ForceReply prompt asks for
            int64_t writePromptMessageId{ 0 }; // the ✍️ / 📝 ForceReply prompt (0 = none, -1 = in flight); deleted when it is done
            std::map<int64_t, std::wstring> held; // messages the "answer or next prompt?" question holds: message id -> text
            bool pictureDrawing{ false }; // the current question's previews are being drawn (one picture at a time per card)
            bool decided{ false }; // a decision or a release went to the registry: nothing more goes to the hook
            int64_t finishedMs{ 0 }; // when the card left Open / Sending for good (0 = not yet); forgotten kAskCardKeepMs later
        };
        /// <summary>
        /// A question the mirror met that no card shows yet: it waits kAskPairWaitMs for its hook's request, then is posted as the
        /// show-only message ("answer it at the PC", with why). A card made meanwhile from a request of the same session and
        /// content takes it over (its sound, its tool_use id, its reply). Under `_mtx`.
        /// </summary>
        struct PlainQuestion
        {
            std::wstring sessionId;
            std::wstring toolUseId;
            std::wstring signature; // the question item's text (AskQuestionsSignature's form)
            std::wstring html; // the show-only message
            bool sound{ true };
            int64_t seenMs{ 0 };
            std::shared_ptr<PromptRef> replyRef;
        };
        /// <summary>What a tap, a typed text or the tick decided about a card, carried out by _applyQuestionEffects outside `_mtx`.</summary>
        struct QuestionEffects
        {
            std::wstring key; // the card ("" = none)
            bool edit{ false }; // the card's text or buttons changed: edit its message
            bool post{ false }; // post the card (anew: its topic changed, or its last post was refused)
            uint64_t askId{ 0 }; // the request the decision or the release goes to
            std::string decisionLine; // non-empty: hand this decision to the hook (AskRegistry::Answer)
            std::wstring releaseWhy; // non-empty: release the hook ("{}": the dialog stays with the PC), for this reason
            AskTextTarget writePrompt{ AskTextTarget::None }; // post the ForceReply prompt that asks for this
            std::vector<int64_t> deleteMessages; // the bot's own transient messages to delete (a write prompt, an "answer or prompt?")
            int64_t reactTo{ 0 }; // the user's message whose text the card took: a 👌 on it
            std::wstring logLine; // one hooks.log line ("" = none)
            std::wstring navLine; // one [nav] line for a decision the user made ("" = none)
        };

        // ===== §21.32.4: the Telegram FILE LINKS (Messaging.Files.cpp) =====================================================

        /// <summary>
        /// Apply: takes the FILE LINKS settings (mode, station, port, the LAN pick, the hosted base, the excluded folders) and,
        /// while the bot runs, starts, stops or moves the server to match. Any thread. Never throws (Rule #18 inside).
        /// </summary>
        /// <param name="settings">The settings being applied.</param>
        void _filesApply(const AppSettings& settings) noexcept;
        /// <summary>
        /// _start: reads the linked set (filelinks.json) and, with file links on, the install's secret (made on first use) and
        /// starts the server. Called before the threads start. Never throws: a failure leaves file links off for this run, with
        /// the reason in the cog's status line and hooks.log.
        /// </summary>
        void _filesStart() noexcept;
        /// <summary>Stop, after the threads are joined: stops the server and saves the linked set. Never throws.</summary>
        void _filesStop() noexcept;
        /// <summary>
        /// The tick: saves the linked set when it changed (at most every 10 s), prunes expired links hourly, and logs one summary
        /// line of the links made every 10 minutes. Tick thread. Never throws.
        /// </summary>
        /// <param name="nowMs">Now (unix ms).</param>
        void _filesTick(int64_t nowMs) noexcept;
        /// <summary>
        /// Rewrites the file links in Claude's own text (Text and Plan items; never the user's prompts, which the mirror matches
        /// against the queue) before the mirror or the history replay renders them: each file:/// link to an existing, linkable
        /// file gets its download link, and the file joins the session's linked set. A no-op with file links off. Never throws:
        /// a failure keeps the items as they were (Claude's original links).
        /// </summary>
        /// <param name="sessionId">The items' session.</param>
        /// <param name="items">The items, rewritten in place.</param>
        void _filesRewriteItems(const std::wstring& sessionId, std::vector<TopicItem>& items) noexcept;
        /// <summary>
        /// The download link of one file for one session, or nothing when it may not be linked (missing, a folder, a secret, an
        /// excluded folder, not a drive path). Adds the file to the session's linked set (or refreshes its time).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="winPath">The file.</param>
        /// <param name="base">The links' base ("https://cc.sysd.cc" or a LAN URL).</param>
        /// <returns>The URL, or nullopt to keep the original link.</returns>
        /// <exception cref="std::bad_alloc">If the strings or the store cannot grow.</exception>
        std::optional<std::wstring> _filesLinkFor(const std::wstring& sessionId, const std::wstring& winPath, const std::wstring& base);
        /// <summary>
        /// The server's authorizer (a connection thread): 200 for a request whose key is one of this install's session keys and
        /// whose path that session linked (not expired, still linkable); 404 with the reason otherwise. Never throws.
        /// </summary>
        /// <param name="target">The parsed request.</param>
        /// <returns>The decision.</returns>
        FileServeDecision _filesAuthorize(const FileLinkTarget& target) noexcept;
        /// <summary>
        /// Starts, stops or restarts the server so it matches the settings (on with a mode, on its port) while the bot is up
        /// (`_filesBotUp`); reads the install's secret first when it is not read yet. A port that cannot be bound, or a secret
        /// Credential Manager refuses, leaves the server off with the reason in the status line (tried again by the tick a minute
        /// later). Serialized by `_filesLifeMtx`; a server is stopped OUTSIDE `_filesMtx` (its connection threads take that lock).
        /// Never throws.
        /// </summary>
        void _filesReconcileServer() noexcept;
        /// <summary>
        /// /download &lt;path&gt; in a session's topic: the file (absolute, or relative to the session's folder) answered with its
        /// download link — through the same checks as a rewritten link, the file added to the linked set — and a "📤 Send as
        /// file" button that uploads it from the PC (at most 50 MB, any type). Poll thread.
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="s">Its snapshot (the folder a relative path is resolved in).</param>
        /// <param name="args">What followed /download.</param>
        /// <param name="threadId">The topic.</param>
        /// <param name="replyTo">The /download message (the answer quotes it; 0 = none).</param>
        /// <returns>The outcome word for the [nav] line ("ok", "refused:...", "usage").</returns>
        /// <exception cref="std::bad_alloc">If the reply cannot be built.</exception>
        std::wstring _filesDownloadCommand(const std::wstring& sessionId, const SessionInfo& s, const std::wstring& args, int64_t threadId, int64_t replyTo);
        /// <summary>
        /// A "📤 Send as file" tap ("dl:&lt;offer&gt;"): reads the offered file again (still there, still linkable, at most 50 MB)
        /// and sends it as a document into the topic. Poll thread.
        /// </summary>
        /// <param name="data">The callback data.</param>
        /// <param name="threadId">The topic the button sits in.</param>
        /// <returns>The tap's toast.</returns>
        /// <exception cref="std::bad_alloc">If the file or the texts cannot be held.</exception>
        std::wstring _filesOnTap(const std::wstring& data, int64_t threadId);
        /// <summary>
        /// A session's key, computed once per station and secret and mapped back to the session (the authorizer's lookup). The
        /// caller holds `_filesMtx` and has checked `_filesSecretReady`.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <returns>The 64-hex key.</returns>
        /// <exception cref="std::bad_alloc">If the maps cannot grow.</exception>
        std::string _filesHashForLocked(const std::wstring& sessionId);
        /// <summary>
        /// Forgets every computed key and computes one for each session of the linked set — after the station or the secret
        /// changed, and after the set was read or pruned — so the authorizer knows every session that has a live link, also the
        /// ones no message of this run linked yet. The caller holds `_filesMtx`; a no-op until the secret is read.
        /// </summary>
        /// <exception cref="std::bad_alloc">If the maps cannot grow (they are then partial: a link of a session missing from them
        /// answers 404 until its next link or the next rehash).</exception>
        void _filesRehashLocked();
        /// <summary>Writes filelinks.json when the linked set changed (or always, when forced). Never throws.</summary>
        /// <param name="force">True at Stop: write whatever changed, without the 10 s gap.</param>
        void _filesSaveStore(bool force) noexcept;

        /// <summary>
        /// _start: forgets the last run's cards (their buttons answer "no longer open here") and becomes the question registry's
        /// ACCEPTOR. Called before the threads start, while `_running` is still false — the acceptor refuses until the bot runs.
        /// </summary>
        /// <exception cref="std::bad_alloc">If the acceptor cannot be stored (the caller's catch logs it; questions then stay with the PC).</exception>
        void _questionsStart();
        /// <summary>
        /// Stop, after the threads are joined: drops the acceptor and RELEASES every open request — while the bot is off nothing
        /// can show or answer a question, so the PC keeps each dialog (the helper prints nothing and ends). Never throws.
        /// </summary>
        void _questionsStop() noexcept;
        /// <summary>
        /// The acceptor (an ask worker thread): a question is taken while the bot runs with topics on, from a live Claude session
        /// the registry knows (only those can have a topic). Never throws: anything unexpected refuses, and the PC answers.
        /// </summary>
        /// <param name="sessionId">The hook payload's session_id.</param>
        /// <returns>True to take the question.</returns>
        bool _acceptsAsk(const std::wstring& sessionId) const noexcept;
        /// <summary>
        /// The tick's question pass, after the per-topic mirror pass: every new request becomes a card in its session's topic (one
        /// whose session has no topic is waited for kAskNoTopicReleaseMs, then released); every card's watch reads its dialog's
        /// result, and the card follows its request (gone at the PC), its topic (made again: the card is posted anew), and its
        /// confirmation clock; finished cards are forgotten kAskCardKeepMs after their outcome; the plain questions whose wait
        /// ended are posted show-only. Tick thread. Never throws (Rule #18 inside: the next tick tries again).
        /// </summary>
        void _questionsTick() noexcept;
        /// <summary>
        /// Makes the card of a new request and posts it: it takes over a plain question of the same session and content (its
        /// sound and tool_use id), else takes the sound from the session's mirror and marks the mirror as waiting for an answer, so
        /// the NeedsApproval edge that follows rings nothing more (§21.6). Tick thread.
        /// </summary>
        /// <param name="entry">The request as the registry shows it.</param>
        /// <param name="req">The parsed request (ParseAskRequestLine; at least one question).</param>
        /// <param name="threadId">The session's topic.</param>
        /// <exception cref="std::bad_alloc">If the card cannot be stored.</exception>
        void _openQuestionCard(const AskEntryInfo& entry, AskRequest req, int64_t threadId);
        /// <summary>
        /// One card's turn in the tick: its watch (the transcript read since the last turn: the dialog's tool_use id and its result),
        /// its request's state (gone at the PC before an answer from here; a write that failed), its session's end (no longer live:
        /// the hook is released at once), its confirmation clock (kAskConfirmTimeoutMs), its topic (made again, or gone), and a
        /// refused post tried again. Tick thread.
        /// </summary>
        /// <param name="key">The card's key.</param>
        /// <param name="entry">Its request in this tick's registry snapshot (null = not in the registry any more).</param>
        /// <param name="now">The tick's clock (WallMs).</param>
        /// <exception cref="std::bad_alloc">If a text cannot be allocated.</exception>
        void _tickQuestionCard(const std::wstring& key, const AskEntryInfo* entry, int64_t now);
        /// <summary>
        /// Applies the dialog's RESULT to a card (the watch's, or the mirror's answer item — whichever reads it first; the second
        /// finds `resultSeen`): an open card becomes "answered at the PC" (with the answer) or "closed at the PC", and its hook is
        /// released; a card whose phone answer was on its way is confirmed (or, for an error, closed: Claude Code did not take
        /// it); a card closed on the request's side learns that the PC answered it. Call with `_mtx` held.
        /// </summary>
        /// <param name="c">The card.</param>
        /// <param name="result">Answered, Rejected, or TimedOut (Claude Code's own idle timeout resolved the dialog: the card says
        /// nobody answered in time, with what was selected before it, and releases the hook).</param>
        /// <param name="answerText">The answer as the transcript recorded it (the mirror's answer item text; "" when none).</param>
        /// <param name="now">The clock.</param>
        /// <param name="fx">Out: the release, the edit and the log line it wants.</param>
        /// <param name="timeoutMs">TimedOut: Claude Code's idle timeout (`afkTimeoutMs`), shown on the card; 0 otherwise.</param>
        /// <exception cref="std::bad_alloc">If a text cannot be allocated.</exception>
        static void _applyQuestionResultLocked(QuestionCard& c, QuestionResult result, const std::wstring& answerText, int64_t now, QuestionEffects& fx, int64_t timeoutMs = 0);
        /// <summary>
        /// Prepares the phone's ANSWER of a card for the hook: the answers and annotations composed as the PC composes them, the
        /// allow decision built, the card in Sending. An answer past Claude Code's bounds is refused (the toast says so) and the card
        /// stays open. Call with `_mtx` held.
        /// </summary>
        /// <param name="c">The card.</param>
        /// <param name="now">The clock.</param>
        /// <param name="fx">Out: the decision, the edit, the log and nav lines, the write prompt to delete.</param>
        /// <param name="toast">Out: the tap's toast (or the reply to a typed answer).</param>
        /// <exception cref="std::bad_alloc">If the decision cannot be built.</exception>
        static void _prepareQuestionAnswerLocked(QuestionCard& c, int64_t now, QuestionEffects& fx, std::wstring& toast);
        /// <summary>Under `_mtx`: the card with this key, or null. The pointer dies with the lock (the vector may move).</summary>
        /// <param name="key">The card's key.</param>
        /// <returns>The card, or null.</returns>
        QuestionCard* _findQuestionCardLocked(std::wstring_view key);
        /// <summary>
        /// The mirror met a QUESTION item (Messaging.cpp _applyItems, tick thread). A card of the session that shows the same
        /// questions takes it (its tool_use id recorded); else, with a question registry, it waits as a PlainQuestion for its hook's
        /// request. Either way the caller only closes the turn's work log — nothing is posted now.
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="it">The question item (its text is the questions' signature, its toolUseId the dialog's).</param>
        /// <param name="html">The show-only message, posted if no hook offers the question within kAskPairWaitMs.</param>
        /// <param name="sound">The show-only message rings (the turn's one "needs you" sound).</param>
        /// <param name="replyRef">The turn's prompt (the message replies to it).</param>
        /// <returns>True when a card shows it or it waits for one; false without a question registry (the m5 harness): the caller
        /// posts the question at once, as before §21.30.</returns>
        /// <exception cref="std::bad_alloc">If the plain question cannot be stored.</exception>
        bool _noteTranscriptQuestion(const std::wstring& sessionId, const TopicItem& it, const std::wstring& html, bool sound, const std::shared_ptr<PromptRef>& replyRef);
        /// <summary>
        /// The mirror met a TOOL RESULT (an answer item, or a plain result) of a dialog (tick thread): a card with that tool_use id
        /// takes it as its result (_applyQuestionResultLocked) and shows the answer itself; a plain question of that tool_use that
        /// still waits for its hook is posted NOW, so it lands above the answer line that follows. Cheap for every other tool's result.
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="toolUseId">The result's tool_use id.</param>
        /// <param name="answerText">The answer as the item carries it ("" for a plain result).</param>
        /// <param name="result">Answered, Rejected, or TimedOut (the item's `afkTimeoutMs`: Claude Code's own idle timeout).</param>
        /// <param name="timeoutMs">TimedOut: the idle timeout in ms (the card shows it); 0 otherwise.</param>
        /// <returns>True when a posted card shows this result: the mirror then posts no "You ↩ answer" line.</returns>
        /// <exception cref="std::bad_alloc">If a text cannot be allocated.</exception>
        bool _onTranscriptAnswer(const std::wstring& sessionId, const std::wstring& toolUseId, const std::wstring& answerText, QuestionResult result, int64_t timeoutMs = 0);
        /// <summary>
        /// A question card's button ("q:&lt;card&gt;:&lt;verb&gt;[:&lt;arg&gt;]", _handleCallback, inside the tap's scope): an option (single-select
        /// answers it — a card of one question SENDS at once, as the PC's Enter does; on a card of several the next unanswered tab,
        /// or the review, comes next — multi-select toggles it), ✍️ write my own answer or 📝 a note (a ForceReply prompt opens the
        /// phone's keyboard), the note removed or sent alone, ✅ Done, a TAB (a question, or the review: "Review your answers", the
        /// PC's Submit tab; ◀ / ▶ of an earlier keyboard move one tab), 📨 Submit answers, 💬 Chat about this (the PC's rejection
        /// with its clarify request), ✖️ Cancel (asked to confirm, then the PC's Esc: the turn ends), 🖼 the previews as a picture,
        /// and the "what is this message?" question's three buttons (my answer / note, 💬 a reply instead of answering — Claude
        /// Code's `response` — or the next prompt). A button of a card this run does not know answers "no longer open here" and
        /// loses its keyboard. Poll thread.
        /// </summary>
        /// <param name="data">The callback data.</param>
        /// <param name="threadId">The topic the button sits in.</param>
        /// <param name="messageId">The message the button sits on (the card, or an "answer or next prompt?" question).</param>
        /// <returns>The tap's toast ("" = none).</returns>
        /// <exception cref="std::bad_alloc">If a text, the decision or an outbox item cannot be allocated.</exception>
        std::wstring _onQuestionTap(const std::wstring& data, int64_t threadId, int64_t messageId);
        /// <summary>
        /// A typed message in a session's topic while its question card is open (_handleTopicMessage, poll thread): a /command is
        /// not the card's; a message written after ✍️ / 📝 or as a reply to the card (or its write prompt) is TAKEN — the user's own
        /// answer (one question: sent at once, as the PC's "Type something." + Enter) or the note, for the tab's question (on the
        /// review: the first one without an answer) — with a 👌 on it; any other message, and a reply when no question is left to
        /// take it, is HELD and asked about: my answer (note), a reply to Claude instead of answering, or the next prompt?
        /// </summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="msg">The Telegram message.</param>
        /// <param name="threadId">The topic.</param>
        /// <returns>True when the card took or held it (the caller does nothing more); false: the topic's usual handling.</returns>
        /// <exception cref="std::bad_alloc">If a text, the decision or an outbox item cannot be allocated.</exception>
        bool _takeQuestionText(const std::wstring& sessionId, const json::Value& msg, int64_t threadId);
        /// <summary>An EDIT of a message the "answer or next prompt?" question holds (poll thread): its new text replaces the held one.</summary>
        /// <param name="sessionId">The topic's session.</param>
        /// <param name="msg">The edited message.</param>
        /// <returns>True when a card held it.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be copied.</exception>
        bool _questionTextEdited(const std::wstring& sessionId, const json::Value& msg);
        /// <summary>
        /// Carries out a change outside `_mtx`: the decision to the hook (refused when the PC was first: the card then says so),
        /// a release, the transient deletes, the 👌, the card's post or edit, the ForceReply prompt, the log and nav lines.
        /// </summary>
        /// <param name="fx">What to do.</param>
        /// <returns>False when a decision was refused because the request was no longer open (the PC answered or closed it first).</returns>
        /// <exception cref="std::bad_alloc">If an outbox item or a line cannot be allocated.</exception>
        bool _applyQuestionEffects(QuestionEffects fx);
        /// <summary>
        /// Posts a card's message (its first post rings when `sound`, and replies to the turn's prompt); its answer records the
        /// message id and queues the edit that catches up with any change made while the post was on its way. A refused post is
        /// tried again by the tick while the card is open. Nothing while a post is in flight.
        /// </summary>
        /// <param name="key">The card's key.</param>
        /// <exception cref="std::bad_alloc">If the text or the outbox item cannot be allocated.</exception>
        void _postQuestionCard(const std::wstring& key);
        /// <summary>
        /// Queues the edit of a card's message (one per card in the outbox: "qcard:&lt;key&gt;" replaces a queued one). The text is
        /// rendered AGAIN right before the call (the item's beforeSend), so the edit always shows the card's latest state however
        /// many changes came while it waited; its answer queues one more edit when a change came after that render. Nothing for a
        /// card whose message is not known (its post's answer catches up instead).
        /// </summary>
        /// <param name="key">The card's key.</param>
        /// <exception cref="std::bad_alloc">If the text or the outbox item cannot be allocated.</exception>
        void _queueQuestionCardEdit(const std::wstring& key);
        /// <summary>
        /// Posts the ForceReply prompt of ✍️ / 📝 (silent, outside the sticky card's clock): Telegram opens the phone's keyboard
        /// as a reply to it, with a placeholder. One per card at a time; deleted when the text came, or the writing was cancelled.
        /// </summary>
        /// <param name="key">The card's key.</param>
        /// <param name="target">Answer or Note.</param>
        /// <exception cref="std::bad_alloc">If the text or the outbox item cannot be allocated.</exception>
        void _postQuestionWritePrompt(const std::wstring& key, AskTextTarget target);
        /// <summary>
        /// Deletes one of the bot's own transient messages (a write prompt, an "answer or next prompt?" question) — at the answers'
        /// priority, since a tapped button that asked for it waits for it (§21.28). A refusal is logged only.
        /// </summary>
        /// <param name="messageId">The message (0 or less = nothing).</param>
        /// <exception cref="std::bad_alloc">If the outbox item cannot be allocated.</exception>
        void _deleteQuestionMessage(int64_t messageId);
        /// <summary>
        /// Posts a plain question show-only into its session's CURRENT topic (a recreation may have made it anew meanwhile); none
        /// when the session has no topic any more.
        /// </summary>
        /// <param name="p">The plain question.</param>
        /// <exception cref="std::bad_alloc">If the text or the outbox item cannot be allocated.</exception>
        void _postPlainQuestion(const PlainQuestion& p);
        /// <summary>
        /// 🖼: draws the card's current question's previews (BuildAskPreviewSheet) into a PICTURE on a worker thread of its own and
        /// sends it to the topic as a photo — box drawing and ASCII mockups survive a phone's narrow screen there. One picture per
        /// card at a time, and the screen pictures' shared bound (kAskPictureMaxRenders). The tapped button stays busy until the
        /// photo is in (the worker carries the tap's holds, §21.28). Poll thread.
        /// </summary>
        /// <param name="key">The card's key.</param>
        /// <returns>The tap's toast ("" when the drawing started).</returns>
        /// <exception cref="std::bad_alloc">If the sheet cannot be built.</exception>
        std::wstring _startQuestionPicture(const std::wstring& key);

        /// <summary>The process's question registry (SetAskRegistry; null in the m5 harness). Under `_mtx`; never called with `_mtx` held.</summary>
        std::shared_ptr<AskRegistry> _asks;
        /// <summary>The question cards of this run, oldest first (at most kAskMaxCards). Under `_mtx`.</summary>
        std::vector<QuestionCard> _questionCards;
        /// <summary>The questions the mirror met that wait for their hook's request (kAskPairWaitMs). Under `_mtx`.</summary>
        std::vector<PlainQuestion> _plainQuestions;
        /// <summary>The requests already turned into a card or released by the tick: never a second card for one. Under `_mtx`.</summary>
        std::unordered_set<uint64_t> _asksTaken;

        // ----- §21.34: a LOCAL command's dialog answered from Telegram — /add-dir's menu (Messaging.Menus.cpp; the pure half is
        //       TuiMenus.h) -----

        /// <summary>
        /// One /add-dir a topic queued (_queueRemotePrompt), WATCHED from its queueing to its result. The command runs inside
        /// Claude Code and records nothing until it is done, so the watch has three readers: the registry (the prompt's delivery),
        /// the screen (the dialog, read by the hosting window — kMenuScreenAction — once the prompt was delivered), and the
        /// transcript from where it stood at the queueing (the echo and the result Claude Code writes once the command ran). A
        /// dialog found becomes the MENU CARD (its options as buttons, the countdown of the user's rule: the default is pressed by
        /// itself kTuiMenuAutoAnswerMs after the dialog was seen); a result without a dialog becomes a reply to the user's
        /// message. Under `_mtx`; never two for one prompt.
        /// </summary>
        struct MenuWatch
        {
            std::wstring key; // the card's callback key ([0-9a-z]{10}, random): a button of another card, or of an earlier run, never acts on this one
            std::wstring sessionId;
            std::wstring promptId; // the queued prompt: its delivery (Sent, injected) and its echo are read in the registry
            std::wstring promptText; // what was queued ("/add-dir <path>"): the transcript's echo is matched against it
            std::wstring args; // the path as the user typed it
            int64_t messageId{ 0 }; // the user's /add-dir message: the card and the replies answer it
            int64_t threadId{ 0 }; // the topic (re-read at each post: a recreated topic has a new thread)
            int64_t createdMs{ 0 };
            // The transcript reader (ReadMenuTranscript, outside `_mtx`).
            std::wstring transcriptPath; // "" until it resolves (a session's first command can come before its transcript)
            int64_t offset{ 0 }; // where the reader goes on: the transcript's size at the queueing, then after each complete line read
            int64_t lastReadMs{ 0 };
            bool echoSeen{ false }; // the command's echo was read; its result is the next result line
            int64_t echoTsMs{ 0 }; // the echo line's time (the outcome's time on the card)
            // The dialog (the screen reads).
            bool menuSeen{ false }; // the dialog was found: the card exists (model) and the clock runs
            int probes{ 0 }; // screen reads asked for (at most kMenuProbeMax)
            bool probeInFlight{ false };
            int64_t probeSentMs{ 0 };
            uint64_t probeSerial{ 0 }; // a reply of an older read (one that timed out) is ignored
            bool unreadableNoted{ false }; // the "a dialog this Agentmaster cannot read" note went (once)
            int64_t notStartedSinceMs{ 0 }; // when the tab was first seen without its claude while the card was open (0 = running)
            // The card.
            AddDirCardModel model;
            int64_t cardMessageId{ 0 }; // 0 = not posted (or refused, or deleted), -1 = its post is in flight
            int64_t postFailedMs{ 0 }; // when the last post was refused (an open card posts again kMenuRepostAfterFailureMs later)
            uint64_t version{ 1 }; // bumped by every change of what the card shows
            uint64_t sendingVersion{ 0 }; // the version the edit on its way rendered (its beforeSend)
            uint64_t shownVersion{ 0 }; // the version Telegram last took: a newer one wants another edit
            // The press (kMenuAnswerAction).
            bool pressInFlight{ false };
            int64_t pressSentMs{ 0 };
            uint64_t pressSerial{ 0 };
            int pressAttempts{ 0 }; // presses that counted (at most kMenuPressMaxAttempts; a BUSY answer gives its attempt back)
            MenuAnswerResult lastPress{ MenuAnswerResult::Failed }; // the window's last reply (meaningful once pressSerial > 0)
            int64_t lastPressMs{ 0 }; // when it came (or when its wait ran out)
            int64_t busySinceMs{ 0 }; // the first of an unbroken run of BUSY answers (0 = the last answer was not busy)
            int64_t finishedMs{ 0 }; // when the watch's work ended (a result, a reply, a close); forgotten kMenuWatchKeepMs later
        };

        /// <summary>
        /// _queueRemotePrompt, right after a "/add-dir &lt;path&gt;" went into the queue: starts its watch, with the transcript's size NOW
        /// as the reader's start (a dialog-free command's result can land within milliseconds of the delivery, before any tick).
        /// At most kMenuMaxWatches open watches (the oldest open one is dropped beyond it, logged). Poll thread.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="promptId">The queued prompt.</param>
        /// <param name="promptText">Its text.</param>
        /// <param name="args">The path as typed (ParseAddDirCommandText).</param>
        /// <param name="messageId">The user's message.</param>
        /// <param name="threadId">The topic.</param>
        /// <exception cref="std::bad_alloc">If the watch cannot be stored (the caller's guard logs it; the command still goes, unwatched).</exception>
        void _watchAddDir(const std::wstring& sessionId, const std::wstring& promptId, const std::wstring& promptText, const std::wstring& args, int64_t messageId, int64_t threadId);
        /// <summary>
        /// The tick's pass over the menu watches (after _questionsTick): each open watch is advanced (_tickMenuWatch), and finished
        /// ones are forgotten kMenuWatchKeepMs after their end. Tick thread. Never throws (Rule #18: a failure is logged, and the next
        /// tick runs the pass again).
        /// </summary>
        void _menusTick() noexcept;
        /// <summary>
        /// One watch's step: reads the prompt's delivery in the registry and the transcript past the watch's offset (both outside
        /// `_mtx`), then decides under it — the result (the card's outcome, or a reply when no dialog was shown), the session's end,
        /// a screen read for the dialog, the clock's default press, a press to repeat — and carries out what it decided outside the
        /// lock. Tick thread.
        /// </summary>
        /// <param name="key">The watch's key.</param>
        /// <param name="now">The tick's wall clock.</param>
        /// <exception cref="std::bad_alloc">If a copy or a message cannot be allocated (the pass's guard logs it).</exception>
        void _tickMenuWatch(const std::wstring& key, int64_t now);
        /// <summary>
        /// A screen read's reply (the hosting window's UI thread): the dialog of the watched command found on it (ParseAddDirMenuScreen,
        /// and its path the command's — AddDirMenuBelongsTo) opens the card: phase Open, the clock started, the card posted.
        /// A reply of an older read is ignored.
        /// </summary>
        /// <param name="key">The watch's key.</param>
        /// <param name="serial">The read's serial (MenuWatch::probeSerial when it was asked for).</param>
        /// <param name="screen">The terminal's last rows ("" for a dormant tab).</param>
        /// <exception cref="std::bad_alloc">If the parse or the card cannot be allocated (the window's action guard logs it).</exception>
        void _onMenuScreen(const std::wstring& key, uint64_t serial, const std::wstring& screen);
        /// <summary>
        /// Asks the hosting window to press the card's chosen option (MenuWatch::model.pressed): kMenuAnswerAction with the card's
        /// path, so the window presses only into that very dialog. `taps` (a tapped option's holds, §21.28) go with the reply, so the
        /// tapped button stays busy until the window answered. Without a session-action hook (the m5 harness) the press fails at once.
        /// </summary>
        /// <param name="key">The watch's key.</param>
        /// <param name="taps">The holds of the tap that chose the option (empty for the clock's press).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be allocated.</exception>
        void _pressMenuOption(const std::wstring& key, std::vector<TapHoldRef> taps);
        /// <summary>
        /// The window's reply to a press (MenuAnswerResultWord): recorded for the tick, which waits for the transcript's result after a
        /// press, repeats a press that found the box busy or failed, and closes the card when the dialog went away with no result.
        /// A reply of an older press is ignored. The hosting window's UI thread.
        /// </summary>
        /// <param name="key">The watch's key.</param>
        /// <param name="serial">The press's serial.</param>
        /// <param name="reply">The window's word.</param>
        /// <exception cref="std::bad_alloc">If the log line cannot be built.</exception>
        void _onMenuPressed(const std::wstring& key, uint64_t serial, const std::wstring& reply);
        /// <summary>
        /// A menu card's button ("tm:&lt;key&gt;:&lt;digit&gt;", _handleCallback, inside the tap's scope): an Open card turns Answering with
        /// the tapped option and asks the window to press it; any other card says what became of the dialog. A button of a card this
        /// run does not know (an earlier run's) loses its keyboard. Poll thread.
        /// </summary>
        /// <param name="data">The callback data.</param>
        /// <param name="messageId">The message the button sits on.</param>
        /// <returns>The tap's toast ("" for none).</returns>
        /// <exception cref="std::bad_alloc">If the toast or an outbox item cannot be allocated.</exception>
        std::wstring _onMenuTap(const std::wstring& data, int64_t messageId);
        /// <summary>
        /// Posts the card in the watch's CURRENT topic as a reply to the user's /add-dir message, with sound (a dialog that answers
        /// itself in a minute is a "needs you" moment), at an answer's priority. A post Telegram refuses is tried again
        /// kMenuRepostAfterFailureMs later while the card is open; a change made while the post was on its way is edited in after.
        /// </summary>
        /// <param name="key">The watch's key.</param>
        /// <exception cref="std::bad_alloc">If the text or the outbox item cannot be allocated.</exception>
        void _postMenuCard(const std::wstring& key);
        /// <summary>
        /// Edits the card to what it shows NOW (rendered again right before the call, one queued edit per card): the outcome, the
        /// answering state, or the close — an edit without a keyboard removes the buttons. A card whose message is gone is not
        /// edited again.
        /// </summary>
        /// <param name="key">The watch's key.</param>
        /// <exception cref="std::bad_alloc">If the text or the outbox item cannot be allocated.</exception>
        void _queueMenuCardEdit(const std::wstring& key);
        /// <summary>The watch of a key, or nullptr. Caller holds `_mtx`.</summary>
        /// <param name="key">The key.</param>
        /// <returns>The watch (valid while `_mtx` is held and the list is not changed).</returns>
        MenuWatch* _findMenuWatchLocked(std::wstring_view key);

        /// <summary>The /add-dir watches of this run, oldest first. Under `_mtx`; cleared at a start and at a change of bot.</summary>
        std::vector<MenuWatch> _menuWatches;
    };
}
