"""Agentmaster leak hunt: who holds the live objects of one class - a histogram of their owners.

Why: ownerchain.py follows one object; a population (17,000 text layouts, 68,000 menu items) needs the
distribution of its holders. This takes every LIVE block whose vftable (at +0..+24) belongs to a class
matching --class, asks heapcensus.exe for the slots pointing into each block (--refs-of: one scan for
all of them), maps each slot to the busy block holding it and names that block's class, then prints
"owner class @ +offset" counts. A slot outside any busy block is reported as a stack / global /
mapped-memory holder. --exclude drops owner classes (e.g. the population's own internal partner
object) so the next real holder shows.

Usage (python -I owners.py ...):
    owners.py <dump> <blocks.tsv> <census.tsv> --class <substring> [--exclude <substring> ...]
              [--sample N] [--heapcensus <exe>] [--work <dir>]
    blocks.tsv  segheap.py --blocks-out of the same dump; census.tsv  heapcensus --out of the same dump.
"""

import argparse
import bisect
import collections
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import segheap  # noqa: E402


def load_blocks(path):
    """Read busy blocks sorted by address.

    Args:
        path: a segheap.py --blocks-out file.

    Returns:
        (starts, sizes): two parallel lists sorted by start.

    Raises:
        OSError: when the file cannot be read.
    """
    rows = []
    with open(path, encoding="ascii") as f:
        for line in f:
            a, s, _k = line.split("\t")
            rows.append((int(a, 16), int(s)))
    rows.sort()
    return [r[0] for r in rows], [r[1] for r in rows]


def block_class(d, vt, start):
    """Name a block by the first known vftable among its first four qwords.

    Args:
        d: the segheap.Dump.
        vt: va -> (module, name) from the census TSV.
        start: the block start.

    Returns:
        (class name, offset of that vftable) or (None, None) when no known vftable is there.

    Raises:
        Nothing.
    """
    for off in (0, 8, 16, 24):
        hit = vt.get(d.q(start + off))
        if hit:
            return segheap.class_key(hit[1]), off
    return None, None


def main(argv):
    """Find the population, collect its referrers in one heapcensus pass, and print the owner histogram.

    Args:
        argv: the arguments after the script name.

    Returns:
        0, or 2 when no live block matches --class.

    Raises:
        OSError when heapcensus cannot run or an input cannot be read.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump")
    ap.add_argument("blocks")
    ap.add_argument("census")
    ap.add_argument("--class", dest="cls", required=True)
    ap.add_argument("--exclude", action="append", default=[])
    ap.add_argument("--sample", type=int, default=0, help="analyze a random sample of N objects (0 = all)")
    ap.add_argument("--heapcensus", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "heapcensus.exe"))
    ap.add_argument("--work", default=os.environ.get("TEMP", "."))
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args(argv)

    d = segheap.Dump(a.dump)
    vt = segheap.load_vtables(d, a.census)
    starts, sizes = load_blocks(a.blocks)
    targets = []
    for s, n in zip(starts, sizes):
        cls, _off = block_class(d, vt, s)
        if cls and a.cls in cls:
            targets.append((s, n))
    if not targets:
        print("no live block matches --class")
        return 2
    if a.sample and len(targets) > a.sample:
        random.seed(1)
        targets = random.sample(targets, a.sample)
    print(f"{len(targets)} live blocks of classes matching {a.cls!r}")
    in_path = os.path.join(a.work, "owners-in.txt")
    out_path = os.path.join(a.work, "owners-out.tsv")
    with open(in_path, "w", encoding="ascii") as f:
        for s, n in targets:
            f.write(f"0x{s:X} {n}\n")
    subprocess.run([a.heapcensus, a.dump, "--refs-of", in_path, "--refs-out", out_path, "--regions", "0"],
                   capture_output=True, check=False)
    owners = collections.Counter()
    owned = set()
    with open(out_path, encoding="ascii") as f:
        for line in f:
            t, slot = (int(x, 16) for x in line.split("\t"))
            i = bisect.bisect_right(starts, slot) - 1
            if i >= 0 and starts[i] <= slot < starts[i] + sizes[i]:
                cls, _ = block_class(d, vt, starts[i])
                label = f"{cls or f'(no vftable, {sizes[i]} B block)'} @+0x{slot - starts[i]:X}"
            else:
                label = "(outside the heap: stack / global / mapped)"
            if any(x in label for x in a.exclude):
                continue
            owners[label] += 1
            owned.add(t)
    print(f"objects with at least one owner left after --exclude: {len(owned)} of {len(targets)}")
    for label, n in owners.most_common(a.top):
        print(f"{n:8}  {label[:200]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
