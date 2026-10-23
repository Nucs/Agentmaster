# 19 · Install, update, uninstall and profiles (INS)

> Conventions and fixtures: [README.md](README.md). Spec: PROFILES.md §2–§2c (the question, the scenario matrix and
> every QA table), CLAUDE.md *Release/dev separation + PROFILES* and *In-app auto-updater*, `Updater.h`. Needs a CLEAN
> Windows desktop per flow (`reset_desktop`), **F-ZIP** (portable zips of the build under test stamped at chosen versions
> — `tools\install-qa\amqa-guest.ps1` `New-AmQaCopy`), **F-PKG** (an MSIX of the same sources signed with a throwaway
> certificate the guest trusts — `tools\install-qa\mkmsix.ps1`), a second (standard) and a third (administrator) local
> account, and a **fake GitHub releases API** (or real releases with a copy stamped older). A UAC prompt sits on the
> secure desktop: answer it through QEMU's monitor (`tools\install-qa\qm.py`). [OS] here = `Get-AmQaState` (every
> install, Apps & Features entry, PATH entry, verb, shortcut and pin with its AUMID, profile, running process with its
> account and elevation). These flows automate the manual QA tables of PROFILES.md §2c.

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
4. **Install just for you.** *Expect:* as INS-01's registrations; the unzip's own folder untouched; the installed copy
   started, the unzip's process gone.

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
