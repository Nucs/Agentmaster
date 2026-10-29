"""Measures the WSL VM's swap traffic (pswpin / pswpout from /proc/vmstat) over a window: is the capped test VM
thrashing, or idle? Pages are 4 KiB.

Usage: python -I swaprate.py SECONDS
"""
import subprocess
import sys
import time


def counters() -> dict[str, int]:
    """Reads pswpin and pswpout from the buildserver distro's /proc/vmstat.

    Returns:
        The two counters (pages since the WSL VM booted).

    Raises:
        subprocess.CalledProcessError: wsl.exe failed.
    """
    out = subprocess.run(["wsl.exe", "-d", "buildserver", "-u", "root", "-e", "grep", "-E", "^pswp(in|out) ",
                          "/proc/vmstat"], capture_output=True, timeout=30, check=True).stdout.decode()
    return {k: int(v) for k, v in (line.split() for line in out.splitlines())}


if __name__ == "__main__":
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    first = counters()
    start = time.monotonic()
    time.sleep(seconds)
    second = counters()
    elapsed = time.monotonic() - start
    for key in ("pswpin", "pswpout"):
        pages = second[key] - first[key]
        print(f"{key}: {pages} pages in {elapsed:.0f} s = {pages * 4 / 1024 / elapsed:.2f} MB/s")
