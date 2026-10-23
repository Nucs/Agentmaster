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


@pytest.mark.p1
@pytest.mark.flow("SP-05")
@pytest.mark.known_defect("SP-K1")
def test_known_defect_plus_card_launch_needs_a_second_click(am, s1):
    """SP-K1 (expected failure once fixed): in the + card, with Launch's presets menu open (the pointer rests on
    Launch), ONE click does not launch: it only closes the menu; the second click launches. The Agent Manager
    page's own launch row launches with one click in the same situation (0.8.0's fix; checked by the suite's
    launch path, launch_session). Measured 2026-10-23 on 0.8.0 with a held (60 ms) click.

    The test asserts TODAY's behavior; when it starts failing, the defect is fixed: drop this test and launch with
    a click in test_plus_launcher_card.

    Args:
        am: The app.
        s1: F-S.
    """
    from amdriver.ui.menus import Menu
    panel = am.window.side_panel
    card = panel.open_launcher()
    card.type_folder(str(am.iso.folder("t2")))
    button = card.launch_button()
    button.hover()
    wait_until(lambda: Menu.open_menus(am), 5, what="Launch's presets menu (it opens on hover)")
    mark = am.profile.hooks.mark()
    card.launch_button().click()
    time.sleep(2.0)
    assert not am.profile.hooks.find(r"\[nav\] open-new claude", since=mark)  # expected: launched
    assert card.is_open() and not Menu.open_menus(am)  # the click only closed the presets menu
    card.launch_button().click()
    am.profile.hooks.wait(r"\[nav\] open-new claude done", since=mark, timeout_s=20)


@pytest.mark.p1
@pytest.mark.flow("SP-06")
def test_collapse_and_dock(am, s1):
    """SP-06 phases 3 and 4: « collapses the panel to its strip and » restores it; ⇄ docks it on the other
    side and back.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    panel = w.side_panel
    assert panel.dock_side() == "left"
    panel.button("«").click()
    wait_until(panel.collapsed, 5, what="the panel collapsed")
    # The strip's chevron: » on 0.8.0, "Expand the side panel" (am.side.expand) on a build with the accessibility
    # metadata — SidePanel.strip_button finds either.
    expand = wait_until(panel.strip_button, 5, what="the collapsed strip's chevron")
    expand.click()
    wait_until(lambda: not panel.collapsed(), 5, what="the panel back")
    panel.button("⇄").click()
    wait_until(lambda: panel.dock_side() == "right", 5, what="docked right", describe=panel.dock_side)
    panel.button("⇄").click()
    wait_until(lambda: panel.dock_side() == "left", 5, what="docked left again", describe=panel.dock_side)


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
@pytest.mark.known_defect("SP-K2")
def test_known_defect_sum_off_card_hover_opens_no_rich_card(am, s1):
    """SP-K2 (expected failure once fixed): with Sum off, resting the pointer on a side-panel card opens NO rich
    session card, while the same hover on the session's TAB opens one (the control half of this test). Measured on
    0.8.0 in the test desktop (2026-10-23): nothing for 9 s, polled every 0.3 s; hooks.log says "cards show the rich
    session tooltip".

    The test asserts TODAY's behavior; when it starts failing, the defect is fixed: assert the card beside the panel.

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
    hold_true(lambda: not am.tooltips(), 8.0, what="no rich card over the side-panel card (expected: one)")
