<!-- Agentmaster 0.5.1 release notes. ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note. -->
## Agentmaster 0.5.1

**0.5.1** is a focused follow-up to 0.5.0: a new **summary-panel "Jump"** that scrolls the terminal to wherever a past prompt was sent, plus the completion of the **hover-tooltip sweep** so every control — and every dense adornment — explains itself.

## New in 0.5.1

- **Summary-panel "Jump" — scroll the terminal to where a prompt was sent.** Every numbered prompt in a tab's summary panel now carries a **▸ jump button**: click it and the session's terminal scrolls to **center on where that prompt is rendered** in the live buffer. It's a fuzzy, duplicate-aware match (a soft-wrap or re-indent won't defeat it, and the *i*-th send of a repeated prompt maps to the *i*-th on-screen occurrence), it resolves **right-to-left prompts** (Hebrew/Arabic, which a terminal stores visually reversed), and a button whose prompt **isn't currently on screen is dimmed** so the working ones stand out. It costs nothing until you click (a per-click buffer search on par with Ctrl+Shift+F).
- **Hover-tooltip sweep completed.** Rounds 6–8 finish what 0.5.0 began — now beyond interactive controls to the **dense informative adornments** that were cryptic at a glance: the **Manager pane resize splitters**, the **markdown code-block Run button**, and the **state dots, the codex / external / host pills, the `model · effort · sandbox · approval` lines, the working-directory color, the `flight` / `typed` chips, and the autopilot/queue badge** all now say what they mean on hover.

## Install

**One command (PowerShell)** — install or upgrade to 0.5.1 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.1
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). *Already on 0.5.0+? Just use **Settings → Check for updates** (or the startup prompt).*

**Portable (no cert):** download `Agentmaster_0.5.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
