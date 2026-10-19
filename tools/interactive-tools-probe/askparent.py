"""The shipped question hook's PARENT check (MESSAGING.md §21.30, INTERACTIVE_TOOLS.md §10.4): a waiting helper must end by itself
when its parent exits — claude.exe in real life, since Claude Code does not end a hook whose dialog the PC answered.

askdrive.exe (build_askdrive.py) holds the request; the helper runs under `cmd /c` (its parent here); killing the cmd must end
the helper within a few seconds, with no output. Measured 2026-10-19: the helper ended 0.5 s after its parent.

Usage:
    python -I askparent.py <empty folder> --driver <path to askdrive.exe>
Exit code 0 when the helper ended within 6 s of its parent.
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from pathlib import Path

PS = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


def records(root):
    """The driver's JSONL log so far.

    Args:
        root: the check's folder (the driver writes drive.jsonl there).
    Returns:
        The parsed records; [] before the driver wrote any.
    Raises:
        json.JSONDecodeError when a line is torn mid-write (the driver writes whole lines, so only at a crash).
    """
    f = root / "drive.jsonl"
    if not f.exists():
        return []
    return [json.loads(l) for l in f.read_text(encoding="utf-8").splitlines() if l.strip()]


def alive(pid):
    """Whether a process runs, read with OpenProcess + GetExitCodeProcess (no signal is sent).

    Args:
        pid: the process id.
    Returns:
        True while it runs; False when it exited or cannot be opened.
    """
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


def main():
    """Hold one helper, kill its parent, watch the helper end.

    Returns:
        0 when the helper ended within 6 s of its parent; 1 otherwise (or when no request arrived).
    Raises:
        OSError when the driver, cmd or PowerShell cannot start.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder")
    ap.add_argument("--driver", required=True)
    a = ap.parse_args()
    root = Path(a.folder).resolve()
    root.mkdir(parents=True, exist_ok=True)
    base = "agentmaster-askparent-%d" % os.getpid()
    drv = subprocess.Popen([a.driver, base, str(root), "hold"], stdout=open(root / "driver.out", "wb"), stderr=subprocess.STDOUT)
    end = time.time() + 20
    while time.time() < end and not any(r.get("ev") == "ready" for r in records(root)):
        time.sleep(0.2)
    payload = root / "payload.json"
    payload.write_text(json.dumps({"session_id": "parent-test", "transcript_path": "", "hook_event_name": "PermissionRequest", "tool_name": "AskUserQuestion",
                                   "tool_input": {"questions": [{"question": "Q?", "options": [{"label": "a"}, {"label": "b"}]}]}}), encoding="utf-8")
    env = dict(os.environ)
    env["CCMGR_HOOK_PIPE"] = "\\\\.\\pipe\\" + base
    cmdline = 'cmd.exe /d /c ""%s" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%s" < "%s""' % (PS, root / "agentmaster-ask.ps1", payload)
    parent = subprocess.Popen(cmdline, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    end = time.time() + 30
    hold = None
    while time.time() < end and hold is None:
        hold = next((r for r in records(root) if r.get("ev") == "holding"), None)
        time.sleep(0.1)
    if hold is None:
        print("no request arrived")
        (root / "stop").write_text("stop", encoding="utf-8")
        return 1
    helper = int(hold["hookPid"])
    time.sleep(3.0)  # past the helper's parent lookup (~1.7 s), into its 2 s polls
    print("helper %d waiting: %s; killing its parent cmd %d" % (helper, alive(helper), parent.pid), flush=True)
    t0 = time.time()
    parent.kill()
    while time.time() < t0 + 15 and alive(helper):
        time.sleep(0.1)
    gone_after = time.time() - t0
    print("helper alive after the parent's end: %s (%.1f s)" % (alive(helper), gone_after), flush=True)
    (root / "stop").write_text("stop", encoding="utf-8")
    drv.wait(timeout=30)
    return 0 if not alive(helper) and gone_after < 6.0 else 1


if __name__ == "__main__":
    sys.exit(main())
