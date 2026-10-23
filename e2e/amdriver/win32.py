"""Win32 plumbing for the driver: ctypes declarations with full signatures, windows, foreground, clipboard.

Every function gets argtypes/restype once, here: without them ctypes passes 64-bit handles as 32-bit ints and
truncates pointers, which works by accident until a handle above 2^31 crashes a call. Errors are read with
ctypes.get_last_error() (use_last_error=True), never GetLastError through ctypes, which ctypes itself may
overwrite between the two calls. Patterns adapted from the claude-desktops Windows agent
(K:\\source\\claude-desktops\\images\\windows\\agent\\claude_desktop_agent\\win32.py), where they are proven
in this very guest image.

The process is made per-monitor-DPI-aware on import, so UI Automation rectangles, cursor positions and
window rectangles are all physical pixels and agree with each other.
"""
from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from dataclasses import dataclass

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

ULONG_PTR = ctypes.c_size_t
LRESULT = ctypes.c_ssize_t


class MOUSEINPUT(ctypes.Structure):
    """SendInput mouse event (dx/dy are absolute 0..65535 when MOUSEEVENTF_ABSOLUTE is set)."""

    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    """SendInput keyboard event (wScan carries a UTF-16 unit when KEYEVENTF_UNICODE is set)."""

    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    """SendInput hardware event (never sent; present so the union has its real size)."""

    _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]


class _INPUTUNION(ctypes.Union):
    """The INPUT union: its size makes INPUT 40 bytes on x64, which SendInput checks."""

    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    """One SendInput event. A wrong size makes SendInput return 0 and do nothing, silently."""

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]


WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_UNICODE = 0x0001, 0x0002, 0x0004
MOUSEEVENTF_MOVE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x0001, 0x0002, 0x0004
MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP = 0x0008, 0x0010
MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP = 0x0020, 0x0040
MOUSEEVENTF_WHEEL, MOUSEEVENTF_HWHEEL = 0x0800, 0x1000
MOUSEEVENTF_VIRTUALDESK, MOUSEEVENTF_ABSOLUTE = 0x4000, 0x8000
WHEEL_DELTA = 120
SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 76, 77, 78, 79
SW_RESTORE, SW_MAXIMIZE, SW_MINIMIZE = 9, 3, 6
WM_CLOSE = 0x0010
CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0002


def _sig(dll, name, restype, *argtypes):
    """Declare one Win32 function's signature.

    Args:
        dll: The WinDLL.
        name: The function name.
        restype: The return type.
        *argtypes: The argument types.
    Returns:
        The function object.
    """
    fn = getattr(dll, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


EnumWindows = _sig(user32, "EnumWindows", wintypes.BOOL, WNDENUMPROC, wintypes.LPARAM)
GetWindowThreadProcessId = _sig(user32, "GetWindowThreadProcessId", wintypes.DWORD, wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
GetClassNameW = _sig(user32, "GetClassNameW", ctypes.c_int, wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
GetWindowTextW = _sig(user32, "GetWindowTextW", ctypes.c_int, wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
IsWindowVisible = _sig(user32, "IsWindowVisible", wintypes.BOOL, wintypes.HWND)
IsWindow = _sig(user32, "IsWindow", wintypes.BOOL, wintypes.HWND)
IsIconic = _sig(user32, "IsIconic", wintypes.BOOL, wintypes.HWND)
GetWindowRect = _sig(user32, "GetWindowRect", wintypes.BOOL, wintypes.HWND, ctypes.POINTER(wintypes.RECT))
GetForegroundWindow = _sig(user32, "GetForegroundWindow", wintypes.HWND)
SetForegroundWindow = _sig(user32, "SetForegroundWindow", wintypes.BOOL, wintypes.HWND)
BringWindowToTop = _sig(user32, "BringWindowToTop", wintypes.BOOL, wintypes.HWND)
ShowWindow = _sig(user32, "ShowWindow", wintypes.BOOL, wintypes.HWND, ctypes.c_int)
PostMessageW = _sig(user32, "PostMessageW", wintypes.BOOL, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
SendInput = _sig(user32, "SendInput", wintypes.UINT, wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
SetCursorPos = _sig(user32, "SetCursorPos", wintypes.BOOL, ctypes.c_int, ctypes.c_int)
GetCursorPos = _sig(user32, "GetCursorPos", wintypes.BOOL, ctypes.POINTER(wintypes.POINT))
GetSystemMetrics = _sig(user32, "GetSystemMetrics", ctypes.c_int, ctypes.c_int)
MapVirtualKeyW = _sig(user32, "MapVirtualKeyW", wintypes.UINT, wintypes.UINT, wintypes.UINT)
VkKeyScanW = _sig(user32, "VkKeyScanW", ctypes.c_short, wintypes.WCHAR)
GetDoubleClickTime = _sig(user32, "GetDoubleClickTime", wintypes.UINT)
OpenClipboard = _sig(user32, "OpenClipboard", wintypes.BOOL, wintypes.HWND)
CloseClipboard = _sig(user32, "CloseClipboard", wintypes.BOOL)
EmptyClipboard = _sig(user32, "EmptyClipboard", wintypes.BOOL)
GetClipboardData = _sig(user32, "GetClipboardData", wintypes.HANDLE, wintypes.UINT)
SetClipboardData = _sig(user32, "SetClipboardData", wintypes.HANDLE, wintypes.UINT, wintypes.HANDLE)
GlobalAlloc = _sig(kernel32, "GlobalAlloc", wintypes.HGLOBAL, wintypes.UINT, ctypes.c_size_t)
GlobalLock = _sig(kernel32, "GlobalLock", wintypes.LPVOID, wintypes.HGLOBAL)
GlobalUnlock = _sig(kernel32, "GlobalUnlock", wintypes.BOOL, wintypes.HGLOBAL)
GlobalFree = _sig(kernel32, "GlobalFree", wintypes.HGLOBAL, wintypes.HGLOBAL)


def last_error(what: str) -> OSError:
    """An OSError for the thread's last Win32 error.

    Args:
        what: What failed (the message's start).
    Returns:
        The error (raise it).
    """
    code = ctypes.get_last_error()
    return OSError(code, f"{what} failed: {ctypes.FormatError(code).strip()} ({code})")


def ensure_dpi_awareness() -> None:
    """Make the process per-monitor-DPI-aware (v2), so every coordinate is a physical pixel.

    Returns:
        None. Never raises: an older Windows without the call (or a process that already set it) keeps its
        awareness; the guest runs 100 % scaling anyway.
    """
    try:
        user32.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
        user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    except (AttributeError, OSError):
        pass


ensure_dpi_awareness()


@dataclass(frozen=True)
class Win:
    """A top-level window, as Win32 sees it.

    Attributes:
        hwnd: The handle.
        pid: The owning process id.
        cls: The window class (CASCADIA_HOSTING_WINDOW_CLASS = an Agentmaster / Windows Terminal window;
            Xaml_WindowedPopupClass = a XAML popup such as a context menu or a flyout).
        title: The window title.
        visible: IsWindowVisible.
    """

    hwnd: int
    pid: int
    cls: str
    title: str
    visible: bool

    def rect(self) -> tuple[int, int, int, int]:
        """The window rectangle in screen pixels.

        Returns:
            (left, top, width, height).
        Raises:
            OSError: When the window is gone.
        """
        r = wintypes.RECT()
        if not GetWindowRect(self.hwnd, ctypes.byref(r)):
            raise last_error("GetWindowRect")
        return r.left, r.top, r.right - r.left, r.bottom - r.top

    def alive(self) -> bool:
        """Whether the window still exists.

        Returns:
            IsWindow.
        """
        return bool(IsWindow(self.hwnd))


def top_windows(pid: int | None = None) -> list[Win]:
    """Every top-level window (optionally of one process), in Z order (topmost first).

    Args:
        pid: Only this process's windows (None = all).
    Returns:
        The windows.
    """
    found: list[Win] = []

    @WNDENUMPROC
    def callback(hwnd, _lparam):
        """Collect one window.

        Args:
            hwnd: The window.
            _lparam: Unused.
        Returns:
            True (keep enumerating).
        """
        owner = wintypes.DWORD()
        GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if pid is None or owner.value == pid:
            cls_buf = ctypes.create_unicode_buffer(256)
            GetClassNameW(hwnd, cls_buf, 256)
            title_buf = ctypes.create_unicode_buffer(512)
            GetWindowTextW(hwnd, title_buf, 512)
            found.append(Win(int(hwnd), owner.value, cls_buf.value, title_buf.value, bool(IsWindowVisible(hwnd))))
        return True

    EnumWindows(callback, 0)
    return found


def foreground_hwnd() -> int:
    """The foreground window's handle.

    Returns:
        The handle (0 when none).
    """
    return int(GetForegroundWindow() or 0)


def set_foreground(hwnd: int) -> bool:
    """Bring a window to the foreground, the way the desktop agent does it.

    Windows lets a process take the foreground only when it sent the last input event, so an unassigned
    key (VK 0xE8) is tapped first. Not Alt: an Alt tap puts XAML apps into access-key mode.

    Args:
        hwnd: The window.
    Returns:
        True when the window is the foreground window afterwards.
    """
    if IsIconic(hwnd):
        ShowWindow(hwnd, SW_RESTORE)
    events = (INPUT * 2)()
    for i, flags in enumerate((0, KEYEVENTF_KEYUP)):
        events[i].type = INPUT_KEYBOARD
        events[i].ki.wVk = 0xE8
        events[i].ki.dwFlags = flags
    SendInput(2, events, ctypes.sizeof(INPUT))
    BringWindowToTop(hwnd)
    SetForegroundWindow(hwnd)
    for _ in range(20):
        if foreground_hwnd() == hwnd:
            return True
        time.sleep(0.05)
    return foreground_hwnd() == hwnd


def maximize(hwnd: int) -> None:
    """Maximize a window.

    Args:
        hwnd: The window.
    Returns:
        None.
    """
    ShowWindow(hwnd, SW_MAXIMIZE)


def post_close(hwnd: int) -> bool:
    """Ask a window to close (WM_CLOSE), as its ✕ would.

    Args:
        hwnd: The window.
    Returns:
        Whether the message was posted.
    """
    return bool(PostMessageW(hwnd, WM_CLOSE, 0, 0))


def clipboard_text() -> str:
    """The clipboard's Unicode text.

    Returns:
        The text ("" when the clipboard holds no text).
    Raises:
        OSError: When the clipboard cannot be opened (another process holds it) after ~1 s of retries.
    """
    for _ in range(20):
        if OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise last_error("OpenClipboard")
    try:
        handle = GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return ""
        ptr = GlobalLock(handle)
        if not ptr:
            return ""
        try:
            return ctypes.wstring_at(ptr)
        finally:
            GlobalUnlock(handle)
    finally:
        CloseClipboard()


def set_clipboard_text(text: str) -> None:
    """Put Unicode text on the clipboard.

    Args:
        text: The text.
    Returns:
        None.
    Raises:
        OSError: When the clipboard cannot be opened or written.
    """
    for _ in range(20):
        if OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise last_error("OpenClipboard")
    try:
        EmptyClipboard()
        data = ctypes.create_unicode_buffer(text)
        size = ctypes.sizeof(data)
        handle = GlobalAlloc(GMEM_MOVEABLE, size)
        if not handle:
            raise last_error("GlobalAlloc")
        ptr = GlobalLock(handle)
        ctypes.memmove(ptr, data, size)
        GlobalUnlock(handle)
        if not SetClipboardData(CF_UNICODETEXT, handle):
            GlobalFree(handle)
            raise last_error("SetClipboardData")
    finally:
        CloseClipboard()


def double_click_ms() -> int:
    """The system double-click time.

    Returns:
        Milliseconds.
    """
    return int(GetDoubleClickTime())
