"""Provisions, deploys and inspects the build server's site on the worker machine: CT 803 "buildserver" (README.md,
"The worker site"; since 2026-11-12).

The worker machine is our Proxmox node in Stuttgart, on the home LAN through the link (K:\\source\\proxmox\\workermachine).
Its site is a privileged Alpine container, CT 803 at 192.168.50.21, made by worker/create-ct803.sh, with its data on the
host's drive 2 (/mnt/drive2/data/buildserver, the container's /srv/buildserver). The coordinator runs there as an OpenRC
service; builds and e2e runs go there by default (`amb`), installs still run through homepc's site.

    python buildserver/site/worker.py provision   # packages, podman's configuration, the service (idempotent)
    python buildserver/site/worker.py deploy      # publish the coordinator and the guest agent, swap them in, restart
    python buildserver/site/worker.py status      # the container, the service, the coordinator, the disk, as JSON
    python buildserver/site/worker.py restart     # restart the service (refused while requests run, unless --force)
    python buildserver/site/worker.py logs [-n N] # the coordinator's log
    python buildserver/site/worker.py run -- podman ps   # one command in the container, as root

Everything reaches the worker through `ssh worker` (the workstation's alias; AMB_WORKER_SSH names another): scripts go
into the container on stdin (`pct exec 803 -- sh -s`), and files are written straight into drive 2's folder on the host,
which IS the container's /srv/buildserver (a privileged container: the host's root is the container's root). Nothing
here touches the host's network or firewall (workermachine/CLAUDE.md: those change only through their own scripts).

Exit codes: 0 success; 1 a step failed (the message says which); 2 usage.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sitectl  # noqa: E402  (publishing, the here-document writer, SiteError)

SSH_HOST = os.environ.get("AMB_WORKER_SSH") or "worker"
"""The worker machine's SSH alias in the workstation's ~/.ssh/config (key ~/.ssh/worker_ed25519, its host key pinned)."""

CT = os.environ.get("AMB_WORKER_CT") or "803"
"""The site's container."""

HOST_DATA = "/mnt/drive2/data/buildserver"
"""The site's folder on the host's drive 2; the container sees it as `SITE_ROOT` (mp0, backup=0)."""

SITE_ROOT = "/srv/buildserver"
"""The site's folder inside the container (the coordinator's AMB_ROOT)."""

URL = os.environ.get("AMB_WORKER_URL") or "http://192.168.50.21:2207"
"""The coordinator as the workstation reaches it (through the link home)."""

PUBLISH_ROOT = Path(r"Q:\tmp\buildserver-build-wk\publish")
"""Where `deploy` publishes (every build writes to Q:); apart from homepc's publish folder."""

BUILD_ROOT = Path(r"Q:\tmp\buildserver-build-wk")
"""The obj\\ and bin\\ of the worker's publishes (AmbBuildRoot): never the folder the live amb.exe is built in."""

FILES = HERE / "worker" / "files"
"""The configuration files `provision` installs (each one documents itself)."""

PACKAGES = [
    "podman",          # runs dockur's VMs, rootful
    "crun",            # podman's OCI runtime (containers.conf pins it)
    "slirp4netns",     # a user-mode network per slot: the guest reaches the coordinator at 10.0.2.2
    "fuse-overlayfs",  # podman's fallback storage driver
    "qemu-img",        # qcow2 layers and overlays of the Windows images (DESIGN.md §4.3)
    "python3",         # hmp.py (the QEMU monitor helper the balloon uses)
    "curl",            # the coordinator's health checks from inside the site
    "ca-certificates", # TLS to registries, GitHub (the submodule's last resort) and Windows downloads
    "libstdc++",       # the self-contained .NET runtime of amb-coordinator (linux-musl-x64) links it
    "libgcc",          # likewise (libgcc_s)
    "git",             # the source mirrors: pushed bundles in, a build VM's bundles out (DESIGN.md §11)
    "tzdata",          # local times in logs
]
"""What the container installs; every package is named with the reason the site needs it."""

SITE_FOLDERS = ["images", "slots", "vms", "exchange", "records", "store", "cache", "bin", "git", "tmp", "kit",
                "coordinator", "containers"]
"""The folders under `SITE_ROOT` the coordinator and podman expect; `provision` makes them, never deletes them."""

INSTALLED = [
    ("containers.conf", "/etc/containers/containers.conf", "0644"),
    ("storage.conf", "/etc/containers/storage.conf", "0644"),
    ("amb-cgroups.start", "/etc/local.d/amb-cgroups.start", "0755"),
    ("amb-coordinator.initd", "/etc/init.d/amb-coordinator", "0755"),
    ("amb-coordinator-run", "/usr/local/bin/amb-coordinator-run", "0755"),
    # The settings are not secret (the token is a file of its own), but nothing but root needs them.
    ("amb-coordinator.env", "/etc/amb-coordinator.env", "0600"),
]
"""(file in `FILES`, its path in the container, its mode): the configuration on the container's root file system, which
is on drive 1 and backed up with the container."""


def ssh(command: str, *, stdin: bytes | None = None, timeout: float = 300.0,
        check: bool = True) -> subprocess.CompletedProcess[bytes]:
    """Runs one command on the worker host.

    Args:
        command: the remote command (the host's shell parses it).
        stdin: bytes for its standard input, or None for none.
        timeout: seconds before it is abandoned.
        check: raise when it fails.

    Returns:
        The finished process (stdout and stderr as bytes).

    Raises:
        sitectl.SiteError: ssh could not run, did not finish in time, or (with check) the command failed.
    """
    try:
        done = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", SSH_HOST, command],
                              input=stdin if stdin is not None else b"", capture_output=True, timeout=timeout,
                              check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise sitectl.SiteError(f"ssh {SSH_HOST}: {exc}") from exc
    if check and done.returncode != 0:
        raise sitectl.SiteError(f"ssh {SSH_HOST} `{command[:80]}` exited {done.returncode}: "
                                f"{done.stderr.decode('utf-8', 'replace').strip()[-800:]}")
    return done


def in_ct(script: str, *, timeout: float = 600.0, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    """Runs a POSIX shell script inside the container as root, the script passed on stdin (no quoting at all).

    Args:
        script: the script.
        timeout: seconds before it is abandoned.
        check: raise when it fails.

    Returns:
        The finished process.

    Raises:
        sitectl.SiteError: as `ssh`.
    """
    return ssh(f"pct exec {CT} -- sh -s", stdin=script.encode("utf-8"), timeout=timeout, check=check)


def provision() -> dict[str, object]:
    """Installs the container's packages, folders, podman's configuration and the coordinator's service; safe to run
    again (a changed setting takes effect at the next `restart`).

    Returns:
        What the container reports afterwards: Alpine, podman, the cgroup controllers, the service's runlevel.

    Raises:
        sitectl.SiteError: a package or file could not be installed.
    """
    lines = ["set -eu", "apk update >/dev/null", "apk add --no-cache " + " ".join(PACKAGES) + " >/dev/null",
             "mkdir -p " + " ".join(f"{SITE_ROOT}/{folder}" for folder in SITE_FOLDERS)]
    for name, target, mode in INSTALLED:
        lines.append(f"mkdir -p '{target.rsplit('/', 1)[0]}'")
        lines.append(sitectl._install_file_script_text(  # noqa: SLF001  (the site scripts' shared writer)
            (FILES / name).read_text(encoding="utf-8"), target, mode))
    lines += [
        # The cgroup delegation runs now as well as at every boot: podman needs it before the first slot.
        "rc-update add local default >/dev/null 2>&1 || true",
        "sh /etc/local.d/amb-cgroups.start",
        "rc-update add amb-coordinator default >/dev/null 2>&1 || true",
        "echo alpine=$(cat /etc/alpine-release)",
        "echo podman=$(podman --version | awk '{print $3}')",
        "echo controllers=\"$(cat /sys/fs/cgroup/cgroup.subtree_control)\"",
        "echo runlevel=\"$(rc-update show default | grep amb-coordinator | tr -s ' ')\"",
    ]
    out = in_ct("\n".join(lines) + "\n", timeout=900).stdout.decode("utf-8", "replace")
    return _facts(out)


def _facts(out: str) -> dict[str, object]:
    """Reads "key=value" lines into a dict (numbers as ints).

    Args:
        out: a script's output.

    Returns:
        The facts.
    """
    facts: dict[str, object] = {}
    for line in out.splitlines():
        key, _, value = line.partition("=")
        if value and key and " " not in key:
            facts[key.strip()] = int(value) if value.strip().isdigit() else value.strip()
    return facts


def _get_json(path: str, token: str | None, timeout: float = 10.0) -> tuple[int, object]:
    """One GET of the coordinator's API.

    Args:
        path: the path (e.g. "/api/v1/status").
        token: the admin token, or None for an open path.
        timeout: seconds.

    Returns:
        (HTTP status, parsed JSON or text); status 0 when it does not answer at all.
    """
    request = urllib.request.Request(URL + path)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw, status = response.read(), response.status
    except urllib.error.HTTPError as exc:
        raw, status = exc.read(), exc.code
    except OSError:
        return 0, None
    try:
        return status, json.loads(raw) if raw else None
    except json.JSONDecodeError:
        return status, raw.decode("utf-8", "replace")


def admin_token() -> str:
    """Reads the coordinator's admin token from the container (callers keep it in memory; nothing prints it).

    Returns:
        The token.

    Raises:
        sitectl.SiteError: the coordinator never started, so there is no token yet.
    """
    done = in_ct(f"cat {SITE_ROOT}/coordinator/admin.token\n", timeout=60, check=False)
    token = done.stdout.decode().strip()
    if done.returncode != 0 or len(token) < 32:
        raise sitectl.SiteError("no admin token yet: start the coordinator once (`deploy`, or `restart`)")
    return token


def busy() -> str | None:
    """Says what a restart would cut off: the coordinator's active requests and slots.

    Returns:
        None when it runs nothing (or does not answer: nothing to cut off); else a line naming what runs.

    Raises:
        sitectl.SiteError: the token could not be read while the coordinator answers.
    """
    if _get_json("/healthz", None)[0] != 200:
        return None
    status, body = _get_json("/api/v1/status", admin_token())
    if status != 200 or not isinstance(body, dict):
        return f"the status answered {status}"
    active, slots = body.get("active") or [], body.get("slots") or []
    if not active and not slots:
        return None
    return (f"{len(active)} active request(s) ({', '.join(r['id'] + ' ' + r['kind'] for r in active)}) and "
            f"{len(slots)} slot(s)")


def _tar(entries: list[tuple[str, Path]]) -> tuple[bytes, dict[str, str]]:
    """Packs files and folders into a tar (in memory) for the host's `tar -x`, root-owned with Linux modes.

    Args:
        entries: (name in the archive, local file or folder).

    Returns:
        The archive's bytes, and the SHA-256 of every file by its name in the archive.
    """
    hashes: dict[str, str] = {}
    buffer = io.BytesIO()

    def add_file(name: str, path: Path) -> None:
        """Adds one file (executable when it is a .sh, a binary without an extension, or the coordinator).

        Args:
            name: its name in the archive.
            path: the local file.
        """
        data = path.read_bytes()
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.mtime = int(path.stat().st_mtime)
        # Windows files carry no exec bit: the modes are set here, as the container needs them.
        executable = name.endswith(".sh") or name.endswith("/amb-coordinator")
        info.mode = 0o755 if executable else 0o644
        info.uid = info.gid = 0
        archive.addfile(info, io.BytesIO(data))
        hashes[name] = hashlib.sha256(data).hexdigest()

    with tarfile.open(fileobj=buffer, mode="w") as archive:
        for name, path in entries:
            if path.is_dir():
                for item in sorted(path.rglob("*")):
                    relative = item.relative_to(path).as_posix()
                    if item.is_file() and "__pycache__" not in relative:
                        add_file(f"{name}/{relative}", item)
            else:
                add_file(name, path)
    return buffer.getvalue(), hashes


def upload(entries: list[tuple[str, Path]], host_folder: str) -> dict[str, str]:
    """Writes files into a folder on the host (drive 2, so into the container's /srv/buildserver), replaced whole, and
    checks every file's SHA-256 there.

    Args:
        entries: (name, local file or folder), as `_tar` takes them.
        host_folder: the host's folder (under `HOST_DATA`), emptied first.

    Returns:
        The SHA-256 of every file by its name.

    Raises:
        sitectl.SiteError: the upload failed, or a file arrived with another SHA-256.
    """
    if not host_folder.startswith(HOST_DATA + "/"):
        raise sitectl.SiteError(f"{host_folder} is not under {HOST_DATA}")
    data, hashes = _tar(entries)
    ssh(f"rm -rf '{host_folder}' && mkdir -p '{host_folder}' && tar -x -C '{host_folder}' -f -", stdin=data, timeout=1800)
    listing = "".join(f"{name}\n" for name in hashes)
    # The hashes are checked on the host, file by file, against what was packed here.
    out = ssh(f"cd '{host_folder}' && xargs -d '\\n' sha256sum", stdin=listing.encode(), timeout=600).stdout.decode()
    remote = {line[66:]: line[:64] for line in out.splitlines() if len(line) > 66}
    wrong = [name for name, sha in hashes.items() if remote.get(name) != sha]
    if wrong:
        raise sitectl.SiteError(f"{len(wrong)} file(s) arrived changed in {host_folder}: {', '.join(wrong[:5])}")
    return hashes


def publish() -> tuple[Path, Path]:
    """Publishes the coordinator (linux-musl-x64, self-contained, one file) and the guest agent (Native AOT, win-x64).

    Returns:
        The coordinator's publish folder and the guest agent's.

    Raises:
        sitectl.SiteError: a publish failed.
    """
    coordinator = PUBLISH_ROOT / "amb-coordinator-linux"
    guest = PUBLISH_ROOT / "amb-guest"
    sitectl._dotnet_publish("Amb.Coordinator", coordinator,  # noqa: SLF001  (the sites' shared publish)
                            ["-r", "linux-musl-x64", "--self-contained", "-p:PublishSingleFile=true",
                             "-p:DebugType=none", "-p:GenerateDocumentationFile=false"], build_root=BUILD_ROOT)
    sitectl._dotnet_publish("Amb.GuestAgent", guest, [], build_root=BUILD_ROOT)  # noqa: SLF001
    return coordinator, guest


def _wait_healthy(seconds: float) -> bool:
    """Waits until the coordinator answers /healthz from the workstation.

    Args:
        seconds: the longest wait.

    Returns:
        True when it answered.
    """
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if _get_json("/healthz", None, timeout=5)[0] == 200:
            return True
        time.sleep(1)
    return False


def deploy(force: bool) -> dict[str, object]:
    """Publishes the coordinator and the guest agent, uploads them with the build steps and the helpers, swaps them in
    and restarts the service.

    Args:
        force: restart even while requests run (they end Lost, and their slots are stopped).

    Returns:
        The deployed files' hashes and sizes, and whether the coordinator answers afterwards.

    Raises:
        sitectl.SiteError: the coordinator runs requests (without force), or a publish, the upload, the swap or the start
            failed.
    """
    running = busy()
    if running and not force:
        raise sitectl.SiteError(f"the coordinator runs {running}: wait, or deploy --force (they end Lost)")
    coordinator, guest = publish()
    repo = sitectl.REPO
    stage = f"{HOST_DATA}/tmp/deploy"
    hashes = upload([
        ("coordinator", coordinator),
        ("amb-guest.exe", guest / "amb-guest.exe"),
        ("hmp.py", repo / "buildserver" / "measure" / "hmp.py"),
        ("steps", repo / "buildserver" / "steps"),
        ("coordinator-up.sh", FILES / "coordinator-up.sh"),
    ], stage)
    swap = f"""set -eu
bin={SITE_ROOT}/bin
new={SITE_ROOT}/tmp/deploy
rc-service amb-coordinator stop >/dev/null 2>&1 || true
rm -rf "$bin/coordinator.old" "$bin/steps.old"
[ -d "$bin/coordinator" ] && mv "$bin/coordinator" "$bin/coordinator.old" || true
mv "$new/coordinator" "$bin/coordinator"
[ -d "$bin/steps" ] && mv "$bin/steps" "$bin/steps.old" || true
mv "$new/steps" "$bin/steps"
mv -f "$new/amb-guest.exe" "$bin/amb-guest.exe"
mv -f "$new/hmp.py" "$bin/hmp.py"
mv -f "$new/coordinator-up.sh" "$bin/coordinator-up.sh"
chmod 0755 "$bin/coordinator/amb-coordinator" "$bin/coordinator-up.sh"
rm -rf "$new"
rc-service amb-coordinator start >/dev/null
echo coordinator_bytes=$(stat -c %s "$bin/coordinator/amb-coordinator")
"""
    facts = _facts(in_ct(swap, timeout=600).stdout.decode("utf-8", "replace"))
    facts["coordinator_sha256"] = hashes.get("coordinator/amb-coordinator", "")[:16]
    facts["guest_sha256"] = hashes.get("amb-guest.exe", "")[:16]
    facts["answers"] = _wait_healthy(60)
    if not facts["answers"]:
        raise sitectl.SiteError("the coordinator did not answer within 60 s after the swap:\n" + logs(40))
    return facts


def restart(force: bool) -> dict[str, object]:
    """Restarts the coordinator's service (it reads /etc/amb-coordinator.env again).

    Args:
        force: restart even while requests run (they end Lost).

    Returns:
        Whether it answers afterwards.

    Raises:
        sitectl.SiteError: it runs requests (without force), or it did not answer afterwards.
    """
    running = busy()
    if running and not force:
        raise sitectl.SiteError(f"the coordinator runs {running}: wait, or restart --force (they end Lost)")
    in_ct("rc-service amb-coordinator restart >/dev/null\n", timeout=600)
    if not _wait_healthy(60):
        raise sitectl.SiteError("the coordinator did not answer within 60 s:\n" + logs(40))
    return {"answers": True}


def logs(lines: int) -> str:
    """The end of the coordinator's log.

    Args:
        lines: how many lines.

    Returns:
        The lines.

    Raises:
        sitectl.SiteError: the container could not be reached.
    """
    return in_ct(f"tail -n {int(lines)} {SITE_ROOT}/coordinator/coordinator.log 2>/dev/null || true\n",
                 timeout=60).stdout.decode("utf-8", "replace")


def status() -> dict[str, object]:
    """Describes the site without changing it.

    Returns:
        The container's state, the service's, the coordinator's health and version, drive 2's space, the images.

    Raises:
        sitectl.SiteError: the worker host could not be reached.
    """
    report: dict[str, object] = {"ct": ssh(f"pct status {CT}", timeout=60).stdout.decode().strip()}
    if "running" not in str(report["ct"]):
        return report
    out = in_ct(f"""echo service=$(rc-service amb-coordinator status 2>&1 | tr -s ' ' | tail -n 1)
df -k {SITE_ROOT} | awk 'NR==2 {{print "disk_free_gb=" int($4/1048576)}}'
for d in {SITE_ROOT}/images/*/; do [ -d "$d" ] && echo "image_$(basename "$d")=$(du -sh "$d" | cut -f1)"; done
echo podman=$(podman --version | awk '{{print $3}}')
""", timeout=120).stdout.decode("utf-8", "replace")
    report.update(_facts(out))
    health, _ = _get_json("/healthz", None)
    report["answers"] = health == 200
    if health == 200:
        code, body = _get_json("/api/v1/status", admin_token())
        if code == 200 and isinstance(body, dict):
            report["version"] = body.get("version")
            report["active_requests"] = len(body.get("active") or [])
            report["slots"] = len(body.get("slots") or [])
            report["free"] = [site.get("free") for site in body.get("sites") or []]
    return report


def main(argv: list[str]) -> int:
    """Parses the command line and runs one command.

    Args:
        argv: the arguments after the program name.

    Returns:
        The process exit code (0 success, 1 a step failed, 2 usage).
    """
    parser = argparse.ArgumentParser(description="The build server's site on the worker machine (CT 803).")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("provision", help="packages, podman's configuration and the coordinator's service")
    deploy_cmd = sub.add_parser("deploy", help="publish and swap in the coordinator and the guest agent, then restart")
    deploy_cmd.add_argument("--force", action="store_true", help="even while requests run (they end Lost)")
    restart_cmd = sub.add_parser("restart", help="restart the coordinator's service")
    restart_cmd.add_argument("--force", action="store_true", help="even while requests run (they end Lost)")
    sub.add_parser("status", help="describe the site as JSON")
    logs_cmd = sub.add_parser("logs", help="the end of the coordinator's log")
    logs_cmd.add_argument("-n", type=int, default=60)
    run = sub.add_parser("run", help="run one command in the container as root")
    run.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        if args.command == "provision":
            print(json.dumps(provision(), indent=2))
        elif args.command == "deploy":
            print(json.dumps(deploy(args.force), indent=2))
        elif args.command == "restart":
            print(json.dumps(restart(args.force), indent=2))
        elif args.command == "status":
            print(json.dumps(status(), indent=2))
        elif args.command == "logs":
            print(logs(args.n), end="")
        elif args.command == "run":
            cmd = [c for c in args.cmd if c != "--"]
            if not cmd:
                parser.error("run needs a command")
            # The words go into the container's shell quoted one by one, so a word with spaces stays one word.
            quoted = " ".join("'" + word.replace("'", "'\"'\"'") + "'" for word in cmd)
            done = in_ct(quoted + "\n", timeout=3600, check=False)
            sys.stdout.buffer.write(done.stdout)
            sys.stderr.buffer.write(done.stderr)
            return done.returncode
        return 0
    except sitectl.SiteError as exc:
        print(f"worker: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
