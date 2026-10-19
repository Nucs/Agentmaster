"""A hook for the /add-dir probe: appends what Claude Code hands it to a JSONL log, and answers nothing.

Claude Code runs it in exec form with the hook's event name as its one argument (add_dir_probe.py writes the
--settings file). It reads the event's JSON payload from stdin, adds the event name and the wall clock, and appends
one line to the file named by the PROBE_HOOK_LOG environment variable. It prints nothing and exits 0, so the hook
never changes what Claude Code does.

Usage (by Claude Code, not by hand):
    python -I log_hook.py <EventName>
"""
import json
import os
import sys
import time


def main():
    """Append one record {"ev", "t", "payload"} to PROBE_HOOK_LOG.

    Returns:
        0 always: a failing hook would show up in the session, and this one must stay invisible.
    Raises:
        Nothing: any failure is swallowed after a best-effort note on stderr (Claude Code logs a hook's stderr in its
        debug log only).
    """
    event = sys.argv[1] if len(sys.argv) > 1 else "?"
    try:
        raw = sys.stdin.buffer.read().decode("utf-8", "replace")
        try:
            payload = json.loads(raw)
        except ValueError:
            payload = {"raw": raw}
        with open(os.environ["PROBE_HOOK_LOG"], "a", encoding="utf-8") as f:
            f.write(json.dumps({"ev": event, "t": time.time(), "payload": payload}, ensure_ascii=False) + "\n")
    except Exception as e:  # noqa: BLE001 - a probe hook must never fail the session
        sys.stderr.write(f"log_hook: {e}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
