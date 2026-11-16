<!-- Agentmaster 0.2.0 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 338718826,
     published 2026-06-13T20:45:08Z). -->

## New in 0.2.0

- **Profiles — pick where everything lives.** On first launch Agentmaster asks for a **profile folder**: Production (`%USERPROFILE%\.agentmaster`), Development (`%USERPROFILE%\.agentmaster-dev`), or **Browse…** to any folder. A profile holds *everything* the app persists — sessions, Flight Plans, window layouts, app settings, hooks, **and Terminal's own settings.json/state.json** (under `<profile>\terminal\`). A "copy existing data" checkbox migrates your current state into the chosen folder (originals kept). Change it later from Manager → ⚙ → **Profile** (applies on restart). A kernel guard warns if two running instances point at one profile.
- **Release and dev builds no longer collide.** The released app owns the `Agentmaster` package identity (`agentmaster` alias, Start menu "Agentmaster"); a from-source dev build is **`AgentmasterDev`** (`agentmasterdev` alias, "Agentmaster Dev"). Both install side by side with separate single-instance scopes, separate launchers, separate profiles — window reopen, jump lists, and new-window launchers always target the install that issued them. Each instance's Claude hooks now also route to **its own** engine (the bridge-discovery fallback is per-profile — previously two instances could cross-route hooks).
- **True portable zips.** `Agentmaster_<ver>_x64.zip` / `_arm64.zip` now ship a `.portable` marker: Terminal settings live in `<unzip>\settings` and the Agentmaster profile in `<unzip>\profile`. Nothing outside the unzip folder is touched — carry it on a USB stick.
- **Ordered hook state machine.** Hook events now carry a fire-time `ts` and a per-turn identity, so a slow `Stop` forwarder landing after the next turn's prompt can no longer paint a stale state — fixes the "second turn never shows Running" bug, and type-ahead prompts queued mid-turn keep the session correctly **Running** through the follow-on turn.
- **Sessions browser: search query grammar.** `"quoted phrases"` match exactly (never fuzzied); a bare session-id GUID finds that session *and its forks*, and scopes content matches to it — paste an id straight from a log. The 📁 directory / 📄 file scopes (which directories/files a session's tool calls touched) are now ON by default and answered instantly from the index.
- **Triage Board parity + cross-window actions.** Board cards carry the same right-click menu as Explorer-Tree rows (Rename… / Archive… / Open New Session Here); double-click **Activates** — and Activate/Rename now work on a session hosted in *any* window: the hosting window selects the tab and comes to the foreground, renames sync registry⇄tab across windows. The Board gets its own **LOCAL/GLOBAL** scope toggle, shared with the tree and persisted per window.
- **Polish.** Tab-strip status dots optically centered; Archive/Sessions page tooltips dismiss when the mouse leaves.

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
