<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.6.4 release notes — PRE-RELEASE (NOT Latest; v0.6.3 stays Latest).
     v0.6.4 = v0.6.3 (4a038e7f8) + 2 cherry-picked crash-fix commits (d606d657 -> 7eb61789), tag commit e8a1646f6.
     Last stable is 0.6.3, so this IS the full changelog since last stable.
     PRERELEASE mechanics: publish --draft=false --prerelease --latest=false (the stable 0.6.3 stays Latest).
     Assets: Agentmaster_0.6.4.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.4 (pre-release)

**0.6.4 is a pre-release** on top of 0.6.3 carrying one important stability fix. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.3** remains the default for everyone else.

## What's fixed

- **No more crash loop when resuming a heavy session.** A large, image-heavy or compacted transcript no longer kills the app a few seconds after it reopens your workspace.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.3** stays the default Latest.

**One command (PowerShell)** — installs 0.6.4 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.4
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.4.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.4.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.4.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
