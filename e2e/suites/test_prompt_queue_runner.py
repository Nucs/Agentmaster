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
- WHY it waited: `[advance-skip] <id> (<reason>)` (DecideAdvance's fixed reasons, logged once per stall) — only when
  the runner is ASKED: nothing asks it while a session runs, needs approval, reads Error or Done, or is Paused
  (Scheduler::_wantsIdleStart; run pqr1), so a hold is judged by the mock's silence, never by a reason that may not
  come;
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

The owner's rulings of 2026-10-26: the runner's mode is per session and persisted — a reopened or resumed session
keeps it, an interrupt hold included (PQR-K8, fixed in code, NOT BUILT: its tests are `needs_fix` and fail on 0.8.0,
which re-stamps the cog's default); Restart session starting no settle window (PQR-K6) and `/clear` leaving the queue
with the old conversation (PQR-K7) are accepted behavior, so their tests prove the behavior.

Each flow's edge cases follow its main tests (added 2026-10-26): the text a prompt carries (an invisible character,
Hebrew and an emoji, line breaks, 2,000 characters), the queue edited mid-drain, the send budget, the global pause's
corners, Send now's targets (a selected row, a parked tab), the interrupt hold's corners (Paused already, a second
Esc, Send now, noise, an error after the resume), the question guard's sentence rule, a window-restored background
tab, retried and cut streams, Claude Code's "Switch model?" dialog for a queued `/model` (a queued `/add-dir`'s dialog
is left out while PQR-K12 is intermittent), the compose box's keys (history, the pop) and row menu, typed rows, a crash
and a quit mid-send, two windows, and an adopted session's send budget (PQR-K5, a known defect).
"""
from __future__ import annotations

import os
import time
from dataclasses import dataclass
from pathlib import Path

import psutil
import pytest

from amdriver import inputs, win32
from amdriver.session import Session
from amdriver.terminal import parse_input_box
from amdriver.timeline import StateTimeline, engine_story
from amdriver.ui.dialogs import Dialog
from amdriver.ui.queue import global_paused, set_global_pause
from amdriver.waits import WaitTimeout, wait_until

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
ZWSP = chr(0x200B)
"""A zero-width space: an "invisible" character Claude Code (2.1.277 and later) cancels an Enter on, so every paste is
cleaned of it first (ClaudeInvisibleChars, DELIVERY.md §15). Built with chr() so the source shows what it is."""
WARNING_EMOJI = chr(0x26A0) + chr(0xFE0F)
"""The warning sign U+26A0 with its emoji presentation selector U+FE0F (VS16), which Claude Code's rule KEEPS after an
emoji (PQR-01 E2)."""
JSON_NEWLINE = chr(92) + "n"
"""A backslash and an n: the JSON escape of a line break inside a mock script's quoted text."""
TEAMMATE_PROMPT = '<teammate-message teammate_id="helper">status ping</teammate-message> [[mock reply "noted"]]'
"""A message shaped like an agent team's delivery: protocol noise to Agentmaster (TranscriptStore's IsNoiseUserPrompt,
the `<teammate-message` prefix) — never a typed row, never "the user's next message" (PQR-07 E5, PQR-14 E2)."""


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


def _wait_quiet(tl: StateTimeline, quiet_s: float, timeout_s: float = 150.0) -> None:
    """Wait until the session reads ready on the side panel and has not changed for a while — past the short running
    blip that follows almost every turn (~20 s after it: flows/03 "Measured") — so a settle window measured from now
    has no status change inside it.

    Args:
        tl: The started timeline.
        quiet_s: How long the ready state must have held.
        timeout_s: How long the wait may take.
    Returns:
        None.
    Raises:
        WaitTimeout: When the session never held still that long (the message lists the timeline).
    """
    def quiet():
        """One look.

        Returns:
            True once the last change is a ready state at least quiet_s old, else None.
        """
        ch = tl.changes()
        if not ch or ch[-1].state not in READY:
            return None
        return True if tl.elapsed() - ch[-1].t >= quiet_s else None

    wait_until(quiet, timeout_s, interval_s=0.2, what=f"{tl.title!r} to hold still for {quiet_s:.0f} s",
               describe=tl.describe)


def _hold_by_interrupt(am, s, marks: Marks, first: str = "p1", second: str = "p2") -> None:
    """Park a session's runner the way PQR-07 does (the interrupt hold): two prompts queued, the first one's turn held
    at a gate, Esc pressed into that running turn, the hold logged, the gate released — and the toggle reading the hold.

    Args:
        am: The app.
        s: The Session (on Auto).
        marks: Marks taken before the prompts were queued.
        first: The first prompt's word (its turn is the one interrupted).
        second: The second prompt's word (it waits behind the hold).
    Returns:
        None.
    Raises:
        WaitTimeout / MockError: When the turn did not start, the hold was not taken or the toggle did not show it.
    """
    pane = am.window.queue.open(s.title)
    pane.queue(f'{first} [[mock say "working"; hold g1; reply "one"]]', f'{second} [[mock reply "two"]]')
    s.select()
    _turn_at(am, f"{first} [[", marks.seq)
    am.mock.wait_holding("g1", timeout_s=60)
    am.window.side_panel.wait_card(s.title, state="running", timeout_s=STATE_WAIT_S)
    _press(am, "escape")
    am.profile.autorunner.wait(rf"\[interrupt-hold\] {s.short} autorunner Full -> Off", since=marks.autorunner,
                               timeout_s=30)
    am.mock.release("g1")
    am.window.queue.open(s.title).wait_mode("Paused → Auto on your next message")


def _let_the_window_record_save(am, mark: int) -> None:
    """Let the window record's autosave that a tab change scheduled land before the app quits. On 0.8.0 an autosave
    still pending at a window's close fires AFTER the close archived the sessions and saves the Claude tabs as shell
    tabs (TAB-K1, fixed in code 2026-10-23, not in 0.8.0; seen in run pqr5: `[window-save] … [sh,sh,sh,]` 300 ms after
    the `[teardown-archive]` lines), so the reopen would bring back shells.

    Args:
        am: The app.
        mark: A hooks.log mark from before the tab change.
    Returns:
        None (also when no save comes: then none was pending).
    """
    try:
        am.profile.hooks.wait(r"\[window-save\] ", since=mark, timeout_s=8)
    except WaitTimeout:
        pass  # the change scheduled no save, so none is pending at the quit either


def _board_global(page) -> None:
    """Make a window's Triage Board show every window's sessions (its LOCAL / GLOBAL toggle on GLOBAL), so a session
    another window hosts has a card there.

    Args:
        page: The window's ManagerPage.
    Returns:
        None.
    Raises:
        WaitTimeout: When the toggle did not change.
    """
    page.open()
    if page.board_scope() != "GLOBAL":
        page.toggle_board_scope()
    wait_until(lambda: page.board_scope() == "GLOBAL", 10, what="the board's GLOBAL scope")


def _close_window(am, w) -> None:
    """Close one window the way a user does: its ✕, then "Close this window?" → Close Window; wait until it is gone.

    Args:
        am: The app.
        w: The MainWindow.
    Returns:
        None.
    Raises:
        WaitTimeout: When the dialog did not ask or the window stayed.
    """
    win32.set_foreground(w.hwnd)
    win32.post_close(w.hwnd)
    # The window's going is the condition (as in App.close): the dialog's own UI Automation calls can fail while its
    # window tears down.
    Dialog.wait(am, title="Close this window?", timeout_s=10).press("Close Window", wait_close=False)
    wait_until(lambda: w.hwnd not in am.main_hwnds(), 20, what="the window to close")


def _adopt_typed_claude(am, name: str, mark_seq: int) -> Session:
    """Start a claude BY HAND in a new shell tab (LIF-05's recipe) and wait until Agentmaster adopted it: a new tab
    (Ctrl+Shift+T), the test's CLAUDE_CONFIG_DIR set in its shell (a `+` tab's shell does not carry the app's
    per-test environment), claude.exe started in a folder nobody trusted, "Yes, I trust this folder" picked (the
    question's first option is "No, exit"), a first prompt typed — then `[adopt] <id> bound via WT_SESSION …`.

    Args:
        am: The app.
        name: The folder's name (the adopted session's title).
        mark_seq: A mock seq mark from before the call.
    Returns:
        The adopted Session (its tab and card show).
    Raises:
        WaitTimeout / MockError: When a step did not happen.
    """
    w = am.window
    before = len(w.tabs.titles())
    w.focus()
    inputs.keys("ctrl+shift+t")
    wait_until(lambda: len(w.tabs.titles()) > before, 20, what="a new shell tab")
    w.terminal.wait_text(r"PS [A-Z]:\\[^>]*>", timeout_s=60)
    folder = am.iso.folder(name)
    exe = Path(os.environ.get("USERPROFILE", r"C:\Users\Docker")) / ".local" / "bin" / "claude.exe"
    w.terminal.focus()
    w.terminal.type(f"$env:CLAUDE_CONFIG_DIR='{am.claude.dir}'; Set-Location '{folder}'; & '{exe}'")
    mark = am.profile.hooks.mark()  # before claude starts: the observer binds it before its first prompt
    inputs.keys("enter")
    w.terminal.wait_text(r"Yes, I trust this folder", timeout_s=90)
    inputs.keys("down")
    w.terminal.wait_text(r"❯ Yes, I trust this folder", timeout_s=10)
    inputs.keys("enter")
    w.terminal.wait_input_box(timeout_s=60)
    w.terminal.submit(f'hello {name} [[mock reply "hi from the shell tab"]]')
    e = am.mock.wait_prompt(f"hello {name} [[", since=mark_seq, timeout_s=90)
    sid = e.get("session", "")
    assert sid, e
    am.profile.hooks.wait(rf"\[adopt\] {sid} bound via WT_SESSION ", since=mark, timeout_s=90)
    w.tabs.wait_tab(name, timeout_s=30)
    w.side_panel.wait_card(name, timeout_s=60)
    return Session(am, sid, folder, am.mock)


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


def _assert_delivered_once_as_queued(am, s, marks: Marks, sent: str, label_start: str) -> None:
    """The checks every PQR-01 text edge case shares, once its turn reached the model: the prompt was recorded once —
    its echo consumed (`[ups] … echo consumed`), no extra "typed" row, the history one row longer — it is in the
    transcript once as `sent`, no lost-send verdict followed it, and the runner is still on Auto.

    Args:
        am: The app.
        s: The Session (F-S: one typed row before the test).
        marks: Marks from before the prompt was queued.
        sent: The text Claude Code received (the transcript's prompt).
        label_start: The start of the prompt's row label in SENT.
    Returns:
        None.
    Raises:
        AssertionError / WaitTimeout: When a check fails.
    """
    am.profile.hooks.wait(rf"\[ups\] {s.short} chars=\d+ hash=\S+ -> echo consumed", since=marks.hooks, timeout_s=60)
    s.select()  # the side panel shows beside the terminal (read the state from the UI, never by polling sessions.json)
    am.window.side_panel.wait_card(s.title, state="waiting-for-you", timeout_s=STATE_WAIT_S)
    time.sleep(20)  # a lost-send verdict comes 15 s at rest after a send whose echo never came
    assert not am.profile.hooks.find(rf"\[lost-send\] {s.short}", since=marks.hooks), _why(am, s, marks)
    assert am.claude.user_prompts(s.id).count(sent) == 1, am.claude.user_prompts(s.id)[-3:]
    pane = am.window.queue.open(s.title)
    row = pane.wait_row(label_start, "sent")
    assert row.tag == "auto" and row.status == "sent", row.texts
    assert pane.counts() == (2, 1, 0), pane.list_texts()  # one more sent row, still one typed
    assert pane.mode() == "Auto", pane.mode()


@pytest.mark.p1
@pytest.mark.flow("PQR-01")
def test_an_invisible_character_is_cleaned_and_the_prompt_still_goes_once(am, s1):
    """PQR-01 E1: a queued prompt holding a zero-width space — a character Claude Code (2.1.277 and later) cancels an
    Enter on — is cleaned with Claude Code's own rule before it is pasted (`[delivered] … sanitized=<n>-><n-1>`), so
    the send is not cancelled: the model gets the cleaned text, the echo is matched to the queued prompt (the compare
    folds the character out), and no lost-send verdict follows.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    queued = f'zw{ZWSP} [[mock reply "clean"]]'
    clean = 'zw [[mock reply "clean"]]'
    am.window.queue.open(s1.title).queue(queued)
    e = am.mock.wait_prompt("zw", since=marks.seq, timeout_s=RUNNER_WAIT_S)
    assert e["user_text"] == clean, ascii(e["user_text"])
    m = am.profile.hooks.wait(rf"\[delivered\] {s1.short} prompt [0-9a-f]{{8}} \(chars=\d+, verified=\w+.*"
                              rf"sanitized=(\d+)->(\d+)\)", since=marks.hooks, timeout_s=30)
    assert int(m.group(1)) - int(m.group(2)) == 1, m.group(0)
    _assert_delivered_once_as_queued(am, s1, marks, clean, "zw")


@pytest.mark.p1
@pytest.mark.flow("PQR-01")
def test_an_emoji_prompt_arrives_intact_and_is_recorded_once(am, s1):
    """PQR-01 E2 (the emoji half; the Hebrew half is PQR-K9): a queued prompt with an emoji — the warning sign U+26A0
    and its presentation selector U+FE0F, which Claude Code's rule keeps — reaches the model exactly as written,
    nothing sanitized, and is recorded once: one SENT "auto" row, no extra "typed" row from its echo.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    prompt = f'emoji {WARNING_EMOJI} ok [[mock reply "ok"]]'
    am.window.queue.open(s1.title).queue(prompt)
    e = am.mock.wait_prompt("emoji ", since=marks.seq, timeout_s=RUNNER_WAIT_S)
    assert e["user_text"] == prompt, ascii(e["user_text"])
    m = am.profile.hooks.wait(rf"\[delivered\] {s1.short} prompt [0-9a-f]{{8}} \((.*)\)", since=marks.hooks,
                              timeout_s=30)
    assert "sanitized=" not in m.group(1), m.group(0)
    _assert_delivered_once_as_queued(am, s1, marks, prompt, "emoji ")


@pytest.mark.p1
@pytest.mark.flow("PQR-01")
@pytest.mark.flow("PQR-14")
def test_a_multi_line_prompt_goes_as_one_message_and_is_recorded_once(am, s1):
    """PQR-01 E3 and PQR-14 phase 2: a prompt composed on three lines (the compose box keeps its line breaks as CR) is
    pasted as ONE message with its line breaks (Claude Code gets LF), recorded as one "auto" row — its echo, which
    carries LF, is matched to the CR-composed prompt, so no extra "typed" row appears.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    composed = 'ml one\rml two\rml three [[mock reply "m"]]'
    received = 'ml one\nml two\nml three [[mock reply "m"]]'
    am.window.queue.open(s1.title).queue(composed)
    e = am.mock.wait_prompt("ml one", since=marks.seq, timeout_s=RUNNER_WAIT_S)
    assert e["user_text"] == received, ascii(e["user_text"])
    time.sleep(3)  # a second message (a line submitted on its own) would have reached the mock by now
    assert len(_turns(am, "ml ", marks.seq)) == 1, [t["user_text"] for t in _turns(am, "ml ", marks.seq)]
    _assert_delivered_once_as_queued(am, s1, marks, received, "ml one ml two")


@pytest.mark.p1
@pytest.mark.flow("PQR-01")
def test_a_long_multi_line_prompt_goes_whole_as_one_message(am, s1):
    """PQR-01 E4 (the multi-line half; one long LINE is PQR-K11): a 2,000-character prompt on 40 lines is one message —
    Claude Code shows the paste collapsed as "[Pasted text #1 +N lines]", which the delivery verifies as its own
    collapsed render (`verified=collapsed`), and expands it on the submit — so the model and the transcript get the
    whole text, once.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    lines = [f"long line {i:02d} " + "x" * 36 for i in range(1, 41)]  # 40 lines of 49 characters
    composed = "\r".join(lines) + ' [[mock reply "long ok"]]'
    received = composed.replace("\r", "\n")
    am.window.queue.open(s1.title).queue(composed)
    e = am.mock.wait_prompt("long line 01", since=marks.seq, timeout_s=RUNNER_WAIT_S)
    assert e["user_text"] == received, f"{len(e['user_text'])} characters arrived of {len(received)}"
    am.profile.hooks.wait(rf"\[delivered\] {s1.short} prompt [0-9a-f]{{8}} \(chars=\d+, verified=collapsed",
                          since=marks.hooks, timeout_s=30)
    _assert_delivered_once_as_queued(am, s1, marks, received, "long line 01")


@pytest.mark.p2
@pytest.mark.flow("PQR-01")
def test_add_to_queue_with_an_empty_box_queues_nothing(am, s1):
    """PQR-01 E5: Add to queue with nothing in the compose box queues nothing (no `[nav] queue` line, no UPCOMING row,
    no ⏳ on the badge).

    Args:
        am: The app.
        s1: F-S.
    """
    pane = am.window.queue.open(s1.title)
    page = am.window.manager
    mark = am.profile.hooks.mark()
    page.compose_box().set_value("")
    buttons = page.compose_buttons()
    assert len(buttons) == 3, [b.line() for b in buttons]
    buttons[2].click()  # the envelope
    time.sleep(2)  # a queued prompt would have logged its line by now
    assert not am.profile.hooks.find(r"\[nav\] queue ", since=mark)
    assert pane.counts() == (1, 1, 0), pane.list_texts()
    s1.select()
    assert am.window.badge.queued() == 0, am.window.badge.texts()


@pytest.mark.p1
@pytest.mark.flow("PQR-01")
@pytest.mark.slow
def test_a_prompt_queued_while_a_turn_runs_waits_for_its_end_then_the_settle(am, s1):
    """PQR-01 E6: a prompt queued while a typed turn runs is not sent into it: it goes only after the turn ended AND
    the status then held still for a whole settle window (counted from the session's last status change, which is
    after the prompt's queue time).

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        s1.ask('busy [[mock say "working"; hold g; reply "busy done"]]', wait="sent")
        am.mock.wait_holding("g", timeout_s=60)
        tl.wait("running", STATE_WAIT_S)
        t_queue = am.window.queue.open(s1.title).queue('after [[mock reply "after the turn"]]')
        _assert_no_turn(am, "after [[", marks.seq, 20, "the typed turn still runs")
        assert tl.current() == "running", tl.describe()
        n = clock.mark()
        am.mock.release("g")
        t_end = clock.entered("waiting-for-you", n)
        _turn_at(am, "after [[", marks.seq)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    assert sent_at > t_end, f"sent {t_end - sent_at:.1f} s before the turn ended: {tl.describe()}"
    clock.assert_settled_send(sent_at, floor=t_queue, what="after")
    assert len(_turns(am, "after [[", marks.seq)) == 1


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


@pytest.mark.p1
@pytest.mark.flow("PQR-02")
@pytest.mark.slow
def test_the_next_prompt_waits_for_the_whole_tool_turn(am, s1):
    """PQR-02 E1: a queued turn that runs tools (a Bash call, then a Write, then the reply: three model responses) is
    ONE turn to the runner: the next prompt goes only after the turn's last response, a settle window after the
    session's last status change — never after the first response.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    target = str(s1.folder / "a.txt").replace(chr(92), "/")
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue(f'tools [[mock bash "echo a"; write {target} "x"; reply "tools done"]]',
                   'after [[mock reply "after the tools"]]')
        first = am.mock.wait_prompt("tools [[", since=marks.seq, timeout_s=RUNNER_WAIT_S)
        last = am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("turn") == first.get("turn")
                                and e.get("k") == 2, since=marks.seq, timeout_s=STATE_WAIT_S,
                                what="the tool turn's third response (the reply)")
        t_after = _turn_at(am, "after [[", marks.seq)
    assert t_after > last["t"], "the next prompt reached the model before the tool turn's last response"
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2 and sends[1] > last["t"], _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="after")
    assert (s1.folder / "a.txt").read_text(encoding="utf-8") == "x"


@pytest.mark.p1
@pytest.mark.flow("PQR-02")
@pytest.mark.flow("PQR-04")
@pytest.mark.am_settings(maxAutoSends=2)
@pytest.mark.slow
def test_the_send_budget_holds_the_queue_and_a_re_arm_resets_it(am, s1):
    """PQR-02 E3 and PQR-04 E3: with the cog's maxAutoSends at 2 for new sessions, the runner sends two queued prompts,
    one per turn, and holds the third — "maxAutoSends reached", past a whole settle window, the session still on Auto
    (the budget is a backstop, not a pause). A re-arm (the toggle: Paused, then Auto) resets the budget, and the third
    goes within one settle window.

    Args:
        am: The app (settings.json says maxAutoSends 2 before the start).
        s1: F-S, launched with that budget.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('b1 [[mock reply "one"]]', 'b2 [[mock reply "two"]]', 'b3 [[mock reply "three"]]')
    _turn_at(am, "b1 [[", marks.seq)
    _turn_at(am, "b2 [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    _wait_skip(am, s1, "maxAutoSends reached", marks.autorunner, timeout_s=STATE_WAIT_S)
    _assert_no_turn(am, "b3 [[", marks.seq, HOLD_CHECK_S, "the session spent its send budget")
    assert len(_sends(am, s1, marks.autorunner)) == 2, _why(am, s1, marks)
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Auto", pane.mode()
    assert pane.click_toggle() == "Paused"
    t_arm = time.time()
    assert pane.click_toggle() == "Auto"
    _turn_at(am, "b3 [[", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    sent_at = _nth_send(am, s1, marks.autorunner, 3)
    assert 0 <= sent_at - t_arm <= RELEASE_S, f"b3 {sent_at - t_arm:.1f} s after the re-arm"


@pytest.mark.p1
@pytest.mark.flow("PQR-02")
@pytest.mark.slow
def test_a_prompt_moved_up_while_a_turn_runs_goes_next(am, s1):
    """PQR-02 E4: three queued prompts; while the first one's turn runs, the third is moved up (its row's Move up):
    it goes next, before the second — the send resolves its target by the prompt's id, never a stale position.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('first [[mock say "working"; hold g0; reply "r0"]]', 'second [[mock reply "r1"]]',
               'third [[mock reply "r2"]]')
    _turn_at(am, "first [[", marks.seq)
    am.mock.wait_holding("g0", timeout_s=60)
    pane = am.window.queue.open(s1.title)
    pane.row_menu(pane.row("third", "upcoming")).choose("Move up")
    wait_until(lambda: [r.label.split()[0] for r in pane.upcoming()] == ["third", "second"], 10,
               what="third above second", describe=pane.list_texts)
    am.mock.release("g0")
    _turn_at(am, "third [[", marks.seq)
    _turn_at(am, "second [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    order = [e["user_text"].split()[0] for e in am.mock.prompts(since=marks.seq)
             if e["user_text"].split()[:1] in (["first"], ["second"], ["third"])]
    assert order == ["first", "third", "second"], order


@pytest.mark.p1
@pytest.mark.flow("PQR-02")
@pytest.mark.slow
def test_a_prompt_deleted_while_a_turn_runs_is_never_sent(am, s1):
    """PQR-02 E5: three queued prompts; while the first one's turn runs, the second is deleted (its row's Delete): the
    third goes next, the deleted one never, and the queue then reads drained.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('first [[mock say "working"; hold g0; reply "r0"]]', 'second [[mock reply "r1"]]',
               'third [[mock reply "r2"]]')
    _turn_at(am, "first [[", marks.seq)
    am.mock.wait_holding("g0", timeout_s=60)
    pane = am.window.queue.open(s1.title)
    pane.row_menu(pane.row("second", "upcoming")).choose("Delete")
    wait_until(lambda: [r.label.split()[0] for r in pane.upcoming()] == ["third"], 10,
               what="second gone from UPCOMING", describe=pane.list_texts)
    am.mock.release("g0")
    _turn_at(am, "third [[", marks.seq)
    am.profile.autorunner.wait(rf"\[plan-done\] {s1.id}", since=marks.autorunner, timeout_s=STATE_WAIT_S)
    assert not _turns(am, "second [[", marks.seq), "the deleted prompt was sent"
    assert len(_sends(am, s1, marks.autorunner)) == 2, _why(am, s1, marks)


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


@pytest.mark.p2
@pytest.mark.flow("PQR-03")
def test_a_prompt_queued_at_rest_is_decided_forty_seconds_after_its_queue_time(am, s1):
    """PQR-03 E4 (the realtime lane's check, at the production value): a prompt queued into a session that has held
    still for a while is decided 40 s after its queue time — the window's own timer, not a later trigger: the engine's
    `[nav] queue` stamp and the runner's `[send]` stamp, both written by the app's clock, are 40 s apart within a
    scheduler tick and a loaded desktop's slack (−0.5 s / +6 s).

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        _wait_quiet(tl, quiet_s=SETTLE_S)  # past the fixture turn's late running blip: no status change ahead
        pane.queue('exact [[mock reply "on time"]]')
        _turn_at(am, "exact [[", marks.seq)
    (queued_at, _m), = am.profile.hooks.stamped(rf'\[nav\] queue {s1.short} "exact ', since=marks.hooks)
    sent_at = _nth_send(am, s1, marks.autorunner, 1)
    changed_at, _state = clock.last_change_before(sent_at)
    assert changed_at <= queued_at.timestamp(), f"the status changed after the queue time: {tl.describe()}"
    gap = sent_at - queued_at.timestamp()
    assert SETTLE_S - 0.5 <= gap <= SETTLE_S + 6, f"decided {gap:.2f} s after the queue time; {_why(am, s1, marks, tl)}"


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


@pytest.mark.p2
@pytest.mark.flow("PQR-04")
@pytest.mark.am_settings(defaultAutorunnerMode="SemiAuto")
def test_a_stored_semiauto_default_mode_loads_as_paused(am, s1):
    """PQR-04 E2: a settings.json from a build that still had the SemiAuto mode (removed on 2026-10-09) says
    `defaultAutorunnerMode: SemiAuto`: it loads as Paused — the safe reading (a mode that confirmed every send must
    never become one that sends by itself) — so a new session starts Paused and its queue waits.

    Args:
        am: The app (settings.json says defaultAutorunnerMode SemiAuto before the start).
        s1: F-S, launched with that default.
    """
    marks = _marks(am)
    am.profile.hooks.wait(r"\[engine\] prompt queue runner started .*default mode for new sessions: Off", timeout_s=10)
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Paused", pane.mode()
    pane.queue('semi [[mock reply "s"]]')
    s1.select()
    am.window.badge.wait_mode("Paused")
    _assert_no_turn(am, "semi [[", marks.seq, HOLD_CHECK_S, "a stored SemiAuto loads as Paused")
    assert not _sends(am, s1, marks.autorunner), _why(am, s1, marks)


@pytest.mark.p1
@pytest.mark.flow("PQR-04")
def test_a_mode_switched_from_another_window_shows_on_the_hosting_windows_badge(am, s2):
    """PQR-04 E4: a session hosted in window 2 is switched from window 1's Agent Manager (its board on GLOBAL): the
    hosting window's badge follows at once ("Paused"); switched back from that badge, window 1's toggle reads Auto —
    one mode, whichever window sets it.

    Args:
        am: The app.
        s2: F-S2 (t2 is moved into a window of its own).
    """
    a, b = s2
    marks = _marks(am)
    w1 = am.window
    w2 = w1.tabs.move_to_new_window(b.title)
    _board_global(w1.manager)
    pane = w1.queue.open(b.title)
    assert pane.mode() == "Auto", pane.mode()
    assert pane.click_toggle() == "Paused"
    am.profile.hooks.wait(rf"\[nav\] autorunner {b.short} -> Off$", since=marks.hooks, timeout_s=10)
    w2.focus()
    w2.badge.wait_mode("Paused")
    assert w2.badge.click_mode() == "Auto"
    am.profile.hooks.wait(rf"\[nav\] autorunner {b.short} -> Full \(overlay\)", since=marks.hooks, timeout_s=10)
    wait_until(lambda: w1.queue.open(b.title).mode() == "Auto", 15, what="window 1's toggle to read Auto")


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


@pytest.mark.p0
@pytest.mark.flow("PQR-05")
@pytest.mark.flow("PQR-06")
def test_send_now_still_works_during_the_global_pause(am, s1):
    """PQR-05 phase 4 and PQR-06 E3: the global pause holds only the AUTOMATIC sends: Send now types a composed prompt
    at once (no runner decision), and the pause stays on — the toolbar keeps reading Resume, the badge
    "Auto (Off globally)".

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    set_global_pause(am.window.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    pane = am.window.queue.open(s1.title)
    t_send = time.time()
    pane.send_now('now [[mock reply "despite the pause"]]').press("Send")
    t_sent = _turn_at(am, "now [[", marks.seq, timeout_s=ARRIVE_S)
    assert t_sent - t_send <= ARRIVE_S - 20, f"Send now took {t_sent - t_send:.1f} s to reach the model"
    assert not _sends(am, s1, marks.autorunner), "Send now went through the runner"
    pane = am.window.queue.open(s1.title)
    assert pane.wait_row("now", "sent").tag == "auto"
    assert global_paused(am.window.manager.open()), "Send now lifted the global pause"
    s1.select()
    am.window.badge.wait_mode("Auto (Off globally)")


@pytest.mark.p2
@pytest.mark.flow("PQR-05")
def test_a_window_opened_during_the_global_pause_shows_it_at_once(am, s1):
    """PQR-05 E1: the pause is one process-wide state: a window opened while it is on reads "Resume Prompt Queue
    Runner" on its own toolbar at once.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    set_global_pause(am.window.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    w2 = am.new_window()
    assert global_paused(w2.manager.open()), "the new window's toolbar reads Pause during the global pause"


@pytest.mark.p1
@pytest.mark.flow("PQR-05")
@pytest.mark.slow
def test_a_mode_chosen_during_the_global_pause_is_kept_after_the_resume(am, s2):
    """PQR-05 E3: during the global pause t1 is set Paused — its badge reads "Paused (Off globally)"; after the Resume,
    t2 (still Auto) sends its queued prompt within one settle window, and t1 keeps its own Paused: its prompt waits past
    a whole window.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    marks = _marks(am)
    set_global_pause(am.window.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    am.window.queue.open(b.title).queue('kb [[mock reply "b"]]')
    pane_a = am.window.queue.open(a.title)
    pane_a.queue('ka [[mock reply "a"]]')
    assert pane_a.click_toggle() == "Paused"
    a.select()
    am.window.badge.wait_mode("Paused (Off globally)")
    t_resume = time.time()
    set_global_pause(am.window.manager, False)
    _turn_at(am, "kb [[", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    sent_b = _nth_send(am, b, marks.autorunner, 1)
    assert 0 <= sent_b - t_resume <= RELEASE_S, f"t2 sent {sent_b - t_resume:.1f} s after the resume"
    _assert_no_turn(am, "ka [[", marks.seq, HOLD_CHECK_S, "t1 was set Paused during the global pause")
    assert not _sends(am, a, marks.autorunner), _why(am, a, marks)
    assert am.window.queue.open(a.title).mode() == "Paused"


@pytest.mark.p2
@pytest.mark.flow("PQR-05")
def test_an_interrupt_hold_during_the_global_pause_shows_on_the_badge(am, s1):
    """PQR-05 E4: Esc during a typed turn while the global pause is on still parks the session's runner: the badge
    reads "Paused → Auto (Off globally)" — the session's own state first, the pause after it — and, once the pause is
    lifted, "Paused → Auto".

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    set_global_pause(am.window.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    s1.ask('busy [[mock say "working"; hold g; reply "x"]]', wait="sent")
    am.mock.wait_holding("g", timeout_s=60)
    am.window.side_panel.wait_card(s1.title, state="running", timeout_s=STATE_WAIT_S)
    _press(am, "escape")
    am.profile.autorunner.wait(rf"\[interrupt-hold\] {s1.short} autorunner Full -> Off", since=marks.autorunner,
                               timeout_s=30)
    am.mock.release("g")
    s1.select()
    am.window.badge.wait_mode("Paused → Auto (Off globally)")
    set_global_pause(am.window.manager, False)
    s1.select()
    am.window.badge.wait_mode("Paused → Auto")


@pytest.mark.p2
@pytest.mark.flow("PQR-05")
@pytest.mark.slow
def test_a_question_hold_stays_visible_through_the_global_pause(am, s1):
    """PQR-05 E5: a turn that ended with a question holds the queue (the question guard), and the badge says so —
    "❓ held — answer the question" — during the global pause too (the pause hides only the "settling" cue); after the
    Resume the question still holds: "question pending …", nothing sent past a whole window.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    s1.ask('q [[mock reply "Shall I go on?"]]')
    set_global_pause(am.window.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    am.window.queue.open(s1.title).queue('next [[mock reply "n"]]')
    s1.select()
    badge = am.window.badge
    wait_until(lambda: badge.cue() == "held", 20, what="the badge's question cue", describe=badge.texts)
    assert badge.mode() == "Auto (Off globally)", badge.texts()
    set_global_pause(am.window.manager, False)
    _wait_skip(am, s1, "question pending", marks.autorunner, timeout_s=60)
    s1.select()
    wait_until(lambda: badge.cue() == "held" and badge.mode() == "Auto", 20, what="the cue after the resume",
               describe=badge.texts)
    _assert_no_turn(am, "next [[", marks.seq, HOLD_CHECK_S, "the last turn ended with a question")


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


@pytest.mark.p0
@pytest.mark.flow("PQR-06")
def test_send_now_sends_the_selected_queued_row_at_once(am, s1):
    """PQR-06 phase 4: with an empty compose box, Send now sends the SELECTED queued row — the confirmation quotes it,
    Send types it at once into the Paused session — and that row moves to SENT while the other one stays queued.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    pane.queue('r1 [[mock reply "row one"]]', 'r2 [[mock reply "row two"]]')
    pane.row("r2", "upcoming").click()
    dlg = pane.send_now()  # the box is empty: the selected row is the one sent
    assert dlg.text().endswith("“r2 [[mock reply \"row two\"]]”"), dlg.text()
    dlg.press("Send")
    _turn_at(am, "r2 [[", marks.seq, timeout_s=ARRIVE_S)
    pane = am.window.queue.open(s1.title)
    pane.wait_row("r2", "sent")
    assert [r.label[:2] for r in pane.upcoming()] == ["r1"], pane.list_texts()
    time.sleep(3)  # a second delivery would have reached the mock by now
    assert len(_turns(am, "r2 [[", marks.seq)) == 1 and not _turns(am, "r1 [[", marks.seq)


@pytest.mark.p2
@pytest.mark.flow("PQR-06")
def test_send_now_with_nothing_to_send_asks_nothing(am, s1):
    """PQR-06 phase 5: Send now with an empty compose box and nothing queued (no row selected) does nothing — no
    confirmation, nothing typed.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    am.window.queue.open(s1.title)
    page = am.window.manager
    page.compose_box().set_value("")
    buttons = page.compose_buttons()
    assert len(buttons) == 3, [b.line() for b in buttons]
    buttons[1].click()  # "!"
    time.sleep(2)  # a confirmation would be on screen by now
    assert Dialog.find(am, title="Send now?") is None, "Send now asked with nothing to send"
    assert not am.profile.hooks.find(rf"\[nav\] send-now {s1.short}", since=marks.hooks)


@pytest.mark.p0
@pytest.mark.flow("PQR-06")
@pytest.mark.flow("PQR-10")
@pytest.mark.slow
def test_send_now_into_a_parked_tab_types_nothing_and_the_prompt_goes_once_it_is_woken(am, s2):
    """PQR-06 E1: Send now into a parked (Deactivated) tab types nothing — its claude does not run, and a write there
    would be lost — and puts the prompt back to Pending (`[send-verify] … NOT sent: the tab is dormant …`), so it
    is not lost: once the tab is woken (Activate Tab) the runner sends it by itself, after the started session settled.

    Args:
        am: The app.
        s2: F-S2 (t2 is where the user is while t1 is parked).
    """
    a, b = s2
    marks = _marks(am)
    b.select()
    mark = am.profile.hooks.mark()
    am.window.tabs.deactivate(a.title)
    a.wait_parked(since=mark)
    pane = am.window.queue.open(a.title)
    pane.send_now('wake [[mock reply "after the wake"]]').press("Send")
    am.profile.hooks.wait(rf"\[send-verify\] {a.short} prompt [0-9a-f]{{8}} NOT sent: the tab is dormant",
                          since=marks.hooks, timeout_s=30)
    pane = am.window.queue.open(a.title)
    pane.wait_row("wake", "upcoming")  # back to Pending, not lost
    _assert_no_turn(am, "wake [[", marks.seq, 10, "the tab is parked")
    assert not a.claude_pids()
    t_wake = time.time()
    am.window.tabs.activate(a.title)
    a.wait_running(timeout_s=60)
    _turn_at(am, "wake [[", marks.seq)
    sent_at = _nth_send(am, a, marks.autorunner, 1)
    assert SETTLE_S - EARLY_S <= sent_at - t_wake, f"sent {sent_at - t_wake:.1f} s after the wake: before it settled"
    time.sleep(3)
    assert len(_turns(am, "wake [[", marks.seq)) == 1


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


@pytest.mark.p1
@pytest.mark.flow("PQR-07")
@pytest.mark.needs_fix("PQR-K8")
@pytest.mark.slow
def test_an_interrupt_hold_survives_a_restart(am, s1):
    """PQR-07 E9 (PQR-K8's fix, `needs_fix`): Esc parks the runner ("Paused → Auto on your next message"), the app quits
    and reopens, and the session comes back STILL held — the hold is part of the session's persisted mode (the owner's
    rule of 2026-10-26: "we need the mode per session persisted"; `sessions.json` keeps the held mode and the marker's
    stamp) — so nothing is sent by itself past a whole settle window, and the user's first message after the reopen
    resumes Auto: the queued prompt follows that message's turn.

    The quit itself saves the record (the window's close archives its sessions), so the hold is on disk before the
    restart. Claude Code counts an Esc-ed turn as finished on resume, so nothing continues the turn by itself. On 0.8.0
    the hold is transient and the reopen re-stamps the cog's default (Auto): the queued prompt goes by itself, and this
    test fails there by design.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    am.window.queue.open(s1.title).queue('p1 [[mock say "working"; hold g1; reply "one"]]', 'p2 [[mock reply "two"]]')
    s1.select()
    _turn_at(am, "p1", marks.seq)
    am.mock.wait_holding("g1", timeout_s=60)
    am.window.side_panel.wait_card(s1.title, state="running", timeout_s=STATE_WAIT_S)
    _press(am, "escape")
    am.profile.autorunner.wait(rf"\[interrupt-hold\] {s1.short} autorunner Full -> Off", since=marks.autorunner,
                               timeout_s=30)
    am.mock.release("g1")
    assert am.window.queue.open(s1.title).mode() == "Paused → Auto on your next message"
    am.restart()
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Paused → Auto on your next message", \
        f"{pane.mode()}: the interrupt hold did not survive the restart (PQR-K8)"
    s1.select()
    s1.wait_running(timeout_s=60)
    _assert_no_turn(am, "p2 [[", marks.seq, HOLD_CHECK_S, "the reopened session is still held by the interrupt")
    mark = am.profile.hooks.mark()
    s1.ask('go [[mock reply "ok"]]', wait="sent")
    am.profile.hooks.wait(rf"\[interrupt-resume\] {s1.short} autorunner Off -> Full \(your message arrived\)",
                          since=mark, timeout_s=60)
    _turn_at(am, "p2 [[", marks.seq)
    assert len(_turns(am, "p2 [[", marks.seq)) == 1
    assert am.window.queue.open(s1.title).mode() == "Auto"


def _wait_interrupt_marker(am, s, timeout_s: float = 30.0) -> None:
    """Wait until Claude Code wrote its interrupt marker ("[Request interrupted by user …]") into the transcript: the
    fact the runner's interrupt hold is taken from.

    Args:
        am: The app.
        s: The Session.
        timeout_s: How long.
    Returns:
        None.
    Raises:
        WaitTimeout: When no marker came.
    """
    wait_until(lambda: any(p.startswith("[Request interrupted by user") for p in am.claude.user_prompts(s.id)),
               timeout_s, what="the interrupt marker in the transcript",
               describe=lambda: am.claude.user_prompts(s.id)[-3:])


@pytest.mark.p1
@pytest.mark.flow("PQR-07")
def test_esc_on_a_paused_session_takes_no_interrupt_hold(am, s1):
    """PQR-07 E1: Esc interrupting a turn of a session whose runner is already Paused takes no hold (there is no mode
    to resume to: the user drives by hand) — no `[interrupt-hold]`, the toggle keeps reading "Paused" — and the user's
    next message leaves it Paused (no `[interrupt-resume]`), so the queued prompt keeps waiting.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    pane.queue('waits [[mock reply "w"]]')
    s1.ask('busy [[mock say "working"; hold g; reply "x"]]', wait="sent")
    am.mock.wait_holding("g", timeout_s=60)
    am.window.side_panel.wait_card(s1.title, state="running", timeout_s=STATE_WAIT_S)
    _press(am, "escape")
    _wait_interrupt_marker(am, s1)
    am.mock.release("g")
    time.sleep(8)  # the scanner reads the marker within a pass or two: a hold would be logged by now
    assert not am.profile.autorunner.find(rf"\[interrupt-hold\] {s1.short}", since=marks.autorunner)
    assert am.window.queue.open(s1.title).mode() == "Paused"
    s1.ask('go [[mock reply "ok"]]', wait="sent")
    time.sleep(5)  # the resume is decided at the message's UserPromptSubmit
    assert not am.profile.hooks.find(rf"\[interrupt-resume\] {s1.short}", since=marks.hooks)
    assert am.window.queue.open(s1.title).mode() == "Paused"
    _assert_no_turn(am, "waits [[", marks.seq, 15, "the session's runner is Paused")


@pytest.mark.p2
@pytest.mark.flow("PQR-07")
@pytest.mark.slow
def test_a_second_esc_during_the_interrupt_hold_changes_nothing(am, s1):
    """PQR-07 E2: a second Esc while the runner is held (at rest, on the empty input box) takes no second hold and
    changes nothing — the toggle keeps "Paused → Auto on your next message" — and the user's next message still resumes
    the ORIGINAL mode (Auto); the queued prompt follows that message's turn.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    _hold_by_interrupt(am, s1, marks)
    s1.select()
    am.window.terminal.wait_input_box(timeout_s=30)
    time.sleep(2)  # well apart from the first Esc: a quick double Esc on an empty box opens Claude Code's Rewind menu
    _press(am, "escape")
    time.sleep(6)  # a second hold, had one been taken, would be logged by now
    assert len(am.profile.autorunner.find(rf"\[interrupt-hold\] {s1.short}", since=marks.autorunner)) == 1
    assert am.window.queue.open(s1.title).mode() == "Paused → Auto on your next message"
    s1.ask('go [[mock reply "ok"]]', wait="sent")
    am.profile.hooks.wait(rf"\[interrupt-resume\] {s1.short} autorunner Off -> Full \(your message arrived\)",
                          since=marks.hooks, timeout_s=60)
    _turn_at(am, "p2 [[", marks.seq)
    assert am.window.queue.open(s1.title).mode() == "Auto"


@pytest.mark.p1
@pytest.mark.flow("PQR-07")
@pytest.mark.slow
def test_send_now_during_the_interrupt_hold_resumes_the_runner(am, s1):
    """PQR-07 E4: a Send now during the interrupt hold is the user's next message: its echo resumes Auto ("the Send-now
    echo"), and the queued prompt follows a settle window after that message's turn.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    _hold_by_interrupt(am, s1, marks)
    pane = am.window.queue.open(s1.title)
    pane.send_now('now [[mock reply "n"]]').press("Send")
    t_now = _turn_at(am, "now [[", marks.seq, timeout_s=ARRIVE_S)
    am.profile.hooks.wait(rf"\[interrupt-resume\] {s1.short} autorunner Off -> Full \(your message arrived - the "
                          rf"Send-now echo\)", since=marks.hooks, timeout_s=60)
    am.window.queue.open(s1.title).wait_mode("Auto")
    _turn_at(am, "p2 [[", marks.seq)
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2 and sends[1] > t_now, _why(am, s1, marks)


@pytest.mark.p2
@pytest.mark.flow("PQR-07")
@pytest.mark.flow("PQR-14")
@pytest.mark.slow
def test_a_teammate_message_does_not_resume_the_interrupt_hold(am, s1):
    """PQR-07 E5 and PQR-14 E2: a message shaped like an agent team's delivery (`<teammate-message …>`) is protocol
    noise, not the user talking: it is not recorded as a typed row (`[ups] … -> noise (not recorded)`) and does not
    resume the interrupt hold — the toggle keeps the hold and the queued prompt waits past a whole window.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    _hold_by_interrupt(am, s1, marks)
    s1.ask(TEAMMATE_PROMPT, wait="sent")
    am.profile.hooks.wait(rf"\[ups\] {s1.short} chars=\d+ hash=\S+ -> noise \(not recorded\)", since=marks.hooks,
                          timeout_s=60)
    time.sleep(3)
    assert not am.profile.hooks.find(rf"\[interrupt-resume\] {s1.short}", since=marks.hooks)
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Paused → Auto on your next message", pane.mode()
    assert not any(r.label.startswith("<teammate-message") for r in pane.sent()), pane.list_texts()
    _assert_no_turn(am, "p2 [[", marks.seq, HOLD_CHECK_S, "only protocol noise arrived since the interrupt")


@pytest.mark.p1
@pytest.mark.flow("PQR-07")
@pytest.mark.slow
def test_an_api_error_after_the_resume_leaves_the_runner_paused_for_good(am, s1):
    """PQR-07 E7: during the interrupt hold the user's next message resumes Auto — and its turn ends in an API error:
    stop-on-error pauses the runner, a backstop pause that is sticky (it clears the hold), so the user's FOLLOWING
    message resumes nothing and the queued prompt keeps waiting.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    _hold_by_interrupt(am, s1, marks)
    s1.ask('oops [[mock error 400 invalid_request_error "Output blocked by content filtering policy"]]', wait="sent")
    am.profile.hooks.wait(rf"\[interrupt-resume\] {s1.short} autorunner Off -> Full \(your message arrived\)",
                          since=marks.hooks, timeout_s=60)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner, timeout_s=STATE_WAIT_S)
    am.window.queue.open(s1.title).wait_mode("Paused")
    mark = am.profile.hooks.mark()
    s1.ask('retry [[mock reply "ok"]]', wait="sent")
    time.sleep(5)  # a resume is decided at the message's UserPromptSubmit
    assert not am.profile.hooks.find(rf"\[interrupt-resume\] {s1.short}", since=mark)
    assert am.window.queue.open(s1.title).mode() == "Paused"
    _assert_no_turn(am, "p2 [[", marks.seq, HOLD_CHECK_S, "stop-on-error paused the runner for good")


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


@pytest.mark.p2
@pytest.mark.flow("PQR-08")
@pytest.mark.slow
def test_a_question_with_trailing_whitespace_still_holds_the_queue(am, s1):
    """PQR-08 E1: a final answer that ends with a question mark followed by spaces and a line break is still a question:
    the next queued prompt is held ("question pending …") past a whole settle window.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue(f'q [[mock reply "Shall I continue?   {JSON_NEWLINE}"]]', 'next [[mock reply "n"]]')
    _turn_at(am, "q [[", marks.seq)
    _wait_skip(am, s1, "question pending", marks.autorunner, timeout_s=90)
    _assert_no_turn(am, "next [[", marks.seq, HOLD_CHECK_S, "the last turn ended with a question (and whitespace)")


@pytest.mark.p2
@pytest.mark.flow("PQR-08")
@pytest.mark.slow
def test_a_question_mark_that_does_not_end_the_answer_holds_nothing(am, s1):
    """PQR-08 E2 and E4: only the answer's LAST sentence counts. A "?" in the middle of the final text ("Is it done?
    Yes, it is done.") and a question in an EARLIER text block of the turn (before a tool call, the turn ending on a
    statement) hold nothing: the three queued prompts go one per turn, and the runner never says "question pending".

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('mid [[mock reply "Is it done? Yes, it is done."]]',
               'blocks [[mock say "Shall I start?"; bash "echo started"; reply "Done, it is finished."]]',
               'last [[mock reply "the end"]]')
    _turn_at(am, "mid [[", marks.seq)
    _turn_at(am, "blocks [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    _turn_at(am, "last [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    assert not any(r.startswith("question pending") for r in _skips(am, s1, marks.autorunner)), _why(am, s1, marks)
    assert len(_sends(am, s1, marks.autorunner)) == 3, _why(am, s1, marks)


@pytest.mark.p2
@pytest.mark.flow("PQR-08")
@pytest.mark.slow
def test_a_new_question_after_a_release_holds_the_queue_again(am, s1):
    """PQR-08 E5: the question guard re-latches: after the user's answer released the first hold, the next queued
    turn itself ends with a question, and the prompt after it is held again ("question pending …" once more) past a
    whole settle window.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue('q1 [[mock reply "Shall I continue?"]]', 'n1 [[mock reply "Do you want more?"]]',
               'n2 [[mock reply "the end"]]')
    _turn_at(am, "q1 [[", marks.seq)
    _wait_skip(am, s1, "question pending", marks.autorunner, timeout_s=90)
    s1.ask('yes [[mock reply "Fine."]]', wait="sent")
    _turn_at(am, "n1 [[", marks.seq)
    mark = am.profile.autorunner.mark()
    _wait_skip(am, s1, "question pending", mark, timeout_s=90)
    _assert_no_turn(am, "n2 [[", marks.seq, HOLD_CHECK_S, "n1's turn ended with a question")


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


@pytest.mark.p0
@pytest.mark.flow("PQR-10")
@pytest.mark.flow("PQR-05")
@pytest.mark.slow
def test_a_restart_lifts_the_global_pause_and_a_background_tab_gets_its_queue_only_once_entered(am, s2):
    """PQR-10 E1 and PQR-05 E2: the global pause holds a prompt queued into t1 (on Auto) until the app quits with t2
    focused. The reopen starts resumed — the pause is not kept — and re-homes t1 as a background tab whose claude does
    not run: nothing is typed into it past a whole settle window; entering the tab starts its claude, and the prompt
    goes once that start has settled.

    The mode is Auto before and after the restart (the cog's default and t1's own), so the test holds on 0.8.0, which
    re-stamps the default at a reopen, and on a build with PQR-K8's fix, which keeps the session's own mode.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    marks = _marks(am)
    set_global_pause(am.window.manager, True)
    am.profile.autorunner.wait(r"\[pause-all\] on", since=marks.autorunner, timeout_s=10)
    am.window.queue.open(a.title).queue('bg [[mock reply "after the reopen"]]')
    _wait_skip(am, a, "global pause", marks.autorunner)
    mark = am.profile.hooks.mark()
    b.select()  # t2 is the focused tab at the quit: t1 comes back in the background, dormant
    _let_the_window_record_save(am, mark)
    am.restart()
    after = am.profile.autorunner.mark()
    assert not global_paused(am.window.manager.open()), "the global pause survived the restart"
    am.window.tabs.wait_tab(a.title, timeout_s=30)
    b.select()
    _assert_no_turn(am, "bg [[", marks.seq, HOLD_CHECK_S, "the restored tab was never entered")
    assert not a.claude_pids(), "the background tab's claude runs before the tab was entered"
    assert not am.profile.autorunner.find(r"\[pause-all\] on", since=after)
    t_enter = time.time()
    a.select()
    a.wait_running(timeout_s=60)
    _turn_at(am, "bg [[", marks.seq)
    sent_at = _nth_send(am, a, marks.autorunner, 1)
    assert SETTLE_S - EARLY_S <= sent_at - t_enter, f"sent {sent_at - t_enter:.1f} s after the tab was entered"
    assert len(_turns(am, "bg [[", marks.seq)) == 1


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


@pytest.mark.p1
@pytest.mark.flow("PQR-11")
@pytest.mark.slow
def test_an_api_error_claude_code_retries_away_never_pauses_the_runner(am, s1):
    """PQR-11 E1: a queued turn whose first two attempts get 529 overloaded (Claude Code retries them) and whose third
    is answered never reads error: no stop-on-error pause, the runner stays on Auto, and the next queued prompt goes a
    settle window after the turn.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        clock = Clock(tl)
        tl.wait("waiting-for-you", 60)
        pane.queue('flaky [[mock error 529 overloaded_error "Overloaded" times=2; reply "ok after retries"]]',
                   'after [[mock reply "a"]]')
        _turn_at(am, "flaky [[", marks.seq)
        _turn_at(am, "after [[", marks.seq, timeout_s=RUNNER_WAIT_S + 60)
    assert "error" not in tl.states(), tl.describe()
    assert not am.profile.autorunner.find(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner)
    statuses = [e.get("status") for e in am.mock.requests(since=marks.seq, kind="main")
                if "flaky [[" in e.get("user_text", "")]
    assert statuses[:3] == [529, 529, 200], statuses
    sends = _sends(am, s1, marks.autorunner)
    assert len(sends) == 2, _why(am, s1, marks, tl)
    clock.assert_settled_send(sends[1], what="after")
    assert am.window.queue.open(s1.title).mode() == "Auto"


@pytest.mark.p1
@pytest.mark.flow("PQR-11")
@pytest.mark.slow
def test_a_stream_cut_mid_response_pauses_the_runner(am, s1):
    """PQR-11 E2: the connection is cut while a queued turn streams: Claude Code writes "Connection lost mid-response"
    as an API error, the session reads error, and stop-on-error pauses the runner, so the next queued prompt waits
    past a whole settle window (Restart session's rewind of such a turn: flows/06 INT-01).

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    with StateTimeline(am.window, s1.title) as tl:
        tl.wait("waiting-for-you", 60)
        pane.queue('cut [[mock say "half"; hold gc; reply "never"]]', 'after [[mock reply "a"]]')
        s1.select()
        _turn_at(am, "cut [[", marks.seq)
        am.mock.wait_holding("gc", timeout_s=60)
        am.mock.drop("gc")
        tl.wait("error", STATE_WAIT_S)
    am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: Connection lost mid-response", since=marks.scanner,
                            timeout_s=30)
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner, timeout_s=30)
    assert am.window.queue.open(s1.title).mode() == "Paused"
    _assert_no_turn(am, "after [[", marks.seq, HOLD_CHECK_S, "the turn's stream was cut: the session reads error")


# ---- PQR-12 · local commands in the queue ---------------------------------------------------------------------------------------


def _dialog_on_screen(terminal, title: str) -> bool:
    """Whether one of Claude Code's dialogs stands where the input box was: its title on screen and no input box —
    from ONE read of the terminal's text (each read walks UI Automation to the terminal and reads its whole buffer on
    the app's UI thread; run pqr6 logged a 31 s UI stall while a wait read it twice every 200 ms).

    Args:
        terminal: The window's Terminal.
        title: The dialog's title ("Switch model?", "Add directory to workspace").
    Returns:
        True while it is open.
    """
    text = terminal.text()
    return title in text and parse_input_box([row.rstrip() for row in text.splitlines()]) is None


@pytest.mark.p1
@pytest.mark.flow("PQR-12")
@pytest.mark.slow
def test_queued_model_commands_count_as_delivered_by_their_echo(am, s1):
    """PQR-12 phase 1 and E1: two queued `/model` commands (a local command: no UserPromptSubmit, no prompt line) and a
    prompt. The first switch opens Claude Code's "Switch model?" confirmation (2.1.291, the caret on "1. Yes, switch
    …"): while it is on screen nothing else is sent and no lost-send verdict comes, even past 15 s; Enter confirms.
    The transcript's command echo marks EACH delivery echoed (`[pull-echo] … the transcript's /model echo …`, one per
    command, oldest first), the runner stays on Auto, and the prompt after them runs on the last model picked.
    Measured (run pqr5): the second switch, right after the first, applied without asking — so a dialog is answered
    whenever one shows, and only the first is required.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue("/model haiku", "/model sonnet", 'after [[mock reply "on the new model"]]')
    s1.select()
    terminal = am.window.terminal

    def dialog_open() -> bool:
        """Whether a "Switch model?" dialog stands where the input box was (its title on screen, no box).

        Returns:
            True while one is open.
        """
        return _dialog_on_screen(terminal, "Switch model?")

    def echoes() -> int:
        """How many `/model` deliveries the transcript's echo has marked echoed so far.

        Returns:
            The count of `[pull-echo] … /model echo` lines since the marks.
        """
        return len(am.profile.hooks.find(rf"\[pull-echo\] {s1.short} prompt [0-9a-f]{{8}} \(the transcript's "
                                         rf"/model echo: the delivered command ran\)", since=marks.hooks))

    wait_until(dialog_open, RUNNER_WAIT_S + 30, interval_s=1.0, what="the first Switch model? dialog",
               describe=lambda: terminal.lines()[-12:])
    time.sleep(20)  # past the lost-send verdict's 15 s at rest: the open dialog must hold it
    assert not am.profile.hooks.find(rf"\[lost-send\] {s1.short}", since=marks.hooks), _why(am, s1, marks)
    assert not _turns(am, "after [[", marks.seq), "a prompt was sent while the model dialog was open"
    assert echoes() == 0, "the command counted as delivered while its dialog was still open"
    end = time.time() + RUNNER_WAIT_S + 60
    while echoes() < 2:  # answer every dialog that shows until both deliveries are echoed
        assert time.time() < end, f"only {echoes()} of the two /model deliveries were echoed; {_why(am, s1, marks)}"
        if dialog_open():
            _press(am, "enter")  # 1. Yes, switch …
            terminal.wait_input_box(timeout_s=30)
        time.sleep(2)
    e = am.mock.wait_prompt("after [[", since=marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    assert "sonnet" in (e.get("model") or "").lower(), e.get("model")
    pulled = am.profile.hooks.find(rf"\[pull-echo\] {s1.short} prompt ([0-9a-f]{{8}}) \(the transcript's /model echo",
                                   since=marks.hooks)
    assert len({m.group(1) for m in pulled}) == 2, [m.group(0) for m in pulled]  # each delivery its own echo
    assert not am.profile.hooks.find(rf"\[lost-send\] {s1.short}", since=marks.hooks), _why(am, s1, marks)
    pane = am.window.queue.open(s1.title)
    assert pane.mode() == "Auto", pane.mode()
    assert [r.status for r in pane.sent() if r.label.startswith("/model")] == ["sent", "sent"], pane.list_texts()


# ---- PQR-13 · editing the queue --------------------------------------------------------------------------------------------------


def _compose_history(am, s) -> tuple:
    """Set up PQR-13 phase 1: a second prompt typed in the terminal (the history, newest first: it, then F-S's "ready"),
    the Prompt Queue tab open, "my draft" typed into its compose box — and a key presser that waits for the box's
    text after each key.

    Args:
        am: The app.
        s: The Session (F-S).
    Returns:
        (the newest sent prompt, the oldest one, press(key, expect): the key pressed in the compose box, then a wait
        until the box reads `expect`).
    Raises:
        WaitTimeout / MockError: When the typed turn or the draft did not land.
    """
    s.ask('second [[mock reply "two"]]')
    am.window.queue.open(s.title)
    page = am.window.manager
    page.compose_box().click()  # the box takes the keyboard (the session has no unsent draft for it to pull in)
    inputs.type_text("my draft")
    wait_until(lambda: page.compose_box().value == "my draft", 5, what="the typed draft")

    def press(key: str, expect: str) -> None:
        """Press a key in the compose box and wait for its text.

        Args:
            key: The chord.
            expect: The text the box must then hold.
        Returns:
            None.
        Raises:
            WaitTimeout: When it did not.
        """
        inputs.keys(key)
        wait_until(lambda: page.compose_box().value == expect, 5, what=f"the box to read {expect!r} after {key}",
                   describe=lambda: page.compose_box().value)

    return 'second [[mock reply "two"]]', 'ready [[mock reply "ready"]]', press


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
def test_up_in_the_compose_box_recalls_the_newest_sent_prompt(am, s1):
    """PQR-13 phase 1 (its first step) and E3: in the compose box a MODIFIED arrow (Shift+Up, Ctrl+Up) belongs to the
    text box — the draft stays — and a plain Up recalls the session's newest sent prompt. (Walking further back is
    PQR-K10.)

    Args:
        am: The app.
        s1: F-S.
    """
    newest, _oldest, press = _compose_history(am, s1)
    press("shift+up", "my draft")
    press("ctrl+up", "my draft")
    press("up", newest)


@pytest.mark.p2
@pytest.mark.flow("PQR-13")
@pytest.mark.known_defect("PQR-K10")
def test_known_defect_a_second_up_does_not_walk_back_to_older_prompts(am, s1):
    """PQR-K10 (found by this suite on 2026-10-26, run pqr5): the compose box's history walk does not go past the
    newest prompt — TODAY the second Up recalls the newest prompt again. The likely cause, read in the code: the
    recall (AgentManagerContent::_ApplyPromptHistoryText) writes the text under a latch meant to keep that write from
    counting as the user's edit, on the assumption that the TextBox raises TextChanged synchronously; CLAUDE.md's
    Gotcha says TextChanged is asynchronous, so the handler would run after the latch is down and reset the walk,
    and the next Up starts over at the newest. Expected once fixed: the second Up recalls the older prompt, Down
    walks back, and Down past the newest — or Esc — restores the draft.

    Args:
        am: The app.
        s1: F-S.
    """
    newest, _oldest, press = _compose_history(am, s1)
    press("up", newest)
    inputs.keys("up")
    time.sleep(1.5)  # a recall shows within a frame or two: the older prompt has had its chance
    assert am.window.manager.compose_box().value == newest  # TODAY (expected: the older "ready …")


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
def test_ctrl_shift_up_pops_the_last_queued_prompt_back_into_an_empty_box(am, s1):
    """PQR-13 phase 2 and E1: Ctrl+Shift+Up in the EMPTY compose box takes the LAST queued prompt back out of the queue
    and into the box (`[nav] queue-pop <sid8> "…"`), to edit it; with text in the box the chord is the text box's own
    (it extends the selection) and pops nothing.

    Args:
        am: The app.
        s1: F-S.
    """
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"  # nothing may go while the queue is edited
    pane.queue('one [[mock reply "1"]]', 'two [[mock reply "2"]]')
    page = am.window.manager
    page.compose_box().set_value("")
    page.compose_box().click()
    mark = am.profile.hooks.mark()
    inputs.keys("ctrl+shift+up")
    am.profile.hooks.wait(rf'\[nav\] queue-pop {s1.short} "two ', since=mark, timeout_s=10)
    wait_until(lambda: page.compose_box().value == 'two [[mock reply "2"]]', 5, what="the popped prompt in the box",
               describe=lambda: page.compose_box().value)
    assert [r.label[:3] for r in pane.upcoming()] == ["one"], pane.list_texts()
    mark = am.profile.hooks.mark()
    inputs.keys("ctrl+shift+up")  # text in the box now: the TextBox's selection, no pop
    time.sleep(2)  # a pop would have logged its line by now
    assert not am.profile.hooks.find(r"\[nav\] queue-pop ", since=mark)
    assert page.compose_box().value == 'two [[mock reply "2"]]'
    assert [r.label[:3] for r in pane.upcoming()] == ["one"], pane.list_texts()


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
def test_a_rows_menu_offers_copy_everywhere_and_the_queue_ops_on_queued_rows(am, s1):
    """PQR-13 phase 3: a queued row's right-click menu holds Copy, Move up, Move down and Delete; a sent row's holds only
    Copy. Copy puts the row's whole prompt on the clipboard — a typed row verbatim, a queued one as queued.

    Args:
        am: The app.
        s1: F-S.
    """
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Paused"
    pane.queue('menu [[mock reply "m"]]')
    menu = pane.row_menu(pane.row("menu", "upcoming"))
    assert menu.names() == ["Copy", "Move up", "Move down", "Delete"], menu.names()
    menu.close()
    menu = pane.row_menu(pane.row("ready", "sent"))
    assert menu.names() == ["Copy"], menu.names()
    win32.set_clipboard_text("")
    mark = am.profile.hooks.mark()
    menu.choose("Copy")
    wait_until(lambda: win32.clipboard_text() == 'ready [[mock reply "ready"]]', 10, what="the typed row's prompt",
               describe=win32.clipboard_text)
    am.profile.hooks.wait(rf"\[nav\] copy auto-testing-prompt {s1.short} \(typed\)", since=mark, timeout_s=10)
    pane = am.window.queue.open(s1.title)
    win32.set_clipboard_text("")
    pane.row_menu(pane.row("menu", "upcoming")).choose("Copy")
    wait_until(lambda: win32.clipboard_text() == 'menu [[mock reply "m"]]', 10, what="the queued row's prompt",
               describe=win32.clipboard_text)


def _save_template_from(am, s, name: str, *prompts: str) -> None:
    """Queue prompts into a Paused session and save its queue as a template (the templates row's name box and "Save as
    queue template"), confirmed by the engine's `[nav] template-save "<name>" …` line.

    Args:
        am: The app.
        s: The Session (set Paused here, so nothing is sent while the test reads its queue).
        name: The template's name.
        *prompts: The prompts to queue first.
    Returns:
        None.
    Raises:
        WaitTimeout: When the save was not logged.
    """
    mark = am.profile.hooks.mark()
    pane = am.window.queue.open(s.title)
    assert pane.click_toggle() == "Paused"
    pane.queue(*prompts)
    pane.save_template(name)
    am.profile.hooks.wait(rf'\[nav\] template-save "{name}" prompts=\d+ from={s.short}', since=mark, timeout_s=10)


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
def test_a_queue_saved_as_a_template_is_appended_to_another_sessions_queue(am, s2):
    """PQR-13 phase 4: a session's queue saved as a template (the paper icon's row: a name, "Save as queue template")
    is appended to another session's queue by "Apply" — its prompts at the end of UPCOMING, queued (Pending), in their
    order (`[nav] template-apply "<name>" prompts=N -> <sid8>`).

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    marks = _marks(am)
    pane_b = am.window.queue.open(b.title)
    assert pane_b.click_toggle() == "Paused"  # what the template appends must not go while the test reads it
    _save_template_from(am, a, "plan1", 'ta [[mock reply "a"]]', 'tb [[mock reply "b"]]')
    pane_b = am.window.queue.open(b.title)
    pane_b.apply_template()
    am.profile.hooks.wait(rf'\[nav\] template-apply "plan1" prompts=\d+ -> {b.short}', since=marks.hooks,
                          timeout_s=10)
    wait_until(lambda: [r.label[:2] for r in pane_b.upcoming()][-2:] == ["ta", "tb"], 10,
               what="the template's prompts at the end of t2's queue", describe=pane_b.list_texts)
    assert all(r.status == "pending" for r in pane_b.upcoming()), pane_b.list_texts()
    assert [r.label[:2] for r in am.window.queue.open(a.title).upcoming()] == ["ta", "tb"]  # the source keeps its own


# ---- PQR-14 · typed prompts are recorded once ------------------------------------------------------------------------------------


def _submit_right_to_left(am, s, text: str, timeout_s: float = 60.0) -> None:
    """Type a prompt that starts in a right-to-left script into a session's terminal, submit it, and wait until the
    model got it. Claude Code draws such a line in VISUAL order (PQR-K9), so its input box never reads like the typed
    text — what Terminal.submit waits for — and this waits instead for the box to hold the same characters in any
    order (spaces aside: the visual line is padded differently) before it presses Enter.

    Args:
        am: The app.
        s: The Session.
        text: The prompt (its mock script inside `[[mock …]]`).
        timeout_s: How long the box and the model's request may take.
    Returns:
        None.
    Raises:
        WaitTimeout / MockError: When the text never showed in the box or never reached the model.
    """
    w = s.select()
    terminal = w.terminal
    terminal.wait_input_box(timeout_s=timeout_s)
    seq = am.mock.last_seq()
    terminal.focus()
    terminal.type(text)
    want = sorted(text.replace(" ", ""))
    wait_until(lambda: (lambda box: box is not None and sorted(box.draft.replace(" ", "")) == want)(terminal.input_box()),
               10, interval_s=0.1, what="the typed right-to-left prompt in the input box",
               describe=lambda: terminal.lines()[-6:])
    inputs.keys("enter")
    am.mock.wait_prompt(text.split("[[", 1)[0].strip()[:60], since=seq, timeout_s=timeout_s)


@pytest.mark.p1
@pytest.mark.flow("PQR-14")
def test_typed_prompts_are_recorded_once_and_protocol_noise_not_at_all(am, s1):
    """PQR-14 phases 1 and 3, E2: every prompt typed into the terminal is recorded once in the session's history as a
    "typed" row (`[ups] … -> recorded as Typed`) — Hebrew intact — while a message shaped like an agent team's
    delivery is protocol noise and is not recorded (`-> noise (not recorded)`).

    Args:
        am: The app.
        s1: F-S (its first prompt is already one typed row).
    """
    marks = _marks(am)
    s1.ask('hello [[mock reply "h"]]')
    mark = am.profile.hooks.mark()
    _submit_right_to_left(am, s1, 'שלום [[mock reply "ok"]]')
    am.profile.hooks.wait(rf"\[Stop\] {s1.id} state=2", since=mark, timeout_s=STATE_WAIT_S)  # its turn ended
    s1.ask(TEAMMATE_PROMPT)
    assert len(am.profile.hooks.find(rf"\[ups\] {s1.short} chars=\d+ hash=\S+ -> recorded as Typed",
                                     since=marks.hooks)) == 2
    am.profile.hooks.wait(rf"\[ups\] {s1.short} chars=\d+ hash=\S+ -> noise \(not recorded\)", since=marks.hooks,
                          timeout_s=30)
    pane = am.window.queue.open(s1.title)
    sent = pane.sent()
    assert [r.label.split()[0] for r in sent] == ["ready", "hello", "שלום"], pane.list_texts()
    assert all(r.tag == "typed" for r in sent), [(r.label, r.tag) for r in sent]
    assert pane.counts() == (3, 3, 0), pane.list_texts()
    assert am.claude.user_prompts(s1.id).count('שלום [[mock reply "ok"]]') == 1


# ---- PQR-15 · the queue across a restart ------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("PQR-15")
@pytest.mark.needs_fix("PQR-K8")
@pytest.mark.slow
def test_a_paused_queue_stays_paused_across_a_restart_and_nothing_sent_is_sent_again(am, s1):
    """PQR-15 phases 1–2 (PQR-K8's fix, `needs_fix`): a Paused session holding two queued prompts survives a quit and a
    reopen — same rows, same statuses, the sent one never typed again — and KEEPS its mode: the runner's mode is per
    session and persisted (the owner's rule of 2026-10-26), so nothing is sent by itself after the reopen, past a whole
    settle window once its claude runs. When the user switches the runner to Auto, the two go, one per turn.

    On 0.8.0 the reopen re-stamps the cog's default (Auto) and the two prompts go by themselves (PQR-K8): this test
    fails there by design (`-m "not needs_fix"` deselects it).

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
    assert pane.mode() == "Paused", f"{pane.mode()}: the reopen did not keep the session's own mode (PQR-K8)"
    s1.select()
    s1.wait_running(timeout_s=60)  # the reopened window's focused tab: its claude starts when the tab is shown
    _assert_no_turn(am, "r1 [[", marks.seq, HOLD_CHECK_S, "the session's runner was left Paused before the restart")
    assert not _sends(am, s1, marks.autorunner), _why(am, s1, marks)
    pane = am.window.queue.open(s1.title)
    assert pane.click_toggle() == "Auto"
    _turn_at(am, "r1 [[", marks.seq, timeout_s=RELEASE_S + ARRIVE_S)
    _turn_at(am, "r2 [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    assert len(_turns(am, "r1 [[", marks.seq)) == 1 and len(_turns(am, "r2 [[", marks.seq)) == 1
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


@pytest.mark.p0
@pytest.mark.flow("PQR-15")
@pytest.mark.slow
def test_a_turn_cut_off_by_a_crash_pauses_the_runner_and_holds_the_queue(am, s1):
    """PQR-15 E1: the app's GUI is killed (a crash: no close flush) while a queued turn runs and another prompt waits.
    The restart reads the cut-off turn as error and stop-on-error pauses the runner, exactly as after a quit: the
    waiting prompt is not sent after the turn — the user decides.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    am.window.queue.open(s1.title).queue('c1 [[mock bash "echo a"; hold g1; reply "one"]]', 'c2 [[mock reply "two"]]')
    s1.select()
    _turn_at(am, "c1 [[", marks.seq)
    am.mock.wait_holding("g1", timeout_s=90)
    am.kill()  # the crash: the GUI ends without its close flush; its ConPTY hosts end their claudes
    am.mock.release("g1")
    am.start()
    am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner, timeout_s=STATE_WAIT_S)
    _assert_no_turn(am, "c2 [[", marks.seq, HOLD_CHECK_S, "the turn was cut off by the crash")
    assert am.window.queue.open(s1.title).mode() == "Paused"


@pytest.mark.p0
@pytest.mark.flow("PQR-15")
@pytest.mark.slow
def test_a_prompt_sent_just_before_a_quit_is_never_sent_again(am, s1):
    """PQR-15 E2 (Rule #4): the app quits right after the runner decided to send a prompt — the prompt is Sent, its
    echo maybe not yet seen. After the reopen it is history (SENT), never queued again and never typed again, and the
    queue is not stuck behind it: a prompt queued after the reopen goes (no false "awaiting injection pickup"). If the
    quit cut the prompt's own turn, the reopen reads it as a cut-off turn and stop-on-error pauses the runner; the
    test then re-arms it, as the user would.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    am.window.queue.open(s1.title).queue('q1 [[mock reply "one"]]')
    am.profile.autorunner.wait(rf"\[send\] {s1.id} #\d+", since=marks.autorunner, timeout_s=RUNNER_WAIT_S)
    am.close(how="all")  # at once: the delivery and its echo may still be under way
    am.kill()
    am.start()
    after = _marks(am)
    pane = am.window.queue.open(s1.title)
    assert not any(r.label.startswith("q1") for r in pane.upcoming()), pane.list_texts()
    assert any(r.label.startswith("q1") for r in pane.sent()), pane.list_texts()
    s1.select()
    s1.wait_running(timeout_s=60)
    # At rest again whatever the quit cut: a turn it cut off reads error first (its verdict, then the stop-on-error
    # pause), and Claude Code continues that turn by itself on its resume — so wait for a ready state that holds.
    with StateTimeline(am.window, s1.title) as tl:
        # Idle when the quit came before the prompt's turn started (run pqr7), waiting-for-you after a turn.
        _wait_quiet(tl, quiet_s=10, timeout_s=STATE_WAIT_S)
    pane = am.window.queue.open(s1.title)
    if pane.mode() == "Paused":
        # The pause is logged during the start itself (the cut-off verdict comes with the first scan), before the
        # post-restart marks were taken: read it from the marks taken before the quit (run pqr5).
        assert am.profile.autorunner.find(rf"\[stop-on-error\] paused {s1.id}", since=marks.autorunner), \
            "the runner reads Paused after the reopen with no stop-on-error pause"
        assert pane.click_toggle() == "Auto"
    pane.queue('q2 [[mock reply "two"]]')
    _turn_at(am, "q2 [[", after.seq, timeout_s=RUNNER_WAIT_S + 30)
    # Agentmaster sent q1 once — its only `[send] #1` and one `[delivered]` of that prompt id — and it is ONE user line
    # in the transcript. The model may see it twice: when the quit cut q1's turn, Claude Code's resume continues it
    # by itself (CLAUDE_CODE_RESUME_INTERRUPTED_TURN), a request whose last user text is q1 joined with "Continue from
    # where you left off." (run pqr6) — Claude Code's continuation, not a send.
    assert len(am.profile.autorunner.find(rf"\[send\] {s1.id} #1$", since=marks.autorunner)) == 1, _why(am, s1, marks)
    q1_id = am.profile.hooks.find(rf"\[delivered\] {s1.short} prompt ([0-9a-f]{{8}}) ", since=marks.hooks)[0].group(1)
    assert len(am.profile.hooks.find(rf"\[delivered\] {s1.short} prompt {q1_id} ", since=marks.hooks)) == 1, \
        "the prompt sent before the quit was typed again"
    assert am.claude.user_prompts(s1.id).count('q1 [[mock reply "one"]]') == 1, am.claude.user_prompts(s1.id)[-4:]
    assert not [e for e in am.mock.prompts(since=marks.seq) if e.get("user_text") == 'q1 [[mock reply "one"]]'][1:], \
        "a second turn started with q1 alone: it was sent again"
    assert not any(r.startswith("awaiting injection pickup") for r in _skips(am, s1, after.autorunner)), \
        _why(am, s1, after)


# ---- PQR-16 · several sessions and windows -------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("PQR-16")
def test_two_sessions_keep_their_own_runner_modes(am, s2):
    """PQR-16 phase 1: two sessions with a queued prompt each, one on Auto and one Paused: only the Auto one's prompt
    goes; the Paused one's waits past a whole settle window.

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    marks = _marks(am)
    pane_b = am.window.queue.open(b.title)
    assert pane_b.click_toggle() == "Paused"
    pane_b.queue('pb [[mock reply "b"]]')
    am.window.queue.open(a.title).queue('pa [[mock reply "a"]]')
    _turn_at(am, "pa [[", marks.seq)
    _assert_no_turn(am, "pb [[", marks.seq, HOLD_CHECK_S, "t2's runner is Paused")
    assert not _sends(am, b, marks.autorunner), _why(am, b, marks)


@pytest.mark.p1
@pytest.mark.flow("PQR-16")
@pytest.mark.slow
def test_a_prompt_queued_from_another_window_is_delivered_by_the_hosting_window(am, s2):
    """PQR-16 phases 2–3: window 1's Agent Manager (its board on GLOBAL) queues a prompt into a session window 2 hosts:
    window 2 delivers it (its tab's box, its draft swap), and window 1's Prompt Queue shows the row move to SENT. The
    compose row's Jump to tab then brings window 2 to the front on that session's tab.

    Args:
        am: The app.
        s2: F-S2 (t2 is moved into a window of its own).
    """
    a, b = s2
    marks = _marks(am)
    w1 = am.window
    w2 = w1.tabs.move_to_new_window(b.title)
    _board_global(w1.manager)
    w1.queue.open(b.title).queue('xw [[mock reply "from window 1"]]')
    e = am.mock.wait_prompt("xw [[", since=marks.seq, timeout_s=RUNNER_WAIT_S)
    assert e.get("session") == b.id, e
    am.profile.hooks.wait(rf"\[delivered\] {b.short} prompt [0-9a-f]{{8}}", since=marks.hooks, timeout_s=30)
    pane = w1.queue.open(b.title)
    pane.wait_row("xw", "sent")
    w1.focus()
    buttons = w1.manager.compose_buttons()
    assert len(buttons) == 3, [x.line() for x in buttons]
    buttons[0].click()  # Jump to tab
    wait_until(lambda: win32.foreground_hwnd() == w2.hwnd, 15, what="window 2 in front",
               describe=win32.foreground_hwnd)
    w2.tabs.wait_selected(b.title)


@pytest.mark.p1
@pytest.mark.flow("PQR-16")
@pytest.mark.slow
def test_closing_a_window_keeps_its_sessions_queue_for_a_resume(am, s2):
    """PQR-16 E1: window 2 is closed (its ✕ → Close Window) while its session has a queued prompt: the session is
    closed with Close's semantics — archived, its record and queue kept — so the Sessions page's Resume here brings it
    back, in window 1, with the prompt still queued, which then goes once into the resumed conversation.

    The session is on Auto throughout (its own mode and the cog's default), so the test holds on 0.8.0 and on a build
    with PQR-K8's fix alike.

    Args:
        am: The app.
        s2: F-S2 (t2 is moved into a window of its own).
    """
    a, b = s2
    marks = _marks(am)
    w1 = am.window
    w2 = w1.tabs.move_to_new_window(b.title)
    t_queue = w2.queue.open(b.title).queue('kept [[mock reply "after the resume"]]')
    _close_window(am, w2)
    assert time.time() - t_queue < SETTLE_S - EARLY_S, "the close took longer than the settle window: the test is void"
    wait_until(lambda: not b.claude_pids(), 30, what="the closed window's claude to exit")
    _assert_no_turn(am, "kept [[", marks.seq, max(0.0, SETTLE_S + 5 - (time.time() - t_queue)), "its window was closed")
    sessions = w1.sessions.open()
    assert sessions.resume(b.title) == b.short  # the same conversation, not a fresh one
    w1.tabs.wait_tab(b.title, timeout_s=30)
    pane = w1.queue.open(b.title)
    assert [r.label[:4] for r in pane.upcoming()] == ["kept"], pane.list_texts()
    b.select()
    b.wait_running(timeout_s=60)
    e = am.mock.wait_prompt("kept [[", since=marks.seq, timeout_s=RUNNER_WAIT_S)
    assert e.get("n_msgs", 0) > 1, e  # into the resumed conversation
    time.sleep(3)  # a second delivery would have reached the mock by now
    assert len(_turns(am, "kept [[", marks.seq)) == 1


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


@pytest.mark.p0
@pytest.mark.flow("PQR-01")
@pytest.mark.known_defect("PQR-K9")
@pytest.mark.slow
@pytest.mark.parametrize("prompt", ['שלום [[mock reply "ok"]]', 'say שלום please [[mock reply "ok"]]'],
                         ids=["hebrew-first", "hebrew-inside"])
def test_known_defect_a_queued_prompt_with_hebrew_is_never_delivered(am, s1, prompt):
    """PQR-K9 (found by this suite on 2026-10-26, PQR-01 E2): Claude Code (2.1.291) draws a line holding right-to-left
    text in VISUAL order, so the verified delivery's read-back of the input box after the fill does not read like the
    queued prompt — measured (run pqr5): the box read `" ]]"mock reply "ok[[ <emoji> םולש"` for the queued
    `שלום <emoji> [[mock reply "ok"]]`. TODAY the delivery takes that for foreign content ("FOREIGN read-back after
    the fill … the merge this would have submitted is refused; undoing our insertion"), tries a second fill, gives up
    ("FAILED after 2 unverifiable deliveries") and marks the prompt Failed: it never reaches the model, and the
    session's remembered unsent draft holds the reversed text. Expected once fixed: the read-back compares in the
    box's visual order, and the prompt is delivered once.

    Args:
        am: The app.
        s1: F-S.
        prompt: The queued prompt: Hebrew first (the whole line reads right to left), or a Hebrew word inside a
            left-to-right line.
    """
    marks = _marks(am)
    pane = am.window.queue.open(s1.title)
    pane.queue(prompt)
    am.profile.hooks.wait(rf"\[send-verify\] {s1.short} FOREIGN read-back after the fill", since=marks.hooks,
                          timeout_s=RUNNER_WAIT_S)  # TODAY
    am.profile.autorunner.wait(rf"\[send-verify\] {s1.short} prompt [0-9a-f]{{8}} FAILED after 2 unverifiable "
                               rf"deliveries", since=marks.autorunner, timeout_s=60)  # TODAY
    _assert_no_turn(am, "[[mock reply", marks.seq, 15, "TODAY the delivery refused its own fill")
    pane = am.window.queue.open(s1.title)
    assert pane.row(prompt[:8], "sent").status == "failed", pane.list_texts()  # TODAY: ✕


@pytest.mark.p1
@pytest.mark.flow("PQR-01")
@pytest.mark.known_defect("PQR-K11")
@pytest.mark.slow
def test_known_defect_a_long_single_line_prompt_is_never_delivered(am, s1):
    """PQR-K11 (found by this suite on 2026-10-26, PQR-01 E4, run pqr6): Claude Code shows a long paste of ONE line
    collapsed as a bare "[Pasted text #1]" — no "+M lines" part, which only a multi-line paste carries — and the
    verified delivery's paste-marker parser (PendingPaste.h ParseMarkerAt) knows only the "+M lines" form, so the
    read-back after the fill is no collapsed render of the prompt but foreign content. TODAY: "FOREIGN read-back after
    the fill (box chars=16 head="[Pasted text #1]")", a second fill, then "FAILED after 2 unverifiable deliveries": the
    2,000-character prompt never reaches the model. Expected once fixed: the bare marker verifies as the collapsed
    render of a one-line prompt (DraftCarry.h already parses it), and the prompt goes once, whole.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    words = " ".join(f"w{i:04d}" for i in range(1, 400))
    prompt = "long " + words[:1990] + ' [[mock reply "long ok"]]'
    am.window.queue.open(s1.title).queue(prompt)
    am.profile.hooks.wait(rf'\[send-verify\] {s1.short} FOREIGN read-back after the fill \(box chars=\d+ '
                          rf'head="\[Pasted text #\d+\]"\)', since=marks.hooks, timeout_s=RUNNER_WAIT_S)  # TODAY
    am.profile.autorunner.wait(rf"\[send-verify\] {s1.short} prompt [0-9a-f]{{8}} FAILED after 2 unverifiable "
                               rf"deliveries", since=marks.autorunner, timeout_s=60)  # TODAY
    _assert_no_turn(am, "long w0001", marks.seq, 15, "TODAY the delivery refused its own fill")


@pytest.mark.p2
@pytest.mark.flow("PQR-13")
@pytest.mark.known_defect("PQR-K4")
def test_known_defect_a_template_saves_the_sessions_history_too(am, s2):
    """PQR-K4 (Persistence.cpp MakeTemplateFromQueue copies EVERY row of the queue): a template saved from a session
    holds its history too. TODAY: F-S's typed "ready" prompt (a SENT row) is saved beside the one queued prompt — the
    save says prompts=2 — and the session the template is applied to gets "ready …" queued as Pending, a prompt that
    session would then be sent. Expected once fixed: only Pending prompts are saved (prompts=1) and applied. (The
    defect's other half — Apply to dir reaching an archived session of the folder — has no test yet.)

    Args:
        am: The app.
        s2: F-S2.
    """
    a, b = s2
    marks = _marks(am)
    pane_b = am.window.queue.open(b.title)
    assert pane_b.click_toggle() == "Paused"
    _save_template_from(am, a, "k4", 'only [[mock reply "o"]]')
    assert am.profile.hooks.find(rf'\[nav\] template-save "k4" prompts=2 from={a.short}', since=marks.hooks), \
        am.profile.hooks.find(r'\[nav\] template-save .*', since=marks.hooks)[-1:]  # TODAY: the history row too
    pane_b = am.window.queue.open(b.title)
    pane_b.apply_template()
    wait_until(lambda: [r.label.split()[0] for r in pane_b.upcoming()] == ["ready", "only"], 10,
               what="t2's queue after the apply", describe=pane_b.list_texts)  # TODAY: "ready …" queued again


@pytest.mark.p2
@pytest.mark.flow("PQR-16")
@pytest.mark.known_defect("PQR-K5")
@pytest.mark.am_settings(maxAutoSends=2)
@pytest.mark.slow
def test_known_defect_an_adopted_session_ignores_the_cogs_send_budget(am, s1):
    """PQR-K5 (SessionRegistry's adopt create stamps only the cog's MODE): a claude typed by hand into a shell tab is
    adopted, and the registry mints its record with the cog's default mode but the struct's backstops. TODAY: with the
    cog's maxAutoSends at 2, the adopted session takes the struct default (100), so all three queued prompts go — while
    a session launched by Agentmaster holds its third one (PQR-02 E3). Expected once fixed: the adopted session takes
    the cog's maxAutoSends / stopOnError too, and its third prompt waits ("maxAutoSends reached").

    Args:
        am: The app (settings.json says maxAutoSends 2 before the start).
        s1: F-S (the window's other tab).
    """
    marks = _marks(am)
    h = _adopt_typed_claude(am, "h1", marks.seq)
    pane = am.window.queue.open("h1")
    pane.queue('k1 [[mock reply "one"]]', 'k2 [[mock reply "two"]]', 'k3 [[mock reply "three"]]')
    _turn_at(am, "k1 [[", marks.seq)
    _turn_at(am, "k2 [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)
    _turn_at(am, "k3 [[", marks.seq, timeout_s=RUNNER_WAIT_S + 30)  # TODAY: the third goes too
    assert len(_sends(am, h, marks.autorunner)) == 3, _why(am, h, marks)


# ---- reclassified: accepted behavior (flows/03 "Reclassified: accepted behavior") ---------------------------------------
# Filed as known defects on 2026-10-24 and ruled ACCEPTED by the owner on 2026-10-26: each test now proves the behavior.


@pytest.mark.p2
@pytest.mark.flow("PQR-03")
@pytest.mark.slow
def test_restart_session_sends_a_held_prompt_once_its_box_is_back(am, s1):
    """PQR-03 E5 (PQR-K6 as filed on 2026-10-24, accepted by the owner on 2026-10-26: "what we have now is okay"): a
    prompt the runner held because the session's claude had ended (no Claude input box on the screen) is typed into the
    claude that Restart session starts as soon as its input box draws — measured 8 s after the restart, a second
    before that claude's SessionStart hook ran — with no new settle window. A restart changes none of the facts the
    window counts from: the session read waiting-for-you before and after it (a claude that ended without its
    SessionEnd never reads done, STA-K1) and its `started` never changed, so the window, long over, is not started
    again. (A parked tab's WAKE does start one: `started` changes — PQR-10.)

    The claude is KILLED here (≥ 10 s after its turn, so the session keeps reading waiting-for-you): the deterministic
    way to an ended claude whose session still reads ready. The probe met the same with `/exit`, whose SessionEnd hook
    the loaded desktop cancelled; when that hook arrives the session reads done, and the SessionStart after a restart
    is then a status change, which does start a window.

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
        f"sent {sent_at - t_restart:.1f} s after the restart: a restart now starts a settle window ({_why(am, s1, marks)})"


@pytest.mark.p1
@pytest.mark.flow("PQR-13")
@pytest.mark.flow("LIF-07")
@pytest.mark.slow
def test_clear_leaves_the_tabs_queue_with_the_old_conversation(am, s1):
    """LIF-07 phase 2 and PQR-13 E5 (PQR-K7 as filed on 2026-10-24, a FEATURE by the owner's ruling of 2026-10-26: the
    queue does not follow the tab across `/clear`): `/clear` in a tab starts a new conversation there — the tab, its
    card and its Telegram topic follow it (`[nav] tab-swap <old> -> <new>`, `[lineage] <new> continues <old> (/clear
    …)`) — while the prompts queued for the tab stay with the old conversation's record, which is archived (with its
    own Paused mode). The tab's new conversation is a new session: an empty queue and the cog's default mode (Auto),
    and the old queue's prompt is never sent into it. The Prompt Queue tab says "Select a session …" until the new card
    is picked.

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
    assert not pane.upcoming(), pane.list_texts()  # the queued prompt stays with the old conversation
    assert pane.mode() == "Auto", pane.mode()  # a new session: the cog's default mode
    _assert_no_turn(am, "kept [[", marks.seq, HOLD_CHECK_S, "its conversation was cleared")  # never sent into the new one
    am.window.tabs.wait_tab(s1.title)
    assert am.profile.session(new_key) is not None
    # The prompt is still in the OLD conversation's record, which keeps its own Paused mode: one read of sessions.json,
    # after the hold check (the app is at rest; a read is never polled — ROB-K1).
    old = am.profile.session(s1.id)
    assert old is not None, "the old conversation's record is gone"
    old_queue = old.raw.get("queue", [])
    assert any(p.get("text", "").startswith("kept [[") and p.get("status") == "Pending" for p in old_queue), old_queue
    assert old.raw.get("autorunner", {}).get("mode") == "Off", old.raw.get("autorunner")

