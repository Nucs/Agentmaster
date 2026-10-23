"""The little the mock must remember between requests: the request log, gates, retry attempts, turn floors.

Everything else is derived from each request alone (E2E_TESTING.md §4.5), so a retried request gets the same
answer. One `MockState` per server; every method is thread-safe (the HTTP server answers each request on its
own thread).
"""
from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Optional


@dataclass
class Gate:
    """A named point a `hold` step waits at until the test releases or fails it.

    Attributes:
        name: The gate's name (chosen by the test; unique per test).
        opened: Set when released or failed — the hold then passes (or fails) at once, also for a hold
            reached AFTER the release (a test may release before Claude Code gets there).
        outcome: None = released normally; {"status", "type", "message"} = fail with an error;
            {"drop": True} = cut the connection.
        waiting: How many streams wait at it now (the control API shows it, so a test can wait until the
            turn really reached the hold before it asserts "Running").
    """

    name: str
    opened: threading.Event = field(default_factory=threading.Event)
    outcome: Optional[dict] = None
    waiting: int = 0


class MockState:
    """The server's shared state and its request log.

    Attributes:
        config: Server defaults the control API may change: floor_ms (the turn floor), default_reply (the
            answer of an unscripted turn), title (the default session title), models (GET /v1/models).
    """

    def __init__(self, log_path: Optional[str] = None, full_path: Optional[str] = None,
                 floor_ms: int = 2500, default_reply: str = "OK") -> None:
        """Create an empty state.

        Args:
            log_path: The JSONL request log to append to (None = memory only).
            full_path: The JSONL file for whole request bodies (None = not kept). Bodies hold the system
                prompt and the conversation, never a credential (headers are not logged).
            floor_ms: The turn floor in milliseconds (E2E_TESTING.md §4.4: Agentmaster ignores a Stop
                under 2 s after its prompt, so an instant turn would only ever test the reconcile path).
            default_reply: The text of a turn whose prompt holds no script.
        Returns:
            None.
        """
        self._lock = threading.Lock()
        self._log_path = log_path
        self._full_path = full_path
        self._seq = 0
        self._entries: list[dict] = []
        self._attempts: dict[tuple, int] = {}
        self._turn_started: dict[str, float] = {}
        self._gates: dict[str, Gate] = {}
        self._expectations: list[dict] = []
        self.config: dict[str, Any] = {
            "floor_ms": floor_ms, "default_reply": default_reply, "title": "Mock session title",
            "models": ["claude-mock-1"],
        }

    # ---- the request log ------------------------------------------------------------------------

    def begin(self, entry: dict, body: Optional[dict] = None) -> dict:
        """Record a request as it arrives.

        Args:
            entry: The request's summary (classify.summarize + path + method); it gets `seq` and `t`.
            body: The whole body, written to the full log when one is configured.
        Returns:
            The same dict, now in the log (the handler adds the outcome to it later through `end`).
        """
        with self._lock:
            self._seq += 1
            entry["seq"] = self._seq
            entry["t"] = time.time()
            self._entries.append(entry)
        self._append(self._log_path, entry)
        if self._full_path and body is not None:
            self._append(self._full_path, {"seq": entry["seq"], "t": entry["t"], "body": body})
        return entry

    def end(self, entry: dict, **outcome: Any) -> None:
        """Record a request's outcome (the answer, the status, the duration).

        Args:
            entry: The dict `begin` returned.
            **outcome: Fields to add (answer, status, why, ms, error ...).
        Returns:
            None.
        """
        with self._lock:
            entry.update(outcome)
        self._append(self._log_path, {"seq": entry.get("seq"), "done": True, **outcome})

    def requests(self, session: str = "", since: int = 0, kind: str = "") -> list[dict]:
        """The logged requests, filtered.

        Args:
            session: Only this Claude Code session ("" = all).
            since: Only requests with seq > since.
            kind: Only this kind ("" = all).
        Returns:
            Copies of the entries, oldest first.
        """
        with self._lock:
            return [dict(e) for e in self._entries
                    if e["seq"] > since and (not session or e.get("session") == session)
                    and (not kind or e.get("kind") == kind)]

    def sessions(self) -> dict:
        """Per-session counts.

        Returns:
            {session: {"requests": n, "kinds": {kind: n}, "last_t": t}}.
        """
        out: dict[str, dict] = {}
        with self._lock:
            for e in self._entries:
                s = out.setdefault(e.get("session") or "", {"requests": 0, "kinds": {}, "last_t": 0.0})
                s["requests"] += 1
                s["kinds"][e.get("kind", "?")] = s["kinds"].get(e.get("kind", "?"), 0) + 1
                s["last_t"] = max(s["last_t"], e.get("t", 0.0))
        return out

    @staticmethod
    def _append(path: Optional[str], obj: dict) -> None:
        """Append one JSON line to a file.

        Args:
            path: The file (None = nothing to do).
            obj: The object.
        Returns:
            None. A failed write is swallowed on purpose: the log is evidence, and a broken log must never
            break the answer Claude Code is waiting for (the in-memory log still has the entry).
        """
        if not path:
            return
        try:
            with open(path, "a", encoding="utf-8") as f:
                f.write(json.dumps(obj, ensure_ascii=False, default=str) + "\n")
        except OSError:
            pass

    # ---- retries and turn floors ----------------------------------------------------------------

    def attempt(self, key: tuple) -> int:
        """Count one more attempt at a failing step.

        Args:
            key: (session, script source, response index).
        Returns:
            The attempt number (1 for the first).
        """
        with self._lock:
            self._attempts[key] = self._attempts.get(key, 0) + 1
            return self._attempts[key]

    def turn_started(self, key: str) -> float:
        """When a turn's first request arrived (recorded at the first call for the key).

        Args:
            key: The turn key (classify.turn_key).
        Returns:
            The time.monotonic() of the turn's first request.
        """
        with self._lock:
            return self._turn_started.setdefault(key, time.monotonic())

    # ---- gates ----------------------------------------------------------------------------------

    def gate(self, name: str) -> Gate:
        """The named gate (created closed on first use).

        Args:
            name: The gate's name.
        Returns:
            The Gate.
        """
        with self._lock:
            if name not in self._gates:
                self._gates[name] = Gate(name)
            return self._gates[name]

    def open_gate(self, name: str, outcome: Optional[dict]) -> Gate:
        """Release (outcome None) or fail a gate.

        Args:
            name: The gate's name (created when unknown, so a test may release before the hold).
            outcome: None, {"status", "type", "message"} or {"drop": True}.
        Returns:
            The Gate.
        """
        gate = self.gate(name)
        with self._lock:
            gate.outcome = outcome
        gate.opened.set()
        return gate

    def gates(self) -> dict:
        """Every gate's state.

        Returns:
            {name: {"open": bool, "waiting": n, "outcome": ...}}.
        """
        with self._lock:
            return {g.name: {"open": g.opened.is_set(), "waiting": g.waiting, "outcome": g.outcome}
                    for g in self._gates.values()}

    def wait_count(self, gate: Gate, delta: int) -> None:
        """Count a stream starting (+1) or ending (-1) its wait at a gate.

        Args:
            gate: The gate.
            delta: +1 or -1.
        Returns:
            None.
        """
        with self._lock:
            gate.waiting += delta

    # ---- expectations ---------------------------------------------------------------------------

    def expectation(self, record: dict) -> None:
        """Record the outcome of an `expect` step.

        Args:
            record: {"seq", "session", "pattern", "ok", "got"}.
        Returns:
            None.
        """
        with self._lock:
            self._expectations.append(record)

    def expectations(self) -> list[dict]:
        """Every recorded expectation.

        Returns:
            Copies, oldest first.
        """
        with self._lock:
            return [dict(e) for e in self._expectations]

    # ---- reset ----------------------------------------------------------------------------------

    def reset(self) -> None:
        """Forget everything but the config and the files (a fresh state between tests).

        Open gates are released first, so a stream still waiting at one ends instead of hanging a thread.

        Returns:
            None.
        """
        with self._lock:
            gates = list(self._gates.values())
            self._entries.clear()
            self._attempts.clear()
            self._turn_started.clear()
            self._gates.clear()
            self._expectations.clear()
        for g in gates:
            g.opened.set()
