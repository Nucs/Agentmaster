# 07 · Tabs: park, close, dock, drag, find (TAB)

> Conventions and fixtures: [README.md](README.md). Spec: CLAUDE.md *Persistence + archive/restore*
> (Deactivate, Close), Gotchas *MUX TabView drag-start…*, *The tab strip VIRTUALIZES…*, *A TabViewItem never
> clamps…*; FAVORITES.md §4–§5; HOOKS.md *Retired hosts*.

**What shipped.** 0.7.7: **Deactivate ▸** (park a session, tab kept); **Close ▸ Of Same Folder / Other of
Same Folder** across windows in one confirm; cross-window tab DOCKING; native tab drag replaced by a
pointer-owned gesture (the WinUI drag crash class); the tab color picker opens ready. 0.7.9: Deactivate gets
Close's scopes (left / right / folder / other of folder); a parked tab is exactly a restored tab (no more
plain-shell tabs, no archive seconds after a Restart); closing a tab never wakes a dormant neighbor; the strip
reveals far tabs, a tab opened from another tab's menu does not scroll the strip back, the strip can be
scrolled away from the selection, a long title ends in "…" inside its tab, moving the focused tab does not
mark its neighbor read.

## Lifecycle model

```text
 a managed tab:  running ──Deactivate──► PARKED (claude shut down; tab, title, color, tags, queue, mode kept;
                    ▲                       │      a never-initialized control on a resume connection)
                    └──── enter / Activate ─┘ (the conversation resumes; a remembered draft is typed back)

                 running/parked ──Close──► ARCHIVED (tab gone, record kept, resumable in Sessions; never deleted)
                                  └─ ★ Favorite & Close: the same + starred

 the strip: tabs reorder / tear out / dock by a pointer gesture (no native drag); far tabs are revealed on selection
```

---

### TAB-01 · Deactivate a tab and wake it

> **Since** 0.7.7 / 0.7.9 · **P0** · **Needs** mock · **Spec** CLAUDE.md *Deactivate ▸*

**Promise.** Deactivate shuts a session's claude down while the tab keeps its place, title, color, tags,
queue and runner mode — exactly a window-restored tab; entering or activating it resumes the conversation.

**Phases**
1. **Park.** *Do:* F-S (Waiting, a draft `keep me`, a queued Pending prompt, a tag); tab menu → **Deactivate ▸
   Deactivate Tab**. *Expect:* no confirm; its `claude.exe` exits gracefully (SessionEnd); the tab shows the
   dormant (half-hollow) dot; title, color, tag ribbon, queue and mode unchanged; the draft's dots stay still.
   *Check:* [OS] the claude process gone; [LOG] `[nav] deactivate-begin` / `deactivate-done`,
   `[deactivate] <sid8> … parked dormant …`, `[retired-host] …`; [ENG] `started=false`, `live=true`.
2. **Stays parked.** *Expect:* no claude starts while another tab is selected; nothing is sent from its queue
   (PQR-10); the tab is NOT turned into a plain shell, NOT archived (wait 30 s, census still has it).
3. **Wake.** *Do:* click the tab. *Expect:* the conversation resumes (`claude --resume` same id); the draft is
   typed back (DRF-06); the queue continues after settle (Auto).
4. **Activate without switching.** *Do:* park again; tab menu → **Activate Tab (Shift+Click)**, or Shift+Click
   the tab (on 0.8.0 that switches to it: TAB-K4, fixed in code 2026-10-23). *Expect:* it starts in place
   (`claude --resume`, `[nav] activate-dormant <sid8>`) without being selected; the Shift+Click also logs `[nav]
   tab-shift-click <sid8> "<title>": started in place, no switch (its release will not select it)`, no `[nav]
   tab-focus`, and the keyboard stays in the tab you were on. A Shift+Click on a session tab that already runs
   starts nothing (`…: nothing to start …`) and does not switch either.

**Edge cases**
- **E1 The focused tab** → the Agent Manager page shows first (MGR-03).
- **E2 A session mid-turn** → the turn is cut off → Error (STA-07 E5); an API-error tail is rewound after the
  exit (INT-02).
- **E3 Adopted (hand-typed) session** → refused (the menu item is disabled / nothing happens).
- **E4 Already dormant** → "Deactivate Tab" grayed.
- **E5 A delivery in flight** → skipped (the gate owner holds the box).
- **E6 The right-clicked tab was on screen** → still on screen after the park (the strip does not jump);
  [LOG] `[strip] deactivate: …` offsets.
- **E7 App restart** → a parked tab reopens as an ordinary restored-dormant tab.
- **E8 "Activate All Tabs (N)"** (the page's toolbar, shown while the window holds dormant tabs) → this window's
  dormant tabs start in place, ~500 ms apart, none selected (`[activate-all] window {…} drip-start …`). While another
  window holds dormant tabs too it asks first: "Activate dormant tabs" — "This window: N • All windows: M." —
  **This window (N)** · **All windows (M)** · **Cancel**.

---

### TAB-02 · Deactivate's scopes

> **Since** 0.7.9 · **P1** · **Needs** mock, F-W2 · **Spec** CLAUDE.md *The scopes mirror the "Close ▸" submenu*

**Phases** (window 1: tabs T1…T5, T2/T4 in folder `fA`; window 2: T6 in `fA`)
1. **Tabs to the Left** on T3 → T1, T2 parked; T3–T5 untouched.
2. **Tabs to the Right** on T3 → T4, T5 parked.
3. **Other Tabs** on T3 → all but T3 parked.
4. **Of Same Folder** on T2 → T2, T4 AND T6 (window 2) parked, without a confirm. *Check:* [LOG] `[nav]
   deactivate-done … elsewhere=1 dir=<fA>`; window 2 logs `[nav] deactivate-remote <sid8>`.
5. **Other of Same Folder** on T2 → T4 and T6; T2 keeps running.
6. **All Tabs** → every managed tab of THIS window parked; window 2 untouched.
7. **Deactivate All Idle Tabs** (2026-10-24, NOT BUILT; Deactivate ▸'s last item, here and in the card / row
   menu — flows/02 SP-02 E7; test `test_deactivate_all_idle_tabs`, marked `needs_fix("TAB-F1")` until a release
   holds it) → with T1 and T3 Idle (launched with no prompt), T2 Waiting for you and no Idle tab in any other
   window: from any tab of window 1, T1 and T3 park at once — nothing asks, since "All windows" would park the
   same tabs — and T2 keeps running; window 2 untouched. Running, Needs-approval, Error and Done sessions keep
   running too. *Check:* [LOG] `[nav] deactivate-idle-answer this-window (no other window holds an idle tab;
   nothing asked)`, then `[nav] deactivate-done scope=idle parked=2 skipped=0`.
8. **This window or All windows** (2026-10-24, NOT BUILT; the user's "Add prompt asking if to deactivate This
   window or All windows"; test `test_deactivate_idle_tabs_asks_which_windows`, `needs_fix("TAB-F1")`) → window 1
   holds T1, T2 (Idle) and T4 (Waiting for you), window 2 holds T3 (Idle). From T4's menu the item asks first:
   a dialog **Deactivate idle tabs** — "This window: 2 • All windows: 3." — with **This window (2)** (the
   default) · **All windows (3)** · **Cancel**. *Cancel* parks nothing. *This window* parks T1 and T2; T3 and T4
   keep running. Asked again (window 1 now holds no Idle tab): **This window (0)** is grayed and Cancel is the
   default; *All windows (1)* parks T3 in window 2 (its focused tab: window 2 shows its Agent Manager page
   first), and T4 keeps running. *Check:* [LOG] `[nav] deactivate-idle-ask from=tab menu here=2 elsewhere=1`,
   `[nav] deactivate-idle-answer cancelled …` / `… this-window` / `… all-windows (1 idle session(s) handed to the
   windows that host them)`; window 2 logs `[nav] deactivate-remote <T3> (an idle-tabs deactivate started in
   another window)` and its own `deactivate-begin scope=this` / `deactivate-done … parked=1`.

**Edge cases**
- **E1 Gates:** Tabs to the Left grayed on the left-most tab, Tabs to the Right on the right-most, Other Tabs
  on a window's only tab; an enabled item can still park nothing (`parked=0`). Deactivate All Idle Tabs is
  grayed while NO window holds an Idle session it would park (phase 8 leaves none: T1, T2 and T3 are dormant);
  with Idle tabs in other windows only, it stays enabled and asks (phase 8's second question).
- **E2 A running turn** in a folder-scoped tab of window 2 → cut off without asking (the tooltip warns "in
  EVERY window, without asking first").
- **E3 Shell tabs in scope** → untouched (only managed sessions park).
- **E4 A session that is busy again when the answer reaches its window** (phase 8, All windows) → it keeps
  running; that window logs `[deactivate] <sid8> left running - another window's "Deactivate All Idle Tabs" (all
  windows) named it, but it is no longer idle (<state>)`. The prompt's counts are read at the click (the other
  windows' from the registry: Idle, started, not adopted), so a window may then park one fewer than it said.

---

### TAB-03 · Close keeps the session; Of Same Folder closes a folder's sessions

> **Since** 0.7.7 · **P0** · **Needs** mock, F-W2 · **Spec** FAVORITES.md §4–§5; CLAUDE.md *Close ▸*

**Promise.** Close never deletes: the session leaves the board and stays resumable from the Sessions page;
★ Favorite & Close stars it in the same gesture; a folder's sessions close across windows in ONE confirm.

**Phases**
1. **Close one.** *Do:* the tab's ✕ on F-S. *Expect:* a confirm **Close · ★ Favorite & Close · Cancel**;
   Close → the tab goes; the claude exits; [ENG] `live=false` (archived); the Sessions page lists it with
   **Resume here**.
2. **Favorite & Close.** *Do:* another session → ★ Favorite & Close. *Expect:* archived AND starred (★ in
   the Sessions page).
3. **Unfavorite & Close.** *Do:* a starred session's ✕. *Expect:* the secondary button reads **☆ Unfavorite &
   Close**.
4. **Of Same Folder.** *Do:* tab menu → Close ▸ **Of Same Folder** (3 sessions in the folder, one in window
   2). *Expect:* ONE confirm, "Close 3 sessions in this folder?", naming the folder and listing their titles as
   bullets (≤ 20 + "…and N more"), **Close All** · **Cancel**; Close All → all three closed (window 2's through its own
   window; a window left without a tab then closes itself, MGR-03). *Check:* [LOG] `[nav] close-folder begin
   dir=<folder> sessions=3` … `close-folder done`, one `[archive] <id>` per session.
5. **Other of Same Folder** → the same minus this one: "Close 2 other sessions in this folder?", `except=<sid8>`
   in the log lines.

**Edge cases**
- **E1 Cancel** → nothing closes (`[nav] close-folder cancelled dir=<folder>`).
- **E2 A batch of managed tabs** (Close ▸ Close other tabs / Close all tabs) → ONE dialog **Close All · ★ Favorite &
  Close All · Cancel All** (TCM-09). The window's ✕ asks "Close this window?" instead (TAB-10).
- **E3 Only shell tabs in a batch** → no dialog.
- **E4 Confirm before closing OFF** (cog) → no dialog.
- **E5 A closed session's transcript** is untouched on disk.
- **E6 An already-exited session in the folder** → archived in place; an alive-but-unhosted one left open.

---

### TAB-04 · Closing a tab never starts another claude

> **Since** 0.7.9 · **P0** · **Needs** F-RS (dormant tabs) · **Spec** FAVORITES.md §4a

**Promise.** After a close, focus goes to a tab that starts nothing (or to the Agent Manager page) — a
dormant neighbor is never woken by a close.

**Phases**
1. **Arrange.** *Do:* restart with 5 managed tabs; the focused one starts, the other 4 dormant.
2. **Close the focused tab.** *Expect:* focus moves to the nearest NON-dormant tab or, if all are dormant, the
   Agent Manager page; no new `claude.exe` starts. *Check:* [OS] the claude count drops by one and stays;
   [LOG] `[close-successor] …` when it overrides the default pick.
3. **MRU mode** (Terminal setting `tabSwitcherMode: mru`) → the most recently used tab that starts nothing.

---

### TAB-05 · Drag to reorder, tear out, and dock into another window

> **Since** 0.7.7 · **P0** · **Needs** pointer drag, F-W2 · **Spec** CLAUDE.md Gotcha *MUX TabView drag-start null-deref*

**Promise.** Tabs move by a pointer gesture: reorder within the strip (a caret shows the slot), tear out into
a new window, or dock into another window's strip at the aimed slot — never crashing, whatever the strip's
scroll.

**Phases**
1. **Reorder.** *Do:* press a tab, drag 6+ DIP along the strip. *Expect:* an accent insertion caret tracks the
   slot; release → ONE move. *Check:* [LOG] `[nav] tab-drag-begin <ident>` … `tab-drag-end from=N to=M`.
2. **Edge auto-scroll.** *Do:* with 60 tabs (overflowing), drag to the strip's edge and hold. *Expect:* the
   strip scrolls; release places the tab.
3. **Tear out.** *Do:* drag a tab away from the strip and release. *Expect:* a new window with that tab; the
   session keeps running. *Check:* `(torn out to a new window)`; [LOG] `[move-out]`.
4. **Dock.** *Do:* drag a tab over window 2's strip. *Expect:* window 2 shows ITS insertion caret; release →
   the tab moves into window 2 at that slot; window 2 comes to the front. *Check:* `(docked into window <id>
   at slot N)`.
5. **Cancel.** *Do:* start a drag, press Esc. *Expect:* nothing moved (`cancelled: esc`).

**Edge cases**
- **E1 A scrolled, overflowed strip** (the old crash) → no crash on any drag (WER clean).
- **E2 Release over the titlebar right of the tabs** of window 2 → appends there (not a new window).
- **E3 Release over a window of ANOTHER install** (dev beside release) → a new window (never cross-process).
- **E4 Touch** → strip panning, no drag.
- **E5 The tab closed mid-drag** → `cancelled: tab closed mid-drag`.

---

### TAB-06 · The strip finds the selected tab and keeps its place

> **Since** 0.7.9 · **P1** · **Needs** 200+ tabs (shell tabs suffice for most) · **Spec** CLAUDE.md Gotcha *The tab strip VIRTUALIZES*

**Promise.** Selecting a tab anywhere (a card, the jump-back, a toast, `switchToTab`, a close successor)
scrolls the strip to it; a tab opened from another tab's menu does not scroll the strip back; the strip can
be scrolled away from the selected tab.

**Phases**
1. **Far selection.** *Do:* 250 tabs; from the Manager, double-click the card of tab #240 while the strip
   shows #1–#20. *Expect:* the strip scrolls so #240 is visible.
2. **No snap-back.** *Do:* scroll the strip far from the selected tab (mouse wheel / `>`). *Expect:* it stays
   there (no snapping back while the strip resizes).
3. **New tab from a menu.** *Do:* on tab #120, menu → New Session Here / Fork session. *Expect:* the new tab
   (beside #120) is on screen; the strip did not scroll back to an old position.
4. **A wholly visible tab clicked** → the strip does not move.

**Edge cases**
- **E1 Jump back (MGR-02) to a far tab** → revealed.
- **E2 A renaming tab** → kept on screen while its box is open.

---

### TAB-07 · Titles stay inside their tabs

> **Since** 0.7.9 · **P2** · **Needs** a long title · **Spec** CLAUDE.md Gotcha *A TabViewItem never clamps…*; Tab size (DSP-05)

**Phases**
1. **Long title.** *Do:* rename a session to 120 characters. *Expect:* the title ends in `…` inside the tab;
   the close button and separator stay visible.
2. **Rename box.** *Do:* double-click the title. *Expect:* the box is bounded by its tab; in Equal width the
   selected tab is held at the maximum tab width while the box is open.
3. **Multi-line title** (a title with a line break) → one line in the strip.

---

### TAB-08 · Moving the focused tab does not mark its neighbor read

> **Since** 0.7.9 · **P2** · **Needs** mock (an unread neighbor) · **Spec** CLAUDE.md Gotcha (the `_movingTab` note)

**Phases**
1. **Arrange.** Tab B (neighbor) finishes a turn in the background → flash ring, unread count 1.
2. **Move.** *Do:* move the focused tab A one slot (moveTab / Ctrl+Shift+Right). *Expect:* B keeps its flash
   ring and stays unread; no `tab-focus B` in the log.

---

### TAB-09 · The tab color picker opens ready

> **Since** 0.7.7 · **P2** · **Spec** release notes 0.7.7

**Phases**
1. **Open.** *Do:* tab menu → Change tab color. *Expect:* Custom and its RGB / HSV / Hex inputs start
   expanded; **Use Tab Color** seeds the picker with the color the tab already wears.
2. **Remember.** *Do:* collapse the advanced inputs, close, reopen. *Expect:* stays collapsed.
3. **Recolor a session** → every live tab in that folder recolors and the color persists (Rule #12,
   `dir-colors.json`); [LOG] `[nav] tab-color dir=… -> #hex`.
4. **Remove colors mode** (cog → Tab coloring) → the item is disabled on every tab.

---

### TAB-10 · Quitting keeps the workspace

> **Since** ≤ 0.7.7 (the regression base: CLAUDE.md Rule #16, PERSISTENCE.md §13.5) · **P0** · **Needs** F-S2, F-RS
> · **Spec** `TerminalPage::CloseWindow` / `RequestQuit` (the close flush), `_RestoreWindowTabs`

**Promise.** However the window is closed — its ✕ with **Close Window** or **Close All Windows** — the next start
reopens the same tabs: every Claude tab as its SESSION (resumed when entered), every shell tab as a shell, the tab
that was focused selected, and only that tab started — plus a busy one (INT-04) and, when the focused tab was a
SHELL, the last Claude tab (E1, by design).

**Phases**
1. **Close Window.** *Do:* F-S2, rename t1 `alpha`, select t2; the window's ✕ → "Close this window?" (Close All
   Windows · Close Window · Cancel) → **Close Window**. *Expect:* the app quits (the last window); `windows\<id>.json`
   holds the shell and BOTH Claude refs (kind `Claude`, the session ids), `selectedSessionId` = t2.
2. **Reopen.** *Do:* start again. *Expect:* the tabs `alpha`, `t2` and the shell (Claude tabs first — the known
   interleave limit, PERSISTENCE.md §13.5); t2 selected and started; `alpha` dormant (no claude.exe, Activate first in
   its menu); entering it resumes the conversation.
3. **Close All Windows.** *Do:* the ✕ → **Close All Windows** → "Do you want to close all windows?" (Close all ·
   Cancel) → Close all. *Expect:* the same record and the same reopen.

**Edge cases**
- **E1 A shell tab focused at the quit** → the reopen selects the shell, and the LAST restored Claude tab (the
  right-most of the Claude tabs, which lead the reopened strip) starts with the window; every other Claude tab stays
  dormant (no claude.exe, Activate first in its menu). A feature, not a defect (the user's decision of 2026-10-23;
  filed that morning as TAB-K2, see *Reclassified* below). [LOG] `[nav] tab-focus <last Claude tab>` at the reopen,
  then `[rehome] … select=… of … refs` names the shell; [OS] one claude.exe, the last Claude tab's.
- **E2 A tab switch, a rename or a resize just before the close** → the record still holds the Claude refs, and
  nothing writes it after the close's own save (TAB-K1: on 0.8.0 the autosave such a change armed could fire after
  the close archived the sessions and save every Claude tab as a shell; fixed in code 2026-10-23).
- **E3 Several windows** → **Close Window** on one keeps its record for **Reopen Windows (N)**, also with a tab switch
  just before the close (E2); the next start reopens the windows that were open at the quit ("Reopen your N previous
  Agentmaster windows?", Yes / No).
- **E4 A Claude tab focused that is not the last Claude tab, with a shell tab in the window** → only the focused tab
  starts. Today the last Claude tab starts too (TAB-K5, E1's mechanism; E1 with a SHELL focused is a feature).

---

## Known defects (expected failures until fixed)

### TAB-K1 · A close can save the window's Claude tabs as plain shell tabs — FIXED in code 2026-10-23 (NOT BUILT into a release)
> **P0** · Seen once by the e2e suite on 2026-10-23 against 0.8.0 (run5, the restart test; 5 later attempts kept the
> refs), then REPRODUCED on 0.8.0 the same day (run tabk1-repro3: 3 of 9 closes) by
> `e2e/suites/test_workspace.py::test_a_close_right_after_a_tab_switch_keeps_the_claude_tabs` and
> `…::test_closing_one_of_two_windows_keeps_its_claude_tab` (`needs_fix("TAB-K1")`) · evidence: each case's
> `close-timeline.json`, hooks.log and `windows\<id>.json`

**What it was on 0.8.0.** After **Close Window** the record held `[Other, Other]` for a shell and a Claude tab (the
Claude tab as an anonymous shell with a `renameTab "alpha"` action), so the next start reopened it as a PowerShell tab
titled `alpha`; the session itself was kept (sessions.json, resumable from Sessions). hooks.log: `[nav] window-close
confirmed` → `[teardown-archive] <id>` → **67 ms later** `[window-save] … tabs=2 [sh,sh,]`.

**The cause.** `CloseWindow` (like every teardown seam) flushes the record, sets `_windowRecordTeardownFlushed` and
then archives the window's sessions (`_ArchiveWindowSessionsOnTeardown`), which empties `_claudeTabs`. But the close
is only POSTED to the emperor (`AppHost::_CloseRequested` → `WM_CLOSE_TERMINAL_WINDOW`), so the page and its 750 ms
autosave (`_saveWindowRecordThrottled`, restarted by every tab switch, tab add or remove, resize and lens change) live
a little longer, and `_FlushWindowRecord` — the autosave's body — never checked the latch. An autosave still pending at
the close therefore captured the window after the archive, read every Claude tab as an anonymous shell
(`_ClaudeSessionForTab` found no session) and saved that over the close's good record. A window that is one of
several loses what **Reopen Windows (N)** brings back too: `UnregisterLiveWindow` reads the record from DISK into the
reclaimable pool.

**Reproduced** (2026-10-23, 0.8.0, Claude Code 2.1.291, a claude-fleet Windows 11 desktop). The ✕ opens "Close this
window?" about a second after the click, so a tab switch made BEFORE the ✕ always lands its save first (run
tabk1-repro: the dialog was found 0.85–1.6 s after the switch). The tests open the dialog first, switch tabs under it
through UI Automation (`[nav] tab-focus <sid8>` marks the moment the autosave restarted) and invoke **Close Window** a
chosen time after the switch; each case's `close-timeline.json` holds the times read off hooks.log's stamps:

| Close | Close Window invoked after the switch | Archived after the switch | The save fell due | Result |
|---|---|---|---|---|
| The only window (the app quits) | 250 ms | 521 ms | 229 ms after the archive | no late save; refs kept |
| | 325 ms | 588 ms | 162 ms after | no late save; refs kept |
| | 400 ms | 655 ms | 95 ms after | no late save; refs kept |
| | 475 ms | 720 ms | 30 ms after | **saved 95 ms after the archive: `[sh,sh,sh,]`** |
| | 550 ms | 784 ms | 34 ms before | saved before the archive; refs kept |
| One of two windows (the app stays) | — (a quiet close, 1.93 s) | 2,191 ms | long before | refs kept |
| | 325 ms | 578 ms | 172 ms after | no late save; refs kept |
| | 400 ms | 694 ms | 56 ms after | **saved 76 ms after the archive: `[sh,sh,]`** |
| | 475 ms | 762 ms | 12 ms before | **the busy close delayed it to 2 ms after the archive: `[sh,sh,]`** |

So a save falls into the gap when it is due between the archive and about 80 ms after it; a later one never fired (the
window, or the whole app, was gone first). Which cases hit moves with the machine's speed, which is why the suite
sweeps the moment.

**The fix** (`TerminalPage.AgentWindowRecord.cpp`; not in a release): the teardown flush is the window's LAST write.
Once `_windowRecordTeardownFlushed` is set, `_FlushWindowRecord` refuses to write and logs `[window-save] <id> late
save refused: …` (how a run tells that a late save existed and was stopped), and `_ScheduleWindowRecordSave` arms
nothing (the archive's own registry notifications are such triggers). Every seam sets the latch only after the user's
final choice (a cancelled close or quit returns before it) and only after its flush, so a window that stays open keeps
saving. **Expected** (and what the two sweeps check): every case keeps the Claude refs and the focused session
(TAB-10 E2, E3).

**Verified** (2026-10-23, run tabk1-verify2): a TEST build of the working tree with the fix (a purged Release build,
packaged as a portable zip stamped 0.8.1.1; never installed on the PC) passed all 9 cases of the two sweeps, the quit
test, `test_tab_menu.py::test_restart_restores_the_renamed_tab_dormant` and `test_tab_lifecycle.py::
test_a_parked_tab_starts_in_place_and_survives_a_restart` (12 of 12). Three cases met the very gap that broke 0.8.0 —
the last window's close with the save due 8 ms after the archive, and the closes of one of two windows with it due
42 and 108 ms after — and each logged `late save refused` (67, 74 and 119 ms after the archive) with the refs intact.

### TAB-K4 · Shift+Click on a parked tab switches to it instead of starting it in place — FIXED in code 2026-10-23, verified on a test build (NOT BUILT into a release)
> **P2** · Found by the e2e suite on 2026-10-23 against 0.8.0 (its known-defect test, replaced by
> `e2e/suites/test_tab_lifecycle.py::test_shift_click_on_a_parked_tab_starts_it_in_place`, `needs_fix`). **The fix:**
> the tab's press handler receives the left press, and the Shift+Click keeps its release from selecting the tab
> (c039d73f1). **Verified** on the test build 0.8.1.1 (a purged Release build of HEAD c0dcc2504 + the working tree at
> 18:25, whose `TerminalApp.dll` holds the fix; never installed on this PC): in the full-suite run `fixall-0811`
> (desktop `am-fixcheck-0811`; finished 2026-10-23 20:53:52) the test passed in 84 s — the parked tab started in place, the
> selection and the keyboard stayed on t2, no `tab-focus`, and a second Shift+Click started nothing. On 0.8.0 the same
> test fails as designed (run `nf-080`: `[nav] tab-focus <sid8>`, no `activate-dormant`). What it was on 0.8.0:

The tab menu names the gesture — "Activate Tab (Shift+Click)", tooltip "You can also Shift+Click the tab to do this" —
but a Shift+Click on a parked (or restored-dormant) tab SELECTS it: `[nav] tab-focus <sid8>`, the page or the tab you
were on is left, and the claude starts as a side effect of the switch; no `[nav] activate-dormant` is logged. Measured
with Shift held 0.08 s, 0.4 s and 1 s around the click. The same Shift+Click on the session's Explorer Tree row or
Triage Board card DOES start it in place (`[nav] activate-dormant`), so the Shift reaches the app.

**Root cause** (read in microsoft-ui-xaml's source on 2026-10-23: WinUI 2.8's `TabViewItem.cpp`, the system XAML's
`ListViewBaseItem_Partial.cpp`, `ListViewBase_Partial_Interaction.cpp`, `Selector_Partial.cpp` and `eventmgr.cpp`):
- `_OnTabPointerPressed` (TabManagement.cpp) was attached with a plain `PointerPressed` handler
  (`tabViewItem.PointerPressed(...)`, upstream's line), and a plain handler is skipped for an event already marked
  handled.
- XAML raises an element's class handler before the handlers added to that element (`CEventManager::Raise`:
  `RaiseControlEvents`, then the element's own handler list). A TabViewItem's class handler is
  `TabViewItem::OnPointerPressed` → `ListViewBaseItem::OnPointerPressed`, which queues the selection tap for a mouse or
  pen LEFT press (`EnqueuePendingTapInteractionForPointerId`) and marks the press handled. So the handler never ran
  for a left press on a tab, and its Shift branch — start the session in place and veto the selection — was dead code
  since it was written. Right and middle presses, which the item leaves unhandled, did reach it: middle-click close
  works through the same handler.
- The same blind spot hid two more of the handler's steps: leaving the Agent Manager page onto the tab MUX holds
  selected under it (a left press on that header), and a left press's clear of the middle-click marker — after a
  middle press released off the tab, the next X click was read as a middle click and refused while "Close tab with
  middle-mouse click" is off.
- The selection itself happens on the RELEASE: `ListViewBaseItem::OnPointerReleased` → `DoTapInteraction` →
  `OnItemPrimaryInteractionGesture` → `OnSelectItemPrimary` focuses the item, then selects it — unless the release
  reaches the item already handled.

**The fix** (TabManagement.cpp, TerminalPage.h; not in a release):
- The handler is registered with handledEventsToo (`AddHandler(PointerPressedEvent, …, true)`), so it receives every
  press. A press that an inner control took — the close button (a ButtonBase) or the rename box (a TextBox) — is told
  apart by walking the press's OriginalSource up to the tab (`ResolveTabPressTarget`) and runs no tab gesture; a middle
  press that arrives handled keeps the old behavior (nothing).
- A Shift+Click on a managed session tab other than the selected one arms a ONE-SHOT PointerReleased handler on the
  tab's template root (`_ArmTabShiftActivateRelease`): every release inside the tab bubbles through it before the
  item's class handler runs, and it marks the press's release handled, so the item skips its tap — no selection, no
  keyboard focus moved onto the tab header, no scroll of the strip. The tab's own Tapped handler (GH#3609) then hands
  the keyboard back to the tab you are on. The armed state (`_tabShiftActivatePress`) is reset by that release and by
  every later tab press, which revokes the handler.
- The one-shot selection veto of the old design (`_suppressTabSelectForActivate`: `_OnTabSelectionChanged` snaps the
  selection back) stays as the fallback for a press whose template root could not be found or armed. Reverting there
  would still scroll the previously selected tab into view (the selection's own ScrollIntoView), which the release
  swallow avoids.
- Logged: `[nav] tab-shift-click <sid8> "<title>": started in place | nothing to start (running, or not startable
  here), no switch (its release will not select it | the selection veto reverts it (fallback) | it is the selected tab
  already)`, beside `_ActivateDormantSession`'s own `[nav] activate-dormant <sid8>`.

**Expected** (and what the replacing test checks): the tab starts in place, the selection and the keyboard stay where
they were (TAB-01 phase 4); a second Shift+Click on the running tab starts nothing and does not switch.

### TAB-K3 · The quit confirmation is invisible to UI Automation — FIXED in code 2026-10-23, verified on a test build (NOT BUILT into a release)
> **P2** · Found by the e2e suite on 2026-10-23 against 0.8.0 (the driver answered it blind, with Enter). **The fix:**
> every ContentDialog the page shows is built in code (8e8480089). Proved by `e2e/suites/test_accessibility.py::
> test_the_quit_confirmation_is_readable` and `…::test_the_large_paste_warning_is_readable` (`needs_fix("TAB-K3")`).
> **Verified** on the test build 0.8.1.1 (a purged Release build whose `src/` equals HEAD 1f4c68678; never installed on
> this PC): both tests pass (runs `fixall-0811` and `fixall-0811b`, 2026-10-23); on 0.8.0 both fail as designed, the
> dialog never found (runs `nf-080`, `nf-080b`). What it was on 0.8.0:

"Do you want to close all windows?" (Close all · Cancel), shown after **Close All Windows**, is on screen, but the
window-level popups UI Automation lists hold only an empty Popup (no title, no buttons): the dialog itself — a Popup
named by its title, with its buttons — is found only by a search of the whole window, nested away from where every
code-built dialog sits, so the driver (and a screen reader looking for the window's dialog) does not find it. Every
Agentmaster ContentDialog, e.g. "Close this window?", is a window-level popup with its title and buttons. Keyboard
works (Enter = Close all; the keyboard focus is on its button). **Expected:** the dialog a window-level popup, found by
its title, with its buttons. A hit test at its "Close all" returns the terminal behind it, but so does a hit test at
EVERY dialog of the app, a code-built one included, on 0.8.0 and on the fix's build alike: a gap of its own, A11Y-K2
(open; `test_accessibility.py::test_known_defect_a_hit_test_passes_through_a_dialog`).

**Root cause** (read in microsoft-ui-xaml's source on 2026-10-23: `dxaml/xcp/dxaml/lib/ContentDialog_Partial.cpp`,
`ShowAsyncImpl`, `HostDialogWithinPopup`, `SetPopupAutomationProperties`; confirmed by the probe below):
- WinUI picks a ContentDialog's placement at its FIRST `ShowAsync` and keeps it. A dialog that is in the LIVE tree is
  shown `TransplantedRootInPopup`: only its `LayoutRoot` moves into a popup, and that popup is itself parented inside
  the dialog's own template (`ContainerPart`), which sits in the page (`TerminalPage.xaml`'s `Root`, row 2). A dialog
  outside the tree is shown `EntireControlInPopup`: the whole control is the child of an unparented popup of the
  island's popup root, named by the dialog's title.
- The quit confirmation (`ConfirmCloseDialog`) was DECLARED in `TerminalPage.xaml` (`x:Load="False"`) and realized by
  `FindName`, so it was in the live tree: transplanted. The empty Popup UI Automation saw at the top is most likely
  its SMOKE layer (an unparented popup of its own, with no content): the probe found one beside EVERY dialog, the
  code-built "Close tab?" included, on both builds (not verified further). Every Agentmaster dialog is built
  in code (`ContentDialog dialog;` … `presenter.ShowDialog(dialog)`), so it was the one dialog of the suite's flows
  that was transplanted.
- Six more dialogs were declared the same way and had the same placement: About, the read-only close warning, the
  multi-line and the large paste warnings, the control notice and the link warning ("This link may lead to an unsafe
  location…").

**The fix** (TerminalPage.cpp, TerminalPage.h, TerminalPage.xaml; not in a release):
- The seven declarations are gone from `TerminalPage.xaml` (an Agentmaster comment there names them). Each dialog is
  built in code when it is shown — the same resource strings (`RS_(L"<Uid>/<Property>")` for what `x:Uid` applied), the
  same buttons and default buttons, the same layout for the two with a body (the multi-line paste preview in a
  ScrollViewer at most 100 px tall; the link and the notice as selectable text with the copy menu) — and shown through
  the window's presenter, which gives a code-built dialog its XamlRoot and theme as before (`TerminalWindow::
  ShowDialog`).
- `_ShowDialogHelper` takes the dialog instead of an `x:Name`; `_ShowMultiLinePasteWarningDialog` takes the clipboard
  preview and empties it once the dialog closed (what the paste handler did to the declared dialog's block); About is
  built once per page and kept (`_aboutDialog`), because its constructor checks for an update.
- The driver (`App.close(how="all")`) now waits up to 4 s to SEE the quit confirmation and presses its **Close all**;
  on 0.8.0 it still answers blind with Enter after that wait.

**The probe** `e2e/suites/probe_dialogs.py` (not collected by a full run: run it by name, `-- -v
suites/probe_dialogs.py`) opens five dialogs — a code-built one ("Close tab?"), the large-paste warning, About, the quit
confirmation from the command palette and after Close All Windows — and records for each the island's popups, 12 hit
tests over the window's middle, the focused element and the title searched in the whole window (its `notes.json` in
the test's evidence folder). Run on 2026-10-23:

| | 0.8.0 (run `nf-080b`) | the test build 0.8.1.1 (run `fixall-0811b`) |
|---|---|---|
| "Close tab?" (built in code on both) | a window-level popup named by its title, its buttons | the same |
| The four declared dialogs | NOT a window-level popup (only the empty Popup is there); a Popup named by the title, with 3 children, found only by the whole-window search | each a window-level popup named by its title, found with its buttons: Paste anyway · Cancel, Send feedback · OK, Close all · Cancel (twice) |
| The keyboard focus | in the dialog (its default button; About: its first link) | the same |
| Hit tests | 60 of 60 on the TermControl behind the dialog | 60 of 60 on the TermControl: A11Y-K2 |

So the cause is confirmed (a declared dialog is shown nested, not as the island's popup) and the fix does what it
should; the hit test is another gap, the same for every dialog (A11Y-K2).

### TAB-K5 · A reopen with a shell tab starts the last Claude tab even when another Claude tab was focused
> **P2** · Found by the e2e suite on 2026-10-23 against 0.8.0 (a probe, run nf-080b), while checking TAB-K2's shape;
> `e2e/suites/test_tab_menu.py::test_known_defect_a_claude_focused_reopen_also_starts_the_last_claude_tab` (passes on
> 0.8.0, run `nf-080c`, and on the test build 0.8.1.1 with every fix of 2026-10-23, run `fixall-0811b`: still there).

The strip at the quit: the shell, t1, t2 (both Waiting), t1 focused. After the reopen the strip reads t1, t2, the
shell, t1 is selected and started — and t2's claude.exe runs too, a Waiting session the restore wake does not start.
Without a shell tab in the window only the focused tab starts. **Cause** (`TerminalPage::_RestoreWindowTabs`, the same
as TAB-K2's): pass 1 opens every Claude ref in record order, each new tab selecting itself; with shells in the record
the focused tab is selected by a `SwitchToTab` at the END of the shells' asynchronous startup batch (the shells would
otherwise take the selection from it); in between the last Claude tab is the selected, SHOWN tab, and a shown terminal
starts. **Expected:** only the focused tab starts (TAB-10's promise, E4). **The user's call:** TAB-K2 — the same
mechanism with a SHELL focused — was ruled a feature on 2026-10-23; if this one is the same feature, it moves to
*Reclassified* and its test becomes the feature's test.

## Reclassified: not defects

### TAB-K2 · A reopen whose focused tab was a shell starts a Claude tab (the last one) — a FEATURE, not a defect
> Filed by the e2e suite on 2026-10-23 against 0.8.0 as a known defect (P1) — "starts the first Claude tab" — and
> **reclassified the same day by the user: "Mark this a feature, not a bug."** TAB-10 E1 expects it;
> `e2e/suites/test_tab_menu.py::test_a_shell_focused_reopen_starts_the_last_claude_tab` proves it (it replaced the
> known-defect test `test_known_defect_a_shell_focused_reopen_starts_the_first_claude_tab`). It passes on 0.8.0 (run
> `nf-080b`) and on the test build 0.8.1.1 (run `fixall-0811b`); a first version that expected the FIRST Claude tab
> failed on both (runs `nf-080`, `fixall-0811`: the reopen logged `[nav] tab-focus <t2>`, and t2's claude ran
> `--resume`, t1's did not).

With a shell focused at the quit, the reopened window selects the shell (`[rehome] … select=… of … refs`), and the
claude.exe of the LAST restored Claude tab starts with it, although a Waiting session is not one the restore wake
(INT-04) starts; every other Claude tab stays dormant. **"First" was a misnomer:** it was measured with the shell and
one Claude tab, which is the first and the last; with the shell, t1 and t2 (run nf-080, 0.8.0, 2026-10-23) the reopen
logged `[nav] tab-focus <t2>` and t2's claude started, t1's did not. **How** (`TerminalPage::_RestoreWindowTabs`): pass
1 opens every Claude ref in record order, each new tab selecting itself; a SHELL's selection is applied by a
`SwitchToTab` at the end of the shells' asynchronous startup batch, after the shells exist; until then the last Claude
tab is the selected, SHOWN tab, and a shown terminal starts.

