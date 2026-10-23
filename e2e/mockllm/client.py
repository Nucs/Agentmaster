"""The test side of the mock's control API: what reached the model, gates, resets.

Used by the suite's fixtures (one MockClient per run) and by hand from a guest shell:
``python -c "from mockllm.client import MockClient; print(MockClient().requests())"``.
"""
from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from typing import Any, Callable, Optional


class MockError(RuntimeError):
    """The mock did not answer, or answered an error, or a wait ran out."""


class MockClient:
    """A small HTTP client for the control API (stdlib only, so it runs anywhere the suite runs)."""

    def __init__(self, base: str = "http://127.0.0.1:8787", timeout_s: float = 10.0) -> None:
        """Create a client.

        Args:
            base: The mock's base URL.
            timeout_s: The per-call HTTP timeout.
        Returns:
            None.
        """
        self.base = base.rstrip("/")
        self.timeout_s = timeout_s

    def _call(self, method: str, path: str, body: Optional[dict] = None) -> Any:
        """One control call.

        Args:
            method: GET or POST.
            path: The path (/_mock/...).
            body: The JSON body of a POST.
        Returns:
            The parsed JSON answer.
        Raises:
            MockError: When the mock is down or answers an error status.
        """
        data = json.dumps(body or {}).encode("utf-8") if method == "POST" else None
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers={"content-type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
                return json.loads(resp.read().decode("utf-8") or "{}")
        except (urllib.error.URLError, OSError, ValueError) as error:
            raise MockError(f"mock {method} {path}: {error}") from None

    def health(self) -> dict:
        """The health answer.

        Returns:
            {"ok": True, "seq": <last request seq>}.
        Raises:
            MockError: When the mock is down.
        """
        return self._call("GET", "/_mock/health")

    def wait_up(self, timeout_s: float = 20.0) -> None:
        """Wait until the mock answers.

        Args:
            timeout_s: How long.
        Returns:
            None.
        Raises:
            MockError: When it never answered.
        """
        end = time.monotonic() + timeout_s
        last: Optional[Exception] = None
        while time.monotonic() < end:
            try:
                self.health()
                return
            except MockError as error:
                last = error
                time.sleep(0.25)
        raise MockError(f"the mock at {self.base} did not come up in {timeout_s} s ({last})")

    def reset(self) -> None:
        """Forget the log, gates, attempts and floors (between tests).

        Returns:
            None.
        Raises:
            MockError: On failure.
        """
        self._call("POST", "/_mock/reset")

    def configure(self, **config: Any) -> dict:
        """Change server defaults (floor_ms, default_reply, title, models).

        Args:
            **config: The fields.
        Returns:
            The whole config after the change.
        Raises:
            MockError: On failure.
        """
        return self._call("POST", "/_mock/config", config)["config"]

    def last_seq(self) -> int:
        """The seq of the newest request (a mark to read only what comes after).

        Returns:
            The seq (0 when nothing arrived yet).
        Raises:
            MockError: On failure.
        """
        return int(self.health().get("seq", 0))

    def requests(self, session: str = "", since: int = 0, kind: str = "") -> list[dict]:
        """The request log, filtered.

        Args:
            session: Only this Claude Code session id.
            since: Only after this seq.
            kind: main / title / compact / count_tokens / unknown.
        Returns:
            The entries, oldest first.
        Raises:
            MockError: On failure.
        """
        q = f"?session={session}&since={since}&kind={kind}"
        return self._call("GET", "/_mock/requests" + q)["requests"]

    def prompts(self, since: int = 0, session: str = "") -> list[dict]:
        """The turns that STARTED: the first main request of each turn (k == 0, or an unscripted turn).

        Args:
            since: Only after this seq.
            session: Only this session.
        Returns:
            Their entries (each with user_text, session, seq, t), oldest first.
        Raises:
            MockError: On failure.
        """
        out, seen = [], set()
        for e in self.requests(session, since, "main"):
            if e.get("k", 0) == 0 and e.get("turn") not in seen:
                seen.add(e.get("turn"))
                out.append(e)
        return out

    def wait_for(self, predicate: Callable[[dict], bool], since: int = 0, timeout_s: float = 60.0,
                 what: str = "a request") -> dict:
        """Wait for a request matching a predicate.

        Args:
            predicate: Called with each entry (newest requests included as they arrive).
            since: Only after this seq.
            timeout_s: How long.
            what: A description for the error.
        Returns:
            The first matching entry.
        Raises:
            MockError: When none arrived in time (the message lists what did arrive).
        """
        end = time.monotonic() + timeout_s
        seen: list[dict] = []
        while time.monotonic() < end:
            seen = self.requests(since=since)
            for e in seen:
                if predicate(e):
                    return e
            time.sleep(0.25)
        tail = [f"#{e['seq']} {e.get('kind')} k={e.get('k')} {e.get('user_text', '')[:60]!r}" for e in seen[-8:]]
        raise MockError(f"no {what} in {timeout_s} s; the last requests: {tail}")

    def wait_prompt(self, text: str, since: int = 0, timeout_s: float = 60.0) -> dict:
        """Wait for a turn whose prompt holds a text (the prompt reached the model).

        Args:
            text: A substring of the user's message.
            since: Only after this seq.
            timeout_s: How long.
        Returns:
            The turn's first request entry.
        Raises:
            MockError: When it never arrived.
        """
        return self.wait_for(lambda e: e.get("kind") == "main" and e.get("k", 0) == 0 and text in e.get("user_text", ""),
                             since, timeout_s, f"turn with {text!r}")

    def release(self, gate: str) -> dict:
        """Release a gate (a `hold` continues; also before it is reached).

        Args:
            gate: Its name.
        Returns:
            The answer ({"gate", "waiting", "outcome"}).
        Raises:
            MockError: On failure.
        """
        return self._call("POST", f"/_mock/gate/{gate}/release")

    def fail(self, gate: str, status: int = 529, etype: str = "overloaded_error", message: str = "mock failure") -> dict:
        """Fail a gate with an API error.

        Args:
            gate: Its name.
            status: The HTTP status (used when the stream has not started).
            etype: The error type.
            message: The error message.
        Returns:
            The answer.
        Raises:
            MockError: On failure.
        """
        return self._call("POST", f"/_mock/gate/{gate}/fail", {"status": status, "type": etype, "message": message})

    def drop(self, gate: str) -> dict:
        """Cut the connection of the stream waiting at a gate.

        Args:
            gate: Its name.
        Returns:
            The answer.
        Raises:
            MockError: On failure.
        """
        return self._call("POST", f"/_mock/gate/{gate}/fail", {"drop": True})

    def gates(self) -> dict:
        """Every gate's state.

        Returns:
            {name: {"open", "waiting", "outcome"}}.
        Raises:
            MockError: On failure.
        """
        return self._call("GET", "/_mock/gates")["gates"]

    def wait_holding(self, gate: str, timeout_s: float = 60.0) -> None:
        """Wait until a stream waits at a gate (the turn really reached its `hold`).

        Args:
            gate: Its name.
            timeout_s: How long.
        Returns:
            None.
        Raises:
            MockError: When no stream reached it in time.
        """
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            if self.gates().get(gate, {}).get("waiting", 0) > 0:
                return
            time.sleep(0.2)
        raise MockError(f"no stream reached gate {gate!r} in {timeout_s} s")

    def expectations(self) -> list[dict]:
        """The `expect` results.

        Returns:
            [{"seq", "session", "pattern", "ok", "got"}].
        Raises:
            MockError: On failure.
        """
        return self._call("GET", "/_mock/expectations")["expectations"]
