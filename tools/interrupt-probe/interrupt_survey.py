"""Survey the shapes of user interrupts (Esc) in this machine's Claude Code transcripts. READ-ONLY.

For every row whose whole text is one of Claude Code's interrupt rows, it walks the parent chain upward and
classifies the rows above it (the interrupted response, rejected tool results, the user row below the
response), then prints how often each shape occurs, a few examples of each, and how many transcripts END on
an interrupt. This is how the shapes PlanInterruptRewind handles were chosen (doc/agentmaster/HOOKS.md
"Interrupted turns"); re-run it after a Claude Code update to see whether a new shape appeared.

Usage:
    python -I interrupt_survey.py [max files]
The transcripts are read from $CLAUDE_CONFIG_DIR/projects when that variable is set, else ~/.claude/projects
(subagent transcripts are skipped). Nothing is written.
"""
import json
import os
import sys
from collections import Counter, defaultdict

#: Claude Code's interrupt row while a reply streams (its bundle's `VS`).
VS = "[Request interrupted by user]"
#: Claude Code's interrupt row while a tool call runs (its bundle's `gu`).
GU = "[Request interrupted by user for tool use]"


def projects_root():
    """The folder of the transcripts to survey.

    Returns:
        $CLAUDE_CONFIG_DIR/projects when the variable is set (Claude Code's own rule), else ~/.claude/projects.
    """
    config = os.environ.get("CLAUDE_CONFIG_DIR")
    return os.path.join(config if config else os.path.expanduser(r"~\.claude"), "projects")


def content_text(content):
    """The text of a message's content.

    Args:
        content: A message's "content": a string, or a list of blocks.
    Returns:
        The string itself, the joined text of the list's text blocks, or "" for anything else.
    """
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text")
    return ""


def describe(row):
    """One line about a row above an interrupt.

    Args:
        row: A parsed transcript line.
    Returns:
        "A[blocks] ..." for an assistant row, "U[tool_result ...]" (with toolUseResult and toolDenialKind)
        or "U[text ...]" (META and SHUTDOWN marked) for a user row, the bare type otherwise.
    """
    t = row.get("type")
    msg = row.get("message") or {}
    content = msg.get("content")
    if t == "assistant":
        kinds = []
        if isinstance(content, list):
            for b in content:
                if isinstance(b, dict):
                    k = b.get("type")
                    if k == "tool_use":
                        kinds.append("tool_use:" + str(b.get("name")))
                    else:
                        kinds.append(str(k))
        flag = " APIERR" if row.get("isApiErrorMessage") else ""
        return f"A[{','.join(kinds)}]{flag} id={str(msg.get('id'))[-6:]} stop={msg.get('stop_reason')}"
    if t == "user":
        if isinstance(content, list) and content and isinstance(content[0], dict) and content[0].get("type") == "tool_result":
            c = content[0].get("content")
            if isinstance(c, list):
                c = content_text(c)
            c = (c or "")[:70].replace("\n", " ")
            tur = row.get("toolUseResult")
            turs = (str(tur)[:40] if tur is not None else "-")
            return f"U[tool_result err={content[0].get('is_error')} '{c}' tur={turs} denial={row.get('toolDenialKind')}]"
        text = content_text(content)[:60].replace("\n", " ")
        meta = " META" if row.get("isMeta") else ""
        shut = " SHUTDOWN" if row.get("interruptedByShutdown") else ""
        return f"U[text '{text}']{meta}{shut}"
    return f"{t}"


def shape_key(marker_text, chain):
    """The compact shape of an interrupt and the rows above it.

    Args:
        marker_text: The interrupt row's text (VS or GU).
        chain: describe() lines of the rows above it, nearest first.
    Returns:
        "VS <- A <- P" style: A = an assistant row, R(rej|ok) = a tool result (rejected or not), P / Pmeta = a
        prompt / a meta prompt, other types by name; runs of one kind are counted ("Ax3").
    """
    key_parts = []
    for d in chain:
        if d.startswith("A["):
            key_parts.append("A")
        elif d.startswith("U[tool_result"):
            key_parts.append("R(" + ("rej" if "doesn't want" in d or "interrupted by user" in d else "ok") + ")")
        elif d.startswith("U[text"):
            key_parts.append("P" + ("meta" if "META" in d else ""))
        else:
            key_parts.append(d.split("[")[0])
    compact = []
    for k in key_parts:
        if compact and compact[-1][0] == k:
            compact[-1][1] += 1
        else:
            compact.append([k, 1])
    return ("GU" if marker_text == GU else "VS") + " <- " + " <- ".join(f"{k}x{n}" if n > 1 else k for k, n in compact)


def survey_file(path, shapes, examples, tail_markers):
    """Add one transcript's interrupts to the counts.

    Args:
        path: The .jsonl file.
        shapes: A Counter of shape keys (updated).
        examples: shape key -> up to two (path, uuid, chain) examples (updated).
        tail_markers: A Counter of the interrupt kinds that END their transcript (updated).
    Returns:
        None. An unreadable file is skipped (a survey reads what it can).
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            raw = f.read()
    except OSError:
        return
    # One substring scan before any JSON: most transcripts hold no interrupt at all.
    if VS not in raw and GU not in raw:
        return
    rows = {}
    order = []
    for line in raw.splitlines():
        try:
            o = json.loads(line)
        except ValueError:
            continue
        if not isinstance(o, dict) or o.get("isSidechain"):
            continue
        u = o.get("uuid")
        if not u or o.get("type") not in ("user", "assistant", "system", "attachment", "progress"):
            continue
        rows[u] = o
        order.append(u)
    last_conv = None
    for u in reversed(order):
        if rows[u].get("type") in ("user", "assistant"):
            last_conv = u
            break
    for u in order:
        o = rows[u]
        if o.get("type") != "user":
            continue
        text = content_text((o.get("message") or {}).get("content"))
        if text not in (VS, GU):
            continue
        chain = []
        cur = o.get("parentUuid")
        guard = 0
        # Up to 12 rows, ending at the first prompt (a non-tool-result, non-meta user row): the shape is what
        # sits between the interrupt and the user row a rewind would aim at.
        while cur and cur in rows and guard < 12:
            r = rows[cur]
            chain.append(describe(r))
            if r.get("type") == "user":
                c = (r.get("message") or {}).get("content")
                is_tr = isinstance(c, list) and c and isinstance(c[0], dict) and c[0].get("type") == "tool_result"
                if not is_tr and not r.get("isMeta"):
                    break
            cur = r.get("parentUuid")
            guard += 1
        key = shape_key(text, chain)
        shapes[key] += 1
        if len(examples[key]) < 2:
            examples[key].append((path, u, chain[:6]))
        if u == last_conv:
            tail_markers["GU" if text == GU else "VS"] += 1


def main(argv):
    """Survey every transcript (or the first N) and print the shapes.

    Args:
        argv: [max files] (default: all).
    Returns:
        0.
    Raises:
        ValueError: When max files is not a number.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    max_files = int(argv[0]) if argv else 10 ** 9
    shapes = Counter()
    examples = defaultdict(list)
    tail_markers = Counter()
    files = 0
    for dirpath, _dirs, names in os.walk(projects_root()):
        if os.sep + "subagents" in dirpath:
            continue
        for name in names:
            if not name.endswith(".jsonl"):
                continue
            files += 1
            if files > max_files:
                break
            survey_file(os.path.join(dirpath, name), shapes, examples, tail_markers)
    print(f"files scanned: {min(files, max_files)}")
    print("tail-of-transcript interrupt markers:", dict(tail_markers))
    for key, n in shapes.most_common(40):
        print(f"{n:6d}  {key}")
        for path, u, chain in examples[key]:
            print("          ", os.path.basename(path), u[:8])
            for d in chain:
                print("               ", d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
