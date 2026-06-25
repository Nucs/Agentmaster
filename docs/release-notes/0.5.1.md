## Agentmaster 0.5.1

A fork of **Windows Terminal** that turns it into a manager for multiple **AI coding agents** (**Claude Code** + **Codex**). **0.5.1** is a focused follow-up to 0.5.0: a new **summary-panel "Jump"** that scrolls the terminal to wherever a past prompt was sent, plus the completion of the **hover-tooltip sweep** so every control — and every dense adornment — explains itself.

## New in 0.5.1

- **Summary-panel "Jump" — scroll the terminal to where a prompt was sent.** Every numbered prompt in a tab's summary panel now carries a **▸ jump button**: click it and the session's terminal scrolls to **center on where that prompt is rendered** in the live buffer. It's a fuzzy, duplicate-aware match (a soft-wrap or re-indent won't defeat it, and the *i*-th send of a repeated prompt maps to the *i*-th on-screen occurrence), it resolves **right-to-left prompts** (Hebrew/Arabic, which a terminal stores visually reversed), and a button whose prompt **isn't currently on screen is dimmed** so the working ones stand out. It costs nothing until you click (a per-click buffer search on par with Ctrl+Shift+F).
- **Hover-tooltip sweep completed.** Rounds 6–8 finish what 0.5.0 began — now beyond interactive controls to the **dense informative adornments** that were cryptic at a glance: the **Manager pane resize splitters**, the **markdown code-block Run button**, and the **state dots, the codex / external / host pills, the `model · effort · sandbox · approval` lines, the working-directory color, the `flight` / `typed` chips, and the autopilot/queue badge** all now say what they mean on hover.

## Capabilities

- **Real sessions, real control** — each session is a live agent (`claude.exe` / Codex) on a ConPTY with shared stdin; state comes from hooks + an out-of-band observer, never screen-scraping.
- **Manager tab** (pinned, leftmost) — a **sortable Triage Board** (color-coded cards), an **Explorer Tree** (LOCAL · GLOBAL · EXTERNAL scopes + sorting), and a **Flight Plan** (per-session prompt queue + history). **Autopilot** auto-sends queued prompts on turn-complete with backstops (max sends, stop-on-error, pause, question-guard, Enter-retry).
- **Per-tab lens** — every classified tab carries a top-right link badge (status · `model · effort · kind` · workdir/branch) with always-on actions (Open Path, Copy menu, summary pencil) and a toggleable **summary panel** (plan · tasks · messages · files · live times) — now with **per-prompt Jump**; the terminal search box docks into the same HUD.
- **Fleet Observer** — detects and manages **every** session, including a hand-typed `claude`/Codex in any tab (no hooks needed), and observes external ones read-only, with **Adopt** (fork-a-copy or resume).
- **Sessions & Archive pages** — full-window browsers over every on-disk conversation and every archived session: indexed + ripgrep search, time range, Resume / **Fork** / Jump / bulk-open / reopen-whole-window, hide-from-list.
- **Workspace persistence** — windows reopen at their geometry with sessions resumed and shell tabs replayed at their cwd; permanent per-directory tab colors; the `agentmaster <verb>` CLI introspects the fleet from any shell.
- **Coexists with Windows Terminal** — installs side-by-side under its own package identity; your real Windows Terminal install is left untouched.

## Install

**One command (PowerShell)** — install or upgrade to 0.5.1 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.1
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). *Already on 0.5.0+? Just use **Settings → Check for updates** (or the startup prompt).*

**Portable (no cert):** download `Agentmaster_0.5.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
