"""Mouse and keyboard as real input events (SendInput), so Agentmaster sees exactly what a user would do.

Adapted from the claude-desktops Windows agent's inputs.py (proven in this guest image):
- The pointer moves with absolute events over the virtual desktop, then the position is checked with
  GetCursorPos (a move Windows dropped would otherwise click the wrong place).
- Text is typed as Unicode key events (KEYEVENTF_UNICODE): any character on any layout, Hebrew and emoji
  included, without depending on the guest's keyboard layout. Line breaks press Return unless told otherwise.
- Chords ("ctrl+shift+t", "alt+F4") press their keys in order and release them in reverse; navigation keys get
  the extended-key flag, or apps read the numeric-keypad variant.

Real input (not UI Automation's Invoke) is what most flows need: a right-click opens a context menu, a hover
shows a tooltip, a double-click jumps, a drag reorders — none of which Invoke can do.
"""
from __future__ import annotations

import ctypes
import math
import time

from . import win32
from .win32 import (INPUT, INPUT_KEYBOARD, INPUT_MOUSE, KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_UNICODE,
                    MOUSEEVENTF_ABSOLUTE, MOUSEEVENTF_HWHEEL, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
                    MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MOVE, MOUSEEVENTF_RIGHTDOWN,
                    MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_VIRTUALDESK, MOUSEEVENTF_WHEEL, WHEEL_DELTA)

BUTTONS = {
    "left": (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP),
    "right": (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP),
    "middle": (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP),
}
"""Button name -> (down flag, up flag)."""

KEYS: dict[str, int] = {
    "ctrl": 0x11, "control": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B, "apps": 0x5D, "menu": 0x5D,
    "enter": 0x0D, "return": 0x0D, "tab": 0x09, "esc": 0x1B, "escape": 0x1B, "space": 0x20, "backspace": 0x08,
    "delete": 0x2E, "del": 0x2E, "insert": 0x2D, "home": 0x24, "end": 0x23, "pageup": 0x21, "pagedown": 0x22,
    "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27, "plus": 0xBB, "minus": 0xBD, "comma": 0xBC,
    "period": 0xBE,
}
"""Key name (lower case) -> virtual-key code. F1..F24, a..z and 0..9 are computed in vk_of."""

EXTENDED = {0x2D, 0x2E, 0x24, 0x23, 0x21, 0x22, 0x26, 0x28, 0x25, 0x27, 0x5B, 0x5C, 0x5D}
"""Virtual keys that need KEYEVENTF_EXTENDEDKEY (the navigation cluster, Win, Apps)."""


def _send(events: list) -> None:
    """Send events in one SendInput call (atomic with respect to other input).

    Args:
        events: INPUT structures.
    Returns:
        None.
    Raises:
        OSError: When Windows inserted fewer events (input blocked by a higher-integrity window, or no
            input desktop, e.g. the lock screen).
    """
    array = (INPUT * len(events))(*events)
    sent = win32.SendInput(len(events), array, ctypes.sizeof(INPUT))
    if sent != len(events):
        raise win32.last_error(f"SendInput ({sent} of {len(events)} events)")


def _mouse(flags: int, x: int = 0, y: int = 0, data: int = 0) -> INPUT:
    """One mouse event.

    Args:
        flags: MOUSEEVENTF_* flags.
        x: Absolute 0..65535 x when MOUSEEVENTF_ABSOLUTE is set.
        y: Same for y.
        data: The wheel delta.
    Returns:
        The event.
    """
    event = INPUT(type=INPUT_MOUSE)
    event.mi.dx, event.mi.dy, event.mi.mouseData, event.mi.dwFlags = x, y, ctypes.c_uint32(data).value, flags
    return event


def _key(vk: int = 0, scan: int = 0, flags: int = 0) -> INPUT:
    """One keyboard event.

    Args:
        vk: Virtual-key code (0 for Unicode events).
        scan: Scan code, or the UTF-16 unit for KEYEVENTF_UNICODE.
        flags: KEYEVENTF_* flags.
    Returns:
        The event.
    """
    event = INPUT(type=INPUT_KEYBOARD)
    event.ki.wVk, event.ki.wScan, event.ki.dwFlags = vk, scan, flags
    return event


def _virtual_screen() -> tuple[int, int, int, int]:
    """The virtual desktop's rectangle.

    Returns:
        (left, top, width, height).
    """
    return (win32.GetSystemMetrics(win32.SM_XVIRTUALSCREEN), win32.GetSystemMetrics(win32.SM_YVIRTUALSCREEN),
            win32.GetSystemMetrics(win32.SM_CXVIRTUALSCREEN), win32.GetSystemMetrics(win32.SM_CYVIRTUALSCREEN))


def cursor() -> tuple[int, int]:
    """The pointer's position.

    Returns:
        (x, y) in screen pixels.
    """
    p = win32.wintypes.POINT()
    win32.GetCursorPos(ctypes.byref(p))
    return p.x, p.y


def _absolute(x: int, y: int) -> tuple[int, int]:
    """Map a screen pixel to SendInput's 0..65535 range over the virtual desktop.

    Args:
        x: Screen x.
        y: Screen y.
    Returns:
        (ax, ay).
    Raises:
        ValueError: When the point is outside the screen.
    """
    left, top, width, height = _virtual_screen()
    if not (left <= x < left + width and top <= y < top + height):
        raise ValueError(f"({x}, {y}) is outside the screen {width}x{height}")
    return round((x - left) * 65535 / max(width - 1, 1)), round((y - top) * 65535 / max(height - 1, 1))


def move(x: int, y: int) -> None:
    """Move the pointer.

    Args:
        x: Screen x.
        y: Screen y.
    Returns:
        None.
    Raises:
        ValueError: When the point is outside the screen.
        RuntimeError: When the pointer did not arrive (input blocked).
    """
    ax, ay = _absolute(x, y)
    _send([_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, ax, ay)])
    if cursor() != (x, y):
        win32.SetCursorPos(x, y)  # rounding in the 0..65535 mapping can land one pixel off
    if cursor() != (x, y):
        raise RuntimeError(f"the pointer is at {cursor()} instead of ({x}, {y})")


DWELL_S = 0.15
"""How long the pointer rests on a target before the press, like a hand does. A press in the same instant the
pointer arrives races the target's PointerEntered work: measured 2026-10-23 on 0.8.0, a click on "Launch
Claude" that arrived together with the pointer was lost (the button's hover opens a presets menu, which took
the press), while a human's click lands after the hover settled."""


HOLD_S = 0.06
"""How long a click holds the button down (a finger's press; a double-click's two presses still fit well
inside the system's double-click time)."""


def click(x: int, y: int, button: str = "left", count: int = 1, modifiers: list[str] | None = None,
          gap_s: float = 0.05, dwell_s: float = DWELL_S) -> None:
    """Click at a position.

    Args:
        x: Screen x.
        y: Screen y.
        button: left, right or middle.
        count: Clicks (2 = a double-click: the gap stays far below the system's double-click time).
        modifiers: Keys held during the clicks, e.g. ["shift"].
        gap_s: The pause between clicks. ⚠ A "slow double-click" (Agentmaster's rename gesture) needs a gap
            ABOVE the double-click time: pass gap_s explicitly.
        dwell_s: The rest between arriving and pressing (DWELL_S; 0 = press on arrival, a flow about racing
            the hover).
    Returns:
        None.
    Raises:
        ValueError: On an unknown button or key, or a point outside the screen.
        OSError / RuntimeError: When Windows blocked the input.
    """
    down, up = BUTTONS[button]
    mods = [vk_of(m) for m in (modifiers or [])]
    move(x, y)
    if dwell_s:
        time.sleep(dwell_s)
    if mods:
        _send([_key(vk, _scan(vk), _flags_for(vk, False)) for vk in mods])
    try:
        for i in range(count):
            # The button is HELD between its down and up events, like a finger: a down and an up sent in
            # one SendInput call are both queued before the app handles the press, so code that reads the
            # button's live state while handling the press (GetAsyncKeyState) sees it already released —
            # measured 2026-10-23: Agentmaster's one-click Launch over its presets menu reads exactly that.
            _send([_mouse(down)])
            time.sleep(HOLD_S)
            _send([_mouse(up)])
            if i + 1 < count:
                time.sleep(gap_s)
    finally:
        if mods:
            _send([_key(vk, _scan(vk), _flags_for(vk, True)) for vk in reversed(mods)])


def drag(points: list[tuple[int, int]], button: str = "left", step_px: int = 12, step_s: float = 0.008,
         hold_s: float = 0.0) -> None:
    """Press at the first point, glide through the others, release at the last.

    The glide (small steps a few ms apart) matters: Agentmaster's tab drag starts only after the pointer moved
    6 DIP with the button down, and a jump from A to B never looks like a drag.

    Args:
        points: Two or more (x, y).
        button: left, right or middle.
        step_px: Pixels per step.
        step_s: Pause per step.
        hold_s: Pause at the last point before the release (lets a drop target's preview appear).
    Returns:
        None.
    Raises:
        ValueError: On fewer than two points, an unknown button, or a point outside the screen.
        OSError: When Windows blocked the input.
    """
    if len(points) < 2:
        raise ValueError("a drag needs at least two points")
    down, up = BUTTONS[button]
    move(*points[0])
    _send([_mouse(down)])
    try:
        for (x1, y1), (x2, y2) in zip(points, points[1:]):
            steps = max(4, int(math.hypot(x2 - x1, y2 - y1) / step_px))
            for step in range(1, steps + 1):
                ax, ay = _absolute(round(x1 + (x2 - x1) * step / steps), round(y1 + (y2 - y1) * step / steps))
                _send([_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, ax, ay)])
                time.sleep(step_s)
        if hold_s:
            time.sleep(hold_s)
    finally:
        _send([_mouse(up)])


def scroll(x: int, y: int, clicks: int) -> None:
    """Turn the mouse wheel over a position.

    Args:
        x: Screen x.
        y: Screen y.
        clicks: Wheel notches; positive scrolls DOWN (content moves up).
    Returns:
        None.
    Raises:
        ValueError: When the point is outside the screen.
        OSError: When Windows blocked the input.
    """
    move(x, y)
    _send([_mouse(MOUSEEVENTF_WHEEL, data=-clicks * WHEEL_DELTA)])


def hscroll(x: int, y: int, clicks: int) -> None:
    """Turn the horizontal wheel over a position.

    Args:
        x: Screen x.
        y: Screen y.
        clicks: Notches; positive scrolls right.
    Returns:
        None.
    Raises:
        ValueError: When the point is outside the screen.
        OSError: When Windows blocked the input.
    """
    move(x, y)
    _send([_mouse(MOUSEEVENTF_HWHEEL, data=clicks * WHEEL_DELTA)])


def vk_of(name: str) -> int:
    """The virtual-key code of a key name.

    Args:
        name: "ctrl", "F5", "a", "enter", "," ...
    Returns:
        The code.
    Raises:
        ValueError: When the name is unknown.
    """
    lower = name.lower()
    if lower in KEYS:
        return KEYS[lower]
    if len(lower) >= 2 and lower[0] == "f" and lower[1:].isdigit() and 1 <= int(lower[1:]) <= 24:
        return 0x70 + int(lower[1:]) - 1
    if len(name) == 1:
        if name.isascii() and name.isalnum():
            return ord(name.upper())
        scan = win32.VkKeyScanW(name)
        if scan != -1:
            return scan & 0xFF
    raise ValueError(f"unknown key {name!r}")


def _chord_keys(chord: str) -> list[int]:
    """Split a chord into virtual keys.

    Args:
        chord: "ctrl+shift+t", "enter", "ctrl+plus" ("+" alone is a key too).
    Returns:
        The keys in press order.
    Raises:
        ValueError: On unknown names or an empty chord.
    """
    if chord in ("+", "plus"):
        return [vk_of("plus")]
    parts = [p for p in chord.split("+") if p]
    if not parts:
        raise ValueError(f"empty chord {chord!r}")
    return [vk_of(p) for p in parts]


def _flags_for(vk: int, up: bool) -> int:
    """Keyboard flags for a virtual key.

    Args:
        vk: The key.
        up: A release.
    Returns:
        KEYEVENTF_EXTENDEDKEY when needed, plus KEYEVENTF_KEYUP for a release.
    """
    return (KEYEVENTF_EXTENDEDKEY if vk in EXTENDED else 0) | (KEYEVENTF_KEYUP if up else 0)


def _scan(vk: int) -> int:
    """The scan code of a virtual key (some apps read scan codes, not virtual keys).

    Args:
        vk: The key.
    Returns:
        The scan code (0 when there is none).
    """
    return int(win32.MapVirtualKeyW(vk, 0))


def keys(*chords: str, gap_s: float = 0.03) -> None:
    """Press chords one after another (each: keys down in order, up in reverse).

    Args:
        *chords: e.g. "ctrl+shift+t", "enter".
        gap_s: The pause after each chord.
    Returns:
        None.
    Raises:
        ValueError: On unknown key names (nothing is pressed then).
        OSError: When Windows blocked the input.
    """
    parsed = [_chord_keys(c) for c in chords]  # validate everything before pressing anything
    for vks in parsed:
        events = [_key(vk, _scan(vk), _flags_for(vk, False)) for vk in vks]
        events += [_key(vk, _scan(vk), _flags_for(vk, True)) for vk in reversed(vks)]
        _send(events)
        time.sleep(gap_s)


def key_down(name: str) -> None:
    """Press a key without releasing it (pair with key_up; the caller must release it, also on failure).

    Args:
        name: The key.
    Returns:
        None.
    Raises:
        ValueError: On an unknown name.
        OSError: When Windows blocked the input.
    """
    vk = vk_of(name)
    _send([_key(vk, _scan(vk), _flags_for(vk, False))])


def key_up(name: str) -> None:
    """Release a key pressed with key_down.

    Args:
        name: The key.
    Returns:
        None.
    Raises:
        ValueError: On an unknown name.
        OSError: When Windows blocked the input.
    """
    vk = vk_of(name)
    _send([_key(vk, _scan(vk), _flags_for(vk, True))])


def type_text(text: str, delay_s: float = 0.004, newline_key: str = "enter") -> None:
    """Type text into the focused window as Unicode key events.

    Args:
        text: Any text. A newline presses `newline_key` ("\\r\\n" counts once).
        delay_s: The pause between characters (a TUI that renders on every key drops keys typed too fast).
        newline_key: The key a newline presses ("enter"; Claude Code's input box takes "shift+enter" for a
            line break inside one prompt).
    Returns:
        None.
    Raises:
        OSError: When Windows blocked the input.
    """
    for char in text.replace("\r\n", "\n"):
        if char == "\n":
            keys(newline_key, gap_s=0)
        elif char == "\t":
            _send([_key(0x09, _scan(0x09)), _key(0x09, _scan(0x09), KEYEVENTF_KEYUP)])
        else:
            units = char.encode("utf-16-le")
            codes = [int.from_bytes(units[i:i + 2], "little") for i in range(0, len(units), 2)]
            events = [_key(0, code, KEYEVENTF_UNICODE) for code in codes]
            events += [_key(0, code, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP) for code in codes]
            _send(events)
        if delay_s:
            time.sleep(delay_s)
