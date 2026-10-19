"""Live end-to-end check of MESSAGING.md §21.30 / INTERACTIVE_TOOLS.md §10.4: the SHIPPED question hook script, the real
AskBridge, and the decision lines AskQuestion.h builds, against a real claude.exe — everything of the feature but Telegram.

askdrive.exe (build_askdrive.py) writes the real agentmaster-ask.ps1 and the real hooks settings file into the folder and
answers each question request with the next action; this script drives claude through the repository's ConPTY driver (the
helpers of run_probe.py) and judges every scenario on the TRANSCRIPT:
  H1  a preview question: option 2 + a note             -> the tool result equals the PC's own form (the probe's golden D)
  H2  a preview question: a note alone ("(notes only)")   -> "(no option selected) notes: ..." (golden E)
  H3  a plain question: the engine RELEASES the hook, the PC answers with Enter -> the PC's answer; the hook is gone
  H4  a plain question: Chat about this                   -> an error result whose text is the deny message, verbatim

First run: 2026-10-19 17:00, Claude Code 2.1.291 (Haiku 4.5): H1-H4 passed; again at 17:11 with the request-first helper.

Usage:
    python -I askprobe.py <empty folder> --driver <path to askdrive.exe> [--model haiku]
It costs four short Haiku turns and leaves a real transcript under ~/.claude/projects for the folder's ws\\. The helper finds
the driver's pipe through CCMGR_HOOK_PIPE, set on the child: the running app's pipe is never touched. Exit code 0 when every
scenario passes.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_probe as rp  # noqa: E402  (the probe's ConPTY helpers, prompts and the result mapper's port)


def drive_records(root):
    """The driver's JSONL log so far.

    Args:
        root: the probe folder.
    Returns:
        The parsed records ([] before the driver wrote any); a torn last line is skipped.
    """
    f = root / "drive.jsonl"
    if not f.exists():
        return []
    out = []
    for line in f.read_text(encoding="utf-8").splitlines():
        if line.strip():
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def wait_decided(root, n, timeout=150.0):
    """Wait until the driver has decided its n-th request.

    Args:
        root: the probe folder.
        n: the request's number (1-based).
        timeout: seconds to wait.
    Returns:
        The "decided" record, or None when it did not come in time.
    """
    end = time.time() + timeout
    while time.time() < end:
        decided = [r for r in drive_records(root) if r.get("ev") == "decided"]
        if len(decided) >= n:
            return decided[n - 1]
        time.sleep(0.3)
    return None


def main():
    """Run H1-H4 and print the verdict.

    Returns:
        0 when every scenario passed, 1 otherwise.
    Raises:
        conpty_claude.ModalError / ReadyTimeout when claude never reaches its input box; OSError when the driver cannot start.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", help="an empty scratch folder")
    ap.add_argument("--driver", required=True, help="askdrive.exe (build_askdrive.py)")
    ap.add_argument("--model", default="haiku")
    a = ap.parse_args()
    root = Path(a.folder).resolve()
    ws = root / "ws"
    ws.mkdir(parents=True, exist_ok=True)
    base = "agentmaster-askprobe-%d" % os.getpid()
    actions = ["pick:1:" + rp.NOTE_TEXT, "notes:" + rp.NOTES_ONLY_TEXT, "release", "chat"]
    drv = subprocess.Popen([a.driver, base, str(root)] + actions, stdout=open(root / "driver.out", "wb"), stderr=subprocess.STDOUT)
    end = time.time() + 20
    while time.time() < end and not any(r.get("ev") == "ready" for r in drive_records(root)):
        time.sleep(0.2)
    rp.say("driver ready: %s" % any(r.get("ev") == "ready" for r in drive_records(root)))
    env = {"CCMGR_HOOK_PIPE": "\\\\.\\pipe\\" + base, "CLAUDE_CODE_SANDBOXED": "1"}
    args = ["--model", a.model, "--settings", str(root / "settings.json"), "--dangerously-skip-permissions"]
    verdict = {}
    with rp.ClaudeWrap(cwd=str(ws), args=args, env_overrides=env, cols=160, rows=50) as w:
        w.wait_ready(timeout=90)
        rp.say("ready, claude pid %s" % w.pid)

        def preview_case(name, n, question):
            """One preview scenario: the driver's n-th action answers; the result must be the PC's own form."""
            before = len(rp.tool_results(w)) if w.session_id else 0
            w.send_prompt(rp.PREVIEW_PROMPT.format(q=question))
            dec = wait_decided(root, n)
            if dec is None:
                rp.say("%s: the driver never got a request" % name)
                return False
            rp.say("%s: request from hook pid %s, preview layout %s" % (name, dec.get("hookPid"), dec.get("previewLayout")))
            line = json.loads(dec["line"])
            upd = line["hookSpecificOutput"]["decision"]["updatedInput"]
            res = rp.wait_results(w, before + 1)
            w.wait_idle(timeout=120)
            got = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
            if not got:
                rp.say("%s: no result" % name)
                return False
            want = rp.expected_result(upd["questions"], upd["answers"], upd["annotations"])
            tur = got[-1]["tur"] or {}
            ok = got[-1]["text"] == want and tur.get("answers") == upd["answers"] and tur.get("annotations") == upd["annotations"]
            rp.say("%s: result %s the PC's form: %r; the model replied %r" % (name, "MATCHES" if got[-1]["text"] == want else "DIFFERS from", got[-1]["text"][:220], rp.last_text(w)))
            (root / ("%s.json" % name)).write_text(json.dumps({"decision": line, "result": got[-1]}, indent=2, ensure_ascii=False), encoding="utf-8")
            return ok and bool(dec.get("previewLayout"))

        verdict["H1"] = preview_case("H1", 1, "Which footer layout do you prefer?")
        verdict["H2"] = preview_case("H2", 2, "Which sidebar layout do you prefer?")

        # H3: released to the PC, which answers with Enter (the first option).
        before = len(rp.tool_results(w))
        w.send_prompt("Use the AskUserQuestion tool to ask me exactly one question: \"Which color do you prefer?\" with the options "
                      "Red and Blue (header: Color). After I answer, reply with exactly CHOSEN=<color>.")
        dec = wait_decided(root, 3)
        ok3 = False
        if dec is not None:
            time.sleep(2.5)  # the helper prints nothing and ends; the dialog is the PC's
            (root / "H3-dialog.txt").write_text(w.screen(), encoding="utf-8")
            hook_pid = dec.get("hookPid")
            alive = rp.process_alive(hook_pid) if hook_pid else None
            w.submit()
            res = rp.wait_results(w, before + 1)
            w.wait_idle(timeout=120)
            got = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
            text = got[-1]["text"] if got else ""
            rp.say("H3: hook alive 2.5 s after its release: %s; result %r; reply %r" % (alive, text[:200], rp.last_text(w)))
            ok3 = bool(got) and "\"Red\"" in text and "have been answered" in text and alive is False
        verdict["H3"] = ok3

        # H4: Chat about this.
        before = len(rp.tool_results(w))
        w.send_prompt("Use the AskUserQuestion tool to ask me exactly one question: \"Which fruit do you prefer?\" with the options "
                      "Apple and Pear (header: Fruit). After I answer, reply with exactly FRUIT=<fruit>.")
        dec = wait_decided(root, 4)
        ok4 = False
        if dec is not None:
            line = json.loads(dec["line"])
            msg = line["hookSpecificOutput"]["decision"]["message"]
            res = rp.wait_results(w, before + 1)
            w.wait_idle(timeout=120)
            got = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
            text = got[-1]["text"] if got else ""
            rp.say("H4: is_error %s; text equals the deny message: %s; reply %r" % (got[-1]["is_error"] if got else None, text == msg, rp.last_text(w)))
            (root / "H4.json").write_text(json.dumps({"decision": line, "result": got[-1] if got else None}, indent=2, ensure_ascii=False), encoding="utf-8")
            ok4 = bool(got) and text == msg and bool(got[-1]["is_error"])
        verdict["H4"] = ok4
        rp.say("session %s transcript %s" % (w.resolve_session(), w.transcript_path()))
    try:
        drv.wait(timeout=30)
    except subprocess.TimeoutExpired:
        (root / "stop").write_text("stop", encoding="utf-8")
        drv.wait(timeout=10)
    rp.say("driver finals: %s" % json.dumps([r for r in drive_records(root) if r.get("ev") == "final"]))
    rp.say("VERDICT %s" % verdict)
    return 0 if verdict and all(verdict.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
