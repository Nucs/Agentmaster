"""Agentmaster leak hunt: walk the Windows SEGMENT HEAP of a full-memory dump - busy blocks only.

Why: a raw vftable scan (heapcensus.exe) also counts FREED objects whose bytes the heap has not reused
yet, so it cannot say how many objects are alive. The heap's own metadata can: every LFH block has a
busy bit, every VS chunk an "allocated" byte, every backend page range an "allocated" flag. Walking
them yields the exact set of live heap blocks with their sizes; reading each block's first qword then
gives a census of LIVE objects by type (and their bytes), and the blocks without a known vftable are
summarized by size. Agentmaster is a packaged app, which opts into the segment heap, so its process
heap and the CRT heap are segment heaps (signature 0xDDEEDDEE).

The layouts are Windows 11 26200's, read from ntdll's public PDB (diatype.exe udt _SEGMENT_HEAP etc.):
    _SEGMENT_HEAP: Signature +0x10, LargeAllocMetadata +0x48, SegContexts[2] +0x140 (0xC0 each)
    _HEAP_SEG_CONTEXT: SegmentMask +0x00, UnitShift +0x08, FirstDescriptorIndex +0x0A, SegmentListHead +0x48
    _HEAP_PAGE_SEGMENT: ListEntry +0x00, Signature +0x10, DescArray[256] of 0x20-byte descriptors
    _HEAP_PAGE_RANGE_DESCRIPTOR: RangeFlags +0x18, UnitOffset +0x1A, UnitSize +0x1F
    _HEAP_LFH_SUBSEGMENT: BlockCount +0x22, BlockOffsets.EncodedData +0x28, BlockBitmap +0x40 (2 bits/block)
    _HEAP_VS_SUBSEGMENT (0x28): Size +0x20; chunks follow, each a 16-byte encoded _HEAP_VS_CHUNK_HEADER
    _RTLP_HP_HEAP_GLOBALS (ntdll!RtlpHpHeapGlobals, RVA passed in): HeapKey +0x00, LfhKey +0x08
The encodings (LFH offsets: data ^ LfhKey ^ subsegment>>12; VS header: bits ^ chunk ^ HeapKey) are the
documented segment-heap ones (Yason, "Windows 10 Segment Heap Internals"); every decoded value is
sanity-checked, and the counts of rejected structures are printed so a layout drift is visible.

Usage (python -I segheap.py ...):
    segheap.py <dump> --globals-rva 0x1CCEC0 [--census <heapcensus.tsv>] [--top N] [--blocks-out <file>]
    --census    a heapcensus --out TSV: its vftables (module + rva) are matched against each busy block's
                first qword, giving live instances + bytes per type; unmatched blocks are grouped by size.
    --blocks-out  write every busy block as "address<TAB>size<TAB>kind" (for ownerchain.py and follow-ups).
    --live-out    with --census: write every live class (count, bytes) and every unmatched block size as
                  TSV; `livediff.py old new` then ranks what grew between two dumps.
"""

import argparse
import bisect
import collections
import mmap
import struct
import sys

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY64_LIST = 9
SEGMENT_HEAP_SIGNATURE = 0xDDEEDDEE
SEGMENT_SIGNATURE_XOR = 0xA2E64EADA2E64EAD


class Dump:
    """A read-only view of a full-memory minidump: memory by virtual address, modules, thread TEBs."""

    def __init__(self, path):
        """Map the dump and index the streams this walker needs.

        Args:
            path: the minidump file.

        Raises:
            ValueError: when the file is not a minidump or lacks a Memory64 list.
            OSError: when the file cannot be opened or mapped.
        """
        self._f = open(path, "rb")
        self.m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        sig, _v, n, dir_rva = struct.unpack_from("<4sIII", self.m, 0)
        if sig != b"MDMP":
            raise ValueError("not a minidump")
        streams = {}
        for i in range(n):
            t, size, rva = struct.unpack_from("<III", self.m, dir_rva + i * 12)
            streams[t] = (rva, size)
        if STREAM_MEMORY64_LIST not in streams:
            raise ValueError("no Memory64ListStream")
        rva, _ = streams[STREAM_MEMORY64_LIST]
        count, base_rva = struct.unpack_from("<QQ", self.m, rva)
        rows, off = [], base_rva
        for i in range(count):
            start, size = struct.unpack_from("<QQ", self.m, rva + 16 + i * 16)
            rows.append((start, size, off))
            off += size
        rows.sort()
        self.starts = [r[0] for r in rows]
        self.sizes = [r[1] for r in rows]
        self.offsets = [r[2] for r in rows]
        self.modules = {}
        rva, _ = streams[STREAM_MODULE_LIST]
        (nmod,) = struct.unpack_from("<I", self.m, rva)
        for i in range(nmod):
            base, size = struct.unpack_from("<QI", self.m, rva + 4 + i * 108)
            name_rva = struct.unpack_from("<I", self.m, rva + 4 + i * 108 + 20)[0]
            (nlen,) = struct.unpack_from("<I", self.m, name_rva)
            name = self.m[name_rva + 4: name_rva + 4 + nlen].decode("utf-16-le", "replace").rsplit("\\", 1)[-1]
            self.modules[name.lower()] = (base, size)
        self.tebs = []
        if STREAM_THREAD_LIST in streams:
            rva, _ = streams[STREAM_THREAD_LIST]
            (nthr,) = struct.unpack_from("<I", self.m, rva)
            for i in range(nthr):
                # MINIDUMP_THREAD: ThreadId, SuspendCount, PriorityClass, Priority (4 each), Teb (8) at +16.
                (teb,) = struct.unpack_from("<Q", self.m, rva + 4 + i * 48 + 16)
                self.tebs.append(teb)

    def read(self, va, n):
        """Read n bytes of the dumped process's memory.

        Args:
            va: the virtual address.
            n: the byte count.

        Returns:
            The bytes, or None when [va, va+n) is not inside one captured range.

        Raises:
            Nothing.
        """
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0 or va + n > self.starts[i] + self.sizes[i]:
            return None
        o = self.offsets[i] + va - self.starts[i]
        return self.m[o: o + n]

    def q(self, va):
        """Read one qword, or None when not captured.

        Args:
            va: the address.

        Returns:
            The little-endian value or None.

        Raises:
            Nothing.
        """
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if b else None


class Walk:
    """The result of walking every segment heap: busy blocks plus counters of what was skipped."""

    def __init__(self):
        """Start empty: blocks is a list of (address, size, kind) with kind "lfh" / "vs" / "range"."""
        self.blocks = []
        self.stats = collections.Counter()
        self.flags_seen = collections.Counter()


def walk_lfh(d, w, sub, range_size, lfh_key):
    """Record the busy blocks of one LFH subsegment.

    Args:
        d: the Dump.
        w: the Walk to append to.
        sub: the subsegment's address (its page range start).
        range_size: the page range's size in bytes (bounds the blocks).
        lfh_key: RtlpHpHeapGlobals.LfhKey.

    Returns:
        Nothing; rejects are counted in w.stats["lfh-bad"].

    Raises:
        Nothing.
    """
    hdr = d.read(sub, 0x48)
    if not hdr:
        w.stats["lfh-unreadable"] += 1
        return
    block_count = struct.unpack_from("<H", hdr, 0x22)[0]
    enc = struct.unpack_from("<I", hdr, 0x28)[0]
    data = enc ^ (lfh_key & 0xFFFFFFFF) ^ ((sub >> 12) & 0xFFFFFFFF)
    block_size, first = data & 0xFFFF, data >> 16
    if not block_size or block_size & 7 or first < 0x48 or first + block_count * block_size > range_size:
        w.stats["lfh-bad"] += 1
        return
    bitmap = d.read(sub + 0x40, (block_count * 2 + 7) // 8)
    if bitmap is None:
        w.stats["lfh-unreadable"] += 1
        return
    bits = int.from_bytes(bitmap, "little")
    base = sub + first
    for i in range(block_count):
        if (bits >> (2 * i)) & 1:
            w.blocks.append((base + i * block_size, block_size, "lfh"))
    w.stats["lfh-subsegments"] += 1


def walk_vs(d, w, sub, range_size, heap_key):
    """Record the allocated chunks of one VS (variable-size) subsegment.

    Args:
        d: the Dump.
        w: the Walk to append to.
        sub: the subsegment's address.
        range_size: the page range's size in bytes (bounds the walk).
        heap_key: RtlpHpHeapGlobals.HeapKey (VS chunk headers are XORed with it and their own address).

    Returns:
        Nothing; a chunk whose decoded size is impossible ends that subsegment's walk (w.stats["vs-bad"]).

    Raises:
        Nothing.
    """
    end = sub + range_size
    chunk = sub + 0x30
    while chunk + 16 <= end:
        raw = d.q(chunk)
        if raw is None:
            w.stats["vs-unreadable"] += 1
            return
        bits = raw ^ chunk ^ heap_key
        size = ((bits >> 16) & 0xFFFF) * 16
        allocated = (bits >> 48) & 0xFF
        if size < 16 or chunk + size > end:
            w.stats["vs-bad"] += 1
            return
        if allocated:
            w.blocks.append((chunk + 16, size - 16, "vs"))
        chunk += size
    w.stats["vs-subsegments"] += 1


def walk_segment_heap(d, w, heap, heap_key, lfh_key):
    """Walk both segment contexts of one segment heap.

    Args:
        d: the Dump.
        w: the Walk to append to.
        heap: the _SEGMENT_HEAP address.
        heap_key: RtlpHpHeapGlobals.HeapKey.
        lfh_key: RtlpHpHeapGlobals.LfhKey.

    Returns:
        Nothing.

    Raises:
        Nothing; unreadable or mis-signed segments are counted and skipped.
    """
    for ctx_i in range(2):
        ctx = heap + 0x140 + ctx_i * 0xC0
        b = d.read(ctx, 0x60)
        if not b:
            continue
        unit_shift = b[0x08]
        first_desc = b[0x0A]
        head = ctx + 0x48
        seg = d.q(head)
        seen = 0
        while seg and seg != head and seen < 100000:
            seen += 1
            sig = d.q(seg + 0x10)
            # Windows 11 26200 signs a segment as segment ^ context ^ HeapKey (measured on production's dump:
            # 2,345 of 2,345 segments); the 2016 paper's form adds a constant and XORs the heap instead.
            # Either is accepted, so a drift between builds shows up as "segment-bad-signature".
            if sig is None or (sig ^ seg ^ ctx ^ heap_key != 0 and sig ^ seg ^ heap ^ heap_key != SEGMENT_SIGNATURE_XOR):
                w.stats["segment-bad-signature"] += 1
            else:
                w.stats[f"segments-ctx{ctx_i}"] += 1
                descs = d.read(seg, 256 * 0x20)
                i = first_desc
                while descs and i < 256:
                    flags = descs[i * 0x20 + 0x18]
                    unit_size = descs[i * 0x20 + 0x1F]
                    w.flags_seen[(ctx_i, flags)] += 1
                    # Measured on 26200 (segdbg2.py): the FIRST descriptor of an allocated range carries 0x08,
                    # the range's other descriptors 0x01 (allocated) with a running UnitOffset. 0x0B = an LFH
                    # subsegment (its header decodes with the LFH key), 0x0F = a VS subsegment (its Signature
                    # equals Size ^ 0x2BED). The 2016 paper's bit names (LFH = 0x01, ALLOCATED = 0x04) do not
                    # hold on this build, so the type is told apart by the 0x04 bit alone and each header is
                    # still validated before it is trusted.
                    if flags & 0x08 and unit_size:
                        start = seg + (i << unit_shift)
                        size = unit_size << unit_shift
                        if flags & 0x04:
                            walk_vs(d, w, start, size, heap_key)
                        else:
                            walk_lfh(d, w, start, size, lfh_key)
                        i += unit_size
                    else:
                        i += 1
            seg = d.q(seg)


def load_vtables(d, tsv):
    """Map absolute vftable addresses to (module, class name) from a heapcensus TSV.

    Args:
        d: the Dump (for the module bases).
        tsv: the heapcensus --out file (count, refd, module, rva[, block], name).

    Returns:
        A dict va -> (module, name).

    Raises:
        OSError: when the file cannot be read.
    """
    out = {}
    with open(tsv, encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\n").split("\t")
        name_col = header.index("name")
        for line in f:
            p = line.rstrip("\n").split("\t")
            mod = d.modules.get(p[2].lower())
            if mod:
                out[mod[0] + int(p[3], 16)] = (p[2], p[name_col])
    return out


def class_key(name):
    """Fold a vftable display name to its class (text between "const " and "::`vftable'").

    Args:
        name: the display name (folded names joined by " | ": the first is used).

    Returns:
        The class name.

    Raises:
        Nothing.
    """
    first = name.split(" | ", 1)[0]
    if first.startswith("const "):
        first = first[6:]
    cut = first.find("::`vftable'")
    return first[:cut] if cut >= 0 else first


def main(argv):
    """Walk the dump's segment heaps and report live blocks (and, with --census, live objects by type).

    Args:
        argv: the arguments after the script name.

    Returns:
        0 on success, 2 when the heap globals cannot be read.

    Raises:
        ValueError / OSError from Dump() on a bad dump.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump")
    ap.add_argument("--globals-rva", type=lambda s: int(s, 0), required=True,
                    help="ntdll!RtlpHpHeapGlobals RVA (from the dump's ntdll PDB)")
    ap.add_argument("--census")
    ap.add_argument("--top", type=int, default=80)
    ap.add_argument("--blocks-out")
    ap.add_argument("--live-out", help="with --census: write every live class and unmatched size as TSV")
    a = ap.parse_args(argv)
    d = Dump(a.dump)
    ntdll = d.modules.get("ntdll.dll")
    globals_va = ntdll[0] + a.globals_rva
    heap_key, lfh_key = d.q(globals_va), d.q(globals_va + 8)
    if heap_key is None or lfh_key is None:
        print("cannot read RtlpHpHeapGlobals")
        return 2
    peb = d.q(d.tebs[0] + 0x60)
    nheaps = struct.unpack("<I", d.read(peb + 0xE8, 4))[0]
    heaps_arr = d.q(peb + 0xF0)
    w = Walk()
    for i in range(nheaps):
        h = d.q(heaps_arr + i * 8)
        sig = struct.unpack("<I", d.read(h + 0x10, 4))[0] if d.read(h + 0x10, 4) else 0
        kind = "segment" if sig == SEGMENT_HEAP_SIGNATURE else "nt"
        before = len(w.blocks)
        if kind == "segment":
            walk_segment_heap(d, w, h, heap_key, lfh_key)
        n = len(w.blocks) - before
        size = sum(b[1] for b in w.blocks[before:])
        print(f"heap[{i}] 0x{h:X} {kind:7}: {n:9} busy blocks, {size / 1048576:9.1f} MB")
    print("walk stats:", dict(w.stats))
    print("range flags seen (ctx, flags) -> descriptors:", dict(sorted(w.flags_seen.items())))
    kinds = collections.Counter()
    for _, size, kind in w.blocks:
        kinds[kind] += size
    print("busy bytes by kind:", {k: f"{v / 1048576:.1f} MB" for k, v in kinds.items()})

    if a.blocks_out:
        with open(a.blocks_out, "w", encoding="ascii") as f:
            for addr, size, kind in w.blocks:
                f.write(f"0x{addr:X}\t{size}\t{kind}\n")

    if a.census:
        vt = load_vtables(d, a.census)
        per_class = collections.defaultdict(lambda: [0, 0])
        unknown_by_size = collections.Counter()
        unknown_bytes = 0
        for addr, size, _kind in w.blocks:
            v = d.q(addr)
            hit = vt.get(v)
            if hit is None:
                # A C++/WinRT object allocated with a leading header puts its vptr one or two qwords in.
                for off in (8, 16):
                    hit = vt.get(d.q(addr + off))
                    if hit:
                        break
            if hit:
                rec = per_class[(hit[0], class_key(hit[1]))]
                rec[0] += 1
                rec[1] += size
            else:
                unknown_by_size[size] += 1
                unknown_bytes += size
        rows = sorted(per_class.items(), key=lambda kv: -kv[1][1])
        known = sum(r[1][1] for r in rows)
        if a.live_out:
            # Every live class plus the unmatched blocks by size, in one TSV, so two dumps can be diffed
            # with censustool-style tooling (kind "class" or "size").
            with open(a.live_out, "w", encoding="utf-8") as f:
                f.write("kind\tcount\tbytes\tmodule\tname\n")
                for (mod, cls), (cnt, byts) in rows:
                    f.write(f"class\t{cnt}\t{byts}\t{mod}\t{cls}\n")
                for size, cnt in unknown_by_size.items():
                    f.write(f"size\t{cnt}\t{cnt * size}\t-\t{size}\n")
        print(f"\nlive blocks with a known vftable: {sum(r[1][0] for r in rows)} blocks, {known / 1048576:.1f} MB; "
              f"without: {sum(unknown_by_size.values())} blocks, {unknown_bytes / 1048576:.1f} MB")
        print(f"top {a.top} live classes by bytes:")
        for (mod, cls), (cnt, byts) in rows[: a.top]:
            print(f"{byts / 1048576:8.1f} MB {cnt:9}  {mod:22} {cls[:160]}")
        print("\nblocks without a known vftable, by size (top 40 by bytes):")
        for size, cnt in sorted(unknown_by_size.items(), key=lambda kv: -kv[0] * kv[1])[:40]:
            print(f"  {cnt:9} x {size:8} = {cnt * size / 1048576:8.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
