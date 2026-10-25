"""The build server's chain on this PC, without a VM: coordinator + guest agent + host facts, end to end.

Starts the coordinator (its Debug build) on 127.0.0.1 with the `local` slot driver, so a "slot" is the real guest agent
started as a process on this machine, then checks through the real HTTP API:

1. /healthz answers, and an API call without the admin token is refused (401);
2. a smoke request with its own steps runs them in the "slot" (run, fetch with a SHA-256 check, put, expand) and ends
   Succeeded, with the steps' events in its log; a shutdown step is skipped by the local agent (it never shuts the PC
   down);
3. an import of a portable zip makes a build id whose manifest carries the zip's SHA-256 and .am-version;
4. a cancel of a running smoke request ends it Cancelled and kills its step's process;
5. a request that can never fit ends no-capacity after its wait timeout, naming memory.

Everything it makes lives under Q:\\tmp\\buildserver-test (deleted first and at the end). Nothing touches the site
distro, a VM or any Agentmaster instance.

    python buildserver/tests/local_chain.py [--zip <a portable zip to import>]

Exit codes: 0 every check passed; 1 a check failed (the output says which).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
BUILD = Path(r"Q:\tmp\buildserver-build")
ROOT = Path(r"Q:\tmp\buildserver-test")
PORT = 2297
BASE = f"http://127.0.0.1:{PORT}"


def call(method: str, path: str, token: str | None, body: object | None = None, timeout: float = 40) -> tuple[int, object]:
    """Calls the coordinator's API.

    Args:
        method: the HTTP method.
        path: the path, e.g. "/api/v1/requests".
        token: the admin token, or None for none.
        body: a JSON body, or None.
        timeout: seconds before the call is abandoned.

    Returns:
        (status, parsed JSON or text); status 0 when the connection failed.
    """
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(BASE + path, data=data, method=method)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            status = response.status
    except urllib.error.HTTPError as exc:
        raw, status = exc.read(), exc.code
    except OSError:
        return 0, None
    try:
        return status, json.loads(raw) if raw else None
    except json.JSONDecodeError:
        return status, raw.decode("utf-8", "replace")


def wait_final(token: str, request_id: str, limit_s: float) -> dict:
    """Follows a request's events until it ends.

    Args:
        token: the admin token.
        request_id: the request.
        limit_s: the longest wait.

    Returns:
        The final record, with the events seen under "_events".

    Raises:
        TimeoutError: it did not end in time.
    """
    skip, events, deadline = 0, [], time.monotonic() + limit_s
    while time.monotonic() < deadline:
        status, page = call("GET", f"/api/v1/requests/{request_id}/events?skip={skip}&wait=20", token)
        if status != 200:
            raise RuntimeError(f"events: HTTP {status}: {page}")
        events += page["events"]
        skip = page["next"]
        record = page["request"]
        if record["state"] in ("Succeeded", "Failed", "Cancelled", "NoCapacity", "Lost"):
            record["_events"] = events
            return record
    raise TimeoutError(f"{request_id} did not end within {limit_s} s")


def main(argv: list[str]) -> int:
    """Runs every check.

    Args:
        argv: the arguments after the program name.

    Returns:
        0 when all passed, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--zip", type=Path, default=None, help="a portable zip to import (default: a small made-up one)")
    args = parser.parse_args(argv)
    coordinator = BUILD / "Amb.Coordinator" / "bin" / "Debug" / "net10.0" / "amb-coordinator.exe"
    agent = BUILD / "Amb.GuestAgent" / "bin" / "Debug" / "net10.0-windows" / "win-x64" / "amb-guest.exe"
    hostfacts = BUILD / "Amb.HostFacts" / "bin" / "Debug" / "net10.0-windows" / "win-x64" / "amb-hostfacts.exe"
    for exe in (coordinator, agent, hostfacts):
        if not exe.exists():
            print(f"missing {exe}: run `dotnet build {SRC / 'Buildserver.slnx'}` first")
            return 1
    shutil.rmtree(ROOT, ignore_errors=True)
    ROOT.mkdir(parents=True)
    env = dict(os.environ, AMB_ROOT=str(ROOT / "root"), AMB_SLOT_DRIVER="local", AMB_LISTEN=BASE, AMB_GUEST_URL=BASE,
               AMB_HOSTFACTS=str(hostfacts), AMB_GUEST_AGENT=str(agent), AMB_IDLE_EXIT_MINUTES="0")
    log = open(ROOT / "coordinator.log", "w", encoding="utf-8")
    process = subprocess.Popen([str(coordinator)], env=env, stdout=log, stderr=subprocess.STDOUT)
    failures: list[str] = []

    def check(name: str, ok: bool, detail: object = "") -> None:
        """Records one check's outcome and prints it."""
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail != "" else ""))
        if not ok:
            failures.append(name)

    try:
        deadline = time.monotonic() + 30
        while call("GET", "/healthz", None)[0] != 200:
            if time.monotonic() > deadline or process.poll() is not None:
                check("coordinator starts", False, (ROOT / "coordinator.log").read_text(encoding="utf-8")[-2000:])
                return 1
            time.sleep(0.2)
        check("coordinator starts", True)
        token = (ROOT / "root" / "coordinator" / "admin.token").read_text().strip()
        check("an API call without the token is refused", call("GET", "/api/v1/status", None)[0] == 401)
        # Host facts are read every 10 s; the first read must have arrived before a slot can be admitted.
        deadline = time.monotonic() + 30
        while (call("GET", "/api/v1/status", token)[1] or {}).get("hostFacts") is None and time.monotonic() < deadline:
            time.sleep(0.5)
        status = call("GET", "/api/v1/status", token)[1]
        check("host facts read through amb-hostfacts", status.get("hostFacts") is not None, status.get("hostFacts"))

        # A tiny file and a tiny zip in the slot's share: fetch it (SHA-256 checked), expand the zip, put a file back.
        payload = ROOT / "payload.txt"
        payload.write_text("payload of the local chain test\n", encoding="utf-8")
        digest = hashlib.sha256(payload.read_bytes()).hexdigest()
        small_zip = ROOT / "small.zip"
        with zipfile.ZipFile(small_zip, "w") as z:
            z.writestr("inner/hello.txt", "hello from the zip\n")
        tiny = {"min": {"memoryMiB": 64, "vcpus": 1, "diskMiB": 0}, "preferred": {"memoryMiB": 128, "vcpus": 1, "diskMiB": 0}}
        steps = [
            {"id": "1", "kind": "run", "command": "cmd.exe", "args": ["/c", "echo local chain says hi"], "timeoutS": 60},
            {"id": "2", "kind": "fetch", "from": "STAGE/in/payload.txt", "to": str(ROOT / "work" / "payload.txt"),
             "sha256": digest, "timeoutS": 60},
            {"id": "3", "kind": "expand", "from": "STAGE/in/small.zip", "to": str(ROOT / "work" / "unzipped")},
            {"id": "4", "kind": "put", "from": str(ROOT / "work" / "payload.txt"), "to": str(ROOT / "work" / "out" / "back.txt"),
             "timeoutS": 60},
        ]
        # The local slot's share is AMB_ROOT/exchange/<slot>; the steps cannot know the slot id in advance, so they
        # read from a fixed staging folder instead: copy the files where the steps expect them.
        stage_dir = ROOT / "stage" / "in"
        stage_dir.mkdir(parents=True)
        shutil.copy(payload, stage_dir / "payload.txt")
        shutil.copy(small_zip, stage_dir / "small.zip")
        for step in steps:
            for key in ("from", "to"):
                if key in step and step[key].startswith("STAGE/"):
                    step[key] = str(ROOT / "stage" / step[key][len("STAGE/"):])
        status, created = call("POST", "/api/v1/requests", token, {"kind": "Smoke", "inputs": {"steps": steps},
                                                                     "resources": tiny})
        check("a smoke request is accepted", status == 202, created if status != 202 else created["id"])
        if status == 202:
            final = wait_final(token, created["id"], 120)
            check("the smoke request succeeds", final["state"] == "Succeeded", final.get("outcome"))
            check("its steps ran in the slot", (ROOT / "work" / "unzipped" / "inner" / "hello.txt").exists()
                  and (ROOT / "work" / "out" / "back.txt").read_text(encoding="utf-8").startswith("payload"))
            lines = [e["message"] for e in final["_events"]]
            check("its log has the guest's hello and every step",
                  any("guest ready after" in m for m in lines) and sum(1 for m in lines if m.startswith("step ")) >= 8,
                  len(lines))
            check("the local agent skipped the shutdown step (it never shuts the PC down)",
                  any("skipped: the slot does not allow a shutdown" in m for m in lines))

        # An import: a portable zip with an .am-version stamp in its top folder, like the release zips.
        source = args.zip
        if source is None:
            source = ROOT / "Agentmaster_9.9.9.9_x64.zip"
            with zipfile.ZipFile(source, "w") as z:
                z.writestr("agentmaster-9.9.9.9/.am-version", "9.9.9.9\n")
                z.writestr("agentmaster-9.9.9.9/Agentmaster.exe", b"MZ" + b"\0" * 1024)
        status, created = call("POST", "/api/v1/requests", token, {"kind": "Import", "inputs": {"source": str(source)}})
        check("an import is accepted", status == 202, created if status != 202 else created["id"])
        if status == 202:
            final = wait_final(token, created["id"], 300)
            check("the import succeeds", final["state"] == "Succeeded", final.get("outcome"))
            build_id = final.get("buildId")
            status, build = call("GET", f"/api/v1/builds/{build_id}", token)
            want = hashlib.sha256(source.read_bytes()).hexdigest()
            check("its manifest carries the zip's SHA-256 and version", status == 200
                  and build["artifacts"][0]["sha256"] == want and build.get("version") not in (None, ""),
                  f"{build_id} {build.get('version') if status == 200 else build}")

        # A cancel: a step that would run for 5 minutes, cancelled after it started.
        long_steps = [{"id": "1", "kind": "run", "command": "powershell.exe",
                       "args": ["-NoProfile", "-Command", "Start-Sleep -Seconds 300"], "timeoutS": 600}]
        status, created = call("POST", "/api/v1/requests", token, {"kind": "Smoke", "inputs": {"steps": long_steps},
                                                                     "resources": tiny})
        if status == 202:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                record = call("GET", f"/api/v1/requests/{created['id']}", token)[1]
                if record["state"] == "Running":
                    break
                time.sleep(0.3)
            time.sleep(2)
            check("a running request can be cancelled", call("POST", f"/api/v1/requests/{created['id']}/cancel", token)[0] == 202)
            final = wait_final(token, created["id"], 120)
            check("it ends Cancelled", final["state"] == "Cancelled", final.get("outcome"))
            sleepers = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                                       "(Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | "
                                       "Where-Object { $_.CommandLine -like '*Start-Sleep -Seconds 300*' }).Count"],
                                      capture_output=True, text=True).stdout.strip()
            check("its step's process was killed", sleepers in ("", "0"), sleepers)

        # No capacity: 1 TB of memory never fits; a one-minute wait timeout ends it.
        huge = {"min": {"memoryMiB": 1 << 20, "vcpus": 1, "diskMiB": 0},
                "preferred": {"memoryMiB": 1 << 20, "vcpus": 1, "diskMiB": 0}}
        status, created = call("POST", "/api/v1/requests", token, {"kind": "Smoke", "resources": huge,
                                                                     "waitTimeoutMinutes": 1})
        if status == 202:
            final = wait_final(token, created["id"], 120)
            check("an impossible request ends NoCapacity naming memory", final["state"] == "NoCapacity"
                  and "memory" in (final.get("outcome") or ""), final.get("outcome"))
    finally:
        process.terminate()
        try:
            process.wait(15)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
    print(f"\n{'all checks passed' if not failures else str(len(failures)) + ' failed: ' + ', '.join(failures)}")
    if not failures:
        shutil.rmtree(ROOT, ignore_errors=True)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
