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
draft's three dots under it, the red unread ring, the cache-warm spark crown (SP-08). After 0.8.x (2026-11-03; in dev
0.0.1.42 and production 0.8.0.9): the header line is **+** · **Sum** · **⋯**, and the ⋯ menu holds the dock, ⌖, the
card scope, the card order and the collapse that were buttons on it (SP-10). 2026-11-04 (NOT BUILT): the line mirrors
with the dock — **⋯** · **Sum** · **+** from the window's left edge docked left, ⋯ always at the window's edge — the
menu hangs from ⋯'s outer edge, and the dock item sits below the separator, just above the collapse (SP-10).
2026-11-06 (NOT BUILT): a card's menu pins it to the top of the list, right under Jump to Tab (SP-11); the filter box's
tip waits 3 s like the buttons'. 2026-11-07 (NOT BUILT): a card's Open New Session Here and Fork session open the new tab
right after the card's tab, in that tab's window (SP-12).

## Lifecycle model

```text
 window opens ─► panel (LEFT, LOCAL, Sum on, ⌖ on) seeded from the global defaults (last resize wins)
   │
   ├─ cards: one per session in scope; edited in place on registry changes (≥ 500 ms apart), timing texts every ~30 s
   ├─ summary: FOLLOWS the focused tab's session ──card click──► PEEK (pinned) ──click into the terminal──► FOLLOW
   ├─ + ─► launcher card at the click (folder list open) ─► launch / Esc×2 / outside / + / tab switch ─► closed
   └─ ⋯ ▸ Collapse (« on 0.8.x) ─► 20 px strip ─► » ─► panel (updates deferred while collapsed, one catch-up)
        (2026-11-12: the strip floats over the terminal's edge, hidden; the pointer within 40 DIPs of it shows it — SP-16)

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
4. **GLOBAL.** *Do:* window 1's ⋯ ▸ Card scope ▸ GLOBAL (its LOCAL → GLOBAL button on 0.8.x). *Expect:* A, B, C;
   count `… across all windows`; window 2 stays LOCAL (no leak).
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
   Other of Same Folder · All Tabs · Deactivate All Idle Tabs · Deactivate All Waiting-for-you (no glyphs; the
   idle item since 2026-10-24, the Waiting-for-you item — the last one — since 2026-11-04, NOT BUILT);
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
   Idle; the list does not scroll; the summary shows B. *Do:* right-click B's card again and close the menu WITHOUT a
   choice (Esc; again, a click on the panel's count line). *Expect:* the same each time: B stays selected, the list
   stays. *Do:* click into A's terminal. *Expect:* the peek ends: A's card selected and, with ⌖ on, scrolled into
   view. *Check:* [LOG] after each close, `[sidepanel] focus came back to the terminal from <origin>, not by a click
   into it: the selection stays on <B8> (the focused tab shows <A8>; …)` — `<origin>` says how the menu handed
   focus back (`nowhere (…)` as read in WinUI's source, or the closed menu's own element); `e2e/suites/
   test_side_panel.py::test_card_menu_keeps_the_peek` (`needs_fix("SP-K3")`).

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
  phase 7) run from the card, in the window whose side panel or page the card is on — whichever window hosts the
  card's session (a card of ANOTHER window's session no longer hands the item to that window). Its Idle sessions
  park, every other one keeps running; when other windows hold Idle tabs too, it asks first in this window
  ("Deactivate idle tabs": This window (N) · All windows (M) · Cancel — flows/07 TAB-02 phase 8); grayed while no
  window holds an Idle tab to park. *Check:* [LOG] `[nav] session-menu deactivate idle <B8>`, then
  `[nav] deactivate-idle-ask from=session menu here=N elsewhere=M` and `[nav] deactivate-done scope=idle parked=N`.
- **E8 The card menu closed with Esc, by any other item (Copy, Mark Unread, Favorite), or by a click outside it**
  (phase 8, NOT BUILT) → the same: B stays selected and the list stays. A click that only closed the menu (XAML
  gives it to no element) does not end the peek — even one on A's terminal; the next click into the terminal does.
- **E9 Show Transcript from B's card menu, then Esc** (phase 8, NOT BUILT) → when the reading view closes, B is still
  selected and the list has not moved.
- **E10 The window comes back to the foreground** (Alt+Tab away and back while peeking B; NOT BUILT) → B stays
  selected: returning to the window is not a touch of the terminal (since the second version of SP-K3's fix; the
  first version, a0e892b90, followed there).
- **E11 Deactivate All Waiting-for-you on a card** (2026-11-04, NOT BUILT; the item after Deactivate All Idle
  Tabs) → the tab menu's item (flows/07 TAB-02 phase 9) run from the card, by E7's rules: in the window whose side
  panel or page the card is on, whichever window hosts the card's session. Its Waiting-for-you sessions park and
  keep that state (the goldenrod dot, the unread ring), every other one keeps running; when other windows hold
  Waiting-for-you tabs too, it asks first in this window ("Deactivate Waiting-for-you tabs": This window (N) · All
  windows (M) · Cancel — flows/07 TAB-02 phase 10); grayed while no window holds one to park, on its own gate (the
  idle item can be enabled while it is grayed). *Check:* [LOG] `[nav] session-menu deactivate waiting <B8>`, then
  `[nav] deactivate-waiting-ask from=session menu here=N elsewhere=M` and `[nav] deactivate-done scope=waiting
  parked=N`.

---

### SP-03 · ⌖ keeps the selected tab's card in view

> **Since** 0.7.9 · **P1** · **Needs** 40+ sessions in one window (mock) · **Spec** SIDE_PANEL.md §2a, §2f

**Phases**
1. **Far card.** *Do:* 40 sessions; select a tab whose card is far below the view. *Expect:* the list jumps
   (unanimated) so that card is in view; the rows around it exist (virtualized list).
2. **Toggle off.** *Do:* ⋯ ▸ Scroll to the selected tab (the ⌖ button on 0.8.x; global, broadcast to every
   window). *Expect:* selecting tabs no longer scrolls the list.
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
3. **Collapse.** *Do:* ⋯ ▸ Collapse the side panel (« on 0.8.x). *Expect:* a 20 px strip; » restores; per window,
   last change seeds new windows. On a 2026-11-12 build the strip floats over the terminal's edge and hides itself
   (SP-16): » is still found by its id and its real click restores.
4. **Dock.** *Do:* ⋯ ▸ Dock on the right (⇄ on 0.8.x). *Expect:* the panel moves to the right of the terminal in
   EVERY window (global); the strip's chevron and the menu's collapse symbol flip; on a 2026-11-04 build the header
   line mirrors too (`⋯ Sum +` → `+ Sum ⋯`, ⋯ at the window's edge — SP-10).
5. **Restart.** *Expect:* everything as left, per window.

**Edge cases**
- **E1 A corrupt record value** (width 0.9 in a window record) → falls back to the global default.
- **E2 Button tooltips** (+, Sum, ⋯ and every item of the ⋯ menu; on 0.8.x +, Sum, LOCAL/GLOBAL, sort, ⌖, ⇄, «) →
  open only after 3 s of rest.

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
- **E7 The ring's size** (2026-11-08, NOT BUILT) → at 100%, a card's ring is 14 px with 2 px of red on every side of
  its dot; its tab's 13 px ring is unchanged (1 px left and above, 2 px right and below). Only the card's ring changed,
  by the user's call. Expected elsewhere (a replica of XAML's layout math, SIDE_PANEL.md §2g *The ring's size*): at
  125% the card's ring is 17 px with 2 px all round, at 150% 21 px with 3 px all round.

---

### SP-09 · The AI search in the side panel

> **Since** 2026-10-31 (in dev 0.0.1.42 and production 0.8.0.9; the loading icon and the end-of-request list 2026-11-05,
> NOT BUILT) · **P1** · **Needs** F-S2, mock (the AI search's model), keys · **Spec** SIDE_PANEL.md §2h; SESSIONS.md §9.6

**Promise.** The filter box's **AI** toggle turns the box into the AI search's request line and shows the model's answer and
its list in place of the cards, as side panel cards with the model's evidence under each — the Sessions page's conversation,
not a second one. Its claude.exe starts only when the box is clicked or a request is sent, and ends after the idle time set
in Settings ▸ Sessions ▸ AI SEARCH (2 hours by default).

**Phases**
1. **Toggle on.** *Do:* type a filter, then click **AI** inside the box. *Expect:* the text stays (now the request), the caret
   after it; the card area shows the strip over the conversation's last list (or an empty one); the count line reads the AI
   hint. *Check:* [LOG] `[nav] sidepanel ai on box=N`, then `[harness] sessions-search: process up` (the caret's focus).
2. **Ask.** *Do:* Enter. *Expect:* the box empties; the strip shows "❯ <request>", a spinning loading icon where the dot
   was (`am.side.ai.busy`), the plan and "Searching …" in blue ("… — N candidates, choosing…" once a search returned
   rows); the count line "AI search — searching…"; the list does NOT change while the request runs (the last answer's
   cards stay, dimmed; no candidate is drawn). At the end the pick replaces the list in one step, each card with its why;
   the answer in the strip, the green dot instead of the spinner. NOT BUILT (2026-11-05): a build of 2026-11-04 or older
   shows the candidates at once and the pick a second later, with no spinner. *Check:* [LOG] `sidepanel ai-ask turn=1
   chars=N`; then `[ai-search] side panel (window …): the idle process parks in 120 min (its request was answered)`.
3. **Results.** *Expect:* an open session's card shows its live state; a closed one reads **closed** in gray; the summary area
   below still follows the focused tab.
4. **Gestures.** *Do:* click an open session's card. *Expect:* the summary peeks it; the Manager lens follows. *Do:*
   double-click it. *Expect:* its tab comes up. *Do:* double-click a closed one. *Expect:* it resumes as a new tab in this
   window. *Do:* right-click a closed one. *Expect:* Resume here · Fork here ▸ · Open New Session Here ▸ · Show Transcript ·
   Copy ▸.
5. **Shared conversation.** *Do:* open the Sessions page, turn its AI on. *Expect:* its list is the side panel's; a follow-up
   asked on the page replaces the side panel's list too.
6. **Toggle off.** *Do:* type text without Enter, click **AI**. *Expect:* the text stays and filters the cards at once; the
   cards are current (a session launched meanwhile is listed); ⌖ scrolls to the focused tab's card. *Check:* [LOG]
   `[nav] sidepanel ai off box=N`.
7. **Persist.** *Do:* close and reopen the window with AI on. *Expect:* it opens in AI mode on the last list and starts no
   claude.exe (no `process up` line until a click into the box).

**Edge cases**
- **E1 Esc during a request** → it stops (`sidepanel ai-stop`); Esc with nothing running does nothing.
- **E2 ⊘** → the conversation resets here and on the Sessions page; the box keeps its text (`ai-reset box=N refill=0`).
- **E3 A Sessions page shows the AI search** → no side-panel park line after a request (the page's rule holds the process).
- **E4 A session Agentmaster never opened** → its card's click opens its transcript reading view (no summary record exists).
- **E5 A restored list of unknown sessions** → their cards read "session <id8> — reading…" and fill in from disk within a
  moment.
- **E6 No native claude.exe** → AI on shows the "Claude not detected" notice and the toggle stays off.
- **E7 A follow-up answered in words** (no show) → the cards stay exactly as they were (no rebuild, no scroll to the top);
  they come back to full brightness at the answer.
- **E8 The panel collapsed during a request** → no ring spins inside the collapsed panel; an expand shows the spinner
  again while the request still runs, or the final list when it ended meanwhile.
- **E9 ⊘ with an EMPTY box** (2026-11-10, NOT BUILT) → during a request and after an answer alike, the conversation resets
  and the newest request is back in the box on one line, the caret after it (`am.side.filter`'s text; `ai-reset box=0
  refill=M`); Enter asks it again as the new conversation's first request. A box of spaces counts as empty.
- **E10 The copy button under ⊘** (2026-11-10, NOT BUILT; `am.side.ai.copy`) → disabled before the first request. After
  two requests and a list, a click puts Markdown on the clipboard: `## AI search · 2 requests`, each request and Claude's
  reply (no plan text), `### Listed sessions · N` with each card's title, state, folder, id and why; the chime plays. During
  a request the running one reads "still running" and the list says it is the answer before it. With the box empty the
  newest request comes back WITHOUT focus (no `process up` after a park). *Check:* [LOG] `sidepanel ai-copy chars=N
  requests=2 listed=N refill=M`.

---

### SP-10 · The header's ⋯ menu

> **Since** after 0.8.x (2026-11-03: dev 0.0.1.42, production 0.8.0.9; the mirrored line and the dock item's place
> 2026-11-04, NOT BUILT) · **P1** · **Needs** F-S2 · **Spec** SIDE_PANEL.md §2i

**Promise.** The header line is **+**, **Sum** and **⋯**, mirrored with the dock so ⋯ is always at the window's edge:
`⋯ Sum +` from the left edge docked left, `+ Sum ⋯` against the right edge docked right. The ⋯ menu hangs from ⋯'s outer
edge and holds what the other five buttons did, in the user's order — Scroll to the selected tab (⌖, a check) · Card
scope ▸ · Card order ▸ · a separator · Dock on the right / left (⇄) · Collapse the side panel — each with its symbol left
of its text and the current state in its text or its check; each item does exactly what its button did.

**Phases**
1. **The line.** *Expect:* docked left (the default) `⋯ Sum +` from the window's left edge, whole on a narrow panel; ⋯ is
   named "More options" (`am.side.more`); its tip opens after 3 s.
2. **The menu.** *Do:* click ⋯. *Expect:* below the button, its left edge on ⋯'s, inside the panel: `Scroll to the
   selected tab` (checked: ⌖ is on by default) · `Card scope: LOCAL ›` · `Card order: MOST ACTIVE ›` · a separator · `Dock
   on the right` · `Collapse the side panel`. *Do:* Esc. *Expect:* the menu closes and nothing changed (no keystroke
   reached the terminal).
3. **Card scope.** *Do:* ⋯ ▸ Card scope ▸ GLOBAL. *Expect:* the count line `… across all windows`; the next open reads
   `Card scope: GLOBAL` with GLOBAL checked. LOCAL again restores the count line.
4. **Card order.** *Do:* ⋯ ▸ Card order ▸ NEWEST, then OLDEST. *Expect:* the cards re-sort at once (t2 above t1, then t1
   above t2), the Triage Board follows, settings.json `boardSort` reads `newest`, then `oldest`; the next open reads
   `Card order: OLDEST` with OLDEST checked; each entry's tip says what it sorts by.
5. **Scroll to the selected tab.** *Do:* pick it twice. *Expect:* settings.json `sidePanelScrollToSelectedTab` off, then
   on; the next open shows the check that matches (every open window follows — SP-03).
6. **Dock.** *Do:* ⋯ ▸ Dock on the right. *Expect:* the panel moves right and the line reads `+ Sum ⋯` against the
   window's right edge; the next open hangs from ⋯'s right edge, reads `Dock on the left`, and the collapse symbol is
   `»`. *Do:* Dock on the left. *Expect:* back, the line `⋯ Sum +` again.
7. **Collapse.** *Do:* ⋯ ▸ Collapse the side panel. *Expect:* the 20 px strip; its chevron restores the panel.

**Edge cases**
- **E1 Another window changes a setting while this menu is open** (the order, ⌖, the dock) → the pick acts on what the menu
  showed: a dock or ⌖ item that no longer matches the state does nothing (never flips it back), an order or scope already
  set does nothing; the next open shows the new state.
- **E2 The + launcher card is open** → the press on ⋯ closes it (an outside press), then the menu opens.
- **E3 A card peek** → opening and closing the menu keeps the peeked card selected (the menu's focus hand-back is no touch
  of the terminal, SP-02 phase 8).
- **E4 Docked right** → the menu still opens inside the panel (right edges aligned with ⋯; docked left the left edges).
- **E5 Another window moves the dock** (a settings broadcast) → this window's panel moves and its line mirrors with it.

---

### SP-11 · Pin to Top

> **Since** 2026-11-06 (NOT BUILT) · **P1** · **Needs** F-S2 (+ F-W2 for E3) · **Spec** SIDE_PANEL.md §2j

**Promise.** A card's right-click menu pins it, right under Jump to Tab — the only way in or out. A pinned card goes to
the top of the list, the newest pin on top, and wears a pushpin at its title band's right end (right of any tag
ribbons); **Pin to Top** on a pinned card brings it back to the top; **Unpin** lets it drop back into the chosen order.
Nothing else about the session changes: it is not Telegram's never-idle `/pin`.

**Phases**
1. **The items.** *Do:* right-click B's card. *Expect:* `Pin to Top` (`am.side.pin`) right under `Jump to Tab`, no
   `Unpin`; the Triage Board card's and the tree row's menus have neither.
2. **Pin.** *Do:* Pin to Top on the card that is LOWER in the list. *Expect:* it moves to the top; its band shows the
   pushpin; its title's item status reads `… pinned to the top …`; sessions.json has its `sidePanelPinnedAtUnixMs`; its
   state is unchanged (Waiting stays Waiting, and an Idle card stays Idle).
3. **The newest on top.** *Do:* pin the other card. *Expect:* it is above the first; the first keeps its pushpin.
4. **Pin again.** *Do:* right-click the lower pinned card. *Expect:* `Pin to Top` and `Unpin` (`am.side.unpin`). *Do:* Pin
   to Top. *Expect:* it is the top card again.
5. **Unpin.** *Do:* Unpin the top card. *Expect:* its pushpin is gone and it is back in the order below the other pin;
   the list did not scroll; sessions.json has no stamp for it.
6. **The pushpin is no button.** *Do:* click a pinned card's pushpin. *Expect:* the card is selected (a peek), nothing is
   pinned or unpinned; the card's tooltip names the pin.
7. **Restart.** *Do:* close and reopen the window (or the app). *Expect:* the pinned card is still on top, with its
   pushpin.

**Edge cases**
- **E1 A long list scrolled down** → Pin to Top on a far card scrolls the list to the top, where the card now is — with ⌖
  on and with ⌖ off.
- **E2 A pinned card with tags** → the ribbons stay in the top-right corner, the pushpin right of them; untagging keeps
  the pushpin, unpinning keeps the ribbons; the band is no taller than an unpinned card's.
- **E3 Another window's GLOBAL list** → it re-sorts within ~2.5 s (its registry drain, or the periodic tick); its card
  shows the pushpin.
- **E4 `/clear` in a pinned tab** → the new conversation's card is pinned at the same place; the old conversation,
  resumed later from the Sessions page, is not.
- **E5 Telegram `/pin`** (MESSAGING.md §5.3) → the card neither moves nor shows a pushpin; a card pin never holds a
  session in Waiting for you.
- **E6 The AI view** → a result card of a pinned open session shows the pushpin; Pin to Top from a result card shows it at
  once, and AI off shows the card at the top of the list.

---

### SP-12 · A card's Open New Session Here and Fork session open next to its tab

> **Since** 2026-11-07 (NOT BUILT) · **P1** · **Needs** F-S2 (+ F-W2 for E1) · **Spec** SIDE_PANEL.md §2k

**Promise.** A card menu's **Open New Session Here ▸** and **Fork session ▸** open the new tab right AFTER the card's own tab,
in the window that hosts that tab — the WT tab menu's "Here" placement — never at the end of the strip, and never in another
window. What opens is what the items always opened (the card's folder, the picked model, a Codex card's codex, a fork's
question when its source is mid-turn).

**Phases**
1. **Open New Session Here.** *Do:* with t1's tab left of t2's, right-click t1's card ▸ Open New Session Here ▸ Default.
   *Expect:* a new session's tab right after t1's and before t2's, selected; t2 keeps its place.
2. **Fork session.** *Do:* right-click t1's card ▸ Fork session ▸ Default (answer the question if one is asked). *Expect:*
   `t1 (fork)` right after t1's tab — before the tab phase 1 opened.
3. **On the Agent Manager page.** *Do:* Home, then a card's Fork session ▸ Default. *Expect:* the page stays; the fork is
   selected on it; its tab sits right after the card's tab.

**Edge cases**
- **E1 Another window's session** (the card list on GLOBAL in window A, the card's tab in window B) → window B comes to the
  front with the new tab right after the card's tab; window A's strip is unchanged.
- **E2 A Codex card** → Open New Codex Session Here and Fork session open next to its tab too.
- **E3 The board card and the tree row** → their Open New Session Here / Fork session still open at the end of the strip of
  the window they were used in.

---

### SP-13 · The filters: the funnel, By Folder and the chips row

> **Since** 2026-11-08 (NOT BUILT) · **P1** · **Needs** F-S2 (two sessions in two folders) · **Spec** SIDE_PANEL.md §2l

**Promise.** The header's funnel opens a panel that looks like a menu and STAYS OPEN while you pick: By Folder… (the `+`
card's own folder picker, with Filter in Launch's place), By State, By Tag, Active within, four flags, Clear all filters. The
cards re-filter at every pick; a row under the filter box — shown only while something narrows the list — says what does,
one chip per filter with ✕, and Clear. Different filters narrow together; picks inside one add up. The window keeps its
filters across a restart; a new window starts unfiltered. With no filter there is no count line on the screen.

**Phases**
1. **No filter, no row.** *Expect:* the header line `⋯ Sum ⏷ +` (docked left; `am.side.launch` innermost, `am.side.funnel`
   beside it — the swap of 2026-11-10; `⋯ Sum + ⏷` on a build before it), the `+` and funnel buttons one size; no chips row
   (`am.side.filterRow.count` absent from the tree); the count words read from the filter box's ItemStatus (`count_line`) —
   "2 sessions in this window".
2. **The panel stays open.** *Do:* click the funnel; expand By State; check Running, then Waiting for you. *Expect:* the
   panel (its rows, `am.side.filter.*`) is still open after each click; each row's ItemStatus reads "checked"; the cards list only
   those states; the funnel's ItemStatus "1 filter on"; the chips row shows "N of 2" and one chip "Running or Waiting for
   you" (`am.side.chip.state`).
3. **Close.** *Do:* Escape. *Expect:* the panel closes on the key's release; no ESC reached the terminal (a running claude
   keeps running); the chips stay.
4. **By Folder.** *Do:* funnel ▸ By Folder…. *Expect:* the folder card (`am.side.folderCard.box`) opens where the panel was, its
   box empty and focused, its list open with FOLDERS YOUR SESSIONS WORK IN first (both sessions' folders, "1 session" each).
   *Do:* click A's folder row, then Filter (`am.side.folderCard.filter`). *Expect:* the card closes; only A's card is
   listed (if A is in a listed state); a folder chip with the folder's leaf (`am.side.chip.folder.0`).
5. **Exactly.** *Do:* By Folder… again, type A's folder, uncheck Include subfolders, Enter. *Expect:* the folder chip reads
   "<leaf> (exactly)" (one chip, not two).
6. **The chips' ✕ and Clear.** *Do:* ✕ on the state chip. *Expect:* the state filter is gone, the folder one stays. *Do:*
   Clear (`am.side.filterRow.clear`). *Expect:* every filter gone, the row gone, the funnel unlit, every card back.
7. **Restart.** *Do:* set a filter, close and reopen the window. *Expect:* the same chips and cards; windows/<id>.json has
   `sidePanelFilters`; a NEW window starts unfiltered.

**Edge cases**
- **E1 Typed text** → typing in the filter box alone shows the row with a “text” chip; its ✕ empties the box.
- **E2 A click outside** → a click on a card closes the panel AND selects the card in one click (no light dismiss).
- **E3 AI mode** → AI on: the chips row hides and the count line shows the AI words; AI off: the chips come back.
- **E4 Unread** → with Unread checked, a session that turns unread appears without a registry change (the census hop).
- **E5 Dock right** → the line reads `+ ⏷ Sum ⋯` (`⏷ + Sum ⋯` before the swap of 2026-11-10); the panel opens at the
  click toward the window's center.

### SP-14 · By Folder's hover submenu: the folders picked, each a toggle

> **Since** 2026-11-10 (NOT BUILT) · **P1** · **Needs** F-S2 (two sessions in two folders A and B), two windows for phase 6 ·
> **Spec** SIDE_PANEL.md §2l *The By Folder submenu*

**Promise.** By Folder… stays exactly as it was when CLICKED (the `+` card's folder picker). RESTING the pointer on it (the
system's menu delay), or pressing → on it, opens a submenu beside the panel: **Pick…** (the same picker), a separator, then
one line per folder picked before — the last pick on top. A ✓ marks a folder that filters THIS window now; a click on a line
turns its folder on or off, the submenu stays open and its lines keep their place; a repick moves the folder to the top at
the next open. The list is one for the app (every window shows the same folders), kept across restarts in the profile's
`filter-folders.json`; each window's ✓ marks are its own.

**Phases**
1. **Empty at first.** *Do:* on a fresh profile, open the funnel's panel, rest the pointer on By Folder…. *Expect:* after
   about the menu delay (400 ms by default) the submenu opens beside the panel's right edge (left edge near the window's
   right side), Pick… (`am.side.filter.folderMenu.pick`) level with By Folder…, then "No folder picked yet — Pick… adds one
   here."; no `filter-folders.json` was written by the open.
2. **A click still picks.** *Do:* click By Folder… itself. *Expect:* the panel and the submenu close, the folder card opens
   (SP-13 phase 4 unchanged). *Do:* pick A's folder. *Expect:* `filter-folders.json` lists A.
3. **The line toggles.** *Do:* reopen the panel, rest on By Folder…. *Expect:* one line, A's leaf in bold and its path dim,
   ✓ (`am.side.filter.folderMenu.folder.0`, ItemStatus "checked"), its count. *Do:* click it. *Expect:* the ✓ goes, the
   folder chip goes, every card is back, the submenu is still open and the line still there. *Do:* click it again.
   *Expect:* ✓ and the chip are back.
4. **Repick moves to top.** *Do:* Pick… → B's folder (unchecked Include subfolders). *Expect:* at the next open: B "(exactly)"
   first, A second, both ✓.
5. **Hover rules.** *Do:* with the submenu open, rest on By State. *Expect:* the submenu closes after the delay. *Do:* move
   from By Folder… diagonally into the submenu across By State. *Expect:* it stays open.
6. **One list, own checks.** *Do:* in a second window, open its panel and rest on By Folder…. *Expect:* the same lines in
   the same order, none checked (that window filters nothing); a click there filters THAT window only.
7. **Keyboard.** *Do:* focus By Folder… (the panel's first row), press →. *Expect:* the submenu opens with Pick… focused;
   ↓ walks the lines, Enter toggles one (the submenu stays); ← (or Escape, on its release) closes it and the keyboard is back
   on By Folder…; another Escape closes the panel. No ESC reaches the terminal.
8. **Restart.** *Do:* restart the app. *Expect:* the submenu lists the same folders in the same order.

**Edge cases**
- **E1 An older filter** → a window whose record filters by a folder the history lacks (saved before the submenu existed)
  lists it on top, checked, and from then on it stays listed when turned off.
- **E2 The cap** → after eleven different picks the oldest folder is gone from the list (ten kept).
- **E3 Touch** → a finger never opens the submenu by itself (no hover); a tap on By Folder… opens the folder card.
- **E4 A narrow window** → the submenu opens on the side with room, or clamped inside the window when neither side holds it.

### SP-15 · Pause filter, Resume filter and a chip's click

> **Since** 2026-11-12 (NOT BUILT) · **P1** · **Needs** F-S2 (two sessions in two folders A and B, in different states) ·
> **Spec** SIDE_PANEL.md §2l *Pause filter, Resume filter and a chip's click*

**Promise.** A filter can be switched OFF without being dropped. A click on a chip switches that one filter off and on
again (its ✕ still drops it). **Pause filter**, right before Clear, switches the whole row off — every chip, the typed text
too — and the list shows every session; **Resume filter** brings back exactly the chips that applied before. Whatever is
off keeps its chip, dimmed. A pick that adds a filter, and typing in the filter box, resume a paused row; removing a pick
does not. The window keeps a paused row and a switched-off chip across a restart.

**Phases**
1. **The link.** *Do:* funnel ▸ By State ▸ Running (the state of A alone). *Expect:* the chips row ends in a link named
   "Pause filter" (`am.side.filterRow.pause`, ItemStatus empty) and Clear; only A's card is listed.
2. **Pause.** *Do:* click Pause filter. *Expect:* both cards listed; the count reads "2 of 2" and the filter box's count
   words "2 sessions in this window"; the link is named "Resume filter" (ItemStatus "paused"); the state chip's body
   (`am.side.chip.state.toggle`) has ItemStatus "paused"; the funnel is unlit, its ItemStatus "1 filter paused".
3. **Resume.** *Do:* click Resume filter. *Expect:* only A's card again; the link "Pause filter"; the funnel lit, "1 filter on".
4. **A chip's click.** *Do:* add By Folder = A's folder (two chips); click the state chip's body. *Expect:* that chip
   reads "paused", the folder chip does not; the list is A's folder's sessions in any state; the funnel "1 filter on, 1
   paused". *Do:* click the body again. *Expect:* both chips apply.
5. **Resume restores.** *Do:* switch the state chip off, click Pause filter, then Resume filter. *Expect:* the folder
   chip applies and the state chip is still "paused" — the row as it was before the pause.
6. **This one alone.** *Do:* Pause filter, then click the state chip's body. *Expect:* the state chip applies, the
   folder chip is "paused", the link reads "Pause filter".
7. **A pick resumes.** *Do:* Pause filter; funnel ▸ By State ▸ check a second state. *Expect:* the panel stays open, the
   row resumes: no chip "paused", the list filtered. *Do:* Pause filter; uncheck that state. *Expect:* still paused.
   *Do:* ✕ on the folder chip. *Expect:* the chip is gone, the row still paused.
8. **Typing.** *Do:* with the row paused, type a word in the filter box. *Expect:* the row resumes and the text chip
   applies. *Do:* Pause filter. *Expect:* the text chip (`am.side.chip.text.toggle`) "paused", every card listed, the box
   still holding the word.
9. **Restart.** *Do:* leave one chip switched off and the row paused; close and reopen the window. *Expect:* the same
   chips, the row paused; after Resume filter the same chip is still off. `windows/<id>.json`'s `sidePanelFilters` has
   `"paused": true` and `"off"`.

**Edge cases**
- **E1 Every chip off by hand** → the link reads "Resume filter"; its click switches every chip on.
- **E2 A double-click** → a double-click on Pause filter pauses once (the second click is ignored: `[nav] sidepanel filter
  toggle click ignored`); a double-click on a chip's body switches it once; neither ever drops a filter through a ✕ or Clear.
- **E3 Only typed text** → the row shows the text chip, Pause filter and Clear; paused, emptying the box removes the row,
  and the next text typed filters at once (no pause left behind).
- **E4 The counts** → with the row paused, the panel's row counts still say what a pick would list (a pick resumes).
- **E5 Clear** → drops every filter, every switch and the pause; the row is gone.
- **E6 A folder picked again** → a dimmed folder picked again in the folder card comes back on (and resumes a paused
  row). The submenu's ✓ marks a folder that is PICKED, on or off: a click on it drops the folder. The pick history never
  lists a folder as off.

---

### SP-16 · The collapsed strip hides itself

> **Since** 2026-11-12 (NOT BUILT) · **P1** · **Needs** F-S (one session, its terminal on screen), the panel docked left ·
> **Spec** SIDE_PANEL.md §2m

**Promise.** A collapsed panel leaves the terminal the whole width. Its 20 DIP strip, with the expand chevron in its
middle, floats over the terminal's docked edge and hides itself: it shows for a moment after the collapse, then only
while the pointer is within 40 DIPs of it (60 DIPs from the edge), at any height. Showing and hiding it never changes
the terminal's size. A press on the strip beside the chevron goes to the terminal and sends the strip away until the
pointer has left and come back. The chevron's ItemStatus reads `hidden` while the strip is hidden, nothing while it shows.

**Phases**
1. **Collapse.** *Do:* ⋯ ▸ Collapse the side panel; move the pointer to the middle of the terminal. *Expect:* the strip
   shows over the terminal's edge for about 1.5 s, then fades out (ItemStatus `hidden`); the terminal's rectangle starts
   at the content's left edge and is as wide as the content; `am.side.expand` is still in the tree with its rectangle.
2. **Approach.** *Do:* move the pointer to 200 px from the edge, then to 30 px from it, well above the chevron.
   *Expect:* hidden at 200 px; at 30 px the strip shows (ItemStatus empty) and stays while the pointer rests; the
   terminal's rectangle is unchanged.
3. **Away.** *Do:* move back to 200 px. *Expect:* the strip hides.
4. **A press beside the chevron.** *Do:* approach again; click the strip 10 px from the edge, above the chevron.
   *Expect:* the strip hides at once and the click lands in the terminal; moving the pointer along the edge shows
   nothing; after the pointer went 200 px away and returned, the strip shows again.
5. **Out through the edge.** *Do:* with the strip showing, move the pointer out of the window past the docked edge.
   *Expect:* the strip hides within about a third of a second (the app's watch; no pointer event tells the content).
6. **Expand.** *Do:* click the chevron. *Expect:* the panel is back at its width, the terminal narrower again; no
   `[exc] sidepanel strip` line in hooks.log.

**Edge cases**
- **E1 A drag passing by** → a text selection dragged from the middle of the terminal to its first column shows no
  strip, during the drag or after the release, until the pointer has left the edge and come back.
- **E2 Click during the linger** → a click into the terminal right after the collapse hides the strip at once.
- **E3 Docked right** → the same at the right edge; the chevron reads «; a press on the strip reaches the scroll bar
  under it.
- **E4 The Agent Manager page** → with the page showing and the panel collapsed, the strip floats over the page's edge
  the same way.
- **E5 A window that opens collapsed** → no strip (and no linger) until the pointer comes near.
- **E6 A finger** → a tap near the edge shows the strip; a tap beside the chevron hides it; a tap on the chevron expands.
- **E7 A dock flip from another window** → a collapsed window's strip moves to the other edge, hidden.

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
> 0 (top item) and have the scroll jump to the top while the click was in a 5th or whatever index card in the list",
> and again later that day: "When I deselect the right-click menu of sidemenu card, i think it also jumps" (closing
> the menu without a choice). Read from the code and from WinUI's source; not yet reproduced by the e2e suite (SP-02
> phase 8 and E8–E10 are its checks; `e2e/suites/test_side_panel.py::test_card_menu_keeps_the_peek`, `needs_fix`).
> Present since the side panel's follow rule (0.7.9).
> **The cause:** the card's context menu takes keyboard focus when it opens, and when it closes WinUI gives focus back
> to the element that held it, the terminal (`CPopup::Close`). The terminal's `GotFocus` was one of the side panel's
> two "back to work" signals (SIDE_PANEL.md §3), so every way of closing the menu — an item, Esc, a click outside it
> — ended the peek of the card the menu was opened on. The focused tab's card was selected instead and, with ⌖ on
> (the default), the list scrolled to it, usually the top card (MOST ACTIVE / MOST RECENT put the session you work in
> first).
> **The fix** (`TerminalPage.AgentSidePanel.cpp`, `SidePanelFocusArrivalEndsPeek`): the `GotFocus` follows only focus
> that comes from the terminal column itself or from the side panel's own controls. Focus that arrives from NOWHERE —
> how a closing light-dismiss menu hands it back: `CPopup::Close` moves focus off its leaving content first (onto the
> popup itself, or nowhere) and only then restores the terminal — keeps the peek, and so does focus handed back by a
> page overlay, focus from the title bar's tab strip and the window coming back to the foreground. A click into the
> terminal still ends a peek; a click that only closes the menu never reaches the terminal (the popup root takes it).
> **The first version** (a0e892b90, the same day, never built) treated focus from nowhere as the window regaining
> activation and followed, so it fixed neither report; the second version keeps it (SIDE_PANEL.md §3 has the WinUI
> reading). Every kept peek that would have moved the selection writes `[sidepanel] focus came back to the terminal
> from <origin>, …` in hooks.log, `<origin>` naming how focus came back, so a test build shows which case a menu's
> hand-back is. What it was on 0.8.0:

Right-click any card below the focused tab's card → **Move to Idle/Done** (or any other item, or Esc, or a click
outside the menu): the card the menu was about is deselected, the focused tab's card is selected, and with ⌖ on the
list jumps to that card. The same happens when **Show Transcript**'s reading view closes. **Repro:** 10+ sessions,
the focused tab's card at the top, the list scrolled down, right-click a lower card → Move to Idle/Done (or Esc).
**Expected:** SP-02 phase 8.

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
