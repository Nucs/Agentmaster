// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — core data model for the Manager tab (Design A / C1 / Flight Plan).
// Plain C++ (no WinRT projection) so it compiles standalone; the XAML layer (M6) wraps
// these in observable view-models. See doc/agentmaster/IMPLEMENTATION.md.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
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

    enum class AutopilotMode
    {
        Off,
        SemiAuto, // confirm each auto-send (recommended for the first N sends)
        Full // auto-send with no confirmation
    };

    // Agentmaster: Explorer Tree ordering — the sort toggle after the LOCAL/GLOBAL/EXTERNAL scope
    // toggle. Orders BOTH the directory groups and the rows within each (and the EXTERNAL census).
    // A GLOBAL app setting (AppSettings::treeSort), persisted to settings.json so the choice is
    // shared by every window and survives restart.
    enum class ExplorerSort
    {
        Newest, // most recently created first (conversation ctime, desc); a fresh/never-prompted session floats up
        Oldest, // oldest created first (ctime, asc)
        MostActive, // most recent activity first; a currently-running session ranks at the very top
        Alpha, // A->Z by title (case-insensitive)
        ByPid // group by host window/shell pid (externals: ExternalClaudeRow::hostPid; managed: the claude pid), then by most active within each group
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
        Held, // a guard blocked auto-send (e.g., the agent asked a question)
        Skipped,
        Failed
    };

    // How a prompt entered the Flight Plan. The Flight Plan reflects EVERY message a session
    // received (DESIGN: "all messages user sent, not only via the flight plan"), so a prompt
    // the human typed straight into the ConPTY — captured from the UserPromptSubmit hook — is
    // recorded too, tagged `Typed`, alongside the `Flight` prompts we queued/injected.
    enum class PromptOrigin
    {
        Flight, // queued and injected through the Flight Plan (our send)
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
        PromptOrigin origin{ PromptOrigin::Flight }; // Flight (we sent it) vs Typed (human typed it)
        // Transient (NOT persisted): a Flight prompt we just injected expects ONE
        // UserPromptSubmit echo back; `echoed` marks that echo consumed so the registry does
        // not re-record our own injection as a `Typed` message. Reset to false at each send.
        bool echoed{ false };
    };

    struct ApprovalPolicy
    {
        bool pauseForHuman{ true }; // default: do not auto-approve tool permissions
        std::vector<std::wstring> autoApproveTools; // allowlist of safe tools
    };

    struct AutopilotState
    {
        AutopilotMode mode{ AutopilotMode::Off };
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
        int64_t lastActivityUnixMs{ 0 };
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
        // arrives (and after a fresh load, until the session re-emits one).
        std::wstring tabToken;
        // Transient runtime flag (NOT persisted): is this session OPEN (has a live tab +
        // claude.exe this run) or ARCHIVED (shut down but kept restorable)? The Triage Board /
        // Explorer Tree show only Open (live) sessions; Archived (!live) ones are listed behind
        // the Manager's "Archived" button and can be restored on demand. Set true when we Launch
        // / restore / adopt a session, false when the user archives it (closes its tab). Always
        // false on load (Correctness Rule #6: startup re-opens NOTHING — every persisted session
        // comes back Archived, restorable as a whole), so it never needs to round-trip to JSON.
        bool live{ false };
        // Transient runtime flag (not persisted): set from the most recent Stop hook's
        // best-effort `lastMessageIsQuestion`. Feeds the Autopilot question-guard (M7):
        // a turn that ended on a clarifying question must NOT be auto-answered.
        bool lastMessageWasQuestion{ false };
        // Transient (NOT persisted): turn accounting for the ordered state machine — see
        // TurnAccounting above / NextSessionStateOrdered (HookEvents.h). Reset on
        // SessionStart / SessionEnd (a resume must not inherit stale turn identity).
        TurnAccounting turns{};
        // Transient (NOT persisted): the latest assistant message text the interval reconciler
        // (SessionScanner) tailed from this session's transcript. Hooks don't carry assistant
        // output — this captures it (the foundation for a live Flight-Plan "peek"). Written via
        // the registry's QUIET path so streaming text never triggers a persist/UI/scheduler
        // cascade. Empty until the scanner reads a transcript line.
        std::wstring lastAssistantText;
        // Transient (not persisted): in SemiAuto, the scheduler arms the next prompt here
        // and the Flight Plan shows a one-click confirm. Empty when nothing awaits confirm.
        std::wstring pendingConfirmPromptId;

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

        std::vector<QueuedPrompt> queue; // the Flight Plan
        AutopilotState autopilot{};
    };

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
        double treeFraction{ 0.4 }; // Explorer Tree width / (Tree + Flight Plan)   [bottom cols]
    };

    // Global app settings — the Manager toolbar's Settings cog (next to "Pause Autopilot").
    // Every default reproduces the prior hardcoded behavior, so a missing settings.json (or
    // any unset field) changes nothing. Persisted to ~/.agentmaster/settings.json, loaded at
    // startup, and applied at two seams: the spawn recipe (Claude fields) and new-session
    // creation (autopilot defaults are stamped onto the session's AutopilotState). These are
    // GLOBAL defaults/backstops; per-session autopilot mode still lives in the Flight Plan.
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
        // Extra environment variables injected into EVERY spawned session, edited as a single
        // ';'-delimited list of NAME=VALUE pairs (e.g. "FOO=bar;HTTPS_PROXY=http://h:8080").
        // Parsed by ParseEnvAssignments at spawn; CCMGR_* names are ignored (reserved).
        std::wstring env{};
        // Agentmaster (native-exe-only policy): an explicit override path to the native claude.exe.
        // "" => auto-detect (PATH claude.exe, ~/.local/bin\claude.exe, or a claude.cmd's npm binary —
        // see ResolveClaudeExe). When set it MUST be an existing *.exe (the Settings UI validates +
        // browses for it). The whole app launches/forks/resumes ONLY when a native claude.exe resolves;
        // otherwise every claude interaction is gated behind an "install the native build" prompt. A
        // pure-Node `claude` (no native binary) is deliberately unsupported (the Fleet Observer + PEB
        // enrichment are all claude.exe-keyed).
        std::wstring claudeExePath{};

        // --- Autopilot defaults stamped onto NEW sessions (not restored ones) ---
        AutopilotMode defaultAutopilotMode{ AutopilotMode::Off };
        uint32_t maxAutoSends{ 100 }; // runaway backstop
        bool stopOnError{ true }; // pause a plan when a turn ends in error
        bool pauseOnHumanInput{ true }; // suspend auto-send while the human is typing

        // --- Behavior sugar ---
        bool confirmBeforeKill{ true }; // confirm before killing a session from the Manager
        std::wstring defaultLaunchDir{}; // "" => the Launch cwd box defaults to %USERPROFILE%
        // Agentmaster: demote a session sitting in WaitingForInput (the Triage Board's
        // "Waiting-for-you" column) to Idle after this many minutes with no activity. Rationale:
        // Claude's SERVER-SIDE prompt cache expires ~5 minutes after the last turn, so past that
        // window the session is no longer "hot" — answering it costs a full cache re-read either
        // way — and Waiting-for-you should only surface sessions worth answering NOW. Enforced by
        // the SessionScanner (a time-derived decay layered on the hook-derived machine; the card
        // moves to the "Idle / Done" column). 0 == never decay. Default 5 == the cache lifetime.
        uint32_t waitingDecayMinutes{ 5 };
        // How many recent working directories the Launch path-picker's "RECENT" section
        // remembers (in recent-dirs.json) and lists. Default 10. (0/garbage falls back to 10.)
        uint32_t recentDirsLimit{ 10 };
        // Agentmaster (TAB_OVERLAY.md): show the per-tab "link badge" overlay pinned to the
        // top-right of each Claude session's terminal (status + autopilot mode + queued count +
        // link state). Default ON; a missing key => true (a no-op default, like the rest).
        bool showTabOverlay{ true };
        // Agentmaster: Explorer Tree sort order (the toggle after the scope toggle). GLOBAL — it
        // applies to every window's tree and persists here. Default Newest. See ExplorerSort.
        ExplorerSort treeSort{ ExplorerSort::Newest };
        // Agentmaster: the Archive page's table|detail splitter position — the TABLE's share of
        // the two columns, kept within (0.05, 0.95). Applied as STAR ratios, so the split scales
        // with the window (window-size-relative, not pixels). GLOBAL like treeSort: written by
        // the splitter itself on drag release (read-modify-write of settings.json, not the cog),
        // seeded into every window's Archive shell. Default 0.5 == the original 50/50 split.
        double archiveSplitFraction{ 0.5 };
        // Agentmaster (Sessions page; SESSIONS.md): the session ids the user chose to HIDE from the
        // global Sessions browser ("Hide from list" on a row's right-click menu). Persisted here so
        // a hide sticks across restarts; cleared from the Settings cog's "Reset hidden sessions"
        // (both paths a freshest-disk read-modify-write of settings.json). The Sessions page filters
        // these out of its table; nothing else reads the list — it is purely a browse-list
        // preference, never a lifecycle action (a hidden session is untouched on disk).
        std::vector<std::wstring> hiddenSessionIds{};
    };

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
    // session's id (its full record — queue, autopilot, live/archived — lives once in
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
        std::wstring selectedPromptId; // selected Flight-Plan row
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
    };
}
