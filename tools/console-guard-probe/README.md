# console-guard-probe

Checks the managed host's console guard (`WrapPwshScriptInConsoleGuard`, `ClaudeSpawn.h`) in a **private** pseudo
console. Background: CLAUDE.md, Gotcha "A killed agent leaves its fullscreen terminal modes on".

**The problem it guards against.** Every managed launch runs `pwsh -NoLogo -NoExit -EncodedCommand <script>`. When
the agent ends, the tab drops to a `PS <cwd>>` prompt. If the agent was killed or crashed while claude's fullscreen
renderer was up, it never undid its modes. The alternate screen, mouse tracking, focus reports, kitty keyboard flags
and a raw VT input mode stay on. PSReadLine then echoes every mouse move as text (`[555;54;31M`) and Ctrl+C arrives as
`[99;5u` (dev tab 2b655223, 2026-10-21).

## Files

| file | what it is |
|---|---|
| `guard.py` | reads the guard out of `ClaudeSpawn.cpp` exactly as the compiler builds it; `python -I guard.py` prints it and its size |
| `faketui.py` | a stand-in that turns claude's fullscreen modes on and TerminateProcess()es itself |
| `faketui_graceful.py` | the same modes, undone, then a normal exit (code 0) |
| `drive.py` | runs a stand-in under the host script, plays the terminal, and reports |
| `drive_claude.py` | the same with the real claude, killed after its fullscreen frame |

## Run

From this folder, with Python that has `pywinpty` and `pyte`:

```bash
python -I drive.py crash old pwsh < /dev/null   # reproduces the bug (echo + input mode 0x208)
python -I drive.py crash new pwsh < /dev/null   # the guard: no echo, the original input mode, the exit note
python -I drive.py clean new pwsh < /dev/null   # after a clean exit the guard changes nothing visible
python -I drive.py crash new ps51 < /dev/null   # Windows PowerShell 5.1, the launcher when pwsh is missing
python -I drive_claude.py new < /dev/null       # the real claude, killed (starts claude; see below)
```

Each run prints:

- which of the reset sequences reached the terminal side;
- whether mouse/focus input echoed at the prompt;
- the exit note;
- the console modes a command run from the prompt inherits.

Raw captures go to `%TEMP%\console-guard-probe`.

## Measured (2026-10-21, PowerShell 7.5.8 and Windows PowerShell 5.1.26100, claude 2.1.291)

| case | echoed | input mode afterwards |
|---|---|---|
| stand-in crash, old host | yes (`[555;10;5M…[I[O`, PSReadLine shows `digit-argument: 3`) | `0x208` |
| stand-in crash, guarded | no | `0x1F7` (the original), plus the note |
| stand-in clean exit, guarded | no | `0x1F7`, no note |
| real claude killed, old host | yes | `0x208` |
| real claude killed, guarded | no | `0x1F7`, plus the note |

## Rules

- **Always redirect stdin from `/dev/null` (or NUL).** In one run whose stdout went to a file, pywinpty gave the shell
  the parent's stdin instead of the pseudo console. Under Claude Code that stdin is a pipe carrying the session's
  forwarded mouse input, and the shell read about 200 real mouse moves from it.
- The probes touch nothing outside their own pseudo console. Never point them at a live tab.
- `drive_claude.py` starts a real claude in `%USERPROFILE%\Desktop` (a folder it already trusts):
  - it runs your SessionStart hooks, makes its startup network calls and bumps `~/.claude.json`'s counters;
  - it sends no prompt (no model request) and creates no transcript;
  - the killed claude's `~/.claude/sessions/<pid>.json` stays until another claude cleans it up.
- pyte, the screen model the drivers use for display, does not know `CSI < u`, `CSI ! p`, OSC 8 or the alternate
  screen. Odd characters in the printed rows come from pyte. The checks read the raw stream.
