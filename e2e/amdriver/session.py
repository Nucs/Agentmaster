"""A managed Claude session as a test handles it: launch it, talk to it, find it on every surface.

`launch_session(app, folder)` is fixture F-S's real path (doc/agentmaster/flows/README.md): type the folder
into the Agent Manager's launch row and click Launch Claude; the engine logs `[nav] open-new claude done
new=<sid8>`, sessions.json gets the record, a tab titled by the folder opens selected, and Claude Code's input
box appears in it.

`Session.ask(prompt)` types a prompt into the session's terminal and confirms it through the oracles the app
does not control: the MOCK received it (the turn started), then sessions.json says the turn ended
(WaitingForInput with a turn end stamped after the prompt).
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from .profile import SessionRecord
from .waits import wait_until


@dataclass
class Session:
    """A managed session of the app under test.

    Attributes:
        app: The App.
        id: The full conversation id.
        folder: Its working folder.
        mock: The MockClient (the prompts' confirmation), or None.
    """

    app: object
    id: str
    folder: Path
    mock: object = None

    @property
    def short(self) -> str:
        """The 8-character id the logs print.

        Returns:
            The short id.
        """
        return self.id[:8]

    def record(self) -> SessionRecord:
        """The session's record in sessions.json now.

        Returns:
            The record.
        Raises:
            LookupError: When sessions.json does not hold it.
        """
        rec = self.app.profile.session(self.id)
        if rec is None:
            raise LookupError(f"session {self.short} is not in sessions.json")
        return rec

    @property
    def title(self) -> str:
        """The session's title (== its tab's title, Rule #11).

        Returns:
            The title.
        """
        return self.record().title

    @property
    def state(self) -> str:
        """The persisted state.

        Returns:
            One of profile.STATES.
        """
        return self.record().state

    def wait_state(self, *states: str, timeout_s: float = 30.0) -> SessionRecord:
        """Wait for a persisted state.

        Args:
            *states: Accepted states.
            timeout_s: How long.
        Returns:
            The record.
        Raises:
            WaitTimeout: When it never got there.
        """
        return self.app.profile.wait_state(self.id, *states, timeout_s=timeout_s)

    def window(self):
        """The window whose strip holds the session's tab.

        Returns:
            The MainWindow.
        Raises:
            LookupError: When no window shows its tab.
        """
        title = self.title
        for w in self.app.windows():
            if w.tabs.find(title) is not None:
                return w
        raise LookupError(f"no window holds tab {title!r}")

    def tab(self):
        """The session's tab.

        Returns:
            The Tab.
        Raises:
            LookupError: When no window holds it.
        """
        return self.window().tabs.get(self.title)

    def select(self):
        """Select the session's tab (a real click), leaving the Agent Manager page if it shows.

        Returns:
            The window.
        Raises:
            WaitTimeout: When the tab did not get selected.
        """
        w = self.window()
        w.tabs.select(self.title)
        return w

    def claude_pids(self) -> list[int]:
        """The claude.exe processes started for this session (the command line names its id).

        Returns:
            The pids.
        """
        out = []
        for p in self.app.claude_processes():
            try:
                if self.id in " ".join(p.cmdline()):
                    out.append(p.pid)
            except Exception:  # noqa: BLE001 - a process gone between listing and reading
                continue
        return out

    def ask(self, prompt: str, wait: str = "turn", timeout_s: float = 60.0) -> None:
        """Type a prompt into the session's terminal and confirm it.

        Args:
            prompt: The prompt (a mock script inside `[[mock …]]`, or plain words: the mock answers "OK").
            wait: "turn" (the mock got it AND the turn ended: WaitingForInput, turn end after the prompt),
                "sent" (the mock got it), or "none".
            timeout_s: How long each wait may take.
        Returns:
            None.
        Raises:
            WaitTimeout / MockError: When the prompt did not arrive or the turn did not end.
        """
        w = self.select()
        terminal = w.terminal
        terminal.wait_input_box(timeout_s=timeout_s)
        seq = self.mock.last_seq() if self.mock is not None else 0
        started_ms = int(time.time() * 1000) - 1000
        terminal.submit(prompt)
        if wait == "none":
            return
        if self.mock is not None:
            marker = prompt.split("[[", 1)[0].strip() or prompt.strip()
            self.mock.wait_prompt(marker[:60], since=seq, timeout_s=timeout_s)
        if wait == "turn":
            self.app.profile.wait_session(
                lambda s: s.id == self.id and s.state == "WaitingForInput"
                and int(s.raw.get("turnEndUnixMs") or 0) >= started_ms,
                timeout_s, what=f"session {self.short}'s turn to end")


def launch_session(app, folder: Path, mock=None, ready: bool = True, timeout_s: float = 60.0) -> Session:
    """Launch a managed Claude session through the Agent Manager's launch row (fixture F-S's real path).

    Args:
        app: The App.
        folder: An existing working folder (its last name becomes the title).
        mock: The MockClient for the session's prompts.
        ready: Wait for Claude Code's input box in the new tab.
        timeout_s: How long the launch may take.
    Returns:
        The Session.
    Raises:
        WaitTimeout: When the engine did not report the launch, or the tab / the box never appeared.
    """
    window = app.window
    mark = app.profile.hooks.mark()
    window.manager.launch(str(folder))
    m = app.profile.hooks.wait(r"\[nav\] open-new claude done new=([0-9a-f]{8})", since=mark, timeout_s=timeout_s)
    rec = app.profile.wait_session(lambda s: s.id.startswith(m.group(1)), timeout_s,
                                   what=f"session {m.group(1)} in sessions.json")
    session = Session(app, rec.id, Path(folder), mock)
    window.tabs.wait_selected(rec.title, timeout_s=timeout_s)
    if ready:
        window.terminal.wait_input_box(timeout_s=timeout_s)
    return session


def wait_new_session(app, mark: int, timeout_s: float = 60.0, pattern: str = r"new=([0-9a-f]{8})",
                     mock=None) -> Session:
    """The session a menu action opened (fork, new session here): read its id from the log line after a mark.

    Args:
        app: The App.
        mark: A hooks.log mark taken before the action.
        timeout_s: How long.
        pattern: A regex whose group 1 is the new session's 8-character id.
        mock: The MockClient for its prompts.
    Returns:
        The Session.
    Raises:
        WaitTimeout: When no such line or record appeared.
    """
    m = app.profile.hooks.wait(pattern, since=mark, timeout_s=timeout_s)
    rec = app.profile.wait_session(lambda s: s.id.startswith(m.group(1)), timeout_s,
                                   what=f"session {m.group(1)} in sessions.json")
    return Session(app, rec.id, Path(rec.working_dir), mock)


def wait_until_no_claude(app, session: Optional[Session] = None, timeout_s: float = 30.0) -> None:
    """Wait until a session's claude.exe (or every claude of the app) has exited.

    Args:
        app: The App.
        session: The session (None = all of the app's claudes).
        timeout_s: How long (a graceful CTRL_CLOSE ends a claude in ~1–2.5 s).
    Returns:
        None.
    Raises:
        WaitTimeout: When a claude stays.
    """
    if session is not None:
        wait_until(lambda: not session.claude_pids(), timeout_s, what=f"session {session.short}'s claude to exit")
    else:
        wait_until(lambda: not app.claude_processes(), timeout_s, what="every claude to exit")
