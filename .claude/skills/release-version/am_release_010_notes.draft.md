<!-- Agentmaster 0.1.0 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 336158553,
     published 2026-06-08T18:25:30Z). -->
## Agentmaster 0.1.0


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
