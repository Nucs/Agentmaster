# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Census of the Claude Code session corpus on this machine (SESSIONS_DB.md section 1.2).

Counts what a session database would have to hold: top-level session transcripts (by modification
window), legacy agent-*.jsonl files, subagent transcripts, spilled tool outputs, history.jsonl, and the
Agentmaster sidecar index and session store of both profiles. READ-ONLY: it lists and stats files and
counts history.jsonl's lines; it opens no transcript.

Usage: python corpus_census.py
"""

import os
import re
import time

HOME = os.path.expanduser("~")
PROJECTS = os.path.join(HOME, ".claude", "projects")
UUID = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")


def gb(n: int) -> str:
    """Format a byte count as gigabytes with two decimals (1e9, as the doc's tables use).

    Args:
        n: a byte count.

    Returns:
        The text, for example "8.26 GB".
    """
    return f"{n / 1e9:.2f} GB"


def walk_projects():
    """List every transcript-shaped file under ~/.claude/projects.

    A session is a TOP-LEVEL `<uuid>.jsonl` (the filename stem is the session id); legacy
    `agent-*.jsonl` sit at the top level too; subagent transcripts and spilled tool outputs live in
    the session's `<uuid>/` folder. Anything else is counted by extension so a new kind of file shows.

    Returns:
        A tuple (top, legacy, sub, toolres, other): `top` is a list of (size, mtime) per session
        transcript, `legacy`/`sub`/`toolres` are lists of sizes, `other` a dict extension -> count.

    Raises:
        FileNotFoundError: ~/.claude/projects does not exist (no Claude Code on this machine).
    """
    top, legacy, sub, toolres, other = [], [], [], [], {}
    for proj in os.scandir(PROJECTS):
        if not proj.is_dir():
            continue
        for entry in os.scandir(proj.path):
            if entry.is_file():
                stem, ext = os.path.splitext(entry.name)
                st = entry.stat()
                if ext == ".jsonl" and UUID.match(stem):
                    top.append((st.st_size, st.st_mtime))
                elif ext == ".jsonl" and stem.startswith("agent-"):
                    legacy.append(st.st_size)
                else:
                    other[ext] = other.get(ext, 0) + 1
            elif entry.is_dir() and UUID.match(entry.name):
                for root, _dirs, files in os.walk(entry.path):
                    leaf = os.path.basename(root)
                    for f in files:
                        p = os.path.join(root, f)
                        try:
                            size = os.path.getsize(p)
                        except OSError:
                            continue  # Claude's sweep deleted it mid-walk: not an error
                        if leaf == "subagents" and f.endswith(".jsonl"):
                            sub.append(size)
                        elif leaf == "tool-results":
                            toolres.append(size)
                        else:
                            key = "sub:" + os.path.splitext(f)[1]
                            other[key] = other.get(key, 0) + 1
    return top, legacy, sub, toolres, other


def main() -> None:
    """Print the census.

    Raises:
        FileNotFoundError: ~/.claude/projects does not exist.
    """
    start = time.time()
    top, legacy, sub, toolres, other = walk_projects()
    sizes = sorted(s for s, _ in top)
    now = time.time()
    print(f"walk took {time.time() - start:.1f} s")
    print(f"top-level session transcripts: {len(top)}  total {gb(sum(sizes))}")
    if sizes:
        n = len(sizes)
        print(f"  size p50 {sizes[n // 2] / 1e6:.2f} MB  p90 {sizes[int(n * 0.9)] / 1e6:.2f} MB  "
              f"p99 {sizes[int(n * 0.99)] / 1e6:.2f} MB  max {sizes[-1] / 1e6:.1f} MB")
    for days in (1, 7, 30, 90, 365):
        window = [s for s, m in top if now - m <= days * 86400]
        print(f"  mtime within {days:>3} d: {len(window):>5} files  {gb(sum(window))}")
    print(f"legacy agent-*.jsonl: {len(legacy)}  {gb(sum(legacy))}")
    print(f"subagent transcripts: {len(sub)}  {gb(sum(sub))}")
    print(f"tool-results files: {len(toolres)}  {gb(sum(toolres))}")
    print("other files:", dict(sorted(other.items(), key=lambda kv: -kv[1])[:12]))
    history = os.path.join(HOME, ".claude", "history.jsonl")
    if os.path.exists(history):
        with open(history, "rb") as fh:
            lines = sum(1 for _ in fh)
        print(f"history.jsonl: {os.path.getsize(history) / 1e6:.1f} MB, {lines} lines")
    for profile in (".agentmaster", ".agentmaster-dev"):
        for folder in ("sessions-index", "session-store"):
            d = os.path.join(HOME, profile, folder)
            if os.path.isdir(d):
                files = [f for f in os.scandir(d) if f.is_file()]
                print(f"{profile}/{folder}: {len(files)} files, {sum(f.stat().st_size for f in files) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
