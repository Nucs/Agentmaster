// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionRegistry: the single source of truth for all Claude Code
// sessions (DESIGN §6). The UI (M6) binds to it; the hooks bridge (HooksBridge) feeds it
// authoritative state; the Autorunner scheduler (M7) reacts to it.
//
// Pure C++ + the STL (no WinRT) so it is unit-testable standalone. The seam to the live
// terminal is an `Injector` callback (bound to the session's ConptyConnection by the app
// layer), which keeps WinRT out of the registry while still letting the scheduler write
// stdin (Correctness Rule #3: bind queue -> sessionId, never "the selected session").

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DraftCarry.h" // PasteJournalEntry + the journal caps (PENDING_INPUT.md §2c — the paste journal)
#include "HookEvents.h"
#include "SessionModels.h"

namespace Agentmaster
{
    // Notified after a hook mutates a session. Receives a snapshot (safe to read off any
    // thread) and the event that caused it. Invoked OUTSIDE the registry lock.
    using RegistryObserver = std::function<void(const SessionInfo& snapshot, HookEvent cause)>;

    // The Autorunner seam (M7): invoked when a clean `Stop` moves a session into
    // WaitingForInput. The handler decides whether to dequeue + inject the next prompt.
    using AdvanceHandler = std::function<void(const std::wstring& sessionId)>;

    // The adoption seam: invoked once, OUTSIDE the lock, when OnHookEvent creates a record
    // for a session that was NOT pre-registered (i.e. a hand-typed `claude`, not a Manager
    // Launch). The app layer uses `tabToken` (the hosting connection's WT_SESSION) to find
    // that ConPTY and bind a stdin injector, promoting the session to full observe+control.
    using AdoptionHandler = std::function<void(const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& tabToken)>;

    // Per-session stdin writer, bound to that session's ConptyConnection by the app layer.
    using Injector = std::function<void(const std::wstring& text)>;

    // Agentmaster (PENDING_INPUT.md §9 — the DRAFT SWAP): the hosting window's hook for SENDING a
    // queued prompt, as opposed to the raw stdin Injector above. Registered next to SetInjector by
    // the ONE window that owns the session's control, because everything the swap needs — reading
    // the live input box, blocking the user's keystrokes while it works — is UI-thread + this-window
    // only. Returns "accepted for delivery" SYNCHRONOUSLY (the swap itself is asynchronous): false
    // means nothing was or will be sent, and the caller performs its usual Rule-#4 rollback right
    // away; true means the window owns the outcome and will roll the prompt back ITSELF
    // (RollbackPromptToPending) if the swap later aborts. With no submitter bound, SubmitPrompt
    // falls back to the historical raw inject, so a session in a window that never registered one
    // behaves exactly as it always did.
    using PromptSubmitter = std::function<bool(const PromptSubmission& submission)>;

    // Agentmaster (DELIVERY_PLAN.md R8 — the VERIFIED PRESSER): the hosting window's hook for the
    // Enter-retry watchdog's lone-Enter press. Where the raw Inject("\r") trusted the scan-stale
    // draft guard alone, the presser reads the LIVE box at press time (fire-and-forget onto its UI
    // thread) and presses only into a verified-Empty box or one holding the watched prompt — a
    // Foreign/NoBox/MenuOpen read refuses and refreshes the registry facts instead. Registered in
    // lockstep with the injector/submitter by the one window that owns the control.
    using EnterPresser = std::function<void(const std::wstring& sessionId, const std::wstring& promptId, const std::wstring& promptText)>;

    // Opaque handles returned by AddObserver / AddAdoptionHandler; pass the same value to the
    // matching Remove* to detach. Agentmaster M9: the engine is a process-wide singleton shared
    // by every window's Manager lens, so a closing window MUST drop its observer (it captures a
    // strong DispatcherQueue) and its adoption handler, or they accumulate / dangle across the
    // shared registry. Tokens are monotonic and never reused, so removing a stale one is a no-op.
    using ObserverToken = uint64_t;
    using AdoptionToken = uint64_t;

    // Agentmaster (RETIRED HOSTS — SessionRegistry::RetireHostToken): how long a retired host token keeps
    // vetoing its session's observations and hooks. A retired token names a ConPTY WE closed (Deactivate and
    // Restart swap a fresh connection into the tab and close the old one), so the only thing that can still
    // speak for it is the claude that ConPTY hosted, while it handles its close: Windows ends a CTRL_CLOSE
    // handler after ~5 s, and a hook forwarder it already spawned can deliver even later (a SessionEnd
    // arrived 29 s after the 2026-08-26 park, under the old shell-form hooks). Ten minutes covers both with
    // a wide margin. A WT_SESSION GUID is never reused, so outliving the dying process costs only memory,
    // which kRetiredHostTokensPerSession bounds.
    inline constexpr int64_t kRetiredHostTokenTtlMs = 10 * 60 * 1000;
    // Agentmaster (RETIRED HOSTS): at most this many retired host tokens are remembered per session, oldest
    // dropped first. Each park/restart retires one; a handful covers rapid repeats on one tab.
    inline constexpr size_t kRetiredHostTokensPerSession = 8;

    // Agentmaster (UI_THREAD.md §10a #4 — the lean registry reads): the three facts that tie one LIVE session
    // to the tab that hosts it, copied WITHOUT the rest of the record. The tab reconcile
    // (TerminalPage::_ReconcileClaudeTabsImpl) runs every ~2.5 s in every window and used to take a full
    // SnapshotLive for exactly these three fields: a deep copy of every live session with its prompt-queue
    // history and transcript texts, ~9 ms/s of the release UI thread at 555 tabs (measured 2026-10-04,
    // UI_THREAD.md §10e). Returned by SessionRegistry::LiveTabBindings.
    struct SessionTabBinding
    {
        std::wstring id; // the session id (the registry key)
        std::wstring workingDir; // the launch working directory — the adopt path's fallback cwd
        std::wstring tabToken; // the hosting ConPTY's WT_SESSION as stored (any case); EMPTY until a hook or the Fleet Observer reveals it
    };

    // Agentmaster (tags perf, 2026-10-10 — one more lean registry read): what a LIST of sessions shows
    // about one of them — its name, its state dot and how recently it was active — copied WITHOUT the rest
    // of the record. The bookmark-tag surfaces need exactly this for the handful of sessions that carry a
    // tag: the Tags panel ranks each tag by its carriers' activity, and the tag hover panel lists a tag's
    // carriers with their status dot. The panel used to take a full Snapshot() for it on every open and
    // after every toggle — a deep copy of every record the profile ever held with its whole prompt-queue
    // history (the release profile on 2026-10-10: 1,614 records, 7,234 queued prompts, 7.1 million
    // characters of prompt text), with the registry mutex held for the length of the copy. Returned by
    // SessionRegistry::HeadlinesOf.
    struct SessionHeadline
    {
        std::wstring id; // the session id (the registry key), as stored
        std::wstring title; // SessionInfo::title — the one name the tab, the tree and the persisted record share (Rule #11); may be empty
        SessionState state{ SessionState::Idle }; // the triage state (meaningful for a live session; an archived one keeps its last)
        bool live{ false }; // has an open tab this run (SessionInfo::live); false = closed, resumable from the Sessions browser
        int64_t lastActivityUnixMs{ 0 }; // max(lastActivityUnixMs, convLastActivityUnixMs): the newer of the hook-side stamp and the transcript-derived one; 0 = no recorded activity
    };

    class SessionRegistry
    {
    public:
        SessionRegistry() = default;
        SessionRegistry(const SessionRegistry&) = delete;
        SessionRegistry& operator=(const SessionRegistry&) = delete;

        // Add a new session or replace an existing one with the same id. Returns the id.
        // Agentmaster (the STATE SETTLE hold — DELIVERY.md §16): the registry OWNS the settle clocks —
        // the incoming stateChangedAtUnixMs is ignored (a NEW record is stamped now; a replaced one keeps
        // its clock unless this upsert changed its state / live / started), and every queued prompt still
        // lacking a queuedAtUnixMs is stamped now (StampSettleClock / StampNewlyQueuedPrompts).
        std::wstring Upsert(SessionInfo info);
        void Remove(const std::wstring& id);

        bool Contains(const std::wstring& id) const;
        std::optional<SessionInfo> Get(const std::wstring& id) const;
        std::vector<SessionInfo> Snapshot() const; // ordered by insertion
        // Agentmaster (perf): the LIVE subset only (SessionInfo::live), same insertion order. The
        // registry holds every session EVER seen this profile (800+ archived records, each with its
        // queue history), while the Manager lens, the per-tick tab reconcile, the dormant count and
        // the keep-awake probe only ever look at the live handful — so the full Snapshot's deep copy
        // (every string of every archived record + every QueuedPrompt) was pure waste on the UI
        // thread, several times per refresh. Callers that need archived records (persistence, the
        // Sessions browser, restore) keep using Snapshot().
        std::vector<SessionInfo> SnapshotLive() const;

        // --- Agentmaster (UI_THREAD.md §10a #4): the LEAN reads ---------------------------------------------
        // Get, Snapshot and SnapshotLive deep-copy whole records: every prompt of the queue history (up to
        // kMaxQueueHistoryEntries per session) and every transcript text. On the 555-tab release those copies
        // cost ~48 ms/s (UI thread ~15, SessionScanner ~28, ProcessObserver ~5; UI_THREAD.md §10e), while the
        // periodic paths that made them read a handful of fields. A periodic path should use the narrowest
        // read below that still holds everything it reads. All are thread-safe and copy under the lock, so
        // each costs what it returns.

        // One record WITHOUT its prompt queue: every other field is copied as Get copies it, and `queue`
        // comes back EMPTY. For a reader that never looks at the queue (the cache-warm and inferred-color
        // sweeps), it skips the most expensive part of the copy.
        // ⚠ An empty queue here means "not copied", never "nothing queued": a caller that reads the queue, or
        // anything derived from it (a pending count, the next prompt), must use Get.
        //   id      — the session id.
        //   returns — the record minus its queue, or nullopt for an unknown id.
        //   throws  — std::bad_alloc on allocation failure; the stored record is left intact.
        std::optional<SessionInfo> GetWithoutQueue(const std::wstring& id) const;

        // The session's host token (SessionInfo::tabToken) alone — the one field the ~2.5 s liveness sweep
        // reads per tab, without a copy of the record around it.
        //   id      — the session id.
        //   returns — the WT_SESSION as stored (any case). EMPTY both for an unknown id and for a session that
        //             no hook or observation has tied to a ConPTY yet; use Contains to tell the two apart.
        //   throws  — std::bad_alloc on allocation failure only.
        std::wstring GetTabToken(const std::wstring& id) const;

        // One SessionTabBinding per LIVE session, in insertion order (the SnapshotLive set and order). A live
        // session that has no host token yet is included with an empty tabToken, so the size is the live count.
        //   returns — the bindings; empty when nothing is live.
        //   throws  — std::bad_alloc on allocation failure only.
        std::vector<SessionTabBinding> LiveTabBindings() const;

        // The host tokens of the MANAGED Codex sessions — live, launched by us (not external), with a token —
        // in insertion order. The Fleet Observer leaves these codex processes out of its observe-only External
        // census, because each already has a managed card; it used to take a FULL Snapshot() of every record
        // (the archived ones and every queue included) on each 2 s survey to collect exactly this.
        //   returns — the tokens as stored (any case); empty when no managed Codex session is live.
        //   throws  — std::bad_alloc on allocation failure only.
        std::vector<std::wstring> ManagedCodexTabTokens() const;

        // The headline of each asked session (SessionHeadline: id, title, state, live, newest activity) —
        // live AND archived records alike, since a closed session can still carry a bookmark tag. For a
        // reader that shows a few named sessions in a list (the Tags panel's activity ranking, the tag
        // hover panel's carrier rows): it copies five small fields per asked session, where Snapshot()
        // copied every record of the registry with its queue.
        // ⚠ Not for a caller that needs every session: it costs one map lookup per asked id, so ask for
        // the ids you show.
        //   ids     — the session ids to look up, in any order. An id the registry does not know is left
        //             out of the result; an id asked twice is answered twice.
        //   returns — one headline per KNOWN asked id, in the order asked; empty when none is known.
        //   throws  — std::bad_alloc on allocation failure only.
        std::vector<SessionHeadline> HeadlinesOf(const std::vector<std::wstring>& ids) const;

        // The SessionScanner's per-pass snapshot: the LIVE sessions (the SnapshotLive set and order), each
        // copied without two parts the scanner never reads:
        //   - The queue keeps only its OPEN prompts — Pending, the legacy Held, and Sent but not yet echoed —
        //     complete and in their order. The completed history (echoed Sent prompts, Typed captures, Skipped,
        //     Failed) is not copied. The scanner reads the queue to find an unacknowledged delivery
        //     (ScanNeedsFreshTail, the lost-send pre-filter) and a waiting prompt (ShouldWarnOnBoxNotVisible),
        //     and it re-reads the full record with Get before it changes a prompt.
        //   - lastAssistantText, recap, pendingInput and pendingPasteRefs come back EMPTY. The scanner writes the
        //     first two into the live record (UpdateQuiet) and never reads any of the four from its snapshot.
        // Before this, the full copy was ~79% of the scanner thread's time on the release (~28 ms/s at 555 tabs),
        // against ~3 ms/s for its own reconcile work (UI_THREAD.md §10e).
        // ⚠ Contract: a reader of this snapshot may look at the queue only for open prompts, and must never read
        // the four emptied texts from it. Anything else needs Get.
        //   returns — the lean live records, insertion order.
        //   throws  — std::bad_alloc on allocation failure; the stored records are left intact.
        std::vector<SessionInfo> SnapshotLiveForScan() const;

        // The side panel's card-list snapshot (SIDE_PANEL.md §2c): the LIVE sessions (the SnapshotLive set and
        // order), each copied WITHOUT the parts a card never shows:
        //   - `queue` comes back EMPTY (not copied) — a card shows no queue count.
        //   - lastAssistantText, recap and pendingPasteRefs come back EMPTY — the transcript texts can be
        //     kilobytes per session, and a card shows none of them.
        //   - pendingInput IS copied: a card shows the unsent-draft dots when it is non-empty.
        // The panel builds every card's model from this on each list update (up to twice a second while the
        // fleet changes, and every ~30 s for the timing texts), where SnapshotLive copied every queued prompt
        // and transcript text of every live session.
        // ⚠ Contract: a reader must never read the queue or the three emptied texts from this snapshot — an
        // empty value there means "not copied". Anything else needs Get.
        //   returns — the lean live records, insertion order.
        //   throws  — std::bad_alloc on allocation failure; the stored records are left intact.
        std::vector<SessionInfo> SnapshotLiveForCards() const;

        size_t Count() const;

        // The single entry point that applies an authoritative hook event and mutates
        // session state (DESIGN §7-8). Thread-safe. Fires the observer (always) and the
        // advance handler (only on a clean Stop -> WaitingForInput), both outside the lock.
        void OnHookEvent(const HookMessage& msg);

        // Fleet Observer (OBSERVER.md §9): the PULL upsert. Merge process facts the S-lane read
        // out-of-band (PEB cwd / cmdline / env) into the session record — provenance-aware: it
        // ENRICHES (pid / liveCwd / model / effort / permissionMode / background / runningApp /
        // amSession / tabToken / live) but NEVER sets SessionState (push hooks + the transcript
        // tail own state, Rule #1/#7). On first sight of a claude we did not Launch it creates an
        // `external` record and fires the adoption handlers (so the app binds its ConPTY by
        // tabToken, same promotion as a hook SessionStart); thereafter it idempotently merges, and
        // a steady-state re-observe with unchanged facts is a NO-OP (no observer / persist / UI
        // churn). No-op for an empty sessionId (a correlated-but-never-prompted claude has no
        // conversation id yet — §11d). Thread-safe.
        // Agentmaster (RETIRED HOSTS): an observation whose (sessionId, tabToken) pair is retired
        // (RetireHostToken) is DROPPED whole — it describes the dying claude of a connection we replaced.
        void ObserveClaude(const ObservedClaude& o);

        // Agentmaster (RETIRED HOSTS — the deactivate/restart unbind race): record that the ConPTY
        // `tabToken` no longer hosts `sessionId` because WE replaced it: Deactivate parks a fresh, never-
        // started connection in the tab, Restart starts one. The claude the old ConPTY hosted keeps running
        // for a few seconds while it handles the close, and until the window republishes its tab roster (its
        // next ~2.5 s probe) the Fleet Observer still correlates that dying process to this session BY THE
        // OLD TOKEN. Its ObserveClaude used to stamp tabToken / pid / presenceStatus back onto the record, so
        // the liveness sweep found no pane holding that token and ARCHIVED the session — the 2026-08-26
        // Deactivate that turned six tabs into plain shells, and 4 of the 8 release restarts — and its
        // SessionEnd, no longer recognized as stale, marked a parked tab Done. From this call on, for
        // kRetiredHostTokenTtlMs, every ObserveClaude and every hook naming this (session, token) pair is
        // dropped whole and logged ([retired-host] in hooks.log). Scoped to the session: the same token on
        // ANOTHER session is untouched. Call it BEFORE re-stamping the record's tabToken to the new
        // connection; re-retiring a token refreshes its clock. A no-op for an empty id or token. Thread-safe.
        //   sessionId — the session whose host connection was replaced.
        //   tabToken  — the replaced connection's WT_SESSION (any case — matched with TabTokenEq).
        //   throws    — std::bad_alloc on allocation failure only.
        void RetireHostToken(const std::wstring& sessionId, const std::wstring& tabToken);

        // Agentmaster (RETIRED HOSTS): is `tabToken` a host of `sessionId` retired by RetireHostToken, and is
        // that retirement still within kRetiredHostTokenTtlMs? Case-insensitive token match. Thread-safe.
        //   sessionId — the session to ask about.
        //   tabToken  — a WT_SESSION as a hook or an observation reported it ("" is never retired).
        //   returns   — true when observations and hooks from that host must be ignored for that session.
        //   throws    — nothing beyond std::mutex::lock's std::system_error.
        bool IsRetiredHostToken(const std::wstring& sessionId, const std::wstring& tabToken) const;

        // Mutate a session's Auto Testing / autorunner under the lock (used by the scheduler
        // and UI). The mutator runs while holding the lock; the post-change snapshot is
        // delivered to the observer afterwards. Returns false if the id is unknown.
        // Agentmaster (the STATE SETTLE hold — DELIVERY.md §16): after the mutator runs, the registry
        // restarts the session's settle clock if the mutator changed its state / live / started, and
        // stamps queuedAtUnixMs on any prompt the mutator queued — callers never stamp either.
        bool Update(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate);

        // Like Update but DOES NOT notify observers — for purely-transient fields (the interval
        // reconciler's captured assistant text) that must not trigger the persist / UI-refresh /
        // scheduler cascade on every byte of streamed output. No-op if the id is unknown.
        // Applies the same settle-clock bookkeeping as Update (DELIVERY.md §16) — a field write, not a notify.
        void UpdateQuiet(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate);

        // Agentmaster (eager-init / "Activate Tab"): set SessionInfo::started — "has this session's
        // control left ConnectionState::NotConnected" (claude actually running) vs. live-but-dormant.
        // CHANGE-GATED: notifies observers ONLY when the value actually flips, so the ~2s liveness
        // tick that re-asserts it for every hosted tab is free in steady state (it would otherwise
        // churn the persist / UI / scheduler cascade every tick). No-op for an unknown id. Maintained
        // exclusively by TerminalPage (the controls' owner). Thread-safe. A flip also restarts the
        // session's STATE SETTLE clock (DELIVERY.md §16): a tab whose claude just launched waits out a
        // full settle window before its queue moves.
        void SetStarted(const std::wstring& id, bool started);

        // Agentmaster (PENDING_INPUT.md): record the session's UNSENT input-box DRAFT (read out-of-band
        // from the rendered buffer by the UI lane — TerminalPage::_ScanPendingInput). The field updates
        // every change, but _notify fires ONLY on the BOOLEAN hasPending FLIP (empty<->non-empty) — the
        // "yes pending / no pending" transition the tab-strip + Triage-Board animations key on — at
        // presence-heartbeat (turn) cadence, never per-keystroke (a text-only edit stays QUIET so it
        // can't thrash the persist / board / scheduler cascade). Returns true iff the boolean flipped
        // (== whether it notified). Every non-empty set (changed or not) quietly re-stamps
        // pendingInputUnixMs — the "last actually observed" clock the staleness display keys on; a
        // clear zeroes the stamp + drops pendingPasteRefs. The draft trio persists with the record
        // (PENDING_INPUT.md §5), riding the flip notify / any later save — never a save of its own.
        // No-op for an unknown id. Thread-safe.
        bool SetPendingInput(const std::wstring& id, const std::wstring& text);

        // Agentmaster (PENDING_INPUT.md §2b): record the paste-cache resolver's verdict for the current
        // draft (ResolvePendingPasteRefs' annotation — display only). QUIET (no notify) + change-gated;
        // dropped when the draft has meanwhile cleared. Thread-safe.
        void SetPendingPasteRefs(const std::wstring& id, const std::wstring& refs);

        // Agentmaster (PENDING_INPUT.md §2c — the PASTE JOURNAL): record a paste Agentmaster itself just
        // delivered into this session's input box — a user clipboard paste (TerminalPage's paste handler)
        // or one of our own no-submit box fills (the §10 re-fill, the /handover-standby fill, the §9
        // swap's re-paste fallback, the §11 fork carry). Current Claude Code keeps an unsent paste ONLY in
        // its own process memory (the paste-cache is written at SUBMIT time), so this journal is the one
        // place a draft's "[Pasted text #N +M lines]" placeholder can be resolved back to its content —
        // what makes a fork carry, a Copy Current Prompt and the §8c store see the real text.
        // The text is stored NormalizePastedTextLikeClaude-normalized (so "+M" and the entry agree by
        // construction), ONLY when claude would collapse it (an inline paste never backs a placeholder),
        // de-duplicated against the newest entry (an identical re-paste just refreshes its stamp), and
        // BOUNDED: at most kPasteJournalMaxEntriesPerSession entries / kPasteJournalMaxSessionChars per
        // session (oldest dropped), an entry over kPasteJournalMaxEntryChars is not recorded at all, and
        // the whole fleet stays under kPasteJournalMaxTotalChars (the globally oldest entries evicted).
        // TRANSIENT — never persisted, never part of a SessionInfo snapshot (a paste can be MBs, and
        // snapshots are copied on hot UI paths). QUIET (no notify). A no-op for an unknown session.
        //   id      — the session the paste went into.
        //   rawText — the text as pasted (normalized here).
        //   unixMs  — when (0 == now).
        //   throws  — std::bad_alloc on allocation failure only.
        void NotePastedText(const std::wstring& id, const std::wstring& rawText, int64_t unixMs = 0);

        // Agentmaster (§2c): a COPY of the session's paste journal, oldest first — the resolver input
        // (ResolveDraftCarry). Empty for an unknown session / nothing pasted since the draft last cleared.
        // Thread-safe; returns by value so the caller can resolve off-thread without holding the lock.
        //   id      — the session.
        //   returns — the entries (may be several MB in the worst case — call on demand, never per tick).
        //   throws  — std::bad_alloc on allocation failure only.
        std::vector<PasteJournalEntry> PasteJournal(const std::wstring& id) const;

        // Agentmaster (§2c): forget the session's paste journal — called when its draft CLEARS (the scan's
        // appear/clear flip to empty: the prompt was sent or discarded, so nothing in the journal can be
        // in the box any more). Without the prune, an old same-length paste could be assigned to a NEW
        // placeholder whose own paste went unrecorded (a drag-dropped text). Thread-safe, idempotent.
        //   id — the session.
        void ClearPasteJournal(const std::wstring& id);

        // Agentmaster (§2c): move a session's journal onto another id (appended after the target's own
        // entries) — the restore-FRESH re-key, where the reopened draft memory moves to a newly minted
        // conversation id and its placeholders must keep resolving. No-op when `fromId` has none or the
        // ids are equal. Thread-safe.
        //   fromId, toId — the old and the new session id.
        void MovePasteJournal(const std::wstring& fromId, const std::wstring& toId);

        // Agentmaster (DELIVERY_PLAN.md R5 — the tri-state box read): record the input box's last
        // observed STATE (SessionInfo::pendingBoxState + its change stamp). Fed by the UI scan lane
        // each liveness tick and by a send's pre-flight decline (fresh evidence). Change-gated and
        // QUIET — except a BLOCKED→UNBLOCKED transition (NoBox/MenuOpen → Empty/Draft/Unknown),
        // which notifies: that release is what re-fires an advance DecideAdvance held on the box
        // (the delivery gate's close-notify idiom). No-op for an unknown id. Thread-safe.
        void SetPendingBoxState(const std::wstring& id, InputBoxState state);

        // Record a human message the interval reconciler (SessionScanner) found in the transcript
        // that the UserPromptSubmit hook dropped. IDEMPOTENT by text: if an identical message is
        // already recorded (any prompt with status Sent — covers our injected Flight echoes AND
        // prior Typed captures), it is NOT re-added, so the push (hook) and pull (scan) paths
        // converge instead of double-recording. Otherwise it appends a Typed/Sent entry exactly
        // like a hook-captured typed prompt and notifies. No-op for empty text / unknown id.
        //
        // Agentmaster (DELIVERY_PLAN.md R1 — the PULL echo-consume): when the fold-matched recorded
        // prompt is a Sent + UNECHOED Autorun one, this transcript line IS the proof our injection
        // became a message — it is marked `echoed` (quietly), unifying delivery evidence for hooked
        // and no-hook sessions alike (the pickup guard / Enter-retry watchdog / lost-send verdict
        // all key on `echoed`). `observedUnixMs` is the transcript LINE's own timestamp (0 = not
        // known): a replayed OLD identical line (ts before the prompt's send) never vouches for a
        // NEW send, while a late-READ fresh line still consumes (deliberately no recency window —
        // the line's own time is the staleness filter, not our read time). Thread-safe.
        void NoteExternalPrompt(const std::wstring& id, const std::wstring& text, int64_t observedUnixMs = 0);

        // Agentmaster (the INTERRUPT HOLD — DELIVERY.md §14): the SessionScanner sighted the user's
        // turn-abort marker (`[Request interrupted by user…]`, Esc) as the transcript's NEWEST user
        // line. PARK this session's Tests Autorunner — mode -> Off, the prior mode remembered in
        // AutorunnerState::interruptHeldMode (TakeInterruptHold) — so the turn-end the interrupt
        // produces (recon-stop -> WaitingForInput -> the advance seam) can never auto-send the next
        // queued prompt into a session the user just stopped to talk to; the user's next message
        // (a prompt-carrying UserPromptSubmit / a NoteExternalPrompt line stamped after the marker)
        // resumes it (ResumeInterruptHold). `markerUnixMs` = the marker line's own transcript
        // timestamp — the resume anchor AND the replay belt: a marker older than
        // kInterruptHoldFreshMs (a restored/adopted session's history replay) or unstamped never
        // holds (InterruptMarkerIsFresh). Live sessions only. Idempotent: an already-Off autorunner
        // (the user's own Off, or an existing hold) is untouched and nothing notifies. A TAKEN hold
        // logs [interrupt-hold] (hooks.log + autorunner.log) and notifies (the header toggle / overlay
        // / board repaint Off; the scheduler's OnObserved reads Off). Thread-safe.
        void NoteInterrupt(const std::wstring& id, int64_t markerUnixMs);

        // Wiring. Multiple observers may register (e.g. a logger, the Triage Board UI, the
        // scheduler, every window's Manager lens); each is invoked on every change, outside the
        // lock. AddObserver returns a token; RemoveObserver detaches it (M9 window teardown).
        ObserverToken AddObserver(RegistryObserver observer);
        void RemoveObserver(ObserverToken token);
        void SetAdvanceHandler(AdvanceHandler handler);
        // Agentmaster (adopt/re-home autorunner default): the Tests Autorunner MODE stamped onto a
        // session record the registry CREATES itself — the hook SessionStart adopt path and the Fleet
        // Observer's first-sight create. Every other opened session gets the cog default at its
        // launch/restore seam (TerminalPage.AgentSessions.cpp), but these two records are minted
        // INSIDE the engine where AppSettings isn't in scope, so they silently defaulted to Off — an
        // adopted hand-typed claude (or the NEW conversation id an in-session /clear or /resume mints
        // on a re-homed tab) then never auto-sent its queued prompts, contradicting the documented
        // "the MODE seeds every OPENED session — new, adopted, AND restored" (AppSettings,
        // SessionModels.h) and reading as "queued prompts stuck Pending, never sent". Wired at engine
        // init in EVERY build since 2026-10-09 (until then it was gated on IsDevOrDebugPackage like the
        // scheduler's auto-advance, so a plain release kept minting Off records) and re-pushed live by
        // the cog Save. Un-wired (the test harness, the CLI) the registry still mints Off. CREATION-only:
        // an existing record's mode is the user's per-session choice (or the launch/restore stamp) and
        // is never touched here.
        //   mode   — the mode to stamp on records created from now on (Off == Paused, Full == Auto).
        //   throws — nothing beyond std::mutex::lock's std::system_error.
        void SetDefaultAutorunnerMode(AutorunnerMode mode);
        // Multiple adoption handlers may register — one per window (M9). When a hook arrives for
        // a session we didn't Launch, ALL are invoked (outside the lock); whichever window hosts
        // the `+` tab binds it, the rest no-op. AddAdoptionHandler returns a token; the window
        // detaches via RemoveAdoptionHandler on teardown.
        AdoptionToken AddAdoptionHandler(AdoptionHandler handler);
        void RemoveAdoptionHandler(AdoptionToken token);

        // Bind / clear a session's stdin injector.
        void SetInjector(const std::wstring& id, Injector injector);
        // Inject text into a session via its bound injector. Returns false if none bound.
        bool Inject(const std::wstring& id, const std::wstring& text) const;
        // Agentmaster (TAB_OVERLAY.md): is a stdin injector currently bound to this session? Drives
        // the per-tab overlay's link state (⛓ linked vs observe-only) without exposing the injector.
        bool HasInjector(const std::wstring& id) const;

        // Agentmaster (PENDING_INPUT.md §9): bind / clear the hosting window's prompt submitter.
        // Set and cleared in lockstep with SetInjector — the same window, the same lifetime.
        void SetPromptSubmitter(const std::wstring& id, PromptSubmitter submitter);

        // Agentmaster (DELIVERY_PLAN.md R8 — the VERIFIED PRESSER): bind / clear the hosting
        // window's Enter presser — the watchdog's lone-Enter rescue routed through a LIVE box read
        // at press time instead of the scan-stale draft guard alone. Same registration lifetime as
        // the submitter/injector. The presser is invoked on the SCHEDULER worker and must
        // self-marshal (fire-and-forget to its UI thread), where it reads the box and presses ONLY
        // into a verified-Empty box or one holding the watched prompt; a Foreign read refreshes
        // pendingInput, a NoBox/MenuOpen read refreshes pendingBoxState (an Enter into a menu
        // SELECTS the highlighted option), and both refuse — each outcome logged by the window.
        void SetEnterPresser(const std::wstring& id, EnterPresser presser);

        // Dispatch the watchdog's lone-Enter press for `promptId`/`promptText`. With a presser
        // bound: invoke it (outside the lock) and return true — the WINDOW owns the verified press
        // + its logging; the caller does its bookkeeping (attempt count / sentAt refresh)
        // immediately, a later refusal reading as a spent attempt (deliberate: refusals must
        // surface through the give-up ladder, never retry silently forever). With none bound:
        // returns false — the caller keeps the historical raw Inject("\r") path (tests/CLI).
        bool PressEnterVerified(const std::wstring& id, const std::wstring& promptId, const std::wstring& promptText);

        // SEND a queued prompt — the ONE seam every submit path goes through (the autorunner's
        // auto-send, the Manager's Send-now, the /handover paste pump), so
        // the draft swap can never apply to some of them and not others. With a submitter bound
        // the hosting window owns the delivery (and any later rollback); otherwise this is exactly
        // the historical Inject(BuildPromptSubmission(text)). Returns "accepted for delivery";
        // false keeps every existing caller's rollback behavior byte-identical. Thread-safe: the
        // submitter is copied under the lock and invoked outside it (the Inject recipe), so a
        // submitter that hops to its UI thread cannot deadlock the caller.
        //
        // Agentmaster (DELIVERY.md): non-const since the gate work — it OPENS the session's
        // delivery gate before invoking anything and declines SYNCHRONOUSLY (false, nothing
        // marshalled) while another delivery/clear holds it; the fallback path closes the gate
        // right after its inline inject, an accepted submitter keeps it open for the hosting
        // window to close on resolve, and a refusing/throwing submitter closes it here.
        bool SubmitPrompt(const PromptSubmission& submission);

        // Agentmaster (DELIVERY.md — the DELIVERY GATE): claim / release / query the per-session
        // "someone owns the input box" fact (SessionInfo::deliveryPromptId/-OpenedUnixMs,
        // transient). TryOpen atomically claims it for `tag` (a prompt id, or the reserved
        // kDeliveryGateClearTag) — false while another unexpired claim holds it; an EXPIRED claim
        // (the holder died without closing) is reclaimed, logged. Close releases ONLY a matching
        // tag (a stale holder can never clear a younger delivery's claim; a double close is a
        // quiet no-op) and NOTIFIES observers on a real release — that notify is what wakes the
        // scheduler's held advance (OnObserved -> RequestAdvance), so nothing polls. Held is the
        // expiry-aware query the fill pumps skip a tick on. All thread-safe.
        bool TryOpenDeliveryGate(const std::wstring& id, const std::wstring& tag);
        void CloseDeliveryGate(const std::wstring& id, const std::wstring& tag);
        bool DeliveryGateHeld(const std::wstring& id) const;
        // Agentmaster (DELIVERY.md §12): re-assert an in-flight delivery's OWN claim. True + a fresh
        // openedUnixMs stamp iff the gate's tag is still `tag` — INCLUDING one that expired while the
        // delivery sat in a backlogged dispatcher (the owner is alive after all; re-stamping revives
        // the claim so the deciders read "delivery in flight" again). False when the tag no longer
        // owns it (reclaimed / re-opened by a newer attempt — the caller must ABORT: another carrier
        // owns the box now) or the gate is closed. Called at the delivery's top guard and before the
        // irreversible CR commit, so a stale delivery can never type into a box it no longer owns.
        bool RevalidateDeliveryGate(const std::wstring& id, const std::wstring& tag);
        // Agentmaster (DELIVERY.md §12 — INJECTION EVIDENCE): stamp QueuedPrompt::injectedAtUnixMs
        // the moment a prompt's bytes actually reach the ConPTY (the [delivered] seams). QUIET
        // bookkeeping (no notify — the echo/turn machinery carries the visible consequences); no-op
        // for an unknown id/prompt or one no longer Sent. This is what lets the lost-send verdict
        // (DecideLostSend) tell "delivered but vanished" from "accepted but never yet delivered"
        // (DecideUndeliveredReclaim) instead of failing a slow delivery one second past gate expiry.
        void MarkPromptInjected(const std::wstring& id, const std::wstring& promptId);

        // Return a Sent prompt to Pending — the shared body of the Rule-#4 rollback every send path
        // already performed inline, now also reachable from the hosting window when an accepted
        // submission aborts asynchronously (a draft swap that could not clear the input box).
        // `refundAutoSend` also gives back the autorunner's autoSendsThisRun budget slot; it must be
        // false for a Send-now, which never spent one. No-op for an unknown id / prompt, or a prompt
        // that is no longer Sent (something else already handled it). Notifies (it is an Update), so
        // the queue UI and the idle-start re-trigger both see the prompt become Pending again.
        void RollbackPromptToPending(const std::wstring& id, const std::wstring& promptId, bool refundAutoSend);

        // pauseOnHumanInput support: record the last time the human typed into a session.
        void NoteHumanInput(const std::wstring& id, int64_t unixMs);
        int64_t LastHumanInputUnixMs(const std::wstring& id) const;

    private:
        // Snapshot the observer list under the lock, then invoke each outside it.
        void _notify(const SessionInfo& snapshot, HookEvent cause);

        // Agentmaster (one ConPTY = one live conversation; Rule #14 / session-id divergence): a single
        // claude.exe — one pid on one ConPTY (tabToken) — can be in exactly ONE conversation at a time.
        // An in-session /clear or /resume switches its conversation id (a /compact does NOT — it is
        // in-place, same id + same file; HOOKS.md "Compaction"), so the OLD id is no
        // longer live on that tab; but its record lingered `live`, leaving TWO live records claiming one
        // ConPTY. The tab reconciler matches purely by tabToken, so it churned re-homing the tab between
        // the two ids — flickering the tab header between the managed name and claude's live OSC title.
        // When `winnerId` (pid `pid`, tabToken `tabToken`) is confirmed as the CURRENT conversation,
        // archive (live=false) every OTHER live record that shares that ConPTY and process. Discriminated:
        //  - by `pid`, so a child sub-claude that inherited the parent's WT_SESSION (a DIFFERENT pid on
        //    the same tabToken) is NEVER superseded — it is a real, concurrent process (an unknown-pid
        //    sibling is also archivable: it is a not-yet-observed stale record, not a live sub-claude);
        //  - by freshness (hook/transcript activity, NOT observation time), so a stale or mis-resolved
        //    observation can never archive the genuinely-current conversation (the newest always wins).
        // Caller holds _mtx; returns the archived snapshots so the caller can _notify them outside it.
        std::vector<SessionInfo> _SupersedeStaleTabSiblings(const std::wstring& winnerId, const std::wstring& tabToken, uint32_t pid, int64_t winnerFreshness);

        // Agentmaster (RETIRED HOSTS): one retired host connection of a session (RetireHostToken).
        struct RetiredHostToken
        {
            std::wstring token; // the replaced connection's WT_SESSION, as given
            int64_t retiredAtUnixMs{ 0 }; // when it was retired — the kRetiredHostTokenTtlMs clock
            // A dropped ObserveClaude for this token was already logged. The observer re-surveys every ~2 s
            // while the dying process lingers, so this keeps the evidence to one line per token. Mutable:
            // flipped through the const finder below, always under _mtx.
            mutable bool observeDropLogged{ false };
        };
        // Agentmaster (RETIRED HOSTS): the unexpired retirement of `tabToken` for `sessionId`, or nullptr.
        // Caller holds _mtx.
        //   sessionId — the session whose retirements to search.
        //   tabToken  — the token to look for (case-insensitive; "" never matches).
        //   nowUnixMs — the TTL clock.
        //   returns   — the matching entry (its log latch may be flipped) or nullptr. Never throws.
        const RetiredHostToken* _FindRetiredHostTokenLocked(const std::wstring& sessionId, const std::wstring& tabToken, int64_t nowUnixMs) const noexcept;

        mutable std::mutex _mtx;
        // insertion-ordered storage so the UI shows a stable order
        std::vector<std::wstring> _order;
        std::unordered_map<std::wstring, SessionInfo> _sessions;
        std::unordered_map<std::wstring, Injector> _injectors;
        std::unordered_map<std::wstring, PromptSubmitter> _submitters; // PENDING_INPUT.md §9
        std::unordered_map<std::wstring, EnterPresser> _pressers; // DELIVERY_PLAN.md R8 — same lifetime as the submitter
        std::unordered_map<std::wstring, int64_t> _lastHumanInput;
        // Agentmaster (PENDING_INPUT.md §2c): the per-session PASTE JOURNAL (NotePastedText), oldest entry
        // first. A side map — never inside SessionInfo — because a paste can be megabytes and SessionInfo
        // snapshots are deep-copied on hot UI paths (the 2026-09-04 lens-rebuild incident). Guarded by _mtx.
        std::unordered_map<std::wstring, std::vector<PasteJournalEntry>> _pasteJournals;
        // Total wchar_t count across every journal — what the fleet-wide kPasteJournalMaxTotalChars cap is
        // enforced against without re-walking every entry per paste. Guarded by _mtx.
        size_t _pasteJournalChars{ 0 };
        // Agentmaster (RETIRED HOSTS): sessionId -> the host connections we replaced, oldest first (at most
        // kRetiredHostTokensPerSession each; expired entries pruned on the next retire). Transient — a dying
        // process cannot outlive an app restart. Dropped with the session by Remove. Guarded by _mtx.
        std::unordered_map<std::wstring, std::vector<RetiredHostToken>> _retiredHostTokens;
        // Agentmaster (the summary's conversation chain — SessionInfo::continuedFromId): one conversation a
        // /clear just ENDED — a SessionEnd whose reason is "clear" (kHookSourceClear).
        struct RecentClearEnd
        {
            std::wstring tabToken; // the ConPTY (WT_SESSION) the cleared conversation ran on
            std::wstring sessionId; // the conversation the /clear ended
            int64_t endedAtUnixMs{ 0 }; // when its SessionEnd arrived — the kRecentClearEndTtlMs clock
        };
        // The conversations /clear recently ended, oldest first. Claude runs the old conversation's
        // SessionEnd hooks to completion BEFORE the new one's SessionStart, so the SessionStart(source
        // "clear") that follows on the same ConPTY pairs with EXACTLY the entry for that ConPTY — a precise
        // signal a token match alone lacks: a child `claude -p` (a Bash tool's or a hook's) inherits the
        // tab's WT_SESSION and can hold a fresher hook on it. An entry is consumed by its pairing. Bounded
        // (kRecentClearEndsMax, oldest out, one per ConPTY) and short-lived; transient. Guarded by _mtx.
        std::vector<RecentClearEnd> _recentClearEnds;
        static constexpr size_t kRecentClearEndsMax = 16; // ConPTYs with a /clear still waiting for its SessionStart — 16 is far past any real burst
        static constexpr int64_t kRecentClearEndTtlMs = 120'000; // the SessionEnd -> SessionStart gap is milliseconds; 2 min absorbs any hook latency
        std::vector<std::pair<ObserverToken, RegistryObserver>> _observers;
        uint64_t _nextObserverId{ 1 };
        AdvanceHandler _advance;
        // The creation-time autorunner mode for records the registry mints itself (hook-adopt +
        // observer first-sight) — see SetDefaultAutorunnerMode. Off until wired, so the standalone
        // harness / CLI / a release build behave exactly as before. Guarded by _mtx.
        AutorunnerMode _defaultAutorunnerMode{ AutorunnerMode::Off };
        std::vector<std::pair<AdoptionToken, AdoptionHandler>> _adopters;
        uint64_t _nextAdopterId{ 1 };
        // Monotonic counter for ids of `Typed` prompts the registry synthesizes from
        // UserPromptSubmit (the registry has no GUID dependency; the id is local-unique).
        uint64_t _typedSeq{ 0 };
        // Monotonic counter behind PromptSubmission::submitNonce (DELIVERY.md §12) — each delivery
        // ATTEMPT through SubmitPrompt gets a unique gate tag, so a stale attempt can never
        // revalidate a newer attempt's claim. Guarded by _mtx (stamped inside SubmitPrompt).
        int64_t _submitSeq{ 0 };
    };
}
