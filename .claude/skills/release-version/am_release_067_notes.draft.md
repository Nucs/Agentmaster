<!-- Agentmaster 0.6.7 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta (the full accumulation
     happens when a STABLE release is cut). See the release-notes skill §2.
     0.6.7's explicit delta over 0.6.6 = Windows notifications (db6cdadc5) + current-model display
     (963bb7fcd) + cog model-family list (f4edd1f54) + the launch-models editor live-lex (8ad08ed50).
     (The launch-model PICKER itself shipped in 0.6.6 — only the editor's validate-as-you-type is new here.)
     (3ee145f81 test-harness work is internal, omitted.)
     Mainline tip 5f9519733 (committed HEAD; the uncommitted dev-brand tweak is a release no-op, excluded).
     PRERELEASE mechanics: publish --draft=false --prerelease --latest=false (0.6.1 stays Latest).
     Assets: Agentmaster_0.6.7.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.7 (pre-release)

**0.6.7 is a pre-release that adds Windows notifications and a live current-model readout.** Now your agents can get your attention when you're looking elsewhere, and every session shows which model it's actually on. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## New in 0.6.7

- **Windows notifications when a session needs you.** Every managed session — Claude *and* Codex — now raises a **Windows toast** the moment its status leaves **Running** for anything else, so a tab or window you aren't looking at can still get your attention: *"‹session title› — Has completed after 2h30m and is ‹status›."* A new **Settings ⚙ → Notifications** tab controls it: a master on/off, a per-state checkbox for each target state (**Waiting for you / Needs approval / Idle / Done / Error** — all on by default), **"Skip when the tab is focused"** (on), and **"Play the notification sound"** (on).
- **See each session's current model at a glance.** A session's **live model** now shows in short form — on the **Triage-Board card**, the **tab tooltip**, and the **per-tab overlay** (left of the status dot). It reflects the model the session is *actually on*, read from the transcript, so a mid-session **`/model`** switch updates it (the launch `--model` / env is only the initial request and goes stale). The family list that shortens bare `--model` aliases is now a **cog setting** (Settings → Sessions), so a newly-shipped Claude family is recognized without waiting for an app update.
- **The "Launch models" editor validates as you type.** The cog's **Settings ⚙ → Sessions → Launch models** box (the launch-model picker's list, from 0.6.6) now lints live like the environment-variable editors — a colored border plus a status line ("✓ N models in the pickers" / a warning with the first issue / an error), so a typo in a `Display name | model-id` entry is obvious before it reaches the menus.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.7 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.7
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.7.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.7.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.7.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
