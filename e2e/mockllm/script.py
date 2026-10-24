"""The `[[mock ...]]` script language: a test writes the model's answers INTO the prompt it sends.

A prompt such as ``fix it [[mock bash "git status"; reply "done"]]`` makes the mock answer the turn's first
request with a Bash tool call and, once Claude Code sends the tool's result back, the second request with
"done" and end_turn. The language is doc/agentmaster/E2E_TESTING.md §4.3; this module is its parser and
the planner that turns a script into the model RESPONSES of one turn. It is pure (no I/O, no clock), so the
unit tests pin every rule.

Steps (separated by ``;``; arguments are JSON values or bare words):

    say "<text>" | say size=<n>         a text block; the response goes on (size=<n>: n characters of numbered
                                        filler lines instead of a text — a big answer without a big prompt)
    think "<text>" | think size=<n>     a thinking block (a dummy signature is added when streamed)
    tool <Name> {json}                  a tool_use block; ENDS the response (stop_reason tool_use)
    tools [["Name", {json}], ...]       parallel tool_use blocks in one response; ends it
    reply "<text>" | reply size=<n>     a text block + end_turn; ENDS the script
    stream <chars> <ms>                 later text / thinking streams in chunks of <chars> every <ms>
    delay <dur>                         pause the stream ("500ms", "3s", or a bare number of ms)
    hold <gate>                         pause until the test releases (or fails) the named gate; a started
                                        stream sends a ping every 2 s meanwhile, so the connection never looks idle
    stall <gate>                        go SILENT until the test releases (or fails) the named gate: the stream
                                        is started (headers + message_start) and then sends no byte at all, no
                                        ping — what Claude Code's stream idle watchdog notices (E2E_TESTING.md
                                        §4.3); a client that gives up meanwhile is noticed and logged
    error <status> <type> "<msg>" [times=N] [retry=no|yes] [retry_after=<s>]
                                        answer with an HTTP error; with times=N only the first N attempts
                                        of the NEXT response fail, then it is served (a retried request);
                                        retry=no sends the API's `x-should-retry: false` (Claude Code then never
                                        retries a 429 / 5xx), retry=yes `x-should-retry: true`; retry_after=<s>
                                        the `retry-after` header (Claude Code gives up at once when it is over 60 s)
    drop [after=N]                      cut the connection (after N more SSE events of this response)
    stop <reason>                       override this response's stop_reason (max_tokens, refusal, ...)
    stop refusal [<category>] ["<explanation>"]
                                        a refusal with the API's stop_details {type: refusal, category,
                                        explanation} (defaults: "cyber" and a fixed explanation), which
                                        Claude Code reads off message_delta to word its refusal message
    next                                end this response here (end_turn unless a `stop` changed it); the
                                        following steps answer the NEXT request of the turn — for the turns
                                        Claude Code continues by itself (after max_tokens, a refusal's retry,
                                        a Stop hook that blocks)
    usage in=<n> out=<n> cache_read=<n> cache_write=<n>
                                        this response's usage numbers (context displays)
    title "<t>"                         what the session-title side request answers for this session
    fast                                no 2.5 s turn floor for this turn
    expect "<regex>"                    the tool result the NEXT response receives must match
    use @<scenario>                     splice the steps of <scenarios dir>/<scenario>.mock
    ask "<question>" A,B[,C,D]          shorthand: an AskUserQuestion tool call with those options
    bash "<cmd>"                        shorthand: Bash {"command": cmd}
    write <path> "<text>"               shorthand: Write {"file_path": path, "content": text}
    write <path> size=<n>               the same with n characters of filler as the content (a transcript that
                                        grows by megabytes in one turn: the Write's input is written into it)
    edit <path> "<old>" "<new>"         shorthand: Edit {"file_path": path, "old_string": ..., "new_string": ...}
    read <path>                         shorthand: Read {"file_path": path}
    agent "<prompt>"                    shorthand: the subagent tool (Agent or Task, whichever the request
                                        lists) with that prompt — which may hold a nested [[mock ...]]

WHICH RESPONSE A REQUEST GETS is the count of assistant messages after the scripted user message: stateless,
so a retried request gets the same answer (E2E_TESTING.md §4.3).
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

#: The words a script may use as step names; anything else is a ScriptError (fails loudly, never guessed).
STEP_NAMES = frozenset({
    "say", "think", "tool", "tools", "reply", "stream", "delay", "hold", "stall", "error", "drop", "stop", "usage",
    "title", "fast", "expect", "use", "ask", "bash", "write", "edit", "read", "agent", "next",
})

#: The options an `error` step accepts; any other ``key=value`` word on it is a typo and fails the script.
ERROR_OPTIONS = frozenset({"times", "retry", "retry_after"})

#: The largest `size=<n>` a script may ask for (characters). A filler is built in memory and sent in one SSE event,
#: so the bound keeps a typo (an extra zero or three) from exhausting the mock's memory; 64 MB is far above anything
#: a test needs (a transcript over the scanner's 1 MiB window takes 1-4 MB).
MAX_FILLER_CHARS = 64 * 1024 * 1024

#: The width of one filler line, its newline included.
FILLER_LINE_CHARS = 80

#: The refusal category a bare `stop refusal` reports (the one this PC's own refusals carried: cyber).
DEFAULT_REFUSAL_CATEGORY = "cyber"
#: The refusal explanation a `stop refusal` without one reports (Claude Code shows it; never parses it).
DEFAULT_REFUSAL_EXPLANATION = "This request was blocked by the mock model's scripted refusal."

#: The opening marker of a script inside a message's text.
OPEN = "[[mock"


class ScriptError(ValueError):
    """A script that does not parse or plan (a typo must fail the test loudly, never run as a guess)."""


class Quoted(str):
    """A JSON string argument of a script (`"a=b"`), as opposed to a BARE word (`times=2`, `529`, `Left,Right`).

    Only a bare word can be a ``key=value`` option: a quoted text that happens to hold "=" (`bash "FOO=1 make"`,
    `say "x = 1"`) is the step's text. Before this type both came out of the parser as plain strings and such a text
    was taken for an option (the step then reported a missing text). A Quoted compares equal to the same plain string
    and serializes as one, so everything downstream (tool inputs, element texts, the unit tests' comparisons) is
    unchanged; an argument list a caller builds by hand (plain strings) keeps the old rule.
    """


@dataclass
class Element:
    """One item of a response's event plan, in order.

    Attributes:
        kind: "text", "thinking", "tool_use" (content blocks), or "delay", "hold", "stall", "drop" (stream control).
        text: The text of a text / thinking block.
        name: The tool's name (tool_use), or the gate's name (hold, stall).
        input: The tool's input object (tool_use).
        ms: The pause in milliseconds (delay).
        after: For drop: how many more SSE events to send before the cut (0 = cut here).
        chunk: For text / thinking: stream in chunks of this many characters (0 = one delta).
        chunk_ms: The pause between those chunks.
    """

    kind: str
    text: str = ""
    name: str = ""
    input: dict = field(default_factory=dict)
    ms: int = 0
    after: int = 0
    chunk: int = 0
    chunk_ms: int = 0


@dataclass
class Response:
    """One model response of a turn: what one request receives.

    Attributes:
        elements: The ordered plan (blocks + stream control).
        stop: The stop_reason ("tool_use" after tool calls, else "end_turn" unless a `stop` step changed it).
        fail: (status, type, message, times) of an `error` step: times None = every attempt fails (the
            response is the error), times N = the first N attempts fail, then the elements are served.
        fail_headers: The HTTP headers the failing attempts carry ({"x-should-retry": "false"}, {"retry-after":
            "120"}; empty = none). Kept apart from `fail`, whose 4-tuple the tests and the server unpack.
        usage: Overrides of the usage numbers ({"input_tokens": n, ...}).
        expect: A regex the tool result arriving WITH this response's request must match ("" = none).
        final: True when this response ends the turn (end_turn / a stop override other than tool_use);
            the server applies the turn floor to it.
        stop_details: The API's structured stop details sent with the stop reason ({"type": "refusal",
            "category": ..., "explanation": ...} for `stop refusal`), or None (the key is then left out).
    """

    elements: list = field(default_factory=list)
    stop: str = "end_turn"
    fail: Optional[tuple] = None
    fail_headers: dict = field(default_factory=dict)
    usage: dict = field(default_factory=dict)
    expect: str = ""
    final: bool = True
    stop_details: Optional[dict] = None


@dataclass
class Plan:
    """A whole script, planned.

    Attributes:
        responses: The responses of the turn, in order.
        title: The answer for this session's title side request ("" = the server's default).
        fast: True disables the turn floor for this turn.
        source: The script text the plan came from (the key of attempt counters and floors).
    """

    responses: list
    title: str = ""
    fast: bool = False
    source: str = ""


def find_scripts(text: str) -> list[str]:
    """Every `[[mock ...]]` body in a text, in order.

    Unlike a non-greedy regex, the scanner skips JSON strings, so a script may hold a nested script inside
    a string argument (``agent "[[mock reply \\"sub\\"]]"``) without its ``]]`` ending the outer one.

    Args:
        text: A message's text.
    Returns:
        The bodies (the text between ``[[mock`` and the matching ``]]``), in order of appearance. An
        unterminated script is ignored (it is ordinary text then; the prompt was not a script).
    """
    bodies: list[str] = []
    i = 0
    while True:
        start = text.find(OPEN, i)
        if start < 0:
            return bodies
        j = start + len(OPEN)
        end = -1
        while j < len(text):
            ch = text[j]
            if ch == '"':
                # Skip a JSON string, honoring backslash escapes, so a "]]" inside it is not the end.
                j += 1
                while j < len(text) and text[j] != '"':
                    j += 2 if text[j] == "\\" else 1
                j += 1
                continue
            if text.startswith("]]", j):
                end = j
                break
            j += 1
        if end < 0:
            return bodies
        bodies.append(text[start + len(OPEN):end])
        i = end + 2


def _parse_duration(word: Any) -> int:
    """A delay argument in milliseconds.

    Args:
        word: "500ms", "3s", "1.5s", a bare number (milliseconds — the spike's form), or a JSON number.
    Returns:
        The milliseconds (int, >= 0).
    Raises:
        ScriptError: When the value is not a duration.
    """
    if isinstance(word, (int, float)) and not isinstance(word, bool):
        return max(0, int(word))
    m = re.fullmatch(r"(\d+(?:\.\d+)?)(ms|s)?", str(word).strip())
    if not m:
        raise ScriptError(f"not a duration: {word!r} (use 500ms, 3s or a number of ms)")
    value, unit = float(m.group(1)), m.group(2) or "ms"
    return int(value * 1000) if unit == "s" else int(value)


def parse_steps(src: str) -> list[tuple[str, list]]:
    """Parse the body of one script into (step name, arguments).

    Args:
        src: The text between ``[[mock`` and ``]]`` (a leading ``:`` is allowed, the spike's form).
    Returns:
        The steps in order. An argument is a JSON value (string, object, array, number) or a bare word
        (a tool name, a status, ``key=value``, a path).
    Raises:
        ScriptError: On a step that does not start with a known name, or a JSON argument that does not
            parse. A body with no step at all is a bare reply (``[[mock some words]]``) — see plan().
    """
    steps: list[tuple[str, list]] = []
    decoder = json.JSONDecoder()
    s = src.strip()
    if s.startswith(":"):
        s = s[1:].strip()
    i = 0
    while i < len(s):
        while i < len(s) and s[i] in " \t\r\n;":
            i += 1
        if i >= len(s):
            break
        m = re.match(r"[A-Za-z_]+", s[i:])
        if not m:
            raise ScriptError(f"expected a step name at {s[i:i + 30]!r}")
        name = m.group(0)
        if name not in STEP_NAMES:
            raise ScriptError(f"unknown step {name!r} (known: {', '.join(sorted(STEP_NAMES))})")
        i += len(name)
        args: list = []
        while True:
            while i < len(s) and s[i] in " \t\r\n":
                i += 1
            if i >= len(s) or s[i] == ";":
                break
            if s[i] in '"{[':
                try:
                    value, i = decoder.raw_decode(s, i)
                except ValueError as error:
                    raise ScriptError(f"step {name!r}: a JSON argument does not parse at {s[i:i + 30]!r}: {error}") from None
                # A quoted text is marked, so a "=" inside it can never make it an option (Quoted).
                args.append(Quoted(value) if isinstance(value, str) else value)
            else:
                word = re.match(r"[^\s;]+", s[i:]).group(0)
                args.append(word)
                i += len(word)
        steps.append((name, args))
    return steps


def _is_option(a: Any) -> bool:
    """Whether a step argument is a ``key=value`` option.

    Args:
        a: One argument as parse_steps returned it (a JSON value — a string one is Quoted — or a bare word), or as a
            caller built it.
    Returns:
        True for a bare word holding "=" (not starting like a JSON object or array). A quoted text is never an option,
        whatever it holds.
    """
    return (isinstance(a, str) and not isinstance(a, Quoted) and "=" in a
            and not a.startswith(("{", "[")))


def _options(args: list) -> dict:
    """The ``key=value`` words among a step's arguments.

    Args:
        args: A step's arguments.
    Returns:
        {key: value-string} for every bare word holding "=".
    """
    out = {}
    for a in args:
        if _is_option(a):
            k, _, v = a.partition("=")
            out[k.strip().lower()] = v.strip()
    return out


def _positional(args: list) -> list:
    """The arguments that are not ``key=value`` words.

    Args:
        args: A step's arguments.
    Returns:
        The others, in order.
    """
    return [a for a in args if not _is_option(a)]


def _text_arg(name: str, args: list, index: int = 0) -> str:
    """A step's string argument.

    Args:
        name: The step (for the error).
        args: Its positional arguments.
        index: Which one.
    Returns:
        The string (a bare word is taken as text too: ``reply done``).
    Raises:
        ScriptError: When the argument is missing or not a string.
    """
    if index >= len(args):
        raise ScriptError(f"step {name!r} needs a text argument")
    value = args[index]
    if not isinstance(value, str):
        raise ScriptError(f"step {name!r}: argument {index + 1} must be a string, not {type(value).__name__}")
    return value


def filler(n: int) -> str:
    """Exactly `n` characters of numbered filler lines ("filler line 000001 ....\\n"), for `size=<n>`.

    The lines are numbered so a reader of a transcript or a terminal can tell where a big block was cut, and they are
    plain ASCII with no character Claude Code's input or its renderer treats specially. Deterministic: the same `n`
    always gives the same text (a retried request gets the same answer, E2E_TESTING.md §4.3).

    Args:
        n: The length (0 gives "").
    Returns:
        The text; its last line is cut to fit (it then has no newline).
    """
    if n <= 0:
        return ""
    lines = []
    total = 0
    number = 0
    while total < n:
        number += 1
        head = f"filler line {number:06d} "
        line = head + "." * (FILLER_LINE_CHARS - 1 - len(head)) + "\n"
        lines.append(line)
        total += len(line)
    return "".join(lines)[:n]


def _size_option(name: str, opts: dict) -> Optional[int]:
    """A step's `size=<n>` option.

    Args:
        name: The step (for the error).
        opts: Its ``key=value`` options.
    Returns:
        The size in characters, or None when the step has no size option.
    Raises:
        ScriptError: When the value is not a whole number from 0 to MAX_FILLER_CHARS.
    """
    if "size" not in opts:
        return None
    try:
        size = int(opts["size"])
    except ValueError:
        raise ScriptError(f"step {name!r}: size must be a whole number of characters, not {opts['size']!r}") from None
    if not 0 <= size <= MAX_FILLER_CHARS:
        raise ScriptError(f"step {name!r}: size must be 0..{MAX_FILLER_CHARS}, not {size}")
    return size


def _body_text(name: str, pos: list, opts: dict, index: int = 0) -> str:
    """The text a step carries: its string argument, or `size=<n>` characters of filler — exactly one of the two.

    Args:
        name: The step (for the error).
        pos: Its positional arguments.
        opts: Its ``key=value`` options.
        index: Which positional argument holds the text when no size is given.
    Returns:
        The text.
    Raises:
        ScriptError: When both or neither are given, or either is malformed (a size and a text together would leave
            the reader guessing which one the step sends).
    """
    size = _size_option(name, opts)
    if size is None:
        return _text_arg(name, pos, index)
    if index < len(pos):
        raise ScriptError(f"step {name!r} takes a text OR size=<n>, not both")
    return filler(size)


def _error_headers(opts: dict) -> dict:
    """The HTTP headers an `error` step's options ask for.

    Args:
        opts: The step's ``key=value`` options (already checked against ERROR_OPTIONS).
    Returns:
        {"x-should-retry": "false" | "true"} for retry=no | yes and {"retry-after": "<s>"} for retry_after=<s>; empty
        when neither is given.
    Raises:
        ScriptError: On a retry value that is not yes / no, or a retry_after that is not a number of seconds >= 0.
    """
    headers: dict = {}
    if "retry" in opts:
        word = opts["retry"].lower()
        if word in ("no", "false", "0"):
            headers["x-should-retry"] = "false"
        elif word in ("yes", "true", "1"):
            headers["x-should-retry"] = "true"
        else:
            raise ScriptError(f"step 'error': retry must be yes or no, not {opts['retry']!r}")
    if "retry_after" in opts:
        raw = opts["retry_after"]
        try:
            seconds = float(raw)
        except ValueError:
            raise ScriptError(f"step 'error': retry_after must be a number of seconds, not {raw!r}") from None
        if seconds < 0:
            raise ScriptError(f"step 'error': retry_after must not be negative, not {raw!r}")
        # The API sends whole seconds; a script's "1.5" is passed as written (Claude Code parses a float).
        headers["retry-after"] = raw
    return headers


def expand_shorthand(name: str, args: list, tool_names: Optional[set] = None) -> tuple[str, list]:
    """Rewrite a shorthand step into a `tool` step.

    Args:
        name: The step name (ask, bash, write, edit, read, agent; anything else is returned unchanged).
        args: Its arguments.
        tool_names: The tool names the request offers (the subagent tool is "Agent" on current Claude Code
            and "Task" on older builds; with no list, "Agent").
    Returns:
        ("tool", [ToolName, input]) for a shorthand, else (name, args) unchanged.
    Raises:
        ScriptError: On missing or malformed arguments (e.g. fewer than two options for ask).
    """
    pos = _positional(args)
    if name == "bash":
        return "tool", ["Bash", {"command": _text_arg(name, pos), "description": "e2e mock command"}]
    if name == "write":
        # write <path> "<text>", or write <path> size=<n> (filler content: megabytes in one turn's transcript line).
        return "tool", ["Write", {"file_path": _text_arg(name, pos, 0),
                                  "content": _body_text(name, pos, _options(args), 1)}]
    if name == "edit":
        return "tool", ["Edit", {"file_path": _text_arg(name, pos, 0), "old_string": _text_arg(name, pos, 1),
                                 "new_string": _text_arg(name, pos, 2)}]
    if name == "read":
        return "tool", ["Read", {"file_path": _text_arg(name, pos, 0)}]
    if name == "agent":
        tool = "Agent"
        if tool_names and "Agent" not in tool_names and "Task" in tool_names:
            tool = "Task"
        return "tool", [tool, {"description": "e2e mock subagent", "prompt": _text_arg(name, pos),
                               "subagent_type": "general-purpose"}]
    if name == "ask":
        question = _text_arg(name, pos, 0)
        if len(pos) < 2:
            raise ScriptError("step 'ask' needs options: ask \"Question?\" A,B[,C,D]")
        raw = pos[1]
        if isinstance(raw, list):
            labels = [str(x) for x in raw]
        else:
            labels = [x.strip() for x in str(raw).split(",") if x.strip()]
        if not 2 <= len(labels) <= 4:
            raise ScriptError(f"step 'ask' needs 2 to 4 options, got {len(labels)}")
        opts = _options(args)
        header = opts.get("header") or re.sub(r"[^A-Za-z0-9 ]", "", question)[:12].strip() or "Question"
        multi = opts.get("multi", "false").lower() in ("1", "true", "yes")
        return "tool", ["AskUserQuestion", {"questions": [{
            "question": question, "header": header, "multiSelect": multi,
            "options": [{"label": label, "description": f"{label} (mock option)"} for label in labels]}]}]
    return name, args


def plan(src: str, tool_names: Optional[set] = None, scenarios: Optional[Callable[[str], str]] = None) -> Plan:
    """Plan a script: parse it, splice scenarios, expand shorthands and group the steps into responses.

    Args:
        src: The script body (between ``[[mock`` and ``]]``).
        tool_names: The tools the request offers (for the subagent shorthand).
        scenarios: A function name -> script text for ``use @name`` (None: ``use`` is an error).
    Returns:
        The Plan. A body of PLAIN WORDS whose first word is not a step name is a bare reply:
        ``[[mock some words]]`` == ``reply "some words"``. A body holding a quote, a ``;`` or a ``{`` is
        always parsed as steps, so a misspelled step (``[[mock replyy "x"]]``) fails loudly instead of
        becoming a reply. ⚠ A bare reply must not START with a step name: ``[[mock read the docs]]`` is the
        `read` shorthand (a Read of "the"); write ``reply "read the docs"``.
    Raises:
        ScriptError: On any parse or planning error (unknown step, bad argument, a `use` loop, ...).
    """
    body = src.strip().lstrip(":").strip()
    first = re.match(r"[A-Za-z_]+", body)
    plain = not any(ch in body for ch in '";{')
    if body and plain and (not first or first.group(0) not in STEP_NAMES):
        return Plan(responses=[Response(elements=[Element("text", text=body)], stop="end_turn")], source=src)
    steps = parse_steps(body)
    # Splice `use @name` scenarios (depth-limited: a scenario that uses itself must not hang the server).
    for _ in range(8):
        if not any(name == "use" for name, _ in steps):
            break
        spliced: list = []
        for name, args in steps:
            if name != "use":
                spliced.append((name, args))
                continue
            if scenarios is None:
                raise ScriptError("step 'use' needs a scenarios folder (the server's --scenarios)")
            ref = _text_arg(name, _positional(args)).lstrip("@")
            spliced.extend(parse_steps(scenarios(ref)))
        steps = spliced
    else:
        raise ScriptError("'use' nests deeper than 8 levels (a scenario that uses itself?)")
    steps = [expand_shorthand(name, args, tool_names) for name, args in steps]
    return _group(steps, src)


def _group(steps: list, src: str) -> Plan:
    """Group planned steps into responses (see the module docstring for each step's effect).

    Args:
        steps: (name, args) with shorthands already expanded.
        src: The script text (kept on the Plan).
    Returns:
        The Plan.
    Raises:
        ScriptError: On bad arguments.
    """
    result = Plan(responses=[], source=src)
    cur = Response()
    chunk, chunk_ms = 0, 0
    pending_expect = ""
    pending_fail: Optional[tuple] = None
    pending_fail_headers: dict = {}

    def close(stop: str, final: bool) -> None:
        """End the current response and start the next one.

        Args:
            stop: The stop_reason unless a `stop` step already set one on it.
            final: Whether the response ends the turn.
        Returns:
            None.
        """
        nonlocal cur
        if cur.stop == "end_turn" or not cur.stop:
            cur.stop = stop
        cur.final = final and cur.stop != "tool_use"
        result.responses.append(cur)
        cur = Response()

    def start_if_new() -> None:
        """Attach the pending expect / fail to the response being built when it gets its first element.

        Returns:
            None.
        """
        nonlocal pending_expect, pending_fail, pending_fail_headers
        if pending_expect and not cur.expect:
            cur.expect, pending_expect = pending_expect, ""
        if pending_fail and cur.fail is None:
            cur.fail, pending_fail = pending_fail, None
            # The headers travel with the failure they belong to (an `error … times=N retry_after=5`).
            cur.fail_headers, pending_fail_headers = pending_fail_headers, {}

    for name, args in steps:
        pos, opts = _positional(args), _options(args)
        if name == "title":
            result.title = _text_arg(name, pos)
            continue
        if name == "fast":
            result.fast = True
            continue
        if name == "expect":
            pattern = _text_arg(name, pos)
            try:
                re.compile(pattern)
            except re.error as error:
                raise ScriptError(f"step 'expect': not a regex: {error}") from None
            # The expectation belongs to the NEXT response's request (it carries the tool result).
            if cur.elements:
                close("end_turn", True)
            pending_expect = pattern
            continue
        if name == "stream":
            if len(pos) < 2:
                raise ScriptError("step 'stream' needs <chars> <ms>")
            try:
                chunk, chunk_ms = max(0, int(pos[0])), _parse_duration(pos[1])
            except (TypeError, ValueError):
                raise ScriptError("step 'stream' needs two numbers: stream <chars> <ms>") from None
            continue
        if name == "error":
            if len(pos) < 1:
                raise ScriptError("step 'error' needs a status: error 529 overloaded_error \"msg\" [times=N]")
            unknown = sorted(set(opts) - ERROR_OPTIONS)
            if unknown:
                # A misspelled option (retry_afer=…) must fail loudly instead of silently sending no header.
                raise ScriptError(f"step 'error': unknown option(s) {unknown} (known: {sorted(ERROR_OPTIONS)})")
            try:
                status = int(pos[0])
            except (TypeError, ValueError):
                raise ScriptError(f"step 'error': not an HTTP status: {pos[0]!r}") from None
            etype = str(pos[1]) if len(pos) > 1 else "api_error"
            message = str(pos[2]) if len(pos) > 2 else "mock error"
            headers = _error_headers(opts)
            times = opts.get("times")
            if times is None and len(pos) > 3:
                times = pos[3]  # the spike's form: a fourth positional number
            if times is None:
                # A response of its own that never succeeds: every attempt fails (Claude Code retries, then
                # records its synthetic API-error line and ends the turn).
                if cur.elements:
                    close("end_turn", True)
                start_if_new()
                cur.fail = (status, etype, message, None)
                cur.fail_headers = headers
                close("end_turn", True)
                continue
            try:
                n = int(times)
            except (TypeError, ValueError):
                raise ScriptError(f"step 'error': times must be a number, not {times!r}") from None
            if cur.elements:
                close("end_turn", True)
            pending_fail = (status, etype, message, n)
            pending_fail_headers = headers
            continue
        if name == "next":
            # The END of a response without a tool call: what Claude Code continues by itself (a max_tokens
            # recovery, a refusal's retry, a Stop hook's block) is the next request of the same turn, and gets
            # the steps that follow. It is handled before start_if_new, so a pending `expect` / `error times=N`
            # stays with the response that follows; on an empty response (right after a tool call) it changes
            # nothing.
            if cur.elements or cur.fail or cur.expect:
                close("end_turn", True)
            continue
        start_if_new()
        if name in ("say", "think"):
            cur.elements.append(Element("text" if name == "say" else "thinking", text=_body_text(name, pos, opts),
                                        chunk=chunk, chunk_ms=chunk_ms))
        elif name == "delay":
            if not pos:
                raise ScriptError("step 'delay' needs a duration")
            cur.elements.append(Element("delay", ms=_parse_duration(pos[0])))
        elif name in ("hold", "stall"):
            # Both wait for the named gate; a `stall` starts the stream first and then stays silent (no ping), so the
            # silence is what the client sees — server.py's Stream.run.
            cur.elements.append(Element(name, name=_text_arg(name, pos)))
        elif name == "drop":
            after = opts.get("after", "0")
            try:
                cur.elements.append(Element("drop", after=max(0, int(after))))
            except ValueError:
                raise ScriptError(f"step 'drop': after must be a number, not {after!r}") from None
        elif name == "stop":
            cur.stop = _text_arg(name, pos)
            if cur.stop == "refusal":
                # The API sends a refusal's category and explanation with the stop reason (stop_details on
                # message_delta); Claude Code words its "safeguards flagged this message" text from them.
                category = _text_arg(name, pos, 1) if len(pos) > 1 else DEFAULT_REFUSAL_CATEGORY
                explanation = _text_arg(name, pos, 2) if len(pos) > 2 else DEFAULT_REFUSAL_EXPLANATION
                if len(pos) > 3:
                    raise ScriptError("step 'stop refusal' takes at most a category and an explanation")
                cur.stop_details = {"type": "refusal", "category": category, "explanation": explanation}
            elif len(pos) > 1:
                # Only a refusal carries details: an extra argument elsewhere is a typo, never ignored.
                raise ScriptError(f"step 'stop {cur.stop}' takes no further argument (only 'stop refusal' has details)")
        elif name == "usage":
            mapping = {"in": "input_tokens", "out": "output_tokens", "cache_read": "cache_read_input_tokens",
                       "cache_write": "cache_creation_input_tokens"}
            for key, value in opts.items():
                if key not in mapping:
                    raise ScriptError(f"step 'usage': unknown field {key!r} (in, out, cache_read, cache_write)")
                try:
                    cur.usage[mapping[key]] = int(value)
                except ValueError:
                    raise ScriptError(f"step 'usage': {key} must be a number") from None
        elif name == "tool":
            if not pos or not isinstance(pos[0], str):
                raise ScriptError("step 'tool' needs a tool name: tool <Name> {json}")
            tool_input = pos[1] if len(pos) > 1 else {}
            if not isinstance(tool_input, dict):
                raise ScriptError(f"step 'tool' {pos[0]}: the input must be a JSON object")
            cur.elements.append(Element("tool_use", name=pos[0], input=tool_input))
            close("tool_use", False)
        elif name == "tools":
            calls = pos[0] if pos else None
            if not isinstance(calls, list) or not calls:
                raise ScriptError('step \'tools\' needs a JSON array: tools [["Bash", {"command": "ls"}], ...]')
            for call in calls:
                if isinstance(call, dict) and "name" in call:
                    tname, tinput = call["name"], call.get("input", {})
                elif isinstance(call, list) and call and isinstance(call[0], str):
                    tname, tinput = call[0], call[1] if len(call) > 1 else {}
                else:
                    raise ScriptError(f"step 'tools': not a call: {call!r}")
                if not isinstance(tinput, dict):
                    raise ScriptError(f"step 'tools' {tname}: the input must be a JSON object")
                cur.elements.append(Element("tool_use", name=tname, input=tinput))
            close("tool_use", False)
        elif name == "reply":
            cur.elements.append(Element("text", text=_body_text(name, pos, opts), chunk=chunk, chunk_ms=chunk_ms))
            close("end_turn", True)
            return result
        else:  # pragma: no cover - STEP_NAMES and this dispatch are kept in step by the unit tests
            raise ScriptError(f"step {name!r} is not handled")
    if cur.elements or cur.fail or cur.expect or pending_expect or pending_fail:
        start_if_new()
        close("end_turn", True)
    return result


def check_tool_input(name: str, value: dict, schema: Optional[dict]) -> list[str]:
    """Check a scripted tool input against the tool's input_schema from the request.

    Only the parts that catch script typos: required keys present, and the JSON type of each declared
    property (string, number, integer, boolean, array, object). A wrong script then fails loudly instead of
    confusing Claude Code (E2E_TESTING.md §4.3).

    Args:
        name: The tool's name (for the messages).
        value: The scripted input.
        schema: The tool's `input_schema` from the request (None when the request does not offer the tool).
    Returns:
        Problems found (empty = fine). A tool the request does not offer is a problem too.
    """
    if schema is None:
        return [f"the request offers no tool named {name!r}"]
    problems = []
    for key in schema.get("required") or []:
        if key not in value:
            problems.append(f"{name}: missing required {key!r}")
    types = {"string": str, "number": (int, float), "integer": int, "boolean": bool, "array": list, "object": dict}
    for key, spec in (schema.get("properties") or {}).items():
        if key not in value or not isinstance(spec, dict):
            continue
        expected = spec.get("type")
        if isinstance(expected, str) and expected in types:
            ok = isinstance(value[key], types[expected])
            if expected in ("number", "integer") and isinstance(value[key], bool):
                ok = False
            if not ok:
                problems.append(f"{name}: {key!r} must be {expected}, got {type(value[key]).__name__}")
    return problems
