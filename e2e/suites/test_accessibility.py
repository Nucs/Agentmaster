"""What UI Automation can read of Agentmaster: its dialogs, names, ids and item status (flows TAB-K3 and A11Y-K1;
doc/agentmaster/E2E_TESTING.md §6.3).

Every test here but one proves a FIX that no published release holds yet (`needs_fix`): on 0.8.0 the quit
confirmation is invisible to UI Automation (TAB-K3), and the Agent Manager's cards, rows and glyph buttons have no
names, ids or status (A11Y-K1). Deselect them there with `-m "not needs_fix"`. The one known-defect test,
test_known_defect_a_hit_test_passes_through_a_dialog, shows a gap no build has fixed: UI Automation's hit test
passes through every dialog to the terminal behind it (A11Y-K2).

What the fixed build exposes (AgentAutomation.h is the one recipe):
- every ContentDialog the page shows is a window-level Popup named by its title (they are built in code now), so
  `Dialog.wait` finds the quit confirmation like any other dialog, with its buttons;
- AutomationIds in one scheme, `am.<surface>.<element>[.<key>]` (key = the session id's first 8 characters);
- names for the controls that had none (the cog, the compose icons, ⋯, a card or a row = its title);
- ItemStatus for what was pixels only: a card "waiting-for-you; selected", a tab "running; unsent draft; selected on
  the Agent Manager page" (the Linked-Lenses pill, MGR-05 phase 2), a side panel card the same as its title's status.
"""
import time

import pytest

from amdriver import inputs, uia, win32
from amdriver.app import QUIT_CONFIRM_TITLE
from amdriver.ui.dialogs import Dialog
from amdriver.ui.manager import STATE_WORD
from amdriver.waits import wait_until

SHELL = "Administrator: Windows PowerShell"
LARGE_PASTE_CHARS = 6000
"""More than the large-paste warning's 5 KiB threshold (TerminalPage::_PasteFromClipboardHandler), with no newline,
so the multi-line warning stays out of it."""


@pytest.mark.p1
@pytest.mark.flow("TAB-10")
@pytest.mark.needs_fix("TAB-K3")
def test_the_quit_confirmation_is_readable(am, s1):
    """TAB-K3: after "Close All Windows", "Do you want to close all windows?" is a dialog UI Automation reads — found
    by its title, with its two buttons (0.8.0: an empty Popup, no title, no buttons); Cancel keeps the app and its
    window. A hit test at its "Close all" still lands on the terminal behind it, as for EVERY dialog of the app, a
    code-built one included: A11Y-K2, test_known_defect_a_hit_test_passes_through_a_dialog (this test asserted the
    hit test until a build with the fix showed it, 2026-10-23, run fixall-0811).

    Args:
        am: The app.
        s1: F-S (a session, so the window has something to keep).
    """
    w = am.window
    mark = am.profile.hooks.mark()
    win32.set_foreground(w.hwnd)
    win32.post_close(w.hwnd)
    Dialog.wait(am, title="Close this window?", timeout_s=10).press("Close All Windows")
    quit_dialog = Dialog.wait(am, title=QUIT_CONFIRM_TITLE, timeout_s=10)
    assert quit_dialog.buttons() == ["Close all", "Cancel"], quit_dialog.buttons()
    assert quit_dialog.element().find(role="button", name="Close all") is not None
    quit_dialog.press("Cancel")
    am.profile.hooks.wait(r"\[nav\] quit cancelled", since=mark, timeout_s=10)
    assert am.alive()
    w.tabs.wait_tab("t1")


@pytest.mark.p2
@pytest.mark.flow("TCM-09")
@pytest.mark.known_defect("A11Y-K2")
def test_known_defect_a_hit_test_passes_through_a_dialog(am):
    """A11Y-K2 (expected failure once fixed): UI Automation's hit test — IUIAutomation::ElementFromPoint, what a screen
    reader's mouse and touch exploration ask — at a button of an open ContentDialog returns the terminal BEHIND the
    dialog, also for a dialog built in code that the tree walk reads on every build: the shell tab's "Close tab?" (found
    by its title, with its buttons). Seen 2026-10-23: 12 of 12 hit points over the window's middle on the
    TermControl for "Close tab?" on 0.8.0 (suites/probe_dialogs.py, run nf-080b), and "Close all" of the quit
    confirmation on a build with TAB-K3's fix (run fixall-0811). The keyboard is in the dialog meanwhile (UI
    Automation's focused element is its button). Cause not investigated: a popup of the XAML island that the island's
    hit test does not reach is the likely place.

    The test asserts TODAY's behavior; when it starts failing, the defect is fixed: assert the hit test returns the
    dialog's button.

    Args:
        am: The app (its shell tab).
    """
    w = am.window
    w.focus()
    w.tabs.wait_tab(SHELL)
    w.tabs.close(SHELL, answer=None)
    dialog = Dialog.wait(am, title="Close tab?", timeout_s=10)
    assert dialog.buttons() == ["Close", "Cancel"], dialog.buttons()
    cancel = dialog.element().find(role="button", name="Cancel")
    assert cancel is not None
    time.sleep(1.0)  # past the dialog's open transition, so the hit test below meets the dialog fully shown
    hit = uia.at_point(*cancel.center)
    assert hit is not None and hit.class_name == "TermControl", \
        f"the hit test found {hit.line() if hit else None} — A11Y-K2 fixed? (expected today: the terminal)"
    dialog.press("Cancel")
    w.tabs.wait_tab(SHELL)


@pytest.mark.p2
@pytest.mark.needs_fix("TAB-K3")
def test_the_large_paste_warning_is_readable(am):
    """TAB-K3's other dialogs: the large-paste warning (declared in TerminalPage.xaml on 0.8.0, built in code since the
    fix) is found by its title with its buttons, and Cancel drops the paste.

    Args:
        am: The app (its shell tab selected).
    """
    w = am.window
    w.tabs.wait_tab(SHELL)
    win32.set_clipboard_text("x" * LARGE_PASTE_CHARS)
    w.focus()
    w.terminal.focus()
    inputs.keys("ctrl+shift+v")
    warning = Dialog.wait(am, title="Warning", timeout_s=10)
    assert warning.buttons() == ["Paste anyway", "Cancel"], warning.buttons()
    assert "5 KiB" in warning.text()
    warning.press("Cancel")
    assert am.alive()


@pytest.mark.p2
@pytest.mark.flow("MGR-01")
@pytest.mark.needs_fix("A11Y-K1")
def test_the_page_names_its_glyph_buttons_and_cards(am, s1):
    """A11Y-K1: the page's cog, its compose icons and its cards have names and ids, and a card says its state; a
    tooltip's text is its control's help text.

    Args:
        am: The app.
        s1: F-S (t1, Waiting for you).
    """
    w = am.window
    page = w.manager.open()
    controls = page.controls()
    cog = next((el for el in controls if el.automation_id == "am.mgr.toolbar.settings"), None)
    assert cog is not None and cog.name == "Settings", [el.line() for el in controls if el.role == "button"][:20]
    assert "Model and launch options" in cog.help_text, cog.help_text
    card = wait_until(lambda: page.card_by_session(s1.id), 10, what="t1's card by its AutomationId",
                      describe=lambda: [(c.title, c.el.automation_id) for c in page.cards()])
    assert card.el.name == "t1"
    word = STATE_WORD[s1.state]
    assert card.facets()[0] == word, card.status
    row = wait_until(lambda: next((r for r in page.tree_rows() if r.el.automation_id == f"am.mgr.tree.row.{s1.short}"),
                                  None), 10, what="t1's tree row by its AutomationId")
    assert row.el.name == "t1" and row.status.split("; ")[0] == word, row.status
    card.click()
    page.show_tab("Prompt Queue")
    buttons = page.compose_buttons()
    assert [b.automation_id for b in buttons] == ["am.mgr.queue.jump", "am.mgr.queue.sendNow", "am.mgr.queue.add"]
    assert [b.name for b in buttons] == ["Jump to tab", "Send now", "Add to queue"]


@pytest.mark.p1
@pytest.mark.flow("MGR-05")
@pytest.mark.needs_fix("A11Y-K1")
def test_the_selection_is_readable_on_every_surface(am, s2):
    """MGR-05 phase 2 (and A11Y-K1): the page's selection reads the same on the board card, the tree row, the side
    panel's card and the tab's selection pill — and moves together when another card is clicked.

    Args:
        am: The app.
        s2: F-S2 (t1 and t2, both Waiting for you).
    """
    a, b = s2
    w = am.window
    a.select()  # the page opens on the tab you come from: t1
    page = w.manager.open()

    def readable(selected, other) -> bool:
        """One look: `selected` is the selection everywhere, `other` nowhere.

        Args:
            selected: The session that should be selected.
            other: The session that should not.
        Returns:
            True when every surface agrees.
        """
        card_s, card_o = page.card_by_session(selected.id), page.card_by_session(other.id)
        tab_s, tab_o = w.tabs.by_session(selected.id), w.tabs.by_session(other.id)
        side = {c.session_key: c for c in w.side_panel.cards()}
        return bool(card_s and card_o and card_s.selected and not card_o.selected
                    and tab_s and tab_o and tab_s.pill and not tab_o.pill
                    and side.get(selected.short) and side[selected.short].selected
                    and side.get(other.short) and not side[other.short].selected)

    def describe() -> dict:
        """What every surface says now (for a failure message).

        Returns:
            The statuses.
        """
        return {"cards": [(c.title, c.status) for c in page.cards()],
                "tabs": [(t.title, t.status) for t in w.tabs.items()],
                "side": [(c.title, c.status) for c in w.side_panel.cards()]}

    wait_until(lambda: readable(a, b), 10, what="t1 selected on every surface", describe=describe)
    page.card("t2").click()
    wait_until(lambda: readable(b, a), 10, what="t2 selected on every surface after its card's click", describe=describe)


@pytest.mark.p2
@pytest.mark.flow("MGR-04")
@pytest.mark.needs_fix("A11Y-K1")
def test_the_tab_says_its_session_state(am, s1):
    """A11Y-K1: the tab that hosts a session is found by its id (`am.tab.<key>`), and its item status follows the
    session — "running" during a held turn, "waiting-for-you" after it.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    tab = wait_until(lambda: w.tabs.by_session(s1.id), 10, what="t1's tab by its AutomationId",
                     describe=lambda: [(t.title, t.el.automation_id) for t in w.tabs.items()])
    assert tab.title == "t1"
    s1.ask('held turn [[mock say "working"; hold g; reply "done"]]', wait="sent")
    am.mock.wait_holding("g", timeout_s=30)
    wait_until(lambda: (w.tabs.by_session(s1.id) or tab).state_word == "running", 20, what="the tab says running",
               describe=lambda: (w.tabs.by_session(s1.id) or tab).status)
    am.mock.release("g")
    wait_until(lambda: (w.tabs.by_session(s1.id) or tab).state_word == "waiting-for-you", 30,
               what="the tab says waiting-for-you", describe=lambda: (w.tabs.by_session(s1.id) or tab).status)
