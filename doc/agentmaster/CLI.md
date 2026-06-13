# Agentmaster CLI (`agentmaster <verb>`)

> Make the fleet **queryable from the commandline** — list every window, tab and session, and
> introspect any one of them deeply enough that an AI agent understands *what is going on inside a
> tab* without ever seeing the UI. Plus a small, reversible **control** surface (`restore` /
> `archive`).

This is the textual twin of the Manager UI: the Triage Board, the per-tab overlay, and the Flight
Plan, rendered as JSON (for agents) or a compact human table. It is built to **always work** — app
running or not — by reading the *same persisted + OS-observable state the Fleet Observer already
trusts*, never by depending on a live process to answer a query.

Related: [`DESIGN.md`](DESIGN.md), [`OBSERVER.md`](OBSERVER.md) (the pull model this reuses),
[`SESSIONS.md`](SESSIONS.md) (the on-disk `~/.claude` map + `TranscriptStore`),
[`STATE.md`](STATE.md), [`PROFILES.md`](PROFILES.md) (per-install state dir the CLI resolves).

---

## 1. The constraint that shapes the transport

Three facts decide the architecture:

1. **The app exe is GUI-subsystem.** `WindowsTerminal.exe` (the alias target's forward) cannot write
   to the caller's stdout — the shell does not wait on a GUI child, so any `AttachConsole` output
   races the next prompt. This is *why* `wt.exe` never returns query output. A queryable CLI must be
   a **console-subsystem** binary.
2. **Single-instance handoff is one-way.** `acquireMutexOrAttemptHandoff` (`WindowEmperor.cpp`) sends
   `WM_COPYDATA` then `TerminateProcess`es — there is **no response channel** back to the caller.
3. **State is split.** The live `SessionRegistry` + `ProcessObserver` tables are in-process only —
   but the engine's primitives are **pure C++ and already standalone** (`ProcessInspect` reads PEBs,
   `TranscriptStore` reads conversations, `Persistence` reads `sessions.json` / `windows/*.json`).
   The test harness links them with no app. So most of "what's going on" is reconstructable
   **without** the app running.

## 2. Transport: overload the alias shim (minimal, always-works)

The execution alias `agentmaster.exe` / `agentmasterdev.exe` already targets the tiny **`wt` launcher
shim** (`src/cascadia/wt/shim.cpp`), not the GUI — it just `CreateProcessW`s `WindowsTerminal.exe`
and exits. Defterm/COM handoff targets `WindowsTerminal.exe` *directly* (manifest `com:ComServer`),
so it is untouched by anything we do to the alias. The shim is the seam.

**Change:** flip the alias-target launcher to `/SUBSYSTEM:CONSOLE` and dispatch on `argv[1]`:

```
wmain:
  if argv[1] ∈ { show, list, sessions, tabs, windows, external, restore, archive, --help, --version }
       → exec agentmaster-cli.exe on THIS console, wait, return its exit code     # CLI
  else → CreateProcessW(WindowsTerminal.exe, <forward argv verbatim>) detached     # GUI (unchanged)
```

- A verb invoked **from a shell** attaches to the existing console → synchronous, ordered stdout
  (the shell waits for a console child). No flash.
- The **only** no-console caller of the alias is our own `reopen` `ShellExecute`, where no human is
  watching a console; a one-line `GetConsoleProcessList()==1 → FreeConsole()` guard removes even that
  sub-perceptible flash before forwarding.
- Start-menu / taskbar / `shell:appsFolder` activate the `App` entry directly (not the alias);
  defterm uses the COM server directly. Neither regresses.

The forward path stays **byte-for-byte** the default for everything that is not a known verb, so the
launch path, single-instance handoff, and defterm are preserved. The launcher delegates verbs to a
separate **`agentmaster-cli.exe`** (console; links only the pure-C++ `AgentMaster/*.cpp`) so engine
deps never load on a plain window launch.

## 3. Data sources: read from state that already exists

The Fleet Observer already proved the entire live picture is derivable **purely out-of-band** — PEB
(cwd/cmdline/model/effort) + transcript (conversation/title/branch/timing) + presence file
(busy/idle/waiting): "the always-correct floor beneath the lossy hook push" (OBSERVER.md). **The CLI
is that same survey, run one-shot from a separate process**, joined with the files the engine
already persists. It therefore depends on no live responder and **always works, app up or down**.

| The CLI reads… | from | freshness |
|---|---|---|
| sessions + queues + autopilot + titles | `sessions.json` (`Persistence::LoadSessions`) | on-change autosave |
| tabs: order, window↔session, selected, geometry, lens | `windows/<id>.json` (`LoadWindowRecords`) | ≤750 ms debounce |
| conversation tail, last reply, prompts, branch | transcripts (`TranscriptStore` / `ProcessInspect`) | live to last flush |
| live process facts, model, effort, alive, external census | PEB (`ProcessInspect`) | live |
| derived state + busy/idle/waiting | transcript-tail + `sessions/<pid>.json` | live |

The only truly live-only facts (in-memory hook `SessionState`, the transient `Held` flag) are exactly
what the Observer re-derives from transcript-tail + presence, so the CLI emits a **`derivedState`**
plus the raw `presence`, and is honest about it. **P1 reads touch no engine code and need no IPC.**

> Future (optional, not P1): a live request/response query pipe on `SharedEngine` for zero-lag
> in-memory fidelity. Deliberately deferred — the state-only model is more robust and far smaller.

## 4. Control: ride the existing handoff (`restore` / `archive`)

`restore` re-launches an archived session as a live tab in a window; `archive` closes a live one.
Both are intrinsically **GUI actions in a window** — a CLI process cannot open/close a tab itself. So
control verbs **dispatch through the existing single-instance `WM_COPYDATA` handoff** rather than a
new pipe: the CLI hands the running Emperor an internal commandline —

```
agentmaster --am-restore <sessionId> [--window <id>]
agentmaster --am-archive <sessionId>
```

— which the Emperor recognizes and routes to the **existing seams** (`_RestoreArchivedSession` /
`_ArchiveAndCloseClaudeTab`). One-way is fine: the CLI **confirms by polling the disk** (`sessions.json`
flips `live`) with a timeout, consistent with its disk-based reads. App down ⇒ a clear "start
Agentmaster first" (you cannot restore into a window that does not exist). No new IPC, no control
sink, no `SessionInfo` change — a small additive intercept in commandline parsing + reuse of seams.

## 5. Command surface

```
agentmaster show <ref> [--tail N] [--json]   # full introspection of one session/tab — the heart
            list                              # fleet overview: windows → tabs → sessions, by state
            sessions [--state S] [--dir D] [--archived]
            tabs [--window W]                 # every tab across windows (claude + shells), kind, link
            windows                           # geometry, ordered tabs, Manager lens
            external                          # unmanaged-claude census (real-WT / bare console)
            restore <ref> [--window W]        # archived → live   (control; app required)
            archive <ref>                     # live → archived   (control; app required)

global: --json  --profile <dir>  --instance dev|release  --offline  --self  --help  --version
```

- **`<ref>`** resolves a conversation UUID / unique id-prefix / title substring / `w<N>:t<M>` tab-ref /
  `--pid <n>`; ambiguity prints the candidates and exits non-zero.
- **`--self`** introspects the tab the agent is *calling from* (reads its own `WT_SESSION` and
  correlates) — "what's my queue, am I being autopiloted, what window am I in".
- **`--profile` / `--instance`** retarget another install's state (release vs dev, or an explicit
  folder); default is the active profile resolved exactly as the engine resolves it.

### The `show` payload (the "understand the tab fully" contract)

A faithful render of the per-tab overlay + Flight Plan + Triage card combined:

- **Identity / placement** — id, title, workingDir (+ live PEB cwd), branch, model, effort,
  permissionMode; window (id + index), tab index, focused?, color, **link state** (linked /
  observe-only / external), **autopilot** mode.
- **State** — `derivedState` (Running / WaitingForInput / NeedsApproval / Error / Idle / Done) +
  raw `presence` (busy/idle/waiting), turn-in-flight?, created-ago / active-for / last-activity-ago,
  pid + alive, hook provenance (`hookWired`, last hook/observed).
- **Activity (what it's actually doing)** — message count, tool-call count, and **`filesTouched`**
  (the dirs/files the session's tool calls have hit — `TranscriptStats::pathsAccessed`), so a consumer
  sees *what the session is working on* at a glance.
- **Conversation (the substance)** — the last *N* turn pairs, the **last assistant reply verbatim**
  (where it left off), **`lastUserPrompt`** (the human's last real ask), last-turn tool calls; if
  NeedsApproval → the pending interactive tool; if waiting on a question → the question text.
- **Flight Plan** — the full queue, each prompt `{label, text, status, origin (flight/typed), gate,
  sentAt}`, plus the sent history and the autopilot backstops
  (maxAutoSends / autoSendsThisRun / stopOnError / pauseOnHumanInput).

A live claude that is *not* in the queried profile's `sessions.json` (managed by the other instance,
or not yet persisted) still gets the **full transcript-derived view** — `show` synthesizes a record
from the live process + transcript, so identity / state / activity / conversation are always present;
only the queue/autopilot are blank. The `external` census additionally classifies each unmanaged
claude's **`host`** — `windows-terminal` / `agentmaster-other` / `console` — so a sibling instance's
session is never mistaken for a truly-foreign one.

Default output is a compact human summary; `--json` (stable, `schemaVersion`-stamped) is the agent
mode.

## 6. Conversation binding + offline state derivation

**Binding a live claude to its conversation id** (most authoritative first), so a claude in a working
dir shared by *several* concurrent claudes is never mis-bound (Rule #14's hard case, which a one-shot
cwd→transcript guess gets wrong):

1. claude's **own presence self-report** (`sessions/<pid>.json`), matched by **PID** — pid-keyed, so
   it is immune to cwd density (the decisive signal; verified to fix `--self` in a 5-claude cwd);
2. an explicit `--session-id` on the command line;
3. `--resume <guid>`;
4. the cwd→transcript creation-time resolution (`ResolveSessionId`, the fragile fallback).

**State.** In-memory hook `SessionState` is in-process only. Offline, the CLI derives an equivalent the
way the Observer/Scanner already do, from two live, file-backed signals:

- **presence** (`~/.claude/sessions/<pid>.json`, claude's own heartbeat): `busy → Running`,
  `waiting → WaitingForInput` (or `NeedsApproval`), `idle/shell → Idle`, none → `archived`/unknown.
- **transcript tail** (`ParseTranscriptDelta` over the last ~128 KB): a terminal `stop_reason`
  (`IsTerminalStopReason`) ⇒ turn complete; an unanswered interactive tool (`AskUserQuestion`,
  `IsInteractiveTool`) ⇒ NeedsApproval; an interrupt marker ⇒ turn ended; a recent non-terminal tail
  ⇒ Running. (These are the same `SessionScanner` predicates.)

The `presence` value and the transcript-derived signal are both emitted, so a consumer never has to
trust a single heuristic.

## 7. Profiles & multi-instance — **the binary targets its build type**

The release and dev packages install side by side, each with its own profile (`~/.agentmaster` vs
`~/.agentmaster-dev`). The CLI must target the *right* one without the user thinking about it. It does,
by a precedence ladder (highest first) — **so in normal use you never pass a flag**:

1. **`--profile <dir>` / `--instance dev|release`** — explicit override (cross-target on purpose).
2. **MSIX package identity (PACKAGED)** — a packaged `agentmaster-cli.exe` resolves via
   `GetCurrentPackageFamilyName()` → the dev exe lands on the dev profile, the release exe on the
   release profile. The alias you typed (`agentmasterdev` vs `agentmaster`) IS the explicit "which
   install" choice, so it **wins over any inherited `AGENTMASTER_PROFILE`** — the CLI clears the
   ambient env first, then lets `Profiles::ResolveProfileDir()` resolve by identity + the saved choice
   (so a Browse…-picked custom folder still wins; it lives in `.agentmaster.profiles` by identity).
   **Automatic, no flag, no hardcode.** (Verified live: `agentmasterdev list` → `~/.agentmaster-dev`
   even from a release-hosted shell.)
3. **Inherited `AGENTMASTER_PROFILE` env (UNPACKAGED)** — an unpackaged build run *inside* an
   Agentmaster tab inherits the app's exported profile, so it auto-targets that instance.
4. **Compile-time brand (UNPACKAGED)** — `-DAGENTMASTER_DEV` so the unpackaged dev CLI defaults to
   `~/.agentmaster-dev` when run outside any app (release omits it → `~/.agentmaster`). The literal
   "the binary targets its build type" knob. (Verified: `env -u AGENTMASTER_PROFILE` → dev.)
5. **Per-identity default** (`ResolveProfileDir` fallback — `~/.agentmaster`).

All resolution is **headless-safe** (never shows UI). So: shipped → automatic by the **alias's
identity**; unpackaged dev build → automatic by brand; an unpackaged run in a tab → by inherited env.
The flags exist only to cross-target another install on purpose.

**Dispatch** (`shim.cpp`): the launcher routes to the CLI when the first commandline token is a verb
(the standard verb-first form, `agentmaster show …`) OR a CLI-only global flag (`--json` / `--self` /
`--offline` / `--tail` / `--instance` / `--state` / `--dir` — none collide with a WindowsTerminal
token), so `agentmaster --instance dev show …` dispatches too. Anything else (`-w`, `-s`, `nt`,
`-Embedding`, bare) forwards to the GUI unchanged.

## 8. Build & packaging

- **`agentmaster-cli.exe`** — a new console-subsystem project (mirrors `wt.vcxproj`'s onecore shape)
  that compiles the pure-C++ engine units (`SessionRegistry` / `Persistence` / `ProcessInspect` /
  `TranscriptStore` / `SessionScanner` / `SessionSearch` / `ClaudeSpawn`) — **no WinRT, no
  `TerminalAppLib`** — and links `ole32 user32 oleaut32` (the `tests/` harness link set). Added to
  `CascadiaPackage` so it ships beside `WindowsTerminal.exe`.
- **Launcher** — `wt/shim.cpp` flips to console subsystem + the verb dispatch in §2.
- **Emperor** — the `--am-restore` / `--am-archive` commandline intercept (§4).
- **Standalone validation** — like the engine tests, `agentmaster-cli` compiles + runs via a `cl`
  one-liner (`AgentMaster/cli/_compile.bat`) for fast iteration without the package build.

## 9. Phasing & status

- **P1 — read (hybrid, standalone reader):** `show` / `list` / `sessions` / `tabs` / `windows` /
  `external` / `--self`. Zero engine edits. **Code COMPLETE + QA'd** (presence-authoritative binding,
  host classification, activity/files/last-prompt). **Packaging authored + PROVEN:**
  `agentmaster-cli.vcxproj` (console; links the engine units) builds clean; the `wt`/`wtd` shim flips
  to console-subsystem + verb-dispatch (`shim.cpp` + `wt.vcxproj` `SubSystem=Console`) — verified
  end-to-end (`wt.exe show --self` dispatches to the CLI with full output; non-verbs forward to the
  GUI byte-for-byte); wired into `OpenConsole.slnx` + `CascadiaPackage.wapproj` so a build ships
  `agentmaster-cli.exe` beside `WindowsTerminal.exe`. **Remaining: the destructive package
  build + deploy** (close → build → relaunch) to make the real `agentmaster show` alias live — which
  must be run from OUTSIDE the dev instance being closed (self-kill hazard).
- **P2 — control:** the `--am-restore` / `--am-archive` handoff intercept + disk-poll confirm →
  `restore` / `archive`.
- **P3 — TODO:** a `watch` event stream, and **prompt control** (`enqueue` / `send-now` /
  `set-autopilot`) so an agent can *drive* other sessions — higher-stakes (it injects prompts), so it
  is deliberately a separate phase.

## 10. Invariants (do not regress)

- **Reads never depend on a live responder** — only persisted + OS-observable state, so the CLI works
  app-up or app-down.
- **The CLI never writes to a shell's stdin** and never injects — it upholds the Observer's
  invisibility invariant (Rule #13). Control acts only through the app's own seams.
- **The forward (GUI-launch) path of the alias shim is unchanged** for every non-verb commandline —
  defterm (`-Embedding`), reopen (`-w -s`), and bare launch must all behave exactly as before.
- **One profile, resolved once, headless-safe** (Rule #15) — the CLI reads through
  `AgentmasterStateDir()`; it never shows UI and never re-homes state.
