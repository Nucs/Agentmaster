<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.6.9 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta vs the previous release (0.6.8).
     24 commits v0.6.8..213e8426e, themed. Mainline tip = 213e8426e (origin was 14 behind; pushed at release).
     Headliners: the /handover command family (3ded15111 foundation + 9e66208de content injection +
     f63f8b108 /handover-here + 1bb4b0c52 fan-out + edb81eb0d Commands tab + c26e6da8f successor shaping),
     the 0.6.8 UI-FREEZE fix (a74e6e348 + afe8cec03), the updater envelope-schema fix (8e954ae3c +
     93b59f717 + 1d51f7e73), the sub-2s toast guard (a86570e08).
     Omitted (internal): exception-forensics sweep/docs, SHA-256 definition history, test/harden commits,
     the agentcli build-script link fix. PRERELEASE mechanics: --draft=false --prerelease --latest=false.
     Assets: Agentmaster_0.6.9.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.9 (pre-release)

**0.6.9 is a pre-release** building on 0.6.8. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## What's new

- **`/handover`.** Type `/handover <what to carry over>` and Claude writes a briefing that opens as a fresh session's first message, in a new tab in the same folder.
- **`/handover-here`.** The same hand-off, but the successor replaces the current tab, and the original session stays resumable from the Sessions browser.
- **A Commands tab in Settings.** Rename or disable either command, and pick the successor's model, its title rewrite and which markdown files count as a briefing.

## What's fixed

- **The 0.6.8 UI freeze is fixed.** The summary panel no longer re-resolves every session's prompt-jump targets on the UI thread every few seconds.
- **Automatic updates work.** The updater kept its preferences at the wrong level of settings.json, so 0.6.x installs never prompted for an update.
- **No more premature "completed" toast.** A session that reported a completion after under 2 s of Running, while still working, no longer fires a "waiting for you" toast.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.9 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.9
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.9.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.9.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.9.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
