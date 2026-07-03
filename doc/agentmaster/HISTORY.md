# Agentmaster — Workspace History (snapshots + the History page) — Design Plan

> **Status: DESIGN (pre-implementation, 2026-06-26).** Nothing here is built yet; this is the
> agreed-and-evolving plan. Amended as we finetune — open choices live in **§9 Decisions to
> confirm**. Companions: [`PERSISTENCE.md`](./PERSISTENCE.md) (the live per-window records this
> builds on) · [`SESSIONS.md`](./SESSIONS.md) (the page this re-skins) ·
> [`FAVORITES.md`](./FAVORITES.md) (never-delete sessions) · [`DESIGN.md`](./DESIGN.md).

## 1. Why

[`PERSISTENCE.md`](./PERSISTENCE.md) gives us **current-state** restore: one
`windows/<windowId>.json` per window, **continuously overwritten** on change, holding **refs not
copies** (Option 1 — a Claude tab is just its `sessionId`; title/cwd/branch live in `sessions.json`
and *mutate or vanish*). So there is **no time series** — we cannot answer *"what windows / tabs /
titles did I have open last Tuesday at 3pm?"*. History needs a **new, append-only, denormalized**
store, and a page to browse it.

The load-bearing decision: snapshots are **self-contained denormalized records, not refs**. A
history row must show **what you saw then** — surviving a later rename, close, or transcript purge —
so each snapshot **freezes** the tab's title/color/cwd/branch/state. The `sessionId` is kept
(nullable) *only* to offer Resume/Jump when it still resolves on disk.

## 2. Goals / non-goals

**Goals**
- Record, over time, a faithful **denormalized snapshot** of the whole workspace: every window's
  geometry + ordered tabs (kind, frozen title, color, cwd, branch, state) + focused tab.
- A dedicated **History page** that browses it **time-wise** — rows grouped by **Day / Week /
  Month** — rendering each window as a **stylized WT-window square with clear, multiline tab
  names**, with **search highlight**.
- A pinned **"Last Session"** row at the top = the workspace at last exit (what startup reopens),
  doubling as the recover affordance until it's restored (§4.1).
- Show that a day is **N moments**, with **windows opening and closing** through it — not one static
  layout (§4.2).
- From a history square you can **Resume/Fork/Jump** a still-on-disk session and **reopen a whole
  past window layout** (even one whose live record was purged — reconstruct from the snapshot).

**Non-goals (this pass)**
- Scrollback/buffer history (we snapshot *layout + identity*, not terminal content).
- Cross-machine sync. Editing history. A live "diff two snapshots" view (maybe later).
- Perfect cross-window **atomic** moments — time-proximity grouping is sufficient for browsing (§3.2).

## 3. The recording layer (Part 1)

### 3.1 Schema (denormalized, self-contained)

```
TabSnap {
  kind:      Manager | Claude | Codex | Shell
  sessionId: string?     // managed tabs only — enables Resume/Jump if still on disk; never required to render
  title:     string      // FROZEN at snapshot time (the one-value title, Rule #11)
  color:     "#RRGGBB"?   // the dir color it wore
  cwd:       string?      // FROZEN
  branch:    string?      // FROZEN
  state:     int?         // the SessionState dot color at snapshot time (managed tabs)
}
WinSnap {
  windowId:  string       // stable GUID (same id space as windows/<id>.json)
  geometry:  { x, y, w, h, launchMode }
  focusedIdx: int         // index into tabs; -1 = none
  tabs:      [ TabSnap ]   // ORDERED left-to-right (Manager included, at 0)
  // lensSummary?: { treeScope, selectedId }   // optional, cheap — see §9 Q6
}
```

A *workspace moment* = the set of `WinSnap`s whose timestamps fall in one coalescing bucket (§3.2).

### 3.2 Storage — per-window append, day-sharded

`history/<YYYY-MM-DD>/<unixms>-<windowId>.json`, **one `WinSnap` per file**, written through the
existing `WriteAllUtf8` chokepoint (a failed write logs `[persist-fail]`, like every other state
write). Rationale:

- **Per-window append, not a process fan-out.** Each `TerminalPage` writes its OWN slice on its own
  thread — **zero new cross-thread machinery**, reusing the exact triggers `_FlushWindowRecord`
  already fires on. A "moment" is reconstructed at **read time** by time-proximity (slices within
  one coalescing minute = one moment). Atomicity isn't needed to browse "what did I have open."
- **Day-sharded dirs** keep any one directory small and make **"prune a day"** a single
  `remove_all(history/<day>)`. Enumeration over a time window touches only the relevant day-dirs.
- Same id space as `windows/<id>.json`, so a square can cross-reference the live record / reopen path.

**Window open/close is derivable from the slices**, no extra bookkeeping: for a given `windowId`,
its **first** slice (in a run) marks *opened*, its **last** slice marks *closed* — unless that
window is in the exit set (`open-windows.json`), in which case its last slice is the **exit** (it's
"Last Session", not "closed mid-day"). This is what lets the timeline show windows coming and going
(§4.2) and the "Last Session" row know what restore will bring back (§4.1).

### 3.3 When (cadence) — reuse the existing seams, add a coalescing clock

Build a `WinSnap` by **denormalizing `_CaptureWindowRecord`** (`TerminalPage.AgentWindowRecord.cpp`):
it already walks `_tabs` in order — also read each tab's live title / dir-color / cwd / branch / the
state-dot color. Append a slice on the **same triggers** as `_FlushWindowRecord`, but governed by:

- **Coalesce** ~60 s (trailing) — a burst of edits writes one slice, not dozens.
- **Keepalive** every ~15 min even when idle — so "open at 3pm" is answerable for a quiet workspace.
- **Exit** — one final slice on the close/quit flush (`CloseWindow` / `RequestQuit` / `~TerminalPage`),
  so the last state is always captured. The exit slice of every still-open window **is the "Last
  Session"** content (§4.1).
- **Dedup** — skip a slice byte-identical (by a content signature, the widened `[window-save]`
  tab-signature incl. titles) to that window's **last** slice. An idle workspace → ~zero writes.

Logged once-per-real-change as `[history-snap] <windowId> tabs=N day=<...>` (change-gated, never
per-tick) so the record is auditable alongside `[window-save]`.

### 3.4 Retention

- Keep **full-resolution** for the recent window (default **14 d**), **thin** older slices to
  hourly-then-daily, **hard-cap** age (default **90 d**) pruned on startup (delete whole day-dirs).
- Cadence + retention are `AppSettings` knobs (`historyCoalesceSeconds`, `historyKeepaliveMinutes`,
  `historyFullResDays`, `historyMaxAgeDays`). Defaults reproduce "cheap + 3 months of browsable
  history". Dedup already bounds idle volume to near-zero.

### 3.5 Engine / persistence pieces

- `AgentMaster/Persistence.{h,cpp}` — `SaveHistorySnapshot(WinSnap)`,
  `LoadHistoryDay(day)` / `LoadHistoryRange(fromTs,toTs)` (enumerate day-dirs in range), `PruneHistory`,
  + `Serialize/Deserialize` for `WinSnap`/`TabSnap` (the `Json.h` idiom).
- `SessionModels.h` — `TabSnap` / `WinSnap` structs (plain C++, no WinRT).
- `TerminalPage.AgentWindowRecord.cpp` — `_CaptureWindowSnapshot()` (denormalizing sibling of
  `_CaptureWindowRecord`) + the coalesce/keepalive throttles + the exit-flush hook.
- Standalone tests (`AgentMaster/tests/`): `WinSnap` round-trip, dedup signature, day-bucketing,
  prune-by-age, range enumeration, **moment-merge** + open/close derivation (§4.2).

> *Alternative (deferred):* a process-level engine `SnapshotSink` per window (the activate/restart/
> settings-sink pattern) fanned into one `history/<ts>.json` = truly atomic moments. More plumbing,
> no browsing benefit — only adopt if a future feature needs guaranteed cross-window atomicity.

## 4. The History page (Part 2) — a re-skin of the Sessions page

### 4.1 Layout — a timeline of workspaces

```
History          [ search…........... ]   [Day ▾] [3 months ▾] [↻]
──────────────────────────────────────────────────────────────────────
★ Last Session   exit Jun 26 17:02 · 3 windows       not restored [Reopen all]
  ┌─ win A · 1466×780 ─┐ ┌─ win B ───────────┐ ┌─ win C ─────────┐
  │ ▤ Manager          │ │ ▤ Manager         │ │ ▤ Manager       │
  │ ● Browse sessions  │ │ ● npyiter perf    │ │ ○ pwsh  K:\tools │
  │ ● Fix tab overlay  │ │ ◐ codex audit     │ │ ● Triage fix    │
  └────────────────────┘ └───────────────────┘ └─────────────────┘
──────────────────────────────────────────────────────────────────────
▼ Thu Jun 26, 2026                 3 windows seen · 1 closed · 3 moments
  ◷ 17:02   at exit · 3 windows
      ┌─ win A ─┐ ┌─ win B ─┐ ┌─ win C ─┐
  ◷ 13:40–17:02 · 2 windows                        win D closed 13:40 ✕
      ┌─ win A ─┐ ┌─ win B ─┐
  ◷ 09:14–13:40 · 3 windows
      ┌─ win A ─┐ ┌─ win B ─┐ ┌─ win D ─┐
▼ Wed Jun 25, 2026                        2 windows · 6 moments  [show all]
  ◷ 18:20   last · 2 windows    ┌─ win A ─┐ ┌─ win D ─┐
```

**The "Last Session" row (pinned top) — what restore will bring back.** The top row is always a
synthetic, pinned **"Last Session"** group showing the workspace **as it was at the last app exit**
= the windows the next launch reopens: the `open-windows.json` open-at-exit set, each window drawn
from its **freshest `history/` slice** (the exit slice, §3.3) — or, before any history exists, from
the live `windows/<id>.json` + title resolution (graceful first-run fallback). It is the visual
companion to the startup **"Reopen your N previous windows?"** decide-prompt, and its state tracks
restore:

- **Pending / declined** — the exit windows are NOT live (first paint, or you answered **No**): the
  row stays pinned with a **`not restored`** badge + **[Reopen all]** (and per-square **[Reopen]**),
  wired to the same `RecoverableWindows` / `_ReopenSavedWindow` recover path as the toolbar's "Reopen
  Windows (N)". So the History page's top row **is** the recover affordance.
- **Restored** — once those windowIds are live again this run, the row shows **`restored ✓`** and
  **demotes**: the exit moment now lives in **Today**'s timeline (§4.2) as the latest moment, so
  "Last Session" stops being special exactly as it becomes the current session — *a regular dated
  snapshot*, as requested. (A thin "restored ✓ — see Today" breadcrumb can remain for the run; §9 Q9.)
- **Partially restored** (you reopened some) — pinned; per-square badges reflect which are back.

**Moments — a day is N snapshots; windows open AND close.** A bucket does **not** collapse to
"last-seen per window" (that erases a window you closed at noon). It expands into its **distinct
workspace MOMENTS**:

- **Moment** = a coalesced workspace snapshot (the time-proximity group of §3.2) reduced to **distinct
  shapes**: consecutive moments whose window-set + per-window tab-signatures are unchanged merge into
  one **time-range** ("09:14–13:40"). A quiet day → a few moments, not 50.
- **Open / close** is derived per `windowId` (§3.2): a window's **closing** is marked on its last
  moment (`closed HH:MM ✕`); a windowId first appearing marks an open. The day reads as a narrative
  of what came and went.
- **Day** (default) shows every distinct moment. **Week / Month** collapse harder — one
  **representative moment per day** (default: the at-exit / last moment; §9 Q8) + a **[show all]**
  expander — so coarse grains stay scannable.
- A bucket header summarizes *windows seen · closed · moments*. Squares lay left-to-right (wrap or
  horizontal-scroll, like the Triage Board).

### 4.2 The window-square (stylized WT window, multiline tabs)

A reusable imperative builder (the `_MakeCard` / `_MakeBoardColumn` style in `AgentManagerContent`):

- **Faux title bar** — the window's accent (or neutral), showing `WxH` + the short windowId; a hover
  action: **Reopen this window**. An open/close badge rides here when relevant (`closed HH:MM ✕`).
- **Tab list rendered as ROWS, not a strip** — each tab a **2-line cell**:
  `[● state-dot] [▎ dir-color chip] Title (wrapped, ≤2 lines)` + a dim subline `folder/branch`.
  The Manager tab is greyed/pinned at top; the **focused** tab wears the selection-pill style. This
  is the whole point — tabs are **legible + multiline**, unlike a cramped real strip.
- Fixed width (~220–280px); height grows with tab count, capped with a `+N more` overflow (§9 Q7).
- Visuals reuse `AgentStatusColors.h` (state dot), `GetDirColor`/`AutoDirColorHex` + `PreferDarkTextOn`
  (the color chip + contrast), `AgentTipHelpers` (tooltips).

### 4.3 Time / grouping / search

- **Granularity:** Day / Week / Month, DST-safe **local** buckets via `SessLocalBucket` (already
  powering the Sessions Filter ▸ By Day/Week/Month).
- **Time window:** how far back to LOAD — reuse the Sessions `[1 month]` window cycle + From/To popup.
- **Search + highlight:** reuse `SessionSearch.h` (`ParseSessionQuery` / `MatchesQueryText` /
  `BuildSearchRegex` — AND-terms, `"quoted phrases"`, whole-guid scoping). Match over frozen
  titles / cwds / branches / window+session ids. **Highlight** matched substrings inside the tab
  cells, and **float buckets/moments/windows with hits to the top** (the Sessions relevance-tier
  idea). Non-matching windows dim (kept for context) — *confirm in §9 Q5*.

### 4.4 Actions

- **Click a tab cell** → if its `sessionId` still resolves on disk, the Sessions verbs **verbatim**:
  **Jump** (if open) / **Resume here** / **Fork here** (`_ResumeSessionFromDisk` /
  `_ForkSessionFromDisk` / the activate seam, transcript-gated). If the transcript is gone → the
  cell is shown but its actions are disabled ("not found → view-only").
- **Click the square's title bar** (or **Last Session [Reopen]**) → **Reopen the whole window**: if
  `windows/<id>.json` still exists → `_ReopenSavedWindow(idx)` (the existing recover path); if the
  live record was **purged** → **reconstruct** a transient `WindowRecord` *from the snapshot* and
  reopen it (resume each managed tab + replay shells). History thus **resurrects** a past layout, not
  just views it.
- **Right-click** → copy windowId / session ids / a session's path·branch (the shared
  `CopySessionField` chokepoint).

### 4.5 Page wiring (reuse the Sessions scaffolding)

- New TU **`TerminalPage.AgentHistoryPage.cpp`** (mirrors `TerminalPage.AgentSessionsPage.cpp`),
  declarations in `TerminalPage.h`, registration in `TerminalAppLib.vcxproj`.
- A **"History"** toolbar button in `AgentManagerContent` (beside **Sessions**), opening a
  full-window page **registered via `_RegisterAgentPageOverlay`** (so the tab-switch seam
  `_DismissAgentPageOverlays` closes it) — the same generic page chrome the Sessions/Archive pages
  pioneered: forced **dark theme**, search-box focus-on-show, **Up/Down** row nav.
- Data load **off-thread** (the `_RefreshSessionsRows` pattern): enumerate the day-dirs in the time
  window, parse, **group into moments** (dedup consecutive identical shapes), derive open/close,
  resolve the **Last Session** set from `open-windows.json`, build the row/square model; render on the
  UI thread.

## 5. Reuse map

| Need | Reuse |
|---|---|
| Full-window page, dark theme, overlay registry, Up/Down, focus-on-show | Sessions page + `_RegisterAgentPageOverlay` / `_DismissAgentPageOverlays` |
| Search semantics + highlight | `SessionSearch.h` (`ParseSessionQuery` / `MatchesQueryText` / `BuildSearchRegex`) |
| Day/Week/Month bucketing (DST-safe, local) | `SessLocalBucket` |
| Capture a window's live tab list | denormalize `_CaptureWindowRecord` → `_CaptureWindowSnapshot` |
| "Last Session" exit set | `open-windows.json` (`LoadOpenWindows`) + the exit-slice content |
| Resume / Fork / Jump a tab | `_ResumeSessionFromDisk` / `_ForkSessionFromDisk` / the activate sink |
| Reopen a whole window | `RecoverableWindows` / `_ReopenSavedWindow` + snapshot→record reconstruction |
| Tab visuals (dot / chip / contrast / tips) | `AgentStatusColors.h`, `GetDirColor`, `PreferDarkTextOn`, `AgentTipHelpers` |
| Copy actions | `AgentCopyActions.h` (`CopySessionField`) |
| State write chokepoint + fail surfacing | `WriteAllUtf8` (`[persist-fail]`) |

## 6. Touch map (where the code lands)

- `AgentMaster/SessionModels.h` — `TabSnap` / `WinSnap`.
- `AgentMaster/Persistence.{h,cpp}` — history IO (`SaveHistorySnapshot` / `LoadHistoryRange` /
  `PruneHistory` / (de)serialize), the `history/` day-sharded layout.
- `TerminalPage.AgentWindowRecord.cpp` — `_CaptureWindowSnapshot` + coalesce/keepalive/exit triggers
  + dedup signature.
- `TerminalPage.AgentHistoryPage.cpp` *(new)* — the page (Last Session row, moments, squares,
  granularity, search, actions).
- `TerminalPage.h`, `TerminalAppLib.vcxproj` — declarations + TU registration.
- `AgentManagerContent.{h,cpp}` — the **History** toolbar button + open handler.
- `AgentMaster/AppSettings` (`settings.json`) — the cadence/retention knobs (§3.4).
- `AgentMaster/tests/` — snapshot round-trip / dedup / bucketing / prune / range / moment-merge tests.
- Keep the diff **additive** (new files + small touches at integration points) — the Agentmaster
  rebase convention.

## 7. Phases

- **P1 — Record** (the only real new engine work): `WinSnap`/`TabSnap` + `history/` IO +
  `_CaptureWindowSnapshot` off the existing capture + coalesce/keepalive/exit triggers + dedup +
  prune. Standalone-testable end-to-end with no UI.
- **P2 — Page (read-only):** the **Last Session** pinned row + the **Day → Moments → Squares**
  timeline (distinct moments, time-ranges, open/close badges) + window-squares + granularity + time
  window + search highlight. Browsing only.
- **P3 — Actions:** per-tab Resume/Fork/Jump; the **Last Session / per-square Reopen** affordances;
  whole-window reopen incl. **purged-window reconstruction** from the snapshot.
- **P4 — Polish:** per-window **filmstrip scrubber** (scrub a window across a merged moment-range),
  retention thinning, cadence/retention settings UI, optional **pin a snapshot**, optional
  diff-two-moments.

## 8. Correctness rules (extend the project rules)

1. **Snapshots are immutable + denormalized.** Once written, a slice is never rewritten; it must be
   renderable with **no** lookup into `sessions.json` (titles/cwd/branch are frozen in it). The
   `sessionId` is an *optional* affordance, never a render dependency.
2. **History never auto-launches or mutates a session.** Browsing reads only; Resume/Fork/Jump and
   the Last Session / window reopen go through the existing transcript-gated reopen seams (Rule #6).
3. **One profile (Rule #15).** `history/` lives under the ACTIVE profile (`AgentmasterStateDir()`),
   like every other state dir; pruning/IO go through `WriteAllUtf8` / the standard helpers.
4. **Cheap by default.** Recording must be ~free at idle (dedup + coalesce); capture is read-only
   over live tab state, never blocking the UI thread for IO (writes are best-effort, off the
   critical path).
5. **"Last Session" reflects the manifest, not a guess.** It is exactly the `open-windows.json`
   open-at-exit set (PERSISTENCE.md), so it can never disagree with what startup actually reopens.

## 9. Decisions to confirm (finetune here)

1. **Recording shape:** per-window append + time-proximity grouping *(recommended — zero new
   cross-thread plumbing)* vs. engine fan-out atomic snapshots (§3.2 / §3.5 alternative)?
2. *(resolved → moments)* A bucket renders **distinct workspace moments** (time-ranged), not
   last-seen-per-window — so windows opening/closing through the day are visible (§4.1/§4.2).
   **Remaining:** the **moment-merge signature** granularity — merge while the *window-set + tab
   set + titles* are unchanged, or also on cwd/branch/state changes? (Looser = fewer moments.)
3. **Defaults:** coalesce **60 s**, keepalive **15 min**, full-res **14 d**, hard-cap **90 d** —
   acceptable? Tune?
4. **Purged-window reopen:** reconstruct a whole window from its snapshot in **P3**, or keep reopen
   view-only (only windows whose live `windows/<id>.json` still exists)?
5. **Search non-matches:** dim-but-keep for context, or hide entirely?
6. **Lens in the snapshot:** capture the Manager lens summary (scope/selection) per `WinSnap`
   (enables a fuller reopen), or skip (tabs-only) for v1?
7. **Square density:** fixed tab-cell height + `+N more` overflow, or fully expand every tab?
8. **Week/Month representative moment:** the **at-exit / last** moment of each day, or the **peak**
   (most windows)?
9. **Last Session after restore:** fully demote into Today, or keep a thin pinned **`restored ✓`**
   breadcrumb for the rest of the run?
10. **Session boundaries:** is "Last Session" only the *single* most-recent prior run, or do we also
    want per-run separators in the timeline ("Session of Jun 26 09:00–17:02")?
