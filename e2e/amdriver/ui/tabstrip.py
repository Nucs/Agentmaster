"""The tab strip: the tabs, their titles and selection, Home, and each tab's context menu.

Measured (0.8.0): `#TabView` → [`#ManagerHomeButton`, `#TabListView` (a ListView) → tab items (role "tab item",
class ListViewItem, NAMED BY THE TAB TITLE, `selected` set on the selected one) → [`#HeaderTextBlock` (the
title), `#CloseButton` "Close Tab"]]. While a tab is renamed, its title text is replaced by an edit
`#HeaderRenamerTextBox`. While the Agent Manager page shows, no tab item is selected.

⚠ The strip VIRTUALIZES (CLAUDE.md Gotchas): a tab scrolled far out of view is not in the tree at all. With the
handful of tabs a test opens every tab is present; a flow with hundreds of tabs must reveal a tab first.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from .. import inputs, uia
from ..waits import wait_until


@dataclass
class Tab:
    """One tab as read from the strip.

    Attributes:
        el: Its element (a snapshot).
        title: The tab title.
        selected: Whether it is the selected tab.
        index: Its position in the strip (0 = left-most).
    """

    el: uia.Element
    title: str
    selected: bool
    index: int

    def click(self, modifiers: Optional[list] = None) -> None:
        """Click the tab with the real pointer (on its title, clear of the close button).

        Args:
            modifiers: Keys held (["shift"] = Shift+Click, the "Activate Tab" gesture).
        Returns:
            None.
        Raises:
            RuntimeError: When the tab is not on screen.
        """
        x, y, w, h = self.el.rect
        self.el.click(modifiers=modifiers, offset=(min(40, w // 3), h // 2))

    def right_click(self) -> None:
        """Right-click the tab (opens its context menu).

        Returns:
            None.
        Raises:
            RuntimeError: When the tab is not on screen.
        """
        x, y, w, h = self.el.rect
        self.el.click("right", offset=(min(40, w // 3), h // 2))

    def close_button(self) -> uia.Element:
        """The tab's ✕.

        Returns:
            The element.
        Raises:
            LookupError: When the tab has none (a tab that cannot close).
        """
        el = self.el.find(aid="CloseButton", max_depth=2)
        if el is None:
            raise LookupError(f"tab {self.title!r} has no close button")
        return el


class TabStrip:
    """A window's tab strip.

    Attributes:
        window: The MainWindow.
    """

    def __init__(self, window) -> None:
        """Bind to a window.

        Args:
            window: The MainWindow.
        Returns:
            None.
        """
        self.window = window

    def element(self) -> uia.Element:
        """The `#TabView`.

        Returns:
            The element.
        Raises:
            LookupError: When the window has no tab view.
        """
        for child in self.window.input_site().children():
            if child.automation_id == "TabView":
                return child
        raise LookupError("no #TabView")

    def items(self) -> list[Tab]:
        """Every tab in the strip, left to right.

        Returns:
            The tabs.
        """
        listview = self.element().find(aid="TabListView", max_depth=2)
        if listview is None:
            return []
        items = [el for el in listview.children() if el.role == "tab item"]
        return [Tab(el, el.name, el.selected, i) for i, el in enumerate(items)]

    def titles(self) -> list[str]:
        """The tab titles, left to right.

        Returns:
            The titles.
        """
        return [t.title for t in self.items()]

    def find(self, title: str) -> Optional[Tab]:
        """A tab by its exact title.

        Args:
            title: The title.
        Returns:
            The tab, or None.
        """
        return next((t for t in self.items() if t.title == title), None)

    def get(self, title: str) -> Tab:
        """A tab by its exact title, which must exist.

        Args:
            title: The title.
        Returns:
            The tab.
        Raises:
            LookupError: When there is no such tab (the message lists the titles).
        """
        tab = self.find(title)
        if tab is None:
            raise LookupError(f"no tab {title!r}; the tabs: {self.titles()}")
        return tab

    def selected(self) -> Optional[Tab]:
        """The selected tab.

        Returns:
            It, or None (the Agent Manager page shows).
        """
        return next((t for t in self.items() if t.selected), None)

    def wait_tab(self, title: str, timeout_s: float = 20.0) -> Tab:
        """Wait for a tab to exist.

        Args:
            title: The exact title.
            timeout_s: How long.
        Returns:
            The tab.
        Raises:
            WaitTimeout: When it never appeared.
        """
        return wait_until(lambda: self.find(title), timeout_s, what=f"tab {title!r}", describe=self.titles)

    def wait_gone(self, title: str, timeout_s: float = 20.0) -> None:
        """Wait for a tab to leave the strip.

        Args:
            title: The exact title.
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When it stayed.
        """
        wait_until(lambda: self.find(title) is None, timeout_s, what=f"tab {title!r} gone", describe=self.titles)

    def wait_selected(self, title: str, timeout_s: float = 15.0) -> Tab:
        """Wait for a tab to be the selected one.

        Args:
            title: The exact title.
            timeout_s: How long.
        Returns:
            The tab.
        Raises:
            WaitTimeout: When another (or none) stayed selected.
        """
        return wait_until(lambda: (lambda t: t if t is not None and t.title == title else None)(self.selected()),
                          timeout_s, what=f"tab {title!r} selected",
                          describe=lambda: (self.selected().title if self.selected() else None))

    def select(self, title: str) -> Tab:
        """Click a tab and wait until it is selected.

        Args:
            title: The exact title.
        Returns:
            The tab.
        Raises:
            LookupError: When there is no such tab.
            WaitTimeout: When the click did not select it.
        """
        self.window.focus()
        self.get(title).click()
        return self.wait_selected(title)

    def context_menu(self, title: str):
        """Right-click a tab and return its context menu.

        Args:
            title: The exact title.
        Returns:
            The open Menu.
        Raises:
            LookupError: When there is no such tab.
            WaitTimeout: When no menu opened.
        """
        from .menus import Menu
        self.window.focus()
        before = Menu.open_hwnds(self.window.app)
        self.get(title).right_click()
        return Menu.wait_new(self.window.app, before)

    def close(self, title: str, answer: Optional[str] = "Close", timeout_s: float = 20.0) -> None:
        """Close a tab with its ✕ and answer the confirmation.

        Every tab asks first (0.8.0): a session tab "Close session?" (Close · ★ Favorite & Close · Cancel), a
        shell tab "Close tab?" (Close · Cancel).

        Args:
            title: The tab.
            answer: The button to press ("Close", "★ Favorite & Close", "Cancel"); None leaves the dialog open.
            timeout_s: How long the tab may take to leave the strip (not waited for with "Cancel" / None).
        Returns:
            None.
        Raises:
            LookupError: When there is no such tab.
            WaitTimeout: When no dialog asked or the tab stayed.
        """
        from .dialogs import Dialog
        self.window.focus()
        self.get(title).close_button().click()
        if answer is None:
            return
        Dialog.wait(self.window.app, title_re=r"^Close (session|tab)\?$").press(answer)
        if answer != "Cancel":
            self.wait_gone(title, timeout_s)

    def rename_box(self) -> Optional[uia.Element]:
        """The inline rename box of the tab being renamed.

        Returns:
            The edit, or None when no tab is being renamed.
        """
        return self.element().find(aid="HeaderRenamerTextBox", max_depth=4)

    def rename(self, title: str, new_title: str, how: str = "menu", commit_key: str = "shift+enter") -> None:
        """Rename a tab the way a user does: open the inline box, replace the text, commit.

        ⚠ The default commit mode (AppSettings::tabRenameCommitMode = ClickAwayOrShiftEnter) commits on
        Shift+Enter or a click outside; a plain Enter types a LINE BREAK into the title (multi-line titles). The cog's
        Enter mode commits on Enter.

        Args:
            title: The tab's current title.
            new_title: The new title.
            how: "menu" (Rename tab (F2) in the context menu) or "f2" (select the tab, press F2).
            commit_key: The key that commits ("shift+enter" in the default mode; "enter" in the Enter mode).
        Returns:
            None.
        Raises:
            WaitTimeout: When the box never opened.
        """
        if how == "menu":
            self.context_menu(title).choose("Rename tab (F2)")
        else:
            self.select(title)
            inputs.keys("f2")
        box = wait_until(self.rename_box, 5.0, what="the tab's rename box")
        box.focus()
        inputs.keys("ctrl+a")
        inputs.type_text(new_title)
        inputs.keys(commit_key)
        wait_until(lambda: self.rename_box() is None, 5.0, what="the rename box to close")
