"""SYSTEM at the keyboard's place: the installer run by a management tool, and a copy started in session 0.

flows/19 INS-04 (an account that is not the signed-in user sets Agentmaster up FOR them) and INS-02 (the question and
who may see it) — the catalog's scenarios with the Machine tag `sys`. The lane needs nothing prepared for them:
`amdriver.install.run_as_system` starts the program through a one-shot scheduled task as SYSTEM, in session 0, the way
a deployment tool, an installer service or Intune runs one.

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import os
import re
import time

import pytest

from amdriver import install
from amdriver.ui.taskdialog import TaskDialog

pytestmark = [pytest.mark.install]


@pytest.mark.p1
@pytest.mark.flow("INS-04")
@pytest.mark.install("IS-28")
def test_system_installs_for_the_signed_in_user(lane, kit, machine):
    """The installer run as SYSTEM, `-Scope User -Launch`: installed for the user signed in to Windows (its per-user
    part runs as them, in their session, through a one-shot task) and started as them.

    Promise (PROFILES.md §2b "The scripts", the 2026-10-21 probe matrix): a script SYSTEM runs installs for the person who
    uses the PC, never into SYSTEM's own profile.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the message, install or process that is wrong.
    """
    signed_in = os.environ.get("USERNAME", "Docker")
    started = time.time()
    run = install.run_as_system(install.WINDOWS_POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                "-File", str(lane.installer), "-Scope", "User", "-Launch", "-ZipPath",
                                str(kit.zip("0.8.1.0")), timeout_s=900)
    assert run.code == 0, str(run)
    # The running account is WindowsIdentity's name, "NT AUTHORITY\SYSTEM" (a space in it).
    assert re.search(rf"Running as [^,]*SYSTEM, but (\S+\\)?{re.escape(signed_in)} is signed in to Windows: installing for "
                     rf"(\S+\\)?{re.escape(signed_in)}", run.output), f"no note that SYSTEM installs for {signed_in}\n{run}"
    assert re.search(rf"is installed for (\S+\\)?{re.escape(signed_in)}", run.output), f"not installed for {signed_in}\n{run}"
    state = install.MachineState.read()
    assert state.user is not None and state.user.version == "0.8.1.0", f"no per-user install for {signed_in}: {state.user}"
    assert state.user_entry is not None, f"{signed_in} got no Apps & Features entry"
    assert state.machine is None and state.machine_entry is None, "SYSTEM's -Scope User run made an all-users install"
    app = install.wait_app(install.user_install_dir(), timeout_s=180, newer_than=started)
    assert app.user.lower().endswith("\\" + signed_in.lower()), f"the app runs as {app.user!r}, not {signed_in}"
    system_profile = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "config", "systemprofile",
                                  ".agentmaster")
    assert not os.path.isdir(os.path.join(system_profile, "bin")), f"SYSTEM got an install of its own in {system_profile}"


@pytest.mark.p2
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-54")
def test_a_copy_started_in_session_0(lane, kit, machine):
    """A portable copy started as SYSTEM in session 0 (a service, no desktop): no question anywhere, and nothing is
    persisted — the copy keeps no decision, so the user's own first start still asks.

    Promise (PortableInstall.h DecideAccountPlan's NoSessionUser, the 2026-10-21 probe matrix): a launch without a
    desktop never decides an install, because nobody could have answered.

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the dialog, decision file or registration that appeared.
    """
    exe = kit.copy("0.8.1.0", "session0")
    copy = exe.parent
    run = install.run_as_system(exe, wait=False)
    assert run.code == 0 and run.output.startswith("PID "), str(run)
    pid = int(run.output.split()[1])
    try:
        # The launch decides at once (before any window); give it the time a first start takes on a loaded desktop.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            assert not (copy / "install.path").exists(), "the session-0 start wrote an install decision"
            time.sleep(1.0)
        session = install.process_session(pid)
        if session is not None:
            assert session == 0, f"the copy runs in session {session}, not session 0 (a task as SYSTEM)"
        assert not (copy / "install.path").exists() and not (copy / "profile.path").exists(), \
            "the session-0 start persisted a decision (install.path / profile.path)"
        assert TaskDialog.all(exe_dirs=[copy]) == [], "a dialog of the copy reached the signed-in user's desktop"
        state = install.MachineState.read()
        assert state.user_entry is None and state.machine_entry is None and state.user_verbs == {} and state.shortcuts == [], \
            f"the session-0 start registered something: entry={state.user_entry} verbs={state.user_verbs} " \
            f"icons={[s.path for s in state.shortcuts]}"
    finally:
        install.stop_as_system(pid)
