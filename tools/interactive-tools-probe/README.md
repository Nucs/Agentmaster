# interactive-tools-probe

These scripts are behind [`doc/agentmaster/INTERACTIVE_TOOLS.md`](../../doc/agentmaster/INTERACTIVE_TOOLS.md).
That document asks whether a `PermissionRequest` hook can answer Claude Code's `AskUserQuestion` dialog, above
all its previews and notes, and how often the dialog makes the user wait. First run: 2026-10-19, Claude Code
2.1.291. The same day the answer was BUILT ([`MESSAGING.md`](../../doc/agentmaster/MESSAGING.md) §21.30), and the
`ask*` tools below check the shipped hook end to end.

| File | What it does |
|---|---|
| `run_probe.py` | The live probe. It runs a real `claude.exe` TUI on a ConPTY, using the repository's `claude-wrap-ConPTY` driver, with a `--settings` file whose only hook is `perm_hook.py`. It checks seven scenarios on the transcript: the hook answers a plain question (A); the PC answers first (B); the PC answers a preview question with a pick and a note (C), with a note alone (F), with a plain pick (G); the hook reproduces the pick-with-a-note answer (D) and the note-alone answer (E). `--with-plans` adds the two plan-approval scenarios (P1, P2), and `--only C,D` runs a subset. Exit code 0 when every scenario run passes. It saves each answer's evidence as `<letter>-pc-answer.json` / `<letter>-hook-answer.json`. |
| `perm_hook.py` | The `PermissionRequest` hook the probe registers. It logs Claude Code's payload, then waits for a reply file and prints the reply as its JSON output. This is the same long poll that Agentmaster's helper would do. |
| `survey_interactive.py` | A read-only survey of every transcript under `~/.claude/projects`: how many questions there are, how long each waited, how each ended, how previews and notes were used, and the previews' sizes and widths. `--real-only` leaves out the sessions that worked in a folder under `%TEMP%` (this probe and other test runs). Needs `rg` on PATH. |
| `grepbin.py` | Finds literal strings in the JavaScript bundled in `claude.exe`, which is how to find the functions again after an upgrade (§9 of the document lists them). |
| `askdrive.cpp` + `build_askdrive.py` | The ENGINE side of the built feature (MESSAGING.md §21.30, the document's §10), as a small exe built from the engine sources the m5 suite compiles: it writes the SHIPPED question hook script (`BuildAskHookScript`) and hooks settings file (`BuildHooksSettingsJson`, with the `PermissionRequest` entry) into a folder, runs the real `AskBridge` + `AskRegistry` on its own pipe, and answers each request with an action (`pick:<i>[:<note>]`, `notes:<note>`, `own:<text>`, `release`, `chat`, `hold`) through the real `AskQuestion.h`. `build_askdrive.py <out folder>` builds it from the working tree (nothing is written into the source tree). |
| `askprobe.py` | The live end-to-end check of the built feature (everything but Telegram): a real `claude.exe` whose `--settings` is the driver's file and whose `CCMGR_HOOK_PIPE` names the driver's pipe, so the shipped PowerShell hook runs. H1 a pick + a note and H2 a note alone must give the PC's own tool result; H3 a release must leave the dialog to the PC (Enter) and end the helper; H4 Chat about this must give the deny text verbatim. |
| `askmem.py`, `askparent.py` | The waiting helper measured (its start-to-request time, its memory, its end after a release) and its parent check (killing its parent must end it). |

```bash
python -I tools/interactive-tools-probe/run_probe.py <empty-scratch-folder>          # ~1.5 min, a few Haiku turns
python -I tools/interactive-tools-probe/survey_interactive.py --real-only             # ~1.5 min over ~11 GB
python -I tools/interactive-tools-probe/grepbin.py ~/.local/bin/claude.exe 200 600 3 "press n to add notes"
python -I tools/interactive-tools-probe/build_askdrive.py Q:/tmp/askdrive             # ~3 min (the engine sources)
python -I tools/interactive-tools-probe/askprobe.py <empty folder> --driver Q:/tmp/askdrive/askdrive.exe   # ~1 min, 4 Haiku turns
python -I tools/interactive-tools-probe/askmem.py <empty folder> --driver Q:/tmp/askdrive/askdrive.exe
python -I tools/interactive-tools-probe/askparent.py <empty folder> --driver Q:/tmp/askdrive/askdrive.exe
```

**Before you run the probe:**
- **It is a real Claude session.** It spends tokens and leaves a transcript under `~/.claude/projects` for the
  scratch folder. Run the survey with `--real-only`, or those sessions are counted too.
- **It skips the workspace-trust dialog by setting `CLAUDE_CODE_SANDBOXED=1` on the child**, so it never writes
  `~/.claude.json`.
- **Run it from a shell outside Agentmaster's tabs if you can.** The driver strips the nesting variables
  (`CLAUDE_CODE_CHILD_SESSION`, `AM_SESSION`, `CCMGR_*`). Even so, the probe's claude appears in the Fleet
  Observer's External census while it runs.
- **Re-run it after a Claude Code upgrade** that changes hooks or `AskUserQuestion`. The PC scenarios (C, F, G)
  type keys into the preview dialog; if its keys change, they fail first. If a hook scenario fails, the
  document's §6 design must fall back to showing the questions only.
