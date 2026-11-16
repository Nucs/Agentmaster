<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.1.0 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 336158553,
     published 2026-06-08T18:25:30Z). -->
## Agentmaster 0.1.0

**0.1.0 is Agentmaster's first public release:** a fork of Windows Terminal that manages many Claude Code sessions.

## What's new

- **The Manager tab.** A pinned tab holds a Triage Board of session cards by state, an Explorer Tree of working folders and their sessions, and a Flight Plan.
- **Launch and manage Claude sessions.** Each session is a real claude.exe in its own terminal tab, its state read from Claude Code's hooks, never from the screen.
- **Adopt any claude.** A claude you type into an ordinary tab is managed too, wired for hooks by a PATH shim.
- **Flight Plan and Autopilot.** Queue prompts for a session, and Autopilot sends the next one when a turn completes, in Full, Semi-auto or Manual mode.
- **Persistence, archive and restore.** Sessions, plans, recent folders and tab colors persist, and an archived session resumes with `claude --resume`.
- **One title and one color.** A session's title is the same on its tree row, its tab and its record, and a folder's tabs share one color.
- **A Settings cog.** It holds the global Claude settings (skip-permissions, model, environment variables) and the Autopilot's.

## Install

Paste into PowerShell — installs **v0.1.0** (one UAC prompt, only to trust the signing cert):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.1.0
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.1.0 -Portable -Launch
```

### Install — portable (recommended, no cert)

1. Download `Agentmaster_0.1.0.0_x64.zip` (or `_arm64`).
2. Unzip anywhere and run `WindowsTerminal.exe` / `agentmaster.exe` in place.

### Install — MSIX bundle (self-signed)

The `.msixbundle` is self-signed with a `CN=Agentmaster` certificate, so you must trust `Agentmaster.cer` once before installing:

1. Download `Agentmaster.cer` and `Agentmaster_0.1.0.0.msixbundle`.
2. Trust the cert (admin PowerShell):
   `Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
3. Double-click the `.msixbundle`, or:
   `Add-AppxPackage .\Agentmaster_0.1.0.0.msixbundle`

---

Requires Windows 10 2004 (19041) or later, x64 or arm64, with Claude Code (`claude`) installed and on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
