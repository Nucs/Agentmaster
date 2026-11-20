"""Durability under a hostile environment (doc/agentmaster/flows/20-robustness.md, ROB).

The suite's own reads of the app's files go through amdriver.win32.read_bytes_shared (a handle held only for the read)
or, for the fleet, sessions_store.load_fleet (a read-only SQLite connection held only for the read). The test below holds
the fleet store open the way other programs do — a plain open() of the file, and a SQLite read transaction like the
`agentmaster` CLI's — and shows that the app's saves go through meanwhile.
"""
import sqlite3
import time
import urllib.request

import pytest

from amdriver.waits import wait_until


@pytest.mark.p2
@pytest.mark.flow("ROB-11")
def test_a_reader_holding_the_fleet_store_never_loses_a_save(am, s1, mock):
    """ROB-11, ROB-K1 fixed by the fleet store (SESSIONS_STORE.md): while another process holds sessions.db open — a
    plain reader (Python's open(): read and write sharing, no delete sharing — the hold that made every sessions.json
    save fail) AND a SQLite read transaction over it (how the CLI reads) — the app's commits still land: the turn that
    ended reads WaitingForInput on disk within seconds, while both readers still hold the store, and no commit failed.

    Before the store (flows/20 ROB-K1, 2026-10-23): each save was a whole-file replace, which any open handle refused
    (`[persist-fail] …\\sessions.json (atomic replace failed — state NOT saved)`), and nothing re-saved after the reader
    let go. A commit now appends to the store's WAL, which no reader blocks; a checkpoint that cannot pass the open read
    transaction simply waits for a later round.

    Args:
        am: The app.
        s1: F-S.
        mock: The scripted model.
    """
    path = am.profile.dir / "sessions.db"
    assert path.exists(), "the profile has no fleet store (sessions.db): the app did not migrate or create it"
    mark = am.profile.hooks.mark()
    started_ms = int(time.time() * 1000)

    def saved() -> bool:
        """Whether the store holds the turn that ended during the hold.

        Returns:
            True once the record on disk is WaitingForInput with a turn end after the prompt.
        """
        r = s1.record()
        return r.state == "WaitingForInput" and int(r.raw.get("turnEndUnixMs") or 0) >= started_ms

    reader = sqlite3.connect("file:" + urllib.request.pathname2url(str(path)) + "?mode=ro", uri=True, timeout=5.0)
    try:
        reader.execute("BEGIN")
        reader.execute("SELECT count(*) FROM sessions").fetchone()  # a read transaction left open: a snapshot held
        with open(path, "rb"):  # a plain reader: read and write sharing, no delete sharing
            s1.ask('held [[mock reply "done while held"]]', wait="sent")
            am.profile.hooks.wait(rf"\[Stop\] {s1.id} state=2", since=mark, timeout_s=60)
            wait_until(saved, 10.0, what="the ended turn saved while two readers hold the store",
                       describe=lambda: (s1.record().state, s1.record().raw.get("turnEndUnixMs"), started_ms))
            failures = am.profile.hooks.find(r"\[fleet-fail\]|\[persist-fail\]", since=mark)
            assert not failures, f"a save failed while a reader held the store: {failures[:3]}"
    finally:
        reader.close()
