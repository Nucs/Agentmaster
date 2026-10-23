"""UI Automation, the driver's eyes: find Agentmaster's controls, read their state, act on them.

Measured in the test guest (2026-10-23, Agentmaster 0.8.0): the XAML Islands content IS exposed — tab items
named by their titles, the Home button (`#ManagerHomeButton`), the side panel's buttons and TextBlocks, the
board's columns and cards as TextBlocks, the terminal as a TextPattern element. Context menus and flyouts live
in SEPARATE top-level windows (class `Xaml_WindowedPopupClass`, title "PopupHost") of the same process, reached
with ElementFromHandle — the desktop root's control view does not list their content.

Reading is batched: each level's children come with every needed property in one cross-process call
(FindAllBuildCache), the pattern the claude-desktops agent's uia_worker.py proved in this guest. The RAW view
is walked (not the control view) so XAML class names and every TextBlock are visible.

⚠ COM must be multi-threaded (sys.coinit_flags = 0) BEFORE comtypes is first imported; amdriver/__init__ sets
it. A UI Automation call into a hung app can block; the suite accepts that risk in v1 (the per-test timeout and
the orchestrator's deadline end a stuck run).
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from typing import Any, Callable, Iterator, Optional

sys.coinit_flags = 0  # COINIT_MULTITHREADED; harmless when amdriver/__init__ already set it

import comtypes  # noqa: E402
import comtypes.client  # noqa: E402

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen import UIAutomationClient as U  # noqa: E402

from . import inputs, win32  # noqa: E402
from .waits import wait_until  # noqa: E402

#: UI Automation property ids (UIAutomationClient.h) read for every element in one batch.
PROP = {
    "rect": 30001, "pid": 30002, "type": 30003, "name": 30005, "has_focus": 30008, "enabled": 30010,
    "automation_id": 30011, "class_name": 30012, "help_text": 30013, "hwnd": 30020, "item_status": 30026,
    "offscreen": 30022, "can_expand": 30028, "can_invoke": 30031, "can_select": 30036, "can_text": 30040,
    "can_toggle": 30041, "can_value": 30043, "value": 30045, "expand_state": 30070, "selected": 30079,
    "toggle_state": 30086,
}
#: Pattern ids and their interfaces.
PATTERN = {"invoke": (10000, U.IUIAutomationInvokePattern), "value": (10002, U.IUIAutomationValuePattern),
           "expand": (10005, U.IUIAutomationExpandCollapsePattern), "select": (10010, U.IUIAutomationSelectionItemPattern),
           "text": (10014, U.IUIAutomationTextPattern), "toggle": (10015, U.IUIAutomationTogglePattern),
           "scroll_item": (10017, U.IUIAutomationScrollItemPattern)}
#: Control type id -> a short role name (UI Automation's own words).
ROLES = {
    50000: "button", 50002: "check box", 50003: "combo box", 50004: "edit", 50005: "hyperlink", 50006: "image",
    50007: "list item", 50008: "list", 50009: "menu", 50010: "menu bar", 50011: "menu item", 50013: "radio button",
    50014: "scroll bar", 50015: "slider", 50018: "tab", 50019: "tab item", 50020: "text", 50021: "tool bar",
    50022: "tool tip", 50023: "tree", 50024: "tree item", 50025: "custom", 50026: "group", 50030: "document",
    50031: "split button", 50032: "window", 50033: "pane", 50038: "separator",
}
TREE_SCOPE_CHILDREN = 2


class _Client:
    """The process's UI Automation client and its batch cache request (created once, on first use)."""

    _instance: Optional["_Client"] = None

    def __init__(self) -> None:
        """Create the CUIAutomation object and the cache request.

        Returns:
            None.
        Raises:
            comtypes.COMError: When UI Automation is unavailable (not in an interactive session).
        """
        self.uia = comtypes.client.CreateObject(U.CUIAutomation, interface=U.IUIAutomation)
        self.cache = self.uia.CreateCacheRequest()
        for pid in PROP.values():
            self.cache.AddProperty(pid)
        self.raw = self.uia.RawViewCondition
        self.walker = self.uia.RawViewWalker

    @classmethod
    def get(cls) -> "_Client":
        """The shared client.

        Returns:
            The client (created on the first call).
        """
        if cls._instance is None:
            cls._instance = _Client()
        return cls._instance


@dataclass
class Element:
    """One UI Automation element with its properties read in the batch that found it.

    The properties are a SNAPSHOT: XAML rebuilds lists and cards constantly, so re-find an element (or call
    refresh) after anything that may have changed it, instead of keeping it across actions.

    Attributes:
        raw: The IUIAutomationElement.
        name, role, class_name, automation_id, help_text, item_status, value: Text properties ("" when absent).
        rect: (left, top, width, height) in screen pixels.
        enabled, offscreen, has_focus, selected: Flags.
        toggle_state: 0 off, 1 on, 2 indeterminate, None when not toggleable.
        expand_state: 0 collapsed, 1 expanded, 2 partially, 3 leaf, None when not expandable.
        pid, hwnd: The owning process and the native window handle (0 for most XAML elements).
        patterns: The available pattern names (invoke, toggle, expand, select, value, text).
    """

    raw: Any
    name: str
    role: str
    class_name: str
    automation_id: str
    help_text: str
    item_status: str
    value: str
    rect: tuple
    enabled: bool
    offscreen: bool
    has_focus: bool
    selected: bool
    toggle_state: Optional[int]
    expand_state: Optional[int]
    pid: int
    hwnd: int
    patterns: frozenset

    # ---- construction --------------------------------------------------------------------------

    @staticmethod
    def wrap(raw: Any) -> "Element":
        """Read an element's cached properties.

        Args:
            raw: An IUIAutomationElement fetched with the client's cache request.
        Returns:
            The Element.
        """
        def get(key: str) -> Any:
            """One cached property (None when the element does not have it).

            Args:
                key: A PROP key.
            Returns:
                The value or None.
            """
            try:
                return raw.GetCachedPropertyValue(PROP[key])
            except (comtypes.COMError, ValueError):
                return None

        r = get("rect") or (0, 0, 0, 0)
        rect = tuple(int(round(v)) for v in r) if len(r) == 4 else (0, 0, 0, 0)
        patterns = {name for name, key in (("invoke", "can_invoke"), ("toggle", "can_toggle"), ("expand", "can_expand"),
                                           ("select", "can_select"), ("value", "can_value"), ("text", "can_text"))
                    if get(key)}
        value = get("value") if "value" in patterns else ""
        return Element(
            raw=raw, name=get("name") or "", role=ROLES.get(get("type") or 0, f"type {get('type')}"),
            class_name=get("class_name") or "", automation_id=get("automation_id") or "",
            help_text=get("help_text") or "", item_status=get("item_status") or "",
            value=value if isinstance(value, str) else "", rect=rect, enabled=bool(get("enabled")),
            offscreen=bool(get("offscreen")), has_focus=bool(get("has_focus")), selected=bool(get("selected")),
            toggle_state=get("toggle_state") if "toggle" in patterns else None,
            expand_state=get("expand_state") if "expand" in patterns else None,
            pid=int(get("pid") or 0), hwnd=int(get("hwnd") or 0), patterns=frozenset(patterns))

    @staticmethod
    def from_hwnd(hwnd: int) -> "Element":
        """The element of a native window.

        Args:
            hwnd: The window handle.
        Returns:
            Its Element.
        Raises:
            comtypes.COMError: When the window is gone.
        """
        c = _Client.get()
        return Element.wrap(c.uia.ElementFromHandleBuildCache(hwnd, c.cache))

    def refresh(self) -> "Element":
        """Read the properties again (the same UI element, its current state).

        Returns:
            A new Element.
        Raises:
            comtypes.COMError: When the element is gone.
        """
        return Element.wrap(self.raw.BuildUpdatedCache(_Client.get().cache))

    # ---- geometry -------------------------------------------------------------------------------

    @property
    def center(self) -> tuple[int, int]:
        """The center of the element's rectangle.

        Returns:
            (x, y) in screen pixels.
        """
        x, y, w, h = self.rect
        return x + w // 2, y + h // 2

    @property
    def visible(self) -> bool:
        """Whether it has a rectangle on screen.

        Returns:
            True when not offscreen and the rectangle is not empty.
        """
        return not self.offscreen and self.rect[2] > 0 and self.rect[3] > 0

    def contains(self, other: "Element") -> bool:
        """Whether another element's center lies inside this one's rectangle (layout containment).

        Args:
            other: The other element.
        Returns:
            True when inside.
        """
        x, y, w, h = self.rect
        cx, cy = other.center
        return x <= cx < x + w and y <= cy < y + h

    # ---- tree -----------------------------------------------------------------------------------

    def children(self) -> list["Element"]:
        """The raw-view children, with their properties, in one call.

        Returns:
            The children (empty when the element is gone).
        """
        c = _Client.get()
        try:
            array = self.raw.FindAllBuildCache(TREE_SCOPE_CHILDREN, c.raw, c.cache)
        except comtypes.COMError:
            return []
        return [Element.wrap(array.GetElement(i)) for i in range(array.Length)] if array else []

    def walk(self, max_depth: int = 40) -> Iterator[tuple[int, "Element"]]:
        """Every descendant, depth first, with its depth.

        Args:
            max_depth: How deep (the element itself is depth 0 and is not yielded).
        Yields:
            (depth, element).
        """
        stack = [(1, child) for child in reversed(self.children())]
        while stack:
            depth, el = stack.pop()
            yield depth, el
            if depth < max_depth:
                stack.extend((depth + 1, child) for child in reversed(el.children()))

    def find_all(self, max_depth: int = 40, **criteria: Any) -> list["Element"]:
        """Every descendant matching the criteria.

        Args:
            max_depth: How deep to search.
            **criteria: See matches().
        Returns:
            The matches, in tree order.
        """
        return [el for _, el in self.walk(max_depth) if el.matches(**criteria)]

    def find(self, max_depth: int = 40, **criteria: Any) -> Optional["Element"]:
        """The first descendant matching the criteria.

        Args:
            max_depth: How deep to search.
            **criteria: See matches().
        Returns:
            The element, or None.
        """
        for _, el in self.walk(max_depth):
            if el.matches(**criteria):
                return el
        return None

    def wait_find(self, timeout_s: float = 10.0, max_depth: int = 40, **criteria: Any) -> "Element":
        """The first descendant matching the criteria, waiting for it to appear.

        Args:
            timeout_s: How long.
            max_depth: How deep to search.
            **criteria: See matches().
        Returns:
            The element.
        Raises:
            WaitTimeout: When it never appeared.
        """
        return wait_until(lambda: self.find(max_depth, **criteria), timeout_s, what=f"an element {criteria}")

    def matches(self, name: Optional[str] = None, name_re: Optional[str] = None, role: Optional[str] = None,
                cls: Optional[str] = None, aid: Optional[str] = None, help_re: Optional[str] = None,
                value_re: Optional[str] = None, visible: Optional[bool] = None,
                pred: Optional[Callable[["Element"], bool]] = None) -> bool:
        """Whether the element fits every given criterion.

        Args:
            name: The exact name.
            name_re: A regex searched in the name.
            role: The role ("button", "text", "menu item", ...).
            cls: The exact XAML / Win32 class name ("MenuFlyoutItem", "TabViewItem", ...).
            aid: The exact AutomationId.
            help_re: A regex searched in the help text (Agentmaster's tooltips go there on some controls).
            value_re: A regex searched in the value.
            visible: Require visible (True) or not visible (False).
            pred: A last predicate.
        Returns:
            True when all hold.
        """
        if name is not None and self.name != name:
            return False
        if name_re is not None and not re.search(name_re, self.name):
            return False
        if role is not None and self.role != role:
            return False
        if cls is not None and self.class_name != cls:
            return False
        if aid is not None and self.automation_id != aid:
            return False
        if help_re is not None and not re.search(help_re, self.help_text):
            return False
        if value_re is not None and not re.search(value_re, self.value):
            return False
        if visible is not None and self.visible != visible:
            return False
        if pred is not None and not pred(self):
            return False
        return True

    def parent(self) -> Optional["Element"]:
        """The raw-view parent.

        Returns:
            The parent, or None at the root / when gone.
        """
        c = _Client.get()
        try:
            p = c.walker.GetParentElementBuildCache(self.raw, c.cache)
        except comtypes.COMError:
            return None
        return Element.wrap(p) if p else None

    def texts(self, max_depth: int = 40) -> list[str]:
        """The names of every TextBlock below (what the element shows as text, in tree order).

        Args:
            max_depth: How deep.
        Returns:
            The texts (empty names skipped).
        """
        return [el.name for _, el in self.walk(max_depth) if el.role == "text" and el.name]

    def dump(self, max_depth: int = 12, max_nodes: int = 600) -> str:
        """The subtree as indented lines (evidence for a failure report).

        Args:
            max_depth: How deep.
            max_nodes: How many lines at most.
        Returns:
            The text.
        """
        lines = [self.line(0)]
        for depth, el in self.walk(max_depth):
            if len(lines) >= max_nodes:
                lines.append("  … (cut)")
                break
            lines.append(el.line(depth))
        return "\n".join(lines)

    def line(self, depth: int = 0) -> str:
        """One dump line: role "name" #aid cls=... @x,y wxh [state].

        Args:
            depth: The indentation level.
        Returns:
            The line.
        """
        x, y, w, h = self.rect
        bits = [f"{'  ' * depth}{self.role} {self.name!r}"]
        if self.automation_id:
            bits.append(f"#{self.automation_id}")
        if self.class_name:
            bits.append(f"cls={self.class_name}")
        bits.append(f"@{x},{y} {w}x{h}")
        state = [s for s, on in (("disabled", not self.enabled), ("offscreen", self.offscreen),
                                 ("focused", self.has_focus), ("selected", self.selected)) if on]
        if self.toggle_state is not None:
            state.append(["off", "on", "mixed"][self.toggle_state] if self.toggle_state in (0, 1, 2) else "?")
        if self.expand_state is not None:
            state.append(["collapsed", "expanded", "partial", "leaf"][self.expand_state] if self.expand_state in (0, 1, 2, 3) else "?")
        if self.item_status:
            state.append(f"status={self.item_status!r}")
        if self.value:
            state.append(f"value={self.value[:60]!r}")
        if state:
            bits.append("[" + ", ".join(state) + "]")
        return " ".join(bits)

    # ---- patterns -------------------------------------------------------------------------------

    def _pattern(self, name: str) -> Any:
        """A control pattern interface.

        Args:
            name: A PATTERN key.
        Returns:
            The interface.
        Raises:
            LookupError: When the element does not support it.
        """
        pid, iface = PATTERN[name]
        unknown = self.raw.GetCurrentPattern(pid)
        if not unknown:
            raise LookupError(f"{self.role} {self.name!r} has no {name} pattern")
        return unknown.QueryInterface(iface)

    def invoke(self) -> None:
        """Invoke it (a button's click without moving the pointer; a menu item's choose).

        Returns:
            None.
        Raises:
            LookupError: When it cannot be invoked.
        """
        self._pattern("invoke").Invoke()

    def toggle(self) -> None:
        """Toggle it.

        Returns:
            None.
        Raises:
            LookupError: When it is not toggleable.
        """
        self._pattern("toggle").Toggle()

    def expand(self) -> None:
        """Expand it (a submenu, a combo box, a split button).

        Returns:
            None.
        Raises:
            LookupError: When it is not expandable.
        """
        self._pattern("expand").Expand()

    def collapse(self) -> None:
        """Collapse it.

        Returns:
            None.
        Raises:
            LookupError: When it is not expandable.
        """
        self._pattern("expand").Collapse()

    def select(self) -> None:
        """Select it (a tab item, a list item).

        Returns:
            None.
        Raises:
            LookupError: When it is not selectable.
        """
        self._pattern("select").Select()

    def set_value(self, text: str) -> None:
        """Set its value (a text box) through the ValuePattern.

        ⚠ This raises TextChanged like typing, but no key events: a flow about key handling must type instead.

        Args:
            text: The new value.
        Returns:
            None.
        Raises:
            LookupError: When it has no value pattern.
        """
        self._pattern("value").SetValue(text)

    def text(self, max_chars: int = -1) -> str:
        """Its whole text through the TextPattern (the terminal's buffer, a rich box's content).

        Args:
            max_chars: The most characters to read (-1 = all).
        Returns:
            The text.
        Raises:
            LookupError: When it has no text pattern.
        """
        return self._pattern("text").DocumentRange.GetText(max_chars) or ""

    def scroll_into_view(self) -> None:
        """Ask its scroll owner to bring it into view.

        Returns:
            None.
        Raises:
            LookupError: When it has no ScrollItem pattern.
        """
        self._pattern("scroll_item").ScrollIntoView()

    def focus(self) -> None:
        """Give it keyboard focus.

        Returns:
            None.
        Raises:
            comtypes.COMError: When it cannot take focus.
        """
        self.raw.SetFocus()

    # ---- real input -----------------------------------------------------------------------------

    def click(self, button: str = "left", count: int = 1, modifiers: Optional[list[str]] = None,
              offset: Optional[tuple[int, int]] = None, gap_s: float = 0.05,
              dwell_s: float = inputs.DWELL_S) -> None:
        """Click it with the real pointer (hover, focus and pointer handlers all run, as for a user).

        Args:
            button: left, right or middle.
            count: 2 for a double-click.
            modifiers: Keys held, e.g. ["shift"].
            offset: (dx, dy) from the top-left corner instead of the center.
            gap_s: The pause between clicks.
            dwell_s: The rest on the target before the press (inputs.DWELL_S).
        Returns:
            None.
        Raises:
            RuntimeError: When the element has no rectangle on screen.
            ValueError / OSError: From the input layer.
        """
        if not self.visible:
            raise RuntimeError(f"cannot click {self.line()}: not on screen")
        x, y = self.center if offset is None else (self.rect[0] + offset[0], self.rect[1] + offset[1])
        inputs.click(x, y, button, count, modifiers, gap_s, dwell_s)

    def right_click(self) -> None:
        """Right-click it (opens a context menu).

        Returns:
            None.
        Raises:
            RuntimeError: When it is not on screen.
        """
        self.click("right")

    def double_click(self) -> None:
        """Double-click it.

        Returns:
            None.
        Raises:
            RuntimeError: When it is not on screen.
        """
        self.click(count=2)

    def hover(self) -> None:
        """Move the pointer onto it.

        Returns:
            None.
        Raises:
            RuntimeError: When it is not on screen.
        """
        if not self.visible:
            raise RuntimeError(f"cannot hover {self.line()}: not on screen")
        inputs.move(*self.center)


def windows_of(pid: int, cls: Optional[str] = None, visible_only: bool = True) -> list[Element]:
    """The top-level windows of a process as UI Automation elements.

    Args:
        pid: The process id.
        cls: Only this window class (CASCADIA_HOSTING_WINDOW_CLASS, Xaml_WindowedPopupClass).
        visible_only: Skip hidden windows.
    Returns:
        The elements, in Z order (topmost first).
    """
    out = []
    for w in win32.top_windows(pid):
        if cls is not None and w.cls != cls:
            continue
        if visible_only and not w.visible:
            continue
        try:
            out.append(Element.from_hwnd(w.hwnd))
        except comtypes.COMError:
            continue  # closed between the enumeration and the read
    return out


def focused() -> Optional[Element]:
    """The element with keyboard focus.

    Returns:
        It, or None.
    """
    c = _Client.get()
    try:
        return Element.wrap(c.uia.GetFocusedElementBuildCache(c.cache))
    except comtypes.COMError:
        return None
