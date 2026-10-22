# interrupt-probe — user interrupts, the "uninterrupt" line, and forks of a running turn

The measurements behind [`doc/agentmaster/HOOKS.md`](../../doc/agentmaster/HOOKS.md) *Interrupted turns*
(2026-10-22): the Restart question ("Continue the interrupted turn?"), the Fork question ("Fork interrupted /
uninterrupted") and the fork stop watch all rest on what these probes showed on Claude Code **2.1.291**.
**Re-run them after a Claude Code update.** A changed result means the planner or the classifier must change
(below).

## What runs, and what it can touch

- The REAL `claude.exe` (the official installer's `~/.local/bin` copy), on a ConPTY through the repo's
  `claude-wrap-ConPTY` skill (`.claude/skills/claude-wrap-ConPTY/scripts/conpty_claude.py`, needs `pywinpty`).
- Against [`tools/e2e-spike/mockllm.py`](../e2e-spike/mockllm.py) on 127.0.0.1: no request reaches Anthropic,
  and each prompt carries its own script (`[[mock: say "…"; tool Bash {…}; delay 15000; reply "…"]]`).
- In a scratch `CLAUDE_CONFIG_DIR` (`<root>\cfg`) and a scratch working folder (`<root>\work`): the user's
  `~/.claude` is never read or written.
- With the minimal environment of `tools/e2e-spike/spike_env.clean_env`, which ALWAYS sets
  `DISABLE_AUTOUPDATER=1`, `CLAUDE_CODE_SKIP_AUTO_UPDATE=1` and `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1`.
  ⚠ Never remove them: a clean environment without them once replaced the pinned `claude.exe` with a newer one.
  The probes add `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1`, the switch that lets a resume continue a turn.

## The live probes (`interrupt_probe.py`)

```bash
# 1. The mock, in the background (it must keep running; its log is how the probe sees a request).
python -I tools/e2e-spike/mockllm.py 48461 "Q:/tmp/am-interrupt-probe/requests.jsonl" --full
# 2. One experiment at a time (Git Bash: MSYS_NO_PATHCONV=1 keeps the Windows path as it is).
MSYS_NO_PATHCONV=1 python -I tools/interrupt-probe/interrupt_probe.py 48461 "Q:\tmp\am-interrupt-probe" e1
```

Each experiment prints the transcript rows it made (`<uuid8><-<parent8> A[…]` / `U[…]`), the planned target of
the rewind line, and whether a resume sent a main request BY ITSELF within its watch window.

| Run | Shape | Resume without the line | Resume with the line |
|---|---|---|---|
| `e1` | Esc while the reply streams (`[Request interrupted by user]` under the partial reply) | no request | the prompt sent again (`interrupted_prompt`) |
| `e2` | Esc during the FIRST tool call (`[Request interrupted by user for tool use]` over a rejected result) | no request | the whole turn again, 3 requests |
| `e2` with `E2_WAIT=16` | Esc during the SECOND tool call | no request | `Continue from where you left off.` from the first call's result, 2 requests |
| `e3` | fork (`--resume --fork-session`) of a source waiting on its 2nd request | — | the fork went busy 4.9 s after launch, its request at 6.0 s; one Esc → `Continue…` + `[Request interrupted by user]` |
| `e3b` | fork of a source inside `sleep 40` | — | busy 5.5 s, request 5.9 s: the fork RE-RAN the call (a call with no result is dropped and continued) |
| `e4 <fork id>` | the e3 fork, rewound later | — | the line stepped over the fork's own `Continue…` row: continued |
| `e5` | Esc BEFORE any output | the prompt is resent by itself (no row was written) | — |

In every fork run the fork's heartbeat (`<cfg>\sessions\<pid>.json`) named the fork's own id and read `busy` at
the first sign of the continued turn, and the continued turn fired NO `UserPromptSubmit` hook. A lone `ESC`
written into the ConPTY (`ClaudeWrap.interrupt()`) is what interrupted the turn; the fork stop watch writes the
same byte.

## The compiled planner (`rewind_probe.cpp` + `cpp_probe.py`)

`rewind_probe.exe` runs the app's own `PlanInterruptRewind` / `RewindInterruptTail` /
`ClassifyResumeContinuation` (`src/cascadia/TerminalApp/AgentMaster/ApiErrorRewind.cpp`) on a transcript file.
It links against the objects of a built m5 harness:

1. Build the harness from a COPY of the engine folder, so the source tree stays clean and the build writes to Q:
   copy `src\cascadia\TerminalApp\AgentMaster` to e.g. `Q:\tmp\am-m5\AgentMaster`, change the copy's
   `tests\run-m5-tests.bat` `cd /d` line to the copy's `tests` folder, and run it (its scratch-profile isolation
   stays as it is).
2. `tools\interrupt-probe\build_rewind_probe.bat Q:\tmp\am-m5\AgentMaster\tests Q:\tmp\am-interrupt-probe\bin`
   (the objects must come from the same sources as this checkout's `ApiErrorRewind.h`).
3. With the mock running:

```bash
MSYS_NO_PATHCONV=1 python -I tools/interrupt-probe/cpp_probe.py 48461 "Q:\tmp\am-interrupt-probe" "Q:\tmp\am-interrupt-probe\bin\rewind_probe.exe" c1
```

| Case | What happens | 2026-10-22 on 2.1.291 |
|---|---|---|
| `c1` | Esc during a reply → `plan` + `apply` with the C++ code → resume | planned to the prompt, appended; the resume sent it again: PASS |
| `c2` | Esc inside the second tool call → `plan` + `apply` → resume | planned to the first result past one cut result; `Continue from where you left off.` and the turn finished: PASS |
| `c3` | `classify` a live source inside `sleep 40` | `ContinuesTurn`, `dropped=Bash`: PASS |

⚠ `rewind_probe apply` WRITES to the transcript it is given: only ever point it at a scratch transcript.

## The corpus survey (`interrupt_survey.py`)

`python -I tools/interrupt-probe/interrupt_survey.py [max files]` reads `$CLAUDE_CONFIG_DIR/projects` (else
`~/.claude/projects`), READ-ONLY, and prints every shape of rows above an interrupt row with counts and
examples. It is how the planner's shapes were chosen; a new shape after an update shows up here first.

## When a result changes

- The rewind's walk and verdicts: `PlanInterruptImpl` in `ApiErrorRewind.cpp` (shared parse: `ParseBranch`).
- What a resume or a fork continues: `ClassifyImpl` (a mirror of Claude Code's `bfo` resume classifier).
- The fork stop watch's timing: `DecideForkTurnStop` and its constants in `AgentMaster/InterruptedTurns.h`.
- The tests: `TestInterruptedTurns` in `AgentMaster/tests/tests_rewind.cpp`; the record: HOOKS.md *Interrupted turns*.
