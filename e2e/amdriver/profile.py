"""The Agentmaster profile's files: the engine's record of sessions and windows, the settings, and the logs.

These are the [ENG] and [LOG] oracles of the flow catalog (doc/agentmaster/flows/README.md) until the
read-only test bridge exists (E2E_TESTING.md §6.2):
- sessions.json — every session the engine knows; written ≤ ~1 s after a change (the debounced autosaver:
  300 ms quiet or a 1 s cap; Engine.cpp), and at once for a close. `live` is NOT persisted.
- windows\\<id>.json — per-window UI state: geometry, the Manager lens, the ordered tab refs.
- settings.json — the Settings cog's values, inside a {"version", "settings": {...}} envelope.
- hooks.log / autorunner.log / scanner.log — every line stamped [HH:MM:SS.mmm]; `[nav]` lines are the user's
  journey (CLAUDE.md *Logging & observability*).

A log is read from a MARK (a byte offset taken before the action), so a check sees only what the action
caused and never an earlier test's or an earlier phase's line.

⚠ Every file here is read through win32.read_bytes_shared (a handle held only for the read): the app saves its JSON
files by replacing them (a temp file + MoveFileExW), and a save that meets ANY open handle FAILS and is not retried
(flows ROB-K1; delete sharing does not help that replace, measured 2026-10-23) — so a test that polls these files
often can still change what it measures. Prefer a log line where the fact is logged.
"""
from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Callable, Optional

from .waits import wait_until
from .win32 import read_bytes_shared

STATES = ("Idle", "Running", "WaitingForInput", "NeedsApproval", "Error", "Done")
"""SessionState's persisted names (Persistence.cpp)."""


@dataclass
class SessionRecord:
    """One session as sessions.json records it.

    Attributes:
        id: The conversation id (a GUID).
        title: The one title (tab == Explorer name == this; CLAUDE.md Rule #11).
        working_dir: The launch folder.
        state: One of STATES.
        external: Adopted (a claude the user typed), not launched by Agentmaster.
        kind: "Claude" or "Codex" (absent = Claude).
        fork_parent_id: The source of a fork that has no transcript of its own yet.
        raw: The whole JSON object.
    """

    id: str
    title: str
    working_dir: str
    state: str
    external: bool
    kind: str
    fork_parent_id: str
    raw: dict

    @property
    def short(self) -> str:
        """The first 8 characters of the id (the logs' convention).

        Returns:
            The short id.
        """
        return self.id[:8]

    @staticmethod
    def from_json(obj: dict) -> "SessionRecord":
        """Read one record.

        Args:
            obj: A sessions.json entry.
        Returns:
            The record.
        """
        return SessionRecord(id=obj.get("id", ""), title=obj.get("title", ""), working_dir=obj.get("workingDir", ""),
                             state=obj.get("state", ""), external=bool(obj.get("external", False)),
                             kind=obj.get("kind", "Claude") or "Claude", fork_parent_id=obj.get("forkParentId", ""),
                             raw=obj)


class LogFile:
    """One of the profile's logs, read from marks.

    Attributes:
        path: The file.
    """

    STAMP = re.compile(r"^\[\d\d:\d\d:\d\d\.\d{3}\] ")

    def __init__(self, path: Path) -> None:
        """Bind to a log file (it may not exist yet).

        Args:
            path: The file.
        Returns:
            None.
        """
        self.path = Path(path)

    def mark(self) -> int:
        """The current end of the log: read from here to see only what comes next.

        Returns:
            The byte offset (0 when the log does not exist yet).
        """
        try:
            return self.path.stat().st_size
        except FileNotFoundError:
            return 0

    def text(self, since: int = 0) -> str:
        """The log's text from a mark.

        Args:
            since: A mark.
        Returns:
            The text (UTF-8; a broken byte is replaced). Empty when the log does not exist.
        """
        try:
            return read_bytes_shared(self.path)[since:].decode("utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def lines(self, since: int = 0) -> list[str]:
        """The log's complete lines from a mark, without their time stamps.

        Args:
            since: A mark.
        Returns:
            The lines (a last line still being written is left out).
        """
        text = self.text(since)
        if text and not text.endswith("\n"):
            text = text[: text.rfind("\n") + 1]
        return [self.STAMP.sub("", line) for line in text.splitlines()]

    def find(self, pattern: str, since: int = 0) -> list[re.Match]:
        """Every line matching a regex, from a mark.

        Args:
            pattern: A regex searched in each line.
            since: A mark.
        Returns:
            The matches, in log order.
        """
        rx = re.compile(pattern)
        return [m for m in (rx.search(line) for line in self.lines(since)) if m]

    def stamped(self, pattern: str, since: int = 0) -> list[tuple[datetime, re.Match]]:
        """Every line matching a regex, from a mark, with the moment the app wrote it — for a check that is about
        WHEN things happened (a race's order, a debounce), not only whether they did.

        The app stamps each line with its local time of day ([HH:MM:SS.mmm], AppendStateLog), the clock this
        process reads too (the test and the app run on one machine), so a stamp compares with datetime.now(). The
        date is today's; a stamp more than 12 h ahead of now is read as yesterday's (a log read just after
        midnight).

        Args:
            pattern: A regex searched in each line (without its stamp).
            since: A mark.
        Returns:
            (when, match) pairs in log order; a line without a stamp (a continuation line) is left out, and so is a
            last line still being written.
        """
        rx = re.compile(pattern)
        text = self.text(since)
        if text and not text.endswith("\n"):
            text = text[: text.rfind("\n") + 1]
        now = datetime.now()
        found: list[tuple[datetime, re.Match]] = []
        for line in text.splitlines():
            stamp = self.STAMP.match(line)
            if not stamp:
                continue
            m = rx.search(line[stamp.end():])
            if not m:
                continue
            clock = datetime.strptime(stamp.group(0)[1:13], "%H:%M:%S.%f").time()
            when = datetime.combine(now.date(), clock)
            if when - now > timedelta(hours=12):
                when -= timedelta(days=1)
            found.append((when, m))
        return found

    def wait(self, pattern: str, since: int = 0, timeout_s: float = 15.0) -> re.Match:
        """Wait for a line matching a regex after a mark.

        Args:
            pattern: A regex.
            since: A mark (take it BEFORE the action that should log).
            timeout_s: How long.
        Returns:
            The first match.
        Raises:
            WaitTimeout: When no such line came; the message holds the log's last lines.
        """
        return wait_until(lambda: (self.find(pattern, since) or [None])[0], timeout_s,
                          what=f"a line /{pattern}/ in {self.path.name}",
                          describe=lambda: self.lines(since)[-12:])


class Profile:
    """An Agentmaster profile folder.

    Attributes:
        dir: The folder (AGENTMASTER_PROFILE).
        hooks: hooks.log (the hook stream, the mechanism tags, the [nav] trail).
        autorunner: autorunner.log (the Prompt Queue Runner's decisions).
        scanner: scanner.log (the transcript reconciler's synthesized states).
    """

    def __init__(self, path: Path) -> None:
        """Bind to a profile folder.

        Args:
            path: The folder.
        Returns:
            None.
        """
        self.dir = Path(path)
        self.hooks = LogFile(self.dir / "hooks.log")
        self.autorunner = LogFile(self.dir / "autorunner.log")
        self.scanner = LogFile(self.dir / "scanner.log")

    # ---- sessions ---------------------------------------------------------------------------------

    def _read_json(self, path: Path, default: Any) -> Any:
        """Read a JSON file the app writes atomically (a temp file then a rename), retrying a torn read.

        Args:
            path: The file.
            default: What a missing file reads as.
        Returns:
            The parsed value.
        Raises:
            ValueError: When the file stays unparseable or unopenable for ~1 s (a real corruption, which is a
                finding).
        """
        last: Optional[Exception] = None
        for _ in range(10):
            try:
                return json.loads(read_bytes_shared(path).decode("utf-8-sig"))
            except FileNotFoundError:
                return default
            except (ValueError, OSError) as error:
                # A torn read, or an open that met the replace itself (a delete-pending target answers
                # ERROR_ACCESS_DENIED for an instant): read again.
                last = error
                time.sleep(0.1)
        raise ValueError(f"{path} stays unreadable: {last}")

    def sessions(self) -> list[SessionRecord]:
        """Every session in sessions.json.

        Returns:
            The records (empty before the first save).
        """
        data = self._read_json(self.dir / "sessions.json", {})
        return [SessionRecord.from_json(s) for s in data.get("sessions", [])]

    def session(self, id_or_prefix: str) -> Optional[SessionRecord]:
        """One session by its id or an id prefix (the logs' 8-character form).

        Args:
            id_or_prefix: The id or its start.
        Returns:
            The record, or None.
        """
        for s in self.sessions():
            if s.id.startswith(id_or_prefix):
                return s
        return None

    def wait_session(self, pred: Callable[[SessionRecord], bool], timeout_s: float = 15.0,
                     what: str = "a session") -> SessionRecord:
        """Wait for a session record that satisfies a predicate.

        Args:
            pred: The condition on one record.
            timeout_s: How long (the autosave lags ≤ ~1 s).
            what: A description for the timeout.
        Returns:
            The first matching record.
        Raises:
            WaitTimeout: When none appeared.
        """
        return wait_until(lambda: next((s for s in self.sessions() if pred(s)), None), timeout_s, what=what,
                          describe=lambda: [(s.short, s.title, s.state) for s in self.sessions()])

    def wait_state(self, session_id: str, *states: str, timeout_s: float = 30.0) -> SessionRecord:
        """Wait until a session's persisted state is one of the given ones.

        Args:
            session_id: The id (or prefix).
            *states: Accepted STATES.
            timeout_s: How long.
        Returns:
            The record.
        Raises:
            WaitTimeout: When it never got there.
        """
        bad = [s for s in states if s not in STATES]
        if bad:
            raise ValueError(f"unknown states {bad}; known: {STATES}")
        return self.wait_session(lambda s: s.id.startswith(session_id) and s.state in states, timeout_s,
                                 what=f"session {session_id[:8]} in {'/'.join(states)}")

    # ---- windows, settings, stores ----------------------------------------------------------------

    def window_records(self) -> list[dict]:
        """Every windows\\<id>.json record.

        Returns:
            The parsed records, sorted by file name (the reopen order).
        """
        folder = self.dir / "windows"
        if not folder.is_dir():
            return []
        return [self._read_json(p, {}) for p in sorted(folder.glob("*.json"))]

    def open_windows(self) -> list[str]:
        """The open-at-exit manifest (the window ids the next start reopens).

        Returns:
            The ids (braced GUIDs as written).
        """
        path = self.dir / "open-windows.json"
        if not path.exists():
            return []
        return re.findall(r"\{[0-9A-Fa-f-]{36}\}", read_bytes_shared(path).decode("utf-8", errors="replace"))

    def settings(self) -> dict:
        """The Settings cog's values.

        Returns:
            The inner "settings" object (empty when the file does not exist).
        """
        data = self._read_json(self.dir / "settings.json", {})
        return data.get("settings", data) if isinstance(data, dict) else {}

    def write_settings(self, values: dict) -> None:
        """Merge values into settings.json. Only BEFORE the app starts (the app reads it at start and rewrites
        it on a Save).

        Args:
            values: AppSettings keys and values (e.g. {"showTabOverlay": False}).
        Returns:
            None.
        """
        path = self.dir / "settings.json"
        data = self._read_json(path, {})
        if "settings" not in data:
            data = {"version": 1, "settings": data}
        data["settings"].update(values)
        self.dir.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2), encoding="utf-8")

    def session_store(self, session_id: str) -> dict:
        """A session's durable store record (title, favorite, tags, draft, launchArgs).

        Args:
            session_id: The full id.
        Returns:
            The parsed record (empty when none).
        """
        return self._read_json(self.dir / "session-store" / f"{session_id}.json", {})

    def dir_colors(self) -> dict:
        """The permanent folder -> tab color map.

        Returns:
            The parsed dir-colors.json (empty when none).
        """
        return self._read_json(self.dir / "dir-colors.json", {})
