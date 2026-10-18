# The AI search's prompt eval

How the Sessions page's and the Telegram Search topic's search agent (Sonnet 5.5, `SessionSearchAgent`) was measured
and tuned on 2026-10-18, so that a later change to its prompt or its tools can be measured the same way. The results,
the engine fixes and the Search Procedure are in [`doc/agentmaster/SESSIONS.md`](../../doc/agentmaster/SESSIONS.md)
§9.15. The user's rule: **searches must never miss**, so the number that counts is the recall of the `must` lists.

## What is here

| File | What it is |
|---|---|
| `queries.json` | The five queries the prompt was tuned on, one of each kind: a subject that is also a folder, words the user remembers, an error code within one project, a file CHANGED, a period with a folder. |
| `heldout_queries.json` | Four more the prompt was never tuned on: a file changed, a day, an error, the user's words. |
| `score.py` | Scores one round's results against a keys file (`--miss`: every missed session, seen by a tool or never; `--calls`: every tool call with its counts). |
| `pieces.txt` | **The prompt's source.** The `AM_PROMPT_*` macros of `SessionSearchTools.cpp` ("the system prompts") are generated from it; the page prompt and the database prompt are two orders of the same pieces. |
| `gen_prompt.py` | pieces.txt -> the macros, the two composed prompts, and the check that the composed page prompt equals a scored prompt file. |
| `splice_prompt.py` + `compose.cpp.txt` | Puts generated macros into `SessionSearchTools.cpp`, with the two compositions and the `#undef`s. |

**Not here: the answer keys.** They name real sessions of the machine they were built on, so `.gitignore` keeps any
`*keys*.json` out. A key file is
`{"titles": {"<whole id>": {"title", "project", "last"}}, "queries": {"q1": {"must": [ids], "optional": [ids]}}}`.
Build one from the transcripts on disk (`~/.claude/projects/*/<id>.jsonl`), never from a tool's answer: for each query,
the sessions that certainly fit go in `must`, the borderline ones in `optional` (showing one is no error, missing one is
no miss). A session the scorer reports as wrongly shown is worth a second look: a few keys of 2026-10-18 were wrong, not
the model (an `STATUS_HEAP_CORRUPTION` spelling the first regex missed, a pasted table that named the word only in
passing).

## A round

```
:: the live harness, optimized, into a folder outside the tree (K: is nearly full)
set AM_LIVE_OUT=Q:\tmp\am-eval\build
src\cascadia\TerminalApp\AgentMaster\tests\run-harness-live.bat --eval tools\ai-search-eval\queries.json ^
    --eval-out Q:\tmp\am-eval\r1.json --agent-thinking medium [--agent-prompt-file <candidate prompt.txt>]
python -I tools\ai-search-eval\score.py Q:\tmp\am-eval\r1.json --keys <keys.json> --calls --miss
```

- Each query runs in a FRESH conversation; every tool call is recorded with its whole result. Without
  `--agent-prompt-file` the built-in prompt runs (the app's own path).
- `AM_LIVE_PROFILE` picks the profile it runs in. Copy a real profile's `sessions-index` and `session-store` into a
  scratch profile first: the index is then warm (a first build reads every transcript), and the stored titles are the
  user's. Delete the copy afterwards. ⚠ Never point it at a live profile.
- The first grep after a build reads the transcripts with a cold file cache: tens of seconds instead of a few. Compare
  times between warm rounds only.
- One round of the five costs about $0.50 and two to three minutes (2026-10-18: $0.02 to $0.25 a query); run the main
  set twice and the held-out set once per candidate, since one run can be lucky.
- `harness_live.exe --tool <search|grep|read> '<arguments JSON>' [--tool-out <file>]` runs ONE tool call over the
  machine's sessions with no model, timed — the way to take a slow or empty result apart (a grep is then timed per
  candidate file too).

## Changing the prompt

1. Edit `pieces.txt` (one piece per `@@NAME`; the page and database prompts share most of them, so a change applies to
   both unless it is in a `_PAGE` / `_DATABASE` piece).
2. `python -I gen_prompt.py <the last scored prompt.txt> <out dir>`: it reports where the pieces now differ, and writes
   `<out dir>/rendered_page.txt` — the candidate.
3. Run a round with `--agent-prompt-file <out dir>\rendered_page.txt` (main twice, held-out once) and read every tool
   call of every miss: was it the tools (never named — fix the engine) or the model (seen, dropped — fix the prompt)?
4. When it wins: `python -I gen_prompt.py <the winning file> <out dir>` (now "page prompt == scored prompt"), then
   `python -I splice_prompt.py <out dir>\macros.cpp.txt`, then the m5 suite: `tests_tabtools.cpp` and
   `tests_harness.cpp` pin the procedure's structure and key rules, and the prompt must stay ASCII and under 30,000
   characters (MSVC's wide-literal limit).
5. Run one round WITHOUT `--agent-prompt-file`, which proves the built-in prompt is the one that scored.

The prompt-engineering choices that held up (SESSIONS.md §9.15 has the failure behind each): duties ranked in the
introduction (completeness, evidence, speed); a numbered procedure the model follows on every search request; rules that
quote the exact failing call ("grep 0xC0000374|heap.?corrupt again with dir:agentmaster, never 0xC0000374 alone");
every rule tied to a field the tools actually return (`more`, `also`, `folders`, `first_prompt`, `in_period`), which
the engine had to add first — a rule cannot make the model see what no result shows.
