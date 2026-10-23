"""A pytest plugin: one line per finished test, appended to a file the host watches — a suite run's live progress.

run_suite.py loads it (`-p guest.progress`) and names the file in E2E_PROGRESS: Z:\\e2e\\out\\<run>.progress.jsonl
on the share, which is the host's persist folder. The host's e2e_run.py wakes on that folder's change notification
and prints each new line, so neither side polls and nobody reads pytest's log to know how a run is going.

Lines (UTF-8 JSON, one object per line):

    {"event": "collected", "total": 49, "time": 1792758873.3}
    {"event": "test", "n": 12, "total": 49, "nodeid": "suites/test_x.py::test_y", "outcome": "failed",
     "seconds": 91.2, "message": "AssertionError: …", "time": …}

`outcome` is passed, failed (the test's own call failed), error (its setup or teardown failed), skipped, xfailed or
xpassed; `message` is the first line of the failure or the skip reason (at most 300 characters), absent for a pass.

A write that fails (the share busy, a sharing violation) is dropped after three tries: the progress is a convenience,
junit.xml and the status file stay the record — a progress write must never fail a test or the run.
"""
from __future__ import annotations

import json
import os
import time

_state = {"n": 0, "total": None}
"""How many tests finished, and how many were collected."""
_reports: dict[str, list] = {}
"""Each running test's phase reports (setup, call, teardown), until it finishes."""


def _append(obj: dict) -> None:
    """Append one line to the progress file (when run_suite.py named one).

    Args:
        obj: The line's object (a "time" key is added).
    Returns:
        None.
    """
    path = os.environ.get("E2E_PROGRESS")
    if not path:
        return
    line = json.dumps({**obj, "time": round(time.time(), 1)}, ensure_ascii=False) + "\n"
    for attempt in range(3):
        try:
            with open(path, "a", encoding="utf-8") as f:
                f.write(line)
            return
        except OSError:
            # A short pause before the retry, not a wait for a condition: the share was busy for this one write.
            if attempt < 2:
                time.sleep(0.2)


def _first_line(report) -> str:
    """The first line of what went wrong in a phase (or why it was skipped).

    Args:
        report: A pytest TestReport.
    Returns:
        At most 300 characters ("" when there is nothing to say).
    """
    longrepr = report.longrepr
    if longrepr is None:
        return ""
    if isinstance(longrepr, tuple) and len(longrepr) == 3:
        text = str(longrepr[2])  # a skip: (path, line, reason)
    else:
        crash = getattr(longrepr, "reprcrash", None)
        text = getattr(crash, "message", "") if crash is not None else ""
        text = text or str(longrepr)
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return lines[0][:300] if lines else ""


def _outcome(reports: list) -> tuple[str, str]:
    """A finished test's outcome from its phase reports, the way pytest's own summary counts it.

    Args:
        reports: The test's setup, call and teardown reports (in that order; a skipped setup has no call).
    Returns:
        (outcome, message).
    """
    by_phase = {r.when: r for r in reports}
    setup, call, teardown = by_phase.get("setup"), by_phase.get("call"), by_phase.get("teardown")
    if setup is not None and setup.failed:
        return "error", _first_line(setup)
    if setup is not None and setup.skipped:
        if hasattr(setup, "wasxfail"):
            return "xfailed", str(setup.wasxfail)[:300]
        return "skipped", _first_line(setup)
    if call is not None:
        if call.failed:
            return "failed", _first_line(call)
        if call.skipped:
            # An expected failure names its xfail reason; a skip names pytest.skip's.
            if hasattr(call, "wasxfail"):
                return "xfailed", str(call.wasxfail)[:300]
            return "skipped", _first_line(call)
        if hasattr(call, "wasxfail"):
            return "xpassed", str(call.wasxfail)[:300]
    if teardown is not None and teardown.failed:
        # The test passed but its teardown did not (the app's end, the evidence): pytest counts an error.
        return "error", _first_line(teardown)
    return "passed", ""


def pytest_collection_finish(session) -> None:
    """Write how many tests the run holds.

    Args:
        session: The pytest session.
    Returns:
        None.
    """
    _state["total"] = len(session.items)
    _append({"event": "collected", "total": _state["total"]})


def pytest_runtest_logreport(report) -> None:
    """Keep each phase's report until its test finishes.

    Args:
        report: A setup, call or teardown report.
    Returns:
        None.
    """
    _reports.setdefault(report.nodeid, []).append(report)


def pytest_runtest_logfinish(nodeid: str, location) -> None:
    """Write the finished test's line.

    Args:
        nodeid: The test's node id.
        location: Its (file, line, name) — unused.
    Returns:
        None.
    """
    reports = _reports.pop(nodeid, [])
    _state["n"] += 1
    outcome, message = _outcome(reports)
    line = {"event": "test", "n": _state["n"], "total": _state["total"], "nodeid": nodeid, "outcome": outcome,
            "seconds": round(sum(r.duration for r in reports), 1)}
    if message:
        line["message"] = message
    _append(line)
