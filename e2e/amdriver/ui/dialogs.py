"""Dialogs: XAML ContentDialogs inside a window, and Win32 message boxes.

ContentDialog (measured, 0.8.0, the close-tab confirm): a `window` element of class Popup NAMED BY THE DIALOG'S
TITLE ("Close session?"), a child of the window's InputSite pane; inside it a ScrollViewer `#ContentScrollViewer`
with the title and the body as TextBlocks, and the buttons `#PrimaryButton` / `#SecondaryButton` /
`#CloseButton` named by their texts ("Close", "★ Favorite & Close", "Cancel").

Message box: the "Reopen your N previous Agentmaster windows?" prompt is a Win32 MessageBox (class #32770,
title "Agentmaster", Yes / No) shown BEFORE the first window exists (WindowEmperor.cpp).
"""
from __future__ import annotations

import re
from typing import Optional

from .. import uia, win32
from ..waits import wait_until

MESSAGE_BOX_CLASS = "#32770"


class Dialog:
    """An open ContentDialog.

    Attributes:
        window: The MainWindow that shows it.
        title: Its title.
    """

    def __init__(self, window, title: str) -> None:
        """Bind to a dialog by its title.

        Args:
            window: The MainWindow.
            title: The dialog's title.
        Returns:
            None.
        """
        self.window = window
        self.title = title

    def __repr__(self) -> str:
        """A short description.

        Returns:
            The text.
        """
        return f"Dialog({self.title!r})"

    @staticmethod
    def _candidates(window) -> list[uia.Element]:
        """The dialog-like popups of a window (a `window` element of class Popup with a name).

        Args:
            window: The MainWindow.
        Returns:
            The elements.
        """
        try:
            return [el for el in window.input_site().children()
                    if el.role == "window" and el.class_name == "Popup" and el.name and el.name != "Popup"]
        except LookupError:
            return []

    @staticmethod
    def find_in(window, title: Optional[str] = None, title_re: Optional[str] = None) -> Optional["Dialog"]:
        """An open dialog in one window.

        Args:
            window: The MainWindow.
            title: The exact title (None = any).
            title_re: A regex searched in the title.
        Returns:
            The Dialog, or None.
        """
        for el in Dialog._candidates(window):
            if title is not None and el.name != title:
                continue
            if title_re is not None and not re.search(title_re, el.name):
                continue
            return Dialog(window, el.name)
        return None

    @staticmethod
    def find(app, title: Optional[str] = None, title_re: Optional[str] = None) -> Optional["Dialog"]:
        """An open dialog in any of the app's windows.

        Args:
            app: The App.
            title: The exact title (None = any).
            title_re: A regex searched in the title.
        Returns:
            The Dialog, or None.
        """
        for window in app.windows():
            found = Dialog.find_in(window, title, title_re)
            if found is not None:
                return found
        return None

    @staticmethod
    def wait(app, title: Optional[str] = None, title_re: Optional[str] = None, timeout_s: float = 10.0) -> "Dialog":
        """Wait for a dialog.

        Args:
            app: The App.
            title: The exact title (None = any).
            title_re: A regex searched in the title.
            timeout_s: How long.
        Returns:
            The Dialog.
        Raises:
            WaitTimeout: When none appeared.
        """
        return wait_until(lambda: Dialog.find(app, title, title_re), timeout_s,
                          what=f"a dialog {title or title_re or ''!r}")

    def element(self) -> uia.Element:
        """The dialog's element.

        Returns:
            The element.
        Raises:
            LookupError: When the dialog closed.
        """
        for el in self._candidates(self.window):
            if el.name == self.title:
                return el
        raise LookupError(f"{self!r} is closed")

    def is_open(self) -> bool:
        """Whether the dialog still shows.

        Returns:
            True while open.
        """
        return any(el.name == self.title for el in self._candidates(self.window))

    def text(self) -> str:
        """The dialog's texts (title and body), joined with newlines.

        Returns:
            The text.
        """
        return "\n".join(self.element().texts())

    def buttons(self) -> list[str]:
        """The dialog's button texts, in order (primary, secondary, close).

        Returns:
            The names.
        """
        return [el.name for el in self.element().find_all(role="button") if el.visible]

    def press(self, name: str, wait_close: bool = True) -> None:
        """Click a button by its text.

        Args:
            name: The button's text ("Close", "Cancel", "★ Favorite & Close" ...).
            wait_close: Wait until the dialog closed.
        Returns:
            None.
        Raises:
            LookupError: When there is no such button (the message lists the buttons).
            WaitTimeout: When the dialog stayed open.
        """
        btn = self.element().find(role="button", name=name)
        if btn is None:
            raise LookupError(f"{self!r} has no button {name!r}; buttons: {self.buttons()}")
        btn.click()
        if wait_close:
            wait_until(lambda: not self.is_open(), 10.0, what=f"{self!r} to close")


class MessageBox:
    """A Win32 message box of the app's process.

    Attributes:
        hwnd: Its window.
        title: Its window title.
    """

    def __init__(self, hwnd: int, title: str) -> None:
        """Bind to a message box.

        Args:
            hwnd: Its window.
            title: Its title.
        Returns:
            None.
        """
        self.hwnd = hwnd
        self.title = title

    @staticmethod
    def find(pid: int, text_re: Optional[str] = None) -> Optional["MessageBox"]:
        """A visible message box of a process.

        Args:
            pid: The process.
            text_re: A regex its text must match (None = any).
        Returns:
            The MessageBox, or None.
        """
        for w in win32.top_windows(pid):
            if w.cls != MESSAGE_BOX_CLASS or not w.visible:
                continue
            box = MessageBox(w.hwnd, w.title)
            if text_re is None or re.search(text_re, box.text()):
                return box
        return None

    def text(self) -> str:
        """The box's texts.

        Returns:
            The texts joined with newlines.
        """
        try:
            return "\n".join(uia.Element.from_hwnd(self.hwnd).texts())
        except Exception:  # noqa: BLE001 - a box closing under the read reads as empty
            return ""

    def press(self, name: str) -> None:
        """Click a button by its text ("Yes", "No", "OK").

        Args:
            name: The button text.
        Returns:
            None.
        Raises:
            LookupError: When there is no such button.
        """
        el = uia.Element.from_hwnd(self.hwnd)
        btn = el.find(role="button", name=name) or el.find(role="button", name_re=rf"^&?{re.escape(name)}$")
        if btn is None:
            raise LookupError(f"message box {self.title!r} has no button {name!r}")
        win32.set_foreground(self.hwnd)
        btn.invoke()
