"""Unit tests of the mock's HTTP server (mockllm.server): the Messages API as Claude Code sees it, and the
control API as a test uses it. Each test starts its own server on a free port with a short turn floor.
"""
from __future__ import annotations

import http.client
import json
import select
import socket
import threading
import time

import pytest

from mockllm.client import MockClient
from mockllm.server import serve_in_thread
from mockllm.state import MockState

#: A tool list like Claude Code's main loop offers (enough for the shorthands the tests use).
TOOLS = [
    {"name": "Bash", "input_schema": {"type": "object", "required": ["command"], "properties": {"command": {"type": "string"}}}},
    {"name": "Write", "input_schema": {"type": "object", "required": ["file_path", "content"],
                                       "properties": {"file_path": {"type": "string"}, "content": {"type": "string"}}}},
    {"name": "AskUserQuestion", "input_schema": {"type": "object", "required": ["questions"],
                                                 "properties": {"questions": {"type": "array"}}}},
]
SESSION_META = {"user_id": json.dumps({"session_id": "sess-1"})}


@pytest.fixture()
def mock(tmp_path):
    """A running server with a 300 ms turn floor, and a client for it.

    Args:
        tmp_path: pytest's temporary folder (the request log goes there).
    Yields:
        (port, MockClient, MockState); the server is shut down after the test.
    """
    state = MockState(str(tmp_path / "requests.jsonl"), floor_ms=300)
    server, _ = serve_in_thread(state)
    port = server.server_address[1]
    yield port, MockClient(f"http://127.0.0.1:{port}"), state
    server.shutdown()


def post(port: int, body: dict, path: str = "/v1/messages", timeout: float = 15.0) -> tuple[int, bytes]:
    """POST a JSON body and read the whole answer.

    Args:
        port: The server's port.
        body: The request body.
        path: The URL path.
        timeout: The socket timeout.
    Returns:
        (status, raw body bytes).
    Raises:
        http.client.HTTPException / OSError: When the server cuts the connection (a `drop`).
    """
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request("POST", path, json.dumps(body), {"content-type": "application/json"})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp.status, data


def post_with_headers(port: int, body: dict, timeout: float = 15.0) -> tuple[int, dict, bytes]:
    """POST a Messages API body and read the whole answer with its headers.

    Args:
        port: The server's port.
        body: The request body.
        timeout: The socket timeout.
    Returns:
        (status, {lower-case header name: value}, raw body bytes).
    Raises:
        http.client.HTTPException / OSError: When the server cuts the connection.
    """
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request("POST", "/v1/messages", json.dumps(body), {"content-type": "application/json"})
    resp = conn.getresponse()
    data = resp.read()
    headers = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, headers, data


class RawStream:
    """A Messages API request on a plain socket, read as raw bytes as they arrive — to see SILENCE, which an
    http.client response cannot show (a read that times out breaks it for good).

    Attributes:
        sock: The connection.
        data: Every byte received so far (headers, chunk sizes and events, undecoded).
    """

    def __init__(self, port: int, body: dict) -> None:
        """Connect and send the request.

        Args:
            port: The server's port.
            body: The request body.
        Returns:
            None.
        Raises:
            OSError: When the connection fails.
        """
        payload = json.dumps(body).encode("utf-8")
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.sock.sendall(b"POST /v1/messages HTTP/1.1\r\nHost: 127.0.0.1\r\ncontent-type: application/json\r\n"
                          b"content-length: %d\r\n\r\n" % len(payload) + payload)
        self.data = bytearray()

    def read_until(self, marker: bytes, timeout_s: float = 10.0) -> None:
        """Read until the received bytes hold a marker.

        Args:
            marker: The bytes to wait for.
            timeout_s: How long.
        Returns:
            None.
        Raises:
            AssertionError: When the marker did not arrive in time, or the server closed first.
        """
        end = time.monotonic() + timeout_s
        while marker not in self.data:
            left = end - time.monotonic()
            assert left > 0, f"{marker!r} did not arrive; got {bytes(self.data)[-300:]!r}"
            readable, _, _ = select.select([self.sock], [], [], left)
            if readable:
                chunk = self.sock.recv(65536)
                assert chunk, f"the server closed before {marker!r}; got {bytes(self.data)[-300:]!r}"
                self.data += chunk

    def quiet_for(self, seconds: float) -> bool:
        """Whether no byte arrives for a while (the bytes that do arrive are kept).

        Args:
            seconds: How long to listen.
        Returns:
            True when nothing arrived.
        """
        readable, _, _ = select.select([self.sock], [], [], seconds)
        if not readable:
            return True
        self.data += self.sock.recv(65536)
        return False

    def close(self) -> None:
        """Close the connection (a client that gives up).

        Returns:
            None.
        """
        self.sock.close()


def events(raw: bytes) -> list[tuple[str, dict]]:
    """Parse server-sent events.

    Args:
        raw: The streamed body (already de-chunked by http.client).
    Returns:
        [(event name, data dict)] in order.
    """
    out = []
    for block in raw.decode("utf-8").split("\n\n"):
        name, data = "", None
        for line in block.splitlines():
            if line.startswith("event: "):
                name = line[7:]
            elif line.startswith("data: "):
                data = json.loads(line[6:])
        if name:
            out.append((name, data))
    return out


def request(prompt: str, history: list | None = None, stream: bool = True, **extra) -> dict:
    """A main-loop request body: the scripted prompt, optional later messages, Claude Code's tools.

    Args:
        prompt: The user's prompt (with its script).
        history: Messages after the prompt (assistant tool calls, user tool results).
        stream: Whether to stream.
        **extra: More body fields (system, tools=..., ...).
    Returns:
        The body.
    """
    body = {"model": "claude-mock-1", "max_tokens": 1000, "stream": stream, "metadata": SESSION_META,
            "tools": TOOLS, "messages": [{"role": "user", "content": [{"type": "text", "text": prompt}]}] + (history or [])}
    body.update(extra)
    return body


def tool_round(name: str = "Bash", result: str = "ok") -> list:
    """The two messages one tool round adds: the assistant's call and the user's result.

    Args:
        name: The tool's name.
        result: The tool result's text.
    Returns:
        [assistant message, user message].
    """
    return [{"role": "assistant", "content": [{"type": "tool_use", "id": "t1", "name": name, "input": {}}]},
            {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "t1", "content": result}]}]


def texts(evts: list) -> str:
    """All text deltas of a stream, joined.

    Args:
        evts: Parsed events.
    Returns:
        The text.
    """
    return "".join(d["delta"].get("text", "") for n, d in evts if n == "content_block_delta" and d["delta"]["type"] == "text_delta")


def test_stream_sequence_and_text(mock):
    """A streamed reply follows the Messages API's event order and carries the scripted text.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong order or text.
    """
    port, _, _ = mock
    status, raw = post(port, request('hi [[mock say "a"; reply "b"]]'))
    evts = events(raw)
    assert status == 200
    names = [n for n, _ in evts]
    assert names[0] == "message_start" and names[-2:] == ["message_delta", "message_stop"]
    assert texts(evts) == "ab"
    assert evts[-2][1]["delta"]["stop_reason"] == "end_turn"


def test_tool_then_next_response_by_assistant_count(mock):
    """The first request gets the tool call; the request carrying its result gets the next response.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong response choice.
    """
    port, client, _ = mock
    prompt = 'go [[mock bash "echo hi"; expect "hi"; reply "done"]]'
    _, raw = post(port, request(prompt))
    first = events(raw)
    starts = [d["content_block"] for n, d in first if n == "content_block_start"]
    assert starts[0]["type"] == "tool_use" and starts[0]["name"] == "Bash"
    assert first[-2][1]["delta"]["stop_reason"] == "tool_use"
    _, raw = post(port, request(prompt, tool_round(result="hi there")))
    assert texts(events(raw)) == "done"
    assert client.expectations()[-1]["ok"] is True
    main = client.requests(kind="main")
    assert [e["k"] for e in main] == [0, 1]
    assert client.prompts()[0]["user_text"].startswith("go ")


def test_expect_mismatch_is_recorded(mock):
    """A tool result that does not match `expect` is recorded as failed (the test's ambient check reads it).

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When the mismatch is not recorded.
    """
    port, client, _ = mock
    prompt = 'go [[mock bash "x"; expect "^yes$"; reply "done"]]'
    post(port, request(prompt, tool_round(result="no")))
    assert client.expectations()[-1]["ok"] is False


def test_whole_message(mock):
    """A non-streamed request gets one JSON message with the scripted blocks.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong message.
    """
    port, _, _ = mock
    status, raw = post(port, request('[[mock think "t"; reply "r"]]', stream=False))
    msg = json.loads(raw)
    assert status == 200 and [b["type"] for b in msg["content"]] == ["thinking", "text"]
    assert msg["stop_reason"] == "end_turn"


def test_title_side_request_uses_the_script_title(mock):
    """The session-title request never runs the script; it answers the script's `title` as JSON.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When the title request runs the script.
    """
    port, client, _ = mock
    body = request('x [[mock title "Lexer work"; bash "rm -rf"; reply "y"]]', stream=False, tools=[],
                   system=[{"type": "text", "text": "You are naming a coding session ..."}])
    _, raw = post(port, body)
    msg = json.loads(raw)
    assert json.loads(msg["content"][0]["text"]) == {"title": "Lexer work"}
    assert client.requests()[-1]["kind"] == "title"


def test_compact_side_request(mock):
    """The compaction request (marker in the LAST USER message, a system message after it) gets a summary.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When the compaction runs the script.
    """
    port, client, _ = mock
    history = [{"role": "assistant", "content": [{"type": "text", "text": "ok"}]},
               {"role": "user", "content": [{"type": "text", "text": "CRITICAL: Respond with TEXT ONLY. Summarize."}]},
               {"role": "system", "content": "reminder"}]
    _, raw = post(port, request('[[mock bash "x"; reply "y"]]', history))
    assert "Mock compaction summary" in texts(events(raw))
    assert client.requests()[-1]["kind"] == "compact"


def test_unknown_side_request_is_flagged_not_scripted(mock):
    """A request without tools and without a known marker is answered generically and logged as unknown.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When it runs the script.
    """
    port, client, _ = mock
    _, raw = post(port, request('[[mock bash "x"]]', tools=[]))
    assert texts(events(raw)) == "Mock."
    assert client.requests()[-1]["kind"] == "unknown"


def test_permanent_error(mock):
    """`error` without times answers that HTTP error on every attempt.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong status or body.
    """
    port, _, _ = mock
    for _ in range(2):
        status, raw = post(port, request('[[mock error 400 invalid_request_error "prompt is too long"]]'))
        assert status == 400
        assert json.loads(raw)["error"]["type"] == "invalid_request_error"


def test_error_times_then_success(mock):
    """`error ... times=1` fails the first attempt only; the retry is served.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong sequence.
    """
    port, _, _ = mock
    body = request('[[mock error 529 overloaded_error "busy" times=1; reply "after"]]')
    assert post(port, body)[0] == 529
    status, raw = post(port, body)
    assert status == 200 and texts(events(raw)) == "after"


def test_refusal_stop_details_and_next(mock):
    """A `stop refusal` response carries stop_details on message_delta (streamed) and on the message (whole); the
    request after it (Claude Code's own retry) gets the step after `next`.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On missing details or a wrong response choice.
    """
    port, client, _ = mock
    prompt = 'refuse [[mock say "no"; stop refusal bio "Blocked."; next; reply "retried"]]'
    _, raw = post(port, request(prompt))
    delta = events(raw)[-2][1]["delta"]
    assert delta["stop_reason"] == "refusal"
    assert delta["stop_details"] == {"type": "refusal", "category": "bio", "explanation": "Blocked."}
    refused = {"role": "assistant", "content": [{"type": "text", "text": "no"}]}
    nudge = {"role": "user", "content": [{"type": "text", "text": "continuing once with that noted"}]}
    _, raw = post(port, request(prompt, [refused, nudge]))
    assert texts(events(raw)) == "retried"
    assert [e["k"] for e in client.requests(kind="main")] == [0, 1]
    status, raw = post(port, request(prompt, stream=False))
    msg = json.loads(raw)
    assert status == 200 and msg["stop_reason"] == "refusal" and msg["stop_details"]["category"] == "bio"
    _, raw = post(port, request('[[mock reply "plain"]]'))
    assert "stop_details" not in events(raw)[-2][1]["delta"]


def test_turn_floor_holds_the_final_response(mock):
    """The final response of a turn is not sent before the floor (300 ms here) since the turn's first request.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When it comes earlier, or `fast` does not skip it.
    """
    port, _, _ = mock
    t0 = time.monotonic()
    post(port, request('[[mock reply "x"]]'))
    assert time.monotonic() - t0 >= 0.28
    t0 = time.monotonic()
    post(port, request('[[mock fast; reply "y"]]'))
    assert time.monotonic() - t0 < 0.25


def test_hold_and_release(mock):
    """A held stream waits until the test releases the gate; the client can see it waiting.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When the hold does not wait or does not continue.
    """
    port, client, _ = mock
    result = {}

    def run():
        """Send the held request (on a thread: it blocks until the release).

        Returns:
            None.
        """
        result["raw"] = post(port, request('[[mock say "before"; hold g1; reply "after"]]'))[1]

    thread = threading.Thread(target=run)
    thread.start()
    client.wait_holding("g1", timeout_s=5)
    assert "raw" not in result
    client.release("g1")
    thread.join(5)
    assert texts(events(result["raw"])) == "beforeafter"


def test_hold_fail_before_start_is_an_http_error(mock):
    """A gate failed before the stream started answers a real HTTP error status.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong status.
    """
    port, client, _ = mock
    client.fail("g2", status=529, message="busy")
    status, raw = post(port, request('[[mock hold g2; reply "never"]]'))
    assert status == 529 and json.loads(raw)["error"]["message"] == "busy"


def test_hold_fail_mid_stream_ends_the_stream_with_an_error_event(mock):
    """A gate failed after the stream started sends an `error` event, then ends the response normally (the last
    chunk; no message_delta / message_stop) — the API's way, which Claude Code reads as a server error mid-response.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a missing error event, a message_stop, or a cut instead of an end.
    """
    port, client, _ = mock
    result = {}

    def run():
        """Send the held request (on a thread: it blocks until the gate is failed).

        Returns:
            None.
        """
        result["status"], result["raw"] = post(port, request('[[mock say "half"; hold g3; reply "never"]]'))

    thread = threading.Thread(target=run)
    thread.start()
    client.wait_holding("g3", timeout_s=5)
    client.fail("g3", status=529, etype="overloaded_error", message="Overloaded")
    thread.join(5)
    evts = events(result["raw"])
    names = [n for n, _ in evts]
    assert result["status"] == 200 and texts(evts) == "half"
    assert names[-1] == "error" and evts[-1][1]["error"] == {"type": "overloaded_error", "message": "Overloaded"}
    assert "message_stop" not in names and "message_delta" not in names


def test_drop_cuts_the_connection(mock):
    """`drop` cuts the stream without message_stop (Claude Code sees a lost connection).

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When the stream completes.
    """
    port, _, _ = mock
    with pytest.raises((http.client.HTTPException, OSError)):
        post(port, request('[[mock say "half"; drop]]'))


def test_error_headers_reach_the_client_and_the_log(mock):
    """An `error … retry=no retry_after=120` answers with the API's x-should-retry / retry-after headers (what makes
    Claude Code give up at once), and the request log records them.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a missing header.
    """
    port, client, _ = mock
    status, headers, raw = post_with_headers(port, request('[[mock error 429 rate_limit_error "slow" retry=no retry_after=120]]'))
    assert status == 429 and json.loads(raw)["error"]["type"] == "rate_limit_error"
    assert headers.get("x-should-retry") == "false" and headers.get("retry-after") == "120"
    assert client.requests(kind="main")[-1]["headers"] == {"x-should-retry": "false", "retry-after": "120"}
    status, headers, _ = post_with_headers(port, request('[[mock error 529 overloaded_error "busy"]]'))
    assert status == 529 and "x-should-retry" not in headers and "retry-after" not in headers


def test_gate_failed_with_headers_before_the_stream_started(mock):
    """A gate failed with headers answers a held request that has sent nothing yet with that status AND those headers.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong status or a missing header.
    """
    port, client, _ = mock
    client.fail("g4", status=429, etype="rate_limit_error", message="slow", headers={"retry-after": "90"})
    status, headers, raw = post_with_headers(port, request('[[mock hold g4; reply "never"]]'))
    assert status == 429 and headers.get("retry-after") == "90"
    assert json.loads(raw)["error"]["message"] == "slow"


def test_stall_starts_the_stream_then_sends_nothing_until_released(mock):
    """A `stall` after some text sends NO byte while it waits (a `hold` would ping every 2 s), then the rest of the
    response once the gate is released; a stall before any output starts the stream at once (message_start), where a
    hold would send nothing at all.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a byte during the stall, or a missing part of the response.
    """
    port, client, _ = mock
    s = RawStream(port, request('[[mock say "half"; stall g5; reply "rest"]]'))
    try:
        s.read_until(b"content_block_stop")  # the text block is complete: the stall comes after it
        client.wait_holding("g5", timeout_s=5)
        assert s.quiet_for(2.6), f"bytes arrived during the stall: {bytes(s.data)[-200:]!r}"
        client.release("g5")
        s.read_until(b"0\r\n\r\n")
        assert b'"rest"' in s.data and b"message_stop" in s.data and b"event: ping" not in s.data
    finally:
        s.close()
    t = RawStream(port, request('[[mock stall g6; reply "late"]]'))
    try:
        t.read_until(b"message_start", timeout_s=3)
        assert b"content_block_start" not in t.data
        client.release("g6")
        t.read_until(b"0\r\n\r\n")
        assert b'"late"' in t.data
    finally:
        t.close()


def test_stall_notices_a_client_that_gives_up(mock):
    """A client that closes the connection during a stall (Claude Code's watchdog) ends the mock's wait without a
    release: the request is logged as `client_gone` and the gate has no waiting stream any more.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When the mock keeps waiting.
    """
    port, client, _ = mock
    s = RawStream(port, request('[[mock say "half"; stall g7; reply "never"]]'))
    s.read_until(b'"half"')
    client.wait_holding("g7", timeout_s=5)
    s.close()
    end = time.monotonic() + 8
    while time.monotonic() < end and not client.requests(kind="main")[-1].get("client_gone"):
        time.sleep(0.2)
    assert client.requests(kind="main")[-1].get("client_gone"), client.requests(kind="main")[-1]
    assert client.gates()["g7"]["waiting"] == 0


def test_size_filler_streams_whole(mock):
    """`say size=<n>` streams exactly n characters of filler, and `write <path> size=<n>` sends a Write whose content
    is n characters long.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong length.
    """
    port, _, _ = mock
    _, raw = post(port, request('[[mock say size=5000; reply "end"]]'))
    text = texts(events(raw))
    assert len(text) == 5003 and text.startswith("filler line 000001 ") and text.endswith("end")
    _, raw = post(port, request('[[mock write C:/w/big.txt size=300000; reply "ok"]]'))
    deltas = [d["delta"]["partial_json"] for n, d in events(raw)
              if n == "content_block_delta" and d["delta"]["type"] == "input_json_delta"]
    assert len(json.loads("".join(deltas))["content"]) == 300000


def test_bad_script_and_bad_tool_input_answer_and_flag(mock):
    """A script error and a tool input the schema rejects are answered as text and flagged in the log.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: When they are not flagged.
    """
    port, client, _ = mock
    _, raw = post(port, request('[[mock replyy "x"]]'))
    assert "bad script" in texts(events(raw))
    assert client.requests()[-1]["flag"] == "script_error"
    _, raw = post(port, request('[[mock tool Bash {"cmd": "x"}; reply "y"]]'))
    assert "bad tool input" in texts(events(raw))


def test_default_reply_and_count_tokens_and_models(mock):
    """An unscripted turn gets the default reply; count_tokens and /v1/models answer.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong answer.
    """
    port, client, _ = mock
    _, raw = post(port, request("Continue from where you left off."))
    assert texts(events(raw)) == "OK"
    status, raw = post(port, request("x"), path="/v1/messages/count_tokens")
    assert status == 200 and json.loads(raw)["input_tokens"] > 0
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", "/v1/models")
    assert json.loads(conn.getresponse().read())["data"][0]["id"] == "claude-mock-1"
    conn.close()


def test_reset_and_since(mock):
    """`since` reads only newer requests; reset forgets the log.

    Args:
        mock: The fixture.
    Returns:
        None.
    Raises:
        AssertionError: On a wrong filter or reset.
    """
    port, client, _ = mock
    post(port, request('[[mock fast; reply "1"]]'))
    mark = client.last_seq()
    post(port, request('[[mock fast; reply "2"]]'))
    assert len(client.requests(since=mark)) == 1
    client.reset()
    assert client.requests() == []
