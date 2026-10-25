"""Makes and checks the build server's Windows images (DESIGN.md §4), through the coordinator in the site.

Every image is made by a request to the coordinator, so its VM is admitted like any other slot (the reserves of
DESIGN.md §3.2 keep the owner's PC usable while it installs) and its steps are logged like any other request:

    python buildserver/image/images.py base   [--name am-base] [--version core11]   # dockur's unattended install
    python buildserver/image/images.py smoke  [--image am-base]                     # boot, `cmd /c ver`, shut down
    python buildserver/image/images.py e2e    [--base am-base] [--name am-e2e]      # the e2e layer (Python + provision)
    python buildserver/image/images.py list                                         # the images in the site

The base holds only the guest agent's bootstrap (image/oem/: install.bat → setup.ps1 → bootstrap.ps1 at every logon);
layers add what their slot kind needs (E4). The coordinator must run (`python buildserver/site/sitectl.py up`); its
admin token is read from the site and never printed.

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


class Coordinator:
    """A minimal client of the coordinator's API (the CLI `amb` comes later; this keeps the image script standalone)."""

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
        client = Coordinator(sitectl.admin_token())
    except sitectl.SiteError as exc:
        print(f"images: {exc}", file=sys.stderr)
        return 2
    if args.command == "base":
        body = {"kind": "Install", "inputs": {"newImage": args.name, "version": args.version,
                                              "oemDir": str(HERE / "oem")}}
    elif args.command == "smoke":
        body = {"kind": "Smoke", "inputs": {"image": args.image}}
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
