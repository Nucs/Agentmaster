"""A managed Claude session as a test handles it: launch it, talk to it, find it on every surface.

`launch_session(app, folder)` is fixture F-S's real path (doc/agentmaster/flows/README.md): type the folder
into the Agent Manager's launch row and click Launch Claude; the engine logs `[nav] open-new claude done
new=<sid8>`, sessions.json gets the record and a tab titled by the folder opens. Since MGR-K1's fix (2026-10-23)
the page STAYS: the tab opens behind it, its claude starts in place and the page selects the session — Home reads
"Jump back to <title>" — so the fixture clicks Home, the user's way to the new tab. A build before the fix (the
published 0.8.0) selects the tab itself. Either way the test then stands on the tab with Claude Code's input box.

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

    def claude_cmdlines(self) -> list[str]:
        """The command lines of the session's claude.exe processes (how Agentmaster started it: --resume /
        --session-id / --fork-session / --model …).

        Returns:
            The command lines.
        """
        out = []
        for p in self.app.claude_processes():
            try:
                line = " ".join(p.cmdline())
            except Exception:  # noqa: BLE001 - a process gone between listing and reading
                continue
            if self.id in line:
                out.append(line)
        return out

    def running(self) -> bool:
        """Whether the session's claude.exe runs now (a parked or restored-dormant tab has none).

        Returns:
            True while a claude of this session runs.
        """
        return bool(self.claude_pids())

    def wait_running(self, timeout_s: float = 30.0) -> list[int]:
        """Wait for the session's claude to start (a wake: the tab entered, Activate, Activate All).

        Args:
            timeout_s: How long.
        Returns:
            Its pids.
        Raises:
            WaitTimeout: When no claude started.
        """
        return wait_until(self.claude_pids, timeout_s, what=f"session {self.short}'s claude to start")

    def wait_parked(self, since: int = 0, timeout_s: float = 30.0) -> None:
        """Wait until the session is PARKED: the engine logged `[deactivate] <sid8> claude parked dormant …` after
        the mark, and its claude has exited (CTRL_CLOSE through the old ConPTY: ~1–2.5 s, at most 5 s).

        Args:
            since: A hooks.log mark taken before the Deactivate.
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When the park was not logged or the claude stayed.
        """
        self.app.profile.hooks.wait(rf"\[deactivate\] {self.short} claude parked dormant", since=since,
                                    timeout_s=timeout_s)
        wait_until(lambda: not self.claude_pids(), timeout_s, what=f"session {self.short}'s claude to exit")

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
    """Launch a managed Claude session through the Agent Manager's launch row (fixture F-S's real path) and end on
    its tab.

    Two builds, one result (see the module docstring): with MGR-K1 fixed the page stays and selects the new session
    (`[nav] manager-page launch <sid8> …` in hooks.log, Home's name "Jump back to <title>"), and the fixture clicks
    Home to reach the tab; the published 0.8.0 selects the tab itself. The page-stays behavior is asserted by its own
    test (test_manager_page.py, MGR-07), not here.

    Args:
        app: The App.
        folder: An existing working folder (its last name becomes the title).
        mock: The MockClient for the session's prompts.
        ready: Wait for Claude Code's input box in the new tab.
        timeout_s: How long the launch may take.
    Returns:
        The Session.
    Raises:
        WaitTimeout: When the engine did not report the launch, the launch settled neither way, Home did not offer
            the way back, or the tab / the box never appeared.
    """
    window = app.window
    mark = app.profile.hooks.mark()
    window.manager.launch(str(folder))
    m = app.profile.hooks.wait(r"\[nav\] open-new claude done new=([0-9a-f]{8})", since=mark, timeout_s=timeout_s)
    rec = app.profile.wait_session(lambda s: s.id.startswith(m.group(1)), timeout_s,
                                   what=f"session {m.group(1)} in sessions.json")
    session = Session(app, rec.id, Path(folder), mock)

    def settled() -> Optional[str]:
        """One poll: how the launch ended.

        Returns:
            "page" (the page stayed and selected the session), "tab" (the tab was selected: a build before the
            fix), or None while neither is true yet.
        """
        if app.profile.hooks.find(rf"\[nav\] manager-page launch {m.group(1)} ", since=mark):
            return "page"
        selected = window.tabs.selected()
        if selected is not None and selected.title == rec.title and not window.manager.showing():
            return "tab"
        return None

    how = wait_until(settled, timeout_s, what=f"the launch of {m.group(1)} to settle (on the page or on its tab)")
    if how == "page":
        wait_until(lambda: window.manager.home_name() == f"Jump back to {rec.title}", 15.0,
                   what=f"Home to offer the way back to {rec.title!r}", describe=window.manager.home_name)
        window.manager.click_home()
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
