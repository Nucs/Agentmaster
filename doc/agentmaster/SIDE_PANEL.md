# The SIDE PANEL — a window-chrome fleet pane beside the terminal

A non-terminal, non-tab panel docked LEFT or RIGHT of the tab content — part of the window
itself (every tab shares it), giving the fleet + the current session a permanent home next to
whatever terminal you are working in. It deliberately REUSES the Manager tab's and the per-tab
overlay's components, so it carries their business logic verbatim instead of a parallel copy.

## 1. Layout

```
┌ tab row ──────────────────────────────────────────────┐
├────────────────────────────────┬──┬────────────────────┤
│                                │▐ │ [search][sort][⇄][»]│  ← header row
│        terminal content        │▐ │ N of M open sessions│  ← count line
│         (TabContent)           │▐ │ ┌────────────────┐ │
│                                │▐ │ │ session CARDS  │ │  ← TOP area (own scroll)
│                                │▐ │ └────────────────┘ │
│                                │▐ │ ━━━━━━━━━━━━━━━━━━ │  ← horizontal splitter
│                                │▐ │ ┌────────────────┐ │
│                                │▐ │ │ selected sess. │ │  ← BOTTOM area (own scroll)
│                                │▐ │ │    SUMMARY     │ │
│                                │▐ │ └────────────────┘ │
└────────────────────────────────┴──┴────────────────────┘
                                  ↑ vertical splitter (the resizable separator)
```

- `TerminalPage.xaml`'s content row became a 3-column `ContentArea` grid; code
  (`_SidePanelApplyDock`) assigns TabContent / splitter / panel to the columns per the GLOBAL
  dock side, so dock-left is a pure column rotation. Both extra columns are 0 until
  `_InitAgentSidePanel` runs, so a build without the panel renders the pre-feature layout.
- The whole panel forces the Agentmaster DARK theme (explicit fills, `RequestedTheme(Dark)`).
- Collapse (the header chevron / the strip) shrinks the panel to a 20px expander strip; the
  header also carries a `⇄` dock-side flip.

## 2. TOP — the session cards (the Triage-Board integration, vertical)

- **Data**: `SessionRegistry::SnapshotLive()` (the whole fleet — GLOBAL, both kinds), rebuilt
  off the page's already-batched registry drain, min-gap throttled (500ms), fully deferred
  while collapsed (the Manager-lens rebuild-tax discipline). The ~30s "ago" refresh rewrites
  the timing TextBlocks IN PLACE — never a rebuild for a clock tick.
- **Search** (top row): the Sessions browser's grammar reused — `ParseSessionQuery` +
  `MatchesQueryText`/`MatchesPathQuery` over title / effective dir / branch / id;
  whitespace-split AND terms, "quoted phrase" exact, a bare whole GUID matches the session +
  its forks by IDENTITY.
- **Order** (top row): the SHARED `AppSettings::boardSort`, cycled through
  `AgentManagerContent::CycleBoardSort` (a new public wrapper over the board's own
  `_CycleBoardSort`) and applied with the board's own `MakeSortKey`/`SortKeyLess` — one sort
  spec, two surfaces, zero drift.
- **A card**: title on a band painted the session's `ResolveSessionColorHex` color
  (WCAG-contrast text), state dot + `StateLabel` in the shared `AgentStatusColors` palette, a
  teal `codex` pill, the pending-draft `BuildPendingDots` cluster, the `TimingText` adornment,
  and a dim `leaf / branch` line.
- **Interactions** (Linked Lenses): click = select (drives `AgentManagerContent::SelectSession`,
  so the board/tree/Auto-Testing follow); double-click = `_ActivateClaudeSession` (jump to the
  tab, cross-window); right-click = **the** session context menu —
  `AgentManagerContent::BuildSessionMenuFor` (a public wrapper over `_MakeLazySessionMenu`), the
  exact lazily-filled flyout the board card shows. Selection flows BOTH ways: the tab-switch
  funnel (`_SyncManagerSelectionToTab`) and the lens-changed push both select the card here,
  with same-id early-outs breaking the echo.

## 3. BOTTOM — the selected session's summary (a fill-hosted AgentTabOverlay)

The bottom area hosts a REAL `AgentTabOverlay` whose summary panel is switched into the new
**fill-host mode** (`SetSummaryFillHost`, called before `Initialize`): the per-tab pencil
panel's entire pipeline — off-thread `AnalyzeSessionTranscript` + `RenderSummaryBox`/
`RenderCodexSummary`, the live times bar, the wrap/truncate/previous toggles, the JUMP buttons,
the context menus — reused verbatim, with three differences:

- it stretches to the HOST slot (fractions + the 50%-of-pane cap ignored; the body viewport's
  MaxHeight tracks the slot height minus the panel's own chrome, change-gated + re-applied on a
  deferred tick so it can never layout-cycle);
- the resize GRIPS are never built (the panel's splitters own sizing; a grip's drag-persist
  would corrupt the per-tab GLOBAL fractions);
- it is never dimmed (`SetOverlayOpacities(1,1)`) and always enabled — independent of the
  per-tab pencil global.

It is DELIBERATELY not in `_claudeOverlays` (that map is keyed by session id and may already
hold the session's own per-tab overlay), so `_ToggleSummaryWrap`/`Truncate`/`Previous` and the
settings broadcast push the flags to it explicitly (`_SidePanelApplySettings`). Rebinding to a
new session replaces the overlay (the old one's registry observer detaches on release); a
same-id rebind just kicks the cheap mtime-gated `RefreshSummaryContent`.

## 4. Persistence — per window, last-one-wins defaults

The user contract: **both separators persist PER WINDOW (not per tab), and the last one to
resize sets the default for new windows.** Collapse follows the same rule; the dock side is
GLOBAL-only.

| state | per-window (windows/<id>.json) | global default (settings.json) |
|---|---|---|
| panel width | `WindowRecord::sidePanelWidthFraction` (0 = unset) | `AppSettings::sidePanelWidthFraction` (0.22) |
| cards/summary split | `WindowRecord::sidePanelSplitFraction` (0 = unset) | `AppSettings::sidePanelSplitFraction` (0.5) |
| collapsed | `WindowRecord::sidePanelCollapsed` (-1 = unset) | `AppSettings::sidePanelCollapsed` (false) |
| dock side | — | `AppSettings::sidePanelOnLeft` (false = right) |

- Every drag release / collapse toggle: update the live members → the window-record autosave
  captures them (`_CaptureWindowRecord`) → a GRANULAR freshest-disk RMW exports ONLY the
  adjusted field as the new global default (`_SidePanelPersistDefaults` — a width drag never
  exports this window's collapse state).
- A new window (or a pre-feature record's unset field) seeds from the globals at
  `_InitAgentSidePanel`; a window that made its own choice keeps it.
- Bands everywhere (drag clamps == load-time checks): width (0.10, 0.50], split (0.15, 0.85).
  A record's out-of-band value degrades to UNSET (the global default); a settings out-of-band
  value degrades to the shipped default — a corrupt file can never collapse the terminal.
- All four AppSettings fields are written OUTSIDE the cog form, so BOTH cog-Save preserve
  blocks (content + page sink) restore them from disk.

## 5. Perf + safety rules upheld

- Rebuilds: batched-drain trigger + 500ms min gap + collapsed-defers + in-place timing refresh;
  no eager flyouts on cards (menus built at right-click); `SnapshotLive` only; the per-card
  color resolve rides the cached `LoadDirColors`.
- Layout mutations from layout-driven triggers (ContentArea/root SizeChanged) go through
  coalesced dispatcher ticks with change-gated writes (the E_LAYOUTCYCLE lesson); pointer-drag
  applies synchronously (input, not layout).
- Card hover handlers reach the card only via `sender` — capturing the element in its own
  handler is a self-cycle that leaks a card per rebuild (the board's weak-capture lesson).
- The periodic tick is a terminate-netted `fire_and_forget` over an awaitable Impl
  (`resume_foreground` first), the `_ScanCacheWarmTabs` idiom.
- Tooltips via `AgentSetTip`/`AgentSetTitledTip` only (the per-element ToolTip leak rule).

## 6. Files

- `TerminalPage.AgentSidePanel.cpp` (new TU; includes `AgentManagerContent.Internal.h` for the
  board's shared helpers), `TerminalPage.xaml` (the ContentArea wrapper), `TerminalPage.h`
  (members + decls), seams in `TerminalPage.cpp` (`_OnFirstLayout`),
  `TerminalPage.AgentEngine.cpp` (drain / liveness / funnels / sink / broadcast),
  `TerminalPage.AgentObserver.cpp` (toggle loops), `TerminalPage.AgentWindowRecord.cpp`
  (capture), `AgentManagerContent.h` (the two public wrappers),
  `AgentManagerContent.Settings.cpp` (cog preserve), `AgentTabOverlay.{h,Summary.cpp}`
  (fill-host mode), `AgentMaster/SessionModels.h` + `Persistence.cpp` (fields + JSON),
  `tests/tests_persistence.cpp` (round-trip / defaults / bands).
