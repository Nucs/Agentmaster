"""Agentmaster leak hunt: read objects' text fields out of a full-memory minidump.

Why: heapcensus.exe says HOW MANY objects of a type a dump holds (e.g. 70,952 MenuFlyoutItems); this
says WHICH ones, by the strings they carry. A boxed string (`box_value(L"...")`, WinTypes'
`Windows::Foundation::Value<ValueScalar<HSTRING>>`), a XAML core object's text, an HSTRING held by
a C++/WinRT object - each points at UTF-16 text, and a histogram of those texts names the menu, the
tooltip or the card a leaked population belongs to.

Input: the dump plus a slots file written by `heapcensus --slots <vftable-substring> --slots-out <file>`
(one `0x<slot address>\t0x<vftable>` line per vptr slot of the matching type).

Commands (python -I dumpview.py <command> <dump> <slots-file> [options]):
    hist   for every slot, collect the strings that the qwords in [slot + lo, slot + hi) point at (an
           HSTRING, an xstring storage, or plain UTF-16), and print how often each string occurs.
           --lo/--hi bound the window (default 0 .. 0x200); --first keeps only the first string found
           per object (the most specific, usually its own text) instead of all of them.
    near   print, for --sample N objects spread over the slots file, every string found in the window
           with its offset from the slot - for working out which offset is the field of interest.
    read   hex + text dump of memory: `read <dump> <hex address> [bytes]` (no slots file).

Nothing here writes anything but stdout. The parser handles exactly the streams it needs
(Memory64ListStream, ModuleListStream) and refuses a dump without a Memory64 list.
"""

import argparse
import bisect
import collections
import mmap
import struct
import sys

STREAM_MODULE_LIST = 4
STREAM_MEMORY64_LIST = 9


class Dump:
    """A read-only view of a minidump: captured memory by virtual address, plus the module ranges.

    The file is memory-mapped, so a multi-GB dump costs address space, not RAM; every read is a slice
    of the mapping (bounded by the captured range it falls in).
    """

    def __init__(self, path):
        """Map the dump and index its memory ranges and modules.

        Args:
            path: the minidump file.

        Raises:
            ValueError: when the file is not a minidump or has no Memory64 list (not a full-memory dump).
            OSError: when the file cannot be opened or mapped.
        """
        self._f = open(path, "rb")
        self.m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        sig, _ver, nstreams, dir_rva = struct.unpack_from("<4sIII", self.m, 0)
        if sig != b"MDMP":
            raise ValueError("not a minidump")
        streams = {}
        for i in range(nstreams):
            stype, size, rva = struct.unpack_from("<III", self.m, dir_rva + i * 12)
            streams[stype] = (rva, size)
        if STREAM_MEMORY64_LIST not in streams:
            raise ValueError("no Memory64ListStream - take a full-memory dump")
        rva, _ = streams[STREAM_MEMORY64_LIST]
        count, base_rva = struct.unpack_from("<QQ", self.m, rva)
        starts, sizes, offsets = [], [], []
        off = base_rva
        for i in range(count):
            start, size = struct.unpack_from("<QQ", self.m, rva + 16 + i * 16)
            starts.append(start)
            sizes.append(size)
            offsets.append(off)
            off += size
        order = sorted(range(count), key=lambda k: starts[k])
        self.starts = [starts[k] for k in order]
        self.sizes = [sizes[k] for k in order]
        self.offsets = [offsets[k] for k in order]
        self.modules = []
        if STREAM_MODULE_LIST in streams:
            mrva, _ = streams[STREAM_MODULE_LIST]
            (nmod,) = struct.unpack_from("<I", self.m, mrva)
            for i in range(nmod):
                # MINIDUMP_MODULE is 108 bytes: BaseOfImage (8), SizeOfImage (4), CheckSum (4),
                # TimeDateStamp (4), ModuleNameRva (4) at +20, ...
                base, size = struct.unpack_from("<QI", self.m, mrva + 4 + i * 108)
                name_rva = struct.unpack_from("<I", self.m, mrva + 4 + i * 108 + 20)[0]
                (nlen,) = struct.unpack_from("<I", self.m, name_rva)
                name = self.m[name_rva + 4: name_rva + 4 + nlen].decode("utf-16-le", "replace")
                self.modules.append((base, base + size, name.rsplit("\\", 1)[-1]))
            self.modules.sort()

    def read(self, va, n):
        """Read n bytes of the dumped process's memory.

        Args:
            va: the virtual address.
            n: the byte count.

        Returns:
            The bytes, or None when [va, va+n) is not entirely inside one captured range.

        Raises:
            Nothing: an address outside every range is a None, never an exception.
        """
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0 or va + n > self.starts[i] + self.sizes[i]:
            return None
        off = self.offsets[i] + (va - self.starts[i])
        return self.m[off: off + n]

    def q(self, va):
        """Read one little-endian qword.

        Args:
            va: the virtual address of the qword.

        Returns:
            The value, or None when the 8 bytes are not captured.

        Raises:
            Nothing.
        """
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def module_of(self, va):
        """Name the module whose image holds an address.

        Args:
            va: any virtual address.

        Returns:
            The module's file leaf, or None for heap / stack / mapped memory (what object data is).

        Raises:
            Nothing.
        """
        i = bisect.bisect_right(self.modules, (va, float("inf"), "")) - 1
        if i >= 0 and self.modules[i][0] <= va < self.modules[i][1]:
            return self.modules[i][2]
        return None


def printable(s):
    """Judge whether a decoded string looks like real text rather than bytes that happen to decode.

    Args:
        s: the candidate string.

    Returns:
        True when it has no control characters (tab / CR / LF allowed), no lone surrogates, and at least
        a third of it is letters or digits.

    Raises:
        Nothing.
    """
    if not s or any(ord(c) < 0x20 and c not in "\t\r\n" for c in s):
        return False
    if any(0xD800 <= ord(c) <= 0xDFFF for c in s):
        return False
    return sum(c.isalnum() for c in s) >= max(1, len(s) // 3)


def utf16_at(d, p, maxchars=200, minchars=3):
    """Decode a NUL-terminated UTF-16 string stored at an address.

    Args:
        d: the Dump.
        p: the address.
        maxchars: the most characters read.
        minchars: the fewest accepted (shorter runs are too likely to be noise).

    Returns:
        The text, or None when there is no plausible string there.

    Raises:
        Nothing.
    """
    b = d.read(p, maxchars * 2) or d.read(p, 32)
    if not b:
        return None
    s = b.decode("utf-16-le", "replace").split("\x00", 1)[0]
    return s if len(s) >= minchars and printable(s) else None


def hstring_at(d, h):
    """Decode an HSTRING handle: combase's header is {u32 flags, u32 length, u64 padding, PCWSTR text}.

    Args:
        d: the Dump.
        h: the HSTRING value (the address of its header).

    Returns:
        The text, or None when the header is implausible (flags, length or text pointer).

    Raises:
        Nothing.
    """
    b = d.read(h, 24)
    if not b:
        return None
    flags, length, _p1, _p2, ref = struct.unpack("<IIIIQ", b)
    if flags > 0xFF or not (0 < length < 4096) or not ref:
        return None
    raw = d.read(ref, length * 2)
    if not raw:
        return None
    s = raw.decode("utf-16-le", "replace")
    return s if printable(s) else None


def xstring_at(d, p):
    """Decode a XAML core xstring storage: {const WCHAR* buffer; UINT32 count; ...}.

    Args:
        d: the Dump.
        p: the address of the storage block.

    Returns:
        The text, or None when the block is implausible.

    Raises:
        Nothing.
    """
    b = d.read(p, 16)
    if not b:
        return None
    buf, count = struct.unpack("<QI", b[:12])
    if not (0 < count < 4096) or not buf:
        return None
    raw = d.read(buf, count * 2)
    if not raw:
        return None
    s = raw.decode("utf-16-le", "replace")
    return s if printable(s) else None


def strings_in(d, slot, lo, hi):
    """Collect the text that an object's fields point at.

    The three decoders are tried in order (HSTRING, xstring, plain UTF-16); the first that succeeds wins
    for that qword. Pointers into module images are skipped: there they are vftables or string literals,
    not the object's own data. Odd values and the first 64 KB are skipped (not heap pointers).

    Args:
        d: the Dump.
        slot: the object's vptr slot (the window is relative to it).
        lo: the window's first offset (may be negative).
        hi: the window's end offset (exclusive).

    Returns:
        A list of (offset, kind, text) with kind "hs", "xs" or "u16".

    Raises:
        Nothing.
    """
    out = []
    for off in range(lo, hi, 8):
        p = d.q(slot + off)
        if not p or p & 1 or p < 0x10000 or d.module_of(p):
            continue
        for kind, fn in (("hs", hstring_at), ("xs", xstring_at), ("u16", utf16_at)):
            s = fn(d, p)
            if s:
                out.append((off, kind, s))
                break
    return out


def load_slots(path):
    """Read a heapcensus --slots-out file.

    Args:
        path: the file (lines of `0x<slot>\\t0x<vftable>`).

    Returns:
        A list of (slot, vftable) integer pairs, in file order.

    Raises:
        OSError: when the file cannot be read. ValueError: on a malformed number.
    """
    slots = []
    with open(path, encoding="ascii") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                slots.append((int(parts[0], 16), int(parts[1], 16)))
    return slots


def main(argv):
    """Dispatch the three commands (see the module docstring).

    Args:
        argv: the command-line arguments after the script name.

    Returns:
        The process exit code: 0 on success, 1 when a `read` address is not captured.

    Raises:
        ValueError / OSError from Dump() or load_slots() on a bad input file (left to surface as a traceback).
    """
    # The texts are any language (Hebrew titles, emoji glyphs); a cp1252 console would raise on them.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=["hist", "near", "read"])
    ap.add_argument("dump")
    ap.add_argument("arg", help="slots file (hist/near) or a hex address (read)")
    ap.add_argument("bytes", nargs="?", default="0x100")
    ap.add_argument("--lo", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--hi", type=lambda s: int(s, 0), default=0x200)
    ap.add_argument("--first", action="store_true")
    ap.add_argument("--sample", type=int, default=20)
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--vtable", type=lambda s: int(s, 0), default=0, help="only slots of this vftable")
    a = ap.parse_args(argv)
    d = Dump(a.dump)

    if a.command == "read":
        va, n = int(a.arg, 16), int(a.bytes, 0)
        b = d.read(va, n)
        if b is None:
            print("not captured")
            return 1
        for i in range(0, n, 16):
            chunk = b[i: i + 16]
            qs = " ".join(f"{struct.unpack_from('<Q', chunk, j)[0]:016X}" for j in range(0, len(chunk) - 7, 8))
            txt = chunk.decode("utf-16-le", "replace").replace("\n", " ")
            mods = [d.module_of(struct.unpack_from("<Q", chunk, j)[0]) for j in range(0, len(chunk) - 7, 8)]
            shown = "".join(c if c.isprintable() else "." for c in txt)
            print(f"{va + i:016X}  {qs:<34} |{shown}| {' '.join(m or '' for m in mods)}")
        return 0

    slots = load_slots(a.arg)
    if a.vtable:
        slots = [s for s in slots if s[1] == a.vtable]
    print(f"{len(slots)} slots; vftables: {collections.Counter(v for _, v in slots).most_common(5)}")
    if a.command == "near":
        step = max(1, len(slots) // max(1, a.sample))
        for slot, vt in slots[::step][: a.sample]:
            print(f"-- slot 0x{slot:X} (vftable 0x{vt:X})")
            for off, kind, s in strings_in(d, slot, a.lo, a.hi):
                print(f"   +0x{off:03X} {kind:3} {s[:150]!r}")
        return 0

    hist = collections.Counter()
    none = 0
    for slot, _vt in slots:
        found = strings_in(d, slot, a.lo, a.hi)
        if not found:
            none += 1
            continue
        if a.first:
            hist[found[0][2][:120]] += 1
        else:
            for s in {f[2][:120] for f in found}:
                hist[s] += 1
    print(f"objects with no string in the window: {none}")
    for s, n in hist.most_common(a.top):
        print(f"{n:8}  {s!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
