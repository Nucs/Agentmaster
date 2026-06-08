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
- **Explorer Tree** (bottom-left) — the working directories → their sessions, with
  `LOCAL`/`GLOBAL`/`EXTERNAL` scope, a sort toggle (newest / oldest / most-active / A–Z / by-PID),
  smart per-directory tab naming and per-directory tab color.
- **Flight Plan** (bottom-right) — a per-session prompt queue plus **Autopilot**; it records the
  full message history, tagging whether each prompt was queued by you or typed straight into the
  terminal.

**Flight Plan / Autopilot** — queue prompts; on turn-complete (the Claude `Stop` hook) the next
queued prompt is auto-sent. Approvals and clarifying questions are handled separately.

## Highlights

- **Launch & manage** — spawn a `claude.exe` in any working directory; full terminal fidelity with
  a shared stdin so you and the orchestrator both drive the same session.
- **Adopt any `claude`** — a `claude` you type yourself into an ordinary tab is managed too (a
  transparent PATH shim auto-wires it for hooks), not just Manager-launched ones.
- **Fleet Observer** — out-of-band detection that finds, correlates, and enriches *every* Claude
  session — even a hand-typed one that fires zero hooks — by reading each process's cwd / cmdline /
  env and its transcript. No hooks, no settings, fully read-only and invisible to the shell.
- **External sessions** — surfaces Claude running outside the manager (other Windows Terminals,
  cmd/console), enriched from its transcript (title, git branch, model · effort, timing); view its
  conversation read-only, or **Adopt** it to resume into a managed, controllable tab.
- **Autopilot** — turn-complete auto-advance with Full / Semi-auto / Manual modes and backstops
  (stop-on-error, max-auto-sends, pause-on-human-input, global pause).
- **Per-tab badge** — every terminal tab carries a top-right HUD: a linked Claude shows status +
  `model · effort · kind` + Autopilot mode + queued count, while any other tab shows a dim
  `○ kind · unlinked` badge that flips live as the tab's activity changes.
- **Persistence + archive/restore** — sessions, plan templates, recent dirs, and per-directory tab
  colors persist under `%USERPROFILE%\.agentmaster\`. Sessions live as **Open ⇄ Archived**;
  restore resumes via `claude --resume` (transcript-gated) and reloads the Flight Plan. Archiving is
  non-destructive — your Claude transcripts on disk are never deleted.
- **Multi-window workspace** — one engine shared across all windows; each window persists its
  geometry, Manager lens, tab layout and focused tab, and reopens on the next launch, with a recover
  button for windows closed along the way.
- **One value per concept** — a session's title is one value shared by the Explorer row, the
  Windows Terminal tab title, and the persisted record; a tab's color is one value per working
  directory.
- **Settings cog** — global Claude-session config (skip-permissions, model, env vars) and Autopilot
  defaults, persisted to `settings.json`.
- **Coexists with Windows Terminal** — installs side-by-side under its own package identity, so
  your real Windows Terminal / Dev install is left untouched.

## Status

All milestones **M0–M8 + session restore** are complete, built, deployed under the `Agentmaster`
package identity, and verified end-to-end (Launch → real `claude.exe` on a ConPTY → `--settings`
hooks → PowerShell forwarder → named pipe → registry → state machine → UI, plus `claude --resume`
restore on reopen). The standalone engine harness passes its checks (`src/cascadia/TerminalApp/AgentMaster/tests/`).

The **Fleet Observer** (O1–O7) is complete — the out-of-band detection layer that manages every
Claude session, including hand-typed ones that fire no hooks. **Workspace persistence (M9–M14)** is
largely landed: **M9** (one process-wide engine shared by all windows) and most of **M10**
(per-window UI-state records — geometry re-apply, Manager-lens restore, focused-tab restore, and
multi-window reopen) are shipped and live-verified; routing a restored session back into its owning
window is the main piece still in progress.

See [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) for the milestone
tracker.

## Download & install

Grab the latest build from [**Releases**](https://github.com/Nucs/Agentmaster/releases):

- **Portable (recommended)** — download `Agentmaster_<version>_x64.zip` (or `_arm64`), unzip
  anywhere, and run `agentmaster.exe` in place. No install and no certificate required.
- **MSIX bundle** — the `.msixbundle` is self-signed, so trust `Agentmaster.cer` once
  (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`),
  then double-click the bundle or `Add-AppxPackage` it.

Requires Windows 10 2004 (19041) or later, on x64 or arm64, with
[Claude Code](https://www.anthropic.com/claude-code) (`claude`) installed and on `PATH`. Agentmaster
installs side-by-side under its own package identity, so any existing Windows Terminal install is
left untouched. (To build from source instead, see [Building](#building) below.)

## Architecture & docs

- [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md) — full design (the "Native Graft" + the
  three-region Manager tab).
- [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) — milestones & build.
- [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md) — the Claude Code hooks bridge.
- [`doc/agentmaster/OBSERVER.md`](doc/agentmaster/OBSERVER.md) — the Fleet Observer (out-of-band
  pull correlation + activity).
- [`doc/agentmaster/TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md) — the per-tab link badge.
- [`doc/agentmaster/PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md) — workspace persistence (M9–M14).
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
Start-Process "shell:appsFolder\Agentmaster_56k4f06dsfp9r!App"
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
