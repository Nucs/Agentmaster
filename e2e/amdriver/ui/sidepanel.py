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

Accessibility metadata (E2E_TESTING.md §6.3; a build of 2026-10-23 or later — NOT in 0.8.0): the header's controls
have AutomationIds (am.side.launch / filter / sum / scope / sort / scrollToTab / dock / collapse, the strip's
am.side.expand), and the glyph-only ones NAMES instead of their glyphs (⌖ "Scroll to the selected tab", ⇄ "Dock side",
the chevrons "Collapse the side panel" / "Expand the side panel"); `button` and `strip_button` find them by id first and
by their 0.8.0 glyphs second. A card's TITLE text carries am.side.card.<key> and the card's ItemStatus (the state word,
then "codex", "unread", "unsent draft", "cache warm", "tags: …", "selected" as they hold).

The ⋯ menu (SIDE_PANEL.md §2i; a build of 2026-11-03 or later — NOT in 0.8.x): the header line is only '+' · 'Sum' ·
'⋯' (button "More options", am.side.more). The ⋯ opens a MenuFlyout whose items keep the ids of the buttons they
replaced: 'Scroll to the selected tab' (am.side.scrollToTab, a ToggleMenuFlyoutItem: its toggle state is the setting) ·
'Card scope: LOCAL' ▸ (am.side.scope; entries am.side.scope.local / .global) · 'Card order: MOST ACTIVE' ▸ (am.side.sort;
entries am.side.sort.<active | recent | newest | oldest | alpha>, the settings.json tokens) · a separator (not reported) ·
'Dock on the right' / 'Dock on the left' (am.side.dock) · 'Collapse the side panel' (am.side.collapse). The panel's own
methods (scope, toggle_scope, set_scope, flip_dock, collapse) work on both header shapes: through the ⋯ menu when the
build has it, through the old buttons otherwise.

The line MIRRORS with the dock (the user's layout of 2026-11-04, a build of that day or later): ⋯ sits at the window's
edge, so docked right the line reads '+' · 'Sum' · '⋯' (right-aligned) and docked left '⋯' · 'Sum' · '+' (left-aligned),
and the menu hangs from ⋯'s outer edge (its left edge docked left, its right edge docked right). The dock moves grid
columns only, never the header's children, so read the line's order from the buttons' screen positions (header_buttons),
not from the tree; cards_view's "the ScrollViewer after +" walk holds in either dock, since the three buttons precede
the cards whatever their order. The 2026-11-03 build kept '+ Sum ⋯' right-aligned in both docks and listed the dock
item first, above Scroll to the selected tab.
"""
from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Optional

from .. import inputs, uia
from ..waits import wait_until
from .manager import STATE_WORDS
from .menus import Menu, MenuItem

PLUS_NAME = "Launch a session"
COUNT_RE = re.compile(r"^(\d+)(?: of (\d+))? sessions? (in this window|across all windows)")
GLYPH_IDS = {"⌖": "am.side.scrollToTab", "⇄": "am.side.dock", "«": "am.side.collapse",
             "»": "am.side.collapse"}
"""The header's glyph buttons as 0.8.0 names them (⌖ ⇄ « ») -> their AutomationIds on a build with the metadata, where
they are named by what they do instead. The header's collapse chevron points toward the docked edge (« docked left)."""
CARD_ID_PREFIX = "am.side.card."
"""The AutomationId prefix of a card's title text (the session id's first 8 characters follow)."""
STRIP_ID = "am.side.expand"
"""The collapsed strip's chevron (» docked left, « docked right) by AutomationId."""
MORE_ID = "am.side.more"
"""The header's ⋯ button (a build with the ⋯ menu, SIDE_PANEL.md §2i) by AutomationId."""
MORE_ITEM_IDS = {"dock": "am.side.dock", "scroll": "am.side.scrollToTab", "scope": "am.side.scope",
                 "order": "am.side.sort", "collapse": "am.side.collapse"}
"""The ⋯ menu's items by what they do -> their AutomationIds (the ids of the header buttons they replaced)."""
MORE_ITEM_ORDER = ["scroll", "scope", "order", "dock", "collapse"]
"""The ⋯ menu's items top to bottom (the user's order of 2026-11-04: the dock moved below the separator, before the
collapse; the separator after the card order is not reported). The 2026-11-03 build listed the dock first."""
HEADER_ORDER = {"right": ["am.side.launch", "am.side.sum", MORE_ID],
                "left": [MORE_ID, "am.side.sum", "am.side.launch"]}
"""The header line's buttons LEFT TO RIGHT on screen, per dock side -> their AutomationIds: the line mirrors with the dock,
⋯ always at the window's edge (the user's layout of 2026-11-04; header_buttons reads it)."""
ORDER_TOKENS = {"MOST ACTIVE": "active", "MOST RECENT": "recent", "NEWEST": "newest", "OLDEST": "oldest", "A–Z": "alpha"}
"""The Card order submenu's words, top to bottom -> the settings.json `boardSort` token (its entry is am.side.sort.<token>)."""


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

    @property
    def status(self) -> str:
        """The card's ItemStatus, read off its title text ("" on 0.8.0).

        Returns:
            The status.
        """
        return self.title_el.item_status

    @property
    def selected(self) -> Optional[bool]:
        """Whether this is the panel's selected (followed or peeked) card.

        Returns:
            True / False — or None when the build exposes no status (0.8.0: read the summary's title instead).
        """
        if not self.status:
            return None
        return "selected" in self.status.split("; ")

    @property
    def session_key(self) -> str:
        """The session's key (its id's first 8 characters), read off the title's AutomationId ("" on 0.8.0).

        Returns:
            The key.
        """
        aid = self.title_el.automation_id
        return aid[len(CARD_ID_PREFIX):] if aid.startswith(CARD_ID_PREFIX) else ""

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

        A glyph name (GLYPH_IDS) is looked up by its AutomationId first: a build with the accessibility metadata names
        those buttons by what they do ("Dock side"), and only 0.8.0 still names them by their glyphs. On a build with
        the ⋯ menu only '+', 'Sum' and '⋯' are buttons: reach the others through the methods below (scope, set_scope,
        flip_dock, collapse) or open_more_menu, which work on both header shapes.

        Args:
            name: The exact name (a glyph as 0.8.0 names it, or the button's word).
        Returns:
            The element.
        Raises:
            LookupError: When there is none.
        """
        controls = self.controls()
        aid = GLYPH_IDS.get(name)
        if aid is not None:
            for k in controls:
                if k.role == "button" and k.automation_id == aid:
                    return k
        for k in controls:
            if k.role == "button" and k.name == name:
                return k
        raise LookupError(f"the side panel has no button {name!r}; it has "
                          f"{[k.name for k in self.controls() if k.role == 'button' and k.name]}")

    # ---- the header -------------------------------------------------------------------------------

    def more_button(self, kids: Optional[list] = None) -> Optional[uia.Element]:
        """The header's ⋯ button.

        Args:
            kids: The content's children, when read already.
        Returns:
            The button, or None on a build whose header still shows every control as a button (0.8.x), and while the
            panel is collapsed.
        """
        return next((k for k in self.controls(kids) if k.role == "button" and k.automation_id == MORE_ID), None)

    def header_buttons(self) -> list[uia.Element]:
        """The header line's buttons, LEFT TO RIGHT on screen: every button of the panel above the filter box (the AI
        toggle sits inside the box, so it is not one of them). Ordered by position, not by tree order: the line mirrors
        with the dock by moving grid columns, and the tree keeps the build's order (HEADER_ORDER has both shapes).

        Returns:
            The buttons ([] while the panel is collapsed or absent).
        Raises:
            LookupError: When the panel shows but has no filter box.
        """
        controls = self.controls()
        if not controls:
            return []
        top = self.filter_box().rect[1]
        return sorted((k for k in controls if k.role == "button" and k.rect[1] < top), key=lambda k: k.rect[0])

    def open_more_menu(self) -> Menu:
        """Click ⋯ and wait for its menu (read once it stopped moving: the menu slides into place).

        Returns:
            The open menu.
        Raises:
            LookupError: When this build has no ⋯ button, or the panel is collapsed.
            WaitTimeout: When no menu opened.
        """
        btn = self.more_button()
        if btn is None:
            raise LookupError("the side panel has no ⋯ button (a build before the ⋯ menu shows each control as a "
                              "button, and a collapsed panel shows none)")
        before = Menu.open_hwnds(self.window.app)
        btn.click()
        menu = Menu.wait_new(self.window.app, before)
        menu.stable_items()
        return menu

    @staticmethod
    def more_item(menu: Menu, aid: str) -> MenuItem:
        """An item of the ⋯ menu (or one of its submenus) by its AutomationId — its TEXT changes with the state ("Dock on
        the right", "Card scope: LOCAL"), its id does not.

        Args:
            menu: The open menu level.
            aid: The AutomationId (MORE_ITEM_IDS, am.side.scope.<local|global>, am.side.sort.<token>).
        Returns:
            The item, read once the menu stopped moving.
        Raises:
            LookupError: When no item of that level has the id (the message lists the level's ids).
        """
        items = menu.stable_items()
        for it in items:
            if it.el.automation_id == aid:
                return it
        raise LookupError(f"no menu item {aid!r}; the menu: {[(i.name, i.el.automation_id) for i in items]}")

    def choose_more(self, *aids: str) -> None:
        """Walk the ⋯ menu by AutomationIds and choose the last: choose_more("am.side.scope", "am.side.scope.global").

        Every menu is closed (Esc) when a step fails, so the next step of a test starts from a closed menu.

        Args:
            *aids: The submenus' ids, then the command's.
        Returns:
            None (the menus closed; the command runs one dispatcher tick later — wait for its effect).
        Raises:
            LookupError / AssertionError / WaitTimeout: As open_more_menu, more_item, Menu.submenu and Menu.choose.
        """
        menu = self.open_more_menu()
        try:
            for aid in aids[:-1]:
                menu = menu.submenu(self.more_item(menu, aid).name)
            menu.choose(self.more_item(menu, aids[-1]).name)
        except Exception:
            Menu.dismiss_all(self.window.app)
            raise

    def scope(self) -> str:
        """LOCAL or GLOBAL.

        Read from the LOCAL / GLOBAL button on a build that has one, else from the count line, which names the scope
        ("… in this window" / "… across all windows") and needs no menu to be opened.

        Returns:
            "LOCAL", "GLOBAL", or "" when neither tells (a collapsed panel, the AI view's count line).
        """
        controls = self.controls()
        word = next((k.name for k in controls if k.role == "button" and k.name in ("LOCAL", "GLOBAL")), "")
        if word:
            return word
        for k in controls:
            m = COUNT_RE.match(k.name) if k.role == "text" else None
            if m:
                return "LOCAL" if m.group(3) == "in this window" else "GLOBAL"
        return ""

    def set_scope(self, scope: str) -> None:
        """Put the card list on a scope: ⋯ ▸ Card scope ▸ LOCAL / GLOBAL, or a click on the old toggle when it differs.

        Args:
            scope: "LOCAL" or "GLOBAL".
        Returns:
            None, once scope() reads it.
        Raises:
            ValueError: For another word.
            WaitTimeout: When the scope did not change.
        """
        if scope not in ("LOCAL", "GLOBAL"):
            raise ValueError(f"no such scope: {scope!r}")
        if self.more_button() is not None:
            self.choose_more(MORE_ITEM_IDS["scope"], f"am.side.scope.{scope.lower()}")
        elif self.scope() != scope:
            self.button(self.scope()).click()
        wait_until(lambda: self.scope() == scope, 5.0, what=f"the panel's scope {scope}", describe=self.count_line)

    def toggle_scope(self) -> str:
        """Switch LOCAL <-> GLOBAL (the ⋯ menu's Card scope, or the old toggle).

        Returns:
            The new scope.
        Raises:
            LookupError: When the scope cannot be read.
            WaitTimeout: When it did not change.
        """
        before = self.scope()
        if not before:
            raise LookupError("the panel's scope cannot be read (collapsed, or the AI view's count line)")
        after = "GLOBAL" if before == "LOCAL" else "LOCAL"
        self.set_scope(after)
        return after

    def flip_dock(self) -> str:
        """Dock the panel on the other side: ⋯ ▸ Dock on the right / left, or the old ⇄ button.

        Returns:
            The new side ("left" / "right").
        Raises:
            WaitTimeout: When the panel did not move.
        """
        before = self.dock_side()
        want = "right" if before == "left" else "left"
        if self.more_button() is not None:
            self.choose_more(MORE_ITEM_IDS["dock"])
        else:
            self.button("⇄").click()
        return wait_until(lambda: (lambda s: s if s == want else None)(self.dock_side()), 5.0,
                          what=f"the panel docked {want}", describe=self.dock_side)

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

    def collapse(self) -> None:
        """Collapse the panel (⋯ ▸ Collapse the side panel, or the old « button) and wait for the strip.

        Returns:
            None.
        Raises:
            WaitTimeout: When the panel stayed expanded.
        """
        if self.more_button() is not None:
            self.choose_more(MORE_ITEM_IDS["collapse"])
        else:
            self.button("«").click()
        wait_until(self.collapsed, 5.0, what="the panel collapsed")

    def strip_button(self) -> Optional[uia.Element]:
        """The collapsed strip's chevron: by its AutomationId (STRIP_ID), else — 0.8.0 — by its glyph (» while docked
        left; the header's own chevron is gone while collapsed, so the glyph is unambiguous then).

        Returns:
            The button, or None while the panel is expanded.
        """
        kids = self.window.content_children()
        by_id = next((k for k in kids if k.role == "button" and k.automation_id == STRIP_ID), None)
        if by_id is not None:
            return by_id
        if self.plus(kids) is not None:
            return None  # expanded: a chevron in the content is the header's own collapse button
        return next((k for k in kids if k.role == "button" and k.name in ("»", "«")), None)

    def expand(self) -> None:
        """Click the collapsed strip's chevron and wait for the panel.

        Returns:
            None.
        Raises:
            WaitTimeout: When there is no chevron or the panel stayed collapsed.
        """
        chevron = wait_until(self.strip_button, 5.0, what="the collapsed strip's chevron")
        chevron.click()
        wait_until(lambda: not self.collapsed(), 5.0, what="the panel expanded")

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
