"""Proof P0.1 (DESIGN.md §13): does fastrund run in the site distro, on WSL's kernel, with no Kubernetes?

Runs the PRODUCTION fastrund image (the one dock's DaemonSet runs) as a privileged podman container in the
`buildserver` distro, the way the DaemonSet runs it (host network, read-only root, a memory-backed /run/fastrun, the
store and data folders as volumes), with releases switched off (no ServiceAccount exists here, so releases could only
be read from a cache). Then it measures, from Windows:

- the time from `podman run` to `/healthz` answering, from Windows through the distro's own address AND through
  WSL's localhost forwarding (the first try, 2026-10-25, found that forwarding passes only IPv4 listeners, and
  fastrund listens on the IPv6 wildcard `:::2200`, so only the distro's address works);
- the sockets fastrund listens on;
- ad-hoc script runs: the first (its runtime image pulled and unpacked) and nine more (warm);
- what the node reports about itself (runtimes, the spawner, cgroups);
- fastrund's idle CPU over 30 s and its resident memory.

The registry is reached over plain HTTP through Traefik (`http://docker-registry.local`, the ingress of the registry
on dock's nodes), the way production pulls in-cluster: the first try found that the static fastrund image cannot
complete a TLS handshake with `https://docker-registry.elibelash.com` from here ("The SSL connection could not be
established", with or without SSL_CERT_FILE), while curl in the same distro verifies that certificate fine.

Afterwards the container is stopped (SIGTERM: fastrund removes its own slots) and removed; the image stays cached in
the distro for the work that follows. The registry credentials are written once into the distro's root-only folder
(`/srv/buildserver/fastrun/regcreds`, the place the site's fastrund will read them from), taken from Docker Desktop's
credential store; neither they nor the spike's token are ever printed.

    python buildserver/measure/p01_fastrund.py [--out buildserver/measure/results-2026-10-25/p01-fastrund.json]

Exit codes: 0 every check passed; 1 a check failed (the result file says which); 2 the proof could not start.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "site"))
import sitectl as _site  # noqa: E402  (buildserver/site/sitectl.py: the site's own helpers)

REGISTRY = "docker-registry.elibelash.com"
"""The homelab registry that holds fastrund's image and the runtime images its slots run."""

IMAGE = f"{REGISTRY}/fastrund:20261012-2"
"""Production's fastrund image (deploy/k8s/fastrun.yaml in K:\\source\\fastrun, 2026-10-12)."""

CONTAINER = "fastrund-p01"
"""The spike's container name; distinct from the site's real node so the two can never be confused."""

API_PORT, GRPC_PORT, LANE_PORT = 2200, 2201, 2202
"""Fixed ports of this PC's node: fastrun's free range 2200-2499 (its CLAUDE.md rule 5), apart from prod and next."""

AUTH_FILE = "/srv/buildserver/fastrun/regcreds/.dockerconfigjson"
"""Where the site keeps the registry credentials (a Docker config.json, mode 0600, its folder 0700)."""

ENV_FILE = "/srv/buildserver/fastrun/p01.env"
"""The spike's environment file inside the distro (mode 0600): it carries the token, so `ps` never shows it."""

REGISTRY_ENDPOINT = f"{REGISTRY}=http://docker-registry.local"
"""FASTRUN_REGISTRY_ENDPOINTS: the registry's plain-HTTP ingress, reachable from the distro (401 without auth)."""


def distro_address() -> str:
    """Finds the site distro's IPv4 address on WSL's NAT network.

    It changes at every start of WSL's VM, so callers ask for it each time instead of keeping it.

    Returns:
        The address of eth0, e.g. "172.23.87.90".

    Raises:
        SiteError: the distro did not answer, or eth0 has no IPv4 address.
    """
    out = _site.in_distro("ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1\n",
                          timeout=30).stdout.decode().strip()
    if not out:
        raise _site.SiteError("eth0 of the site distro has no IPv4 address")
    return out.splitlines()[0]


def registry_credentials() -> tuple[str, str]:
    """Reads the registry's user and secret from Docker Desktop's credential store.

    Returns:
        (username, secret).

    Raises:
        RuntimeError: the helper failed or returned no credentials (log in with `docker login` first).
    """
    helper = r"C:\Program Files\Docker\Docker\resources\bin\docker-credential-desktop.exe"
    done = subprocess.run([helper, "get"], input=REGISTRY.encode(), capture_output=True, timeout=30, check=False)
    if done.returncode != 0:
        raise RuntimeError(f"docker-credential-desktop get failed ({done.returncode}); run `docker login {REGISTRY}`")
    data = json.loads(done.stdout)
    if not data.get("Username") or not data.get("Secret"):
        raise RuntimeError("the credential store holds no credentials for the registry")
    return data["Username"], data["Secret"]


def write_auth_file(user: str, secret: str) -> None:
    """Writes the registry credentials into the distro as a Docker config.json readable by root only.

    Args:
        user: the registry user.
        secret: the registry password or token; it travels on wsl.exe's stdin and is never echoed.

    Raises:
        SiteError: writing the file failed.
    """
    auth = base64.b64encode(f"{user}:{secret}".encode()).decode()
    config = json.dumps({"auths": {REGISTRY: {"auth": auth}}})
    # umask first, so the file is never readable by others even for an instant.
    _site.in_distro(f"umask 077\nprintf '%s' '{config}' > {AUTH_FILE}.tmp\nmv -f {AUTH_FILE}.tmp {AUTH_FILE}\n",
                    timeout=60)


def http(method: str, url: str, token: str | None = None, body: bytes | None = None,
         timeout: float = 30.0) -> tuple[int, bytes, float]:
    """Sends one HTTP request from Windows.

    Args:
        method: the HTTP method.
        url: the full URL.
        token: a bearer token for the Authorization header, or None.
        body: the request body, or None.
        timeout: seconds before the request is abandoned.

    Returns:
        (status, body, seconds taken); status 0 when the connection failed.
    """
    request = urllib.request.Request(url, data=body, method=method)
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    if body is not None:
        request.add_header("Content-Type", "application/octet-stream" if method == "PUT" else "application/json")
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read(), time.perf_counter() - t0
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read(), time.perf_counter() - t0
    except OSError:
        return 0, b"", time.perf_counter() - t0


def redact(text: str, *secrets_: str) -> str:
    """Removes secrets from a text before it is stored.

    Args:
        text: the text.
        secrets_: the values to remove.

    Returns:
        The text with every secret replaced by "<redacted>".
    """
    for value in secrets_:
        if value:
            text = text.replace(value, "<redacted>")
    return text


def main(argv: list[str]) -> int:
    """Runs the proof and writes its result file.

    Args:
        argv: the arguments after the program name.

    Returns:
        0 when every check passed, 1 when one failed, 2 when the proof could not start.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path,
                        default=Path(__file__).resolve().parent / "results-2026-10-25" / "p01-fastrund.json")
    args = parser.parse_args(argv)
    result: dict[str, object] = {"image": IMAGE, "checks": {}}
    checks: dict[str, bool] = result["checks"]  # type: ignore[assignment]
    token = secrets.token_hex(32)
    try:
        user, secret = registry_credentials()
        write_auth_file(user, secret)
        t0 = time.monotonic()
        _site.in_distro(f"export PODMAN_IGNORE_CGROUPSV1_WARNING=1\npodman pull -q --authfile {AUTH_FILE} {IMAGE} >/dev/null\n",
                        timeout=600)
        result["pull_s"] = round(time.monotonic() - t0, 2)
    except Exception as exc:  # noqa: BLE001 - any failure here means the proof cannot run at all
        print(f"p01: cannot start: {exc}", file=sys.stderr)
        return 2

    env_lines = [
        "NODE_NAME=homepc",
        "NODE_IP=127.0.0.1",
        f"FASTRUN_TOKEN={token}",
        "FASTRUN_RELEASES=0",
        f"FASTRUN_API_PORT={API_PORT}",
        f"FASTRUN_GRPC_PORT={GRPC_PORT}",
        f"FASTRUN_LANE_PORT={LANE_PORT}",
        "FASTRUN_HOME_NODES=homepc",
        f"FASTRUN_REGISTRY_ENDPOINTS={REGISTRY_ENDPOINT}",
    ]
    start_script = "\n".join([
        "export PODMAN_IGNORE_CGROUPSV1_WARNING=1",
        f"podman rm -f {CONTAINER} >/dev/null 2>&1 || true",
        "umask 077",
        f"cat > {ENV_FILE} <<'AMB_ENV_END'\n" + "\n".join(env_lines) + "\nAMB_ENV_END",
        # The DaemonSet's shape: privileged, host network, read-only root, /run/fastrun in memory, store and data on
        # disk. The lane's socket folder and the registry credentials are the site's own folders.
        f"podman run -d --name {CONTAINER} --privileged --network host --read-only "
        "--tmpfs /run/fastrun:rw,size=256m,mode=0755 "
        "-v /srv/buildserver/fastrun/store:/var/lib/fastrun-store "
        "-v /srv/buildserver/fastrun/data:/var/lib/fastrun "
        "-v /srv/buildserver/fastrun/scratch:/var/lib/fastrun-scratch "
        "-v /srv/buildserver/fastrun/lane:/run/fastrun-lane "
        "-v /srv/buildserver/fastrun/regcreds:/etc/fastrun/regcreds:ro "
        f"--env-file {ENV_FILE} {IMAGE} >/dev/null",
    ])
    address = distro_address()
    result["distro_address"] = address
    t_start = time.monotonic()
    _site.in_distro(start_script, timeout=120)
    base = f"http://{address}:{API_PORT}"
    healthy_after = None
    while time.monotonic() - t_start < 90:
        status, _, _ = http("GET", base + "/healthz", timeout=2)
        if status == 200:
            healthy_after = round(time.monotonic() - t_start, 2)
            break
        time.sleep(0.1)
    result["healthz_from_windows_s"] = healthy_after
    checks["healthz from Windows (the distro's address)"] = healthy_after is not None
    # Recorded, not required: localhost forwarding is a convenience the site does not depend on.
    status, _, _ = http("GET", f"http://127.0.0.1:{API_PORT}/healthz", timeout=3)
    result["healthz_via_windows_localhost"] = status
    result["listeners"] = _site.in_distro("netstat -ltn | awk 'NR>2 && $4 ~ /:22(00|01|02)$/ {print $4}'\n",
                                          timeout=30, check=False).stdout.decode().split()

    try:
        if healthy_after is not None:
            status, body, _ = http("GET", base + "/api/v1/node", token)
            result["node_status_http"] = status
            if status == 200:
                result["node"] = json.loads(body)
            # One ad-hoc script: what the slot sees of the kernel and its cgroup.
            script = b"#!/bin/sh\necho hello from homepc\nuname -r\nhead -c 300 /proc/self/cgroup\n"
            digest = hashlib.sha256(script).hexdigest()
            status, body, put_s = http("PUT", f"{base}/api/v1/artifacts/{digest}?kind=script", token, script)
            result["artifact_put"] = {"status": status, "s": round(put_s, 4)}
            runs: list[dict[str, object]] = []
            request = json.dumps({"adhoc": {"kind": "KIND_SCRIPT", "digestHex": digest}}).encode()
            # The first run waits for the runtime: fastrund pulls and unpacks the sh image after its start, and until
            # then a run is refused. Retried for up to 3 minutes; how long it took is the cold-start cost of a node.
            t_ready = time.monotonic()
            while True:
                status, body, seconds = http("POST", f"{base}/api/v1/runs", token, request, timeout=120)
                if status == 200 or time.monotonic() - t_ready > 180:
                    break
                time.sleep(1)
            result["first_run_ready_s"] = round(time.monotonic() - t_start, 2)
            for i in range(10):
                if i > 0:
                    status, body, seconds = http("POST", f"{base}/api/v1/runs", token, request, timeout=120)
                run: dict[str, object] = {"n": i + 1, "status": status, "client_s": round(seconds, 4)}
                if status == 200:
                    answer = json.loads(body)
                    run["exit_code"] = answer.get("exitCode", 0)
                    run["timings"] = answer.get("timings")
                    if i == 0:
                        run["stdout"] = base64.b64decode(answer.get("stdout", "")).decode("utf-8", "replace")
                        run["stderr"] = base64.b64decode(answer.get("stderr", "")).decode("utf-8", "replace")[:2000]
                else:
                    run["body"] = body.decode("utf-8", "replace")[:2000]
                runs.append(run)
            result["adhoc_runs"] = runs
            checks["ad-hoc script runs"] = all(r["status"] == 200 and r.get("exit_code", 1) == 0 for r in runs)

            # Idle cost: fastrund's own CPU ticks over 30 s with nothing asked of it, and its resident memory.
            idle = _site.in_distro(
                f"pid=$(podman inspect -f '{{{{.State.Pid}}}}' {CONTAINER})\n"
                "t1=$(awk '{print $14+$15}' /proc/$pid/stat); sleep 30; t2=$(awk '{print $14+$15}' /proc/$pid/stat)\n"
                "echo ticks=$((t2-t1)) hz=$(getconf CLK_TCK) rss_kb=$(awk '/VmRSS/{print $2}' /proc/$pid/status)\n",
                timeout=90, check=False).stdout.decode("utf-8", "replace")
            facts = dict(part.split("=", 1) for part in idle.split() if "=" in part)
            if "ticks" in facts and "hz" in facts:
                ms_per_s = int(facts["ticks"]) * 1000 / int(facts["hz"]) / 30
                result["idle"] = {"cpu_ms_per_s": round(ms_per_s, 3), "rss_mb": round(int(facts.get("rss_kb", 0)) / 1024, 1)}
    finally:
        logs = _site.in_distro(f"export PODMAN_IGNORE_CGROUPSV1_WARNING=1\npodman logs {CONTAINER} 2>&1 | tail -n 80\n",
                               timeout=60, check=False).stdout.decode("utf-8", "replace")
        result["log_tail"] = redact(logs, token).splitlines()
        t_stop = time.monotonic()
        _site.in_distro(f"export PODMAN_IGNORE_CGROUPSV1_WARNING=1\npodman stop -t 20 {CONTAINER} >/dev/null\n"
                        f"podman rm {CONTAINER} >/dev/null\nrm -f {ENV_FILE}\n", timeout=120, check=False)
        result["stop_s"] = round(time.monotonic() - t_stop, 2)

    result["passed"] = all(checks.values()) and bool(checks)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(redact(json.dumps(result, indent=2), token), encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "checks": checks, "healthz_s": healthy_after,
                      "pull_s": result.get("pull_s"), "idle": result.get("idle"), "out": str(args.out)}, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
