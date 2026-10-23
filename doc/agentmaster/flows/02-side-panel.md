# 02 · The fleet side panel (SP)

> Conventions and fixtures: [README.md](README.md). Spec: SIDE_PANEL.md §1–§5; CLAUDE.md *The SIDE PANEL*.

**What shipped.** 0.7.9: a panel that belongs to the window, docked LEFT of the terminal (⇄ flips it): the
session cards (state dot, folder color band, draft dots, timing, tag ribbons, filter box, the board's sort,
per-window LOCAL / GLOBAL) over the summary of the session you are in; card click = peek; ⌖ scrolls to the
selected tab's card; **Sum** hides the summary and shows a rich card beside the panel on hover; **+** opens the
launch row in a card at the click; per-window splitters; « collapses. 0.8.0: the card list is virtualized and
a card costs no composition node; the **+** card opens with its folder list ready; one click on Launch
while its preset menu is open.

## Lifecycle model

```text
 window opens ─► panel (LEFT, LOCAL, Sum on, ⌖ on) seeded from the global defaults (last resize wins)
   │
   ├─ cards: one per session in scope; edited in place on registry changes (≥ 500 ms apart), timing texts every ~30 s
   ├─ summary: FOLLOWS the focused tab's session ──card click──► PEEK (pinned) ──click into the terminal──► FOLLOW
   ├─ + ─► launcher card at the click (folder list open) ─► launch / Esc×2 / outside / + / tab switch ─► closed
   └─ « ─► 20 px strip ─► » ─► panel (updates deferred while collapsed, one catch-up)

 per window (windows/<id>.json): width, split, collapsed, Sum, LOCAL/GLOBAL      global: dock side, ⌖, the defaults
```

---

### SP-01 · The panel shows this window's sessions

> **Since** 0.7.9 · **P0** · **Needs** F-S2, F-W2 · **Spec** SIDE_PANEL.md §1, §2

**Promise.** By default the panel lists the sessions hosted in THIS window, each card showing its state,
folder color, draft dots, timing and tags; GLOBAL widens one window's list to every window.

**Phases**
1. **Default.** *Do:* F-W2 (window 1: A, B; window 2: C). *Expect:* window 1's panel: A and B; count line `2
   of 3 sessions in this window`; window 2: C.
2. **Card content.** *Expect per card:* the title on a band in the session's folder color (black or white text
   by contrast), the state dot + word, the timing line `created …, active … ago` (ends in `…` on a narrow
   panel), `leaf / branch`, tag ribbons in the band's top-right corner, three dots while a draft exists.
3. **State follows.** *Do:* run a turn in A. *Expect:* A's dot and word follow STA-01 within a scan tick.
4. **GLOBAL.** *Do:* window 1's LOCAL → GLOBAL. *Expect:* A, B, C; count `… across all windows`; window 2
   stays LOCAL (no leak).
5. **Restart.** *Expect:* window 1 reopens GLOBAL, window 2 LOCAL; a NEW window starts LOCAL.

**Edge cases**
- **E1 A tab docked into / torn out of the window** (no registry change) → the LOCAL list follows within a
  tick.
- **E2 Codex session** → a teal `codex` pill.
- **E3 Collapsed** → no updates while collapsed; one catch-up on expand.
- **E4 Filter box** → `ParseSessionQuery` grammar: AND terms, `"quoted phrase"`, a whole session id
  matches the session and its forks.

---

### SP-02 · Following, peeking and jumping

> **Since** 0.7.9 · **P0** · **Needs** F-S2 · **Spec** SIDE_PANEL.md §2, §3

**Promise.** The summary shows the session you are working in; clicking a card peeks at another until you
click back into the terminal; double-click jumps to a card's tab.

**Phases**
1. **Follow.** *Do:* select tab A. *Expect:* A's card highlighted, the summary shows A.
2. **Peek.** *Do:* click B's card. *Expect:* the summary shows B; tab A stays selected; the tab strip does
   NOT move.
3. **Back to work.** *Do:* click into A's terminal. *Expect:* the summary snaps back to A.
4. **Jump.** *Do:* double-click B's card. *Expect:* tab B selected (and scrolled into view in the strip).
5. **Context menu.** *Do:* right-click a card. *Expect:* the board card's session menu, anchored on the card;
   its **Tags** opens the tag panel right under the card.

**Edge cases**
- **E1 On the Agent Manager page** a card click also scrolls the tab strip to that session's tab (even a
  re-click of the selected card).
- **E2 A shell tab focused** → nothing to follow; the current peek is kept.
- **E3 A session in another window** (GLOBAL) → double-click brings that window to the front.

---

### SP-03 · ⌖ keeps the selected tab's card in view

> **Since** 0.7.9 · **P1** · **Needs** 40+ sessions in one window (mock) · **Spec** SIDE_PANEL.md §2a, §2f

**Phases**
1. **Far card.** *Do:* 40 sessions; select a tab whose card is far below the view. *Expect:* the list jumps
   (unanimated) so that card is in view; the rows around it exist (virtualized list).
2. **Toggle off.** *Do:* click ⌖ (global, broadcast to every window). *Expect:* selecting tabs no longer
   scrolls the list.
3. **A new session** (launch) → its card starts at the TOP (MOST ACTIVE / MOST RECENT), so ⌖ does not drag
   the list to the bottom.

---

### SP-04 · Sum off: cards fill the panel, a rich card on hover

> **Since** 0.7.9 · **P1** · **Needs** F-S2 · **Spec** SIDE_PANEL.md §2d

**Phases**
1. **Sum off.** *Do:* click **Sum**. *Expect:* the summary area and its splitter disappear; the cards fill
   the panel.
2. **Hover.** *Do:* rest the pointer on a card. *Expect:* the session's rich card (the tab header's hover
   card) opens BESIDE the panel (right of it when docked left), vertically centered on the card; its summary
   wheel-scrolls. Not observed in the test desktop on 0.8.0 (SP-K2).
3. **Leave.** *Expect:* the card closes; it never covers the panel's own controls.
4. **Persist.** *Expect:* per window; the last change seeds new windows.

**Edge cases**
- **E1 Docked right** → the rich card opens left of the panel.
- **E2 Tag ribbons** on a card are a tooltip-free zone: hovering them opens the tag hover panel, not the
  rich card.

---

### SP-05 · The + launcher card

> **Since** 0.7.9; folder list ready + one-click Launch 0.8.0 · **P0** · **Needs** mock, keys · **Spec** SIDE_PANEL.md §2e

**Promise.** **+** opens the Agent Manager's launch row in a card at the click, with the caret in the
path box and its folder list open right under it; a launch, Esc twice, a click outside, + again or a tab
switch closes it; no keystroke leaks into the terminal.

**Phases**
1. **Open.** *Do:* click **+** (panel docked left). *Expect:* a card whose corner sits at the click,
   extending toward the window's center, clamped 8 DIP inside the window: `● Claude [ box ] [Launch
   Claude]`; the box shows the Agent Manager box's text; the caret in the box; the folder list (RECENT,
   SUBFOLDERS, Browse…) open right UNDER the box — never over it or Launch.
2. **Edit mirrors.** *Do:* type a folder path. *Expect:* the Agent Manager page's box shows the same text;
   the validation underline (neutral / amber "Create & Launch Claude" / red).
3. **Esc.** *Do:* Esc once. *Expect:* the folder list closes, the card stays. Esc again → the card closes.
   *Check:* the terminal received no ESC (Claude did not interrupt / no stray input).
4. **Launch.** *Do:* + again, pick a recent folder, click **Launch Claude** once. *Expect:* one new session
   (a tab), the card closes. *Check:* [ENG] exactly one new session.

**Edge cases**
- **E1 A session id typed** in the box (found on disk) → green underline, **Resume session** + **Fork**.
- **E2 Launch's preset hover menu open** (pointer resting on Launch) → ONE click launches (not a
  double-click); a double-click launches once.
- **E3 Enter** → in the BOX it only commits the path (the folder list closes, nothing launches — the Agent
  Manager page's Enter); on the focused **Launch** button it launches, on the KEY-UP, so no CR reaches the terminal
  (`TerminalPage.AgentSidePanel.cpp`, the card's PreviewKeyDown/Up). Corrected 2026-10-23 by the e2e suite: this
  line first said Enter in the box launches.
- **E4 Card near the window's right/bottom edge** → opens leftward/upward.
- **E5 Window resized while open** → re-placed on the next tick; the list re-anchors.
- **E6 A tab switch / collapse / dock flip** → closes the card.
- **E7 UI Automation invoke** of + (no pointer press) → anchors under the button.
- **E8 Codex** → the chip is Claude-only (tooltip says Codex launches are planned).

---

### SP-06 · Size, dock and collapse are remembered per window

> **Since** 0.7.9 · **P1** · **Needs** F-W2, F-RS · **Spec** SIDE_PANEL.md §4

**Phases**
1. **Drag.** *Do:* drag the vertical splitter (3 px) to widen the panel; drag the horizontal splitter.
   *Expect:* clamped (width 10–50% of the window, split 15–85%); the splitter tooltips never show during a
   drag (they open only after 7 s of rest).
2. **New window.** *Expect:* a new window uses the LAST resize as its default; the first window keeps its
   own.
3. **Collapse.** *Do:* «. *Expect:* a 20 px strip; » restores; per window, last change seeds new windows.
4. **Dock.** *Do:* ⇄. *Expect:* the panel moves to the right of the terminal in EVERY window (global); the
   chevrons flip.
5. **Restart.** *Expect:* everything as left, per window.

**Edge cases**
- **E1 A corrupt record value** (width 0.9 in a window record) → falls back to the global default.
- **E2 Button tooltips** (+, Sum, LOCAL/GLOBAL, sort, ⌖, ⇄, «) → open only after 3 s of rest.

---

### SP-07 · Hundreds of sessions stay cheap

> **Since** 0.8.0 · **P2** · **Needs** 300 sessions (mock; F-CORP-like fleet), the cpuhunt probe · **Spec** SIDE_PANEL.md §2f; UI_THREAD.md §13–§14

**Phases**
1. **Fleet.** 300 sessions in one window, GLOBAL panel.
2. **Scroll.** *Expect:* the scroll extent matches `count × card height`; cards appear as they scroll in;
   no blank gaps; ⌖ reveals far cards.
3. **Idle cost.** *Expect:* the UI thread's busy share stays low at rest (`tools/cpuhunt` threshold to be
   set from the first run), and the composition node count does NOT grow with the number of cards:
   measure it (`tools/leakhunt/xamltree.py`) at 100 and at 300 sessions — the two should differ by
   about one viewport's worth of realized cards, not by 200 cards' nodes. (Production's reference:
   ~700 tabs in two GLOBAL panels walked 3,634 nodes before the fix, 81% of them cards; UI_THREAD.md §13.)

---

## Known defects (expected failures until fixed)

### SP-K1 · In the + card, Launch needs a second click while its presets menu is open
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (`e2e/suites/test_side_panel.py`
> `test_known_defect_plus_card_launch_needs_a_second_click`)

With the pointer resting on the + card's **Launch Claude**, its presets hover menu opens (Default · Specify… · the
launch models · Add presets…); one click — a held one, like a hand's — only closes that menu, the card stays open,
and nothing launches; a second click launches. The Agent Manager page's own launch row launches with ONE click in the
same situation (0.8.0's fix, CLAUDE.md Gotchas *A light-dismiss flyout eats the press that dismisses it*), so the
fix does not reach the + card (its Launch sits inside a Popup). **Repro:** + → type a folder → rest on Launch until
the menu shows → click once. **Expected:** one session launched, the card closed (SP-05 E2).

**Related, measured the same day (not a defect for a person):** a press and release delivered together (an
automation's zero-length click) is lost on the page's Launch too, because the fix reads the button's LIVE state
(`GetAsyncKeyState`) when the menu closes and finds it already released. A fast tap on a busy UI thread could meet
the same race.

### SP-K2 · With Sum off, a card's hover opens no rich card
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (`e2e/suites/test_side_panel.py`
> `test_known_defect_sum_off_card_hover_opens_no_rich_card`)

**Sum** off (hooks.log: `[nav] sidepanel summary -> hidden (cards show the rich session tooltip)`), then the pointer
glides onto a card and rests: no tooltip opens in 9 s (every popup window of the app polled every 0.3 s). The same
glide onto the session's TAB opens its rich card at once (the test's control half), so the input reaches XAML's
tooltip service. **Not yet known:** whether the card's `AgentSessionToolTipHost` is never attached in the virtualized
list (`_SidePanelArmCardToolTip` runs from `_SidePanelApplyCardTipMode`), or attached and then closed by the card's
suppressed floating tip. **Expected:** the rich card beside the panel (SP-04 phase 2).
