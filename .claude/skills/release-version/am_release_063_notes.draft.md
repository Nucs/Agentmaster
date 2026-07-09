<!-- Agentmaster 0.6.3 release notes — STABLE (becomes Latest; NOT a pre-release).
     Cut off v0.6.1 (merge-base 8ea00ecc) with ONLY the two safe fixes isolated from the 0.6.2 pre-release
     UI line: the 23 GB Manager-rebuild leak fix (fcf9013c6) + the updater 1h re-check (4d2976176) + docs.
     RELEASING from 4d2976176 (tag v0.6.3). Tag v0.6.3 -> 0.6.3.0.
     STABLE mechanics: publish --draft=false --latest (REPLACES 0.6.1 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.3.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.3

**0.6.3 is a stable release** — a focused hotfix on 0.6.1 that ships the critical **memory-leak fix** to everyone as the default, plus an **hourly update check**. The larger tab / overlay / restart work that debuted in the 0.6.2 pre-release is still baking and will land in a later release; 0.6.3 deliberately carries **only** the two changes below on top of the stable 0.6.1 base. This build becomes the new default **Latest**.

## Fixes

- **The 23 GB Manager memory leak.** After 0.6.1 fixed the tooltip-leak freeze, a *second* leak remained: a C++/WinRT reference cycle in the Triage-Board / Explorer-tree session context menu (the menu held its host card/row strongly, and the menu's **Tags** item held the host strongly back — an uncollectable cycle under COM refcounting). Because the Manager rebuilds its board and tree on every registry update (plus a 30 s backstop), each rebuild leaked the entire discarded card/row subtree — measured at roughly **85 MB/minute** under a busy fleet, reaching ~23 GB and eventually freezing the UI. The menu is now weak-anchored to its host, animation storyboards are stopped instead of left running forever, and every insert-only cache is bounded — so a long-lived Manager no longer grows.

## New

- **Hourly update check.** With auto-update enabled, Agentmaster now re-checks for a new version **every hour while running**, not only at startup — so a fix like the one above reaches you without a restart.

## Install

**One command (PowerShell)** — installs 0.6.3 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.3
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.3 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.3.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.3.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.3.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
