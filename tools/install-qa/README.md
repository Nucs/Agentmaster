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
(`[install]` lines), the registry (`HKCU` / `HKLM ...\Uninstall\Agentmaster`, `Software\Classes\...\shell\Agentmaster`,
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
