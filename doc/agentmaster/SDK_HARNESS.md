# The agent harness (Claude conversations that Agentmaster hosts itself)

> Status: written 2026-10-10. The app was built with it in both configurations on 2026-10-11
> (compile and link); **no build with it is deployed**, and the Sessions page's AI search was never
> run on screen. The permission policy, the inventory report and the removal of the MCP-resource
> tools are of 2026-10-12 and are **NOT BUILT**: checked with `cl /Zs` (the flags of that build),
> the m5 suite, and the live test against the real `claude.exe` and Sonnet 5.5 (72 checks,
> section 8).

Agentmaster manages Claude Code sessions that the user drives in a terminal. The harness is the
opposite case: a Claude conversation that **Agentmaster itself** drives, with no terminal and no
tab, for a feature of its own. It is what `claude -p` is for a script, kept open: one windowless
`claude.exe` that stays up between messages, speaks stream-json in both directions, and calls
tools that run **inside** Agentmaster.

The first feature that uses it is the AI search of the Sessions browser
([`SESSIONS.md`](SESSIONS.md) section 9). The harness itself knows nothing about sessions: a
caller gives it a system prompt and tools, sends messages, and gets events.

| Part | File | Needs the SDK? |
|---|---|---|
| The conversation: process, turns, events, tools | `AgentMaster/AgentHarness.{h,cpp}` | the `.cpp` only |
| The model-free half: record, digest, launch policy, readers | `AgentMaster/AgentHarnessCore.{h,cpp}` | no |
| The SDK, compiled as one unit | `AgentMaster/ClaudeSdkUnity.cpp` | yes |
| The Windows process backend (replaces the SDK's) | `AgentMaster/ClaudeSdkProcessWin32.cpp` | yes |
| The SDK source, pinned | `refs/claude-agent-sdk-cpp` (git submodule) | — |

## 1. The shape

```
caller thread            worker thread                  SDK reader thread
-------------            -------------                  -----------------
Send(text)  ──────────►  start the process if needed
                         send the user message  ──────► claude.exe
                         read the stream        ◄────── assistant / system / result lines
                                                        a tool call: HarnessTool::handler runs HERE
EventSink(event) ◄────── state, text, the turn's end    ToolCall / ToolResult events
```

- **One process per conversation.** A follow-up message costs no process start and keeps its
  context. A start takes 0.7 to 1.0 s; one model round trip takes about 1 s (measured, section 10).
- **Every method returns at once.** A caller never waits for the model. Results arrive as events,
  on the conversation's own threads.
- **The newest message wins.** A `Send` while a turn runs interrupts that turn. This is the
  behavior that a search box needs.
- **A reset is an epoch.** `Reset` ends the process, clears the record and raises `Epoch()`. Every
  event carries its epoch, so a view drops what belongs to a conversation that is gone.

## 2. How the SDK is built into the app

The SDK is [`0xeb/claude-agent-sdk-cpp`](https://github.com/0xeb/claude-agent-sdk-cpp) (MIT, C++17),
a git submodule at `refs/claude-agent-sdk-cpp`, pinned to commit `9fd28724`. It is a CMake project,
and Agentmaster is MSBuild. There is no second project file: **one translation unit**,
`ClaudeSdkUnity.cpp`, includes the SDK source files that the harness uses.

- **Included:** `version.cpp`, `errors.cpp`, `types.cpp`, `protocol/control.cpp`,
  `internal/message_parser.cpp`, `client.cpp`, `internal/transport/subprocess_transport.cpp`.
- **Not included:** `internal/subprocess/process_win32.cpp` (replaced, section 3), `query.cpp`,
  `sessions/`, `ext/`, `testing/` (not used).
- **Warnings.** The app compiles with `/W3 /WX /sdl` and five more warnings as errors. The SDK is
  written for other settings, so its code is inside a `warning(push)` / `warning(pop)` region that
  disables 14 warnings. The SDK code is not changed.
- **`atexit`.** The SDK registers an exit handler that reads function-local statics. Those are
  destroyed before the handler runs. The unity file redirects that one registration to
  `at_quick_exit`, which a normal exit never runs. The app's own clean-up of the child is the job
  object (section 5).
- **nlohmann/json** is the SDK's JSON library. It comes from vcpkg: the manifest's `terminal`
  feature lists `nlohmann-json`, pinned to the version of the repository's vcpkg baseline
  (3.12.0, port revision 2).
- **Include folders.** Only the three files that include SDK headers get them, in
  `TerminalAppLib.vcxproj`: `refs\claude-agent-sdk-cpp\include` and `...\src`.
- **A build needs the submodule.** `git submodule update --init refs/claude-agent-sdk-cpp`. The
  two GitHub workflows check out with `submodules: true`, and their vcpkg cache is keyed on
  `vcpkg.json`.

⚠ **`nlohmann::json member{ other }` builds a one-element ARRAY.** The braces select the
initializer-list constructor. A tool whose input schema was stored this way was listed as
`"inputSchema":[{…}]`, Claude Code rejected the tool list, and the model said that it had no tools.
Initialize JSON members with parentheses (`AgentHarness.cpp`, `DynamicTool`).

## 3. The Windows process backend

The SDK starts Claude Code through an internal interface (`claude::subprocess::Process`,
`ReadPipe`, `WritePipe`). Its Windows implementation has three defects, so
`ClaudeSdkProcessWin32.cpp` implements the same interface again.

| The SDK's `process_win32.cpp` | Agentmaster's file |
|---|---|
| `ReadPipe::has_data(timeout)` ignores its timeout. The transport's reader loop then spins: one thread uses a whole CPU core for as long as the child lives. | The read end is an overlapped named pipe. `has_data` waits on the pending read's event. Measured: 3 s of an idle conversation cost 0 ms of CPU. |
| ANSI only (`CreateProcessA`, `GetEnvironmentStringsA`). It does not compile with `UNICODE`, and a path outside the ANSI code page breaks. | `CreateProcessW` and a UTF-16 environment block. |
| No way to remove an inherited environment variable. | An override with an **empty value removes** the variable from the child. |

It also quotes arguments by the exact `CommandLineToArgvW` rules (the SDK doubles every backslash),
refuses a `.cmd` / `.bat` (the app runs the native `claude.exe` only), gives the child only the
three pipe handles (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), and starts it suspended, puts it in a
kill-on-close job, then resumes it.

**A child that dies is noticed at once.** The SDK's reader loop ends quietly when the child exits,
and a control request that waits for an answer (the `initialize` handshake) then waits 60 s. The
harness wraps the transport (`WatchedTransport`): after the child has exited, a read throws, and
every pending request fails. A missing `claude.exe` fails in 3 ms.

The interface is internal to the SDK. It is safe to implement it here only because the SDK is
pinned. Section 9 lists what to check after a move of the pin.

## 4. The conversation

`HarnessConversation` (`AgentHarness.h`) is the whole API.

| Method | What it does |
|---|---|
| `Prewarm()` | Start the process now, so that the first `Send` finds it ready. |
| `Send(text)` | Ask. Returns the turn's id. A running turn is interrupted (newest wins). |
| `Interrupt()` | Stop the running turn. The context keeps what was said. |
| `Reset()` | Throw the conversation away: process, record, file. The epoch goes up. |
| `ParkAfter(ms)` | End the idle process after a delay, unless a `Prewarm`, a `Send` or a `Reset` comes first. The conversation is kept (section 6). |
| `Shutdown()` | End the process and the worker for good. Blocks. |
| `State()`, `Epoch()`, `Record()` | The state now. |
| `Inventory()` | What the process reported about **itself**: the permission mode in effect (known when the process is ready, before any model request) and, after its first turn, every tool, tool server, slash command, skill, agent type and plugin its model has. Use it to check a conversation instead of trusting its options. |
| `DescribeCommandLine()` | The command line the SDK builds for the next start, for a test or a log. |

**Events** (`HarnessEvent`): `State`, `Ready` (with the model, the version and the permission
mode), `UserMessage`, `AssistantDelta`, `AssistantText`, `ToolCall`, `ToolResult`, `Compacting`,
`Compacted`, `TurnCompleted`, `Notice`, `Error`. One sink call runs at a time. The sink may call
`Send` or `Reset`.

**Three guards keep a turn from hanging:**

1. An interrupt that gets no result in 10 s ends the process.
2. A turn that is silent for `turnSilenceTimeoutMs` ends as an error. Time inside a tool handler
   does not count.
3. `maxTurnsPerMessage` bounds how many model requests one message may use.

**A tool handler runs on the SDK's reader thread.** While it runs, that thread delivers nothing
else, not even the interrupt of the same turn. A handler must return fast, and a handler that can
be slow must ask whether its request is still wanted (the search tool does: `SESSIONS.md` 9.4).

**Park.** An idle `claude.exe` holds about 225 MB of working set and 400 MB of private memory
(measured). `ParkAfter` ends it when nobody uses the conversation. The next process gets a digest
of the record, not the model's whole context, so the delay must be minutes.

## 5. The launch policy, and the fleet

The harness decides the launch for every conversation (`AgentHarnessCore.h`).

**The command line** (built by the SDK from the options):

```
claude.exe --output-format stream-json --input-format stream-json --verbose
  --system-prompt-file <file> --allowedTools mcp__<server>,… --tools ""
  --model claude-sonnet-5-5 --permission-mode bypassPermissions --strict-mcp-config
  --settings <json> --mcp-config {"mcpServers":{…"type":"sdk"…}} --setting-sources ""
  --autocompact 500000 --dangerously-skip-permissions --disable-slash-commands
  --no-session-persistence --thinking disabled --effort low
```

- **No built-in tools** (`--tools ""`): the model can call only the caller's tools.
- **Nothing of the user**: no settings, no `CLAUDE.md`, no hooks, no MCP servers
  (`--setting-sources ""`, `--strict-mcp-config`), and the system prompt replaces Claude Code's own.
- **Nothing written under `~/.claude`** (`--no-session-persistence`): the conversation never shows
  up among the user's sessions.
- **Bypass permissions, always** (the next part).

### 5.1 The permission policy

**The rule (the user's, 2026-10-12):** a harness process always runs with **bypass permissions**,
and in **auto mode** when bypass is not allowed. The harness adds one last step for the case that
auto is not allowed either: **`dontAsk`**.

A harness process has no window, so nobody can answer a permission question. Each of the three
modes decides by itself:

| Mode | A tool call that no allow rule covers | A tool call on the allowed list |
|---|---|---|
| `bypassPermissions` | runs | runs |
| `auto` | a classifier model decides: 0.3 to 0.6 s per call (measured) | runs at once |
| `dontAsk` | refused | runs |

Every other mode can ask (`default`, which is the wire word of `manual`; `acceptEdits`; `plan`).

⚠ **With bypass, nothing refuses a tool call.** What the model can do is decided by the tools it
is given (`--tools ""`, the tool servers), never by the permission mode. Give a conversation a tool
only when it may use that tool without limit.

**The allowed list stays** (`--allowedTools mcp__<server>`). Bypass does not read it. The two
fallback modes do: in `auto` it removes the classifier request, and in `dontAsk` it is what lets a
tool run at all.

**How "not allowed" shows** (Claude Code 2.1.291; nothing of it is printed):

| Case | What Claude Code does | What the harness does |
|---|---|---|
| A policy switches bypass off (`permissions.disableBypassPermissionsMode: "disable"` in any settings) | Starts, in `auto`, and says nothing | Reads the mode, keeps `auto`, logs `… bypassPermissions is NOT ALLOWED here, Claude Code chose auto` |
| The policy switches auto off too (`permissions.disableAutoMode: "disable"`) | Starts, in `default` | Asks for `auto` (refused: `Cannot set permission mode to auto: auto mode disabled by settings`), then for `dontAsk` (accepted) |
| The start itself is refused: `--restricted` (`Error: bypassPermissions not supported in restricted mode`), or a Claude Code that does not know a mode word (`error: option '--permission-mode <mode>' argument '…' is invalid`) | Ends at once with the error text | Starts again with the next lower mode, at most twice. The lower mode is then remembered for the conversation object's life |

**Where the mode is read.** The one sign of a lowered mode is what the process **reports**:
`current_permission_mode` in its answer to `initialize` (0.7 s after the start, before any model
request), and `permissionMode` in each turn's `system/init` line. `EnsureProcess` reads the first
before the state becomes `Ready`; `OnSystemMessage` compares the second at every turn's start.
The decisions are pure functions (`DecideHarnessPermissionStep`,
`HarnessStartFailureNamesPermissionMode`, `NextHarnessPermissionMode`); `SettlePermissionMode`
carries them out with the control request `set_permission_mode`.

- A switch **up** is never tried: `set_permission_mode bypassPermissions` is refused unless the
  process was started with bypass (`…because the session was not launched with
  --dangerously-skip-permissions`).
- An empty report (a Claude Code that has no `current_permission_mode`) changes nothing at start.
  The mode is then taken from the first turn. A blind switch could lower a process that runs in
  bypass.
- `default` in a process without a permission handler does **not** wait: a tool call that no rule
  allows is refused at once (`system/permission_denied`, "…but you haven't granted it yet"). The
  harness still moves to `dontAsk`, the mode that says so.
- A refused tool call is one `[harness] <name>: turn N: permission DENIED for <tool>: <reason>`
  line (the model gets the same text as that call's result).

**The test seam** (`HarnessOptions::permissionProbe`, no product code sets it): three switches that
make Claude Code refuse a mode for one conversation, so that each row of the table can be run on a
machine whose policy allows everything. The launch still asks for bypass; the refusal is Claude
Code's own.

### 5.2 The settings text, the environment and the fleet

**The `--settings` text** (`BuildHarnessSettingsJson`):

- **No `permissions` key in a product launch.** The mode is asked for on the command line, and the
  tools are limited by `--tools` and `--allowedTools`. Only the test seam writes the two policy
  switches of 5.1.
- ⚠ **`"autoCompactEnabled": true` beside `"autoCompactWindow"`.** With `--setting-sources ""`
  Claude Code falls back to the legacy `~/.claude.json`. A user who turned auto-compaction off
  there keeps it off, and `--autocompact 500000` alone then sets a window that never compacts
  (measured: `isAutoCompactEnabled: false`). With the key: `maxTokens 500000`,
  `autoCompactThreshold 467000`.
- Bounded API retries and no retry watchdog (a user's settings can make a failed request retry
  for hours), the effort variable (`CLAUDE_CODE_EFFORT_LEVEL` beats `--effort`), and the switches
  of a quiet background child: no auto-update, no nonessential traffic, no terminal-title model
  call, no `CLAUDE.md`, no auto memory, no background tasks.

**The environment** (`BuildHarnessEnvironment`): the variables of a parent Claude Code session or
terminal tab are **removed** (`CLAUDECODE`, `CLAUDE_CODE_SESSION_ID`, `WT_SESSION`, `AM_SESSION`,
the hook pipe, the effort and compaction variables, `DEBUG`, …). Agentmaster can itself be started
from inside a Claude Code session; a child that inherits those variables reports the parent's
session and tab as its own.

**The harness mark.** The child gets `AM_HARNESS=<conversation name>` (`kHarnessEnvMarker`,
`Activity.h`). It has no `WT_SESSION` and no `AM_SESSION`, so without the mark the Fleet Observer
would list it as an External "console" claude that the user could try to adopt.
`ParseClaudeFacts` reads the mark into `ClaudeProcessFacts::harnessName`, and both censuses skip
such a process: the app's (`ProcessObserver.cpp`, one `[observer] skipping harness claude` line
per process) and the CLI's (`agentcli.cpp`). It is never listed, adopted or reaped.

**The end of the child.** The app ends with `TerminateProcess`, so no exit handler runs. The child
is in a kill-on-close job object: when the app's process goes, Windows ends the child.

## 6. Persistence

`--no-session-persistence` means that Claude Code keeps nothing. What survives a restart is
Agentmaster's own **record** (`HarnessRecord`), one JSON file per conversation:
`<profile>\harness\<name>.json`, written atomically after every turn.

- **What it holds:** the items in order (user message, assistant text, tool call with its
  arguments, a short preview of each tool result, notices), the turn count, the model, the
  compaction count, the usage.
- **Caps:** 400 items, 8,000 characters per text, 2,000 per tool result. The record is a history
  and a digest source, not an archive.
- **A corrupt file reads as "no conversation"**, never as a wrong one. An empty record deletes its
  file.

**The digest.** A new process (after an app restart, or after a park) has none of the conversation
in its context. Its first user message starts with a digest of the last turns
(`BuildHarnessDigest`: each kept turn's user message, the tool calls with their arguments, the
assistant's text; no tool results). So "and the other one" still makes sense. The digest is a
summary, not the context.

**Why not the SDK's session mirror?** Claude Code's `--session-mirror` sends `transcript_mirror`
lines only when session persistence is ON. Under `--no-session-persistence` it sends nothing
(measured). So the record is built from the stream itself.

## 7. Tools

A caller gives the conversation **tool servers** (`HarnessToolServer`), each with tools
(`HarnessTool`: a name, a description, a JSON schema, a handler). The SDK serves them as in-process
MCP servers; the model calls a tool as `mcp__<server>__<tool>`.

- A handler gets the arguments as `json::Value` (Agentmaster's own JSON type; nlohmann stays inside
  `AgentHarness.cpp`) and returns a text and an error flag.
- `readOnly` marks a tool that changes nothing. Claude Code may run several such calls of one
  model step.
- An **error result is a message for the model**. It must say how to fix the call.
- The SDK's MCP server answers `notifications/*` with "method not found". The harness answers
  notifications with an empty result.
- **The model gets the caller's tools and nothing else.** The SDK's MCP server also announces a
  `resources` capability, which it does not have. Claude Code answers that claim with three more
  tools (`ListMcpResourcesTool`, `ReadMcpResourceTool`, `ReadMcpResourceDirTool`): 769 tokens in
  every request (measured: 2,224 instead of 1,455 for two tools), tools that can only fail, and
  with bypass permissions nothing refuses them. The harness removes the capability from the
  server's `initialize` answer (2026-10-12). The process's own tool list proves it
  (`Inventory().tools`).
- Each tool server is on `--allowedTools` (5.1).

**What the hosted process has** (its own report, `Inventory()`, Claude Code 2.1.291):

| Kind | In a harness process |
|---|---|
| Tools | the caller's tools only (the search agent: `mcp__sessions__search`, `mcp__display__show`) |
| MCP servers | the caller's in-process servers only (`--strict-mcp-config`) |
| Built-in tools (Bash, Read, Write, Agent, WebFetch, …) | none (`--tools ""`) |
| Slash commands, skills | none (`--disable-slash-commands`) |
| Agent types | 5 are **listed** (`claude`, `Explore`, `general-purpose`, `Plan`, `statusline-setup`), but the model has no tool to start one |
| Plugins | 2 built-in ones are loaded (`cc-plugin-agents-md`, `cc-plugin-plugin-authoring`); none of the user |
| Hooks, `CLAUDE.md`, auto memory, the user's settings | none (`--setting-sources ""`, the `--settings` switches) |
| Credentials | the Claude login (`apiKeySource: "none"`) |

## 8. Tests

**The m5 suite** (`tests\tests_harness.cpp`, no model, no SDK): the record's caps and JSON round
trip, a corrupt file, the atomic save, the digest, the `--settings` text, the permission policy
(the mode words, the step for each mode a process can report, which start failures are a refused
mode, the test seam's switches), the environment, the readers of Claude Code's lines (fed the
shapes captured from 2.1.291: `init` with its whole inventory, a refused tool call, the compaction
lines, the result), the harness mark in `ParseClaudeFacts`, and the search tools
(`SESSIONS.md` 9.8).

**The live test** (`tests\harness_live.cpp`, run by hand with `tests\run-harness-live.bat`) starts
the real `claude.exe` and makes real model calls: about twelve short Sonnet requests per run. It
proves, end to end, with the engine files the app compiles:

| # | What | Result (2026-10-12, Claude Code 2.1.291) |
|---|---|---|
| 1 | The command line the SDK builds | 11 checks; it asks for bypass with both switches |
| 2 | Start, tools, events, the inventory | ready in 700 ms; the process reports `bypassPermissions` before any message; search, show and a sentence in 3.5 s; the model has exactly the two harness tools |
| 3 | A follow-up keeps its context | 2.8 s |
| 4 | An idle conversation uses no CPU | 0 ms in 3 s |
| 5 | Newest wins | the older turn ends as interrupted |
| 6 | The record survives; a new process gets the digest | "show that one again" works in a new process |
| 6b | Park | the process ends, `Prewarm` brings one back, a `Prewarm` cancels a pending park |
| 7 | Reset | the epoch goes up, the file is gone, a fresh process is ready in 0.8 s |
| 8 | A missing `claude.exe` | fails in 3 ms |
| P1 | Bypass is not allowed (the policy switch) | ready in 0.7 s in `auto`; the tools run; the log names the reason |
| P2 | Neither bypass nor auto is allowed | Claude Code starts in `default`; the harness ends in `dontAsk`; the tools run; the first turn reports `dontAsk` too |
| P3 | Bypass is refused with an error at start (`--restricted`) | the second start (with `auto`) works, 1.1 s for both; no Error event; later starts ask for `auto` directly |
| 9 | `--agent "<request>"`: the Sessions search agent on this machine's sessions | `SESSIONS.md` 9.9 |

`run-harness-live.bat` builds optimized (`/O2`: the test prints latencies) into
`%AM_LIVE_OUT%`, never into the source tree. `harness_live.exe --index [days]` measures the session
index alone, with no model call. `--only-permissions` runs P1 to P3 alone (two short turns).

**Scenario 9 prints how a request was answered**, for a review: the process's command line, the
whole system prompt, each tool as the model reads it (name, description, argument schema), each
request step by step from the conversation's record (the tool calls with their arguments, the
results, the reply, each with its time), the process's own inventory, the token usage, and the
harness's `[harness]` log lines of the run. `--verbose` prints the tool results in full.

⚠ **The live test never runs in a real profile.** A shell that a running Agentmaster hosts carries
`AGENTMASTER_PROFILE` = that instance's live profile. `harness_live.exe` ignores an inherited
value and uses `%TEMP%\agentmaster-harness-live-profile` (override: `AM_LIVE_PROFILE`). This guard
exists because one early run of the exe without its script wrote 15 trace lines into the release
profile's `hooks.log` and refreshed 1,751 of its sidecar index files (in the release app's own
format; no session data was touched).

## 9. After a move of the submodule pin

1. Compare `src/internal/subprocess/process.hpp` and `process_win32.cpp` of the new commit with
   `ClaudeSdkProcessWin32.cpp`: the interface it implements is internal to the SDK.
2. Compare the SDK's `CMakeLists.txt` source list with the includes of `ClaudeSdkUnity.cpp`.
3. Check that `subprocess_transport.cpp` still has the one `std::atexit` call that the unity file
   redirects.
4. Syntax-check the three SDK-including files with the app's flags (`cl /Zs`).
5. Run the live test. Scenario 1 prints the command line; compare it with section 5.
6. Update the commit in section 2.

## 10. Measured facts about Claude Code that the design depends on

Claude Code 2.1.291, Sonnet 5.5, 2026-10-10. Re-check them after a Claude Code release that
touches headless mode.

- `--thinking disabled` is accepted; thinking tokens are then 0.
- Process start to ready: 0.7 to 1.05 s. One model round trip: 0.9 to 1.3 s. `claude -v`: 100 ms.
- `--autocompact` alone does not turn auto-compaction on (section 5).
- `--session-mirror` sends nothing under `--no-session-persistence` (section 6).
- The compaction lines: `{"type":"system","subtype":"status","status":"compacting"}`, then
  `{"type":"system","subtype":"status","status":null,"compact_result":"success"}`, then
  `{"type":"system","subtype":"compact_boundary","compact_metadata":{"trigger":"auto","pre_tokens":…,"post_tokens":…,"duration_ms":…}}`.
- **A result's failure flag is `is_error`, not `subtype`.** A wrong model name ends with subtype
  `success` and `is_error` true.
- `system/init` repeats at every turn and lists the tools, the MCP servers with their status, the
  model and the version — and `permissionMode`, `slash_commands`, `skills`, `agents`, `plugins`,
  `apiKeySource`, `output_style`. It arrives only when a turn starts, not at the process start.
- Assistant lines arrive one per content block, each with the same usage; `total_cost_usd` is
  cumulative per process.
- An idle process: about 225 MB of working set, 400 to 430 MB private.

**Permission modes** (2026-10-12; 25 starts of the real `claude.exe` with a wire probe and no
model request, then 7 short turns with the probe and 5 with the C++ stack):

| Asked at start | Settings | Reported `current_permission_mode` |
|---|---|---|
| `--permission-mode bypassPermissions` | — | `bypassPermissions` |
| `--dangerously-skip-permissions` | — | `bypassPermissions` |
| both | — | `bypassPermissions` |
| both | bypass disabled | `auto` |
| both | bypass and auto disabled | `default` |
| `auto` | — | `auto` (available for this account and Sonnet 5.5) |
| `auto` | auto disabled | `default` |
| `manual` | — | `default` (the wire word) |
| `dontAsk` | — | `dontAsk` |
| bypass + `--restricted` | — | no start: `Error: bypassPermissions not supported in restricted mode` |
| `auto` + `--restricted` | — | `auto` |
| an unknown word | — | no start: `error: option '--permission-mode <mode>' argument '…' is invalid. Allowed choices are acceptEdits, auto, bypassPermissions, manual, dontAsk, plan.` |

- The answer to `initialize` carries `current_permission_mode`, so the mode is known about 0.7 s
  after the start, before any model request. (The same answer holds the user's `account` object:
  the harness reads only the one key.)
- `set_permission_mode`: to `auto` and to `dontAsk` it succeeds from any mode (unless the mode is
  disabled: an error answer, `error_code: "auto_mode_settings"`); to `bypassPermissions` it
  succeeds only in a process that was started with bypass or with
  `--allow-dangerously-skip-permissions`.
- A refused tool call writes `{"type":"system","subtype":"permission_denied","tool_name":…,"message":…}`
  and gives the model the same text as the call's error result. The turn goes on.
- The modes change nothing in the prompt: the first request has the same 1,455 tokens in each.
- **Turn times, one search turn (2 searches, 1 show, 1 sentence):** bypass 3.4 to 3.5 s (3 runs);
  `auto` with the allowed list 4.1 to 4.8 s (3 runs). In `auto` the extra time is in the model
  request that follows a tool result; the cause was not looked for, and three runs each are few.
  A tool call itself starts within 10 ms in `auto` when the tool is on the allowed list, and
  after 0.3 to 0.6 s when it is not (the classifier). `dontAsk`: 2.9 and 3.1 s for a turn with
  one search (2 runs).

## 11. Open points

- **One live compaction through the C++ stack was not run** (it costs about 500K tokens). The
  compaction lines and the settings were measured with a wire probe; the readers are unit-tested
  with the captured lines.
- **The SDK's `message_parser` re-queues a line that fails typed parsing.** No stall was seen; a
  watchdog (section 4, guard 2) bounds it.
- **The record is per conversation name.** A second feature needs its own name; nothing else is
  shared.
- **Bypass on a machine that never accepted it was not measured.** Claude Code shows a one-time
  acceptance of bypass mode in a terminal (`bypassPermissionsModeAccepted` in `~/.claude.json`).
  This machine has accepted it. A headless start showed no dialog here; whether a machine that
  never accepted it starts in bypass, or in a lower mode (which the policy then handles), is not
  known.
- **A refused start is recognized by its text** (`HarnessStartFailureNamesPermissionMode`). The
  text reaches the harness through the child's stderr, which the SDK's reader stops reading when
  the child has exited. A line written in the child's last instant can be lost; the start then
  fails with the plain "process ended" text and is not repeated with a lower mode. Not seen in
  the live runs (P3 passed each time).
- **The harness has no built-in tools on purpose.** With bypass, a conversation that is given
  `Bash` or `Write` runs them without any check, in a process the user cannot see. Decide that
  per tool, never as a default.
