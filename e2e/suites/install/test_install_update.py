"""The in-app updater and the installer against releases a test publishes (flows/19 IS-11, IS-12, IS-80–IS-82, IS-87).

The `net` machine: the `fake_github` fixture (amdriver.fakegithub) answers api.github.com, github.com and
raw.githubusercontent.com from 127.0.0.1:443 inside the disposable desktop — its certificates made there, its CA
trusted there for the test only. What it shows that the real releases cannot: a release newer than the build under test
(the updater's whole flow: the startup prompt and its six choices, the cog's check, Update now, the swap, the relaunch, a
swap that fails), a token GitHub refuses, and the exact requests each program made (FakeGitHub.requests(): what was
asked, what was downloaded — or not).

flows/19 INS-06 (the in-app update: phases 1–6, E1) and INS-01 (the installer's release lookup). Runs only in the
install lane (E2E_TESTING.md §8.5).
"""
from __future__ import annotations

import ctypes
import json
import re
import time
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional

import psutil
import pytest

from amdriver import install, win32
from amdriver.app import MAIN_CLASS
from amdriver.ui import installed
from amdriver.ui.taskdialog import TaskDialog
from amdriver.waits import hold_true, wait_until

pytestmark = [pytest.mark.install]

CHOICES = ("Remind me next restart", "Remind me tomorrow", "Remind me in 3 days", "Remind me in 7 days",
           "Remind me in 30 days", "Skip this version")
"""The update prompt's six choices, in order (Updater.h ShowUpdatePrompt); "tomorrow" carries its day and time."""
DAY_MS = 24 * 3600 * 1000
"""A day in milliseconds."""
LOCKED_NAME = "NOTICE.html"
"""IS-87's locked file: one the release ships and the app never opens, so the app runs while it is locked."""


# ---- helpers ------------------------------------------------------------------------------------------------------

def _install_user(lane: install.Lane, kit: install.Kit, version: str) -> Path:
    """Install a version just for you with the installer (non-interactive), the way a user's first install leaves it.

    Args:
        lane: The lane.
        kit: The kit.
        version: The stamped version ("0.8.1.0").
    Returns:
        The install folder.
    Raises:
        AssertionError: When the install failed.
    """
    run = install.run_installer(lane, "-Scope", "User", "-ZipPath", str(kit.zip(version)))
    assert run.code == 0, str(run)
    return install.user_install_dir()


def _settings() -> dict:
    """The Default profile's settings (settings.json's "settings" object).

    Returns:
        The object ({} when the file does not exist yet).
    Raises:
        json.JSONDecodeError: When the file is not JSON.
    """
    path = install.default_profile() / "settings.json"
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8-sig")).get("settings", {})


def _set_settings(**values) -> None:
    """Merge values into the Default profile's settings.json (its {"version": 1, "settings": {…}} envelope), the way
    the app's own read-modify-writes keep everything else; a value of None removes the key.

    Args:
        **values: The keys and values.
    Returns:
        None.
    Raises:
        OSError: When the file cannot be written.
    """
    path = install.default_profile() / "settings.json"
    root = json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {"version": 1, "settings": {}}
    settings = root.setdefault("settings", {})
    for key, value in values.items():
        if value is None:
            settings.pop(key, None)
        else:
            settings[key] = value
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.e2e-tmp")
    tmp.write_text(json.dumps(root, indent=1), encoding="utf-8")
    tmp.replace(path)


def _prompt(dest: Path, version_tag: str, timeout_s: float = 90.0) -> TaskDialog:
    """The update prompt of the copy in a folder ("Agentmaster <tag> is available").

    Args:
        dest: The copy's folder.
        version_tag: The offered tag ("v0.8.2").
        timeout_s: How long to wait for it.
    Returns:
        The dialog.
    Raises:
        WaitTimeout: When it did not show.
    """
    return TaskDialog.wait(instruction=f"Agentmaster {version_tag} is available", exe_dirs=[dest], timeout_s=timeout_s)


def _no_prompt_for(dest: Path, seconds: float) -> None:
    """Hold that no update prompt of the copy shows for a while (its window up, nothing asked).

    Args:
        dest: The copy's folder.
        seconds: How long.
    Returns:
        None.
    Raises:
        AssertionError: When a prompt shows (its text in the message).
    """
    def quiet() -> bool:
        """Whether no update prompt of the copy is open.

        Returns:
            True while none is.
        """
        return TaskDialog.find(instruction_re=r"^Agentmaster v\S+ is available$", exe_dirs=[dest]) is None

    hold_true(quiet, seconds, interval_s=1.0, what="no update prompt",
              describe=lambda: [d.dump() for d in TaskDialog.all(exe_dirs=[dest])])


def _next_local_morning_ms(now: datetime) -> int:
    """The next local 08:00 after a moment ("Remind me tomorrow": the next occurrence, always within 24 h).

    Args:
        now: The local moment.
    Returns:
        Unix milliseconds.
    """
    morning = now.replace(hour=8, minute=0, second=0, microsecond=0)
    if morning <= now:
        morning += timedelta(days=1)
    return int(morning.timestamp() * 1000)


def _lock_against_moves(path: Path) -> int:
    """Open a file the way a program that keeps it open does: reading and writing shared, DELETE not shared — so it
    cannot be renamed or moved (a swap's set-aside move fails) while the handle lives.

    Args:
        path: The file.
    Returns:
        The handle (close it with _unlock).
    Raises:
        OSError: When it cannot be opened.
    """
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = ctypes.c_void_p
    handle = k.CreateFileW(str(path), 0x80000000, 0x1 | 0x2, None, 3, 0, None)  # GENERIC_READ, READ|WRITE, OPEN_EXISTING
    if handle in (None, ctypes.c_void_p(-1).value):
        raise OSError(f"cannot open {path}: Win32 error {ctypes.get_last_error()}")
    return handle


def _unlock(handle: int) -> None:
    """Close a handle from _lock_against_moves.

    Args:
        handle: The handle.
    Returns:
        None.
    """
    ctypes.WinDLL("kernel32").CloseHandle(ctypes.c_void_p(handle))


def _update_window(after: float, timeout_s: float = 120.0) -> int:
    """The PowerShell running the update script (am-update.ps1) the app started.

    Args:
        after: Epoch seconds it started after.
        timeout_s: How long.
    Returns:
        Its process id.
    Raises:
        WaitTimeout: When none ran.
    """
    def found() -> Optional[int]:
        """The newest powershell.exe whose command line runs am-update.ps1.

        Returns:
            Its id, or None.
        """
        for p in psutil.process_iter(["name", "cmdline", "create_time"]):
            try:
                if (p.info["name"] or "").lower() == "powershell.exe" and "am-update.ps1" in " ".join(p.info["cmdline"] or []) \
                        and (p.info["create_time"] or 0) >= after - 1:
                    return p.pid
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
        return None

    return wait_until(found, timeout_s, interval_s=0.5, what="the update script's window")


def _downloads(fake, name: str) -> list[dict]:
    """The fake's answered requests for one asset.

    Args:
        fake: The FakeGitHub.
        name: The asset's file name.
    Returns:
        The log entries of its downloads.
    """
    return [r for r in fake.requests() if r["host"] == "github.com" and r["path"].endswith("/" + name)]


# ---- the tests ----------------------------------------------------------------------------------------------------

@pytest.mark.p1
@pytest.mark.flow("INS-06")
@pytest.mark.install("IS-81", "IS-82")
def test_the_startup_prompt_and_its_choices(lane, kit, machine, fake_github):
    """An installed copy stamped older than the newest release asks at its start, before any window: Update now /
    Postpone and six choices (IS-81); each choice does what it says (IS-82) — next restart writes nothing and the next
    start asks again, tomorrow is the next local 08:00, 3 / 7 / 30 days are that many days, a postponed copy asks
    nothing, and a skipped version is never offered again while a newer one is.

    Promise (INS-06 phases 1–4): an update is offered once per start at most, never on top of a window, and a "later"
    of the user's is kept exactly as chosen.

    Args:
        lane: The install lane's facts.
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
        fake_github: The started fake GitHub.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the prompt, choice or setting that is wrong.
    """
    dest = _install_user(lane, kit, "0.8.1.0")
    fake_github.publish("v0.8.2", {"Agentmaster_0.8.2.0_x64.zip": kit.zip("0.8.2.0")})

    def start_and_answer(choice: Optional[str], tag: str = "v0.8.2") -> tuple[int, float]:
        """Start the copy, answer its prompt with a choice and Postpone, and wait for its window.

        Args:
            choice: The choice's label start (None: leave the default, "Remind me next restart").
            tag: The tag the prompt must offer.
        Returns:
            (the running app's pid, the epoch time Postpone was pressed).
        Raises:
            AssertionError / WaitTimeout: When the prompt is wrong or the app did not come up.
        """
        started = time.time()
        install.start_like_explorer(dest / "Agentmaster.exe")
        prompt = _prompt(dest, tag)
        # Before any window: the startup check runs before the app makes its first window (and before "Reopen your N
        # windows?", which only a window-less start asks).
        assert not any(w.cls == MAIN_CLASS for p in install.app_processes(dest) for w in win32.top_windows(p.pid)), \
            "the update prompt came after a window"
        labels = prompt.labels()
        assert "Update now" in labels and "Postpone" in labels, f"the prompt's buttons: {labels}"
        radios = prompt.choices()
        assert len(radios) == 6 and all(r.startswith(c) for r, c in zip(radios, CHOICES)), f"the six choices: {radios}"
        assert re.fullmatch(r"Remind me tomorrow \(\w+, 8:00\s?AM\)", radios[1]), f"tomorrow's day and time: {radios[1]!r}"
        assert prompt.chosen().startswith("Remind me next restart"), f"the default choice: {prompt.chosen()!r}"
        if choice:
            prompt.choose(rf"^{re.escape(choice)}")
        pressed = time.time()
        prompt.press(r"^Postpone$")
        app = install.wait_app(dest, timeout_s=120, newer_than=started)
        return app.pid, pressed

    # IS-81 + Remind me next restart: nothing written, the next start asks again.
    start_and_answer(None)
    s = _settings()
    assert not s.get("updatePostponedUntilUnixMs") and not s.get("updateSkippedVersion"), f"next restart wrote: {s}"
    assert any("check (startup)" in ln and "available" in ln for ln in install.profile_log_lines("[update]")), \
        "no [update] check (startup) … available line"
    install.end_agentmaster(dest)

    # Tomorrow: the next local 08:00; a postponed copy asks nothing.
    _, pressed = start_and_answer("Remind me tomorrow")
    until = _settings().get("updatePostponedUntilUnixMs", 0)
    expected = _next_local_morning_ms(datetime.fromtimestamp(pressed))
    assert abs(until - expected) <= 120_000, f"tomorrow = {until}, the next 08:00 = {expected}"
    install.end_agentmaster(dest)
    started = time.time()
    install.start_like_explorer(dest / "Agentmaster.exe")
    install.wait_app(dest, timeout_s=120, newer_than=started)
    _no_prompt_for(dest, 20)
    install.end_agentmaster(dest)

    # 3 / 7 / 30 days.
    for days, choice in ((3, "Remind me in 3 days"), (7, "Remind me in 7 days"), (30, "Remind me in 30 days")):
        _set_settings(updatePostponedUntilUnixMs=None)
        _, pressed = start_and_answer(choice)
        until = _settings().get("updatePostponedUntilUnixMs", 0)
        assert abs(until - (pressed * 1000 + days * DAY_MS)) <= 120_000, f"{choice}: {until} for a press at {pressed}"
        install.end_agentmaster(dest)

    # Skip: never offered again; a newer release is.
    _set_settings(updatePostponedUntilUnixMs=None)
    start_and_answer("Skip this version")
    assert _settings().get("updateSkippedVersion") == "v0.8.2", f"skip wrote: {_settings()}"
    install.end_agentmaster(dest)
    started = time.time()
    install.start_like_explorer(dest / "Agentmaster.exe")
    install.wait_app(dest, timeout_s=120, newer_than=started)
    _no_prompt_for(dest, 20)
    install.end_agentmaster(dest)
    fake_github.publish("v0.8.3", {"Agentmaster_0.8.3.0_x64.zip": kit.zip("0.8.3.0")})
    start_and_answer(None, tag="v0.8.3")
    install.end_agentmaster(dest)


@pytest.mark.p1
@pytest.mark.flow("INS-06")
@pytest.mark.install("IS-80")
def test_update_now_from_the_cog(lane, kit, machine, fake_github):
    """⚙ → About → Check for updates, pre-releases on, on a copy stamped older → the prompt → Update now: the app
    closes, the release's zip is downloaded and swapped in, the Apps & Features entry re-stamped, the user's files and
    data kept, and the new version starts.

    Promise (INS-06 phase 5 and E1): one click updates a per-user copy in place — no administrator rights, nothing of
    the user's lost — and a pre-release is offered only with the switch on.

    Args:
        lane: The install lane's facts.
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
        fake_github: The started fake GitHub.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the control, file, entry or request that is wrong.
    """
    dest = _install_user(lane, kit, "0.8.1.0")
    (dest / "mine.txt").write_text("the user's own file\n", encoding="utf-8")
    _set_settings(allowUpdatePrerelease=True)
    zip_name = "Agentmaster_0.8.2.0_x64.zip"
    fake_github.publish("v0.8.2-prerelease", {zip_name: kit.zip("0.8.2.0")}, prerelease=True)

    started = time.time()
    install.start_like_explorer(dest / "Agentmaster.exe")
    _prompt(dest, "v0.8.2-prerelease").press(r"^Postpone$")  # the startup prompt: next restart (the default)
    app = install.wait_app(dest, timeout_s=120, newer_than=started)
    window = installed.InstalledApp(app.pid).window()
    root = installed.open_settings_tab(window, "About")
    toggle = root.wait_find(20, 40, name="Allow updating to pre-release versions")
    assert toggle.toggle_state == 1, f"the pre-release switch reads {toggle.toggle_state} with allowUpdatePrerelease on"
    root.wait_find(20, 40, role="button", name="Check for updates").invoke()
    prompt = _prompt(dest, "v0.8.2-prerelease", timeout_s=60)
    update = prompt.button(r"^Update now$")
    assert update is not None, f"no Update now: {prompt.labels()}"
    clicked = time.time()
    update.element.click()  # a real click: the app quits for the update right after it
    wait_until(lambda: not psutil.pid_exists(app.pid), 60, what="the app to close for the update")
    new = install.wait_app(dest, timeout_s=180, newer_than=clicked)
    took = new.created - clicked
    facts = install.InstallFacts.read(dest)
    # The swap restamps .am-version from the release's TAG (am-update.ps1: the numeric part of v0.8.2-prerelease), over
    # the zip's own "0.8.2.0" — the same version to every comparison (seen 2026-10-24).
    assert facts.version == "0.8.2", f"not updated: {facts.version}"
    assert facts.install_path == "installed" and (dest / "profile.path").exists(), "the decision or profile pointer was lost"
    assert (dest / "mine.txt").exists(), "the user's own file was removed"
    assert not [p.name for p in dest.iterdir() if p.name.lower().startswith(".am-previous-")], "a set-aside folder was left"
    entry = install.MachineState.read().user_entry
    assert entry is not None and str(entry.get("DisplayVersion", "")).startswith("0.8.2"), f"the entry was not re-stamped: {entry}"
    assert (install.default_profile() / "hooks.log").exists(), "the profile's data is gone"
    got = _downloads(fake_github, zip_name)
    assert got and got[-1]["status"] == 200, f"the release's zip was not downloaded: {fake_github.requests()[-10:]}"
    assert any("check (cog) done: v0.8.2-prerelease available (pre-release, installable)" in ln
               for ln in install.profile_log_lines("[update]")), "no [update] check (cog) … available line"
    print(f"IS-80: the new version started {took:.1f} s after the click")


@pytest.mark.p1
@pytest.mark.flow("INS-06")
@pytest.mark.install("IS-87")
def test_a_failed_update_starts_the_old_version_again(lane, kit, machine, fake_github):
    """Update now while a file of the copy cannot be moved (a program holds it open): the swap fails, the old files are
    put back, the update window says why and starts the installed version again, and that run asks nothing (it behaves
    as Remind me next restart).

    Promise (INS-06 phase 6): an update that cannot complete never leaves the user without a working Agentmaster, and
    does not offer itself again at once.

    Args:
        lane: The install lane's facts.
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
        fake_github: The started fake GitHub.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: Naming the message, file or process that is wrong.
    """
    dest = _install_user(lane, kit, "0.8.1.0")
    before = install.InstallFacts.read(dest)
    fake_github.publish("v0.8.2", {"Agentmaster_0.8.2.0_x64.zip": kit.zip("0.8.2.0")})
    handle = _lock_against_moves(dest / LOCKED_NAME)
    try:
        install.start_like_explorer(dest / "Agentmaster.exe")
        prompt = _prompt(dest, "v0.8.2")
        clicked = time.time()
        prompt.button(r"^Update now$").element.click()
        window = _update_window(clicked)
        screen = install.wait_console_text(window, r"Agentmaster starts again in 30 seconds", 300)
        assert "Failed:" in screen, f"the update window does not say why it failed:\n{screen[-1500:]}"
        install.console_type(window, "\r")  # "press any key to start it now"
        install.wait_app(dest, timeout_s=120, newer_than=clicked)  # the installed version, started again
        _no_prompt_for(dest, 20)
    finally:
        _unlock(handle)
    after = install.InstallFacts.read(dest)
    assert after.version == "0.8.1.0", f"the failed update left version {after.version}"
    assert not [n for n in before.names if not (dest / n).exists()], "files of the old version are missing"
    assert not [p.name for p in dest.iterdir() if p.name.lower().startswith(".am-previous-")], "a set-aside folder was left"
    assert not (install.default_profile() / "am-update-restarted.txt").exists(), "the restarted app did not take the marker"
    assert any("the update script started this version again after an update to v0.8.2 that did not complete" in ln
               for ln in install.profile_log_lines("[update]")), "no [update] line for the restarted version"


@pytest.mark.p2
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-11")
def test_a_refused_token_is_dropped(lane, kit, machine, fake_github):
    """`-Version 0.7.10` with a `$env:GITHUB_TOKEN` GitHub refuses, a newer copy installed: the run says the token was
    refused and goes on without it, finds the release under its published tag v0.7.10-prerelease, and is already up to
    date — nothing downloaded.

    Promise (INS-01): a stale token in the environment never blocks an install from a public repository.

    Args:
        lane: The install lane's facts.
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
        fake_github: The started fake GitHub.
    Returns:
        None.
    Raises:
        AssertionError: Naming the message or request that is wrong.
    """
    _install_user(lane, kit, "0.8.4.0")
    zip_name = "Agentmaster_0.7.10.0_x64.zip"
    fake_github.publish("v0.7.10-prerelease", {zip_name: kit.zip("0.7.10.0", zip_name)}, prerelease=True)
    fake_github.refuse_tokens("e2e-revoked-token")
    env = install.clean_env()
    env["GITHUB_TOKEN"] = "e2e-revoked-token"
    run = install.run_installer(lane, "-Scope", "User", "-Version", "0.7.10", env=env)
    out = run.output
    assert "GitHub refused the token" in out and "going on without one" in out, run.tail()
    assert "v0.7.10-prerelease" in out, run.tail()
    assert "Already up to date" in out, run.tail()
    asked = fake_github.requests()
    assert any(r["status"] == 401 and r["token"] for r in asked), f"no refused request: {asked}"
    assert any(r["status"] == 200 and not r["token"] for r in asked), f"no request without the token: {asked}"
    assert not _downloads(fake_github, zip_name), "a zip was downloaded although the copy is up to date"


@pytest.mark.p2
@pytest.mark.flow("INS-01")
@pytest.mark.install("IS-12")
def test_irm_iex_with_a_current_install(lane, kit, machine, fake_github):
    """`irm https://raw.githubusercontent.com/…/Install-Agentmaster.ps1 | iex` with no arguments while the newest
    release is installed just for you: the latest resolved, the installed kind chosen, "Already up to date", nothing
    downloaded.

    Promise (INS-01): the one line a user pastes again later is harmless — it never reinstalls what is current.

    Args:
        lane: The install lane's facts (the staged installer: what raw.githubusercontent.com serves).
        kit: Stamped zips of the build under test.
        machine: The clean machine the test starts from.
        fake_github: The started fake GitHub.
    Returns:
        None.
    Raises:
        AssertionError: Naming the message or request that is wrong.
    """
    _install_user(lane, kit, "0.8.2.0")
    zip_name = "Agentmaster_0.8.2.0_x64.zip"
    fake_github.publish("v0.8.2", {zip_name: kit.zip("0.8.2.0")})
    url = fake_github.serve_raw("/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1", lane.installer)
    run = install.run_powershell(["-Command", f"irm {url} | iex"])
    out = run.output
    assert "Already up to date" in out, run.tail()
    assert any(r["path"].endswith("/releases/latest") for r in fake_github.requests()), "the latest release was not asked for"
    assert not _downloads(fake_github, zip_name), "a zip was downloaded although the copy is current"
