"""Task dialogs: the Win32 dialogs (TaskDialogIndirect, comctl32 v6) Agentmaster shows BEFORE its first window exists.

The portable copy's first launch asks "Where should Agentmaster live?" (PortableInstall.h `ShowInstallPrompt`), a kept-
portable copy then asks which profile to use (ProfileBootstrap.h `ShowProfilePicker`), `--uninstall-portable` asks
"Uninstall Agentmaster?" or says "Nothing to uninstall here", and the install runs under a marquee dialog ("Installing
Agentmaster…"). All of them are top-level windows of class `#32770` titled "Agentmaster", shown by an Agentmaster.exe
that has no XAML window yet — so they are found by WINDOW, not inside an app window like a ContentDialog
(`ui/dialogs.py`).

UI Automation sees a task dialog's main instruction and content as text elements and each command link as a button.
A command link's label is the text before the first line break of its TASKDIALOG_BUTTON text; Windows shows the rest as
the link's note, which UI Automation gives as the button's HelpText (also its legacy Description), never as a text
element or in the name (probed 2026-10-24: `Button.note`). Agentmaster marks the pre-selected option by the label itself
("… (recommended)"), which is what a user reads, so a test checks that instead of the keyboard focus.

⚠ A task dialog's process may be another account's or elevated; with UAC off (the test desktops) every process of the
signed-in administrator is elevated, so the suite reads and presses every dialog it starts.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

import comtypes
import psutil

from .. import uia, win32
from ..waits import wait_until

DIALOG_CLASS = "#32770"
"""The window class of a Win32 dialog (message boxes and task dialogs alike)."""
APP_TITLE = "Agentmaster"
"""The window title every Agentmaster task dialog carries (pszWindowTitle)."""


@dataclass(frozen=True)
class Button:
    """One button of a task dialog: a command link or a common button (OK, Cancel, Yes, No).

    Attributes:
        name: Its whole UI Automation name (a command link's: its label alone).
        label: The name's first line — a command link's label without its note.
        element: The UI Automation element (a snapshot; press through TaskDialog.press, which reads it again).
        note: A command link's note, the text Windows shows under its label (UI Automation's HelpText); "" for a
            common button.
    """

    name: str
    label: str
    element: uia.Element
    note: str = ""


def _exe_dir_of(pid: int) -> Optional[Path]:
    """The folder of a process's image.

    Args:
        pid: The process id.
    Returns:
        The folder, or None when the process is gone or its image cannot be read.
    """
    try:
        exe = psutil.Process(pid).exe()
    except (psutil.NoSuchProcess, psutil.AccessDenied, OSError):
        return None
    return Path(exe).parent if exe else None


def _under(folder: Optional[Path], roots: Iterable[Path]) -> bool:
    """Whether a folder is one of the roots or inside one (case-insensitive, as Windows compares paths).

    Args:
        folder: The folder (None never matches).
        roots: The roots.
    Returns:
        True when inside.
    """
    if folder is None:
        return False
    f = str(folder).rstrip("\\").lower()
    for root in roots:
        r = str(root).rstrip("\\").lower()
        if f == r or f.startswith(r + "\\"):
            return True
    return False


class TaskDialog:
    """An open task dialog.

    Attributes:
        hwnd: Its window.
        pid: The process that shows it.
        title: Its window title.
    """

    def __init__(self, hwnd: int, pid: int, title: str) -> None:
        """Bind to a dialog window.

        Args:
            hwnd: The dialog's window.
            pid: Its process.
            title: Its window title.
        Returns:
            None.
        """
        self.hwnd = hwnd
        self.pid = pid
        self.title = title

    def __repr__(self) -> str:
        """A short description.

        Returns:
            The text.
        """
        return f"TaskDialog(pid={self.pid}, {self.title!r}, {self.instruction()!r})"

    # ---- finding --------------------------------------------------------------------------------

    @staticmethod
    def all(pids: Optional[Iterable[int]] = None, exe_dirs: Optional[Iterable[Path]] = None,
            title: Optional[str] = APP_TITLE) -> list["TaskDialog"]:
        """Every visible dialog window that fits the filters.

        Args:
            pids: Only these processes (None = any).
            exe_dirs: Only processes whose image lies in one of these folders (None = any). A copy started through
                Explorer gets a pid the test does not know; its folder is.
            title: Only dialogs with this window title (None = any).
        Returns:
            The dialogs, topmost first.
        """
        pid_set = set(pids) if pids is not None else None
        roots = [Path(d) for d in exe_dirs] if exe_dirs is not None else None
        found = []
        for w in win32.top_windows():
            if w.cls != DIALOG_CLASS or not w.visible:
                continue
            if title is not None and w.title != title:
                continue
            if pid_set is not None and w.pid not in pid_set:
                continue
            if roots is not None and not _under(_exe_dir_of(w.pid), roots):
                continue
            found.append(TaskDialog(w.hwnd, w.pid, w.title))
        return found

    @staticmethod
    def find(instruction: Optional[str] = None, instruction_re: Optional[str] = None,
             pids: Optional[Iterable[int]] = None, exe_dirs: Optional[Iterable[Path]] = None) -> Optional["TaskDialog"]:
        """The first dialog whose main instruction (or any of its texts) fits.

        Args:
            instruction: The exact main instruction ("Where should Agentmaster live?").
            instruction_re: A regex searched in every text of the dialog instead.
            pids: As in all().
            exe_dirs: As in all().
        Returns:
            The dialog, or None.
        """
        for d in TaskDialog.all(pids, exe_dirs):
            texts = d.texts()
            if instruction is not None and instruction not in texts:
                continue
            if instruction_re is not None and not any(re.search(instruction_re, t) for t in texts):
                continue
            return d
        return None

    @staticmethod
    def wait(instruction: Optional[str] = None, instruction_re: Optional[str] = None,
             pids: Optional[Iterable[int]] = None, exe_dirs: Optional[Iterable[Path]] = None,
             timeout_s: float = 60.0) -> "TaskDialog":
        """Wait for a dialog to appear.

        Args:
            instruction: As in find().
            instruction_re: As in find().
            pids: As in all().
            exe_dirs: As in all().
            timeout_s: How long.
        Returns:
            The dialog.
        Raises:
            WaitTimeout: When none appeared; the message lists the dialogs that were open.
        """
        pid_list = list(pids) if pids is not None else None
        dir_list = list(exe_dirs) if exe_dirs is not None else None
        return wait_until(lambda: TaskDialog.find(instruction, instruction_re, pid_list, dir_list), timeout_s,
                          interval_s=0.3, what=f"a task dialog {instruction or instruction_re!r}",
                          describe=lambda: [repr(d) for d in TaskDialog.all(title=None)])

    # ---- reading --------------------------------------------------------------------------------

    def element(self) -> uia.Element:
        """The dialog's UI Automation element.

        Returns:
            The element.
        Raises:
            comtypes.COMError: When the dialog is gone.
        """
        return uia.Element.from_hwnd(self.hwnd)

    def is_open(self) -> bool:
        """Whether the dialog window still exists and shows.

        Returns:
            True while open.
        """
        return bool(win32.IsWindow(self.hwnd)) and bool(win32.IsWindowVisible(self.hwnd))

    def texts(self) -> list[str]:
        """Every text the dialog shows (main instruction, content, footer, the links' notes).

        Returns:
            The texts in tree order; empty when the dialog closed under the read.
        """
        try:
            return self.element().texts(12)
        except comtypes.COMError:
            return []

    def owner(self) -> str:
        """The account of the process that shows the dialog (DOMAIN\\user).

        A copy restarted for another account shows the same question from a new process: the owner tells the two apart.

        Returns:
            The account, or "" when the process is gone or unreadable.
        """
        try:
            return psutil.Process(self.pid).username() or ""
        except (psutil.NoSuchProcess, psutil.AccessDenied, OSError):
            return ""

    def instruction(self) -> str:
        """The main instruction: the dialog's first text.

        Returns:
            It, or "" when the dialog shows none (or closed).
        """
        texts = self.texts()
        return texts[0] if texts else ""

    def text(self) -> str:
        """Every text of the dialog and the names of its buttons with their notes, one per line — what a user can read
        in it.

        Returns:
            The text.
        """
        return "\n".join(self.texts() + [b.name + (f"\n{b.note}" if b.note else "") for b in self.buttons()])

    def buttons(self) -> list[Button]:
        """The dialog's buttons in order: its command links, then its common buttons.

        Returns:
            The buttons (empty when the dialog closed under the read).
        """
        try:
            found = self.element().find_all(12, role="button")
        except comtypes.COMError:
            return []
        out = []
        for el in found:
            if not el.name:
                continue  # the title bar's own buttons are unnamed on some builds
            label = el.name.replace("\r\n", "\n").split("\n", 1)[0].strip()
            out.append(Button(el.name, label, el, el.help_text))
        return out

    def labels(self) -> list[str]:
        """The buttons' labels.

        Returns:
            The labels, in order.
        """
        return [b.label for b in self.buttons()]

    def _radios(self) -> list[uia.Element]:
        """The dialog's radio buttons (TASKDIALOGCONFIG pRadioButtons), in order.

        Returns:
            Their elements (empty when the dialog has none, or closed under the read).
        """
        try:
            return [el for el in self.element().find_all(12, role="radio button") if el.name]
        except comtypes.COMError:
            return []

    def choices(self) -> list[str]:
        """The labels of the dialog's radio buttons (the updater's prompt: "Remind me next restart" … "Skip this version").

        Returns:
            The labels, in order.
        """
        return [el.name for el in self._radios()]

    def chosen(self) -> str:
        """The radio button selected now.

        Returns:
            Its label, or "" when none is.
        """
        return next((el.name for el in self._radios() if el.selected), "")

    def choose(self, label_re: str) -> str:
        """Select the first radio button whose label matches, the way a click on it does (UI Automation's
        SelectionItem pattern), and check that it took.

        Args:
            label_re: A regex searched in the label.
        Returns:
            The selected label.
        Raises:
            LookupError: When no radio button matches (the message lists them), or the selection did not take.
        """
        for el in self._radios():
            if re.search(label_re, el.name):
                el.select()
                if self.chosen() != el.name:
                    raise LookupError(f"selecting {el.name!r} did not take: {self.chosen()!r} is selected")
                return el.name
        raise LookupError(f"no radio button matches {label_re!r}: {self.choices()}")

    def button(self, label_re: str) -> Optional[Button]:
        """The first button whose label matches.

        Args:
            label_re: A regex searched in the label.
        Returns:
            The button, or None.
        """
        for b in self.buttons():
            if re.search(label_re, b.label):
                return b
        return None

    def checkbox(self) -> Optional[uia.Element]:
        """The verification check box ("Also remove the installed Agentmaster package …"), when the dialog has one.

        Returns:
            Its element, or None.
        """
        try:
            return self.element().find(12, role="check box")
        except comtypes.COMError:
            return None

    # ---- acting ---------------------------------------------------------------------------------

    def press(self, label_re: str, wait_close: bool = True, timeout_s: float = 15.0) -> str:
        """Press a button: the dialog comes to the front, then the button is invoked (UI Automation's Invoke presses a
        command link exactly as a click does; MessageBox.press works the same way).

        Args:
            label_re: A regex searched in the button's label.
            wait_close: Wait for the dialog to close afterwards (a task dialog closes on every press of a command link
                Agentmaster defines; "Browse…" opens a folder dialog first).
            timeout_s: How long the close may take.
        Returns:
            The pressed button's label.
        Raises:
            LookupError: When no button matches (the message lists the labels).
            WaitTimeout: When the dialog stayed open.
        """
        btn = self.button(label_re)
        if btn is None:
            raise LookupError(f"{self!r} has no button matching {label_re!r}; it has {self.labels()}")
        win32.set_foreground(self.hwnd)
        btn.element.invoke()
        if wait_close:
            wait_until(lambda: not self.is_open(), timeout_s, what=f"{self!r} to close after {btn.label!r}")
        return btn.label

    def wait_closed(self, timeout_s: float = 60.0) -> None:
        """Wait for the dialog to close by itself (a progress dialog ends when its work is done).

        Args:
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When it stayed open.
        """
        wait_until(lambda: not self.is_open(), timeout_s, interval_s=0.5, what=f"{self!r} to close")

    def dump(self) -> str:
        """The dialog's UI Automation subtree (evidence).

        Returns:
            The text, or the error when the dialog is gone.
        """
        try:
            return self.element().dump(14, 400)
        except comtypes.COMError as error:
            return f"(gone: {error})"
