"""Run the end-to-end suite from the host PC: stage it into a test desktop, provision, run, collect the results.

The test desktop is a claude-fleet Windows 11 VM (skill `claude-desktops`); its persist folder on this PC
(Q:\\claude-desktops\\windows\\desktops\\<name>\\persist) is the guest's Z:. Staging and collecting are plain file
copies through that folder; only starting processes in the guest goes through the fleet (MCP `desktop_call`
`launch`, read_file).

    python e2e/host/e2e_run.py --desktop am-e2e-1 --create --provision -- -v suites/test_smoke.py
    python e2e/host/e2e_run.py --desktop am-e2e-1 -- -v -m p0            # an existing, provisioned desktop
    python e2e/host/e2e_run.py --desktop am-e2e-1 --delete-after -- -v   # and delete the desktop at the end

Results land in Q:\\agentmaster-e2e\\runs\\<run>\\ (pytest.log, junit.xml, the mock's request log, and every failed
test's evidence: screen.png, uia.txt, the profile's logs). The exit code is pytest's.

⚠ The fleet token is read from the cluster (kubectl) into a variable and never printed or written; desktop
tokens and noVNC links never leave the fleet's answers either (CLAUDE.md, desktops). Windows desktops take
4–6 GB and several cores of this PC: delete them when done (--delete-after, or the fleet's delete_desktop).
"""
from __future__ import annotations

import argparse
import base64
import json
import shutil
import subprocess
import sys
import time
import zipfile
from datetime import datetime
from pathlib import Path

E2E = Path(__file__).resolve().parents[1]
FLEET_URL = "http://192.168.50.150:7800/mcp"
FLEET_CLIENT = Path(r"K:\source\claude-desktops\fleet")
PERSIST_ROOT = Path(r"Q:\claude-desktops\windows\desktops")
RUNS = Path(r"Q:\agentmaster-e2e\runs")
STAGED = ("amdriver", "mockllm", "suites", "guest", "pytest.ini")
"""What the guest needs of the e2e folder."""


class Fleet:
    """The claude-fleet hub, through its MCP endpoint.

    Attributes:
        client: The McpClient (holds the token; never print it).
    """

    def __init__(self) -> None:
        """Read the fleet token from the cluster and prepare the client.

        Returns:
            None.
        Raises:
            subprocess.CalledProcessError: When kubectl cannot read the secret.
        """
        sys.path.insert(0, str(FLEET_CLIENT))
        from mcp_client import McpClient  # the repo's stdlib client
        raw = subprocess.check_output(["kubectl", "-n", "claude-desktops", "get", "secret", "fleet-token",
                                       "-o", "jsonpath={.data.token}"])
        self.client = McpClient(FLEET_URL, base64.b64decode(raw).decode(), timeout_s=900)

    def tool(self, name: str, arguments: dict) -> str:
        """Call a fleet tool.

        Args:
            name: The tool (list_desktops, create_desktop, delete_desktop, desktop_call …).
            arguments: Its arguments.
        Returns:
            The text of its answer.
        Raises:
            RuntimeError: When the tool reported an error.
        """
        result = self.client.request("tools/call", {"name": name, "arguments": arguments})
        text = "\n".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        if result.get("isError"):
            raise RuntimeError(f"{name} failed: {text[:500]}")
        return text

    def desk(self, desktop: str, tool: str, arguments: dict) -> str:
        """Call a tool of a desktop's agent.

        Args:
            desktop: The desktop's name.
            tool: The desktop tool (launch, read_file, run_command …).
            arguments: Its arguments.
        Returns:
            The text of its answer.
        Raises:
            RuntimeError: When the tool reported an error.
        """
        return self.tool("desktop_call", {"name": desktop, "tool": tool, "arguments": arguments})

    def exists(self, desktop: str) -> bool:
        """Whether a desktop exists.

        Args:
            desktop: The name.
        Returns:
            True when list_desktops names it.
        """
        return f'"{desktop}"' in self.tool("list_desktops", {}) or desktop in self.tool("list_desktops", {})


def log(message: str) -> None:
    """Print a time-stamped progress line.

    Args:
        message: The text.
    Returns:
        None.
    """
    print(f"[{datetime.now():%H:%M:%S}] {message}", flush=True)


def stage(persist: Path) -> None:
    """Copy the suite into the desktop's persist folder (the guest's Z:\\e2e\\src).

    Args:
        persist: The desktop's persist folder on this PC.
    Returns:
        None.
    """
    src = persist / "e2e" / "src"
    for name in STAGED:
        target = src / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()
        source = E2E / name
        if source.is_dir():
            shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__", ".pytest_cache", "*.pyc"))
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    (persist / "e2e" / "out").mkdir(parents=True, exist_ok=True)
    (persist / "e2e" / "cache").mkdir(parents=True, exist_ok=True)


def wait_guest_file(fleet: Fleet, desktop: str, path: str, deadline_s: float) -> str:
    """Poll a file in the guest until it exists.

    Args:
        fleet: The fleet.
        desktop: The desktop.
        path: The guest path.
        deadline_s: How long.
    Returns:
        The file's text.
    Raises:
        TimeoutError: When it never appeared.
    """
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        try:
            return fleet.desk(desktop, "read_file", {"path": path})
        except RuntimeError:
            time.sleep(10)
    raise TimeoutError(f"{path} did not appear in {deadline_s:.0f} s")


def provision(fleet: Fleet, desktop: str, extra: list[str]) -> None:
    """Provision the guest (Agentmaster, claude.exe, Git, PowerShell 7, the venv, hosts, env, dumps).

    Args:
        fleet: The fleet.
        desktop: The desktop.
        extra: More provision.ps1 arguments (e.g. ["-AgentmasterZip", "Z:\\e2e\\builds\\x.zip"]).
    Returns:
        None.
    Raises:
        RuntimeError: When provisioning failed (its log's tail is in the message).
    """
    fleet.desk(desktop, "launch", {"command": "cmd /c Z:\\e2e\\src\\guest\\provision.cmd -Pwsh " + " ".join(extra),
                                   "cwd": "C:\\"})
    code = wait_guest_file(fleet, desktop, "C:/e2e/provision.done", 1800).strip()
    tail = fleet.desk(desktop, "read_file", {"path": "C:/e2e/provision.log"})[-1500:]
    if code.splitlines()[-1].strip() != "0":
        raise RuntimeError(f"provisioning failed (exit {code}):\n{tail}")
    log("provisioned: " + tail.strip().splitlines()[-1][:300])


def run(fleet: Fleet, desktop: str, persist: Path, name: str, pytest_args: list[str], deadline_s: float) -> dict:
    """Start the suite in the guest and wait for its status file.

    Args:
        fleet: The fleet.
        desktop: The desktop.
        persist: Its persist folder.
        name: The run's name.
        pytest_args: Arguments for pytest.
        deadline_s: How long the run may take.
    Returns:
        The final status object.
    Raises:
        TimeoutError: When the run did not finish in time.
    """
    status_file = persist / "e2e" / "out" / f"{name}.status.json"
    status_file.unlink(missing_ok=True)
    command = "C:\\e2e\\venv\\Scripts\\python.exe Z:\\e2e\\src\\guest\\run_suite.py " + name + " " + " ".join(pytest_args)
    fleet.desk(desktop, "launch", {"command": command, "cwd": "C:\\e2e"})
    end = time.monotonic() + deadline_s
    last = ""
    while time.monotonic() < end:
        time.sleep(10)
        try:
            status = json.loads(status_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if status.get("state") == "done":
            return status
        if status.get("state") != last:
            last = status.get("state", "")
            log(f"run {name}: {last}")
    raise TimeoutError(f"run {name} did not finish in {deadline_s:.0f} s")


def collect(persist: Path, name: str) -> Path:
    """Unpack the run's results zip into the runs folder.

    Args:
        persist: The desktop's persist folder.
        name: The run's name.
    Returns:
        The results folder.
    """
    target = RUNS / name
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    with zipfile.ZipFile(persist / "e2e" / "out" / f"{name}.zip") as z:
        z.extractall(target)
    return target


def main(argv: list[str]) -> int:
    """The command line.

    Args:
        argv: The arguments; everything after "--" goes to pytest.
    Returns:
        pytest's exit code (or 98 when the orchestration failed).
    Raises:
        SystemExit: When the desktop does not exist and --create was not given.
        RuntimeError / TimeoutError: When provisioning or the run failed (the desktop is still deleted when
            --delete-after was given).
    """
    pytest_args: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, pytest_args = argv[:i], argv[i + 1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--desktop", default="am-e2e-1")
    parser.add_argument("--create", action="store_true", help="create the desktop when it does not exist")
    parser.add_argument("--memory-gb", type=int, default=8)
    parser.add_argument("--cpus", type=int, default=6)
    parser.add_argument("--provision", action="store_true", help="run provision.ps1 first")
    parser.add_argument("--provision-arg", action="append", default=[], help="an extra provision.ps1 argument")
    parser.add_argument("--run", default="", help="the run's name (default: a time stamp)")
    parser.add_argument("--deadline-min", type=float, default=120)
    parser.add_argument("--delete-after", action="store_true", help="delete the desktop when done")
    args = parser.parse_args(argv)
    name = args.run or f"run-{datetime.now():%Y%m%d-%H%M%S}"
    persist = PERSIST_ROOT / args.desktop / "persist"
    fleet = Fleet()
    code = 98
    try:
        if not fleet.exists(args.desktop):
            if not args.create:
                raise SystemExit(f"desktop {args.desktop} does not exist (pass --create)")
            log(f"creating {args.desktop}")
            fleet.tool("create_desktop", {"name": args.desktop, "os": "windows", "width": 1920, "height": 1080,
                                          "memory_gb": args.memory_gb, "cpus": args.cpus, "wait": True})
        log(f"staging into {persist}")
        stage(persist)
        if args.provision:
            log("provisioning (Agentmaster, claude.exe, Git, PowerShell 7, the venv) …")
            provision(fleet, args.desktop, args.provision_arg)
        log(f"running {name}: pytest {' '.join(pytest_args)}")
        status = run(fleet, args.desktop, persist, name, pytest_args, args.deadline_min * 60)
        results = collect(persist, name)
        code = int(status.get("exit", 98))
        log(f"done: exit {code}, tests {status.get('tests')}, failures {status.get('failures')}, "
            f"errors {status.get('errors')}, {status.get('seconds')} s — results in {results}")
    finally:
        if args.delete_after:
            log(f"deleting {args.desktop}")
            fleet.tool("delete_desktop", {"name": args.desktop})
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
