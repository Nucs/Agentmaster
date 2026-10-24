# 02 · The fleet side panel (SP)

> Conventions and fixtures: [README.md](README.md). Spec: SIDE_PANEL.md §1–§5; CLAUDE.md *The SIDE PANEL*.

**What shipped.** 0.7.9: a panel that belongs to the window, docked LEFT of the terminal (⇄ flips it): the
session cards (state dot, folder color band, draft dots, timing, tag ribbons, filter box, the board's sort,
per-window LOCAL / GLOBAL) over the summary of the session you are in; card click = peek; ⌖ scrolls to the
selected tab's card; **Sum** hides the summary so the cards fill the panel (a card's hover then shows no rich card —
a feature, SP-K2); **+** opens the
launch row in a card at the click; per-window splitters; « collapses. 0.8.0: the card list is virtualized and
a card costs no composition node; the **+** card opens with its folder list ready; one click on Launch
while its preset menu is open. After 0.8.0 (NOT BUILT): each card's dot is the tab strip's status icon — the
draft's three dots under it, the red unread ring, the cache-warm spark crown (SP-08).

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
   panel), `leaf / branch`, tag ribbons in the band's top-right corner, three dots while a draft exists (since
   SP-08, NOT BUILT: under the dot, in the tab's status icon).
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
6. **Deactivate ▸ on a card** (2026-10-23, NOT BUILT). *Do:* right-click B's card (B running, in this window,
   neither the left-most nor the right-most tab). *Expect:* **Deactivate ▸** directly above **Close ▸**, as in
   the tab menu, holding Deactivate Tab · Tabs to the Left · Tabs to the Right · Other Tabs · Of Same Folder ·
   Other of Same Folder · All Tabs · Deactivate All Idle Tabs (no glyphs; the last item since 2026-10-24);
   **Deactivate Tab** parks B with no confirm, its tab kept, exactly like the tab menu's item. *Check:* [LOG]
   `[nav] session-menu deactivate this <B8>`, then `[nav] deactivate-begin scope=this` / `deactivate-done …
   parked=1`.
7. **Close ▸ lines up with it.** *Do:* open B's card menu's Close ▸. *Expect:* Close Tab · Tabs to the Left ·
   Tabs to the Right · Other Tabs · Of Same Folder · Other of Same Folder · All Tabs · ★ Favorite & Close All
   Tabs; **Tabs to the Right** asks once ("Close N tabs?") and closes only the tabs right of B's tab.
   *Check:* [LOG] `[nav] session-menu close right <B8>`, then `close-all begin`.
8. **A card menu's action keeps the card and the list** (2026-10-24, NOT BUILT; SP-K3's fix). *Do:* 20+ sessions
   in this window, tab A selected (MOST ACTIVE: A's card at the top); scroll the list down; right-click the 5th
   visible card B (Waiting for you) → **Move to Idle/Done**. *Expect:* B's card stays selected and its state turns
   Idle; the list does not scroll; the summary shows B. *Do:* click into A's terminal. *Expect:* the peek ends: A's
   card selected and, with ⌖ on, scrolled into view. *Check:* [LOG] `[sidepanel] focus came back to the terminal
   from a menu, a dialog or an overlay: the selection stays on <B8> (the focused tab shows <A8>; …)`.

**Edge cases**
- **E1 On the Agent Manager page** a card click also scrolls the tab strip to that session's tab (even a
  re-click of the selected card).
- **E2 A shell tab focused** → nothing to follow; the current peek is kept.
- **E3 A session in another window** (GLOBAL) → double-click brings that window to the front.
- **E4 The tab menu's gates** (phases 6–7) → on the card of the window's left-most tab, Tabs to the Left is
  grayed in both submenus; right-most → Tabs to the Right; a window's only tab → Other Tabs; a dormant
  session → Deactivate Tab grayed (Activate Tab leads the menu); an adopted session → no Deactivate ▸.
- **E5 Another window's session** (GLOBAL, phases 6–7) → no strip item grayed. Deactivate ▸ Tabs to the
  Right parks the tabs right of that session's tab IN ITS WINDOW, which stays behind; Close ▸ Close Tab
  brings that window to the front and asks there. *Check:* [LOG] `-> handed to the window that hosts its
  tab`, then that window's `[nav] session-menu-remote …` and the tab menu's own lines.
- **E6 A session whose agent exited and that no window hosts** → Close ▸ Close Tab archives it in place
  (`… archived in place`); it leaves the list and stays in Sessions.
- **E7 Deactivate All Idle Tabs on a card** (2026-10-24, NOT BUILT) → the tab menu's item (flows/07 TAB-02
  phase 7) run from the card: the Idle sessions of the card's window park, every other one keeps running;
  grayed while that window holds no Idle session to park; a card of ANOTHER window's session leaves it
  enabled and that window parks its own Idle tabs. *Check:* [LOG] `[nav] session-menu deactivate idle <B8>`,
  then `[nav] deactivate-done scope=idle parked=N`.
- **E8 The card menu closed with Esc, by any other item (Copy, Mark Unread, Favorite), or by a click outside it**
  (phase 8, NOT BUILT) → the same: B stays selected and the list stays. A click that only closed the menu (XAML
  gives it to no element) does not end the peek; the next click into the terminal does.
- **E9 Show Transcript from B's card menu, then Esc** (phase 8, NOT BUILT) → when the reading view closes, B is still
  selected and the list has not moved.

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

### SP-04 · Sum off: cards fill the panel, and a card's hover opens no rich card

> **Since** 0.7.9 · **P1** · **Needs** F-S2 · **Spec** SIDE_PANEL.md §2d

**Phases**
1. **Sum off.** *Do:* click **Sum**. *Expect:* the summary area and its splitter disappear; the cards fill
   the panel.
2. **Hover.** *Do:* rest the pointer on a card. *Expect:* NO rich card opens (a feature, the user's ruling of
   2026-10-24: SP-K2, *Reclassified* below); the same hover on the session's TAB still opens the tab's rich card.
   (Until the ruling this phase expected the rich card beside the panel, SIDE_PANEL.md §2d; the test desktop never
   showed it, on 0.8.0 or 0.8.1.1.)
3. **Leave.** *Expect:* nothing to close; the card and the panel's own controls stay as they were.
4. **Persist.** *Expect:* per window; the last change seeds new windows.

**Edge cases**
- **E2 Tag ribbons** on a card are a tooltip-free zone: hovering them opens the tag hover panel. (E1, "docked right:
  the rich card opens left of the panel", went with phase 2's ruling.)

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
5. **One click with the presets menu open** (SP-K1's fix, 2026-10-23). *Do:* + over a tab, type a folder, rest on
   **Launch Claude** until its presets menu shows, click ONCE. *Expect:* one new session in front, the card closed.
   *Check:* [LOG] `[launchbar] Launch clicked while its hover menu was up: …` (`its release on Launch completed the
   click` or `the release watch completed the click`); no `a second Launch click … was ignored`. Over the PAGE the
   launch stays behind it (MGR-07 phase 5).

**Edge cases**
- **E1 A session id typed** in the box (found on disk) → green underline, **Resume session** + **Fork**.
- **E2 Launch's preset hover menu open** (pointer resting on Launch) → ONE click launches (not a
  double-click); a double-click launches once. (On 0.8.0 the + card needed two clicks: SP-K1, phase 5.)
- **E2b The + card over the Agent Manager page** → the launch keeps the page and selects the new session there
  (MGR-07 phase 5); over a tab it opens in front.
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

### SP-08 · A card's status icon is the tab's

> **Since** after 0.8.0 (NOT BUILT) · **P1** · **Needs** F-S2, F-W2, mock, keys · **Spec** SIDE_PANEL.md §2g

**Promise.** Every card's state dot is the tab strip's own status icon, showing what the tab shows at the same
time: three dots under it while the session's input box holds an unsent draft, the red ring blinking around it
while the tab is unread (in step with the tab's ring, in every window's list), and the spark crown (an amber glow and
rising embers) while the server cache is warm.

**Phases**
1. **The dot.** *Expect:* each card's dot is the tab's: 10 DIP with a thin black outline, its state word beside it
   on the same line, the timing text on that line too.
2. **Draft.** *Do:* type into session A's box without sending. *Expect:* within a scan tick, three dots under A's card
   dot AND under A's tab dot, both pulsing. *Do:* send it. *Expect:* both clear.
3. **Unread.** *Do:* A on a background tab; let a turn end. *Expect:* A's tab and A's card both show the red ring,
   blinking together. *Do:* select tab A. *Expect:* both rings stop at once. *Check:* [LOG] `[unread] 0 -> 1`, then
   `1 -> 0`.
4. **Mark Unread.** *Do:* A's tab menu → Mark Unread. *Expect:* the card rings too, until the tab is visited.
5. **Cache warm.** *Do:* let a turn end. *Expect:* A's card shows the glow and the embers while A's tab does; both go
   out on the same tick once the cache lifetime (`serverCacheMinutes`, 5 by default — shorten it in the cog) has
   passed since the last API turn.
6. **GLOBAL.** *Do:* window 1's panel GLOBAL; a draft, a ring and a warm cache on a session of window 2. *Expect:*
   window 1's card for it shows all three, the ring blinking with window 2's tab.

**Edge cases**
- **E1 A dormant tab with a remembered draft** (restored, never entered) → still dots on its card, as on its tab; the
  card keeps a full dot (the tab's half-hollow dot is not carried over).
- **E2 Windows "Animation effects" off** → still dots and a steady glow without embers on cards AND tabs; the ring
  still blinks.
- **E3 Collapsed panel** → nothing moves while collapsed; on expand the indicators show their current state and move.
- **E4 The cog's "status flashing color" changed** → the cards' rings take the new color at once (one shared brush
  per window, the tabs' own).
- **E5 Cards scrolled out of view and back** (the virtualized list) → a card shows the current indicators within a
  tick; no ring left on a session whose tab was visited meanwhile.
- **E6 A Codex session** → no draft dots and no crown (both are Claude-only, as on its tab); its ring follows its tab's.

---

## Known defects (expected failures until fixed)

### SP-K1 · In the + card, Launch needs a second click while its presets menu is open — FIXED in code 2026-10-23 (NOT BUILT into a release)
> **P1** · Found by the e2e suite on 2026-10-23 against 0.8.0 (its known-defect test, replaced by
> `e2e/suites/test_side_panel.py::test_plus_card_launch_takes_one_click_with_its_menu_open`, SP-05 phase 5).
> **The fix** (`AgentLaunchBar`, the bar both hosts share): the press that closes the menu is also recognized from the
> pointer MESSAGE being processed (`GetPointerInfo` — a busy UI thread may process the press after the physical
> release), and a release WATCH completes an armed press whose release never reached Launch: it polls the physical
> button and, once it is up, judges the press and release points with the last pointer position seen on Launch.
> Every arm, completion and decline is a `[launchbar]` line in hooks.log, so a click that still fails says why.
> **The cause, measured with the test build on 2026-10-23** (e2e run fix1): the press is armed in both hosts, but in the
> card the release never reaches Launch — `… the menu took the press and no release reached Launch; the release watch
> completed the click` — while on the page it does (`… its release on Launch completed the click`). What it was on
> 0.8.0:

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

### SP-K3 · A card menu's action selects the focused tab's card and jumps the list to it — FIXED in code 2026-10-24 (NOT BUILT into a release)
> **P1** · Reported by the user on 2026-10-24: "if I right-click -> Move to Idle/Done then it should not select index
> 0 (top item) and have the scroll jump to the top while the click was in a 5th or whatever index card in the list".
> Read from the code and from WinUI's source; not yet reproduced by the e2e suite (SP-02 phase 8 and E8–E9 are its
> checks). Present since the side panel's follow rule (0.7.9).
> **The cause:** the card's context menu takes keyboard focus when it opens, and when it closes WinUI gives focus back
> to the element that held it, the terminal (`CPopup::Close`). The terminal's `GotFocus` was one of the side panel's
> two "back to work" signals (SIDE_PANEL.md §3), so every item of the menu, and its Esc, ended the peek of the card
> the menu was opened on. The focused tab's card was selected instead and, with ⌖ on (the default), the list scrolled
> to it, usually the top card (MOST ACTIVE / MOST RECENT put the session you work in first).
> **The fix** (`TerminalPage.AgentSidePanel.cpp`, `SidePanelFocusArrivalEndsPeek`): the `GotFocus` follows only
> focus that comes from the terminal column itself, the side panel's own controls or the window's activation. Focus
> handed back by a menu, a flyout, a dialog, a popup or a page overlay keeps the peek, and so does focus from the title
> bar's tab strip. A click into the terminal still ends a peek. What it was on 0.8.0:

Right-click any card below the focused tab's card → **Move to Idle/Done** (or any other item, or Esc): the card the
menu was about is deselected, the focused tab's card is selected, and with ⌖ on the list jumps to that card. The
same happens when **Show Transcript**'s reading view closes. **Repro:** 10+ sessions, the focused tab's card at the
top, the list scrolled down, right-click a lower card → Move to Idle/Done. **Expected:** SP-02 phase 8.

## Reclassified: not defects

### SP-K2 · With Sum off, a card's hover opens no rich card — a FEATURE, not a defect
> Filed by the e2e suite on 2026-10-23 against 0.8.0 as a known defect (P1), and **reclassified by the user on
> 2026-10-24: "Mark this as a feature and not a problem."** SP-04 phase 2 expects it;
> `e2e/suites/test_side_panel.py::test_sum_off_card_hover_opens_no_rich_card` proves it (it replaced the known-defect
> test `test_known_defect_sum_off_card_hover_opens_no_rich_card`, which passed on 0.8.0 — run g1full — and on the test
> build 0.8.1.1 — run fixall-0811).

**Sum** off (hooks.log: `[nav] sidepanel summary -> hidden (cards show the rich session tooltip)`), then the pointer
glides onto a card and rests: no tooltip opens (in the first measurement, 9 s with every popup window of the app polled
every 0.3 s). The same glide onto the session's TAB opens its rich card at once (the test's control half), so the input
reaches XAML's tooltip service. What the documents and the code still say: SIDE_PANEL.md §2d describes the rich card
beside the panel, the code keeps its hover host for such cards (`_SidePanelArmCardToolTip`, run from
`_SidePanelApplyCardTipMode`), and the log line above still names it. Why that card never opens was not looked into: it
is not wanted. Whether to remove the unused host and reword the line is the owner's call.
