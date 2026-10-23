"""Quitting keeps the workspace (doc/agentmaster/flows/07-tabs-park-close-dock.md, TAB-10).

A restart (fixture F-RS) through the windows' ✕; the record on disk (`windows\\<id>.json`) is read between the quit
and the next start, so a failure says whether the SAVE or the REOPEN lost the tab.

The close-with-a-pending-autosave tests (TAB-10 E2 / E3, flows TAB-K1) need the window record's autosave to fire
AFTER the close has saved the record and archived the window's sessions, and before the window is gone. A tab switch
restarts that 750 ms autosave, but "Close this window?" opens about a second after the ✕, so a switch made before
the ✕ always lands its save first (measured 2026-10-23, run tabk1-repro: the dialog was found 0.85–1.6 s after the
switch). So these tests open the dialog FIRST, switch tabs under it through UI Automation (the tab's SelectionItem
pattern; `[nav] tab-focus <sid8>` marks the moment the autosave restarted), and invoke "Close Window" a chosen time
after the switch — a sweep of that time moves the save's due moment across the close's archive. Each test writes
what it measured into its evidence folder (close-timeline.json), passed or failed, so a run shows where every case
landed.

Measured on 0.8.0 (run tabk1-repro3, 2026-10-23, 3 of the 9 cases lost the Claude refs): the late save lands, and
records every Claude tab as a shell, when it falls due between the archive and about 80 ms after it — due 30 and
56 ms after the archive, and one due 12 ms BEFORE it that the busy close delayed to 2 ms after. A save due 95 ms or
more after the archive never fired (the window, or the whole app, was gone by then), and one due before the archive
saved the refs intact. So which cases hit moves with the machine's speed. On a build of the fix every case keeps the
refs, and a case that hits logs `[window-save] <id> late save refused` (run tabk1-verify2, a test build of the fix:
9 of 9 passed, 3 of them hit and were refused 67–119 ms after the archive). The two sweeps prove that fix, which no
published release holds yet (`needs_fix("TAB-K1")`): deselect them on 0.8.0 with -m "not needs_fix".
"""
import json
import subprocess
import time
from datetime import datetime, timedelta
from typing import Optional

import pytest

from amdriver import win32
from amdriver.ui.dialogs import Dialog
from amdriver.waits import wait_until

SHELL = "Administrator: Windows PowerShell"
DEBOUNCE_MS = 750
"""The window record's autosave debounce (_saveWindowRecordThrottled, TerminalPage.AgentEngine.cpp): every tab switch,
tab add or remove, resize and lens change restarts it, and the save it ends in rewrites windows\\<id>.json."""
QUIET_S = DEBOUNCE_MS / 1000 + 1.0
"""A pause after which no autosave is pending any more."""


def _tab_refs(record: dict) -> list[tuple[str, str]]:
    """The tabs a window record holds, in order.

    Args:
        record: One parsed windows\\<id>.json.
    Returns:
        (kind, session id's first 8 characters) per tab; a shell tab is ("Other", "").
    """
    return [(t.get("kind"), (t.get("sessionId") or "")[:8]) for t in record.get("window", {}).get("tabs", [])]


def _rx_id(window_id: str) -> str:
    """A window id ready for a regex (its braces escaped).

    Args:
        window_id: A braced GUID.
    Returns:
        The escaped text.
    """
    return window_id.replace("{", r"\{").replace("}", r"\}")


def _record_of(am, window_id: str) -> dict:
    """One window's record on disk.

    Args:
        am: The app.
        window_id: The window's id as the log writes it (a braced GUID).
    Returns:
        The parsed record, or {} when there is none.
    """
    for record in am.profile.window_records():
        if record.get("window", {}).get("windowId", "").lower() == window_id.lower():
            return record
    return {}


def _window_ids(am) -> list[str]:
    """The ids of the windows this run created, in creation order (each window's `[window-fresh]` line).

    Args:
        am: The app.
    Returns:
        The braced GUIDs.
    """
    return [m.group(1) for m in am.profile.hooks.find(r"\[window-fresh\] (\{[0-9a-fA-F-]{36}\})")]


def _switch_moment(am, session, mark: int) -> datetime:
    """When the app switched to a session's tab after a mark — the moment its window record's autosave restarted.

    Args:
        am: The app.
        session: The Session whose tab was selected.
        mark: A hooks.log mark taken before the switch.
    Returns:
        The last `[nav] tab-focus <sid8>` line's stamp.
    Raises:
        WaitTimeout: When no such line came.
    """
    stamped = wait_until(lambda: am.profile.hooks.stamped(rf"\[nav\] tab-focus {session.short}", since=mark), 5.0,
                         what=f"the [nav] tab-focus line of {session.short}")
    return stamped[-1][0]


def _open_close_dialog(window):
    """Open one window's "Close this window?" the user's way — its ✕ (WM_CLOSE) — and find its "Close Window" button.

    Args:
        window: The MainWindow.
    Returns:
        (the Dialog, the button's element).
    Raises:
        WaitTimeout: When the dialog did not open.
        LookupError: When it has no "Close Window" button.
    """
    win32.set_foreground(window.hwnd)
    win32.post_close(window.hwnd)
    dialog = wait_until(lambda: Dialog.find_in(window, "Close this window?"), 10.0, what="Close this window?")
    button = dialog.element().find(role="button", name="Close Window")
    if button is None:
        raise LookupError(f"{dialog!r} has no 'Close Window' button; buttons: {dialog.buttons()}")
    return dialog, button


def _sleep_until(moment: datetime) -> None:
    """Sleep until a moment of this machine's clock (at once when it has passed).

    Args:
        moment: The moment.
    Returns:
        None.
    """
    lead = (moment - datetime.now()).total_seconds()
    if lead > 0:
        time.sleep(lead)


def _close_timeline(am, mark: int, window_id: str, switched: datetime, invoked: datetime) -> dict:
    """What the close did and when, from hooks.log, in ms after the tab switch that restarted the autosave.

    Args:
        am: The app.
        mark: The hooks.log mark taken before the switch.
        window_id: The closed window's id.
        switched: The switch's moment.
        invoked: When "Close Window" was invoked.
    Returns:
        {"invoked", "confirmed", "archived" (ms after the switch, None for a missing line),
         "save_due_after_archive_ms" (DEBOUNCE_MS − archived: > 0 means the switch's autosave was still pending when
         the close archived the sessions, the moment TAB-K1 needs; the switch's own remaining work makes the real due
         moment a few ms later), "saves" (the `[window-save] <id> tabs=…` lines after the confirmation — the app logs
         one only when the saved tab set CHANGED), "late_save_refused" (a fixed build's `[window-save] <id> late save
         refused …` lines; 0.8.0 has no such line)}.
    """

    def ms(when: Optional[datetime]) -> Optional[int]:
        """Milliseconds after the switch.

        Args:
            when: A moment, or None.
        Returns:
            The rounded ms, or None.
        """
        return None if when is None else round((when - switched).total_seconds() * 1000)

    hooks = am.profile.hooks
    confirmed = hooks.stamped(r"\[nav\] window-close confirmed", since=mark)
    archived = hooks.stamped(r"\[teardown-archive\] ", since=mark)
    after = confirmed[0][0] if confirmed else switched
    wid = _rx_id(window_id)
    saves = [{"ms": ms(when), "line": m.string} for when, m in hooks.stamped(rf"\[window-save\] {wid} tabs=", since=mark)
             if when >= after]
    refused = [{"ms": ms(when), "line": m.string}
               for when, m in hooks.stamped(rf"\[window-save\] {wid} late save refused", since=mark)]
    archived_ms = ms(archived[0][0]) if archived else None
    return {"invoked": ms(invoked), "confirmed": ms(confirmed[0][0]) if confirmed else None, "archived": archived_ms,
            "save_due_after_archive_ms": None if archived_ms is None else DEBOUNCE_MS - archived_ms,
            "saves": saves, "late_save_refused": refused}


def _write_timeline(am, timeline: dict, record: dict) -> None:
    """Keep the measured close in the test's evidence folder (zipped with the results, passed or failed).

    Args:
        am: The app.
        timeline: _close_timeline's answer.
        record: The window's record as saved.
    Returns:
        None.
    """
    out = am.iso.out_dir
    out.mkdir(parents=True, exist_ok=True)
    (out / "close-timeline.json").write_text(json.dumps({"timeline": timeline, "tabs": _tab_refs(record)}, indent=1),
                                             encoding="utf-8")


@pytest.mark.p0
@pytest.mark.flow("TAB-10")
def test_close_all_windows_keeps_the_claude_tab(am, s1):
    """TAB-10 phase 3: ✕ → Close All Windows → Close all; the record keeps the Claude tab as a session ref, and the
    next start reopens it as the session (selected and resumed, since it was the focused tab).

    Args:
        am: The app.
        s1: F-S (t1 selected).
    """
    assert am.window.tabs.selected().title == "t1"
    am.close(how="all")
    tabs = [(t.get("kind"), (t.get("sessionId") or "")[:8]) for r in am.profile.window_records()
            for t in r.get("window", {}).get("tabs", [])]
    assert ("Claude", s1.short) in tabs, f"the quit saved: {tabs}"
    am.kill()
    am.start()
    w = am.window
    w.tabs.wait_selected("t1", timeout_s=30)
    wait_until(lambda: any("--resume" in c for c in s1.claude_cmdlines()), 30,
               what="t1 resumed at the reopen", describe=s1.claude_cmdlines)
    menu = w.tabs.context_menu("t1")
    assert "Copy" in menu.names() and "Deactivate" in menu.names()  # a session tab, not a shell titled t1
    menu.close()


@pytest.mark.p0
@pytest.mark.flow("TAB-10")
@pytest.mark.needs_fix("TAB-K1")
@pytest.mark.parametrize("after_ms", [250, 325, 400, 475, 550])
def test_a_close_right_after_a_tab_switch_keeps_the_claude_tabs(am, s2, after_ms):
    """TAB-10 E2 (flows TAB-K1): "Close Window" while the window record's autosave is still pending after a tab
    switch must leave both Claude tabs in the record as session refs (the app quits: it was the last window).

    On 0.8.0 the pending save fires after the close has saved the record and archived the window's sessions (which
    empties the window's tab → session map) and records the Claude tabs as anonymous shells, so the next start
    reopens them as PowerShell tabs: the e2e suite saw `[teardown-archive]` and, 67 ms later, `[window-save] … [sh,sh,]`
    (run5). The window is closed only when the emperor handles the close it was POSTED, so the page and its pending
    timer outlive the archive by a few tens of ms; the sweep of after_ms moves the save's due moment across that span.

    Args:
        am: The app.
        s2: F-S2 (the shell, t1, t2).
        after_ms: When "Close Window" is invoked after the switch to t2 (made under the open dialog).
    """
    a, b = s2
    w = a.select()
    time.sleep(QUIET_S)  # the record holds both refs with t1 focused, and no autosave is pending
    window_id = _window_ids(am)[0]
    tab_b = w.tabs.get("t2")
    _dialog, button = _open_close_dialog(w)
    mark = am.profile.hooks.mark()
    tab_b.el.select()  # under the open dialog: the switch to t2 restarts the autosave
    switched = _switch_moment(am, b, mark)
    am.ended_by_test = True  # the close quits the app (its last window)
    _sleep_until(switched + timedelta(milliseconds=after_ms))
    invoked = datetime.now()
    button.invoke()
    try:
        am.proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass  # a quit that hangs is not this test's question: the record check below still tells what the close saved
    record = _record_of(am, window_id)
    timeline = _close_timeline(am, mark, window_id, switched, invoked)
    _write_timeline(am, timeline, record)
    tabs = _tab_refs(record)
    assert ("Claude", a.short) in tabs and ("Claude", b.short) in tabs, \
        f"the close saved the Claude tabs as {tabs} (TAB-K1); the close, in ms after the switch: {timeline}"
    assert record["window"].get("selectedSessionId", "")[:8] == b.short, \
        f"the record's focused tab is {record['window'].get('selectedSessionId')!r}, not t2; {timeline}"


@pytest.mark.p0
@pytest.mark.flow("TAB-10")
@pytest.mark.needs_fix("TAB-K1")
@pytest.mark.parametrize("after_ms", [None, 325, 400, 475])
def test_closing_one_of_two_windows_keeps_its_claude_tab(am, s2, after_ms):
    """TAB-10 E3 with E2 (flows TAB-K1): Close Window on ONE of two windows — the app keeps running, so a save still
    pending at the close, or one the close itself sets off, has the closed window's whole remaining life to fire —
    leaves the closed window's Claude tab in its record as a session ref, for "Reopen Windows (N)" (which reads the
    record from disk).

    after_ms None: a quiet close (the switch to t1 1.75 s before it, its autosave landed): the record must also
    survive whatever the close itself schedules. A number: "Close Window" is invoked that long after the switch to t1,
    made under the open dialog, as in test_a_close_right_after_a_tab_switch_keeps_the_claude_tabs. Either way the
    record is read 3 s after the window is gone, past any save the closed window could still have pending.

    Args:
        am: The app.
        s2: F-S2 (t2 moves to a window of its own; window 1 keeps the shell and t1).
        after_ms: When "Close Window" is invoked after the switch to t1, or None for a quiet close.
    """
    a, b = s2
    w1 = am.window
    w2 = w1.tabs.move_to_new_window("t2")
    window_id = _window_ids(am)[0]
    w1.tabs.select(SHELL)
    time.sleep(QUIET_S)
    tab_a = w1.tabs.get("t1")
    mark = am.profile.hooks.mark()
    if after_ms is None:
        tab_a.el.select()
        switched = _switch_moment(am, a, mark)
        time.sleep(QUIET_S)  # the switch's own save has landed: nothing is pending at the close
        _dialog, button = _open_close_dialog(w1)
    else:
        _dialog, button = _open_close_dialog(w1)
        tab_a.el.select()  # under the open dialog: the switch to t1 restarts the autosave
        switched = _switch_moment(am, a, mark)
        _sleep_until(switched + timedelta(milliseconds=after_ms))
    invoked = datetime.now()
    button.invoke()
    wait_until(lambda: w1.hwnd not in am.main_hwnds(), 20, what="window 1 to close")
    time.sleep(3.0)  # past every autosave the closed window could still have pending
    assert am.alive() and w2.hwnd in am.main_hwnds(), "the app or its second window ended with window 1"
    record = _record_of(am, window_id)
    timeline = _close_timeline(am, mark, window_id, switched, invoked)
    _write_timeline(am, timeline, record)
    tabs = _tab_refs(record)
    assert tabs == [("Other", ""), ("Claude", a.short)], \
        f"window 1's record holds {tabs} (TAB-K1); the close, in ms after the switch: {timeline}"
    assert record["window"].get("selectedSessionId", "")[:8] == a.short, \
        f"the record's focused tab is {record['window'].get('selectedSessionId')!r}, not t1; {timeline}"
