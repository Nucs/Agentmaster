# 19 · Install, update, uninstall and profiles (INS)

> Conventions and fixtures: [README.md](README.md). Spec: PROFILES.md §2–§2c (the question, the scenario matrix and
> every QA table), CLAUDE.md *Release/dev separation + PROFILES* and *In-app auto-updater*, `Updater.h`. Needs a CLEAN
> Windows desktop per flow (`reset_desktop`), **F-ZIP** (portable zips of the build under test stamped at chosen versions
> — `tools\install-qa\amqa-guest.ps1` `New-AmQaCopy`), **F-PKG** (an MSIX of the same sources signed with a throwaway
> certificate the guest trusts — `tools\install-qa\mkmsix.ps1`), a second (standard) and a third (administrator) local
> account, and a **fake GitHub releases API** (or real releases with a copy stamped older). A UAC prompt sits on the
> secure desktop: answer it through QEMU's monitor (`tools\install-qa\qm.py`). [OS] here = `Get-AmQaState` (every
> install, Apps & Features entry, PATH entry, verb, shortcut and pin with its AUMID, profile, running process with its
> account and elevation). These flows automate the manual QA tables of PROFILES.md §2c. **Every scenario of them is
> listed at the end of this file** ("Every install scenario", IS-01–IS-99, with the machine each needs and its test);
> the suite runs them in the **install lane** ([`../E2E_TESTING.md`](../E2E_TESTING.md) §8.5), which resets the test
> desktop before every scenario and once more after the last.

**What shipped.** 0.7.7: a portable copy's first launch asks where Agentmaster should live (user folder, a picked folder,
here, keep portable, later); the binary is `Agentmaster.exe` with a `WindowsTerminal.exe` forwarder; the updater's
"Remind me next restart" / "tomorrow" (08:00) / 3 / 7 / 30 days / skip. 0.8.0: **just for you** (default, no rights),
**for all users** (one approval, that step only), **for the signed-in user** when another account or SYSTEM runs it;
rights never kept; uninstall removes only what a release shipped; an upgrade puts the old version back on failure; a
failed or cancelled update restarts the installed version; the one-command installer asks the same and takes `-Scope`;
a user's own install wins over an all-users one for that user (629a4f753, `AGENTMASTER_NO_HANDOFF=1` turns it off).
After 0.8.0 (9245a890f, the installer script on the branch; the app's embedded updater copy NOT BUILT): the installer's
edge cases.

## Lifecycle model

```text
 a portable unzip, first launch ─► "Where should Agentmaster live?" ─► just for you · all users · for <signed-in user> · a folder · here · keep portable · later
     ├─ just for you ─► %USERPROFILE%\.agentmaster\bin + icons + Start + verbs + Apps & Features + PATH ─► the installed copy starts (unelevated, in front)
     ├─ all users ─► UAC once ─► Program Files + HKLM ("Agentmaster (all users)") + Active Setup (each user's own icons at sign-in)
     ├─ keep portable ─► install.path = portable ─► the profile picker (Portable / Default / Development / Browse…)
     └─ later ─► nothing written; asks again next launch
 installed ─► (hourly + startup + cog) update check ─► a newer release ─► Update now ─► swap (old set aside; put back on failure) ─► relaunch
                                                                         └─► Postpone: next restart · tomorrow 08:00 · 3 / 7 / 30 days · skip this version
 Uninstall (Apps & Features / ⚙ About) ─► only the shipped names + registrations removed; profile, settings and your own files stay
```

---

### INS-01 · One command installs just for you

> **Since** 0.8.0 · **P0** · **Needs** a clean desktop, network (or `-ZipPath`) · **Spec** PROFILES.md §2b, §2c

**Phases**
1. **Run.** *Do:* in a plain PowerShell window, the README's one-liner (`… Install-Agentmaster.ps1))) -Version <v>`), answer
   `[1] Just for you` (Enter). *Expect:* no UAC prompt; "installed" with the folder; Agentmaster starts, unelevated, in front.
2. **Registrations.** *Check:* [OS] `%USERPROFILE%\.agentmaster\bin\Agentmaster.exe` + `WindowsTerminal.exe` +
   `agentmaster-cli.exe` + `.am-manifest`; a desktop icon and a Start menu entry stamped with the app's AUMID; "Open in
   Agentmaster" on folders / backgrounds / drives; HKCU Apps & Features `Agentmaster` (DisplayVersion = v); the folder on
   the user PATH; no file with a `Zone.Identifier` stream.
3. **First start.** *Expect:* no profile question; the Default profile `%USERPROFILE%\.agentmaster`; [LOG] `[install] …` lines.
4. **Folder verb.** *Do:* right-click a folder → Show more options → Open in Agentmaster. *Expect:* a tab in that folder (the
   running app gets it: one process).

**Edge cases**
- **E1 `-Scope User -NonInteractive`** → no question.
- **E2 Run again, same version** → "Already up to date", nothing downloaded.
- **E3 Typed into a TAB of the Agentmaster it upgrades** → "the step goes on in a new window"; the new window asks, closes the
  app, upgrades, starts it again (9245a890f).
- **E4 `irm … | iex`** → the caller's PowerShell keeps its `$ErrorActionPreference`, no leftover function or variable
  (9245a890f).
- **E5 Ctrl+C half way** → the previous version back (9245a890f).
- **E6 `-Version 0.7.10`** → finds `v0.7.10-prerelease` (9245a890f).

---

### INS-02 · A portable copy's first launch

> **Since** 0.7.7 (all choices 0.8.0) · **P0** · **Needs** F-ZIP · **Spec** PROFILES.md §2a, §2b

**Phases**
1. **The question.** *Do:* unzip; double-click `Agentmaster.exe` (from Explorer). *Expect:* "Where should Agentmaster live?"
   with *Install just for you (recommended)* pre-selected, *Install for all users of this PC*, *Install to a folder you
   pick…*, *Install here*, *Keep it portable, run from here*, *Ask me later*; it is in front.
2. **Ask me later.** *Expect:* the app runs on `<unzip>\profile`; [OS] nothing written (no `install.path`, no
   `profile.path`); the next launch asks again.
3. **Keep it portable.** *Expect:* `install.path` = `portable`; then the profile picker (*Portable profile
   (self-contained)* defaulted, Default, Development, Browse…, the "existing data" notes); the choice stored in
   `profile.path`; never asked again.
4. **Install just for you.** *Expect:* as INS-01's registrations; the unzip becomes a launcher stub of the install (its
   `install.path` names the install folder, so a later double-click of it starts the installed copy — measured by the
   install lane on 2026-10-24; this line said "untouched" before); the installed copy started, the unzip's process gone.

**Edge cases**
- **E1 A newer zip over a per-user install** → *Update the installed Agentmaster (vX → vY)* recommended; the user's own file in
  the install folder kept.
- **E2 An older zip** → *Use the installed Agentmaster (vZ)*; never a downgrade; the unzip becomes a stub forwarding every
  launch.
- **E3 Install to a picked non-empty folder** → into `<folder>\Agentmaster`; inside the unzip → refused; Program Files without
  rights → refused, pointing at *for all users*.
- **E4 Started ELEVATED** (Run as administrator) → the note *running with administrator rights*; the installed copy runs
  unelevated ([OS] `elevated=0`).
- **E5 A browser-marked zip** → SmartScreen once; the installed copy asks nothing more.
- **E6 `-Embedding` / session 0 / `AGENTMASTER_PROFILE` set** → no question.

---

### INS-03 · For all users

> **Since** 0.8.0 · **P1** · **Needs** F-ZIP, a standard second account, `qm.py` for UAC · **Spec** PROFILES.md §2b, §2c

**Phases**
1. **Install.** *Do:* *Install for all users of this PC* → UAC Yes. *Expect:* only the script elevated; [OS] `C:\Program
   Files\Agentmaster`, HKLM `Uninstall\AgentmasterAllUsers` named *Agentmaster (all users)*, machine PATH, verbs, the Active
   Setup component; the user's own icons made at the copy's first start; the copy runs unelevated.
2. **A second user.** *Do:* sign in as the standard user. *Expect:* Active Setup makes their own icons at sign-in; starting it
   asks nothing; their profile `C:\Users\<them>\.agentmaster`; the Program Files folder untouched.
3. **UAC No.** *Do:* the same with UAC No. *Expect:* "cancelled at the UAC prompt", back to the question, nothing installed.
4. **Policy refuses elevation.** *Do:* as the standard user under "Automatically deny elevation requests": update or uninstall
   the all-users copy. *Expect:* "Windows does not let this account run programs as administrator (a setting of this PC)" and
   what to do; nothing changed.

---

### INS-04 · Run by another account: set it up for the signed-in user

> **Since** 0.8.0 · **P1** · **Needs** three accounts · **Spec** PROFILES.md §2b (Who runs it), §2c

**Phases**
1. **Another administrator, elevated** (UAC credentials in the signed-in user's session) → *Set it up for <user>* recommended →
   the copy restarts as the signed-in user, who gets their own question; nothing installed for the administrator.
2. **A standard other account** → the explanation and its own options; nothing written.
3. **SYSTEM** (`Install-Agentmaster.ps1 -Scope User -Launch` from a scheduled task as SYSTEM) → installed for the signed-in user;
   the app runs as them.
4. **A standard other account with the script** → refused before downloading; `-ForCurrentAccount` → installed for itself.

---

### INS-05 · A user's own install wins over the all-users one

> **Since** 0.8.0 (629a4f753) · **P1** · **Needs** INS-03's state · **Spec** PROFILES.md §2b, §2c (2026-10-22)

**Phases**
1. **Pin the shared copy, then install your own.** *Expect:* the desktop and Start icons now open the user's copy (its AUMID);
   the taskbar pin re-pointed in place; one taskbar button.
2. **Start the shared copy by path** (`"C:\Program Files\Agentmaster\Agentmaster.exe" -d C:\Windows`) → handed off: the user's
   copy opens a tab in `C:\Windows`, the shared process gone; logged.
3. **`AGENTMASTER_NO_HANDOFF=1`** → the shared copy runs itself.
4. **The shared CLI** → runs the user's CLI with the arguments, a redirected file and the exit code passed through.
5. **Uninstall your own** → its icons and the pin open the shared copy again, stamped with its id.

---

### INS-06 · The in-app update

> **Since** 0.7.7 (radios) / 0.8.0 (fallbacks) · **P0** · **Needs** F-ZIP stamped older, a fake releases API (or the real
> one), F-PKG for the package path · **Spec** CLAUDE.md *In-app auto-updater*; PROFILES.md §2c (A3, A4, B1)

**Phases**
1. **The prompt.** *Do:* start an installed copy stamped older than the newest release. *Expect:* before "Reopen your N
   windows?", a dialog with **Update now** / **Postpone** and six choices: *Remind me next restart* (default), *Remind me
   tomorrow (<day>, 8:00 AM)*, 3 / 7 / 30 days, *Skip this version*. *Check:* [LOG] `[update] check (startup) … available`.
2. **Postpone, next restart.** *Expect:* no prompt for the rest of this run (the hourly check silent: `check (periodic)
   skipped: declined this run`); the cog's Check for updates still offers it; a restart asks again.
3. **Tomorrow.** *Expect:* `updatePostponedUntilUnixMs` = the next local 08:00; silent until then.
4. **Skip.** *Expect:* this version never offered again; a newer one is.
5. **Update now (per-user / portable).** *Expect:* the app closes; the zip swapped in (old names set aside, then removed);
   `settings\`, `profile\`, `profile.path`, `install.path` kept; `.am-version` restamped; relaunched in ~12 s; Apps &
   Features re-stamped.
6. **A failed update.** *Do:* make the swap fail (a locked file). *Expect:* the old files put back; the installed version
   started again; that run behaves as *Remind me next restart* (no immediate re-offer).

**Edge cases**
- **E1 Pre-release switch** → betas offered; a nightly only with the nightly switch (accepted through its warning).
- **E2 The all-users copy, UAC declined** → *[A] ask again / [U] install just for you (default) / [C] cancel*; [U] installs per
  user on the same profile.
- **E3 The MSIX package** → the certificate question [A] / [U] / [C]; [U] installs per user on the package's profile and
  removes the package.
- **E4 Offline** → the check fails quietly within 6 s; the app starts normally.
- **E5 An installer URL outside `github.com/<repo>/releases/download/`** → refused.
- **E6 `AGENTMASTER_E2E=1`** → no updater at all (the test machine's rule; PROPOSED in E2E_TESTING.md §6.1, not built).

---

### INS-07 · Uninstall removes only what a release shipped

> **Since** 0.8.0 · **P0** · **Needs** INS-01 or INS-03 state, a user file in the install folder · **Spec** PROFILES.md §2b (Uninstall)

**Phases**
1. **Per-user.** *Do:* drop `mine.txt` into the install folder; Apps & Features → Uninstall (or ⚙ → About → Uninstall
   Agentmaster…). *Expect:* the confirm names what goes; the app closes; [OS] the shipped names, shortcuts, verbs, the entry and
   the PATH entry gone; `mine.txt`, `profile\`, `settings\`, `profile.path` and `%USERPROFILE%\.agentmaster` kept.
2. **All users.** *Expect:* elevated once; every user's icons swept; every user's data kept.
3. **Not an install** (a plain portable, a stub, the package, a build folder) → an information dialog; nothing removed.

**Edge cases**
- **E1 A pre-manifest install (v0.7.10)** → removed through the legacy list; the user's files kept.
- **E2 `Install-Agentmaster.ps1 -Uninstall -Scope User`** → the same result; `profile\` kept.

---

### INS-08 · The package, and taking it over

> **Since** 0.7.x / 0.8.0 · **P2** · **Needs** F-PKG · **Spec** PROFILES.md §2b (Existing installs)

**Phases**
1. **`-Scope Package`** → the release's certificate trusted through one elevation (that step only); the package added;
   `agentmaster` alias and Start entry.
2. **A portable copy over it** → the question names the package; *Install just for you* adopts its DATA; the checkbox (checked
   unless the package runs) removes it afterwards (its consoles drained, never killed).

---

### INS-09 · The old name still works

> **Since** 0.7.7 · **P1** · **Needs** any install · **Spec** CLAUDE.md *Our additions* (WindowsTerminalShim)

**Phases**
1. **By name.** *Do:* `WindowsTerminal.exe -d C:\e2e` in the install folder. *Expect:* the running Agentmaster opens a tab in
   `C:\e2e`; no second process stays; the forwarder exits.
2. **No app running** → `Agentmaster.exe` starts with the arguments.

---

### INS-10 · Profiles

> **Since** ≤ 0.7.7 · **P1** · **Needs** any install · **Spec** PROFILES.md §2, §2a

**Phases**
1. **Installed first launch** → no prompt; Default (release) or Development (dev); Terminal's own settings under
   `<profile>\terminal\`.
2. **⚙ → PROFILE → Change profile folder…** → the picker; the cog reads `current → new (after restart)`; after a restart the app
   runs on the new folder (copied data when chosen).
3. **Two instances on one profile** → the second warns (the profile mutex).
4. **A non-ASCII user name** → hooks still delivered (the forwarder file carries a BOM; ROB-04).

---

## Every install scenario — the catalog the install lane runs (2026-10-24)

Every scenario the install work covered, on one list: the probe matrix of 2026-10-21, the real app that evening, A1–A5 and
B1 later that night, *A user's own install wins* of 2026-10-22, the installer's edge cases that evening (PROFILES.md §2c,
each table), the INS flows above, and what those passes listed as **not covered**. Each scenario changes the MACHINE: it
installs into Program Files or a user's folder, writes HKCU and HKLM, shortcuts, the PATH and Active Setup, creates
accounts, changes UAC. So the suite runs them in the **install lane** ([`../E2E_TESTING.md`](../E2E_TESTING.md) §8.5): the
host resets the test desktop before EVERY scenario and once more after the last one, and an install test never runs in
any other run — it refuses a desktop the lane did not prepare.

**Machine** — what a scenario needs beyond a freshly reset desktop (UAC off, the signed-in administrator `Docker`, no
network to `api.github.com`, which the lane blocks so the installed app's startup update check fails at once instead of
spending the 60 GitHub API requests an hour this PC shares with its desktops):

| Tag | Means | The lane today |
|---|---|---|
| `fresh` | nothing more | prepared |
| `net` | the real GitHub releases (the API and the downloads) | not yet (the block is lifted per test: planned) |
| `std` | a second local account, standard (`qastd`) | prepared: `provision.ps1 -Lane Install -Accounts` (a generated password kept in the guest); tests start programs as it in the signed-in user's session ("Run as different user") |
| `adm2` | a second local account, administrator (`qaadmin`) | prepared by the same `-Accounts`; with UAC off its start is elevated ("another administrator, elevated") |
| `uac` | UAC on at Windows' defaults (a reboot); its prompts sit on the secure desktop, answered through QEMU's monitor (`tools\install-qa\qm.py`) | not yet |
| `deny` | standard users' elevation refused by policy | not yet |
| `demote` | the signed-in user removed from Administrators (a reboot) | not yet |
| `pkg` | an MSIX package the guest trusts (F-PKG, or the real release bundle with `net`) | not yet |
| `sys` | a one-shot scheduled task as SYSTEM | not yet |
| `old` | a build from before the manifest / `--register-user` (the released v0.7.10 zip, with or without a manifest added) | prepared: `provision.ps1 -Lane Install -Old` extracts v0.7.10 as published; `Kit.old_zip(manifest=…)` makes the zips |
| `sign-in` | a Windows sign-in inside the scenario (Active Setup runs then): a reboot the host must wait out | not yet |
| `human` | a surface the suite cannot drive yet: the taskbar's jump list (pinning), SmartScreen's *Run anyway* | — |

**Checked by hand** names the §2c table that QA'd it (*probe* = 2026-10-21 probe matrix, *real app* = that evening,
*A1*… = later that night, *own* = 2026-10-22, *edges* = 2026-10-22 evening), or *unit* when only
`tools\tests\Test-AgentmasterInstaller.ps1` / the m5 suite covers it. **e2e** names the test in `e2e/suites/install/`, or
what it waits for.

### A · The one-command installer (`tools\Install-Agentmaster.ps1`)

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-01 | `-Scope User -ZipPath <zip> -NonInteractive` | the files and `.am-manifest` in `%USERPROFILE%\.agentmaster\bin`; HKCU Apps & Features `Agentmaster` (DisplayVersion, InstallLocation, `UninstallString` = `"<exe>" --uninstall-portable`); the three "Open in Agentmaster" verbs; the folder on the user PATH; a desktop and a Start-menu shortcut; no file with a download mark; `install.path` = installed, `profile.path` = the Default profile | fresh | edges | `test_install_script.py::test_just_for_you_life_cycle` |
| IS-02 | the same again | "Already up to date for this user"; nothing rewritten | fresh | edges | same test |
| IS-03 | a newer zip over it, a user's own file in the folder | upgraded in place; the user's file kept; no `.am-previous-*` folder left; DisplayVersion re-stamped | fresh | edges | same test |
| IS-04 | `-Uninstall -Scope User` | the shipped names, shortcuts, verbs, entry and PATH entry gone; the user's file, `profile.path` and `%USERPROFILE%\.agentmaster` kept | fresh | probe (pre-manifest), edges (typed into a tab) | same test |
| IS-05 | interactive in a plain PowerShell window, Enter at the question | `[1] Just for you` taken; installed | fresh | probe | planned: typing into a console window from the suite |
| IS-06 | `-Scope User` while the copy runs, typed into a separate window | "It closes for the upgrade and starts again …" → Enter → closed, upgraded, started again (its parent `explorer.exe`) | fresh | edges | planned: same |
| IS-07 | typed into a TAB of the Agentmaster it upgrades | the tab says "the step goes on in a new window"; the window *Agentmaster installer* asks, closes the app, upgrades, starts it again; its record in `%TEMP%\Agentmaster-install` | fresh | edges | planned: same |
| IS-08 | the script-block form typed into a tab (the console-host path) | as IS-07; the tab's session holds nothing of the script afterwards | fresh | edges | planned: same |
| IS-09 | `-Uninstall -Scope User` typed into a tab | the new window; removed; the data kept | fresh | edges | planned: same |
| IS-10 | `& ([scriptblock]::Create(…))` and `… \| iex` in a Windows PowerShell 5.1 and a PowerShell 7 | the caller's `$ErrorActionPreference` kept; no function, `$script:` list, parameter variable or module left behind | fresh | edges, unit | `test_install_script.py::test_the_callers_powershell_keeps_nothing` |
| IS-11 | `-Version 0.7.10` with a bogus `$env:GITHUB_TOKEN` | the token dropped and the request made again; `v0.7.10-prerelease` found | net | edges | planned: `net` |
| IS-12 | `irm … \| iex`, no arguments, a current install | the latest resolved, the installed kind chosen, "Already up to date", nothing downloaded | net | edges | planned: `net` |
| IS-13 | Ctrl+C half way through an upgrade | the previous version back | fresh | unit | planned: a Ctrl+C into the installer's console |
| IS-14 | `-InstallDir .\relative` | made absolute (an elevated or signed-in-user step is another process) | fresh | unit | `test_install_script.py::test_install_dir_rules` |
| IS-15 | `-InstallDir <a folder of other files>` | "… holds other files: Agentmaster goes into …\Agentmaster" | fresh | edges | same test |
| IS-16 | a second per-user copy elsewhere | refused, with how to move it; nothing written | fresh | edges | same test |
| IS-17 | a zip for another processor | refused (an x64 zip on ARM64: a warning) | fresh | unit | planned |
| IS-18 | `-Scope Machine -ZipPath <zip>` | `C:\Program Files\Agentmaster`; HKLM `Uninstall\AgentmasterAllUsers` named *Agentmaster (all users)*; the machine PATH; the HKLM verbs; the Active Setup component (`"<exe>" --register-user`); the signed-in user's own icons, none shared; only that step elevated (UAC on: one consent) | fresh (UAC off: no prompt) | probe, edges | `test_install_script.py::test_for_all_users_life_cycle` |
| IS-19 | the same again | "Already up to date for all users" | fresh | edges | same test |
| IS-20 | `-Uninstall -Scope Machine` | files, the component, the entry, verbs and machine PATH gone; the icons it gave swept; the user's data kept | fresh | own | same test |
| IS-21 | `-Scope Machine` with an OLDER release (no `--register-user`) | shared *Agentmaster (all users)* shortcuts on the Public desktop and in the common Start menu; no Active Setup step | old | edges | `test_install_old_releases.py::test_an_older_release_for_all_users` |
| IS-22 | that copy upgraded to a current build | the shared shortcuts gone, the Active Setup component written | old | edges | same test |
| IS-23 | the older release again, without / with `-Force` | "Already up to date for all users" / downgraded: the component removed, the shared shortcuts back | old | edges | same test |
| IS-24 | `-Scope Machine` with a release that has no manifest (v0.7.10 as published) | refused cleanly | old | probe | `test_install_old_releases.py::test_a_release_without_a_manifest` |
| IS-25 | the all-users upgrade typed into a tab of the all-users copy | the new window, Go on?, closed, upgraded, started again | fresh | edges | planned: typing |
| IS-26 | a standard account, `-ForCurrentAccount`, non-interactive, beside the all-users copy | "non-interactive: installing for scope User"; installed without rights; the note that its own copy takes over | std | edges | `test_install_accounts.py::test_another_account_runs_the_installer` |
| IS-27 | a standard OTHER account, `-Scope User` | refused before anything is downloaded; `-ForCurrentAccount` installs for itself | std | probe | same test |
| IS-28 | SYSTEM (a scheduled task), `-Scope User -Launch` | installed for the signed-in user through a phase in their session; the app runs as them | sys | probe | planned: `sys` |
| IS-29 | another administrator, `-Scope User -Force -Launch` | installed for the signed-in user; their running app closed (GUI only), replaced, started again as them | adm2 | probe | `test_install_accounts.py::test_another_administrator_runs_the_installer` (a fresh machine: nothing running to close) |
| IS-30 | `-Scope Package -NonInteractive` | the release's certificate trusted through one elevation (that step only); the package added for the user | net, pkg | probe, edges | planned: `net` |
| IS-31 | the package upgrade typed into the package app's tab | the new window; the user's own instance closed right before the install (`-ForceApplicationShutdown` does not close it); the new version added and started | net, pkg | edges | planned: `net`, typing |
| IS-32 | `-Uninstall -Scope Package` typed into its tab | the new window, closed, removed, the data kept | net, pkg | edges | planned: `net`, typing |
| IS-33 | a standard user under the deny policy, `-Scope Package -ForCurrentAccount` | the explanation BEFORE the 20 MB bundle is downloaded | std, uac, deny | probe | planned |
| IS-34 | an administrator's password typed for a standard user's elevated step | the user's own Agentmaster closed as theirs (`-OwnSids`), not refused as another user's | std, uac | unit | planned |

### B · A portable copy's first launch (the app's own question)

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-40 | a fresh copy started from Explorer | "Where should Agentmaster live?" with *Install just for you (recommended)*, *Install for all users of this PC*, *Install to a folder you pick…*, *Install here*, *Keep it portable, run from here*, *Ask me later* | fresh | probe, real app | `test_install_first_launch.py::test_install_just_for_you` |
| IS-41 | *Install just for you* | IS-01's install; the copy becomes a launcher stub of it (`install.path` = the install folder); the installed copy started from its folder, the copy's own process gone; `[install]` lines in the Default profile's `hooks.log` | fresh | real app | same test |
| IS-42 | started elevated (with UAC off every start is) | the note *Agentmaster is running with administrator rights*; the installed copy started through the one-shot task (UAC on: unelevated, measured `elevated=0`) | fresh | probe, A5 | same test (the note; the dropped rights need `uac`) |
| IS-43 | *Ask me later* | runs on `<copy>\profile`; no `install.path`, no `profile.path`; the next launch asks again | fresh | probe | `…::test_ask_me_later_then_keep_it_portable` |
| IS-44 | *Keep it portable, run from here* | `install.path` = portable; the profile picker (*Portable profile (self-contained)* first, Default, Development, Browse…); `profile.path` written; never asked again | fresh | — | same test |
| IS-45 | a newer copy over a per-user install | *Update the installed Agentmaster (vX → vY) (recommended)*; updated in place; a user's own file kept | fresh | probe, real app | `…::test_a_newer_copy_updates_and_an_older_one_hands_over` |
| IS-46 | an older copy over it | *Use the installed Agentmaster (vZ) (recommended)*; never a downgrade; the copy becomes a stub (`install.path` = the install) and its next launch starts the installed copy | fresh | probe, real app | same test |
| IS-47 | *Install for all users of this PC* (UAC off: no prompt) | IS-18's install; the user's own icons; the copy runs as the user | fresh | probe, real app (UAC Yes) | `…::test_install_for_all_users` |
| IS-48 | *Install for all users*, UAC *No* | "cancelled at the UAC prompt", back to the question, nothing installed | uac | real app | planned: `uac` |
| IS-49 | a per-user and an all-users install, both current | the per-user one recommended | fresh | probe | planned |
| IS-50 | *Install to a folder you pick…* | a non-empty folder → `<folder>\Agentmaster`; inside the copy → refused; Program Files without rights → refused, pointing at *for all users* | fresh | — | planned: the folder dialog |
| IS-51 | *Install here* | the registration only, no copy | fresh | — | planned |
| IS-52 | another administrator, elevated, in the user's session | *Set it up for <user> (recommended)*; the copy restarts as the user (~13 s), who gets their own question; nothing installed for the administrator | adm2 | probe, real app | `test_install_accounts.py::test_another_administrator_sets_it_up_for_the_signed_in_user` |
| IS-53 | another account without rights in the user's session | the warning with how to set it up for the signed-in user; its own install offered by name; nothing written | std | probe, real app | `test_install_accounts.py::test_another_account_sees_the_question` |
| IS-54 | SYSTEM in session 0 | no question; nothing persisted | sys | probe | planned: `sys` |
| IS-55 | `AGENTMASTER_PROFILE` set, or `-Embedding` | no question | fresh | — | every other suite (each test sets the profile) |
| IS-56 | a browser-marked zip extracted by Explorer | SmartScreen once (*More info* → *Run anyway*); the installed copy asks nothing more; no installed file keeps the mark (60 of 63 did before the fix) | human | real app | planned |
| IS-57 | the copy `Install-Agentmaster.ps1` v0.7.10 made, then a newer copy | *Update the installed Agentmaster … the copy the install script made*; registered in that folder; *Agentmaster (Portable)* removed | fresh | A1 | planned |
| IS-58 | an installed MSIX package, then a portable copy | the question names it; *Install just for you* adopts its data; the checkbox (checked unless it runs) removes it, its consoles drained | pkg | — | planned: `pkg` |
| IS-59 | a STANDARD signed-in user: *Install just for you* / *for all users* with an administrator's password | no prompt / the all-users install, the copy running as the user | std, uac, demote | real app | planned |
| IS-60 | a standard signed-in user: *Run as administrator* with an administrator's password | the app runs as that administrator → *Set it up for <user>* → restarts as the user | std, uac, demote | real app | planned |

### C · For all users, and its users

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-61 | the user's marker and both icons deleted, then a sign-in | Active Setup's stub ran at the sign-in: both icons back with the AUMID, the marker written, no app started | sign-in | own | planned: `sign-in` |
| IS-62 | the desktop icon deleted, the shared copy started from Start | the icon stays deleted | fresh | own | planned |
| IS-63 | a standard user starts the all-users copy | no question; their own profile `C:\Users\<them>\.agentmaster`; the folder untouched | std | probe | `test_install_accounts.py::test_a_standard_user_and_the_shared_copy` |
| IS-64 | `--register-user` as the second account | that account's own icons; no `.agentmaster` folder made | std | own | same test |
| IS-65 | the all-users uninstall with a second account's icons | every user's icons swept; after a reinstall the second account gets icons again | std | own | same test (the icons again through `--register-user`; Active Setup's own run at a sign-in needs `sign-in`) |
| IS-66 | a standard user under the deny policy: update or uninstall the all-users copy, its in-app update | "Windows does not let this account run programs as administrator (a setting of this PC)" and what to do; nothing changed | std, uac, deny | probe, real app | planned |

### D · A user's own install wins

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-70 | the shared copy pinned to the taskbar, then the user's own copy | the icons replaced (its AUMID); the pin re-pointed in place; one taskbar button | human (pinning) | own | planned |
| IS-71 | the shared copy started by path with `-d C:\Windows` | handed off: the user's copy runs with the arguments, the shared process gone; logged | fresh | own | `test_install_coexistence.py::test_a_users_own_copy_takes_over` |
| IS-72 | the same while the user's copy runs | one process, the user's copy | fresh | own | same test |
| IS-73 | `AGENTMASTER_NO_HANDOFF=1` | the shared copy runs itself | fresh | own | same test |
| IS-74 | the shared copy's `agentmaster-cli.exe` | the user's CLI runs with the arguments; a redirected file and the exit code pass through | fresh | own | planned |
| IS-75 | the user's copy uninstalled while the shared one stays | its icons open the shared copy again | fresh | own | same test |
| IS-76 | the user's copy deleted by hand, then the shared copy starts | the leftovers removed (verbs, entry, PATH, icons), the pins moved back, the icons made again | fresh | own | planned |

### E · The in-app update

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-80 | ⚙ *Check for updates* on a copy stamped older, pre-releases on → *Update now* | the release's zip swapped in; the registration re-stamped; relaunched in ~12 s | net | real app, B1 | planned: a fake releases API, or `net` |
| IS-81 | the startup prompt | before "Reopen your N windows?": *Update now* / *Postpone* + six choices; `[update] check (startup) … available` | net | A3–A5 | planned |
| IS-82 | *Remind me next restart* / tomorrow / 3 · 7 · 30 days / skip | the latch / `updatePostponedUntilUnixMs` / `updateSkippedVersion` | net | — | planned |
| IS-83 | the all-users copy's in-app update | only the swap elevated; HKLM re-stamped; relaunched unelevated | net, uac | probe | planned |
| IS-84 | another user runs the all-users copy during its update (A3) | the swap refused → [A] / [U] / [C]; [C] restarts the installed version with no re-offer; [U] installs just for this user on the same profile | net, std, uac | A3 | planned |
| IS-85 | the MSIX package's update (A4) | the certificate question [A] / [U] / [C]; [C] restarts it with no prompt; [U] installs per user on the package's profile and removes the package | net, pkg | A4 | planned |
| IS-86 | a demoted user updates the per-user install an administrator made (B1) | no prompt; updated in ~12 s; no set-aside folder left | net, demote | B1 | planned |
| IS-87 | the swap fails half way (a locked file) | the old files back; the installed version started again; that run behaves as *Remind me next restart* | net | unit | planned |
| IS-88 | offline | the check fails quietly within 6 s; the app starts normally | fresh | — | planned (the lane is offline to GitHub: a startup-time check) |

### F · Uninstall

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-90 | ⚙ → About → *Uninstall Agentmaster…* on a per-user install | the confirm names what goes; the app closes; everything but the data removed | fresh | real app | planned: the cog's About tab |
| IS-91 | Apps & Features' command (`Agentmaster.exe --uninstall-portable`) on a per-user install | *Uninstall Agentmaster?* → *Uninstall*: everything but the data removed | fresh | probe | `test_install_coexistence.py::test_uninstall_from_apps_and_features` |
| IS-92 | the same, *Cancel* | nothing removed | fresh | — | same test |
| IS-93 | `--uninstall-portable` on a copy that is not an install | *Nothing to uninstall here*; nothing removed | fresh | probe | same test |
| IS-94 | the all-users copy's Uninstall, UAC *No* / *Yes* | nothing removed / everything removed, every user's data kept | uac | real app | planned: `uac` |
| IS-95 | a pre-manifest install (v0.7.10) | removed through the legacy list; the user's files kept | old | probe | same test |

### G · The old name, profiles

| Id | Scenario | Expected | Machine | Checked by hand | e2e |
|---|---|---|---|---|---|
| IS-97 | `WindowsTerminal.exe -d <dir>` in the install folder while the app runs | the running Agentmaster takes it (a new tab); the forwarder exits; one Agentmaster process | fresh | — | `test_install_coexistence.py::test_the_old_name_forwards` |
| IS-98 | the same with no app running | `Agentmaster.exe` starts with the arguments | fresh | — | same test |
| IS-99 | an installed copy's first start | no profile question; the Default profile; Terminal's own settings under `<profile>\terminal\` | fresh | real app | `test_install_first_launch.py::test_install_just_for_you` |
