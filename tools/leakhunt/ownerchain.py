"""Agentmaster leak hunt: walk "who owns this?" up from one heap block, level by level.

Why: a census names a big population (e.g. 25,634 DirectWrite text analyzers holding 16 KB each);
the question that leads to a fix is which object of OURS keeps them alive. Each level here finds the
busy heap block that contains the address, names it by the vftable in its first qwords (from a
heapcensus TSV, plus heapcensus --symat for vftables that are not public ??_7 symbols), and asks
heapcensus --findref for the slots that point into that block; the referrer's block is the next level.

Usage (python -I ownerchain.py ...):
    ownerchain.py <dump> <blocks.tsv> <census.tsv> <hex address> [--levels 8] [--heapcensus <exe>]
                  [--sym <dir> ...]
    blocks.tsv  segheap.py --blocks-out (address, size, kind of every busy block)
    census.tsv  heapcensus --out (for the vftable names)
The walk follows the FIRST referrer outside the block itself at each level (printing up to 6 others),
so it follows one ownership path, not all of them.
"""

import argparse
import bisect
import re
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
import segheap  # noqa: E402


def load_blocks(path):
    """Read busy blocks sorted by address.

    Args:
        path: a segheap.py --blocks-out file.

    Returns:
        Two parallel lists (starts, sizes), sorted by start.

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


def block_of(starts, sizes, va):
    """Find the busy block that contains an address.

    Args:
        starts: sorted block starts.
        sizes: the matching sizes.
        va: the address.

    Returns:
        (start, size) or None when the address is in no busy heap block (a stack, a global, a mapping).

    Raises:
        Nothing.
    """
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0 and starts[i] <= va < starts[i] + sizes[i]:
        return starts[i], sizes[i]
    return None


def name_block(d, vtables, start):
    """Name a block by the first known vftable among its first four qwords.

    Args:
        d: the segheap.Dump.
        vtables: va -> (module, name) from the census TSV.
        start: the block start.

    Returns:
        A short description: "<module> <class> @+<offset>", or the module + RVA of the first qword that
        points into an image (to look up with heapcensus --symat), or "?".

    Raises:
        Nothing.
    """
    for off in (0, 8, 16, 24):
        v = d.q(start + off)
        if v in vtables:
            mod, name = vtables[v]
            return f"{mod} {segheap.class_key(name)[:150]} @+{off}"
    for off in (0, 8, 16, 24):
        v = d.q(start + off)
        for mod, (base, size) in d.modules.items():
            if v and base <= v < base + size:
                return f"(unknown vftable) {mod}+0x{v - base:X} @+{off}"
    return "?"


def find_refs(exe, dump, start, size):
    """Ask heapcensus for every slot that points into [start, start + size).

    Args:
        exe: heapcensus.exe's path.
        dump: the dump path.
        start: the block start.
        size: the block size.

    Returns:
        A list of slot addresses (ints), in heapcensus's order (at most 200).

    Raises:
        OSError: when heapcensus cannot be started.
    """
    out = subprocess.run([exe, dump, "--findref", hex(start), "--span", str(size), "--regions", "0"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    return [int(m.group(1), 16) for m in re.finditer(r"slot 0x([0-9A-F]+) =", out)]


def main(argv):
    """Walk the ownership chain from one address and print each level.

    Args:
        argv: the arguments after the script name.

    Returns:
        0.

    Raises:
        OSError / ValueError from the inputs.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump")
    ap.add_argument("blocks")
    ap.add_argument("census")
    ap.add_argument("address", type=lambda s: int(s, 16))
    ap.add_argument("--levels", type=int, default=8)
    ap.add_argument("--heapcensus", default=__file__.rsplit("\\", 1)[0] + "\\heapcensus.exe")
    a = ap.parse_args(argv)
    d = segheap.Dump(a.dump)
    vtables = segheap.load_vtables(d, a.census)
    starts, sizes = load_blocks(a.blocks)
    va = a.address
    for level in range(a.levels):
        blk = block_of(starts, sizes, va)
        if not blk:
            print(f"[{level}] 0x{va:X} is not inside a busy heap block (stack / global / mapped) - stop")
            return 0
        start, size = blk
        print(f"[{level}] block 0x{start:X} size {size}: {name_block(d, vtables, start)}")
        refs = [r for r in find_refs(a.heapcensus, a.dump, start, size) if not start <= r < start + size]
        if not refs:
            print("     no referrer outside the block - stop (a root, or held only by itself)")
            return 0
        for r in refs[:6]:
            rb = block_of(starts, sizes, r)
            where = f"block 0x{rb[0]:X}+0x{r - rb[0]:X} ({name_block(d, vtables, rb[0])})" if rb else "outside the heap"
            print(f"     <- slot 0x{r:X} in {where}")
        va = refs[0]
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
