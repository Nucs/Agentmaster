"""Older releases under the current installer: one without `--register-user`, one without a release manifest.

flows/19 INS-03 and INS-07 — the catalog's scenarios with the Machine tag `old`. The lane extracts an older release as
published (`provision.ps1 -Lane Install -Old`: v0.7.10, from before the manifest and `--register-user`), and the tests
make zips of it with and without a manifest (amdriver.install.Kit.old_zip), the way the 2026-10-22 evening QA gave the
released v0.7.10 zip a manifest to stand for "an older app without --register-user".

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import os
import re

import pytest

from amdriver import install

pytestmark = [pytest.mark.install]


def _norm(path) -> str:
    """A path as Windows compares it (case-insensitive, no trailing backslash).

    Args:
        path: A path or text.
    Returns:
        The normalized text.
    """
    return os.path.normcase(str(path).rstrip("\\"))


def _shared_links(state: install.MachineState) -> list[install.Shortcut]:
    """The icons on the Public desktop and in the common Start menu (shared by every user).

    Args:
        state: The machine.
    Returns:
        Those shortcuts.
    """
    return state.shortcuts_in("public_desktop") + state.shortcuts_in("common_programs")


@pytest.mark.p2
@pytest.mark.flow("INS-03")
@pytest.mark.install("IS-21", "IS-22", "IS-23")
def test_an_older_release_for_all_users(lane, kit, machine):
    """An all-users install of a release that knows no `--register-user`: shared shortcuts, no Active Setup; upgraded to
    a current build: each user's own icons instead; the older one again: up to date, and with -Force a downgrade that
    puts the shared shortcuts back.

    Promise (PROFILES.md §2b "The installer's edge cases"): the installer gives every release the icons IT can run —
    a release that cannot make a user's own icons gets the shared ones, and an upgrade or a downgrade swaps them.

    Args:
        lane: The install lane's facts (the staged installer, the older release).
        kit: Stamped zips of the build under test and of the older release.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the icon, component or message that is wrong.
    """
    dest = install.machine_install_dir()
    exe = dest / "Agentmaster.exe"
    old_zip = kit.old_zip(manifest=True)

    # IS-21: the older release for all users.
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(old_zip))
    assert run.code == 0, str(run)
    assert "an older release: its Start-menu and desktop shortcuts" in run.output, str(run)
    state = install.MachineState.read()
    shared = _shared_links(state)
    assert [s for s in shared if _norm(s.target) == _norm(exe)], f"no shared shortcuts of the older release: {shared}"
    assert all(os.path.basename(s.path) == "Agentmaster (all users).lnk" for s in shared), \
        f"the shared shortcuts' name: {[s.path for s in shared]}"
    assert state.active_setup is None, f"an older release got the Active Setup step: {state.active_setup}"
    own = state.shortcuts_in("desktop") + state.shortcuts_in("programs")
    assert own == [], f"the older release made the user icons of their own: {own}"

    # IS-22: upgraded to the build under test: the shared shortcuts go, Active Setup comes, the user's own icons.
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    assert _shared_links(state) == [], f"the shared shortcuts are left after the upgrade: {_shared_links(state)}"
    assert state.active_setup is not None and _norm(state.active_setup.get("StubPath", "")) == _norm(f'"{exe}" --register-user'), \
        f"Active Setup after the upgrade: {state.active_setup}"
    for folder in ("desktop", "programs"):
        assert [s for s in state.shortcuts_in(folder) if _norm(s.target) == _norm(exe)], \
            f"the user got no {folder} icon of their own after the upgrade: {state.shortcuts_in(folder)}"

    # IS-23: the older release again: without -Force up to date; with -Force downgraded, the shared shortcuts back.
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(old_zip))
    assert run.code == 0 and "Already up to date for all users" in run.output, str(run)
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(old_zip), "-Force")
    assert run.code == 0, str(run)
    state = install.MachineState.read()
    assert state.machine is not None and re.match(r"^0\.7\.", state.machine.version), f"not downgraded: {state.machine}"
    assert state.active_setup is None, f"the downgrade left the Active Setup step: {state.active_setup}"
    assert [s for s in _shared_links(state) if _norm(s.target) == _norm(exe)], "the downgrade did not put the shared shortcuts back"
    own = [s for s in state.shortcuts_in("desktop") + state.shortcuts_in("programs") if _norm(s.target) == _norm(exe)]
    assert own == [], f"the downgrade left the icons the current build gave the user: {own}"


@pytest.mark.p2
@pytest.mark.flow("INS-07")
@pytest.mark.install("IS-24", "IS-95")
def test_a_release_without_a_manifest(lane, kit, machine):
    """A release as published before the manifest: refused for all users; installed just for you, then uninstalled
    through the legacy list of names, the user's own file kept.

    Promise (PROFILES.md §2b "The manifest"): an all-users install never runs an app that cannot know what
    `install.path = machine` means, and an uninstall removes only names a release shipped — also for a release that
    listed none.

    Args:
        lane: The install lane's facts (the staged installer, the older release).
        kit: Stamped zips of the older release.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the message, file or registration that is wrong.
    """
    bare = kit.old_zip(manifest=False)

    # IS-24: refused for all users, nothing written.
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(bare))
    assert run.code != 0, f"a release without a manifest was installed for all users\n{run}"
    assert "This release predates installs for all users (its zip has no .am-manifest)" in run.output, str(run)
    state = install.MachineState.read()
    assert state.machine is None and state.machine_entry is None, "the refused run installed something for all users"

    # IS-95: just for you is allowed; its uninstall goes by the legacy list (no manifest to read).
    dest = install.user_install_dir()
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(bare))
    assert run.code == 0, str(run)
    facts = install.InstallFacts.read(dest)
    assert facts is not None and ".am-manifest" not in facts.names, f"the pre-manifest install: {facts}"
    (dest / "mine.txt").write_text("the user's own file\n", encoding="utf-8")
    shipped = [n for n in facts.names if n not in ("install.path", "profile.path", "profile", "settings")]
    run = install.run_installer(lane, "-Uninstall", "-Scope", "User")
    assert run.code == 0, str(run)
    left = sorted(p.name for p in dest.iterdir()) if dest.is_dir() else []
    assert "mine.txt" in left, f"the uninstall removed the user's own file: {left}"
    still = [n for n in shipped if n in left]
    assert still == [], f"shipped names the legacy list left behind: {still}"
    assert install.MachineState.read().user_entry is None, "the Apps & Features entry is left"
