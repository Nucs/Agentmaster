"""Review the AI search's CONVERSATION LOG (SESSIONS.md 9.17): the JSON lines Agentmaster writes in DEBUG MODE (the dev
install always; a release with Settings > About > Enable Debug Mode) to <profile>\\harness\\logs\\<conversation>-<date>.jsonl,
one file per conversation per local day: every request of the AI search (the Sessions page's and the side panel's
"sessions-search", the Telegram Search topic's "telegram-search"), the model's plan, every tool call with its WHOLE result,
the sessions shown with their whys, the reply, and each request's time, tokens and cost.

    python -I review_log.py [<file or folder> ...] [--conv NAME] [--since YYYY-MM-DD] [--last N]
                            [--full] [--summary] [--eval-json OUT.json]

With no path it reads the default profiles' log folders that exist: %USERPROFILE%\\.agentmaster-dev\\harness\\logs (dev),
then %USERPROFILE%\\.agentmaster\\harness\\logs (release). A profile moved elsewhere (Settings > About > PROFILE) is given
as a path.

  (default)     a review of each request: when, how long, the request, the plan, every tool call (its arguments, its own
                duration, its result's size and counts), the sessions shown with their whys, the reply, tokens and cost.
  --full        the same, with every tool result WHOLE (the text the model read).
  --summary     one line per request, then the totals: how requests ended, median / p90 time, the tool calls per kind with
                their median durations, why coverage, the cost.
  --eval-json   write the requests in harness_live.exe --eval's results format, so score.py can score REAL requests
                against an answer key: `python -I score.py OUT.json --keys KEYS.json --calls --miss`. Each request's id
                is "<date>-<conversation>-p<pid>e<epoch>t<turn>": use those ids as the keys' query ids (README, "Live
                requests").
  --conv NAME   only that conversation (sessions-search, telegram-search).
  --since DATE  only requests asked on or after that local date.
  --last N      only the newest N requests (after the other filters).

The lines are split on "\\n" only (never str.splitlines: the writer leaves U+2028 unescaped inside strings). A line that does
not parse is counted and skipped. Lines of one process are told apart from another's by the "start" line each process writes
first (its pid); a file of a later day continues the process that wrote before it.
"""
import json
import os
import pathlib
import statistics
import sys

sys.stdout.reconfigure(encoding="utf-8")

# The kinds that belong to one request (the rest describe the conversation: start, process, compacted, failure, reset,
# thinking; they are shown in the review of the request they fall into when they carry its turn).
REQUEST_KINDS = {"ask", "narration", "call", "result", "show", "end"}


def parse_args(argv):
    """Read the command line.

    Args:
        argv: the arguments after the script's name.

    Returns:
        A dict: paths (list of str), conv (str or None), since (str or None), last (int or None), full, summary (bools),
        eval_json (str or None).

    Raises:
        SystemExit: on an unknown option, a missing value, or a --last that is not a positive number.
    """
    opts = {"paths": [], "conv": None, "since": None, "last": None, "full": False, "summary": False, "eval_json": None}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--conv", "--since", "--last", "--eval-json"):
            if i + 1 >= len(argv):
                raise SystemExit("%s needs a value" % a)
            value = argv[i + 1]
            i += 2
            if a == "--last":
                if not value.isdigit() or int(value) < 1:
                    raise SystemExit("--last needs a positive number")
                opts["last"] = int(value)
            else:
                opts[a[2:].replace("-", "_")] = value
            continue
        if a in ("--full", "--summary"):
            opts[a[2:]] = True
        elif a.startswith("--"):
            raise SystemExit("unknown option %s (see the docstring at the top of review_log.py)" % a)
        else:
            opts["paths"].append(a)
        i += 1
    return opts


def default_folders():
    """The default profiles' log folders that exist: the dev profile's, then the release profile's.

    Returns:
        A list of pathlib.Path (possibly empty).
    """
    home = pathlib.Path(os.environ.get("USERPROFILE") or pathlib.Path.home())
    found = []
    for profile in (".agentmaster-dev", ".agentmaster"):
        folder = home / profile / "harness" / "logs"
        if folder.is_dir():
            found.append(folder)
    return found


def log_files(paths):
    """Every log file named by `paths` (a file as it is, a folder's *.jsonl), grouped by folder, each group in name order
    (which is date order for one conversation: "<conversation>-<yyyy-MM-dd>.jsonl").

    Args:
        paths: files and folders (str or Path).

    Returns:
        A list of pathlib.Path.

    Raises:
        SystemExit: when a path does not exist.
    """
    files = []
    for p in paths:
        path = pathlib.Path(p)
        if path.is_dir():
            files.extend(sorted(path.glob("*.jsonl")))
        elif path.is_file():
            files.append(path)
        else:
            raise SystemExit("not found: %s" % p)
    return files


def read_lines(files):
    """Every parsed line of `files`, each with the request key it belongs to.

    The key is (folder, conversation, pid, epoch, turn): the pid comes from the newest "start" line of that folder and
    conversation seen so far (the files are read in order), so two processes of one day never merge their requests.

    Args:
        files: the log files, in the order log_files() returns them.

    Returns:
        (lines, bad): lines is a list of (key, line dict); bad counts the lines that did not parse.
    """
    out = []
    bad = 0
    pid_of = {}  # (folder, conv) -> the pid of its newest start line
    for f in files:
        for raw in f.read_bytes().split(b"\n"):
            if not raw.strip():
                continue
            try:
                line = json.loads(raw.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                bad += 1
                continue
            if not isinstance(line, dict):
                bad += 1
                continue
            stream = (str(f.parent), line.get("conv", ""))
            if line.get("kind") == "start":
                pid_of[stream] = line.get("pid", 0)
            key = stream + (pid_of.get(stream, 0), line.get("epoch", 0), line.get("turn", 0))
            out.append((key, line))
    return out, bad


def group_requests(lines):
    """Fold the lines into requests, in the order their first line appears.

    Args:
        lines: read_lines()'s list of (key, line).

    Returns:
        A list of request dicts: key, ask (the ask line or None), lines (every line of the request, in order), calls (a list
        of {"call": line, "result": line or None} in seq order), shows (show lines), end (the end line or None).
    """
    by_key = {}
    order = []
    for key, line in lines:
        kind = line.get("kind")
        if kind not in REQUEST_KINDS and not (kind in ("failure", "compacted") and key[4] != 0):
            continue
        request = by_key.get(key)
        if request is None:
            request = {"key": key, "ask": None, "lines": [], "calls": {}, "shows": [], "end": None}
            by_key[key] = request
            order.append(request)
        request["lines"].append(line)
        if kind == "ask":
            request["ask"] = line
        elif kind == "call":
            request["calls"].setdefault(line.get("seq", 0), {"call": None, "result": None})["call"] = line
        elif kind == "result":
            # seq 0 = a result whose call the log never saw: keep each such result apart (negative keys never collide).
            seq = line.get("seq", 0) or -(len(request["calls"]) + 1)
            request["calls"].setdefault(seq, {"call": None, "result": None})["result"] = line
        elif kind == "show":
            request["shows"].append(line)
        elif kind == "end":
            request["end"] = line
    for request in order:
        request["calls"] = [request["calls"][k] for k in sorted(request["calls"], key=lambda s: (s < 0, abs(s)))]
    return order


def asked_date(request):
    """The request's local date ("yyyy-MM-dd"): its ask's, else its first line's.

    Args:
        request: a request dict.

    Returns:
        The date text ("" when no line has a stamp).
    """
    first = request["ask"] or (request["lines"][0] if request["lines"] else {})
    return str(first.get("at", ""))[:10]


def counts_of(result_text):
    """A search / grep result's counts in one short line, like score.py's (a copy: score.py runs on import).

    Args:
        result_text: a tool result's text (JSON for search and grep, Markdown for read, plain text for an error).

    Returns:
        "window=... searched=... read=... matched=... returned=..." plus "stopped!" / "indexing!" / "hint!" /
        "too_complex!" when present; the first 100 characters of a text that is not a JSON object.
    """
    try:
        o = json.loads(result_text)
    except ValueError:
        return result_text[:100].replace("\n", " ")
    if not isinstance(o, dict):
        return ""
    parts = ["%s=%s" % (k, o[k]) for k in ("window", "searched", "read", "matched", "returned") if k in o]
    parts += ["%s!" % k for k in ("stopped", "indexing", "hint", "too_complex") if k in o]
    return " ".join(parts)


def short_tool(name):
    """A tool's name without its server prefix, for a compact column.

    Args:
        name: the tool's full name as the log records it ("mcp__sessions__search").

    Returns:
        The last part ("search"); "?" for an empty name.
    """
    return name.rsplit("__", 1)[-1] if name else "?"


def seconds(ms):
    """A duration for a person: milliseconds as seconds with one decimal.

    Args:
        ms: the duration in milliseconds, or None when the log did not know it (a call whose start was not seen).

    Returns:
        "6.4 s", or "?" for None.
    """
    return "?" if ms is None else "%.1f s" % (ms / 1000.0)


def status_of(request):
    """How a request ended.

    Args:
        request: a request dict (group_requests).

    Returns:
        Its end line's status (answered, failed, interrupted, reset), or "OPEN" when it has no end line: the app ended
        mid-request, or the request still runs while the file is read.
    """
    return request["end"].get("status", "?") if request["end"] else "OPEN"


def review(request, full):
    """Print one request as a readable review.

    Args:
        request: a request dict.
        full: True to print every tool result whole.
    """
    folder, conv, pid, epoch, turn = request["key"]
    ask = request["ask"] or {}
    end = request["end"] or {}
    usage = end.get("usage", {})
    print("=" * 110)
    print("%s  %s  pid %s epoch %s turn %s  %s  in %s (turn %s)  thinking %s  $%.3f" % (
        ask.get("at", "?")[:19], conv, pid, epoch, turn, status_of(request).upper(), seconds(end.get("elapsed_ms")),
        seconds(end.get("turn_ms")), ask.get("thinking", "?"), usage.get("cost_usd", 0.0)))
    print("  > " + ask.get("text", "(the ask line is not in these files)").replace("\n", "\n    "))
    for line in request["lines"]:
        kind = line.get("kind")
        if kind == "narration":
            print("  plan +%s: %s" % (seconds(line.get("at_ms")), line.get("text", "").replace("\n", " | ")))
        elif kind in ("failure", "compacted"):
            print("  %s +%s: %s" % (kind, seconds(line.get("at_ms")), line.get("text") or "%s -> %s tokens" % (line.get("pre"), line.get("post"))))
    for pair in request["calls"]:
        call, result = pair["call"] or {}, pair["result"] or {}
        tool = short_tool(call.get("tool") or result.get("tool", ""))
        print("  #%-2s +%-7s %-10s %s" % (call.get("seq", "?"), seconds(call.get("at_ms")), tool, call.get("arguments", "")[:160]))
        if result:
            text = result.get("result", "")
            print("       -> %s%s, %s chars  %s" % ("ERROR " if result.get("error") else "", seconds(result.get("ms")), "{:,}".format(result.get("chars", len(text))), counts_of(text)))
            if full:
                print("          " + text.replace("\n", "\n          "))
        else:
            print("       -> NO RESULT (the call never returned in these files)")
    for show in request["shows"]:
        ids, whys = show.get("ids", []), show.get("whys", [])
        print("  show +%s: %d session(s), %d with a why  %s" % (seconds(show.get("at_ms")), len(ids), sum(1 for w in whys if w), show.get("note", "")))
        for i, sid in enumerate(ids):
            print("     %s  %s" % (sid[:8], (whys[i] if i < len(whys) else "")[:150]))
    if request["end"]:
        print("  answer: " + end.get("answer", "").replace("\n", "\n          "))
        print("  usage: in %s out %s cache_read %s cache_write %s  ctx %s / %s  thinking %s block(s), ~%s token(s)" % (
            usage.get("input", "?"), usage.get("output", "?"), usage.get("cache_read", "?"), usage.get("cache_write", "?"),
            usage.get("context", "?"), usage.get("window", "?"), end.get("thinking_blocks", 0), end.get("thinking_tokens", 0)))


def summarize(requests):
    """Print one line per request, then the totals.

    Args:
        requests: the request dicts to summarize.
    """
    elapsed, cost, durations, why_shown, shown_total, statuses = [], 0.0, {}, 0, 0, {}
    for r in requests:
        ask, end = r["ask"] or {}, r["end"] or {}
        tools = {}
        for pair in r["calls"]:
            name = short_tool((pair["call"] or pair["result"] or {}).get("tool", ""))
            tools[name] = tools.get(name, 0) + 1
            if pair["result"] and pair["result"].get("ms") is not None:
                durations.setdefault(name, []).append(pair["result"]["ms"])
        last_show = r["shows"][-1] if r["shows"] else {}
        ids, whys = last_show.get("ids", []), last_show.get("whys", [])
        shown_total += len(ids)
        why_shown += sum(1 for w in whys if w)
        status = status_of(r)
        statuses[status] = statuses.get(status, 0) + 1
        if end.get("elapsed_ms") is not None:
            elapsed.append(end["elapsed_ms"])
        cost += end.get("usage", {}).get("cost_usd", 0.0)
        print("%-19s %-15s e%st%-3s %-11s %7s  calls=%-2d (%s)  shown %d (%d why)  $%.3f  %s" % (
            ask.get("at", "?")[:19], r["key"][1], r["key"][3], r["key"][4], status, seconds(end.get("elapsed_ms")), len(r["calls"]),
            " ".join("%s %d" % kv for kv in sorted(tools.items())), len(ids), sum(1 for w in whys if w),
            end.get("usage", {}).get("cost_usd", 0.0), ask.get("text", "")[:70].replace("\n", " ")))
    print("-" * 110)
    print("%d request(s): %s" % (len(requests), ", ".join("%s %d" % kv for kv in sorted(statuses.items()))))
    if elapsed:
        p90 = sorted(elapsed)[min(len(elapsed) - 1, int(round(0.9 * (len(elapsed) - 1))))]
        print("time: median %s, p90 %s, longest %s" % (seconds(statistics.median(elapsed)), seconds(p90), seconds(max(elapsed))))
    for name, ms in sorted(durations.items()):
        print("  %-10s %4d call(s), median %s, longest %s" % (name, len(ms), seconds(statistics.median(ms)), seconds(max(ms))))
    if shown_total:
        print("whys: %d of %d shown sessions (%.0f%%)" % (why_shown, shown_total, 100.0 * why_shown / shown_total))
    print("cost: $%.3f" % cost)


def eval_record(request):
    """One request in harness_live.exe --eval's results format (what score.py reads).

    Args:
        request: a request dict.

    Returns:
        The record dict: id, text, answered, elapsed_ms, shown_at_ms, show_calls, shown [{id, why}], note, answer,
        narration, usage {input, output, cache_read, cache_write, context, cost_usd}, calls [{at_ms, tool, arguments,
        result, error}].
    """
    folder, conv, pid, epoch, turn = request["key"]
    ask, end = request["ask"] or {}, request["end"] or {}
    last_show = request["shows"][-1] if request["shows"] else {}
    ids, whys = last_show.get("ids", []), last_show.get("whys", [])
    usage = end.get("usage", {})
    calls = []
    for pair in request["calls"]:
        call, result = pair["call"] or {}, pair["result"] or {}
        calls.append({"at_ms": result.get("at_ms", call.get("at_ms", 0)), "tool": call.get("tool") or result.get("tool", ""),
                      "arguments": call.get("arguments", ""), "result": result.get("result", ""), "error": bool(result.get("error"))})
    return {
        "id": "%s-%s-p%se%st%s" % (asked_date(request), conv, pid, epoch, turn),
        "text": ask.get("text", ""),
        "answered": status_of(request) == "answered",
        "elapsed_ms": end.get("elapsed_ms", 0),
        "shown_at_ms": last_show.get("at_ms", 0),
        "show_calls": len(request["shows"]),
        "shown": [{"id": sid, "why": whys[i] if i < len(whys) else ""} for i, sid in enumerate(ids)],
        "note": last_show.get("note", ""),
        "answer": end.get("answer", ""),
        "narration": "\n".join(l.get("text", "") for l in request["lines"] if l.get("kind") == "narration"),
        "usage": {"input": usage.get("input", 0), "output": usage.get("output", 0), "cache_read": usage.get("cache_read", 0),
                  "cache_write": usage.get("cache_write", 0), "context": usage.get("context", 0), "cost_usd": usage.get("cost_usd", 0.0)},
        "calls": calls,
    }


def main(argv):
    """Read, filter and print (or export) the requests of the logs named on the command line.

    Args:
        argv: the arguments after the script's name.

    Returns:
        0 when at least one request was found, 1 otherwise.

    Raises:
        SystemExit: on a bad command line or a path that does not exist.
        OSError: when a log file or the --eval-json output cannot be read / written.
    """
    opts = parse_args(argv)
    paths = opts["paths"] or default_folders()
    if not paths:
        print("no log folder: debug mode writes <profile>\\harness\\logs (give a path)")
        return 1
    lines, bad = read_lines(log_files(paths))
    requests = group_requests(lines)
    if opts["conv"]:
        requests = [r for r in requests if r["key"][1] == opts["conv"]]
    if opts["since"]:
        requests = [r for r in requests if asked_date(r) >= opts["since"]]
    if opts["last"]:
        requests = requests[-opts["last"]:]
    if bad:
        print("(%d line(s) did not parse and were skipped)" % bad)
    if not requests:
        print("no request in %s" % ", ".join(str(p) for p in paths))
        return 1
    if opts["eval_json"]:
        records = [eval_record(r) for r in requests]
        with open(opts["eval_json"], "w", encoding="utf-8", newline="\n") as f:
            json.dump({"prompt": "(live requests)", "thinking": "(per request)", "results": records}, f, ensure_ascii=False, indent=1)
        print("wrote %d request(s) to %s" % (len(records), opts["eval_json"]))
    elif opts["summary"]:
        summarize(requests)
    else:
        for r in requests:
            review(r, opts["full"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
