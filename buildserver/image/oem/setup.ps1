<#
.SYNOPSIS
    Makes a freshly installed Windows a build-server base image; runs once, from install.bat, at the end of dockur's
    unattended install.

.DESCRIPTION
    The base image holds only what every slot needs, so layers (am-e2e, am-build) stay small and the guest agent can be
    changed without rebuilding any image:
    - C:\amb\bootstrap.ps1 (copied from this folder), registered as the scheduled task AmbBootstrap: at every logon of
      the auto-logon user, elevated, with no time limit. It maps the slot's share to Z:, copies the guest agent from the
      share by its exact path, and runs it (bootstrap.ps1 says why each step is there);
    - the time zone set to UTC (dockur's guest came up on Pacific time over a Jerusalem clock, DESIGN.md §4.4), so
      every timestamp a guest writes compares with the coordinator's;
    - the power plan High performance (a VM's guest otherwise throttles its vCPUs on a balanced plan).
    Then the bootstrap task is started at once: the first boot's agent says hello to the coordinator, which is how the
    coordinator knows the install has finished (DESIGN.md §4.3).

.OUTPUTS
    None. C:\OEM\installed.txt gets the install's end time and how the task was registered.

.NOTES
    Register-ScheduledTask can be missing from a trimmed Windows (Tiny11 Core); schtasks.exe is the fallback (measured
    2026-10-25: both work on Tiny11 Core 25H2). Throws only when both registrations fail, which setup.log then shows.
#>
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path 'C:\amb' | Out-Null
Copy-Item -Force 'C:\OEM\bootstrap.ps1' 'C:\amb\bootstrap.ps1'
(Get-Date).ToUniversalTime().ToString('o') | Set-Content -Encoding ascii 'C:\OEM\installed.txt'

# UTC, so guest and coordinator timestamps agree; High performance (SCHEME_MIN), so vCPUs are not throttled.
& tzutil.exe /s 'UTC'
& powercfg.exe /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c

$runner = 'powershell.exe'
# Not $args: that name is PowerShell's automatic variable for a script's own arguments.
$arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\amb\bootstrap.ps1'
try {
    $action = New-ScheduledTaskAction -Execute $runner -Argument $arguments
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName 'AmbBootstrap' -Action $action -Trigger $trigger -Principal $principal `
        -Settings $settings -Force | Out-Null
    'registered with Register-ScheduledTask' | Add-Content 'C:\OEM\installed.txt'
} catch {
    # The same task through schtasks.exe; it has no switch for the time limit (its default limit is 72 h).
    schtasks.exe /Create /F /TN AmbBootstrap /SC ONLOGON /RL HIGHEST /TR "$runner $arguments" | Out-Null
    "registered with schtasks.exe ($($_.Exception.Message))" | Add-Content 'C:\OEM\installed.txt'
}
schtasks.exe /Run /TN AmbBootstrap | Out-Null
