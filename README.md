# Agentmaster
_By developers, for developers - Never lose a session again - feel like Windows Terminal so you forget you are vibing_

[Microsoft Windows Terminal](https://github.com/microsoft/terminal) but pumped up with observability, persistence and all the tools you need to scale up your parallel context management.<br/> Coding fast is no longer about typing faster but about holding and manuvering around and between 10 coding contextes.

Each session is a real `claude.exe` on a **ConPTY** connection: a full-fidelity terminal with a
shared stdin (you and the orchestrator coexist), output tapped for the UI, and semantic state
taken from **Claude Code hooks** and an **out-of-band observer** — never screen-scraping.

![The Agentmaster Manager tab — the Triage Board (sessions as cards in state columns: Running · Waiting-for-you · Needs-approval · Error · Idle/Done · External), the Explorer Tree (working directories → their sessions), and the Flight Plan (per-session prompt queue + Autopilot)](doc/agentmaster/img/agent-manager.png)
<span style='fontsize: 10px'>_The Agentmaster Manager tab — the Triage Board (sessions as cards in state columns: Running · Waiting-for-you · Needs-approval · Error · Idle/Done · External), the Explorer Tree (working directories → their sessions), and the Flight Plan (per-session prompt queue + Autopilot_</span>

## Features

Here's what Agentmaster adds — and, as an engineer, why each piece is built the way it is.

### The Agent Manager tab

A pinned, leftmost, non-closable tab (tab 0) that is mission control for the whole fleet — three
regions wired into one **selection-synced lens**:

- **Triage Board** — every session as a card in a state column (*Running · Waiting-for-you ·
  Needs-approval · Error · Idle/Done*), plus an *External* column for agents running outside the
  manager.
- **Explorer Tree** — your working directories → their sessions, with `LOCAL` / `GLOBAL` /
  `EXTERNAL` scope and a sort toggle (newest / oldest / most-active / A–Z / by-PID).
- **Flight Plan** — a per-session prompt queue and the full message history, tagging each prompt
  *queued* (by you) or *typed* (straight into the terminal).

What makes it feel alive is the **Linked Lenses**: the three regions, the terminal tab strip, and
the launch box all track one selection — *both ways*. Click a card and the tree row, Flight Plan,
and the launch box's working directory follow; switch to a session's terminal tab and the Manager
selects it right back; hover a card and its tab lights up in the strip without switching to it.
And because one engine backs every window, the board and tree show the **whole fleet** — Activate,
Rename, and Jump reach a session hosted in *any* window and bring that window forward.

### The Fleet Observer

The part I'm proudest of. Hooks are great when they fire — but a `claude` you type yourself (a
shell alias shadowing the PATH shim) fires *none*. The Observer is the always-correct floor
underneath: an out-of-band survey that **finds, correlates, and enriches every Claude (and Codex)
session on the machine** by reading each process's PEB — working directory, command line,
environment — and its transcript, bound to the right tab by `WT_SESSION`.

No hooks, no settings, no shell cooperation, and **it never writes to a shell** — every read is
out-of-band, so a hand-typed session is detected, bound, and driveable with nothing the user can
feel. It tells your own sessions from a sibling install's from a real Windows Terminal's by package
identity, survives PID reuse, and costs microseconds at steady state. It also surfaces agents
running *outside* the manager — other Windows Terminals, a bare console — as read-only **External**
rows you can watch, or **Adopt** (resume, or fork a safe copy) into a managed, driveable tab. It's
the reason a session can't hide from Agentmaster no matter how you start it.

### The Claude Code engine

Pulling trustworthy *semantic* state out of a TUI that repaints constantly is the hard problem, and
the engine solves it **without ever screen-scraping**. State is hook-derived first (a named-pipe
bridge fed by Claude Code's own hooks), then reconciled by two independent backstops — a
transcript-tail reader and the Observer — so the truth survives dropped or out-of-order events.

The state machine is layered and self-healing:

- Hook events carry a fire-time and per-turn identity, so a slow `Stop` landing after the next
  prompt can't paint a stale state.
- When a `Stop` is dropped, the engine *synthesizes* the turn boundary from the transcript tail,
  the presence heartbeat, or subagent activity — recovering the cases a naïve reader gets wrong: a
  session **blocked on a question**, **interrupted** with Esc, **answered and back to work**, or
  **delegating to a subagent** while its own transcript sits silent.
- It even follows a conversation that diverges under `/clear`, `/compact`, or `/resume`, re-binding
  to the live conversation instead of the dead launch id.

Each session is a real `claude.exe` on a ConPTY with a **shared stdin** — you and the orchestrator
drive the same terminal — and one process-wide engine backs every window.

### Autopilot & templating

Queue a plan and let turn-completion drive it: on the `Stop` hook the next prompt **auto-sends**
(Full), waits for a **one-click confirm** (Semi), or holds for you (Manual). It's bounded by real
backstops — stop-on-error, max-auto-sends, pause-on-human-input, a global pause, and a
question-guard that refuses to "answer" a clarifying question with a queued prompt.

My favorite detail is the **Enter-retry**: Claude's Ink TUI can swallow an injected prompt's submit
keystroke as a newline if it lands a beat too early, leaving the prompt typed-but-unsent. Autopilot
notices the turn never started and re-presses a *lone* Enter (never the text again) until it does.
Sends are idempotent — marked sent atomically and never replayed across a restart. **Templates**
save a session's queue so you can re-apply it to another session or broadcast it across a whole
working directory. Autopilot's defaults — mode, max-sends, stop-on-error — live in the **Settings
cog**, alongside the global Claude config it stamps onto new sessions (model, skip-permissions,
environment).

### Tab status indicator

Every classified tab shows its state where you actually work. The tab strip itself carries a
**state-colored dot** next to the title (the same palette as the Triage Board), and each terminal
pane wears a small **top-right badge**:

- A **linked** Claude session shows live status + `model · effort · kind`, Autopilot mode, queued
  count, a `⛓ linked` marker, and a dim second line with the working-directory folder and **live
  git branch**.
- Any **other** tab shows a dim `○ kind · unlinked` badge — `pwsh` / `cmd` / a started-but-
  unprompted `claude` / `codex` — that **flips in place** as the tab's activity changes (a `pwsh`
  tab becomes `claude` the instant you run it, then the full linked badge on its first prompt).

Hover expands an action row: open the working directory, or copy the session id, path, branch, the
**real** launch command line, the full summary, or the entire transcript — each with a little
confirmation chime. It's the per-tab *here-and-now* lens that complements the Manager's fleet view.

### Per-tab summary panel

Click the badge's pencil and a second overlay drops in below it — a faithful in-process port of
Claude Code's `session-end.js` analyzer, rendered live from the transcript: the plan, tasks, the
numbered prompt history, and the files **read / created / edited** (created distinguished from
edited; each file listed once). A times bar ticks *age · last user message · last activity* in real
time. It's resizable, it re-reads only when the transcript actually grows (a quiet tab costs one
file-stat), and it has wrap and truncate toggles. The panel shows the value the badge doesn't
already cover; the copy menu hands you the complete box.

### Sessions browser

A full-window page over **every Claude Code session on disk** — not just the ones Agentmaster
launched. The piece I love here is the **two-phase search**: an instant pass over per-session
sidecar indexes (and Claude's own `history.jsonl`), then a **ripgrep-prefiltered** content scan
whose every hit is **attributed in-process to its scope** — 👤 your typed prompts vs 🤖 the
assistant's replies, tool inputs, and results (ripgrep can't tell them apart; the classifier can).
Add 📁/📄 to match the directories and files a session actually *touched*, and `F` for fuzzy.

The query grammar is real: `"quoted phrases"` match exactly, a pasted session-id GUID finds that
session *and its forks*, and bare words AND-match across fields. Rows carry fork-aware creation
dates and line-derived last-activity (file mtime lies — measured up to 43 days off), a
per-directory color chip, and a live presence ring. From any row: **Jump**, **Resume here**,
**Fork here**, or open a new session in that directory.

![The Agentmaster Sessions browser — a full-window page listing every on-disk Claude Code session (Title · Directory · Branch · Created · Active · Msgs·Tools), with a scoped/fuzzy search bar and time-range filter on the left, and a detail pane on the right (session metadata, the conversation, and Resume / Fork / Open-New-Session-Here actions)](doc/agentmaster/img/sessions-browser.png)

### Sessions archive page

Closing a session **archives** it — it never destroys it, and your Claude transcripts on disk are
never touched. The Archive page is a full-window, sortable, searchable table **grouped by window**,
with a detail pane that reads the metadata, the conversation, and — my favorite touch — the **last
assistant reply**, tail-read from the transcript so you can see exactly where a session left off
before deciding to bring it back. Restore one, multi-select **bulk-restore**, or **reopen a whole
saved window**. Restore is a transcript-gated `claude --resume`, so a never-prompted session comes
back fresh instead of dying on "no conversation found."

### The `agentmaster` CLI

The fleet is queryable from any shell — **whether the app is running or not** — because it reads
only persisted and OS-observable state (the Observer's pull model, run one-shot from a separate
process). `agentmaster show <ref>` gives a session's derived state and presence, the conversation
tail and last assistant reply, activity (messages / tools / **files touched**), the last prompt,
and the Flight-Plan queue; plus `list` / `sessions` / `tabs` / `windows` / `external`.

`--self` introspects the calling tab via `WT_SESSION` — so an AI agent running *inside* a tab can
ask Agentmaster what it's looking at — and `--json` emits a schema-stamped payload. Read-only, no
live responder, no new IPC.

### Persistence & restoration

Close a window, reopen it later, and it comes back **whole**: its saved geometry, its Claude *and*
Codex sessions **resumed in place**, its shell tabs replayed at their **real working directory**
(recovered out-of-band — even though PowerShell freezes its own process cwd on `cd`), the focused
tab re-selected by stable identity, and the Manager lens — selection, scopes, splitter positions —
restored. One engine is shared across all windows, and a per-window record *references* sessions by
id rather than copying them, so there is exactly one source of truth.

Everything lives under a per-install **profile folder** — sessions, Flight Plans, plan templates,
window layouts, settings, and Terminal's own settings too. Sessions are **Open ⇄ Archived**;
"Restart session" resumes the current conversation rather than replaying a stale launch command;
and a working directory keeps the **same tab color permanently**, across tabs, windows, and
restarts.

## Status

Agentmaster is in active use and ships regular [releases](https://github.com/Nucs/Agentmaster/releases).
The whole pipeline runs end-to-end in the released package — Launch → real `claude.exe` on a
ConPTY → hooks → named-pipe bridge → registry → state machine → UI, with `claude --resume` restore
on reopen — and the standalone engine harness passes its checks (900+).

Where things stand:

- **Claude Code** is fully managed end-to-end: launch, observe, drive (Autopilot), persist,
  archive, and restore.
- The **Fleet Observer** detects and manages *every* session — including hand-typed ones that fire
  no hooks — and surfaces external ones read-only.
- **Codex** (the OpenAI Codex CLI) is a first-class *managed* agent for observe + state + the full
  launch / restore / window-restore / adopt lifecycle; driving its TUI with a stdin injector and
  Autopilot is the next step.
- **Workspace persistence** — geometry, Manager lens, focused tab, resumed sessions, and replayed
  shell tabs — is shipped and live-verified across single- and multi-window reopen.
- Installs **side-by-side** under its own package identity, distinct from real Windows Terminal and
  from a from-source dev build, so all of them coexist.

See [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) for the milestone
tracker and [`CLAUDE.md`](CLAUDE.md) for the detailed status by area.

## Download & install

**One line, no download** — paste into PowerShell. It fetches the installer in memory, downloads
the signed bundle, **verifies its SHA-256**, trusts the signing certificate behind a single UAC
prompt (skipped when the cert is already trusted, so upgrades are usually prompt-free), and installs
the version you pick (here `0.4.1` — every [release](https://github.com/Nucs/Agentmaster/releases)
page shows a one-liner pinned to that version):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.1
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.1 -Portable -Launch
```

Options: `-Version X.Y.Z` (omit for latest) · `-Portable` (no cert / no admin) · `-Launch` ·
`-Prerelease` · `-Force` · `-Uninstall` (see [`tools/Install-Agentmaster.ps1`](tools/Install-Agentmaster.ps1)).
A missing VCLibs dependency is fetched automatically, and a conflicting older registration is
detected and cleared.

---

Or grab the assets yourself from [**Releases**](https://github.com/Nucs/Agentmaster/releases):

- **Portable** — download `Agentmaster_<version>_x64.zip` (or `_arm64`), unzip
  anywhere, and run `WindowsTerminal.exe` in place. No install and no certificate required. The zip
  is **fully self-contained**: Terminal settings live in `<unzip>\settings` and all Agentmaster
  state in `<unzip>\profile` — nothing outside the folder is touched.
- **MSIX bundle** — the `.msixbundle` is self-signed, so trust `Agentmaster.cer` once
  (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`),
  then double-click the bundle or `Add-AppxPackage` it. Installed, it runs as the `agentmaster`
  execution alias / the **Agentmaster** Start-menu entry.

On **first launch** the app **silently picks a per-install default profile** — Production
(`%USERPROFILE%\.agentmaster`) for the released build, Development (`%USERPROFILE%\.agentmaster-dev`)
for a from-source build — and remembers it; no prompt. A profile holds *everything* the app persists
(sessions, Flight Plans, plan templates, window layouts, settings — including Terminal's own settings
under `<profile>\terminal\`). Change it later from the Manager tab → ⚙ → Profile, or point
`AGENTMASTER_PROFILE` at any folder (a `.portable` marker keeps state inside the unzip dir). See
[`doc/agentmaster/PROFILES.md`](doc/agentmaster/PROFILES.md).

Requires Windows 10 2004 (19041) or later, on x64 or arm64, with the **native**
[Claude Code](https://www.anthropic.com/claude-code) build (a real `claude.exe`) installed — via the
native installer or `npm i -g @anthropic-ai/claude-code` (npm ships the same native binary) or
`claude install`. Agentmaster auto-detects it on `PATH`, in `%USERPROFILE%\.local\bin`, or behind an
npm `claude.cmd`; if none is found it gates Claude actions and points you to install or Browse to it
(the Settings cog also takes an explicit `claude.exe` override). A pure-Node `claude` CLI is **not**
supported. Agentmaster
installs side-by-side under its own package identity, so any existing Windows Terminal install is
left untouched — and the release identity (`Agentmaster`, alias `agentmaster`) is likewise distinct
from a from-source dev build (`AgentmasterDev`, alias `agentmasterdev`), so the two coexist without
sharing any state. (To build from source instead, see [Building](#building) below.)

## Architecture & docs

- [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md) — full design (the "Native Graft" + the
  three-region Manager tab).
- [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) — milestones & build.
- [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md) — the Claude Code hooks bridge.
- [`doc/agentmaster/OBSERVER.md`](doc/agentmaster/OBSERVER.md) — the Fleet Observer (out-of-band
  pull correlation + activity).
- [`doc/agentmaster/STATE.md`](doc/agentmaster/STATE.md) — the observer-owned session-state engine.
- [`doc/agentmaster/SESSIONS.md`](doc/agentmaster/SESSIONS.md) — the Sessions browser + the
  `~/.claude` storage map.
- [`doc/agentmaster/TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md) — the per-tab badge + summary
  panel.
- [`doc/agentmaster/CLI.md`](doc/agentmaster/CLI.md) — the `agentmaster <verb>` command line.
- [`doc/agentmaster/PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md) — workspace persistence &
  restoration.
- [`doc/agentmaster/PROFILES.md`](doc/agentmaster/PROFILES.md) — release/dev package identities +
  the per-install state profiles (first-launch default, migration, coexistence).
- [`CLAUDE.md`](CLAUDE.md) — the working notes: status by area, build/deploy details, gotchas, and
  the correctness rules.

### Where the code lives

All additions are marked `Agentmaster`, kept additive where practical so rebasing onto upstream
stays cheap:

- `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}` — the Manager tab content (the C1 UI).
- `src/cascadia/TerminalApp/AgentMaster/` — the engine (plain C++, no WinRT):
  `SessionRegistry`, `HooksBridge`, `ClaudeSpawn`, `Scheduler`, `Engine` (the process-wide shared
  engine), `Persistence`, the **Fleet Observer** (`ProcessInspect`, `ProcessObserver`, `Activity`),
  and `tests/` (a standalone harness).
- `src/cascadia/TerminalApp/AgentTabOverlay.{h,cpp}` — the per-tab link badge overlay.
- Small touches in `TerminalPage.{h,cpp}`, `Tab.{h,cpp}`, and `TabManagement.cpp` at the
  integration points, plus the registrations in `TerminalAppLib.vcxproj`.
- `src/cascadia/CascadiaPackage/Package-Rel.appxmanifest` + `Package-Dev.appxmanifest` — the two
  package identities: `Agentmaster` (what releases ship; alias `agentmaster`) and `AgentmasterDev`
  (the local dev loose layout; alias `agentmasterdev`) — distinct from each other *and* from real
  Windows Terminal, so all of them coexist.

## Building

Requires the Windows Terminal toolchain: VS 2022 + the *Desktop Development with C++* and
*Universal Windows Platform Development* workloads + the Windows SDK (10.0.22621 / 26100). The build
entry is `OpenConsole.slnx` (slnx format).

A convenience wrapper builds just the app target in parallel:

```powershell
# first build (restores packages)
pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1
# inner loop (packages already restored)
pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore
```

Raw equivalent:

```powershell
msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /t:Terminal\CascadiaPackage
```

See [`CLAUDE.md`](CLAUDE.md) (*Building FAST*) for incremental-build tips, lib-only compile checks,
and Defender exclusions.

## Deploy & run

A packaged app can't be launched by running `WindowsTerminal.exe` directly — it must be deployed.
Deploy the loose layout (what Visual Studio F5 does — no signing/cert/admin):

```powershell
# one-time per machine (or after the manifest changes)
Add-AppxPackage -Register ".\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion
```

The loose layout registers the **dev identity** (`AgentmasterDev` — deliberately distinct from the
released `Agentmaster` package, so both can be installed at once). Launch via the `agentmasterdev`
execution alias, the **Agentmaster Dev** Start-menu entry, or:

```powershell
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"
```

Runtime/session state lives in the install's **profile folder** (auto-selected on first launch; the
dev default is `%USERPROFILE%\.agentmaster-dev\` — see
[`doc/agentmaster/PROFILES.md`](doc/agentmaster/PROFILES.md)); tail `hooks.log` there to confirm
the engine is live and that spawned sessions' hooks arrive.

## Relationship to Windows Terminal

Agentmaster is a fork of [`microsoft/terminal`](https://github.com/microsoft/terminal) at tag
`v1.24.2372`. The upstream Windows Terminal code, documentation, and third-party notices are
retained — see [`NOTICE.md`](NOTICE.md). For everything about Windows Terminal itself (the console
host, shared components, general build/contribution guidance), refer to the upstream repository and
[aka.ms/terminal-docs](https://aka.ms/terminal-docs). The pristine upstream tree is preserved on the
`main` branch; Agentmaster's work lives on the `agentmaster` branch.

## License

Agentmaster is licensed under the [MIT License](LICENSE), the same license as upstream Windows
Terminal. The original `Copyright (c) Microsoft Corporation` notice is retained alongside the fork
author's copyright, per the terms of the MIT License.

---

> *This feels like the entire codebase went into `Form1.cs`, but patience and careful designing made
> this little gem.*
