# 01 · The Agent Manager page and the Home button (MGR)

> Conventions and fixtures: [README.md](README.md). Spec: CLAUDE.md *Agent Manager page*; FAVORITES.md §4a;
> `TerminalPage.AgentEngine.cpp` (`_ShowManagerPage`, `_ApplyManagerHomeButtonFace`).

**What shipped.** 0.7.9: the pinned Manager tab became a per-window PAGE behind the tab strip's **Home**
button; a window may hold zero tabs (it shows the page instead of closing); Alt+Left and Ctrl+Home went back
to Windows Terminal. 0.7.10: Home turns into the **jump-back** arrow on the page; a double-click guard.
0.8.0: the board keeps its columns and paces its rebuilds by their cost.

## Lifecycle model

```text
                Home click / automatic show
  a tab shown ─────────────────────────────────► the page shown (no tab selected; Home = "active" accent)
       ▲                                              │
       │  any tab click · jump-back arrow · nextTab   │  Home's face:
       │  (first tab) · prevTab (last tab) · a new    │   a session THIS window hosts is selected → ↶ "Jump back to <title>"
       │  tab / launch · card double-click · toast    │   otherwise → ⌂ "Agent Manager" (a click does nothing)
       └──────────────────────────────────────────────┘

  automatic shows: the last tab closed · a close whose remaining tabs are all dormant · Deactivate of the
  focused tab · a zero-tab window at start · a window reopened while it was on the page
  a window that holds only its page (no tab) closes itself — unless it is the last window (then it stays,
  and its record is discarded so the next start does not reopen an empty window)
```

**Accessible names (UI Automation).** Home: `Agent Manager` (⌂, glyph E80F) or `Jump back to <tab title>`
(↶, glyph E7A7). The window title while the page shows: `Agent Manager` (`Agent Manager Dev` on the dev
identity).

---

### MGR-01 · Open the page and leave it

> **Since** 0.7.9 · **P0** · **Needs** F-S2 · **Spec** CLAUDE.md *Agent Manager page*

**Promise.** Home shows the Triage Board, Explorer Tree and Prompt Queue in place of the tab content; any
tab click returns to that tab; while the page shows, no tab is selected and tab actions target nothing.

**Phases**
1. **Open.** *Do:* click Home. *Expect:* the page replaces the terminal area (the side panel stays); the
   strip shows no selected tab; Home paints as the active entry (accent); the window title reads `Agent
   Manager`. *Check:* [ENG] census `managerPageVisible: true`, every tab `selected: false`; [LOG] `[nav]
   manager-page show (…)`.
2. **Tab actions do nothing.** *Do:* run `closeTab` from the command palette, and Ctrl+Shift+W. *Expect:* no
   tab closes (there is no focused tab).
3. **Leave by a tab.** *Do:* click the second tab. *Expect:* that tab's content shows; the page is hidden;
   Home returns to its normal look. *Check:* [LOG] `[nav] manager-page hide`, `tab-focus`.

**Edge cases**
- **E1 nextTab** (Ctrl+Tab) on the page → the FIRST tab; **prevTab** (Ctrl+Shift+Tab) → the LAST tab; the
  cycle never lands back on the page.
- **E2 Selecting the tab MUX still holds selected** (the one the page hid) → leaves the page (the funnel's
  blind spot is covered).
- **E3 A new tab** (Ctrl+Shift+T / a launch) → leaves the page onto it.
- **E4 Opening Settings** (Ctrl+,) → leaves the page.
- **E5 A second window** → each window has its own page and its own lens selection.
- **E6 Split pane on the page** → nothing (no pane is focused); an `agentManager` pane request is refused.

---

### MGR-02 · Home turns into the way back

> **Since** 0.7.10 · **P0** · **Needs** F-S2 · **Spec** CLAUDE.md *Agent Manager page* (jump-back)

**Promise.** On the page, Home shows a back arrow naming the tab it returns to whenever a session hosted in
this window is selected; a click goes back; with nothing to go back to it stays Home and does nothing.

**Phases**
1. **From a session's tab.** *Do:* select tab A (session `alpha`), click Home. *Expect:* the page opens with
   `alpha` selected (the lenses follow the tab you came from); Home shows ↶; its accessible name `Jump back
   to alpha`; the tooltip `Jump back to “alpha” — leave the Agent Manager …` after ~2× the hover time.
2. **Back.** *Do:* click ↶. *Expect:* tab A shows; Home is ⌂ again. *Check:* [LOG] `[nav] home-button
   jump-back -> <sid8>`, then `activate`, `manager-page hide`.
3. **Retarget.** *Do:* Home, then click session B's board card. *Expect:* ↶ now names B.
4. **Nothing to return to.** *Do:* **Clear** the selection on the board. *Expect:* ⌂ `Agent Manager`; a
   click does nothing (no navigation).
5. **Another window's session.** *Do:* board in GLOBAL scope, select a session hosted in window 2.
   *Expect:* ⌂ (no back arrow: this window does not host it).

**Edge cases**
- **E1 Double-click Home from a tab** → the page opens and STAYS (the second click within the double-click
  time is ignored: `[nav] home-button click ignored …`).
- **E2 Rename the selected session while the page shows** → hovering Home shows the new title (the face
  re-reads on hover).
- **E3 The selected session's tab is closed while on the page** → ⌂.
- **E4 Keyboard** — Home has no keyboard shortcut any more (E2E: Alt+Left / Ctrl+Home are WT's, MGR-05).

---

### MGR-03 · A window can hold zero tabs

> **Since** 0.7.9 · **P0** · **Needs** F-S, Deactivate, restart · **Spec** CLAUDE.md *Agent Manager page* (automatic shows), FAVORITES.md §4a

**Promise.** Closing or parking the last thing in a window shows its page instead of closing the window or
waking a dormant session.

**Phases**
1. **Close the last tab.** *Do:* a second window with one shell tab; close it. *Expect:* the window stays on
   its page… then closes itself shortly after (a page-only window that is NOT the last window
   self-closes). *Check:* [OS] the window is gone; its `windows/<id>.json` deleted.
2. **The last window.** *Do:* in the only window, close every tab. *Expect:* the window stays open on its
   page; on the next start the app does not reopen a page-only window (its record was discarded) — it
   opens a fresh default window.
3. **Deactivate the focused tab.** *Do:* with two tabs, Deactivate ▸ Deactivate Tab on the focused one.
   *Expect:* the PAGE shows first (a never-started control on screen would start its claude); the tab stays
   dormant.
4. **Close onto dormant tabs.** *Do:* after a restart (every restored tab dormant), close the focused tab.
   *Expect:* the page shows (selecting a dormant neighbor would start its claude). *Check:* [LOG]
   `[close-successor] …` naming the page.

**Edge cases**
- **E1 A zero-tab window at startup** (a record with no tabs) → shows its page.
- **E2 A window closed while on its page** → reopens on the page (its record names no focused tab).
- **E3 Two windows emptied at once** → never both close (the reservation) — the app keeps one.
- **E4 Shell tabs re-homing slowly** at a restore → the window is not mistaken for empty.

---

### MGR-04 · Alt+Left and Ctrl+Home are Windows Terminal's again

> **Since** 0.7.9 · **P1** · **Needs** split panes · **Spec** release notes 0.7.9

**Phases**
1. **Alt+Left** with two panes, focus in the right one → focus moves to the left pane (not to the Manager).
2. **Ctrl+Home** in a shell tab → reaches the shell (e.g. PSReadLine's beginning-of-history), no Manager.
3. **In a Claude tab** → the keys reach claude's TUI unchanged.

---

### MGR-05 · The linked lenses: tab ⇄ board ⇄ tree ⇄ queue ⇄ side panel

> **Since** 0.7.x · **P1** · **Needs** F-S2, F-W2 · **Spec** CLAUDE.md *C1 UI*

**Promise.** One selection drives every surface of a window: switching tabs selects the session in the
Manager; selecting a card or row shows its queue, its summary and its tab's selection pill.

**Phases**
1. **Tab → Manager.** *Do:* select tab B, open the page. *Expect:* B's card and tree row highlighted; the
   Prompt Queue shows B.
2. **Card → tab pill.** *Do:* hover / select A's card. *Expect:* a translucent accent pill behind A's tab.
3. **Card double-click.** *Expect:* jumps to A's tab (leaves the page).
4. **Board scope.** *Do:* LOCAL ⇄ GLOBAL on the board header. *Expect:* the tree's scope follows (one state);
   LOCAL lists only this window's sessions; persisted per window.
5. **Clear / Show all.** *Do:* select a folder scope in the tree, then Show all; then Clear. *Expect:* Show
   all appears only while a folder is scoped; Clear deselects (the Prompt Queue reads nothing selected).

**Edge cases**
- **E1 A session in another window** (GLOBAL) → double-click activates it there (the other window comes
  to the front, the tab selected).
- **E2 Restart** → the lens (selection, scope, splitters) comes back per window.

---

### MGR-06 · The board stays responsive with many sessions

> **Since** 0.8.0 · **P2** · **Needs** 30+ sessions (mock), a timing probe · **Spec** UI_THREAD.md §12

**Promise.** State changes refill the board's columns in place; rebuilds space themselves by what they
cost; the page never stops answering clicks.

**Phases**
1. **Many sessions.** Launch 30 sessions (mock replies), all cycling turns.
2. **Watch.** *Expect:* clicks on the page answer within ~1 s; [LOG] no repeated `[manager] slow rebuild …`
   (at most one a minute when a rebuild takes ≥ 1 s); the Error / External columns appear and vanish with
   their content.
3. **Hidden page.** *Expect:* while a tab shows, the board does not rebuild (one catch-up on the next show).

---

## Known defects (expected failures until fixed)

### MGR-K1 · A session launched from the page is not selected on the page
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (`e2e/suites/test_manager_page.py`
> `test_known_defect_a_launched_session_is_not_selected_on_the_page`)

Launching from the page's launch row opens and selects the new session's tab (the side panel follows it), but the
page's lens does not: **Home** → ⌂ `Agent Manager` (no way back to the tab you just came from) and the right pane
reads "Select a Claude session to see its summary." until the user switches tabs once; hooks.log has no `[nav]
tab-focus <sid8>` after the launch. **Cause (by reading):** the launch selects the new tab through the selection
funnel BEFORE `_claudeTabs` maps the tab to its session, so `_SyncManagerSelectionToTab` finds no session id and
returns. **Repro:** F-S (launched from the page; never switch tabs) → Home. **Expected:** ↶ `Jump back to t1` and
t1's summary — as after any tab switch (MGR-02, MGR-05 phase 1).

