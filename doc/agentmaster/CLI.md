# Agentmaster CLI (`agentmaster <verb>`)

> Make the fleet **queryable from the commandline** — list every window, tab and session, and
> introspect any one of them deeply enough that an AI agent understands *what is going on inside a
> tab* without ever seeing the UI. Plus a small, reversible **control** surface (`restore` /
> `archive`).

This is the textual twin of the Manager UI: the Triage Board, the per-tab overlay, and the Auto
Testing queue, rendered as JSON (for agents) or a compact human table. It is built to **always
work** — app running or not — by reading the *same persisted + OS-observable state the Fleet
Observer already trusts*, never by depending on a live process to answer a query.

Related: [`DESIGN.md`](DESIGN.md), [`OBSERVER.md`](OBSERVER.md) (the pull model this reuses),
[`SESSIONS.md`](SESSIONS.md) (the on-disk `~/.claude` map + `TranscriptStore`),
[`STATE.md`](STATE.md), [`PROFILES.md`](PROFILES.md) (per-install state dir the CLI resolves).

---

## 1. The constraint that shapes the transport

Three facts decide the architecture:

1. **The app exe is GUI-subsystem.** `Agentmaster.exe` (the GUI binary — the alias target's forward; named `WindowsTerminal.exe` pre-rename) cannot write
   to the caller's stdout — the shell does not wait on a GUI child, so any `AttachConsole` output
   races the next prompt. This is *why* `wt.exe` never returns query output. A queryable CLI must be
   a **console-subsystem** binary. (Since 2026-10-23, NOT BUILT, the app exe still answers a CLI
   call that reaches it, by starting that console CLI on the caller's handles: §2a. Callers that
   wait for a GUI program get the full answer there.)
2. **Single-instance handoff is one-way.** `acquireMutexOrAttemptHandoff` (`WindowEmperor.cpp`) sends
   `WM_COPYDATA` then `TerminateProcess`es — there is **no response channel** back to the caller.
3. **State is split.** The live `SessionRegistry` + `ProcessObserver` tables are in-process only —
   but the engine's primitives are **pure C++ and already standalone** (`ProcessInspect` reads PEBs,
   `TranscriptStore` reads conversations, `Persistence` reads `sessions.json` / `windows/*.json`).
   The test harness links them with no app. So most of "what's going on" is reconstructable
   **without** the app running.

## 2. Transport: overload the alias shim (minimal, always-works)

The execution alias `agentmaster.exe` / `agentmasterdev.exe` already targets the tiny **`wt` launcher
shim** (`src/cascadia/wt/shim.cpp`), not the GUI — it just `CreateProcessW`s `Agentmaster.exe`
and exits. Defterm/COM handoff targets `Agentmaster.exe` *directly* (manifest `com:ComServer`),
so it is untouched by anything we do to the alias. The shim is the seam.

**Change:** flip the alias-target launcher to `/SUBSYSTEM:CONSOLE` and dispatch on `argv[1]`:

```
wmain:
  if first token ∈ { show, list, sessions, tabs, windows, external, restore, archive }
        OR a leading CLI-only flag { --json, --self, --offline, --tail, --instance, --state, --dir }
       → exec agentmaster-cli.exe on THIS console, wait, return its exit code     # CLI
  else → CreateProcessW(Agentmaster.exe, <forward argv verbatim>) detached          # GUI (unchanged)
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

Since 2026-10-23 the rule above (the verbs, the CLI-only flags, the argv[0] split) lives in ONE
header-only file, `AgentMaster/CliDispatch.h`, which the shim and the app exe both include (§2a):
add a new verb there, and both route it.

## 2a. The app exe answers a CLI call too (unpackaged installs; 2026-10-23, NOT BUILT)

**The incident (2026-10-23).** Two `agentmaster show a77419cf` calls from a Claude session put two
"Agentmaster — profile in use" dialogs on the user's screen. An install of v0.8.0 for all users was
in `C:\Program Files\Agentmaster` (made 2026-10-22), and its folder is on the machine PATH, which
Windows searches before the user PATH that holds the package's `agentmaster` alias. So `agentmaster`
found `Agentmaster.exe`, the app itself. An unpackaged copy has a single-instance identity of its own
(the exe-path hash in `WindowEmperor::HandleCommandlineArgs`), so it did not hand the command line to
the running package: it started a SECOND app on the user's profile, which stopped at the
profile-in-use warning. A Yes would have run two instances on one profile, and the app would then
have read `show a77419cf` as a new tab's command line. Every unpackaged install whose folder is on
PATH behaves the same: the per-user install (`~\.agentmaster\bin`), the one for all users, and a
portable folder someone added (PROFILES.md §2b).

**The fix.** `wWinMain` (`WindowsTerminal/main.cpp`) first calls `CliDispatch::TryRunCliCall`,
before the architecture check, COM, the single-instance hand-off, the install question and the
profile. A CLI call (§2's rule, from the shared header) runs the `agentmaster-cli.exe` beside the
exe with the caller's arguments unchanged, waits, and exits with its exit code. The app is never
started for a CLI call: when the CLI cannot be started, one line on the caller's error handle says
why, and the exit code is 1 (the shim's value). Any other command line starts the app as before;
`--help` and `--version` first still do (§11).

Where the CLI's output goes (`PlanCliLaunch`; the app exe owns no console of its own):

| The caller | The CLI's output | Creation flags |
|---|---|---|
| passed standard handles and has a console (bash, Python, Node, a redirect or a pipe in a terminal) | the caller's handles | none: the shared console |
| passed handles and has no console (pythonw, a scheduled task) | the caller's handles | `CREATE_NO_WINDOW` |
| has a console and passed no handles (cmd.exe, PowerShell's own console) | that console, through inheritable `CONIN$` / `CONOUT$` | none |
| neither (the Run box, a shortcut) | a console window of the CLI's own, closed when it ends, like the package's launcher started the same way | `CREATE_NEW_CONSOLE` |

**Who waits** (measured 2026-10-23 with a GUI-subsystem stand-in built on the header and a fake
CLI that exits 42). bash (Git Bash, Claude Code's Bash tool), Python's `subprocess`, Node and
`cmd /c` wait for the GUI exe and get the output and the exit code, also through a redirect, with
quoted arguments kept. An interactive cmd.exe does not wait, and PowerShell waits only when the
output is piped on: `& agentmaster show x | Out-String` waits, while a plain call and
`$o = & agentmaster show x` return at once with no output and no `$LASTEXITCODE` (the CLI still runs
and writes to the console). In those shells type **`agentmaster-cli <verb>`**, the console CLI in
the same folder on PATH. The table's last row was not run live: it would open a window on the
user's screen.

## 2b. Every dialog is echoed to the command's console (2026-10-23, NOT BUILT)

**Why.** In the §2a incident the "profile in use" dialog appeared on the user's screen, and the
Claude session that typed the command read nothing about it in its output. The user's request of
2026-10-23: every dialog Agentmaster opens is also written to the console of the command that
started the process, so a command that makes a dialog appear says so where it ran.

**What a dialog prints.** Before the dialog shows: its title, its text line by line, its choices.
Once it is answered: the answer. Every line starts with `[agentmaster dialog] `, so a reader (an
agent, `grep`) finds these lines among the command's output:

```
[agentmaster dialog] opened: Agentmaster — profile in use (Agentmaster.exe, pid 1234, waits for an answer on the desktop)
[agentmaster dialog] | The profile
[agentmaster dialog] |
[agentmaster dialog] |     C:\Users\<user>\.agentmaster
[agentmaster dialog] |
[agentmaster dialog] | is already in use by another running Agentmaster instance (...)
[agentmaster dialog] | choices: Yes | No (default)
[agentmaster dialog] answered: Agentmaster — profile in use: No
```

- A **task dialog** (the update prompt, the install question) lists each button (`button: Update now
  (default)`; a command link's note follows a dash), each option (`(o) Remind me next restart
  (default)`, `( ) Remind me in 3 days`), its check box (`[x]` / `[ ]`), its collapsed details and its
  footer links as text plus URL. The answer names the button, the option and the check box
  (`answered: Agentmaster: Postpone; option: Remind me in 3 days`). A progress dialog answers `closed`.
- A **file or folder picker** prints its title and `choices: pick a folder | Cancel`, and answers
  `chosen: <path>` or `cancelled`.
- A **UAC prompt** our code asks for (the install for all users, its uninstall, PROFILES.md §2b)
  prints `opened: User Account Control` with what the rights are for, and answers `approved`,
  `declined` or `refused: a policy forbids this account to elevate`.
- A dialog that could not open answers `not shown (...)`. One dialog's echo is cut at 8,000
  characters with a marker: a pipe nobody reads blocks its writer once it is full.

**Where the lines go: the caller sink** (`AgentMaster/CallerSink.h`). `wWinMain` captures it right
after the CLI check (§2a), before the first dialog can open. The first of three sources that exists
wins:

1. **The launcher's hand-over.** The package's alias runs the console launcher (`wt/shim.cpp`), which
   starts `Agentmaster.exe` and exits at once, so the app cannot find the caller by itself. The
   launcher names the caller's sink in `AGENTMASTER_CALLER_SINK`. A pipe or a file on stderr is passed
   as one inheritable duplicate (`handle:0x…`, the only handle in the start's
   `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, so nothing else is inherited, as before). A console is passed
   as up to 8 processes attached to it, the launcher's parent first (`console:<pid>,<pid>…`). The app
   reads and removes the variable at once, so no child of the app ever sees it.
2. **The standard handles the caller passed** (`STARTF_USESTDHANDLES`): stderr, else stdout, but only
   a handle this process really inherited (`HANDLE_FLAG_INHERIT` set, a pipe, a disk file or a
   character device).
3. **The parent's console**, attached for each write: `AttachConsole(parent)`, `CONOUT$`,
   `WriteConsoleW`, `FreeConsole`, with a Ctrl+C / Ctrl+Break handler for those milliseconds, so a key
   pressed in that console cannot end the app.

A pipe or a file gets UTF-8, a console gets `WriteConsoleW` (right in any console code page). Every
handle the sink keeps is a private, non-inheritable duplicate, and a console sink holds a
`SYNCHRONIZE` handle on each process it may attach through, accepted only when that process is older
than the app, so a reused process id can never receive the text.

**How long.** The sink belongs to the command, not to the app: `WindowEmperor` releases it where the
startup ends (before the message loop). A dialog still open then keeps it (`CallerSink::Hold`) until
its answer is written. Kept longer, a caller that reads the pipe to its end (bash `$(...)`, Claude
Code's Bash tool) would wait for the whole life of the app.

**A command line handed to the running app.** When another instance already runs, the new process
hands its command line over (`WM_COPYDATA`) and exits, and a dialog that line causes (Windows
Terminal's `--help` / `--version` box, a command-line error) opens in the RUNNING app. The sender
creates a small named section (`Local\AgentmasterHandoffReply-<pid>-<tick>`, 64 KB) and names it in
the payload as an optional fifth field. The receiver writes the dialog's title and text into it
before its `WM_COPYDATA` handler returns, and the sender echoes it, with a line saying the running
app shows the dialog and that its answer is not reported. An older receiver ignores the field, and
nothing is echoed.

**Enforced.** No code calls `MessageBoxW`, `TaskDialog`, `TaskDialogIndirect` or `IFileDialog::Show`
directly: it calls the wrappers in `AgentMaster/DialogEcho.h` (`ShowMessageBox`, `ShowTaskDialog`,
`ShowTaskDialogIndirect`, `ShowFileDialog`, `ShellExecuteWithEcho`). The m5 test
`TestDialogEchoCoverage` scans every `.cpp` / `.h` under `src\cascadia` of the checkout it was built
in, with comments and string literals stripped, and fails on a direct call. It also fails on any
other Win32 dialog family (the common file / folder / color / font / print dialogs, the shell's
message boxes, resource dialogs, the credential prompts): none is used today, and a first use needs a
wrapper. Two files are allowed, each with its reason: `DialogEcho.h` (the wrappers) and upstream's
`WinRTUtils\inc\Utils.h` (the settings editor's file picker, opened only from the running app's UI).

**What is echoed.** Every Win32 dialog of the app exe's startup: the architecture mismatch box, the
portable copy's install question, its progress dialog, its errors, the uninstall confirmation and
"Nothing to uninstall here", the profile picker and its folder browser, "profile in use", the update
prompt, "Reopen your N previous windows?", Windows Terminal's `--help` / `--version` / command-line
error box, and the UAC prompt of the install or uninstall for all users.

**What is not echoed, and why.**

- **The running app's own UI**: every dialog it opens after the startup, such as the hourly update
  prompt and the XAML dialogs inside a window (the settings load warnings, the close and quit
  confirmations, the Sessions page's questions, the Settings cog). They open after the startup
  released the sink, so no command waits on them. The Settings cog's message boxes and pickers go
  through the wrappers too, but `TerminalApp.dll` keeps its own copy of the sink state (an inline
  function's static is per binary) and never captures one.
- **UAC prompts that helper processes ask for**: `elevate-shim.exe` (upstream's helper that opens a
  tab whose profile asks for administrator rights; the running app starts it after the startup) and
  the update script's own elevation (`am-update.ps1`, after the app quit).
- **A caller with nothing to write to**: the Run box, a shortcut, our own reopen `ShellExecute` (no
  console and no handles). A caller that discarded stderr (`2>nul`) gets what it asked for.
- **A console caller that does not wait** (an interactive cmd.exe, PowerShell's own console): the
  lines still reach its console, after its next prompt.

**Measured** (2026-10-23, the probe kit `tools/dialog-echo-probe`: a GUI stand-in compiled on the
real headers opens REAL dialogs on a hidden desktop, `WinSta0\AgentmasterDialogProbe`, where each
answers itself; eleven checks, three runs, no failure):

- pipe caller: a message box, a task dialog with buttons, options and a footer link, a dialog still
  open when the release is requested, no dialog → no output;
- console caller: an interactive cmd.exe's way (no handles: the GUI attaches to its parent's
  console), a caller that passes the console's handles explicitly (a GUI process cannot use them:
  step 2 refuses them, and step 3 still lands in the console), and PowerShell 7 with the output piped
  on (stderr the console, refused; stdout PowerShell's pipe, which gets the lines);
- the REAL launcher shim (`wt/shim.cpp`, compiled by the kit) with a pipe caller and with a console
  caller, and `cmd /c <shim>`;
- no probe process left running.

The facts the design rests on, each measured with a probe:

- a `CONOUT$` handle opened while attached is dead after `FreeConsole` (error 6), so a console sink
  attaches for each write;
- a console handle passed to a GUI process is unusable (`HANDLE_FLAG_INHERIT` clear, file type 0,
  both write calls fail with error 6), also when the caller made it inheritable;
- cmd.exe passes its standard handles to its children by inheritance. PowerShell 7 passes none when
  the output is not piped on (like cmd.exe), and with `| Out-String` passes stdin and stderr as the
  console's handles and stdout as its pipe;
- under `cmd /c agentmaster ...` the launcher's parent, the cmd.exe, exits as soon as the launcher
  did, before the app writes. A hand-over naming that one process lost every line, hence the list of
  processes attached to the console.

## 3. Data sources: read from state that already exists

The Fleet Observer already proved the entire live picture is derivable **purely out-of-band** — PEB
(cwd/cmdline/model/effort) + transcript (conversation/title/branch/timing) + presence file
(busy/idle/waiting): "the always-correct floor beneath the lossy hook push" (OBSERVER.md). **The CLI
is that same survey, run one-shot from a separate process**, joined with the files the engine
already persists. It therefore depends on no live responder and **always works, app up or down**.

| The CLI reads… | from | freshness |
|---|---|---|
| sessions + queues + autorunner + titles | `sessions.json` (`Persistence::LoadSessions`) | on-change autosave |
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
            sessions [--state S] [--dir D] [--archived]   # --dir D: substring on workingDir, slash-INSENSITIVE (/ ≡ \)
            tabs [--window W]                 # every tab across windows (claude + shells), kind, link (--window: --json only, §11)
            windows                           # geometry, ordered tabs, Manager lens
            external                          # unmanaged-claude census (real-WT / bare console)
            restore <ref> [--window W]        # archived → live   (control; P2 — deferred, §9)
            archive <ref>                     # live → archived   (control; P2 — deferred, §9)
            search <words> [--dir D]          # every session on this computer, from the session database
            sql "<SELECT ...>"                # one read-only query over the database's views
            sessdb [status|stop|resync|rebuild]   # the session database service itself

global: --json  --profile <dir>  --instance dev|release  --self  --help  --version
```

**The session database verbs** (2026-10-15, NOT BUILT yet; [`SESSIONS_DB.md`](SESSIONS_DB.md) §8.3). `search`, `sql`
and `sessdb` are pipe clients of the profile's database service (`agentmaster-sessiondb.exe`, started when none runs;
`sessdb stop` never starts one). They read the database, not the live fleet, so they skip the fleet gather; they work
whether `AppSettings::sessionDbMode` is Off or not (a service started this way sweeps at its start, then answers).

- `search <words> [--dir D]` runs the database's `search` tool (20 rows, the last 90 days): one line per session (short
  id, last activity, why it matched — name / path / prompt / text — with the hit count, the title), its folder and
  branch, and its snippets with refs (`<id>:<line>`).
- `sql "<SELECT ...>"` runs the guarded read-only `sql` tool (200 rows, 2.5 s): tab-separated rows under a header. A
  statement that is not a SELECT, or a binder error, prints the database's message.
- `sessdb status` prints one `key: value` line per field (sessions, files, lines, blocks, terms, the file's size, the
  sweep and its progress, `coveredSince` while the first build runs, the write queue); `stop` asks the service to
  exit; `resync` sweeps and waits (up to an hour: a first build took 37 minutes on 3,600 sessions); `rebuild`
  re-derives the index from the stored lines.
- `--json` prints the service's result object as it came. Exit codes: 0, 1 (the service failed or answered an error),
  2 (usage: `sql` without a statement, an unknown `sessdb` sub-command).

(All reads are already offline-style — persisted + OS-observable state only — so there is no
distinct online mode; `--offline` is reserved as a shim dispatch token, not yet a parsed flag.)

`--help` and `--version` reach the CLI only behind a verb or a CLI-only flag (`agentmaster tabs
--help`, `agentmaster --json --version` → `agentmaster-cli schema 2`). As the FIRST token they are
an ordinary non-CLI commandline, so the launcher forwards them to the running GUI (§7), which shows
a message box.

- **`<ref>`** resolves a conversation UUID / unique id-prefix / title substring (`ResolveSessionRef`;
  a live claude managed by the *other* instance also matches by id/prefix — see below); ambiguity prints
  the candidates and exits non-zero. (`w<N>:t<M>` tab-refs and `--pid <n>` are designed but not yet parsed.)
- **`--self`** introspects the tab the agent is *calling from* (reads its own `WT_SESSION` and
  correlates) — "what's my queue, is the Prompt Queue Runner driving me, what window am I in".
- **`--profile` / `--instance`** retarget another install's state (release vs dev, or an explicit
  folder); default is the active profile resolved exactly as the engine resolves it.

### The `show` payload (the "understand the tab fully" contract)

A faithful render of the per-tab overlay + Prompt Queue + Triage card combined:

- **Identity / placement** — id, title, workingDir (+ live PEB cwd), branch, model, effort,
  permissionMode; window (id + index), tab index, focused?, color, **link state** (linked /
  observe-only / external), **autorunner** mode.
- **State** — `derivedState` (Running / WaitingForInput / NeedsApproval / Error / Idle / Done) +
  raw `presence` (busy/idle/waiting), turn-in-flight?, created-ago / active-for / last-activity-ago,
  pid + alive, hook provenance (`hookWired`, last hook/observed).
- **Activity (what it's actually doing)** — message count, tool-call count, and **`filesTouched`**
  (the dirs/files the session's tool calls have hit — `TranscriptStats::pathsAccessed`), so a consumer
  sees *what the session is working on* at a glance.
- **Conversation (the substance)** — the last *N* turn pairs, the **last assistant reply verbatim**
  (where it left off), **`lastUserPrompt`** (the human's last real ask), last-turn tool calls; if
  NeedsApproval → the pending interactive tool; if waiting on a question → the question text.
  *As built* (§11): the tail is the last *N* TEXT BLOCKS (`--tail N`, default 8 — each user prompt
  and each assistant text line is one entry, though the human view heads them "last N turns"), the
  pending tool is only its NAME (`AskUserQuestion`), and the last turn's tool calls are collected but
  never emitted. For the whole conversation turn by turn, the reference renderer is
  `transcript_turns.py` in the user-local agent skill (§11): one choice of detail level, from
  `--minimal` (each turn's message and final reply) through the default steps view (every tool call
  on one line) to `--max` (verbatim Bash, first output lines, written files, a stats line per turn).
  It is the baseline that any future CLI or in-app turns view is compared against.
- **Prompt Queue** — the full queue, each prompt `{label, text, status, origin (flight/typed), gate,
  sentAt}`, plus the sent history and the autorunner backstops
  (maxAutoSends / autoSendsThisRun / stopOnError / pauseOnHumanInput).

A live claude that is *not* in the queried profile's `sessions.json` (managed by the other instance,
or not yet persisted) still gets the **full transcript-derived view** — `show` synthesizes a record
from the live process + transcript, so identity / state / activity / conversation are always present;
only the queue/autorunner are blank. The `external` census additionally classifies each unmanaged
claude's **`host`** — `windows-terminal` / `agentmaster-other` (a sibling install's session, by its
`AM_SESSION` GUID prefix) / `agentmaster-self` (one this same instance stamped) / `console` — so a
sibling instance's session is never mistaken for a truly-foreign one.

A **harness child** is never listed (2026-10-10): the headless `claude.exe` that an Agentmaster runs
for the Sessions page's AI search carries `AM_HARNESS=<name>` in its environment, and every verb's
process census skips it, like the app's own census does ([`SDK_HARNESS.md`](./SDK_HARNESS.md)
section 5). It is not a session and has no tab.

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
4. **Compile-time brand (UNPACKAGED, dev identity only)** — `-DAGENTMASTER_DEV`: an unpackaged
   dev-identity CLI that inherits no `AGENTMASTER_PROFILE` defaults to `~/.agentmaster-dev`.
   (Verified: `env -u AGENTMASTER_PROFILE` → dev.) Since 2026-10-23 (NOT BUILT)
   `agentmaster-cli.vcxproj` defines it only when `AgentmasterPackageIdentity` is not `Release`.
   Until then EVERY build carried it, so the release CLI of a portable copy, of the per-user install
   and of the install for all users read `~/.agentmaster-dev` (§11). Checked by evaluating the
   project without building it (`msbuild -getItem:ClCompile`): none of its 17 units gets the
   define with `/p:AgentmasterPackageIdentity=Release`, all 17 do without it. The property is a
   global one on the command line (`release.yml`, `Build-Agentmaster.ps1 -ReleaseIdentity`), so the
   package build passes it on to this referenced project.
5. **`ResolveProfileDir`, as the app beside it resolves it** — the exe folder's `profile.path`, a
   portable's `<exedir>\profile`, the home map, else the per-identity default (`~/.agentmaster`).
   The CLI ships beside the app exe in every layout, so it reads the app's profile.

All resolution is **headless-safe** (never shows UI). So: shipped → automatic by the **alias's
identity**; unpackaged dev build → automatic by brand; an unpackaged run in a tab → by inherited env.
The flags exist only to cross-target another install on purpose.

**Dispatch** (`shim.cpp`): the launcher routes to the CLI when the first commandline token is a verb
(the standard verb-first form, `agentmaster show …`) OR a CLI-only global flag (`--json` / `--self` /
`--offline` / `--tail` / `--instance` / `--state` / `--dir` — none collide with a WindowsTerminal
token), so `agentmaster --instance dev show …` dispatches too. Anything else (`-w`, `-s`, `nt`,
`-Embedding`, bare) forwards to the GUI unchanged.

**The all-users copy hands every call to the user's own install** (2026-10-22, NOT BUILT; PROFILES.md §2b
*A user's own install wins*). Windows puts the machine PATH before the user's, so on a PC with an install
for all users (Program Files) AND the user's own per-user install, a typed `agentmaster-cli` finds the
shared copy first, and a user without administrator rights cannot change that. Its first step, before it
reads anything (`InstallHandoff::ForwardCliToUserInstall` in `wmain`), runs the user's own copy's
`agentmaster-cli.exe` instead: the same arguments, the same console (the caller's startup info and
inherited standard handles, so `--json > out.json` and a pipe keep working), and its exit code passed
on. Only a shared copy forwards (`Profiles::IsMachineCopyIn`), and never to itself or to another shared
copy (the user's HKCU Apps & Features entry names the target); `AGENTMASTER_NO_HANDOFF=1` makes the
shared copy answer itself. When the user's copy cannot be started, this copy answers (a running CLI
beats none).

## 8. Build & packaging

- **`agentmaster-cli.exe`** — a new console-subsystem project (mirrors `wt.vcxproj`'s onecore shape)
  that compiles the pure-C++ engine units (`SessionRegistry` / `Persistence` / `ProcessInspect` /
  `TranscriptStore` / `SessionScanner` / `SessionSearch` / `ClaudeSpawn`) — **no WinRT, no
  `TerminalAppLib`** — and links `ole32 user32 oleaut32` (the `tests/` harness link set). Added to
  `CascadiaPackage` so it ships beside `Agentmaster.exe`.
- **Launcher** — `wt/shim.cpp` flips to console subsystem + the verb dispatch in §2.
- **The CLI-call rule** — `AgentMaster/CliDispatch.h` (header-only, plain Win32): included by the
  launcher (§2) and the app exe's `main.cpp` (§2a); `TestCliDispatch` in the m5 harness covers it.
- **Emperor** — the `--am-restore` / `--am-archive` commandline intercept (§4).
- **Standalone validation** — like the engine tests, `agentmaster-cli` compiles + runs via a `cl`
  one-liner (`AgentMaster/cli/_compile.bat`) for fast iteration without the package build.

## 9. Phasing & status

- **P1 — read (hybrid, standalone reader):** `show` / `list` / `sessions` / `tabs` / `windows` /
  `external` / `--self`. Zero engine edits. **Code COMPLETE + QA'd** (presence-authoritative binding,
  host classification, activity/files/last-prompt), and **shipped by the full package build**:
  `agentmaster-cli.vcxproj` (console; links the engine units) plus the console-subsystem, dual-mode
  `wt`/`wtd` shim (`shim.cpp` + `wt.vcxproj` `SubSystem=Console`), wired into `OpenConsole.slnx` +
  `CascadiaPackage.wapproj`, put `agentmaster-cli.exe` beside the GUI exe in both built layouts.
  **Live on both aliases**, each auto-targeting its own profile by package identity — verified
  2026-10-01: `agentmaster --json --version` (the release install, 0.6.8.13, from its Release layout)
  and `agentmasterdev --json --version` (the dev install) both answer `agentmaster-cli schema 2`.
  ⚠ That check proves less than it seems: `--version` and `--help` return before the CLI reads any
  file. On 2026-10-09 the DEV alias failed on every real verb, because the Debug build of the CLI
  ends with `0xC0000409` while the process exits (§11; first seen 2026-10-06). The release alias
  passes every verb.
  (History: it first went live on the dev alias through an in-place binary swap, before any full
  build had run.) The JSON envelope is `schemaVersion` **2** since the Auto Testing rename
  (2026-06-30: `autopilot` → `autorunner`, `autopilotDetail` → `autorunnerDetail`, `flightPlan` →
  `autoTesting`). Where its answers differ from the app's, and what it leaves out: §11.
- **P2 — control (designed + deferred):** the `--am-restore` / `--am-archive` handoff intercept +
  disk-poll confirm → `restore` / `archive`.
- **P3 — designed + deferred:** a `watch` event stream, and **prompt control** (`enqueue` / `send-now` /
  `set-autorunner`) so an agent can *drive* other sessions — higher-stakes (it injects prompts), so it
  is deliberately a separate phase.

## 10. Invariants (do not regress)

- **Reads never depend on a live responder** — only persisted + OS-observable state, so the CLI works
  app-up or app-down.
- **The CLI never writes to a shell's stdin** and never injects — it upholds the Observer's
  invisibility invariant (Rule #13). Control acts only through the app's own seams.
- **The forward (GUI-launch) path of the alias shim is unchanged** for every non-verb commandline —
  defterm (`-Embedding`), reopen (`-w -s`), and bare launch must all behave exactly as before.
- **A CLI call never starts the app**, from the shim or from the app exe itself (§2a), also when the
  CLI cannot be started; every other command line reaches the app exactly as before.
- **Every dialog goes through `DialogEcho.h`** (§2b), so it is echoed to the console or pipe of the
  command that caused it while the startup lasts; `TestDialogEchoCoverage` fails on a direct dialog
  call. The sink is released when the startup ends (or when the last echoed dialog was answered), so
  the app never holds the caller's pipe open.
- **One profile, resolved once, headless-safe** (Rule #15) — the CLI reads through
  `AgentmasterStateDir()`; it never shows UI and never re-homes state.

## 11. Known gaps (verified 2026-09-29 on release 0.6.8.13, schema 2)

Where the CLI's answer differs from the app's, or leaves something out. They were found while
writing a user-local agent skill over the CLI (`~/.claude/skills/am-agentmaster-cli`, not part of
this repo). Its `am_inspect.py` helper works around each gap by reading what the CLI doesn't:
`sessions.json`, `session-store/<id>.json`, `open-windows.json`, `hooks.log`, the transcript, and
the visible screen. Its `transcript_turns.py` is the per-turn renderer §5 points to. Re-check this
list when `agentcli.cpp` changes, and drop a gap once it is fixed.

- **"Waiting for you" reads as Idle.** The presence heartbeat only says `idle`. Whether the user
  has seen a finished turn is app state (`sessions.json` `state`). 25 of 35 open sessions differed
  for exactly this reason.
- **A stale "Needs approval", in the app too.** After a restart the transcript still ends on the
  unanswered `AskUserQuestion` asked by a claude that has since exited. Claude Code writes nothing
  when a question is declined or dropped, so the app and the CLI both read NeedsApproval forever,
  while the resumed claude's screen says "User declined to answer questions". The signature:
  heartbeat `idle` (not `waiting`) and a process start LATER than the question. Both open
  NeedsApproval sessions on 2026-09-29 were like this.
- **`model` is almost always blank** (0 of 35 open sessions had it). It comes only from a
  `--model` on the claude command line; the transcript's assistant lines carry the real model.
- **`live: false` / `linkState: "archived"` only means "no claude runs now".** A restored tab
  that hasn't been focused yet (dormant) reads the same as a closed session.
- **A session recorded in BOTH profiles shows the OTHER instance's claude as its own** (seen
  2026-10-05 with `agentmasterdev`). Both profiles' `sessions.json` held `a77419cf`; production
  ran it. The pid-keyed presence heartbeat (`~/.claude/sessions/<pid>.json`) is shared by every
  install, so `agentmasterdev sessions` bound production's claude to the dev record and listed it
  as a live dev session with production's pid. After the dev instance was swapped, that claude
  was still alive and read like an orphan. Tell the owner by the claude's `--settings` path
  (`.agentmaster` vs `.agentmaster-dev`) or its parent chain, not by which CLI lists it.
- **`tabs` lists closed windows too** (every saved `windows/<id>.json`) and doesn't mark them;
  `open-windows.json` says which windows are open.
- **`tabs --window W` filters only with `--json`.** The human view is the `windows` tree, printed
  unfiltered (`CmdTabs` → `CmdWindows`).
- **A title ref is a plain substring over every session, closed ones included** (`show cli` also
  matches "bad click"), and an exact title that several sessions share stays ambiguous (exit 1).
- **Designed in §5 but not emitted:** the pending question's text (`pendingInteractiveTool` is
  only the tool's name), the last turn's tool calls (`lastTurnTools` is never written, and it
  only ever holds interactive tool names, because `ParseTranscriptDelta` reports no other tool),
  and hook provenance (`hookWired`). The conversation tail counts text blocks, not turns.
- **Codex is mostly invisible** (read from the code; no Codex tab was open to test). A managed
  Codex tab reads `kind: "shell"` with no id, and `external` lists claudes only.
- **Not in the CLI at all:** unsent drafts, tags, favorites, API errors (an errored session reads
  Idle or WaitingForInput), and a background shell job (heartbeat `shell` → Idle).
- **`--help` / `--version` as the first token go to the GUI** (§5): the running app shows a
  message box. Put a verb or a CLI-only flag first.

### Found 2026-10-09 (release 0.6.8.18, dev 0.0.1.4, schema 2)

A study of the whole CLI (design, source, launcher, build files, the skill) with read-only runs
on both installs. Each item says how it is known: **measured** (run on the live installs), or
**read** (from the code, and not run, because a run would act on the user's running app).

- **The dev alias fails on every real verb. The cause is the Debug build of the CLI.** (measured)
  - **What you see:** `agentmasterdev sessions`, `windows`, `tabs`, `external`, `list` and `show`
    exit with `0xC0000409` (Git Bash prints exit 127). When stdout is a pipe or a file, the
    output is cut at the last full stdio buffer, or it is empty. `--version` and `--help` work.
    The same binary fails when it is run by path, with either profile. Both Debug builds on disk
    fail (2026-10-06 and 2026-10-09). The Release binary reads the same dev profile correctly.
  - **Where it ends:** after `wmain` returns, inside `exit()`. The C++ library destroys its
    lazily created locale facets there (`std::_Fac_tidy_reg_t::~_Fac_tidy_reg_t` →
    `std::_Fac_node::~_Fac_node`: `delete _Facptr->_Decref()`), and one of them is a
    `std::codecvt<char,char,_Mbstatet>`. That facet exists because `ReadAllUtf8`
    (`Persistence.cpp`) reads files through `std::ifstream`, so every verb that loads state
    creates it. `--version` and `--help` return before any file is read.
  - **Why it ends:** Control Flow Guard refuses the call (`__fastfail` subcode 10,
    `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE`). The facet and its vtable are intact. In the
    2026-10-09 build, slot 0 of the vtable holds the incremental-link jump thunk at RVA `0xFED0`,
    which jumps to the facet's scalar deleting destructor (RVA `0x17D410`). The image's Guard CF
    function table (658 entries, `dumpbin /loadconfig`) does not list that thunk. It lists a
    second thunk to the same function (RVA `0x16AD0`, `??_E?$codecvt@DDU_Mbstatet@@…`, the
    vector deleting destructor), which the vtable does not use. For `codecvt<wchar_t,char>` and
    `codecvt<unsigned short,char>` the table lists two rows each. The Debug link runs with
    `/INCREMENTAL /guard:cf`; the Release link has no thunks, and its table lists the function
    itself.
  - **Not known:** why the linker lists only one of the two thunks; whether a clean,
    non-incremental Debug link has the same fault; when it began; and whether another Debug
    binary that deletes this facet has it. Windows logs no Application Error event for these
    runs, and the reason was not established. With a real console the whole output probably
    shows before the process ends, because the CRT does not hold back a console stream (not
    tested).
  - **How it was found:** `procdump -e -ma -x <dir> <exe> windows`, then the debug-dumps skill's
    `dumpexc`, `dumpourscan`, `dumpmem` and `diasym`.
  - **Until it is fixed:** `agentmaster <verb> --instance dev` (the release CLI on the dev
    profile). The skill's `am_inspect.py --dev` fails for the same reason (`tabs --json exited
    3221226505`); run it as `AGENTMASTER_PROFILE=<dev profile> am_inspect.py --cli <a release
    agentmaster-cli.exe>`.
- **A release CLI without package identity reads the DEV profile.** (measured; FIXED in code
  2026-10-23, NOT BUILT: the brand is now defined only for the dev identity, §7 step 4; until a
  release-identity build with it is installed, the text below holds)
  `agentmaster-cli.vcxproj` defines `AGENTMASTER_DEV` for every configuration (§7, step 4). So an
  unpackaged release binary that inherits no `AGENTMASTER_PROFILE` sets that variable to
  `~/.agentmaster-dev` itself, and the profile resolver reads the variable before a portable
  copy's `profile.path` and `<exedir>\profile`. Measured with the 0.6.8.18 binary: run by path
  with the variable removed, it answers from `~/.agentmaster-dev`; in a folder with a `.portable`
  marker and a `profile.path`, it still answers from `~/.agentmaster-dev`, and it answers from
  the pointed profile only when the variable is inherited. So in a portable copy, or in a
  portable install (its folder is on the user PATH, PROFILES.md §2b), `agentmaster-cli.exe` from
  a shell outside the app reads the wrong profile, and creates `~/.agentmaster-dev` when it is
  missing. The packaged aliases are not affected. Workaround: `--instance release`, or
  `--profile <dir>` after the verb.
- **The CLI creates the profile folder that it resolves.** (measured) `ResolveProfileDir` and
  `AgentmasterStateDir` create the folder when it is missing. `agentmaster sessions --profile
  <a folder that does not exist>` exits 0 with an empty list and leaves that folder behind. It
  is the one thing the CLI writes.
- **Only a verb or one of seven flags may come first.** (read) The launcher sends a command line
  to the CLI when its first token is a verb or one of `--json --self --offline --tail --instance
  --state --dir`. Every other first token goes to the running app, and its Windows Terminal
  parser treats an unknown word as the command line of a NEW TAB. So `--profile <dir>` first (a
  new-tab option there), `--window <w>` first (the parser's window target), `--archived` first,
  a mistyped verb and a verb in capitals (the match is exact) all act on the user's running app
  and print nothing. Put the verb first, always. Since 2026-10-23 (NOT BUILT) the app exe applies
  the same rule to a command line that reaches it directly (§2a).
- **An unpackaged install's `agentmaster` was the app itself.** (measured 2026-10-23; FIXED in code,
  NOT BUILT) Where an unpackaged install's folder is on PATH, `agentmaster <verb>` ran
  `Agentmaster.exe <verb>`: a second app on the user's profile, stopped by "profile in use" while
  the package ran (§2a has the incident and the fix). Until a build with the fix is in that folder,
  type `agentmaster-cli <verb>` there, or the CLI's full path; `type -a agentmaster` (bash) or
  `where agentmaster` (cmd) shows which file the name finds.
- **`show` reads only the last 512 KiB for the conversation.** (measured) `conversation`,
  `lastUserPrompt` and `lastAssistantReply` come from the text blocks inside that window. A long
  turn made of tool calls leaves none there: the studying session's own `show --self` had 0
  blocks and no `lastUserPrompt`, with its only prompt 2.75 MB before the end of the transcript.
  The `autoTesting` rows with origin `Typed` are the prompts that the registry recorded, and
  they do not depend on the window.
- **A message typed while claude worked is not in `show`.** (measured, session `f62a102b`)
  Claude Code stores it as a `queued_command` attachment, which `ParseTranscriptDelta` does not
  read. It is missing from `conversation` and `lastUserPrompt`, and `activity.userPrompts` does
  not count it (2, against 3 `Typed` rows).
- **`managed` is always `true`.** (measured) `show` on a live claude that the profile does not
  manage prints `managed: true` next to `external: true` and `linkState: "external"`. Read
  `external` and `linkState`.
- **Ids match by exact case.** (measured) An id prefix in capitals matches nothing (exit 1). A
  FULL id in capitals of a live session misses the saved record and takes the live-claude path,
  so a managed session comes back as the synthesized view: `external: true`, autorunner `Off`,
  an empty queue.
- **`host` is relative to the shell that asks.** (measured) `agentmaster-self` means "launched by
  the Agentmaster that hosts the calling shell" (the shell's own `AM_SESSION`), not "by the
  install whose profile is queried". From a release tab, `external --instance dev` labels the
  release sessions `agentmaster-self`. From a shell outside Agentmaster, every stamped claude
  reads `agentmaster-other`.
- **Filter values are not checked.** (measured) `--instance <anything but dev>` means release.
  `--state <unknown>` gives an empty list with exit 0. `--tail 0`, a negative number or a
  non-number gives every text block in the window.
- **The whole-fleet verbs grow with the profile.** (measured, 1,592 saved sessions)
  `sessions --archived --json` took 69 s, `sessions --archived` 28 s and `list --json` 20 s.
  Every other verb took about 0.4 s, `list` without `--json` included (it prints open sessions
  only). Each saved session costs a search of every Claude project folder for its transcript
  (`ResolveClaudeTranscriptPath`) and a 128 KiB tail read. One `sessions --json` call took 38 s
  instead of 0.5 s and three repeats did not; the cause is unknown. The CLI has no timeout.
- **Old names in the human view.** `show` heads the queue `FLIGHT PLAN` (measured; the app calls the
  pane Prompt Queue since 2026-10-09 and called it Auto Testing before, and the JSON key is
  `autoTesting`) and the tail `CONVERSATION (last N turns)` (read; N counts text blocks). The CLI also
  prints the mode as `Off` / `Full`, the stored words; the app shows Paused / Auto.
- **Two comments in the source were wrong.** (read; both FIXED 2026-10-23) The precedence comment
  above the profile block in `agentcli.cpp` `wmain` put the inherited variable before the package
  identity; the code under it, and §7, do the reverse for a packaged build. The comment in
  `agentmaster-cli.vcxproj` said that the release package build turns the dev brand off; no build
  file did, and since 2026-10-23 the project file itself does (§7 step 4).
- **No test covers `agentcli.cpp`.** (read) The m5 harness covers the engine units that it
  links. Its own code (argument parsing, ref resolution, `DeriveState`, the JSON builders) has
  no test.
- **The Prompt Queue Runner is visible, not drivable.** The CLI prints each session's mode and
  queue counts and, in `show`, the limits and every queue row. It has no verb that queues or
  sends a prompt, sets a mode or pauses (§9, P3), and the app has no command channel that such a
  verb could use: the app loads `sessions.json` once at its start and writes it again on every
  change. What the CLI prints is also incomplete:
  - `autorunnerDetail.autoSendsThisRun` is always 0. (read, and 0 on all 23 open sessions)
    `Persistence.cpp` neither writes nor reads the counter, and the CLI reads the saved file.
  - `queueCounts` has no failed or skipped count. (measured) The release profile held 49
    `Failed` rows; only `show` lists them, one session at a time.
  - It prints no scheduler state (read): Pause all and the interrupt hold (both in memory only),
    and why a prompt waits (`[advance-skip]` in `autorunner.log`). (Whether the scheduler runs in
    an install is no longer a question: since 2026-10-09 it runs in every build. Before, that
    depended on the dev-or-debug gate, `debugMode` in `settings.json`.)
  - Saved and not printed (read): a row's `guardPattern`, `delayMs`, `dependsOn` and `attempts`,
    the session's `lastMessageWasQuestion`, and `templates.json`.
- **Engine code that the CLI links and no verb uses.** (read) `SessionSearch.cpp` (the Sessions
  browser's search) and `ProcessInspect.Summary.cpp` (the session summary analyzer) are compiled
  into `agentmaster-cli.exe`, but no verb runs them: the `search` verb (2026-10-15) asks the session
  database instead, and there is no summary in `show`. The turns
  renderer (`TranscriptTurns.cpp`, the app's Copy ▸ Transcript) is not in the project, so the
  CLI cannot print a whole conversation.
- **A turn that a shutdown cut off does not read Error.** (read) Since 2026-10-09 the app shows
  `Error` for a session whose last turn never finished and that no process can be running
  (HOOKS.md "Cut-off turns"). The CLI's `DeriveState` has no such rule. While the tab is dormant
  (no claude runs) the session reads `Archived`. Once the tab is opened and the resumed claude's
  heartbeat reads `idle`, it reads `Idle` — or `NeedsApproval`, when its tail is a question that
  nobody can answer any more. The app's verdict needs two facts the CLI does not have: whether the
  tab is dormant, and what the record said when it was last restored. The saved `state` in
  `sessions.json` does read `Error` once the app has set it.
