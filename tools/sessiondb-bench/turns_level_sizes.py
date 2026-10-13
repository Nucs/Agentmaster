# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""How big a session is at each transcript level (SESSIONS_AGENT.md section 4.2).

The session agent's `read` and `peek` tools page a session by turns at a zoom level (the Copy >
Transcript levels: minimal, compact, default, full, max). Their default budgets come from these
numbers: whole-session characters per level, turns per session, characters per turn, and an
OUTLINE proxy (one line per turn: a 40-character header, the first line of the user's message cut
at 140 characters, and the turn's last non-empty line cut at 140 characters).

READ-ONLY. It reads transcripts under ~/.claude/projects and runs the am-agentmaster-cli skill's
reference script (transcript_turns.py, the script the C++ port TranscriptTurns.cpp is pinned to)
once per level and session, with --no-chain. Nothing is written but the output JSON.

Usage:
    python turns_level_sizes.py measure <out.json> [--per-bucket N] [--days D] [--seed S]
    python turns_level_sizes.py stats <out.json>

measure: picks transcripts changed in the last D days (default 120), in five size buckets, N per
bucket (default 6; the largest bucket gets N // 3), with a fixed random seed (default 7), and
writes one row per transcript. About 0.15 to 0.9 s per level and transcript.
stats: prints the tables of SESSIONS_AGENT.md section 13.1 from such a file.
"""

import json
import os
import random
import re
import subprocess
import sys
import time

# The reference renderer. Its output is what Copy > Transcript copies (the C++ port is pinned to
# goldens this script writes), so its sizes are the sizes the agent would read.
SCRIPT = os.path.expanduser(os.path.join("~", ".claude", "skills", "am-agentmaster-cli", "scripts", "transcript_turns.py"))
PROJECTS = os.path.expanduser(os.path.join("~", ".claude", "projects"))
# (level name, the reference script's flags for it); "default" has no flag.
LEVELS = [("minimal", ["--minimal"]), ("compact", ["--compact"]), ("default", []), ("full", ["--full"]), ("max", ["--max"])]
# Transcript size buckets in bytes of the .jsonl: small, typical, large, very large, huge.
BUCKETS = [(0, 200_000), (200_000, 1_000_000), (1_000_000, 5_000_000), (5_000_000, 20_000_000), (20_000_000, 10**12)]
BUCKET_NAMES = ["<200 KB", "200 KB-1 MB", "1-5 MB", "5-20 MB", ">20 MB"]
# A turn's heading in the rendered Markdown ("### Turn 3 · Oct 5 21:30 → last reply ...").
TURN_RE = re.compile(r"^### Turn (\d+)", re.M)


def candidates(days: int) -> list:
    """List the top-level transcripts changed in the last `days` days.

    Args:
        days: the age limit, by the file's modification time.
    Returns:
        A list of (path, size in bytes) pairs. Files of 2,000 bytes or less (a session that never
        got a prompt) are left out. Folders or files that vanish while listed (Claude's sweep)
        are skipped.
    """
    now = time.time()
    out = []
    for proj in os.listdir(PROJECTS):
        folder = os.path.join(PROJECTS, proj)
        if not os.path.isdir(folder):
            continue
        try:
            names = os.listdir(folder)
        except OSError:
            continue  # the folder went away between the two listings
        for name in names:
            if not name.endswith(".jsonl"):
                continue
            path = os.path.join(folder, name)
            try:
                st = os.stat(path)
            except OSError:
                continue
            if now - st.st_mtime <= days * 86400 and st.st_size > 2000:
                out.append((path, st.st_size))
    return out


def render(path: str, flags: list) -> tuple:
    """Render one transcript at one level with the reference script.

    Args:
        path: the transcript (.jsonl).
        flags: the level's flags (see LEVELS).
    Returns:
        (the Markdown text, the seconds the render took including the interpreter's start,
        the script's exit code).
    Raises:
        subprocess.TimeoutExpired: the render took more than 10 minutes.
    """
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"  # the child is not started with -I, so it honors this
    t0 = time.time()
    r = subprocess.run([sys.executable, SCRIPT, path, "--no-chain"] + flags, capture_output=True, env=env, timeout=600)
    return r.stdout.decode("utf-8", "replace"), time.time() - t0, r.returncode


def outline_chars(text: str) -> tuple:
    """Estimate the size of a one-line-per-turn outline from a rendered transcript.

    Per turn: 40 characters of header, the first non-empty line after the user's label (cut at
    140) and the turn's last non-empty line (cut at 140; a proxy for the start of the last reply).

    Args:
        text: a rendered transcript (any level; the minimal one is used).
    Returns:
        (the estimated outline size in characters, the number of turns found).
    """
    parts = TURN_RE.split(text)
    total = 0
    turns = 0
    # split() gives [preamble, number1, body1, number2, body2, ...]
    for i in range(1, len(parts) - 1, 2):
        lines = [line.strip() for line in parts[i + 1].splitlines() if line.strip()]
        first_user = ""
        for j, line in enumerate(lines):
            if line.startswith("**❯"):  # the "**❯ User**" label
                first_user = lines[j + 1] if j + 1 < len(lines) else ""
                break
        last = lines[-1] if lines else ""
        total += 40 + min(len(first_user), 140) + min(len(last), 140)
        turns += 1
    return total, turns


def measure(out_path: str, per_bucket: int, days: int, seed: int) -> None:
    """Render a stratified sample at every level and write the sizes.

    Args:
        out_path: the JSON file to write ({"n", "candidates", "rows": [...]}).
        per_bucket: transcripts per size bucket (the largest bucket gets per_bucket // 3, at least 1).
        days: the age limit of the candidates.
        seed: the random seed of the sample (the same seed and corpus give the same sample).
    Raises:
        OSError: the output cannot be written.
        subprocess.TimeoutExpired: a render took more than 10 minutes.
    """
    rnd = random.Random(seed)
    cands = candidates(days)
    picked = []
    for lo, hi in BUCKETS:
        bucket = [c for c in cands if lo <= c[1] < hi]
        rnd.shuffle(bucket)
        count = per_bucket if hi <= 20_000_000 else max(1, per_bucket // 3)
        picked += bucket[:count]
    rows = []
    for path, size in picked:
        row = {"file": os.path.basename(path), "bytes": size, "levels": {}}
        for name, flags in LEVELS:
            text, secs, rc = render(path, flags)
            row["levels"][name] = {"chars": len(text), "turns": len(TURN_RE.findall(text)), "secs": round(secs, 2), "rc": rc}
            if name == "minimal":
                row["outline_chars"], row["turns"] = outline_chars(text)
        rows.append(row)
        print(json.dumps(row), flush=True)  # progress, one line per transcript
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump({"n": len(rows), "candidates": len(cands), "rows": rows}, fh, indent=1)


def quantile(values: list, q: float) -> float:
    """Linear-interpolated quantile.

    Args:
        values: numbers (any order).
        q: the quantile, 0.0 to 1.0.
    Returns:
        The quantile, or 0 for an empty list.
    """
    v = sorted(values)
    if not v:
        return 0
    k = (len(v) - 1) * q
    f = int(k)
    c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def stats(in_path: str) -> None:
    """Print per-bucket rows, per-level quantiles and budget fits from a measure() file.

    Args:
        in_path: the JSON file measure() wrote.
    Raises:
        OSError, ValueError: the file cannot be read or is not such a file.
    """
    data = json.load(open(in_path, encoding="utf-8"))
    rows = data["rows"]
    names = [name for name, _ in LEVELS]
    print(f"sessions measured: {data['n']} of {data['candidates']} candidates")
    for (lo, hi), label in zip(BUCKETS, BUCKET_NAMES):
        in_bucket = [r for r in rows if lo <= r["bytes"] < hi]
        if not in_bucket:
            continue
        print(f"\n== {label}: n={len(in_bucket)}")
        print("  file           bytes  turns  outline  " + "  ".join(f"{n:>9}" for n in names))
        for r in in_bucket:
            lv = r["levels"]
            print(f"  {r['file'][:8]}  {r['bytes']:>12,}  {r.get('turns', 0):>5}  {r.get('outline_chars', 0):>7,}  " + "  ".join(f"{lv[n]['chars']:>9,}" for n in names))
    print("\n== characters per turn (sessions with at least one turn): median / p75 / p90 / max")
    for n in names:
        v = [r["levels"][n]["chars"] / r["levels"][n]["turns"] for r in rows if r["levels"][n]["turns"] > 0]
        print(f"  {n:8} {quantile(v, .5):>9,.0f} {quantile(v, .75):>9,.0f} {quantile(v, .9):>9,.0f} {max(v):>10,.0f}")
    v = [r["outline_chars"] / r["turns"] for r in rows if r.get("turns", 0) > 0]
    print(f"  outline  {quantile(v, .5):>9,.0f} {quantile(v, .75):>9,.0f} {quantile(v, .9):>9,.0f} {max(v):>10,.0f}")
    print("\n== whole session, characters: median / p75 / p90 / max")
    for n in names:
        v = [r["levels"][n]["chars"] for r in rows]
        print(f"  {n:8} {quantile(v, .5):>10,.0f} {quantile(v, .75):>10,.0f} {quantile(v, .9):>10,.0f} {max(v):>11,.0f}")
    v = [r.get("outline_chars", 0) for r in rows]
    print(f"  outline  {quantile(v, .5):>10,.0f} {quantile(v, .75):>10,.0f} {quantile(v, .9):>10,.0f} {max(v):>11,.0f}")
    v = [r.get("turns", 0) for r in rows]
    print(f"\n== turns per session: median {quantile(v, .5):.0f}, p75 {quantile(v, .75):.0f}, p90 {quantile(v, .9):.0f}, max {max(v)}")
    print("\n== share of sessions whose WHOLE text fits a budget")
    for budget in (12_000, 16_000, 24_000, 48_000, 96_000):
        fits = [f"{n}:{sum(1 for r in rows if r['levels'][n]['chars'] <= budget) / len(rows) * 100:.0f}%" for n in names]
        fits.append(f"outline:{sum(1 for r in rows if r.get('outline_chars', 0) <= budget) / len(rows) * 100:.0f}%")
        print(f"  {budget:>6,} chars: " + "  ".join(fits))
    secs = [r["levels"][n]["secs"] for r in rows for n in names]
    print(f"\n== render seconds per level (reference script, interpreter start included): median {quantile(secs, .5):.2f}, max {max(secs):.2f}")
    failed = [(r["file"][:8], n) for r in rows for n in names if r["levels"][n]["rc"] != 0]
    print("renders that failed:", failed or "none")


def main() -> None:
    """Parse the command line and run `measure` or `stats` (see the module's docstring).

    Raises:
        SystemExit: a usage error.
    """
    sys.stdout.reconfigure(encoding="utf-8")  # titles and paths are not all cp1252
    args = sys.argv[1:]
    if len(args) < 2 or args[0] not in ("measure", "stats"):
        raise SystemExit(__doc__)
    if args[0] == "stats":
        stats(args[1])
        return
    per_bucket, days, seed = 6, 120, 7
    rest = args[2:]
    for i, a in enumerate(rest):
        if a == "--per-bucket":
            per_bucket = int(rest[i + 1])
        elif a == "--days":
            days = int(rest[i + 1])
        elif a == "--seed":
            seed = int(rest[i + 1])
    measure(args[1], per_bucket, days, seed)


if __name__ == "__main__":
    main()
