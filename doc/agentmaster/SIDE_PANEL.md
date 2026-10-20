# The SIDE PANEL — a window-chrome fleet pane beside the terminal

A non-terminal, non-tab panel docked LEFT (the default) or RIGHT of the tab content — part of
the window itself (every tab shares it), giving the fleet + the current session a permanent home
next to whatever terminal you are working in. It deliberately REUSES the Agent Manager page's and the
per-tab overlay's components, so it carries their business logic verbatim instead of a parallel copy.

## 1. Layout

Shown in the DEFAULT dock (LEFT); the header's `⇄` flips it to the mirror image (dock RIGHT),
which also flips the collapse chevron (`«` here — it always points toward the docked edge).

```
┌ tab row ────────────────────────────────────────────────────────────┐
├───────────────────────────────┬──┬──────────────────────────────────┤
│[+][Sum][LOCAL][sort][⌖][⇄][«] │ ▌│                                  │  ← header line 1 (right-aligned): + launcher (§2e) /
│                               │ ▌│                                  │    summary area / card SCOPE / order / scroll-to-tab / dock / collapse
│ [filter sessions…]            │ ▌│                                  │  ← header line 2: the filter box, full width
│ N of M (this window)          │ ▌│        terminal content          │  ← count line (names the scope)
│ ┌───────────────────────────┐ │ ▌│          (TabContent)            │
│ │ session CARDS             │ │ ▌│                                  │  ← TOP area (own scroll)
│ └───────────────────────────┘ │ ▌│                                  │
│ ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ │ ▌│                                  │  ← horizontal splitter
│ ┌───────────────────────────┐ │ ▌│                                  │
│ │ selected sess.            │ │ ▌│                                  │  ← BOTTOM area (own scroll)
│ │    SUMMARY                │ │ ▌│                                  │
│ └───────────────────────────┘ │ ▌│                                  │
└───────────────────────────────┴──┴──────────────────────────────────┘
                                  ↑ vertical splitter (the resizable separator)
```

With **Sum** OFF (§2d) the bottom area and the horizontal splitter are gone, the cards fill the
whole body, and hovering a card opens that session's RICH card — the one a tab header shows — beside
the panel:

```
┌ tab row ────────────────────────────────────────────────────────────┐
├───────────────────────────────┬──┬──────────────────────────────────┤
│[+][Sum][LOCAL][sort][⌖][⇄][«] │ ▌│                                  │  ← "Sum" OFF (unchecked)
│ [filter sessions…]            │ ▌│                                  │
│ N of M (this window)          │ ▌│                                  │
│ ┌───────────────────────────┐ │ ▌│ ┌──────────────────────────────┐ │  ← hover a card: the session's RICH card
│ │ card                      │ │ ▌│ │ ● title         folder/branch│ │    opens beside the panel (left of it
│ │ card  (hovered)           │ │ ▌│ │ state · ago · why            │ │    when the panel is docked RIGHT),
│ │ card                      │ │ ▌│ │ claude · model · effort      │ │    vertically centered on the hovered
│ │ …                         │ │ ▌│ │ tags                         │ │    card — the card a tab shows on hover
│ │ (the cards fill           │ │ ▌│ │ ── Summary (wheel-scrolls) ──│ │
│ │  the whole body)          │ │ ▌│ └──────────────────────────────┘ │
│ └───────────────────────────┘ │ ▌│                                  │
└───────────────────────────────┴──┴──────────────────────────────────┘
```

- `TerminalPage.xaml`'s content row became a 3-column `ContentArea` grid; code
  (`_SidePanelApplyDock`) assigns TabContent / splitter / panel to the columns per the GLOBAL
  dock side (`AppSettings::sidePanelOnLeft`, LEFT by default), so either dock is a pure column
  rotation — LEFT = `[panel][splitter][TabContent *]`, RIGHT = its mirror. Both extra columns are
  0 until `_InitAgentSidePanel` runs, so a build without the panel renders the pre-feature layout.
- The whole panel forces the Agentmaster DARK theme (explicit fills, `RequestedTheme(Dark)`).
- Collapse (the header chevron / the strip) shrinks the panel to a 20px expander strip; the
  header also carries the **+** launcher (§2e, the first button, left of Sum), the **Sum**
  summary-area toggle (§2d, left of LOCAL / GLOBAL), the card
  list's **LOCAL / GLOBAL** scope toggle (§2), the `⌖` **scroll-to-selected-tab** icon toggle
  (§2a), and a `⇄` dock-side flip.
- The header is **two lines** (one 2-row grid): the buttons, right-aligned in their original order,
  over the filter box, which spans every column. Sharing one line with the buttons, the box was
  squeezed toward its minimum on a narrow panel while the buttons kept their natural widths.
- **Two font sizes.** The worded buttons (Sum, LOCAL / GLOBAL, the sort label) are 11 px; the ICON
  buttons (`+`, `⌖`, `⇄`, the collapse chevron, and the collapsed strip's chevron) are 12 px
  (`kSidePanelIconFontSize`, 2026-10-05 — they were 11 px, and a symbol reads smaller than a word at
  the same size).
- **The two separators are 3 px thick** (`kSidePanelSplitterPx`: the vertical one's ContentArea
  column and the horizontal one's body row; 2026-10-05 — they were 6 px, the size of the per-tab
  overlay's resize grips, and read as heavy borders). They stay solid gray, `#3E3E3E` at rest and
  `#606060` under the pointer or during a drag. Each bar is its own grab area, so the drag target is
  3 px too; a wider invisible hit area would have to overlap the terminal's edge or the cards and
  take their pointer input.
- **Their tooltips open only after 7 s of rest** (`kSidePanelSplitterTipDelay`, 2026-10-06; the
  panel's buttons wait 3 s, next bullet, and most other Agentmaster tips open after ~133 ms). A tip
  that opened while the user aimed at a bar got in the way of the drag. Any move restarts the 7 s,
  and a drag never shows the tip: the press silences it (`AgentSuppressTip` — an open tip closes, a
  pending one is cancelled) and the drag's end (`endWidthDrag` / `endSplitDrag`, on release or
  capture loss) gives the text back.
- **The buttons' tooltips open only after 3 s of rest** (`kSidePanelButtonTipDelay`, 2026-10-10, commit
  952c246fa — the user's request; in production 0.6.8.19 since 2026-10-12 and in the v0.7.9
  pre-release): the header's `+`, **Sum**, LOCAL / GLOBAL, the sort, `⌖`, `⇄` and
  the collapse chevron, and the collapsed strip's expand chevron. Each tip is a paragraph, and at
  ~133 ms it opened over the panel whenever the pointer reached a button on its way to a click. Any
  move restarts the 3 s. A new button in the panel must pass the constant to its `AgentSetTip` /
  `AgentSetTitledTip`. Not changed on purpose: the filter box (not a button), the cards (4 s), the
  separators (7 s), the summary area's own buttons (the per-tab summary panel's, shared with every
  tab) and the `+` launcher card (the Agent Manager's own launch bar, §2e). Check after the build:
  the pointer still on each of the eight buttons opens its tip after ~3 s, a move restarts the wait,
  and a click before then shows no tip.

## 2. TOP — the session cards (the Triage-Board integration, vertical)

- **Data**: `SessionRegistry::SnapshotLiveForCards()` (both kinds — the live sessions without their
  prompt queues and transcript texts, which no card shows; the unsent draft stays, for the dots),
  narrowed by the **card scope**:
  **LOCAL by default — only the sessions whose tab is hosted in THIS window**
  (`TerminalPage::_HostedSessionIds()`, the live `_claudeTabs` set — the SAME predicate the
  Explorer Tree's LOCAL scope provider reads, so the two surfaces can never disagree about what
  "here" means); the header's **LOCAL / GLOBAL** toggle (`_SidePanelToggleScope`) widens THIS
  window's list to every window's sessions. It is a SEPARATE state from the Manager's tree/board
  scope (they answer different questions — the fleet lens vs. what sits beside your terminal) and
  is persisted **per window only** (§4). The list is **edited in place, never rebuilt** (§2c), and only
  the cards near the viewport exist (§2f, **virtualized**); it is updated off
  the page's already-batched registry drain, min-gap throttled (500ms), fully deferred while
  collapsed (the Manager-lens rebuild-tax discipline) — plus, while LOCAL, a per-tick **hosted-set
  diff** (`_SidePanelPeriodicTickImpl`): a tab docked into / torn out of the window changes
  `_claudeTabs` with NO registry notify (the moved session stays live + unchanged), so the drain
  trigger alone would leave a LOCAL list stale. After ~30 s with no update, the periodic tick runs
  the same in-place update for the "ago" texts: only the timing texts whose value moved are written
  — never a rebuild for a clock tick. The count line names the scope (`N of M sessions in this
  window` / `… across all windows`), so a short list reads as "this window", not "the fleet shrank".
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
- **A new session or fork starts at the TOP** (MOST ACTIVE and MOST RECENT). Both orders read the
  decay anchor `SessionInfo::lastActivityUnixMs` while a session has no transcript activity and no
  turn stamps — and a brand-new card has neither. So the launch seam (`_LaunchClaudeSession` /
  `_LaunchCodexSession`) stamps a NEW session's anchor with its **creation instant**: a fresh spawn
  ("Open New Session Here", the launch bar, a /handover successor) and every fork (the tab menu, a
  card menu, the Sessions page, an adopt-fork), Claude and Codex. The card then sits at the top — in
  MOST ACTIVE right under the Running cards that order pins above everything, in MOST RECENT at the
  very top — until another session's activity overtakes it. Before the stamp the anchor was 0 and the
  new card sat at the **bottom**: a fresh spawn until its first `SessionStart` hook, a fork until its
  own first hook, usually its first prompt (its startup `SessionStart` arrives under the SOURCE id and
  the fork-echo guard drops it whole), a managed Codex for good (no hooks; `_ReconcileManagedCodex` writes no time) — and since
  creating a tab selects it, the `⌖` reveal (§2a) scrolled the card list all the way down to it. A
  restore/resume is NOT stamped: it keeps its persisted anchor, and its transcript orders it.
- **A card**: title on a band painted the session's `ResolveSessionColorHex` color
  (WCAG-contrast text), the session's **bookmark-tag ribbons in the band's top-right corner**
  (§2b), state dot + `StateLabel` in the shared `AgentStatusColors` palette, a
  teal `codex` pill, the pending-draft `BuildPendingDots` cluster, the timing line, and a dim
  `leaf / branch` line.
- **The timing line** (2026-10-05) reads `created 4d1h, active 2m4s ago` — the same text as the
  Triage Board's cards and the Explorer tree's rows, from one pure formatter
  (`AgentMaster/SessionTimingText.h`, `FormatSessionTimingLine`, harness-tested):
  - **created** — how long ago the session was created, in years / days / hours (`y`, `d`, `h`),
    never minutes or seconds. Under an hour it is a word: `created recently`; under 5 minutes
    `created now`.
  - **active … ago** — how long ago it last did something, in `y` / `d` / `h` / `m` (minutes) /
    `s` (seconds only under an hour). Left out when it is 0: no recorded activity, or activity
    this very second.
  - There is no month unit, so `m` is always minutes. It replaced `-7h5m/7h6m/2m4s` (created ago /
    active span / last activity ago, with `m` meaning months before a `d`/`h` and minutes after
    one), which the user found unreadable.
  - It sits in the STAR column of the card's state line (`[ state row Auto ][ timing * ]`,
    `AgentSidePanelCardView::stateLine`), so on a narrow panel it ends in `…`. In the row's
    horizontal StackPanel it was offered unlimited width and was cut off mid-letter at the card's
    edge — and the new text is about twice as long as the old one.
- **Interactions** (Linked Lenses): click = select (drives `AgentManagerContent::SelectSession`,
  so the board/tree/Prompt Queue follow); double-click = `_ActivateClaudeSession` (jump to the
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
- **What never scrolls it.** A list update (updates follow every fleet change and would fight your
  own scrolling), a card click (the card is already where you clicked it), a Manager-side selection (the
  lens push), and the Agent Manager page itself (no tab is selected there).
- **Minimal and one-shot.** The scroll is `UpdateLayout()` + `StartBringIntoView()` on the card — the
  Manager board's own `BringSelectedIntoView` recipe — so a card already fully visible never moves
  and a far one stops at the nearest edge. Keyboard focus never leaves the terminal. A session whose
  row has no card (it lies outside the realized rows, §2f) first gets the rows around it realized, and
  the card is then reached in ONE jump, without the scroll animation.
- **An owed reveal.** `_SidePanelRevealTabCard` records the id (`_sidePanelRevealPendingId`) and
  queues ONE coalesced Low-priority tick (`_SidePanelQueueReveal` → `_SidePanelRevealPendingNow`), so
  rapid Ctrl+Tab cycling folds into one scroll to the LAST tab, and the tick measures the list after
  whatever list update / content swap ran first. The debt is consumed by the first tick that finds the session
  **listed** (its row realized first when it has no card, §2f), and survives until then: a just-launched session's card (it lands on the NEXT
  update — `_SidePanelRebuildCards` re-queues), a collapsed panel (the expand re-queues), a card the
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
┌──────────────────────────────────────────┐
│ Fix the flaky login…               ▮▮▮▮+2│  ← title band (folder color); ribbons hang from its top edge (6 tags: 4 + "+2")
│ ● Waiting  created 4d1h, active 2m4s ago │  ← state line: the timing text trims with "…" when the panel is narrow
│ Agentmaster / main                       │
└──────────────────────────────────────────┘
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
  With **Sum** OFF the card's tooltip is the rich session card (§2d), which shows every tag as a
  colored chip.
- **⚠ The strip is a tooltip-free zone for the card.** The card's one `AgentSetTip` tooltip covers
  its whole `Border`, pointer events bubble, and this panel has no LocalTooltip area — so without
  help a ribbon would also arm the card tip, which then floats at the pointer ON TOP of the hover
  panel. A ribbon's entry therefore cancels a pending card tip / closes an open one
  (`AgentCloseTipsIn`) and marks the event handled; the strip swallows the moves bubbling up from
  its children (each would re-arm the card tip); the `+N` hooks its own tip BEFORE marking its
  entry handled, so its tip opens while the card's cannot. Leaving the strip re-arms the card tip
  normally.
- **With Sum OFF** (§2d) the card's tooltip is a `ToolTip` that `ToolTipService` owns, and nothing
  may close it by writing `IsOpen`. So a ribbon's or the `+N`'s entry DETACHES it
  (`_SidePanelSuspendCardToolTip`; the framework then closes an open card and cancels a pending
  open itself), and the rich card never floats beside the panel while the tag hover panel is up.
  The next entry into the card's body attaches it again: the card push re-hosts whenever nothing is
  attached. The `+N` has no floating tip in this mode (the rich card lists every tag), and a mode
  switch rebuilds the strip so the `+N` always matches the mode.
- **Data + freshness**: the list keeps each listed row's tags in a per-row cache
  (`_sidePanelTagsById`; every row, realized or not, since every row keeps its model — §2f).
  `GetSessionTags` (SessionStore's (size,mtime)-validated record cache: one
  file stat) runs only for a row new to the list, and for every listed row once the
  **tag-change epoch** moved: `SessionTagsEpoch()`, a process-wide counter that SessionStore moves
  after every write that CHANGED a session's tag list or a tag's color. `LoadAllTagColors()` runs
  once per epoch, and only when a listed row is tagged. So an update with no tag change reads no
  tag file at all; it used to stat one record per rendered card, ~466 per update in the release's
  biggest window. The epoch is loaded BEFORE any tag read: a write that lands during the reads
  leaves the older epoch cached, and the next update reads again — a cache refreshed once too often,
  never kept stale. Tags live in the SessionStore, not the registry, so no drain reports a tag edit:
  the Tags panel's two mutation paths (`_ToggleSessionTag`, `_CommitTagEditorAdd`) call
  `_SidePanelNoteTagsChanged()` (update NOW past the min-gap; collapsed → just dirty) beside the
  board's `_RefreshManagerBoardTags`. (Since 2026-10-10, not built yet, the two paths reach both
  through `_QueueTagSurfacesRefresh`: ~40 ms after the last edit, so the Tags panel's own list is
  rebuilt first — UI_THREAD.md §11.) An edit made in **another window** or on the Sessions page
  moves the same epoch (every window runs in one process), and the periodic tick (~2.5 s) compares
  it, so every side panel follows within one tick. Before, another window's panel caught up only on
  its next unrelated rebuild. Not seen: a tag write by another PROCESS on the same profile (the
  profile mutex already warns about two instances on one profile, and the CLI only reads). Deleting
  a 0-carrier tag needs no refresh: no card can show a tag nothing carries.

### 2c. The card list is edited in place, never rebuilt

Each update brings the list up to date with the fewest XAML edits. The old update cleared the list and
built every card again: ~466 cards and ~940 composition nodes in the release's biggest window, up to
twice a second while sessions changed, and once more every 30 s to move the "ago" texts.

- **A model per row.** `SidePanelCardModelFor` computes everything a card shows from its live
  session — the title, the band color and ink, the state, the codex pill, the draft dots (with their
  color and whether they pulse), the timing text, the dir line, the tags with their RESOLVED colors,
  the tooltip — into a plain `AgentSidePanelCardModel` (`TerminalPage.h`). Since §2f only the rows near
  the viewport have cards, but EVERY listed row keeps its model (`_sidePanelRowIds` /
  `_sidePanelRowModels`), so a row that a scroll realizes shows its current model at once. A card keeps
  the model it shows (`AgentSidePanelCardView::model`).
- **Only the differences are written.** `_SidePanelApplyCardModel` compares the new model with the one
  the card shows, value by value, and writes only what changed. An optional part (the codex pill, the
  draft dots, the timing text, the band's tag strip) is added or removed only when its presence
  changes. In the steady state (one session changed its state) the update writes one dot, one label
  and one border; a session that changed nothing costs its card nothing.
- **A card is built once per session.** `_SidePanelCreateCardView` builds the skeleton (band, title,
  state row, dir line) and wires the handlers once; the first model is applied with `fresh` (every
  value written). A card is never re-keyed: a session that leaves the list loses its card, and a session
  that comes back gets a new one.
- **The order changes with the fewest removes, inserts and moves.** `PlanKeyedListOps`
  (`AgentMaster/KeyedListDiff.h`, pure, header-only) diffs the shown order against the new one:
  removes first (highest index first), then the cards on the longest increasing subsequence of target
  positions stay where they are, and every other card is moved or inserted right before its target
  successor. That is the minimum (cards kept − LIS length). A card that jumps to the top, or sinks to
  the bottom, is ONE move. The plan's indexes are those of XAML's `UIElementCollection`: `Move` is a
  reorder of the panel's item array, with no `Unloaded`/`Loaded`, so a moved card keeps its elements,
  its pulsing dots and its hover state.
- **The mirror.** `_sidePanelCardOrder` (the shown order, as session ids) and `_sidePanelCardViews`
  (id → card) must match the panel's children exactly; every edit is made on both in step. Since §2f
  they hold the REALIZED rows only, and the panel's children are [top spacer][the realized cards]
  [bottom spacer], so every planned index is the mirror's + 1 and the spacers never move. Before an
  update edits anything, the panel size, the mirror and the map are compared; after it, the mirror
  must equal the new order and the panel must hold that many children. A mismatch, or a throw midway
  (a XAML edit refused, a torn-down tree), resets the list to empty (`_SidePanelResetCards`, logged
  `[sidepanel] … out of step` / `[exc] _SidePanelRebuildCards`) and marks it dirty; the next tick fills
  it from empty (front-to-back appends).
- **Lean inputs, read once per update:** `SnapshotLiveForCards` (no queues, no transcript texts), ONE
  `LoadDirColorIndex()` snapshot for every card's band color (`ResolveSessionColorHex`'s 3-argument
  overload — the same precedence as the 2-argument one, through one shared template), the tags from
  the per-card cache (§2b), the dots' color, and Windows' animation switch.
- **Hover and selection read the card's model**, not a registry copy: the hover ring takes the state
  color of the state the card shows, and the card remembers that it is hovered
  (`AgentSidePanelCardView::hovered`), so a state change repaints the ring in the new color.
- **The timing texts** ride the same update: the periodic tick runs it after ~30 s without one, and
  only the texts whose value moved are written.
- **Tested** (no build needed): `TestKeyedListDiff` (`tests/tests_sidepanel.cpp`) replays every plan
  and checks it against the target and against the minimum (an O(n²) LIS): the named shapes, all 120
  orders of five cards, 3000 random lists, and 466-card rotations (one move each). The lean snapshot,
  the tag epoch and the color index have their own checks (`TestRegistryLeanReads`,
  `TestSessionTags`, the color-index block of `TestTabColorModes`). The card code itself
  (`TerminalPage.AgentSidePanel.cpp`, commit `d99d18d6b`) is built and runs in the local release
  install 0.6.8.16 (2026-10-04; its `TerminalApp.dll` was linked after that commit). The live checks
  below are not done yet.
- **Check after the build:** a session that changes state repaints only its card (no flicker of the
  list); launch, close, filter and sort changes put the cards in the right order; a hovered card keeps
  its ring while another card moves; a card's draft dots keep pulsing when it moves; a tag added in
  another window shows on this window's card within ~2.5 s; no `[sidepanel] … out of step` line in
  hooks.log.

### 2d. The "Sum" toggle — no summary area, a rich card beside the panel instead

The header's **Sum** button (a real `ToggleButton`, left of LOCAL / GLOBAL; checked = the summary
area is shown, the default) hides the bottom summary area. The session cards then fill the whole
body, and **hovering a card opens that session's RICH card beside the panel** — the very card a tab
header shows when you hover it: same content, same lifecycle, same wheel scrolling. It opens to the
RIGHT of a left-docked panel and to the LEFT of a right-docked one.

- **The click** (`_SidePanelToggleSummary`): flips `_sidePanelSummaryHidden`, logs
  `[nav] sidepanel summary -> hidden (cards show the rich session tooltip)` / `-> shown`, re-pins the
  button (`_SidePanelUpdateSumButton` — the member is the truth, not the button's own flip), applies
  the layout, rebinds the summary, switches every card's tooltip mode, and persists like collapse
  (§4: this window's record + the last-change default for new windows).
- **The layout** (`_SidePanelApplyLayout`): hidden → the body rows are `*` / 0 / 0 and the horizontal
  splitter and the summary host are collapsed; shown → the split fraction as before.
- **The summary overlay works only while it can be seen**: `_SidePanelBindSummary` releases it while
  the area is hidden (its registry observer detaches, no more transcript analyses), and showing the
  area again rebinds the current selection (a peek included).

**One host, extracted from Tab — not a copy.** The tooltip LIFECYCLE that lived inside `Tab` is now
`AgentSessionToolTipHost` (`AgentSessionToolTip.{h,cpp}`), moved verbatim. `Tab` keeps one host for
its header (its old public methods forward to it), and each side-panel card keeps one for its
`Border` (`AgentSidePanelCardView::tipHost`). So both owners run the same code and keep the same
crash rules (HANDOVER_tab-tooltip.md §9–§10):

- **framework-managed**: `ToolTipService` opens and closes it; nothing writes `IsOpen`;
- **one reused `ToolTip`**, its `Content` swapped only while closed, with the one-shot open swap for
  the summary body that lands while you read;
- **the owner guard**: nothing is created or changed for an owner that is not loaded;
- **detach on `Unloaded`, re-attach on `Loaded`**, so a tooltip is always attached before a pointer
  can arrive (an attach during a hover does not open for that hover);
- **the hover hook**: `PointerEntered` re-anchors a CLOSED tooltip, then runs the page's build;
- **the wheel hook**: `PointerWheelChanged` read on the OWNER (`handledEventsToo`), acted on only
  while the card is open; the notch is marked handled only when the card's body has something to
  scroll (even at an end stop); logged `[tooltip-wheel] card: cb= tip= open=`.

**One content push, shared.** `_PushSessionToolTipCard` (TerminalPage.AgentObserver.cpp) is the old
body of `_UpdateTabAgentToolTip`, now called by both surfaces: the strings, the change signature,
the 80%-of-window height budget, the tags, the cached Summary body, the scroll-piece adoption, and
the summary kick (mtime-gated, at most every 4 s, one load per session). `_UpdateTabAgentToolTip` and
`_UpdateSidePanelCardToolTip` are thin surfaces over it. The summary loader
`_EnsureTabTooltipSummary` is now per SESSION, not per tab: on arrival it re-hosts every surface that
waits for that session (`_tabTooltipSummaryWaiters[session][surface]`, each in its own catch) with
the open-swap grant and never kicks another analysis (the 2026-08-29 loop rule). The wheel's offset
math, scrollbar sync and fade are the shared `_ScrollSessionToolTipBody`.

**Surface keys.** The signature and the scroll state are kept per SURFACE: a tab's under its session
id, a card's under `"\x1F" "sidepanel:" <id>` (`SidePanelToolTipKey`). So the tab and the card of one
session never share a scroll position or a signature, while they DO share the one Summary cache.

**What differs on purpose:**

1. **Placement** (`_SidePanelCardToolTipPlacement`, the host's placement provider): a
   `PlacementRect` in the card's own coordinates that spans the whole panel plus its splitter
   horizontally and the card vertically, with `PlacementMode::Right` (left dock) or `Left` (right
   dock). WinUI adds its 20 px default mouse offset around the rect, docks the card outside it,
   centers it vertically on the hovered card, and slides it back inside the window when needed. The
   provider runs on every closed re-anchor, so a width drag or a dock flip applies at the next hover.
   A tab keeps the old default: `Bottom` under the tab's own bounds.
2. **The width budget** (`_SidePanelCardToolTipRoomWidth`): the content area minus the panel, the
   splitter, the 20 px gap and an 8 px margin, clamped to [320, 660] and carried in the signature on a
   50 px grain. A tab passes the flat 660. ⚠ Per the WinUI 2.8 source, the `ToolTip` style itself caps
   every tooltip at **320 px** (`ToolTipMaxWidth`, padding 9,6,9,8), and the host sets no `MaxWidth` —
   so both the tab's card and this card render at most ~300 px wide today, whatever the budget says
   (the `kTtCardMaxWidth` note in TerminalPage.AgentObserver.cpp). Not yet measured on screen.
3. **The wheel** stays with the card list's own `ScrollViewer` while the rich card is closed or its
   body fits, and it never sets `_tooltipWheelClaimed` (that flag de-dupes the tab ROW's belt
   handler only); its per-notch line reads `[tooltip-wheel] <sid8> src=card …`.
4. **The card's floating tips are silenced** in this mode: `AgentSuppressTip` (new in
   `AgentTipHelpers.h`) on the card and its timing text, and the `+N` gets none. The shared tip host
   would otherwise REPLACE the card's `ToolTip` on open (it attaches its own tip to the hovered
   element) and DETACH it on close. `AgentSuppressTip` keeps the element's "already wired" marker, so
   turning **Sum** back on just gives the texts back (from the card's current model).
5. **The tag strip suspends it** (§2b): a ribbon or `+N` entry detaches the rich card.
6. **No default tooltip to fall back to**: a session that is gone or archived at hover time clears
   the host; the next list update removes the card anyway.

**Cost.** When a card is created in this mode, or the mode turns on, `_SidePanelArmCardToolTip`
hosts a HEADER-ONLY card (no transcript read): enough for `ToolTipService` to have a tooltip
attached before the first hover. It is fed from the list update's lean row, or from ONE
`SnapshotLiveForCards` in the mode switch — never a whole-record copy per card (UI_THREAD.md §10g).
A hover reads `GetWithoutQueue`, rebuilds the card only when the signature changed or nothing is
attached, and kicks the Summary analysis, which is shared with the tab's tooltip (one cache entry per
session). No transcript is read for a card nobody hovers.

**Lifetime.** Before a card leaves the list (an update's Remove, or `_SidePanelResetCards`),
`_SidePanelDropCardToolTip` shuts its host down (detach + drop both callbacks) and forgets its
surface state: a `ToolTip` left on a dropped element is the islands leak class (AgentTipHelpers.h),
and the host's own `Unloaded` detach is deferred, so it can run after the card view is gone. A card
MOVED inside the list keeps its tooltip (`Move` raises no `Unloaded` / `Loaded`). A Summary load still
in flight finds the card by id when it lands (or finds none).

**Check after the build:** Sum OFF hides the summary and its splitter, and the cards fill the body;
hovering a card opens the rich card beside the panel, centered on the card's height, on the right of
a left-docked panel and on the left after `⇄`; the wheel scrolls its Summary body and, when it can't,
scrolls the card list; a ribbon's hover panel never has the rich card behind it; a width drag or a
dock flip is followed at the next hover; Sum ON brings the summary and the small card tips back; a
reopened window keeps its own choice, and a new window takes the last one; measure the card's real
width (the 320 px `ToolTip` cap above); no `[exc] Side-panel card …` lines in hooks.log.

### 2e. The "+" launcher — the Agent Manager's launch box, at the click

Built 2026-10-06 (commit f73481af7); in production 0.6.8.19 since 2026-10-12 and in the v0.7.9
pre-release. The fix of item 5 below (2026-10-12, commit bd9ba2eae) is in dev since 0.0.1.6 and in
production since 0.6.8.20. The folder list opening WITH the card (item 5, 2026-10-19) and the
one-click Launch (item 8, 2026-10-19) are in no build yet. The
header's **+** button (the first of the right-aligned buttons, left
of **Sum** — the user's "leftmost, to Sum") opens a small card AT THE CLICK that holds the Agent
Manager's own launch row, so a session starts from the side panel without leaving the tab:

```
  [+]  ← the click
     ┌───────────────────────────────────────────────────────────────────┐
     │ ● Claude   [ K:\source\Agentmaster           ]   [Launch Claude]  │
     │            ═════════ validation underline ════                    │
     └───────────────────────────────────────────────────────────────────┘
     ↑ the card's corner sits at the click; the card extends toward the window's center
       (Fork appears beside Launch only while the box holds a session id found on disk)
```

**The user's eight answers** (2026-10-06), and how each is met:

1. **Where:** `+` in header column 1, left of **Sum**; Sum, LOCAL / GLOBAL, the sort, `⌖`, `⇄` and
   the chevron each moved one column right (column 7 is new). An ICON button: 12 px like `⌖` / `⇄`,
   `IsTabStop` false like its siblings, an automation name ("Launch a session").
2. **Placement — tooltip-like, but interactive.** The card's corner sits at the click point and the
   card extends toward the window's center: a click in the left half opens it rightward (its left
   edge at the click), in the right half leftward (its right edge at the click), and the same with
   top / bottom (`AgentMaster/PointerCardPlacement.h` `PlacePointerCard`, pure; the exact middle
   counts as left / top; a 2-DIP gap off the click pixel). It is clamped 8 DIP inside the window, a
   card larger than the window is pinned to the near margin, and an unmeasured window is not
   clamped. Nothing closes on pointer-leave: the pointer can move onto the card and work in it.
   - The point is read on the `+` button's **press** (`PointerPressed`, `handledEventsToo`): a
     `Click` carries no position. `PointerCaptureLost` drops a press that ended without a click, so a
     `Click` with no press of its own (UI Automation) anchors under the button instead.
   - Placed in the open's own tick at the card's measured size (no frame at a wrong spot), and again
     on the next tick after every size change of the card or the window
     (`_SidePanelQueueLaunchCardPlacement` — never inside `SizeChanged`, the `E_LAYOUTCYCLE` rule);
     the offsets are written only on a real move.
3. **What it shows:** `[● Claude] [ box ] [Launch Claude] [Fork]` — the Manager's row without its
   "launch a" / "session in" words.
4. **The text — "keep the last, same one we have in agent manager tab":** the card opens on the
   Agent Manager box's text (`AgentManagerContent::LaunchBoxText`), mirrors every edit into it live
   (the bar's `textEdited` → `SetLaunchBoxText`), and writes its last, normalized text back when it
   closes. So the two boxes always read the same; a tab switch then pre-aims the Manager's box as
   always (the card closes first, in `_ApplyAgentTabSelection`, before `_SyncManagerSelectionToTab`).
5. **Focus, and the folder list OPEN with the card (the user's rule, 2026-10-19, in dev since 0.0.1.31, not in production):**
   "That path selection menu's path textbox's dropdown menu is supposed to be open by default when +
   the menu is shown." One dispatcher tick after the open, `_SidePanelFinishLaunchCardOpen` runs three
   steps, in this order:
   1. **The card is laid out at its place:** one synchronous layout pass (`UpdateLayout` on the card).
      A popup's child moves to new offsets only in a layout pass (`CPopupRoot::ArrangeOverride`
      arranges it at `HorizontalOffset` / `VerticalOffset`), so before that pass the box still reads
      the card's previous place, or (0,0) on a window's first "+" — the 2026-10-12 report below. The
      call runs the pass XAML would run before its next frame anyway, and does nothing when that pass
      already ran (WinUI `CLayoutManager::UpdateLayout` returns at once when nothing is dirty).
   2. **The box gets PROGRAMMATIC focus:** the caret waits in it. A refused focus logs
      `[sidepanel] launcher card: its box did not take focus` (keys then still go to the terminal).
   3. **The box's folder list opens right below it** (`AgentLaunchBar::OpenPathPicker`, exactly what a
      click in the box does). The bar's re-anchor round after the next pass stays as the belt: the
      card re-places itself when its size settles, and a real move re-anchors the list.
   - **Before (2026-10-06 to 2026-10-19):** the drop-down stayed closed until a click in the box or a
     keystroke — the user's answer of 2026-10-06 ("put the cursor in the box, but do not open the
     folder list. Your click or first key opens it." — "true").
   - **What it changes:** Esc closes the list first (the box's own Esc, as on the Agent Manager page)
     and a second Esc closes the card. The list's rows read the disk at every "+" (the subfolders,
     each recent folder's git branch) — what a click in the box always cost. The Agent Manager page is
     unchanged: its box opens the list on a click, a Tab or a key.
   - **The fix of 2026-10-12 (in dev since 0.0.1.6, production since 0.6.8.20) — the user's
     report:** "In some cases when pressing + it causes the Browse which is top row to be over the
     path text box and so it hides the entire textbox and Launch Claude button." Two XAML facts, both
     read in the WinUI source, broke the rule of that time (the list closed until a click or a key):
     - XAML coerces a programmatic focus before `GotFocus` (`CFocusManager::CoerceFocusState`): after
       the "+" click the box's `FocusState` read **Pointer**, so `GotFocus` opened the list on every
       "+". The card's copy of the Agent Manager text did the same, through its late, asynchronous
       `TextChanged`.
     - That open came before the card was laid out at its place: a popup's child moves to new offsets
       only in the next layout pass (`CPopupRoot::ArrangeOverride`). The list read the card's
       previous place, or (0,0) on a window's first open, and covered the box and Launch. Whether
       the late focus event came before or after that pass decided "some cases".
   - **Since then** the bar reads how focus really arrived from the box's `GettingFocus` (synchronous,
     uncoerced; `_cwdBoxFocusArrival`), and a programmatic write (`SetBoxText`, a Browse… pick)
     opens no list (`_programmaticTextPending`). Both still hold with the list open by default: the
     card opens its list itself, after step 1, and a programmatic focus or write never does. An open
     list re-anchors after the next layout pass
     (`_ArmPathPickerReanchor`: a one-shot `LayoutUpdated`, then the queued re-fit), at every open
     and after every real card move (`_SidePanelPlaceLaunchCard` →
     `AgentLaunchBar::ReanchorPathPickerAfterLayout`). A re-anchor that moved the list writes
     `[launchbar] the folder list moved under its box after a layout pass (dx=… dy=… DIP)` to
     hooks.log.
   - **Side effects, both what the code's comments always said:** after Browse…, a closed list stays
     closed ("leave a closed one closed"), and on the Agent Manager page a selection that pre-aims
     the box no longer opens its list when the box happens to hold focus ("unfocused then, so no
     path-picker pop").
6. **Claude only.** The agent element is a fixed `● Claude` chip (the toggle's content, no button
   chrome, no click); its tooltip says Codex launches from here are planned. The bar never enters
   Codex mode.
7. **Closing:** a press OUTSIDE the card (a `Root()` `handledEventsToo` hook — the tag editor's
   recipe), Esc (with the drop-down closed), `+` again, a launch / resume / fork, a tab switch, a
   collapse, a dock flip (`⇄` here, or a settings broadcast that moved the dock). INSIDE counts the
   card, its drop-down (a separate `Root()` popup), the `+` button (its click toggles), and the
   content of any other open popup — the **Specify a model** prompt the Launch hover menu opens is a
   `Root()` popup too, and a click in it must not close the card whose launch it is about to finish.
   Logged `[nav] sidepanel launcher open (at the click | under the + button)` /
   `[nav] sidepanel launcher close (<reason>)`.
8. **"Same functionalities exactly like in agent manager tab":** the row IS the Manager's code — the
   launch bar was moved VERBATIM out of `AgentManagerContent.Launch.cpp` (and the toolbar / drop-down
   blocks of `_BuildLayout`) into one shared class, **`AgentLaunchBar`** (`AgentLaunchBar.{h,cpp}`),
   which the Agent Manager now hosts too. The same validation underline (green = a found session id,
   amber = a folder Launch will create, red = Launch disabled), the same drop-down (Browse…, RECENT /
   RECENT MATCHES, GIT WORKTREES, MATCHES / SUBFOLDERS, sized against the window), the same keys in
   the box (Esc closes the drop-down, Enter commits without launching), the same Launch hover menu
   (Default · Specify… · models · presets — a hover opens it at once, 2026-10-10; a touch waits a
   450 ms hold, so a tap launches without it), the same launch / resume / fork / "Create & Launch".
   The card's host calls the very page seams the Manager's handlers call (`_SpawnClaudeSession`,
   `_ResumeSessionFromDisk`, `_ForkSessionFromDisk`), with the page's own "Claude not detected"
   notice (`_PromptClaudeMissing`). Both bars push into ONE recent-folders list
   (`AgentManagerContent::LaunchRecentDirs`).
   - **ONE click on Launch launches (2026-10-19, in dev since 0.0.1.31, not in production) — the user's report:** "Can Launch
     Claude work exactly like it does now but on a single-click … instead of double-click". The
     hover menu is a light-dismiss `MenuFlyout`, and XAML EATS the press that dismisses one: the
     press closed the menu and never reached Launch, and the next pointer move over the button
     reopened the menu (PointerEntered opens it at once), so only a double-click — no move between
     its presses — got through. The note of 2026-10-10 said every press passed through (read in the
     WinUI 3 source, where a transient flyout makes the root visual its pass-through element); the
     installed builds do not behave so, and even an explicit `OverlayInputPassThroughElement` could
     not reach the card's Launch, which sits in ANOTHER popup (WinUI's re-hit-test disables the whole
     popup root). Production's hooks.log of 08:53: the card opened, then closed 4 s later on an
     outside press, with no launch.
   - **Now** the menu's `Closing` (raised synchronously while the press is processed — WinUI
     `CPopupRoot::CloseTopmostPopup`) ARMS the press when the physical primary button is down
     (`GetAsyncKeyState`, `SM_SWAPBUTTON`; `_NoteLaunchMenuClosing`), keeping the cursor's screen
     position; the menu does not reopen while a press is armed; and the release on Launch
     (`PointerReleased`, handledEventsToo — ButtonBase never saw the press, so it raises no Click)
     completes the click when the press point, mapped into Launch's coordinates through the cursor's
     screen delta and the island's rasterization scale, lies on Launch (`_OnLaunchButtonReleased` →
     `ClickLaunch`). An arm lives 3 s at most (`kLaunchEatenPressMaxMs`). Logged
     `[launchbar] Launch clicked while its hover menu was up: …`.
   - **A double-click launches once:** `ClickLaunch` ignores a call within the system double-click
     time of the previous one (`GetDoubleClickTime`; logged `[launchbar] a second Launch click within
     the double-click time was ignored …`) — the habit the eaten click taught.
   - **Not covered:** a press that a busy UI thread processes only after the physical release reads
     "up" and stays eaten (`GetKeyState` would see it, but it may never be updated by the island's
     pointer input, and a stale "down" would arm every close); a touch or pen press (no mouse button
     state). The same light dismiss still eats a first click anywhere else (the box, a folder row)
     while the hover menu is up.

**Keys act on the KEY-UP** (the `AgentModelPrompt.h` rule): the card's `PreviewKeyDown` arms
Escape (drop-down closed) or Enter on the focused Launch / Fork button and marks it handled; its
`PreviewKeyUp` acts. Closing the card moves focus to a terminal, and Windows still delivers the rest
of a keystroke — the CR of an Enter, the ESC of an Escape — to whatever holds focus next: a claude
would submit its unsent draft or interrupt its running turn. Ctrl / Alt chords are never ours.
Enter in the BOX stays the box's own commit (never a launch), as on the Agent Manager page. Tab
cycles inside the card.

**Focus back.** When the card held keyboard focus at its close (Esc, `+`, a launch, a press on a
surface that takes no focus), focus returns to the current tab on a Low-priority tick
(`_FocusCurrentTab`); a press on a focusable element (the terminal, a text box) keeps the focus it
took.

**The box width** is fixed while the card is open (Min = Max — the card does not grow while you
type): 504 DIP (the Manager's comfortable width), less when the window leaves less beside the chip,
Launch and Fork, never under 160 (`_SidePanelFitLaunchBox`).

**The look:** the tag editor's dark card (`#262626`, a `#3A3A3A` edge, 6 px corners), plus the
Agent Manager's six Button state brushes (`AgentLaunchBar::SeedButtonStateBrushes`): under the
forced-Dark theme the default `ButtonBackground*` brushes resolve to null, and a button would take a
click only on its text. The bar seeds its own drop-down panel with them too — that is what the
Manager's drop-down rows found on the Manager root before; in `Root()` there is none.

**Cost.** Built on the FIRST `+` click and kept for the window's life (`_SidePanelEnsureLaunchCard`);
a window that never opens it pays nothing. The two `Root()` hooks are inert while it is closed. A
press while it is open reads the open popups once and walks the press's ancestor chain once
(identities compared as raw `IUnknown` pointers).

**Check after the build:** `+` sits left of Sum and toggles the card; a click near the top-left
opens it down-right with its corner at the click, and after `⇄` (dock right) down-left; the card
opens with the caret in the box and its folder list OPEN right below the box (2026-10-19), never
over the box or Launch Claude, not even for one frame — the first "+" of a new window, a "+" after
selecting another tab (the Manager's text changed), a "+" after `⇄`, and a "+" while the box holds a
session id; a plain "+" writes no `[launchbar] the folder list moved under its box …` line to
hooks.log (one at every "+" would mean the layout pass before the open did not place the card);
Esc closes the list and keeps the card, a second Esc closes the card, and after that first Esc a
click in the box opens the list again; with the list open, a window resize that moves the card
and a typed session id that shows Fork keep the list under the box; after Browse… the list stays as
it was; the card
shows the Manager box's text and the same underline; typing opens the drop-down and mirrors into the
Manager's box (open the Agent Manager page beside it to watch); Browse… / a RECENT row / a GIT
WORKTREES row work; hovering Launch opens the model / preset menu at once (no wait), ONE click on
Launch while that menu is up launches the Default (2026-10-19: one
`[launchbar] Launch clicked while its hover menu was up …` line per such click in hooks.log), on
the card AND on the Agent Manager page, also with the pointer moved a little before the click; a
double-click starts ONE session (a `[launchbar] a second Launch click … ignored` line); a press on
Launch dragged off and released elsewhere launches nothing; and Specify… keeps the card open
until its pick launches; Launch, a typed session id's Resume and Fork each start the session and
close the card; Esc closes the drop-down first, then the card, and no ESC reaches the terminal (a
running claude keeps running); Enter on a focused Launch launches on the key-up and no CR reaches
the new tab; a click on the terminal, a tab, the side panel or `+` closes it; a window resize keeps
it inside the window; no `[exc] side panel launcher …` lines in hooks.log.

### 2f. Only the cards near the viewport exist, and a card adds no composition node (2026-10-20, in dev since 0.0.1.32, not in production)

The user's request, after the CPU report of UI_THREAD.md §13: "draw each card's rounded corners on a background
element instead of the card itself (about 80% of the redraw cost), then load only the cards on screen" — "Kindly
fix". Measured there on production 0.6.8.22: two windows show their panel GLOBAL, ~706 cards each, all built and
loaded even when scrolled out of view; every frame of a window walked 3,634 composition nodes, 81% of them
side-panel cards (two per card); the cards held ~224 MB of DirectWrite state. The why and the evidence:
UI_THREAD.md §14.

**1. The rounded shapes are childless.** XAML gives an element with a non-zero `CornerRadius` AND children its
own composition node, so that it can clip the children to the curve (WinUI `framework.cpp`,
`CFrameworkElement::UpdateRequiresCompNodeForRoundedCorners`), and every frame walks every node (§10h). The card
was a rounded `Border` holding the body, its band a rounded `Border` holding the title: two nodes per card. Now
each is a plain Grid with two layers in one cell:

```
card       (Grid, margin 4,2,4,2, NO corner radius)
├─ cardBack  (Border: #333 fill, 1 px border, radius 4, NO child)    ← the hover / selection ring is its BorderBrush
└─ body      (StackPanel, margin 1: inside the border, where the old Border laid out its child)
   ├─ bandArea  (Grid, NO corner radius)
   │  ├─ band     (Border: the folder color, top corners 3, NO child)
   │  └─ title    (margin 6,2,6,2 = the old band padding), or [ title * ][ tag strip ] in a grid with that margin
   ├─ state line
   └─ dir line
```

- A childless rounded Border draws the same shape as plain render data, with no node. Nothing needs the clip:
  the band rounds its own top corners, and no content reaches a corner.
- ⚠ Never give `cardBack` or `band` a child, and never set a `CornerRadius` on `card` or `bandArea`: a Grid has
  that property too, so the compiler would not object.

**2. Only the rows near the viewport have cards.** The list update still makes one model per LISTED session —
the rows (`_sidePanelRowIds` + `_sidePanelRowModels`, in sort order) — but only the rows of ONE contiguous range
have cards. The rows above that range are one top spacer of `first × stride` DIPs, the rows below it one bottom
spacer of `(count − last) × stride` (transparent Borders, so a wheel over a spacer still scrolls). The list is
therefore always `count × stride` tall: realizing other rows never changes the scroll extent, so a scroll in
flight (inertia, a thumb drag) is never disturbed.

- **The stride** is one row: a card's height + its 2 × 2 DIP vertical margins. Every card has the same shape
  (one-line texts of fixed sizes), and the stride is the MEDIAN of the realized cards' measured heights, so a card
  whose emoji title came from a taller fallback font cannot move it; it changes only when most cards change (a DPI
  or text-size change), by more than 0.05 DIP. Before any card was measured it is 60 DIPs
  (`kSidePanelCardDefaultStride`).
- **The policy** (`PlanRealizedRows`, `AgentMaster/VirtualListWindow.h`, pure): NEAR = the viewport ± max(half a
  viewport, 4 rows), FAR = the viewport ± max(one viewport, 8 rows). While the realized range covers the NEAR rows,
  nothing changes; otherwise the FAR rows are realized — so an edit runs about every half viewport of scrolling.
  A range larger than twice FAR (a viewport that shrank) is trimmed to FAR. A viewport with no height yet (the
  first update runs before the first layout) counts as 720 DIPs. For the release's 706-row list under a ~600 DIP
  viewport that is 20–30 cards instead of 706.
- **When it runs.** The ScrollViewer's `ViewChanged` plans on every offset change (a few multiplications) and edits
  the list only when the plan changed — synchronously: ViewChanged is raised after the offset changed, never
  inside a layout pass, and a queued edit would leave a frame of blank spacer during a fast wheel scroll. Its
  `SizeChanged` (the "Sum" toggle, a splitter drag, a window resize) queues one Low-priority re-check, and so does
  every pass that built cards (`_SidePanelQueueRealize`): their heights are known only after the next layout pass,
  and the re-check measures them. A list update plans too, and writes the new models into the realized cards that
  stay.
- **The edit** (`_SidePanelSyncRealizedCards`) is §2c's, over the realized rows. A row that leaves the range loses
  its card (with "Sum" off, its rich tooltip is shut down first — §2d); a row that enters gets a new card from its
  stored model (with "Sum" off, its rich tooltip is armed from `GetWithoutQueue`, a lean one-session read). A
  scroll pass writes no model (the realized cards already show their rows'). A mismatch or a throw resets the list
  to empty and marks it dirty, as in §2c (logged `[sidepanel] card list … pass ended out of step …` /
  `[exc] _SidePanelRunRealizePass`).
- **The ⌖ reveal** (§2a) of a row without a card first realizes the rows around it (`RowsAroundRow`: both
  viewports a minimum scroll can end in — the row at the top edge or at the bottom edge — plus the FAR margin),
  then scrolls with `StartBringIntoView` WITHOUT the animation: an animated scroll across hundreds of rows would
  realize and drop cards at every offset on its way. The plan after that scroll keeps the range, so the reveal
  needs no second pass (tested).
- **What it changes elsewhere:** the selection restyle, the hover ring, the tag strip and the "Sum" switch act on
  the realized cards; a row realized later gets its look (selected or not) from its first, full apply. UI
  Automation (Narrator, Inspect) sees only the realized cards.
- **Known limits:**
  - The rows are taken to be equal. A card taller than the stride (a fallback-font title) shifts the content by
    the difference when it leaves the range ABOVE the viewport: a few DIPs, at most once per pass.
  - A stride change (DPI, text size) moves the content above the viewport by `first × Δstride`. Not compensated.
- **Tested** (no build): `TestVirtualListWindow` (`tests/tests_sidepanel.cpp`) — the row arithmetic at exact
  boundaries, the clamp of an offset the shrunk content no longer allows, the hysteresis on the release's shape
  (a range is kept for half a viewport of scrolling and not one DIP more), the trim, the reveal range and its
  no-second-pass property, the spacer total, the median stride; a 20,000-case reveal sweep, a 300-list × 400-step
  scroll simulation (every visible row always has its card, every plan is a fixed point, no plan larger than
  twice FAR) and one-row-per-step scrolls (at most one re-plan per 4 rows). The 17 translation units that include
  `TerminalPage.h` pass `cl /Zs`, with a negative control inside `_SidePanelSyncRealizedCards`.
- **Check after the build:**
  - The cards look as before: the border, the band's corners, the ring on hover and selection, the tag ribbons
    hung from the band's top edge.
  - In the 556-tab window's GLOBAL list, a wheel scroll, a thumb drag and a scroll by keyboard show no blank band
    and no jump; ⌖ reaches a far tab's card in one jump; a sort or filter change lands on the right cards; with
    "Sum" off a card scrolled into view shows its rich card on hover.
  - `tools/leakhunt/xamltree.py nodes` on a dump: no node on a side-panel card (production had 2 per card); the
    UI thread's frame walk (`stacksnap sample` + `uiclass.py`) far below ~250 ms/s.
  - No `[sidepanel] … out of step` line in hooks.log.

## 3. BOTTOM — the selected session's summary (a fill-hosted AgentTabOverlay)

The bottom area hosts a REAL `AgentTabOverlay` whose summary panel is switched into the new
**fill-host mode** (`SetSummaryFillHost`, called before `Initialize`): the per-tab pencil
panel's entire pipeline — off-thread `AnalyzeSessionTranscript` + `RenderSummaryBox`/
`RenderCodexSummary`, the live times bar, the wrap/truncate/whole-conversation toggles, the JUMP
buttons, the context menus — reused verbatim, with four differences:

- it stretches to the HOST slot (fractions + the 50%-of-pane cap ignored; the body viewport's
  MaxHeight tracks the slot height minus the panel's own chrome, change-gated + re-applied on a
  deferred tick so it can never layout-cycle);
- the resize GRIPS are never built (the panel's splitters own sizing; a grip's drag-persist
  would corrupt the per-tab GLOBAL fractions);
- it is never dimmed (`SetOverlayOpacities(1,1)`) and always enabled — independent of the
  per-tab pencil global;
- it draws NO FRAME (2026-10-05): no border, square corners, and a transparent fill, so the
  bottom area's own `#252525` shows through and the summary reads as the area's content. The
  per-tab panel keeps its 1 px 25%-white border, 4 px rounded corners and `#202020` fill, which
  mark a card floating over a terminal. The fill is transparent, not null: a `Border` without a
  background is not hit-testable in its empty parts, and the right-click menu must still open on
  the gaps between rows. Square corners also drop the rounded-corner clip, which XAML backs with a
  composition node (UI_THREAD.md §10h).

With **Sum** OFF (§2d) this area and its splitter are hidden and the overlay is RELEASED (no
registry observer, no analyses); turning **Sum** back on rebinds it to the current selection.

It is DELIBERATELY not in `_claudeOverlays` (that map is keyed by session id and may already
hold the session's own per-tab overlay), so `_ToggleSummaryWrap`/`Truncate`/`History` and the
settings broadcast push the flags to it explicitly (`_SidePanelApplySettings`). Rebinding to a
new session replaces the overlay (the old one's registry observer detaches on release); a
same-id rebind just kicks the cheap mtime-gated `RefreshSummaryContent`.

**The whole conversation.** The Messages list reads as ONE conversation: every prompt of the
sessions this one continues — a fork's parent, a plan-mode restart's parent, a `/clear`
predecessor, a `/handover` origin, followed **upstream only** — numbered 1..N straight through,
with a thin separator naming each earlier session's id (first 8 characters) after its last
prompt. Clicking a separator copies that full id and pops "Copied session id of up to here".
Because the side panel builds a NEW overlay at every selection, the chain walk is cached
engine-side (`CollectConversationUpstream`'s process-wide cache), never per overlay — re-selecting
a card re-reads nothing. TAB_OVERLAY.md §13e has the rules.

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
board/tree/Prompt Queue keeps its own deliberate selection. The same-id early-out in
`_SidePanelSelectSession` makes the steady state (already on the focused tab) free, so hooking
these high-frequency events costs nothing at rest.

## 4. Persistence — per window, last-one-wins defaults

The user contract: **both separators persist PER WINDOW (not per tab), and the last one to
resize sets the default for new windows.** Collapse and the **Sum** summary area follow the same
rule; the dock side is GLOBAL-only; the **card scope is per-window ONLY** (no global default — see
below).

| state | per-window (windows/<id>.json) | global default (settings.json) |
|---|---|---|
| panel width | `WindowRecord::sidePanelWidthFraction` (0 = unset) | `AppSettings::sidePanelWidthFraction` (0.22) |
| cards/summary split | `WindowRecord::sidePanelSplitFraction` (0 = unset) | `AppSettings::sidePanelSplitFraction` (0.5) |
| collapsed | `WindowRecord::sidePanelCollapsed` (-1 = unset) | `AppSettings::sidePanelCollapsed` (false) |
| summary area hidden (**Sum** off, §2d) | `WindowRecord::sidePanelSummaryHidden` (-1 = unset; written only when set; a non-bool value reads unset) | `AppSettings::sidePanelSummaryHidden` (false = **shown, the default** — the pre-toggle layout) |
| dock side | — | `AppSettings::sidePanelOnLeft` (true = **left, the default**; false = right) |
| card scope (LOCAL/GLOBAL) | `WindowRecord::sidePanelGlobal` (absent = LOCAL; written only when GLOBAL) | — **none by design**: every new window starts LOCAL |
| `⌖` scroll to the selected tab (§2a) | — | `AppSettings::sidePanelScrollToSelectedTab` (true = **ON, the default**; absent key = ON) — a GLOBAL preference, broadcast live to every open window |

- **The card scope is the one field with NO last-one-wins export**: `_SidePanelToggleScope`
  persists it through the window-record autosave alone — never `_SidePanelPersistDefaults`, and
  there is no `AppSettings` field to seed from. A window that flips to GLOBAL keeps that for
  itself; a new window (or a pre-feature record, where the key is absent) starts LOCAL. This is
  what guarantees one window's fleet-wide view can never leak into another window's list — the
  bug that motivated the scope ("the upper list shows the tabs of a different window").

- Every drag release / collapse toggle / **Sum** click: update the live members → the window-record
  autosave captures them (`_CaptureWindowRecord`) → a GRANULAR freshest-disk RMW exports ONLY the
  adjusted field as the new global default (`_SidePanelPersistDefaults` — a width drag never
  exports this window's collapse state, and a **Sum** click exports only `sidePanelSummaryHidden`).
- A new window (or a pre-feature record's unset field) seeds from the globals at
  `_InitAgentSidePanel`; a window that made its own choice keeps it.
- Bands everywhere (drag clamps == load-time checks): width (0.10, 0.50], split (0.15, 0.85).
  A record's out-of-band value degrades to UNSET (the global default); a settings out-of-band
  value degrades to the shipped default — a corrupt file can never collapse the terminal.
- All six AppSettings fields are written OUTSIDE the cog form, so BOTH cog-Save preserve
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

- The list is VIRTUALIZED (§2f): only the cards near the viewport exist (20–40 in a ~600 DIP list,
  whatever the session count), and a scroll edits the list only about every half viewport. The card's
  and the band's rounded backgrounds are CHILDLESS Borders, so a card adds no composition node to the
  walk XAML runs on every frame of the window (UI_THREAD.md §14).
- Updates: batched-drain trigger + 500ms min gap + collapsed-defers; the list is edited in place
  with the fewest removes / inserts / moves, and a card gets only its changed values written (§2c) —
  never `Children().Clear()` + rebuild (the reset after a failed edit is the one exception); the
  timing refresh is the same in-place update; no eager flyouts on cards (menus built at
  right-click); `SnapshotLiveForCards` only (never `Snapshot`, never a per-card `Get`); ONE
  `LoadDirColorIndex()` snapshot per update for every card's color; the tags from a per-card cache,
  re-read only when `SessionTagsEpoch` moved or for a card new to the list, with `tag-colors.json`
  read once per epoch and only when a rendered card is tagged (§2b); hover and selection restyle from
  the card's own model, with no registry read per pointer event.
- The `⌖` reveal is one coalesced Low-priority tick per tab selection (a map lookup + one
  `UpdateLayout` + `StartBringIntoView` when owed; nothing at all when not), never a per-update
  re-scroll, and never a focus move — the terminal keeps typing focus.
- Layout mutations from layout-driven triggers (ContentArea/root SizeChanged) go through
  coalesced dispatcher ticks with change-gated writes (the E_LAYOUTCYCLE lesson); pointer-drag
  applies synchronously (input, not layout).
- Card hover handlers reach the card only via `sender` — capturing the element in its own
  handler is a self-cycle that leaks every card ever dropped from the list (the board's
  weak-capture lesson). A sender that is not the card registered for its session (a card already
  dropped, or replaced by a reset) is ignored.
- The periodic tick is a terminate-netted `fire_and_forget` over an awaitable Impl
  (`resume_foreground` first), the `_ScanCacheWarmTabs` idiom.
- Tooltips via `AgentSetTip`/`AgentSetTitledTip` only (the per-element ToolTip leak rule) — with ONE
  exception: the rich card of the **Sum** OFF mode (§2d), a `ToolTip` attached to the card for its
  life through `AgentSessionToolTipHost`, the tab tooltip's own crash-hardened lifecycle. It is shut
  down (detached) before its card leaves the list or the list resets, its floating tips are silenced
  meanwhile (`AgentSuppressTip`), its arm-build reads no transcript, and every handler it wires holds
  the page and the host weakly.
- The `+` launcher card (§2e) is built on the first click and never rebuilt; its placement runs on a
  coalesced dispatcher tick (never inside `SizeChanged`), writes the popup offsets only on a real
  move, and its keys act on the KEY-UP, so no CR / ESC of a key that closes the card can reach a
  terminal. Every handler it wires holds the page weakly; the launch bar's own handlers hold the bar
  weakly; `~TerminalPage` runs the bar's noexcept `Shutdown` (its hover timer and hover menu are
  UI-affine).

## 6. Files

- `TerminalPage.AgentSidePanel.cpp` (new TU; includes `AgentManagerContent.Internal.h` for the
  board's shared helpers), `TerminalPage.xaml` (the ContentArea wrapper), `TerminalPage.h`
  (members + decls, and the card's `AgentSidePanelCardModel` / `AgentSidePanelCardView` structs —
  §2c), seams in `TerminalPage.cpp` (`_OnFirstLayout`),
  `TerminalPage.AgentEngine.cpp` (drain / liveness / funnels / sink / broadcast),
  `TerminalPage.AgentObserver.cpp` (toggle loops + the Tags panel's `_SidePanelNoteTagsChanged`
  calls), `TerminalPage.AgentWindowRecord.cpp` (capture), `AgentManagerContent.h` (the two public
  wrappers — `BuildSessionMenuFor` takes the card as its Tags-panel anchor),
  `AgentManagerContent.Settings.cpp` (cog preserve), `AgentTabOverlay.{h,Summary.cpp}`
  (fill-host mode), `AgentMaster/SessionModels.h` + `Persistence.cpp` (fields + JSON),
  `tests/tests_persistence.cpp` (round-trip / defaults / bands). The in-place card list (§2c):
  `AgentMaster/KeyedListDiff.h` (the planner, pure), `AgentMaster/SessionRegistry.{h,cpp}`
  (`SnapshotLiveForCards`), `AgentMaster/SessionStore.{h,cpp}` (`SessionTagsEpoch`),
  `AgentMaster/Persistence.{h,cpp}` (`DirColorIndex` / `LoadDirColorIndex` + the 3-argument
  `ResolveSessionColorHex`), `tests/tests_sidepanel.cpp` (`TestKeyedListDiff`). The **Sum** mode
  and its rich card (§2d): `AgentSessionToolTip.{h,cpp}` (the tooltip lifecycle host, extracted from
  `Tab`), `Tab.{h,cpp}` (now forwarders over one host), `TerminalPage.AgentObserver.cpp`
  (`_PushSessionToolTipCard`, `_ScrollSessionToolTipBody`, the per-session summary waiters, the
  surface-keyed signature + scroll state, `TtBuildTooltipCard`'s width parameter),
  `AgentTipHelpers.h` (`AgentSuppressTip`), `TerminalAppLib.vcxproj` (the new TU),
  `AgentMaster/SessionModels.h` + `Persistence.cpp` (`sidePanelSummaryHidden`, both halves) +
  `tests/tests_persistence.cpp` (round-trip, legacy absent, committed false, non-bool, unset
  omitted, settings default). The timing line (§2): `AgentMaster/SessionTimingText.h` (the pure
  formatter), `AgentManagerContent.Internal.h` (`FormatSessionTiming` — the wall-clock wrapper every
  card and row calls — and the `kTimingTooltip` text), `tests/tests_sidepanel.cpp`
  (`TestSessionTimingText`). The `+` launcher (§2e): `AgentLaunchBar.{h,cpp}` (the shared launch
  bar — the Agent Manager's launch row, moved out of `AgentManagerContent.Launch.cpp` and
  `_BuildLayout`), `AgentManagerContent.Launch.cpp` (now the Manager's HOST half: `_MakeLaunchBarHost`,
  the forwarders, `LaunchBoxText` / `SetLaunchBoxText`), `AgentManagerContent.{h,cpp}` (the bar
  member, the shared `LaunchRecentDirs`), `AgentMaster/PointerCardPlacement.h` (the placement, pure),
  `TerminalPage.AgentSidePanel.cpp` (the button, the card, its keys, hooks, fit and placement),
  `TerminalPage.AgentEngine.cpp` (the tab-switch close, the destructor's `Shutdown`),
  `TerminalPage.h` (members + decls), `TerminalAppLib.vcxproj` (the new TU + headers),
  `tests/tests_sidepanel.cpp` (`TestPointerCardPlacement`). The virtualized list and the childless
  rounded shapes (§2f): `AgentMaster/VirtualListWindow.h` (the realized-rows policy, pure),
  `TerminalPage.AgentSidePanel.cpp` (`_SidePanelSyncRealizedCards`, the scroll / measure / reveal passes,
  the spacers, the card's two layers), `TerminalPage.h` (the rows, the spacers, the realized range, the
  stride, the card view's `cardBack` / `bandArea`), `tests/tests_sidepanel.cpp` (`TestVirtualListWindow`).
