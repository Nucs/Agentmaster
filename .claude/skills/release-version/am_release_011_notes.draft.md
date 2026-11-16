<!-- Agentmaster 0.1.1 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 338410856,
     published 2026-06-12T06:34:00Z). -->
## Agentmaster 0.1.1


### New in 0.1.1

- **Sessions page** — a full-window browser over *every* Claude Code session on disk (not just managed ones), backed by an on-disk session store, a sidecar index, and two-phase `ripgrep` search across titles, directories and transcript text. **Resume here** or **Fork here** (`--fork-session`) any session straight from the list.
- **Archive, redesigned as a full-window page** — table on the left, rich detail on the right: searchable (including the plan text), copy/open actions, the conversation + last assistant reply, hover tooltips, window chips, a persisted splitter, and double-click to run a row's primary action.
- **Tab-strip status dots** — every tab now reads `[icon] ● <title>` with the dot state-colored like the Triage Board / Explorer Tree, so a session's state is legible straight from the tab strip.
- **Full-window page chrome** — the Sessions and Archive pages dismiss on tab-switch and support Up/Down keyboard navigation.
- **Waiting-for-you decay** — a stale "waiting" card demotes to Idle after N minutes (configurable).
- **Cross-window robustness** — moving a tab between windows re-homes its Claude session instead of orphaning it; duplicating a tab forks the conversation; broadcast-input skips managed Claude panes; reopen surfaces ignore Manager-only (empty) windows; the focused tab is restored on reopen.
- Plus **13 audited correctness fixes** across the archive, resume/restore, persistence, registry and liveness paths.

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
