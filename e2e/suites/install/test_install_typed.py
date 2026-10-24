"""The installer TYPED, the way its README tells a user to run it: in a PowerShell window, and in a tab of Agentmaster.

flows/19 INS-01 (the installer script) and INS-03 — the catalog's rows that a user answers at the keyboard, and the
Ctrl+C a user presses half way through an upgrade (IS-13). The suite types like a user, focus-free:
`amdriver.install.console_type` attaches a helper to the program's console and writes key presses (Read-Host and
PSReadLine read them like the keyboard), `console_text` reads its screen, and `console_ctrl_c_when` sends a Ctrl+C at
a chosen point. A PowerShell window is a new console (`start_in_console`; the test desktops' default terminal makes it
a Windows Terminal tab); a tab of Agentmaster is its shell's console (a ConPTY of the app's own).

What only a run inside the app can show: the installer notices that it runs in the Agentmaster its step closes, says
"the step goes on in a new window", and goes on in a copy of itself in a window of its own (Start-InstallerInNewWindow),
which asks "Go on? [Y/n]", closes the app, does the step, starts the app again and waits for Enter.

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import re
import time
from pathlib import Path
from typing import Optional

import psutil
import pytest

from amdriver import install
from amdriver.waits import wait_until

pytestmark = [pytest.mark.install]

SHELLS = ("powershell.exe", "pwsh.exe")
"""The shells a tab of the installed copy runs (Windows PowerShell on the test desktops: no PowerShell 7 on PATH)."""


PADDING = "zz-e2e-padding.bin"
"""IS-13's large last file. The installer lays the new layout down entry by entry in name order (Get-ChildItem), and
this name sorts after every name the build ships (its last is wtd.exe), so it is copied last: when it appears in the
install folder, every other new file is laid down and the old ones wait in the set-aside folder."""
PADDING_BYTES = 512 * 1024 * 1024
"""Its size (zeros: ~0.5 MB in the zip): copying it takes seconds, so a Ctrl+C sent when it appears lands mid-copy."""


def _powershell_file(lane: install.Lane, *args: str, no_exit: bool = False) -> list[str]:
    """The installer as a user's PowerShell window runs it: a file, interactive (no -NonInteractive).

    Args:
        lane: The lane (the staged installer).
        *args: The script's parameters.
        no_exit: Keep the window at PowerShell's prompt after the script (-NoExit): its screen stays readable after a
            run that a Ctrl+C stopped.
    Returns:
        The command line's parts.
    """
    head = [str(install.WINDOWS_POWERSHELL), "-NoProfile", "-ExecutionPolicy", "Bypass"] + (["-NoExit"] if no_exit else [])
    return head + ["-File", str(lane.installer), *args]


def _typed_file_run(lane: install.Lane, *args: str) -> str:
    """The command a user types into a tab to run the downloaded installer file: a nested Windows PowerShell with
    `-ExecutionPolicy Bypass`, as a user must — the tab's own PowerShell runs no script file, because Windows 11's
    default policy (LocalMachine Undefined = Restricted) refuses `& '…\\Install-Agentmaster.ps1'` there ("running
    scripts is disabled on this system"; seen in the lane 2026-10-24). The README's script-block form needs no policy.

    Args:
        lane: The lane (the staged installer).
        *args: The installer's parameters (a value with spaces must come quoted).
    Returns:
        The command line (without Enter).
    """
    return " ".join(["powershell -NoProfile -ExecutionPolicy Bypass -File", f"'{lane.installer}'", *args])


def _tab_shell(app: install.AppProcess, timeout_s: float = 120.0) -> int:
    """The shell of the app's first tab, at its prompt.

    Args:
        app: The running Agentmaster.
        timeout_s: How long.
    Returns:
        The shell's process id.
    Raises:
        WaitTimeout: When no shell under the app reached its prompt.
    """
    def shell() -> Optional[int]:
        """The first shell process under the app whose console shows a PowerShell prompt.

        Returns:
            Its id, or None.
        """
        try:
            children = psutil.Process(app.pid).children(recursive=True)
        except psutil.NoSuchProcess:
            return None
        for child in children:
            try:
                if child.name().lower() in SHELLS and re.search(r"PS [A-Za-z]:\\[^\n]*>\s*$", install.console_text(child.pid)):
                    return child.pid
            except psutil.NoSuchProcess:
                continue
        return None

    return wait_until(shell, timeout_s, interval_s=1.0, what=f"a tab's PowerShell prompt in Agentmaster pid {app.pid}")


def _record_text(path: Path) -> str:
    """A continued run's record (a PowerShell transcript) as text, in whichever encoding the PowerShell wrote it.

    Args:
        path: The record.
    Returns:
        Its text (UTF-16 by its byte-order mark, else UTF-8).
    Raises:
        OSError: When it cannot be read.
    """
    data = path.read_bytes()
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8-sig", errors="replace")


def _continued_run(after: float, timeout_s: float = 60.0) -> int:
    """The installer's continued run: the PowerShell its run in a tab started in a window of its own.

    Args:
        after: Epoch seconds the run was started after.
        timeout_s: How long.
    Returns:
        Its process id.
    Raises:
        WaitTimeout: When none started.
    """
    def found() -> Optional[int]:
        """The newest powershell.exe running a copy of the installer (Install-Agentmaster-<hex>.ps1).

        Returns:
            Its id, or None.
        """
        for p in psutil.process_iter(["name", "cmdline", "create_time"]):
            try:
                line = " ".join(p.info["cmdline"] or [])
                if (p.info["name"] or "").lower() in SHELLS and re.search(r"Install-Agentmaster-[0-9a-f]{8}\.ps1", line) \
                        and (p.info["create_time"] or 0) >= after - 1:
                    return p.pid
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        return None

    return wait_until(found, timeout_s, interval_s=0.5, what="the installer's continued run in a window of its own")


def _through_a_tab(app: install.AppProcess, command: str, closes: str) -> tuple[float, str, Path]:
    """Type an installer command into the app's tab and answer its continued run like a user.

    Args:
        app: The Agentmaster whose tab the command is typed into (the one its step closes).
        command: The PowerShell command line (without Enter).
        closes: The text the continued run says before "Go on? [Y/n]" ("It closes for the upgrade …" / "The
            uninstall closes it").
    Returns:
        (the epoch time Enter was pressed at "Go on?", the continued run's last screen, its record's path).
    Raises:
        AssertionError / WaitTimeout: Naming the message or window that did not come.
    """
    shell = _tab_shell(app)
    typed = time.time()
    install.console_type(shell, command + "\r")
    tab = install.wait_console_text(shell, r"its record: \S+\.log", 180)
    assert "the step goes on in a new window" in tab, f"the tab does not say the step goes on in a new window:\n{tab[-1500:]}"
    record = Path(re.search(r"its record: (\S+\.log)", tab).group(1))
    run = _continued_run(typed)
    asked = install.wait_console_text(run, r"Go on\? \[Y/n\]", 300)
    assert closes in asked, f"the continued run does not say {closes!r} before it asks:\n{asked[-1500:]}"
    answered = time.time()
    install.console_type(run, "\r")
    done = install.wait_console_text(run, r"Press Enter to close this window", 600)
    install.console_type(run, "\r")
    wait_until(lambda: not psutil.pid_exists(run), 60, what="the continued run's window to close after Enter")
    return answered, done, record


@pytest.mark.p1
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-05", "IS-06")
def test_the_installer_answered_in_console_windows(lane, kit, machine):
    """The installer in a plain PowerShell window, interactive: Enter at its question installs just for you (IS-05);
    then, with the installed copy running, an upgrade in a separate window says it closes the copy and starts it again,
    asks "Go on?", and does so (IS-06).

    Promise (INS-01, the 2026-10-21 probe matrix and the 2026-10-22 evening edge cases): the README's command asks one
    question whose Enter is the safe choice, and an upgrade never pulls a running app's files away without saying so.
    The 2026-10-22 check also saw the restarted copy's parent be explorer.exe: on the lane's desktops (UAC off) the
    installer runs elevated and starts the app through a one-shot task instead (Start-InstalledApp), so that part
    needs the `uac` machine.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the question, process or install that is wrong.
    """
    dest = install.user_install_dir()

    # IS-05: the question in the window; Enter takes [1] Just for you.
    window = install.start_in_console(*_powershell_file(lane, "-ZipPath", str(kit.zip("0.8.1.0"))))
    try:
        asked = install.wait_console_text(window.pid, r"Choice \[1/2/C\] \(Enter = 1\)", 180)
        assert "Where should Agentmaster be installed?" in asked and "[1] Just for you (recommended" in asked, asked[-1500:]
        install.console_type(window.pid, "\r")
        window.wait(timeout=600)
    finally:
        if window.poll() is None:
            window.kill()
    assert window.returncode == 0, f"the interactive install ended with {window.returncode}"
    state = install.MachineState.read()
    assert state.user is not None and state.user.version == "0.8.1.0", f"Enter did not install just for you: {state.user}"
    assert state.machine is None and state.machine_entry is None, "Enter installed for all users"

    # IS-06: the copy running, the upgrade typed into a separate window.
    started = time.time()
    install.start_like_explorer(dest / "Agentmaster.exe")
    app = install.wait_app(dest, timeout_s=120, newer_than=started)
    window = install.start_in_console(*_powershell_file(lane, "-Scope", "User", "-ZipPath", str(kit.zip("0.8.2.0"))))
    try:
        asked = install.wait_console_text(window.pid, r"Go on\? \[Y/n\]", 300)
        assert "It closes for the upgrade and starts again when the upgrade is done" in asked, asked[-1500:]
        answered = time.time()
        install.console_type(window.pid, "\r")
        window.wait(timeout=600)
    finally:
        if window.poll() is None:
            window.kill()
    assert window.returncode == 0, f"the upgrade ended with {window.returncode}"
    wait_until(lambda: not psutil.pid_exists(app.pid), 60, what="the running copy to be closed for the upgrade")
    assert install.InstallFacts.read(dest).version == "0.8.2.0", "not upgraded"
    install.wait_app(dest, timeout_s=120, newer_than=answered)


@pytest.mark.p1
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-07", "IS-08", "IS-09")
def test_the_installer_typed_into_a_tab_of_the_copy_it_changes(lane, kit, machine):
    """The installer typed into a TAB of the very Agentmaster it upgrades (as a file, IS-07; as a script block, the
    console-host path, IS-08) and uninstalls (IS-09): each time the tab says the step goes on in a new window, which asks,
    closes the app, does the step — upgraded and started again, or removed with the data kept — and keeps its record.

    Promise (INS-01, the 2026-10-22 evening edge cases): the installer never cuts the branch it sits on — a step that
    closes the app it runs in moves out of it first, and the user sees why.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the message, window, version or file that is wrong.
    """
    dest = install.user_install_dir()
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    started = time.time()
    install.start_like_explorer(dest / "Agentmaster.exe")
    app = install.wait_app(dest, timeout_s=120, newer_than=started)

    # IS-07: the downloaded file, run by a nested PowerShell (the 2026-10-22 pass: the run is not the tab's process).
    zip2 = kit.zip("0.8.2.0")
    answered, done, record = _through_a_tab(app, _typed_file_run(lane, "-Scope User", f"-ZipPath '{zip2}'"),
                                            "It closes for the upgrade and starts again when the upgrade is done")
    assert install.InstallFacts.read(dest).version == "0.8.2.0", f"not upgraded:\n{done[-1500:]}"
    assert record.exists() and "0.8.2.0" in _record_text(record), \
        f"the continued run's record {record} is missing or says nothing of the upgrade"
    app = install.wait_app(dest, timeout_s=120, newer_than=answered)

    # IS-08: as a script block (no file behind it: the continued run writes its copy from the block's own text).
    zip3 = kit.zip("0.8.3.0")
    answered, done, _ = _through_a_tab(
        app, f"& ([scriptblock]::Create([IO.File]::ReadAllText('{lane.installer}'))) -Scope User -ZipPath '{zip3}'",
        "It closes for the upgrade and starts again when the upgrade is done")
    assert install.InstallFacts.read(dest).version == "0.8.3.0", f"not upgraded by the script-block form:\n{done[-1500:]}"
    app = install.wait_app(dest, timeout_s=120, newer_than=answered)

    # IS-09: the uninstall, typed into the tab.
    profile = install.default_profile()
    _, done, _ = _through_a_tab(app, _typed_file_run(lane, "-Uninstall -Scope User"), "The uninstall closes it")
    state = install.MachineState.read()
    assert state.user is None and state.user_entry is None, f"not uninstalled:\n{done[-1500:]}"
    assert (profile / "hooks.log").exists(), f"the uninstall removed the data in {profile}"
    assert not install.app_processes(dest), "the app runs again after its uninstall"


@pytest.mark.p2
@pytest.mark.flow("INS-03")
@pytest.mark.install("IS-25")
def test_the_all_users_upgrade_typed_into_a_tab(lane, kit, machine):
    """The all-users upgrade typed into a tab of the all-users copy: the step goes on in a new window, which asks,
    closes the copy, upgrades Program Files and starts the copy again.

    Promise (INS-03, the 2026-10-22 evening edge cases): the shared copy is upgraded from inside it like a user's own.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the message, window or version that is wrong.
    """
    shared_dir = install.machine_install_dir()
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    started = time.time()
    install.start_direct(shared_dir / "Agentmaster.exe")
    app = install.wait_app(shared_dir, timeout_s=120, newer_than=started)
    zip2 = kit.zip("0.8.2.0")
    answered, done, _ = _through_a_tab(app, _typed_file_run(lane, "-Scope Machine", f"-ZipPath '{zip2}'"),
                                       "It closes for the upgrade and starts again when the upgrade is done")
    assert install.InstallFacts.read(shared_dir).version == "0.8.2.0", f"the all-users copy was not upgraded:\n{done[-1500:]}"
    install.wait_app(shared_dir, timeout_s=120, newer_than=answered)


@pytest.mark.p2
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-13")
def test_ctrl_c_half_way_through_an_upgrade(lane, kit, machine):
    """Ctrl+C in the installer's window half way through an upgrade's copy: the run says it was stopped and puts the
    previous version back — its stamp and every one of its files, nothing of the new version, no set-aside folder,
    the Apps & Features entry unchanged — and that copy starts.

    Promise (INS-01, Install-AmLayout): an upgrade stopped half way never leaves a folder with neither version. A run
    STOPPED by Ctrl+C never reaches a catch — PowerShell runs only the finally then — so the finally puts the previous
    version back itself (before 2026-10-22 a Ctrl+C during the copy left a folder with neither).

    The Ctrl+C lands at a chosen point, the way a user's would mid-copy: the new layout carries a large last file
    (PADDING), and the console helper sends the signal the moment that file appears in the install folder — every
    other new file laid down, the old ones in the set-aside folder.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the message, file, folder or entry that is wrong.
    """
    dest = install.user_install_dir()
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    before = install.InstallFacts.read(dest)
    assert before is not None and before.version == "0.8.1.0", f"the first install: {before}"
    zip2 = kit.zip("0.8.2.0", extra={PADDING: PADDING_BYTES})

    # -NoExit keeps the window, and its screen, at PowerShell's prompt after the stop.
    window = install.start_in_console_handling_ctrl_c(
        *_powershell_file(lane, "-Scope", "User", "-ZipPath", str(zip2), no_exit=True))
    try:
        # The console is handed to the default terminal a moment after the start: the Ctrl+C is armed once the
        # installer's first step shows (long before its copy: the zip's extraction alone takes seconds).
        install.wait_console_text(window.pid, r"==> ", 120)
        ctrl = install.console_ctrl_c_when(window.pid, dest, PADDING, timeout_s=600)
        _, err = ctrl.communicate(timeout=660)
        assert ctrl.returncode == 0, (f"no Ctrl+C was sent (helper exit {ctrl.returncode}; 5 = {PADDING} never appeared "
                                      f"in {dest}): {err.decode('utf-8', 'replace')[-400:]}")
        screen = install.wait_console_text(window.pid, r"the install was stopped half way; putting the previous version back.*"
                                          r"PS [A-Za-z]:\\\S*>", 300)
        assert "the previous version is back in place" in screen, f"the run does not say the version is back:\n{screen[-1500:]}"
    finally:
        try:
            install.console_type(window.pid, "exit\r")
            window.wait(timeout=30)
        except (RuntimeError, psutil.Error):
            if window.is_running():
                window.kill()

    after = install.InstallFacts.read(dest)
    assert after is not None and after.version == "0.8.1.0", f"the previous version is not back: {after}"
    assert not (dest / PADDING).exists(), f"the new version's last file was left in {dest}"
    aside = [p.name for p in dest.iterdir() if p.name.lower().startswith(".am-previous-")]
    assert not aside, f"set-aside folders left in {dest}: {aside}"
    missing = [n for n in before.names if not (dest / n).exists()]
    assert not missing, f"files of the previous version missing from {dest}: {missing}"
    entry = install.MachineState.read().user_entry
    assert entry is not None and str(entry.get("DisplayVersion", "")).startswith("0.8.1"), f"the entry changed: {entry}"
    started = time.time()
    install.start_like_explorer(dest / "Agentmaster.exe")
    install.wait_app(dest, timeout_s=120, newer_than=started)
