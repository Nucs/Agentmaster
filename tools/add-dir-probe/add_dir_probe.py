"""Live probe: what Claude Code's /add-dir does in a real TUI, so Agentmaster can answer its menu from Telegram.

Drives a real claude.exe on a ConPTY (the repository's claude-wrap-ConPTY driver) in a throwaway folder and records,
for each scenario, the SCREEN (the exact text of the "Add directory to workspace" menu), the TRANSCRIPT lines the
command leaves (and WHEN: before or after the answer), and every hook Claude Code fires (a logging hook on
UserPromptSubmit, UserPromptExpansion, DirectoryAdded, Notification and ConfigChange, log_hook.py). Agentmaster
delivers a queued prompt as a bracketed paste and a CR; the probe types the command the same way.

Scenarios (each judged on what Claude Code wrote, never on a guess):
  menu-1    /add-dir <dir> -> the menu -> key "1" (Yes, for this session)
  menu-2    /add-dir <dir> -> the menu -> key "2" (Yes, and remember this directory: .claude/settings.local.json)
  menu-3    /add-dir <dir> -> the menu -> key "3" (No)
  menu-esc  /add-dir <dir> -> the menu -> Esc
  already   /add-dir <a dir menu-1 added> -> a message, no menu
  missing   /add-dir <no such folder> -> a message, no menu
  bare      /add-dir alone -> the path input ("Enter the path to the directory:") -> Esc
First run: 2026-10-19, Claude Code 2.1.291 (doc/agentmaster/MESSAGING.md §21.34). Re-run it after a Claude Code upgrade
that touches /add-dir or the select menus: Agentmaster's menu detector (TuiMenus.h) keys on the texts it records.

Usage:
    python -I add_dir_probe.py <empty-scratch-folder>
It writes report.json, the screens (screen-<scenario>-<step>.txt) and the hook log into the folder, and runs claude
in <folder>\\ws (CLAUDE_CODE_SANDBOXED=1, so the folder needs no workspace-trust entry). It makes no model call: every
command is local. It leaves a real transcript under ~/.claude/projects for that folder.

Exit code: 0 when every scenario saw what it expected, 1 otherwise.
"""
import argparse
import json
import sys
import time
from pathlib import Path

# The driver lives in the repository's skill folder: <repo>/.claude/skills/claude-wrap-ConPTY/scripts.
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / ".claude" / "skills" / "claude-wrap-ConPTY" / "scripts"))
from conpty_claude import ClaudeWrap  # noqa: E402

TITLE = "Add directory to workspace"
HOOK_EVENTS = ["UserPromptSubmit", "UserPromptExpansion", "DirectoryAdded", "Notification", "ConfigChange"]


def say(msg):
    """Print one progress line with the wall clock.

    Args:
        msg: the line.
    """
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class Probe:
    """The probe's folders and files, and the readers of what Claude Code left there.

    Attributes:
        root: the scratch folder given on the command line.
        ws: the folder claude runs in.
        log: the hook log (JSONL, log_hook.py).
        settings: the --settings file holding the logging hooks.
        report: the per-scenario records (written to report.json at the end).
    """

    def __init__(self, root):
        """Lay out the folders under `root`.

        Args:
            root: the scratch folder (created when missing).
        Raises:
            OSError when a folder cannot be created.
        """
        self.root = Path(root).resolve()
        self.ws = self.root / "ws"
        self.log = self.root / "hooklog.jsonl"
        self.settings = self.root / "settings.json"
        self.report = {}
        self.ws.mkdir(parents=True, exist_ok=True)

    def target(self, name):
        """A folder to add: outside ws, so it is not accessible already.

        Args:
            name: the folder's name under the scratch folder.
        Returns:
            Its absolute path (created).
        """
        d = self.root / "targets" / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def write_settings(self):
        """Write the --settings file: one exec-form logging hook per event of HOOK_EVENTS.

        Raises:
            OSError when the file cannot be written.
        """
        hook = str(Path(__file__).resolve().parent / "log_hook.py")
        hooks = {ev: [{"hooks": [{"type": "command", "command": sys.executable, "args": ["-I", hook, ev], "timeout": 30}]}]
                 for ev in HOOK_EVENTS}
        self.settings.write_text(json.dumps({"hooks": hooks}, indent=2), encoding="utf-8")

    def hooks(self):
        """Every hook record so far.

        Returns:
            The parsed records, oldest first.
        """
        if not self.log.exists():
            return []
        return [json.loads(l) for l in self.log.read_text(encoding="utf-8").splitlines() if l.strip()]

    def save(self, w, scenario, step):
        """Save the screen as it is now (rows unstripped, so the layout reads as on the terminal).

        Args:
            w: the ClaudeWrap.
            scenario: the scenario's name.
            step: what moment it is ("menu", "after", ...).
        Returns:
            The screen text (stripped of trailing blanks per line).
        """
        text = w.screen(strip=True)
        (self.root / f"screen-{scenario}-{step}.txt").write_text(w.screen(strip=False), encoding="utf-8")
        return text


def transcript(w):
    """The session's transcript lines so far ([] before claude wrote one).

    Args:
        w: the ClaudeWrap.
    Returns:
        The parsed JSON lines.
    """
    try:
        return w.transcript_lines()
    except Exception:  # noqa: BLE001 - no transcript yet reads as none
        return []


def brief(line):
    """A transcript line in short: its type, subtype and the text it carries, for the report.

    Args:
        line: one parsed transcript line.
    Returns:
        A dict with the fields that tell what the line is.
    """
    msg = line.get("message") or {}
    content = msg.get("content")
    if isinstance(content, list):
        content = " | ".join(str(c.get("text") or c.get("content") or c.get("type")) for c in content if isinstance(c, dict))
    return {"type": line.get("type"), "subtype": line.get("subtype"), "isMeta": line.get("isMeta"),
            "content": (content or line.get("content") or "")[:400], "timestamp": line.get("timestamp")}


def run_menu(p, w, name, folder, key, expect):
    """One menu scenario: type /add-dir <folder>, read the menu, press `key`, read what follows.

    Args:
        p: the Probe.
        w: the ClaudeWrap.
        name: the scenario's name.
        folder: the folder to add.
        key: what to press on the menu ("1", "2", "3" or ESC).
        expect: a text the screen must show after the answer.
    Returns:
        True when the menu showed and the expected text followed.
    """
    rec = {"folder": str(folder), "key": repr(key)}
    lines0 = len(transcript(w))
    hooks0 = len(p.hooks())
    w.send_prompt(f"/add-dir {folder}")
    try:
        w.wait_for(lambda s: TITLE in s, timeout=20)
    except Exception as e:  # noqa: BLE001
        rec["error"] = f"no menu: {e}"
        p.save(w, name, "nomenu")
        p.report[name] = rec
        return False
    time.sleep(0.8)
    rec["menu_screen"] = p.save(w, name, "menu")
    rec["input_box_ready_during_menu"] = w.input_box_ready()
    rec["transcript_before_answer"] = [brief(l) for l in transcript(w)[lines0:]]
    rec["hooks_before_answer"] = [h["ev"] for h in p.hooks()[hooks0:]]
    w.send_raw(key)
    try:
        w.wait_for(lambda s: TITLE not in s and expect in s, timeout=20)
        ok = True
    except Exception as e:  # noqa: BLE001
        rec["error"] = f"after the answer: {e}"
        ok = False
    w.wait_quiet(quiet=1.5, timeout=20)
    time.sleep(1.5)
    rec["after_screen"] = p.save(w, name, "after")
    rec["transcript_after_answer"] = [brief(l) for l in transcript(w)[lines0:]]
    rec["hooks_after_answer"] = [{"ev": h["ev"], "payload": h["payload"]} for h in p.hooks()[hooks0:]]
    p.report[name] = rec
    return ok


def run_message(p, w, name, arg, expect):
    """A scenario with no menu: type /add-dir <arg> and read the message it leaves.

    Args:
        p: the Probe.
        w: the ClaudeWrap.
        name: the scenario's name.
        arg: what follows /add-dir ("" for the bare command).
        expect: a text the screen must show.
    Returns:
        True when the expected text showed.
    """
    rec = {"arg": arg}
    lines0 = len(transcript(w))
    hooks0 = len(p.hooks())
    w.send_prompt(f"/add-dir {arg}".rstrip())
    try:
        w.wait_for(lambda s: expect in s, timeout=20)
        ok = True
    except Exception as e:  # noqa: BLE001
        rec["error"] = str(e)
        ok = False
    time.sleep(1.0)
    rec["screen"] = p.save(w, name, "shown")
    rec["menu_title_shown"] = TITLE in w.screen()
    if arg == "":
        # The bare command opens a path INPUT, not the menu: leave it with Esc.
        w.send_raw("\x1b")
        w.wait_quiet(quiet=1.5, timeout=20)
        rec["screen_after_esc"] = p.save(w, name, "after")
    w.wait_quiet(quiet=1.5, timeout=20)
    time.sleep(1.5)
    rec["transcript"] = [brief(l) for l in transcript(w)[lines0:]]
    rec["hooks"] = [{"ev": h["ev"], "payload": h["payload"]} for h in p.hooks()[hooks0:]]
    p.report[name] = rec
    return ok


def main():
    """Run every scenario against one claude, then write report.json.

    Returns:
        0 when every scenario passed, 1 otherwise.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", help="an empty scratch folder for the probe's files")
    a = ap.parse_args()
    p = Probe(a.folder)
    p.write_settings()
    env = {"PROBE_HOOK_LOG": str(p.log), "CLAUDE_CODE_SANDBOXED": "1"}
    args = ["--settings", str(p.settings), "--dangerously-skip-permissions"]
    verdict = {}
    with ClaudeWrap(cwd=str(p.ws), args=args, env_overrides=env, cols=160, rows=50) as w:
        w.wait_ready(timeout=90)
        say(f"ready, claude pid {w.pid}")
        t1 = p.target("one")
        verdict["menu-1"] = run_menu(p, w, "menu-1", t1, "1", "for this session")
        verdict["menu-2"] = run_menu(p, w, "menu-2", p.target("two"), "2", "saved to local settings")
        verdict["menu-3"] = run_menu(p, w, "menu-3", p.target("three"), "3", "Did not add")
        verdict["menu-esc"] = run_menu(p, w, "menu-esc", p.target("four"), "\x1b", "Did not add")
        verdict["already"] = run_message(p, w, "already", str(t1), "already")
        verdict["missing"] = run_message(p, w, "missing", str(p.root / "targets" / "nope"), "was not found")
        verdict["bare"] = run_message(p, w, "bare", "", "Enter the path to the directory")
        for name, ok in verdict.items():
            say(f"{name}: {'PASS' if ok else 'FAIL'}")
        p.report["session"] = w.resolve_session()
        p.report["transcript_path"] = str(w.transcript_path())
    local = p.ws / ".claude" / "settings.local.json"
    p.report["settings_local"] = local.read_text(encoding="utf-8") if local.exists() else None
    p.report["verdict"] = verdict
    (p.root / "report.json").write_text(json.dumps(p.report, indent=2, ensure_ascii=False), encoding="utf-8")
    say(f"VERDICT {verdict}")
    return 0 if verdict and all(verdict.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
