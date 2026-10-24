"""Interrupted work goes on (doc/agentmaster/flows/06-interrupted-work.md, INT).

A turn that ended abnormally is continued when its session comes back: Agentmaster appends ONE rewind line to a
transcript whose turn died on an API error (after the old claude exited), and Claude Code — started with
CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 like every claude of the suite (amdriver/app.py) — continues the turn by itself
with the meta prompt "Continue from where you left off.". The mock serves that continuation the scripted turn's NEXT
response (the continuation request carries the turn so far), so a test holds it at a gate and watches the session's
state through it.

The restore wake (INT-04: after a restart the busy tabs start by themselves) is proven together with the cut-off turn
it continues: `test_session_state.py::test_a_turn_cut_off_by_the_app_ending_reads_error_then_the_restore_wake_continues_it`.
"""
import pytest

from amdriver.timeline import StateTimeline, engine_story, transcript_kinds

STATE_WAIT_S = 120.0
"""How long a state may take to show on a busy PC (a hook that times out, then the scanner's settle)."""

CONTINUE_PROMPT = "Continue from where you left off."
"""What Claude Code sends by itself to continue a turn its resume found unfinished (a meta user line)."""


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


@pytest.mark.p0
@pytest.mark.flow("INT-01")
def test_restart_session_continues_a_turn_that_died_on_an_api_error(am, s1):
    """INT-01 phases 1–3: a tool result, then a response cut mid-stream ("API Error: Connection lost mid-response …")
    — error, the runner paused. Restart session: once the old claude has exited, Agentmaster appends ONE line
    (`last-prompt`, `explicit: true`, no `rewound`) aiming the conversation's leaf at the tool result below the failed
    response (`[api-error-rewind] <sid8> restart: rewound past "API Error: Connection lost mid-response …"`); the
    scanner sees the error left behind (`[recon-error-release] … (rewound past error -> Waiting)`), and the new claude
    continues the turn by itself ("Continue from where you left off.", carrying the tool result: the mock serves the
    scripted turn's second response again) — running while that is held, waiting-for-you after it. Nothing is deleted:
    the error line stays in the file, on a dead branch.

    The failed response holds at TWO gates: the first attempt is cut at `g`; `g` is then released, so the
    continuation streams past it and waits at `g2`, where the test sees it running.

    Args:
        am: The app.
        s1: F-S.
    """
    w = am.window
    hooks, scanner, seq = am.profile.hooks.mark(), am.profile.scanner.mark(), am.mock.last_seq()
    runner = am.profile.autorunner.mark()
    start = len(am.claude.transcript(s1.id))
    with StateTimeline(w, s1.title) as tl:
        tl.wait("waiting-for-you", 60)  # at rest on the panel before the action (it can lag the record)
        s1.ask('fix it [[mock bash "echo step1"; say "I will now"; hold g; hold g2; reply "continued after the restart"]]',
               wait="sent", timeout_s=STATE_WAIT_S)
        am.mock.wait_holding("g", timeout_s=STATE_WAIT_S)
        am.mock.drop("g")  # the connection is lost after "I will now"
        tl.wait("error", STATE_WAIT_S)
        am.profile.scanner.wait(rf"\[recon-error\] {s1.id} \(API Error: Connection lost mid-response", since=scanner,
                                timeout_s=30)
        am.profile.autorunner.wait(rf"\[stop-on-error\] paused {s1.id}", since=runner, timeout_s=30)
        am.mock.release("g")  # from now on a hold at g passes: the continuation waits at g2
        w.tabs.context_menu("t1").choose("Restart session")
        am.profile.hooks.wait(rf'\[api-error-rewind\] {s1.short} restart: rewound past "API Error: Connection lost '
                              rf'mid-response', since=hooks, timeout_s=60)
        am.profile.scanner.wait(rf"\[recon-error-release\] {s1.id} \(rewound past error -> Waiting\)", since=scanner,
                                timeout_s=60)
        go_on = am.mock.wait_for(lambda e: e.get("kind") == "main" and e.get("session") == s1.id
                                 and e.get("user_text") == CONTINUE_PROMPT, since=seq, timeout_s=STATE_WAIT_S)
        am.mock.wait_holding("g2", timeout_s=STATE_WAIT_S)
        tl.wait("running", STATE_WAIT_S)
        am.mock.release("g2")
        tl.settled_at("waiting-for-you", STATE_WAIT_S)
    assert _ordered(tl.states(), ["waiting-for-you", "running", "error", "waiting-for-you", "running",
                                  "waiting-for-you"]), \
        f"timeline: {tl.describe()}; engine: {engine_story(am.profile, s1.id, hooks, scanner)}"
    assert go_on.get("k") == 1, f"the continuation did not carry the turn's tool result: {go_on}"
    kinds = transcript_kinds(am.claude.transcript(s1.id)[start:])
    rewinds = [t for t in kinds if t.type == "last-prompt" and t.raw.get("explicit") is True]
    assert len(rewinds) == 1 and "rewound" not in rewinds[0].raw, [t.raw for t in rewinds]
    assert any(t.api_error and "Connection lost mid-response" in t.text for t in kinds), "the error line was deleted"
    assert any(t.type == "assistant" and t.text == "continued after the restart" for t in kinds), \
        [(t.type, t.text) for t in kinds]
