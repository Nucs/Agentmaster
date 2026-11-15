# agentmaster-link

The session's end of Agentmaster's **session link** (Agentmaster's `doc/agentmaster/MOD_LINK.md`; this is its first slice).
Agentmaster ships it inside `TerminalApp.dll` and loads it into every Claude tab it launches (`--plugin-dir`, like its
other bundled mods: `doc/agentmaster/BUNDLED_MODS.md`). Unlike them, its source lives here, in the Agentmaster
repository (`bundled-mods.json` marks it `"source": "repo"`, so `bundled_mods.py sync` leaves it alone).

## What it does

Agentmaster launches each Claude tab with two variables:

| Variable | Value |
|---|---|
| `AM_LINK` | Agentmaster's loopback server, `http://127.0.0.1:<port>` (an ephemeral port, one per Agentmaster process) |
| `AM_LINK_TOKEN` | 32 hex digits minted for that launch, registered with the server, never logged or saved |

When both are present and well formed, the mod says hello and keeps ONE poll open. Agentmaster holds it up to 5 s and
answers the moment it has a command for this session; the poll is the link's heartbeat.

| Request | Body → answer |
|---|---|
| `POST /v1/hello` | `{protocol: 1, mod, claude, session}` → `{protocol, hold}` (404: not this Agentmaster's token, the link stops; 409: another protocol) |
| `GET /v1/commands?hold=5000&session=<id>` | → `{commands: [...]}` (or `{release: true}`: stop) |
| `POST /v1/results` | `{results: [{id, ...}]}` → `{ok, taken}` |

The one command so far is **`fork`** `{id, prompt, effort?}`: a side question (Telegram's `/btw`). The mod runs it with
`$.model.fork({ prompt })` — the call Claude Code's own `/btw` is built on: the main thread's LAST request again
(its system prompt, tools, model and messages) with one more user message, every tool denied, its own tail never cached,
nothing written to the transcript — so the API serves the whole conversation from the prompt cache. The result goes back
as `$.model.fork` resolved it (`isAnswered`, `text`, `usage`, `effort`, or `reason`), with the command's `id` and the
time it took. Each command runs on its own, so the polling never waits for a fork.

Measured on Claude Code 2.1.322 (2026-11-15): a fork over a 548k-token conversation read 548,147 tokens from the cache
and wrote 899, answering in 8.3 s at the session's effort.

## What it does not do

- Nothing at all without both variables, or with an endpoint that is not `http://127.0.0.1:<port>` (the token is never
  sent anywhere else).
- It draws nothing and hooks only `session.start`: no tool call, prompt, keystroke or render passes through it.
- It never writes to the conversation.

## Limits

- `$.model.fork` answers `nothing-to-fork` until the session's claude has finished a request of its own (a tab resumed
  and not yet prompted). Agentmaster then asks a headless fork instead (`SideQuestion.cpp`).
- The loop ends on a 404, a 409, Agentmaster's `release`, or 20 failures in a row (Agentmaster ended: its tabs end with it).

## Files

| File | What |
|---|---|
| `hooks/register.ts` | The hook and the loop: hello, the held poll, the commands, the results. |
| `hooks/link.ts` | The pure rules: the endpoint and token checks, the poll's URL and answer, the backoff, the effort. |
| `tests/` | `claude plugin test .` (11 tests). |
