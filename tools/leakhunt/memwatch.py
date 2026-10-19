"""Agentmaster leak hunt: sample a RUNNING process's memory counters into a CSV, read-only.

Why this exists: a leak is GROWTH over time, and Agentmaster logs no memory figures of its own, so a
single reading (or a single dump) cannot say whether the private bytes are growing. This sampler
records the counters every N seconds while the app keeps running, so a slope can be read off later
(and lined up with the hooks.log timeline of what the user did).

It only ever opens the target with PROCESS_QUERY_LIMITED_INFORMATION: nothing is suspended, read
from the address space, or written. The cost to the target is one kernel query per counter per tick.

Usage (Python 3.8+, no third-party packages; run with -I so nothing next to it is imported):
    python -I memwatch.py <pid> <out.csv> [--interval 60] [--hours 6]

Columns: utc time, local time, private bytes (commit charge), working set, private working set,
paged / non-paged pool quota, handle count, thread count, GDI and USER objects (current + peak),
system commit total / limit. The file is appended and flushed every tick, so a reader can tail it
while the sampler runs; the sampler ends by itself when the process exits or the time is up.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import os
import sys
import time

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPPROCESS = 0x00000002
GR_GDIOBJECTS, GR_USEROBJECTS, GR_GDIOBJECTS_PEAK, GR_USEROBJECTS_PEAK = 0, 1, 2, 4
STILL_ACTIVE = 259


class PROCESS_MEMORY_COUNTERS_EX2(ctypes.Structure):
    """The psapi counters block; the _EX2 form adds PrivateWorkingSetSize + SharedCommitUsage."""

    _fields_ = [
        ("cb", wt.DWORD),
        ("PageFaultCount", wt.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
        ("PrivateWorkingSetSize", ctypes.c_size_t),
        ("SharedCommitUsage", ctypes.c_ulonglong),
    ]


class PERFORMANCE_INFORMATION(ctypes.Structure):
    """GetPerformanceInfo's block; only the commit totals (in pages) are used."""

    _fields_ = [
        ("cb", wt.DWORD),
        ("CommitTotal", ctypes.c_size_t),
        ("CommitLimit", ctypes.c_size_t),
        ("CommitPeak", ctypes.c_size_t),
        ("PhysicalTotal", ctypes.c_size_t),
        ("PhysicalAvailable", ctypes.c_size_t),
        ("SystemCache", ctypes.c_size_t),
        ("KernelTotal", ctypes.c_size_t),
        ("KernelPaged", ctypes.c_size_t),
        ("KernelNonpaged", ctypes.c_size_t),
        ("PageSize", ctypes.c_size_t),
        ("HandleCount", wt.DWORD),
        ("ProcessCount", wt.DWORD),
        ("ThreadCount", wt.DWORD),
    ]


class PROCESSENTRY32W(ctypes.Structure):
    """Toolhelp's process row; cntThreads is the one field read (a thread count with no extra rights)."""

    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE


def thread_count(pid):
    """Count the target's threads through one Toolhelp process snapshot.

    Args:
        pid: the target process id.

    Returns:
        The thread count, or -1 when the snapshot failed or the pid is not in it (the process exited).

    Raises:
        Nothing: every Win32 failure maps to -1.
    """
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == wt.HANDLE(-1).value:
        return -1
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.th32ProcessID == pid:
                return entry.cntThreads
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
        return -1
    finally:
        kernel32.CloseHandle(snap)


def sample(handle, pid):
    """Read one row of counters for the open process handle.

    Args:
        handle: a PROCESS_QUERY_LIMITED_INFORMATION handle to the target.
        pid: the same target's id (for the Toolhelp thread count).

    Returns:
        A list of column values in the CSV's order, or None when the process has exited (so the caller stops).

    Raises:
        OSError: when GetProcessMemoryInfo fails on a live process (an unexpected access problem —
        surfacing it beats writing a row of zeros that would read as a memory drop).
    """
    code = wt.DWORD()
    if kernel32.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value != STILL_ACTIVE:
        return None
    pmc = PROCESS_MEMORY_COUNTERS_EX2()
    pmc.cb = ctypes.sizeof(pmc)
    if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(pmc), pmc.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    handles = wt.DWORD()
    kernel32.GetProcessHandleCount(handle, ctypes.byref(handles))
    perf = PERFORMANCE_INFORMATION()
    perf.cb = ctypes.sizeof(perf)
    psapi.GetPerformanceInfo(ctypes.byref(perf), perf.cb)
    now = datetime.datetime.now()
    return [
        datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        now.strftime("%Y-%m-%d %H:%M:%S"),
        pmc.PrivateUsage,
        pmc.WorkingSetSize,
        pmc.PrivateWorkingSetSize,
        pmc.QuotaPagedPoolUsage,
        pmc.QuotaNonPagedPoolUsage,
        handles.value,
        thread_count(pid),
        user32.GetGuiResources(handle, GR_GDIOBJECTS),
        user32.GetGuiResources(handle, GR_GDIOBJECTS_PEAK),
        user32.GetGuiResources(handle, GR_USEROBJECTS),
        user32.GetGuiResources(handle, GR_USEROBJECTS_PEAK),
        perf.CommitTotal * perf.PageSize,
        perf.CommitLimit * perf.PageSize,
    ]


HEADER = [
    "utc", "local", "private_bytes", "working_set", "private_ws", "paged_pool", "nonpaged_pool",
    "handles", "threads", "gdi", "gdi_peak", "user", "user_peak", "sys_commit", "sys_commit_limit",
]


def main(argv):
    """Parse the arguments and sample until the process exits or the time runs out.

    Args:
        argv: the command-line arguments after the script name.

    Returns:
        The process exit code: 0 when sampling ended normally, 2 when the target could not be opened.

    Raises:
        Nothing on the normal paths; an OSError from sample() ends the run with a traceback on purpose.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pid", type=int)
    ap.add_argument("out")
    ap.add_argument("--interval", type=float, default=60.0, help="seconds between samples (default 60)")
    ap.add_argument("--hours", type=float, default=6.0, help="stop after this many hours (default 6)")
    args = ap.parse_args(argv)

    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, args.pid)
    if not handle:
        print(f"cannot open pid {args.pid}: {ctypes.WinError(ctypes.get_last_error())}", file=sys.stderr)
        return 2
    deadline = time.monotonic() + args.hours * 3600.0
    new_file = not os.path.exists(args.out) or os.path.getsize(args.out) == 0
    with open(args.out, "a", encoding="utf-8", newline="") as f:
        if new_file:
            f.write(",".join(HEADER) + "\n")
            f.flush()
        try:
            while time.monotonic() < deadline:
                row = sample(handle, args.pid)
                if row is None:
                    f.write(f"# pid {args.pid} exited at {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
                    break
                f.write(",".join(str(v) for v in row) + "\n")
                f.flush()
                time.sleep(args.interval)
        finally:
            kernel32.CloseHandle(handle)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
