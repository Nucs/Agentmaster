"""The per-tab link badge: the small HUD at the top right of a Claude tab's terminal (AgentTabOverlay, TAB_OVERLAY.md),
read for what it says about the Prompt Queue Runner (flows/03 PQR).

What it shows (AgentTabOverlay.cpp `_Refresh`):
    row 1   the model, the status (a colored glyph and its word), the action buttons (folder, copy, pencil, mail),
            then " · " separators around the runner's MODE BUTTON — "Auto", "Paused", "Paused → Auto" (the
            interrupt hold), each followed by " (Off globally)" while the toolbar's global pause is on — and, while
            prompts are queued, "⏳N" (how many are Pending)
    row 2   the working folder and the branch (dim)
    row 3   while a prompt is Pending: "⏳ <its first line>", led by "❓ held — answer the question  " when the last
            turn ended with a question (the question guard holds the queue), or by "settling  " while the 40 s
            settle window runs (DELIVERY.md §16; never while the global pause is on)
Every part is a TextBlock (the mode a Button holding one), so UI Automation sees them as texts and a button whose name
is the mode. They are siblings of the TermControl in the content element's tree; this reader keeps those whose center
lies in the terminal's top band.
"""
from __future__ import annotations

import re
from typing import Optional

from .. import uia
from ..waits import wait_until

MODE_RE = re.compile(r"^(Auto|Paused|Paused → Auto)( \(Off globally\))?$")
"""The mode button's name: the session's own mode, the global pause's suffix after it."""
QUEUED_RE = re.compile(r"^⏳(\d+)$")
"""Row 1's queued count."""
PROMPT_LINE_RE = re.compile(r"^(❓ held — answer the question\s+|settling\s+)?⏳ (.*)$", re.S)
"""Row 3: an optional cue, then the hourglass and the next queued prompt's first line."""
TOP_BAND_PX = 140
"""How far below the terminal's top edge the badge's rows can reach (three rows of 11–12 pt text, wrapped)."""


class TabBadge:
    """The badge of the terminal a window shows.

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

    def elements(self) -> list[uia.Element]:
        """The elements of the badge: the content's elements whose center lies in the terminal's top band (the
        terminal control itself excluded).

        Returns:
            The elements, in tree order ([] when no terminal shows — the Agent Manager page, a closed tab).
        """
        try:
            term = self.window.terminal.element()
        except LookupError:
            return []
        x, y, w, h = term.rect
        out = []
        for _, el in self.window.content().walk(max_depth=6):
            if el.class_name == "TermControl" or not el.visible:
                continue
            cx, cy = el.center
            if x <= cx < x + w and y <= cy < y + min(h, TOP_BAND_PX):
                out.append(el)
        return out

    def texts(self) -> list[str]:
        """The badge's texts and button names, in tree order (the evidence a failed check carries).

        Returns:
            The names.
        """
        return [el.name for el in self.elements() if el.name and el.role in ("text", "button")]

    def mode_button(self) -> Optional[uia.Element]:
        """The runner's mode button.

        Returns:
            The button, or None (no badge: a shell tab, or the page shows).
        """
        return next((el for el in self.elements() if el.role == "button" and MODE_RE.match(el.name)), None)

    def mode(self) -> str:
        """What the mode button says ("Auto", "Paused (Off globally)", "Paused → Auto" …).

        Returns:
            The text ("" when there is no badge).
        """
        btn = self.mode_button()
        return btn.name if btn is not None else ""

    def wait_mode(self, mode: str, timeout_s: float = 15.0) -> str:
        """Wait for the mode button to say a text.

        Args:
            mode: The exact text.
            timeout_s: How long.
        Returns:
            The text.
        Raises:
            WaitTimeout: When it never did (the message lists the badge's texts).
        """
        return wait_until(lambda: (lambda m: m if m == mode else None)(self.mode()), timeout_s,
                          what=f"the badge's mode to read {mode!r}", describe=self.texts)

    def click_mode(self, timeout_s: float = 10.0) -> str:
        """Click the mode button (Paused ↔ Auto) and wait for its new text.

        Args:
            timeout_s: How long the change may take.
        Returns:
            The new text.
        Raises:
            LookupError: When there is no badge.
            WaitTimeout: When the text did not change.
        """
        btn = self.mode_button()
        if btn is None:
            raise LookupError(f"no badge mode button; the badge: {self.texts()}")
        before = btn.name
        btn.click()
        return wait_until(lambda: (lambda m: m if m and m != before else None)(self.mode()), timeout_s,
                          what=f"the badge's mode to change from {before!r}", describe=self.texts)

    def queued(self) -> int:
        """Row 1's "⏳N": how many prompts are queued.

        Returns:
            N (0 when no count shows).
        """
        for t in self.texts():
            m = QUEUED_RE.match(t)
            if m:
                return int(m.group(1))
        return 0

    def prompt_line(self) -> str:
        """Row 3: the next queued prompt's line with its cue ("" when nothing is queued).

        Returns:
            The text.
        """
        return next((t for t in self.texts() if PROMPT_LINE_RE.match(t)), "")

    def cue(self) -> str:
        """Row 3's cue: "held" (the question guard), "settling" (the settle window), or "" (none, or nothing queued).

        Returns:
            The cue.
        """
        m = PROMPT_LINE_RE.match(self.prompt_line())
        if not m or not m.group(1):
            return ""
        return "held" if m.group(1).startswith("❓") else "settling"
