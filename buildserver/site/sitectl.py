"""Creates, provisions and inspects this PC's build-server site: the WSL distro `buildserver` (DESIGN.md §5.2, E2).

The site is one WSL 2 distro of its own whose disk is a file in the dedicated folder `Q:\\buildserver\\wsl`; fastrund,
the coordinator and the Windows VMs of this PC run inside it, and Docker Desktop plays no part. This script is the
only way the distro is made or changed, so its state is reproducible from the repository:

    python buildserver/site/sitectl.py create      # download Alpine, import the distro, install and configure it
    python buildserver/site/sitectl.py provision   # re-apply packages, folders and configuration (idempotent)
    python buildserver/site/sitectl.py status      # what the site is now, as JSON
    python buildserver/site/sitectl.py run -- podman ps   # one command inside the distro, as root

Why each choice:
- Alpine's minirootfs: 3.7 MB, a cold distro start in 0.52-0.79 s (measured 2026-10-25), and the packages the site
  needs (podman, crun, slirp4netns, qemu-img, python3) all exist in its main and community repositories.
- Scripts go into the distro on stdin (`wsl.exe -e sh -s`), never on wsl.exe's command line: `wsl.exe -- sh -c`
  re-parses the script in the distro's default shell (its `$VARS` expand early), and stdin needs no quoting at all.
- `WSL_UTF8=1` makes wsl.exe print its own messages in UTF-8 instead of UTF-16LE.
- Nothing here restarts WSL's VM (`wsl --shutdown`): that would end every Docker container and other sessions'
  desktops. Only this distro is ever terminated (`wsl --terminate buildserver`), which touches nothing else.

Exit codes: 0 success; 1 a step failed (the message says which); 2 usage.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

DISTRO = "buildserver"
"""The distro's name in WSL; every other component (the host agent, the image script) finds the site by it."""

DEFAULT_ROOT = Path(r"Q:\buildserver")
"""The dedicated folder of E2: the distro's disk lives in `<root>\\wsl`, downloads in `<root>\\cache`."""

ALPINE_BRANCH = "v3.22"
"""The Alpine branch whose newest minirootfs is imported; the one the measurements of 2026-10-25 used."""

ALPINE_MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
"""Alpine's CDN; the release index and the tarball both come from it, and the tarball is checked by its SHA-256."""

PACKAGES = [
    "podman",          # runs fastrund, the coordinator (until it is a fastrun app) and dockur VMs, rootful
    "crun",            # podman's OCI runtime (containers.conf pins it)
    "slirp4netns",     # a user-mode network namespace per container: no bridge in the namespace WSL shares
    "fuse-overlayfs",  # podman's fallback storage driver, if the kernel's overlay refuses a mount
    "qemu-img",        # qcow2 layers and overlays of the Windows images (DESIGN.md §4.3)
    "python3",         # the image script and the slot helpers run here (D12: Python for setup scripts)
    "curl",            # health checks of fastrund and the coordinator from inside the site
    "ca-certificates", # TLS to the registry and to Alpine's mirror
]
"""What the site installs; every package is named with the reason the site needs it."""

SITE_FOLDERS = [
    "/srv/buildserver/images",            # read-only Windows images: data.qcow2 + UEFI files + windows.boot
    "/srv/buildserver/slots",             # throwaway overlays of e2e slots
    "/srv/buildserver/vms",               # kept build VMs: overlay + raw data disk
    "/srv/buildserver/exchange",          # each slot's share: bundles in, artifacts out
    "/srv/buildserver/records",           # the coordinator's JSON records (DESIGN.md §6)
    "/srv/buildserver/store",             # artifacts the site keeps (zips, logs), by build id
    "/srv/buildserver/cache",             # downloads (ISOs, installers) reused across images
    "/srv/buildserver/bin",               # the coordinator and helper scripts deployed into the site
    "/srv/buildserver/fastrun/store",     # fastrund's node store (FASTRUN_STORE_DIR)
    "/srv/buildserver/fastrun/data",      # fastrund's logs (FASTRUN_DATA_DIR)
    "/srv/buildserver/fastrun/scratch",   # fastrund's scratch folders (FASTRUN_SCRATCH_DIR)
    "/srv/buildserver/fastrun/lane",      # fastrund's raw-lane socket folder
    "/srv/buildserver/fastrun/regcreds",  # the registry credentials fastrund pulls with (mode 0700, file 0600)
]
"""The folders the site's components expect; created by `provision`, never deleted by it."""

FILES_DIR = Path(__file__).resolve().parent / "files"
"""The configuration files this script installs into the distro (each one documents itself)."""


class SiteError(RuntimeError):
    """A step of making or inspecting the site failed; the message names the step and what went wrong."""


def _wsl_env() -> dict[str, str]:
    """Returns the environment for wsl.exe calls.

    Returns:
        A copy of this process's environment with `WSL_UTF8=1`, so that wsl.exe's own output (`-l -v`, errors) is
        UTF-8 instead of UTF-16LE and can be decoded like everything else.
    """
    env = dict(os.environ)
    env["WSL_UTF8"] = "1"
    return env


def wsl(args: list[str], *, stdin: bytes | None = None, timeout: float = 120.0,
        check: bool = True) -> subprocess.CompletedProcess[bytes]:
    """Runs wsl.exe with the given arguments.

    Args:
        args: wsl.exe's arguments (without the program name).
        stdin: bytes for the child's standard input, or None for none.
        timeout: seconds before the call is abandoned.
        check: raise SiteError when wsl.exe exits non-zero.

    Returns:
        The completed process, its stdout and stderr as bytes.

    Raises:
        SiteError: wsl.exe exited non-zero (with `check`) or did not finish within `timeout`.
    """
    try:
        done = subprocess.run(["wsl.exe", *args], input=stdin, capture_output=True, timeout=timeout,
                              env=_wsl_env(), check=False)
    except subprocess.TimeoutExpired as exc:
        raise SiteError(f"wsl.exe {' '.join(args[:4])}... did not finish within {timeout:.0f} s") from exc
    if check and done.returncode != 0:
        raise SiteError(f"wsl.exe {' '.join(args[:4])}... exited {done.returncode}: "
                        f"{(done.stderr or done.stdout).decode('utf-8', 'replace').strip()[:2000]}")
    return done


def in_distro(script: str, *, timeout: float = 600.0, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    """Runs a POSIX shell script inside the site distro as root, the script passed on stdin.

    Args:
        script: the script's text; `set -eu` is prepended so a failing command stops it.
        timeout: seconds before the call is abandoned.
        check: raise SiteError when the script exits non-zero.

    Returns:
        The completed process.

    Raises:
        SiteError: the script failed (with `check`) or timed out.
    """
    body = "set -eu\n" + script
    return wsl(["-d", DISTRO, "-u", "root", "-e", "sh", "-s"], stdin=body.encode("utf-8"), timeout=timeout,
               check=check)


def distro_states() -> dict[str, str]:
    """Lists every WSL distro with its state.

    Returns:
        A mapping from distro name to its state word ("Running" or "Stopped").

    Raises:
        SiteError: `wsl.exe -l -v` failed.
    """
    out = wsl(["-l", "-v"], timeout=30).stdout.decode("utf-8", "replace")
    states: dict[str, str] = {}
    for line in out.splitlines()[1:]:
        # Each row is "[*] NAME STATE VERSION"; the asterisk marks the default distro and is not part of the name.
        parts = line.replace("*", " ").split()
        if len(parts) >= 3:
            states[parts[0]] = parts[1]
    return states


def _download(url: str, dest: Path, *, timeout: float = 120.0) -> None:
    """Downloads a URL into a file, through a temporary name so a broken download never looks complete.

    Args:
        url: what to fetch.
        dest: where the finished file goes.
        timeout: the socket timeout of the request, in seconds.

    Raises:
        SiteError: the request failed.
    """
    tmp = dest.with_name(dest.name + ".part")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response, open(tmp, "wb") as out:
            while chunk := response.read(1 << 20):
                out.write(chunk)
        tmp.replace(dest)
    except OSError as exc:
        tmp.unlink(missing_ok=True)
        raise SiteError(f"download of {url} failed: {exc}") from exc


def _sha256(path: Path) -> str:
    """Hashes a file.

    Args:
        path: the file.

    Returns:
        Its SHA-256, lowercase hex.
    """
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def parse_latest_releases(text: str, flavor: str = "alpine-minirootfs") -> dict[str, str]:
    """Finds one flavor's entry in Alpine's `latest-releases.yaml`, without a YAML library.

    The file is a list of flat mappings ("-" then indented "key: value" lines); only the scalar keys this script reads
    (flavor, file, sha256, version) are needed, so a line parser is enough and keeps the script dependency-free.

    Args:
        text: the file's content.
        flavor: the `flavor` value to look for.

    Returns:
        The entry's scalar keys and values.

    Raises:
        SiteError: no entry has that flavor, or it lacks `file` or `sha256`.
    """
    entries: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.strip() == "-":
            current = {}
            entries.append(current)
            continue
        if current is not None and line.startswith("  ") and ":" in line:
            key, _, value = line.strip().partition(":")
            current[key.strip()] = value.strip().strip('"')
    for entry in entries:
        if entry.get("flavor") == flavor:
            if not entry.get("file") or not entry.get("sha256"):
                raise SiteError(f"the {flavor} entry of latest-releases.yaml has no file or sha256")
            return entry
    raise SiteError(f"latest-releases.yaml lists no {flavor} entry")


def fetch_rootfs(cache_dir: Path, branch: str = ALPINE_BRANCH) -> Path:
    """Gets the newest Alpine minirootfs of a branch into the cache, checked by the SHA-256 Alpine publishes.

    Args:
        cache_dir: the download cache (created when missing).
        branch: the Alpine branch, e.g. "v3.22".

    Returns:
        The verified tarball's path.

    Raises:
        SiteError: the index or the tarball could not be downloaded, or the tarball's hash does not match.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    base = f"{ALPINE_MIRROR}/{branch}/releases/x86_64"
    index = cache_dir / f"alpine-{branch}-latest-releases.yaml"
    _download(f"{base}/latest-releases.yaml", index)
    entry = parse_latest_releases(index.read_text(encoding="utf-8"))
    tarball = cache_dir / entry["file"]
    if not tarball.exists() or _sha256(tarball) != entry["sha256"]:
        _download(f"{base}/{entry['file']}", tarball)
    actual = _sha256(tarball)
    if actual != entry["sha256"]:
        tarball.unlink(missing_ok=True)
        raise SiteError(f"{entry['file']}: SHA-256 {actual} is not the published {entry['sha256']}")
    return tarball


def _install_file_script(source: Path, target: str, mode: str = "0644") -> str:
    """Builds the shell lines that write one of `files/` into the distro.

    The content travels inside the script on stdin (a quoted here-document), so no Windows path has to be visible
    from the distro and no quoting of the content is needed.

    Args:
        source: the file in this repository.
        target: its absolute path inside the distro.
        mode: the octal mode to give it.

    Returns:
        The shell lines.

    Raises:
        SiteError: the content contains the here-document's end marker (it cannot be sent this way).
    """
    content = source.read_text(encoding="utf-8")
    marker = "AMB_SITE_FILE_END"
    if marker in content:
        raise SiteError(f"{source.name} contains the marker {marker}")
    folder = target.rsplit("/", 1)[0] or "/"
    return (f"mkdir -p '{folder}'\n"
            f"cat > '{target}.tmp' <<'{marker}'\n{content}{'' if content.endswith(chr(10)) else chr(10)}{marker}\n"
            f"chmod {mode} '{target}.tmp'\nmv -f '{target}.tmp' '{target}'\n")


def provision() -> dict[str, object]:
    """Installs the site's packages, folders and configuration into the distro; safe to run again.

    Returns:
        What the distro reports afterwards: kernel, Alpine release, podman version.

    Raises:
        SiteError: a package or file could not be installed.
    """
    script = "\n".join([
        _install_file_script(FILES_DIR / "wsl.conf", "/etc/wsl.conf"),
        _install_file_script(FILES_DIR / "containers.conf", "/etc/containers/containers.conf"),
        # apk's index first: a minirootfs ships without one.
        "apk update >/dev/null",
        "apk add --no-cache " + " ".join(PACKAGES) + " >/dev/null",
        "mkdir -p " + " ".join(SITE_FOLDERS),
        # The registry credentials folder is readable by root only: fastrund reads it, nobody else needs to.
        "chmod 0700 /srv/buildserver/fastrun/regcreds",
        "echo kernel=$(uname -r)",
        "echo alpine=$(cat /etc/alpine-release)",
        "echo podman=$(podman --version | awk '{print $3}')",
    ])
    out = in_distro(script, timeout=900).stdout.decode("utf-8", "replace")
    facts: dict[str, object] = {}
    for line in out.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            facts[key.strip()] = value.strip()
    # wsl.conf is read when the distro starts: terminate THIS distro (never `wsl --shutdown`) so the next call
    # starts it with the new settings.
    wsl(["--terminate", DISTRO], timeout=60)
    return facts


def create(root: Path) -> dict[str, object]:
    """Makes the site from nothing: the folders, the rootfs, the imported distro, then `provision`.

    Args:
        root: the dedicated folder (E2), normally Q:\\buildserver.

    Returns:
        The provisioned distro's facts plus the timings of each step.

    Raises:
        SiteError: the distro already exists, or a step failed.
    """
    if DISTRO in distro_states():
        raise SiteError(f"the distro {DISTRO} exists already; use `provision` to update it")
    timings: dict[str, float] = {}
    t0 = time.monotonic()
    tarball = fetch_rootfs(root / "cache")
    timings["rootfs_s"] = round(time.monotonic() - t0, 2)
    disk_dir = root / "wsl"
    disk_dir.mkdir(parents=True, exist_ok=True)
    t1 = time.monotonic()
    wsl(["--import", DISTRO, str(disk_dir), str(tarball), "--version", "2"], timeout=300)
    timings["import_s"] = round(time.monotonic() - t1, 2)
    t2 = time.monotonic()
    facts = provision()
    timings["provision_s"] = round(time.monotonic() - t2, 2)
    return {**facts, "timings": timings, "rootfs": tarball.name, "disk": str(disk_dir / "ext4.vhdx")}


def status(root: Path) -> dict[str, object]:
    """Describes the site without changing it (a stopped distro stays stopped).

    Args:
        root: the dedicated folder.

    Returns:
        The distro's state, the disk file's size, and, when the distro is running, its kernel, free space and
        memory as seen inside.

    Raises:
        SiteError: wsl.exe failed.
    """
    states = distro_states()
    disk = root / "wsl" / "ext4.vhdx"
    report: dict[str, object] = {
        "distro": DISTRO,
        "state": states.get(DISTRO, "absent"),
        "disk": str(disk),
        "disk_gb": round(disk.stat().st_size / 1e9, 2) if disk.exists() else None,
    }
    if report["state"] == "Running":
        # Only a running distro is asked: asking a stopped one would start it, and status must change nothing.
        out = in_distro("df -k / | awk 'NR==2{print \"root_free_kb=\"$4}'\n"
                        "awk '/MemAvailable/{print \"mem_available_kb=\"$2}' /proc/meminfo\n"
                        "echo kernel=$(uname -r)\n", timeout=60).stdout.decode("utf-8", "replace")
        for line in out.splitlines():
            key, _, value = line.partition("=")
            report[key] = int(value) if value.isdigit() else value
    return report


def main(argv: list[str]) -> int:
    """Parses the command line and runs one command.

    Args:
        argv: the arguments after the program name.

    Returns:
        The process exit code (0 success, 1 a step failed, 2 usage).
    """
    parser = argparse.ArgumentParser(description="The build server's site distro on this PC.")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="the dedicated folder (default Q:\\buildserver)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("create", help="make the distro from nothing")
    sub.add_parser("provision", help="re-apply packages, folders and configuration")
    sub.add_parser("status", help="describe the site as JSON")
    run = sub.add_parser("run", help="run one command inside the distro as root")
    run.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            print(json.dumps(create(args.root), indent=2))
        elif args.command == "provision":
            print(json.dumps(provision(), indent=2))
        elif args.command == "status":
            print(json.dumps(status(args.root), indent=2))
        elif args.command == "run":
            cmd = [c for c in args.cmd if c != "--"]
            if not cmd:
                parser.error("run needs a command")
            done = wsl(["-d", DISTRO, "-u", "root", "-e", *cmd], timeout=3600, check=False)
            sys.stdout.buffer.write(done.stdout)
            sys.stderr.buffer.write(done.stderr)
            return done.returncode
        return 0
    except SiteError as exc:
        print(f"site: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
