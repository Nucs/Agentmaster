"""The Agent Manager page: Home, the launch row, the toolbar, the Triage Board, the Explorer Tree, the right pane.

Measured (0.8.0, the page showing in a maximized 1920x1033 window, the side panel docked left): the page's
controls are DIRECT children of the window's content element, in this order —
    launch row   text 'Agentmaster' · the agent toggle (a button holding '●' 'Claude') · edit 'working directory
                 (the M axis)' · button 'Launch Claude' (or 'Create & Launch Claude' / 'Resume session' +
                 'Fork' / 'Launch Codex')
    toolbar      a nameless button (the ⚙ cog) · 'Sessions' · 'Keep Awake' · 'Pause Prompt Queue Runner' ·
                 'Reopen Windows (N)' (only when N > 0)
    board        text 'TRIAGE BOARD' · 'LOCAL'/'GLOBAL' · the sort ('MOST ACTIVE' …) · '↻' · 'Clear' (with a
                 selection) · 'Show all' (with a folder scope) · a ScrollViewer holding, per column, text '●' ·
                 text <column name> · text <count> · a ScrollViewer of CARDS (nameless buttons whose texts are:
                 title, folder, timing, model, 'ctx …', '⚙ sent/total', and a nameless ⋯ button)
    tree         text 'EXPLORER TREE' · 'LOCAL'/'GLOBAL'/'EXTERNAL' · the sort ('NEWEST' …) · '↻' · a
                 ScrollViewer of rows: folder rows (texts '▾'/'▸', the folder, the count) and session rows
                 (texts: title, state word, timing)
    right pane   'Summary' · 'Prompt Queue' · a ScrollViewer with the selected session's content
— and the side panel's controls follow in the same list, so the page reads only the elements OUTSIDE the
side panel's horizontal band.

The board has no UI Automation selection: which card is selected is read from its effects (Home's face, the
right pane, the side panel's peek).
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Optional

from .. import inputs, uia, win32
from ..waits import wait_until

COLUMNS = ("Running", "Waiting-for-you", "Needs-approval", "Error", "Idle / Done", "External")
"""The board's column names, left to right (Error and External appear only with content)."""
STATE_WORDS = ("running", "waiting-for-you", "needs-approval", "error", "done", "idle", "observe-only")
"""The state words of the tree rows and the side panel cards (AgentManagerContent.Internal.h StateLabel)."""
STATE_WORD = {"Running": "running", "WaitingForInput": "waiting-for-you", "NeedsApproval": "needs-approval",
              "Error": "error", "Done": "done", "Idle": "idle"}
"""sessions.json state -> the word the page and the side panel show."""
COLUMN_OF = {"Running": "Running", "WaitingForInput": "Waiting-for-you", "NeedsApproval": "Needs-approval",
             "Error": "Error", "Done": "Idle / Done", "Idle": "Idle / Done"}
"""sessions.json state -> the board column a managed session's card sits in."""
LAUNCH_NAMES = ("Launch Claude", "Create & Launch Claude", "Resume session", "Launch Codex", "Create & Launch Codex")


@dataclass
class Card:
    """A Triage Board card.

    Attributes:
        el: The card's button (a snapshot).
        column: The column it sits in.
        texts: Its texts in order (title, folder, timing, model, ctx, ⚙ …).
    """

    el: uia.Element
    column: str
    texts: list = field(default_factory=list)

    @property
    def title(self) -> str:
        """The card's title (its first text).

        Returns:
            The title.
        """
        return self.texts[0] if self.texts else ""

    def click(self) -> None:
        """Select the card (a single click on its title band).

        Returns:
            None.
        """
        x, y, w, h = self.el.rect
        self.el.click(offset=(min(60, w // 3), 10))

    def double_click(self) -> None:
        """Activate: jump to the session's tab.

        Returns:
            None.
        """
        x, y, w, h = self.el.rect
        self.el.click(count=2, offset=(min(60, w // 3), 10))

    def right_click(self) -> None:
        """Open the card's session menu.

        Returns:
            None.
        """
        x, y, w, h = self.el.rect
        self.el.click("right", offset=(min(60, w // 3), 10))


@dataclass
class TreeRow:
    """An Explorer Tree row.

    Attributes:
        el: The row's button.
        kind: "folder" or "session".
        texts: Its texts.
    """

    el: uia.Element
    kind: str
    texts: list = field(default_factory=list)

    @property
    def label(self) -> str:
        """The folder path of a folder row, the title of a session row.

        Returns:
            The text.
        """
        if self.kind == "folder":
            return self.texts[1] if len(self.texts) > 1 else ""
        return self.texts[0] if self.texts else ""

    @property
    def state(self) -> str:
        """A session row's state word ("" for a folder row).

        Returns:
            The word.
        """
        if self.kind != "session":
            return ""
        return next((t for t in self.texts[1:] if t in STATE_WORDS), "")


class ManagerPage:
    """A window's Agent Manager page.

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

    # ---- showing ----------------------------------------------------------------------------------

    def showing(self) -> bool:
        """Whether the page shows.

        Returns:
            True on the page.
        """
        return self.window.page_showing()

    def home_name(self) -> str:
        """Home's face as its accessible name: "Agent Manager" or "Jump back to <title>".

        Returns:
            The name.
        """
        return self.window.home().name

    def click_home(self, count: int = 1) -> None:
        """Click Home with the real pointer.

        ⚠ Home ignores a click that comes within the system double-click time of a click that opened the page or
        jumped back (MGR-02 E1: a double-click must not open the page and leave it at once), so two separate
        Home clicks are spaced past that time here, the way two deliberate clicks of a user are.

        Args:
            count: 2 for a double-click (sent as one, inside the double-click time).
        Returns:
            None.
        """
        app = self.window.app
        spacing = win32.double_click_ms() / 1000.0 + 0.15
        wait = getattr(app, "_last_home_click", 0.0) + spacing - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        self.window.focus()
        self.window.home().click(count=count)
        app._last_home_click = time.monotonic()

    def open(self) -> "ManagerPage":
        """Show the page (Home), unless it shows already.

        Returns:
            self.
        Raises:
            WaitTimeout: When the page did not show.
        """
        if not self.showing():
            self.click_home()
            self.wait_showing()
        return self

    def wait_showing(self, timeout_s: float = 10.0) -> None:
        """Wait for the page to show.

        Args:
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When it did not.
        """
        wait_until(self.showing, timeout_s, what="the Agent Manager page",
                   describe=lambda: (self.window.title, [t.title for t in self.window.tabs.items() if t.selected]))

    def wait_hidden(self, timeout_s: float = 10.0) -> None:
        """Wait for the page to hide (a tab shows).

        Args:
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When it kept showing.
        """
        wait_until(lambda: not self.showing(), timeout_s, what="the Agent Manager page to hide")

    # ---- the page's controls -----------------------------------------------------------------------

    def controls(self) -> list[uia.Element]:
        """The page's own controls (the content's children outside the side panel's band).

        Returns:
            The elements, in tree order (empty while the page does not show).
        """
        kids = self.window.content_children()
        band = self.window.side_panel.band(kids)
        if band is None:
            return kids
        x0, x1 = band
        return [k for k in kids if not (x0 <= k.center[0] < x1)]

    def control(self, name: str, role: Optional[str] = None) -> uia.Element:
        """A page control by its name.

        Args:
            name: The exact name.
            role: Its role (None = any).
        Returns:
            The element.
        Raises:
            LookupError: When the page has no such control (the message lists the named ones).
        """
        for el in self.controls():
            if el.name == name and (role is None or el.role == role):
                return el
        raise LookupError(f"the page has no {role or 'control'} {name!r}; it has: "
                          f"{[el.name for el in self.controls() if el.name][:40]}")

    def has_control(self, name: str) -> bool:
        """Whether the page shows a control with this name.

        Args:
            name: The exact name.
        Returns:
            True when shown.
        """
        return any(el.name == name for el in self.controls())

    def activate_all_button(self) -> Optional[uia.Element]:
        """The toolbar's "Activate All Tabs (N)" button — shown only while THIS window holds dormant managed tabs
        (restored or parked ones whose claude has not started).

        Returns:
            The button, or None when the window has no dormant tab.
        """
        return next((el for el in self.controls()
                     if el.role == "button" and el.name.startswith("Activate All Tabs")), None)

    def dormant_count(self) -> int:
        """The N of "Activate All Tabs (N)": this window's dormant managed tabs, as the page counts them.

        Returns:
            N (0 when the button is not shown).
        """
        btn = self.activate_all_button()
        if btn is None:
            return 0
        digits = "".join(c for c in btn.name if c.isdigit())
        return int(digits) if digits else 0

    def activate_all(self, scope: str = "this") -> int:
        """Wake dormant managed tabs in place with the toolbar's "Activate All Tabs (N)" (the page starts them one
        by one, ~500 ms apart, at most 4 per 10 s; none of them is selected and the page stays).

        Measured (0.8.0): while OTHER windows hold dormant tabs too, the click first asks — a dialog "Activate
        dormant tabs" ("This window: N • All windows: M.") with the buttons "This window (N)", "All windows (M)",
        "Cancel"; with dormant tabs in this window only, it starts them at once (`[activate-all] window {…} drip-start
        …`).

        Args:
            scope: "this" (this window's tabs) or "all" (every window's) — the dialog's answer when it asks.
        Returns:
            The N the button showed before the click.
        Raises:
            LookupError: When the button is not shown (no dormant tab), or the dialog lacks the scope's button.
            ValueError: On another scope.
        """
        from .dialogs import Dialog
        if scope not in ("this", "all"):
            raise ValueError(f"scope must be 'this' or 'all', not {scope!r}")
        self.open()
        btn = self.activate_all_button()
        if btn is None:
            raise LookupError("the page shows no 'Activate All Tabs (N)' button (this window has no dormant tab)")
        count = self.dormant_count()
        mark = self.window.app.profile.hooks.mark()
        self.window.focus()
        btn.click()

        def started_or_asked():
            """One poll: the drip started, or the choice dialog asks.

            Returns:
                "started", the Dialog, or None.
            """
            if self.window.app.profile.hooks.find(r"\[activate-all\] window \{[^}]*\} drip-start", since=mark):
                return "started"
            return Dialog.find_in(self.window, title="Activate dormant tabs")

        outcome = wait_until(started_or_asked, 10.0, what="Activate All Tabs to start, or to ask which windows")
        if outcome != "started":
            prefix = "This window" if scope == "this" else "All windows"
            names = [n for n in outcome.buttons() if n.startswith(prefix)]
            if not names:
                raise LookupError(f"the dialog has no {prefix!r} button: {outcome.buttons()}")
            outcome.press(names[0])
        return count

    def _after(self, marker: str, role: str, controls: Optional[list] = None) -> Optional[uia.Element]:
        """The first control of a role after a marker text (the page's sections follow their headers).

        Args:
            marker: A header text ('TRIAGE BOARD', 'EXPLORER TREE', 'Prompt Queue').
            role: The role wanted ("pane" for a ScrollViewer).
            controls: The controls to search (default: read now).
        Returns:
            The element, or None.
        """
        items = controls if controls is not None else self.controls()
        seen = False
        for el in items:
            if el.name == marker:
                seen = True
                continue
            if seen and el.role == role and (role != "pane" or el.class_name == "ScrollViewer"):
                return el
        return None

    # ---- the launch row ---------------------------------------------------------------------------

    def launch_box(self) -> uia.Element:
        """The working-folder box of the launch row.

        Returns:
            The edit.
        Raises:
            LookupError: When the page does not show.
        """
        return self.control("working directory (the M axis)", role="edit")

    def launch_button(self) -> uia.Element:
        """The launch row's main button (its name follows the box: Launch / Create & Launch / Resume).

        Returns:
            The button.
        Raises:
            LookupError: When the page does not show.
        """
        for el in self.controls():
            if el.role == "button" and el.name in LAUNCH_NAMES:
                return el
        raise LookupError("no launch button on the page")

    def type_folder(self, folder: str) -> None:
        """Replace the launch box's text by typing (the folder list opens and filters as a user sees it).

        Args:
            folder: The path to type.
        Returns:
            None.
        """
        box = self.launch_box()
        box.click()
        inputs.keys("ctrl+a")
        inputs.type_text(str(folder), delay_s=0.002)
        wait_until(lambda: self.launch_box().value == str(folder), 5.0, what="the folder in the launch box",
                   describe=lambda: self.launch_box().value)

    def launch(self, folder: str) -> None:
        """Launch a Claude session in a folder through the launch row (the real path: type, click Launch).

        Args:
            folder: An existing folder.
        Returns:
            None (Session.launch waits for the session).
        Raises:
            AssertionError: When the button does not read "Launch Claude" for an existing folder.
        """
        self.open()
        self.type_folder(folder)
        button = wait_until(lambda: (lambda b: b if b.name == "Launch Claude" else None)(self.launch_button()),
                            5.0, what="'Launch Claude' for the typed folder",
                            describe=lambda: self.launch_button().name)
        button.click()

    # ---- the board --------------------------------------------------------------------------------

    def board_scope(self) -> str:
        """The board's LOCAL / GLOBAL toggle.

        Returns:
            Its text.
        """
        controls = self.controls()
        btn = self._after("TRIAGE BOARD", "button", controls)
        return btn.name if btn is not None else ""

    def toggle_board_scope(self) -> str:
        """Click the board's LOCAL / GLOBAL toggle.

        Returns:
            The new text.
        """
        before = self.board_scope()
        self._after("TRIAGE BOARD", "button").click()
        return wait_until(lambda: (lambda s: s if s and s != before else None)(self.board_scope()), 5.0,
                          what="the board scope to change")

    def board_view(self) -> Optional[uia.Element]:
        """The board's scroll viewer.

        Returns:
            The element, or None while the page does not show.
        """
        return self._after("TRIAGE BOARD", "pane")

    def columns(self) -> dict[str, tuple[int, list[Card]]]:
        """The board's columns with their counts and cards.

        Returns:
            {column name: (the header's count, the cards)}.
        """
        view = self.board_view()
        if view is None:
            return {}
        out: dict[str, tuple[int, list[Card]]] = {}
        name, count = "", 0
        for el in view.children():
            if el.role == "text" and el.name in COLUMNS:
                name, count = el.name, 0
            elif el.role == "text" and name and el.name.isdigit():
                count = int(el.name)
            elif el.role == "pane" and name:
                cards = [Card(b, name, b.texts(max_depth=3)) for b in el.children() if b.role == "button"]
                out[name] = (count, cards)
                name = ""
        return out

    def cards(self) -> list[Card]:
        """Every card on the board, column by column.

        Returns:
            The cards.
        """
        return [c for _, cards in self.columns().values() for c in cards]

    def card(self, title: str) -> Card:
        """A card by its title.

        Args:
            title: The session's title.
        Returns:
            The card.
        Raises:
            LookupError: When the board has no such card (the message lists the titles per column).
        """
        for c in self.cards():
            if c.title == title:
                return c
        raise LookupError(f"no board card {title!r}; the board: "
                          f"{ {k: [c.title for c in v[1]] for k, v in self.columns().items()} }")

    def wait_card(self, title: str, column: Optional[str] = None, timeout_s: float = 15.0) -> Card:
        """Wait for a card (optionally in a column).

        Args:
            title: The session's title.
            column: The column it must sit in (None = any).
            timeout_s: How long.
        Returns:
            The card.
        Raises:
            WaitTimeout: When it never got there.
        """
        def found():
            """One look at the board.

            Returns:
                The card, or None.
            """
            for c in self.cards():
                if c.title == title and (column is None or c.column == column):
                    return c
            return None

        return wait_until(found, timeout_s, what=f"card {title!r} in {column or 'any column'}",
                          describe=lambda: {k: [c.title for c in v[1]] for k, v in self.columns().items()})

    def clear(self) -> None:
        """Click Clear (deselect).

        Returns:
            None.
        Raises:
            LookupError: When Clear is not shown (nothing is selected).
        """
        self.control("Clear", role="button").click()

    def show_all(self) -> None:
        """Click Show all (drop the folder scope).

        Returns:
            None.
        Raises:
            LookupError: When it is not shown (no folder is scoped).
        """
        self.control("Show all", role="button").click()

    # ---- the tree ---------------------------------------------------------------------------------

    def tree_scope(self) -> str:
        """The tree's LOCAL / GLOBAL / EXTERNAL toggle.

        Returns:
            Its text.
        """
        btn = self._after("EXPLORER TREE", "button")
        return btn.name if btn is not None else ""

    def tree_rows(self) -> list[TreeRow]:
        """The Explorer Tree's rows, top to bottom.

        Returns:
            The rows.
        """
        view = self._after("EXPLORER TREE", "pane")
        if view is None:
            return []
        rows = []
        for el in view.children():
            if el.role != "button":
                continue
            texts = el.texts(max_depth=3)
            kind = "folder" if texts and texts[0] in ("▾", "▸") else "session"
            rows.append(TreeRow(el, kind, texts))
        return rows

    def tree_row(self, label: str) -> TreeRow:
        """A tree row by its label (a folder path or a session title).

        Args:
            label: The label.
        Returns:
            The row.
        Raises:
            LookupError: When there is no such row.
        """
        for row in self.tree_rows():
            if row.label == label:
                return row
        raise LookupError(f"no tree row {label!r}; rows: {[(r.kind, r.label) for r in self.tree_rows()]}")

    # ---- the right pane ---------------------------------------------------------------------------

    def show_tab(self, name: str) -> None:
        """Pick the right pane's Summary or Prompt Queue tab.

        Args:
            name: "Summary" or "Prompt Queue".
        Returns:
            None.
        """
        self.control(name, role="button").click()
        time.sleep(0.3)

    def right_pane_texts(self) -> list[str]:
        """The right pane's texts (the selected session's summary or queue).

        Returns:
            The texts ([] when nothing is selected or the page does not show).
        """
        view = self._after("Prompt Queue", "pane")
        return view.texts(max_depth=8) if view is not None else []

    # ---- the Prompt Queue tab's compose row -------------------------------------------------------

    COMPOSE_NAMES = ("queue a prompt for the selected session…", "click to pull in this session's unsent prompt…")
    """The compose box's accessible name is its placeholder: the first while the selected session has no unsent
    draft, the second while it has one (a pointer or keyboard focus on the EMPTY box then pulls the draft in)."""

    def compose_box(self) -> uia.Element:
        """The Prompt Queue tab's compose box (measured 0.8.0: an edit of class TextBox below the queue's list, with
        three nameless icon buttons on its left — Jump to tab, Send now, Add to queue — and the pull-draft and
        templates buttons on its right).

        Returns:
            The edit.
        Raises:
            LookupError: When the Prompt Queue tab is not showing (show_tab("Prompt Queue") first).
        """
        for el in self.controls():
            if el.role == "edit" and el.class_name == "TextBox" and el.name in self.COMPOSE_NAMES:
                return el
        raise LookupError("no compose box: is the Prompt Queue tab showing with a session selected?")

    def compose_buttons(self) -> list[uia.Element]:
        """The compose row's icon buttons LEFT of the box, left to right: [Jump to tab, Send now, Add to queue].

        Returns:
            The buttons (they have no accessible names: their position is their identity).
        Raises:
            LookupError: When the compose box is not showing.
        """
        bx, by, _bw, _bh = self.compose_box().rect
        found = [el for el in self.controls()
                 if el.role == "button" and el.visible and abs(el.rect[1] - by) <= 14 and el.rect[0] < bx
                 and bx - el.rect[0] < 260]
        return sorted(found, key=lambda el: el.rect[0])

    def queue_prompt(self, text: str) -> None:
        """Append a prompt to the selected session's queue: the compose box's text set through UI Automation (no
        focus, so no unsent draft is pulled into it), then the envelope (Add to queue).

        Args:
            text: The prompt (a mock script inside `[[mock …]]` is sent to the model as it is).
        Returns:
            None.
        Raises:
            LookupError: When the Prompt Queue tab or its buttons are not showing.
        """
        box = self.compose_box()
        box.set_value(text)
        buttons = self.compose_buttons()
        if len(buttons) != 3:
            raise LookupError(f"expected the 3 compose icons left of the box, found {[b.line() for b in buttons]}")
        buttons[2].click()
