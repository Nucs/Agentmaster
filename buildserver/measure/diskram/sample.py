"""Samples the disk-backed-RAM experiment every few seconds, from Windows' side and from the container's.

Each line: Windows' available RAM and free commit (GlobalMemoryStatusEx), vmmemWSL's private bytes (what Windows
charges for the whole WSL VM), and the container's memory cgroup (resident, swapped, peak, cap hits) plus how much of
the swap file on Q: is in use (probe.sh, run inside the buildserver distro). Rows are appended to a CSV on Q: (the
DISKRAM_CSV variable, default Q:\\tmp\\diskram-samples.csv: measurements never write into the repository on K:). The
run of 2026-10-29 is ../results-2026-10-29/diskram-samples.csv (DESIGN.md §16.14).

Tripwire: when Windows' free commit drops under 3 GB or its available RAM under 2.5 GB, the container is removed at once
and the script exits 3 (the experiment must never starve the owner's PC). Exits 2 when the container is gone or exited
(e.g. killed at its cap), 0 at the end of the window.

Usage (from Windows, after setup.sh started the container):
    python -I sample.py SECONDS [INTERVAL]
"""
import csv
import ctypes
import datetime
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "probe.sh")
CSV_PATH = os.environ.get("DISKRAM_CSV", r"Q:\tmp\diskram-samples.csv")
MIN_FREE_COMMIT_GB = 3.0
MIN_AVAIL_GB = 2.5
GB = 1024 ** 3


class MemoryStatusEx(ctypes.Structure):
    """Win32 MEMORYSTATUSEX: ullTotalPageFile is the commit limit, ullAvailPageFile the free commit."""

    _fields_ = [
        ("dwLength", ctypes.c_ulong),
        ("dwMemoryLoad", ctypes.c_ulong),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def windows_memory() -> tuple[float, float, float]:
    """Reads Windows' memory status.

    Returns:
        Available RAM, free commit and the commit limit, in GB.

    Raises:
        OSError: GlobalMemoryStatusEx failed.
    """
    status = MemoryStatusEx()
    status.dwLength = ctypes.sizeof(MemoryStatusEx)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise OSError("GlobalMemoryStatusEx failed")
    return status.ullAvailPhys / GB, status.ullAvailPageFile / GB, status.ullTotalPageFile / GB


def vmmem_private_gb() -> float | None:
    """Reads vmmemWSL's private bytes: what Windows charges to commit for the whole WSL VM.

    Returns:
        The value in GB, or None when psutil is missing or the process cannot be read.
    """
    try:
        import psutil  # Present on this PC (7.2.1); optional, so the sampler still runs without it.
    except ImportError:
        return None
    for proc in psutil.process_iter(["name"]):
        if proc.info.get("name") == "vmmemWSL":
            try:
                return proc.memory_info().private / GB
            except (psutil.AccessDenied, psutil.NoSuchProcess):
                return None
    return None


def probe() -> dict[str, str]:
    """Runs probe.sh in the buildserver distro.

    Returns:
        Its key=value lines; {"state": "probe failed: ..."} when wsl.exe failed or timed out.
    """
    with open(PROBE, "rb") as script:
        text = script.read().replace(b"\r\n", b"\n")
    try:
        done = subprocess.run(["wsl.exe", "-d", "buildserver", "-u", "root", "-e", "sh", "-s"], input=text,
                              capture_output=True, timeout=40)
    except subprocess.TimeoutExpired:
        return {"state": "probe failed: timeout"}
    facts: dict[str, str] = {}
    for line in done.stdout.decode("utf-8", "replace").splitlines():
        key, sep, value = line.partition("=")
        if sep:
            facts[key.strip()] = value.strip()
    return facts or {"state": f"probe failed: exit {done.returncode}"}


def remove_container() -> None:
    """Removes the experiment's container at once (the tripwire)."""
    subprocess.run(["wsl.exe", "-d", "buildserver", "-u", "root", "-e", "sh", "-c",
                    "PODMAN_IGNORE_CGROUPSV1_WARNING=1 podman rm -f -t 0 amb-exp-diskram"],
                   capture_output=True, timeout=60)


def gb_of(facts: dict[str, str], key: str, scale: float = 1.0) -> float | None:
    """Reads a byte (or scaled) count from the probe's facts as GB.

    Args:
        facts: the probe's facts.
        key: the key.
        scale: what one unit is in bytes (1024 for kB values).

    Returns:
        The value in GB, or None when absent or not a number.
    """
    value = facts.get(key, "")
    return int(value) * scale / GB if value.isdigit() else None


def fmt(value: float | None) -> str:
    """Formats a GB value for the line (or '-' when unknown)."""
    return "-" if value is None else f"{value:5.2f}"


def main() -> int:
    """Samples until the window ends, the tripwire fires, or the container is gone.

    Returns:
        0 at the end of the window, 2 when the container is gone or exited, 3 when the tripwire fired.
    """
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    end = time.monotonic() + seconds
    new_file = not os.path.exists(CSV_PATH)
    with open(CSV_PATH, "a", newline="") as out:
        writer = csv.writer(out)
        if new_file:
            writer.writerow(["time", "avail_gb", "free_commit_gb", "commit_limit_gb", "vmmem_private_gb", "state",
                             "cg_mem_gb", "cg_memsw_gb", "cg_swap_gb", "cg_peak_gb", "cg_failcnt", "swapfile_used_gb",
                             "wsl_avail_gb"])
        print("time      avail  freeCommit vmmemPriv | container: resident  swapped  peak  capHits  swapfile(Q:)")
        while True:
            now = datetime.datetime.now().strftime("%H:%M:%S")
            avail, free_commit, limit = windows_memory()
            vmmem = vmmem_private_gb()
            facts = probe()
            mem, memsw = gb_of(facts, "mem"), gb_of(facts, "memsw")
            swap = gb_of(facts, "total_swap")
            peak, swapfile = gb_of(facts, "peak"), gb_of(facts, "swapfile_used_kb", 1024)
            state = facts.get("state", "?")
            print(f"{now}  {fmt(avail)}  {fmt(free_commit)}     {fmt(vmmem)}   | {state:<22} {fmt(mem)}   {fmt(swap)}  "
                  f"{fmt(peak)}  {facts.get('failcnt', '-'):>7}  {fmt(swapfile)}", flush=True)
            writer.writerow([now, avail, free_commit, limit, vmmem, state, mem, memsw, swap, peak,
                             facts.get("failcnt", ""), swapfile, gb_of(facts, "wsl_avail_kb", 1024)])
            out.flush()
            if free_commit < MIN_FREE_COMMIT_GB or avail < MIN_AVAIL_GB:
                remove_container()
                print(f"TRIPWIRE: free commit {free_commit:.2f} GB / available {avail:.2f} GB: container removed",
                      flush=True)
                return 3
            if state.startswith("gone") or state.startswith("exited"):
                print(f"container ended: {state}", flush=True)
                return 2
            if time.monotonic() + interval > end:
                return 0
            time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())
