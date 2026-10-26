"""The Prompt Queue Runner's surfaces on the Agent Manager page: the Prompt Queue tab of the right pane (the selected
session's history and queue, its runner toggle, Send now) and the toolbar's global pause (flows/03 PQR).

What the tab holds (AgentManagerContent.AutoTesting.cpp `_RebuildPlan`; the layout in AgentManagerContent.cpp):
    toggle       a button whose content is a dot and the label "Prompt Queue Runner: Auto" / "…: Paused" /
                 "…: Paused → Auto on your next message" (the interrupt hold) — on 0.8.0 the button has NO name
                 (its content is a panel), so it is found by the label among its texts; a build with the
                 accessibility metadata (2026-10-23, not 0.8.0) names it by the label, gives it the id
                 `am.mgr.queue.runner` and the mode alone as its item status
    header       the session's title, its state pill and its working folder (texts)
    the list     a scroll viewer: the caption "SENT — N  (K typed)", one button per sent prompt (texts: "✓" or
                 "✕" for a failed one, the prompt's label = its first 56 characters, the pill "auto" or
                 "typed"), then the caption "UPCOMING — N" and one button per queued prompt (texts: "⏳", the
                 label, the gate "[turn-done]"); "No messages yet. …" when both are empty
    compose      the edit named by its placeholder and its icons (ManagerPage.compose_box / compose_buttons):
                 Jump to tab, Send now ("!": asks "Send now?" with the prompt quoted, Send · Cancel), Add to queue
    templates    the paper icon at the box's top right shows a row (collapsed at first): the name box (named by its
                 placeholder "queue template name"), "Save as queue template", the saved templates' combo box (the
                 last saved one selected), "Apply" and "Apply to dir" (AgentManagerContent.AutoTesting.cpp
                 _OnSaveTemplate / _OnApplyTemplate)
The toolbar's last button reads "Pause Prompt Queue Runner", and "Resume Prompt Queue Runner" while the ONE process-wide
pause is on (every window shows the same word: Scheduler::SetGlobalPause calls each window's listener).

The rows and captions are read from the list's own elements; nothing here reads sessions.json (ROB-K1: a read can
make the app's save fail), so a test asserts the queue the user sees.
"""
from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Optional

from .. import uia
from ..waits import wait_until

RUNNER_PREFIX = "Prompt Queue Runner: "
"""The toggle's label starts with this; the mode follows."""
RUNNER_MODES = ("Auto", "Paused", "Paused → Auto on your next message")
"""What the toggle can say after the prefix: Auto, Paused, and the interrupt hold (DELIVERY.md §14)."""
RUNNER_ID = "am.mgr.queue.runner"
"""The toggle's AutomationId on a build with the accessibility metadata (not 0.8.0)."""
PAUSE_ALL = "Pause Prompt Queue Runner"
"""The toolbar button while the global pause is off."""
RESUME_ALL = "Resume Prompt Queue Runner"
"""The same button while the global pause is on."""
SENT_CAPTION = re.compile(r"^SENT — (\d+)\s+\((\d+) typed\)$")
"""The history's caption: how many sent, how many of them typed in the terminal."""
UPCOMING_CAPTION = re.compile(r"^UPCOMING — (\d+)$")
"""The queue's caption."""
GLYPH_STATUS = {"✓": "sent", "⏳": "pending", "✕": "failed", "⛔": "held", "—": "skipped"}
"""A row's first text -> the prompt's status (AgentManagerContent.Internal.h PromptGlyph)."""
TEMPLATES_ID = "am.mgr.queue.templates"
"""The paper icon's AutomationId on a build with the accessibility metadata (not 0.8.0)."""
TEMPLATE_NAME_ID = "am.mgr.queue.templateName"
"""The template name box's AutomationId on such a build."""
TEMPLATE_NAME_PLACEHOLDER = "queue template name"
"""The template name box's placeholder — its accessible name on 0.8.0."""
SAVE_TEMPLATE = "Save as queue template"
"""The templates row's save button (its content is its label, so its name)."""


@dataclass
class QueueRow:
    """One prompt of the Prompt Queue tab.

    Attributes:
        el: The row's button (a snapshot).
        section: "sent" (the history) or "upcoming" (the queue).
        texts: Its texts in order (glyph, label, pill or gate, and "(held: …)" for a held one).
    """

    el: uia.Element
    section: str
    texts: list = field(default_factory=list)

    @property
    def glyph(self) -> str:
        """The status glyph (✓ ⏳ ✕ ⛔ —).

        Returns:
            The glyph ("" when the row shows none).
        """
        return self.texts[0] if self.texts else ""

    @property
    def status(self) -> str:
        """The prompt's status as the glyph says it: "sent", "pending", "failed", "held", "skipped".

        Returns:
            The status ("" for an unknown glyph).
        """
        return GLYPH_STATUS.get(self.glyph, "")

    @property
    def label(self) -> str:
        """The row's label: the prompt's first 56 characters, line breaks as spaces (a typed prompt's label is the
        text Claude Code recorded).

        Returns:
            The label.
        """
        return self.texts[1] if len(self.texts) > 1 else ""

    @property
    def tag(self) -> str:
        """The row's third text: "auto" / "typed" on a sent row (who sent it), the gate ("[turn-done]") on a queued one.

        Returns:
            The tag ("" when absent).
        """
        return self.texts[2] if len(self.texts) > 2 else ""

    def click(self) -> None:
        """Select the row (a click; Send now then targets it when the compose box is empty).

        Returns:
            None.
        """
        self.el.click()

    def right_click(self) -> None:
        """Open the row's menu (Copy on every row; Move up / Move down / Delete on a queued one).

        Returns:
            None.
        """
        self.el.right_click()


class PromptQueuePane:
    """The Prompt Queue tab of a window's Agent Manager page.

    Attributes:
        page: The ManagerPage.
    """

    def __init__(self, page) -> None:
        """Bind to a page.

        Args:
            page: The ManagerPage.
        Returns:
            None.
        """
        self.page = page

    # ---- showing ----------------------------------------------------------------------------------

    def open(self, title: str) -> "PromptQueuePane":
        """Show the page, select a session's board card and pick the Prompt Queue tab — the way a user gets to a
        session's queue.

        Args:
            title: The session's title (its card's title).
        Returns:
            self.
        Raises:
            WaitTimeout: When the board shows no such card within 20 s (a reopened window fills its board as its sessions
                load), or the tab never showed that session.
        """
        self.page.open()
        self.page.wait_card(title, timeout_s=20.0).click()
        if not self.showing():
            self.page.show_tab("Prompt Queue")
        wait_until(lambda: self.showing() and title in self.header_texts(), 10.0,
                   what=f"the Prompt Queue tab showing {title!r}", describe=self.header_texts)
        return self

    def showing(self) -> bool:
        """Whether the Prompt Queue tab shows (its runner toggle is on the page).

        Returns:
            True when it shows.
        """
        return self._toggle() is not None

    def header_texts(self) -> list[str]:
        """The texts between the toggle and the list: the selected session's title, state pill and folder.

        Returns:
            The texts ([] while the tab does not show).
        """
        out, seen = [], False
        for el in self.page.controls():
            if not seen:
                seen = self._is_toggle(el)
                continue
            if el.role == "pane" and el.class_name == "ScrollViewer":
                break
            if el.role == "text" and el.name:
                out.append(el.name)
        return out

    # ---- the runner toggle ------------------------------------------------------------------------

    @staticmethod
    def _toggle_label(el: uia.Element) -> str:
        """The toggle's label from its name (a build with the metadata) or its texts (0.8.0).

        Args:
            el: A candidate button.
        Returns:
            The label ("" when the element is not the toggle).
        """
        if el.name.startswith(RUNNER_PREFIX):
            return el.name
        if el.role != "button":
            return ""
        return next((t for t in el.texts(max_depth=3) if t.startswith(RUNNER_PREFIX)), "")

    def _is_toggle(self, el: uia.Element) -> bool:
        """Whether an element is the runner toggle.

        Args:
            el: The element.
        Returns:
            True for the toggle.
        """
        return el.automation_id == RUNNER_ID or bool(self._toggle_label(el))

    def _toggle(self) -> Optional[uia.Element]:
        """The runner toggle.

        Returns:
            The button, or None while the tab does not show.
        """
        return next((el for el in self.page.controls() if self._is_toggle(el)), None)

    def mode(self) -> str:
        """The selected session's runner mode as the toggle says it: "Auto", "Paused", or "Paused → Auto on your
        next message" (the interrupt hold).

        Returns:
            The mode.
        Raises:
            LookupError: When the tab does not show.
        """
        el = self._toggle()
        if el is None:
            raise LookupError("no runner toggle: is the Prompt Queue tab showing?")
        return self._toggle_label(el)[len(RUNNER_PREFIX):]

    def click_toggle(self, timeout_s: float = 10.0) -> str:
        """Click the runner toggle (Paused ↔ Auto; from the interrupt hold, to Auto) and wait for the new mode.

        Args:
            timeout_s: How long the change may take.
        Returns:
            The new mode.
        Raises:
            LookupError: When the tab does not show.
            WaitTimeout: When the mode did not change.
        """
        before = self.mode()
        self._toggle().click()
        return wait_until(lambda: (lambda m: m if m != before else None)(self.mode()), timeout_s,
                          what=f"the runner toggle to change from {before!r}")

    def wait_mode(self, mode: str, timeout_s: float = 15.0) -> str:
        """Wait for the toggle to show a mode.

        Args:
            mode: One of RUNNER_MODES.
            timeout_s: How long.
        Returns:
            The mode.
        Raises:
            WaitTimeout: When it never did.
        """
        return wait_until(lambda: (lambda m: m if m == mode else None)(self.mode()), timeout_s,
                          what=f"the runner toggle to read {mode!r}", describe=self.mode)

    # ---- the list ---------------------------------------------------------------------------------

    def _list(self) -> Optional[uia.Element]:
        """The list's scroll viewer (the first one after the toggle).

        Returns:
            The element, or None.
        """
        seen = False
        for el in self.page.controls():
            if not seen:
                seen = self._is_toggle(el)
                continue
            if el.role == "pane" and el.class_name == "ScrollViewer":
                return el
        return None

    def list_texts(self) -> list[str]:
        """Every text of the list (captions and rows), top to bottom.

        Returns:
            The texts ([] while the tab does not show).
        """
        view = self._list()
        return view.texts(max_depth=8) if view is not None else []

    def rows(self) -> list[QueueRow]:
        """The rows, top to bottom, each with its section (the caption above it).

        Returns:
            The rows.
        """
        view = self._list()
        if view is None:
            return []
        out, section = [], ""
        for el in view.children():
            if el.role == "text":
                if SENT_CAPTION.match(el.name):
                    section = "sent"
                elif UPCOMING_CAPTION.match(el.name):
                    section = "upcoming"
            elif el.role == "button":
                out.append(QueueRow(el, section, el.texts(max_depth=4)))
        return out

    def sent(self) -> list[QueueRow]:
        """The history (the SENT section), oldest first.

        Returns:
            The rows.
        """
        return [r for r in self.rows() if r.section == "sent"]

    def upcoming(self) -> list[QueueRow]:
        """The queue (the UPCOMING section), in delivery order.

        Returns:
            The rows.
        """
        return [r for r in self.rows() if r.section == "upcoming"]

    def counts(self) -> tuple[int, int, int]:
        """The captions' numbers: (sent, of them typed, upcoming) — 0 for a caption that is not shown.

        Returns:
            The three numbers.
        """
        sent = typed = upcoming = 0
        for t in self.list_texts():
            m = SENT_CAPTION.match(t)
            if m:
                sent, typed = int(m.group(1)), int(m.group(2))
            m = UPCOMING_CAPTION.match(t)
            if m:
                upcoming = int(m.group(1))
        return sent, typed, upcoming

    def row(self, prefix: str, section: Optional[str] = None) -> QueueRow:
        """A row by the start of its label.

        Args:
            prefix: The label's start (a prompt's first words).
            section: "sent" / "upcoming" / None (either).
        Returns:
            The row.
        Raises:
            LookupError: When there is no such row (the message lists the rows).
        """
        for r in self.rows():
            if r.label.startswith(prefix) and (section is None or r.section == section):
                return r
        raise LookupError(f"no {section or ''} row {prefix!r}; rows: {[(r.section, r.label) for r in self.rows()]}")

    def wait_row(self, prefix: str, section: str, timeout_s: float = 20.0) -> QueueRow:
        """Wait for a row to be in a section (a queued prompt moving to SENT, a new row appearing).

        Args:
            prefix: The label's start.
            section: "sent" or "upcoming".
            timeout_s: How long.
        Returns:
            The row.
        Raises:
            WaitTimeout: When it never got there.
        """
        def found():
            """One look.

            Returns:
                The row, or None.
            """
            return next((r for r in self.rows() if r.label.startswith(prefix) and r.section == section), None)

        return wait_until(found, timeout_s, what=f"row {prefix!r} in {section}",
                          describe=lambda: [(r.section, r.label, r.tag) for r in self.rows()])

    def row_menu(self, row: QueueRow):
        """Open a row's context menu: Copy on every row; Move up, Move down and Delete on a queued one.

        Args:
            row: The row (from rows() / row()).
        Returns:
            The open Menu.
        Raises:
            WaitTimeout: When no menu opened.
        """
        from .menus import Menu
        app = self.page.window.app
        before = Menu.open_hwnds(app)
        self.page.window.focus()
        row.right_click()
        return Menu.wait_new(app, before)

    # ---- queue, Send now --------------------------------------------------------------------------

    def queue(self, *prompts: str) -> float:
        """Queue prompts for the selected session (the compose row's Add to queue, one per prompt) and wait until
        each shows in UPCOMING.

        Args:
            *prompts: The prompts (each may carry its `[[mock …]]` script).
        Returns:
            The time (time.time()) just before the first was queued — a settle window is measured from it.
        Raises:
            LookupError: When the compose row does not show.
            WaitTimeout: When a row never appeared.
        """
        t0 = time.time()
        for prompt in prompts:
            self.page.queue_prompt(prompt)
            # A row's label is the prompt's first characters with its line breaks shown as spaces (the engine's
            # label, _OnAddPrompt), so a multi-line prompt is found by that form of its start.
            self.wait_row(prompt[:30].replace("\r", " ").replace("\n", " "), "upcoming", timeout_s=10.0)
        return t0

    def send_now(self, text: Optional[str] = None):
        """Click Send now ("!"), with `text` in the compose box (set through UI Automation) or with an empty box
        (then the selected — or the first queued — row is the one it sends), and return its confirmation.

        Args:
            text: The prompt to compose first (None = leave the box as it is).
        Returns:
            The "Send now?" Dialog (press "Send" or "Cancel").
        Raises:
            LookupError: When the compose row does not show.
            WaitTimeout: When no confirmation asked.
        """
        from .dialogs import Dialog
        if text is not None:
            self.page.compose_box().set_value(text)
        buttons = self.page.compose_buttons()
        if len(buttons) != 3:
            raise LookupError(f"expected the 3 compose icons left of the box, found {[b.line() for b in buttons]}")
        buttons[1].click()
        return Dialog.wait(self.page.window.app, title="Send now?", timeout_s=10.0)

    def compose_text(self) -> str:
        """The compose box's text now.

        Returns:
            The text.
        """
        return self.page.compose_box().value

    # ---- queue templates --------------------------------------------------------------------------

    def _templates_button(self) -> uia.Element:
        """The compose row's paper icon, which shows / hides the templates row: by its AutomationId on a build with
        the accessibility metadata (TEMPLATES_ID), else — 0.8.0, a nameless glyph button — the visible button at the
        compose box's top right (its only neighbor there while the session has no unsent draft to pull in).

        Returns:
            The button.
        Raises:
            LookupError: When the compose row does not show or no such button is there.
        """
        controls = self.page.controls()
        by_id = next((el for el in controls if el.automation_id == TEMPLATES_ID), None)
        if by_id is not None:
            return by_id
        bx, by, bw, _bh = self.page.compose_box().rect
        right = [el for el in controls if el.role == "button" and el.visible and el.rect[0] >= bx + bw - 4
                 and abs(el.rect[1] - by) <= 14 and el.rect[0] - (bx + bw) < 120]
        if not right:
            raise LookupError("no templates (paper) icon right of the compose box")
        return max(right, key=lambda el: el.rect[0])  # the paper icon is the right-most (the pull-draft one is left)

    def templates_showing(self) -> bool:
        """Whether the templates row shows (its "Save as queue template" button is on the page).

        Returns:
            True when it shows.
        """
        return any(el.role == "button" and el.name == SAVE_TEMPLATE for el in self.page.controls())

    def show_templates(self, timeout_s: float = 10.0) -> None:
        """Open the templates row (the paper icon) unless it already shows.

        Args:
            timeout_s: How long the row may take to show.
        Returns:
            None.
        Raises:
            LookupError: When the paper icon is not found.
            WaitTimeout: When the row did not show.
        """
        if self.templates_showing():
            return
        self._templates_button().click()
        wait_until(self.templates_showing, timeout_s, what="the templates row")

    def _template_button(self, name: str) -> uia.Element:
        """One of the templates row's text buttons ("Save as queue template", "Apply", "Apply to dir").

        Args:
            name: Its label (its name).
        Returns:
            The button.
        Raises:
            LookupError: When the row does not show it.
        """
        for el in self.page.controls():
            if el.role == "button" and el.name == name:
                return el
        raise LookupError(f"no {name!r} button: is the templates row open?")

    def save_template(self, name: str) -> None:
        """Save the selected session's queue as a template: the name typed into the row's name box (set through UI
        Automation), then "Save as queue template". The saved template becomes the row's selected one.

        Args:
            name: The template's name ("" = the app's default, "<session>-plan").
        Returns:
            None.
        Raises:
            LookupError: When the row or its name box does not show.
        """
        self.show_templates()
        box = next((el for el in self.page.controls() if el.role == "edit" and
                    (el.automation_id == TEMPLATE_NAME_ID or el.name == TEMPLATE_NAME_PLACEHOLDER)), None)
        if box is None:
            raise LookupError("no template name box in the templates row")
        box.set_value(name)
        self._template_button(SAVE_TEMPLATE).click()

    def apply_template(self, to_folder: bool = False) -> None:
        """Apply the row's selected template: "Apply" (append its prompts to the selected session's queue) or
        "Apply to dir" (to every session of that session's working folder).

        Args:
            to_folder: True for "Apply to dir".
        Returns:
            None.
        Raises:
            LookupError: When the row does not show.
        """
        self.show_templates()
        self._template_button("Apply to dir" if to_folder else "Apply").click()


# ---- the toolbar's global pause ---------------------------------------------------------------------


def global_pause_button(page) -> uia.Element:
    """The toolbar's Pause / Resume Prompt Queue Runner button.

    Args:
        page: The ManagerPage (it must show).
    Returns:
        The button.
    Raises:
        LookupError: When the page does not show it.
    """
    for el in page.controls():
        if el.role == "button" and el.name in (PAUSE_ALL, RESUME_ALL):
            return el
    raise LookupError("no Pause / Resume Prompt Queue Runner button: does the page show?")


def global_paused(page) -> bool:
    """Whether the window's toolbar says the global pause is on ("Resume Prompt Queue Runner").

    Args:
        page: The ManagerPage.
    Returns:
        True while it reads Resume.
    """
    return global_pause_button(page).name == RESUME_ALL


def set_global_pause(page, on: bool, timeout_s: float = 10.0) -> None:
    """Click the toolbar's button when it does not already say `on`, and wait for its new word.

    Args:
        page: The ManagerPage (shown first if hidden).
        on: True to pause every session's automatic send, False to resume.
        timeout_s: How long the change may take.
    Returns:
        None.
    Raises:
        WaitTimeout: When the button did not change.
    """
    page.open()
    if global_paused(page) == on:
        return
    global_pause_button(page).click()
    wait_until(lambda: global_paused(page) == on, timeout_s,
               what=f"the toolbar to read {RESUME_ALL if on else PAUSE_ALL!r}")
