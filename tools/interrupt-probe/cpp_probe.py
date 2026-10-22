"""Live check of the C++ planner (doc/agentmaster/HOOKS.md "Interrupted turns").

The real claude.exe writes the interrupted transcript (as in interrupt_probe.py); the COMPILED
PlanInterruptRewind / RewindInterruptTail / ClassifyResumeContinuation of ApiErrorRewind.cpp
(rewind_probe.exe, built by build_rewind_probe.bat from a built m5 harness's objects) then plan and append
the line, and a resume shows whether the turn continues. This is the step that ties the probes' findings to
the code the app runs.

Usage (Git Bash: prefix MSYS_NO_PATHCONV=1):
    python -I cpp_probe.py <port> <root> <rewind_probe.exe> c1|c2|c3
    <port> <root>  as interrupt_probe.py (the mock must already listen on <port>, logging to <root>\\requests.jsonl)
    c1  Esc during a reply -> plan + append with the C++ code -> resume: expect the prompt sent again
    c2  Esc during the second tool call -> plan + append -> resume: expect the turn continued from the first result
    c3  a source inside a running Bash call -> classify its live transcript: expect ContinuesTurn, dropped=Bash
Environment:
    E2_WAIT  seconds c2 waits after the turn went busy before the Esc (default 16: inside the 2nd call's sleep).
Each case prints "RESULT: PASS" or "RESULT: FAIL".
"""
import os
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from interrupt_probe import Probe, exit_claude, resume_and_watch, show_rows, wait_busy  # noqa: E402


def run_probe(probe_exe, *args):
    """Run rewind_probe.exe and print what it said.

    Args:
        probe_exe: The path of rewind_probe.exe.
        *args: Its arguments: plan|apply|classify <transcript> [sid].
    Returns:
        (returncode, output): apply returns 0 only when the line was appended.
    Raises:
        OSError: When the exe cannot be started.
    """
    r = subprocess.run([probe_exe, *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout + r.stderr).strip()
    print("  rewind_probe %s -> rc=%d\n    %s" % (args[0], r.returncode, out.replace("\n", "\n    ")))
    return r.returncode, out


def interrupted_session(probe, script, wait_s, tool_esc):
    """Start a session, send a scripted prompt, press Esc after `wait_s`, and /exit.

    Args:
        probe: The Probe.
        script: The prompt with its [[mock ...]] script.
        wait_s: Seconds after the turn went busy before the Esc.
        tool_esc: Whether the Esc is meant to land in a tool call (only the printout's label).
    Returns:
        The session id.
    Raises:
        ReadyTimeout, ModalError: When the claude never shows its input box (conpty_claude).
    """
    sid = str(uuid.uuid4())
    w = probe.start(["--session-id", sid])
    w.wait_ready(timeout=40)
    w.send_prompt(script)
    print("busy after %s s" % wait_busy(w, 15))
    time.sleep(wait_s)
    w.interrupt()
    time.sleep(3.0)
    exit_claude(w)
    show_rows(probe, sid, "after the Esc (%s) + /exit" % ("tool call" if tool_esc else "reply"), last=8)
    return sid


def check_rewind(probe, probe_exe, sid, expect_requests):
    """Plan and append with the C++ code, resume, and judge whether the turn continued.

    Args:
        probe: The Probe.
        probe_exe: The path of rewind_probe.exe.
        sid: The interrupted session.
        expect_requests: How many main requests the resume must send by itself for a PASS.
    Raises:
        OSError: When the exe cannot be started.
    """
    path = probe.tpath(sid)
    run_probe(probe_exe, "plan", path, sid)
    rc, out = run_probe(probe_exe, "apply", path, sid)
    if rc != 0:
        print("  NOT appended - stop")
        print("RESULT: FAIL")
        return
    w, reqs = resume_and_watch(probe, sid, [], 25, "resume after the C++ rewind")
    time.sleep(3.0)
    w.interrupt()
    time.sleep(2.0)
    exit_claude(w)
    show_rows(probe, sid, "after the resume", last=10)
    print("RESULT: %d main request(s) after the C++ line (expected >= %d) -> %s" %
          (len(reqs), expect_requests, "PASS" if len(reqs) >= expect_requests else "FAIL"))


def c1(probe, probe_exe):
    """Esc during a reply: the C++ line must make the resume send the prompt again.

    Args:
        probe: The Probe.
        probe_exe: The path of rewind_probe.exe.
    """
    sid = interrupted_session(probe, 'c1 prompt [[mock: say "partial text"; delay 15000; reply "the end"]]', 3.0, False)
    check_rewind(probe, probe_exe, sid, 1)


def c2(probe, probe_exe):
    """Esc inside the second tool call: the C++ line must make the resume continue from the first result.

    Args:
        probe: The Probe.
        probe_exe: The path of rewind_probe.exe.
    Raises:
        ValueError: When E2_WAIT is not a number.
    """
    sid = interrupted_session(probe, 'c2 prompt [[mock: tool Bash {"command": "echo one", "description": "first"}; '
                                     'tool Bash {"command": "sleep 25", "description": "long"}; reply "done"]]',
                              float(os.environ.get("E2_WAIT", "16")), True)
    check_rewind(probe, probe_exe, sid, 1)


def c3(probe, probe_exe):
    """Classify a LIVE source inside a running Bash call: a fork of it would continue the turn and re-run Bash.

    Args:
        probe: The Probe.
        probe_exe: The path of rewind_probe.exe.
    Raises:
        ReadyTimeout, ModalError: When the source never shows its input box (conpty_claude).
    """
    src = str(uuid.uuid4())
    w = probe.start(["--session-id", src])
    w.wait_ready(timeout=40)
    w.send_prompt('c3 prompt [[mock: tool Bash {"command": "sleep 40", "description": "long"}; reply "done"]]')
    print("source busy after %s s" % wait_busy(w, 15))
    time.sleep(9.0)
    show_rows(probe, src, "source inside the running Bash call", last=6)
    rc, out = run_probe(probe_exe, "classify", probe.tpath(src))
    print("RESULT: %s" % ("PASS" if ("dropped=Bash" in out and ("ContinuesTurn" in out or "ResendsPrompt" in out)) else "FAIL"))
    w.interrupt()
    time.sleep(2.0)
    exit_claude(w)


def main(argv):
    """Run one case.

    Args:
        argv: <port> <root> <rewind_probe.exe> <case>.
    Returns:
        0.
    Raises:
        SystemExit: With the usage text for missing arguments, an unknown case or a missing exe.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(argv) < 4 or argv[3] not in ("c1", "c2", "c3") or not os.path.isfile(argv[2]):
        raise SystemExit(__doc__)
    port, root, probe_exe, case = int(argv[0]), argv[1], argv[2], argv[3]
    probe = Probe(port, root)
    {"c1": c1, "c2": c2, "c3": c3}[case](probe, probe_exe)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
