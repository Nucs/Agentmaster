<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.1.1 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 338410856,
     published 2026-06-12T06:34:00Z). -->
## Agentmaster 0.1.1

**0.1.1** builds on 0.1.0.

## What's new

- **A Sessions page.** A full-window browser over every Claude Code session on disk, searched with ripgrep across titles, folders and transcripts, with Resume here and Fork here.
- **The Archive as a full-window page.** A searchable table with a rich detail pane, copy and open actions, and a double-click for each row's main action.
- **Status dots on the tab strip.** Every tab reads `[icon] ● <title>`, its dot colored by the session's state.
- **Full-window page chrome.** The Sessions and Archive pages close on a tab switch and take Up / Down.
- **Waiting-for-you decay.** A stale "waiting" card drops to Idle after a number of minutes you set.
- **Sturdier across windows.** A tab moved between windows keeps its Claude session, duplicating a tab forks the conversation, and the focused tab comes back on reopen.

## What's fixed

- **13 audited correctness fixes.** Across the archive, resume and restore, persistence, the registry and liveness.

## Install

Paste into PowerShell — installs **v0.1.1** (one UAC prompt, only to trust the signing cert):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.1.1
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.1.1 -Portable -Launch
```

### Install — portable (recommended, no cert)

1. Download `Agentmaster_0.1.1.0_x64.zip` (or `_arm64`).
2. Unzip anywhere and run `agentmaster.exe` in place.

### Install — MSIX bundle (self-signed)

The `.msixbundle` is self-signed with a `CN=Agentmaster` certificate, so trust `Agentmaster.cer` once before installing:

1. Download `Agentmaster.cer` and `Agentmaster_0.1.1.0.msixbundle`.
2. Trust the cert (admin PowerShell):
   `Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
3. Double-click the `.msixbundle`, or `Add-AppxPackage .\Agentmaster_0.1.1.0.msixbundle`.

---

Requires Windows 10 2004 (19041) or later, on x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) installed and on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
