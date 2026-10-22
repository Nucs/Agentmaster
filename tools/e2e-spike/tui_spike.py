"""Spike: the INTERACTIVE Claude Code TUI against mockllm.py, on a ConPTY (the repository's driver).

What Agentmaster hosts is the interactive TUI, not `claude -p`, so the spike drives it the way
.claude/skills/claude-wrap-ConPTY does: spawn on a pseudo-console, render the screen with pyte, paste
prompts, and read the result from the screen and the transcript.

Usage:
    python -I tui_spike.py <port> <config dir> <cwd> firstrun
        Start claude and print the first screen that stays unchanged for 6 s (a first-run modal or the
        ready input box). Answers nothing.
    python -I tui_spike.py <port> <config dir> <cwd> turns <item> [<item> ...]
        Wait for the input box, then process each item in order and print the screen after it:
          <prompt>         send it (paste + Enter) and wait until the turn is over
          NOWAIT:<prompt>  send it and go on at once (for a turn that will stop on a dialog)
          WAITFOR:<regex>  wait up to 30 s for the regex on the screen
          KEYS:<text>      type raw bytes (backslash escapes decoded: KEYS:\\r is Enter)
          QUIET:           wait until the screen has not changed for 1.5 s (before keys into a dialog)
          SHOW:            print the screen

⚠ The two harness rules this spike learned the hard way (2026-10-22):
  1. A WAITFOR regex must not match the PROMPT's own echo: the script is on screen too (`reply "ok"` puts
     "ok" there before any reply exists). Wait for text only the answer can show ("User answered").
  2. Keys sent while a dialog is still rendering are dropped: QUIET: before KEYS: into a dialog.
  Neither is verified by this script: the product driver must check every input's effect on disk (the
  transcript, the mock's request log), as the claude-wrap-ConPTY skill's contract says.

⚠ Call it from PowerShell or with MSYS_NO_PATHCONV=1 from Git Bash: Git Bash rewrites an argument that
starts with "/" into a Windows path ("/compact" arrived as "C:/Program Files/Git/compact").
⚠ First-run seeds (a fresh config dir shows a theme picker and a bypass-permissions warning; README.md
lists the three seeds that remove both).
"""
import os
import sys
import time

#: This folder; the ConPTY driver is found relative to it (the repository's own skill), so the spike runs
#: from any working directory without installing anything.
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", ".claude", "skills", "claude-wrap-ConPTY", "scripts"))
sys.path.insert(0, HERE)
from conpty_claude import ClaudeWrap  # noqa: E402  (the repository's verified ConPTY driver)
from spike_env import CLAUDE, clean_env  # noqa: E402


def show(w, label):
    """Print the rendered screen under a label.

    Args:
        w: The running ClaudeWrap.
        label: A heading for the dump (the time since launch is appended).
    Returns:
        None.
    """
    print("=" * 30, label, "(t=%.1fs)" % (time.time() - w.launched_at))
    print(w.screen())


def first_run(w):
    """Print the first screen that stays unchanged for 6 s (or the screen after 60 s).

    Args:
        w: The started ClaudeWrap.
    Returns:
        None.
    """
    last, stable_since, deadline = None, time.time(), time.time() + 60
    while time.time() < deadline:
        time.sleep(1.0)
        s = w.screen()
        if s != last:
            last, stable_since = s, time.time()
        elif time.time() - stable_since > 6:
            break
    show(w, "settled screen")


def turns(w, items):
    """Wait for the input box, then run the items (see the module docstring).

    Args:
        w: The started ClaudeWrap.
        items: The command-line items.
    Returns:
        None.
    Raises:
        conpty_claude.ModalError: When a startup modal blocks the TUI.
        conpty_claude.ReadyTimeout: When the box never appears, or a turn does not end in 60 s.
        TimeoutError: When a WAITFOR regex does not appear in 30 s.
    """
    w.wait_ready(timeout=40)
    show(w, "ready")
    for item in items:
        t0 = time.time()
        if item.startswith("WAITFOR:"):
            w.wait_for(item[8:], timeout=30)
            show(w, "waited for %r: %.2fs" % (item[8:], time.time() - t0))
        elif item.startswith("KEYS:"):
            w.send_raw(item[5:].encode("utf-8").decode("unicode_escape"))
            time.sleep(0.5)
        elif item.startswith("SHOW:"):
            show(w, "show")
        elif item.startswith("QUIET:"):
            # ⚠ Keys sent while a dialog is still rendering are DROPPED (2026-10-22: an Enter sent the
            # instant AskUserQuestion's "Enter to select" appeared never answered it, and the NEXT
            # prompt's Enter did — so that prompt vanished). Wait for the screen to settle first.
            w.wait_quiet(quiet=1.5, timeout=30)
        elif item.startswith("NOWAIT:"):
            w.wait_idle(timeout=60, quiet=1.0)
            w.send_prompt(item[7:])
        else:
            # ⚠ Never paste the moment a previous turn's text appears: on 2026-10-22 a prompt pasted in the
            # same instant the turn ended (a WAITFOR matched the reply before the turn was over) was LOST —
            # no transcript line, no request. Wait until the turn is over and the screen is quiet.
            w.wait_idle(timeout=60, quiet=1.0)
            w.send_prompt(item)
            time.sleep(0.3)  # let the busy footer appear before waiting for it to go
            w.wait_idle(timeout=60, quiet=1.0)
            show(w, "after %r: %.2fs" % (item[:50], time.time() - t0))


def main(argv):
    """Command line entry (see the module docstring).

    Args:
        argv: <port> <config dir> <cwd> <phase> [items...].
    Returns:
        0. A failure raises.
    Raises:
        SystemExit: On a bad command line.
        Everything turns() raises.
    """
    if len(argv) < 4 or argv[3] not in ("firstrun", "turns"):
        raise SystemExit(__doc__)
    port, cfg, cwd, phase = int(argv[0]), argv[1], argv[2], argv[3]
    # --dangerously-skip-permissions: what Agentmaster passes by default (and the cause of the bypass
    # warning on a fresh config). The ConPTY driver additionally strips the nesting variables.
    w = ClaudeWrap(cwd=cwd, exe=CLAUDE, args=["--dangerously-skip-permissions"], cols=150, rows=42,
                   env=clean_env(port, cfg), strip_nesting_env=True)
    w.start()
    try:
        if phase == "firstrun":
            first_run(w)
        else:
            turns(w, argv[4:])
    finally:
        w.close()  # force-terminates: a leaked claude.exe would hold a live session
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
