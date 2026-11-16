# Session notes — a note on a session, shown when you hover its tab or its side-panel card

> **Status: DESIGN, not implemented.** Decided with the user on 2026-11-16 ("I want Set Note and showing note on hover
> for tab and for sidemenu card"), over two rounds of mockups. Nothing of it is in the tree yet. §0 lists what the user
> picked; §5 and the editor's keys in §3c are defaults chosen without a question and can still change.

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

- Plain text, several lines, at most **2,000 characters**. No Markdown: what you type is what shows.
- It belongs to a **session** — a managed Claude or Codex session id. A shell tab has no session, so it has no note:
  its menu does not offer one, and F3 passes through to the shell (§3b).
- It outlives the session's tab: Close (an archive) keeps it, a restart keeps it, and a resumed session shows it again.

---

## 2. Storage — two SessionStore keys

- **`note`** (the text) and **`noteAtUnixMs`** (when it was last saved), in the session's SessionStore record
  (`<profile>\session-store\<sid>.json`, `AgentMaster/SessionStore.{h,cpp}`). The store's header already names notes as
  the kind of datum it was made for ("any future per-session datum (pinned, notes, tags, …) rides the same store by
  adding a KEY").
- **One fact, one write.** Both keys are written together through `SetSessionStoreFieldsIn` (the draft's recipe), so
  a crash can never leave a note with the wrong stamp. An empty note removes both keys (the store stays sparse). The
  write goes through the store's `AtomicWriteUtf8`, so a record it cannot read is never replaced (Rule #16).
- **Why not `sessions.json`:** the same reasons as tags and favorites. The store is keyed by id alone, survives a fleet
  record that is dropped, is one small read by id, rewrites no fleet document, and needs no `SessionModels.h` change.
- **Typed helpers** beside the tag ones: `GetSessionNoteIn` / `SetSessionNoteIn` (+ the bare wrappers over
  `AgentmasterStateDir`). The setter normalizes (CRLF → LF, trailing whitespace and newlines trimmed, cut to 2,000
  characters) and stamps the save time; a blank text removes the note.
- **`SessionNotesEpoch()`**: a process-wide counter that every write that CHANGED a note moves (the
  `SessionTagsEpoch` recipe, SIDE_PANEL.md §2b). A reader that caches notes — the side panel's rows, the tab markers —
  loads the epoch BEFORE it reads and compares it on its periodic tick, so an edit made in another window shows
  within one tick (~2.5 s). An edit in the same window refreshes at once (§3c).
- **Tests** (`tests/`, the m5 harness): the two keys written together, the removal, the normalization, the cap, the
  epoch moving only on a change.

---

## 3. Setting it — the menu item, F3 and the editor card

### 3a. The menu item

- **The tab's menu:** right under **Rename tab (F2)**: **Set Note… (F3)**, or **Edit Note… (F3)** when the session has
  a note. The label is set at every open, like Favorite / Unfavorite. Shown only on a tab that hosts a managed session
  (like Copy ▸ and Tags).
- **The session menu** (a side-panel card, a board card, a tree row; `AgentManagerContent`'s `_MakeLazySessionMenu`,
  built at open): right under **Rename (F2)**, with the same label. The two menus keep one order (the session menu
  follows the tab menu).
- Icon: Segoe Fluent's QuickNote (`\xE70B`), the same glyph as the tab marker and the card's glyph.
- It opens the editor card (§3c) under the tab, or under the card or row you right-clicked. The session menu's anchor
  stays a WEAK reference (the 23 GB leak rule in `_MakeLazySessionMenu`'s Tags item).

### 3b. F3

- A new action, **`openSessionNote`** (id `Agentmaster.OpenSessionNote`), bound to **`f3`** in `defaults.json` beside
  F2's `Terminal.OpenTabRenamer` line. Windows Terminal binds nothing to F3.
- On a tab that hosts a managed session: it opens that session's editor under the tab, and the key is handled.
- Anywhere else (a shell tab; the Agent Manager page without a focused tree row): the handler answers
  `args.Handled(false)`, and the key reaches the terminal. `cmd.exe`, for one, repeats the last command on F3.
- **The Explorer tree:** F3 on a focused row opens that row's session's editor, beside its F2 (rename). The row's tip
  names F3 too.
- Where it is wired: `AllShortcutActions.h` (`ON_ALL_ACTIONS(OpenSessionNote)`), `ActionAndArgs.cpp` (the
  `openSessionNote` key and the action's name), `TerminalPage::_HandleOpenSessionNote`, and `defaults.json` (the
  command and its key). The prompt-scroll actions (`agentScrollToPrevPrompt`) are the model. ⚠ A new `ShortcutAction`
  means a purged build.

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

- **A popup card in the main tree**, the Tags panel's recipe (`_OpenTagEditorAt`): a `Popup` parented in `Root()`,
  opened one dispatcher tick after the invoking menu closes (the menu's refocus must land first), its box focused one
  tick after that, the keyboard's way back captured (`AgentCaptureFocusReturn`) and given back when it closes
  (`_ReturnKeyboardAfterPanel`). ⚠ Never a ContentDialog: a text box inside one gets no keypresses under XAML Islands.
- **Its parts:** a header ("Note · <session title>"), a multi-line box (`AcceptsReturn`, wrapping, about five lines
  tall, `MaxLength` 2,000), a hint line with the count ("93 / 2000"), and three buttons: **Remove note** · **Cancel** ·
  **Save**.
- **Keys:** Enter is a new line; **Ctrl+Enter saves**; **Esc cancels**. Both act on the KEY-UP: a close on the key-down
  hands the keyboard back to the terminal while the key is still down, and its ESC or CR would reach claude (Gotcha *A
  panel opened from a menu…*).
- **A press outside the card saves**, as a tab rename's click-away commits: a stray click never loses what you typed.
  Esc and Cancel are the only ways to drop an edit. Saving an empty or blank box removes the note; **Remove note**
  removes it at once and closes.
- **After a save:** the store write (§2), then this window's tab marker and side-panel card at once; other windows
  through the epoch; the tab's rich card at its next hover (the note rides its signature, §4a).
- **Its dark theme**, its tooltips through `AgentSetTip` with the Tags panel's 3 s delay (`kTagPanelTipDelay`), and
  every catch logged (Rule #18).

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

- **`TtBuildTooltipCard`** (TerminalPage.AgentObserver.cpp) gets a note block right after the header Grid: a Border
  with a dark amber fill (`#3A3220`), a 3 px amber left edge (`#E0B04C`) and corner radius 3, holding a dim caption
  ("NOTE · edited 2h ago": the stamp through the card's own `TtSpan`) over the text (Cascadia Mono 11.5, `#F3E6C4`,
  wrapping). A note longer than 12 lines ends its 12th line in "…", followed by "Edit Note… (F3) shows the rest".
- **Inert content only** (HANDOVER_tab-tooltip.md invariant 6a): TextBlocks and Borders, never a ScrollViewer.
- **`_PushSessionToolTipCard`** reads the note (one SessionStore read per hover, served by the store's record cache)
  and adds the text and its stamp to the card's **signature** (invariant 9), so an edit re-hosts the card at the next
  hover. The body's height budget (`maxCardHeight − kTtChromeReserve`) also subtracts the note block's estimated
  height, so a long note shortens the summary's viewport and never pushes the card past its 80%-of-window budget.
- Every surface of the shared builder shows the note the same way: the tab, the glyph's card (§4c), and the "Sum off"
  card of SIDE_PANEL.md §2d (which never opens: SP-K2).

### 4b. The tab marker — a glyph after the title

- A small note glyph (QuickNote, 12 px, the title's ink at 85%) **after the title, before the ✕**. `TabHeaderControl`'s
  `HeaderTitleRow` gets a third column (Auto) after the title's star column, so a long title trims with "…" in front of
  the glyph and never hides it.
- **Only a tab with a note has the element** (`x:Load="False"`, loaded at the first note), so the ~700 tabs of a
  large window pay nothing for it (Gotcha *Per-tab XAML is multiplied by ~700*).
- `IsHitTestVisible="False"`: hovering the glyph is hovering the tab, whose rich card shows the note.
- **The page sets it** (`Tab::SetAgentHasNote`, a bool): at bind and restore, at once after an edit in this window,
  and for an edit in another window when the observer lane's tick sees the epoch move. Never on a shell tab.
- Its AutomationId is `am.tab.note.<sid8>`, named "Note".

### 4c. The side-panel card — a glyph, and the full session card on it

- **The glyph:** a FontIcon (QuickNote, 11 px, the band's ink) in the title band, left of the tag ribbons.
  `bandGrid` becomes `[ title * ][ note Auto ][ tag strip Auto ][ pushpin Auto ]` and exists while the session is
  tagged OR pinned OR noted. The card keeps its height, so the virtualized list's one stride holds (SIDE_PANEL.md §2f).
  No corner radius, so no composition node (SIDE_PANEL.md §2f).
- **The model:** `AgentSidePanelCardModel` gains `hasNote`. The rows keep a per-row note cache refreshed by the epoch,
  the tag cache's recipe (SIDE_PANEL.md §2b): no store read per update unless a note changed.
- **Resting on the glyph opens the session's full rich card** — the tab's card with the note under its title (§4a) —
  **beside the panel**: right of a left-docked panel, left of a right-docked one, centered on the card and slid back
  inside the window (the "Sum off" placement, SIDE_PANEL.md §2d). Its summary body comes from the shared off-thread loader (one
  cache per session, shared with the tab) and lands in the open card when it arrives. The wheel over the glyph or the
  card scrolls the summary, and is marked handled only when the summary can scroll (else the list scrolls).
- **The mechanism is the tag hover panel's, not a ToolTip.** The "Sum off" design hosted this same card on this same
  card in an `AgentSessionToolTipHost` (a ToolTip that `ToolTipService` opens), and it never opened (SP-K2; the cause
  was never looked into). The tag ribbons' hover panel opens on these very cards in production: a `Popup` parented
  in `Root()`, a one-shot 160 ms open timer armed by the glyph's `PointerEntered` (no tree work inside pointer
  dispatch), and a 300 ms grace close on `PointerExited`, cancelled while the pointer is on the open card
  (`_OnTagBadgeHoverBegin` / `_OnTagBadgeHoverEnd` / `_ShowTagHoverPanelNow`). The glyph's card uses that recipe with
  one popup per window, its content rebuilt per open by the shared builder under its own surface key
  (`"\x1F" "sidepanel-note:" <id>`, so its signature and scroll state never mix with the tab's).
- **The card's own tip stays quiet over the glyph:** the glyph's entry cancels a pending card tip, closes an open one
  (`AgentCloseTipsIn`) and marks the event handled, and the glyph swallows the moves that bubble to the card — the
  ribbons' recipe (SIDE_PANEL.md §2b).
- **It also closes** when the card leaves the realized rows or the list, the panel collapses, Sum or the AI view
  toggles, the tab changes or the window deactivates.
- Its AutomationId is `am.side.note.<sid8>`, named "Note", and the card's item status gains a `note` facet
  (`AgentItemStatus`, `kAgentStatusNote`) for screen readers and the e2e driver.

### 4d. The card's own tip — deferred

The plain tip that opens after resting 4 s on a card's body ("Sum" on, the default) stays as it is: folder, branch,
tags, the gestures; no note. With "Sum" off, the card's designed tooltip is the full session card (SIDE_PANEL.md §2d), which would
show the note by itself through the shared builder, but it never opens (SP-K2). The user deferred the choice ("Defer,
i think its like the Full Session Card if Sum is off."); a "note: …" line like the tip's "tags: …" line was the
suggestion.

---

## 5. When the conversation changes (defaults)

- **The same tab, a new conversation** (`/clear`, an in-session `/resume`, `/handover-here`'s in-place successor; all
  reach the re-home in `_BindClaudeSessionToTab`): the tab keeps its note. It is COPIED to the successor when the
  successor has no note of its own (the side panel pin's carry, SIDE_PANEL.md §2j), and the predecessor keeps its copy,
  so resuming it later still shows it.
- **A new tab** (a fork, a `/handover` successor, Open New Session Here): starts without a note.
- **Close** keeps it; a restore or a reopened window shows it again.

---

## 6. Cost and safety

- **No disk read per card per update** (the per-row cache and the epoch) and **one store read per hover** (the tab's
  card, the glyph's card).
- **No element for a tab or a card without a note** (the tab's `x:Load`; the card's glyph only while noted).
- **No new composition node** on a card, and no storyboard.
- **One editor popup and one glyph popup per window**, each built once (the `_tagHoverPopup` recipe).
- **UI thread only**; the summary loader is today's (`_EnsureTabTooltipSummary`, its terminate-nets kept).
- **Purge:** `TerminalPage.h` and `Tab.h` gain members, `AgentSidePanelCardModel` a field, and TerminalSettingsModel a
  `ShortcutAction`: the first build is a purged one (CLAUDE.md *Building FAST* #5).

---

## 7. The change list

- `AgentMaster/SessionStore.{h,cpp}`: the two keys, `GetSessionNote(In)` / `SetSessionNote(In)`,
  `SessionNotesEpoch()`; their tests.
- `TerminalPage.AgentObserver.cpp`: the note block in `TtBuildTooltipCard`, the signature and the height budget in
  `_PushSessionToolTipCard`, the re-home carry in `_BindClaudeSessionToTab`.
- A new `TerminalPage.AgentNotes.cpp`: the editor card, `_HandleOpenSessionNote`, the tab markers' refresh.
- `TerminalPage.AgentSidePanel.cpp`: the glyph, `hasNote`, the per-row cache, the glyph's hover popup.
- `TerminalPage.h`, `Tab.{h,cpp}`, `TabHeaderControl.{xaml,h,cpp}`: the members, the tab menu item, the marker.
- `AgentManagerContent.Tree.cpp` (+ `.h`): the session menu item, the tree's F3, a note handler the page wires.
- `TerminalSettingsModel`: `AllShortcutActions.h`, `ActionAndArgs.cpp`, `defaults.json`, the action's resource name.
- `AgentAutomation.h`: the ids and the `note` facet.
- Docs: this file becomes the feature's doc; CLAUDE.md gets a feature entry; SIDE_PANEL.md, TAB_STRIP.md,
  HANDOVER_tab-tooltip.md and AGENT_MANAGER.md (the menu) point to it; the flow catalog gets the flows below.

---

## 8. Checks after the build

- **Set:** the tab menu shows "Set Note… (F3)" right under "Rename tab (F2)" on a Claude tab and not on a shell tab;
  the board card, the tree row and the side-panel card show it under "Rename (F2)"; the editor opens under the tab or
  card with its box focused and typing reaching it; Ctrl+Enter saves, Esc cancels, a click outside saves, an empty save
  removes; the label reads "Edit Note… (F3)" afterwards.
- **F3:** on a Claude tab it opens the editor; on a cmd tab it repeats the last command (it passed through); on a
  focused tree row it opens that row's editor.
- **Tab:** the marker shows after the title on noted tabs only and survives a long title; hovering the tab shows the
  note under the title; an edit shows at the next hover.
- **Side panel:** the glyph shows on noted cards only, the card's height is unchanged and scrolling stays smooth;
  resting on the glyph opens the full card beside the panel (on the right of a left dock, on the left after a dock
  flip), the card's own 4 s tip never opens over it, the wheel scrolls its summary; it closes on leaving.
- **Across windows and restarts:** an edit in window A shows in window B within ~2.5 s; a restart and a reopened
  window keep every note; a `/clear` keeps the tab's note; a fork starts without one.
- No `[exc]` line from the note paths in hooks.log.

Flows to add to the catalog (`doc/agentmaster/flows/`): a TAB flow (set, edit, remove from the tab menu and F3; the
marker; the hover) and an SP flow (the glyph; its card beside the panel in both docks; the card's own tip yielding).

---

## 9. Not in this design

The note on the Sessions page (a column, a search scope), on the board card's and the tree row's tips, on Telegram's
sticky card, in the AI search's tab tools and in the session database's `am_*` tables; notes on shell tabs (they would
live in the window record, the shell tab's only store).
