"""The build MVP's done-when on the REAL site (DESIGN.md §13, P3's MVP): `amb build` of a commit yields a build id whose
files are on Q:, an identical request returns that id, `amb e2e` passes on it, `amb fetch` copies it out, and the
source repository on K: was only read.

Checks, in order, all through `amb` (the CLI a person uses) against the coordinator in the site distro:

1. `amb build <rev> --wait` (Release, dev identity unless told otherwise) ends Succeeded with a build id;
2. the build's manifest lists a portable zip and a layout zip, each with its SHA-256 (`amb artifacts --json`);
3. the same `amb build` again ends Succeeded within a minute, naming the same build id (no second build);
4. `amb e2e <build id> -- -v suites/test_smoke.py` ends Succeeded (the e2e loop closes on a build of this service);
5. `amb fetch <build id>` copies the portable zip to Q:\\buildserver\\out\\<build id> and checks its SHA-256;
6. no build slot is left (its container is gone), while the kept build VM's folder stays (its disks are the next
   build's);
7. K: was only read: the source repository has no new worktree and no FETCH_HEAD written since the run began (the
   mirror's fetch writes into the mirror, never into the repository it reads).

The first build of a commit in a new VM is a cold build (an hour or more on this PC); later ones are incremental.

    python buildserver/tests/site_build.py [--rev HEAD] [--config Release] [--identity dev] [--purge none]

Exit codes: 0 every check passed; 1 a check failed (the output says which); 2 no coordinator or no such rev.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCE = Path(r"K:\source\Agentmaster")
AMB = Path(r"Q:\tmp\buildserver-build\Amb.Cli\bin\Debug\net10.0-windows\win-x64\amb.exe")
sys.path.insert(0, str(REPO / "buildserver" / "site"))
import sitectl  # noqa: E402  (commands in the site distro)

FAILED: list[str] = []
"""The names of the checks that failed."""


def check(name: str, ok: bool, detail: str = "") -> bool:
    """Prints one check's result and remembers a failure.

    Args:
        name: what is checked.
        ok: whether it held.
        detail: what was seen.

    Returns:
        ok.
    """
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""), flush=True)
    if not ok:
        FAILED.append(name)
    return ok


def amb(*args: str, timeout: float = 4 * 3600) -> tuple[int, str]:
    """Runs the CLI and echoes its output.

    Args:
        args: the CLI's arguments.
        timeout: the longest wait.

    Returns:
        (exit code, output).
    """
    done = subprocess.run([str(AMB), *args], capture_output=True, text=True, timeout=timeout, check=False)
    out = done.stdout + done.stderr
    for line in out.splitlines()[-12:]:
        print("    " + line, flush=True)
    return done.returncode, out


def build_id_of(output: str) -> str | None:
    """The build id an `amb build --wait` reported in its last line ("Succeeded: built b... / build b... exists").

    Args:
        output: the CLI's output.

    Returns:
        The id, or None.
    """
    for word in reversed(output.replace(":", " ").replace(",", " ").split()):
        if word.startswith("b") and "-" in word and word[1:9].isdigit():
            return word
    return None


def source_marks() -> dict[str, object]:
    """What a write into the source repository would change: its worktree list and its FETCH_HEAD.

    Returns:
        The worktree list and FETCH_HEAD's modification time (None when it has none).
    """
    worktrees = subprocess.run(["git", "-C", str(SOURCE), "worktree", "list", "--porcelain"], capture_output=True,
                               text=True, check=False).stdout
    fetch_head = SOURCE / ".git" / "FETCH_HEAD"
    return {"worktrees": worktrees, "fetch_head": fetch_head.stat().st_mtime if fetch_head.exists() else None}


def main(argv: list[str]) -> int:
    """Runs the checks.

    Args:
        argv: the arguments after the program name.

    Returns:
        The exit code.
    """
    parser = argparse.ArgumentParser(description="The build MVP's done-when on the real site.")
    parser.add_argument("--rev", default="HEAD")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--identity", default="dev")
    parser.add_argument("--purge", default="none")
    args = parser.parse_args(argv)
    marks = source_marks()
    flags = ["--config", args.config, "--identity", args.identity, "--purge", args.purge, "--wait"]

    started = time.time()
    code, out = amb("build", args.rev, *flags)
    if code == 2:
        print(f"site_build: {out.strip()}", file=sys.stderr)
        return 2
    build_id = build_id_of(out)
    if not check("the build ends Succeeded with a build id", code == 0 and build_id is not None,
                 f"{build_id} in {(time.time() - started) / 60:.1f} min"):
        return 1

    code, out = amb("artifacts", build_id, "--json")
    manifest = json.loads(out[out.index("{"):]) if code == 0 and "{" in out else {}
    kinds = {a["kind"]: a for a in manifest.get("artifacts", [])}
    check("its manifest lists a portable zip and a layout zip with their SHA-256",
          all(k in kinds and len(kinds[k]["sha256"]) == 64 for k in ("portable-zip", "layout-zip")),
          ", ".join(f"{k} {v['bytes'] / 1048576:.0f} MB" for k, v in kinds.items()))

    again = time.time()
    code, out = amb("build", args.rev, *flags)
    check("an identical request returns the same build id at once",
          code == 0 and build_id_of(out) == build_id and time.time() - again < 120,
          f"{build_id_of(out)} in {time.time() - again:.0f} s")

    code, out = amb("e2e", build_id, "--wait", "--", "-v", "suites/test_smoke.py")
    check("e2e on the build passes", code == 0, out.strip().splitlines()[-1] if out.strip() else "")

    code, out = amb("fetch", build_id)
    target = Path(r"Q:\buildserver\out") / build_id / kinds.get("portable-zip", {}).get("name", "?")
    check("fetch copies the portable zip to Q: and checks its SHA-256", code == 0 and target.exists(), str(target))

    left = sitectl.in_distro("podman ps -a --format '{{.Names}}' 2>/dev/null | grep -c '^amb-slot-' || true\n"
                             "ls /srv/buildserver/vms/\n", timeout=60).stdout.decode().split()
    check("no build slot is left; the kept build VM stays", left[:1] == ["0"] and "build-1" in left[1:], " ".join(left))

    after = source_marks()
    check("K: was only read (no worktree added, no FETCH_HEAD written in the source repository)",
          after == marks, "unchanged" if after == marks else f"before {marks}, after {after}")

    print("\nall checks passed" if not FAILED else f"\n{len(FAILED)} check(s) failed: {', '.join(FAILED)}")
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
