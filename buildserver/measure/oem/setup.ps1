<#
.SYNOPSIS
    Registers the measurement job runner in a freshly installed build-server test VM; runs once, from install.bat.

.DESCRIPTION
    dockur copies the OEM folder to C:\OEM and runs install.bat at the end of the unattended install, as the
    auto-logon user (an administrator). This script:
    - records when the install finished (C:\OEM\installed.txt), the end point of the install-time measurement;
    - registers agent.ps1 as the scheduled task BsAgent, started at that user's logon, elevated, with no time limit
      (a job can be a build of an hour or more);
    - starts the task at once, so the first boot reports without waiting for a second logon.
    Register-ScheduledTask can be missing from a trimmed Windows (Tiny11 Core); schtasks.exe is the fallback, and
    which one worked is appended to installed.txt.

.OUTPUTS
    None. Errors go to C:\OEM\setup.log through install.bat's redirection.

.NOTES
    Throws (and so leaves the task unregistered, which setup.log shows) only when both registration paths fail.
#>
$ErrorActionPreference = 'Stop'
(Get-Date).ToString('o') | Set-Content -Encoding ascii C:\OEM\installed.txt
$runner = 'powershell.exe'
# Not $args: that name is PowerShell's automatic variable for a script's own arguments.
$arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\OEM\agent.ps1'
try {
    $action = New-ScheduledTaskAction -Execute $runner -Argument $arguments
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName 'BsAgent' -Action $action -Trigger $trigger -Principal $principal `
        -Settings $settings -Force | Out-Null
    'registered with Register-ScheduledTask' | Add-Content C:\OEM\installed.txt
} catch {
    # The same task through schtasks.exe; it has no time-limit switch (its default limit is 72 h).
    schtasks.exe /Create /F /TN BsAgent /SC ONLOGON /RL HIGHEST /TR "$runner $arguments" | Out-Null
    "registered with schtasks.exe ($($_.Exception.Message))" | Add-Content C:\OEM\installed.txt
}
schtasks.exe /Run /TN BsAgent | Out-Null
