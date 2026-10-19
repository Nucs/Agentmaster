"""Live probe: can a PermissionRequest hook answer Claude Code's AskUserQuestion, previews and notes included?

Drives a real claude.exe TUI on a ConPTY (the repository's claude-wrap-ConPTY driver) in a throwaway folder,
with a --settings file whose only hook is a PermissionRequest hook on "AskUserQuestion" (perm_hook.py; with
--with-plans also "ExitPlanMode"). Every scenario is judged on the TRANSCRIPT, never on the screen:

  A  the hook answers a plain question (allow + updatedInput.answers)
  B  the PC answers first (Enter in the dialog); the hook's later answer must be ignored, and the probe
     checks whether Claude Code ends the waiting hook process
  C  the PC answers a PREVIEW question with a NOTE: key 2 focuses option 2, n opens the Notes field, the
     note is typed, Enter picks the focused option (the baseline the hook must match)
  D  the hook answers a preview question with option 2 + its preview + a note, built the way the PC builds it
  E  the hook answers a preview question with a note and NO option (the "(notes only)" sentinel)
  F  the PC answers a preview question with a note and no option (n, type, Enter without moving the focus)
  G  the PC picks the focused option of a preview question with NO note (is the preview still echoed?)
  With --with-plans, also (Claude Code's plan approval, out of the design's scope since 2026-10-19):
  P1 the hook approves ExitPlanMode with updatedInput {} and no mode change (the pre-plan mode comes back)
  P2 the hook rejects ExitPlanMode with feedback, then approves the revised plan with setMode "default"

First run: 2026-10-19, Claude Code 2.1.291 (doc/agentmaster/INTERACTIVE_TOOLS.md §6). Re-run it after a
Claude Code upgrade that touches hooks or AskUserQuestion.

Usage:
    python -I run_probe.py <empty-scratch-folder> [--model haiku] [--with-plans]
It writes the settings file, the hook log, the reply files, JSON records of the answers and screen captures
into the folder, and runs claude in <folder>\\ws. It costs a few turns of the given model and leaves a real
transcript under ~/.claude/projects for that folder. CLAUDE_CODE_SANDBOXED=1 is set on the child so the
folder needs no workspace-trust entry in ~/.claude.json (nothing outside the folder is written by the probe
itself).

Exit code: 0 when every scenario run passes, 1 otherwise.
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

    def write_settings(self, with_plans=False):
        """Write the --settings file: one exec-form PermissionRequest hook running perm_hook.py.

        The hook runs with this very interpreter, isolated (-I), with a 600 s timeout: longer than any
        scenario, so a scenario's wait is never cut short by Claude Code.

        Args:
            with_plans: also match ExitPlanMode (the --with-plans scenarios). Without it the hook sees only
                AskUserQuestion, the tool the design covers.
        Returns:
            None.
        Raises:
            OSError when the file cannot be written.
        """
        hook = str(Path(__file__).resolve().parent / "perm_hook.py")
        matcher = "AskUserQuestion|ExitPlanMode" if with_plans else "AskUserQuestion"
        settings = {"hooks": {"PermissionRequest": [{"matcher": matcher, "hooks": [
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

    def save_json(self, name, obj):
        """Keep a scenario's evidence (the call's input, the structured answer, the text) as <root>/<name>.json.

        Args:
            name: the file stem.
            obj: any JSON-serializable value.
        Returns:
            None.
        Raises:
            OSError when the file cannot be written; TypeError for a value JSON cannot hold.
        """
        (self.root / f"{name}.json").write_text(json.dumps(obj, indent=2, ensure_ascii=False), encoding="utf-8")


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
    # Counted before the prompt, so --only can run this scenario after others. With no session yet (the very
    # first turn) there is no transcript to count, and the count is 0 without `tool_results`' 3 s resolve.
    before = len(tool_results(w)) if w.session_id else 0
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
    res = wait_results(w, before + 1)[before:]
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
    # Relative counts (not "the second result"), so --only can run this scenario alone or in another order.
    before = len(tool_results(w)) if w.session_id else 0
    w.send_prompt("Again use AskUserQuestion with exactly one question: \"Which fruit do you prefer?\" with the options "
                  "Apple and Pear (header: Fruit). After I answer, reply with exactly FRUIT=<fruit>.")
    st = p.wait_start(n0, "AskUserQuestion")
    if st is None:
        say("B: no hook started")
        return False
    time.sleep(2.5)
    p.save_screen(w, "B-dialog")
    w.submit()  # Enter at the PC picks the focused (first) option.
    res = wait_results(w, before + 1)
    time.sleep(3.0)
    say(f"B: the hook process is still alive 3 s after the PC answer: {process_alive(st['pid'])}")
    q = st["payload"]["tool_input"]["questions"][0]
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": dict(st["payload"]["tool_input"], answers={q["question"]: "Pear"})})
    time.sleep(2.0)
    w.wait_idle(timeout=120)
    res = tool_results(w)[before:]
    say(f"B: result {res[0]['text'][:160]!r}; the model's reply {last_text(w)!r}" if res else "B: no result")
    # Exactly one new result: the hook's late "Pear" must not have produced a second one.
    return len(res) == 1 and "Apple" in res[0]["text"]


# The probe's notes. Plain ASCII on purpose: Claude Code trims a note and keeps it verbatim otherwise.
NOTE_TEXT = "use rounded corners"
NOTES_ONLY_TEXT = "none of these, make it a tab strip"
PC_NOTES_ONLY_TEXT = "neither, put the logo in the footer"


def expected_result(questions, answers, annotations):
    """The tool result Claude Code 2.1.291 writes for an AskUserQuestion answer (a port of its result mapper).

    Each answered or annotated question becomes `"Q"="A"` (or `"Q"=(no option selected)` when there is no
    answer or the answer is the "(notes only)" sentinel), followed by `selected preview:\\n<preview>` and
    `notes: <notes>` when the annotations hold them; the pieces are joined with ", ". With a note anywhere
    (or an answer that is not an option label) the text takes the "The user answered: … Read the answers
    carefully …" form; otherwise "Your questions have been answered: …". Used to check that a hook's answer
    reads to the model exactly like the PC dialog's.

    Scope: the answers + annotations cases of single-select "choice" questions. Claude Code's `response`,
    `followUp` and `afkTimeoutMs` forms, its "text" / "number" kinds and its parse of a comma-joined
    multi-select answer are not ported (the probe asks single-select questions only, so a joined answer is
    never produced; this port would call one "not plain").

    Args:
        questions: the call's `questions` list.
        answers: question text -> answer.
        annotations: question text -> {preview?, notes?}.
    Returns:
        The exact tool-result text.
    """
    parts = []
    all_plain = True
    for q in questions:
        text = q["question"]
        ans = answers.get(text)
        ann = annotations.get(text) or {}
        has_answer = bool(ans) and ans != "(notes only)"
        if not has_answer and not ann.get("notes"):
            continue
        piece = [f'"{text}"="{ans}"' if has_answer else f'"{text}"=(no option selected)']
        if ann.get("preview"):
            piece.append(f"selected preview:\n{ann['preview']}")
        if ann.get("notes"):
            piece.append(f"notes: {ann['notes']}")
            all_plain = False
        labels = {op["label"] for op in q.get("options") or []}
        if has_answer and ans not in labels:
            all_plain = False
        parts.append(" ".join(piece))
    joined = ", ".join(parts)
    if not joined:
        return "The user did not answer the questions."
    if all_plain:
        return f"Your questions have been answered: {joined}. You can now continue with these answers in mind."
    return (f"The user answered: {joined}. Read the answers carefully — they may request clarification, changes, "
            f"or that you not proceed — and follow what they actually say.")


PREVIEW_PROMPT = ("Use the AskUserQuestion tool with exactly one single-select question: \"{q}\" (header: Layout) and "
                  "three options, Left, Center and Right. Give EACH option a `preview` field holding a three-line ASCII "
                  "mockup of a page header with the logo on that side, for example \"+--------------+\\n|[LOGO]        "
                  "|\\n+--------------+\" for Left. After I answer, reply with exactly LAYOUT=<label> and nothing else.")


def preview_question_started(p, w, n0, question):
    """Send the preview-question prompt and wait for its hook; check that the call really has previews.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
        n0: the hook start count before the prompt.
        question: the question text the model is told to use.
    Returns:
        The hook's start record, or None when no hook started or the model sent no previews (the scenario
        then fails: without previews the TUI shows no notes field).
    """
    w.send_prompt(PREVIEW_PROMPT.format(q=question))
    st = p.wait_start(n0, "AskUserQuestion")
    if st is None:
        say("no hook started for the preview question")
        return None
    qs = st["payload"]["tool_input"].get("questions") or []
    previews = [op.get("preview") for op in (qs[0].get("options") if qs else []) or []]
    say(f"the question has {len(previews)} options, {sum(1 for x in previews if x)} with a preview")
    if not qs or not all(previews) or qs[0].get("multiSelect"):
        return None
    return st


def pc_answer(p, w, name, question, keys, note):
    """Open a preview question and answer it AT THE PC with keystrokes; return what the PC dialog sent.

    The hook is left waiting while the keys go in (so the PC answers first) and is released afterwards; its
    late answer is dropped by Claude Code (scenario B).

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
        name: the scenario's letter (the evidence files' stem).
        question: the question text the model is told to use.
        keys: the keystrokes before the note, e.g. ["2", "n"] (each sent alone, 0.6 s apart: Ink reads one
            chunk as one key event, and "2n" in one chunk would be no key at all).
        note: the note to type into the Notes field, or None to type nothing (the last key then submits).
    Returns:
        (question, toolUseResult, text) of the new AskUserQuestion result, or None when no hook started, the
        call had no previews, or no result was written.
    """
    n0 = p.starts()
    st = preview_question_started(p, w, n0, question)
    if st is None:
        return None
    # Counted before any key: the result is written ~0.02 s after the answer, so a count taken after the
    # Enter could already include it and the wait below would then wait for one result too many.
    before = len(tool_results(w))
    time.sleep(2.5)
    p.save_screen(w, f"{name}-dialog")
    for key in keys:
        w.send_raw(key)
        time.sleep(0.6)
    if note is not None:
        w.send_raw(note)
        time.sleep(0.8)
        p.save_screen(w, f"{name}-note")
        w.submit()
    res = wait_results(w, before + 1)
    w.wait_idle(timeout=120)
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": st["payload"]["tool_input"]})  # frees it; ignored
    last = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
    if not last:
        say(f"{name}: no result")
        return None
    q = st["payload"]["tool_input"]["questions"][0]
    tur = last[-1]["tur"] or {}
    p.save_json(f"{name}-pc-answer", {"tool_input": st["payload"]["tool_input"], "toolUseResult": tur,
                                      "text": last[-1]["text"]})
    say(f"{name}: the PC sent answers {json.dumps(tur.get('answers'))}; annotations "
        f"{json.dumps(tur.get('annotations'))[:300]}")
    return q, tur, last[-1]["text"]


def scenario_c(p, w):
    """C: the PC answers a preview question with a NOTE: key 2 focuses option 2, n opens Notes, Enter picks it.

    The preview layout's keys (2.1.291, the dialog's `A5`): a digit FOCUSES that option (it does not pick it),
    "n" opens the Notes field, and Enter in the field closes it and - because the focus moved - picks the
    focused option, which answers a one-question call at once. This is the baseline the hook's answer in D
    must reproduce.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the PC sent option 2, option 2's preview and the note, and the text is `expected_result`'s.
    """
    got = pc_answer(p, w, "C", "Which header layout do you prefer?", ["2", "n"], NOTE_TEXT)
    if got is None:
        return False
    q, tur, text = got
    opt2 = q["options"][1]
    ann = (tur.get("annotations") or {}).get(q["question"]) or {}
    want = expected_result([q], tur.get("answers") or {}, tur.get("annotations") or {})
    say(f"C: preview echoed exactly: {ann.get('preview') == opt2.get('preview')}; note {ann.get('notes')!r}; result "
        f"{'matches' if text == want else 'DIFFERS from'} the mapper: {text[:200]!r}")
    return ((tur.get("answers") or {}).get(q["question"]) == opt2["label"] and ann.get("notes") == NOTE_TEXT
            and ann.get("preview") == opt2.get("preview") and text == want)


def scenario_f(p, w):
    """F: the PC answers a preview question with a note and NO option: n, the note, Enter (focus not moved).

    With no option picked and the focus where it started, Enter in the Notes field sends the "(notes only)"
    sentinel (`_0t`) as the answer and the note as the only annotation.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the PC sent "(notes only)", the note and no preview, and the text is `expected_result`'s.
    """
    got = pc_answer(p, w, "F", "Which logo position do you prefer?", ["n"], PC_NOTES_ONLY_TEXT)
    if got is None:
        return False
    q, tur, text = got
    ann = (tur.get("annotations") or {}).get(q["question"]) or {}
    want = expected_result([q], tur.get("answers") or {}, tur.get("annotations") or {})
    say(f"F: result {'matches' if text == want else 'DIFFERS from'} the mapper: {text[:200]!r}")
    return ((tur.get("answers") or {}).get(q["question"]) == "(notes only)" and ann.get("notes") == PC_NOTES_ONLY_TEXT
            and "preview" not in ann and text == want)


def scenario_g(p, w):
    """G: the PC picks the focused (first) option of a preview question with NO note: Enter.

    Checks that the PC echoes the chosen option's preview even without a note (the model then reads
    `selected preview:` in the plain "Your questions have been answered" form).

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the PC sent option 1 and its preview and no note, and the text is `expected_result`'s.
    """
    got = pc_answer(p, w, "G", "Which menu layout do you prefer?", ["\r"], None)
    if got is None:
        return False
    q, tur, text = got
    opt1 = q["options"][0]
    ann = (tur.get("annotations") or {}).get(q["question"]) or {}
    want = expected_result([q], tur.get("answers") or {}, tur.get("annotations") or {})
    say(f"G: preview echoed exactly: {ann.get('preview') == opt1.get('preview')}; result "
        f"{'matches' if text == want else 'DIFFERS from'} the mapper: {text[:200]!r}")
    return ((tur.get("answers") or {}).get(q["question"]) == opt1["label"] and ann.get("preview") == opt1.get("preview")
            and "notes" not in ann and text == want)


def hook_answer(p, w, name, question, build):
    """Open a preview question and answer it FROM THE HOOK; return what the model read.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
        name: the scenario's letter (the evidence file's stem).
        question: the question text the model is told to use.
        build: a function (question dict) -> (answers, annotations) that composes the hook's answer the way
            a phone answer would.
    Returns:
        (question, answers, annotations, toolUseResult, text), or None when no hook started, the call had no
        previews, or no result was written.
    """
    n0 = p.starts()
    st = preview_question_started(p, w, n0, question)
    if st is None:
        return None
    ti = st["payload"]["tool_input"]
    q = ti["questions"][0]
    answers, annotations = build(q)
    before = len(tool_results(w))
    # The updated input is the call's own input plus the answer, as the PC dialog sends it (`nJ`).
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": dict(ti, answers=answers, annotations=annotations)})
    res = wait_results(w, before + 1)
    w.wait_idle(timeout=120)
    last = [r for r in res[before:] if r["name"] == "AskUserQuestion"]
    if not last:
        say(f"{name}: no result")
        return None
    tur = last[-1]["tur"] or {}
    p.save_json(f"{name}-hook-answer", {"tool_input": ti, "answers": answers, "annotations": annotations,
                                        "toolUseResult": tur, "text": last[-1]["text"], "reply": last_text(w)})
    return q, answers, annotations, tur, last[-1]["text"]


def scenario_d(p, w):
    """D: the hook answers a preview question with option 2 + its preview + a note, built the way the PC does.

    answers = {question: option 2's label}; annotations = {question: {preview: option 2's preview, notes}},
    exactly what the PC sent in C. The model must read the mapper's text for these values, and the stored
    toolUseResult must carry the same answer and annotations the PC's did.

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the text equals `expected_result` and the stored answer and annotations are the hook's.
    """
    def build(q):
        """Option 2 + its preview + the note, the shape the PC sent in C.

        Args:
            q: the question as the call holds it.
        Returns:
            (answers, annotations) for the hook's updatedInput.
        """
        opt = q["options"][1]
        return {q["question"]: opt["label"]}, {q["question"]: {"preview": opt["preview"], "notes": NOTE_TEXT}}

    got = hook_answer(p, w, "D", "Which footer layout do you prefer?", build)
    if got is None:
        return False
    q, answers, annotations, tur, text = got
    want = expected_result([q], answers, annotations)
    say(f"D: result {'matches' if text == want else 'DIFFERS from'} the PC's form: {text[:200]!r}; the model "
        f"replied {last_text(w)!r}")
    return text == want and tur.get("answers") == answers and tur.get("annotations") == annotations


def scenario_e(p, w):
    """E: the hook answers a preview question with a note and NO option (the TUI's "(notes only)" answer).

    Args:
        p: the Probe.
        w: the running ClaudeWrap.
    Returns:
        True when the result reads `"Q"=(no option selected) notes: …`, as the PC's F does.
    """
    def build(q):
        """The "(notes only)" sentinel and the note alone, the shape the PC sends in F (no preview: no option).

        Args:
            q: the question as the call holds it.
        Returns:
            (answers, annotations) for the hook's updatedInput.
        """
        return {q["question"]: "(notes only)"}, {q["question"]: {"notes": NOTES_ONLY_TEXT}}

    got = hook_answer(p, w, "E", "Which sidebar layout do you prefer?", build)
    if got is None:
        return False
    q, answers, annotations, tur, text = got
    want = expected_result([q], answers, annotations)
    say(f"E: result {'matches' if text == want else 'DIFFERS from'} the mapper: {text[:200]!r}; the model "
        f"replied {last_text(w)!r}")
    return text == want and tur.get("annotations") == annotations


def scenario_plan_approve(p, w):
    """P1 (only with --with-plans): the hook approves the plan with updatedInput {} and no mode change.

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
        say("P1: no ExitPlanMode hook started")
        return False
    say(f"P1: permission_mode={st['payload'].get('permission_mode')}; tool_input keys {sorted(st['payload']['tool_input'])}")
    time.sleep(2.0)
    p.save_screen(w, "P1-dialog")
    before = len(tool_results(w))
    # {} and not the tool_input: echoing the injected `plan` back would mark the plan "edited by user".
    p.reply(st["pid"], {"behavior": "allow", "updatedInput": {}})
    res = [r for r in wait_results(w, before + 1) if r["name"] == "ExitPlanMode"]
    w.wait_idle(timeout=180)
    p.save_screen(w, "P1-after")
    say(f"P1: result {res[-1]['text'][:120]!r}" if res else "P1: no result")
    return bool(res) and "approved" in res[-1]["text"]


def scenario_plan_revise(p, w):
    """P2 (only with --with-plans): the hook rejects the plan with feedback, then approves the revision with
    setMode "default".

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
        say("P2: no ExitPlanMode hook started")
        return False
    p.reply(st["pid"], {"behavior": "deny", "message": KEEP_PLANNING + "Change step 1 to print bye."})
    st2 = p.wait_start(n0 + 1, "ExitPlanMode", timeout=240)
    if st2 is None:
        say("P2: the model did not ask again")
        return False
    say(f"P2: the revised plan {st2['payload']['tool_input'].get('plan', '')[:80]!r}")
    p.reply(st2["pid"], {"behavior": "allow", "updatedInput": {},
                         "updatedPermissions": [{"type": "setMode", "mode": "default", "destination": "session"}]})
    time.sleep(3.0)
    w.wait_idle(timeout=180)
    p.save_screen(w, "P2-after")
    res = [r for r in tool_results(w) if r["name"] == "ExitPlanMode"]
    return len(res) >= 2 and "the user said" in res[-2]["text"] and "approved" in res[-1]["text"]


def main():
    """Run the scenarios in one claude session and print the verdict.

    Returns:
        0 when every scenario run passes, 1 otherwise.
    Raises:
        conpty_claude.ModalError / ReadyTimeout when claude never reaches its input box (a startup modal).
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", help="an empty scratch folder for the probe's files")
    ap.add_argument("--model", default="haiku", help="the model for the probe's turns (default: haiku)")
    ap.add_argument("--with-plans", action="store_true",
                    help="also run the two ExitPlanMode scenarios (P1, P2), outside the design's scope")
    ap.add_argument("--only", default="", help="run only these scenarios, comma-separated (e.g. C,D)")
    a = ap.parse_args()
    p = Probe(a.folder)
    p.write_settings(with_plans=a.with_plans)
    env = {"PROBE_HOOK_LOG": str(p.log), "PROBE_REPLY_DIR": str(p.replies), "PROBE_WAIT": "240",
           "CLAUDE_CODE_SANDBOXED": "1"}
    args = ["--model", a.model, "--settings", str(p.settings), "--dangerously-skip-permissions"]
    plan = [("A", scenario_a), ("B", scenario_b), ("C", scenario_c), ("D", scenario_d), ("E", scenario_e),
            ("F", scenario_f), ("G", scenario_g)]
    if a.with_plans:
        plan += [("P1", scenario_plan_approve), ("P2", scenario_plan_revise)]
    wanted = {x.strip().upper() for x in a.only.split(",") if x.strip()}
    if wanted:
        plan = [(n, fn) for n, fn in plan if n in wanted]
    verdict = {}
    with ClaudeWrap(cwd=str(p.ws), args=args, env_overrides=env, cols=160, rows=50) as w:
        w.wait_ready(timeout=90)
        say(f"ready, claude pid {w.pid}")
        for name, fn in plan:
            verdict[name] = fn(p, w)
            say(f"{name}: {'PASS' if verdict[name] else 'FAIL'}")
            if not verdict[name]:
                p.save_screen(w, f"{name}-failed")
        say(f"session {w.resolve_session()} transcript {w.transcript_path()}")
    say(f"VERDICT {verdict}")
    return 0 if verdict and all(verdict.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
