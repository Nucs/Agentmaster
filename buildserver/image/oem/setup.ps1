<#
.SYNOPSIS
    Makes a freshly installed Windows a build-server base image; runs once, from install.bat, at the end of dockur's
    unattended install.

.DESCRIPTION
    The base image holds only what every slot needs, so layers (am-e2e, am-build) stay small and the guest agent can be
    changed without rebuilding any image:
    - C:\amb\bootstrap.ps1 (copied from this folder), registered as the scheduled task AmbBootstrap by
      register-bootstrap.ps1: at every logon of the auto-logon user and again every minute while it does not run,
      elevated, with no time limit. It maps the slot's share to Z:, copies the guest agent from the share by its exact
      path, and runs it (bootstrap.ps1 says why each step is there);
    - the time zone set to UTC (dockur's guest came up on Pacific time over a Jerusalem clock, DESIGN.md §4.4), so
      every timestamp a guest writes compares with the coordinator's;
    - the power plan High performance (a VM's guest otherwise throttles its vCPUs on a balanced plan).
    Then the bootstrap task is started at once: the first boot's agent says hello to the coordinator, which is how the
    coordinator knows the install has finished (DESIGN.md §4.3).

.OUTPUTS
    None. C:\OEM\installed.txt gets the install's end time and how the task was registered.

.NOTES
    Register-ScheduledTask can be missing from a trimmed Windows (Tiny11 Core); register-bootstrap.ps1 then registers
    the same definition with schtasks.exe (measured 2026-10-25: both work on Tiny11 Core 25H2). Throws only when both
    registrations fail, which setup.log then shows.
#>
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path 'C:\amb' | Out-Null
Copy-Item -Force 'C:\OEM\bootstrap.ps1' 'C:\amb\bootstrap.ps1'
(Get-Date).ToUniversalTime().ToString('o') | Set-Content -Encoding ascii 'C:\OEM\installed.txt'

# UTC, so guest and coordinator timestamps agree; High performance (SCHEME_MIN), so vCPUs are not throttled.
& tzutil.exe /s 'UTC'
& powercfg.exe /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c

# AmbBootstrap: at logon, and again every minute while it does not run (register-bootstrap.ps1 says why); its XML
# definition is printed to setup.log.
& (Join-Path $PSScriptRoot 'register-bootstrap.ps1') -Note 'C:\OEM\installed.txt'
# Started now as well: the logon that triggers it has happened already, before this script ran.
schtasks.exe /Run /TN AmbBootstrap | Out-Null
