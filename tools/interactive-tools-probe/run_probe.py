"""Live probe: can a PermissionRequest hook answer Claude Code's interactive tools?

Drives a real claude.exe TUI on a ConPTY (the repository's claude-wrap-ConPTY driver) in a throwaway folder,
with a --settings file whose only hook is a PermissionRequest hook on "AskUserQuestion|ExitPlanMode"
(perm_hook.py). Four scenarios, each judged on the TRANSCRIPT, never on the screen:

  A  the hook answers an AskUserQuestion (allow + updatedInput.answers)
  B  the PC answers first (Enter in the dialog); the hook's later answer must be ignored, and the probe
     checks whether Claude Code ends the waiting hook process
  C  the hook approves ExitPlanMode with updatedInput {} and no mode change (the pre-plan mode comes back)
  D  the hook rejects ExitPlanMode with feedback, then approves the revised plan with setMode "default"

First run: 2026-10-19, Claude Code 2.1.291, all four passed (doc/agentmaster/INTERACTIVE_TOOLS.md §6).
Re-run it after a Claude Code upgrade that touches hooks or these tools.

Usage:
    python -I run_probe.py <empty-scratch-folder> [--model haiku]
It writes the settings file, the hook log, the reply files and screen captures into the folder, and runs
claude in <folder>\\ws. It costs a few turns of the given model and leaves a real transcript under
~/.claude/projects for that folder. CLAUDE_CODE_SANDBOXED=1 is set on the child so the folder needs no
workspace-trust entry in ~/.claude.json (nothing outside the folder is written by the probe itself).

Exit code: 0 when all four scenarios pass, 1 otherwise.
"""
import argparse
import ctypes
import json
import os
import sys
import time
from pathlib import Path

# The driver lives in the repository's skill folder: <repo>/.claude/skills/claude-wrap-ConPTY/scripts.
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / ".claude" / "skills" / "claude-wrap-ConPTY" / "scripts"))
from conpty_claude import ClaudeWrap  # noqa: E402

# Claude Code's own text for "No, keep planning" with feedback (cancelAndAbort's prefix, 2.1.291). The hook's
# deny reuses it, so the model reads exactly what the PC dialog would have given it.
KEEP_PLANNING = ("The user doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a "
                 "file edit, the new_string was NOT written to the file). To tell you how to proceed, the user said:\n")


class Probe:
    """The probe's files and the helpers that talk to the waiting hook processes.

    Attributes:
        root: the scratch folder given on the command line.
        ws: the folder claude runs in.
        log: the hook's JSONL log.
        replies: where reply-<pid>.json files are dropped for a waiting hook.
        settings: the --settings file holding the PermissionRequest hook.
    """

    def __init__(self, root):
        """Lay out the probe's paths under `root` and create its folders.

        Args:
            root: the scratch folder (created when missing).
        Raises:
            OSError when a folder cannot be created.
        """
        self.root = Path(root).resolve()
        self.ws = self.root / "ws"
        self.log = self.root / "hooklog.jsonl"
        self.replies = self.root / "replies"
        self.settings = self.root / "settings.json"
        self.ws.mkdir(parents=True, exist_ok=True)
        self.replies.mkdir(parents=True, exist_ok=True)

    def write_settings(self):
        """Write the --settings file: one exec-form PermissionRequest hook running perm_hook.py.

        The hook runs with this very interpreter, isolated (-I), with a 600 s timeout: longer than any
        scenario, so a scenario's wait is never cut short by Claude Code.

        Returns:
            None.
        Raises:
            OSError when the file cannot be written.
        """
        hook = str(Path(__file__).resolve().parent / "perm_hook.py")
        settings = {"hooks": {"PermissionRequest": [{"matcher": "AskUserQuestion|ExitPlanMode", "hooks": [
            {"type": "command", "command": sys.executable, "args": ["-I", hook], "timeout": 600}]}]}}
        self.settings.write_text(json.dumps(settings, indent=2), encoding="utf-8")

    def records(self):
        """Every record the hook processes wrote so far.

        Returns:
            The parsed JSONL records (start / replied / timeout), oldest first; [] before the first hook ran.
        """
        if not self.log.exists():
            return []
        return [json.loads(l) for l in self.log.read_text(encoding="utf-8").splitlines() if l.strip()]

    def starts(self):
        """How many hook processes have started, so the next new one can be told apart.

        Returns:
            The count of "start" records.
        """
        return sum(1 for r in self.records() if r["ev"] == "start")

    def wait_start(self, n_before, tool, timeout=150.0):
        """Wait for a NEW hook start record for `tool`.

        Args:
            n_before: the start count before the prompt was sent (only later records count).
            tool: "AskUserQuestion" or "ExitPlanMode".
            timeout: seconds to wait.
        Returns:
            The start record (its "payload" is what Claude Code handed the hook, "pid" the waiting process),
            or None when no such hook started in time.
        """
        end = time.time() + timeout
        while time.time() < end:
            starts = [r for r in self.records() if r["ev"] == "start"]
            for r in starts[n_before:]:
                if r["payload"].get("tool_name") == tool:
                    return r
            time.sleep(0.3)
        return None

    def reply(self, pid, decision):
        """Hand the waiting hook process its answer; it prints it as its JSON output and exits.

        Args:
            pid: the hook process's pid (from its start record).
            decision: the PermissionRequest decision object ({"behavior": "allow"|"deny", ...}).
        Returns:
            None.
        Raises:
            OSError when the reply file cannot be written.
        """
        body = {"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}}
        tmp = self.replies / f"reply-{pid}.json.tmp"
        tmp.write_text(json.dumps(body), encoding="utf-8")
        # A rename is atomic, so the hook never reads a half-written file.
        os.replace(tmp, self.replies / f"reply-{pid}.json")

    def save_screen(self, wrap, name):
        """Keep the rendered screen as <root>/<name>.txt (evidence of what the user saw).

        Args:
            wrap: the running ClaudeWrap.
            name: the file stem.
        Returns:
            None.
        """
        (self.root / f"{name}.txt").write_text(wrap.screen(), encoding="utf-8")


def process_alive(pid):
    """Whether a process is still running, read with OpenProcess + GetExitCodeProcess (no signal is sent).

    Args:
        pid: the process id.
    Returns:
        True while the process runs; False when it exited or cannot be opened.
    """
    k32 = ctypes.windll.kernel32
    handle = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    ok = k32.GetExitCodeProcess(handle, ctypes.byref(code))
    k32.CloseHandle(handle)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


def tool_results(wrap):
    """The interactive tools' results in the transcript, in order.

    Args:
        wrap: the running ClaudeWrap (its session is resolved lazily: the transcript exists only after the
            first turn).
    Returns:
        A list of dicts: id, name, text (what the model read), tur (the structured toolUseResult), is_error.
    """
    if not wrap.session_id:
        wrap.resolve_session(timeout=3.0)
    names = {}
    found = []
    for o in wrap.transcript_lines():
        content = (o.get("message") or {}).get("content")
        if not isinstance(content, list):
            continue
        for b in content:
            if not isinstance(b, dict):
                continue
            if o.get("type") == "assistant" and b.get("type") == "tool_use":
                names[b.get("id")] = b.get("name")
            if o.get("type") == "user" and b.get("type") == "tool_result" and b.get("tool_use_id") in names:
                cont = b.get("content")
                text = cont if isinstance(cont, str) else " ".join(x.get("text", "") for x in cont if isinstance(x, dict))
                if names[b["tool_use_id"]] in ("AskUserQuestion", "ExitPlanMode"):
                    found.append({"id": b["tool_use_id"], "name": names[b["tool_use_id"]], "text": text,
                                  "tur": o.get("toolUseResult"), "is_error": b.get("is_error")})
    return found


def wait_results(wrap, n, timeout=150.0):
    """Wait until the transcript holds at least `n` interactive-tool results.

    Args:
        wrap: the running ClaudeWrap.
        n: the count to wait for.
        timeout: seconds to wait.
    Returns:
        The results found (possibly fewer than `n` on a timeout; the caller's check then fails).
    """
    end = time.time() + timeout
    res = []
    while time.time() < end:
        res = tool_results(wrap)
        if len(res) >= n:
            return res
        time.sleep(0.5)
    return res


def last_text(wrap):
    """The newest assistant text in the transcript (the model's reply after an answer).

    Args:
        wrap: the running ClaudeWrap.
    Returns:
        The text, or "" when there is none.
    """
    msgs = [t for role, t in wrap.transcript_messages() if role == "assistant" and t.strip()]
    return msgs[-1] if msgs else ""


def say(msg):
    """Print one progress line with a wall-clock stamp.

    Args:
        msg: the line.
    Returns:
        None.
    """
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def scenario_a(p, w):
    """A: the hook answers an AskUserQuestion; the model must read the hook's answer.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the transcript's tool result names the hook's answer.
    """
    n0 = p.starts()
    w.send_prompt("Use the AskUserQuestion tool to ask me exactly one question: \"Which color do you prefer?\" with "
                  "the options Red and Blue (header: Color). After I answer, reply with exactly CHOSEN=<color>.")
    st = p.wait_start(n0, "AskUserQuestion")
    if st is None:
        say("A: no PermissionRequest hook started")
        return False
    payload = st["payload"]
    say(f"A: hook payload keys {sorted(payload)}; permission_mode={payload.get('permission_mode')}")
    time.sleep(2.0)
    p.save_screen(w, "A-dialog")
    q = payload["tool_input"]["questions"][0]
    label = q["options"][1]["label"]
    # The answer is the call's own input plus `answers`, keyed by the question's text.
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": dict(payload["tool_input"], answers={q["question"]: label})})
    res = wait_results(w, 1)
    w.wait_idle(timeout=120)
    say(f"A: result {res[0]['text'][:160]!r}" if res else "A: no result")
    say(f"A: the model's reply {last_text(w)!r}")
    return bool(res) and "have been answered" in res[0]["text"] and label in res[0]["text"]


def scenario_b(p, w):
    """B: the PC answers first; the hook's late answer must be ignored.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the result holds the PC's choice and no second result appeared.
    """
    n0 = p.starts()
    w.send_prompt("Again use AskUserQuestion with exactly one question: \"Which fruit do you prefer?\" with the options "
                  "Apple and Pear (header: Fruit). After I answer, reply with exactly FRUIT=<fruit>.")
    st = p.wait_start(n0, "AskUserQuestion")
    if st is None:
        say("B: no hook started")
        return False
    time.sleep(2.5)
    p.save_screen(w, "B-dialog")
    w.submit()  # Enter at the PC picks the focused (first) option.
    res = wait_results(w, 2)
    time.sleep(3.0)
    say(f"B: the hook process is still alive 3 s after the PC answer: {process_alive(st['pid'])}")
    q = st["payload"]["tool_input"]["questions"][0]
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": dict(st["payload"]["tool_input"], answers={q["question"]: "Pear"})})
    time.sleep(2.0)
    w.wait_idle(timeout=120)
    res = tool_results(w)
    say(f"B: result {res[1]['text'][:160]!r}; the model's reply {last_text(w)!r}" if len(res) > 1 else "B: no result")
    return len(res) == 2 and "Apple" in res[1]["text"]


def scenario_c(p, w):
    """C: the hook approves the plan with updatedInput {} and no mode change.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the tool result is the approval text.
    """
    n0 = p.starts()
    w.send_prompt("Call the EnterPlanMode tool now. Then write your plan to the plan file: one line, \"Step 1: print "
                  "hello\". Then call ExitPlanMode for approval. After it is approved, reply with exactly PLAN-OK.")
    st = p.wait_start(n0, "ExitPlanMode", timeout=240)
    if st is None:
        say("C: no ExitPlanMode hook started")
        return False
    say(f"C: permission_mode={st['payload'].get('permission_mode')}; tool_input keys {sorted(st['payload']['tool_input'])}")
    time.sleep(2.0)
    p.save_screen(w, "C-dialog")
    before = len(tool_results(w))
    # {} and not the tool_input: echoing the injected `plan` back would mark the plan "edited by user".
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": {}})
    res = [r for r in wait_results(w, before + 1) if r["name"] == "ExitPlanMode"]
    w.wait_idle(timeout=180)
    p.save_screen(w, "C-after")
    say(f"C: result {res[-1]['text'][:120]!r}" if res else "C: no result")
    return bool(res) and "approved" in res[-1]["text"]


def scenario_d(p, w):
    """D: the hook rejects the plan with feedback, then approves the revision with setMode "default".

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the model read the feedback, asked again, and the second ask ended approved.
    """
    n0 = p.starts()
    w.send_prompt("Call EnterPlanMode again. Write a new one-line plan to the plan file: \"Step 1: print hello\". Then "
                  "call ExitPlanMode. If the plan is rejected, follow the feedback, update the plan file, and call "
                  "ExitPlanMode again. After approval reply with exactly PLAN2-OK.")
    st = p.wait_start(n0, "ExitPlanMode", timeout=240)
    if st is None:
        say("D: no ExitPlanMode hook started")
        return False
    p.reply(st["pid"], {"behavior": "deny", "message": KEEP_PLANNING + "Change step 1 to print bye."})
    st2 = p.wait_start(n0 + 1, "ExitPlanMode", timeout=240)
    if st2 is None:
        say("D: the model did not ask again")
        return False
    say(f"D: the revised plan {st2['payload']['tool_input'].get('plan', '')[:80]!r}")
    p.reply(st2["pid"], {"behavior": "allow", "updatedInput": {},
                         "updatedPermissions": [{"type": "setMode", "mode": "default", "destination": "session"}]})
    time.sleep(3.0)
    w.wait_idle(timeout=180)
    p.save_screen(w, "D-after")
    res = [r for r in tool_results(w) if r["name"] == "ExitPlanMode"]
    return len(res) >= 2 and "the user said" in res[-2]["text"] and "approved" in res[-1]["text"]


def main():
    """Run the four scenarios in one claude session and print the verdict.

    Returns:
        0 when all scenarios pass, 1 otherwise.
    Raises:
        conpty_claude.ModalError / ReadyTimeout when claude never reaches its input box (a startup modal).
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", help="an empty scratch folder for the probe's files")
    ap.add_argument("--model", default="haiku", help="the model for the probe's turns (default: haiku)")
    a = ap.parse_args()
    p = Probe(a.folder)
    p.write_settings()
    env = {"PROBE_HOOK_LOG": str(p.log), "PROBE_REPLY_DIR": str(p.replies), "PROBE_WAIT": "240",
           "CLAUDE_CODE_SANDBOXED": "1"}
    args = ["--model", a.model, "--settings", str(p.settings), "--dangerously-skip-permissions"]
    verdict = {}
    with ClaudeWrap(cwd=str(p.ws), args=args, env_overrides=env, cols=160, rows=50) as w:
        w.wait_ready(timeout=90)
        say(f"ready, claude pid {w.pid}")
        for name, fn in (("A", scenario_a), ("B", scenario_b), ("C", scenario_c), ("D", scenario_d)):
            verdict[name] = fn(p, w)
            if not verdict[name]:
                p.save_screen(w, f"{name}-failed")
        say(f"session {w.resolve_session()} transcript {w.transcript_path()}")
    say(f"VERDICT {verdict}")
    return 0 if all(verdict.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
