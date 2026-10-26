"""The user's data survives every install: an install into the Default profile's `bin` beside an all-users copy the user
already worked in.

flows/19 INS-02 and INS-10 — the catalog's section H (IS-100–IS-104). Found 2026-10-26 by the owner: an all-users install
was in use (sessions with titles in `%USERPROFILE%\\.agentmaster`), then a portable copy's "Install just for you", which
installs into `%USERPROFILE%\\.agentmaster\\bin` — INSIDE that profile — and the profile's data was gone afterwards. Each
test plants a profile the way a user's work leaves one (the shared copy's own first start makes the real folder; the
sessions, titles, tags, window records and a file of the user's own are added on top), runs an install the way a user
does, and compares every file of the profile before and after.

Runs only in the install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import time
from pathlib import Path

import pytest

from amdriver import install
from amdriver.ui.taskdialog import TaskDialog
from amdriver.waits import wait_until

pytestmark = [pytest.mark.install]

SESSIONS = [
    {"id": "3f1c2a90-5b7e-4d2a-9c41-0d6e8f7a1b23", "title": "Refactor the billing service"},
    {"id": "8a4d6e12-0c3b-4f59-b7a8-2e9d5c1f4a60", "title": "Investigate the flaky upload test"},
]
"""The sessions the user had (sessions.json records with their titles), as the all-users copy would have written them."""

OWN_FILES = {
    "my-notes.txt": b"notes the user keeps beside the profile\r\n",
    "kept/deep/scratch.bin": bytes(range(256)) * 64,
}
"""Files of the user's own in the profile: nothing Agentmaster knows about, so nothing may ever touch them."""


def _norm(path) -> str:
    """A path as Windows compares it (case-insensitive, no trailing backslash).

    Args:
        path: A path or text.
    Returns:
        The normalized text.
    """
    return os.path.normcase(str(path).rstrip("\\"))


def _snapshot(profile: Path) -> dict[str, str]:
    """Every file of a profile folder and its SHA-256, outside the per-user install folder `bin` (the binaries an install
    lays down there are not the user's data).

    Args:
        profile: The profile folder.
    Returns:
        relative path (forward slashes) -> hex digest; empty when the folder does not exist.
    """
    out: dict[str, str] = {}
    if not profile.is_dir():
        return out
    for path in profile.rglob("*"):
        rel = path.relative_to(profile).as_posix()
        if rel == "bin" or rel.startswith("bin/") or not path.is_file():
            continue
        try:
            out[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as error:  # a log the running app holds open: its presence is what counts
            out[rel] = f"unreadable: {error.__class__.__name__}"
    return out


def _plant_user_data(profile: Path) -> None:
    """Add what a user's work leaves in a profile: sessions with titles, a per-session record with a title of the user's
    own and a tag, the known-tag registry, and files of the user's own (one nested).

    Args:
        profile: The Default profile (made by the shared copy's first start).
    Returns:
        None.
    Raises:
        OSError: When a file cannot be written.
    """
    now_ms = int(time.time() * 1000)
    fleet = {"version": 1, "sessions": [
        {"id": s["id"], "title": s["title"], "workingDir": r"C:\e2e", "branch": "", "state": "Idle",
         "lastActivityUnixMs": now_ms} for s in SESSIONS]}
    (profile / "sessions.json").write_text(json.dumps(fleet, indent=1), encoding="utf-8")
    store = profile / "session-store"
    store.mkdir(exist_ok=True)
    # SessionStore.cpp's record: one JSON object of string values; the tag list is JSON-encoded into one string.
    for s in SESSIONS:
        (store / f"{s['id']}.json").write_text(json.dumps({"title": s["title"], "tags": json.dumps(["important"])}),
                                               encoding="utf-8")
    (profile / "tags.json").write_text(json.dumps({"tags": ["important"]}), encoding="utf-8")
    for rel, data in OWN_FILES.items():
        target = profile / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def _assert_user_data_kept(profile: Path, before: dict[str, str]) -> None:
    """Check that an install kept the user's data: every file that was in the profile is still there, the user's own
    files byte for byte, and the sessions with their titles (the app may rewrite sessions.json when it starts, so its
    sessions are checked by content, not by bytes).

    Args:
        profile: The Default profile.
        before: Its _snapshot from before the install.
    Returns:
        None.
    Raises:
        AssertionError: Naming every file that is gone or changed, with what the folder holds now.
    """
    after = _snapshot(profile)
    gone = sorted(rel for rel in before if rel not in after)
    assert not gone, (f"the install removed {len(gone)} of the {len(before)} files of {profile}: {gone[:40]}\n"
                      f"the folder now: {sorted(after)[:60]}")
    for rel, data in OWN_FILES.items():
        assert (profile / rel).read_bytes() == data, f"the user's own file {rel} changed"
    text = (profile / "sessions.json").read_text(encoding="utf-8")
    for s in SESSIONS:
        assert s["id"] in text and s["title"] in text, f"sessions.json lost {s}: {text[:600]}"
    for s in SESSIONS:
        record = profile / "session-store" / f"{s['id']}.json"
        assert record.exists() and s["title"] in record.read_text(encoding="utf-8"), f"the title record {record} is gone"


def _use_the_shared_copy() -> Path:
    """Start the all-users copy the way the user does (Explorer), let it make the Default profile, then close it.

    Returns:
        The Default profile.
    Raises:
        WaitTimeout: When the copy did not start or made no profile.
    """
    shared = install.machine_install_dir() / "Agentmaster.exe"
    started = time.time()
    install.start_like_explorer(shared)
    install.wait_app(install.machine_install_dir(), timeout_s=120, newer_than=started)
    profile = install.default_profile()
    wait_until(lambda: (profile / "terminal").is_dir(), 60, interval_s=0.5,
               what="the shared copy's Default profile (its terminal settings folder)")
    install.end_agentmaster()
    return profile


@pytest.mark.p0
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-100", "IS-101")
def test_just_for_you_beside_the_all_users_copy_keeps_the_profile(lane, kit, machine):
    """An all-users install the user worked in, then a newer copy's "Install just for you": the per-user install goes
    into `%USERPROFILE%\\.agentmaster\\bin`, inside the profile that holds the user's sessions — and every file of that
    profile is still there afterwards, and the installed copy runs on it.

    Promise (PROFILES.md §2b "The installed copy's state", Rule #16): an install never removes or overwrites the user's
    data; the installed copy adopts the profile the user already has.

    Args:
        lane: The install lane's facts (the staged installer, for the all-users install).
        kit: Stamped zips and copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the files that are gone, or the step that did not happen.
    """
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    profile = _use_the_shared_copy()
    _plant_user_data(profile)
    before = _snapshot(profile)
    assert len(before) >= len(OWN_FILES) + len(SESSIONS) + 2, f"the planted profile is too small to prove anything: {before}"

    exe = kit.copy("0.8.2.0", "beside-shared")
    started = time.time()
    install.start_like_explorer(exe)
    question = install.wait_question(exe.parent)
    labels = question.labels()
    assert labels and re.match(r"^Install just for you", labels[0]), f"the first option is {labels[:1]}; all: {labels}"
    question.press(r"^Install just for you")
    install.answer_leftover_question(exe.parent, delete=False)
    app = install.wait_app(install.user_install_dir(), timeout_s=180, newer_than=started)
    _assert_user_data_kept(profile, before)
    facts = install.InstallFacts.read(install.user_install_dir())
    assert facts is not None and facts.install_path == "installed", f"no per-user install: {facts}"
    assert _norm(facts.profile_path) == _norm(profile), f"the installed copy's profile.path {facts.profile_path!r}"
    assert TaskDialog.all(pids=[app.pid]) == [], "the installed copy asked something at its first start"


PACKAGE_KIT = install.SRC_KIT / "pkg-0.7.0"
"""The v0.7.0 release's msixbundle and its certificate, staged by the host (install_lane.py stage_package_kit): the
"admin installation" of the owner's report — the release package needs an administrator once, to trust its
certificate."""
PACKAGE_FAMILY = "Agentmaster_56k4f06dsfp9r"
"""The release package's family (CN=Agentmaster); its profile choice is the home map's line under this key."""


def _package_location() -> str:
    """The installed release package's folder, or "" when it is not installed.

    Returns:
        Its InstallLocation.
    """
    run = install.run_powershell(["-Command", "(Get-AppxPackage -Name Agentmaster).InstallLocation"])
    return run.output.strip().splitlines()[-1].strip() if run.code == 0 and run.output.strip() else ""


def _package_processes(location: str) -> list:
    """The release package's running GUI processes (its 0.7.0 app is WindowsTerminal.exe, in a WindowsApps folder that
    _beside_agentmaster cannot read, so the driver's app_processes never lists it).

    Args:
        location: The package's InstallLocation.
    Returns:
        psutil processes.
    """
    import psutil  # noqa: PLC0415 - the guest's venv has it; kept local like the test's other one-off reads

    root = location.rstrip("\\").lower() + "\\"
    found = []
    for p in psutil.process_iter(["name", "exe"]):
        try:
            if (p.info["name"] or "").lower() in ("windowsterminal.exe", "agentmaster.exe") and \
                    (p.info["exe"] or "").lower().startswith(root):
                found.append(p)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return found


WORKSPACE_TITLES = ("Alpha workspace tab", "Beta workspace tab")
"""The titled tabs the user's package app has open (its window record keeps them; the installed copy must show them)."""


def _tab_names(pid: int) -> list[str]:
    """The tab titles an Agentmaster process shows across its main windows (UI Automation's tab items).

    Args:
        pid: The process.
    Returns:
        The names, in window and strip order.
    """
    from amdriver import uia  # noqa: PLC0415 - only this check reads the tab strip

    names: list[str] = []
    for w in install.win32.top_windows(pid):
        if w.cls != install.MAIN_CLASS or not w.visible:
            continue
        try:
            names += [t.name for t in uia.Element.from_hwnd(w.hwnd).find_all(30, role="tab item")]
        except Exception:  # noqa: BLE001 - a window closing under the read counts as no tabs
            continue
    return names


def _record_titles(profile: Path) -> str:
    """Every window record of a profile, as one text (a tab's title is in its record's startup actions).

    Args:
        profile: The profile folder.
    Returns:
        The records' text joined ("" when there are none).
    """
    folder = profile / "windows"
    return "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in folder.glob("*.json")) if folder.is_dir() else ""


def _use_the_old_package(with_workspace: bool = False) -> tuple[Path, str]:
    """Install the v0.7.0 release package (its certificate trusted machine-wide, the bundle added for the signed-in
    user), start it the way the user does (Start menu: shell:appsFolder), let it make its profile, then close it.

    Args:
        with_workspace: Also open WORKSPACE_TITLES as titled tabs through the package's own `agentmaster` alias, and
            wait until its window record keeps them, before the app is closed — the workspace a user has.
    Returns:
        (the Default profile, the package's InstallLocation).
    Raises:
        AssertionError / WaitTimeout: When the package cannot be installed or never ran.
    """
    bundle, cer = PACKAGE_KIT / "Agentmaster_0.7.0.0.msixbundle", PACKAGE_KIT / "Agentmaster.cer"
    assert bundle.exists() and cer.exists(), f"the host staged no package kit in {PACKAGE_KIT}"
    run = install.run_powershell(["-Command", f"Import-Certificate -FilePath '{cer}' -CertStoreLocation "
                                              f"Cert:\\LocalMachine\\TrustedPeople | Out-Null; Add-AppxPackage -Path '{bundle}'"])
    assert run.code == 0, f"the v0.7.0 package did not install:\n{run}"
    location = _package_location()
    assert location, "the package is not listed after Add-AppxPackage"
    started = time.time()
    install.start_like_explorer(Path(f"shell:appsFolder\\{PACKAGE_FAMILY}!App"), cwd=install.E2E_ROOT)
    profile = install.default_profile()
    wait_until(lambda: (profile / "terminal").is_dir() and _package_processes(location), 120, interval_s=0.5,
               what="the v0.7.0 package's app and its Default profile")
    wait_until(lambda: (profile.parent / ".agentmaster.profiles").exists(), 30, interval_s=0.5,
               what="the package's profile choice in the home map")
    assert all(p.create_time() >= started - 5 for p in _package_processes(location)), "an older package process runs"
    if with_workspace:
        # The package's own execution alias (Package-Rel.appxmanifest: agentmaster.exe), as a user types it: two
        # titled tabs in the running window. Its window record keeps them as startup actions (BuildStartupActions).
        alias = Path(os.environ["LOCALAPPDATA"]) / "Microsoft" / "WindowsApps" / "agentmaster.exe"
        argv = [str(alias), "-w", "0"]
        for n, title in enumerate(WORKSPACE_TITLES):
            argv += ([";"] if n else []) + ["nt", "--title", title, "cmd.exe", "/k", "echo", title]
        import subprocess  # noqa: PLC0415 - only the workspace step starts the alias

        subprocess.Popen(argv, cwd=str(install.E2E_ROOT), env=install.clean_env(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_until(lambda: all(t in _record_titles(profile) for t in WORKSPACE_TITLES), 90, interval_s=1.0,
                   what="the package app's window record with the titled tabs",
                   describe=lambda: _record_titles(profile)[:1500])
    for p in _package_processes(location):
        p.kill()
    wait_until(lambda: not _package_processes(location), 30, interval_s=0.5, what="the package's app to end")
    install.end_agentmaster(drain_s=20.0)
    return profile, location


@pytest.mark.p0
@pytest.mark.flow("INS-08")
@pytest.mark.install("IS-103")
def test_just_for_you_over_the_0_7_0_package_keeps_the_profile(lane, kit, machine):
    """The owner's case of 2026-10-26: the release PACKAGE v0.7.0 installed (the "admin installation": its certificate
    needs an administrator) and used — sessions and titles in `%USERPROFILE%\\.agentmaster` — then a newer portable
    copy's question, answered "Install just for you" with nothing else touched (the package box keeps its default).
    Every file of the profile must still be there, the installed copy must run on it, and the package goes (the box's
    default when it does not run).

    Promise (PROFILES.md §2b "Existing installs": the install adopts the package's DATA; "its data … is kept and
    reused" is the box's own text).

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the files that are gone, or the step that did not happen.
    """
    profile, location = _use_the_old_package(with_workspace=True)
    _plant_user_data(profile)
    before = _snapshot(profile)
    home_map_before = (profile.parent / ".agentmaster.profiles").read_text(encoding="utf-8", errors="replace")

    exe = kit.copy("0.8.2.0", "over-package")
    started = time.time()
    install.start_like_explorer(exe)
    question = install.wait_question(exe.parent)
    text = question.text()
    assert "An installed Agentmaster package v0.7.0" in text, f"the question does not name the package:\n{text}"
    question.press(r"^Install just for you")
    install.answer_leftover_question(exe.parent, delete=False, timeout_s=240)
    app = install.wait_app(install.user_install_dir(), timeout_s=240, newer_than=started)
    _assert_user_data_kept(profile, before)
    facts = install.InstallFacts.read(install.user_install_dir())
    assert facts is not None and _norm(facts.profile_path) == _norm(profile), \
        f"the installed copy does not run on the package's profile: {facts}; the home map before: {home_map_before!r}"
    assert not _package_location(), "the package was not removed (the box's default when it does not run)"
    # The workspace, not only the files: the installed copy reopens the package app's window with its titled tabs.
    wait_until(lambda: all(any(t in name for name in _tab_names(app.pid)) for t in WORKSPACE_TITLES), 60,
               interval_s=1.0, what="the package app's titled tabs in the installed copy's window",
               describe=lambda: (_tab_names(app.pid), _record_titles(profile)[:1500]))


PROFILE_IN_USE = r"is already in use by another running Agentmaster instance"
"""The text of the warning a second instance shows on a profile another instance holds (ProfileBootstrap.h
EnsureProfileResolvedAtStartup: "Agentmaster — profile in use", Yes = continue anyway, No = exit)."""


@pytest.mark.p0
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-102")
def test_just_for_you_while_the_all_users_copy_runs_keeps_the_profile(lane, kit, machine):
    """The same install while the all-users copy RUNS on the profile (the user worked in it, and installed from a
    download without closing it): the installed copy starts on that very profile, a second instance — and still every
    file of the profile is there afterwards, with every session and title.

    Promise (Rule #16, PROFILES.md §2b): nothing an install or a first start does removes the user's data, whatever else
    runs on the profile.

    Args:
        lane: The install lane's facts (the staged installer, for the all-users install).
        kit: Stamped zips and copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the files that are gone, or the step that did not happen.
    """
    run = install.run_installer(lane, "-Scope", "Machine", "-ZipPath", str(kit.zip("0.8.1.0")))
    assert run.code == 0, str(run)
    profile = _use_the_shared_copy()
    _plant_user_data(profile)
    # The shared copy runs again, on the planted profile, and stays up through the install.
    started = time.time()
    install.start_like_explorer(install.machine_install_dir() / "Agentmaster.exe")
    shared = install.wait_app(install.machine_install_dir(), timeout_s=120, newer_than=started)
    time.sleep(3)  # its engine loads the fleet and saves its first records right after the window shows
    before = _snapshot(profile)

    exe = kit.copy("0.8.2.0", "while-shared-runs")
    started = time.time()
    install.start_like_explorer(exe)
    question = install.wait_question(exe.parent)
    question.press(r"^Install just for you")
    install.answer_leftover_question(exe.parent, delete=False)

    def installed_copy_state() -> str:
        """What the installed copy shows: its profile-in-use warning, its window, or nothing yet.

        Returns:
            "warning", "window" or "".
        """
        for p in install.app_processes(install.user_install_dir()):
            if p.created < started:
                continue
            if install.MessageBox.find(p.pid, text_re=PROFILE_IN_USE) is not None:
                return "warning"
            if any(w.cls == install.MAIN_CLASS and w.visible for w in install.win32.top_windows(p.pid)):
                return "window"
        return ""

    seen = wait_until(installed_copy_state, 180, interval_s=0.5, what="the installed copy's warning or window")
    after_install = _snapshot(profile)
    gone_at_install = sorted(rel for rel in before if rel not in after_install)
    if seen == "warning":
        # A user who wants their Agentmaster goes on: two instances on one profile from here.
        for p in install.app_processes(install.user_install_dir()):
            box = install.MessageBox.find(p.pid, text_re=PROFILE_IN_USE)
            if box is not None:
                box.press("Yes")
        install.wait_app(install.user_install_dir(), timeout_s=120, newer_than=started)
    time.sleep(5)  # the installed copy's engine loads the fleet and writes its first records right after its window
    after_start = _snapshot(profile)
    gone_at_start = sorted(rel for rel in before if rel not in after_start)
    install.end_agentmaster()
    assert shared.pid not in [p.pid for p in install.app_processes()], "the shared copy outlived end_agentmaster"
    assert not gone_at_install, f"the install itself removed {gone_at_install}"
    assert not gone_at_start, f"the installed copy's start ({seen}) removed {gone_at_start}"
    _assert_user_data_kept(profile, before)


def _plant_bin_in_the_way(profile: Path) -> dict[str, bytes]:
    """Put files of the user's own into %USERPROFILE%\\.agentmaster\\bin before any Agentmaster install exists there: a
    folder the install would otherwise lay its files into (a tool of theirs, a note, a nested folder).

    Args:
        profile: The Default profile.
    Returns:
        relative path inside bin -> its bytes.
    Raises:
        OSError: When a file cannot be written.
    """
    files = {"my-tool.exe": b"MZ not Agentmaster", "notes.txt": b"kept in bin by the user\r\n", "sub/data.bin": bytes(range(64))}
    for rel, data in files.items():
        target = profile / "bin" / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return files


def _assert_bin_kept_aside(profile: Path, files: dict[str, bytes]) -> Path:
    """Check that the folder that was in the way was renamed aside and kept whole, and that the install went into a
    fresh bin.

    Args:
        profile: The Default profile.
        files: What _plant_bin_in_the_way wrote.
    Returns:
        The folder kept aside.
    Raises:
        AssertionError: Naming what is missing or mixed.
    """
    kept = sorted(p for p in profile.iterdir() if p.is_dir() and p.name.startswith("bin.before-install-"))
    assert len(kept) == 1, f"expected one bin.before-install-* folder in {profile}: {sorted(p.name for p in profile.iterdir())}"
    for rel, data in files.items():
        assert (kept[0] / rel).read_bytes() == data, f"the kept folder lost or changed {rel}"
    mixed = [rel for rel in files if (profile / "bin" / rel).exists()]
    assert not mixed, f"the install mixed itself into the user's folder: {mixed} are still in bin"
    facts = install.InstallFacts.read(profile / "bin")
    assert facts is not None and facts.install_path == "installed", f"no install in the fresh bin: {facts}"
    return kept[0]


@pytest.mark.p0
@pytest.mark.flow("INS-02")
@pytest.mark.install("IS-106")
def test_a_bin_folder_in_the_way_is_kept_aside(lane, kit, machine):
    """%USERPROFILE%\\.agentmaster\\bin already holds files that are not an Agentmaster install's, and the user has worked
    in the profile: "Install just for you" renames that folder aside and keeps it whole ("bin.before-install-<time>"),
    lays the install into a fresh bin, and leaves every file of the profile as it was.

    Promise (PROFILES.md §2b "No install ever loses the user's state", PortableInstall.h SetAsideFolderInTheWay): an
    install never mixes itself into, overwrites or empties a folder of someone else's files.

    Args:
        lane: The install lane's facts.
        kit: Stamped copies of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the files that are gone or mixed.
    """
    profile = install.default_profile()
    profile.mkdir(parents=True, exist_ok=True)
    _plant_user_data(profile)
    files = _plant_bin_in_the_way(profile)
    before = _snapshot(profile)  # bin/ is outside the snapshot: its files are checked in the folder kept aside

    exe = kit.copy("0.8.2.0", "bin-in-the-way")
    started = time.time()
    install.start_like_explorer(exe)
    question = install.wait_question(exe.parent)
    question.press(r"^Install just for you")
    install.answer_leftover_question(exe.parent, delete=False)
    install.wait_app(install.user_install_dir(), timeout_s=180, newer_than=started)
    kept = _assert_bin_kept_aside(profile, files)
    _assert_user_data_kept(profile, before)
    lines = install.install_log_lines()
    assert any("not Agentmaster's" in ln and kept.name in ln for ln in lines), f"the install log does not name the kept folder: {lines[-8:]}"


@pytest.mark.p1
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-107")
def test_the_installer_keeps_a_bin_folder_in_the_way(lane, kit, machine):
    """The one-command installer, `-Scope User`, with %USERPROFILE%\\.agentmaster\\bin holding files of the user's own:
    the shared PowerShell core renames the folder aside and keeps it (Move-AmFolderInTheWay), installs into a fresh bin,
    and the profile's files stay.

    Promise (PROFILES.md §2b, the core's Install-AmLayout): the same rule as the app's own install.

    Args:
        lane: The install lane's facts (the staged installer).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
    Returns:
        None.
    Raises:
        AssertionError: Naming the files that are gone or mixed, with the installer's run.
    """
    profile = install.default_profile()
    profile.mkdir(parents=True, exist_ok=True)
    _plant_user_data(profile)
    files = _plant_bin_in_the_way(profile)
    before = _snapshot(profile)
    # No console input (run_powershell): a question the script reaches takes its default and says so.
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(kit.zip("0.8.2.0")))
    assert run.code == 0, str(run)
    kept = _assert_bin_kept_aside(profile, files)
    assert kept.name in " ".join(run.output.split()), f"the installer did not say where the folder went:\n{run}"
    _assert_user_data_kept(profile, before)
