<#
.SYNOPSIS
    Registers the scheduled task AmbBootstrap, which starts the guest agent (C:\amb\bootstrap.ps1) at logon and again
    every minute while it does not run. Used by setup.ps1 at a base install, and by `images.py retask` on an image made
    before 2026-11-12, whose task started the bootstrap at logon only.

.DESCRIPTION
    The task, as one XML definition (the same through either way of registering it):
    - Trigger: the logon of the user this script runs as (the auto-logon user of dockur's install), repeated every
      minute for as long as the user stays logged on.
    - MultipleInstancesPolicy IgnoreNew: while the bootstrap runs (it waits for the agent, which runs for the slot's whole
      life), a repetition starts nothing. A repetition starts the bootstrap only when the previous run has ended: a run
      that died, or a bootstrap that gave up after its agent's 20 starts.
    - The action is unchanged: powershell.exe -WindowStyle Hidden -File C:\amb\bootstrap.ps1, elevated (HighestAvailable),
      in the interactive session (the e2e suite drives the desktop), with no time limit, on batteries too.
    Why the repetition: on the worker machine's site (2026-11-12, a 2013 Xeon shared with other guests) the logon's run
    died at once with STATUS_CONTROL_C_EXIT (0xC000013A) before bootstrap.ps1 wrote its first line, on every boot with 4
    or more vCPUs, and nothing started it again, so the slot never said hello. A second run, started by hand minutes
    later in the same boot, worked. With 2 vCPUs, and on homepc with the same image, the logon's run worked.

.PARAMETER Note
    A file that gets one line saying how the task was registered (setup.ps1: C:\OEM\installed.txt).

.OUTPUTS
    The task's definition as Task Scheduler holds it afterwards (XML), so a layer step's output shows what was set.

.NOTES
    PowerShell 5.1 (the guest's own); ASCII only (5.1 reads a script without a byte-order mark in the ANSI code page).
    Register-ScheduledTask can be missing from a trimmed Windows (Tiny11 Core); schtasks.exe /XML is the fallback.
    Throws when both registrations fail.
    Re-registering replaces the definition (-Force / schtasks /F); the bootstrap that runs at that moment keeps running
    (measured 2026-11-12: `images.py retask` re-registered the task from a step of the agent that bootstrap had started,
    and that agent went on to its shutdown step).
#>
param([string]$Note = 'C:\OEM\installed.txt')
$ErrorActionPreference = 'Stop'

# The account as Task Scheduler resolves a local user; escaped, because it goes into XML text.
$user = [System.Security.SecurityElement]::Escape($env:USERNAME)
# Not $args: that name is PowerShell's automatic variable for a script's own arguments.
$arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\amb\bootstrap.ps1'
# Task Scheduler 1.2's schema. A Repetition without a Duration repeats for as long as the trigger's logon lasts.
$xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>The build server's guest agent: C:\amb\bootstrap.ps1 at logon, and again every minute while it does not run.</Description>
  </RegistrationInfo>
  <Triggers>
    <LogonTrigger>
      <Enabled>true</Enabled>
      <UserId>$user</UserId>
      <Repetition>
        <Interval>PT1M</Interval>
        <StopAtDurationEnd>false</StopAtDurationEnd>
      </Repetition>
    </LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>$user</UserId>
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>powershell.exe</Command>
      <Arguments>$arguments</Arguments>
    </Exec>
  </Actions>
</Task>
"@

try {
    Register-ScheduledTask -TaskName 'AmbBootstrap' -Xml $xml -Force | Out-Null
    $how = 'registered with Register-ScheduledTask (at logon, again every minute while it does not run)'
} catch {
    $first = $_.Exception.Message
    # schtasks.exe reads the definition from a file; UTF-16, as the XML's own declaration says.
    $file = Join-Path $env:TEMP 'AmbBootstrap.xml'
    Set-Content -Path $file -Value $xml -Encoding Unicode
    & schtasks.exe /Create /F /TN AmbBootstrap /XML $file | Out-Null
    $code = $LASTEXITCODE
    Remove-Item -Force $file -ErrorAction SilentlyContinue
    if ($code -ne 0) {
        throw "AmbBootstrap could not be registered: Register-ScheduledTask said '$first'; schtasks.exe exited $code"
    }
    $how = "registered with schtasks.exe /XML, at logon and again every minute ($first)"
}
$how | Add-Content $Note
# What Task Scheduler now holds: the trigger's Repetition and the IgnoreNew policy can be read in the step's output.
& schtasks.exe /Query /TN AmbBootstrap /XML
