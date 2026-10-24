"""Another account at the keyboard: a standard user beside the signed-in administrator.

flows/19 INS-01 (the script's account rules), INS-02 (the question as another account), INS-03 (the all-users copy and
its other users) — the catalog's scenarios with the Machine tag `std`. The lane makes the standard account `qastd`
(`provision.ps1 -Lane Install -Accounts`) and the tests start programs as it in the signed-in user's session, the way
"Run as different user" does (amdriver.install.run_as: Start-Process -Credential, all three standard streams
redirected). What the other account sees — its own HKCU entries and icons — is read AS that account
(amdriver.install.account_state).

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import os
import re
import time

import pytest

from amdriver import install
from amdriver.ui.taskdialog import TaskDialog
from amdriver.waits import wait_until

pytestmark = [pytest.mark.install]


def _norm(path) -> str:
    """A path as Windows compares it (case-insensitive, no trailing backslash).

    Args:
        path: A path or text.
    Returns:
        The normalized text.
    """
    return os.path.normcase(str(path).rstrip("\\"))


def _targets(state: dict) -> list[str]:
    """The normalized targets of an account's Agentmaster icons.

    Args:
        state: account_state()'s answer.
    Returns:
        The targets.
    """
    return [_norm(s.get("target", "")) for s in state.get("shortcuts") or []]


@pytest.mark.p1
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-26", "IS-27")
def test_another_account_runs_the_installer(lane, kit, machine):
    """The installer run by a standard account while another user is signed in: refused for the signed-in user, and
    with -ForCurrentAccount installed for itself without rights, its own copy taking over beside the all-users copy.

    Promise (PROFILES.md §2b "Who runs it", §2c): another account at the keyboard never installs for the wrong person,
    and a standard user can always install just for themselves.

    Args:
        lane: The install lane's facts (the staged installer, the accounts).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the message, install or registration that is wrong.
    """
    account = lane.account("std")
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    zip2 = kit.zip("0.8.2.0")

    # IS-27: -Scope User, run by a standard account that is not the signed-in user: refused, nothing installed.
    run = install.run_installer_as(account, lane, "-Scope", "User", "-ZipPath", str(zip2))
    assert run.code != 0, f"the other account's run was not refused\n{run}"
    assert "This PowerShell runs as" in run.output and "is signed in" in run.output and "-ForCurrentAccount" in run.output, \
        str(run)
    assert not (account.install_dir / "Agentmaster.exe").exists(), "the refused run installed for the other account"
    assert install.MachineState.read().user is None, "the refused run installed for the signed-in user"

    # IS-26: -ForCurrentAccount, no -Scope, non-interactive: installed just for that account, no rights needed.
    run = install.run_installer_as(account, lane, "-ForCurrentAccount", "-ZipPath", str(zip2))
    assert run.code == 0, str(run)
    assert "non-interactive: installing for scope User" in run.output, str(run)
    assert "your own copy takes over" in run.output, f"no note that its own copy takes over\n{run}"
    exe = account.install_dir / "Agentmaster.exe"
    assert exe.exists(), f"no install in {account.install_dir}"
    state = install.account_state(account)
    assert state["user_entry"] and _norm(state["user_entry"]["InstallLocation"]) == _norm(account.install_dir), \
        f"{account.name}'s Apps & Features entry: {state['user_entry']}"
    assert _norm(exe) in _targets(state), f"{account.name}'s icons do not open its own copy: {state['shortcuts']}"
    assert install.MachineState.read().user is None, "the signed-in user got a per-user install"


@pytest.mark.p1
@pytest.mark.flow("INS-03")
@pytest.mark.install("IS-63", "IS-64", "IS-65")
def test_a_standard_user_and_the_shared_copy(lane, kit, machine):
    """The all-users copy and a standard user: `--register-user` gives them icons of their own, their start asks nothing
    and runs on their own profile, the uninstall sweeps their icons, and a new install gives them icons again.

    Promise (INS-03 phase 2, PROFILES.md §2b "No shared icons"): one install for every account, each with their own
    icons and data; the shared folder never changes under a user.

    Args:
        lane: The install lane's facts (the staged installer, the accounts).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the icon, profile or process that is wrong.
    """
    account = lane.account("std")
    shared_dir = install.machine_install_dir()
    shared = shared_dir / "Agentmaster.exe"
    zip1 = kit.zip("0.8.1.0")
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(zip1))
    assert run.code == 0, str(run)

    # IS-64: --register-user as the other account (what Active Setup runs at its sign-in): its icons, no profile.
    run = install.run_as(account, shared, "--register-user", timeout_s=120)
    assert run.code == 0, str(run)
    state = install.account_state(account)
    assert _targets(state).count(_norm(shared)) >= 2, f"{account.name} got no desktop + Start icons: {state['shortcuts']}"
    assert not state["profile"], f"--register-user made {account.name} an Agentmaster profile"

    # IS-63: the other account starts the shared copy: no question, its own profile, the shared folder unchanged.
    names = sorted(p.name for p in shared_dir.iterdir())
    started = time.time()
    install.start_as(account, shared)
    app = install.wait_app(shared_dir, timeout_s=120, newer_than=started)
    assert app.user.lower().endswith("\\" + account.name.lower()), f"the shared copy runs as {app.user!r}"
    assert TaskDialog.all(pids=[app.pid]) == [], f"{account.name}'s start of the shared copy asked something"
    wait_until(lambda: (account.profile / "hooks.log").exists(), 60, what=f"{account.name}'s own profile in use")
    assert sorted(p.name for p in shared_dir.iterdir()) == names, "the other account's start changed the shared folder"
    install.end_agentmaster()

    # IS-65: the all-users uninstall sweeps the other account's icons; a new install gives them again.
    run = install.run_installer(lane, "-Uninstall", "-Scope", "Machine")
    assert run.code == 0, str(run)
    state = install.account_state(account)
    assert _norm(shared) not in _targets(state), f"{account.name}'s icons of the removed copy are left: {state['shortcuts']}"
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(zip1))
    assert run.code == 0, str(run)
    run = install.run_as(account, shared, "--register-user", timeout_s=120)
    assert run.code == 0, str(run)
    state = install.account_state(account)
    assert _targets(state).count(_norm(shared)) >= 2, f"{account.name} got no icons after the new install: {state['shortcuts']}"


@pytest.mark.p1
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-53")
def test_another_account_sees_the_question(lane, kit, machine):
    """A copy started by a standard account in the signed-in user's session: the question says whose session it is,
    offers the install for the account that runs it by name, and writes nothing until asked.

    Promise (PROFILES.md §2b "Who runs it", OtherAccountExplain): an install never lands in the wrong account without
    the user seeing for whom it is.

    Args:
        lane: The install lane's facts (the accounts).
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the text or option that is wrong.
    """
    account = lane.account("std")
    exe = kit.copy("0.8.1.0", "otheraccount")
    started = time.time()
    install.start_as(account, exe)
    question = install.wait_question(exe.parent, timeout_s=120)
    text = question.text()
    signed_in = os.environ.get("USERNAME", "Docker")
    # The accounts are named DOMAIN\user in the text (DetectInstallIdentity): match the user part.
    assert "Agentmaster is running as" in text and account.name in text, text
    assert re.search(rf"(\S+\\)?{re.escape(signed_in)} is signed in to Windows", text), text
    assert re.search(rf"start Agentmaster\.exe from (\S+\\)?{re.escape(signed_in)}'s own account", text), \
        f"no word on how to set it up for {signed_in}\n{text}"
    first = question.labels()[0]
    assert first.startswith("Install for ") and account.name.lower() in first.lower(), \
        f"the per-user option does not name the account it installs for: {first!r}; all: {question.labels()}"
    assert not any(label.startswith("Set it up for") for label in question.labels()), \
        "a standard account was offered the hand-over (only an elevated one or SYSTEM can start it as the user)"
    question.press(r"^Ask me later")
    app = install.wait_app(exe.parent, timeout_s=120, newer_than=started)
    assert app.user.lower().endswith("\\" + account.name.lower()), f"the copy runs as {app.user!r}"
    assert not install.InstallFacts.read(exe.parent).install_path, "'Ask me later' wrote a decision"
    assert not (account.install_dir / "Agentmaster.exe").exists(), "something was installed for the other account"
    assert install.MachineState.read().user is None, "something was installed for the signed-in user"


def _question_of(copy_dir, account_name: str):
    """The copy's install question shown by a process of one account.

    Args:
        copy_dir: The copy's folder.
        account_name: The account (its user part).
    Returns:
        The TaskDialog, or None while that account's question is not up.
    """
    for d in TaskDialog.all(exe_dirs=[copy_dir]):
        if d.owner().lower().endswith("\\" + account_name.lower()) and install.QUESTION in d.texts():
            return d
    return None


@pytest.mark.p1
@pytest.mark.flow("INS-04")
@pytest.mark.install("IS-52")
def test_another_administrator_sets_it_up_for_the_signed_in_user(lane, kit, machine):
    """A copy started by ANOTHER administrator (elevated: UAC is off) in the signed-in user's session offers to set it
    up for the signed-in user; that starts the copy again as them, and they get their own question.

    Promise (INS-04 phase 1, PROFILES.md §2b OtherAccountRelaunch): an administrator helping at the keyboard never
    installs for themselves by accident; nothing is installed for the administrator.

    Args:
        lane: The install lane's facts (the accounts).
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the option, owner or install that is wrong.
    """
    admin = lane.account("adm2")
    signed_in = os.environ.get("USERNAME", "Docker")
    exe = kit.copy("0.8.1.0", "foruser")
    install.start_as(admin, exe)
    first = wait_until(lambda: _question_of(exe.parent, admin.name), 120, what=f"{admin.name}'s question")
    labels = first.labels()
    assert re.fullmatch(rf"Set it up for (\S+\\)?{re.escape(signed_in)} \(recommended\)", labels[0]), \
        f"the first option is {labels[0]!r}; all: {labels}"
    assert any(label.startswith("Install for ") and admin.name.lower() in label.lower() for label in labels), \
        f"the administrator's own install is not offered by name: {labels}"
    restarted = time.time()
    first.press(r"^Set it up for")
    # The copy starts again as the signed-in user (a one-shot task, ~13 s measured), who sees the question in their
    # own account: the per-user install for them, recommended.
    second = wait_until(lambda: _question_of(exe.parent, signed_in), 180, what=f"{signed_in}'s own question")
    assert second.labels()[0].startswith("Install just for you"), f"{signed_in}'s question offers {second.labels()}"
    second.press(r"^Ask me later")
    app = install.wait_app(exe.parent, timeout_s=120, newer_than=restarted)
    assert app.user.lower().endswith("\\" + signed_in.lower()), f"the copy runs as {app.user!r}"
    assert not (admin.install_dir / "Agentmaster.exe").exists(), "something was installed for the administrator"
    assert install.MachineState.read().user is None, "something was installed for the signed-in user"


@pytest.mark.p1
@pytest.mark.flow("INS-04")
@pytest.mark.install("IS-29")
def test_another_administrator_runs_the_installer(lane, kit, machine):
    """The installer run by ANOTHER administrator, `-Scope User -Force -Launch`: installed for the user signed in to
    Windows (its per-user part runs as them through a one-shot task) and started as them; nothing for the administrator.

    Promise (INS-04 phase 3's twin, PROFILES.md §2b "The scripts"): remote support or a second administrator at the
    keyboard installs for the person who uses the PC.

    Args:
        lane: The install lane's facts (the staged installer, the accounts).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the install or process that is wrong.
    """
    admin = lane.account("adm2")
    signed_in = os.environ.get("USERNAME", "Docker")
    started = time.time()
    run = install.run_installer_as(admin, lane, "-Scope", "User", "-Force", "-Launch", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    assert re.search(rf"is installed for (\S+\\)?{re.escape(signed_in)}", run.output), f"not installed for {signed_in}\n{run}"
    state = install.MachineState.read()
    assert state.user is not None and state.user.version == "0.8.1.0", f"no per-user install for {signed_in}: {state.user}"
    assert state.user_entry is not None, f"{signed_in} got no Apps & Features entry"
    app = install.wait_app(install.user_install_dir(), timeout_s=120, newer_than=started)
    assert app.user.lower().endswith("\\" + signed_in.lower()), f"the app runs as {app.user!r}, not {signed_in}"
    assert not (admin.install_dir / "Agentmaster.exe").exists(), "the administrator got an install of its own"
