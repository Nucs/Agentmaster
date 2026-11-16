<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.6.1 release notes — STABLE patch (becomes Latest; NOT a pre-release).
     Range: v0.6.0..8ea00ecc = 8 commits (3 user-facing + docs). Last stable is 0.6.0, so this IS
     the full changelog since last stable. RELEASING from the pinned commit 8ea00ecc5d88ac01449214c5c261b569b04f45f1.
     Tag v0.6.1 -> 0.6.1.0. STABLE mechanics: publish --draft=false --latest (REPLACES 0.6.0 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.1.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.1

**0.6.1 is a stable patch** on 0.6.0. This build becomes the new default **Latest**.

## What's new

- **The inferred working folder is used everywhere.** With the Inferred working directory coloring mode, the folder a session really works in drives its grouping, scope and Open New Session Here, not only its color.
- **The contributor CLA is final.** Its governing law is set and its draft framing removed, and the README's licensing section is clearer.

## What's fixed

- **The long-session freeze is fixed.** A memory leak that grew about 1.6 GB an hour on a long-running Agentmaster, until Windows ran out of commit and the app froze, is gone.
- **No false ⚡ "still cached" hints.** The board's warm-cache hint no longer lights after an adopt, a Move to Waiting-for-you, a launch or a resume.

## Install

**One command (PowerShell)** — installs 0.6.1 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.1
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.1 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
