# End-to-end UI testing — Agentmaster driven like a user, Claude Code on a scripted model

> **Status (2026-10-23): the first suites RUN.** Phases 1 and 2 (§12) are built for three areas, with NO change to
> the app: the scripted model (`e2e/mockllm/`, §4, its unit tests), the guest's provisioning and runner (`e2e/guest/`),
> the driver (`e2e/amdriver/`, §7: real input, UI Automation, the app's process and profile files, the terminal, page
> objects for the tab strip, menus, dialogs, the Agent Manager page and the side panel, failure evidence), the host
> orchestrator (`e2e/host/e2e_run.py`), and suites for the Agent Manager page (MGR), the side panel (SP) and the tab's
> context menu (TCM, [`flows/23-tab-context-menu.md`](flows/23-tab-context-menu.md)) — run against the published 0.8.0
> portable build and Claude Code 2.1.291 in a claude-fleet Windows 11 desktop. On its first day the suite found three
> defects of 0.8.0, kept as known-defect tests (MGR-K1, SP-K1, TCM-K1). How to run it and how to write a scenario:
> [`e2e/README.md`](../../e2e/README.md). §6 (the app's testability changes: E2E mode, the read-only bridge,
> AutomationIds) is still a proposal — the suite reads UI Automation names, `sessions.json` and the logs instead.
> The spike (`tools/e2e-spike/`, 2026-10-22) stays as the record of §2.
>
> **What the suite must prove (2026-10-23):** [`flows/README.md`](flows/README.md) catalogs the end-to-end flows of the
> features of 0.7.7 → 0.8.0 and after (behaviors, lifecycle phases, checks, edge cases, known defects), one file per
> area. This document is the HOW; the flows are the WHAT — §10's ladder and §11's areas map onto them.

**The aim, in the user's words:** "run end to end tests as complex as UI usage gets and be able to
automate it in a suite … in a docker win11 machine with full computer use control … agentmaster and
claude code native (latest version) that will use mocked LLM that lets claude code run instantly and
the sender gets to define at the sent message the response … detect any usage defects, covering every
feature (we start small with one focused area with various complexities)."

## Contents

1. [Goal, scope and principles](#1-goal-scope-and-principles)
2. [What the spike proved](#2-what-the-spike-proved)
3. [Architecture](#3-architecture)
4. [The scripted model (mockllm)](#4-the-scripted-model-mockllm)
5. [The test machine](#5-the-test-machine)
6. [Agentmaster changes for testability](#6-agentmaster-changes-for-testability)
7. [The driver library (amdriver)](#7-the-driver-library-amdriver)
8. [Runner, orchestration and lanes](#8-runner-orchestration-and-lanes)
9. [AI-driven exploratory runs](#9-ai-driven-exploratory-runs)
10. [First focus area: the Prompt Queue Runner](#10-first-focus-area-the-prompt-queue-runner)
11. [Coverage map for the rest of the app](#11-coverage-map-for-the-rest-of-the-app)
12. [Phases and acceptance](#12-phases-and-acceptance)
13. [Risks](#13-risks)
14. [Decisions for the owner](#14-decisions-for-the-owner)
- [Appendix A — the seeds and variables of a test machine](#appendix-a--the-seeds-and-variables-of-a-test-machine)
- [Appendix B — what a test looks like](#appendix-b--what-a-test-looks-like)

---

## 1. Goal, scope and principles

**Goal.** A suite of end-to-end tests that use Agentmaster the way a person does — real windows, real
tabs, real `claude.exe` processes on real ConPTYs, real hooks, real clicks and keystrokes — at every
level of complexity, run unattended in a disposable Windows 11 machine, fast and repeatable, and able
to tell a usage defect from noise.

**In scope (v1):** the Agentmaster UI and engine against Claude Code; one focus area first (§10),
then the rest of the app area by area (§11).

**Out of scope (v1):** the Telegram bot, Codex, the installers and the updater (each needs a fake of
its own service — §11 lists them), performance and memory measurements (separate tools exist:
`tools/cpuhunt`, `tools/leakhunt`).

**Principles** (each one is a lesson of this codebase or of the spike):

1. **Act through the UI, verify through independent oracles.** A test clicks, types and drags like a
   user. It checks the result through something the feature under test did not compute: the mock's
   request log (what reached the model), Claude Code's transcript (what Claude Code recorded), UI
   Automation (what the screen shows). The app's own state (registry, logs) is read too, but an
   assertion never rests on the very code path it tests (e.g. never verify the pending-input detector
   with `pendingInput` alone).
2. **Wait on conditions, never on clocks.** Every wait names what it waits for and has a timeout. A
   screen wait must never be satisfiable by the prompt's own echo (the spike's first false pass).
3. **Verify every input's effect.** A keystroke or paste is not "sent" until its effect is on disk —
   keys typed into a TUI that is still rendering are dropped (measured, §2.3).
4. **One process under test per machine, a clean state per test.** Fresh profile, fresh working folders,
   a fresh Claude state; nothing a test leaves behind can decide the next one.
5. **The tested binary is the shipped binary.** Test hooks are switched on by environment variables at
   start-up, never compiled into a separate flavor (§6.1).
6. **Ambient failure detection.** After every test, crash dumps, swallowed exceptions (`[exc]`, `[wil]`),
   UI stalls, orphaned processes and unknown model requests are failures even when the test's own
   assertions passed (§7.6).

---

## 2. What the spike proved

`tools/e2e-spike/` (2026-10-22, Claude Code 2.1.291, on this workstation; its README has every command and
number). The spike mock is one stdlib-only Python file (~520 lines with its documentation).

### 2.1 Claude Code runs instantly against a scripted mock

| Check | Result |
|---|---|
| `claude -p` turn | the scripted reply, **1.2–1.4 s wall** (process start dominates); the mock answered in 3–4 ms; one `POST /v1/messages?beta=true` |
| Interactive TUI | text, thinking, `Write` and `Bash` executed for real, `AskUserQuestion` answered by Enter, `/compact`; every turn reports "… for 0s" |
| A Running state of chosen length | `delay 3000` → "Crunched for 3s" |
| Retried errors | `error 529 … 2` then a reply: retried after ~0.6 s and ~1.1 s, reply at 1.7 s |
| A final API error | `error 400 … "prompt is too long…"` → the transcript gets `isApiErrorMessage: true`, `apiErrorStatus: 400` (Agentmaster's Error signal, HOOKS.md *API-error turns*) |
| A cut stream | `say "half"; drop` → 3 retries, then "API Error: Connection lost mid-response": a partial assistant line + a synthetic `server_error` line — the production incident shape of the API-error rewind |
| Auth | `ANTHROPIC_BASE_URL=http://127.0.0.1:<port>` + `ANTHROPIC_AUTH_TOKEN=<dummy>` (Bearer). No login, no key question |

### 2.2 Facts the product mock must honor

- **Side requests quote the prompt.** A new session sends a title request beside its first turn (system
  prompt "You are naming a coding session…", no tools, `output_config.format` = JSON schema `{title}`);
  `/compact` sends one whose last USER message starts "CRITICAL: Respond with TEXT ONLY…". A naive mock
  ran the turn's script for both (a `Write` tool call answered the title request; a script step became the
  compaction summary).
- **`role: "system"` messages inside `messages`** (beta `mid-conversation-system-2026-04-07`): the prompt is
  often not the last message; the compaction instruction is followed by one.
- **`metadata.user_id` is a JSON string holding `session_id`** — Claude Code's conversation id, the join key
  between the mock's log and a test's session.
- No `count_tokens`, no `/v1/models`, no other call reached the base URL.

### 2.3 Facts the test machine and driver must honor

- **First-run screens** on a fresh Claude config: a theme picker, then (with
  `--dangerously-skip-permissions`, which Agentmaster passes) the "Bypass Permissions mode" warning, then
  workspace trust. Three seeds remove them (Appendix A). ⚠ The older `.claude.json` key
  `bypassPermissionsModeAccepted` no longer suppresses the warning on 2.1.291;
  `settings.json` `skipDangerousModePermissionPrompt` does.
- **The auto-updater replaces claude.exe.** A clean environment loses `DISABLE_AUTOUPDATER`; the spike's first
  interactive run replaced this workstation's pinned `~/.local/bin/claude.exe` (2.1.291 → 2.1.308) within
  three minutes (restored from `~/.local/share/claude/versions/2.1.291`). A test machine must set it.
- **Keys sent while a TUI dialog is still rendering are dropped.** An Enter sent the instant
  AskUserQuestion's question appeared was ignored in 11 of 13 runs; the NEXT prompt's Enter then answered
  the dialog and that prompt vanished (no transcript line, no request). A 1.5 s quiet wait fixed 3 of 3.
- **Never wait for text the prompt itself shows.** `reply "answered"` puts "answered" on screen at once.
- **The renderer is chosen from hidden state** ("fullscreen renderer didn't finish starting last time …
  using the classic renderer"): pin it per test (`CLAUDE_CODE_NO_FLICKER=1` for fullscreen).
- **Git Bash rewrites `/`-arguments** (`/compact` → `C:/Program Files/Git/compact`): the runner must not route
  argv through Git Bash.
- **Claude Code on Windows needs Git for Windows or PowerShell 7** (or `CLAUDE_CODE_GIT_BASH_PATH`); the
  claude-desktops golden image has neither.

### 2.4 Facts about Agentmaster that shape the design (from the code)

| Fact | Where | Consequence |
|---|---|---|
| A managed claude inherits Agentmaster's own environment (`reloadEnvironmentVariables = false`) | `TerminalPage.AgentSessions.cpp:2663`, `ConptyConnection.cpp:323` | Agentmaster started with the mock's URL in its environment points every managed session at the mock. A shell `+` tab regenerates its environment from the REGISTRY, so a hand-typed `claude` needs the variables in the user's registry environment or in `~/.claude/settings.json` `env`. |
| The AI-search harness strips session/effort/retry variables but keeps `ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN` | `AgentHarnessCore.cpp:919` `BuildHarnessEnvironment` | The Sessions page's AI search follows the app's environment to the mock too. |
| A Stop less than 2 s after its prompt is read as stale; the scanner reconciles ~2.5 s later | `HookEvents.h:455` `kMinRealTurnSpanMs`, `SessionScanner.h` | Instant mock turns would exercise the PULL reconcile path on EVERY turn instead of the hook path. The mock's default turn must last ≥ 2.5 s (§4.4). |
| An automatic send waits 40 s of settled state and keeps 20 s between sends | `Scheduler.h:97` `kStateSettleMs`, `:78` `kMinSendSpacingMs` | A queue of five prompts takes > 4 minutes at production timing. A fast lane needs both configurable (§6.1); `Scheduler::SetStateSettleMs` exists, the spacing is a constant. |
| `AGENTMASTER_PROFILE` set → no install question, no profile picker | `PortableInstall.h:807`, `ProfileBootstrap.h` | A test launches the portable build with its own profile and meets no first-run dialog. |
| A portable copy checks GitHub for updates at start and every hour | `Updater.h:1980` `IsUpdaterChannel` | A test machine must switch the updater off (§6.1). |
| The window census (strip order, titles, session ids, selection, Agent Manager page, state words) is thread-safe and always registered | `WindowTabTools.h`, `Engine.h:884` `CollectWindowCensus`, `TerminalPage.AgentEngine.cpp:1594` | A read-only test bridge can serve it almost for free (§6.2). |
| Agentmaster's own surfaces set about ten accessible names (the Home button, the side panel's launch button, the transcript overlay's controls) and no AutomationId; glyph buttons (eye, `!`, envelope, ⚙…) have no accessible name at all; cards, dots and badges built from `Border` / `Ellipse` have no automation peer | grep of `TerminalApp` (2026-10-22) | UI Automation can find text and named controls only. §6.3 adds names, ids and accessible state. |
| `AgentSetTitledTip` (95 call sites) and `AgentSetTip` (239) set every tooltip | `AgentTipHelpers.h:483,622` | One edit there names most glyph buttons for UI Automation AND screen readers. |
| The terminal is UI Automation class `TermControl`, type Text, with TextPattern | `TermControlAutomationPeer.cpp:275-283` | A test reads a tab's screen through UI Automation, independently of Agentmaster's draft detector. |
| Managed sessions run under `pwsh.exe` when PowerShell 7 is on PATH, else Windows PowerShell 5.1 | `ClaudeSpawn.cpp:3618` `ResolvePwshLauncher` | The test machine installs PowerShell 7 (the common user setup); a 5.1-only lane covers the fallback. |

---

## 3. Architecture

```text
 THIS PC (the host)                                   │  WINDOWS 11 TEST DESKTOP (KVM VM, claude-fleet)
                                                      │
 e2e/orchestrator/run.py ──claude-fleet MCP──────────►│  claude-desktop agent (24 tools: UIA, input,
  1 create/reset the desktop (create_desktop)         │   shell, files, screenshots, recording)
  2 stage artifacts into Q:\claude-desktops\…\persist  ═══► Z:\e2e\ (SMB share)
  3 provision (run_command → provision.ps1)           │   C:\e2e\  Agentmaster portable build, claude.exe,
  4 start the run (launch → run-suite.cmd)            │            Git + PowerShell 7, Python venv
  5 poll Z:\e2e\out\<run>\status.json                 │
  6 collect the report + evidence, delete the desktop │   pytest runner (in the signed-in session)
                                                      │     ├─ amdriver: UIA (comtypes) + SendInput
 artifacts: Q:\agentmaster-e2e\runs\<stamp>\          │     ├─ mockllm  127.0.0.1:8787  ◄── claude.exe ×N
                                                      │     ├─ test bridge \\.\pipe\agentmaster-e2e.<pid>
 AI explorer (optional, §9): a Claude session        │     └─ log follower: hooks.log, autorunner.log,
  driving the same desktop through the same MCP      │        scanner.log, transcripts, mock log
                                                      │   Agentmaster.exe (AGENTMASTER_PROFILE=C:\e2e\p\<test>,
                                                      │                    AGENTMASTER_E2E=1, mock env)
```

**Why the runner lives INSIDE the guest.** The guest's firewall admits only its gateway, so the host cannot
reach the mock, the test bridge (a local named pipe) or UI Automation events directly; every host call
through the fleet is an HTTP round trip (~0.1–0.5 s; a screenshot ~0.5 s). Inside the guest a UIA read is
milliseconds, waits can subscribe to events, and logs are local files. The host only prepares, starts,
polls and collects.

⚠ **Start the runner with the agent's `launch` tool (or a one-shot scheduled task), never `run_command`:**
`run_command` waits for every process in its job, and the runner starts Agentmaster, which starts
claudes (tools/install-qa/README.md "More traps met").

---

## 4. The scripted model (mockllm)

Product location: `e2e/mockllm/` (Python 3.13 stdlib only — the guest already has Python; no wheel to
install). Grown from `tools/e2e-spike/mockllm.py`.

### 4.1 Endpoints

| Endpoint | Answer |
|---|---|
| `POST /v1/messages` (`stream: true`) | the Messages API's SSE sequence: `message_start`, per block `content_block_start` / `_delta` (`text_delta`, `thinking_delta` + `signature_delta`, `input_json_delta`) / `_stop`, `message_delta` (stop reason, usage), `message_stop`; chunked transfer, flushed per event |
| `POST /v1/messages` (`stream: false`) | one message object |
| `POST /v1/messages/count_tokens` | `{"input_tokens": <estimate>}` (never seen called; cheap to have) |
| `GET /v1/models` | the models the run declares (Agentmaster's model pickers, `/model`) |
| `/_mock/*` (control, §4.5) | loopback only |
| anything else | 404 + a `[unknown-endpoint]` log line (an ambient failure, §7.6) |

Every response carries `request-id`; usage numbers default to small values and can be scripted (`usage`).

### 4.2 Request classification

Every request is classified BEFORE a script is consulted, from Claude Code's own markers. Each kind has a
default answer and can be scripted separately.

| Kind | How it is recognized (2.1.291) | Default answer |
|---|---|---|
| `main` | tools present, not one of the below | the turn's script (§4.3), else `reply "OK"` |
| `title` | system prompt contains "naming a coding session"; `output_config.format` JSON schema with `title` | `{"title": <the turn's title directive, or "Mock session">}` |
| `compact` | the last USER message contains "CRITICAL: Respond with TEXT ONLY" | `<summary>` + a fixed summary (scriptable: `compact "…"`) |
| `subagent` | system prompt of a Task/Agent subagent (marker to pin in Phase 1) | the script inside the subagent's prompt (the parent passes it in the `Agent` tool input — nesting is free) |
| `classifier` | auto-mode permission classifier (only without bypass) | "allow" (marker to pin when a test needs auto mode) |
| `unknown` | any request with no tools that matches none of the above | `reply "OK"` + an `[unknown-kind]` line → an ambient failure, so a new Claude Code side request is noticed the day it appears |

⚠ Classify on the last USER message, never on `messages[-1]` (2.1.291 appends a `system` message after the
compaction instruction — the spike's classifier missed it this way).

### 4.3 The script language

The script rides in the prompt: `[[mock <steps>]]`, steps separated by `;`. The newest USER message that
holds a script is the turn's script; which step a request gets is the count of assistant messages after
that message — stateless, so a retried request gets the same answer.

| Step | Meaning |
|---|---|
| `say "<text>"` | a text block; the response goes on |
| `think "<text>"` | a thinking block |
| `tool <Name> {json}` | a tool_use block; ends the response (stop_reason `tool_use`); the next request of the turn gets the next step. The input is checked against the request's `tools[].input_schema` (required keys, types) — a typo fails loudly instead of confusing Claude Code |
| `tools [<Name> {json}, …]` | parallel tool calls in one response |
| `reply "<text>"` | text + `end_turn`; ends the script |
| `stream <n> <ms>` | stream the next text in chunks of `n` characters every `ms` (realistic typing) |
| `delay <dur>` | pause before the next block (`500ms`, `3s`) |
| `hold <gate>` | stop streaming until the test releases the gate (§4.5) — a Running state of exactly the length the test wants |
| `error <status> <type> "<msg>" [times=N]` | answer with an HTTP error; with `times=N` only the first N attempts fail, then the step proceeds |
| `drop [after=N]` | cut the connection after N events (no `message_stop`) |
| `stop <reason>` | override the stop reason (`max_tokens`, `refusal`, `pause_turn`) |
| `usage in=<n> out=<n> cache_read=<n>` | the usage of this response (context displays, auto-compaction thresholds) |
| `title "<t>"` | the title the `title` side request returns for this session |
| shorthands | `ask "<question>" <opt1>,<opt2>` (AskUserQuestion), `bash "<cmd>"`, `write <path> "<text>"`, `edit <path> "<old>" "<new>"`, `read <path>`, `agent "<subagent prompt>"` |
| `use @<scenario>` | splice a named scenario from the scenario folder (keeps prompts short) |
| `expect "<regex>"` | the tool result this step receives must match; a mismatch is logged and fails the test (lets a test assert what Claude Code returned to the model) |

A bare `[[mock some words]]` is `reply "some words"`. Without any script a `main` request gets
`reply "OK"`: Agentmaster's own prompts (the restore wake's "Continue from where you left off.", a
/handover's first message) need no scripting to complete.

Examples:

```text
fix the parser [[mock reply "done"]]
[[mock think "plan"; bash "git status" ; write C:\e2e\w\t1\a.txt "x"; reply "wrote it"]]
[[mock ask "Which side?" Left,Right; expect "Left"; reply "going left"]]
[[mock hold g1; reply "released"]]                     (the test releases g1 when it wants the turn to end)
[[mock say "half"; drop]]                               (connection lost mid-response -> Error)
[[mock error 529 overloaded_error "Overloaded" times=2; reply "after retries"]]
[[mock agent "[[mock bash \"echo sub\"; reply \"sub done\"]]"; reply "parent done"]]
```

### 4.4 Turn duration — the 2-second floor

Agentmaster discards a Stop that arrives less than 2 s after the turn's prompt (`kMinRealTurnSpanMs`, the
duplicate-Stop defense of DELIVERY.md §9) and lets the scanner reconcile ~2.5 s later. A mock that finishes
every turn in milliseconds would therefore test the reconcile path on every turn and the hook path never.
**Default: the final response of a turn is held until ≥ 2.5 s after the turn's first request.** A test that
wants the sub-floor path says so (`[[mock fast; reply "x"]]`), and the suite covers both on purpose.

### 4.5 State, gates and control

The mock keeps only what a stateless answer cannot: error attempt counts (keyed by session id + script +
step), open gates, and the request log. A loopback-only control API:

| Call | Effect |
|---|---|
| `POST /_mock/gate/<name>/release` | a held stream continues |
| `POST /_mock/gate/<name>/fail` (`{"status":…}` or `{"drop":true}`) | a held stream ends with an error or a cut |
| `GET /_mock/requests?session=<id>&since=<seq>` | the classified request log (also a JSONL file) |
| `GET /_mock/sessions` | sessions seen, with their request counts and open gates |
| `POST /_mock/reset` | a fresh state between tests |
| `POST /_mock/config` | defaults: the default reply, the turn floor (§4.4), the declared models |

### 4.6 What a test can assert through the mock

- The exact text that reached the model (a queued prompt arrived once, unmerged with a draft, in order).
- That no request of a session overlapped another turn (no send while Running).
- What Claude Code sent back as a tool result (an AskUserQuestion answer, a Bash output).
- Which model and settings a launch used (`--model`, presets, effort, thinking).
- That nothing unexpected happened: no `unknown` kinds, no request from a session the test did not open.

---

## 5. The test machine

### 5.1 The base

A claude-fleet Windows desktop (skill `claude-desktops`; `K:\source\claude-desktops\docs\windows.md`): Windows 11
Pro 25H2 under QEMU/KVM in Docker Desktop on this PC, auto-logon admin `Docker` (UAC off), a gateway with 24
MCP tools, the persist share `Z:` = `Q:\claude-desktops\windows\desktops\<name>\persist`, a reset in 35–70 s.
The golden image has Python 3.13 (+ `comtypes`, `psutil`, `pillow` in the agent's venv), Node 22, ffmpeg and
Edge. Proposed size for E2E: `create_desktop(name="am-e2e-<n>", os="windows", width=1920, height=1080,
memory_gb=6, cpus=4)`; deleted after each run (the user works and games on this PC).

### 5.2 What the E2E layer adds (per run, from `Z:\e2e\cache`)

| Component | How | Why |
|---|---|---|
| Agentmaster build | the portable folder (`New-UnpackagedTerminalDistribution.ps1 -PortableMode`) copied to `C:\e2e\am\` | the unpackaged build users run; MSIX later for packaged-only features (toasts, AUMID) |
| claude.exe | `downloads.claude.ai/claude-code-releases/<ver>/win32-x64/claude.exe`, verified against that version's `manifest.json` SHA-256, cached per version, copied to `%USERPROFILE%\.local\bin\` | where Agentmaster's `ResolveClaudeExe` looks |
| Git for Windows (portable) | extracted to `C:\Program Files\Git` | Claude Code's Bash tool |
| PowerShell 7 | the MSI or zip | the host Agentmaster prefers for managed sessions |
| Python venv | `pytest`, `comtypes`, `psutil`, `pillow` from cached wheels | the runner |
| mockllm, amdriver, suites | copied from the repository at the tested commit | |

If per-run provisioning grows past ~3 minutes, bake an `agentmaster-e2e` golden image
(`build_golden.py` + an extra payload) — Phase 5.

### 5.3 Seeds and variables (Appendix A has the exact list)

- **Claude Code:** `~/.claude.json` (onboarding done, theme, no update nags) and `~/.claude/settings.json`
  (`skipDangerousModePermissionPrompt`, an `env` block with the mock's URL and token). Workspace trust is
  Agentmaster's own job (`EnsureClaudeWorkspaceTrusted`) — the suite must NOT pre-seed it, or that feature is
  never tested.
- **User environment (registry):** the mock URL + token, `DISABLE_AUTOUPDATER=1`,
  `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1`, the renderer pin. Registry, because a shell `+` tab rebuilds
  its environment from it (§2.4).
- **Agentmaster's launch environment:** `AGENTMASTER_PROFILE=C:\e2e\p\<test>`, `AGENTMASTER_E2E=1` (+ the
  timing variables of the lane), the same mock variables.
- **Leak guard:** `127.0.0.1 api.anthropic.com` (and the telemetry hosts) in the hosts file, so anything that
  bypasses the base URL fails at once instead of reaching the real API. No real credential ever enters a
  test machine.
- **Determinism:** fixed resolution and 100% scaling, Windows animations off (Agentmaster's ambient animations
  then stand still — RENDERING.md §4c), the time zone of the host (already so), WER LocalDumps for
  `Agentmaster.exe` and `OpenConsole.exe` into `C:\e2e\dumps` (STARTUP_GUARD.md §6: the key is per exe name).

### 5.4 Claude Code versions — two lanes

- **pinned:** the version the user runs (2.1.291 on 2026-10-22). Agentmaster changes are judged here.
- **latest:** resolved at run start (`…/claude-code-releases/latest`; 2.1.308 on 2026-10-22). Nightly. A
  failure that is green on the pinned lane is Claude Code drift — exactly what tells the user whether a new
  Claude Code is safe to adopt (the workstation pins its version with `DISABLE_AUTOUPDATER`).
- The run report records the version, and the mock's `unknown` kinds show new side requests at once.

---

## 6. Agentmaster changes for testability

All NOT BUILT. Each is small, env-gated or behavior-neutral, and useful beyond tests.

### 6.1 E2E mode (`AGENTMASTER_E2E=1`)

A header `AgentMaster/E2eMode.h` reads the variables ONCE at engine init and logs one line
(`[engine] E2E mode: settle=… spacing=… lostSend=… bridge=…`). Unset = today's behavior exactly.

| Variable | Effect | Code |
|---|---|---|
| `AGENTMASTER_E2E=1` | the test bridge (§6.2) starts; the updater stays silent (`IsUpdaterChannel` → false, logged) | `Engine.cpp`, `Updater.h` |
| `AGENTMASTER_E2E_SETTLE_MS` | the STATE SETTLE window | `Scheduler::SetStateSettleMs` (exists) |
| `AGENTMASTER_E2E_SEND_SPACING_MS` | the minimum send spacing | NEW: `kMinSendSpacingMs` becomes a `DecideAdvance` parameter + a `Scheduler` setter (the `SetStateSettleMs` pattern; the m5 tests pass the constant) |
| `AGENTMASTER_E2E_LOST_SEND_MS` | the lost-send settle | same pattern for `kLostSendSettleMs` |

The fast lane runs with settle 2 s / spacing 1 s; the real-timing lane (nightly) with none of them. Tests are
written timing-agnostic (waits, not sleeps), so the same test passes in both.

### 6.2 The read-only test bridge

`AgentMaster/TestBridge.{h,cpp}`: a named pipe `\\.\pipe\agentmaster-e2e.<pid>` (discovery:
`<profile>\e2e-bridge.json`), started only in E2E mode, one JSON request per line, one JSON answer per line.

| Request | Answer |
|---|---|
| `hello` | build, version, profile, E2E timings, settings snapshot, log paths |
| `census` | `CollectWindowCensus` as JSON: windows, Agent Manager page shown, tabs in strip order with title, session id, selected, state word, dormant, dir, branch |
| `sessions` / `session <id>` | the registry's live records as `sessions.json` writes them PLUS the transient facts tests need: started, delivery gate, box state, interrupt hold, presence, flash / unread sets, the inferred dir |
| `ui` | per window: Sessions page / transcript overlay / settings overlay open, side panel shown / scope / Sum, the Manager lens selection (a new `WindowToolRequest::Kind::UiFacts`, answered on each window's UI thread like Census) |

Rules: **read-only** — no request changes anything (tests act through the UI); an explicit DACL (current
user, `PIPE_REJECT_REMOTE_CLIENTS`), unlike the hook pipe's default security; never on the UI thread.
`sessions.json` (≤ ~1 s behind, `Engine.cpp` `SessionAutosaver`) remains the fallback oracle for builds
without the bridge.

### 6.3 UI Automation: names, ids, accessible state

1. **Names from the tooltips.** `AgentSetTitledTip` also sets `AutomationProperties.Name` (when unset) to its
   title, and `AgentSetTip` sets `AutomationProperties.HelpText` to its text. One edit in
   `AgentTipHelpers.h` names the glyph-only buttons (eye, `!`, envelope, ↻, ⚙, …) for screen readers and
   tests at once. Cost: two string properties per element — no `ToolTip` object (the per-element ToolTip
   leak gotcha does not apply).
2. **AutomationIds** with one scheme, `am.<surface>.<element>[.<sid8>]` — the session's first 8 id
   characters, so a test joins the UI to the engine and the mock:
   - `am.home`, `am.mgr.launch.dir`, `am.mgr.launch.go`, `am.mgr.toolbar.{settings,pause,sessions,reopen,activateAll}`
   - `am.mgr.board.col.<state>`, `am.mgr.board.card.<sid8>`, `am.mgr.tree.row.<sid8>`
   - `am.mgr.queue.{compose,add,sendNow,jump,runner,templates}`, `am.mgr.queue.row.<promptId8>`
   - `am.tab.<sid8>` (the `TabViewItem`), `am.badge.<sid8>.{mode,mail,folder,copy,pencil}`
   - `am.side.card.<sid8>`, `am.sessions.*`, `am.settings.*`, `am.transcript.*`
3. **Accessible state** where the visual has no automation peer (a `Border` card, an `Ellipse` dot):
   `AutomationProperties.ItemStatus` on the nearest peer-bearing element — the tab's `TabViewItem`
   (`"Running"`, `"Waiting for you; unread; draft; favorite"`), the board card button, the queue row
   (`"Pending"` / `"Sent (flight)"` / `"Typed"` / `"Failed"`), the runner toggle (`"Auto"`,
   `"Paused → Auto on your next message"`, `"Off globally"`). Screen readers gain the state too.
4. A side-panel card is a `Border`: its title `TextBlock` gets the id; a test clicks the text's rectangle.

Delivered per area: Phase 3 covers the surfaces of §10 only; each later area adds its own.

### 6.4 What does NOT change

No test-only code path in the features: the bridge reads, the timings are values the code already has as
constants, and the names/ids/statuses are accessibility metadata. A build without `AGENTMASTER_E2E`
behaves exactly as today.

---

## 7. The driver library (amdriver)

`e2e/amdriver/` — Python 3.13, `comtypes` UI Automation (the pattern the desktop agent's `uia_worker.py`
already proves in this guest: batched `FindAllBuildCache` reads, a worker process that can be abandoned when
a UIA call hangs).

### 7.1 App and process lifecycle

- `App.start(profile, env, timing)`: launch `Agentmaster.exe` with a fresh profile; wait for the window
  (`CASCADIA_HOSTING_WINDOW_CLASS…`, by pid) and the bridge's `hello`; fail on any unexpected dialog.
- `App.close()`: the window's close and its confirm, through the UI; wait for the GUI to exit and the
  ConPTY hosts to DRAIN (they end their claudes gracefully). Last resort: kill the GUI only — ⚠ never
  `OpenConsole.exe` (orphans every session, OBSERVER.md §13a).
- Single instance: an unpackaged build's instance is keyed by its exe path, so a second start hands off to
  the first — the previous test's instance must be gone first.

### 7.2 UI Automation layer

Find by AutomationId, Name, ControlType, ItemStatus; scoped to the app's windows and its popups (menus and
flyouts live in the popup root); `wait_until(predicate, timeout)` on cached reads + UIA events; dumps of the
subtree for evidence. Actions: `invoke` (InvokePattern), `click` (SendInput at the element's centre — real
pointer events for hover, double-click, right-click, drag), `type` (ValuePattern / SendInput), `select`,
`expand`, `toggle`, `context_menu(element).choose("Deactivate ▸", "Of Same Folder")`.

### 7.3 Input layer

`SendInput` from the signed-in session; the desktop agent's foreground technique (it taps VK 0xE8 — an Alt
tap puts apps into access-key mode); unicode typing; chords; clipboard set/get (paste is a feature path:
Agentmaster journals pastes, PENDING_INPUT.md §2c).

### 7.4 The terminal inside Agentmaster

- Find a tab's `TermControl`, focus it with a click, type a prompt like a user (keystrokes or paste + Enter).
- Read its screen through TextPattern (visible ranges); find Claude's input box with a parser independent of
  Agentmaster's (the claude-wrap-ConPTY driver's rule: the bottom `❯` line between flush-left rules).
- `send_prompt` is **verified**: wait for a quiet, ready box first; after Enter, wait for the prompt in the
  mock log or the transcript; a miss is reported as "input dropped" with the screen, not retried silently.
- Dialogs (AskUserQuestion, permission prompts): wait for the dialog AND 1.5 s of quiet before keys (§2.3).

### 7.5 Oracles

| Oracle | Source | Typical use |
|---|---|---|
| mock log | `GET /_mock/requests` | what reached the model, when, from which session; overlaps |
| transcript | `~/.claude/projects/<enc cwd>/<sid>.jsonl` | what Claude Code recorded (prompts, answers, API errors) |
| bridge | §6.2 | the app's engine view: state, queue, census, UI facts |
| UI Automation | §7.2 | what the user sees |
| logs | `<profile>\hooks.log`, `autorunner.log`, `scanner.log` with read marks | event waits (`[nav] … done`, `[delivered]`, `[send]`, `[recon-*]`) and the why of a failure |
| screenshots | the screen, sampled at UIA rectangles | the few visuals with no text form (a dot colour, the flash ring); no full-image goldens in v1 |

### 7.6 Ambient checks after every test

Each one fails the test (or warns, per a policy file) even when the test's own assertions passed:
new crash dumps or WER `Application Error` events; `[exc]` / `[wil]` / `[persist-fail]` / `[launch-fail]` /
`[ui-stall]` lines in hooks.log; `[unknown-kind]` / `[unknown-endpoint]` in the mock log; a `claude.exe`,
`pwsh` or `OpenConsole.exe` still alive after `App.close()`; a request from a session the test did not open.

### 7.7 Evidence

Per test, kept on failure (and sampled on success): the screen recording (ffmpeg gdigrab), a screenshot per
step, UIA dumps at failure, the three logs, the transcripts, the mock log, the bridge's last answers, the
profile's `sessions.json` and `windows/*.json`, crash dumps (the `debug-dumps` skill reads them).

---

## 8. Runner, orchestration and lanes

### 8.1 In the guest

`pytest` with fixtures: `machine` (once: provision check, mock up), `app` (per test: fresh profile and
workdirs, `App.start`, ambient checks + teardown), `session` (a managed claude launched THROUGH THE UI and
ready), `mock` (reset, gates, log). Markers: area (`pqr`, `lifecycle`, …), level (`L0`–`L5`), lane
(`fast`, `realtime`), `known_defect` (xfail with the defect's id: green while the defect exists, fails when
it is fixed so the marker is removed). Output: JUnit XML + an HTML report + `status.json`
(progress, for the host to poll).

### 8.2 On the host

`e2e/orchestrator/run.py --suite pqr --build <portable folder | ci:<run id>> --claude pinned|latest`:
reuse or create the desktop → reset → stage `Z:\e2e` (build, cache, suites) → `run_command provision.ps1`
→ `launch run-suite.cmd` → poll `status.json` → copy `Z:\e2e\out\<run>` to
`Q:\agentmaster-e2e\runs\<stamp>\` → delete the desktop. It talks to the fleet with the repo's stdlib MCP
client (`K:\source\claude-desktops\fleet\mcp_client.py`, token read into a variable — never printed).

**Builds:** a local Release build obeys the Development Rules (the user's go; Q: only; the build mutex). The
CI workflow can instead produce the portable folder per commit (an extra step in `ci.yml`), so testing a
commit needs no build on this PC.

### 8.3 Flakes

No automatic retry of a product failure. A failure is re-run once only to classify it (`flaky` vs `failed`);
a test that flakes twice in a week is quarantined with an issue. Infrastructure failures (desktop did not
boot, provisioning) are reported apart from test failures.

### 8.4 Lanes

| Lane | When | Claude | Timing | Size |
|---|---|---|---|---|
| contract | on a Claude Code version change, nightly | latest | — | the mock + claude.exe alone (spike style, no Agentmaster): side-request markers, error shapes, the dialog rules of §2.3 |
| fast | on demand, before a release | pinned | E2E timings | the whole suite |
| realtime | nightly | pinned | production | the timing-sensitive subset (settle, spacing, lost-send, enter-retry) |
| latest | nightly | latest | E2E timings | the whole suite |
| renderer | weekly | pinned | E2E timings | the suite with `CLAUDE_CODE_NO_FLICKER=1` (Claude's ALT-buffer renderer: PENDING_INPUT.md §12) |

---

## 9. AI-driven exploratory runs

The scripted suite is the gate. Beside it, a Claude session can explore with the same desktop: the
desktop's MCP tools (screenshots, UIA, input), plus the in-guest helpers through `run_command`
(`am-e2e census`, `am-e2e logs --since`, `am-e2e mock requests`, `am-e2e gate release <g>`), and the script
language to make the inner Claude Code do anything instantly. A charter names a feature and its document
("try to lose a queued prompt", "make the side panel disagree with the board"); the explorer reports
findings with evidence; a confirmed finding becomes a scripted test. Explorer runs never gate a release.

---

## 10. First focus area: the Prompt Queue Runner

**Why this area:** it joins the UI (queue pane, toggles, badge), the state engine (turn ends, settle, guards)
and the terminal (pasting into Claude's box, the draft swap), and its history is long (DELIVERY.md §1–§18).
The mock is essential to it: turn length, questions, errors and cuts become one line of script. The
2026-10-21 study left open defects a user meets (memory `prompt-queue-runner-study-2026-10-21`; two re-checked
in the code on 2026-10-22).

Every test launches its session through the Agent Manager's launch bar, acts through the UI, and asserts
through the mock log + transcript + bridge + UIA.

### L0 — the chain works

| Id | Test | Assertions |
|---|---|---|
| PQR-00 | the app starts clean in a fresh profile | one window, no dialog, bridge `hello`, `[engine] bridge listening`, no ambient failure |
| PQR-01 | launch a session from the launch bar; type `[[mock reply "pong"]]` into its terminal | tab title pinned; state Running → Waiting for you (bridge, tab ItemStatus); board card in the Waiting column; transcript holds prompt + "pong"; the mock logged one `main` + one `title` |

### L1 — the runner's basic contract

| Id | Test | Assertions |
|---|---|---|
| PQR-10 | queue one prompt into an idle session (Auto) via compose box + envelope | delivered exactly once after the settle; its row moves to SENT (flight); `[send]` + `[delivered]`; the mock got its text exactly |
| PQR-11 | queue three prompts, each `delay 3s` | order kept; one per turn — no `main` request of a prompt starts before the previous turn's end (mock log) |
| PQR-12 | Paused mode holds; switching the toggle to Auto releases | nothing sent while Paused (past several settle windows); sending starts after the toggle |
| PQR-13 | the toolbar's global Pause holds two sessions; Resume releases both | both badges read "(Off globally)"; no send; both proceed after Resume |
| PQR-14 | Send now (`!` + confirm) in a Paused session | delivered at once, ahead of the queue |

### L2 — turn shapes

| Id | Test | Assertions |
|---|---|---|
| PQR-20 | a queued prompt runs `bash` + `write` + `reply` | the next prompt waits for the whole turn |
| PQR-21 | the reply ends with "?" | the next prompt stays Pending; the badge shows "❓ held"; a non-question turn typed by the user releases it |
| PQR-22 | the turn asks (AskUserQuestion) | NeedsApproval; nothing sent; answering in the terminal ends the turn; the next prompt follows |
| PQR-23 | `hold g1` | Running for as long as the gate is held; no send; release → Waiting → next prompt |
| PQR-24 | a turn under the 2 s floor (`fast`) | the state still settles (the scanner's reconcile) and the next prompt still goes |

### L3 — a person in the loop

| Id | Test | Assertions |
|---|---|---|
| PQR-30 | an unsent draft sits in Claude's box when the runner sends | the model received the queued prompt ALONE; the draft is back in the box afterwards (read through UIA, independent of the detector) |
| PQR-31 | Esc interrupts a held turn | the runner reads "Paused → Auto on your next message"; nothing sent; the user's next message resumes Auto |
| PQR-32 | the badge's mail button | the draft is queued and the box cleared; Shift+click keeps the box |
| PQR-33 | a prompt typed by hand | recorded once as Typed (not doubled, not re-sent) |

### L4 — failures

| Id | Test | Assertions |
|---|---|---|
| PQR-40 | the turn ends in a final API error (`error 400 …`) | Error (crimson, Error column); the runner pauses itself (stop-on-error); the next prompt stays Pending |
| PQR-41 | `error 529 … times=2` | no Error; the queue continues |
| PQR-42 | `say "half"; drop` | Error with "Connection lost mid-response"; a Restart continues the turn (the mock receives "Continue from where you left off." after the API-error rewind, HOOKS.md) |
| PQR-43 | the session's claude is killed mid-turn | the documented open gap: no Done; captured as a known defect until fixed (CLAUDE.md Gotcha "A killed agent leaves its fullscreen terminal modes on") |

### L5 — restarts

| Id | Test | Assertions |
|---|---|---|
| PQR-50 | close and reopen the app with a queue | the queue and its statuses survive; nothing Sent is replayed |
| PQR-51 | quit while a turn is held | after reopen: Error (cut-off turn), the runner paused (the 2026-10-21 interaction), the restore wake continues the turn |

### Known defects the suite should show on day one (xfail until fixed)

| Id | Defect (re-checked 2026-10-22) | The test |
|---|---|---|
| PQR-K1 | Send now with an empty box re-sends the SELECTED row whatever its status (`AgentManagerContent.AutoTesting.cpp:804` `_DoSendNow`): a Typed history row is re-sent and recorded twice | select a Typed row, press Send now: one new message, no duplicate row |
| PQR-K2 | Move up / down swap by raw queue index (`:1321` `_OnMovePrompt`), so with history rows between Pending ones a click can change nothing on screen | Move up on a Pending row next to a Sent row: the UPCOMING order changes |
| PQR-K3 | the board's ⚙ sent/total counts every recorded message | a session with no queue shows no blue ⚙ N/N |

Estimated size: ~25 tests, 20–60 s each in the fast lane → ~20 minutes per run.

---

## 11. Coverage map for the rest of the app

In a proposed order. "Needs" = infrastructure beyond §4–§8. Each area's flows — what to drive, what to expect,
where to check — are in [`flows/`](flows/README.md) (its *Infrastructure the flows need* table extends this one).

| Area | Main documents | Needs |
|---|---|---|
| Session lifecycle & state engine (dots, board columns, flash ring, unread count, Waiting decay) | HOOKS.md, OBSERVER.md, NOTIFICATIONS.md §8 | pixel sampling for the flash ring; the realtime lane for decay |
| Persistence, reopen, restore wake, cut-off turns, Deactivate / Activate | PERSISTENCE.md, FAVORITES.md §4a | multi-start fixtures |
| Fork / handover family, fork carry of drafts | COMMANDS.md, PENDING_INPUT.md §11 | scripts that `write HANDOVER-*.md` |
| Pending input, draft swap, mail button, Copy Current Prompt | PENDING_INPUT.md, DELIVERY.md §9 | clipboard oracle; fullscreen-renderer lane |
| Sessions browser (search, filters, folders, resume, fork, favorites, rename, hide) | SESSIONS.md | a seeded corpus of transcripts |
| Side panel, tab overlay and summary panel, transcript overlay, copy menus | SIDE_PANEL.md, TAB_OVERLAY.md, TRANSCRIPT_OVERLAY.md | — |
| Tags | CLAUDE.md *Bookmark TAGS* | — |
| Settings cog (each setting's effect) | CLAUDE.md *Settings cog* | — |
| Adopt a hand-typed claude, external census, orphan reaper | OBSERVER.md §13a | registry environment (§5.3) |
| AI search (the harness) | SESSIONS.md §9, SDK_HARNESS.md | scripts calling `mcp__sessions__*` / `mcp__display__show` |
| Presets | HARNESSES.md | a preset pointed at the mock (verifies URL, token, model plumbing) |
| Notifications (toasts) | NOTIFICATIONS.md | the MSIX build (an unpackaged build shows no toast) |
| CLI | CLI.md | none (runs in the guest against the test profile) |
| Telegram | MESSAGING.md | a fake Bot API server + an API base override in E2E mode |
| Codex | CLAUDE.md *Codex* | a fake OpenAI endpoint |
| Updater, installers | Updater.h, PROFILES.md §2b/§2c | a fake GitHub releases API; `tools/install-qa` already drives the installers |

---

## 12. Phases and acceptance

| Phase | Deliverables | Done when |
|---|---|---|
| 0 — spike | `tools/e2e-spike/` (this document's §2) | DONE 2026-10-22 on this workstation |
| 1 — the mock | `e2e/mockllm/`: §4 complete, unit tests of the parser and the SSE output, the contract lane (claude.exe + mock, no Agentmaster) | DONE 2026-10-23 for 2.1.291 (the unit tests; every suite test runs claude.exe on it); the latest lane not run yet |
| 2 — the machine and the chain, no app change | `e2e/guest/provision.ps1`, `e2e/host/e2e_run.py`, `amdriver` lifecycle + UIA + terminal + page objects, the first suites using names and `sessions.json` | DONE 2026-10-23 with the MGR / SP / TCM suites instead of PQR-00/01 (the user's choice of areas); PQR-00/01 next |
| 3 — testability in the app | §6.1–§6.3 for the §10 surfaces; one Release build (the user's go) | the bridge answers; the §10 controls are found by id; m5 green |
| 4 — the first suite | §10 L0–L5 + K1–K3; ambient checks; fast + realtime lanes | 10 consecutive fast runs green except the known defects, < 30 min each |
| 5 — breadth | §11 area by area; the e2e golden image; the CI build artifact; the explorer runs | per area: its ladder green; the latest lane nightly |

---

## 13. Risks

| Risk | Mitigation |
|---|---|
| Claude Code drift (new side requests, changed markers, TUI changes) | the `unknown` kind fails loudly; the contract lane; pinned vs latest lanes |
| Timing flakiness | event waits only; E2E timings; the 2 s floor default; verified inputs; quarantine policy |
| UI Automation gaps in XAML Islands (no peer, popups, virtualized tab strip) | §6.3; the tab strip reveals tabs (the `_RevealTabInStrip` gotcha) before a test reads them |
| The VM costs the user's PC 6 GB and 4 cores | one desktop per run, deleted afterwards; runs on demand or at night |
| A test leaks into the real world | no real credentials in the guest; mock bound to loopback; the hosts-file guard; the guest firewall |
| Test hooks change behavior | env-gated, read-only bridge, timings that are existing constants (§6.4); the realtime lane runs without them |
| A wrong oracle (the app checks itself) | principle 1: the mock log and the transcript are independent of the app |

---

## 14. Decisions for the owner

| # | Decision | Recommendation |
|---|---|---|
| D1 | the first focus area | the Prompt Queue Runner (§10) — alternative: session lifecycle & state engine alone |
| D2 | the runner's language | Python in the guest (Python 3.13 + comtypes are already there; one language with the mock) — alternative: C# + FlaUI |
| D3 | test hooks in shipped builds | env-gated in every build (§6.1–6.2) — alternative: a separate test build flavor |
| D4 | Claude Code versions | the pinned + latest lanes (§5.4) |
| D5 | the accessibility changes of §6.3 | yes, area by area (they also serve screen readers) |
| D6 | where builds come from | a local Release build per test round now; a CI artifact per commit later |
| D7 | the desktop's size and lifetime | 6 GB / 4 CPUs, created per run, deleted after |
| D8 | AI-driven exploration (§9) | after Phase 4, never as a gate |

---

## Appendix A — the seeds and variables of a test machine

Measured on 2.1.291 (`tools/e2e-spike/spike_env.py` `seed_config`); re-check on every Claude Code version.

| Where | Key | Value | Removes |
|---|---|---|---|
| `~/.claude.json` | `hasCompletedOnboarding` | `true` | the first-run onboarding |
| `~/.claude.json` | `theme` | `"dark"` | the theme picker |
| `~/.claude/settings.json` | `skipDangerousModePermissionPrompt` | `true` | the "Bypass Permissions mode" warning |
| `~/.claude/settings.json` | `env.ANTHROPIC_BASE_URL` / `env.ANTHROPIC_AUTH_TOKEN` | the mock / a dummy | every claude reaches the mock, hand-typed ones included |
| user environment | `ANTHROPIC_BASE_URL`, `ANTHROPIC_AUTH_TOKEN` | same | the same for processes that skip user settings (the AI-search harness reads the app's environment) |
| user environment | `DISABLE_AUTOUPDATER`, `CLAUDE_CODE_SKIP_AUTO_UPDATE` | `1` | claude replacing itself mid-run |
| user environment | `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC` | `1` | telemetry, release notes, changelog fetches |
| user environment | `CLAUDE_CODE_NO_FLICKER` | per lane | the renderer chosen from hidden state |
| user environment | `CLAUDE_CODE_GIT_BASH_PATH` | the portable Git's `bash.exe` | the Bash tool's shell lookup |
| user environment | `CLAUDE_CODE_RESUME_INTERRUPTED_TURN` | `1` (as on the workstation since 2026-10-09) | without it a resumed cut-off or rewound turn does not continue by itself (PQR-42, PQR-51; HOOKS.md *Cut-off turns*, *API-error turns*) |
| Agentmaster launch | `AGENTMASTER_PROFILE`, `AGENTMASTER_E2E` (+ timings) | per test | the install question, the profile picker, the updater |
| NOT seeded | `projects.<dir>.hasTrustDialogAccepted` | — | Agentmaster seeds workspace trust itself; seeding it would hide that feature |

⚠ `ANTHROPIC_API_KEY` raises Claude Code's "use this API key?" question; use `ANTHROPIC_AUTH_TOKEN`.

## Appendix B — what a test looks like

> The design's sketch, written before the driver existed. The real API and a real test: [`e2e/README.md`](../../e2e/README.md)
> ("Writing a scenario") and `e2e/suites/`.

```python
@pytest.mark.pqr
@pytest.mark.L1
def test_three_queued_prompts_run_in_order_one_per_turn(app, mock, workdir):
    s = app.manager.launch_claude(workdir)            # the launch bar: type the folder, click Launch Claude
    s.terminal.send_prompt('[[mock reply "ready"]]')  # real keystrokes; verified in the mock log
    s.wait_state("Waiting for you")                   # tab ItemStatus + bridge agree
    app.manager.select_card(s)                        # click its board card
    for i in range(3):
        app.queue.compose(f'step {i} [[mock delay 3s; reply "done {i}"]]')
        app.queue.add()                               # the envelope button (am.mgr.queue.add)
    reqs = mock.wait_prompts(session=s.id, texts=["step 0", "step 1", "step 2"], timeout=120)
    assert mock.overlaps(session=s.id) == []          # never two turns at once
    assert s.transcript.user_prompts()[-3:] == [r.prompt for r in reqs]
    app.queue.expect_rows(upcoming=0, sent_flight=3)  # UIA: the rows moved to SENT
```
