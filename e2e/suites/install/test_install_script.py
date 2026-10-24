"""The one-command installer, tools/Install-Agentmaster.ps1, run for real on a clean test desktop.

flows/19 INS-01, INS-03, INS-07 — the catalog's section A ("The one-command installer"). Every test starts on a machine
with no Agentmaster (the `machine` fixture checks) and installs the build under test from a zip stamped with a version
of its own (`-ZipPath`: nothing is downloaded; the lane blocks api.github.com anyway). The oracle is the machine itself
— the files, the registry, the shortcuts, the PATH (amdriver.install.MachineState) — and the script's own words, never
the app's view of itself.

Runs only in the install lane (E2E_TESTING.md §8.5): `python e2e/host/install_lane.py …`.
"""
from __future__ import annotations

import json
import os
import re
import shutil
from pathlib import Path

import pytest

from amdriver import install
from amdriver.app import E2E_ROOT

pytestmark = [pytest.mark.install]


def _norm(path) -> str:
    """A path as Windows compares it (case-insensitive, no trailing backslash).

    Args:
        path: A path or text.
    Returns:
        The normalized text.
    """
    return os.path.normcase(str(path).rstrip("\\"))


def _version_shown(version: str) -> str:
    """A regex for how Apps & Features shows a version (`major.minor.patch[.build]`: a zero build may be left out).

    Args:
        version: "0.8.1.0".
    Returns:
        The anchored regex.
    """
    parts = version.split(".")
    head = re.escape(".".join(parts[:3]))
    return rf"^{head}(\.{re.escape(parts[3])})?$" if len(parts) > 3 and parts[3] == "0" else rf"^{re.escape(version)}$"


def _assert_user_install(state: install.MachineState, version: str, dest: Path) -> None:
    """Check every registration a per-user install makes (IS-01's list).

    Args:
        state: The machine.
        version: The version it must hold.
        dest: Its folder.
    Returns:
        None.
    Raises:
        AssertionError: Naming the first registration that is wrong.
    """
    assert state.user is not None and _norm(state.user.dir) == _norm(dest), f"no per-user install in {dest}: {state.user}"
    for name in ("Agentmaster.exe", "WindowsTerminal.exe", "agentmaster-cli.exe", ".am-manifest", ".portable"):
        assert name in state.user.names, f"{name} missing from {dest}: {state.user.names}"
    assert state.user.version == version, f".am-version is {state.user.version!r}, not {version}"
    assert state.user.install_path == "installed", f"install.path is {state.user.install_path!r}"
    assert _norm(state.user.profile_path) == _norm(install.default_profile()), \
        f"profile.path is {state.user.profile_path!r}, not the Default profile {install.default_profile()}"
    assert state.user.marked == [], f"installed files keep a download mark: {state.user.marked}"
    e = state.user_entry
    assert e is not None, "no HKCU Apps & Features entry 'Agentmaster'"
    exe = dest / "Agentmaster.exe"
    assert e.get("DisplayName") == "Agentmaster", e
    assert re.match(_version_shown(version), str(e.get("DisplayVersion"))), f"DisplayVersion {e.get('DisplayVersion')!r} for {version}"
    assert _norm(e.get("InstallLocation", "")) == _norm(dest), e
    assert _norm(e.get("UninstallString", "")) == _norm(f'"{exe}" --uninstall-portable'), e
    assert set(state.user_verbs) == set(install.VERB_KEYS), f"verbs: {state.user_verbs}"
    for key, command in state.user_verbs.items():
        assert _norm(str(exe)) in _norm(command) and "-d" in command, f"the {key} verb runs {command!r}"
    assert any(_norm(p) == _norm(dest) for p in state.user_path), f"{dest} is not on the user PATH: {state.user_path}"
    for folder in ("desktop", "programs"):
        links = state.shortcuts_in(folder)
        assert any(_norm(s.target) == _norm(exe) for s in links), f"no {folder} shortcut to {exe}: {links}"


def _assert_no_user_install(state: install.MachineState, dest: Path) -> None:
    """Check that no per-user install is registered (what an uninstall leaves, or an all-users-only machine has).

    Args:
        state: The machine.
        dest: The per-user folder.
    Returns:
        None.
    Raises:
        AssertionError: Naming what is left.
    """
    assert state.user is None, f"Agentmaster.exe still in {dest}: {state.user}"
    assert state.user_entry is None, f"the HKCU Apps & Features entry is still there: {state.user_entry}"
    assert state.user_verbs == {}, f"HKCU verbs left: {state.user_verbs}"
    assert not any(_norm(p) == _norm(dest) for p in state.user_path), f"{dest} still on the user PATH"
    left = [s for s in state.shortcuts if _norm(dest) in _norm(s.target)]
    assert left == [], f"shortcuts into {dest} left: {left}"


@pytest.mark.p0
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-01", "IS-02", "IS-03", "IS-04")
def test_just_for_you_life_cycle(lane, kit, machine):
    """A per-user install from a zip, again (up to date), upgraded over a user's own file, then uninstalled.

    Promise (INS-01, INS-07): just for you needs no rights and registers everything a user expects; a run of the same
    version changes nothing; an upgrade replaces only what a release ships; the uninstall removes only that and keeps
    the user's data.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from (checked; evidence and clean-up after a failure).
    Returns:
        None.
    Raises:
        AssertionError: Naming the first registration, file or message that is wrong.
    """
    dest = install.user_install_dir()
    zip1 = kit.zip("0.8.1.0")
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(zip1))
    assert run.code == 0, str(run)
    assert "installed just for you" in run.output, str(run)
    state = install.MachineState.read()
    _assert_user_install(state, "0.8.1.0", dest)
    assert state.machine is None and state.machine_entry is None and state.active_setup is None, \
        "a per-user install wrote all-users registrations"

    # IS-02: the same version again changes nothing (Test-AmUpToDate is asked before anything is replaced).
    exe = dest / "Agentmaster.exe"
    stamp = exe.stat().st_mtime_ns
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(zip1))
    assert run.code == 0, str(run)
    assert "Already up to date for this user" in run.output, str(run)
    assert exe.stat().st_mtime_ns == stamp, "the up-to-date run rewrote Agentmaster.exe"

    # IS-03: a newer zip over it, with a file of the user's own in the folder.
    (dest / "mine.txt").write_text("the user's own file\n", encoding="utf-8")
    zip2 = kit.zip("0.8.2.0")
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(zip2))
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    _assert_user_install(state, "0.8.2.0", dest)
    assert (dest / "mine.txt").read_text(encoding="utf-8") == "the user's own file\n", "the upgrade touched the user's file"
    assert not [n for n in state.user.names if n.startswith(".am-previous")], \
        f"the upgrade left its set-aside folder: {state.user.names}"

    # IS-04: the uninstall removes what a release shipped and keeps the rest.
    run = install.run_installer(lane, "-Uninstall", "-Scope", "User")
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    _assert_no_user_install(state, dest)
    assert (dest / "mine.txt").exists(), "the uninstall removed the user's own file"
    assert (dest / "profile.path").exists(), "the uninstall removed profile.path (it is kept: the user's choice)"
    assert not (dest / "install.path").exists(), "install.path is left (it goes with the install)"
    assert install.default_profile().is_dir(), "the uninstall removed the profile folder"


@pytest.mark.p1
@pytest.mark.flow("INS-03")
@pytest.mark.install("IS-18", "IS-19", "IS-20")
def test_for_all_users_life_cycle(lane, kit, machine):
    """An all-users install from a zip (UAC off: no prompt), again (up to date), then uninstalled.

    Promise (INS-03, INS-07): for all users lands in Program Files with HKLM registrations under its own name, gives
    the signed-in user icons of their own through Active Setup instead of shared ones, and its uninstall sweeps them.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the first registration that is wrong or left behind.
    """
    dest = install.machine_install_dir()
    exe = dest / "Agentmaster.exe"
    zip1 = kit.zip("0.8.1.0")
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(zip1))
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    assert state.machine is not None and state.machine.version == "0.8.1.0", f"no all-users install: {state.machine}"
    assert state.machine.install_path == "machine", f"install.path is {state.machine.install_path!r}"
    assert state.machine.profile_path == "", "an all-users install has no profile.path (each user resolves their own)"
    e = state.machine_entry
    assert e is not None and e.get("DisplayName") == "Agentmaster (all users)", f"HKLM entry: {e}"
    assert re.match(_version_shown("0.8.1.0"), str(e.get("DisplayVersion"))), e
    assert _norm(e.get("InstallLocation", "")) == _norm(dest), e
    assert set(state.machine_verbs) == set(install.VERB_KEYS), f"HKLM verbs: {state.machine_verbs}"
    assert any(_norm(p) == _norm(dest) for p in state.machine_path), f"{dest} not on the machine PATH: {state.machine_path}"
    a = state.active_setup
    assert a is not None, "no Active Setup component"
    assert _norm(a.get("StubPath", "")) == _norm(f'"{exe}" --register-user'), f"Active Setup StubPath: {a}"
    assert str(a.get("IsInstalled")) == "1", a
    for folder in ("desktop", "programs"):
        links = state.shortcuts_in(folder)
        assert any(_norm(s.target) == _norm(exe) for s in links), f"the user has no {folder} icon of the shared copy: {links}"
    shared = state.shortcuts_in("public_desktop") + state.shortcuts_in("common_programs")
    assert shared == [], f"shared icons (a current release gives each user their own): {shared}"
    _assert_no_user_install(state, install.user_install_dir())

    # IS-19: the same version again.
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(zip1))
    assert run.code == 0, str(run)
    assert "Already up to date for all users" in run.output, str(run)

    # IS-20: the uninstall.
    run = install.run_installer(lane, "-Uninstall", "-Scope", "Machine")
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    assert state.machine is None, f"Agentmaster.exe still in {dest}"
    assert state.machine_entry is None and state.machine_verbs == {} and state.machine_path == [], \
        f"HKLM leftovers: entry={state.machine_entry} verbs={state.machine_verbs} path={state.machine_path}"
    assert state.active_setup is None, f"the Active Setup component is left: {state.active_setup}"
    left = [s for s in state.shortcuts if _norm(dest) in _norm(s.target)]
    assert left == [], f"icons of the removed copy left: {left}"


_LEAK_PROBE = r"""
param([string]$Installer, [string]$Zip)
$ErrorActionPreference = 'Continue'
$t = [IO.File]::ReadAllText($Installer)
function Get-Leaks([string]$Form) {
    $names = @('AmCoreFunctionNames', 'AmPreservedNames', 'Scope', 'ZipPath', 'Repo', 'GitHubToken', 'amSelf', 'AmInstallerIsolated')
    [ordered]@{
        form      = $Form
        eap       = "$ErrorActionPreference"
        variables = @($names | Where-Object { Get-Variable -Name $_ -ErrorAction SilentlyContinue })
        functions = @(@('Write-Step', 'Install-AmLayout', 'Invoke-AmUserInstall') | Where-Object { Get-Command $_ -CommandType Function -ErrorAction SilentlyContinue })
        module    = [bool](Get-Module Agentmaster-installer)
    }
}
$results = @()
& ([scriptblock]::Create($t)) -Scope User -ZipPath $Zip
$results += Get-Leaks 'scriptblock'
# iex binds no parameter: the run asks (no console input: the default, User), wants the newest release - and the lane
# blocks api.github.com, so it FAILS. The caller must go on, and keep nothing, either way.
$t | Invoke-Expression
$results += Get-Leaks 'iex'
'RESULT ' + (ConvertTo-Json -InputObject $results -Compress -Depth 4)
"""


@pytest.mark.p1
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-10")
def test_the_callers_powershell_keeps_nothing(lane, kit, machine):
    """The script-block form and `… | iex` in a Windows PowerShell 5.1 and a PowerShell 7 (E4, 9245a890f).

    Promise: a run that is not a file run leaves the caller's session as it was — its $ErrorActionPreference, no
    function, no `$script:` list, no parameter variable, no module — whether the run succeeds (the script block
    installs from the zip) or fails (iex cannot pass -ZipPath, and the newest release is out of reach here).

    Args:
        lane: The install lane's facts (the staged installer, PowerShell 7 when provisioned).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the edition, the form and what it left in the caller's session.
    """
    zip1 = kit.zip("0.8.1.0")
    probe = E2E_ROOT / "w" / "leak-probe.ps1"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text(_LEAK_PROBE, encoding="ascii")
    editions = ["5.1"] + (["7"] if lane.pwsh else [])
    for edition in editions:
        run = install.run_powershell(["-File", str(probe), "-Installer", str(lane.installer), "-Zip", str(zip1)],
                                     edition, lane)
        line = next((ln for ln in run.output.splitlines() if ln.startswith("RESULT ")), None)
        assert line is not None, f"PowerShell {edition}: the probe printed no RESULT (the caller did not go on)\n{run}"
        results = json.loads(line[len("RESULT "):])
        for r in results:
            assert r["eap"] == "Continue", f"PowerShell {edition}, {r['form']}: $ErrorActionPreference is {r['eap']!r}\n{run}"
            assert not r["variables"], f"PowerShell {edition}, {r['form']}: variables left: {r['variables']}"
            assert not r["functions"], f"PowerShell {edition}, {r['form']}: functions left: {r['functions']}"
            assert not r["module"], f"PowerShell {edition}, {r['form']}: the throwaway module is left"
    state = install.MachineState.read()
    assert state.user is not None and state.user.version == "0.8.1.0", \
        "the script-block form's parameters did not reach the run (no install from the zip)"


@pytest.mark.p1
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-14", "IS-15", "IS-16")
def test_install_dir_rules(lane, kit, machine):
    """-InstallDir: a relative folder is made absolute; a folder of other files gets an Agentmaster folder inside it; a
    second per-user copy elsewhere is refused with how to move the first (9245a890f).

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the rule that did not hold.
    """
    base = E2E_ROOT / "w" / "installdir"
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    zip1 = kit.zip("0.8.1.0")
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(zip1), "-InstallDir", r".\amrel", cwd=base)
    assert run.code == 0, str(run)
    dest = base / "amrel"
    assert (dest / "Agentmaster.exe").exists(), f"the relative -InstallDir did not land in {dest}\n{run}"
    state = install.MachineState.read()
    assert state.user_entry is not None and _norm(state.user_entry.get("InstallLocation", "")) == _norm(dest), \
        f"the registration does not name the absolute folder: {state.user_entry}"

    # A folder that holds other files, while the per-user install lives elsewhere.
    foreign = base / "foreign"
    foreign.mkdir()
    (foreign / "notes.txt").write_text("not Agentmaster's\n", encoding="utf-8")
    zip2 = kit.zip("0.8.2.0")
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(zip2), "-InstallDir", str(foreign))
    assert run.code != 0, f"a second per-user copy was not refused\n{run}"
    assert f"{foreign} holds other files: Agentmaster goes into {foreign / 'Agentmaster'}" in run.output, str(run)
    assert "Agentmaster is already installed just for you at" in run.output and "uninstall it first" in run.output, str(run)
    assert not (foreign / "Agentmaster").exists(), "the refused run wrote into the folder"
    assert (foreign / "notes.txt").exists()
    state = install.MachineState.read()
    assert (dest / ".am-version").read_text(encoding="ascii").strip() == "0.8.1.0", "the refused run changed the install"
    assert _norm(state.user_entry.get("InstallLocation", "")) == _norm(dest), "the refused run moved the registration"
