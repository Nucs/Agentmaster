# The tab strip

How Agentmaster draws and drives Windows Terminal's tab strip: the indicators on each tab header, the
tab menu, tab size, revealing a tab, dragging tabs, the `+` button (Agentmaster's launcher), the `▾` menu button, the
scroll arrows and the auto-hiding tab row. Code: `TabHeaderControl.{xaml,h,cpp}`, `TerminalTabStatus.{h,idl}`,
`Tab.{h,cpp}`, `TabManagement.cpp`, `TerminalPage.AgentEngine.cpp` (the drag gesture, the scroll arrows),
`AgentMaster/TabDragMath.h` (the pure placement math), `TabRowControl.xaml` + `TerminalPage.AgentSidePanel.cpp` +
`AgentLaunchBar.{h,cpp}` + `AgentMaster/LauncherRules.h` (the launcher), `TerminalPage.AgentTabRow.cpp` +
`AgentMaster/TabRowAutoHide.h` + `src/cascadia/WindowsTerminal/NonClientIslandWindow.{h,cpp}` (the auto-hiding row and
the `▾` menu).

## The + button: the launcher

Built 2026-11-14, NOT BUILT yet (the next build must PURGE: `SessionModels.h`, `TerminalPage.h` and
`AgentManagerContent.h` changed). The user's request: "Lets discuss the + and downarrow. We have a + in side menu but
thats also supposed to support powershell. Then we also have model picking menu. I need this integrated into something
for agentmaster and not old windows terminal functionality". Their four picks the same day (design-ask with mockups of
their own models, presets, profiles and folders):

| Question | Their pick |
|---|---|
| What + and ▾ do | **One + panel, no ▾** — "The dropmenu of FOLDER is same like the + of sidemenu" |
| How a launcher picks Claude / Codex / a shell | **The kind chip opens a menu** — "on all menus of this kind" |
| The folder a + opens on | **The focused tab's folder** (the side panel's + too) |
| Ctrl+Shift+T | **Follows the new +** |

**What the strip shows.** The split button keeps its control (Windows Terminal's code that themes it, attaches its
profile flyout and reads it runs unchanged) but its ▾ part is gone: `SplitButtonSecondaryButtonSize` 0 and a transparent
`SplitButtonBorderBrushDivider` in `TabRowControl.xaml`. `#am.strip.new`; an Agentmaster tooltip
(`_LauncherDecorateNewTabButton`; the XAML tooltip was removed — the shared tip replaces and then clears one). A
separate `▾` MENU button now stands right of `+` (the user's pick of 2026-11-15 for the auto-hide toggle: *Auto-hiding
the tab row*, below); it opens a menu, never Windows Terminal's profile list.

**What opens it** (`TerminalPage::_LauncherOpenFromTabStrip`):

- a click on `+` (a second click closes the card it opened: its press counts as inside the card);
- the `openNewTabDropdown` action — **Ctrl+Shift+Space**, and **Ctrl+Shift+T** (Agentmaster's `defaults.json` binds it
  to `Terminal.OpenNewTabDropdown`; `newTab` keeps its palette entry, Ctrl+Shift+1…9 keep opening profile 1…9);
- the split button's own drop-down keys, Alt+Down and F4 (a `PreviewKeyDown` turns them into the launcher);
- a **folder dropped** on `+` (the card opens on that folder; a drop of several items opens it on the first).

**What stays Windows Terminal's:** Alt+click `+` splits the focused pane, Shift+click opens a new window, Ctrl+click an
elevated one — all with the default profile (`_LauncherNewTabModifiersHeld` → `_OpenNewTerminalViaDropdown`), and the
same modifiers on a drop. Windows Terminal's profile menu is still built (`_CreateNewTabFlyout`), no longer opened.

**The card** is the side panel's `+` card (SIDE_PANEL.md §2e), anchored under `+`'s bottom center (the window's top
center when the strip is hidden), placed by `PlacePointerCard` toward the window's center:

```
 [● Claude ⌄] [ K:\source\Agentmaster        ] [Launch Claude]     ← the kind chip, the folder box, Launch (Fork for an id)
 MODEL   (Default) (Fable) (Opus) (Sonnet) (Astra) (Sol) (Sol 6.1) (Specify…)
 PRESET  (MiMo Flash Abliterated · NanoGPT) (GLM-5.3 Flash Uncensored · NanoGPT) (Apex …)
 ─────────────────────────────────────────────────────────────
 ⟲ Sessions…    ⚙ Agentmaster settings    ▭ Windows Terminal ⌄
   └ the folder list (Browse…, RECENT, GIT WORKTREES, SUBFOLDERS) opens with the card, UNDER the card
```

- **The kind chip** (`#am.launcher.kind`, named "What to launch: <kind>"): a click opens a menu — **Claude**, **Codex**,
  then **every profile of Windows Terminal's new-tab menu** in its order (`_LauncherShellChoices`, which reads
  `GlobalSettings().NewTabMenu()` the way `_CreateNewTabFlyoutItems` does: folders such as the user's "SSH" stay
  submenus, an empty folder is dropped, an auto-inline folder of one entry is inlined, actions are left out; the default
  profile is bold; each entry wears its profile's icon through `_CreateNewTabFlyoutIcon`). The same chip is on the Agent
  Manager's launch row (AGENT_MANAGER.md *The launch row*). A pick hands the keyboard back to the box.
- **A shell** is directory-only, like Codex: Launch reads **Open \<profile\>** / **Create & Open \<profile\>**
  (`LaunchButtonLabel`); a session id turns the box red; an empty box opens the profile in its own starting directory.
  `_LauncherSpawnShell`: `NewTerminalArgs` with the profile's GUID and the folder → `_MakePane` → `_CreateNewTabFromPane`
  (the tab Windows Terminal's menu opened; over the Agent Manager page it shows). Logs `[nav] launcher shell begin / end`.
- **The pills** (MODEL and PRESET, only for a NEW Claude session: `LauncherShowsModelRows` hides them for Codex, a shell
  and a session id): Default · each launch model (AppSettings `launchModels`) · Specify… (the page's model prompt; the
  typed id becomes a lit pill of its own) · each visible preset, or one **Add presets…** while there is none. One is lit
  (ItemStatus "selected"); every open starts on **Default** (a preset never sticks unseen). A pill never takes focus, so
  Enter still launches. Launch has **no hover menu** here (`AgentLaunchBarHost::hoverModelMenu` false); the Agent
  Manager's row keeps its own.
- **The folder** (`_LauncherFolderForFocusedTab`, `PickLauncherFolder`): a managed session's effective folder; else a
  shell's OSC 9;9 folder; else the folder the Fleet Observer read for that shell (the window restore's source); else the
  Agent Manager's box text (the page is up). The box still mirrors into the Agent Manager's box.
- **The remembered kind:** a user's pick on the chip is written to `settings.json` `launcherKind` ("claude" / "codex" /
  "profile:{guid}"; a freshest-disk RMW, `_LauncherRememberKind`) and every open of the card starts on it
  (`ApplyLaunchKindPick`: a profile gone since, or a kind the host cannot start, falls back to Claude). The Agent
  Manager's row keeps its own kind per window.
- **Keys:** Enter in the box launches what is lit (it only committed the path before), on the KEY-UP; Esc closes the
  folder list, then the card.
- **The footer:** Sessions… (the Sessions browser) · Agentmaster settings (shows the Agent Manager page and opens its ⚙,
  `ShowSettingsOverlay`) · Windows Terminal ⌄ (Settings, Command palette, About — what the old ▾ menu held). None takes
  the keyboard.
- **Logs:** `[nav] tab strip launcher open (+ click | keys | keys on + | folder dropped) kind=<token>`, `[launchbar] kind
  picked: <token>`, `[launcher] target <model>`.
- **Tests:** `TestLauncherRules` (the m5 harness: the token, the lookups, the fallback, the rows, the folder, the
  labels), `tests_persistence` (`launcherKind`); e2e `test_launcher.py` (TAB-12) and `test_side_panel.py` SP-05.

## The scroll arrows

- **The user's report (2026-11-14): "The < and > needs to be vertically centered with the + and V down arrow."** MUX
  2.8's `TabScrollViewerStyle` wraps each scroll `RepeatButton` in a Border (`ScrollDecreaseButtonContainer` /
  `ScrollIncreaseButtonContainer`) with `VerticalAlignment="Bottom"` and a vertical padding of its own, while `+` is
  centred in the row (`Height 24` in a stretched slot). On a row taller than a button plus that padding — a taller tab
  (the cog's tab min height, a larger title font) — the arrows sat below `+`.
- **Now** `_EnsureTabStripScrollArrowsCentered` (from `_EnsureTabStripScrollViewer`, which `_UpdateManagerNavButtons`
  runs on every tab add / remove / switch) finds both Borders in the strip's ScrollViewer template once it is applied,
  sets `VerticalAlignment Center` and drops their top / bottom padding (left / right kept, so the strip's horizontal
  layout and `TabDragMath` are unchanged), then latches; `[strip] scroll arrows centred on the tab row (level with +)`
  once per window. The walk visits the template only (it skips the `ScrollContentPresenter`, which holds every tab).
  NOT BUILT yet.

## Auto-hiding the tab row

Built 2026-11-15, NOT BUILT yet (the next build must PURGE: `SessionModels.h`, `TerminalPage.h`,
`AgentManagerContent.h`, `TerminalPage.idl`, `TerminalWindow.idl` and `TabRowControl.xaml` changed). The user's request:
"I want to add a autohide of the top row with tabs row and home button and + and V and minimize, maximize, close
buttons. This row needs to be autohiding and functioning similar to windows taskbar autohide. Add a new configuration in
settings too, default false. synced to the V menu item. It should bo toggleable via the V icon". Their picks the same day
(design-ask with live demos):

| Question | Their pick |
|---|---|
| Where the toggle lives (777ce6b25 had removed the ▾ on their "One + panel, no ▾") | **▾ beside + again** — a menu button |
| How the row hides and comes back | **Like the taskbar** — "but no 2px line stays, 0" |
| What else brings it out | **Peek on keyboard tab switch** and **Stay out on the Manager page** |

**What hides.** The whole title bar — the tab row (Home, the tabs, `<` `>`, `+`, `▾`), the drag bar and the minimize /
maximize / close buttons: Windows Terminal's `TitlebarControl`, owned by the window host (`NonClientIslandWindow`,
WindowsTerminal.exe). With the setting on, the bar leaves its own grid row and lies OVER the top of the content (the
content row, `Canvas.ZIndex` 1): the content gains the row's height once, and the terminal never resizes for the row again
(a resize would make every Claude Code session redraw). Away, the bar is parked exactly its own height above the island
(its `Translation`), where the island clips it, and takes no input (`IsHitTestVisible` false) — never `Visibility`, which
would re-measure the whole tab strip at every reveal. Nothing of it stays on screen (the user's "no 2px line").

**The rule** (pure: `AgentMaster/TabRowAutoHide.h`; pinned by `TestTabRowAutoHide`, `tests_tabrow.cpp`):

- **Comes out** when the pointer RESTS 0.15 s (`kTabRowRevealDelayMs`) in the window's top 4 DIPs (`kTabRowEdgeDip`,
  measured from the client area's top: a restored window's one-pixel frame border row, a maximized window's screen edge).
  A pointer that only crosses the edge, leaving the window upward, never brings it.
- **A drag from the terminal** (a selection pulled to the top row) never brings it, and it stays away until the pointer
  has left the edge band (the side panel strip's rule). A drag that started elsewhere (a folder dragged from Explorer onto
  `+`) brings it like a plain approach.
- **At once** for the HOLD (the window shows its Agent Manager page), a PEEK, and the keyboard moving into the row (a tab
  rename started by its key, Tab navigation). While HELD the bar is DOCKED: it takes its own row again, above the page
  (`_AutoHidePlace`), so the Manager page's own toolbar never lies under it; leaving the page for a tab floats it again
  (the content's height changes then, but no terminal is on screen while the page is).
- **Stays** while: the pointer is in the row or 8 DIPs below it (`kTabRowStayBelowDip`); the pointer is over any popup of
  the window (a menu hanging below it, the `+` card, a tooltip); the keyboard is in the row by the keyboard or in its text
  box, or inside an open popup (the `▾` menu, a tab's menu, the `+` card's box); a button is held from a press in the row
  (a tab drag); the hold; a peek; a finger.
- **Goes** 0.6 s (`kTabRowHideDelayMs`) after the last reason ended; a reason coming back meanwhile cancels the wait. A
  peek that was the last reason ends with itself (out for 1.5 s, not 1.5 + 0.6).
- **A finger** cannot rest: a tap in the top 16 DIPs (`kTabRowTouchEdgeDip`) brings the row out until a tap elsewhere
  (or a mouse move, which hands it back to the cursor's rule).
- A window that cannot show its title bar (minimized, focus mode, full screen without tabs) forgets everything: the row is
  hidden at once and nothing stays pending. Full screen WITH tabs (`showTabsFullscreen`) auto-hides against the screen's
  top edge.

**The motion:** a 0.15 s composition slide of the bar's `Translation` (`UIElement::StartAnimation`, eased out coming, in
going), placed at once while Windows' "Animation effects" are off (`SPI_GETCLIENTAREAANIMATION`). It is one-shot: XAML
follows the Translation for hit testing during those 0.15 s only — not the ambient loops `AgentAmbientAnimation.h` keeps
off XAML elements. A slide reversed midway leaves from where it got to; at its end (a one-shot `WM_TIMER`) the static
Translation is pinned, which ends the animation for good.

**The window host's half** (`NonClientIslandWindow`: `SetTitlebarAutoHide(enabled, hold)`, `PeekTitlebar(ms, instant)`):

- **When it looks:** every 100 ms (`kTabRowWatchMs`, a `WM_TIMER`) while the row is out and not held — the pointer can
  leave through the drag bar window (an HWND) without any XAML event — and exactly at the end of a rest, a hide delay or a
  peek. A hidden row with nothing pending runs no timer; events start its looks: a pointer move near the top
  (handledEventsToo on the island's root grid; every other move costs one coordinate read), a move over the frame border
  row (`WM_NCMOUSEMOVE` / `WM_MOUSEMOVE` on the window itself), a press, a size / maximize / DPI / mode change, the
  title bar getting focus, the page's calls.
- **What it reads:** `GetCursorPos` + `WindowFromPoint` (another window covering ours never counts; a windowed popup
  ours owns does), the client area's top and width, the island's rectangle, `GetAsyncKeyState` for the buttons, where the
  held button went down (a XAML press: the row or the content), the focus (`FocusManager::GetFocusedElement`) walked up to
  the bar or to an open popup's child, and each open popup's rectangle (`GetOpenPopupsForXamlRoot`).
- **What follows the row:** the drag bar window (the caption buttons, the window drag, the top resize band, Snap Layouts)
  is hidden while the row is away and comes back at the slide's end; `_OnNcHitTest` stops calling the band under an away
  row a caption; `GetTotalNonClientExclusiveSize` stops counting the bar's height (window sizing, snap to grid).
- **An opaque bar:** floating over the terminal, the bar wears `TabRowOpaqueBrush` of the app's brush — a translucent solid
  gets full alpha, a fully transparent one (Mica) or none gets the theme's tab-row color (`#2E2E2E` dark, `#F3F3F3`
  light); acrylic and other brushes stay. Turning the setting off gives back the app's brush unchanged.
- A hidden row re-parks at its new height when its height changes (a DPI change, the cog's tab height).
- **Logs** (hooks.log, through the EXE-safe `Startup::Note`): `[tabrow] auto-hide on` / `off`, `held out` / `hold
  released`, `reveal (<why>)`, `hide`, and every caught failure as `[tabrow] <context> threw (0x…)`.

**The page's half** (`TerminalPage.AgentTabRow.cpp`): `_ApplyTabRowAutoHide` decides on (the setting AND tabs in the
title bar) and held (on AND the Agent Manager page on screen) and raises `TabRowAutoHideChanged` → `TerminalWindow` →
AppHost → `SetTitlebarAutoHide`; `_PeekTabRow` raises `TabRowPeekRequested` → `PeekTitlebar`. Applied at engine init,
by the setting's writer, by the cross-window broadcast, and by `_ShowManagerPage` / `_HideManagerPage`. Peeks: `nextTab`,
`prevTab` and `switchToTab` (Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+Alt+1…9: `kTabRowPeekMs`, 1.5 s, sliding); the launcher opened
from the strip (`_LauncherOpenFromTabStrip`: `kTabRowLauncherPeekMs`, placed at once, so `+` is where the card's anchor
measures it — Ctrl+Shift+T on a hidden row; the card's own keyboard then holds the row while it is open).

**The setting** `autoHideTabRow` (default off, GLOBAL; SETTINGS.md *Tabs & Overlay*): two INSTANT writers of one field —
the `▾` menu's item and the cog's switch (Tabs & Overlay › TAB STRIP, `#am.settings.autoHideTabRow`) — both through
`_SetTabRowAutoHide`: a freshest-disk RMW of the field, applied here, broadcast to every other window, and the cog's switch
re-synced (`SyncTabRowAutoHideSwitch`, behind a latch so a programmatic set never re-fires it; an open cog in another
window follows through `ApplyExternalSettings`). The switch re-seeds from disk at every cog open, and both cog-Save
preserve blocks keep the field. Turning it on starts the row OUT: it leaves 0.6 s after the pointer is elsewhere, so the
user sees where it went. With the system title bar (Windows Terminal's `showTabsInTitlebar` off) there is no row the window
can slide: the page tells the host "off" and the menu item is disabled with that reason.

**The ▾ menu button** (`TabRowMenuButton` in `TabRowControl.xaml`, `#am.strip.menu`): a plain Button right of `+`, in the
old ▾'s look (the TabView button colors, the SplitButton template's fallback chevron E96E at 8). Its MenuFlyout is filled
at every open (`_TabRowMenuFill`), so its check is never stale:

```
 ⌃ Auto-hide the tab row        ✓     #am.strip.menu.autoHide
 ─────────────────────────────
 ⟲ Sessions…                            #am.strip.menu.sessions
 ⚙ Agentmaster settings                 #am.strip.menu.settings
 ▭ Windows Terminal               ▸     #am.strip.menu.terminal  (Settings · Command palette · About)
```

Each command runs one dispatcher tick after its click (`_TabRowRunMenuCommand`), the toggle first re-checking the state
its item showed (a menu left open while another window flipped the setting never inverts it); Windows Terminal's Settings
runs at the click, because it reads the click's Alt / Shift.

**Open:** with the system title bar nothing auto-hides (a page-side overlay of the tab row alone is not built); Windows' own
auto-hide taskbar at the top edge keeps its 2-pixel strip above a maximized window (`_OnNcCalcSize`), so the row's band
starts below it; whether the slide reads as smooth on the user's machine, and the timings, are owed to a build (live checks
below).

**Tests:** `TestTabRowAutoHide` (the m5 harness: the bands at every scale and for broken inputs, every named case, all
40,960 input combinations over the reachable states against a second statement of the rule, one visit event by event);
`tests_persistence` (`autoHideTabRow`: round trip, default, absent key, wrong type). E2E: flows TAB-13 (spec only).
**Owed after a build (PURGED):** the rest at the top of a restored and of a maximized window; the slide both ways and
with animation effects off; the row staying under the `▾` menu, a tab's menu, the `+` card, a tab drag and a rename; a
selection pulled to the top row; Ctrl+Tab's peek; the Agent Manager page's hold; the caption buttons, Snap Layouts, the
window drag and the top resize band on an out row, and none of them on an away one; the cog's switch and the menu item in
step across two windows; the terminal's size unchanged by every reveal.

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
- **The note glyph** (2026-11-16, [SESSION_NOTES.md](SESSION_NOTES.md) §4b): a small QuickNote glyph after the title of a
  tab whose session has a note (`HeaderAgentNoteGlyph`, the title row's third, Auto column, so the title trims before
  it), x:Load-bound to `TerminalTabStatus.AgentNoteVisible` — an un-noted tab creates no element. Not hit-testable:
  the tab's rich card shows the note right under its title. Set by `_SetTabAgentNote` / `_RefreshTabNote` (launch, bind,
  a save here) and `_RefreshAllTabNotesIfChanged` (the notes epoch, from the liveness sweep).
- **The rich tooltip** (a session-aware hover card) is `AgentSessionToolTipHost`
  ([HANDOVER_tab-tooltip.md](HANDOVER_tab-tooltip.md): its lifecycle, invariants and crash history);
  its tag row's opacity is `tooltipTagsOpacity` (0.9).
- The side panel's cards wear the same icon through `AgentStatusGlyph`, the code twin of
  `HeaderAgentStatusDotWrap` (one blink clock; `AgentMakeCacheWarmGlow`; motion rules in
  `AgentMaster/StatusIconMotion.h`). ⚠ A change to the wrap's elements or to
  `TabHeaderControl::_Update*Animation`'s gates must be made in the twin too.

## The tab menu

Agentmaster's items in a tab's context menu (shown for a managed session unless noted; visibility set
when the flyout opens): Activate Tab and Activate All Tabs (N) for dormant tabs, Set Note… (F3) / Edit Note… (F3) right
under Rename tab (F2) ([SESSION_NOTES.md](SESSION_NOTES.md) §3a: the note editor card under the tab; F3 is the
`agentOpenSessionNote` action, which passes the key through on a shell tab), Mark Unread, Open In
Explorer (`OpenSessionFolder`: the effective folder), Copy ▸ (`CopySessionField`), Show Transcript, Tags,
Filter ▸ right under Tags (the side panel card menu's Filter lines for the tab's session, filled at every open by
`TerminalPage::_SidePanelFillTabFilterMenu`; a pick narrows this window's side panel and brings its cards on screen —
[SIDE_PANEL.md](SIDE_PANEL.md) §2l *The Folder line's own click, and the tab menu's Filter*),
Favorite / Unfavorite, New Session Here ▸ and Fork session ▸ (Default, `Specify…`, the launch models, the
presets; a shell tab's Fork session is Windows Terminal's duplicate tab), Restart session, Deactivate ▸ (a mouse
or pen click on the line itself parks this tab, the same as its Deactivate Tab, unless the tab is already parked;
resting on it opens the submenu — `AgentWireSubmenuHeaderClick`, AGENT_MANAGER.md *Deactivating sessions*)
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
