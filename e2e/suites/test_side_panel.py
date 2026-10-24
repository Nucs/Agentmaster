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
from amdriver.ui.sidepanel import COUNT_RE
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
    # 2. A click outside the menu, on the panel's count line: the popup root takes that press (it reaches no element).
    open_menu_on_t2()
    count = next(k for k in panel.controls() if k.role == "text" and COUNT_RE.match(k.name))
    count.click()
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
