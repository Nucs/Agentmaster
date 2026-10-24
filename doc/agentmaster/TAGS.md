# Bookmark tags

User-named, colored labels on a session, shown as small bookmark ribbons on its tab and everywhere the
session appears. Code: `AgentMaster/SessionStore.{h,cpp}` (storage, the scan cache),
`TerminalPage.AgentObserver.cpp` (the tab spec, the Tags panel, the hover panel), `TabHeaderControl`
(the tab ribbons), `AgentManagerContent.Board.cpp` (the board's ribbons and chips),
`TerminalPage.AgentSessionsPage*.cpp` (the Sessions column and chips), `TerminalPage.AgentSidePanel.cpp`
(the side-panel ribbons), `AgentStatusColors.h` (the palette).

## Model

- A tag is a durable, case-insensitive name (`NormalizeTagName` / `FoldTagName`); a session carries any
  number of tags.
- Three files in the active profile: the per-session list (`session-store/<sid>.json` key `tags`,
  JSON-array-encoded), the known-tag registry `tags.json` (a case-insensitively deduped list of
  display-cased names that keeps a tag no session carries) and `tag-colors.json` (folded name →
  `#AARRGGBB`, the picked colors).
- The universe is `CollectGlobalTags(sessions ∪ registry)`: fold-merged, each tag stamped with its
  carriers' newest activity and its carrier count, most active first; a registry tag nobody carries
  lists at ·0, last.
- **Colors:** every surface resolves a tag's color through `ResolveTagDisplayColor` (a user-picked color
  from `tag-colors.json` > the stable name hash `TagColorFor`; the picker's swatches are `kTagPalette` /
  `TagPaletteColor`). The palette lives in the engine (`SessionStore.h` `kTagPaletteRgb` /
  `TagPaletteIndexFor` / `ResolveTagDisplayHex`), so the Telegram bot paints a tag in the app's color.
  The tab producer `_SetTabAgentTags` resolves each color once into `TerminalTabStatus::AgentTagsSpec`
  (`name\t#AARRGGBB` lines joined by `\n`), which the ribbons and the tooltip chips parse with no store
  I/O. Never re-resolve per consumer (Rule #17).
- **Removal is explicit** (Rule #17): untagging a tag's last session keeps it listed at ·0, re-appliable;
  only the ✕ on a 0-carrier row deletes it (`_RemoveGlobalTag` → `UnregisterKnownTag`). A carried tag
  shows no ✕. A deleted tag keeps its color entry, so a re-created name regains its color.
- **The cap:** `AppSettings::maxTags` (default 20, at most 40, `ClampMaxTags`) limits only the creation
  of a new name; applying or removing an existing tag is never capped.

## The Tags panel

- `_OpenTagEditorAt`: an islands-safe raw `Popup` parented into `Root()`. A `[name box | +]` row (Enter
  or **+** adds), a swatch row (one swatch per palette color; the pick is ringed white and painted onto
  the **+**; pre-picked at random on open and again after every add), the tag list (each row
  `[✓][ribbon][name][·count]`) and a "K of N tags" footer.
- A new tag takes the current pick and enters `tags.json`; an existing tag is recolored only by an
  explicit swatch tap, never by the random pick.
- Opened from the tab menu (Tags), a Sessions-page row's right-click, and a board card or tree row
  (`AgentManagerContent::SetTagsHandler` → `_OpenTagEditorForElement`, anchored under the element). A
  side-panel card's menu is anchored on the card (`BuildSessionMenuFor`'s `anchor`), so its Tags item
  opens under it.
- **The keyboard:** the name box takes the keyboard at the open and keeps it past the tab menu's close;
  Esc closes the panel on its key-up; a close while the panel holds the keyboard gives it back to the
  Control that had it before the panel opened, else to the current tab (CLAUDE.md Gotcha *A panel opened
  from a menu …*).
- **Its tooltips wait 3 s** (`kTagPanelTipDelay`, `TerminalPage.AgentObserver.cpp`): `AgentSetTip`'s
  open-delay override for one element, passed at each call site in `_EnsureTagEditorPopup` and
  `_RebuildTagEditorList`. ⚠ A new tooltip in the panel must pass it too. `_CloseTagEditorPopup` ends the
  panel's tooltips (`AgentCloseTipsIn`) before closing, so a pending wait never opens on the next open.

## Where tags show

- **The tab:** one ribbon per tag (cap 20) in the always-open parented popup `HeaderTagBookmarksPopup`,
  so each 6.5×9.3 ribbon sits ~30% on and ~70% below the tab's bottom edge (a parented popup's child
  draws in the island's popup root, outside the strip ScrollViewer's clip). `_UpdateTagBadges` rebuilds
  the row (change-gated on `_renderedTagsSpec`); `_PositionTagBadges` schedules its placement for a clean
  tick (`_badgeReposQueued`) and `_PositionTagBadgesNow` places it. The header's `LayoutUpdated` hook
  (which follows a tab reorder) is armed only while the header is tagged and loaded
  (`_UpdateBadgeLayoutHook`, reading `IsLoaded()`, never the event); a layout pass re-places only when
  `_BadgePlacementSignature` (plain `ActualOffset` / size / scroll-offset reads, no `TransformToVisual`)
  changed. A popup-hosted ribbon does not bubble presses to the tab, so its `Tapped` selects the tab.
- **The hover panel** (`TagBadgeHoverBegin/End` → `_ShowTagHoverPanelNow`): every session carrying the
  tag with its status dot (the Triage palette for a live session, a hollow gray ring for a closed one);
  a click on a live row jumps to its tab (`_ActivateClaudeSession`, cross-window).
- **The rich tab tooltip:** a chip row, each name over a 2 px colored underscore `Border` (XAML can't
  color an underline separately), at `tooltipTagsOpacity`.
- **The Sessions browser:** a Tags column (column 5, after Branch) with the same ribbons and hover panel,
  and a chip row under the search toggles (`_RebuildSessionsTagChips`, gently rounded rectangles; a facet
  ANDed with the search and scope filters).
- **A board card:** ribbons hang from the title band's bottom edge (a `-3` top margin; ≤ 8 + a dim "+N";
  `_MakeCard`), with the hover panel (`SetTagHoverHandlers`).
- **A side-panel card:** ribbons hang from the title band's top-right corner (`[ title * ][ strip Auto ]`;
  ≤ 5 slots, the last "+N" naming the rest; `_SidePanelMakeTagStrip`) and the card tooltip lists
  `tags:`. ⚠ The strip is a tooltip-free zone for the card: the ribbons cancel or close the card's tip
  and stop their pointer events, or it would cover the hover panel.
- **The board header's chip row** (`_RebuildBoardTagChips` / `_MakeBoardTagChip` / `_ToggleBoardTagFilter`
  / `_BoardTagFilterAccepts`), right after Clear:
  - Plain `Button`s, not `ToggleButton`s: a picked chip adds only a 1 px accent border (`BorderBrush`
    local + `ButtonBorderBrush{PointerOver,Pressed}` overrides in the chip's own resources), and the
    background never changes; the look is set from `isChecked` at build time.
  - The picks combine with **OR** ("anything tagged release or hotfix").
  - A leading **Untagged** chip (the same builder; a hollow ribbon; ON by default) judges untagged cards
    alone and tagged cards are judged by the picks alone: on with nothing picked shows everything, off
    shows only tagged cards, "only release" = release picked and Untagged off.
  - The universe is `CollectGlobalTags` over `_boardTags` (the sessions the board shows) plus the picked
    tags (`knownTags`), so every chip has a card behind it and a pick whose carrier left stays clickable;
    a pick is dropped only by the user or the trailing ✕.
  - The row collapses when no tag exists and Untagged is on; with Untagged off it always renders. The
    trailing ✕ restores the default (`_ClearBoardTagFilter`), shown while either half filters.
  - Chips are alphabetical (folded), so a rebuild on every registry notification never reshuffles them
    under the pointer.
  - Both halves persist per window in the lens (`ManagerState::boardTagFilter`, absent = no filter;
    `ManagerState::boardShowUntagged`, absent = true).
  - The header is a 2-track Grid (`Auto`: every fixed control, `[scope: …]` + Show all included; `Star`:
    the chips, scrolling).

## Cost

- `session-store\` holds a record for every launched session, most of them without a tag. Whole-directory
  reads (`LoadAllSessionStoreFieldIn` and the typed `LoadAll*`) go through the scan cache
  (`SessionStore.cpp` `VisitStoreDir`): one directory listing per call, a record re-read only when the
  listing's (size, mtime) differs from its cached stamp. `LoadAllTagColors` and `LoadKnownTags` are cached
  the same way (one stat per read). A read-modify-write reads the file, never the cache
  (`SetSessionStoreFieldIn`, `SetTagColorIn`, `RegisterKnownTagIn`, `UnregisterKnownTagIn`), and a reader
  never stores its parse over a write of this process that landed while it parsed (`WithRecordCached`).
- **The first read of a process opens every file**, so it never runs on the UI thread: the engine warms
  the store on a background thread at start-up (`[engine] session store warmed: N record(s), M file
  read(s), X ms`). Until `SessionStoreScanIsWarm()`, the Tags panel shows "Loading tags…" and the hover
  panel "loading…", `_WarmTagStoreThenRefresh` reads on a pool thread and refills them, and the **+** is
  hidden and Enter adds nothing (`_tagEditorUniverseReady`), because an add against an unknown universe
  could recolor an existing tag or pass the cap.
- **Registry reads are lean:** `SessionRegistry::HeadlinesOf(ids)` returns id, title, state, live,
  newest activity, started and external for the asked sessions only (the tagged ones, the hovered tag's
  carriers).
- **An edit answers in the panel first:** the panel's list and the session's tab ribbons repaint at once;
  the Sessions page, the board and the side panel follow ~40 ms after the last edit
  (`_QueueTagSurfacesRefresh`, one repaint per burst). Tags never notify the registry, so the panel's
  mutations repaint those surfaces explicitly (`_RefreshManagerBoardTags`, `_SidePanelNoteTagsChanged`).
- **A hidden surface costs a flag:** a hidden Sessions page sets `_sessionsTagsDirty` (repainted when it
  returns); a shown one with no chip picked and no recolor updates the chips and the changed row in place
  (`_UpdateSessionsRowTags`, `_MakeSessionsTagsCell`), else rebuilds its table; a hidden Manager page only
  sets its dirty flag (`AgentManagerContent::NoteTagsChanged`); a shown one rebuilds the board alone
  (`_Refresh(boardOnly)`). The board caches card tags by the tag epoch (`_boardTagsCache`), like the side
  panel.
- ⚠ Never read the whole store on the UI thread without `SessionStoreScanIsWarm()`, and never put a
  per-call file read back into `LoadAllTagColors` / `LoadKnownTags` / `LoadAll*`: every tag surface calls
  them.
- **Tests:** `TestSessionStoreScanCache` proves the cost by counting file opens and stats
  (`SessionStoreRecordFileReads`, `SessionStoreRecordFileStats`); `TestRegistryLeanReads` covers
  `HeadlinesOf`.

## Popup rules

- The tab popup opens only on a rooted header (`HeaderRootGrid().IsLoaded()` + `XamlRoot()`; re-armed
  on the header's `Loaded`): a restored tab asserts its ribbons before its header enters the tree, and
  opening an unrooted popup throws (`E_UNEXPECTED`).
- The popup is never mutated synchronously inside a layout-driven handler (`SizeChanged`,
  `LayoutUpdated`, `ViewChanged`): that re-enters the layout pass and trips XAML's layout-cycle detector
  (`E_LAYOUTCYCLE`, `0x88000FA8`). The triggers schedule one coalesced tick; a `CATCH_LOG` guards the
  popup tail.
- Painting is instant in the window that made the edit; another window catches up on its next bind,
  launch or gather (the panel, the editor and the chips always read the store fresh).
