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
4. **Activate without switching.** *Do:* park again; tab menu → **Activate Tab** (or Shift+click the tab).
   *Expect:* it starts in place without being selected.

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

**Edge cases**
- **E1 Gates:** Tabs to the Left grayed on the left-most tab, Tabs to the Right on the right-most, Other Tabs
  on a window's only tab; an enabled item can still park nothing (`parked=0`).
- **E2 A running turn** in a folder-scoped tab of window 2 → cut off without asking (the tooltip warns "in
  EVERY window, without asking first").
- **E3 Shell tabs in scope** → untouched (only managed sessions park).

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
   2). *Expect:* ONE confirm listing their titles (≤ 20 + "…and N more"); accept → all three closed
   (window 2's through its own window).
5. **Other of Same Folder** → the same minus this one.

**Edge cases**
- **E1 Cancel** → nothing closes.
- **E2 Window ✕ with managed tabs** → one batch dialog **Close All · ★ Favorite & Close All · Cancel All**.
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
