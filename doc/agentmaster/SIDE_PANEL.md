# The SIDE PANEL — a window-chrome fleet pane beside the terminal

A non-terminal, non-tab panel docked LEFT (the default) or RIGHT of the tab content — part of
the window itself (every tab shares it), giving the fleet + the current session a permanent home
next to whatever terminal you are working in. It deliberately REUSES the Agent Manager page's and the
per-tab overlay's components, so it carries their business logic verbatim instead of a parallel copy.

## 1. Layout

Shown in the DEFAULT dock (LEFT); the header's `⇄` flips it to the mirror image (dock RIGHT),
which also flips the collapse chevron (`«` here — it always points toward the docked edge).

```
┌ tab row ─────────────────────────────────────────────────┐
├──────────────────────┬──┬────────────────────────────────┤
│[LOCAL][sort][⌖][⇄][«]│ ▌│                                │  ← header line 1 (right-aligned): card SCOPE /
│                      │ ▌│                                │    order / scroll-to-tab / dock / collapse
│ [filter sessions…]   │ ▌│                                │  ← header line 2: the filter box, full width
│ N of M (this window) │ ▌│        terminal content        │  ← count line (names the scope)
│ ┌──────────────────┐ │ ▌│          (TabContent)          │
│ │ session CARDS    │ │ ▌│                                │  ← TOP area (own scroll)
│ └──────────────────┘ │ ▌│                                │
│ ━━━━━━━━━━━━━━━━━━━━ │ ▌│                                │  ← horizontal splitter
│ ┌──────────────────┐ │ ▌│                                │
│ │ selected sess.   │ │ ▌│                                │  ← BOTTOM area (own scroll)
│ │    SUMMARY       │ │ ▌│                                │
│ └──────────────────┘ │ ▌│                                │
└──────────────────────┴──┴────────────────────────────────┘
                         ↑ vertical splitter (the resizable separator)
```

- `TerminalPage.xaml`'s content row became a 3-column `ContentArea` grid; code
  (`_SidePanelApplyDock`) assigns TabContent / splitter / panel to the columns per the GLOBAL
  dock side (`AppSettings::sidePanelOnLeft`, LEFT by default), so either dock is a pure column
  rotation — LEFT = `[panel][splitter][TabContent *]`, RIGHT = its mirror. Both extra columns are
  0 until `_InitAgentSidePanel` runs, so a build without the panel renders the pre-feature layout.
- The whole panel forces the Agentmaster DARK theme (explicit fills, `RequestedTheme(Dark)`).
- Collapse (the header chevron / the strip) shrinks the panel to a 20px expander strip; the
  header also carries a `⇄` dock-side flip, the card list's **LOCAL / GLOBAL** scope toggle (§2),
  and the `⌖` **scroll-to-selected-tab** icon toggle (§2a).
- The header is **two lines** (one 2-row grid): the buttons, right-aligned in their original order,
  over the filter box, which spans every column. Sharing one line with the buttons, the box was
  squeezed toward its minimum on a narrow panel while the buttons kept their natural widths.

## 2. TOP — the session cards (the Triage-Board integration, vertical)

- **Data**: `SessionRegistry::SnapshotLive()` (both kinds), narrowed by the **card scope**:
  **LOCAL by default — only the sessions whose tab is hosted in THIS window**
  (`TerminalPage::_HostedSessionIds()`, the live `_claudeTabs` set — the SAME predicate the
  Explorer Tree's LOCAL scope provider reads, so the two surfaces can never disagree about what
  "here" means); the header's **LOCAL / GLOBAL** toggle (`_SidePanelToggleScope`) widens THIS
  window's list to every window's sessions. It is a SEPARATE state from the Manager's tree/board
  scope (they answer different questions — the fleet lens vs. what sits beside your terminal) and
  is persisted **per window only** (§4). Rebuilt off the page's already-batched registry drain,
  min-gap throttled (500ms), fully deferred while collapsed (the Manager-lens rebuild-tax
  discipline) — plus, while LOCAL, a per-tick **hosted-set diff** (`_SidePanelPeriodicTickImpl`):
  a tab docked into / torn out of the window changes `_claudeTabs` with NO registry notify (the
  moved session stays live + unchanged), so the drain trigger alone would leave a LOCAL list
  stale. The ~30s "ago" refresh rewrites the timing TextBlocks IN PLACE — never a rebuild for a
  clock tick. The count line names the scope (`N of M sessions in this window` / `… across all
  windows`), so a short list reads as "this window", not "the fleet shrank".
- **Search** (the header's second line, full width — slim: FontSize 11 over a ~22px floor, the
  TextBox template's stock 32px lifted for this box alone by overriding `TextControlThemeMinHeight`
  in its own `Resources`, since a `MinHeight` on the control can't lower a template part's floor):
  the Sessions browser's grammar reused — `ParseSessionQuery` +
  `MatchesQueryText`/`MatchesPathQuery` over title / effective dir / branch / id;
  whitespace-split AND terms, "quoted phrase" exact, a bare whole GUID matches the session +
  its forks by IDENTITY.
- **Order** (header, first line): the SHARED `AppSettings::boardSort`, cycled through
  `AgentManagerContent::CycleBoardSort` (a new public wrapper over the board's own
  `_CycleBoardSort`) and applied with the board's own `MakeSortKey`/`SortKeyLess` — one sort
  spec, two surfaces, zero drift. The cycle is MOST ACTIVE · **MOST RECENT** · NEWEST · OLDEST ·
  A–Z. **MOST RECENT** orders by the newest **turn boundary** only — `SessionInfo::turnStartUnixMs`
  (the user's last prompt SENT: a prompt-carrying `UserPromptSubmit`, the scanner's pull-echo of a
  real prompt line, or the observer's transcript-tail read) and `turnEndUnixMs` (the assistant's
  last turn END: a real `Stop`, a terminal `stop_reason` line, or the `[Request interrupted…]`
  marker), every writer a monotonic max and both persisted — so mid-turn tool calls, subagent /
  teammate side-file writes, and the SessionStart / triage stamps that make MOST ACTIVE jumpy never
  move it, and there is no Running pin (a running session ranks by when its turn was sent). A
  session with no turn evidence yet falls back to the hook decay anchor so it isn't stranded at
  the bottom. The pure extractor is `TurnBoundariesFromTranscriptChunk` (unit-tested).
- **A card**: title on a band painted the session's `ResolveSessionColorHex` color
  (WCAG-contrast text), the session's **bookmark-tag ribbons in the band's top-right corner**
  (§2b), state dot + `StateLabel` in the shared `AgentStatusColors` palette, a
  teal `codex` pill, the pending-draft `BuildPendingDots` cluster, the `TimingText` adornment,
  and a dim `leaf / branch` line.
- **Interactions** (Linked Lenses): click = select (drives `AgentManagerContent::SelectSession`,
  so the board/tree/Auto-Testing follow); double-click = `_ActivateClaudeSession` (jump to the
  tab, cross-window); right-click = **the** session context menu —
  `AgentManagerContent::BuildSessionMenuFor` (a public wrapper over `_MakeLazySessionMenu`), the
  exact lazily-filled flyout the board card shows, **anchored on the card** (its optional `anchor`
  argument, held weakly) so the menu's **Tags** item opens the tag panel right under the card,
  as on the board (without an anchor it opened at the window's top-left). Selection flows BOTH
  ways: the tab-switch funnel (`_SyncManagerSelectionToTab`) and the lens-changed push both
  select the card here, with same-id early-outs breaking the echo.
- **The tab strip follows a click only on the Agent Manager page.** While the Manager page is on screen, a
  card click also scrolls the tab strip to that session's tab — like a board card click, and even
  on a RE-click of the already-selected card (`_RevealClickedSessionTab`; the same-id early-outs
  above would otherwise swallow it). On any other tab a click is a PEEK (§3) and never moves the
  strip. A double-click's jump selects the tab, and the selection funnel scrolls it into view
  (`_RevealTabInStrip`, virtualization-aware — the strip derealizes far tabs and a bare
  `StartBringIntoView` can't reach them), including the tab you are already on, whose strip you
  may have scrolled away.

### 2a. The `⌖` toggle — selecting a tab scrolls the list to its card

The header's `⌖` icon toggle (a real `ToggleButton` between the sort button and `⇄` — the Sessions
page's scope-toggle idiom, so its accent fill *is* the on/off state) makes **selecting a tab scroll
the card list until that tab's card is on screen**. It is a GLOBAL preference,
`AppSettings::sidePanelScrollToSelectedTab`, **ON by default**.

- **What scrolls it.** A tab selection — the one funnel (`_OnTabSelectionChanged` →
  `_ApplyAgentTabSelection` → `_SyncManagerSelectionToTab`, so a tab click, Ctrl+Tab, `switchToTab`,
  a card double-click's jump and a close successor all count) calls `_SidePanelRevealTabCard(id)`,
  even when the panel already had that card selected (you peeked it, scrolled away, then picked its
  tab). And a terminal touch that ENDS a peek (`_SidePanelFollowFocusedTab` moving the selection back
  to your tab — §3) scrolls back to your tab's card; a terminal touch that changes nothing never
  scrolls, so clicking into the terminal can't snap back a list you scrolled by hand.
- **What never scrolls it.** A rebuild (they follow every fleet change and would fight your own
  scrolling), a card click (the card is already where you clicked it), a Manager-side selection (the
  lens push), and the Agent Manager page itself (no tab is selected there).
- **Minimal and one-shot.** The scroll is `UpdateLayout()` + `StartBringIntoView()` on the card — the
  Manager board's own `BringSelectedIntoView` recipe — so a card already fully visible never moves
  and a far one stops at the nearest edge. Keyboard focus never leaves the terminal.
- **An owed reveal.** `_SidePanelRevealTabCard` records the id (`_sidePanelRevealPendingId`) and
  queues ONE coalesced Low-priority tick (`_SidePanelQueueReveal` → `_SidePanelRevealPendingNow`), so
  rapid Ctrl+Tab cycling folds into one scroll to the LAST tab, and the tick measures the list after
  whatever rebuild/content swap ran first. The debt is consumed by the first tick that finds the card
  **rendered**, and survives until then: a just-launched session's card (it lands on the NEXT
  rebuild — `_SidePanelRebuildCards` re-queues), a collapsed panel (the expand re-queues), a card the
  filter hides (paid when the filter admits it).
- **Cancelling it.** A card click (the user's own pick outranks a pending scroll — cleared in
  `_SidePanelSelectSession` when `driveManagerLens`), selecting a tab with no card (pwsh / cmd /
  external), and turning the toggle off (here, or by another window's broadcast) all drop the debt,
  so a stale reveal can never yank the list to a tab you have already left.
- **Persistence + reach.** The click is a freshest-disk RMW of that ONE field (the `⇄` recipe) plus a
  LIVE `BroadcastSettingsChanged` of the struct just read from disk — never this window's copy, which
  can be stale on other out-of-cog fields — so every open window re-pins its toggle at once
  (`_ApplyBroadcastSettings` → `_SidePanelApplySettings` → `_SidePanelUpdateScrollToTabButton`).
  Turning it ON scrolls straight to the focused tab's card when that card is the selected one (a live
  peek of another card is left alone).

### 2b. Bookmark tags — ribbons in the title bar's top-right corner

A tagged session's card shows its **bookmark tags as ribbons in the title band's top-right corner**,
hung from the band's top edge. The band becomes `[ title * ][ tag strip Auto ]`: the strip takes the
width its ribbons need and the title takes the rest, trimming with its ellipsis, so a long title can
never push the tags off the card. An untagged card's band is exactly the old title-only band.

```
┌──────────────────────────────┐
│ Fix the flaky login…   ▮▮▮▮+2│  ← title band (folder color); ribbons hang from its top edge (6 tags: 4 + "+2")
│ ● Waiting  -2m/12h/-3m       │
│ Agentmaster / main           │
└──────────────────────────────┘
```

- **Top-right on the bar, not below it** (the ask). The Triage-Board card hangs its ribbons out of
  the band's BOTTOM edge (the tab badges' "bookmark out of the book" look); here they stay ON the
  bar, where the eye already reads the card's name and folder color, and the compact card body
  (a state row and a dir line, 10px text) keeps its spacing.
- **The same ribbon as everywhere**: the 6.5×9.3 swallowtail `Polygon` with a thin black
  outline (`SidePanelMakeTagRibbon`), in the order the tags were added, colored through the ONE
  `ResolveTagDisplayColor` (a user-picked `tag-colors.json` color, else the stable name hash) —
  a tag can never wear a different color here than on its tab or board card.
- **A capped footprint**: at most `kSidePanelCardTagSlots` (5) slots. With more tags the last
  slot becomes a `+N` in the title's own ink (dark on a light band, light on a dark one), whose
  tip names the hidden tags. Tighter than the board card's 8 because this list can be dragged
  down to 10% of the window — the Sessions browser Tags column's fixed-footprint idiom.
- **Hover** a ribbon = the SAME rich per-tag panel the tab badges, the board card and the
  Sessions Tags column open (`_OnTagBadgeHoverBegin/End`): every session carrying the tag (live
  first, then closed), and a click on a live row jumps to its tab, cross-window.
- **The card's tooltip names every tag** (a `tags: a · b · c` line) — the ribbons are colors only.
- **⚠ The strip is a tooltip-free zone for the card.** The card's one `AgentSetTip` tooltip covers
  its whole `Border`, pointer events bubble, and this panel has no LocalTooltip area — so without
  help a ribbon would also arm the card tip, which then floats at the pointer ON TOP of the hover
  panel. A ribbon's entry therefore cancels a pending card tip / closes an open one
  (`AgentCloseTipsIn`) and marks the event handled; the strip swallows the moves bubbling up from
  its children (each would re-arm the card tip); the `+N` hooks its own tip BEFORE marking its
  entry handled, so its tip opens while the card's cannot. Leaving the strip re-arms the card tip
  normally.
- **Data + freshness**: `_SidePanelRebuildCards` reads `GetSessionTags` for exactly the cards it
  renders (SessionStore's (size,mtime)-validated record cache: one file stat per card at rest)
  and `LoadAllTagColors()` once, only when a rendered card is tagged — the board's per-rebuild
  recipe. Tags live in the SessionStore, not the registry, so no drain reports a tag edit: the
  Tags panel's two mutation paths (`_ToggleSessionTag`, `_CommitTagEditorAdd`) call
  `_SidePanelNoteTagsChanged()` (rebuild NOW past the min-gap; collapsed → just dirty) beside the
  board's `_RefreshManagerBoardTags`. Another window's side panel catches up on its own next
  rebuild — the cross-window lag every tag surface shares. Deleting a 0-carrier tag needs no
  refresh: no card can show a tag nothing carries.

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

### Which session the summary shows — FOLLOW the terminal vs PEEK a card

The bottom summary tracks **the session you are working IN by default, and the card you click
when you deliberately peek another.** Two inputs drive the one selection (`_sidePanelSelectedId`
= card highlight + summary):

- **FOLLOW (default)** — the summary shows the **focused terminal tab's** session. On a tab
  switch this rides the existing `_SyncManagerSelectionToTab`; and — the key case — whenever you
  **re-engage the terminal content** (a click into it, or focus landing inside it),
  `_SidePanelFollowFocusedTab` snaps the summary + card highlight back to the focused tab. It is
  hooked to `TabContent`'s `PointerPressed` (`AddHandler`, `handledEventsToo` — `TermControl`
  marks its presses Handled) plus its bubbling `GotFocus`. The panel's OWN controls live in a
  SIBLING `ContentArea` column (not under `TabContent`), so touching cards / search / the summary
  never fires these — only touching the terminal does, which is exactly what ends a peek.
- **PEEK** — clicking a **card** pins the summary to THAT session (drives the Manager lens too,
  "like the board card"); it holds until you re-engage the terminal. Because a non-focusable card
  `Border` doesn't move keyboard focus off the terminal, `PointerPressed` (not `GotFocus`) is the
  reliable "back to work" trigger.

`_SidePanelFollowFocusedTab` is gated on `StartupState::Initialized` (never clobber the
restore-seeded selection) and on the focused tab being a managed session (the Agent Manager page — no tab at all / a
`pwsh`·`cmd`·external shell resolve empty → the current peek is kept, nothing to summarize). It
passes `driveManagerLens=false`, so a terminal click moves ONLY the side panel — the Manager
board/tree/Auto-Testing keeps its own deliberate selection. The same-id early-out in
`_SidePanelSelectSession` makes the steady state (already on the focused tab) free, so hooking
these high-frequency events costs nothing at rest.

## 4. Persistence — per window, last-one-wins defaults

The user contract: **both separators persist PER WINDOW (not per tab), and the last one to
resize sets the default for new windows.** Collapse follows the same rule; the dock side is
GLOBAL-only; the **card scope is per-window ONLY** (no global default — see below).

| state | per-window (windows/<id>.json) | global default (settings.json) |
|---|---|---|
| panel width | `WindowRecord::sidePanelWidthFraction` (0 = unset) | `AppSettings::sidePanelWidthFraction` (0.22) |
| cards/summary split | `WindowRecord::sidePanelSplitFraction` (0 = unset) | `AppSettings::sidePanelSplitFraction` (0.5) |
| collapsed | `WindowRecord::sidePanelCollapsed` (-1 = unset) | `AppSettings::sidePanelCollapsed` (false) |
| dock side | — | `AppSettings::sidePanelOnLeft` (true = **left, the default**; false = right) |
| card scope (LOCAL/GLOBAL) | `WindowRecord::sidePanelGlobal` (absent = LOCAL; written only when GLOBAL) | — **none by design**: every new window starts LOCAL |
| `⌖` scroll to the selected tab (§2a) | — | `AppSettings::sidePanelScrollToSelectedTab` (true = **ON, the default**; absent key = ON) — a GLOBAL preference, broadcast live to every open window |

- **The card scope is the one field with NO last-one-wins export**: `_SidePanelToggleScope`
  persists it through the window-record autosave alone — never `_SidePanelPersistDefaults`, and
  there is no `AppSettings` field to seed from. A window that flips to GLOBAL keeps that for
  itself; a new window (or a pre-feature record, where the key is absent) starts LOCAL. This is
  what guarantees one window's fleet-wide view can never leak into another window's list — the
  bug that motivated the scope ("the upper list shows the tabs of a different window").

- Every drag release / collapse toggle: update the live members → the window-record autosave
  captures them (`_CaptureWindowRecord`) → a GRANULAR freshest-disk RMW exports ONLY the
  adjusted field as the new global default (`_SidePanelPersistDefaults` — a width drag never
  exports this window's collapse state).
- A new window (or a pre-feature record's unset field) seeds from the globals at
  `_InitAgentSidePanel`; a window that made its own choice keeps it.
- Bands everywhere (drag clamps == load-time checks): width (0.10, 0.50], split (0.15, 0.85).
  A record's out-of-band value degrades to UNSET (the global default); a settings out-of-band
  value degrades to the shipped default — a corrupt file can never collapse the terminal.
- All five AppSettings fields are written OUTSIDE the cog form, so BOTH cog-Save preserve
  blocks (content + page sink) restore them from disk. For the `⌖` toggle the page sink's line is
  load-bearing beyond the cog: the board-sort cycle (the board header's AND this panel's sort
  button) saves through that sink carrying the Manager content's `AppSettings` copy, which never
  learns of THIS window's own `⌖` flip (the toggle lives on the page; the content only hears other
  windows' broadcasts) — without the preserve line, the next sort click in this window would
  silently turn the toggle back.
- **The dock side defaults to LEFT** (it was RIGHT when the panel first landed). The struct
  default and `AppSettingsFromJson`'s absent-key fallback deliberately agree, so a fresh install
  and a settings.json from before the key both dock left. A PRESENT value always wins, `false`
  included — and because every settings write serializes the WHOLE struct, an install that ran
  the original RIGHT-default build and saved anything since stores `false`, which is
  indistinguishable from choosing RIGHT on purpose. It is honored, never migrated: one `⇄` click
  moves it (and persists). The RIGHT default never shipped in a public release, so only
  locally-built installs can carry one.

## 5. Perf + safety rules upheld

- Rebuilds: batched-drain trigger + 500ms min gap + collapsed-defers + in-place timing refresh;
  no eager flyouts on cards (menus built at right-click); `SnapshotLive` only; the per-card
  color resolve rides the cached `LoadDirColors`; the per-card tag read rides SessionStore's
  (size,mtime) record cache (one stat per rendered card), with `tag-colors.json` read once per
  rebuild and only when a rendered card is tagged (§2b).
- The `⌖` reveal is one coalesced Low-priority tick per tab selection (a map lookup + one
  `UpdateLayout` + `StartBringIntoView` when owed; nothing at all when not), never a per-rebuild
  re-scroll, and never a focus move — the terminal keeps typing focus.
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
  `TerminalPage.AgentObserver.cpp` (toggle loops + the Tags panel's `_SidePanelNoteTagsChanged`
  calls), `TerminalPage.AgentWindowRecord.cpp` (capture), `AgentManagerContent.h` (the two public
  wrappers — `BuildSessionMenuFor` takes the card as its Tags-panel anchor),
  `AgentManagerContent.Settings.cpp` (cog preserve), `AgentTabOverlay.{h,Summary.cpp}`
  (fill-host mode), `AgentMaster/SessionModels.h` + `Persistence.cpp` (fields + JSON),
  `tests/tests_persistence.cpp` (round-trip / defaults / bands).
