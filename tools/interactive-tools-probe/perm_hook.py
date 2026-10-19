"""PermissionRequest hook for the interactive-tools probe: log the payload, then long-poll for a reply.

Claude Code runs this when AskUserQuestion or ExitPlanMode opens its dialog (the probe's --settings file
registers it with the matcher "AskUserQuestion|ExitPlanMode"). It stands in for Agentmaster's ask helper
(doc/agentmaster/INTERACTIVE_TOOLS.md §7.2): it records exactly what Claude Code hands a hook, then waits, the
way a phone answer would, until the probe driver drops a reply file for THIS process. It prints that reply as
the hook's JSON output and exits 0. With no reply before the deadline it exits 0 with no output, which
Claude Code reads as "no decision" (the dialog stays with the person at the keyboard).

Environment (the driver sets it on the claude child; hooks inherit claude's environment):
  PROBE_HOOK_LOG   a JSONL file; one record per start / reply / timeout, stamped with the time and the pid.
  PROBE_REPLY_DIR  where the driver writes reply-<pid>.json.
  PROBE_WAIT       seconds to wait for a reply (default 120).
"""
import json
import os
import sys
import time


def log(record):
    """Append one record to the probe log, stamped with the time and this process's pid.

    A probe hook that cannot write its log must still answer or exit cleanly, so a write failure is
    swallowed here on purpose: the driver then sees a missing record and reports the scenario as failed.

    Args:
        record: a JSON-serializable dict; the keys "t" (epoch seconds) and "pid" are added to it.
    Returns:
        None.
    """
    record["t"] = time.time()
    record["pid"] = os.getpid()
    try:
        with open(os.environ["PROBE_HOOK_LOG"], "a", encoding="utf-8") as fh:
            fh.write(json.dumps(record) + "\n")
    except (OSError, KeyError):
        pass


def main():
    """Read the hook payload from stdin, log it, wait for the driver's reply, print it.

    Returns:
        0 always. Claude Code reports any other exit code as a hook error (and 2 as a blocking error);
        printing nothing is the honest "no decision".
    """
    raw = sys.stdin.buffer.read().decode("utf-8", "replace")
    try:
        payload = json.loads(raw)
    except ValueError:
        # Keep the evidence: a payload Claude Code sent that is not JSON is itself a finding.
        payload = {"unparsed": raw[:2000]}
    log({"ev": "start", "payload": payload})
    reply_path = os.path.join(os.environ.get("PROBE_REPLY_DIR", "."), f"reply-{os.getpid()}.json")
    deadline = time.time() + float(os.environ.get("PROBE_WAIT", "120"))
    while time.time() < deadline:
        if os.path.exists(reply_path):
            # The driver writes the file atomically (a rename), so a present file is complete.
            with open(reply_path, encoding="utf-8") as fh:
                reply = fh.read()
            sys.stdout.write(reply)
            sys.stdout.flush()
            log({"ev": "replied", "reply": json.loads(reply)})
            return 0
        time.sleep(0.2)
    log({"ev": "timeout"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
