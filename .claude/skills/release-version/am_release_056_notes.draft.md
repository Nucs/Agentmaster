<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.5.6 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 343783327,
     published 2026-06-23T21:59:03Z). -->
## Agentmaster 0.5.6 (pre-release)

**0.5.6 is a pre-release** building on 0.5.5. Enable **Settings → Updates → Allow pre-release versions** to receive it automatically, or install from the assets below. The stable **0.5.5** remains the default for everyone else.

## What's new

- **Keep Awake.** A toolbar toggle keeps the PC and display awake: Off, Always, or only While Running.
- **Favorites replace the Archive.** A tab's lifecycle is Close and Favorite, and the Sessions page is the one browser for your history, with a ★ column and filter.
- **Context usage on board cards.** Each card shows how many tokens its conversation carries, so a session near auto-compact stands out.
- **A Filter menu on Sessions rows.** Right-click a row to narrow the list to the same folder, branch, day, week, month or fork family.
- **A refreshed Manager toolbar.** Settings, Sessions, Keep Awake and Pause Autopilot sit in a compact row under the title.

## What's fixed

- **The rich tab tooltip no longer sticks open.** It closes by itself when XAML drops the pointer's exit.
- **No crash hovering the Sessions filter.** Re-tipping a persistent element no longer orphans a tooltip, on any surface.

## Thanks

- **@galiani1** for the community PR (#1) that introduced **Keep Awake** and the **context-window usage** indicator (both refined here — Keep Awake promoted to tri-mode, context shown as a reliable raw token count).

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.5** stays the default Latest.

**One command (PowerShell)** — install 0.5.6 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.6
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.6.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.6.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.6.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
