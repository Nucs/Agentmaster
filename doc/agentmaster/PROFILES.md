# Profiles & package identities — release/dev coexistence

> **Goal:** installing the GitHub **release** and the local **dev** build on one machine must
> produce two fully independent apps — different identity, different launchers, and **zero shared
> persisted state**. The app "merely loads a profile folder": everything an install stores —
> engine state *and* Terminal's own settings — lives in ONE folder chosen **automatically by
> package identity on first launch** (release → Production, dev → Development), and changeable later
> from the cog (Production / Development / **Browse…**).

Status: **implemented** (lib-compiled green; engine harness 604/604 incl. the new profile checks).
Runtime verification of the picker + side-by-side install rides the next deploy cycle (the running
dev instance could not be restarted while this was built).

---

## 1. The collision matrix (what used to collide, and what fixes it)

| # | Seam | Before | After |
|---|------|--------|-------|
| 1 | **Package identity** | `release.yml` shipped `Package-Dev.appxmanifest` — the SAME `Agentmaster_56k4f06dsfp9r` family the dev loose layout registers. Installing one replaced/broke the other (`0x80073CF6`-class conflicts). | Two manifests: **`Package-Rel.appxmanifest`** (`Name=Agentmaster` — what releases ship, and what v0.1.x already shipped, so existing installs upgrade in place) and **`Package-Dev.appxmanifest`** (`Name=AgentmasterDev`). Publisher stays `CN=Agentmaster` for both — the PFN hash derives from the Publisher ALONE, so the families are `Agentmaster_56k4f06dsfp9r` and `AgentmasterDev_56k4f06dsfp9r` and the release self-signing flow is untouched. Selected by `/p:AgentmasterPackageIdentity=Release` (wapproj; default = Dev). |
| 2 | **Execution alias** | Both declared `agentmaster.exe`. The global `WindowsApps\agentmaster.exe` link points at ONE package — shell launches and the app's own reopen-window dispatch (`_ReopenSavedWindows`) could activate the WRONG install. | Per-identity aliases: release = **`agentmaster.exe`**, dev = **`agentmasterdev.exe`**. `GetWtExePath()` picks by PFN prefix (**Dev tested first** — `Agentmaster` is a prefix of `AgentmasterDev`), and the reopen dispatch resolves `_AgentmasterReopenTarget()` the same way (unpackaged now falls back to the neighbor `WindowsTerminal.exe` instead of silently no-opping). |
| 3 | **Single-instance mutex / window class / AUMID** | Already fixed pre-profiles: `windowClassName` appends the PFN. | Distinct automatically once the families differ. Start menu shows **Agentmaster** and **Agentmaster Dev**. |
| 4 | **The state dir `~/.agentmaster`** | One dir for both: two `SharedEngine`s would clobber `sessions.json`, `open-windows.json`, `windows/<id>.json`, `settings.json`, fight over window records and reopen each other's workspaces. | **Per-install profiles** (§2). Each install resolves its own profile folder; ALL engine state lives there. |
| 5 | **`bridge.json` discovery** | The generated PowerShell forwarder hardcoded `$env:USERPROFILE\.agentmaster\bridge.json` — a no-env hook from one instance's session could route to the OTHER instance's pipe. | `BuildForwarderScript(stateDir)` bakes the **per-profile** `<profile>\bridge.json` path in (PS-single-quoted; unit-tested). The shim already embedded the per-profile `--settings` path. |
| 6 | **Terminal's own settings.json / state.json** | Per-package `LocalState` — isolated, but OUTSIDE the profile (violates "everything in the profile"). | `GetBaseSettingsPath()` honors **`AGENTMASTER_PROFILE`** → `<profile>\terminal\` (exported by the bootstrap before any settings load). Headless hosts (tests/tools) that never ran the bootstrap keep stock paths. |
| 7 | **The "portable" zip** | Built WITHOUT the `.portable` marker — it shared the global unpackaged WT settings dir, and its Agentmaster state went to `~/.agentmaster`. | `release.yml` passes `-PortableMode:$true`: WT settings in `<unzip>\settings` (upstream portable mode) and the profile in **`<unzip>\profile`** (the bootstrap's portable branch — fully self-contained, never asks, never touches the user profile). |
| 8 | **Same profile, two instances** (user points both installs at one folder via Browse…) | n/a (new failure mode the picker introduces) | A kernel **profile mutex** (`Local\Agentmaster.profile.<fnv1a64(dir)>`, auto-released on process death — can't go stale): the second instance gets a warn-and-confirm before touching anything. |

**Known shared seam (deliberate, documented):** the defterm/handoff CLSIDs
(`{1F9F2BF5-…}` / `{051F34EE-…}`) and the shell-extension CLSID (`{52065414-…}`) are upstream's
DEV-branding GUIDs, compiled into the binaries and declared by BOTH manifests (and by any real
`WindowsTerminalDev`). If several of these packages are installed, the OS "default terminal
application" handoff and the Explorer "Open in Terminal" verb resolve to whichever package the OS
picks for the CLSID. Guidance: set the **release** install as the default terminal; don't pick the
dev build. Follow-up (below) tracks minting per-identity GUIDs.

## 2. The profile system

A **profile** is one folder holding everything an install persists:

```
<profile>\
  sessions.json            windows\<id>.json        open-windows.json
  settings.json            templates.json           recent-dirs.json
  dir-colors.json          sessions-index\          hooks-settings.json
  agentmaster-hook.ps1     shim\                    bridge.json
  hooks.log                autopilot.log
  terminal\settings.json   terminal\state.json      ← Terminal's OWN settings (seam #6)
```

`~/.claude` is **not** part of a profile — transcripts belong to Claude itself and are shared by
design (both instances browse the same conversations; the Sessions browser, resume, fork all
operate on Claude's store).

### Resolution order (`Agentmaster::Profiles::ResolveProfileDir`, ProfileBootstrap.h)

1. **`AGENTMASTER_PROFILE`** env var — explicit override; also what the bootstrap exports so every
   module (exe, TerminalApp.dll, TerminalSettingsModel.dll) resolves identically in-process.
2. **`<exedir>\profile`** when `<exedir>\.portable` exists (the true-portable zip; never asks).
3. **The saved per-install choice** — `%USERPROFILE%\.agentmaster.profiles`, a tiny UTF-8 text map
   (`<PFN or "Unpackaged">=<absolute dir>` per line). Plain text, NOT JSON, on purpose: the
   WindowsTerminal EXE links `TerminalApp.dll` (not the static lib) and parses it without JSON
   helpers; it lives OUTSIDE any profile (chicken-and-egg) and outside MSIX-virtualized AppData,
   so it's shared, honest, debuggable, and survives package reinstall.
4. **Per-identity default** — `~/.agentmaster` (release package **and** unpackaged/headless runs:
   the historical dir, which keeps the test harness and old tooling unchanged) or
   `~/.agentmaster-dev` (the `AgentmasterDev` package).

Resolution is cached process-wide — a profile cannot change mid-run; the cog's "Change…" applies
on the next launch.

### First launch — auto-select by identity (no prompt)

`WindowEmperor::HandleCommandlineArgs` calls `EnsureProfileResolvedAtStartup(allowUi)` **after**
winning the single-instance handoff (a handed-off second process never shows UI) and **before**
anything reads persisted state (`ReloadSettings`, `ApplicationState`, the `windows/*.json` reopen
scan, and — much later — the engine's `AgentmasterStateDir()`). With no saved choice it **does not
prompt** — it auto-selects the **per-identity default** (`DefaultProfileDir()`) and persists it:

- the **RELEASE** install (`Agentmaster`) → **Production**, `%USERPROFILE%\.agentmaster`
- the **DEV** install (`AgentmasterDev`) → **Development**, `%USERPROFILE%\.agentmaster-dev`
- unpackaged / portable-without-marker → the release default (the historical `~/.agentmaster`)

It then **seeds `<profile>\terminal\`** with the install's current Terminal settings (from package
`LocalState` / the unpackaged dir) so the first redirected launch looks identical instead of
resetting to defaults. The auto-pick needs no UI, so it runs identically for a real launch and a
`-Embedding` COM activation (defterm handoff) — `allowUi` now governs only the "profile in use by
another instance" warning, never the choice itself.

> Earlier builds showed a one-time **Production / Development / Browse…** `TaskDialogIndirect`
> picker (with a "Copy existing data from `~/.agentmaster`" migrate checkbox). That UI still exists
> (`ShowProfilePicker`, plus `MigrateProfileData` for the migrate path) but is now reached **only**
> from the cog's **Change profile folder…** — first launch is silent. To land on a non-default
> folder (a synced drive, a shared dir, or to copy the legacy `~/.agentmaster` into a fresh dev
> profile), launch once, then switch via the cog (below).

### Changing later

Manager tab → **⚙ Settings → PROFILE**: shows the active folder (and a pending `current → new
(after restart)` when a change is staged) + **Change profile folder…** — the same picker, with the
migrate-checkbox source being the ACTIVE profile, and a "applies on next start" note. The running
engine never re-homes mid-run.

## 3. This machine's migration (one-time, at the next deploy window)

The current `~/.agentmaster` holds the DEV fleet, and the current loose registration owns the OLD
`Agentmaster` family — which now belongs to the release. In order:

```powershell
# 0. (next deploy cycle) close our dev instance as usual, build, then:
# 1. drop the OLD loose registration (frees the Agentmaster_56k4f06dsfp9r family for the release)
Remove-AppxPackage Agentmaster_56k4f06dsfp9r   # ok if it errors: nothing to remove
# 2. register the rebuilt loose layout — it now carries the AgentmasterDev identity
Add-AppxPackage -Register "K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion
# 3. launch: alias `agentmasterdev`, Start menu "Agentmaster Dev", or
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"
```

First launch no longer prompts — the dev build **auto-lands on the fresh `~/.agentmaster-dev`**, so
the existing `~/.agentmaster` dev fleet is NOT picked up automatically. To keep using it, do ONE of:
- **before** the first launch, set `AGENTMASTER_PROFILE=%USERPROFILE%\.agentmaster` (or write that
  line into `%USERPROFILE%\.agentmaster.profiles` under the `AgentmasterDev_56k4f06dsfp9r=` key) →
  the dev build resolves straight to the existing folder, in place; or
- **after** the first launch (which lands on the empty `~/.agentmaster-dev`), open the cog →
  **Change profile folder… → Browse… → `~/.agentmaster`** to use the existing folder in place
  (applies on the next restart). Give the release a fresh dir when it installs.

Installing the GitHub release afterwards (`Add-AppxPackage Agentmaster_<ver>.msixbundle`) is clean —
the family is free after step 1 — and ITS first launch silently lands on **Production**
(`~/.agentmaster`); change it from the cog if needed.

## 4. What runs where (quick reference)

| | Release install | Dev loose layout | Portable zip |
|---|---|---|---|
| Package family | `Agentmaster_56k4f06dsfp9r` | `AgentmasterDev_56k4f06dsfp9r` | (unpackaged) |
| Start menu | Agentmaster | Agentmaster Dev | — |
| Alias | `agentmaster` | `agentmasterdev` | run `WindowsTerminal.exe` |
| Manifest | `Package-Rel.appxmanifest` (`/p:AgentmasterPackageIdentity=Release`; release.yml stamps the version here) | `Package-Dev.appxmanifest` (default) | n/a |
| Default profile | `~/.agentmaster` | `~/.agentmaster-dev` | `<unzip>\profile` (marker; no picker) |
| Terminal settings | `<profile>\terminal\` | `<profile>\terminal\` | `<unzip>\settings` (upstream portable) |
| Local build | `Build-Agentmaster.ps1 -ReleaseIdentity` (smoke tests the release identity — NOTE: registering it replaces an installed GitHub release) | `Build-Agentmaster.ps1` | `New-UnpackagedTerminalDistribution.ps1 -PortableMode:$true` |

Cross-instance behavior while BOTH run: each engine has its own pipe (`agentmaster.<pid>`), shim,
hooks files and fleet; the Fleet Observer of one sees the other's claudes as **external,
observe-only** (different `AM_SESSION` GUID, never in this window's roster — Rule #13), which is
correct and even useful (watch the prod fleet from dev).

## 5. Follow-ups (non-blocking)

- **Per-identity defterm/shellext CLSIDs** — mint new GUIDs for the handoff coclasses
  (`CConsoleHandoff` / terminal handoff / `WindowsTerminalShellExt`) keyed off
  `AgentmasterPackageIdentity`, update both manifests; removes the §1 shared seam entirely.
- **Distinct dev iconography** — a tinted icon set for the `AgentmasterDev` tiles so the two Start
  entries differ visually, not just by name.
- **Profile import/export** — the profile folder is already a self-contained unit; a zip
  export/import in the cog would make moving machines trivial.
- The choice file keys unpackaged NON-portable runs as one `Unpackaged` slot; if multiple loose
  copies ever need distinct profiles, key by exe-path hash instead (the AUMID already does this).
