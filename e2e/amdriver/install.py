"""The install lane's driver: a test desktop's Agentmaster installs, read and made like a user makes them.

The install scenarios (doc/agentmaster/flows/19-install-update-profiles.md, "Every install scenario") change the
MACHINE — Program Files, HKCU and HKLM, shortcuts, the PATH, Active Setup — so they run only on a desktop the install
lane prepared (E2E_TESTING.md §8.5): `e2e/guest/provision.ps1 -Lane Install` wrote C:\\e2e\\install-lane.json and
extracted the build under test, `.portable` marker and all, to C:\\e2e\\kit\\build; the host's `install_lane.py` reset
the desktop before the scenario and staged the scripts under test into C:\\e2e\\src\\kit.

What a scenario does with it:
    Lane.load()            the lane's facts (None on any other desktop: the scenarios are then deselected)
    Kit.copy / Kit.zip     a FRESH portable copy, or a release-shaped zip, of the build stamped with a chosen version
                           (the app reads its own version from `.am-version`; tools/install-qa's New-AmQaCopy did this)
    run_installer          tools/Install-Agentmaster.ps1 in Windows PowerShell 5.1 or PowerShell 7, non-interactive
    run_as / start_as      a program as the lane's other account in the signed-in user's session ("std", "adm2")
    run_as_system          a program as SYSTEM in session 0, through a one-shot scheduled task ("sys")
    MachineState.read      every install, registration, shortcut, PATH entry, profile and process, in one snapshot
    start_like_explorer    a double-click: Explorer starts the exe with its own token and environment
    wait_question / ...    the copy's "Where should Agentmaster live?" (ui/taskdialog.py)
    end_agentmaster        the GUI processes ended, their ConPTY hosts left to drain (never OpenConsole killed first)
    collect_evidence       what a failed scenario leaves: the screen, every dialog, the state, the logs, the scripts' runs

⚠ Only for a disposable desktop: nothing here asks before it installs, uninstalls or deletes, and `reset_machine`
removes every Agentmaster install of every account (tools/install-qa/amqa-guest.ps1 `Reset-AmQa`).
"""
from __future__ import annotations

import ctypes
import json
import os
import shutil
import subprocess
import time
import traceback
import uuid
import winreg
import zipfile
from ctypes import wintypes
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

import psutil

from . import win32
from .app import _LEAKY_NAMES, _LEAKY_PREFIXES, MAIN_CLASS
from .ui.dialogs import MessageBox
from .ui.taskdialog import DIALOG_CLASS, TaskDialog
from .waits import WaitTimeout, wait_until

E2E_ROOT = Path(r"C:\e2e")
LANE_FILE = E2E_ROOT / "install-lane.json"
"""Written by provision.ps1 -Lane Install; its presence is what admits an install scenario."""
SRC_KIT = E2E_ROOT / "src" / "kit"
"""The scripts under test, staged by host/install_lane.py (Install-Agentmaster.ps1, amqa-guest.ps1)."""
WINDOWS_POWERSHELL = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
"""Windows PowerShell 5.1 (every Windows 11 has it; the installer's README one-liner runs there)."""
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall"
"""Where Apps & Features reads its entries, under HKCU (a per-user install) and HKLM (the all-users one)."""
USER_ENTRY = "Agentmaster"
"""The per-user install's Apps & Features key name (PortableInstall.h)."""
MACHINE_ENTRY = "AgentmasterAllUsers"
"""The all-users install's key name since the evening of 2026-10-21 (kMachineUninstallKey)."""
OLD_MACHINE_ENTRY = "Agentmaster"
"""The all-users key name before that (read for an old registration)."""
VERB_KEYS = ("Directory", r"Directory\Background", "Drive")
"""The three right-click verbs ("Open in Agentmaster") each install registers: folders, folder backgrounds, drives."""
ACTIVE_SETUP_KEY = r"SOFTWARE\Microsoft\Active Setup\Installed Components\{F8D18BA4-7DD1-4F04-8EE0-C9C7342F3264}"
"""The all-users install's Active Setup component (kActiveSetupKey): StubPath `"<exe>" --register-user`."""
QUESTION = "Where should Agentmaster live?"
"""The portable copy's first-launch question (PortableInstall.h ShowInstallPrompt)."""
PROFILE_PICKER = "Choose a profile for this installation"
"""A kept-portable copy's profile question (ProfileBootstrap.h ShowProfilePicker, first launch)."""
UNINSTALL_CONFIRM = "Uninstall Agentmaster?"
"""`--uninstall-portable`'s confirmation on an install."""
NOTHING_TO_UNINSTALL = "Nothing to uninstall here"
"""`--uninstall-portable`'s answer on a copy that is not an install."""
APP_IMAGES = ("Agentmaster.exe", "WindowsTerminal.exe")
"""The GUI binary and its old-name forwarder (WindowsTerminalShim)."""


# ---- the lane ---------------------------------------------------------------------------------------------------------

@dataclass
class Lane:
    """The install lane's facts, as provision.ps1 wrote them.

    Attributes:
        isolation: "reset" (the host reset the desktop before this scenario), "clean" (one desktop for every scenario:
            the machine fixture removes every install first) or "none" (nothing is removed).
        kit: C:\\e2e\\kit (the build and the copies, stamps and zips made from it).
        build: The pristine portable copy of the build under test (with `.portable`).
        version: The build's own `.am-version` ("0.8.0.0").
        zip: The zip the build came from.
        pwsh: PowerShell 7's exe ("" when the lane installed none).
        accounts: The lane's accounts file ("" when provision.ps1 made no second account: no -Accounts).
        old: The older release as published (C:\\e2e\\kit\\old; "" without -Old).
    """

    isolation: str
    kit: Path
    build: Path
    version: str
    zip: Path
    pwsh: str
    accounts: str = ""
    old: str = ""

    @staticmethod
    def load(path: Path = LANE_FILE) -> Optional["Lane"]:
        """Read the lane file.

        Args:
            path: The file.
        Returns:
            The Lane, or None when the file is missing or unreadable (this desktop is not an install lane's).
        """
        try:
            raw = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            return None
        if raw.get("lane") != "install":
            return None
        return Lane(isolation=str(raw.get("isolation") or "reset").lower(), kit=Path(raw["kit"]),
                    build=Path(raw["build"]), version=str(raw.get("version") or ""), zip=Path(raw.get("zip") or ""),
                    pwsh=str(raw.get("pwsh") or ""), accounts=str(raw.get("accounts") or ""),
                    old=str(raw.get("old") or ""))

    def account(self, kind: str = "std") -> "Account":
        """A second local account the lane made (flows/19's Machine tag of that name).

        Args:
            kind: "std" (the standard account qastd).
        Returns:
            The Account.
        Raises:
            LookupError: When the lane made none (the host provisions -Accounts only for a scenario that needs one).
        """
        if not self.accounts or not Path(self.accounts).exists():
            raise LookupError("the install lane made no second account (provision.ps1 -Accounts)")
        raw = json.loads(Path(self.accounts).read_text(encoding="utf-8-sig"))
        entry = raw.get(kind)
        if not entry:
            raise LookupError(f"the install lane made no {kind!r} account")
        return Account(kind=kind, name=str(entry["name"]), accounts_file=Path(self.accounts))

    @property
    def installer(self) -> Path:
        """tools/Install-Agentmaster.ps1 as staged for this run.

        Returns:
            Its path in the guest.
        """
        return SRC_KIT / "Install-Agentmaster.ps1"

    @property
    def qa_helpers(self) -> Path:
        """tools/install-qa/amqa-guest.ps1 as staged (Get-AmQaState for evidence, Reset-AmQa for the clean isolation).

        Returns:
            Its path in the guest.
        """
        return SRC_KIT / "amqa-guest.ps1"


class Kit:
    """Portable copies and release-shaped zips of the build under test, each stamped with a version of the test's choice.

    The app takes its own version from `.am-version` (PortableInstall.h `CurrentInstallVersion`), and the installer
    takes a zip's from its name (`Agentmaster_<a.b.c.d>_<arch>.zip`) or that file, so a stamp is enough to make "a newer"
    or "an older" copy of one build — the method of the manual QA (tools/install-qa `New-AmQaCopy`).

    Attributes:
        lane: The lane.
        runs: Where copies go (C:\\e2e\\kit\\run).
        zips: Where zips go (C:\\e2e\\kit\\zips).
    """

    def __init__(self, lane: Lane) -> None:
        """Bind to the lane's build.

        Args:
            lane: The lane.
        """
        self.lane = lane
        self.runs = lane.kit / "run"
        self.zips = lane.kit / "zips"

    def copy(self, version: str, name: str) -> Path:
        """A FRESH pristine copy of the build — what a user has right after unzipping a release.

        A copy that decided once (a stub, a kept-portable copy) never asks again, so every scenario step that needs
        the question starts from a copy of its own.

        Args:
            version: The `.am-version` to stamp ("0.8.1.0").
            name: The copy's folder name under C:\\e2e\\kit\\run.
        Returns:
            The copy's Agentmaster.exe.
        Raises:
            OSError: When the copy cannot be made (a process still runs from an old copy of that name).
        """
        dst = self.runs / name
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(self.lane.build, dst)
        (dst / ".am-version").write_text(version + "\n", encoding="ascii")
        # Every account may write it, like an unzip in a user's own folder: a copy another account cannot write is a
        # MACHINE copy to it (Profiles::IsMachineCopyIn — no question at all), which a scenario of the other account
        # must not meet by accident of this folder's ACL (New-AmQaCopy granted the same).
        subprocess.run(["icacls", str(dst), "/grant", "*S-1-1-0:(OI)(CI)M", "/Q"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False, creationflags=subprocess.CREATE_NO_WINDOW)
        return dst / "Agentmaster.exe"

    def zip(self, version: str, name: Optional[str] = None, extra: Optional[dict[str, int]] = None) -> Path:
        """A release-shaped zip of the build: one top folder `agentmaster-<version>` holding the copy, stamped.

        Stored, not deflated: the installer extracts it on the same disk, and a 40 MB deflate costs seconds per zip.

        Args:
            version: The version in the name and in `.am-version` ("0.8.1.0").
            name: The file name (default `Agentmaster_<version>_x64.zip`, the name the installer reads the version from).
            extra: Files added at the layout's root, name -> size in bytes, all zeros and deflated (512 MB cost ~0.5 MB
                in the zip) — e.g. a large file that the installer copies last, which keeps its copy running for a
                while (IS-13's Ctrl+C half way).
        Returns:
            The zip's path.
        Raises:
            OSError: When it cannot be written.
        """
        return self._pack(self.lane.build, version, name or f"Agentmaster_{version}_x64.zip", manifest=None, extra=extra)

    def old_zip(self, manifest: bool, version: str = "", name: Optional[str] = None) -> Path:
        """A release-shaped zip of the OLDER release the lane extracted (provision.ps1 -Old: v0.7.10 by default), with or
        without a release manifest.

        The older release ships no `.am-manifest` and its app knows no `--register-user`: as published it is the
        release an all-users install refuses (IS-24) and a pre-manifest install's uninstall reads through the legacy
        list (IS-95); given a manifest it is "an older app without --register-user" (IS-21–IS-23, the method of the
        2026-10-22 evening QA).

        Args:
            manifest: Add an `.am-manifest` naming the layout's top-level names (what New-UnpackagedTerminalDistribution.ps1
                -PortableMode writes since 2026-10-21).
            version: The version to stamp ("" = the release's own `.am-version`).
            name: The file name (default `Agentmaster_<version>_x64.zip`).
        Returns:
            The zip's path.
        Raises:
            LookupError: When the lane extracted no older release.
            OSError: When the zip cannot be written.
        """
        if not self.lane.old or not (Path(self.lane.old) / "Agentmaster.exe").exists():
            raise LookupError("the install lane extracted no older release (provision.ps1 -Old)")
        source = Path(self.lane.old)
        own = _first_value(source / ".am-version")
        version = version or own or "0.7.10.0"
        names = sorted(p.name for p in source.iterdir() if p.name not in (".e2e-source", ".am-manifest"))
        if manifest:
            names = sorted(set(names) | {".am-manifest"})
        text = ("# Agentmaster portable distribution manifest - the top-level names this build ships.\n" + "\n".join(names)
                + "\n") if manifest else None
        return self._pack(source, version, name or f"Agentmaster_{version}_x64.zip", manifest=text)

    def _pack(self, source: Path, version: str, file_name: str, manifest: Optional[str],
              extra: Optional[dict[str, int]] = None) -> Path:
        """Pack a layout like a release zip: one top folder `agentmaster-<version>`, `.am-version` stamped.

        Args:
            source: The layout folder.
            version: The version (the top folder's name and `.am-version`).
            file_name: The zip's file name.
            manifest: An `.am-manifest` text to add, or None to keep the layout's own (or none).
            extra: Files to add at the layout's root, name -> size in bytes of zeros (deflated, written 1 MB at a time).
        Returns:
            The zip's path.
        Raises:
            OSError: When the zip cannot be written.
        """
        self.zips.mkdir(parents=True, exist_ok=True)
        target = self.zips / file_name
        top = f"agentmaster-{version}"
        with zipfile.ZipFile(target, "w", zipfile.ZIP_STORED) as z:
            for path in sorted(source.rglob("*")):
                rel = path.relative_to(source).as_posix()
                if path.is_dir() or rel == ".e2e-source":
                    continue  # folders come with their files; the lane's own stamp is no part of a release
                if rel == ".am-version":
                    z.writestr(f"{top}/.am-version", version + "\n")
                elif rel == ".am-manifest" and manifest is not None:
                    continue  # replaced below
                else:
                    z.write(path, f"{top}/{rel}")
            if manifest is not None:
                z.writestr(f"{top}/.am-manifest", manifest)
            for leaf, size in (extra or {}).items():
                info = zipfile.ZipInfo(f"{top}/{leaf}", date_time=time.localtime()[:6])
                info.compress_type = zipfile.ZIP_DEFLATED
                chunk = bytes(1 << 20)
                with z.open(info, "w", force_zip64=size >= 1 << 31) as out:
                    for _ in range(size // len(chunk)):
                        out.write(chunk)
                    out.write(bytes(size % len(chunk)))
        return target


# ---- running the installer ----------------------------------------------------------------------------------------

@dataclass
class ScriptRun:
    """One run of a PowerShell script or command.

    Attributes:
        what: The command line shown in reports.
        code: The exit code.
        output: stdout and stderr, merged, as text.
        seconds: How long it took.
    """

    what: str
    code: int
    output: str
    seconds: float

    def tail(self, lines: int = 40) -> str:
        """The output's last lines (for an assertion message).

        Args:
            lines: How many.
        Returns:
            The text.
        """
        return "\n".join(self.output.splitlines()[-lines:])

    def __str__(self) -> str:
        """The run as a report: the command, its exit code and time, and its output's tail.

        Returns:
            The text.
        """
        return f"{self.what}\n  exit {self.code} in {self.seconds:.1f} s\n" + "\n".join(
            "  | " + line for line in self.tail().splitlines())


RUNS: list[ScriptRun] = []
"""Every script run of the current test, in order (the evidence writes them; the machine fixture empties it)."""


def clean_env(extra: Optional[dict] = None, drop: tuple = ()) -> dict:
    """The runner's environment without anything that could point a child at another profile or session.

    Args:
        extra: Variables to add.
        drop: More variable names to remove.
    Returns:
        The environment.
    """
    env = {k: v for k, v in os.environ.items()
           if not k.upper().startswith(_LEAKY_PREFIXES) and k.upper() not in _LEAKY_NAMES and k.upper() not in drop}
    env.update(extra or {})
    return env


def powershell_exe(edition: str, lane: Optional[Lane] = None) -> Path:
    """The PowerShell of an edition.

    Args:
        edition: "5.1" (Windows PowerShell) or "7" (PowerShell 7 of the lane).
        lane: The lane (for "7").
    Returns:
        The exe.
    Raises:
        LookupError: When the lane installed no PowerShell 7.
    """
    if edition == "5.1":
        return WINDOWS_POWERSHELL
    if lane is not None and lane.pwsh and Path(lane.pwsh).exists():
        return Path(lane.pwsh)
    raise LookupError("this install lane has no PowerShell 7 (provision.ps1 -Pwsh)")


def _decode(data: bytes) -> str:
    """A redirected PowerShell's output as text.

    Args:
        data: The bytes (Windows PowerShell writes a redirected stream in the console's code page; the installer is
            pure ASCII, and so are the guest's paths).
    Returns:
        The text.
    """
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("oem", "replace")


def run_powershell(args: list[str], edition: str = "5.1", lane: Optional[Lane] = None, env: Optional[dict] = None,
                   cwd: Optional[Path] = None, timeout_s: float = 900.0) -> ScriptRun:
    """Run PowerShell with arguments, non-interactive, and record the run.

    The run's standard input is empty (DEVNULL), so a question the script reaches takes its default and says which
    (`Test-CanPrompt`), and `-NonInteractive` makes a prompt PowerShell itself would show fail instead of waiting. No
    console window opens (CREATE_NO_WINDOW): the runner has none to share, and a visible one would take the foreground
    from the dialogs a scenario reads.

    Args:
        args: PowerShell's arguments after `-NoProfile -NonInteractive -ExecutionPolicy Bypass`.
        edition: "5.1" or "7".
        lane: The lane (for "7").
        env: The environment (default: clean_env()).
        cwd: The working folder (default C:\\e2e).
        timeout_s: How long it may take.
    Returns:
        The ScriptRun (also appended to RUNS).
    Raises:
        subprocess.TimeoutExpired: When it ran longer (the process is killed first).
        LookupError: For edition "7" without PowerShell 7.
    """
    exe = powershell_exe(edition, lane)
    argv = [str(exe), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", *args]
    started = time.monotonic()
    proc = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          env=env if env is not None else clean_env(), cwd=str(cwd or E2E_ROOT), timeout=timeout_s,
                          creationflags=subprocess.CREATE_NO_WINDOW)
    run = ScriptRun(what=" ".join(f'"{a}"' if " " in a else a for a in argv), code=proc.returncode,
                    output=_decode(proc.stdout), seconds=time.monotonic() - started)
    RUNS.append(run)
    return run


def run_installer(lane: Lane, *args: str, edition: str = "5.1", env: Optional[dict] = None,
                  cwd: Optional[Path] = None, timeout_s: float = 900.0) -> ScriptRun:
    """Run tools/Install-Agentmaster.ps1 as a FILE (`powershell -File …`), the way its README's local form runs it.

    Args:
        lane: The lane (the staged script).
        *args: The script's parameters ("-Scope", "User", "-ZipPath", "…").
        edition: "5.1" or "7".
        env: The environment (default: clean_env()).
        cwd: The working folder (a relative -InstallDir is taken from it).
        timeout_s: How long it may take.
    Returns:
        The ScriptRun.
    """
    return run_powershell(["-File", str(lane.installer), *args], edition, lane, env, cwd, timeout_s)


# ---- another account ----------------------------------------------------------------------------------------------

@dataclass
class Account:
    """A second local account of the lane, started in the signed-in user's session ("Run as different user").

    Attributes:
        kind: The Machine tag ("std": a standard user).
        name: The account name ("qastd").
        accounts_file: The lane's accounts file (its password; readable by administrators only, never printed).
    """

    kind: str
    name: str
    accounts_file: Path

    @property
    def home(self) -> Path:
        """The account's profile folder (made at its first start with -LoadUserProfile).

        Returns:
            C:\\Users\\<name>.
        """
        return Path(os.environ.get("SystemDrive", "C:") + "\\") / "Users" / self.name

    @property
    def profile(self) -> Path:
        """The account's Default Agentmaster profile.

        Returns:
            C:\\Users\\<name>\\.agentmaster.
        """
        return self.home / ".agentmaster"

    @property
    def install_dir(self) -> Path:
        """Where its own per-user install goes.

        Returns:
            C:\\Users\\<name>\\.agentmaster\\bin.
        """
        return self.profile / "bin"


_RUN_AS = r"""
param([string]$AccountsFile, [string]$Kind, [string]$Exe, [string]$ArgLine64, [string]$OutDir, [int]$Wait, [int]$TimeoutS)
# Start a program as the lane's other account in THIS session (Start-Process -Credential: CreateProcessWithLogonW with
# the account's profile loaded). All three standard streams go to files: without that, .NET hands the caller's own
# pipes to the child and a caller that waits on them waits until the child ends (tools/install-qa README).
$ErrorActionPreference = 'Stop'
$acc = (Get-Content -Raw -LiteralPath $AccountsFile | ConvertFrom-Json).$Kind
$cred = New-Object System.Management.Automation.PSCredential ($acc.name, (ConvertTo-SecureString $acc.password -AsPlainText -Force))
# '_none_' stands for no arguments: an empty argument does not survive every command line on the way here, and a bare
# '-' (the first sentinel) is read by -File's binding as a parameter without a name ("Cannot process argument because
# the value of argument "name" is not valid", every start_as of 2026-10-24's first run). '_' is outside base64's alphabet.
$argLine = if ($ArgLine64 -eq '_none_') { '' } else { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ArgLine64)) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$in = Join-Path $OutDir 'stdin.txt'; $out = Join-Path $OutDir 'stdout.txt'; $err = Join-Path $OutDir 'stderr.txt'
Set-Content -LiteralPath $in -Value '' -Encoding ASCII
$sp = @{ FilePath = $Exe; Credential = $cred; LoadUserProfile = $true; PassThru = $true; WorkingDirectory = $OutDir
         RedirectStandardInput = $in; RedirectStandardOutput = $out; RedirectStandardError = $err }
if ($argLine) { $sp.ArgumentList = $argLine }
$p = Start-Process @sp
# Read the handle at once: a Process that Start-Process returns reports no ExitCode later unless its handle was opened
# while the process lived (a known .NET / PowerShell 5.1 trap).
$null = $p.Handle
if ($Wait -eq 0) { 'PID ' + $p.Id; exit 0 }
if (-not $p.WaitForExit($TimeoutS * 1000)) { try { $p.Kill() } catch { }; 'TIMEOUT'; exit 3 }
'EXIT ' + $p.ExitCode
"""


def _quote_arg(arg: str) -> str:
    """One argument quoted the way CommandLineToArgvW splits it back (the rule every C/C++ program's argv follows).

    Args:
        arg: The argument.
    Returns:
        The argument as it goes on a command line.
    """
    if arg and not any(c in arg for c in ' \t"'):
        return arg
    out, backslashes = ['"'], 0
    for c in arg:
        if c == "\\":
            backslashes += 1
            continue
        if c == '"':
            out.append("\\" * (backslashes * 2 + 1) + '"')
        else:
            out.append("\\" * backslashes + c)
        backslashes = 0
    out.append("\\" * (backslashes * 2) + '"')
    return "".join(out)


_RUN_AS_COUNT = [0]
"""How many run-as calls this process made (each gets an output folder of its own)."""


def run_as(account: Account, exe: Path, *args: str, wait: bool = True, timeout_s: float = 600.0) -> ScriptRun:
    """Run a program as the lane's other account in the signed-in user's session — "Run as different user".

    The password never appears on a command line: a small PowerShell (run as the signed-in administrator) reads it
    from the lane's accounts file and makes the credential. The arguments travel base64-encoded and are quoted with
    CommandLineToArgvW's rules, since Start-Process passes one string as the command line as is.

    Args:
        account: The account.
        exe: The program.
        *args: Its arguments.
        wait: Wait for it to end (its exit code and output in the ScriptRun); False returns at once with "PID <n>" as
            the output (a GUI app).
        timeout_s: How long a waited run may take (it is killed after).
    Returns:
        The ScriptRun: code = the program's exit code (3 = timed out; for wait=False the launcher's 0), output = its
        stdout + stderr (or the PID line). Appended to RUNS.
    Raises:
        subprocess.TimeoutExpired: When the launcher itself hangs.
    """
    import base64
    _RUN_AS_COUNT[0] += 1
    out_dir = E2E_ROOT / "w" / "runas" / f"{os.getpid()}-{_RUN_AS_COUNT[0]}"
    script = E2E_ROOT / "w" / "runas" / "run-as.ps1"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(_RUN_AS, encoding="ascii")
    line = " ".join(_quote_arg(a) for a in args)
    launcher = [str(WINDOWS_POWERSHELL), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script),
                "-AccountsFile", str(account.accounts_file), "-Kind", account.kind, "-Exe", str(exe),
                "-ArgLine64", base64.b64encode(line.encode("utf-8")).decode("ascii") or "_none_", "-OutDir", str(out_dir),
                "-Wait", "1" if wait else "0", "-TimeoutS", str(int(timeout_s))]
    started = time.monotonic()
    proc = subprocess.run(launcher, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          env=clean_env(), cwd=str(E2E_ROOT), timeout=timeout_s + 120,
                          creationflags=subprocess.CREATE_NO_WINDOW)
    said = _decode(proc.stdout).strip()
    code = proc.returncode
    last = said.splitlines()[-1] if said else ""
    if wait and last.startswith("EXIT "):
        code = int(last.split()[1])
    elif last == "TIMEOUT":
        code = 3
    output = said
    for name in ("stdout.txt", "stderr.txt"):
        f = out_dir / name
        if f.exists():
            output += f"\n--- {name}\n" + _decode(f.read_bytes())
    run = ScriptRun(what=f"as {account.name}: {exe} {line}", code=code, output=output, seconds=time.monotonic() - started)
    RUNS.append(run)
    return run


def start_as(account: Account, exe: Path, *args: str) -> int:
    """Start a GUI program as the lane's other account in the signed-in user's session, without waiting for it.

    Args:
        account: The account.
        exe: The program (an Agentmaster.exe).
        *args: Its arguments.
    Returns:
        The started process's id.
    Raises:
        RuntimeError: When the start failed (the launcher's output is in the message).
    """
    run = run_as(account, exe, *args, wait=False, timeout_s=120)
    line = next((ln for ln in run.output.splitlines() if ln.startswith("PID ")), None)
    if run.code != 0 or line is None:
        raise RuntimeError(f"could not start {exe} as {account.name}: {run}")
    return int(line.split()[1])


def run_installer_as(account: Account, lane: Lane, *args: str, timeout_s: float = 900.0) -> ScriptRun:
    """Run tools/Install-Agentmaster.ps1 as the lane's other account (Windows PowerShell, as a file, no console input).

    Args:
        account: The account.
        lane: The lane.
        *args: The script's parameters.
        timeout_s: How long it may take.
    Returns:
        The ScriptRun.
    """
    return run_as(account, WINDOWS_POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                  str(lane.installer), *args, timeout_s=timeout_s)


_ACCOUNT_STATE = r"""
# What one account sees of Agentmaster: its own HKCU entries, PATH, icons, Active Setup marker and profile. Run AS that
# account (run_as), so HKCU and the shell folders are its own.
$o = [ordered]@{ user = "$env:USERDOMAIN\$env:USERNAME" }
$k = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster'
$o.user_entry = $null
if (Test-Path $k) { $e = Get-ItemProperty $k; $o.user_entry = [ordered]@{ DisplayName = $e.DisplayName; DisplayVersion = $e.DisplayVersion; InstallLocation = $e.InstallLocation; UninstallString = $e.UninstallString } }
$verbs = [ordered]@{}
foreach ($v in 'Directory', 'Directory\Background', 'Drive') {
    $c = "HKCU:\Software\Classes\$v\shell\Agentmaster\command"
    if (Test-Path -LiteralPath $c) { $verbs[$v] = [string](Get-Item -LiteralPath $c).GetValue('') }
}
$o.user_verbs = $verbs
$o.user_path = @(([string][Environment]::GetEnvironmentVariable('Path', 'User')) -split ';' | Where-Object { $_ -match 'agentmaster' })
$sh = New-Object -ComObject WScript.Shell
$links = @()
foreach ($f in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
    if ($f -and (Test-Path -LiteralPath $f)) {
        Get-ChildItem -LiteralPath $f -Filter 'Agentmaster*.lnk' -ErrorAction SilentlyContinue | ForEach-Object {
            $links += [ordered]@{ path = $_.FullName; target = [string]$sh.CreateShortcut($_.FullName).TargetPath }
        }
    }
}
$o.shortcuts = $links
$m = 'HKCU:\SOFTWARE\Microsoft\Active Setup\Installed Components\{F8D18BA4-7DD1-4F04-8EE0-C9C7342F3264}'
$o.active_setup_user = if (Test-Path $m) { [string](Get-Item $m).GetValue('Version') } else { $null }
$o.profile = Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.agentmaster')
'STATE ' + ($o | ConvertTo-Json -Compress -Depth 5)
"""


def account_state(account: Account) -> dict:
    """What the lane's other account sees of Agentmaster (read AS that account: its HKCU, its shell folders).

    Args:
        account: The account.
    Returns:
        {"user", "user_entry", "user_verbs", "user_path", "shortcuts": [{"path", "target"}], "active_setup_user",
        "profile"}.
    Raises:
        RuntimeError: When the read printed no STATE line (the run's output is in the message).
    """
    script = E2E_ROOT / "w" / "runas" / "account-state.ps1"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(_ACCOUNT_STATE, encoding="ascii")
    run = run_as(account, WINDOWS_POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                 str(script), timeout_s=180)
    RUNS.pop()  # a read, not one of the scenario's runs
    line = next((ln for ln in run.output.splitlines() if ln.startswith("STATE ")), None)
    if line is None:
        raise RuntimeError(f"no state read as {account.name}:\n{run}")
    return json.loads(line[len("STATE "):])


# ---- SYSTEM (the catalog's Machine tag "sys") ------------------------------------------------------------------------

_SYSTEM_TASK = r"""
param([string]$Exe, [string]$ArgLine64, [string]$OutDir, [int]$Wait, [int]$TimeoutS)
# Runs AS SYSTEM, in session 0 (a one-shot scheduled task: run_as_system). The program's three standard streams go to
# files, then its exit code - or, not waited for, its process id - to result.txt: a task's own output goes nowhere.
$result = Join-Path $OutDir 'result.txt'
try {
    $argLine = if ($ArgLine64 -eq '_none_') { '' } else { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ArgLine64)) }
    $in = Join-Path $OutDir 'stdin.txt'
    Set-Content -LiteralPath $in -Value '' -Encoding ASCII
    $sp = @{ FilePath = $Exe; PassThru = $true; WorkingDirectory = $OutDir; RedirectStandardInput = $in
             RedirectStandardOutput = (Join-Path $OutDir 'stdout.txt'); RedirectStandardError = (Join-Path $OutDir 'stderr.txt') }
    if ($argLine) { $sp.ArgumentList = $argLine }
    $p = Start-Process @sp
    # Read the handle at once, or a finished process reports no ExitCode later (as in run-as.ps1).
    $null = $p.Handle
    if ($Wait -eq 0) { Set-Content -LiteralPath $result -Value ('PID ' + $p.Id) -Encoding ASCII; exit 0 }
    if (-not $p.WaitForExit($TimeoutS * 1000)) { try { $p.Kill() } catch { }; Set-Content -LiteralPath $result -Value 'TIMEOUT' -Encoding ASCII; exit 3 }
    Set-Content -LiteralPath $result -Value ('EXIT ' + $p.ExitCode) -Encoding ASCII
}
catch {
    Set-Content -LiteralPath $result -Value ('ERROR ' + $_.Exception.Message) -Encoding UTF8
}
"""

_SYSTEM_REGISTER = r"""
param([string]$TaskName, [string]$TaskArgs64, [string]$OutDir)
# Register and start a one-shot task that runs Windows PowerShell as SYSTEM with the task script's arguments.
$ErrorActionPreference = 'Stop'
$taskArgs = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($TaskArgs64))
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$action = New-ScheduledTaskAction -Execute $ps -Argument $taskArgs -WorkingDirectory $OutDir
$principal = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
'STARTED'
"""

_RUN_AS_SYSTEM_COUNT = [0]
"""How many SYSTEM runs this process made (each gets a task and an output folder of its own)."""


def run_as_system(exe: Path, *args: str, wait: bool = True, timeout_s: float = 600.0) -> ScriptRun:
    """Run a program as SYSTEM in session 0 — the catalog's Machine tag "sys": a one-shot scheduled task, the way a
    management tool, an installer service or a deployment runs one.

    The task is named `amqa-sys-…` (tools/install-qa's Reset-AmQa removes every `amqa-*` task a failed run leaves) and
    is unregistered here once its result is in. A program not waited for keeps running after its task ended.

    Args:
        exe: The program.
        *args: Its arguments (quoted with CommandLineToArgvW's rules, carried base64-encoded like run_as).
        wait: Wait for it to end; False returns once it started, with "PID <n>" as the output.
        timeout_s: How long a waited run may take (it is killed after).
    Returns:
        The ScriptRun: code = the program's exit code (3 = timed out, 4 = the task script failed or never wrote its
        result, for wait=False 0), output = the result line + the program's stdout and stderr. Appended to RUNS.
    Raises:
        RuntimeError: When the task could not be registered or started (its output is in the message).
    """
    import base64
    _RUN_AS_SYSTEM_COUNT[0] += 1
    n = _RUN_AS_SYSTEM_COUNT[0]
    task = f"amqa-sys-{os.getpid()}-{n}"
    out_dir = E2E_ROOT / "w" / "system" / f"{os.getpid()}-{n}"
    out_dir.mkdir(parents=True, exist_ok=True)
    script = E2E_ROOT / "w" / "system" / "system-task.ps1"
    script.write_text(_SYSTEM_TASK, encoding="ascii")
    register = E2E_ROOT / "w" / "system" / "register.ps1"
    register.write_text(_SYSTEM_REGISTER, encoding="ascii")
    line = " ".join(_quote_arg(a) for a in args)
    task_args = " ".join(_quote_arg(a) for a in (
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script), "-Exe", str(exe),
        "-ArgLine64", base64.b64encode(line.encode("utf-8")).decode("ascii") or "_none_", "-OutDir", str(out_dir),
        "-Wait", "1" if wait else "0", "-TimeoutS", str(int(timeout_s))))
    started = time.monotonic()
    reg = run_powershell(["-File", str(register), "-TaskName", task,
                          "-TaskArgs64", base64.b64encode(task_args.encode("utf-8")).decode("ascii"), "-OutDir", str(out_dir)],
                         "5.1", timeout_s=120)
    RUNS.pop()  # the registration is plumbing; the run below is the scenario's
    if reg.code != 0 or "STARTED" not in reg.output:
        raise RuntimeError(f"the SYSTEM task {task} did not start:\n{reg}")
    result_file = out_dir / "result.txt"
    deadline = time.monotonic() + (timeout_s if wait else 120) + 60
    result = ""
    try:
        while time.monotonic() < deadline:
            try:
                result = result_file.read_text(encoding="utf-8-sig").strip()
            except OSError:
                result = ""
            if result:
                break
            time.sleep(1.0)
    finally:
        # The task is not needed once its result is in (or never came): a left task would run again only if started.
        run_powershell(["-Command", f"Unregister-ScheduledTask -TaskName '{task}' -Confirm:$false -ErrorAction SilentlyContinue"],
                       "5.1", timeout_s=60)
        RUNS.pop()
    if result.startswith("EXIT "):
        code = int(result.split()[1])
    elif result.startswith("PID ") and not wait:
        code = 0
    elif result == "TIMEOUT":
        code = 3
    else:
        code = 4
    output = result or "(the task wrote no result)"
    for name in ("stdout.txt", "stderr.txt"):
        f = out_dir / name
        if f.exists():
            output += f"\n--- {name}\n" + _decode(f.read_bytes())
    run = ScriptRun(what=f"as SYSTEM: {exe} {line}", code=code, output=output, seconds=time.monotonic() - started)
    RUNS.append(run)
    return run


def stop_as_system(pid: int) -> ScriptRun:
    """End a process SYSTEM started (an administrator may not hold the right to end a SYSTEM process; SYSTEM does).

    Args:
        pid: Its id.
    Returns:
        The taskkill run (code 128 = no such process: it had ended already).
    """
    taskkill = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "taskkill.exe"
    return run_as_system(taskkill, "/F", "/T", "/PID", str(pid), timeout_s=60)


def process_session(pid: int) -> Optional[int]:
    """The Windows session a process runs in (0 = services, no desktop).

    Args:
        pid: Its id.
    Returns:
        The session id, or None when the process is gone or cannot be asked.
    """
    session = wintypes.DWORD()
    if ctypes.windll.kernel32.ProcessIdToSessionId(wintypes.DWORD(pid), ctypes.byref(session)):
        return int(session.value)
    return None


# ---- a program's console: keys in, screen out --------------------------------------------------------------------

_CONSOLE_HELPER = r'''
"""Type into, or read, ANOTHER process's console (the install lane's console_type / console_text).

Run in a process of its own: it gives up its own console and attaches to the target's (AttachConsole), so neither the
runner's console nor the target's window focus is touched. Keys go in as KEY_EVENT records (WriteConsoleInputW), which
Read-Host and PSReadLine read like typing; the screen comes out of the host's buffer (ReadConsoleOutputCharacterW); a
Ctrl+C is a console signal (GenerateConsoleCtrlEvent), never a key record (a written ^C key is only input).
Usage: python console_helper.py type <pid> <text as hex UTF-16LE> | read <pid>
       | ctrlc <pid> <folder> <name prefix> <timeout s>   (Ctrl+C to the console once the folder holds such an entry)
       | launch <argv as hex UTF-8 JSON>                     (start a program in a new console, Ctrl+C handled; prints
                                                               its process id)
Exit codes: 0 done, 2 cannot attach, 3 no console handle, 4 a write, read or signal failed, 5 the awaited entry never
appeared (no Ctrl+C sent).
"""
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

k = ctypes.WinDLL("kernel32", use_last_error=True)
u = ctypes.WinDLL("user32", use_last_error=True)


class KEY_EVENT_RECORD(ctypes.Structure):
    """One key press or release (wincon.h KEY_EVENT_RECORD)."""
    _fields_ = [("bKeyDown", wintypes.BOOL), ("wRepeatCount", wintypes.WORD), ("wVirtualKeyCode", wintypes.WORD),
                ("wVirtualScanCode", wintypes.WORD), ("uChar", wintypes.WCHAR), ("dwControlKeyState", wintypes.DWORD)]


class _EVENT(ctypes.Union):
    """INPUT_RECORD's event union, padded to its largest member (16 bytes)."""
    _fields_ = [("KeyEvent", KEY_EVENT_RECORD), ("_pad", ctypes.c_byte * 16)]


class INPUT_RECORD(ctypes.Structure):
    """One console input event (EventType 1 = KEY_EVENT)."""
    _fields_ = [("EventType", wintypes.WORD), ("Event", _EVENT)]


class COORD(ctypes.Structure):
    """A character cell position."""
    _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]


class SMALL_RECT(ctypes.Structure):
    """A rectangle of cells (the window inside the buffer)."""
    _fields_ = [("Left", wintypes.SHORT), ("Top", wintypes.SHORT), ("Right", wintypes.SHORT), ("Bottom", wintypes.SHORT)]


class CSBI(ctypes.Structure):
    """CONSOLE_SCREEN_BUFFER_INFO: the buffer size, the cursor, the window."""
    _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD), ("wAttributes", wintypes.WORD),
                ("srWindow", SMALL_RECT), ("dwMaximumWindowSize", COORD)]


k.CreateFileW.restype = wintypes.HANDLE
u.VkKeyScanW.restype = ctypes.c_short  # a SHORT: -1 for a character no key types, else layout key + shift state
u.VkKeyScanW.argtypes = [wintypes.WCHAR]
GENERIC_RW, SHARE_RW, OPEN_EXISTING = 0xC0000000, 3, 3
INVALID = wintypes.HANDLE(-1).value


def console(name):
    """A handle of the attached console's input or screen ("CONIN$" / "CONOUT$"); exits 3 when there is none."""
    h = k.CreateFileW(name, GENERIC_RW, SHARE_RW, None, OPEN_EXISTING, 0, None)
    if not h or h == INVALID:
        sys.exit(3)
    return h


def records(text):
    """The key-down and key-up records that type a text: each character with its layout's virtual key and shift
    state (PSReadLine reads both), CR and LF as Enter (VK_RETURN, "\r")."""
    out = []
    for ch in text:
        if ch in "\r\n":
            vk, shift, ch = 0x0D, 0, "\r"
        else:
            scan = u.VkKeyScanW(ch)
            vk, shift = (scan & 0xFF, (scan >> 8) & 0xFF) if scan != -1 else (0, 0)
        state = 0x10 if shift & 1 else 0  # SHIFT_PRESSED
        sc = u.MapVirtualKeyW(vk, 0) if vk else 0
        for down in (True, False):
            r = INPUT_RECORD()
            r.EventType = 1  # KEY_EVENT
            r.Event.KeyEvent = KEY_EVENT_RECORD(down, 1, vk, sc, ch, state)
            out.append(r)
    return out


def launch(argv_hex):
    """Start a program in a console of its own with Ctrl+C handled normally, and print its process id.

    Windows hands a process's "ignore Ctrl+C" flag on to the processes it starts (a parent started with
    CREATE_NEW_PROCESS_GROUP has it set), so a program started by a runner that ignores Ctrl+C would ignore the one a
    test sends. This helper clears the flag for itself first; the program inherits the cleared flag.
    """
    k.SetConsoleCtrlHandler(None, False)
    argv = json.loads(bytes.fromhex(argv_hex).decode("utf-8"))
    child = subprocess.Popen(argv, creationflags=subprocess.CREATE_NEW_CONSOLE)
    sys.stdout.write(str(child.pid))
    sys.exit(0)


def ctrl_c_when(folder, prefix, timeout_s):
    """(Attached to the target's console.) Wait until the folder holds an entry whose name starts with the prefix,
    then send Ctrl+C to every process of the console; exit 5 when the deadline passes first (nothing sent)."""
    k.SetConsoleCtrlHandler(None, True)  # this helper ignores the Ctrl+C it sends to the whole console
    want = prefix.lower()
    deadline = time.monotonic() + float(timeout_s)
    while True:
        try:
            if any(name.lower().startswith(want) for name in os.listdir(folder)):
                break
        except OSError:
            pass  # the folder can be missing or replaced for a moment during an install: look again
        if time.monotonic() > deadline:
            sys.exit(5)
        time.sleep(0.005)  # a 5 ms look: the signal lands within milliseconds of the entry's creation
    sys.exit(0 if k.GenerateConsoleCtrlEvent(0, 0) else 4)  # CTRL_C_EVENT to every process sharing the console


def attach(pid, seconds):
    """Attach to the target's console, trying again for a while: a process just started in a new window has no console
    to attach to until Windows has handed it to the default terminal (a moment after the start; seen 2026-10-24).
    Exits 2 when it never succeeds, with the last Win32 error on stderr."""
    deadline = time.monotonic() + seconds
    while not k.AttachConsole(pid):
        error = ctypes.get_last_error()
        if time.monotonic() >= deadline:
            sys.stderr.write(f"AttachConsole({pid}) failed: Win32 error {error}\n")
            sys.exit(2)
        time.sleep(0.1)


def main():
    """Run one verb: type the text, print the screen's rows, send a Ctrl+C when an entry appears, or launch."""
    if sys.argv[1] == "launch":
        launch(sys.argv[2])
    verb, pid = sys.argv[1], int(sys.argv[2])
    k.FreeConsole()
    # type and read are polled by their callers (a failed read is ""), so only the one-shot Ctrl+C waits for a console.
    attach(pid, 15.0 if verb == "ctrlc" else 0.0)
    if verb == "ctrlc":
        ctrl_c_when(sys.argv[3], sys.argv[4], sys.argv[5])
    if verb == "type":
        recs = records(bytes.fromhex(sys.argv[3]).decode("utf-16-le"))
        arr = (INPUT_RECORD * len(recs))(*recs)
        written = wintypes.DWORD()
        ok = k.WriteConsoleInputW(console("CONIN$"), arr, len(recs), ctypes.byref(written))
        sys.exit(0 if ok and written.value == len(recs) else 4)
    h = console("CONOUT$")
    info = CSBI()
    if not k.GetConsoleScreenBufferInfo(h, ctypes.byref(info)):
        sys.exit(4)
    width, last = info.dwSize.X, max(info.dwCursorPosition.Y, info.srWindow.Bottom)
    buf, n, out = ctypes.create_unicode_buffer(width), wintypes.DWORD(), []
    for y in range(0, last + 1):
        if not k.ReadConsoleOutputCharacterW(h, buf, width, COORD(0, y), ctypes.byref(n)):
            sys.exit(4)
        row = buf.value[:n.value]
        # A row whose last cell is not blank was filled to the edge: the console wrapped a longer line there (or the
        # line is exactly that long), so the line goes on in the next row and gets no line break. The console API keeps
        # no wrap mark, so a wrap that falls on a space still ends its row; the tests collapse whitespace for that.
        out.append(row if len(row) == width and row[-1:] != " " else row.rstrip() + "\n")
    sys.stdout.buffer.write("".join(out).rstrip().encode("utf-8"))
    sys.exit(0)


main()
'''


def _console_helper_argv(*args: str) -> list[str]:
    """The console helper's command line, its script written first (C:\\e2e\\w\\console_helper.py).

    Args:
        *args: Its verb and arguments.
    Returns:
        The command line's parts (this Python, the script, the arguments).
    Raises:
        OSError: When the script cannot be written.
    """
    import sys
    script = E2E_ROOT / "w" / "console_helper.py"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(_CONSOLE_HELPER, encoding="utf-8")
    return [sys.executable, str(script), *args]


def _console_helper(*args: str, env: Optional[dict] = None, cwd: Optional[Path] = None) -> subprocess.CompletedProcess:
    """Run the console helper (a process of its own: it attaches to the target's console) and wait for it.

    Args:
        *args: Its verb and arguments.
        env: Its environment (default: this process's) — what a program it launches inherits.
        cwd: Its working folder (default: this process's) — likewise.
    Returns:
        The completed process (stdout = what a read or a launch returned).
    """
    # CREATE_NO_WINDOW gives the helper a console of its own without a window, which it frees before it attaches.
    return subprocess.run(_console_helper_argv(*args), stdin=subprocess.DEVNULL, capture_output=True, timeout=60,
                          env=env, cwd=str(cwd) if cwd else None, creationflags=subprocess.CREATE_NO_WINDOW)


def console_type(pid: int, text: str) -> None:
    """Type text into a process's console as key presses ("\\r" or "\\n" = Enter), the way a user types at it — focus
    and window untouched: Read-Host, PSReadLine and the installer's questions read it like the keyboard.

    Args:
        pid: A process attached to the console (a shell, a script's PowerShell).
        text: The text.
    Returns:
        None.
    Raises:
        RuntimeError: When the helper could not attach (the process has no console, or is gone) or the write failed.
    """
    r = _console_helper("type", str(pid), text.encode("utf-16-le").hex())
    if r.returncode != 0:
        raise RuntimeError(f"typing into the console of pid {pid} failed (helper exit {r.returncode}): "
                           f"{r.stderr.decode('utf-8', 'replace')[-400:]}")


def console_text(pid: int) -> str:
    """The text of a process's console screen buffer, from its top to the cursor (or the window's bottom).

    A line longer than the console is wide (the installer's are, in a 120-column tab) wraps into the next row; a row
    filled to the edge is therefore joined to the next one without a line break, so a long path or message reads whole.
    A wrap that falls on a space cannot be told from a line end: match on text with its whitespace collapsed
    (`" ".join(text.split())`) when a phrase may wrap.

    Args:
        pid: A process attached to the console.
    Returns:
        The lines, right-trimmed, joined with newlines; "" when the console cannot be read (the process is gone).
    """
    r = _console_helper("read", str(pid))
    return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else ""


def start_in_console(*argv: str, env: Optional[dict] = None) -> subprocess.Popen:
    """Start a console program in a NEW console window, the way a user opens one (its input is the keyboard: a
    question it asks waits for console_type).

    The test desktops' default terminal is the Store Windows Terminal, so the window is a Windows Terminal tab; its
    console still belongs to the started process, which is what console_type and console_text attach to.

    Args:
        *argv: The program and its arguments.
        env: The environment (default: clean_env()).
    Returns:
        The process.
    Raises:
        OSError: When it cannot start.
    """
    return subprocess.Popen(list(argv), env=env if env is not None else clean_env(), cwd=str(E2E_ROOT),
                            creationflags=subprocess.CREATE_NEW_CONSOLE)


def start_in_console_handling_ctrl_c(*argv: str, env: Optional[dict] = None) -> psutil.Process:
    """Start a console program in a NEW console window, like start_in_console, with Ctrl+C handled normally — for a
    test that sends it one (console_ctrl_c_when).

    Windows hands a process's "ignore Ctrl+C" flag on to the processes it starts, and how the guest's runner was
    started is not this suite's to know; the console helper clears the flag for itself and starts the program, which
    inherits the cleared flag.

    Args:
        *argv: The program and its arguments.
        env: The environment (default: clean_env()).
    Returns:
        The process (not a child of this one: wait for it through psutil).
    Raises:
        RuntimeError: When the helper could not start it.
        psutil.NoSuchProcess: When it ended at once.
    """
    r = _console_helper("launch", json.dumps(list(argv)).encode("utf-8").hex(),
                        env=env if env is not None else clean_env(), cwd=E2E_ROOT)
    if r.returncode != 0 or not r.stdout.strip().isdigit():
        raise RuntimeError(f"the console helper did not start {argv[0]} (exit {r.returncode}): "
                           f"{r.stderr.decode('utf-8', 'replace')[-400:]}")
    return psutil.Process(int(r.stdout.strip()))


def console_ctrl_c_when(pid: int, folder: Path, name_prefix: str, timeout_s: float) -> subprocess.Popen:
    """Press Ctrl+C in a process's console the moment a folder holds an entry whose name starts with a prefix — a
    user's Ctrl+C at an exact point of a run (IS-13: half way through an upgrade's copy). Returns at once: the helper
    attaches to the console, looks at the folder every 5 ms and then sends the signal (GenerateConsoleCtrlEvent) to every
    process of that console.

    Args:
        pid: A process attached to the console (start it with start_in_console_handling_ctrl_c).
        folder: The folder to watch.
        name_prefix: The start of the awaited entry's name (case-insensitive).
        timeout_s: How long the helper waits for the entry.
    Returns:
        The helper's process: its exit code is 0 when the Ctrl+C was sent, 5 when the entry never appeared (nothing
        sent), 2 when it could not attach (the process is gone), 4 when the signal failed.
    Raises:
        OSError: When the helper cannot start.
    """
    return subprocess.Popen(_console_helper_argv("ctrlc", str(pid), str(folder), name_prefix, f"{timeout_s:.0f}"),
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            creationflags=subprocess.CREATE_NO_WINDOW)


# ---- the machine's state ------------------------------------------------------------------------------------------

_KNOWN_FOLDERS = {
    "desktop": "{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}",
    "programs": "{A77F5D77-2E2B-44C3-A6A2-ABA601054A51}",
    "public_desktop": "{C4AA340D-F20F-4863-AFEF-F87EF2E6BA25}",
    "common_programs": "{0139D44E-6AFE-49F2-8690-3DAFCAE6FFB8}",
}
"""The shell folders an install puts icons in (KNOWNFOLDERID values)."""


class _GUID(ctypes.Structure):
    """A GUID for SHGetKnownFolderPath."""

    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


def known_folder(name: str) -> Path:
    """A shell folder of the signed-in user (or the machine's common one), as the app and the installer resolve it.

    Args:
        name: A _KNOWN_FOLDERS key.
    Returns:
        The folder.
    Raises:
        OSError: When the shell cannot resolve it.
    """
    u = uuid.UUID(_KNOWN_FOLDERS[name])
    guid = _GUID.from_buffer_copy(u.bytes_le)
    out = ctypes.c_wchar_p()
    shell32 = ctypes.WinDLL("shell32")
    shell32.SHGetKnownFolderPath.argtypes = [ctypes.POINTER(_GUID), wintypes.DWORD, wintypes.HANDLE,
                                             ctypes.POINTER(ctypes.c_wchar_p)]
    shell32.SHGetKnownFolderPath.restype = ctypes.c_long
    hr = shell32.SHGetKnownFolderPath(ctypes.byref(guid), 0, None, ctypes.byref(out))
    try:
        if hr != 0:
            raise OSError(f"SHGetKnownFolderPath({name}) failed: 0x{hr & 0xFFFFFFFF:08X}")
        return Path(out.value)
    finally:
        ctypes.windll.ole32.CoTaskMemFree(out)


def user_install_dir() -> Path:
    """The per-user install's default folder (the Default profile's `bin`).

    Returns:
        %USERPROFILE%\\.agentmaster\\bin.
    """
    return default_profile() / "bin"


def default_profile() -> Path:
    """The release identity's Default profile of the signed-in user.

    Returns:
        %USERPROFILE%\\.agentmaster.
    """
    return Path(os.environ.get("USERPROFILE", r"C:\Users\Docker")) / ".agentmaster"


def legacy_script_dir() -> Path:
    """Where the OLD install script (Install-Agentmaster.ps1 up to v0.7.10) put its copy: %LOCALAPPDATA%\\Programs\\
    Agentmaster, with a Start-menu entry "Agentmaster (Portable)" but no registry entry and no install.path
    (PortableInstall.h LegacyScriptInstallDir: the app and the current installer still look there).

    Returns:
        The folder.
    """
    return Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))) / "Programs" / "Agentmaster"


def make_shortcut(lnk: Path, target: Path) -> ScriptRun:
    """Make a .lnk through WScript.Shell, the way the old install script made its "Agentmaster (Portable)" entry.

    Args:
        lnk: The shortcut file.
        target: What it opens (its folder is the working folder).
    Returns:
        The PowerShell run (code 0 when the shortcut was saved). Appended to RUNS.
    """
    command = (f"$s = (New-Object -ComObject WScript.Shell).CreateShortcut('{_ps_literal(lnk)}'); "
               f"$s.TargetPath = '{_ps_literal(target)}'; $s.WorkingDirectory = '{_ps_literal(target.parent)}'; $s.Save()")
    return run_powershell(["-Command", command], "5.1", timeout_s=60)


def _ps_literal(value) -> str:
    """A value's text for inside a PowerShell single-quoted string (a quote doubled; nothing else is special there).

    Args:
        value: A path or text.
    Returns:
        The text.
    """
    return str(value).replace("'", "''")


def machine_install_dir() -> Path:
    """The all-users install's default folder.

    Returns:
        %ProgramW6432%\\Agentmaster.
    """
    return Path(os.environ.get("ProgramW6432") or os.environ.get("ProgramFiles", r"C:\Program Files")) / "Agentmaster"


def _read_values(hive, path: str) -> Optional[dict]:
    """Every value of a registry key, 64-bit view.

    Args:
        hive: winreg.HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE.
        path: The key's path.
    Returns:
        {name: value} ("" for the default value), or None when the key does not exist.
    """
    try:
        key = winreg.OpenKey(hive, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    except OSError:
        return None
    values = {}
    with key:
        i = 0
        while True:
            try:
                name, value, _kind = winreg.EnumValue(key, i)
            except OSError:
                break
            values[name] = value
            i += 1
    return values


def _verbs(hive) -> dict:
    """The "Open in Agentmaster" verbs of a hive.

    Args:
        hive: winreg.HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE.
    Returns:
        {"Directory": command, …} for each of VERB_KEYS that is registered.
    """
    found = {}
    for k in VERB_KEYS:
        cmd = _read_values(hive, rf"Software\Classes\{k}\shell\Agentmaster\command")
        if cmd is not None:
            found[k] = cmd.get("", "")
    return found


def _path_entries(hive, path: str) -> list[str]:
    """A PATH value's entries.

    Args:
        hive: The hive.
        path: The Environment key (HKCU\\Environment, or HKLM's Session Manager\\Environment).
    Returns:
        The non-empty entries, as stored (unexpanded).
    """
    values = _read_values(hive, path) or {}
    raw = values.get("Path") or values.get("PATH") or ""
    return [p for p in str(raw).split(";") if p.strip()]


@dataclass
class Shortcut:
    """A .lnk file and what it starts.

    Attributes:
        path: The .lnk.
        target: Its target (TargetPath).
        arguments: Its arguments.
        aumid: The AppUserModelID it carries (System.AppUserModel.ID), "" when none or unreadable.
    """

    path: str
    target: str
    arguments: str
    aumid: str


def read_shortcuts(folders: list[Path], pattern: str = "Agentmaster*.lnk") -> list[Shortcut]:
    """The Agentmaster shortcuts in some folders, read through the Windows Shell (the same COM objects Explorer and the
    installer's own WScript.Shell use).

    Args:
        folders: The folders (missing ones are skipped).
        pattern: The file pattern.
    Returns:
        The shortcuts.
    """
    links = [p for folder in folders if folder.is_dir() for p in sorted(folder.glob(pattern))]
    if not links:
        return []
    import comtypes.client  # late: only a lane run reads shortcuts, and the module is heavy
    shell = comtypes.client.CreateObject("WScript.Shell", dynamic=True)
    try:
        app = comtypes.client.CreateObject("Shell.Application", dynamic=True)
    except Exception:  # noqa: BLE001 - the AppUserModelID is a detail; the target still reads
        app = None
    out = []
    for lnk in links:
        try:
            sc = shell.CreateShortcut(str(lnk))
            target, arguments = str(sc.TargetPath or ""), str(sc.Arguments or "")
        except Exception as error:  # noqa: BLE001 - one unreadable link is reported, the rest still read
            target, arguments = f"(unreadable: {error})", ""
        aumid = ""
        if app is not None:
            try:
                item = app.NameSpace(str(lnk.parent)).ParseName(lnk.name)
                aumid = str(item.ExtendedProperty("System.AppUserModel.ID") or "")
            except Exception:  # noqa: BLE001 - reported as "" (no id), like a link that has none
                aumid = ""
        out.append(Shortcut(str(lnk), target, arguments, aumid))
    return out


@dataclass
class InstallFacts:
    """One folder that holds an Agentmaster.exe.

    Attributes:
        dir: The folder.
        version: Its `.am-version` ("" when it has none).
        install_path: `install.path`'s value line ("" when absent).
        profile_path: `profile.path`'s value line ("" when absent).
        portable: Whether `.portable` is there.
        names: Every top-level name in the folder.
        marked: The files that keep a download mark (a Zone.Identifier stream).
    """

    dir: str
    version: str
    install_path: str
    profile_path: str
    portable: bool
    names: list[str]
    marked: list[str]

    @staticmethod
    def read(folder: Path) -> Optional["InstallFacts"]:
        """Read a folder.

        Args:
            folder: The folder.
        Returns:
            The facts, or None when it holds no Agentmaster.exe.
        """
        if not (folder / "Agentmaster.exe").exists():
            return None
        names = sorted(p.name for p in folder.iterdir())
        marked = [p.name for p in folder.iterdir() if p.is_file() and _has_mark(p)]
        return InstallFacts(str(folder), _first_value(folder / ".am-version"), _first_value(folder / "install.path"),
                            _first_value(folder / "profile.path"), (folder / ".portable").exists(), names, marked)


def _first_value(path: Path) -> str:
    """A decision file's value: its first line that is neither empty nor a `#` comment (the app's own rule).

    Args:
        path: The file.
    Returns:
        The value, or "" when the file is missing or holds none.
    """
    try:
        for line in path.read_text(encoding="utf-8-sig").splitlines():
            if line.strip() and not line.lstrip().startswith("#"):
                return line.strip()
    except OSError:
        pass
    return ""


def _has_mark(path: Path) -> bool:
    """Whether a file keeps the browser's download mark.

    Args:
        path: The file.
    Returns:
        True when its Zone.Identifier stream exists.
    """
    try:
        with open(f"{path}:Zone.Identifier", "rb"):
            return True
    except OSError:
        return False


@dataclass
class AppProcess:
    """A running Agentmaster.exe (or WindowsTerminal.exe).

    Attributes:
        pid: Its id.
        image: Its image name.
        exe: Its image path.
        cmdline: Its command line, joined.
        user: Its account ("" when unreadable).
        created: Its start time (epoch seconds).
    """

    pid: int
    image: str
    exe: str
    cmdline: str
    user: str
    created: float


def _beside_agentmaster(exe: str) -> bool:
    """Whether an image lies in an Agentmaster folder (beside an Agentmaster.exe): Agentmaster's old-name forwarder
    WindowsTerminal.exe and its own OpenConsole.exe hosts do; the real Windows Terminal's do not. The test desktops'
    image runs the real Windows Terminal as its default terminal — it hosts the console of the suite's own runner —
    and on 2026-10-24 the first lane run took it for a leftover Agentmaster.

    Args:
        exe: The process's image path.
    Returns:
        True when an Agentmaster.exe sits beside it; False otherwise, also when the folder cannot be read (WindowsApps).
    """
    if not exe:
        return False
    try:
        return (Path(exe).parent / "Agentmaster.exe").exists()
    except OSError:
        return False


def app_processes(under: Optional[Path] = None) -> list[AppProcess]:
    """The running Agentmaster GUI processes (and old-name forwarders), oldest first.

    Args:
        under: Only those whose image lies in this folder (None = any).
    Returns:
        The processes. A WindowsTerminal.exe counts only when it is Agentmaster's forwarder (_beside_agentmaster): the
        real Windows Terminal is never one of them, so nothing here ends it.
    """
    root = str(under).rstrip("\\").lower() + "\\" if under is not None else None
    found = []
    for p in psutil.process_iter(["name", "exe", "cmdline", "username", "create_time"]):
        try:
            name = p.info["name"] or ""
            if name.lower() not in (n.lower() for n in APP_IMAGES):
                continue
            exe = p.info["exe"] or ""
            if name.lower() == "windowsterminal.exe" and not _beside_agentmaster(exe):
                continue
            if root is not None and not exe.lower().startswith(root):
                continue
            found.append(AppProcess(p.pid, name, exe, " ".join(p.info["cmdline"] or []), p.info["username"] or "",
                                    p.info["create_time"] or 0.0))
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return sorted(found, key=lambda a: a.created)


OTHER_ACCOUNTS = ("qastd", "qaadmin")
"""The second accounts the install QA uses (tools/install-qa/amqa-guest.ps1 `$AmQaAccounts`, provision.ps1 -Accounts)."""


def _other_account_leftovers() -> list[str]:
    """What the lane's other accounts hold of Agentmaster: a profile folder, an Agentmaster icon, and in their own
    registry an Apps & Features entry, the Active Setup marker, a right-click verb or a PATH entry.

    Returns:
        One line per leftover (none for an account that never signed in: it has no folder and no registry).
    """
    out = []
    for name in OTHER_ACCOUNTS:
        home = Path(os.environ.get("SystemDrive", "C:") + "\\") / "Users" / name
        if (home / ".agentmaster").exists():
            out.append(f"{name}'s profile {home / '.agentmaster'}")
        for folder in (home / "Desktop", home / "AppData" / "Roaming" / "Microsoft" / "Windows" / "Start Menu" / "Programs"):
            try:
                out += [f"{name}'s icon {lnk}" for lnk in folder.glob("Agentmaster*.lnk")]
            except OSError:
                continue
        out += _account_registry_leftovers(name, home)
    return out


_PROFILE_LIST = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList"
"""Where Windows maps each SID that ever signed in to its profile folder (ProfileImagePath)."""


def _profile_sid(home: Path) -> Optional[str]:
    """The SID whose profile folder is a given folder (Windows' ProfileList).

    Args:
        home: The account's profile folder (C:\\Users\\<name>).
    Returns:
        The SID text, or None for an account that never signed in (no profile, so no registry to check).
    """
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, _PROFILE_LIST) as root:
            for i in range(winreg.QueryInfoKey(root)[0]):
                sid = winreg.EnumKey(root, i)
                try:
                    with winreg.OpenKey(root, sid) as k:
                        image = str(winreg.QueryValueEx(k, "ProfileImagePath")[0])
                except OSError:
                    continue
                if os.path.normcase(os.path.expandvars(image).rstrip("\\")) == os.path.normcase(str(home).rstrip("\\")):
                    return sid
    except OSError:
        return None
    return None


def _key_exists(path: str) -> bool:
    """Whether a key under HKEY_USERS exists.

    Args:
        path: The key's path below HKEY_USERS.
    Returns:
        True when it opens.
    """
    try:
        with winreg.OpenKey(winreg.HKEY_USERS, path):
            return True
    except OSError:
        return False


def _reg_hive(action: str, *args: str) -> bool:
    """`reg.exe load` / `reg.exe unload` (it enables the backup and restore privileges itself, which winreg's LoadKey
    would need the caller to hold already).

    Args:
        action: "load" or "unload".
        *args: The key below HKU, and for "load" the hive file.
    Returns:
        True when reg.exe succeeded.
    """
    hku = "HKU\\" + args[0]
    r = subprocess.run(["reg.exe", action, hku, *args[1:]], stdin=subprocess.DEVNULL, capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
    return r.returncode == 0


def _account_registry_leftovers(name: str, home: Path) -> list[str]:
    """What one other account's own registry (its HKCU) holds of Agentmaster.

    Its hive is under HKEY_USERS\\<sid> only while it is loaded (signed in, or a process of that account runs); an
    account that ran a scenario a minute ago is usually unloaded again. Its NTUSER.DAT and UsrClass.dat (the second
    hive, HKCU\\Software\\Classes: the verbs) are then loaded under temporary names for the read and unloaded after —
    the way tools/install-qa's Reset-AmQa cleans them (2026-10-24: the clean isolation's first run of the account
    scenarios started one with qastd's registration of a copy whose files were already gone, unseen).

    Args:
        name: The account name.
        home: Its profile folder.
    Returns:
        One line per leftover; a hive that cannot be loaded is one line too (the machine cannot be called clean).
    """
    sid = _profile_sid(home)
    if not sid:
        return []
    hive, classes, mounts = sid, sid + "_Classes", []
    if not _key_exists(sid):
        hive = f"amqa-check-{name}"
        if not _reg_hive("load", hive, str(home / "NTUSER.DAT")):
            return [f"{name}'s registry could not be read (reg load of its NTUSER.DAT failed)"]
        mounts.append(hive)
        classes = f"amqa-check-{name}-classes"
        if _reg_hive("load", classes, str(home / "AppData" / "Local" / "Microsoft" / "Windows" / "UsrClass.dat")):
            mounts.append(classes)
        else:
            classes = None
    out = []
    try:
        for sub, what in ((UNINSTALL_KEY + "\\" + USER_ENTRY, "Apps & Features entry"),
                          (ACTIVE_SETUP_KEY, "Active Setup marker")):
            if _key_exists(hive + "\\" + sub):
                out.append(f"{name}'s {what} (HKCU\\{sub})")
        if classes is None:
            out.append(f"{name}'s right-click verbs could not be read (reg load of its UsrClass.dat failed)")
        else:
            out += [f"{name}'s right-click verb (HKCU\\Software\\Classes\\{v}\\shell\\Agentmaster)" for v in VERB_KEYS
                    if _key_exists(f"{classes}\\{v}\\shell\\Agentmaster")]
        try:
            with winreg.OpenKey(winreg.HKEY_USERS, hive + r"\Environment") as k:
                path = str(winreg.QueryValueEx(k, "Path")[0])
        except OSError:
            path = ""
        out += [f"{name}'s PATH entry {e}" for e in path.split(";") if "agentmaster" in e.lower()]
    finally:
        # Every handle above is closed by its `with`; a hive left loaded would keep the account's next start from
        # loading its profile, so the unload is retried (reg.exe refuses while a handle is still being released).
        for m in reversed(mounts):
            for _ in range(10):
                if _reg_hive("unload", m):
                    break
                time.sleep(0.3)
            else:
                out.append(f"{name}'s hive LEFT LOADED at HKU\\{m} (reg unload failed 10 times)")
    return out


@dataclass
class MachineState:
    """Everything an install leaves on the machine, read in one go (the signed-in user's view, plus the machine's).

    Attributes:
        user: The per-user install in %USERPROFILE%\\.agentmaster\\bin (None when absent).
        machine: The all-users install in Program Files (None when absent).
        user_entry: HKCU Apps & Features `Agentmaster` (None when absent).
        machine_entry: HKLM `AgentmasterAllUsers`, else the old `Agentmaster` (None when absent).
        user_verbs: HKCU "Open in Agentmaster" verbs ({key: command}).
        machine_verbs: HKLM ones.
        user_path: The user PATH entries that mention agentmaster.
        machine_path: The machine PATH entries that mention agentmaster.
        shortcuts: The Agentmaster shortcuts of the user's desktop and Start menu, and the common ones.
        active_setup: The HKLM Active Setup component's values (None when absent).
        active_setup_user: The user's own marker of it, HKCU (None when absent).
        default_profile: Whether %USERPROFILE%\\.agentmaster exists, and its top-level names.
        home_map: Whether %USERPROFILE%\\.agentmaster.profiles exists.
        processes: The running Agentmaster processes.
        other_accounts: What the lane's other accounts hold: their profile folders and Agentmaster icons, and in their
            own registry the Apps & Features entry, the Active Setup marker, the verbs and the PATH (an unloaded hive is
            loaded for the read: _account_registry_leftovers). A leftover there is a leftover too.
        legacy: The copy the old install script left in %LOCALAPPDATA%\\Programs\\Agentmaster (None when absent).
    """

    user: Optional[InstallFacts]
    machine: Optional[InstallFacts]
    user_entry: Optional[dict]
    machine_entry: Optional[dict]
    user_verbs: dict
    machine_verbs: dict
    user_path: list[str]
    machine_path: list[str]
    shortcuts: list[Shortcut]
    active_setup: Optional[dict]
    active_setup_user: Optional[dict]
    default_profile: Optional[list[str]]
    home_map: bool
    processes: list[AppProcess] = field(default_factory=list)
    other_accounts: list[str] = field(default_factory=list)
    legacy: Optional[InstallFacts] = None

    @staticmethod
    def read() -> "MachineState":
        """Read the machine now.

        Returns:
            The snapshot.
        """
        hkcu, hklm = winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE
        machine_entry = (_read_values(hklm, rf"{UNINSTALL_KEY}\{MACHINE_ENTRY}")
                         or _read_values(hklm, rf"{UNINSTALL_KEY}\{OLD_MACHINE_ENTRY}"))
        folders = [known_folder(k) for k in ("desktop", "programs", "public_desktop", "common_programs")]
        profile = default_profile()
        return MachineState(
            user=InstallFacts.read(user_install_dir()),
            machine=InstallFacts.read(machine_install_dir()),
            user_entry=_read_values(hkcu, rf"{UNINSTALL_KEY}\{USER_ENTRY}"),
            machine_entry=machine_entry,
            user_verbs=_verbs(hkcu),
            machine_verbs=_verbs(hklm),
            user_path=[p for p in _path_entries(hkcu, "Environment") if "agentmaster" in p.lower()],
            machine_path=[p for p in _path_entries(hklm, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment")
                          if "agentmaster" in p.lower()],
            shortcuts=read_shortcuts(folders),
            active_setup=_read_values(hklm, ACTIVE_SETUP_KEY),
            active_setup_user=_read_values(hkcu, ACTIVE_SETUP_KEY),
            default_profile=sorted(p.name for p in profile.iterdir()) if profile.is_dir() else None,
            home_map=(profile.parent / ".agentmaster.profiles").exists(),
            processes=app_processes(),
            other_accounts=_other_account_leftovers(),
            legacy=InstallFacts.read(legacy_script_dir()),
        )

    def shortcuts_in(self, which: str) -> list[Shortcut]:
        """The shortcuts of one shell folder.

        Args:
            which: A _KNOWN_FOLDERS key ("desktop", "programs", "public_desktop", "common_programs").
        Returns:
            Those whose .lnk lies directly in it.
        """
        folder = str(known_folder(which)).rstrip("\\").lower()
        return [s for s in self.shortcuts if str(Path(s.path).parent).rstrip("\\").lower() == folder]

    def baseline_problems(self) -> list[str]:
        """What keeps this machine from being a clean one (no Agentmaster anywhere) — the start of every scenario.

        Returns:
            One line per leftover; empty for a clean machine.
        """
        problems = []
        if self.user:
            problems.append(f"a per-user install in {self.user.dir}")
        if self.machine:
            problems.append(f"an all-users install in {self.machine.dir}")
        if self.user_entry is not None:
            problems.append(f"the HKCU Apps & Features entry {self.user_entry.get('InstallLocation')!r}")
        if self.machine_entry is not None:
            problems.append(f"the HKLM Apps & Features entry {self.machine_entry.get('InstallLocation')!r}")
        problems += [f"the HKCU verb {k}" for k in self.user_verbs] + [f"the HKLM verb {k}" for k in self.machine_verbs]
        problems += [f"the user PATH entry {p}" for p in self.user_path]
        problems += [f"the machine PATH entry {p}" for p in self.machine_path]
        problems += [f"the shortcut {s.path}" for s in self.shortcuts]
        if self.active_setup is not None:
            problems.append("the Active Setup component")
        if self.default_profile is not None:
            problems.append(f"the profile {default_profile()} ({len(self.default_profile)} entries)")
        if self.home_map:
            problems.append("the profile choice file %USERPROFILE%\\.agentmaster.profiles")
        problems += [f"a running {p.image} pid {p.pid} ({p.exe})" for p in self.processes]
        problems += self.other_accounts
        if self.legacy:
            problems.append(f"the old install script's copy in {self.legacy.dir}")
        return problems

    def to_json(self) -> str:
        """The snapshot as JSON (evidence).

        Returns:
            The text.
        """
        return json.dumps(asdict(self), indent=1, default=str)


# ---- starting and ending the app ------------------------------------------------------------------------------------

def start_like_explorer(exe: Path, cwd: Optional[Path] = None) -> None:
    """Start an exe the way a double-click does: Explorer starts it, with Explorer's own token and environment.

    `explorer.exe <file>` hands the file to the RUNNING shell and returns; the started process's parent is the desktop's
    Explorer, not this runner, so nothing of the runner's environment (or its console) reaches the app.

    Args:
        exe: The exe.
        cwd: The folder explorer.exe starts in (none needed).
    Returns:
        None.
    Raises:
        OSError: When explorer.exe cannot start.
    """
    subprocess.Popen(["explorer.exe", str(exe)], cwd=str(cwd or exe.parent), env=clean_env(),
                     creationflags=subprocess.CREATE_NO_WINDOW)


def start_direct(exe: Path, *args: str, env: Optional[dict] = None) -> subprocess.Popen:
    """Start an exe with arguments, as a shortcut, a script or the Apps & Features entry does.

    Args:
        exe: The exe.
        *args: Its arguments.
        env: The environment (default: clean_env()).
    Returns:
        The process (its exit code is the forwarder's or the handed-off launch's).
    Raises:
        OSError: When it cannot start.
    """
    return subprocess.Popen([str(exe), *args], cwd=str(exe.parent), env=env if env is not None else clean_env(),
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


REOPEN_PROMPT = r"^Reopen your \d+ previous Agentmaster windows\?"
"""The start-up question of a profile whose last run left more than one window open (WindowEmperor.cpp)."""


def wait_app(under: Path, timeout_s: float = 90.0, newer_than: float = 0.0, window: bool = True,
             image: str = "Agentmaster.exe", answer_reopen: Optional[str] = "No") -> AppProcess:
    """Wait for an Agentmaster to run from a folder (and, by default, to show its main window).

    Two installs of one user share the Default profile, so a copy can start on a profile another copy left with several
    windows open; it then asks "Reopen your N previous Agentmaster windows?" before any window shows (seen 2026-10-24:
    the shared copy started with AGENTMASTER_NO_HANDOFF=1 after the user's own copy had two windows). The wait answers
    that question like a user, by default No (one window).

    Args:
        under: The folder its image must lie in.
        timeout_s: How long.
        newer_than: Only a process started after this epoch time (an earlier instance of the same folder does not count).
        window: Also wait for its CASCADIA_HOSTING_WINDOW_CLASS window (the app is up, past every startup question).
        image: The image name.
        answer_reopen: The button to press on the reopen question ("Yes" / "No"), or None to leave it (the wait then
            times out while the question is up).
    Returns:
        The process.
    Raises:
        WaitTimeout: When none came; the message lists what ran.
    """
    def ready() -> Optional[AppProcess]:
        """The process when it is there (and shows its window); the reopen question answered on the way.

        Returns:
            It, or None.
        """
        for p in app_processes(under):
            if p.image.lower() != image.lower() or p.created < newer_than:
                continue
            if answer_reopen is not None:
                box = MessageBox.find(p.pid, text_re=REOPEN_PROMPT)
                if box is not None:
                    box.press(answer_reopen)
                    continue
            if not window or any(w.cls == MAIN_CLASS and w.visible for w in win32.top_windows(p.pid)):
                return p
        return None

    return wait_until(ready, timeout_s, interval_s=0.5, what=f"{image} from {under}" + (" with its window" if window else ""),
                      describe=lambda: [(p.pid, p.exe) for p in app_processes()])


def end_agentmaster(under: Optional[Path] = None, drain_s: float = 20.0) -> list[str]:
    """End the running Agentmaster GUIs, then whatever their ConPTY hosts leave.

    The GUI goes first and alone: its hosts then end their tabs' shells themselves (CTRL_CLOSE) and exit — a host
    killed first orphans its tab's processes (CLAUDE.md, OBSERVER.md §13a). What is still alive after `drain_s` is
    killed and reported: on a disposable desktop a leftover would break the next step's "is it running" checks.

    Args:
        under: Only the GUIs whose image lies in this folder (None = every Agentmaster).
        drain_s: How long the hosts may take.
    Returns:
        A line per process killed after the drain (empty when the hosts ended by themselves).
    """
    victims = [p.pid for p in app_processes(under)]
    procs = []
    for pid in victims:
        try:
            pr = psutil.Process(pid)
            pr.kill()
            procs.append(pr)
        except psutil.NoSuchProcess:
            continue
    psutil.wait_procs(procs, timeout=10)
    deadline = time.monotonic() + drain_s
    hosts: list[psutil.Process] = []
    while time.monotonic() < deadline:
        # Only Agentmaster's own hosts: the real Windows Terminal's OpenConsole.exe hosts the runner's own console.
        hosts = [h for h in psutil.process_iter(["name", "exe"])
                 if (h.info["name"] or "").lower() == "openconsole.exe" and _beside_agentmaster(h.info["exe"] or "")
                 and (under is None or (h.info["exe"] or "").lower().startswith(str(under).lower()))]
        if not hosts:
            break
        time.sleep(0.5)  # the hosts end their clients first (≤ 5 s per stubborn one), then themselves
    leftovers = []
    for h in hosts:
        try:
            leftovers.append(f"{h.pid} {h.info['exe']}")
            h.kill()
        except psutil.NoSuchProcess:
            continue
    return leftovers


# ---- the dialogs of a first launch ----------------------------------------------------------------------------------

def wait_question(copy_dir: Path, timeout_s: float = 60.0) -> TaskDialog:
    """Wait for a copy's "Where should Agentmaster live?".

    Args:
        copy_dir: The copy's folder (its process's image lies there).
        timeout_s: How long.
    Returns:
        The question.
    """
    return TaskDialog.wait(QUESTION, exe_dirs=[copy_dir], timeout_s=timeout_s)


def no_question_for(copy_dir: Path, seconds: float) -> bool:
    """Whether a copy shows no install question for a while (a decided copy never asks again).

    Args:
        copy_dir: The copy's folder.
        seconds: How long to watch.
    Returns:
        True when no question appeared.
    """
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if TaskDialog.find(QUESTION, exe_dirs=[copy_dir]) is not None:
            return False
        time.sleep(0.3)  # a watch for something that must NOT appear: every pass is a fresh look
    return True


def install_log_lines(profile: Optional[Path] = None) -> list[str]:
    """The `[install]` lines of a profile's hooks.log (the first launch and the uninstall log there).

    Args:
        profile: The profile folder (default: the Default profile).
    Returns:
        The lines, oldest first.
    """
    return profile_log_lines("[install]", profile)


def profile_log_lines(tag: str, profile: Optional[Path] = None) -> list[str]:
    """The lines of a profile's hooks.log that carry one tag (`[install]`, `[update]`, …).

    Args:
        tag: The tag, brackets included.
        profile: The profile folder (default: the Default profile).
    Returns:
        The lines, oldest first (none when the log is missing).
    """
    log = (profile or default_profile()) / "hooks.log"
    try:
        text = win32.read_bytes_shared(log).decode("utf-8", "replace")
    except OSError:
        return []
    return [line for line in text.splitlines() if tag in line]


def log_time_s(line: str) -> Optional[float]:
    """The seconds of the day a hooks.log line's `[HH:MM:SS.mmm]` stamp says (the app's local time).

    Args:
        line: The line.
    Returns:
        The seconds since midnight, or None when the line has no stamp.
    """
    import re
    m = re.match(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]", line)
    if not m:
        return None
    h, mi, s, ms = (int(g) for g in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


# ---- clean-up and evidence --------------------------------------------------------------------------------------

def reset_machine(lane: Lane) -> ScriptRun:
    """Remove every Agentmaster install, registration, shortcut, PATH entry, profile and process of the machine
    (tools/install-qa/amqa-guest.ps1 `Reset-AmQa` — the manual QA's own clean-up), for the "clean" isolation.

    Args:
        lane: The lane (the staged helpers).
    Returns:
        The run (one line per removed item).
    """
    return run_powershell(["-Command", f". '{lane.qa_helpers}'; Reset-AmQa"], "5.1", lane, timeout_s=300)


def collect_evidence(out_dir: Path, label: str = "", lane: Optional[Lane] = None) -> Path:
    """Write what a failed scenario leaves behind.

    Files (each best effort; a failing part writes its error into evidence-errors.txt):
        screen.png      the desktop
        dialogs.txt     every open Win32 dialog of an Agentmaster process, as UI Automation sees it
        state.json      MachineState.read()
        amqa-state.txt  tools/install-qa's Get-AmQaState (every account's installs, entries, verbs, links, processes)
        runs.txt        every script run of the test: command, exit code, output
        install.log     the `[install]` lines of the Default profile's hooks.log
        installer\\      %TEMP%\\Agentmaster-install\\*.log (the installer's own records) and the app's update logs

    Args:
        out_dir: The folder (created).
        label: A header line.
        lane: The lane (for Get-AmQaState); None skips it.
    Returns:
        The folder.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    errors = []

    def part(name: str, fn) -> None:
        """Run one collector; record its failure instead of raising.

        Args:
            name: The collector's name.
            fn: The collector.
        Returns:
            None.
        """
        try:
            fn()
        except Exception:  # noqa: BLE001 - evidence is best effort; the error is kept beside it
            errors.append(f"--- {name}\n{traceback.format_exc()}")

    def screen() -> None:
        """The desktop's screenshot."""
        from PIL import ImageGrab
        ImageGrab.grab(all_screens=True).save(out_dir / "screen.png")

    def dialogs() -> None:
        """Every open dialog of an Agentmaster process."""
        pids = [p.pid for p in app_processes()]
        parts = [f"===== {d!r}\n{d.dump()}" for d in TaskDialog.all(pids=pids, title=None)] if pids else []
        (out_dir / "dialogs.txt").write_text("\n\n".join(parts) or "(none)", encoding="utf-8")

    def state() -> None:
        """The machine's state."""
        (out_dir / "state.json").write_text(MachineState.read().to_json(), encoding="utf-8")

    def amqa() -> None:
        """The QA kit's own report."""
        if lane is None:
            return
        run = run_powershell(["-Command", f". '{lane.qa_helpers}'; Get-AmQaState"], "5.1", lane, timeout_s=120)
        RUNS.pop()  # the report is evidence, not one of the test's runs
        (out_dir / "amqa-state.txt").write_text(run.output, encoding="utf-8")

    def runs() -> None:
        """The test's script runs."""
        (out_dir / "runs.txt").write_text("\n\n".join(str(r) + "\n--- full output ---\n" + r.output for r in RUNS)
                                          or "(none)", encoding="utf-8")

    def logs() -> None:
        """The install lines and the installer's records."""
        (out_dir / "install.log").write_text("\n".join(install_log_lines()) or "(none)", encoding="utf-8")
        temp = Path(os.environ.get("TEMP", r"C:\Windows\Temp"))
        dst = out_dir / "installer"
        for folder in ("Agentmaster-install", "Agentmaster-update"):
            src = temp / folder
            if src.is_dir():
                for f in src.glob("*.log"):
                    dst.mkdir(exist_ok=True)
                    shutil.copy2(f, dst / f"{folder}-{f.name}")

    for name, fn in (("screen", screen), ("dialogs", dialogs), ("state", state), ("amqa", amqa), ("runs", runs),
                     ("logs", logs)):
        part(name, fn)
    if errors or label:
        (out_dir / "evidence-errors.txt").write_text((f"evidence for {label}\n" if label else "") + "\n".join(errors),
                                                     encoding="utf-8")
    return out_dir


__all__ = ["Lane", "Kit", "ScriptRun", "RUNS", "MachineState", "InstallFacts", "Shortcut", "AppProcess", "run_installer",
           "Account", "run_as", "start_as", "run_installer_as", "account_state", "OTHER_ACCOUNTS", "REOPEN_PROMPT",
           "run_powershell", "clean_env", "start_like_explorer", "start_direct", "wait_app", "end_agentmaster",
           "wait_question", "no_question_for", "install_log_lines", "reset_machine", "collect_evidence", "user_install_dir",
           "machine_install_dir", "default_profile", "known_folder", "app_processes", "QUESTION", "PROFILE_PICKER",
           "UNINSTALL_CONFIRM", "NOTHING_TO_UNINSTALL", "DIALOG_CLASS", "WaitTimeout"]
