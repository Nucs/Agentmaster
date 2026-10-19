"""Live end-to-end check of MESSAGING.md §21.30 / INTERACTIVE_TOOLS.md §10.4: the SHIPPED question hook script, the real
AskBridge, and the decision lines AskQuestion.h builds, against a real claude.exe — everything of the feature but Telegram.

askdrive.exe (build_askdrive.py) writes the real agentmaster-ask.ps1 and the real hooks settings file into the folder and
answers each question request with the next action; this script drives claude through the repository's ConPTY driver (the
helpers of run_probe.py) and judges every scenario on the TRANSCRIPT:
  H1  a preview question: option 2 + a note             -> the tool result equals the PC's own form (the probe's golden D)
  H2  a preview question: a note alone ("(notes only)")   -> "(no option selected) notes: ..." (golden E)
  H3  a plain question: the engine RELEASES the hook, the PC answers with Enter -> the PC's answer; the hook is gone
  H4  a plain question: Chat about this                   -> an error result whose text is the deny message, verbatim
  H5  THREE questions in one dialog (a single-select, a multi-select with an answer of the user's own, a preview question
      with a note alone), answered tab by tab from the card -> the result equals the PC's form for those answers
  H6  a plain question: ✖ Cancel (the PC's Esc)          -> an error result whose text is Claude Code's STOP rejection, and the
      turn ends (the model does not carry out the prompt's instruction)
  H7  a plain question: a free-form reply (`response`)    -> "The user responded: <text>", not an error, and the model goes on

First run: 2026-10-19 17:00, Claude Code 2.1.291 (Haiku 4.5): H1-H4 passed; again at 17:11 with the request-first helper.

Usage:
    python -I askprobe.py <empty folder> --driver <path to askdrive.exe> [--model haiku] [--only H5,H6,H7]
It costs a short Haiku turn per scenario and leaves a real transcript under ~/.claude/projects for the folder's ws\\. The helper finds
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


TABS_PROMPT = ("Use the AskUserQuestion tool ONCE, with exactly three questions in one call: (1) a single-select question "
               "\"Which color do you prefer?\" (header: Color) with the options Red, Blue and Green; (2) a multi-select question "
               "\"Which toppings do you want?\" (header: Toppings) with the options Cheese, Ham and Olives; (3) a single-select "
               "question \"Which logo position do you prefer?\" (header: Logo) with the options Left and Right, each with a "
               "`preview` field holding a three-line ASCII mockup of a page header with the logo on that side. After I answer, "
               "reply with exactly DONE and nothing else.")
# H5's answers, one part per question (askdrive's tabs: syntax): Blue; Olives THEN Cheese (the PC keeps the tick order) + an answer of
# the user's own holding ", " (Tct quotes it); a note alone.
TABS_ACTION = "tabs:p1|m2.0;o=extra basil, please|n=put it in the footer"
RESPONSE_TEXT = "Spring, actually - none of these"


def main():
    """Run the chosen scenarios (H1-H7 by default) and print the verdict.

    Returns:
        0 when every scenario that ran passed, 1 otherwise.
    Raises:
        conpty_claude.ModalError / ReadyTimeout when claude never reaches its input box; OSError when the driver cannot start.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", help="an empty scratch folder")
    ap.add_argument("--driver", required=True, help="askdrive.exe (build_askdrive.py)")
    ap.add_argument("--model", default="haiku")
    ap.add_argument("--only", default="H1,H2,H3,H4,H5,H6,H7", help="the scenarios to run, comma-separated, in this order")
    a = ap.parse_args()
    chosen = [s.strip().upper() for s in a.only.split(",") if s.strip()]
    action_of = {"H1": "pick:1:" + rp.NOTE_TEXT, "H2": "notes:" + rp.NOTES_ONLY_TEXT, "H3": "release", "H4": "chat", "H5": TABS_ACTION,
                 "H6": "decline", "H7": "respond:" + RESPONSE_TEXT}
    unknown = [s for s in chosen if s not in action_of]
    if unknown:
        raise SystemExit("unknown scenario(s): %s" % ", ".join(unknown))
    root = Path(a.folder).resolve()
    ws = root / "ws"
    ws.mkdir(parents=True, exist_ok=True)
    base = "agentmaster-askprobe-%d" % os.getpid()
    # One driver action per scenario, in the order they run: the n-th question request gets the n-th action.
    actions = [action_of[s] for s in chosen]
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

        def answered_case(name, n, prompt, need_preview):
            """A scenario whose driver action ANSWERS (an allow): the result must be the PC's own form for those answers.

            Args:
                name: the scenario's name (H1, H2, H5).
                n: the driver request it is (1-based).
                prompt: what to tell claude.
                need_preview: the call must have the preview layout (H1, H2).
            Returns:
                True when the result equals the PC's form and the structured answer equals what was sent.
            """
            before = len(rp.tool_results(w)) if w.session_id else 0
            w.send_prompt(prompt)
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
            rp.say("%s: result %s the PC's form: %r; the model replied %r" % (name, "MATCHES" if got[-1]["text"] == want else "DIFFERS from", got[-1]["text"][:400], rp.last_text(w)))
            (root / ("%s.json" % name)).write_text(json.dumps({"decision": line, "result": got[-1], "expected": want}, indent=2, ensure_ascii=False), encoding="utf-8")
            return ok and (bool(dec.get("previewLayout")) or not need_preview)

        def one_question(subject, options, header, reply):
            """The prompt of a plain one-question scenario.

            Args:
                subject: the question's text.
                options: the option labels, as one phrase ("Red and Blue").
                header: the question's header.
                reply: what the model must answer with afterwards.
            Returns:
                The prompt.
            """
            return ("Use the AskUserQuestion tool to ask me exactly one question: \"%s\" with the options %s (header: %s). "
                    "After I answer, %s" % (subject, options, header, reply))

        for n, name in enumerate(chosen, 1):
            if name == "H1":
                verdict[name] = answered_case(name, n, rp.PREVIEW_PROMPT.format(q="Which footer layout do you prefer?"), True)
            elif name == "H2":
                verdict[name] = answered_case(name, n, rp.PREVIEW_PROMPT.format(q="Which sidebar layout do you prefer?"), True)
            elif name == "H5":
                verdict[name] = answered_case(name, n, TABS_PROMPT, False)
            elif name == "H3":
                # Released to the PC, which answers with Enter (the first option).
                before = len(rp.tool_results(w))
                w.send_prompt(one_question("Which color do you prefer?", "Red and Blue", "Color", "reply with exactly CHOSEN=<color>."))
                dec = wait_decided(root, n)
                ok = False
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
                    ok = bool(got) and "\"Red\"" in text and "have been answered" in text and alive is False
                verdict[name] = ok
            elif name in ("H4", "H6"):
                # A deny: Chat about this (the turn goes on) or Cancel (the PC's Esc: the turn ends).
                before = len(rp.tool_results(w))
                if name == "H4":
                    w.send_prompt(one_question("Which fruit do you prefer?", "Apple and Pear", "Fruit", "reply with exactly FRUIT=<fruit>."))
                else:
                    w.send_prompt(one_question("Which drink do you prefer?", "Tea and Coffee", "Drink", "reply with exactly DRINK=<drink>."))
                dec = wait_decided(root, n)
                ok = False
                if dec is not None:
                    line = json.loads(dec["line"])
                    decision = line["hookSpecificOutput"]["decision"]
                    msg = decision["message"]
                    res = rp.wait_results(w, before + 1)
                    w.wait_idle(timeout=120)
                    got = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
                    text = got[-1]["text"] if got else ""
                    reply = rp.last_text(w)
                    rp.say("%s: interrupt %s; is_error %s; text equals the deny message: %s; reply %r" % (name, decision.get("interrupt"), got[-1]["is_error"] if got else None,
                                                                                                            text == msg, reply))
                    (root / ("%s.json" % name)).write_text(json.dumps({"decision": line, "result": got[-1] if got else None, "reply": reply}, indent=2, ensure_ascii=False),
                                                         encoding="utf-8")
                    ok = bool(got) and text == msg and bool(got[-1]["is_error"])
                    if name == "H6":
                        # The turn ENDED: the model did not go on to answer the prompt's instruction (an Esc at the PC ends it too).
                        (root / "H6-screen.txt").write_text(w.screen(), encoding="utf-8")
                        ok = ok and "DRINK=" not in (reply or "")
                verdict[name] = ok
            elif name == "H7":
                # A free-form reply instead of the answers (Claude Code's `response`).
                before = len(rp.tool_results(w))
                w.send_prompt(one_question("Which season do you prefer?", "Summer and Winter", "Season",
                                           "reply with exactly SEASON=<season>; if I answered in my own words, reply with exactly GOT=<my words>."))
                dec = wait_decided(root, n)
                ok = False
                if dec is not None:
                    line = json.loads(dec["line"])
                    res = rp.wait_results(w, before + 1)
                    w.wait_idle(timeout=120)
                    got = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
                    text = got[-1]["text"] if got else ""
                    tur = (got[-1]["tur"] or {}) if got else {}
                    reply = rp.last_text(w)
                    want = "The user responded: " + RESPONSE_TEXT
                    rp.say("H7: is_error %s; result %r (%s); toolUseResult.response %r; reply %r" % (got[-1]["is_error"] if got else None, text[:200],
                                                                                                    "MATCHES" if text == want else "DIFFERS", tur.get("response"), reply))
                    (root / "H7.json").write_text(json.dumps({"decision": line, "result": got[-1] if got else None, "reply": reply}, indent=2, ensure_ascii=False),
                                                  encoding="utf-8")
                    ok = bool(got) and text == want and not got[-1]["is_error"]
                verdict[name] = ok
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
