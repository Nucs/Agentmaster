# Install QA kit — the first-launch install flow in a disposable Windows desktop

The manual QA behind [`doc/agentmaster/PROFILES.md`](../../doc/agentmaster/PROFILES.md) §2c (2026-10-21):
the per-user / all-users installs, the account and rights scenarios, the uninstalls and the in-place
update, run for real — in a **disposable** Windows desktop (the `claude-fleet` Windows desktops, skill
`claude-desktops`), never on a workstation: everything here installs, registers, creates accounts and
uninstalls for real.

| File | What |
|---|---|
| `amprobe.cpp` + `amprobe.rc` | A GUI exe NAMED `Agentmaster.exe` that is the app's startup prelude and nothing else — `HandleUninstallFlagAtStartup` → `EnsureInstallDecidedAtStartup` → `EnsureProfileResolvedAtStartup` → `RefreshInstalledRegistration` from `PortableInstall.h` / `ProfileBootstrap.h`, with the repository's real `am-update.ps1` embedded as `AM_UPDATE_PS1` — that logs one line per step to `C:\amqa\probe.log` (`START` with the account, elevation, session user and account plan; the install step's outcome; `RESULT` with the decision, machine-copy / installed flags and the resolved profile). |
| `build.cmd` | Builds it (`/W4`, static CRT, embedded manifest) into `Q:\tmp\amprobe` (or the folder given). Not part of the product build. |
| `stage.py` | Stages fake portable unzips of the probe (`probe-<ver>` with `.portable`, `.am-version`, `.am-manifest`), a release-shaped zip of the newest one, and the scripts, into a desktop's persist folder (`Z:\amqa` in the guest). |
| `amqa-guest.ps1` | Guest-side helpers for the REAL app (dot-source it in the guest): `Get-AmQaState` prints every install, Apps & Features entry, PATH entry, verb, the all-users install's Active Setup component and each account's marker of it, every shortcut and taskbar pin with its AppUserModelID (`Get-AmQaLinkAumid`, `Get-AmQaPins`), each profile (+ its last `[install]` lines) and each running `Agentmaster.exe` with its account, real token elevation and command line; `New-AmQaCopy <version> <name>` makes a fresh stamped copy of the build; `Reset-AmQa` removes every install, registration, pin file, Active Setup entry and profile between scenarios. |
| `qm.py` | Hardware-level keys and screen of the desktop through QEMU's monitor (`key`, `type`, `shot`) — the only way to see and answer a UAC consent or credential prompt, which sits on the secure desktop where the desktop's own agent (SendInput, its screenshots) cannot reach. |
| `mkmsix.ps1` | PowerShell 7, on the development PC: re-packs a build's unsigned `.msix` at a chosen (older) version and signs it with a throwaway `CN=Agentmaster` certificate made in memory; leaves the signed `.msix` and the public `qa-signing.cer`. The test desktop trusts the `.cer` and installs the package, so the app's own PACKAGE update can run there. |

## Run it

```powershell
tools\install-qa\build.cmd
python -I tools\install-qa\stage.py --persist Q:\claude-desktops\windows\desktops\<name>\persist
```

In the guest (the desktop's `run_command`, which runs as the signed-in administrator `Docker`, elevated):

```powershell
Copy-Item -Recurse -Force Z:\amqa\* C:\amqa\
icacls C:\amqa /grant '*S-1-1-0:(OI)(CI)M'          # every test account writes C:\amqa\probe.log
# Two more accounts: qastd (standard) and qaadmin (Administrators), with a generated password kept in
# C:\amqa\.pw readable by Docker only; New-LocalUser / Add-LocalGroupMember.
```

How each kind of start was made (all verified):

- **The signed-in user, unelevated:** the desktop's `launch("explorer.exe C:\amqa\<copy>\Agentmaster.exe")` —
  Explorer starts it with its own (filtered) token.
- **The signed-in user, elevated:** `launch("C:\amqa\<copy>\Agentmaster.exe")` (the agent is elevated).
- **Unelevated with arguments** (`--uninstall-portable`, a script): a one-shot task,
  `New-ScheduledTaskPrincipal -UserId Docker -LogonType Interactive -RunLevel Limited`.
- **Another account in the signed-in user's session** ("Run as different user"): `Start-Process -Credential …
  -LoadUserProfile` with ALL THREE standard streams redirected (`-RedirectStandardInput/Output/Error`) —
  otherwise .NET hands the agent's own pipe to the child, and the desktop call waits until the probe exits.
- **SYSTEM in session 0:** a task with `-UserId SYSTEM -LogonType ServiceAccount`.

Always start from a FRESH copy of `Z:\amqa\probe-<ver>`: a copy that decided becomes a launcher stub (its
`install.path` names the install) and hands every later launch straight off.

**Two passes.** The desktops come with UAC OFF (every administrator token unsplit — "elevated" for every
admin). For the escalation / de-escalation paths, turn it on and reboot:

```powershell
$k = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
Set-ItemProperty $k EnableLUA 1; Set-ItemProperty $k ConsentPromptBehaviorAdmin 0   # admins elevate without a prompt
Set-ItemProperty $k ConsentPromptBehaviorUser 0; Set-ItemProperty $k PromptOnSecureDesktop 0   # standard users: refused
shutdown /r /t 5 /f
```

The agent comes back elevated (its task runs with highest privileges). ⚠ After that reboot the elevated
agent no longer sees the `Z:` mapping: use `\\host.lan\Data`.

**Read the results:** `C:\amqa\probe.log`, each account's `%USERPROFILE%\.agentmaster\hooks.log`
(`[install]` lines), the registry (`HKCU ...\Uninstall\Agentmaster`, `HKLM ...\Uninstall\AgentmasterAllUsers` —
`HKLM ...\Uninstall\Agentmaster` for an all-users install registered before the evening of 2026-10-21 —,
`Software\Classes\...\shell\Agentmaster`,
the user / machine `Path`), the shortcuts, and the folders. The PowerShell installer
(`C:\amqa\repo\tools\Install-Agentmaster.ps1`) runs against the real releases (`-Prerelease` while all of
them are pre-releases); its suite (`C:\amqa\repo\tools\tests\Test-AgentmasterInstaller.ps1
-IncludeScheduledTask`) runs there too.

**Traps met:** the desktop's `launch` gives a console a redirected standard input, so the installer's
question reads an empty line at once and takes the default (the installer now notices and says so);
`cmd /c start` from `run_command` shows a real console but the call then waits on it (inherited
handles); an unattended installer run in a hidden window needs `-NonInteractive`, or a question it
reaches waits forever; a `Get-CimInstance ... -like '*Install-Agentmaster.ps1*'` kill also matches the
agent's own command line.

**Cleanup:** move the evidence out of the persist folder, remove `amqa` from it, and shut the guest down
(`shutdown /s`) — or delete / reset the desktop; a reset wipes the accounts and every install.

## The real app (a Release build), with Windows' own UAC prompts

The probe covers the startup prelude; the installed app itself (its window, the ⚙ About tab's Uninstall,
the in-app update) needs a real build. Done this way on 2026-10-21 (PROFILES.md §2c, "The real app"):

1. **Build** the release identity into a QA folder (the build mutex; on a loaded PC a commit guard and
   `-MaxCpu 1 -ClMpCount 2`): `tools\Build-Agentmaster.ps1 -NoRestore -Configuration Release -ReleaseIdentity -LayoutDir Q:\agentmaster-layouts\QA`.
   The build also writes the package's `.msix` to `src\cascadia\CascadiaPackage\AppPackages\CascadiaPackage_0.0.1.0_x64_Test\`.
2. **Zip** it like release.yml: `build\scripts\New-UnpackagedTerminalDistribution.ps1 -TerminalAppX <that .msix> -XamlAppX <its Dependencies\x64\Microsoft.UI.Xaml.2.8.appx> -Destination <dir> -PortableMode:$true`
   (the `-TerminalLayout` mode makes no zip: it leaves an assembled folder in `%TEMP%`).
3. **Stage** the unzipped build as `amqa\build`, the zip, `amqa-guest.ps1` and a password file `amqa\.pw`
   in the persist folder; in the guest copy them to `C:\amqa`, move `.pw` off the share, create `qastd` /
   `qaadmin` with it, and turn UAC on (`EnableLUA 1` + a reboot: the golden image keeps the prompt settings
   at Windows' defaults with LUA off). A 4 GB desktop is enough.
4. **Run** each scenario on a FRESH copy (`New-AmQaCopy '0.8.0.0' 's1'`; stamp other versions for
   upgrades / downgrades / the in-app update), start it the way a user would (the desktop's `launch` of
   `explorer.exe <exe>` = a double-click), read the outcome with `Get-AmQaState`, `Reset-AmQa` between.
5. **Answer UAC with `qm.py`:** `python -I qm.py shot s.png` shows the secure desktop; `key esc` = No;
   `key shift-tab ret` = Yes (No has the focus); `type @<password file>` + `key ret` for a credential prompt.
   A window that runs elevated cannot take the (unelevated) agent's input either — press its keys through
   `qm.py` too.
6. **A standard signed-in user:** first give `Docker` write access to `C:\ProgramData\claude-desktop` (the
   desktop's agent writes there) and to `C:\amqa`, then remove `Docker` from Administrators and reboot; the
   agent then runs as a standard user. Changing a machine policy afterwards needs `qaadmin` through a
   credential prompt — or, once "deny elevation requests" is set, a reset.
7. **A downloaded zip (the Mark of the Web):** `[IO.Compression.ZipFile]::CreateFromDirectory` (not
   `Compress-Archive`: the share marks the dot files hidden and it skips them), a `Zone.Identifier` stream
   on the zip (`ZoneId=3`), and the shell's own extraction (`Shell.Application` `CopyHere`, what Explorer's
   *Extract All* does — it copies the mark onto every file). Launching it brings up SmartScreen.

**More traps met:** the agent's `run_command` waits for every process in its job — start anything that
waits on a UAC prompt with the agent's `launch` tool (or `Start-Process -Credential`, which leaves the
job), or the call hangs and its timeout kills the request; `docker cp` cannot read the monitor's tmpfs
(`qm.py` streams the screendump through `docker exec cat`); a PowerShell started from Git Bash finds Git's
`tar.exe` first (the packaging script now calls Windows' own).

## The package update, two users on one copy, a demotion

Done this way later the same night (PROFILES.md §2c, "The paths that were still open"); the same desktop
setup as above. Each run used a fresh copy stamped OLDER than the newest published release
(`New-AmQaCopy '0.7.0.0' <name>`): ⚙ → About → *Allow updating to pre-release versions* + *Check for
updates* then offers that release, and *Update now* downloads it from GitHub. (A local file cannot stand in
for the release: Windows PowerShell 5.1's `Invoke-WebRequest` refuses `file://`.)

1. **Zip and package.** From Git Bash, quote the switch: `'-PortableMode:$true'`. Unquoted, bash expands
   `$true` to nothing, the switch is off, and the zip has no `.portable`, `.am-manifest` or `.am-version`.
   Then `pwsh -File tools\install-qa\mkmsix.ps1 -Msix <the build's .msix> -OutDir <dir> -Version 0.7.0.0`
   and stage the `.msix`, `qa-signing.cer` and the build's `Dependencies\x64\Microsoft.UI.Xaml.2.8.appx`.
   After copying from `Z:`, clear the Hidden attribute the share puts on the dot files.
2. **The MSIX package:** `Import-Certificate -FilePath qa-signing.cer -CertStoreLocation
   Cert:\LocalMachine\TrustedPeople`, `Add-AppxPackage -Path <msix> -DependencyPath <Microsoft.UI.Xaml.2.8.appx>`,
   start it with `shell:appsFolder\Agentmaster_56k4f06dsfp9r!App`. Its update reaches the certificate
   question (the release's own certificate is not trusted); `Reset-AmQa` removes the package again (remove
   the `CN=Agentmaster` certificate from `TrustedPeople` by hand).
3. **The copy the old install script made** (v0.7.10 and older): extract the published portable zip into
   `%LOCALAPPDATA%\Programs\Agentmaster`, write `.am-version`, and create `Agentmaster (Portable).lnk` in the
   user's Start-menu Programs folder pointing at its `Agentmaster.exe`. No registry entry: that script made
   none.
4. **Two users on the all-users copy:** install it for all users, then start it as the second account in the
   signed-in user's session: `Start-Process 'C:\Program Files\Agentmaster\Agentmaster.exe' -Credential …
   -LoadUserProfile` with all three standard streams redirected (the input file must exist, or
   `Start-Process` fails). That instance keeps the folder busy, so the first user's update cannot swap it.
5. **A demotion:** give `Docker` Modify on `C:\ProgramData\claude-desktop` and `C:\amqa` (`icacls … /T`),
   `Remove-LocalGroupMember -Group Administrators -Member Docker`, `shutdown /r`. The agent comes back as a
   standard user (its task's "highest privileges" gives a standard user its normal token); `whoami /groups`
   shows no Administrators and a Medium mandatory level. `Get-AmQaState` then cannot read other users'
   folders or every package (`Access is denied` on stderr: expected). A `reset_desktop` makes `Docker` an
   administrator again.

**Traps met:** an UNELEVATED process query lists other users' processes with their paths and owners, so
"is my app running from this folder" needs an owner filter; Settings → Installed apps lists an Uninstall
key NAME once, so two installs need two key names; a screenshot taken right after a click can still show
the previous frame (take another before deciding the click did nothing — though twice a first click on ⚙
or *Check for updates* really did nothing).

## A user's own install beside the all-users one (2026-10-22)

The checks behind PROFILES.md §2b *A user's own install wins* / §2c (the per-user icons through Active Setup,
the take-over, the hand-off of the app and the CLI, the uninstalls, a copy deleted by hand). The same setup
as above, with UAC left OFF (the hand-off must stay quick for an elevated start) and one standard account
`qastd`. How each step was made:

1. **The all-users install:** a fresh copy (`New-AmQaCopy '0.8.0.0' m1`) started through Explorer, *Install for
   all users* clicked through the agent's `find_elements` + `element_action` (the TaskDialog's command links
   are buttons). The installed copy's first start makes the user's icons.
2. **Windows' own Active Setup:** delete the user's marker (`HKCU\SOFTWARE\Microsoft\Active Setup\Installed
   Components\{F8D18BA4-7DD1-4F04-8EE0-C9C7342F3264}`) and both icons, `shutdown /r`: the desktop signs
   `Docker` in again, and Windows runs the StubPath at that sign-in (the icons came back 17 s after the boot).
3. **Another account's step:** `Start-Process 'C:\Program Files\Agentmaster\Agentmaster.exe' -ArgumentList
   '--register-user' -Credential <qastd> -LoadUserProfile` with all three standard streams redirected.
4. **A taskbar pin:** start the copy, right-click its taskbar button (the button's name is "Agentmaster - 1
   running window"), then *Pin to taskbar* in the jump list (a plain `click` on its position: the jump list
   is not in the agent's accessibility tree). Programs cannot pin by themselves on Windows 11.
5. **The user's own copy:** a fresh copy stamped NEWER (`New-AmQaCopy '0.8.1.0' u1`), *Install just for you*.
6. **The app hand-off:** the agent's `launch` of `"C:\Program Files\Agentmaster\Agentmaster.exe" -d C:\Windows`,
   then the process list (`Get-AmQaState` prints each process's command line). The opt-out:
   `launch('cmd.exe /c set AGENTMASTER_NO_HANDOFF=1&& start "" "<exe>"')`.
7. **The CLI hand-off:** copy `C:\Windows\System32\cmd.exe` over the user's `agentmaster-cli.exe` (keep the real
   one aside), then run the SHARED CLI as `& <shared cli> /c "echo HANDOFF-%USERNAME% & exit 7"`: the output,
   `$LASTEXITCODE` and a `> file` redirect show what reached the user's copy. Put the real CLI back after.
8. **A copy deleted by hand:** stop it, `Remove-Item -Recurse` its folder, start the shared copy.

**Traps met:** the agent's `launch` reported a timeout for the start through `cmd /c start` although the app
ran (the hand-off's own start was found, its window belonging to the copy it started): read the process list; Explorer's UI Automation id of a pinned taskbar button (`Appid: …`) keeps the
AppUserModelID the pin had when it was pinned, while the grouping follows the pin file as it is now (one
button, "1 running window pinned"); the agent kills with `Stop-Process`, which skips the app's own exit path,
so a pin's id the app would stamp at its exit stays as it was; a `Reset-AmQa` deletes pin files, and the
taskbar shows such a pin blank until Explorer restarts.
