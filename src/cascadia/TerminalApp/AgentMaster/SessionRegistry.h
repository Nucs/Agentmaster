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

    class SessionRegistry
    {
    public:
        SessionRegistry() = default;
        SessionRegistry(const SessionRegistry&) = delete;
        SessionRegistry& operator=(const SessionRegistry&) = delete;

        // Add a new session or replace an existing one with the same id. Returns the id.
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
        void ObserveClaude(const ObservedClaude& o);

        // Mutate a session's Auto Testing / autorunner under the lock (used by the scheduler
        // and UI). The mutator runs while holding the lock; the post-change snapshot is
        // delivered to the observer afterwards. Returns false if the id is unknown.
        bool Update(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate);

        // Like Update but DOES NOT notify observers — for purely-transient fields (the interval
        // reconciler's captured assistant text) that must not trigger the persist / UI-refresh /
        // scheduler cascade on every byte of streamed output. No-op if the id is unknown.
        void UpdateQuiet(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate);

        // Agentmaster (eager-init / "Activate Tab"): set SessionInfo::started — "has this session's
        // control left ConnectionState::NotConnected" (claude actually running) vs. live-but-dormant.
        // CHANGE-GATED: notifies observers ONLY when the value actually flips, so the ~2s liveness
        // tick that re-asserts it for every hosted tab is free in steady state (it would otherwise
        // churn the persist / UI / scheduler cascade every tick). No-op for an unknown id. Maintained
        // exclusively by TerminalPage (the controls' owner). Thread-safe.
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
        // init (gated on IsDevOrDebugPackage like the Scheduler itself, so a plain release keeps
        // minting Off records to match its never-started autorunner) and re-pushed live by the cog
        // Save. CREATION-only: an existing record's mode is the user's per-session choice (or the
        // launch/restore stamp) and is never touched here.
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
        // auto-send, its SemiAuto confirm, the Manager's Send-now, the /handover paste pump), so
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
        // An in-session /clear, /compact, or /resume switches its conversation id, so the OLD id is no
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

        mutable std::mutex _mtx;
        // insertion-ordered storage so the UI shows a stable order
        std::vector<std::wstring> _order;
        std::unordered_map<std::wstring, SessionInfo> _sessions;
        std::unordered_map<std::wstring, Injector> _injectors;
        std::unordered_map<std::wstring, PromptSubmitter> _submitters; // PENDING_INPUT.md §9
        std::unordered_map<std::wstring, EnterPresser> _pressers; // DELIVERY_PLAN.md R8 — same lifetime as the submitter
        std::unordered_map<std::wstring, int64_t> _lastHumanInput;
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
