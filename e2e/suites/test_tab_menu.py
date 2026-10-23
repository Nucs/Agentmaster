"""The tab's context menu (doc/agentmaster/flows/23-tab-context-menu.md, TCM).

Every test right-clicks a tab and works the menu the way a user does: hover a ▸ to open a submenu, click an item.
The checks read the menu itself (UI Automation: what is shown, what is enabled), the engine's record
(sessions.json, the session store), the log's [nav] trail, the clipboard, the processes and the mock.
"""
import time

import pytest

from amdriver import inputs, win32
from amdriver.session import wait_new_session, wait_until_no_claude
from amdriver.ui.dialogs import Dialog
from amdriver.ui.menus import Menu
from amdriver.waits import wait_until

SHELL = "Administrator: Windows PowerShell"
"""The default tab of a fresh window in the guest (Windows PowerShell; the guest's user is an administrator)."""

SESSION_MENU = [
    "Rename tab (F2)", "Copy", "Show Transcript", "Mark Unread", "Open In Explorer", "Move to Idle/Done",
    "Favorite", "Tags", "Split tab", "Move tab", "Change tab color", "Export text", "Find", "New Session Here",
    "Restart session", "Fork session", "Deactivate", "Close", "Close tab",
]
SHELL_MENU = [
    "Rename tab (F2)", "Split tab", "Move tab", "Change tab color", "Export text", "Find", "New Session Here",
    "Restart session", "Fork session", "Close", "Close tab",
]
COPY_MENU = ["Session Id", "Copy Path", "Copy Branch Name", "Copy Current Prompt", "Claude Launch CLI", "Summary",
             "Transcript"]
TRANSCRIPT_MENU = ["Show", "Default", "Max", "Full", "Compact", "Minimal"]
DEACTIVATE_MENU = ["Deactivate Tab", "Tabs to the Left", "Tabs to the Right", "Other Tabs", "Of Same Folder",
                   "Other of Same Folder", "All Tabs"]
CLOSE_MENU_SESSION = ["Close tabs to the left", "Close tabs to the right", "Close other tabs", "Of Same Folder",
                      "Other of Same Folder", "Close all tabs", "★ Favorite & close all tabs"]
"""Close ▸ on a session tab with one pane ("Close pane" shows only when the tab is split)."""
MOVE_MENU = ["Move tab to new window", "Move right", "Move left", "Move to start", "Move to end"]
SENTINEL = "e2e-clipboard-sentinel"


def _copy(am, title: str, *path: str) -> str:
    """Choose a Copy ▸ item on a tab and return what reached the clipboard.

    Args:
        am: The app.
        title: The tab.
        *path: The submenu path under Copy ▸ (e.g. "Session Id", or "Transcript", "Default").
    Returns:
        The clipboard's text once it changed.
    Raises:
        WaitTimeout: When the clipboard did not change.
    """
    win32.set_clipboard_text(SENTINEL)
    am.window.tabs.context_menu(title).choose_path("Copy", *path)
    return wait_until(lambda: (lambda t: t if t != SENTINEL else None)(win32.clipboard_text()), 15.0,
                      what=f"the clipboard after Copy ▸ {' ▸ '.join(path)}")


# ---- TCM-01 -------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TCM-01")
def test_menu_fits_the_tab(am, s1):
    """TCM-01 phases 1–3: the session tab's menu, the shell tab's, and a dormant tab's.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    # 1. The session tab, Waiting for you: every session item, in order.
    menu = w.tabs.context_menu("t1")
    assert menu.names() == SESSION_MENU
    assert [i.name for i in menu.items() if i.is_sub] == ["Copy", "Move tab", "New Session Here", "Fork session",
                                                           "Deactivate", "Close"]
    copy = menu.submenu("Copy")
    assert copy.names() == COPY_MENU
    assert copy.submenu("Transcript").names() == TRANSCRIPT_MENU
    menu.close()
    menu = w.tabs.context_menu("t1")
    assert menu.submenu("Deactivate").names() == DEACTIVATE_MENU
    menu.close()
    menu = w.tabs.context_menu("t1")
    close = menu.submenu("Close")
    assert close.names() == CLOSE_MENU_SESSION
    menu.close()
    menu = w.tabs.context_menu("t1")
    assert menu.submenu("Move tab").names() == MOVE_MENU
    menu.close()
    menu = w.tabs.context_menu("t1")
    fork = menu.submenu("Fork session")
    assert fork.names()[:2] == ["Default", "Specify…"] and "Add presets…" in fork.names()
    menu.close()

    # 2. The shell tab: no session item, no Deactivate; Fork session is Windows Terminal's plain duplicate.
    menu = w.tabs.context_menu(SHELL)
    assert menu.names() == SHELL_MENU
    assert not menu.item("Fork session").is_sub and menu.item("New Session Here").is_sub
    close = menu.submenu("Close")
    assert "Of Same Folder" not in close.names() and "Other of Same Folder" not in close.names()
    assert "★ Favorite & close all tabs" in close.names()  # the WINDOW holds a managed session
    menu.close()

    # 3. Dormant: Deactivate Tab parks it; the menu then leads with Activate and grays Deactivate Tab.
    pid_before = s1.claude_pids()
    assert pid_before, "the session's claude.exe is not running"
    w.tabs.context_menu("t1").choose_path("Deactivate", "Deactivate Tab")
    wait_until_no_claude(am, s1)
    menu = w.tabs.context_menu("t1")
    assert menu.names()[0] == "Activate Tab (Shift+Click)"
    deactivate = menu.submenu("Deactivate")
    assert not deactivate.enabled("Deactivate Tab")
    assert deactivate.enabled("All Tabs") and deactivate.enabled("Of Same Folder")
    menu.close()
    # Activate starts it in place: a new claude, the same conversation.
    w.tabs.context_menu("t1").choose("Activate Tab (Shift+Click)")
    wait_until(lambda: s1.claude_pids(), 30, what="the activated session's claude")
    assert s1.claude_pids() != pid_before


@pytest.mark.p1
@pytest.mark.flow("TCM-01")
def test_triage_item_follows_the_state(am, s1):
    """TCM-01 E1 + TCM-09 phase 5: the triage item's direction follows the session's state, and moves it.

    Args:
        am: The app.
        s1: F-S (Waiting for you).
    """
    w = am.window
    pid = s1.claude_pids()
    w.tabs.context_menu("t1").choose("Move to Idle/Done")
    s1.wait_state("Idle", timeout_s=10)
    menu = w.tabs.context_menu("t1")
    assert "Move to Waiting-for-you" in menu.names() and "Move to Idle/Done" not in menu.names()
    menu.choose("Move to Waiting-for-you")
    s1.wait_state("WaitingForInput", timeout_s=10)
    assert s1.claude_pids() == pid  # the triage move never touches the claude
    # Running: the item is hidden while a turn runs (a held turn, released afterwards).
    s1.ask('hold on [[mock say "working"; hold g1; reply "done"]]', wait="sent")
    am.mock.wait_holding("g1", timeout_s=30)
    s1.wait_state("Running", timeout_s=15)
    menu = w.tabs.context_menu("t1")
    assert not any(n.startswith("Move to ") for n in menu.names()), menu.names()
    menu.close()
    am.mock.release("g1")
    s1.wait_state("WaitingForInput", timeout_s=30)


# ---- TCM-02 -------------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("TCM-02")
def test_position_decides_what_is_enabled(am, s2):
    """TCM-02: the left-most, right-most and middle tabs gray exactly the items that could reach no tab.

    Args:
        am: The app.
        s2: F-S2 (tabs: the shell, t1, t2).
    """
    w = am.window
    assert w.tabs.titles() == [SHELL, "t1", "t2"]

    def states(title: str, sub: str) -> dict:
        """The enabled state of a submenu's items on one tab.

        Args:
            title: The tab.
            sub: The submenu.
        Returns:
            {item: enabled}.
        """
        menu = w.tabs.context_menu(title)
        try:
            return {i.name: i.enabled for i in menu.submenu(sub).items()}
        finally:
            Menu.dismiss_all(am)

    left_close, right_close = states(SHELL, "Close"), states("t2", "Close")
    assert not left_close["Close tabs to the left"] and left_close["Close tabs to the right"]
    assert right_close["Close tabs to the left"] and not right_close["Close tabs to the right"]
    left_move, right_move, mid_move = states(SHELL, "Move tab"), states("t2", "Move tab"), states("t1", "Move tab")
    assert not left_move["Move left"] and not left_move["Move to start"]
    assert left_move["Move right"] and left_move["Move to end"]
    assert not right_move["Move right"] and not right_move["Move to end"]
    assert right_move["Move left"] and right_move["Move to start"]
    assert all(mid_move.values()), mid_move
    mid_deactivate, right_deactivate = states("t1", "Deactivate"), states("t2", "Deactivate")
    assert all(mid_deactivate.values()), mid_deactivate
    assert not right_deactivate["Tabs to the Right"] and right_deactivate["Tabs to the Left"]


# ---- TCM-03 -------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TCM-03")
def test_rename_renames_the_session_everywhere(am, s1):
    """TCM-03 phase 1 and edges E2–E3: one title on every surface; surrounding spaces trimmed; Esc cancels.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    mark = am.profile.hooks.mark()
    w.tabs.rename("t1", "alpha")
    w.tabs.wait_tab("alpha")
    w.wait_title("alpha")
    am.profile.wait_session(lambda s: s.id == s1.id and s.title == "alpha", 10, what="the renamed title on disk")
    w.side_panel.wait_card("alpha")
    am.profile.hooks.wait(r"\[nav\] rename", since=mark)
    page = w.manager.open()
    page.wait_card("alpha")
    s1.select()
    # E2: surrounding spaces are trimmed.
    w.tabs.rename("alpha", "  beta  ")
    w.tabs.wait_tab("beta")
    am.profile.wait_session(lambda s: s.id == s1.id and s.title == "beta", 10, what="the trimmed title on disk")
    # E3: Esc cancels.
    w.tabs.context_menu("beta").choose("Rename tab (F2)")
    box = wait_until(w.tabs.rename_box, 5.0, what="the rename box")
    box.focus()
    inputs.keys("ctrl+a")
    inputs.type_text("gamma")
    inputs.keys("esc")
    wait_until(lambda: w.tabs.rename_box() is None, 5.0, what="the box to close")
    time.sleep(1.5)  # a wrong commit would show by now
    assert w.tabs.find("beta") is not None and s1.title == "beta"


@pytest.mark.p1
@pytest.mark.flow("TCM-03")
@pytest.mark.known_defect("TCM-K1")
def test_known_defect_blank_rename_shows_claudes_own_title(am, s1):
    """TCM-K1 (expected failure once fixed): a blank rename is refused for the SESSION (sessions.json keeps the title)
    but the TAB then shows Claude Code's own terminal title ("✳ <Claude's session title>") instead of the session's —
    the one-title rule (CLAUDE.md Rule #11) broken. `_SyncClaudeTitleFromTab` is meant to re-pin the managed name on
    an emptied rename; measured on 0.8.0 (2026-10-23), the tab is left floating with the OSC title.

    The test asserts TODAY's behavior; when it starts failing, the defect is fixed: assert the tab reads "alpha".

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    w.tabs.rename("t1", "alpha")
    am.profile.wait_session(lambda s: s.id == s1.id and s.title == "alpha", 10, what="the renamed title on disk")
    w.tabs.context_menu("alpha").choose("Rename tab (F2)")
    box = wait_until(w.tabs.rename_box, 5.0, what="the rename box")
    box.focus()
    inputs.keys("ctrl+a", "delete", "shift+enter")
    wait_until(lambda: w.tabs.rename_box() is None, 5.0, what="the box to close")
    time.sleep(1.5)  # the re-pin would have happened by now
    assert s1.title == "alpha"  # the session's title is kept (correct)
    tab_title = w.tabs.items()[1].title
    assert tab_title != "alpha" and "Mock session title" in tab_title, tab_title  # expected: "alpha"


# ---- TCM-04 -------------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("TCM-04")
def test_copy_puts_the_sessions_facts_on_the_clipboard(am, mock):
    """TCM-04: each Copy ▸ item copies one fact of the session.

    Args:
        am: The app.
        mock: The scripted model.
    """
    folder = am.iso.git_folder("t1", branch="feature/x")
    s1 = am.launch("t1")
    assert str(s1.folder) == str(folder)
    w = am.window
    mark = am.profile.hooks.mark()
    assert _copy(am, "t1", "Session Id") == s1.id
    assert _copy(am, "t1", "Copy Path").rstrip("\\") == str(folder)
    assert _copy(am, "t1", "Copy Branch Name") == "feature/x"
    cli = _copy(am, "t1", "Claude Launch CLI")
    assert cli.startswith("& ") and "claude.exe" in cli.lower() and s1.id in cli and "--settings" in cli
    summary = _copy(am, "t1", "Summary")
    assert s1.id in summary and "ready" in summary
    transcript = _copy(am, "t1", "Transcript", "Default")
    assert "ready" in transcript
    # The unsent draft: typed, not sent; the copy reads it live and leaves it in the box.
    w.terminal.focus()
    w.terminal.type("draft text")
    wait_until(lambda: (w.terminal.input_box() or None) and w.terminal.input_box().draft == "draft text", 10,
               what="the draft in the box")
    assert _copy(am, "t1", "Copy Current Prompt") == "draft text"
    assert w.terminal.input_box().draft == "draft text"
    fields = [m.group(1) for m in am.profile.hooks.find(r"\[nav\] copy ([a-z-]+)", since=mark)]
    for field in ("session-id", "path", "branch", "claude-cli", "summary", "transcript-default", "current-prompt"):
        assert field in fields, fields


# ---- TCM-05 -------------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("TCM-05")
def test_show_transcript_mark_unread_favorite(am, s2):
    """TCM-05 phases 1, 2 and 4: Show Transcript opens and Esc closes; Mark Unread is logged; Favorite flips.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    w = am.window
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t1").choose("Show Transcript")
    am.profile.hooks.wait(rf"\[nav\] transcript-show {a.short}", since=mark)
    time.sleep(1.0)
    inputs.keys("esc")
    am.profile.hooks.wait(rf"\[nav\] transcript-close {a.short} \(esc", since=mark)
    # Mark Unread on t1 while t2 is selected.
    b.select()
    w.tabs.context_menu("t1").choose("Mark Unread")
    am.profile.hooks.wait(r"\[nav\] mark-unread", since=mark)
    # Favorite: the label flips, the store records it, Unfavorite reverses.
    w.tabs.context_menu("t2").choose("Favorite")
    wait_until(lambda: am.profile.session_store(b.id).get("favorite") in (True, "true", "1", 1), 10,
               what="the star in the session store", describe=lambda: am.profile.session_store(b.id))
    menu = w.tabs.context_menu("t2")
    assert "Unfavorite" in menu.names() and "Favorite" not in menu.names()
    menu.choose("Unfavorite")
    wait_until(lambda: am.profile.session_store(b.id).get("favorite") in (None, False, "false", "0", 0), 10,
               what="the star removed", describe=lambda: am.profile.session_store(b.id))


@pytest.mark.p1
@pytest.mark.flow("TCM-05")
def test_open_in_explorer_opens_the_folder(am, s1):
    """TCM-05 phase 3: Open In Explorer opens a File Explorer window on the session's folder.

    Args:
        am: The app.
        s1: F-S.
    """
    before = {w.hwnd for w in win32.top_windows() if w.cls == "CabinetWClass"}
    mark = am.profile.hooks.mark()
    am.window.tabs.context_menu("t1").choose("Open In Explorer")
    am.profile.hooks.wait(r"\[nav\] open-path", since=mark)
    new = wait_until(lambda: [w for w in win32.top_windows() if w.cls == "CabinetWClass" and w.hwnd not in before],
                     15, what="a File Explorer window")
    try:
        # File Explorer titles its window "File Explorer" while it loads, then by the folder.
        wait_until(lambda: any(x.title.startswith("t1") for x in win32.top_windows() if x.hwnd == new[0].hwnd), 15,
                   what="the Explorer window titled by the folder",
                   describe=lambda: [x.title for x in win32.top_windows() if x.hwnd == new[0].hwnd])
    finally:
        for w in new:
            win32.post_close(w.hwnd)


# ---- TCM-06 -------------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("TCM-06")
def test_move_tab_reorders_the_strip(am, s2):
    """TCM-06 phases 1–2: Move to start / end / left / right on a tab.

    Args:
        am: The app.
        s2: F-S2 (the shell, t1, t2).
    """
    w = am.window
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t2").choose_path("Move tab", "Move to start")
    wait_until(lambda: w.tabs.titles() == ["t2", SHELL, "t1"], 5, what="t2 first", describe=w.tabs.titles)
    am.profile.hooks.wait(r"\[nav\] tab-move .*idx=2 -> 0", since=mark)
    assert w.tabs.selected().title == "t2"  # it was selected and stays so
    w.tabs.context_menu("t2").choose_path("Move tab", "Move to end")
    wait_until(lambda: w.tabs.titles() == [SHELL, "t1", "t2"], 5, what="t2 last", describe=w.tabs.titles)
    w.tabs.context_menu("t2").choose_path("Move tab", "Move left")
    wait_until(lambda: w.tabs.titles() == [SHELL, "t2", "t1"], 5, what="t2 one left", describe=w.tabs.titles)
    w.tabs.context_menu("t2").choose_path("Move tab", "Move right")
    wait_until(lambda: w.tabs.titles() == [SHELL, "t1", "t2"], 5, what="t2 back", describe=w.tabs.titles)


# ---- TCM-07 -------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TCM-07")
def test_fork_session_branches_the_conversation(am, s1, mock):
    """TCM-07 phase 2 and E1: Fork session ▸ Default opens "t1 (fork)" next to the source, carrying the
    conversation; a fork of the fork is "(fork 2)".

    Args:
        am: The app.
        s1: F-S.
        mock: The scripted model.
    """
    w = am.window
    s1.ask('first words [[mock reply "noted"]]')
    turns_before = len(am.claude.user_prompts(s1.id))
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t1").choose_path("Fork session", "Default")
    fork = wait_new_session(am, mark, pattern=r"\[nav\] fork-managed-done new=([0-9a-f]{8})", mock=mock)
    assert fork.id != s1.id
    assert fork.record().title == "t1 (fork)"
    w.tabs.wait_selected("t1 (fork)")
    assert w.tabs.titles().index("t1 (fork)") == w.tabs.titles().index("t1") + 1
    w.terminal.wait_input_box()
    seq = mock.last_seq()
    fork.ask('after the fork [[mock reply "forked"]]')
    first = [e for e in mock.requests(session=fork.id, since=seq, kind="main")][0]
    assert first["n_msgs"] > 1, first  # the fork's model call carries the source's conversation
    assert len(am.claude.user_prompts(s1.id)) == turns_before  # the source is untouched
    # E1: a fork of the fork.
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t1 (fork)").choose_path("Fork session", "Default")
    fork2 = wait_new_session(am, mark, pattern=r"\[nav\] fork-managed-done new=([0-9a-f]{8})", mock=mock)
    assert fork2.record().title == "t1 (fork 2)"


@pytest.mark.p0
@pytest.mark.flow("TCM-07")
def test_new_session_here_starts_a_fresh_session(am, s1, mock):
    """TCM-07 phase 1: New Session Here ▸ Default starts a new, independent session in the same folder.

    Args:
        am: The app.
        s1: F-S.
        mock: The scripted model.
    """
    w = am.window
    first_turn_of_s1 = [e for e in mock.requests(session=s1.id, kind="main") if e.get("k", 0) == 0][0]
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t1").choose_path("New Session Here", "Default")
    new = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    assert new.id != s1.id and new.record().working_dir.rstrip("\\") == str(s1.folder)
    assert len([t for t in w.tabs.titles() if t == "t1"]) == 2  # same folder, same derived title
    w.terminal.wait_input_box()
    seq = mock.last_seq()
    w.terminal.submit('fresh start [[mock reply "hi"]]')
    first = mock.wait_prompt("fresh start", since=seq)
    # A fresh conversation: its first request is as long as t1's FIRST request was (nothing of t1's history).
    assert first["session"] == new.id and first["n_msgs"] == first_turn_of_s1["n_msgs"], (first, first_turn_of_s1)


# ---- TCM-08 -------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TCM-08")
def test_restart_session_resumes_the_same_conversation(am, s1, mock):
    """TCM-08: Restart session ends the claude and resumes the same conversation in the same tab.

    Args:
        am: The app.
        s1: F-S.
        mock: The scripted model.
    """
    w = am.window
    pids = s1.claude_pids()
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t1").choose("Restart session")
    # The tab menu's Restart logs the mechanism line (the board's / tree's Restart adds [nav] restart-begin/done).
    am.profile.hooks.wait(rf"\[restart\] claude {s1.id} \(resume\)", since=mark, timeout_s=30)
    wait_until(lambda: s1.claude_pids() and s1.claude_pids() != pids, 30, what="a new claude for the session")
    w.terminal.wait_input_box()
    assert w.tabs.selected().title == "t1"
    seq = mock.last_seq()
    s1.ask('after restart [[mock reply "still here"]]')
    first = [e for e in mock.requests(session=s1.id, since=seq, kind="main")][0]
    assert first["n_msgs"] > 1  # the resumed conversation came along
    time.sleep(3)
    assert s1.record().id == s1.id and w.tabs.find("t1") is not None  # not archived afterwards


# ---- TCM-09 -------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TCM-09")
def test_close_tab_asks_and_keeps_the_session(am, s2):
    """TCM-09 phases 1–3: Close tab asks; Cancel keeps; Close archives (record and transcript kept);
    ★ Favorite & Close also stars.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    w = am.window
    w.tabs.context_menu("t1").choose("Close tab")
    dlg = Dialog.wait(am, title="Close session?")
    assert dlg.buttons() == ["Close", "★ Favorite & Close", "Cancel"]
    dlg.press("Cancel")
    assert w.tabs.find("t1") is not None and a.claude_pids()
    mark = am.profile.hooks.mark()
    w.tabs.context_menu("t1").choose("Close tab")
    Dialog.wait(am, title="Close session?").press("Close")
    w.tabs.wait_gone("t1")
    am.profile.hooks.wait(rf"\[nav\] close-done.*{a.short}|\[nav\] close-done", since=mark)
    wait_until_no_claude(am, a)
    assert am.profile.session(a.id) is not None  # kept, resumable from Sessions
    assert am.claude.transcript_path(a.id) is not None  # nothing on disk deleted
    w.tabs.context_menu("t2").choose("Close tab")
    Dialog.wait(am, title="Close session?").press("★ Favorite & Close")
    w.tabs.wait_gone("t2")
    wait_until(lambda: am.profile.session_store(b.id).get("favorite") in (True, "true", "1", 1), 10,
               what="the star", describe=lambda: am.profile.session_store(b.id))


@pytest.mark.p1
@pytest.mark.flow("TCM-09")
def test_close_other_tabs_asks_once(am, s2):
    """TCM-09 phase 4: Close ▸ Close other tabs over two sessions asks ONE question.

    Args:
        am: The app.
        s2: F-S2.
    """
    w = am.window
    w.tabs.context_menu(SHELL).choose_path("Close", "Close other tabs")
    dlg = Dialog.wait(am)
    assert "Close All" in dlg.buttons() and "Cancel All" in dlg.buttons(), dlg.buttons()
    dlg.press("Close All")
    wait_until(lambda: w.tabs.titles() == [SHELL], 15, what="only the shell left", describe=w.tabs.titles)
    assert Dialog.find(am) is None  # one question, not one per session
