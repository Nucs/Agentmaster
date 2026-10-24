"""A fake GitHub for the install lane's `net` machine: api.github.com, github.com and raw.githubusercontent.com served
from 127.0.0.1:443 inside a disposable test desktop, so the in-app updater and the installer script meet releases a
test publishes (flows/19 IS-11, IS-12, IS-80–IS-82, IS-87).

Why a fake and not the real releases: the published releases change, cannot be made to refuse a token, rate-limit or
offer a version "newer than" the build under test, and the lane's guest must not depend on the internet. Both programs
under test reach GitHub only through names the guest resolves (its hosts file) and certificates it trusts (its machine
root store), so a disposable desktop can stand in for GitHub with no change to them:

* the in-app updater (Updater.h): WinHTTP to api.github.com/repos/Nucs/Agentmaster/releases[...] (no revocation check),
  then am-update.ps1 downloads the asset's browser_download_url (Invoke-WebRequest), which IsTrustedAssetUrl only
  accepts under https://github.com/Nucs/Agentmaster/releases/download/;
* the installer (Install-Agentmaster.ps1): Invoke-RestMethod to the same API, Invoke-WebRequest for the asset, and
  `irm https://raw.githubusercontent.com/…/Install-Agentmaster.ps1` for the README's one line.

**The certificates never leave the guest.** `FakeGitHub.start` runs a PowerShell 7 script (.NET's CertificateRequest)
that makes a CA and a server certificate for the four GitHub names IN MEMORY: the CA's private key is never written
anywhere and is gone when the script ends, so nothing can mint another certificate with it; only the CA's public
certificate goes into the guest's LocalMachine\\Root, and `stop` takes it out again (the desktop is reset after the
scenario anyway). The install lane's leak guard already points api.github.com at 127.0.0.1 (provision.ps1); `start`
adds the other three names in a marked hosts block and `stop` removes it.

What the server answers (the parts the two programs read — Updater.h ParseReleaseObj and Install-Agentmaster.ps1):
    GET api.github.com/repos/<owner>/<repo>/releases[?…]        the published releases, newest first, no drafts
    GET api.github.com/repos/<owner>/<repo>/releases/latest     the newest that is neither a draft nor a prerelease
    GET api.github.com/repos/<owner>/<repo>/releases/tags/<tag> one release (404 like GitHub's when unknown)
    GET github.com/<owner>/<repo>/releases/download/<tag>/<name> an asset's bytes
    GET raw.githubusercontent.com/<path>                         a file published with serve_raw
A token listed with refuse_tokens is answered 401 "Bad credentials"; rate_limited() answers every API call 403 with
GitHub's rate-limit headers. Every request is logged (requests()): the oracle of what the program under test asked.

The server is this same file run as a script in its own process: `python fakegithub.py serve <folder>`. It reads the
folder's state.json on every request, so a test publishes and changes rules without restarting it.
"""
from __future__ import annotations

import hashlib
import http.server
import json
import os
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional

HOSTS_FILE = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "drivers" / "etc" / "hosts"
"""The guest's hosts file."""
HOSTS_MARK_BEGIN = "# agentmaster-e2e fake GitHub (begin)"
"""The first line of the hosts block start() writes; stop() removes everything from it to HOSTS_MARK_END."""
HOSTS_MARK_END = "# agentmaster-e2e fake GitHub (end)"
"""The last line of that block."""
NAMES = ("api.github.com", "github.com", "raw.githubusercontent.com", "objects.githubusercontent.com")
"""The names the server certificate carries and the hosts block points at 127.0.0.1."""
CA_SUBJECT = "CN=agentmaster-e2e fake GitHub CA (test desktops only)"
"""The test CA's subject — how stop() finds the CA it trusted (and only that one) in LocalMachine\\Root."""
REPO = "Nucs/Agentmaster"
"""The repository both programs under test name (Updater.h kRepo, the installer's -Repo default)."""

_CERT_SCRIPT = r"""
# Make the fake GitHub's certificates in memory (PowerShell 7, .NET CertificateRequest) and write only what the server
# and the trust need: the server certificate chain + its key (PEM) and the CA's public certificate (DER). The CA's
# private key is never exported or written: when this script ends, no further certificate can be signed with it.
param([string]$Out, [string]$CaSubject, [string]$Names)
$ErrorActionPreference = 'Stop'
$X = 'System.Security.Cryptography.X509Certificates'
$sha = [System.Security.Cryptography.HashAlgorithmName]::SHA256
$pad = [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
$now = [DateTimeOffset]::UtcNow
$caKey = [System.Security.Cryptography.RSA]::Create(2048)
$caReq = New-Object "$X.CertificateRequest" -ArgumentList $CaSubject, $caKey, $sha, $pad
$caReq.CertificateExtensions.Add((New-Object "$X.X509BasicConstraintsExtension" -ArgumentList $true, $true, 0, $true))
$caReq.CertificateExtensions.Add((New-Object "$X.X509KeyUsageExtension" -ArgumentList ([System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]'KeyCertSign, CrlSign'), $true))
$caReq.CertificateExtensions.Add((New-Object "$X.X509SubjectKeyIdentifierExtension" -ArgumentList $caReq.PublicKey, $false))
$ca = $caReq.CreateSelfSigned($now.AddDays(-1), $now.AddDays(3))
$key = [System.Security.Cryptography.RSA]::Create(2048)
$req = New-Object "$X.CertificateRequest" -ArgumentList 'CN=github.com', $key, $sha, $pad
$san = New-Object "$X.SubjectAlternativeNameBuilder"
foreach ($n in $Names.Split(',')) { $san.AddDnsName($n) }
$req.CertificateExtensions.Add($san.Build($false))
$req.CertificateExtensions.Add((New-Object "$X.X509BasicConstraintsExtension" -ArgumentList $false, $false, 0, $false))
$req.CertificateExtensions.Add((New-Object "$X.X509KeyUsageExtension" -ArgumentList ([System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]'DigitalSignature, KeyEncipherment'), $true))
$eku = New-Object System.Security.Cryptography.OidCollection
$null = $eku.Add((New-Object System.Security.Cryptography.Oid -ArgumentList '1.3.6.1.5.5.7.3.1'))
$req.CertificateExtensions.Add((New-Object "$X.X509EnhancedKeyUsageExtension" -ArgumentList $eku, $false))
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509AuthorityKeyIdentifierExtension]::CreateFromCertificate($ca, $true, $false))
$serial = [System.Security.Cryptography.RandomNumberGenerator]::GetBytes(16)
$serial[0] = $serial[0] -band 0x7F  # a positive serial number
$leaf = $req.Create($ca, $now.AddDays(-1), $now.AddDays(2), $serial)
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $Out 'server.pem'), $leaf.ExportCertificatePem() + "`n" + $ca.ExportCertificatePem() + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $Out 'server.key'), $key.ExportPkcs8PrivateKeyPem() + "`n", $utf8)
[IO.File]::WriteAllBytes((Join-Path $Out 'ca.cer'), $ca.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Write-Output $ca.Thumbprint
"""


# ---- the test's side -------------------------------------------------------------------------------------------------

class FakeGitHub:
    """One test's fake GitHub: its folder (state, assets, certificates, request log), the server process and the
    machine changes that route the GitHub names to it.

    Attributes:
        folder: Where everything of this fake lives (C:\\e2e\\w\\fakegithub by default).
        pwsh: The PowerShell 7 that makes the certificates (the lane's C:\\e2e\\pwsh\\pwsh.exe).
        thumbprint: The trusted CA's thumbprint while started ("" before).
    """

    def __init__(self, folder: Path, pwsh: str) -> None:
        """Prepare a fake (nothing runs and nothing on the machine changes until start).

        Args:
            folder: Its folder (created by start; an earlier fake's content there is replaced).
            pwsh: The PowerShell 7 executable.
        Returns:
            None.
        """
        self.folder = folder
        self.pwsh = pwsh
        self.thumbprint = ""
        self._server: Optional[subprocess.Popen] = None
        self._state = {"releases": [], "raw": {}, "bad_tokens": [], "rate_limited": False}

    # ---- life cycle -------------------------------------------------------------------------------------------------

    def start(self, timeout_s: float = 60.0) -> "FakeGitHub":
        """Make the certificates, trust the CA, route the names and start the server; wait until it answers.

        Args:
            timeout_s: How long the server may take to answer its first request.
        Returns:
            self.
        Raises:
            RuntimeError: When the certificates cannot be made, the CA cannot be trusted, or the server does not
                answer (its stderr in the message).
            OSError: When the folder or the hosts file cannot be written.
        """
        if self.folder.exists():
            for p in self.folder.iterdir():
                if p.is_file():
                    p.unlink()
        self.folder.mkdir(parents=True, exist_ok=True)
        (self.folder / "assets").mkdir(exist_ok=True)
        self._save()
        script = self.folder / "make-certificates.ps1"
        script.write_text(_CERT_SCRIPT, encoding="utf-8")
        made = subprocess.run([self.pwsh, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                               str(script), "-Out", str(self.folder), "-CaSubject", CA_SUBJECT, "-Names", ",".join(NAMES)],
                              capture_output=True, text=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
        if made.returncode != 0 or not (self.folder / "server.key").exists():
            raise RuntimeError(f"the fake GitHub's certificates could not be made (exit {made.returncode}): "
                               f"{(made.stderr or made.stdout)[-800:]}")
        self.thumbprint = made.stdout.strip().splitlines()[-1].strip()
        trusted = _powershell(f"Import-Certificate -FilePath '{self.folder / 'ca.cer'}' "
                              f"-CertStoreLocation Cert:\\LocalMachine\\Root | Out-Null")
        if trusted.returncode != 0:
            raise RuntimeError(f"the fake GitHub's CA could not be trusted: {trusted.stderr[-600:]}")
        _route_names(True)
        log = open(self.folder / "server.log", "ab")
        self._server = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "serve", str(self.folder)],
                                        stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        log.close()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self._server.poll() is not None:
                raise RuntimeError(f"the fake GitHub's server ended at once (exit {self._server.returncode}): "
                                   f"{(self.folder / 'server.log').read_text(encoding='utf-8', errors='replace')[-800:]}")
            if (self.folder / "listening").exists():
                return self
            time.sleep(0.2)
        raise RuntimeError("the fake GitHub's server did not start listening: "
                           f"{(self.folder / 'server.log').read_text(encoding='utf-8', errors='replace')[-800:]}")

    def stop(self) -> list[str]:
        """End the server and undo every machine change start made (the hosts block, the trusted CA).

        Each undo step runs even when an earlier one failed; what failed is returned, never raised (a teardown).

        Returns:
            The problems, empty when everything was undone.
        """
        problems: list[str] = []
        if self._server is not None and self._server.poll() is None:
            self._server.kill()
            try:
                self._server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                problems.append(f"the server (pid {self._server.pid}) did not end")
        try:
            _route_names(False)
        except OSError as e:
            problems.append(f"the hosts block could not be removed: {e}")
        # Every certificate with the test CA's subject goes: a start that failed after the import left one too.
        removed = _powershell(f"Get-ChildItem Cert:\\LocalMachine\\Root | Where-Object {{ $_.Subject -eq '{CA_SUBJECT}' }} "
                              f"| Remove-Item -Force")
        if removed.returncode != 0:
            problems.append(f"the test CA could not be removed from LocalMachine\\Root: {removed.stderr[-400:]}")
        return problems

    # ---- what it serves ---------------------------------------------------------------------------------------------

    def publish(self, tag: str, assets: dict[str, Path], prerelease: bool = False, draft: bool = False,
                body: str = "", digest: bool = True) -> dict:
        """Publish a release (the newest one: a later publish comes first in the list, as on GitHub).

        Args:
            tag: Its tag ("v0.8.2").
            assets: Asset name -> the local file served for it (copied into the fake's folder now).
            prerelease: Marked prerelease.
            draft: A draft (listed nowhere, as GitHub lists drafts only to the repository's writers).
            body: Its notes.
            digest: Give each asset GitHub's "sha256:<hex>" digest (the newer API does; older answers had none).
        Returns:
            The release object as the API answers it.
        Raises:
            OSError: When an asset cannot be copied.
        """
        entries = []
        for name, source in assets.items():
            stored = self.folder / "assets" / f"{tag}--{name}"
            data = Path(source).read_bytes()
            stored.write_bytes(data)
            entries.append({"name": name, "size": len(data),
                            "browser_download_url": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
                            "digest": f"sha256:{hashlib.sha256(data).hexdigest()}" if digest else None,
                            "_file": str(stored)})
        release = {"tag_name": tag, "name": tag, "prerelease": prerelease, "draft": draft, "body": body,
                   "html_url": f"https://github.com/{REPO}/releases/tag/{tag}",
                   "published_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "assets": entries}
        self._state["releases"].insert(0, release)
        self._save()
        return _public(release)

    def serve_raw(self, path: str, source: Path) -> str:
        """Serve a file on raw.githubusercontent.com.

        Args:
            path: Its path there ("/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1").
            source: The local file (read on every request: the latest content is served).
        Returns:
            Its URL.
        """
        self._state["raw"][path] = str(source)
        self._save()
        return f"https://raw.githubusercontent.com{path}"

    def refuse_tokens(self, *tokens: str) -> None:
        """Answer requests carrying any of these tokens 401 "Bad credentials", as GitHub answers a revoked token.

        Args:
            *tokens: The tokens.
        Returns:
            None.
        """
        self._state["bad_tokens"] = list(tokens)
        self._save()

    def rate_limited(self, on: bool = True) -> None:
        """Answer every API request 403 "API rate limit exceeded" with GitHub's rate-limit headers.

        Args:
            on: Switch it on (True) or off.
        Returns:
            None.
        """
        self._state["rate_limited"] = on
        self._save()

    def requests(self) -> list[dict]:
        """Every request the server answered so far, oldest first.

        Returns:
            One dict each: time, method, host, path, status, token (whether an Authorization header came).
        """
        log = self.folder / "requests.jsonl"
        if not log.exists():
            return []
        return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines() if line.strip()]

    def _save(self) -> None:
        """Write the state the server reads on every request (atomically: a request never reads half a file).

        Returns:
            None.
        Raises:
            OSError: When it cannot be written.
        """
        tmp = self.folder / "state.json.tmp"
        tmp.write_text(json.dumps(self._state, indent=1), encoding="utf-8")
        os.replace(tmp, self.folder / "state.json")


def _public(release: dict) -> dict:
    """A release as the API shows it (without the fake's own bookkeeping).

    Args:
        release: The stored release.
    Returns:
        A copy whose assets have no "_file", and no "digest" key where none was given.
    """
    out = dict(release)
    out["assets"] = [{k: v for k, v in a.items() if k != "_file" and not (k == "digest" and v is None)}
                     for a in release["assets"]]
    return out


def _powershell(command: str) -> subprocess.CompletedProcess:
    """Run one Windows PowerShell command (no window, no profile).

    Args:
        command: The command.
    Returns:
        The completed process (text output).
    """
    return subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True,
                          text=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)


def _route_names(on: bool) -> None:
    """Add (or remove) the hosts block that points the GitHub names at 127.0.0.1. api.github.com may already point
    there (the install lane's leak guard); a second line for it is harmless and goes with the block.

    Args:
        on: Add the block (True) or remove it.
    Returns:
        None.
    Raises:
        OSError: When the hosts file cannot be read or written.
    """
    # latin-1 maps every byte to one character and back: whatever else the file holds is written back unchanged.
    text = HOSTS_FILE.read_text(encoding="latin-1") if HOSTS_FILE.exists() else ""
    lines = text.splitlines()
    if HOSTS_MARK_BEGIN in lines:
        start = lines.index(HOSTS_MARK_BEGIN)
        end = lines.index(HOSTS_MARK_END, start) if HOSTS_MARK_END in lines[start:] else len(lines) - 1
        lines = lines[:start] + lines[end + 1:]
    if on:
        lines += [HOSTS_MARK_BEGIN] + [f"127.0.0.1 {n}" for n in NAMES] + [HOSTS_MARK_END]
    HOSTS_FILE.write_text("\r\n".join(lines) + "\r\n", encoding="latin-1", newline="")
    # Windows caches name lookups (including negative ones): the new routes must apply to the next request.
    subprocess.run(["ipconfig", "/flushdns"], capture_output=True, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)


# ---- the server (this file run as a script) -----------------------------------------------------------------------------

class _Handler(http.server.BaseHTTPRequestHandler):
    """One request: routed by its Host header to the API, the downloads or the raw files of the folder's state."""

    folder: Path = Path(".")
    """The fake's folder (set by serve)."""
    lock = threading.Lock()
    """Serializes the request log's appends."""
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args) -> None:  # noqa: A002 - the base class's parameter name
        """Silence the base class's stderr line per request (the request log has them).

        Args:
            format: The message format.
            *args: Its values.
        Returns:
            None.
        """

    def do_GET(self) -> None:  # noqa: N802 - the base class's dispatch name
        """Answer a GET: the API, an asset download, a raw file, or 404.

        Returns:
            None.
        """
        self._answer(head_only=False)

    def do_HEAD(self) -> None:  # noqa: N802 - the base class's dispatch name
        """Answer a HEAD like the GET, without the body.

        Returns:
            None.
        """
        self._answer(head_only=True)

    def _answer(self, head_only: bool) -> None:
        """Route the request and send the answer; log it whatever happens.

        Args:
            head_only: Send the headers only.
        Returns:
            None.
        """
        host = (self.headers.get("Host") or "").split(":")[0].lower()
        path = self.path.split("?", 1)[0]
        auth = self.headers.get("Authorization") or ""
        status, error = 500, ""
        try:
            state = json.loads((self.folder / "state.json").read_text(encoding="utf-8"))
            if host == "api.github.com":
                status, body, ctype, extra = _api(state, path, auth)
            elif host in ("github.com", "objects.githubusercontent.com"):
                status, body, ctype, extra = _download(state, path)
            elif host == "raw.githubusercontent.com":
                raw = state["raw"].get(path)
                status, body, ctype, extra = (200, Path(raw).read_bytes(), "text/plain; charset=utf-8", {}) if raw \
                    else (404, b"404: Not Found", "text/plain", {})
            else:
                status, body, ctype, extra = 404, b"unknown host", "text/plain", {}
        except Exception as e:  # noqa: BLE001 - a server answers every request; the failure goes into the answer and the log
            error = repr(e)
            status, body, ctype, extra = 500, f"fake GitHub failed: {error}".encode("utf-8"), "text/plain", {}
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra.items():
            self.send_header(k, v)
        self.end_headers()
        if not head_only:
            self.wfile.write(body)
        entry = {"time": time.time(), "method": self.command, "host": host, "path": self.path, "status": status,
                 "token": bool(auth), "agent": self.headers.get("User-Agent") or ""}
        if error:
            entry["error"] = error
        with self.lock:
            with open(self.folder / "requests.jsonl", "a", encoding="utf-8") as f:
                f.write(json.dumps(entry) + "\n")


def _json(status: int, value, extra: Optional[dict] = None) -> tuple:
    """A JSON answer.

    Args:
        status: The HTTP status.
        value: The value.
        extra: More headers.
    Returns:
        (status, body, content type, headers).
    """
    return status, json.dumps(value).encode("utf-8"), "application/json; charset=utf-8", extra or {}


def _api(state: dict, path: str, auth: str) -> tuple:
    """Answer an api.github.com request from the state.

    Args:
        state: The fake's state.
        path: The request path (no query).
        auth: The Authorization header ("" when none).
    Returns:
        (status, body, content type, headers).
    """
    token = auth.split(None, 1)[1].strip() if " " in auth else auth.strip()
    if token and token in state.get("bad_tokens", []):
        return _json(401, {"message": "Bad credentials", "documentation_url": "https://docs.github.com/rest",
                           "status": "401"})
    if state.get("rate_limited"):
        reset = str(int(time.time()) + 3600)
        return _json(403, {"message": "API rate limit exceeded for 127.0.0.1. (But here's the good news: "
                                      "Authenticated requests get a higher rate limit.)",
                           "documentation_url": "https://docs.github.com/rest/overview/rate-limits-for-the-rest-api"},
                     {"X-RateLimit-Limit": "60", "X-RateLimit-Remaining": "0", "X-RateLimit-Reset": reset})
    base = f"/repos/{REPO}/releases"
    published = [_public(r) for r in state["releases"] if not r.get("draft")]
    if path.rstrip("/") == base:
        return _json(200, published)
    if path == base + "/latest":
        stable = [r for r in published if not r.get("prerelease")]
        return _json(200, stable[0]) if stable else _json(404, {"message": "Not Found"})
    if path.startswith(base + "/tags/"):
        tag = path[len(base + "/tags/"):]
        hit = [r for r in published if r["tag_name"] == tag]
        return _json(200, hit[0]) if hit else _json(404, {"message": "Not Found"})
    return _json(404, {"message": "Not Found"})


def _download(state: dict, path: str) -> tuple:
    """Answer an asset download (github.com/<repo>/releases/download/<tag>/<name>) or a release page.

    Args:
        state: The fake's state.
        path: The request path (no query).
    Returns:
        (status, body, content type, headers).
    """
    prefix = f"/{REPO}/releases/download/"
    if path.startswith(prefix):
        tag, _, name = path[len(prefix):].partition("/")
        for r in state["releases"]:
            if r["tag_name"] == tag:
                for a in r["assets"]:
                    if a["name"] == name:
                        return 200, Path(a["_file"]).read_bytes(), "application/octet-stream", {}
        return 404, b"Not Found", "text/plain", {}
    if path.startswith(f"/{REPO}/releases/tag/"):
        return 200, b"<html><body>a release page of the fake GitHub</body></html>", "text/html", {}
    return 404, b"Not Found", "text/plain", {}


def serve(folder: Path, port: int = 443) -> None:
    """Serve the fake GitHub from a folder on 127.0.0.1 until killed (writes <folder>\\listening when ready).

    Args:
        folder: The fake's folder (state.json, server.pem, server.key, assets\\).
        port: The port (443 for the programs under test; another one only to check the fake itself).
    Returns:
        Never (serve_forever).
    Raises:
        OSError: When the port cannot be bound (another server holds it).
        ssl.SSLError: When the certificate or key cannot be loaded.
    """
    _Handler.folder = folder
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(str(folder / "server.pem"), str(folder / "server.key"))
    server = _TlsServer(("127.0.0.1", port), _Handler, context)
    (folder / "listening").write_text(str(os.getpid()), encoding="ascii")
    server.serve_forever()


class _TlsServer(http.server.ThreadingHTTPServer):
    """A threading HTTP server that speaks TLS on each connection, the handshake in the connection's own thread (a
    wrapped LISTENING socket would run every handshake in the accept loop, where one stuck client stalls them all)."""

    daemon_threads = True

    def __init__(self, address: tuple, handler, context: ssl.SSLContext) -> None:
        """Bind and listen.

        Args:
            address: (host, port).
            handler: The request handler class.
            context: The server-side TLS context.
        Returns:
            None.
        Raises:
            OSError: When the address cannot be bound.
        """
        super().__init__(address, handler)
        self.context = context

    def finish_request(self, request, client_address) -> None:
        """Run one connection (in its thread): the TLS handshake, then the handler.

        Args:
            request: The accepted socket.
            client_address: The client's address.
        Returns:
            None.
        Raises:
            ssl.SSLError / OSError: When the handshake fails (socketserver reports it through handle_error, which
                writes the traceback to the server's log).
        """
        self.RequestHandlerClass(self.context.wrap_socket(request, server_side=True), client_address, self)


if __name__ == "__main__":
    if len(sys.argv) in (3, 4) and sys.argv[1] == "serve":
        serve(Path(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) == 4 else 443)
    else:
        sys.exit("usage: python fakegithub.py serve <folder> [port]")
