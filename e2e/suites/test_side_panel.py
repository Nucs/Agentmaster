"""The fleet side panel (doc/agentmaster/flows/02-side-panel.md, SP).

The panel is read through its header buttons, its count line, its cards (title + state word) and the summary
under them; it is driven with real clicks, hovers and keys.
"""
import re
import time

import pytest

from amdriver import inputs
from amdriver.session import wait_new_session
from amdriver.ui.menus import Menu
from amdriver.ui.sidepanel import HEADER_ORDER, MORE_ITEM_IDS, MORE_ITEM_ORDER, ORDER_TOKENS
from amdriver.waits import hold_true, wait_until

SHELL = "Administrator: Windows PowerShell"


@pytest.mark.p0
@pytest.mark.flow("SP-01")
def test_panel_lists_this_windows_sessions(am, s2):
    """SP-01 phases 1, 3 and 4: the cards and the count line; a card follows its session's state; GLOBAL.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    panel = am.window.side_panel
    wait_until(lambda: sorted(panel.titles()) == ["t1", "t2"], 10, what="both cards", describe=panel.titles)
    assert panel.count_line() == "2 sessions in this window"
    # The cards follow the registry within a refresh (edits at least 500 ms apart): t2's turn just ended.
    panel.wait_card("t1", state="waiting-for-you", timeout_s=5)
    panel.wait_card("t2", state="waiting-for-you", timeout_s=5)
    assert panel.scope() == "LOCAL"
    # 3. The state follows: a held turn reads running, then waiting-for-you again.
    a.ask('hold it [[mock say "busy"; hold g; reply "done"]]', wait="sent")
    am.mock.wait_holding("g", timeout_s=30)
    panel.wait_card("t1", state="running", timeout_s=20)
    am.mock.release("g")
    panel.wait_card("t1", state="waiting-for-you", timeout_s=30)
    # 4. GLOBAL: every window's sessions (one window here), and the count line says so.
    assert panel.toggle_scope() == "GLOBAL"
    wait_until(lambda: re.match(r"^2 sessions across all windows", panel.count_line()), 5,
               what="the GLOBAL count line", describe=panel.count_line)
    assert panel.toggle_scope() == "LOCAL"


@pytest.mark.p0
@pytest.mark.flow("SP-02")
def test_follow_peek_and_jump(am, s2):
    """SP-02 phases 1–4: the summary follows the selected tab; a card click peeks (the tab stays); a click into
    the terminal snaps back; a card double-click jumps to its tab.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    w = am.window
    panel = w.side_panel
    a.select()
    wait_until(lambda: panel.summary_title() == "t1", 10, what="the summary on t1", describe=panel.summary_texts)
    titles_before = w.tabs.titles()
    panel.card("t2").click()
    wait_until(lambda: panel.summary_title() == "t2", 10, what="the peek on t2", describe=panel.summary_texts)
    assert w.tabs.selected().title == "t1" and w.tabs.titles() == titles_before
    w.terminal.focus()
    wait_until(lambda: panel.summary_title() == "t1", 10, what="the summary back on t1", describe=panel.summary_texts)
    panel.card("t2").double_click()
    w.tabs.wait_selected("t2")


@pytest.mark.p1
@pytest.mark.flow("SP-02")
def test_card_context_menu(am, s1):
    """SP-02 phase 5: right-clicking a card opens the board card's session menu.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    before = Menu.open_hwnds(am)
    w.side_panel.card("t1").right_click()
    menu = Menu.wait_new(am, before)
    names = menu.names()
    for item in ("Jump to Tab", "Move to Idle/Done", "Rename (F2)", "Tags", "Copy", "Show Transcript",
                 "Open New Session Here", "Restart session", "Fork session", "Close"):
        assert item in names, names
    menu.close()


@pytest.mark.p1
@pytest.mark.flow("SP-02")
@pytest.mark.needs_fix("SP-K3")
def test_card_menu_keeps_the_peek(am, s2):
    """SP-02 phase 8 + E8 (SP-K3's fix, 2026-10-24): closing a card's right-click menu keeps that card selected —
    with Esc, with a click outside the menu, and with one of its commands (Move to Idle/Done); a click into the
    terminal still ends the peek. On 0.8.0 the menu handed the keyboard back to the terminal as it closed, the
    terminal's GotFocus ended the peek, and the focused tab's card was selected again (with ⌖ on, the list scrolled to
    it — two cards here, so only the selection is read).

    The app names how each hand-back arrived (`[sidepanel] focus came back to the terminal from <origin>, …`):
    printed for the record, since WinUI's source says "nowhere" (the closing menu moves focus off its own content
    first) and only a real build can confirm it. At least one such line must appear: without it the menus never handed
    focus to the terminal, and this test did not exercise SP-K3's path.

    Args:
        am: The app.
        s2: F-S2 (t1 and t2 in one window).
    """
    a, b = s2
    w = am.window
    panel = w.side_panel
    a.select()
    # The keyboard in t1's terminal first: the menu gives focus back to whatever held it when it opened, and only a
    # return INTO the terminal is the path under test (a real click — it also makes the summary follow t1).
    w.terminal.focus()
    wait_until(lambda: panel.summary_title() == "t1", 10, what="the summary on t1", describe=panel.summary_texts)
    mark = am.profile.hooks.mark()

    def open_menu_on_t2() -> Menu:
        """Right-click t2's card: its menu opens, and the right-click itself peeks at t2 (like a click).

        Returns:
            The open menu.
        """
        before = Menu.open_hwnds(am)
        panel.card("t2").right_click()
        menu = Menu.wait_new(am, before)
        wait_until(lambda: panel.summary_title() == "t2", 10, what="the right-click's peek on t2",
                   describe=panel.summary_texts)
        return menu

    # 1. Esc (Menu.close presses it).
    open_menu_on_t2().close()
    hold_true(lambda: panel.summary_title() == "t2", 2.0, what="t2 still selected after Esc closed its menu",
              describe=panel.summary_texts)
    # 2. A click outside the menu, on the card list's empty area below the two cards: the popup root takes that press (it
    # reaches no element). It was the count line until the filters (SIDE_PANEL.md §2l) took that line off the screen while
    # no filter is on; the empty list area is as inert a target (no card, no control, nothing that takes focus).
    open_menu_on_t2()
    view = panel.cards_view()
    assert view is not None, "the card list's ScrollViewer is not in the tree"
    inputs.click(view.rect[0] + view.rect[2] - 12, view.rect[1] + view.rect[3] - 12)
    wait_until(lambda: not Menu.open_hwnds(am), 5, what="the click outside closes the menu")
    hold_true(lambda: panel.summary_title() == "t2", 2.0, what="t2 still selected after a click outside closed its menu",
              describe=panel.summary_texts)
    # 3. A command: the user's own report ("Move to Idle/Done" selected the top card and jumped the list there).
    open_menu_on_t2().choose("Move to Idle/Done")
    panel.wait_card("t2", state="idle", timeout_s=10)
    hold_true(lambda: panel.summary_title() == "t2", 2.0, what="t2 still selected after Move to Idle/Done",
              describe=panel.summary_texts)
    assert w.tabs.selected().title == "t1"  # the menus never switched the tab
    kept = am.profile.hooks.find(r"\[sidepanel\] focus came back to the terminal from (.*?), not by a click", since=mark)
    for line in kept:
        print("SP-K3: focus came back from " + line.group(1))
    assert kept, "no kept-peek line in hooks.log: no menu handed focus back to the terminal"
    # 4. A click into the terminal ends the peek.
    w.terminal.focus()
    wait_until(lambda: panel.summary_title() == "t1", 10, what="the peek ended by a click into the terminal",
               describe=panel.summary_texts)


@pytest.mark.p1
@pytest.mark.flow("SP-04")
def test_sum_off_lets_the_cards_fill_the_panel(am, s1):
    """SP-04 phase 1: Sum off removes the summary; Sum on brings it back.

    Args:
        am: The app.
        s1: F-S.
    """
    panel = am.window.side_panel
    assert panel.sum_on()
    height_on = panel.cards_view().rect[3]
    assert panel.summary_title() == "t1"
    panel.set_sum(False)
    wait_until(lambda: panel.summary_texts() == [], 5, what="the summary gone", describe=panel.summary_texts)
    assert panel.cards_view().rect[3] > height_on
    panel.set_sum(True)
    wait_until(lambda: panel.summary_title() == "t1", 5, what="the summary back", describe=panel.summary_texts)


@pytest.mark.p0
@pytest.mark.flow("SP-05")
def test_plus_launcher_card(am, s1, mock):
    """SP-05 phases 1, 3 and 4 (+ E3): + opens the launch row in a card with its folder list open; Esc closes the
    list, a second Esc the card (no ESC reaches claude); Enter in the box commits the path only; Enter on the Launch
    button starts exactly one session and closes the card (no CR reaches claude).

    Args:
        am: The app.
        s1: F-S (t1's terminal is the one an ESC would reach).
        mock: The scripted model.
    """
    w = am.window
    panel = w.side_panel
    card = panel.open_launcher()
    wait_until(lambda: card.folder_list() is not None, 5, what="the folder list open under the box")
    box = card.box()
    assert box.has_focus, "the caret is not in the + card's box"
    lst = card.folder_list()
    assert lst.rect[1] >= box.rect[1] + box.rect[3] - 2, (lst.rect, box.rect)  # under the box, not over it
    assert "Browse…" in card.folder_list_texts()
    inputs.keys("esc")
    wait_until(lambda: card.folder_list() is None, 5, what="Esc closes the folder list")
    assert card.is_open()
    inputs.keys("esc")
    wait_until(lambda: not card.is_open(), 5, what="a second Esc closes the card")
    assert s1.state == "WaitingForInput" and s1.claude_pids()  # no ESC reached claude (no interrupt)
    # 4 (E3). Launch from the keyboard: Enter in the BOX only commits the path (the folder list closes, nothing
    # launches); Enter on the focused Launch button launches, on the key-up, so no CR reaches the terminal.
    folder = am.iso.folder("t2")
    card = panel.open_launcher()
    card.type_folder(str(folder))
    sessions_before = {s.id for s in am.profile.sessions()}
    mark = am.profile.hooks.mark()
    inputs.keys("enter")
    wait_until(lambda: card.folder_list() is None, 5, what="Enter in the box closes the folder list")
    time.sleep(1.0)  # the box's Enter must not launch: give a wrong launch its chance to show
    assert not am.profile.hooks.find(r"\[nav\] open-new claude", since=mark) and card.is_open()
    card.launch_button().focus()
    inputs.keys("enter")
    new = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    wait_until(lambda: not card.is_open(), 5, what="the card closes after the launch")
    w.tabs.wait_selected("t2")
    time.sleep(2)
    assert {s.id for s in am.profile.sessions()} - sessions_before == {new.id}  # exactly one new session
    assert s1.state == "WaitingForInput"  # t1 got no stray Enter


@pytest.mark.p0
@pytest.mark.flow("SP-05")
@pytest.mark.needs_fix("SP-K1")
def test_plus_card_launch_takes_one_click_with_its_menu_open(am, s1, mock):
    """SP-05 phase 5 (SP-K1's fix, 2026-10-23): in the + card, with Launch's presets menu open (a hover opens it at
    once), ONE click on Launch launches — exactly one session — as on the Agent Manager page's own row. On 0.8.0 the
    click only closed the menu and a second click launched (SP-K1).

    The app completes such a click itself (the menu's light dismiss eats the press): hooks.log says how, either
    "its release on Launch completed the click" or "the release watch completed the click" — printed for the record,
    since which of the two paths answers in a popup-hosted card is exactly what the fix had to cover.

    Args:
        am: The app.
        s1: F-S (t1 on screen; the card opens over its tab).
        mock: The scripted model.
    """
    w = am.window
    panel = w.side_panel
    card = panel.open_launcher()
    card.type_folder(str(am.iso.folder("t2")))
    sessions_before = {s.id for s in am.profile.sessions()}
    card.launch_button().hover()
    wait_until(lambda: Menu.open_menus(am), 5, what="Launch's presets menu (it opens on hover)")
    mark = am.profile.hooks.mark()
    card.launch_button().click()  # ONE click, held like a finger's (the driver's 60 ms)
    new = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock,
                           timeout_s=20)
    wait_until(lambda: not card.is_open(), 5, what="the card closes after the launch")
    w.tabs.wait_selected("t2")  # over a tab, the new tab opens in front
    how = am.profile.hooks.find(r"\[launchbar\] Launch clicked while its hover menu was up: (.*)", since=mark)
    print("SP-05 phase 5: " + (how[0].group(1) if how else "no [launchbar] completion line (a real Click: the press reached Launch)"))
    time.sleep(2)
    assert {s.id for s in am.profile.sessions()} - sessions_before == {new.id}  # exactly one new session
    assert not am.profile.hooks.find(r"\[launchbar\] a second Launch click within the double-click time", since=mark)
    w.terminal.wait_input_box()


@pytest.mark.p1
@pytest.mark.flow("SP-06")
def test_collapse_and_dock(am, s1):
    """SP-06 phases 3 and 4: the collapse shrinks the panel to its strip and the strip's chevron restores it; the
    dock moves it to the other side and back. The driver takes whichever header the build has: the ⋯ menu's
    "Collapse the side panel" and "Dock on the right / left" (SP-10), or the « and ⇄ buttons of 0.8.x.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    panel = w.side_panel
    assert panel.dock_side() == "left"
    panel.collapse()
    # The strip's chevron: » on 0.8.0, "Expand the side panel" (am.side.expand) on a build with the accessibility
    # metadata — SidePanel.strip_button finds either.
    expand = wait_until(panel.strip_button, 5, what="the collapsed strip's chevron")
    expand.click()
    wait_until(lambda: not panel.collapsed(), 5, what="the panel back")
    assert panel.flip_dock() == "right"
    assert panel.flip_dock() == "left"


def _assert_header_line(panel, side: str) -> None:
    """The header line for a dock side (SIDE_PANEL.md §2i, the user's layout of 2026-11-04; the funnel of §2l, 2026-11-08): exactly
    +, the funnel, Sum and ⋯ above the filter box, HEADER_ORDER[side] from left to right — ⋯ leftmost docked left, rightmost
    docked right, + innermost with the funnel beside it (the user's swap of 2026-11-10) — with ⋯ in
    the panel's OUTER half, at the window's edge. Waits up to 5 s: a dock flip re-lays the line in the layout pass that
    moves the panel, and UI Automation can report the old positions for a moment.

    Args:
        panel: The side panel.
        side: The dock side the line is read for, "left" or "right".
    Returns:
        None.
    Raises:
        WaitTimeout: When the line does not read HEADER_ORDER[side] with ⋯ in the outer half within 5 s (the message lists
            the buttons as last read, with their rectangles).
        AssertionError: When ⋯ is not named "More options".
    """
    def line_at_the_outer_end():
        header = panel.header_buttons()
        band = panel.band()
        if band is None or [k.automation_id for k in header] != HEADER_ORDER[side]:
            return None
        more = header[0] if side == "left" else header[-1]
        middle = (band[0] + band[1]) / 2
        return header if (more.center[0] < middle if side == "left" else more.center[0] > middle) else None

    header = wait_until(line_at_the_outer_end, 5, what=f"the header line docked {side}",
                        describe=lambda: [(k.name, k.automation_id, k.rect) for k in panel.header_buttons()])
    more = header[0] if side == "left" else header[-1]
    assert more.name == "More options", (more.name, more.automation_id)


def _assert_menu_hangs_from_the_outer_edge(panel, menu: Menu, side: str) -> None:
    """The ⋯ menu opens BELOW ⋯ and hangs from the button's OUTER edge (SIDE_PANEL.md §2i): docked left its left edge is
    ⋯'s and it spreads right, over the panel; docked right its right edge is ⋯'s and it spreads left. Aligned on the
    other edge, it would reach past the window's edge (where the framework slides or flips it, often over the terminal).

    Args:
        panel: The side panel.
        menu: The open ⋯ menu, read once it stopped moving (open_more_menu waits for that).
        side: The dock side, "left" or "right".
    Returns:
        None.
    Raises:
        AssertionError: When there is no ⋯ button, the menu opens over or above it, or it hangs from the other edge (the
            message carries both rectangles).
    """
    more = panel.more_button()
    assert more is not None, "no ⋯ button"
    mx, my, mw, _ = menu.element().rect
    bx, by, bw, bh = more.rect
    where = f"menu {menu.element().rect} vs ⋯ {more.rect}"
    # 4 px of overlap and 12 px of edge offset absorb the presenter's border and shadow insets; the other edge of the
    # button is a whole button width (~26 px) away, so a menu aligned on it still fails.
    assert my >= by + bh - 4, f"the menu opens over or above ⋯: {where}"
    if side == "left":
        assert abs(mx - bx) <= 12 and mx + mw > bx + bw, f"docked left the menu must hang from ⋯'s LEFT edge: {where}"
    else:
        assert abs((mx + mw) - (bx + bw)) <= 12 and mx < bx, f"docked right the menu must hang from ⋯'s RIGHT edge: {where}"


@pytest.mark.p1
@pytest.mark.flow("SP-10")
@pytest.mark.needs_fix("SP-F1")
def test_more_menu_holds_the_header_controls(am, s2):
    """SP-10 phases 1–7 (SP-F1, the user's layouts of 2026-11-03 and 2026-11-04 — a build with the ⋯ menu; 0.8.x has none,
    and a 2026-11-03 build fails phases 1, 2 and 6 by design: its line does not mirror and its menu lists the dock
    first): docked left the header line is ⋯, Sum and + from the window's edge, docked right its mirror +, Sum, ⋯; the
    ⋯ menu hangs from ⋯'s outer edge and lists ⌖, Card scope ▸, Card order ▸, the dock and the collapse, in that order,
    each showing the current state in its text or its check; Esc closes it and changes nothing; each item does what its
    button did — the scope (the count line), the order (the cards and settings.json), ⌖ (settings.json and the check),
    the dock (the panel's side, the line's mirror, the menu's edge and the item's words), the collapse (the strip).

    Args:
        am: The app.
        s2: F-S2 (t1 launched before t2, so NEWEST and OLDEST order the two cards differently).
    """
    w = am.window
    panel = w.side_panel
    wait_until(lambda: sorted(panel.titles()) == ["t1", "t2"], 10, what="both cards", describe=panel.titles)
    settings = am.profile.settings()
    order_word = {token: word for word, token in ORDER_TOKENS.items()}.get(settings.get("boardSort", "active"), "MOST ACTIVE")
    scroll_on = settings.get("sidePanelScrollToSelectedTab", True)
    assert panel.dock_side() == "left"

    # 1. The header line, docked left: ⋯, Sum, + from the window's edge (the docked-right line's mirror) — no other button
    #    above the filter box (the AI toggle sits inside the box).
    _assert_header_line(panel, "left")

    # 2. The menu: below ⋯, hanging from its left edge over the panel; five items in the user's order, the state in their
    #    texts and checks.
    menu = panel.open_more_menu()
    _assert_menu_hangs_from_the_outer_edge(panel, menu, "left")
    items = menu.stable_items()
    assert [i.el.automation_id for i in items] == [MORE_ITEM_IDS[k] for k in MORE_ITEM_ORDER], \
        [(i.name, i.el.automation_id) for i in items]
    by_id = {i.el.automation_id: i for i in items}
    assert by_id[MORE_ITEM_IDS["dock"]].name == "Dock on the right"
    assert by_id[MORE_ITEM_IDS["scroll"]].name == "Scroll to the selected tab"
    assert by_id[MORE_ITEM_IDS["scroll"]].el.toggle_state == (1 if scroll_on else 0)
    assert by_id[MORE_ITEM_IDS["scope"]].is_sub and by_id[MORE_ITEM_IDS["scope"]].name == "Card scope: LOCAL"
    assert by_id[MORE_ITEM_IDS["order"]].is_sub and by_id[MORE_ITEM_IDS["order"]].name == f"Card order: {order_word}"
    assert by_id[MORE_ITEM_IDS["collapse"]].name == "Collapse the side panel"
    # 3. Card scope ▸: LOCAL checked, GLOBAL not; Esc on every level changes nothing.
    sub = menu.submenu(by_id[MORE_ITEM_IDS["scope"]].name)
    assert [(i.el.automation_id, i.el.toggle_state) for i in sub.stable_items()] == \
        [("am.side.scope.local", 1), ("am.side.scope.global", 0)]
    menu.close()
    assert not panel.collapsed() and panel.scope() == "LOCAL" and panel.dock_side() == "left"

    # 4. Card scope ▸ GLOBAL: the count line names every window, the menu's header says GLOBAL; then LOCAL again.
    panel.set_scope("GLOBAL")
    assert re.match(r"^2 sessions across all windows", panel.count_line()), panel.count_line()
    menu = panel.open_more_menu()
    assert panel.more_item(menu, MORE_ITEM_IDS["scope"]).name == "Card scope: GLOBAL"
    menu.close()
    panel.set_scope("LOCAL")

    # 5. Card order ▸: the five orders, the current one checked; NEWEST and OLDEST sort t1 / t2 both ways and are saved.
    menu = panel.open_more_menu()
    sub = menu.submenu(panel.more_item(menu, MORE_ITEM_IDS["order"]).name)
    entries = sub.stable_items()
    assert [i.name for i in entries] == list(ORDER_TOKENS), [i.name for i in entries]
    assert [i.el.automation_id for i in entries] == [f"am.side.sort.{t}" for t in ORDER_TOKENS.values()]
    assert [i.name for i in entries if i.el.toggle_state == 1] == [order_word]
    menu.close()
    for word, titles in (("NEWEST", ["t2", "t1"]), ("OLDEST", ["t1", "t2"])):
        panel.choose_more(MORE_ITEM_IDS["order"], f"am.side.sort.{ORDER_TOKENS[word]}")
        wait_until(lambda token=ORDER_TOKENS[word]: am.profile.settings().get("boardSort") == token, 5,
                   what=f"boardSort {word} in settings.json")
        wait_until(lambda want=titles: panel.titles() == want, 5, what=f"the cards in {word} order", describe=panel.titles)
    menu = panel.open_more_menu()
    assert panel.more_item(menu, MORE_ITEM_IDS["order"]).name == "Card order: OLDEST"
    menu.close()
    panel.choose_more(MORE_ITEM_IDS["order"], f"am.side.sort.{ORDER_TOKENS[order_word]}")  # the order it had

    # 6. ⌖: each pick flips the saved setting, and the next open shows the new check.
    for want in (not scroll_on, scroll_on):
        panel.choose_more(MORE_ITEM_IDS["scroll"])
        wait_until(lambda want_on=want: am.profile.settings().get("sidePanelScrollToSelectedTab", True) == want_on, 5,
                   what=f"scroll-to-tab {'on' if want else 'off'} in settings.json")
        menu = panel.open_more_menu()
        assert panel.more_item(menu, MORE_ITEM_IDS["scroll"]).el.toggle_state == (1 if want else 0)
        menu.close()

    # 7. The dock: the item says where the panel goes and takes it there; docked right the line is +, Sum, ⋯ with ⋯ at the
    #    window's right edge and the menu hangs from ⋯'s right edge; back on the left the line mirrors again. The collapse
    #    leaves the strip, which restores.
    assert panel.flip_dock() == "right"
    _assert_header_line(panel, "right")
    menu = panel.open_more_menu()
    _assert_menu_hangs_from_the_outer_edge(panel, menu, "right")
    assert panel.more_item(menu, MORE_ITEM_IDS["dock"]).name == "Dock on the left"
    menu.close()
    assert panel.flip_dock() == "left"
    _assert_header_line(panel, "left")
    panel.collapse()
    panel.expand()


@pytest.mark.p1
@pytest.mark.flow("SP-11")
@pytest.mark.needs_fix("SP-F2")
def test_pin_to_top(am, s2):
    """SP-11 phases 1-5 (SP-F2, the user's request of 2026-11-06 - a build with Pin to Top; SIDE_PANEL.md section 2j): a
    card's right-click menu has Pin to Top right under Jump to Tab, and Unpin only on a pinned card; a pin moves the card
    to the top, its item status says "pinned to the top", sessions.json keeps its stamp, and its state stays as it was;
    the newest pin is on top; Pin to Top on the lower pinned card brings it back to the top; Unpin drops it back into the
    order and clears its stamp. (Phases 6-7 and the edge cases - the pushpin's look, a restart, other windows, /clear,
    the AI view - are not covered here.)

    Args:
        am: The app.
        s2: F-S2 (t1 and t2 in one window, both Waiting for you).
    """
    panel = am.window.side_panel
    wait_until(lambda: sorted(panel.titles()) == ["t1", "t2"], 10, what="both cards", describe=panel.titles)
    ids = {"t1": s2[0].id, "t2": s2[1].id}

    def card_menu(title):
        """Open a card's right-click menu.

        Args:
            title: The card's title.
        Returns:
            The menu.
        """
        before = Menu.open_hwnds(am)
        panel.card(title).right_click()
        return Menu.wait_new(am, before)

    def says_pinned(title):
        """Whether a card's item status carries the pin's words.

        Args:
            title: The card's title.
        Returns:
            True when "pinned to the top" is one of its facets.
        """
        return "pinned to the top" in panel.card(title).status.split("; ")

    def stamp(title):
        """The card's pin stamp as sessions.json saved it.

        Args:
            title: The card's title.
        Returns:
            The stamp, 0 when the key is absent (not pinned) or the record is not saved yet.
        """
        rec = am.profile.session(ids[title])
        return int(rec.raw.get("sidePanelPinnedAtUnixMs", 0)) if rec else 0

    # 1. The items: Pin to Top right under Jump to Tab; no Unpin on a card that is not pinned.
    upper, lower = panel.titles()[0], panel.titles()[1]
    state_before = panel.card(lower).state
    menu = card_menu(lower)
    names = menu.names()
    assert "Pin to Top" in names and names.index("Pin to Top") == names.index("Jump to Tab") + 1, names
    assert "Unpin" not in names, names
    # 2. Pin the lower card: it goes to the top and says so, sessions.json keeps its stamp, and its state is unchanged
    #    (a card pin is not Telegram's never-idle pin).
    menu.choose("Pin to Top")
    wait_until(lambda: panel.titles()[0] == lower, 5, what=f"{lower} at the top", describe=panel.titles)
    wait_until(lambda: says_pinned(lower), 5, what=f"{lower}'s item status names the pin", describe=lambda: panel.card(lower).status)
    wait_until(lambda: stamp(lower) > 0, 5, what=f"{lower}'s stamp in sessions.json")
    assert panel.card(lower).state == state_before, (panel.card(lower).state, state_before)
    assert not says_pinned(upper), panel.card(upper).status
    # 3. Pin the other card: the newest pin is on top, with the newer stamp.
    card_menu(upper).choose("Pin to Top")
    wait_until(lambda: panel.titles() == [upper, lower], 5, what="the newest pin on top", describe=panel.titles)
    wait_until(lambda: stamp(upper) > stamp(lower) > 0, 5, what="a newer stamp for the newer pin")
    # 4. Pin again: the lower pinned card offers Pin to Top and Unpin under it; Pin to Top brings it back to the top.
    menu = card_menu(lower)
    names = menu.names()
    assert "Unpin" in names and names.index("Unpin") == names.index("Pin to Top") + 1, names
    menu.choose("Pin to Top")
    wait_until(lambda: panel.titles() == [lower, upper], 5, what="a pin again back on top", describe=panel.titles)
    # 5. Unpin the top card: it drops below the remaining pin and loses its words and its stamp; the other keeps its pin.
    card_menu(lower).choose("Unpin")
    wait_until(lambda: panel.titles() == [upper, lower], 5, what="the unpinned card below the pin", describe=panel.titles)
    wait_until(lambda: not says_pinned(lower), 5, what=f"{lower}'s item status without the pin")
    wait_until(lambda: stamp(lower) == 0, 5, what=f"{lower}'s stamp gone from sessions.json")
    assert says_pinned(upper), panel.card(upper).status


@pytest.mark.p1
@pytest.mark.flow("SP-12")
@pytest.mark.needs_fix("SP-F3")
def test_card_menu_opens_next_to_its_tab(am, s2, mock):
    """SP-12 phases 1-2 (SP-F3, the user's rule of 2026-11-07 - a build with it; SIDE_PANEL.md section 2k): a card menu's
    Open New Session Here > Default opens the new session's tab right AFTER the card's own tab - between t1's tab and
    t2's, not at the end of the strip - and Fork session > Default opens "t1 (fork)" right after t1's tab too, before the
    tab the first step opened. Tabs are found by their session ids: a new session in t1's folder can derive t1's own title.
    (Phase 3, on the Agent Manager page, and the edge cases - another window's session, a Codex card, the board and the
    tree keeping the end of the strip - are not covered here.)

    Args:
        am: The app.
        s2: F-S2 (t1 launched before t2, so t1's tab is left of t2's).
        mock: The scripted model.
    """
    w = am.window
    panel = w.side_panel
    t1, t2 = s2
    wait_until(lambda: sorted(panel.titles()) == ["t1", "t2"], 10, what="both cards", describe=panel.titles)

    def index_of(session):
        """A session's tab position in this window's strip.

        Args:
            session: The Session.
        Returns:
            Its index (0 = left-most), -1 when no tab of this window hosts it.
        """
        tab = w.tabs.by_session(session.id)
        return tab.index if tab else -1

    def card_menu(title):
        """Open a card's right-click menu.

        Args:
            title: The card's title.
        Returns:
            The menu.
        """
        before = Menu.open_hwnds(am)
        panel.card(title).right_click()
        return Menu.wait_new(am, before)

    assert 0 <= index_of(t1) < index_of(t2), w.tabs.titles()
    # 1. Open New Session Here > Default on t1's card: the new tab sits right after t1's and right before t2's, and it went
    #    the card's own way (not the content's spawn, which opened at the end of the strip).
    mark = am.profile.hooks.mark()
    card_menu("t1").choose_path("Open New Session Here", "Default")
    new = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    wait_until(lambda: index_of(new) == index_of(t1) + 1, 10, what="the new session's tab right after t1's", describe=w.tabs.titles)
    assert index_of(t2) == index_of(new) + 1, w.tabs.titles()
    assert am.profile.hooks.find(r"\[nav\] sidepanel new-session next to " + t1.short, since=mark), "the card's own route was not taken"
    # 2. Fork session > Default on t1's card (t1 is at rest: no interrupted-turn question): "t1 (fork)" right after t1's tab,
    #    before the tab step 1 opened.
    mark = am.profile.hooks.mark()
    card_menu("t1").choose_path("Fork session", "Default")
    fork = wait_new_session(am, mark, pattern=r"\[nav\] fork-managed-done new=([0-9a-f]{8})", mock=mock)
    wait_until(lambda: index_of(fork) == index_of(t1) + 1, 10, what="the fork's tab right after t1's", describe=w.tabs.titles)
    assert index_of(new) == index_of(fork) + 1, w.tabs.titles()
    assert fork.record().title == "t1 (fork)"


# ---- batch 2: restart, the hover card -----------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("SP-06")
def test_panel_state_survives_a_restart(am, s1):
    """SP-06 phase 5 (F-RS): Sum off, GLOBAL and collapsed come back after a restart.

    Args:
        am: The app.
        s1: F-S.
    """
    panel = am.window.side_panel
    panel.set_sum(False)
    assert panel.toggle_scope() == "GLOBAL"
    panel.collapse()
    time.sleep(1.5)  # the window record's debounced save
    am.restart()
    panel = am.window.side_panel
    wait_until(panel.collapsed, 30, what="the panel collapsed after the restart")
    panel.expand()
    assert not panel.sum_on() and panel.scope() == "GLOBAL"


@pytest.mark.p1
@pytest.mark.flow("SP-04")
def test_sum_off_card_hover_opens_no_rich_card(am, s1):
    """SP-04 phase 2 — SP-K2, a FEATURE (the user's ruling of 2026-10-24; the suite had filed it as a known defect on
    2026-10-23): with Sum off, resting the pointer on a side-panel card opens NO rich session card, while the same
    hover on the session's TAB opens one. The tab's half is this test's control: it proves the input reaches XAML's
    tooltip service, so the card's silence is the app's behavior, not the test's. Measured on 0.8.0 and on the test
    build 0.8.1.1 (2026-10-23, runs g1full and fixall-0811): no popup window of the app over the card for the whole
    hold — although hooks.log still says "cards show the rich session tooltip" and SIDE_PANEL.md §2d describes the
    card beside the panel (the code's hover host for such cards stays in place, unused).

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    # Control: the tab header's rich card opens under the same input.
    w.tabs.get("t1").el.hover(glide=True, offset=(30, 16))
    wait_until(am.tooltips, 8, what="the tab's rich card (the control half)")
    inputs.glide(1200, 700)
    wait_until(lambda: not am.tooltips(), 5, what="the tab's card closed")
    w.side_panel.set_sum(False)
    card = w.side_panel.card("t1")
    card.title_el.hover(glide=True, offset=(40, 8))
    hold_true(lambda: not am.tooltips(), 8.0, what="no rich card over the side-panel card (SP-K2, a feature)")
