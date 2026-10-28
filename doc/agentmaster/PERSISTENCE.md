# Agentmaster — Workspace Persistence & Restore (Design Plan)

> **Status:** SHIPPED & live-verified — the window layer is **COMPLETE** (see §13.5).
> Supersedes the flat session-only persistence shipped in **M8** (see `IMPLEMENTATION.md`).
> Scope: persist and restore the **whole workspace** — every window, its tabs (order,
> color, title), panes, window geometry, the per-window **Manager tab** state, and the
> live **Claude (and Codex) sessions** inside them — so first launch of the process reopens
> exactly the state the user left.
>
> This document keeps the full design narrative (§1–§12 are the agreed-and-built plan; §13
> is the as-built delivery record). The original **M9–M14** milestone ladder (§11) is
> **superseded** by §13's Increments 1–5, all of which shipped and were live-verified.

## 1. Why

Today persistence is **session-only and window-blind**: `_InitAgentmasterEngine()`
autosaves a single flat `sessions.json` on every registry change
(`TerminalPage.cpp:743`), and `_RestoreClaudeSessions()` re-launches that flat list on
startup. Two facts break this the moment there is more than one window:

- **One process, many windows.** v1.24 uses the **WindowEmperor** model: launching a
  second app process (`Agentmaster.exe`) hands its command line to the existing instance and exits
  (`WindowEmperor.cpp:482`); the Emperor hosts every window on its own thread. So all
  Agentmaster windows share one process — see `DESIGN.md` and the "two windows = one
  process" note.
- **The engine is per-window, keyed on the process.** `_InitAgentmasterEngine()` is a
  `TerminalPage` method → one registry + one `HooksBridge` **per window**, but the bridge
  pipe is `\\.\pipe\agentmaster.<pid>` (`TerminalPage.cpp:748`) — **same PID for every
  window**. Two windows ⇒ two registries racing on one pipe and clobbering one
  `sessions.json` (last-writer-wins). Multi-window is currently incoherent.

We also want richer-than-session restore (windows, geometry, tab order/color/title) and a
non-destructive close.

## 2. Goals / non-goals

**Goals**
- Persist the full tree: **windows → tabs (order, color, title) → panes → Claude
  sessions**, plus **window position + size + launch mode**, plus each window's **Manager
  tab lens state**.
- First launch of the process **archives** the saved workspace and **offers** to restore it
  (Rule #6: never auto-launch); restore resumes each Claude conversation per-window (§6/§6a).
- **Close is never destructive.** Closing a window or the app tears down the live
  windows/tabs/`claude.exe` processes but leaves the workspace **restorable**.
- **`Kill` remains the only destructive discard** (per-session) — Correctness Rule #6.

**Non-goals (for this pass)**
- Cross-machine / roaming sync.
- Reflowing a restored window onto a monitor layout that no longer exists (best-effort:
  fall back to WT's own off-screen clamping).
- Per-window *isolated fleets* (model B). We chose the shared fleet — see §3.

## 3. Decisions (locked)

1. **Model A — singleton engine + per-window lens.** Exactly **one**
   `SessionRegistry` / `HooksBridge` / `Scheduler` for the whole process. Every window's
   Manager tab is an independent **view** over that one shared fleet; it persists its *own*
   selection / scope / splitter sizes / collapsed dirs. Sessions are tagged with
   `(windowId, tabId)`.
2. **Close = hibernate, not delete.** Both close scopes only tear down live state:
   - **Close window** → ends that window's tabs + the `claude.exe` (and child trees) in
     them; the app keeps running; the window is **restorable**.
   - **Close app** → the same for every window; the process exits; all **restorable**.
3. **`Kill` is the sole prune** (per-session discard, destructive). There is no "permanent
   window close." Closing never removes anything from persistence.
4. **Pruning default (confirm in §9):** Option 1 — `Kill` is the only prune — plus a
   convenience **"Kill all sessions in this window."** If closing many windows becomes
   common, add Option 3 (restore picker / LRU cap) so launch is not an avalanche.

## 4. What Windows Terminal already gives us (leverage, don't rebuild)

Enable persistence with the global setting `firstWindowPreference` ∈ `defaultProfile` /
`persistedWindowLayout` / `persistedLayoutAndContent` (the last also persists scrollback
buffers). Then the following are **free**:

| Need | Free via WT | Evidence |
|---|---|---|
| Window restore loop on launch | Emperor replays saved layouts | `WindowEmperor.cpp:520` |
| Window **position + size + launch mode** | `WindowLayout.InitialPosition` / `InitialSize` / `LaunchMode` | `ApplicationState.idl:23`, captured in `TerminalPage.cpp:2864` |
| **Tab order** | `WindowLayout.TabLayout` is an *ordered* `IVector<ActionAndArgs>` | `ApplicationState.idl:23` |
| **Tab color** | `SetTabColor` action emitted from `_runtimeTabColor` | `Tab.cpp:560` |
| **Tab title** | `RenameTab` action emitted from `_runtimeTabText` | `Tab.cpp:571` |
| Per-pane content + commandline | `GetTerminalArgsForPane(kind)` | `Tab.cpp:539` |
| **Per-pane custom-state hook** | `IPaneContent.GetNewTerminalArgs(BuildStartupKind::Persist)` | `IPaneContent.idl:6` |
| Scrollback buffers (optional) | `firstWindowPreference: persistedLayoutAndContent` | `WindowEmperor.cpp:1288` |

`WindowLayout.ToJson` / `FromJson` (`ApplicationState.idl:20`) serialize a window's layout;
we reuse them rather than re-deriving geometry/tab serialization.

> **Caveat — tab color.** WT persists only a color that is actually *set*
> (`_runtimeTabColor`). We don't color tabs today; if we want **auto-color-by-state**
> (e.g. gold = waiting-for-you) that's a small *separate* feature (call
> `SetRuntimeTabColor` on state changes) — persistence then comes free.

## 5. Architecture

### 5.1 Singleton engine

Hoist the registry/bridge/scheduler out of `TerminalPage` into one process-wide owner
(created once by the Emperor/`App`, exposed via a shared accessor). Each window's
`TerminalPage` receives the **shared** `SessionRegistry` and hands it to its Manager tab
(`content->SetRegistry(shared)`). Result: **one** `\\.\pipe\agentmaster.<pid>` (PID is now
unambiguous), one `sessions`/record writer, and `+`-tab adoption stays well-defined (one
registry adopts hand-typed `claude`). The Manager **view** state already lives on
`AgentManagerContent` (per instance = per window), so per-window lenses fall out for free.

### 5.2 The per-window record (the new source of truth)

Because "close window → restorable while the app keeps running" cannot rely on WT's
**exit-only** layout save (`WindowEmperor.cpp:1244` dumps only the *currently-open* set,
overwriting it), Agentmaster owns a **per-window record**, debounced-autosaved on
meaningful change (tab add/remove/reorder/rename/recolor, pane split, window move/resize,
Manager-state change) — not just at exit:

```
WindowRecord {
  windowId:  stable GUID (generated once per window, embedded in the Manager tab args)
  geometry:  { InitialPosition, InitialSize, LaunchMode }   // from WT WindowLayout
  tabs:      [ TabEntry... ]   // ORDERED
  selectedSessionId / selectedTabIndex   // focused-at-close tab by STABLE identity (Claude convId,
                                         //   else shell-tab index; Increment 5)
  manager:   { selectedId, scopeDir, selectedPromptId, collapsedDirs[], splitterFractions,
               treeScope }   // treeScope: 0=LOCAL 1=GLOBAL 2=EXTERNAL — the ONE scope behind the
                             // Explorer Tree's 3-way toggle AND the Triage Board's 2-way twin
                             // (the board reads External as Global); absent => 0 (older records)
}

TabEntry { kind: Claude | Codex | Other }   // a discriminated REFERENCE, never a copy (Option 1)
  | Claude / Codex -> sessionId    // references the session in sessions.json; title / color / queue
                                   //   live THERE + the dir-color system (Codex rollout uuid on
                                   //   SessionInfo.codexSessionId). workingDir/title are NOT copied here.
  | Other          -> actionsJson  // WT ActionAndArgs (its own NewTab/SetTabColor/RenameTab)
```

> **As built (Option 1 — refs, not copies).** A managed `TabEntry` records the
> `sessionId` + a **`TabKind` discriminator** (`Claude` / `Codex` / `Other`) — NOT a copy
> of the session's auto-testing / autorunner / color. The session data lives in `sessions.json`
> (the one source of truth); the record only fixes tab **order** + window↔session **affinity**
> + the kind, so restore replays the right CLI (`claude --resume` vs `codex resume`). Color
> + title ride the referenced `SessionInfo` / the dir-color system (Rule #12), so
> `_CaptureWindowRecord` **stopped filling** the launch-time `tabColor` (dead round-trip data —
> `_RestoreWindowTabs` re-derives color from the dir-color map); the field + a tolerant deserialize
> survive only for back-compat.

Stored under `%USERPROFILE%\.agentmaster\windows\<windowId>.json` (one file per window so
closes/opens don't contend on a single document). The flat `sessions.json` is **kept** as the
session source of truth (the migration in §10 never fired — there was no legacy format to convert).

### 5.3 Manager-tab state via the persist hook

`AgentManagerContent::GetNewTerminalArgs(BuildStartupKind::Persist)` returns
`agentManager` content args that **embed `windowId` + the lens blob** (§5.2 `manager`). WT
calls it while building the layout (`Tab.cpp:539`); on restore, `_MakePane`'s
`agentManager` branch reads the blob and seeds the new `AgentManagerContent`. Consequences:
- **Splitter sizes move per-window** — out of the single global `layout.json` (which
  becomes only the default for brand-new windows).
- The embedded `windowId` is how a restored window re-binds to its `WindowRecord`.

### 5.4 Claude tabs vs. WT replay (the env-correlation reason)

WT's naive layout replay would re-run a Claude tab's commandline **without** our per-session
env (`CCMGR_SESSION_ID`/`CCMGR_HOOK_PIPE`) and **without** the engine wiring (injector bind,
registry record, `_claudeTabs` map) — so the restored `claude` would not correlate to hooks
or be controllable. Therefore:

- **Agentmaster owns Claude-tab restore.** `Claude` `TabEntry`s are recreated via
  `_LaunchClaudeSession(...)` (full cmdline + env + `--resume <convId>` + hooks), preserving
  their stored order/title/color.
- **WT owns the rest** — window geometry, the Manager tab, and any non-Claude (`Other`)
  tabs (replayed from their `actionsJson`).

This keeps a clean split and avoids double-spawning Claude tabs.

### 5.5 The pinned Manager tab — SUPERSEDED (2026-09-25): the Manager is a page, not a tab

> **Superseded.** The Agent Manager is no longer a tab. `_CreateAgentManagerPage()` builds the
> window's one `AgentManagerContent` at first layout; it is owned by `_managerPage` and never
> inserted into `_tabs`, and the tab strip's **Home** button shows it. So a window record never
> holds a Manager tab ref (capture walks `_tabs`), and there is nothing to force-create or re-pin.
> A window that was ON the Manager at close records no focused tab (`selectedSessionId` empty,
> `selectedTabIndex` -1), and `_RestoreWindowTabs` shows the page again. `selectedTabIndex` was
> always an index into `rec.tabs` (not a strip position), so older records keep working. The
> lens (`ManagerState`) persists exactly as before; the retired `managerTabColor` key is still
> read and written, unused. The original plan is kept below for history.

Originally, `_OpenAgentManagerTab()` force-created the Manager tab at index 0 in
`_OnFirstLayout`. With restore, the window's record already contains an `agentManager` tab.
New rule: **only force-create when the record has none** (first run / `defaultProfile`);
otherwise restore it from the record, then **re-pin it leftmost & non-closable** regardless
of stored order (it is special).

## 6. Lifecycle flows

**First launch (restore).** *(Revised for the Open ⇄ Archived model — Correctness Rule #6:
startup **ARCHIVES, never auto-launches**. The earlier "recreate every tab automatically"
flow is superseded by an explicit restore, so a relaunch never avalanches into N spawning
`claude.exe`es the user didn't ask for.)*
1. Emperor starts → create the **singleton engine** (one registry + one pipe + scheduler).
2. Load every `windows/<id>.json` (per-window UI state) **and** `sessions.json`; the sessions
   enter the registry **Archived** (`live=false`) — NOT auto-launched. The app opens to one
   window with no session tab (originally "just its pinned, non-closable Manager tab"; since
   2026-09-25 the Manager is a page behind the tab strip's Home button).
3. A **decide-prompt** offers to bring the workspace back — and the WindowRecord capture →
   restore wiring it was once *held* on is now **SHIPPED** (Increment 3/4, §13.5), so this is
   how launch actually behaves: when >1 record was open at last exit, a Yes/No "Reopen your N
   previous windows?" gates the reopen (Yes → reopen each at its geometry + lens + its tabs in
   order; No → one default window + the Manager's **Reopen Windows (N)** recover button). A lone
   open record reopens silently; first run opens one default window. Each window's tabs rebuild
   **per window, in order** — a `Claude` ref via `_LaunchClaudeSession(..., resume=convId)`, a
   `Codex` ref via `_LaunchCodexSession(..., codex resume <uuid>)` (both transcript/rollout-
   gated), an `Other` ref via WT action replay (`_RestoreWindowTabs`). The Manager's **Archived**
   page also restores any session **per-tab** into the current window anytime.
4. As each restored Claude `SessionStart` arrives, the engine re-attaches that `convId`'s
   Prompt Queue + autorunner from the record (correlate by id — window-scoped). A managed Codex
   has no hooks; its state comes from the C2 rollout tail (`_ReconcileManagedCodex`).

**Close window.** Tear down the window's tabs → each ConPTY closes → its `claude.exe` job
exits. The record is already current (debounced autosave), so nothing else to do — the
window is restorable. App keeps running.

**Close app.** Flush any pending debounce; tear down all windows (processes exit). All
records remain → full restore next launch.

**Session teardown — *as built*, `Kill` became `Archive` (non-destructive).** The agreed
design above kept a destructive per-session `Kill`; **what shipped has no `Kill`** — closing a
tab (the X, tree `Del`, the Manager's Archive, the Prompt Queue Archive button) all route through
ONE seam (`_HandleCloseTabRequested` → `_ArchiveAndCloseClaudeTab`) that flips `live=false`,
clears the injector, persists, and closes the tab while **keeping the record** (restorable from
the Archived page). Archive is terminal and the Claude transcript on disk is never deleted —
there is no discard (see §13's archive model + CLAUDE.md). The §3/§7/§9 `Kill` decisions are
historical; the `_KillClaudeSession`/"Kill all in window" items below never shipped.

**New window / new tab.** A fresh `windowId`; the Manager tab seeds from the global default
lens; sessions launched into it are tagged `(windowId, tabId)` and autosaved.

### 6a. Restore granularity & the launch prompt

When restoring, there are **two grains**, one model:

- **Per-window — the *layout* / "workspace".** The `WindowRecord` is the unit: geometry +
  Manager lens + the **ordered set of sessions that were open in that window**. The launch
  prompt restores at this grain ("reopen where I left off"), recreating the window(s) with
  their tabs in order. **Requires the WindowRecord capture → restore wiring** (M10 capture →
  M12 restore).
- **Per-tab — the *session*.** The Manager's **Archived** list restores **one session at a
  time** (or **Restore all**) into the *current* window — a cherry-pick, decoupled from any
  window grouping. **Already shipped** (`_RestoreArchivedSession` / the full-window Archived page).

So **per-window is the coarse "restore my workspace" unit (the launch prompt); per-tab is the
fine-grained cherry-pick (the Archived button), always available.** A session restored per-tab
re-homes into the current window; full per-window restore (the `WindowRecord` tab ref →
`_RestoreWindowTabs`) is **now shipped**, so the affinity is honored, not merely advisory.

**Locked decisions for the launch prompt (with what actually shipped):**
- *Decided:* it would **offer all archived sessions** (not only those open at last close — so
  no `openAtExit` marker needed). **As built (§13.5, Increment 3):** the reverse choice won —
  an **open-at-exit manifest** (`open-windows.json`) now reopens **only the windows that were
  open at last exit**, so a window closed mid-session is not re-offered; the Archived page still
  lets you restore *any* session per-tab.
- *Decided:* it would be **held until the full per-window `WindowRecord` capture is wired
  first** (to restore true per-window layouts, not a flat global list). **As built:** that
  capture/restore IS wired (Increments 1–5), so the prompt is **no longer held** — launch
  reopens true per-window layouts (geometry + lens + ordered tabs). Startup still stays clean
  (no auto-launched claude — Rule #6).

## 7. Correctness rules (extend `IMPLEMENTATION.md` §"Correctness rules")

6. **Restore = resume, not replay.** Claude tabs restore via `claude --resume <convId>`
   with the full per-session env (Codex via `codex resume <rolloutUuid>`); `Sent` prompts
   are never re-sent. (Unchanged in spirit; now window-scoped, transcript/rollout-gated, and
   kind-aware — `_RestoreArchivedSession` / `_RestoreWindowTabs` branch on `SessionInfo.kind`.)
   **Restart, too, resumes — never replays the launch commandline.** "Restart session" (WT's
   `restartConnection`, relabeled) rebuilds a managed pane from its **CURRENT** conversation
   (`_RestartManagedSession`: Claude via `BuildClaudeRestartSpec` — keep the id, `--resume`
   when a transcript exists else fresh-with-the-same-id; Codex via `codex resume <uuid>`,
   rollout-gated), **not** by replaying `ConptyConnection::Commandline()` verbatim — the
   original launch command was `--session-id <A>`, which on a transcript that now exists makes
   claude refuse ("session id already in use") and exit immediately (a fresh-launched session
   could never be restarted). It resumes the re-homed `_claudeTabs` key, so a conversation that
   diverged via `/clear` · `/compact` · `/resume` is restarted at its CURRENT id, not the stale
   launch id.
7. **Close persists, only `Kill` discards.** Closing a window or the app must leave every
   `WindowRecord` intact and restorable; the sole removal path is `Kill`.
8. **One engine per process.** Exactly one registry/bridge/pipe; windows share it. Never
   key the bridge on anything that collides across windows (the PID pipe is fine *because*
   the engine is now a singleton).
9. **`windowId` binds the lens to the window.** The Manager tab's persisted args carry the
   `windowId`; restore re-binds via it. Never infer window identity from tab position.

## 8. Risks

- **WT save vs. our save.** WT's own exit-time layout save could fight ours. Mitigation:
  drive restore from **our** records only; either leave `firstWindowPreference:
  defaultProfile` (WT doesn't persist; we do everything) **or** mark Agentmaster windows so
  WT skips them. Decide in §9.
- **Autosave cost.** `GetWindowLayout` walks the tab tree on the UI thread — debounce and
  only on structural change, never per keystroke.
- **Restore avalanche.** Many saved windows all reopen — see pruning Option 3 (§9).
- **Monitor changes.** Restored geometry may be off-screen; rely on WT's existing clamping.

## 9. Decisions to confirm before coding

- **Pruning:** Option 1 (`Kill` only) + "Kill all in window" — accept as default, or also
  add Option 3 (restore picker / cap)?
- **Restore authority:** confirm §5.4 split (Agentmaster restores Claude tabs; WT restores
  geometry + non-Claude tabs) and whether `firstWindowPreference` is left at
  `defaultProfile` (we own all persistence) vs `persistedWindowLayout` (and we suppress
  WT's claude-tab replay).
- **Buffers:** restore scrollback (`persistedLayoutAndContent`) or just re-attach the live
  session?

## 10. Migration

On first run of the new build, if `windows/` is empty but a legacy `sessions.json` exists:
synthesize **one** default `WindowRecord` (current geometry) whose `tabs` are the legacy
sessions, then archive `sessions.json`. No user-visible loss; subsequent runs use the
per-window records.

## 11. Milestones (continue `IMPLEMENTATION.md`; M0–M8 are done)

> **⚠️ Superseded by §13.** This original list predates two decisions that reshaped the work:
> the **session-archive model** (sessions persist/restore *window-blind* via `sessions.json` +
> `SessionInfo.live`, **on demand** — no auto-restore, no `Kill`) and **Option 1** (the
> `WindowRecord` holds per-window *UI state only* — geometry + lens + tab *refs* — never copies
> session data). **M9 shipped; M10's data layer shipped.** M11 is largely obviated;
> M14 migration is moot. See **§13** for the live delivery plan that finishes the job.

- **M9 — Singleton engine.** Hoist registry/bridge/scheduler to one process-wide owner;
  share it to every window's Manager tab; one pipe; tag sessions `(windowId, tabId)`. Fixes
  the multi-window collision. *(Prereq for all below.)*
- **M10 — Per-window record store.** `WindowRecord` schema + `windows/<id>.json`;
  `windowId` generation + embedding; debounced autosave on structural/lens change; load on
  startup. Reuse `WindowLayout.ToJson` for geometry + `Other` tabs.
- **M11 — Manager state (de)serialize.** `GetNewTerminalArgs(Persist)` emits lens+`windowId`;
  `_MakePane(agentManager)` seeds from it; splitter sizes per-window; reconcile the forced
  index-0 Manager tab (no double-create; re-pin).
- **M12 — Restore engine.** Rebuild all windows (geometry → Manager tab → ordered tabs);
  Claude via `_LaunchClaudeSession(resume)`, `Other` via action replay; re-attach side-data
  on `SessionStart`.
- **M13 — Close lifecycle.** Window-close & app-close = teardown only (records already
  persisted); `Kill` prune + "Kill all in window"; (optional) restore picker/cap.
- **M14 — Migration + tests.** Legacy `sessions.json` → one default record; standalone
  checks: record (de)serialize, `windowId` binding, restore ordering, close-keeps-restorable,
  `Kill`-prunes. Extend `AgentMaster/tests/` (keep the all-pass tradition).

## 12. Touch map (where the code lands)

- `AgentMaster/Persistence.{h,cpp}` — `WindowRecord` (de)serialize, `windows/` IO, migration.
- `AgentMaster/SessionModels.h` — `WindowRecord` / `TabEntry`; `(windowId, tabId)` on
  `SessionInfo`.
- New process-singleton holder for the engine (Emperor/`App` scope) + accessor.
- `TerminalPage.{h,cpp}` — consume the shared engine; restore driver; window/tab lifecycle
  hooks (close → record flush; `windowId` plumbing); Claude-tab restore.
- `AgentManagerContent.{h,cpp}` — `GetNewTerminalArgs(Persist)` lens (de)serialize; splitter
  sizes per-window; "Kill all in window".
- Settings: default `firstWindowPreference` per §9.

Keep the diff against upstream **additive** (new engine files + small touches at the
integration points) so rebasing onto `microsoft/terminal` stays cheap — same convention as
the rest of Agentmaster.

## 13. Delivery plan to full ship (revised — supersedes §11)

Persistence is now **two layers**:
- **Session layer — SHIPPED.** The archive model: `sessions.json` + `SessionInfo.live`; startup
  loads everything **Archived**; closing a tab archives it; the global full-window **Archived** page
  restores on demand (`claude --resume`, transcript-gated). This is the session source of truth
  and is done — nothing below changes it.
- **Window layer — THIS PLAN.** Per-window **UI state** (geometry + Manager lens + ordered tab
  *refs*) in `windows/<windowId>.json` (Option 1, §3). **M9 + the M10 data layer are done.** What
  remains is *capturing* that record from a live window and *re-applying* it on launch.

### 13.0 Lock one product decision first (blocks Phase C only)

**On app launch, what comes back?** The session layer already guarantees "no claude is
relaunched — everything is Archived." The window layer adds: do we **reopen the saved windows**
(empty — Manager tab + their geometry + their splitter/lens), or keep launching a **single**
window?
- **Target (recommended):** reopen each saved window at its geometry/lens, empty of sessions; the
  Archived page restores sessions, which **re-home** to their origin window. Delivers
  "close == reopen your workspace layout" without relaunching any claude.
- **Lite:** if multi-window restore is *not* wanted, the plan collapses to **single-window
  geometry+lens** (Phases A, B, C-lite, E — skip the Emperor multi-window work). Much smaller.

Phases A–B are identical either way, so coding starts before this is locked.

### 13.1 Phases

**Phase A — Identity + lens exposure (additive, low-collision).**
- `TerminalPage::_windowId` (a GUID `hstring`), minted once at engine init; a restored window
  adopts its record's id instead (Phase C).
- `AgentManagerContent::GetManagerState()` → `ManagerState` (selection / scopeDir /
  selectedPromptId / collapsed dirs / `_layout` / `treeScope` — the shared tree+board
  LOCAL·GLOBAL·EXTERNAL scope, added later) — a pure getter over existing members.
- *Files:* `TerminalPage.{h,cpp}`, `AgentManagerContent.{h,cpp}`.
- *Done when:* both compile (lib check) and a debug line can dump a window's id + lens.

**Phase B — Capture + autosave (the core of M10).**
- `TerminalPage::_CaptureWindowRecord()` builds the `WindowRecord`:
  - **geometry** — reuse WT's own layout path (study `PersistState()` /
    `IslandWindow`/`AppHost` `GetWindowLayout` → `InitialPosition` / `InitialSize` / `LaunchMode`)
    and fill `WindowGeometry`.
  - **tabs** — walk `_tabs` in order; classify each: the Manager tab → skip; a Claude tab (id in
    `_claudeTabs`) → `TabEntry{Claude, sessionId, tabColor}` (color from the tab's runtime color,
    Rule #12); else → `TabEntry{Other, actionsJson}` from the tab's persist actions.
  - **manager** — `content->GetManagerState()`.
- **Debounced autosave** (`ThrottledFunc`, already used in `TerminalPage`): `SaveWindowRecord`
  on structural/lens change — tab add/remove/reorder (`_tabs.VectorChanged`), tab rename/recolor,
  window move/resize, and a lens-changed callback from the content. **Never per keystroke.**
- *Files:* `TerminalPage.{h,cpp}`, `AgentManagerContent.{h,cpp}` (the lens-changed callback).
- *Done when:* open/move/resize/retab a window → a correct `windows/<id>.json` appears; verified
  by inspecting the file + a standalone capture-shape test.

**Phase C — Restore (M12; gated on §13.0).**
- *Multi-window target:* at startup (Emperor/`App` scope), for each `LoadWindowRecords()` record
  (deterministic order): create a window at its `geometry`, seed its Manager tab with the lens.
  The session layer leaves every session Archived, so the window comes back **empty**. This needs
  Emperor-level multi-window creation — the deepest cut; **spike a 2-window reopen first.**
- **Re-home:** when `_RestoreArchivedSession` opens a session, route its tab into the window whose
  record references that `sessionId` (if open), else the current window.
- **windowId stability:** a restored window adopts its record's `windowId`, so its next autosave
  overwrites the same file.
- **Pinned Manager tab:** force-create only when the record has none; else restore + re-pin
  leftmost/non-closable (mirrors today's `_OpenAgentManagerTab` guard).
- *Files:* `TerminalPage.{h,cpp}`, the Emperor/`App` startup seam, `AgentManagerContent` (lens seed).
- *Done when:* close app with N windows → relaunch → N windows reopen at geometry + lens, empty;
  restoring a session re-homes it.

**Phase D — Lifecycle + cleanup.**
- **Close flush:** capture once more on teardown (hook `PersistState()` / the window-close seam)
  so the last move/resize isn't lost past the debounce.
- **Retention:** keep all window records (cheap); a manual "forget this window's layout" only if
  clutter appears (`DeleteWindowRecord` already exists). There is **no `Kill`** — archive is the
  only session teardown.
- *Files:* `TerminalPage.{h,cpp}`.

**Phase E — Tests, verify, deploy.**
- Standalone: capture-shape unit (fake tab list → expected record), restore ordering, re-home
  affinity lookup, geometry round-trip with the `has*` flags. Keep the all-pass tradition.
- Live: run the §13.3 acceptance script in the deployed package; tail `hooks.log`.
- Update `CLAUDE.md` Status (window layer shipped) and retire §11.

### 13.2 Risks & mitigations
- **Geometry-capture API is the unknown** — spike the geometry *read* first (find the exact
  `GetWindowLayout`/`InitialPosition` path) before building autosave around it.
- **Emperor multi-window creation** (Phase C) is the deepest integration — spike a 2-window
  reopen before wiring the rest; if too invasive, fall back to §13.0's single-window-lite ship.
- **Live-edit collision** — Phases A/B/D touch `TerminalPage`/`AgentManagerContent`, files under
  active archive-model work. Land each as a small, separately-compilable diff when those areas
  are at rest.
- **Lazy tabs / multi-monitor** — restored windows open *empty*, so there's no lazy-claude start
  problem; WT clamps off-screen geometry.

### 13.3 Definition of done (acceptance)
1. Two windows, different positions/sizes + different splitter layouts + a few sessions → close
   app → relaunch → **both reopen at their geometry with their splitter/lens**, Manager-only,
   sessions Archived.
2. Restore a session from Archived → its tab opens **in the window it was closed in**.
3. Single-window use unchanged; first run (no `windows/`) opens one default window.
4. Move/resize/retab, then kill the app hard (no clean close) → relaunch → last layout is within
   one debounce of correct.
5. All standalone checks pass (incl. new capture/restore tests); lib + full exe build clean;
   deployed and verified running.

### 13.4 Sequencing
A → B (ship capture+autosave first — useful alone, de-risks the schema) → **confirm §13.0** →
C (spike Emperor multi-window, then wire) → D → E. Each phase compiles via the lib check and
lands as its own commit.

### 13.5 Delivery status (as built)

> **STATUS — window layer COMPLETE & live-verified.** Increments 1–5 are all SHIPPED &
> live-verified: single-window geometry+lens (1–2), multi-window reopen with open-at-exit manifest +
> recover button (3), window-grouped restore = session re-home + shell-tab replay + grouped Archived
> UI (4), and selected-tab persistence (5). The §11 `M9–M14` ladder is retired (M9/M10 shipped, M11
> obviated, M12/M13 delivered as these Increments, M14 migration moot). The **only** open item is
> cosmetic: the exact left-to-right interleave of Claude vs `Other` tabs on reopen (both kinds return;
> only the ordering is approximate — Claude tabs land first, then shells).

**Increment 1 — SHIPPED & live-verified** (commits `67d8b8e4b` engine/data-layer +
`caa3f1f1d` capture/restore). Phases A + B + per-window **lens** restore, single-window:
- **Identity (A):** `Engine::ClaimWindowRecord()` hands each window an existing
  `windows/<id>.json` (popped from a shared unclaimed set under lock) or a fresh
  `CoCreateGuid` id; `TerminalPage::_windowId`/`_windowRecord`/`_windowRecordClaimed`.
- **Capture + autosave (B):** `_CaptureWindowRecord()` reads geometry (the exact
  `PersistState` recipe — `RequestLaunchPosition` + tab-content size + launch-mode flags),
  ordered tab refs (Manager/Settings skipped; Claude → `sessionId` + color; else `Other`),
  and the cached lens. `_saveWindowRecordThrottled` (750 ms trailing) → `_FlushWindowRecord`,
  triggered by `_tabs.VectorChanged`, `_tabContent.SizeChanged`, tab recolor, the content's
  lens push, and a one-shot save at the end of `_CompleteInitialization` (so the record exists
  from launch).
- **Lens restore + content push:** `AgentManagerContent::GetManagerState`/`SetManagerState` +
  `SetLensChangedHandler`; every lens mutation pushes the current lens to the page. A **claimed**
  record seeds the lens (re-applies splitter fractions to the live tracks); a **fresh** window
  primes the page cache *from* the content's actual lens.
- **Verified live:** app launches, engine up, archive model intact; a record is written with
  **real** geometry (e.g. `x:156,y:156,1113×586`) and the **real** global splitter layout
  (`0.39066/0.448785`, not the `0.4/0.4` default); on relaunch the **same windowId file is
  reused** (claim works), confirming capture → persist → claim → restore → re-persist.

**Findings (this pass):**
- **`PersistState()` is never called in our mode.** `WindowEmperor::_persistState`
  (`WindowEmperor.cpp:1249`) calls `TerminalPage::PersistState()` **only when
  `firstWindowPreference != DefaultProfile`** — but "Agentmaster owns everything" deliberately
  runs `DefaultProfile`. So a close-flush hooked there is dead code. Increment 1 instead relies
  on autosave-on-change (lens changes always save), which is sufficient for the lens. The
  proper close/teardown flush (for geometry move-without-resize) moves to **Increment 2** via a
  seam that *does* fire (window deactivate / visibility-change), independent of WT's gating.
- **Self-found bug (fixed):** the page caches the lens only via the content's *push*, but a
  fresh window never pushes before its first change — so a save triggered before the first lens
  push (e.g. a resize) would persist a **stale default layout** and reset splitters on reopen.
  Fixed by priming the cache from `content->GetManagerState()` at wire time (verified: the live
  record shows the real `0.39066/0.448785`, not `0.4/0.4`).

**Increment 2 — SHIPPED & live-verified** (commit `6d609a815`). A relaunched window reopens
at its saved position/size/launch-mode via the **`TerminalWindow` startup seam**
(`GetInitialPosition`/`GetLaunchDimensions`/`GetLaunchMode` — WT's own persisted layout is OFF
in DefaultProfile, so they otherwise default). `_AgentmasterRestoreGeometry()` returns the front
record's geometry (single-window) — the same record `ClaimWindowRecord` pops, so geometry + lens
agree. Runs at window creation (before control-init → no AV hazard). **Verified live:** edited a
record to a distinctive `x:480 y:320 1000×640`, relaunched, `GetWindowRect` → `Left=472 Top=320
size=1016×689` (Top exact; Left = 480 − 8px frame; size = content + frame + titlebar). So
**single-window workspace restore — geometry + lens — fully works.**

**Increment 3 — multi-window reopen: CORE SHIPPED & live-verified** (decision: **auto-reopen on
launch, gated by a decide-prompt, + a recover button** for history). Commits `bf2161602` (plumbing),
`319cdb44a` (Emperor loop), `a9364b7d6` (prompt).
- **Plumbing:** `Engine::ClaimWindowRecord(windowId)`; `TerminalPage::SetAgentmasterWindowId`;
  TerminalWindow indexes geometry by `_loadFromPersistedLayoutIdx` (the `-s <idx>`) and hands the
  resolved id to the page — so a restored window's geometry + lens come from the same record,
  race-free.
- **Emperor loop:** `WindowEmperor` (links `TerminalApp.dll`, not the `TerminalAppLib` static lib, so
  it can't call `::Agentmaster::LoadWindowRecords`) **counts `windows/*.json` itself** and dispatches
  `wt -w new -s <idx>` per record, mirroring WT's own `PersistedWindowLayouts` loop (empty in our
  DefaultProfile mode); the trailing `_windows.empty()` guard suppresses the extra default window.
- **Decide-prompt:** when >1 record exists, a Yes/No "Reopen your N previous windows?" gates the
  loop (Yes → reopen all; No → one default window). A lone record restores silently; first run (0
  records) opens one default window.
- **Verified live:** 2 records → relaunch → the prompt (#32770 dialog) appears with **0** windows yet
  (reopen correctly gated); `IDYES` → **2 windows**, each at its OWN saved position (record `200,150`
  → window `Left=192 Top=150`; record `840,440` → `Left=832 Top=440`; 8px X = Win11 frame, Top
  exact). Single-record and first-run paths unchanged.

**Increment 3 — refinements (SHIPPED & live-verified):**
- **Open-at-exit manifest — DONE** (commit `26991a062`). The reopen set is no longer *every record
  ever*; it is exactly the windowIds that were OPEN at last exit (a window you close mid-session no
  longer lingers and gets re-offered). `WM_CLOSE_TERMINAL_WINDOW` can't tell a single user-close from
  app-exit, so rather than have the Emperor enumerate windows at the exit seam (which would need
  `windowId` on the projected `TerminalWindow` surface), the **process-wide `SharedEngine` owns the
  manifest** — it already receives every window's id (each `TerminalPage` registers `_windowId` at
  engine init, unregisters in `~TerminalPage`). Every change rewrites `open-windows.json` (a sibling
  of `sessions.json`) = the live id set, **except** a change that empties it is skipped (skip-empty
  keeps the final snapshot; a hard shutdown that kills the threads before they unregister leaves the
  full set on disk — both correct). `LoadWindowRecords` now **sorts by filename** so a record's index
  is canonical (`-s <idx>` agrees across the Emperor scan and the reopened window's lookup). The
  Emperor reads the manifest (a byte-scan for braced `{guid}` tokens — it links `TerminalApp.dll`, not
  the static lib, so it can't call the JSON helpers; keys `version`/`open` aren't braced so they're
  excluded), intersects with the sorted records, and reopens only the open-at-exit set
  (`wt -w new -s <idx>`); the decide-prompt count is that intersected size. A missing/empty manifest
  falls back to ALL records (graceful migration; skip-empty makes a present manifest never legitimately
  empty). **Less plumbing than the projected-surface route, and it survives a hard shutdown better.**
  Verified live: 2 records + a manifest naming only one ⇒ exactly that one reopens (silent, no prompt)
  at its geometry; a seeded fake id is dropped and the second record never added (register rewrote to
  the live set); the manifest survives app-close (skip-empty); and a **gracefully** mid-session-closed
  window (`SC_CLOSE` → `~TerminalPage` → `UnregisterLiveWindow`) is **pruned** from the manifest while
  its record stays on disk (recoverable).
- **Manager "Reopen Windows (N)" recover button — DONE** (commit `04b1989ec`). The "if I answered No"
  path: a toolbar button next to **Archived**, shown only when N>0, where N == records-minus-live
  (`Engine::RecoverableWindows`). Click → confirm → `TerminalPage::_ReopenSavedWindows`, which
  ShellExecutes `wt -w -1 -s <idx>` per not-currently-open record (the same wt-exe path as
  `_OpenNewWindow`; the single-instance handoff routes it back to the running Emperor → the identical
  reopen pipeline, just at runtime). **No Manager→Emperor IPC** — a within-process page already reaches
  new-window creation by ShellExecuting the wt exe with `-w -1`. Verified live: a recoverable record
  reopened a 2nd window at its saved geometry (`Left=692 Top=480`) via the `-s <idx>` runtime handoff,
  alongside the already-open window.
- **`Other`-tab `actionsJson` capture** (non-Claude tab recreation) and session **re-home** (route a
  restored session into the window whose record references it) — **both now SHIPPED & live-verified
  (Increment 4, window-grouped restore).** `TerminalPage::_RestoreWindowTabs` (run from `_OnFirstLayout`
  after `_RestoreClaudeSessions`, gated on a *claimed* record) walks the record's ordered tab refs and
  rebuilds the window in place: a **Claude** ref resumes its session INTO this window
  (`_LaunchClaudeSession`, lazy-start safe), an **Other** ref replays its captured startup actions
  (`WindowLayout::FromJson` → one `ProcessStartupActions`) to recreate the shell tab with title + color
  + cwd. Capture fills `actionsJson` via `BuildStartupActions(Persist)` → `WindowLayout::ToJson`
  (`_CaptureWindowRecord`). **Shell-tab cwd is recovered out-of-band** so a reopened pwsh/cmd tab lands
  where the user `cd`'d, not at the launch dir: `BuildStartupActions` only knows `WorkingDirectory()`
  (OSC 9;9 shell integration) else the profile default, and PowerShell **freezes its own process cwd**
  (`Set-Location` updates only `$PWD`), so capture overrides the `NewTab` `StartingDirectory` with the
  Fleet Observer's reading — `cmd`'s own PEB cwd (it syncs on `cd`) or a pwsh's **newest native child**
  PEB cwd (children inherit the live `$PWD` at spawn), cached per tab across idle gaps
  (`ResolveShellCwd` / `ProcessObserver._shellCwdCache` → `TabActivityRow.cwd`; capture reads it via
  `_ShellTabIdent` + `Activity()`). Read-only — never writes to a shell (Rule #13). *Caveat (structural,
  investigated + deliberately deferred):* a pwsh tab that only ran shell **builtins** + `cd` (no external
  command ever) leaves no observable child, so it falls back to its launch dir. This residue is
  **unclosable by any out-of-band read** — pwsh freezes its process cwd, `[Environment]::CurrentDirectory`,
  AND its open directory handle (all verified stale at launch dir), and **no file on the machine records
  a plain shell's cwd** (the "read the file the agent writes" trick already covers every OTHER case — a
  claude tab via its transcript, a codex tab via its rollout `session_meta.cwd`, cmd/bash via the PEB —
  but a plain pwsh writes nothing; even Codex, open-source, only knows its OWN cwd via `current_dir()` and
  has no foreign-cwd reader / introspection command). The ONLY lossless fix is **shell integration** — the
  shell emitting its cwd from a prompt hook (`OSC 9;9` ConEmu / `OSC 7` file-uri / VS Code's
  `OSC 633;P;Cwd`, all already parsed by our terminal into `WorkingDirectory()`), injectable VS-Code-style
  (a prompt-WRAP that preserves oh-my-posh/starship) into our OWN pwsh profiles in `settings.json`. Deferred
  by decision (2026-06): the gap is narrow — cmd & bash already sync their PEB cwd, so only a cd-only
  *plain pwsh* tab is affected — and the fix requires a one-time prompt-hook injection nobody wanted yet. The Archive page is **grouped by window** (`_GatherArchiveRows` over
  `RecoverableWindows`): a per-window **Reopen window** (`_ReopenSavedWindow(idx)` → `agentmaster -w -1
  -s <idx>`) over its session rows (**Restore here** cherry-picks one into the current window). A clobber
  guard (`_FlushWindowRecord`: skip a no-tabs+no-geometry or pre-Initialized capture) + a close-flush
  before the gap-#1 teardown keep the record intact. Live-verified: a 1-Claude + 3-pwsh window closed →
  reopened at geometry with the session resumed + all shells (title/cwd) replayed; record round-tripped.
  **Still deferred:** exact left-to-right interleave of Claude vs Other tabs on reopen (Claude first,
  then shells — both kinds return, only the ordering is approximate).

**Acceptance (§13.3) status:** #2 ✅ (session re-home — Increment 4), #3 (single-window unchanged; clean first run), and
**#1 multi-window reopen at geometry/lens — DONE & verified.** The two refinements above are now also
**DONE & verified** (the reopen set is exactly the open-at-exit windows; a mid-session-closed window is
pruned; the in-session recover button brings back any saved-but-not-open window). **Increment 3 is
complete; Increment 4 (window-grouped restore: session re-home + `Other`-tab recreation + grouped
Archive UI) is now also complete & live-verified.**

**Increment 5 — selected-tab persistence — SHIPPED & live-verified** (commits `a8f744fa5` +
`549c4fd8e`). The reopened window **re-selects the tab that was focused at close**, persisted by
STABLE IDENTITY: a Claude tab via `WindowRecord::selectedSessionId` (the conversation id — the same
stable ref `sessions.json` uses), a non-Claude shell tab via `selectedTabIndex` (its WT session GUID
is regenerated each run, so it has no cross-restart id), Manager / none ⇒ both unset (Manager is
index 0, the natural default). `_CaptureWindowRecord` records it (and a tab-switch persists it live,
not just at close); `_RestoreWindowTabs` resolves the focused tab's live position and applies it
deterministically — a trailing `SwitchToTab` appended to the shell `ProcessStartupActions` batch (so
it runs after the async shells exist), or a synchronous select when there are no shells.

**Net:** the window layer is **complete & live-verified** end-to-end (geometry + lens + ordered tabs
+ session re-home + shell replay + focused-tab restore). The single remaining refinement is the
approximate Claude-vs-`Other` tab interleave noted above (cosmetic).

## 14. The restore wake: the tabs that were busy when Agentmaster went down (2026-10-19, in production since 0.6.8.23 and dev since 0.0.1.31)

**The gap (the user's report, 2026-10-19).** `_RestoreWindowTabs` re-homes every tab of a reopened
window DORMANT: a tab's terminal starts its claude only on the tab's first layout, so after a
restart only the tab each window opens to starts. That is right for the hundred tabs that sat idle.
It is wrong for the few that were in the middle of something: with 100 tabs and 5 of them busy, the
4 busy tabs nobody looks at sit dormant and stuck until someone clicks them.

**What it does.** After a window restore, every re-homed tab whose session was in the middle of
something when Agentmaster went down starts by itself, in place (the "Activate Tab" path,
`_ActivateDormantSession`). "In the middle of something" is the user's rule: its state AT THE
RESTART was anything but Idle / Waiting-for-you / Done, so Running, NeedsApproval or Error. The
wake waits for two things:

1. the tab EVERY reopened window opens to has finished launching (the cross-window barrier below),
   so the tabs the user looks at get the machine first;
2. the window's UI thread is idle (1.5 s of punctual timer ticks, the splash watch's own bar), or
   30 s since the barrier released it.

The wakes are then paced ACROSS EVERY WINDOW (`RestoreWakePacer`): one tab per slot, slots
500 ms apart and at most 4 per 10 s for the whole process — the "Activate All Tabs" drip's pace,
but process-wide, because the barrier releases all windows at the same moment, and four windows
each running their own drip would start sixteen claudes within 1.5 s. The window's own restore-
wake timer takes the slots (first come, first served on its 250 ms ticks), so the per-window drip
is not used and shows no busy state. A user's "Activate All Tabs" meanwhile runs its own drip;
the restore wake's later candidates then read started and are skipped without a slot.

**Which tabs: `RestoreWakeWantsSession`** (`AgentMaster/RestoreWake.h`). A tab is a candidate when
its record is live, never started, ours (not `external`), and `SessionInfo::midTurnAtRestore` is
set. The restore seams set that flag from the persisted state before they normalize it
(`RestoredMidTurnFlag` → `RestoredSessionState`, HOOKS.md *Cut-off turns*). The state is read again
at wake time, because the scanner refines a restored record while it waits:

| State at wake time (with the flag) | Woken? | Why |
|---|---|---|
| Idle | yes | a persisted Running or Error, normalized to Idle |
| NeedsApproval | yes | kept by the restore; the question died with its process. Waking brings the conversation back up so it can be answered or continued (what the resumed claude does with the open question is Claude Code's, not verified). The scanner usually re-reads such a dormant tab as a cut-off Error first (HOOKS.md *Cut-off turns*) |
| Error | yes | the scanner's cut-off or API-error verdict since the restore |
| Waiting-for-you, unpinned | no | the scanner's recon-stop proved the turn ended |
| Waiting-for-you, PINNED | yes | the pin hold (`ApplyPinHold`) turns a live pinned Idle into Waiting, so here it still means a normalized Running / Error |
| Running | no | a live process runs the turn somewhere (another tab's claude on the same conversation); starting this tab would add a second writer |
| Done | no | |

A record that was Idle, Waiting-for-you or Done at the restart has no flag and is never woken,
even when the scanner later reads it as Error (an error the user had dismissed comes back as Error
after a restart): the rule is the state at the restart, and re-running a turn the user abandoned
would spend tokens on it. A restore-FRESH (the transcript was gone) removes the old id, so it is
not a candidate: a new conversation has no turn to continue. At wake time a candidate is also
skipped when its tab started meanwhile (the focused one, or one the user opened), was closed, or
moved to another window.

**When, per window: the foreground gate** (`RestoreWakeForegroundStep`, ticked every 250 ms by
`_TickRestoreWake`). "The tab the window opens to" is the managed session the restore focused,
while this window still hosts it under that id (with shells in the record, the selection lands
only after the async shell replay, and a shell created meanwhile must not count for it). Otherwise
it is whatever the window shows now. It counts as LAUNCHED:

| Foreground | Launched when |
|---|---|
| no terminal (the Agent Manager page, no tab, a Settings tab) | at once |
| a terminal that hosts no Claude session (a shell, a Codex) | its connection started |
| a Claude session's terminal | its connection started AND a hook newer than the restore arrived (its SessionStart, normally); or 15 s after it was first seen started without one (a broken forwarder; a re-forked never-messaged fork, whose startup SessionStart arrives under the SOURCE id and is dropped by the fork-echo guard) |
| anything | 90 s after the re-home ended, whatever it shows (a focused tab that never starts must not hold every window) |

A hook OLDER than the restore proves nothing: it is the session's earlier life in the same run (a
window closed and reopened with "Reopen Windows (N)").

**When, across windows: the barrier** (`RestoreWakeBarrier`, one per process,
`SharedRestoreWakeBarrier()`):

- `Announce` at window CREATION: `TerminalWindow::Initialize` calls
  `TerminalPage::SetAgentmasterWindowId` for every window the Emperor reopens from a saved record,
  and the Emperor creates every reopened window before the first one lays out. So every reopened
  window is waiting in the barrier before any restore ends, and the first window to finish its
  re-home cannot wake its busy tabs while another window is still restoring. (The windows lay out
  one after another on the one UI thread; on 2026-10-19 the second window registered 4 ms after
  the first one's re-home ended.)
- `Join` at the end of `_RestoreWindowTabs`. A window restored without an announce (the one default
  window that front-pops a record after a "No" to the reopen prompt) enters here.
- `NoteForegroundLaunched` once, the tick the window's gate latches.
- The release rule (`RestoreWakeReleaseVerdict`): when every waiting window has joined and launched
  its foreground, all of them are released at once. A window with no busy tab still joins, holds the
  other windows back until its own foreground launched, and then leaves.
- The cap: 5 minutes after the OLDEST waiting entry, every waiting window is released, ready or not,
  so an announced window that never restores (destroyed before its first layout) costs the others a
  late wake, never a lost one. A straggler released by the cap waits only for its own foreground.
- `Leave`: after the wake, when the window restored nothing (`_RestoreWindowTabs`'s exit guard: the
  assigned record was gone, the record holds no tab, or the restore threw), when its job failed, and
  in `~TerminalPage` (thread-safe, any thread).
- Rounds: windows reopened later (the "Reopen Windows (N)" button) form a round of their own; the
  released entries of an earlier round no longer count.

**The code.** The decisions are pure and header-only in `AgentMaster/RestoreWake.h` (the predicate,
the gate, the wake decision, the barrier, the pacer). The glue is in
`TerminalPage.AgentWindowRecord.cpp`: `_AnnounceRestoreWake` (from `SetAgentmasterWindowId`),
`_BeginRestoreWake` (the end of `_RestoreWindowTabs`), `_TickRestoreWake`,
`_ReadRestoreWakeForeground`, `_WakeRestoredBusyTabs` (the release: the plan, then the WAKING
phase), `_StepRestoreWakeQueue` (one paced wake per free slot, through `_ActivateDormantSession`),
`_EndRestoreWake`, `_LeaveRestoreWake`. The window's job is `TerminalPage::_restoreWake` (a
`RestoreWakeWindowJob`) with its timer `_restoreWakeTimer`. Tests: `TestRestoreWake`
(`tests/tests_restorewake.cpp`).

**The log** (`[restore-wake]` in hooks.log; one line per step and window; the ids and times below are
made up):

```
[restore-wake] window {id}: announced - it restores a saved record; ...
[restore-wake] window {id}: restored; 3 busy tab(s) to wake once the tab every reopened window opens to has launched: 1a2b3c4d(Idle) 5e6f7a8b(NeedsApproval) 9c0d1e2f(Error); focus=7d8e9f0a; barrier: 4 window(s) waiting, 1 restored, 0 launched
[restore-wake] window {id}: its focused tab finished launching (its claude is up (a hook arrived)) 23810 ms after its restore; 3 busy tab(s) to wake; barrier: ...
[restore-wake] window {id}: waking 3 busy tab(s) 26190 ms after its restore (every reopened window launched its focused tab; the UI is idle; paced across every window: 500 ms apart, at most 4 per 10 s): 1a2b3c4d(Idle) ...; skipped 1: 3f4e5d6c(running already)
[nav] activate-dormant 1a2b3c4d
[restore-wake] window {id}: done - woke 3 busy tab(s), 27240 ms after its restore
```

One `[nav] activate-dormant <sid8>` per woken tab (`_ActivateDormantSession`'s own line). A window
that left without waking logs `left the wake barrier - <why>`.

**What it does not do.**

- The wake STARTS the claude; whether the turn continues is Claude Code's. With
  `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` (in this machine's `~/.claude/settings.json` since
  2026-10-09) a resumed claude continues an interrupted turn by itself; a turn that died on an API
  error continues through the API-error rewind, which the restored launch applied before the tab
  could start (HOOKS.md *API-error turns*, `resumeApiErrorTurns`, default ON). Without the variable
  the woken claude waits at its box: the tab is up, the question or the error is on screen.
- A Codex tab is started too, but `codex resume` does not continue a turn.
- A window reopened mid-session ("Reopen Windows (N)") wakes its busy tabs the same way. Their state
  is the one they had when the window was closed; a window closed by hand usually delivers its
  claudes' SessionEnd (Done) before the app quits, so usually nothing wakes there.
- A Deactivated (parked) tab whose turn was cut off by the park reads Error; at the next restart
  that Error is "busy at the restart", so it is woken.
- No setting switches it off yet.
- ⚠ `TerminalPage` gained data members (`_restoreWake`, `_restoreWakeTimer`): the next build must
  purge (the build-install skill §5.8). Done: dev 0.0.1.31's Debug build and both builds of af3d40b5e (2026-10-20)
  were purged by force (`TerminalPage.h` is not on the purge-trigger list).

**Live checks owed after a build:** (1) quit with a turn running in a background tab of window 2 →
restart → in hooks.log, the `[restore-wake]` steps per window, the wakes only after every window's
"finished launching" line, and the background tab running again (its claude continues the turn);
(2) a session sitting at an AskUserQuestion at the restart → woken; note what the resumed claude
does with the open question; (3) a restart with nothing busy → each window's "no busy tab to wake
here" line and no wake; (4) busy tabs in several windows → the `[nav] activate-dormant` lines of
ALL windows together keep the pace (≥ 500 ms apart, at most 4 in any 10 s).

## 15. Unreadable state files: a file Agentmaster could not read is never replaced without its bytes kept (2026-10-26)

**The report.** After a portable copy's first-launch "Install just for you" (into `%USERPROFILE%\.agentmaster\bin`,
beside a v0.7.0 release package whose data lived in `%USERPROFILE%\.agentmaster`), the profile looked "reset or deleted
and recreated": the sessions and their titles were gone. The install step was reproduced on e2e (PROFILES.md §2b, flows/19
IS-100–IS-107) and kept every file. The owner then asked: does the install or the launch wipe or override the state, and
what could? The answer, from the code and the experiments below:

| Candidate | Verdict | Evidence |
|---|---|---|
| The install deletes the profile | **No** (v0.7.7–v0.8.0, the releases with the question) | Their only recursive delete (`RemoveShippedEntries`) runs inside the install folder on names the zip itself ships; v0.8.0's 4 e2e scenarios kept every file. Since 5b292ab4a every installer delete is profile-guarded. |
| The installed copy opens another, empty profile | **No** (v0.8.0) | The question runs before the zip copy resolves or exports a profile (`EnsureInstallDecidedAtStartup` precedes `EnsureProfileResolvedAtStartup`), and the installed copy's `profile.path` names the package's own folder (`UserInstallProfileDir`). v0.7.7–v0.7.10 always took the Default profile: wrong only for a package moved to another folder. |
| The package removal deletes state | **No** | Both 0.7.0 and 0.8.0 keep Terminal's own settings in `<profile>\terminal\` (the `AGENTMASTER_PROFILE` redirect), and the removal script stops the old GUI and drains its hosts before the installed copy starts — no two apps write the profile at once. |
| The first launch rewrites the state | **Yes, whenever a state file cannot be read** | Below. |

**The chain.** Every loader fails OPEN: a file that cannot be opened, or whose text does not parse, loads as "nothing
saved" (`ReadAllUtf8` returned `""` without a word for a failed open; `DeserializeSessions` returns an empty fleet for a
failed parse). At startup that is the whole workspace:

1. `sessions.json` reads as an empty fleet (`[restore] 0 session(s) loaded as archived`).
2. Each reopened window's Claude tab refs are unknown, so none is re-homed (`[rehome] … skip … (unknown — fleet not loaded
   / pruned)`).
3. A window left with only its Manager page closes itself and DELETES its record, and the last one discards its record
   (`[manager-only] self-close window …` / `keep last window … (record discarded)`).
4. The next registry save writes the empty fleet over `sessions.json`.

`settings.json` has the same shape: two engine-init seeders (`SeedSessionEnvDefaults`, `SeedClaudeCleanupPeriodDaysIfNeeded`)
and the command reconcile do load → change one field → save, so an unreadable file is rewritten with the defaults at
every start. A session-store record too: the read-modify-write setters read an unreadable record as empty and wrote one
key over the title, tags and favorite.

**What made a file "unreadable" (measured on the parser with a scratch probe):** a UTF-8 BOM (decoded as U+FEFF, which the
parser read as the number 0 with the parse still "ok" — so a BOM'd `sessions.json` was an EMPTY fleet, silently); a file
of NUL bytes (what a power loss can leave — also "the number 0"); a torn or empty file; a NaN or infinity that `json::Dump`
had written as `nan` / `inf` (one such field failed the whole document); a lock by another program at the moment of the
read (an antivirus scan, a backup); a session-store record over 1 MiB (a stored draft has no cap; the read had one).

**The fix — four layers:**

1. **The reader** (`Json.h`): a leading BOM is skipped; a value that starts with no value character is a failed parse (it
   used to be the number 0); `nan`, `-nan(ind)`, `inf`, `-inf`, `NaN`, `Infinity` read as null (the field takes its
   default, the rest of the document survives); `Dump` writes `null` for a non-finite number.
2. **The state-file reader** (`ReadStateFileUtf8`, `StateFileGuard.h`): tells MISSING from UNREADABLE; retries a sharing
   violation, a lock violation or access denied for 400 ms (3 s for the startup fleet read); drops a BOM; logs a file that
   stays unreadable once per 30 s episode (`[persist-read-fail]`), and makes one attempt inside that window. Every loader's
   `ReadAllUtf8` goes through it.
3. **The guard before every replace** (`PrepareStateFileReplace`, called by Persistence's `WriteAllUtf8` and the session
   store's `AtomicWriteUtf8`). It costs one stat while the file is still what this process last wrote or loaded. Otherwise
   (the first replace in a run, or someone else changed the file) it reads the file:

   | The file | The replace |
   |---|---|
   | missing, blank, or valid JSON | goes ahead |
   | not valid JSON | goes ahead after a copy `<name>.could-not-read-<yyyyMMdd-HHmmss>` is kept beside it |
   | marked keep-before-replace (a loader could not use it) | goes ahead after a copy, whatever it holds now |
   | cannot be read (or the copy cannot be made) | REFUSED — the file keeps its bytes; the caller's next save retries (`[persist-guard] … NOT replaced`, once per 30 s) |

   A copy's name ends in no `.json`, so no `*.json` scan (`windows\`, the session store) reads it as live state.
4. **The startup fleet read** (`LoadSessionsForStartup`, `_RestoreClaudeSessions`): when `sessions.json` exists but cannot
   be used, `PreserveWorkspaceForRecovery` copies the workspace BEFORE the restore changes anything — `sessions.json`,
   `open-windows.json`, `settings.json`, `layout.json` and every `windows\*.json` — into `<profile>\recovery\<yyyyMMdd-HHmmss>\`
   with a `README.txt` (what happened, how to put the files back). The load marks `sessions.json` keep-before-replace, so
   a file the snapshot could not copy (a lock) is kept by the first save after the lock ends; one the snapshot did copy is
   marked safe, so the save keeps no second copy. The `[restore]` line names the problem; `[persist-guard]` names the folder.
   The session store's setters read through `ReadRecordForUpdate` (no 1 MiB cap; "could not be read" refuses the write).

**Recovering a workspace** from `<profile>\recovery\<stamp>\`: close Agentmaster, copy the files back into the profile
(replacing), start it. A `*.could-not-read-*` copy is the file's bytes as they were before the replace; rename it back to
look at it, or to restore it.

**On the machine of the 2026-10-26 report** (not inspected — it is not this PC): `hooks.log` in the profile answers which
step happened. Lines from before the install (and the `[install] install begin` line) mean the folder was NOT deleted;
`[restore] N session(s) loaded as archived` gives the fleet the installed copy read — `0` while the older lines show
sessions is the empty-fleet chain above; `[rehome] … skip …` and `[manager-only] …` lines show the window records being
dropped. v0.8.0 logged nothing for a failed open (only an exception reached `[exc] ReadAllUtf8`), so a lock leaves no
line of its own there.

**Limits (open):**

- No on-screen notice: a run that started without its sessions says so in `hooks.log` and in the recovery folder's README,
  but the window only looks empty.
- ~~The tag registries' read-modify-write (`tags.json`, `tag-colors.json`) still reads a locked file as "no registry"~~ —
  closed by §16 (#4): their update reads refuse a file they cannot read.
- Other loaders (window records, `open-windows.json`, `dir-colors.json`, `templates.json`, …) still read an unreadable file
  as empty; the guard covers their writes, and since §16 (#3) every such read marks the file, so the save after the lock
  ended keeps a copy. An unparseable window record is skipped by the reopen and never rewritten.
- Writers outside the two chokepoints are not guarded: the sessions index sidecars and the AI search's `harness\` files
  (both regenerated from the transcripts), the hook files (regenerated at every start). The Telegram bot's
  `messaging\telegram\state.json` goes through the guard since §16 (#5).
- The display read of a session-store record still caps at 1 MiB: a record above it shows no title or tags until it
  shrinks (nothing is lost — the setters read it whole).

Tests: `TestUnreadableStateFiles` (m5 harness): the reader's cases, the state-file reader (missing / BOM / empty / a lock
retried, refused and ridden out / the quiet window), the guard's table on a scratch file, the real `SaveAppSettings` over a
torn `settings.json`, the startup fleet read (fresh / good / BOM / empty list / no list / NUL-filled / locked through its
3 s budget) with the snapshot and the first save after it, and the store's setters (a record over 1 MiB keeps every key; a
torn record is kept aside; a locked record is not rewritten).

## 16. Every other way state could be overwritten: held tab refs, restore-fresh under the same id, the last unguarded writers (2026-10-28)

**The question.** A production swap asked whether to add the 34 files only MSIX packaging adds to a loose layout. That made
the owner ask: "Check if MSI installing overrides or deletes, check if there's ANY other chance we will EVER override
accidentally those files". This section is the answer. It covers every file write and delete in the engine, the UI layer,
the install scripts and Windows Terminal's own settings code.

**Installing (MSI / MSIX) writes no state file.**
- No MSI exists: every release from v0.1.0 to v0.8.0 ships an `.msixbundle`, `Agentmaster.cer` and `.zip` files only.
- An MSIX package runs no install code. `Add-AppxPackage` (a double-click, `Install-Agentmaster.ps1`, `am-update.ps1`) writes
  the package folder under `WindowsApps` and creates the package's own (empty) storage.
- The install scripts write only these:
  - the install folder's pointers: `install.path`, and `profile.path` only when there is none;
  - a NEW `<profile>\package-data\<family>-<stamp>\` before a package removal;
  - copy-if-absent seeds of `<profile>\terminal\`.
- So the risk is in the app: at the first launch after an install, and in every run after it.

**What could still overwrite state, and what does now:**

| # | Where | What happened | Now |
|---|---|---|---|
| 1 | A reopened window's restore could not make a saved tab. Causes: Claude Code not found at that start, a preset whose key is missing from Credential Manager, a pane that failed to build, or a session unknown because `sessions.json` did not load. | A window record is rebuilt from the live strip at every save (`_CaptureWindowRecord`), so the tab left the workspace at the next save. A window that made no tab counted as "Manager-only" and DELETED its record (`_CloseWindowIfManagerOnly`). After the start-up hold, the Telegram bot deleted those sessions' topics as "closed", and `deleteForumTopic` deletes the topic's history with it. A start without Claude Code did this to every window. | **Held tab refs** (`HeldTabRefs.h`; see below). |
| 2 | A restored tab whose transcript was not found (`ClaudeConversationExists`). | Restore-fresh minted a NEW id and removed the old record (`_LaunchClaudeSession`). The tags, the favorite and the Telegram topic stayed on the old id. The lookup runs for every reopened tab at startup, so an unlistable Claude config folder (an unmounted drive, a `CLAUDE_CONFIG_DIR` this process does not see) re-keyed every tab. So did a transcript path past MAX_PATH (a deep working folder's project name), which the plain probe reported missing. | Restore-fresh keeps the record's id (`claude --session-id <id>`, as "Restart session" already did, `BuildClaudeRelaunch`). Claude refuses an id whose transcript exists, so a lookup that missed one fails in the tab instead of replacing anything. The record starts as a fresh session: `Idle`, no turn in flight, no pending question. The lookup probes a long path in the extended-length form (`ResolveClaudeTranscriptPath`). |
| 3 | A loader whose read failed (a lock past the reader's 400 ms: an antivirus scan, a sync client, a backup) built its defaults, and the lock ended before the caller's save. | The guard (§15) found a readable, valid file and replaced it with no copy. | Every Unreadable read through `ReadStateFileUtf8` marks its file keep-before-replace, so the first replace after it keeps a `.could-not-read-<stamp>` copy. |
| 4 | `tags.json` and `tag-colors.json` read-modify-writes. | They read through the display read, which reads a locked file as empty. A lock that ended before the write replaced the registry with one tag, or every picked color with one. | Their update reads (`ReadKnownTagsFileForUpdate`, `ReadTagColorsFileForUpdate`) tell unreadable from missing, and `RegisterKnownTagIn`, `UnregisterKnownTagIn` and `SetTagColorIn` return false and write nothing. |
| 5 | The Telegram bot's `messaging\telegram\state.json`, the only record of which topic belongs to which session. | Its reader answered "" for a locked file too, so the start had no topics, and the first save wrote the empty map over the file. JSON that was not a state object did the same without trying the backup. A file over 16 MiB also read as empty. | `LoadMessagingStateFile` tells the cases apart and falls back to `state.json.prev` for an unreadable, torn or unrecognized file. It marks the file, so its first save keeps a copy, and every save goes through `PrepareStateFileReplace`. |
| 6 | The session store's records (`session-store\<sid>.json`). | A record that EMPTIED was deleted without the guard. A member this version's string store cannot carry (a newer build's) was dropped by any write, with no copy. | The delete goes through the guard (`WriteRecord`). A record holding such a member is marked by `ReadRecordForUpdate` and kept aside before its first write or delete. |

**How a held ref works.**
- **Which refs are held:** `ShouldHoldRestoreRef` holds a launch that made no tab, and a shell whose saved actions throw
  while parsing. It holds an unknown session only when `sessions.json` could not be used at this start
  (`Engine::startupFleetLoadUsable`).
- **Never held:** a duplicate ref, a session another window hosts, a truly unknown session, an empty shell ref.
- **Saves:** `_RestoreWindowTabs` keeps the held refs in `_heldTabRefs`, sorted by record index. `_CaptureWindowRecord` puts
  them back at their old index, clamped (`MergeHeldTabRefs`; the focused shell's index follows), so every save writes them.
- **When a ref is let go:** when its session has a tab again, in this window or in another one (`live`), or when the user
  closes the window. Closing it demotes the whole record to recoverable, like any close.
- **The window:** a window holding refs is never Manager-only (`_IsManagerOnlyWindow`): no self-close, no record delete.
- **The bot:** the window publishes the held sessions (`_PublishHeldTabRefs` → `Engine::NoteWindowHeldSessions`). The bot's
  topic sync keeps a held session's "closed" topic (`ServiceHooks::sessionHeld`, the WINDOW HOLD) for as long as a window
  holds it. Unlike the start-up hold (MESSAGING.md §21.4) it has no time limit.
- **What the user sees** when Claude Code is missing at a start: the windows open without their Claude tabs (each launch is
  `[launch-blocked]`), but no window closes itself, every record keeps its tabs, and the topics stay. The next start with
  Claude Code found brings every tab back.

**Log lines:**
- per held tab: `[rehome] window <id> skip <sid> (the launch made no tab — see the line above) — HELD in the record; the
  next start tries it again`, or `… (unknown — fleet not loaded / pruned) — HELD in the record: sessions.json could not be
  used at this start`;
- per held shell: `[rehome] window <id> skip shell #<n> (its saved actions could not be read) — HELD in the record`;
- in the summary line: `held=N`;
- when a held tab comes back: `[rehome] window <id> let go of held tab ref(s) whose session has a tab again; N still held`;
- from the bot: `[msg] window hold: N topic(s) kept because a window holds their session's saved tab …`, written when N
  changes;
- for a restore without a transcript: `[restore-fresh] <sid> … (no transcript: a fresh conversation under the session's own
  id)`;
- for the bot's state file: `[msg] state.json could not be used (<why>); the previous copy (state.json.prev) was used …`.

**Still open (the owner's call):**
- **An older Agentmaster on this profile.** This happens when a second install is left beside the new one: the zip's
  take-over box is unticked by default while the package is running. When the older version runs, it saves
  `sessions.json`, `settings.json` and the window records without the fields it does not know. Builds before c50fb6468
  also still fail open (§15). Nothing in this build can change an older build's code. A forward-compatible save (unknown
  members kept on every round trip) plus a "written by a newer version" marker would cover the next downgrade.
- **Two instances on one profile.** The second start asks "profile in use — Continue anyway?" (default No). Answering Yes,
  or a headless `-Embedding` start (which never asks), runs both, and each saves its own fleet and window records over the
  other's.
- **Held refs of an unknown session last one run.** After a failed fleet load, the run's saves write the run's own fleet,
  so the next start knows the fleet again and lets those refs go. The §15 recovery folder holds the copy.
- **`settings.json` read-modify-writes and the engine-init seeders** still replace `settings.json` when it read as defaults.
  It is now always with a copy kept (#3).
- **No on-screen notice** of held refs: `hooks.log` only.
- **Claude's own files.** The `~/.claude/settings.json` `cleanupPeriodDays` write re-serializes that file through
  Agentmaster's JSON model: equivalent JSON, but a number past 2^53 loses precision. It runs once, or when the cog's "Keep
  Claude history" changes. `~/.claude.json`'s trust flag is a text splice under Claude's own lock, refused on a file it
  cannot parse.

**Tests (m5 harness):**
- `TestHeldTabRefs`: the hold table, the merge, the published ids.
- `TestEngineHeldSessions`: the engine's held set.
- `TestTranscriptLookupLongPath`: a real transcript past MAX_PATH.
- `TestStateFileOverrides`: the read mark, the tag files' refusals, and the race itself — a lock that ends 150 ms into an
  add or a color pick must keep every tag and color — then the store's guarded delete and foreign member.
- `TestMessagingStateFileLoad`: missing, good, torn, foreign and locked `state.json`.

**Negative controls (m5).** Each of eight fixes was undone, one by one, in an isolated copy (the m5 harness on Q:), and the
check that guards it must FAIL in that run. The eight fixes: the extended-length probe, the read mark, the tag registry's
and the tag colors' update reads, the guarded delete, the focused-shell index, the engine's per-window erase, and the bot's
fallback for a locked file. A check that holds the lock through the whole call missed the tag fix (the guard refuses then
anyway), which is why the race checks exist.

The `TerminalPage` glue (restore, capture, Manager-only, restore-fresh, the Telegram resume's `fresh` flag) is checked with
`cl /Zs` against the Debug PCH. It is not built and not run. The change adds data members to `TerminalPage`, `Engine` and
`MessagingService`, so the next build must purge.
