"""The Agentmaster process under test: start it isolated, find its windows, close it, kill it, start it again.

Isolation (E2E_TESTING.md §5.3, Appendix A): every test gets its own folders under C:\\e2e —
    p\\<name>   the Agentmaster profile (AGENTMASTER_PROFILE: sessions.json, window records, the logs)
    c\\<name>   Claude Code's config folder (CLAUDE_CONFIG_DIR: .claude.json, settings, projects\\ = transcripts)
    w\\<name>   the working folders the test's sessions run in (w\\<name>\\t1, t2, ...)
    out\\tests\\<name> the test's evidence (screenshots, UIA dumps, log copies) when it fails
— so a test reads only what it made, and a test that leaves state behind cannot break the next one.

Every Claude Code started by Agentmaster inherits the app's environment, so the mock model's address and the
switches that keep Claude Code offline are set on the app's process here (ANTHROPIC_BASE_URL + a fake
ANTHROPIC_AUTH_TOKEN — never ANTHROPIC_API_KEY, which opens Claude Code's unanswerable "use this API key?"
dialog; DISABLE_AUTOUPDATER and CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC so nothing replaces the pinned
claude.exe or calls home).

⚠ One Agentmaster per exe path: an unpackaged Agentmaster derives its single-instance identity from its image
path, so a second start of the same exe HANDS ITS COMMAND LINE to the running one and exits. start() refuses
while another instance of the same exe runs; the suite kills the previous test's app first.

⚠ Ending the app: kill() terminates the GUI process ONLY and lets the ConPTY hosts end their shells and
claudes themselves (CLAUDE.md: a host killed outright skips its CTRL_CLOSE and orphans its tab's processes).
Whatever is still alive after the drain is killed — this is a disposable test machine, and a leftover claude
would write into the next test's mock log — and reported, because on a user's PC it would be a bug.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import psutil

from . import uia, win32
from .claude import ClaudeConfig
from .profile import Profile
from .waits import Abort, WaitTimeout, wait_until

E2E_ROOT = Path(r"C:\e2e")
DEFAULT_EXE = E2E_ROOT / "am" / "Agentmaster.exe"
MAIN_CLASS = "CASCADIA_HOSTING_WINDOW_CLASS"
"""The window class of an Agentmaster window (Windows Terminal's)."""
POPUP_CLASS = "Xaml_WindowedPopupClass"
"""The window class of a XAML popup: context menus, submenus, flyouts, tooltips, the + launcher card."""
MOCK_TOKEN = "mock-token-not-a-secret"
"""The fake bearer token the mock accepts (Claude Code needs SOME credential to start without a login)."""

_LEAKY_PREFIXES = ("AGENTMASTER_", "CLAUDE_", "ANTHROPIC_", "CCMGR_", "CODEX_")
_LEAKY_NAMES = {"WT_SESSION", "WT_PROFILE_ID", "AM_SESSION", "AM_HARNESS"}


def find_agentmaster_pids(exe: Optional[Path] = None, image: str = "Agentmaster.exe") -> list[int]:
    """The running Agentmaster GUI processes, oldest first.

    Args:
        exe: Only processes of this exe path (None = any path).
        image: The process image name.
    Returns:
        Their pids.
    """
    found = []
    for p in psutil.process_iter(["name", "exe", "create_time"]):
        try:
            if (p.info["name"] or "").lower() != image.lower():
                continue
            if exe is not None and (p.info["exe"] or "").lower() != str(exe).lower():
                continue
            found.append((p.info["create_time"] or 0.0, p.pid))
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return [pid for _, pid in sorted(found)]


def _safe_name(name: str) -> str:
    """A folder-safe version of a test name.

    Args:
        name: Any text (a pytest node name).
    Returns:
        Letters, digits, '-', '_' and '.' only, at most 60 characters.
    """
    out = "".join(c if c.isalnum() or c in "-_." else "_" for c in name)
    return out[:60].strip("_") or "test"


@dataclass
class Isolation:
    """The per-test folders and the environment that points Agentmaster and Claude Code at them.

    Attributes:
        name: The test's folder name (sanitized).
        root: The suite's root folder in the guest.
        mock_url: The mock model's base URL (ANTHROPIC_BASE_URL).
        extra_env: More variables for the app (a flow's switch, e.g. CLAUDE_CODE_NO_FLICKER=1).
    """

    name: str
    root: Path = E2E_ROOT
    mock_url: str = "http://127.0.0.1:8787"
    extra_env: dict = field(default_factory=dict)

    def __post_init__(self) -> None:
        """Sanitize the name.

        Returns:
            None.
        """
        self.name = _safe_name(self.name)

    @property
    def profile_dir(self) -> Path:
        """The Agentmaster profile folder.

        Returns:
            root\\p\\<name>.
        """
        return self.root / "p" / self.name

    @property
    def claude_dir(self) -> Path:
        """Claude Code's config folder.

        Returns:
            root\\c\\<name>.
        """
        return self.root / "c" / self.name

    @property
    def work_dir(self) -> Path:
        """The parent of the test's working folders.

        Returns:
            root\\w\\<name>.
        """
        return self.root / "w" / self.name

    @property
    def out_dir(self) -> Path:
        """The test's evidence folder.

        Returns:
            root\\out\\tests\\<name> (empty unless the test failed).
        """
        return self.root / "out" / "tests" / self.name

    def prepare(self) -> "Isolation":
        """Empty and create the test's folders, and seed Claude Code's config.

        Returns:
            self.
        Raises:
            OSError: When a folder cannot be removed (a process from an earlier run still holds a file in it —
                the suite's kill should have ended it).
        """
        for d in (self.profile_dir, self.claude_dir, self.work_dir, self.out_dir):
            if d.exists():
                shutil.rmtree(d)
            d.mkdir(parents=True)
        ClaudeConfig(self.claude_dir).seed()
        return self

    def folder(self, name: str) -> Path:
        """A working folder for a session (created when missing).

        Args:
            name: The leaf ("t1"); it becomes the session's default title (the last word of the folder name).
        Returns:
            Its path.
        """
        path = self.work_dir / name
        path.mkdir(parents=True, exist_ok=True)
        return path

    def git_folder(self, name: str, branch: str = "main") -> Path:
        """A working folder that is a git repository on a given branch (no commits needed: the branch is read
        from .git\\HEAD).

        Args:
            name: The folder's name.
            branch: The branch `git init -b` creates.
        Returns:
            Its path.
        Raises:
            subprocess.CalledProcessError: When git fails.
        """
        path = self.folder(name)
        git = self.root / "git" / "cmd" / "git.exe"
        subprocess.run([str(git), "init", "-q", "-b", branch, str(path)], check=True, capture_output=True)
        return path

    def env(self) -> dict[str, str]:
        """The app's environment: the runner's own, minus anything that could point it elsewhere, plus the
        isolation and the mock.

        Returns:
            The variables.
        """
        env = {k: v for k, v in os.environ.items()
               if not k.upper().startswith(_LEAKY_PREFIXES) and k.upper() not in _LEAKY_NAMES}
        env.update({
            "AGENTMASTER_PROFILE": str(self.profile_dir),
            "CLAUDE_CONFIG_DIR": str(self.claude_dir),
            "ANTHROPIC_BASE_URL": self.mock_url,
            "ANTHROPIC_AUTH_TOKEN": MOCK_TOKEN,
            "DISABLE_AUTOUPDATER": "1",
            "CLAUDE_CODE_SKIP_AUTO_UPDATE": "1",
            "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
            "CLAUDE_CODE_GIT_BASH_PATH": str(self.root / "git" / "bin" / "bash.exe"),
            "CLAUDE_CODE_RESUME_INTERRUPTED_TURN": "1",
        })
        path_head = [str(self.root / "pwsh"), str(self.root / "git" / "cmd"),
                     str(Path(os.environ.get("USERPROFILE", r"C:\Users\Docker")) / ".local" / "bin")]
        env["PATH"] = ";".join(path_head + [env.get("PATH", "")])
        env.update({k: str(v) for k, v in self.extra_env.items()})
        return env


@dataclass
class KillReport:
    """What ending the app took.

    Attributes:
        drained: Every child process ended by itself after the GUI went.
        leftovers: The processes that had to be killed after the drain (image names), on a user's PC a bug.
        seconds: How long it took.
    """

    drained: bool
    leftovers: list
    seconds: float


class App:
    """One Agentmaster process started for a test.

    Attributes:
        iso: The test's isolation.
        exe: The Agentmaster.exe to run.
        proc: The Popen of the started process (None before start()).
        profile: Its profile (sessions.json, window records, logs).
        claude: Its Claude Code config folder (transcripts).
    """

    def __init__(self, iso: Isolation, exe: Path = DEFAULT_EXE, mock=None) -> None:
        """Bind the app to its isolation (nothing starts yet).

        Args:
            iso: The test's folders and environment.
            exe: The Agentmaster.exe.
            mock: The MockClient of the scripted model the app's sessions talk to (sessions confirm their
                prompts through it).
        Returns:
            None.
        """
        self.iso = iso
        self.exe = Path(exe)
        self.mock = mock
        self.proc: Optional[subprocess.Popen] = None
        self.profile = Profile(iso.profile_dir)
        self.claude = ClaudeConfig(iso.claude_dir)
        self.ended_by_test = False
        """Set when the test itself ended the app (close / kill), so the suite does not report a crash."""

    # ---- life cycle ------------------------------------------------------------------------------

    @property
    def pid(self) -> int:
        """The GUI process id.

        Returns:
            The pid.
        Raises:
            RuntimeError: Before start().
        """
        if self.proc is None:
            raise RuntimeError("the app was not started")
        return self.proc.pid

    def alive(self) -> bool:
        """Whether the GUI process still runs.

        Returns:
            True while it runs.
        """
        return self.proc is not None and self.proc.poll() is None

    def start(self, args: tuple = (), timeout_s: float = 60.0, maximize: bool = True,
              reopen_answer: Optional[str] = "Yes") -> "App":
        """Start Agentmaster and wait for its first window.

        Args:
            args: Command-line arguments (Windows Terminal's: e.g. ("-d", folder)).
            timeout_s: How long to wait for the window.
            maximize: Maximize the first window (every layout the page objects read assumes room).
            reopen_answer: The button pressed when "Reopen your N previous Agentmaster windows?" asks (it asks
                before any window exists, when more than one window was open at the last exit); None makes
                that prompt a failure.
        Returns:
            self.
        Raises:
            RuntimeError: When another instance of the same exe runs (it would swallow this start).
            WaitTimeout: When no window appeared (the evidence is the profile's hooks.log).
            Abort: When the process exited before showing a window, or an unexpected message box asked.
        """
        from .ui.dialogs import MessageBox
        others = find_agentmaster_pids(self.exe)
        if others:
            raise RuntimeError(f"another {self.exe.name} runs ({others}): a second start would hand its command "
                               "line to it and exit — kill it first")
        self.profile.dir.mkdir(parents=True, exist_ok=True)
        self.ended_by_test = False
        self.proc = subprocess.Popen([str(self.exe), *args], env=self.iso.env(), cwd=str(self.iso.work_dir),
                                     creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)

        def first_window():
            """One poll of the start: a window, a message box to answer, or the process gone.

            Returns:
                The window handles (empty while none).
            Raises:
                Abort: The process exited, or a message box this start does not expect.
            """
            if not self.alive():
                _raise_exited(self)
            box = MessageBox.find(self.pid)
            if box is not None:
                text = box.text()
                if reopen_answer is not None and "Reopen your" in text:
                    box.press(reopen_answer)
                    return []
                raise Abort(f"a message box asked during the start: {box.title!r}: {text!r}")
            return self.main_hwnds()

        wait_until(first_window, timeout_s, what="Agentmaster's first window")
        if maximize:
            hwnd = self.main_hwnds()[0]
            win32.maximize(hwnd)
            win32.set_foreground(hwnd)
        return self

    def main_hwnds(self) -> list[int]:
        """The visible Agentmaster windows of this process, topmost first.

        Returns:
            Their handles.
        """
        if not self.alive():
            return []
        return [w.hwnd for w in win32.top_windows(self.pid) if w.cls == MAIN_CLASS and w.visible]

    def windows(self) -> list:
        """The app's windows as page objects, topmost first.

        Returns:
            MainWindow objects.
        """
        from .ui.window import MainWindow  # the page objects import App's module; avoid the cycle at import time
        return [MainWindow(self, hwnd) for hwnd in self.main_hwnds()]

    @property
    def window(self):
        """The app's only (or topmost) window.

        Returns:
            The MainWindow.
        Raises:
            WaitTimeout: When the app has no window within 10 s.
        """
        from .ui.window import MainWindow
        hwnd = wait_until(lambda: self.main_hwnds() and self.main_hwnds()[0], 10.0, what="an Agentmaster window")
        return MainWindow(self, hwnd)

    def launch(self, name: str = "t1", prompt: Optional[str] = 'ready [[mock reply "ready"]]',
               timeout_s: float = 60.0):
        """Launch a managed Claude session in the test's folder `name` and (by default) run a first turn —
        fixture F-S of the flow catalog: a session Waiting for you, with a transcript.

        Args:
            name: The working folder's name under the test's work folder; it becomes the session's title.
            prompt: The first prompt (None = launch only: the session stays Idle with no transcript).
            timeout_s: How long the launch and the turn may take.
        Returns:
            The Session.
        Raises:
            WaitTimeout / MockError: When the launch or the first turn did not complete.
        """
        from .session import launch_session
        session = launch_session(self, self.iso.folder(name), self.mock, timeout_s=timeout_s)
        if prompt is not None:
            session.ask(prompt, timeout_s=timeout_s)
        return session

    def popups(self) -> list[uia.Element]:
        """The app's open popup windows (menus, flyouts, tooltips, the + card), topmost first.

        Returns:
            Their elements.
        """
        return uia.windows_of(self.pid, POPUP_CLASS) if self.alive() else []

    def children(self) -> list[psutil.Process]:
        """Every process the GUI started, recursively (ConPTY hosts, shells, claudes, their tools).

        Returns:
            The processes (empty when the GUI is gone).
        """
        try:
            return psutil.Process(self.pid).children(recursive=True)
        except (psutil.NoSuchProcess, RuntimeError):
            return []

    def claude_processes(self) -> list[psutil.Process]:
        """The claude.exe processes this app started.

        Returns:
            The processes.
        """
        return [p for p in self.children() if _name(p) == "claude.exe"]

    def kill(self, drain_s: float = 30.0) -> KillReport:
        """End the app the way the deploy recipe does: the GUI only, then let its children end themselves.

        Args:
            drain_s: How long the ConPTY hosts get to end their shells and claudes.
        Returns:
            A KillReport (leftovers = processes killed after the drain).
        """
        start = time.monotonic()
        if self.proc is None:
            return KillReport(True, [], 0.0)
        self.ended_by_test = True
        kids = self.children()
        if self.alive():
            try:
                psutil.Process(self.pid).kill()
            except psutil.NoSuchProcess:
                pass
            self.proc.wait(timeout=15)
        gone, alive = psutil.wait_procs(kids, timeout=drain_s)
        leftovers = []
        for p in alive:
            leftovers.append(_name(p))
            try:
                p.kill()
            except psutil.NoSuchProcess:
                pass
        psutil.wait_procs(alive, timeout=10)
        return KillReport(not alive, leftovers, time.monotonic() - start)

    def close(self, timeout_s: float = 60.0) -> None:
        """Quit the app through its windows' ✕, the way a user ends a session (the close flush runs: window
        records saved, sessions archived) — the path a restart test (F-RS) needs.

        Each window's ✕ asks "Close this window?" (Close All Windows / Close Window / Cancel); the last window
        is closed with "Close Window" (the app quits when no window is left).

        Args:
            timeout_s: How long the whole quit may take.
        Returns:
            None.
        Raises:
            WaitTimeout: When the process does not end.
        """
        from .ui.dialogs import Dialog
        self.ended_by_test = True
        end = time.monotonic() + timeout_s
        while self.alive() and time.monotonic() < end:
            hwnds = self.main_hwnds()
            if not hwnds:
                time.sleep(0.2)
                continue
            win32.set_foreground(hwnds[0])
            win32.post_close(hwnds[0])
            dialog = Dialog.wait(self, title="Close this window?", timeout_s=10)
            dialog.press("Close Window")
            wait_until(lambda: hwnds[0] not in self.main_hwnds(), 20, what="the window to close")
        if self.alive():
            self.proc.wait(timeout=max(1.0, end - time.monotonic()))
        if self.alive():
            raise WaitTimeout("Agentmaster did not quit after its last window closed")

    def restart(self, timeout_s: float = 60.0, graceful: bool = True, answer_reopen: Optional[str] = "Yes") -> "App":
        """End the app and start it again on the same profile (F-RS).

        Args:
            timeout_s: How long each half may take.
            graceful: Quit through the windows' ✕ (True) or kill the GUI (False = the crash variant).
            answer_reopen: The button to press when "Reopen your N previous Agentmaster windows?" asks ("Yes" /
                "No"; None makes the prompt a failure).
        Returns:
            self (a new process).
        Raises:
            WaitTimeout: When either half does not complete.
            Abort: When the new process exits or an unexpected message box asks.
        """
        if graceful:
            self.close(timeout_s)
        self.kill()
        self.start(timeout_s=timeout_s, maximize=True, reopen_answer=answer_reopen)
        return self


def _name(p: psutil.Process) -> str:
    """A process's image name in lower case.

    Args:
        p: The process.
    Returns:
        The name ("" when it is gone).
    """
    try:
        return (p.name() or "").lower()
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        return ""


def _raise_exited(app: App) -> bool:
    """Fail a start wait at once when the process ended (instead of waiting out the timeout).

    Args:
        app: The app.
    Returns:
        Never returns normally.
    Raises:
        Abort: Always, naming the exit code.
    """
    raise Abort(f"Agentmaster exited with code {app.proc.returncode} before showing a window "
                f"(see {app.profile.dir}\\hooks.log)")
