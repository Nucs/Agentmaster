"""Reproduce and check the managed host's console guard in a PRIVATE ConPTY, with a stand-in agent.

CLAUDE.md Gotcha "A killed agent leaves its fullscreen terminal modes on"; ClaudeSpawn.h WrapPwshScriptInConsoleGuard.

usage:  python -I drive.py <crash|clean> <old|new> <pwsh|ps51> [--no-input]  < /dev/null

  crash   faketui.py turns claude's fullscreen modes on and TerminateProcess()es itself (the 2026-10-21 incident)
  clean   faketui_graceful.py undoes its modes and exits 0 (the guard must then change nothing visible)
  old     the host script before the guard: `& <inner>`
  new     the guarded script, read from ClaudeSpawn.cpp (guard.py): the probe tests the code as written
  pwsh    PowerShell 7 (C:\\Program Files\\PowerShell\\7\\pwsh.exe), the launcher this machine resolves
  ps51    Windows PowerShell 5.1, the launcher when pwsh is not installed
  --no-input  send nothing once the prompt is back (checks that no stray input reaches the shell)

It spawns `<shell> -NoLogo -NoProfile -NoExit -EncodedCommand <script>` (the launch's shape, minus the profile) in a
120x30 pseudo console, waits for the stand-in to die and the prompt to return, then plays the terminal that still has
mouse tracking on: six SGR mouse moves and a focus-in/out pair. It prints whether that input echoed at the prompt,
which reset sequences reached the terminal side, the console modes a command run from the prompt now inherits, and
the stand-in's original modes. The raw capture goes to %TEMP%\\console-guard-probe.

Measured on 2026-10-21 (PowerShell 7.5.8 and 5.1.26100): crash/old -> `[555;10;5M...[I[O` at the prompt and input mode
0x208; crash/new -> no echo, input mode 0x1F7 (the original) and the exit note; clean/new -> nothing changed.

ALWAYS redirect stdin from /dev/null (or NUL). With stdout redirected to a file, pywinpty handed the shell the parent's
stdin, which under Claude Code is a pipe carrying the session's forwarded mouse input: the shell read that instead of
the probe's input. Needs pywinpty (`winpty`) and pyte; touches nothing outside its own pseudo console.
"""
import base64
import os
import re
import sys
import tempfile
import threading
import time

import pyte
import winpty

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ beside the probe in the repository
sys.path.insert(0, HERE)  # `python -I` leaves the script's own folder off sys.path; guard.py lives beside it
import guard  # noqa: E402
OUT = os.path.join(tempfile.gettempdir(), "console-guard-probe")
SHELLS = {
    "pwsh": r"C:\Program Files\PowerShell\7\pwsh.exe",
    "ps51": r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
}
# Typed at the returned prompt: prints the console modes a command run from there inherits (PSReadLine restores the
# mode it found before running a command, so this is the mode `git`, `python` or a typed `claude` would get).
PROBE = ("& {$c=[Microsoft.PowerShell.ConsoleShell].Assembly.GetType('Microsoft.PowerShell.ConsoleControl');"
         "$f=[Reflection.BindingFlags]'NonPublic,Static';$g={$c.GetMethod($args[0],$f).Invoke($null,$args[1])};"
         "'MODES in=0x{0:X} out=0x{1:X}' -f [int](&$g GetMode @(,(&$g GetConioDeviceHandle $null))),"
         "[int](&$g GetMode @(,(&$g GetActiveScreenBufferHandle $null)))}")
RESET_PIECES = ("\x1b[<99u", "\x1b[?1049l", "\x1b[?1000;1002;1003;1004;1005;1006;1007;1015;1016;2004;2026;2031l", "\x1b[!p")
VT = re.compile(r"\x1b\[[0-9;?<>=!]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[()][0-9A-Za-z]|\x1b[=>78]")


def child_environment() -> dict:
    """Return this process's environment without the variables that tie a child to Agentmaster or Claude Code.

    Returns:
        A copy of os.environ minus WT_SESSION, AM_SESSION, CLAUDECODE and every CCMGR_* / CLAUDE_CODE_* /
        AGENTMASTER_* variable, so the shell under test is correlated to no tab and no session.
    """
    return {k: v for k, v in os.environ.items()
            if not (k.upper() in ("WT_SESSION", "AM_SESSION", "CLAUDECODE")
                    or k.upper().startswith(("CCMGR_", "CLAUDE_CODE_", "AGENTMASTER_")))}


def main() -> int:
    """Run one case and print its findings.

    Returns:
        0 when the case ran to the end (the findings are in the output, not the exit code), 2 when the stand-in never
        drew, the prompt never came back, or the arguments were wrong.

    Raises:
        OSError: the shell or the stand-in cannot be started (pywinpty), or the output folder cannot be written.
        SystemExit: guard.py could not read the guard out of ClaudeSpawn.cpp.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 3 or args[0] not in ("crash", "clean") or args[1] not in ("old", "new") or args[2] not in SHELLS:
        print(__doc__)
        return 2
    case, variant, shell_key = args
    send_input = "--no-input" not in sys.argv
    os.makedirs(OUT, exist_ok=True)
    tag = f"{case}-{variant}-{shell_key}"
    modes_file = os.path.join(OUT, f"modes-{tag}.txt")
    fake = os.path.join(HERE, "faketui.py" if case == "crash" else "faketui_graceful.py")
    run = f'& "{sys.executable}" "{fake}" "{modes_file}"'
    script = run if variant == "old" else guard.load_template().replace("RUN", run)
    b64 = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    print(f"== {tag}: script {len(script)} chars")

    proc = winpty.PtyProcess.spawn([SHELLS[shell_key], "-NoLogo", "-NoProfile", "-NoExit", "-EncodedCommand", b64],
                                   dimensions=(30, 120), env=child_environment(), cwd=HERE)
    screen = pyte.Screen(120, 30)
    stream = pyte.Stream(screen)
    raw = []
    lock = threading.Lock()

    def reader() -> None:
        """Copy the pseudo console's output into `raw` and the pyte screen until the shell goes away.

        Returns:
            None, when the read fails (EOF or the process was terminated); pyte errors on a sequence it does not know
            are ignored (pyte is only a viewer here, the checks read `raw`).
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
        """Poll `pred` every 100 ms.

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
            time.sleep(0.1)
        return False

    try:
        if not wait_for(lambda: "FAKE FULLSCREEN" in text(), 30):
            print("  !! the stand-in never drew")
            return 2
        mark = len(text())
        if not wait_for(lambda: re.search(r"PS [^\r\n]*> ", text()[mark:]) is not None, 30):
            print("  !! no prompt after the stand-in ended")
            return 2
        time.sleep(1.5)
        after = text()[mark:]
        for piece in RESET_PIECES:
            print(f"  reset reached the terminal {piece!r:<72} {'yes' if piece in after else 'no'}")
        if send_input:
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
        proc.write("\x1b")  # PSReadLine: revert the line, so the probe starts on an empty one
        time.sleep(0.5)
        proc.write(PROBE + "\r")
        wait_for(lambda: re.search(r"MODES in=0x[0-9A-F]+ out=0x[0-9A-F]+", text()[mark:]) is not None, 40)
        found = re.findall(r"MODES in=0x[0-9A-F]+ out=0x[0-9A-F]+", text()[mark:])
        print(f"  console modes a command now inherits: {found[-1] if found else '(probe did not answer)'}")
        try:
            print("  stand-in's " + open(modes_file).read().strip())
        except OSError:
            pass
        return 0
    finally:
        open(os.path.join(OUT, f"raw-{tag}.txt"), "w", encoding="utf-8").write(text())
        proc.terminate(force=True)


if __name__ == "__main__":
    sys.exit(main())
