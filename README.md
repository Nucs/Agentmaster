# Agentmaster

**Agentmaster** is a fork of [Microsoft Windows Terminal](https://github.com/microsoft/terminal)
(MIT) that turns the terminal into a **manager for multiple
[Claude Code](https://www.anthropic.com/claude-code) sessions**.

> One Windows Terminal–based app that manages **N Claude Code sessions across M working
> directories** — letting you **fully interact** with each session *and* giving the app
> **100% programmatic control** (inject prompts, read output, drive state).

Each session is a real `claude.exe` on a **ConPTY** connection: a full-fidelity terminal with a
shared stdin (you and the orchestrator coexist), output tapped for the UI, and semantic state
taken from **Claude Code hooks** — never screen-scraping.

---

## The Manager tab

Agentmaster adds a pinned, leftmost, non-closable **Manager tab** (tab 0, open by default) split
into three selection-synced regions:

- **Triage Board** (top) — sessions as cards in state columns
  (*Running* · *Waiting-for-you* · *Needs-approval* · *Error*).
- **Explorer Tree** (bottom-left) — the working directories → their sessions, with `LOCAL`/`GLOBAL`
  scope, smart per-directory tab naming and per-directory tab color.
- **Flight Plan** (bottom-right) — a per-session prompt queue plus **Autopilot**.

**Flight Plan / Autopilot** — queue prompts; on turn-complete (the Claude `Stop` hook) the next
queued prompt is auto-sent. Approvals and clarifying questions are handled separately.

## Highlights

- **Launch & manage** — spawn a `claude.exe` in any working directory; full terminal fidelity with
  a shared stdin so you and the orchestrator both drive the same session.
- **Adopt any `claude`** — a `claude` you type yourself into an ordinary tab is managed too (a
  transparent PATH shim auto-wires it for hooks), not just Manager-launched ones.
- **Autopilot** — turn-complete auto-advance with Full / Semi-auto / Manual modes and backstops
  (stop-on-error, max-auto-sends, pause-on-human-input, global pause).
- **Persistence + archive/restore** — sessions, plan templates, recent dirs, and per-directory tab
  colors persist under `%USERPROFILE%\.agentmaster\`. Sessions live as **Open ⇄ Archived**;
  restore resumes via `claude --resume` (transcript-gated) and reloads the Flight Plan.
- **One value per concept** — a session's title is one value shared by the Explorer row, the
  Windows Terminal tab title, and the persisted record; a tab's color is one value per working
  directory.
- **Settings cog** — global Claude-session config (skip-permissions, model, env vars) and Autopilot
  defaults, persisted to `settings.json`.

## Status

All milestones **M0–M8 + session restore** are complete, built, deployed under the `Agentmaster`
package identity, and verified end-to-end (Launch → real `claude.exe` on a ConPTY → `--settings`
hooks → PowerShell forwarder → named pipe → registry → state machine → UI, plus `claude --resume`
restore on reopen). The standalone engine harness passes its checks (`src/cascadia/TerminalApp/AgentMaster/tests/`).

**Workspace persistence (M9–M14)** is in progress: **M9** (one process-wide engine shared by all
windows) is complete and unit-tested; **M10** (per-window UI-state records) has its data layer plus
per-window record capture and Manager-lens restore working, with window-geometry re-apply and
multi-window reopen still to come.

See [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) for the milestone
tracker.

## Architecture & docs

- [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md) — full design (the "Native Graft" + the
  three-region Manager tab).
- [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) — milestones & build.
- [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md) — the Claude Code hooks bridge.
- [`doc/agentmaster/PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md) — workspace persistence (M9–M14).
- [`CLAUDE.md`](CLAUDE.md) — the working notes: status by area, build/deploy details, gotchas, and
  the correctness rules.

### Where the code lives

All additions are marked `Agentmaster`, kept additive where practical so rebasing onto upstream
stays cheap:

- `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}` — the Manager tab content (the C1 UI).
- `src/cascadia/TerminalApp/AgentMaster/` — the engine (plain C++, no WinRT):
  `SessionRegistry`, `HooksBridge`, `ClaudeSpawn`, `Scheduler`, `Engine` (the process-wide shared
  engine), `Persistence`, and `tests/` (a standalone harness).
- Small touches in `TerminalPage.{h,cpp}`, `Tab.{h,cpp}`, and `TabManagement.cpp` at the
  integration points, plus the registrations in `TerminalAppLib.vcxproj`.
- `src/cascadia/CascadiaPackage/Package-Dev.appxmanifest` — the distinct `Agentmaster` package
  identity (so it coexists with real Windows Terminal).

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

Then launch via the `agentmaster` execution alias, the **Agentmaster** Start-menu entry, or:

```powershell
Start-Process "shell:appsFolder\Agentmaster_8wekyb3d8bbwe!App"
```

Runtime/session state lives in `%USERPROFILE%\.agentmaster\`; tail `hooks.log` to confirm the
engine is live and that spawned sessions' hooks arrive.

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
