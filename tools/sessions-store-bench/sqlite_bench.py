"""SQLite page-size and schema benchmark for Agentmaster's session store, on the real fleet (every prompt kept, the
owner's choice: history stays in the record).

Two schemas, each at page sizes 512 / 1024 / 2048 / 4096:
  doc     - one row per session holding the WHOLE record (queue included) as JSON: the naive mapping.
  split   - sessions(id, doc without the queue); prompt_state(one small row per queued prompt: every member but its
            text); prompt_text(one row per prompt, written once). A routine change rewrites one small session row, a
            prompt's status change one small state row; big texts are never rewritten.

Per variant (each in a fresh database under <workdir>):
  - the one-time migration: import the whole fleet in ONE transaction (synchronous=FULL), then read it all back and
    compare every session and prompt with the source (the verification the migration will do) - time and db size;
  - batches of K changed sessions (K = 1, 5, 20; 90 % session-field changes, 8 % prompt status changes, 2 % new
    prompts), committed with synchronous=NORMAL (relaxed) and FULL (durable), wal_autocheckpoint OFF during the
    phase: latency and the bytes written to the WAL per changed session;
  - then one checkpoint: the bytes it writes into the database per change of the phase (each dirty page once);
  - a cold load of the fleet (every session and prompt into dicts).
Bytes are the process's own write counter (GetProcessIoCounters): the WAL and the database, nothing else.

Usage: python -I sqlite_bench.py <fleet.json> <workdir>     (writes only under <workdir>; prints a table)
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import random
import shutil
import sqlite3
import statistics
import sys
import time

PAGE_SIZES = (512, 1024, 2048, 4096)
K_VALUES = (1, 5, 20)
BATCHES = 300
LIVE = 31


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                                                  "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.GetCurrentProcess.restype = wt.HANDLE
_k32.GetProcessIoCounters.argtypes = [wt.HANDLE, ctypes.POINTER(IO_COUNTERS)]


def written():
    """Returns the bytes this process has written since it started (GetProcessIoCounters.WriteTransferCount)."""
    c = IO_COUNTERS()
    if not _k32.GetProcessIoCounters(_k32.GetCurrentProcess(), ctypes.byref(c)):
        raise ctypes.WinError(ctypes.get_last_error())
    return c.WriteTransferCount


def dumps(v):
    return json.dumps(v, ensure_ascii=False, separators=(",", ":"))


# ---------------------------------------------------------------- the two schemas
class DocSchema:
    """One row per session with its whole record (queue included) as one JSON doc."""
    name = "doc"

    @staticmethod
    def create(con):
        con.execute("CREATE TABLE sessions(id TEXT NOT NULL UNIQUE, doc TEXT NOT NULL)")

    @staticmethod
    def import_all(con, sessions):
        con.executemany("INSERT INTO sessions(id, doc) VALUES(?,?)", [(s["id"], dumps(s)) for s in sessions])

    @staticmethod
    def write_session(con, s):
        con.execute("UPDATE sessions SET doc=? WHERE id=?", (dumps(s), s["id"]))

    @staticmethod
    def write_prompt_state(con, s, p, seq):
        con.execute("UPDATE sessions SET doc=? WHERE id=?", (dumps(s), s["id"]))

    @staticmethod
    def add_prompt(con, s, p, seq):
        con.execute("UPDATE sessions SET doc=? WHERE id=?", (dumps(s), s["id"]))

    @staticmethod
    def load(con):
        return {i: json.loads(d) for i, d in con.execute("SELECT id, doc FROM sessions")}


class SplitSchema:
    """Small mutable rows (a session's fields; each prompt's state) apart from the big immutable prompt texts."""
    name = "split"

    @staticmethod
    def create(con):
        con.execute("CREATE TABLE sessions(id TEXT NOT NULL UNIQUE, doc TEXT NOT NULL)")
        con.execute("CREATE TABLE prompt_state(session_id TEXT NOT NULL, seq INTEGER NOT NULL, prompt_id TEXT NOT NULL, state TEXT NOT NULL)")
        con.execute("CREATE UNIQUE INDEX prompt_state_key ON prompt_state(session_id, seq)")
        con.execute("CREATE TABLE prompt_text(session_id TEXT NOT NULL, prompt_id TEXT NOT NULL, text TEXT NOT NULL)")
        con.execute("CREATE UNIQUE INDEX prompt_text_key ON prompt_text(session_id, prompt_id)")

    @staticmethod
    def _session_doc(s):
        return dumps({k: v for k, v in s.items() if k != "queue"})

    @staticmethod
    def _state_doc(p):
        return dumps({k: v for k, v in p.items() if k != "text"})

    @staticmethod
    def import_all(con, sessions):
        con.executemany("INSERT INTO sessions(id, doc) VALUES(?,?)", [(s["id"], SplitSchema._session_doc(s)) for s in sessions])
        rows_state, rows_text = [], []
        for s in sessions:
            for seq, p in enumerate(s.get("queue") or []):
                rows_state.append((s["id"], seq, p["id"], SplitSchema._state_doc(p)))
                rows_text.append((s["id"], p["id"], p.get("text", "")))
        con.executemany("INSERT INTO prompt_state VALUES(?,?,?,?)", rows_state)
        con.executemany("INSERT INTO prompt_text VALUES(?,?,?)", rows_text)

    @staticmethod
    def write_session(con, s):
        con.execute("UPDATE sessions SET doc=? WHERE id=?", (SplitSchema._session_doc(s), s["id"]))

    @staticmethod
    def write_prompt_state(con, s, p, seq):
        con.execute("UPDATE prompt_state SET state=? WHERE session_id=? AND seq=?", (SplitSchema._state_doc(p), s["id"], seq))

    @staticmethod
    def add_prompt(con, s, p, seq):
        con.execute("INSERT INTO prompt_state VALUES(?,?,?,?)", (s["id"], seq, p["id"], SplitSchema._state_doc(p)))
        con.execute("INSERT INTO prompt_text VALUES(?,?,?)", (s["id"], p["id"], p.get("text", "")))

    @staticmethod
    def load(con):
        recs = {i: json.loads(d) for i, d in con.execute("SELECT id, doc FROM sessions")}
        texts = {(sid, pid): t for sid, pid, t in con.execute("SELECT session_id, prompt_id, text FROM prompt_text")}
        for sid, seq, pid, st in con.execute("SELECT session_id, seq, prompt_id, state FROM prompt_state ORDER BY session_id, seq"):
            p = json.loads(st)
            p["text"] = texts.get((sid, pid), "")
            recs[sid].setdefault("queue", []).append(p)
        for r in recs.values():
            r.setdefault("queue", [])
        return recs


def canonical(s):
    """A session as compared by the verification: its JSON with the queue always present."""
    s = dict(s)
    s["queue"] = s.get("queue") or []
    return dumps(s)


# ---------------------------------------------------------------- one variant
def run_variant(schema, page_size, sessions, workdir):
    """Measures one schema at one page size (see the module doc). Returns a dict of results."""
    folder = os.path.join(workdir, f"{schema.name}-{page_size}")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    path = os.path.join(folder, "sessions.db")
    con = sqlite3.connect(path, isolation_level=None)
    con.execute(f"PRAGMA page_size={page_size}")
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA synchronous=FULL")
    res = {"schema": schema.name, "page": page_size}

    # The migration: one transaction, then the verification read-back.
    t = time.perf_counter(); w = written()
    con.execute("BEGIN")
    schema.create(con)
    schema.import_all(con, sessions)
    con.execute("COMMIT")
    con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    res["import_ms"] = (time.perf_counter() - t) * 1000
    res["import_mb"] = (written() - w) / 1e6
    t = time.perf_counter()
    back = schema.load(con)
    ok = len(back) == len(sessions) and all(canonical(back[s["id"]]) == canonical(s) for s in sessions)
    res["verify_ms"] = (time.perf_counter() - t) * 1000
    res["verify_ok"] = ok
    res["db_mb"] = os.path.getsize(path) / 1e6

    con.execute("PRAGMA wal_autocheckpoint=0")  # checkpoints measured apart
    rng = random.Random(42)
    live = [dict(s, queue=list(s.get("queue") or [])) for s in sessions[:LIVE]]
    now = 1763600000000
    phases = []
    for durable in (False, True):
        con.execute("PRAGMA synchronous=" + ("FULL" if durable else "NORMAL"))
        for k in K_VALUES:
            lat = []
            w = written()
            for b in range(BATCHES):
                picks = rng.sample(range(len(live)), k)
                t = time.perf_counter()
                con.execute("BEGIN")
                for i in picks:
                    s = live[i]
                    now += rng.randint(1, 50)
                    r = rng.random()
                    if r < 0.02:
                        size = 50000 if rng.random() < 0.05 else rng.randint(500, 5000)
                        p = {"id": f"b{b}-{i}-{now}", "text": "x" * size, "status": "Pending", "label": "", "origin": "Typed"}
                        s["queue"].append(p)
                        schema.add_prompt(con, s, p, len(s["queue"]) - 1)
                    elif r < 0.10 and s["queue"]:
                        seq = rng.randrange(len(s["queue"]))
                        p = s["queue"][seq]
                        p["status"] = rng.choice(("Sent", "Pending", "Failed"))
                        p["sentAtUnixMs"] = now
                        schema.write_prompt_state(con, s, p, seq)
                    else:
                        for m in rng.sample(("lastActivityUnixMs", "pendingInputUnixMs", "turnStartUnixMs", "turnEndUnixMs", "state"), rng.randint(1, 3)):
                            s[m] = rng.choice(("Running", "Idle", "WaitingForInput")) if m == "state" else now
                        schema.write_session(con, s)
                con.execute("COMMIT")
                lat.append((time.perf_counter() - t) * 1000)
            changes = k * BATCHES
            wal_bytes = written() - w
            w = written()
            t = time.perf_counter()
            con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            ck_ms = (time.perf_counter() - t) * 1000
            ck_bytes = written() - w
            phases.append({"durable": durable, "k": k, "median_ms": statistics.median(lat),
                           "p99_ms": sorted(lat)[int(len(lat) * 0.99)],
                           "wal_b_per_change": wal_bytes / changes, "ckpt_b_per_change": ck_bytes / changes,
                           "ckpt_ms": ck_ms})
    res["phases"] = phases
    con.close()
    t = time.perf_counter()
    con2 = sqlite3.connect(path)
    loaded = schema.load(con2)
    con2.close()
    res["load_ms"] = (time.perf_counter() - t) * 1000
    res["loaded"] = len(loaded)
    return res


def main():
    fleet, workdir = sys.argv[1], sys.argv[2]
    with open(fleet, encoding="utf-8-sig") as f:
        sessions = json.load(f)["sessions"]
    os.makedirs(workdir, exist_ok=True)
    print(f"fleet: {len(sessions)} sessions, {sum(len(s.get('queue') or []) for s in sessions)} prompts; sqlite {sqlite3.sqlite_version}")
    out = []
    for schema in (DocSchema, SplitSchema):
        for ps in PAGE_SIZES:
            r = run_variant(schema, ps, sessions, workdir)
            out.append(r)
            print(f"\n== {r['schema']} page {r['page']}: migration {r['import_ms']:.0f} ms ({r['import_mb']:.1f} MB written), verify {r['verify_ms']:.0f} ms "
                  f"{'OK' if r['verify_ok'] else 'MISMATCH'}, db {r['db_mb']:.1f} MB; cold load {r['load_ms']:.0f} ms", flush=True)
            for p in r["phases"]:
                print(f"   {'durable' if p['durable'] else 'relaxed'} K={p['k']:2d}: median {p['median_ms']:6.3f} ms p99 {p['p99_ms']:7.3f}  "
                      f"WAL {p['wal_b_per_change']:7.0f} B/change + checkpoint {p['ckpt_b_per_change']:7.0f} B/change (ckpt {p['ckpt_ms']:.0f} ms)", flush=True)
    with open(os.path.join(workdir, "sqlite_results.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
