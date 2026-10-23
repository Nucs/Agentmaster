"""Context menus and their submenus (MenuFlyout), the way a user works them: open, read, hover, choose, Esc.

Measured (0.8.0): a context menu opens in its OWN top-level popup window (class Xaml_WindowedPopupClass,
title "PopupHost") of the app's process: `window 'Popup'` → `menu` (class MenuFlyout) → items, each a `menu
item` of class MenuFlyoutItem (a command) or MenuFlyoutSubItem (a submenu; ExpandCollapse state "collapsed" /
"expanded"). A submenu opens in ANOTHER popup window, over the first. A hidden item (Visibility Collapsed — the
tab menu hides every session item on a shell tab) is ABSENT from the tree; a disabled one is present with
IsEnabled false. Separators are not reported.

A popup window can stay alive after its menu closed, so "open" means: a visible popup window that holds a
MenuFlyout with a non-empty rectangle.

⚠ A menu SLIDES into place when it opens (the flyout's open animation, a few hundred ms): rectangles read in
that time are where the items WERE, and a click there lands on the item above (measured 2026-10-23: "Mark
Unread" clicked for "Open In Explorer", "Move left" for "Move to start"). Every action therefore reads the
items until two reads agree (stable_items) before it points at one.
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional

import comtypes

from .. import inputs, uia, win32
from ..waits import wait_until

POPUP_CLASS = "Xaml_WindowedPopupClass"


@dataclass
class MenuItem:
    """One menu item as read.

    Attributes:
        el: Its element (a snapshot).
        name: The item's text.
        enabled: IsEnabled.
        is_sub: A submenu (MenuFlyoutSubItem).
        expanded: Its submenu is open.
    """

    el: uia.Element
    name: str
    enabled: bool
    is_sub: bool
    expanded: bool


class Menu:
    """One open menu level (a MenuFlyout in its popup window).

    Attributes:
        app: The App.
        hwnd: The popup window that holds it.
    """

    def __init__(self, app, hwnd: int) -> None:
        """Bind to an open menu's popup window.

        Args:
            app: The App.
            hwnd: The popup window.
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
        try:
            return f"Menu(0x{self.hwnd:x} {self.names()})"
        except Exception:  # noqa: BLE001 - a closed menu still prints
            return f"Menu(0x{self.hwnd:x} closed)"

    # ---- finding menus ----------------------------------------------------------------------------

    @staticmethod
    def _menu_in(hwnd: int) -> Optional[uia.Element]:
        """The visible MenuFlyout inside a popup window.

        Args:
            hwnd: The popup window.
        Returns:
            The `menu` element, or None.
        """
        try:
            el = uia.Element.from_hwnd(hwnd)
        except comtypes.COMError:
            return None
        menu = el.find(role="menu", cls="MenuFlyout", max_depth=4)
        return menu if menu is not None and menu.rect[2] > 0 and menu.rect[3] > 0 else None

    @staticmethod
    def open_hwnds(app) -> set[int]:
        """The popup windows that hold an open menu now.

        Args:
            app: The App.
        Returns:
            Their handles.
        """
        if not app.alive():
            return set()
        return {w.hwnd for w in win32.top_windows(app.pid)
                if w.cls == POPUP_CLASS and w.visible and Menu._menu_in(w.hwnd) is not None}

    @staticmethod
    def open_menus(app) -> list["Menu"]:
        """Every open menu level, topmost first (a submenu before its parent).

        Args:
            app: The App.
        Returns:
            The menus.
        """
        if not app.alive():
            return []
        return [Menu(app, w.hwnd) for w in win32.top_windows(app.pid)
                if w.cls == POPUP_CLASS and w.visible and Menu._menu_in(w.hwnd) is not None]

    @staticmethod
    def wait_new(app, before: set[int], timeout_s: float = 5.0) -> "Menu":
        """Wait for a menu that was not open before an action.

        Args:
            app: The App.
            before: open_hwnds() taken before the action.
            timeout_s: How long.
        Returns:
            The new menu.
        Raises:
            WaitTimeout: When none opened.
        """
        hwnd = wait_until(lambda: next(iter(Menu.open_hwnds(app) - before), None), timeout_s, interval_s=0.1,
                          what="a menu to open")
        return Menu(app, hwnd)

    @staticmethod
    def dismiss_all(app, max_presses: int = 5) -> None:
        """Close every open menu with Esc (one level per press).

        Args:
            app: The App.
            max_presses: The most Esc presses.
        Returns:
            None.
        Raises:
            AssertionError: When menus stay open.
        """
        for _ in range(max_presses):
            if not Menu.open_hwnds(app):
                return
            inputs.keys("esc")
            time.sleep(0.25)
        if Menu.open_hwnds(app):
            raise AssertionError(f"menus stay open: {Menu.open_menus(app)}")

    # ---- reading ----------------------------------------------------------------------------------

    def element(self) -> uia.Element:
        """The `menu` element.

        Returns:
            The element.
        Raises:
            LookupError: When the menu closed.
        """
        menu = self._menu_in(self.hwnd)
        if menu is None:
            raise LookupError(f"menu 0x{self.hwnd:x} is closed")
        return menu

    def is_open(self) -> bool:
        """Whether this menu is still open.

        Returns:
            True while open.
        """
        return self._menu_in(self.hwnd) is not None

    def items(self) -> list[MenuItem]:
        """The menu's items, top to bottom (hidden ones are absent).

        Returns:
            The items.
        Raises:
            LookupError: When the menu closed.
        """
        out = []
        for el in self.element().children():
            if el.role != "menu item":
                continue
            out.append(MenuItem(el, el.name, el.enabled, el.class_name == "MenuFlyoutSubItem", el.expand_state == 1))
        return out

    def stable_items(self, timeout_s: float = 2.0, settle_s: float = 0.08) -> list[MenuItem]:
        """The items once the menu stopped moving (its open animation is over).

        Args:
            timeout_s: How long to wait for two equal reads.
            settle_s: The pause between the reads.
        Returns:
            The items of the last read.
        Raises:
            LookupError: When the menu closed.
        """
        end = time.monotonic() + timeout_s
        last = self.items()
        while time.monotonic() < end:
            time.sleep(settle_s)
            now = self.items()
            if [(i.name, i.el.rect) for i in now] == [(i.name, i.el.rect) for i in last]:
                return now
            last = now
        return last

    def names(self) -> list[str]:
        """The items' texts, top to bottom.

        Returns:
            The names.
        """
        return [i.name for i in self.items()]

    def has(self, name: str) -> bool:
        """Whether an item is shown.

        Args:
            name: Its text.
        Returns:
            True when present.
        """
        return name in self.names()

    def item(self, name: str, stable: bool = False) -> MenuItem:
        """An item by its text.

        Args:
            name: The text.
            stable: Read it only once the menu stopped moving (before pointing at it).
        Returns:
            The item.
        Raises:
            LookupError: When it is not shown (the message lists what is).
        """
        items = self.stable_items() if stable else self.items()
        for it in items:
            if it.name == name:
                return it
        raise LookupError(f"no menu item {name!r}; the menu: {[i.name for i in items]}")

    def enabled(self, name: str) -> bool:
        """Whether an item is enabled.

        Args:
            name: Its text.
        Returns:
            IsEnabled.
        Raises:
            LookupError: When it is not shown.
        """
        return self.item(name).enabled

    # ---- acting -----------------------------------------------------------------------------------

    def choose(self, name: str, wait_close: bool = True) -> None:
        """Click a command item (the menu closes, the command runs).

        Args:
            name: The item's text.
            wait_close: Wait until every menu closed.
        Returns:
            None.
        Raises:
            LookupError: When it is not shown.
            AssertionError: When it is disabled or a submenu (use submenu()).
            WaitTimeout: When the menus did not close.
        """
        it = self.item(name, stable=True)
        if it.is_sub:
            raise AssertionError(f"{name!r} is a submenu: use submenu({name!r})")
        if not it.enabled:
            raise AssertionError(f"menu item {name!r} is disabled")
        it.el.click()
        if wait_close:
            wait_until(lambda: not Menu.open_hwnds(self.app), 5.0, interval_s=0.1, what="the menus to close")

    def submenu(self, name: str, timeout_s: float = 4.0) -> "Menu":
        """Open a submenu by hovering its item (UI Automation's Expand if hovering does not open it).

        Args:
            name: The submenu item's text.
            timeout_s: How long to wait for the submenu.
        Returns:
            The submenu.
        Raises:
            LookupError: When it is not shown.
            AssertionError: When it is not a submenu or is disabled.
            WaitTimeout: When it never opened, or the menu that opened is not this item's.
        """
        it = self.item(name, stable=True)
        if not it.is_sub:
            raise AssertionError(f"{name!r} is not a submenu")
        if not it.enabled:
            raise AssertionError(f"submenu {name!r} is disabled")
        before = Menu.open_hwnds(self.app) - {self.hwnd}
        it.el.hover()
        try:
            sub = Menu.wait_new(self.app, before | {self.hwnd}, timeout_s=timeout_s / 2)
        except AssertionError:
            self.item(name).el.expand()
            sub = Menu.wait_new(self.app, before | {self.hwnd}, timeout_s=timeout_s / 2)
        # The submenu must be THIS item's: its item reads expanded (a wrong hover opens a neighbor's).
        wait_until(lambda: self.item(name).expanded, 2.0, interval_s=0.1, what=f"{name!r} expanded",
                   describe=lambda: [(i.name, i.expanded) for i in self.items()])
        sub.stable_items()
        return sub

    def choose_path(self, *names: str) -> None:
        """Walk submenus and choose the last item: choose_path("Close", "Close other tabs").

        Args:
            *names: The submenu names, then the command's.
        Returns:
            None.
        Raises:
            LookupError / AssertionError / WaitTimeout: As submenu() and choose().
        """
        menu: Menu = self
        for sub in names[:-1]:
            menu = menu.submenu(sub)
        menu.choose(names[-1])

    def close(self) -> None:
        """Close every open menu (Esc).

        Returns:
            None.
        Raises:
            AssertionError: When menus stay open.
        """
        Menu.dismiss_all(self.app)
