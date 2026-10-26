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
then the copy asks whether its own folder may go (*The downloaded copy*, below), the INSTALLED copy is launched and the
installing process exits (an in-place install carries on).
*For all users* — written by the shared PowerShell core (`Install-AmMachineCopy`): `HKLM\Software\Classes`
verbs, `HKLM\...\Uninstall\AgentmasterAllUsers`, the machine PATH, and — instead of shortcuts in the common
Start menu and on the Public desktop — an **Active Setup component** that gives every user icons of their
OWN (since 2026-10-22; the next paragraph); `install.path` = `machine` and NO `profile.path` (each user
resolves their own profile). It refuses a release whose zip has no `.am-manifest` (built before
2026-10-21): that app would not know what `install.path = machine` means. Its Apps & Features entry is
named **Agentmaster (all users)**, a per-user install's **Agentmaster** (`Get-AmRegistrationName`), and
has its own key NAME (`AgentmasterAllUsers`, `kMachineUninstallKey`; both since the evening of
2026-10-21). Before, both entries were `...\Uninstall\Agentmaster`: Windows lists an Uninstall key name
ONCE, so Settings → Installed apps showed only the all-users entry and the per-user copy could not be
uninstalled there (measured in the test desktop: "1 app found"; with the HKLM key renamed, both listed).
An all-users copy registered under the old names gets the new ones at its next update (the old shared
shortcuts and the old-name entry are removed only when they point into the folder; detection reads the
new key, then the old).

**A user's own install wins for that user** (2026-10-22; the user's request "make our install be able to
deal with these cases": a user who installed a copy of their own beside an older all-users one — the
all-users update's *[U] just for you*, for one — kept landing in the shared copy, and the two then ran on
one profile). What a user WITHOUT administrator rights can change was measured first, as a standard user
in the test desktop: the Public desktop, the common Start menu, Program Files, the HKLM verbs and the
machine PATH refuse every change; their own desktop, Start-menu folder, HKCU and taskbar pins are theirs;
an HKCU verb REPLACES the HKLM one of the same name (one menu entry); a same-named icon of their own does
NOT hide a shared one (the desktop showed two, Start listed "Apps 2": it keys its entries by target); the
machine PATH comes before the user PATH; Windows refuses an app that asks to change a Start pin
(`E_ACCESSDENIED`); and a taskbar pin re-pointed in place is followed (the window groups under it). So:

- **No shared icons.** The all-users install registers the Active Setup component
  `HKLM\SOFTWARE\Microsoft\Active Setup\Installed Components\{F8D18BA4-7DD1-4F04-8EE0-C9C7342F3264}`
  (`kActiveSetupKey`, `$script:AmActiveSetupId`): StubPath `"<exe>" --register-user`, Version
  `1,<year>,<MMdd>,<HHmm>`, IsInstalled 1, DontAsk 2. Windows runs the StubPath once for each user whose
  own Version (HKCU, the same subkey) is lower, at their next sign-in: `Agentmaster.exe --register-user`
  (handled in the prelude before the single-instance handoff, `HandleRegisterUserFlagAtStartup` →
  `EnsureMachineCopyUserSetup`; it never starts the app) makes the user's own *Agentmaster* desktop and
  Start-menu icons, stamped with the AUMID the shared copy runs under for them — only where no
  *Agentmaster* icon exists yet, and none for a user with an install of their own — and writes their HKCU
  Version, as Windows itself does. The shared copy's own start runs the same step (`MachineCopyUserUpkeep`),
  and the installer runs it for the signed-in user at once (`Invoke-AmUserIconStep`). An icon a user
  deletes stays deleted: an update keeps the component's Version, and only a fresh install writes a newer
  one (so after an all-users uninstall, a reinstall gives every user icons again).
- **The user's own install takes over.** Its icons have the same file name (`Agentmaster.lnk`), so it
  replaces the shared copy's per-user ones — made ANEW (a WScript.Shell shortcut saved over one that exists
  keeps the old one's AppUserModelID) — and at every start it re-points the user's taskbar pins of the
  shared copy at itself IN PLACE (`RepointSharedCopyTaskbarPins` → `RepointPinsIn`; a pin deleted and made
  again can drop out of the taskbar), with its own AUMID, so its window groups under the pin. Its HKCU verbs
  already replace the HKLM ones.
- **The shared copy hands every launch to it** (`InstallHandoff.h`): the app
  (`StartupPlan::HandoffToUserCopy`, checked FIRST in `DecideStartup` — a launch from an Agentmaster tab
  carries `AGENTMASTER_PROFILE`, and the machine PATH finds the shared copy first in that tab too) and
  `agentmaster-cli` (`ForwardCliToUserInstall`: the user's copy's CLI on the same console with the same
  standard handles, its exit code passed on). That covers what the user cannot redirect: the machine PATH, a
  Start pin, an old taskbar pin, a double-click in Program Files. ALWAYS the user's own copy, also when an
  administrator later updates the shared one past it (the user's decision of 2026-10-22: their copy offers
  the same update itself, without rights). Loops are impossible by construction: the target must hold the
  exe, must not be the running folder, and must not be a shared copy itself. A launch the process must
  serve itself (`-Embedding`, session 0) is never handed off. The app hand-off keeps the token, like a
  launcher stub's (an elevated start stays elevated, as a start of the user's own copy would; dropping
  rights would cost a scheduled task at every start on a PC with UAC off). It runs AFTER the
  single-instance hand-off: a shared instance that is already running (started before the user's own
  install existed) keeps taking the shared copy's launches until it exits, so the hand-off never puts a
  second instance on the same profile. `AGENTMASTER_NO_HANDOFF=1` makes the shared copy run itself (an
  administrator testing it).
- **Uninstalls give back.** Removing the user's own copy while the all-users one stays hands its icons and
  pins to the shared copy (`Move-AmUserEntriesToMachineCopy`: the icons it had made anew, the pins
  re-pointed in place, the component marked done for the user) instead of leaving the user without one. A
  script cannot compute the app's per-path AppUserModelID, and without it the Start icon routes none of the
  shared copy's toasts, and the pin, which still names the removed copy, opens the shared copy's first
  window as a second taskbar button (the app defers stamping such a pin until it exits, `_setupAumid`,
  GH#20053). So once the user's copy is unregistered, the uninstall runs the shared copy's own
  `--register-user` (`Invoke-AmMachineCopyUserCommand`, only when the Active Setup component names that very
  exe — an older copy would start the app instead); that process opens no window and stamps the id on the
  user's *Agentmaster* icons and pins of it that lack it (`StampMachineCopyUserLinks`, every run; a link
  that already carries the id is never rewritten). The all-users uninstall removes the Active Setup
  component (only when its command starts a program in its folder) and sweeps the icons it gave every user
  (`Get-AmUserShellFolders`: every real profile's desktop and Start-menu folders, a OneDrive desktop, a
  signed-in user's redirected folders; only icons that point into its folder — a user's own install's icon
  of the same name stays). Taskbar pins are left, as Windows leaves the pin of any program that was
  uninstalled (it then offers to delete it on a click).
- **A user's own copy deleted by hand.** Its HKCU entry then names a folder that is GONE (`InstallFolderIsGone`:
  the drive answers and the folder holds no app exe — a folder on a drive that is not there now, an
  unplugged stick, is kept). The shared copy's start removes its leftovers that point into that folder —
  the verbs (they would keep replacing the shared copy's and open nothing), the Apps & Features entry, the
  PATH entry, the icons — moves its taskbar pins back, and makes the user's icons again
  (`CleanStaleUserInstall`, then `EnsureMachineCopyUserSetup` forced).

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

**No install ever loses the user's state (2026-10-26).** The owner's report: an all-users install from the 0.7.0 era (the
release package, which needs an administrator once for its certificate) had been used, then a newer zip's *Install just
for you* put the install into `%USERPROFILE%\.agentmaster\bin` — inside the profile that held the sessions and titles —
and the app came up "as if the `.agentmaster` folder was reset or deleted and recreated". Their rule: never delete any
`.agentmaster` folder; rename what is in the way, in every case. Four install-lane scenarios on v0.8.0 (IS-100–IS-103:
the all-users copy closed, the all-users copy RUNNING on the profile, the 0.7.0 package taken over with its box's default,
and that package's workspace of titled tabs) kept every file of the profile and reopened the workspace, and no code path
was found that deletes a profile; the old `Install-Agentmaster.ps1` (v0.7.10 and older) did have one — its portable
install deleted everything in its install folder except five names, and its `-Uninstall -Portable` the whole folder —
which would wipe a profile only when pointed at one. So the rule is now enforced in every install path, whatever the
cause was:

- **Nothing that holds Agentmaster state is deleted or moved.** `Profiles::PathHoldsProfileNow` (ProfileBootstrap.h) and
  the core's `Test-AmProfileHolder` name it: a known profile folder (the per-identity defaults, every folder the home map
  names, `AGENTMASTER_PROFILE`), a folder ABOVE one, any folder named `.agentmaster*` (known or not), and a folder with a
  `.agentmaster*` folder directly inside. Every recursive delete of an installer goes through one door that refuses
  those and anything outside the folder the step works in — `detail::RemoveTreeGuarded` (PortableInstall.h: the
  set-aside clean-ups, the restore of a failed copy) and `Remove-AmTreeGuarded` (the core: `Remove-AmEntries`, the
  set-asides, the uninstall, every temp folder); a refusal is logged and leaves the item as it was. An uninstall that
  empties a `.agentmaster*` folder keeps the empty folder.
- **A destination folder in the way is renamed aside and kept, never mixed into.** An install into a folder that holds
  no Agentmaster but files of someone else's — anything beyond an install's leftovers (`profile.path`, `install.path`,
  `settings\`, `profile\`, the markers, a set-aside folder: what an uninstall leaves) — renames that whole folder to
  `<name>.before-install-<yyyyMMdd-HHmmss>` in the same parent (one rename: nothing copied, overwritten or deleted) and
  lays the install into a fresh folder; the install's notes and `[install]` line name where it went
  (`detail::SetAsideFolderInTheWay`, the core's `Move-AmFolderInTheWay`). A folder that holds a profile is refused
  instead: renaming it would point its install at an empty profile — which would look exactly like the reported reset.
- **The copy no longer carries `settings\`** (a pre-profile portable's Terminal settings): copied, it overwrote the
  destination's own preserved `settings\`, which the core never did. Its files go to `<profile>\terminal\`, copy-if-absent.

**The downloaded copy (2026-10-26).** A zip must be unpacked somewhere to run; once that copy installed Agentmaster
elsewhere (*Install just for you*, a picked folder, for all users) or handed over to an install that was already there
(*Use the installed …*), it only forwards launches. The owner's request: the user was never asked whether to delete it.
So the copy now asks **"Delete the downloaded copy?"** right after the install (before the installed copy starts):
*Delete the downloaded copy (recommended)* / *Keep it* (it stays a launcher stub; Cancel / Esc keep it too). It is offered
only for a folder that holds nothing but the download's files (`JudgeLeftoverCopy`: the manifest's names, else the legacy
list; `install.path` / `profile.path`; `profile\` and `settings\` only when EMPTY — a "later" run's profile with data stays
the user's), that this account can write, that holds no Agentmaster state, and that is no well-known folder (the user
folder, Desktop, Documents, Downloads, Program Files, Windows, a drive root). The removal starts only once the installed
copy started (a failed start goes back to the question, which needs the folder): a hidden Windows PowerShell, outside
this process's job, waits for the copy's process to end, then for anything else running from the folder (a minute at
most), proves nothing holds it (a rename there and back), and moves it to the **Recycle Bin** — recoverable, never a plain
delete — then removes the extraction folder around it (`Agentmaster_<version>_<arch>` from Explorer's *Extract All*) when
that is left EMPTY and is named `Agentmaster*`. `[install] leftover copy: …` lines record the verdict, the answer and the
scheduling.

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
folder, the PATH entry and the shipped files; every profile folder is kept. The all-users one also
removes its Active Setup component and the icons it gave every user; a per-user one, beside an all-users
install, first hands its icons and taskbar pins to that copy (*A user's own install wins*, above). The
confirm says which. The whole flow logs
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

**The installer's edge cases (2026-10-22; checked in a test desktop, §2c).**

- **Nothing stays in the caller's PowerShell.** `irm … | iex` ran the text IN the caller's scope: its
  `$ErrorActionPreference = 'Stop'` and every function stayed in the user's window afterwards, and that
  preference turned every later error there into a stop. A script block (`& ([scriptblock]::Create(…))`)
  has a scope of its own but no script scope, so the core's `$script:` lists landed in the caller's (the
  global one at the prompt). Every run that is not a file run nor dot-sourced — told by `$PSCmdlet` and its
  text, since under iex `$MyInvocation` is the CALLER's — now starts the whole text again inside a throwaway
  dynamic module with the parameters it was given, and ends. A failed run inside it also no longer ends a
  calling script with `exit 1` (only a file run exits).
- **The release a version names.** This repository publishes with a suffix, so `-Version 0.7.10` (or
  `0.7.10.0`, the version the app shows) installs `v0.7.10-prerelease` and says so; a version published
  only as a nightly says to add `-Nightly`. A token GitHub refuses (401 — a stale `$env:GITHUB_TOKEN`) is
  dropped and the request made again without it; an exhausted rate limit (60 an hour per network address)
  says until when and how to pass a token; any other failure is shown as it is, never as "not found".
- **`-ZipPath`** installs a zip on disk (User or Machine): its version from its name, else from the stamp
  of its layout; a zip for another processor is refused (an x64 zip on an ARM64 PC only warns).
- **`-InstallDir`** is made absolute (the elevated step and a run for the signed-in user are other
  processes: a relative `.\am` would have landed in `C:\Windows\System32` there), a folder that holds other
  files gets an `Agentmaster` folder inside it (the app's own rule for a picked folder), and a second copy
  of the same kind elsewhere, or one kind laid over the other's folder, is refused with how to move it.
- **The default** upgrades what is installed only when this account can: a standard user's approval for
  the all-users copy or the package takes an administrator's password, so their default stays *Just for
  you*, which then takes over for them.
- **Up to date costs nothing**: the one rule `Test-AmUpToDate` (the core's installs use it too) is asked
  before anything is downloaded or any approval asked; an all-users or package upgrade that would stop at
  another user's visible instance stops before the download too.
- **A running Agentmaster** closes for its upgrade or uninstall: an interactive run asks first ("Go on?",
  not under `-Force`), and the one that ran is started again afterwards — through the user's own Explorer
  when the run is that user, so it gets the shell's environment (the PATH the install just changed, not a
  PowerShell 7 module path). The PACKAGE's app is closed by the installer itself, only the user's own
  instances, right before the package is added or removed: `Add-AppxPackage -ForceApplicationShutdown`
  and `-ForceTargetApplicationShutdown` do NOT close it (0x80073D02, "the following apps need to be
  closed", measured).
- **Typed into a tab of the Agentmaster it closes**, the run would close with it half way. It now goes on in
  a NEW PowerShell window of its own: its text written to the download folder, the settled choices on the
  command line (the scope, the resolved tag or the zip, absolute folders — never a token, which travels in
  the environment and is removed there at once), a record of the window in the download folder, and Enter
  to close it. Told by `Test-AmHostedIn`: the console's HOST (`NtQueryInformationProcess`
  `ProcessConsoleHostProcess`, as the app's orphan reaper reads it — not the console window's owner, which
  Windows gives to the first CLIENT, the tab's shell) or a parent process from the folder. An uninstall
  removes what does not close it first and leaves the rest to the new window. `Stop-ProcessesUnder` itself
  refuses when its own console's host is in the folder it closes.
- **An older release for all users** (one without `--register-user`, probed in its exe) gets the shared
  *Agentmaster (all users)* shortcuts and no Active Setup step; a downgrade to one removes the step and the
  icons it gave. **A run stopped half way** (Ctrl+C) puts the previous version back (PowerShell runs only
  the `finally` then, never the `catch`). **An administrator's password** typed for a standard user runs
  the elevated step as that administrator: the user's own Agentmaster is closed as theirs (`-OwnSids`), not
  refused as another user's.

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
  (*[U] Install … just for you instead*, the same data). Since 2026-10-22 their own copy then wins for them
  everywhere (§2b, *A user's own install wins*): their *Agentmaster* icons and taskbar pins open it, and a
  start of the shared copy (the machine PATH, a Start pin, Program Files) is handed to it; Apps & Features
  lists *Agentmaster* (theirs, updated without rights) beside *Agentmaster (all users)*. No account without
  administrator rights can change, update or take over the all-users copy itself: Program Files and HKLM
  are Windows-protected; it stays for the other users until an administrator updates or removes it.
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

**A user's own install wins (2026-10-22).** The checks of §2b's *A user's own install wins*: a Release
build of these sources made into the portable zip, a CLEAN Windows 11 25H2 test desktop with UAC off (the
signed-in administrator's processes are elevated, which the hand-off must not slow down), the all-users
install made through the app's own question, and a second, standard account. The kit's `Get-AmQaState`
now also shows the Active Setup component, each account's marker, the taskbar pins and every link's
AppUserModelID.

| What | Result |
|---|---|
| *Install for all users* from a fresh copy | no shortcut in the common Start menu or on the Public desktop; the Active Setup component (StubPath, a Version, IsInstalled, DontAsk); the installed copy's first start made the user's own desktop and Start icons, stamped with its AUMID, and wrote their marker |
| the marker and both icons deleted, then a restart | 17 s after the boot, Windows' Active Setup ran the stub at sign-in: both icons back with the AUMID, the marker written, no app started |
| the desktop icon deleted, the shared copy started from Start | the icon stays deleted |
| the shared copy pinned through its taskbar button; then a copy of the user's own beside it (*Install just for you*, recommended; the question's note says what a copy of one's own takes over) | the desktop and Start icons replaced by the user's copy's (its AUMID); the pin re-pointed IN PLACE with that AUMID ("moved 1 taskbar pin(s)"); one taskbar button, "1 running window pinned" |
| `"C:\Program Files\Agentmaster\Agentmaster.exe" -d C:\Windows` | handed off: the user's copy started with `-d C:\Windows` (its tab in `C:\Windows`, in front), the shared process gone; logged |
| the same while the user's copy runs | one process, the user's copy, with a new window |
| `AGENTMASTER_NO_HANDOFF=1` | the shared copy runs itself |
| the shared copy's `agentmaster-cli.exe`, with the user's CLI swapped for a renamed `cmd.exe` | the user's copy ran: the arguments, a redirected `>` file and exit code 7 went through; with the opt-out the shared CLI answered itself (exit 2); the real CLI restored, `--json sessions` → 0 and the JSON |
| the user's copy uninstalled with its own *Uninstall* (the confirm names the hand-back) | its files, entry, verbs and PATH gone; its icons and the pin open the shared copy, and the shared copy's `--register-user` stamped its id on all three ("its id stamped on 3 of this user's icons and taskbar pins") |
| `--register-user` as the second account | that account's own icons; no `.agentmaster` folder made for it |
| the user's copy installed again, its folder deleted by hand, the shared copy started | "gone (deleted by hand): 8 leftover entries removed, or moved to the all-users copy" (3 verbs, the entry, the PATH entry, 2 icons, the pin); the icons made again for the shared copy |
| the all-users uninstall (with an older and with the final build) | files, the component, the entry, verbs and machine PATH gone; every user's icons of it swept, the second account's included; after a reinstall the second account got icons again (a newer Version) |

**Fixed during that QA** (each re-checked with a rebuilt zip):

- **The all-users entry's version.** Settings showed `1,2026,1022,944`: the new Active Setup step in
  `Register-AmInstall` set `$version`, and PowerShell variable names ignore case, so it overwrote the
  `-Version` parameter. Renamed (`$asVersion`); the suite checks an all-users registration's DisplayVersion.
- **The handed-back links' id.** The icons a script made carried no AppUserModelID and the pin kept the
  removed copy's; the uninstall now runs the shared copy's `--register-user`, which stamps it (§2b).

Found by the suite on the way: `Get-AmRegistrationTargets` threw when the user's AppData folder does not
exist (`Join-Path` on the `''` .NET answers), and `Get-AmUserShellFolders` reached an `@(...)` caller as ONE
item, its folders joined by spaces. Found by review: the app hand-off dropped rights through the
scheduled-task hop, which would cost ~6 s at every start on a PC with UAC off; it keeps the token now, like a
launcher stub's.

**Not covered there:** a standard signed-in user with UAC on (the same HKCU entry and `install.path` decide
for every account); a desktop moved into OneDrive; a taskbar pin's grouping right after a restart of Explorer
(Explorer's UI Automation id of the pinned button kept the id it had when pinned, while the grouping followed
the re-pointed pin).

**The installer's edge cases (2026-10-22, evening).** §2b's *The installer's edge cases*, checked with the
scripts of that commit in a CLEAN Windows 11 25H2 test desktop (UAC off, the signed-in administrator
`Docker`): the app is the QA build of the afternoon (`--register-user`, a manifest) in portable zips stamped
0.8.1.0 … 0.8.5.0, plus the released v0.7.10 zip given a manifest (an older app without `--register-user`),
and the real GitHub releases. The suite ran there first (263, then 267 checks, 0 failures, the real
scheduled task included).

| What | Result |
|---|---|
| `-Scope User -ZipPath` 0.8.1.0, then the same again | installed from the zip; the second run "Already up to date", nothing installed |
| 0.8.2.0 typed into a Windows Terminal window while Agentmaster runs | "Agentmaster is running from … It closes for the upgrade and starts again …" → Enter → closed, upgraded, started again (its parent `explorer.exe`) |
| 0.8.3.0 typed into a TAB of that Agentmaster | the tab: "the step goes on in a new window" + the record's path; a new window *Agentmaster installer* (the copy's command line: the scope, the zip, absolute folders, no token) → Go on? → the old app and the tab closed, upgraded, started again; "Press Enter to close this window"; the record complete, the script copy deleted |
| 0.8.4.0 by the script-block form typed into a tab (the run in the tab's own process: the console-host path) | the same; the tab's session after it: `EAP=Continue`, no function, no `$script:` list, no parameter, no module |
| `-Uninstall -Scope User` typed into a tab | the same new window: "The uninstall closes it" → removed, `profile.path` and the data kept |
| `-Version 0.7.10` with a bogus `$env:GITHUB_TOKEN`, 0.8.4.0 installed | "GitHub refused the token … going on without one", "v0.7.10 is published as v0.7.10-prerelease", "Already up to date" — nothing downloaded |
| `-Scope Machine -ZipPath` the older app | *Agentmaster (all users)* in the common Start menu and on the Public desktop, no Active Setup step, the user's own icons untouched |
| the all-users copy upgraded to the QA build | the shared shortcuts gone, the Active Setup step (`"…\Agentmaster.exe" --register-user`) written |
| the older app again: without / with `-Force` | "Already up to date for all users" / downgraded: the step removed, the shared shortcuts back |
| a STANDARD account (`-ForCurrentAccount`, no `-Scope`, non-interactive) beside the all-users copy | "non-interactive: installing for scope User"; installed without rights; the note that its own copy takes over |
| `-InstallDir .\relforeign` (a folder of other files) with a per-user install elsewhere | "… holds other files: Agentmaster goes into …\relforeign\Agentmaster", then refused: already installed, how to move it; nothing written |
| the all-users upgrade typed into a tab of the all-users copy | the new window, Go on?, closed, upgraded, started again |
| `-Scope Package -Version 0.7.9`, then the latest (v0.8.0) typed into the v0.7.9 package app's tab | 0.7.9 resolved and added (certificate trusted); the upgrade went on in a new window, asked, closed the user's instance right before the install, 0.8.0.0 added and started again |
| `-Uninstall -Scope Package` typed into the package app's tab | the new window, Go on?, closed, removed, the data kept |
| `irm \| iex` (no arguments), the all-users copy at 0.8.2.0 | the latest (v0.8.0) resolved, the all-users copy chosen (an administrator), "Already up to date", nothing downloaded; the caller's `EAP=Continue` |

**Fixed during that QA:**

- **The console host was misread.** `GetConsoleWindow` + `GetWindowThreadProcessId` named the tab's shell
  (`powershell.exe`): Windows gives a console window to its first client. The host is now read with
  `ProcessConsoleHostProcess` (measured: a tab's shell → `…\.agentmaster\bin\OpenConsole.exe`).
- **The package's running app was not closed.** The first run asked, downloaded, and then failed with
  0x80073D02 although `Add-AppxPackage -ForceApplicationShutdown` was passed; `-ForceTargetApplicationShutdown`
  failed the same way, run by hand. The installer now closes the user's own instances itself
  (`Stop-PackageApp`), as the copy installs do; the error text no longer says `-Force` closes it.
- **The older release's text** said every user gets icons of their own; it now says its shortcuts are shared.

**Not covered:** a real Ctrl+C half way through a copy (the suite stops one in a nested PowerShell: the
previous version is back); a real exhausted rate limit; an administrator's password typed for a standard
user (`-OwnSids`; a desktop with UAC off cannot ask for it — the suite closes a process of this account
through it); the package as the WINDOWS DEFAULT TERMINAL: the app starts its update script with
`ShellExecute` and a visible console, which would then open inside the package app the update closes (not
checked; it predates this work). **Observed in the app, not investigated:** the test desktop's Agentmaster
window got narrower at each restart (1115 px → 674 → 491; the window record then held 306 × 586).

**Automated coverage:** the m5 suite (`TestPortableInstall`, `TestProfileBootstrap`: the decision and
pointer files, the startup plan, the account plan, the prompt default, the manifest and legacy names on a
real scratch tree, the download mark removed from copied and in-place files but not from a user's own,
the machine-copy rules with a real deny ACL, the uninstall target, the start-failure text, the mutex
verdict; since 2026-10-22 also the Active Setup version rules, the hand-off rule and its loop guards on real
folders, the per-user icons, the pins re-pointed in place with their id, a link's id read back, and the
leftovers of a copy deleted by hand on scratch HKCU keys) and `tools\tests\Test-AgentmasterInstaller.ps1`
(plain PowerShell, 5.1 and 7: the shared core's identity across both scripts, the manifest, the version
text, the decision and pointer files, the write probe with a real deny ACL, per-user and all-users installs
and uninstalls into a scratch registry key and scratch folders, the download mark on an installed layout,
the Win32 error reading, the identity, a phase script that runs and reports back; since 2026-10-22 the Active
Setup component — written, kept by an update, removed with its copy, never another copy's — the icons swept
from scratch user folders, the hand-back of icons and pins, and the guard of the shared copy's command; since
that evening an upgrade stopped half way (a Ctrl+C in a nested PowerShell: the previous version back), the
older-release probe and its shared shortcuts and downgrade sweep, nothing left by iex / a script block /
Invoke-Command (a child PowerShell of each edition), the release lookup against a fake GitHub API (the suffixed
tag, nightly-only, a refused token, the rate limit in both editions' error shapes), the zip version, the
`-InstallDir` rules, the scope default, the new window's command line split back by Windows'
`CommandLineToArgvW`, the continued run's environment, the hosted check, the close question, the up-to-date
rule, the console-host read, and the package app closed only for its own user;
`-IncludeScheduledTask` adds a real one-shot task — run it on a test desktop).

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
