"""In-window panels the session menus open: the Tags panel and the "Specify a model" card.

Measured (0.8.0): both are `window 'Popup'` elements among the window's content children (not separate popup
windows):
- the Tags panel: an edit named 'tag name' (Enter adds the typed tag), nameless buttons (+, the color swatches,
  each tag row's toggle and ✕), and the texts 'No tags yet — …' or the tag names, and 'N of M tags';
- the Specify card: a full-window Popup (the dimmed backdrop) holding the title 'Specify a model', an editable
  combo box whose edit is named by its placeholder ('e.g. opus, claude-opus-4-8, gpt-5.6-sol'), two links, and the
  buttons 'Fetch models', 'Cancel', 'Use this model'.
"""
from __future__ import annotations

import re
from typing import Optional

from .. import inputs, uia
from ..waits import wait_until


def _content_popups(window) -> list[uia.Element]:
    """The window's in-window popups.

    Args:
        window: The MainWindow.
    Returns:
        The `window 'Popup'` elements among the content's children.
    """
    return [k for k in window.content_children() if k.role == "window" and k.class_name == "Popup"]


class TagsPanel:
    """The Tags panel of one session.

    Attributes:
        window: The MainWindow that shows it.
    """

    def __init__(self, window) -> None:
        """Bind to a window.

        Args:
            window: The MainWindow.
        Returns:
            None.
        """
        self.window = window

    def element(self) -> Optional[uia.Element]:
        """The panel.

        Returns:
            Its popup element, or None when closed.
        """
        for p in _content_popups(self.window):
            if p.find(role="edit", name="tag name", max_depth=3) is not None:
                return p
        return None

    def is_open(self) -> bool:
        """Whether the panel shows.

        Returns:
            True while open.
        """
        return self.element() is not None

    def wait_open(self, timeout_s: float = 5.0) -> "TagsPanel":
        """Wait for the panel.

        Args:
            timeout_s: How long.
        Returns:
            self.
        Raises:
            WaitTimeout: When it never showed.
        """
        wait_until(self.is_open, timeout_s, what="the Tags panel")
        return self

    def box(self) -> uia.Element:
        """The tag-name box.

        Returns:
            The edit.
        Raises:
            LookupError: When the panel is closed.
        """
        panel = self.element()
        if panel is None:
            raise LookupError("the Tags panel is closed")
        return panel.find(role="edit", name="tag name", max_depth=3)

    def texts(self) -> list[str]:
        """The panel's texts (the tag rows, the hint, the 'N of M tags' line).

        Returns:
            The texts ([] when closed).
        """
        panel = self.element()
        return panel.texts(max_depth=8) if panel is not None else []

    def count(self) -> tuple[int, int]:
        """The 'N of M tags' line.

        Returns:
            (N, M).
        Raises:
            LookupError: When the line is not shown.
        """
        for t in self.texts():
            m = re.match(r"^(\d+) of (\d+) tags", t)
            if m:
                return int(m.group(1)), int(m.group(2))
        raise LookupError(f"no 'N of M tags' line; the panel: {self.texts()}")

    def add(self, name: str) -> None:
        """Click into the box, type a tag name, press Enter (the panel's add).

        Args:
            name: The tag.
        Returns:
            None.
        Raises:
            WaitTimeout: When the tag never appeared in the panel.
        """
        self.box().click()
        inputs.keys("ctrl+a")
        inputs.type_text(name)
        inputs.keys("enter")
        wait_until(lambda: name in self.texts(), 5.0, what=f"tag {name!r} in the panel", describe=self.texts)


class SpecifyModelCard:
    """The "Specify a model" card (Specify… in any launch / fork model menu).

    Attributes:
        window: The MainWindow that shows it.
    """

    TITLE = "Specify a model"

    def __init__(self, window) -> None:
        """Bind to a window.

        Args:
            window: The MainWindow.
        Returns:
            None.
        """
        self.window = window

    def element(self) -> Optional[uia.Element]:
        """The card's popup.

        Returns:
            Its element, or None when closed.
        """
        for p in _content_popups(self.window):
            if p.find(role="text", name=self.TITLE, max_depth=3) is not None:
                return p
        return None

    def is_open(self) -> bool:
        """Whether the card shows.

        Returns:
            True while open.
        """
        return self.element() is not None

    def wait_open(self, timeout_s: float = 5.0) -> "SpecifyModelCard":
        """Wait for the card.

        Args:
            timeout_s: How long.
        Returns:
            self.
        Raises:
            WaitTimeout: When it never showed.
        """
        wait_until(self.is_open, timeout_s, what="the Specify a model card")
        return self

    def box(self) -> uia.Element:
        """The model id box (the editable combo box's edit).

        Returns:
            The edit.
        Raises:
            LookupError: When the card is closed.
        """
        card = self.element()
        if card is None:
            raise LookupError("the Specify card is closed")
        combo = card.find(role="combo box", max_depth=3)
        return (combo or card).find(role="edit", max_depth=3)

    def value(self) -> str:
        """The id in the box (the card opens pre-filled with the last one committed).

        Returns:
            The text.
        """
        return self.box().value

    def use(self, model: str) -> None:
        """Type a model id and press Use this model (the session launches with --model <id>).

        Args:
            model: The id.
        Returns:
            None.
        Raises:
            WaitTimeout: When the card did not close.
        """
        self.box().click()
        inputs.keys("ctrl+a")
        inputs.type_text(model)
        wait_until(lambda: self.value() == model, 5.0, what="the typed model id", describe=self.value)
        self.press("Use this model")

    def press(self, name: str) -> None:
        """Click one of the card's buttons ('Use this model', 'Cancel', 'Fetch models').

        Args:
            name: The button's text.
        Returns:
            None.
        Raises:
            LookupError: When there is no such button.
            WaitTimeout: When Use / Cancel did not close the card.
        """
        card = self.element()
        btn = card.find(role="button", name=name, max_depth=3) if card is not None else None
        if btn is None:
            raise LookupError(f"the Specify card has no button {name!r}")
        btn.click()
        if name != "Fetch models":
            wait_until(lambda: not self.is_open(), 5.0, what="the Specify card to close")
