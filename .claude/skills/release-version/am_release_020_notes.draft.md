<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.2.0 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 338718826,
     published 2026-06-13T20:45:08Z). -->
## Agentmaster 0.2.0

**0.2.0** brings per-install profiles, and release and dev builds that live side by side.

## What's new

- **Profiles: pick where everything lives.** The first launch asks for a profile folder (Production, Development or any folder), which holds everything the app saves, Terminal's own settings included.
- **Release and dev builds no longer collide.** The release is `Agentmaster` and a dev build `AgentmasterDev`, each with its own alias, Start entry, single-instance scope and profile.
- **True portable zips.** A portable zip keeps its Terminal settings and Agentmaster profile inside the unzip folder and touches nothing else.
- **An ordered hook state machine.** Hooks carry their fire time and turn, so a slow Stop can no longer paint a stale state, and the second turn shows Running.
- **A Sessions search grammar.** "Quoted phrases" match exactly, a pasted session id finds the session and its forks, and the folder and file scopes are on by default.
- **Triage Board parity and cross-window actions.** Board cards get the tree's right-click menu, and Activate and Rename work on a session hosted in any window.
- **Polish.** The tab strip's status dots are centered, and page tooltips close when the mouse leaves.

## Install

Paste into PowerShell — installs **v0.2.0** (one UAC prompt, only to trust the signing cert):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.2.0
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.2.0 -Portable -Launch
```

**Portable (recommended, no cert):** download `Agentmaster_0.2.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `WindowsTerminal.exe`. Fully self-contained.

**MSIX bundle (self-signed):**
1. Download `Agentmaster.cer` + `Agentmaster_0.2.0.0.msixbundle`.
2. Trust the cert once (admin PowerShell):
   `Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
3. `Add-AppxPackage .\Agentmaster_0.2.0.0.msixbundle` (or double-click it).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading from v0.1.x keeps your data: pick **Production** in the first-launch profile picker (your existing `%USERPROFILE%\.agentmaster` state carries over; Terminal settings are seeded into the profile automatically). Portable-zip users from v0.1.x: state now lives inside the unzip folder — copy `%USERPROFILE%\.agentmaster` into `<unzip>\profile` to bring sessions along.

Requires Windows 10 2004+ (x64/arm64) with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) on `PATH`.
