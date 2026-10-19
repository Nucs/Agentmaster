# interactive-tools-probe

These scripts are behind [`doc/agentmaster/INTERACTIVE_TOOLS.md`](../../doc/agentmaster/INTERACTIVE_TOOLS.md).
That document asks whether a `PermissionRequest` hook can answer Claude Code's `AskUserQuestion` dialog, above
all its previews and notes, and how often the dialog makes the user wait. First run: 2026-10-19, Claude Code
2.1.291.

| File | What it does |
|---|---|
| `run_probe.py` | The live probe. It runs a real `claude.exe` TUI on a ConPTY, using the repository's `claude-wrap-ConPTY` driver, with a `--settings` file whose only hook is `perm_hook.py`. It checks seven scenarios on the transcript: the hook answers a plain question (A); the PC answers first (B); the PC answers a preview question with a pick and a note (C), with a note alone (F), with a plain pick (G); the hook reproduces the pick-with-a-note answer (D) and the note-alone answer (E). `--with-plans` adds the two plan-approval scenarios (P1, P2), and `--only C,D` runs a subset. Exit code 0 when every scenario run passes. It saves each answer's evidence as `<letter>-pc-answer.json` / `<letter>-hook-answer.json`. |
| `perm_hook.py` | The `PermissionRequest` hook the probe registers. It logs Claude Code's payload, then waits for a reply file and prints the reply as its JSON output. This is the same long poll that Agentmaster's helper would do. |
| `survey_interactive.py` | A read-only survey of every transcript under `~/.claude/projects`: how many questions there are, how long each waited, how each ended, how previews and notes were used, and the previews' sizes and widths. `--real-only` leaves out the sessions that worked in a folder under `%TEMP%` (this probe and other test runs). Needs `rg` on PATH. |
| `grepbin.py` | Finds literal strings in the JavaScript bundled in `claude.exe`, which is how to find the functions again after an upgrade (§9 of the document lists them). |

```bash
python -I tools/interactive-tools-probe/run_probe.py <empty-scratch-folder>          # ~1.5 min, a few Haiku turns
python -I tools/interactive-tools-probe/survey_interactive.py --real-only             # ~1.5 min over ~11 GB
python -I tools/interactive-tools-probe/grepbin.py ~/.local/bin/claude.exe 200 600 3 "press n to add notes"
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
