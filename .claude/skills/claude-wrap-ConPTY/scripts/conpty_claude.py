"""conpty_claude - drive the Claude Code TUI under a Windows pseudo-console.

Library + CLI. Spawns claude.exe (or any TUI) on a real ConPTY, renders its
ANSI output into a readable screen via pyte, waits on SCREEN CONTENT (never a
blind sleep), injects prompts as bracketed paste, and verifies what landed in
the session transcript on disk.

    from conpty_claude import ClaudeWrap
    with ClaudeWrap(cwd=r"C:\\work\\repo") as w:
        w.wait_ready()                       # raises ModalError on a startup modal
        w.send_prompt("What is 2+2?")
        w.wait_idle()
        print(w.screen())
        print(w.transcript_messages())

CLI:
    python conpty_claude.py probe                     # spawn, dump screen, exit
    python conpty_claude.py send "Hi!" --verify       # full loop + transcript check
    python conpty_claude.py raw --seconds 10          # dump raw escape stream

Requires: pywinpty, pyte  (pip install pywinpty pyte)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Sequence

try:
    import winpty
except ImportError:  # pragma: no cover
    sys.exit("FATAL: pywinpty is not installed.  pip install pywinpty")
try:
    import pyte
except ImportError:  # pragma: no cover
    sys.exit("FATAL: pyte is not installed.  pip install pyte")


# ───────────────────────────── Terminal control codes ─────────────────────────

BRACKET_PASTE_START = "\x1b[200~"
BRACKET_PASTE_END = "\x1b[201~"
CR = "\r"
ESC = "\x1b"
CTRL_C = "\x03"
CTRL_U = "\x15"   # kill to line start
CTRL_S = "\x13"   # Claude's whole-box draft stash TOGGLE (press twice == no-op)
CTRL_Y = "\x19"   # yank back a Ctrl+U kill
KEY_END = "\x1b[F"
KEY_UP = "\x1b[A"
KEY_DOWN = "\x1b[B"
BACKSPACE = "\x7f"

#: Env vars that tell a child "you are a nested/non-interactive Claude".  Left in
#: place, the child can refuse to start its interactive TUI or mis-bind its
#: session.  Stripped from the child env by default.
NESTING_ENV_KEYS = (
    "CLAUDECODE",
    "CLAUDE_CODE_SESSION_ID",
    "CLAUDE_CODE_PARENT_SESSION_ID",
    "CLAUDE_CODE_CHILD_SESSION",
    "CLAUDE_CODE_ENTRYPOINT",
    "CLAUDE_CODE_EXECPATH",
    "CLAUDE_CODE_SSE_PORT",
    "AI_AGENT",
    # Agentmaster / manager stamps - a wrapped child is not a managed tab.
    "AM_SESSION",
    "CCMGR_SESSION_ID",
    "CCMGR_HOOK_PIPE",
)

#: Claude prints `debug\<uuid>.txt` in its startup banner when --debug is on.
_SID_IN_BANNER = re.compile(
    r"debug[\\/]([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"
)
_UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")

#: Modern-terminal sequences pyte does not implement; unparsed, their tails leak
#: onto the rendered screen as stray glyphs (a lone "u" on row 0 is the classic).
#: They carry no cell content, so dropping them before feeding pyte is lossless.
#:   ESC[<u ESC[>1u  kitty keyboard protocol push/pop
#:   ESC[>4;2m       xterm modifyOtherKeys
#:   ESC[>0q         XTVERSION query
#:   ESC[1t          window manipulation
_PYTE_NOISE = re.compile(r"\x1b\[[<>][0-9;]*[a-zA-Z]|\x1b\[[0-9;]*t")


# ───────────────────────────── Startup modals ─────────────────────────────────


@dataclass(frozen=True)
class Modal:
    """A blocking startup dialog that must be answered before the TUI is usable."""

    key: str
    title: str
    #: Screen text that identifies it (all must be present).
    markers: tuple[str, ...]
    #: Keystrokes that dismiss it in the LEAST destructive way, or None if the
    #: only correct fix is outside the wrapper.
    safe_answer: str | None
    remedy: str


#: How long an answered modal may still be on screen before we call the answer
#: rejected (it repaints away in well under a second in practice).
_MODAL_ANSWER_GRACE_S = 6.0

#: Ordered most-specific first.
KNOWN_MODALS: tuple[Modal, ...] = (
    Modal(
        key="settings-error",
        title="Settings Error (malformed JSON)",
        markers=("Settings Error", "Invalid or malformed JSON"),
        safe_answer="3" + CR,  # "Continue without these settings"
        remedy="Fix the JSON in the named settings file (check for merge-conflict "
        "markers, trailing commas). Answering '3' continues WITHOUT those settings, "
        "so the session runs with a different config than you think.",
    ),
    Modal(
        key="workspace-trust",
        title="Workspace trust",
        markers=("Do you trust the files in this folder",),
        safe_answer=None,
        remedy="Pre-seed ~/.claude.json projects.<git-root-or-cwd>.hasTrustDialogAccepted"
        "=true (key is path.normalize'd with \\ -> /, case-sensitive), or launch in an "
        "already-trusted directory. --dangerously-skip-permissions does NOT cover this.",
    ),
    Modal(
        key="external-imports",
        title="External CLAUDE.md imports",
        markers=("external CLAUDE.md file imports",),
        safe_answer=None,
        remedy="An artifact of running with an isolated CLAUDE_CONFIG_DIR: it "
        "reclassifies ~/.claude/CLAUDE.md @imports as project-external. Use the real "
        "config dir, or pre-seed hasClaudeMdExternalIncludesApproved=true.",
    ),
    Modal(
        key="bypass-permissions",
        title="Bypass Permissions mode acceptance",
        markers=("Bypass Permissions mode",),
        safe_answer=None,
        remedy="One-time global acceptance stored as ~/.claude.json "
        "bypassPermissionsModeAccepted. Accept it once by hand, or drop "
        "--dangerously-skip-permissions.",
    ),
    Modal(
        key="theme-picker",
        title="First-run theme picker",
        markers=("Choose the option that looks best",),
        safe_answer=CR,
        remedy="First-run onboarding on a fresh config dir. Answering picks the "
        "default theme and persists it.",
    ),
    Modal(
        key="login",
        title="Not logged in",
        markers=("Select login method",),
        safe_answer=None,
        remedy="The child has no credentials. Run `claude` by hand once and log in.",
    ),
)


class ModalError(RuntimeError):
    """Raised when a startup modal blocks the TUI."""

    def __init__(self, modal: Modal, screen: str):
        self.modal = modal
        self.screen = screen
        super().__init__(
            f"startup modal blocked the TUI: {modal.title}\n"
            f"  remedy: {modal.remedy}\n"
            f"--- screen ---\n{screen}"
        )


class ReadyTimeout(RuntimeError):
    """The TUI never reached an interactive prompt."""


# ───────────────────────────── The wrapper ────────────────────────────────────


@dataclass
class ClaudeWrap:
    """A claude.exe (or arbitrary TUI) running on a ConPTY, with a rendered screen."""

    cwd: str = field(default_factory=os.getcwd)
    exe: str | None = None
    args: Sequence[str] = ()
    cols: int = 160
    rows: int = 45
    env: dict[str, str] | None = None
    #: Extra env vars to set for the child (applied after nesting-key stripping).
    env_overrides: dict[str, str] = field(default_factory=dict)
    strip_nesting_env: bool = True
    echo: bool = False  # mirror the child's raw stream to our stderr (debug only)

    # runtime
    proc: "winpty.PtyProcess | None" = field(default=None, init=False, repr=False)
    session_id: str | None = field(default=None, init=False)
    launched_at: float = field(default=0.0, init=False)
    _raw: list[str] = field(default_factory=list, init=False, repr=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, init=False, repr=False)
    _stop: threading.Event = field(default_factory=threading.Event, init=False, repr=False)
    _last_byte: float = field(default=0.0, init=False, repr=False)
    _screen: "pyte.Screen | None" = field(default=None, init=False, repr=False)
    _stream: "pyte.Stream | None" = field(default=None, init=False, repr=False)
    _before_transcripts: set[str] = field(default_factory=set, init=False, repr=False)

    # ── lifecycle ────────────────────────────────────────────────────────────

    def __enter__(self) -> "ClaudeWrap":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def resolve_exe(self) -> str:
        if self.exe:
            return self.exe
        found = which_claude()
        if not found:
            raise FileNotFoundError(
                "claude.exe not found. Pass exe=..., or install to ~/.local/bin."
            )
        return found

    def child_env(self) -> dict[str, str]:
        base = dict(self.env if self.env is not None else os.environ)
        if self.strip_nesting_env:
            for k in NESTING_ENV_KEYS:
                base.pop(k, None)
        base.update(self.env_overrides)
        return base

    def start(self) -> "ClaudeWrap":
        exe = self.resolve_exe()
        self._before_transcripts = {
            p.name for p in project_dir_for(self.cwd).glob("*.jsonl")
        }
        self._screen = pyte.Screen(self.cols, self.rows)
        self._screen.set_mode(pyte.modes.LNM)
        self._stream = pyte.Stream(self._screen)
        self.launched_at = time.time()
        argv = [exe, *self.args]
        self.proc = winpty.PtyProcess.spawn(
            argv if len(argv) > 1 else argv[0],
            cwd=self.cwd,
            env=self.child_env(),
            dimensions=(self.rows, self.cols),
        )
        self._last_byte = time.time()
        threading.Thread(target=self._reader, daemon=True).start()
        return self

    def _reader(self) -> None:
        # pywinpty's read() BLOCKS, so it must own a thread; the main thread only
        # ever inspects the rendered screen.
        while not self._stop.is_set():
            try:
                chunk = self.proc.read(8192)
            except Exception:
                break
            if not chunk:
                time.sleep(0.02)
                continue
            if self.echo:
                sys.stderr.write(chunk)
                sys.stderr.flush()
            with self._lock:
                self._raw.append(chunk)
                self._last_byte = time.time()
                self._stream.feed(_PYTE_NOISE.sub("", chunk))
                if self.session_id is None:
                    m = _SID_IN_BANNER.search(chunk)
                    if m:
                        self.session_id = m.group(1)

    def close(self, force: bool = True) -> None:
        self._stop.set()
        p, self.proc = self.proc, None
        if p is None:
            return
        try:
            p.terminate(force=force)
        except Exception:
            pass

    @property
    def alive(self) -> bool:
        try:
            return bool(self.proc and self.proc.isalive())
        except Exception:
            return False

    @property
    def pid(self) -> int | None:
        return getattr(self.proc, "pid", None)

    # ── observation ──────────────────────────────────────────────────────────

    def screen(self, strip: bool = True) -> str:
        """The rendered screen as text (what a human would SEE)."""
        with self._lock:
            lines = list(self._screen.display)
        if strip:
            lines = [ln.rstrip() for ln in lines]
            while lines and not lines[-1]:
                lines.pop()
        return "\n".join(lines)

    def raw(self) -> str:
        """Every byte the child emitted, escape sequences included."""
        with self._lock:
            return "".join(self._raw)

    def quiet_for(self) -> float:
        """Seconds since the child last wrote a byte."""
        with self._lock:
            return time.time() - self._last_byte

    def find_modal(self) -> Modal | None:
        s = self.screen()
        for m in KNOWN_MODALS:
            if all(marker in s for marker in m.markers):
                return m
        return None

    # ── waiting ──────────────────────────────────────────────────────────────

    def wait_for(
        self,
        predicate: str | Callable[[str], bool],
        timeout: float = 30.0,
        poll: float = 0.15,
    ) -> str:
        """Block until the SCREEN satisfies predicate (regex str or callable)."""
        if isinstance(predicate, str):
            rx = re.compile(predicate, re.S)
            test: Callable[[str], bool] = lambda s: bool(rx.search(s))
            label = predicate
        else:
            test, label = predicate, getattr(predicate, "__name__", "predicate")
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = self.screen()
            if test(s):
                return s
            if not self.alive:
                raise ReadyTimeout(f"child exited while waiting for {label!r}\n{s}")
            time.sleep(poll)
        raise ReadyTimeout(f"timed out after {timeout}s waiting for {label!r}\n{self.screen()}")

    def wait_quiet(self, quiet: float = 1.5, timeout: float = 30.0) -> None:
        """Block until the child has emitted nothing for `quiet` seconds."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.quiet_for() >= quiet:
                return
            time.sleep(0.1)
        raise ReadyTimeout(f"child never went quiet for {quiet}s within {timeout}s")

    def wait_ready(
        self,
        timeout: float = 45.0,
        quiet: float = 1.0,
        answer_modals: Iterable[str] | bool = False,
    ) -> str:
        """Block until the interactive input box is rendered and the TUI settled.

        Raises ModalError on a known startup modal unless `answer_modals` names it
        (or is True, meaning "answer any modal that has a safe answer").
        """
        deadline = time.time() + timeout
        answered: dict[str, float] = {}  # key -> when we sent the answer
        while time.time() < deadline:
            if not self.alive:
                raise ReadyTimeout(f"child exited before becoming ready\n{self.screen()}")
            modal = self.find_modal()
            if modal is not None:
                allow = (
                    answer_modals is True
                    or (answer_modals and modal.key in set(answer_modals))
                )
                if allow and modal.safe_answer:
                    sent_at = answered.get(modal.key)
                    if sent_at is None:
                        answered[modal.key] = time.time()
                        self.send_raw(modal.safe_answer)
                        time.sleep(0.5)
                        continue
                    # An answered modal needs a moment to repaint away; only give
                    # up once it is STILL on screen past the grace window.
                    if time.time() - sent_at < _MODAL_ANSWER_GRACE_S:
                        time.sleep(0.25)
                        continue
                raise ModalError(modal, self.screen())
            if self.input_box_ready() and self.quiet_for() >= quiet:
                return self.screen()
            time.sleep(0.2)
        raise ReadyTimeout(
            f"TUI never reached an input prompt in {timeout}s\n--- screen ---\n{self.screen()}"
        )

    def input_box_ready(self) -> bool:
        """True when Claude's input box is on screen.

        The box is the bottom-most `>` prompt line WRAPPED by flush-left horizontal
        rules - the same two cues that separate a live input box from an already-sent
        prompt (inline, no box) or a menu selection caret (the line above is the
        question, not a rule).
        """
        lines = [ln.rstrip() for ln in self.screen(strip=False).split("\n")]
        rule_rows = [i for i, ln in enumerate(lines) if _is_rule(ln)]
        if len(rule_rows) < 2:
            return False
        for i, ln in enumerate(lines):
            if not _is_prompt_line(ln):
                continue
            above = [r for r in rule_rows if r < i]
            below = [r for r in rule_rows if r > i]
            if above and below:
                return True
        return False

    def is_busy(self) -> bool:
        """True while a turn is running.

        Keyed on the footer's `esc to interrupt` affordance, which Claude shows for
        exactly as long as the turn can be cancelled. Do NOT key this on the input
        box: the box stays rendered mid-turn (it accepts type-ahead), so
        `input_box_ready()` is True during a turn too.
        """
        return "esc to interrupt" in self.screen()

    def wait_idle(self, timeout: float = 300.0, quiet: float = 1.5) -> str:
        """Block until the turn finishes: not busy + input box back + stream quiet.

        NOTE this is a SCREEN heuristic - fine for a test harness. Production code
        should take turn state from hooks or the transcript tail, never the screen.
        """
        deadline = time.time() + timeout
        # A turn takes a moment to start; don't mistake the pre-start lull for done.
        started = False
        while time.time() < deadline:
            if not self.alive:
                raise ReadyTimeout(f"child exited mid-turn\n{self.screen()}")
            busy = self.is_busy()
            started = started or busy
            if not busy and self.input_box_ready() and self.quiet_for() >= quiet:
                if started or self.quiet_for() >= quiet + 2.0:
                    return self.screen()
            time.sleep(0.25)
        raise ReadyTimeout(f"turn did not finish in {timeout}s\n{self.screen()}")

    # ── input ────────────────────────────────────────────────────────────────

    def send_raw(self, data: str) -> None:
        """Write bytes to the child's stdin verbatim."""
        if not self.proc:
            raise RuntimeError("not started")
        self.proc.write(data)

    def fill(self, text: str) -> None:
        """Type text into the input box as ONE bracketed paste - NO submit."""
        self.send_raw(BRACKET_PASTE_START + text + BRACKET_PASTE_END)

    def submit(self) -> None:
        """Press Enter."""
        self.send_raw(CR)

    def send_prompt(self, text: str, settle: float = 0.35) -> None:
        """Fill + submit as one atomic delivery (the standard injection)."""
        self.fill(text)
        time.sleep(settle)  # let the TUI absorb the paste before the CR
        self.submit()

    def send_keys(self, *keys: str) -> None:
        for k in keys:
            self.send_raw(k)

    def interrupt(self) -> None:
        self.send_raw(ESC)

    def resize(self, cols: int, rows: int) -> None:
        self.cols, self.rows = cols, rows
        with self._lock:
            self._screen.resize(rows, cols)
        self.proc.setwinsize(rows, cols)

    # ── transcript ───────────────────────────────────────────────────────────

    def project_dir(self) -> Path:
        return project_dir_for(self.cwd)

    def resolve_session(self, timeout: float = 30.0) -> str | None:
        """Resolve this child's conversation id (banner id, else new-file detection).

        A transcript only appears once the session has written its FIRST turn, so
        call this after sending a prompt.
        """
        deadline = time.time() + timeout
        pdir = self.project_dir()
        while time.time() < deadline:
            if self.session_id and (pdir / f"{self.session_id}.jsonl").exists():
                return self.session_id
            for f in sorted(pdir.glob("*.jsonl"), key=lambda p: p.stat().st_ctime):
                if f.name in self._before_transcripts:
                    continue
                # Identity is transcript CREATION time ~ process start, never
                # newest-mtime: two claudes can share one cwd.
                if f.stat().st_ctime < self.launched_at - 1:
                    continue
                self.session_id = f.stem
                return self.session_id
            time.sleep(0.4)
        return self.session_id

    def transcript_path(self) -> Path | None:
        if not self.session_id:
            return None
        p = self.project_dir() / f"{self.session_id}.jsonl"
        return p if p.exists() else None

    def transcript_lines(self) -> list[dict]:
        p = self.transcript_path()
        if not p:
            return []
        out = []
        try:
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        except OSError:
            pass
        return out

    def transcript_messages(self, role: str | None = None) -> list[tuple[str, str]]:
        """[(role, text)] for real user/assistant messages, meta lines skipped."""
        msgs = []
        for obj in self.transcript_lines():
            t = obj.get("type")
            if t not in ("user", "assistant"):
                continue
            if obj.get("isMeta") or obj.get("isSidechain"):
                continue
            if role and t != role:
                continue
            msgs.append((t, flatten_content(obj.get("message", {}).get("content", ""))))
        return msgs

    def wait_transcript_contains(
        self, needle: str, role: str = "user", timeout: float = 45.0
    ) -> str | None:
        """Block until a `role` message in the transcript contains `needle`."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.session_id:
                self.resolve_session(timeout=2.0)
            for r, text in self.transcript_messages(role=role):
                if needle in text:
                    return text
            time.sleep(0.5)
        return None


# ───────────────────────────── helpers ───────────────────────────────────────


def _is_rule(line: str) -> bool:
    """A flush-left horizontal rule (the input box's top/bottom border)."""
    s = line.rstrip()
    return len(s) >= 8 and set(s) <= {"\u2500", "\u2501", "\u2504", "\u2508", "\u23af"}


def _is_prompt_line(line: str) -> bool:
    s = line.lstrip()
    return s.startswith("\u276f") or s.startswith("\u203a") or s.startswith(">")


def flatten_content(content) -> str:
    """Claude message content -> plain text (string, or a list of blocks)."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for b in content:
            if isinstance(b, dict):
                parts.append(b.get("text") or b.get("content") or "")
            else:
                parts.append(str(b))
        return "".join(p if isinstance(p, str) else str(p) for p in parts)
    return str(content)


def encode_cwd(cwd: str) -> str:
    """Claude's project-dir encoding: every non-[A-Za-z0-9] char becomes '-'."""
    return re.sub(r"[^A-Za-z0-9]", "-", str(Path(cwd).resolve()))


def claude_home() -> Path:
    return Path(os.environ.get("CLAUDE_CONFIG_DIR") or (Path.home() / ".claude"))


def project_dir_for(cwd: str) -> Path:
    return claude_home() / "projects" / encode_cwd(cwd)


def which_claude() -> str | None:
    """Resolve a NATIVE claude.exe. A .cmd shim is a breadcrumb, never launched:
    CreateProcessW appends only '.exe' and ignores PATHEXT."""
    env = os.environ.get("CLAUDE_EXE")
    if env and Path(env).exists():
        return env
    cands = [Path.home() / ".local" / "bin" / "claude.exe"]
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if d:
            cands.append(Path(d) / "claude.exe")
    for c in cands:
        try:
            if c.is_file():
                return str(c)
        except OSError:
            pass
    return None


# ───────────────────────────── CLI ───────────────────────────────────────────


def _add_common(ap: argparse.ArgumentParser) -> None:
    ap.add_argument("--cwd", default=os.getcwd(), help="working dir for the child")
    ap.add_argument("--exe", default=None, help="path to claude.exe")
    ap.add_argument("--cols", type=int, default=160)
    ap.add_argument("--rows", type=int, default=45)
    ap.add_argument("--arg", action="append", default=[], help="extra child argv (repeatable)")
    ap.add_argument("--timeout", type=float, default=45.0)
    ap.add_argument("--answer-modals", action="store_true",
                    help="auto-answer known startup modals that have a safe answer")
    ap.add_argument("--keep-env", action="store_true",
                    help="do NOT strip nesting env vars (debugging only)")


def _mk(a) -> ClaudeWrap:
    return ClaudeWrap(
        cwd=a.cwd, exe=a.exe, args=a.arg, cols=a.cols, rows=a.rows,
        strip_nesting_env=not a.keep_env,
    )


def _banner(t: str, stdout: bool = False) -> None:
    # The screen is the payload and goes to stdout; everything else is
    # diagnostics on stderr. A banner must ride the stream it labels, or the
    # two interleave out of order when piped.
    print(f"\n\x1b[1m== {t} ==\x1b[0m", file=sys.stdout if stdout else sys.stderr,
          flush=True)


def cmd_probe(a) -> int:
    w = _mk(a).start()
    print(f"[probe] pid={w.pid} cwd={a.cwd}", file=sys.stderr)
    try:
        try:
            w.wait_ready(timeout=a.timeout, answer_modals=a.answer_modals)
            print("[probe] READY", file=sys.stderr)
            rc = 0
        except ModalError as e:
            print(f"[probe] MODAL: {e.modal.title}\n  remedy: {e.modal.remedy}", file=sys.stderr)
            rc = 3
        except ReadyTimeout as e:
            print(f"[probe] NOT READY: {e.args[0].splitlines()[0]}", file=sys.stderr)
            rc = 4
        _banner("screen", stdout=True)
        print(w.screen(), flush=True)
        return rc
    finally:
        w.close()


def cmd_raw(a) -> int:
    w = _mk(a).start()
    try:
        time.sleep(a.seconds)
        out = w.raw()
        if a.out:
            Path(a.out).write_text(out, encoding="utf-8")
            print(f"[raw] {len(out)} chars -> {a.out}", file=sys.stderr)
        else:
            print(repr(out))
        return 0
    finally:
        w.close()


def cmd_send(a) -> int:
    w = _mk(a).start()
    print(f"[send] pid={w.pid} cwd={a.cwd}", file=sys.stderr)
    try:
        try:
            w.wait_ready(timeout=a.timeout, answer_modals=a.answer_modals)
        except ModalError as e:
            print(f"[send] BLOCKED by modal: {e.modal.title}\n  remedy: {e.modal.remedy}",
                  file=sys.stderr)
            _banner("screen", stdout=True)
            print(w.screen(), flush=True)
            return 3
        print("[send] ready, injecting prompt", file=sys.stderr)
        w.send_prompt(a.text)
        if a.wait_idle:
            try:
                w.wait_idle(timeout=a.idle_timeout)
            except ReadyTimeout:
                print("[send] turn did not settle in time", file=sys.stderr)
        _banner("screen", stdout=True)
        print(w.screen(), flush=True)
        rc = 0
        if a.verify:
            sid = w.resolve_session(timeout=30)
            hit = w.wait_transcript_contains(a.text, role="user", timeout=30)
            _banner("verification")
            print(f"  pid          : {w.pid}", file=sys.stderr)
            print(f"  session id   : {sid or '(not found)'}", file=sys.stderr)
            print(f"  transcript   : {w.transcript_path() or '(not found)'}", file=sys.stderr)
            ok = hit is not None
            print(f"  prompt landed: {'PASS' if ok else 'FAIL'}", file=sys.stderr)
            if ok:
                print(f"  user content : {hit[:160]}", file=sys.stderr)
            for role, text in w.transcript_messages(role="assistant")[-1:]:
                print(f"  reply        : {text[:300]}", file=sys.stderr)
            rc = 0 if ok else 1
        return rc
    finally:
        w.close()


def cmd_selftest(a) -> int:
    """Prove the whole chain works on THIS machine, cheaply (one tiny turn)."""
    checks: list[tuple[str, bool, str]] = []

    def chk(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, ok, detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{('  ' + detail) if detail else ''}",
              file=sys.stderr, flush=True)

    print("[selftest] running", file=sys.stderr)
    exe = which_claude()
    chk("claude.exe resolves", bool(exe), exe or "not found")
    if not exe:
        return 1

    w = _mk(a).start()
    try:
        chk("spawned on a ConPTY", bool(w.pid), f"pid={w.pid}")
        try:
            w.wait_ready(timeout=a.timeout, answer_modals=a.answer_modals)
            chk("TUI reached the input box", True)
        except ModalError as e:
            chk("TUI reached the input box", False, f"blocked by modal: {e.modal.title}")
            print(f"    remedy: {e.modal.remedy}", file=sys.stderr)
            return 1
        except ReadyTimeout as e:
            chk("TUI reached the input box", False, e.args[0].splitlines()[0])
            return 1

        chk("transcript saving is on",
            "Transcript saving is off" not in w.screen(),
            "(a CLAUDE_CODE_CHILD_SESSION leak turns it off)")

        # fill without submit: the draft must land in the box, un-sent
        w.fill("SELFTEST draft line")
        time.sleep(1.2)
        chk("bracketed paste fills the box without sending",
            "SELFTEST draft line" in w.screen() and not w.is_busy())
        # clear it: End, then kill-to-start
        w.send_keys(KEY_END, CTRL_U)
        time.sleep(0.8)
        chk("Ctrl+U clears the draft", "SELFTEST draft line" not in w.screen())

        token = "CONPTY_SELFTEST_OK"
        w.send_prompt(f"Reply with exactly: {token}")
        try:
            w.wait_idle(timeout=a.idle_timeout)
            chk("turn completed", True)
        except ReadyTimeout:
            chk("turn completed", False, "did not settle in time")

        sid = w.resolve_session(timeout=30)
        chk("session id resolved", bool(sid), sid or "")
        chk("transcript exists", w.transcript_path() is not None, str(w.transcript_path() or ""))
        hit = w.wait_transcript_contains(token, role="user", timeout=30)
        chk("prompt recorded in transcript", hit is not None, (hit or "")[:60])
        replies = w.transcript_messages(role="assistant")
        chk("assistant replied", bool(replies), (replies[-1][1][:60] if replies else ""))

        failed = [n for n, ok, _ in checks if not ok]
        print(f"\n[selftest] {len(checks) - len(failed)}/{len(checks)} passed"
              + (f" - FAILED: {', '.join(failed)}" if failed else ""), file=sys.stderr)
        return 1 if failed else 0
    finally:
        w.close()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="conpty_claude", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="spawn, wait for ready, dump the screen")
    _add_common(p); p.set_defaults(fn=cmd_probe)

    p = sub.add_parser("raw", help="dump the raw escape stream")
    _add_common(p)
    p.add_argument("--seconds", type=float, default=8.0)
    p.add_argument("--out", default=None)
    p.set_defaults(fn=cmd_raw)

    p = sub.add_parser("send", help="inject a prompt and (optionally) verify it landed")
    p.add_argument("text")
    _add_common(p)
    p.add_argument("--verify", action="store_true", help="check the transcript on disk")
    p.add_argument("--wait-idle", action="store_true", help="wait for the turn to finish")
    p.add_argument("--idle-timeout", type=float, default=300.0)
    p.set_defaults(fn=cmd_send)

    p = sub.add_parser("selftest", help="prove the whole chain works here (one tiny turn)")
    _add_common(p)
    p.add_argument("--idle-timeout", type=float, default=180.0)
    p.set_defaults(fn=cmd_selftest)

    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
