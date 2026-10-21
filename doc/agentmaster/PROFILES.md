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
cleanly. **2026-10-21 (in the dev 0.0.1.33 build, where a packaged install never runs it; in no release):** the install question prefers the per-user install, offers
installing for all users and handing the copy to the signed-in user, never leaves the app running with
rights it was installed with, and says why an elevation failed; installs and uninstalls touch only what
a release ships (`.am-manifest`); the scenario matrix and its test-desktop evidence are §2c.

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
| 8 | **Same profile, two instances** (user points both installs at one folder via Browse…) | n/a (new failure mode the picker introduces) | A kernel **profile mutex** (`Local\Agentmaster.profile.<fnv1a64(dir)>`, auto-released on process death — can't go stale): the second instance gets a warn-and-confirm before touching anything. An `ERROR_ACCESS_DENIED` from `CreateMutex` counts as held (`ProfileMutexVerdict`, 2026-10-21): an ELEVATED instance's mutex cannot be opened by an unelevated one, which used to read as "free". |

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
- An **installed** copy is unchanged: first launch auto-selects Default (release) / Development (dev)
  with no prompt; only a manually planted `profile.path` next to its exe would steer it (resolution
  step 2). A **machine copy** (§2b: `install.path` = `machine`, or a portable folder this account cannot
  write) skips the portable steps entirely — no question, no pointer written next to the exe — and
  resolves each user's profile from their own home map or the per-identity default.
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
nor a `profile.path` beside the exe, in a folder this account can write (a folder it cannot write is a
*machine copy*, below). An explicit `AGENTMASTER_PROFILE` skips it; a `-Embedding` activation and a
process in session 0 (a service, SYSTEM) never prompt — they run self-contained and persist nothing.

**The options** (command links; the pre-selected one reads *(recommended)*):

- **Install just for you** — `%USERPROFILE%\.agentmaster\bin` (the Default profile's `bin` sibling),
  no administrator rights. It ADAPTS to a per-user install of ours (the HKCU Apps & Features entry with
  a live app exe): **Update the installed Agentmaster (vX → vY)** upgrades it in place, or — when it is
  already the same or a newer version — **Use the installed Agentmaster (vX)**: this unzip records
  `install.path` = that dir and becomes a **launcher stub** that forwards every later launch to it (a
  vanished install re-asks). When another account runs the copy, the label names the account it would
  install for (*Install for DOMAIN\user*).
- **Install for all users of this PC** — `%ProgramW6432%\Agentmaster`, needs administrator rights (a UAC
  prompt; only the install itself runs elevated). It ADAPTS to an all-users install of ours (the HKLM
  entry with a live exe): **Update the Agentmaster installed for all users (vX → vY)**, or **Use the
  Agentmaster installed for all users (vX)** — same or newer: no rights needed, this unzip becomes a stub.
- **Set it up for DOMAIN\signed-in-user** — only when ANOTHER account runs this copy with the rights to
  act for the user signed in to Windows (an administrator's UAC credentials, *Run as different user* with
  an elevated administrator, SYSTEM in a user's session): starts this same copy again as the signed-in
  user, without administrator rights; they then see this question in their own account. Nothing is
  installed for the running account (only its install trace lands in its own profile folder).
- **Install to a folder you pick…** — Browse; a non-empty folder that isn't already ours gets an
  `\Agentmaster` subfolder. A destination inside the running unzip, or containing it, is refused, and so
  is one this account cannot write (Program Files without rights): the message points to *Install for
  all users*.
- **Install here** — the folder holding the RUNNING binary (`ExeDirPath`, never the cwd): no copy, just
  the registration.
- **Keep it portable, run from here** — no shortcuts, no registry; writes `install.path` = `portable` so
  it never asks again, then the classic §2a profile picker follows.
- **Ask me later** (also Cancel / X / Esc) — writes NOTHING: this launch runs on the self-contained
  `<exedir>\profile` WITHOUT persisting a profile choice (the §2a picker is deferred too —
  `EnsureProfileResolvedAtStartup(allowUi, deferPortablePicker)`), and the next launch asks again.

**The pre-selected option** (`DecidePromptDefault`; the user's rule of 2026-10-21: prefer the per-user
install, which needs no administrator rights): *Set it up for the signed-in user* when another account
can hand the copy over; otherwise the per-user option whenever this user already has a per-user install
(use it or update it — no second copy either); otherwise *Use the Agentmaster installed for all users*
when one exists that is this version or newer; otherwise *Install just for you*.

**Who runs it** (`DetectInstallIdentity` → `DecideAccountPlan`): the token's user and elevation, and
the user signed in to the process's Windows session (`WTSQuerySessionInformation`, then
`LookupAccountName` + `EqualSid`; a name compare when the lookup fails). The plans: **AsIs** (the
signed-in user, unelevated), **AsIsElevated** (the signed-in user, elevated: *Run as administrator*, or
any administrator on a PC with UAC turned off), **OtherAccountRelaunch** (another account that is
elevated or SYSTEM), **OtherAccountExplain** (another account without those rights — a standard user,
or an administrator's filtered token: the question explains how to set it up for the signed-in user),
**NoSessionUser** (session 0: never prompt). Every probe fails OPEN — an unreadable session user reads
as "this user", so a lookup hiccup never redirects an install.

**Rights only where they are needed, and dropped right after:**

- *Install for all users* runs the embedded `am-update.ps1 -InstallMachine` ELEVATED
  (`RunEmbeddedScript(..., elevate = true)`, the "runas" verb); this process waits UNelevated and then
  starts the installed copy itself.
- When this process is the signed-in user running ELEVATED, the copy it installed or adopted is started
  WITHOUT administrator rights: `StartAsSessionUser` → `am-update.ps1 -RunAsMainUser` → a one-shot
  scheduled task for the signed-in user (interactive logon, limited rights) that starts it through
  Explorer (no arguments) or a hidden PowerShell (with arguments); the task is deleted once that launcher
  has finished. The app must never keep rights it was not meant to run with: every `claude.exe` it starts
  would run elevated. (On a PC with UAC turned off there is no unelevated token to drop to; the hop runs
  and changes nothing.)
- A step that could not get its rights says why (`ScriptStartFailureText`): a declined UAC prompt simply
  returns to the question; a refusal by the PC's policy (`ERROR_ACCESS_DISABLED_BY_POLICY`, 1260 — a
  standard user under *Automatically deny elevation requests*) reads "Windows does not let this account
  run programs as administrator (a setting of this PC)" plus what to do; a missing PowerShell or another
  Windows error is named as such.

**What an install is.** *Per user* — MSI-like with no MSI, package, certificate or admin: the binaries
copied (or registered in place); a **desktop** shortcut; a **Start-menu** shortcut (Start search finds
it — the `.lnk` carries the unpackaged AUMID the installed copy runs under, `unpackagedAumidForImagePath`
in `WindowEmperor.cpp`, which routes an unpackaged app's toasts); an **"Open in Agentmaster"** verb on
folders, folder backgrounds and drives (`HKCU\Software\Classes\{Directory | Directory\Background |
Drive}\shell\Agentmaster`, `"<exe>" -d "%1|%V"`; Windows 11 lists it under *Show more options*); an
**Apps & Features** entry (`HKCU\...\Uninstall\Agentmaster` — DisplayVersion `major.minor.patch[.build]`,
InstallLocation, DisplayIcon, `UninstallString` = `"<exe>" --uninstall-portable`, NoModify/NoRepair,
EstimatedSize); and the folder on the **user PATH** (`WM_SETTINGCHANGE` broadcast). `install.path` =
`installed`, `profile.path` = the Default profile (absolute). The work runs under a marquee TaskDialog;
then the INSTALLED copy is launched and the installing process exits (an in-place install carries on).
*For all users* — the same pieces machine-wide, written by the shared PowerShell core
(`Install-AmMachineCopy`): the all-users desktop and Start menu, `HKLM\Software\Classes` verbs,
`HKLM\...\Uninstall\AgentmasterAllUsers`, the machine PATH; `install.path` = `machine` and NO `profile.path`
(each user resolves their own profile). It refuses a release whose zip has no `.am-manifest` (built
before 2026-10-21): that app would not know what `install.path = machine` means. Its shortcuts and its
Apps & Features entry are named **Agentmaster (all users)**, a per-user install's **Agentmaster**
(`Get-AmRegistrationName`), and its Apps & Features entry has its own key NAME (`AgentmasterAllUsers`,
`kMachineUninstallKey`; both since the evening of 2026-10-21). Before, both installs said *Agentmaster* in
Start, on the desktop and in Apps & Features — and both entries were `...\Uninstall\Agentmaster`: Windows
lists an Uninstall key name ONCE, so Settings → Installed apps showed only the all-users entry and the
per-user copy could not be uninstalled there (measured in the test desktop: "1 app found"; with the HKLM
key renamed, both listed). An all-users copy registered under the old names gets the new ones at its next
update (the old `Agentmaster.lnk` and the old-name entry are replaced only when they point into the
folder; detection reads the new key, then the old). Where both installs exist, Windows puts the machine
PATH BEFORE the user PATH, so `agentmaster-cli` in a new shell is the all-users copy's; the "Open in
Agentmaster" verbs are the per-user copy's (`HKCU\Software\Classes` outranks `HKLM\Software\Classes`).

**Machine copies** (`Profiles::IsMachineCopyIn`): a portable folder whose `install.path` says `machine`,
or one THIS account cannot write (a write probe — the all-users install as a standard user sees it, a
copy an administrator unpacked into Program Files). It runs like an installed program: no question,
never a pointer or a profile written next to the exe; each user's profile resolves from their own home
map (the `Unpackaged` slot) or the per-identity default (`~/.agentmaster`), and the cog's profile change
is stored in that home map. A portable copy in a folder the user CAN write but never decided about stays
an ordinary portable.

**The installed copy's state** (per user): the Default profile (`~/.agentmaster`) — or, when the MSIX
release package already chose a profile (the home map's `Agentmaster_56k4f06dsfp9r` entry), that one,
so a switch from the package keeps every session (`UserInstallProfileDir`). Whatever a "later" run
accumulated in `<unzip>\profile` is migrated in (`MigrateProfileData`, copy-if-absent; `bin/` excluded,
since the default install dir sits INSIDE the Default profile). It keeps its `.portable` marker and
self-updates in place; `RefreshInstalledRegistration` (every launch, registry reads only) re-stamps the
Apps & Features version and the verbs' command paths after such an update. An all-users copy updates in
place too: the in-app update runs only its file swap elevated (`am-update.ps1 -Portable` →
`Invoke-AmElevatedPhase` when the folder is not writable) and relaunches the app unelevated; the
elevated swap re-stamps the HKLM entry. When that swap does not happen — the approval declined (1223),
refused by the PC's policy (1260), or the swap failed (another user still runs Agentmaster from the
folder) — the updater asks (`Read-MachineUpdateChoice`): **[A]** ask for approval again (not after a
policy refusal: it cannot help), **[U]** install the new version **just for this user** instead (the
default: no rights, now or later; `profile.path` → the profile the app ran on, which the app passes as
`-ProfileDir`, so every session stays; the all-users copy stays for the other users), **[C]** cancel.
Before the evening of 2026-10-21 it ended there, with the app closed.

**An update never leaves the user without the app.** The app quits to let `am-update.ps1` run; when the
update fails, is cancelled, or keeps the installed version, the script starts the INSTALLED version
again (`Restart-PreviousApp`: after waiting for the closing app, and not when THIS account's Agentmaster
already runs from there again — another user's instance of an all-users copy is visible to the check, path
and owner both, and counting it kept the user's app from coming back in the test desktop) — after a
failure, 30 s later or at a key press, so the window says why first. Right before that start it leaves a
one-shot marker in the profile (`am-update-restarted.txt`: the tag and the write time), which the started
app reads and deletes (`ConsumeRestartedAfterUpdateMarker`, fresh for 10 minutes): that run behaves as
*Remind me next restart* — no startup or hourly prompt, the cog's check stays live — instead of offering
the same update again at once (seen in the test desktop right after a cancel). A swap that fails half way
first puts the previous version back (next paragraph). The generated `am-update.cmd`
(and both uninstall launchers) start with `chcp 65001`: they are UTF-8, and cmd.exe decodes a batch file
in the console's code page, so a non-ASCII profile or install path — a Windows user name in Hebrew —
reached PowerShell garbled before (measured on a CP 437 console: `╫É╫£╫Ö` for three Hebrew letters).

**The manifest `<exedir>\.am-manifest`** (written by `New-UnpackagedTerminalDistribution.ps1
-PortableMode` since 2026-10-21): the top-level names the build ships, one per line. An upgrade — the
C++ `PerformInstall` and the PowerShell core's `Install-AmLayout` alike — replaces ONLY shipped names:
the new layout's, and those the OLD install's manifest lists (a file a newer version stopped shipping
goes). An old install without a manifest is read through the **legacy list** — the names releases
shipped before it (`kLegacyShippedNames` / `IsLegacyShippedLeaf` in C++, `$script:AmLegacyShippedNames`
+ `Microsoft.Terminal.*.dll` in the core; keep the two in step). An uninstall removes only shipped names
too. A user's own files in the folder, another `.exe` included, and `settings\`, `profile\`,
`profile.path` always stay; `install.path` stays on an upgrade and goes on an uninstall. A manifest line
is accepted only as a plain name (no separator, drive or stream colon, wildcard, `.` or `..`), so a
corrupt or planted manifest cannot name anything outside the folder. An upgrade MOVES what it replaces
into a set-aside folder inside the install folder (`.am-previous-…`: the same volume, a rename each, and a
running image can be renamed) before it copies the new layout, and puts it back when the copy fails
(`detail::SetAsideShippedEntries` / `RestoreSetAside` in C++, `Install-AmLayout` / `Restore-AmSetAside` in
the core); a destination entry that cannot be moved stops the upgrade before anything changed. Before the
evening of 2026-10-21 the old files were deleted first, and a copy that failed half way left a folder
holding neither version. A set-aside folder an interrupted run left is deleted by the next upgrade or
uninstall (either installer: the prefix is shared).

**Existing installs — never overridden, taken over where we can:**

- the installed **package** (the MSIX release family `Agentmaster_56k4f06dsfp9r`, incl. an
  admin-trusted one — `GetPackagesByPackageFamily`): its files are untouchable and untouched; a per-user
  install adopts its DATA (the profile it chose), and a verification checkbox — checked by default unless
  the package is RUNNING — removes it afterwards (hidden Windows PowerShell, `Remove-AppxPackage`,
  per-user, no admin; the GUI under the package path is stopped first and its ConPTY hosts DRAINED, never
  killed). A failed removal is a note; the install still succeeds.
- an **all-users** install of ours (`HKLM\...\Uninstall\AgentmasterAllUsers` — or, registered before the
  evening of 2026-10-21, `...\Uninstall\Agentmaster` — with a live exe): offered for use
  (current) or update (older) — see the options.
- a previous **per-user** install of ours: the adaptive first option.
- the copy **`Install-Agentmaster.ps1` v0.7.10 and older** made — `%LOCALAPPDATA%\Programs\Agentmaster`
  with a Start-menu entry *Agentmaster (Portable)* and no registry entry (`LegacyScriptInstallDir`; since
  the evening of 2026-10-21): when no HKCU install exists it is the adaptive first option too (*the copy the
  install script made*), or — when the app's first launch made that folder a launcher stub — the install
  the stub names; never offered to itself (`LegacyScriptOfferDir`). Its update registers it in place (a
  folder kept portable gets only its files), copies the data a never-decided copy kept in
  `<folder>\profile` into the profile (copy-if-absent), and retires *Agentmaster (Portable)* — only when it
  leads into the install, directly or through a stub (`RetireLegacyScriptShortcut`; the core's
  `Remove-AmLegacyScriptShortcut`, which until then removed it whatever it pointed at, also after an update
  that kept the folder portable and left it no Start-menu entry). Before, a new zip offered a second
  per-user install and the script's copy stayed behind.

**The decision file `<exedir>\install.path`** (plain UTF-8, `#` comments, first value line wins — the
`profile.path` idiom): `installed` (this folder IS the per-user install; the older spelling `here` reads
the same) · `machine` (this folder is the install for all users) · `portable` (stay portable, never ask)
· `<absolute dir>` (installed elsewhere — a launcher stub). A relative/garbage value reads as none
(re-asks). Every installer swap preserves it.

**Uninstall:** `Agentmaster.exe --uninstall-portable` (the Apps & Features entry; the cog's About →
*Uninstall Agentmaster…* routes the same way) is handled in the prelude BEFORE the single-instance
handoff and acts on what the folder IS (`DecideUninstallTarget`): a per-user install → confirm, then the
embedded `am-update.ps1 -UninstallPortable -PortableDir <dir> -WaitPid <pid>` runs detached from
`%TEMP%\Agentmaster-update`; an all-users install (or an unwritable portable folder) → confirm, then
`-UninstallMachine` runs ELEVATED (`LaunchMachineUninstaller`; a declined prompt changes nothing quietly,
a refused one says why); anything else (the package, a plain or kept portable, a launcher stub, a build
folder) → an information dialog, nothing removed (before 2026-10-21 the flag ran the portable
uninstaller on WHATEVER folder the exe sat in). The uninstallers remove the shortcuts that point INTO
the folder, the three verbs that point into it, the Apps & Features entry whose InstallLocation IS the
folder, the PATH entry and the shipped files; every profile folder is kept. The whole flow logs
`[install]` lines into the running account's Default profile `hooks.log` (only after a decision that
writes anyway — a pristine "later" creates nothing).

**The scripts.** `tools\Install-Agentmaster.ps1` (the downloadable installer) asks **"Where should
Agentmaster be installed?"** — `[1] Just for you` (no administrator rights; the default on a fresh PC),
`[2] For all users of this PC` (administrator rights: only that step elevates), `[3] As the app package
(MSIX; administrator approval once per release, for its certificate)`, `[C] Cancel` — or takes
`-Scope User|Machine|Package` (`-Portable` = User, `-Elevate` = Package). With something already
installed the default is to upgrade THAT. It finds the user signed in to Windows (`Get-AmIdentity`: the
owner of `explorer.exe` in its session, else the console user): run by SYSTEM or by another
administrator it installs for the signed-in user — the per-user part runs as them in their session
(`Invoke-AmPhaseAsUser`, a one-shot task, its transcript printed back) — and starts the app as them;
run by another account without those rights it stops before downloading anything and says how
(`-ForCurrentAccount` installs for the running account instead). A console whose input is redirected
(CI, an agent, `< NUL`) cannot answer: the run says which default it took. ⚠ A HIDDEN console is still
a console: an unattended run in a hidden window (a scheduled task, a deployment tool) must pass
`-NonInteractive`, or a question it reaches (the MSIX certificate one) waits for a key nobody can press
(seen in the test desktop). `-Uninstall -Scope …` removes. `src\cascadia\TerminalApp\AgentMaster\am-update.ps1` (embedded in the app exe, run by the app)
carries the modes: the package update (with **switch to the per-user install** as the answer when an
administrator is not at hand for the release's certificate), `-Portable` (the in-place update),
`-UninstallPortable`, `-UninstallMachine`, `-InstallMachine`, `-RunAsMainUser`. Both scripts carry ONE
**shared install core**, byte-identical between the `AGENTMASTER SHARED INSTALL CORE` markers
(`tools\tests\Test-AgentmasterInstaller.ps1` checks it, and that every core function is exported to the
phase scripts). Both stay pure ASCII: the app writes `am-update.ps1` without a BOM, and Windows
PowerShell 5.1 reads a BOM-less script in the ANSI code page.

### §2c Rights and accounts over time — the scenario matrix

Checked on 2026-10-21 (the sources of that day's install commit; in the dev 0.0.1.33 build, where a packaged install never runs it, and in no release) in a disposable Windows 11 25H2 test
desktop with three accounts — the signed-in administrator, a second administrator and a standard user —
first with UAC off (every administrator token unsplit), then with UAC on (administrators elevate without a
prompt, standard users' elevation is refused by policy, so the refusal path runs). The C++ flow ran
through a probe exe (`tools\install-qa\`, whose README has the whole procedure to repeat it) that IS the
app's startup prelude (`HandleUninstallFlagAtStartup` →
`EnsureInstallDecidedAtStartup` → `EnsureProfileResolvedAtStartup` → `RefreshInstalledRegistration`, the
real `am-update.ps1` embedded) and logged each step; the PowerShell installer ran against the real
v0.7.10 release.

| Who starts it | What | Result |
|---|---|---|
| the signed-in user, unelevated | a fresh unzip | the question; *Install just for you* recommended; the installed copy starts directly (unelevated) |
| the signed-in user, ELEVATED (*Run as administrator*, or UAC off) | a fresh unzip | the same question plus *running with administrator rights*; the installed / adopted copy is started through the task and runs **unelevated** (measured: `elevated=0`, ~6 s) |
| the signed-in user, unelevated | *Install for all users* | one elevation for the install script only; the copy starts unelevated |
| the signed-in user | a newer unzip / an older one | *Update the installed Agentmaster (v0.8.1 → v0.8.2)* / *Use the installed Agentmaster (v0.8.3)* — never a downgrade; the unzip becomes a stub |
| the signed-in user | per-user AND all-users installs both current | the per-user one recommended (fixed during the QA: the all-users one was) |
| another administrator, elevated (UAC credentials, *Run as different user*) | a fresh unzip | *Set it up for <signed-in user>* recommended; the copy restarts as the signed-in user (~13 s), who gets their own question |
| another account without rights (standard, or an administrator's filtered token) | a fresh unzip | the warning with how to set it up for the signed-in user; its own per-user install offered by name; a current all-users install recommended when it has none |
| a standard user | the all-users install | no question; its own profile (`C:\Users\<them>\.agentmaster`); the folder untouched |
| a standard user, elevation refused by policy | *Update for all users* / uninstall the all-users install / the in-app update of it | "Windows does not let this account run programs as administrator (a setting of this PC)" + what to do; the question returns / nothing removed / the version stays (fixed during the QA: it read "PowerShell or the script is missing" / "declined") |
| SYSTEM in session 0 | a fresh unzip | no question; nothing persisted |
| the signed-in user | the in-app update of the all-users install | only the file swap elevated; HKLM re-stamped; the app relaunched unelevated |
| anyone | the upgrade of a pre-manifest install (the real v0.7.10, 28 entries) | the old binaries removed through the legacy list, the user's files kept (fixed during the QA: the C++ upgrade left them) |
| `Install-Agentmaster.ps1`, the signed-in user | interactive | the question above; `[2]` with v0.7.10 refused cleanly (no manifest) |
| `Install-Agentmaster.ps1`, SYSTEM | `-Scope User -Launch` | installed for the signed-in user through a phase in their session; the app runs as them |
| `Install-Agentmaster.ps1`, another administrator | `-Scope User -Force -Launch` | installed for the signed-in user; their running app closed (GUI only), replaced, relaunched as them; nothing for the administrator |
| `Install-Agentmaster.ps1`, a standard other account | `-Scope User` | refused before downloading (fixed during the QA: it downloaded first and its first line claimed it would install for the signed-in user); `-ForCurrentAccount` installed for itself without rights |
| `Install-Agentmaster.ps1` | `-Uninstall -Scope User` (a pre-manifest install) | everything but `profile.path` removed through the legacy list |
| `Install-Agentmaster.ps1`, the signed-in user, unelevated | `-Scope Package -NonInteractive` (the real v0.7.10 bundle) | the release's certificate trusted through one elevation (that step only), the package added for the user |
| `Install-Agentmaster.ps1`, a standard user, elevation refused by policy | `-Scope Package -ForCurrentAccount` | "Windows does not let this account run programs as administrator …, and the MSIX package needs each release's signing certificate trusted for the whole PC. Install just for you instead" — before the 20 MB bundle is downloaded (fixed during the QA: it downloaded first, and so did the app's package update) |

**Over time:**

- **A per-user install never needs rights** — not to install, update or uninstall, also when an
  administrator made it with administrator rights and later lost them (checked: B1 below; its files are
  owned by Administrators, but the user's full control comes from the profile folder's inherited
  permissions). It is the default everywhere, and the escape hatch everywhere else: the MSIX update offers
  switching to it when an administrator is not at hand for a release's certificate (every release has its
  own; that is fixed only by signing with one stable certificate — the `SIGNING_PFX_*` secrets, the owner's
  decision), and the all-users update offers it when its approval is declined or refused.
- **An all-users install needs an administrator for every update and its uninstall** — also when the
  person who installed it later lost those rights. A standard user is told so and can install just for
  themselves next to it: the in-app update offers exactly that when the approval is declined or refused
  (*[U] Install … just for you instead*, the same data), and Start, the desktop and Apps & Features then show
  *Agentmaster* (theirs, updated without rights) beside *Agentmaster (all users)* (the shared one). No
  account without administrator rights can change, update or take over the all-users copy itself: Program
  Files and HKLM are Windows-protected; it stays for the other users until an administrator updates or
  removes it.
- **A signed-in administrator who runs things elevated** (by habit, or on a PC with UAC off) never hands
  those rights to the app it installs: the copy is started through the unelevated hop.
- **Another account at the keyboard** (an administrator typing UAC credentials for a standard user,
  remote support, a deployment agent as SYSTEM) never installs for the wrong person: the installer and
  the first launch install for the signed-in user, or say how.
- **The profile mutex** (`Local\Agentmaster.profile.<hash>`) treats `ERROR_ACCESS_DENIED` as "held
  elsewhere": an elevated instance's mutex is not openable by an unelevated one, which used to read as
  "free".

**The real app (the same evening).** A Release build of these sources, made into the portable zip by
`New-UnpackagedTerminalDistribution.ps1`, in a CLEAN Windows 11 25H2 test desktop (4 GB) with UAC at
Windows' own defaults: administrators get the consent prompt and standard users the credential prompt,
both on the secure desktop. Nothing inside the guest can reach that desktop, so the prompts were seen and
answered through QEMU's monitor (`tools\install-qa\qm.py`; the kit's README has the procedure).

| Who / what | Result |
|---|---|
| a fresh unzip from Explorer, *Install just for you* (Enter: the default) | the copy in `%USERPROFILE%\.agentmaster\bin` with Apps & Features, PATH, verbs and shortcuts; the installed app starts unelevated on the Default profile |
| the installed copy's ⚙ → About → *Uninstall Agentmaster…* | the confirm names what goes; everything removed but the data; the app closes |
| *Install for all users* from an unelevated copy, UAC *No* | "cancelled at the UAC prompt": back to the question, nothing installed |
| the same, UAC *Yes* | Program Files, HKLM, machine PATH, verbs, common shortcuts; only the script ran elevated; the copy runs unelevated |
| the all-users copy's ⚙ Uninstall, UAC *No* / *Yes* | nothing removed, the app stays open / everything removed, every user's data kept |
| a v0.8.1 unzip over a per-user v0.8.0; a v0.8.0 unzip over v0.8.1 | *Update the installed Agentmaster (recommended)*, a user's own file beside it kept / *Use the installed Agentmaster*, never a downgrade |
| ⚙ → *Check for updates* on a copy stamped 0.7.9, pre-releases on | the real v0.7.10-prerelease offered → *Update now* → its zip swapped in (files only the newer build had removed), the registration re-stamped, relaunched in ~12 s |
| the signed-in administrator, ELEVATED | the *running with administrator rights* note; the installed copy runs unelevated |
| another administrator, elevated (its own consent) in the user's session | *Set it up for <user>* recommended → the copy restarts as the user, who gets their own question |
| a standard other account in the user's session | the explanation and its own options; nothing written |
| a STANDARD signed-in user: *Install just for you* / *Install for all users* with an administrator's password | no prompt / the all-users install, the copy running as the user |
| a standard signed-in user: *Run as administrator* with an administrator's password | the app runs as that administrator → *Set it up for <user>* → it restarts as the signed-in user |
| a standard user under "Automatically deny elevation requests": ⚙ Uninstall of the all-users copy / its update | "Agentmaster was not uninstalled … Windows does not let this account run programs as administrator (a setting of this PC)" / "The update of Agentmaster for all users could not be started …"; back to the question |
| a browser-marked zip extracted by Explorer | SmartScreen's "Windows protected your PC" → *More info* → *Run anyway*; the installed copy asks nothing more |

**Fixed during that QA** (re-checked in a reset desktop with a rebuilt zip):

- **The foreground.** After *Install just for you* the installed app opened BEHIND the window the user had
  before, only its taskbar button flashing (three times out of three); the question restarted by the
  hand-over, and the one started by SmartScreen's *Run anyway*, came up inactive (Enter went to the window
  behind). `LaunchApp` now hands the started copy the foreground (`AllowSetForegroundWindow`), the
  hand-over grants it before its task runs (`ASFW_ANY`), and the question forces itself forward when
  Windows refuses (`ForceForeground`). Re-checked: all three in front.
- **The download mark.** The installed files kept the browser's mark (60 of 63; SmartScreen clears only the
  exe the user ran), so a helper started through the shell (`elevate-shim.exe`) could meet its own
  SmartScreen prompt. The C++ install and the scripts' `Install-AmLayout` now remove it from what they
  install, and *Install here* from the shipped entries; a user's own file keeps it. Re-checked: 0 of 63.
- **Wording.** A refused all-users *update* read "install"; the packaging script used whichever `tar.exe`
  came first on PATH (Git's cannot read an `.msix`, so no zip was made).

**Observed, outside the code:** the consent prompt for the all-users steps names *Windows PowerShell*
(verified publisher: Microsoft Windows), because the install script runs there; *Run as administrator*
on the unsigned exe shows "unknown publisher"; SmartScreen asks once per download; Smart App Control was in
evaluation mode on this image, and on a PC where it is ON an unsigned exe does not start at all. Only the
last three change with a code-signing certificate from a trusted authority (the MSIX's self-signed one
does not).

**The paths that were still open (later that night).** The user's questions: can a user who installed as
an administrator and later lost those rights always update; how do an all-users and a per-user install
live side by side; can a user without rights take the all-users install over. A rebuilt Release zip of the
evening's fixes (the all-users names, the restorable swap, the updater's fallback and restart, the old
script's copy), the same kind of test desktop with UAC at Windows' defaults, and an MSIX of the same sources
signed with a throwaway certificate the desktop trusts (`tools\install-qa\mkmsix.ps1`). Each run used a
fresh copy stamped OLDER than the published v0.7.10-prerelease (`New-AmQaCopy`), so the app's own update
found that release on GitHub and installed it.

| Who / what | Result |
|---|---|
| A1: the copy `Install-Agentmaster.ps1` v0.7.10 makes (`%LOCALAPPDATA%\Programs\Agentmaster`, *Agentmaster (Portable)* in Start, no Apps & Features entry), then a newer zip from Explorer | *Update the installed Agentmaster … the copy the install script made* recommended → updated and registered in that folder (Apps & Features, PATH, verbs, *Agentmaster* in Start), *Agentmaster (Portable)* removed; the app in front |
| A2: *Install for all users*, UAC *Yes* | the Apps & Features key `AgentmasterAllUsers`, named *Agentmaster (all users)*, and so are its shortcuts |
| A3: two users on that copy (a standard user's Agentmaster running from Program Files, started with `Start-Process -Credential`): the administrator's in-app update, UAC *Yes* | the swap refused while the other user runs the copy → *[A] Ask for administrator approval again (for example once the other users have closed Agentmaster)*, [U], [C] → [C]: the installed version starts again and offers no update |
| the same, UAC *No* | the choice without that hint → Enter ([U], the default): the release installed just for this user on the same profile, started in front; Start search and Settings → Installed apps list *Agentmaster* and *Agentmaster (all users)*; the other user's instance untouched |
| A4: the MSIX package of these sources, ⚙ → *Check for updates* → *Update now* | the certificate question [A] / [U] / [C] → [C]: the package starts again with no update prompt (hooks.log: "skipped: the update script started this version again after an update to v0.7.10-prerelease that did not complete") → [U]: the release installed just for the user on the profile the package used, the package removed, the app unelevated |
| A5: an old copy started ELEVATED, *Install just for you* | the *running with administrator rights* note; installed (its files owned by Administrators, the user's full control inherited) and started as the signed-in user, unelevated |
| B1: that user removed from Administrators and signed in again (a standard user, medium integrity), ⚙ → *Check for updates* → *Update now* | no prompt of any kind; v0.7.0 → v0.7.10 in ~12 s, Apps & Features re-stamped, no set-aside folder left; the updated app in front, unelevated |

**Fixed during that pass** (each re-checked with a rebuilt zip):

- **No restart after a cancel while another user ran the all-users copy.** `Restart-PreviousApp` skips the
  start when Agentmaster already runs from the folder, and an UNELEVATED process query also sees other
  users' processes, path and owner both (measured: a limited-rights query by the administrator listed the
  standard user's `C:\Program Files\Agentmaster\Agentmaster.exe` with that user's SID). The check now counts
  only this account's processes (`Test-AmAppRunningUnder`, by owner SID).
- **Settings → Installed apps listed one Agentmaster.** With both installs, the shared key name `Agentmaster`
  was listed once ("1 app found"), so the per-user copy could not be uninstalled there; the all-users key is
  `AgentmasterAllUsers` now (§2b).
- **The same update offered again at once.** The version started again after a cancel ran its startup check
  and offered the same release right away; the restart marker makes that run behave as *Remind me next
  restart* (§2b).

Found beside it, on the development PC: the generated `.cmd` files' missing `chcp 65001` (§2b; a UTF-8 path
reached PowerShell garbled on a CP 437 console). Observed and not investigated: twice in A4, a first click on
⚙ or *Check for updates* did nothing and a second one worked (not seen in A5 or B1).

**Automated coverage:** the m5 suite (`TestPortableInstall`, `TestProfileBootstrap`: the decision and
pointer files, the startup plan, the account plan, the prompt default, the manifest and legacy names on a
real scratch tree, the download mark removed from copied and in-place files but not from a user's own,
the machine-copy rules with a real deny ACL, the uninstall target, the start-failure text, the mutex
verdict) and `tools\tests\Test-AgentmasterInstaller.ps1` (plain PowerShell, 5.1 and 7: the shared core's
identity across both scripts, the manifest, the version text, the decision and pointer files, the write
probe with a real deny ACL, per-user and all-users installs and uninstalls into a scratch registry key and
scratch folders, the download mark on an installed layout, the Win32 error reading, the identity, a phase
script that runs and reports back; `-IncludeScheduledTask` adds a real one-shot task — run it on a test
desktop).

**Not covered:** *[A]* in either question (the all-users update's after a decline or a refused swap, and
the package update's certificate question); a swap that fails half way in a real desktop (the put-back is
covered by the automated tests, where a locked file makes the move or the copy fail); an account whose name
is not ASCII (the `chcp` fix was measured on the development PC only). The package update and its switch to
the per-user install, and whether the app the in-app update starts comes to the front (after B1's update
and A3's [U] it did), were checked in the pass above. Known limits: script-made shortcuts carry no AUMID (the script cannot
compute the app's per-path formula; the app's own install stamps it); a shared unzip used by several
accounts is a stub for all of them once one decides (the decision is per folder); HKLM detection reads the
64-bit view only (`Uninstall\AgentmasterAllUsers`, then `Uninstall\Agentmaster`).

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

Agent Manager (the tab strip's Home button) → **⚙ Settings → PROFILE**: shows the active folder (and a pending `current → new
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
