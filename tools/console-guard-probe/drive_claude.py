"""The 2026-10-21 failure end to end with the REAL claude, in a PRIVATE ConPTY.

usage:  python -I drive_claude.py <old|new>  < /dev/null

Starts claude.exe (the user's ~/.local/bin claude; its fullscreen renderer comes from the user's settings,
CLAUDE_CODE_NO_FLICKER=1) under `pwsh -NoLogo -NoProfile -NoExit -EncodedCommand <script>`, where the script is the
host before the guard (`old`: `& claude`) or the guarded one read from ClaudeSpawn.cpp (`new`). Once claude has entered
the alternate screen and settled, it kills claude.exe with TerminateProcess (exit code 1, no exit handlers: the
incident), then plays the terminal: six SGR mouse moves and a focus-in/out pair. Prints whether that input echoed, the
exit note, and the console modes a command run from the prompt inherits. The raw capture goes to
%TEMP%\\console-guard-probe\\raw-claude-<variant>.txt.

Measured on 2026-10-21 with claude 2.1.291: old -> echoed, input mode 0x208; new -> no echo, input mode 0x1F7 and the
note "[Agentmaster] The agent exited with code 1. ...".

What it costs and leaves behind: claude starts in %USERPROFILE%\\Desktop (a folder it already trusts, so no trust
dialog), runs the user's own SessionStart hooks, makes its startup network calls and bumps ~/.claude.json's counters;
no prompt is sent (no model request) and no transcript is created. The killed claude's ~/.claude/sessions/<pid>.json
stays until another claude cleans it up. The Fleet Observer of a running Agentmaster counts the claude as `other`
(it belongs to no tab). Redirect stdin from /dev/null (see drive.py).
"""
import base64
import ctypes
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

import pyte
import winpty

sys.dont_write_bytecode = True  # no __pycache__ beside the probe in the repository
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` leaves this folder off sys.path
import guard  # noqa: E402
from drive import PROBE, VT, child_environment  # noqa: E402

PWSH = r"C:\Program Files\PowerShell\7\pwsh.exe"
CLAUDE = os.path.expandvars(r"%USERPROFILE%\.local\bin\claude.exe")
CWD = os.path.expandvars(r"%USERPROFILE%\Desktop")
OUT = os.path.join(tempfile.gettempdir(), "console-guard-probe")


def claude_children(pid: int) -> list:
    """Return the pids of the claude.exe processes whose parent is `pid` (the pwsh host).

    Args:
        pid: the host shell's process id.

    Returns:
        A list of pids (normally exactly one).

    Raises:
        OSError: powershell.exe cannot be started for the CIM query.
    """
    out = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                          f"Get-CimInstance Win32_Process -Filter 'ParentProcessId={pid}' | "
                          f"Where-Object Name -eq 'claude.exe' | ForEach-Object ProcessId"],
                         capture_output=True, text=True).stdout
    return [int(x) for x in out.split() if x.isdigit()]


def kill(pid: int) -> bool:
    """TerminateProcess(pid, 1): the abrupt end the incident had (no SessionEnd, no exit handlers, no mode restore).

    Args:
        pid: the claude.exe to end.

    Returns:
        True when the process was terminated, False when it could not be opened or ended.
    """
    handle = ctypes.windll.kernel32.OpenProcess(0x0001, False, pid)  # PROCESS_TERMINATE
    if not handle:
        return False
    try:
        return bool(ctypes.windll.kernel32.TerminateProcess(handle, 1))
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def main() -> int:
    """Run the real-claude case and print its findings.

    Returns:
        0 when the case ran to the end, 2 when claude never reached its fullscreen frame, the host had no single
        claude child, the kill failed, or the prompt never came back.

    Raises:
        OSError: pwsh or claude cannot be started, or the output folder cannot be written.
        SystemExit: guard.py could not read the guard out of ClaudeSpawn.cpp.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    variant = sys.argv[1] if len(sys.argv) > 1 else ""
    if variant not in ("old", "new"):
        print(__doc__)
        return 2
    os.makedirs(OUT, exist_ok=True)
    run = f'& "{CLAUDE}"'
    script = run if variant == "old" else guard.load_template().replace("RUN", run)
    b64 = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    proc = winpty.PtyProcess.spawn([PWSH, "-NoLogo", "-NoProfile", "-NoExit", "-EncodedCommand", b64],
                                   dimensions=(30, 120), env=child_environment(), cwd=CWD)
    screen = pyte.Screen(120, 30)
    stream = pyte.Stream(screen)
    raw, lock = [], threading.Lock()

    def reader() -> None:
        """Copy the pseudo console's output into `raw` and the pyte screen until the shell goes away.

        Returns:
            None, when the read fails (EOF or the process was terminated).
        """
        while True:
            try:
                data = proc.read(65536)
            except Exception:
                return
            if data:
                with lock:
                    raw.append(data)
                    try:
                        stream.feed(data)
                    except Exception:
                        pass

    threading.Thread(target=reader, daemon=True).start()

    def text() -> str:
        """Return everything the pseudo console has written so far (escape sequences included)."""
        with lock:
            return "".join(raw)

    def wait_for(pred, timeout: float) -> bool:
        """Poll `pred` every 200 ms.

        Args:
            pred: a no-argument callable returning truthy when the awaited output is there.
            timeout: seconds to wait at most.

        Returns:
            True when `pred` held before the timeout, else False.
        """
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.2)
        return False

    print(f"== real claude / {variant}: pwsh pid {proc.pid}")
    try:
        if not wait_for(lambda: "\x1b[?1049h" in text(), 60):
            print("  !! claude never entered the alternate screen")
            return 2
        time.sleep(6)  # let the fullscreen frame settle: mouse tracking and the raw input mode are on by now
        kids = claude_children(proc.pid)
        print(f"  claude.exe under the host: {kids}")
        if len(kids) != 1 or not kill(kids[0]):
            print("  !! could not kill exactly one claude.exe")
            return 2
        mark = len(text())
        if not wait_for(lambda: re.search(r"PS [^\r\n]*> ", text()[mark:]) is not None, 30):
            print("  !! no prompt after claude died")
            return 2
        time.sleep(1.5)
        for x in range(10, 16):
            proc.write(f"\x1b[<35;{x};5M")
            time.sleep(0.05)
        proc.write("\x1b[I")
        time.sleep(0.05)
        proc.write("\x1b[O")
        time.sleep(1.5)
        echoed = VT.sub("", text()[mark:])
        stray = re.search(r"\[555;|\[<35;|\[\d*;1\d;5[Mm]|\[I|\[O|digit-argument", echoed) is not None
        print(f"  mouse / focus input echoed at the prompt: {'YES' if stray else 'no'}")
        notes = re.findall(r"\[Agentmaster\][^\r\n]*", echoed)
        print(f"  exit note: {notes[0] if notes else '(none)'}")
        proc.write("\x1b")
        time.sleep(0.5)
        proc.write(PROBE + "\r")
        wait_for(lambda: re.search(r"MODES in=0x[0-9A-F]+ out=0x[0-9A-F]+", text()[mark:]) is not None, 40)
        found = re.findall(r"MODES in=0x[0-9A-F]+ out=0x[0-9A-F]+", text()[mark:])
        print(f"  console modes a command now inherits: {found[-1] if found else '(probe did not answer)'}")
        return 0
    finally:
        open(os.path.join(OUT, f"raw-claude-{variant}.txt"), "w", encoding="utf-8").write(text())
        proc.terminate(force=True)


if __name__ == "__main__":
    sys.exit(main())
