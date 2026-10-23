"""The side panel: its header, the session cards, the summary below them, and the + launcher card.

Measured (0.8.0, docked left, 422 px wide in a 1920 px window): the panel's controls are direct children of the
window's content element (after the page's, when the page shows):
    header      button 'Launch a session' (the +) · ToggleButton 'Sum' · 'LOCAL'/'GLOBAL' · the sort ('MOST
                ACTIVE' …) · ToggleButton '⌖' · '⇄' (dock side) · '«' (collapse)
    filter      edit 'filter sessions'
    count       text 'N session(s) in this window' / '… across all windows' (and 'N of M …' when filtered)
    cards       a ScrollViewer whose texts are, per card: title · state word · timing · '<leaf> / <branch>'
                (a card is a Border, not a control: it is clicked through its title text)
    summary     (Sum on) the followed / peeked session's title, its times line, three buttons, a ScrollViewer of
                its numbered prompts
The + card is an in-window Popup holding '●' 'Claude', the folder box and 'Launch Claude'; its folder list is a
second Popup whose rows include 'Browse…' and section headers ('RECENT', 'SUBFOLDERS OF …', 'MATCHES …').
"""
from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Optional

from .. import inputs, uia
from ..waits import wait_until
from .manager import STATE_WORDS

PLUS_NAME = "Launch a session"
COUNT_RE = re.compile(r"^(\d+)(?: of (\d+))? sessions? (in this window|across all windows)")


@dataclass
class PanelCard:
    """One session card of the side panel.

    Attributes:
        title_el: The title's text element (the card is clicked through it).
        title: The session's title.
        state: The state word.
        texts: Every text of the card, in order (title, state, timing, subline ...).
    """

    title_el: uia.Element
    title: str
    state: str
    texts: list = field(default_factory=list)

    def click(self) -> None:
        """Click the card (peek: the summary shows this session; the tab stays).

        Returns:
            None.
        """
        self.title_el.click(offset=(8, self.title_el.rect[3] // 2))

    def double_click(self) -> None:
        """Double-click the card (jump to its tab).

        Returns:
            None.
        """
        self.title_el.click(count=2, offset=(8, self.title_el.rect[3] // 2))

    def right_click(self) -> None:
        """Right-click the card (its session menu).

        Returns:
            None.
        """
        self.title_el.click("right", offset=(8, self.title_el.rect[3] // 2))

    def hover(self) -> None:
        """Rest the pointer on the card.

        Returns:
            None.
        """
        x, y, w, h = self.title_el.rect
        inputs.move(x + 8, y + h // 2)


class SidePanel:
    """A window's side panel.

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

    # ---- locating ---------------------------------------------------------------------------------

    def _kids(self, kids: Optional[list] = None) -> list[uia.Element]:
        """The content's children (read now unless given).

        Args:
            kids: A list read already.
        Returns:
            The elements.
        """
        return kids if kids is not None else self.window.content_children()

    def plus(self, kids: Optional[list] = None) -> Optional[uia.Element]:
        """The + button (the header's anchor).

        Args:
            kids: The content's children, when read already.
        Returns:
            The button, or None while the panel is collapsed or absent.
        """
        return next((k for k in self._kids(kids) if k.role == "button" and k.name == PLUS_NAME), None)

    def cards_view(self, kids: Optional[list] = None) -> Optional[uia.Element]:
        """The cards' ScrollViewer: the first one below the count line, in the header's band.

        Args:
            kids: The content's children, when read already.
        Returns:
            The element, or None while collapsed.
        """
        kids = self._kids(kids)
        plus = self.plus(kids)
        if plus is None:
            return None
        after = False
        for k in kids:
            if k is plus or (k.role == "button" and k.name == PLUS_NAME):
                after = True
                continue
            if after and k.role == "pane" and k.class_name == "ScrollViewer" and k.rect[1] > plus.rect[1] \
                    and k.rect[0] <= plus.center[0] < k.rect[0] + k.rect[2]:
                return k
        return None

    def band(self, kids: Optional[list] = None) -> Optional[tuple[int, int]]:
        """The panel's horizontal band on screen.

        Args:
            kids: The content's children, when read already.
        Returns:
            (left x, right x), or None when the panel is collapsed or absent.
        """
        view = self.cards_view(kids)
        if view is None:
            return None
        return view.rect[0], view.rect[0] + view.rect[2]

    def controls(self, kids: Optional[list] = None) -> list[uia.Element]:
        """The panel's controls (the content's children inside its band).

        Args:
            kids: The content's children, when read already.
        Returns:
            The elements, in tree order.
        """
        kids = self._kids(kids)
        band = self.band(kids)
        if band is None:
            return []
        return [k for k in kids if band[0] <= k.center[0] < band[1]]

    def button(self, name: str) -> uia.Element:
        """A header button by its name ('Sum', 'LOCAL', '⌖', '⇄', '«' ...).

        Args:
            name: The exact name.
        Returns:
            The element.
        Raises:
            LookupError: When there is none.
        """
        for k in self.controls():
            if k.role == "button" and k.name == name:
                return k
        raise LookupError(f"the side panel has no button {name!r}; it has "
                          f"{[k.name for k in self.controls() if k.role == 'button' and k.name]}")

    # ---- the header -------------------------------------------------------------------------------

    def scope(self) -> str:
        """LOCAL or GLOBAL.

        Returns:
            The toggle's text.
        """
        return next((k.name for k in self.controls() if k.role == "button" and k.name in ("LOCAL", "GLOBAL")), "")

    def toggle_scope(self) -> str:
        """Click LOCAL / GLOBAL.

        Returns:
            The new scope.
        """
        before = self.scope()
        self.button(before).click()
        return wait_until(lambda: (lambda s: s if s and s != before else None)(self.scope()), 5.0,
                          what="the panel's scope to change")

    def sum_on(self) -> bool:
        """Whether Sum is on (the summary shows under the cards).

        Returns:
            Its toggle state.
        """
        return self.button("Sum").toggle_state == 1

    def set_sum(self, on: bool) -> None:
        """Turn Sum on or off.

        Args:
            on: The wanted state.
        Returns:
            None.
        """
        if self.sum_on() != on:
            self.button("Sum").click()
            wait_until(lambda: self.sum_on() == on, 5.0, what=f"Sum {'on' if on else 'off'}")

    def count_line(self) -> str:
        """The count line ('2 sessions in this window', '1 of 3 sessions …').

        Returns:
            The text ("" when absent).
        """
        return next((k.name for k in self.controls() if k.role == "text" and COUNT_RE.match(k.name)), "")

    def filter_box(self) -> uia.Element:
        """The filter box.

        Returns:
            The edit.
        Raises:
            LookupError: When absent.
        """
        for k in self.controls():
            if k.role == "edit" and k.name == "filter sessions":
                return k
        raise LookupError("no filter box")

    def dock_side(self) -> str:
        """Which side of the window the panel sits on.

        Returns:
            "left" or "right" ("" when collapsed).
        """
        band = self.band()
        if band is None:
            return ""
        wx, _, ww, _ = self.window.rect()
        return "left" if (band[0] + band[1]) / 2 < wx + ww / 2 else "right"

    def collapsed(self) -> bool:
        """Whether the panel is collapsed to its strip.

        Returns:
            True when collapsed (no + button shows).
        """
        return self.plus() is None

    # ---- the cards --------------------------------------------------------------------------------

    def cards(self) -> list[PanelCard]:
        """The visible cards, top to bottom.

        Returns:
            The cards (realized ones only: the list is virtualized).
        """
        view = self.cards_view()
        if view is None:
            return []
        texts = [el for _, el in view.walk(max_depth=8) if el.role == "text" and el.name]
        cards: list[PanelCard] = []
        for i, el in enumerate(texts):
            if el.name in STATE_WORDS and i > 0:
                title_el = texts[i - 1]
                if cards and cards[-1].texts and cards[-1].texts[-1] == title_el.name:
                    cards[-1].texts.pop()
                cards.append(PanelCard(title_el, title_el.name, el.name, [title_el.name, el.name]))
            elif cards:
                cards[-1].texts.append(el.name)
        return cards

    def titles(self) -> list[str]:
        """The visible cards' titles, top to bottom.

        Returns:
            The titles.
        """
        return [c.title for c in self.cards()]

    def card(self, title: str) -> PanelCard:
        """A card by its title.

        Args:
            title: The session's title.
        Returns:
            The card.
        Raises:
            LookupError: When it is not shown.
        """
        for c in self.cards():
            if c.title == title:
                return c
        raise LookupError(f"no side-panel card {title!r}; cards: {self.titles()}")

    def wait_card(self, title: str, state: Optional[str] = None, timeout_s: float = 15.0) -> PanelCard:
        """Wait for a card (optionally showing a state word).

        Args:
            title: The session's title.
            state: The state word it must show (None = any).
            timeout_s: How long.
        Returns:
            The card.
        Raises:
            WaitTimeout: When it never showed so.
        """
        def found():
            """One look at the cards.

            Returns:
                The card or None.
            """
            return next((c for c in self.cards() if c.title == title and (state is None or c.state == state)), None)

        return wait_until(found, timeout_s, what=f"side-panel card {title!r} {state or ''}".strip(),
                          describe=lambda: [(c.title, c.state) for c in self.cards()])

    # ---- the summary ------------------------------------------------------------------------------

    def summary_texts(self) -> list[str]:
        """The texts of the summary under the cards (Sum on): the session's title first.

        Returns:
            The texts ([] with Sum off or nothing to show).
        """
        kids = self.window.content_children()
        view = self.cards_view(kids)
        if view is None:
            return []
        band = view.rect[0], view.rect[0] + view.rect[2]
        bottom = view.rect[1] + view.rect[3]
        out = []
        for k in kids:
            if not (band[0] <= k.center[0] < band[1]) or k.rect[1] < bottom:
                continue
            if k.role == "text" and k.name:
                out.append(k.name)
            elif k.role == "pane":
                out.extend(k.texts(max_depth=6))
        return out

    def summary_title(self) -> str:
        """The title of the session the summary shows.

        Returns:
            The title ("" when none).
        """
        texts = self.summary_texts()
        return texts[0] if texts else ""

    # ---- the + card -------------------------------------------------------------------------------

    def open_launcher(self) -> "LauncherCard":
        """Click + and return the launcher card.

        Returns:
            The LauncherCard.
        Raises:
            WaitTimeout: When it did not open.
        """
        self.window.focus()
        plus = self.plus()
        if plus is None:
            raise LookupError("the panel is collapsed: no + button")
        plus.click()
        card = LauncherCard(self.window)
        wait_until(card.is_open, 5.0, what="the + launcher card")
        return card


class LauncherCard:
    """The + launcher card (an in-window Popup with the Agent Manager's launch row).

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

    def popups(self) -> list[uia.Element]:
        """The content's in-window popups.

        Returns:
            The `window 'Popup'` elements.
        """
        return [k for k in self.window.content_children() if k.role == "window" and k.class_name == "Popup"]

    def element(self) -> Optional[uia.Element]:
        """The card (the popup holding the folder box and the Launch button).

        Returns:
            The element, or None when closed.
        """
        for p in self.popups():
            if p.find(role="edit", name="working directory (the M axis)", max_depth=3) is not None:
                return p
        return None

    def is_open(self) -> bool:
        """Whether the card shows.

        Returns:
            True while open.
        """
        return self.element() is not None

    def box(self) -> uia.Element:
        """The card's folder box.

        Returns:
            The edit.
        Raises:
            LookupError: When the card is closed.
        """
        card = self.element()
        if card is None:
            raise LookupError("the + card is closed")
        return card.find(role="edit", name="working directory (the M axis)", max_depth=3)

    def launch_button(self) -> uia.Element:
        """The card's Launch button.

        Returns:
            The button.
        Raises:
            LookupError: When the card is closed.
        """
        from .manager import LAUNCH_NAMES
        card = self.element()
        if card is None:
            raise LookupError("the + card is closed")
        for el in card.find_all(role="button", max_depth=3):
            if el.name in LAUNCH_NAMES:
                return el
        raise LookupError("the + card has no Launch button")

    def folder_list(self) -> Optional[uia.Element]:
        """The folder list under the box.

        Returns:
            Its popup, or None when closed.
        """
        for p in self.popups():
            if any(t == "Browse…" for t in p.texts(max_depth=4)):
                return p
        return None

    def folder_list_texts(self) -> list[str]:
        """The folder list's texts (sections and rows).

        Returns:
            The texts ([] when closed).
        """
        lst = self.folder_list()
        return lst.texts(max_depth=6) if lst is not None else []

    def type_folder(self, folder: str) -> None:
        """Replace the box's text by typing.

        Args:
            folder: The path.
        Returns:
            None.
        """
        inputs.keys("ctrl+a")
        inputs.type_text(str(folder), delay_s=0.002)
        wait_until(lambda: self.box().value == str(folder), 5.0, what="the folder in the + card's box")

    def launch(self) -> None:
        """Click Launch.

        Returns:
            None.
        """
        self.launch_button().click()
        time.sleep(0.2)
