"""Spike: a scripted mock of the Anthropic Messages API, so a real Claude Code finishes its turns instantly.

This is the feasibility probe behind doc/agentmaster/E2E_TESTING.md (2026-10-22). It is NOT the product
mock that document designs (`e2e/mockllm`, Phase 1): it covers just enough of the protocol to prove that a
native claude.exe (2.1.291 here) runs every kind of turn against it — text, thinking, tool calls that
Claude Code really executes, an AskUserQuestion dialog, /compact, retried and final API errors, and a
stream cut mid-response.

THE SCRIPT RIDES IN THE PROMPT. The newest user message that holds `[[mock ...]]` is the script of the
turn. Its steps are separated by `;`:

    say "<text>"                       a text block; the response goes on
    think "<text>"                     a thinking block (with a dummy signature)
    tool <Name> {json input}           a tool_use block; ENDS this response (stop_reason tool_use).
                                       Claude Code runs the tool and sends its result back; the next
                                       request of the turn gets the next response of the script.
    reply "<text>"                     a text block and end_turn; ends the script
    delay <ms>                         sleep before the next block is streamed (the session stays Running)
    drop                               cut the connection where it stands (no message_stop)
    error <status> <type> "<message>"  answer with that HTTP error; a 4th argument N fails only the
                                       first N attempts of the NEXT response (a retry then succeeds)

WHICH RESPONSE A REQUEST GETS is derived from the request alone: the count of assistant messages after
the scripted user message. A retried request therefore gets the same answer, and the server keeps no
conversation state (only the attempt counters of `error ... N`, keyed by Claude Code's session id).

SIDE REQUESTS are answered WITHOUT the script, because they quote the prompt (and so its script) too:
  - the session-title request (system prompt "naming a coding session", a JSON-schema output) gets
    {"title": "Mock session title"};
  - the compaction request (last user text "CRITICAL: Respond with TEXT ONLY...") gets a fixed summary.
A request with no script anywhere gets "mock: <first 60 characters of its last message>".

Every request is appended to <log> as one JSON line (path, model, the tool list, the classification, the
answer); with --full, the whole body is appended to <log>.full as well (it holds the system prompt and the
conversation, never a credential: the authorization headers are dropped).

Usage:  python -I mockllm.py <port> <log.jsonl> [--full]
Then:   ANTHROPIC_BASE_URL=http://127.0.0.1:<port>  ANTHROPIC_AUTH_TOKEN=<anything>  claude ...
⚠ Bound to 127.0.0.1 only; plain HTTP; no authentication. A throwaway: never expose it on a network.
"""
import json
import re
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

#: Serializes log writes: the server answers every request on its own thread.
LOG_LOCK = threading.Lock()
#: The JSONL request log (set by main); the full bodies go to LOG_PATH + ".full".
LOG_PATH = None
#: Whether every request body is logged too (main's --full). Bodies hold the system prompt and the
#: conversation — never a credential (the authorization headers are dropped).
FULL_LOG = False
#: A script anywhere in a message's text: `[[mock` ... `]]`, across lines.
SCRIPT_RE = re.compile(r"\[\[mock(.*?)\]\]", re.S)
#: Attempts of each `error ... N` step, keyed (session id, script text, response index): the only state
#: the server keeps. It grows with every distinct failing step and is never pruned — fine for a spike.
ATTEMPTS = {}
#: Guards ATTEMPTS across request threads.
ATTEMPTS_LOCK = threading.Lock()


def log_line(path, entry):
    """Append one JSON line to a log file.

    Args:
        path: The file to append to.
        entry: A JSON-serializable dict.
    Returns:
        None. Never raises: a failed log write must not break the response Claude Code is waiting for
        (an OSError is swallowed on purpose; the spike's log is evidence, not state).
    """
    try:
        with LOG_LOCK, open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass


def text_of(content):
    """The concatenated text of a message's content.

    Args:
        content: A message's `content`: a string, or a list of blocks (only `text` blocks count; a
            tool_result or an image contributes nothing).
    Returns:
        The text blocks joined with newlines ("" for None or a list without text).
    """
    if isinstance(content, str):
        return content
    return "\n".join(b.get("text", "") for b in content or [] if isinstance(b, dict) and b.get("type") == "text")


def parse_steps(src):
    """Parse the body of one `[[mock ...]]` into its steps.

    Args:
        src: The text between `[[mock` and `]]` (a leading `:` is allowed).
    Returns:
        A list of (kind, args) tuples; each arg is a JSON value (a quoted string, an object, an array) or
        a bare word (a status number, a tool name).
    Raises:
        ValueError: When a step does not start with a word, or a JSON argument does not parse (the
            caller answers with a "bad script" text instead of failing the request).
    """
    steps, i, dec = [], 0, json.JSONDecoder()
    src = src.strip().lstrip(":").strip()
    while i < len(src):
        while i < len(src) and src[i] in " \t\r\n;":
            i += 1
        if i >= len(src):
            break
        m = re.match(r"[A-Za-z_]+", src[i:])
        if not m:
            raise ValueError("expected a step name at %r" % src[i:i + 20])
        kind, args = m.group(0), []
        i += len(kind)
        while True:
            while i < len(src) and src[i] in " \t\r\n":
                i += 1
            if i >= len(src) or src[i] == ";":
                break
            if src[i] in "\"{[":
                val, i = dec.raw_decode(src, i)
                args.append(val)
            else:
                word = re.match(r"[^\s;]+", src[i:]).group(0)
                args.append(word)
                i += len(word)
        steps.append((kind, args))
    return steps


def build_responses(steps):
    """Group the steps into the model responses of one turn.

    Args:
        steps: The output of parse_steps.
    Returns:
        A list of responses, in turn order. A response is {"blocks": [(kind, args)...], "stop": reason}
        or {"error": [status, type, message]}. A `tool` step ends its response (stop_reason tool_use),
        `reply` ends the script (end_turn); trailing say/think steps form a last end_turn response.
        An `error` with a 4th argument becomes a ("fail", args) pseudo-block at the head of the NEXT
        response instead of a response of its own.
    Raises:
        ValueError: On an unknown step name.
    """
    responses, cur, pending_fail = [], [], None
    for kind, args in steps:
        if kind == "error":
            if len(args) > 3:
                pending_fail = args
                continue
            responses.append({"error": args})
            cur = []
            continue
        if pending_fail and not cur:
            cur.append(("fail", pending_fail))
            pending_fail = None
        if kind in ("say", "think", "delay", "drop"):
            cur.append((kind, args))
        elif kind == "tool":
            cur.append((kind, args))
            responses.append({"blocks": cur, "stop": "tool_use"})
            cur = []
        elif kind == "reply":
            cur.append(("say", args))
            responses.append({"blocks": cur, "stop": "end_turn"})
            cur = []
            break
        else:
            raise ValueError("unknown step " + kind)
    if cur:
        responses.append({"blocks": cur, "stop": "end_turn"})
    return responses


def session_of(body):
    """Claude Code's session id, from the request's metadata.

    Args:
        body: The parsed request body.
    Returns:
        metadata.user_id's `session_id` (Claude Code sends user_id as a JSON STRING holding device_id,
        account_uuid and session_id), or "" when it is missing or does not parse.
    """
    try:
        return json.loads((body.get("metadata") or {}).get("user_id") or "{}").get("session_id", "")
    except (ValueError, AttributeError):
        return ""


def side_call(body):
    """Classify a request as one of Claude Code's side requests, which must NOT run the turn's script.

    Args:
        body: The parsed request body.
    Returns:
        (kind, response) for a side request ("title" or "compact"), or None for a main-loop request.
        ⚠ The markers are Claude Code 2.1.291's own texts; a later version may change them, and an
        unrecognized side request then runs the script a second time (seen in the spike before this
        rule existed: the title request answered with a Write tool call).
    """
    sysp = body.get("system")
    if isinstance(sysp, list):
        sysp = "\n".join(b.get("text", "") for b in sysp if isinstance(b, dict))
    msgs = body.get("messages") or []
    if "naming a coding session" in (sysp or ""):
        return "title", {"blocks": [("say", [json.dumps({"title": "Mock session title"})])], "stop": "end_turn"}
    # The LAST USER message, never simply the last message: 2.1.291 appends a role "system" message
    # after the compaction instruction, and the instruction shares its user message with the previous
    # tool_result (measured: reading msgs[-1] missed it and answered the compaction with a script step).
    users = [m for m in msgs if m.get("role") == "user"]
    last = text_of(users[-1].get("content")) if users else ""
    if "CRITICAL: Respond with TEXT ONLY" in last:
        return "compact", {"blocks": [("say", ["<summary>Mock compaction summary.</summary>"])], "stop": "end_turn"}
    return None


def pick_response(body):
    """Choose the response of one request.

    Args:
        body: The parsed request body.
    Returns:
        (response, why): the response (see build_responses) and a short label for the log ("side:title",
        "step 2/3", "default", "past-end", "step 1/1 attempt 2 (fail)").
    Raises:
        ValueError: When the script does not parse (the caller turns it into a "bad script" reply).
    """
    side = side_call(body)
    if side:
        return side[1], "side:" + side[0]
    msgs = body.get("messages") or []
    scripted_at = None
    # The newest USER message holding a script. Not simply the last message: Claude Code 2.1.291 appends
    # role "system" messages (the environment, reminders) after the user's prompt.
    for idx in range(len(msgs) - 1, -1, -1):
        if msgs[idx].get("role") == "user" and SCRIPT_RE.search(text_of(msgs[idx].get("content"))):
            scripted_at = idx
            break
    if scripted_at is None:
        last = text_of(msgs[-1].get("content")) if msgs else ""
        return {"blocks": [("say", ["mock: " + last.strip()[:60]])], "stop": "end_turn"}, "default"
    script = SCRIPT_RE.findall(text_of(msgs[scripted_at].get("content")))[-1]
    responses = build_responses(parse_steps(script))
    k = sum(1 for m in msgs[scripted_at + 1:] if m.get("role") == "assistant")
    if k >= len(responses):
        return {"blocks": [("say", ["mock: script done"])], "stop": "end_turn"}, "past-end"
    resp = responses[k]
    blocks = resp.get("blocks") or []
    if blocks and blocks[0][0] == "fail":
        fail = blocks[0][1]
        key = (session_of(body), script, k)
        with ATTEMPTS_LOCK:
            ATTEMPTS[key] = ATTEMPTS.get(key, 0) + 1
            attempt = ATTEMPTS[key]
        if attempt <= int(fail[3]):
            return {"error": fail[:3]}, "step %d/%d attempt %d (fail)" % (k + 1, len(responses), attempt)
        return dict(resp, blocks=blocks[1:]), "step %d/%d attempt %d" % (k + 1, len(responses), attempt)
    return resp, "step %d/%d" % (k + 1, len(responses))


class Handler(BaseHTTPRequestHandler):
    """One HTTP/1.1 request of Claude Code (the server runs each on its own thread)."""

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        """Silence the default stderr access log (the JSONL log is the record).

        Args:
            fmt: The format string BaseHTTPRequestHandler passes.
            *args: Its arguments.
        Returns:
            None.
        """

    def _read_body(self):
        """Read and parse the request body.

        Returns:
            The parsed JSON object, {} for an empty body, or {"_raw": <first 2000 chars>} for a body that is
            not JSON (logged, then answered like any request).
        Raises:
            OSError: When the socket fails mid-read (the server's thread then ends the request).
        """
        n = int(self.headers.get("content-length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw.decode("utf-8")) if raw else {}
        except ValueError:
            return {"_raw": raw[:2000].decode("utf-8", "replace")}

    def _send_json(self, status, obj):
        """Answer with a whole JSON body.

        Args:
            status: The HTTP status.
            obj: The JSON-serializable answer.
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

    def _chunk(self, text):
        """Write one HTTP chunk and flush it, so Claude Code sees each SSE event at once.

        Args:
            text: The chunk's text (UTF-8 encoded here).
        Returns:
            None.
        Raises:
            OSError: When the client has gone away (an interrupt aborts the request).
        """
        data = text.encode("utf-8")
        self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
        self.wfile.flush()

    def _event(self, name, obj):
        """Write one server-sent event.

        Args:
            name: The event name (message_start, content_block_delta, ...).
            obj: Its JSON data.
        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        self._chunk("event: %s\ndata: %s\n\n" % (name, json.dumps(obj)))

    def do_GET(self):
        """Answer GET /v1/models with one mock model, anything else with 404 (logged either way).

        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        log_line(LOG_PATH, {"t": time.time(), "method": "GET", "path": self.path, "ua": self.headers.get("user-agent")})
        if self.path.startswith("/v1/models"):
            model = {"type": "model", "id": "claude-mock-1", "display_name": "Mock", "created_at": "2026-01-01T00:00:00Z"}
            self._send_json(200, {"data": [model], "has_more": False, "first_id": model["id"], "last_id": model["id"]})
        else:
            self._send_json(404, {"type": "error", "error": {"type": "not_found_error", "message": self.path}})

    def do_POST(self):
        """Answer POST /v1/messages (streamed or whole) and /v1/messages/count_tokens; 404 otherwise.

        Returns:
            None.
        Raises:
            OSError: When the client goes away mid-answer (a ConPTY interrupt, a dropped stream).
        """
        t0 = time.time()
        body = self._read_body()
        msgs = body.get("messages") or []
        entry = {"t": t0, "method": "POST", "path": self.path, "session": session_of(body), "model": body.get("model"),
                 "stream": body.get("stream"), "n_msgs": len(msgs), "n_tools": len(body.get("tools") or []),
                 "tool_names": [t.get("name") for t in (body.get("tools") or [])],
                 "output_format": ((body.get("output_config") or {}).get("format") or {}).get("type"),
                 "last_role": msgs[-1].get("role") if msgs else None,
                 "last_text": text_of(msgs[-1].get("content"))[:300] if msgs else ""}
        if FULL_LOG:
            headers = {k: v for k, v in self.headers.items() if k.lower() not in ("authorization", "x-api-key")}
            log_line(LOG_PATH + ".full", {"t": t0, "path": self.path, "headers": headers, "body": body})
        if self.path.startswith("/v1/messages/count_tokens"):
            log_line(LOG_PATH, entry)
            self._send_json(200, {"input_tokens": 1234})
            return
        if not self.path.startswith("/v1/messages"):
            log_line(LOG_PATH, entry)
            self._send_json(404, {"type": "error", "error": {"type": "not_found_error", "message": self.path}})
            return
        try:
            resp, why = pick_response(body)
        except ValueError as e:
            resp, why = {"blocks": [("say", ["mock: bad script: %s" % e])], "stop": "end_turn"}, "bad-script"
        entry["why"] = why
        if "error" in resp:
            args = resp["error"]
            status = int(args[0]) if args else 500
            entry["answer"] = "error %d" % status
            log_line(LOG_PATH, entry)
            self._send_json(status, {"type": "error", "error": {"type": args[1] if len(args) > 1 else "api_error",
                                                                "message": args[2] if len(args) > 2 else "mock error"}})
            return
        entry["answer"] = [[kind, args[0] if args else None] for kind, args in resp["blocks"]]
        log_line(LOG_PATH, entry)
        if body.get("stream"):
            self._stream(body, resp)
        else:
            self._whole(body, resp)
        log_line(LOG_PATH, {"t": time.time(), "done": self.path, "ms": round((time.time() - t0) * 1000)})

    @staticmethod
    def _usage(out_tokens):
        """The usage object of an answer.

        Args:
            out_tokens: The output token count to report.
        Returns:
            A usage dict with fixed input and zero cache counts (the spike never models context size).
        """
        return {"input_tokens": 100, "output_tokens": out_tokens, "cache_creation_input_tokens": 0,
                "cache_read_input_tokens": 0}

    def _whole(self, body, resp):
        """Answer a non-streamed request with one message.

        Args:
            body: The request body (its model is echoed).
            resp: The chosen response (delay and drop blocks are ignored here).
        Returns:
            None.
        Raises:
            OSError: When the client has gone away.
        """
        content = []
        for kind, args in resp["blocks"]:
            if kind == "say":
                content.append({"type": "text", "text": args[0] if args else ""})
            elif kind == "think":
                content.append({"type": "thinking", "thinking": args[0] if args else "", "signature": "mock-signature"})
            elif kind == "tool":
                content.append({"type": "tool_use", "id": "toolu_mock_" + uuid.uuid4().hex[:16], "name": args[0],
                                "input": args[1] if len(args) > 1 else {}})
        self._send_json(200, {"id": "msg_mock_" + uuid.uuid4().hex[:16], "type": "message", "role": "assistant",
                              "model": body.get("model", "claude-mock"), "content": content,
                              "stop_reason": resp["stop"], "stop_sequence": None, "usage": self._usage(10)})

    def _stream(self, body, resp):
        """Answer a streamed request with the Messages API's SSE sequence, one block at a time.

        Args:
            body: The request body (its model is echoed).
            resp: The chosen response. A `delay` block sleeps; a `drop` block shuts the socket down where
                the stream stands, so Claude Code sees a connection lost mid-response (it retries, then
                records a synthetic `isApiErrorMessage` line — the production incident shape).
        Returns:
            None.
        Raises:
            OSError: When the client goes away (an Esc in the TUI aborts the request).
        """
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("cache-control", "no-cache")
        self.send_header("transfer-encoding", "chunked")
        self.send_header("request-id", "req_mock_" + uuid.uuid4().hex[:12])
        self.end_headers()
        self._event("message_start", {"type": "message_start", "message": {
            "id": "msg_mock_" + uuid.uuid4().hex[:16], "type": "message", "role": "assistant",
            "model": body.get("model", "claude-mock"), "content": [], "stop_reason": None,
            "stop_sequence": None, "usage": self._usage(1)}})
        idx = 0
        for kind, args in resp["blocks"]:
            if kind == "delay":
                time.sleep(int(args[0]) / 1000.0 if args else 1.0)
                continue
            if kind == "drop":
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass  # already gone: the cut is what was asked for
                self.close_connection = True
                return
            if kind == "say":
                block, delta = {"type": "text", "text": ""}, {"type": "text_delta", "text": args[0] if args else ""}
            elif kind == "think":
                block, delta = {"type": "thinking", "thinking": ""}, {"type": "thinking_delta", "thinking": args[0] if args else ""}
            elif kind == "tool":
                block = {"type": "tool_use", "id": "toolu_mock_" + uuid.uuid4().hex[:16], "name": args[0], "input": {}}
                delta = {"type": "input_json_delta", "partial_json": json.dumps(args[1] if len(args) > 1 else {})}
            else:
                continue
            self._event("content_block_start", {"type": "content_block_start", "index": idx, "content_block": block})
            self._event("content_block_delta", {"type": "content_block_delta", "index": idx, "delta": delta})
            if kind == "think":
                self._event("content_block_delta", {"type": "content_block_delta", "index": idx,
                                                    "delta": {"type": "signature_delta", "signature": "mock-signature"}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": idx})
            idx += 1
        self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": resp["stop"], "stop_sequence": None},
                                      "usage": {"output_tokens": 10}})
        self._event("message_stop", {"type": "message_stop"})
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


def main(argv):
    """Serve until killed.

    Args:
        argv: [port, log path] plus an optional "--full" (also log every request body).
    Returns:
        Never returns normally (serve_forever).
    Raises:
        SystemExit: On bad arguments.
        OSError: When the port is taken.
    """
    global LOG_PATH, FULL_LOG
    if len(argv) < 2:
        raise SystemExit(__doc__)
    port, LOG_PATH = int(argv[0]), argv[1]
    FULL_LOG = "--full" in argv[2:]
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print("mock listening on 127.0.0.1:%d" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main(sys.argv[1:])
