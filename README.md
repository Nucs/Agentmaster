# Agentmaster
_By developers, for developers - Never lose a session again - feel like Windows Terminal so you forget you are vibing_

[Microsoft Windows Terminal](https://github.com/microsoft/terminal) but pumped up with observability, persistence and all the tools you need to scale up your parallel context management.<br/> Coding fast is no longer about typing faster but about holding and manuvering around and between 10 coding contextes.

Each session is a real `claude.exe` on a **ConPTY** connection: a full-fidelity terminal with a shared stdin (you and the orchestrator coexist), output tapped for the UI, and semantic state from **Claude Code hooks** and an **out-of-band observer** — never screen-scraping.

![The Agentmaster Manager tab — the Triage Board (sessions as cards in state columns: Running · Waiting-for-you · Needs-approval · Error · Idle/Done · External), the Explorer Tree (working directories → their sessions), and the Flight Plan (per-session prompt queue + Autopilot)](doc/agentmaster/img/agent-manager.png)

## Features

- **Agent Manager tab** — a pinned mission-control tab: a **Triage Board** (state columns), an **Explorer Tree** (working dirs → sessions; `LOCAL`/`GLOBAL`/`EXTERNAL` + sort), and a **Flight Plan** (per-session queue + history). The regions, the tab strip, and the launch box track one selection **both ways**, across every window.
- **Fleet Observer** — finds, correlates, and enriches *every* Claude (and Codex) session — even a hand-typed one that fires zero hooks — by reading each process's PEB and transcript out-of-band. Read-only and invisible to the shell; sessions running outside the manager can be watched or **adopted**.
- **Claude Code engine** — semantic state without screen-scraping: hook-derived first, then reconciled from the transcript tail and the Observer so it self-heals dropped/out-of-order events (blocked-on-question, Esc-interrupt, subagent work, `/clear`·`/compact`·`/resume` divergence).
- **Autopilot & templating** — on turn-complete the next queued prompt auto-sends (Full) / one-click confirms (Semi) / waits (Manual), bounded by stop-on-error, max-sends, pause-on-human-input and a question-guard. It re-presses Enter when the TUI eats a submit, sends idempotently, and can save a queue as a template to broadcast across a directory.
- **Tab status indicator** — every tab gets a state-colored strip dot and a top-right badge: linked → status · `model · effort · kind` · Autopilot · queue · workdir/branch; anything else → a dim `○ kind · unlinked` that flips live. Hover to copy the id / path / branch / real launch CLI / summary / transcript.
- **Per-tab summary panel** — a pencil-toggled overlay rendering the live `session-end.js` box (plan · tasks · prompts · files read/created/edited) straight from the transcript, with a ticking age/activity line.
- **Sessions browser** — search *every* Claude session on disk: an instant index pass, then a ripgrep content scan with hits attributed in-process to scope (👤 you · 🤖 assistant · 📁/📄 paths · fuzzy). Resume / Fork / Jump from any row.
- **Archive page** — closing a session archives it (never destroys it — transcripts are never touched). Sortable, searchable, grouped by window, with a "last assistant reply" preview; restore one, bulk-restore, or reopen a whole window.
- **`agentmaster` CLI** — query the fleet from any shell, app up or down: `show` / `list` / `sessions` / `tabs` / `windows` / `external`, plus `--self` and `--json`. Read-only, no live responder.
- **Persistence & restoration** — reopen a window whole: geometry, resumed Claude *and* Codex sessions, shell tabs at their real cwd, focused tab, and the Manager lens. One engine across all windows, a per-install profile folder, and permanent per-directory tab colors.

![The Agentmaster Sessions browser — every on-disk Claude Code session with a scoped/fuzzy search bar and time-range filter on the left, and a detail pane (metadata, conversation, Resume / Fork / Open-New-Session-Here) on the right](doc/agentmaster/img/sessions-browser.png)

![The tab right-click menu's Copy › submenu — copy a session's Session Id, working-dir Path, Branch Name, the real Claude or Codex launch CLI, the full session Summary, or the whole Transcript](doc/agentmaster/img/tab-copy-menu.png)

## Status

In active use, shipping regular [releases](https://github.com/Nucs/Agentmaster/releases). The full pipeline runs end-to-end in the released package, and the standalone engine harness passes 900+ checks.

- **Claude Code** — fully managed: launch, observe, drive (Autopilot), persist, archive, restore.
- **Fleet Observer** — manages *every* session, hooks or not; external ones read-only.
- **Codex** (the OpenAI Codex CLI) — a managed agent for observe + state + the full launch / restore / adopt lifecycle; driving its TUI is next.
- Installs **side-by-side** under its own identity, distinct from real Windows Terminal and a from-source dev build.

See [`IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md) and [`CLAUDE.md`](CLAUDE.md) for detail.

## Install

**One line, no download** — paste into PowerShell (omit `-Version` for latest; add `-Portable` for a cert-free/no-admin install, `-Launch` to start after):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.1
```

It downloads the signed bundle, verifies its SHA-256, trusts the cert behind a single UAC prompt (skipped when already trusted, so upgrades are usually prompt-free), and installs. Options: `-Version` · `-Portable` · `-Launch` · `-Prerelease` · `-Force` · `-Uninstall`.

Or grab the assets from [**Releases**](https://github.com/Nucs/Agentmaster/releases): the **portable zip** (unzip, run `WindowsTerminal.exe`, fully self-contained — no cert), or the self-signed **`.msixbundle`** (trust `Agentmaster.cer` once, then `Add-AppxPackage`).

On first launch the app silently picks a per-install **profile folder** (`~/.agentmaster`, or `~/.agentmaster-dev` for a dev build) holding everything it persists; change it later from ⚙ → Profile. Requires Windows 10 19041+, x64/arm64, and a native [Claude Code](https://www.anthropic.com/claude-code) `claude.exe` (auto-detected on `PATH` / `~/.local/bin` / npm; a pure-Node CLI is unsupported). See [`PROFILES.md`](doc/agentmaster/PROFILES.md).

## Docs

- [`DESIGN`](doc/agentmaster/DESIGN.md) · [`IMPLEMENTATION`](doc/agentmaster/IMPLEMENTATION.md) — the design + milestones.
- [`HOOKS`](doc/agentmaster/HOOKS.md) · [`OBSERVER`](doc/agentmaster/OBSERVER.md) · [`STATE`](doc/agentmaster/STATE.md) — the hooks bridge, Fleet Observer, and state engine.
- [`SESSIONS`](doc/agentmaster/SESSIONS.md) · [`TAB_OVERLAY`](doc/agentmaster/TAB_OVERLAY.md) · [`CLI`](doc/agentmaster/CLI.md) — the Sessions browser, per-tab badge/panel, and the `agentmaster` command line.
- [`PERSISTENCE`](doc/agentmaster/PERSISTENCE.md) · [`PROFILES`](doc/agentmaster/PROFILES.md) — workspace restore + per-install identities/profiles.
- [`CLAUDE.md`](CLAUDE.md) — full working notes: status by area, build/deploy, gotchas, correctness rules.

Code (all marked `Agentmaster`, kept additive): the Manager UI in `AgentManagerContent`, the plain-C++ engine + Fleet Observer under `src/cascadia/TerminalApp/AgentMaster/`, the per-tab overlay `AgentTabOverlay`, small touches at the `TerminalPage` / `Tab` / `TabManagement` integration points, and the `Package-{Rel,Dev}.appxmanifest` identities.

## Building

Toolchain: VS 2022 + the *Desktop Development with C++* and *Universal Windows Platform Development* workloads + the Windows SDK (10.0.22621 / 26100). Build entry: `OpenConsole.slnx`.

```powershell
pwsh -File .\tools\Build-Agentmaster.ps1            # first build (restores packages)
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore # inner loop
```

See [`CLAUDE.md`](CLAUDE.md) (*Building FAST*) for incremental-build tips, lib-only compile checks, and the raw `msbuild` invocation.

## Deploy & run (from source)

A packaged app can't be run via `WindowsTerminal.exe` directly — deploy the loose layout (what VS F5 does; no signing/cert/admin), which registers the **dev** identity `AgentmasterDev` (distinct from the released `Agentmaster`, so both coexist):

```powershell
Add-AppxPackage -Register ".\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion
```

Launch via the `agentmasterdev` alias or the **Agentmaster Dev** Start-menu entry. State lives in the dev profile (`%USERPROFILE%\.agentmaster-dev\`); see [`PROFILES.md`](doc/agentmaster/PROFILES.md).

## Relationship to Windows Terminal

Agentmaster is a fork of [`microsoft/terminal`](https://github.com/microsoft/terminal) at `v1.24.2372`; upstream code, docs, and third-party notices are retained (see [`NOTICE.md`](NOTICE.md)). The pristine upstream tree lives on `main`; Agentmaster's work is on the `agentmaster` branch. For Windows Terminal itself, see [aka.ms/terminal-docs](https://aka.ms/terminal-docs).

## License

[MIT](LICENSE), the same as upstream Windows Terminal — the original `Copyright (c) Microsoft Corporation` notice is retained alongside the fork author's.

---

> *This feels like the entire codebase went into `Form1.cs`, but patience and careful designing made this little gem.*
