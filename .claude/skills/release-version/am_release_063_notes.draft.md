<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.6.3 release notes — STABLE (becomes Latest; NOT a pre-release).
     Cut off v0.6.1 (merge-base 8ea00ecc) with ONLY the two safe fixes isolated from the 0.6.2 pre-release
     UI line: the 23 GB Manager-rebuild leak fix (fcf9013c6) + the updater 1h re-check (4d2976176) + docs.
     RELEASING from 4d2976176 (tag v0.6.3). Tag v0.6.3 -> 0.6.3.0.
     STABLE mechanics: publish --draft=false --latest (REPLACES 0.6.1 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.3.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.3

**0.6.3 is a focused hotfix on 0.6.1:** the critical memory-leak fix, plus an hourly update check.

## What's new

- **An hourly update check.** With auto-update on, Agentmaster checks for a new version every hour while it runs, not only at startup.

## What's fixed

- **The 23 GB Manager memory leak is gone.** A reference cycle in the board's and tree's session menu kept every rebuilt card alive.

## Install

**One command (PowerShell)** — installs 0.6.3 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.3
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.3 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.3.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.3.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.3.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
