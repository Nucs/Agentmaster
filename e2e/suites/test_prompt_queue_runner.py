"""The Prompt Queue Runner through the session's status changes (doc/agentmaster/flows/03-prompt-queue-runner.md, PQR).

The runner types a queued prompt into a session only when the session is READY and has STAYED ready: open, its claude
started, Idle or Waiting for you, no question pending, its input box visible — and the status has held for the settle
window (40 s, `kStateSettleMs`) counted from the later of its last status change and the prompt's queue time, at least
20 s after its previous automatic send (DELIVERY.md §16, Scheduler.h DecideAdvance). Every test here moves a session
through one status change — a turn, a question, Esc, an API error, a menu on screen, a compaction, a background
agent, a park, the global pause, a mode switch, a restart — and checks what the runner did about it, from the oracles
the app does not control first:

- WHAT reached the model, and in which order: the mock's request log (`mock.prompts()`: each turn's first request);
- WHEN the runner decided to send: autorunner.log's `[send] <id> #<index>`, stamped by the app's own clock — the
  moment the settle window is measured to (the request reaches the mock 10–20 s later on a loaded desktop: Claude
  Code runs its UserPromptSubmit hook first);
- WHY it waited: `[advance-skip] <id> (<reason>)` (DecideAdvance's fixed reasons, logged once per stall);
- WHAT the user sees: the Prompt Queue tab (its runner toggle, the SENT / UPCOMING rows), the tab badge (its mode
  button, "⏳N", the row-3 cue), the toolbar's Pause / Resume — and the side panel's state word (`StateTimeline`),
  whose LAST change before a send is the settle window's anchor.

Measured with the probes of 2026-10-24 (run `pqprobe1`, 0.8.0 + Claude Code 2.1.291 in the test desktop of a busy PC):
- a prompt queued at rest reached the model 47–53 s after the click (the window, then Claude Code's own hook);
- after EVERY turn the session reads waiting-for-you, then running again ~20 s later for a few seconds (the scanner's
  `[recon-subagent] … outlived turn-end`: claude's heartbeat says busy while its slow Stop hooks run), then
  waiting-for-you — a status change that moves the anchor: a next prompt goes ~40 s after the LAST entry;
- the question guard held the next prompt 50+ s; the answer's turn released it 46 s after that turn ended;
- Esc parked the runner (`[interrupt-hold] <sid8> autorunner Full -> Off …`); the user's next message resumed it.
The production timings apply (0.8.0 has no E2E lane), so a test that sends costs about a minute per prompt.
"""
from __future__ import annotations

import time
from dataclasses import dataclass

import psutil
import pytest

from amdriver import inputs
from amdriver.timeline import StateTimeline, engine_story
from amdriver.ui.queue import global_paused, set_global_pause
from amdriver.waits import wait_until

SETTLE_S = 40.0
"""The settle window (Scheduler.h kStateSettleMs): an automatic send waits until the session has held a sendable status
this long, counted from the later of its last status change and the prompt's queue time."""
EARLY_S = 3.0
"""How much sooner than its window a send may LOOK to come: the test's own marks (the click that queued, the side
panel's state word) come up to ~1–2 s after the registry's stamps."""
DECIDE_LATE_S = 20.0
"""How much later than its window the runner's DECISION ([send]) may come: its timed re-check fires at the window's end,
then the throttle (500 ms), the verified placement's start and a busy scheduler on a loaded desktop."""
ARRIVE_S = 60.0
"""How long after the runner's decision the prompt may take to reach the model: the verified placement (fill, read
back, Enter) and Claude Code's own UserPromptSubmit hook (10–20 s each on a loaded PC)."""
RELEASE_S = SETTLE_S + 5.0
"""How soon after the USER releases a held queue (a re-arm, the global pause's Resume, a menu closed) the runner
decides to send: at once when the status has long held still, else when its settle window ends — on a loaded desktop
every turn is followed by a short running blip ~20 s later (the busy heartbeat outliving the Stop hook), which moves
the window's anchor, so a release can fall inside a window that is still running."""
RUNNER_WAIT_S = SETTLE_S + DECIDE_LATE_S + ARRIVE_S + 30.0
"""How long a test waits for a send the runner owes (the window, a flap that moves its anchor, the delivery)."""
HOLD_CHECK_S = SETTLE_S + 12.0
"""How long a "the runner holds" check watches: a whole settle window and more, so a send the hold failed to stop would
have come by then."""
STATE_WAIT_S = 120.0
"""How long a state may take to show on the side panel (a slow hook, then the scanner)."""
READY = ("waiting-for-you", "idle")
"""The state words of a session the runner may send into."""


# ---- helpers --------------------------------------------------------------------------------------------------------


@dataclass
class Marks:
    """The ends of the logs and the mock's request log before an action (read only what comes after).

    Attributes:
        hooks: hooks.log's end.
        scanner: scanner.log's end.
        autorunner: autorunner.log's end.
        seq: The mock's last request number.
    """

    hooks: int
    scanner: int
    autorunner: int
    seq: int


def _marks(am) -> Marks:
    """Mark the logs and the mock now.

    Args:
        am: The app.
    Returns:
        The Marks.
    """
    return Marks(am.profile.hooks.mark(), am.profile.scanner.mark(), am.profile.autorunner.mark(), am.mock.last_seq())


class Clock:
    """A StateTimeline with wall-clock times: the side panel's state changes, comparable with the app's log stamps and
    the mock's request times (one machine, one clock).

    Attributes:
        tl: The timeline (started by the caller).
        base: The wall-clock time the timeline started at (a change at tl time t happened at base + t).
    """

    def __init__(self, tl: StateTimeline) -> None:
        """Bind to a started timeline.

        Args:
            tl: The timeline (StateTimeline.start() stamped its monotonic start; a change's t counts from it).
        Returns:
            None.
        """
        self.tl = tl
        # The timeline keeps its start on the monotonic clock (a Change.t is relative to it); translate that start to
        # the wall clock once, so the Clock may be made at any time after the timeline started.
        self.base = time.time() - (time.monotonic() - tl._t0)

    def mark(self) -> int:
        """How many changes the timeline holds now (a later `entered(state, mark)` waits for a NEW change).

        Returns:
            The count.
        """
        return len(self.tl.changes())

    def entered(self, state: str, after: int, timeout_s: float = STATE_WAIT_S) -> float:
        """Wait until the timeline changes INTO a state after a mark (a new entry, not the state it already shows).

        Args:
            state: The state word.
            after: A mark().
            timeout_s: How long.
        Returns:
            The change's wall-clock time.
        Raises:
            WaitTimeout: When it never did (the message lists the timeline).
        """
        t = wait_until(lambda: next((c.t for c in self.tl.changes()[after:] if c.state == state), None), timeout_s,
                       interval_s=0.1, what=f"{self.tl.title!r} to enter {state!r}", describe=self.tl.describe)
        return self.base + t

    def last_change_before(self, when: float) -> tuple[float, str]:
        """The session's last state change before a moment: the settle window's anchor for a send at that moment.

        Args:
            when: A wall-clock time.
        Returns:
            (its wall-clock time, the state it changed into); (0.0, "") when nothing changed before it.
        """
        before = [c for c in self.tl.changes() if self.base + c.t <= when]
        return (self.base + before[-1].t, before[-1].state) if before else (0.0, "")

    def assert_settled_send(self, sent_at: float, floor: float = 0.0, what: str = "the prompt") -> float:
        """Assert that a send came a settle window after the later of the session's last status change before it and
        `floor` (the prompt's queue time, a wake), and that the status it went into was a ready one.

        Args:
            sent_at: The runner's decision ([send] stamp, wall-clock).
            floor: Another anchor the window counts from (0 = none).
            what: The prompt, for the message.
        Returns:
            The gap between the anchor and the send (s).
        Raises:
            AssertionError: When the send came too early or too late, or into a status the runner must not send to.
        """
        changed_at, state = self.last_change_before(sent_at)
        anchor = max(changed_at, floor)
        gap = sent_at - anchor
        assert state in READY, f"{what} was sent while the session read {state!r}: {self.tl.describe()}"
        assert SETTLE_S - EARLY_S <= gap <= SETTLE_S + DECIDE_LATE_S, \
            f"{what} was sent {gap:.1f} s after its anchor (the settle window is {SETTLE_S:.0f} s): {self.tl.describe()}"
        return gap


def _turn_at(am, marker: str, since: int, timeout_s: float = RUNNER_WAIT_S) -> float:
    """Wait for a turn whose prompt holds a text to reach the model, and say when.

    Args:
        am: The app.
        marker: A text of the prompt (its first words).
        since: A mock seq mark.
        timeout_s: How long.
    Returns:
        The request's arrival (time.time()).
    Raises:
        MockError: When it never arrived.
    """
    return am.mock.wait_prompt(marker, since=since, timeout_s=timeout_s).get("t", 0.0)


def _turns(am, marker: str, since: int) -> list[dict]:
    """Every turn whose prompt holds a text (a prompt sent twice shows twice).

    Args:
        am: The app.
        marker: A text of the prompt.
        since: A mock seq mark.
    Returns:
        The turns' first requests.
    """
    return [e for e in am.mock.prompts(since=since) if marker in e.get("user_text", "")]


def _assert_no_turn(am, marker: str, since: int, seconds: float, why: str) -> None:
    """Watch the mock for a while: no turn with this text may reach the model.

    Args:
        am: The app.
        marker: A text of the prompt.
        since: A mock seq mark.
        seconds: How long to watch (a hold check: past a whole settle window).
        why: What the hold is (the failure message).
    Returns:
        None.
    Raises:
        AssertionError: When the prompt was sent.
    """
    end = time.time() + max(0.0, seconds)
    while time.time() < end:
        assert not _turns(am, marker, since), f"{marker!r} was sent although {why}"
        time.sleep(0.5)


def _sends(am, s, since: int) -> list[float]:
    """The moments the runner decided to send into a session (autorunner.log `[send] <id> #<index>`, the app's clock).

    Args:
        am: The app.
        s: The Session.
        since: An autorunner.log mark.
    Returns:
        The decisions' wall-clock times, in log order (a Send now is no runner decision: it logs none).
    """
    return [when.timestamp() for when, _m in am.profile.autorunner.stamped(rf"\[send\] {s.id} #\d+", since=since)]


def _nth_send(am, s, since: int, n: int, timeout_s: float = 30.0) -> float:
    """Wait for the runner's n-th send decision into a session since a mark.

    Args:
        am: The app.
        s: The Session.
        since: An autorunner.log mark.
        n: 1 for the first.
        timeout_s: How long (call it after the mock got the prompt: the decision is logged before).
    Returns:
        Its wall-clock time.
    Raises:
        WaitTimeout: When there were fewer.
    """
    return wait_until(lambda: (lambda xs: xs[n - 1] if len(xs) >= n else None)(_sends(am, s, since)), timeout_s,
                      what=f"the runner's send #{n} into {s.short}")


def _skips(am, s, since: int) -> list[str]:
    """Why the runner did not send into a session: the reasons of its `[advance-skip]` lines.

    Args:
        am: The app.
        s: The Session.
        since: An autorunner.log mark.
    Returns:
        The reasons, in log order (a stall is logged once until another reason or a send).
    """
    return [m.group(1) for m in am.profile.autorunner.find(rf"\[advance-skip\] {s.id} \((.*)\)$", since=since)]


def _wait_skip(am, s, reason_start: str, since: int, timeout_s: float = 30.0) -> str:
    """Wait until the runner logs a reason for holding a session's queue.

    Args:
        am: The app.
        s: The Session.
        reason_start: The reason's beginning ("tab dormant", "question pending", "autorunner off" …).
        since: An autorunner.log mark.
        timeout_s: How long.
    Returns:
        The whole reason.
    Raises:
        WaitTimeout: When it was never logged (the message lists the reasons that were).
    """
    return wait_until(lambda: next((r for r in _skips(am, s, since) if r.startswith(reason_start)), None), timeout_s,
                      what=f"the runner to hold {s.short} for {reason_start!r}",
                      describe=lambda: _skips(am, s, since))


def _press(am, key: str) -> None:
    """Press a key in the selected tab's terminal (the window forward and the terminal clicked first: real keys reach
    only the focused control).

    Args:
        am: The app.
        key: The key ("escape", "enter").
    Returns:
        None.
    """
    am.window.focus()
    am.window.terminal.focus()
    inputs.keys(key)


def _why(am, s, marks: Marks, tl: StateTimeline = None) -> str:
    """The evidence a failed check carries: the runner's reasons and sends, the engine's story, the timeline.

    Args:
        am: The app.
        s: The Session.
        marks: Marks taken before the action.
        tl: The StateTimeline (None = none).
    Returns:
        One line.
    """
    runner = [ln for ln in am.profile.autorunner.lines(marks.autorunner) if s.short in ln][-12:]
    story = engine_story(am.profile, s.id, marks.hooks, marks.scanner)
    return f"runner: {runner}; engine: {story}" + (f"; timeline: {tl.describe()}" if tl is not None else "")


# ---- PQR-01 · one prompt into an idle session -----------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-01")
def test_a_queued_prompt_waits_out_the_settle_window_and_is_delivered_once(am, s1):
    """PQR-01 phases 1–5: a prompt queued into a session that is waiting for you waits a whole settle window from its
    queue time — the runner says "queue settle", the badge shows ⏳1 and "settling" — then is typed in exactly once,
    verified, and moves from UPCOMING to SENT with an "auto" pill; the turn reads running, then waiting-for-you.

    Args:
        am: The app.
        s1: F-S (its first prompt is the history's one "typed" row).
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Auto", pane.mode()  # the cog's default for new sessions
    assert pane.counts() == (1, 1, 0), pane.list_texts()  # F-S's first prompt: one sent row, typed
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        t_queue = pane.queue('p1 [[mock reply "one"]]')
        am.profile.hooks.wait(rf'\[nav\] queue {s1.short} "p1 ', since=marks.hooks, timeout_s=10)
        _wait_skip(am, s1, "queue settle", marks.autorunner)
        s1.select()
        badge = am.window.badge
        wait_until(lambda: badge.queued() == 1, 10, what="the badge's ⏳1", describe=badge.texts)
        assert badge.cue() == "settling" and "p1" in badge.prompt_line(), badge.texts()
        # Nothing goes out before the window ends …
        _assert_no_turn(am, "p1", marks.seq, SETTLE_S - EARLY_S - (time.time() - t_queue), "its settle window runs")
        # … and it goes out once it did.
        _turn_at(am, "p1", marks.seq)
        n = clock.mark()  # p1's turn runs now (its UserPromptSubmit hook said so before the request went out)
        clock.entered("waiting-for-you", n)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    clock.assert_settled_send(sent_at, floor=t_queue, what="p1")
    assert tl.seen("running"), _why(am, s1, marks, tl)
    am.profile.hooks.wait(rf"\[delivered\] {s1.short} prompt [0-9a-f]{{8}} \(chars=\d+, verified=exact",
                          since=marks.hooks, timeout_s=10)
    time.sleep(3)  # a second delivery would have reached the mock by now
    assert len(_turns(am, "p1", marks.seq)) == 1, "the prompt reached the model twice"
    assert len(_sends(am, s1, marks.autorunner)) == 1, _why(am, s1, marks)
    assert am.claude.user_prompts(s1.id).count('p1 [[mock reply "one"]]') == 1  # once in the transcript
    pane = am.window.queue.open(s1.title)
    row = pane.wait_row("p1", "sent")
    assert row.tag == "auto" and row.status == "sent", row.texts
    assert pane.counts() == (2, 1, 0), pane.list_texts()
    s1.select()
    wait_until(lambda: am.window.badge.queued() == 0 and not am.window.badge.prompt_line(), 10,
               what="the badge's queue parts gone", describe=am.window.badge.texts)


# ---- PQR-02 · one prompt per turn, never into a running turn -----------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-02")
@pytest.mark.slow
def test_the_queue_drains_one_prompt_per_turn_never_into_a_running_turn(am, s1):
    """PQR-02 phases 1–3 and PQR-09 phase 1: three queued prompts go one per turn, in order. While the first one's
    turn runs (held past a whole settle window) nothing else is sent, and each next prompt goes only after the turn
    before it ENDED and the status then held still for the settle window ("status settle"); at the end the runner logs
    the queue drained.

    While the turn runs the runner is not even asked: only an idle or waiting session's change requests a decision
    (Scheduler::_wantsIdleStart), so no "not ready (mid-turn …)" line is written (measured 2026-10-24, run pqr1) — the
    oracle for "nothing goes into a running turn" is the mock's silence while the side panel reads running.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue('d0 [[mock say "working"; hold g0; reply "r0"]]', 'd1 [[mock reply "r1"]]', 'd2 [[mock reply "r2"]]')
        assert [r.label[:2] for r in pane.upcoming()] == ["d0", "d1", "d2"], pane.list_texts()
        _turn_at(am, "d0", marks.seq)
        am.mock.wait_holding("g0", timeout_s=60)
        tl.wait("running", STATE_WAIT_S)
        _assert_no_turn(am, "d1", marks.seq, HOLD_CHECK_S, "the turn before it is still running")
        assert tl.current() == "running", tl.describe()  # the hold was a running turn all along
        am.mock.release("g0")
        _turn_at(am, "d1", marks.seq)
        _turn_at(am, "d2", marks.seq)
        am.profile.autorunner.wait(rf"\[plan-done\] {s1.id}", since=marks.autorunner, timeout_s=STATE_WAIT_S)
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 3, _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="d1")
    clock.assert_settled_send(sends[2], what="d2")
    assert any(r.startswith("status settle") for r in _skips(am, s1, marks.autorunner)), _why(am, s1, marks, tl)
    order = [e["user_text"][:2] for e in am.mock.prompts(since=marks.seq) if e["user_text"][:2] in ("d0", "d1", "d2")]
    assert order == ["d0", "d1", "d2"], order  # in queue order, each once
    pane = am.window.queue.open(s1.title)
    assert [r.label[:2] for r in pane.sent()][-3:] == ["d0", "d1", "d2"] and not pane.upcoming(), pane.list_texts()


@pytest.mark.p1
@pytest.mark.flow("PQR-02")
@pytest.mark.slow
def test_a_background_agents_work_holds_the_queue_until_it_is_done(am, s1):
    """PQR-02 E2: the queued turn starts a background agent (Claude Code 2.1.291 runs every Agent call in the
    background): the parent's turn ends at once, but the agent keeps working for ~35 s. The next queued prompt is not
    sent into that work: the session re-lights running once the work has outlived the turn (flows/05 STA-05), the
    agent's completion comes back as a notification turn, and only a settle window after the LAST change does the
    prompt go.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    sub = '[[mock bash \\"sleep 8\\"; bash \\"sleep 8\\"; bash \\"sleep 8\\"; bash \\"sleep 8\\"; reply \\"sub done\\"]]'
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue('bga [[mock tool Agent {"description": "bg agent", "prompt": "' + sub + '", '
                   '"subagent_type": "general-purpose", "run_in_background": true}; reply "started"]]',
                   'after [[mock reply "after the agent"]]')
        _turn_at(am, "bga [[", marks.seq)
        notified = am.mock.wait_for(lambda e: e.get("kind") == "main" and "<task-notification>" in e.get("user_text", ""),
                                    since=marks.seq, timeout_s=240, what="the agent's completion notification turn")
        t_after = _turn_at(am, "after [[", marks.seq)
    assert t_after > notified["t"], "the queued prompt went before the background agent's work was done"
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2, _why(am, s1, marks, tl)
    assert sends[1] > notified["t"], "the runner decided to send while the agent still worked"
    clock.assert_settled_send(sends[1], what="after")


# ---- PQR-03 · a status change restarts the settle window --------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("PQR-03")
def test_a_status_change_restarts_the_settle_window(am, s1):
    """PQR-03 phase 2: a prompt queued at rest is held by its queue's settle window; a turn typed in the terminal
    20 s later is a status change, which restarts the window: the prompt goes a whole window after THAT turn ended
    (after the last status change of it), not 40 s after it was queued ("status settle").

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        t_queue = pane.queue('later [[mock reply "queued one"]]')
        _wait_skip(am, s1, "queue settle", marks.autorunner)
        time.sleep(max(0.0, 20 - (time.time() - t_queue)))  # half the window gone
        s1.ask('typed [[mock reply "typed one"]]', wait="sent")
        _turn_at(am, "later", marks.seq)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    assert sent_at - t_queue >= SETTLE_S + 10, f"sent {sent_at - t_queue:.1f} s after its queue time: the typed turn " \
                                               f"did not restart the window; {_why(am, s1, marks, tl)}"
    clock.assert_settled_send(sent_at, floor=t_queue, what="later")
    assert any(r.startswith("status settle") for r in _skips(am, s1, marks.autorunner)), _why(am, s1, marks, tl)


# ---- PQR-04 · Paused and Auto -------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-04")
def test_paused_holds_the_queue_and_the_badges_auto_releases_it(am, s1):
    """PQR-04 phases 1–4: the pane's toggle pauses the session's runner (the badge follows at once); a queued prompt
    then waits past a whole settle window (the badge shows no "settling" — nothing waits for a clock); the badge's
    mode button switches back to Auto (the pane follows), and the runner sends by itself — within one settle window,
    at once when the status has long held still (a mode change is no status change: it does not restart the window).

    A Paused session's queue is not even put to the runner (Scheduler::_wantsIdleStart wants a mode other than Off),
    so no "autorunner off" line is written until a turn ends (measured 2026-10-24, run pqr1: no line at all); the
    oracles for the hold are the mock's silence and no `[send]`.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    am.profile.hooks.wait(rf"\[nav\] autorunner {s1.short} -> Off$", since=marks.hooks, timeout_s=10)
    pane.queue('held [[mock reply "h"]]')
    s1.select()
    badge = am.window.badge
    badge.wait_mode("Paused")
    wait_until(lambda: badge.queued() == 1, 10, what="the badge's ⏳1", describe=badge.texts)
    assert badge.cue() == "", badge.texts()
    _assert_no_turn(am, "held", marks.seq, HOLD_CHECK_S, "the session's runner is Paused")
    assert not _sends(am, s1, marks.autorunner), _why(am, s1, marks)
    t_click = time.time()
    assert badge.click_mode() == "Auto"
    am.profile.hooks.wait(rf"\[nav\] autorunner {s1.short} -> Full \(overlay\)", since=marks.hooks, timeout_s=10)
    _turn_at(am, "held", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    assert 0 <= sent_at - t_click <= RELEASE_S, f"sent {sent_at - t_click:.1f} s after the re-arm; " \
                                                    f"{_why(am, s1, marks)}"
    assert am.window.queue.open(s1.title).mode() == "Auto"


@pytest.mark.p1
@pytest.mark.flow("PQR-04")
@pytest.mark.am_settings(defaultAutorunnerMode="Off")
def test_the_cogs_default_mode_paused_starts_new_sessions_paused(am, s1):
    """PQR-04 E1: with the cog's default mode for new sessions set to Paused, a launched session starts Paused on
    both surfaces, and its queue waits for the user (no `[send]`, nothing reaches the model; a Paused queue writes no
    runner line — see the test above).

    Args:
        am: The app (settings.json says defaultAutorunnerMode Off before the start).
        s1: F-S, launched with that default.
    """
    marks = _marks(am)
    am.profile.hooks.wait(r"\[engine\] prompt queue runner started .*default mode for new sessions: Off", timeout_s=10)
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Paused", pane.mode()
    pane.queue('waits [[mock reply "w"]]')
    s1.select()
    am.window.badge.wait_mode("Paused")
    _assert_no_turn(am, "waits", marks.seq, HOLD_CHECK_S, "the session started Paused")
    assert not _sends(am, s1, marks.autorunner), _why(am, s1, marks)


# ---- PQR-05 · the global pause ------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-05")
@pytest.mark.slow
def test_the_global_pause_holds_every_window_and_resume_releases_them(am, s2):
    """PQR-05 phases 1–5: two sessions in two windows, each with a queued prompt. Window 1's toolbar pauses the runner:
    both windows' buttons read "Resume Prompt Queue Runner" and both badges "Auto (Off globally)" (the session's own
    mode kept, no "settling" cue); nothing is sent past a whole settle window ("global pause"); window 2's Resume
    re-asks every held queue by itself and both prompts go — within one settle window, no other trigger needed.

    Args:
        am: The app.
        s2: F-S2 (t2 is moved into a window of its own).
    """
    a, b = s2
    marks = _marks(am)
    w1 = am.window
    w2 = w1.tabs.move_to_new_window(b.title)
    set_global_pause(w1.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    wait_until(lambda: global_paused(w2.manager.open()), 10, what="window 2's toolbar to read Resume")
    w1.queue.open(a.title).queue('ga [[mock reply "a"]]')
    w2.queue.open(b.title).queue('gb [[mock reply "b"]]')
    a.select()
    b.select()
    w1.badge.wait_mode("Auto (Off globally)")
    w2.badge.wait_mode("Auto (Off globally)")
    assert w1.badge.cue() == "" and w2.badge.cue() == "", (w1.badge.texts(), w2.badge.texts())
    _wait_skip(am, a, "global pause", marks.autorunner)
    _wait_skip(am, b, "global pause", marks.autorunner)
    _assert_no_turn(am, "ga", marks.seq, HOLD_CHECK_S, "the runner is paused globally")
    assert not _turns(am, "gb", marks.seq), "gb was sent during the global pause"
    t_resume = time.time()
    set_global_pause(w2.manager, False)
    am.profile.autorunner.wait(r"\[pause-all\] off \(re-requested an advance for 2 session\(s\) with queued work\)",
                               since=marks.autorunner, timeout_s=10)
    assert not global_paused(w1.manager.open())
    _turn_at(am, "ga", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    _turn_at(am, "gb", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    for s in (a, b):
        sent_at = _nth_send(am, s, marks.autorunner, 1)
        assert 0 <= sent_at - t_resume <= RELEASE_S, f"{s.short} sent {sent_at - t_resume:.1f} s after the resume"


# ---- PQR-06 · Send now ----------------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-06")
def test_send_now_asks_first_then_types_at_once_into_a_paused_session(am, s1):
    """PQR-06 phases 1–3: Send now asks "Send now?" quoting the composed prompt; Cancel sends nothing and keeps the
    text; Send types it at once — no settle window, the session Paused, no runner decision — records it as a sent
    "auto" row and empties the box, and the session's mode stays Paused.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    prompt = 'now [[mock reply "sent at once"]]'
    dlg = pane.send_now(prompt)
    assert dlg.buttons() == ["Send", "Cancel"], dlg.buttons()
    assert dlg.text().endswith(f"“{prompt}”"), dlg.text()
    dlg.press("Cancel")
    assert pane.compose_text() == prompt
    _assert_no_turn(am, "now [[", marks.seq, 8, "Send now was cancelled")
    t_send = time.time()
    pane.send_now().press("Send")  # the box still holds the prompt
    t_sent = _turn_at(am, "now [[", marks.seq, timeout_s=ARRIVE_S)
    assert t_sent - t_send <= ARRIVE_S - 20, f"Send now took {t_sent - t_send:.1f} s to reach the model"
    am.profile.hooks.wait(rf'\[nav\] send-now {s1.short} "now ', since=marks.hooks, timeout_s=10)
    assert not _sends(am, s1, marks.autorunner), "Send now went through the runner"
    row = pane.wait_row("now", "sent")
    assert row.tag == "auto", row.texts
    assert pane.compose_text() == "" and pane.mode() == "Paused"
    time.sleep(3)
    assert len(_turns(am, "now [[", marks.seq)) == 1


@pytest.mark.p1
@pytest.mark.flow("PQR-06")
def test_send_now_into_a_running_turn_runs_as_the_next_turn(am, s1):
    """PQR-06 E4: Send now while a turn runs: the prompt is typed into Claude Code's box at once and Claude Code
    keeps it until the running turn ends, then runs it as the next turn — once, never merged into the running one;
    the session never reads error.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        s1.ask('busy [[mock say "working"; hold g; reply "first done"]]', wait="sent")
        am.mock.wait_holding("g", timeout_s=60)
        tl.wait("running", STATE_WAIT_S)
        pane = am.window.queue.open(s1.title)
        pane.send_now('next [[mock reply "second done"]]').press("Send")
        am.profile.hooks.wait(rf"\[delivered\] {s1.short} prompt", since=marks.hooks, timeout_s=60)
        _assert_no_turn(am, "next [[", marks.seq, 8, "the running turn has not ended yet")
        n = clock.mark()
        am.mock.release("g")
        _turn_at(am, "next [[", marks.seq, timeout_s=90)
        clock.entered("waiting-for-you", n)
    assert len(_turns(am, "next [[", marks.seq)) == 1
    prompts = am.claude.user_prompts(s1.id)
    assert sum('next [[mock reply "second done"]]' in p for p in prompts) == 1, prompts[-4:]
    assert "error" not in tl.states(), tl.describe()


# ---- PQR-07 · Esc parks the runner until the user's next message ------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-07")
@pytest.mark.slow
def test_esc_parks_the_runner_until_the_users_next_message(am, s1):
    """PQR-07 phases 1–4: Esc during a queued turn is the user taking the wheel: the runner parks itself ("Paused →
    Auto on your next message" on the toggle, "Paused → Auto" on the badge) and the next queued prompt waits past a
    whole settle window; the user's own next message resumes Auto ("your message arrived"), and the queued prompt
    follows a settle window after that message's turn — never into it.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue('p1 [[mock say "working"; hold g1; reply "one"]]', 'p2 [[mock reply "two"]]')
        s1.select()
        _turn_at(am, "p1", marks.seq)
        am.mock.wait_holding("g1", timeout_s=60)
        tl.wait("running", STATE_WAIT_S)
        _press(am, "escape")
        am.profile.autorunner.wait(rf"\[interrupt-hold\] {s1.short} autorunner Full -> Off", since=marks.autorunner,
                                   timeout_s=30)
        am.mock.release("g1")
        am.window.badge.wait_mode("Paused → Auto")
        assert am.window.queue.open(s1.title).mode() == "Paused → Auto on your next message"
        s1.select()
        _wait_skip(am, s1, "autorunner off (interrupt hold", marks.autorunner)
        _assert_no_turn(am, "p2", marks.seq, HOLD_CHECK_S, "the runner is parked by the interrupt")
        s1.ask('go [[mock reply "ok"]]', wait="sent")
        am.profile.hooks.wait(rf"\[interrupt-resume\] {s1.short} autorunner Off -> Full \(your message arrived\)",
                              since=marks.hooks, timeout_s=60)
        t_go = am.mock.prompts(since=marks.seq)[-1]["t"]
        _turn_at(am, "p2", marks.seq)
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2 and sends[1] > t_go, _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="p2")
    assert am.window.queue.open(s1.title).mode() == "Auto"


@pytest.mark.p1
@pytest.mark.flow("PQR-07")
def test_a_mode_chosen_during_the_interrupt_hold_cancels_its_resume(am, s1):
    """PQR-07 E3: during the interrupt hold the user picks a mode themselves (the toggle: Auto, then Paused) — that
    cancels the pending resume ("cancels the interrupt hold"), so their next message changes nothing: no resume, the
    runner stays Paused and the queued prompt keeps waiting.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('p1 [[mock say "working"; hold g1; reply "one"]]', 'p2 [[mock reply "two"]]')
    s1.select()
    _turn_at(am, "p1", marks.seq)
    am.mock.wait_holding("g1", timeout_s=60)
    _press(am, "escape")
    am.profile.autorunner.wait(rf"\[interrupt-hold\] {s1.short}", since=marks.autorunner, timeout_s=30)
    am.mock.release("g1")
    pane = am.window.queue.open(s1.title)
    pane.wait_mode("Paused → Auto on your next message")
    assert pane.click_toggle() == "Auto"
    am.profile.hooks.wait(rf"\[nav\] autorunner {s1.short} -> Full \(cancels the interrupt hold\)", since=marks.hooks,
                          timeout_s=10)
    assert pane.click_toggle() == "Paused"
    s1.ask('go [[mock reply "ok"]]', wait="sent")
    _assert_no_turn(am, "p2", marks.seq, HOLD_CHECK_S, "the user chose Paused during the hold")
    assert not am.profile.hooks.find(rf"\[interrupt-resume\] {s1.short}", since=marks.hooks)
    assert am.window.queue.open(s1.title).mode() == "Paused"


# ---- PQR-08 · a question holds the queue ------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-08")
@pytest.mark.slow
def test_a_question_holds_the_queue_until_the_user_answers(am, s1):
    """PQR-08 phases 1–2: a queued turn that ends with a question holds the next queued prompt past a whole settle
    window — "question pending …" in the log, "❓ held — answer the question" on the badge; the user's answer typed in
    the terminal ends without a question, and the held prompt goes a settle window after that turn.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue('q [[mock reply "Shall I continue?"]]', 'next [[mock reply "n"]]')
        s1.select()
        _turn_at(am, "q [[", marks.seq)
        _wait_skip(am, s1, "question pending", marks.autorunner, timeout_s=90)
        wait_until(lambda: am.window.badge.cue() == "held", 15, what="the badge's question cue",
                   describe=am.window.badge.texts)
        assert "next" in am.window.badge.prompt_line(), am.window.badge.texts()
        _assert_no_turn(am, "next [[", marks.seq, HOLD_CHECK_S, "the last turn ended with a question")
        s1.ask('yes [[mock reply "Fine, done."]]', wait="sent")
        t_yes = am.mock.prompts(since=marks.seq)[-1]["t"]
        _turn_at(am, "next [[", marks.seq)
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2 and sends[1] > t_yes, _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="next")


@pytest.mark.p1
@pytest.mark.flow("PQR-08")
def test_re_arming_the_runner_releases_a_question_hold(am, s1):
    """PQR-08 phase 3: the question hold also ends when the user re-arms the runner (the toggle: Paused, then Auto —
    an explicit re-arm clears the latch): the held prompt then goes by itself, within one settle window.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('q [[mock reply "Do you want me to go on?"]]', 'next [[mock reply "n"]]')
    _turn_at(am, "q [[", marks.seq)
    _wait_skip(am, s1, "question pending", marks.autorunner, timeout_s=90)
    _assert_no_turn(am, "next [[", marks.seq, HOLD_CHECK_S, "the last turn ended with a question")
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    t_arm = time.time()
    assert pane.click_toggle() == "Auto"
    _turn_at(am, "next [[", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    sent_at = _nth_send(am, s1, marks.autorunner, 2)
    assert 0 <= sent_at - t_arm <= RELEASE_S, f"next {sent_at - t_arm:.1f} s after the re-arm"


# ---- PQR-09 · states and screens that hold the queue ----------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("PQR-09")
@pytest.mark.slow
def test_an_open_question_dialog_holds_the_queue_until_it_is_answered(am, s1):
    """PQR-09 phase 2: a queued turn opens an AskUserQuestion dialog: needs-approval, and the next queued prompt waits
    past a whole settle window; Enter answers the dialog in the terminal, the turn ends, and the prompt goes a settle
    window after its last status change. (While the dialog is open nothing asks the runner to decide — no "not ready"
    line is written — so the hold is read from the mock's silence while the side panel reads needs-approval.)

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue('pick [[mock ask "Pick a side?" Left,Right; reply "picked"]]', 'after [[mock reply "a"]]')
        s1.select()
        _turn_at(am, "pick", marks.seq)
        tl.wait("needs-approval", STATE_WAIT_S)
        _assert_no_turn(am, "after [[", marks.seq, HOLD_CHECK_S, "a question dialog is open")
        assert len(_sends(am, s1, marks.autorunner)) == 1, _why(am, s1, marks, tl)
        am.window.terminal.wait_text(r"Pick a side\?", timeout_s=30)
        _press(am, "enter")
        t_answer = time.time()
        _turn_at(am, "after [[", marks.seq)
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2 and sends[1] > t_answer, _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="after")


@pytest.mark.p1
@pytest.mark.flow("PQR-09")
def test_a_menu_on_screen_holds_the_queue_until_it_closes(am, s1):
    """PQR-09 phase 3: Claude Code's /model picker on screen hides the input box, so a queued prompt waits past a whole
    settle window ("input box not visible …"); Esc closes the picker and, once the box shows again, the prompt goes
    within one settle window (its queue time long past).

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    s1.select()
    am.window.terminal.focus()
    am.window.terminal.type("/model")
    inputs.keys("enter")
    wait_until(lambda: am.window.terminal.input_box() is None, 20, what="the /model picker over the input box",
               describe=lambda: am.window.terminal.lines()[-10:])
    am.window.queue.open(s1.title).queue('after [[mock reply "a"]]')
    s1.select()
    _wait_skip(am, s1, "input box not visible", marks.autorunner, timeout_s=SETTLE_S + 30)
    _assert_no_turn(am, "after [[", marks.seq, HOLD_CHECK_S, "a menu covers the input box")
    t_esc = time.time()
    _press(am, "escape")
    am.window.terminal.wait_input_box(timeout_s=20)
    _turn_at(am, "after [[", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    assert 0 <= sent_at - t_esc <= RELEASE_S, f"after {sent_at - t_esc:.1f} s after the picker closed"


@pytest.mark.p1
@pytest.mark.flow("PQR-09")
@pytest.mark.slow
def test_a_compaction_holds_the_queue_until_the_status_settles(am, s1):
    """PQR-09 E2: a prompt queued while `/compact` runs is not sent during the compaction, nor at the moment its
    PostCompact sets waiting-for-you — the status runs again right after (flows/05 STA-K3, an accepted delay) — but a
    settle window after the session's LAST status change.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        s1.select()
        am.window.terminal.submit("/compact")
        tl.wait("running", STATE_WAIT_S)
        am.window.queue.open(s1.title).queue('after [[mock reply "after the compaction"]]')
        am.profile.hooks.wait(rf"\[PostCompact\] {s1.id}", since=marks.hooks, timeout_s=STATE_WAIT_S)
        compacted = time.time()
        _turn_at(am, "after [[", marks.seq)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    assert sent_at >= compacted + SETTLE_S - EARLY_S, f"sent {sent_at - compacted:.1f} s after the compaction ended"
    clock.assert_settled_send(sent_at, what="after")
    kinds = [e.get("kind") for e in am.mock.requests(since=marks.seq)]
    assert "compact" in kinds and kinds.index("compact") < len(kinds) - 1, kinds


# ---- PQR-10 · never into a dormant tab --------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-10")
@pytest.mark.slow
def test_a_parked_tab_never_gets_a_queued_prompt_until_it_is_woken(am, s2):
    """PQR-10 phases 1–2: a prompt queued into t1, then t1 parked (Deactivate) before its window ended: the dormant
    gate holds it past a whole settle window ("tab dormant …") — nothing typed into the parked tab, no lost-send verdict;
    Activate Tab starts its claude again, and the prompt goes once the started session has settled.

    Args:
        am: The app.
        s2: F-S2 (t2 is where the user is while t1 is parked).
    """
    a, b = s2
    marks = _marks(am)
    am.window.queue.open(a.title).queue('p [[mock reply "after the wake"]]')
    b.select()
    mark = am.profile.hooks.mark()
    am.window.tabs.deactivate(a.title)
    a.wait_parked(since=mark)
    _wait_skip(am, a, "tab dormant", marks.autorunner, timeout_s=SETTLE_S + 20)
    _assert_no_turn(am, "p [[", marks.seq, HOLD_CHECK_S, "the tab is parked")
    assert not am.profile.hooks.find(rf"\[lost-send\] {a.short}", since=marks.hooks)
    t_wake = time.time()
    am.window.tabs.activate(a.title)
    a.wait_running(timeout_s=60)
    _turn_at(am, "p [[", marks.seq)
    sent_at = _nth_send(am, a, marks.autorunner, 1)
    assert SETTLE_S - EARLY_S <= sent_at - t_wake, f"sent {sent_at - t_wake:.1f} s after the wake: before the start settled"
    assert len(_turns(am, "p [[", marks.seq)) == 1


# ---- PQR-11 · failures pause the runner -------------------------------------------------------------------------------------------


ERROR_SCRIPT = 'bad [[mock error 400 invalid_request_error "Output blocked by content filtering policy"]]'
"""A queued prompt whose turn ends in a final API error (a 400 is not retried; flows/05 STA-04)."""


@pytest.mark.p0
@pytest.mark.flow("PQR-11")
@pytest.mark.slow
def test_an_api_error_pauses_the_runner_until_the_user_re_arms_it(am, s1):
    """PQR-11 phases 1–2: a queued turn ends in an API error: Error, and stop-on-error pauses the session's runner
    ("[stop-on-error] paused", the toggle reads Paused); the next queued prompt waits. A retry typed in the terminal
    brings the session back to waiting-for-you — and the pause is sticky: nothing is sent until the user re-arms the
    runner, and then the prompt goes by itself, within one settle window.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)
        pane.queue(ERROR_SCRIPT, 'after [[mock reply "a"]]')
        s1.select()
        _turn_at(am, "bad [[", marks.seq)
        tl.wait("error", STATE_WAIT_S)
        am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner, timeout_s=30)
        assert am.window.queue.open(s1.title).mode() == "Paused"
        s1.select()
        clock = Clock(tl)
        n = clock.mark()
        s1.ask('retry [[mock reply "ok"]]', wait="sent")
        clock.entered("waiting-for-you", n)
        _assert_no_turn(am, "after [[", marks.seq, HOLD_CHECK_S, "the runner was paused by the error")
        pane = am.window.queue.open(s1.title)
        assert pane.mode() == "Paused"
        t_arm = time.time()
        assert pane.click_toggle() == "Auto"
        _turn_at(am, "after [[", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    sent_at = _nth_send(am, s1, marks.autorunner, 2)
    assert 0 <= sent_at - t_arm <= RELEASE_S, f"after {sent_at - t_arm:.1f} s after the re-arm"
    assert "autorunner off" in _skips(am, s1, marks.autorunner), _why(am, s1, marks, tl)


@pytest.mark.p1
@pytest.mark.flow("PQR-11")
@pytest.mark.am_settings(stopOnError=False)
@pytest.mark.slow
def test_with_stop_on_error_off_the_queue_waits_out_the_error(am, s1):
    """PQR-11 phase 3: with the cog's stop-on-error off for new sessions, an API error leaves the runner on Auto — but
    the session is not ready while it reads Error, so the next prompt waits (no runner line needs to say so: an Error
    session's change asks the runner nothing); once a typed retry brings the session back, the prompt goes a settle
    window after the retry's turn.

    Args:
        am: The app (settings.json says stopOnError false before the start).
        s1: F-S, launched with that default.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue(ERROR_SCRIPT, 'after [[mock reply "a"]]')
        s1.select()
        _turn_at(am, "bad [[", marks.seq)
        tl.wait("error", STATE_WAIT_S)
        _assert_no_turn(am, "after [[", marks.seq, HOLD_CHECK_S, "the session reads Error")
        assert len(_sends(am, s1, marks.autorunner)) == 1, _why(am, s1, marks, tl)
        assert not am.profile.autorunner.find(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner)
        assert am.window.queue.open(s1.title).mode() == "Auto"
        s1.select()
        s1.ask('retry [[mock reply "ok"]]', wait="sent")
        t_retry = am.mock.prompts(since=marks.seq)[-1]["t"]
        _turn_at(am, "after [[", marks.seq)
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2 and sends[1] > t_retry, _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="after")


# ---- PQR-15 · the queue across a restart ------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-15")
@pytest.mark.slow
def test_a_queue_survives_a_restart_and_nothing_sent_is_sent_again(am, s1):
    """PQR-15 phases 1–2: a Paused session holding two queued prompts survives a quit and a reopen — same rows, same
    statuses, the sent one never typed again. The restored session's mode is the cog's default (Auto), not the saved
    Paused one (by design today: a reopened session is re-armed on the default), so once its tab has started and
    settled the two prompts go, one per turn.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    pane.queue('r1 [[mock reply "one"]]', 'r2 [[mock reply "two"]]')
    am.restart()
    pane = am.window.queue.open(s1.title)
    assert [r.label[:2] for r in pane.upcoming()] == ["r1", "r2"], pane.list_texts()
    assert [r.label[:5] for r in pane.sent()] == ["ready"], pane.list_texts()
    assert pane.mode() == "Auto", pane.mode()  # the cog's default, re-stamped at the restored launch
    s1.select()
    _turn_at(am, "r1", marks.seq)
    _turn_at(am, "r2", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    assert not _turns(am, "ready [[", marks.seq), "the history's prompt was sent again after the restart"


@pytest.mark.p0
@pytest.mark.flow("PQR-15")
@pytest.mark.slow
def test_a_turn_cut_off_by_a_quit_pauses_the_runner_and_holds_the_queue(am, s1):
    """PQR-15 phase 3: the app quits while a queued turn runs and another prompt waits. After the reopen the cut-off
    turn reads Error and stop-on-error pauses the runner (Claude Code continues the turn by itself on its resume,
    `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1`), so the waiting prompt is not sent after the turn: the user decides.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    am.window.queue.open(s1.title).queue('p1 [[mock bash "echo a"; hold g1; reply "one"]]', 'p2 [[mock reply "two"]]')
    s1.select()
    _turn_at(am, "p1", marks.seq)
    am.mock.wait_holding("g1", timeout_s=90)
    am.close(how="all")
    am.kill()
    am.mock.release("g1")
    am.start()
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner, timeout_s=STATE_WAIT_S)
    _assert_no_turn(am, "p2", marks.seq, HOLD_CHECK_S, "the turn was cut off by the quit")
    assert am.window.queue.open(s1.title).mode() == "Paused"


# ---- known defects (flows/03 "Known defects") ----------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("PQR-06")
@pytest.mark.known_defect("PQR-K1")
def test_known_defect_send_now_sends_a_selected_history_row_again(am, s1):
    """PQR-K1 (`AgentManagerContent.AutoTesting.cpp` `_DoSendNow`): with an empty compose box Send now sends the
    SELECTED row, whatever its status. TODAY: select F-S's sent, typed row in SENT, click "!", Send — the confirmation
    quotes that history row and the prompt is typed into the session a second time. Expected once fixed: a history row
    is never sent again (Send now does nothing for it, or is disabled).

    Args:
        am: The app.
        s1: F-S (its first prompt is the history's typed row).
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.row("ready", "sent").click()
    dlg = pane.send_now()
    assert "ready [[mock reply" in dlg.text(), dlg.text()  # TODAY: it offers to send the history row
    dlg.press("Send")
    _turn_at(am, "ready [[", marks.seq, timeout_s=ARRIVE_S)  # TODAY: typed into the session again


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
@pytest.mark.known_defect("PQR-K2")
def test_known_defect_move_up_swaps_a_queued_row_with_a_history_row(am, s1):
    """PQR-K2 (`_OnMovePrompt`): Move up / Move down swap a row with its neighbor in the RAW queue, history rows
    included. The raw queue here is [ready (sent, typed), b (queued), c (sent, typed), d (queued)]. TODAY: Move up on d
    swaps it with c — a history row — so UPCOMING still reads b, d. Expected once fixed: d moves above b.

    Args:
        am: The app.
        s1: F-S.
    """
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"  # nothing may be sent while the queue is built
    pane.queue('b [[mock reply "b"]]')
    s1.ask('c [[mock reply "c"]]')  # typed in the terminal: a history row between b and d
    pane = am.window.queue.open(s1.title)
    pane.queue('d [[mock reply "d"]]')
    assert [r.label[:1] for r in pane.upcoming()] == ["b", "d"], pane.list_texts()
    pane.row_menu(pane.row("d [[", "upcoming")).choose("Move up")
    time.sleep(1.0)
    assert [r.label[:1] for r in pane.upcoming()] == ["b", "d"], pane.list_texts()  # TODAY: unchanged


@pytest.mark.p2
@pytest.mark.flow("PQR-14")
@pytest.mark.known_defect("PQR-K3")
def test_known_defect_the_boards_send_count_counts_typed_history(am, s1):
    """PQR-K3: the board card's "⚙ sent/total" counts every row of the session's queue — the history too. TODAY: F-S
    has one prompt the user TYPED and nothing queued, and its card says "⚙ 1/1" (in the Auto blue). Expected once
    fixed: no count for a session with nothing queued (or 0/0).

    Args:
        am: The app.
        s1: F-S.
    """
    card = am.window.manager.open().wait_card(s1.title)
    assert "⚙ 1/1" in card.texts, card.texts  # TODAY


@pytest.mark.p2
@pytest.mark.flow("PQR-03")
@pytest.mark.known_defect("PQR-K6")
@pytest.mark.slow
def test_known_defect_restart_session_does_not_restart_the_settle_window(am, s1):
    """PQR-K6 (found by the probe pqprobe1 pq04, 2026-10-24): a prompt the runner held because the session's claude
    had ended (no Claude input box on the screen) is typed into the claude that Restart session starts as soon as its
    input box draws — measured 8 s after the restart, a second BEFORE that claude's SessionStart hook ran. It went
    through (the Enter-retry's press 3 s later found the box already empty: only the UserPromptSubmit hook was slow),
    but the settle window exists to keep a prompt out of exactly such a claude's first seconds ("a resumed tab's
    start-up", DELIVERY.md §16), and a parked tab's wake gets it (pq05: the send 40.7 s after Activate Tab — `started`
    changed) while a restart does not: the session read waiting-for-you before and after it (a claude that ended
    without its SessionEnd never reads done, STA-K1) and its `started` never changed, so the window had long ended.
    Expected once fixed: the window counts from the restarted claude's start, so the prompt goes ≥ 40 s after it.

    The claude is KILLED here (≥ 10 s after its turn, so the session keeps reading waiting-for-you): the deterministic
    way to an ended claude whose session still reads ready. The probe met the same with `/exit`, whose SessionEnd hook
    the loaded desktop cancelled; on a fast PC `/exit` reads done, and the SessionStart after a restart is then a status
    change.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    time.sleep(10)  # well past F-S's turn (a kill within ~2 s of a turn's end reads running for good: STA-K1)
    for pid in s1.claude_pids():
        try:
            psutil.Process(pid).kill()
        except psutil.NoSuchProcess:
            pass
    wait_until(lambda: not s1.claude_pids(), 30, what="the claude to be gone")
    am.window.terminal.wait_text(r"\[Agentmaster\] The agent exited", timeout_s=60)
    t_queue = am.window.queue.open(s1.title).queue('back [[mock reply "I am back"]]')
    s1.select()
    _assert_no_turn(am, "back [[", marks.seq, max(0.0, SETTLE_S + 5 - (time.time() - t_queue)),
                    "claude has exited")  # the queue's own window is over by the restart
    t_restart = time.time()
    before = set(s1.claude_pids())
    am.window.tabs.context_menu(s1.title).choose("Restart session")
    wait_until(lambda: [p for p in s1.claude_pids() if p not in before] or None, 60, what="the restarted claude")
    _turn_at(am, "back [[", marks.seq)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    assert sent_at - t_restart < SETTLE_S - 10, \
        f"sent {sent_at - t_restart:.1f} s after the restart: the defect is gone? ({_why(am, s1, marks)})"


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
@pytest.mark.flow("LIF-07")
@pytest.mark.known_defect("PQR-K7")
@pytest.mark.slow
def test_known_defect_clear_leaves_the_tabs_queue_behind(am, s1):
    """PQR-K7 (found by the probe pqprobe1 pq06, 2026-10-24): `/clear` in a tab starts a new conversation there —
    the tab, its card and its Telegram topic follow it (`[nav] tab-swap <old> -> <new>`, `[lineage] <new> continues
    <old> (/clear …)`) — but the QUEUE does not: the prompts queued for the tab stay in the old conversation's record,
    which is archived. TODAY: the tab's new conversation shows an empty queue, its runner on the cog's default (Auto)
    although the user had paused it, the Prompt Queue tab says "Select a session …" until its new card is picked, and
    the queued prompt is never sent (it would go into the OLD conversation if that one were ever resumed). Expected
    (to be decided by the owner, like TAB-K2): the queue and the runner's mode follow the tab, as its topic does.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    pane.queue('kept [[mock reply "for the tab"]]')
    s1.select()
    am.window.terminal.submit("/clear")
    m = am.profile.hooks.wait(rf"\[nav\] tab-swap {s1.short} -> ([0-9a-f]{{8}})", since=marks.hooks, timeout_s=90)
    new_key = m.group(1)
    am.window.side_panel.wait_card(s1.title, timeout_s=30)
    pane = am.window.queue.open(s1.title)  # the tab's card is the NEW conversation's now
    assert not pane.upcoming(), pane.list_texts()  # TODAY: the queued prompt stayed behind
    assert pane.mode() == "Auto", pane.mode()  # TODAY: the new conversation took the cog's default, not Paused
    _assert_no_turn(am, "kept [[", marks.seq, HOLD_CHECK_S, "its conversation was cleared")  # TODAY: never sent
    am.window.tabs.wait_tab(s1.title)
    assert am.profile.session(new_key) is not None

