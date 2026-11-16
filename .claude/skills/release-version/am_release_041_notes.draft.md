<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.4.1 release notes. Cumulative since v0.4.0. A fixes-and-polish point release.
     Format matches prior notes: flat "- **Title.** desc." bullets, New + Fixes sections.
     Drafted from a one-by-one read of all 15 commits in v0.4.0..HEAD (docs-only commits omitted).
     Asset names assume tag v0.4.1 -> 0.4.1.0. -->
## Agentmaster 0.4.1

**0.4.1 is a fixes-and-polish release** on top of 0.4.0.

## What's new

- **A one-command installer.** One PowerShell line installs or upgrades to a given version, checking the bundle's SHA-256 and trusting its certificate behind one UAC prompt.
- **The build's identity in Settings.** Version, commit, channel and configuration show under the Settings title.
- **A truncate toggle in the summary panel.** `…` caps long messages (on by default), beside the wrap toggle.
- **Each file listed once in the summary.** A file both read and written appears only under Edited or Created.

## What's fixed

- **Restart session resumes the conversation.** It no longer replays the original launch, which Claude refused for an id already in use.
- **Alt+Left / Alt+Right work from the Manager tab.** The Manager's search box no longer swallows them.
- **The Triage Board keeps its scroll.** A rebuild no longer snaps each column to the top.
- **The Sessions date-range popup is anchored right.** It opens under its own button instead of over the window buttons.
- **The selection pill is a real pill.** It has padding and rounded corners, without moving the tab's icon or title.

## Install

**One command (PowerShell)** — install or upgrade to 0.4.1 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.1
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.4.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.4.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.4.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data — pick **Production** in the first-launch profile picker (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
