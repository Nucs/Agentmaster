# The tab strip

How Agentmaster draws and drives Windows Terminal's tab strip: the indicators on each tab header, the
tab menu, tab size, revealing a tab, and dragging tabs. Code: `TabHeaderControl.{xaml,h,cpp}`,
`TerminalTabStatus.{h,idl}`, `Tab.{h,cpp}`, `TabManagement.cpp`, `TerminalPage.AgentEngine.cpp` (the
drag gesture), `AgentMaster/TabDragMath.h` (the pure placement math).

## The header

- Every classified tab's header reads `[icon] ● <title>`. The dot is a state-colored `Ellipse` with a
  thin black stroke in `TabHeaderControl.xaml`'s indicator row (dot-wrap margin `0,1,6,-1`; a
  `TabViewItemHeaderIconMargin` override `0,0,4,0` on the `TabView` in `TabRowControl.xaml`), bound to
  `TerminalTabStatus.AgentStatusVisible` / `AgentStatusBrush` and set idempotently by
  `_SetTabAgentDot(tab, color?)`, which also hands over the state word for UI Automation. Tab icons are
  hidden by default (`showTabIcon`).
- **Colors** (`AgentStatusColors.h`, `AgentStatusColorFor`): a managed session wears its Triage-Board
  state color — Running blue · Waiting goldenrod · NeedsApproval orange-red · Error crimson · Done green ·
  Idle gray; a managed Codex at its 3-state floor (Running · Waiting · Idle). An observed, unmanaged tab
  (pwsh / cmd / a claude not yet prompted / an external codex) wears a dim gray dot. ⚠
  `AgentManagerContent.cpp` keeps its own identical `StateColor` table for the board: change both.
- **The flash ring (unread):** when a session leaves Running for Idle, WaitingForInput or NeedsApproval
  (not Done or Error) on a tab that is not the focused tab of the active window, its dot grows a ring
  drawn behind it (`TerminalTabStatus.AgentFlashRingVisible`, `_EvaluateAgentFlash`). Visiting the tab
  clears it (`_VisitTabClearFlash`, from `_OnTabSelectionChanged`), and so does activating a background
  window whose selected tab rang (`_OnAgentWindowActivated`). The tab menu's Mark Unread sets it by
  hand. A session archived and later restored forgets its last state, so it never flashes spuriously.
  The ring's color and opacity come from `flashRingColor` (`#AARRGGBB`, default `#CCFF0000`) through the
  per-window `_flashRingBrush` (`_RefreshFlashRingBrush`, live on Save and across windows). Every ring
  blinks in lockstep, 600 ms on / 600 ms off: the page sets the flag once per flash and the header runs
  `AgentLockstepBlink`, a composition expression on the ring's `Opacity` that follows one shared clock
  per UI thread (`TabHeaderControl::_UpdateFlashRingAnimation`). ⚠ Never set the ring's `Opacity`: a
  static set cancels the blink, and it is how the blink ends. `_OnAgentFlashTick` (600 ms) only
  re-asserts rings, prunes closed tabs and publishes the unread census (the taskbar count).
- **The 3 dots** of an unsent draft sit below the dot (`HeaderPendingDots`, `AgentPendingVisible` /
  `AgentPendingBrush`, `_SetTabPending`). They pulse (`AgentDotsPulse`) only on a started tab whose
  header is in the live tree, with Windows' animation effects on; otherwise they stand still (a dormant
  tab's remembered draft). Their color is the light/dark pair picked by the tab color's luminance
  (`PendingDotsColorFor`, PENDING_INPUT.md).
- **The spark crown** (`HeaderCacheWarmGlow` + `HeaderCacheWarmSparks`): a soft amber glow behind the dot
  and three embers rising from it while the session is at rest and its server-side prompt cache is still
  warm — the tab twin of the board's ⚡ glyph, from the same predicate (`ServerCacheStillWarm`,
  `serverCacheMinutes`, default 5; never while Running). It is polled on the scanner's ~2.5 s tick
  (`_ScanCacheWarmTabs` → `_SetTabCacheWarm` → `AgentCacheWarmVisible` / `AgentCacheWarmBrush`), because
  warmth ends on a clock, not an event. The glow loops through `AgentOpacityLoop` (two concentric flat
  discs in one Grid, breathing together); the embers are composition visuals (`AgentEmberSparks`)
  colored by a fire pair picked against the tab's rendered background (`CacheWarmSparkColorFor`:
  `#FFC107` on a dark tab, `#B02A00` on a light one; copied by `_SyncCacheWarmEmberColor`). The embers
  are drawn last (over the favorite crown), the glow first (under the flash ring); each ember rests at
  opacity 0 and rises ≤ 3 DIP inside the 18 px cell. Animated only while the header is in the live tree
  and Windows' animation effects are on (a still crown = the steady glow, embers hidden).
- **The favorite marker** (FAVORITES.md §5a), chosen by `favoriteIcon`: **Crown** (default) — a gold
  (`#F5C242`) `Path` crown at the dot's north-west, tilted, drawn over the dot — or **Star** — the dot as
  the center of a white, gold-tipped five-point star drawn behind it. `HeaderAgentFavoriteCrown` /
  `HeaderAgentFavoriteStar`, mutually exclusive through `AgentFavoriteVisible` /
  `AgentFavoriteStarVisible` (`_SetTabAgentFavorite`, `_RefreshTabFavoriteCrown`,
  `_RefreshAllFavoriteIcons`, from the durable `IsSessionFavorite`). Never a title prefix (Rule #11).
- **The selection pill:** a translucent accent pill behind the tab (`HeaderAgentSelectionPill`,
  `AgentSelectionVisible` / `AgentSelectionBrush` at ~40% alpha, `_SetTabSelectionPill`) while the
  Agent Manager page shows and the session is hovered or selected there.
- **Tag ribbons** hang ~70% below the tab's bottom edge from an always-open parented popup
  (`HeaderTagBookmarksPopup`; [TAGS.md](TAGS.md)).
- **The rich tooltip** (a session-aware hover card) is `AgentSessionToolTipHost`
  ([HANDOVER_tab-tooltip.md](HANDOVER_tab-tooltip.md): its lifecycle, invariants and crash history);
  its tag row's opacity is `tooltipTagsOpacity` (0.9).
- The side panel's cards wear the same icon through `AgentStatusGlyph`, the code twin of
  `HeaderAgentStatusDotWrap` (one blink clock; `AgentMakeCacheWarmGlow`; motion rules in
  `AgentMaster/StatusIconMotion.h`). ⚠ A change to the wrap's elements or to
  `TabHeaderControl::_Update*Animation`'s gates must be made in the twin too.

## The tab menu

Agentmaster's items in a tab's context menu (shown for a managed session unless noted; visibility set
when the flyout opens): Activate Tab and Activate All Tabs (N) for dormant tabs, Mark Unread, Open In
Explorer (`OpenSessionFolder`: the effective folder), Copy ▸ (`CopySessionField`), Show Transcript, Tags,
Favorite / Unfavorite, New Session Here ▸ and Fork session ▸ (Default, `Specify…`, the launch models, the
presets; a shell tab's Fork session is Windows Terminal's duplicate tab), Restart session, Deactivate ▸
and Close ▸ (Close tabs to the left / right, Other tabs, Of Same Folder, Other of Same Folder, All tabs,
★ Favorite & Close All Tabs). Their behavior: [AGENT_MANAGER.md](AGENT_MANAGER.md) (*Closing sessions*,
*Deactivating sessions*). In the Remove-colors mode Change tab color and `openTabColorPicker` are
disabled on every tab; the color flyout opens with its Custom and Advanced sections expanded
(`tabColorPickerCustomOpen`, `tabColorPickerAdvancedOpen`). The close button and middle-click close
follow `showTabCloseButton` and `closeTabOnMiddleClick`.

## Size

- **Settings** (GLOBAL; applied live on Save and across windows by `_ApplyTabSizingSettings`, and at
  engine init before the first tab; logged `[tab-size]`): `tabWidthMode` — Windows Terminal setting
  (the default: Terminal's own `tabWidthMode`) · Equal width · Fit to title · Compact; `tabMinWidth` /
  `tabMaxWidth` (130 / 240 px, band 40–1000; the max is raised to the min, `NormalizeTabWidthRange`);
  `tabTitleFontSize` (12, 8–28) and `tabTitleFontWeight` (Normal / SemiBold / Bold; the rename box
  follows); `tabMinHeight` (32 px, 20–120). A "Reset tab size to defaults" link resets the form.
- **How each lever lands:** Equal width re-publishes the `TabViewItemMinWidth` / `TabViewItemMaxWidth`
  application resources at the root of `Application.Resources` (`_PublishTabWidthBounds`; MUX reads
  them on every width pass) and forces a width pass (`_ForceTabStripWidthPass`, then
  `_ClearTabStripListMaxWidth`); the resources are process-wide, so a per-window record
  (`_tabSizingApplied*`) decides the forced pass in a window reached by the broadcast. Fit to title and
  Compact set per-item `MinWidth` / `MaxWidth`; the height is per-tab `MinHeight`; the font is
  `TabHeaderControl::ApplyTitleFont` on the title and the multi-line reserver TextBlocks. A default
  clears Agentmaster's local value, so the XAML styles govern. The effective mode also reaches
  `Tab::_UpdateHeaderControlMaxWidth` (the rename box's cap) through `SetTabWidthModeOverride`.
- **The title stays inside its tab:** it sits in the star column of the header's 2-column title row
  (`HeaderTitleRow`) with `TextTrimming="CharacterEllipsis"`, so a long title ends in "…" and the close
  button and separator stay visible; the rename box shares that column (MinWidth 40). In Equal width
  the selected tab is held at the maximum width while its rename box is open (`RenamingTabWidth`, 200 px
  floor, `Tab::RenamerOpenChanged` → `_ApplyTabSizingToTab`).
- **Equal width never widens a tab for its title** (min / max only clamp the share every tab gets);
  Fit to title does, up to the maximum. A hint under the sliders says so (`_UpdateTabWidthHint`).

## Revealing a tab

- **The strip virtualizes:** its `ItemsStackPanel` derealizes far-off tabs (windows hold hundreds), and
  `StartBringIntoView` silently does nothing on a derealized `TabViewItem`.
- **The strip ignores bring-into-view requests:** `_EnsureTabStripScrollViewer` sets
  `BringIntoViewOnFocusChange(false)` on its ScrollViewer when it binds it (one `[strip] bring-into-view
  off` line per window). MUX's `OnItemsPresenterSizeChanged` → `BringSelectedTabIntoView` runs on every
  content resize, and a big strip's content resizes while the user scrolls. XAML acts on a request from
  inside the ScrollViewer only when the request is forced or `BringIntoViewOnFocusChange` is true;
  `ChangeView` and `ListView::ScrollIntoView` are not such requests.
- **`_RevealTabInStrip` is the only way the strip scrolls to a tab.** The tab-switch funnel
  (`_UpdatedSelectedTab`) calls `_RevealTabInStrip(tab, leaveFullyVisibleInPlace=true)`: ScrollIntoView
  plus a precise pass on Low-priority retries for a far tab; a tab already wholly on screen never moves.
  The placement is the pure `DecideTabStripRevealOffset` (`TabDragMath.h`, `TestTabStripReveal`),
  measured viewport-relative (MUX 2.8.4 puts the `<` / `>` buttons beside the viewport, not over it);
  `_revealTabStripGeneration` stops an older retry chain. Selecting the tab that is already selected
  (`_FocusClaudeSessionTab`) and a card click on the Manager page (`_RevealClickedSessionTab`) reveal
  explicitly; a renaming tab is kept on screen after every size change (`_TrackRenamingTabInStrip`);
  after a Deactivate the strip is kept where it was (`_KeepTabHeaderInView`).
- **The scroll extent is an estimate:** the panel records one width per laid-out index
  (`StackingLayoutStrategyImpl::RegisterSize`) and estimates every other tab at the average
  (`EstimateNonGroupedExtent`); a record goes stale when a width changes while its tab is not laid out,
  so the extent changes while the user scrolls and an offset never maps steadily to an index.
- ⚠ Never `StartBringIntoView` a tab; never compute a scroll offset from an index; never `ScrollIntoView`
  a tab the strip has not laid out yet (the next measure anchors it at index × average width).
- **New tabs:** `_InitializeTab` selects the new tab with the tab-switch funnel held
  (`_holdTabSwitchForNewTab`), settles the strip, then runs the held switch. Settle only with the tab that
  must end up on screen already selected, and never right after a new tab's content swap (its first
  layout would start its terminal before `_LaunchClaudeSession` wires the session). Inside
  `_OnFirstLayout` every settle is a no-op (the layout manager's reentrancy guard), which keeps a window
  restore fast and its tabs dormant.
- **Moves and removals:** `_TryMoveTab` suppresses the funnel while the tab is out of `TabItems`
  (`_movingTab`), so the momentary neighbor selection marks nothing read and wakes nothing. `_RemoveTab`'s
  successor selection bypasses the funnel (`_removing`) and calls `_ApplyAgentTabSelection` itself. ⚠ Add
  any new per-selection effect to `_ApplyAgentTabSelection`, never inline in `_OnTabSelectionChanged`.

## Dragging tabs

- **Native MUX drag is off:** `CanReorderTabs(false)` + `CanDragTabs(false)` (`TerminalPage::Create`,
  the configuration Windows Terminal ships for elevated windows), and every `TabViewItem` is pinned
  `CanDrag(false)` by `_ApplyTabDragPolicy`. The reason is CLAUDE.md's Gotcha *MUX TabView drag-start
  null-deref*: any native drag on a scrolled, overflowed strip can crash inside MUX.
- **Reorder and tear-out are a pointer-owned gesture** (`_WireTabReorderGesture` / `_TabReorder*`):
  press a header (mouse or pen; touch keeps native strip panning); past a 6 DIP threshold the pointer is
  captured on the TabView, and an accent insertion caret (parented in the TabView's template root, since
  with tabs in the titlebar the tab row is not under `Root()`) tracks the midpoint rule over the realized
  headers only (`DecideTabInsertionSlot`), with edge auto-scroll (per move, plus a 50 ms hold-still
  `SafeDispatcherTimer`). Nothing moves until the release, then one `_TryMoveTab` (UIA announce, settle).
  A release clear of the strip tears the tab out into a new window through the native downstream
  (`_sendDraggedTabToWindow`); Esc cancels.
- **Docking into another window:** a release over another Agentmaster window's tab-strip row moves the tab
  into that window at the aimed slot, and the target's own caret previews the slot while the drag hovers
  its strip. Each window registers a dock target (`Engine::RegisterWindowDockTarget`: its top-level HWND
  and a probe; `FindWindowDockProbe`; detached in `~TerminalPage`); the dragging window finds the
  top-level window under the cursor (`WindowFromPoint` → `GA_ROOT`, z-order-true, its own excluded) and
  calls that window's probe synchronously (every window lives on the Emperor's one UI thread). The probe
  (`_DockProbeHit`) maps screen → client → island DIPs → TabView-local, hit-tests the strip row (y only,
  12 DIP slack; a drop right of the tabs appends) and answers the slot with the same `TabDragMath`; in
  preview mode it lights the target's caret (`_UpdateDockPreview` / `_ClearDockPreview`). The send is the
  native downstream: `_sendDraggedTabToWindow(<window id>, slot)` → `_MoveContent` →
  `AppHost::_handleMoveContent` → the target's `AttachContent` + `_TryMoveTab`; then the target window is
  foregrounded. A foreign process's window or any failure reads as a miss and tears out.
- The OLE receive side (`AllowDropTabs`, `_onTabStripDragOver` / `_onTabStripDrop`) and the native drag
  handlers stay wired and inert.
- **Logs:** `[nav] tab-drag-begin <ident>` ↔ `tab-drag-end` (`from=N to=M` / `(docked into window <id> at
  slot N)` / `(no same-window reorder)` / `(torn out to a new window)` / `(cancelled: esc|capture
  lost|tab closed mid-drag)` / `(tear-out refused: not a terminal tab)`; a begin with no end = died
  mid-drag), `[nav] tab-move <ident> idx=N -> M`, `[nav] tab-send-to-window <ident> win=<id|-1> idx=N`,
  `[tab-new] idx=N tabs=M <ident>`, `[tabdrag-guard]` (`<ident>` = `_DescribeTabForLog`).
- ⚠ Route every `TabItems()` mutation through `_SettleTabStripLayout()` (an `UpdateLayout()` after
  `_InitializeTab` / `_RemoveTab` / `_TryMoveTab`) and `_ApplyTabDragPolicy(tvi)`.

## Header layout rules

- **A `TabViewItem` never clamps its children's desired size:** XAML treats each item of a ListView on an
  ItemsStackPanel as a non-clipping subtree (`FrameworkElement::MeasureCore`;
  `ListViewBase::PreProcessContentPreparation`). `Width` / `MaxWidth` still clamp and a
  `ScrollContentPresenter` clamps itself, but a TextBlock in a horizontal StackPanel reports its whole
  text; the tab then arranges its content at that width and clips it at its own. Bound every header
  child that can grow (a Grid star column + `TextTrimming`, or `MaxWidth`).
- **A `TabViewItem` takes every mouse or pen LEFT press before your handler:** XAML runs the class
  handler first, and `ListViewBaseItem::OnPointerPressed` marks a left press handled (right and middle
  presses stay unhandled); the selection happens on the release (`OnPointerReleased` performs the queued
  tap unless the release arrives handled). Register with handledEventsToo
  (`AddHandler(UIElement::PointerPressedEvent(), box_value(PointerEventHandler{…}), true)`), tell a press
  on an inner control (the close button, the rename box) from a press on the tab by walking
  `OriginalSource` up to the item (`ResolveTabPressTarget`), and keep a click from selecting by marking
  its release handled below the item (a one-shot `PointerReleased` on the template root,
  `_ArmTabShiftActivateRelease`). Shift+Click on a dormant tab activates it in place. Every ListView /
  GridView item behaves the same.
- `_updateAllTabCloseButtons()` re-applies the theme's close-button visibility to every tab on every
  theme or settings apply: a tab that must keep `CloseButtonVisibility::Never` needs a skip there.
