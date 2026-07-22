# Agentmaster

> A fork of **Windows Terminal** (`microsoft/terminal`, MIT) that turns it into a manager
> for multiple **Claude Code** sessions.

## Development Rules

- Do not build or deploy or install without the user's permission.
- Do commit changes once done — a single `git add` + `git commit` with an extensive
  message (see *Commit Instructions* in the global guidance). Committing needs no
  permission; only build/deploy/install do.

## What we're doing & our aim

**Aim:** one Windows Terminal–based app that manages **N Claude Code sessions across M
working directories** — letting you **fully interact** with each session *and* giving the
app **100% programmatic control** (inject prompts, read output, drive state).

Each session is a real `claude.exe` on a **ConPTY** connection: a full-fidelity terminal
with a shared stdin (you and the orchestrator coexist), output tapped for the UI, and
semantic state taken from **Claude Code hooks** — never screen-scraping.

**Converged design:**
- **Design A — Native Graft:** the management "brain" lives *inside* the fork (C++/WinRT).
- **Manager tab (C1 "Linked Lenses"):** a pinned, leftmost, non-closable tab (tab 0, open
  by default), split into three selection-synced regions:
  - **Triage Board** (top) — sessions as cards in five state columns (Running · Waiting-for-you
    · Needs-approval · Error · Idle/Done), plus an observe-only **External** census column.
  - **Explorer Tree** (bottom-left) — the M working directories → their N sessions.
  - **Auto Testing** (bottom-right) — a per-session prompt queue + **Tests Autorunner**.
- **Auto Testing / Tests Autorunner:** queue prompts; on **turn-complete** (`Stop` hook) the next
  prompt is auto-sent. Approvals and clarifying-questions are handled separately.
- **⚠ Auto Testing / Tests Autorunner is a DEV-ONLY feature** (gated on
  `Profiles::IsDevPackage()` — the `AgentmasterDev` package). In a **release** install the whole
  subsystem is **hidden and inert**: the Manager's bottom-right pane shows the read-only **Summary**
  view only (no `[Summary | Auto Testing]` toggle), the autorunner `Scheduler` is **never started**
  (`Engine.cpp`) so nothing auto-sends, and every autorunner surface — the toolbar **Pause Tests
  Autorunning** button, the Settings cog **Tests Autorunner** tab, the Triage-Board ⚙ queue badge, the
  per-tab overlay autorunner button + queued rows, and the tab-strip tooltip's autorunner line — is
  not shown. (Formerly "Flight Plan" / "Autopilot", renamed + gated in one commit; persisted
  `autopilot`/`flightPlanShowsSummary`/`defaultAutopilotMode` keys still read back for back-compat.)
- **Per-tab link badge (overlay) — built ([`TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md)):**
  **every terminal tab the Fleet Observer classifies** carries a small **top-right terminal HUD**
  that makes the tab ⇄ Agentmaster link legible *while you work inside the session*. A **linked**
  Claude session shows the full badge — status (color-matched to the Triage Board) + the Fleet
  Observer's `model · effort · kind`, Tests Autorunner mode (**Manual/Semi/Full**), queued count, and link
  state **⛓ linked**, plus a dim **second row** `<workdir folder>/<branch>` and a dim **third row**
  `⏳ <next queued prompt>` (the first `Pending` prompt's first line, ≤300 chars + `...`; shown only when
  something is queued, mode-agnostic). Any **other** tab shows a
  dim **observe badge** `○ <kind> · unlinked` (kind =
  `pwsh` / `cmd` / `claude` *started-but-not-yet-prompted* (§11d) / `codex`) that **flips in place**
  as the tab's activity changes — a `pwsh` tab → `claude` the moment you run it → the full linked
  badge on its first prompt; cleared when claude exits or the tab closes — so a started-but-unprompted
  claude (no transcript id yet) is never invisible. Dim until hover; hover/click **expands** controls
  (Tests Autorunner cycle · Send-now · queue peek · Jump-to-Manager) + a contextual SemiAuto confirm.
  Off-switchable (`AppSettings.showTabOverlay`). Hover also reveals a **row of actions** — a folder
  button (Open Path) + a copy menu (Session Id · working dir · branch · the REAL Claude/Codex launch
  CLI · the full session **Summary** · the **Transcript**) + a **pencil** that toggles a **SUMMARY
  PANEL**: a second overlay below the badge (≤20% pane width) rendering the `session-end.js` box
  (messages/files/tasks/plan) analyzed from the transcript, its show/hide a GLOBAL setting
  (`AppSettings.showSummaryPanel`); a **wrap-line toggle** (↵) at the right of its times bar flips a
  message's newlines between a literal `\n` and real multi-line (`AppSettings.summaryPanelWrapNewlines`,
  also GLOBAL).
  The per-tab *here-and-now* lens, complementing the Manager's *fleet* view.

Full design: [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md).
Milestones & build: [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md).
Hooks bridge: [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md).
Workspace persistence (window layer): [`doc/agentmaster/PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md).
Release/dev identities + per-install state profiles: [`doc/agentmaster/PROFILES.md`](doc/agentmaster/PROFILES.md).
Per-tab link badge (overlay): [`doc/agentmaster/TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md).
Fleet Observer (pull correlation + activity): [`doc/agentmaster/OBSERVER.md`](doc/agentmaster/OBSERVER.md).
Sessions browser + the `~/.claude` storage map: [`doc/agentmaster/SESSIONS.md`](doc/agentmaster/SESSIONS.md).
Observer-owned session state (the PULL state engine — design, pre-implementation): [`doc/agentmaster/STATE.md`](doc/agentmaster/STATE.md).
Commandline introspection (the `agentmaster <verb>` CLI): [`doc/agentmaster/CLI.md`](doc/agentmaster/CLI.md).
Summary-panel JUMP (transcript→buffer resolve + center the view on a prompt): [`doc/agentmaster/SUMMARY_JUMP.md`](doc/agentmaster/SUMMARY_JUMP.md).
Favorite + Close refactor (Archive removed; Sessions is the sole history view): [`doc/agentmaster/FAVORITES.md`](doc/agentmaster/FAVORITES.md).
Pending-input monitor (detect an UNSENT draft in a Claude tab's input box): [`doc/agentmaster/PENDING_INPUT.md`](doc/agentmaster/PENDING_INPUT.md).
System notifications (Windows toasts when a session leaves Running; click = foreground + jump to tab): [`doc/agentmaster/NOTIFICATIONS.md`](doc/agentmaster/NOTIFICATIONS.md).
Slash-command bindings + /handover + /handover-here (CommandWatch: bind to typed /commands, await follow-up activity): [`doc/agentmaster/COMMANDS.md`](doc/agentmaster/COMMANDS.md).

## Status

**All milestones M0–M8 + session restore are complete, built, deployed (until the next deploy
cycle, still under the pre-split `Agentmaster` loose registration — the dev identity is now
`AgentmasterDev`, see *Deploy & run* migration), and verified running.** The engine passes
**694/694** standalone
checks (`AgentMaster/tests/`), and the full pipeline has been exercised end-to-end in the
deployed package: Launch → real `claude.exe` on a ConPTY → `--settings` hooks → PowerShell
forwarder → named pipe → registry → state machine → UI, plus `claude --resume` restore on
reopen (traces in `~/.agentmaster/hooks.log`).

**Workspace persistence ([`PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md)) — the window layer is
COMPLETE + live-verified.** (The original **M9–M14** milestone ladder is **superseded** by
PERSISTENCE.md §13's Increments 1–4: M9 + M10 shipped, **M11 obviated** — the lens rides the
`WindowRecord` autosave, not a persist-hook — M12/M13 delivered AS the Increments, and **M14
migration is moot** (no legacy format). The lone remaining item is one **cosmetic** refinement —
exact left-to-right tab interleave on reopen, noted at the end of this block.) **M9 (singleton
engine) is complete — compiles + unit-tested:** the
registry/bridge/scheduler are now ONE process-wide `SharedEngine` (`AgentMaster/Engine.{h,cpp}`),
so the WindowEmperor's many windows share one fleet over one pipe instead of two registries
racing on `\\.\pipe\agentmaster.<pid>` and clobbering `sessions.json`. **M10's data layer is
done — unit-tested:** the per-window `WindowRecord` schema + `windows/<id>.json` (de)serialize,
shaped as **Option 1** — per-window *UI state only* (geometry + Manager lens + ordered tab
*refs*); `sessions.json` + `SessionInfo.live` stay the single session source of truth, never
duplicated. **M10 capture + per-window lens restore is now done — built + live-verified**
(PERSISTENCE.md §13.5): each window **claims** its `windows/<id>.json` at engine init
(`Engine::ClaimWindowRecord`, or mints a fresh GUID), `_CaptureWindowRecord` reads live
geometry (the `PersistState` recipe) + ordered tab refs + the Manager lens, a debounced
autosave (`_saveWindowRecordThrottled`, fed by tab/resize/recolor/lens-push triggers + a
one-shot save at end of startup) persists it, and a claimed record **seeds the lens**
(`AgentManagerContent::Get/SetManagerState` + a lens-changed push). Verified in the deployed
package: the record carries real geometry + the real splitter layout, and a relaunch **reuses
the same windowId** (claim → restore round-trip). **Increment 2 (geometry re-apply) is also
done + live-verified:** a relaunched window reopens at its saved position/size/launch-mode via
the `TerminalWindow` startup seam (`GetInitialPosition`/`GetLaunchDimensions`/`GetLaunchMode` —
WT's own persisted layout is OFF in our DefaultProfile mode; note WT only calls
`TerminalPage::PersistState()` when `firstWindowPreference != DefaultProfile`, which our mode is
*not*, so geometry capture rides the autosave, not a close-flush). **So single-window workspace
restore — geometry + lens — fully works.** **Multi-window reopen (Increment 3) is also done +
live-verified — including both refinements.** On startup the `WindowEmperor` reopens the
**open-at-exit** set: it scans `windows/*.json` (sorted by filename, matching `LoadWindowRecords` so
`-s <idx>` is canonical), reads the **`open-windows.json` manifest** (a byte-scan for braced `{guid}`
tokens — it links `TerminalApp.dll`, not the `TerminalAppLib` static lib, so it can't call the JSON
helpers), intersects, and dispatches `wt -w new -s <idx>` only for the records that were open at last
exit; each window resolves `records[idx]` for geometry (TerminalWindow) and claims it by id
(TerminalPage), so geometry + lens agree. A **decide-prompt** (Yes/No "Reopen your N previous Agentmaster windows?")
gates the reopen when >1; a lone record / first run is silent. The **manifest** is owned by the
process-wide `SharedEngine` (NOT the Emperor enumerating the projected surface — the engine already has
every window's id): each `TerminalPage` `RegisterLiveWindow`s its `_windowId` at engine init and
`UnregisterLiveWindow`s in `~TerminalPage`, and every change rewrites `open-windows.json` = the live id
set **except** a change that empties it (skip-empty preserves the final snapshot; a hard shutdown that
kills the threads before they unregister leaves the full set). So a window closed mid-session is
**pruned** from the manifest (won't be re-offered) while its record stays on disk. The Manager's
**"Reopen Windows (N)"** recover button (in the toolbar before the cog — `_reopenBtn`, shown when N>0 == records-minus-live,
`Engine::RecoverableWindows`) is the "if I answered No" path: it reopens each not-currently-open record
via `<our alias> -w -1 -s <idx>` (`TerminalPage::_ReopenSavedWindows` ShellExecutes
**`_AgentmasterReopenTarget()`** — the per-IDENTITY execution alias **by name**, `agentmaster.exe`
release / `agentmasterdev.exe` dev, so side-by-side installs never reopen into each other; not the
upstream `wt.exe`, which doesn't exist for our packages; unpackaged falls back to the neighbor
`WindowsTerminal.exe` — and the
single-instance handoff routes it back to the Emperor) — the runtime analog of the Emperor loop. A
window claimed-then-closed THIS session is **re-claimed** (its real id + lens) from a second
**reclaimable pool** (`Engine::reclaimableWindowRecords`, fed by `UnregisterLiveWindow`, drawn by id
ONLY), not minted as a lens-less duplicate; the no-arg front-pop claim never draws from it, so a plain
"+ new window" never adopts a closed window's layout. Verified end-to-end: 2 records + a manifest
naming only one → exactly that one reopens (silent) at its geometry; a fake id is dropped + the second
never added (register rewrote to the live set); the manifest survives app-close (skip-empty); a
gracefully closed window is pruned; and the recover path reopens a not-open record at its saved geometry.
**Window-grouped restore is now shipped + live-verified** — a reopened window re-homes its **whole
workspace**, not just geometry + lens. `TerminalPage::_RestoreWindowTabs` (run from `_OnFirstLayout`
after `_RestoreClaudeSessions`, gated on a *claimed* record) walks the record's ordered tab refs and
rebuilds them in place: each **Claude** ref resumes its session INTO this window (`_LaunchClaudeSession`,
lazy-start safe — no eager `connection.Start()`); each **Other** ref replays its captured WT startup
actions (`WindowLayout::FromJson` → one `ProcessStartupActions`) to recreate the shell tab with its
title + color + cwd. Capture fills the Other ref's `actionsJson` from `BuildStartupActions(Persist)` →
`WindowLayout::ToJson` in `_CaptureWindowRecord` (so `windows/<id>.json` now carries the pwsh/cmd tabs,
not just Claude refs). Saved windows that aren't currently open reopen WHOLE via the toolbar **"Reopen
Windows (N)"** button (`Engine::RecoverableWindows` → `_ReopenSavedWindows` → `agentmaster -w -1 -s <idx>`);
the per-window "Saved window" cards + "Restore here" the removed Archive page once offered are gone
(FAVORITES.md — a closed session is resumed from the Sessions browser instead). A clobber guard keeps a not-yet-laid-out window (no tabs AND no geometry, or
pre-Initialized) from overwriting a good record on disk (`_FlushWindowRecord`), and the close-flush
captures the final state before the gap-#1 teardown clears `_claudeTabs` (`CloseWindow`). A reopened
(claimed-record) window also **suppresses the default startup tab** — `_OnFirstLayout` gates
`ProcessStartupActions` on `!_windowRecordClaimed`, because in DefaultProfile mode a reopen's `wt -w new
-s <idx>` (where `ShouldUsePersistedLayout` is off) falls back to a default `newTab` that would otherwise
append a spurious pwsh tab to every reopened window and **accumulate one shell per reopen cycle** in the
record; a reopened window's content comes from the re-home, not the default action. Live-verified:
a window with 1 Claude + 3 pwsh tabs closed → reopened at its geometry (1466×780 @ 14,173) with the
Claude session resumed + all three pwsh tabs (title/cwd) replayed; the record round-tripped intact.
The reopened window also **re-selects the tab that was focused at close** — persisted by STABLE
IDENTITY (`WindowRecord::selectedSessionId` = the Claude conversation id; a shell tab, which has no
cross-restart id, falls back to `selectedTabIndex`; Manager / none ⇒ both unset). `_RestoreWindowTabs`
resolves the focused tab's live position and applies it deterministically — a trailing `SwitchToTab`
appended to the shell `ProcessStartupActions` batch (so it runs AFTER the async shells exist), or a
synchronous select when there are no shells. **Still deferred:** exact left-to-right interleave of
Claude vs Other tabs on reopen (Claude tabs land first, then shells — see PERSISTENCE.md §13.5).

**The Fleet Observer (O1–O7, [`OBSERVER.md`](doc/agentmaster/OBSERVER.md)) is complete — built,
deployed, and live-verified.** It is the **PULL** half of the state engine: a process- +
transcript-driven survey that detects + correlates + enriches **every** Claude session and tab
activity **out-of-band** (reading each `claude.exe`'s PEB — cwd / cmdline / env — and its
transcript), so a hand-typed `claude` that fires **zero hooks** (a shell `claude` function/alias
shadows the PATH shim) is still detected, bound, and driven — with **no hooks, no shim, no
settings, and nothing the user can feel** (all reads are out-of-band; it **never** writes to a
shell). It is the always-correct floor beneath the lossy hook **push** and subsumes the old per-tab
Toolhelp walk + the global `projects/` discovery scan. Live-verified end-to-end in the deployed
package: a no-hook `claude.exe` typed after a `cd` lands a Triage-Board card + per-tab overlay
within ~3 s, classified `Agentmaster`; two claudes in one cwd bind to their **own** conversations;
real-WindowsTerminal claudes are classified external + never bound; steady-state cost is µs.

**Codex (the OpenAI Codex CLI, `codex.exe`) is now a first-class MANAGED agent — observe (C1) +
state (C2) + the full LAUNCH / RESTORE / WINDOW-RESTORE / ADOPT lifecycle, *lifecycle + state only*;
lib-compiled green + engine-tested (771/771 incl. new Codex checks), rides the next deploy cycle
(OBSERVER.md §11f / §19-Q3).** Codex graduated in four steps on top of the C1 observe-only census,
each a focused commit — all zero writes to `~/.codex` (every read is out-of-band, Rule #13):

- **C1 — observe-only enrichment (done).** The Fleet Observer reads every `codex.exe` out-of-band:
  `ReadCodexFacts` (PEB cwd/cmdline/env → model `--model`/`-m` · sandbox `--sandbox`/`-s` · approval
  `--ask-for-approval`/`-a` · `WT_SESSION`/`AM_SESSION`/`CODEX_HOME` · an explicit `resume <guid>`), a
  **date-sharded rollout resolver** (Codex shards by LOCAL date —
  `<CODEX_HOME|~/.codex>/sessions/YYYY/MM/DD/rollout-<ISO-ts>-<uuid>.jsonl`, the uuid IS the conversation
  id, so the Claude cwd-encoded glob does NOT apply: scan the start-day ±1, confirm each candidate's cwd
  by a head-scan of its `session_meta` line, pick by **ctime≈start identity** with a **newest-mtime-in-cwd
  fallback for a `resume`d** session whose rollout predates the process), and a rollout reader
  (`RolloutLine`/`payload` JSONL — model/effort/sandbox/approval from the first `turn_context`; title +
  human prompts from `event_msg`/`user_message`, skipping the AGENTS.md `response_item` blobs).
- **C2 — state via a rollout-tail PULL reconciler (done, commit `b91018a77`).** `ClassifyCodexLine`
  (pure: an `event_msg` payload → a turn-boundary verdict) + `ReadCodexStateDelta` (a byte-cursor forward
  delta over the rollout — first-sight tail-seek of the last 1 MiB, a 4 MiB catch-up cap, a partial
  trailing line left unconsumed, sticky on a quiet read) derive **Idle / Running / Waiting** from the
  transcript tail: `task_started` → Running; `task_complete` / `turn_aborted(interrupted)` /
  `thread_rolled_back` → Waiting; **last-boundary-wins** (proven over 33 live rollouts: timestamps are
  100 % monotonic, file-order = truth). **NeedsApproval / Error are NOT PULL-derivable** — no
  approval/permission/error event exists in a rollout — so Codex runs on a **3-state floor**
  (Running / Waiting / Idle); the new `Activity.h` `CodexState` maps onto the board + tree state dot.
- **Managed lifecycle — launch / restore / window-restore / adopt (done, commits `64b2472e7` Part 1 ·
  `9922e4658` Part 2 · `51a8ca51d` Part 3 · `efaa586e2` Part 4; mirrors Claude — "do what we do there").**
  A managed Codex is a real registry citizen via a **two-id model** (the §19-Q3 follow-on, resolved to
  the **first-class `AgentKind` path**, not a parallel one): `SessionInfo.id` = OUR minted durable handle
  (the registry / persistence / `_claudeTabs` key — Codex can't pin a session id at launch, so we never
  re-key), `SessionInfo.codexSessionId` = the real rollout uuid (the `codex resume` target + the "does a
  transcript exist" gate), filled by the observer's `_ReconcileManagedCodex` on the first prompt.
  `SessionInfo` gained **`kind`** (Claude default) + `codexSessionId`; `TabKind` gained **Codex** (the
  window-record tab refs + persistence (de)serialize it). **Launch** — the primary launch bar carries a
  **Claude⇄Codex toggle** (Part 4, the scope/sort/autorunner toggle idiom; Codex is *directory-only* — no
  typed-id resume/fork, button reads "Launch Codex"), and the EXTERNAL menu's **Open New Codex Session
  Here** spawns one in a running codex's cwd → `_SpawnCodexSession` → `_LaunchCodexSession`
  (`BuildCodexCommandline` = `<codex>` / `<codex> resume <uuid>` / `<codex> fork <uuid>`, where `<codex>`
  is the launcher resolved ONCE at engine init to a **full path** (`ResolveCodexLauncher` →
  `Engine::codexExePath`) and run by path — a `.exe` quoted, a `.cmd`/`.bat` via `cmd /c` — because
  ConPTY's `CreateProcessW` appends only `.exe` and IGNORES `PATHEXT`, so a bare `codex` token would miss
  an npm `codex.cmd` and die `0x80070002` (the same trap Claude's native-exe policy fixed; Codex is NOT
  exe-only — the observer finds `codex.exe` as a DESCENDANT, so a `.cmd` re-exec is fine, and an empty
  launcher falls back to the bare `codex` token + the surfaced error); immediate managed card,
  `AM_SESSION` stamp, no `CCMGR_*`). **Restore** — the resume seam branches on `kind` (`_RestoreArchivedSession`,
  now reached from the Sessions page's "Resume here", → `_LaunchCodexSession`, transcript-gated on the
  rollout existing, else fresh — Rule #6). **Window-restore**
  — `_RestoreWindowTabs` re-homes a Codex tab ref via `_LaunchCodexSession`. **Adopt** — the EXTERNAL menu's
  Adopt brings an external codex's rollout under management via a **Fork-a-copy vs Resume-anyway** choice
  (`_AdoptExternalCodex(pid,cwd,fork)` behind `_ConfirmChoice`): fork = `codex fork <uuid>` into a NEW
  rollout — safe while the original is still running (no two-writers hazard) — else `codex resume <uuid>`
  (take-over); original left running. A managed Codex reads distinct everywhere via a teal **`codex` pill** (Part 3 —
  board card + tree row, the same teal as the External pill). **Lifecycle + state ONLY** — NO stdin
  injector / Tests Autorunner / Send-now (driving the Codex TUI is C4); a managed codex's `autorunner.mode` is Off.

The observer still **never** feeds a codex to `ObserveClaude` (the External census stays observe-only); a
MANAGED codex is registered by the launch path and reconciled (state + `codexSessionId` + `tabToken`) by
the UI-lane `_ReconcileManagedCodex`, and is **deduped out of the External census** (`managedCodexTokens`)
so it shows once. **Lib-compiled green; not yet full-exe deployed** (rides the next deploy cycle).
**Deferred:** **C3** = low-latency PUSH via Codex `notify` / `~/.codex/hooks.json` (a GLOBAL config
mutation — a product decision, no per-session `--settings` like Claude); **C4** = bind a stdin injector +
Tests Autorunner (drive the Codex TUI; launch is PULL-correlated, resume=`codex resume <id>`).

**Archive is REMOVED — replaced by Favorite + Close; the Sessions browser is the SOLE history view
([`FAVORITES.md`](doc/agentmaster/FAVORITES.md); commit `b55765138`; lib-compiled green + engine-tested
1101/1101 + Debug-built + dev-deployed + verified engine-live).** The full-window Archive *page*
(`TerminalPage.AgentArchivePage.cpp` — deleted), the **Archived (N)** toolbar button, the retired
in-content archive overlay, and the Archive page's per-window "Reopen window" + synthetic "Saved window"
rows are ALL gone. A managed tab's lifecycle verbs are now just **Close** and **Favorite**: **Close**
*always archives* (keeps the `live=false` record so the session stays resumable from Sessions — it NEVER
deletes; the old 3-way Delete/Archive/Cancel confirm became **Close · ★ Favorite & Close · Cancel**, the
batch close **Close All · ★ Favorite & Close All · Cancel All**, and every "Delete permanently" UI was
removed — **★ Favorite & Close** (dialog `Secondary`) stars the session(s) via `SetSessionFavorite` before
archiving, so a keep-this close is one gesture; closing NEVER auto-favorites). On the **single-tab** close
confirm the `Secondary` button **flips sense on the session's current star** (`IsSessionFavorite`): an
already-favorite session offers **☆ Unfavorite & Close** instead (the hollow ☆ anti-star — the Sessions ★
column's own unfavorited glyph — `SetSessionFavorite(id,false)`), since re-favoriting a starred session
would be a no-op. **Favorite** (a hollow ☆ /
filled-yellow ★) is the new "keep/find this" marker, persisted via the **SessionStore `favorite` key**
(the durable per-session *title* store, reused — survives Close, works for never-managed on-disk sessions,
one sparse scan; `IsSessionFavorite` / `SetSessionFavorite` / `LoadAllFavoriteSessions`). It surfaces as a
**leftmost ★ column** + a **`[ ] Favorite` filter** + a row "Favorite/Unfavorite" menu on the Sessions
page, and a **Favorite/Unfavorite** item on a managed Claude tab's right-click menu (beside Close). The
removal lost NO capability: the Sessions page's "Resume here" already rehydrated a closed session's Flight
Plan via `_RestoreArchivedSession` (so it subsumed the Archive page's "Restore here"). **Reopen window is
unchanged** — the toolbar **"Reopen Windows (N)"** button + the `wt -w -1 -s <idx>` mechanism stay. A
transcript that's gone (rare — Claude seldom sweeps) simply isn't listed (no broken Resume — "not found
=> don't show"). `_RemoveSessionRecord` survives only as the internal resume-fresh stale-drop (no longer
auto-hides). Detail: *C1 UI* + the lifecycle bullet under *Persistence*. (The prior full-window Archive
page + its round-2/round-3 audits + informativeness batch are HISTORY, superseded by this refactor.)

**The Sessions browser ([`SESSIONS.md`](doc/agentmaster/SESSIONS.md)) is implemented — engine + UI,
lib-compiled green + engine-tested (the 604-check harness incl. a live-corpus smoke); it rides the
next deploy cycle. It is now the SOLE history view (the Archive page is gone — FAVORITES.md).** A
**"Sessions"** toolbar button (the toolbar's rightmost, after Pause Tests Autorunner) opens a full-window page
(the deferred-pointer-handler discipline the Archive page pioneered) listing **EVERY on-disk
Claude Code session** (`~/.claude/projects/*/<uuid>.jsonl` — not just managed ones) in a selectable
window: the `[1 month]` button click-cycles 1d/3d/7d/14d/1mo/3mo, hover opens a **From/To range
popup** (plain text boxes — islands-safe). It carries a **leftmost ★ favorite column** (hollow ☆ /
filled-yellow ★, click toggles the durable SessionStore star) + a **`[ ] Favorite` filter** + a row
"Favorite/Unfavorite" menu (FAVORITES.md). The search bar `[ search ] (👤)(🤖)(📁)(📄)(🏷)(F) [☐ Open]
[☐ Hidden] [☐ Favorite] [1 month] [↻]` runs
**two-phase**: FAST = in-memory over per-session **sidecar indexes**
(`~/.agentmaster/sessions-index/<sid>.json` — `(size,mtime)`-invalidated, **incrementally**
re-accumulated from the stored byte offset; built by `TranscriptStore`) + the **`history.jsonl`
accelerator**; SLOW = **ripgrep**-prefiltered transcript content (`rg -il`, PATH-resolved,
batched under the cmdline cap, full in-process fallback), every match **scope-attributed
in-process** (👤 typed prompts vs 🤖 assistant text/thinking + tool inputs/results — rg can't tell
them apart, `ClassifyTranscriptLine` can), generation-cancelled on re-type. Both message scopes
OFF ⇒ title + directory + the sidecar's cached first prompt (message *bodies* still need a 👤/🤖 scope); 📁/📄 match the **directories/files a session's tool calls touched**
(`TranscriptStats::pathsAccessed`) and **default ON** (fast-phase-only — in-memory over the
sidecar, no rg/transcript IO; 👤 also **default ON** — it flips on the SLOW content scan for your
own typed prompts by default; 🤖/(F) default OFF — 🤖 also flips on the SLOW
content scan); (F) fuzzy has identical rg/in-process semantics
(`BuildSearchRegex`/`MatchesQueryText`, tested as a pair). The query parses into
**whitespace-split AND terms** (`ParseSessionQuery` — every term must hit, each may hit a
different field): `"quoted phrase"` = ONE **exact** contiguous term ((F) never applies inside
quotes), and a bare **whole session-id GUID** token ({braces} tolerated, never fuzzied) also
matches the session's **identity** — its id + fork-parent id — so a pasted id finds the session
and its forks; in the content phases a guid term **scopes** hits to that session, and a
guid-only query is answered by the fast phase alone. **When a search is active the rows are RANKED IN
RELEVANCE TIERS** (`_RenderSessionsTable`, reusing the fast phase's `ParseSessionQuery`/`MatchesQueryText`/
guid-identity primitives over the name fields): **tier 0** a NAME/identity match (the displayed title,
branch, or a pasted id) > **tier 1** a dir/path-only match (cwd or a tool-touched path) > **tier 2**
content-only (the slow phase), with the chosen **column sort applied WITHIN each tier**. So the session
you searched BY NAME lands on top instead of being buried under a recently-active *incidental* content
match — the default sort is Active-desc, which otherwise floated a one-off transcript mention of "browse"
above the session actually titled "Browse …" (and a click/resume/fork then acted on the wrong top row).
Rows carry fork-aware **created** (a
fork duplicates its parent's lines verbatim with `forkedFrom` stamps — file birth is the truth),
**line-derived last-activity** (file mtime lies: measured median ~1 h, max ~43 days —
SESSIONS.md §5), the title precedence `customTitle > aiTitle > legacy summary > first REAL
prompt`, the msgs·tools weight, and the session's **per-dir tab color** as a chip (solid = OPEN
here, dim = on-disk) with a **presence ring** when claude's own heartbeat reports busy/idle/
waiting. Detail = metadata + scope-tagged match snippets + the numbered prompt list (off-thread,
(id,mtime)-cached); actions: **Jump** (OPEN here), **Resume here** (`_ResumeSessionFromDisk`: an
unknown sid gets a minimal archived-shaped record, then the SAME transcript-gated `--resume` seam
— title pinning, dir color, hook correlation all reused). **Resume / Fork / window-restore open EXACTLY
the picked/recorded session id — no "continuation tail" redirect.** The earlier `ResolveContinuationTailOnDisk`
heuristic (follow a `/clear`/`/compact`/plan chain to its newest same-cwd link so you "land where you left
off") was **REMOVED — it had no solid basis and produced systemic false positives.** There is NO solid
on-disk signal for a `/clear`/`/compact`/plan SUCCESSOR: a `/compact` is **in-place** (one file, a
`system/compact_boundary` line — proven 397/400 corpus-wide; it never mints a new id), a `/clear` leaves
**no** link to its successor, and a plan-restart child references its **parent** (backward), never the
parent its successor. The edge was therefore pure timing (same cwd + B non-fork + B created within
`[A.lastActivity − 5s, +15min]`), which merely chains the **next independent session** the user started in
a busy dir onto the prior one — proven on the real 198-session `K:\source\Agentmaster` corpus: it would
redirect **21/198** sessions, **5 targets each "continuing" 2–4 unrelated predecessors** (e.g. one fresh
`[spawn]`ed session claimed as the continuation of both an "observer screen-monitor" and a "settings-tabs"
conversation — the exact reported "won't resume / jumps to a different tab" bug). The SOLID **plan-restart
parent** link (the explicit `"read the full transcript at: <parent>.jsonl"` reference, 620 in the corpus)
is **kept** — it still drives the summary panel's "previous session(s)" lineage (`CollectConversationLineage`,
backward only). **Fork here** (`_ForkSessionFromDisk` —
the duplicate-tab fork's recipe: `claude --resume <parent> --fork-session --session-id <new>`, the
new id minted by us so hooks/registry correlate from the first event; offered on EVERY row
**including a LIVE one** — a fork writes its OWN transcript, so the adopt path's two-writers
hazard doesn't apply; transcript-gated → fresh; titled via `DeriveForkTitle` (`"<title> (fork)"`,
then `(fork 2)`/`(fork 3)`/… on a fork-of-a-fork, never stacked `(fork) (fork)`), logged
`[sessions-page->fork]`), **Open New Session Here**, an **Edit Title** (rename in place), a right-click
**Filter ▸** submenu, and a
right-click **Hide from list**
(`_HideSessionFromList` → `_AddSessionIdToHiddenList` → `AppSettings.hiddenSessionIds`, persisted +
filtered out of the browser; cleared from the cog's **Reset hidden sessions**); double-click = resume.
**Edit Title — rename a session IN PLACE** (right-click **Edit Title** OR a **"slow double-click"**: a
re-click of the already-selected row, the Windows-Explorer rename gesture, disambiguated from a fast
double-click=resume by an OS-`GetDoubleClickTime` arm timer a `DoubleTapped` disarms). It swaps the row's
Title cell for a focused in-place `TextBox` (`_BeginSessionsRename`; a `ContentDialog` text box gets no
keypresses under XAML Islands, so editing is inline — the Manager's Explorer-tree rename idiom), Enter /
focus-loss commits + Escape cancels (both deferred so the re-render can't tear the box out; a blank edit
keeps the old title — never empty, Rule #11). The title is **persisted durably**
(`_PersistEditedSessionTitle`): a registry-known session (open OR archived) routes through
`_RenameClaudeSession` (registry + the WT tab if open + the Engine observer mirrors it to the
**SessionStore** `title` key AND `sessions.json` — the title is ONE value), a pure on-disk session writes
the SessionStore directly (`SetStoredSessionTitle`, the overlay `LoadAllStoredSessionTitles` reads for
closed rows); the edit reflects instantly into the in-memory rows + 🏷 search index (no re-gather) and
survives across windows/runs.
The **Filter ▸** submenu (`_SessionsRowFilterState` / `_SessionsRowFilterKind`, SESSIONS.md §1) narrows
the list to sessions **like the clicked row** — **By Same Directory** (filesystem-aware `NormDirKey`),
**By Same Branch** (exact; only when the row has one), **By Same Day / Week / Month** (the row's
**created** time bucketed in **local** time, DST-safe `[start,end)` via `SessLocalBucket`; week is
Monday-start), and **By Fork Family** (the anchor's connected fork-graph component, when it has lineage).
Facets **stack across dimensions as AND**, the time dimension holds **one granularity at a time**, and a
facet the anchor row already matches reads **✓** and toggles **OFF** on re-click. It is applied at the
SAME render chokepoint (`_SessionsRowPassesRowFilter` in `_RenderSessionsTable`), so it composes (**AND**)
with the search text + the scope/Open/Hidden toggles; a dismissible **`✕ filter: …`** chip beside the
search box (`_sessFilterChip`) shows + clears the active facets (the submenu also has **Clear filters**),
the count line notes `· filtered`. Pure browse-state — nothing persisted, the transcript untouched (it
resets on restart, unlike `hiddenSessionIds`).
"Hide from list" is **manual only** now — the **Delete permanently** path that used to auto-hide a
session is GONE (FAVORITES.md: Close keeps every session, never deletes), so `_RemoveSessionRecord` no
longer calls `_AddSessionIdToHiddenList`. A search-bar **"Hidden" checkbox** (`_sessHiddenBtn`, default
OFF, beside "Open") **reveals** the hidden set — shown dimmed, with the row menu's **"Unhide"**
(`_UnhideSessionFromList`) replacing "Hide from list" — so a manually-hidden session is findable +
resumable without clearing the whole set from the cog. (The earlier auto-hide-on-delete change + its
documented follow-up gaps are MOOT — there is no Delete; the stale "still appears in Sessions" delete-path
copy went with the removed Delete UI.) **Presence
integration (§7-Q5's separation):** `TranscriptStore::ReadSessionPresence`
owns the raw `~/.claude/sessions/<pid>.json` read; the **observer** validates rows against its
process snapshot (stale/PID-reuse dropped) and publishes a `Presence()` table + the transient
`SessionInfo.presenceStatus` fact through `ObserveClaude` (**never** `SessionState` — Rule #13).
The same pass also fixed engine bugs: `ReadTranscriptInfo` now honors `ai-title`/legacy `summary`
+ skips sidechain/compact-summary lines, and the shared **noise filter** (`IsNoiseUserPrompt`)
keeps interrupt markers / command echoes / task notifications out of titles, prompt lists, AND
the scanner's Auto-Testing back-fill (STATE.md §8 bug-2 fixed). **Both full-window pages (Archive +
Sessions) now share generic chrome:** a window-level **overlay registry** — each page
`_RegisterAgentPageOverlay`s its host + atomic visibility mirror + an optional dismiss hook (the
Sessions page closes its range Popup there: popups render in the popup ROOT, a collapsed host
does NOT hide them) and the tab-switch seam (`TabManagement.cpp`) calls
`_DismissAgentPageOverlays()` — every page closes on tab switch with no page named there, so a
future page binds automatically by registering. **Up/Down navigate the visible (sorted +
filtered) rows** in both pages — nothing selected ⇒ Down picks the FIRST row, Up the LAST; then
±1 **wrapping** at the ends — via `PreviewKeyDown` on the page host (tunneling, so it beats the
focused search box), deferred to a clean tick, recolor-only highlight
(`_UpdateArchive/SessionsSelectionHighlight`) + `StartBringIntoView`; both pages **focus their
search box on show** (keyboard events only route through the page when focus is INSIDE it —
typing filters immediately, arrows work from the first keystroke). The button +
header-declaration wiring (AgentManagerContent, TerminalPage.h, ProcessObserver.cpp,
m5_tests.cpp, the Archive-TU registration/key-hook) rides the in-flight working tree alongside
the concurrent UIA work; the tree as a whole builds green.

**The tab strip itself now carries the state dot.** Every classified tab's header reads
`[icon] ● <title>`: a state-colored **Ellipse** (thin black stroke for contrast on any tab
chrome; Margin `0,1,6,-1` — **left is 0**: an earlier negative-left overhang (to pull the dot toward
the profile icon) was reverted because it hung past the header's left edge and got CLIPPED. The MUX
`TabViewItemHeaderIconMargin` (10px) icon→dot gap is instead tightened the **safe, global way** — a
`TabViewItemHeaderIconMargin` override on the `TabView` in `TabRowControl.xaml` (the override is `0,0,4,0`; the dot-wrap's own `0,1,6,-1` keeps the
dot→title gap); **+1/−1** dips it 1px below slot-center — dead-center reads optically high against the
title — while keeping the 10px slot so the header row doesn't grow) in
`TabHeaderControl.xaml`'s indicator row right before the title — one more
`x:Bind`'ed element over `TerminalTabStatus` (two new observable properties,
`AgentStatusVisible`/`AgentStatusBrush`; `Tab.idl` already projects `TabStatus{get;}`, so no
`Tab.{h,cpp}` changes — the page drives it idempotently via `_SetTabAgentDot(tab, color?)`). A
**managed** session's tab wears its Triage-Board state color (Running blue · Waiting goldenrod ·
NeedsApproval orange-red · Error crimson · Done green · Idle gray — a MANAGED Codex tab wears the
same dot at its 3-state floor: Running blue · Waiting goldenrod · Idle gray); an observed-but-unmanaged
tab (pwsh / cmd / unprompted-claude / external codex) a **dim gray** dot; the Manager tab none. A
**red-flash alert** rides the dot too: when a session leaves **Running** for a *needs-you* state —
`Running → {Idle · WaitingForInput · NeedsApproval}` (NOT `→Done` / `→Error`) — on a tab that is **not
the currently-focused one**, that tab's dot grows a **flash ring** (a separate Ellipse drawn behind the
dot — ring · the dot's black stroke · its status fill, `TerminalTabStatus.AgentFlashRingVisible`; a
ring, NOT a stroke-color change) until you switch to it. The ring's **color AND opacity are
user-configurable** (the Settings cog's **TABS ▸ Status flashing color** picker — a `muxc::ColorPicker`
with its alpha slider enabled — a GLOBAL `AppSettings::flashRingColor` stored `#AARRGGBB`, **default
red at 80% opacity** `#CCFF0000`; painted via the per-window shared
`_flashRingBrush` bound to `TerminalTabStatus.AgentFlashRingBrush`, applied live on Save + cross-window
broadcast — `_RefreshFlashRingBrush`)
(`_EvaluateAgentFlash` — the active tab counts as visited so it never flashes; visiting it clears the
flash via `_VisitTabClearFlash` from `_OnTabSelectionChanged`). ONE shared per-window `DispatcherTimer`
toggles `_agentFlashPhase` every **600 ms** so all flashing tabs blink in **lockstep** (a tab joining
mid-cycle adopts the current phase); an archived (`!live`) session forgets its last state so a later
background restore can't spuriously flash. A **selection pill** rides the header too — a translucent
accent pill behind the tab (`HeaderAgentSelectionPill`, driven by `TerminalTabStatus.AgentSelectionVisible`/
`AgentSelectionBrush` at ~40% alpha via `_SetTabSelectionPill`) shown while the **Manager tab is active**
and this session is hovered/selected there: the tab-strip half of the Linked-Lenses selection sync. A
**FAVORITE marker** also rides the dot (FAVORITES.md §5a) in one of **two user-selectable glyphs** (the
Settings cog's **TABS ▸ Favorite marker** dropdown — a GLOBAL `AppSettings::favoriteIcon`, default
**Crown**): **Crown** = a small **gold `Path` crown** (`#F5C242`, the Sessions ★ color) perched at the
**north-west** of the status dot (peak tilted NW, `RotateTransform`), drawn as the wrap-Grid's last child
so it sits ON the dot; **Star** = the state-colored status dot becomes the **foreground of a white,
golden-tipped `Path` star** (a 5-point star drawn BEHIND the dot — white fill, gold `#F5C242` stroke
whose miter tips read golden — so its points radiate around the dot). The one visible "keeper" marker on
a LIVE session's tab (`HeaderAgentFavoriteCrown` / `HeaderAgentFavoriteStar`, bound to the **mutually
exclusive** `TerminalTabStatus.AgentFavoriteVisible` (crown) / `AgentFavoriteStarVisible` (star), driven
by `_SetTabAgentFavorite` (reads `favoriteIcon`, asserts exactly one) / `_RefreshTabFavoriteCrown` /
`_RefreshAllFavoriteIcons` from the durable `IsSessionFavorite` at launch / bind / favorite-toggle, and
re-asserted live on a Crown↔Star change via the cog Save + cross-window broadcast; same-window-instant,
cross-window-on-next-bind). Deliberately
NOT a title prefix — the one-title invariant (Rule #11: Explorer name == tab title == persisted
title) must never carry presentation glyphs through renames/persistence. The state palette is
shared through **`AgentStatusColors.h`** (`AgentStatusColorFor`): the **per-tab overlay**
(`AgentTabOverlay`) + the **tab-strip dot** (`TerminalTabStatus`) read it (the overlay's hand-synced
copy folded in — the third consumer, the tab dot, was the cue to factor it). **`AgentManagerContent.cpp`
still holds its own identical `StateColor` copy** for the board cards/columns (a concurrently-edited
file — converge on a quiet day), so a color change must be made in BOTH places.

**Release/dev separation + per-install state PROFILES ([`PROFILES.md`](doc/agentmaster/PROFILES.md))
is implemented — lib-compiled green + engine-tested (604/604 incl. new profile checks); it rides the
next deploy cycle (and needs the one-time `Remove-AppxPackage`/re-register migration documented in
Deploy & run).** The GitHub release now ships its OWN identity (`Package-Rel.appxmanifest`:
`Agentmaster`/`agentmaster.exe`, selected by `/p:AgentmasterPackageIdentity=Release`) while the dev
loose layout becomes **`AgentmasterDev`**/`agentmasterdev.exe`/"Agentmaster Dev" — so both install
side by side; `GetWtExePath`-class launchers and the reopen dispatch (`_AgentmasterReopenTarget`) pick
the alias by PFN prefix (Dev first). ALL persisted state — engine files AND Terminal's own
settings.json/state.json (a `GetBaseSettingsPath` redirect to `<profile>\terminal\`) — lives in ONE
**profile folder** per install: resolved env `AGENTMASTER_PROFILE` > `.portable` marker
(`<exedir>\profile`, true-portable zips now pass `-PortableMode`) > the per-install slot in
`~/.agentmaster.profiles` > per-identity default (`~/.agentmaster` release+unpackaged /
`~/.agentmaster-dev` dev). An install's **first launch AUTO-SELECTS the per-identity default WITHOUT
prompting** — release → **Production** (`~/.agentmaster`), dev → **Development** (`~/.agentmaster-dev`)
— and persists it (`EnsureProfileResolvedAtStartup` → `DefaultProfileDir()` + `SaveChoice` +
`SeedTerminalSettings`), running from `WindowEmperor::HandleCommandlineArgs` AFTER the single-instance
handoff and BEFORE any state read (no UI, so a `-Embedding` defterm activation takes the same path —
`allowUi` now gates only the two-instances-on-one-profile warning). The old **Production / Development /
Browse…** TaskDialog picker (comctl32 v6 dep in `WindowsTerminal.manifest`; + a "copy existing data from
`~/.agentmaster`" checkbox that skips `locks/`+`shim/`+`bridge.json` and never clobbers) still exists
(`ShowProfilePicker` / `MigrateProfileData`) but is now reached **only** from the cog's **PROFILE** row's
Change… (applies on restart) — first launch is silent. A kernel **profile mutex** warns if two live
instances point at one folder. The generated hook
forwarder's bridge discovery is now per-profile too (`BuildForwarderScript(stateDir)` — was a
hardcoded `~/.agentmaster/bridge.json`, a cross-instance hook-routing bug). Engine code is
otherwise untouched: `AgentmasterStateDir()` simply resolves through `ProfileBootstrap.h`, so
unpackaged/test runs keep the historical `~/.agentmaster`.

**Commandline introspection — the `agentmaster <verb>` CLI ([`CLI.md`](doc/agentmaster/CLI.md)) is
implemented (P1, read-only), deployed to the DEV alias by an in-place binary swap (no instance closed),
and live-verified; the ONE unexercised step is a full package build of the committed wiring.** The
fleet is queryable from a shell so an AI agent can understand *what is going on inside any tab/session*
WITHOUT the UI: **`show <ref>`** (full introspection — identity/placement, derived state + presence,
the conversation tail + last assistant reply, activity [msgs/tools/**files touched**], the last user
prompt, the Auto-Testing queue + autorunner; `--tail N` sets how many conversation turns `show` emits,
**default 8** — no effect on the other verbs), **`list` / `sessions` / `tabs` / `windows` / `external`**
(**`sessions` defaults to OPEN/live only** — `--archived` adds shut-down sessions, `--state <S>`
exact-matches the derived state, `--dir <D>` substring-matches the working dir; **`tabs --window <W>`**
filters by window-id substring), plus **`--self`** (introspect the calling agent's OWN tab via
`WT_SESSION`), **`--version`** (prints the CLI's schema version), and **`--json`** — each reply a
`{schemaVersion: 1, profile, <verb-key>: …}` envelope (full per-verb schema in CLI.md). Exit codes:
**0** success · **1** ref-not-found / ambiguous / `--self`-found-no-session · **2** usage/arg error. It **always works — app up OR down** — by reading only persisted +
OS-observable state (the Fleet Observer's PULL model run one-shot from a separate process):
`sessions.json` + `windows/<id>.json` + PEB process facts + transcripts + claude's presence heartbeat;
no live responder, no new IPC, **no engine edits for the reads** (`AgentMaster/cli/agentcli.cpp` links
the existing pure-C++ engine units, like the test harness). A live claude is bound to its conversation
by claude's OWN presence self-report (`sessions/<pid>.json`, **pid-keyed** — immune to the cwd-density
mis-bind Rule #14 warns of), then explicit `--session-id` / `--resume`, then the cwd→transcript
fallback; `external` rows are **host-classified** (`agentmaster-self` [a claude THIS instance launched but not
in our roster] / `agentmaster-other` [a sibling install's session] / `windows-terminal` / `console`). State is **derived** offline from the presence heartbeat +
transcript-tail via the same `SessionScanner` predicates (`ParseTranscriptDelta` /
`IsTerminalStopReason` / `IsInteractiveTool`). **Transport — the overload:** the execution alias
already targets the tiny `wt`/`wtd` launcher shim (`src/cascadia/wt/shim.cpp`, NOT the GUI), now
**console-subsystem + DUAL-MODE** — a CLI verb (or a CLI-only LEADING flag the shim routes to the CLI:
`--json`/`--self`/`--tail`/`--instance`/`--state`/`--dir` — plus `--offline`, **reserved**: the shim
routes it, but the P1 reader (`agentcli.cpp`) does not yet implement it and rejects it as unknown — none of which collide with a WT commandline)
execs **`agentmaster-cli.exe`** on the caller's console; **any other commandline forwards to
`WindowsTerminal.exe` byte-for-byte** (bare launch / `-w` / `-s` reopen / `-Embedding` defterm
unaffected — defterm + Start-menu activate the GUI directly, never the alias). A GUI-subsystem exe
can't own stdout (the reason `wt.exe` never returned output), so the launcher MUST be console; a
console allocated for a no-parent-console launch (our reopen `ShellExecute`) is `FreeConsole`d before
forwarding so a GUI launch never flashes one. **Targeting needs no flag:** a packaged
`agentmaster-cli.exe` resolves its profile by **package identity** (`GetCurrentPackageFamilyName` —
`agentmasterdev` → `~/.agentmaster-dev`, `agentmaster` → `~/.agentmaster`), and the alias you TYPE wins
over any inherited `AGENTMASTER_PROFILE` (the CLI clears the ambient env when packaged, so a
release-hosted shell still gets dev from `agentmasterdev`); `--profile`/`--instance` cross-target; an
unpackaged build falls back to inherited env > the `-DAGENTMASTER_DEV` compile brand > default. Wired
into `OpenConsole.slnx` + `CascadiaPackage.wapproj` (mirrors the `wt` references; the flatten step
vends `agentmaster-cli.exe` into the package beside `WindowsTerminal.exe`) + `wt.vcxproj`
`SubSystem=Console`. Live-verified end-to-end via the real `agentmasterdev` alias (every verb returns
valid JSON, auto-targeting dev). **Still to finish (NOT P2/P3):** the committed build wiring has only
been **isolation-built + XML-validated — a real full `Build-Agentmaster.ps1` / CI Release build has NOT
run**, so the wapproj integration + a properly-branded `wtd.exe` are unconfirmed and the live dev alias
runs on **hand-copied binaries** until then (needs the dev instance closed); and the **release** alias
`agentmaster show` is **not deployed** (only `agentmasterdev`). **P2 (control — `restore`/`archive` via
the existing `WM_COPYDATA` handoff + a disk-poll confirm) and P3 (`watch` event stream + prompt-driving
`enqueue`/`send-now`/`set-autorunner`) are designed + deferred** (CLI.md §4/§9).

**In-app auto-updater (`Updater.h`) — implemented + wired (startup check + hourly autocheck + cog),
header-only.** A
GitHub-release self-updater for our side-by-side packaged app, header-only pure-Win32 like
`ProfileBootstrap.h` (so the WindowsTerminal EXE includes it without the engine lib). On launch the
`WindowEmperor` runs `RunStartupUpdateCheck` **after the profile resolves and BEFORE the "Reopen your
N windows?" prompt** — a bounded (≤6 s, on a worker so a slow network can't wedge launch) GitHub-API
query for the newest release, gated to **packaged RELEASE installs** (dev/unpackaged skip unless
`AGENTMASTER_UPDATE_STARTUP` is set). When a strictly-newer version exists it shows a TaskDialog —
**Update now / Postpone (3·7·30 days) / Skip this version / Not now** (Cancel == Not now). **The same
check re-runs every 1 h while the app is running** (`RunPeriodicUpdateCheck`, the shared
`RunUpdateCheckAndPrompt` core): the `WindowEmperor` arms a plain Win32 **`WM_TIMER`** on its message
window (`_setupUpdateAutocheck`, armed during window-setup — a `WM_TIMER`, NOT the XAML
`DispatcherTimer`, which our DefaultProfile mode never starts) and each tick runs the identical
bounded-network + prompt flow on a **DETACHED background thread** so neither the round-trip nor the
modal prompt ever blocks the UI (the prompt is pure Win32 with a `nullptr` owner, pumping its own
nested loop — the same one startup uses). An **in-flight guard** (a `shared_ptr<atomic<bool>>` the
worker captures instead of `this`, so a late finish after teardown can't dangle) prevents stacking a
second prompt while one is still showing; the same **postpone / skip / channel** gates apply, so a
postponed-or-skipped version stays silent until it expires. **Update
now** materializes a BAKED-IN installer (`am-update.cmd` + `am-update.ps1`, written into the active
profile — never fetched) that downloads the `.msixbundle` + `.cer`, trusts the self-signed cert
(elevating only if needed), `Add-AppxPackage`s it (with the VCLibs-dependency fallback), and
relaunches; the app then quits (`TerminateProcess`, like the single-instance handoff — durability
rules reopen the workspace on the updated relaunch) so the package isn't in use. Only the **release**
family is ever
published, so a dev install that updates **graduates** to release. The Settings cog's **UPDATES**
section is the manual twin — a "Check for updates" button, a "vX.Y.Z available!" label, and an "Allow
pre-release versions" toggle (NOT startup-gated, available on any build; **INSTANT-APPLY** — flipping
it persists immediately via a freshest-disk RMW + re-kicks the silent check, no Save needed — a flip
followed by a backdrop-tap close used to be silently discarded, the "checkbox doesn't persist" report).
**NIGHTLY channel — a tier BELOW pre-release:** a NIGHTLY is an unstable development build whose
release TAG contains "nightly" (the naming contract, e.g. `v0.6.10-prerelease-nightly`; published as a
GitHub prerelease — `release.yml` auto-marks a nightly-named version prerelease and its prep step
strips the tag suffix to digits so the MSIX Identity Version stays numeric). Nightlies are **ALWAYS
skipped by every check** (startup / hourly / cog — even with pre-releases allowed) unless the user
opted in via the cog's **"Allow updating to nightly builds (unstable)"** switch, whose turn-ON is
**gated behind a warning confirm** (memory leaks / CPU issues / crashes; "use it to contribute and
help, but be willing to have your work suddenly interrupted") — the Toggled handler REVERTS the
switch, asks, and re-applies only on accept (so every enable attempt re-asks; Cancel leaves it
honestly OFF), then the same INSTANT-APPLY RMW (`_ApplyAllowNightly`). The TAG is the authoritative
signal (`IsNightlyTag` — contains, case-insensitive; a mis-published nightly missing the prerelease
flag is still nightly-gated, and a `/releases/latest` belt keeps one off the stable channel), the two
opt-ins are ORTHOGONAL per-tier gates (`ReleaseAllowedOnChannel`: stable always · beta ⇔ prerelease
opt-in · nightly ⇔ nightly opt-in — the list scan picks the newest ELIGIBLE release, so a
prerelease-only user skips past a newer nightly to the next beta/stable), the update prompt restates
the nightly warning inline, and `CompareVersion` stays numeric — a nightly must BUMP `X.Y.Z` past the
installed version to be offered.
State persists in `settings.json` — **INSIDE the engine's `{version, settings:{...}}` ENVELOPE**, the
schema-mismatch fix: `Updater::ReadPrefs`/`WriteUpdateState` used to read/write the TOP level, so the
startup + hourly checks NEVER saw the saved `allowUpdatePrerelease` (they queried `/releases/latest`,
stable-only, forever — with every v0.6.4+ release marked pre-release, they compared against v0.6.1 and
never prompted) and a top-level Skip/Postpone was WIPED by the next engine save (`SerializeAppSettings`
rebuilds the whole envelope). Now all four keys — `allowUpdatePrerelease` / **`allowUpdateNightly`**
(each written by its toggle's own
RMW) + `updateSkippedVersion` / `updatePostponedUntilUnixMs` (written by the prompt's RMW, EXE-safe, no
engine link) — live nested where `AppSettings` round-trips them; BOTH cog-Save preserve blocks (content
+ page sink) restore all four from disk, `ReadPrefs` falls back to a pre-fix top-level stray (the three
original keys; nightly postdates the fix and needs none), and
`WriteUpdateState` MIGRATES strays into the envelope (never drops a made choice), refuses to rebuild an
unparseable non-empty file (no clobber), and writes ATOMICALLY (temp + flush + `MoveFileExW`, the
engine's `WriteAllUtf8` recipe — was a torn-file-prone trunc `ofstream`). **"Not now" silences the
updater until the NEXT LAUNCH**: it latches a process-scoped declined-this-run marker
(`AGENTMASTER_UPDATE_DECLINED=<tag>` env var — one env block per PROCESS, unlike a per-module inline,
so the cog's DLL prompt silences the EXE's hourly timer too) that gates the startup/hourly checks
**pre-network, PRESENCE-based** — no query, no prompt, even for a NEWER release published mid-run —
while the cog's explicit "Check for updates" stays fully live (user-initiated); the latch dies at
exit, and `RunStartupUpdateCheck` **clears an inherited latch** at every fresh launch (the installer
relaunch / a child-spawned instance carries the parent's env), so a new run always asks again. Each
suppressed tick still logs (`check (periodic) skipped: declined this run`). **Observability**:
every check logs an **`[update]`** line to hooks.log via `Updater::LogUpdate` (the EXE-safe
`AppendStateLog` twin — same file, same `[HH:MM:SS.mmm]` stamp, one `FILE_APPEND_DATA` write per line):
timer armed, per-tick begin/outcome tagged `(startup)`/`(periodic)`/`(cog)`/`(cog-silent)` (incl.
postponed-skip with time left, up-to-date, available, skipped/declined suppression), every prompt
decision, installer/uninstaller launch + failures — so the hourly cadence is verifiable straight off
the log (previously fully silent). Toggle flips log `[nav] update-prerelease -> on/off`. **The whole
surface is HARDENED no-throw**: every Updater.h entry point is try/catch-logged with a safe default
(channel gate ⇒ not-channel, prompt ⇒ Not now, decision-apply ⇒ latched Not-now, installer/uninstaller
⇒ not-launched so the app never quits with nothing running, prefs ⇒ pristine defaults; the comctl
hyperlink callback and `HttpsGet`'s read loop are guarded too — the latter closed its 3 WinHTTP handles
on the throw path, a per-tick leak), `ParseVersion` clamps components (no signed-overflow UB on a
hostile tag), and an **asset-URL allowlist** (`IsTrustedAssetUrl` — only
`https://github.com/<repo>/releases/download/…`) gates both `ParseReleaseObj` and `LaunchInstaller`,
since the installer downloads + executes what those URLs point at. The cog is wedge-proof: the check
worker (a DETACHED thread — an escape is process death) and its UI completion
(`_ApplyUpdateCheckResult`) are fully guarded with button/flag recovery, `std::thread` spawn failure
restores the UI, and a fresh cog open resets `_interactiveUpdateInFlight` + the button label so a dead
check can never permanently kill "Check for updates"; the prerelease seed latch clears on every path
(a stuck latch would silently ignore all future flips). **No-silent-catch POLICY**: every catch on
the update surface either logs (the `[update]`/`[nav]` trail — incl. the `detail::` file helpers,
which log with the target path, and every safe-default gate) or is one of the two structurally
un-loggable cases, each ANNOTATED in place: the logger itself (`LogUpdate`'s own catch) and a
logger-failed nested catch (the trace's argument construction threw). Covered by the harness's
hardening suite (clamp, trust gate, synthetic-release parse, malformed/garbage/blocked-dir
robustness, LogUpdate, decision→persist→read-back loop, declined-latch presence gate + the
fresh-launch clear, full-fidelity envelope preservation, and the **nightly channel matrix** —
`IsNightlyTag` cases, the `ReleaseAllowedOnChannel` tier grid incl. "prerelease opt-in alone never
admits a nightly" + the mis-published-nightly tag-authority cases, `ParseReleaseObj` classification,
and the `allowUpdateNightly` prefs/engine round-trip + engine-save survival).

**Summary-panel JUMP ([`SUMMARY_JUMP.md`](doc/agentmaster/SUMMARY_JUMP.md)) — core complete, tested +
benchmarked + optimized; full chain lib-compiles green (TerminalControlLib + TerminalAppLib); runtime
verification pending a deploy.** Each numbered prompt in the per-tab summary panel now carries a **▸ jump
button** that scrolls the session's terminal view to **center on where that prompt is rendered**. The brain
is a **pure, header-only resolver** (`AgentMaster/PromptAnchor.h`) that bridges the transcript↔buffer
coordinate gap: it linearizes the live ConPTY buffer (soft-wrap-continuous, like `TextBuffer::SearchText`'s
own haystack), then fuzzily matches each prompt with **whitespace tolerance**, **needle backoff**, a
**partial "match as much as possible" quality score**, and an **order-preserving greedy assignment** so a
**duplicate prompt** maps to the right on-screen occurrence (out-of-order falls back to the most-recent,
flagged). A **prompt-marker preference** (`AnchorOptions::promptMarkers` = `kClaudePromptMarkers` ❯/›,
injected by `ControlCore`) binds a match to the **real user-prompt render** — Claude prefixes a SENT prompt's
line with the glyph — rather than an assistant **echo** of the same words; it is a *preference* with a legacy
soft-fallback + a no-marker-present self-disable, so it **never regresses** (SUMMARY_JUMP.md §5). A
**collision second pass** then de-conflicts the cases where several prompts resolve onto **one render** — a
**prefix** (`deploy dev please` ⊂ `deploy dev please, fast mode`), a **suffix/substring** (`deploy dev`
inside `Please deploy dev`), or a longer prompt that **backed off** onto a shorter sibling's render — by
**region containment + a quality tiebreak**: the prompt the render actually shows owns it, the others **dim**
(exact-duplicate texts are kept; the "(4) and (5) both jump to (5)" report; SUMMARY_JUMP.md §5b). Covered by
`TestPromptAnchorCollisions` (20 checks) + the marker / edge-case / real-corpus suites
(`tests/tests_summary_anchor.cpp`). The chain: overlay ▸ button → `TerminalPage::_JumpToPromptInSession` (resolves the tab's control
live) → `TermControl::JumpToConversationPrompt` → `ControlCore::ResolveConversationPromptRow` (read-only:
linearize a recent window → `ResolvePromptAnchors` → offset→row) → center via the scrollbar. **Performance
(benchmarked in the 1005-check engine harness):** per-click **~9 ms** at a realistic ~2 MB scrollback
(parity with Ctrl+Shift+F, under the read-lock), `validate` fast-path ~3.5 µs. Optimizations:
index-written normalization (~37% off the common case), a true-absence membership pre-check (~8× on a
scrolled-off prompt), a **lazy floor-hit candidate index** capping the MISS CASCADE (a floor-present
prompt whose longer prefixes are absent pays ~2 full scans instead of one per backoff length × probe
family — 3-5× on the freeze-shape cascade bench, results bit-identical, dense floors fall back to the
legacy scans; `detail::FloorHitIndex`), and a recent-window haystack cap (`kAnchorRecentWindowChars`)
bounding cost regardless of scrollback depth. **The PERIODIC icon-eligibility refresh is the part that had to be gated —
ungated, it FROZE the release app (2026-07-19; SUMMARY_JUMP.md §4a).** `_summaryTimer` is started per
overlay by `SetSummaryEnabled`, which mirrors the GLOBAL `showSummaryPanel` — *not* tab visibility — so
**every** linked Claude tab ran a full 5 s resolve, not just the visible one; with 19 live sessions and
182/244-prompt conversations (most prompts scrolled off ⇒ the expensive all-miss path) the UI thread
pegged at **~98% of a core inside `ResolvePromptAnchors`** and the window stopped pumping input entirely
(diagnosed by IP-sampling the wedged thread: 94% in that function + its STL substring searches). Three
gates now bound it, none weakening "never lose sync": **(1) an epoch cache** in
`ControlCore::ResolveConversationPromptRows`, keyed on `TextBuffer::GetLastMutationId()` + an FNV-1a
fingerprint of the prompt list (`AgentPromptListFingerprint`) — identical key ⇒ the rows provably can't
have changed; ANY buffer write bumps the id (`GetMutableRowByOffset`), the same invariant
`ReadPendingInputDraft`'s gate already ships on; **(2) a focus gate** —
`AgentTabOverlay::SetTabFocused`, driven from the one tab-switch funnel via
`TerminalPage::_SyncOverlayFocusToTab` (Manager/shell/null tab ⇒ none focused) and seeded in
`_AttachClaudeOverlay`, so only the SELECTED tab refreshes periodically and becoming focused FORCES one
immediate resolve; **(3) an interval floor** `kJumpEligibilityMinIntervalMs` (**2m30s**) that both
periodic callers (the panel's 5 s tick + the page's 30 s focused refresh) pass through, so it — not the
timers — sets the cadence. The panel REBUILD path is deliberately NOT forced (it rebuilds on every
transcript growth); the focus transition and a click ARE (both usually free via the cache — neither a tab
switch nor a viewport scroll mutates the buffer). Deferred (non-blocking): a flash
highlight on landing, "end of turn" jumps (neighbor-derived), Codex prompts, moving the eligibility
resolve off the UI thread entirely, and a context-sensitive Ctrl+F reusing the same resolve+center path.

**Pending-input monitor ([`PENDING_INPUT.md`](doc/agentmaster/PENDING_INPUT.md)) — complete: detection +
the "yes pending / no pending" observer NOTIFY + a "3 dots" animation on BOTH the tab strip and the
Triage-Board cards. Pure detector + the registry notify-on-flip unit-tested (engine harness 1354/1354);
full chain lib-compiles green (TerminalControlLib + TerminalAppLib); runtime pulse/trace pend a deploy.**
Detects an **UNSENT draft** in a Claude tab's input box — text the user typed but hasn't
submitted. This is the **ONE session fact hooks can never carry** (they fire on SUBMIT; a draft is by
definition not yet submitted), so it is the lone screen-**READ** fact: a transient draft FACT, analogous to
the presence heartbeat, **never** `SessionState` (Rule #7/#13) and strictly read-only. A **pure, header-only
detector** (`AgentMaster/PendingInput.h`, the `PromptAnchor.h` idiom — pure-ASCII source, `\u` escapes)
finds the input box by the user's two cues — the **bottom-most `❯` prompt line** that is **wrapped by `─`
rules** (a rule directly above + below) — which separates the live box from a SENT prompt (inline, no box)
and a menu selection `❯` (the line above it is the question, not a rule); it extracts the single/multi-line
draft (marker + 2-space continuation indent stripped, trailing blanks trimmed; empty box ⇒ no draft). A
read-only `ControlCore::ReadPendingInputDraft()` (guarded on `_initializedTerminal`, reads the last ~120
buffer rows under the read-lock — the box always sits at the buffer bottom regardless of scroll) →
`TermControl` passthrough; the UI lane `TerminalPage::_ScanPendingInput()` (ticked by the scanner's liveness
probe alongside `_SweepClaudeLiveness`/`_ObserverProbe`) reads each **bound, started, Claude** tab —
**background tabs too** (the point is to notice a draft in a tab you switched away from) — and, after an
**eager-show/lazy-hide clear DEBOUNCE** (2 consecutive empty reads to clear, so a mid-repaint frame can't
flicker it off), records it via `SetPendingInput` (transient `SessionInfo::pendingInput`; never persisted).
`SetPendingInput` updates the field every change but **`_notify`s ONLY on the boolean hasPending FLIP**
(empty↔non-empty — the "yes/no pending" transition; a text-only edit stays quiet, so no per-keystroke
persist/board/scheduler cascade — presence-heartbeat cadence). The flip logs `[pending] <id> draft (chars=N):
<first line>` / `[pending] <id> cleared`. **The indicator** is a **3-dot opacity pulse**: on the
**tab strip** below the status dot (`TerminalTabStatus::AgentPendingVisible` ← `_SetTabPending` directly from
the UI lane; `TabHeaderControl.xaml` `HeaderPendingDots`, its pulse storyboard started/stopped on the flag so
idle tabs animate nothing), and on the **Triage-Board cards** (`AgentManagerContent::_MakeCard` →
`BuildPendingDots`, driven by the flip notify rebuilding the board — **cross-window**: a draft in window A
shows on window B's GLOBAL board). **The dots' COLOR is a user-configurable LIGHT/DARK contrast PAIR**
(`AppSettings::pendingDotsLightColor` / `pendingDotsDarkColor`, GLOBAL, two cog `muxc::ColorPicker`s under
TABS; defaults gold `#FFE0A92B` on dark / deep amber `#FF5A3E00` on light): the dots are auto-painted the
DARK color on a LIGHT tab background and the LIGHT color on a DARK one, picked by the **WCAG luminance** of
the session's per-directory tab color (`AgentStatusColors.h` `BackgroundIsLight` / `PendingDotsColorFor`, the
~0.179 crossover the title-band `PreferDarkTextOn` uses) — so the dots are **never invisible** against the
tab/card. The tab strip carries the picked brush on `TerminalTabStatus::AgentPendingBrush` (set by
`_SetTabPending`, contrast-picked from the per-dir color each scan tick); the board card body is the always-
dark Manager fill, so cards use the LIGHT color. Applied live + cross-window via the `flashRingColor` settings
idiom. **Follow-ups:** an off-switch setting, placeholder/dim-attribute filtering, and a `pauseOnHumanInput`
autorunner tie-in (PENDING_INPUT.md §4/§6).

**Bookmark TAGS — user-named, colored labels on a session, shown as little BOOKMARK RIBBONS on its tab +
everywhere the session appears; lib-compiled green + engine-tested (1523/1523 incl. tag CRUD, the tag-colors
store, and the known-tag registry). Rides the next deploy cycle.** A tag is a durable, case-insensitive
name a user attaches to a session; a session can carry many, and each shows as a small bookmark glyph
color-coded to the tag. Persistence is three files under the ACTIVE PROFILE, all in `SessionStore.{h,cpp}`
(the generalized per-session KV): the per-session list (`session-store/<sid>.json` `tags` key,
JSON-array-encoded), the **known-tag registry** `tags.json` (a durable, CI-deduped array of display-cased
names — the reason a tag SURVIVES losing its last carrier, below), and `tag-colors.json` (folded-name →
`#AARRGGBB`, the picker's chosen colors). The GLOBAL tag universe is `CollectGlobalTags(sessions ∪ registry)`
— fold-merged, each tag stamped with max-carrier-activity + carrier count, sorted activity-desc; a registry
tag no session carries lists at **·0** (sorts last).
- **The tab badges (TAB_OVERLAY-adjacent, `TabHeaderControl`).** Every managed tab renders one bookmark
  **ribbon per tag**, hosted in an **always-open PARENTED `Popup`** (`HeaderTagBookmarksPopup`) so the row can
  **OVERHANG the tab's bottom edge** — ~30% of each 6.5×9.3 ribbon above the tab's bottom line, ~70% hanging
  BELOW it (the "bookmark out of the book" look). The overhang is only renderable because a parented popup's
  child draws in the island's POPUP ROOT, outside the tab strip's ScrollViewer clip (whose viewport bottom IS
  the tab's bottom edge — an in-tree element can't paint past it). The row is fed by
  `TerminalTabStatus::AgentTagsSpec` — the producer `TerminalPage::_SetTabAgentTags` resolves each tag's color
  ONCE (user-picked > name-hash) and writes `'\n'`-joined **`name\t#AARRGGBB`** lines, so every consumer (the
  badges, the tooltip chip row) parses the same resolution with no store I/O. `_UpdateTagBadges` rebuilds
  (change-gated on `_renderedTagsSpec`, cap 20); `_PositionTagBadges` places + tracks it. Hovering a ribbon
  raises `TagBadgeHoverBegin/End` → the page's **rich hover panel** (`_ShowTagHoverPanelNow`): a popup listing
  EVERY session carrying the tag with its status dot (the Triage palette for a live session, a hollow gray ring
  for a closed one), and clicking a live row JUMPS to its tab (`_ActivateClaudeSession`, cross-window) — richer
  than a `ToolTip`, which can't take clicks. A popup-hosted badge no longer bubbles presses to the tab, so a
  badge `Tapped` explicitly selects it (click-to-switch preserved).
- **The Tags panel (`_OpenTagEditorAt`, an islands-safe raw `Popup` parented into `Root()`).** A `[name box | +]`
  row (Enter or **+** adds), a **color-picker swatch row** (one swatch per palette color, the pick ringed white
  + painted onto the **+**; **pre-picked RANDOM on open and re-rolled after every add**), the global tag list
  (each row `[✓][ribbon][name][·count]`), and a "K of N tags" footer. A NEW tag takes the current pick + enters
  `tags.json`; an EXISTING tag is recolored only by an EXPLICIT swatch tap (never the roulette). **TAG REMOVAL
  IS EXPLICIT** (Correctness Rule #17): toggling a tag off its LAST session keeps it listed at ·0 (re-appliable);
  only the **✕** on a 0-carrier row deletes it (`_RemoveGlobalTag` → `UnregisterKnownTag`; a carried tag shows
  no ✕ and can never be deleted — the derived union keeps it alive; its color entry is kept so a re-created tag
  regains its color). Opened from the **WT tab context menu** ("Tags"), a **Sessions-page row** right-click, and
  a **Triage-Board card / Explorer-tree row** right-click (`AgentManagerContent::SetTagsHandler` →
  `_OpenTagEditorForElement`, anchored under the clicked element).
- **Everywhere else.** The rich **tab TOOLTIP** shows a tag-chip row (each name over a 2px **colored underscore**
  — XAML can't color a text-decoration underline separately, so a thin colored `Border` is the practical
  underscore). The **Sessions browser** gained a **Tags column** (col 5, after Branch) rendering the same
  hoverable ribbons + the same hover panel, plus a filter-chips row under the search toggles (gently-rounded
  rectangles, `_RebuildSessionsTagChips` — a tag facet that ANDs with the search/scope filters). A global
  **`AppSettings::maxTags`** (default 20, ceiling 40; Settings cog → TABS) caps only NEW-name creation.
- **Islands crash lessons (both in Gotchas).** The popup cost two `0xC000027B` fail-fasts, both now excluded by
  construction: **(1)** opening the popup on a not-yet-rooted header throws (`E_UNEXPECTED`, no `XamlRoot`) — a
  restored tagged tab asserts its badges BEFORE its header enters the tree — so the open is gated on
  `IsLoaded()`+`XamlRoot()` + re-armed on `Loaded`; **(2)** mutating the popup SYNCHRONOUSLY from a
  layout-driven trigger (`SizeChanged`/`LayoutUpdated`/`ViewChanged`) re-enters the pass and trips XAML's
  `E_LAYOUTCYCLE` (`0x88000FA8`) detector — so `_PositionTagBadges` is now a **coalescing scheduler**
  (`_badgeReposQueued` + one dispatcher tick) and the reads + gated mutations run in `_PositionTagBadgesNow` on
  a CLEAN tick after layout settles. **Cross-window caveat** (like the favorite crown): badge/chip/column paints
  are same-window instant; another window catches up on next bind/launch/gather (the panel/editor/chips always
  read the store fresh).

**Slash-command bindings + /handover + its in-place twin /handover-here + its standby member
/handover-standby
([`COMMANDS.md`](doc/agentmaster/COMMANDS.md)) — implemented +
HARDENED, delivery = FULL CONTENT INJECTION (never truncated), RESTART-RESILIENT (durable
per-session progress) + MULTI-FILE (one command's several HANDOVER files consumed as one):
engine-tested (2521/2521 — the
`TestCommandWatch` units + safeguard belts + the content-injection/tier units + the /handover-here
twin units (hyphen echo, name-exact binding isolation, its own definition file) + the
/handover-standby units (§5b — its own definition file + digest gate + fill-not-send sentinels +
no-bare-sibling-token render check, the THREE-way binding isolation ["handover" is a PREFIX of
"handover-standby" — the exact aliasing hazard], the family-supersede pivot onto the standby path,
`BuildPromptFill` [bracketed paste, NO trailing CR ever; `BuildPromptSubmission == fill + "\r"` so
the channels can't drift], the standby settings round-trip) + the multi-file
collect/seal/settle units + the durable-progress units (watermark / marker revival / prune /
encode-decode) + the family-supersede race-guard units + the SHA-256 definition-history units
(NIST vectors + padding edges; the create/upgrade/never-overwrite policy over a synthetic command;
the `last digest == sha256(current text)` version gate; the three histories pairwise disjoint) + the §6a
CUSTOMIZATION units (name normalize + the `ResolveCommandNameTriple` heal [defaults, every collision
direction incl. the squatter-eviction cascade], the render↔identity inverse over synthetic AND real
texts, the named ensure/remove + rename/disable reconcile policy, the AppSettings round-trip incl.
marker semantics) + the §6b SHAPING units (RegexUtil's never-throw/caps/backrefs contract, the
successor-title rewrite's five fallback paths, the authoritative-vs-invalid leaf-match regex, the
shaping settings round-trip), the FABRICATED
end-to-end `/handover` session `TestCommandHandoverE2E` (incl. the multi-file scenario F, the
restart-persistence scenario G + the family-race pivot scenario H), and the REAL-corpus echo replay
`TestCommandEchoRealCorpus`) + lib-compiled green; rides the next deploy cycle.** Bind to `/commands`
the user TYPES into a managed Claude session and AWAIT the session's
follow-up activity — async, bounded, zero state-machine impact. A typed command's transcript ECHO (a
`type:"user"` line carrying `<command-name>/x</command-name>` + `<command-args>` — current Claude Code
writes even built-ins this way; the older `system/local_command` stratum parses too, order-agnostic
`ParseCommandEcho`) now surfaces from `ParseTranscriptDelta` as an ordered, NON-turn
**`Kind::Command`** event (the echo stays NOISE for the state machine — the /model false-Running fix is
byte-identical, pinned by test), alongside assistant **`fileWritePaths`** (Write/Edit tool_use
`file_path`s). The scanner feeds both + turn boundaries + a per-pass Tick into the process-wide
**`CommandWatch`** (`AgentMaster/CommandWatch.{h,cpp}`, `Engine::commandWatch`): a BINDING says "when
/name is sighted, await X, then fire" — v1's await shape is the **markdown await**, now
**MULTI-FILE**: every hint-matching `.md` Write/Edit after the command is COLLECTED in write order
(deduped; a batch's first md only as the nothing-collected-yet fallback — an incidental doc edit
never rides along), the collection **SEALS at the first turn end after a match** (+ a 20s
write-silence settle fallback — `kCommandMatchSettleMs` — when no turn end ever arrives), and the
fire is gated on EVERY collected file actually existing on disk (a tool_use only proves the request;
a write may sit behind an approval) — ONE fire carrying the whole path set ('|'-joined through the
fan-out, `Join/SplitWatchPaths`; `IsSaneWatchPath` rejects `|` per-path so the separator is
unambiguous). Replay-proof by the caught-up-cursor gate + **durable per-session PROGRESS (COMMANDS.md
§3a — restart resilience)**: the SessionStore KV's `cmdProgress` key (injectable seam
`SetProgressStore`; `Encode/DecodeCommandProgress`, `"v1;p=<ms>;a=<cmd>@<ts>,…"`) carries the **fired
watermark** — advanced INSIDE the fire path BEFORE handlers run, so a replayed echo at/under it NEVER
re-fires (at-most-once across a restart; closes the resume-a-just-handed-over-session and
crash-after-fire double-successor holes the 60s freshness gate alone could not) — and the **armed
markers**, which REVIVE a sighting that was mid-await at shutdown even past the freshness window
(deadline anchored at the ORIGINAL echo timestamp, so the 15-min bound stays absolute; past-deadline
markers prune at load; markers retire on fire/expiry/eviction and override the watermark for an
out-of-order fire). An echo neither fresh nor marked never arms (foreign/deep history stays inert);
same-echo idempotence never double-arms one line; bounded everywhere (2 turn-ends for an unmatched
sighting, 15-min deadline, per-session cap, FIFO — the seal pairs each command's writes with its own
turn; pendings stay transient in-memory, the PROGRESS is the durable half). **SAME-FAMILY SUPERSEDE
— /handover and /handover-here never race each other's files:** bindings sharing a leaf hint are ONE
logical operation with different handling paths, so a new family sighting RE-AIMS a still-unsatisfied
older await (an unmatched predecessor is superseded — marker retired, the newest handling path takes
the next write, so a /handover pivoted to /handover-here fires the in-place path, never a stale
new-tab spawn; a matched-but-unsealed one defensively seals and fires with its own files; SEALED
pendings are untouched — every satisfied command still fires) — at most ONE unsealed family pending
exists per session, so a family write has exactly one possible owner. A same-command re-run
supersedes too (a retry is one operation). **The family is USER-CUSTOMIZABLE (COMMANDS.md §6a — the
Settings cog's new "Commands" tab):** each command can be RENAMED (the typed word == the definition's
file leaf `<name>.md`; stored normalized `NormalizeCommandName` + collision-healed
`ResolveCommandNamePair` — the two names can never collide) and DISABLED (no definition materialized,
no binding registered), both **applied at the NEXT START** (bindings register once at engine init; the
tab's status lines stage `/old → /new after restart` off the engine-owned `command*MaterializedName`
markers). A rename renders the definition text for the new name (`RenderShippedCommandText`,
word-boundary token substitution) while the digest HISTORY stays default-name — identity checks
substitute the name BACK before hashing (`NormalizeCommandBytesForIdentity`), so ANY shipped version
is recognized under ANY name (custom-named pristine files still auto-upgrade); the init reconcile
(`ReconcileHandoverCommandFiles`) then migrates a renamed/disabled command's OLD file away — deleted
ONLY when byte-identical to something we shipped (a user-edited file is NEVER touched) — and the
fan-out action names stay canonical `handover`/`handover-here`, so a rename never reaches the UI
layer. (Two-install caveat: dev + release share `~/.claude/commands` with separate settings — the
other install's init re-materializes ITS configured names.) **SUCCESSOR SHAPING (COMMANDS.md §6b —
the Commands tab's second half) configures what a handover PRODUCES**, consumed at ACTION time so it
applies to the NEXT handover right after Save (no restart) except where noted: a **per-command
successor MODEL** (`commandHandoverSuccessorModel`/`…Here…`; `""` == Default, else this launch's
`--model <id>` through the existing launch-model seam — `_LaunchClaudeSession`'s `modelOverride` and
`_RestartTabIntoFreshSession`'s new one; the cog lists Default + `launchModels`, an unlisted stored id
shown `(custom) <id>` — **overridable PER MESSAGE by a leading model word in the typed command**:
`/handover [fable] do a b c` / `/handover fable 5: fix x` — `PickModelFromArgsHint`, pure + tested:
partial + caseless + characters-only fold matched as a SUBSTRING of EITHER side of every launchModels
entry (display name / model id), first entry wins; brackets optional — bare form needs the FIRST word
to hit alone (≥3 folded chars, so a stray "a"/"do" never picks) then greedily extends ≤4 words while
still matching, longest wins; only the args' first line's leading words are consulted, nothing is
stripped from the text; the echo's `<command-args>` now rides the whole fan-out — `CommandActionSink`/
`RaiseCommandActionInWindows`/the per-window sink/`_HandleCommandHandover` gained an `args` leg —
logged `[handover] <sid8> successor model from the message hint: <id>`), a family **TITLE REWRITE** (`commandHandoverTitleFindRegex`/`…TitleReplace` →
the pure `DeriveHandoverSuccessorTitle`, which returns `""` on unset/invalid/no-match/blank-result so
the classic `"(handover)"` naming is the fallback — the rewrite can only IMPROVE a title, never lose
one; `$1` backrefs, trimmed, 255-capped, still uniqueness-bumped), a family **FILE-MATCH regex**
(`commandHandoverFileMatchRegex` → `BindMarkdownAwait`'s new optional `leafMatchRegex`: a VALID
pattern is authoritative over the leaf hint — case-insensitive search on the file NAME — while an
invalid one falls back to the shipped hint, belted at bind time (logged) AND per leaf; a VALID
pattern also SUPPRESSES the legacy first-markdown fallback, so an unrelated `notes.md` can never
become the briefing; **RESTART-applied**, bindings register once; the §3 supersede family key stays the leaf HINT so the two commands
stay one family), and **DELETE-AFTER-HAND-OFF** (`commandHandoverDeleteFileAfterLaunch`, default ON on
a FRESH install — the §6c scratchpad pairing below; an install that stored OFF keeps it)
— **DEFERRED to a successful START, not fired at spawn**: `_HandleCommandHandover` only ARMS
(successor id → md path) and `_SweepHandoverDeletes` (same liveness tick as the paste pump) deletes
once `SessionInfo.started` — because a successor in a BACKGROUND tab starts LAZILY and a launch that
never comes up must keep its briefing on disk. Gone/archived-before-start ⇒ KEEP the file; past the
10-min deadline ⇒ KEEP; delete failed ⇒ logged, kept. Never the POINTER tier (its first message NAMES
the file), never a failed spawn — the answer to `HANDOVER-*.md` litter. All user-typed patterns
run through the ONE shared **`AgentMaster/RegexUtil.h`** (header-only + pure, the `PromptAnchor.h`
idiom): `RegexIsValid`/`RegexSearch`/`RegexReplace` never throw (invalid ⇒ no-match/unchanged), cap
pattern (512) + input (4096), and fix one flavor (ECMAScript, search semantics, optional
case-insensitivity, `$1` backrefs, replace-ALL); its catches are the documented Rule #18
*expected-control-flow* exemption (an invalid pattern mid-edit is normal — the cog's live status line
is the reporting channel). **The three regex settings ship SEEDED with their real defaults** (`kDefaultCommandTitleFindRegex`
/`…TitleReplace`/`…FileMatchRegex`, presence-gated on load like `launchModels`) — the cog's boxes show
the ACTUAL rule instead of hiding a code fallback, a CLEARED box means "use the built-in behavior", and
the default title pair reproduces the classic naming EXACTLY *including* the chain-bump (it eats an
existing `(handover N)` suffix and re-adds it, so the uniqueness bump walks `(handover 2)` instead of
STACKING `(handover) (handover)` — the DeriveForkTitle bug), while the default FILE MATCH is
**`HANDOVER\-`** — the regex spelling of the definitions' own `HANDOVER-<topic>.md` contract, and
deliberately TIGHTER than the contains-`handover` leaf hint a cleared/invalid box falls back to (a doc
merely mentioning handover — `handover.md`, `old-handover.md` — is no longer collected as a briefing;
a lone mis-named file still rides the first-markdown tolerance, which the default keeps ON). Because
the default is now a VALUE, the
first-markdown-tolerance policy moved to the CALLER (`BindMarkdownAwait`'s `allowFirstMarkdownFallback`,
Engine passes `pattern == default || empty`). The cog also gained a **per-tab Reset** (footer, left of
Cancel — `_settingsTabResets`, index-aligned; shown only for a tab that registered a handler, today just
Commands, the rest passing `nullptr`): it restores that tab's CONTROLS from a default-constructed
`AppSettings` and touches no disk (Save commits, Cancel discards — hence no confirm).
**WHERE the briefing is WRITTEN is now a setting too (COMMANDS.md §6c — the Commands tab's
"Handover file location", a free-typed box + a preset menu):** the definitions' hard-coded "in the
current working directory" moved onto a rendered **`WRITE IT IN: <phrase>`** line, and the shipped
default is the session **SCRATCHPAD** — a briefing's CONTENT is what reaches the successor, so the
file is a courier that has no business landing in a repo (the `HANDOVER-*.md` litter, at the
source). `commandHandoverWritePath` is **FOLDER-ONLY + family-wide**: the `HANDOVER-<topic>.md` NAME
contract every other stage keys on (the file-match regex, the one-successor-per-file fan-out,
delete-after) is untouched, so nothing else changes — `scratchpad`/`""` = the temp scratchpad · `./`
= the working dir (the old behavior, kept as a preset) · `./docs`, `./handovers`, an absolute path =
that folder, created if missing (`NormalizeCommandWritePath` drops anything that could break the
one-line instruction out). It renders through the SAME trick as the custom name
(`RenderShippedCommandWritePath` ↔ `NormalizeCommandWritePathBytesForIdentity`, exact inverses) —
except the inverse is **DELIMITED** (marker → end of line), so it needs NO per-install marker and
recognizes a hand-changed location; identity tries TWO candidates (`MatchShippedCommandVersion`:
name-normalized bytes match any HISTORICAL version verbatim — pre-§6c texts carry no marker, so the
fold is a no-op on them — and the additionally location-normalized bytes match the CURRENT version
under a custom folder), which is what keeps every pristine older file auto-upgrading. Because only
the TEXT changes (never a binding), the cog's Save **applies it immediately**
(`RefreshHandoverCommandWritePath` re-renders the LIVE definition files, keyed on the engine-owned
MATERIALIZED names — a rename stays restart-applied so a file and its binding can never disagree
mid-run), so the rewrite rule generalized to **"ours AND not already exactly what we would write"**
(one predicate covering a version upgrade, a name re-render and a location re-render; a
byte-identical file stays a no-op). Picking the **Scratchpad** preset also ticks delete-after. The
tab's new **COMMAND DEFINITION FILES** section states what each file on disk IS
(`InspectHandoverCommandFiles` → up-to-date / managed / **EDITED BY YOU — left alone** / not
installed, sampled at cog open, not per keystroke) — because a hand-edited definition is frozen
forever and therefore silently STOPS following these settings — and offers the confirmed
**Reinstall definition files…** (`ForceReinstallShippedCommandFileNamedIn`), the ONE path that
overwrites regardless of digest. ⚠ Every FUTURE definition version must keep the `WRITE IT IN: `
marker with its phrase on ONE line, or it silently stops honoring the setting.
**The `/handover <context-or-filepath>` integration:** engine init materializes the command DEFINITION
`<claude-config>/commands/handover.md` (**create-if-absent + a VERSION-AWARE UPGRADE gated on SHA-256 —
the ONE write outside the profile**: the shipped history is a list of **DIGESTS**
(`ShippedHandoverCommandHashes` — the SHA-256 of each version's UTF-8 bytes as written to disk, oldest
first, the LAST entry being the digest of the CURRENT text `ShippedHandoverCommandText()`; append-only,
frozen forever), so the on-disk file is hashed (`AgentMaster/Sha256.h`, pure + header-only, hand-rolled
like `Base64Encode` so no bcrypt has to be threaded through lib + harness + CLI) and a match on a PRIOR
entry — a pristine older OURS — silently upgrades to current, while anything user-edited (or already
current, whose digest is the LAST entry, never a prior one) is NEVER touched — the ApplyEnvDefaults
discipline; a deliberate additive `~/.claude` mutation. Digests, not texts: recognizing "a version WE
shipped, unmodified" is a content-IDENTITY question, so a superseded version costs ONE line instead of a
frozen 2–3 KB literal (retired texts live in git history), and the harness's
`last == sha256(current text)` gate — whose failure message PRINTS the digest to append — makes the
history self-maintaining: a text edit that forgot its digest fails the suite instead of silently
orphaning the upgrade rule. The current V6 instructs
Claude to Write each `HANDOVER-<topic>.md` AS a SELF-CONTAINED direct briefing TO its successor —
because EACH file's content becomes a DIFFERENT successor's first message (the FAN-OUT: N files in
one turn = N parallel successor tabs, in write order; "never write 'continue in file B'") — then
end the turn, and carries
the SELF-INVOCATION guard (a MODEL-invoked Skill call writes no `<command-name>` echo, so the watch
never arms — the guard makes a self-invoked model write nothing and redirect the user to TYPE the
command; handover-here V3 same)) and binds
`handover` → the new per-window **command-action
sinks** (`Engine::CommandActionSink`, the activateSinks idiom — registered at page init,
token-detached in `~TerminalPage`). The hosting window's `_HandleCommandHandover` spawns the
successor: same **effective working dir**, titled `"<origin> (handover)"` via the generalized
**`DeriveSuffixedTitle`** (DeriveForkTitle now delegates to it; registry-bumped so sibling handovers
never collide) — **FAN-OUT: EACH collected md starts its OWN successor tab** (write order ==
strip order, sequential slots beside the origin; per-path sanity + existence re-asserted, a
vanished subset drops with a log, the survivors proceed), each file **delivered VERBATIM and
IN FULL as ITS successor's FIRST USER MESSAGE** ("as if the user typed it") — **NEVER truncated**:
one command writing N files hands off to N parallel successors.
`ReadHandoverDocumentPrompt` reads (4 MiB sanity cap per file) + normalizes (BOM strip, CRLF→LF, C0
controls dropped — which also makes the paste framing injection-proof, ESC can't survive — trimmed);
each file's DELIVERY then tiers on its own pure `PsEscapedCost` vs
**`kHandoverPromptEscapedBudget`** (11,500
escaped chars — the pwsh `-EncodedCommand` wrap costs ≈2.67× and both CreateProcessW hops cap at
32,767; ` `` ` `"` `$` cost 2): **fits** ⇒ the launch commandline's positional prompt
(`BuildClaudeCommandline(..., initialPrompt)`, PS-quoted `PsDoubleQuote` for the pwsh-host `&` context
— a PS double-quoted string legally spans newlines; zero-race: nothing typed into the TUI, a real
`UserPromptSubmit` fires so Running + the record ride the push path; structurally dropped on any
restore/resume); **over budget** ⇒ the FULL document rides the **ConPTY stdin instead — no size
ceiling**: parked as a Pending prompt at the FRONT of the successor's queue (durable + visible in Auto
Testing, Send-now-able, restart-safe) and paste-injected by **`_PumpHandoverInjections`**
(scanner-ticked; waits for `SessionInfo.started` — a pre-Connected `WriteInput` silently drops — + a
1.5s settle, then the Send-now recipe: mark Sent → `Inject(BuildPromptSubmission(text))` — the
existing bracketed-paste ONE-block submit — rollback on failure, echo dedup + the Enter-retry watchdog
backing it like any flight prompt; 10-min give-up leaves it Pending, never lost);
**unreadable/whitespace-only/beyond-cap** ⇒ that successor gets the pointer-style prompt fallback.
`handover-done` carries parallel per-file lists (`new=<sid8>,<sid8> … inject=content,paste`;
a failed spawn logs `(failed)` at its position). Repeatable — every /handover in a conversation
spawns its own successor(s). Logs: `[cmd]`/`[cmd-fire]`/`[cmd-expire]` + the `[nav] handover-begin ↔
handover-done` pair. **The `/handover-here <context-or-filepath>` twin (COMMANDS.md §5a)** reuses this ENTIRE
pipeline — its own definition `handover-here.md` (`EnsureHandoverHereCommandFile` /
`ShippedHandoverHereCommandHashes` + `...Text()`, all family files through the ONE shared
`EnsureShippedCommandFileIn(configDir, leaf, shippedHashes, currentText, label)` core so the write
policy can't drift, and the digest histories are asserted pairwise DISJOINT so none can cross-upgrade
another's file), the same markdown await (same
"handover" leaf; the watch's name-EXACT binding lookup keeps the family from cross-firing), the same
guards/title/tiers/fan-out — but **the FIRST file's successor REPLACES the origin tab IN PLACE**
(additional files' successors open beside it): the
hosting window's `_RestartTabIntoFreshSession` runs a **"New Session Here → Default" spawn through
the Restart-session swap** (`BuildClaudeSpawn` fresh minted id / settings model / same effective
dir; `_RestartManagedSession`'s recipe — tabToken-matched pane, NotConnected guard,
`HardResetWithoutErase` + `Connection(newConn)` + `Start()`, inheritCursor so the origin's
scrollback stays readable, `SuppressAutoClose` re-applied), then ONE `_BindClaudeSessionToTab`
re-home archives the ORIGIN (live=false, injector cleared — resumable from the Sessions browser,
Close semantics) and re-keys tab/overlay/injector/title onto the successor (started=true
immediately, so the paste tier works unchanged); any refusal/failure **degrades to the classic
new-tab spawn** (`(fallback=new-tab)`). Logs: `[nav] handover-here-begin ↔ handover-here-done` +
`[handover-here] <new> replaced <old> in place` + the re-home's `[rehome]`/`tab-swap`. **The
`/handover-standby <context-or-filepath>` member (COMMANDS.md §5b)** is /handover with the SUBMIT
withheld: its own definition `handover-standby.md` (`EnsureHandoverStandbyCommandFile` /
`ShippedHandoverStandbyCommandHashes` + `...Text()`, same core/policy), the same await/family (a
/handover pivoted to /handover-standby fires the standby path — one §3 supersede family of three),
new successor tabs like /handover — but each briefing is **TYPED into its successor's input box and
NEVER sent** ("a handover in standby": one Enter away, nothing runs until the user presses it). The
commandline tier is structurally excluded (a positional prompt auto-submits); every standby file
(content AND pointer) rides the pump's **STANDBY lane**: `Inject(BuildPromptFill(text))` — the
bracketed paste with NO submit CR (`BuildPromptSubmission == BuildPromptFill + "\r"`, so the two
channels can never drift) — deliberately NOT a queue row (a Pending row could be auto-SENT by a
Full autorunner, a Sent row would arm the Enter-retry watchdog — either defeats standby), then the
fill is **VERIFIED by reading the input box back** (`ReadPendingInputDraft`, the PENDING_INPUT.md
primitive — no echo ever confirms a fill): box non-empty ⇒ verified (logged `draft VERIFIED … one
Enter away`; the §6b delete-after arms ONLY here — an unverified draft always leaves its briefing
file, standby's one durable copy); box still empty after 12s ⇒ the TUI ate the paste pre-raw-mode ⇒
re-fill (≤2 attempts, then give up + keep the file); a box already holding USER text is never
touched (no append-to-a-human-draft, the 10-min deadline caps); a session the user DROVE is
never filled/re-filled — the pure `StandbySessionTakenOver` latch, BOTH phases: a turn in
flight now OR proof one ever ran (`turns.lastPromptUnixMs`/`convLastActivityUnixMs`, both 0 on
a fresh successor until a real submit — closing the fast-turn hole where a submit+complete
between ticks reads as an eaten paste and would re-fill the delivered briefing) ⇒ hands off,
file kept. The pending-input monitor's "3 dots"
then mark the standby tab for free (the draft IS a pending input). Full settings suite: its own
Commands-tab section (enable + rename + status + Successor-model combo,
`commandHandoverStandby*`), every family-wide §6b/§6c rule (title rewrite · file match · write
location · delete-after) and the per-message model hint apply. Logs: `[nav]
handover-standby-begin ↔ -done inject=standby|pointer-standby` + the `[handover-standby]`
armed/FILLED/VERIFIED/kept lines. (This work also fixed a latent race: `_PumpHandoverInjections` +
`_SweepHandoverDeletes` ran on the SCANNER thread while `_HandleCommandHandover` wrote their maps
on the UI thread — both now self-marshal, fire_and_forget + Impl, which is also what makes the
standby lane's UI-affine control read legal.) **Safeguards (COMMANDS.md §7, all under the never-lose-a-swallowed-exception policy):** every
CommandWatch feed is a SELF-CONTAINED function-try (a watch bug / throwing handler / throwing probe can
never cost the scanner a pass), handlers caught PER FIRE, a throwing probe reads "file absent"
(retried), the **sane-path gate** (`IsSaneWatchPath` — control chars / quotes / oversize rejected AT
THE MATCH, since the path flows into logs + the successor's launch prompt), per-sink catch in the
fan-out (a dead window's dispatcher can't stop the host window's sink), the UI dispatch lambda +
`_HandleCommandHandover` guarded (`AgentLogCaughtException`) with the md's existence + path sanity
RE-ASSERTED at action time (a vanished md drops with `[handover] … md vanished` instead of spawning a
successor pointed at nothing), and the command-definition write best-effort (engine init never
derails). **Coverage (COMMANDS.md §8):** a fabricated expected-behavior `/handover` transcript (real
ISO timestamps) replayed through the REAL parser + the `_readDelta` feed mapping + the DEFAULT disk
probe — happy path (asserted event shape, one fire), clarification round, no-md expiry, TWO handovers
in one conversation, stale restart replay (never arms), and chunked scanner-style parse equivalence —
plus a guarded REAL-corpus sweep (newest ~120 transcripts: at authoring 54 user-echo + 22 system-echo
+ 60 Write-tool lines — every echo parses to a Command event, ZERO turn-event leaks corpus-wide).
Deferred: a hook push fast-path, more bindings/await shapes, a cog off-switch (COMMANDS.md §10).

What works, by area:
- **Engine (M5, `AgentMaster/`; M9 process singleton).** Thread-safe `SessionRegistry` (single
  source of truth; **token-based** observers — `AddObserver`→token + `RemoveObserver` — and
  multiple `AddAdoptionHandler`s, so a closing window detaches its lens observer + adoption
  handler cleanly instead of dangling on the shared registry), `HooksBridge` (local named-pipe
  server `\\.\pipe\agentmaster.<pid>`), `ClaudeSpawn` (spawn/`--resume` recipe + the shared hooks
  config + PowerShell forwarder). **M9:** one process-wide **`SharedEngine()`** (`Engine.{h,cpp}`)
  owns the registry/bridge/scheduler for ALL windows (the WindowEmperor is one process, N windows
  on N threads); each `TerminalPage` copies the shared `shared_ptr`s and its Manager tab is a
  per-window *lens* over the one fleet. The engine also carries the per-window **activate sinks**
  (`Engine::RegisterWindowActivateHandler` / `ActivateSessionInOtherWindows` — the cross-window
  Activate fan-out; see *C1 UI*), registered at engine init and token-detached in `~TerminalPage`
  like the rest. The `<pid>` pipe is unambiguous *because* there is exactly
  one bridge; one writer for `sessions.json`; restore loads process-once
  under a load **barrier** (`Engine::restoreMutex` — a 2nd window blocks until the fleet is fully loaded, then
  skips, so it can't double-load NOR race its tab re-home against a half-loaded registry).
  Hooks → wire line → registry → hook-driven `SessionState` (Correctness Rule #1). The wire
  line carries a 7th **`tabToken`** field (the hosting `WT_SESSION`, for adopting a hand-typed
  `claude` — see *Adopt any `claude`* below), an 8th, **escaped `prompt`** field on
  `UserPromptSubmit` (`WireEscape`/
  `WireUnescape`: `\ \t \r \n`), so the registry records **every** message a session got — a
  prompt typed straight into the ConPTY becomes a `Sent`/`Typed` Auto-Testing entry, while the
  `UserPromptSubmit` echo of a prompt WE injected is recognized (text + a recency window + the
  transient `QueuedPrompt::echoed` flag) and NOT double-recorded, and machine-injected
  protocol/control traffic is noise-gated OUT of the Typed record (`IsNoiseUserPrompt` at the
  registry seam, the SAME filter the scanner's back-fill applies — a TEAMMATE-message delivery
  fires a REAL `UserPromptSubmit` on the lead per report/idle notification, which used to fill the
  SENT list + sessions.json with wrapper spam [8 wrapper rows found PERSISTED in the prod registry
  — the bug's live fingerprint]; the wake turn still drives state, only the record is filtered.
  Corpus-audited across BOTH delivery strata — the newer `"Another Claude session sent a
  message:\n<teammate-message …>"` preamble shape AND the older bare `<teammate-message …>` block
  (486 bare + 21 preambled real deliveries, 507/507 filtered, 0 leaks; a human prompt merely
  MENTIONING the tag mid-text is kept — prefix-anchored, never substring)) — and a trailing 9th **`ts`**
  field (the hook's FIRE time, stamped by the forwarder before its slow Stop-path transcript
  work; old 8-field lines parse with ts=0 → arrival order). `ts` drives the **ordered state
  machine** (`NextSessionStateOrdered` + `SessionInfo.turns`, HOOKS.md *State machine*): a
  **stale Stop** (fired before the newest prompt — the slow Stop forwarder lands it after the
  next turn's `UserPromptSubmit`) keeps state + suppresses its question-bit/advance, and a
  **type-ahead** prompt (`UserPromptSubmit` at Enter-time mid-turn; the queued batch then runs
  as the next turn with NO further hook) is counted so that turn's `Stop` stays **Running**
  instead of stranding the whole follow-on turn in `WaitingForInput` — the "second turn never
  shows Running" bug. The scanner's synthesized missed-Stop is `quiescentStop` (≥2s-quiet
  transcript): always lands `WaitingForInput`, never stale, never held by the queue. Hook `ts`
  also refreshes `lastActivityUnixMs` monotonically (real hooks previously never updated the
  Waiting→Idle decay anchor — it only moved on synthesized events). The scanner's missed-Stop
  reconciliation is **generalized** past the bare end_turn/Running case (each proved against
  live sessions): (a) **interrupt** — a user-abort marker (`[Request interrupted by user…]`, Esc;
  fires no clean `Stop`) is a turn-ender (`IsUserInterruptMarker` → `ScanState.interrupted`), so a
  killed turn no longer shows Running forever; (b) **blocked-on-user** — an UNANSWERED interactive
  tool_use (`AskUserQuestion`, surfaced by `ParseTranscriptDelta`'s new `toolName` +
  `IsInteractiveTool`) on a quiescent transcript synthesizes a permission-style Notification →
  **`NeedsApproval`** (`ShouldSynthesizeBlockedOnUser` → `[recon-block]`), so a session blocked
  waiting for your answer reads "needs you", not Running (a pending NON-interactive tool — a long
  Bash — still reads Running); (c) **needs-approval exit** — `ShouldSynthesizeStop` now fires from
  **`NeedsApproval` as well as `Running`** on a terminal/interrupt tail, so an approved (or
  answered) session whose post-turn `Stop` hook was dropped is released to `WaitingForInput`
  instead of stranding in `NeedsApproval` (the original "answer the question, stay needs-approval"
  report); (d) **needs-approval RESUME** — `ShouldSynthesizeResumed` releases a `NeedsApproval`
  session BACK to **`Running`** when the user answered/approved and the agent kept working (a fresh live
  append this pass on a primed cursor, no pending interactive tool, no interrupt, non-terminal tail) —
  previously its only pull-side exit was → `WaitingForInput` at end-of-turn, so an answered-but-still-
  working session showed orange "needs you" for the rest of the turn; mutually exclusive with
  `ShouldSynthesizeStop` (the interrupt + terminal guards), synthesized as tool ACTIVITY (PostToolUse),
  NOT a `UserPromptSubmit` (which would wrongly `++queuedPrompts`); and (e) **presence-idle release** —
  a turn can end with NEITHER a `Stop` hook NOR a terminal/interrupt tail (its last transcript line is a
  bare prompt that produced no assistant output **and** cleared the tracked `stop_reason`, `_readDelta`),
  so (a)–(c) can't fire and a done session strands **`Running`/`NeedsApproval`** forever though claude is
  idle. `ShouldSynthesizeStopFromPresenceIdle` releases it (→ `WaitingForInput` as a `quiescentStop`,
  logged `[recon-stop-idle]`) when claude's OWN pid-validated presence heartbeat reads **`idle`**
  (`PresenceIsAtRest` — `idle`-only; `waiting`/`shell`/empty excluded) on a non-terminal, non-interrupted
  tail with NO pending interactive tool (so it never pre-empts (b)). The quiescence floor is **per-state**
  (`ShouldSynthesizeStopFromPresenceIdle`): a `NeedsApproval` session uses `kScanPresenceIdleQuiescenceMs`
  (5s, > the 2s stop-quiescence so it outlasts the ~2s S-lane presence-refresh lag), but a **`Running`**
  cleared-tail turn requires the far longer `kScanPresenceIdleRunningQuiescenceMs` (30s) — and a `Running`
  session with a still-**pending** (non-empty, non-terminal) `stop_reason` tail is **never** released here
  at all (the idle↔running flap fix: an API-retry pause mid-turn must not be mistaken for turn-end; a
  just-STARTED turn's heartbeat already reads `busy`). Claude's heartbeat is consumed as a state INPUT here
  (Rule #13: a pid-validated FACT) — the IDLE counterpart to the same `busy` reading that elsewhere only
  ever HELD Running; proved live (session `d271a31f`). `ParseTranscriptDelta` now also emits
  a `ToolResult` marker (a tool completed → it answers the pending question) that does NOT count as a
  run-repair turn event. **API-error turn → `Error` — the out-of-band failure capture (`recon-error`).**
  A turn can DIE with an API failure: Claude Code writes a SYNTHETIC assistant line (`model:"<synthetic>"`,
  top-level **`isApiErrorMessage:true`** + an `apiErrorStatus` HTTP code) for a rate/usage limit,
  `"Prompt is too long"`, a 4xx/5xx, a dropped/overloaded connection, a model-not-found, an auth failure,
  or a context-window overflow. The **flag is the signal, never the text** — `"API Error:"` is NOT always
  prefixed (`"Prompt is too long"` / `"You've hit your limit…"` / model+auth errors lack it), so ALL seven
  `error` categories are caught by one predicate. The error line carries a TERMINAL `stop_reason`
  (`"stop_sequence"`), so without this the missed-Stop backstop (recon-stop) would read it as a clean
  turn-complete → `WaitingForInput`, HIDING the failure; `ShouldSynthesizeError` fires INSTEAD and is checked
  BEFORE recon-stop so it wins. Synthesized through the ONE state machine (`Notification{apiError=true}` →
  `SessionState::Error`, `HookEvents.h`; no wire hook ever sets `apiError`, so the PULL scanner is the SOLE
  Error source — Claude's hooks carry no error event), with the **message + HTTP status preserved** on
  `SessionInfo.errorMessage/errorStatus` for the crimson `✕` Error card, and `Scheduler` **`stopOnError`**
  pauses the Autorunner so no queued prompt is auto-sent into a broken session. Logged `[recon-error]`.
  **Active-leaf-aware (the fix for an API error that fired NO Error state — reported live on `3751c455`
  "API Error: Overloaded … not detected in error state").** The error is "the tail" only while it is the
  ACTIVE LEAF — and Claude (≥ v2.1.x) appends post-error BOOKKEEPING (a `system/turn_duration` CHILD
  parented to the error, then an `away_summary`) and rewrites the `{type:last-prompt,leafUuid}` marker to
  name that turn_duration child, advancing the active leaf FORWARD onto the error's OWN descendant chain.
  The scanner tracks that chain as a **descendant frontier** (`ScanState.errorBranchUuids` = the error uuid
  ∪ every later bookkeeping line whose parent is already on it, fed by a new `ParseTranscriptDelta`
  `Kind::Node` lineage event; `ApiErrorIsActiveLeaf`) so the forward advance reads as the error STILL being
  the tail — NOT a double-ESC rewind. The prior exact-`errorEpochLeaf`-equality check MISSED this (it only
  entered Error when no post-error marker happened to be written), suppressing the state on every
  newer-Claude error that wrote one. The session **leaves Error on the first turn event** — a real
  `UserPromptSubmit` (push) → `Running`, or `ShouldSynthesizeRunning` (pull) — and a GENUINE double-ESC
  rewind to a leaf OFF the frontier releases to `WaitingForInput` (`ShouldReleaseErrorOnLeafMove`, logged
  `[recon-error-release]`). Validated by replaying the NEW pipeline over all on-disk transcripts:
  **503/503** API errors flag Error at their live-tail instant (was 498 — the leaf bug silently suppressed
  5 across 3 projects), **0 false positives** (any turn event clears `lastWasApiError` + the frontier), **0
  genuine rewinds** in 2906 transcripts. **NOT flagged Error (by design / structural):** a *tool* failure
  (`tool_result.is_error`) stays Running (Claude usually continues); a process crash → `Done`
  (clean-vs-crash is not yet distinguished); a managed **Codex** has NO Error — its rollout records no
  error event (the 3-state floor, OBSERVER §11f / `Activity.h`). **Subagent / fork activity — the out-of-band `Running` mirror
  (`recon-subagent`).** A turn that delegates to a Task/Agent **subagent** leaves the tailed parent
  `<id>.jsonl` **quiescent** while the work streams to a SIDE file
  (`projects/<proj>/<id>/subagents/agent-<agentId>.jsonl` — shares the parent's `sessionId`,
  `isSidechain:true`, linked to the parent's `Agent` tool_use by `<agentId>.meta.json` `toolUseId`;
  empirically the parent does NOT grow for the whole subagent run — Claude Code v2.1.x); a live
  `/fork`|`/clear`|`/compact`|`/resume` likewise moves the work to a NEW conversation id the pid-keyed
  presence heartbeat already tracks — so the tab wrongly read **Idle/`WaitingForInput`**. Two
  out-of-band signals (read-only, never screen-scraped) recover "still working":
  `ProcessInspect::SubagentActivityUnixMs` (newest write across `<id>/subagents/*.jsonl` +
  `<id>/tool-results/*` — FILES enumerated, since Windows doesn't bump a dir mtime on a child append)
  and `PresenceIsBusy(SessionInfo.presenceStatus)` (claude's own pid-validated **`busy`** heartbeat —
  the BUSY half whose IDLE counterpart is (e) above). Both fold into the scanner's quiescence clock — a
  fresh subagent write counts as a recent transcript write, and `busy` forces `quietForMs`→0 — so
  (a)/(b)'s missed-Stop / blocked-on-user synths can't demote a working session, and `busy` also blocks
  the Waiting→Idle decay (`_maybeDecayWaiting`). `ShouldSynthesizeRunningFromExternalWork` then promotes
  an Idle/`WaitingForInput` session → **`Running`** (synthesized as `PostToolUse`, so no
  `++queuedPrompts`; logged `[recon-subagent]`) — the `recon-run` mirror for work OUTSIDE the parent
  transcript. BOTH plain arms are gated on a **non-terminal** tail; the **subagent** arm additionally requires a side-file
  write within `kScanSubagentFreshMs` (15s), while the `busy` arm is an instantaneous heartbeat read (no
  freshness window): a subagent's final write lands µs BEFORE the parent's `end_turn` and
  `busy` lingers a tick after a real `Stop`, so a TERMINAL tail (the turn truly ended) must never bounce
  a settled session back to Running on that residue. **Work that OUTLIVES the turn — a TEAMMATE /
  background agent / live shell — is the deliberate exception (`externalOutlivesTurn`,
  `kScanExternalWorkGraceMs` 20s): "a shell or agent or teammate still running means Running, not
  idle/done/waiting-for-you".** Claude Code teams run teammates IN-PROCESS writing the lead's
  `subagents/agent-a<name>-*.jsonl` for minutes–hours AFTER the lead's `end_turn` (empirically: a lead
  settles "All three teammates are running" while the side files grow another 30 min), a
  `run_in_background` Agent likewise outlives its spawning turn, and a live shell job keeps claude's own
  heartbeat on the (new-in-2.1.x) **`shell`** status (measured: sessions carrying hours-old cmd/bash
  children report `shell` on a quiet transcript — `PresenceIsWorking` = `busy`∪`shell`, now the scanner's
  activity signal for the quiet-fold + promotion + decay-block, while `PresenceIsBusy`/`PresenceIsAtRest`
  stay the narrow turn predicates). External activity that postdates the parent transcript's last write
  by MORE than the 20s grace (> the 15s post-Esc dying-subagent flush window — `static_assert`ed — and ≫
  the ~2s busy-linger tick) is PROOF of ongoing work, not residue: the promotion fires even on a
  terminal/interrupted tail, and `ShouldSynthesizeStop` is **HELD** (Running-scoped `externalWorkOngoing`
  param) on the SAME expression so promotion/demotion stay mutually exclusive (never oscillate; the
  NeedsApproval release still fires — answered → Waiting → re-promoted next pass). A real hook `Stop`
  (teammates end the lead's turn normally) still lands Waiting for ≤1 scanner tick before the promotion
  re-lights Running — the flash ring self-clears on the move back to Running (`_EvaluateAgentFlash`), so
  no stuck flash. When the background work finally quiets (15s side-file staleness + heartbeat off
  `busy`/`shell`), the hold drops and the normal recon-stop settles the session → Waiting.
  `TranscriptTimesIn` also folds the newest subagent mtime into
  `convLastActivityUnixMs`, so the per-tab overlay's `-lastActivityAgo` reflects subagent writes instead
  of reading stale. The pure gates (`PresenceIsBusy` / `ShouldSynthesizeRunningFromExternalWork`) are
  unit-tested (m5_tests). No UI code changed — this feeds the existing state→overlay→tab-dot pipe a
  corrected `Running`.
- **Adopt any `claude` — observe + control of sessions we did NOT Launch.** A `claude` you
  type yourself into any tab (the WT `+` button → `cd` → `claude`) is managed too, not just
  Manager-Launched ones. At engine init we export `CCMGR_HOOK_PIPE` into the app's process env
  and prepend a transparent **`claude` PATH shim** (`<profile>\shim\` — the ACTIVE profile dir,
  default `~/.agentmaster/shim/`; `claude.cmd` for
  cmd/PowerShell + a POSIX `claude`) that injects `--settings <ours>` + `--dangerously-skip-permissions` (unless the caller already passed
  `--settings`, in which case it runs untouched) then execs the real claude
  (`ResolveRealClaude`, resolved BEFORE the PATH prepend so it never finds the shim); every new
  tab is *meant* to inherit this env so a bare `claude` self-wires for hooks (but WT's env
  regeneration breaks that for `+` tabs — see the caveat below). (Launch/Restore does **not** go
  through the shim: it spawns the **native `claude.exe` by full path** — `Engine::claudeExePath`,
  resolved once at engine init by `ResolveClaudeExe` (Settings override → PATH `claude.exe` →
  `~/.local/bin` → a `claude.cmd`'s npm binary), BEFORE the PATH prepend so it's the real claude not
  the shim. **Native-exe-only policy** (see Gotchas): if no `claude.exe` resolves, `ClaudeAvailable()`
  is false and the Manager **gates** every claude interaction (launch / new / fork / resume) behind a
  "Claude not detected" modal — Browse… + `claude install`; a pure-Node `claude` is unsupported.) The forwarder takes the session id from the hook **payload** and emits
  the hosting **`WT_SESSION`** as the `tabToken` wire field, falling back to a **`bridge.json`**
  discovery file when it didn't inherit the pipe env. The registry **adopts** an unknown session
  on `SessionStart` (flagged `SessionInfo::external`) and fires every window's adoption handler (`AddAdoptionHandler`, fanned out — whichever window hosts the `+` tab binds it);
  `TerminalPage::_AdoptExternalSession` matches the `tabToken` to a live ConPTY
  (`ITerminalConnection::SessionId`) and **binds a stdin injector** — promoting it to full
  observe+control (Tests Autorunner can drive it). A claude hosted outside this app (no matching
  connection) stays observe-only (Rule #9). **Caveat — this hook fast-path is *degraded* for
  hand-typed `+`-tab claudes:** WT regenerates a `+`-tab's child env from the registry, dropping
  these runtime-only vars (`AM_SESSION` / `CCMGR_HOOK_PIPE` / the PATH shim — see Gotchas), so a bare
  `claude` there fires **zero** hooks (a Manager-Launched session is unaffected — it sets env
  explicitly via `spec.env`). The **Fleet Observer** (below) is the reliable detection + bind path
  that needs none of this env — it keys on the **roster correlation**, not the shim/pipe.
- **Fleet Observer — PULL correlation + activity (O1–O7, `OBSERVER.md`; `AgentMaster/ProcessInspect.{h,cpp}`,
  `ProcessObserver.{h,cpp}`, `Activity.h`).** The always-correct floor BENEATH the lossy hook push — the
  one path no shell function / alias / PATH quirk can shadow. Empirically grounded (we measured it live):
  **`WT_SESSION` is the correlation key** (== `ITerminalConnection::SessionId()`, stable across a claude
  close→`cd`→reopen), enumeration (~10 ms Toolhelp) is the only cost while PEB reads are µs, and Claude
  closes its `.jsonl` after each write so correlation is by **cwd → newest transcript**, not an open handle.
  - **Primitives (`ProcessInspect`).** ONE `SnapshotProcesses()` per survey (reused for census + every tab
    tree) + µs PEB reads via the x64 `RTL_USER_PROCESS_PARAMETERS` offsets WT itself reads (centralized +
    commented, WOW64-guarded): `ReadProcessCwd` (the claude PROCESS cwd — tracks `cd` across a relaunch;
    PowerShell never syncs ITS process cwd, but spawns claude with the right one), `ReadProcessCommandLine`
    (exposes `--resume`/`--session-id`/`--model`/`--effort`/`--permission-mode`), `ReadProcessEnv`
    (`WT_SESSION` + `AM_SESSION` + `CLAUDE_*`); `ProcessStartUnixMs`/`ProcessAlive`;
    `ReadProcessImageSubsystem` → `IsClaudeDesktopGuiApp` (the **Claude DESKTOP Electron app** ships a
    binary *also* named `Claude.exe` — a GUI subsystem, not the console CLI — so the census **excludes**
    it by PE subsystem; an undeterminable read is treated as the CLI so a real session is never hidden);
    pure tree helpers (`FindDescendantByImage`/`ChildrenOf`, replacing the old `FindClaudeDescendantPid`) +
    `CommandChildrenOf`/`ResolveShellCwd` (a `pwsh` **freezes its OWN process cwd**, so a shell tab's live
    cwd is read from its NEWEST command child's PEB — the frozen shell cwd is only the fallback);
    `ReadClaudeFacts`
    (cmdline+env parse); `ClassifyRunningApp`; transcript resolution (`EncodeCwdToProjectDir` — every
    non-`[A-Za-z0-9]` → `-`; `ResolveSessionId` — see Rule #14); and **transcript content** —
    `TranscriptTimes` (cheap stat: ctime = conversation age + mtime — the latter used now only as the
    gate/fallback, NOT as "last activity": `claude --resume` + /model/permission-mode/shell-cwd changes
    APPEND untimestamped `last-prompt`/`mode`/`permission-mode`/`summary` trailer lines that bump mtime
    without being activity, so a restored tab focused after restart read "active just now"; measured 7–32 h
    gaps) + `LastActivityMsFromTranscriptChunk`/`ReadTranscriptLastActivityTail` (the LINE-DERIVED
    last-activity — the newest `timestamp` among non-meta/compact/sidechain user/assistant lines + folded
    subagent side-file activity; the cheap, mtime-gateable form of `TranscriptInfo.lastTs`, matching
    `TranscriptStore::QuickRowFacts`; the observer feeds `convLastActivityUnixMs` from THIS) + `ReadTranscriptInfo`
    (head or full read → the first-prompt **title**, `gitBranch`, and the human prompts, parsed like
    `ParseTranscriptDelta`; recent transcripts carry NO `summary` — verified 0/1842 over 90 days — so the
    first prompt is the title). A denied/elevated/WOW64 PEB read ⇒ observe-only, never mis-bound.
    (Promoted out of `ClaudeSpawn`'s anon namespace; `ClaudeCwdForShell` now delegates here.)
  - **`AM_SESSION` ownership stamp (O2, `Engine`).** A per-process GUID exported into the app env at
    engine init (alongside `CCMGR_HOOK_PIPE`); a Manager-Launched claude additionally carries
    `AM_SESSION=<guid>:<windowId>` (via `spec.env`, the launching window appended). `ClassifyRunningApp`
    matches on the GUID **prefix**: ours → `Agentmaster`, a bare `WT_SESSION` (no `AM_SESSION`) → the real
    `WindowsTerminal`, neither → `Other`. **This is the *census* signal, REFINED by the roster:** a
    hand-typed `+`-tab claude does NOT reliably inherit `AM_SESSION` (WT regenerates a tab's env — see
    Gotchas), so a claude correlated to one of OUR roster tabs is `Agentmaster` *regardless* (Rule #13);
    `AM_SESSION` only classifies the NON-rostered remainder (a background daemon we spawned, telling our
    instance from another's, or a real `WindowsTerminal` → external, observe-only). Per-process GUID ⇒ two
    Agentmaster instances stay cleanly separable.
  - **Registry feed (O3, `SessionRegistry::ObserveClaude`).** A provenance-aware upsert: first sight of a
    claude we did NOT Launch creates an `external` record + fires adoption; thereafter it idempotently
    ENRICHES the transient `SessionInfo` facts (pid / liveCwd / model / effort / permissionMode /
    background / runningApp / amSession / ownerWindowId / branch / presenceStatus, plus the transcript timing
    `convCreated/convLastActivityUnixMs` that drives the per-session timing adornment — all runtime-only,
    NOT persisted) but **NEVER** sets `SessionState` (push hooks + the transcript tail own state, Rule
    #1/#7). The timing is refreshed silently (like `lastObservedUnixMs`) — mtime ticks constantly, so it
    must not drive the change cascade; the UI recomputes the "ago" live on any rebuild. A steady-state re-observe
    with unchanged facts is a no-op (no observer / persist / UI churn). `hookWired`/`lastHookUnixMs` are set
    in `OnHookEvent` for provenance.
  - **S-lane (`ProcessObserver`).** A process-wide worker (next to the scanner, thread/condvar shape
    mirrored) on a ~2 s heartbeat + on-roster-change `Wake()`: ONE snapshot → `ReadClaudeFacts` + classify
    EVERY claude (the census, logged `[observer] census claudes=N ours=A wt=W other=O orphan=P codex=C rostered=R`
    + an `[observer]   ours pid=…` detail line per OUR claude) → merge
    every window's published tab roster → per roster tab, `FindDescendantByImage` the shell's claude,
    resolve its id, and feed `ObserveClaude`. **A claude correlated to a tab in OUR roster is OURS
    regardless of `AM_SESSION`** (Rule #13). Publishes two copy-under-lock tables (Correlation + Activity).
    **Census log gating (signal over noise — `ProcessObserver.cpp`).** The census block re-logs ONLY when
    OUR fleet changes (the ours-claude set or any of their identifying facts — rostered/bg/model/effort/cwd/
    wt/win — exactly what the detail line prints) **or** the `kObserverCensusKeepaliveMs` keepalive (5 min)
    elapses. EXTERNAL-world churn (unrelated claudes/codex starting + dying — `wt`/`other`/`orphan`/`codex`/
    the total) does **NOT** trigger a re-log — the summary line still PRINTS the live counts as context,
    they're just not what TRIGGERS it. (Was: ANY count change + a 15 s keepalive re-emitted the whole
    summary+detail block, ~34% of a 65 MB `hooks.log`; the `[observer] skipping GUI/orphaned/PEB-denied`
    lines are separately one-shot-per-pid gated, and `[activity]` transitions are change-gated.)
  - **UI lane (`TerminalPage::_ObserverProbe`, replaces `_DiscoverClaudeTabsByCwd`).** The one WinRT
    thread: each scanner tick it builds THIS window's roster `{WT_SESSION = SessionId(), shell PID =
    GetProcessId(ConptyConnection::RootProcessHandle()), bound}`, `PublishRoster`s it (both reads are µs),
    then — after a short settle so the publish-triggered survey lands — reads the Correlation table and
    binds each of our unbound, id-resolved tabs via the **unchanged** `_BindClaudeSessionToTab` (injector +
    overlay + title + per-dir color). Detached via `_observer->UnpublishWindow(_windowId)` in
    `~TerminalPage` (Rule #10).
  - **Activity + adornments (O6).** Full `TabActivity` taxonomy (Powershell / Cmd / ClaudeCode / Codex
    (now **observe-only enrichment** — Codex C1, below; formerly bare image+pid acknowledge) / Other),
    enriched `[activity]` events, `model · effort · kind`
    adornments on the Manager cards + per-tab overlay, and an **External (N)** group. The external
    census now includes **cmd-/console-hosted** claudes too (the `Other` bucket — previously counted but
    hidden), not only real-WindowsTerminal, and each `ExternalClaudeRow` is **enriched from its
    transcript** (worker-thread title cache so the head-read happens once): resolved `sessionId`, a
    **title** (first prompt), `gitBranch`, a resolved **host label** (`ResolveExternalHostLabel` via
    `ReadProcessPackageFamily` — "Windows Terminal" for real WT vs "Agentmaster" / "Agentmaster Dev" for a
    SIBLING install's session vs a bare shell leaf), and `created/lastActivity`
    timing. Observe-only — surfaced on the board AND the Explorer Tree's **EXTERNAL** scope, where a row's
    **Open New Session Here** / **Adopt** lives on the right-click menu and a **left-click → a read-only Auto Testing**
    of the conversation. **Codex rides this same External group** (`ExternalClaudeRow.kind == Codex`): a
    parallel `codex.exe` census enriches each from its date-sharded rollout (model · effort · sandbox ·
    approval · title · timing + the C2 rollout-tail state) — a teal `codex` pill, an `○ codex · <model>`
    per-tab badge, a read-only Auto Testing from the rollout — and the kind-aware menu now offers **Adopt**
    (resume its rollout into a managed tab) / **Open New Codex Session Here** (a fresh managed codex in the
    cwd), routing to the Codex launch handler (the managed-lifecycle work, above). The OBSERVER still
    **never** feeds a codex to `ObserveClaude` (the External census stays observe-only); a MANAGED codex is
    registered by the launch path + reconciled by the UI lane (`_ReconcileManagedCodex`) and deduped out of
    this census (`managedCodexTokens`). See the Codex status block above + OBSERVER.md §11f / §19-Q3.
  - **Hardening (O7).** Steady-state is µs: the survey skips the Toolhelp snapshot when the roster is
    byte-identical to last tick AND every correlated `(pid, start-time)` pair is still alive (the start time
    is paired — per the rule that PIDs reuse — so a recycled PID can't masquerade as alive), except on the
    slow heartbeat. Retires the now-dead `SessionScanner` transcript-DISCOVERY sweep (`ArmDiscovery` /
    `RecentTranscripts`, which the observer subsumes; the scanner keeps its state-reconcile tail). A
    denied / elevated / WOW64 PEB read is guarded → observe-only, never a misread. 24-h soak: no leak / wedge.
- **C1 UI (M6, `AgentManagerContent`).** Triage Board + Explorer Tree + Auto Testing,
  imperative and snapshot-driven from the registry (cross-thread refresh via
  `DispatcherQueue`), bidirectional selection + directory scope. Each **Triage-Board column**
  is a fixed-width box at full board height with a **pinned header over a vertically-scrolling
  card list** (`_MakeBoardColumn`), so a tall column (e.g. a large **External** census) scrolls
  within the board instead of clipping past the bottom edge (the board's own ScrollViewer scrolls
  only horizontally). The Board header's **"Show all"** (clears the directory scope) is shown only
  when a directory IS scoped — it auto-hides (`_showAllBtn`, kept in sync by `_RebuildBoard`) while
  already showing all directories. Next to the LOCAL/GLOBAL twin a **"Clear"** button (`_clearSelBtn`,
  the same hide-when-idle idiom as "Show all") **deselects** the current card/row — managed OR external
  (`_ClearSelection`) — so the Auto Testing reads nothing-selected. A managed card's **state-colored
  border shows only on hover or when selected** (thickness 0/1/2 at rest/hover/selected, the accent
  pushed onto the Button's PointerOver state) — borderless at rest to cut visual noise on a busy board.
  A managed card's **title sits in a colored band** across the card top, painted the session's
  **working-directory color** — the SAME permanent per-dir color its terminal TABS wear (Rule #12 /
  `dir-colors.json`; `GetDirColor` persisted-first, else the deterministic `AutoDirColorHex`) — with
  **rounded top corners** (matching the card) over a **straight, square bottom edge**, covering ONLY
  the title; the body below (codex pill · working dir · `model·effort` · timing · autorunner badge)
  stays the neutral gray fill. The title text **flips black/white for contrast** (`PreferDarkTextOn`
  — the WCAG relative-luminance crossover ~0.179: near-black ink on a LIGHT band, white on a DARK
  one), so a card reads its folder at a glance and same-dir cards cluster across the state columns
  (`_MakeCard`: card Padding→0 so the band reaches the rounded corners + side edges, an explicit
  `CornerRadius{4,4,4,4}` so the band's `{4,4,0,0}` top corners line up, a `#RRGGBB`→Color `HexToColor`
  parser; External-census cards (`_MakeExternalCard`) stay plain — they are not our tabs).
  **Selecting any card/row — by click, by switching to its terminal tab (`SelectSession` →
  `_SelectSession`), or an external (`_SelectExternal`) — drops that session's cwd into the "Launch
  Claude" box** (unfocused then, so no path-picker pop), pre-aiming Launch / Open-New-Session. A
  title-only `_Refresh` (a rename, or claude floating its OSC title) **preserves keyboard focus** on the
  clicked card/row: each card/row is tagged `b:<id>`/`t:<id>` + mapped (`_boardCardsById` /
  `_treeRowsById`), and `_Refresh` re-focuses the rebuilt element (sync try + `Loaded` fallback) so a
  recreated element never drops focus. All Manager / Archive / Sessions hover tooltips (`AgentSetTip`)
  now open at **~1/3 the system hover delay** — a manual `DispatcherTimer` open (this SDK exposes no
  `ToolTipService.InitialShowDelay`), mirroring WT's `MinMaxCloseControl`. A managed **board card**
  mirrors the Explorer-Tree row's
  interactions (one card/row, one action set): single-click selects, **double-click Activates**
  (jump to the live tab), and **right-click opens the SAME context menu** as the tree session row —
  also surfaced by a **hover-revealed `⋯` more-button** in the card's top-right corner (a
  discoverable twin for users who never right-click)
  (`_MakeSessionMenu` — **Jump to Tab** / Rename (F2) / **Close** (always archives, keeps it resumable in
  Sessions; FAVORITES.md) / Open New
  Session Here / a **Copy** submenu [Session Id · Path · Branch · Launch CLI · Transcript · Summary, via
  the shared `CopySessionField`]; a board-invoked Rename first
  makes the tree row renderable — un-collapses its dir, widens a LOCAL scope to GLOBAL for a
  session hosted elsewhere — since the in-place editor lives in the tree). **Activate is
  cross-window**: the board and the tree's GLOBAL scope show the WHOLE fleet, but a session's tab
  lives in exactly one window, so `_ActivateClaudeSession` selects locally when this window hosts
  the tab and otherwise fans out through the engine's per-window **activate sinks**
  (`Engine::RegisterWindowActivateHandler` / `ActivateSessionInOtherWindows`; registered at engine
  init, token-detached in `~TerminalPage`, Rule #10) — the (single) hosting window hops to its UI
  thread, re-checks its `_claudeTabs`, selects the tab, and **foregrounds itself**
  (`_FocusClaudeSessionTab`: restore-if-minimized + `SetForegroundWindow` + the
  `SwitchToThisWindow` fallback — same-process, so the hand-off is permitted). Board/tree
  double-click, tree `Enter`, the Auto-Testing eye, and the Sessions page's Jump all ride this one
  seam. **The reverse holds too — switching to a session's tab selects it in the Manager**: the one
  post-startup tab-switch funnel (`_OnTabSelectionChanged`) calls `_SyncManagerSelectionToTab`, which
  resolves the newly-focused tab's managed session (`_ClaudeSessionForTab` — Claude OR Codex, both live
  in `_claudeTabs`) and drives the content's public **`SelectSession`** (the SAME `_SelectSession` path a
  board-card single-click takes), so moving to the Manager tab shows the session you were just in
  highlighted (board card + tree row + its Auto Testing) — the per-tab → Manager half of the
  Linked-Lenses selection sync (the board/tree → tab half being Activate, above). User click, `Ctrl+Tab`,
  and a `switchToTab` action all route through the funnel. Gated on `_startupState == Initialized` (so a
  reopen's focused-tab restore can't clobber the lens selection seeded from the `WindowRecord` — the same
  gate `_ScheduleWindowRecordSave` uses) and a no-op for the Manager tab itself (returning to it must
  SHOW the last selection, not change it) and for a non-session tab (pwsh / cmd / external — those leave
  the current Manager selection untouched). The selection is part of the per-window lens
  (`ManagerState`), so it also persists across restart via the WindowRecord autosave. **Rename is
  cross-window too**: the rename writes the shared registry; the window hosting
  the tab re-pins its title via the registry observer (`_SyncClaudeTabTitleFromRegistry`, riding
  the tab-dot push — equality-guarded both directions, so the settled case is a no-op; Rule #11).
  The Board/Tree show only
  **OPEN** (`live`) sessions; a **closed** session (Close keeps its `live=false` record — it NEVER deletes;
  [`FAVORITES.md`](doc/agentmaster/FAVORITES.md)) leaves the Board/Tree and lives in the **Sessions browser**,
  the SOLE history view now (the full-window Archive page + its **Archived (N)** button + the per-window
  "Reopen window"/"Saved window" rows were removed). It is resumable there (`_RestoreArchivedSession`,
  `claude --resume`, transcript-gated — the Sessions page's "Resume here" rehydrates its Auto Testing, so it
  subsumed the old Archive "Restore here") and markable with the **★ favorite**. Saved windows still reopen
  whole via the toolbar **"Reopen Windows (N)"** button. Explorer `Enter`=Activate / `Del`=Close (never
  injects — Rule #2). The tree's **scope toggle is 3-way — LOCAL · GLOBAL ·
  EXTERNAL** (this window's sessions · all windows · the Fleet Observer's observe-only externals);
  the **Triage Board header carries a 2-way LOCAL/GLOBAL twin** (`_boardScopeBtn`, same style) over
  the **same ONE state** — `_SetTreeScope` is the single mutator behind both buttons, the board
  reads EXTERNAL as GLOBAL (it has no External mode; a click there flips the shared scope to
  LOCAL), and in LOCAL the board filters its five state columns to this window's sessions (the
  External census column is never scoped — externals belong to no window). The scope is **persisted
  per window in the lens** (`ManagerState.treeScope`, riding the `WindowRecord` autosave — no
  longer in-memory-only; an older record without the field reads LOCAL, the prior default). The
  board's old `[all directories]` placeholder is **gone** (it was display-only); the `[scope: …]`
  label now appears only while a directory IS scoped, next to "Show all". After the tree's scope
  toggle comes a **sort toggle — NEWEST · OLDEST · MOST ACTIVE · A–Z · BY PID** (`_CycleTreeSort` /
  `_UpdateTreeSortButton`) that orders **both the directory groups and the rows within each, in every
  scope** (a dir's rank is an aggregate over its sessions: NEWEST/OLDEST by conversation ctime, MOST
  ACTIVE by recency with a currently-**Running** session pinned to the top, A–Z by name, **BY PID** by
  the **host window/shell pid** — externals: `ExternalClaudeRow::hostPid`, the same key as the
  color-coded pid underline, so same-window rows group together; managed: the claude pid — **then by
  most active** within each pid group; ctime/mtime via the same transcript timing as the adornment,
  `SortKey`/`MakeSortKey`/`SortKeyLess`). Unlike the
  per-window scope (lens-persisted, above), the sort is a **GLOBAL, persisted** setting (`AppSettings::treeSort` →
  `settings.json`, via the same settings sink the cog uses) — it survives restart and seeds every
  window (the changing window re-sorts live; others adopt it on next launch). After the sort comes a
  **↻ refresh button** (`_treeRefreshBtn`) that **reloads the data for the current scope**: it redraws
  immediately (re-pulls the registry snapshot + recomputes the live "ago" timing) and fires
  `_refreshHandler` → the page's `_RefreshObserverData` — which `Wake()`s the Fleet Observer for an
  immediate full survey (re-enrich the registry + recompute the External/Correlation tables, bypassing
  the O7 debounce), re-runs `_ObserverProbe`, then forces one `RefreshNow()` redraw once it lands — so
  LOCAL/GLOBAL re-pull and the EXTERNAL census both refresh on demand instead of waiting for the next
  tick. EXTERNAL/GLOBAL keep their local-first grouping, with the sort applied **within** each group:
  **EXTERNAL** (`_RebuildExternalTree`) lists every claude we do NOT manage — both real-WindowsTerminal
  **and cmd-/console-hosted** (the `Other` census bucket, previously hidden) — grouped by cwd. Each row is
  **enriched out-of-band from the transcript**: a real **title** (the conversation's first prompt — recent
  transcripts carry no `summary`), a resolved **host label** (`ResolveExternalHostLabel` →
  `ReadProcessPackageFamily`: "Windows Terminal" / "Agentmaster" / "Agentmaster Dev" / shell leaf —
  telling real WT from a sibling install apart), `gitBranch`,
  `model · effort · pid`, and the timing adornment. The **pid number carries a color-coded underline**
  keyed by its **host window/shell** (`ExternalClaudeRow::hostPid` = the claude's parent shell pid,
  filled by the observer census; `WindowKeyColor` maps it through a stable palette) — claudes running in
  the **same terminal window/tab share a host shell**, so they get the **same underline color** and are
  easy to identify at a glance, even across cwd groups (note: real WT runs single-process, so this
  groups by host **tab/shell**, the finest reliable unit — it never falsely merges distinct windows).
  **Left-click selects** an external — from the
  Explorer-Tree EXTERNAL row **OR a Triage-Board External card** (the whole card is the click target;
  there is no inline observe pill / Adopt button) — → the Auto Testing shows its conversation
  **read-only** (`_RebuildExternalPlan` — the human prompts, read from the transcript on a
  **background thread** and cached; observe-only — we host no ConPTY, so it is never drivable, no
  queue/Tests Autorunner). Selecting an external is **Linked-Lenses-synced** (`_SelectExternal`): it switches
  the tree to **EXTERNAL** with the row highlighted, highlights the board card, and renders the
  read-only plan — so a board click behaves exactly like a tree click. **Right-click** (on either the
  tree row or the board card — both use `_MakeExternalTreeMenu`) offers **Adopt** (bring its conversation
  under management via a **Fork-a-copy vs Resume-anyway** choice — `_AdoptExternalClaude(pid,cwd,fork)`
  behind `_ConfirmChoice`: fork = `claude --resume <id> --fork-session` into a NEW transcript (safe while
  the original is still running — no two-writers hazard), else `claude --resume <id>` take-over; id via
  `ResolveSessionId`, original left running — Rule #13),
  **Open New Session Here** (spawn a managed session in that cwd, a new independent conversation),
  and, **as the last item, Bring Window To Front** (surface the external's HOSTING window:
  `_BringExternalToFront` resolves the row's `hostPid`/`sessionId`/title from the latest snapshot
  and hands the OS work to a background thread — `ProcessInspect::BringClaudeWindowToFront` walks
  the claude's ancestor chain to the nearest visible window (conhost children cover a classic
  console; a Win11 default-terminal HANDOFF console falls back to scanning foreign WT-class windows
  for a confident tab match), restores it when minimized, foregrounds it, and — when the host is a
  Windows Terminal-class window (`CASCADIA_HOSTING_WINDOW_CLASS`, real WT or our fork) — **also
  selects the claude's tab** via UI Automation. The pick is the pure, unit-tested `PickClaudeTab`
  over the window's tab names: per-tab score = max of the title tiers (`ScoreClaudeTabName`: exact
  title 100 > "claude" 90 > OSC status glyph 80 > containment 70 > title head 60 > cwd leaf 40 —
  hints are the row title, the transcript's **custom title** (`TranscriptInfo::customTitle`, the
  LAST `custom-title` line; it is also now preferred for the external row's display title), and the
  first prompt) and a **token-overlap tier** for hand-renamed tab labels (`ScoreTabNameTokens` over
  a 2 MB transcript read: every ≥3-char tab-name token present in the conversation corpus as a
  whole word or word-PREFIX — "act" ~ "actions"; tokens hitting the title+**first-prompt** HEAD
  corpus score 50 over 45 for later-prompt-only hits, so a purpose-named tab outranks an incidental
  word match; and a tab whose token set is a **strict subset** of a sibling's ("npyiter pr" beside
  "npyiter perf") is token-disqualified — a generic prefix would "uniquely" match any conversation
  sharing the word). The tab is selected **only on a UNIQUE strict-best score** — a tie or no
  signal keeps the window's current tab (a wrong-tab flip is worse than none; verified against the
  live fleet via `tests/uia_probe.cpp` + `_run-uia-probe.bat`, the ad-hoc dry-run diagnostic). A
  non-WT host (cmd console / ConEmu / VS Code) is just foregrounded. Window activation only — never
  input into the foreign session, upholding the Rule-#13 invariant). **Open New
  Session Here is offered in EVERY scope** — it is also the **last item** on the LOCAL/GLOBAL
  session-row menu (`_MakeSessionMenu`, after Jump to Tab / Rename / Close), spawning in that session's working
  dir. With no external selected the
  Auto Testing reads **nothing-selected**. Every card/row (board, tree LOCAL/GLOBAL/EXTERNAL) carries a dim
  **timing adornment** `-createdAgo/activeFor/-lastActivityAgo` (e.g. `-2m7d/12h/-2h30m` — created ago /
  active span / last-activity ago; `m`=month or minute by position, tooltip-explained;
  `FormatSessionTiming`/`FormatSpan`) from the transcript's ctime/mtime (managed:
  `SessionInfo::convCreated/convLastActivityUnixMs`; external: `ExternalClaudeRow` times, falling back to
  the process start). **A session's title is one value** — the
  Explorer-tree row, the WT **tab** title, and the persisted `SessionInfo.title` are the same
  thing: it's **pinned** onto the tab at launch/restore (`Tab::SetTabText`, so it stops floating
  with claude's OSC title) and renaming from **either** side syncs the other + persists (Explorer
  right-click **Rename…** → `_RenameClaudeSession`; a WT tab rename → `_SyncClaudeTitleFromTab`;
  Rule #11). A launched session's default name is **smart-derived from its cwd**
  (`DeriveSessionTitle`: walk up past generic `bin/obj/Debug/...` segments to the first meaningful
  folder, then apply the cog's **Tab title naming** technique — **Last word in folder name** default
  (`Potato.Tomato.SlangGang` → `SlangGang`) · Folder name as is · Two folder names (`repos/Foo`) ·
  Capital letters (`PotaTo.Tomato.Slang` → `PTTS`) · the Branch trio (branch alone /
  `branch/folder` / `branch/parent/folder` — `ReadGitBranchForDir`, resolved only for those modes;
  no branch ⇒ component dropped) — + the Title-case / spaces→`_` transforms and an unconditional
  `\`→`/` normalization
  (`TabTitleNaming`/`TitleNamingOptions`; never empty, trimmed only past 255 chars to 252 + `...`),
  and each tab is
  **colored per working directory** — a
  **permanent** color a dir keeps across tabs/windows/restarts (persisted to `dir-colors.json`): the
  dir's persisted color, or a fresh **collision-free auto color** (first in the dir's seeded probe
  order that no other folder holds; on a full palette it resets + reuses, avoiding colors open tabs
  show) that is then persisted. Recoloring one tab persists that pick + recolors every tab in the
  dir — Rule #12). Auto Testing: a **compose row** — three top-left icon buttons
  (**eye** = Focus / jump to the live tab · **!** = Send now, *which now confirms first* ·
  **envelope** = Add to the queue) beside a **multiline textarea** that grows as you type;
  per-message actions moved off a button strip onto a **right-click menu over the messages**
  (`_MakePromptMenu`: **Move up / Move down / Delete** on upcoming rows, **Archive session** on
  any). The compose box **recalls prompt history with Up / Down** (`VirtualKey::Up` walks older,
  Down newer; the in-progress text is saved as `_promptHistoryDraft` and restored at the bottom of
  the walk; the list is built lazily by `_BuildPromptHistory`, applied via `_ApplyPromptHistoryText`
  behind a `_promptHistoryNavigating` latch so a recall write doesn't reset the index), and **focus
  snaps back to the compose box after a queue/send** (deferred PAST the Send-now confirm so it can't steal
  the dialog's focus) so you can keep typing. Only a **plain** Up/Down browses history — a **modified**
  arrow (Shift/Ctrl+arrow) passes through for caret/selection, never hijacked. **Tests Autorunner** is now a **toggle in the AUTO TESTING header** (mirrors the Explorer Tree
  LOCAL/GLOBAL/EXTERNAL toggle) — a colored state dot, gray ○ Off / amber ◐ Semi / green ● Full, that
  **cycles** Off → Semi-auto → Full on click (`_CycleAutorunner` / `_UpdateAutorunnerButton`,
  replacing the old combo); the **Templates** row (save / apply / apply-to-dir) is collapsed
  behind a **paper icon** at the textarea's top-right (kept inline — NOT a Flyout — so its name
  box still takes keypresses; the XAML-Islands text-input trap). It still reflects **all**
  messages a session got, not just queued ones: a chronological **SENT** summary (each row
  tagged **flight** = we queued+injected it vs **typed** = you typed it into the terminal) over
  the **UPCOMING** queue (Pending/Held). `Focus()` focuses the cwd `TextBox`. The Launch cwd box has a
  focus-triggered **path-picker drop-down** (`Primitives::Popup`): up to `recentDirsLimit` recent dirs (default 10; the
  current one excluded) over the subfolders of the current path + a `..` up-nav; click a row
  to navigate, and it re-lists. **Typing filters it live** — the subfolder listing is leaf-prefix-filtered
  (`LeafStartsWith`) and a fuzzy-ranked **RECENT MATCHES** section is shown above it (`_RebuildPathPicker` —
  full-Unicode lowercased, Levenshtein approximate-substring closeness to the typed token). The cwd box is **live-validated** (`_ValidateLaunchBox`) — it repaints a
  2px underline + the Launch button by what you typed: an **existing dir** = neutral + "Launch Claude"; a
  typed **conversation GUID** (`LooksLikeSessionId`) present on disk (`ClaudeConversationExists`) =
  **green** + the button becomes **"Resume session"** with a **Fork** twin (`_ResumeSessionFromDisk` /
  `_ForkSessionFromDisk`); a **not-yet-existing but creatable** absolute path = **amber** + **"Create &
  Launch Claude"** (`_EnsureLaunchDirExists` makes the folder on launch); a missing id / malformed-relative
  path = **red** + disabled. (With the Claude⇄Codex toggle on **Codex** the box is **directory-only** — no
  green id-resume, no Fork — keeping only the amber "Create & Launch Codex".) Working dirs are
  grouped/scoped with a filesystem-aware
  `PathEq`, so case-variant spellings collapse to one Explorer Tree root (Rule #8).
- **Tests Autorunner (M7, `Scheduler`).** Pure `DecideAdvance()` + a worker thread on the registry
  advance seam. Turn-complete → auto-send next Pending (Full) / one-click confirm (SemiAuto)
  / Held by the question-guard (transient) / skip Manual gate. Each `QueuedPrompt` also carries optional
  per-prompt scheduling metadata the data model supports beyond this default path — `gate` (OnTurnComplete /
  AfterDelay `delayMs` / Manual), a custom `guardPattern` (empty ⇒ the default not-a-question guard),
  `dependsOn` (a prompt id that must be `Sent` first), and `attempts`/`maxAttempts` (the current Auto-Testing
  UI queues at the `OnTurnComplete` default). Backstops: pause-on-human-
  input, maxAutoSends, stopOnError, global Pause-all. Idempotent (atomic mark-Sent before
  inject). Advances fire on **two** triggers: the `Stop` seam (turn-complete) AND observed
  changes (`OnObserved`), so a session sitting **Idle** (freshly launched / just `--resume`d —
  it never emits a `Stop`) with a queued plan + autorunner **starts** consuming instead of
  waiting forever. `DecideAdvance` treats `Idle` as *ready* alongside `WaitingForInput`; a
  time-bounded **pickup guard** (the `echoed` flag + `kPickupGuardMs`) holds the next send until
  the just-injected prompt is picked up, so a change-driven advance never drains the queue (one
  prompt per turn). `RequestAdvance` dedups; a failed inject (no injector bound yet, e.g.
  mid-restore) rolls the prompt back to `Pending` rather than stranding a phantom `Sent` — on **every** inject
  path: auto-send, the SemiAuto `Confirm`, and the Manager's Send-now (Rule #4). **Enter-retry (the
  "TUI ate my submit Enter" backstop) — pure `DecideEnterRetry()`:** the ConPTY can deliver an
  injected `prompt + CR` faster than Claude's Ink TUI initializes its input handler, so the submit
  Enter is absorbed as a **newline** instead of sending — the prompt sits typed-but-unsubmitted and
  the turn never starts (no `UserPromptSubmit`, no transcript write, state stuck `Idle`/
  `WaitingForInput`). The scheduler **watches** every just-sent Flight prompt and, if the turn hasn't
  **started**, re-presses a **lone Enter** (never the text again — it is already typed; resending
  would duplicate it): the FIRST re-press fires fast (`kEnterRetryFirstMs`, 3s — rescue the common
  eaten-CR case without a long stall), later presses space out by `kEnterRetryIntervalMs` (6s), up
  to `kEnterRetryMax` (3) presses, then **gives up — the prompt is marked `Failed` and the session's
  Tests Autorunner is PAUSED** (mode→Off): it never landed, so don't strand a phantom `Sent` nor advance
  past a broken step (the user Send-nows / re-arms; rolling back to `Pending` would just re-send and
  be re-eaten — an infinite loop). **"Started" = OR of three signals** so it
  degrades across hook / no-hook sessions: the prompt's `UserPromptSubmit` echo arrived (`echoed`) ·
  the session left the ready set (state advanced past `Idle`/`WaitingForInput`, e.g. `Running` — the
  transcript-tail-driven adopted path) · the transcript advanced past the send
  (`convLastActivityUnixMs`, with `kEnterRetryActivityMarginMs` slop — the no-hook fast-turn
  fallback). Only **live, injector-bound** sessions are driven — the gate is **controllability**
  (`HasInjector`: do we hold this session's stdin?), NOT provenance (`external`: did we launch it?);
  an **adopted** `+`-tab claude is `external=true` yet injector-bound, so it IS driven (gating on
  `external` here was the autorunner-on-adopted bug). The watch is **armed
  from `OnObserved`** — every send path marks the prompt `Sent` through the registry, which notifies
  this observer — so no send path needs to know about it, and it works regardless of autorunner **mode**
  (a manual Send-now must still submit). The worker `wait_for`s a `kEnterRetryPollMs` poll cadence
  while a send awaits pickup; `_sweepPendingPickups()` does the re-press + give-up (all registry I/O
  **outside** the scheduler mutex). A retry **restarts** the prompt's `sentAtUnixMs` so a late press's
  echo still lands inside the registry's 15s echo window (else it'd be mis-recorded as a fresh `Typed`
  prompt) AND the pickup guard stays armed (the queue won't drain past the stuck prompt during
  retries). `enterRetries` is transient (reset to 0 at each fresh send). Logs: `[enter-retry] <id>
  press k/3` · `[enter-retry-giveup] <id>` (`autorunner.log`).
- **Persistence + archive/restore (M8, `Json.h`/`Persistence`).** Sessions + named plan
  templates + the path-picker's recent-dirs MRU (de)serialize to JSON under the **ACTIVE
  PROFILE** dir (`AgentmasterStateDir()` — default `%USERPROFILE%\.agentmaster\`, dev package
  `…\.agentmaster-dev\`; PROFILES.md); sessions autosave on change. **Lifecycle = Open ⇄ Archived**
  (the transient `SessionInfo::live` flag, never persisted): Open == has a live tab/claude this
  run (on the Board); closed == shut down but kept resumable (in the Sessions browser — FAVORITES.md).
  On startup `_RestoreClaudeSessions()` loads each saved session into the registry as
  **Archived** and does **NOT** auto-launch it (Rule #6) — the app opens to just the Manager
  tab; the prior fleet comes back from the **Sessions browser** (per-row **Resume here**). Closing a
  session's tab (the X, the tree `Del`, the Manager's **Close**, or the Auto-Testing **Close** item) all
  route through the ONE close seam (`_HandleCloseTabRequested`→`_ArchiveAndCloseClaudeTab`): a 3-way
  confirm (gated by `confirmBeforeKill`) — **Close · ★ Favorite & Close · Cancel** — where both Close
  paths flip `live=false` + clear the injector + persist + close the tab, KEEPING the record so the
  session stays resumable from the Sessions browser, and **★ Favorite & Close** additionally stars it
  (`SetSessionFavorite(id,true)`, dialog `Secondary`) so a keep-this close is one gesture (closing never
  auto-favorites — FAVORITES.md §4/§5). **Close always archives — there is no Delete**
  ([`FAVORITES.md`](doc/agentmaster/FAVORITES.md): always archive, never delete); the conversation
  `.jsonl` on disk is **never** touched. Closing a **batch** that holds managed sessions (a window close,
  or the tab menu's **Close ›**) raises ONE consolidated dialog instead of a
  train of per-tab confirms — **Close All · ★ Favorite & Close All · Cancel All** (either Close path keeps
  each session resumable in the Sessions browser with its Auto Testing — nothing on disk is deleted; ★
  Favorite & Close All stars every managed session first via a `favoriteAll` flag in the apply loop;
  Cancel All aborts the whole close; a batch of only plain shell tabs skips the dialog). The
  tab context-menu's **Close ›** submenu also gained **Close tabs to the left** (`_CloseTabsBefore`, the
  left twin of close-to-the-right), and both close-left/right now **skip the pinned Manager tab** (index 0)
  so a bulk close can never kill it. Restore
  re-launches in the working dir + reloads the Auto Testing + autorunner; resume is
  **transcript-gated**: `claude --resume <id>` only when Claude actually has a conversation for
  that id, otherwise a **fresh** session (new id, same dir + queue) — and the stale archived
  record is dropped. A never-prompted session has no transcript and a blind `--resume` would die
  with "No conversation found" (Rule #6). `Sent` prompts are never replayed. **Restore is agent-aware:**
  `_RestoreArchivedSession` branches on `SessionInfo.kind` — a **Codex** record re-launches via
  `_LaunchCodexSession`, transcript-gated on its **rollout** (`codex resume <codexSessionId>` only when
  that uuid still has a rollout on disk, else fresh), and the durable `id` handle is reused either way so
  the record flips live without re-keying (Codex can't pin a session id at launch — the two-id model).
  Templates: save a session's queue, apply it, or broadcast to a whole directory.
  - **Lifecycle coverage — audit of the add-tab / launch / close / archive / resume paths
    end-to-end. #1 & #2 are ✅ FIXED since the audit; #3 (by design), #4 (mitigated), #5 (moot),
    #6 (cosmetic) remain — all low-severity.** The tab-X archive seam above is sound; the *non-tab-X*
    exits were the risk. **(1, HIGH — ✅ FIXED) closing a _window_ now archives its sessions.**
    `_ArchiveWindowSessionsOnTeardown` (`TerminalPage.AgentSessions.cpp:590`, commit `4fa9fb7bc`) mirrors
    `_ArchiveAndCloseClaudeTab`'s bookkeeping for every hosted session — minus the dialog + `tab.Close()`
    (the tabs go with the window): flip `live=false`, clear the injector (releases the ConptyConnection →
    claude.exe exits), drop the per-window maps, persist once. It runs from the deterministic close seam
    (`CloseWindow`, `TerminalPage.cpp:2652`, after the confirm) for immediate phantom-clearing AND idempotently from
    `~TerminalPage` (`TerminalPage.AgentEngine.cpp:94`) as the catch-all for quit / any other teardown. So a window-chrome ✕ (still the
    _primary_ window exit — the non-closable Manager tab means `_RemoveTab`'s `size()==0` path can't fire)
    no longer leaves `live=true` **phantom cards** or **leaked injectors/connections** on the other
    windows' Triage Boards. **(2, MED — ✅ FIXED) no-tab archive no longer fake-archives a still-running
    session.** `_ArchiveClaudeSession`'s no-tab branch (`TerminalPage.AgentSessions.cpp:384`, commit `0336fa420`) now guards on
    `ProcessAlive(info->pid)`: it refuses to flip `live=false` on a claude that is still alive but not
    hosted in THIS window (an *ours* claude the S-lane carded a tick before its bind completed, or one
    hosted by another window) — which `ObserveClaude` would otherwise bounce back to `live=true` the next
    survey (`SessionRegistry.cpp:388`, Rule #7). Only a claude that has actually EXITED archives there.
    **(3, MED — open, by design) adopt-external = two writers on one transcript** — `_AdoptExternalClaude`
    (`TerminalPage.AgentSessions.cpp:859`) `--resume`s the id into a managed tab while the original external keeps running, both
    appending the same `.jsonl` (only a code comment — "the user closes it" — mitigates). **(4, LOW —
    mitigated) `SessionEnd`/`Done` doesn't eagerly archive** — only the liveness sweep
    (`_SweepClaudeLiveness`, `TerminalPage.AgentObserver.cpp:949`) does, when the hosting connection reaches Closed; the window-close case
    it once missed (→ gap 1) is now covered by the teardown archive, leaving only ~one slow heartbeat
    (~2 s) of a finished claude lingering as a live card. **(5, LOW — effectively moot) Launch with a null
    tab** — `Upsert(live=true)`+`SetInjector` still run unconditionally after `_CreateNewTabFromPane`
    (`TerminalPage.AgentSessions.cpp:183`; the upsert/inject at `:209`/`:219`), but the `pane==null` guard (`:160`) makes a null `tab` unreachable
    (`_CreateNewTabFromPane` returns null only for a null pane), so there is no phantom in practice; the
    unconditional upsert is a latent defensive nit. **(6, ✅ FIXED — crash/restore state fidelity, Rule #16)
    persisted `state` was dead weight** — `ToJson(SessionInfo)` wrote it but the reopen path forced every
    session to `Idle`, so a CRASH (which persisted the LIVE state, unlike a clean shutdown's deliberate
    close) dropped the fleet's "which sessions need me" triage: a Running or Waiting-for-you session came
    back Idle/Done (the reported bug). Now the three restore seams (`_RestoreClaudeSessions` load +
    `_LaunchClaudeSession`/`_LaunchCodexSession` re-home) map the persisted state through the pure
    `RestoredSessionState` (`SessionModels.h`): the AT-REST "needs you" states (WaitingForInput /
    NeedsApproval) are PRESERVED (they survive a `--resume` — the transcript tail still says so), while
    in-flight/ended states (Running / Error / Done) normalize to `Idle` (a resumed claude does NOT continue
    the interrupted turn, and the SessionScanner's primed-cursor gate would strand a seeded Running). The
    scanner refines the seed on its first pass, the Waiting-for-you decay treats a reopened session as
    UNREAD (`readUnixMs` resets to 0 → it keeps waiting until actually read, never instant-decaying off an
    ancient `lastActivity`), no flash-storm fires (the ring keys on a live Running→needs-you EDGE; a restore
    is first-sight), and hooks own it live — but the resume's `SessionStart` now **PRESERVES** these two
    at-rest states (`NextSessionState` mirrors `RestoredSessionState`'s exact preserved set, since a
    re-homed/background tab's LAZY `claude.exe` start on first activation IS a `--resume` that reloads
    without continuing the turn), so the preserved seed **survives opening the tab** and only a genuine new
    turn (`UserPromptSubmit → Running`) changes it. (It previously reset to `Idle` on the first visit — the
    lazy-start `SessionStart→Idle` clobber, which ALSO silently undid a manual **Move to Waiting-for-you** /
    **Mark Unread** promote on a not-yet-focused tab: the reported "mark it Waiting-for-you, activate the
    inactive tab, it resets to Idle/Done" bug.) Resume-vs-fresh is STILL transcript-gated, never state-gated (the gotcha stands). The
    **companion question-guard flag** (`lastMessageWasQuestion`) is now persisted the same way and gated by
    `RestoredQuestionFlag`, so a crash while the agent was WaitingForInput on a clarifying question can't
    drop the guard and let the Autorunner auto-ANSWER it on reopen (the flag rides ONLY a preserved
    needs-you state; a stale flag from an interrupted Running turn — already answered — is dropped when the
    state normalizes to Idle, so it can't falsely hold the queue). Unit-tested (`TestRestoredSessionState`
    + the persistence round-trip). **Verified clean (not gaps):**
    external WindowsTerminal/Other claudes never enter the registry or `sessions.json` (`ObserveClaude` is
    gated on rostered + resolved id, `ProcessObserver.cpp:680`) — no foreign-claude leak into the Archived
    list; cross-window double-bind is guarded (`HasInjector`; one ConPTY lives in one window); and
    resume-fresh drops the stale archived record (`TerminalPage.AgentSessions.cpp:216`).
  - **Archive-page + resume/restore + persistence audit (round 2) — 13 findings, all ✅ FIXED + deployed**
    (commit `b5768081e`; a read-only sweep of the Archive page, resume/restore, and the `WindowRecord`
    layer). *Archive page:* the header count tracks the active filter ("K of N"); **Reopen its window**
    re-resolves its target from the record's stable `windowId` at click time (a gather-time index goes stale
    if the record set shifts while the page is open); `_GatherArchiveRows` indexes archived sessions by id
    ONCE (was O(tabs·sessions)) + dedupes a session referenced by two records to a single row; the
    Restore/Reopen/bulk handlers **defer** their tree mutation (the page's pointer-handler crash class);
    bulk-restore + its "(N)" count act on **checked ∩ visible** only (a check hidden by a later search is
    never silently restored) and stale checks are pruned each gather. *Persistence:* **quit-all now flushes**
    each window's record (`RequestQuit` + the `~TerminalPage` catch-all, latched so the catch-all can't
    clobber a good close-seam flush after `_claudeTabs` is cleared) — previously only the 750 ms debounce
    saved it, losing trailing geometry/tab-order/lens on app quit; the process-once fleet load is now a true
    **barrier** (`Engine::restoreMutex`) so a 2nd reopened window BLOCKS until the registry is populated
    before re-homing, instead of skipping its not-yet-loaded sessions and flushing an EMPTY record over its
    workspace; `_CaptureWindowRecord` stops persisting a **dead** per-tab Claude color (the dir-color system
    owns it, Rule #12) and only records a focused shell-tab target that can actually be recreated. *Registry
    / autorunner:* `SessionRegistry::Remove` now `_notify`s (a resume-fresh drop refreshes every window's
    Archive list — no ghost row); `ObserveClaude`'s `live=true` revive is gated on a **different pid** so a
    just-archived session whose claude is briefly still alive isn't bounced back (Rule #7); `Scheduler::Confirm`
    (SemiAuto) + the Manager's Send-now now **roll a failed inject back to `Pending`** like the auto-send path
    (Rule #4). *Primitive:* `ProcessAlive` prefers the unambiguous `WaitForSingleObject` liveness test (avoids
    the `GetExitCodeProcess`==`STILL_ACTIVE`/259 false-alive), falling back to the query path when SYNCHRONIZE
    is denied.
- **Per-window records (M10 data layer, `Persistence`/`SessionModels`).** A `WindowRecord`
  (one file per window: `windows/<windowId>.json`) holds per-window **UI state** — geometry
  (position/size/launch-mode), the Manager **lens** (selection / dir scope / selected prompt /
  collapsed dirs / splitter fractions / the shared tree+board LOCAL·GLOBAL·EXTERNAL scope,
  `treeScope`), and an **ordered list of tab refs** (a Claude OR Codex tab = its `sessionId` + a
  `TabKind` discriminator; a shell tab = an opaque WT `actionsJson`). This is **Option 1** — a thin
  layer OVER the archive model: it records tab order + window↔session affinity + geometry/lens
  WITHOUT duplicating session data (`sessions.json` stays the session truth, so there is one copy
  of every session). Schema + (de)serialize + `Save/Load/Delete/LoadWindowRecord` are done and
  unit-tested; the live **capture** (debounced autosave) and **restore** (re-apply geometry/lens,
  claim/re-claim a record by id) are **shipped + live-verified** (see Status + `PERSISTENCE.md` §13.5).
  **Session re-home + Other-tab recreation are now shipped too** (`_RestoreWindowTabs`): a reopened
  window resumes its Claude **and Codex** sessions (re-launched by `TabKind` — `_LaunchClaudeSession` /
  `_LaunchCodexSession`) and replays its shell tabs (title/color/cwd) from the record's tab
  refs, in order — so closing and reopening a window brings the whole workspace back, not just
  geometry + lens. (Saved windows reopen whole via the toolbar **"Reopen Windows (N)"** button — the
  Archive page's per-window "Reopen window" is gone, FAVORITES.md.) (Tab `actionsJson` capture, once
  deferred, is now live in `_CaptureWindowRecord`.)
  **A window that ends up holding ONLY the pinned Manager tab is never kept around or restored.** When a
  window's last terminal tab is closed / torn out (or it reopens from an empty/legacy record, or its
  sessions all fail to re-home), a **debounced** check (`_CloseWindowIfManagerOnly`, fed by
  `_tabs.VectorChanged` + the end of startup so the SETTLED tab set is evaluated — an async shell re-home
  is never momentarily mistaken for empty) **self-closes it UNLESS it is the last Agentmaster window**;
  the last window stays open (the app needs one) but **discards its record** so the app never reopens a
  content-less Manager-only window (it falls back to a fresh default window). The not-last-vs-last
  decision is race-safe across windows on different threads (`Engine::ReserveManagerOnlyClose` reserves
  the close under `windowMutex`, counting live windows minus already-reserved ones, so two windows
  emptying at once can never both close and quit the app). "Not saved / not restorable" is enforced by
  `_FlushWindowRecord` **deleting** the on-disk record when the window is Manager-only + the discard latch
  is set (the latch's ONLY writer is the post-startup check — never during restore — so an only-shells
  reopen that is briefly tab-empty is safe), reinforcing the existing `UnregisterLiveWindow` empty-record
  deletion + `RecoverableWindows` empty-record filter.
- **Settings cog (`AppSettings`, `settings.json`).** A `⚙` (toolbar order: Launch · Fork · Reopen · `⚙` ·
  Pause Tests Autorunner · **Sessions** — the cog sits *before* Pause Tests Autorunner; **Sessions** is the rightmost
  (the **Archived** button was removed — FAVORITES.md)) opens a
  global-settings surface — an **in-content modal overlay** (a dimmed `Grid` over `_root`),
  NOT a `ContentDialog` (a text box inside one gets no keypresses in XAML Islands — see
  Gotchas). Exposes **Claude-session** config — `skipPermissions` (the spawn's
  `--dangerously-skip-permissions`), `model` (== `/model <v>`), **`launchModels`** (the
  **launch-model picker**: a multi-line `Display name | model-id` list — default `Fable 5 |
  claude-fable-5` / `Opus 4.8 | claude-opus-4-8` / `Sonnet 5 | claude-sonnet-5` — that turns EVERY
  **"Open New Session Here"** AND **"Fork session"/"Fork here"** into a submenu [board/tree session
  menu + External menu + the Sessions page's row menu + its detail-pane SplitButtons (Open-New AND
  Fork) + the WT tab menu's "New Session Here" and "Fork session", the shared `AgentModelMenu.h`
  recipe]: **Default** (the plain behavior — the settings `model`) + one item per
  configured model, each starting that ONE session with `--model <id>` [`ParseLaunchModels` →
  `BuildClaudeCommandline`; a fork IS a launch — the pick rides `_ForkManagedSessionById` /
  `_ForkSessionFromDisk` onto the forked session's commandline (the WT tab's fork submenu raises
  `Tab::ForkSessionRequested`; its plain twin stays the DuplicateTab action) — per-launch only,
  never persisted — resume follows the settings model again; a Codex row keeps the plain items
  (neither a codex spawn nor `codex fork` takes `--model`), a shell tab keeps the plain "Fork
  session" (for it that's WT's duplicate-tab), and the launch bar's Launch/Fork + the double-click
  Resume/Fork dialog stay Default]; every tooltip points here, an ABSENT key seeds the
  defaults while a cleared box stays empty ["just Default"], the tab menu repopulates at
  flyout-open so a cog edit applies live, and the cog editor is **LIVE-LEXED like the env
  editors** — `LexLaunchModelsText` (the `LexEnvText` twin, reusing `EnvLexResult`) recolors the
  wrapping border green/amber/red + a counts/first-issue status line beneath
  (`_RefreshLaunchModelsLex`, per keystroke + on open): Error = a skipped entry (empty name/id
  side), Warn = listed-but-noteworthy (duplicate display name / a spaced model id) or dropped
  past the 32-model cap, and `ok` counts what is actually OFFERED so "N models" == the submenu
  size), `includeCoAuthoredBy`, and a
  global **`env`** (a `;`-delimited `NAME=VALUE` list applied to every session via
  `ParseEnvAssignments`→`spec.env`, `CCMGR_*` filtered) — plus a **CLAUDE BINARY** row (the
  native-exe-only policy): the auto-detected `claude.exe` (read-only) + an **`.exe`-only override**
  (`claudeExePath`) with **Browse…**, re-resolved live on Save via `RefreshClaudeExe` — plus **Tests Autorunner defaults** stamped
  onto NEW sessions (mode / maxAutoSends / stopOnError / pauseOnHumanInput) and **behavior**
  (`confirmBeforeKill` — relabeled "Confirm before closing" — routes the Close action
  (tab X / Manager **Close** / tree `Del`) through the confirm dialog;
  `defaultLaunchDir` seeds the cwd box — empty ⇒ `%USERPROFILE%`). It also exposes `tabRenameCommitMode` (the rename box's
  commit key — click-away-or-Shift+Enter vs Enter), `waitingForYouTimeoutMinutes` (the **Waiting-for-you "unread"
  timeout** — a `WaitingForInput` card demotes to `Idle` only once this timeout elapses **AND** the user
  has **read** it [visited its tab since the last turn]; an unread or manually **Mark-Unread**-ed card keeps
  waiting past the timeout — `ShouldDecayWaitingToIdle`; cog control = a **1m–3d slider + a "Never" toggle**,
  default **60 = 1h**. **Renamed from the legacy `waitingDecayMinutes`** so a pre-existing settings.json's
  value [tuned for the old 5-min cache window] is **invalidated** → existing installs fall back to the 1h
  default; the old key is ignored + dropped on next save), `serverCacheMinutes` (Claude's server-side
  prompt-cache lifetime, default **5**, drives ONLY the Triage-Board card's **⚡ "still cached"** hint shown to
  the right of `⚙ sent/total` — keyed on **REAL API-turn evidence of THIS conversation** via the pure
  `ServerCacheStillWarm` (SessionModels.h): the transcript's PARENT-line-derived API activity
  (`convApiActivityUnixMs` — the fold-free sibling of the display value `convLastActivityUnixMs`, which rides
  subagent/TEAMMATE side-file writes and would keep ⚡ lit for a background team's whole minutes–hours run
  while the lead's cache is long cold) + the hook-side `lastTurnUnixMs` stamp (`IsApiTurnEvidence`,
  HookEvents.h — never `SessionStart`/`SessionEnd`/a synthesized quiescent Stop, and since the teammate pass
  never `SubagentStop` nor the scanner's external-work `PostToolUse` synth: a subagent/teammate turn runs in
  its OWN context and never re-warms the lead's prefix cache),
  deliberately NOT the `lastActivityUnixMs` decay anchor, which launch/adopt/resume `SessionStart`s and the
  "Move to Waiting-for-you" triage promote stamp "now" with ZERO API traffic — the old ⚡ false positives
  ("shows right after adopting / after Move to Waiting-for-you / on a never-prompted launch"); Claude-only —
  a managed Codex never shows it), `recentDirsLimit` (the path-picker MRU size, default 10), (the
  **COMMANDS tab** — COMMANDS.md §6a) the **/handover-family customization** —
  **`commandHandoverName`/`Enabled` + `commandHandoverHereName`/`Enabled` +
  `commandHandoverStandbyName`/`Enabled`** (rename each command [the
  typed word == the definition file `<name>.md`; normalized + collision-healed via
  `ResolveCommandNameTriple`, per-command status
  lines staging `/old → /new after restart`] or disable it entirely; **applies at the NEXT START** —
  engine init binds + reconciles the definition files, migrating a renamed/disabled command's old
  file away only when it is pristine-ours; the engine-owned `command*MaterializedName` markers are
  preserved-from-disk on Save like the seed markers) **plus the SUCCESSOR SHAPING half (§6b)** —
  **`commandHandoverSuccessorModel`/`…Here…`/`…Standby…`** (a per-command "Successor model" combo:
  Default + the `launchModels` list, rebuilt each cog open, an unlisted stored id shown `(custom)
  <id>`; a LEADING model word in the typed command — `/handover [fable] …` / `/handover fable 5: …`,
  partial/caseless/characters-only against either side of a launchModels entry — overrides the combo
  for that one handover, `PickModelFromArgsHint`), **`commandHandoverTitleFindRegex`/`…TitleReplace`** (a find/replace pair rewriting successor
  titles off the origin title — `$1` backrefs; unset/invalid/no-match/blank falls back to the classic
  `"(handover)"` naming), **`commandHandoverFileMatchRegex`** (which markdown files a handover
  collects, matched case-insensitively against the file NAME; seeded `HANDOVER\-` == the
  `HANDOVER-<topic>.md` contract, blank/invalid falling back to the looser built-in
  "name contains handover" hint — **the one restart-applied field here**), and
  **`commandHandoverDeleteFileAfterLaunch`** (delete the md once its successor exists + delivery is
  secured; never the pointer tier — the `HANDOVER-*.md` litter fix), **plus the §6c WRITE LOCATION** —
  **`commandHandoverWritePath`** (the "Handover file location" box + a **Presets ▾** menu — Scratchpad
  (default, also what a blank box means) · `./` · `./docs` · `./docs/handovers` · `./handovers`, or any
  typed folder incl. an absolute one; FOLDER-only, the `HANDOVER-<topic>.md` name is untouched; picking
  Scratchpad also ticks delete-after) and the **COMMAND DEFINITION FILES** section (a per-file state line
  — up-to-date / managed / **EDITED BY YOU — left alone** / not installed — plus a confirmed **Reinstall
  definition files…**, the one overwrite that ignores the digest). Model/title/delete/**location** apply
  to the NEXT handover right after Save (the location by re-rendering the live definition files); a live
  status line under the boxes calls out an INVALID regex and spells out where briefings will land
  (validated through the shared `RegexUtil.h`), and (the
  **NOTIFICATIONS tab** — **System notifications**) **`notificationsEnabled`** + the five per-target-state
  switches **`notifyOnWaiting`/`notifyOnNeedsApproval`/`notifyOnIdle`/`notifyOnDone`/`notifyOnError`** +
  **`notifySuppressFocused`** + **`notifySound`** (ALL default ON — a **Windows toast** whenever a managed
  session's status leaves **Running** for anything else: line 1 = the session title, line 2 = `Has completed
  after <2h30m> and is <status>`, the duration being the observed Running span [omitted when the entry edge
  wasn't seen, e.g. adopted mid-turn]. Fired by the ONE window hosting the session's tab on the SAME
  registry-observer push as the tab status dot (`TerminalPage::_EvaluateAgentNotification`, its own edge
  tracker beside `_agentFlashLastState` — deliberately NOT shared with the flash), so exactly one toast per
  transition across N windows; a **spurious-completion HOLD** keeps the toast honest against the
  outlived-turn promotion (the SessionScanner toast gates; proven live on `513d1366` — "waiting for you"
  toasted at the Stop edge, presence=shell re-lit Running 20s later, and a shown toast can't be recalled):
  a Running→**Idle/Waiting** toast is **HELD** while the session's external-work signal is live
  (`PresenceIsWorking` on the heartbeat copy OR side files fresh within `kScanSubagentFreshMs` —
  `AgentExternalWorkSignal`), then **dropped** if the session re-lights Running (`[notify-hold]` →
  `[notify-drop]`, no pop; the push edge's Running re-entry is the primary drop, the liveness-ticked
  `_SweepAgentPendingToasts` → pure `DecideHeldToast` the belt) or **fired with the CURRENT state** when
  the signal clears (a plain completion's post-Stop `busy` linger costs ~one 2.5s sweep tick) / a hard
  needs-you state lands / the 30s `kNotifyExternalHoldCapMs` backstop elapses — cog switches +
  focused-skip re-applied at fire time; **NeedsApproval/Error/Done never hold** (work can't answer a
  question), and a **per-session double-toast guard** (`kNotifyDuplicateToastMs` 20s, `[notify-dedupe]`)
  caps a W→R→W flap at one SHOWN toast per window on both the immediate and deferred paths; per-session **Tag+Group** makes a newer toast REPLACE the older in Action
  Center; **clicking the toast brings the hosting window to the FRONT and jumps to the session's tab**
  (NOTIFICATIONS.md §4a) — via a **per-identity TOAST COM ACTIVATOR** (`AgentToastActivator.h`, header-only,
  registered process-once at engine init; CLSIDs declared in BOTH manifests — release `{7608CBBC-…}` / dev
  `{6CB0FAE1-…}`, distinct because a CLSID is machine-global and the two installs coexist): the shell
  CoCreateInstances our CLSID and reaches the RUNNING process (`INotificationActivationCallback::Activate`
  → `ActivateSessionInOtherWindows(id, "")` → the hosting window's
  `_FocusClaudeSessionTab(id, bringWindowToFront=true)` = restore-if-minimized + `SetForegroundWindow` +
  the `SwitchToThisWindow` fallback), so **no second process launches and the click can't open a stray
  window** — the bug the first cut had, where the shell's fallback AUMID activation launched
  `WindowsTerminal.exe` with no args and the Emperor turned it into a new window with a default tab. The
  toast's `launch` attr carries the session id (== `invokedArgs`); the legacy in-process
  `ToastNotification.Activated` handler is wired ONLY when the activator didn't register
  (`ToastActivator::IsRegistered()` — unpackaged, or a package not re-registered since the manifest gained
  the CLSID), so a stale registration is un-fixed but never regressed. **A manifest change ⇒ the loose
  layout must be re-registered.** Cold start (app gone): the manifests' ExeServer relaunches us
  `-ToastActivated -Embedding`, which `WindowEmperor` strips to a plain no-arg launch (normal workspace
  restore, never the defterm path) and the pending activation then jumps if it beats the SCM timeout.
  Logged `[nav] notify-click` + `[notify] toast activator registered`;
  `notifySuppressFocused` skips a toast for the focused tab of the ACTIVE window [the flash ring's
  "current tab is always visited" rule]; `notifySound` OFF adds `<audio silent>`; the checkboxes grey out
  while the master is OFF but keep their stored values. Best-effort `ToastNotificationManager` — an
  unpackaged build [no AUMID] logs `[notify] toast failed` ONCE and no-ops; fires log `[notify] <id>
  running -> <state> (after <span>)` in hooks.log. Codex rides it too at its 3-state floor), and (TABS
  section) **`tabTitleNaming`/`tabTitleCase`/`tabTitleSpacesToUnderscores`** (the **Tab title naming**
  trio — HOW an untitled session's default tab title derives from its working dir, all applied by the
  ONE `DeriveSessionTitle` (its 2-arg form is pure — options in; the 1-arg form reads these fresh from
  disk at each derive, so a Save hits the very next launch in every window): the technique dropdown
  **Last word in folder name** default (`Potato.Tomato.SlangGang` → `SlangGang`; no separators ⇒ whole
  name; separators = `.`/`-`/`_`/whitespace) · **Folder name as is** · **Two folder names**
  (`C:\repos\Potato.Tomato.SlangGang` → `repos/Potato.Tomato.SlangGang`; at a drive root just the
  folder) · **Folder name capital letters** (`PotaTo.Tomato.Slang` → `PTTS`; a no-capitals name falls
  back to word initials, a single lowercase word as-is) · the **Branch trio** — **Branch name** ·
  **Branch name / folder name** · **Branch name / two folder names** (`feature/ui` ·
  `feature/ui/Agentmaster` · `feature/ui/source/Agentmaster` — the dir's LIVE git branch as an
  `TitleNamingOptions::branch` INPUT so the 2-arg derive stays pure; the 1-arg configured form
  resolves it via `ReadGitBranchForDir` ONLY for these modes (`TitleNamingUsesBranch`), detached HEAD
  reads as the short SHA, and an empty branch (non-git dir) drops the component + its `/` — bare
  Branch then falls back to the folder name), plus a **Title case** dropdown
  (Default/Lowercase/Uppercase) and a **Spaces to underscores** toggle — every mode still walks past
  generic `bin/obj/Debug/…` segments first, normalizes any `\` in the title to `/` (unconditional, no
  setting), and trims only a degenerate >255-char title to 252 + `...` (a safety net — real names
  stay whole, the strip trims visually); a **live example
  preview** under the controls re-derives four made-up paths through the real 2-arg derive on every
  change (`_UpdateTitleNamingPreview` — a Branch* pick previews with a made-up `feature/ui` branch,
  noted in the block's first line). Applies at the NEXT launch/adopt/fork-from-disk of an UNTITLED
  session only — existing/renamed titles never re-derive, Rule #11), and (TABS
  section) `favoriteIcon` (the **Favorite marker** dropdown — **Crown** default / **Star** — the glyph a
  favorited session wears on its live tab strip; FAVORITES.md §5a, applied live on Save + cross-window
  broadcast), and (TABS section) **`maxTags`** (the **Max bookmark tags (global)** box — the ceiling on how
  many DISTINCT bookmark-tag names can exist across all sessions; default **20**, clamped 1–40 by
  `ClampMaxTags`; gates only the creation of a NEW name in the Tags panel, never applying/removing an
  existing one), and (TABS section) **`tabColorMode`** (the **Tab coloring** dropdown — HOW managed tabs get
  their color: **Shared per working directory** default [the classic Rule-#12 per-dir permanence] ·
  **Individual per tab** [each session dealt + KEEPS its own color, persisted on the record —
  `SessionInfo::tabColorHex`; a user pick recolors only that session, a reset re-deals next launch] ·
  **Inferred working directory** [per-dir semantics keyed by the dir the session ACTUALLY works in.
  **The inference — RANKED BY OCCURRENCE** (`InferWorkingDirectory`): every tool-touched path VOTES
  for its whole ancestor-directory chain (leaf excluded) and belongs to exactly ONE disjoint **work
  CLUSTER** — its enclosing git root (the snap, below) else its top-level dir below the drive/share
  root; clusters are **RANKED by vote count** and the STRICT top wins — plurality, no majority bar
  (40/35/25 across three repos picks the 40; a TIED top ⇒ the launch cwd). A git cluster answers its
  repo root AS-IS; a non-git cluster answers by **LOCAL-MAJORITY DESCENT** — a child takes the pick
  from its BASE only while it holds >50% of the base's own paths — the base-vs-path priority by
  occurrence: a 60/40 sibling split stays on the shared base, a stray one-off read (~/.claude files)
  can never drag the pick to the drive root the way a longest-common-prefix would, and the old
  deepest-global-majority rule is subsumed (a global majority is always the top cluster; the descent
  reproduces its deep pick exactly when concentration is real). Drive/UNC roots + relative paths
  never vote/win. **The votes** =
  `TranscriptStats::pathsAccessed` (deduped per-FILE, cap 512): the canonical tool path fields
  (Read/Edit/Write/Grep/Glob `file_path`/`notebook_path`/`path`) **plus absolute paths MINED from
  shell `command` strings** (`ExtractPathsFromText` — a QUOTED path is taken whole, spaces + leaf
  included, "(x86)" parens and all [the shell itself demands the quotes]; an UNQUOTED path tolerates
  **one interior space, only inside a folder name a later separator confirms** — `C:\Program
  Files\App\x.exe` — so prose after a path is never swallowed and a space-carrying LEAF truncates at
  the space with its parent, the thing the vote uses, exact; `f.cs:123` line refs stop at the colon,
  `,;=&`+quotes/wildcards terminate, unbalanced `)`/`]` + sentence dots trim; per-line cap 8 shared
  with the field extraction — these also enrich the Sessions 📁/📄 scopes). **Machine-TEMP paths
  never vote** (`ExcludePathsUnderRoots` over `CollectMachineTempRoots` — the effective user temp
  via `GetTempPathW` long-formed + `<windir>\Temp` — applied in the scan just before the infer, per
  candidate): a Claude session scratches under `%TEMP%` constantly (heredoc scripts, outputs, its
  per-session scratchpad dir), and a proven live case had a session whose ONLY captured path was one
  scratchpad `pr-body.md` — a 1/1 "majority" that inferred the SCRATCHPAD dir and recolored the tab
  off its repo (no git root above temp, so the git snap couldn't catch it); temp is scratch by
  definition, so it's excluded from the vote — possibly emptying the corpus, which lands the honest
  cwd fallback. Inference-time ONLY: the sidecar keeps the full set, the Sessions 📁/📄 scopes still
  match temp paths. **The git snap** (the
  **Use .git folder to infer** toggle right under the dropdown — `AppSettings::inferGitRoot`, default
  **ON**, enabled only while the mode is Inferred, read back even when disabled): each voting path's
  PARENT dir resolves to its **nearest enclosing git root** (`FindGitRootForDir` — walk-up probing for
  a `.git` DIR or worktree/submodule FILE, nearest wins so worktree work keys the WORKTREE, never a
  bare drive/share root; memoized per NormDirKey across the scan batch), and a git-root CLUSTER that
  ranks top answers the repo root — **as-is, never deeper**: the repo is ONE working area, so an
  in-repo session infers the repo root == (normally) its launch cwd, which keeps the Inferred mode's
  colors IN STEP with Shared-per-directory (the "switching modes suddenly recolors my tab" fix —
  colors now differ only when a session genuinely works OUTSIDE its cwd's repo). Repos are clusters
  like any other ⇒ a cross-repo session picks its most-occurrences repo (plurality), and a repo
  outranked by a bigger non-repo cluster loses honestly; OFF ⇒ every path clusters by its top-level
  dir; the resolver is INJECTED (`InferWorkingDirectory`'s 3rd arg) so the picker stays pure/testable.
  **The loop:** re-detected
  mtime-gated + ~15s-throttled off the scanner tick (`_ScanInferredTabColors`, reading the Sessions
  sidecar incrementally), cached persisted on `SessionInfo::inferredWorkingDir` (stored EMPTY when ==
  cwd) so a reopened session wears its color immediately; `[infer-dir]` logs each change; a
  mode/`inferGitRoot` change (cog Save AND the cross-window broadcast) CLEARS `_inferredColorScan` +
  kicks one pass, so every hosted session re-infers under the new rule instead of waiting out its
  mtime gate. **HOME-DIR forcing — a session LAUNCHED in the user's home directory (`%USERPROFILE%`,
  the launch box's empty-`defaultLaunchDir` fallback) INFERS in EVERY tab-color mode, not only this
  one** (`SessionInfersWorkingDir`, Persistence — the ONE predicate gating both the producer
  [`_ScanInferredTabColors` admits per-session: the whole fleet under Inferred, ONLY home-dir
  launches in the other modes — the state map stays empty otherwise] and the consumers
  [`EffectiveWorkingDir`/`SessionColorKeyDir`/the paint seam's inferred override/the tooltip+subline
  "inferred/launched in" flip]): a claude started in the home dir almost never WORKS there (a fresh
  tab + `claude` with no `cd`), so its meaningless cwd is replaced by where its tool calls
  concentrate for grouping/color/every semantic surface in all four modes — while a
  deliberately-chosen cwd (incl. a home SUBfolder — exact-`NormDirKey` match only) keeps the
  classic mode gate, and a dormant inference from a past Inferred-mode run still never leaks;
  **the rich tab TOOLTIP surfaces a non-empty inference** as a dim full-path
  `inferred → <dir>` row under the meta line (it's stored only when ≠ cwd, so "present" already means
  "working somewhere else"). **Fork
  lineage:** a fork INHERITS its source's inference at launch (registry copy in `_LaunchClaudeSession`
  — a fork's own transcript doesn't exist until its first turn, and its content-to-be is a verbatim
  copy of the parent's, so parent color parity from the first frame — the "forked session got a
  different color" fix), and a still-transcript-less fork's scan pass infers from the SOURCE's
  transcript via `forkParentId` (against the SOURCE's sidecar — covers adopt-external forks whose
  source the registry never knew); the moment the fork writes its own transcript the scan switches to
  it. **Other lifecycle seams:** resume of an unknown-on-disk id ⇒ its transcript exists, first scan
  pass (~2s) infers; `/clear` ⇒ honestly no inference (cwd color) until the new conversation touches
  files; restore-FRESH keeps the dead conversation's inference as a CONTINUITY seed until the new
  history overrides it; Codex ⇒ never inferred (rollouts aren't path-parsed — cwd keys its color)] ·
  **Remove colors** [`TabColorMode::NoColor`: NO tab is colored, **STRIP-WIDE** — a managed tab is
  RESET by the paint seam (its color re-derives from the maps on exit), and every OTHER tab's
  runtime color — an **ex-claude pwsh tab** still wearing its exited session's dir paint (archive
  drops it from `_claudeTabs`, the tab lives on — the reported gap), a user-colored/restored shell
  tab, the Manager tab's per-window tint — is **SUSPENDED, not reset** (`Tab::SetTabColorSuspended`:
  visual shed, value PARKED on the tab in `_suspendedTabColor`, raises no `TabColorChanged`;
  restored on leaving the mode by the `_ReapplyManagedTabColors` strip sweep). **Change tab color**
  + the `openTabColorPicker` action are disabled on EVERY tab (`Tab::SetColorPickerEnabled` —
  seeded at tab registration, swept on mode change, re-asserted at flyout-open), and any color that
  lands mid-mode (a window-restore's replayed `setColor` action, the Manager record re-tint at
  claim, a `setTabColor` keybinding, a tear-out recreated here) is immediately parked by the
  `_OnClaudeTabColorChanged` chokepoint (`TabColorChanged` is wired for every tab). Persistence is
  NEVER voided: `dir-colors.json` + `SessionInfo::tabColorHex` are neither read nor written
  (`ResolveSessionColorHex` answers empty so board band / Sessions chip / pending-dots render
  neutral), a shell tab's `actionsJson` `setColor` persists the PARKED color
  (`BuildStartupActions`' runtime-else-suspended fold), and the window record's `managerTabColor`
  captures through `GetPersistableTabColor` — so switching back restores exactly the prior colors;
  grouping/dir semantics stay classic (`EffectiveWorkingDir` ⇒ cwd)];
  GLOBAL, applied live on Save + cross-window broadcast via `_ReapplyManagedTabColors`; every color
  read-surface — board title band, Sessions chip, pending-dots contrast — resolves through the shared
  `ResolveSessionColorHex(mode, s)` so cards/chips always match the tab. **The inferred dir is the
  session's EFFECTIVE working dir everywhere the UI asks "where does this session belong", not just
  its color** (`EffectiveWorkingDir(mode, s)` in Persistence — the ONE semantic resolver;
  `SessionColorKeyDir` builds on it, diverging ONLY to canonicalize a git **WORKTREE** effective dir
  to its **main repo root** (`ResolveWorktreeMainRoot`, memoized) so a repo + all its worktrees share
  ONE color while grouping/mechanics stay worktree-granular; a non-worktree dir is unchanged so color
  key == effective dir everywhere else): the Explorer-Tree
  **grouping** + dir **scope** + rename-uncollapse, the board card's **dir line** (tip carries
  `launched in <cwd>` when divergent) + scope filter, the Auto-Testing **header dir** +
  **apply-template-to-dir broadcast**, the **selection pre-aim** into the Launch box, every
  **Open New Session Here** (tree/board rows via `_WorkDirOf`, the WT tab menu), the per-tab
  overlay's **row-2 subline** + **Open Path** + the shared **Copy Path** (`CopySessionField` case 1,
  mode threaded as a defaulted param; the overlay mirrors the mode via
  `AgentTabOverlay::SetTabColorMode`, seeded on attach + re-broadcast by `_ReapplyManagedTabColors`),
  and the rich tab **tooltip** (header leaf = effective dir; the dim detail row flips to
  `launched in → <cwd>` under the Inferred mode, staying `inferred → <dir>` in the others). The
  **terminal's cwd keeps owning the MECHANICS** — spawn/resume/fork/restart cwd, hook + transcript
  correlation (`EncodeCwdToProjectDir`), per-dir env, recent-dirs MRU, persistence, the summary box's
  `Dir:` — so launching, restoring, and correlation are byte-identical to before; the `agentmaster`
  CLI stays fact-based (an `inferredWorkingDir` JSON field + a `works in:` line on `show` + `--dir`
  matching either dir, unconditional on the mode)), and (TABS section)
  **`flashRingColor`** (the **Status flashing color** picker — a
  `muxc::ColorPicker` with its **alpha slider enabled**, so one control sets both the hue AND the
  **opacity** of the tab status-dot **"unread" flash ring**; stored `#AARRGGBB`, **default red at 80%
  opacity** `#CCFF0000`; GLOBAL, applied live on Save + cross-window broadcast via
  `_RefreshFlashRingBrush` re-pointing each window's shared `_flashRingBrush`), and (TABS section)
  **`pendingDotsLightColor` / `pendingDotsDarkColor`** (the **Pending dots (on dark/light tabs)** picker
  PAIR — two `muxc::ColorPicker`s for the unsent-draft "3 dots" color, painted LIGHT-on-dark / DARK-on-light
  auto-picked by the tab background's WCAG luminance so the dots are never invisible; both `#AARRGGBB`,
  defaults gold `#FFE0A92B` / amber `#FF5A3E00`; GLOBAL, applied live — tab dots re-read on the next scan
  tick, board cards on the next rebuild; PENDING_INPUT.md).
  A **PROFILE row** (read-only path + **Change profile
  folder…**) shows the ACTIVE per-install profile dir and re-runs the ProfileBootstrap picker —
  deliberately NOT an `AppSettings` field (the profile is the pointer TO `settings.json`, stored
  in the `.agentmaster.profiles` choice file / env, never inside the profile it selects); a change
  applies on the NEXT start and is shown staged as `current → new (after restart)` until then
  (PROFILES.md). It also carries non-cog global state set elsewhere in the
  UI but persisted through the same file: `showTabOverlay`, **`showSummaryPanel`** (the per-tab summary
  panel's pencil toggle — GLOBAL across windows, written by `_ToggleSummaryPanel` via a settings.json
  read-modify-write, NOT the cog; preserved from disk on a cog Save), **`summaryPanelWrapNewlines`** (the
  summary panel's wrap-line toggle — same GLOBAL + freshest-disk-RMW idiom via `_ToggleSummaryWrap`,
  preserved on a cog Save in both save paths; default off), **`treeSort`** (the Explorer Tree's
  NEWEST/OLDEST/MOST ACTIVE/A–Z sort — written by the tree's sort toggle via the settings sink, NOT
  the cog), and **`archiveSplitFraction`** (was the Archive page's table|detail split; **now unused** —
  the Archive page was removed, FAVORITES.md — the field is kept for back-compat so an old settings.json
  round-trips unchanged), plus **`summaryPanelWidthFraction`/`summaryPanelHeightFraction`**
  (the summary panel's drag-resized size), **`summaryPanelTruncate`** (its truncate-long-messages toggle),
  and **`hiddenSessionIds`** (the Sessions browser's per-row **Hide from list** set; the cog also carries a
  **Reset hidden sessions** button — `_resetHiddenSessionsHandler` — that clears it). Loaded at engine init, seeded via `SetSettings`,
  persisted + re-materialized on Save via `SetSettingsHandler`. Every default reproduces prior
  behavior, so a missing `settings.json` (or any unset field) is a no-op.
- **Logging & observability (`hooks.log`).** All traces go through `AppendStateLog(fileLeaf, line)`
  (`ClaudeSpawn.cpp`, thread-safe + best-effort). Three layers: (1) the **hook event stream**
  (`[SessionStart]`/`[UserPromptSubmit]`/`[Stop]`/…) — the push state machine; (2) **engine-mechanism
  tags** — `[fork]`/`[resume]`/`[restore-fresh]`/`[rehome]`/`[spawn]`/`[launch-fail]`/`[archive]`/`[teardown-archive]`/
  `[recon-*]`/`[send]`/`[hold]`/`[enter-retry]`/`[codex-*]`/`[adopt-*]`/`[pending]`/`[notify]`/`[update]`/`[cmd]`/`[cmd-fire]`/`[cmd-expire]`/`[persist-fail]`/`[observer]`/`[activity]`/… (each
  carries the resulting ids), plus the **window-restore story** — one coherent trace per `windowId`:
  `[window-claim]`/`[window-fresh]` (claim a saved record or start fresh, at engine init) → `[rehome-begin]`
  (every tab ref listed BY SESSION ID + the focus target) → per-tab `[rehome] window <id> resume|skip <sid>`
  (each NAMED; a skip carries its reason — unknown / already-live / dedup) → `[rehome] window <id> resumed=N
  shells=M skipped=K` (the end counts), with `[window-save]` the change-gated persisted tab set (what
  re-homes next launch) and `[reopen]` the "Reopen Windows (N)" dispatch; and (3) the **`[nav]` USER-NAVIGATION
  AUDIT TRAIL** — the user-INTENT layer
  ABOVE the mechanism tags (whose ids it references), so `grep '\[nav\]' hooks.log` reconstructs the whole
  journey. Helpers: `LogNav(msg)` writes `[nav] <msg>\n`; `ShortId(id)` = the first-8-char convention
  (`ClaudeSpawn.h/.cpp`). **Begin/end pairing (crash-resilient):** every action that LAUNCHES a process or
  tears one down logs a **BEGIN** before the work + an **END** after — so a begin with no matching end
  pinpoints a crash/hang mid-action. The END of a resource-creating action carries the **new managed id**,
  resolved from the just-created tab via `_ClaudeSessionForTab` (so it reflects what ACTUALLY opened — a
  transcript-gated resume/fork that degraded to FRESH reports the NEW id, not the requested one). Paired
  funnels: `fork-click` ↔ `fork-done new=… from=…` (the Sessions-page on-disk fork) · `fork-managed-begin`
  ↔ `fork-managed-done` (the LIVE managed-session fork — the WT tab "Fork session" / board+tree menu) ·
  `resume-click` ↔ `resume-done` (or the early `resume -> jump` when already open) · `open-new {claude,codex}`
  ↔ `open-new … done` · `adopt-{external,codex}` ↔ `adopt-… done` · `restart-begin` ↔ `restart-done` ·
  `handover-begin` ↔ `handover-done new=… from=…` (the /handover successor spawn — COMMANDS.md) ·
  `close-begin` ↔ `close-done` · `sessions-page open-begin` ↔ `shown` (the page-open crash class) ·
  `reopen-windows-begin` ↔ `…-done` (the toolbar "Reopen Windows (N)" dispatch loop). A blocked launch (no
  native `claude.exe`) still emits its `…-done (blocked …)` so an unpaired begin ALWAYS means a crash, never
  the gate. Other covered funnels (~52 sites total, all
  user-action-driven — never per-tick; search is debounced): **Sessions page** — `search` (q + active scopes
  + fast count, then `search-done content=N`), `select` (row click AND Up/Down nav, tagged `via=name/dir |
  content | browse` — the field a row matched, the line that makes "why did this row surface" self-evident),
  `favorite on/off`, `hide`/`unhide`; **cross-cutting funnels** (cover the Manager board/tree + page + tab
  menus) — `activate` (local vs cross-window fan-out), `rename` (Explorer/Manager `_RenameClaudeSession` AND
  the tab-strip `_SyncClaudeTitleFromTab`, the latter only on a real change); **Auto Testing** — `queue`,
  `send-now` (+ delivered vs no-injector rollback), `autorunner -> Off|Semi|Full`, `pause-all`, `template-save`
  (a session's queue → a reusable plan) + `template-apply` (the template's prompts → one session, or → EVERY
  session in a dir — the broadcast carries the affected count); **navigation**
  — `tab-focus` (the core "where is the user now"; gated on `Initialized` so a restore's focus-restore can't
  spam it), `tab-swap` (a tab's bound conversation changed in place — `/clear`/`/resume`/`/compact`, beside
  `[rehome]`), `manager select-external`, `jump-to-prompt` (a summary-panel ▸, SUMMARY_JUMP.md),
  `notify-click` (a session's Windows toast clicked → foreground its window + jump to its tab,
  NOTIFICATIONS.md), and the **tab-strip drag pair** (the v0.6.7 MUX drag-start AV forensics — see the
  Gotchas bullet): `tab-drag-begin <ident>` ↔ `tab-drag-end from=N to=M` (the gesture BEGIN/END — a begin
  with no end == died mid-drag; end carries the same-window reorder result or `(no same-window reorder)`;
  a Manager-tab attempt logs `tab-drag-begin refused`), `tab-move <ident> idx=N -> M` (moveTab action /
  drop routing; `refused (manager tab is pinned)`), `tab-send-to-window <ident> win=<id|-1> idx=N` (the
  tab left this window: `-1` = torn out into a NEW window, else a cross-window drop — beside the
  managed-session `[move-out]`), with mechanism tags `[tab-new] idx=N tabs=M <ident>` (every strip
  INSERTION timestamped — the insert→drag gap is the crash-window measurement), `[pin-manager]`
  (a drop displaced the pinned tab; snapped back to 0), and `[tabdrag-guard]` (the AV guard's deferred
  arm / re-enable / settle-threw; `<ident>` everywhere = `_DescribeTabForLog`: `<sid8> "<title>"`,
  title-only for a shell tab);
  **lifecycle/window** — `mark-unread` (the tab "Mark Unread"; its clear twin is an automatic tab VISIT,
  already covered by `tab-focus`), `move-out` (a managed tab dragged to ANOTHER window — where the session
  went, beside `[move-out]`), `manager bring-to-front` (surface an external's hosting window),
  `sessions-page close (back)` (the explicit Back; the programmatic hide after a resume/fork rides THAT
  action), the **close/quit DECISION layer** (every confirm dialog's outcome — an "asked, declined" is
  otherwise invisible): `close-cancelled <sid8>` + `close-confirm <sid8> (favorite & close | unfavorite &
  close)` (the single-tab 3-way's Cancel / star-flip; plain Close needs no line — `close-begin/done`
  follows immediately), `close-all begin tabs=N managed=M [favorite-all]` ↔ `close-all done` (the batch —
  an unpaired begin == died mid-batch; `close-all cancelled …` for either dialog's decline),
  `window-close cancelled|confirmed|-> close ALL windows (quit requested)` (the window ✕ 3-way), `quit
  cancelled|confirmed (closing all windows)` (RequestQuit's own confirm), and **workspace mutations**:
  `new-window` (`_OpenNewWindow`), `pane-split <ident>` (Initialized-gated so a restore's replayed splits
  can't spam it), `duplicate-tab <ident>` (a plain shell duplicate; a managed tab takes the fork path =
  `fork-managed-*`), `tab-color <sid8> -> <#hex|reset> (individual)` / `tab-color dir=<dir> -> <#hex|reset>`
  (a genuine user pick/reset — the equality guards filter our own paints), `tag recolor "<name>" <#hex>`
  (the explicit swatch recolor of an existing tag, beside the existing `tag add`/`tag remove`/`tag delete`);
  **clipboard/shell** — `copy <field>` (the ONE `CopySessionField` chokepoint behind EVERY copy
  menu — the per-tab overlay's AND the board/tree Copy submenu — `field` ∈
  session-id/path/branch/claude-cli/codex-cli/transcript/summary), `open-path` (the overlay folder button →
  explorer); **cog/settings** — `settings-save` (the cog Save — logs the behavior-impacting fields
  skipPerms/model/autorunner/claudeExe), `profile-change <from> -> <to>` (re-point the install's profile
  folder, applies on restart), `check-for-updates` (the interactive button only — not the silent on-open
  check), `reset-hidden-sessions` (un-hide every Sessions-browser row). The fork chain reads
  `[nav] sessions fork-click row=… → [sessions-page->fork] source=… →
  [fork] <new> (forked from <source>) → [nav] sessions fork-done new=<new> from=<source>` (row-clicked →
  resolved source → new id, end-capped so a crash mid-fork is visible). **Deliberately UNLOGGED**
  (low signal / would add noise): Auto-Testing queue micro-edits (move/delete a Pending row), the settings-cog
  OPEN + the summary-panel pencil/wrap toggles + the path-picker navigation, and the pure view-filter toggles
  (sort column, scope LOCAL/GLOBAL/EXTERNAL, window preset, row-filter facets — the active scopes already ride
  the `search` line). **Census log gating** (the observer's
  `[observer] census`): re-logs only on OUR-fleet change + a 5-min keepalive, NOT on external-world churn —
  see the *Fleet Observer S-lane* bullet. **Timestamps (DONE):** every record is now prefixed at the
  `AppendStateLog` chokepoint with a LOCAL-time `[HH:MM:SS.mmm]` stamp (applied only at a line boundary, so
  a partial-line caller is never split mid-line), time-ordering EVERY layer at once — turn time, Enter-retry
  gaps, observer lag, and the span between a `[nav]` begin and its end now read straight off the log.
  **Silent-failure surfacing (DONE):** a launch that built no tab (`[launch-fail]` — the Claude + Codex
  null-pane returns) and any failed state write (`[persist-fail]` — the `WriteAllUtf8` chokepoint behind
  sessions.json / window-records / templates / dir-colors / …, i.e. state silently lost on next launch) now
  LOG instead of vanishing.
  **Exception forensics — `[exc]` / `[wil]` (DONE; the *never lose a swallowed exception* policy, Rule #18).**
  A `catch (...)` that recovers silently used to discard BOTH what was thrown and where from — and a catch
  block runs AFTER the unwind, so by then the throw-site stack is already gone. A process-wide **Vectored
  Exception Handler** (`InstallThrowStackCapture`, `ClaudeSpawn.{h,cpp}`) therefore captures at **raise**
  time (VEH runs *before* unwinding): every MSVC C++ throw (`0xE06D7363`) snapshots ≤64 frames + a tick into
  a **per-thread 4-entry ring** — lock-free, allocation-free, capture-only (`EXCEPTION_CONTINUE_SEARCH`
  always, dispatch untouched), so a coroutine's stored exception re-raised at `co_await` captures again while
  the older ring entries keep the ORIGINAL throw. Two reporters share one chokepoint
  (`LogSwallowedExceptionCore`): **`Agentmaster::LogSwallowedException(L"<context>")`** for the plain-C++
  engine TUs (no WinRT — they must keep compiling in the standalone harness + the CLI) and
  **`AgentLogCaughtException(context)`** (`AgentCatchLog.h`) for the TerminalApp layer, which additionally
  classifies `winrt::hresult_error` (hr + message) / `wil::ResultException` / `std::system_error` (**RTTI is
  OFF repo-wide**, so classification is by rethrow, never `typeid`). `AgentCatchLog.h` also installs
  **`AgentWilFailureLogger`** — the `wil::SetResultLoggingCallback` sink for **TerminalApp.dll** (wil state
  is per-MODULE), so every pre-existing `CATCH_LOG` / `LOG_*` / fail-fast in that binary now lands as
  `[wil] <kind> hr=… at <file>:<line> … ret=Module+RVA`. Both are `noexcept`, throttled ~2s per context
  (fails OPEN, with a `[suppressed N earlier repeat(s)]` counter), and installed process-once by
  `InstallAgentExceptionTrace()` at engine init (`[exc] exception forensics installed …`). Frames log as
  **`Module.dll+0xRVA`** — ASLR-stable, so a bare hooks.log line is symbolizable offline later against that
  build's PDBs: `./.claude/skills/debug-dumps/scripts/diasym.exe <TerminalApp.pdb> 0x22B0F9` (the PDB **must**
  match the build that produced the log). Shape:
  `[exc] _ObserverProbe: swallowed winrt::hresult_error hr=0x8001010E msg="…" tid=0x1586C (no crash)` followed
  by `[exc]   throw#0 (age 2ms, 31 frames): TerminalApp.dll+0x22B0F9 …`. Swept across the TerminalApp layer
  (35 sites / 10 files) and the engine (31 sites; 9 already logged), each keeping its recovery side-effects —
  **except the 9 sites that are themselves ON the logging path, which must stay bare (see Gotchas).**

Follow-ups (not blocking): the PROFILES.md §5 set (per-identity defterm/shellext CLSIDs — the one
shared seam left between the release and dev packages; distinct dev iconography; profile
export/import); feed `pauseOnHumanInput` from a TermControl input tap;
bracketed-paste for true multi-line prompt bodies; a live buffer "peek" in the Auto Testing;
**bulk open** (the Sessions page's background Resume/Fork) re-opens tabs lazily (a non-foreground tab starts its `claude` only
when first focused — WT's lazy-background-tab behavior; open one at a time to force start);
a one-time **"Restore your previous layout?"** launch prompt (offers **all archived sessions**;
decided + **deferred** — it ships *after* the per-window `WindowRecord` capture is wired so it
restores true per-window layouts, not a flat global list — see `PERSISTENCE.md` §6/§6a);
prevent splitting the Manager tab. The **per-tab link badge / overlay**
([`TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md) / DESIGN §9.7) is **built** — `AgentTabOverlay`
wrapped in the terminal's slot `Grid` (`TerminalPaneContent::SetAgentOverlay`), driven by
`SessionRegistry::HasInjector` + `AppSettings.showTabOverlay`, attached on bind
(`_AttachClaudeOverlay` / `_claudeOverlays`) and **enriched by the Fleet Observer** with
`model · effort · kind` (O6). It now also shows on **every non-bound tab** as a registry-LESS
**observe badge** (`○ <kind> · unlinked`): the `_ObserverProbe` UI lane reads the observer's
`Correlation()` + `Activity()` tables and `_SetTabActivityBadge`s each tab's kind via
`AgentTabOverlay::ShowActivity` (registry-less — there is no session to observe; keyed by `WT_SESSION`
in `_pendingOverlays`, idempotent on an unchanged kind) — a `pwsh` / `cmd` / `codex` tab, or a
never-prompted `claude` (correlated but no transcript id yet, §11d). The badge flips kind in place as
activity changes and is **promoted to the bound `_AttachClaudeOverlay`** the instant a claude resolves
an id (its first prompt); `_DropPendingOverlay` collapses + releases it when the tab binds, the claude
exits, or the tab leaves the window's roster. The linked badge now also carries a **second (dim) row** —
`<root workdir folder>/<branch>` (e.g. `myworkdir/feature/issue123`, `AgentTabOverlay::_subline`) so the
session's place + branch read at a glance; hidden when there's no dir/branch and on observe badges. The
branch is the **live** current branch (`ReadGitBranchForDir` — read from `.git/HEAD`, handling a
worktree/submodule `.git` FILE + a detached HEAD → short SHA), distinct from a transcript's historical
first-seen snapshot; and the observer's `ObservedClaude.gitBranch` (fed into `SessionRegistry::ObserveClaude`,
which does `assign(s.branch, o.gitBranch)`) is now also a **live writer** for `SessionInfo.branch` (the
round-3 audit's "no live writer" gap), beside the off-thread transcript backfill.
The linked badge also carries a **third (dim) row** (`AgentTabOverlay::_promptLine`) — a preview of the
**next queued prompt** waiting to be sent: `⏳ <first line of the first `Pending` prompt, ≤300 chars>`
(a trailing `...` when that first line surpasses 300 chars OR there's more content behind it, via the
anon-namespace `FirstLinePreview`). It is the per-tab echo of row 1's `⏳N` count — the count is HOW MANY,
this is WHAT'S NEXT (the same item `Scheduler::DecideAdvance` fires next) — so it's **mode-agnostic** (shown
whenever something is `Pending`, regardless of Tests Autorunner). It **wraps** (so the full ≤300-char line can
show) but is `MaxWidth`-capped + right-anchored so a long prompt can't balloon the HUD; the hourglass run is
goldenrod (matching the row-1 `⏳N`), a hover tooltip reveals the FULL prompt, and it's **hidden** when
nothing is queued and on observe badges (built only via `_Refresh`, a linked session).
The badge also carries an **always-shown action group on row 2** (left of the dir/branch label) — a
**folder** button (Open Path: the session's working dir via `explorer.exe`, off-thread) + a **copy** menu + a **pencil**. The copy menu
yields `Session Id` · `Copy Path` (working dir) · `Copy Branch Name` · `Claude Launch CLI` · `Codex
Launch CLI` (each the **REAL** full command — the live process commandline read from the PEB
`ReadProcessCommandLine`, or the builder Launch/Restore would use, NOT a toy `--resume <id>`) ·
`Summary` (the full textual session box) · `Transcript` (the whole conversation, user + assistant TEXT
only via `ReadConversationText` — no tools/results/thinking). Every copy / Open Path plays a short
confirmation chime (`PlaySoundW`). Built only for a LINKED session; the action buttons stay visible —
the whole badge is dim at rest (opacity ~0.55) and brightens on hover or while the copy menu is open
(`_SetExpanded` toggles the badge opacity, not the buttons' visibility).
The pencil toggles a **SUMMARY PANEL** — a **second overlay** in its own slot stacked **below the
badge** (`TerminalPaneContent::SetAgentSummaryOverlay`, defaulting to **20% of the pane width** but
**drag-resizable** 8–50% wide / 6–75% tall via a grip (persisted as `summaryPanelWidthFraction`/`summaryPanelHeightFraction`), re-capped
on the wrapper's `SizeChanged`), shown while the **GLOBAL** `AppSettings.showSummaryPanel` is ON. The
toggle is **global, not per-session** (mirrors `showTabOverlay`/`treeSort`): the pencil hands off to
`TerminalPage::_ToggleSummaryPanel` — a freshest-disk read-modify-write of just that field + a **live
broadcast** to every linked overlay in the window (`AgentTabOverlay::SetSummaryEnabled`); other windows
adopt on next launch, and the cog Save preserves it from disk (like `hiddenSessionIds`). The panel
renders the **`~/.claude/hooks/session-end.js` box**, a faithful C++ port of that analyzer in
`ProcessInspect`: `AnalyzeSessionTranscript` (one forward pass → user messages [deduped, noise-
filtered], files read [`Read`] / edited [`Edit`/`Write`], branch, first/last timestamps, tasks [last
`TodoWrite`], plan-start/plan-end signals, parent session id, plan file) + `FormatSessionDuration` +
`FindPlanFileInTranscript` (a plan lives in the PARENT transcript). It is analyzed **off-thread**
(`_LoadSummaryAsync`), reloaded only when the transcript **mtime grows** (a quiet tab costs one
`GetFileAttributesEx`) and is mtime/loading-guarded. The **displayed** panel is a **TRIMMED** view
(`full=false`) — it omits everything panel 1 (the badge) already shows (the live-state header [kept only
for the plan-start/plan-end signal], session id, Dir, Folder, the resume CLI, Branch, Codex
`model·effort`), leaving the value-add: Parent/Plan, Tasks, Messages, Files Read/Edited; the copy menu's
**`Summary`** (`CopySummaryAsync`) yields the **COMPLETE** box. A **wrap-line toggle** (↵, font-sized;
dim when off / a lighter shade when on) at the RIGHT of the panel's **times bar** (the live
age/last-user-msg/last-activity line stays left — a 2-column `Grid`) flips how a message renders its
newlines: OFF (default, the session-end.js look) collapses each message to ONE line with newlines
escaped to a literal `\n` (500-char cap), ON **preserves** the real newlines (multi-line, capped at 6 lines;
the panel scrolls). GLOBAL + persisted exactly like `showSummaryPanel` (`AppSettings.summaryPanelWrapNewlines`,
default off): `TerminalPage::_ToggleSummaryWrap` does the freshest-disk RMW + **live broadcast**
(`AgentTabOverlay::SetSummaryWrapNewlines`, which invalidates the mtime gate so the panel re-renders —
a `_summaryWrapDirty` flag covers a toggle that lands while an analyze+render is mid-flight), and the flag
threads through to BOTH the displayed panel and the copy-menu `Summary`, for Claude (`RenderSummaryBox`)
and Codex (`RenderCodexSummary` — via `SummaryEscapeMsg`'s `wrapNewlines` mode). Section separators fill the panel
**border-to-border** — the body is a `StackPanel` of monospace `TextBlock`s interleaved with full-width
`Border` rules (`HorizontalAlignment::Stretch`, re-fills on resize; a fixed run of `─` can't in a
wrapping block), driven by a sentinel line (`\x1F`) the display turns into a `Border` and the plain-text
copy turns into a `─` rule. **System-injected "user" messages are filtered** out of the Messages list
(`SeIsCommandNoise` (in `ProcessInspect.cpp`, exposed for tests) — the summary-only filter, distinct from titles/Auto-Testing's
`IsNoiseUserPrompt`): `<command-*>` / `<bash-*>` echoes, `<task-notification>` /
`<output-file>` / `<status>`+`<summary>`, subagent telemetry `<usage>` / `<subagent_tokens>`, background
bash `<bash-notification>` / `<shell-id>` / `<persisted-output>`, `<background-task-input>`,
`<system-reminder>`, AND a message that begins with a Claude-Code **TUI output marker** (`●` U+25CF /
`⏺` U+23FA / `⎿` U+23BF — assistant/tool output the user pasted back in, never a typed prompt; matched by
code point so the source stays pure-ASCII) — the set chosen after a **full-corpus `jq` scan of all ~4900 on-disk sessions**;
real content (C# generics `<int>` / `<T>`, exceptions, XML-doc `<summary>` / `<remarks>`, C++
`#include`s, MSBuild/HTML, the user's review/diff templates, own type names, doc placeholders) is
deliberately **kept** (the `<status>`&&`<summary>` combo guards against filtering C# XML-doc).
Milestones tracked in `doc/agentmaster/IMPLEMENTATION.md`.

## Repo facts

- Forked from `microsoft/terminal` @ `v1.24.2372`; work branch **`agentmaster`**.
- Build entry: **`OpenConsole.slnx`** (slnx format). No git submodules in 1.24
  (`doc/building.md` is stale on that point).
- Toolchain: VS 2022 + C++/UWP workloads + Windows SDK 10.0.22621/26100.
- **TWO package identities, one branding** (`PROFILES.md`): the **release** identity
  **`Agentmaster`** (PFN `Agentmaster_56k4f06dsfp9r`, alias `agentmaster.exe`,
  `Package-Rel.appxmanifest` — selected by `/p:AgentmasterPackageIdentity=Release`, what
  `release.yml` ships and what v0.1.x already shipped) and the **dev** identity
  **`AgentmasterDev`** (PFN `AgentmasterDev_56k4f06dsfp9r`, alias `agentmasterdev.exe`, Start
  menu "Agentmaster Dev", `Package-Dev.appxmanifest` — the default; what the local loose layout
  registers). Distinct identities + per-identity aliases + the per-install state **profiles**
  are what let the released app and the dev build install side by side with zero collision —
  and both are deliberately **distinct from `WindowsTerminalDev`** so they coexist with real
  Windows Terminal. ⚠️ There is a **separate `K:\source\windowsterminal` checkout on this
  machine that owns the `WindowsTerminalDev` identity** — never reuse that identity here (see
  Gotchas). The Publisher is **`CN=Agentmaster` for BOTH** — an honest self-signed identity,
  NOT `CN=Microsoft Corporation` — and it is what **derives the PFN hash `56k4f06dsfp9r`**
  (the hash depends on the Publisher ALONE, which is why both families share it), so the
  public-release self-signed cert can carry that exact subject (an MSIX signature is valid only
  when the cert subject == `<Identity Publisher>`; see *Releasing a public version*). No C++
  hardcodes a PFN: `GetWtExePath` + the reopen dispatch key on the `AgentmasterDev`/`Agentmaster`
  name *prefixes* (**Dev first** — `Agentmaster` is a prefix of `AgentmasterDev`);
  `windowClassName` uses `GetCurrentPackageFamilyName()` at runtime. The dev loose-layout
  register is **unsigned** (no `CascadiaPackage_TemporaryKey.pfx` ⇒
  `AppxPackageSigningEnabled=false` in the wapproj). Hash recompute if a Publisher ever changes:
  SHA-256 the UTF-16LE Publisher, first 8 bytes, base32 over `0123456789abcdefghjkmnpqrstvwxyz`.
  Known shared seam: the defterm/shellext CLSIDs are upstream-Dev GUIDs in BOTH manifests — see
  `PROFILES.md` §1/§5 (don't set the dev build as the OS default terminal).
- Our additions (all marked `Agentmaster`):
  - `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}` — the Manager tab content (C1 UI).
  - `src/cascadia/TerminalApp/AgentMaster/` — the engine (plain C++, no WinRT; the `.cpp`
    are `<PrecompiledHeader>NotUsing`): `SessionModels.h` (`SessionInfo` + the managed-Codex two-id
    model — `AgentKind kind` / `codexSessionId` — and `TabKind::Codex`), `HookEvents.h`, `HookWire.h`,
    `SessionRegistry.{h,cpp}`, `HooksBridge.{h,cpp}`, `ClaudeSpawn.{h,cpp}` (+ `BuildCodexCommandline`),
    `Scheduler.{h,cpp}`, `Engine.{h,cpp}` (the M9 process-wide `SharedEngine`),
    `SessionScanner.{h,cpp}` (the interval reconciler / PULL transcript tail; its parser also emits
    the non-turn `Kind::Command` slash-command echoes + assistant `fileWritePaths` — COMMANDS.md),
    `CommandWatch.{h,cpp}` (slash-command bindings + bounded async awaits — `ParseCommandEcho`,
    the markdown await, the /handover binding's feed; COMMANDS.md), the **Fleet
    Observer** — `Activity.h` (data models — incl. `AgentKind` / `CodexProcessFacts` / `CodexState`),
    `ProcessInspect.{h,cpp}` (PEB / Toolhelp / transcript primitives — id resolution + content: title /
    prompts / ctime·mtime timing; the Codex date-sharded rollout resolver + `ReadCodexFacts` + the C2
    rollout-tail state deriver `ClassifyCodexLine`/`ReadCodexStateDelta`; the summary-panel
    `session-end.js` port `AnalyzeSessionTranscript` / `FormatSessionDuration` / `FindPlanFileInTranscript`
    + `ReadConversationText` + the summary-only noise filter `SeIsCommandNoise`),
    `ProcessObserver.{h,cpp}` (the S-lane; also validates + publishes the `sessions/<pid>.json`
    presence heartbeat) — `TranscriptStore.{h,cpp}` (the on-disk Claude-session
    store API for the Sessions browser, SESSIONS.md §6: global transcript enumeration, the
    byte-offset-resumable scan + stats fold, fork-aware quick row facts, the per-session sidecar
    index, the raw presence read, and the shared prompt-noise + title-precedence rules),
    `SessionSearch.{h,cpp}` (the two-phase search: pure regex/match/snippet primitives + the
    history.jsonl accelerator + the rg-prefiltered, scope-attributed content scan with in-process
    fallback),
    `SessionStore.{h,cpp}` (the generalized DURABLE per-session KV — `session-store/<sid>.json`,
    one file per session: the `title` / `favorite` / `tags` keys; plus the **bookmark-tag**
    primitives — `NormalizeTagName`/`FoldTagName`, list encode/decode, per-session tag CRUD, the
    profile-level **known-tag registry** `tags.json` (Load/Register/UnregisterKnownTag — keeps a
    0-carrier tag alive) + `tag-colors.json` color map (Load/Get/SetTagColor), and `CollectGlobalTags`
    (the sessions ∪ registry universe)),
    `PromptAnchor.h` (header-only, pure — the **summary-panel JUMP resolver**, SUMMARY_JUMP.md:
    given the linearized terminal buffer + the conversation's prompts, resolves each to a buffer
    location with whitespace-tolerant fuzzy matching, backoff, partial "match as much as possible"
    scoring, an order-preserving greedy assignment for duplicate prompts, a **prompt-marker (❯/›)
    preference** that binds a match to the real user-prompt render over an echo (§5, with a legacy
    soft-fallback so it never regresses), and a **collision second pass** that de-conflicts prefix /
    suffix / substring / backoff overlaps on one render by region containment + a quality tiebreak
    (§5b); header-only so both the overlay path AND `ControlCore` (a separate DLL) share it.
    Unit-tested + benchmarked in `tests/`),
    `PendingInput.h` (header-only, pure — the **pending-input detector**, PENDING_INPUT.md: given the
    bottom region of the terminal buffer, finds Claude's input box by the bottom-most `❯` line wrapped
    by `─` rules and extracts the UNSENT draft; the `PromptAnchor.h` idiom — pure-ASCII source, header-
    only so `ControlCore` + `tests/` share it. Unit-tested in `tests/`),
    `Sha256.h` (header-only, pure — FIPS 180-4 SHA-256, hand-rolled like `Base64Encode` so no
    bcrypt/crypt32 has to be threaded through the lib + harness + CLI builds; the content-IDENTITY
    primitive behind the shipped `/handover`+`/handover-here` definitions' digest version history —
    COMMANDS.md §6. NIST-vector + padding-edge tested in `tests/`),
    `RegexUtil.h` (header-only, pure — the ONE **guarded regex component** every user-typed pattern
    goes through, COMMANDS.md §6b: `RegexIsValid`/`RegexSearch`/`RegexReplace` never throw (an
    invalid pattern reads as no-match / input-unchanged — the state a box being EDITED is in most
    keystrokes), cap pattern (512) + input (4096) against pathological backtracking, and fix ONE
    flavor — ECMAScript, `regex_search` semantics, optional case-insensitivity, `$1` backrefs,
    replace-ALL, plus an `applied` out-param as the "configured AND it did something" signal.
    Shared by CommandWatch (the file-match leaf qualifier), ClaudeSpawn
    (`DeriveHandoverSuccessorTitle`), Engine (bind-time validation), and the Settings cog (live
    validation). Unit-tested in `tests/`), `Json.h`, `Persistence.{h,cpp}`,
    `ProfileBootstrap.h` (header-only, pure Win32 — the per-install state PROFILE: resolution
    [env > portable marker > saved choice > per-identity default], the `.agentmaster.profiles`
    choice file, the first-launch TaskDialog picker + folder Browse, legacy-data migration,
    Terminal-settings seeding, and the one-instance-per-profile kernel mutex; included by the
    engine, AgentManagerContent, TerminalPage AND the WindowsTerminal EXE — PROFILES.md),
    `Updater.h` (header-only, pure Win32 like `ProfileBootstrap.h` — the in-app GitHub-release
    **auto-updater**: a bounded GitHub-API version check, the Update / Postpone (3·7·30 days) / Skip /
    Not-now TaskDialog, and the BAKED-IN `am-update.cmd` + `am-update.ps1` installer it writes into the
    active profile; included by the WindowsTerminal EXE [startup check] AND `TerminalApp.dll`'s Settings
    cog — see the *In-app auto-updater* Status block),
    `tests/` (standalone harness, not in the msbuild — run `tests/run-m5-tests.bat`), and
    `cli/` — the **commandline introspection tool** (CLI.md): `agentcli.cpp` (the read-only P1
    reader — `show`/`list`/`sessions`/`tabs`/`windows`/`external`/`--self`/`--json`, linking these
    engine units like the test harness) + `agentmaster-cli.vcxproj` (a CONSOLE exe shipped beside
    `WindowsTerminal.exe`) + `_compile.bat` / `_build-proj.bat` (standalone + isolation builds).
  - `src/cascadia/TerminalApp/AgentTabOverlay.{h,cpp}` — the per-tab link badge (TAB_OVERLAY.md),
    enriched by the observer with `model · effort · kind`; also the registry-less `ShowActivity`
    **observe badge** (`○ <kind> · unlinked`: pwsh / cmd / unprompted-claude / codex) for every non-bound tab.
    Carries the **hover action row** (folder Open Path + a copy menu — Session Id / Copy Path / Copy
    Branch / Claude·Codex Launch CLI / Summary / Transcript, with a chime — `BuildLaunchCli` /
    `CopyConversationAsync`) and the **pencil-toggled SUMMARY PANEL** (`SetAgentSummaryOverlay` 2nd slot;
    `RenderSummaryBox`/`RenderCodexSummary` with a `full` trim flag, `_SetSummaryContent`'s `StackPanel`
    + full-width `Border` rules, `_LoadSummaryAsync` off-thread, `CopySummaryAsync` for the full box) —
    whose times bar carries a **wrap-line toggle** (`_ToggleSummaryWrap`/`SetSummaryWrapNewlines`, the
    GLOBAL `AppSettings.summaryPanelWrapNewlines`: preserve message newlines vs literal `\n`).
  - `src/cascadia/TerminalApp/AgentToastActivator.h` — **System notifications' TOAST COM ACTIVATOR**
    (NOTIFICATIONS.md §4a): the per-identity ToastActivatorCLSID pair (release/dev, matching the two
    manifests' `<desktop:ToastNotificationActivation>` + `<com:Class>`), an
    `INotificationActivationCallback` whose `Activate` routes the toast's `launch` payload (the session
    id) through `ActivateSessionInOtherWindows` → foreground + jump, its `IClassFactory`, and
    `Register()`/`IsRegistered()`. Header-only + included by exactly ONE TU (`TerminalPage.AgentEngine.cpp`,
    which `std::call_once`s `Register()` at engine init) — the `PromptAnchor.h` idiom, so no vcxproj entry.
    It is what makes the shell activate the RUNNING instance in-process instead of launching a second
    process (whose no-arg startup the Emperor turned into a stray window).
  - `src/cascadia/TerminalApp/AgentStatusColors.h` — the ONE shared `SessionState` → color table
    (Triage-Board dot, per-tab overlay, and the tab-strip status dot all read it; replaced the
    overlay's hand-synced palette copy). Also the shared `#AARRGGBB` color parse/format
    (`ParseArgbHexColor` / `FormatArgbHexColor`, for `flashRingColor` + the pending-dots colors), the
    pending-dots contrast pick (`BackgroundIsLight` / `PendingDotsColorFor` — WCAG luminance, used by the
    tab strip + board card so the unsent-draft "3 dots" are never invisible; PENDING_INPUT.md), and the
    **bookmark-tag** color palette + resolvers (`kTagPalette`/`TagPaletteColor` — the picker's swatches;
    `TagColorFor` — the stable name-hash fallback; `ResolveTagDisplayColor` — the ONE user-picked >
    name-hash resolution every tag renderer shares: the `AgentTagsSpec` producer, the Tags panel/hover
    panel, and the Sessions Tags column).
  - `src/cascadia/TerminalApp/AgentTipHelpers.h` — the ONE islands-safe hover-tooltip recipe
    (`AgentSetTip` / `AgentCloseTipOn`; `ToolTipService`'s auto-dismiss is unreliable under XAML
    Islands), shared by `AgentManagerContent` + the Sessions page + the per-tab overlay + the tab
    strip (thin TU-local wrappers like `SessSetTip` delegate here). Architecture: ONE
    per-UI-thread tip HOST (one shared `ToolTip` + one one-shot open timer + a 1s while-open
    watchdog); per element only attached DPs (text + delay + optional placement/title) + three
    capture-less pointer handlers, and `SetToolTip` is attached only for the duration of a real
    hover-open — see the per-element-ToolTip LEAK gotcha (the 68 GB prod freeze) before touching
    this.
  - `src/cascadia/TerminalApp/AgentLocalTooltip.h` — **LocalTooltip**: a DESIGNATED-AREA tooltip
    surface — the hovered element's `AgentSetTip` text renders in ONE fixed panel (title auto-derived
    from the control's Header/Content, `AgentSetTipTitle` overrides) instead of a floating ToolTip
    chasing the pointer. `AttachScope(root)` = ONE bubbling PointerMoved per SCOPE (never per element
    — the 68 GB lesson; multiple roots may feed one panel), immediate + STICKY (gaps between controls
    don't strobe it), and marks the root so the floating host's open tick YIELDS inside
    (`LocalTipScopeProperty`, a live switch); three change-gated anchors — `AnchorTopLeftOutside`
    (left of the anchor, tops aligned) · `AnchorTopRightAbove` (right edges aligned, top at the
    HOST's top) · `AnchorTopRightInside` (nested in the anchor's top-right corner) — all growing
    only leftward; `SetClickThrough(true)` for placements floating OVER content (never eats a
    click; the default swallows taps for over-a-modal-dim placements). THREE consumers: the
    **Settings cog** (`_settingsLocalTip` — left-outside the card, tap-swallowing), the **Sessions
    page** (`_sessionsLocalTip` — the header's empty right column above the detail pane,
    click-through, kept across tab-switch restore like the scroll offset), and the **Manager tab**
    (`_managerLocalTip` — nested in the Triage Board's top-right, click-through, scoped to the
    toolbar/board/bottom regions — NOT `_root`, so the settings/claude-missing overlay cards stay
    outside it; also retires the board cards' 4s tip hold-back *intrusiveness* rationale there —
    the side panel updates immediately). All ~200 existing `AgentSetTip` call sites on these
    surfaces feed the panels UNCHANGED; a window too narrow to host a panel falls back to the
    classic floating tips automatically.
  - `src/cascadia/TerminalApp/AgentCopyActions.h` — the ONE shared `CopySessionField` action
    (Session Id · working-dir Path · Branch · Claude/Codex Launch CLI · Transcript · Summary) behind
    BOTH the per-tab overlay's copy menu (`AgentTabOverlay`) AND the Triage Board / Explorer-tree
    session menu's Copy submenu (`AgentManagerContent`), so the two copy menus can never drift apart.
  - `src/cascadia/TerminalApp/TerminalPage.Agent{Engine,Sessions,Observer,WindowRecord,SessionsPage}.cpp`
    — the TerminalPage-side Agentmaster *implementation* in five same-class TUs (the upstream
    `TabManagement.cpp` pattern). (**`TerminalPage.AgentArchivePage.cpp` was DELETED** — the full-window
    Archive page is gone; FAVORITES.md.)
    **Engine** (`~TerminalPage`, `_InitAgentmasterEngine`, the Manager tab, `_WireAgentManagerContent`),
    **Sessions** (spawn/launch/restore/close/adopt-external for **both** Claude and Codex —
    `_LaunchCodexSession`/`_SpawnCodexSession`/`_AdoptExternalCodex` mirror the Claude seams — tab-title
    sync, smart naming + per-dir tab color), **Observer** (the per-tab overlay/badge, bind/reconcile/
    liveness incl. the managed-Codex state reconcile `_ReconcileManagedCodex`, the UI lane
    `_ObserverProbe`; also the **bookmark-tag UI** — `_SetTabAgentTags`/`_RefreshTabTags` badge spec, the
    Tags editor panel + color picker `_OpenTagEditorAt`/`_CommitTagEditorAdd`/`_RemoveGlobalTag`, and the
    rich `_ShowTagHoverPanelNow` hover panel), **WindowRecord** (M10 capture/flush/restore + reopen saved windows; Codex tab refs),
    **SessionsPage** (the full-window Sessions browser — SESSIONS.md — incl. the ★ favorite column +
    filter + `_ToggleSessionFavorite`, FAVORITES.md). Declarations stay in `TerminalPage.h`.
  - small touches in `TerminalPage.{h,cpp}` (~20 integration seams left in the `.cpp`:
    `_OnFirstLayout` startup, `_MakePane`'s `agentManager` branch, close/quit record-flush +
    teardown-archive, tab-move detach, title/color sync hooks, `_restartPaneConnection` injector
    re-point), `Tab.{h,cpp}` (a
    `TabColorChanged` event + `GetRuntimeTabColor`), `TabManagement.cpp` (incl. the generic
    `_DismissAgentPageOverlays` tab-switch seam), `TabHeaderControl.xaml` +
    `TerminalTabStatus.{h,idl}` (the tab-strip status dot: an `AgentStatusVisible`/
    `AgentStatusBrush`-bound Ellipse in the indicator row, plus the **FAVORITE marker** `Path`s — the
    `AgentFavoriteVisible`-bound gold **crown** at the dot's NW *or* the `AgentFavoriteStarVisible`-bound
    white, golden-tipped **star** behind the dot, mutually exclusive, chosen by the cog's
    `AppSettings::favoriteIcon` — FAVORITES.md §5a; **and the bookmark-TAG badges** — an always-open
    parented `HeaderTagBookmarksPopup` (the below-tab 30/70 overhang) whose ribbons are (re)built by
    `_UpdateTagBadges` from `TerminalTabStatus::AgentTagsSpec` and placed by the coalescing
    `_PositionTagBadges` → `_PositionTagBadgesNow` scheduler; hover raises `TagBadgeHoverBegin/End`);
    registrations in `TerminalAppLib.vcxproj`.
  - `src/cascadia/wt/shim.cpp` + `wt.vcxproj` (the `agentmaster <verb>` overload, CLI.md §2): the
    alias-target launcher shim is now **console-subsystem + dual-mode** (`SubSystem=Console`) — it
    execs `agentmaster-cli.exe` for a CLI verb / leading CLI-flag and forwards everything else to
    `WindowsTerminal.exe` byte-for-byte; the `agentmaster-cli.vcxproj` reference + the `wt`-mirrored
    entry in `OpenConsole.slnx` + `CascadiaPackage.wapproj` ship the CLI in the package.
  - `Package-Rel.appxmanifest` + `Package-Dev.appxmanifest` (the two identities; selection in
    `CascadiaPackage.wapproj` via `AgentmasterPackageIdentity`; each also declares its OWN
    **toast-activator CLSID** — `<desktop:ToastNotificationActivation>` + a `<com:Class>` under a
    `WindowsTerminal.exe` ExeServer with `Arguments="-ToastActivated"` — see `AgentToastActivator.h` /
    NOTIFICATIONS.md §4a; a change here needs a **re-register**), a comctl32-v6 dependency in
    `WindowsTerminal.manifest` (the profile picker's TaskDialog), the `AGENTMASTER_PROFILE`
    redirect in `TerminalSettingsModel/FileUtils.cpp` (Terminal's own settings →
    `<profile>\terminal\`), the profile bootstrap call in `WindowEmperor.cpp`,
    `doc/agentmaster/`, `tools/Build-Agentmaster.ps1` (incl. `-ReleaseIdentity`),
    `tools/am-lock.sh` (the global build/launch mutex — see Deploy & run → *Concurrency lock*).
  - `.github/workflows/ci.yml` + `.github/workflows/release.yml` — the GitHub Actions build gate
    + public-release pipeline (see *Releasing a public version*). The **only** CI in the repo;
    Windows Terminal's upstream Azure-Pipelines / OneBranch under `build/pipelines/` is
    Microsoft-internal and never runs for this fork.
- **Runtime state dir = the ACTIVE PROFILE** (PROFILES.md; historically — and still, as the
  release/unpackaged default — `%USERPROFILE%\.agentmaster\`; dev-package default
  `%USERPROFILE%\.agentmaster-dev\`; resolution: env `AGENTMASTER_PROFILE` > `.portable` marker >
  the `%USERPROFILE%\.agentmaster.profiles` per-install choice file > the per-identity default;
  picked on an install's FIRST LAUNCH — Production / Development / Browse… — and changeable from
  the cog's PROFILE row, applied on restart). Contents: `hooks-settings.json` +
  `agentmaster-hook.ps1` (the shared hooks config Claude is pointed at via `--settings`),
  `hooks.log` + `autorunner.log` (engine traces — `hooks.log` carries the hook event stream
  [`[SessionStart]`/`[Stop]`/…], the engine-mechanism tags [`[fork]`/`[resume]`/`[rehome]`/`[spawn]`/
  `[archive]`/`[restore-fresh]`/`[recon-*]`/…], the `[observer]` census, **and the `[nav]` USER-NAVIGATION
  AUDIT TRAIL** — see the *Logging & observability* bullet above; `grep '\[nav\]' hooks.log` reconstructs the user's whole journey),
  `forwarder-errors.log` (the hook forwarder's
  local silent-drop trace — a delivery that never reached the bridge: no sid / no pipe / a dead
  pipe's connect timeout; the bridge-side hooks.log only sees lines that ARRIVED), `sessions.json` (persisted fleet),
  `templates.json` (saved plans), `recent-dirs.json` (path-picker MRU), `dir-colors.json`
  (the **permanent** per-working-directory tab color map, schema **v2** — both user picks and
  auto-assigned colors, so a folder keeps its color across restarts), `session-store/<sid>.json`
  (the durable per-session KV — the `title` / `favorite` / `tags` keys), `tags.json` (the **bookmark-tag
  registry** — the durable CI-deduped name list that keeps a 0-carrier tag alive until its ✕ deletes it),
  `tag-colors.json` (the tag color picker's folded-name → `#AARRGGBB` map), `sessions-index/<sid>.json` (the Sessions browser's
  per-session search/stats sidecar cache — `(size,mtime)`-keyed, incrementally re-accumulated
  from the stored byte offset), `settings.json`
  (the Settings cog's `AppSettings`, incl. the updater's `allowUpdatePrerelease` / `updateSkippedVersion` /
  `updatePostponedUntilUnixMs` keys), `am-update.cmd` + `am-update.ps1` (the in-app updater's baked-in
  installer, written here on "Update now"; *Updater.h*), `windows/<id>.json` (M10 per-window UI-state records —
  one file per window; captured + autosaved + restored), `open-windows.json` (the M10 Increment-3
  open-at-exit manifest — the live window-id set the next launch reopens), `bridge.json`
  (live-bridge discovery for the shim), `shim/` (the transparent `claude` PATH shim —
  `claude.cmd` + a POSIX `claude` — that auto-wires hand-typed sessions; see *Adopt any
  `claude`*), and `locks/` (the `build-launch` mutex — `tools/am-lock.sh`; see Deploy & run →
  *Concurrency lock*; the lock dir stays at the FIXED `%USERPROFILE%\.agentmaster\locks\` — it
  guards the ONE build tree, not a profile), and `terminal/` (Terminal's OWN settings.json +
  state.json — the `AGENTMASTER_PROFILE` redirect in `GetBaseSettingsPath`, seeded from the
  install's previous location on first profile claim). Deliberately NOT under `%LOCALAPPDATA%`
  — see Gotchas (MSIX).

## Integration points (1.24 pluggable pane-content model)

- New tab content implements **`IPaneContent`** (like `ScratchpadContent` — no `.idl`).
  Ours: `AgentManagerContent`, compiled via `src/cascadia/TerminalApp/TerminalAppLib.vcxproj`.
- Content dispatch by type string in `TerminalPage::_MakePane` (`TerminalPage.cpp`): we add
  an `else if (paneType == L"agentManager")` branch → `make_self<AgentManagerContent>()`.
- The Manager tab is opened by **`TerminalPage::_OpenAgentManagerTab()`**, called from
  `_OnFirstLayout` *before* startup terminal tabs, so it lands at index 0 (leftmost), and is
  tracked in the `_managerTab` member (nulled on close in `TabManagement.cpp`, mirroring
  `_settingsTab`). It is **non-closable AND non-movable**:
  - *Non-closable* — `CloseButtonVisibility = Never` (→ `IsClosable(false)`) hides the X and
    `DisableCloseAndMoveMenuItems()` greys the context-menu Close/Move. **Critically,**
    `_updateAllTabCloseButtons()` (which re-applies the theme's global close-button setting to
    *every* tab on each theme/settings pass) **skips `_managerTab`** — otherwise it re-enabled
    the X right after we set it (see Gotchas). The only way to "kill" is closing the **window**.
  - *Non-movable* — its `TabViewItem` is `CanDrag(false)`/`AllowDrop(false)`; `_TryMoveTab`
    refuses to move it and reserves index 0 for it; and `_PinManagerTabFirst()` (called from
    `_TabDragCompleted`) snaps it back to 0 if another tab is dropped ahead of it.
- Tab placement primitive: `_CreateNewTabFromPane(pane, insertPosition)` (`TabManagement.cpp`).
- **Engine wiring (`TerminalPage`):** `_InitAgentmasterEngine()` (from `_OnFirstLayout`,
  before the Manager tab) **consumes the process-wide `::Agentmaster::SharedEngine()`** (M9) —
  copies the shared `SessionRegistry` + `HooksBridge` + `Scheduler` + `SessionScanner` +
  `ProcessObserver` `shared_ptr`s and registers THIS window's adoption handler + liveness probe
  (tokens, detached in `~TerminalPage`). The once-per-process wiring (the logging / scheduler /
  persistence observers, the pipe, bridge discovery + hook files + the PATH shim, the
  **`AM_SESSION`** mint + export, and the `ProcessObserver` start) lives in `Engine.cpp` and runs
  on first access. The Fleet Observer's UI lane is `_ObserverProbe()` (ticked by the scanner's
  liveness probe alongside `_ReconcileClaudeTabs` + `_SweepClaudeLiveness`; it replaced
  `_DiscoverClaudeTabsByCwd`): publish this window's tab roster, then bind via the observer's
  Correlation table. `~TerminalPage` calls `_observer->UnpublishWindow(_windowId)` (Rule #10).
  `_WireAgentManagerContent()` hands the content the shared registry + spawn / activate / archive
  / restore / rename / adopt-external / **codex-launch** / pause / confirm callbacks + the cog's
  settings seed/persist
  (`SetSettings`/`SetSettingsHandler`). `_LaunchClaudeSession(dir, title, restored)` builds a
  claude `ConptyConnection` (cmdline/cwd/env ours) and opens it as a normal terminal tab via
  `_MakePane(args, …, existingConnection)`; `_SpawnClaudeSession` = fresh,
  `_RestoreClaudeSessions()` = load the persisted fleet **as Archived** (process-once via
  the `Engine::restoreMutex` load barrier — a 2nd window blocks until it's loaded, then skips),
  `_RestoreArchivedSession()` = the on-demand resume.
  **Codex mirrors these seams (lifecycle + state only — NO injector/Tests Autorunner):**
  `_LaunchCodexSession(dir, title, restored, forkFromCodexUuid)` builds a `codex` / `codex resume <uuid>`
  / `codex fork <uuid>` `ConptyConnection` (`BuildCodexCommandline`, launcher resolved to a FULL PATH —
  `ResolveCodexLauncher` / `Engine::codexExePath`, a `.cmd`/`.bat` run via `cmd /c`; `AM_SESSION` stamp,
  no `CCMGR_*`) and opens it as a normal tab; `_SpawnCodexSession` = fresh, the Archive page's Restore +
  `_RestoreWindowTabs` re-launch a Codex record by `SessionInfo::kind`, and `_AdoptExternalCodex(pid,cwd,
  fork)` forks (`codex fork`) or resumes an external codex's rollout (the Fork-a-copy vs Resume-anyway choice).
  The launch bar's **Claude⇄Codex toggle** retargets the cwd box's Launch button to `_SpawnCodexSession`.
  A managed codex's rollout state (the C2 deriver) is folded onto the registry by the UI lane's
  `_ReconcileManagedCodex` (which also fills `codexSessionId` + `tabToken` on first prompt) —
  `ObserveClaude` is never used for it (Rule #13).
  `sessionId → Tab` lives in `_claudeTabs` (per window) for Activate / Archive / retitle. A
  session's **title is one value** (Explorer name == tab title == persisted `SessionInfo.title`):
  `_LaunchClaudeSession` **pins** it onto the tab (`Tab::SetTabText`); an Explorer rename routes
  through the `rename` callback → `_RenameClaudeSession` (registry + tab in lockstep); a WT tab
  rename (double-click / right-click **Rename Tab** / `renameTab` action, all via `Tab::SetTabText`)
  flows back through `_UpdateTitle` → `_SyncClaudeTitleFromTab`, which writes the registry (Rule #11).
  `_LaunchClaudeSession`/`_AdoptExternalSession` also **smart-name** an untitled session
  (`DeriveSessionTitle`) and **color the tab per working dir** (`_ApplyDirColorToTab` — the dir's
  persisted color, else `AssignDirAutoColor`'s collision-free auto color, which is then **persisted**
  so the folder→color mapping is permanent); a user color change flows `Tab::SetRuntimeTabColor` → the
  new `Tab::TabColorChanged` event → `_OnClaudeTabColorChanged`, which persists it to
  `dir-colors.json` and recolors every live tab in that dir (Rule #12).
- **Shared stdin:** the registry holds a per-session injector bound to that session's
  `ConptyConnection::WriteInput`, so the user's keystrokes and the scheduler's prompts both
  reach the same `claude.exe` stdin (Correctness Rule #3 binds the injector to the id).

## Building FAST

This machine: **i9-13900K — 32 threads (8 P + 16 E cores), 64 GB RAM.** The stock
`Invoke-OpenConsoleBuild` is slow because it runs msbuild with **no `/m`** (projects build
serially → 31 threads idle), builds the **whole** solution (tests/tools/samples), and
re-runs `nuget restore` every call. Per-file `/MP` is already enabled
(`src/common.build.pre.props:145`).

> ⚠️ **Hold the `build-launch` mutex before any full exe build, launch, or deploy** — see
> Deploy & run → *Concurrency lock*. (A lib-only compile-check, #6, doesn't relink the running
> exe and so needs no lock.)

> 🚀 **THE single biggest lever — skip the `.appxsym` symbol package (the wrapper now does, by
> default).** A binlog `PerformanceSummary` of a normal Debug package build measured
> `GenerateAppxSymbolPackage` at **~156s of a ~204s build = 77%**: it zips every full Debug PDB
> (hundreds of MB) into a Microsoft-Store symbol-upload bundle we **never consume** — Agentmaster
> ships the `.msix` + portable zips via GitHub, not the Store, and `release.yml` only ever uses the
> `.msix`. The PDBs still emit next to the binaries, so local debugging is unaffected; it is a FIXED
> ~156s tax paid on every build regardless of how much source changed. `Build-Agentmaster.ps1` now
> passes `/p:AppxSymbolPackageEnabled=false` by default, dropping the same build to **~20–50s (4–10×,
> measured)**; a RAW `msbuild` invocation must add the flag itself. Opt back in with
> `-WithSymbolPackage` only when you genuinely need a Store symbol bundle.

1. **Build on NVMe, not the A400.** `K:` is a DRAM-less SATA SSD; a WT build is tens of
   thousands of tiny files and 32 threads thrash it. Prefer **`Q:` (Kingston Fury
   Renegade, Gen4 NVMe + DRAM, ~546 GB free)** or `C:` (Corsair MP600 PRO).

2. **Use the wrapper** — parallel `/m` + per-file `/MP`, scoped to just the app target
   (`Terminal\CascadiaPackage`), skips the redundant double restore on rebuilds:
   ```powershell
   pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1            # first build
   pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore # inner loop
   ```
   Raw equivalent (note the appxsym-skip flag — the wrapper adds it for you):
   `msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /p:AppxSymbolPackageEnabled=false /t:Terminal\CascadiaPackage /v:m`
   64 GB handles unbounded `/m`; if it ever pages, add `-ClMpCount 6`.

3. **Windows Defender exclusions** (Admin, once — 20–40% on cold builds):
   ```powershell
   Add-MpPreference -ExclusionPath (Resolve-Path .)
   'MSBuild.exe','cl.exe','link.exe','cppwinrt.exe','midl.exe','mc.exe','VBCSCompiler.exe','nuget.exe','tracker.exe' |
     ForEach-Object { Add-MpPreference -ExclusionProcess $_ }
   ```

4. **Iterate incrementally.** The cold build (restore + cppwinrt projection) is the
   expensive one; afterwards `-NoRestore` rebuilds (our edits touch only `TerminalApp`)
   are quick thanks to MSBuild's up-to-date check. Reference times on this box, **with the
   default appxsym-skip**: a typical code-change deploy rebuild is **~20–50s** (a 1-TU change
   that doesn't touch a hot header validated at **20.5s**); a wide recompile (a hot header like
   `ProcessInspect.h`, included by dozens of TUs) is bounded by `CL` (~15s) + `Link`/`Lib`
   (~16s). (Historical, *with* the ~156s symbol-package zip that the wrapper now skips:
   first ~236s, code-change rebuilds ~165–290s.) The remaining variable cost is **header
   fan-out** — tightening a hot engine header shrinks the recompile set.

5. **Optional — MSBuildCache** for clean-rebuild / branch-switch cache hits: add
   `-p:MsBuildCacheEnabled=true` (uses file copies, not hardlinks).

6. **Compile-check without relinking the exe.** `TerminalAppLib` is a **static lib**, so you
   can validate code changes (and catch all our compile errors) while the app is still
   running — build just the lib (after `vcvars64.bat`):
   ```
   msbuild src\cascadia\TerminalApp\TerminalAppLib.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\
   ```
   `/p:SolutionDir=` (trailing `\`) is **required** when building a `.vcxproj` directly —
   otherwise `$(SolutionDir)build\rules\*.targets` imports fail (MSB4019). The full exe link
   is `msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /p:AppxSymbolPackageEnabled=false /t:Terminal\CascadiaPackage`
   (**~20–50s with the appxsym-skip flag**; without it ~3–3.5 min, dominated by the symbol-package
   zip — SolutionDir is implicit for the `.slnx`).

## Deploy & run

A packaged app can't be launched by running `WindowsTerminal.exe` directly (WT #926/#4043);
it must be deployed. Deploy the **loose layout** (what VS F5 does) — no signing/cert/admin:

```powershell
# one-time per machine (or after the manifest changes): register the loose layout
Add-AppxPackage -Register "K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion
```

Launch any of these ways (the loose layout registers the **DEV identity**, `AgentmasterDev` —
the release MSIX owns `Agentmaster`/`agentmaster`; see PROFILES.md):
- execution alias: **`agentmasterdev`**
- Start menu: **“Agentmaster Dev”**
- `Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"`

⚠️ **One-time migration on this machine** (first deploy after the identity split): the old loose
registration still owns `Agentmaster_56k4f06dsfp9r` (now the RELEASE family) and its on-disk
manifest no longer matches it — `Remove-AppxPackage Agentmaster_56k4f06dsfp9r`, then register as
above. First launch then shows the **profile picker** (current data lives in `~/.agentmaster`:
either Browse… to it, or pick Development + the migrate checkbox to copy it to
`~/.agentmaster-dev`). PROFILES.md §3 has the full recipe.

### Concurrency lock (multi-agent) — REQUIRED before any build, launch, or deploy

There is exactly ONE dev instance and ONE build output tree (`…\CascadiaPackage\bin\x64\Debug`),
so the close→build→relaunch cycle is **process-global and destructive**. Two actors running it at
once — two AI agents, or an agent + a human — collide: one closes the instance the other just
launched, two msbuilds race on the same outputs, and the **exe link fails because a running
`WindowsTerminal.exe` locks the very `WindowsTerminal.exe` being relinked**. **REQUIREMENT: hold
the global `build-launch` mutex for the WHOLE cycle before you build (full exe), launch, deploy,
OR close our instance.** (A lib-only compile-check — Building FAST #6 — doesn't relink the running
exe, so it needs no lock.) The mutex is a filesystem lock (`tools/am-lock.sh`, built on `mkdir(2)`
atomicity) under `%USERPROFILE%\.agentmaster\locks\`, so it **persists across separate Bash calls**
(each Bash tool call is a fresh shell) and is shared by every agent.

```bash
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
# ... entire close → build → relaunch cycle while holding $TOKEN ...
bash tools/am-lock.sh release --token "$TOKEN"
```
`acquire` prints the token to **stdout** (capture it) and logs to stderr; `--wait 600` **queues**
behind another agent's build (picking up within ~1s of its release) instead of failing. Without
`--wait`, a contended `acquire` exits **3 (`BUSY`)** with the holder printed — back off, don't
spin. A crashed holder can't wedge it: a lock past its TTL (default 30 min) is auto-broken on the
next `acquire`. Token-checked `release` means one agent can't drop another's lock. Helpers:
`status` (who holds it + age), `refresh --token T` (extend a long hold), `acquire --force` (steal
now), `with --wait 600 -- <cmd>` (acquire → run one command → release). `bash tools/am-lock.sh --help`.

**Inner loop.** The loose layout is live (binaries update in place), but you **cannot
relink `WindowsTerminal.exe` while the app is running** — it locks the exe. So: close *our*
dev instance (spare the Store WT), rebuild, relaunch. **Building, deploying, and installing
require the user's permission — always ask first** (see *Development Rules*; the prior "always
auto deploy, run without prompting" standing authorization is **revoked**). Just never touch the
Store WT (it's not under our path — see Gotchas). **Hold the `build-launch`
mutex around the whole cycle** (acquire at step 0, release at step 4 — see *Concurrency lock*).
```bash
# 0. acquire the global mutex — REQUIRED (queues behind another agent's build)
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
```
```powershell
# 1. close ONLY our dev instance (path filter spares the Store WT — see Gotchas)
Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" |
  ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | % { Stop-Process -Id $_.ProcessId -Force }
# 2. build (full exe link; the wrapper skips the ~156s appxsym by default — see Building FAST)
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore      # or: msbuild OpenConsole.slnx /t:Terminal\CascadiaPackage /m /p:Configuration=Debug /p:Platform=x64 /p:AppxSymbolPackageEnabled=false
# 3. relaunch
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"   # or: agentmasterdev
```
```bash
# 4. release the mutex (always — even if a step above failed)
bash tools/am-lock.sh release --token "$TOKEN"
```
Re-register **only** when `Package-Dev.appxmanifest` changes. (VS F5 on `CascadiaPackage`
also builds + deploys.) Runtime/session state lives in the dev install's **profile** (default
`%USERPROFILE%\.agentmaster-dev\`, or wherever the picker pointed it — check
`%USERPROFILE%\.agentmaster.profiles`); tail `hooks.log` there to confirm the engine is live
(`[engine] bridge listening …`) and that spawned sessions' hooks arrive (`[SessionStart]`,
`[Stop]`, …).

## Releasing a public version

**Only when the user agrees / requests it.** Cutting a release builds, self-signs, and publishes
a GitHub Release — it is outward-facing, so confirm the **version** with the user first and don't
do it on your own initiative. (Local dev deploys via *Deploy & run* above; this is the public path.)

Two GitHub Actions workflows are the repo's only CI:
- **`.github/workflows/ci.yml`** — build-only gate on `pull_request` + push to `agentmaster`/`main`
  (+ `workflow_dispatch`): restores vcpkg + nuget (cached) and builds `Terminal\CascadiaPackage`
  x64 Release. No packaging / signing / release. **No path filter**, so a docs-only push still
  triggers a full ~20-min build (add `paths-ignore: ['**.md','doc/**','LICENSE']` if that matters).
- **`.github/workflows/release.yml`** — the public-release pipeline: triggers on a **tag `v*`**
  push OR **`workflow_dispatch`** (a `version` input). No scheduled cron, no Azure, no NuGet
  *publish* (NuGet *restore* stays — it's a build dependency). A **NIGHTLY release** is still cut
  by tag/dispatch: tag `vX.Y.Z-prerelease-nightly` (any tag containing `nightly`; bump `X.Y.Z` —
  the updater compares numerically) — prep strips each version component to digits so the MSIX
  `Identity Version` stays numeric while the tag/release name/installer `-Version` keep the full
  suffix (`rawver`), and the release is **auto-marked prerelease** with a ⚠ NIGHTLY warning in its
  body; the in-app updater offers it ONLY to users who accepted the cog's warning-gated nightly
  opt-in (see the *In-app auto-updater* Status block), and `Install-Agentmaster.ps1` mirrors the
  tiers (`-Nightly`; `-Prerelease` alone never installs a nightly).

**To cut a release (happy path):**
```bash
# agree X.Y.Z with the user, then either tag-push (real release) ...
git tag vX.Y.Z && git push origin vX.Y.Z          # auto-triggers release.yml
# ... or dispatch a DRAFT dry-run without creating a tag:
gh workflow run release.yml --repo Nucs/Agentmaster -f version=X.Y.Z
# find + watch the run (background it; it pings on exit; --exit-status => failure is non-zero):
gh run list --repo Nucs/Agentmaster --workflow release.yml --limit 3
gh run watch <run-id> --repo Nucs/Agentmaster --exit-status
```
The pipeline is **prep** (version from the tag/input, stamped into `Package-Rel.appxmanifest`'s
`Identity Version`) → **build** matrix **x64 + arm64** (Release, `WindowsTerminalBranding=Dev` +
**`/p:AgentmasterPackageIdentity=Release`** = the `Agentmaster` RELEASE identity — never the dev
`AgentmasterDev` manifest, `AppxPackageSigningEnabled=false`) → **bundle**
(`build/scripts/Create-AppxBundle.ps1` merges both arches → `.msixbundle`, then self-signs;
`New-UnpackagedTerminalDistribution.ps1 -PortableMode:$true` makes the portable zips — TRUE
portable: `.portable` marker ⇒ WT settings in `<unzip>\settings`, the Agentmaster profile in
`<unzip>\profile`) → **release** (`softprops/action-gh-release` creates a **DRAFT** GitHub Release
with the `.msixbundle`, `Agentmaster.cer`, and both portable `.zip`s). It lands as a **draft** —
review the assets, then **the user publishes it** (a draft creates no git tag until published; a
`workflow_dispatch` draft is safe to delete). After publishing, refresh the release notes + README
(capabilities / Download) if the feature set moved.

**Signing.** The release self-signs with a **`CN=Agentmaster`** code-signing cert minted at runtime
whose subject is **read from the manifest**, so it always equals the Publisher (this is *why* the
Publisher is `CN=Agentmaster` — see Repo facts). Being self-signed, users must trust
`Agentmaster.cer` once to install the `.msixbundle`; the **portable `.zip` needs no cert** (unzip +
run `WindowsTerminal.exe` — fully self-contained: settings + profile live inside the unzip dir).
To sign with a real cert instead, set repo secrets **`SIGNING_PFX_BASE64`**
+ **`SIGNING_PFX_PASSWORD`** (its subject must still equal the Publisher).

**One-time repo settings** (already done for `Nucs/Agentmaster`; a fresh fork needs them — Actions
is **OFF by default on forks**, and the release job needs write to create the Release):
```bash
gh api -X PUT repos/<owner>/<repo>/actions/permissions -F enabled=true -f allowed_actions=all
gh api -X PUT repos/<owner>/<repo>/actions/permissions/workflow -f default_workflow_permissions=write
```
Verified end-to-end on a stock `windows-2022` runner (UWP workload + Windows SDK + vcpkg all
present): `v0.1.0` ran green — CI ~21 min, release ~23 min (x64+arm64 in parallel). On failure the
build **binlog uploads as an artifact** to diagnose the first run.

## Gotchas (learned the hard way)

- **`nuget.exe` can't parse `.slnx`.** The bundled `dep\nuget\nuget.exe` errors with
  "file type was not recognized" on `OpenConsole.slnx`, and a bare `packages.config`
  restore needs `-PackagesDirectory`. The wrapper restores `dep\nuget\packages.config`
  into `packages\` and is non-fatal; use `-NoRestore` once packages exist. (Stock
  `Invoke-OpenConsoleBuild` has the same latent bug — it only "works" because VS already
  restored.)
- **`Grid`/`Panel` has no `Focus(FocusState)`** in this XAML projection — only
  `Control`-derived types do (this caused error C2039). `IPaneContent::Focus` must focus a
  `Control` child; `AgentManagerContent::Focus` focuses its cwd `TextBox`.
- **`TextBox` has no `VerticalScrollBarVisibility`** in this projection (WPF puts it on the
  TextBox; UWP doesn't) — set the **attached** `ScrollViewer.VerticalScrollBarVisibility`
  instead (`ScrollViewer::SetVerticalScrollBarVisibility(box, …)`; also a C2039, hit on the
  multiline Auto-Testing compose box). Related glyph-alignment quirk: a bare symbol rides
  differently per element — a **`TextBlock`** reserves descent space in its line box (so a
  centered `"!"` sits high), while a **`FontIcon`** centers the glyph's ink. Render compose-bar
  symbols as `FontIcon` (even a text-font one, e.g. `Segoe UI` glyph `"!"`) so an icon row lines
  up; mixing `TextBlock` + `FontIcon` siblings misaligns them.
- **Building a `.vcxproj` directly needs `/p:SolutionDir=K:\source\Agentmaster\`** (trailing
  `\`), else `$(SolutionDir)build\rules\*.targets` imports fail with MSB4019. The `.slnx`
  build sets it implicitly. (See Building FAST #6.)
- **Imperative XAML name clashes:** a `using namespace winrt::Windows::UI;` pulls the nested
  `Text` namespace into scope and collides with a `Text(...)` helper (C2872/C2882) — prefer
  narrow `using`-declarations (`Color`/`ColorHelper`/`Colors`). The `.cpp` can't run-time
  test here, so the compiler is the safety net; build the lib (#6) after UI edits.
- **A text box inside a `ContentDialog` gets no keypresses in XAML Islands.** The dialog's
  PopupRoot sits outside our island's input path, so a hosted `TextBox`/`NumberBox` takes
  focus but receives no typing (first hit with the tree-rename box). So: use a **buttons-only**
  ContentDialog for confirms (`_OnDeleteSession`), and build anything that needs typing **into
  the main visual tree** instead — the in-place rename editor, and the Settings cog's
  **in-content modal overlay** (a dimmed `Grid` over `_root`, `RowSpan`-all; the card swallows
  taps via a handled `Tapped`, a backdrop tap = cancel). Don't reach for a ContentDialog when a
  field needs keyboard input.
- **A pinned tab's `CloseButtonVisibility::Never` is silently undone by
  `_updateAllTabCloseButtons()`.** Setting `Never` on a tab once (as `_OpenAgentManagerTab` does)
  is NOT enough: that method loops EVERY tab and re-applies the theme's *global* close-button
  setting (`tab.CloseButtonVisibility(theme…)` → `IsClosable`), and it runs on every theme/
  settings apply — which happens *after* the Manager tab is created — so the X reappeared. Fix:
  **skip `_managerTab` in that loop.** Note hiding the X (`IsClosable`) is independent of
  drag-reorder: a non-closable tab can still be dragged/torn out, so non-movable needs its own
  guard (`TabViewItem.CanDrag(false)` + `_PinManagerTabFirst()` snap-back + a `_TryMoveTab`
  refusal). Context-menu Close/Move is a third axis (`DisableCloseAndMoveMenuItems()`).
- **Never reuse the `WindowsTerminalDev` package identity.** It belongs to the separate
  `K:\source\windowsterminal` checkout; registering the same identity tries to *replace*
  it and fails with a file-in-use lock (`0x80073CF6 / 0x80070020`) when its
  `OpenConsoleProxy.dll` is loaded. Our distinct `Agentmaster` identity sidesteps this.
- **Upstream "wt identity" assumptions are package-BLIND and break under our rename** (commit
  `404c04f72`; per-identity since the profiles split). Several WT helpers hardcode the
  `wt.exe`/`WindowsTerminal` identity and silently
  misbehave for our packages: (1) **`GetWtExePath()`** (`WtExeUtils.h`) resolves
  `<PFN>\wt.exe`/`wtd.exe`, which **doesn't exist** (our manifests register the **`agentmaster.exe`**
  / **`agentmasterdev.exe`** execution aliases) — so every launcher built on it (`_OpenNewWindow`,
  the Reopen-Windows button, Jump
  List shortcuts) silently no-ops; (2) **`windowClassName`** (`WindowEmperor.cpp` — it seeds BOTH the
  single-instance **mutex** and the **window class**) is built from `WT_BRANDING` only, so our Debug
  "Windows Terminal Dev" branding **shared one single-instance identity with the real
  `WindowsTerminalDev`** (a launch could hand its commandline to the *other* window — defeating the
  whole point of the rename). Fix (both, package-aware): `GetWtExePath` picks the alias by PFN
  prefix — **`AgentmasterDev` FIRST, then `Agentmaster`** (the former contains the latter as a
  prefix; same ordering rule in `_AgentmasterReopenTarget`) —
  and `windowClassName` appends `GetCurrentPackageFamilyName()`
  for packaged builds. So coexistence (with `WindowsTerminalDev` AND between our own release/dev
  pair) needs distinct package identities (above)
  AND package-distinct **runtime** identifiers. Launch the alias by **name** (`agentmaster.exe` /
  `agentmasterdev.exe` — per-identity, so it can never activate the other install) — it
  resolves on PATH + follows the APPEXECLINK reparse and hands off; a full reparse-path
  `CreateProcess`/`Start-Process` bypasses the alias and cascades a fresh window instead
  (ShellExecuteEx on the full `<PFN>\<alias>` path is the verified exception — see `GetWtExePath`).
- **Closing instances to relink.** Closing **our** dev instance for the deploy inner loop
  **requires the user's permission first** (see *Development Rules*; the prior "always auto
  deploy, no prompt" authorization is **revoked**). Once permitted, filter by
  `ExecutablePath -like 'K:\source\Agentmaster\*'` (matches our `WindowsTerminal.exe` *and*
  its `OpenConsole.exe` ConPTY hosts) — but **scope it tighter** (e.g. `\bin\x64\Debug\`),
  because a **Release** instance can host the very session you're running in (`AM_SESSION` /
  `CCMGR_HOOK_PIPE` set), so the broad path filter would **self-kill** it — then `Stop-Process`,
  build, relaunch. But **never** touch the running **Store** Windows Terminal — it's the user's live session
  (under `Program Files\WindowsApps\…`, not our path). Never blanket-`taskkill` by image
  name; always path-filter so the Store WT is spared.
- **Packaging (`PRI210 / 0x800704c8`) can fail to overwrite `resources.pri`.** The registered
  loose-layout package keeps `src\cascadia\CascadiaPackage\bin\x64\Debug\resources.pri`
  memory-mapped, so MakePri's final overwrite-move dies with `0x800704c8` (ERROR_USER_MAPPED_FILE)
  — and `handle64` shows *no* owning user process (it's a registered-package/kernel mapping). The
  exe/dll already linked by then (your code is in the fresh `TerminalApp.dll`, copied into the
  layout *before* this step), so it's purely the PRI. Fix: **delete that `resources.pri` and
  rebuild** — MakePri then *creates* it fresh instead of overwriting a mapped target. (The Defender
  exclusion in Building FAST #3 also reduces the transient-lock variant.)
- **MSIX virtualizes a packaged app's `%LOCALAPPDATA%`** to the package LocalCache, but the
  spawned **`claude.exe` is external** and resolves paths against the real filesystem. So
  the hooks files + `--settings` path **must** live somewhere un-virtualized that both
  agree on — Agentmaster anchors the **profile defaults under `%USERPROFILE%`**
  (`AgentmasterStateDir()` == the ACTIVE PROFILE: default `~\.agentmaster`, dev package
  `~\.agentmaster-dev`; a Browse…-picked profile inherits this constraint — it must be a real
  filesystem folder, which `FOS_FORCEFILESYSTEM` enforces).
  Using `%LOCALAPPDATA%` here silently breaks hooks for spawned sessions (the app writes to
  LocalCache; Claude reads the empty real path). Verified live: with the fix, a spawned
  session's `SessionStart`/`UserPromptSubmit` reach the registry (`<profile>\hooks.log`).
- **`--dangerously-skip-permissions` also skips the startup "trust this folder" dialog.**
  Spawned sessions run with the flag by default (`AppSettings.skipPermissions`): besides
  auto-accepting tool prompts, permission mode `bypassPermissions` makes claude skip the
  per-folder trust dialog at startup (claude's block is gated on `mode !== "bypassPermissions"`),
  which would otherwise wedge an unattended ConPTY session waiting on a keypress. It does NOT
  suppress the one-time **global** "Bypass Permissions mode" acceptance (`~/.claude.json`
  `bypassPermissionsModeAccepted` — shown once, ever, until accepted). Toggling skipPermissions
  OFF drops the flag and instead pins `permissions.defaultMode:"default"` in the hooks-settings
  file (normal prompts + trust apply). The trust decision itself keys on the **git toplevel**
  of the cwd (forward-slash) under `~/.claude.json` `projects.<dir>.hasTrustDialogAccepted` — a
  trusted ancestor counts; accepting at your **home dir never persists** (so it re-prompts).
- **`claude --resume <id>` dies if there's no conversation.** A session that was opened but
  never prompted has no saved transcript; resuming it exits code 1 ("No conversation found")
  and the tab is dead. **Don't gate resume-vs-fresh on the persisted `SessionState`** — a
  just-resumed session reads `Idle` until its first new turn, so state is useless as a
  "was-it-used" signal. (Restore DOES seed a reopened session's DISPLAY state from the persisted
  one — the at-rest "needs you" states WaitingForInput/NeedsApproval survive a crash via
  `RestoredSessionState`, Rule #16 — but that is triage display, never the resume decision.)
  Gate on the transcript on disk:
  `ClaudeConversationExists(id)` globs `<CLAUDE_CONFIG_DIR | ~/.claude>/projects/*/<id>.jsonl`
  (ids are unique UUIDs, so no need to reproduce Claude's cwd→dir encoding). No transcript ⇒
  launch fresh (`[restore-fresh]` in `hooks.log`) instead of `--resume` (`[resume]`).
- **Native-exe-only policy: launch the native `claude.exe` by FULL PATH, gate everything else.**
  Launch/Restore hands a command line to ConPTY, which launches it via `CreateProcessW` — and
  `CreateProcessW` appends only `.exe` and **never consults `PATHEXT`**. So a bare `claude` token
  finds a native `claude.exe` (the `~/.local/bin` installer) but **silently misses the npm
  `claude.cmd`**, failing with `0x80070002` / `ERROR_FILE_NOT_FOUND` — *even though a hand-typed
  `claude` works* (a shell honors `PATHEXT` + the shim). It cost a friend's whole release ("my friend
  couldn't run it"): the dev had `claude.exe`, the friend had `claude.cmd`. **The fix is a deliberate
  policy, not a `.cmd` workaround:** Agentmaster is so `claude.exe`-coupled (the entire Fleet Observer
  + PEB enrichment key on the `claude.exe` image) that a pure-Node `claude` (`node.exe`, no native
  binary) is **unsupported**. `ResolveClaudeExe` resolves a *real `claude.exe`* — Settings override
  (must be `.exe`) → PATH `claude.exe` (exact leaf, never a `.cmd`) → `~/.local/bin\claude.exe` →
  **follow a `claude.cmd`/`.bat` to its npm native binary** (`<dir>\node_modules\@anthropic-ai\…\claude.exe`;
  the `.cmd` is a breadcrumb, never launched). Run once at engine init **before the PATH shim is
  prepended** (so a PATH `claude.cmd` resolves to the real binary, not our shim) → cached in
  `Engine::claudeExePath`; `hooks.log` prints `[engine] claude.exe: …`. Empty ⇒ `ClaudeAvailable()`
  false ⇒ the Manager gates launch/new/fork/resume behind the "Claude not detected" modal (Browse…
  via `IFileOpenDialog` + `claude install`), `TerminalPage::_LaunchClaudeSession` hard-refuses
  (`[launch-blocked]`) as the backstop for non-UI restore paths, and the Settings cog shows the
  detected path + an `.exe`-only override (`AppSettings::claudeExePath`, re-resolved live via
  `RefreshClaudeExe`). Because both npm and the native installer ship the SAME native `claude.exe`
  today (npm just delivers it via a per-platform optional dep), this gates only the legacy Node CLI.
  (Codex's `BuildCodexCommandline` still emits a bare `codex` — same latent bug, not yet fixed.)
- **A ConPTY connection's process spawns on the control's first non-zero layout — never
  eager-`Start()` it before the control initializes.** `TermControl::_InitializeTerminal`
  (gated on `SwapChainPanel().LayoutUpdated`) is what calls `_core.Connection().Start()`, so a
  background/unfocused tab's `claude.exe` doesn't launch until the tab is first shown (hence
  "Restore all" re-opens lazily). Trying to force a background session to run by calling
  `connection.Start()` early from the app layer **crashes the app**: the connection's output
  then reaches `ControlCore`'s output handler before `_core.Initialize()` (gated on that same
  layout) has run → **AV `0xc0000005` in `Microsoft.Terminal.Control.dll`**, on every startup
  restore. The safe way to start a background session is to let its control initialize first
  (select the tab) — which is *why* startup ARCHIVES instead of auto-launching (Rule #6): no
  startup tabs ⇒ nothing to lazily-not-start, and a user Restore opens one focused tab that
  initializes normally.
- **A parented `Popup` that OVERHANGS its parent's clip renders — but mutating it wrong fail-fasts twice
  (`0xC000027B`).** The bookmark-tag badges hang BELOW the tab's bottom edge by living in an always-open
  `Popup` parented into the tab header (a popup child draws in the island's POPUP ROOT, outside the tab
  strip ScrollViewer's clip — the only way in-tree-invisible pixels below the tab can paint). Two ways it
  killed the app, both diagnosed from the dumps (`tools/dumpstowed2` — the stowed backtraces named the
  exact frames): **(1) opening an UNROOTED popup throws** (`IsOpen(true)` → `E_UNEXPECTED`, no `XamlRoot`)
  — a restored tab asserts its badges (via a `TabStatus` property → PropertyChanged) BEFORE its header
  enters the visual tree, so the open had no root; gate every open on `HeaderRootGrid().IsLoaded()` +
  `.XamlRoot()` and re-arm on the header's `Loaded`. **(2) mutating the popup SYNCHRONOUSLY inside a
  layout-driven handler is a LAYOUT CYCLE** (`E_LAYOUTCYCLE 0x88000FA8`): the placement triggers are all
  layout passes (`SizeChanged` / `LayoutUpdated` / the strip `ScrollViewer.ViewChanged`), and writing
  `HorizontalOffset`/`IsOpen` re-invalidates layout → re-enters the pass; while the strip is still
  settling the mid-pass geometry reads shift each iteration so the "unchanged" gates never latch and XAML's
  ~250-iteration detector fail-fasts. Fix: never touch the popup from inside the pass — make the trigger a
  **coalescing scheduler** (a `bool` + one `Dispatcher().RunAsync`) and do the reads + gated mutations on
  the CLEAN tick after layout settles (the `_ApplyRenamerMaxWidth` cycle-safety lesson, again). Both
  guards + a `CATCH_LOG` on the whole popup tail now live in `TabHeaderControl::_PositionTagBadgesNow`.
- **MUX TabView drag-start null-deref (WinUI 2.8) — dragging ANY tab on a scrolled, overflowed strip
  crashes INSIDE Microsoft's DLL, before any event reaches us. ROOT CAUSE (dump-proven ×3 on release
  0.6.7.x, full-dump memory forensics): the dragged "item" WUX reports is the tab's CONTENT — WT's
  BODGY unique `Border` — which MUX can never resolve, so EVERY drag walks a null-unsafe fallback
  loop over ALL containers; any virtualized-out container before the dragged tab is an instant AV.**
  The chain: WUX `ListViewBase::GetDraggedItems` (ListViewBase_Partial_Reorder.cpp) puts
  `container.Content()` — NOT the container — into `DragItemsStarting.Items` for an
  items-are-their-own-containers list (WT's `TabItems` of `TabViewItem`s), and upstream WT plants a
  unique empty `Border` as every tab's Content (Tab.cpp `TabViewItem().Content(Border{})`, its "BODGY"
  disambiguator for exactly this MUX lookup). MUX's `TabView::OnListViewDragItemsStarting →
  FindTabViewItemFromDragItem` (TabView.cpp:823) then misses both fast paths —
  `ContainerFromItem(border)` (a Border is no item of TabItems) and
  `VisualTreeHelper::GetParent(border)` (the Content is never rendered) — and falls into the loop
  `ContainerFromIndex(i).Content() == item` from index 0 with NO null check: the first
  virtualized-out container (index 0 = the pinned Manager tab, whenever the overflowed strip is
  scrolled) is `null.Content()` → `0xC0000005` at `Microsoft.UI.Xaml.dll+0xB605C` (+ the `0xc000041d`
  death-rattle at the same offset), raised BEFORE `TabDragStarting` reaches our handlers
  (unguardable at the event, uncatchable under /EHsc). Dump registers: `Rcx=0`, `Rsi=0x41/0x47` ==
  `TabItems().Size()` (65/71 tabs), `Rdi=0` == died at loop index 0; the dragged-item stack slot
  resolved to a WUX `DirectUI::Border` — so the original v0.6.7 "fork → immediately drag" crash was
  THIS all along (the fork appends+reveals the new tab far right, scrolling index 0 out), NOT an
  uncommitted item→container map, which is why the settle/CanDrag guards alone didn't stop the
  recurrence (three more crashes 2026-07-16, same offset, 0.6.7.0 + 0.6.7.1). **⚠ DEAD END (0.6.7.2,
  REMOVED — `_NeutralizeTabStripVirtualization()`): "make the strip non-virtualizing by swapping its
  `ItemsPanel` for a plain `StackPanel`" is NOT legal and made things far WORSE.** It fixed the drag AV
  but replaced it with a **deterministic startup crash on EVERY launch** — `0xC0000005` read of a
  `0xD0` field in `Windows.UI.Xaml.dll+0x591677`, a deferred **render** pass (0 of our frames on the
  faulting thread), pinned by full-dump forensics + the WER before/after (the drag AV `+0xB605C`
  vanished the instant the swap compiled into Release, replaced by `+0x591677` at startup). Root
  reason: a **system-XAML `ListView` (ListViewBase) REQUIRES its `ItemsPanel` to be a
  `ModernCollectionBasePanel`** — an `ItemsStackPanel` or `ItemsWrapGrid`. A plain `StackPanel` is a
  legal host only for a bare `ItemsControl`; inside a `ListView` the framework's internal panel casts
  return null and the render pass null-derefs. (The removed comment's claim "MUX has ZERO code
  dependency on `ItemsStackPanel`" missed that the dependency is the *system* `ListViewBase`, not MUX.)
  There is **NO legal non-virtualizing `ItemsPanel` for a `ListView`**, so de-virtualization is
  unreachable at our layer — don't retry it. **Current state: the drag AV is UNFIXED**, mitigated only
  by the belts below (which the root-cause analysis proves are *insufficient* — a by-CONTENT lookup no
  settling can map); the remaining real options are all product decisions (disable MUX tab-drag
  reorder/tear-out — `TabView.CanReorderTabs`/`CanDragTabs` — or replace the strip's reorder
  mechanism). The belts: **`_SettleTabStripLayout()`** — `UpdateLayout()` after EVERY `TabItems()`
  mutation (`_InitializeTab` / `_RemoveTab` / `_TryMoveTab` / `_PinManagerTabFirst` /
  `_TabDragCompleted`) so the pump never sees an unsettled strip (XAML dispatches queued input ahead of
  the pending layout pass) — plus **`_GuardTabDragUntilRegistered(tvi)`** — a (re)inserted tab stays
  `CanDrag(false)` until `ContainerFromItem` resolves it, re-enabled inline or on its `Loaded`; the
  Manager tab never re-enables (its non-movable contract, re-checked in the deferred path). **Don't add
  a `TabItems()` mutation without routing through these**; logs `[tabdrag-guard]` when the deferred path
  arms.
- **A per-element `ToolTip` on rebuilt elements is a process-killing LEAK under XAML Islands — never
  `ToolTipService.SetToolTip` at BUILD time on any Manager-rebuilt surface.** The framework-side
  registration `SetToolTip` creates is never torn down under islands (the same broken bookkeeping that
  makes its auto-dismiss unreliable — the reason `AgentTipHelpers.h` exists), so every tipped element a
  board/tree/page rebuild recreates stays PEGGED with its whole element tree. Measured in the prod
  instance 2026-07-06: ~100 tips/refresh × a registry-notify rebuild cadence × 2 windows × 36 h ≈
  **7.5 M live tip clusters ≈ 68 GB of committed heap** → system commit exhausted (WER event 2004 named
  the process at 72.8 GB) → XAML's composition pipeline took fatal allocation failures and latched dead →
  the "window pumps messages but never repaints and reacts to nothing" freeze (`IsHungAppWindow` stays
  FALSE — the app logic, hooks, autorunner all keep running; only pixels stop). The heap census pinned it
  conclusively: the two `AgentSetTip` handler-delegate vtables + its `make_shared` control block at
  ~1:1:1 × millions, plus ~1.5 B `Windows.UI.Xaml.dll` object refs. `AgentTipHelpers.h` therefore keeps
  ALL tip state in ONE per-UI-thread host (one shared `ToolTip`, one open timer, a while-open watchdog);
  per element only two attached DPs + three CAPTURE-LESS handlers, and the service is touched only for
  the duration of a real hover-open (`SetToolTip(owner, tip)` right before `IsOpen(true)`,
  `SetToolTip(owner, nullptr)` on close — bounded by human hovers, not rebuild churn). Don't reintroduce
  per-element `ToolTip` objects, per-element capturing handlers, or build-time `SetToolTip` — and treat
  any per-element allocation on a per-refresh-rebuilt surface as a leak until proven torn down.
- **Working-dir comparison must be filesystem-aware** (else the Explorer Tree forks one dir
  into multiple roots). Windows is case-INsensitive (`C:\…\Desktop` == `…\desktop`) and
  treats `/`≡`\`; POSIX is case-SENSITIVE with `\` a literal char. Route every dir
  grouping/scope/match through `PathEq`/`NormPath` (`#ifdef _WIN32` → `CompareStringOrdinal`
  ignoreCase; `#else` → exact) — never raw `==`/`find`.
- **The hook wire's `prompt` field must be escaped — and the PowerShell escape must mirror
  `WireEscape` byte-for-byte.** The forwarder appends the `UserPromptSubmit` prompt as the
  trailing TAB-separated wire field; a real prompt has embedded TAB/newline, which would break
  the single-line, TAB-split, newline-framed record. Both the PowerShell forwarder and
  `BuildWireLine` escape it identically — `\ → \\` FIRST, then tab/CR/LF → `\t \r \n` — and the
  bridge `WireUnescape`s it. The PowerShell side is the one seam the C++ tests can't cover, so
  verify it with a parity check (`C:\src` → `C:\\src`, `https://` survives, no raw newline/tab
  remains). PowerShell gotcha: `-replace '\\','\\'` (pattern is regex = one backslash;
  replacement is literal = two) does the backslash-doubling, and it MUST run before the
  tab/CR/LF replacements or it would double the backslashes they introduce.
- **Path-picker Popup placement.** Parent the `Primitives::Popup` into the content root
  (top/left aligned) so its `Horizontal/VerticalOffset` is root-relative; the popup's own
  layout anchor already carries the root's offset within the XAML island, so the two compose
  to the box's on-screen position. Open it only on `FocusState::Pointer`/`Keyboard` (not
  `Programmatic`) so it doesn't pop on tab activation, and keep it open across row clicks via
  a **deferred** `LostFocus` check (re-focus the box on pick; bail if it regained focus).
- **Erasing a map node invalidates references to its key — copy the key first.** `_BindClaudeSession-
  ToTab`'s re-home block did `for (const auto& [oldId, w] : _claudeTabs) { … _claudeTabs.erase(oldId);
  _claudeOverlays.erase(oldId); }` — `oldId` is a reference INTO the `_claudeTabs` node, so the first
  erase freed it and the second `erase(oldId)` hashed **freed memory** → AV `0xc0000005` in
  `std::_Fnv1a_append_bytes` (`std::hash<wstring>`). Latent for ages (re-home was rare); the Fleet
  Observer's far more frequent correlation/re-home **exposed it**. Fix: `const std::wstring superseded
  = oldId;` BEFORE any erase. (General rule: never use a `[key, val]` structured-binding ref after
  erasing that element.)
- **A `+`-tab claude does NOT inherit our runtime env** (`AM_SESSION` / `CCMGR_HOOK_PIPE` / the PATH
  shim are all absent), so a hand-typed `claude` there fires **zero hooks** and is classified `wt`
  by `AM_SESSION` alone. WT regenerates a tab's child env from the **registry** (`til::env::regenerate`)
  unless `reloadEnvironmentVariables` is genuinely OFF, which drops these runtime-only vars — so the
  "Adopt any `claude`" hook fast-path is **degraded for `+` tabs** (a Manager-Launched session is fine:
  it sets env explicitly via `spec.env`). This is **why the observer keys binding on the roster
  correlation, not `AM_SESSION`** (Rule #13): a claude under one of our tabs' shells is ours
  regardless. The observer + the transcript-tail reconciler give it full detection + state without
  hooks; restoring the hook fast-path for `+` tabs (fixing the env regeneration) is a separate,
  non-blocking follow-up.
- **Symbolize a crash without cdb/WinDbg via the in-box DIA SDK + DbgHelp.** When the app AVs, the
  WER **Application Error** event already gives `Faulting module` + `Exception code` + `Fault offset`
  (the RVA), and a full dump lands in `%LOCALAPPDATA%\CrashDumps\`. No debugger is installed, but
  `msdia140.dll` + the **DIA SDK** ship with VS 2022 (`…\DIA SDK\include\dia2.h`, `…\lib\amd64\
  diaguids.lib`). A ~60-line C++ tool can `NoRegCoCreate(msdia140.dll)` → `loadDataFromPdb(TerminalApp.pdb)`
  → `findSymbolByRVA(rva, SymTagFunction)` + `findLinesByRVA` to map the fault offset to **function +
  source line** (link `diaguids.lib ole32 oleaut32 advapi32`). For the full call chain, `DbgHelp`'s
  `MiniDumpReadDumpStream` (ExceptionStream → faulting thread id; ThreadListStream → its stack memory;
  ModuleListStream → `TerminalApp.dll` base/size) + scanning the stack for in-module return addresses,
  symbolized through the same DIA session, reconstructs the stack top-down. This is how the re-home
  use-after-free above was pinned exactly.
- **A fire_and_forget coroutine can drop the LAST page ref on a POOL thread, destructing the whole UI
  tree off the UI thread — where any UI-affine call in a destructor becomes `std::terminate`.** The
  2026-07-18 release crash (`0xC0000409`, `ucrtbase+0xA527E`, `ExceptionInformation[0]==7` ==
  `FAST_FAIL_FATAL_APP_EXIT`): closing a window while the ~2 s `_ObserverProbe` tick was in flight holding
  `get_strong()` meant the **coroutine frame's destruction on the threadpool thread** released the final
  reference, so `~TerminalPage → ~Tab → ~Pane → ~AgentManagerContent` all ran on that thread, and
  `DispatcherTimer.Stop()` (UI-thread-affine) threw `RPC_E_WRONG_THREAD` **inside a noexcept destructor** →
  terminate → the WHOLE process (all windows — one WindowEmperor process) died from ONE window closing.
  Note `~TerminalPage` itself was ALREADY guarded for exactly this mechanism; the **destructor cascade it
  triggers** was not — a guard on the coroutine BODY (the 2026-07-01 net) does not cover the FRAME's
  destruction. So: (1) every destructor reachable from a page teardown wraps UI-affine calls in
  `try { … } CATCH_LOG();` — done in `~AgentManagerContent` (3 timer `Stop()`s) and
  `SafeDispatcherTimer::Destroy()` (the latent twin: `~Tab`'s `_bellIndicatorTimer`, which only escaped the
  live crash because that 4-second-old window had never shown a bell); (2) release/revoke lines stay
  OUTSIDE the try so refs always drop; (3) a thread-safe engine detach (`RemoveObserver`) stays BARE on
  purpose — it must never be skipped by a guarded throw; and (4) **every `fire_and_forget` needs a
  terminate-net** — an escaped exception there IS `winrt::terminate`, the same `0xC0000409` (16 lanes
  audited; the pattern for one needing `co_await` is to split the body into an awaited `IAsyncAction`, as
  `_AdoptExternalSession`/`_AdoptExternalSessionImpl` and `_SweepClaudeLiveness` do). A catch that re-arms
  a latch (`_claudeMissingPromptShowing`, `_sessionsIndexing`) must keep doing so, or the feature is dead
  for the window's life. Not-yet-done hardening: ending each lane with a best-effort hop back to the
  dispatcher so the last ref normally releases on the UI thread.
- **⚠ The exception-forensics logging path must never log its own failures — 9 `catch (...)` sites in
  `ClaudeSpawn.cpp` are DELIBERATELY BARE.** The *never lose a swallowed exception* policy (Rule #18) says
  every `catch (...)` reports — with exactly one class of exemption, and it is a hard constraint, not a
  judgement call. The chain is `LogSwallowedException → LogSwallowedExceptionCore →
  ExcLogThrottleAllow / CaptureRecentThrowStacksText → FormatAddressModuleRva → AppendStateLog →
  AgentmasterStateDir`, so logging from any link re-enters it. Two links are worse than unbounded
  recursion: **`AgentmasterStateDir` is called BY `AppendStateLog`** to build its path (throw → log →
  throw → …), and **`AppendStateLog` holds a NON-RECURSIVE `std::mutex`**, so logging its own failure
  **self-deadlocks** before it could even recurse — i.e. "completing" the sweep here wedges the app on the
  first failed log. Each of the nine carries an explicit `⚠ DELIBERATELY BARE` marker naming its
  degradation, under one authoritative `LOGGING-PATH RECURSION RULE` block listing them all. (The
  classifier's terminal `catch (...)` — which RECORDS `"unknown exception"` — is explicitly *not* one of
  them, and is annotated so it isn't "fixed" either.)
- **A managed session's conversation id can DIVERGE from its launch id — attribute state + bind by
  Claude's CURRENT conversation, never the pinned launch id.** `/resume` (into another
  conversation), `/clear`, and `/compact` all switch a live claude's active `session_id`, while the
  id we launched it with stays **pinned** on the process **cmdline** (`--session-id`) AND in the
  **`CCMGR_SESSION_ID`** env. Two places trusted the launch id and stranded the session — confirmed
  live: a claude launched `--session-id A` was running conversation `B` (per its OWN presence
  heartbeat `sessions/<pid>.json` + the `B` transcript), with every hook mis-filed under `A` and
  **no transcript under `A`**. (1) The **hook forwarder** resolved the id ENV-first, so every
  post-divergence hook fed the dead `A` record — which owns no transcript, so the SessionScanner's
  missed-`Stop` reconciler (it reads a terminal `stop_reason` from the transcript tail) could never
  release it → stuck `NeedsApproval`, then `Idle` while `B` ran (the `/compact` `SessionStart` reset
  it to Idle and the auto-continuation fires no `UserPromptSubmit`). (2) The **Fleet Observer** bound
  the tab via the cmdline `--session-id` (`ResolveObservedId`), so the tab-strip dot showed `A`'s
  phantom state, not `B`. **Fix:** the forwarder is **payload-first** (the hook `session_id` is
  Claude's current conversation; the env is only the fallback), and the observer prefers Claude's
  **own presence heartbeat** (pid-keyed, `startedAt`-validated against PID reuse) over the cmdline id
  when they disagree — matching the `agentmaster` CLI's bind precedence (HOOKS.md *Session
  correlation*, OBSERVER.md §8b). Both are no-ops until an actual divergence (a fresh spawn's first
  conversation has payload id == `--session-id` == the env). **Latent corollaries (not fixed):** the
  scanner can't reconcile a LIVE session with **no transcript at all** (`ShouldSynthesizeStop` needs
  a tail — a `live + active-state + zero-transcript + quiescent → release` safety net would
  self-heal one); and `/resume`-ing an already-open conversation into a managed tab is **two writers**
  on one transcript (the adopt-external hazard, now reachable manually).
- **A `--fork-session` fork echoes its SOURCE id on the FIRST SessionStart, not the new id — don't
  re-home the fork's tab onto it.** `claude --resume <src> --fork-session --session-id <new>` (the
  Sessions-page / duplicate-tab / adopt-external fork) writes a NEW transcript `<new>` and every steady
  hook lands on `<new>` — BUT its **first `SessionStart` hook fires under the SOURCE id `<src>`** (claude
  is "resuming `<src>`" at that instant, before it forks). Confirmed live: a fork minted `45f96288` (the
  id we registered + bound at launch) fired `[SessionStart] ec794664` (its fork SOURCE), which the
  registry adopted as an unknown session and `_BindClaudeSessionToTab` mistook for an in-session
  `/resume` → **`[rehome] 45f96288 -> ec794664`**, re-homing the fork's tab onto the inactive source.
  The real fork (`45f96288`, still getting every later hook + observer enrichment) was orphaned: the
  tab/overlay/state dot tracked `ec794664` (no observer pid → its copy→Launch-CLI synthesized a plain
  `--resume ec794664`, the symptom that surfaced this — it didn't match the actual `45f96288`). **Fix:**
  a fork records `SessionInfo::forkParentId = <src>` (**persisted** — see the never-messaged-fork note
  below) AND its ConPTY's `tabToken` is stamped
  **eagerly at launch** (not lazily on the first hook — the echo arrives BEFORE any own-id hook).
  `SessionRegistry::OnHookEvent` then IGNORES a `SessionStart` for `<src>` whose `tabToken` matches a
  live fork carrying `forkParentId == <src>` (`[fork-echo] ignored …`); `_BindClaudeSessionToTab` has the
  same backstop. The guard is **one-shot** — cleared on the fork's first own-id hook — so a LATER
  deliberate `/resume <src>` in the fork's tab still re-homes normally. (Mirrors the observer's id
  resolution, which already prefers `--session-id` over `--resume`; the push side needed the same.)
- **A `--fork-session` fork writes NO transcript until its FIRST turn — so a never-messaged fork must be
  RE-FORKED (not restore-freshed) on reopen, or its branch is lost.** Empirically (live, both profiles):
  forking `claude --resume <src> --fork-session --session-id <new>` does **not** create `<new>.jsonl`
  until the fork's first message — a fork the user created but never prompted has the SOURCE transcript
  on disk and **none** for `<new>`. The restore path is transcript-gated (`ClaudeConversationExists`),
  so on every window restart such a fork failed `wantResume`, fell to **`[restore-fresh]`**, and came
  back as a brand-new EMPTY conversation with the same `"(fork)"` title but a **churned id** — the
  forked context silently gone (observed as one `"(fork)"` title hitting `[restore-fresh]` dozens of
  times across restarts). **Fix:** `SessionInfo::forkParentId` is now **persisted** (Persistence.cpp
  `ToJson`/`SessionFromJson`, omitted when empty), and `_LaunchClaudeSession` detects the case — when a
  restored record has `!wantResume` (the fork's own transcript is absent) AND a `forkParentId` whose
  source transcript still exists, it **re-forks from the source into the SAME id** (`[restore->refork]`):
  `BuildClaudeSpawn`'s new `forkIntoSessionId` param forks back into the fork's existing id instead of
  minting, so the fork keeps its identity (and its `WindowRecord` tab ref) across restarts. Gated on
  `!wantResume`, so a fork that LATER got its own transcript is always resumed, never re-forked off a
  now-divergent source (the registry's one-shot first-own-hook clear also wipes `forkParentId` the
  moment the fork produces content, persisted on the next change); if the source ALSO vanished, it
  falls through to a fresh launch. The re-fork's source-id echo is handled by the existing guard above
  (the fork is live with `forkParentId == <src>` + the eager `tabToken`). Codex has the SAME class of
  issue (a never-prompted Codex fork has no rollout) — **not yet fixed** (its source-rollout uuid isn't
  persisted; tracked as a follow-up).
- **Tab title — the mechanism + its traps (the MECHANISM behind Rule #11).** A tab carries TWO title
  channels: the user-rename **override** `Tab::_runtimeTabText`, and the active control's
  OSC/profile/`StartingTitle` title. `Tab::Title()`/`_GetActiveTitle()` returns the override when set,
  else the control title — but **`Tab::GetTabText()` returns ONLY the override** (the one fact the two
  title sync directions hinge on; missing it is easy — even a careful audit mis-read it as "the OSC
  title leaks"). A managed Claude/Codex tab **pins** the override (`SetTabText`), so claude's volatile
  OSC title is hidden AND can never reach the registry: the reverse mirror `_SyncClaudeTitleFromTab`
  reads `GetTabText()` (the override), while the OSC `TitleChanged` handler only calls `UpdateTitle()`
  (sets `Title()`, never the override). An UNBOUND `+`-tab claude has an EMPTY override, so it floats
  with the OSC title until the observer binds it (`_BindClaudeSessionToTab` then pins the
  managed/derived name; a name the user already gave the `+` tab wins). **Every registry→tab pin MUST
  go through `_SetClaudeTabTextPinned`** (the `_pinningClaudeTabTitle` latch): `SetTabText`
  synchronously raises `PropertyChanged("Title")` → `_UpdateTitle` → `_SyncClaudeTitleFromTab`, so a raw
  pin would mirror our OWN write back into the registry — and an async observer that captured a
  now-stale title would write it back and the tab⇄registry directions **ping-pong forever** (the
  observed `/clear` title-swap + `[Unknown]` flood). The latch is a plain bool, UI-thread-only and
  synchronous-safe (set → `SetTabText` → re-entry no-ops → cleared, one stack frame, `wil::scope_exit`);
  the cross-window re-pin (`_SyncClaudeTabTitleFromRegistry`) reads the title **FRESH** on the UI thread,
  never the captured snapshot. **Persistence is split by tab kind:** a managed tab persists as a
  sessionId **ref** in the `WindowRecord` (the title rides `sessions.json` — Option 1, `SessionInfo.title`);
  a shell tab persists its `_runtimeTabText` as a `RenameTab` action inside `actionsJson`
  (`BuildStartupActions`). The two are mutually exclusive (`_ClaudeSessionForTab` non-empty → ref, empty →
  Other), so a managed tab is never ALSO replayed as a RenameTab-bearing shell; on restore a managed tab
  re-pins from `SessionInfo.title`, a shell tab replays its `RenameTab`. **On-disk DISPLAY titles are a
  SEPARATE concept** — the Sessions browser / CLI / external-row enrichment derive one
  from the transcript via `PickDisplayTitle` (precedence `customTitle > aiTitle > summary > firstPrompt`);
  it is **display-only** and never written over a managed `SessionInfo.title` (resume/fork-from-disk seed
  a record's title only when the session is UNKNOWN to the registry — `_ResumeSessionFromDisk` /
  `_ForkSessionFromDisk` gate on `!existing`). `DeriveSessionTitle` (the cwd-derived default) is never
  empty, so a managed title can't go blank; renames are trimmed/rejected on BOTH entry points
  (`_CommitRename` and `_SyncClaudeTitleFromTab`).

## Correctness rules (do not regress)

1. **"Waiting-for-you" is three states.** A session is *ready* for an auto-send when it is
   **turn-complete** (`Stop` → `WaitingForInput`) **or** sitting **Idle** with no turn in
   progress (a freshly launched / just-resumed plan must START, not wait for a `Stop` it will
   never emit); `Running` / `NeedsApproval` / `Error` / `Done` are never ready. A change-driven
   advance is held to **one prompt per turn** by the pickup guard (don't regress that — it
   prevents a queue-drain). `Notification(permission)` → Approval Policy (NOT the prompt queue).
   A `Stop` whose last message is a question → **Held** by the question-guard.
2. **Tree `Enter` = Activate** (jump to the session's live tab); never forward it to
   `ITerminalConnection::WriteInput` (that would submit a stray carriage return).
3. **Bind queue → sessionId**, never "the selected session" at send time.
4. **Idempotent sends:** mark `Sent` atomically + persist; survive restart without replay.
5. **Backstops:** stop-on-error, maxAutoSends, global pause/kill, pause-on-human-input.
6. **Startup ARCHIVES, never auto-launches; restore = resume, not replay (transcript-gated).**
   On startup, persisted sessions load into the registry as **closed** (`live=false`) and are
   **NOT** re-launched — the app opens to just the Manager tab, and the prior fleet is resumable
   from the **Sessions browser** (a deliberate reversal of the old auto-reopen). A user-initiated
   **Resume** re-launches one: `claude --resume <id>` (same id ⇒ hooks still correlate) **only when
   Claude has a transcript for that id**, else a fresh session (new id, same dir + queue, stale
   record dropped). Queues reload with statuses intact. **Close is the only close verb — it always
   archives** (keeps the `live=false` record so the session stays resumable; it NEVER deletes, and
   there is no Delete — FAVORITES.md). The Claude transcript on disk is never touched; mark a session
   you want to keep with the **★ favorite** (SessionStore). Never decide **resume-vs-fresh** from the
   persisted `SessionState` (that is transcript-gated, above) — but the persisted state DOES seed a
   reopened session's DISPLAY state: its at-rest "needs you" triage (WaitingForInput / NeedsApproval)
   is PRESERVED across a crash while in-flight/ended states normalize to Idle (`RestoredSessionState`,
   Rule #16). See Gotchas.
7. **State is hook-derived,** never screen-scraped (the Ink TUI repaints constantly).
8. **Same directory = same path, filesystem-aware.** Group/scope/match sessions by working
   dir through `PathEq` (case-insensitive on Windows, case-sensitive on POSIX), so
   case-variant spellings of one directory never fork the Explorer Tree into two roots.
9. **Adopt only on `SessionStart`; bind by `tabToken`, never guess.** A hook for a session we
   didn't Launch creates an `external` record ONLY on `SessionStart` (any other event for an
   unknown id is ignored — there's no connection to bind). Promotion to control correlates the
   `WT_SESSION` `tabToken` to a live ConPTY and binds the injector to THAT id (Rule #3); if no
   connection matches (a claude hosted outside this app), it stays observe-only — never bind to
   "the active tab".
10. **One engine per process (M9); a closing window must detach; records reference, never copy.**
    Exactly ONE `SessionRegistry` / `HooksBridge` / `Scheduler` for the whole process
    (`SharedEngine()`), shared by every window — never re-create them per `TerminalPage`, and
    never key the bridge on anything that collides across windows (the `<pid>` pipe is fine
    *because* there is one bridge). Each window registers its lens observer + adoption handler +
    activate sink by **token** and detaches them on teardown (`RemoveObserver` in
    `~AgentManagerContent`; `RemoveAdoptionHandler` + `UnregisterWindowActivateHandler` in
    `~TerminalPage`); the fleet loads **process-once**
    under the `Engine::restoreMutex` barrier (a second window blocks until it's loaded, then skips) so it can't
    double-insert NOR race its tab re-home against a half-loaded registry. Per-window persisted UI
    state (geometry + lens + ordered tab refs) is the `WindowRecord` (`windows/<id>.json`) and it
    must **reference** sessions by id, never copy them — `sessions.json` + `SessionInfo.live`
    remain the one session source of truth (Option 1).
11. **A session's title is ONE value — Explorer name == tab title == persisted `SessionInfo.title`.**
    `s.title` is the single source of truth; it is **pinned** onto the WT tab at launch / restore /
    adopt (`Tab::SetTabText`, so the tab stops floating with claude's volatile OSC title). Renaming
    from **either** side writes that one value and persists (the autosave-on-change observer): the
    Explorer-tree **Rename…** routes through the `rename` callback → `_RenameClaudeSession` (registry
    + tab); a WT tab rename (double-click / right-click **Rename Tab** / `renameTab` action — all
    funnel through `Tab::SetTabText` → `PropertyChanged("Title")` → `_UpdateTitle`) flows back via
    `_SyncClaudeTitleFromTab`. The sync is **cross-window**: a rename run in a window that does NOT
    host the tab (the board/tree show the whole fleet) writes the registry, and the hosting
    window's registry observer re-pins its tab (`_SyncClaudeTabTitleFromRegistry`, riding the
    tab-dot push). Equality guards make an already-in-step sync a no-op (no loops); an
    emptied override (`ResetTabText`) re-pins. A WT-tab rename is **normalized like the Explorer
    editor** (`_CommitRename`): `_SyncClaudeTitleFromTab` trims surrounding whitespace/newlines and
    **rejects** a blank/whitespace-only rename (re-pins the managed name — a title never goes empty),
    and when the user typed surrounding whitespace it re-pins the tab to the trimmed value so the strip
    matches the stored name. It reads the `_runtimeTabText` override (`Tab::GetTabText()`), **never**
    the volatile active/OSC title (`_GetActiveTitle()`), so claude's OSC title can't leak into the
    registry. On **adopt**, a name the user already gave the `+` tab wins (mirrored into the registry);
    else the tab is pinned to the managed name. On an in-session **`/resume`** re-home (the tab's claude
    switches conversation id on a stable ConPTY — `_BindClaudeSessionToTab`'s `reHomedFromOtherId`
    latch), the new conversation keeps **ITS own** title (its registry title if any, else
    `DeriveSessionTitle`) and the tab is re-pinned to it — it **never** inherits the superseded
    (now-archived) conversation's still-pinned tab text, which would mask the new id under the old one's
    name. A **fork's** name comes from `DeriveForkTitle`: a first fork appends ` (fork)`, forking a fork
    **increments** (` (fork 2)`, ` (fork 3)`, … — multi-digit, nested/earlier parens preserved) instead
    of stacking ` (fork) (fork)`. Don't reintroduce a separate tab title or scrape claude's OSC title
    for the name.
12. **A tab's color is ONE value per COLOR KEY — PERMANENT.** The key is per-`tabColorMode` (the
    cog's **Tab coloring** dropdown): the **working dir** (default — the classic rule below, verbatim),
    the **session itself** (`Individual` — the color persists on `SessionInfo::tabColorHex`, never in
    the dir map, so Individual can't exhaust the folder palette; no fan-out), or the **inferred
    working dir** (`InferredWorkingDirectory` — the SAME dir machinery keyed by
    `SessionInfo::inferredWorkingDir` when known, else the cwd; `SessionColorKeyDir` is the one key
    resolver, `ResolveSessionColorHex` the one read-side resolution every display surface shares).
    **Across the dir-keyed modes a git WORKTREE keys its MAIN repo's color** (`SessionColorKeyDir` →
    `ResolveWorktreeMainRoot`, memoized): a repo and all its worktrees resolve to ONE color key, so
    they wear one color — the SINGLE place the color key diverges from `EffectiveWorkingDir` (which
    stays worktree-granular for grouping + mechanics: a new session still spawns in the worktree, the
    Explorer tree still lists it there); a main-checkout / submodule / non-git dir is unchanged.
    The fourth mode, **Remove colors** (`NoColor`), SUSPENDS painting rather than re-keying it,
    STRIP-WIDE: a managed tab is reset (re-derivable from the maps), every NON-managed tab's
    runtime color — ex-claude shell tabs, user-colored/restored pwsh tabs, the Manager tab — is
    PARKED on the tab (`Tab::SetTabColorSuspended`; persistence still records the parked value via
    `GetPersistableTabColor` / `BuildStartupActions`' fold), "Change tab color" +
    `openTabColorPicker` are disabled on EVERY tab (`Tab::SetColorPickerEnabled`), any color
    landing mid-mode is parked by the `_OnClaudeTabColorChanged` chokepoint, and the persisted maps
    are neither read nor WRITTEN (`ResolveSessionColorHex` ⇒ empty) — permanence upheld by
    abstinence: switching back to any colored mode restores exactly the prior colors.
    Every paint routes through `_ApplySessionTabColor` (the mode dispatch); everything below is the
    DEFAULT (dir-keyed) mode's contract, which the inferred mode inherits over its key. Every Claude
    tab in a dir
    shares one color (filesystem-aware key `NormDirKey` — slash/case/trailing-normalized), and a dir
    **keeps the same color across tabs, windows, and restarts** (the user gets used to a folder's
    color). `dir-colors.json` (schema **v2**) is the single source of truth — the PERMANENT
    folder→color map, holding BOTH user picks and auto-assigned colors (no in-memory cycle state).
    On launch/restore/adopt `_ApplyDirColorToTab` paints the dir's persisted color if it has one,
    else `AssignDirAutoColor` deals a fresh one and **persists it**. The deal (pure core
    `ChooseDirColor`): (0) an already-assigned dir keeps its color; (1) else the first color in the
    dir's seeded probe order (Fisher-Yates / splitmix64 over `(seed, key)`) that **no other folder
    already holds** — the "color collection" is just the palette colors present in the map, so two
    dirs never share a color while colors remain (the fix for the "two folders, same color" bug — a
    hash collision into one `% 14` slot in the old scheme); (2) when **every palette color is already
    assigned** ("the color list is over") the collection **resets** — reuse is allowed, but the deal
    still **avoids any color an OPEN tab is actively showing** (across all windows, via `openDirKeys`
    = the process-wide registry's live set); (3) only when every palette color is active (more open
    dirs than colors) is reuse unavoidable → the dir's preferred slot. The **seed** (Engine init →
    `SeedDirColors`, random) only randomizes a dir's **first-ever** assignment, which then persists —
    it never moves an assigned color. A user color change
    (`Tab::SetRuntimeTabColor`/`Reset` → `TabColorChanged` → `_OnClaudeTabColorChanged`) **persists
    it for the dir AND recolors every live tab in that dir** (Rule #8); a reset drops the entry. A
    single de-dupe (`GetDirColor(dir) == newHex`) makes our OWN writes no-ops — our launch-time auto
    paint AND a user-pick fan-out already match the persisted color, so only a genuine NEW user pick
    re-persists + re-fans (no loop). A one-time `MigrateDirColorsToV2IfNeeded` (Engine init)
    **de-collides** a v1 file (`DeCollideDirColors`): every folder keeps its color where possible,
    duplicate palette colors are reassigned to free ones, and off-palette user picks are kept
    verbatim — fixing the existing collisions without discarding the mapping. (The default *name*,
    `DeriveSessionTitle`, only seeds an *untitled* session — a real/renamed title wins, Rule #11.)
13. **Fleet Observer: rostered == ours; correlate by `WT_SESSION`, never guess; provenance: push
    wins for state.** A claude correlated to a tab in OUR published roster (its shell == the ConPTY
    root of one of our tabs) is running in one of our windows, so it is **OURS** (`RunningApp::
    Agentmaster`) and bindable **regardless of `AM_SESSION`** — a hand-typed `+`-tab claude does NOT
    reliably inherit `AM_SESSION` (WT regenerates a tab's env; see Gotchas), so the **roster
    correlation is the authoritative bind signal**, and `AM_SESSION` is only the secondary signal
    for the census of NON-rostered claudes (a background daemon we spawned, or telling our instance
    from another's / a real WindowsTerminal). Correlation keys on the **exact `WT_SESSION`** (==
    `ITerminalConnection::SessionId()`) — never PPID ancestry, never the stale tab cwd, never "the
    active tab". `ObserveClaude` **enriches facts but NEVER sets `SessionState`** — push hooks + the
    transcript tail own state (Rule #1/#7); a hooked and an un-hooked claude converge on the same
    record. The observer **only reads** (PEB / Toolhelp / filesystem) — it must NEVER write to a
    shell's stdin (the invisibility invariant). **Adopting** an external (the Explorer-Tree
    EXTERNAL scope's *Adopt*) upholds this — it never injects into / kills the foreign process; it
    resumes the external's *conversation* into a NEW managed tab (`claude --resume <id>`, id from
    `ResolveSessionId`, transcript-gated → fresh if none) and leaves the original running. The
    transient enrichment fields are **never persisted** (`Persistence.cpp`); the observer
    re-derives them each run.
14. **Conversation identity is by transcript CREATION time ≈ process start, not newest-mtime.** A
    claude's OWN transcript is created when it first writes — at/after its process start.
    `ResolveSessionId` (for a bare claude with no explicit `--session-id`/`--resume <guid>`) picks
    the cwd's transcript whose **creation time is closest to (and not significantly before) the
    claude's start**, and REJECTS transcripts created before it started. This is what keeps **two
    claudes sharing one cwd** bound to their OWN conversations (newest-mtime collapses them onto
    whichever is momentarily most active), and resolves a **never-prompted** claude to `""` (no card
    until its first prompt — §11d) rather than collapsing it onto a stale leftover transcript. An
    explicit `--session-id` / `--resume <guid>` is authoritative and wins (collision-free, known
    before the transcript exists).
15. **One profile per install, resolved ONCE, before ANY state read; everything persists inside
    it.** The WindowEmperor resolves the profile **after** winning the single-instance handoff and
    **before** the first settings/state read — first launch **auto-selects the per-identity default**
    (release → Production, dev → Development) WITHOUT UI, so a handed-off process or a `-Embedding`
    (defterm) activation takes the same silent path; the only profile picker left is the cog's
    explicit **Change…** (never shown from a handed-off / `-Embedding` process). Never read or
    write persisted state (engine files, Terminal settings, window records, the reopen scan)
    through any path that isn't `AgentmasterStateDir()` / the `AGENTMASTER_PROFILE`-redirected
    `GetBaseSettingsPath()`. The resolution is cached for the process lifetime — a profile change
    (the cog's Change…) applies on restart only; never re-home state mid-run. The ONLY data outside
    a profile: the `.agentmaster.profiles` choice file + the env override (the pointer must live
    outside what it points at) and the fixed `~/.agentmaster/locks/` build mutex (it guards the ONE
    build tree, not a profile). Library-side resolution (`ProfileBootstrap::ResolveProfileDir`)
    must never show UI — headless hosts (tests, tools) silently land on the per-identity default,
    which for unpackaged runs is the historical `~/.agentmaster` (don't regress that — it's what
    keeps the test harness and old tooling stable). Generated artifacts that embed paths or live
    endpoints (the shim, hooks-settings, the forwarder's `bridge.json` discovery) must be derived
    from the ACTIVE profile at engine init — a fixed `~/.agentmaster` literal in generated content
    is a cross-instance routing bug (the forwarder had exactly that).
16. **Durability: never lose a window, tab, or session unless the user CLOSED IT DELIBERATELY — and
    even then, demote to recoverable, never erase** (PERSISTENCE.md §13.5 / FAVORITES.md). The
    workspace survives **every** exit — graceful close, app quit, AND a hard crash. "Closed slowly"
    (deliberately, one window at a time) changes only whether a window is **auto-reopened** next
    launch vs. **kept recoverable on demand** — closing NEVER deletes anything. Three durable layers
    carry it, all under the ACTIVE profile (Rule #15): **`windows/<id>.json`** (one `WindowRecord`
    per window — geometry + Manager lens + ORDERED tab refs + focused-tab identity; Option 1 *refs*,
    never session copies), **`open-windows.json`** (the open-at-exit manifest = the live window-id
    set), and **`sessions.json`** (the fleet — only ever GROWS, never pruned). The **"closed slowly"
    distinction IS the manifest's SKIP-EMPTY rule** (`Engine::UnregisterLiveWindow`): each window
    unregister rewrites the manifest EXCEPT a write that would EMPTY it is skipped — so closing
    windows one-by-one prunes each (a deliberately-closed window won't auto-reopen) while the LAST
    close leaves the final snapshot, and a hard shutdown that kills the threads before they
    unregister leaves the FULL set (everything auto-reopens). A deliberately-closed window's
    `WindowRecord` STAYS on disk → it is still offered by the Manager's **"Reopen Windows (N)"**
    button (`RecoverableWindows` = records − live − content-less). On startup the Emperor reopens the
    manifest∩records set (`WindowEmperor.cpp`), gated by the **decide-prompt** when >1; **No** still
    loses nothing (one window claims the front record, the rest stay recoverable). **Session half**
    (FAVORITES.md): **Close ALWAYS ARCHIVES** (`live=false`, keep the record + Auto Testing +
    autorunner; the transcript `.jsonl` is NEVER touched) — there is no Delete; a closed session
    leaves the Board and is resumable from the **Sessions browser** (the sole history view), the
    recover button bringing back the WHOLE window. **Enforcement that must not regress:** the
    **deterministic close/quit flush** captures the record BEFORE teardown clears `_claudeTabs`
    (`CloseWindow` / `RequestQuit`, latched by `_windowRecordTeardownFlushed`) with an idempotent
    `~TerminalPage` catch-all for the non-initiating windows on a quit-all; the **load barrier**
    (`Engine::restoreMutex`) blocks a 2nd reopened window until the fleet is loaded so it can't flush
    an EMPTY record over its workspace; **anti-clobber** refuses to overwrite a good record with a
    pre-`Initialized` / no-tabs-AND-no-geometry capture; and the **reclaimable pool** re-claims a
    this-session-closed record BY ID (real id + lens), so a recover/reopen never mints a lens-less
    duplicate. **The ONLY two erasures**, both content-free by construction: a window emptied to just
    the pinned Manager tab self-closes + deletes its (content-less) record (`_CloseWindowIfManager-
    Only`, race-safe via `ReserveManagerOnlyClose` — the last window STAYS open, record discarded),
    and a session whose transcript VANISHED from disk simply stops listing ("not found → don't show";
    `cleanupPeriodDays` is seeded to ~never so Claude never sweeps). Never add a destructive close
    path, a manifest write that can legitimately empty the file, a capture that runs AFTER
    `_claudeTabs` is cleared, or a `sessions.json` prune that drops a record with a live transcript.
17. **A bookmark TAG is removed only EXPLICITLY, and its color/name resolution is single-source.** Untagging
    a tag's LAST carrier must NEVER delete the tag — it stays in the universe at ·0 (re-appliable) via the
    durable known-tag registry (`tags.json`), and only the ✕ on a 0-carrier row deletes it
    (`UnregisterKnownTag`). A tag any session still carries can NOT be deleted (the derived union keeps it
    alive), so the ✕ is shown only on 0-carrier rows. A deleted tag KEEPS its `tag-colors.json` entry (a
    re-created name regains its color). Creation is capped by `AppSettings::maxTags` (applying/removing an
    existing tag is never capped). Every surface resolves a tag's color through the ONE
    `ResolveTagDisplayColor` (user-picked `tag-colors.json` > `TagColorFor` name-hash), and the tab badges +
    tooltip read it from the resolved `AgentTagsSpec` (`name\t#AARRGGBB`) the producer wrote — never
    re-resolve per consumer, or the same tag can wear two colors.
18. **Never lose a swallowed exception.** Wherever we `catch (...)` — at ALL times, in ALL cases — the
    exception and its origin must survive into `hooks.log`, so any later report is diagnosable in full
    without a repro. A catch may still recover silently *for the user*; it may not be silent *for us*.
    Report through the ONE chokepoint: **`Agentmaster::LogSwallowedException(L"<context>")`** in the
    plain-C++ engine TUs (no WinRT — they must keep compiling in the standalone harness and the CLI) and
    **`AgentLogCaughtException(context)`** (`AgentCatchLog.h`) in the TerminalApp layer, which adds
    `hresult_error`/`wil` detail; both attach the VEH-captured **throw-site** stack as
    `Module.dll+0xRVA` (the catch site itself runs after the unwind, so the raise-time ring is the only
    place that stack still exists). **Preserve every recovery side-effect exactly** — a re-armed latch, a
    fallback return, a `continue`-skip — logging is purely additive; and never gate behavior on it (both
    reporters are `noexcept` and throttled, so they cannot turn a contained failure into a new one). Two
    exemptions, both narrow: a catch that is itself on the **logging path** MUST stay bare (it recurses /
    self-deadlocks — see Gotchas; mark it `⚠ DELIBERATELY BARE` with its degradation), and a catch whose
    throw is genuinely expected control flow in a hot loop may stay bare *with a comment saying so*.
    "It's just a best-effort helper" is NOT an exemption — those are exactly the ones that vanished
    (`OutputDebugStringW` doesn't count: it needs a debugger attached at the time, so in the field it is
    silence). Don't add a bare `catch (...)` to this codebase.

## Conventions

- Mark our additions with `Agentmaster`. The repo `LICENSE` is **AGPL-3.0-or-later**
  (the combined-work license; paid escape hatch in `LICENSE-COMMERCIAL.md`); the upstream
  Windows Terminal **MIT** license is retained in `LICENSE-MIT` and the third-party
  attributions in `NOTICE.md` (the build's `NOTICE.html` source) + `THIRD-PARTY-NOTICES.txt`
  — **never strip them** (MIT requires retaining the notice). External contributions require
  signing `CLA.md` before merge (the dual-license depends on it).
- Keep the diff against upstream minimal where practical (additive files, small touches at
  integration points) so rebasing onto `microsoft/terminal` stays cheap.
- Build artifacts (`bin/`, `packages/`, `Generated Files/`) are gitignored — never commit them.
- **The Agent Manager UI is ALWAYS dark, independent of the Windows / Windows Terminal theme.**
  Every Agentmaster surface root forces `RequestedTheme(ElementTheme::Dark)` — the Manager tab
  `_root` (`AgentManagerContent`), the full-window Sessions page host, and the settings /
  claude-missing / path-picker overlays — and paints an **explicit dark fill** (e.g. `_root` uses
  `#2e2e2e`, NOT an app-theme-resolved brush: `Application.Resources().Lookup("UnfocusedBorderBrush")`
  resolves against the *app* theme and returns light `#e8e8e8` in light mode, which used to bleed
  through the Manager pane's widget gaps). Confirm dialogs that **self-host** (`AgentManagerContent`,
  via `ShowAsync`) follow `_root.ActualTheme()` (Dark); dialogs shown via WT's **shared presenter**
  (`TerminalWindow::ShowDialog`, which force-themes *every* dialog to the WT setting up its whole
  ancestor chain) opt into dark by tagging themselves **`agentmaster-dark`** (the Archive/Sessions
  page + batch-close confirms do). **Popups** that render in the popup root inherit Dark from their
  now-dark anchor: `MenuFlyout` context menus (`_MakeSessionMenu` / `_MakeExternalTreeMenu` /
  `_MakePromptMenu` / the Copy submenu) and `ComboBox` dropdowns follow their **target element's**
  theme — the SAME mechanism WT uses for all its own flyouts (nothing in the tree sets
  `MenuFlyoutPresenterStyle`, and WT's per-app theming proves it), so menus/combos under `_root`
  need no per-flyout theming — and the path-picker `Popup` is explicitly Dark. The one exception is
  **`ToolTip`s**: `ToolTipService` theme inheritance is unreliable under XAML Islands (the reason
  `AgentTipHelpers` exists), so `AgentSetTip` pins every tip `RequestedTheme(Dark)`. Don't
  reintroduce an app-theme-dependent brush/lookup on these surfaces, tag any new presenter-shown
  Agentmaster dialog `agentmaster-dark`, and keep new tooltips going through `AgentSetTip`.
