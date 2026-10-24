# The Agent Manager page

The page that manages the fleet: a Triage Board, an Explorer Tree and a Prompt Queue, one page per
window, behind the tab strip's Home button. Code: `AgentManagerContent*.cpp` (the page: `.Board`,
`.Tree`, `.AutoTesting`, `.Launch`, `.Settings`), `TerminalPage.AgentEngine.cpp` (showing and hiding
it), `AgentLaunchBar.{h,cpp}` (the launch row), `TabManagement.cpp` (closing tabs).

## The page

- **Not a tab.** `TerminalPage::_CreateAgentManagerPage` (from `_OnFirstLayout`, before the side
  panel) builds the window's one `AgentManagerContent`. `_managerPage` owns it for the window's life;
  it is never in `_tabs`, so no tab loop, drag, close, split or rename reaches it. Its lens starts
  hidden.
- **Showing it:** the Home button (`TabRowControl.xaml` `ManagerHomeButton`, left of the `<` scroll
  arrow) calls `_ShowManagerPage(reason)`. It swaps the page's root into `_tabContent` (the host a
  tab's content uses; the layout and side panel stay), deselects the tab strip (every tab-targeting
  action then no-ops), re-opens a Sessions page left open over it, and runs
  `_ApplyAgentTabSelection(nullptr)` (the lens on screen with a catch-up rebuild, the selection pill,
  bring-into-view). Home paints as the strip's active entry while the page is up. The window title is
  "Agent Manager" ("Agent Manager Dev" in dev).
- **The one authority** is `_managerPageVisible`. `_GetFocusedTabIndex` answers `nullopt` while it is
  set, even when MUX re-selected a tab under the page (MUX selects a neighbor when an item leaves a
  selection-less strip; `_RemoveTab` clears it again). `_UpdateManagerSelectionHighlight`,
  `_RevealClickedSessionTab`, `_ManagerPaneNavPreviewKeyDown`, `Title()` and `_updateThemeColors`
  read it.
- **Leaving it:** a tab selection hides the page through the funnel (`_OnTabSelectionChanged` →
  `_HideManagerPage` → `_UpdatedSelectedTab`). Selecting the tab MUX already holds selected raises no
  event, so `_SelectTab`, `_FocusClaudeSessionTab`, a header press and `OpenSettingsUI` call
  `_LeaveManagerPageOntoSelectedTab`. `nextTab` goes to the first tab and `prevTab` to the last; the
  cycle never lands on the page.
- **Zero tabs is a legal state:** a window with no tab shows its page instead of closing.
  `_CompleteInitialization` still raises `Initialized` (else the window stays cloaked); `_SelectTab`,
  `_SelectNextTab`, `_TryMoveTab` and `_WindowSizeChanged` tolerate an empty strip; `_UpdateTabView`
  keeps the tab row visible while a page exists, so Home stays reachable.
- **The page shows by itself** when the last tab closes (the window stays open on the page; the
  Manager-only check then closes the window unless it is the last one), when a close leaves only
  dormant tabs (showing one would start its claude.exe), when the focused tab is deactivated, for a
  window that starts with no tab, and for a window reopened while it was on the Manager (with async
  shell replays, after the batch's last tab: `_showManagerPageAfterStartupBatch`).
- `_MakePane` refuses an `agentManager` pane: there is never a second Manager.
- **Toolbar:** Launch · Fork · Reopen Windows (N) (`_reopenBtn`, shown while N > 0) · ⚙ · Pause Prompt
  Queue Runner · Sessions.
- The page is always dark (CLAUDE.md *Conventions*).

## Home and the jump-back arrow

- While a session that this window hosts as a live tab is selected on the page (usually the tab the
  user came from, through the selection sync), Home shows ↶ (`E7A7`, the Undo glyph) and a click
  returns to that tab. With nothing to go back to it keeps the Home glyph and a click does nothing.
- One face at a time: `_ApplyManagerHomeButtonFace`, change-gated on the tooltip text, flips the glyph,
  the tooltip (`AgentSetTip`, naming the session by its tab title; open delay 2 × the system hover
  time, `_ManagerHomeButtonTipDelay`) and the accessible name together. `_ManagerJumpBackSessionId`
  decides; the click (`_OnManagerHomeButtonClick`) reads the same live state, never the face. A hover
  re-runs the update, so a rename never leaves a stale title in the tooltip.
- ⚠ The FontIcon must stay the button's direct Content, and the button gets no XAML tooltip
  (`TabRowControl.xaml`).
- A click within the system double-click time of a click that opened the page or jumped back is
  ignored (`[nav] home-button click ignored …`), so a double-click never opens the page and leaves it.

## Launches made on the page

- The launch row's Launch / Resume / Fork, a board, tree or External row menu's Open New Session Here /
  Fork session / Adopt / Codex launch, and the side panel's "+" card while the page shows all run
  through `_LaunchFromManagerPage`. The new tab opens in the background (`_openClaudeTabInBackground`),
  and one Low-priority tick later `_FinishManagerPageLaunch` starts its claude in place
  (`_ActivateDormantSession`) and selects the session on the page (`SelectSession`: card, tree row,
  right pane, the tab's pill, the side panel's card, Home's jump-back). Logged
  `[nav] manager-page launch <sid8> (<origin>): started in place, selected on the page`.
- A Resume of a session that is already open jumps to its tab; a fork decides when it runs (after its
  question, HOOKS.md *Interrupted turns*).
- A launch that selects its new tab (a tab menu, the "+" card over a tab, a Sessions-page resume) syncs
  the Manager selection once `_claudeTabs` maps the tab (`_NoteLaunchedSessionTab`, at the end of
  `_LaunchClaudeSession` / `_LaunchCodexSession`).

## Triage Board

- Cards in five state columns (Running · Waiting-for-you · Needs-approval · Error · Idle/Done) plus the
  observe-only **External** census column, which no scope filters.
- Each column is a fixed-width box at full board height: a pinned header over a vertically scrolling
  card list (the board's own ScrollViewer scrolls only horizontally). The columns persist per window
  (`_EnsureBoardColumn`, `BoardColumnView`): a rebuild rewrites the count and refills the cards, and
  `_SyncBoardColumnOrder` adds or removes Error / External with the fewest edits (`PlanKeyedListOps`).
- **Header:** the LOCAL/GLOBAL twin of the tree's scope (`_boardScopeBtn`; in LOCAL the five state
  columns show this window's sessions), `[scope: …]` + **Show all** (only while a folder is scoped,
  `_showAllBtn`), **Clear** (deselects the current card or row, managed or external; hidden while
  nothing is selected, `_clearSelBtn`), then the tag chip row ([TAGS.md](TAGS.md)). The header is a
  2-track Grid: every fixed control in track 0 (`Auto`), the chips in track 1 (`Star`, scrolling).
- **A managed card:** the title sits in a band painted the session's tab color (`ResolveSessionColorHex`;
  rounded top corners, square bottom; the title ink black or white by WCAG luminance,
  `PreferDarkTextOn`), over a neutral body: the codex pill, the working folder (tooltip
  `launched in <cwd>` when the inferred folder differs), `model · effort`, the timing line, the runner
  badge (`⚙ sent/total`) and the amber ⚡ "still server-cached" glyph (`ServerCacheStillWarm`, at rest
  only). The state-colored border shows only on hover (1 px) or selection (2 px). Tag ribbons hang from
  the band's bottom edge (≤ 8 + "+N"). A hover-revealed `⋯` in the top-right corner opens the session
  menu. External cards stay plain.
- **Interactions:** a click selects; a double-click Activates; a right-click (or `⋯`) opens the session
  menu. A title-only `_Refresh` keeps keyboard focus on the clicked card or row (tagged `b:<id>` /
  `t:<id>`, mapped in `_boardCardsById` / `_treeRowsById`, re-focused after the rebuild).
- **Tooltips** open at ~1/3 of the system hover delay (`AgentSetTip`); on the page their text renders in
  a fixed panel nested in the board's top-right corner (`AgentLocalTooltip`, `_managerLocalTip`; the
  settings and claude-missing overlays stay outside it).
- The board and tree show only open (`live`) sessions; a closed session lives in the Sessions browser.

## Explorer Tree

- Working directories → their sessions, grouped by the effective working directory
  (`EffectiveWorkingDir`; the filesystem-aware `PathEq`, Rule #8).
- **Scope:** LOCAL · GLOBAL · EXTERNAL (this window · all windows · the Fleet Observer's externals).
  `_SetTreeScope` is the one mutator behind both scope buttons; the board reads EXTERNAL as GLOBAL (a
  click there flips the shared scope to LOCAL). Persisted per window in the lens (`ManagerState.treeScope`;
  absent = LOCAL).
- **Sort** (`_CycleTreeSort` / `_UpdateTreeSortButton`; GLOBAL and persisted, `AppSettings::treeSort`):
  NEWEST · OLDEST (conversation ctime) · MOST ACTIVE (recency, a Running session pinned on top) ·
  MOST RECENT (turn boundaries only: `SessionInfo::turnStartUnixMs` / `turnEndUnixMs`, persisted, so
  mid-turn tool calls and side-file writes never move it; no Running pin) · A–Z · BY PID (externals by
  their host shell's pid, managed sessions by the claude pid; then most active). It orders folders and
  rows in every scope (`SortKey` / `MakeSortKey` / `SortKeyLess`); the board and side panel share
  `boardSort` (MOST ACTIVE · MOST RECENT · NEWEST · OLDEST · A–Z).
- **A new session or fork sorts at the top:** the launch seam (`_LaunchClaudeSession` /
  `_LaunchCodexSession`, not on restore) stamps its `lastActivityUnixMs` with its creation time.
- **↻** (`_treeRefreshBtn`) redraws at once and fires `_RefreshObserverData`: a full Fleet Observer
  survey, `_ObserverProbe`, then one more redraw.
- `Enter` = Activate; `Del` = Close; neither types into the terminal (Rule #2).
- **Rename in place** (`_CommitRename`): trimmed; a blank rename keeps the old title (Rule #11).
- **EXTERNAL** (`_RebuildExternalTree`) lists every claude and codex Agentmaster does not manage (real
  Windows Terminal, consoles, sibling installs), grouped by folder. Each row is enriched from its
  transcript: the title, a host label (`ResolveExternalHostLabel`: "Windows Terminal" / "Agentmaster" /
  "Agentmaster Dev" / the shell), the branch, `model · effort · pid`, the timing line. The pid carries
  an underline colored by the host shell (`ExternalClaudeRow::hostPid`, `WindowKeyColor`): claudes of
  one tab share a color.
- **Selecting an external** (a row or a board External card) switches the tree to EXTERNAL with the row
  highlighted, highlights the card, and shows its conversation read-only in the Prompt Queue
  (`_SelectExternal`, `_RebuildExternalPlan`: the human prompts, read off-thread and cached).
- **The External menu** (`_MakeExternalTreeMenu`, the same for row and card): Adopt (Fork a copy —
  `claude --resume <id> --fork-session` into a new transcript, safe while the original runs — or Resume
  anyway; `_AdoptExternalClaude` / `_AdoptExternalCodex`; the original keeps running), Open New Session
  Here, Copy Session Id, Show Transcript, and last Bring Window To Front (`_BringExternalToFront` →
  `ProcessInspect::BringClaudeWindowToFront`: the claude's nearest visible ancestor window is restored
  and foregrounded; in a Windows Terminal-class window its tab is selected through UI Automation when
  one tab scores uniquely best, `PickClaudeTab` / `ScoreClaudeTabName` / `ScoreTabNameTokens`; never
  input into the foreign session).

## The timing line

- Every card and row shows `created 4d1h, active 2m4s ago` (`FormatSessionTimingLine`,
  `AgentMaster/SessionTimingText.h`): created in `y` / `d` / `h` (`created recently` under an hour,
  `created now` under 5 minutes), activity in `y` / `d` / `h` / `m` / `s` (seconds only under an hour),
  omitted when 0; no month unit. Managed sessions read `SessionInfo::convCreated/convLastActivityUnixMs`,
  externals their transcript times. The 30 s tick rewrites the bound timing texts in place
  (`_RefreshTimingTexts`); a full rebuild runs only when a ⚡ glyph must flip.

## Selection sync (Linked Lenses)

- **Activate is cross-window:** `_ActivateClaudeSession` selects locally when this window hosts the tab,
  else fans out through the engine's activate sinks (`Engine::RegisterWindowActivateHandler` /
  `ActivateSessionInOtherWindows`); the hosting window selects the tab and foregrounds itself
  (`_FocusClaudeSessionTab`: restore-if-minimized, `SetForegroundWindow`, the `SwitchToThisWindow`
  fallback). A board or tree double-click, tree `Enter`, the Prompt Queue eye and the Sessions page's
  Jump ride this seam.
- **Switching tabs selects the session on the page:** the tab-switch funnel (`_OnTabSelectionChanged`)
  calls `_SyncManagerSelectionToTab` → `SelectSession` (the same path a card click takes) for the newly
  focused tab's session (Claude or Codex). A click, `Ctrl+Tab` and `switchToTab` all route through it;
  it is gated on `_startupState == Initialized` and is a no-op for a non-session tab.
- **Rename is cross-window:** the rename writes the registry; the hosting window re-pins its tab
  through the registry observer (`_SyncClaudeTabTitleFromRegistry`).
- Selecting any card or row drops the session's folder into the Launch box (unfocused, so no picker
  pops). The selection persists per window in the lens (`ManagerState`).

## The session menu

One menu for a board card, a tree row and a side-panel card (`_MakeLazySessionMenu`, filled in its
`Opening` by `_PopulateSessionMenu`): Jump to Tab, Rename (F2), Deactivate ▸, Close ▸, Open New Session
Here ▸ and Fork session ▸ (Default, `Specify…`, the launch models, then the presets), Copy ▸ (Session
Id, Path, Branch, Current Prompt, Launch CLI, Summary, Transcript ▸ Show + Default / Max / Full / Compact
/ Minimal; `CopySessionField`), Show Transcript (directly below Copy), Tags.

- A board-invoked Rename first makes the tree row renderable (un-collapses its folder, widens a LOCAL
  scope to GLOBAL for a session hosted elsewhere): the in-place editor lives in the tree.
- **Deactivate ▸ and Close ▸** list the tab menu's scopes row for row: Deactivate Tab / Close Tab ·
  Tabs to the Left · Tabs to the Right · Other Tabs · Of Same Folder · Other of Same Folder · All Tabs,
  plus **★ Favorite & Close All Tabs** under Close and **Deactivate All Idle Tabs** under Deactivate.
  They are grayed by the tab menu's rules: Deactivate Tab on a dormant session, the strip items by where
  the session's tab sits in this window's strip (`SetSessionTabSpotProvider`; nothing grayed for a
  session another window hosts). Deactivate ▸ shows only for a live, non-adopted session; Close ▸
  always.
- Every item but Close's two folder items goes through `TerminalPage::_RunSessionMenuScope` and makes
  the tab menu's own call (`_DeactivateTabsFromMenu`, `_HandleCloseTabRequested`, `_CloseTabsBefore`,
  `_RemoveTabs`, `_CloseAllTabs`) on the session's tab in the window that hosts it. Another window's tab
  is handed over through the session-action fan-out (`RequestSessionActionInWindows` with
  `session-menu-deactivate` / `session-menu-close` and the scope code → `_RemoteSessionAction` →
  `_RunSessionMenuScopeHere`), where a Close first brings that window to the front to ask. A Close Tab
  of a session whose agent exited and that no window hosts archives the record in place. Deactivate All
  Idle Tabs runs in the window the menu is open in.
- The content's `SessionMenuScope` values are `Tab::kDeactivateScope*` (static_asserted in the wiring),
  plus the page's `kSessionMenuScopeFavoriteAllTabs` (8). Logs `[nav] session-menu <verb> <scope>
  <sid8>` (+ `-> handed to the window that hosts its tab`); the hosting window logs
  `[nav] session-menu-remote …`.

## Closing sessions

- **One close seam** for the tab ✕, tree `Del`, the Manager's Close and the Prompt Queue's Close item:
  `_HandleCloseTabRequested` → `_ArchiveAndCloseClaudeTab`. With `confirmBeforeKill` it asks Close ·
  ★ Favorite & Close (☆ Unfavorite & Close for a starred session, `IsSessionFavorite`) · Cancel. Both
  close paths set `live=false`, clear the injector, persist and close the tab; the record and the
  transcript stay (FAVORITES.md). Logged `[nav] close-begin` ↔ `close-done`.
- **A batch** with managed sessions (a window close, the tab menu's Close ›) asks once: Close All ·
  ★ Favorite & Close All · Cancel All; a batch of only shell tabs closes without asking.
- **Of Same Folder / Other of Same Folder** (the session menu and the tab menu's Close ›, shown for a
  session with a folder): `_ConfirmAndCloseClaudeSessionsInFolder` asks once, listing the tab titles
  (≤ 20 + "…and N more"), then `_CloseClaudeSessionsInFolder` closes across windows: local tabs archived
  and closed, other windows' through `CloseSessionInOtherWindows` (→ `_CloseClaudeSessionLocal`), an
  exited session archived in place, an alive-but-unhosted one left open.
- **The close successor:** `_RemoveTab` focuses upstream's pick unless selecting it would start a
  process (any pane `NotConnected`: `_TabSelectionWouldStartProcess`); then the nearest tab that starts
  nothing (in order: the slid-in tab, the old left neighbor, outward right-first; MRU mode: the most
  recently used). When every remaining tab would start something, the page shows
  (`_PickCloseSuccessorTab`, the pure `PickCloseSuccessorIndex` in `TabCloseSuccessor.h`; logged
  `[close-successor]`).

## Deactivating sessions

**Deactivate ▸** (moon glyph `\xE708`, directly above Close ▸ in the tab menu, `Tab::_AppendDeactivateMenuItems`,
and in the session menu) parks managed tabs: claude is shut down, the tab keeps its place, title, color,
tags, queue and runner mode, and the conversation resumes when the tab is entered or activated.

- **Scopes:** each item raises `Tab::DeactivateTabsRequested` with a `Tab::kDeactivateScope*` code;
  `TerminalPage::_DeactivateTabsFromMenu` picks the tabs (an unknown code parks nothing and is logged).
  The strip scopes (this tab, left, right, other, all) act on this window; `Tab::_EnableMenuItems`
  grays Tabs to the Left on the left-most tab, Tabs to the Right on the right-most and Other Tabs on a
  window's only tab. The folder scopes act on every window, matched by `NormDirKey(EffectiveWorkingDir)`:
  this window parks its own tabs and hands each other session to its host
  (`RegisterWindowDeactivateSessionHandler` / `DeactivateSessionInOtherWindows` →
  `_DeactivateSessionLocal`, one dispatcher item per session). ⚠ No confirmation: a folder scope also
  stops sessions in windows the user is not looking at, and a turn in progress is cut off (the tooltips
  say so).
- **Deactivate All Idle Tabs** (scope `kDeactivateScopeIdleTabs` = 7): every tab of the window the menu
  is open in whose session is Idle (the gray dot) and that the park accepts. The one list is
  `_IdleTabsToDeactivate` (one lean `HeadlinesOf` read; dormant and adopted sessions dropped; strip
  order); it also grays the item (`Tab::SetAgentDeactivateIdleEnabled`, the card menu's
  `SetIdleTabsProvider`). When other windows hold idle tabs too (`_IdleSessionsElsewhere`), it asks
  "Deactivate idle tabs" — This window (N) (default) · All windows (M) · Cancel
  (`_DeactivateIdleTabsAsking`); All windows hands each session to its host (`kDeactivateIdleSessionAction`
  → `_DeactivateIdleSessionLocal`, which parks it only if it is still live and Idle).
- **The park** (`_DeactivateManagedTab`) produces a restored tab's own object: it builds the resume-gated
  connection (resume / re-fork a never-messaged fork / fresh with the same id), wraps it in a donor pane
  (`_MakePane(NewTerminalArgs{}, nullptr, conn)`) and swaps the donor's never-initialized control into
  the session's pane (`Tab::ReplacePaneContent` → `Pane::AdoptContentFrom`, tabToken-matched; splits,
  pane id, active pane and the tab object stay). The old control closes like a pane close (graceful
  `CTRL_CLOSE` to the old claude). Then `SessionRegistry::RetireHostToken(<old wt>)`, the record is
  normalized like a reopen (tabToken = the new connection, pid 0, `started=false`, presence cleared),
  the flash and notify trackers are pre-seeded, the injector re-pointed, the overlay re-attached,
  `started` tracked on the new control (`_TrackSessionStartedOn`) and a kept draft re-armed for the
  re-fill. The session keeps its runner mode.
- **Waking:** entering the tab lays the control out and its first-layout init starts the resume;
  Activate Tab, Shift+Click and Activate All Tabs (N) initialize it in place (`InitializeWithSize`).
  `TermControl::StartDormantConnection` + `_WakeDeactivatedTab` start an initialized control that sits
  on a never-started connection.
- **Why the parked record stays parked:** retired hosts (the closing claude's observations and hooks are
  dropped for 10 min, HOOKS.md *Retired hosts*) and the dormant hold (`ScanSessionIsDormant`: the scanner
  never promotes a live, ours, not-started session to Running; `[recon-dormant]` in scanner.log).
- **Refusals:** adopted / external sessions (their ConPTY is the user's own shell); sessions mid-delivery
  (the gate owner holds the box); already-dormant tabs (Deactivate Tab grayed, `SetAgentDeactivateState`).
- **The focused tab:** the page shows first, before the swap (a never-initialized control laid out on
  screen would start at once); a window whose page cannot show keeps its focused tab running (logged).
- **The strip stays where it was:** a wholly visible right-clicked tab is checked again after the work
  (next low-priority tick + 400 ms) and revealed in place if it moved out of view
  (`_KeepTabHeaderInView`); each `[strip] deactivate:` line records the offset and scrollable width.
- An app restart reopens a deactivated tab as a plain restored-dormant tab.
- Logs: `[nav] deactivate-begin` / `deactivate-done` (scope `this` / `others` / `all` / `left` / `right` /
  `folder` / `folder-others` / `idle`; a folder scope adds `elsewhere=<N> dir=<folder>`),
  `[nav] deactivate-idle-ask` / `deactivate-idle-answer`, per tab `[deactivate] <sid8> … parked dormant …`,
  and the `[retired-host]` lines.

## The Prompt Queue

- A tab beside the read-only **Summary** tab (the `[Summary | Prompt Queue]` toggle,
  `autoTestingShowsSummary`).
- **The compose row:** three icon buttons (eye = jump to the live tab · `!` = Send now, which confirms
  first · envelope = add to the queue) beside a multiline textarea that grows as you type. Per-message
  actions are a right-click menu over the messages (`_MakePromptMenu`): Copy on every row (a row
  Agentmaster queued copies paste placeholders as the pasted text), Move up / Move down / Delete on
  upcoming rows.
- **History:** a plain Up / Down walks the prompt history (`_BuildPromptHistory`,
  `_ApplyPromptHistoryText`, the in-progress text kept as `_promptHistoryDraft`); a modified arrow keeps
  its caret / selection meaning. **Ctrl+Shift+Up on an empty box** takes the last Pending prompt back
  into the box (`_PopLastQueuedPromptIntoBox` → `TakeLastPendingPrompt`, found and erased in one
  registry `Update`; logged `[nav] queue-pop`). Focus returns to the box after a queue or send
  (deferred past the Send-now confirm).
- **The unsent draft:** clicking or tabbing into the EMPTY box pulls the session's unsent input-box
  draft in (`_MaybePrefillPromptFromDraft`: the live read, else the remembered draft,
  `PickCurrentPromptText`; gated on edit intent — `GotFocus` with `Pointer|Keyboard` or a `Tapped` —
  and deferred one tick; managed Claude only; one-shot per (session, draft), `_promptPrefill*`; a copy:
  the draft stays in the tab). A non-empty box is reconciled by `EvaluateDraftPull`: an empty box is
  filled; a box whose text the draft strictly continues is extended in place only when that text came
  from Agentmaster (the provenance guard); a box ahead of the draft is never touched; a different draft
  is offered by the pull button beside the templates icon (`_pullDraftBtn`, `\xE896`; shown when the
  box is contained in the draft, or both exceed 50 chars and agree ≥ 80% by `DraftSimilarityPercent`).
  The placeholder text advertises the pull. Logged `[nav] compose pull-draft <sid8> src=… how=…`.
  ⚠ A text-only edit of the draft in the terminal raises no notify, so the button updates on the next
  Manager interaction (PENDING_INPUT.md §8a–§8b).
- **The runner toggle** at the tab's top right: gray ○ Paused / green ● Auto, flipping on click
  (`_CycleAutorunner` / `_UpdateAutorunnerButton`); an interrupt hold reads
  `Paused → Auto on your next message` (hollow, green).
- **The queue view** shows every message the session got: a chronological SENT summary (each row tagged
  **auto** in blue — queued and injected by Agentmaster — or **typed** in amber — typed into the
  terminal) over UPCOMING (Pending / Held).
- **Templates** (save / apply / apply to every session in a folder) sit behind a paper icon at the
  textarea's top right, inline rather than in a Flyout so the name box gets keys.

## The launch row

- `AgentLaunchBar` (shared with the side panel's "+" card): the agent element (the Claude⇄Codex toggle,
  or a fixed `● Claude` chip for a Claude-only host), the working-folder box with its validation
  underline and folder picker, Launch (with the PRESETS hover menu) and Fork. A host
  (`AgentLaunchBarHost`) supplies the callbacks, the picker's parent panel, the dialog anchor and the
  window's one recent-folders list (`AgentLaunchRecentDirs`, `AgentManagerContent::LaunchRecentDirs`);
  `Shutdown()` stops the hover timer and closes an open menu and picker.
- **Live validation** (`_ValidateLaunchBox`): an existing folder → neutral, "Launch Claude"; a session
  id with a transcript on disk (`LooksLikeSessionId`, `ClaudeConversationExists`) → green,
  "Resume session" + Fork (`_ResumeSessionFromDisk` / `_ForkSessionFromDisk`); a creatable absolute path
  → amber, "Create & Launch Claude" (`_EnsureLaunchDirExists`); a missing id or malformed path → red,
  disabled. With the toggle on Codex the box takes folders only ("Launch Codex", "Create & Launch Codex").
- **The folder picker** (`Primitives::Popup`, opened by a pointer or keyboard focus, never a
  programmatic one): Browse…, RECENT (up to `recentDirsLimit`, default 10, the current folder excluded),
  RECENT MATCHES (fuzzy-ranked while typing), GIT WORKTREES, MATCHES / SUBFOLDERS (leaf-prefix
  filtered, `LeafStartsWith`) and `..`; a click navigates and re-lists. It is sized against the window
  (`_LayoutPathPicker`: width ≤ 90% of the pane, height ≤ 90% of the window and the room below the
  box; re-laid on resize by the tick-deferred `_QueuePathPickerRelayout`).
- **The PRESETS hover menu** over Launch: a mouse hover opens it at once, a touch after a 450 ms hold.
  One click launches while it is up: the flyout's light dismiss eats the press, so `Closing` arms it
  (the physical button down by `GetAsyncKeyState`, or the pointer message by `GetPointerInfo`), and the
  release over Launch (`_OnLaunchButtonReleased`) or the release watch (`_OnLaunchReleaseWatchTick`, a
  15 ms timer until the release) completes it; `ClickLaunch` ignores a second click within the
  double-click time; every arm and completion logs `[launchbar]`.
- `Focus()` focuses the box. ⚠ Read a focus arrival in `GettingFocus` (`_cwdBoxFocusArrival`): a
  programmatic focus reads as Pointer/Keyboard by `GotFocus`; a programmatic text write is marked
  (`_programmaticTextPending`) so its late `TextChanged` opens no picker; an open picker re-anchors
  after the next layout pass (`_ArmPathPickerReanchor`).

## Rebuild cost

- The board and tree rebuild from registry snapshots, and only while the page is on screen
  (`SetLensVisible`, driven from the tab-switch funnel; a hidden lens marks itself dirty and catches up
  once on show). Every lens trigger goes through `_Refresh`.
- Live sessions only (`SessionRegistry::SnapshotLive()`); menus are built lazily (`_MakeLazySessionMenu`,
  `_MakeLazyExternalMenu`, filled in `Opening`); `LoadDirColors` is cached by the file's (size, mtime);
  `AgentmasterStateDir()` re-asserts its folder at most every 2 s; the package family name is memoized;
  the reopen count has a 5 s TTL.
- The registry-driven rebuild gap follows the rebuild's cost (`AgentMaster/RefreshPacing.h`: 2× the last
  rebuild, 250 ms – 5 s, the gap stamped after the rebuild returns); a rebuild of ≥ 1 s logs
  `[manager] slow rebuild …` at most once a minute.
- ⚠ Never add per-card disk, registry or syscall work (resolve once per rebuild, or cache by file
  stamp), never attach an eagerly built flyout to a rebuilt element, and never call `Snapshot()` on a
  periodic path that needs only live sessions (use the narrowest lean read: `LiveTabBindings`,
  `GetTabToken`, `GetWithoutQueue`, `SnapshotLiveForScan`, `SnapshotLiveForCards`, `HeadlinesOf`).
- A tab switch logs `[tab-switch-slow]` (a 4-lap breakdown when it takes ≥ 250 ms) and
  `[tab-switch-settle]` (when the UI thread is not idle again within 750 ms). A terminal's ticket lock
  (`til::recursive_ticket_lock`) held for seconds by its output or render thread blocks the UI thread's
  pointer handlers; `tools/livesample.exe lockwatch <pid> <uiTid> 24000 40` names the holder (⚠ it
  suspends threads: use it on dev; on production use `tools/stacksnap.cpp`, which never does).
