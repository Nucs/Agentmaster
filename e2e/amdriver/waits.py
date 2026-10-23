"""Waiting on conditions — the suite's only way to wait (E2E_TESTING.md principle: event waits, never sleeps).

Every UI or engine change Agentmaster makes is asynchronous (a dispatcher hop, a 2.5 s scanner tick, a
debounced save), so a test asks "until when?" instead of sleeping a guessed time: a wait returns the moment the
condition holds, and a timeout says WHAT was awaited and what was last seen.
"""
from __future__ import annotations

import time
from typing import Any, Callable, Optional, TypeVar

T = TypeVar("T")


class WaitTimeout(AssertionError):
    """A condition did not hold in time. An AssertionError, so pytest reports it as a test failure (not an
    error in the harness) with the description of what was awaited."""


class Abort(Exception):
    """Raised inside a waited-on condition to end the wait AT ONCE: what is awaited can no longer happen (the
    app exited, a dialog the flow forbids appeared). wait_until re-raises it unchanged instead of counting it
    as "not yet" like every other exception."""


def wait_until(fn: Callable[[], T], timeout_s: float = 10.0, interval_s: float = 0.2, what: str = "",
               describe: Optional[Callable[[], Any]] = None) -> T:
    """Call `fn` until it returns a truthy value.

    Args:
        fn: The condition. Exceptions it raises count as "not yet" (the UI may be mid-rebuild: an element can
            vanish between finding and reading it); the last one is reported on timeout. Abort ends the wait.
        timeout_s: How long.
        interval_s: The pause between calls.
        what: A description of the condition for the timeout message.
        describe: Called on timeout to add "what was seen" to the message (e.g. the tab titles).
    Returns:
        The first truthy value `fn` returned.
    Raises:
        WaitTimeout: When it never held.
        Abort: When `fn` raised it.
    """
    end = time.monotonic() + timeout_s
    last_exc: Optional[BaseException] = None
    last_value: Any = None
    while True:
        try:
            last_value = fn()
            if last_value:
                return last_value
            last_exc = None
        except Abort:
            raise
        except Exception as error:  # noqa: BLE001 - "not yet" for any read error; reported on timeout
            last_exc = error
        if time.monotonic() >= end:
            break
        time.sleep(interval_s)
    seen = ""
    if describe is not None:
        try:
            seen = f"; seen: {describe()!r}"
        except Exception as error:  # noqa: BLE001 - the description is best effort
            seen = f"; (describe failed: {error})"
    cause = f"; last error: {type(last_exc).__name__}: {last_exc}" if last_exc else f"; last value: {last_value!r}"
    raise WaitTimeout(f"timed out after {timeout_s:.1f} s waiting for {what or fn!r}{cause}{seen}")


def hold_true(fn: Callable[[], Any], duration_s: float, interval_s: float = 0.2, what: str = "") -> None:
    """Check that a condition STAYS true for a while (the negative twin of wait_until: "nothing happens").

    Args:
        fn: The condition.
        duration_s: How long it must hold.
        interval_s: The pause between checks.
        what: A description for the failure message.
    Returns:
        None.
    Raises:
        AssertionError: The first time it is false (or raises).
    """
    end = time.monotonic() + duration_s
    while time.monotonic() < end:
        try:
            ok = fn()
        except Exception as error:  # noqa: BLE001 - a read error breaks the "holds" claim too
            raise AssertionError(f"{what or fn!r} stopped holding: {type(error).__name__}: {error}") from error
        if not ok:
            raise AssertionError(f"{what or fn!r} stopped holding after {duration_s - (end - time.monotonic()):.1f} s")
        time.sleep(interval_s)
