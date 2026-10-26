"""A portable copy's first launch: "Where should Agentmaster live?" answered like a user answers it.

flows/19 INS-02 and INS-03 — the catalog's section B. Each test unpacks a FRESH copy of the build under test (the kit,
stamped with a version of its own), starts it the way a double-click does (Explorer starts it), reads the question
through UI Automation, presses an option, and checks the outcome on the machine: the files and decision files, the
registry, the shortcuts, which Agentmaster runs from where. The test desktops run with UAC off, so every start is an
elevated one: the question carries its "running with administrator rights" note, and the installed copy is started
through the one-shot task that drops rights (which, with UAC off, has none to drop — IS-42's measured `elevated=0`
needs the `uac` machine).

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import os
import re
import shutil
import time
from pathlib import Path

import pytest

from amdriver import install
from amdriver.ui.taskdialog import TaskDialog
from amdriver.waits import hold_true, wait_until

pytestmark = [pytest.mark.install]

ARROW = "→"
"""The arrow of "Update the installed Agentmaster (vX → vY)"."""


def _norm(path) -> str:
    """A path as Windows compares it (case-insensitive, no trailing backslash).

    Args:
        path: A path or text.
    Returns:
        The normalized text.
    """
    return os.path.normcase(str(path).rstrip("\\"))


def _v(version: str) -> str:
    """A regex for a version as the question shows it (FormatVersion may leave a zero build out).

    Args:
        version: "0.8.1.0".
    Returns:
        The regex (unanchored).
    """
    parts = version.split(".")
    if len(parts) == 4 and parts[3] == "0":
        return re.escape(".".join(parts[:3])) + r"(\.0)?"
    return re.escape(version)


def _start_copy(exe: Path) -> tuple[TaskDialog, float]:
    """Start a copy through Explorer and wait for its question.

    Args:
        exe: The copy's Agentmaster.exe.
    Returns:
        (the question, the epoch time just before the start).
    Raises:
        WaitTimeout: When the question did not come.
    """
    started = time.time()
    install.start_like_explorer(exe)
    return install.wait_question(exe.parent), started


def _gone(folder: Path, timeout_s: float = 60.0) -> None:
    """Wait until no Agentmaster runs from a folder.

    Args:
        folder: The folder.
        timeout_s: How long.
    Returns:
        None.
    Raises:
        WaitTimeout: When one still runs.
    """
    wait_until(lambda: not install.app_processes(folder), timeout_s, interval_s=0.5,
               what=f"no Agentmaster running from {folder}",
               describe=lambda: [(p.pid, p.cmdline) for p in install.app_processes(folder)])


def _assert_registered_for_user(state: install.MachineState, version: str) -> None:
    """Check the per-user install the question's "Install just for you" makes (IS-41: IS-01's registrations, plus the
    app's own AppUserModelID on its icons, which only the app can compute).

    Args:
        state: The machine.
        version: The version the install must hold.
    Returns:
        None.
    Raises:
        AssertionError: Naming the first registration that is wrong.
    """
    dest = install.user_install_dir()
    exe = dest / "Agentmaster.exe"
    assert state.user is not None, f"no per-user install in {dest}"
    assert state.user.version == version, f".am-version {state.user.version!r}, expected {version}"
    assert state.user.install_path == "installed", f"install.path {state.user.install_path!r}"
    assert _norm(state.user.profile_path) == _norm(install.default_profile()), f"profile.path {state.user.profile_path!r}"
    assert state.user.marked == [], f"installed files keep a download mark: {state.user.marked}"
    e = state.user_entry
    assert e is not None and e.get("DisplayName") == "Agentmaster", f"HKCU Apps & Features: {e}"
    assert _norm(e.get("UninstallString", "")) == _norm(f'"{exe}" --uninstall-portable'), e
    assert set(state.user_verbs) == set(install.VERB_KEYS), f"verbs: {state.user_verbs}"
    assert any(_norm(p) == _norm(dest) for p in state.user_path), f"not on the user PATH: {state.user_path}"
    for folder in ("desktop", "programs"):
        links = [s for s in state.shortcuts_in(folder) if _norm(s.target) == _norm(exe)]
        assert links, f"no {folder} icon of {exe}: {state.shortcuts_in(folder)}"
        assert all(s.aumid for s in links), f"the app's own {folder} icon carries no AppUserModelID: {links}"


@pytest.mark.p0
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-40", "IS-41", "IS-42", "IS-99", "IS-88")
def test_install_just_for_you(lane, kit, machine):
    """The question as a user first sees it, then its recommended answer: installed just for this user.

    Promise (INS-02 phases 1 and 4, INS-10 phase 1): a fresh copy asks where Agentmaster should live, with the per-user
    install recommended; that install registers everything, the installed copy starts on the Default profile without a
    profile question, and the copy's own process goes away. Offline (the lane blocks GitHub's API), the installed
    copy's startup update check fails quietly within its bound (IS-88).

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the option, registration or process that is wrong.
    """
    exe = kit.copy("0.8.1.0", "fresh")
    question, started = _start_copy(exe)
    labels = question.labels()
    expected = [r"^Install just for you \(recommended\)$", r"^Install for all users of this PC$",
                r"^Install to a folder you pick", r"^Install here$", r"^Keep it portable, run from here$", r"^Ask me later$"]
    assert len(labels) >= len(expected), f"the question offers {labels}"
    for pattern, label in zip(expected, labels):
        assert re.search(pattern, label), f"option {label!r} where {pattern!r} was expected; all: {labels}"
    text = question.text()
    assert re.search(rf"This is a portable copy of Agentmaster v{_v('0.8.1.0')}", text), text
    assert "Agentmaster is running with administrator rights" in text, \
        f"the elevated start's note is missing (UAC is off on the test desktops: every start is elevated)\n{text}"

    question.press(r"^Install just for you")
    # IS-104: installed elsewhere, the copy asks whether its folder may go (Keep here: the stub is checked below).
    leftover = install.answer_leftover_question(exe.parent, delete=False)
    assert leftover.labels[:2] and re.match(r"^Delete the downloaded copy \(recommended\)$", leftover.labels[0]) and \
        re.match(r"^Keep it$", leftover.labels[1]), f"the leftover question offers {leftover.labels}"
    flat = " ".join(leftover.text.split())
    assert _norm(install.user_install_dir()) in _norm(flat) and _norm(exe.parent) in _norm(flat) and "Recycle Bin" in flat, \
        f"the leftover question does not name the install, the copy and the Recycle Bin:\n{leftover.text}"
    app = install.wait_app(install.user_install_dir(), timeout_s=180, newer_than=started)
    _gone(exe.parent)
    state = install.MachineState.read()
    _assert_registered_for_user(state, "0.8.1.0")
    assert state.machine is None and state.machine_entry is None, "a per-user install wrote all-users registrations"
    # The unzip becomes a launcher stub of the install (PerformInstall, "the unzip becomes a launcher stub"): a later
    # double-click of it starts the installed copy instead of asking again (seen 2026-10-24: install.path names the
    # install; the first version of this test expected the folder untouched, as flows/19 said then).
    stub = install.InstallFacts.read(exe.parent)
    assert stub.portable and _norm(stub.install_path) == _norm(install.user_install_dir()), \
        f"the copy is no launcher stub of the install: install.path {stub.install_path!r}, .portable {stub.portable}"
    # IS-99: the installed copy's first start asked nothing (its window is up) and runs on the Default profile.
    assert TaskDialog.all(pids=[app.pid]) == [], "the installed copy shows a dialog at its first start"
    assert (install.default_profile() / "terminal").is_dir(), "Terminal's own settings folder is not in the Default profile"
    lines = install.install_log_lines()
    assert any("install begin:" in ln for ln in lines), f"no [install] begin line in the Default profile's hooks.log: {lines}"
    assert not any("install FAILED" in ln for ln in lines), lines
    _assert_offline_check_failed_quietly()


def _assert_offline_check_failed_quietly() -> None:
    """IS-88: the installed copy's startup update check, offline. The lane points api.github.com at 127.0.0.1, so the
    check of a portable install (an updater channel) fails at once; it must say so in its `[update]` lines, end within
    its 6 s bound (Updater.h RunUpdateCheckAndPrompt's kDeadlineMs: past it the check is "abandoned"), and the app must
    start anyway — the caller has already seen its window.

    Returns:
        None.
    Raises:
        AssertionError: Naming the lines that are missing or the time it took.
    """
    updates = install.profile_log_lines("[update]")
    begins = [i for i, ln in enumerate(updates) if "check (startup) begin:" in ln]
    assert begins, f"no startup update check in the Default profile's hooks.log: {updates}"
    after = updates[begins[-1] + 1:]
    end = next((ln for ln in after if "check (startup)" in ln), None)
    assert end is not None and "check (startup) failed:" in end, \
        f"the offline startup check did not fail quietly: {updates[begins[-1]]!r} then {end!r}"
    took = install.log_time_s(end) - install.log_time_s(updates[begins[-1]])
    if took < 0:
        took += 86400  # across midnight
    assert took < 6.0, f"the offline check took {took:.1f} s (its bound is 6 s): {updates[begins[-1]]} / {end}"


@pytest.mark.p0
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-43", "IS-44")
def test_ask_me_later_then_keep_it_portable(lane, kit, machine):
    """"Ask me later" writes nothing and asks again; "Keep it portable" then asks for a profile once and never again.

    Promise (INS-02 phases 2 and 3): a user who is not ready loses nothing and gets the question back; a portable copy
    stays self-contained (no registry, no shortcuts) and remembers its choice in its own folder.

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming what was written, asked or started wrongly.
    """
    exe = kit.copy("0.8.1.0", "later")
    copy = exe.parent

    # Ask me later: the copy runs on <copy>\profile and writes no decision.
    question, started = _start_copy(exe)
    question.press(r"^Ask me later")
    install.wait_app(copy, newer_than=started)
    assert (copy / "profile").is_dir(), "the 'later' run does not run on <copy>\\profile"
    assert not (copy / "install.path").exists() and not (copy / "profile.path").exists(), \
        "'Ask me later' wrote a decision"
    state = install.MachineState.read()
    assert state.default_profile is None, f"'Ask me later' created the Default profile: {state.default_profile}"
    assert state.user_entry is None and state.shortcuts == [] and state.user_path == [], \
        f"'Ask me later' registered something: {state.user_entry} {state.shortcuts} {state.user_path}"
    install.end_agentmaster(copy)

    # The next launch asks again; Keep it portable, then the profile picker.
    question, started = _start_copy(exe)
    question.press(r"^Keep it portable")
    picker = TaskDialog.wait(install.PROFILE_PICKER, exe_dirs=[copy], timeout_s=60)
    labels = picker.labels()
    assert labels and labels[0].startswith("Portable profile (self-contained)"), f"the picker offers {labels}"
    for wanted in ("Default profile", "Development profile", "Browse for a profile folder"):
        assert any(label.startswith(wanted) for label in labels), f"{wanted!r} missing from the picker: {labels}"
    picker.press(r"^Portable profile")
    install.wait_app(copy, newer_than=started)
    facts = install.InstallFacts.read(copy)
    assert facts.install_path == "portable", f"install.path {facts.install_path!r}"
    assert facts.profile_path, "the profile choice was not stored in profile.path"
    assert _norm(copy / facts.profile_path) == _norm(copy / "profile") or _norm(facts.profile_path) == _norm(copy / "profile"), \
        f"profile.path {facts.profile_path!r} does not name <copy>\\profile"
    state = install.MachineState.read()
    assert state.user_entry is None and state.shortcuts == [] and state.user_path == [] and state.user_verbs == {}, \
        "a kept-portable copy registered something"
    install.end_agentmaster(copy)

    # Never asked again.
    started = time.time()
    install.start_like_explorer(exe)
    app = install.wait_app(copy, newer_than=started)
    assert TaskDialog.all(pids=[app.pid]) == [], "a kept-portable copy asked again"


@pytest.mark.p0
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-45", "IS-46")
def test_a_newer_copy_updates_and_an_older_one_hands_over(lane, kit, machine):
    """Over a per-user install, a newer copy offers to update it in place; an older one offers to use it and becomes a
    launcher stub — never a downgrade.

    Promise (INS-02 E1, E2): one per-user install per user, always the newest; a user's own file in it survives; an old
    unzip left on the desktop keeps starting the installed copy.

    Args:
        lane: The install lane's facts (the staged installer, for the first install).
        kit: Stamped zips and copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the option, version or process that is wrong.
    """
    dest = install.user_install_dir()
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    (dest / "mine.txt").write_text("the user's own file\n", encoding="utf-8")

    # A newer copy: update the installed one.
    newer = kit.copy("0.8.2.0", "newer")
    question, started = _start_copy(newer)
    first = question.labels()[0]
    assert re.fullmatch(rf"Update the installed Agentmaster \(v{_v('0.8.1.0')} {ARROW} v{_v('0.8.2.0')}\) \(recommended\)",
                        first), f"first option {first!r}; all: {question.labels()}"
    question.press(r"^Update the installed Agentmaster")
    # IS-105: the copy that updated the install is of no use now: "Delete the downloaded copy" sends its folder to the
    # Recycle Bin once its process ended (a hidden PowerShell waits for it) — recoverable, never a plain delete.
    install.answer_leftover_question(newer.parent, delete=True)
    install.wait_app(dest, timeout_s=180, newer_than=started)
    _gone(newer.parent)
    wait_until(lambda: not newer.parent.exists(), 90, interval_s=1.0, what=f"the downloaded copy {newer.parent} to be removed")
    recycled = install.run_powershell(["-Command", "(New-Object -ComObject Shell.Application).NameSpace(10).Items() | "
                                                   "ForEach-Object { $_.Name }"])
    assert newer.parent.name in recycled.output.split(), f"the copy is not in the Recycle Bin: {recycled.output[-500:]}"
    assert (newer.parent.parent).exists(), "the folder around the copy went too (it is not an extraction folder)"
    lines = install.install_log_lines()
    assert any("leftover copy:" in ln and "chose to delete it" in ln for ln in lines), f"no leftover-copy line: {lines[-8:]}"
    state = install.MachineState.read()
    assert state.user is not None and state.user.version == "0.8.2.0", f"not updated: {state.user}"
    assert (dest / "mine.txt").exists(), "the update removed the user's own file"
    assert not [n for n in state.user.names if n.startswith(".am-previous")], "the update left its set-aside folder"
    install.end_agentmaster()

    # An older copy: use the installed one; the copy becomes a stub.
    older = kit.copy("0.8.0.5", "older")
    question, started = _start_copy(older)
    first = question.labels()[0]
    assert re.fullmatch(rf"Use the installed Agentmaster \(v{_v('0.8.2.0')}\) \(recommended\)", first), \
        f"first option {first!r}; all: {question.labels()}"
    question.press(r"^Use the installed Agentmaster")
    install.answer_leftover_question(older.parent, delete=False)  # kept: it stays a stub that forwards launches
    install.wait_app(dest, timeout_s=120, newer_than=started)
    _gone(older.parent)
    stub = install.InstallFacts.read(older.parent)
    assert _norm(stub.install_path) == _norm(dest), f"the older copy is no stub of {dest}: install.path {stub.install_path!r}"
    assert install.InstallFacts.read(dest).version == "0.8.2.0", "the installed copy was downgraded"
    install.end_agentmaster()

    # The stub forwards every later launch.
    started = time.time()
    install.start_like_explorer(older)
    app = install.wait_app(dest, timeout_s=90, newer_than=started)
    _gone(older.parent)
    assert TaskDialog.all(pids=[app.pid]) == [], "the stub's launch asked something"


@pytest.mark.p1
@pytest.mark.flow("INS-03")
@pytest.mark.install("IS-47")
def test_install_for_all_users(lane, kit, machine):
    """"Install for all users of this PC" (UAC off: no prompt): Program Files, HKLM, Active Setup, the user's own icons.

    Promise (INS-03 phase 1): one install for every account, registered under its own name; the signed-in user gets
    icons of their own (no shared ones) and runs the copy as themselves.

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the registration or process that is wrong.
    """
    dest = install.machine_install_dir()
    exe = dest / "Agentmaster.exe"
    copy_exe = kit.copy("0.8.1.0", "allusers")
    question, started = _start_copy(copy_exe)
    question.press(r"^Install for all users of this PC")
    install.answer_leftover_question(copy_exe.parent, delete=False, timeout_s=240)  # after the elevated install's window
    install.wait_app(dest, timeout_s=240, newer_than=started)
    _gone(copy_exe.parent)
    state = install.MachineState.read()
    assert state.machine is not None and state.machine.version == "0.8.1.0", f"no all-users install: {state.machine}"
    assert state.machine.install_path == "machine" and state.machine.profile_path == "", \
        f"install.path {state.machine.install_path!r}, profile.path {state.machine.profile_path!r}"
    assert state.machine_entry is not None and state.machine_entry.get("DisplayName") == "Agentmaster (all users)", \
        f"HKLM entry: {state.machine_entry}"
    assert set(state.machine_verbs) == set(install.VERB_KEYS), f"HKLM verbs: {state.machine_verbs}"
    assert any(_norm(p) == _norm(dest) for p in state.machine_path), f"not on the machine PATH: {state.machine_path}"
    assert state.active_setup is not None and _norm(state.active_setup.get("StubPath", "")) == _norm(f'"{exe}" --register-user'), \
        f"Active Setup: {state.active_setup}"
    # The user's own icons and marker come from the shared copy's first start (MachineCopyUserUpkeep), which may finish
    # after its window shows: wait for them instead of reading once.
    def user_step_done() -> bool:
        """Whether the per-user step of the shared copy is visible: the marker and both icons.

        Returns:
            True when done.
        """
        s = install.MachineState.read()
        marker = s.active_setup_user is not None and s.active_setup is not None and \
            s.active_setup_user.get("Version") == s.active_setup.get("Version")
        icons = all(any(_norm(link.target) == _norm(exe) for link in s.shortcuts_in(f)) for f in ("desktop", "programs"))
        return marker and icons

    wait_until(user_step_done, 60, interval_s=1.0, what="the user's own icons and Active Setup marker",
               describe=lambda: (install.MachineState.read().active_setup_user,
                                 [(link.path, link.target) for link in install.MachineState.read().shortcuts]))
    state = install.MachineState.read()
    assert state.shortcuts_in("public_desktop") == [] and state.shortcuts_in("common_programs") == [], \
        "shared icons (a current release gives every user their own)"
    assert state.user is None and state.user_entry is None, "an all-users install made a per-user one"
    hold_true(lambda: len(install.app_processes(dest)) == 1, 3.0, what="one Agentmaster from Program Files")


@pytest.mark.p1
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-49")
def test_both_installs_current(lane, kit, machine):
    """A copy started where the user has an install of their own AND an all-users install exists, both of its
    version: the question recommends the user's own install and offers the all-users one without recommending it.

    Promise (PortableInstall.h DecidePromptDefault, the 2026-10-21 probe matrix): for a user with an install of their
    own, that install comes first — it is the one their icons, pins and the agentmaster command open.

    Args:
        lane: The install lane's facts (the staged installer, for the two installs).
        kit: Stamped zips and copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the option that is wrong or missing.
    """
    zip1 = kit.zip("0.8.1.0")
    for scope in ("Machine", "User"):
        run = install.run_installer(lane, "-Scope", scope, "-ZipPath", str(zip1))
        assert run.code == 0, str(run)
    exe = kit.copy("0.8.1.0", "both")
    question, started = _start_copy(exe)
    labels = question.labels()
    assert re.fullmatch(rf"Use the installed Agentmaster \(v{_v('0.8.1.0')}\) \(recommended\)", labels[0]), \
        f"first option {labels[0]!r}; all: {labels}"
    shared = next((label for label in labels if label.startswith("Use the Agentmaster installed for all users")), None)
    assert shared is not None, f"the all-users install is not offered: {labels}"
    assert re.fullmatch(rf"Use the Agentmaster installed for all users \(v{_v('0.8.1.0')}\)", shared), \
        f"the all-users option {shared!r} (recommended too, or the wrong version)"
    question.press(r"^Ask me later")
    install.wait_app(exe.parent, newer_than=started)
    assert not (exe.parent / "install.path").exists(), "'Ask me later' wrote a decision"


@pytest.mark.p1
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-51")
def test_install_here(lane, kit, machine):
    """"Install here": the copy becomes the install where it is — registered in place, nothing copied elsewhere.

    Promise (INS-02, PortableInstall.h PerformInstall in place): a user who unzipped Agentmaster where they want it gets
    the same install as "Install just for you" — Apps & Features, the verbs, the PATH, the icons — for that folder.

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the decision file, registration or copy that is wrong.
    """
    # Named like a real unzip (the release zip's top folder is agentmaster-<version>): the lane's PATH reading, its
    # baseline check and Reset-AmQa know a PATH entry by "agentmaster" in it, and the first run of this test, with a
    # copy named "here", left C:\e2e\kit\run\here on the user PATH unseen (2026-10-24).
    exe = kit.copy("0.8.1.0", "agentmaster-0.8.1.0")
    copy = exe.parent
    question, started = _start_copy(exe)
    question.press(r"^Install here$")
    # In place, the elevated start (UAC off) hands over to a start without rights from the same folder.
    app = install.wait_app(copy, timeout_s=180, newer_than=started)
    facts = install.InstallFacts.read(copy)
    assert facts.install_path == "installed", f"install.path {facts.install_path!r} in {copy}"
    assert _norm(facts.profile_path) == _norm(install.default_profile()), f"profile.path {facts.profile_path!r}"
    state = install.MachineState.read()
    assert state.user is None, f"'Install here' copied the app to {install.user_install_dir()}"
    e = state.user_entry
    assert e is not None and _norm(e.get("InstallLocation", "")) == _norm(copy), f"HKCU Apps & Features: {e}"
    assert _norm(e.get("UninstallString", "")) == _norm(f'"{exe}" --uninstall-portable'), e
    assert set(state.user_verbs) == set(install.VERB_KEYS), f"verbs: {state.user_verbs}"
    assert any(_norm(p) == _norm(copy) for p in state.user_path), f"{copy} not on the user PATH: {state.user_path}"
    for folder in ("desktop", "programs"):
        assert any(_norm(link.target) == _norm(exe) for link in state.shortcuts_in(folder)), \
            f"no {folder} icon of {exe}: {state.shortcuts_in(folder)}"
    assert TaskDialog.all(pids=[app.pid]) == [], "the copy installed in place asked something more"


PICK_TITLE = "Choose where to install Agentmaster"
"""The folder dialog's title for "Install to a folder you pick…" (PortableInstall.h ShowInstallPrompt, BrowseForFolder)."""


def _pick_folder(copy_dir: Path, folder: Path) -> None:
    """Answer the copy's folder dialog: the common IFileOpenDialog (a #32770 window), its "Folder:" box given a full
    path, then "Select Folder" — what a user does who types or pastes the path.

    Args:
        copy_dir: The copy's folder (the dialog belongs to its process).
        folder: The folder to pick (it must exist: the dialog checks).
    Returns:
        None.
    Raises:
        WaitTimeout: When the dialog or one of its controls did not come.
    """
    picker = wait_until(lambda: next(iter(TaskDialog.all(exe_dirs=[copy_dir], title=PICK_TITLE)), None), 60,
                        what="the folder dialog", describe=lambda: [repr(d) for d in TaskDialog.all(title=None)])
    root = picker.element()
    root.wait_find(30, role="edit", name="Folder:").set_value(str(folder))
    root.wait_find(10, role="button", name="Select Folder").invoke()


@pytest.mark.p2
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-50")
def test_install_to_a_folder_you_pick(lane, kit, machine):
    """"Install to a folder you pick…": the folder dialog; a folder inside the copy is refused and the question comes
    back; a folder that holds files of the user's gets the install in its own `Agentmaster` subfolder, registered there,
    and the user's files stay as they were. (The row's third case — Program Files without administrator rights — needs
    a user who has none: the `uac` machine.)

    Promise (INS-02, PortableInstall.h idBrowse and PerformInstall): like any installer, a pick of a busy folder never
    mixes the app into the user's files, and a pick that would copy the app into itself is refused with a way out.

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the dialog, folder or registration that is wrong.
    """
    target = install.E2E_ROOT / "w" / "picked"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    (target / "notes.txt").write_text("the user's own file\n", encoding="utf-8")
    exe = kit.copy("0.8.1.0", "agentmaster-pick")
    inside = exe.parent / "sub"
    inside.mkdir()

    # A folder inside the copy: refused, and the question comes back.
    question, started = _start_copy(exe)
    question.press(r"^Install to a folder you pick")
    _pick_folder(exe.parent, inside)
    error = TaskDialog.wait("The install did not complete", exe_dirs=[exe.parent], timeout_s=60)
    assert "must not contain (or sit inside) the folder Agentmaster is running from" in error.text(), error.text()
    error.press(r"^OK$")
    question = install.wait_question(exe.parent, timeout_s=60)
    assert not (inside / "Agentmaster.exe").exists(), "the refused pick installed into the copy"

    # A folder of the user's files: the install goes to its Agentmaster subfolder.
    question.press(r"^Install to a folder you pick")
    _pick_folder(exe.parent, target)
    dest = target / "Agentmaster"
    # No "Delete the downloaded copy?" here: the copy holds `sub`, which the download did not bring, so its folder is
    # never offered for deletion (PortableInstall.h JudgeLeftoverCopy) — the installed copy starts without the question.
    install.wait_app(dest, timeout_s=180, newer_than=started)
    lines = install.install_log_lines()
    assert any("leftover copy:" in ln and "is not offered for deletion" in ln and "holds sub" in ln for ln in lines), \
        f"no leftover-copy verdict naming the user's folder in the copy: {lines[-8:]}"
    facts = install.InstallFacts.read(dest)
    assert facts is not None and facts.version == "0.8.1.0", f"no install in {dest}: {facts}"
    assert facts.install_path == "installed", f"install.path {facts.install_path!r} in {dest}"
    assert (target / "notes.txt").exists() and not (target / "Agentmaster.exe").exists(), \
        f"the install went into the picked folder itself, beside the user's files: {sorted(p.name for p in target.iterdir())}"
    e = install.MachineState.read().user_entry
    assert e is not None and _norm(e.get("InstallLocation", "")) == _norm(dest), f"not registered in {dest}: {e}"


@pytest.mark.p2
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-57")
def test_the_old_scripts_copy_is_updated(lane, kit, machine):
    """The copy the OLD install script made (v0.7.10's Install-Agentmaster.ps1: %LOCALAPPDATA%\\Programs\\Agentmaster,
    a Start-menu entry "Agentmaster (Portable)", nothing registered), then a newer copy: its question offers to update
    THAT copy, which becomes a registered install in its own folder, the old entry replaced by the usual one.

    Promise (PROFILES.md §2b, the A1 pass of the 2026-10-21 night): a user of the old script never gets a second copy
    beside the first; the update keeps their folder and gives it what an install has.

    Args:
        lane: The install lane's facts (the older release, v0.7.10 as published).
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the option, folder or registration that is wrong.
    """
    legacy = install.legacy_script_dir()
    old_entry = install.known_folder("programs") / "Agentmaster (Portable).lnk"
    # What the old script left, made by hand: the A1 pass ran that script itself, and it needs GitHub's API, which the
    # lane blocks. Its folder holds the release as extracted (never started: no install.path) and its Start-menu entry.
    shutil.copytree(lane.old, legacy, ignore=shutil.ignore_patterns(".e2e-source"))
    run = install.make_shortcut(old_entry, legacy / "Agentmaster.exe")
    assert run.code == 0 and old_entry.exists(), str(run)
    old_version = install.InstallFacts.read(legacy).version
    assert old_version, f"the older release in {legacy} carries no .am-version"

    exe = kit.copy("0.8.1.0", "agentmaster-0.8.1.0-newer")
    question, started = _start_copy(exe)
    first = question.buttons()[0]
    assert re.fullmatch(rf"Update the installed Agentmaster \(v{_v(old_version)} {ARROW} v{_v('0.8.1.0')}\) \(recommended\)",
                        first.label), f"first option {first.label!r}; all: {question.labels()}"
    # The note is the command link's second line, "<folder>  —  the copy the install script made; …" (UI Automation's
    # HelpText: Button.note).
    assert re.match(re.escape(str(legacy)) + r"\s+—\s+the copy the install script made", first.note, re.IGNORECASE), \
        f"the first option's note does not name the old script's copy: {first.note!r}"
    question.press(r"^Update the installed Agentmaster")
    install.answer_leftover_question(exe.parent, delete=False)
    install.wait_app(legacy, timeout_s=180, newer_than=started)
    _gone(exe.parent)
    facts = install.InstallFacts.read(legacy)
    assert facts.version == "0.8.1.0", f"the old script's copy was not updated: {facts.version}"
    assert facts.install_path == "installed", f"install.path {facts.install_path!r} in {legacy}"
    state = install.MachineState.read()
    e = state.user_entry
    assert e is not None and _norm(e.get("InstallLocation", "")) == _norm(legacy), f"not registered in {legacy}: {e}"
    assert state.user is None, f"a second copy went to {install.user_install_dir()}"
    assert not old_entry.exists(), "the old script's 'Agentmaster (Portable)' entry is left"
    assert any(_norm(link.target) == _norm(legacy / "Agentmaster.exe") for link in state.shortcuts_in("programs")), \
        f"no Start-menu entry of the updated copy: {[(s.path, s.target) for s in state.shortcuts_in('programs')]}"
