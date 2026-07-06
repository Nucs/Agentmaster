// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — core data model for the Manager tab (Design A / C1 / Auto Testing).
// Plain C++ (no WinRT projection) so it compiles standalone; the XAML layer (M6) wraps
// these in observable view-models. See doc/agentmaster/IMPLEMENTATION.md.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Activity.h" // RunningApp (Fleet Observer live-enrichment field on SessionInfo)

namespace Agentmaster
{
    // Hook-driven lifecycle of a Claude Code session. Authoritative state comes from
    // Claude Code hooks (see doc/agentmaster/HOOKS.md), never from screen-scraping.
    enum class SessionState
    {
        Idle, // launched, no active turn
        Running, // mid-turn (PreToolUse/PostToolUse activity)
        WaitingForInput, // Stop hook fired: turn complete, ready for next user message
        NeedsApproval, // Notification(permission): tool-permission prompt awaiting y/n
        Error, // last turn ended in error
        Done // session ended (SessionEnd)
    };

    // Agentmaster (crash/restore state fidelity — Correctness Rule #16): map a PERSISTED SessionState
    // to the state a freshly-REOPENED session should SEED with, so a CRASH (or a window/session restore)
    // does not silently drop the fleet's "which sessions need me" triage. sessions.json persists the
    // real state, but the reopen path used to force every session to Idle unconditionally — harmless on
    // a clean shutdown (those sessions were deliberately closed) but on a crash it threw away the live
    // Running / WaitingForInput / NeedsApproval the user wanted back (every card landed in "Idle/Done").
    //
    // The mapping keeps ONLY the "AT REST, needs you" states — WaitingForInput and NeedsApproval — which
    // genuinely SURVIVE a `claude --resume`: the conversation is parked at a completed turn / an
    // unanswered question, and the transcript tail still says so. Everything else normalizes to Idle:
    //   * Running -> Idle: a resumed claude does NOT continue the interrupted turn (it waits for you),
    //       and the SessionScanner's primed-cursor gate (ShouldSynthesizeRunning) deliberately refuses to
    //       re-light Running off the initial history replay — so seeding Running would just STICK. Idle is
    //       the true post-resume state.
    //   * Error -> Idle: transient. The scanner re-derives Error from the tail (ShouldSynthesizeError
    //       fires from any non-Error/Done state) if the API error is still the active leaf.
    //   * Done -> Idle: "ended"; a session being reopened is alive again, not done.
    //   * Idle -> Idle: unchanged.
    // The SessionScanner then REFINES the seed on its first reconcile pass (recon-stop corrects a
    // NeedsApproval whose turn actually ended -> WaitingForInput; recon-run/recon-resume promote on fresh
    // work), the Waiting-for-you decay treats a reopened session as UNREAD (readUnixMs resets to 0) so a
    // restored WaitingForInput card keeps waiting until the user actually reads it (never instant-decays
    // off an ancient lastActivity), and hooks own it live the instant the tab is activated (claude
    // resumes -> SessionStart -> Idle). PURE + total.
    inline SessionState RestoredSessionState(SessionState persisted) noexcept
    {
        switch (persisted)
        {
        case SessionState::WaitingForInput:
        case SessionState::NeedsApproval:
            return persisted; // an at-rest "needs you" state survives a resume — preserve the triage cue
        case SessionState::Idle:
        case SessionState::Running:
        case SessionState::Error:
        case SessionState::Done:
        default:
            return SessionState::Idle; // in-flight / transient / ended -> a reopened session starts Idle
        }
    }

    // Agentmaster (crash/restore state fidelity — Rule #16, companion to RestoredSessionState): the
    // question-guard flag (SessionInfo::lastMessageWasQuestion — "the last turn ended on a clarifying
    // question", which HOLDS the Autorunner queue so a queued prompt never auto-ANSWERS the question) is
    // now PERSISTED so it survives a crash — but it is only meaningful at a PRESERVED needs-you
    // turn-boundary. DROP it whenever the restored state normalized to Idle: a session that was Running
    // at the crash had ALREADY answered any prior question (that is what STARTED the interrupted turn),
    // so its now-stale flag=true must not falsely hold the queue on reopen. Kept for WaitingForInput
    // (the fix — the queue stays held so a crash can't auto-answer a pending question) and harmlessly for
    // NeedsApproval (never Autorunner-ready anyway). PURE + total.
    inline bool RestoredQuestionFlag(SessionState restoredState, bool persistedFlag) noexcept
    {
        return restoredState == SessionState::Idle ? false : persistedFlag;
    }

    enum class AutorunnerMode
    {
        Off,
        SemiAuto, // confirm each auto-send (recommended for the first N sends)
        Full // auto-send with no confirmation
    };

    // Agentmaster: session ordering — the basis for BOTH sort toggles. (1) The Explorer Tree's sort
    // (AppSettings::treeSort) after its LOCAL/GLOBAL/EXTERNAL scope toggle, ordering the directory
    // groups AND the rows within each (and the EXTERNAL census); it cycles Newest -> Oldest ->
    // MostActive -> Alpha -> ByPid. (2) The Triage Board's sort (AppSettings::boardSort) after its
    // LOCAL/GLOBAL toggle, ordering the cards within each state column; it cycles MostActive (its
    // DEFAULT) -> Newest -> Oldest -> Alpha — the tree's set minus ByPid (host/shell grouping is
    // meaningless once cards are split across state columns). Both are GLOBAL app settings, persisted
    // to settings.json so the choice is shared by every window and survives restart. One ExplorerSort
    // comparator (SortKeyLess) serves both consumers.
    enum class ExplorerSort
    {
        Newest, // most recently created first (conversation ctime, desc); a fresh/never-prompted session floats up
        Oldest, // oldest created first (ctime, asc)
        MostActive, // most recent activity first; a currently-running session ranks at the very top
        Alpha, // A->Z by title (case-insensitive)
        ByPid // group by host window/shell pid (externals: ExternalClaudeRow::hostPid; managed: the claude pid), then by most active within each group
    };

    // Agentmaster: how the tab/session RENAME box treats Enter / Shift+Enter as a COMMIT (accept +
    // save, exactly like clicking away). The rename box is multi-line (AcceptsReturn), so the key
    // that ISN'T the commit key inserts a newline into the title instead. Clicking away (focus loss)
    // ALWAYS commits in every mode — this only governs the keyboard shortcut. GLOBAL app setting
    // (AppSettings::tabRenameCommitMode), persisted to settings.json so the choice is shared by every
    // window and survives restart. The integer order is wire-stable AND mirrored by the raw-int
    // constants in TabHeaderControl.cpp (a static_assert in TerminalPage.AgentEngine.cpp locks it).
    enum class TabRenameCommitMode
    {
        ClickAwayOnly = 0, // None: only focus-loss commits; both Enter and Shift+Enter insert a newline
        ClickAwayOrShiftEnter = 1, // default: Shift+Enter commits; a plain Enter inserts a newline
        ClickAwayOrEnter = 2 // Enter commits; Shift+Enter inserts a newline
    };

    // Agentmaster (FAVORITES.md §5a): which glyph marks a FAVORITE (starred) session on its LIVE
    // tab strip, drawn over/around the state-colored status dot. Crown = the original — a small gold
    // crown perched at the dot's north-west. Star = the status dot becomes the FOREGROUND of a white,
    // golden-tipped star (the star is drawn BEHIND the dot, so its points radiate around the dot).
    // GLOBAL app setting (AppSettings::favoriteIcon), persisted to settings.json so the choice is
    // shared by every window and survives restart; applied live (cog Save + cross-window broadcast
    // re-assert every hosted favorited tab). Serialized as a string token (Persistence ToString /
    // FavoriteIconFromString); a missing key => Crown (the prior behavior). Tab-strip ONLY — the
    // board card / tree row / overlay HUD carry no favorite glyph.
    enum class FavoriteIcon
    {
        Crown = 0, // default: a gold crown at the status dot's north-west
        Star = 1 // the status dot as the foreground of a white, golden-tipped star (drawn behind the dot)
    };

    // Agentmaster (tab color modes): HOW a managed session's tab gets its color — the cog's TABS
    // "Tab coloring" dropdown. Rule #12's "ONE color per working directory" becomes the DEFAULT of
    // three modes rather than the only behavior:
    //   * WorkingDirectory — the original: every tab in a working dir shares that dir's PERMANENT
    //     color (dir-colors.json; user picks fan out to the whole dir).
    //   * Individual — every managed session gets its OWN color, dealt collision-free against the
    //     other OPEN sessions' colors and PERSISTED on the session record (SessionInfo::tabColorHex,
    //     sessions.json) so it survives close/restore; a user pick recolors ONLY that session.
    //   * InferredWorkingDirectory — WorkingDirectory semantics, but keyed by the directory the
    //     session ACTUALLY works in, INFERRED from the files its tool calls read/edit/create plus
    //     the absolute paths its shell commands name (InferWorkingDirectory over
    //     TranscriptStats::pathsAccessed — canonical tool path fields + ExtractPathsFromText's
    //     command mining — the deepest directory a strict majority of the touched paths share,
    //     re-detected as the transcript grows). A FORK inherits its source's inference at launch
    //     and, while it has no transcript of its own yet, is inferred from the source's (its
    //     content-to-be is a verbatim copy), so it wears the parent conversation's color from the
    //     first frame. Until an inference exists (no file ops yet; or a Codex session — its
    //     rollout isn't path-parsed) the launch cwd keys the color, exactly like WorkingDirectory.
    // GLOBAL app setting (AppSettings::tabColorMode), persisted to settings.json, applied live on
    // cog Save + the cross-window broadcast (every window repaints its hosted managed tabs).
    // Serialized as a string token (Persistence ToString / TabColorModeFromString); a missing key
    // => WorkingDirectory (the prior behavior).
    enum class TabColorMode
    {
        WorkingDirectory = 0, // default: one shared color per working directory (Rule #12 classic)
        Individual = 1, // every managed tab/session wears its own color
        InferredWorkingDirectory = 2 // shared color, keyed by the dir inferred from the files the session touches
    };

    // When a queued prompt is allowed to fire.
    enum class PromptGate
    {
        OnTurnComplete, // default: fire on the Stop hook
        AfterDelay, // fire delayMs after the gate condition is met
        Manual // only via "Send now"
    };

    enum class PromptStatus
    {
        Pending,
        Sent,
        Held, // LEGACY — no longer produced. A pending question / "needs you" state now keeps the
              // prompt Pending (it stays queued and waits, like a Running mid-turn) instead of being
              // parked here. Still parsed (for older sessions.json) + rehabilitated to Pending.
        Skipped,
        Failed
    };

    // How a prompt entered the Auto Testing queue. The queue reflects EVERY message a session
    // received (DESIGN: "all messages user sent, not only via the auto testing"), so a prompt
    // the human typed straight into the ConPTY — captured from the UserPromptSubmit hook — is
    // recorded too, tagged `Typed`, alongside the `Autorun` prompts we queued/injected.
    // (The serialized value is "Autorun"; a pre-rename "Flight" still reads back as Autorun — see
    // PromptOriginFromString.)
    enum class PromptOrigin
    {
        Autorun, // queued and injected through Auto Testing / Tests Autorunner (our send) — was "Flight"
        Typed, // typed directly into the terminal by the human (captured via UserPromptSubmit)
    };

    struct QueuedPrompt
    {
        std::wstring id;
        std::wstring label; // short display name
        std::wstring text; // the prompt body (may be multi-line)
        PromptStatus status{ PromptStatus::Pending };
        PromptGate gate{ PromptGate::OnTurnComplete };
        uint32_t delayMs{ 0 };
        // Optional guard: hold auto-send unless the session output satisfies this.
        // Empty => the scheduler applies the default "not-a-question" guard.
        std::wstring guardPattern;
        std::optional<std::wstring> dependsOn; // prompt id that must be Sent first
        uint32_t attempts{ 0 };
        uint32_t maxAttempts{ 1 };
        int64_t sentAtUnixMs{ 0 };
        PromptOrigin origin{ PromptOrigin::Autorun }; // Autorun (we sent it) vs Typed (human typed it)
        // Transient (NOT persisted): an Autorun prompt we just injected expects ONE
        // UserPromptSubmit echo back; `echoed` marks that echo consumed so the registry does
        // not re-record our own injection as a `Typed` message. Reset to false at each send.
        bool echoed{ false };
        // Transient (NOT persisted): how many EXTRA Enter keystrokes the scheduler has re-pressed
        // for this Sent-but-unacknowledged Flight prompt. The Claude TUI can absorb the original
        // submit Enter as a NEWLINE when the keystroke lands before its input box is ready (the
        // ConPTY delivers text+CR faster than the Ink UI initializes), leaving the prompt typed but
        // never submitted and the turn never starting. The scheduler watches such a prompt and, if
        // the turn has not started within kEnterRetryIntervalMs, re-sends a lone Enter — capped at
        // kEnterRetryMax (Scheduler.h). Reset to 0 at each fresh send.
        uint32_t enterRetries{ 0 };
    };

    // Agentmaster (#6 — multi-line submit): build the ConPTY input that types `text` into Claude's
    // Ink TUI and submits it as ONE message. A bare `text + CR` makes Ink submit on the FIRST embedded
    // line break (the WinUI compose TextBox emits CR per line), tearing a multi-line prompt across
    // submits — and the lone-CR Enter-retry can't reassemble it. Wrap the body in a bracketed paste
    // (ESC[200~ … ESC[201~) so Ink treats embedded newlines as literal pasted text, then a single
    // trailing CR (outside the paste) submits the whole block. Embedded CR / CRLF are normalized to LF
    // for clean pasted lines. Assumes Claude's TUI enables bracketed-paste mode (it does — it supports
    // multi-line paste). Also hardens single-line sends against the CR-eaten race: the submit CR now
    // follows a complete, delimited paste instead of riding in raw with the text. PURE.
    inline std::wstring BuildPromptSubmission(std::wstring_view text)
    {
        std::wstring body;
        body.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'\r')
            {
                body.push_back(L'\n');
                if (i + 1 < text.size() && text[i + 1] == L'\n')
                {
                    ++i; // collapse CRLF -> one LF
                }
            }
            else
            {
                body.push_back(text[i]);
            }
        }
        return L"\x1b[200~" + body + L"\x1b[201~\r";
    }

    struct ApprovalPolicy
    {
        bool pauseForHuman{ true }; // default: do not auto-approve tool permissions
        std::vector<std::wstring> autoApproveTools; // allowlist of safe tools
    };

    struct AutorunnerState
    {
        AutorunnerMode mode{ AutorunnerMode::Off };
        uint32_t throttleMs{ 500 }; // min delay between auto-sends
        bool stopOnError{ true }; // pause the plan if a turn ends in error
        bool pauseOnHumanInput{ true }; // suspend while the human is typing
        uint32_t maxAutoSends{ 100 }; // runaway backstop
        uint32_t autoSendsThisRun{ 0 };
        ApprovalPolicy approval{};
    };

    // Agentmaster (event ordering + turn identity): per-session turn accounting for the
    // hook-driven state machine. Each hook is an independent fire-and-forget forwarder process,
    // so events can arrive LATE (the Stop path does transcript work the UserPromptSubmit path
    // doesn't) and prompts can be TYPED AHEAD (Claude Code fires UserPromptSubmit at Enter-time
    // for a prompt queued behind the in-flight turn, then consumes the queued batch as the next
    // turn with NO further UserPromptSubmit). NextSessionStateOrdered (HookEvents.h) reads/writes
    // this so a stale Stop cannot demote a newer turn and a Stop with a queued prompt behind it
    // stays Running. ALL transient (NOT persisted — turn identity is meaningless across restarts).
    struct TurnAccounting
    {
        int64_t lastPromptUnixMs{ 0 }; // hook FIRE time (wire ts) of the newest UserPromptSubmit
        int32_t queuedPrompts{ 0 }; // prompts submitted while a turn was in flight (type-ahead), pending behind it
    };

    // One Claude Code session = one claude.exe on a ConPTY connection, tracked by the
    // SessionRegistry. `id` is also injected into the child as CCMGR_SESSION_ID so hooks
    // can be correlated back to this record (see HOOKS.md).
    struct SessionInfo
    {
        std::wstring id;
        std::wstring title; // task / display name
        std::wstring workingDir; // the "M" axis: which working directory
        std::wstring branch; // git branch / worktree
        // Agentmaster (tab color modes — TabColorMode::Individual): this session's OWN tab color
        // ("#RRGGBB"; empty = none dealt/picked yet). Only meaningful while the GLOBAL
        // AppSettings::tabColorMode is Individual: dealt collision-free against the other OPEN
        // sessions' colors on first paint (_ApplySessionTabColor -> ChooseSessionAutoColor) and
        // set by a user color pick on the tab (no dir fan-out — individual). PERSISTED
        // (sessions.json) so a session keeps ITS color across close/restore — the per-session
        // analog of dir-colors.json's per-folder permanence (Rule #12). Kept (dormant) while the
        // mode is dir-keyed, so switching back to Individual restores the same colors. A tab-color
        // RESET clears it (the next launch deals a fresh one).
        std::wstring tabColorHex;
        // Agentmaster (tab color modes — TabColorMode::InferredWorkingDirectory): the working
        // directory this session was INFERRED to actually work in — the deepest directory a
        // majority of its tool-touched paths (files read/edited/created + searched dirs) share
        // (InferWorkingDirectory over the transcript's pathsAccessed). Re-detected as the
        // transcript grows (TerminalPage::_ScanInferredTabColors, mtime-gated off the scanner
        // tick) and updated through the registry; empty until the session has file ops (the
        // launch cwd then keys the color) and always empty for Codex (its rollout isn't
        // path-parsed). PERSISTED (sessions.json) — a derived CACHE, like the title — so a
        // reopened session wears its inferred color immediately instead of flipping from the cwd
        // color after the first scan. Only consulted while tabColorMode is InferredWorkingDirectory.
        std::wstring inferredWorkingDir;
        // Agentmaster (Codex managed-session support): which coding agent this session is. Default
        // Claude, so every existing record/path is byte-for-byte unchanged. A Codex session rides the
        // SAME managed path as Claude (immediate card at launch, registry record, archive/restore,
        // per-window persistence); the divergences are confined to `codexSessionId` + the resume
        // commandline (Codex can't pin an id and has no hooks — its state comes from the C2 rollout
        // tail). Persisted.
        AgentKind kind{ AgentKind::Claude };
        // Codex only: the REAL rollout conversation uuid — the `codex resume <uuid>` target and the
        // "does a transcript exist" gate (the Codex analog of how Claude's `id` doubles as its resume
        // id). OUR `id` above is a minted, durable handle (the registry / persistence / tab-map key,
        // exactly Claude's id role); for Codex it is NOT the conversation id (Codex mints that itself,
        // embedded in the date-sharded rollout filename — no --session-id), so the resume target is
        // carried HERE, filled by the Fleet Observer once the rollout resolves. Empty until the first
        // turn writes it (a never-prompted Codex restore-freshes, like Claude); always empty for Claude.
        // Persisted.
        std::wstring codexSessionId;
        SessionState state{ SessionState::Idle };
        // The Waiting-for-you "unread" DECAY ANCHOR (wall-clock ms, PERSISTED). Stamped monotonically
        // by EVERY hook's wire `ts` — SessionStart included — and deliberately RESTARTED by the UI's
        // plain "Move to Waiting-for-you" triage promote (both the tab-menu and board-card surfaces),
        // so it means "when did this card last demand attention", NOT "when did Claude last talk to
        // the API". Display surfaces fall back to it for the "-lastActivityAgo" adornment / MOST
        // ACTIVE sort when the transcript timing is unresolved. It must NEVER feed the ⚡
        // server-cache hint (that is lastTurnUnixMs + convLastActivityUnixMs — ServerCacheStillWarm
        // below): a launch/adopt/resume SessionStart or a triage move stamps this "now" with zero
        // API traffic, which was exactly the reported ⚡ false positive.
        int64_t lastActivityUnixMs{ 0 };
        // Agentmaster (⚡ server-cache hint): the last hook-time EVIDENCE OF A REAL API TURN —
        // stamped monotonically by SessionRegistry::OnHookEvent from the wire `ts`, but ONLY for
        // events that mean Claude actually just made an API request (IsApiTurnEvidence, HookEvents.h:
        // UserPromptSubmit / Pre-/PostToolUse / Notification / SubagentStop / a REAL Stop). NEVER
        // stamped by SessionStart (launch / --resume / adopt / /clear send no request), SessionEnd,
        // a synthesized quiescent Stop (reconciliation-timed — incl. the no-API double-ESC
        // recon-error-release), or any UI mutation (the triage moves / Mark Unread). Feeds the
        // Triage-Board ⚡ "still server-cached" hint together with the transcript-derived
        // convLastActivityUnixMs (ServerCacheStillWarm takes the freshest of the two): the hook side
        // lights the ⚡ the instant a prompt is submitted (the observer's transcript enrichment is
        // silent + survey-lagged), the transcript side covers hook-less (observer-adopted) sessions.
        // Transient (NOT persisted; Persistence.cpp must not write it) — after a reopen there is
        // nothing to follow up INTO until the session is resumed, and a resumed session's truth is
        // re-derived from the transcript within one survey.
        int64_t lastTurnUnixMs{ 0 };
        // True for a session ADOPTED from a claude we did NOT launch (typed into a `+` tab,
        // not Launched by the Manager). It is observe-only until the app correlates it to its
        // ConPTY (via the WT_SESSION tabToken) and binds an injector. Cleared once it is
        // (re)launched as a managed session on restore. Persisted so the card survives reopen.
        bool external{ false };
        // Transient (NOT persisted): the latest hosting WT_SESSION (the ConPTY's stable id) that
        // hooks reported for this session (the wire `tabToken`). The app correlates a tab to its
        // session by this. It is STABLE across an in-session `/resume` — which mints a NEW Claude
        // session id but keeps the SAME ConPTY — so the periodic tab reconcile re-homes the tab to
        // the new id by matching this against each live terminal's WT_SESSION. Empty until a hook
        // arrives (and after a fresh load, until the session re-emits one) — EXCEPT a Manager launch
        // stamps it eagerly from the connection's WT_SESSION (see the forkParentId note below).
        std::wstring tabToken;
        // Agentmaster (PERSISTED): the SOURCE conversation id this session was FORKED from — set at
        // launch when this is a fork (`claude --resume <src> --fork-session --session-id <this.id>`; the
        // Sessions-page / duplicate-tab / adopt-external fork). It serves TWO purposes:
        //   (1) The --fork-session source-id ECHO GUARD. A `--fork-session` claude fires its FIRST
        //       SessionStart hook under the SOURCE id `<src>`, NOT the freshly-minted `<this.id>` we
        //       registered + bound at launch. Without this, the registry sees `<src>` as an unknown
        //       session and the bind/re-home path mistakes the echo for an in-session `/resume`,
        //       re-homing the fork's tab off `<this.id>` onto `<src>` — so the tab tracks the wrong
        //       (inactive, source) conversation while the real fork (`<this.id>`, which gets every later
        //       hook) is orphaned. SessionRegistry::OnHookEvent uses it (with the eagerly-stamped
        //       tabToken) to IGNORE that source-id startup echo. The guard is ONE-SHOT — cleared on the
        //       fork's FIRST own-id hook, so a LATER deliberate `/resume <src>` re-homes normally.
        //   (2) Restoring a NEVER-MESSAGED fork — the reason this is now PERSISTED (it was transient
        //       before). A fork's OWN transcript (`<this.id>.jsonl`) is written only on its FIRST turn,
        //       so a fork the user created but never sent a message to has NO transcript on disk — and a
        //       plain restore would transcript-gate to a brand-new EMPTY conversation, silently LOSING
        //       the forked branch (and churning the id) on EVERY restart. With `<src>` persisted,
        //       _LaunchClaudeSession re-forks from it into the SAME id when the fork's own transcript is
        //       absent but the source's still exists — re-materializing the identical branch with its
        //       identity (and the WindowRecord tab ref) intact. Gated on the fork being transcript-less,
        //       so once the fork gets its own conversation, purpose (1)'s one-shot clear wipes this
        //       (persisted on the next change) and it is never re-forked off a now-divergent source.
        // Empty for a non-fork (and cleared once the fork has its own conversation).
        std::wstring forkParentId;
        // Transient runtime flag (NOT persisted): is this session OPEN (has a live tab +
        // claude.exe this run) or ARCHIVED (shut down but kept restorable)? The Triage Board /
        // Explorer Tree show only Open (live) sessions; Archived (!live) ones are listed behind
        // the Manager's "Archived" button and can be restored on demand. Set true when we Launch
        // / restore / adopt a session, false when the user archives it (closes its tab). Always
        // false on load (Correctness Rule #6: startup re-opens NOTHING — every persisted session
        // comes back Archived, restorable as a whole), so it never needs to round-trip to JSON.
        bool live{ false };
        // Agentmaster (eager-init / "Activate Tab"): has this session's ConPTY/claude actually
        // STARTED, i.e. has its hosting TermControl left ConnectionState::NotConnected at least
        // once? A WT background tab spawns its child lazily — only on the SwapChainPanel's first
        // non-zero layout, which fires when the tab is first SHOWN — so a window-restored / re-homed
        // managed tab that the user never clicked stays dormant (claude never resumes, no hooks, no
        // autorunner). This flag is the UI's "needs activation" signal: false == live but dormant
        // (the half-hollow status dot + the "Activate All Tabs (N)" count + the per-tab/menu
        // "Activate Tab" gate); it flips true when the control starts (naturally on focus, or in
        // place via TermControl::InitializeWithSize). Transient (NOT persisted) and maintained ONLY
        // by TerminalPage (the single owner of the controls) from ConnectionState() each liveness
        // tick + immediately on an Activate; always false on load (a restored session is dormant
        // until opened), so it never round-trips to JSON. Meaningless for an `external` session
        // (we host no control) — left false, and every consumer also gates on `!external`.
        bool started{ false };
        // Set from the most recent Stop hook's best-effort `lastMessageIsQuestion`. Feeds the
        // Autorunner question-guard (M7): a turn that ended on a clarifying question must NOT be
        // auto-answered (DecideAdvance holds the queued prompt while this is true). PERSISTED (Rule
        // #16, crash/restore fidelity): the flag rides sessions.json so a crash while the agent was
        // waiting on a question can't drop the guard and auto-answer it on reopen — restored ONLY
        // alongside a preserved needs-you state (RestoredQuestionFlag drops it when the state
        // normalizes to Idle, so a stale flag from an interrupted Running turn can't falsely hold the
        // queue). Written only by a fresh Stop (SessionRegistry::OnHookEvent), never cleared by
        // SessionStart — so it correctly stays true across a --resume until a real non-question turn.
        bool lastMessageWasQuestion{ false };
        // Agentmaster (Waiting-for-you "unread" model): the last time the user READ this session —
        // i.e. visited (switched to) its terminal tab, or had it as the focused tab while a turn
        // completed. Transient (NOT persisted), wall-clock ms (NowMs / system_clock), set from the
        // UI lane (TerminalPage::_VisitTabClearFlash / _EvaluateAgentFlash). A session is "unread for
        // the current turn" when readUnixMs < lastActivityUnixMs (new activity landed since the last
        // read). The Waiting-for-you -> Idle decay (SessionScanner::_maybeDecayWaiting /
        // ShouldDecayWaitingToIdle) fires only once the session has been READ *and* the timeout has
        // elapsed: an unread, past-timeout session keeps waiting until the user reads it. 0 == never read.
        int64_t readUnixMs{ 0 };
        // Agentmaster (Waiting-for-you "unread" model): the context-menu "Mark Unread" — a STICKY
        // manual mark. While set, the session shows in Waiting-for-you (the mark can PROMOTE an
        // Idle/Done session there) and the time-decay never demotes it; only a visit (read) or
        // archive clears it. Transient (NOT persisted), set/cleared from the UI lane
        // (TerminalPage::_MarkSessionUnread / _ClearSessionUnread), the engine twin of the per-window
        // red-flash-ring set so the BOARD STATE (in every window) reflects the mark, not just one
        // window's tab ring.
        bool manualUnread{ false };
        // Transient (NOT persisted): turn accounting for the ordered state machine — see
        // TurnAccounting above / NextSessionStateOrdered (HookEvents.h). Reset on
        // SessionStart / SessionEnd (a resume must not inherit stale turn identity).
        TurnAccounting turns{};
        // Transient (NOT persisted): the latest assistant message text the interval reconciler
        // (SessionScanner) tailed from this session's transcript. Hooks don't carry assistant
        // output — this captures it (the foundation for a live Auto-Testing "peek"). Written via
        // the registry's QUIET path so streaming text never triggers a persist/UI/scheduler
        // cascade. Empty until the scanner reads a transcript line.
        std::wstring lastAssistantText;
        // Agentmaster (API-error triage — Transient, NOT persisted; Persistence.cpp must not write
        // them): the reason a turn DIED, preserved while state == Error so the Triage-Board Error card
        // (and any other surface) can show WHAT failed, not just a crimson dot. errorMessage is the
        // synthetic isApiErrorMessage line's text ("API Error: Server is temporarily limiting requests
        // … Rate limited" / "Prompt is too long" / "Credit balance is too low" / …); errorStatus is the
        // companion HTTP code from the transcript's apiErrorStatus (429 / 529 / 500 / 404 / 401), or 0
        // for a client-side error that carries none. Set by the SessionScanner's recon-error synth when
        // it produces Error, and CLEARED the moment the session leaves Error (recovery), both through
        // SessionRegistry::OnHookEvent. Empty/0 whenever the session is not in Error.
        std::wstring errorMessage;
        int errorStatus{ 0 };
        // Transient (NOT persisted; Persistence.cpp must not write it): the Claude Code idle RECAP —
        // the latest {"type":"system","subtype":"away_summary"} body the SessionScanner tailed from this
        // session's transcript, normalized (NormalizeRecapText: the "(disable recaps in /config)" hint
        // stripped). Claude Code emits it when a session sits idle >5 min: a one-paragraph "what we did /
        // what's next". Mirrored via the registry's QUIET path (display-only, no persist/UI/scheduler
        // cascade). Empty until a recap is seen (recaps off, or never idle). Shown in the Triage-Board
        // card's hover tooltip; the summary panel / Sessions detail / copy derive their own from
        // AnalyzeSessionTranscript (SessionSummary.awaySummary), the same data by a different path.
        std::wstring recap;
        // Transient (not persisted): in SemiAuto, the scheduler arms the next prompt here
        // and the Auto Testing shows a one-click confirm. Empty when nothing awaits confirm.
        std::wstring pendingConfirmPromptId;
        // Agentmaster (PENDING_INPUT.md): the session's UNSENT input-box DRAFT — text the user typed
        // into Claude's input box but has NOT yet submitted. Read out-of-band from the rendered terminal
        // buffer by the UI lane (TerminalPage::_ScanPendingInput -> ControlCore::ReadPendingInputDraft;
        // the bottom-most ❯ prompt line wrapped by ── rules, PendingInput.h). This is the ONE session
        // fact hooks can never carry — a draft is by definition not yet submitted, so no UserPromptSubmit
        // ever fires for it — so it is the lone screen-READ fact (analogous to the presence heartbeat;
        // never authoritative for SessionState, Rule #7/#13). Updated via the registry's QUIET,
        // change-gated SetPendingInput (the draft changes as the user types — like lastAssistantText it
        // must never trigger the persist / UI / scheduler cascade). Empty => no pending draft. Transient
        // (NOT persisted — Persistence.cpp must not write it). The future tab "unsent message" indicator
        // reads this.
        std::wstring pendingInput;

        // --- Fleet Observer live enrichment (OBSERVER.md §5c) ---
        // ALL transient (NOT persisted — Persistence.cpp must not write them; PIDs / WT_SESSION /
        // AM_SESSION / process facts are per-run and re-derived each launch by the observer). Filled
        // by SessionRegistry::ObserveClaude from the S-lane's out-of-band PEB read; provenance only,
        // never authoritative state (push hooks + the transcript tail own SessionState).
        uint32_t pid{}; // claude.exe PID (0 = unknown / not running here)
        std::wstring liveCwd; // PEB cwd (authoritative live dir; tracks `cd` across a relaunch)
        std::wstring model; // --model / CLAUDE_CODE_* (drives Manager/overlay adornments)
        std::wstring effort; // --effort / CLAUDE_CODE_EFFORT_LEVEL
        std::wstring permissionMode; // --permission-mode
        std::wstring sessionName; // CLAUDE_CODE_SESSION_NAME (bg jobs)
        bool background{}; // a background/daemon claude
        RunningApp runningApp{ RunningApp::Unknown }; // ours (Agentmaster) vs external (WindowsTerminal)
        std::wstring amSession; // owning Agentmaster instance stamp (empty if external)
        std::wstring ownerWindowId; // the window that hosts this session's tab (§19-Q1 attribution)
        // Claude's OWN self-reported heartbeat state from its presence file
        // (~/.claude/sessions/<pid>.json → "busy"/"idle"/"waiting"/"shell"; SESSIONS.md §7-Q5).
        // A display-only FACT fed by the S-lane (validated against pid liveness) — deliberately
        // NOT SessionState (push hooks + the transcript tail own state, Rule #13; STATE.md owns
        // any future promotion). Empty when no live presence file backs this session.
        std::wstring presenceStatus;
        bool hookWired{}; // have we received ANY hook for this id this run? (provenance)
        int64_t lastHookUnixMs{}; // last authoritative push (hook) — provenance vs the pull
        int64_t lastObservedUnixMs{}; // last pull observation (the S-lane survey)
        // Transcript-derived timing (the conversation's true age + last activity), filled by the
        // S-lane (ObserveClaude) from the transcript's ctime/mtime. Drives the Manager's per-session
        // timing adornment (created-ago / active-for / last-activity-ago). 0 until the transcript
        // exists. Transient — re-derived each run (NOT persisted; Persistence.cpp must not write them).
        int64_t convCreatedUnixMs{}; // transcript ctime (≈ conversation start)
        int64_t convLastActivityUnixMs{}; // transcript mtime (≈ last activity)
        // Context-window occupancy: the NEWEST assistant message's usage tokens
        // (input + cache_creation + cache_read + output ≈ the size of the last request = current
        // context size). Filled QUIETLY by the SessionScanner as it tail-reads the transcript;
        // displayed on the Manager board card as a RAW TOKEN COUNT (PR feedback: a % needs a context-
        // window denominator that can't be reliably inferred from the model id, so the raw count is
        // shown instead). 0 until the first assistant turn. Transient — re-derived each run (NOT persisted).
        int64_t contextTokens{};

        std::vector<QueuedPrompt> queue; // the Auto Testing
        AutorunnerState autorunner{};
    };

    // Agentmaster (the Triage-Board ⚡ "still server-cached" hint) — PURE + unit-tested. Claude's
    // server-side prompt cache stays warm ~cacheMinutes after the last REAL API request, so a
    // follow-up inside the window reuses the cached prefix (cheaper & faster). "Real API request"
    // is the operative phrase — the hint reads ONLY the two API-turn signals:
    //   • convLastActivityUnixMs — the transcript's LINE-DERIVED last activity (real user/assistant
    //     line timestamps; a `--resume`'s untimestamped trailer appends never move it), fed by the
    //     Fleet Observer. Note a fork/adopt of a recently-active conversation legitimately reads
    //     warm: the cache is PREFIX-keyed, so the duplicated history IS still cached for the new
    //     session — that ⚡ is a true positive.
    //   • lastTurnUnixMs — the hook-time turn evidence (IsApiTurnEvidence, HookEvents.h), which
    //     lights the hint the moment a prompt is submitted and covers a session whose transcript
    //     the observer can't resolve.
    // Deliberately NOT lastActivityUnixMs (the Waiting-for-you decay anchor): SessionStart at
    // launch / adopt / resume / /clear and the "Move to Waiting-for-you" triage promote stamp that
    // anchor "now" with zero API traffic — the reported ⚡ false positives ("shows right after
    // adopting / after Move to Waiting-for-you / on a never-prompted launch") this predicate
    // exists to exclude. Codex never shows the hint: the tooltip copy ("Claude's prompt cache")
    // and both signals are Claude-specific (a managed Codex has no hooks and its conv timing is
    // never fed), and before this gate a Codex card could ⚡ off a bare triage-move stamp.
    inline bool ServerCacheStillWarm(const SessionInfo& s, uint32_t cacheMinutes, int64_t nowMs) noexcept
    {
        if (!s.live || s.kind != AgentKind::Claude || cacheMinutes == 0)
        {
            return false;
        }
        const int64_t last = (s.convLastActivityUnixMs > s.lastTurnUnixMs) ? s.convLastActivityUnixMs : s.lastTurnUnixMs;
        return last > 0 && (nowMs - last) < static_cast<int64_t>(cacheMinutes) * 60000;
    }

    // A reusable plan (DESIGN §10 "Plans across the fleet"): a named sequence of prompts
    // that can be applied to any session or broadcast to many. Persisted separately.
    struct PlanTemplate
    {
        std::wstring name;
        std::vector<QueuedPrompt> prompts; // status is reset to Pending (with fresh ids) on apply
    };

    // Persisted geometry for the Manager tab's draggable pane splitters (C1 "Linked
    // Lenses"). Each value is the FIRST track's share of the two tracks its splitter
    // divides, kept within (0,1) — the second track takes the remainder. Defaults match the
    // original 2:3 star ratios so first run looks unchanged.
    struct ManagerLayout
    {
        double boardFraction{ 0.4 }; // Triage Board height / (Board + Bottom)      [root rows]
        double treeFraction{ 0.4 }; // Explorer Tree width / (Tree + Auto Testing)   [bottom cols]
    };

    // Global app settings — the Manager toolbar's Settings cog (next to "Pause Autorunner").
    // Every default reproduces the prior hardcoded behavior EXCEPT defaultAutorunnerMode (now
    // Full, the product default — "all new or opened sessions run on Autorunner"), so a missing
    // settings.json mostly changes nothing. Persisted to ~/.agentmaster/settings.json, loaded at
    // startup, and applied at two seams: the spawn recipe (Claude fields) and session OPEN — the
    // autorunner MODE is stamped onto EVERY opened session (new / adopted / restored); the other
    // autorunner backstops are stamped onto NEW sessions only. These are GLOBAL defaults/backstops;
    // per-session autorunner mode still lives in the Auto Testing (changeable after open).
    struct AppSettings
    {
        // --- Claude sessions (spawn recipe; see ClaudeSpawn) ---
        // ON  => spawn with --dangerously-skip-permissions (also skips the startup trust
        //        dialog). OFF => no flag; the settings file instead carries the "other
        //        variation" permissions.defaultMode:"default" (normal prompts + trust apply).
        bool skipPermissions{ true };
        // "" (As Is) => don't override the model. Else == what you'd type after `/model `
        //  (e.g. "opus" / "sonnet" / a full id) -> emitted as the settings `model` key.
        std::wstring model{};
        // false => emit includeCoAuthoredBy:false (drop Claude's commit/PR co-author byline).
        bool includeCoAuthoredBy{ true };
        // The GLOBAL extra environment injected into EVERY spawned session (Claude AND Codex), edited
        // in the Settings cog's "Environment variables" area (Global tab) as a multi-line NAME=VALUE
        // list (one per line; an old ';'-delimited single line still parses). Parsed by
        // ParseEnvAssignments; merged with the PER-DIRECTORY overrides (dir-env.json, NOT stored here —
        // it's keyed by working dir) at spawn via MergeSessionEnv/ResolveSessionEnv, per-dir winning.
        // CCMGR_* names are dropped (reserved for hook correlation); AM_SESSION/WT_SESSION are set by
        // the connection and a user value is ignored.
        std::wstring env{};
        // Agentmaster (native-exe-only policy): an explicit override path to the native claude.exe.
        // "" => auto-detect (PATH claude.exe, ~/.local/bin\claude.exe, or a claude.cmd's npm binary —
        // see ResolveClaudeExe). When set it MUST be an existing *.exe (the Settings UI validates +
        // browses for it). The whole app launches/forks/resumes ONLY when a native claude.exe resolves;
        // otherwise every claude interaction is gated behind an "install the native build" prompt. A
        // pure-Node `claude` (no native binary) is deliberately unsupported (the Fleet Observer + PEB
        // enrichment are all claude.exe-keyed).
        std::wstring claudeExePath{};

        // --- Autorunner defaults ---
        // The MODE seeds every OPENED session — new, adopted, AND restored/window-restored (a
        // reopened session is re-armed on this default, OVERRIDING its saved per-session mode),
        // and stays changeable afterward (the Auto Testing toggle / this cog). Default Full ==
        // "all new or opened sessions run on Autorunner". The backstops below are stamped onto
        // NEW sessions only; a restored one keeps its persisted maxAutoSends/stopOnError/pauseOnHumanInput.
        AutorunnerMode defaultAutorunnerMode{ AutorunnerMode::Full };
        uint32_t maxAutoSends{ 100 }; // runaway backstop
        bool stopOnError{ true }; // pause a plan when a turn ends in error
        bool pauseOnHumanInput{ true }; // suspend auto-send while the human is typing

        // --- Behavior sugar ---
        bool confirmBeforeKill{ true }; // confirm before killing a session from the Manager
        // How the tab/session rename box commits via the keyboard (focus-loss always commits). GLOBAL
        // across windows. Default = Shift+Enter commits while a plain Enter still inserts a newline
        // (titles are multi-line). See TabRenameCommitMode.
        TabRenameCommitMode tabRenameCommitMode{ TabRenameCommitMode::ClickAwayOrShiftEnter };
        std::wstring defaultLaunchDir{}; // "" => the Launch cwd box defaults to %USERPROFILE%
        // Agentmaster (Waiting-for-you "unread" model): how long a session may sit in the Triage
        // Board's "Waiting-for-you" column before the time-decay is allowed to demote it to Idle.
        // The decay is gated on BOTH this timeout AND read-state: a WaitingForInput session demotes
        // to Idle only once (a) this many minutes have passed with no activity AND (b) the user has
        // READ it (visited its tab) since the last turn — an unread, past-timeout session keeps
        // waiting until read; a manually "Mark Unread"-ed session never time-decays at all. Enforced
        // by the SessionScanner (ShouldDecayWaitingToIdle). 0 == never decay (the cog's "Never"
        // toggle). Default 4320 (3 days). The OLD conflation of this with Claude's ~5-minute server
        // cache is split out into serverCacheMinutes (below), which now drives only the card's
        // "still cached" ⚡ indicator. The cog exposes a SLIDER of 1m .. 7d (1..10080) beside a
        // free-text "12h5m" box that may exceed 7d (the slider then sits maxed), plus a "Never" toggle.
        // NOTE: this was renamed from the legacy "waitingDecayMinutes" key DELIBERATELY — the meaning
        // changed (a 5-minute cache window -> a read-gated unread timeout), so a pre-existing
        // settings.json (which carried a value tuned for the old behavior, often 5) must NOT carry
        // over. The new key is absent there, so every existing install falls back to this 4320 default;
        // the orphaned old key is ignored and dropped on the next save (Persistence rebuilds the file).
        uint32_t waitingForYouTimeoutMinutes{ 4320 };
        // Agentmaster: Claude's SERVER-SIDE prompt-cache lifetime, in minutes (Anthropic caches the
        // prompt prefix ~5 minutes after the last turn, so a follow-up within the window is cheap).
        // Drives ONLY the Triage-Board card's "still cached" ⚡ indicator (shown for this many minutes
        // after a session's last activity) — it no longer gates the Waiting-for-you decay (that is
        // waitingForYouTimeoutMinutes). Default 5. 0 falls back to 5 (the indicator is a cosmetic hint).
        uint32_t serverCacheMinutes{ 5 };
        // How many recent working directories the Launch path-picker's "RECENT" section
        // remembers (in recent-dirs.json) and lists. Default 10. (0/garbage falls back to 10.)
        uint32_t recentDirsLimit{ 10 };
        // Agentmaster (bookmark tags): how many DISTINCT tags may exist GLOBALLY (the tag universe
        // is the union of every session's SessionStore "tags" lists — a tag lives while >=1 session
        // carries it). The cap gates only the creation of a NEW tag name from the tab menu's Tag
        // panel; tags already applied are never dropped by lowering it. Default 20, configurable in
        // the cog up to the hard ceiling 40 (ClampMaxTags below — load + save both clamp).
        uint32_t maxTags{ 20 };
        // Agentmaster (bookmark tags): the OPACITY of the bookmark-tag chips rendered in the rich
        // tab TOOLTIP (the name + its colored underscore), 0..1. A cog slider (10..100%); default
        // 0.9 (90% solid) — a hair of translucency so a dense tag row reads as secondary to the
        // title/state above it without disappearing. ONLY the tooltip's tag row honors it (the
        // tab-strip badge ribbons + the Sessions column stay fully opaque — they're the primary
        // affordance). ClampTooltipTagsOpacity below bands it on load + save (0/absent/garbage ->
        // 0.9), so a missing key reproduces the default.
        double tooltipTagsOpacity{ 0.9 };

        // --- Tab strip ---
        // Agentmaster: show the close (x) button on terminal tabs. ON (default) keeps the theme's
        // showCloseButton policy (Always / Hover / etc.); OFF forces every tab to "Never" — hiding
        // the X entirely (the pinned Manager tab is always X-less regardless). GLOBAL across windows;
        // applied live via _updateAllTabCloseButtons on Save + cross-window broadcast. Default true
        // reproduces prior behavior (theme-driven).
        bool showTabCloseButton{ true };
        // Agentmaster: close a tab when it is clicked with the MIDDLE mouse button. ON (default) is the
        // long-standing Windows Terminal behavior; OFF disables it on BOTH paths — the manual hook used
        // when the X is hidden (TerminalPage::_OnTabPointerPressed) AND WinUI's native middle-click
        // close when the X is shown (suppressed in TerminalPage::_OnTabCloseRequested). GLOBAL across
        // windows; applied live like showTabCloseButton. Default true reproduces prior behavior.
        bool closeTabOnMiddleClick{ true };
        // Agentmaster: ALWAYS show the tab-strip "Home" button (the affordance that jumps to the pinned
        // Manager tab), not only when the Manager tab has scrolled out of view. ON (default) keeps a
        // persistent Home button in the strip header whenever you're on a non-Manager tab; OFF restores
        // the scroll-triggered behavior (Home appears only once the Manager tab is scrolled off the left
        // edge). Either way it hides while the Manager tab itself is active. GLOBAL across windows;
        // applied live on Save + cross-window broadcast. A missing key => true (checked by default).
        bool alwaysShowHomeButton{ true };
        // Agentmaster (FAVORITES.md §5a): the FAVORITE marker glyph on a live session's tab — Crown
        // (default, a gold crown at the status dot's north-west) or Star (the status dot foregrounded
        // on a white, golden-tipped star drawn behind it). GLOBAL across windows; applied live on Save
        // + cross-window broadcast (every hosted favorited tab is re-asserted). A missing key => Crown
        // (the prior behavior). Tab-strip ONLY — see FavoriteIcon.
        FavoriteIcon favoriteIcon{ FavoriteIcon::Crown };
        // Agentmaster (tab color modes): HOW managed tabs are colored — shared per working
        // directory (default, Rule #12 classic), individual per tab/session, or shared per the
        // INFERRED working directory (detected from the files the session reads/edits/creates).
        // See TabColorMode. GLOBAL across windows; applied live on Save + cross-window broadcast
        // (TerminalPage::_ReapplyManagedTabColors repaints every hosted managed tab). A missing
        // key => WorkingDirectory (the prior behavior).
        TabColorMode tabColorMode{ TabColorMode::WorkingDirectory };
        // Agentmaster (tab color modes — "Use .git folder to infer"): whether the inferred-workdir
        // detection SNAPS to the enclosing git repository root. ON (default): each tool-touched
        // path resolves to its nearest ancestor holding a `.git` entry (dir, or a worktree /
        // submodule FILE — the nearest wins, so worktree work keys the worktree, not the outer
        // checkout), and a git root carrying the strict majority of the votes IS the inferred dir
        // — never a subfolder of it. The repo is the working area: a session concentrated in
        // `repo\src\x` then infers `repo`, which normally equals its launch cwd, so the inferred
        // mode agrees with shared-per-directory coloring for in-repo sessions (switching modes
        // doesn't recolor). No git majority (or OFF) => the plain deepest-majority directory vote
        // (InferWorkingDirectory / FindGitRootForDir). Only consulted while tabColorMode is
        // InferredWorkingDirectory; a change clears the scan gate so live sessions re-infer.
        // GLOBAL; persisted "inferGitRoot"; a missing key => ON.
        bool inferGitRoot{ true };
        // Agentmaster (status-dot RED FLASH RING color): the COLOR — with OPACITY in the alpha byte —
        // of the "unread" ring that pulses around a managed session's tab status dot when it leaves
        // Running for a needs-you state (Idle / WaitingForInput / NeedsApproval) on an unvisited tab
        // (TerminalPage::_EvaluateAgentFlash), and of the manual "Mark Unread" ring. Stored as an
        // "#AARRGGBB" hex string — the leading ALPHA byte is the ring's opacity, so the Settings cog's
        // color picker (alpha slider enabled) drives BOTH hue and opacity in one control; a legacy
        // "#RRGGBB" (no alpha) is read as fully opaque. GLOBAL across windows; applied live on Save +
        // cross-window broadcast (each window re-points its shared flash-ring brush). Default
        // "#CCFF0000" == red at 80% opacity (alpha 0xCC) — a slightly softer ring than a fully-opaque
        // one. A malformed / empty value falls back to that default at the (UI-layer) parse, never
        // wedging the ring.
        std::wstring flashRingColor{ L"#CCFF0000" };

        // Agentmaster (PENDING_INPUT.md): the unsent-draft "3 dots" indicator color, kept as a CONTRAST
        // PAIR so the dots are never invisible against the tab/card they ride on. The dots are painted the
        // LIGHT color on a DARK background and the DARK color on a LIGHT one — the algorithm picks by the
        // session's per-directory tab color (Rule #12) via BackgroundIsLight / PendingDotsColorFor
        // (AgentStatusColors.h, the WCAG luminance crossover). Both "#AARRGGBB" (the alpha byte is honored,
        // atop the dots' own opacity pulse); GLOBAL across windows, applied live on Save + the cross-window
        // broadcast (the flashRingColor idiom — the tab-strip dots re-read these on the next scan tick, the
        // board cards on the next rebuild). Defaults reproduce the prior single hardcoded gold shown on a
        // dark background (light = #FFE0A92B) and add a deep amber that reads on a light one (dark =
        // #FF5A3E00). A malformed/empty value falls back to its default at the (UI-layer) parse.
        std::wstring pendingDotsLightColor{ L"#FFE0A92B" }; // shown on a DARK tab/card background
        std::wstring pendingDotsDarkColor{ L"#FF5A3E00" }; // shown on a LIGHT tab/card background

        // Agentmaster (TAB_OVERLAY.md): show the per-tab "link badge" overlay pinned to the
        // top-right of each Claude session's terminal (status + autorunner mode + queued count +
        // link state). Default ON; a missing key => true (a no-op default, like the rest).
        bool showTabOverlay{ true };
        // Agentmaster (TAB_OVERLAY.md): the per-tab overlay (badge + its summary panel) sits dim at REST
        // and brightens to HOVER opacity on pointer-over / while the copy menu is open. Both are
        // user-configurable via the cog's TABS "Overlay opacity" dual-thumb slider (rest = left/transparent
        // dot, hover = right/solid dot). GLOBAL across windows, applied live on Save + cross-window
        // broadcast. INVARIANT: rest <= hover (the slider can't cross the dots; enforced again at load).
        // Defaults reproduce the prior hardcoded look (rest 0.50, hover 1.0).
        double tabOverlayRestOpacity{ 0.50 };
        double tabOverlayHoverOpacity{ 1.0 };
        // Agentmaster (TAB_OVERLAY.md summary panel): whether the per-tab SUMMARY PANEL (the 2nd
        // overlay, toggled by the badge's pencil button) is shown. GLOBAL across windows — like
        // showTabOverlay/treeSort it lives here in settings.json, NOT per-session: the pencil on any
        // tab flips this one value (a freshest-disk read-modify-write), every linked overlay in the
        // window applies it live, and it seeds every window on launch. Default ON (panel shown).
        bool showSummaryPanel{ true };
        // Agentmaster (TAB_OVERLAY.md summary panel): the panel's SIZE, stored as FRACTIONS of the
        // pane (so it scales with the window — the archiveSplitFraction idiom). The panel is anchored
        // top-right; the user drags its LEFT edge (width), BOTTOM edge (height), or BOTTOM-LEFT corner
        // (both). 0 == "auto" — the original look (width capped at 20% of the pane, height content-
        // driven up to a modest cap); a drag pins an explicit fraction. Width is clamped to (0.08, 0.5)
        // — at most HALF the pane wide; height to (0.06, 0.75) — at most THREE-QUARTERS of the pane
        // tall. GLOBAL like showSummaryPanel: written by the resize grips (freshest-disk RMW),
        // broadcast live to every linked overlay in the window, and seeds every window on launch.
        // Default 0 (a no-op default — the pre-resize behavior).
        double summaryPanelWidthFraction{ 0.0 };
        double summaryPanelHeightFraction{ 0.0 };
        // Agentmaster (TAB_OVERLAY.md summary panel): how the panel renders newlines INSIDE a message.
        // false (default) == the session-end.js look: each message is collapsed to ONE line, real
        // newlines escaped to a literal "\n" (and tabs to "\t"). true == preserve the message's real
        // newlines so a multi-line prompt reads as multiple lines. GLOBAL like showSummaryPanel — toggled
        // by the wrap-line icon at the right of the panel's times bar (a freshest-disk RMW), broadcast
        // live to every linked overlay in the window, and seeds every window on launch.
        bool summaryPanelWrapNewlines{ false };
        // Agentmaster (TAB_OVERLAY.md summary panel): TRUNCATE long messages in the numbered list. ON
        // (default) limits each message — to 6 lines when wrap is on (a 7th+ line collapses to "..."),
        // or to 500 characters when wrap is off; OFF shows EVERYTHING. GLOBAL like summaryPanelWrapNewlines:
        // written by the truncate toggle (left of the wrap toggle) in the panel's times bar, a freshest-
        // disk RMW, broadcast live to every linked overlay. Default true (truncate long messages).
        bool summaryPanelTruncate{ true };
        // Agentmaster (conversation lineage): SHOW PREVIOUS SESSION(S) in the summary panel — when a
        // session was `/compact`ed, the earlier (pre-compaction) segments are rendered ABOVE the current
        // Messages, each numbered + labeled. GLOBAL like the wrap/truncate toggles (written by the
        // previous-session toggle in the panel's times bar, freshest-disk RMW, broadcast live). Default
        // false (hidden; the toggle only appears when the session actually has a previous segment).
        bool summaryPanelShowPrevious{ false };
        // Agentmaster: Explorer Tree sort order (the toggle after the scope toggle). GLOBAL — it
        // applies to every window's tree and persists here. Default Newest. See ExplorerSort.
        ExplorerSort treeSort{ ExplorerSort::Newest };
        // Agentmaster: Triage Board sort order (the toggle after the board's LOCAL/GLOBAL scope toggle)
        // — orders the cards WITHIN each state column. A SEPARATE global setting from treeSort, so the
        // board and the tree sort independently and each remembers its own choice. GLOBAL like treeSort:
        // shared by every window, persisted here, survives restart (the changing window re-sorts live;
        // others adopt it on next launch). Default MostActive == most-recently-active first. The board
        // cycles MostActive/Newest/Oldest/Alpha — the tree's set minus ByPid (pid grouping is meaningless
        // once cards are split across state columns).
        ExplorerSort boardSort{ ExplorerSort::MostActive };
        // Agentmaster: which tab the Manager's FLIGHT-PLAN pane shows. Its top line is a two-state
        // [Summary | Auto Testing] segmented toggle; true == the (currently empty) Summary tab is
        // selected, false == the Auto Testing tab (the prompt queue / Autorunner / compose box). GLOBAL
        // across windows like treeSort/boardSort: a click in any window persists it here + broadcasts,
        // so every window's pane shows the same tab and the choice survives restart. Default true
        // (Summary). NOTE: distinct from showSummaryPanel — that is the per-tab OVERLAY's pencil-toggled
        // summary panel (TAB_OVERLAY.md); this is the Manager Auto-Testing pane's tab selection.
        bool autoTestingShowsSummary{ true };
        // Agentmaster: the Archive page's table|detail splitter position — the TABLE's share of
        // the two columns, kept within (0.05, 0.95). Applied as STAR ratios, so the split scales
        // with the window (window-size-relative, not pixels). GLOBAL like treeSort: written by
        // the splitter itself on drag release (read-modify-write of settings.json, not the cog),
        // seeded into every window's Archive shell. Default 0.5 == the original 50/50 split.
        double archiveSplitFraction{ 0.5 };
        // Agentmaster (updater; Updater.h): the in-app GitHub-release updater's state. allowUpdatePrerelease
        // is the Settings cog's "Allow updating to pre-release versions" toggle (default OFF — the check
        // uses /releases/latest, which excludes prereleases; ON uses the list endpoint, newest published).
        // updateSkippedVersion / updatePostponedUntilUnixMs are written OUTSIDE the cog form by the updater
        // (a freshest-disk JSON read-modify-write that the WindowsTerminal EXE can do without linking the
        // engine — see Updater.h), so the cog's Save PRESERVES them from disk like the summary-panel fields:
        // skip == the exact tag the user chose to "Skip this version" (never re-prompt for it); postpone ==
        // the epoch-ms before which no check/prompt fires ("Remind me in 3/7/30 days"). All three default to
        // a no-op (prompt normally, nothing skipped, not postponed) so a missing settings.json changes nothing.
        bool allowUpdatePrerelease{ false };
        std::wstring updateSkippedVersion{};
        int64_t updatePostponedUntilUnixMs{ 0 };
        // Agentmaster (Sessions page; SESSIONS.md): the session ids the user chose to HIDE from the
        // global Sessions browser ("Hide from list" on a row's right-click menu). Persisted here so
        // a hide sticks across restarts; cleared from the Settings cog's "Reset hidden sessions"
        // (both paths a freshest-disk read-modify-write of settings.json). The Sessions page filters
        // these out of its table; nothing else reads the list — it is purely a browse-list
        // preference, never a lifecycle action (a hidden session is untouched on disk).
        std::vector<std::wstring> hiddenSessionIds{};

        // --- shipped-default seeding markers (ENV_VARS.md §8; NOT shown in the cog) ---
        // Agentmaster ships a few defaults ONCE and then respects user edits/removals. These markers
        // record that the one-time seed ran, so a default a user deletes never returns:
        //  * envDefaultsVersion — the highest kEnvDefaultsVersion already seeded into `env`. 0 == none yet
        //    (a new install OR a pre-feature settings.json), so the seed runs once and bumps it. See
        //    ApplyEnvDefaults / SeedSessionEnvDefaults. v1 seeds CLAUDE_CODE_MAX_RETRIES=50000.
        //  * claudeCleanupDaysSeeded — whether we've already written the default cleanupPeriodDays=36500
        //    into the user's GLOBAL ~/.claude/settings.json (so Claude never purges global history). Only
        //    seeded when the user hasn't set it themselves; never re-seeded after they change/remove it.
        //    See SeedClaudeCleanupPeriodDaysIfNeeded. (cleanupPeriodDays itself lives in Claude's file, not
        //    here — it's editable from the cog's "Keep Claude history (days)" field via the repository.)
        uint32_t envDefaultsVersion{ 0 };
        bool claudeCleanupDaysSeeded{ false };
    };

    // Agentmaster (bookmark tags): clamp AppSettings::maxTags into its valid band — the cog allows
    // 1..40 (hard ceiling 40); 0/absent/garbage falls back to the 20 default. Shared by the
    // Persistence load and the cog Save so a hand-edited settings.json self-heals identically.
    inline uint32_t ClampMaxTags(uint32_t v)
    {
        if (v < 1)
        {
            return 20;
        }
        return v > 40 ? 40 : v;
    }

    // Agentmaster (bookmark tags): clamp AppSettings::tooltipTagsOpacity into 0.1..1.0 — the cog
    // slider's 10..100% band. A non-finite / <=0 value (absent key, hand-edit) falls back to the
    // 0.9 default; anything above 1 pins to fully solid. Shared by the Persistence load and the cog
    // Save so a hand-edited settings.json self-heals identically (the ClampMaxTags idiom).
    inline double ClampTooltipTagsOpacity(double v)
    {
        if (!(v > 0.0)) // NaN or <= 0 -> the default
        {
            return 0.9;
        }
        if (v < 0.1)
        {
            return 0.1;
        }
        return v > 1.0 ? 1.0 : v;
    }

    // ===== Workspace persistence (M10; see doc/agentmaster/PERSISTENCE.md) =====
    // The WindowEmperor runs every window in one process, so window-level UI state is
    // per-WINDOW: each window owns a WindowRecord — geometry + Manager-tab lens + ORDERED tab
    // refs — stored as windows/<windowId>.json. This is a THIN layer OVER the session-archive
    // model: sessions.json + SessionInfo.live stay the single source of truth for the fleet
    // (lifecycle, queues, open/archived). The WindowRecord never duplicates session data — a
    // Claude tab here is just the session's id (tab order + window<->session affinity) — so a
    // window re-applies its geometry/lens and re-homes its sessions without a second copy.

    // A window's position/size/launch-mode, captured from WT's WindowLayout and re-applied by
    // us on restore. The `has*` flags distinguish "unset" from a real 0 (e.g. a window at the
    // top-left corner). launchMode is WT's LaunchMode serialized as its JSON token
    // ("default"/"maximized"/"focus"/...); empty => default.
    struct WindowGeometry
    {
        bool hasPosition{ false };
        double x{ 0 };
        double y{ 0 };
        bool hasSize{ false };
        double width{ 0 };
        double height{ 0 };
        std::wstring launchMode;
    };

    enum class TabKind
    {
        Claude, // a Manager-owned claude.exe session — restored via `claude --resume <convId>`
        Codex, // a Manager-owned codex.exe session — restored via `codex resume <rolloutUuid>` (the uuid lives on the referenced SessionInfo.codexSessionId; sessionId here is our durable handle)
        Other, // any other WT tab — restored by replaying its stored ActionAndArgs JSON
    };

    // One ordered tab inside a window — a REFERENCE, not a copy. A Claude tab stores only the
    // session's id (its full record — queue, autorunner, live/archived — lives once in
    // sessions.json); `tabColor` is the optional "#RRGGBB" we paint it. An Other tab is opaque:
    // `actionsJson` is the WT ActionAndArgs (NewTab + SetTabColor + RenameTab) that recreates
    // it, so its own color/title ride along inside that blob. This records tab ORDER + the
    // window<->session affinity without duplicating any session data.
    struct TabEntry
    {
        TabKind kind{ TabKind::Claude };
        std::wstring sessionId; // valid when kind == Claude — references the session in sessions.json
        std::wstring tabColor; // optional "#RRGGBB" (Claude tabs); empty => none
        std::wstring actionsJson; // valid when kind == Other (opaque WT ActionAndArgs)
    };

    // A window's Manager-tab lens — the per-window VIEW over the one shared fleet (selection,
    // directory scope, which prompt is selected, which dir rows are collapsed, splitter sizes).
    // These are view preferences; the fleet itself lives in the shared SessionRegistry.
    struct ManagerState
    {
        std::wstring selectedId; // selected session card
        std::wstring scopeDir; // Explorer-tree directory scope ("" => all)
        std::wstring selectedPromptId; // selected Auto-Testing row
        std::vector<std::wstring> collapsedDirs; // Explorer-tree dirs the user collapsed
        ManagerLayout layout{}; // splitter fractions (was the global layout.json; now per-window)
        // Agentmaster: the ONE session scope behind BOTH toggles — the Explorer Tree's 3-way cycle
        // (LOCAL/GLOBAL/EXTERNAL) and the Triage Board's 2-way LOCAL/GLOBAL (the board has no
        // External mode; it reads External as Global). 0=LOCAL 1=GLOBAL 2=EXTERNAL. Persisted per
        // window in the lens so a reopened window keeps its scope; absent in an older record => 0
        // (LOCAL, the prior in-memory default).
        int treeScope{ 0 };
    };

    // Per-window UI state: geometry + lens + ordered tab refs. One file per window
    // (windows/<windowId>.json) so opening/closing windows never contend on a single document.
    // NOT the session source of truth — that is sessions.json (the archive model).
    struct WindowRecord
    {
        std::wstring windowId; // stable GUID, generated once per window and embedded in its Manager tab
        WindowGeometry geometry;
        std::vector<TabEntry> tabs; // ORDERED (left-to-right)
        // The focused tab, persisted by STABLE IDENTITY so it survives a restart (positions/indices
        // shift between runs — a session id does not). A Claude tab => selectedSessionId (the
        // conversation id, the same stable ref sessions.json uses). A non-Claude (shell) tab has no
        // cross-restart id (its WT session GUID is regenerated each run), so it falls back to
        // selectedTabIndex (an index into `tabs`). Manager / none => both unset (empty / -1); the
        // Manager tab is re-created at index 0, the natural default. On reopen the window re-selects this
        // tab so closing/reopening preserves the ACTIVE tab, not just the set of tabs.
        std::wstring selectedSessionId; // selected Claude tab's conversation id (stable); empty otherwise
        int selectedTabIndex{ -1 }; // fallback for a non-Claude selected tab: index into `tabs`; -1 = Manager/none
        ManagerState manager;
        // Agentmaster: the pinned Manager tab's user-chosen color ("#RRGGBB"; empty => none/default),
        // persisted PER WINDOW. Unlike a session tab's color (ONE value per working directory, Rule #12,
        // owned by dir-colors.json), the Manager tab is a per-window singleton with no dir — so its color
        // rides here in the window record, letting each Agentmaster window's home tab be color-coded
        // independently. Empty/absent (an older record) => no override (the default tab look).
        std::wstring managerTabColor;
    };
}
