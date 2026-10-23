"""The scripted Anthropic Messages API: the HTTP server Claude Code talks to in the end-to-end suite.

Endpoints (E2E_TESTING.md §4.1):
    POST /v1/messages                 the agent loop and Claude Code's side requests; streamed (SSE) or whole
    POST /v1/messages/count_tokens    a rough count (so a /context or a warning never fails)
    GET  /v1/models[/<id>]            the configured mock models
Control API (loopback only, like everything here):
    GET  /_mock/health                {"ok": true, "seq": <last request>}
    GET  /_mock/requests?session=&since=&kind=
                                      the classified request log (the test's oracle of what reached the model)
    GET  /_mock/sessions              per-session counts
    GET  /_mock/gates                 every gate: open, waiting streams, outcome
    POST /_mock/gate/<name>/release   a `hold` continues (also before it is reached)
    POST /_mock/gate/<name>/fail      {"status": 529, "type": "...", "message": "..."} or {"drop": true}
    GET  /_mock/expectations          the `expect` results
    POST /_mock/reset                 forget the log, gates, attempts, floors and expectations
    POST /_mock/config                {"floor_ms": n, "default_reply": "...", "title": "...", "models": [...]}

⚠ Bound to 127.0.0.1; plain HTTP; no authentication. It must never listen on a network.
"""
from __future__ import annotations

import json
import os
import re
import socket
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Optional
from urllib.parse import parse_qs, urlparse

from . import classify as cls
from .script import Element, Plan, Response, ScriptError, check_tool_input, find_scripts, plan
from .state import MockState

#: How often a stream that waits (a hold, a long delay) sends a `ping` event once it has started, so neither
#: side mistakes the silence for a dead connection and a client that went away is noticed.
PING_EVERY_S = 2.0
#: The compaction summary the compact side request returns.
COMPACT_SUMMARY = "<summary>Mock compaction summary: the conversation so far was scripted by the e2e suite.</summary>"


class Cut(Exception):
    """Raised inside a stream to cut the connection where it stands (a `drop` step or a failed gate)."""


class Handler(BaseHTTPRequestHandler):
    """One HTTP/1.1 request (the server runs each on its own thread)."""

    protocol_version = "HTTP/1.1"
    #: Set by make_server: the shared state and the scenario loader.
    state: MockState = None  # type: ignore[assignment]
    scenarios: Optional[Callable[[str], str]] = None

    # ---- plumbing -------------------------------------------------------------------------------

    def log_message(self, fmt: str, *args: Any) -> None:
        """Silence the default stderr access log (the request log is the record).

        Args:
            fmt: The format string.
            *args: Its arguments.
        Returns:
            None.
        """

    def _body(self) -> dict:
        """Read and parse the request body.

        Returns:
            The JSON object, {} for an empty body, {"_raw": <first 2000 chars>} for one that is not JSON.
        Raises:
            OSError: When the socket fails mid-read.
        """
        n = int(self.headers.get("content-length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            value = json.loads(raw.decode("utf-8")) if raw else {}
            return value if isinstance(value, dict) else {"_raw": str(value)[:2000]}
        except ValueError:
            return {"_raw": raw[:2000].decode("utf-8", "replace")}

    def _json(self, status: int, obj: Any) -> None:
        """Answer with a whole JSON body.

        Args:
            status: The HTTP status.
            obj: The answer.
        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        data = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.send_header("request-id", "req_mock_" + uuid.uuid4().hex[:12])
        self.end_headers()
        self.wfile.write(data)

    @staticmethod
    def _api_error(status: int, etype: str, message: str) -> dict:
        """An Anthropic API error body.

        Args:
            status: The HTTP status (unused in the body; kept for symmetry with the call sites).
            etype: The error type (overloaded_error, invalid_request_error, ...).
            message: The message.
        Returns:
            {"type": "error", "error": {"type": ..., "message": ...}}.
        """
        return {"type": "error", "error": {"type": etype, "message": message}}

    # ---- control API ----------------------------------------------------------------------------

    def do_GET(self) -> None:
        """Answer GET: the models list and the control API.

        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        url = urlparse(self.path)
        q = {k: v[-1] for k, v in parse_qs(url.query).items()}
        st = self.state
        if url.path == "/_mock/health":
            self._json(200, {"ok": True, "seq": max((e["seq"] for e in st.requests()), default=0)})
        elif url.path == "/_mock/requests":
            self._json(200, {"requests": st.requests(q.get("session", ""), int(q.get("since", "0") or 0), q.get("kind", ""))})
        elif url.path == "/_mock/sessions":
            self._json(200, {"sessions": st.sessions()})
        elif url.path == "/_mock/gates":
            self._json(200, {"gates": st.gates()})
        elif url.path == "/_mock/expectations":
            self._json(200, {"expectations": st.expectations()})
        elif url.path.startswith("/v1/models"):
            ids = list(st.config.get("models") or ["claude-mock-1"])
            models = [{"type": "model", "id": i, "display_name": i, "created_at": "2026-01-01T00:00:00Z"} for i in ids]
            tail = url.path[len("/v1/models"):].strip("/")
            if tail:
                match = next((m for m in models if m["id"] == tail), None)
                # An unknown model id still answers: Claude Code only checks that the call works.
                self._json(200, match or {"type": "model", "id": tail, "display_name": tail, "created_at": "2026-01-01T00:00:00Z"})
            else:
                self._json(200, {"data": models, "has_more": False, "first_id": ids[0], "last_id": ids[-1]})
        else:
            self._json(404, self._api_error(404, "not_found_error", self.path))

    def _control_post(self, path: str, body: dict) -> None:
        """Answer a POST of the control API.

        Args:
            path: The URL path (/_mock/...).
            body: The parsed body.
        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        st = self.state
        m = re.fullmatch(r"/_mock/gate/([^/]+)/(release|fail)", path)
        if m:
            name, action = m.group(1), m.group(2)
            outcome = None
            if action == "fail":
                outcome = {"drop": True} if body.get("drop") else {
                    "status": int(body.get("status", 529)), "type": str(body.get("type", "overloaded_error")),
                    "message": str(body.get("message", "mock gate failure"))}
            gate = st.open_gate(name, outcome)
            self._json(200, {"gate": name, "waiting": gate.waiting, "outcome": outcome})
        elif path == "/_mock/reset":
            st.reset()
            self._json(200, {"ok": True})
        elif path == "/_mock/config":
            for key in ("floor_ms", "default_reply", "title", "models"):
                if key in body:
                    st.config[key] = body[key]
            self._json(200, {"config": st.config})
        else:
            self._json(404, self._api_error(404, "not_found_error", path))

    # ---- the Messages API -----------------------------------------------------------------------

    def do_POST(self) -> None:
        """Answer POST: the Messages API, or the control API.

        Returns:
            None.
        Raises:
            OSError: When the client goes away mid-answer (an Esc in the TUI aborts the request).
        """
        url = urlparse(self.path)
        body = self._body()
        if url.path.startswith("/_mock/"):
            self._control_post(url.path, body)
            return
        if not url.path.startswith("/v1/messages"):
            self._json(404, self._api_error(404, "not_found_error", url.path))
            return
        st = self.state
        entry = st.begin({"method": "POST", "path": url.path, **cls.summarize(url.path, body)}, body)
        t0 = time.monotonic()
        try:
            self._answer(url.path, body, entry)
        except (ConnectionError, OSError) as error:
            # The client went away (an Esc, a closed tab, a cut we made): nothing to answer any more.
            st.end(entry, ms=round((time.monotonic() - t0) * 1000), client_gone=type(error).__name__)
            self.close_connection = True
            return
        st.end(entry, ms=round((time.monotonic() - t0) * 1000))

    def _answer(self, path: str, body: dict, entry: dict) -> None:
        """Choose and send the answer of one Messages API request.

        Args:
            path: The URL path.
            body: The parsed request.
            entry: Its log entry (the choice is recorded on it).
        Returns:
            None.
        Raises:
            OSError: When the client goes away.
        """
        st = self.state
        kind = entry["kind"]
        msgs = body.get("messages") or []
        if kind == "count_tokens":
            st.end(entry, answer="count_tokens")
            self._json(200, {"input_tokens": max(1, len(json.dumps(body)) // 4)})
            return
        script_idx = cls.scripted_index(msgs)
        tools = cls.tool_schemas(body)
        the_plan: Optional[Plan] = None
        plan_error = ""
        if script_idx is not None:
            source = find_scripts(cls.text_of(msgs[script_idx].get("content")))[-1]
            try:
                the_plan = plan(source, set(tools), self.scenarios)
            except ScriptError as error:
                plan_error = str(error)
        if kind == "title":
            title = (the_plan.title if the_plan and the_plan.title else st.config.get("title")) or "Mock session title"
            self._send(body, Response(elements=[Element("text", text=json.dumps({"title": title}))]), entry,
                       why="side:title")
            return
        if kind == "compact":
            self._send(body, Response(elements=[Element("text", text=COMPACT_SUMMARY)]), entry, why="side:compact")
            return
        if kind == "unknown":
            self._send(body, Response(elements=[Element("text", text="Mock.")]), entry, why="side:unknown")
            return
        # The agent loop (or a subagent): the turn's script, or the default reply.
        session = entry.get("session") or ""
        turn_index = script_idx if script_idx is not None else cls.last_user_index(msgs)
        turn = cls.turn_key(session, msgs, turn_index)
        started = st.turn_started(turn)
        entry["turn"] = turn
        if plan_error:
            self._send(body, Response(elements=[Element("text", text=f"mock: bad script: {plan_error}")]), entry,
                       why="bad-script", flag="script_error", detail=plan_error)
            return
        if the_plan is None:
            reply = Response(elements=[Element("text", text=str(st.config.get("default_reply") or "OK"))])
            self._floor(started, False)
            self._send(body, reply, entry, why="default", k=0)
            return
        k = sum(1 for m in msgs[script_idx + 1:] if isinstance(m, dict) and m.get("role") == "assistant")
        entry["k"] = k
        if k >= len(the_plan.responses):
            self._floor(started, the_plan.fast)
            self._send(body, Response(elements=[Element("text", text="mock: script done")]), entry, why="past-end", k=k)
            return
        resp = the_plan.responses[k]
        why = f"step {k + 1}/{len(the_plan.responses)}"
        if resp.expect:
            got = "\n".join(cls.tool_results_of((msgs[cls.last_user_index(msgs)] or {}).get("content")))
            ok = re.search(resp.expect, got) is not None
            st.expectation({"seq": entry["seq"], "session": session, "pattern": resp.expect, "ok": ok, "got": got[:2000]})
            entry["expect"] = {"pattern": resp.expect, "ok": ok}
        if resp.fail is not None:
            status, etype, message, times = resp.fail
            attempt = st.attempt((session, the_plan.source, k))
            if times is None or attempt <= times:
                st.end(entry, why=f"{why} attempt {attempt} (fail)", status=status, answer=f"error {status}")
                self._json(status, self._api_error(status, etype, message))
                return
            why += f" attempt {attempt}"
        problems = [p for el in resp.elements if el.kind == "tool_use"
                    for p in check_tool_input(el.name, el.input, tools.get(el.name))]
        if problems:
            self._send(body, Response(elements=[Element("text", text="mock: bad tool input: " + "; ".join(problems))]),
                       entry, why="bad-tool-input", flag="script_error", detail="; ".join(problems))
            return
        if resp.final:
            self._floor(started, the_plan.fast)
        self._send(body, resp, entry, why=why, k=k)

    def _floor(self, started: float, fast: bool) -> None:
        """Hold a turn's final response until the turn floor has passed since the turn's first request.

        Args:
            started: time.monotonic() of the turn's first request.
            fast: True when the script asked for no floor.
        Returns:
            None.
        """
        floor_ms = 0 if fast else int(self.state.config.get("floor_ms") or 0)
        wait = started + floor_ms / 1000.0 - time.monotonic()
        if wait > 0:
            time.sleep(wait)

    def _send(self, body: dict, resp: Response, entry: dict, why: str, k: Optional[int] = None,
              flag: str = "", detail: str = "") -> None:
        """Record the choice and send the response, streamed or whole.

        Args:
            body: The request (its model and `stream` flag are used).
            resp: The response to send.
            entry: The log entry.
            why: A short label of the choice ("step 2/3", "side:title", "default", ...).
            k: The response index within the script, when scripted.
            flag: A problem class to flag in the log ("script_error"), or "".
            detail: The problem's text.
        Returns:
            None.
        Raises:
            OSError: When the client goes away.
        """
        answer = [[el.kind, el.name or el.text[:80]] for el in resp.elements]
        fields = {"why": why, "answer": answer, "stop": resp.stop, "status": 200}
        if k is not None:
            fields["k"] = k
        if flag:
            fields["flag"], fields["detail"] = flag, detail
        self.state.end(entry, **fields)
        if body.get("stream"):
            Stream(self, body, resp).run()
        else:
            self._whole(body, resp)

    def _whole(self, body: dict, resp: Response) -> None:
        """Answer a non-streamed request with one message (delays and holds still apply).

        Args:
            body: The request.
            resp: The response.
        Returns:
            None.
        Raises:
            OSError: When the client goes away.
        """
        content: list[dict] = []
        for el in resp.elements:
            if el.kind == "delay":
                time.sleep(el.ms / 1000.0)
            elif el.kind == "hold":
                outcome = _wait_gate(self.state, el.name, None)
                if outcome and outcome.get("drop"):
                    self.close_connection = True
                    return
                if outcome:
                    self._json(outcome["status"], self._api_error(outcome["status"], outcome["type"], outcome["message"]))
                    return
            elif el.kind == "drop":
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass  # already gone: the cut is what was asked for
                return
            elif el.kind == "text":
                content.append({"type": "text", "text": el.text})
            elif el.kind == "thinking":
                content.append({"type": "thinking", "thinking": el.text, "signature": "mock-signature"})
            elif el.kind == "tool_use":
                content.append({"type": "tool_use", "id": "toolu_mock_" + uuid.uuid4().hex[:16], "name": el.name,
                                "input": el.input})
        self._json(200, {"id": "msg_mock_" + uuid.uuid4().hex[:16], "type": "message", "role": "assistant",
                         "model": body.get("model", "claude-mock-1"), "content": content,
                         "stop_reason": resp.stop, "stop_sequence": None, "usage": _usage(resp)})


def _usage(resp: Response) -> dict:
    """A response's usage numbers (defaults, overridden by a `usage` step).

    Args:
        resp: The response.
    Returns:
        The usage dict.
    """
    usage = {"input_tokens": 100, "output_tokens": 10, "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0}
    usage.update(resp.usage)
    return usage


def _wait_gate(state: MockState, name: str, ping: Optional[Callable[[], None]]) -> Optional[dict]:
    """Wait until a gate opens.

    Args:
        state: The mock state.
        name: The gate's name.
        ping: Called every PING_EVERY_S while waiting (a started stream sends a ping event; it raises
            OSError when the client went away, which ends the wait), or None.
    Returns:
        The gate's outcome (None = released normally).
    Raises:
        OSError: From ping, when the client has gone away.
    """
    gate = state.gate(name)
    state.wait_count(gate, +1)
    try:
        while not gate.opened.wait(PING_EVERY_S):
            if ping:
                ping()
    finally:
        state.wait_count(gate, -1)
    return gate.outcome


class Stream:
    """Send one response as the Messages API's server-sent events.

    The stream starts LAZILY: headers and `message_start` go out with the first content block (or the first
    ping of a wait that follows content), so a `hold` or an `error` before any output can still answer with
    a real HTTP error status.
    """

    def __init__(self, handler: Handler, body: dict, resp: Response) -> None:
        """Prepare the stream.

        Args:
            handler: The request's handler (its socket).
            body: The request (its model is echoed).
            resp: The response to send.
        Returns:
            None.
        """
        self.h = handler
        self.body = body
        self.resp = resp
        self.started = False
        self.cut_after: Optional[int] = None  # events still allowed before a drop cuts the stream

    def _chunk(self, text: str) -> None:
        """Write one HTTP chunk and flush it, so Claude Code sees each event at once.

        Args:
            text: The chunk's text.
        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        data = text.encode("utf-8")
        self.h.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
        self.h.wfile.flush()

    def start(self) -> None:
        """Send the headers and `message_start` once.

        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        if self.started:
            return
        self.started = True
        h = self.h
        h.send_response(200)
        h.send_header("content-type", "text/event-stream")
        h.send_header("cache-control", "no-cache")
        h.send_header("transfer-encoding", "chunked")
        h.send_header("request-id", "req_mock_" + uuid.uuid4().hex[:12])
        h.end_headers()
        usage = _usage(self.resp)
        start_usage = {k: usage[k] for k in ("input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")}
        start_usage["output_tokens"] = 1
        self.event("message_start", {"type": "message_start", "message": {
            "id": "msg_mock_" + uuid.uuid4().hex[:16], "type": "message", "role": "assistant",
            "model": self.body.get("model", "claude-mock-1"), "content": [], "stop_reason": None,
            "stop_sequence": None, "usage": start_usage}}, count=False)

    def event(self, name: str, obj: dict, count: bool = True) -> None:
        """Write one server-sent event (starting the stream first).

        Args:
            name: The event name.
            obj: Its data.
            count: Whether the event counts toward a pending `drop after=N`.
        Returns:
            None.
        Raises:
            Cut: When a pending `drop after=N` has run out (the caller cuts the connection).
            OSError: When the client has gone away.
        """
        if not self.started:
            self.start()
        if count and self.cut_after is not None:
            if self.cut_after <= 0:
                raise Cut()
            self.cut_after -= 1
        self._chunk(f"event: {name}\ndata: {json.dumps(obj)}\n\n")

    def ping(self) -> None:
        """Send a ping event when the stream has started (keeps the connection visibly alive while waiting).

        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        if self.started:
            self._chunk('event: ping\ndata: {"type": "ping"}\n\n')

    def _cut(self) -> None:
        """Cut the connection where the stream stands (no message_stop: Claude Code sees a lost connection).

        Returns:
            None.
        """
        self.h.close_connection = True
        try:
            self.h.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass  # already gone: the cut is what was asked for

    def _wait(self, seconds: float) -> None:
        """Sleep, pinging every PING_EVERY_S once the stream has started.

        Args:
            seconds: How long.
        Returns:
            None.
        Raises:
            OSError: When a ping finds the client gone.
        """
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return
            time.sleep(min(left, PING_EVERY_S))
            if end - time.monotonic() > 0:
                self.ping()

    def run(self) -> None:
        """Send the whole response.

        Returns:
            None.
        Raises:
            OSError: When the client goes away (an interrupt in the TUI).
        """
        index = 0
        try:
            for el in self.resp.elements:
                if el.kind == "delay":
                    self._wait(el.ms / 1000.0)
                    continue
                if el.kind == "hold":
                    outcome = _wait_gate(self.h.state, el.name, self.ping)
                    if outcome and outcome.get("drop"):
                        raise Cut()
                    if outcome:
                        if not self.started:
                            self.h._json(outcome["status"], Handler._api_error(outcome["status"], outcome["type"],
                                                                               outcome["message"]))
                            return
                        # Mid-stream: the API's own way to fail a started stream.
                        self.event("error", {"type": "error", "error": {"type": outcome["type"],
                                                                        "message": outcome["message"]}}, count=False)
                        raise Cut()
                    continue
                if el.kind == "drop":
                    if el.after <= 0:
                        if not self.started:
                            self.start()
                        raise Cut()
                    self.cut_after = el.after
                    continue
                self._block(index, el)
                index += 1
            self.event("message_delta", {"type": "message_delta",
                                         "delta": {"stop_reason": self.resp.stop, "stop_sequence": None},
                                         "usage": {"output_tokens": _usage(self.resp)["output_tokens"]}})
            self.event("message_stop", {"type": "message_stop"})
            self.h.wfile.write(b"0\r\n\r\n")
            self.h.wfile.flush()
        except Cut:
            self._cut()

    def _block(self, index: int, el: Element) -> None:
        """Send one content block: start, its deltas (chunked when the script asked), stop.

        Args:
            index: The block index.
            el: The block (text, thinking or tool_use).
        Returns:
            None.
        Raises:
            Cut: When a drop runs out mid-block.
            OSError: When the client goes away.
        """
        if el.kind == "tool_use":
            block = {"type": "tool_use", "id": "toolu_mock_" + uuid.uuid4().hex[:16], "name": el.name, "input": {}}
            self.event("content_block_start", {"type": "content_block_start", "index": index, "content_block": block})
            self.event("content_block_delta", {"type": "content_block_delta", "index": index,
                                               "delta": {"type": "input_json_delta", "partial_json": json.dumps(el.input)}})
            self.event("content_block_stop", {"type": "content_block_stop", "index": index})
            return
        is_text = el.kind == "text"
        block = {"type": "text", "text": ""} if is_text else {"type": "thinking", "thinking": ""}
        self.event("content_block_start", {"type": "content_block_start", "index": index, "content_block": block})
        pieces = [el.text[i:i + el.chunk] for i in range(0, len(el.text), el.chunk)] if el.chunk > 0 and el.text else [el.text]
        for n, piece in enumerate(pieces):
            delta = {"type": "text_delta", "text": piece} if is_text else {"type": "thinking_delta", "thinking": piece}
            self.event("content_block_delta", {"type": "content_block_delta", "index": index, "delta": delta})
            if el.chunk_ms and n + 1 < len(pieces):
                time.sleep(el.chunk_ms / 1000.0)
        if not is_text:
            self.event("content_block_delta", {"type": "content_block_delta", "index": index,
                                               "delta": {"type": "signature_delta", "signature": "mock-signature"}})
        self.event("content_block_stop", {"type": "content_block_stop", "index": index})


def make_server(port: int, state: MockState, scenarios_dir: Optional[str] = None,
                host: str = "127.0.0.1") -> ThreadingHTTPServer:
    """Create the server (not started).

    Args:
        port: The TCP port (0 = any free port; read it back from server.server_address).
        state: The shared state.
        scenarios_dir: The folder `use @name` reads <name>.mock files from (None = `use` is an error).
        host: The address to bind. ⚠ Keep 127.0.0.1: the mock has no authentication.
    Returns:
        The server; run it with serve_forever() (on a thread for tests).
    Raises:
        OSError: When the port is taken.
    """
    def load(name: str) -> str:
        """Read a scenario file.

        Args:
            name: The scenario name (letters, digits, dashes, underscores).
        Returns:
            Its text.
        Raises:
            ScriptError: On a bad name or a missing file.
        """
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise ScriptError(f"bad scenario name {name!r}")
        path = os.path.join(scenarios_dir or "", name + ".mock")
        if not scenarios_dir or not os.path.isfile(path):
            raise ScriptError(f"no scenario {name!r} in {scenarios_dir!r}")
        with open(path, encoding="utf-8") as f:
            return f.read()

    handler = type("BoundHandler", (Handler,), {"state": state, "scenarios": staticmethod(load) if scenarios_dir else None})
    server = ThreadingHTTPServer((host, port), handler)
    server.daemon_threads = True
    return server


def serve_in_thread(state: MockState, port: int = 0, scenarios_dir: Optional[str] = None) -> tuple[ThreadingHTTPServer, threading.Thread]:
    """Start a server on a background thread (the unit tests' entry point).

    Args:
        state: The shared state.
        port: The port (0 = any free port).
        scenarios_dir: See make_server.
    Returns:
        (server, thread); stop with server.shutdown().
    Raises:
        OSError: When the port is taken.
    """
    server = make_server(port, state, scenarios_dir)
    thread = threading.Thread(target=server.serve_forever, name="mockllm", daemon=True)
    thread.start()
    return server, thread
