"""Phase 1's done-when on the REAL site: a slot leaves nothing behind in every outcome (DESIGN.md §13).

Runs four smoke requests against the coordinator in the site distro, each in a fresh VM of an image (default am-base,
the cheapest boot), and checks after each that its slot left no container, no slot folder and no share:

1. passed:    the smoke's own `cmd /c ver`                  -> Succeeded
2. failed:    a step that exits 3                           -> Failed, the step's exit code in the outcome
3. cancelled: a 600 s step, cancelled once it runs          -> Cancelled
4. lost:      a 600 s step, the VM's QEMU killed once it runs -> Lost through the liveness watch, well before the
              3-minute silence limit

Each scenario boots a VM (~45 s on am-base) and they run one after another: ~5 minutes in all. The coordinator must run
(`python buildserver/site/sitectl.py up`); its admin token is read from the site and never printed. Other requests may
run meanwhile: every check looks at this run's own slots only.

    python buildserver/tests/site_outcomes.py [--image am-base] [--only passed,failed,cancelled,lost]

Exit codes: 0 every check passed; 1 a check failed (the output says which); 2 no coordinator.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "buildserver" / "image"))
sys.path.insert(0, str(REPO / "buildserver" / "site"))
import images  # noqa: E402  (the coordinator client and the final states)
import sitectl  # noqa: E402  (the admin token, commands in the distro)

LONG_STEP = {"id": "long", "kind": "run", "command": "ping.exe", "args": ["-n", "600", "127.0.0.1"], "timeoutS": 900}
"""A step that runs ten minutes and does nothing: the target of the cancel and of the killed VM."""

FAILED: list[str] = []
"""The names of the checks that failed."""


def check(name: str, ok: bool, detail: str = "") -> bool:
    """Prints one check's result and remembers a failure.

    Args:
        name: what is checked.
        ok: whether it held.
        detail: what was seen, printed after the name.

    Returns:
        ok, so a scenario can stop at a failed check.
    """
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""), flush=True)
    if not ok:
        FAILED.append(name)
    return ok


def submit(client: images.Coordinator, image: str, steps: list[dict] | None) -> str:
    """Submits one smoke request.

    Args:
        client: the coordinator client.
        image: the image to boot.
        steps: the steps to run instead of the smoke's `ver`, or None.

    Returns:
        The request's id.

    Raises:
        RuntimeError: the coordinator refused it.
    """
    inputs: dict[str, object] = {"image": image}
    if steps is not None:
        inputs["steps"] = steps
    status, created = client.call("POST", "/api/v1/requests", {"kind": "Smoke", "inputs": inputs})
    if status != 202:
        raise RuntimeError(f"refused ({status}): {created}")
    return created["id"]


def wait_for_event(client: images.Coordinator, request_id: str, prefix: str, limit_s: float) -> dict | None:
    """Follows a request's events (the coordinator's long poll) until one starts with a text.

    Args:
        client: the coordinator client.
        request_id: the request.
        prefix: the event message's start, e.g. "step long: run".
        limit_s: the longest wait.

    Returns:
        The request's record when the event came, or None when the time ran out or the request ended first.
    """
    deadline, skip = time.time() + limit_s, 0
    while time.time() < deadline:
        status, page = client.call("GET", f"/api/v1/requests/{request_id}/events?skip={skip}&wait=20", timeout=40)
        if status != 200:
            continue
        if any(e["message"].startswith(prefix) for e in page["events"]):
            return page["request"]
        skip = page["next"]
        if page["request"]["state"] in images.FINAL:
            return None
    return None


def wait_final(client: images.Coordinator, request_id: str, limit_s: float) -> dict:
    """Follows a request until it ends.

    Args:
        client: the coordinator client.
        request_id: the request.
        limit_s: the longest wait.

    Returns:
        Its last record (still not final when the time ran out).
    """
    deadline, skip = time.time() + limit_s, 0
    record: dict = {}
    while time.time() < deadline:
        status, page = client.call("GET", f"/api/v1/requests/{request_id}/events?skip={skip}&wait=20", timeout=40)
        if status != 200:
            continue
        record, skip = page["request"], page["next"]
        if record["state"] in images.FINAL:
            break
    return record


def leftovers(slot_id: str | None) -> str:
    """What a slot left in the site: its container, its slot folder, its share.

    Args:
        slot_id: the slot's id (None when the request never got one: nothing can be left).

    Returns:
        "" when nothing is left, else what is.
    """
    if not slot_id:
        return ""
    script = (f"podman ps -a --format '{{{{.Names}}}}' 2>/dev/null | grep -x 'amb-slot-{slot_id}' || true\n"
              f"[ -e /srv/buildserver/slots/{slot_id} ] && echo 'slot folder' || true\n"
              f"[ -e /srv/buildserver/exchange/{slot_id} ] && echo share || true\n")
    return ", ".join(sitectl.in_distro(script, timeout=60).stdout.decode().split())


def kill_vm(slot_id: str) -> bool:
    """Kills a slot's QEMU inside its container, as a crashed VM would end.

    Args:
        slot_id: the slot.

    Returns:
        True when the kill was delivered.
    """
    done = sitectl.in_distro(f"podman exec amb-slot-{slot_id} sh -c 'kill -9 $(cat /run/shm/qemu.pid)'\n",
                             timeout=60, check=False)
    return done.returncode == 0


def scenario(client: images.Coordinator, name: str, image: str) -> None:
    """Runs one outcome and checks it.

    Args:
        client: the coordinator client.
        name: "passed", "failed", "cancelled" or "lost".
        image: the image to boot.
    """
    steps = {"passed": None,
             "failed": [{"id": "fails", "kind": "run", "command": "cmd.exe", "args": ["/c", "exit 3"], "timeoutS": 60}],
             "cancelled": [LONG_STEP], "lost": [LONG_STEP]}[name]
    started = time.time()
    request_id = submit(client, image, steps)
    acted_at = None
    if name in ("cancelled", "lost"):
        record = wait_for_event(client, request_id, "step long: run", 300)
        if not check(f"{name}: the long step started", record is not None, request_id):
            client.call("POST", f"/api/v1/requests/{request_id}/cancel")
            wait_final(client, request_id, 120)
            return
        time.sleep(3)  # a pause so the step surely runs in the guest, not a wait for a condition
        acted_at = time.time()
        if name == "cancelled":
            status, _ = client.call("POST", f"/api/v1/requests/{request_id}/cancel")
            check("cancelled: the cancel was accepted", status in (200, 202), f"HTTP {status}")
        else:
            check("lost: the VM's QEMU was killed", kill_vm(record["slotId"]))
    final = wait_final(client, request_id, 600)
    expected = {"passed": "Succeeded", "failed": "Failed", "cancelled": "Cancelled", "lost": "Lost"}[name]
    took = time.time() - (acted_at or started)
    check(f"{name}: the request ends {expected}", final.get("state") == expected,
          f"{final.get('state')} {took:.0f} s {'after the act' if acted_at else 'in all'}: {final.get('outcome')}")
    if name == "failed":
        check("failed: the outcome names the step's exit code", "exit code 3" in (final.get("outcome") or ""))
    if name == "lost":
        check("lost: the liveness watch gave it up before the silence limit", took < 120, f"{took:.0f} s")
    left = leftovers(final.get("slotId"))
    check(f"{name}: its slot left nothing behind", left == "", left or "no container, slot folder or share")


def main(argv: list[str]) -> int:
    """Parses the command line and runs the scenarios.

    Args:
        argv: the arguments after the program name.

    Returns:
        The exit code.
    """
    parser = argparse.ArgumentParser(description="Phase 1's outcomes on the real site.")
    parser.add_argument("--image", default="am-base")
    parser.add_argument("--only", default="passed,failed,cancelled,lost")
    args = parser.parse_args(argv)
    try:
        client = images.Coordinator(sitectl.admin_token())
    except sitectl.SiteError as exc:
        print(f"site_outcomes: {exc}", file=sys.stderr)
        return 2
    for name in [n.strip() for n in args.only.split(",") if n.strip()]:
        scenario(client, name, args.image)
    print("\nall checks passed" if not FAILED else f"\n{len(FAILED)} check(s) failed: {', '.join(FAILED)}")
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
