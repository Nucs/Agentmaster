"""Does a flush stall a concurrent append? Win32 serializes synchronous I/O per FILE OBJECT (handle), so a FlushFileBuffers
on the writer's own handle should block its next WriteFile, while a flush through a SECOND handle to the same file should not.

Thread A appends one 2 KB line every ~1 ms and records each os.write's latency; thread B flushes in a loop (os.fsync =
FlushFileBuffers) - through A's own fd, then through a separately opened fd. Prints append latency percentiles per mode.
Writes only inside the folder given as argv[1].
"""
import os
import sys
import threading
import time

folder = sys.argv[1]
os.makedirs(folder, exist_ok=True)
LINE = (b'{"op":"put","id":1,"params":{"text":"' + b"x" * 2000 + b'"}}\n')


def run(mode, seconds=4.0):
    path = os.path.join(folder, f"contention_{mode}.jsonl")
    if os.path.exists(path):
        os.remove(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_BINARY)
    flush_fd = fd if mode == "same_handle" else os.open(path, os.O_WRONLY | os.O_BINARY)
    stop = threading.Event()
    flushes = [0]

    def flusher():
        while not stop.is_set():
            if mode != "no_flusher":
                os.fsync(flush_fd)
                flushes[0] += 1
            time.sleep(0.002)

    th = threading.Thread(target=flusher)
    th.start()
    lat = []
    t_end = time.perf_counter() + seconds
    while time.perf_counter() < t_end:
        t0 = time.perf_counter()
        os.write(fd, LINE)
        lat.append((time.perf_counter() - t0) * 1e6)
        time.sleep(0.001)
    stop.set()
    th.join()
    if flush_fd != fd:
        os.close(flush_fd)
    os.close(fd)
    lat.sort()
    p = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))]
    print(f"{mode:14s} appends={len(lat):5d} flushes={flushes[0]:5d}  append us: median {p(0.5):8.1f}  p90 {p(0.9):8.1f}  p99 {p(0.99):8.1f}  max {lat[-1]:8.1f}")


for m in ("no_flusher", "same_handle", "second_handle"):
    run(m)
