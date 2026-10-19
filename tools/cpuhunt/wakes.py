"""Agentmaster CPU hunt: the wake-up PATTERN of XAML's frame thread, and what moves with it.

Why: XAML draws a frame whenever something in any window changed (or an animation runs), and on a big
fleet each frame walks thousands of composition nodes (doc/agentmaster/UI_THREAD.md §13). A running
animation shows as steady 60 Hz runs; property changes show as irregular bursts. This samples every
thread's context-switch counter of the target every 20 ms and prints the frame thread's wake-ups per
100 ms, plus each busy thread's correlation with it per 100 ms (a thread whose work makes XAML draw
correlates positively).

READ-ONLY: NtQuerySystemInformation(SystemProcessInformation) only; nothing is opened or suspended.

The frame thread's id comes from `dotnet run tools/threadcensus.cs -- <pid> 20` (its row
"Windows.UI.Xaml.dll+0x6F59C0" on Windows 11 26200); the UI thread is the row starting in the exe.

Usage: python -I wakes.py <pid> <frame tid> [--seconds 20] [--top 12]
"""

import argparse
import collections
import ctypes
import math
import time


def thread_switches(ntdll, pid):
    """Read every thread's context-switch count of one process.

    Args:
        ntdll: the loaded ntdll (ctypes.WinDLL).
        pid: the target process id.

    Returns:
        {tid: context switches}; empty when the process is gone.

    Raises:
        OSError: when the kernel query fails.
    """
    size = 8 << 20
    buf = ctypes.create_string_buffer(size)
    ret = ctypes.c_ulong()
    st = ntdll.NtQuerySystemInformation(5, buf, size, ctypes.byref(ret))
    if st != 0:
        raise OSError(f"NtQuerySystemInformation 0x{st & 0xFFFFFFFF:X}")
    raw = buf.raw
    off = 0
    while True:
        nxt = int.from_bytes(raw[off:off + 4], "little")
        nt = int.from_bytes(raw[off + 4:off + 8], "little")
        if int.from_bytes(raw[off + 80:off + 88], "little") == pid:
            out = {}
            for i in range(nt):
                # SYSTEM_THREAD_INFORMATION entries follow the process entry (+256), 0x50 bytes each:
                # ClientId.UniqueThread +48, ContextSwitches +64.
                b = off + 256 + i * 0x50
                out[int.from_bytes(raw[b + 48:b + 56], "little")] = int.from_bytes(raw[b + 64:b + 68], "little")
            return out
        if not nxt:
            return {}
        off += nxt


def correlation(a, b):
    """Pearson correlation of two equally long series.

    Args:
        a: the first series.
        b: the second series.

    Returns:
        A value in [-1, 1]; 0 when either series is constant.

    Raises:
        Nothing.
    """
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))
    return num / den if den else 0.0


def main():
    """Sample, then print the frame thread's pattern and the correlations.

    Returns:
        0, or 2 when the frame thread is not a thread of the target.

    Raises:
        OSError: from thread_switches().
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pid", type=int)
    ap.add_argument("frame_tid", type=int)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()
    ntdll = ctypes.WinDLL("ntdll")
    ntdll.NtQuerySystemInformation.restype = ctypes.c_long
    prev = thread_switches(ntdll, a.pid)
    if a.frame_tid not in prev:
        print(f"tid {a.frame_tid} is not a thread of pid {a.pid}")
        return 2
    t0 = time.perf_counter()
    rows = []
    while time.perf_counter() - t0 < a.seconds:
        time.sleep(0.02)
        cur = thread_switches(ntdll, a.pid)
        rows.append((time.perf_counter() - t0, {tid: cur.get(tid, 0) - prev.get(tid, 0) for tid in cur}))
        prev = cur
    nb = int(a.seconds / 0.1)

    def series(tid):
        """Wake-ups of one thread per 100 ms bucket."""
        s = collections.Counter()
        for t, d in rows:
            s[min(int(t / 0.1), nb - 1)] += d.get(tid, 0)
        return [s[k] for k in range(nb)]

    fr = series(a.frame_tid)
    print(f"frame thread {a.frame_tid}: {sum(fr) / a.seconds:.1f} wake-ups/s; 100 ms buckets with none: "
          f"{sum(1 for x in fr if x == 0)} of {nb}")
    print("wake-ups per 100 ms (steady runs = an animation; bursts and gaps = property changes):")
    for i in range(0, nb, 50):
        print("  " + " ".join(f"{x:2}" for x in fr[i:i + 50]))
    tot = collections.Counter()
    for _t, d in rows:
        for tid, v in d.items():
            tot[tid] += v
    print("\nbusiest threads and their correlation with the frame thread per 100 ms:")
    for tid, n in tot.most_common(a.top + 1):
        if tid != a.frame_tid:
            print(f"  tid {tid:6}: {n / a.seconds:7.1f} wake-ups/s, correlation {correlation(fr, series(tid)):+.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
