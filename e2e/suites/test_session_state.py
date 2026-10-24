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
from datetime import datetime

import psutil
import pytest

from amdriver import inputs
from amdriver.timeline import StateTimeline, engine_story, transcript_kinds
from amdriver.waits import wait_until

STATE_WAIT_S = 120.0
"""How long a state may take to show: a hook that times out (10 s, twice on a busy PC) plus the scanner's settle."""

ACCEPTED_LAG_S = 60.0
"""The longest a status may LAG a change Claude Code made without telling us (flows/05 *Reclassified: accepted delays*):
the user's ruling of 2026-10-24 — "no reason to worry for 30s-ish status update delay. We sometimes use that delay for
safety of pilot autorunner to not spam a mistake sent during status change". Measured on 0.8.0: 20–30 s (the
presence-idle release's 30 s of quiet; the busy heartbeat outliving the transcript by 20 s); the bound doubles it for a
loaded PC. A lag over this bound — or a status that never settles — is a defect."""


BLIP_S = 1.5
"""A state the card shows for less than this is a hook-timing BLIP, not a state the session took: a hook that arrives
late is applied for one scan and corrected by the next (measured 2026-10-24 on the busy test desktop, where every hook
took 8–20 s: a restarted claude's SessionStart(resume) landed after the scanner had already promoted the continuing turn
to Running, and the record read idle for 52–115 ms — one or two of the timeline's 150 ms samples). Tests that cross a
restart or a resume allow it (`_longest_stretch`); a blip that holds longer is a wrong state."""


def _longest_stretch(tl, state: str, after_t: float = -1.0) -> float:
    """The longest time the card kept showing a state, counting only stretches that began after a timeline time.

    Args:
        tl: The StateTimeline.
        state: The word ("idle" …).
        after_t: Only stretches entered after this timeline time count (-1 = all).
    Returns:
        The longest stretch in seconds (0.0 when the state was never entered after `after_t`); a stretch still showing
        now counts up to now.
    """
    ch = tl.changes()
    longest = 0.0
    for i, c in enumerate(ch):
        if c.t <= after_t or c.state != state:
            continue
        end = ch[i + 1].t if i + 1 < len(ch) else tl.elapsed()
        longest = max(longest, end - c.t)
    return longest


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
@pytest.mark.am_settings(waitingForYouTimeoutMinutes=1)
def test_a_read_session_decays_to_idle_after_the_waiting_timeout(am, s1):
    """STA-01 E1 (read): the Waiting-for-you timeout set to 1 minute (the cog's shortest; the default is 3 days). The
    session's turn ended while its tab was the focused one, so it is READ: one minute later it decays to idle
    (`[decay-waiting] <id> read + waited 1m (>= 1m) -> Idle`), on the panel and the board.

    Args:
        am: The app (started with a 1-minute timeout).
        s1: F-S (its first turn ended on its own, selected tab).
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)
        tl.wait("idle", 150)
    am.profile.scanner.wait(rf"\[decay-waiting\] {s1.id} read \+ waited 1m \(>= 1m\) -> Idle", since=marks[1],
                            timeout_s=30)
    assert tl.states() == ["waiting-for-you", "idle"], _why(am, s1, marks, tl)
    page = am.window.manager.open()
    page.wait_card(s1.title, column="Idle / Done", timeout_s=30)


@pytest.mark.p2
@pytest.mark.flow("STA-01")
@pytest.mark.am_settings(waitingForYouTimeoutMinutes=1)
def test_an_unread_session_keeps_waiting_past_the_timeout_until_read(am, s2):
    """STA-01 E1 (unread): with the 1-minute timeout, t1's turn ends while t2 is the selected tab — UNREAD (its tab's
    flash ring). It keeps reading waiting-for-you well past the timeout (100 s after the turn; Claude Code's own idle
    notification ~60 s after the turn changes nothing); once t1 is selected (read) it decays to idle within the
    timeout (counted from its last activity, which that idle notification refreshed).

    Args:
        am: The app (started with a 1-minute timeout).
        s2: F-S2 (t1, t2).
    """
    a, b = s2
    marks = _marks(am)
    with StateTimeline(am.window, a.title) as tl:
        # At rest before the action: t1's first turn was read, so with the 1-minute timeout it may already be idle.
        wait_until(lambda: tl.current() in ("waiting-for-you", "idle"), 60, what="t1 at rest on the panel",
                   describe=tl.describe)
        _send(a, 'unread [[mock delay 6s; reply "done"]]')
        b.select()  # t1's turn ends while t2 is selected: unread
        running_t = tl.wait("running", STATE_WAIT_S)
        ended_t = tl.entered_after("waiting-for-you", running_t, STATE_WAIT_S)
        while tl.elapsed() < ended_t + 100:
            time.sleep(0.5)
        later = [c.state for c in tl.changes() if c.t > running_t]
        assert tl.current() == "waiting-for-you" and "idle" not in later, \
            f"an unread session decayed: {_why(am, a, marks, tl)}"
        a.select()  # read
        tl.wait("idle", 150)
    am.profile.scanner.wait(rf"\[decay-waiting\] {a.id} read \+ waited 1m \(>= 1m\) -> Idle", since=marks[1],
                            timeout_s=30)
    assert [c.state for c in tl.changes() if c.t >= running_t] == ["running", "waiting-for-you", "idle"], \
        _why(am, a, marks, tl)


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


RATE_LIMIT_MESSAGE = "Number of request tokens has exceeded your per-minute rate limit"
"""The API's own per-minute rate-limit message (the owner's transcripts hold it; the mock sends it as written)."""


@pytest.mark.p1
@pytest.mark.flow("STA-04")
@pytest.mark.parametrize("label,status,etype,message,options,logged", [
    ("429-marked-final", 429, "rate_limit_error", RATE_LIMIT_MESSAGE, "retry=no",
     r"API Error: Request rejected \(429\) · " + RATE_LIMIT_MESSAGE),
    ("429-long-retry-after", 429, "rate_limit_error", RATE_LIMIT_MESSAGE, "retry_after=120",
     r"API Error: Request rejected \(429\) · " + RATE_LIMIT_MESSAGE),
    ("500-marked-final", 500, "api_error", "Internal server error", "retry=no",
     r"API Error: 500 Internal server error\. This is a server-side issue, usually temporary"),
], ids=["429-marked-final", "429-long-retry-after", "500-marked-final"])  # short node ids: the label alone
def test_an_api_error_claude_code_will_not_retry_reads_error_at_once(am, s1, label, status, etype, message, options,
                                                                     logged):
    """STA-04 E8: a 429 or a 500 that Claude Code does not retry — the API marked it final (`x-should-retry: false`), or
    asked for a wait over Claude Code's 60 s ceiling (`retry-after: 120`: 2.1.291 gives up instead of waiting) — ends
    the turn after ONE attempt with Claude Code's own wording of the error: running → error, the message on the record's
    scanner line, the runner paused. (Without those headers both are retried up to 15 times:
    test_a_retrying_request_reads_running_until_esc. A 529 is retried three times even when marked final — measured.)

    Args:
        am: The app.
        s1: F-S.
        label: The case (its prompt's marker).
        status: The HTTP status.
        etype: The API error type.
        message: The API's message.
        options: The `error` step's header options.
        logged: A regex of what Claude Code wrote (the `[recon-error]` line's text).
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    runner = am.profile.autorunner.mark()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, f'{label} [[mock error {status} {etype} "{message}" {options}]]')
        tl.wait("error", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \({logged}", since=marks[1], timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
    attempts = [e.get("status") for e in am.mock.requests(since=seq, kind="main")]
    assert attempts == [status], f"Claude Code retried an error it should not have: {attempts}"


@pytest.mark.p1
@pytest.mark.flow("STA-04")
def test_a_rate_limit_retry_after_is_waited_out_running(am, s1):
    """STA-04 E1 (a 429 with a short retry-after): Claude Code waits the server's `retry-after` (4 s) and tries again;
    the session reads running through the wait — never error — and waiting-for-you after the retry's answer.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'wait it out [[mock error 429 rate_limit_error "slow down" times=1 retry_after=4; reply "after the wait"]]')
        am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("status") == 200, since=seq,
                         timeout_s=STATE_WAIT_S)
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)
    tries = [e for e in am.mock.requests(since=seq, kind="main")]
    assert [e.get("status") for e in tries[:2]] == [429, 200], [e.get("status") for e in tries]
    assert tries[1]["t"] - tries[0]["t"] >= 3.9, f"the retry did not wait the retry-after: {tries[1]['t'] - tries[0]['t']:.1f} s"


@pytest.mark.p1
@pytest.mark.flow("STA-04")
@pytest.mark.claude_env(_CLAUDE_CODE_ASSUME_FIRST_PARTY_BASE_URL="1", CLAUDE_BYTE_STREAM_IDLE_TIMEOUT_MS="20000")
def test_a_stream_that_goes_silent_reads_running_then_error(am, s1):
    """STA-04 E9: the stream stops sending after some text — no byte, no ping (the mock's `stall`). Claude Code's
    byte-level idle watchdog gives up after 20 s of silence: it keeps the partial text and ends the turn with "API Error:
    The response stopped arriving. The response above may be incomplete." The session reads running through the silence
    (claude's heartbeat stays busy: a silent stream is no turn end), then error, and the runner pauses.

    The watchdog is armed only for Anthropic's own API (180 s there): `_CLAUDE_CODE_ASSUME_FIRST_PARTY_BASE_URL=1` — a
    private switch of Claude Code 2.1.291 — makes the mock count as that, and `CLAUDE_BYTE_STREAM_IDLE_TIMEOUT_MS` sets 20
    s (its floor is 10 s; measured with 15 s: the error 15 s after the last byte, the connection closed). Without them the event-level watchdog waits at least 300 s. If a later Claude Code drops the
    switch, this test fails at its error wait: re-measure (flows/05 "Claude Code's events, measured").

    Args:
        am: The app (every claude with the switch and a 20 s byte watchdog).
        s1: F-S.
    """
    marks = _marks(am)
    seq = am.mock.last_seq()
    runner = am.profile.autorunner.mark()
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'silent [[mock say "half of it"; stall g; reply "never"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        tl.hold("running", 5)  # silence on an open stream is no turn end
        tl.wait("error", STATE_WAIT_S)
        am.mock.release("g")
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error"]), _why(am, s1, marks, tl)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: The response stopped arriving", since=marks[1],
                            timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
    kinds = _kinds_since(am, s1, start)
    assert any(t.type == "assistant" and t.text == "half of it" for t in kinds), [(t.type, t.text) for t in kinds]
    gone = [e for e in am.mock.requests(since=seq, kind="main") if e.get("client_gone")]
    assert gone, "Claude Code never gave up on the silent stream (its watchdog is not armed?)"


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


@pytest.mark.p1
@pytest.mark.flow("STA-12")
def test_esc_on_a_question_reads_waiting_and_holds_the_runner(am, s1):
    """STA-12 E3 (a question): Esc while an AskUserQuestion is open: Claude Code closes it and writes "[Request
    interrupted by user for tool use]"; needs-approval → waiting-for-you (`[recon-stop] … interrupted`), and the
    runner takes its interrupt hold until the next message.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'ask then esc [[mock ask "Pick a side?" Left,Right; reply "picked"]]')
        tl.wait("needs-approval", STATE_WAIT_S)
        am.window.terminal.wait_text(r"Pick a side\?", timeout_s=60)
        _press(am, "escape")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "needs-approval", "waiting-for-you"]), \
        _why(am, s1, marks, tl)
    _wait_transcript(am, s1, start, lambda t: t.type == "user"
                     and t.text.startswith("[Request interrupted by user for tool use]"), "the interrupt marker",
                     timeout_s=30)
    am.profile.hooks.wait(rf"\[interrupt-hold\] {s1.short}", since=marks[0], timeout_s=30)
    assert not any(t.type == "assistant" and t.text == "picked" for t in _kinds_since(am, s1, start))


@pytest.mark.p1
@pytest.mark.flow("STA-12")
@pytest.mark.am_settings(skipPermissions=False)
def test_esc_on_a_permission_prompt_reads_waiting_and_writes_nothing(am, iso, s1):
    """STA-12 E3 (a permission prompt): with skip-permissions off a Write asks; Esc refuses it: "[Request interrupted
    by user for tool use]", needs-approval → waiting-for-you, the interrupt hold — and the file is not written.

    Args:
        am: The app (started with skipPermissions off).
        iso: The test's folders.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    target = iso.folder("t1") / "refused.txt"
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, f'perm then esc [[mock write {str(target).replace(chr(92), "/")} "x"; reply "written"]]')
        tl.wait("needs-approval", STATE_WAIT_S)
        time.sleep(2)  # the prompt is drawn
        _press(am, "escape")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "needs-approval", "waiting-for-you"]), \
        _why(am, s1, marks, tl)
    _wait_transcript(am, s1, start, lambda t: t.type == "user"
                     and t.text.startswith("[Request interrupted by user for tool use]"), "the interrupt marker",
                     timeout_s=30)
    am.profile.hooks.wait(rf"\[interrupt-hold\] {s1.short}", since=marks[0], timeout_s=30)
    assert not target.exists(), "the refused Write wrote the file"


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


def _arm_blocking_stop_hook(am, iso):
    """Give Claude Code a user Stop hook that blocks ONE stop ({"decision": "block", "reason": "keep going"}) each time
    the test arms it, the way 164 stops were blocked in the owner's transcripts (flows/05 STA-13 phase 2).

    Args:
        am: The app (Claude Code's settings are seeded before any session starts).
        iso: The test's folders (the hook script and its arm file live in the work folder).
    Returns:
        The arm file: write it right before the prompt whose first stop must be blocked (the hook deletes it).
    """
    arm = iso.work_dir / "stopblock.arm"
    script = iso.work_dir / "stophook.ps1"
    script.write_text(
        "$arm = '" + str(arm) + "'\n"
        "if (Test-Path $arm) { Remove-Item $arm; Write-Output '{\"decision\": \"block\", \"reason\": \"keep going\"}' }\n"
        "exit 0\n", encoding="utf-8")
    am.claude.seed(settings={"hooks": {"Stop": [{"hooks": [{"type": "command", "command":
        "powershell -NoProfile -ExecutionPolicy Bypass -File " + str(script).replace("\\", "/")}]}]}})
    return arm


@pytest.mark.p1
@pytest.mark.flow("STA-13")
def test_a_blocking_stop_hooks_continuation_reads_running_within_the_accepted_lag(am, iso):
    """STA-13 phase 2 (STA-K4, an accepted delay since 2026-10-24): a user Stop hook blocks the first stop, so Claude
    Code goes on with the same turn ("Stop hook feedback: keep going"). Our own Stop hook already said "turn
    complete", so the session reads waiting-for-you at the start of the continuation — ACCEPTED: the busy heartbeat
    outlives the turn by 20 s and the session reads running again (`[recon-subagent] … outlived turn-end`) within
    ACCEPTED_LAG_S of the block, holds it while the continuation runs, and settles waiting-for-you after the
    continuation's own stop.

    Args:
        am: The app.
        iso: The test's folders.
    """
    arm = _arm_blocking_stop_hook(am, iso)
    s = am.launch("t1")
    marks = _marks(am)
    start = len(am.claude.transcript(s.id))
    with StateTimeline(am.window, s.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        arm.write_text("1", encoding="utf-8")
        _send(s, 'go [[mock say "first stop"; next; hold g; reply "continued"]]')
        _wait_transcript(am, s, start, lambda t: t.type == "user" and t.text.startswith("Stop hook feedback"),
                         "the blocked stop's feedback line")
        blocked_t = tl.elapsed()
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        # The continuation is held past the accepted lag: once the lag is over the card must show running, and have
        # shown it for a while — whether it read waiting-for-you first (0.8.0) or running all along (a later fix).
        while tl.elapsed() < blocked_t + ACCEPTED_LAG_S + 8:
            time.sleep(0.2)
        last = tl.changes()[-1]
        assert last.state == "running" and tl.elapsed() - last.t >= 5, \
            f"not running {ACCEPTED_LAG_S:.0f} s into a blocked stop's continuation: {_why(am, s, marks, tl)}"
        running_t = last.t
        am.mock.release("g")
        _wait_transcript(am, s, start, lambda t: t.type == "assistant" and t.text == "continued", "the continuation")
        tl.settled_at("waiting-for-you", STATE_WAIT_S, after_t=running_t)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "waiting-for-you"]), _why(am, s, marks, tl)
    assert "error" not in tl.states() and "done" not in tl.states(), _why(am, s, marks, tl)
    assert any(t.type == "attachment" and t.sub == "hook_blocking_error" for t in _kinds_since(am, s, start))


@pytest.mark.p1
@pytest.mark.flow("STA-05")
def test_a_long_tool_call_stays_running(am, s1):
    """STA-05 phase 1: a Bash call of 50 s writes nothing to the transcript while it runs; the session reads running
    for all of it — claude's heartbeat says busy, and a quiet transcript under a pending tool call is no turn end —
    then waiting-for-you after the reply.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'long tool [[mock bash "sleep 50"; reply "slept"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.stop == "tool_use", "the Bash call")
        tl.wait("running", STATE_WAIT_S)
        tl.hold("running", 35)
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "slept", "the reply after the tool")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)


@pytest.mark.p1
@pytest.mark.flow("STA-05")
def test_a_background_shell_that_outlives_its_turn_relights_running(am, s1):
    """STA-05 E1 (a live shell job): a background Bash of 60 s outlives its turn. The turn ends waiting-for-you; ~20 s
    after the transcript's last write claude's heartbeat still says `shell` (a job runs), and the session reads running
    again (`[recon-subagent] … (presence=shell, outlived turn-end, Waiting -> Running)`) BEFORE the job finishes; the
    job's notification turn follows, then waiting-for-you. (A toast for the first stop is held, then dropped as
    spurious: `[notify-drop] … re-lit running`.)

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'bg shell [[mock tool Bash {"command": "sleep 60", "run_in_background": true, "description": '
                  '"bg sleep"}; reply "started"]]')
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "started", "the turn's reply")
        am.profile.scanner.wait(rf"\[recon-subagent\] {s1.id} \(presence=shell, outlived turn-end, Waiting -> Running\)",
                                since=marks[1], timeout_s=STATE_WAIT_S)
        assert not any(t.text.startswith("<task-notification>") for t in _kinds_since(am, s1, start)
                       if t.type in ("user", "queue-operation")), "the job had already finished: no outlived shell seen"
        _wait_transcript(am, s1, start, lambda t: t.type == "assistant" and t.text == "mock: script done",
                         "the notification turn's answer", timeout_s=180)
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "waiting-for-you", "running", "waiting-for-you"]), \
        _why(am, s1, marks, tl)


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
def test_compact_settles_waiting_within_the_accepted_lag(am, s1):
    """STA-06 phase 1 (STA-K3, an accepted delay since 2026-10-24): `/compact` reads running while it compacts, and
    after its PostCompact the session settles waiting-for-you within ACCEPTED_LAG_S. On 0.8.0 the scanner sets running
    again 2 ms after the PostCompact (claude's heartbeat still reads busy) and the presence-idle release brings
    waiting-for-you back ~30 s later — accepted: the status settles by itself, and nothing is sent into it meanwhile
    (the runner's 40 s settle hold). Never error, never done.

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
        ended_t = tl.elapsed()
        settled_t = tl.settled_at("waiting-for-you", ACCEPTED_LAG_S + 10, hold_s=10)
    assert settled_t - ended_t <= ACCEPTED_LAG_S, _why(am, s1, marks, tl)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "waiting-for-you"]), _why(am, s1, marks, tl)
    assert "error" not in tl.states() and "done" not in tl.states(), _why(am, s1, marks, tl)


# ---- STA-07 · a turn cut off by a crash or a quit ---------------------------------------------------------------

CONTINUE_PROMPT = "Continue from where you left off."
"""What Claude Code sends by itself to continue a turn its resume found unfinished (a meta user line;
CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 is in every test's environment, amdriver/app.py)."""


def _all_marks(am, s) -> dict:
    """Marks taken BEFORE an app restart (the logs and the mock outlive it): the three logs' ends and the mock's last
    request.

    Args:
        am: The app.
        s: The session (unused; named for the reader of a call site).
    Returns:
        {"hooks", "scanner", "autorunner", "seq"}.
    """
    return {"hooks": am.profile.hooks.mark(), "scanner": am.profile.scanner.mark(),
            "autorunner": am.profile.autorunner.mark(), "seq": am.mock.last_seq()}


def _restart(am, how: str) -> None:
    """End the app — killed (a crash) or quit through its windows (the close flush runs) — and start it again on the
    same profile.

    Args:
        am: The app.
        how: "kill" or "quit".
    Returns:
        None.
    """
    if how == "quit":
        am.close()
    am.kill()
    am.start()


@pytest.mark.p0
@pytest.mark.flow("STA-07")
@pytest.mark.flow("INT-04")
@pytest.mark.parametrize("how", ["kill", "quit"])
def test_a_turn_cut_off_by_the_app_ending_reads_error_then_the_restore_wake_continues_it(am, s2, how):
    """STA-07 phases 1–4 (E1: a graceful quit is the same) and INT-04 in one window: t1 is mid-turn (a tool result,
    then a held response), t2 is the focused tab; the app ends — killed or quit — and starts again.

    Measured (2.1.291, 0.8.0): t1's dormant record reads ERROR at once — `[recon-cutoff] <id> (the last turn stopped
    on a tool result and never finished; the tab is dormant …)` — and its runner pauses (stop-on-error); once t2 (the
    focused tab) has launched, the restore wake starts t1 in place (`[nav] activate-dormant`, `[restore-wake] … done -
    woke 1 busy tab(s)`); its claude continues the turn by itself ("Continue from where you left off.", the scripted
    turn's NEXT response); running while that is held, waiting-for-you after it. Never a ready Idle the runner could
    send into.

    Args:
        am: The app.
        s2: F-S2 (t1, t2; t2 selected).
        how: "kill" (a crash) or "quit" (every window's X).
    """
    a, b = s2
    _send(a, f'cut off {how} [[mock bash "echo a"; hold g; reply "end"]]')
    am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
    b.select()  # t2 is the focused tab at the end: t1 restores dormant
    time.sleep(3)  # the window record's autosave keeps the focused tab
    marks = _all_marks(am, a)
    _restart(am, how)
    with StateTimeline(am.window, a.title) as tl:
        am.profile.scanner.wait(rf"\[recon-cutoff\] {a.id} \(the last turn stopped on a tool result and never "
                                rf"finished; the tab is dormant", since=marks["scanner"], timeout_s=60)
        tl.wait_seen("error", 60)
        am.profile.autorunner.wait(rf"\[stop-on-error\] paused {a.id}", since=marks["autorunner"], timeout_s=30)
        am.profile.hooks.wait(rf"\[nav\] activate-dormant {a.short}", since=marks["hooks"], timeout_s=STATE_WAIT_S)
        am.profile.hooks.wait(r"\[restore-wake\] window \{[0-9a-f-]+\}: done - woke 1 busy tab", since=marks["hooks"],
                              timeout_s=30)
        go_on = am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("session") == a.id
                                 and e.get("user_text") == CONTINUE_PROMPT, since=marks["seq"], timeout_s=STATE_WAIT_S)
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["error", "running", "waiting-for-you"]), \
        f"timeline: {tl.describe()}; engine: {engine_story(am.profile, a.id, marks['hooks'], marks['scanner'])}"
    assert "idle" not in tl.states()[tl.states().index("error"):], tl.describe()
    assert go_on.get("k") == 1, f"the continuation did not carry the turn's tool result: {go_on}"


@pytest.mark.p1
@pytest.mark.flow("STA-07")
def test_a_question_open_at_a_crash_reads_error_then_is_asked_again(am, s1):
    """STA-07 E4: an AskUserQuestion is open (needs-approval) when the app is killed. Measured: after the restart the
    record reads ERROR (`[recon-cutoff] … stopped on an unfinished assistant step`), never a stale needs-approval; the
    tab (the only one, focused) starts, Claude Code drops the unanswered call and continues the turn — its request ends
    with the user's prompt AND "Continue from where you left off." in ONE user message (the dropped call leaves two user
    texts in a row, which Claude Code joins) — and the turn asks the question AGAIN (the scripted turn's first
    response): needs-approval; Enter answers it and the turn ends waiting-for-you. On the busy desktop the restarted
    claude's SessionStart hook can land after the turn already reads running: idle for a blip (BLIP_S), never longer.

    Args:
        am: The app.
        s1: F-S.
    """
    _send(s1, 'ask then crash [[mock ask "Pick a side?" Left,Right; reply "picked"]]')
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("needs-approval", STATE_WAIT_S)
    time.sleep(2)
    marks = _all_marks(am, s1)
    _restart(am, "kill")
    with StateTimeline(am.window, s1.title) as tl:
        am.profile.scanner.wait(rf"\[recon-cutoff\] {s1.id} \(the last turn stopped on an unfinished assistant step",
                                since=marks["scanner"], timeout_s=60)
        tl.wait_seen("error", 60)
        again = am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("session") == s1.id
                                 and (e.get("user_text") or "").endswith(CONTINUE_PROMPT), since=marks["seq"],
                                 timeout_s=STATE_WAIT_S, what="the continuation request")
        assert again.get("user_text", "").startswith("ask then crash"), \
            f"the continuation did not carry the user's prompt: {again.get('user_text')!r}"
        tl.wait("needs-approval", STATE_WAIT_S)
        am.window.terminal.wait_text(r"Pick a side\?", timeout_s=60)
        _press(am, "enter")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    story = f"timeline: {tl.describe()}; engine: {engine_story(am.profile, s1.id, marks['hooks'], marks['scanner'])}"
    assert _ordered(tl.states(), ["error", "running", "needs-approval", "waiting-for-you"]), story
    assert _longest_stretch(tl, "idle") < BLIP_S, f"idle held while the turn went on: {story}"
    assert am.claude.wait_transcript(s1.id, lambda lines: any(
        t.type == "assistant" and t.text == "picked" for t in transcript_kinds(lines)), 30, what="the turn's answer")


@pytest.mark.p1
@pytest.mark.flow("STA-07")
def test_deactivate_mid_turn_reads_error_and_activate_continues_the_turn(am, s2):
    """STA-07 E5 (Deactivate): t1 is deactivated from t2 while its turn runs (a tool result, then a held response).
    Measured: the parked record reads ERROR (`[recon-cutoff] … the tab is dormant`) and its runner pauses; Activate
    Tab starts it in place, its claude continues the turn ("Continue from where you left off.", the turn's next
    response): running while held, waiting-for-you after it.

    Args:
        am: The app.
        s2: F-S2 (t1, t2; t2 selected).
    """
    a, b = s2
    w = am.window
    marks = _all_marks(am, a)
    with StateTimeline(w, a.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(a, 'park mid-turn [[mock bash "echo a"; hold g; reply "end"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        b.select()
        mark = am.profile.hooks.mark()
        w.tabs.deactivate("t1")
        a.wait_parked(since=mark)
        am.profile.scanner.wait(rf"\[recon-cutoff\] {a.id} \(the last turn stopped on a tool result and never "
                                rf"finished; the tab is dormant", since=marks["scanner"], timeout_s=60)
        tl.wait("error", 60)
        am.profile.autorunner.wait(rf"\[stop-on-error\] paused {a.id}", since=marks["autorunner"], timeout_s=30)
        tl.hold("error", 5)  # parked: nothing runs it
        w.tabs.activate("t1")
        a.wait_running(60)
        go_on = am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("session") == a.id
                                 and e.get("user_text") == CONTINUE_PROMPT, since=marks["seq"], timeout_s=STATE_WAIT_S)
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error", "running", "waiting-for-you"]), \
        f"timeline: {tl.describe()}; engine: {engine_story(am.profile, a.id, marks['hooks'], marks['scanner'])}"
    assert go_on.get("k") == 1, f"the continuation did not carry the turn's tool result: {go_on}"


@pytest.mark.p1
@pytest.mark.flow("STA-07")
def test_restart_session_mid_turn_continues_the_turn_running(am, s1):
    """STA-07 E5 (Restart session): Restart while the turn runs (a tool result, then a held response). Measured: the
    new claude starts at once and continues the turn ("Continue from where you left off.", the turn's next response),
    so the session reads running throughout — no error (a cut-off turn reads error only while NO process can run it:
    a dormant tab) — then waiting-for-you. On the busy desktop the new claude's SessionStart hook can land after the
    scanner has already promoted the continuing turn to running: the record reads idle for one scan (52 ms measured in
    run stcov1), so a blip of idle (BLIP_S) is allowed; an idle that holds is not.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    marks = _all_marks(am, s1)
    with StateTimeline(w, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'restart mid-turn [[mock bash "echo a"; hold g; reply "end"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        restart_t = tl.elapsed()
        w.tabs.context_menu("t1").choose("Restart session")
        am.profile.hooks.wait(rf"\[restart\] claude {s1.id} \(resume\)", since=marks["hooks"], timeout_s=30)
        go_on = am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("session") == s1.id
                                 and e.get("user_text") == CONTINUE_PROMPT, since=marks["seq"], timeout_s=STATE_WAIT_S)
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        # The continuation is held: a late SessionStart may blip idle meanwhile, so give it time, then the card must
        # read running again (and keep it).
        time.sleep(8)
        tl.hold("running", 5)
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    story = f"timeline: {tl.describe()}; engine: {engine_story(am.profile, s1.id, marks['hooks'], marks['scanner'])}"
    assert _ordered(tl.states(), ["waiting-for-you", "running", "waiting-for-you"]), story
    after = [c.state for c in tl.changes() if c.t > restart_t]
    assert "error" not in after and "done" not in after, story
    assert _longest_stretch(tl, "idle", restart_t) < BLIP_S, f"idle held while the turn went on: {story}"
    assert go_on.get("k") == 1, f"the continuation did not carry the turn's tool result: {go_on}"


# ---- STA-10 · /clear ----------------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("STA-10")
def test_clear_keeps_the_tab_managed_on_the_new_conversation(am, s1):
    """STA-10 phase 1: `/clear` in a managed session. Measured: claude switches to a NEW conversation id on the same
    ConPTY; the tab follows it (`[nav] tab-swap <old> -> <new>`, `[rehome] … inherits MANAGED provenance`, `[lineage]
    <new> continues <old> (/clear …)`), keeps its title, and the card reads idle (a new conversation, no turn yet); the
    next prompt runs in the new conversation (the mock sees a fresh context under the new id): running →
    waiting-for-you. The new record is not `external`. On the busy desktop (run stcov1) the card first read DONE for
    3 s: the old conversation's SessionEnd(reason clear) landed before the tab followed the new one, and the card
    showed the old record until the swap — a status lag, allowed within ACCEPTED_LAG_S and only before the idle.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    marks = _all_marks(am, s1)
    with StateTimeline(w, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        w.terminal.submit("/clear")
        swap = am.profile.hooks.wait(rf"\[nav\] tab-swap {s1.short} -> ([0-9a-f]{{8}})", since=marks["hooks"],
                                     timeout_s=STATE_WAIT_S)
        new = swap.group(1)
        am.profile.hooks.wait(rf"\[rehome\] {new}[0-9a-f-]* inherits MANAGED provenance from {s1.id}",
                              since=marks["hooks"], timeout_s=30)
        am.profile.hooks.wait(rf"\[lineage\] {new} continues {s1.short} \(/clear", since=marks["hooks"], timeout_s=30)
        tl.wait("idle", 60)
        w.terminal.wait_input_box(timeout_s=60)
        w.terminal.submit('after clear [[mock say "new"; hold g; reply "done"]]')
        # The new conversation's first request (its last user message also carries Claude Code's reminders, so it is
        # found by its session id, not by the prompt's words).
        first = am.mock.wait_for(lambda e: e.get("kind") == "main" and (e.get("session") or "").startswith(new),
                                 since=marks["seq"], timeout_s=STATE_WAIT_S)
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    states = tl.states()
    # A "done" may show first — the old conversation's SessionEnd(clear) before the tab follows the new one — but only
    # before the new conversation's idle, and not for longer than the accepted lag.
    assert [st for st in states if st != "done"] == ["waiting-for-you", "idle", "running", "waiting-for-you"], \
        tl.describe()
    assert "done" not in states or states.index("done") < states.index("idle"), tl.describe()
    assert _longest_stretch(tl, "done") < ACCEPTED_LAG_S, tl.describe()
    assert first.get("session", "").startswith(new) and first.get("n_msgs", 99) <= 3, first
    rec = am.profile.wait_session(lambda r: r.id.startswith(new), 15, what="the new conversation's record")
    assert rec.title == "t1" and not rec.external, (rec.title, rec.external)
    assert w.tabs.find("t1") is not None and w.side_panel.titles() == ["t1"]


# ---- STA-08 / STA-09 · one conversation in two tabs; a multi-megabyte transcript ------------------------------


@pytest.mark.p1
@pytest.mark.flow("STA-08")
def test_one_conversation_in_two_tabs_keeps_one_host(am, s1):
    """STA-08 phases 1–2: t1's conversation is resumed BY HAND in a new shell tab (`claude.exe --resume <id>` from the
    session's folder, with the test's Claude config). Measured: the observer keeps ONE host — `[observer] duplicate
    conversation <id>: host tab wt=<t1's ConPTY> …` and `[discover] <id> wt=<the shell tab's> … not bound` — so the
    session keeps its tab and its single side-panel card, and nothing re-homes it. A turn in the second tab writes the
    SAME transcript, so the session's state follows it too (the scanner reads the shared file); a turn in t1 then
    reads running → waiting-for-you as always.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    marks = _all_marks(am, s1)
    before = set(w.tabs.titles())
    with StateTimeline(w, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        w.focus()
        inputs.keys("ctrl+shift+t")  # a new shell tab (Windows Terminal's newTab)
        wait_until(lambda: len(w.tabs.titles()) > len(before), 20, what="a new shell tab")
        time.sleep(3)  # the shell's prompt
        w.terminal.focus()
        # The shell tab's environment comes from the registry, not from the test's isolation: point it at the test's
        # Claude config, run claude.exe by its path, from the session's folder (a resume looks the conversation up
        # there; the folder is pre-trusted in that config).
        exe = r"$env:USERPROFILE\.local\bin\claude.exe"
        inputs.type_text(f"cd '{s1.folder}'; $env:CLAUDE_CONFIG_DIR='{am.iso.claude_dir}'; & \"{exe}\" --resume "
                         f"{s1.id}", delay_s=0.004, newline_key="enter")
        inputs.keys("enter")
        w.terminal.wait_input_box(timeout_s=90)
        am.profile.hooks.wait(rf"\[observer\] duplicate conversation {s1.id}: host tab wt=", since=marks["hooks"],
                              timeout_s=60)
        am.profile.hooks.wait(rf"\[discover\] {s1.id} wt=[0-9a-f-]+ \(pid \d+\) not bound: tab wt=[0-9a-f-]+ hosts "
                              r"the same", since=marks["hooks"], timeout_s=60)
        w.terminal.submit('in the second tab [[mock reply "from the second tab"]]')
        am.claude.wait_transcript(s1.id, lambda lines: any(
            t.type == "assistant" and t.text == "from the second tab" for t in transcript_kinds(lines)),
            STATE_WAIT_S, what="the second tab's turn in the shared transcript")
        s1.select()
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
        first_t = tl.elapsed()
        _send(s1, 'in the first tab [[mock say "a"; hold g; reply "from the first tab"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.entered_after("running", first_t, STATE_WAIT_S)
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert w.side_panel.titles() == [s1.title], w.side_panel.titles()
    assert s1.record().title == s1.title and w.tabs.find(s1.title) is not None
    swaps = am.profile.hooks.find(rf"\[rehome\] {s1.id} ->|\[nav\] tab-swap {s1.short}", since=marks["hooks"])
    assert not swaps, f"the session moved between tabs: {[m.string for m in swaps]}"
    assert len(s1.claude_pids()) == 2, "both claudes should run the conversation"


@pytest.mark.p1
@pytest.mark.flow("STA-09")
def test_a_multi_megabyte_transcript_reads_right_after_a_restart(am, iso, s1):
    """STA-09 phases 1–3 in one: a turn writes a 1.5 MB file — its Write input and result put ~3 MB into the
    transcript, one line over the scanner's 1 MiB window; the app restarts, so the scanner meets a 3 MB transcript cold
    (its first-sight tail window, then the catch-up read); the next turn reads running → waiting-for-you as on a small
    one, never stuck on the old tail (measured: 3.19 MB, running at the prompt, waiting-for-you after the release).

    Args:
        am: The app.
        iso: The test's folders.
        s1: F-S.
    """
    target = str(iso.folder("t1") / "big.txt").replace("\\", "/")
    s1.ask(f'big write [[mock write {target} size=1500000; reply "written"]]', timeout_s=STATE_WAIT_S)
    path = am.claude.transcript_path(s1.id)
    assert path is not None and path.stat().st_size > 2_500_000, path and path.stat().st_size
    am.kill()
    am.start()
    am.window.terminal.wait_input_box(timeout_s=90)
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'after the restart [[mock say "x"; hold g; reply "done"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        tl.hold("running", 5)
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)


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
def test_esc_before_any_output_settles_waiting_within_the_accepted_lag(am, s1):
    """STA-12 E1 (STA-K2, an accepted delay since 2026-10-24): Esc before the model sent anything: Claude Code cancels
    the turn, puts the prompt back in the input box and writes nothing after the prompt's own line (no interrupt marker,
    no reply, no Stop). The session settles waiting-for-you within ACCEPTED_LAG_S of the Esc. On 0.8.0 it keeps reading
    running until the presence-idle release (30 s of quiet; measured 18–28 s) — accepted: the status settles by itself,
    and the runner's 40 s settle hold sends nothing into a moving status (what the runner does once it has settled:
    STA-K5). Never error, never done.

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
        esc_t = tl.elapsed()
        _press(am, "escape")
        # Claude Code restores the prompt into the box (its "Esc before output" behavior).
        wait_until(lambda: (lambda box: box is not None and box.draft.startswith("wait"))(am.window.terminal.input_box()),
                   30, what="the prompt back in the input box")
        settled_t = tl.settled_at("waiting-for-you", ACCEPTED_LAG_S + 10, hold_s=5, after_t=esc_t)
        am.mock.release("g")
    assert settled_t - esc_t <= ACCEPTED_LAG_S, _why(am, s1, marks, tl)
    assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"], _why(am, s1, marks, tl)


# ---- the runner across the accepted delays (flows/05 *Reclassified*; PQR-03, PQR-07) ------------------------


def _queue_for(am, s, text: str) -> None:
    """Queue a prompt for a session the way a user does — the Agent Manager page, the session's board card clicked
    (on 0.8.0 a session launched from the page is not selected there until a tab switch: MGR-K1), the Prompt Queue tab,
    the compose row (ManagerPage.queue_prompt confirms through `[nav] queue`) — then back to the session's tab.

    Args:
        am: The app.
        s: The Session.
        text: The prompt (with its script).
    Returns:
        None.
    Raises:
        LookupError: When nothing was queued.
    """
    page = am.window.manager.open()
    page.wait_card(s.title, timeout_s=20).click()
    time.sleep(0.5)  # the right pane follows the selection
    page.show_tab("Prompt Queue")
    page.queue_prompt(text)
    s.select()


def _line_unix(line) -> float:
    """A transcript line's own time stamp as Unix seconds — the moment Claude Code wrote it, independent of when a
    hook reached Agentmaster (hooks run seconds late on the busy test desktop).

    Args:
        line: A TranscriptLine (its raw line's "timestamp", UTC ISO 8601 with a trailing Z).
    Returns:
        The seconds since the epoch.
    Raises:
        ValueError: When the line carries no parsable time stamp (a line Claude Code writes without one, such as a
            file-history snapshot — never pass one).
    """
    return datetime.fromisoformat(str(line.raw.get("timestamp", "")).replace("Z", "+00:00")).timestamp()


@pytest.mark.p1
@pytest.mark.flow("STA-13")
@pytest.mark.flow("PQR-03")
def test_the_runner_sends_nothing_into_a_blocked_stops_continuation(am, iso):
    """STA-K4's safety (an accepted delay): a Stop hook blocks the first stop, and the session reads waiting-for-you at
    the start of the continuation — a status that is still MOVING. A prompt queued meanwhile (runner Auto) is held by the
    settle window (`[advance-skip] … status settle`, counted from that Waiting and restarted by the Running that
    follows) through the whole continuation — held 45 s here, longer than the 40 s window, so a runner that trusted the
    first Waiting would have sent into it — and goes only once the status held still after the continuation's own end:
    the turn after "continued", never part of it. Measured (probe r21, 2026-10-24): nothing sent while the continuation
    ran 30 s; sent 40.5 s after the continuation's Stop, 48.8 s after its reply line.

    Args:
        am: The app.
        iso: The test's folders.
    """
    arm = _arm_blocking_stop_hook(am, iso)
    s = am.launch("t1")
    marks = _all_marks(am, s)
    start = len(am.claude.transcript(s.id))
    with StateTimeline(am.window, s.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        arm.write_text("1", encoding="utf-8")
        _send(s, 'blocked [[mock say "first stop"; next; hold g; reply "continued"]]')
        _queue_for(am, s, 'queued after [[mock reply "from the queue"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        time.sleep(45)  # the continuation is held: the first stop's Waiting, the re-lit Running, all of it
        early = [p for p in am.mock.prompts(since=marks["seq"]) if "queued after" in str(p.get("user_text"))]
        assert not early, f"the runner sent the queued prompt into the blocked stop's continuation: " \
                          f"{_why(am, s, (marks['hooks'], marks['scanner']), tl)}"
        released_t = tl.elapsed()
        am.mock.release("g")
        cont = _wait_transcript(am, s, start, lambda t: t.type == "assistant" and t.text == "continued",
                                "the continuation's reply")
        am.mock.wait_prompt("queued after", since=marks["seq"], timeout_s=STATE_WAIT_S)
        queued = _wait_transcript(am, s, start, lambda t: t.type == "user" and t.text.startswith("queued after"),
                                  "the queued prompt's line")
        _wait_transcript(am, s, start, lambda t: t.type == "assistant" and t.text == "from the queue",
                         "the queued turn's reply")
        stop_t = tl.entered_after("waiting-for-you", released_t, STATE_WAIT_S)  # the continuation's own end
        run_t = tl.entered_after("running", stop_t, STATE_WAIT_S)  # the queued turn
        tl.settled_at("waiting-for-you", STATE_WAIT_S, after_t=run_t)
    # The queued prompt is the turn AFTER the continuation, sent only once the status had held still again: without the
    # settle window the runner would send within a few seconds of the continuation's Stop.
    gap = _line_unix(queued) - _line_unix(cont)
    assert gap >= 30, f"the queued prompt went {gap:.1f} s after the continuation's reply — inside the settle window"
    am.profile.autorunner.wait(rf"\[advance-skip\] {s.id} \(status settle", since=marks["autorunner"], timeout_s=10)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "waiting-for-you", "running", "waiting-for-you"]), \
        _why(am, s, (marks["hooks"], marks["scanner"]), tl)


@pytest.mark.p1
@pytest.mark.flow("STA-12")
@pytest.mark.flow("PQR-07")
@pytest.mark.known_defect("STA-K5")
def test_known_defect_esc_before_any_output_takes_no_interrupt_hold(am, s1):
    """STA-K5: Esc before the model sent anything takes no interrupt hold. Claude Code then writes nothing after the
    prompt's own line — no "[Request interrupted by user]" marker, no reply, no Stop — and puts the prompt back in the
    box, and the interrupt hold is taken only on that marker (SessionScanner → NoteInterrupt); every other Esc parks the
    runner until the user's next message (PQR-07, DELIVERY.md §14). TODAY, with a prompt queued (runner Auto), the
    status settles waiting-for-you (STA-K2's accepted ~30 s lag), the 40 s settle window passes, and the runner SENDS
    the queued prompt — the draft swap stashes the user's restored prompt around the send and puts it back (probe r22,
    2026-10-24: sent 66 s after the Esc). Expected once fixed: the runner parks (`[interrupt-hold] <sid8> …`) and sends
    nothing until the user's next message.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _all_marks(am, s1)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        _send(s1, 'wait then esc [[mock hold g; reply "never"]]')
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        _queue_for(am, s1, 'queued meanwhile [[mock reply "from the queue"]]')
        esc_t = tl.elapsed()
        _press(am, "escape")
        wait_until(lambda: (lambda box: box is not None and box.draft.startswith("wait then esc"))(
            am.window.terminal.input_box()), 30, what="the prompt back in the input box")
        # TODAY (the defect): the queued prompt is sent once the status has settled and held still …
        am.mock.wait_prompt("queued meanwhile", since=marks["seq"], timeout_s=180)
        sent_t = tl.elapsed()
        am.mock.release("g")
        tl.settled_at("waiting-for-you", STATE_WAIT_S, after_t=sent_t)
    assert sent_t - esc_t >= 30, f"sent {sent_t - esc_t:.1f} s after the Esc — before the status had settled"
    # … with no interrupt hold taken …
    assert not am.profile.hooks.find(rf"\[interrupt-hold\] {s1.short}", since=marks["hooks"])
    # … and the user's prompt survives the send (the draft swap stashed it and put it back).
    am.profile.hooks.wait(rf"\[draft-swap\] {s1.short} prompt sent and the draft is back in the box", since=marks["hooks"],
                          timeout_s=30)
    box = am.window.terminal.input_box()
    assert box is not None and box.draft.startswith("wait then esc"), box


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
