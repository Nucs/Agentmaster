"""One Agentmaster window, the root of the page objects.

A window's tree (measured, 0.8.0): root → InputSite pane → [`#TabView`, the content element]. The content
element's direct children are the whole visible UI below the tab strip, in one flat list: the Agent Manager
page's controls (while it shows), the side panel's, the terminal and the per-tab badge (while a tab shows), and
the in-window popups. Page objects read that list once per question (one cross-process call) instead of
walking the whole tree.
"""
from __future__ import annotations

from typing import Optional

from .. import uia, win32
from ..terminal import Terminal
from ..waits import wait_until

INPUT_SITE_CLASS = "Windows.UI.Input.InputSite.WindowClass"
TITLE_ON_PAGE = ("Agent Manager", "Agent Manager Dev")
"""The window title while the Agent Manager page shows (release / dev identity)."""


class MainWindow:
    """An Agentmaster window.

    Attributes:
        app: The App that owns it.
        hwnd: The window handle.
    """

    def __init__(self, app, hwnd: int) -> None:
        """Bind to a window.

        Args:
            app: The App.
            hwnd: The window handle.
        Returns:
            None.
        """
        self.app = app
        self.hwnd = hwnd

    def __repr__(self) -> str:
        """A short description.

        Returns:
            The text.
        """
        return f"MainWindow(0x{self.hwnd:x} {self.title!r})"

    # ---- the tree ---------------------------------------------------------------------------------

    def element(self) -> uia.Element:
        """The window's element, read now.

        Returns:
            The element.
        Raises:
            comtypes.COMError: When the window is gone.
        """
        return uia.Element.from_hwnd(self.hwnd)

    def input_site(self) -> uia.Element:
        """The XAML island's root pane (the parent of the tab view, the content and the dialogs).

        Returns:
            The element.
        Raises:
            LookupError: When the window has no island (it is closing).
        """
        for child in self.element().children():
            if child.class_name == INPUT_SITE_CLASS:
                return child
        raise LookupError(f"{self!r} has no XAML island")

    def content(self) -> uia.Element:
        """The element that holds everything below the tab strip.

        Returns:
            The element.
        Raises:
            LookupError: When it is not there.
        """
        for child in self.input_site().children():
            if child.role == "custom":
                return child
        raise LookupError(f"{self!r} has no content element")

    def content_children(self) -> list[uia.Element]:
        """The content element's direct children (the flat list most page objects read).

        Returns:
            The elements, in tree order.
        """
        return self.content().children()

    # ---- the window -------------------------------------------------------------------------------

    @property
    def title(self) -> str:
        """The window title (the selected tab's title, or "Agent Manager" while the page shows).

        Returns:
            The title.
        """
        for w in win32.top_windows(self.app.pid):
            if w.hwnd == self.hwnd:
                return w.title
        return ""

    def alive(self) -> bool:
        """Whether the window still exists.

        Returns:
            True while it does.
        """
        return self.hwnd in self.app.main_hwnds()

    def rect(self) -> tuple[int, int, int, int]:
        """The window rectangle.

        Returns:
            (left, top, width, height).
        """
        return next(w for w in win32.top_windows(self.app.pid) if w.hwnd == self.hwnd).rect()

    def focus(self) -> None:
        """Bring the window to the foreground (real input only reaches the foreground window).

        Returns:
            None.
        Raises:
            RuntimeError: When Windows refused.
        """
        if not win32.set_foreground(self.hwnd):
            raise RuntimeError(f"could not bring {self!r} to the foreground")

    def maximize(self) -> None:
        """Maximize the window.

        Returns:
            None.
        """
        win32.maximize(self.hwnd)

    # ---- the parts --------------------------------------------------------------------------------

    @property
    def tabs(self):
        """The tab strip.

        Returns:
            A TabStrip.
        """
        from .tabstrip import TabStrip
        return TabStrip(self)

    @property
    def manager(self):
        """The Agent Manager page.

        Returns:
            A ManagerPage.
        """
        from .manager import ManagerPage
        return ManagerPage(self)

    @property
    def side_panel(self):
        """The side panel.

        Returns:
            A SidePanel.
        """
        from .sidepanel import SidePanel
        return SidePanel(self)

    @property
    def terminal(self) -> Terminal:
        """The terminal of the selected tab.

        Returns:
            A Terminal.
        """
        return Terminal(self)

    @property
    def tags(self):
        """The Tags panel (open it from a session menu's Tags).

        Returns:
            A TagsPanel.
        """
        from .popups import TagsPanel
        return TagsPanel(self)

    @property
    def specify(self):
        """The "Specify a model" card (open it from a model menu's Specify…).

        Returns:
            A SpecifyModelCard.
        """
        from .popups import SpecifyModelCard
        return SpecifyModelCard(self)

    @property
    def queue(self):
        """The Agent Manager page's Prompt Queue tab (open it for a session with `queue.open(title)`).

        Returns:
            A PromptQueuePane.
        """
        from .queue import PromptQueuePane
        return PromptQueuePane(self.manager)

    @property
    def badge(self):
        """The per-tab badge of the terminal this window shows (its runner mode, queued count, next prompt).

        Returns:
            A TabBadge.
        """
        from .badge import TabBadge
        return TabBadge(self)

    @property
    def sessions(self):
        """The Sessions page (open it from the Agent Manager toolbar with `sessions.open()`).

        Returns:
            A SessionsPage.
        """
        from .sessions import SessionsPage
        return SessionsPage(self)

    def home(self) -> uia.Element:
        """The Home button (its name is its face: "Agent Manager" or "Jump back to <title>").

        Returns:
            The element.
        Raises:
            LookupError: When the strip has no Home button.
        """
        el = self.tabs.element().find(aid="ManagerHomeButton", max_depth=2)
        if el is None:
            raise LookupError("no Home button")
        return el

    def page_showing(self) -> bool:
        """Whether the Agent Manager page shows (the window title says so and no tab is selected).

        Returns:
            True on the page.
        """
        return self.title in TITLE_ON_PAGE and self.tabs.selected() is None

    def dialog(self, title: Optional[str] = None):
        """An open ContentDialog in this window.

        Args:
            title: Its title (None = any).
        Returns:
            A Dialog, or None.
        """
        from .dialogs import Dialog
        return Dialog.find_in(self, title=title)

    def wait_title(self, title: str, timeout_s: float = 10.0) -> str:
        """Wait for the window title.

        Args:
            title: The expected title.
            timeout_s: How long.
        Returns:
            The title.
        Raises:
            WaitTimeout: When it never read so.
        """
        return wait_until(lambda: self.title if self.title == title else None, timeout_s,
                          what=f"window title {title!r}", describe=lambda: self.title)
