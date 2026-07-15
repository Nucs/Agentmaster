<!-- Agentmaster 0.6.6 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta (the full accumulation
     happens when a STABLE release is cut). See the release-notes skill §2.
     0.6.6's explicit delta over 0.6.5 = the launch-model picker: spawn (4afb948d7) AND fork (af18c741d).
     Mainline tip eff9000e0. PRERELEASE mechanics: publish --draft=false --prerelease --latest=false (0.6.1 stays Latest).
     Assets: Agentmaster_0.6.6.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.6 (pre-release)

**0.6.6 is a pre-release that adds the launch-model picker** — pick which Claude model a session starts on, right where you start or fork it. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## New in 0.6.6

- **Launch-model picker — start or fork a session on the model you choose.** Every **"Open New Session Here"** *and* every **"Fork session" / "Fork here"** affordance — on the Triage Board, the Explorer tree, an **External** row, the **Sessions** page (its detail buttons are now split buttons), and the tab right-click menu — is now a **picker**: **Default** (your global model, or Claude's own default when that's blank) plus one item per configured model. Picking a model launches (or forks) *that one session* with `--model <id>` — only that launch is affected (resume / restart still follow your settings model, and it's deliberately not persisted). The model list is a **new cog setting** (**Settings ⚙ → Sessions → Launch models**), a simple **"Display name | model-id"** list shipped with **Fable 5**, **Opus 4.8**, and **Sonnet 5** (edit it to add your own). The Triage-Board card / per-tab overlay `model·effort` adornment reflects the pick automatically; Codex sessions keep their plain New/Fork items (the list is Claude models).

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.6 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.6
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.6.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.6.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.6.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
