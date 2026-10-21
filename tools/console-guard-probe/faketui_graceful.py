"""The well-behaved twin of faketui.py (drive.py `clean`): the same modes, all undone, then a normal exit (code 0),
the way claude leaves on /exit. The guard's unconditional reset must change nothing visible after it: no stray text,
the agent's own last line still on screen, the console modes unchanged, no exit note.

usage: faketui_graceful.py <modes file>
"""
import ctypes
import sys
import time

k = ctypes.windll.kernel32
k.CreateFileW.restype = ctypes.c_void_p
GENERIC_READ_WRITE = 0xC0000000
hin = ctypes.c_void_p(k.CreateFileW("CONIN$", GENERIC_READ_WRITE, 3, None, 3, 0, None))
hout = ctypes.c_void_p(k.CreateFileW("CONOUT$", GENERIC_READ_WRITE, 3, None, 3, 0, None))
mode = ctypes.c_uint()
k.GetConsoleMode(hin, ctypes.byref(mode))
orig_in = mode.value
k.GetConsoleMode(hout, ctypes.byref(mode))
orig_out = mode.value
with open(sys.argv[1], "w") as f:
    f.write(f"original modes in=0x{orig_in:X} out=0x{orig_out:X}\n")

k.SetConsoleMode(hin, 0x0008 | 0x0200)
k.SetConsoleMode(hout, 0x0001 | 0x0004 | 0x0008)
written = ctypes.c_uint()


def write(s: str) -> None:
    """Write text and escape sequences straight to the console screen buffer (VT processing is on at every call).

    Args:
        s: the text to write; sequences take effect in ConPTY and pass through to the probe's terminal side.

    Returns:
        None. A failed write is not reported: the driver judges the run from what reaches its side of the ConPTY.
    """
    k.WriteConsoleW(hout, s, len(s), ctypes.byref(written), None)


write("\x1b[?1049h\x1b[?1003h\x1b[?1006h\x1b[?1004h\x1b[?2004h\x1b[?25l\x1b[>1u\x1b[2J\x1b[HFAKE FULLSCREEN TUI (exits cleanly)\r\n")
time.sleep(1.0)
write("\x1b[<u\x1b[?25h\x1b[?2004l\x1b[?1004l\x1b[?1006l\x1b[?1003l\x1b[?1049l")
k.SetConsoleMode(hin, orig_in)
k.SetConsoleMode(hout, orig_out)
write("Resume this session with: fake --resume 1234\r\n")
sys.exit(0)
