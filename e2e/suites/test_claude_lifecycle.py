"""Claude Code's life cycle as Agentmaster manages it (doc/agentmaster/flows/24-claude-lifecycle.md, LIF).

A managed session's claude.exe is born at a launch (a fresh `--session-id`), lives through turns, and ends by
`/exit` (its SessionEnd: Done, the tab drops to its PowerShell host), by a kill (no SessionEnd), by a park
(Deactivate), by a Close (the session archived) or by the app's quit — and is born again by Restart session, a wake,
Resume here on the Sessions page, or the reopen. A claude the user types by hand into a shell tab is adopted. Every
test here walks one of those edges and checks the three things a user relies on across it:

- the PROCESS: which claude.exe runs, with which command line (`--session-id` for a new conversation, `--resume
  <id>` to continue one), and that none is left behind;
- the CONVERSATION: the same id, the transcript continued (the mock's request carries the earlier messages:
  `n_msgs > 1`), or a fresh one where nothing was ever said (Rule #6, transcript-gated);
- the QUEUE: a prompt queued across the edge waits while no claude can take it (never typed into the shell left
  behind) and is delivered once a claude runs again and has settled (the Prompt Queue Runner, flows/03).

Measured against 0.8.0 + Claude Code 2.1.291 in the test desktop (2026-10-24, probe run `pqprobe1`); the facts each
assertion leans on are in the test's docstring.
"""
from __future__ import annotations

import os
import time
from pathlib import Path

import psutil
import pytest

from amdriver import inputs
from amdriver.session import Session
from amdriver.timeline import StateTimeline, engine_story
from amdriver.waits import wait_until

SETTLE_S = 40.0
"""The Prompt Queue Runner's settle window (Scheduler.h kStateSettleMs)."""
EARLY_S = 3.0
"""How much sooner than its window a send may look to come (the test's marks lag the registry's stamps)."""
LATE_S = 45.0
"""How much later than its window a send may come on a loaded desktop (the scheduler's re-check, the verified
placement, Claude Code's own UserPromptSubmit hook before it calls the model)."""
RUNNER_WAIT_S = SETTLE_S + LATE_S + 30.0
"""How long a test waits for a send the runner owes."""
HOLD_CHECK_S = SETTLE_S + 12.0
"""How long a "nothing is sent" check watches: past a whole settle window."""
STATE_WAIT_S = 120.0
"""How long a state may take to show on the side panel."""


def _marks(am) -> tuple[int, int, int, int]:
    """The ends of hooks.log, scanner.log, autorunner.log and the mock's request log now.

    Args:
        am: The app.
    Returns:
        (hooks, scanner, autorunner, mock seq).
    """
    return (am.profile.hooks.mark(), am.profile.scanner.mark(), am.profile.autorunner.mark(), am.mock.last_seq())


def _turns(am, marker: str, since: int) -> list[dict]:
    """Every turn whose prompt holds a text.

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
        seconds: How long to watch.
        why: What holds it (the failure message).
    Returns:
        None.
    Raises:
        AssertionError: When it was sent.
    """
    end = time.time() + seconds
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
        The decisions' wall-clock times, in log order.
    """
    return [when.timestamp() for when, _m in am.profile.autorunner.stamped(rf"\[send\] {s.id} #\d+", since=since)]


def _skips(am, s, since: int) -> list[str]:
    """The runner's reasons for holding a session's queue (autorunner.log `[advance-skip] <id> (<reason>)`).

    Args:
        am: The app.
        s: The Session.
        since: An autorunner.log mark.
    Returns:
        The reasons, in log order.
    """
    return [m.group(1) for m in am.profile.autorunner.find(rf"\[advance-skip\] {s.id} \((.*)\)$", since=since)]


def _why(am, s, marks, tl: StateTimeline = None) -> str:
    """The evidence a failed check carries.

    Args:
        am: The app.
        s: The Session.
        marks: _marks() before the action.
        tl: The StateTimeline (None = none).
    Returns:
        One line.
    """
    runner = [ln for ln in am.profile.autorunner.lines(marks[2]) if s.short in ln][-10:]
    story = engine_story(am.profile, s.id, marks[0], marks[1])
    return f"runner: {runner}; engine: {story}" + (f"; timeline: {tl.describe()}" if tl is not None else "")


def _restart_session(am, s) -> list[int]:
    """Restart a session from its tab's menu and wait for its new claude.

    Args:
        am: The app.
        s: The Session.
    Returns:
        The new claude's pids.
    Raises:
        WaitTimeout: When no new claude started.
    """
    before = set(s.claude_pids())
    am.window.tabs.context_menu(s.title).choose("Restart session")
    return wait_until(lambda: [p for p in s.claude_pids() if p not in before] or None, 60,
                      what=f"a new claude for {s.short}")


# ---- LIF-01 · a launch -------------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("LIF-01")
def test_a_launch_starts_a_fresh_claude_and_the_first_prompt_makes_its_transcript(am):
    """LIF-01 phases 1–3: a launch from the Agent Manager's launch row starts ONE claude with a new `--session-id`
    (never `--resume`), Agentmaster's hooks (`--settings`) and skip-permissions; the session reads idle with no
    transcript on disk; its first prompt creates the transcript under that id and the session reads running, then
    waiting-for-you.

    Args:
        am: The app.
    """
    marks = _marks(am)
    s = am.launch("t1", prompt=None)
    cmdlines = s.claude_cmdlines()
    assert len(cmdlines) == 1, cmdlines
    assert f"--session-id {s.id}" in cmdlines[0] and "--resume" not in cmdlines[0], cmdlines[0]
    assert "--settings" in cmdlines[0] and "--dangerously-skip-permissions" in cmdlines[0], cmdlines[0]
    am.profile.hooks.wait(rf"\[SessionStart\] {s.id} state=0", since=marks[0], timeout_s=60)
    am.window.side_panel.wait_card(s.title, state="idle", timeout_s=60)
    assert am.claude.transcript_path(s.id) is None, "a transcript before the first prompt"
    with StateTimeline(am.window, s.title) as tl:
        s.ask('first [[mock say "working"; delay 3s; reply "hello"]]', wait="sent")
        tl.wait("running", STATE_WAIT_S)
        tl.wait("waiting-for-you", STATE_WAIT_S)
    assert tl.states() == ["idle", "running", "waiting-for-you"], _why(am, s, marks, tl)
    assert am.claude.transcript_path(s.id) is not None
    assert 'first [[mock say "working"; delay 3s; reply "hello"]]' in am.claude.user_prompts(s.id)


@pytest.mark.p1
@pytest.mark.flow("LIF-01")
def test_a_prompt_queued_into_a_fresh_session_is_its_first_message(am):
    """LIF-01 E1: a session that has never been prompted sits idle — and idle is ready: a prompt queued into it is
    sent once the settle window from its queue time has passed, as the conversation's first message (the runner's
    idle start: a fresh session never ends a turn, so nothing else would start its queue).

    Args:
        am: The app.
    """
    marks = _marks(am)
    s = am.launch("t1", prompt=None)
    am.window.side_panel.wait_card(s.title, state="idle", timeout_s=60)
    t_queue = am.window.queue.open(s.title).queue('opening [[mock reply "first words"]]')
    e = am.mock.wait_prompt("opening", since=marks[3], timeout_s=RUNNER_WAIT_S)
    assert SETTLE_S - EARLY_S <= e["t"] - t_queue <= SETTLE_S + LATE_S, f"{e['t'] - t_queue:.1f} s after the queue"
    # The conversation's first message: the transcript (created by this very prompt) starts with it.
    wait_until(lambda: am.claude.user_prompts(s.id)[:1] == ['opening [[mock reply "first words"]]'], 30,
               what="the queued prompt as the transcript's first prompt",
               describe=lambda: am.claude.user_prompts(s.id)[:3])


# ---- LIF-02 · /exit, then Restart session --------------------------------------------------------------------------------


HOLD_AFTER_EXIT = ("input box not visible", "not ready")
"""Why the runner holds a session whose claude ended, when it writes a reason at all: no Claude input box on the screen
(the tab shows its PowerShell host). Two outcomes were measured: when Claude Code cancels the SessionEnd hook at its
5 s budget on a loaded desktop ("SessionEnd hook … failed: Hook cancelled"), the session keeps reading
waiting-for-you (STA-K1), the runner is asked, and the box gate holds the queue (pqprobe1 pq04: "input box not
visible", then the 60 s warning `[send-verify] <sid8> queued prompts HELD: no parseable input box for >60s …`); when
the hook arrives (run pqr1), the session reads Done and the runner is not asked at all — no line. A `/exit` test
therefore judges the hold by the mock's silence, never by a reason."""


@pytest.mark.p0
@pytest.mark.flow("LIF-02")
@pytest.mark.flow("PQR-09")
@pytest.mark.slow
def test_exit_ends_claude_and_restart_session_resumes_the_conversation_and_its_queue(am, s1):
    """LIF-02 phases 1–4 and PQR-09 phase 6: `/exit` ends claude (no claude left; the tab stays, on its PowerShell
    host, with claude's resume line; the session stays open); a prompt queued now waits past a whole settle window —
    nothing reaches the model, nothing is typed into the shell, no `[send]` (HOLD_AFTER_EXIT: the session reads Done
    and the runner is not asked, or it still reads waiting-for-you and the box gate holds it). Restart session (the
    tab's menu) starts claude again with `--resume <id>`: the same conversation continues (the queued prompt's request
    carries the earlier messages) and the prompt is delivered once.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    am.window.terminal.submit("/exit")
    wait_until(lambda: not s1.claude_pids(), 60, what="claude to exit")
    am.window.terminal.wait_text(r"claude --resume " + s1.id, timeout_s=30)
    assert am.window.tabs.find(s1.title) is not None  # the session stays open: the tab and its card
    am.window.queue.open(s1.title).queue('back [[mock reply "I am back"]]')
    s1.select()
    _assert_no_turn(am, "back [[", marks[3], HOLD_CHECK_S, "claude has exited")
    assert not _sends(am, s1, marks[2]), _why(am, s1, marks)
    assert all(r.startswith(HOLD_AFTER_EXIT) or r.startswith(("queue settle", "status settle"))
               for r in _skips(am, s1, marks[2])), _why(am, s1, marks)  # a reason, when one is written, is one of those
    assert "back [[" not in am.window.terminal.text(), "the queued prompt was typed into the shell"
    _restart_session(am, s1)
    am.profile.hooks.wait(rf"\[restart\] claude {s1.id} \(resume\)", since=marks[0], timeout_s=30)
    assert any(f"--resume {s1.id}" in c for c in s1.claude_cmdlines()), s1.claude_cmdlines()
    e = am.mock.wait_prompt("back [[", since=marks[3], timeout_s=RUNNER_WAIT_S)
    assert e.get("n_msgs", 0) > 1, e  # the resumed conversation came along
    time.sleep(3)
    assert len(_turns(am, "back [[", marks[3])) == 1


# ---- LIF-03 · a killed claude --------------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("LIF-03")
@pytest.mark.slow
def test_a_killed_claudes_shell_never_gets_the_queue_and_restart_session_recovers(am, s1):
    """LIF-03 phases 1–3: claude.exe is killed while the session waits for you (no SessionEnd: the session still reads
    waiting-for-you, STA-K1). Its tab drops to the PowerShell host with the console guard's note. A prompt queued now
    is never typed into that shell: the runner finds no Claude input box on the screen and holds ("input box not
    visible"). Restart session starts claude again with `--resume <id>`, and the prompt is delivered into it.

    Args:
        am: The app.
        s1: F-S.
    """
    marks = _marks(am)
    time.sleep(10)  # well past the first turn (a kill within ~2 s of a turn's end reads running for good: STA-K1)
    for pid in s1.claude_pids():
        try:
            psutil.Process(pid).kill()
        except psutil.NoSuchProcess:
            pass
    wait_until(lambda: not s1.claude_pids(), 30, what="the claude to be gone")
    am.window.terminal.wait_text(r"\[Agentmaster\] The agent exited", timeout_s=60)
    am.window.queue.open(s1.title).queue('revived [[mock reply "alive again"]]')
    s1.select()
    wait_until(lambda: next((r for r in _skips(am, s1, marks[2]) if r.startswith(HOLD_AFTER_EXIT)), None),
               SETTLE_S + 30, what=f"the runner to hold {s1.short} (its claude was killed)",
               describe=lambda: _skips(am, s1, marks[2]))
    _assert_no_turn(am, "revived", marks[3], 15, "no claude runs")
    assert "revived" not in am.window.terminal.text(), "the queued prompt was typed into the shell"
    _restart_session(am, s1)
    assert any(f"--resume {s1.id}" in c for c in s1.claude_cmdlines()), s1.claude_cmdlines()
    e = am.mock.wait_prompt("revived", since=marks[3], timeout_s=RUNNER_WAIT_S)
    assert e.get("n_msgs", 0) > 1, e
    assert len(_turns(am, "revived", marks[3])) == 1


# ---- LIF-04 · Close, then Resume here ------------------------------------------------------------------------------------


@pytest.mark.p0
@pytest.mark.flow("LIF-04")
@pytest.mark.flow("PQR-10")
@pytest.mark.slow
def test_close_keeps_the_session_and_resume_here_brings_it_back_with_its_queue(am, s2):
    """LIF-04 phases 1–4 and PQR-10 phase 3: a session with a queued prompt is closed inside the prompt's settle window
    (the runner on Auto, but the window keeps the prompt for 40 s, and a closed session asks the runner nothing): its
    claude exits, its card leaves the board, nothing is sent. The Sessions page lists it; Resume here opens it in a tab
    again with `--resume <id>` — the same conversation — with its queue and its mode (Auto) back, so the queued prompt
    goes once, into the resumed conversation. Auto is both the session's own mode and the cog's default, so this holds
    on every build; the Paused case — a mode the default does not restore — is the next test (PQR-K8).

    Args:
        am: The app.
        s2: F-S2 (t2 keeps the window from emptying).
    """
    a, _b = s2
    marks = _marks(am)
    pane = am.window.queue.open(a.title)
    assert pane.mode() == "Auto", pane.mode()
    t_queue = pane.queue('later [[mock reply "resumed and sent"]]')
    a.select()
    am.window.tabs.close(a.title, "Close")
    wait_until(lambda: not a.claude_pids(), 30, what="the closed session's claude to exit")
    am.profile.hooks.wait(rf"\[archive\] {a.id}", since=marks[0], timeout_s=30)
    assert time.time() - t_queue < SETTLE_S - EARLY_S, "the close took longer than the settle window: the test is void"
    page = am.window.manager.open()
    wait_until(lambda: all(c.title != a.title for c in page.cards()), 15, what="the closed session's card to leave")
    _assert_no_turn(am, "later [[", marks[3], max(0.0, SETTLE_S + 5 - (time.time() - t_queue)), "the session is closed")
    sessions = am.window.sessions.open()
    assert sessions.resume(a.title) == a.short  # the same conversation, not a fresh one
    am.window.tabs.wait_tab(a.title, timeout_s=30)
    a.select()  # a tab opened in the background starts its claude when it is first shown
    a.wait_running(timeout_s=60)
    assert any(f"--resume {a.id}" in c for c in a.claude_cmdlines()), a.claude_cmdlines()
    pane = am.window.queue.open(a.title)
    assert [r.label[:5] for r in pane.upcoming()] == ["later"], pane.list_texts()
    assert pane.mode() == "Auto", pane.mode()
    e = am.mock.wait_prompt("later [[", since=marks[3], timeout_s=RUNNER_WAIT_S)
    assert e.get("n_msgs", 0) > 1, e  # into the resumed conversation
    time.sleep(3)  # a second delivery would have reached the mock by now
    assert len(_turns(am, "later [[", marks[3])) == 1


@pytest.mark.p0
@pytest.mark.flow("LIF-04")
@pytest.mark.flow("PQR-15")
@pytest.mark.needs_fix("PQR-K8")
@pytest.mark.slow
def test_resume_here_keeps_the_sessions_paused_mode(am, s2):
    """LIF-04 E1 (PQR-K8's fix, `needs_fix`): the runner's mode is per session and persisted (the owner's rule of
    2026-10-26: "we need the mode per session persisted"), so a session closed Paused with a queued prompt comes back
    Paused from the Sessions page's Resume here: nothing is sent by itself past a whole settle window once its claude
    runs; when the user switches it to Auto, the prompt goes once, into the resumed conversation.

    On 0.8.0 the restore re-stamps the cog's default (Auto) and the prompt goes by itself (PQR-K8): this test fails
    there by design (`-m "not needs_fix"` deselects it).

    Args:
        am: The app.
        s2: F-S2 (t2 keeps the window from emptying).
    """
    a, _b = s2
    marks = _marks(am)
    pane = am.window.queue.open(a.title)
    assert pane.click_toggle() == "Paused"
    pane.queue('kept [[mock reply "sent when the user says so"]]')
    a.select()
    am.window.tabs.close(a.title, "Close")
    wait_until(lambda: not a.claude_pids(), 30, what="the closed session's claude to exit")
    am.profile.hooks.wait(rf"\[archive\] {a.id}", since=marks[0], timeout_s=30)
    sessions = am.window.sessions.open()
    assert sessions.resume(a.title) == a.short
    am.window.tabs.wait_tab(a.title, timeout_s=30)
    a.select()
    a.wait_running(timeout_s=60)
    pane = am.window.queue.open(a.title)
    assert [r.label[:4] for r in pane.upcoming()] == ["kept"], pane.list_texts()
    assert pane.mode() == "Paused", f"{pane.mode()}: Resume here did not keep the session's own mode (PQR-K8)"
    a.select()
    _assert_no_turn(am, "kept [[", marks[3], HOLD_CHECK_S, "the resumed session's runner is Paused")
    assert not _sends(am, a, marks[2]), _why(am, a, marks)
    pane = am.window.queue.open(a.title)
    assert pane.click_toggle() == "Auto"
    e = am.mock.wait_prompt("kept [[", since=marks[3], timeout_s=RUNNER_WAIT_S)
    assert e.get("n_msgs", 0) > 1, e  # into the resumed conversation
    time.sleep(3)  # a second delivery would have reached the mock by now
    assert len(_turns(am, "kept [[", marks[3])) == 1


# ---- LIF-05 · a hand-typed claude ----------------------------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("LIF-05")
@pytest.mark.slow
def test_a_claude_typed_into_a_shell_tab_is_adopted_and_driven(am, s1):
    """LIF-05 phases 1–4: a claude the user types by hand into a new shell tab — no `--session-id` from Agentmaster, a
    folder nobody pre-trusted — is adopted: the Fleet Observer binds it to the tab by `WT_SESSION` (`[adopt] <id> bound
    via WT_SESSION …`), the tab takes the session's name (its folder's), the session gets a card, and the runner drives
    it like a launched one (an adopted session is exempt from the dormant gate: its claude runs in the user's own
    shell) — a queued prompt is delivered once. Its `/exit` leaves the tab on its shell.

    Measured (pqprobe1 pq07 and run pqr1, 2026-10-24): the `+` tab's shell does not carry the app's per-test environment
    (Windows Terminal builds a new tab's environment from the registry), so the test sets CLAUDE_CONFIG_DIR itself, the
    way a user with a custom config does; Claude Code's workspace-trust question defaults to "No, exit" — Enter alone
    ends claude — so the test picks "Yes, I trust this folder" first; and the bind comes ~2 s after claude starts,
    BEFORE its first prompt (Claude Code 2.1.291's presence heartbeat names the conversation from the start, and the
    observer prefers it), so the log is marked before claude starts.

    Args:
        am: The app.
        s1: F-S (the window's other tab).
    """
    marks = _marks(am)
    w = am.window
    before = len(w.tabs.titles())
    managed_claudes = len(am.claude_processes())  # F-S's own claude
    w.focus()
    inputs.keys("ctrl+shift+t")
    wait_until(lambda: len(w.tabs.titles()) > before, 20, what="a new shell tab")
    w.terminal.wait_text(r"PS [A-Z]:\\[^>]*>", timeout_s=60)
    folder = am.iso.folder("h1")
    exe = Path(os.environ.get("USERPROFILE", r"C:\Users\Docker")) / ".local" / "bin" / "claude.exe"
    w.terminal.focus()
    w.terminal.type(f"$env:CLAUDE_CONFIG_DIR='{am.claude.dir}'; Set-Location '{folder}'; & '{exe}'")
    mark = am.profile.hooks.mark()  # before claude starts: the observer binds it before its first prompt
    inputs.keys("enter")
    # Claude Code's workspace-trust question: its first option ("No, exit") is the selected one.
    w.terminal.wait_text(r"Yes, I trust this folder", timeout_s=90)
    inputs.keys("down")
    w.terminal.wait_text(r"❯ Yes, I trust this folder", timeout_s=10)
    inputs.keys("enter")
    w.terminal.wait_input_box(timeout_s=60)
    w.terminal.submit('hello [[mock reply "hi from the shell tab"]]')
    e = am.mock.wait_prompt("hello [[", since=marks[3], timeout_s=90)
    sid = e.get("session", "")
    assert sid, e
    am.profile.hooks.wait(rf"\[adopt\] {sid} bound via WT_SESSION ", since=mark, timeout_s=90)
    rec = am.profile.session(sid)
    assert rec is not None and rec.title == "h1", rec  # the tab took the folder's name
    s = Session(am, rec.id, folder, am.mock)
    w.tabs.wait_tab("h1", timeout_s=30)
    am.window.side_panel.wait_card("h1", timeout_s=60)
    am.window.queue.open("h1").queue('driven [[mock reply "sent by the runner"]]')
    am.mock.wait_prompt("driven", since=marks[3], timeout_s=RUNNER_WAIT_S)
    time.sleep(3)
    assert len(_turns(am, "driven", marks[3])) == 1
    w.tabs.select("h1")
    w.terminal.submit("/exit")
    # The hand-typed claude's command line names no session id: count the app's claudes instead (it runs under the
    # tab's shell, which is the GUI's child like every tab's).
    wait_until(lambda: len(am.claude_processes()) <= managed_claudes, 60, what="the hand-typed claude to exit")
    w.terminal.wait_text(r"PS [A-Z]:\\[^>]*h1>", timeout_s=60)
    assert s.id == sid


# ---- LIF-06 · a never-prompted session across a restart -------------------------------------------------------------------


@pytest.mark.p1
@pytest.mark.flow("LIF-06")
def test_a_never_prompted_session_comes_back_fresh_after_a_restart(am):
    """LIF-06 phases 1–2 (Rule #6): a session launched and never prompted has no transcript, so a reopen cannot resume
    it: the restored tab starts a FRESH claude (`[restore-fresh] <new id> "<title>"`, a new `--session-id`, never
    `--resume`), in the same folder and under the same title, and the stale record is dropped (measured, run pqr1: the
    old id is gone from sessions.json — so the title is read before the restart).

    Args:
        am: The app.
    """
    s = am.launch("t1", prompt=None)
    title = s.title  # read now: the restore-fresh drops the old record
    am.window.side_panel.wait_card(title, state="idle", timeout_s=60)
    assert am.claude.transcript_path(s.id) is None
    mark = am.profile.hooks.mark()
    am.restart()
    m = am.profile.hooks.wait(r"\[restore-fresh\] ([0-9a-f-]{36}) \"(.*?)\"", since=mark, timeout_s=60)
    new_id = m.group(1)
    assert new_id != s.id and m.group(2) == title, m.group(0)
    am.window.tabs.wait_tab(title, timeout_s=30)
    claudes = wait_until(lambda: [" ".join(p.cmdline()) for p in am.claude_processes()] or None, 60,
                         what="the restored tab's claude")
    assert all("--resume" not in c and s.id not in c for c in claudes), claudes
    assert any(f"--session-id {new_id}" in c for c in claudes), claudes
    wait_until(lambda: am.profile.session(new_id), 15, what=f"the fresh session {new_id[:8]} in sessions.json")
    assert am.profile.session(s.id) is None, "the never-prompted record was kept"
