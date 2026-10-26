"""Makes and checks the build server's Windows images (DESIGN.md §4), through the coordinator in the site.

Every image is made by a request to the coordinator, so its VM is admitted like any other slot (the reserves of
DESIGN.md §3.2 keep the owner's PC usable while it installs) and its steps are logged like any other request:

    python buildserver/image/images.py base   [--name am-base] [--version core11]   # dockur's unattended install
    python buildserver/image/images.py smoke  [--image am-base]                     # boot, `cmd /c ver`, shut down
    python buildserver/image/images.py e2e    [--base am-base] [--name am-e2e]      # the e2e layer (Python + provision)
    python buildserver/image/images.py build-image [--base am-base] [--name am-build]   # the build layer (Build Tools)
    python buildserver/image/images.py check  [--image am-e2e]                      # boot it and check what it holds
    python buildserver/image/images.py list                                         # the images in the site

The base holds only the guest agent's bootstrap (image/oem/: install.bat → setup.ps1 → bootstrap.ps1 at every logon);
layers add what their slot kind needs (E4). The coordinator is started when it does not answer (it exits by itself after
10 idle minutes), as `amb` does; its admin token is read from the site and never printed.

Exit codes: 0 the request succeeded; 1 it ended otherwise (its outcome is printed); 2 usage or no coordinator.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "buildserver" / "site"))
import sitectl  # noqa: E402  (the site's helpers: the admin token, commands in the distro)

BASE_URL = "http://127.0.0.1:2207"
"""The coordinator as Windows reaches it (it listens on IPv4, which WSL's localhost forwarding passes)."""

PYTHON_URL = "https://www.python.org/ftp/python/3.13.9/python-3.13.9-amd64.exe"
"""The CPython the e2e layer installs (provision.ps1 expects 3.13 at C:\\Program Files\\Python313)."""

FINAL = ("Succeeded", "Failed", "Cancelled", "NoCapacity", "Lost")
"""The request states that end a request."""

DOWNLOAD_CACHE = Path(r"Q:\buildserver\cache\dl")
"""Pinned downloads kept on this PC for image builds (PowerShell 7, PortableGit): staged into a layer slot's share when
present, so a rebuilt image does not fetch them again; the steps check each file's SHA-256 either way."""

GIB = 1024
"""MiB per GiB (the coordinator's resources are in MiB)."""


def ensure_coordinator() -> None:
    """Starts the coordinator in the site unless it answers, as `amb` does.

    The coordinator exits by itself after 10 minutes with nothing to do (the site is lazy, DESIGN.md §5.2), so a
    script that runs after a pause would otherwise meet "connection refused".

    Raises:
        sitectl.SiteError: it did not start (its log's last lines are in the message).
    """
    try:
        with urllib.request.urlopen(BASE_URL + "/healthz", timeout=5) as response:
            if response.status == 200:
                return
    except OSError:
        pass  # Expected when it idled out: start it below.
    sitectl.up()


class Coordinator:
    """A minimal client of the coordinator's API (`amb` is the full one; this keeps the Python tools standalone)."""

    def __init__(self, token: str) -> None:
        """Creates a client.

        Args:
            token: the admin token.
        """
        self._token = token

    def call(self, method: str, path: str, body: object | None = None, timeout: float = 60) -> tuple[int, object]:
        """Sends one call.

        Args:
            method: the HTTP method.
            path: the path under the coordinator's URL.
            body: a JSON body, or None.
            timeout: seconds before the call is abandoned.

        Returns:
            (status, parsed JSON or text).
        """
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(BASE_URL + path, data=data, method=method)
        request.add_header("Authorization", "Bearer " + self._token)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                raw, status = response.read(), response.status
        except urllib.error.HTTPError as exc:
            raw, status = exc.read(), exc.code
        try:
            return status, json.loads(raw) if raw else None
        except json.JSONDecodeError:
            return status, raw.decode("utf-8", "replace")

    def submit_and_follow(self, body: dict) -> dict:
        """Submits a request and prints its events until it ends.

        Args:
            body: the CreateRequest body.

        Returns:
            The final request record.

        Raises:
            RuntimeError: the coordinator refused the request.
        """
        status, created = self.call("POST", "/api/v1/requests", body)
        if status != 202:
            raise RuntimeError(f"refused ({status}): {created}")
        request_id = created["id"]
        print(f"request {request_id} accepted ({body['kind']})", flush=True)
        skip = 0
        while True:
            try:
                status, page = self.call("GET", f"/api/v1/requests/{request_id}/events?skip={skip}&wait=25", timeout=45)
            except OSError as exc:
                # The coordinator restarting or a slow answer: ask again (the request goes on regardless).
                print(f"  (events: {exc}; asking again)", flush=True)
                time.sleep(2)
                continue
            if status != 200:
                raise RuntimeError(f"events: HTTP {status}: {page}")
            for event in page["events"]:
                print(f"  {event['at'][11:19]}  {event['kind']:<9} {event['message']}", flush=True)
            skip = page["next"]
            if page["request"]["state"] in FINAL:
                return page["request"]


def e2e_layer_steps() -> list[dict]:
    """The steps that make the e2e layer over the base (DESIGN.md §4.2: Python with the e2e runner's venv, PowerShell 7,
    PortableGit, Claude Code, the leak guard, crash dumps - what e2e/guest/provision.ps1 installs after every reset of a
    claude-fleet desktop, done once here).

    Returns:
        The guest steps.
    """
    return [
        {"id": "python-download", "kind": "run", "command": "powershell.exe", "timeoutS": 900,
         "args": ["-NoProfile", "-Command",
                  "$ProgressPreference='SilentlyContinue'; New-Item -ItemType Directory -Force C:\\amb\\dl | Out-Null; "
                  f"Invoke-WebRequest -UseBasicParsing -Uri '{PYTHON_URL}' -OutFile C:\\amb\\dl\\python.exe"]},
        {"id": "python-install", "kind": "run", "command": "C:\\amb\\dl\\python.exe", "timeoutS": 1800,
         "args": ["/quiet", "InstallAllUsers=1", "PrependPath=0", "Include_test=0", "Include_launcher=0",
                  "TargetDir=C:\\Program Files\\Python313"]},
        # provision.ps1 (Standard lane, PowerShell 7 too) installs and caches everything but the build under test; its
        # default build is the v0.8.0 release, which each run replaces with its own (-AgentmasterZip).
        {"id": "provision", "kind": "run", "command": "powershell.exe", "timeoutS": 3600,
         "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "Z:\\e2e\\src\\guest\\provision.ps1", "-Pwsh",
                  "-Cache", "C:\\e2e\\cache"]},
        {"id": "trims", "kind": "run", "command": "powershell.exe", "timeoutS": 600,
         "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "Z:\\image\\steps\\trims.ps1"]},
        {"id": "cleanup", "kind": "run", "command": "powershell.exe", "timeoutS": 600,
         "args": ["-NoProfile", "-Command", "Remove-Item -Recurse -Force C:\\amb\\dl -ErrorAction SilentlyContinue; "
                  "Remove-Item -Recurse -Force C:\\amb\\steps -ErrorAction SilentlyContinue; exit 0"]},
    ]


def e2e_check_steps() -> list[dict]:
    """The checks of the e2e layer: every tool provision.ps1 installed answers from where the suite expects it, and the
    hosts guard that keeps a stray call off the real API is in place. Each check is one step; its output is logged on
    the request, and the first check that fails (a non-zero exit) fails the request with its name.

    Returns:
        The guest steps.
    """
    return [
        {"id": "venv", "kind": "run", "command": "C:\\e2e\\venv\\Scripts\\python.exe", "timeoutS": 120,
         "args": ["-c", "import sys, pytest, comtypes, psutil, PIL; "
                        "print('python', sys.version.split()[0], 'pytest', pytest.__version__)"]},
        # cmd expands %USERPROFILE% (the agent runs as the logged-on user, Docker).
        {"id": "claude", "kind": "run", "command": "cmd.exe", "timeoutS": 120,
         "args": ["/c", "%USERPROFILE%\\.local\\bin\\claude.exe --version"]},
        {"id": "git", "kind": "run", "command": "C:\\e2e\\git\\bin\\bash.exe", "timeoutS": 60, "args": ["--version"]},
        {"id": "pwsh", "kind": "run", "command": "C:\\e2e\\pwsh\\pwsh.exe", "timeoutS": 120,
         "args": ["-NoProfile", "-Command", "'PowerShell ' + $PSVersionTable.PSVersion.ToString()"]},
        {"id": "agentmaster", "kind": "run", "command": "cmd.exe", "timeoutS": 60,
         "args": ["/c", "type C:\\e2e\\am\\.am-version"]},
        {"id": "hosts", "kind": "run", "command": "cmd.exe", "timeoutS": 60,
         "args": ["/c", "findstr /i /c:api.anthropic.com C:\\Windows\\System32\\drivers\\etc\\hosts"]},
        # The suite was written on 1600x900 claude-fleet desktops; the slot driver asks the VM for the same size.
        {"id": "screen", "kind": "run", "command": "powershell.exe", "timeoutS": 120,
         "args": ["-NoProfile", "-Command",
                  "Add-Type -AssemblyName System.Windows.Forms; $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "
                  "'screen {0}x{1}' -f $b.Width, $b.Height; if ($b.Width -ne 1600 -or $b.Height -ne 900) { exit 1 }"]},
    ]


def build_layer_steps() -> list[dict]:
    """The steps that make the build layer over the base (DESIGN.md §4.2: VS 2022 Build Tools with the C++ and UWP
    workloads, both SDKs and vcpkg; PowerShell 7, Git, the VSSetup module, long paths; the trims; a seal with Windows'
    servicing idle). Each step is a script in image/steps (staged into the slot's share), so the request's events time
    and report each part.

    Returns:
        The guest steps.
    """
    def script(step_id: str, name: str, timeout_s: int) -> dict:
        """One step that runs a staged PowerShell script.

        Args:
            step_id: the step's id (its events are named by it).
            name: the script's file name in image/steps.
            timeout_s: the step's timeout.

        Returns:
            The guest step.
        """
        return {"id": step_id, "kind": "run", "command": "powershell.exe", "timeoutS": timeout_s,
                "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", f"Z:\\image\\steps\\{name}"]}

    return [
        # The install took 28 min with its 4.8 GB download (measured); 90 min covers a slow download.
        script("buildtools", "install-buildtools.ps1", 5400),
        script("tools", "install-tools.ps1", 1200),
        script("trims", "trims.ps1", 600),
        # The seal waits up to 20 min for servicing to go idle, in 30-s samples.
        script("seal", "seal.ps1", 1800),
        {"id": "cleanup", "kind": "run", "command": "powershell.exe", "timeoutS": 600,
         "args": ["-NoProfile", "-Command", "Remove-Item -Recurse -Force C:\\amb\\dl -ErrorAction SilentlyContinue; "
                  "Remove-Item -Recurse -Force C:\\amb\\steps -ErrorAction SilentlyContinue; exit 0"]},
    ]


def build_check_steps() -> list[dict]:
    """The checks of the build layer: every tool a build runs answers from where the build looks for it, Build Tools
    is found the way tools\\OpenConsole.psm1 finds it (VSSetup), and Windows' servicing stays idle after a boot (a busy
    TiWorker would take a vCPU from every build).

    Returns:
        The guest steps.
    """
    # A literal path: the checks quote their paths in single quotes, where PowerShell expands no variable.
    kits = "C:\\Program Files (x86)\\Windows Kits\\10"
    return [
        {"id": "msbuild", "kind": "run", "command": "cmd.exe", "timeoutS": 120,
         "args": ["/c", "dir /b /s C:\\BuildTools\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe"]},
        {"id": "cl", "kind": "run", "command": "powershell.exe", "timeoutS": 120,
         "args": ["-NoProfile", "-Command",
                  "$c = Get-ChildItem C:\\BuildTools\\VC\\Tools\\MSVC -Recurse -Filter cl.exe | "
                  "Where-Object FullName -like '*Hostx64\\x64*' | Select-Object -First 1; "
                  "if (-not $c) { exit 1 }; $c.FullName"]},
        {"id": "sdks", "kind": "run", "command": "powershell.exe", "timeoutS": 120,
         "args": ["-NoProfile", "-Command",
                  f"$f = @('{kits}\\Include\\10.0.22621.0\\um\\windows.h', '{kits}\\Include\\10.0.26100.0\\um\\windows.h', "
                  f"'{kits}\\bin\\10.0.22621.0\\x64\\MakeAppx.exe', 'C:\\BuildTools\\VC\\vcpkg\\vcpkg.exe'); "
                  "$f | ForEach-Object { '{0} {1}' -f (Test-Path $_), $_ }; "
                  "if (@($f | Where-Object { -not (Test-Path $_) }).Count) { exit 1 }"]},
        {"id": "pwsh", "kind": "run", "command": "C:\\Program Files\\PowerShell\\7\\pwsh.exe", "timeoutS": 120,
         "args": ["-NoProfile", "-Command", "'PowerShell ' + $PSVersionTable.PSVersion.ToString()"]},
        {"id": "git", "kind": "run", "command": "C:\\Program Files\\Git\\cmd\\git.exe", "timeoutS": 60, "args": ["--version"]},
        {"id": "vssetup", "kind": "run", "command": "powershell.exe", "timeoutS": 120,
         "args": ["-NoProfile", "-Command",
                  "Import-Module (Get-ChildItem C:\\BuildServer\\psmodules\\VSSetup -Recurse -Filter VSSetup.psd1 | "
                  "Select-Object -First 1).FullName; $i = Get-VSSetupInstance -All | Select-VSSetupInstance -Latest "
                  "-Product * -Require 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'; if (-not $i) { exit 1 }; "
                  "'{0} {1}' -f $i.InstallationPath, $i.InstallationVersion"]},
        {"id": "servicing", "kind": "run", "command": "powershell.exe", "timeoutS": 300,
         "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "Z:\\image\\steps\\seal.ps1", "-MaxMinutes", "1"]},
    ]


def main(argv: list[str]) -> int:
    """Parses the command line and runs one command.

    Args:
        argv: the arguments after the program name.

    Returns:
        The exit code.
    """
    parser = argparse.ArgumentParser(description="The build server's Windows images.")
    sub = parser.add_subparsers(dest="command", required=True)
    base = sub.add_parser("base", help="install a base image (dockur's unattended install + the bootstrap)")
    base.add_argument("--name", default="am-base")
    base.add_argument("--version", default="core11", help="dockur's VERSION (core11 = Tiny11 Core, tiny11 = Tiny11)")
    smoke = sub.add_parser("smoke", help="boot an image, run `cmd /c ver`, shut it down")
    smoke.add_argument("--image", default="am-base")
    e2e = sub.add_parser("e2e", help="make the e2e layer")
    e2e.add_argument("--base", default="am-base")
    e2e.add_argument("--name", default="am-e2e")
    build = sub.add_parser("build-image", help="make the build layer (VS 2022 Build Tools, PowerShell 7, Git)")
    build.add_argument("--base", default="am-base")
    build.add_argument("--name", default="am-build")
    check = sub.add_parser("check", help="boot an image and check what it holds (a layer's tools; else `ver`)")
    check.add_argument("--image", default="am-e2e")
    sub.add_parser("list", help="list the images in the site")
    args = parser.parse_args(argv)
    if args.command == "list":
        out = sitectl.in_distro("for d in /srv/buildserver/images/*/; do [ -d \"$d\" ] || continue; "
                                "printf '%s %s\\n' \"$(basename \"$d\")\" \"$(du -sh \"$d\" | cut -f1)\"; "
                                "qemu-img info \"$d/data.qcow2\" 2>/dev/null | grep -E 'backing file:|disk size:' | sed 's/^/    /'; "
                                "done\n", timeout=60).stdout.decode()
        print(out or "no images")
        return 0
    try:
        ensure_coordinator()
        client = Coordinator(sitectl.admin_token())
    except sitectl.SiteError as exc:
        print(f"images: {exc}", file=sys.stderr)
        return 2
    if args.command == "base":
        body = {"kind": "Install", "inputs": {"newImage": args.name, "version": args.version,
                                              "oemDir": str(HERE / "oem")}}
    elif args.command == "smoke":
        body = {"kind": "Smoke", "inputs": {"image": args.image}}
    elif args.command == "check":
        # A smoke run with the image kind's own checks as its steps (an image without checks gets the smoke's `ver`).
        inputs: dict[str, object] = {"image": args.image}
        if args.image.startswith("am-e2e"):
            inputs["steps"] = e2e_check_steps()
        elif args.image.startswith("am-build"):
            # The servicing check runs the layer's own seal script, so the steps folder goes into the share too.
            inputs["steps"] = build_check_steps()
            inputs["stage"] = [{"source": str(HERE / "steps"), "target": "image/steps"}]
        body = {"kind": "Smoke", "inputs": inputs}
    elif args.command == "build-image":
        stage = [{"source": str(HERE / "steps"), "target": "image/steps"},
                 {"source": str(REPO / ".PowershellModules" / "VSSetup"), "target": "image/psmodules/VSSetup"}]
        if DOWNLOAD_CACHE.is_dir():
            stage.append({"source": str(DOWNLOAD_CACHE), "target": "image/dl"})
        body = {"kind": "Layer",
                "inputs": {"image": args.base, "newImage": args.name, "steps": build_layer_steps(), "stage": stage},
                # The layer grows ~18.5 GB (measured), plus the downloads while it installs; the install likes vCPUs.
                "resources": {"min": {"memoryMiB": 4 * GIB, "vcpus": 2, "diskMiB": 24 * GIB},
                              "preferred": {"memoryMiB": 8 * GIB, "vcpus": 8, "diskMiB": 28 * GIB}},
                # Build Tools (up to 90 min with a slow download) + the tools + a seal of up to 30 min.
                "runTimeoutMinutes": 180}
    else:
        body = {"kind": "Layer", "inputs": {
            "image": args.base, "newImage": args.name, "steps": e2e_layer_steps(),
            "stage": [{"source": str(REPO / "e2e"), "target": "e2e/src"},
                      {"source": str(HERE / "steps"), "target": "image/steps"}]}}
    final = client.submit_and_follow(body)
    print(f"{final['state']}: {final.get('outcome')}")
    return 0 if final["state"] == "Succeeded" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
