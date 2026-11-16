<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.6.5 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta (not a comprehensive
     changelog — the full accumulation happens when a STABLE release is cut). See the release-notes skill §2.
     0.6.5's explicit delta over 0.6.4 = the tab/overlay/restart batch held back in the abandoned 0.6.2
     pre-release + the newer state/triage commits. It deliberately does NOT re-list the 23 GB leak fix +
     hourly update (shipped in 0.6.3) or the resume crash-loop fix (shipped in 0.6.4).
     Mainline tip 369e02ef8. PRERELEASE mechanics: publish --draft=false --prerelease --latest=false (0.6.1 stays Latest).
     Assets: Agentmaster_0.6.5.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.5 (pre-release)

**0.6.5 is a pre-release** that ships the tab, overlay and restart work held back in the abandoned 0.6.2 pre-release. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## What's new

- **A "Remove colors" tab-coloring mode.** No tab is colored, while your saved colors are kept for when you switch back.
- **Hide the profile icon on tabs.** "Show icons on tabs" (off by default) drops the profile icon from every tab.
- **The tab strip reveals a selected session's tab.** Selecting a session on the board or tree scrolls the strip to its tab.
- **Work that outlives the turn keeps a session Running.** A live shell, a background agent or a teammate keeps the session Running instead of dropping it to Idle or Done.
- **Dismiss an Error card by hand.** Move to Idle and Move to Done work on Error cards too.
- **Ctrl+Home toggles the Agent Manager.** It replaces Shift+Home.

## What's fixed

- **Restart session really restarts.** No exit-code 259 banner, no false archive seconds later, and a forked session keeps its branch.
- **A marked triage state survives opening the tab.** A background session moved to Waiting-for-you or marked unread no longer resets to Idle when its tab first starts.
- **Overlay buttons take clicks on their icons.** A click on a button's glyph no longer misses, and the pencil gives clear feedback.
- **Teammate and background-agent messages stay out of your history.** Their deliveries no longer land in a session's Sent / Typed history or light the cache hint.
- **The tab strip keeps one height.** A tab with a multi-line title no longer changes the strip's height as you scroll.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.5 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.5
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.5.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.5.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.5.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
