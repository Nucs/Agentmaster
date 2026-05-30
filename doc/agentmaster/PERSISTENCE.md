# Agentmaster — Workspace Persistence & Restore (Design Plan)

> **Status:** Proposed (design agreed; not yet implemented). Supersedes the flat
> session-only persistence shipped in **M8** (see `IMPLEMENTATION.md`).
> Scope: persist and restore the **whole workspace** — every window, its tabs (order,
> color, title), panes, window geometry, the per-window **Manager tab** state, and the
> live **Claude sessions** inside them — so first launch of the process reopens exactly
> the state the user left.

## 1. Why

Today persistence is **session-only and window-blind**: `_InitAgentmasterEngine()`
autosaves a single flat `sessions.json` on every registry change
(`TerminalPage.cpp:743`), and `_RestoreClaudeSessions()` re-launches that flat list on
startup. Two facts break this the moment there is more than one window:

- **One process, many windows.** v1.24 uses the **WindowEmperor** model: launching a
  second `WindowsTerminal.exe` hands its command line to the existing instance and exits
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
  manager:   { selectedId, scopeDir, selectedPromptId, collapsedDirs[], splitterFractions }
}

TabEntry =
  | Claude { convId, workingDir, title, tabColor?, flightPlan[], autopilot }   // ours
  | Other  { actionsJson }   // WT ActionAndArgs (carries its own NewTab/SetTabColor/RenameTab)
```

Stored under `%USERPROFILE%\.agentmaster\windows\<windowId>.json` (one file per window so
closes/opens don't contend on a single document). The flat `sessions.json` is retired
(migrated — §10).

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

### 5.5 The pinned Manager tab

Today `_OpenAgentManagerTab()` force-creates the Manager tab at index 0 in
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
   window with just its (pinned, non-closable) Manager tab.
3. A one-time **"Restore your previous layout?"** prompt offers to bring the workspace back.
   Decision: it **offers all archived sessions**, and once it can read the `WindowRecord`s it
   restores **per window** (geometry + Manager lens + that window's tabs in order — `Claude`
   via `_LaunchClaudeSession(..., resume=convId)`, `Other` via WT action replay). Dismiss → the
   sessions stay in the **Archived** list, restorable **per-tab** anytime. *(Held until the
   WindowRecord capture/restore is wired — §6a.)*
4. As each restored Claude `SessionStart` arrives, the engine re-attaches that `convId`'s
   Flight Plan + autopilot from the record (correlate by id — window-scoped).

**Close window.** Tear down the window's tabs → each ConPTY closes → its `claude.exe` job
exits. The record is already current (debounced autosave), so nothing else to do — the
window is restorable. App keeps running.

**Close app.** Flush any pending debounce; tear down all windows (processes exit). All
records remain → full restore next launch.

**`Kill` (destructive).** Remove the session from its `WindowRecord.tabs` + drop its
side-data, then close its tab (`_KillClaudeSession`, `TerminalPage.cpp:1008`). A record with
no tabs left is deleted. Add **"Kill all sessions in this window."**

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
  window grouping. **Already shipped** (`_RestoreArchivedSession` / the Archived overlay).

So **per-window is the coarse "restore my workspace" unit (the launch prompt); per-tab is the
fine-grained cherry-pick (the Archived button), always available.** A session restored per-tab
re-homes into the current window; window↔session affinity (the `WindowRecord` tab ref) is
advisory until full per-window restore lands.

**Locked decisions for the launch prompt:**
- It **offers all archived sessions** (not only those open at last close — so no `openAtExit`
  marker is needed).
- It is **held until the full per-window `WindowRecord` capture is wired first** — so when it
  ships it restores true **per-window** layouts rather than a flat global list. Until then,
  restore is **per-tab only** (the Archived list); startup stays clean (just the Manager tab,
  Rule #6).

## 7. Correctness rules (extend `IMPLEMENTATION.md` §"Correctness rules")

6. **Restore = resume, not replay.** Claude tabs restore via `claude --resume <convId>`
   with the full per-session env; `Sent` prompts are never re-sent. (Unchanged in spirit;
   now window-scoped.)
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
  loads everything **Archived**; closing a tab archives it; the global **Archived** overlay
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
  Archived overlay restores sessions, which **re-home** to their origin window. Delivers
  "close == reopen your workspace layout" without relaunching any claude.
- **Lite:** if multi-window restore is *not* wanted, the plan collapses to **single-window
  geometry+lens** (Phases A, B, C-lite, E — skip the Emperor multi-window work). Much smaller.

Phases A–B are identical either way, so coding starts before this is locked.

### 13.1 Phases

**Phase A — Identity + lens exposure (additive, low-collision).**
- `TerminalPage::_windowId` (a GUID `hstring`), minted once at engine init; a restored window
  adopts its record's id instead (Phase C).
- `AgentManagerContent::GetManagerState()` → `ManagerState` (selection / scopeDir /
  selectedPromptId / collapsed dirs / `_layout`) — a pure getter over existing members.
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

**Increment 3 — remaining (refinements):**
- **Open-at-exit manifest** (retention). `WM_CLOSE_TERMINAL_WINDOW` fires for BOTH a user closing one
  window and app-exit closing each window, so without a manifest the reopen set = *every record ever*
  (a window you closed mid-session lingers and the prompt re-offers it). The prompt mitigates (you
  see the count + can decline), but for an accurate "windows open at last exit" set the Emperor must
  record open window ids at the exit seam (`_persistState`/quit) — which needs `windowId` exposed on
  the projected `TerminalWindow` surface. Until then, the prompt's N counts all records.
- **Manager "Reopen Windows (N)" recover button** (the "if I answered No" path). A toolbar button
  (next to **Archived**) that reopens saved-but-not-open windows from inside a running session — the
  runtime analog of the Emperor loop (dispatch `wt -w new -s <idx>` per not-open record via the
  new-window path). Needs the Manager→Emperor new-window-with-`-s` request + the open-set (manifest).
- **`Other`-tab `actionsJson` capture** (non-Claude tab recreation) and session **re-home** (route a
  restored session into the window whose record references it) — both still deferred.

**Acceptance (§13.3) status:** #2 partial (lens), #3 (single-window unchanged; clean first run), and
**#1 multi-window reopen at geometry/lens — DONE & verified** (the manifest/button above refine
*which* set reopens + the in-session recover path, not the reopen mechanism itself).
