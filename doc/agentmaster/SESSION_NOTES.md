# Session notes — a note on a session, shown when you hover its tab or its side-panel card

> **Status: IMPLEMENTED 2026-11-16, NOT BUILT.** Designed with the user on 2026-11-16 ("I want Set Note and showing note
> on hover for tab and for sidemenu card"), over two rounds of mockups, then implemented on their "implement". The first
> build must be PURGED (a TerminalTabStatus IDL property, a new ShortcutAction, new members in TerminalPage.h and Tab.h).
> §0 lists what the user picked; §5 and the editor's keys in §3c are defaults chosen without a question. §8 says how the
> code was checked without a build; §9 lists what to check once it runs.

A **note** is plain text the user writes about a session: why the tab exists, what it waits for, what comes next
("Waiting on Dana's review of PR #412. Next: rerun e2e with RETRIES=0."). It is set from the tab's menu, the session
menu or **F3**, and it shows when you hover the session's tab and its side-panel card.

---

## 0. Decisions

The user's picks (2026-11-16), each made on a mockup:

1. **The tab's rich card shows the note right under the title**, before the folder, state, model, tags and the
   transcript summary (not above the summary, not above the title). §4a.
2. **The side-panel card gets a note glyph in its title band**, and **resting on the glyph opens the FULL session
   card** — the tab's own rich card, with the note under its title — beside the panel. The user's words on the first
   mockup, which showed a plain tip on the glyph: "can hovering the glyph show the tooltip and not that little classic
   tooltip you suggested?"; on the second they picked the full session card over a small styled note card. §4c.
3. **A tab with a note shows a small note glyph after its title**, before the ✕ (not before the status dot, not
   nothing). §4b.
4. **The menu item goes right under Rename**, in the tab's menu and in the session menu, **with an F3 hotkey named in
   its label** like Rename's F2: "And add F3 hotkey and mention F3 like we do for Rename tab". §3.
5. **Deferred: the side-panel card's own tip** (the plain tip that opens after resting 4 s on the card's body). The
   user: "Defer, i think its like the Full Session Card if Sum is off." It stays as it is. §4d.

---

## 1. What a note is

- Plain text, several lines, at most **2,000 characters** (`kMaxSessionNoteLength`). No Markdown: what you type is what
  shows.
- It belongs to a **session** — a managed Claude or Codex session id. A shell tab has no session, so it has no note:
  its menu does not offer one, and F3 passes through to the shell (§3b).
- It outlives the session's tab: Close (an archive) keeps it, a restart keeps it, and a resumed session shows it again.

---

## 2. Storage — two SessionStore keys

- **`note`** (the text) and **`noteAtUnixMs`** (when it was last saved), in the session's SessionStore record
  (`<profile>\session-store\<sid>.json`; `kSessionStoreNoteKey` / `kSessionStoreNoteAtKey` in
  `AgentMaster/SessionStore.h`). The store's header already named notes as the kind of datum it was made for.
- **One fact, one write.** `SetSessionNoteIn` writes both keys through `SetSessionStoreFieldsIn` (the draft's recipe),
  so a crash can never leave a note with the wrong stamp. A note that normalizes to "" removes both keys (the store
  stays sparse); a stamp of 0 stores the text alone. The write goes through the store's `AtomicWriteUtf8`, so a record
  it cannot read is never replaced (Rule #16).
- **`NormalizeSessionNote`** is the stored form: CRLF and a lone CR become LF (XAML's box writes a lone CR); C0, DEL
  and C1 control characters are dropped (TAB and LF stay); the WHOLE note's ends are trimmed (spaces, tabs, line
  breaks, U+00A0, U+3000), the inside kept as typed; the cut to 2,000 UTF-16 units never splits a surrogate pair, and
  blanks the cut leaves at the end are trimmed too.
- **Reads:** `GetSessionNoteIn` returns `StoredSessionNote { text, atUnixMs }` from ONE record-cache read (one stat);
  a stamp that does not parse, or a stamp left without its note, reads 0. `LoadAllSessionNotesIn` lists every noted
  session in one pass of the scan cache (⚠ on a COLD store it opens every record: UI-thread callers ask
  `SessionStoreScanIsWarm` first). Live wrappers: `GetSessionNote`, `SetSessionNote`, `LoadAllSessionNotes`.
- **`SessionNotesEpoch()`**: a process-wide counter that moves after every write that CHANGED the `note` or the
  `noteAtUnixMs` key (both setters bump it; a no-op save does not). The side panel's per-row cache and each window's tab
  markers compare it — loaded BEFORE their reads, the `SessionTagsEpoch` contract — so an edit made in another window
  shows within one tick. The facts epoch (the session database's) does not move for a note: the database does not
  mirror notes.
- **Tests:** `TestSessionNotes` (`tests/tests_transcript.cpp`, the m5 harness): the normalizer's rules, the round trip,
  both keys on the record, the epoch (moves on a changed text or a new stamp alone; never on a no-op or another key),
  `LoadAllSessionNotesIn`, the tolerant reads, the removal, an unstamped note, a bad id, an empty store dir.

---

## 3. Setting it — the menu item, F3 and the editor card

### 3a. The menu item

- **The tab's menu** (`Tab::_noteMenuItem`, built per tab, collapsed, with no build-time ToolTip): right under
  **Rename tab (F2)**: **Set Note… (F3)**, or **Edit Note… (F3)** when the session has a note. The page calls
  `Tab::SetAgentNoteState(isSession, hasNote)` from the flyout's Opening (one record-cache read per open, like the ★),
  which also gives the item its tooltip (`AgentSetTip`) and its AutomationId `am.tabmenu.note`. A click raises
  `Tab::NoteEditorRequested`; the page opens the editor under the tab (`_OpenNoteEditorForTab`).
- **The session menu** (a side-panel card, a board card, a tree row; `AgentManagerContent::_MakeLazySessionMenu`, built
  at open): right under **Rename (F2)**, the same label from the same read, AutomationId `am.sessionmenu.note`. The
  page installs the action (`AgentManagerContent::SetNoteHandler` → `_OpenNoteEditorForElement`); the anchor is held
  WEAKLY (the 23 GB leak rule of the Tags item), and the click defers one tick like Rename and Tags. The item is left
  out until the page wired the handler.
- Icon: Segoe Fluent's QuickNote (`\xE70B`), the glyph the tab marker and the card's glyph wear.

### 3b. F3

- A no-argument action, **`agentOpenSessionNote`** (ShortcutAction `AgentOpenSessionNote`, command id
  `Agentmaster.OpenSessionNote`), bound to **`f3`** in `defaults.json` beside F2's `Terminal.OpenTabRenamer` line. Its
  name in the command palette and the settings UI is "Set session note…" (`AgentOpenSessionNoteCommandKey`, en-US
  only, like Agentmaster's other strings). Windows Terminal binds nothing to F3.
- Registered like the prompt-scroll actions: `AllShortcutActions.h` (`ON_ALL_ACTIONS(AgentOpenSessionNote)`),
  `ActionAndArgs.cpp` (`AgentOpenSessionNoteKey`), `ActionMap.cpp` (the name), `defaults.json`; the dispatch event and
  the `_HandleAgentOpenSessionNote` declaration come from the X-macros. ⚠ The ShortcutAction enum is an IDL type: a purge.
- `TerminalPage::_HandleAgentOpenSessionNote`: on a tab that hosts a managed session (and not on the Agent Manager
  page), the editor opens under the focused tab and the key is handled; anywhere else `args.Handled(false)` lets the
  key reach the terminal (`cmd.exe` repeats the last command on F3).
- **The Explorer tree:** F3 on a focused row opens that row's editor under the row (marked handled first, so the page's
  own F3 binding does not also run); the row's tip names F3 beside F2.

### 3c. The editor card

```
┌ ▤ Note · Fix flaky login test ─────────────────────────────┐
│ ┌────────────────────────────────────────────────────────┐ │
│ │ Waiting on Dana's review of PR #412.                   │ │
│ │ Next: rerun e2e with RETRIES=0 and compare             │ │
│ │ the timings.▌                                          │ │
│ └────────────────────────────────────────────────────────┘ │
│ Enter: new line · Ctrl+Enter: save · Esc: cancel 93 / 2000 │
│ [Remove note]                              [Cancel] [Save] │
└────────────────────────────────────────────────────────────┘
```

- **A popup card in the main tree**, built once (`_EnsureNoteEditorPopup`), the Tags panel's recipe: a `Popup`
  parented in `Root()`; opened one dispatcher tick after the invoking menu closes (`_OpenNoteEditorAt`; the menu's
  refocus lands first), its box focused a tick after that with the caret at the end; the keyboard's way back captured
  (`AgentCaptureFocusReturn`) and given back on close when the card held it (`_ReturnKeyboardAfterPanel`). ⚠ Never a
  ContentDialog: a text box inside one gets no keypresses under XAML Islands.
- **Its parts:** the header ("Note · <first line of the title>", the glyph in amber), the box (`AcceptsReturn`,
  wrapping, 118 DIPs tall, `MaxLength` 2,000, AutomationId `am.note.box`), the hint + count line ("93 / 2000", amber
  from 1,800 on), and **Remove note** (`am.note.remove`, enabled only while a note is stored) · **Cancel**
  (`am.note.cancel`) · **Save** (`am.note.save`); the card is 360 DIPs wide, dark, kept inside the window.
- **Keys:** Enter is a new line; **Ctrl+Enter saves** (claimed in the box's tunneling PreviewKeyDown, so no line break
  is inserted); **Esc cancels**. Both act on the KEY-UP (the card's handledEventsToo hooks, armed only by a key-down they
  saw inside the open card): a close on the key-down hands the keyboard back while the key is down, and its ESC or CR
  would reach claude.
- **A press outside the card saves** (the Root() hook, registered once), as a tab rename's click-away commits; so does
  a **tab switch** (`_ApplyAgentTabSelection`). Esc and Cancel drop the edit. A close that saves writes only when the
  box's normalized text differs from the note the card opened with, so an untouched click-away never re-stamps it.
  Saving an empty or blank box removes the note; **Remove note** removes it at once and closes. An editor open for
  another session is saved before a new one opens.
- **The save** (`_SaveSessionNote`): the store write at once (stamped now) and one `[note] <sid> note saved (N chars)` /
  `note removed` / `NOT saved: …` line in hooks.log (the size, never the text); the repaint of this window's surfaces
  one dispatcher tick later — the tab marker (`_RefreshTabNote`), the side-panel cards (`_SidePanelNoteSessionNoteChanged`),
  an open glyph card (`_UpdateNoteHoverCard`) — because the close can run inside a pointer or key handler, and the
  repaint creates or removes elements. Other windows follow through the notes epoch. `[nav] note editor open/close`
  lines trace the card.

---

## 4. Showing it

### 4a. The tab's rich card — the note under the title

```
● Fix flaky login test
┃ NOTE · EDITED 2H AGO
┃ Waiting on Dana's review of PR #412.
┃ Next: rerun e2e with RETRIES=0 and compare the timings.
auth-service / fix/login-flake
Waiting · 4m ago · ⚠ unread
claude · opus · high · ⚡ bypass
bug  ci
────────────────────────────────────────
 1. Find why login.spec flakes on CI
 2. Try a retry around the token refresh
 …
```

- **`TtBuildNoteBlock`** (TerminalPage.AgentObserver.cpp) is appended by `TtBuildTooltipCard` right after the header:
  a Border with a dark amber fill (`#3A3220`), a 3 px amber left edge (`#E0B04C`) and corner radius 3, holding the
  caption ("NOTE  ·  edited 2h5m ago" — `TtSpan`, "just now" under a minute, no age without a stamp; Cascadia Mono 9.5,
  `#C9A85F`) over the text (Cascadia Mono 11.5, `#F3E6C4`, wrapping, at most `kTtNoteMaxLines` = 12 lines, the last
  ending in "…"). When the wrap estimate (`TtEstimateNoteLines`: characters per line from the card's width) passes 12
  lines, a dim "… Edit Note… (F3) shows the rest" follows.
- **Inert content only** (HANDOVER_tab-tooltip.md invariant 6a): TextBlocks, a StackPanel, a Border.
- **The budget:** the note's estimated height (its lines, capped at 13, × 16 px + 34 px of chrome) comes out of the
  summary body's budget, so the card keeps to its 80%-of-window height with a note on top.
- **The push:** `_PushSessionToolTipCard` is a forwarder now; the body is **`_PushSessionCard(sink, …)`**, whose surface
  is a `_SessionCardSink` (TerminalPage.h: `attached`, `open`, `setContent`) — a ToolTip host answers it for the tab and
  the "Sum" off card exactly as the body asked the host before, and the glyph's popup answers it in §4c. The body reads
  the note (`GetSessionNote`, one stat, contained) and adds its text and caption to the **signature** (invariant 9), so
  an edit or the caption's age re-hosts the card at the next hover.
- Every surface of the shared builder shows the note the same way: the tab, the glyph's card (§4c), and the "Sum" off
  card of SIDE_PANEL.md §2d (which never opens: SP-K2).

### 4b. The tab marker — a glyph after the title

- `TabHeaderControl.xaml`'s `HeaderTitleRow` has a third column (Auto) after the title's star column, holding
  **`HeaderAgentNoteGlyph`** (QuickNote, 11 px, the title's inherited ink at 85%, `IsHitTestVisible="False"`, named
  "Note"). A long title trims with "…" in front of it.
- **`x:Load="{x:Bind TabStatus.AgentNoteVisible, Mode=OneWay}"`**: a tab whose session has no note never creates the
  glyph (per-tab XAML is multiplied by the fleet). `AgentNoteVisible` is a new `TerminalTabStatus` property (IDL).
- **The page sets it** through `_SetTabAgentNote` / `_RefreshTabNote`: at launch (Claude and Codex) and at bind, beside
  the crown and the tags; after a save in this window; and, for an edit in another window, from the liveness sweep
  (`_RefreshAllTabNotesIfChanged`, ~2.5 s): when the notes epoch moved and the store is WARM, one `LoadAllSessionNotes`
  (a directory listing) re-asserts every hosted tab's marker. One atomic load per tick while nothing changed.
- The tab's UI Automation item status gains **`note`** (`kAgentStatusNote`, AgentAutomation.h) while the marker shows.

### 4c. The side-panel card — a glyph, and the full session card on it

- **The glyph** (`_SidePanelApplyCardNote`, TerminalPage.AgentSidePanel.cpp): a transparent hit-testable Border
  (`AgentSidePanelCardView::noteGlyph`, AutomationId `am.side.note.<sid8>`, named "Note") around a QuickNote FontIcon
  (10.5 DIP, the title's ink), in the band grid's column 1. The band grid is `[ title * ][ note Auto ][ tag strip Auto ]
  [ pushpin Auto ]` (the strip moved to column 2, the pushpin to 3) and exists while the card is noted, tagged or
  pinned (`_SidePanelReleaseBandGridIfBare` checks all three). The card keeps its height (SIDE_PANEL.md §2f); the
  Border has no corner radius, so no composition node.
- **The model:** `AgentSidePanelCardModel::hasNote`, from `SidePanelCardModelFor`'s new `hasNote` argument. The rows'
  cache `_sidePanelNoteById` is read for a row new to the list (one record-cache stat), dropped when the notes epoch
  moved (`_SidePanelSyncFactCaches`), pruned to the listed rows; the periodic tick's notes belt marks the list dirty when
  the epoch moved. The card's item status gains `note`.
- **The hover card:** resting on the glyph opens the session's **full rich card** (the tab's, the note under its title)
  **beside the panel** — right of a left-docked panel, left of a right-docked one, 24 px from its edge, centered on the
  glyph's line and kept 8 px inside the window (`_PlaceNoteHoverCard` measures the card first). The wheel over the
  glyph or the card scrolls its summary (`_ScrollNoteHoverCard` → `_ScrollSessionToolTipBody`, logged `src=note`),
  marked handled only when the summary can scroll.
- **The mechanism is the tag hover panel's, not a ToolTip** (the "Sum" off ToolTip host on these cards never opened —
  SP-K2): `_OnSidePanelNoteGlyphHoverBegin` arms a one-shot 160 ms timer (no tree work inside pointer dispatch) and
  `_ShowNoteHoverCardNow` builds the card on its tick; `_OnSidePanelNoteGlyphHoverEnd` starts a 300 ms grace close that
  an entry into the card (`_noteHoverHost`, which carries the keepalive and the wheel, wired once) cancels. One popup per
  window (`_EnsureNoteHoverPopup`, AutomationId `am.side.notecard` on its host). The card comes from `_PushSessionCard`
  through a popup sink (`_UpdateNoteHoverCard`) under its own surface key (`"\x1F" "sidepanel-note:" <id>`,
  `SidePanelNoteCardKey`); the summary body lands in the open card through the shared loader's re-host (a Popup may
  swap its child while open; the card is re-placed for its new height).
- **The card's own tip stays quiet over the glyph** — the tag ribbons' recipe: the glyph's entry cancels / closes the
  card's tip (`AgentCloseTipsIn`) and marks the event handled, its moves are swallowed, and with "Sum" off it detaches
  the card's rich ToolTip (`_SidePanelSuspendCardToolTip`).
- **It closes** after the grace, on a tab switch, when the panel collapses, when Sum toggles, when the AI view opens,
  when its card leaves the list (`_SidePanelDropCardToolTip` → `_CloseNoteHoverCardFor`), when its note is removed
  (here or in another window: `_SidePanelApplyCardNote` drops the glyph and closes its card — an element removed under
  the pointer is not sure to raise its `PointerExited`, so the grace close might never start), and when the list
  resets; `[nav] sidepanel note card open` traces it.
- The AI view's result cards share the card skeleton but never get a glyph (their models leave `hasNote` false).

### 4d. The card's own tip — deferred

The plain tip that opens after resting 4 s on a card's body ("Sum" on, the default) stays as it is: folder, branch,
tags, the gestures; no note. With "Sum" off, the card's designed tooltip is the full session card (SIDE_PANEL.md §2d),
which would show the note by itself through the shared builder, but it never opens (SP-K2). The user deferred the
choice ("Defer, i think its like the Full Session Card if Sum is off."); a "note: …" line like the tip's "tags: …" line
was the suggestion.

---

## 5. When the conversation changes (defaults)

- **The same tab, a new conversation** (`/clear`, an in-session `/resume`, `/handover-here`'s in-place successor; all
  reach the re-home in `_BindClaudeSessionToTab`): the tab keeps its note — COPIED, text and stamp, to the successor
  when the successor has no note of its own (beside the side panel pin's carry, SIDE_PANEL.md §2j); the predecessor
  keeps its copy, so resuming it later still shows it. `[rehome] <id> keeps the note of <old>` in hooks.log.
- **A new tab** (a fork, a `/handover` successor, Open New Session Here): starts without a note.
- **Close** keeps it; a restore or a reopened window shows it again.

---

## 6. Cost and safety

- **No disk read per card per update** (the per-row cache and the epoch) and **one store read per hover** (the tab's
  card, the glyph's card) and **per menu open**.
- **No element for a tab or a card without a note** (the tab's `x:Load`; the card's glyph only while noted).
- **No new composition node** on a card, and no storyboard.
- **One editor popup and one glyph popup per window**, each built once; the glyph card's host keeps its handlers, the
  card inside is replaced per build.
- **UI thread only**; the summary loader is today's (`_EnsureTabTooltipSummary`, its terminate-nets kept). Every catch
  logs (Rule #18); `_CloseNoteEditorPopup`, `_SaveSessionNote`, `_CloseNoteHoverCard`, the tab-marker functions and
  `_SidePanelNoteSessionNoteChanged` are noexcept.
- **Purge:** the first build is a purged one (CLAUDE.md *Building FAST* #5): `TerminalTabStatus.idl` (+ the enum the
  X-macro feeds `Command.idl` and `ShortcutActionDispatch.idl`), and new members in `TerminalPage.h`, `Tab.h` and
  `AgentSidePanelCardModel` / `AgentSidePanelCardView`.

---

## 7. Where the code is

- `AgentMaster/SessionStore.{h,cpp}`: the two keys, `NormalizeSessionNote`, `StoredSessionNote`,
  `GetSessionNote(In)` / `SetSessionNote(In)` / `LoadAllSessionNotes(In)`, `SessionNotesEpoch()`; `TestSessionNotes`
  in `tests/tests_transcript.cpp` (+ `m5_tests.h` / `m5_tests.cpp`).
- `TerminalPage.AgentNotes.cpp` (new; `TerminalAppLib.vcxproj`): the editor card, `_HandleAgentOpenSessionNote`, the
  save and its repaint, the tab markers.
- `TerminalPage.AgentObserver.cpp`: `TtBuildNoteBlock` + `TtEstimateNoteLines`, the note in `TtBuildTooltipCard`, the
  `_PushSessionCard` split with the note read and its signature, the re-home carry and the marker in
  `_BindClaudeSessionToTab`, the cross-window belt in `_SweepClaudeLivenessImpl`.
- `TerminalPage.AgentSidePanel.cpp`: the glyph, the band's fourth column, `hasNote` and its cache + epoch belt, the
  hover card (`_OnSidePanelNoteGlyphHover*`, `_EnsureNoteHoverPopup`, `_ShowNoteHoverCardNow`, `_UpdateNoteHoverCard`,
  `_PlaceNoteHoverCard`, `_CloseNoteHoverCard(For)`, `_ScrollNoteHoverCard`), `_SidePanelNoteSessionNoteChanged`, the
  closes in collapse / Sum / AI / drop / reset.
- `TerminalPage.h` (declarations, members, `_SessionCardSink`, the card model and view fields), `TerminalPage.cpp` (the
  tab's `NoteEditorRequested` and the Opening's `SetAgentNoteState`), `TerminalPage.AgentEngine.cpp` (`SetNoteHandler`,
  the close on a tab switch), `TerminalPage.AgentSessions.cpp` (the marker at launch).
- `Tab.{h,cpp}` (the item, its event and setter, the `note` facet), `TabHeaderControl.xaml` (the column and the glyph),
  `TerminalTabStatus.{idl,h}` (`AgentNoteVisible`), `AgentAutomation.h` (`kAgentStatusNote`).
- `AgentManagerContent.{h,cpp}` (`SetNoteHandler`, `_noteHandler`), `AgentManagerContent.Tree.cpp` (the session menu
  item, the tree row's F3 and its tip).
- `TerminalSettingsModel`: `AllShortcutActions.h`, `ActionAndArgs.cpp`, `ActionMap.cpp`,
  `Resources/en-US/Resources.resw`, `defaults.json`.

---

## 8. How it was checked (no build)

The project's no-build checks (the zs recipe: `cl /Zs` with a clean worktree build's recorded command lines and its
precompiled header, `Q:\tmp\am-release-wt` at ad89700e3; nothing written to a build tree):

- Passed: `SessionStore.cpp` (and under the CLI's `/W4 /WX` line), `AgentManagerContent.cpp`,
  `AgentManagerContent.Tree.cpp`, `TerminalPage.AgentEngine.cpp`, `TerminalPage.AgentSessions.cpp`,
  `TerminalPage.AgentObserver.cpp`, `TerminalPage.AgentSidePanel.cpp`.
- `Tab.cpp` and `TerminalPage.AgentNotes.cpp` fail ONLY on `AgentNoteVisible` (C2039: the worktree's projection
  predates the new IDL property); their copies with that one call swapped for an existing property of the same type
  pass. `TerminalPage.cpp` (a path-split unit) was checked through the shadow recipe and fails only on
  `TabRowMenuButton`, a XAML accessor an earlier commit added after the worktree (not this change).
- Negative controls: a `static_assert` planted inside the new code of `TerminalPage.AgentNotes.cpp`,
  `TerminalPage.AgentSidePanel.cpp` and `TerminalPage.AgentObserver.cpp` fails each with exactly C2338 at its line.
- The settings model's `ActionAndArgs.cpp` / `ActionMap.cpp` lines name `ShortcutAction::AgentOpenSessionNote`, which
  no old projection has: checked by review (each line mirrors the prompt-scroll actions' entries).
- A sweep of all 116 TerminalAppLib units the worktree's build recorded: 107 pass. `Tab.cpp` (C2039 `AgentNoteVisible`)
  and `ShortcutActionDispatch.cpp` (C2838 / C2440 / C2046, all at its X-macro's `case
  ShortcutAction::AgentOpenSessionNote:`) fail only on the two names the new IDL property and the new enum value add.
  The other 7 are the path-split units (`TerminalPage.cpp`, `TabManagement.cpp`, `CommandPalette.cpp`,
  `CommandPaletteItems.cpp`, `PaletteItemTemplateSelector.cpp`, `SnippetsPaneContent.cpp`, `SuggestionsControl.cpp`): a
  header this change does not touch (`cppwinrt_utils.h`, `til/string.h`), reached through two paths, redefines itself
  before any of the change's code; of them only `TerminalPage.cpp` changed, and it was checked through the shadow
  recipe (above).
- The m5 harness, compiled and run from a Q: scratch folder with a scratch profile: 13,626 checks, the 3 known
  TestTaskbarBadge failures (the baseline), `TestSessionNotes` clean.
- Not checked without a build: the XAML (`x:Load` bound with `x:Bind` on the tab's glyph), the IDL, and the behavior
  on screen (§9).

---

## 9. Checks after the build

- **Set:** the tab menu shows "Set Note… (F3)" right under "Rename tab (F2)" on a Claude tab and not on a shell tab;
  the board card, the tree row and the side-panel card show it under "Rename (F2)"; the editor opens under the tab or
  card with its box focused and typing reaching it; Ctrl+Enter saves (no line break added), Esc cancels, a click outside
  saves, a tab switch saves, an empty save removes, Remove note removes; the label reads "Edit Note… (F3)" afterwards;
  the keyboard goes back to the terminal after a tab menu's editor closes, and no ESC / CR reaches claude.
- **F3:** on a Claude tab it opens the editor; on a cmd tab it repeats the last command (it passed through); on a
  focused tree row it opens that row's editor; the command palette lists "Set session note…".
- **Tab:** the marker shows after the title on noted tabs only and survives a long title (the title trims before it);
  hovering the tab shows the note under the title; an edit shows at the next hover; the tab's item status reads `note`.
- **Side panel:** the glyph shows on noted cards only, the card's height is unchanged and scrolling stays smooth;
  resting on the glyph opens the full card beside the panel (on the right of a left dock, on the left after a dock
  flip), the card's own 4 s tip never opens over it, the pointer can cross into the card, the wheel scrolls its
  summary, and it closes on leaving, on collapse, Sum, the AI view, a tab switch, and when the note is removed while
  the card is up (F3 on the same session's tab → Remove note: the glyph and its card go together).
- **Across windows and restarts:** an edit in window A shows in window B within ~2.5 s (its tab marker and its cards);
  a restart and a reopened window keep every note; a `/clear` keeps the tab's note; a fork starts without one.
- No `[exc]` line from the note paths in hooks.log; the `[note]` and `[nav] note editor` lines read right.

Flows to add to the catalog (`doc/agentmaster/flows/`): a TAB flow (set, edit, remove from the tab menu and F3; the
marker; the hover) and an SP flow (the glyph; its card beside the panel in both docks; the card's own tip yielding).

---

## 10. Not in this design

The note on the Sessions page (a column, a search scope), on the board card's and the tree row's tips, in the AI search's
tab tools, in the session database's `am_*` tables and on the side panel's AI result cards; notes on shell tabs (they would
live in the window record, the shell tab's only store).

## 11. In Telegram

The note is on the topic's sticky card too, and edited from the phone: [MESSAGING.md §21.59](MESSAGING.md) (the user's
request of 2026-11-18, "We need note in telegram bottom pinned message and through cog icon we can edit the note and such").
The card shows it under its title in an expandable quote; ⚙️ Session → 🗒️ Note ▸ opens a Note page (✏️ Edit note · ➕ Add a
line · 🗑 Remove note); a note prompt takes the topic's next message (or a reply) for five minutes, then expires with a
message; `/note` shows, writes, adds a line, removes; a change made here at the PC is told in the topic. Both sides write the
same SessionStore keys, so the PC's belts (the tab markers, the side panel's cards, an open hover card) follow a note written
from the phone as they follow another window's (`SessionNotesEpoch()`), and the bot follows the PC's the same way.
