// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TeamsService: the fleet in ONE Teams channel, posted as the owner through Microsoft Graph, with no bot and no
// administrator (TEAMS_CHANNEL.md). It runs inside the app beside the Telegram bot (MessagingService); each keeps its own state.
//
// What runs (TEAMS_CHANNEL.md §12):
//   * the TICK thread: the two token sources (TeamsAuth.h) and which one makes which call (the router, TeamsModels.h
//     PickTokenFor), the device-code sign-in, the session threads (a root post that is the card, edited as the session
//     changes; the history replay; the live mirror with its alerts; the approval notice), the legend post and its fleet
//     board, the state file;
//   * the SEND thread: the outbox — every Graph write in priority order, one call per 1.2 s for the channel (§6.5), a 429's
//     Retry-After, retries with back-off, a 401 that renews the token once, a 403 that moves the call to the other token, and
//     LANES: one session's posts go out strictly in order;
//   * the POLL thread (§7): the channel's delta at 5 s / 30 s / 90 s, each changed session thread's newest replies, the owner's
//     new replies as prompts and commands (§7.5), the backlog offer (§7.4), a new post's folder step and its pick (§9.1);
//   * a window's thread, briefly: a new session's launch answer is only queued under a leaf lock (_launchMtx) for the tick,
//     which binds the post to the session (§9.1).
// Milestones M2–M6 (§13) are this file's scope (M5: the emoji on a card, a confirm, a backlog offer, a queued prompt; M6 so
// far: the screen's picture, the status report, a new session from a new post). A thread's transcript cursor is COMMITTED only when the posts that cover it
// are answered (and never past a text still held), so a restart reads again what was not posted: at least once, never lost.
// Both Agentmaster and the owner write AS THE OWNER: every post is journaled by id once answered and fingerprinted when
// queued, so the poll never takes Agentmaster's own post for the owner's. The emoji actions (M5) and new sessions (M6) are
// built on it.
//
// Every entry point is no-throw (a failure is logged and the status line says it); every catch logs (Rule #18). The state
// file (<profile>\messaging\teams\state-<channel>.json) is written only through WriteStateDocument (Rule #16). Nothing runs
// on a UI thread: Apply and Stop join the threads, which never wait long (an HTTP call in flight is aborted, and the Azure
// CLI's run is detached).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <map>

#include "GraphClient.h"
#include "Messaging.h" // Msg::ServiceHooks: the window-hosted seams, the same ones the Telegram connector uses
#include "Persistence.h" // DirColorIndex: the folder colors the cards and the board read
#include "TeamsAuth.h"
#include "TeamsModels.h"

namespace Agentmaster
{
    class SessionRegistry;
    class Scheduler;
}

namespace Agentmaster::Teams
{
    /// <summary>The order the outbox sends in (§6.5): a lower value goes first; a tie goes by arrival.</summary>
    enum class OutboxPriority : int
    {
        /// <summary>An answer to an action the owner started (a toast-like reply).</summary>
        Answer = 0,
        /// <summary>A post that alerts the owner (the end of a turn, Claude needs them, an error).</summary>
        Alert = 1,
        /// <summary>The live mirror.</summary>
        Mirror = 2,
        /// <summary>A card: a thread's root, or its edit.</summary>
        Card = 3,
        /// <summary>The history replay of a new thread.</summary>
        History = 4,
        /// <summary>The legend's fleet board.</summary>
        Legend = 5
    };

    /// <summary>One Graph write waiting in the outbox (§6.5). Kept in memory only (§6.5: the durable journal is a later step).</summary>
    struct OutboxItem
    {
        /// <summary>Its arrival number (a tie inside one priority goes to the lower).</summary>
        uint64_t seq{ 0 };
        /// <summary>Its priority.</summary>
        OutboxPriority priority{ OutboxPriority::Mirror };
        /// <summary>The kind of call: what the router routes.</summary>
        GraphOp op{ GraphOp::Post };
        /// <summary>The HTTP method ("POST", "PATCH", "DELETE").</summary>
        std::wstring method;
        /// <summary>The Graph path (GraphClient checks it again).</summary>
        std::wstring path;
        /// <summary>The JSON body (Null = none).</summary>
        json::Value body;
        /// <summary>The call's purpose for the log ("legend post").</summary>
        std::wstring what;
        /// <summary>
        /// The session whose thread the write goes into: the writes of one lane go out strictly in their order (a retry holds
        /// the lane), so a thread's history, prompts and replies never overtake each other whatever their priorities. "" = no
        /// lane (a card edit, the legend: a newer one supersedes an older one anyway).
        /// </summary>
        std::wstring lane;
        /// <summary>How many attempts failed so far with a retryable answer.</summary>
        int attempts{ 0 };
        /// <summary>Not before this time (Unix ms): a retry's back-off or a 429's wait.</summary>
        int64_t notBeforeMs{ 0 };
        /// <summary>A 403 already moved it to the other token once.</summary>
        bool triedOtherSource{ false };
        /// <summary>A 401 already renewed its token once.</summary>
        bool renewedToken{ false };
        /// <summary>
        /// The final answer, on the send thread, with no service lock held (empty = none wanted). Not called for a call the stop
        /// aborted: the service is going away.
        /// </summary>
        std::function<void(const GraphResult&)> done;
    };

    /// <summary>
    /// A launch asked of a window from Teams, from the ask to the window's answer: a NEW SESSION for the owner's new post (§9.1:
    /// what binding the post to the session needs once it is open), one asked IN a session's thread — a FORK (🔀,
    /// /forksession) or a new session in its folder (/newsession) — whose thread hears how it went (§8), or a closed session
    /// RESUMED from a 🔎 Search answer (§9.3). Kept in memory only (the answer comes within the run, or never).
    /// </summary>
    struct LaunchAsk
    {
        /// <summary>Which launch this is (what its answer does).</summary>
        enum class Kind
        {
            /// <summary>A new session for the owner's new post (§9.1): the post becomes its thread.</summary>
            NewPost,
            /// <summary>A fork of askedIn (§8 🔀): askedIn's thread hears how it went.</summary>
            Fork,
            /// <summary>A new session in askedIn's folder (/newsession): askedIn's thread hears how it went.</summary>
            NewInFolder,
            /// <summary>A closed session resumed from a Search answer (§9.3): the Search thread (rootId) hears how it went.</summary>
            SearchPick
        };
        /// <summary>Which launch this is.</summary>
        Kind kind{ Kind::NewPost };
        /// <summary>Fork / NewInFolder: the session whose thread asked (a fork's SOURCE, or /newsession's session).</summary>
        std::wstring askedIn;
        /// <summary>NewPost: the owner's post (the session's thread once it is open). SearchPick: the Search thread's root.</summary>
        std::wstring rootId;
        /// <summary>
        /// The id the new session takes, minted by the service (RemoteLaunchRequest::newSessionId), so the post is known as that
        /// session's thread before its tab exists. "" for a fork (the window mints the fork's id; the answer names it).
        /// </summary>
        std::wstring sessionId;
        /// <summary>The post's words: the session's first prompt.</summary>
        std::wstring prompt;
        /// <summary>The post's deep link (the thread's webUrl).</summary>
        std::wstring webUrl;
        /// <summary>The folder the session opens in (for the log).</summary>
        std::wstring folder;
        /// <summary>
        /// The newest of the owner's replies under the post that the step read (Unix ms, the server's clock): the thread reads only
        /// newer replies as prompts, so the typed folder (and any words written while the step waited) never become one.
        /// </summary>
        int64_t readToMs{ 0 };
        /// <summary>
        /// The run that asked (TeamsService::_startedMs): an answer that reaches a later run (the settings changed meanwhile) binds
        /// nothing, since that run's state may be another channel's.
        /// </summary>
        int64_t runMs{ 0 };
    };

    /// <summary>
    /// The bot-less Teams connector (see the file header). One per process: Engine::teams, made by std::make_shared — a window's
    /// answer to an action (an Esc, an Enter, a park) holds only a weak reference to it.
    /// </summary>
    class TeamsService : public std::enable_shared_from_this<TeamsService>
    {
    public:
        /// <summary>Creates the service stopped. Apply starts it when the settings ask for it.</summary>
        /// <param name="registry">The fleet (the legend's board; the threads' input from M3 on).</param>
        /// <param name="scheduler">The runner (the prompts from Teams, M4).</param>
        /// <param name="profileDir">The active profile: the state lives in &lt;profile&gt;\messaging\teams\, and the app's sign-in in
        /// Credential Manager under the profile's leaf.</param>
        /// <param name="hooks">The window-hosted seams (the same as the Telegram connector's; members may be empty).</param>
        /// <exception cref="std::bad_alloc">If the service cannot be allocated.</exception>
        TeamsService(std::shared_ptr<SessionRegistry> registry, std::shared_ptr<Scheduler> scheduler, std::wstring profileDir, Msg::ServiceHooks hooks);
        /// <summary>Stop(). Never throws.</summary>
        ~TeamsService();
        TeamsService(const TeamsService&) = delete;
        TeamsService& operator=(const TeamsService&) = delete;

        /// <summary>
        /// Applies the settings (§10): teamsEnabled with a team and a channel starts the threads (idempotent); off, or no
        /// channel, stops them; another channel, app, tenant or Azure CLI switch restarts them; verbose and the markers apply
        /// at once. Each Apply also offers a new sign-in code when the app needs one and none waits (so a cog Save is a way to
        /// get a fresh code). Called at engine init and on every Settings save.
        /// </summary>
        /// <param name="settings">The settings.</param>
        /// <remarks>Joins the threads on a stop (quick: calls in flight are aborted). Never throws: a failure is logged and the
        /// status line says it.</remarks>
        void Apply(const AppSettings& settings) noexcept;

        /// <summary>Stops the threads, aborts the calls in flight, saves the state and drops the outbox. Idempotent. Never throws.</summary>
        void Stop() noexcept;

        /// <summary>Do the threads run? Any thread.</summary>
        /// <returns>True while running.</returns>
        bool Running() const noexcept;

        /// <summary>
        /// The cog's status line (TeamsModels.h BuildStatusLine): off, what to set, the sign-in code to enter, or "on" with who
        /// makes which call, and the newest problem. Any thread.
        /// </summary>
        /// <returns>The line.</returns>
        /// <exception cref="std::bad_alloc">If the line cannot be built.</exception>
        std::wstring StatusText() const;

        /// <summary>
        /// The app is closing (its last window closed; TerminateProcess follows, so no Stop runs): the state file gets its last
        /// save now. Runs under the engine's window mutex and takes only the service's short locks. Never throws.
        /// </summary>
        void NoteAppClosing() noexcept;

        /// <summary>
        /// Sets the 🔎 Search thread's agent factory (TEAMS_CHANNEL.md §9.3): the app's MakeTeamsSearchBridge, a conversation of
        /// the connector's own. Set once per process by the app layer (TerminalPage.AgentEngine.cpp), before or while the
        /// connector runs; without one (the test harness) no Search thread is posted. Never throws.
        /// </summary>
        /// <param name="factory">The factory (empty = none).</param>
        void SetSearchFactory(Msg::SearchBridgeFactory factory) noexcept;

        /// <summary>The cog's Sign in: a new device code is asked on the tick thread (it replaces one that waits). Never throws.</summary>
        void RequestSignIn() noexcept;

        /// <summary>The cog's Sign out: the app's stored sign-in is deleted (Credential Manager). Never throws.</summary>
        void SignOut() noexcept;

    private:
        /// <summary>The router's index of the app's token (PickTokenFor's tie goes to the lower index).</summary>
        static constexpr size_t kAppSource = 0;
        /// <summary>The router's index of the Azure CLI's token.</summary>
        static constexpr size_t kCliSource = 1;

        /// <summary>
        /// What the service knows of one thread between passes, in memory only (the persisted half is ChannelState::threads):
        /// the live mirror's carry-over, where the next read starts, and what is in the outbox for it. Cleared at every start,
        /// so nothing a stopped run left in flight blocks the next one.
        /// </summary>
        struct ThreadRuntime
        {
            /// <summary>The live mirror's held text and verbose narrative (§6.1).</summary>
            MirrorState mirror;
            /// <summary>Where the next live read starts (-1 = at the committed cursor, Thread::transcriptOffset).</summary>
            int64_t readOffset{ -1 };
            /// <summary>The root (card) post of a new thread is in the outbox.</summary>
            bool rootPosting{ false };
            /// <summary>A failed root post is tried again after this time (Unix ms).</summary>
            int64_t rootRetryAtMs{ 0 };
            /// <summary>A card edit is in the outbox.</summary>
            bool cardEditQueued{ false };
            /// <summary>A history window's posts are in the outbox: the next window waits for their answers.</summary>
            bool historyInFlight{ false };
            /// <summary>The history's day key (Msg::TgDayKey of the last turn replayed): a day line marks a new day.</summary>
            int64_t historyDayKey{ 0 };
            /// <summary>When the transcript was last looked for (Unix ms): a session without one is looked for every 10 s.</summary>
            int64_t lastResolveMs{ 0 };
            /// <summary>When a "needs you" post (a question, a plan, an approval) was last queued (Unix ms).</summary>
            int64_t lastNeedsYouMs{ 0 };
            /// <summary>The state the last pass saw (an approval alerts on ENTERING NeedsApproval).</summary>
            SessionState lastState{ SessionState::Idle };
            /// <summary>`lastState` holds a pass's value.</summary>
            bool stateSeen{ false };
            /// <summary>The card of a session that left the live set was brought up to date ("closed").</summary>
            bool closedShown{ false };
            /// <summary>No source can edit (§4.2): the last state line posted as a reply instead of a card edit.</summary>
            std::wstring lastStateLine;
        };

        /// <summary>A prompt the owner sent from Teams whose status is shown on their reply (§2: 👀 → 👨‍💻 → 👌).</summary>
        struct PromptTrack
        {
            /// <summary>The session.</summary>
            std::wstring sessionId;
            /// <summary>The thread's root.</summary>
            std::wstring rootId;
            /// <summary>The owner's reply the reactions go on.</summary>
            std::wstring replyId;
            /// <summary>The queued prompt.</summary>
            std::wstring promptId;
            /// <summary>The status emoji shown now ("" = none yet).</summary>
            std::wstring shown;
            /// <summary>When the follow began (Unix ms; it ends after kPromptTrackMaxMs at most).</summary>
            int64_t sinceMs{ 0 };
        };

        /// <summary>How one poll went.</summary>
        enum class PollOutcome
        {
            /// <summary>No source can read (or the service stops): the next poll waits 30 s.</summary>
            NoReader,
            /// <summary>The channel was read; nothing changed.</summary>
            Unchanged,
            /// <summary>The channel was read; something changed.</summary>
            Changed,
            /// <summary>The read failed (logged; a 429's wait is in retryAfterSec).</summary>
            Failed
        };

        /// <summary>Both sources' tokens at one moment, and the router's view of them.</summary>
        struct SourceTokens
        {
            /// <summary>The app's token (none: no app, not signed in, or refused).</summary>
            std::optional<AccessToken> app;
            /// <summary>The Azure CLI's token (none: off, not ready, or another account).</summary>
            std::optional<AccessToken> cli;
            /// <summary>The router's view: [kAppSource] the app, [kCliSource] the Azure CLI.</summary>
            std::vector<TokenSourceView> views;
            /// <summary>Why the Azure CLI's token was left out although it had one ("" = it was not).</summary>
            std::wstring cliProblem;
        };

        /// <summary>Starts the threads for the current config (`_applyMtx` held, `_mtx` not). Never throws.</summary>
        void _start() noexcept;
        /// <summary>Stops the threads (`_applyMtx` held, `_mtx` not): aborts, joins, saves, drops the outbox. Never throws.</summary>
        void _stop() noexcept;
        /// <summary>The tick thread's body. Never throws.</summary>
        void _tickLoop() noexcept;
        /// <summary>The send thread's body. Never throws.</summary>
        void _sendLoop() noexcept;
        /// <summary>
        /// One tick: the new sessions' launch answers (first: never throwing, it clears the flag that ends the tick's wait; and
        /// before the threads, so a post bound to its session is that session's thread in the same pass), the router, the
        /// sign-in, the threads, the legend, the housekeeping.
        /// </summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::exception">Anything a step throws; the loop logs it and goes on.</exception>
        void _tickOnce(int64_t nowMs);
        /// <summary>Both sources' tokens (a refresh of the app's when due; the CLI's never blocks), the same-account check.</summary>
        /// <param name="http">The client for the app's refresh.</param>
        /// <param name="useCli">teamsUseAzureCli.</param>
        /// <returns>The tokens and the router's view.</returns>
        /// <exception cref="std::bad_alloc">If the tokens cannot be copied.</exception>
        SourceTokens _sourceTokens(GraphClient& http, bool useCli);
        /// <summary>The router's step: the owner, and which source makes each kind of call, for the status line.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the words cannot be allocated.</exception>
        void _routeStep(int64_t nowMs);
        /// <summary>The device-code sign-in's step: asks the token endpoint when due; starts a sign-in when asked, or once per
        /// Apply when the app needs one.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If a text cannot be allocated.</exception>
        void _signInStep(int64_t nowMs);
        /// <summary>The legend's step (§5): posts it when it is missing; edits its fleet board at most once a minute when it changed.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _legendStep(int64_t nowMs);
        /// <summary>The legend post's answer (on the send thread): keeps its id and journals it, or schedules a new try.</summary>
        /// <param name="r">The answer.</param>
        /// <param name="boardHash">The board the post carried.</param>
        void _onLegendPosted(const GraphResult& r, uint64_t boardHash) noexcept;
        /// <summary>The legend edit's answer (on the send thread): a 404 means the owner deleted the post, so a new one is made.</summary>
        /// <param name="r">The answer.</param>
        /// <param name="boardHash">The board the edit carried.</param>
        /// <param name="legendId">The post that was edited.</param>
        void _onLegendEdited(const GraphResult& r, uint64_t boardHash, const std::wstring& legendId) noexcept;
        /// <summary>
        /// The threads' step (§5, §6; at most every 2 s): a thread for each session that qualifies, then for each thread its
        /// card, its approval notice, its history replay or its live mirror; a thread whose session left the live set gets its
        /// card brought up to date once and its transcript finished.
        /// </summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If a snapshot or a post cannot be built.</exception>
        void _threadsStep(int64_t nowMs);
        /// <summary>Queues a new thread's root post (the card) for a session (one at a time; a failure waits a minute).</summary>
        /// <param name="s">The session.</param>
        /// <param name="colorHex">Its tab color.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _postRoot(const SessionInfo& s, const std::wstring& colorHex, int64_t nowMs);
        /// <summary>The root post's answer (on the send thread): the thread's record is made and journaled, or a retry is scheduled.</summary>
        /// <param name="r">The answer.</param>
        /// <param name="sessionId">The session.</param>
        /// <param name="cardHash">The card the root carries.</param>
        void _onRootPosted(const GraphResult& r, const std::wstring& sessionId, uint64_t cardHash) noexcept;
        /// <summary>
        /// The card's step (§5): an edit (subject and body) when the card changed, at most every 10 s; with no source that can
        /// edit (§4.2), a changed state line is posted as a short reply instead.
        /// </summary>
        /// <param name="s">The session (a record whose queue holds its open prompts).</param>
        /// <param name="colorHex">Its tab color.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <param name="canEdit">A source can edit.</param>
        /// <returns>True when the card shows the session as it is now (nothing to edit).</returns>
        /// <exception cref="std::bad_alloc">If the card cannot be built.</exception>
        bool _cardStep(const SessionInfo& s, const std::wstring& colorHex, int64_t nowMs, bool canEdit);
        /// <summary>A card edit's answer (on the send thread): a 404 means the owner deleted the thread, so its record goes and
        /// the next pass makes a new one.</summary>
        /// <param name="r">The answer.</param>
        /// <param name="sessionId">The session.</param>
        /// <param name="cardHash">The card the edit carried.</param>
        /// <param name="rootId">The root that was edited.</param>
        void _onCardEdited(const GraphResult& r, const std::wstring& sessionId, uint64_t cardHash, const std::wstring& rootId) noexcept;
        /// <summary>
        /// The approval notice (§6.4): on ENTERING NeedsApproval (a recent change, not a state restored at a start), one
        /// NeedsYou post — unless a question or a plan alerted moments before.
        /// </summary>
        /// <param name="s">The session.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _approvalStep(const SessionInfo& s, int64_t nowMs);
        /// <summary>The transcript's step for one thread: its path looked up when unknown, then the history replay until it is
        /// done, the live mirror after it.</summary>
        /// <param name="s">The session.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the posts cannot be built.</exception>
        void _mirrorStep(const SessionInfo& s, int64_t nowMs);
        /// <summary>
        /// The history replay's step (§6.3): the next window of the transcript (complete lines only, growing for a giant line),
        /// split by turns, rendered as Telegram's history (non-verbose unless teamsVerbose), packed into posts; the replay's
        /// last window leaves the newest turn to the live mirror. One window in flight per thread.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="path">Its transcript.</param>
        /// <exception cref="std::bad_alloc">If the posts cannot be built.</exception>
        void _historyStep(const std::wstring& sessionId, const std::wstring& path);
        /// <summary>
        /// The live mirror's step (§6.1): the complete lines past the read offset (at most 1 MiB a pass), MirrorLines' decisions,
        /// the posts queued in the thread's lane with their commits.
        /// </summary>
        /// <param name="s">The session (its state says whether a held text is the final reply).</param>
        /// <param name="path">Its transcript.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the posts cannot be built.</exception>
        void _liveStep(const SessionInfo& s, const std::wstring& path, int64_t nowMs);
        /// <summary>
        /// Queues a reply in a session's thread, in its lane: split under the size limit (the alert and the commit on the last
        /// part); its answer journals the post (§7.2: Agentmaster's own posts are never read as the owner's), moves the thread's
        /// cursor to `commitTo` (unless the stop aborted it, or no token could send it), then calls `onDone`.
        /// </summary>
        /// <param name="sessionId">The session (its thread must exist).</param>
        /// <param name="html">The reply's Teams HTML.</param>
        /// <param name="alert">Whether it alerts the owner.</param>
        /// <param name="priority">Its priority.</param>
        /// <param name="commitTo">The cursor's move once it is answered (0 = none).</param>
        /// <param name="onDone">Called with the last part's answer (may be empty).</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _postReply(const std::wstring& sessionId, const std::wstring& html, AlertKind alert, OutboxPriority priority, int64_t commitTo,
                        std::function<void(const GraphResult&)> onDone = {});
        /// <summary>Moves a thread's committed transcript cursor forward (never back). Never throws.</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="commitTo">The new cursor.</param>
        void _commit(const std::wstring& sessionId, int64_t commitTo) noexcept;
        /// <summary>Journals a post Agentmaster made (§7.2), from its answer. Never throws.</summary>
        /// <param name="r">The answer (a failure journals nothing).</param>
        void _journalPost(const GraphResult& r) noexcept;
        /// <summary>
        /// Remembers a post's fingerprint (PostFingerprint) for its thread, so the poll knows it as Agentmaster's even before the
        /// send's answer journals its id. Never throws.
        /// </summary>
        /// <param name="rootId">The thread's root ("" = a root post: the channel's own list).</param>
        /// <param name="html">The post's Teams HTML.</param>
        void _rememberOwnPost(const std::wstring& rootId, const std::wstring& html) noexcept;
        /// <summary>Is a message read from the channel one of Agentmaster's recent posts (its fingerprint)? Journals it when it is.</summary>
        /// <param name="rootId">The thread's root ("" = a root).</param>
        /// <param name="m">The message read.</param>
        /// <returns>True when it is Agentmaster's own.</returns>
        bool _isRecentOwnPost(const std::wstring& rootId, const InboundMessage& m) noexcept;

        /// <summary>The poll thread's body (§7.1, §7.3): a read of the channel's delta at NextPollDelayMs' cadence. Never throws.</summary>
        void _pollLoop() noexcept;
        /// <summary>
        /// One poll: the delta link's pages (the first sync from the start, or the link a page left), each page's changed roots
        /// handled at once, and the link saved after every page — a long first sync resumes where it stopped.
        /// </summary>
        /// <param name="retryAfterSec">Receives a 429's wait (0 = none).</param>
        /// <returns>How it went.</returns>
        /// <exception cref="std::bad_alloc">If a page cannot be read into memory.</exception>
        PollOutcome _pollOnce(int& retryAfterSec);
        /// <summary>
        /// A GET with the token that can read (the router), never sooner than kMinReadGapMs after the last read (Graph allows
        /// one read a second per channel).
        /// </summary>
        /// <param name="http">The client.</param>
        /// <param name="pathOrUrl">The path or a link Graph handed back.</param>
        /// <param name="what">The read's purpose for the log.</param>
        /// <param name="result">Receives the answer.</param>
        /// <returns>False when no source can read (nothing was sent).</returns>
        /// <exception cref="std::bad_alloc">If the tokens cannot be copied.</exception>
        bool _readGet(GraphClient& http, const std::wstring& pathOrUrl, std::wstring_view what, GraphResult& result);
        /// <summary>
        /// A root the delta says changed (§7.2): a session's thread reads its new replies; a post whose new session is under way
        /// goes to its step; another root is classified, and the owner's post opens a new session only when it was written in
        /// this run.
        /// </summary>
        /// <param name="root">The root as the delta gave it.</param>
        /// <exception cref="std::bad_alloc">If the root cannot be handled.</exception>
        /// <remarks>
        /// A post written before this run started is history at EVERY poll, not only at the first sync: the delta also returns
        /// an old post when a reply or a reaction is added to it, and that does not make it a new post (§7.1, §9.1).
        /// </remarks>
        void _onChangedRoot(const InboundMessage& root);
        /// <summary>Reads a session thread's newest replies and handles the owner's new ones, oldest first (§7.2, §7.5).</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its thread's root.</param>
        /// <exception cref="std::bad_alloc">If the replies cannot be read into memory.</exception>
        void _readThreadReplies(const std::wstring& sessionId, const std::wstring& rootId);
        /// <summary>
        /// One reply of the owner in a session's thread (§7.5): written while Agentmaster was closed = offered (§7.4); a plain
        /// text or a Claude Code command = a queued prompt; /steer, /enter, /interrupt, /deactivate = their actions; a refused
        /// command = why not.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its thread's root.</param>
        /// <param name="m">The reply.</param>
        /// <exception cref="std::bad_alloc">If the answer cannot be built.</exception>
        void _handleOwnerReply(const std::wstring& sessionId, const std::wstring& rootId, const InboundMessage& m);
        /// <summary>
        /// Queues the owner's words as a remote prompt (§7.5; Telegram's own fields): next in line before the runner's planned
        /// prompts, source "teams:&lt;reply id&gt;", the question guard's pass; a dormant tab is started; 👀 on the reply and its
        /// status followed. A bare /add-dir is answered with its usage instead (its path input could not be filled from here).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its thread's root.</param>
        /// <param name="replyId">The owner's reply; "" = the words are the root itself (§9.1: a new post that became the card),
        /// which gets no status emoji and no ❌ watch — on the card they would stay for good (§8) — and the source
        /// "teams:&lt;root id&gt;".</param>
        /// <param name="text">The words.</param>
        /// <exception cref="std::bad_alloc">If the prompt cannot be built.</exception>
        void _queueTeamsPrompt(const std::wstring& sessionId, const std::wstring& rootId, const std::wstring& replyId, const std::wstring& text);
        /// <summary>/steer (§7.5, MessagingSteer.h): the words sent NOW when the session can take them, else first in the queue.</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its thread's root.</param>
        /// <param name="replyId">The owner's reply.</param>
        /// <param name="words">The words after /steer.</param>
        /// <exception cref="std::bad_alloc">If the prompt cannot be built.</exception>
        void _steerTeams(const std::wstring& sessionId, const std::wstring& rootId, const std::wstring& replyId, const std::wstring& words);
        /// <summary>/interrupt (and M5's 🛑): ONE Esc, pressed by the hosting window, which answers what it did.</summary>
        /// <param name="sessionId">The session.</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _escTeams(const std::wstring& sessionId);
        /// <summary>/enter (and M5's ↩️): ONE Enter on the prompt waiting in the box, read back by the hosting window.</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="args">Words after /enter (any = the usage line: an Enter must not go with words that go nowhere).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _enterTeams(const std::wstring& sessionId, const std::wstring& args);
        /// <summary>/deactivate (and M5's 🌙): parks the tab at rest; during a turn it asks first (M5's confirm).</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="cutOffTurn">The owner confirmed: a turn in flight may be cut off.</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _parkTeams(const std::wstring& sessionId, bool cutOffTurn);
        /// <summary>Offers a reply written while Agentmaster was closed (§7.4): quoted, with ✅ / ❌, and watched (M5 acts on it).</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its thread's root.</param>
        /// <param name="m">The reply.</param>
        /// <param name="text">Its words.</param>
        /// <exception cref="std::bad_alloc">If the offer cannot be built.</exception>
        void _offerBacklog(const std::wstring& sessionId, const std::wstring& rootId, const InboundMessage& m, const std::wstring& text);
        /// <summary>A short answer in a session's thread (an Answer post in its lane).</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="html">The answer's Teams HTML.</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _answer(const std::wstring& sessionId, const std::wstring& html);
        /// <summary>
        /// Sets or removes a reaction of Agentmaster's (as the owner) on a message; journaled when it is queued, so the poll never
        /// reads it as the owner's action (§7.2).
        /// </summary>
        /// <param name="rootId">The thread's root.</param>
        /// <param name="replyId">The reply ("" = the root itself).</param>
        /// <param name="emoji">The emoji.</param>
        /// <param name="set">True to set, false to remove.</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _react(const std::wstring& rootId, const std::wstring& replyId, std::wstring_view emoji, bool set);
        /// <summary>Starts a prompt's status follow and shows its first emoji.</summary>
        /// <param name="track">The prompt (its `shown` is set here).</param>
        /// <param name="emoji">The first emoji.</param>
        /// <exception cref="std::bad_alloc">If the follow cannot be stored.</exception>
        void _trackPrompt(PromptTrack track, std::wstring_view emoji);
        /// <summary>The status follows' step (every threads' step): each prompt's emoji brought up to its queue status.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        void _trackStep(int64_t nowMs) noexcept;
        /// <summary>
        /// A session card's reactions (§8): compared with the last set read; each emoji the owner ADDED since (not Agentmaster's,
        /// not a status emoji) is consumed and acted on. One added while Agentmaster was closed is removed and not acted on.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="root">Its root as the delta gave it (with its reactions).</param>
        /// <exception cref="std::bad_alloc">If the reactions cannot be compared.</exception>
        void _rootReactions(const std::wstring& sessionId, const InboundMessage& root);
        /// <summary>The legend's reactions: kept for the next comparison; ➕ (a new session) comes with M6 and is removed.</summary>
        /// <param name="root">The legend as the delta gave it.</param>
        /// <exception cref="std::bad_alloc">If the reactions cannot be compared.</exception>
        void _legendReactions(const InboundMessage& root);
        /// <summary>
        /// The watched replies of a changed thread (§7.2: a confirm, a backlog offer, a queued prompt): each one read, its owner's new
        /// emoji acted on by its kind — once: the first meaningful emoji ends the watch.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its thread's root.</param>
        /// <exception cref="std::bad_alloc">If a reply cannot be read into memory.</exception>
        void _watchedStep(const std::wstring& sessionId, const std::wstring& rootId);
        /// <summary>What a watched reply's emoji does (the watch ends first, the owner's emoji is consumed).</summary>
        /// <param name="w">The watch.</param>
        /// <param name="a">The action its emoji means on its kind (ActionForReaction).</param>
        /// <param name="emoji">The owner's emoji.</param>
        /// <exception cref="std::bad_alloc">If the answer cannot be built.</exception>
        void _watchedAction(const Watched& w, Action a, const std::wstring& emoji);
        /// <summary>One emoji the owner put on a session's card (§8): consumed, then its action (a confirm first where one is due).</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="rootId">Its card (the thread's root).</param>
        /// <param name="emoji">The owner's emoji.</param>
        /// <exception cref="std::bad_alloc">If the action cannot be built.</exception>
        void _cardAction(const std::wstring& sessionId, const std::wstring& rootId, const std::wstring& emoji);
        /// <summary>Posts a confirm (ConfirmPostHtml) and watches it for 60 s from its answer (MessageKind::Confirm).</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="a">Action::Restart or Action::Park.</param>
        /// <exception cref="std::bad_alloc">If the confirm cannot be built.</exception>
        void _confirm(const std::wstring& sessionId, Action a);
        /// <summary>A confirmed restart: the existing cross-window seam, the turn kept as it is (KeepInterrupted: no question at the PC).</summary>
        /// <param name="sessionId">The session.</param>
        /// <exception cref="std::bad_alloc">If the answer cannot be built.</exception>
        void _restartTeams(const std::wstring& sessionId);
        /// <summary>☀️: a parked tab is started in place; any other session says what it needs instead.</summary>
        /// <param name="sessionId">The session.</param>
        /// <exception cref="std::bad_alloc">If the answer cannot be built.</exception>
        void _wakeTeams(const std::wstring& sessionId);
        /// <summary>
        /// 🖼️ and /screen: the terminal's styled snapshot from the hosting window, drawn as a PNG on a worker (ScreenImage, as
        /// Telegram's 🖼 Screen Image) and posted inline; a snapshot that cannot be read or drawn falls back to the screen's text.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _screenTeams(const std::wstring& sessionId);
        /// <summary>
        /// 📊 and /status: Claude Code's own /status facts read out of band on a worker (Telegram's report: the heartbeat, the
        /// transcript, the account, the plan usage — ClaudeStatus.h, MessagingStatus.h), posted as one or two replies.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _statusTeams(const std::wstring& sessionId);
        /// <summary>
        /// 📜 and /allscreen (§8; Telegram's §21.39): the WHOLE conversation drawn from the transcript as tall pictures
        /// (RenderConversationImages, on a worker), each part posted inline in the session's thread as soon as it is drawn,
        /// with a caption naming the level, the part and its turns; a conversation too long for the parts is told how much was
        /// left out and what draws more. No window is asked: a parked tab or a closed session draws too.
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="args">The words after /allscreen: a level, "diffs", "last" ("" = the default level, the cog's Messaging
        /// tab's — one setting for both connectors).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        /// <remarks>
        /// At most kAllScreenMaxRenders drawings run at once in this connector (a long transcript holds hundreds of MB while it is
        /// read and drawn), and one per session (a second press while its parts are drawn would post the conversation twice):
        /// both are answered in the thread. Parts are cut at kAllScreenPartHeightPx, so each stays a post of its own.
        /// </remarks>
        void _allScreenTeams(const std::wstring& sessionId, const std::wstring& args);
        /// <summary>
        /// Queues a reply that shows a PNG inline (BuildImagePostJson) in the session's lane; a picture past kImagePostMaxBytes is
        /// answered with words instead. Its answer journals the post. Any thread (a worker's).
        /// </summary>
        /// <param name="sessionId">The session (its thread must exist).</param>
        /// <param name="captionHtml">The caption (Teams HTML).</param>
        /// <param name="png">The picture.</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _postImage(const std::wstring& sessionId, const std::wstring& captionHtml, const std::string& png);
        /// <summary>
        /// A new post of the owner (§9.1, D-C9): its folder step is posted as a reply (the folders sessions worked in, numbered)
        /// and watched (MessageKind::Step, the WizardData in its payload, for an hour). Once per post in a run (_wizardRoots).
        /// Poll thread.
        /// </summary>
        /// <param name="root">The owner's new post.</param>
        /// <exception cref="std::bad_alloc">If the step cannot be built.</exception>
        void _startWizard(const InboundMessage& root);
        /// <summary>
        /// The folders a new session's step offers (§9.1): Telegram's picker list, the recent half
        /// (MessagingService::_buildNewThreadListsNow) — where the registry's sessions worked, the most recent first, the
        /// machine's temp folders left out, a folder gone since not offered. Poll thread.
        /// </summary>
        /// <returns>At most kWizardMaxFolders folders, the most recent first.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        /// <remarks>
        /// Each folder is checked by a BOUNDED probe (FolderProbe.h: 250 ms each, 1.5 s for the whole list, an answer up to a
        /// minute old taken as it is), because a drive mapped to a share or a sleeping disk can hold a check for the network's
        /// timeout; a UNC path is listed unchecked, and a folder that did not answer is listed too. The pick checks the folder
        /// again (_pickWizardFolder).
        /// </remarks>
        std::vector<std::wstring> _wizardFolders();
        /// <summary>
        /// A post whose folder step waits (§9.1): the owner's number emoji on the step picks a listed folder (consumed: removed
        /// again), or the owner's reply that is one folder's full path names one (Msg::NewThreadTypedFolder). Each reply is read
        /// once (_wizardRepliesMs). A deleted post ends its step. Poll thread.
        /// </summary>
        /// <param name="root">The owner's post as the delta gave it.</param>
        /// <exception cref="std::bad_alloc">If the step cannot be read into memory.</exception>
        void _wizardStep(const InboundMessage& root);
        /// <summary>
        /// The pick's check (§9.1, Telegram's §21.17.4): a FRESH bounded probe of the folder (kNewThreadPickProbeWaitMs). A folder
        /// that is not there, or that did not answer, is said under the post and the step stays for another pick; an existing
        /// folder launches.
        /// </summary>
        /// <param name="rootId">The owner's post.</param>
        /// <param name="stepId">The folder step.</param>
        /// <param name="data">The step's data.</param>
        /// <param name="folder">The folder picked or typed.</param>
        /// <param name="readToMs">The newest owner reply the step read (Unix ms; LaunchAsk::readToMs).</param>
        /// <exception cref="std::bad_alloc">If an answer or the request cannot be built.</exception>
        /// <remarks>Blocks the poll thread for up to kNewThreadPickProbeWaitMs (2 s) on an unreachable folder, as Telegram's pick does.</remarks>
        void _pickWizardFolder(const std::wstring& rootId, const std::wstring& stepId, const WizardData& data, const std::wstring& folder, int64_t readToMs);
        /// <summary>
        /// Asks a window for the new session (ServiceHooks::launchAnswered, NewSession in `folder`, a session id minted here),
        /// with the post's words to come as its first prompt. The step's watch ends first (a second pick finds nothing); the id is
        /// recorded as launching (_launching) BEFORE the ask, so the threads' step posts no root of its own for a session whose
        /// tab exists before its answer reaches the tick; "Opening a session in …" is posted under the post. Poll thread.
        /// </summary>
        /// <param name="rootId">The owner's post.</param>
        /// <param name="stepId">The folder step (its watch ends here).</param>
        /// <param name="data">The step's data.</param>
        /// <param name="folder">The folder (checked by _pickWizardFolder).</param>
        /// <param name="readToMs">The newest owner reply the step read (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        /// <remarks>
        /// A host without the answered launch (an older engine, a test harness), or one that takes no request (no window), is
        /// answered under the post at once; the post is then done: the owner writes a new one.
        /// </remarks>
        void _launchFromWizard(const std::wstring& rootId, const std::wstring& stepId, const WizardData& data, const std::wstring& folder, int64_t readToMs);
        /// <summary>
        /// The window's answer to a launch asked from Teams, a new session's or a fork's (RemoteLaunchReply: exactly once, ANY
        /// thread — the window's UI thread when it acted, maybe before the ask returned). It only queues the answer under a leaf
        /// lock (_launchMtx) and wakes the tick, so the window's thread never waits on _mtx (as MessagingService::_onLaunchAnswered).
        /// </summary>
        /// <param name="launch">What the ask knew.</param>
        /// <param name="outcome">The window's answer.</param>
        void _onLaunchAnswered(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome) noexcept;
        /// <summary>The tick's step for the queued launch answers (_onLaunchAnswered): each one settled by _settleLaunchAnswer.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        void _settleLaunchAnswers(int64_t nowMs) noexcept;
        /// <summary>
        /// One launch answer, on the tick thread. One asked in a session's thread (LaunchAsk::askedIn: a fork, /newsession) goes
        /// to _settleThreadAsk. A new session's for a post (§9.1):
        /// an open session gets the post as its thread — the card replaces the post's words at the next pass (they stay on it as
        /// the first prompt), no history to replay, replies older than the step's last read not read as prompts — and the words
        /// are queued as its first prompt (_queueTeamsPrompt, no status emoji); a refusal says why under the post
        /// (RemoteLaunchRefusalText), and the post is done.
        /// </summary>
        /// <param name="launch">What the ask knew.</param>
        /// <param name="outcome">The window's answer.</param>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the thread or an answer cannot be built.</exception>
        /// <remarks>
        /// A session that has a thread already (an answer that came after the threads' step made one — not possible while the id is
        /// in _launching, kept as a belt) keeps it: the post stays as it is, and the prompt goes into that thread's session.
        /// </remarks>
        void _settleLaunchAnswer(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome, int64_t nowMs);
        /// <summary>
        /// 🔀 and /forksession (§8): a fork of the session in its folder. A Claude Code source is assessed first
        /// (AssessForkContinuation, HOOKS.md "Interrupted turns": the source runs or stopped mid-turn, its turn died on an API
        /// error, or the owner interrupted it); when the fork would continue or wait at that turn, the question is asked
        /// (_askForkTeams), else the fork goes Uninterrupted at once (_launchForkTeams). A Codex source forks at once.
        /// </summary>
        /// <param name="sessionId">The source session.</param>
        /// <param name="model">A per-launch model (/forksession's argument; "" = the settings' model).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        /// <remarks>Reads settings.json (LoadAppSettings) and the source's transcript tail: the poll thread only, never a UI thread.</remarks>
        void _forkTeams(const std::wstring& sessionId, const std::wstring& model);
        /// <summary>
        /// Posts the fork question in the source's thread (ForkQuestionHtml) and watches it for an hour from its answer
        /// (MessageKind::ForkChoice, the ForkQuestionData in its payload): the owner's 1️⃣ or 2️⃣ forks (ForkChoiceFor), ❌ does not.
        /// </summary>
        /// <param name="sessionId">The source session.</param>
        /// <param name="title">The source's title.</param>
        /// <param name="data">What the answer forks (the situation, the folder, the title, the model).</param>
        /// <param name="facts">What the question says.</param>
        /// <exception cref="std::bad_alloc">If the question cannot be built.</exception>
        void _askForkTeams(const std::wstring& sessionId, const std::wstring& title, const ForkQuestionData& data, const ForkQuestionFacts& facts);
        /// <summary>
        /// Asks a window for the fork (ServiceHooks::launchAnswered, Fork of the source with the turn choice) after "🔀 Forking …"
        /// in the source's thread; the answer is settled on the tick (_settleThreadAsk). No window takes it: said at once.
        /// </summary>
        /// <param name="sourceId">The source session.</param>
        /// <param name="data">The folder, the title and the model.</param>
        /// <param name="choice">What the fork does about the source's unfinished turn (never Ask).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _launchForkTeams(const std::wstring& sourceId, const ForkQuestionData& data, ForkTurnChoice choice);
        /// <summary>
        /// /newsession [model] in a session's thread (Telegram's /newsession in a topic): a NEW session in that session's folder,
        /// with no first prompt (its own thread comes up: the owner writes there). The answer is said in the asking thread.
        /// </summary>
        /// <param name="sessionId">The session whose thread asked (its folder is the new session's).</param>
        /// <param name="model">A per-launch model ("" = the settings' model).</param>
        /// <exception cref="std::bad_alloc">If the request cannot be built.</exception>
        void _newSessionTeams(const std::wstring& sessionId, const std::wstring& model);
        /// <summary>
        /// Hands a launch to a window (ServiceHooks::launchAnswered) with the answer routed to _onLaunchAnswered (a weak
        /// reference: the answer may outlive the service). The answer comes exactly once for a request the engine took, from any
        /// thread, maybe before this returns; none comes for a request it refused.
        /// </summary>
        /// <param name="req">The request.</param>
        /// <param name="launch">What the answer's settling needs.</param>
        /// <returns>True when a window took the request; false when none did, the host has no answered launch, or it threw (logged).</returns>
        bool _askWindow(const RemoteLaunchRequest& req, const LaunchAsk& launch) noexcept;
        /// <summary>
        /// The answer to a launch asked in a session's thread (a fork, or /newsession's new session), on the tick thread: opened →
        /// the asking thread names the new tab, and it gets a thread of its own at the threads' next pass even outside Telegram's
        /// rule (_wantThread: the owner asked for it here); refused → why.
        /// </summary>
        /// <param name="launch">What the ask knew (askedIn: the asking session; kind: Fork or NewInFolder).</param>
        /// <param name="outcome">The window's answer.</param>
        /// <exception cref="std::bad_alloc">If an answer cannot be built.</exception>
        void _settleThreadAsk(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome);
        /// <summary>
        /// The 🔎 Search thread's root (§9.3), posted once when it is missing (with a search factory and a source that can post;
        /// a failed post is tried again a minute later); its id is kept in the state (ChannelState::searchId). Tick thread.
        /// </summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _searchRootStep(int64_t nowMs);
        /// <summary>
        /// A changed Search thread (poll thread): the owner's new replies under it, each read once (_searchReadToMs, from the
        /// run's start: a request written while Agentmaster was closed is not run), oldest first — a request goes to the agent,
        /// a number picks from the last answer, /clear starts the conversation over, /help shows the root's text again.
        /// </summary>
        /// <param name="root">The Search root as the delta gave it.</param>
        /// <exception cref="std::bad_alloc">If the replies cannot be read into memory.</exception>
        void _searchStep(const InboundMessage& root);
        /// <summary>
        /// The Search conversation's bridge, made on first use from the factory (outside _mtx: the factory may start the
        /// conversation's worker), its events routed to _onSearchEvent through a weak reference; kept until the stop.
        /// </summary>
        /// <returns>The bridge; null when no factory is set (search is not available in this host).</returns>
        /// <exception cref="std::bad_alloc">If the bridge cannot be made.</exception>
        /// <exception cref="std::system_error">If the conversation's worker cannot start (its first use in the process).</exception>
        std::shared_ptr<Msg::SearchBridge> _ensureSearch();
        /// <summary>
        /// The Search conversation's events (its threads, never a window's): an Answer is posted under the Search root as
        /// SearchAnswerHtml and its rows kept for the picks (_searchRows); a Failure says why; the rest are quiet.
        /// </summary>
        /// <param name="ev">The event.</param>
        void _onSearchEvent(const Msg::SearchBridgeEvent& ev) noexcept;
        /// <summary>
        /// A number replied in the Search thread: that row of the last answer. An open session is named with a link to its
        /// thread (or gets one, _wantThread); a closed one is resumed into a tab through the answered launch (LaunchAsk kind
        /// SearchPick), whose answer is said in the Search thread.
        /// </summary>
        /// <param name="rootId">The Search root.</param>
        /// <param name="pick">The number, 1-based.</param>
        /// <exception cref="std::bad_alloc">If an answer or the request cannot be built.</exception>
        void _searchPick(const std::wstring& rootId, int pick);
        /// <summary>A resume's answer from a Search pick, on the tick thread: opened → named, its thread wanted; refused → why.</summary>
        /// <param name="launch">What the ask knew (rootId: the Search root).</param>
        /// <param name="outcome">The window's answer.</param>
        /// <exception cref="std::bad_alloc">If an answer cannot be built.</exception>
        void _settleSearchPick(const LaunchAsk& launch, const RemoteLaunchOutcome& outcome);
        /// <summary>
        /// A reply under a post that is no session's thread (a new post's steps, the legend): its own lane ("post:&lt;root&gt;", so
        /// its replies keep their order), fingerprinted when queued and journaled by its answer (the poll never reads it as the
        /// owner's).
        /// </summary>
        /// <param name="rootId">The post.</param>
        /// <param name="html">The reply's Teams HTML.</param>
        /// <param name="onDone">Called with the answer, on the send thread with no service lock held (may be empty).</param>
        /// <exception cref="std::bad_alloc">If the post cannot be built.</exception>
        void _postToRoot(const std::wstring& rootId, const std::wstring& html, std::function<void(const GraphResult&)> onDone);
        /// <summary>❌ on a queued prompt: taken out of the queue while it is still Pending; the answer says which happened.</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="promptId">The prompt.</param>
        /// <exception cref="std::bad_alloc">If the answer cannot be built.</exception>
        void _unqueueTeams(const std::wstring& sessionId, const std::wstring& promptId);
        /// <summary>
        /// The mirror's question for one prompt it is about to post: did the owner write it in the thread? A match is consumed (a
        /// prompt typed again at the PC with the same words is posted).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="text">The prompt's text as the transcript holds it.</param>
        /// <returns>True when it came from Teams.</returns>
        bool _consumeTeamsPrompt(const std::wstring& sessionId, const std::wstring& text) noexcept;
        /// <summary>The fleet board's lines (every live Claude tab, the most recent first, at most kLegendFleetMax).</summary>
        /// <param name="more">Receives how many sessions were left out.</param>
        /// <returns>The lines.</returns>
        /// <exception cref="std::bad_alloc">If the lines cannot be allocated.</exception>
        std::vector<FleetLine> _fleetLines(size_t& more);
        /// <summary>Prunes the journal hourly and saves the state when it changed.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        void _housekeeping(int64_t nowMs) noexcept;
        /// <summary>Queues a Graph write (its `seq` is set here) and wakes the send thread.</summary>
        /// <param name="item">The write.</param>
        /// <exception cref="std::bad_alloc">If the queue cannot grow.</exception>
        void _enqueue(OutboxItem item);
        /// <summary>Puts a write back into the outbox after a failed attempt, keeping its place in its priority.</summary>
        /// <param name="item">The write.</param>
        /// <param name="notBeforeMs">Not before this time (Unix ms; 0 = at once).</param>
        /// <exception cref="std::bad_alloc">If the queue cannot grow.</exception>
        void _requeue(OutboxItem item, int64_t notBeforeMs);
        /// <summary>Sends one write: routes it to a token, makes the call, and handles 401, 403, 429 and retryable failures.</summary>
        /// <param name="item">The write.</param>
        void _sendOne(OutboxItem item) noexcept;
        /// <summary>The (source, kind of call) pairs a 403 keeps out of the router now; expired ones are dropped.</summary>
        /// <param name="nowMs">The clock (Unix ms).</param>
        /// <returns>The pairs.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        std::vector<RefusedPair> _refusedNow(int64_t nowMs);
        /// <summary>Loads the channel's state file (or its previous copy) at a start; a file it cannot use is kept aside before
        /// the first save replaces it.</summary>
        /// <exception cref="std::bad_alloc">If the state cannot be allocated.</exception>
        void _loadState();
        /// <summary>Saves the state when it changed, at most once per 2 s unless forced (one writer at a time).</summary>
        /// <param name="force">Save now even within the gap (a stop, the app closing).</param>
        /// <exception cref="std::bad_alloc">If the document cannot be built.</exception>
        void _saveState(bool force);
        /// <summary>Records the newest problem for the status line, and logs it when it changed ("" clears it).</summary>
        /// <param name="problem">The problem, one line.</param>
        void _noteProblem(std::wstring problem) noexcept;
        /// <summary>Wakes the tick thread.</summary>
        void _wake() noexcept;
        /// <summary>Appends a `[teams]` line to hooks.log.</summary>
        /// <param name="line">The line.</param>
        void _log(const std::wstring& line) const noexcept;

        /// <summary>The fleet.</summary>
        std::shared_ptr<SessionRegistry> _registry;
        /// <summary>The runner (M4).</summary>
        std::shared_ptr<Scheduler> _scheduler;
        /// <summary>The active profile.</summary>
        std::wstring _profileDir;
        /// <summary>The window-hosted seams (M5, M6).</summary>
        Msg::ServiceHooks _hooks;
        /// <summary>The app's token source (lives as long as the service; Configure follows the settings).</summary>
        AppSource _app;
        /// <summary>The Azure CLI's token source (a shared_ptr: its detached run holds it).</summary>
        std::shared_ptr<AzureCliSource> _cli;

        /// <summary>Serializes Apply and Stop (taken before `_mtx`, never while holding it).</summary>
        std::mutex _applyMtx;
        /// <summary>One writer of the state file at a time (taken before `_mtx`).</summary>
        std::mutex _saveMtx;
        /// <summary>Guards every member below.</summary>
        mutable std::mutex _mtx;
        /// <summary>Wakes the tick and send loops (on `_mtx`).</summary>
        std::condition_variable _cv;

        /// <summary>The config the service runs with (or would run with).</summary>
        TeamsConfig _config;
        /// <summary>The tab-color mode (the board's color squares), from the last Apply.</summary>
        TabColorMode _colorMode{ TabColorMode::WorkingDirectory };
        /// <summary>The threads run.</summary>
        bool _running{ false };
        /// <summary>The stop is under way: the loops end.</summary>
        bool _stopping{ false };
        /// <summary>The tick thread was woken (a sign-in wanted, a setting changed).</summary>
        bool _tickWake{ false };
        /// <summary>The tick thread.</summary>
        std::thread _tick;
        /// <summary>The send thread.</summary>
        std::thread _send;
        /// <summary>This start's client (Shutdown at the stop is final, so each start makes a new one).</summary>
        std::shared_ptr<GraphClient> _http;

        /// <summary>The channel's state.</summary>
        ChannelState _state;
        /// <summary>Its file (&lt;profile&gt;\messaging\teams\state-&lt;8 hex&gt;.json).</summary>
        std::wstring _stateFile;
        /// <summary>The state changed since the last save.</summary>
        bool _stateDirty{ false };
        /// <summary>The file was read (or written) fine in this run: only then is it copied to ".prev" before a save.</summary>
        bool _stateFileReadable{ false };
        /// <summary>When the state was last saved (Unix ms).</summary>
        int64_t _lastSaveMs{ 0 };
        /// <summary>When ".prev" was last refreshed (Unix ms).</summary>
        int64_t _lastPrevCopyMs{ 0 };
        /// <summary>When the journal was last pruned (Unix ms).</summary>
        int64_t _lastPruneMs{ 0 };

        /// <summary>The outbox (small: a linear scan picks the next).</summary>
        std::vector<OutboxItem> _outbox;
        /// <summary>The last arrival number.</summary>
        uint64_t _seq{ 0 };
        /// <summary>No write goes out before this time (Unix ms): the send gap, or a 429's wait.</summary>
        int64_t _nextSendMs{ 0 };
        /// <summary>The pairs a 403 refused, each with the time it may be tried again (Unix ms).</summary>
        std::vector<std::pair<RefusedPair, int64_t>> _refused;

        /// <summary>The owner (the app's token's user, else the CLI's).</summary>
        Owner _owner;
        /// <summary>Which source makes each kind of call ("the app", "the Azure CLI", "" = none), for the status line.</summary>
        std::wstring _postsBy;
        /// <summary>See _postsBy.</summary>
        std::wstring _editsBy;
        /// <summary>See _postsBy.</summary>
        std::wstring _readsBy;
        /// <summary>See _postsBy.</summary>
        std::wstring _reactsBy;
        /// <summary>Why the Azure CLI's token is left out ("" = it is not).</summary>
        std::wstring _cliProblem;
        /// <summary>The newest problem ("" = none).</summary>
        std::wstring _problem;
        /// <summary>The owner asked for a new sign-in code (the cog's Sign in).</summary>
        bool _signInWanted{ false };
        /// <summary>This Apply's automatic sign-in was decided (one code per Apply at most).</summary>
        bool _autoSignInTried{ false };

        /// <summary>The legend post is in the outbox.</summary>
        bool _legendPosting{ false };
        /// <summary>A failed legend post is tried again after this time (Unix ms).</summary>
        int64_t _legendRetryAtMs{ 0 };
        /// <summary>A legend edit is in the outbox.</summary>
        bool _legendEditQueued{ false };
        /// <summary>The board the legend shows (CardHash of its body without the clock; 0 = unknown: the first pass edits once).</summary>
        uint64_t _legendBoardHash{ 0 };
        /// <summary>When the legend was last posted or edited (Unix ms).</summary>
        int64_t _lastLegendEditMs{ 0 };
        /// <summary>When the board was last built (Unix ms; it is built at most every 5 s).</summary>
        int64_t _lastLegendBuildMs{ 0 };

        /// <summary>Each thread's runtime, by session id (cleared at every start).</summary>
        std::map<std::wstring, ThreadRuntime> _runtime;
        /// <summary>When the threads' step last ran (Unix ms).</summary>
        int64_t _lastThreadsStepMs{ 0 };
        /// <summary>The idle grace a session gets a thread within (AppSettings::messagingIdleGraceMinutes, shared with Telegram).</summary>
        int64_t _idleGraceMs{ 10LL * 60 * 1000 };
        /// <summary>The level a bare /allscreen or 📜 draws (AppSettings::messagingAllScreenLevel, shared with Telegram). Under _mtx.</summary>
        TurnsLevel _allScreenLevel{ Msg::kAllScreenDefaultLevel };
        /// <summary>The sessions whose conversation pictures are being drawn (one drawing per session at a time). Under _mtx.</summary>
        std::set<std::wstring> _allScreenSessions;
        /// <summary>How many conversations are being drawn now (at most kAllScreenMaxRenders); a worker counts itself out.</summary>
        std::atomic<int> _allScreenRenders{ 0 };
        /// <summary>The folder colors (dir-colors.json), read again at most every 30 s. The tick thread's only.</summary>
        DirColorIndex _dirColors;
        /// <summary>When `_dirColors` was read (Unix ms). The tick thread's only.</summary>
        int64_t _dirColorsAtMs{ 0 };

        /// <summary>The poll thread.</summary>
        std::thread _poll;
        /// <summary>
        /// When this run started (Unix ms): a reply older than it was written while Agentmaster was closed (§7.4), a post older
        /// than it is history (§9.1), and a launch answer is known as this run's by it (LaunchAsk::runMs).
        /// </summary>
        int64_t _startedMs{ 0 };
        /// <summary>When the owner last acted in Teams (a reply, a reaction; Unix ms): the poll runs every 5 s for 2 minutes.</summary>
        int64_t _lastOwnerActionMs{ 0 };
        /// <summary>When the channel last changed (Unix ms): an hour without a change slows the poll to 90 s.</summary>
        int64_t _lastChangeMs{ 0 };
        /// <summary>A session waits for the owner (Waiting for you, Needs approval): the poll runs every 5 s.</summary>
        bool _ownerAwaited{ false };
        /// <summary>When the last GET went out (Unix ms; reads keep kMinReadGapMs apart).</summary>
        int64_t _lastReadMs{ 0 };
        /// <summary>The prompts from Teams whose status is shown on the owner's replies.</summary>
        std::vector<PromptTrack> _tracks;
        /// <summary>Prompts the owner wrote in a thread, by session: their compare form and when (the mirror posts them no line).</summary>
        std::map<std::wstring, std::vector<std::pair<std::wstring, int64_t>>> _teamsPrompts;
        /// <summary>Agentmaster's recent posts' fingerprints by thread root ("" = roots), with when they were queued.</summary>
        std::map<std::wstring, std::vector<std::pair<uint64_t, int64_t>>> _ownFingerprints;
        /// <summary>The tick loaded this run's state: the poll waits for it (it reads the delta link and the threads).</summary>
        bool _stateLoaded{ false };
        /// <summary>When an Esc was last asked for each session (Unix ms): a second within 3 s is refused (two Esc open Rewind).</summary>
        std::map<std::wstring, int64_t> _lastEscMs;
        /// <summary>
        /// The owner's posts this run started a new session for (§9.1): the post changes again with every reply under it (the
        /// step, "Opening…", a refusal) and must never start a second one. Kept for the run — a refused launch or an expired step
        /// ends that post's chance (the owner writes a new post) — except a step whose post failed, which the post's next change
        /// asks again. A restart knows a waiting step by its watch.
        /// </summary>
        std::set<std::wstring> _wizardRoots;
        /// <summary>The newest owner reply each waiting step read (by post; Unix ms, the server's clock): a reply is read once.</summary>
        std::map<std::wstring, int64_t> _wizardRepliesMs;
        /// <summary>
        /// The new sessions asked of a window whose answer has not been settled (by session id): the threads' step posts no root
        /// for them — their post becomes their thread (§9.1).
        /// </summary>
        std::set<std::wstring> _launching;
        /// <summary>Guards _launchAnswers only: a leaf lock a window's thread takes (never _mtx; no other lock is taken under it).</summary>
        std::mutex _launchMtx;
        /// <summary>The windows' answers to the launches asked from Teams (new sessions, forks), waiting for the tick (_settleLaunchAnswers).</summary>
        std::vector<std::pair<LaunchAsk, RemoteLaunchOutcome>> _launchAnswers;
        /// <summary>An answer waits in _launchAnswers: set by a window's thread without _mtx; the tick's wait reads it and ends.</summary>
        std::atomic<bool> _launchAnswersPending{ false };
        /// <summary>
        /// Sessions the owner opened from Teams (a fork, §8 🔀) that get a thread at the threads' next pass even outside Telegram's
        /// rule (QualifiesForThread); an id leaves once its thread exists. Under _mtx; cleared by a new run.
        /// </summary>
        std::set<std::wstring> _wantThread;
        /// <summary>The Search agent's factory (SetSearchFactory; empty = no search here). Under _mtx.</summary>
        Msg::SearchBridgeFactory _searchFactory;
        /// <summary>
        /// The bridge to the connector's Search conversation, made on first use (_ensureSearch) and dropped at the stop (the
        /// conversation itself is the process's and goes on). Shared, so a caller copies it under _mtx and asks outside it.
        /// </summary>
        std::shared_ptr<Msg::SearchBridge> _search;
        /// <summary>The thinking level word of the Search conversation (AppSettings::messagingSearchThinking, shared with Telegram). Under _mtx.</summary>
        std::wstring _searchThinking{ L"high" };
        /// <summary>The last Search answer's rows, for the picks by number (a restart forgets them: ask again). Under _mtx.</summary>
        std::vector<SearchListing> _searchRows;
        /// <summary>The newest owner reply in the Search thread already read (Unix ms; the run's start at first). Under _mtx.</summary>
        int64_t _searchReadToMs{ 0 };
        /// <summary>The Search root's post is in flight (it is posted once). Under _mtx.</summary>
        bool _searchPosting{ false };
        /// <summary>A failed Search root post is tried again from this time on (Unix ms). Under _mtx.</summary>
        int64_t _searchRetryAtMs{ 0 };
    };
}
