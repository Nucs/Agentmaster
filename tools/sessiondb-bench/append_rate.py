# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""How fast Claude Code writes transcripts: the load an always-on ingest must absorb (SESSIONS_DB.md section 7.6).

Two measurements, both read-only on ~/.claude:

  1. THE LAST 24 HOURS, by line timestamp. Every transcript (top-level and subagent) modified in the last
     24 h is parsed line by line; each line's bytes are counted in the hour of its own `timestamp` (a
     line without one — mode, last-prompt, cost-state, … — counts in the hour of the line before it).
     A file CREATED in the last 24 h counts whole, in the hour it was created: a fork copies its parent's
     lines with their old timestamps, so the timestamps alone would miss bytes written today. Prints
     bytes and lines per hour, the busiest hour, and how many files each hour touched. Spilled tool
     outputs (<sid>/tool-results/*) are counted by modification time.
  2. RIGHT NOW: the sizes of every transcript sampled N times, `--interval` seconds apart. Prints the
     bytes appended per interval, how many files grew, and the largest single-file growth — the event
     rate a watcher sees.

Usage: python append_rate.py [--samples N] [--interval S]   (defaults: 4 samples, 30 s apart)
"""

import collections
import glob
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402

ROOT = os.path.join(os.path.expanduser("~"), ".claude", "projects")


def transcript_paths() -> list:
    """Every top-level and subagent transcript under ~/.claude/projects.

    Returns:
        The paths (top-level `<project>/<sid>.jsonl` and `<project>/<sid>/subagents/*.jsonl`).
    """
    return (glob.glob(os.path.join(ROOT, "*", "*.jsonl"))
            + glob.glob(os.path.join(ROOT, "*", "*", "subagents", "*.jsonl")))


def sizes() -> dict:
    """The current size of every transcript and spilled tool output.

    Returns:
        path -> size in bytes. A file that vanishes between the listing and its stat is left out.
    """
    out = {}
    for path in transcript_paths() + glob.glob(os.path.join(ROOT, "*", "*", "tool-results", "*")):
        try:
            out[path] = os.stat(path).st_size
        except OSError:
            continue
    return out


def hour_of(ts: str) -> str:
    """The hour bucket of an ISO-8601 UTC timestamp ("2026-10-12T21:05:09.123Z" -> "2026-10-12T21").

    Args:
        ts: the timestamp text.

    Returns:
        The first 13 characters, or "" for a malformed stamp.
    """
    return ts[:13] if len(ts) >= 13 and ts[4] == "-" and ts[10] == "T" else ""


def last_24h() -> None:
    """Measurement 1: bytes and lines per hour over the last 24 hours, by line timestamp.

    Raises:
        Nothing: an unreadable file or line is skipped (counted in the summary line).
    """
    now = time.time()
    cutoff = time.strftime("%Y-%m-%dT%H", time.gmtime(now - 24 * 3600))
    bytes_by = collections.Counter()
    lines_by = collections.Counter()
    files_by = collections.defaultdict(set)
    scanned = skipped = created = 0
    copied_bytes = 0
    for path in transcript_paths():
        try:
            st = os.stat(path)
            if st.st_mtime < now - 24 * 3600:
                continue
            handle = open(path, "rb")
        except OSError:
            skipped += 1
            continue
        scanned += 1
        # Windows: st_birthtime (3.12+) or st_ctime is the creation time.
        birth = getattr(st, "st_birthtime", st.st_ctime)
        new_file = birth >= now - 24 * 3600
        created += new_file
        birth_hour = time.strftime("%Y-%m-%dT%H", time.gmtime(birth))
        current = ""
        with handle:
            for raw in handle:
                size = len(raw)
                try:
                    o = json.loads(raw)
                    ts = o.get("timestamp") if isinstance(o, dict) else None
                except ValueError:
                    ts = None  # a torn last line: counted in the current hour
                if isinstance(ts, str) and hour_of(ts):
                    current = hour_of(ts)
                # A line stamped before the cutoff in a file created today is a copy written today
                # (a fork's inherited history): count it in the hour the file was created.
                hour = current if (current and current >= cutoff) else (birth_hour if new_file else "")
                if hour and hour != current:
                    copied_bytes += size
                if hour:
                    bytes_by[hour] += size
                    lines_by[hour] += 1
                    files_by[hour].add(path)
    spill_bytes = spill_files = 0
    for path in glob.glob(os.path.join(ROOT, "*", "*", "tool-results", "*")):
        try:
            st = os.stat(path)
        except OSError:
            continue
        if st.st_mtime >= now - 24 * 3600:
            spill_bytes += st.st_size
            spill_files += 1
    total = sum(bytes_by.values())
    print(f"== the last 24 h by line timestamp (UTC hours): {scanned} transcripts modified ({created} created), {skipped} unreadable; "
          f"{copied_bytes / 1e6:.1f} MB of older-stamped lines in files created today (fork copies) counted at their file's birth")
    for hour in sorted(bytes_by):
        print(f"   {hour}h  {bytes_by[hour] / 1e6:8.1f} MB  {lines_by[hour]:7d} lines  {len(files_by[hour]):4d} files")
    if bytes_by:
        peak = max(bytes_by, key=bytes_by.get)
        print(f"   total {total / 1e6:.1f} MB in {sum(lines_by.values())} lines; average {total / 24 / 3600 / 1e3:.1f} KB/s; "
              f"busiest hour {peak}h: {bytes_by[peak] / 1e6:.1f} MB ({bytes_by[peak] / 3600 / 1e3:.1f} KB/s)")
    print(f"   spilled tool outputs written in the last 24 h: {spill_files} files, {spill_bytes / 1e6:.1f} MB")


def right_now(samples: int, interval: float) -> None:
    """Measurement 2: size deltas over `samples` snapshots `interval` seconds apart.

    Args:
        samples: how many snapshots (>= 2).
        interval: seconds between snapshots.
    """
    start = time.perf_counter()
    prev = sizes()
    stat_ms = (time.perf_counter() - start) * 1000
    print(f"== right now: {len(prev)} files, one stat sweep {stat_ms:.0f} ms; {samples - 1} intervals of {interval:.0f} s")
    for i in range(samples - 1):
        time.sleep(interval)
        cur = sizes()
        grown = {p: cur[p] - prev.get(p, 0) for p in cur if cur[p] != prev.get(p, -1)}
        added = sum(d for d in grown.values() if d > 0)
        biggest = max(grown.items(), key=lambda kv: kv[1], default=(None, 0))
        new_files = sum(1 for p in cur if p not in prev)
        print(f"   interval {i + 1}: +{added / 1e3:.0f} KB ({added / interval / 1e3:.1f} KB/s) in {len(grown)} files "
              f"({new_files} new); largest growth {biggest[1] / 1e3:.0f} KB")
        prev = cur


def main() -> None:
    """Run both measurements.

    Raises:
        SystemExit: a bad argument.
    """
    bc.utf8_stdout()
    samples, interval = 4, 30.0
    try:
        if "--samples" in sys.argv:
            samples = max(2, int(sys.argv[sys.argv.index("--samples") + 1]))
        if "--interval" in sys.argv:
            interval = float(sys.argv[sys.argv.index("--interval") + 1])
    except (IndexError, ValueError):
        raise SystemExit(__doc__) from None
    last_24h()
    right_now(samples, interval)


if __name__ == "__main__":
    main()
