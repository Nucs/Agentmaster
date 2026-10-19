"""Measures the shipped question hook script while it WAITS (MESSAGING.md §21.30, INTERACTIVE_TOOLS.md §10.4): the time from its
start to its request at the engine, its memory, and its end on a release.

askdrive.exe (build_askdrive.py) holds every request (the "hold" action) until a stop file; the helpers are started the way
Claude Code starts the hook (exec form: powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <script>), each
with a payload on stdin and CCMGR_HOOK_PIPE naming the driver's pipe. Measured 2026-10-19 (Windows PowerShell 5.1): 0.41-0.47 s
to the request, ~93 MB working set / ~72 MB private while waiting, 0.04 s to exit after the release.

Usage:
    python -I askmem.py <empty folder> --driver <path to askdrive.exe> [--helpers 3]
"""
import argparse
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
        root: the measurement folder (the driver writes drive.jsonl there).
    Returns:
        The parsed records; [] before the driver wrote any.
    Raises:
        json.JSONDecodeError when a line is torn mid-write (the driver writes whole lines, so only at a crash).
    """
    f = root / "drive.jsonl"
    if not f.exists():
        return []
    return [json.loads(l) for l in f.read_text(encoding="utf-8").splitlines() if l.strip()]


def memory_mb(pid):
    """A process's working set and private bytes, read with Get-Process (a separate PowerShell, so it costs ~0.3 s).

    Args:
        pid: the process id.
    Returns:
        (working set MB, private MB), or None when the process is gone.
    """
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "$p = Get-Process -Id %d -ErrorAction SilentlyContinue; if ($p) { '{0} {1}' -f $p.WorkingSet64, $p.PrivateMemorySize64 }" % pid],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        return None
    ws, priv = out.split()
    return int(ws) / 1048576.0, int(priv) / 1048576.0


def main():
    """Start the helpers, measure, release, report.

    Returns:
        0 (the numbers are the result).
    Raises:
        OSError when the driver or PowerShell cannot start; subprocess.TimeoutExpired when a helper never ends.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder")
    ap.add_argument("--driver", required=True)
    ap.add_argument("--helpers", type=int, default=3)
    a = ap.parse_args()
    root = Path(a.folder).resolve()
    root.mkdir(parents=True, exist_ok=True)
    base = "agentmaster-askmem-%d" % os.getpid()
    drv = subprocess.Popen([a.driver, base, str(root)] + ["hold"] * a.helpers, stdout=open(root / "driver.out", "wb"), stderr=subprocess.STDOUT)
    end = time.time() + 20
    while time.time() < end and not any(r.get("ev") == "ready" for r in records(root)):
        time.sleep(0.2)
    env = dict(os.environ)
    env["CCMGR_HOOK_PIPE"] = "\\\\.\\pipe\\" + base
    payload = json.dumps({"session_id": "mem-%d" % os.getpid(), "transcript_path": "", "hook_event_name": "PermissionRequest", "tool_name": "AskUserQuestion",
                          "tool_input": {"questions": [{"question": "Q?", "header": "H", "options": [{"label": "a"}, {"label": "b"}], "multiSelect": False}]}})
    helpers = []
    for i in range(a.helpers):
        t0 = time.time()
        p = subprocess.Popen([PS, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(root / "agentmaster-ask.ps1")],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        p.stdin.write(payload.encode("utf-8"))
        p.stdin.close()
        while time.time() < t0 + 30 and len([r for r in records(root) if r.get("ev") == "holding"]) <= i:
            time.sleep(0.05)
        helpers.append(p)
        print("helper %d: pid %d, request at the engine %.2f s after the start" % (i + 1, p.pid, time.time() - t0), flush=True)
    time.sleep(5)
    for p in helpers:
        m = memory_mb(p.pid)
        print("helper pid %d waiting: working set %.1f MB, private %.1f MB" % (p.pid, m[0], m[1]) if m else "helper pid %d: gone" % p.pid, flush=True)
    t1 = time.time()
    (root / "stop").write_text("stop", encoding="utf-8")  # the driver's Stop() hands every waiting helper "{}"
    for p in helpers:
        out, _ = p.communicate(timeout=30)
        print("helper pid %d exited %d after the release, %.2f s; stdout %r" % (p.pid, p.returncode, time.time() - t1, out[:80]), flush=True)
    drv.wait(timeout=30)
    return 0


if __name__ == "__main__":
    sys.exit(main())
