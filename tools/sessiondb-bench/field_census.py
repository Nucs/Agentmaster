# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Which fields Claude Code writes, and how often a session changes them (SESSIONS_DB.md section 5.6).

The input for choosing the session table's columns: a column is worth storing when the transcripts
carry it, and a "first / last" pair is worth storing when sessions really change the value mid-way.
Streams the transcripts newest first (top-level sessions and subagent transcripts), parses every line,
and reports:

  * the line kinds (type, plus a system line's subtype and an attachment's type) by lines and bytes;
  * for the kinds a session column comes from, the share of lines carrying each top-level key;
  * the values of the small enumerations (entrypoint, permissionMode, userType, model, stop_reason,
    a system line's level, the hook attachment kinds, the Claude Code version range);
  * per session: how many change cwd, gitBranch, model, permissionMode or version mid-way, and how many
    carry compactions, API errors, interrupts, rewinds (more than one leaf marker), subagents, custom
    titles, recaps, queued messages, plan-mode exits, images.

Read-only on ~/.claude; nothing is written. Usage: python field_census.py [--budget-gb N] [--all]
"""

import collections
import glob
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

# Kinds whose keys are listed (the lines the session columns read).
KEYED_KINDS = ("user", "assistant", "system:compact_boundary", "system:turn_duration", "system:api_error",
               "attachment:queued_command", "summary", "custom-title", "ai-title", "last-prompt", "permission-mode", "mode")


def kind_of(o: dict) -> str:
    """The census kind of one parsed line: its type, plus the subtype of a system line or the type of an attachment.

    Args:
        o: the parsed line.

    Returns:
        e.g. "assistant", "system:compact_boundary", "attachment:hook_success".
    """
    t = o.get("type") or "?"
    if t == "system":
        return f"system:{o.get('subtype') or '?'}"
    if t == "attachment":
        att = o.get("attachment")
        return f"attachment:{att.get('type') if isinstance(att, dict) else '?'}"
    return t


def transcripts() -> list:
    """Every transcript under ~/.claude/projects, newest first (the byte budget decides how many are read).

    Returns:
        (path, size, is_subagent) tuples, newest modification first. A file that vanishes between the
        listing and its stat is left out.
    """
    root = os.path.join(os.path.expanduser("~"), ".claude", "projects")
    files = [(p, False) for p in glob.glob(os.path.join(root, "*", "*.jsonl"))]
    files += [(p, True) for p in glob.glob(os.path.join(root, "*", "*", "subagents", "*.jsonl"))]
    stamped = []
    for path, sub in files:
        try:
            st = os.stat(path)
        except OSError:
            continue  # vanished between the glob and the stat
        stamped.append((st.st_mtime, path, st.st_size, sub))
    stamped.sort(reverse=True)
    return [(p, s, sub) for _m, p, s, sub in stamped]


def version_key(v: str) -> tuple:
    """A sortable key for a Claude Code version string ("2.1.280" -> (2, 1, 280)).

    Args:
        v: the version text.

    Returns:
        A tuple of ints (non-numeric parts become 0).
    """
    out = []
    for part in v.split("."):
        digits = "".join(ch for ch in part if ch.isdigit())
        out.append(int(digits) if digits else 0)
    return tuple(out)


def main() -> None:
    """Stream the newest transcripts up to the byte budget and print the census.

    Raises:
        SystemExit: a bad argument.
    """
    bc.utf8_stdout()
    budget = 2.0e9
    include_all = "--all" in sys.argv
    if "--budget-gb" in sys.argv:
        try:
            budget = float(sys.argv[sys.argv.index("--budget-gb") + 1]) * 1e9
        except (IndexError, ValueError):
            raise SystemExit("--budget-gb needs a number of gigabytes") from None
    if include_all:
        budget = float("inf")

    lines_by = collections.Counter()
    bytes_by = collections.Counter()
    keys_by = collections.defaultdict(collections.Counter)
    enums = collections.defaultdict(collections.Counter)
    versions = collections.Counter()
    per_session = collections.Counter()
    sessions = subagents = bad = 0
    read = 0
    start = time.time()
    for path, size, sub in transcripts():
        if read >= budget:
            break
        read += size
        if sub:
            subagents += 1
        else:
            sessions += 1
        seen = collections.defaultdict(set)
        flags = collections.Counter()
        try:
            handle = open(path, "rb")
        except OSError:
            continue
        with handle:
            for raw in handle:
                raw = raw.rstrip(b"\r\n")
                if not raw:
                    continue
                try:
                    o = json.loads(raw)
                except ValueError:
                    bad += 1
                    continue
                if not isinstance(o, dict):
                    continue
                k = kind_of(o)
                lines_by[k] += 1
                bytes_by[k] += len(raw)
                if k in KEYED_KINDS:
                    keys_by[k].update(o.keys())
                    msg = o.get("message")
                    if isinstance(msg, dict):
                        keys_by[k + " .message"].update(msg.keys())
                        usage = msg.get("usage")
                        if isinstance(usage, dict):
                            keys_by[k + " .message.usage"].update(usage.keys())
                for name in ("entrypoint", "permissionMode", "userType"):
                    if isinstance(o.get(name), str):
                        enums[name][o[name]] += 1
                if k.startswith("system:") and isinstance(o.get("level"), str):
                    enums["system level"][o["level"]] += 1
                if k.startswith("attachment:hook"):
                    att = o.get("attachment") or {}
                    enums["hook attachments (hookEvent)"][str(att.get("hookEvent"))] += 1
                if isinstance(o.get("version"), str):
                    versions[o["version"]] += 1
                    seen["version"].add(o["version"])
                for name in ("cwd", "gitBranch", "permissionMode"):
                    if isinstance(o.get(name), str) and o[name]:
                        seen[name].add(o[name])
                if o.get("type") == "assistant" and isinstance(o.get("message"), dict):
                    m = o["message"]
                    if isinstance(m.get("model"), str):
                        enums["model"][m["model"]] += 1
                        seen["model"].add(m["model"])
                    if isinstance(m.get("stop_reason"), str):
                        enums["stop_reason"][m["stop_reason"]] += 1
                    if o.get("isApiErrorMessage"):
                        flags["api error"] += 1
                    content = m.get("content")
                    if isinstance(content, list) and any(isinstance(b, dict) and b.get("type") == "tool_use" and b.get("name") in ("Task", "Agent") for b in content):
                        flags["spawned a subagent"] += 1
                    if isinstance(content, list) and any(isinstance(b, dict) and b.get("type") == "tool_use" and b.get("name") == "ExitPlanMode" for b in content):
                        flags["exited plan mode"] += 1
                if o.get("type") == "user" and isinstance(o.get("message"), dict):
                    content = o["message"].get("content")
                    text = content if isinstance(content, str) else ""
                    if isinstance(content, list):
                        for b in content:
                            if isinstance(b, dict) and b.get("type") == "image":
                                flags["pasted an image"] += 1
                            if isinstance(b, dict) and b.get("type") == "text" and isinstance(b.get("text"), str):
                                text = b["text"]
                    if text.startswith("[Request interrupted"):
                        flags["interrupted"] += 1
                if k == "system:compact_boundary":
                    flags["compacted"] += 1
                if k == "system:away_summary":
                    flags["has a recap"] += 1
                if k == "custom-title":
                    flags["custom title"] += 1
                if k == "attachment:queued_command":
                    flags["typed while claude worked"] += 1
                if "leafUuid" in o:
                    flags["leaf markers"] += 1
        if not sub:
            for name in ("cwd", "gitBranch", "model", "permissionMode", "version"):
                if len(seen[name]) > 1:
                    per_session[f"changes {name} mid-session"] += 1
            for name in ("api error", "spawned a subagent", "exited plan mode", "pasted an image", "interrupted", "compacted",
                         "has a recap", "custom title", "typed while claude worked"):
                if flags[name]:
                    per_session[name] += 1
            if flags["leaf markers"] > 1:
                per_session["more than one leaf marker (resumes / rewinds)"] += 1
    elapsed = time.time() - start

    total_lines = sum(lines_by.values())
    total_bytes = sum(bytes_by.values())
    print(f"== {sessions} sessions + {subagents} subagent transcripts, {total_lines} lines, {total_bytes / 1e9:.2f} GB, "
          f"{bad} unparsable lines, read in {elapsed:.0f} s (newest first, budget {'none' if budget == float('inf') else f'{budget / 1e9:.1f} GB'})")
    print("== line kinds (top 40 by lines): lines, share of bytes")
    for k, n in lines_by.most_common(40):
        print(f"   {k}: {n} ({bytes_by[k] / total_bytes * 100:.1f}% of bytes)")
    print("== keys of the kinds the session columns read (share of that kind's lines)")
    for k in sorted(keys_by):
        base = k.split(" .")[0]
        n = lines_by[base] or 1
        print(f"   {k}: " + ", ".join(f"{key} {count / n * 100:.0f}%" for key, count in keys_by[k].most_common()))
    print("== small enumerations")
    for name, counter in sorted(enums.items()):
        print(f"   {name}: " + ", ".join(f"{v} {c}" for v, c in counter.most_common(12)))
    ordered = sorted(versions, key=version_key)
    if ordered:
        print(f"   version: {len(versions)} distinct, {ordered[0]} .. {ordered[-1]}; newest five by lines: "
              + ", ".join(f"{v} {versions[v]}" for v in sorted(versions, key=version_key, reverse=True)[:5]))
    print(f"== per session ({sessions} top-level sessions)")
    for name, n in per_session.most_common():
        print(f"   {name}: {n} ({n / max(sessions, 1) * 100:.1f}%)")


if __name__ == "__main__":
    main()
