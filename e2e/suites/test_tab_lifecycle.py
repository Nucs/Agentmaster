"""Parking, waking and closing tabs (doc/agentmaster/flows/07-tabs-park-close-dock.md TAB-01–04, and
doc/agentmaster/flows/01-agent-manager-page.md MGR-03's second-window and dormant-neighbor phases).

A parked tab is checked the way it matters to a user: its claude.exe is gone and nothing starts it but entering or
activating the tab ([OS]); the engine says what it did (`[deactivate] … parked dormant`, `[nav] deactivate-done
scope=… parked=N`, `[close-successor] …`) [LOG]; the record and the window record keep the session [ENG]; and a
woken tab resumes the SAME conversation (`claude --resume <id>`) with its unsent draft typed back.

Two sessions launched in one folder share the folder's name as their title, so the folder-scope tests launch them
with `am.launch(folder, title=…)`, which renames each new tab before the next launch.
"""
import json
import time

import pytest

from amdriver.session import wait_until_no_claude
from amdriver.ui.dialogs import Dialog
from amdriver.ui.menus import Menu
from amdriver.waits import hold_true, wait_until

SHELL = "Administrator: Windows PowerShell"


def _window_tab_refs(am) -> list[tuple[str, str]]:
    """Every window record's tab refs as (kind, short id) pairs.

    Args:
        am: The app.
    Returns:
        The pairs ("Claude", "<sid8>") / ("Other", "").
    """
    return [(t.get("kind"), (t.get("sessionId") or "")[:8]) for r in am.profile.window_records()
            for t in r.get("window", {}).get("tabs", [])]


def _queue(session) -> list[dict]:
    """A session's persisted prompt queue.

    Args:
        session: The Session.
    Returns:
        The queue entries (text, status, origin …).
    """
    return session.record().raw.get("queue", [])


def _shell_count(am) -> int:
    """How many default shell TABS the app runs: Windows PowerShell processes the GUI started ITSELF (managed sessions
    run in pwsh.exe). A tab's shell is ConPTY's client, which the terminal creates with the pseudoconsole attribute, so
    its parent is the GUI — never the OpenConsole.exe started beside it (the evidence's processes.txt lists the GUI's
    children in psutil's order: each OpenConsole.exe and its tab's shell side by side, the claudes after them).

    ⚠ Only those: every hook forwarder is a Windows PowerShell too (exec-form hooks, HOOKS.md), started by claude.exe and
    alive for a moment. Counted, a forwarder running at one count and not at the other made test_deactivate_scopes'
    E3 check read "1 == 2" (2026-10-23, run fix3: a [Notification] hook of t3 at the first count). A first fix that
    asked for an OpenConsole.exe parent counted no shell at all (run fix5: "assert 0 >= 1").

    Args:
        am: The app.
    Returns:
        The count.
    """
    def started_by_the_gui(p) -> bool:
        """Whether the GUI itself started a process (a tab's shell) — False when it is gone or unreadable.

        Args:
            p: A psutil process.
        Returns:
            True for a tab's shell.
        """
        try:
            return p.ppid() == am.pid
        except Exception:  # noqa: BLE001 - a process gone between listing and reading, or the GUI gone
            return False

    return sum(1 for p in am.children() if _pname(p) == "powershell.exe" and started_by_the_gui(p))


def _pname(p) -> str:
    """A process's image name in lower case ("" when it is gone).

    Args:
        p: A psutil process.
    Returns:
        The name.
    """
    try:
        return (p.name() or "").lower()
    except Exception:  # noqa: BLE001 - a process gone between listing and reading
        return ""


# ---- TAB-01 · Deactivate a tab and wake it -------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TAB-01")
def test_deactivate_parks_the_session_and_a_click_wakes_it(am, s2, mock):
    """TAB-01 phases 1–3 and E4: Deactivate ▸ Deactivate Tab on a background tab parks its session without asking —
    the claude exits, the tab keeps its title, its unsent draft and its queued prompt; nothing starts it and the
    Prompt Queue Runner sends nothing into it while parked; a click wakes it on the same conversation, types the
    draft back, and the queue then continues (the runner is still Auto).

    Args:
        am: The app.
        s2: F-S2 (t1, t2; t2 selected).
        mock: The scripted model.
    """
    a, b = s2
    w = am.window
    # Arrange t1: a queued prompt (the page's compose row) and an unsent draft in its box.
    a.select()
    page = w.manager.open()  # the lens follows the tab we came from: t1
    page.show_tab("Prompt Queue")
    page.queue_prompt('queued later [[mock reply "from the queue"]]')
    wait_until(lambda: [q for q in _queue(a) if q.get("status") == "Pending" and q.get("text", "").startswith("queued later")],
               10, what="the queued prompt Pending in t1's record", describe=lambda: _queue(a))
    queued_at = time.monotonic()
    a.select()
    w.terminal.wait_input_box()
    w.terminal.focus()
    w.terminal.type("keep me")
    am.profile.wait_session(lambda s: s.id == a.id and s.raw.get("pendingInput") == "keep me", 15,
                            what="t1's unsent draft on disk")
    # 1. Park t1 from t2 (t1 is not the focused tab, so the page does not show).
    b.select()
    mark = am.profile.hooks.mark()
    amark = am.profile.autorunner.mark()
    w.tabs.deactivate("t1")
    a.wait_parked(since=mark)
    am.profile.hooks.wait(r"\[nav\] deactivate-done scope=this parked=1 skipped=0", since=mark)
    am.profile.hooks.wait(rf"\[retired-host\] {a.id} no longer hosted", since=mark)
    am.profile.hooks.wait(rf"\[draft-restore\] {a.short} armed", since=mark)
    assert time.monotonic() - queued_at < 40, "the park came after the runner's 40 s settle: re-time the test"
    assert Dialog.find(am) is None  # no confirmation
    assert w.tabs.selected().title == "t2" and not w.manager.showing()
    rec = a.record()
    assert rec.title == "t1" and rec.raw.get("pendingInput") == "keep me"
    assert rec.raw.get("autorunner", {}).get("mode") == "Full"  # the park keeps the runner's mode
    assert any(q.get("status") == "Pending" for q in _queue(a))
    # E4: a parked tab's menu leads with Activate, and Deactivate Tab is grayed.
    menu = w.tabs.context_menu("t1")
    assert menu.names()[0] == "Activate Tab (Shift+Click)"
    assert not menu.submenu("Deactivate").enabled("Deactivate Tab")
    Menu.dismiss_all(am)
    # 2. It stays parked: no claude, still a session tab in the record, never archived — and the runner holds the
    # queued prompt past its 40 s settle ("tab dormant").
    hold_true(lambda: not a.claude_pids(), max(30.0, 45.0 - (time.monotonic() - queued_at)),
              what="the parked tab stays parked")
    am.profile.autorunner.wait(rf"\[advance-skip\] {a.id} \(tab dormant", since=amark)
    assert not [p for p in mock.prompts() if "queued later" in json.dumps(p)], "a prompt was sent into a parked tab"
    assert ("Claude", a.short) in _window_tab_refs(am)
    assert not am.profile.hooks.find(rf"\[archive\] {a.id}|{a.short}.*-> archived", since=mark)
    # 3. Wake: a click resumes the same conversation and types the draft back.
    seq = mock.last_seq()
    mark = am.profile.hooks.mark()
    w.tabs.select("t1")
    a.wait_running(30)
    assert any("--resume" in c and a.id in c for c in a.claude_cmdlines()), a.claude_cmdlines()
    am.profile.hooks.wait(rf"\[draft-restore\] {a.short} draft RESTORED \+ VERIFIED", since=mark, timeout_s=40)
    wait_until(lambda: (w.terminal.input_box() or None) and w.terminal.input_box().draft == "keep me", 10,
               what="the draft back in t1's box", describe=w.terminal.input_box)
    # The queue continues after the settle: the queued prompt reaches the model (the draft swap takes the draft out
    # of the box first), and the draft is put back after it.
    mock.wait_prompt("queued later", since=seq, timeout_s=100)
    am.profile.hooks.wait(rf"\[draft-swap\] {a.short} holding an unsent draft", since=mark, timeout_s=10)
    a.wait_state("WaitingForInput", timeout_s=60)
    wait_until(lambda: [q for q in _queue(a) if q.get("text", "").startswith("queued later") and q.get("status") == "Sent"],
               20, what="the queued prompt Sent", describe=lambda: _queue(a))
    wait_until(lambda: (w.terminal.input_box() or None) and w.terminal.input_box().draft == "keep me", 30,
               what="the draft back in the box after the queued send", describe=w.terminal.input_box)


@pytest.mark.p0
@pytest.mark.flow("TAB-01")
def test_a_parked_tab_starts_in_place_and_survives_a_restart(am, s2):
    """TAB-01 phase 4 and E7: a parked tab wakes WITHOUT being selected through its menu's Activate Tab (its
    Shift+Click twin on the tab: test_shift_click_on_a_parked_tab_starts_it_in_place, TAB-K4's fix); after a
    restart a parked tab comes back as an ordinary restored-dormant tab.

    Args:
        am: The app.
        s2: F-S2 (t1, t2; t2 selected).
    """
    a, b = s2
    w = am.window
    # 4. Activate Tab (Shift+Click) from the menu.
    mark = am.profile.hooks.mark()
    w.tabs.deactivate("t1")
    a.wait_parked(since=mark)
    mark = am.profile.hooks.mark()
    w.tabs.activate("t1")
    a.wait_running(30)
    am.profile.hooks.wait(rf"\[nav\] activate-dormant {a.short}", since=mark)
    hold_true(lambda: w.tabs.selected().title == "t2", 1.5, what="t2 still selected after Activate Tab")
    assert any("--resume" in c and a.id in c for c in a.claude_cmdlines()), a.claude_cmdlines()
    # E7: park t1, quit, start again — t1 reopens dormant (t2 was focused: it alone starts).
    mark = am.profile.hooks.mark()
    w.tabs.deactivate("t1")
    a.wait_parked(since=mark)
    time.sleep(2)  # past the window record's autosave debounce
    am.close()
    assert ("Claude", a.short) in _window_tab_refs(am), _window_tab_refs(am)
    am.kill()
    am.start()
    w = am.window
    w.tabs.wait_selected("t2", timeout_s=30)
    b.wait_running(30)
    hold_true(lambda: not a.claude_pids(), 3.0, what="the parked tab stays dormant after the reopen")
    assert w.tabs.context_menu("t1").names()[0] == "Activate Tab (Shift+Click)"
    Menu.dismiss_all(am)


@pytest.mark.p2
@pytest.mark.flow("TAB-01")
@pytest.mark.needs_fix("TAB-K4")
def test_shift_click_on_a_parked_tab_starts_it_in_place(am, s2):
    """TAB-01 phase 4's gesture on the tab itself (TAB-K4's fix, 2026-10-23): Shift+Click on a parked tab — the
    gesture its own menu item names, "Activate Tab (Shift+Click)" — starts its claude IN PLACE and leaves the selection
    and the keyboard where they were: `[nav] activate-dormant <sid8>` and `[nav] tab-shift-click <sid8> "t1": started
    in place, no switch (its release will not select it)`, no `[nav] tab-focus <sid8>`, t2 stays selected, and text
    typed next lands in t2's box (the tap that would have focused t1's header never ran). A second Shift+Click on the
    now-running tab starts nothing and still does not switch.

    On 0.8.0 the Shift+Click switches to the tab instead (TAB-K4, measured 2026-10-23 with Shift held 0.08 s, 0.4 s
    and 1 s): the handler was a plain PointerPressed handler, and the tab (a ListViewItem) marks a left press handled
    before such a handler runs. Deselect this test there with -m "not needs_fix".

    Args:
        am: The app.
        s2: F-S2 (t1, t2; t2 selected).
    """
    a, b = s2
    w = am.window
    mark = am.profile.hooks.mark()
    w.tabs.deactivate("t1")
    a.wait_parked(since=mark)
    w.terminal.focus()  # the keyboard starts in t2's terminal, whatever the menu left focused
    mark = am.profile.hooks.mark()
    w.tabs.get("t1").click(modifiers=["shift"])
    a.wait_running(30)
    am.profile.hooks.wait(rf"\[nav\] activate-dormant {a.short}", since=mark)
    am.profile.hooks.wait(rf"\[nav\] tab-shift-click {a.short} .*: started in place, no switch "
                          r"\(its release will not select it\)", since=mark)
    hold_true(lambda: w.tabs.selected().title == "t2", 1.5, what="t2 still selected after the Shift+Click")
    assert not am.profile.hooks.find(rf"\[nav\] tab-focus {a.short}", since=mark)
    assert any("--resume" in c and a.id in c for c in a.claude_cmdlines()), a.claude_cmdlines()
    # The keyboard stayed with t2: no tab header took the focus.
    w.terminal.type("still t2")
    wait_until(lambda: (w.terminal.input_box() or None) and w.terminal.input_box().draft == "still t2", 10,
               what="the typed text in t2's box", describe=w.terminal.input_box)
    # Again on the running tab: nothing to start, and still no switch.
    mark = am.profile.hooks.mark()
    w.tabs.get("t1").click(modifiers=["shift"])
    am.profile.hooks.wait(rf"\[nav\] tab-shift-click {a.short} .*: nothing to start", since=mark)
    hold_true(lambda: w.tabs.selected().title == "t2", 1.0, what="t2 still selected after the second Shift+Click")
    assert not am.profile.hooks.find(rf"\[nav\] tab-focus {a.short}", since=mark)


# ---- TAB-02 · Deactivate's scopes ----------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("TAB-02")
def test_deactivate_scopes(am, mock):
    """TAB-02 phases 1–6 and E1, E3: window 1 holds the shell, t1, A2, t3, A4, t5 (A2 and A4 in folder `fa`),
    window 2 holds A6 (in `fa` too). Each scope parks exactly its tabs; the folder scopes reach window 2 without a
    confirmation; the shell is never touched; the gates gray what cannot apply.

    Args:
        am: The app.
        mock: The scripted model.
    """
    s = {"t1": am.launch("t1"), "A2": am.launch("fa", title="A2"), "t3": am.launch("t3"),
         "A4": am.launch("fa", title="A4"), "t5": am.launch("t5"), "A6": am.launch("fa", title="A6")}
    w1 = am.window
    w2 = w1.tabs.move_to_new_window("A6")
    w1.tabs.wait_gone("A6")
    every = set(s)
    shells = _shell_count(am)
    assert shells >= 1
    fa = str(am.iso.folder("fa"))

    def running() -> set:
        """The titles whose claude runs now.

        Returns:
            The set.
        """
        return {t for t, ses in s.items() if ses.claude_pids()}

    def park(window, anchor: str, scope: str, done: str, parked: set) -> None:
        """Run one Deactivate scope and check exactly `parked` went dormant.

        Args:
            window: The window whose tab menu is used.
            anchor: The right-clicked tab.
            scope: The Deactivate ▸ item.
            done: A regex for the done line (scope + counts).
            parked: The titles that must park (the others keep running).
        Returns:
            None.
        Raises:
            AssertionError / WaitTimeout: When the scope parked another set.
        """
        before = running()
        mark = am.profile.hooks.mark()
        window.tabs.deactivate(anchor, scope)
        am.profile.hooks.wait(done, since=mark, timeout_s=20)
        for t in parked:
            s[t].wait_parked(since=mark)
        hold_true(lambda: running() == before - parked, 2.0, what=f"{scope} on {anchor} parking exactly {sorted(parked)}",
                  describe=running)

    def wake(window, scope: str = "this") -> None:
        """Wake dormant tabs with the page's Activate All Tabs (N) — this window's, or (scope "all", answered in the
        dialog that asks while another window has dormant tabs too) every window's.

        Args:
            window: The window whose page is used.
            scope: "this" or "all".
        Returns:
            None.
        """
        window.manager.activate_all(scope)
        wait_until(lambda: window.manager.dormant_count() == 0, 30, what="the window's dormant tabs woken",
                   describe=window.manager.dormant_count)

    # E1: the gates.
    sub = w1.tabs.context_menu("t5").submenu("Deactivate")
    assert not sub.enabled("Tabs to the Right") and sub.enabled("Tabs to the Left")
    Menu.dismiss_all(am)
    sub = w2.tabs.context_menu("A6").submenu("Deactivate")
    assert [sub.enabled(n) for n in ("Tabs to the Left", "Tabs to the Right", "Other Tabs")] == [False, False, False]
    assert sub.enabled("Deactivate Tab") and sub.enabled("Of Same Folder")
    Menu.dismiss_all(am)
    # E1: an enabled item can still park nothing (t1 has only the shell on its left).
    w1.tabs.select("t1")
    park(w1, "t1", "Tabs to the Left", r"\[nav\] deactivate-done scope=left parked=0 skipped=0", set())
    # 1–2. Left and right of t3.
    w1.tabs.select("t3")
    park(w1, "t3", "Tabs to the Left", r"\[nav\] deactivate-done scope=left parked=2 skipped=0", {"t1", "A2"})
    park(w1, "t3", "Tabs to the Right", r"\[nav\] deactivate-done scope=right parked=2 skipped=0", {"A4", "t5"})
    assert running() == {"t3", "A6"} and not w1.manager.showing()
    wake(w1)
    w1.tabs.select("t3")
    wait_until(lambda: running() == every, 40, what="every session running again", describe=running)
    # 3. Other Tabs.
    park(w1, "t3", "Other Tabs", r"\[nav\] deactivate-done scope=others parked=4 skipped=0", {"t1", "A2", "A4", "t5"})
    wake(w1)
    w1.tabs.select("t3")
    wait_until(lambda: running() == every, 40, what="every session running again", describe=running)
    # 4. Of Same Folder from A2: A2 and A4 here, A6 in window 2 (its focused tab: window 2 shows its page first).
    mark = am.profile.hooks.mark()
    park(w1, "A2", "Of Same Folder",
         r"\[nav\] deactivate-done scope=folder parked=2 skipped=0 elsewhere=1 dir=" + _re(fa), {"A2", "A4", "A6"})
    am.profile.hooks.wait(rf"\[nav\] deactivate-remote {s['A6'].short} \(a folder deactivate started in another window\)",
                          since=mark)
    w2.manager.wait_showing()
    assert Dialog.find(am) is None
    wake(w1, "all")  # dormant tabs in both windows: the button asks, "All windows (3)" wakes A2, A4 and A6
    wait_until(lambda: running() == every, 40, what="every session running again", describe=running)
    w1.tabs.select("t3")
    # 5. Other of Same Folder from A2: A4 and A6; A2 keeps running.
    park(w1, "A2", "Other of Same Folder",
         r"\[nav\] deactivate-done scope=folder-others parked=1 skipped=0 elsewhere=1 dir=" + _re(fa), {"A4", "A6"})
    wake(w1)  # "This window (1)": A4 only …
    hold_true(lambda: not s["A6"].claude_pids(), 3.0, what="window 2's A6 left dormant by 'This window'")
    w1.tabs.select("t3")
    wake(w2)  # … and window 2's page wakes A6 (no other window has a dormant tab now: no question)
    wait_until(lambda: running() == every, 40, what="every session running again", describe=running)
    # 6. All Tabs: every managed tab of window 1 (t3 is focused: the page shows first); window 2 untouched.
    park(w1, "t3", "All Tabs", r"\[nav\] deactivate-done scope=all parked=5 skipped=0", {"t1", "A2", "t3", "A4", "t5"})
    w1.manager.wait_showing()
    assert running() == {"A6"}
    # E3: the shell tab was in scope of several phases and never touched.
    assert _shell_count(am) == shells


DEACTIVATE_IDLE = "Deactivate All Idle Tabs"
"""Deactivate ▸'s last item since 2026-10-24 (test_tab_menu.py has the same constant for the submenu's list)."""

IDLE_QUESTION = "Deactivate idle tabs"
"""The title of the question DEACTIVATE_IDLE asks while other windows hold idle tabs too (2026-10-24)."""


@pytest.mark.p1
@pytest.mark.flow("TAB-02")
@pytest.mark.needs_fix("TAB-F1")
def test_deactivate_all_idle_tabs(am, mock):
    """TAB-02 phase 7 and E1's content gate (2026-10-24): Deactivate ▸ ends with "Deactivate All Idle Tabs", which parks
    every session of the window in the Idle state and nothing else — t1 and t3 are Idle (launched with no prompt), t2
    is Waiting for you — and is grayed once no Idle session is left to park. A build of the branch holds the item; 0.8.0
    does not (its submenu ends at "All Tabs"), hence needs_fix("TAB-F1").

    Args:
        am: The app.
        mock: The scripted model (t2's first turn).
    """
    s = {"t1": am.launch("t1", prompt=None), "t2": am.launch("t2"), "t3": am.launch("t3", prompt=None)}
    w = am.window
    for title in ("t1", "t3"):
        s[title].wait_state("Idle", timeout_s=30)
    s["t2"].wait_state("WaitingForInput", timeout_s=30)
    for ses in s.values():
        ses.wait_running()  # every claude runs before the park, so "parked" below means this item parked it
    # The item is the submenu's last one, after All Tabs, and enabled: two Idle sessions are there to park.
    sub = w.tabs.context_menu("t2").submenu("Deactivate")
    assert sub.names()[-2:] == ["All Tabs", DEACTIVATE_IDLE], sub.names()
    assert sub.enabled(DEACTIVATE_IDLE)
    Menu.dismiss_all(am)
    # 7. From t2's menu (t2 itself is not Idle): t1 and t3 park, t2 keeps running. t3 is the focused tab, so the
    #    window shows its Agent Manager page before t3's control is swapped (the scope's focused-tab rule). One
    #    window only: "All windows" would park the same tabs, so nothing asks (phase 8 has the question).
    mark = am.profile.hooks.mark()
    w.tabs.deactivate("t2", DEACTIVATE_IDLE)
    am.profile.hooks.wait(r"\[nav\] deactivate-done scope=idle parked=2 skipped=0", since=mark, timeout_s=20)
    am.profile.hooks.wait(r"\[nav\] deactivate-idle-answer this-window \(no other window holds an idle tab; nothing asked\)",
                          since=mark)
    assert Dialog.find(am, title=IDLE_QUESTION) is None
    s["t1"].wait_parked(since=mark)
    s["t3"].wait_parked(since=mark)
    hold_true(lambda: s["t2"].running(), 2.0, what="t2 (Waiting for you) still running after the idle park")
    # E1: no Idle session is left that a park would take (t1 and t3 are dormant now), so the item is grayed.
    sub = w.tabs.context_menu("t2").submenu("Deactivate")
    assert not sub.enabled(DEACTIVATE_IDLE)
    Menu.dismiss_all(am)


@pytest.mark.p1
@pytest.mark.flow("TAB-02")
@pytest.mark.needs_fix("TAB-F1")
def test_deactivate_idle_tabs_asks_which_windows(am, mock):
    """TAB-02 phase 8 and E1 (2026-10-24, the user's "Add prompt asking if to deactivate This window or All
    windows"): while another window holds Idle tabs too, "Deactivate All Idle Tabs" asks first — a dialog "Deactivate
    idle tabs" with This window (N) · All windows (M) · Cancel. Window 1 holds t1 and t2 (Idle, launched with no
    prompt) and t4 (Waiting for you); window 2 holds t3 (Idle). Cancel parks nothing; This window parks t1 and t2
    only; asked again with no Idle tab left in window 1, "This window (0)" is grayed and All windows parks window 2's
    t3 (its only tab: window 2 shows its Agent Manager page first); then no window holds one and the item is grayed.
    t4 keeps running throughout. 0.8.0 has neither the item nor the question, hence needs_fix("TAB-F1").

    Args:
        am: The app.
        mock: The scripted model (t4's first turn).
    """
    s = {"t1": am.launch("t1", prompt=None), "t2": am.launch("t2", prompt=None), "t3": am.launch("t3", prompt=None),
         "t4": am.launch("t4")}
    w1 = am.window
    w2 = w1.tabs.move_to_new_window("t3")
    w1.tabs.wait_gone("t3")
    for title in ("t1", "t2", "t3"):
        s[title].wait_state("Idle", timeout_s=30)
    s["t4"].wait_state("WaitingForInput", timeout_s=30)
    for ses in s.values():
        ses.wait_running()  # every claude runs first, so "parked" below means the question's answer parked it

    def ask(buttons: list) -> Dialog:
        """Choose the item from t4's tab menu (t4 is not Idle) and return the question it asks.

        Args:
            buttons: The button texts the dialog must show, in order (primary, secondary, close).
        Returns:
            The open Dialog.
        Raises:
            WaitTimeout: When no question appeared.
            AssertionError: When its buttons differ.
        """
        w1.tabs.deactivate("t4", DEACTIVATE_IDLE)
        dlg = Dialog.wait(am, title=IDLE_QUESTION, timeout_s=10)
        assert dlg.buttons() == buttons, dlg.buttons()
        return dlg

    # 8a. Cancel parks nothing; the counts read "This window: 2 • All windows: 3.".
    mark = am.profile.hooks.mark()
    dlg = ask(["This window (2)", "All windows (3)", "Cancel"])
    assert "This window: 2" in dlg.text() and "All windows: 3" in dlg.text(), dlg.text()
    am.profile.hooks.wait(r"\[nav\] deactivate-idle-ask from=tab menu here=2 elsewhere=1", since=mark)
    dlg.press("Cancel")
    am.profile.hooks.wait(r"\[nav\] deactivate-idle-answer cancelled", since=mark)
    hold_true(lambda: all(ses.running() for ses in s.values()), 2.0, what="every session still running after Cancel")
    # 8b. This window: t1 and t2 park; window 2's t3 and the waiting t4 keep running.
    mark = am.profile.hooks.mark()
    ask(["This window (2)", "All windows (3)", "Cancel"]).press("This window (2)")
    am.profile.hooks.wait(r"\[nav\] deactivate-idle-answer this-window", since=mark)
    am.profile.hooks.wait(r"\[nav\] deactivate-done scope=idle parked=2 skipped=0", since=mark, timeout_s=20)
    s["t1"].wait_parked(since=mark)
    s["t2"].wait_parked(since=mark)
    hold_true(lambda: s["t3"].running() and s["t4"].running(), 2.0, what="t3 (window 2) and t4 still running")
    # 8c / E1: window 1 holds no Idle tab now but window 2 does, so the item stays enabled and asks again, with
    #    "This window (0)" grayed; All windows hands t3 to window 2, which parks it.
    sub = w1.tabs.context_menu("t4").submenu("Deactivate")
    assert sub.enabled(DEACTIVATE_IDLE)
    Menu.dismiss_all(am)
    mark = am.profile.hooks.mark()
    dlg = ask(["This window (0)", "All windows (1)", "Cancel"])
    this_window = dlg.element().find(role="button", name="This window (0)")
    assert this_window is not None and not this_window.enabled
    dlg.press("All windows (1)")
    am.profile.hooks.wait(r"\[nav\] deactivate-idle-answer all-windows \(1 idle session", since=mark)
    am.profile.hooks.wait(rf"\[nav\] deactivate-remote {s['t3'].short} \(an idle-tabs deactivate started in another window\)",
                          since=mark, timeout_s=20)
    s["t3"].wait_parked(since=mark)
    w2.manager.wait_showing()
    hold_true(lambda: s["t4"].running(), 2.0, what="t4 (Waiting for you) still running")
    # E1: no window holds an Idle tab a park would take now: the item is grayed.
    sub = w1.tabs.context_menu("t4").submenu("Deactivate")
    assert not sub.enabled(DEACTIVATE_IDLE)
    Menu.dismiss_all(am)


def _re(text: str) -> str:
    """A literal text as a regex.

    Args:
        text: The text.
    Returns:
        The escaped pattern.
    """
    import re
    return re.escape(text)


# ---- TAB-03 · Close keeps the session; Of Same Folder ----------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TAB-03")
def test_unfavorite_and_close(am, s1):
    """TAB-03 phase 3: a starred session's close confirm offers ☆ Unfavorite & Close in the place of ★ Favorite &
    Close; it closes the tab, archives the session and removes the star.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    w.tabs.context_menu("t1").choose("Favorite")
    wait_until(lambda: am.profile.session_store(s1.id).get("favorite") in (True, "true", "1", 1), 10,
               what="the star", describe=lambda: am.profile.session_store(s1.id))
    mark = am.profile.hooks.mark()
    w.tabs.close("t1", answer=None)
    dlg = Dialog.wait(am, title="Close session?")
    assert dlg.buttons() == ["Close", "☆ Unfavorite & Close", "Cancel"], dlg.buttons()
    dlg.press("☆ Unfavorite & Close")
    w.tabs.wait_gone("t1")
    am.profile.hooks.wait(rf"\[nav\] close-confirm {s1.short} \(unfavorite & close\)", since=mark)
    am.profile.hooks.wait(rf"\[archive\] {s1.id}", since=mark)
    wait_until_no_claude(am, s1)
    wait_until(lambda: am.profile.session_store(s1.id).get("favorite") not in (True, "true", "1", 1), 10,
               what="the star removed", describe=lambda: am.profile.session_store(s1.id))
    assert am.profile.session(s1.id) is not None and am.claude.transcript_path(s1.id) is not None


@pytest.mark.p0
@pytest.mark.flow("TAB-03")
def test_close_of_same_folder_across_windows(am, mock):
    """TAB-03 phases 4–5 and E1, E5: Close ▸ Other of Same Folder / Of Same Folder asks ONE question listing the
    folder's sessions in every window; Cancel closes nothing; Close All closes them here and in the other window
    (which, left without a tab, closes itself), keeps every record and transcript, and leaves other folders alone.

    Args:
        am: The app.
        mock: The scripted model.
    """
    b1 = am.launch("fb", title="B1")
    b2 = am.launch("fb", title="B2")
    b3 = am.launch("fb", title="B3")
    other = am.launch("t2")
    w1 = am.window
    w2 = w1.tabs.move_to_new_window("B3")
    w1.tabs.wait_gone("B3")
    fb = str(am.iso.folder("fb"))
    # E1: Cancel closes nothing.
    mark = am.profile.hooks.mark()
    w1.tabs.context_menu("B1").choose_path("Close", "Other of Same Folder")
    dlg = Dialog.wait(am, title="Close 2 other sessions in this folder?")
    text = dlg.text()
    assert "• B2" in text and "• B3" in text and "• B1" not in text and fb in text, text
    assert dlg.buttons() == ["Close All", "Cancel"], dlg.buttons()
    dlg.press("Cancel")
    am.profile.hooks.wait(rf"\[nav\] close-folder cancelled dir={_re(fb)} except={b1.short}", since=mark)
    hold_true(lambda: all(x.claude_pids() for x in (b1, b2, b3)), 1.5, what="nothing closed after Cancel")
    # 5. Other of Same Folder → Close All: B2 here, B3 in window 2 (which then closes itself); B1 stays.
    mark = am.profile.hooks.mark()
    w1.tabs.context_menu("B1").choose_path("Close", "Other of Same Folder")
    Dialog.wait(am, title="Close 2 other sessions in this folder?").press("Close All")
    w1.tabs.wait_gone("B2")
    am.profile.hooks.wait(rf"\[nav\] close-folder begin dir={_re(fb)} except={b1.short} sessions=2", since=mark)
    am.profile.hooks.wait(rf"\[nav\] close-folder done dir={_re(fb)}", since=mark)
    for x in (b2, b3):
        am.profile.hooks.wait(rf"\[archive\] {x.id}", since=mark)
        wait_until_no_claude(am, x)
    wait_until(lambda: not w2.alive(), 20, what="window 2, left without a tab, to close itself")
    am.profile.hooks.wait(r"\[manager-only\] self-close window", since=mark)
    assert b1.claude_pids() and other.claude_pids()
    # 4. Of Same Folder: now only B1 is open in the folder.
    mark = am.profile.hooks.mark()
    w1.tabs.context_menu("B1").choose_path("Close", "Of Same Folder")
    dlg = Dialog.wait(am, title="Close 1 session in this folder?")
    assert "• B1" in dlg.text()
    dlg.press("Close All")
    w1.tabs.wait_gone("B1")
    wait_until_no_claude(am, b1)
    assert w1.tabs.titles() == [SHELL, "t2"] and other.claude_pids()
    # E5: nothing on disk is deleted.
    for x in (b1, b2, b3):
        assert am.profile.session(x.id) is not None and am.claude.transcript_path(x.id) is not None


# ---- TAB-04 + MGR-03 phase 4 · closing never wakes a dormant neighbor ----------------------------------------


@pytest.mark.p0
@pytest.mark.flow("TAB-04")
@pytest.mark.flow("MGR-03")
def test_closing_the_focused_tab_never_starts_a_dormant_one(am, s2, mock):
    """TAB-04 phase 2 (both branches) and MGR-03 phase 4: after a restart only the focused tab's claude runs; closing
    it moves the focus to the nearest tab that starts nothing (a dormant neighbor is skipped), and with every
    remaining tab dormant the Agent Manager page shows instead — no claude starts.

    Args:
        am: The app.
        s2: F-S2.
        mock: The scripted model.
    """
    t1, t2 = s2
    t3 = am.launch("t3")
    t4 = am.launch("t4")
    w = am.window
    w.tabs.close(SHELL)  # a window of four managed tabs (a replayed shell's start is not what this flow is about)
    w.tabs.select("t2")
    time.sleep(2)  # past the window record's autosave debounce
    # 1. Arrange: restart; t2 reopens focused and starts, t1/t3/t4 stay dormant.
    am.close()
    am.kill()
    am.start()
    w = am.window
    w.tabs.wait_selected("t2", timeout_s=30)
    t2.wait_running(30)
    hold_true(lambda: not any(x.claude_pids() for x in (t1, t3, t4)), 3.0, what="the other tabs dormant")
    # t4 is started in place (its menu's Activate Tab), so the strip is: t1 (dormant), t2 (focused), t3 (dormant),
    # t4 (running).
    w.tabs.activate("t4")
    t4.wait_running(30)
    w.tabs.wait_selected("t2")
    # 2a. Close t2: upstream would select t3 (the tab that slides into its slot), which would start it; the nearest
    # tab that starts nothing is t4.
    mark = am.profile.hooks.mark()
    w.tabs.close("t2")
    w.tabs.wait_selected("t4")
    am.profile.hooks.wait(r"\[close-successor\] left dormant [0-9a-f]{8} \"t3\" asleep; focused [0-9a-f]{8} \"t4\" "
                          r"idx=2 \(nearest tab that starts nothing\)", since=mark)
    wait_until_no_claude(am, t2)
    hold_true(lambda: not t1.claude_pids() and not t3.claude_pids(), 4.0, what="no dormant tab woken by the close")
    # 2b. Close t4: every remaining tab is dormant — the page shows, nothing starts.
    mark = am.profile.hooks.mark()
    w.tabs.close("t4")
    w.manager.wait_showing()
    am.profile.hooks.wait(r"\[close-successor\] every remaining tab would start a process on being shown", since=mark)
    wait_until_no_claude(am, t4)
    hold_true(lambda: not am.claude_processes(), 4.0, what="no claude started by the close")
    assert w.tabs.titles() == ["t1", "t3"] and w.tabs.selected() is None


# ---- MGR-03 phase 1 · a second window left without a tab closes itself ----------------------------------------


@pytest.mark.p0
@pytest.mark.flow("MGR-03")
def test_a_second_window_left_without_a_tab_closes_itself(am):
    """MGR-03 phase 1: a second window (Ctrl+Shift+N, one shell tab) whose last tab is closed shows its page, then
    closes itself — it is not the last window — and its window record is deleted; the first window is untouched.

    Args:
        am: The app.
    """
    w1 = am.window
    w2 = am.new_window()
    w2.tabs.wait_tab(SHELL)
    wait_until(lambda: len(am.profile.window_records()) == 2, 15, what="the second window's record saved",
               describe=lambda: len(am.profile.window_records()))
    mark = am.profile.hooks.mark()
    w2.tabs.close(SHELL)
    am.profile.hooks.wait(r"\[nav\] manager-page show \(last tab closed\)", since=mark)
    am.profile.hooks.wait(r"\[manager-only\] self-close window \{", since=mark, timeout_s=15)
    wait_until(lambda: not w2.alive(), 15, what="window 2 to close itself")
    wait_until(lambda: len(am.profile.window_records()) == 1, 15, what="its record deleted",
               describe=lambda: len(am.profile.window_records()))
    assert w1.alive() and w1.tabs.titles() == [SHELL]
