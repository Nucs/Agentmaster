"""Score one round of the AI search's prompt eval (SESSIONS.md 9.15): harness_live.exe --eval's results against an
answer-key file.

    python -I score.py <results.json> --keys <keys.json> [--calls] [--miss]

Per query it prints recall over `must` (the "never miss" number), precision over must + optional, how many `must`
sessions the tools NAMED in any result ("seen") against how many were SHOWN (a seen-but-not-shown miss is the model's
choice, a never-seen one is the search's), the time, the tokens, and (with --calls) every tool call with its result's
counts; --miss lists each missed session (seen or never) and each wrongly shown one with its why. The last line is
the mean recall of the round.

The keys name real sessions of one machine, so they never go into the repository (.gitignore): keep them beside the
results, outside the tree. Their shape:

    {"titles": {"<whole session id>": {"title": "...", "project": "K--source-x", "last": "2026-10-12 ..."}},
     "queries": {"q1": {"must": ["<whole id>", ...], "optional": ["<whole id>", ...]}}}

`titles` holds every id the keys use (it also resolves the 8-character ids the tools print). A query of the results
that the keys do not have raises KeyError; a missing or unreadable file raises OSError / json.JSONDecodeError.
"""
import json
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")
if len(sys.argv) < 2 or "--keys" not in sys.argv or sys.argv.index("--keys") + 1 >= len(sys.argv):
    raise SystemExit("usage: python -I score.py <results.json> --keys <keys.json> [--calls] [--miss]")
keys = json.load(open(sys.argv[sys.argv.index("--keys") + 1], encoding="utf-8"))
res = json.load(open(sys.argv[1], encoding="utf-8"))
show_calls = "--calls" in sys.argv
show_miss = "--miss" in sys.argv
titles = keys["titles"]
# Every id a tool result names: a search / grep row's "id", and a read result's "## Session <id>" heading.
ID_RX = re.compile(r'"id":"([0-9a-f]{8,36})"|## Session ([0-9a-f]{8,36})')


def resolve(handle, universe):
    """The whole id a short id names among the keys' ids, so a tool's 8-character id counts as the session it is.

    Args:
        handle: an id as a tool printed it (8 to 36 hex characters and dashes).
        universe: the keys' whole ids.

    Returns:
        The one whole id that starts with `handle`, or `handle` itself when none or several do (an ambiguous short id
        then matches no key, the cautious side: it never makes a miss look like a hit).
    """
    found = [u for u in universe if u.startswith(handle)]
    return found[0] if len(found) == 1 else handle


def counts_of(result_text):
    """The counts of a search or grep result in one short line, for reading a round's calls at a glance.

    Args:
        result_text: a tool result's text as the round recorded it (JSON for search / grep, Markdown for read).

    Returns:
        "window=... searched=... read=... matched=... returned=..." plus "stopped!" / "indexing!" / "hint!" /
        "too_complex!" when the result has them; the first 120 characters of a result that is not JSON; "" for JSON
        that is not an object. Never raises for a text it cannot parse.
    """
    try:
        o = json.loads(result_text)
    except Exception:
        return result_text[:120].replace("\n", " ")
    if not isinstance(o, dict):
        return ""
    parts = []
    for k in ("window", "searched", "read", "matched", "returned"):
        if k in o:
            parts.append("%s=%s" % (k, o[k]))
    for k in ("stopped", "indexing", "hint", "too_complex"):
        if k in o:
            parts.append("%s!" % k)
    return " ".join(parts)


total_recall = []
for r in res["results"]:
    qid = r["id"]
    key = keys["queries"][qid]
    must = set(key["must"])
    ok = must | set(key["optional"])
    universe = list(titles.keys())
    shown = [s["id"] for s in r["shown"]]
    whys = {s["id"]: s["why"] for s in r["shown"]}
    seen = set()
    for c in r["calls"]:
        for m in ID_RX.finditer(c["result"]):
            h = m.group(1) or m.group(2)
            seen.add(resolve(h, universe))
    hit = must & set(shown)
    seen_must = must & seen
    recall = len(hit) / len(must) if must else 1.0
    precision = len([s for s in shown if s in ok]) / len(shown) if shown else 0.0
    total_recall.append(recall)
    u = r["usage"]
    print("=" * 110)
    print("%s  %s" % (qid, r["text"]))
    print("  recall %.0f%% (%d/%d)  seen-by-tools %d/%d  precision %.0f%% (%d shown, %d wrong)  %s  %.0f s  calls=%d  in=%d out=%d cache_read=%d  $%.3f" % (
        recall * 100, len(hit), len(must), len(seen_must), len(must), precision * 100, len(shown), len([s for s in shown if s not in ok]),
        "answered" if r["answered"] else "NOT ANSWERED", r["elapsed_ms"] / 1000, len(r["calls"]), u["input"], u["output"], u["cache_read"], u["cost_usd"]))
    with_why = sum(1 for s in shown if whys.get(s))
    print("  whys: %d of %d shown" % (with_why, len(shown)))
    missed = sorted(must - set(shown))
    if missed:
        never = [m for m in missed if m not in seen]
        print("  MISSED %d: %d never named by a tool, %d named but not shown" % (len(missed), len(never), len(missed) - len(never)))
        if show_miss:
            for m in missed:
                t = titles.get(m, {})
                print("     %s %s %-28s %s %s" % ("never" if m not in seen else "SEEN ", m[:8], t.get("project", "")[:28], t.get("last", "")[:10], t.get("title", "")[:70]))
    wrong = [s for s in shown if s not in ok]
    if wrong and show_miss:
        print("  WRONG shown:")
        for s in wrong:
            print("     %s  why=%s" % (s[:8], whys.get(s, "")[:100]))
    if show_calls:
        for c in r["calls"]:
            args = c["arguments"]
            print("   %6.1fs %-26s %s" % (c["at_ms"] / 1000, c["tool"].replace("mcp__", ""), args[:150]))
            if "show" not in c["tool"]:
                print("            -> %s%s" % ("ERROR " if c.get("error") else "", counts_of(c["result"])))
    print("  narration: " + r["narration"][:400].replace("\n", " | "))
    print("  answer: " + r["answer"][:600].replace("\n", " | "))
print("=" * 110)
print("mean recall %.1f%%" % (100 * sum(total_recall) / len(total_recall) if total_recall else 0))
