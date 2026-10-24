# 23 · The tab's context menu (TCM)

> Conventions and fixtures: [README.md](README.md). Spec: `Tab.cpp` (`_CreateContextMenu`, `_EnableMenuItems`,
> `_AppendDeactivateMenuItems`, `_AppendCloseMenuItems`, the `SetAgent*` visibility setters) and `TerminalPage.cpp`
> (the flyout's `Opening` handler that decides what a tab's menu shows); CLAUDE.md *Persistence + archive/restore*
> (Deactivate, Close), *Settings cog* (launch models, Specify…), *Per-tab link badge* (the copy menu's fields);
> FAVORITES.md; TRANSCRIPT_OVERLAY.md; PENDING_INPUT.md §8 (Copy Current Prompt).

**What shipped.** Windows Terminal's tab menu grew Agentmaster's session actions over the 0.7.x line: **Copy ▸**
(Session Id, Path, Branch Name, Current Prompt, the launch command line, Summary, **Transcript ▸** at five levels),
**Mark Unread**, **Open In Explorer**, the triage move, **Favorite**, **Tags**, **New Session Here ▸** and **Fork
session ▸** with a model per launch (Default, Specify…, the launch models, then the presets), **Restart session**,
**Move tab ▸ Move to start / end**, and **Close tabs to the left**. 0.7.7: **Deactivate ▸** (Deactivate Tab, Other
Tabs, All Tabs) and **Close ▸ Of Same Folder / Other of Same Folder**. 0.7.9: **Show Transcript**, Copy ▸
**Transcript ▸** levels, the presets in every launch menu, Deactivate's left / right / folder scopes, Specify…
remembers the last pick. This file covers the MENU — what it shows on which tab, what is enabled where, and what each
command does; the deeper behavior of Deactivate, Close and the strip lives in [07](07-tabs-park-close-dock.md)
(TAB-01 … TAB-09), of the transcript view in [16](16-transcripts-and-summaries.md).

## Lifecycle model

```text
 right-click a tab ─► Opening: the page decides, FOR THIS TAB NOW (the menu is never cached):
     │                   session or shell · Claude or Codex · adopted or launched · dormant or started ·
     │                   the session's state (the triage item) · the window holds a managed session (★ close-all) ·
     │                   the tab color mode (Change tab color) · the launch models + presets (the two submenus)
     ├─ hover a ▸ item ─► its submenu opens beside it (a second popup)
     ├─ choose an item ─► the menu closes, the command runs on THIS tab (not on the selected one)
     └─ Esc / click outside ─► closed, focus back to the terminal
```

**The menu, top to bottom** (measured on 0.8.0, a started Claude session tab Waiting for you):

| Item | Session tab | Shell tab | Note |
|---|---|---|---|
| Activate Tab (Shift+Click) | only when DORMANT | — | first item when shown |
| Rename tab (F2) | ✓ | ✓ | |
| Copy ▸ | ✓ | — | Session Id · Copy Path · Copy Branch Name · Copy Current Prompt (Claude) · Claude Launch CLI or Codex Launch CLI · Summary · Transcript ▸ (Show, then the levels) |
| Show Transcript | ✓ | — | |
| Mark Unread | ✓ | — | |
| Open In Explorer | ✓ | — | |
| Move to Idle/Done · Move to Waiting-for-you | by state | — | Waiting / Error → "Move to Idle/Done"; Idle / Done → "Move to Waiting-for-you"; Running / Needs-approval → hidden |
| Favorite · Unfavorite | ✓ | — | the label follows the star |
| Tags | ✓ | — | |
| Split tab | ✓ | ✓ | |
| Move tab ▸ | ✓ | ✓ | Move tab to new window · Move right · Move left · Move to start · Move to end |
| Change tab color | ✓ | ✓ | disabled in the "Remove colors" mode |
| Export text · Find | ✓ | ✓ | |
| New Session Here ▸ | ✓ (plain for Codex) | ✓ | Default · Specify… · the launch models · (presets) · Add presets… |
| Restart session | ✓ | ✓ | |
| Fork session ▸ | ✓ (plain for Codex) | plain = Windows Terminal's duplicate tab | Default · Specify… · the launch models · (presets) · Add presets… |
| Deactivate ▸ | ✓, never for an ADOPTED session | — | Deactivate Tab (disabled when dormant) · Tabs to the Left · Tabs to the Right · Other Tabs · Of Same Folder · Other of Same Folder · All Tabs · Deactivate All Idle Tabs (2026-10-24, NOT BUILT: the window's Idle sessions; disabled while none would park — flows/07 TAB-02 phase 7) |
| Close ▸ | ✓ | ✓ | Close tabs to the left · Close tabs to the right · Close other tabs · Of Same Folder · Other of Same Folder (session) · Close all tabs · ★ Favorite & close all tabs (when the window holds a managed session) · Close pane (only when the tab is split) |
| Close tab | ✓ | ✓ | |

**Accessible names (UI Automation).** The menu is a MenuFlyout in its own popup window; every item is a `menu item`
named by its text (class MenuFlyoutItem, or MenuFlyoutSubItem with an expand state); a hidden item is absent, a
disabled one has IsEnabled false; a submenu opens in a second popup window.

---

### TCM-01 · The menu fits the tab it was opened on

> **Since** 0.7.x · **P0** · **Needs** F-S, the default shell tab · **Spec** `TerminalPage.cpp` flyout `Opening`

**Promise.** A session tab's menu offers the session actions; a shell tab's menu offers none of them (and no
Deactivate); a parked tab offers Activate; every item reflects the tab NOW, not when the menu was first built.

**Phases**
1. **Session tab.** *Do:* F-S; right-click the session's tab. *Expect:* exactly the "Session tab" column above, in
   that order, with "Move to Idle/Done" (Waiting for you) and "Favorite"; Close ▸ includes Of Same Folder, Other of
   Same Folder and ★ Favorite & close all tabs.
2. **Shell tab.** *Do:* right-click the window's PowerShell tab. *Expect:* none of Copy ▸, Show Transcript, Mark
   Unread, Open In Explorer, the triage item, Favorite, Tags, Deactivate ▸; Fork session is a plain item (no
   submenu); Close ▸ has no folder items but still ★ Favorite & close all tabs (the window holds a session).
3. **Dormant tab.** *Do:* Deactivate ▸ Deactivate Tab on the session (or restart, F-RS). *Expect:* "Activate Tab
   (Shift+Click)" is the FIRST item; Deactivate ▸ Deactivate Tab is disabled; the other Deactivate scopes stay
   enabled.
4. **A shell that became a session.** *Do:* in the PowerShell tab run `claude`, send a prompt. *Expect:* once the
   Fleet Observer binds it (its first prompt), the same tab's menu shows the session items — but Deactivate ▸ stays
   hidden (an ADOPTED session's ConPTY is the user's shell).

**Edge cases**
- **E1 The triage item follows the state** — Running → absent; Waiting → "Move to Idle/Done"; Idle → "Move to
  Waiting-for-you"; Error → "Move to Idle/Done" (the error dismissal).
- **E2 Favorite's label** — "Unfavorite" on a starred session.
- **E3 "Remove colors" tab coloring** (the cog) → Change tab color disabled on every tab.
- **E4 A Codex session** → Copy ▸ shows "Codex Launch CLI" and no "Copy Current Prompt"; New Session Here and Fork
  session are plain items.
- **E5 The launch models change** in the cog → the two submenus list the new models at the next open (no restart).
- **E6 A window with no managed session** → ★ Favorite & close all tabs is hidden.

---

### TCM-02 · Position decides what is enabled

> **Since** 0.7.x · **P1** · **Needs** three tabs (F-S2 + the shell) · **Spec** `Tab::_EnableMenuItems`

**Promise.** An item that could reach no tab is grayed, never a silent no-op: the gates follow the tab's position.

**Phases**
1. **Left-most tab.** *Expect:* Close ▸ Close tabs to the left, Move tab ▸ Move left and Move to start, Deactivate ▸
   Tabs to the Left — disabled; their right twins enabled.
2. **Right-most tab.** *Expect:* the mirror image: the "right" / "end" items disabled.
3. **A middle tab.** *Expect:* every position item enabled.
4. **One tab only.** *Expect:* Close other tabs and Deactivate ▸ Other Tabs disabled.

**Edge cases**
- **E1 An enabled scope that reaches only shells** (Deactivate ▸ Tabs to the Right over shell tabs) parks nothing:
  `[nav] deactivate-done … parked=0` — enabled by position, not by content (documented, not a defect).
- **E2 Deactivate All Idle Tabs is the one item gated by CONTENT** (2026-10-24, NOT BUILT): enabled only while the
  window holds a session in the Idle state that a park would accept (the page counts them at the flyout's Opening),
  whatever the tab's position.

---

### TCM-03 · Rename tab (F2) renames the session everywhere

> **Since** ≤ 0.7.7 · **P0** · **Needs** F-S · **Spec** CLAUDE.md Correctness Rule #11

**Promise.** One title: renaming the tab renames the session on every surface and on disk.

**Phases**
1. **Rename.** *Do:* tab menu → Rename tab (F2); select all; type `alpha`; **Shift+Enter** (the default commit mode,
   "click away or Shift+Enter": a plain Enter types a line break into the title). *Expect:* the box closes; the tab
   reads `alpha`; within ~1 s sessions.json's title is `alpha`; the side panel card and the board card read `alpha`;
   the window title reads `alpha`. *Check:* [ENG] sessions.json; [UI] the strip, the panel, the board; [LOG] `[nav]
   rename`.
2. **Restart.** *Do:* F-RS. *Expect:* the restored tab is titled `alpha`.

**Edge cases**
- **E1 A blank rename** (select all, Delete, Shift+Enter) → rejected: the title stays `t1` everywhere — today the
  session keeps it but the TAB shows Claude Code's own terminal title (TCM-K1).
- **E2 Surrounding spaces** (`  beta  `) → stored and shown as `beta`.
- **E3 Esc** → cancels, the title unchanged.
- **E4 F2** on the selected tab opens the same box.
- **E5 The commit mode** — the default ("click away or Shift+Enter"): Enter adds a line break (a multi-line title),
  Shift+Enter or a click outside commits; the cog's "Enter" mode: Enter commits.

---

### TCM-04 · Copy ▸ puts the session's facts on the clipboard

> **Since** ≤ 0.7.7; Transcript ▸ levels 0.7.9 · **P1** · **Needs** F-S in a git repository (`git init -b feature/x`)
> · **Spec** `AgentCopyActions.h` / `AgentTabOverlay.Actions.cpp` (`CopySessionField`)

**Promise.** Each Copy item puts exactly one fact of THIS session on the clipboard, the same as the badge's copy menu
and the board's.

**Phases**
1. **Session Id** → the conversation id (a GUID == sessions.json `id`).
2. **Copy Path** → the session's effective working folder (`C:\e2e\w\<test>\t1`).
3. **Copy Branch Name** → `feature/x`. *Edge:* a folder outside git → the clipboard is left unchanged (nothing to
   copy).
4. **Copy Current Prompt** → type `draft text` into the box WITHOUT sending; the item copies `draft text`; the box
   keeps it.
5. **Claude Launch CLI** → a PowerShell-runnable line: `& "…claude.exe" … --settings "…" … <the session id>`.
6. **Summary** → the full summary box, holding the session id and the first prompt.
7. **Transcript ▸ Default** → Markdown holding `ready` (the first prompt) and the reply.

*Check:* [OS] the clipboard; [LOG] one `[nav] copy <field>` per item (`session-id`, `path`, `branch`,
`current-prompt`, `claude-cli`, `summary`, `transcript-default`).

**Edge cases**
- **E1 A paste placeholder** in the draft (`[Pasted text #1 +40 lines]`) → Copy Current Prompt copies the pasted text
  itself (PENDING_INPUT.md §8e).
- **E2 A fork with no messages** → Summary and Transcript copy the SOURCE's conversation (TRANSCRIPT_OVERLAY.md §3e).

---

### TCM-05 · Show Transcript, Mark Unread, Open In Explorer, Favorite, Tags

> **Since** ≤ 0.7.7; Show Transcript 0.7.9 · **P1** · **Needs** F-S2 · **Spec** TRANSCRIPT_OVERLAY.md;
> NOTIFICATIONS.md §8; FAVORITES.md §5

**Phases**
1. **Show Transcript.** *Expect:* the reading view over the window (one tab per level, the conversation as Markdown);
   Esc closes it and the terminal gets the focus back (no ESC reaches claude). *Check:* [LOG] `[nav] transcript-show
   <sid8>` and `transcript-close <sid8> (esc)`.
2. **Mark Unread** on B while A is selected. *Expect:* B's dot grows the flash ring; the taskbar count reads 1;
   selecting B clears both. *Check:* [LOG] `[nav] mark-unread`; [OS] the overlay icon.
3. **Open In Explorer.** *Expect:* a File Explorer window on the session's folder. *Check:* [OS] a new explorer.exe
   window whose title is the folder's name; [LOG] `[nav] open-path`.
4. **Favorite.** *Expect:* the menu item reads "Unfavorite" at the next open; the tab wears the favorite crown;
   `session-store\<id>.json` holds `"favorite"`. Unfavorite reverses all three.
5. **Tags.** *Expect:* the Tags panel opens under the tab: a name box (with the keyboard focus), the color
   swatches, the tag list; a typed name + Enter adds the tag (`N of M tags`, the session store's `tags`); Esc
   closes it on the key-up and reaches nothing else — a running turn goes on — and the keyboard goes back to the
   terminal. *Check:* [UIA] the focused element is the edit `tag name` from ~one tick after the open and stays it
   past the menu's close; after Esc, a `TermControl`; [LOG] `[focus] tab menu closed while the keyboard is in an open
   popup (Windows.UI.Xaml.Controls.TextBox): no hand-back to the terminal` and `[focus] the Tags panel closed: the
   keyboard went back to …`. On 0.8.0 the focus stayed in the terminal (TCM-K2, fixed in code 2026-10-23, NOT BUILT
   into a release).

---

### TCM-06 · Move tab ▸ reorders the strip

> **Since** ≤ 0.7.7 · **P1** · **Needs** three tabs · **Spec** `TerminalPage::_TryMoveTab`

**Phases**
1. **Move to start** on the right-most tab → it is first; the selection stays where it was. *Check:* [LOG] `[nav]
   tab-move <ident> idx=2 -> 0`.
2. **Move right / left / to end** → one step / the end, each logged.
3. **Move tab to new window** → a second window opens holding the tab; the session's claude keeps running (same pid);
   the first window's LOCAL side panel no longer lists it, the second's does. *Check:* [LOG] `[move-out] <id>
   (Claude tab leaving this window; …)` and `[nav] move-out <sid8> (tab moved to another window)` (the gesture's tear-out
   logs `tab-send-to-window … win=-1` instead).

**Edge cases**
- **E1 The moved tab was selected** → it stays selected, its neighbor is NOT marked read (TAB-08).

---

### TCM-07 · New Session Here ▸ and Fork session ▸ start a new session from this tab

> **Since** ≤ 0.7.7; models 0.7.x; presets 0.7.9 · **P0** · **Needs** F-S, mock · **Spec** CLAUDE.md *Settings cog*
> (launchModels, Specify…); HARNESSES.md

**Promise.** Both start a NEW managed session from the tab they were opened on — New Session Here in its folder, Fork
session as a branch of its conversation — on the model picked in the submenu.

**Phases**
1. **New Session Here ▸ Default.** *Expect:* a new tab (selected), a new session id, the same folder, titled by the
   folder; a fresh conversation (its first request to the mock carries no earlier messages). *Check:* [LOG] `[nav]
   open-new claude dir=<folder>` … `done new=<sid8>`.
2. **Fork session ▸ Default.** *Expect:* a new tab `t1 (fork)` right after the source, selected; the fork's first
   prompt reaches the model WITH the source's conversation before it; the source session is untouched (same id,
   same transcript length). *Check:* [LOG] `[nav] fork-managed-begin` … `fork-managed-done new=<sid8> from=<sid8>`;
   [MOCK] the fork's request holds the source's prompt.
3. **A model.** *Do:* Fork session ▸ Sonnet. *Expect:* the fork's claude runs `--model sonnet` (its command line);
   the mock sees the model Claude Code resolved for it.
4. **Specify…** *Do:* Specify… → the "Specify a model" card → type `claude-mock-1` → **Use this model** (or Enter in
   the box, acted on its key-up). *Expect:* the fork runs `--model claude-mock-1`; the next Specify… opens pre-filled
   with it; **Cancel** launches nothing.

**Edge cases**
- **E1 Fork of a fork** → titled `t1 (fork 2)` (never `t1 (fork) (fork)`).
- **E2 A fork with no message yet, then a restart** → it is re-forked into the SAME id (its branch is not lost).
- **E3 New Session Here on a SHELL tab** → a Claude session in the shell's current folder.
- **E4 Fork session on a shell tab** → Windows Terminal's duplicate tab (a second shell), no session.
- **E5 The source's unsent draft** is carried into the fork's box (DRF fork carry).

---

### TCM-08 · Restart session

> **Since** ≤ 0.7.7 · **P0** · **Needs** F-S · **Spec** RESTART.md; HOOKS.md *Retired hosts*

**Promise.** Restart ends the tab's claude and starts it again on the SAME conversation, in the same tab.

**Phases**
1. **Restart.** *Expect:* the old claude exits, a new claude starts with `--resume <same id>`; the tab, its title
   and the session id stay; the state settles at Waiting for you (or Idle); the record is NOT archived afterwards.
   *Check:* [OS] a new claude pid; [LOG] `[restart] claude <id> (resume)` (the tab menu's Restart; the board's / tree's
   Restart and `/restartsession` add `[nav] restart-begin` … `restart-done`); [ENG] the same id, live.
2. **Continue.** *Do:* a prompt. *Expect:* the model receives it with the earlier conversation (the resume worked).

**Edge cases**
- **E1 Restart during a running turn** → cut off (CLAUDE.md *Cut-off turns*): Error until the next message — or, from
  the build after 381c6c715 (NOT BUILT), the "continue the interrupted turn?" question (INT flows).
- **E2 Restart on a dormant tab** → starts it in place (like Activate).
- **E3 Restart on a shell tab** → Windows Terminal's restart: a fresh shell in the same pane.

---

### TCM-09 · Close tab, Close ▸, and the triage move

> **Since** ≤ 0.7.7 · **P0** · **Needs** F-S2 · **Spec** FAVORITES.md §4; CLAUDE.md *Persistence + archive/restore*

**Promise.** Closing a session asks first and never deletes it; a batch asks once; the triage item moves a session
between the board's columns without touching its claude.

**Phases**
1. **Close tab → Cancel.** *Expect:* the dialog "Close session?" (Close · ★ Favorite & Close · Cancel); Cancel keeps
   the tab and its claude.
2. **Close tab → Close.** *Expect:* the tab goes; the claude exits; the record stays in sessions.json (resumable
   from Sessions); nothing on disk is deleted (the transcript stays). *Check:* [LOG] `[nav] close-begin` …
   `close-done`.
3. **★ Favorite & Close** on the other session → closed AND starred (`session-store` favorite).
4. **Close ▸ Close other tabs** with two sessions and a shell → ONE dialog (Close All · ★ Favorite & Close All ·
   Cancel All); Close All closes the others, the menu's tab stays.
5. **Move to Idle/Done** on a Waiting session → its state reads Idle, its card moves to the Idle / Done column, the
   menu offers "Move to Waiting-for-you" next; that item moves it back. Its claude is untouched (same pid).

**Edge cases**
- **E1 Close tab on a shell tab** → it asks too: "Close tab?" — `"<title>" will be closed.` (Close · Cancel); measured on
  0.8.0 (the close confirmation cannot be turned off, CLAUDE.md *Settings cog*).
- **E2 Closing the last tab** → the window stays on its Agent Manager page (MGR-03).
- **E3 Closing onto dormant neighbors** → the page shows instead of waking one (TAB-04).
- **E4 Close ▸ Of Same Folder** → TAB-03.

---

## Known defects (expected failures until fixed)

### TCM-K1 · A blank rename leaves the tab showing Claude Code's own title
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (`e2e/suites/test_tab_menu.py`
> `test_known_defect_blank_rename_shows_claudes_own_title`)

Rename tab (F2) → select all → Delete → Shift+Enter on a session renamed `alpha`: sessions.json keeps `alpha` (the
blank rename is refused, correctly), but the tab — and with it the window title — then reads Claude Code's own
terminal title (`✳ Mock session title` under the mock: the `✳` status glyph and the title Claude Code generated), so
the tab and the session no longer share one title (CLAUDE.md Rule #11). `_SyncClaudeTitleFromTab` is meant to re-pin
the managed name when the override is emptied; the cause of the miss is not identified yet. **Expected:** the tab
reads `alpha` again.

### TCM-K2 · Tags from the tab menu leaves the keyboard in the terminal — FIXED in code 2026-10-23 (NOT BUILT into a release)
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (its known-defect test, replaced by
> `e2e/suites/test_tab_menu.py::test_tags_from_the_tab_menu_take_the_keyboard`, marked `needs_fix("TCM-K2")`).
> **The fix:** the tab menu's `Closed` hand-back stands aside while the keyboard is inside an open popup; the Tags
> panel closes on the Escape KEY-UP and gives the keyboard back when it closes; the "Specify a model" prompt that the
> tab menu's model submenus open gives it back the same way (`AgentPanelFocus.h`, `Tab.cpp`). What it was on 0.8.0:

Tab menu → **Tags**: the panel opens, but the focused element stays the terminal (UI Automation, polled 1.5 s):
a typed tag name lands in Claude's input box as an unsent draft, and **Esc goes to claude** — it would interrupt a
running turn — while the panel stays open. The panel works once clicked into (a name + Enter adds and stores the tag).
**Cause (by reading):** the tab menu's `Closed` handler raises `RequestFocusActiveControl` (Windows Terminal's
GH#5750: "toss the focus back to our control" after a menu) unless a RENAME is in progress — the Tags panel's own
focus is taken back. **Expected:** the panel's name box has the focus; Esc closes the panel and reaches nothing else.

**How it happened, from the WinUI source** (microsoft-ui-xaml, read 2026-10-23):
- `MenuFlyoutItem::Invoke` raises Click FIRST and closes the menu after it. The menu's popup closes in that same call
  and gives the focus back to what had it before the menu opened (`CPopup::Close`, its light-dismiss branch).
- The Tags handler opens the panel one dispatcher tick later and focuses its box one tick after that: the box had the
  keyboard within a few milliseconds.
- The menu's `Closed` EVENT comes much later: `FlyoutBase` raises it from its presenter's `Unloaded`, after the close
  animation (`FlyoutBase::OnPresenterUnloaded`). Its GH#5750 hand-back (`_FocusCurrentTab`) then focused the
  terminal and took the keyboard out of the box. The same happened to the "Specify a model" prompt that a tab menu's
  New Session Here ▸ / Fork session ▸ opens.

**The fix, in three parts:**
1. **`Tab.cpp`, the menu's `Closed`:** the hand-back is skipped while the keyboard is inside an open popup
   (`AgentFocusInOpenPopup`: the focused element lies under the Child of a popup that `GetOpenPopupsForXamlRoot` lists).
   An Esc that dismisses the menu leaves the focus on the tab or the terminal, never in a popup, so GH#5750's own case
   still gets the hand-back. The color picker is not affected: its Flyout opens only after the menu's `Closed` (WinUI
   stages a flyout that opens while another one closes). One `[focus] tab menu closed while the keyboard is in an open
   popup (<class>): no hand-back to the terminal` line per such close.
2. **The Tags panel closes on the Escape KEY-UP** (a key-down inside the panel arms it, any other key-down disarms;
   both hooks `handledEventsToo` on the card). On the key-down, the rest of the keystroke — its ESC character — went to
   whatever got the keyboard next, which is claude once the keyboard goes back to the terminal; by the key-up the
   character has gone to the panel's own box (the Specify prompt's rule; upstream's tab renamer closes on key-up too).
3. **The panel gives the keyboard back when it closes while holding it.** It is a plain Popup: closing it with the
   focus inside only removes the focused box, and the focus manager then moves the focus to the next focusable
   element it finds (`CFocusManager::SetFocusOnNextFocusableElement`) — anywhere in the window. `_CloseTagEditorPopup`
   and `_HideSpecifyModel` now ask `AgentPanelHoldsKeyboard` BEFORE the close and then call `_ReturnKeyboardAfterPanel`:
   the Control that held the keyboard when the panel opened (`AgentCaptureFocusReturn`, read right before the box takes
   it — the terminal, a board card, a Sessions row), else the current tab. A tab header counts as "the current tab":
   after a tab's menu the keyboard belongs to the terminal (GH#5750's rule). A close the panel did not hold the
   keyboard for moves no focus (an outside press already focused what it hit; a tab switch already focused the new
   tab). One `[focus] <panel> closed: the keyboard went back to …` line per such close.

⚠ `TerminalPage` gained data members (`_tagEditorReturnFocus`, `_tagEditorEscArmed`, `_specifyModelReturnFocus`):
the next build must purge.
