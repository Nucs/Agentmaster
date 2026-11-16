<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.3.0 release notes. Cumulative since the last PUBLIC release (v0.1.1):
     v0.2.0 was built but never published, so its line is folded into "New in 0.3.0".
     Format matches the v0.2.0 / v0.1.1 notes: flat "- **Title.** desc." changelog bullets.
     Asset names assume tag v0.3.0 -> 0.3.0.0. -->
## Agentmaster 0.3.0

**0.3.0** takes the 0.2.0 line to its first public release, and adds Codex and a command line.

## What's new

- **Codex, managed alongside Claude.** A Claude ⇄ Codex toggle on the launch bar starts Codex sessions that restore with their window and wear a teal codex pill.
- **`agentmaster <verb>` on the command line.** Query the fleet from any shell, app running or not: `agentmaster show <ref>` gives a session's state, conversation tail, activity and queue.
- **Shell tabs restore at their real folder.** Reopening a window brings pwsh and cmd tabs back where you left them.
- **A sharper External census.** External claudes are labeled by their host, orphans are dropped, and the Claude desktop app is left out.
- **Launch box and Manager niceties.** A pasted session id validates live with Resume and Fork, Copy Session Id is on every menu, and titles can be multi-line.
- **Per-install profiles, release and dev side by side.** The first launch asks for a profile folder that holds everything the app saves, and release and dev builds no longer collide.
- **An ordered hook state machine.** A slow Stop can no longer paint a stale state, so the second turn shows Running.
- **Sessions search grammar and cross-window actions.** "Quoted phrases" match exactly, a pasted session id finds the session and its forks, and board cards get the full right-click menu.

## What's fixed

- **Sessions no longer strand in the wrong state.** Blocked, interrupted and approved sessions read right, a resumed mid-turn session no longer lights Running, and Claude tabs stay open when claude.exe exits.
- **Tooltip and tab-strip polish.** Tooltips dismiss correctly everywhere, and the tab strip's status dot no longer clips at the tab's edge.

## Install

Paste into PowerShell — installs **v0.3.0** (one UAC prompt, only to trust the signing cert):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.3.0
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.3.0 -Portable -Launch
```

**Portable (recommended, no cert):** download `Agentmaster_0.3.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):**
1. Download `Agentmaster.cer` + `Agentmaster_0.3.0.0.msixbundle`.
2. Trust the cert once (admin PowerShell):
   `Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
3. `Add-AppxPackage .\Agentmaster_0.3.0.0.msixbundle` (or double-click it).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading from v0.1.x keeps your data — pick **Production** in the first-launch profile picker (your existing `%USERPROFILE%\.agentmaster` carries over; Terminal settings are seeded in automatically). Portable-zip users: state now lives inside the unzip folder — copy `%USERPROFILE%\.agentmaster` into `<unzip>\profile` to bring sessions along.

Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally [Codex](https://openai.com/codex/) — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
