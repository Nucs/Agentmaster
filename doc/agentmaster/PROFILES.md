# Profiles & package identities — release/dev coexistence

> **Goal:** installing the GitHub **release** and the local **dev** build on one machine must
> produce two fully independent apps — different identity, different launchers, and **zero shared
> persisted state**. The app "merely loads a profile folder": everything an install stores —
> engine state *and* Terminal's own settings — lives in ONE folder chosen **automatically by
> package identity on first launch** (release → Default, dev → Development), and changeable later
> from the cog (Default / Development / **Browse…**). A **PORTABLE copy** instead **chooses on
> its first launch** — the same picker, led by a **Portable (`<unzip>\profile`)** option — and
> remembers the answer **next to the exe** (`profile.path`), so a portable can be self-contained
> OR deliberately share the Default/Development/any profile. (“Default” is the picker option formerly
> labeled “Production” — renamed because `~/.agentmaster` IS the installed default.)

Status: **shipped & live-verified.** The identity split + per-install profiles are in public
releases (v0.2.0+); an installed first launch **auto-selects** the per-identity default silently —
the Default / Development / Browse… picker is shown by a **portable first launch** (§2a) and the
cog (§2). Engine harness 900+ incl. the profile checks; side-by-side release + dev installs run
cleanly.

---

## 1. The collision matrix (what used to collide, and what fixes it)

| # | Seam | Before | After |
|---|------|--------|-------|
| 1 | **Package identity** | `release.yml` shipped `Package-Dev.appxmanifest` — the SAME `Agentmaster_56k4f06dsfp9r` family the dev loose layout registers. Installing one replaced/broke the other (`0x80073CF6`-class conflicts). | Two manifests: **`Package-Rel.appxmanifest`** (`Name=Agentmaster` — what releases ship, and what v0.1.x already shipped, so existing installs upgrade in place) and **`Package-Dev.appxmanifest`** (`Name=AgentmasterDev`). Publisher stays `CN=Agentmaster` for both — the PFN hash derives from the Publisher ALONE, so the families are `Agentmaster_56k4f06dsfp9r` and `AgentmasterDev_56k4f06dsfp9r` and the release self-signing flow is untouched. Selected by `/p:AgentmasterPackageIdentity=Release` (wapproj; default = Dev). |
| 2 | **Execution alias** | Both declared `agentmaster.exe`. The global `WindowsApps\agentmaster.exe` link points at ONE package — shell launches and the app's own reopen-window dispatch (`_ReopenSavedWindows`) could activate the WRONG install. | Per-identity aliases: release = **`agentmaster.exe`**, dev = **`agentmasterdev.exe`**. `GetWtExePath()` picks by PFN prefix (**Dev tested first** — `Agentmaster` is a prefix of `AgentmasterDev`), and the reopen dispatch resolves `_AgentmasterReopenTarget()` the same way (unpackaged now falls back to the neighbor `Agentmaster.exe` — the GUI binary — instead of silently no-opping). |
| 3 | **Single-instance mutex / window class / AUMID** | Already fixed pre-profiles: `windowClassName` appends the PFN. | Distinct automatically once the families differ. Start menu shows **Agentmaster** and **Agentmaster Dev**. |
| 4 | **The state dir `~/.agentmaster`** | One dir for both: two `SharedEngine`s would clobber `sessions.json`, `open-windows.json`, `windows/<id>.json`, `settings.json`, fight over window records and reopen each other's workspaces. | **Per-install profiles** (§2). Each install resolves its own profile folder; ALL engine state lives there. |
| 5 | **`bridge.json` discovery** | The generated PowerShell forwarder hardcoded `$env:USERPROFILE\.agentmaster\bridge.json` — a no-env hook from one instance's session could route to the OTHER instance's pipe. | `BuildForwarderScript(stateDir)` bakes the **per-profile** `<profile>\bridge.json` path in (PS-single-quoted; unit-tested). The shim already embedded the per-profile `--settings` path. |
| 6 | **Terminal's own settings.json / state.json** | Per-package `LocalState` — isolated, but OUTSIDE the profile (violates "everything in the profile"). | `GetBaseSettingsPath()` honors **`AGENTMASTER_PROFILE`** → `<profile>\terminal\` (exported by the bootstrap before any settings load). Headless hosts (tests/tools) that never ran the bootstrap keep stock paths. |
| 7 | **The "portable" zip** | Built WITHOUT the `.portable` marker — it shared the global unpackaged WT settings dir, and its Agentmaster state went to `~/.agentmaster`. Then (first cut): the marker hard-wired the profile to `<unzip>\profile` — self-contained, but the user never got to say "use my Default (installed) data", and the cog's Change… was a DEAD write (resolution never read the home map for portables). | `release.yml` passes `-PortableMode:$true` (the `.portable` marker). **First launch PROMPTS** — Portable (`<unzip>\profile`, the default) / Default / Development / Browse… — and the answer persists in the exe-side **`profile.path` pointer** (§2a): relative when the profile lives inside the unzip (movable), absolute otherwise. Terminal's own settings ride the CHOSEN profile (`<profile>\terminal\`, seam #6 — the redirect now outranks upstream portable mode's `<unzip>\settings`, which is seeded in on first choice). |
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
  hooks.log                autorunner.log            scanner.log
  forwarder-errors.log
  terminal\settings.json   terminal\state.json      ← Terminal's OWN settings (seam #6)
```

`~/.claude` is **not** part of a profile — transcripts belong to Claude itself and are shared by
design (both instances browse the same conversations; the Sessions browser, resume, fork all
operate on Claude's store).

### Resolution order (`Agentmaster::Profiles::ResolveProfileDir`, ProfileBootstrap.h)

1. **`AGENTMASTER_PROFILE`** env var — explicit override; also what the bootstrap exports so every
   module (exe, TerminalApp.dll, TerminalSettingsModel.dll) resolves identically in-process.
2. **`<exedir>\profile.path`** — the **exe-side pointer** (§2a): one line naming the profile folder
   this exe location uses; a relative line resolves against the exe dir. Written by a portable's
   first-launch picker; honored for installed copies too when someone plants one next to the exe
   (creating it there needs a writable exe dir — an MSIX package dir isn't, so that's a power-user
   loose-layout move).
3. **`<exedir>\profile`** when `<exedir>\.portable` exists and no pointer was written yet — the
   self-contained default. The EXE prelude **prompts** here on a portable's first interactive
   launch (§2a); headless/library resolution never asks (Rule #15) and lands here silently. A
   portable **never falls through to the home map below** — the one shared `Unpackaged` slot can't
   tell two unzips apart and would cross-contaminate them (and the legacy tools that key on it).
4. **The saved per-install choice** — `%USERPROFILE%\.agentmaster.profiles`, a tiny UTF-8 text map
   (`<PFN or "Unpackaged">=<absolute dir>` per line). Plain text, NOT JSON, on purpose: the
   WindowsTerminal EXE links `TerminalApp.dll` (not the static lib) and parses it without JSON
   helpers; it lives OUTSIDE any profile (chicken-and-egg) and outside MSIX-virtualized AppData,
   so it's shared, honest, debuggable, and survives package reinstall.
5. **Per-identity default** — `~/.agentmaster` (release package **and** unpackaged/headless runs:
   the historical dir, which keeps the test harness and old tooling unchanged) or
   `~/.agentmaster-dev` (the `AgentmasterDev` package).

Resolution is cached process-wide — a profile cannot change mid-run; the cog's "Change…" applies
on the next launch.

### §2a The portable first-launch choice + the `profile.path` pointer

A **portable copy** (the `.portable` marker next to the exe) integrates with the profile system
instead of hard-coding `<unzip>\profile`:

- **First interactive launch** (marker present, no `profile.path` yet — and after the §2b INSTALL
  question, which comes first and, for an installed copy, answers this one silently): `EnsureProfileResolvedAtStartup`
  shows the SAME picker the cog uses, with a leading, defaulted **"Portable profile (self-contained)"**
  command link for `<unzip>\profile`, then Default / Development / Browse… (each option’s path line
  reads “— existing data” when that folder already holds something, so adopting an in-use profile
  vs starting fresh is an informed pick). When a pre-existing
  `<unzip>\profile` holds data (an upgrade from a build that used it unconditionally), the "copy
  existing data" checkbox offers migrating it into a non-portable pick; picking Portable keeps that
  data in place (Enter = keep everything). **Cancel** — and a headless `-Embedding` activation —
  lands on `<unzip>\profile` **without persisting**, so the next interactive launch asks again.
- **The pointer file `<exedir>\profile.path`** persists the answer *next to the exe* (the home
  map's one shared `Unpackaged` slot can't tell two unzips apart). Plain UTF-8, `#` comments +
  blank lines skipped, first value line wins. The stored line **prefers a RELATIVE path** — used
  whenever the chosen dir sits INSIDE the exe dir (`profile`, `data\p1`) so a moved unzip keeps
  working — and falls back to **absolute** for anything else (the home-dir defaults, another
  drive); a `..`-relative spelling is deliberately never written (it would silently re-anchor to a
  wrong, then auto-created, folder if the unzip moved). `Install-Agentmaster.ps1` preserves it on
  upgrade alongside `settings\` + `profile\`.
- **Terminal's own settings follow the chosen profile** (`<profile>\terminal\` — seam #6 now
  outranks upstream portable mode's `<unzip>\settings` in `GetBaseSettingsPath`); the bootstrap
  seeds `<profile>\terminal\` from a pre-choice `<unzip>\settings` (copy-if-absent, idempotent) so
  an upgrading portable keeps its terminal look. Headless hosts that never ran the bootstrap still
  fall through to the upstream exe-side `settings` folder, unchanged.
- **The cog's Change…** persists through `PersistProfileChoice`: a portable (or any copy already
  steered by a pointer) rewrites `profile.path`; everything else writes its home-map slot. This
  also fixed the old dead-write: a portable's cog change used to land in the home map, which
  portable resolution never read. A refused pointer write (read-only exe dir) is surfaced with the
  store path instead of silently pretending the change took.
- An **installed** (user/machine) copy is unchanged: first launch auto-selects Default
  (release) / Development (dev) with no prompt; only a manually planted `profile.path` next to its
  exe would steer it (resolution step 2).
- **The in-app updater covers portables** (Updater.h): a portable copy is an updater channel and
  **self-updates IN PLACE** — its version reads from the exe-side `.am-version` stamp the zip
  ships, "Update now" downloads the release's arch-matched portable zip and `am-update.ps1
  -Portable` swaps the unzip's binaries while preserving `settings\` + `profile\` + `profile.path`,
  restamps `.am-version`, and relaunches. The zip's wrapper folder is **`agentmaster-<ver>`**
  (renamed from upstream's `terminal-<ver>`; installers unwrap both spellings).

### §2b The portable first-launch INSTALL question + the `install.path` decision file

Before the §2a profile question — before ANY file is written — a pristine portable copy asks
**"Where should Agentmaster live?"** (`AgentMaster/PortableInstall.h`, `EnsureInstallDecidedAtStartup`,
run from the EXE prelude right after the single-instance handoff and BEFORE
`EnsureProfileResolvedAtStartup`). *Pristine* = the `.portable` marker with neither an `install.path`
nor a `profile.path` beside the exe (a portable that already chose a profile before this question
existed is never nagged; an explicit `AGENTMASTER_PROFILE` skips it; a `-Embedding` activation never
prompts — it runs self-contained and persists nothing). The command links, the first defaulted:

- **Install to your user folder** — `%USERPROFILE%\.agentmaster\bin` (the Default profile's `bin`
  sibling). The option ADAPTS to a previous user install of ours (the HKCU Apps & Features entry with a
  live app exe): **Update the installed Agentmaster (vX → vY)** upgrades it in place (its `settings\`
  + `profile\` + `profile.path` + `install.path` preserved; only entries the zip SHIPS are replaced, so
  a folder shared with other files keeps them; refused while something runs from it), or — when it is
  already the same/newer version — **Use the installed Agentmaster**: this unzip records
  `install.path` = that dir and hands the launch off to it. It is then a **launcher stub**: every later
  launch of the unzip forwards its commandline to the install (a vanished install re-asks).
- **Install to a folder you pick…** — Browse; a non-empty folder that isn't already ours gets an
  `\Agentmaster` subfolder (the usual installer manners). A destination inside the running unzip, or
  containing it, is refused.
- **Install here** — the folder holding the RUNNING binary (`ExeDirPath`, never the cwd): no copy,
  just the registration below.
- **Keep it portable, run from here** — no shortcuts, no registry; writes `install.path` = `portable`
  so it never asks again, then the classic §2a profile picker follows.
- **Ask me later** (also Cancel / X / Esc) — writes NOTHING: this launch runs on the self-contained
  `<exedir>\profile` WITHOUT persisting a profile choice (the §2a picker is deferred too —
  `EnsureProfileResolvedAtStartup(allowUi, deferPortablePicker)` — one "later" defers every
  first-launch question instead of trading one dialog for another), and the next launch asks again.

**What an install is** — MSI-like, per-user, with no MSI, package, certificate or admin: the binaries
copied (or registered in place); a **desktop** shortcut; a **Start-menu** shortcut (Start search finds
it — the `.lnk` carries the unpackaged AUMID the installed copy runs under, `unpackagedAumidForImagePath`
in `WindowEmperor.cpp` == the app's own formula, which is what routes an unpackaged app's toasts); an
**"Open in Agentmaster"** right-click verb on folders, folder backgrounds and drives
(`HKCU\Software\Classes\{Directory | Directory\Background | Drive}\shell\Agentmaster`, command
`"<exe>" -d "%1|%V"` — the classic registry form of what the MSIX shell extension provides;
Windows 11 lists it under *Show more options*); an **Apps & Features** entry
(`HKCU\...\Uninstall\Agentmaster` — DisplayVersion from `.am-version`, InstallLocation, DisplayIcon,
`UninstallString` = `"<exe>" --uninstall-portable`, NoModify/NoRepair, EstimatedSize); and the install
dir on the **user PATH** (`agentmaster-cli.exe` reachable from any shell; `WM_SETTINGCHANGE`
broadcast). The work runs on a worker under a marquee TaskDialog (*Installing Agentmaster…*, no
cancel), then the INSTALLED copy is launched and the installing process exits like the updater
handoff (an in-place install just carries on); any failure is shown and the question returns.

**The installed copy's state:** it is an installed copy, so it uses the per-identity **Default**
profile (`~/.agentmaster`) silently — the installer writes its `profile.path` (absolute) — and
migrates whatever a "later" run accumulated in `<unzip>\profile` into it (`MigrateProfileData`,
copy-if-absent; `bin/` is now excluded from every profile migration, since the default install dir
sits INSIDE the Default profile). It keeps its `.portable` marker: it still self-updates in place
(the zip swap in `am-update.ps1 -Portable` / `Install-Agentmaster.ps1` preserves `install.path` beside
`profile.path`), and `RefreshInstalledRegistration` (every launch, registry reads only) re-stamps the
Apps & Features version + the verbs' command paths after such an update.

**Existing installs — never overridden, taken over where we can:**

- the installed **package** (the MSIX release family `Agentmaster_56k4f06dsfp9r`, incl. an
  admin-trusted one — `GetPackagesByPackageFamily`): its files are untouchable and untouched; the
  install adopts its DATA (the same Default profile), and a verification checkbox — checked by default
  unless the package is RUNNING (its windows would be closed; `packageRunning` = a process image under
  the package path) — removes it afterwards (hidden Windows PowerShell, `Remove-AppxPackage`, per-user,
  no admin; the GUI under the package path is stopped first and its ConPTY hosts DRAINED, never
  killed). A failed removal is a note; the install still succeeds.
- a **machine-wide** entry (`HKLM\...\Uninstall\Agentmaster`, administrator-managed): left alone,
  called out in the prompt — this copy installs for the current user only.
- a previous **user install** of ours: the adaptive first option above (update / use).

**The decision file `<exedir>\install.path`** (plain UTF-8, `#` comments, first value line wins —
the `profile.path` idiom): `installed` (this folder IS the install — in place, or an install's
destination) · `portable` (stay portable, never ask) · `<absolute dir>` (installed elsewhere — a
launcher stub). A relative/garbage value reads as none (re-asks). Both installer swaps preserve it.

**Uninstall:** `Agentmaster.exe --uninstall-portable` (the Apps & Features entry; the cog's About →
*Uninstall Agentmaster…* routes an installed portable there too) is handled in the prelude BEFORE the
single-instance handoff (a running instance would otherwise receive the flag as a commandline): a
confirm, then the embedded `am-update.ps1` is materialized into `%TEMP%\Agentmaster-update` and run
detached with `-UninstallPortable -PortableDir <dir> -WaitPid <pid>` — the shortcuts (only those
pointing INTO the folder), the three verbs, the Apps & Features entry (only when its InstallLocation
IS the folder), the PATH entry and the binaries go; `settings\` + `profile\` + `profile.path` and the
profile folder itself are kept. The whole flow logs `[install]` lines into the Default profile's
`hooks.log` (only after a decision that writes anyway — a pristine "later" creates nothing).

### First launch — installed auto-selects by identity (no prompt); portable asks

`WindowEmperor::HandleCommandlineArgs` calls `EnsureProfileResolvedAtStartup(allowUi)` **after**
winning the single-instance handoff (a handed-off second process never shows UI) and **before**
anything reads persisted state (`ReloadSettings`, `ApplicationState`, the `windows/*.json` reopen
scan, and — much later — the engine's `AgentmasterStateDir()`). With no saved choice an INSTALLED
copy **does not prompt** — it auto-selects the **per-identity default** (`DefaultProfileDir()`)
and persists it:

- the **RELEASE** install (`Agentmaster`) → **Default**, `%USERPROFILE%\.agentmaster`
- the **DEV** install (`AgentmasterDev`) → **Development**, `%USERPROFILE%\.agentmaster-dev`
- unpackaged / portable-without-marker → the release default (the historical `~/.agentmaster`)
- a **PORTABLE** copy (`.portable` marker, no `profile.path` pointer yet) instead **prompts** —
  first the §2b INSTALL question (install / keep portable / later), then, for a kept-portable copy,
  the §2a picker; Cancel / "Ask me later" / `-Embedding` lands on `<unzip>\profile` un-persisted
  (asks again next launch); an INSTALLED copy has its `profile.path` written to the Default profile

It then **seeds `<profile>\terminal\`** with the install's current Terminal settings (from package
`LocalState` / the unpackaged dir / a portable's `<unzip>\settings`) so the first redirected launch
looks identical instead of resetting to defaults. The installed auto-pick needs no UI, so it runs
identically for a real launch and a `-Embedding` COM activation (defterm handoff) — `allowUi`
governs the portable first-launch prompt (§2a) and the "profile in use by another instance"
warning, never the installed choice.

> Earlier builds showed a one-time **Default / Development / Browse…** `TaskDialogIndirect`
> picker (with a "Copy existing data from `~/.agentmaster`" migrate checkbox). That UI still exists
> (`ShowProfilePicker`, plus `MigrateProfileData` for the migrate path — a skip-existing copy that
> excludes `locks/`, `shim/`, `bridge.json` and `*.tmp`, all machine-global or regenerated per-profile
> at engine init) and is reached from a **portable first launch** (§2a — with the leading Portable
> option) and the cog's **Change profile folder…** — an installed first launch is silent. To land an
> installed copy on a non-default folder (a synced drive, a shared dir, or to copy the legacy
> `~/.agentmaster` into a fresh dev profile), launch once, then switch via the cog (below).

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
the family is free after step 1 — and ITS first launch silently lands on **Default**
(`~/.agentmaster`); change it from the cog if needed.

## 4. What runs where (quick reference)

| | Release install | Dev loose layout | Portable zip |
|---|---|---|---|
| Package family | `Agentmaster_56k4f06dsfp9r` | `AgentmasterDev_56k4f06dsfp9r` | (unpackaged) |
| Start menu | Agentmaster | Agentmaster Dev | — |
| Alias | `agentmaster` | `agentmasterdev` | run `Agentmaster.exe` (or the `WindowsTerminal.exe` compat shim) |
| Manifest | `Package-Rel.appxmanifest` (`/p:AgentmasterPackageIdentity=Release`; release.yml stamps the version here) | `Package-Dev.appxmanifest` (default) | n/a |
| Default profile | `~/.agentmaster` | `~/.agentmaster-dev` | **chosen at first launch** (picker, default `<unzip>\profile`) → `<unzip>\profile.path` pointer (§2a) |
| Terminal settings | `<profile>\terminal\` | `<profile>\terminal\` | `<profile>\terminal\` (seeded from a pre-choice `<unzip>\settings`) |
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
  copies ever need distinct profiles, key by exe-path hash instead (the AUMID already does this) —
  or drop a `profile.path` pointer next to each copy's exe (§2a), which already gives per-location
  profiles today; PORTABLE copies are covered (each unzip remembers via its own pointer).
