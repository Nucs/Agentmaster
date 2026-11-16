<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.5.8 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 344535323,
     published 2026-06-25T07:19:12Z). -->
## Agentmaster 0.5.8 (pre-release)

**0.5.8 is a pre-release** building on 0.5.7: a stability-and-diagnostics pass. Enable **Settings → Updates → Allow pre-release versions** to receive it automatically, or install from the assets below. The stable **0.5.5** remains the default for everyone else.

## What's new

- **A navigation audit trail.** Every click, selection, launch, fork, resume or close writes one `[nav]` line to hooks.log.
- **Timestamps on every log line.** Each line starts with a local `[HH:MM:SS.mmm]` stamp, so every log reads in time order.
- **Silent failures are logged.** A launch that made no tab and a failed state write now log their id or path and the reason.
- **About 95% quieter logs.** The observer census logs only when your fleet changes, and its heartbeat went from every 15 s to every 5 min.

## What's fixed

- **No freeze when you switch keyboard layout.** Changing the input language while a Claude session runs no longer freezes the window for seconds.
- **No freeze on an interrupted subagent turn.** Esc on a session running subagents no longer flips it Running ↔ Waiting on every tick and floods the log.
- **Sessions search ranks names first.** A session's name or title match now outranks one that only mentions the words.
- **Closing a favorite offers Unfavorite & Close.** The close confirm's keep button follows the session's star.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.5** stays the default Latest.

**One command (PowerShell)** — install 0.5.8 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.8
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.8.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.8.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.8.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
