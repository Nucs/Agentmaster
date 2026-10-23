"""The Agent Manager page and the Home button (doc/agentmaster/flows/01-agent-manager-page.md, MGR).

The page is reached the only way a user has: the tab strip's Home button. Its face is read from its accessible
name ("Agent Manager" / "Jump back to <title>"); the page's presence from the window title and the strip's
selection; the board and the tree from their cards and rows; the journey from hooks.log's [nav] trail.
"""
import time

import pytest

from amdriver import inputs
from amdriver.session import wait_new_session, wait_until_no_claude
from amdriver.waits import hold_true, wait_until

SHELL = "Administrator: Windows PowerShell"
NOTHING_SELECTED = ["Select a Claude session to see its summary."]
"""The right pane's text while the page's lens has no session selected (AgentManagerContent.AutoTesting.cpp)."""


@pytest.mark.p0
@pytest.mark.flow("MGR-01")
def test_open_the_page_and_leave_it(am, s2):
    """MGR-01 phases 1–3 and E1: Home shows the page with no tab selected; tab actions do nothing there; a tab
    click leaves; Ctrl+Tab / Ctrl+Shift+Tab leave onto the first / last tab.

    Args:
        am: The app.
        s2: F-S2 (the shell, t1, t2).
    """
    w = am.window
    mark = am.profile.hooks.mark()
    page = w.manager.open()
    assert w.title == "Agent Manager"
    assert w.tabs.selected() is None
    assert page.board_view() is not None and page.tree_rows()
    am.profile.hooks.wait(r"\[nav\] manager-page show \(home\)", since=mark)
    # 2. A tab action has no tab to act on.
    inputs.keys("ctrl+shift+w")
    hold_true(lambda: w.tabs.titles() == [SHELL, "t1", "t2"], 2.0, what="no tab closed on the page")
    assert page.showing()
    # 3. Leave by a tab.
    mark = am.profile.hooks.mark()
    w.tabs.get("t2").click()
    w.tabs.wait_selected("t2")
    page.wait_hidden()
    am.profile.hooks.wait(r"\[nav\] manager-page hide", since=mark)
    # E1: the tab cycle never lands back on the page.
    page.open()
    inputs.keys("ctrl+tab")
    w.tabs.wait_selected(SHELL)
    page.open()
    inputs.keys("ctrl+shift+tab")
    w.tabs.wait_selected("t2")


@pytest.mark.p0
@pytest.mark.flow("MGR-02")
def test_home_turns_into_the_way_back(am, s2):
    """MGR-02 phases 1–4: Home names the tab it returns to, goes back, retargets with a card, and stays Home
    with nothing to return to.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    w = am.window
    page = w.manager
    a.select()
    assert page.home_name() == "Agent Manager"
    page.open()
    wait_until(lambda: page.home_name() == "Jump back to t1", 5, what="Home's back arrow", describe=page.home_name)
    # 2. Back.
    mark = am.profile.hooks.mark()
    page.click_home()
    w.tabs.wait_selected("t1")
    wait_until(lambda: page.home_name() == "Agent Manager", 5, what="Home's own face", describe=page.home_name)
    am.profile.hooks.wait(rf"\[nav\] home-button jump-back -> {a.short}", since=mark)
    # 3. Retarget with t2's card.
    page.open()
    page.card("t2").click()
    wait_until(lambda: page.home_name() == "Jump back to t2", 5, what="Home retargeted", describe=page.home_name)
    # 4. Clear: nothing to go back to — Home, and a click does nothing.
    page.clear()
    wait_until(lambda: page.home_name() == "Agent Manager", 5, what="Home after Clear", describe=page.home_name)
    time.sleep(0.6)  # past the double-click guard of the last click
    page.click_home()
    hold_true(page.showing, 1.5, what="the page stays after a Home click with nothing to go back to")


@pytest.mark.p1
@pytest.mark.flow("MGR-02")
def test_double_click_home_opens_the_page_and_stays(am, s1):
    """MGR-02 E1: a double-click on Home from a tab opens the page; the second click is ignored, logged.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    s1.select()
    mark = am.profile.hooks.mark()
    w.manager.click_home(count=2)
    w.manager.wait_showing()
    hold_true(w.manager.showing, 1.5, what="the page stays after the double-click")
    am.profile.hooks.wait(r"\[nav\] home-button click ignored", since=mark)


@pytest.mark.p0
@pytest.mark.flow("MGR-03")
def test_closing_the_last_tab_keeps_the_window_on_its_page(am, s1):
    """MGR-03 phase 2: closing every tab of the only window leaves it open on its page.

    Args:
        am: The app.
        s1: F-S (the shell and t1).
    """
    w = am.window
    w.tabs.close(SHELL)  # "Close tab?" -> Close
    w.tabs.close("t1")  # "Close session?" -> Close
    w.manager.wait_showing()
    assert w.tabs.items() == []
    hold_true(lambda: am.alive() and w.alive(), 3.0, what="the last window stays open")
    wait_until_no_claude(am, s1)


@pytest.mark.p0
@pytest.mark.flow("MGR-03")
def test_deactivating_the_focused_tab_shows_the_page_first(am, s1):
    """MGR-03 phase 3: Deactivate ▸ Deactivate Tab on the focused tab shows the page (a never-started control on
    screen would start its claude again) and the tab stays parked.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    s1.select()
    w.tabs.context_menu("t1").choose_path("Deactivate", "Deactivate Tab")
    w.manager.wait_showing()
    wait_until_no_claude(am, s1)
    hold_true(lambda: not s1.claude_pids(), 3.0, what="the parked tab stays parked")
    assert w.tabs.find("t1") is not None


@pytest.mark.p1
@pytest.mark.flow("MGR-05")
def test_linked_lenses(am, s2):
    """MGR-05 phases 1, 3 and 4: the tab you come from is the page's selection; a card double-click jumps to its
    tab; the board's LOCAL/GLOBAL is the tree's too.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    a.ask('alpha words [[mock reply "a"]]')
    w = am.window
    page = w.manager
    b.select()  # a real tab SWITCH (t1 was selected): the lens follows it
    page.open()
    wait_until(lambda: page.home_name() == "Jump back to t2", 5, what="Home's way back to t2",
               describe=page.home_name)
    a.select()
    page.open()
    wait_until(lambda: any("alpha words" in t for t in page.right_pane_texts()), 10,
               what="t1's summary in the right pane", describe=page.right_pane_texts)
    # 3. A card double-click jumps to its tab.
    page.card("t1").double_click()
    w.tabs.wait_selected("t1")
    page.wait_hidden()
    # 4. One scope for the board and the tree.
    page.open()
    assert page.board_scope() == "LOCAL" and page.tree_scope() == "LOCAL"
    assert page.toggle_board_scope() == "GLOBAL"
    wait_until(lambda: page.tree_scope() == "GLOBAL", 5, what="the tree's scope to follow")
    assert page.toggle_board_scope() == "LOCAL"
    wait_until(lambda: page.tree_scope() == "LOCAL", 5, what="the tree's scope back")


@pytest.mark.p1
@pytest.mark.flow("MGR-05")
def test_board_follows_the_session_state(am, s1):
    """The board's card moves between columns with the session's state (a held turn: Running, then Waiting).

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    s1.ask('long one [[mock say "working"; hold g; reply "done"]]', wait="sent")
    am.mock.wait_holding("g", timeout_s=30)
    page = w.manager.open()
    page.wait_card("t1", column="Running", timeout_s=20)
    assert page.tree_row("t1").state == "running"
    am.mock.release("g")
    page.wait_card("t1", column="Waiting-for-you", timeout_s=30)
    wait_until(lambda: page.tree_row("t1").state == "waiting-for-you", 10, what="the tree row's state")


# ---- known defects ----------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("MGR-07")
@pytest.mark.needs_fix("MGR-K1")
def test_a_launch_from_the_page_stays_on_the_page(am, s1, mock):
    """MGR-07 phases 1–4 (MGR-K1's fix, 2026-10-23 — the user's "make selected but do not go to the tab"): a session
    launched from the page's launch row opens its tab BEHIND the page, starts its claude there and is selected on the
    page, as if its card had been clicked — Home turns into "Jump back to t2", the right pane leaves its "select a
    session" hint — and nothing goes to the tab; Home then jumps to it, where Claude Code's box waits.

    On 0.8.0 (MGR-K1) the launch switched to the new tab and the page kept no selection of it.

    Args:
        am: The app.
        s1: F-S (t1 on screen; the launch row starts from the page).
        mock: The scripted model (t2's first turn).
    """
    w = am.window
    page = w.manager.open()
    mark = am.profile.hooks.mark()
    page.launch(str(am.iso.folder("t2")))  # one click on Launch, its hover menu open (the driver rests on it)
    new = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    am.profile.hooks.wait(rf"\[nav\] manager-page launch {new.short} \(new session\): started in place, selected on the page",
                          since=mark, timeout_s=20)
    # 1. The page stayed: its title, no tab selected, and the new tab in the strip behind it.
    assert page.showing() and w.title == "Agent Manager"
    assert w.tabs.selected() is None
    assert "t2" in w.tabs.titles()
    # 2. The new session is the page's selection: Home's way back names it, the right pane is about it.
    wait_until(lambda: page.home_name() == "Jump back to t2", 10, what="Home's way back to t2",
               describe=page.home_name)
    wait_until(lambda: page.right_pane_texts() not in ([], NOTHING_SELECTED), 10, what="t2 shown in the right pane",
               describe=page.right_pane_texts)
    # 3. Its claude started behind the page, and nothing went to the tab — not even later.
    wait_until(new.claude_pids, 30, what="t2's claude, started in place behind the page")
    hold_true(lambda: page.showing() and w.tabs.selected() is None, 2.0, what="the page staying on screen")
    assert not am.profile.hooks.find(rf"\[nav\] tab-focus {new.short}", since=mark)
    # 4. Home leads to it; the started claude's box is there, and it works.
    page.click_home()
    w.tabs.wait_selected("t2")
    page.wait_hidden()
    am.profile.hooks.wait(rf"\[nav\] home-button jump-back -> {new.short}", since=mark)
    w.terminal.wait_input_box()
    new.ask('second ready [[mock reply "ok"]]')


@pytest.mark.p1
@pytest.mark.flow("MGR-07")
@pytest.mark.needs_fix("MGR-K1")
def test_a_plus_card_launch_follows_where_the_user_looks(am, s1, mock):
    """MGR-07 phases 5–6: the side panel's + card launches the page's way. Over the PAGE the new session stays behind
    it and is selected there (the row's behavior); over a TAB its tab opens in front and the page's lens follows it,
    so the page opened later offers the way back to it (MGR-K1's other half: on 0.8.0 the lens kept the previous tab's
    session, because the launch selected the new tab before the tab was mapped to its session).

    Launch is pressed with Enter (focused, on the key-up): this test is about where the launch goes; one CLICK with
    the presets menu open is SP-05's test_plus_card_launch_takes_one_click_with_its_menu_open.

    Args:
        am: The app.
        s1: F-S (t1).
        mock: The scripted model.
    """
    w = am.window
    page = w.manager
    panel = w.side_panel
    # 5. Over the page.
    page.open()
    mark = am.profile.hooks.mark()
    card = panel.open_launcher()
    card.type_folder(str(am.iso.folder("t3")))
    card.launch_button().focus()
    inputs.keys("enter")
    t3 = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    am.profile.hooks.wait(rf"\[nav\] manager-page launch {t3.short} \(plus-card\): started in place, selected on the page",
                          since=mark, timeout_s=20)
    wait_until(lambda: not card.is_open(), 5, what="the card closes after the launch")
    assert page.showing() and w.tabs.selected() is None
    wait_until(lambda: page.home_name() == "Jump back to t3", 10, what="Home's way back to t3",
               describe=page.home_name)
    wait_until(t3.claude_pids, 30, what="t3's claude, started in place behind the page")
    # 6. Over a tab: t1 in front, then a launch from the card selects its new tab, and the lens follows it.
    s1.select()
    page.wait_hidden()
    mark = am.profile.hooks.mark()
    card = panel.open_launcher()
    card.type_folder(str(am.iso.folder("t4")))
    card.launch_button().focus()
    inputs.keys("enter")
    t4 = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    w.tabs.wait_selected("t4")
    am.profile.hooks.wait(rf"\[nav\] tab-focus {t4.short}", since=mark, timeout_s=10)
    assert not am.profile.hooks.find(rf"\[nav\] manager-page launch {t4.short}", since=mark)
    page.open()
    wait_until(lambda: page.home_name() == "Jump back to t4", 10, what="Home's way back to the launched t4",
               describe=page.home_name)


# ---- batch 2: restart, panes ---------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("MGR-03")
def test_a_page_only_last_window_is_not_reopened(am, s1):
    """MGR-03 phase 2 (the restart half): the last window, emptied to its page, is not reopened as an empty window —
    the next start opens a fresh default window.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    w.tabs.close(SHELL)
    w.tabs.close("t1")
    w.manager.wait_showing()
    am.restart()
    w = am.window
    wait_until(lambda: w.tabs.titles() == [SHELL], 30, what="a fresh default window with one shell tab",
               describe=w.tabs.titles)
    assert not w.manager.showing()


@pytest.mark.p1
@pytest.mark.flow("MGR-04")
def test_alt_left_and_ctrl_home_are_windows_terminals(am, s1):
    """MGR-04 phases 1–2: in a split shell tab Alt+Left moves the focus to the left pane, and Ctrl+Home reaches the
    shell — neither opens the Agent Manager page.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    w.tabs.select(SHELL)
    w.tabs.context_menu(SHELL).choose("Split tab")

    def panes():
        """The window's visible terminals, left to right.

        Returns:
            The TermControl elements.
        """
        return sorted((e for e in w.content().find_all(cls="TermControl", max_depth=8) if e.visible),
                      key=lambda e: e.rect[0])

    wait_until(lambda: len(panes()) == 2, 15, what="two panes", describe=lambda: [p.rect for p in panes()])
    left, right = panes()
    inputs.click(right.rect[0] + 40, right.rect[1] + right.rect[3] - 40)
    wait_until(lambda: panes()[1].has_focus, 5, what="the right pane focused")
    inputs.keys("alt+left")
    wait_until(lambda: panes()[0].has_focus, 5, what="Alt+Left focusing the left pane")
    assert not w.manager.showing()
    inputs.keys("ctrl+home")
    hold_true(lambda: not w.manager.showing(), 1.5, what="Ctrl+Home not opening the page")
