"""Durability under a hostile environment (doc/agentmaster/flows/20-robustness.md, ROB).

The suite's own reads of the app's files go through amdriver.win32.read_bytes_shared, a handle held only for the read
(its FILE_SHARE_DELETE does not let the app's replace through: measured 2026-10-23, flows/20 ROB-K1); the test below
holds sessions.json the way an ordinary reader does — Python's open(), MSVC's std::ifstream, so also the `agentmaster`
CLI's own read by the reading of its code — and shows what the app's save does meanwhile.
"""
import time

import pytest

from amdriver.waits import hold_true


@pytest.mark.p2
@pytest.mark.flow("ROB-11")
@pytest.mark.known_defect("ROB-K1")
def test_known_defect_a_reader_holding_sessions_json_loses_the_save(am, s1, mock):
    """ROB-K1 (expected failure once fixed): while another process holds sessions.json open (here without
    FILE_SHARE_DELETE; a reader that grants it fails the save too, measured 2026-10-23), every save fails —
    `[persist-fail] …\\sessions.json (atomic replace failed — state NOT saved)` — and once the reader lets go NOTHING
    saves again until the next change: the turn that ended is still "Running" on disk. Found 2026-10-23 on 0.8.0 when
    the suite itself polled the file with open() (a finished turn stayed Running on disk for a minute). By reading:
    WriteAllUtf8 (Persistence.cpp) makes one MoveFileExW(REPLACE_EXISTING) attempt, which fails while any handle holds
    the target, logs, and returns.

    The test asserts TODAY's behavior; when it starts failing, the defect is fixed (a retry, or a re-save of the
    dirty state; a POSIX-semantics replace alone does not pass THIS reader, which grants no delete sharing): assert
    the record reads WaitingForInput within a few seconds of the release.

    Args:
        am: The app.
        s1: F-S.
        mock: The scripted model.
    """
    path = am.profile.dir / "sessions.json"
    mark = am.profile.hooks.mark()
    started_ms = int(time.time() * 1000)

    def stale() -> bool:
        """Whether the file still lacks the turn that ended during the hold.

        Returns:
            True while the record on disk is not WaitingForInput with a turn end after the prompt.
        """
        r = s1.record()
        return r.state != "WaitingForInput" or int(r.raw.get("turnEndUnixMs") or 0) < started_ms

    with open(path, "rb"):  # a plain reader: read and write sharing, no delete sharing
        s1.ask('held [[mock reply "done while held"]]', wait="sent")
        am.profile.hooks.wait(rf"\[Stop\] {s1.id} state=2", since=mark, timeout_s=60)
        am.profile.hooks.wait(r"\[persist-fail\] .*sessions\.json \(atomic replace failed", since=mark, timeout_s=10)
        # Hold on while the turn's follow-up saves (the late hooks, the transcript tail's turn stamps) fail too.
        time.sleep(8)
        failed = len(am.profile.hooks.find(r"\[persist-fail\] .*sessions\.json", since=mark))
        assert stale(), "the record changed on disk while the reader held it"
    # The reader is gone and the session sits idle: nothing changes, so nothing saves — the engine says
    # WaitingForInput, the file still says what was saved before the hold.
    hold_true(stale, 6.0, what="the stale record (expected: re-saved once the reader let go)",
              describe=lambda: (s1.record().state, s1.record().raw.get("turnEndUnixMs"), started_ms, failed))
