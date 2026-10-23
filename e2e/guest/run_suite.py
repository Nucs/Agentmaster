"""Run the end-to-end suite inside the test guest and leave the results where the host can fetch them.

Start it DETACHED (the desktop's `launch` tool, not `run_command`: run_command waits on every process the job
started and its MCP call ends after a few minutes, while a suite run takes tens of minutes):

    C:\\e2e\\venv\\Scripts\\python.exe Z:\\e2e\\src\\guest\\run_suite.py <run-name> [pytest arguments]

What it does:
1. copies the suite from the persist share (Z:\\e2e\\src, staged by the host) to the guest's own disk
   (C:\\e2e\\src) and runs it from there (the share is slow, and Python cannot create folders on it);
2. empties C:\\e2e\\out\\tests (the per-test evidence of the last run);
3. runs pytest with a JUnit report, its output in pytest.log, and the progress plugin (guest/progress.py), which
   appends one line per finished test to Z:\\e2e\\out\\<run-name>.progress.jsonl;
4. zips pytest.log, junit.xml, the mock's request log and every test's evidence folder into
   Z:\\e2e\\out\\<run-name>.zip, and writes Z:\\e2e\\out\\<run-name>.status.json — {"state": "running"} at the
   start, {"state": "done", "exit": N, "tests": …, "failures": …, "errors": …, "skipped": …} at the end.

The host does not poll: Z:\\e2e\\out is a folder of the host's disk, and its e2e_run.py wakes on that folder's change
notification at each of these writes.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import traceback
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

SHARE_SRC = Path(r"Z:\e2e\src")
SHARE_OUT = Path(r"Z:\e2e\out")
LOCAL_SRC = Path(r"C:\e2e\src")
OUT = Path(r"C:\e2e\out")


def write_status(run: str, status: dict) -> None:
    """Write the run's status file on the share (the host wakes on the write and reads it).

    The host reads the file the moment a change wakes it, so this write can meet the host's open: it is tried
    again for up to ~10 s before it gives up — the final status is what ends the host's wait.

    Args:
        run: The run's name.
        status: The status object.
    Returns:
        None.
    Raises:
        OSError: When every try failed.
    """
    text = json.dumps(status, indent=2)
    for attempt in range(10):
        try:
            (SHARE_OUT / f"{run}.status.json").write_text(text, encoding="utf-8")
            return
        except OSError:
            if attempt == 9:
                raise
            time.sleep(1)  # a pause before the retry, not a wait for a condition


def copy_source() -> None:
    """Replace the guest's local copy of the suite with the staged one.

    Returns:
        None.
    Raises:
        OSError: When the copy fails.
    """
    if LOCAL_SRC.exists():
        shutil.rmtree(LOCAL_SRC)
    shutil.copytree(SHARE_SRC, LOCAL_SRC, ignore=shutil.ignore_patterns("__pycache__", ".pytest_cache", "*.pyc"))


def junit_counts(path: Path) -> dict:
    """The totals of a JUnit report.

    Args:
        path: junit.xml.
    Returns:
        {"tests", "failures", "errors", "skipped"} ({} when the report is missing or broken).
    """
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError):
        return {}
    suite = root if root.tag == "testsuite" else root.find("testsuite")
    if suite is None:
        return {}
    return {k: int(suite.get(k, 0)) for k in ("tests", "failures", "errors", "skipped")}


def pack(run: str) -> Path:
    """Zip the run's results onto the share.

    Args:
        run: The run's name.
    Returns:
        The zip's path on the share.
    """
    local_zip = OUT / f"{run}.zip"
    with zipfile.ZipFile(local_zip, "w", zipfile.ZIP_DEFLATED) as z:
        for name in ("pytest.log", "junit.xml", "mock-requests.jsonl"):
            p = OUT / name
            if p.exists():
                z.write(p, name)
        tests = OUT / "tests"
        if tests.is_dir():
            for f in tests.rglob("*"):
                if f.is_file():
                    z.write(f, str(f.relative_to(OUT)))
    target = SHARE_OUT / f"{run}.zip"
    shutil.copyfile(local_zip, target)
    return target


def main(argv: list[str]) -> int:
    """Run the suite.

    Args:
        argv: [run-name, pytest arguments ...].
    Returns:
        pytest's exit code (99 when the runner itself failed).
    """
    if not argv:
        print(__doc__)
        return 2
    run, pytest_args = argv[0], argv[1:]
    started = time.time()
    write_status(run, {"state": "running", "started": started, "args": pytest_args})
    try:
        copy_source()
        if (OUT / "tests").exists():
            shutil.rmtree(OUT / "tests")
        (OUT / "tests").mkdir(parents=True, exist_ok=True)
        for name in ("pytest.log", "junit.xml", "mock-requests.jsonl"):
            (OUT / name).unlink(missing_ok=True)
        progress = SHARE_OUT / f"{run}.progress.jsonl"
        progress.unlink(missing_ok=True)
        # `-p guest.progress` imports C:\e2e\src\guest\progress.py: `python -m` puts the working folder (the suite's
        # copy) first on sys.path, and `guest` is a namespace package there.
        env = dict(os.environ, E2E_PROGRESS=str(progress))
        with open(OUT / "pytest.log", "w", encoding="utf-8") as log:
            code = subprocess.run([sys.executable, "-X", "utf8", "-m", "pytest", "-p", "guest.progress", *pytest_args,
                                   f"--junitxml={OUT / 'junit.xml'}"],
                                  cwd=str(LOCAL_SRC), stdout=log, stderr=subprocess.STDOUT, env=env).returncode
    except Exception:  # noqa: BLE001 - the runner reports its own failure in the status file
        write_status(run, {"state": "done", "exit": 99, "runner_error": traceback.format_exc(),
                           "seconds": round(time.time() - started, 1)})
        return 99
    zip_path = pack(run)
    write_status(run, {"state": "done", "exit": code, **junit_counts(OUT / "junit.xml"),
                       "seconds": round(time.time() - started, 1), "zip": str(zip_path)})
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
