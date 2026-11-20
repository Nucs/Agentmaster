"""Benchmarks four ways to persist Agentmaster's session fleet on this PC's profile drive, with the real fleet:

  today    - the whole fleet as one JSON file, replaced atomically per save (temp + fsync + rename): what the app does now.
  journal  - an append-only journal of CRC-sealed lines holding only the CHANGED members of the changed sessions, plus
             an occasional checkpoint of the hot records (the in-house design measured against the databases).
  journal-pre - the same journal with segments preallocated in 4 MB extents of zeros (no file growth per append).
  sqlite   - SQLite in WAL mode, a row per session (id, JSON doc); synchronous=NORMAL (relaxed) or FULL (durable).
  duckdb   - DuckDB, a row per session (id, JSON doc); every commit goes through its WAL.

Usage (from the scratchpad; every file it writes lives under <workdir>):
  python -I store_bench.py all <fleet.json> <workdir>          run every engine in its own child process, then the
                                                                crash tests; prints a report and writes results.json
  python -I store_bench.py run <engine> <fleet.json> <workdir>  one engine's measurements (a child; prints one JSON line)
  python -I store_bench.py crashchild <engine> <dir>            commit forever, printing "ack <seq>" after each durable
                                                                commit (the crash test's victim)

What is measured, per engine (each in a fresh process, so memory and caches do not leak between engines):
  - import: writing the 2,190 hot records the first time;
  - batch commits of K changed sessions (K = 1, 5, 20), relaxed (no flush) and durable (flushed before returning):
    latency median / p95 / p99 and the bytes the process wrote per changed session (GetProcessIoCounters);
  - a checkpoint (journal: the hot records rewritten + the journal rotated; sqlite: wal_checkpoint(TRUNCATE);
    duckdb: CHECKPOINT; today: n/a);
  - a cold load of every record into dicts (journal: checkpoint + fold of the journal written above);
  - the process's private memory after the load and the workload (GetProcessMemoryInfo), minus the baseline before it.
The crash test kills a committing child with TerminateProcess at a random moment, 30 times per engine, then opens the
store: it must open, every record must parse, and the last ACKNOWLEDGED durable commit must be there; its first round
also reads the store from this process while the child holds it open. The torn-tail test cuts the journal at 250 random
offsets (50 followed by garbage) and requires the fold to equal the fold of the whole lines before the cut.

Python's own per-call cost is in every number (it weighs most on the journal's microsecond appends), so a C++ build of
the winner is cheaper still; the ratios are what matter. Nothing outside <workdir> is written.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import random
import statistics
import subprocess
import sys
import threading
import time
import zlib

GB = 1024 ** 3
K_VALUES = (1, 5, 20)
BATCHES_PER_K = 300
LIVE_SESSIONS = 31
CRASH_ROUNDS = 30
MEMBERS = ("lastActivityUnixMs", "state", "pendingInputUnixMs", "turnStartUnixMs", "turnEndUnixMs", "pendingInput")
STATES = ("Running", "WaitingForInput", "Idle", "NeedsApproval")


# ---------------------------------------------------------------- Windows process counters (ctypes, no psutil)
class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                                                  "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)
_k32.GetCurrentProcess.restype = wt.HANDLE
# Declared argument types: the pseudo-handle (-1) is a 64-bit value that ctypes' default int conversion overflows.
_k32.GetProcessIoCounters.argtypes = [wt.HANDLE, ctypes.POINTER(IO_COUNTERS)]
_psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX), wt.DWORD]


def written_bytes():
    """Returns the bytes this process has written through I/O calls since it started (files, pipes; here: files)."""
    c = IO_COUNTERS()
    if not _k32.GetProcessIoCounters(_k32.GetCurrentProcess(), ctypes.byref(c)):
        raise ctypes.WinError(ctypes.get_last_error())
    return c.WriteTransferCount


def private_bytes():
    """Returns this process's private committed memory (PrivateUsage) in bytes."""
    m = PROCESS_MEMORY_COUNTERS_EX()
    m.cb = ctypes.sizeof(m)
    if not _psapi.GetProcessMemoryInfo(_k32.GetCurrentProcess(), ctypes.byref(m), m.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return m.PrivateUsage


# ---------------------------------------------------------------- the fleet and the workload
def load_fleet(path):
    """Reads the fleet copy and splits each session into its HOT record (open prompts only) and its completed prompts.

    Args:
        path: a copy of sessions.json.
    Returns:
        (hot records as a list of dicts, the total bytes of completed prompts as compact JSON).
    Raises:
        OSError / ValueError: the file cannot be read or parsed.
    """
    with open(path, encoding="utf-8-sig") as f:
        doc = json.load(f)
    hot = []
    done_bytes = 0
    for s in doc["sessions"]:
        s = dict(s)
        q = s.get("queue") or []
        s["queue"] = [p for p in q if p.get("status") not in ("Sent", "Failed", "Skipped")]
        done_bytes += sum(len(json.dumps(p, ensure_ascii=False).encode()) for p in q if p.get("status") in ("Sent", "Failed", "Skipped"))
        hot.append(s)
    return hot, done_bytes


def load_full_fleet(path):
    """Reads the fleet copy with every session's WHOLE queue (completed prompts included): what today's file holds.

    Args:
        path: a copy of sessions.json.
    Returns:
        The session records as a list of dicts, exactly as the file has them.
    Raises:
        OSError / ValueError: the file cannot be read or parsed.
    """
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)["sessions"]


def make_batches(records, k, n, rng):
    """Builds n batches of k changed sessions among the LIVE_SESSIONS busiest ids, each change setting 1-3 members
    (5 % of the changes add a queued prompt instead), the way a busy fleet changes.

    Returns: a list of batches; a batch is a list of (index, {member: value}).
    """
    live = list(range(min(LIVE_SESSIONS, len(records))))
    out = []
    now = 1763600000000
    for b in range(n):
        batch = []
        for i in rng.sample(live, k):
            now += rng.randint(1, 50)
            if rng.random() < 0.05:
                q = list(records[i].get("queue") or [])
                q.append({"id": f"bench-{b}-{i}", "text": "x" * 200, "status": "Pending", "label": "", "origin": "Typed"})
                batch.append((i, {"queue": q}))
                continue
            patch = {}
            for m in rng.sample(MEMBERS, rng.randint(1, 3)):
                patch[m] = (rng.choice(STATES) if m == "state" else "draft text " * rng.randint(0, 3) if m == "pendingInput" else now)
            batch.append((i, patch))
        out.append(batch)
    return out


# ---------------------------------------------------------------- the engines
class TodayStore:
    """The current design: every save writes the whole fleet document (temp file, fsync, atomic replace)."""

    def __init__(self, folder):
        self.path = os.path.join(folder, "sessions.json")
        self.records = []

    def import_all(self, records):
        self.records = [dict(r) for r in records]
        self._save(durable=True)

    def commit(self, batch, durable):
        for i, patch in batch:
            self.records[i].update(patch)
        self._save(durable)

    def _save(self, durable):
        # The app fsyncs every save (FlushFileBuffers) whatever the change; "relaxed" skips it here for comparison.
        tmp = self.path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(json.dumps({"version": 3, "sessions": self.records}, ensure_ascii=False, indent=1).encode("utf-8"))
            f.flush()
            if durable:
                os.fsync(f.fileno())
        os.replace(tmp, self.path)

    def checkpoint(self):
        pass

    def close(self):
        pass

    @staticmethod
    def load(folder):
        with open(os.path.join(folder, "sessions.json"), encoding="utf-8") as f:
            return {r["id"]: r for r in json.load(f)["sessions"]}


def _seal(obj):
    """Serializes one journal record as a line: the JSON members, then "crc" = CRC-32 of the bytes before it."""
    body = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))[:-1]
    crc = zlib.crc32(body.encode("utf-8")) & 0xFFFFFFFF
    return (body + ',"crc":"%08x"}\n' % crc).encode("utf-8")


class JournalStore:
    """The proposed design: CRC-sealed member patches appended to journal-<gen>.jsonl; checkpoint-<gen> holds the hot
    records; a checkpoint writes the next generation's file (temp + fsync + replace) BEFORE the old journal is deleted."""

    def __init__(self, folder):
        self.folder = folder
        self.records = []
        self.gen = 0
        self.fd = None

    def _journal_path(self, gen):
        return os.path.join(self.folder, "journal-%d.jsonl" % gen)

    def _open_journal(self):
        # O_APPEND + unbuffered: each os.write is ONE WriteFile at the end of the file.
        self.fd = os.open(self._journal_path(self.gen), os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_BINARY, 0o644)

    def import_all(self, records):
        self.records = [dict(r) for r in records]
        self._write_checkpoint()
        self._open_journal()

    def _write_checkpoint(self):
        path = os.path.join(self.folder, "checkpoint.json")
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(json.dumps({"version": 4, "gen": self.gen, "sessions": self.records}, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

    def commit(self, batch, durable):
        for i, patch in batch:
            self.records[i].update(patch)
        data = b"".join(_seal({"op": "set", "id": self.records[i]["id"], "m": patch}) for i, patch in batch)
        os.write(self.fd, data)  # one WriteFile for the whole group: a group is whole or absent
        if durable:
            os.fsync(self.fd)  # FlushFileBuffers

    def checkpoint(self):
        # Order matters for a crash: the new generation's checkpoint is durable before the old journal goes.
        old = self.gen
        os.close(self.fd)
        self.gen += 1
        self._write_checkpoint()
        self._open_journal()
        os.remove(self._journal_path(old))

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    @staticmethod
    def load(folder):
        with open(os.path.join(folder, "checkpoint.json"), encoding="utf-8") as f:
            cp = json.load(f)
        recs = {r["id"]: r for r in cp["sessions"]}
        path = os.path.join(folder, "journal-%d.jsonl" % cp["gen"])
        if os.path.exists(path):
            with open(path, "rb") as f:
                # Only \n-terminated lines count: the part after the last \n is a write in progress or torn, even when
                # its CRC happens to be whole (a cut right before the newline) - the outbox's rule (§5.2).
                for raw in f.read().split(b"\n")[:-1]:
                    if not raw:
                        continue
                    cut = raw.rfind(b',"crc":"')
                    if cut < 0 or not raw.endswith(b'"}'):
                        continue  # a torn tail (no newline yet / cut short): not written
                    if "%08x" % (zlib.crc32(raw[:cut]) & 0xFFFFFFFF) != raw[cut + 8:-2].decode():
                        continue  # damaged: skipped (the real reader also cuts it off at the end of the file)
                    line = json.loads(raw)
                    recs.setdefault(line["id"], {"id": line["id"]}).update(line["m"])
        return recs


EXTENT = 4 * 1024 * 1024


class JournalPreStore(JournalStore):
    """The journal with PRE-ALLOCATED segments: the file is extended 4 MB at a time with zeros (written, so NTFS's valid
    data length is already past them), and each commit overwrites zeros at the logical end instead of growing the file.
    A flush then has only data blocks to write, no file-size metadata - the trick SQLite's reused WAL gets for free. The
    logical end is the first line that starts with a zero byte (a commit never writes one: JSON text has no NULs)."""

    def _open_journal(self):
        path = self._journal_path(self.gen)
        self.fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_BINARY, 0o644)
        self.pos = JournalPreStore.logical_end(path)
        self.alloc = os.path.getsize(path)

    def _ensure(self, n):
        # Extend by whole extents of zeros until n more bytes fit; the extension is flushed once (rare: every 4 MB).
        while self.pos + n > self.alloc:
            os.lseek(self.fd, self.alloc, os.SEEK_SET)
            os.write(self.fd, b"\0" * EXTENT)
            self.alloc += EXTENT
            os.fsync(self.fd)

    def commit(self, batch, durable):
        for i, patch in batch:
            self.records[i].update(patch)
        data = b"".join(_seal({"op": "set", "id": self.records[i]["id"], "m": patch}) for i, patch in batch)
        self._ensure(len(data))
        os.lseek(self.fd, self.pos, os.SEEK_SET)
        os.write(self.fd, data)
        self.pos += len(data)
        if durable:
            os.fsync(self.fd)

    @staticmethod
    def logical_end(path):
        """Returns the offset just past the journal's last WHOLE, CRC-valid line: where the next commit goes.

        A torn tail (a line cut short, then zeros) is NOT skipped over: the next commit overwrites it, so a new line
        never lands behind garbage on the same line (which would hide it from the fold).
        Args:
            path: the journal segment.
        Returns:
            The byte offset (0 for a missing or empty journal).
        """
        if not os.path.exists(path):
            return 0
        with open(path, "rb") as f:
            data = f.read()
        pos = 0
        while pos < len(data) and data[pos:pos + 1] != b"\0":
            nl = data.find(b"\n", pos)
            if nl < 0:
                break  # no newline: torn
            raw = data[pos:nl]
            cut = raw.rfind(b',"crc":"')
            if cut < 0 or not raw.endswith(b'"}') or "%08x" % (zlib.crc32(raw[:cut]) & 0xFFFFFFFF) != raw[cut + 8:-2].decode("ascii", "replace"):
                break  # damaged: the end of what can be trusted
            pos = nl + 1
        return pos

    @staticmethod
    def load(folder):
        # The base fold reads every \n-terminated line; the zero tail has none that passes the CRC, so it adds nothing.
        return JournalStore.load(folder)


class SqliteStore:
    """SQLite in WAL mode; each changed session's whole JSON doc is UPSERTed in one transaction per batch."""

    def __init__(self, folder):
        import sqlite3
        self.path = os.path.join(folder, "sessions.db")
        self.con = sqlite3.connect(self.path, isolation_level=None, check_same_thread=False)
        self.con.execute("PRAGMA journal_mode=WAL")
        self.con.execute("CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY, doc TEXT NOT NULL)")
        self.records = []
        self.mode = None

    def _sync(self, durable):
        mode = "FULL" if durable else "NORMAL"
        if mode != self.mode:
            self.con.execute("PRAGMA synchronous=" + mode)
            self.mode = mode

    def import_all(self, records):
        self.records = [dict(r) for r in records]
        self._sync(True)
        self.con.execute("BEGIN")
        self.con.executemany("INSERT OR REPLACE INTO sessions VALUES(?,?)",
                             [(r["id"], json.dumps(r, ensure_ascii=False, separators=(",", ":"))) for r in self.records])
        self.con.execute("COMMIT")

    def commit(self, batch, durable):
        self._sync(durable)
        for i, patch in batch:
            self.records[i].update(patch)
        self.con.execute("BEGIN")
        self.con.executemany("INSERT INTO sessions(id,doc) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET doc=excluded.doc",
                             [(self.records[i]["id"], json.dumps(self.records[i], ensure_ascii=False, separators=(",", ":"))) for i, _ in batch])
        self.con.execute("COMMIT")

    def checkpoint(self):
        self.con.execute("PRAGMA wal_checkpoint(TRUNCATE)")

    def close(self):
        self.con.close()

    @staticmethod
    def load(folder):
        import sqlite3
        con = sqlite3.connect(os.path.join(folder, "sessions.db"))
        try:
            return {i: json.loads(d) for i, d in con.execute("SELECT id, doc FROM sessions")}
        finally:
            con.close()


class DuckdbStore:
    """DuckDB; each changed session's whole JSON doc is INSERT OR REPLACEd in one transaction per batch. DuckDB has no
    relaxed commit: every commit writes and syncs its WAL, so relaxed == durable here."""

    def __init__(self, folder):
        import duckdb
        self.path = os.path.join(folder, "sessions.duckdb")
        self.con = duckdb.connect(self.path)
        self.con.execute("CREATE TABLE IF NOT EXISTS sessions(id VARCHAR PRIMARY KEY, doc VARCHAR NOT NULL)")
        self.records = []

    def import_all(self, records):
        self.records = [dict(r) for r in records]
        self.con.execute("BEGIN TRANSACTION")
        self.con.executemany("INSERT OR REPLACE INTO sessions VALUES(?,?)",
                             [(r["id"], json.dumps(r, ensure_ascii=False, separators=(",", ":"))) for r in self.records])
        self.con.execute("COMMIT")

    def commit(self, batch, durable):
        for i, patch in batch:
            self.records[i].update(patch)
        self.con.execute("BEGIN TRANSACTION")
        self.con.executemany("INSERT OR REPLACE INTO sessions VALUES(?,?)",
                             [(self.records[i]["id"], json.dumps(self.records[i], ensure_ascii=False, separators=(",", ":"))) for i, _ in batch])
        self.con.execute("COMMIT")

    def checkpoint(self):
        self.con.execute("CHECKPOINT")

    def close(self):
        self.con.close()

    @staticmethod
    def load(folder):
        import duckdb
        con = duckdb.connect(os.path.join(folder, "sessions.duckdb"), read_only=True)
        try:
            return {i: json.loads(d) for i, d in con.execute("SELECT id, doc FROM sessions").fetchall()}
        finally:
            con.close()


ENGINES = {"today": TodayStore, "journal": JournalStore, "journal-pre": JournalPreStore, "sqlite": SqliteStore,
           "duckdb": DuckdbStore}
JOURNALS = ("journal", "journal-pre")


# ---------------------------------------------------------------- one engine's run (a child process)
def pct(values, p):
    """Returns the p-quantile (0..1) of values by rank (no interpolation)."""
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))]


def open_existing(name, folder, witness_id=None):
    """Opens an engine's store that already holds the fleet, with its records in memory (the witness first, when
    given): how the crash child and the journal's cold-load replay continue a store.

    Args:
        name: the engine. folder: its folder. witness_id: the record to put at index 0 (its benchSeq is checked).
    Returns:
        The open store.
    Raises:
        Whatever the engine raises on open.
    """
    store = ENGINES[name](folder)
    if name in JOURNALS:
        store.gen = json.load(open(os.path.join(folder, "checkpoint.json"), encoding="utf-8"))["gen"]
        recs = list(ENGINES[name].load(folder).values())
        store.records = recs
        store._open_journal()
    elif name == "duckdb":
        # One connection per process: DuckDB refuses a second one to the same file with another configuration.
        store.records = [json.loads(d) for _, d in store.con.execute("SELECT id, doc FROM sessions").fetchall()]
    else:
        store.records = list(ENGINES[name].load(folder).values())
    if witness_id is not None:
        idx = next(i for i, r in enumerate(store.records) if r["id"] == witness_id)
        store.records.insert(0, store.records.pop(idx))
    return store


def run_engine(name, fleet_path, workdir):
    """Measures one engine (see the module doc) and prints one JSON line with the results.

    Raises: whatever the engine raises (the parent reports a failed engine instead of a result).
    """
    records = load_full_fleet(fleet_path) if name == "today" else load_fleet(fleet_path)[0]
    folder = os.path.join(workdir, name)
    os.makedirs(folder, exist_ok=True)
    rng = random.Random(42)
    res = {"engine": name}
    mem0 = private_bytes()
    store = ENGINES[name](folder)
    t = time.perf_counter(); w = written_bytes()
    store.import_all(records)
    res["import_ms"] = (time.perf_counter() - t) * 1000
    res["import_mb"] = (written_bytes() - w) / 1e6
    phases = []
    # A whole-file save writes ~14 MB: 8 rounds per phase (48 saves, ~0.7 GB) are enough for its latency and its bytes,
    # and spare the SSD the 2.5 GB that 30 would write.
    batches_n = 8 if name == "today" else BATCHES_PER_K
    for durable in (False, True):
        for k in K_VALUES:
            batches = make_batches(records, k, batches_n, rng)
            lat = []
            w = written_bytes()
            for b in batches:
                t = time.perf_counter()
                store.commit(b, durable)
                lat.append((time.perf_counter() - t) * 1000)
            changes = k * len(batches)
            phases.append({"durable": durable, "k": k, "batches": len(batches), "median_ms": statistics.median(lat),
                           "p95_ms": pct(lat, 0.95), "p99_ms": pct(lat, 0.99),
                           "bytes_per_change": (written_bytes() - w) / changes})
    res["phases"] = phases
    t = time.perf_counter(); w = written_bytes()
    store.checkpoint()
    res["checkpoint_ms"] = (time.perf_counter() - t) * 1000
    res["checkpoint_mb"] = (written_bytes() - w) / 1e6
    res["mem_mb"] = (private_bytes() - mem0) / 1e6
    store.close()
    res["files_mb"] = sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder)) / 1e6
    # Cold load: a fresh read of what is on disk. For a journal, 2,000 more changes go into the new generation first,
    # so the fold has real work (a checkpoint plus a journal, as at a start after a crash).
    if name in JOURNALS:
        store2 = open_existing(name, folder)
        for b in make_batches(store2.records, 20, 100, rng):
            store2.commit(b, False)
        store2.close()
    t = time.perf_counter()
    loaded = ENGINES[name].load(folder)
    res["load_ms"] = (time.perf_counter() - t) * 1000
    res["loaded"] = len(loaded)
    print(json.dumps(res))


# ---------------------------------------------------------------- the crash test
def crash_child(name, folder, witness_id):
    """Commits durable batches forever; prints "ack <seq>" after each returns. The witness record's benchSeq is the
    proof the parent checks after the kill."""
    store = open_existing(name, folder, witness_id)
    rng = random.Random(os.getpid())
    seq = 0
    ids = list(range(1, min(LIVE_SESSIONS, len(store.records))))
    while True:
        seq += 1
        batch = [(0, {"benchSeq": seq})] + [(i, {"lastActivityUnixMs": seq}) for i in rng.sample(ids, 4)]
        store.commit(batch, True)
        sys.stdout.write("ack %d\n" % seq)
        sys.stdout.flush()


def crash_test(name, fleet_path, workdir):
    """Kills a committing child CRASH_ROUNDS times (TerminateProcess at a random moment); after each kill the store
    must open, hold every record, and hold the last ACKNOWLEDGED commit. A round whose child acknowledged nothing is a
    failure (it tested nothing). Round 0 also tries a read from this process while the child holds the store open.

    Returns: {"rounds", "ok", "failures": [...], "acked_max", "read_while_open"}.
    """
    records = load_fleet(fleet_path)[0]
    witness = records[0]["id"]
    folder = os.path.join(workdir, name + "-crash")
    os.makedirs(folder, exist_ok=True)
    store = ENGINES[name](folder)
    store.import_all(records)
    if name in JOURNALS:
        store.checkpoint()
    store.close()
    rng = random.Random(7)
    failures = []
    acked_max = 0
    read_while_open = None
    for r in range(CRASH_ROUNDS):
        proc = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "crashchild", name, folder, witness],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        last = [0]

        def reader():
            for line in proc.stdout:
                if line.startswith("ack "):
                    last[0] = int(line.split()[1])

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        deadline = time.time() + 30
        while last[0] < 3 and time.time() < deadline and proc.poll() is None:
            time.sleep(0.01)
        if r == 0:
            try:
                n = len(ENGINES[name].load(folder))
                read_while_open = f"ok ({n} records)"
            except Exception as ex:
                read_while_open = f"refused: {type(ex).__name__}: {str(ex)[:160]}"
        time.sleep(rng.uniform(0.01, 0.4))
        proc.kill()
        proc.wait()
        th.join(timeout=5)
        acked = last[0]
        acked_max = max(acked_max, acked)
        if acked == 0:
            failures.append(f"round {r}: the child acknowledged nothing ({proc.stderr.read()[-200:]!r})")
            continue
        try:
            recs = ENGINES[name].load(folder)
            got = recs[witness].get("benchSeq", 0)
            if len(recs) != len(records):
                failures.append(f"round {r}: {len(recs)} records, expected {len(records)}")
            elif got < acked:
                failures.append(f"round {r}: last acked {acked}, found {got}")
        except Exception as ex:  # an open or parse failure IS the finding
            failures.append(f"round {r}: {type(ex).__name__}: {ex}")
    return {"rounds": CRASH_ROUNDS, "ok": CRASH_ROUNDS - len(failures), "failures": failures[:5], "acked_max": acked_max,
            "read_while_open": read_while_open}


def torn_tail_test(workdir):
    """Simulates a power cut inside a journal write: the journal is cut at 200 random byte offsets (and 50 times a cut
    is followed by garbage); each time the fold must return exactly the records of the whole lines before the cut.

    Returns: {"cases", "ok", "failures"}.
    """
    import shutil
    src = os.path.join(workdir, "journal")
    tmp = os.path.join(workdir, "torn")
    rng = random.Random(11)
    jpath = [f for f in os.listdir(src) if f.startswith("journal-")][0]
    data = open(os.path.join(src, jpath), "rb").read()
    lines_end = [i + 1 for i, b in enumerate(data) if b == 0x0A]
    failures = []
    cases = 0
    for c in range(250):
        cut = rng.randint(0, len(data))
        garbage = bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 300))) if c >= 200 else b""
        if os.path.exists(tmp):
            shutil.rmtree(tmp)
        shutil.copytree(src, tmp)
        with open(os.path.join(tmp, jpath), "wb") as f:
            f.write(data[:cut] + garbage)
        whole = [e for e in lines_end if e <= cut]
        expect_prefix = data[:whole[-1]] if whole else b""
        # The expected state: the fold of only the whole lines before the cut.
        with open(os.path.join(tmp, jpath + ".expect"), "wb") as f:
            f.write(expect_prefix)
        got = JournalStore.load(tmp)
        os.replace(os.path.join(tmp, jpath + ".expect"), os.path.join(tmp, jpath))
        want = JournalStore.load(tmp)
        cases += 1
        if got != want:
            failures.append(f"case {c}: cut {cut} garbage {len(garbage)}")
    shutil.rmtree(tmp, ignore_errors=True)
    return {"cases": cases, "ok": cases - len(failures), "failures": failures[:5]}


def main():
    mode = sys.argv[1]
    if mode == "run":
        run_engine(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if mode == "crashchild":
        crash_child(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    fleet, workdir = sys.argv[2], sys.argv[3]
    os.makedirs(workdir, exist_ok=True)
    records, done_bytes = load_fleet(fleet)
    print(f"fleet: {len(records)} sessions; hot records {len(json.dumps(records, ensure_ascii=False).encode())/1e6:.2f} MB; "
          f"completed prompts {done_bytes/1e6:.2f} MB (only 'today' keeps them in its file)", flush=True)
    results = {}
    for name in ("journal", "journal-pre", "sqlite", "duckdb", "today"):
        t = time.perf_counter()
        out = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "run", name, fleet, workdir],
                             capture_output=True, text=True)
        line = [l for l in out.stdout.splitlines() if l.startswith("{")]
        results[name] = json.loads(line[-1]) if line else {"error": out.stderr[-600:]}
        print(f"{name}: done in {time.perf_counter() - t:.0f} s", flush=True)
    for name in ("journal", "journal-pre", "sqlite", "duckdb"):
        if "error" in results[name]:
            continue
        results[name]["crash"] = crash_test(name, fleet, workdir)
        print(f"{name} crash test: {results[name]['crash']['ok']}/{CRASH_ROUNDS} ok; read while open: "
              f"{results[name]['crash']['read_while_open']}", flush=True)
    results["journal"]["torn_tail"] = torn_tail_test(workdir)
    print(f"journal torn-tail test: {results['journal']['torn_tail']['ok']}/{results['journal']['torn_tail']['cases']} ok", flush=True)
    with open(os.path.join(workdir, "results.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
