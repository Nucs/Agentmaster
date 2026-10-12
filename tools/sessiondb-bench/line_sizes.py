# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Distribution of JSONL line lengths over every transcript (SESSIONS_DB.md section 1.2).

Two design numbers come from it: the JSON reader's maximum object size (ingest.py uses 20 MB, the
largest line seen being 18.5 MB) and the per-block text caps of the schema. READ-ONLY; reads every
transcript once (about 40 s for 10 GB on an NVMe disk).

Usage: python line_sizes.py
"""

import glob
import os

PROJECTS = os.path.expanduser("~/.claude/projects")
# Upper bounds of the histogram buckets, in bytes; the last bucket is "larger than all of these".
BUCKETS = [1 << 10, 1 << 14, 1 << 16, 1 << 20, 1 << 22, 1 << 24, 1 << 26]
LABELS = ["<=1K", "<=16K", "<=64K", "<=1M", "<=4M", "<=16M", "<=64M", ">64M"]
# Lines above this size are listed one by one (they decide the reader's object-size cap).
BIG_LINE = 1 << 22


def main() -> None:
    """Print the histogram, the line count and the biggest lines with their files.

    Unreadable files (deleted by Claude's sweep mid-run, or locked) are skipped, not fatal.
    """
    files = glob.glob(os.path.join(PROJECTS, "*", "*.jsonl")) + glob.glob(os.path.join(PROJECTS, "*", "*", "subagents", "*.jsonl"))
    counts = [0] * (len(BUCKETS) + 1)
    total = 0
    biggest = []
    for path in files:
        try:
            with open(path, "rb") as fh:
                for line in fh:
                    n = len(line)
                    total += 1
                    i = 0
                    while i < len(BUCKETS) and n > BUCKETS[i]:
                        i += 1
                    counts[i] += 1
                    if n > BIG_LINE:
                        biggest.append((n, path))
        except OSError:
            continue
    print("files", len(files), "lines", total)
    for label, count in zip(LABELS, counts):
        print(f"{label:>6}: {count}")
    biggest.sort(reverse=True)
    for n, path in biggest[:8]:
        print(f"{n / 1e6:8.1f} MB  {path}")


if __name__ == "__main__":
    main()
