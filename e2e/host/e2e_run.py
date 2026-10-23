"""Run the end-to-end suite from the host PC: stage it into a test desktop, provision, run, collect the results.

The test desktop is a claude-fleet Windows 11 VM (skill `claude-desktops`); its persist folder on this PC
(Q:\\claude-desktops\\windows\\desktops\\<name>\\persist) is the guest's Z:. Staging and collecting are plain file
copies through that folder; only starting processes in the guest goes through the fleet (MCP `desktop_call`
`launch`, read_file).

    python e2e/host/e2e_run.py --desktop am-e2e-1 --create --provision -- -v suites/test_smoke.py
    python e2e/host/e2e_run.py --desktop am-e2e-1 -- -v -m p0            # an existing, provisioned desktop
    python e2e/host/e2e_run.py --desktop am-e2e-1 --delete-after -- -v   # and delete the desktop at the end
    python e2e/host/e2e_run.py --desktop am-e2e-1 --attach g1full        # rejoin a run already started
    python e2e/host/e2e_run.py --desktop am-e2e-1 --attach g1full --wait-max 270   # at most 270 s, then exit 97

Waiting costs nothing: the host blocks on a change notification of the persist folder's out\\ folder
(FindFirstChangeNotificationW) and wakes the moment the guest writes there — no sleep interval, no log read. The
guest's pytest appends one line per finished test to <run>.progress.jsonl (guest/progress.py); the host prints each
line as it arrives and ends with a summary: the counts, every failed test with the first line of its failure and its
evidence folder (also written to summary.txt in the results folder). From a Claude session, start a run with the
Bash tool's run_in_background and let its completion notification bring the summary; a blocking call that must stay
short uses --attach … --wait-max.

Results land in Q:\\agentmaster-e2e\\runs\\<run>\\ (pytest.log, junit.xml, the mock's request log, and every failed
test's evidence: screen.png, uia.txt, the profile's logs). The exit code is pytest's (97: --wait-max ended first;
98: the orchestration failed).

⚠ The fleet token is read from the cluster (kubectl) into a variable and never printed or written; desktop
tokens and noVNC links never leave the fleet's answers either (CLAUDE.md, desktops). Windows desktops take
4–6 GB and several cores of this PC: delete them when done (--delete-after, or the fleet's delete_desktop).
"""
from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import json
import msvcrt
import os
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

E2E = Path(__file__).resolve().parents[1]
FLEET_URL = "http://192.168.50.150:7800/mcp"
FLEET_CLIENT = Path(r"K:\source\claude-desktops\fleet")
PERSIST_ROOT = Path(r"Q:\claude-desktops\windows\desktops")
RUNS = Path(r"Q:\agentmaster-e2e\runs")
STAGED = ("amdriver", "mockllm", "suites", "guest", "pytest.ini")
"""What the guest needs of the e2e folder."""
BELT_S = 60.0
"""The longest one wait lasts without a change notification before the host looks at the files anyway. A belt only:
every write of the guest's share reaches this PC's disk as an ordinary NTFS write and wakes the wait at once
(measured 2026-10-23); the belt bounds what a notification that never came would cost."""
START_TIMEOUT_S = 180.0
"""How long a launched run may take to write its first status ({"state": "running"} is the runner's first act)."""
STILL_RUNNING = 97
"""The exit code when --wait-max (or the deadline) ended the wait before the run did."""
ORCHESTRATION_FAILED = 98
"""The exit code when staging, provisioning, the launch or the collection failed."""
OUTCOME_WORDS = {"passed": "PASS", "failed": "FAIL", "error": "ERROR", "skipped": "SKIP", "xfailed": "XFAIL",
                 "xpassed": "XPASS"}
"""How a progress line names a test's outcome."""


def _kernel32() -> ctypes.WinDLL:
    """kernel32 with the prototypes this script calls (declared once: ctypes guesses int for every argument and
    result otherwise, which truncates a 64-bit HANDLE).

    Returns:
        The library object.
    """
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.FindFirstChangeNotificationW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL, wintypes.DWORD]
    k32.FindFirstChangeNotificationW.restype = wintypes.HANDLE
    k32.FindNextChangeNotification.argtypes = [wintypes.HANDLE]
    k32.FindNextChangeNotification.restype = wintypes.BOOL
    k32.FindCloseChangeNotification.argtypes = [wintypes.HANDLE]
    k32.FindCloseChangeNotification.restype = wintypes.BOOL
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.WaitForSingleObject.restype = wintypes.DWORD
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL
    return k32


_K32 = _kernel32()
_INVALID_HANDLE = wintypes.HANDLE(-1).value
"""INVALID_HANDLE_VALUE as ctypes returns a HANDLE (an int)."""


class Fleet:
    """The claude-fleet hub, through its MCP endpoint.

    Attributes:
        client: The McpClient (holds the token; never print it).
    """

    def __init__(self) -> None:
        """Read the fleet token from the cluster and prepare the client.

        Returns:
            None.
        Raises:
            subprocess.CalledProcessError: When kubectl cannot read the secret.
        """
        sys.path.insert(0, str(FLEET_CLIENT))
        from mcp_client import McpClient  # the repo's stdlib client
        raw = subprocess.check_output(["kubectl", "-n", "claude-desktops", "get", "secret", "fleet-token",
                                       "-o", "jsonpath={.data.token}"])
        self.client = McpClient(FLEET_URL, base64.b64decode(raw).decode(), timeout_s=900)

    def tool(self, name: str, arguments: dict) -> str:
        """Call a fleet tool.

        Args:
            name: The tool (list_desktops, create_desktop, delete_desktop, desktop_call …).
            arguments: Its arguments.
        Returns:
            The text of its answer.
        Raises:
            RuntimeError: When the tool reported an error.
        """
        result = self.client.request("tools/call", {"name": name, "arguments": arguments})
        text = "\n".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        if result.get("isError"):
            raise RuntimeError(f"{name} failed: {text[:500]}")
        return text

    def desk(self, desktop: str, tool: str, arguments: dict) -> str:
        """Call a tool of a desktop's agent.

        Args:
            desktop: The desktop's name.
            tool: The desktop tool (launch, read_file, run_command …).
            arguments: Its arguments.
        Returns:
            The text of its answer.
        Raises:
            RuntimeError: When the tool reported an error.
        """
        return self.tool("desktop_call", {"name": desktop, "tool": tool, "arguments": arguments})

    def exists(self, desktop: str) -> bool:
        """Whether a desktop exists.

        Args:
            desktop: The name.
        Returns:
            True when list_desktops names it.
        """
        return f'"{desktop}"' in self.tool("list_desktops", {}) or desktop in self.tool("list_desktops", {})


class FolderWatch:
    """A kernel change notification on one folder: `wait` blocks without using the CPU and returns the moment a file
    in the folder is created, deleted, renamed, resized or written.

    The guest writes its share (Z:) through the desktop's Samba server and Docker Desktop's file sharing, which
    reach this PC's disk as ordinary NTFS writes, so the notification fires for the guest's writes as for local
    ones. Windows records a change that happens while nobody waits and hands it to the next wait, so a change
    between two waits is never lost (FindNextChangeNotification's documented contract).

    Attributes:
        folder: The watched folder.
    """

    _FLAGS = 0x1 | 0x8 | 0x10
    """FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE."""
    _WAIT_OBJECT_0 = 0x0
    _WAIT_TIMEOUT = 0x102

    def __init__(self, folder: Path) -> None:
        """Start watching.

        Args:
            folder: An existing folder.
        Raises:
            OSError: When Windows refuses the notification (the folder is missing, or not a folder).
        """
        self.folder = folder
        handle = _K32.FindFirstChangeNotificationW(str(folder), False, self._FLAGS)
        # FindFirstChangeNotificationW answers INVALID_HANDLE_VALUE (-1) on failure, never NULL; both are refused.
        if not handle or handle == _INVALID_HANDLE:
            raise ctypes.WinError(ctypes.get_last_error())
        self._handle = handle

    def wait(self, timeout_s: float) -> bool:
        """Block until a change or the timeout, then re-arm for the next change.

        Args:
            timeout_s: The longest wait (0 = only ask whether a change is pending).
        Returns:
            True when a change woke it, False on the timeout.
        Raises:
            OSError: When the wait or the re-arm failed (the handle is broken; the caller stops waiting).
        """
        rc = _K32.WaitForSingleObject(self._handle, max(0, int(timeout_s * 1000)))
        if rc == self._WAIT_TIMEOUT:
            return False
        if rc != self._WAIT_OBJECT_0:
            raise ctypes.WinError(ctypes.get_last_error())
        # Re-arm BEFORE the caller reads the files: a write that lands while it reads is recorded against the
        # re-armed handle and ends the next wait at once, so no write between two reads can go unseen.
        if not _K32.FindNextChangeNotification(self._handle):
            raise ctypes.WinError(ctypes.get_last_error())
        return True

    def close(self) -> None:
        """Stop watching (idempotent).

        Returns:
            None.
        """
        if self._handle:
            _K32.FindCloseChangeNotification(self._handle)
            self._handle = None

    def __enter__(self) -> "FolderWatch":
        """Use it as a context manager.

        Returns:
            self.
        """
        return self

    def __exit__(self, *exc) -> None:
        """Close it when the block ends.

        Args:
            *exc: The exception info (not suppressed).
        Returns:
            None.
        """
        self.close()


def read_shared(path: Path) -> bytes | None:
    """Read a whole file while letting its writer replace, truncate or delete it.

    The guest writes its status and progress files while the host reads them, the moment a change wakes it. A
    plain open() shares read and write but not delete, which can make a replace on the other side fail (the
    app's own ROB-K1); this open shares all three and keeps the handle only for the read.

    Args:
        path: The file.
    Returns:
        Its bytes, or None when it does not exist or cannot be read right now (the next change or the belt reads
        it again).
    """
    # GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL.
    handle = _K32.CreateFileW(str(path), 0x80000000, 0x7, None, 3, 0x80, None)
    if not handle or handle == _INVALID_HANDLE:
        return None
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except OSError:
        _K32.CloseHandle(handle)
        return None
    # From here the file object owns the handle: closing it closes the handle (no CloseHandle of our own).
    try:
        with os.fdopen(fd, "rb") as f:
            return f.read()
    except OSError:
        return None


def read_status(path: Path) -> dict | None:
    """The guest's status object.

    Args:
        path: <run>.status.json.
    Returns:
        The object, or None while the file is missing or half written (the guest rewrites it whole).
    """
    data = read_shared(path)
    if not data:
        return None
    try:
        status = json.loads(data.decode("utf-8"))
    except ValueError:
        return None
    return status if isinstance(status, dict) else None


def short_nodeid(nodeid: str) -> str:
    """A test's id without the suites/ folder every test shares.

    Args:
        nodeid: The pytest node id ("suites/test_x.py::test_y").
    Returns:
        "test_x.py::test_y".
    """
    return nodeid[len("suites/"):] if nodeid.startswith("suites/") else nodeid


def safe_name(name: str) -> str:
    """The folder a test's evidence lands in: amdriver.app._safe_name of its name, repeated here so the host does
    not import the driver (it needs psutil and UI Automation, which the host's Python may lack).

    A name over 60 characters keeps its first 51 and 8 hex digits of a hash of the whole name, so the cases of a long
    parametrized test land in different folders (their "[700]" ids sit at the end, which a plain cut dropped).

    Args:
        name: The test function's name (with its parameters, if any).
    Returns:
        Letters, digits, '-', '_' and '.' only, at most 60 characters.
    """
    out = "".join(c if c.isalnum() or c in "-_." else "_" for c in name)
    if len(out) <= 60:
        return out.strip("_") or "test"
    return out[:51].strip("_") + "-" + hashlib.sha1(name.encode("utf-8")).hexdigest()[:8]


class Progress:
    """The guest's progress file (guest/progress.py), read as it grows.

    Attributes:
        path: <run>.progress.jsonl.
        mode: Which lines to print: "all", "failures" or "none".
        offset: How many bytes are consumed (always up to a line's end).
        total: How many tests pytest collected (None before the collected line).
        tests: Every test line read so far.
    """

    def __init__(self, path: Path, mode: str) -> None:
        """Start before the first line.

        Args:
            path: The progress file.
            mode: "all", "failures" or "none".
        """
        self.path = path
        self.mode = mode
        self.offset = 0
        self.total: int | None = None
        self.tests: list[dict] = []

    def read_new(self) -> list[dict]:
        """The lines written since the last call (complete lines only; a line still being written waits for the
        next change).

        Returns:
            The new objects, in order.
        """
        data = read_shared(self.path)
        if data is None:
            return []
        if len(data) < self.offset:
            # Shorter than what was read: the file was started again (a new run under the same name).
            self.offset, self.total, self.tests = 0, None, []
        chunk = data[self.offset:]
        end = chunk.rfind(b"\n")
        if end < 0:
            return []
        self.offset += end + 1
        new = []
        for raw in chunk[:end].splitlines():
            try:
                obj = json.loads(raw.decode("utf-8"))
            except ValueError:
                continue
            if obj.get("event") == "collected":
                self.total = obj.get("total")
            elif obj.get("event") == "test":
                self.tests.append(obj)
            new.append(obj)
        return new

    def format(self, obj: dict) -> str | None:
        """The printed line of one progress object, or None when the mode leaves it out.

        Args:
            obj: A progress object.
        Returns:
            "12/49 FAIL  test_x.py::test_y  91 s — <first line of the failure>", "collected 49 tests", or None.
        """
        if self.mode == "none":
            return None
        if obj.get("event") == "collected":
            return f"collected {obj.get('total')} tests" if self.mode == "all" else None
        if obj.get("event") != "test":
            return None
        outcome = obj.get("outcome", "")
        if self.mode == "failures" and outcome not in ("failed", "error", "xpassed"):
            return None
        word = OUTCOME_WORDS.get(outcome, outcome.upper())
        text = (f"{obj.get('n')}/{obj.get('total', self.total) or '?'} {word:<5} "
                f"{short_nodeid(obj.get('nodeid', ''))}  {obj.get('seconds', 0):.0f} s")
        if obj.get("message"):
            text += " — " + obj["message"]
        return text

    def catch_up(self) -> str:
        """One line for everything read so far (an --attach to a run in flight prints this instead of each line).

        Returns:
            "so far 12/49 finished: 11 passed, 1 failed (test_x.py::test_y)".
        """
        counts: dict[str, int] = {}
        bad = []
        for t in self.tests:
            counts[t.get("outcome", "?")] = counts.get(t.get("outcome", "?"), 0) + 1
            if t.get("outcome") in ("failed", "error", "xpassed"):
                bad.append(short_nodeid(t.get("nodeid", "")))
        parts = ", ".join(f"{n} {k}" for k, n in sorted(counts.items())) or "none yet"
        text = f"so far {len(self.tests)}/{self.total if self.total is not None else '?'} finished: {parts}"
        return text + (f" ({', '.join(bad)})" if bad else "")


def log(message: str) -> None:
    """Print a time-stamped progress line.

    Args:
        message: The text.
    Returns:
        None.
    """
    print(f"[{datetime.now():%H:%M:%S}] {message}", flush=True)


def stage(persist: Path) -> None:
    """Copy the suite into the desktop's persist folder (the guest's Z:\\e2e\\src).

    Args:
        persist: The desktop's persist folder on this PC.
    Returns:
        None.
    """
    src = persist / "e2e" / "src"
    for name in STAGED:
        target = src / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()
        source = E2E / name
        if source.is_dir():
            shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__", ".pytest_cache", "*.pyc"))
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    (persist / "e2e" / "out").mkdir(parents=True, exist_ok=True)
    (persist / "e2e" / "cache").mkdir(parents=True, exist_ok=True)


def file_body(answer: str) -> str:
    """The file's own text from a read_file answer, which starts with a "bytes a-b of n" line.

    Args:
        answer: The agent's answer.
    Returns:
        The text after that line (the whole answer when it has no such line).
    """
    first, _, rest = answer.partition("\n")
    return rest if first.startswith("bytes ") else answer


def wait_guest_file(fleet: Fleet, desktop: str, path: str, deadline_s: float, nonempty: bool = False) -> str:
    """Poll a file in the guest until it exists (and, when asked, holds text).

    The one remaining poll: provisioning writes its marker on the guest's own C:, which no change notification of
    this PC can see.

    ⚠ An empty marker is not a moment caught mid-write: in cmd, `echo %ERRORLEVEL%> file` with a code of 0–9 reads
    `echo 0> file` — a redirection of HANDLE 0 — so the file is created EMPTY and the code is lost (seen
    2026-10-23: a provisioning that had succeeded reported "exit bytes 0-0 of 0", and then, waited for with
    nonempty=True, hung for the whole deadline). A provision.cmd that puts the redirection first
    (`> file echo %ERRORLEVEL%`) writes the code; provision() decides an empty marker by the log. Use nonempty=True
    only with a short deadline.

    Args:
        fleet: The fleet.
        desktop: The desktop.
        path: The guest path.
        deadline_s: How long.
        nonempty: Keep polling while the file exists but is still empty.
    Returns:
        The file's text (without the agent's "bytes a-b of n" line).
    Raises:
        TimeoutError: When it never appeared (or stayed empty).
    """
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        try:
            body = file_body(fleet.desk(desktop, "read_file", {"path": path}))
        except RuntimeError:
            time.sleep(10)
            continue
        if body.strip() or not nonempty:
            return body
        time.sleep(2)
    raise TimeoutError(f"{path} did not appear{' with content' if nonempty else ''} in {deadline_s:.0f} s")


def provision(fleet: Fleet, desktop: str, extra: list[str]) -> None:
    """Provision the guest (Agentmaster, claude.exe, Git, PowerShell 7, the venv, hosts, env, dumps).

    Args:
        fleet: The fleet.
        desktop: The desktop.
        extra: More provision.ps1 arguments (e.g. ["-AgentmasterZip", "Z:\\e2e\\builds\\x.zip"]).
    Returns:
        None.
    Raises:
        RuntimeError: When provisioning failed (its log's tail is in the message), or when the guest's agent refused
            the clean-up or the launch.
    """
    # The PREVIOUS provisioning's marker and log stay on the guest's C: until provision.cmd deletes the marker and
    # truncates the log, and the first poll below can come before that: it then reads the OLD outcome — an empty
    # marker and a log being rewritten (seen 2026-10-23: a re-provisioning with a new zip was reported failed at once,
    # while it went on and succeeded), or worse, "0" and the old log's PROVISION line while the new provisioning still
    # extracts the build. So both go before the launch; a missing file is no error.
    fleet.desk(desktop, "run_command", {"command": "Remove-Item -Force -ErrorAction SilentlyContinue "
                                                   "C:/e2e/provision.done, C:/e2e/provision.log", "timeout_s": 60})
    fleet.desk(desktop, "launch", {"command": "cmd /c Z:\\e2e\\src\\guest\\provision.cmd -Pwsh " + " ".join(extra),
                                   "cwd": "C:\\"})
    # The marker exists once provision.ps1 has exited. It may be empty: give a provision.cmd that writes the code
    # a moment to have written it, then let the log decide (provision_outcome).
    marker = wait_guest_file(fleet, desktop, "C:/e2e/provision.done", 1800)
    if not marker.strip():
        try:
            marker = wait_guest_file(fleet, desktop, "C:/e2e/provision.done", 10, nonempty=True)
        except TimeoutError:
            marker = ""
    log_text = file_body(fleet.desk(desktop, "read_file", {"path": "C:/e2e/provision.log"}))
    ok, how = provision_outcome(marker, log_text)
    tail = log_text[-1500:]
    if not ok:
        raise RuntimeError(f"provisioning failed ({how}):\n{tail}")
    log(f"provisioned ({how}): " + tail.strip().splitlines()[-1][:300])


def provision_outcome(marker: str, log_text: str) -> tuple[bool, str]:
    """Whether provisioning succeeded, from its marker file and its log.

    The marker holds provision.ps1's exit code — unless an older provision.cmd wrote it with
    `echo %ERRORLEVEL%> file`, which leaves it empty (wait_guest_file's warning). provision.ps1 always ends its log
    with one "PROVISION {json}" line, every step's outcome; a failed step's value starts with "FAILED". So an empty
    marker is decided by that line: success when it exists, parses, and no step failed.

    Args:
        marker: The marker's text ("" when empty).
        log_text: provision.log's text.
    Returns:
        (succeeded, how it was decided — "exit 0", "exit 1", or what the log said for an empty marker).
    """
    code = marker.strip().splitlines()[-1].strip() if marker.strip() else ""
    if code:
        return code == "0", f"exit {code}"
    lines = [line for line in log_text.strip().splitlines() if line.strip()]
    last = lines[-1] if lines else ""
    if not last.startswith("PROVISION "):
        return False, "an empty marker, and the log does not end with its PROVISION line"
    try:
        summary = json.loads(last[len("PROVISION "):])
    except ValueError:
        return False, "an empty marker, and the log's PROVISION line does not parse"
    failed = [k for k, v in summary.items() if str(v).startswith("FAILED")] if isinstance(summary, dict) else ["?"]
    if failed:
        return False, f"an empty marker; the log names failed steps: {', '.join(failed)}"
    return True, "an empty marker (a provision.cmd from before its fix); the log's PROVISION line has no failed step"


def out_folder(persist: Path) -> Path:
    """The folder the guest writes its run files into (its Z:\\e2e\\out).

    Args:
        persist: The desktop's persist folder.
    Returns:
        <persist>\\e2e\\out.
    """
    return persist / "e2e" / "out"


def launch(fleet: Fleet, desktop: str, persist: Path, name: str, pytest_args: list[str]) -> None:
    """Start the suite in the guest, detached (the guest's runner goes on even when this process ends).

    Args:
        fleet: The fleet.
        desktop: The desktop.
        persist: Its persist folder.
        name: The run's name.
        pytest_args: Arguments for pytest.
    Returns:
        None.
    Raises:
        RuntimeError: When the desktop's agent refused the launch.
    """
    out = out_folder(persist)
    # An earlier run under the same name must not be read as this one: its status and its progress go first
    # (the guest's runner deletes its progress file too, but only once it runs).
    (out / f"{name}.status.json").unlink(missing_ok=True)
    (out / f"{name}.progress.jsonl").unlink(missing_ok=True)
    command = "C:\\e2e\\venv\\Scripts\\python.exe Z:\\e2e\\src\\guest\\run_suite.py " + name + " " + " ".join(pytest_args)
    fleet.desk(desktop, "launch", {"command": command, "cwd": "C:\\e2e"})


def follow(persist: Path, name: str, wait_s: float, progress_mode: str, attach: bool) -> dict | None:
    """Wait for the run to end, printing its progress as the guest writes it.

    The wait is a change notification of the out folder (FolderWatch): it wakes on each of the guest's writes —
    a progress line, the status file — and costs nothing in between. No sleep interval, no log read.

    Args:
        persist: The desktop's persist folder.
        name: The run's name.
        wait_s: How long to wait at most.
        progress_mode: "all", "failures" or "none" (which progress lines to print).
        attach: Joining a run in flight: everything already finished is printed as one catch-up line instead of a
            line per test, and a run that never started is refused at once.
    Returns:
        The final status object, or None when the wait ended first (the run goes on in the guest).
    Raises:
        OSError: When the out folder cannot be watched.
        SystemExit: When the out folder is missing, or --attach names a run that never started (no status file).
        RuntimeError: When a launched run's status file did not appear within START_TIMEOUT_S (the guest's runner
            never started: a wrong venv, a missing share).
    """
    out = out_folder(persist)
    if not out.is_dir():
        raise SystemExit(f"{out} does not exist (was the suite ever staged into this desktop?)")
    status_file = out / f"{name}.status.json"
    progress = Progress(out / f"{name}.progress.jsonl", progress_mode)
    begun = time.monotonic()
    end = begun + wait_s
    first = True
    state = ""
    with FolderWatch(out) as watch:
        while True:
            new = progress.read_new()
            status = read_status(status_file)
            done = bool(status) and status.get("state") == "done"
            if first and attach:
                if status is None and not status_file.exists():
                    raise SystemExit(f"no run named {name} in {out} (no {status_file.name})")
                if progress.tests and not done:
                    log(progress.catch_up())
                # The tests already finished were summed up in the catch-up line (or are summed up by the end
                # summary); only what arrives from now on gets a line of its own.
                new = [] if progress.tests else new
            for obj in new:
                line = progress.format(obj)
                if line:
                    log(line)
            first = False
            if done:
                return status
            if status and status.get("state", "") != state:
                state = status.get("state", "")
                log(f"run {name}: {state}")
            if not attach and not state and not status_file.exists() and time.monotonic() - begun > START_TIMEOUT_S:
                raise RuntimeError(f"the guest's runner did not start: no {status_file.name} after "
                                   f"{START_TIMEOUT_S:.0f} s")
            remaining = end - time.monotonic()
            if remaining <= 0:
                return None
            watch.wait(min(remaining, BELT_S))


def collect(persist: Path, name: str) -> Path:
    """Unpack the run's results zip into the runs folder.

    Args:
        persist: The desktop's persist folder.
        name: The run's name.
    Returns:
        The results folder.
    """
    target = RUNS / name
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    with zipfile.ZipFile(out_folder(persist) / f"{name}.zip") as z:
        z.extractall(target)
    return target


def summary_lines(results: Path, status: dict) -> list[str]:
    """The end of a run in a few lines: the counts, then every failed test with its first line and its evidence.

    Args:
        results: The collected results folder.
        status: The guest's final status object.
    Returns:
        The lines (without time stamps).
    """
    tests, failures, errors, skipped = (int(status.get(k) or 0) for k in ("tests", "failures", "errors", "skipped"))
    seconds = float(status.get("seconds") or 0)
    lines = [f"done: exit {status.get('exit')} — {tests} tests: {tests - failures - errors - skipped} passed, "
             f"{failures} failed, {errors} errors, {skipped} skipped, {seconds / 60:.1f} min"]
    if status.get("runner_error"):
        lines.append("  the guest's runner failed: " + status["runner_error"].strip().splitlines()[-1][:300])
    try:
        root = ET.parse(results / "junit.xml").getroot()
    except (OSError, ET.ParseError) as e:
        lines.append(f"  (no readable junit.xml: {e})")
        root = None
    if root is not None:
        for case in root.iter("testcase"):
            for kind, word in (("failure", "FAIL"), ("error", "ERROR")):
                element = case.find(kind)
                if element is None:
                    continue
                module = (case.get("classname") or "").rsplit(".", 1)[-1]
                test = case.get("name") or "?"
                text = (element.get("message") or element.text or "").strip()
                first = text.splitlines()[0][:300] if text else ""
                lines.append(f"  {word:<5} {module}.py::{test} ({float(case.get('time') or 0):.0f} s): {first}")
                evidence = results / "tests" / safe_name(test)
                if evidence.is_dir():
                    lines.append(f"        evidence: {evidence}")
    lines.append(f"  results: {results} (pytest.log, junit.xml, summary.txt)")
    return lines


def main(argv: list[str]) -> int:
    """The command line.

    Args:
        argv: The arguments; everything after "--" goes to pytest.
    Returns:
        pytest's exit code; 97 when --wait-max or the deadline ended the wait first (the run goes on in the
        guest: --attach it); 98 when the orchestration failed.
    Raises:
        SystemExit: When the desktop does not exist and --create was not given, or --attach names no run.
        RuntimeError / TimeoutError / OSError: When provisioning, the launch, the wait or the collection failed (the
            desktop is still deleted when --delete-after was given).
    """
    pytest_args: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, pytest_args = argv[:i], argv[i + 1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--desktop", default="am-e2e-1")
    parser.add_argument("--create", action="store_true", help="create the desktop when it does not exist")
    parser.add_argument("--memory-gb", type=int, default=8)
    parser.add_argument("--cpus", type=int, default=6)
    parser.add_argument("--provision", action="store_true", help="run provision.ps1 first")
    parser.add_argument("--provision-arg", action="append", default=[], help="an extra provision.ps1 argument")
    parser.add_argument("--run", default="", help="the run's name (default: a time stamp)")
    parser.add_argument("--attach", default="", metavar="RUN",
                        help="join a run already started (no create, stage, provision or launch): wait, collect, sum up")
    parser.add_argument("--deadline-min", type=float, default=120, help="how long a run may take")
    parser.add_argument("--wait-max", type=float, default=0, metavar="SEC",
                        help="wait at most this long, then exit 97 if the run goes on (0: up to the deadline)")
    parser.add_argument("--progress", choices=("all", "failures", "none"), default="all",
                        help="which per-test lines to print while the run goes on")
    parser.add_argument("--delete-after", action="store_true", help="delete the desktop when done")
    args = parser.parse_args(argv)
    name = args.attach or args.run or f"run-{datetime.now():%Y%m%d-%H%M%S}"
    persist = PERSIST_ROOT / args.desktop / "persist"
    wait_s = min(args.wait_max, args.deadline_min * 60) if args.wait_max > 0 else args.deadline_min * 60
    fleet = None if args.attach and not args.delete_after else Fleet()
    code = ORCHESTRATION_FAILED
    try:
        if not args.attach:
            if not fleet.exists(args.desktop):
                if not args.create:
                    raise SystemExit(f"desktop {args.desktop} does not exist (pass --create)")
                log(f"creating {args.desktop}")
                fleet.tool("create_desktop", {"name": args.desktop, "os": "windows", "width": 1920, "height": 1080,
                                              "memory_gb": args.memory_gb, "cpus": args.cpus, "wait": True})
            log(f"staging into {persist}")
            stage(persist)
            if args.provision:
                log("provisioning (Agentmaster, claude.exe, Git, PowerShell 7, the venv) …")
                provision(fleet, args.desktop, args.provision_arg)
            log(f"running {name}: pytest {' '.join(pytest_args)}")
            launch(fleet, args.desktop, persist, name, pytest_args)
        else:
            log(f"attaching to {name} in {out_folder(persist)}")
        status = follow(persist, name, wait_s, args.progress, attach=bool(args.attach))
        if status is None:
            log(f"still running after {wait_s:.0f} s — the guest goes on; rejoin with --attach {name}")
            code = STILL_RUNNING
        else:
            results = collect(persist, name)
            lines = summary_lines(results, status)
            (results / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
            for line in lines:
                print(line if line.startswith("  ") else f"[{datetime.now():%H:%M:%S}] {line}", flush=True)
            code = int(status.get("exit", ORCHESTRATION_FAILED))
    finally:
        if args.delete_after:
            log(f"deleting {args.desktop}")
            (fleet or Fleet()).tool("delete_desktop", {"name": args.desktop})
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
