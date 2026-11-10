"""Measures what a JSONL outbox journal costs on this PC's C: drive (the profile's drive).

Each test appends one JSON line per record with a single os.write on an O_APPEND handle (the shape a Win32
WriteFile(FILE_APPEND_DATA) would have), optionally followed by os.fsync (= FlushFileBuffers on Windows):
  - no flush at all (survives a process kill: the bytes are in the OS cache),
  - a flush after EVERY record (survives a power loss per record),
  - a group commit: one flush per N records,
then a recovery scan (read + parse every line), and a concurrent tail reader that follows the file while it grows.
Prints the per-record cost of each. Writes only inside the folder given as argv[1].
"""
import json
import os
import sys
import threading
import time

folder = sys.argv[1]
os.makedirs(folder, exist_ok=True)

record = {
    "v": 1, "op": "put", "id": 0, "lane": "t:45101", "prio": 0, "key": "",
    "method": "sendRichMessage",
    "params": {"chat_id": 123456789, "message_thread_id": 45101, "disable_notification": False,
               "rich_message": {"markdown": "x" * 1800}},
    "then": {"kind": "final-reply", "sid": "0e4c554e-1111-2222-3333-444455556666"},
}


def line_for(i):
    record["id"] = i
    return (json.dumps(record, separators=(",", ":")) + "\n").encode("utf-8")


def bench(name, count, flush_every):
    path = os.path.join(folder, name + ".jsonl")
    if os.path.exists(path):
        os.remove(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_BINARY)
    lines = [line_for(i) for i in range(count)]
    t0 = time.perf_counter()
    for i, ln in enumerate(lines):
        os.write(fd, ln)
        if flush_every and (i + 1) % flush_every == 0:
            os.fsync(fd)
    if flush_every:
        os.fsync(fd)
    dt = time.perf_counter() - t0
    os.close(fd)
    size = os.path.getsize(path)
    print(f"{name:28s} {count:6d} records  {dt * 1000:9.1f} ms total  {dt / count * 1e6:9.1f} us/record  ({size / 1024 / 1024:.1f} MB)")
    return path


def recover(path):
    t0 = time.perf_counter()
    live = {}
    torn = 0
    with open(path, "rb") as f:
        for raw in f:
            if not raw.endswith(b"\n"):
                torn += 1
                continue
            try:
                r = json.loads(raw)
            except ValueError:
                torn += 1
                continue
            if r.get("op") == "put":
                live[r["id"]] = r
            elif r.get("op") in ("done", "drop"):
                live.pop(r["id"], None)
    dt = time.perf_counter() - t0
    print(f"{'recovery scan':28s} {len(live):6d} live     {dt * 1000:9.1f} ms total  (torn lines skipped: {torn})")


def concurrent(count):
    """A writer appends while a reader tails the same file with its own handle: the reader sees only whole lines."""
    path = os.path.join(folder, "concurrent.jsonl")
    if os.path.exists(path):
        os.remove(path)
    open(path, "wb").close()
    seen = []
    stop = threading.Event()

    def reader():
        with open(path, "rb") as f:
            buf = b""
            while not stop.is_set() or True:
                chunk = f.read(65536)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        ln, buf = buf.split(b"\n", 1)
                        seen.append((time.perf_counter(), json.loads(ln)["id"]))
                elif stop.is_set():
                    break
                else:
                    time.sleep(0.001)

    th = threading.Thread(target=reader)
    th.start()
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_BINARY)
    sent_at = {}
    for i in range(count):
        sent_at[i] = time.perf_counter()
        os.write(fd, line_for(i))
        time.sleep(0.0005)
    os.close(fd)
    time.sleep(0.05)
    stop.set()
    th.join()
    lags = sorted((t - sent_at[i]) * 1000 for t, i in seen)
    in_order = [i for _, i in seen] == list(range(count))
    print(f"{'concurrent tail reader':28s} {len(seen):6d}/{count} lines seen, in order: {in_order}, lag median {lags[len(lags) // 2]:.2f} ms, p99 {lags[int(len(lags) * 0.99)]:.2f} ms")


p = bench("no_flush", 5000, 0)
bench("flush_every_record", 300, 1)
bench("group_commit_32", 5000, 32)
# a log of 5000 puts + 4000 dones: what a restart reads
with open(p, "ab") as f:
    for i in range(4000):
        f.write((json.dumps({"v": 1, "op": "done", "id": i, "message_id": 100000 + i}) + "\n").encode())
    f.write(b'{"v":1,"op":"put","id":99999,"method":"sendMess')  # a torn last line (a kill mid-write)
recover(p)
concurrent(2000)
