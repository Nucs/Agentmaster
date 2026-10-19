"""Read-only survey of Claude Code's interactive tools across every transcript on this machine.

For each AskUserQuestion / ExitPlanMode / EnterPlanMode / ProposeGoal `tool_use` it finds the matching
`tool_result` (by `tool_use_id`) and reports what a remote-answer feature has to handle:

- how many calls, in how many transcripts, and the newest one;
- how long each call waited for the user (tool_use -> tool_result timestamps);
- how each ended (answered / with notes / "Chat about this" / Esc / idle auto-continue; a plan approved in
  place, approved through the "clear context" restart, rejected with feedback, rejected);
- the sizes that bound a Telegram message (questions per call, options, previews, plan length).

A plan approved with "clear context" is recorded by Claude Code as a REJECTION of the call, followed by a
fresh conversation whose first message names the old transcript ("read the full transcript at: <id>.jsonl").
The survey finds those successors with ripgrep, so the two cases are counted apart.

Nothing is written anywhere but stdout. Results of the first run (2026-10-19): doc/agentmaster/INTERACTIVE_TOOLS.md §2.

Usage:
    python -I survey_interactive.py [<claude-projects-dir>]
    (default: %CLAUDE_CONFIG_DIR%\\projects, else ~/.claude/projects; needs `rg` on PATH)
"""
import json
import os
import statistics
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import datetime

TOOLS = ("AskUserQuestion", "ExitPlanMode", "EnterPlanMode", "ProposeGoal")

# The rejection prefixes Claude Code 2.1.291 writes (cancelAndAbort's TM / ik texts).
REJECT_FEEDBACK = "To tell you how to proceed, the user said:"
REJECT_STOP = "doesn't want to proceed"


def parse_ts(s):
    """Parse a transcript timestamp to epoch seconds.

    Args:
        s: an ISO-8601 string such as "2026-10-18T12:34:56.789Z", or None.
    Returns:
        The epoch seconds as a float, or None when the value is missing or malformed (the caller then leaves
        that call out of the wait statistics rather than guessing).
    """
    if not s:
        return None
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def result_text(content):
    """Flatten a tool_result's content to its text.

    Args:
        content: a string, or a list of content blocks (only "text" blocks carry text).
    Returns:
        The text, blocks joined by newlines; "" for anything else.
    """
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "\n".join(b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text")
    return ""


def classify_question(text, tur, is_error):
    """Name the way one AskUserQuestion call ended.

    Args:
        text: the tool_result's text (what the model read).
        tur: the line's `toolUseResult` (the structured answer) or None.
        is_error: the tool_result's is_error flag.
    Returns:
        A short outcome name. The structured answer is read first because the text varies by case; the
        rejection texts are matched by Claude Code's own fixed prefixes.
    """
    if isinstance(tur, dict):
        if tur.get("afkTimeoutMs"):
            return "auto-continued while idle"
        if tur.get("followUp"):
            return "asked for more questions"
        if tur.get("response"):
            return "replied instead (response)"
        if tur.get("answers"):
            return "answered + notes/preview" if tur.get("annotations") else "answered"
    if REJECT_FEEDBACK in (text or ""):
        # "Chat about this" is a rejection whose feedback is a canned "The user wants to clarify ..." text.
        return "chat about this" if "wants to clarify these questions" in text else "rejected with feedback"
    if REJECT_STOP in (text or ""):
        return "rejected (Esc)"
    if "did not answer" in (text or ""):
        return "not answered"
    return ("error: " if is_error else "other: ") + (text or "")[:60].replace("\n", " ")


def classify_plan(text, tur, is_error):
    """Name the way one ExitPlanMode call ended (before the clear-context correction).

    Args:
        text: the tool_result's text.
        tur: the line's `toolUseResult` or None.
        is_error: the tool_result's is_error flag.
    Returns:
        A short outcome name. "rejected (stop)" is later split into a real rejection and a clear-context
        approval by `has_restart_successor`.
    """
    if isinstance(tur, dict) and not is_error:
        if tur.get("awaitingLeaderApproval"):
            return "sent to the team lead"
        if tur.get("planWasEdited"):
            return "approved in place (edited)"
        return "approved in place" if tur.get("plan") else "approved in place (no plan)"
    if REJECT_FEEDBACK in (text or ""):
        return "keep planning (feedback)"
    if REJECT_STOP in (text or ""):
        return "rejected (stop)"
    if "not in plan mode" in (text or ""):
        return "error: not in plan mode"
    return ("error: " if is_error else "other: ") + (text or "")[:60].replace("\n", " ")


def pct(values, p):
    """The p-th percentile of a list by nearest rank (no interpolation, so it is always a real sample).

    Args:
        values: a non-empty list of numbers.
        p: the percentile, 0..100.
    Returns:
        The sample at rank round(p/100 * (n-1)) of the sorted list.
    Raises:
        IndexError when `values` is empty (the callers only ask for non-empty lists).
    """
    v = sorted(values)
    return v[max(0, min(len(v) - 1, int(round(p / 100 * (len(v) - 1)))))]


def fmt_secs(s):
    """A duration as a short human string, so a report line stays readable from seconds to days.

    Args:
        s: the duration in seconds.
    Returns:
        "<n>s" under 90 s, "<n.n>min" under 90 min, else "<n.n>h".
    """
    if s < 90:
        return f"{s:.0f}s"
    if s < 5400:
        return f"{s / 60:.1f}min"
    return f"{s / 3600:.1f}h"


def candidate_files(projects):
    """List the transcripts that call one of the tools, with ripgrep.

    Args:
        projects: the Claude Code projects folder.
    Returns:
        Paths of top-level session transcripts (subagent side files are left out: their dialogs are the
        parent's, and the parent records the call).
    Raises:
        FileNotFoundError when `rg` is not on PATH (the survey would otherwise read ~11 GB line by line).
    """
    args = ["rg", "-l", "--no-messages", "-g", "*.jsonl"]
    for t in TOOLS:
        args += ["-e", f'"name":"{t}"']
    out = subprocess.run(args + [projects], capture_output=True, text=True, encoding="utf-8", errors="replace")
    files = [l.strip() for l in out.stdout.splitlines() if l.strip()]
    return [f for f in files if "subagents" not in f.replace("\\", "/").split("/")]


def has_restart_successor(projects, stems):
    """Find which sessions were continued by a clear-context plan restart.

    Args:
        projects: the Claude Code projects folder.
        stems: session ids (transcript file stems) whose plan call ended in a plain rejection.
    Returns:
        The subset of `stems` that some transcript names in "read the full transcript at: <stem>.jsonl" —
        Claude Code's own wording for the first message of the fresh conversation.
    """
    if not stems:
        return set()
    found = set()
    stems = sorted(stems)
    # Batches keep the regex (and the command line) short.
    for i in range(0, len(stems), 40):
        batch = stems[i:i + 40]
        pattern = r"read the full transcript at:[^\"]*(" + "|".join(batch) + r")\.jsonl"
        out = subprocess.run(["rg", "-o", "--no-filename", "--no-messages", "-g", "*.jsonl", "-e", pattern, projects],
                             capture_output=True, text=True, encoding="utf-8", errors="replace")
        for line in out.stdout.splitlines():
            for s in batch:
                if s in line:
                    found.add(s)
    return found


def main():
    """Run the survey and print the report.

    Returns:
        0 on success; 1 when the projects folder does not exist.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) > 1:
        projects = sys.argv[1]
    else:
        base = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(os.path.expanduser("~"), ".claude")
        projects = os.path.join(base, "projects")
    if not os.path.isdir(projects):
        print(f"no projects folder at {projects}")
        return 1
    files = candidate_files(projects)
    uses = {}
    results = {}
    for path in files:
        try:
            fh = open(path, "rb")
        except OSError:
            continue
        with fh:
            for raw in fh:
                # Cheap byte test first: the corpus is large and most lines are neither.
                if b"tool_use" not in raw and b"tool_result" not in raw:
                    continue
                try:
                    o = json.loads(raw)
                except (ValueError, UnicodeDecodeError):
                    continue
                content = (o.get("message") or {}).get("content")
                if not isinstance(content, list):
                    continue
                ts = parse_ts(o.get("timestamp"))
                for b in content:
                    if not isinstance(b, dict):
                        continue
                    if o.get("type") == "assistant" and b.get("type") == "tool_use" and b.get("name") in TOOLS:
                        uses.setdefault(b.get("id"), {"name": b.get("name"), "ts": ts, "input": b.get("input") or {},
                                                      "stem": os.path.splitext(os.path.basename(path))[0]})
                    elif o.get("type") == "user" and b.get("type") == "tool_result" and b.get("tool_use_id") not in results:
                        results[b.get("tool_use_id")] = (ts, result_text(b.get("content")), o.get("toolUseResult"),
                                                         bool(b.get("is_error")))
    by_tool = defaultdict(list)
    for tid, u in uses.items():
        by_tool[u["name"]].append((tid, u))
    # The clear-context correction: a plain rejection of a plan whose session has a restart successor.
    stop_stems = {u["stem"] for tid, u in by_tool.get("ExitPlanMode", [])
                  if tid in results and classify_plan(results[tid][1], results[tid][2], results[tid][3]) == "rejected (stop)"}
    restarted = has_restart_successor(projects, stop_stems)
    for name in TOOLS:
        recs = by_tool.get(name, [])
        print(f"\n===== {name}: {len(recs)} calls in {len({u['stem'] for _, u in recs})} transcripts")
        if not recs:
            continue
        waits, outcomes, unmatched, newest = [], Counter(), 0, 0
        qcount, optcount, multi, previews = Counter(), Counter(), 0, []
        qlen, hlen, llen, plan_len = [], [], [], []
        for tid, u in recs:
            inp = u["input"]
            newest = max(newest, u["ts"] or 0)
            if name == "AskUserQuestion":
                qs = inp.get("questions") or []
                qcount[len(qs)] += 1
                for q in qs:
                    if not isinstance(q, dict):
                        continue
                    opts = q.get("options") or []
                    optcount[len(opts)] += 1
                    multi += 1 if q.get("multiSelect") else 0
                    qlen.append(len(q.get("question") or ""))
                    hlen.append(len(q.get("header") or ""))
                    for op in opts:
                        if isinstance(op, dict):
                            llen.append(len(op.get("label") or ""))
                            if op.get("preview"):
                                previews.append(len(op["preview"]))
            if name == "ExitPlanMode" and isinstance(inp.get("plan"), str) and inp["plan"]:
                # Claude Code writes the plan (read from the plan file) into the call's input.
                plan_len.append(len(inp["plan"]))
            r = results.get(tid)
            if r is None:
                unmatched += 1
                continue
            rts, text, tur, is_error = r
            if u["ts"] and rts and rts >= u["ts"]:
                waits.append(rts - u["ts"])
            if name == "AskUserQuestion":
                outcomes[classify_question(text, tur, is_error)] += 1
            elif name == "ExitPlanMode":
                kind = classify_plan(text, tur, is_error)
                if kind == "rejected (stop)" and u["stem"] in restarted:
                    kind = "approved with clear context (restart)"
                outcomes[kind] += 1
            else:
                outcomes[("error: " if is_error else "ok: ") + (text or "")[:50].replace("\n", " ")] += 1
        print(f"  newest call: {datetime.fromtimestamp(newest).isoformat(timespec='minutes') if newest else '?'}")
        print(f"  no result found (session ended, still open, or written elsewhere): {unmatched}")
        if waits:
            print(f"  wait: median {fmt_secs(statistics.median(waits))}, p75 {fmt_secs(pct(waits, 75))}, "
                  f"p90 {fmt_secs(pct(waits, 90))}, max {fmt_secs(max(waits))}; >1 min {sum(w > 60 for w in waits)}, "
                  f">5 min {sum(w > 300 for w in waits)}, >30 min {sum(w > 1800 for w in waits)}, "
                  f">2 h {sum(w > 7200 for w in waits)} of {len(waits)}")
        print("  outcomes:")
        for k, v in outcomes.most_common(16):
            print(f"    {v:5d}  {k}")
        if name == "AskUserQuestion":
            print(f"  questions per call: {dict(sorted(qcount.items()))}")
            print(f"  options per question: {dict(sorted(optcount.items()))}; multi-select questions: {multi}")
            if qlen:
                print(f"  question chars: median {statistics.median(qlen):.0f}, max {max(qlen)}; header max {max(hlen)}; "
                      f"label max {max(llen) if llen else 0}")
            if previews:
                print(f"  options with a preview: {len(previews)} (chars median {statistics.median(previews):.0f}, max {max(previews)})")
        if plan_len:
            print(f"  plan chars (every call with a plan): {len(plan_len)} plans, median {statistics.median(plan_len):.0f}, "
                  f"p90 {pct(plan_len, 90)}, max {max(plan_len)}; over 4,096 (one Telegram message) "
                  f"{sum(p > 4096 for p in plan_len)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
