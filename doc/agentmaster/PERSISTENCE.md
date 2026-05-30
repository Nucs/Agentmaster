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
- First launch of the process **restores everything** (resume each Claude conversation).
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

**First launch (restore).**
1. Emperor starts → create the **singleton engine** (one registry + one pipe + scheduler).
2. Load every `windows/<id>.json`.
3. For each record (in saved order): create a window at its `geometry`; add the Manager tab
   at index 0 seeded with its lens; then recreate `tabs` **in order** — `Other` via WT
   action replay, `Claude` via `_LaunchClaudeSession(..., resume=convId)`.
4. As each Claude `SessionStart` arrives, the engine re-attaches that `convId`'s Flight Plan
   + autopilot from the record (correlate by id — same as M8 restore, now window-scoped).

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
