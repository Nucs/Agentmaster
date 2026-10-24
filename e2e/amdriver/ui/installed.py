"""An Agentmaster the install lane started — an installed copy on the Default profile, started like a double-click — seen
through the Standard lane's page objects (MainWindow, ManagerPage) and its settings card.

The Standard lane's amdriver.app.App launches the app on a test profile and owns that process; the install lane only
finds a process (install.wait_app). InstalledApp gives the page objects the little they read from an App: the process,
its main windows, and the time of the last Home click (Home ignores a second click within the double-click time).

Used by the install lane's tests that act inside the app: the cog's Uninstall (IS-90) and Check for updates (IS-80).
"""
from __future__ import annotations

from .. import uia, win32
from ..app import MAIN_CLASS
from ..waits import wait_until
from .manager import ManagerPage
from .window import MainWindow


class InstalledApp:
    """The process of an installed copy, shaped like the App the page objects expect.

    Attributes:
        pid: The Agentmaster process.
    """

    def __init__(self, pid: int) -> None:
        """Bind to a process.

        Args:
            pid: The Agentmaster process.
        Returns:
            None.
        """
        self.pid = pid
        self._last_home_click = 0.0

    def main_hwnds(self) -> list[int]:
        """The process's visible main windows.

        Returns:
            Their handles.
        """
        return [w.hwnd for w in win32.top_windows(self.pid) if w.cls == MAIN_CLASS and w.visible]

    def window(self, timeout_s: float = 30.0) -> MainWindow:
        """Its first main window, once it shows.

        Args:
            timeout_s: How long to wait for it.
        Returns:
            The window's page object.
        Raises:
            WaitTimeout: When no main window showed.
        """
        return MainWindow(self, wait_until(self.main_hwnds, timeout_s, what=f"a main window of Agentmaster pid {self.pid}")[0])


def open_settings_tab(window: MainWindow, tab: str) -> uia.Element:
    """Open the settings card on a tab: the Agent Manager page, its cog, then the tab's button.

    The cog is found by its place: v0.8.0's is a nameless button right before "Sessions" in the page's toolbar (a later
    build names it "Settings": E2E_TESTING.md §6.3).

    Args:
        window: The app's main window.
        tab: The settings tab's name ("About").
    Returns:
        The window's root element (the card lives in the window's tree).
    Raises:
        AssertionError: When the page has no Sessions button, or what comes before it is not a button.
        WaitTimeout: When the page or the tab's button did not come.
    """
    page = ManagerPage(window).open()
    controls = page.controls()
    at = next((i for i, el in enumerate(controls) if el.role == "button" and el.name == "Sessions"), None)
    assert at is not None, f"no Sessions button on the page: {[el.name for el in controls if el.name][:30]}"
    cog = controls[at - 1]
    assert cog.role == "button" and cog.name in ("", "Settings"), f"before Sessions comes {cog.role} {cog.name!r}"
    cog.invoke()
    root = window.element()
    root.wait_find(30, 40, role="button", name=tab).invoke()
    return root
