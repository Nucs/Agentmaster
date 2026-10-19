"""Census of every AskUserQuestion call on this machine — the SHAPES a Telegram question card must handle (MESSAGING.md §21.30).

survey_interactive.py counts outcomes and preview sizes; this one lists what the card itself meets: how many questions a call
has and how they mix (multi-select, previews), option counts, the lengths of headers, questions, labels and descriptions, the
fields of Claude Code's EXTENDED schema (`title`, `kind` text / number, a question's `description`, `min` / `max` / `unit` …),
the keys of the structured answer (`answers` as strings or arrays, `annotations`, `response`, `followUp`, `afkTimeoutMs`), what
each answer is (an option's label, several labels, the user's own words, "(notes only)", an image), how many questions of a
call were left unanswered, and every form of the tool result the model read. It reads main transcripts AND subagent side
files (a subagent's dialog is answered at the PC: the hook leaves at once for it), deduplicated by tool_use id (a fork or a
resume copies the lines).

With --export it writes one JSON object per call ({id, file, line, version, sidechain, probe, input, result, isError,
toolUseResult}) for askcorpus.exe, which replays each call through the card's pure core (AskQuestion.h).

Read-only: transcripts are only opened for reading; nothing is written but the export file you name.

Usage:
    python -I ask_census.py [<claude-projects-dir>] [--real-only] [--export <file.jsonl>] [--samples N]
    (default: %CLAUDE_CONFIG_DIR%\\projects, else ~/.claude/projects; needs `rg` on PATH)
    --real-only leaves out sessions that worked in a folder under %TEMP% (probes and self-tests).
    --samples N prints up to N examples of each rare shape (default 3).
"""
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

NOTES_ONLY = "(notes only)"
REJECT_FEEDBACK = "To tell you how to proceed, the user said:"
REJECT_STOP = "doesn't want to proceed"


def projects_dir(argv):
    """Find the projects folder to read.

    Args:
        argv: the command line; the first argument that does not start with "--" (and is not a flag's value) names it.
    Returns:
        The folder: the argument, else %CLAUDE_CONFIG_DIR%\\projects, else ~/.claude/projects.
    """
    skip = False
    for a in argv[1:]:
        if skip:
            skip = False
            continue
        if a in ("--export", "--samples"):
            skip = True
            continue
        if not a.startswith("--"):
            return a
    base = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(os.path.expanduser("~"), ".claude")
    return os.path.join(base, "projects")


def flag_value(argv, name, default=None):
    """Read a flag's value from the command line.

    Args:
        argv: the command line.
        name: the flag, e.g. "--export".
        default: what to return when the flag is absent.
    Returns:
        The word after the flag, or `default`.
    Raises:
        SystemExit: when the flag is the last word (no value follows it).
    """
    if name not in argv:
        return default
    i = argv.index(name)
    if i + 1 >= len(argv):
        raise SystemExit("%s needs a value" % name)
    return argv[i + 1]


def candidate_files(projects):
    """List the transcripts that hold an AskUserQuestion tool_use, with ripgrep (the corpus is ~11 GB).

    Args:
        projects: the projects folder.
    Returns:
        Paths, subagent side files included. The pattern names the tool_use block's own keys, so a transcript whose only
        mention is the tool list in a prompt snapshot is not read.
    Raises:
        FileNotFoundError: when `rg` is not on PATH.
    """
    out = subprocess.run(["rg", "-l", "--no-messages", "-g", "*.jsonl", "-e", '"name":"AskUserQuestion","input"', projects],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    return sorted(l.strip() for l in out.stdout.splitlines() if l.strip())


def result_text(content):
    """Flatten a tool_result's content to its text.

    Args:
        content: a string, or a list of content blocks (only "text" blocks carry text).
    Returns:
        The text; "" for anything else.
    """
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "\n".join(b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text")
    return ""


def is_probe_session(cwd):
    """Is this a probe or self-test session (it worked in a folder under %TEMP%)?

    Args:
        cwd: the line's working directory, or None.
    Returns:
        True when the folder lies under the user's temp folder.
    """
    if not cwd:
        return False
    temp = (os.environ.get("TEMP") or os.environ.get("TMP") or "").lower().rstrip("\\/")
    c = cwd.lower()
    return bool(temp) and c.startswith(temp)


def read_calls(path):
    """Read one transcript's AskUserQuestion calls and their results.

    Args:
        path: the transcript.
    Returns:
        A list of call records ({id, file, line, ts, version, sidechain, cwd, input}) and a dict tool_use id ->
        (text, is_error, toolUseResult, line).
    Raises:
        OSError: when the file cannot be read (the caller skips it).
    """
    calls = []
    results = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if "AskUserQuestion" not in line and "tool_result" not in line:
                continue
            try:
                o = json.loads(line)
            except ValueError:
                continue
            msg = o.get("message") or {}
            content = msg.get("content")
            if not isinstance(content, list):
                continue
            for b in content:
                if not isinstance(b, dict):
                    continue
                if b.get("type") == "tool_use" and b.get("name") == "AskUserQuestion":
                    calls.append({"id": b.get("id"), "file": path, "line": n, "ts": o.get("timestamp"), "version": o.get("version"),
                                  "sidechain": bool(o.get("isSidechain")) or ("subagents" in path.replace("\\", "/").split("/")),
                                  "cwd": o.get("cwd"), "input": b.get("input")})
                elif b.get("type") == "tool_result":
                    results[b.get("tool_use_id")] = (result_text(b.get("content")), bool(b.get("is_error")), o.get("toolUseResult"), n)
    return calls, results


def pct(values, p):
    """The p-th percentile by nearest rank.

    Args:
        values: a list of numbers (may be empty).
        p: the percentile, 0..100.
    Returns:
        The sample at rank round(p/100*(n-1)), or 0 for an empty list.
    """
    if not values:
        return 0
    v = sorted(values)
    return v[max(0, min(len(v) - 1, int(round(p / 100 * (len(v) - 1)))))]


def stats(values):
    """A one-line summary of a list of numbers.

    Args:
        values: the numbers.
    Returns:
        "n=… median=… p90=… max=…".
    """
    return "n=%d median=%s p90=%s max=%s" % (len(values), pct(values, 50), pct(values, 90), max(values) if values else 0)


def split_multi(answer):
    """Split a multi-select answer the way Claude Code's own parser `H` does (items joined by ", ", a quoted item as JSON).

    Args:
        answer: the joined answer.
    Returns:
        The items, or None when the text is not in that form.
    """
    items = []
    rest = answer
    while True:
        if rest.startswith('"'):
            end = -1
            esc = False
            for i in range(1, len(rest)):
                ch = rest[i]
                if esc:
                    esc = False
                    continue
                if ch == "\\":
                    esc = True
                    continue
                if ch == '"':
                    end = i
                    break
            if end < 0:
                return None
            try:
                v = json.loads(rest[:end + 1])
            except ValueError:
                return None
            if not isinstance(v, str):
                return None
            items.append(v)
            rest = rest[end + 1:]
        else:
            k = rest.find(", ")
            piece = rest if k < 0 else rest[:k]
            if '"' in piece:
                return None
            items.append(piece)
            rest = "" if k < 0 else rest[k:]
        if rest == "":
            return items
        if not rest.startswith(", "):
            return None
        rest = rest[2:]
        if rest == "":
            return None


def answer_kind(q, answer):
    """Name what one recorded answer is.

    Args:
        q: the question object of the call's input.
        answer: the recorded answer (string, list, or None).
    Returns:
        "unanswered", "label", "labels (multi)", "labels+own (multi)", "own words", "notes only", "image", "array", or "other".
    """
    if answer is None or answer == "":
        return "unanswered"
    if isinstance(answer, list):
        return "array"
    labels = [op.get("label") for op in (q.get("options") or []) if isinstance(op, dict)]
    if answer == NOTES_ONLY:
        return "notes only"
    if "(Image attached)" in answer:
        return "image"
    if answer in labels:
        return "label"
    if q.get("multiSelect"):
        parts = split_multi(answer)
        if parts is not None and parts and all(p in labels for p in parts):
            return "labels (multi)"
        if parts is not None and parts and any(p in labels for p in parts):
            return "labels+own (multi)"
    return "own words"


def result_form(text, is_error, tur):
    """Name the form of the tool result the model read.

    Args:
        text: the tool_result's text.
        is_error: its is_error flag.
        tur: the line's toolUseResult (or None).
    Returns:
        A short name.
    """
    t = text or ""
    if isinstance(tur, dict):
        if tur.get("afkTimeoutMs"):
            return "afk auto-continue"
        if tur.get("followUp"):
            return "followUp"
        if isinstance(tur.get("response"), str) and tur.get("response").strip():
            return "response"
    if t.startswith("Your questions have been answered"):
        return "answered (plain)"
    if t.startswith("The user answered"):
        return "answered (careful)"
    if t.startswith("User has answered your questions"):
        return "answered (old form)"
    if t.startswith("The user did not answer"):
        return "not answered"
    if REJECT_FEEDBACK in t:
        return "chat about this" if "wants to clarify these questions" in t else "rejected with feedback"
    if REJECT_STOP in t:
        return "rejected (Esc)"
    if "InputValidationError" in t or "<tool_use_error>" in t:
        return "error: " + re.sub(r"\s+", " ", t)[:90]
    return ("error: " if is_error else "other: ") + re.sub(r"\s+", " ", t)[:90]


def main(argv):
    """Read every call, print the census, and write the export when asked.

    Args:
        argv: the command line (see the module's usage).
    Returns:
        0 on success, 2 when the projects folder does not exist.
    Raises:
        FileNotFoundError: when `rg` is not on PATH.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    projects = projects_dir(argv)
    if not os.path.isdir(projects):
        print("no projects folder:", projects)
        return 2
    real_only = "--real-only" in argv
    export = flag_value(argv, "--export")
    samples_n = int(flag_value(argv, "--samples", "3"))
    files = candidate_files(projects)
    calls = {}
    results = {}
    for path in files:
        try:
            cs, rs = read_calls(path)
        except OSError:
            continue
        for c in cs:
            prev = calls.get(c["id"])
            # A fork or a resume copies the call: keep the copy that has a result, else the first.
            if prev is None or (c["id"] in rs and prev["id"] not in results):
                calls[c["id"]] = c
        for k, v in rs.items():
            if k not in results:
                results[k] = v
    recs = []
    for tid, c in calls.items():
        c["probe"] = is_probe_session(c.get("cwd"))
        if real_only and c["probe"]:
            continue
        recs.append(c)
    print("transcripts read: %d; unique calls: %d (%s)" % (len(files), len(recs), "real only" if real_only else "probes included"))
    print("  in subagent transcripts / sidechain: %d; with a result: %d" % (sum(1 for c in recs if c["sidechain"]), sum(1 for c in recs if c["id"] in results)))
    versions = Counter((c.get("version") or "?") for c in recs)

    def vkey(v):
        """A version as a sortable tuple ("2.1.71" before "2.1.291", which a string sort gets wrong).

        Args:
            v: the version text.
        Returns:
            Its numeric parts; (0,) when a part is not a number.
        """
        try:
            return tuple(int(x) for x in v.split("."))
        except ValueError:
            return (0,)

    vs = sorted((v for v in versions if v != "?"), key=vkey)
    print("  Claude Code versions: %s .. %s (%d distinct; %d without)" % (vs[0] if vs else "-", vs[-1] if vs else "-", len(vs), versions.get("?", 0)))

    sample = defaultdict(list)

    def note(kind, c, detail=""):
        """Keep an example of a rare shape (at most --samples per kind).

        Args:
            kind: the shape's name.
            c: the call record.
            detail: a short extra text.
        """
        if len(sample[kind]) < samples_n:
            sample[kind].append("%s:%d %s %s" % (os.path.basename(c["file"]), c["line"], c.get("version") or "?", detail))

    qcount = Counter()
    combos = Counter()
    optcount = Counter()
    keys_input = Counter()
    keys_q = Counter()
    keys_o = Counter()
    kinds = Counter()
    header_len = []
    question_len = []
    label_len = []
    desc_len = []
    preview_len = []
    flags = Counter()
    for c in recs:
        inp = c.get("input")
        if not isinstance(inp, dict):
            flags["input not an object"] += 1
            note("input not an object", c)
            continue
        for k in inp:
            keys_input[k] += 1
        qs = inp.get("questions")
        if not isinstance(qs, list):
            flags["questions not a list"] += 1
            note("questions not a list", c, repr(qs)[:80])
            continue
        qcount[len(qs)] += 1
        ms = sum(1 for q in qs if isinstance(q, dict) and q.get("multiSelect"))
        pv = sum(1 for q in qs if isinstance(q, dict) and not q.get("multiSelect") and any(isinstance(o, dict) and o.get("preview") for o in (q.get("options") or [])))
        combos[(len(qs), ms, pv)] += 1
        texts = [q.get("question") for q in qs if isinstance(q, dict)]
        if len(texts) != len(set(texts)):
            flags["duplicate question texts in a call"] += 1
            note("duplicate question texts", c)
        heads = [q.get("header") for q in qs if isinstance(q, dict) and q.get("header")]
        if len(heads) != len(set(heads)):
            flags["duplicate headers in a call"] += 1
            note("duplicate headers", c, repr(heads))
        if isinstance(inp.get("title"), str):
            flags["extended: title"] += 1
            note("extended: title", c, inp.get("title")[:60])
        for q in qs:
            if not isinstance(q, dict):
                flags["question not an object"] += 1
                continue
            for k in q:
                keys_q[k] += 1
            kinds[q.get("kind", "(none)")] += 1
            if q.get("kind") in ("text", "number"):
                note("extended kind " + q.get("kind"), c, (q.get("question") or "")[:60])
            h = q.get("header")
            if h is None or h == "":
                flags["question without header"] += 1
                note("no header", c, (q.get("question") or "")[:60])
            else:
                header_len.append(len(h))
                if len(h) > 12:
                    flags["header over 12 chars"] += 1
                    note("header over 12 chars", c, repr(h))
            qt = q.get("question") or ""
            question_len.append(len(qt))
            if "\n" in qt:
                flags["question text with a line break"] += 1
                note("question with a line break", c, repr(qt[:80]))
            opts = q.get("options")
            if not isinstance(opts, list):
                flags["options not a list"] += 1
                optcount["(none)"] += 1
                continue
            optcount[len(opts)] += 1
            if len(opts) > 4:
                note("more than 4 options", c, str(len(opts)))
            if len(opts) < 2:
                note("fewer than 2 options", c, str(len(opts)))
            labels = []
            for o in opts:
                if not isinstance(o, dict):
                    flags["option not an object"] += 1
                    continue
                for k in o:
                    keys_o[k] += 1
                lab = o.get("label") or ""
                labels.append(lab)
                label_len.append(len(lab))
                if len(lab) > 40:
                    flags["label over 40 chars (the button cut)"] += 1
                    note("label over 40 chars", c, repr(lab[:70]))
                if ", " in lab or '"' in lab:
                    flags["label with ', ' or a quote (Tct quoting)"] += 1
                    note("label with ', ' or quote", c, repr(lab[:70]))
                if "\n" in lab:
                    flags["label with a line break"] += 1
                    note("label with a line break", c, repr(lab[:70]))
                if lab.strip() == "":
                    flags["empty label"] += 1
                    note("empty label", c)
                if lab.strip().lower() in ("other", "type something.", "chat about this", "__other__", "__chat__"):
                    flags["label that names a built-in row"] += 1
                    note("label naming a built-in row", c, repr(lab))
                d = o.get("description")
                if d is None:
                    flags["option without description"] += 1
                else:
                    desc_len.append(len(d))
                    if "\n" in d:
                        flags["description with a line break"] += 1
                        note("description with a line break", c, repr(d[:80]))
                p = o.get("preview")
                if isinstance(p, str) and p:
                    preview_len.append(len(p))
                    if q.get("multiSelect"):
                        flags["preview on a multi-select question"] += 1
                        note("preview on multi-select", c)
                    if len(p) > 2000:
                        flags["preview over 2,000 chars (withheld)"] += 1
                        note("withheld preview", c, str(len(p)))
            if len(labels) != len(set(labels)):
                flags["duplicate labels in a question"] += 1
                note("duplicate labels", c, repr(labels))
    print()
    print("questions per call:", dict(sorted(qcount.items())))
    print("(questions, multi-select, preview) per call:")
    for k, v in sorted(combos.items()):
        print("   %s: %d" % (k, v))
    print("options per question:", dict(sorted(optcount.items(), key=lambda kv: str(kv[0]))))
    print("question kinds:", dict(kinds))
    print("input keys:", dict(keys_input))
    print("question keys:", dict(keys_q))
    print("option keys:", dict(keys_o))
    print("header length:", stats(header_len))
    print("question length:", stats(question_len))
    print("label length:", stats(label_len))
    print("description length:", stats(desc_len))
    print("preview length:", stats(preview_len))
    print("flags:")
    for k, v in sorted(flags.items(), key=lambda kv: -kv[1]):
        print("   %5d  %s" % (v, k))

    # ----- the answers -----
    forms = Counter()
    tur_keys = Counter()
    akinds = Counter()
    partial = Counter()
    errors = Counter()
    for c in recs:
        r = results.get(c["id"])
        if r is None:
            forms["(no result: open, cut off)"] += 1
            continue
        text, is_error, tur, _line = r
        form = result_form(text, is_error, tur)
        forms[form if not form.startswith(("error", "other")) else form.split(":")[0]] += 1
        if form.startswith(("error", "other")):
            errors[form] += 1
            note(form[:40], c)
        if form in ("response", "followUp", "afk auto-continue"):
            note("result " + form, c, (text or "")[:80])
        if not isinstance(tur, dict):
            continue
        for k in tur:
            tur_keys[k] += 1
        answers = tur.get("answers")
        if not isinstance(answers, dict):
            continue
        qs = (c.get("input") or {}).get("questions") or []
        n_answered = 0
        for q in qs:
            if not isinstance(q, dict):
                continue
            a = answers.get(q.get("question"))
            k = answer_kind(q, a)
            akinds[("multi " if q.get("multiSelect") else "single ") + k] += 1
            if k == "array":
                note("array answer", c, repr(a)[:80])
            if k == "image":
                note("image answer", c, repr(a)[:80])
            if k != "unanswered":
                n_answered += 1
        if len(qs) > 1:
            partial["%d of %d answered" % (n_answered, len(qs))] += 1
    print()
    print("result forms:")
    for k, v in sorted(forms.items(), key=lambda kv: -kv[1]):
        print("   %5d  %s" % (v, k))
    print("toolUseResult keys:", dict(tur_keys))
    print("answer kinds:")
    for k, v in sorted(akinds.items(), key=lambda kv: -kv[1]):
        print("   %5d  %s" % (v, k))
    print("multi-question calls, answered questions:", dict(sorted(partial.items())))
    if errors:
        print("error / other results:")
        for k, v in errors.most_common(25):
            print("   %5d  %s" % (v, k))
    print()
    print("samples:")
    for k in sorted(sample):
        print("  [%s]" % k)
        for s in sample[k]:
            print("     ", s)

    if export:
        with open(export, "w", encoding="utf-8", newline="\n") as f:
            for c in recs:
                r = results.get(c["id"])
                f.write(json.dumps({"id": c["id"], "file": c["file"], "line": c["line"], "version": c.get("version"), "sidechain": c["sidechain"],
                                    "probe": c["probe"], "input": c.get("input"), "result": r[0] if r else None, "isError": r[1] if r else None,
                                    "toolUseResult": r[2] if r else None}, ensure_ascii=False) + "\n")
        print("\nexported %d calls to %s" % (len(recs), export))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
