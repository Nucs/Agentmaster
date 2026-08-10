---
name: claude-wrap-ConPTY
description: |
  Drive a REAL Claude Code TUI from code on Windows — spawn `claude.exe` on a pseudo-console
  (ConPTY), render its ANSI output into a readable screen, inject prompts as bracketed paste,
  and verify what actually landed in the session transcript on disk. This is the black-box test
  harness for Claude Code itself: what a headless `claude -p` can NEVER exercise (the Ink TUI's
  input box, startup modals, draft/type-ahead behavior, paste framing, the eaten-Enter class of
  bug). Ships a verified driver (`scripts/conpty_claude.py`, pywinpty + pyte) with a `selftest`
  that proves the whole chain on this machine in one tiny turn, plus `probe` / `send` / `raw`
  CLI verbs. Encodes the measured facts that make it work: `\r` submits while `\n` is a literal
  newline in the box; bracketed paste is the only atomic multi-line delivery; an inherited
  `CLAUDE_CODE_CHILD_SESSION` SILENTLY DISABLES transcript saving (so every on-disk verification
  fails for a reason nothing on screen explains) — the trap for any wrapper launched from inside
  a Claude session; a startup modal (malformed settings.json, workspace trust, login) parks the
  TUI forever and is invisible-by-design unless you render the screen; conversation identity is
  the transcript's CREATION time, never newest-mtime; and the raw Win32 CreatePseudoConsole
  P/Invoke path is a reproducible dead end. Use when asked to test/automate/drive/script the
  Claude Code TUI, wrap claude.exe in a pty, inject or send a prompt programmatically, reproduce
  a TUI bug, verify a prompt reached the transcript, or debug why a wrapped claude hangs, starts
  no session, writes no transcript, or ignores injected input.
keywords: ConPTY, pseudo-console, pseudoconsole, CreatePseudoConsole, pywinpty, winpty, pyte, TUI, Ink, claude.exe, wrap claude, drive claude, automate claude, inject prompt, bracketed paste, ESC[200~, type-ahead, input box, draft, transcript, jsonl, session id, startup modal, workspace trust, settings error, CLAUDE_CODE_CHILD_SESSION, transcript saving off, eaten Enter, submit CR, terminal emulator, screen scrape, black-box test, headless, claude -p
keywords-sparse: test the claude TUI, drive claude from code, wrap claude in a pty, send a prompt to claude programmatically, why does my wrapped claude write no transcript
---

# Driving the Claude Code TUI under ConPTY

Claude Code's interactive mode is an **Ink (React-for-terminals) TUI**. It only behaves like
itself when its stdin/stdout is a **real TTY** — on Windows that means a **pseudo-console
(ConPTY)**. A plain pipe makes it degrade or refuse; `claude -p` is a different, non-interactive
code path entirely.

So this is the harness for everything `-p` can't reach: the input box, startup modals, paste
framing, type-ahead queuing, draft handling, and the "the TUI ate my submit Enter" class of bug.

**The contract:** drive it like a user (bytes into a pty), but **never assert on the screen** —
assert on the **transcript on disk**. The screen repaints constantly and is a rendering, not a
fact. The screen is for *waiting* and *diagnosing*; the `.jsonl` is for *verifying*.

## Quickstart

```bash
S=".claude/skills/claude-wrap-ConPTY/scripts/conpty_claude.py"

python "$S" selftest --cwd "C:/some/trusted/dir"   # 11 checks, one tiny turn — run this first
python "$S" probe    --cwd "C:/some/trusted/dir"   # spawn, wait for the box, dump the screen
python "$S" send "What is 2+2?" --verify --wait-idle --cwd "C:/some/trusted/dir"
python "$S" raw --seconds 10 --out raw.txt         # the escape stream, for parser work
```

Exit codes: `0` ok · `1` verification failed · `3` blocked by a startup modal · `4` never ready.
Add `--answer-modals` to auto-answer known modals that have a safe answer (read the caveat in
§3 before you rely on it). The **screen goes to stdout, diagnostics to stderr**, so
`python "$S" probe > screen.txt` gives you a clean screenshot.

Requires `pywinpty` + `pyte` (`pip install pywinpty pyte`) — both already installed here.

## Library

```python
import sys; sys.path.insert(0, r".claude\skills\claude-wrap-ConPTY\scripts")
from conpty_claude import ClaudeWrap, ModalError, KEY_END, CTRL_U

with ClaudeWrap(cwd=r"C:\work\repo", cols=160, rows=45) as w:
    w.wait_ready()                       # raises ModalError / ReadyTimeout, never hangs blind
    w.fill("a draft")                    # bracketed paste, NO submit -> sits in the box
    w.send_keys(KEY_END, CTRL_U)         # ...and clear it again
    w.send_prompt("What is 2+2?")        # fill + CR, one atomic delivery
    w.wait_idle()                        # turn finished (screen heuristic)
    print(w.screen())                    # rendered screen, as a human would see it
    w.resolve_session()                  # -> conversation uuid
    w.wait_transcript_contains("2+2")    # the ONLY sound assertion
```

Observation: `screen()` `raw()` `is_busy()` `quiet_for()` `find_modal()` `input_box_ready()`.
Waiting: `wait_ready` `wait_idle` `wait_for(regex)` `wait_quiet`.
Input: `fill` `submit` `send_prompt` `send_raw` `send_keys` `interrupt` `resize`.
Disk: `resolve_session` `transcript_path` `transcript_messages` `wait_transcript_contains`.

## The eight techniques

### 1. Spawn on a real ConPTY

`winpty.PtyProcess.spawn(exe, cwd=, env=, dimensions=(rows, cols))` — note `(rows, cols)`, the
opposite order of most APIs. Give it a **wide** terminal (160×45): Claude reflows to width, and
a narrow pty wraps prompts mid-word, which breaks naive screen matching.

Resolve a **native `claude.exe`**, never a `.cmd` shim: `CreateProcessW` appends only `.exe`
and **ignores `PATHEXT`**, so a bare `claude` token finds the native binary but silently misses
an npm `claude.cmd` and dies `0x80070002`. `which_claude()` does this (env `CLAUDE_EXE` →
`~/.local/bin/claude.exe` → PATH).

### 2. Strip the nesting env — the silent-failure trap

⚠ **The single most expensive gotcha here.** A wrapper launched from *inside* a Claude session
inherits that session's markers, and the child then starts, looks perfectly healthy, and writes
**no transcript at all** — so every on-disk verification fails with nothing on screen to explain
it (the notice is one dim footer line you will not read).

Measured on 2.1.225 by dropping one variable at a time:

| child env | transcript written? |
|---|---|
| inherited as-is | **no** |
| drop `CLAUDE_CODE_CHILD_SESSION` only | **yes** |
| drop `CLAUDECODE` only | no |
| drop `CLAUDE_CODE_SESSION_ID` only | no |

`CLAUDE_CODE_CHILD_SESSION` **alone** is the culprit; the footer says *"Transcript saving is off
— inherited CLAUDE_CODE_CHILD_SESSION marker · restart with `CLAUDE_CODE_FORCE_SESSION_PERSISTENCE=1`"*.
`NESTING_ENV_KEYS` strips it plus `CLAUDECODE`, `CLAUDE_CODE_SESSION_ID` /
`_PARENT_SESSION_ID` / `_ENTRYPOINT` / `_EXECPATH` / `_SSE_PORT`, `AI_AGENT`, and the
Agentmaster stamps (`AM_SESSION`, `CCMGR_*`). `selftest` asserts the footer is absent, so this
can never regress silently. Use `--keep-env` only to reproduce the bug.

### 3. Detect startup modals instead of hanging

A modal parks the TUI forever and is **invisible by design** to everything downstream: no prompt
is accepted, no transcript id is minted, no hooks fire. The naive "sleep, then send" wrapper
types its prompt straight into the menu.

`find_modal()` matches the rendered screen against `KNOWN_MODALS` (settings-error, workspace
trust, external CLAUDE.md imports, bypass-permissions, theme picker, login); `wait_ready()`
raises `ModalError` carrying the modal, its **remedy**, and the screen. Fail in ~4s with a
diagnosis, not 35s with a shrug.

⚠ `--answer-modals` is for **unblocking a test run, not for production**. Each answer has a
cost: answering the settings-error modal with *"3. Continue without these settings"* runs the
session with a **different config than you think** — fine to observe the TUI, wrong for a test
whose result depends on settings. Prefer fixing the cause. Modals with no safe answer
(trust, login) always raise.

Adding a modal = one `Modal(...)` entry: `markers` (all must appear on screen), a `safe_answer`
or `None`, and a remedy.

### 4. Wait on the screen, never on a clock

`sleep(5)` is the bug generator: it races a slow start (you type into nothing) and wastes time on
a fast one. Everything here waits on **rendered content**.

**Ready** = the input box is on screen *and* the stream has gone quiet. `input_box_ready()`
finds the box the way a human does — the bottom-most `❯` line **wrapped by flush-left horizontal
rules** (a rule directly above *and* below). Both cues are load-bearing: a **sent** prompt renders
inline with no box, and a **menu** caret has the question above it, not a rule. Matching a bare
`❯` matches all three.

**Busy** = the footer shows `esc to interrupt` — shown for exactly as long as the turn is
cancellable. ⚠ Do **not** key "turn finished" on the input box: the box **stays rendered during a
turn** (it accepts type-ahead), so `input_box_ready()` is true mid-turn too. That mistake makes
`wait_idle` return the moment the model pauses to think.

### 5. Render the ANSI stream (pyte), don't regex it

Feeding raw output into `pyte.Screen` gives the **rendered** screen. Claude's output is
cursor-addressed (`ESC[3G`-style column jumps per word), so the raw stream contains no readable
lines at all — a substring search over it is hopeless, while the rendered screen is exactly what
a human sees. This is what makes waiting and diagnosis reliable.

pyte doesn't implement a few modern sequences and leaks their tails as stray glyphs (a lone `u`
on row 0 is the classic — kitty keyboard `ESC[<u` / `ESC[>1u`). `_PYTE_NOISE` strips those before
feeding; they carry no cell content, so it's lossless. `raw()` keeps every original byte for
parser work.

### 6. Inject with bracketed paste — and know your line endings

**Measured, and the whole reason the recipe looks like it does:**

| you write | the TUI does |
|---|---|
| `\r` (CR) | **submits** |
| `\n` (LF) | a **literal newline inside the box** — never submits |
| `ESC[200~ …text… ESC[201~` | one atomic paste, no submit |

So a multi-line prompt written raw is a trap either way: with `\n` it silently accumulates and is
never sent; with `\r` it becomes **N separate messages**. Bracketed paste is the only atomic
multi-line delivery — verified: a 3-line paste sat in the box unsent, then one CR submitted it as
**one** message with `\n` preserved.

`send_prompt()` = `fill()` + a short settle + `submit()`. The settle matters: the CR can outrun
the paste on a busy machine, which is the classic **eaten-Enter** symptom (prompt typed, never
sent, turn never starts).

`fill()` without `submit()` is a first-class capability — that's a **standby** delivery (one Enter
away), and it's how you test draft handling.

**Type-ahead:** a CR sent while a turn is running does not vanish — the message is **queued**
(`Press up to edit queued messages`) and runs as the next turn. Account for it, or a stray CR
becomes a message you never meant to send.

### 7. Resolve the session by CREATION time

Two claudes can share one cwd, so "newest transcript in the project dir" binds to whichever is
momentarily most active. Identity is **transcript creation ≈ process start**: `resolve_session()`
ignores every `.jsonl` present before launch and any created before `launched_at`.

Faster path: with debug logging on, the banner prints `debug\<uuid>.txt`, and **that uuid is the
session id** (verified — it matches the transcript name). The reader scrapes it live.

A transcript only exists **after the first turn** — a started-but-never-prompted session has no
file. Resolve *after* sending, and never `--resume` an id with no transcript on disk (it exits 1,
"No conversation found").

Project dir = `~/.claude/projects/<cwd with every non-alphanumeric char replaced by ->`.

### 8. Verify on disk, not on screen

`transcript_messages()` reads the `.jsonl`, skips `isMeta`/`isSidechain` lines, and flattens
content blocks to text. `wait_transcript_contains(needle, role="user")` is the assertion that
matters: it proves the bytes you injected became a **real message**, which is precisely what the
screen cannot tell you (a prompt can be rendered in the box and never submitted).

## Gotchas

- **A malformed `settings.json` blocks every new session.** Found live on this machine: four
  unresolved `<<<<<<< Updated upstream` merge-conflict markers from a `git stash pop`. Claude
  shows the settings-error modal on **every** launch, and even after answering it, the whole file
  is skipped. `python -m json.tool < settings.json` is the two-second check.
- **Workspace trust is not covered by `--dangerously-skip-permissions`.** Pre-seed
  `~/.claude.json` → `projects.<git-root-or-cwd>.hasTrustDialogAccepted = true`. The key is
  `path.normalize`d with `\` → `/`, **case-sensitive**, no trailing separator. Accepting in your
  **home dir never persists**, so `%USERPROFILE%` re-prompts forever — never launch a harness
  there.
- **`CLAUDE_CONFIG_DIR` isolation raises a second dialog of its own** ("Allow external CLAUDE.md
  file imports?") by reclassifying `~/.claude/CLAUDE.md` `@imports` as project-external. That is
  a harness artifact, not real-config behavior — don't "fix" it.
- **`pywinpty.read()` BLOCKS.** It must own a daemon thread; the main thread only ever inspects
  the rendered screen. A read on the main thread deadlocks the whole harness (and no timeout
  saves you — that's why the first prototype appeared to "hang at 958 bytes").
- **Don't echo the child's raw stream to your own stdout** (`echo=True` is debug-only). It leaves
  *your* terminal in whatever modes the child set (alt screen, bracketed paste, kitty keyboard),
  and the child's capability queries prompt *your* terminal to write replies into *your* stdin,
  corrupting later reads.
- **Terminate explicitly.** `close()` force-terminates; a leaked `claude.exe` holds a live
  session (and, in Agentmaster, keeps showing up in the fleet).
- **Every run costs tokens and leaves a real transcript** in `~/.claude/projects/…`. Keep test
  prompts trivial ("Reply with exactly: X") and remember the sessions are real history.

## Dead end: the raw Win32 P/Invoke path

Doing `CreatePipe` → `CreatePseudoConsole` → `UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE)`
→ `CreateProcessW(EXTENDED_STARTUPINFO_PRESENT)` by hand (as `claude-wrap-proto.cs` /
`claude-wrap-proto-cmd.cs` attempt) **does not capture child output here.**

Fingerprint: `CreatePseudoConsole` returns `S_OK`, `CreateProcessW` succeeds, and you capture
exactly **16 chars** — `ESC[?9001h ESC[?1004h`, conhost's own mode-set bytes — while the child's
real output goes to the **parent's** console. Ruled out by measurement: struct layout
(`STARTUPINFOEX` = 112, attribute list = 48), attribute value (`0x00020016`, correct),
`bInheritHandles` **both** ways, and `FreeConsole()` before `CreateProcessW`. All three variants
fail identically with cmd.exe as the child.

**Use pywinpty** — its native implementation handles the attach correctly and is verified
end-to-end here. Don't re-derive the P/Invoke path without new information; this is where the
original prototypes stalled.

## Files

- `scripts/conpty_claude.py` — the driver (library + `selftest`/`probe`/`send`/`raw` CLI).
- `reference/findings.md` — the raw measurements behind every claim above, with repro commands.

Origin: hardened from `~/.claude/scripts/claude-wrap-proto.py` and the two `claude-wrap-proto*.cs`
prototypes. Verified on Claude Code **2.1.225**, Windows 11 26200, Python 3.12, pywinpty + pyte.
