# e2e-spike — a real Claude Code against a scripted mock model (feasibility, 2026-10-22)

The probe behind [`doc/agentmaster/E2E_TESTING.md`](../../doc/agentmaster/E2E_TESTING.md) §2: can a native
`claude.exe` run every kind of turn instantly against a local mock of the Anthropic Messages API whose
answers are written into the prompt itself? Yes — measured on Claude Code **2.1.291**, Windows 11 26200,
Python 3.12, on this workstation (not yet in a test desktop).

This is a SPIKE. The product mock, the test driver and the suite are designed in E2E_TESTING.md and do not
exist yet.

| File | What |
|---|---|
| `mockllm.py` | The mock: `POST /v1/messages` (SSE or whole), `count_tokens`, `GET /v1/models`; the `[[mock ...]]` script language (`say`, `think`, `tool`, `reply`, `delay`, `drop`, `error` with an optional retry count); the two side requests answered apart (session title, compaction); a JSONL request log (`--full` adds every body). |
| `spike_env.py` | `seed` pre-answers Claude Code's first-run screens in an isolated config folder; `p` runs one headless `claude -p` turn in a CLEAN environment (see the warning below). |
| `tui_spike.py` | Drives the INTERACTIVE TUI on a ConPTY with the repository's `.claude/skills/claude-wrap-ConPTY` driver: send prompts, wait for screen text or quiet, type raw keys, dump screens. |

## Run it

```bash
cd tools/e2e-spike
python -I mockllm.py 48433 Q:/tmp/am-e2e-spike/requests.jsonl --full &          # the mock
python -I spike_env.py seed 'Q:\tmp\am-e2e-spike\cfg' 'Q:\tmp\am-e2e-spike\work'  # first-run seeds
python -I spike_env.py p 48433 'Q:\tmp\am-e2e-spike\cfg' 'Q:\tmp\am-e2e-spike\work' '[[mock: reply "pong"]]'
MSYS_NO_PATHCONV=1 python -I tui_spike.py 48433 'Q:\tmp\am-e2e-spike\cfg' 'Q:\tmp\am-e2e-spike\work' turns \
  'one [[mock: reply "pong"]]' \
  'two [[mock: think "plan"; tool Write {"file_path": "Q:/tmp/am-e2e-spike/work/x.txt", "content": "hi"}; reply "done"]]' \
  'NOWAIT:three [[mock: tool AskUserQuestion {"questions":[{"question":"Pick a side?","header":"Side","options":[{"label":"Left","description":"l"},{"label":"Right","description":"r"}],"multiSelect":false}]}; reply "ok"]]' \
  'WAITFOR:Enter to select' 'QUIET:' 'KEYS:\r' 'WAITFOR:User answered' \
  'four [[mock: error 529 overloaded_error "Overloaded" 2; reply "recovered"]]' \
  '/compact' \
  'five [[mock: say "half"; drop]]'
```

## ⚠ Before you run it

- **The auto-updater.** `spike_env.clean_env` sets `DISABLE_AUTOUPDATER=1`, `CLAUDE_CODE_SKIP_AUTO_UPDATE=1`
  and `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1`. The first interactive run had no such variables (a clean
  environment drops the user's own) and its claude **replaced the user's pinned `~/.local/bin/claude.exe`,
  2.1.291 → 2.1.308, within three minutes**. It was restored by copying
  `~/.local/share/claude/versions/2.1.291` back. Keep those variables on any machine whose claude matters.
- **Isolation.** Every run uses an isolated `CLAUDE_CONFIG_DIR` (transcripts, presence files and
  `.claude.json` stay in it) and a working folder OUTSIDE the user profile (a cwd under `C:\Users\<user>`
  makes Claude Code read the user's `~/.claude/CLAUDE.md` as a project file). Nothing reaches the real API:
  the base URL is the mock, the token a dummy.
- **Git Bash.** Call the scripts from PowerShell, or with `MSYS_NO_PATHCONV=1`: Git Bash rewrites an
  argument starting with `/` (`/compact` arrived as `C:/Program Files/Git/compact`).
- **Cleanup.** `tui_spike.py` force-closes its claude; the mock runs until stopped. Run from a shell outside
  Agentmaster if you can: a spike claude shows in the Fleet Observer's External census while it lives.

## What was measured (2026-10-22, Claude Code 2.1.291)

| Check | Result |
|---|---|
| `claude -p` turn against the mock | the scripted text, **1.2–1.4 s wall** (process start); the mock answered in 3–4 ms; ONE request (`POST /v1/messages?beta=true`, 22–23 tools, model `claude-opus-5-5`, Bearer token, 11 `anthropic-beta` flags) |
| Interactive TUI, fresh config | first a theme picker, then the "Bypass Permissions mode" warning; with the three seeds (`spike_env.seed_config`) the input box is ready in ~5 s with no screen to answer |
| Turns in the TUI | text, thinking, `Write` and `Bash` really executed, an `AskUserQuestion` dialog answered by Enter (the tool result flows back to the next step), `/compact`; each turn shows "… for 0s" |
| `delay 3000` | the session stays busy for 3 s ("Crunched for 3s") — a Running state of chosen length |
| `error 529 … 2` then a reply | Claude Code retried after ~0.6 s and ~1.1 s and got the reply: 1.7 s in all |
| `error 400 invalid_request_error "prompt is too long…"` | "Context limit reached"; the transcript gets a synthetic assistant line `isApiErrorMessage: true`, `apiErrorStatus: 400` — what Agentmaster's Error state keys on |
| `say "half"; drop` | three retries (0.6 / 1.1 / 2.1 s), then "API Error: Connection lost mid-response": a partial assistant line + a synthetic `isApiErrorMessage` / `server_error` line — the shape behind HOOKS.md *API-error turns* |
| `/v1/models`, `count_tokens` | never called |

## Protocol facts the product mock must honor

- **Side requests quote the prompt.** Every new session sends a title request in parallel with the first turn
  (system prompt "You are naming a coding session…", no tools, `output_config.format` = JSON schema
  `{title}`, the prompt inside `<session>` tags). `/compact` sends a request with the full tool list whose last
  USER message starts "CRITICAL: Respond with TEXT ONLY…". A mock that runs the script for these answers
  the title request with the turn's tool calls and the compaction with a script step (both seen).
- **`role: "system"` messages sit inside `messages`** (beta `mid-conversation-system-2026-04-07`): the
  environment block, reminders, `<total_tokens>…`. The user's prompt is often NOT the last message, and the
  compaction instruction is followed by one. Find the script in the newest USER message that holds one.
- **`metadata.user_id` is a JSON string** with `device_id`, `account_uuid` and **`session_id`** — Claude Code's
  conversation id: the key for per-session mock state and for joining the request log to a test's session.
- Main-loop requests carry `thinking: {type: adaptive, display: updates}`, `output_config.effort`, and
  `context_management` (`clear_thinking_20251015`); the mock may ignore them.

## Harness rules learned here (they bind the product driver)

1. **Never wait for text the prompt itself shows.** The script is on screen as soon as it is sent:
   `WAITFOR:answered` matched `reply "answered"` in the prompt line, before any reply existed.
2. **Input sent while a TUI dialog is still rendering is dropped.** An Enter sent the instant
   AskUserQuestion's question (or its "Enter to select" footer) appeared was ignored in 11 of 13 runs. The
   dialog stayed open, and the NEXT prompt's Enter answered it, so that prompt vanished (no transcript line,
   no request). A 1.5 s quiet wait before the key fixed it in 3 of 3 runs.
3. **Verify every input's effect on disk** (the transcript, the mock's log), never on the screen alone.
4. **Pin the renderer.** One launch printed "Claude Code's fullscreen renderer didn't finish starting last
   time on this machine, so this launch is using the classic renderer": the choice depends on hidden state
   from earlier runs. Set it explicitly (`CLAUDE_CODE_NO_FLICKER=1` for fullscreen) per test.
