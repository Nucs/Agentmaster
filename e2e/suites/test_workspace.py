"""Quitting keeps the workspace (doc/agentmaster/flows/07-tabs-park-close-dock.md, TAB-10).

A restart (fixture F-RS) through the windows' ✕; the record on disk (`windows\\<id>.json`) is read between the quit
and the next start, so a failure says whether the SAVE or the REOPEN lost the tab.
"""
import pytest

from amdriver.waits import wait_until

SHELL = "Administrator: Windows PowerShell"


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
