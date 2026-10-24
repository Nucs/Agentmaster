"""Installs side by side, their uninstall, and the old name: a user's own copy wins over the shared one, Apps &
Features' Uninstall, `WindowsTerminal.exe`.

flows/19 INS-05, INS-07 and INS-09 — the catalog's sections D, F and G. The installs are made with the installer script
(its own tests prove that path); what is tested here is how the APP behaves on top of them: the shared copy handing
every launch to the user's copy, the `--uninstall-portable` entry Windows' Settings runs, the old-name forwarder.

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import os
import time
from pathlib import Path

import psutil
import pytest

from amdriver import install, uia, win32
from amdriver.app import MAIN_CLASS
from amdriver.ui.taskdialog import TaskDialog
from amdriver.waits import hold_true, wait_until

pytestmark = [pytest.mark.install]

SHELLS = ("powershell.exe", "pwsh.exe", "cmd.exe")
"""The shells a new tab can run."""


def _norm(path) -> str:
    """A path as Windows compares it (case-insensitive, no trailing backslash).

    Args:
        path: A path or text.
    Returns:
        The normalized text.
    """
    return os.path.normcase(str(path).rstrip("\\"))


def _install(lane: install.Lane, kit: install.Kit, scope: str, version: str) -> None:
    """Install the build with the script (non-interactive, from a stamped zip).

    Args:
        lane: The lane.
        kit: The kit.
        scope: "User" or "Machine".
        version: The stamp.
    Returns:
        None.
    Raises:
        AssertionError: When the script failed (its run is in the message).
    """
    run = install.run_installer(lane, "-Scope", scope, "-ZipPath", str(kit.zip(version)))
    assert run.code == 0, str(run)


def _main_windows(pid: int) -> list[int]:
    """An Agentmaster's main windows.

    Args:
        pid: The process.
    Returns:
        Their handles.
    """
    return [w.hwnd for w in win32.top_windows(pid) if w.cls == MAIN_CLASS and w.visible]


def _tab_count(pid: int) -> int:
    """How many tabs an Agentmaster shows across its windows (UI Automation's tab items).

    Args:
        pid: The process.
    Returns:
        The count.
    """
    total = 0
    for hwnd in _main_windows(pid):
        try:
            total += len(uia.Element.from_hwnd(hwnd).find_all(30, role="tab item"))
        except Exception:  # noqa: BLE001 - a window closing under the read counts as no tabs
            continue
    return total


def _shell_cwds(app_pid: int) -> list[str]:
    """The working folders of the shells an Agentmaster's tabs run (each tab: an OpenConsole host and its shell).

    Args:
        app_pid: The Agentmaster process.
    Returns:
        The folders, as the processes report them.
    """
    out = []
    try:
        for child in psutil.Process(app_pid).children(recursive=True):
            try:
                if child.name().lower() in SHELLS:
                    out.append(child.cwd())
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
    except psutil.NoSuchProcess:
        pass
    return out


@pytest.mark.p1
@pytest.mark.flow("INS-05")
@pytest.mark.install("IS-71", "IS-72", "IS-73", "IS-75")
def test_a_users_own_copy_takes_over(lane, kit, machine):
    """The all-users copy and a per-user copy of the same user: every start of the shared copy runs the user's own.

    Promise (INS-05, PROFILES.md §2b "A user's own install wins"): the shared copy by path hands off (with its
    arguments), one instance stays the user's, `AGENTMASTER_NO_HANDOFF=1` opts out, and when the user's copy is
    uninstalled their icons open the shared copy again.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the process, log line or icon that is wrong.
    """
    _install(lane, kit, "Machine", "0.8.1.0")
    _install(lane, kit, "User", "0.8.2.0")
    shared_dir, own_dir = install.machine_install_dir(), install.user_install_dir()
    shared, own = shared_dir / "Agentmaster.exe", own_dir / "Agentmaster.exe"
    state = install.MachineState.read()
    for folder in ("desktop", "programs"):
        targets = [_norm(s.target) for s in state.shortcuts_in(folder)]
        assert _norm(own) in targets and _norm(shared) not in targets, \
            f"the user's {folder} icons do not open their own copy: {targets}"

    # IS-71: the shared copy started by path hands off, with its arguments.
    started = time.time()
    install.start_direct(shared, "-d", r"C:\Windows")
    app = install.wait_app(own_dir, timeout_s=90, newer_than=started)
    assert "-d" in app.cmdline and "c:\\windows" in app.cmdline.lower(), f"the arguments were not passed on: {app.cmdline}"
    wait_until(lambda: not install.app_processes(shared_dir), 30, what="the shared copy's process to end")
    lines = install.install_log_lines()
    assert any("handing off to this user's own install" in ln for ln in lines), f"no hand-off line: {lines[-5:]}"

    # IS-72: again while the user's copy runs: still one process, the user's, with a window more.
    windows_before = len(_main_windows(app.pid))
    install.start_direct(shared)
    wait_until(lambda: len(_main_windows(app.pid)) > windows_before, 45,
               what="the running copy to open a window for the handed-off launch")
    hold_true(lambda: [p.pid for p in install.app_processes() if p.image == "Agentmaster.exe"] == [app.pid], 3.0,
              what="one Agentmaster process, the user's own",
              describe=lambda: [(p.pid, p.exe) for p in install.app_processes()])
    install.end_agentmaster()

    # IS-73: the opt-out makes the shared copy run itself.
    started = time.time()
    install.start_direct(shared, env=install.clean_env({"AGENTMASTER_NO_HANDOFF": "1"}))
    install.wait_app(shared_dir, timeout_s=90, newer_than=started)
    assert not install.app_processes(own_dir), "the opt-out still handed off"
    install.end_agentmaster()

    # IS-75: the user's copy uninstalled: their icons open the shared copy again.
    run = install.run_installer(lane, "-Uninstall", "-Scope", "User")
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    assert state.user is None and state.user_entry is None, "the user's copy is still installed"
    for folder in ("desktop", "programs"):
        targets = [_norm(s.target) for s in state.shortcuts_in(folder)]
        assert _norm(shared) in targets, f"the user's {folder} icons were not handed back to the shared copy: {targets}"


@pytest.mark.p0
@pytest.mark.flow("INS-07")
@pytest.mark.install("IS-91", "IS-92", "IS-93")
def test_uninstall_from_apps_and_features(lane, kit, machine):
    """The command Windows' Settings runs for Uninstall (`Agentmaster.exe --uninstall-portable`): its confirmation,
    Cancel, Uninstall; and the same flag on a copy that is not an install.

    Promise (INS-07): the confirmation says what goes and what stays; Cancel changes nothing; Uninstall removes only
    what a release shipped and keeps the user's data; a plain copy is never "uninstalled".

    Args:
        lane: The install lane's facts.
        kit: Stamped zips and copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the dialog text or leftover that is wrong.
    """
    _install(lane, kit, "User", "0.8.1.0")
    dest = install.user_install_dir()
    exe = dest / "Agentmaster.exe"
    (dest / "mine.txt").write_text("the user's own file\n", encoding="utf-8")
    entry = install.MachineState.read().user_entry
    assert entry is not None and _norm(entry.get("UninstallString", "")) == _norm(f'"{exe}" --uninstall-portable'), entry

    # IS-92: Cancel changes nothing.
    proc = install.start_direct(exe, "--uninstall-portable")
    confirm = TaskDialog.wait(install.UNINSTALL_CONFIRM, exe_dirs=[dest], timeout_s=60)
    text = confirm.text()
    assert f"This removes Agentmaster from" in text and str(dest) in text, text
    assert str(install.default_profile()) in text, f"the confirmation does not say the data stays in the profile\n{text}"
    confirm.press(r"^Cancel$")
    proc.wait(timeout=30)
    state = install.MachineState.read()
    assert state.user is not None and state.user_entry is not None, "Cancel removed the install"

    # IS-91: Uninstall removes what shipped, keeps the rest.
    proc = install.start_direct(exe, "--uninstall-portable")
    TaskDialog.wait(install.UNINSTALL_CONFIRM, exe_dirs=[dest], timeout_s=60).press(r"^Uninstall$")
    # The uninstaller runs detached (am-update.ps1 -UninstallPortable, after this process exits).
    wait_until(lambda: not (dest / "Agentmaster.exe").exists() and install.MachineState.read().user_entry is None, 180,
               interval_s=1.0, what="the uninstaller to remove the install")
    state = install.MachineState.read()
    assert state.user_verbs == {} and state.user_path == [], f"verbs / PATH left: {state.user_verbs} {state.user_path}"
    assert [s for s in state.shortcuts if _norm(dest) in _norm(s.target)] == [], "shortcuts into the install left"
    assert (dest / "mine.txt").exists() and (dest / "profile.path").exists(), "the uninstall removed what is the user's"
    assert install.default_profile().is_dir(), "the uninstall removed the profile"

    # IS-93: a copy that is not an install is never "uninstalled".
    plain = kit.copy("0.8.1.0", "plain")
    names = sorted(p.name for p in plain.parent.iterdir())
    proc = install.start_direct(plain, "--uninstall-portable")
    TaskDialog.wait(install.NOTHING_TO_UNINSTALL, exe_dirs=[plain.parent], timeout_s=60).press(r"^OK$")
    proc.wait(timeout=30)
    assert sorted(p.name for p in plain.parent.iterdir()) == names, "the flag changed a copy that is no install"


@pytest.mark.p1
@pytest.mark.flow("INS-09")
@pytest.mark.install("IS-97", "IS-98")
def test_the_old_name_forwards(lane, kit, machine):
    """`WindowsTerminal.exe` in an install folder starts Agentmaster.exe with the same arguments, and a running one
    takes the launch: a new tab in the folder asked for.

    Promise (INS-09): old shortcuts and scripts keep working, and never make a second instance.

    Args:
        lane: The install lane's facts.
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the process, tab or folder that is wrong.
    """
    _install(lane, kit, "User", "0.8.1.0")
    dest = install.user_install_dir()
    forwarder = dest / "WindowsTerminal.exe"
    folder = Path(r"C:\e2e\w")
    folder.mkdir(parents=True, exist_ok=True)

    # IS-98: no Agentmaster running: the forwarder starts one with its arguments and exits.
    started = time.time()
    proc = install.start_direct(forwarder, "-d", str(folder))
    app = install.wait_app(dest, timeout_s=90, newer_than=started)
    assert "-d" in app.cmdline and _norm(folder) in _norm(app.cmdline), f"arguments not passed on: {app.cmdline}"
    proc.wait(timeout=30)
    wait_until(lambda: any(_norm(c) == _norm(folder) for c in _shell_cwds(app.pid)), 60,
               what=f"a tab's shell in {folder}", describe=lambda: _shell_cwds(app.pid))

    # IS-97: while it runs: the running Agentmaster takes the launch; one process stays.
    tabs_before = _tab_count(app.pid)
    proc = install.start_direct(forwarder, "-d", r"C:\Windows")
    proc.wait(timeout=30)
    wait_until(lambda: _tab_count(app.pid) > tabs_before, 45, what="a new tab for the forwarded launch",
               describe=lambda: _tab_count(app.pid))
    wait_until(lambda: any(_norm(c) == _norm(r"C:\Windows") for c in _shell_cwds(app.pid)), 60,
               what="the new tab's shell in C:\\Windows", describe=lambda: _shell_cwds(app.pid))
    hold_true(lambda: [p.pid for p in install.app_processes() if p.image == "Agentmaster.exe"] == [app.pid], 3.0,
              what="one Agentmaster process", describe=lambda: [(p.pid, p.cmdline) for p in install.app_processes()])
