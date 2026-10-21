"""A stand-in for claude's fullscreen renderer that dies without cleaning up (drive.py `crash`).

It leaves the console the way claude 2.1.291 leaves it when it is killed while its fullscreen renderer
(CLAUDE_CODE_NO_FLICKER=1) is up:
  - input mode raw VT input (libuv's raw mode: window input + VT input; processed, line and echo off);
  - output mode VT processing + DISABLE_NEWLINE_AUTO_RETURN;
  - alternate screen, any-event mouse tracking + SGR encoding, focus reports, bracketed paste, hidden cursor,
    a kitty keyboard push;
then TerminateProcess()es itself, so no exit handler restores anything.

usage: faketui.py <modes file>   (writes the console modes it found BEFORE changing them)
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

k.SetConsoleMode(hin, 0x0008 | 0x0200)            # window input + VT input
k.SetConsoleMode(hout, 0x0001 | 0x0004 | 0x0008)  # processed + VT processing + DISABLE_NEWLINE_AUTO_RETURN
seq = ("\x1b[?1049h\x1b[?1003h\x1b[?1006h\x1b[?1004h\x1b[?2004h\x1b[?25l\x1b[>1u"
       "\x1b[2J\x1b[HFAKE FULLSCREEN TUI (about to be killed)\r\n")
written = ctypes.c_uint()
k.WriteConsoleW(hout, seq, len(seq), ctypes.byref(written), None)
time.sleep(1.0)
k.TerminateProcess(ctypes.c_void_p(-1), 1)  # the current process; exit code 1, no cleanup at all
