"""Agentmaster leak hunt: rank and diff heapcensus.exe TSVs.

Why: heapcensus writes one row per vftable (count, refd, module, rva, block, name). A C++/WinRT or XAML
object has one vftable per interface, so the rows of one class repeat its instance count; this folds
them into one row per CLASS (the instance count is the largest of its rows), estimates the bytes the
class holds (instances x the block size heapcensus measured), and - given two censuses of the same
process taken apart in time - shows which classes GREW. Growth between two dumps is the leak
evidence a single dump cannot give (a dump also holds freed blocks the heap has not reused yet).

Commands (python -I censustool.py ...):
    bytes <census.tsv> [--top N] [--module SUBSTR]   classes ranked by estimated bytes
    diff  <old.tsv> <new.tsv> [--top N] [--min N]    classes ranked by instance growth (new - old)
    live  <old-live.tsv> <new-live.tsv> [--top N]     segheap.py --live-out files: LIVE bytes growth per
                                                      class and per unmatched block size (the leak test)
"""

import argparse
import collections
import sys


def class_key(name):
    """The class a vftable row belongs to: the text between "const " and "::`vftable'".

    Args:
        name: the row's display name (possibly several ICF-folded names joined by " | ").

    Returns:
        The class name (the first folded name's); the whole name when the shape is unexpected.

    Raises:
        Nothing.
    """
    first = name.split(" | ", 1)[0]
    if first.startswith("const "):
        first = first[6:]
    cut = first.find("::`vftable'")
    return first[:cut] if cut >= 0 else first


def load(path):
    """Fold a census TSV into one record per (module, class).

    Args:
        path: a heapcensus --out TSV (with or without the "block" column of --sizes).

    Returns:
        A dict (module, class) -> {"count", "refd", "block"}: the count and refd of the class's busiest
        row (its instances), and that row's block size (0 when unknown).

    Raises:
        OSError: when the file cannot be read.
    """
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\n").split("\t")
        has_block = "block" in header
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < len(header):
                continue
            count, refd, module = int(parts[0]), int(parts[1]), parts[2]
            block = int(parts[4]) if has_block else 0
            name = parts[5] if has_block else parts[4]
            key = (module, class_key(name))
            rec = out.get(key)
            if rec is None or count > rec["count"]:
                out[key] = {"count": count, "refd": refd, "block": block}
    return out


def cmd_bytes(a):
    """Print classes ranked by estimated bytes (count x block).

    Args:
        a: the parsed arguments (tsv, top, module).

    Returns:
        0.

    Raises:
        OSError: from load().
    """
    recs = load(a.tsv)
    rows = [(r["count"] * r["block"], k, r) for k, r in recs.items() if a.module.lower() in k[0].lower()]
    rows.sort(reverse=True)
    total = sum(b for b, _, _ in rows)
    print(f"estimated bytes in vftable-bearing objects (top-level classes overlap with their bases): {total / 1048576:.1f} MB")
    for b, (mod, cls), r in rows[: a.top]:
        print(f"{b / 1048576:8.1f} MB  {r['count']:9}  x {r['block']:5}  refd {r['refd']:9}  {mod:22} {cls[:150]}")
    return 0


def cmd_diff(a):
    """Print classes ranked by instance growth between two censuses.

    Args:
        a: the parsed arguments (old, new, top, min).

    Returns:
        0.

    Raises:
        OSError: from load().
    """
    old, new = load(a.old), load(a.new)
    rows = []
    for k in set(old) | set(new):
        o = old.get(k, {"count": 0, "block": 0})
        n = new.get(k, {"count": 0, "block": 0})
        d = n["count"] - o["count"]
        if abs(d) >= a.min:
            rows.append((d, k, o["count"], n["count"], n["block"] or o["block"]))
    rows.sort(key=lambda r: -r[0])
    print("growth (new - old), largest first:")
    for d, (mod, cls), oc, nc, block in rows[: a.top]:
        print(f"{d:+9}  {oc:9} -> {nc:9}  ({d * block / 1048576:+7.1f} MB)  {mod:22} {cls[:140]}")
    print("...shrink, largest first:")
    for d, (mod, cls), oc, nc, block in sorted(rows, key=lambda r: r[0])[: min(15, a.top)]:
        if d < 0:
            print(f"{d:+9}  {oc:9} -> {nc:9}  ({d * block / 1048576:+7.1f} MB)  {mod:22} {cls[:140]}")
    return 0


def load_live(path):
    """Read a segheap.py --live-out TSV.

    Args:
        path: the file (kind, count, bytes, module, name).

    Returns:
        A dict (kind, module, name) -> (count, bytes).

    Raises:
        OSError: when the file cannot be read.
    """
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        f.readline()
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5:
                out[(p[0], p[3], p[4])] = (int(p[1]), int(p[2]))
    return out


def cmd_live(a):
    """Rank LIVE classes (and unmatched block sizes) by byte growth between two segheap censuses.

    Unlike `diff`, both inputs count only busy heap blocks, so a change here is a change in what the
    process holds, never freed bytes awaiting reuse.

    Args:
        a: the parsed arguments (old, new, top).

    Returns:
        0.

    Raises:
        OSError: from load_live().
    """
    old, new = load_live(a.old), load_live(a.new)
    rows = []
    for k in set(old) | set(new):
        oc, ob = old.get(k, (0, 0))
        nc, nb = new.get(k, (0, 0))
        rows.append((nb - ob, nc - oc, k, oc, nc))
    tot_old = sum(v[1] for v in old.values())
    tot_new = sum(v[1] for v in new.values())
    print(f"live bytes: {tot_old / 1048576:.1f} MB -> {tot_new / 1048576:.1f} MB ({(tot_new - tot_old) / 1048576:+.1f} MB)")
    rows.sort(key=lambda r: -r[0])
    print("grew most (bytes):")
    for db, dc, (kind, mod, name), oc, nc in rows[: a.top]:
        label = f"{name} B blocks" if kind == "size" else f"{mod} {name[:140]}"
        print(f"{db / 1048576:+8.2f} MB {dc:+8}  ({oc} -> {nc})  {label}")
    print("shrank most (bytes):")
    for db, dc, (kind, mod, name), oc, nc in sorted(rows, key=lambda r: r[0])[: min(20, a.top)]:
        label = f"{name} B blocks" if kind == "size" else f"{mod} {name[:140]}"
        print(f"{db / 1048576:+8.2f} MB {dc:+8}  ({oc} -> {nc})  {label}")
    return 0


def main(argv):
    """Parse the command line and run one command.

    Args:
        argv: the arguments after the script name.

    Returns:
        The command's exit code.

    Raises:
        SystemExit: from argparse on a usage error.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bytes")
    b.add_argument("tsv")
    b.add_argument("--top", type=int, default=60)
    b.add_argument("--module", default="")
    d = sub.add_parser("diff")
    d.add_argument("old")
    d.add_argument("new")
    d.add_argument("--top", type=int, default=60)
    d.add_argument("--min", type=int, default=50)
    lv = sub.add_parser("live")
    lv.add_argument("old")
    lv.add_argument("new")
    lv.add_argument("--top", type=int, default=50)
    a = ap.parse_args(argv)
    if a.cmd == "bytes":
        return cmd_bytes(a)
    return cmd_diff(a) if a.cmd == "diff" else cmd_live(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
