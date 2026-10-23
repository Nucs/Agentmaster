"""A session's state through Claude Code's own events (doc/agentmaster/flows/05-session-state.md, STA).

Each test drives a real Claude Code 2.1.291 against the scripted model and watches the session's state the way the
user sees it: the side panel card's state word, sampled about every 150 ms through UI Automation
(amdriver/timeline.py). A flicker, a stuck state or a wrong intermediate state therefore fails with the whole sequence
in the message, together with the engine's own story (hooks.log hook lines and scanner.log reconciliations).

The events come from a survey of every Claude Code transcript on the owner's PC (7,044 files) and of the installed
release's scanner.log (flows/05 "Claude Code's events, measured"); the mock reproduces each: a refusal and its retry,
max_tokens and its continuation, a prompt too long and its compaction, retried and final API errors, a cut stream,
Esc mid-stream and mid-tool, a question, a permission prompt, type-ahead, a background task's notification turn, a
Stop hook that blocks, /compact, /exit, /model, and a Claude Code whose hooks never reach Agentmaster at all.

⚠ Timing on a loaded PC: our PowerShell hook forwarders can take longer than Claude Code's 10 s hook timeout (Claude
Code then records `hook_cancelled`, and the hook may arrive late or never). The scanner's belts then carry the state a
few seconds later. So the tests assert the ORDER of the states and the final state, with generous waits
(STATE_WAIT_S), and never a hook's latency.
"""
from __future__ import annotations

import re
import time

import psutil
import pytest

from amdriver import inputs
from amdriver.timeline import StateTimeline, engine_story, transcript_kinds
from amdriver.waits import wait_until

STATE_WAIT_S = 120.0
"""How long a state may take to show: a hook that times out (10 s, twice on a busy PC) plus the scanner's settle."""


def _marks(am) -> tuple[int, int]:
    """The ends of hooks.log and scanner.log now (read the engine's story from here).

    Args:
        am: The app.
    Returns:
        (hooks mark, scanner mark).
    """
    return am.profile.hooks.mark(), am.profile.scanner.mark()


def _why(am, s, marks, tl) -> str:
    """The evidence a failed state assertion carries: the sampled timeline and the engine's story.

    Args:
        am: The app.
        s: The Session.
        marks: _marks() taken before the action.
        tl: The StateTimeline.
    Returns:
        One line of text.
    """
    return f"timeline: {tl.describe()}; engine: {engine_story(am.profile, s.id, *marks)}"


def _kinds_since(am, s, start: int):
    """The session's transcript lines from an index on, reduced to their state-relevant kinds.

    Args:
        am: The app.
        s: The Session.
        start: The index of the first line to keep (len(transcript) before the action).
    Returns:
        transcript_kinds of those lines.
    """
    return transcript_kinds(am.claude.transcript(s.id)[start:])


def _wait_transcript(am, s, start: int, pred, what: str, timeout_s: float = STATE_WAIT_S):
    """Wait until a transcript line written after `start` satisfies a predicate.

    Args:
        am: The app.
        s: The Session.
        start: The transcript index the action started at.
        pred: A predicate over a TranscriptLine.
        what: What is awaited (for the timeout message).
        timeout_s: How long.
    Returns:
        The first matching TranscriptLine.
    Raises:
        WaitTimeout: When no such line was written.
    """
    return wait_until(lambda: next((t for t in _kinds_since(am, s, start) if pred(t)), None), timeout_s,
                      interval_s=0.5, what=what,
                      describe=lambda: [(t.type, t.sub, t.stop) for t in _kinds_since(am, s, start)])


def _ordered(states: list[str], expected: list[str]) -> bool:
    """Whether `expected` appears in `states` in order (other states may come between).

    Args:
        states: The sampled sequence.
        expected: The states that must appear, in this order.
    Returns:
        True when every expected state is found after the previous one.
    """
    i = 0
    for st in states:
        if i < len(expected) and st == expected[i]:
            i += 1
    return i == len(expected)


def _send(s, prompt: str) -> None:
    """Submit a prompt and wait until the mock received it (not for the turn).

    Args:
        s: The Session.
        prompt: The prompt (with its script).
    Returns:
        None.
    Raises:
        WaitTimeout: When the mock never got it.
    """
    s.ask(prompt, wait="sent", timeout_s=STATE_WAIT_S)


def _press(am, key: str) -> None:
    """Press a key in the selected tab's terminal (Enter on a dialog, Esc to interrupt).

    The window is brought forward and the terminal clicked first: real key events reach only the focused control, and
    a page or the side panel may hold the focus (Claude Code's inline renderer takes no mouse input, so the click
    only focuses).

    Args:
        am: The app.
        key: The key ("enter", "escape").
    Returns:
        None.
    """
    am.window.focus()
    am.window.terminal.focus()
    inputs.keys(key)


def _error_line(card) -> str:
    """A board card's error line ("⚠ <status>: <message>", AgentManagerContent.Board.cpp).

    Args:
        card: The board Card.
    Returns:
        The line, or "" when the card shows none.
    """
    return next((t for t in card.texts if t.startswith("⚠")), "")


# ---- STA-01 / STA-02 · a turn's states -------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("STA-01")
def test_turn_reads_running_then_waiting_with_no_blip(am, s1):
    """STA-01 phases 2–3 and STA-02: a held turn reads running for as long as it is held, then waiting-for-you — and
    nothing in between; the board and the tree agree at the end.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 20)
        _send(s1, 'go [[mock say "busy"; hold g; reply "done"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        # Held mid-response for 10 s: a quiet transcript under a running turn is no turn end (STA-02).
        tl.hold("running", 10)
        am.mock.release("g")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    # Every surface agrees (read through UI Automation: a poll of sessions.json could make the app's save fail —
    # ROB-K1 — and the file lags the registry by up to a second anyway).
    page = am.window.manager.open()
    page.wait_card(s1.title, column="Waiting-for-you", timeout_s=20)
    assert page.tree_row(s1.title).state == "waiting-for-you"


@pytest.mark.p1
@pytest.mark.flow("STA-01")
def test_turn_under_two_seconds_settles_to_waiting(am, s1):
    """STA-01 E2: a turn shorter than the 2 s floor (its Stop reads too fast) still ends waiting-for-you — the
    scanner settles it; nothing is left running.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'quick [[mock fast; reply "x"]]')
        # The engine logs a Stop for this turn (the hook's, or the scanner's settle when the hook's reads too fast).
        am.profile.hooks.wait(rf"\[Stop\] {s1.id} state=2", since=marks[0], timeout_s=STATE_WAIT_S)
        tl.wait("waiting-for-you", STATE_WAIT_S)
        time.sleep(3)  # a late, stale hook must not bring running back
    assert tl.current() == "waiting-for-you", _why(am, s1, marks, tl)


@pytest.mark.p1
@pytest.mark.flow("STA-01")
def test_type_ahead_is_absorbed_into_the_running_turn(am, s1):
    """STA-01 E3: a prompt typed while the turn's tool runs is absorbed into the SAME turn (Claude Code: queue
    enqueue → remove `absorbed_mid_turn` → a `queued_command` attachment); the session reads running throughout and
    waiting-for-you once, at the end.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'first [[mock bash "sleep 20"; reply "first done"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.stop == "tool_use", "the Bash call")
        tl.wait("running", STATE_WAIT_S)
        am.window.terminal.focus()
        am.window.terminal.type('second [[mock reply "second done"]]')
        inputs.keys("enter")
        last = _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.stop == "end_turn",
                                "the turn's final reply")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "queue-operation" and t.sub == "enqueue" for t in kinds)
    assert any(t.type == "attachment" and t.sub == "queued_command" for t in kinds), [(t.type, t.sub) for t in kinds]
    # The absorbed message is the newest scripted user message, so it answers the turn (mock: E2E_TESTING.md §4.3).
    assert last.text == "second done"


# ---- STA-03 · needs approval ---------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("STA-03")
def test_question_reads_needs_approval_until_answered(am, s1):
    """STA-03 phases 1–2: an AskUserQuestion reads needs-approval (claude's `waiting` heartbeat or its Notification)
    on the panel and the board; Enter answers it and the session ends waiting-for-you.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'ask [[mock ask "Pick a side?" Left,Right; reply "ok"]]')
        tl.wait("needs-approval", STATE_WAIT_S)
        page = am.window.manager.open()
        page.wait_card(s1.title, column="Needs-approval", timeout_s=20)
        s1.select()
        am.window.terminal.wait_text(r"Pick a side\?", timeout_s=30)
        _press(am, "enter")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "needs-approval", "waiting-for-you"]), \
        _why(am, s1, marks, tl)
    assert tl.states()[-1] == "waiting-for-you"
    answered = [t for t in _kinds_since(am, s1, start) if t.type == "user"
                and "Left" in str((t.raw.get("toolUseResult") or {}).get("answers", ""))]
    assert answered, "the question's answer (Left) is not in the transcript"


@pytest.mark.p1
@pytest.mark.flow("STA-03")
@pytest.mark.am_settings(skipPermissions=False)
def test_permission_prompt_reads_needs_approval(am, iso, s1):
    """STA-03 E3: with skip-permissions off, a Write asks for permission: needs-approval until Enter approves it;
    the file is written and the session ends waiting-for-you.

    Args:
        am: The app (started with skipPermissions off).
        iso: The test's folders.
        s1: F-S.
    """
    marks = _marks(am)
    target = iso.folder("t1") / "perm.txt"
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, f'perm [[mock write {str(target).replace(chr(92), "/")} "x"; reply "written"]]')
        tl.wait("needs-approval", STATE_WAIT_S)
        assert not target.exists(), "the file was written before the permission was given"
        _press(am, "enter")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["running", "needs-approval", "waiting-for-you"]), _why(am, s1, marks, tl)
    assert target.read_text(encoding="utf-8") == "x"


# ---- STA-04 · API errors -------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("STA-04")
def test_final_api_error_reads_error_with_its_message_and_recovers(am, s1):
    """STA-04 phases 1–2: a final API error (a 400 is not retried) reads error, the board card says why, the runner
    pauses (stop-on-error); the next message reads running, then waiting-for-you.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    runner = am.profile.autorunner.mark()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'filter [[mock error 400 invalid_request_error "Output blocked by content filtering policy"]]')
        tl.wait("error", STATE_WAIT_S)
        am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: 400 Output blocked", since=marks[1],
                                timeout_s=30)
        am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
        page = am.window.manager.open()
        card = page.wait_card(s1.title, column="Error", timeout_s=30)
        line = _error_line(card)
        assert "Output blocked by content filtering policy" in line, card.texts
        s1.select()
        _send(s1, 'again [[mock reply "ok"]]')
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error", "running", "waiting-for-you"]), \
        _why(am, s1, marks, tl)


@pytest.mark.p0
@pytest.mark.flow("STA-04")
def test_retried_overload_never_reads_error(am, s1):
    """STA-04 E1: two 529 overloaded answers that Claude Code retries, then a good one: running → waiting-for-you,
    never error.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'retry [[mock error 529 overloaded_error "Overloaded" times=2; reply "recovered"]]')
        tl.wait_seen("running", STATE_WAIT_S)
        am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("status") == 200, since=seq,
                         timeout_s=STATE_WAIT_S)
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    statuses = [e.get("status") for e in am.mock.requests(since=seq, kind="main")]
    assert statuses[:3] == [529, 529, 200], statuses


@pytest.mark.p0
@pytest.mark.flow("STA-04")
def test_stream_cut_mid_response_reads_error(am, s1):
    """STA-04 E2: the connection cut mid-response: Claude Code keeps the partial text and writes "Connection lost
    mid-response" as an API error; the session reads error with that message.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'cut [[mock say "half of the answer"; drop]]')
        tl.wait("error", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: Connection lost mid-response", since=marks[1],
                            timeout_s=30)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "assistant" and t.text == "half of the answer" for t in kinds)
    assert any(t.api_error and "Connection lost mid-response" in t.text for t in kinds)


@pytest.mark.p1
@pytest.mark.flow("STA-04")
def test_model_not_found_reads_error(am, s1):
    """STA-04 E4: the model answers 404 not_found: Claude Code says the selected model has an issue; the session reads
    error (not done) and its runner pauses.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    runner = am.profile.autorunner.mark()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'model [[mock error 404 not_found_error "model: claude-nope-9"]]')
        tl.wait("error", STATE_WAIT_S)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(There's an issue with the selected model", since=marks[1],
                            timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
    assert "done" not in tl.states(), _why(am, s1, marks, tl)
    page = am.window.manager.open()
    card = page.wait_card(s1.title, column="Error", timeout_s=30)
    assert "There's an issue with the selected model" in _error_line(card), card.texts


@pytest.mark.p1
@pytest.mark.flow("STA-04")
def test_repeated_overload_reads_error(am, s1):
    """STA-04 E6: 529 overloaded on every attempt: Claude Code gives up after three ("API Error: Repeated 529
    Overloaded errors …"); the session reads running while it tries, then error with that message, and the runner
    pauses.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    runner = am.profile.autorunner.mark()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'busy [[mock error 529 overloaded_error "Overloaded"]]')
        tl.wait("error", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: Repeated 529 Overloaded errors", since=marks[1],
                            timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
    attempts = [e.get("status") for e in am.mock.requests(since=seq, kind="main")]
    assert attempts == [529, 529, 529], attempts


@pytest.mark.p1
@pytest.mark.flow("STA-04")
def test_server_error_mid_stream_reads_error(am, s1):
    """STA-04 E2 (a server error, not a cut): the stream fails with an error event after some text — Claude Code keeps
    the partial text and ends the turn at once with "API Error: Server error mid-response …" (no retry); error.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'mid [[mock say "half of it"; hold g; reply "never"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        am.mock.fail("g", 529, "overloaded_error", "Overloaded")
        tl.wait("error", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: Server error mid-response", since=marks[1],
                            timeout_s=30)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "assistant" and t.text == "half of it" for t in kinds)


@pytest.mark.p1
@pytest.mark.flow("STA-04")
def test_a_retrying_request_reads_running_until_esc(am, s1):
    """STA-04 E6: a 429 that Claude Code keeps retrying ("Retrying in Ns · attempt n/15", with backoff — minutes, and
    CLAUDE_CODE_MAX_RETRIES does not shorten the interactive loop's 15): the session reads running all along, never
    error; Esc ends the turn and it reads waiting-for-you.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'rate [[mock error 429 rate_limit_error "Number of request tokens has exceeded your per-minute rate limit"]]')
        wait_until(lambda: len([e for e in am.mock.requests(since=seq, kind="main") if e.get("status") == 429]) >= 3,
                   STATE_WAIT_S, what="three attempts of the request")
        am.window.terminal.wait_text(r"Retrying in \d+s · attempt \d+/15", timeout_s=30)
        tl.wait("running", 30)
        tl.hold("running", 5)
        _press(am, "escape")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)


# ---- STA-12 · interrupts -------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("STA-12")
def test_esc_mid_stream_reads_waiting_and_holds_the_runner(am, s1):
    """STA-12 phase 1: Esc while the answer streams: Claude Code writes "[Request interrupted by user]" (no Stop
    hook); the scanner settles the session to waiting-for-you, and the runner takes its interrupt hold.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'talk [[mock say "thinking out loud"; hold g; reply "never"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        _press(am, "escape")
        tl.wait("waiting-for-you", STATE_WAIT_S)
        am.mock.release("g")
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    _wait_transcript(am, s1, start, lambda t: t.type == "user" and t.text.startswith("[Request interrupted by user]"),
                     "the interrupt marker", timeout_s=30)
    am.profile.hooks.wait(rf"\[interrupt-hold\] {s1.short}", since=marks[0], timeout_s=30)


@pytest.mark.p1
@pytest.mark.flow("STA-12")
def test_esc_during_a_tool_reads_waiting(am, s1):
    """STA-12 phase 2: Esc while a Bash call runs: "[Request interrupted by user for tool use]"; waiting-for-you.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'sleep [[mock bash "sleep 60"; reply "slept"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.stop == "tool_use", "the Bash call")
        tl.wait("running", STATE_WAIT_S)
        time.sleep(2)  # the tool is running (Git Bash started)
        _press(am, "escape")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    _wait_transcript(am, s1, start, lambda t: t.type == "user"
                     and t.text.startswith("[Request interrupted by user for tool use]"), "the interrupt marker",
                     timeout_s=30)


# ---- STA-14 · Claude Code's own recoveries -------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("STA-14")
def test_max_tokens_continuation_stays_running(am, s1):
    """STA-14 phase 1: an answer cut at max_tokens: Claude Code continues by itself in the same turn ("Output token
    limit hit. Resume directly …"); the session reads running through the continuation, then waiting-for-you.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'long [[mock say "partial answer cut here"; stop max_tokens; next; hold g; reply "the rest"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        tl.hold("running", 10)  # the continuation is held: the max_tokens line is no turn end
        am.mock.release("g")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "assistant" and t.stop == "max_tokens" for t in kinds)
    assert any(t.type == "user" and t.text.startswith("Output token limit hit") for t in kinds)
    assert any(t.type == "assistant" and t.stop == "end_turn" and t.text == "the rest" for t in kinds)


@pytest.mark.p0
@pytest.mark.flow("STA-14")
def test_refusal_retried_once_then_answered_never_reads_error(am, s1):
    """STA-14 phase 2: a refusal (stop_reason refusal): Claude Code notes "safeguards stopped the response above ·
    continuing once with that noted" and retries; the retry answers — running → waiting-for-you, never error.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'refuse [[mock say "I cannot help with that."; stop refusal; next; reply "answered after the retry"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "answered after the retry",
                         "the retry's answer")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "assistant" and t.stop == "refusal" for t in kinds)
    assert any(t.type == "system" and t.sub == "informational" and "continuing once" in str(t.raw.get("content"))
               for t in kinds)


@pytest.mark.p0
@pytest.mark.flow("STA-14")
def test_refusal_twice_reads_error(am, s1):
    """STA-14 phase 3: refused again on the retry (and on the fallback model Claude Code tries): Claude Code ends with
    `model_refusal_no_fallback` and an API error ("…'s safeguards flagged this message …"); the session reads error
    and its runner pauses.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    runner = am.profile.autorunner.mark()
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'refuse2 [[mock say "I cannot help with that."; stop refusal; next; say "Still no."; stop refusal]]')
        tl.wait("error", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: .*safeguards flagged this message", since=marks[1],
                            timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "system" and t.sub == "model_refusal_no_fallback" for t in kinds)
    assert any(t.api_error and t.stop == "refusal" for t in kinds)


@pytest.mark.p0
@pytest.mark.flow("STA-14")
def test_prompt_too_long_compacts_and_continues(am, s1):
    """STA-14 phase 4 (STA-06 E4): a 400 "prompt is too long": Claude Code compacts the conversation by itself
    (PreCompact … PostCompact, a compact_boundary) and sends the turn again; the session reads running throughout,
    then waiting-for-you — never error, never a waiting blip during the compaction.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'big [[mock error 400 invalid_request_error "prompt is too long: 250001 tokens > 200000 maximum" '
                  'times=1; reply "after the compaction"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "after the compaction",
                         "the answer after the compaction")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    assert any(t.type == "system" and t.sub == "compact_boundary" for t in _kinds_since(am, s1, start))
    kinds = [e.get("kind") for e in am.mock.requests(since=seq) if e.get("kind") in ("main", "compact")]
    assert kinds[:3] == ["main", "compact", "main"], kinds
    story = engine_story(am.profile, s1.id, *marks)
    assert "PreCompact" in story and "PostCompact" in story, story


@pytest.mark.p0
@pytest.mark.flow("STA-14")
def test_prompt_too_long_that_compaction_cannot_fix_reads_error(am, s1):
    """STA-14 E2 — the most common Error on the owner's PC (91 of 231): a prompt still too long after Claude Code's own
    compaction. Claude Code compacts, sends again, gets the 400 again and ends with "Prompt is too long"; the session
    reads running through the compaction, then error, and the runner pauses.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    start = len(am.claude.transcript(s1.id))
    runner = am.profile.autorunner.mark()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'big [[mock error 400 invalid_request_error "prompt is too long: 250001 tokens > 200000 maximum"]]')
        tl.wait("error", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(Prompt is too long\)", since=marks[1], timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
    assert any(t.type == "system" and t.sub == "compact_boundary" for t in _kinds_since(am, s1, start))
    kinds = [e.get("kind") for e in am.mock.requests(since=seq) if e.get("kind") in ("main", "compact")]
    assert kinds[:3] == ["main", "compact", "main"], kinds


# ---- STA-13 · turns nobody typed -------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("STA-13")
def test_background_bash_completion_starts_a_turn_nobody_typed(am, s1):
    """STA-13 phase 1: a background Bash outlives its turn; when it finishes, Claude Code delivers its
    <task-notification> as a new turn (a UserPromptSubmit nobody typed): waiting-for-you → running → waiting-for-you
    again, and the notification is not recorded as a typed prompt.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'bg [[mock tool Bash {"command": "sleep 20", "run_in_background": true, "description": "bg sleep"}; '
                  'reply "started"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "user" and t.text.startswith("<task-notification>"),
                         "the background task's notification turn")
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "mock: script done",
                         "the notification turn's answer")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you", "running", "waiting-for-you"], \
        _why(am, s1, marks, tl)
    am.profile.hooks.wait(rf"\[ups\] {s1.short} chars=\d+ hash=\w+ -> noise \(not recorded\)", since=marks[0],
                          timeout_s=30)


@pytest.mark.p1
@pytest.mark.flow("STA-13")
@pytest.mark.known_defect("STA-K4")
def test_known_defect_a_blocking_stop_hook_reads_waiting_while_claude_continues(am, iso):
    """STA-K4 (STA-13 phase 2): a user Stop hook blocks the first stop ({"decision": "block"}), so Claude Code goes on
    with the same turn ("Stop hook feedback: keep going"). TODAY our own Stop hook has already said "turn complete":
    the session reads waiting-for-you while Claude Code works on the continuation, until the busy heartbeat outlives
    the turn by 20 s (`[recon-subagent] … outlived turn-end`). Expected once fixed: running throughout.

    Args:
        am: The app.
        iso: The test's folders.
    """
    arm = iso.work_dir / "stopblock.arm"
    script = iso.work_dir / "stophook.ps1"
    script.write_text(
        "$arm = '" + str(arm) + "'\n"
        "if (Test-Path $arm) { Remove-Item $arm; Write-Output '{\"decision\": \"block\", \"reason\": \"keep going\"}' }\n"
        "exit 0\n", encoding="utf-8")
    am.claude.seed(settings={"hooks": {"Stop": [{"hooks": [{"type": "command", "command":
        "powershell -NoProfile -ExecutionPolicy Bypass -File " + str(script).replace("\\", "/")}]}]}})
    s = am.launch("t1")
    marks = _marks(am)
    start = len(am.claude.transcript(s.id))
    with StateTimeline(am.window, s.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        arm.write_text("1", encoding="utf-8")
        _send(s, 'go [[mock say "first stop"; next; hold g; reply "continued"]]')
        _wait_transcript(am, s, start, lambda t: t.type == "user" and t.text.startswith("Stop hook feedback"),
                         "the blocked stop's feedback line")
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        # TODAY: waiting-for-you while Claude Code is answering the continuation (the defect).
        tl.wait("waiting-for-you", 60)
        tl.hold("waiting-for-you", 8)
        am.mock.release("g")
        _wait_transcript(am, s, start, lambda t: t.type == "assistant" and t.text == "continued", "the continuation")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "waiting-for-you"]), _why(am, s, marks, tl)
    assert any(t.type == "attachment" and t.sub == "hook_blocking_error" for t in _kinds_since(am, s, start))


@pytest.mark.p1
@pytest.mark.flow("STA-05")
def test_background_agent_that_keeps_working_relights_running(am, s1):
    """STA-05 phase 3: a background agent (2.1.291 runs every Agent call in the background) that keeps writing for
    ~35 s: the parent's turn ends waiting-for-you, the session re-lights running once the agent's work has outlived
    the turn by 20 s ([recon-subagent] … outlived turn-end), and its completion notification turn ends
    waiting-for-you.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    sub = '[[mock bash \\"sleep 8\\"; bash \\"sleep 8\\"; bash \\"sleep 8\\"; bash \\"sleep 8\\"; reply \\"sub done\\"]]'
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'bga [[mock tool Agent {"description": "bg agent", "prompt": "' + sub + '", '
                  '"subagent_type": "general-purpose", "run_in_background": true}; reply "started"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "user" and t.text.startswith("<task-notification>"),
                         "the agent's completion notification", timeout_s=180)
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "mock: script done",
                         "the notification turn's answer")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you", "running", "waiting-for-you"], \
        _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-subagent\] {s1.id} \(presence=busy, outlived turn-end", since=marks[1],
                            timeout_s=10)


# ---- STA-06 · compaction ----------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("STA-06")
@pytest.mark.claude_env(CLAUDE_CODE_AUTO_COMPACT_WINDOW="100000")
def test_auto_compaction_mid_turn_keeps_running(am, s1):
    """STA-06 phase 2: a response whose usage nears the (100K) window: Claude Code compacts before the turn goes on
    (PreCompact, PostCompact auto); the session reads running throughout, then waiting-for-you.

    Args:
        am: The app (every claude with a 100K auto-compact window).
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'fill [[mock usage in=99000; bash "echo x"; reply "after"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "system" and t.sub == "compact_boundary",
                         "the auto-compaction's boundary")
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.stop == "end_turn", "the turn's end")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    boundary = next(t for t in _kinds_since(am, s1, start) if t.sub == "compact_boundary")
    assert (boundary.raw.get("compactMetadata") or {}).get("trigger") == "auto", boundary.raw.get("compactMetadata")


@pytest.mark.p2
@pytest.mark.flow("STA-06")
@pytest.mark.known_defect("STA-K3")
def test_known_defect_compact_reads_running_half_a_minute_after_it_ended(am, s1):
    """STA-K3 (STA-06 phase 1): `/compact` reads running while it compacts (correct) and its PostCompact sets
    waiting-for-you — but TODAY the scanner sets running again 2 ms later (claude's heartbeat still reads busy:
    `[recon-subagent] … (presence=busy, Waiting -> Running)`), and the session reads running until the presence-idle
    release ~30 s later. Expected once fixed: waiting-for-you at the PostCompact.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        am.window.terminal.submit("/compact")
        tl.wait("running", STATE_WAIT_S)
        _wait_transcript(am, s1, start, lambda t: t.type == "system" and t.sub == "compact_boundary",
                         "the compaction's boundary")
        am.profile.hooks.wait(rf"\[PostCompact\] {s1.id} state=2", since=marks[0], timeout_s=STATE_WAIT_S)
        # TODAY: the busy heartbeat's promotion right after the PostCompact (the defect) …
        am.profile.scanner.wait(rf"\[recon-subagent\] {s1.id} \(presence=busy, Waiting -> Running\)", since=marks[1],
                                timeout_s=10)
        # … keeps the panel on running 10 s after the compaction ended …
        tl.hold("running", 10)
        # … until the presence-idle release.
        tl.wait("waiting-for-you", 60)
    am.profile.scanner.wait(rf"\[recon-stop-idle\] {s1.id}", since=marks[1], timeout_s=10)


# ---- STA-01 phase 5 / STA-16 · the end of a claude, and hooks that never arrive -------------------------------


@pytest.mark.p1
@pytest.mark.flow("STA-01")
def test_exit_reads_done(am, s1):
    """STA-01 phase 5: `/exit` ends claude (its SessionEnd hook): the session reads done, and the tab stays with a
    shell prompt and claude's resume line. (On a heavily loaded PC the SessionEnd hook can miss its 5 s budget and
    the session then stays waiting-for-you — STA-K1.)

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        am.window.terminal.submit("/exit")
        tl.wait("done", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "done"], _why(am, s1, marks, tl)
    am.window.terminal.wait_text(r"claude --resume " + s1.id, timeout_s=30)
    wait_until(lambda: not s1.claude_pids(), 30, what="claude to exit")


@pytest.mark.p1
@pytest.mark.flow("STA-16")
def test_state_follows_with_every_hook_disabled(am):
    """STA-16 phase 1: a Claude Code whose hooks never run (its settings say disableAllHooks): the scanner alone
    carries the state — running within a few seconds of the prompt, waiting-for-you after the turn.

    Args:
        am: The app.
    """
    am.claude.seed(settings={"disableAllHooks": True})
    s = am.launch("t1")
    marks = _marks(am)
    with StateTimeline(am.window, s.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s, 'quiet [[mock say "working"; hold g; reply "done"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", 30)
        tl.hold("running", 10)
        am.mock.release("g")
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s, marks, tl)
    story = engine_story(am.profile, s.id, *marks)
    assert "scanner:recon-run" in story and "scanner:recon-stop" in story, story
    # The proof that no hook ran: a managed claude's SessionStart hook is its first line in hooks.log.
    assert not re.search(rf"\[SessionStart\] {s.id} ", am.profile.hooks.text()), "a hook reached Agentmaster"


@pytest.mark.p1
@pytest.mark.flow("STA-11")
@pytest.mark.known_defect("STA-K1")
def test_known_defect_a_killed_claude_keeps_reading_running(am, s1):
    """STA-K1 (STA-11 E3): the session's claude.exe is killed mid-turn (no SessionEnd, no Stop). TODAY the session
    reads running for good: its presence fact keeps the dead process's last "busy", which the scanner trusts (the
    "work outlived the turn" rule), while the tab shows the console guard's "[Agentmaster] The agent exited …" note.
    Expected once fixed: not running — error ("the agent exited") or done — within a scan or two of the exit.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'busy [[mock say "working"; hold g; reply "never"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        for pid in s1.claude_pids():
            try:
                psutil.Process(pid).kill()
            except psutil.NoSuchProcess:
                pass
        wait_until(lambda: not s1.claude_pids(), 30, what="the claude to be gone")
        am.window.terminal.wait_text(r"\[Agentmaster\] The agent exited", timeout_s=60)
        # TODAY: still running 30 s after the claude is gone (the defect). After the fix this hold fails.
        tl.hold("running", 30)
        am.mock.release("g")
    assert tl.current() == "running", _why(am, s1, marks, tl)


@pytest.mark.p2
@pytest.mark.flow("STA-12")
@pytest.mark.known_defect("STA-K2")
def test_known_defect_esc_before_any_output_reads_running_for_half_a_minute(am, s1):
    """STA-K2 (STA-12 E1): Esc before the model sent anything: Claude Code cancels the turn, puts the prompt back in
    the input box and writes NOTHING to the transcript (no interrupt marker, no Stop). TODAY the session keeps reading
    running until the presence-idle release (30 s of quiet), then waiting-for-you. Expected once fixed:
    waiting-for-you within a few seconds (claude's own heartbeat reads idle at once).

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'wait [[mock hold g; reply "never"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        _press(am, "escape")
        # Claude Code restores the prompt into the box (its "Esc before output" behavior).
        wait_until(lambda: (lambda box: box is not None and box.draft.startswith("wait"))(am.window.terminal.input_box()),
                   30, what="the prompt back in the input box")
        # TODAY: still running 10 s after the Esc (the defect) …
        tl.hold("running", 10)
        # … and released by the presence-idle rule within its 30 s.
        tl.wait("waiting-for-you", 60)
        am.mock.release("g")
    am.profile.scanner.wait(rf"\[recon-stop-idle\] {s1.id}", since=marks[1], timeout_s=10)


# ---- STA-15 · Claude Code's local commands -------------------------------------------------------------------


@pytest.mark.p2
@pytest.mark.flow("STA-15")
def test_model_switch_dialog_changes_no_state(am, s1):
    """STA-15 phase 1: `/model sonnet` opens Claude Code's "Switch model?" dialog (no hook, no turn); cancelling it
    with Esc leaves the session waiting-for-you the whole time.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 20)
        am.window.terminal.submit("/model sonnet")
        am.window.terminal.wait_text(r"Switch model\?", timeout_s=60)
        tl.hold("waiting-for-you", 8)
        _press(am, "escape")
        am.window.terminal.wait_input_box(timeout_s=60)
        tl.hold("waiting-for-you", 5)
    assert tl.states() == ["waiting-for-you"], _why(am, s1, marks, tl)
