"""A tab's terminal: read its screen, find Claude Code's input box, type into it.

The terminal (a TermControl) exposes its buffer through the UI Automation TextPattern: rows joined by CRLF,
the scrollback included (measured 2026-10-23: 51 rows read in ~10 ms). Claude Code 2.1.291's inline renderer
draws its input box at the bottom of that text:

    ──────────────────────────────── (a full-width rule)
    ❯ <the unsent draft>             (U+276F, then a no-break space)
    ──────────────────────────────── (a rule)
      ⏵⏵ bypass permissions on …     (the status line)

which is the shape Agentmaster's own pending-input detector keys on (PendingInput.h: the bottom-most ❯ line
wrapped by flush-left rules). A box on screen means Claude Code is up and reading keys; the text after ❯ is
the draft. ⚠ The fullscreen renderer (CLAUDE_CODE_NO_FLICKER=1, the "renderer" lane) draws the same box in
the alternate screen; the text pattern then reads the alternate screen, and the parser holds.

Typing goes to the focused control as real key events (inputs.type_text); a prompt is confirmed by the MOCK
(the model received it), never by the screen alone.
"""
from __future__ import annotations

import re
import time
from dataclasses import dataclass
from typing import Optional

from . import inputs, uia
from .waits import wait_until

RULE = "─"
CARET = "❯"
NBSP = " "


@dataclass
class InputBox:
    """Claude Code's input box as read from the screen.

    Attributes:
        draft: The unsent text in the box ("" when empty); continuation rows joined with newlines.
        caret_row: The row index of the ❯ line in the text.
    """

    draft: str
    caret_row: int


def _is_rule(line: str) -> bool:
    """Whether a row is one of the box's rules (flush-left, at least 20 rule characters).

    Args:
        line: The row.
    Returns:
        True for a rule row.
    """
    return line.startswith(RULE * 20)


def parse_input_box(lines: list[str]) -> Optional[InputBox]:
    """Find the bottom-most input box in the screen rows.

    Args:
        lines: The rows, top to bottom.
    Returns:
        The box, or None when there is none (Claude Code not started, a menu or dialog on screen, a shell).
    """
    for i in range(len(lines) - 1, 0, -1):
        row = lines[i]
        if not row.startswith(CARET) or not _is_rule(lines[i - 1]):
            continue
        j = i + 1
        while j < len(lines) and not _is_rule(lines[j]):
            j += 1
        if j >= len(lines):
            continue  # no closing rule below: a sent prompt's echo, not the box
        first = row[len(CARET):].lstrip(NBSP + " ")
        rest = [r[2:] if r.startswith("  ") else r for r in lines[i + 1:j]]
        draft = "\n".join([first] + rest).rstrip()
        return InputBox(draft=draft, caret_row=i)
    return None


class Terminal:
    """The terminal shown in a window (the selected tab's focused pane).

    Attributes:
        window: The MainWindow page object.
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
        """The visible terminal control (the focused one when the tab is split).

        Returns:
            Its element.
        Raises:
            LookupError: When no terminal is on screen (the Agent Manager page shows, or the tab is not a
                terminal).
        """
        controls = [e for e in self.window.content().find_all(cls="TermControl", max_depth=6) if e.visible]
        if not controls:
            raise LookupError("no terminal on screen (is the Agent Manager page showing?)")
        focused = [e for e in controls if e.has_focus]
        return (focused or sorted(controls, key=lambda e: -e.rect[2] * e.rect[3]))[0]

    def text(self) -> str:
        """The terminal's whole text (scrollback and screen).

        Returns:
            The text, rows joined with CRLF.
        Raises:
            LookupError: When no terminal is on screen.
        """
        return self.element().text()

    def lines(self) -> list[str]:
        """The terminal's rows, trailing spaces removed.

        Returns:
            The rows.
        Raises:
            LookupError: When no terminal is on screen.
        """
        return [row.rstrip() for row in re.split(r"\r\n|\n", self.text())]

    def input_box(self) -> Optional[InputBox]:
        """Claude Code's input box, when one is on screen.

        Returns:
            The box or None.
        """
        try:
            return parse_input_box(self.lines())
        except LookupError:
            return None

    def wait_input_box(self, timeout_s: float = 60.0) -> InputBox:
        """Wait for Claude Code's input box (it is up and reads keys).

        Args:
            timeout_s: How long (a first start of claude.exe in a fresh config folder takes several seconds).
        Returns:
            The box.
        Raises:
            WaitTimeout: When it never appeared; the message holds the screen's last rows.
        """
        return wait_until(self.input_box, timeout_s, interval_s=0.3, what="Claude Code's input box",
                          describe=lambda: self.lines()[-8:])

    def wait_text(self, pattern: str, timeout_s: float = 30.0) -> re.Match:
        """Wait for a regex to appear in the terminal's text.

        Args:
            pattern: The regex.
            timeout_s: How long.
        Returns:
            The match.
        Raises:
            WaitTimeout: When it never appeared.
        """
        rx = re.compile(pattern)
        return wait_until(lambda: rx.search(self.text()), timeout_s, interval_s=0.3, what=f"/{pattern}/ on screen",
                          describe=lambda: self.lines()[-8:])

    def focus(self) -> None:
        """Click into the terminal (bottom-left area, clear of the per-tab badge at the top right).

        Returns:
            None.
        Raises:
            LookupError: When no terminal is on screen.
        """
        x, y, w, h = self.element().rect
        inputs.click(x + min(60, w // 4), y + h - 40)
        time.sleep(0.15)

    def type(self, text: str, newline_key: str = "shift+enter") -> None:
        """Type text into the terminal (it must hold the focus; focus() first). A newline inside the text is a
        line break inside one Claude prompt (Shift+Enter), not a submit.

        Args:
            text: The text.
            newline_key: The key a newline presses.
        Returns:
            None.
        """
        inputs.type_text(text, delay_s=0.004, newline_key=newline_key)

    def submit(self, text: str) -> None:
        """Type a prompt into Claude Code's box and press Enter (no confirmation: Session.ask confirms
        through the mock).

        Args:
            text: The prompt.
        Returns:
            None.
        Raises:
            LookupError: When no terminal is on screen.
        """
        self.focus()
        self.type(text)
        wait_until(lambda: (lambda box: box is not None and box.draft.endswith(text.strip()[-20:]))(self.input_box()),
                   10.0, interval_s=0.1, what="the typed prompt in the input box",
                   describe=lambda: self.lines()[-6:])
        inputs.keys("enter")
