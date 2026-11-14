"""The LAUNCHER (doc/agentmaster/flows/07-tabs-park-close-dock.md TAB-12; TAB_STRIP.md "The + button: the launcher";
SIDE_PANEL.md §2e): the tab strip's "+" opens Agentmaster's launcher card under itself — the side panel's + card, grown
into a panel (the user's picks of 2026-11-14): the KIND CHIP (Claude, Codex, every Windows Terminal profile), the folder
box on the FOCUSED TAB's folder, the MODEL / PRESET pills for a new Claude session, Launch, and a footer. Ctrl+Shift+T
opens it too; Alt / Shift / Ctrl + click keep Windows Terminal's own "+" (split pane, new window, elevated).

Every test here proves behavior no published release holds (`needs_fix("TAB-12")`): deselect them on an older build with
`-m "not needs_fix"`. Checked the way a user meets them: the card shows under "+" [UI], the new session or tab exists
[ENG/UI], and the engine says what it did (`[nav] tab strip launcher open (…)`, `[nav] launcher shell …`) [LOG].
"""
import time

import pytest

from amdriver import inputs
from amdriver.session import wait_new_session
from amdriver.waits import wait_until

# The e2e desktop runs elevated, so Windows Terminal titles its PowerShell tab this way (test_tab_lifecycle.py's SHELL).
SHELL_PROFILE = "Windows PowerShell"
SHELL_TAB = "Administrator: Windows PowerShell"


def _same_folder(a: str, b: str) -> bool:
    """Do two folder strings name one folder (Windows: case-insensitive, either slash, no trailing separator)?

    Args:
        a, b: The folders.
    Returns:
        True for the same folder.
    """
    def norm(s: str) -> str:
        return str(s).replace("/", "\\").rstrip("\\").lower()
    return norm(a) == norm(b)


@pytest.mark.p0
@pytest.mark.flow("TAB-12")
@pytest.mark.needs_fix("TAB-12")
def test_strip_plus_opens_the_launcher_on_the_focused_tab(am, s1, mock):
    """TAB-12 phases 1-3: a click on the strip's "+" opens the launcher card UNDER "+" (no new tab, no Windows Terminal
    profile menu), on the focused tab's folder, kind Claude, Default lit; the folder list hangs under the whole card;
    Esc closes the list, a second Esc the card; then "+, Enter" starts exactly one Claude session in that folder.

    Args:
        am: The app.
        s1: F-S (t1 focused: its folder is the one the card must open on).
        mock: The scripted model.
    """
    w = am.window
    tabs_before = w.tabs.titles()
    mark = am.profile.hooks.mark()
    card = w.tabs.open_launcher()
    plus = w.tabs.new_tab_button()
    el = card.element()
    assert el is not None
    # Under "+": the card's top edge is at or below the button's bottom edge (the corner placement adds a 2-DIP gap).
    assert el.rect[1] >= plus.rect[1] + plus.rect[3] - 2, (el.rect, plus.rect)
    assert am.profile.hooks.find(r"\[nav\] tab strip launcher open \(\+ click\)", since=mark)
    assert w.tabs.titles() == tabs_before, "the + click opened a tab instead of the launcher"
    # The focused tab's folder, Claude, Default.
    assert _same_folder(card.box().value, am.iso.folder("t1")), (card.box().value, am.iso.folder("t1"))
    assert card.kind() == "Claude", card.kind()
    lit = card.lit_pill()
    assert lit is not None and lit.automation_id == "am.launcher.model.default", lit
    # The folder list is open, under the whole card (the pills stay clickable).
    wait_until(lambda: card.folder_list() is not None, 5, what="the folder list open with the card")
    lst = card.folder_list()
    assert lst.rect[1] >= el.rect[1] + el.rect[3] - 2, (lst.rect, el.rect)
    inputs.keys("esc")
    wait_until(lambda: card.folder_list() is None, 5, what="Esc closes the folder list")
    inputs.keys("esc")
    wait_until(lambda: not card.is_open(), 5, what="a second Esc closes the card")
    assert s1.state == "WaitingForInput"  # no ESC reached t1's claude
    # "+, Enter": one new Claude session in the focused tab's folder.
    sessions_before = {s.id for s in am.profile.sessions()}
    card = w.tabs.open_launcher()
    mark = am.profile.hooks.mark()
    inputs.keys("enter")
    new = wait_new_session(am, mark, pattern=r"\[nav\] open-new claude done new=([0-9a-f]{8})", mock=mock)
    wait_until(lambda: not card.is_open(), 5, what="the card closes after the launch")
    time.sleep(2)
    assert {s.id for s in am.profile.sessions()} - sessions_before == {new.id}  # exactly one
    assert _same_folder(new.record().working_dir, am.iso.folder("t1")), new.record().working_dir
    assert s1.state == "WaitingForInput"  # no CR reached t1


@pytest.mark.p0
@pytest.mark.flow("TAB-12")
@pytest.mark.needs_fix("TAB-12")
def test_ctrl_shift_t_opens_the_launcher(am, s1):
    """TAB-12 phase 4: Ctrl+Shift+T (Windows Terminal's new-tab key) opens the launcher under "+" — the user's pick
    "Follows the new +" — and opens no tab by itself; a click on "+" while it is open closes it (a toggle).

    Args:
        am: The app.
        s1: F-S (t1 focused, its terminal holding the keyboard).
    """
    from amdriver.ui.sidepanel import LauncherCard
    w = am.window
    w.focus()
    tabs_before = w.tabs.titles()
    mark = am.profile.hooks.mark()
    inputs.keys("ctrl+shift+t")
    card = LauncherCard(w)
    wait_until(card.is_open, 5, what="Ctrl+Shift+T opens the launcher")
    assert am.profile.hooks.find(r"\[nav\] tab strip launcher open \(keys\)", since=mark)
    time.sleep(1.0)
    assert w.tabs.titles() == tabs_before, "Ctrl+Shift+T opened a tab"
    w.tabs.new_tab_button().click()  # its press counts as inside the card: the click toggles it closed
    wait_until(lambda: not card.is_open(), 5, what="+ closes the launcher it opened")
    assert w.tabs.titles() == tabs_before


@pytest.mark.p0
@pytest.mark.flow("TAB-12")
@pytest.mark.needs_fix("TAB-12")
def test_kind_chip_opens_a_shell_and_is_remembered(am, s1):
    """TAB-12 phases 5-6: the kind chip's menu lists Claude, Codex and the Windows Terminal profiles; picking Windows
    PowerShell hides the MODEL / PRESET pills and turns Launch into "Open Windows PowerShell", whose Enter opens that
    shell as a new tab in the box's folder (`[nav] launcher shell …`). The pick is remembered: the next open starts on it;
    picking Claude again brings the pills back.

    Args:
        am: The app.
        s1: F-S (t1 focused).
    """
    w = am.window
    card = w.tabs.open_launcher()
    card.pick_kind(SHELL_PROFILE)
    wait_until(lambda: card.pills() == [], 5, what="a shell hides the MODEL / PRESET pills")
    assert card.launch_button().name == "Open " + SHELL_PROFILE, card.launch_button().name
    mark = am.profile.hooks.mark()
    inputs.keys("enter")
    wait_until(lambda: not card.is_open(), 5, what="the card closes after the shell opens")
    w.tabs.wait_tab(SHELL_TAB, timeout_s=20)
    assert am.profile.hooks.find(r"\[nav\] launcher shell end \(launcher card\): " + SHELL_PROFILE, since=mark)
    # Remembered (settings.json launcherKind), in any window: the next open starts on the shell.
    w.tabs.select("t1")
    card = w.tabs.open_launcher()
    assert card.kind() == SHELL_PROFILE, card.kind()
    assert card.pills() == []
    card.pick_kind("Claude")
    wait_until(lambda: card.lit_pill() is not None, 5, what="Claude brings the pills back, Default lit")
    assert card.launch_button().name == "Launch Claude"
    inputs.keys("esc")
    wait_until(lambda: card.folder_list() is None or not card.is_open(), 5, what="Esc")
    if card.is_open():
        inputs.keys("esc")
        wait_until(lambda: not card.is_open(), 5, what="a second Esc closes the card")
    assert s1.state == "WaitingForInput"
