"""A session's state over time, as the user sees it and as the engine logs it.

`StateTimeline` samples the session's state word on the window's side panel (the card's "running" /
"waiting-for-you" / "needs-approval" / "error" / "done" / "idle") in a background thread, about every 150 ms, and
keeps every CHANGE with its time. A test then asserts the whole sequence a user would have seen — "running, then
waiting-for-you, with no blip in between" — instead of a single sample at the end, which is how a flicker, a stuck
state or a wrong intermediate state shows up (flows/05 STA-02).

`engine_events` reads the same story from the engine's logs, from a mark: every hook line of the session
(`[Stop] <id> state=2 …` — the state AFTER the event, SessionModels.h's enum order) and every scanner
reconciliation (`[recon-stop] <id> …`, `[recon-error] <id> (message)`), merged in time order. It is the evidence a
failing timeline is read against: which event moved the state, and whether a hook or the scanner did it. Read it
knowing three things (measured 2026-10-23 on 0.8.0):
- an event the SCANNER synthesizes is logged under the hook's name too (a missed Stop as `[Stop]`, a busy heartbeat's
  promotion as `[PostToolUse]`), with its own `[recon-…]` line in scanner.log beside it;
- `[Unknown]` is a change of the session's record that came from no hook (a launch, an observer update, the scanner's
  bookkeeping);
- under load a hook can arrive many seconds late or never (Claude Code cancels a hook after its 10 s timeout and
  records `hook_cancelled` in the transcript), so a test asserts the state's order, never which path moved it.

The side panel is read through UI Automation only (no file is touched), so sampling cannot disturb the app's saves
(flows/20 ROB-K1).
"""
from __future__ import annotations

import re
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import comtypes

from .waits import wait_until

STATE_NAMES = ("Idle", "Running", "WaitingForInput", "NeedsApproval", "Error", "Done")
"""SessionState in its enum order (SessionModels.h): a hook line's `state=N` is an index into this."""
WORD_OF = {"Idle": "idle", "Running": "running", "WaitingForInput": "waiting-for-you",
           "NeedsApproval": "needs-approval", "Error": "error", "Done": "done"}
"""The engine's state name -> the word the side panel, the tree and the board show."""

_HOOK_LINE = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] \[([A-Za-z]+)\] ([0-9a-f-]{36}) state=(\d+)")
_SCAN_LINE = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\] \[(recon-[a-z-]+|decay-waiting)\] ([0-9a-f-]{8,36})(.*)$")


@dataclass
class Change:
    """One change of the sampled state.

    Attributes:
        t: Seconds since the timeline started.
        state: The state word now shown ("" while the card is not readable, e.g. scrolled away or rebuilding).
    """

    t: float
    state: str


@dataclass
class EngineEvent:
    """One line of the engine's story of a session.

    Attributes:
        clock: The line's own time stamp (HH:MM:SS.mmm, local time).
        source: "hook" (hooks.log) or "scanner" (scanner.log).
        tag: The hook event (`Stop`, `UserPromptSubmit` …) or the reconciliation (`recon-stop` …).
        state: For a hook line, the engine's state AFTER it (a STATE_NAMES name); "" for a scanner line.
        detail: The rest of a scanner line (its reason, e.g. "(Prompt is too long)").
    """

    clock: str
    source: str
    tag: str
    state: str = ""
    detail: str = ""

    def __str__(self) -> str:
        """A one-line form for messages.

        Returns:
            The text.
        """
        what = f"{self.tag} -> {self.state}" if self.state else f"{self.tag}{self.detail[:90]}"
        return f"{self.clock} {self.source}:{what}"


class StateTimeline:
    """The state word of one session's side-panel card, sampled in the background.

    Use it around the action under test:

        with StateTimeline(am.window, "t1") as tl:
            s1.ask('go [[mock hold g; reply "done"]]', wait="sent")
            tl.wait("running")
            mock.release("g")
            tl.wait("waiting-for-you")
        assert tl.states() == ["waiting-for-you", "running", "waiting-for-you"]

    Attributes:
        window: The MainWindow whose side panel shows the session (its LOCAL scope lists this window's sessions).
        title: The session's title (the card's title).
        interval_s: The pause between two samples.
    """

    def __init__(self, window, title: str, interval_s: float = 0.15) -> None:
        """Bind to a session's card (nothing is sampled until start()).

        Args:
            window: The MainWindow.
            title: The session's title.
            interval_s: The sampling pause.
        Returns:
            None.
        """
        self.window = window
        self.title = title
        self.interval_s = interval_s
        self._changes: list[Change] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._t0 = 0.0
        self.samples = 0
        """How many samples were taken (a test can check the sampler really ran)."""

    def __enter__(self) -> "StateTimeline":
        """Start sampling.

        Returns:
            self.
        """
        return self.start()

    def __exit__(self, *exc) -> None:
        """Stop sampling.

        Args:
            *exc: The exception, if any (not suppressed).
        Returns:
            None.
        """
        self.stop()

    def _read(self) -> str:
        """One sample: the card's state word.

        Returns:
            The word, or "" when the card cannot be read right now.
        """
        try:
            for card in self.window.side_panel.cards():
                if card.title == self.title:
                    return card.state
        except Exception:  # noqa: BLE001 - a card rebuilt under the read is "unreadable", not a failure
            return ""
        return ""

    def _run(self) -> None:
        """The sampling loop (its own thread, in the COM multithreaded apartment like the main thread).

        Returns:
            None.
        """
        comtypes.CoInitializeEx(comtypes.COINIT_MULTITHREADED)
        try:
            last = None
            while not self._stop.is_set():
                state = self._read()
                self.samples += 1
                if state and state != last:
                    with self._lock:
                        self._changes.append(Change(round(time.monotonic() - self._t0, 2), state))
                    last = state
                self._stop.wait(self.interval_s)
        finally:
            comtypes.CoUninitialize()

    def start(self) -> "StateTimeline":
        """Start the sampling thread (the first sample is the state at the start).

        Returns:
            self.
        """
        self._t0 = time.monotonic()
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name=f"timeline:{self.title}", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> list[Change]:
        """Stop sampling.

        Returns:
            The changes.
        """
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
        return self.changes()

    def changes(self) -> list[Change]:
        """Every change so far (an unreadable sample is left out, so a re-rendering card is no change).

        Returns:
            A copy of the list.
        """
        with self._lock:
            return list(self._changes)

    def states(self) -> list[str]:
        """The sequence of states shown, consecutive repeats folded.

        Returns:
            The words.
        """
        return [c.state for c in self.changes()]

    def current(self) -> str:
        """The latest sampled state.

        Returns:
            The word ("" before the first readable sample).
        """
        ch = self.changes()
        return ch[-1].state if ch else ""

    def seen(self, state: str) -> bool:
        """Whether the state was shown at any time.

        Args:
            state: The word.
        Returns:
            True when sampled at least once.
        """
        return state in self.states()

    def wait(self, state: str, timeout_s: float = 30.0) -> float:
        """Wait until the card shows a state.

        Args:
            state: The word ("running", "waiting-for-you" …).
            timeout_s: How long.
        Returns:
            The time (s since start) of the change into it.
        Raises:
            WaitTimeout: When the card never showed it (the message lists what it showed).
        """
        def now():
            """One look.

            Returns:
                The change time, or None.
            """
            ch = self.changes()
            return ch[-1].t if ch and ch[-1].state == state else None

        return wait_until(now, timeout_s, interval_s=0.05, what=f"{self.title!r} to show {state!r}",
                          describe=self.describe)

    def wait_seen(self, state: str, timeout_s: float = 30.0) -> None:
        """Wait until the card has shown a state at least once (it may have left it again).

        Args:
            state: The word.
            timeout_s: How long.
        Returns:
            None.
        Raises:
            WaitTimeout: When it never showed it.
        """
        wait_until(lambda: self.seen(state), timeout_s, interval_s=0.05,
                   what=f"{self.title!r} to have shown {state!r}", describe=self.describe)

    def hold(self, state: str, duration_s: float) -> None:
        """Check that the card keeps showing a state for a while (nothing else sampled meanwhile).

        Args:
            state: The word it must show the whole time.
            duration_s: How long.
        Returns:
            None.
        Raises:
            AssertionError: When another state is sampled (the message lists the timeline).
        """
        start = len(self.changes())
        if self.current() != state:
            raise AssertionError(f"{self.title!r} shows {self.current()!r}, not {state!r}: {self.describe()}")
        end = time.monotonic() + duration_s
        while time.monotonic() < end:
            later = self.changes()[start:]
            if later:
                raise AssertionError(f"{self.title!r} left {state!r}: {self.describe()}")
            time.sleep(0.05)

    def describe(self) -> str:
        """The timeline as text, for failure messages.

        Returns:
            "t=0.0 waiting-for-you, t=1.2 running, …".
        """
        return ", ".join(f"t={c.t:.1f} {c.state}" for c in self.changes()) + f" ({self.samples} samples)"


def engine_events(profile, session_id: str, hooks_mark: int = 0, scanner_mark: int = 0) -> list[EngineEvent]:
    """The engine's story of one session since two marks: its hook lines and the scanner's reconciliations.

    Args:
        profile: The Profile.
        session_id: The full session id.
        hooks_mark: A hooks.log mark.
        scanner_mark: A scanner.log mark.
    Returns:
        The events, in time order (hook lines carry the state AFTER the event).
    """
    out: list[EngineEvent] = []
    for line in profile.hooks.text(hooks_mark).splitlines():
        m = _HOOK_LINE.match(line)
        if m and m.group(3) == session_id:
            idx = int(m.group(4))
            out.append(EngineEvent(m.group(1), "hook", m.group(2),
                                   STATE_NAMES[idx] if 0 <= idx < len(STATE_NAMES) else f"#{idx}"))
    short = session_id[:8]
    for line in profile.scanner.text(scanner_mark).splitlines():
        m = _SCAN_LINE.match(line)
        if m and m.group(3).startswith(short):
            out.append(EngineEvent(m.group(1), "scanner", m.group(2), "", m.group(4)))
    return sorted(out, key=lambda e: e.clock)


def engine_story(profile, session_id: str, hooks_mark: int = 0, scanner_mark: int = 0) -> str:
    """engine_events as one line of text, for assertion messages.

    Args:
        profile: The Profile.
        session_id: The full session id.
        hooks_mark: A hooks.log mark.
        scanner_mark: A scanner.log mark.
    Returns:
        The events joined with " | ".
    """
    return " | ".join(str(e) for e in engine_events(profile, session_id, hooks_mark, scanner_mark))


@dataclass
class TranscriptLine:
    """One transcript line, reduced to what moves a session's state.

    Attributes:
        type: The line's type (user, assistant, system, attachment, queue-operation …).
        sub: The system subtype, the attachment type or the queue operation ("" otherwise).
        stop: An assistant line's stop_reason ("" otherwise).
        api_error: The line is a synthetic API-error message (isApiErrorMessage).
        text: The first 160 characters of the line's text (an API error's message, a user marker).
        raw: The parsed line.
    """

    type: str
    sub: str = ""
    stop: str = ""
    api_error: bool = False
    text: str = ""
    raw: dict = field(default_factory=dict)


def transcript_kinds(lines: list[dict]) -> list[TranscriptLine]:
    """Reduce transcript lines to their state-relevant kinds.

    Args:
        lines: Parsed transcript lines (ClaudeConfig.transcript).
    Returns:
        One TranscriptLine each, in order.
    """
    out = []
    for o in lines:
        t = str(o.get("type", ""))
        sub = ""
        if t == "system":
            sub = str(o.get("subtype", ""))
        elif t == "attachment":
            sub = str((o.get("attachment") or {}).get("type", ""))
        elif t == "queue-operation":
            sub = str(o.get("operation", ""))
        msg = o.get("message") if isinstance(o.get("message"), dict) else {}
        stop = str(msg.get("stop_reason") or "") if t == "assistant" else ""
        content = msg.get("content")
        if isinstance(content, str):
            text = content
        elif isinstance(content, list):
            text = " ".join(str(b.get("text", "")) for b in content if isinstance(b, dict) and b.get("type") == "text")
        else:
            text = str(o.get("content") or "")
        out.append(TranscriptLine(t, sub, stop, bool(o.get("isApiErrorMessage")), text[:160], o))
    return out
