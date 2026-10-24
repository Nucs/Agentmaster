"""The Sessions page: every Claude Code session on disk in one table, opened from the Agent Manager toolbar's
"Sessions" button (SESSIONS.md; TerminalPage.AgentSessionsPage.cpp).

What it is to UI Automation (measured on 0.8.0 with the probe pqprobe1, 2026-10-24): the page is laid over the Agent
Manager page, so its controls FOLLOW the page's in the window's content element (the Manager's stay in the tree):
    header   button "←  Back" · text "Claude Code Sessions" · text "<N> sessions · 1 month" · edit "search for
             sessions" (with its "AI" toggle) · the scope toggles 👤 🤖 📁 📄 🏷 F · check boxes Open / Hidden / Favorite
             · "📂 Folders ▾" · "1 month" · "↻"
    columns  buttons "Title", "Directory", "Branch", "Created", "Active ▼", "Msgs·Tools", "Ctx" (the sort)
    table    a scroll viewer whose children are the rows' CELLS as texts, row after row: "☆" (or "★"), the title, the
             folder, the branch, created, active, "msgs·tools", ctx. A row is a XAML Border, which has no automation
             peer, so it is no element: a click at its title cell lands on the row (every cell but the star lets the
             pointer through) and selects it.
    detail   the next scroll viewer: the selected row's title, its timing and weight, and its actions — "Jump to tab"
             (the session is open in this window) or "Resume here" (it is not), "Fork here", "Open New Session Here"
Resume here logs `[nav] sessions resume-click row=<sid8> …` and, once the tab opened, `[nav] sessions resume-done
new=<sid8>` (`(target was <sid8> — fresh)` when there was no transcript to resume).
"""
from __future__ import annotations

import time
from typing import Optional

from .. import uia
from ..waits import wait_until

BACK = "←  Back"
"""The page's Back button (an arrow, two spaces)."""
TITLE_COLUMN = "Title"
"""The table's first sortable column header: the table's scroll viewer follows the headers."""


class SessionsPage:
    """A window's Sessions page.

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

    def _kids(self) -> list[uia.Element]:
        """The window content's direct children (the page's controls are among them, after the Manager page's).

        Returns:
            The elements.
        """
        return self.window.content_children()

    def showing(self) -> bool:
        """Whether the page shows (its Back button is on screen).

        Returns:
            True while it shows.
        """
        return any(el.role == "button" and el.name == BACK and el.visible for el in self._kids())

    def open(self, timeout_s: float = 20.0) -> "SessionsPage":
        """Open the page from the Agent Manager toolbar's "Sessions" button.

        Args:
            timeout_s: How long.
        Returns:
            self.
        Raises:
            WaitTimeout: When the page never showed.
        """
        if not self.showing():
            page = self.window.manager.open()
            mark = self.window.app.profile.hooks.mark()
            page.control("Sessions", role="button").click()
            self.window.app.profile.hooks.wait(r"\[nav\] sessions-page shown", since=mark, timeout_s=timeout_s)
        wait_until(self.showing, timeout_s, what="the Sessions page")
        return self

    def _panes(self) -> tuple[Optional[uia.Element], Optional[uia.Element]]:
        """The table's and the detail pane's scroll viewers (the first two after the Title column header).

        Returns:
            (table, detail); None for one that is not there.
        """
        kids = self._kids()
        start = next((i for i, el in enumerate(kids) if el.role == "button" and el.name == TITLE_COLUMN), None)
        if start is None:
            return None, None
        views = [el for el in kids[start + 1:] if el.role == "pane" and el.class_name == "ScrollViewer"]
        return (views[0] if views else None), (views[1] if len(views) > 1 else None)

    def rows(self) -> list[list[str]]:
        """The table's rows as their cell texts: each row starts with its star (☆ or ★).

        Returns:
            One list of texts per row, top to bottom.
        """
        table, _detail = self._panes()
        if table is None:
            return []
        out: list[list[str]] = []
        for el in table.children():
            if el.role != "text":
                continue
            if el.name in ("☆", "★"):
                out.append([el.name])
            elif out:
                out[-1].append(el.name)
        return out

    def titles(self) -> list[str]:
        """The titles the table lists, top to bottom.

        Returns:
            The titles.
        """
        return [r[1] for r in self.rows() if len(r) > 1]

    def title_cell(self, title: str) -> Optional[uia.Element]:
        """A row's title cell (its text element: the row itself has no element).

        Args:
            title: The session's title.
        Returns:
            The text element, or None when no row shows that title.
        """
        table, _detail = self._panes()
        if table is None:
            return None
        kids = table.children()
        for i, el in enumerate(kids):
            # A row's title is the text right after its star cell.
            if el.role == "text" and el.name == title and i > 0 and kids[i - 1].name in ("☆", "★"):
                return el
        return None

    def wait_row(self, title: str, timeout_s: float = 30.0) -> uia.Element:
        """Wait until a row shows a session (the table renders after an off-thread gather).

        Args:
            title: The session's title.
            timeout_s: How long.
        Returns:
            Its title cell.
        Raises:
            WaitTimeout: When it never showed (the message lists the titles shown).
        """
        return wait_until(lambda: self.title_cell(title), timeout_s, what=f"a Sessions row {title!r}",
                          describe=self.titles)

    def action(self, name: str) -> Optional[uia.Element]:
        """A detail-pane action ("Resume here", "Jump to tab", "Fork here", "Open New Session Here").

        Args:
            name: Its text.
        Returns:
            The button, or None.
        """
        _table, detail = self._panes()
        if detail is None:
            return None
        return next((el for _, el in detail.walk(max_depth=6)
                     if el.role in ("button", "split button") and el.name == name and el.visible), None)

    def select(self, title: str, timeout_s: float = 15.0) -> None:
        """Click a row (through its title cell) and wait for its detail pane's actions.

        Args:
            title: The session's title.
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When the row never showed or its actions never appeared.
        """
        cell = self.wait_row(title, timeout_s)
        cell.click(offset=(min(6, max(1, cell.rect[2] // 2)), cell.rect[3] // 2))
        wait_until(lambda: self.action("Resume here") or self.action("Jump to tab"), timeout_s,
                   what=f"the detail pane's actions for {title!r}")

    def resume(self, title: str, timeout_s: float = 60.0) -> str:
        """Resume a closed session: select its row, click "Resume here", wait for the engine's resume-done.

        Args:
            title: The session's title.
            timeout_s: How long the resume may take.
        Returns:
            The opened session's 8-character id (resume-done's new=).
        Raises:
            LookupError: When the row offers no "Resume here" (the session is open: "Jump to tab").
            WaitTimeout: When the resume never completed.
        """
        self.select(title)
        btn = self.action("Resume here")
        if btn is None:
            raise LookupError(f"the Sessions row {title!r} offers no 'Resume here' (is the session open?)")
        mark = self.window.app.profile.hooks.mark()
        btn.click()
        m = self.window.app.profile.hooks.wait(r"\[nav\] sessions resume-done new=([0-9a-f]{8})", since=mark,
                                               timeout_s=timeout_s)
        time.sleep(0.3)
        return m.group(1)

    def back(self) -> None:
        """Close the page with its Back button.

        Returns:
            None.
        """
        btn = next((el for el in self._kids() if el.role == "button" and el.name == BACK), None)
        if btn is not None:
            btn.click()
            wait_until(lambda: not self.showing(), 10.0, what="the Sessions page to close")
