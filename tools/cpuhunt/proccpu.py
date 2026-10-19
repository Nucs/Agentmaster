"""Agentmaster CPU hunt: per-process CPU of the whole machine over a window, with a target's process tree.

Why: "Agentmaster is slow / uses CPU" first needs to know WHO uses the CPU: the app itself, the claude.exe
sessions it hosts, its ConPTY hosts and shells, short-lived helpers (hook forwarders), or something else on
the machine. Get-Process deltas miss the processes that start and end inside the window; this polls the
kernel's process list every --period seconds, so a process that lived 300 ms is still counted.

READ-ONLY: one NtQuerySystemInformation(SystemProcessInformation) call per poll; no process is opened,
nothing is suspended or written. Safe on production.

Caveats:
    - A process born inside the window counts from 0; one that ends counts until the last poll that saw it,
      so up to one period of its CPU can be missed.
    - The shell that runs this script is in the target's tree when the session runs inside the target
      (python.exe / bash.exe rows): subtract them.

Usage: python -I proccpu.py <target pid> <seconds> [--period 0.25]
"""

import argparse
import collections
import ctypes
import time

SYSTEM_PROCESS_INFORMATION = 5
STATUS_INFO_LENGTH_MISMATCH = 0xC0000004


def snapshot(ntdll):
    """Read every process's identity and CPU time right now.

    Args:
        ntdll: the loaded ntdll (ctypes.WinDLL).

    Returns:
        {pid: (image name, parent pid, user + kernel CPU in 100 ns units, create time, thread count)}.

    Raises:
        OSError: when the query keeps failing for a reason other than a too-small buffer.
    """
    size = 4 * 1024 * 1024
    for _ in range(8):
        buf = ctypes.create_string_buffer(size)
        ret = ctypes.c_ulong(0)
        st = ntdll.NtQuerySystemInformation(SYSTEM_PROCESS_INFORMATION, buf, size, ctypes.byref(ret))
        if st == 0:
            break
        if st & 0xFFFFFFFF == STATUS_INFO_LENGTH_MISMATCH:
            # The process list grew between the size probe and the copy: retry with room to spare.
            size = max(size * 2, ret.value + 65536)
            continue
        raise OSError(f"NtQuerySystemInformation 0x{st & 0xFFFFFFFF:X}")
    raw = buf.raw
    out = {}
    off = 0
    while True:
        # SYSTEM_PROCESS_INFORMATION (x64): NextEntryOffset +0, NumberOfThreads +4, CreateTime +32,
        # UserTime +40, KernelTime +48, ImageName (UNICODE_STRING: Length +56, Buffer +64),
        # UniqueProcessId +80, InheritedFromUniqueProcessId +88.
        nxt = int.from_bytes(raw[off:off + 4], "little")
        nthreads = int.from_bytes(raw[off + 4:off + 8], "little")
        create = int.from_bytes(raw[off + 32:off + 40], "little")
        user = int.from_bytes(raw[off + 40:off + 48], "little")
        kern = int.from_bytes(raw[off + 48:off + 56], "little")
        nlen = int.from_bytes(raw[off + 56:off + 58], "little")
        nptr = int.from_bytes(raw[off + 64:off + 72], "little")
        pid = int.from_bytes(raw[off + 80:off + 88], "little")
        ppid = int.from_bytes(raw[off + 88:off + 96], "little")
        # The name buffer points into our own copy of the list, so it is readable here.
        name = ctypes.wstring_at(nptr, nlen // 2) if nptr and nlen else ("System Idle" if pid == 0 else "?")
        out[pid] = (name, ppid, user + kern, create, nthreads)
        if not nxt:
            break
        off += nxt
    return out


def main():
    """Poll the process list for the window and print the CPU report.

    Returns:
        0.

    Raises:
        OSError: from snapshot() when the kernel query fails.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pid", type=int, help="the target process (its descendants are reported as its tree)")
    ap.add_argument("seconds", type=float, help="the window")
    ap.add_argument("--period", type=float, default=0.25, help="seconds between polls")
    a = ap.parse_args()
    ntdll = ctypes.WinDLL("ntdll")
    ntdll.NtQuerySystemInformation.restype = ctypes.c_long
    t0 = time.perf_counter()
    # (pid, create time) identifies one process even when a pid is reused inside the window.
    seen = {}  # (pid, create) -> [image, ppid, cpu at first sight, cpu at last sight, first poll, last poll]
    polls = 0
    cur = snapshot(ntdll)
    while True:
        for pid, (name, ppid, cpu, create, _n) in cur.items():
            k = (pid, create)
            if k not in seen:
                seen[k] = [name, ppid, cpu, cpu, polls, polls]
            else:
                seen[k][3] = cpu
                seen[k][5] = polls
        polls += 1
        if time.perf_counter() - t0 >= a.seconds:
            break
        time.sleep(a.period)
        cur = snapshot(ntdll)
    elapsed = time.perf_counter() - t0
    names = {pid: v[0] for (pid, _c), v in seen.items()}
    children = collections.defaultdict(set)
    for (pid, _c), v in seen.items():
        children[v[1]].add(pid)
    tree, stack = set(), [a.pid]
    while stack:
        p = stack.pop()
        if p not in tree:
            tree.add(p)
            stack.extend(children.get(p, ()))

    def pct(v):
        """CPU in 100 ns units over the window -> percent of ONE core."""
        return v / (elapsed * 1e7) * 100

    total = 0
    in_tree, in_tree_n = collections.Counter(), collections.Counter()
    rest, rest_n = collections.Counter(), collections.Counter()
    short, short_n = collections.Counter(), collections.Counter()
    for (pid, _c), (name, ppid, c0, c1, p0, p1) in seen.items():
        if pid == 0:
            continue
        born = p0 > 0
        used = c1 - (0 if born else c0)
        total += used
        key = name.lower()
        bucket, counts = (in_tree, in_tree_n) if pid in tree else (rest, rest_n)
        bucket[key] += used
        counts[key] += 1
        if born or p1 < polls - 1:
            k = (key, names.get(ppid, "?").lower(), "in the target's tree" if pid in tree else "elsewhere")
            short[k] += used
            short_n[k] += 1
    print(f"window {elapsed:.1f} s, {polls} polls; all processes except Idle: {pct(total):.0f}% of one core")
    print(f"\nthe target's tree (pid {a.pid} + descendants): {pct(sum(in_tree.values())):.1f}% of one core")
    for k, v in in_tree.most_common(15):
        print(f"  {pct(v):7.2f}%  {in_tree_n[k]:4} x {k}")
    print("\nthe rest of the machine (top 15):")
    for k, v in rest.most_common(15):
        print(f"  {pct(v):7.2f}%  {rest_n[k]:4} x {k}")
    print("\nprocesses that started or ended inside the window (image <- parent image):")
    for k, v in sorted(short.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {pct(v):7.2f}%  {short_n[k]:4} x {k[0]} <- {k[1]} ({k[2]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
